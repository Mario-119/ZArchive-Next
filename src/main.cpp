#include "zarchive/zarchivewriter.h"
#include "zarchive/zarchivereader.h"

#include <vector>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <optional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>
#include <string>

#include <stdio.h>
#include <stdlib.h>
#if !defined(_WIN32)
#include <fcntl.h>
#endif

namespace fs = std::filesystem;

void PrintHelp()
{
	puts("Usage:\n");
	puts("zarchive.exe input_path [output_path] [-t threads] [-l level]");
	puts("If input_path is a directory, then output_path will be the ZArchive output file path");
	puts("If input_path is a ZArchive file path, then output_path will be the output directory");
	puts("output_path is optional");
	puts("");
	puts("Options (only used when creating an archive):");
	puts("  -t, --threads N   number of compression threads (default: number of logical CPU cores)");
	puts("  -l, --level N     zstd compression level, default 6. Lower is faster, higher is smaller.");
	puts("                    The archive format is the same for every level.");
}

bool ExtractFile(ZArchiveReader* reader, std::string_view srcPath, const fs::path& path)
{
	ZArchiveNodeHandle fileHandle = reader->LookUp(srcPath, true, false);
	if (fileHandle == ZARCHIVE_INVALID_NODE)
	{
		puts("Unable to extract file:");
		puts(std::string(srcPath).c_str());
		return false;
	}

	std::vector<uint8_t> buffer;
	buffer.resize(64 * 1024);

	std::ofstream fileOut(path, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
	if (!fileOut.is_open())
	{
		puts("Unable to write file:");
		puts(path.generic_string().c_str());
	}
	uint64_t readOffset = 0;
	while (true)
	{
		uint64_t bytesRead = reader->ReadFromFile(fileHandle, readOffset, buffer.size(), buffer.data());
		if (bytesRead == 0)
			break;
		fileOut.write((const char*)buffer.data(), bytesRead);
		readOffset += bytesRead;
	}
	if (readOffset != reader->GetFileSize(fileHandle))
		return false;

	return true;
}

bool ExtractRecursive(ZArchiveReader* reader, std::string srcPath, fs::path outputDirectory)
{
	ZArchiveNodeHandle dirHandle = reader->LookUp(srcPath, false, true);
	if (dirHandle == ZARCHIVE_INVALID_NODE)
		return false;
	std::error_code ec;
	fs::create_directories(outputDirectory);
	uint32_t numEntries = reader->GetDirEntryCount(dirHandle);
	for (uint32_t i = 0; i < numEntries; i++)
	{
		ZArchiveReader::DirEntry dirEntry;
		if (!reader->GetDirEntry(dirHandle, i, dirEntry))
		{
			puts("Directory contains invalid node");
			return false;
		}
		puts(std::string(srcPath).append("/").append(dirEntry.name).c_str());
		if (dirEntry.isDirectory)
		{
			ExtractRecursive(reader, std::string(srcPath).append("/").append(dirEntry.name), outputDirectory / dirEntry.name);
		}
		else
		{
			// extract file
			if (!ExtractFile(reader, std::string(srcPath).append("/").append(dirEntry.name), outputDirectory / dirEntry.name))
				return false;
		}
	}
	return true;
}

int Extract(fs::path inputFile, fs::path outputDirectory)
{
	std::error_code ec;
	if (!fs::exists(inputFile, ec))
	{
		puts("Unable to find archive file");
		return -10;
	}

	ZArchiveReader* reader = ZArchiveReader::OpenFromFile(inputFile);
	if (!reader)
	{
		puts("Failed to open ZArchive");
		return -11;
	}
	bool r = ExtractRecursive(reader, "", outputDirectory);
	if (!r)
	{
		puts("Extraction failed");
		delete reader;
		return -12;
	}
	delete reader;
	return 0;
}

struct PackOptions
{
	uint32_t threads{ 0 }; // 0 = auto
	int level{ 6 };
};

struct PackContext
{
	fs::path outputFilePath;
	std::ofstream currentOutputFile;
	std::atomic<bool> hasError{false};
	std::atomic<uint64_t> bytesWritten{0};
};

void _pack_NewOutputFile(const int32_t partIndex, void* ctx)
{
	PackContext* packContext = (PackContext*)ctx;
	packContext->currentOutputFile = std::ofstream(packContext->outputFilePath, std::ios::binary);
	if (!packContext->currentOutputFile.is_open())
	{
		printf("Failed to create output file: %s\n", packContext->outputFilePath.string().c_str());
		packContext->hasError = true;
	}
}

// note: in multi-threaded mode this callback is invoked from the writer's I/O thread (never concurrently, always in order)
void _pack_WriteOutputData(const void* data, size_t length, void* ctx)
{
	PackContext* packContext = (PackContext*)ctx;
	if (packContext->hasError)
		return;
	packContext->currentOutputFile.write((const char*)data, length);
	if (!packContext->currentOutputFile.good())
	{
		puts("Failed to write to output file (disk full?)");
		packContext->hasError = true;
		return;
	}
	packContext->bytesWritten += length;
}

// Reads the input files in order on a background thread using large sequential reads, so that the disk stays busy while the CPU is compressing
class FilePrefetcher
{
public:
	static constexpr size_t kChunkSize = 4 * 1024 * 1024;
	static constexpr size_t kMaxQueuedChunks = 6;

	enum class Kind { Data, EndOfFile, Error };
	struct Chunk
	{
		Kind kind{ Kind::Data };
		std::vector<uint8_t> data;
	};

	explicit FilePrefetcher(std::vector<fs::path> files) : m_files(std::move(files))
	{
		m_thread = std::thread(&FilePrefetcher::ThreadMain, this);
	}

	~FilePrefetcher()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
		}
		m_cvNotFull.notify_all();
		m_cvNotEmpty.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}

	// blocks until the next chunk is available
	Chunk Next()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_cvNotEmpty.wait(lock, [&]() { return !m_queue.empty(); });
		Chunk c = std::move(m_queue.front());
		m_queue.pop_front();
		lock.unlock();
		m_cvNotFull.notify_one();
		return c;
	}

	void Recycle(std::vector<uint8_t>&& buffer)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_freeBuffers.emplace_back(std::move(buffer));
	}

private:
	static FILE* OpenSequential(const fs::path& path)
	{
#if defined(_WIN32)
		// "S" = optimize the OS file cache behavior for sequential access
		FILE* f = _wfopen(path.c_str(), L"rbS");
#else
		FILE* f = fopen(path.c_str(), "rb");
#if defined(POSIX_FADV_SEQUENTIAL)
		if (f)
			posix_fadvise(fileno(f), 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#endif
		if (f)
			setvbuf(f, nullptr, _IONBF, 0); // we already read in large chunks, no need for a second layer of buffering
		return f;
	}

	bool Push(Chunk&& chunk)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_cvNotFull.wait(lock, [&]() { return m_queue.size() < kMaxQueuedChunks || m_stop; });
		if (m_stop)
			return false;
		m_queue.emplace_back(std::move(chunk));
		lock.unlock();
		m_cvNotEmpty.notify_one();
		return true;
	}

	void ThreadMain()
	{
		for (const fs::path& path : m_files)
		{
			FILE* f = OpenSequential(path);
			if (!f)
			{
				Chunk c;
				c.kind = Kind::Error;
				Push(std::move(c));
				return;
			}
			while (true)
			{
				Chunk c;
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					if (m_stop)
					{
						fclose(f);
						return;
					}
					if (!m_freeBuffers.empty())
					{
						c.data = std::move(m_freeBuffers.back());
						m_freeBuffers.pop_back();
					}
				}
				c.data.resize(kChunkSize);
				size_t n = fread(c.data.data(), 1, kChunkSize, f);
				if (n == 0)
				{
					bool failed = ferror(f) != 0;
					fclose(f);
					c.data.clear();
					c.kind = failed ? Kind::Error : Kind::EndOfFile;
					if (!Push(std::move(c)) || failed)
						return;
					break;
				}
				c.data.resize(n);
				c.kind = Kind::Data;
				if (!Push(std::move(c)))
				{
					fclose(f);
					return;
				}
			}
		}
	}

	std::vector<fs::path> m_files;
	std::thread m_thread;
	std::mutex m_mutex;
	std::condition_variable m_cvNotEmpty;
	std::condition_variable m_cvNotFull;
	std::deque<Chunk> m_queue;
	std::vector<std::vector<uint8_t>> m_freeBuffers;
	bool m_stop{ false };
};

int Pack(fs::path inputDirectory, fs::path outputFile, const PackOptions& packOptions)
{
	std::error_code ec;
	PackContext packContext;
	packContext.outputFilePath = outputFile;

	uint32_t numThreads = packOptions.threads;
	if (numThreads == 0)
	{
		numThreads = std::thread::hardware_concurrency();
		if (numThreads == 0)
			numThreads = 4;
	}
	ZArchiveWriter::Options writerOptions;
	writerOptions.numThreads = numThreads;
	writerOptions.compressionLevel = packOptions.level;

	auto startTime = std::chrono::steady_clock::now();
	ZArchiveWriter zWriter(_pack_NewOutputFile, _pack_WriteOutputData, &packContext, writerOptions);
	if (packContext.hasError)
		return -16;

	// collect the entries first (same order as before), so the files can be read ahead on a separate thread
	struct Entry
	{
		fs::path relativePath;
		fs::path fullPath;
		bool isDirectory;
	};
	std::vector<Entry> entries;
	std::vector<fs::path> filesToRead;
	for (auto const& dirEntry : fs::recursive_directory_iterator(inputDirectory))
	{
		fs::path pathEntry = fs::relative(dirEntry.path(), inputDirectory, ec);
		if (dirEntry.is_directory())
		{
			entries.push_back({ pathEntry, dirEntry.path(), true });
		}
		else if (dirEntry.is_regular_file())
		{
			if (dirEntry == outputFile)
				continue;
			entries.push_back({ pathEntry, inputDirectory / pathEntry, false });
			filesToRead.push_back(inputDirectory / pathEntry);
		}
	}

	uint64_t totalInputBytes = 0;
	{
		FilePrefetcher prefetcher(std::move(filesToRead));
		for (const Entry& entry : entries)
		{
			if (entry.isDirectory)
			{
				if (!zWriter.MakeDir(entry.relativePath.generic_string().c_str(), false))
				{
					printf("Failed to create directory %s\n", entry.relativePath.string().c_str());
					return -13;
				}
				continue;
			}
			printf("Adding %s\n", entry.relativePath.string().c_str());
			if (!zWriter.StartNewFile(entry.relativePath.generic_string().c_str()))
			{
				printf("Failed to create archive file %s\n", entry.relativePath.string().c_str());
				return -14;
			}
			while (true)
			{
				FilePrefetcher::Chunk chunk = prefetcher.Next();
				if (chunk.kind == FilePrefetcher::Kind::Error)
				{
					printf("Failed to open or read input file %s\n", entry.relativePath.string().c_str());
					return -15;
				}
				if (chunk.kind == FilePrefetcher::Kind::EndOfFile)
					break;
				zWriter.AppendData(chunk.data.data(), chunk.data.size());
				totalInputBytes += chunk.data.size();
				prefetcher.Recycle(std::move(chunk.data));
			}
			if (packContext.hasError)
				return -16;
		}
	}
	zWriter.Finalize();
	packContext.currentOutputFile.flush();
	if (packContext.hasError || !packContext.currentOutputFile.good())
		return -16;
	packContext.currentOutputFile.close();

	double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
	double inMiB = totalInputBytes / (1024.0 * 1024.0);
	double outMiB = packContext.bytesWritten.load() / (1024.0 * 1024.0);
	printf("\nDone. %.1f MiB -> %.1f MiB (%.1f%%) in %.1f s (%.1f MiB/s input) using %u compression threads, level %d\n",
		inMiB, outMiB, inMiB > 0.0 ? outMiB * 100.0 / inMiB : 100.0, seconds, seconds > 0.0 ? inMiB / seconds : 0.0, numThreads, packOptions.level);
	return 0;
}

int main(int argc, char* argv[])
{
	if (argc <= 1)
	{
		PrintHelp();
		return 0;
	}
	std::optional<std::string> strInput;
	std::optional<std::string> strOutput;
	PackOptions packOptions;
	for (int i = 1; i < argc; i++)
	{
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help" || arg == "/?")
		{
			PrintHelp();
			return 0;
		}
		if (arg == "-t" || arg == "--threads" || arg == "-l" || arg == "--level")
		{
			if (i + 1 >= argc)
			{
				printf("Missing value for %s\n", arg.c_str());
				return -1;
			}
			char* endPtr = nullptr;
			long value = strtol(argv[++i], &endPtr, 10);
			if (endPtr == argv[i] || *endPtr != '\0')
			{
				printf("Invalid value for %s: %s\n", arg.c_str(), argv[i]);
				return -1;
			}
			if (arg == "-t" || arg == "--threads")
			{
				if (value < 1 || value > 256)
				{
					puts("Thread count must be between 1 and 256");
					return -1;
				}
				packOptions.threads = (uint32_t)value;
			}
			else
				packOptions.level = (int)value;
			continue;
		}
		if (strInput)
		{
			if (strOutput)
			{
				puts("Too many paths specified");
				return -1;
			}
			else
			{
				strOutput = argv[i];
			}
		}
		else
		{
			strInput = argv[i];
		}
	}

	if (strInput)
	{
		std::error_code ec;
		fs::path p(*strInput);
		if (fs::is_regular_file(p, ec))
		{
			// extract
			fs::path outputDirectory;
			if (!strOutput)
			{
				fs::path defaultOutputPath = p.parent_path() / (p.stem().filename().string().append("_extracted"));
				outputDirectory = defaultOutputPath;
				printf("Extracting to: %s\n", outputDirectory.generic_string().c_str());
			}
			else
				outputDirectory = *strOutput;
			if (fs::exists(outputDirectory, ec) && !fs::is_directory(outputDirectory, ec))
			{
				puts("The specified output path is not a valid directory");
				return -3;
			}
			fs::create_directories(outputDirectory, ec);
			if (!fs::exists(outputDirectory, ec))
			{
				puts("Failed to create output directory");
				return -4;
			}
			return Extract(p, outputDirectory);
		}
		else if(fs::is_directory(p, ec))
		{
			// pack
			fs::path outputFile;
			if (!strOutput)
			{
				fs::path defaultOutputPath = p.parent_path() / (p.stem().filename().string().append(".zar"));
				outputFile = defaultOutputPath;
				printf("Outputting to: %s\n", outputFile.generic_string().c_str());
			}
			else
				outputFile = *strOutput;
			if ((fs::exists(outputFile, ec) && !fs::is_regular_file(outputFile, ec)))
			{
				puts("The specified output path is not a valid file");
				return -10;
			}
			if ((fs::exists(outputFile, ec) && fs::is_regular_file(outputFile, ec)))
			{
				puts("The output file already exists");
				return -11;
			}
			int r = Pack(p, outputFile, packOptions);
			if (r != 0)
			{
				// delete incomplete output file
				fs::remove(outputFile, ec);
			}
			return r;
		}
		else
		{
			puts("Input path is not a valid file or directory");
			return -1;
		}
	}
	return 0;
}
