#include "zarchive/zarchivewriter.h"
#include "zarchive/zarchivecommon.h"

#include <string>
#include <string_view>
#include <queue>

#include <zstd.h>

#include "sha_256.h"

#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace
{
	// output is collected and handed to the write callback in chunks of at least this size (fewer, larger, more uniform writes)
	constexpr size_t kOutputFlushSize = 4 * 1024 * 1024;
	// maximum number of full output buffers waiting for the I/O thread (bounds memory and provides back-pressure)
	constexpr size_t kMaxQueuedIoBuffers = 8;
	constexpr uint32_t kMaxThreads = 256;
}

ZArchiveWriter::ZArchiveWriter(CB_NewOutputFile cbNewOutputFile, CB_WriteOutputData cbWriteOutputData, void* ctx) : ZArchiveWriter(cbNewOutputFile, cbWriteOutputData, ctx, Options{})
{
}

ZArchiveWriter::ZArchiveWriter(CB_NewOutputFile cbNewOutputFile, CB_WriteOutputData cbWriteOutputData, void* ctx, const Options& options) : m_cbNewOutputFile(cbNewOutputFile), m_cbWriteOutputData(cbWriteOutputData), m_cbCtx(ctx), m_options(options)
{
	m_options.numThreads = std::min(m_options.numThreads, kMaxThreads);
	m_options.compressionLevel = std::clamp(m_options.compressionLevel, ZSTD_minCLevel(), ZSTD_maxCLevel());
	cbNewOutputFile(-1, ctx);
	m_mainShaCtx = (struct Sha_256*)malloc(sizeof(struct Sha_256));
	sha_256_init(m_mainShaCtx, m_integritySha);
	m_outPending.reserve(kOutputFlushSize + _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	if (m_options.numThreads > 0)
		StartPipeline();
	else
		m_legacyCctx = ZSTD_createCCtx();
};

ZArchiveWriter::~ZArchiveWriter()
{
	if (m_pipelineRunning || m_ioThreadRunning)
		AbortPipeline(); // Finalize() was not called, discard whatever is still in flight
	if (m_legacyCctx)
		ZSTD_freeCCtx(m_legacyCctx);
	free(m_mainShaCtx);
}

ZArchiveWriter::PathNode* ZArchiveWriter::GetNodeByPath(ZArchiveWriter::PathNode* root, std::string_view path)
{
	PathNode* currentNode = &m_rootNode;

	std::string_view pathParser = path;
	while (true)
	{
		std::string_view nodeName;
		if (!_ZARCHIVE::GetNextPathNode(pathParser, nodeName))
			break;
		PathNode* nextSubnode = FindSubnodeByName(currentNode, nodeName);
		if (!nextSubnode || (nextSubnode && nextSubnode->isFile))
			return nullptr;
		currentNode = nextSubnode;
	}
	return currentNode;
}

ZArchiveWriter::PathNode* ZArchiveWriter::FindSubnodeByName(ZArchiveWriter::PathNode* parent, std::string_view nodeName)
{
	for (auto& it : parent->subnodes)
	{
		std::string_view itName = m_nodeNames[it->nameIndex];
		if (_ZARCHIVE::CompareNodeNameBool(itName, nodeName))
			return it;
	}
	return nullptr;
}

bool ZArchiveWriter::StartNewFile(const char* path)
{
	m_currentFileNode = nullptr;
	std::string_view pathParser = path;
	std::string_view filename;
	_ZARCHIVE::SplitFilenameFromPath(pathParser, filename);
	PathNode* dir = GetNodeByPath(&m_rootNode, pathParser);
	if (!dir)
		return false;
	if (FindSubnodeByName(dir, filename))
		return false;
	// add new entry and make it the currently active file for append operations
	PathNode*& r = dir->subnodes.emplace_back(new PathNode(true, CreateNameEntry(filename)));
	m_currentFileNode = r;
	r->fileOffset = m_currentInputOffset;
	return true;
}

bool ZArchiveWriter::MakeDir(const char* path, bool recursive)
{
	std::string_view pathParser = path;
	while (!pathParser.empty() && (pathParser.back() == '/' || pathParser.back() == '\\'))
		pathParser.remove_suffix(1);
	if (!recursive)
	{
		std::string_view dirName;
		_ZARCHIVE::SplitFilenameFromPath(pathParser, dirName);
		PathNode* dir = GetNodeByPath(&m_rootNode, pathParser);
		if (!dir)
			return false;
		if (FindSubnodeByName(dir, dirName))
			return false;
		dir->subnodes.emplace_back(new PathNode(false, CreateNameEntry(dirName)));
	}
	else
	{
		PathNode* currentNode = &m_rootNode;
		while (true)
		{
			std::string_view nodeName;
			if (!_ZARCHIVE::GetNextPathNode(pathParser, nodeName))
				break;
			PathNode* nextSubnode = FindSubnodeByName(currentNode, nodeName);
			if (nextSubnode && nextSubnode->isFile)
				return false;
			if (!nextSubnode)
			{
				PathNode*& r = currentNode->subnodes.emplace_back(new PathNode(false, CreateNameEntry(nodeName)));
				nextSubnode = r;
			}
			currentNode = nextSubnode;
		}
	}
	return true;
}

uint32_t ZArchiveWriter::CreateNameEntry(std::string_view name)
{
	auto it = m_nodeNameLookup.find(std::string(name));
	if (it != m_nodeNameLookup.end())
		return it->second;
	uint32_t nameIndex = (uint32_t)m_nodeNames.size();
	m_nodeNames.emplace_back(name);
	m_nodeNameLookup.emplace(name, nameIndex);
	return nameIndex;
}

void ZArchiveWriter::OutputData(const void* data, size_t length)
{
	// only ever called from one thread at a time (caller thread in legacy mode, commit thread in pipelined mode, caller thread again after the pipeline was joined)
	const uint8_t* bytes = (const uint8_t*)data;
	m_outPending.insert(m_outPending.end(), bytes, bytes + length);
	m_currentCompressedWriteIndex += length;
	if (m_outPending.size() >= kOutputFlushSize)
		FlushOutput();
}

void ZArchiveWriter::FlushOutput()
{
	if (m_outPending.empty())
		return;
	// the hash always covers the output stream in order, and is computed in large pieces here
	if (m_mainShaCtx)
		sha_256_write(m_mainShaCtx, m_outPending.data(), m_outPending.size());
	if (!m_ioThreadRunning)
	{
		m_cbWriteOutputData(m_outPending.data(), m_outPending.size(), m_cbCtx);
		m_outPending.clear();
		return;
	}
	// hand the filled buffer to the I/O thread and continue with a recycled one
	std::vector<uint8_t> next;
	{
		std::unique_lock<std::mutex> lock(m_ioMutex);
		m_cvIoSpace.wait(lock, [&]() { return m_ioQueue.size() < kMaxQueuedIoBuffers || m_ioAbort; });
		if (m_ioAbort)
		{
			m_outPending.clear();
			return;
		}
		m_ioQueue.emplace_back(std::move(m_outPending));
		if (!m_ioFreeBuffers.empty())
		{
			next = std::move(m_ioFreeBuffers.back());
			m_ioFreeBuffers.pop_back();
		}
	}
	m_cvIoWork.notify_one();
	next.clear();
	next.reserve(kOutputFlushSize + _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	m_outPending = std::move(next);
}

uint64_t ZArchiveWriter::GetCurrentOutputOffset() const
{
	return m_currentCompressedWriteIndex;
}

void ZArchiveWriter::CommitBlock(const uint8_t* storedData, size_t storedSize)
{
	uint64_t compressedWriteOffset = GetCurrentOutputOffset();
	OutputData(storedData, storedSize);
	// add offset translation record
	if ((m_numWrittenOffsetRecords % _ZARCHIVE::ENTRIES_PER_OFFSETRECORD) == 0)
		m_compressionOffsetRecord.emplace_back().baseOffset = compressedWriteOffset;
	m_compressionOffsetRecord.back().size[m_numWrittenOffsetRecords % _ZARCHIVE::ENTRIES_PER_OFFSETRECORD] = (uint16_t)storedSize - 1;
	m_numWrittenOffsetRecords++;
}

void ZArchiveWriter::StoreBlock(const uint8_t* uncompressedData)
{
	if (m_pipelineRunning)
	{
		SubmitBlock(uncompressedData);
		return;
	}
	// legacy synchronous path (compression context is reused between blocks instead of being re-created for every block)
	m_compressionBuffer.resize(ZSTD_compressBound(_ZARCHIVE::COMPRESSED_BLOCK_SIZE));
	size_t outputSize = ZSTD_compressCCtx(m_legacyCctx, m_compressionBuffer.data(), m_compressionBuffer.size(), uncompressedData, _ZARCHIVE::COMPRESSED_BLOCK_SIZE, m_options.compressionLevel);
	if (ZSTD_isError(outputSize) || outputSize >= _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
		CommitBlock(uncompressedData, _ZARCHIVE::COMPRESSED_BLOCK_SIZE); // store block uncompressed if it is equal or larger than the input after compression
	else
		CommitBlock(m_compressionBuffer.data(), outputSize);
}

// ---------------------------------------------------------------------------------------------
// pipelined compression: caller thread -> N compression workers -> commit thread (in-order, hashing) -> I/O thread (write callback)
// ---------------------------------------------------------------------------------------------

void ZArchiveWriter::StartPipeline()
{
	const size_t numSlots = std::max<size_t>(128, (size_t)m_options.numThreads * 32);
	m_slots.resize(numSlots);
	const size_t outputBound = ZSTD_compressBound(_ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	for (auto& slot : m_slots)
	{
		slot.input.resize(_ZARCHIVE::COMPRESSED_BLOCK_SIZE);
		slot.output.resize(outputBound);
	}
	m_pipelineRunning = true;
	m_ioThreadRunning = true;
	m_ioThread = std::thread(&ZArchiveWriter::IoThreadMain, this);
	m_commitThread = std::thread(&ZArchiveWriter::CommitThreadMain, this);
	m_workers.reserve(m_options.numThreads);
	for (uint32_t i = 0; i < m_options.numThreads; i++)
		m_workers.emplace_back(&ZArchiveWriter::WorkerThreadMain, this);
}

void ZArchiveWriter::SubmitBlock(const uint8_t* uncompressedData)
{
	const uint64_t seq = m_nextSubmitSeq++;
	BlockSlot& slot = m_slots[seq % m_slots.size()];
	{
		std::unique_lock<std::mutex> lock(m_pipeMutex);
		m_cvSlotFree.wait(lock, [&]() { return slot.state == BlockSlot::State::Free || m_abort; });
		if (m_abort)
			return;
		slot.state = BlockSlot::State::Filling;
	}
	memcpy(slot.input.data(), uncompressedData, _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	{
		std::lock_guard<std::mutex> lock(m_pipeMutex);
		slot.state = BlockSlot::State::Queued;
		m_workQueue.push_back(seq);
	}
	m_cvWork.notify_one();
}

void ZArchiveWriter::WorkerThreadMain()
{
	ZSTD_CCtx* cctx = ZSTD_createCCtx();
	const int level = m_options.compressionLevel;
	while (true)
	{
		uint64_t seq;
		{
			std::unique_lock<std::mutex> lock(m_pipeMutex);
			m_cvWork.wait(lock, [&]() { return !m_workQueue.empty() || m_finishRequested || m_abort; });
			if (m_abort || m_workQueue.empty())
				break; // aborted, or finishing and nothing left to compress
			seq = m_workQueue.front();
			m_workQueue.pop_front();
		}
		BlockSlot& slot = m_slots[seq % m_slots.size()];
		size_t outputSize = ZSTD_compressCCtx(cctx, slot.output.data(), slot.output.size(), slot.input.data(), _ZARCHIVE::COMPRESSED_BLOCK_SIZE, level);
		if (ZSTD_isError(outputSize) || outputSize >= _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
			outputSize = _ZARCHIVE::COMPRESSED_BLOCK_SIZE; // stored uncompressed, the commit thread will use slot.input
		{
			std::lock_guard<std::mutex> lock(m_pipeMutex);
			slot.outputSize = outputSize;
			slot.state = BlockSlot::State::Done;
		}
		m_cvDone.notify_one();
	}
	ZSTD_freeCCtx(cctx);
}

void ZArchiveWriter::CommitThreadMain()
{
	uint64_t seq = 0;
	while (true)
	{
		BlockSlot* slot;
		{
			std::unique_lock<std::mutex> lock(m_pipeMutex);
			m_cvDone.wait(lock, [&]() { return m_abort || (m_finishRequested && seq == m_totalSubmitted) || m_slots[seq % m_slots.size()].state == BlockSlot::State::Done; });
			if (m_abort || (m_finishRequested && seq == m_totalSubmitted))
				break;
			slot = &m_slots[seq % m_slots.size()];
		}
		if (slot->outputSize >= _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
			CommitBlock(slot->input.data(), _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
		else
			CommitBlock(slot->output.data(), slot->outputSize);
		{
			std::lock_guard<std::mutex> lock(m_pipeMutex);
			slot->state = BlockSlot::State::Free;
		}
		m_cvSlotFree.notify_one();
		seq++;
	}
}

void ZArchiveWriter::IoThreadMain()
{
	while (true)
	{
		std::vector<uint8_t> buffer;
		{
			std::unique_lock<std::mutex> lock(m_ioMutex);
			m_cvIoWork.wait(lock, [&]() { return !m_ioQueue.empty() || m_ioStop || m_ioAbort; });
			if (m_ioAbort || m_ioQueue.empty())
				return;
			buffer = std::move(m_ioQueue.front());
			m_ioQueue.pop_front();
		}
		m_cvIoSpace.notify_one();
		m_cbWriteOutputData(buffer.data(), buffer.size(), m_cbCtx);
		buffer.clear();
		{
			std::lock_guard<std::mutex> lock(m_ioMutex);
			m_ioFreeBuffers.emplace_back(std::move(buffer));
		}
	}
}

void ZArchiveWriter::FinishPipeline()
{
	{
		std::lock_guard<std::mutex> lock(m_pipeMutex);
		m_finishRequested = true;
		m_totalSubmitted = m_nextSubmitSeq;
	}
	m_cvWork.notify_all();
	m_cvDone.notify_all();
	for (auto& t : m_workers)
		t.join();
	m_workers.clear();
	m_commitThread.join();
	m_pipelineRunning = false;
	// from here on the calling thread takes over the role of the commit thread (writes the remaining sections in order).
	// The I/O thread keeps running until Finalize() has queued the last buffer.
}

void ZArchiveWriter::AbortPipeline()
{
	{
		std::lock_guard<std::mutex> lock(m_pipeMutex);
		m_abort = true;
	}
	{
		std::lock_guard<std::mutex> lock(m_ioMutex);
		m_ioAbort = true;
	}
	m_cvWork.notify_all();
	m_cvDone.notify_all();
	m_cvSlotFree.notify_all();
	m_cvIoWork.notify_all();
	m_cvIoSpace.notify_all();
	for (auto& t : m_workers)
		if (t.joinable())
			t.join();
	m_workers.clear();
	if (m_commitThread.joinable())
		m_commitThread.join();
	if (m_ioThread.joinable())
		m_ioThread.join();
	m_pipelineRunning = false;
	m_ioThreadRunning = false;
}

void ZArchiveWriter::AppendData(const void* data, size_t size)
{
	size_t dataSize = size;
	const uint8_t* input = (const uint8_t*)data;
	while (size > 0)
	{
		size_t bytesToCopy = _ZARCHIVE::COMPRESSED_BLOCK_SIZE - m_currentWriteBuffer.size();
		if (bytesToCopy > size)
			bytesToCopy = size;
		if (bytesToCopy == _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
		{
			// if incoming data is block-aligned we can store it directly without memcpy to temporary buffer
			StoreBlock(input);
			input += bytesToCopy;
			size -= bytesToCopy;
			continue;
		}
		m_currentWriteBuffer.insert(m_currentWriteBuffer.end(), input, input + bytesToCopy);
		input += bytesToCopy;
		size -= bytesToCopy;
		if (m_currentWriteBuffer.size() == _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
		{
			StoreBlock(m_currentWriteBuffer.data());
			m_currentWriteBuffer.clear();
		}
	}
	if (m_currentFileNode)
		m_currentFileNode->fileSize += dataSize;
	m_currentInputOffset += dataSize;
}

void ZArchiveWriter::Finalize()
{
	m_currentFileNode = nullptr; // make sure the padding added below doesn't modify the active file
	// flush write buffer by padding it to the length of a full block
	if (!m_currentWriteBuffer.empty())
	{
		std::vector<uint8_t> padBuffer;
		padBuffer.resize(_ZARCHIVE::COMPRESSED_BLOCK_SIZE - m_currentWriteBuffer.size());
		AppendData(padBuffer.data(), padBuffer.size());
	}
	// wait until all queued blocks have been compressed and written in order
	if (m_pipelineRunning)
		FinishPipeline();
	m_footer.sectionCompressedData.offset = 0;
	m_footer.sectionCompressedData.size = GetCurrentOutputOffset();
	// pad to 8 byte
	while ((GetCurrentOutputOffset() % 8) != 0)
	{
		uint8_t b = 0;
		OutputData(&b, sizeof(uint8_t));
	}
	WriteOffsetRecords();
	WriteNameTable();
	WriteFileTree();
	WriteMetaData();
	WriteFooter();
	FlushOutput();
	if (m_ioThreadRunning)
	{
		{
			std::lock_guard<std::mutex> lock(m_ioMutex);
			m_ioStop = true;
		}
		m_cvIoWork.notify_all();
		m_ioThread.join();
		m_ioThreadRunning = false;
	}
}

void ZArchiveWriter::WriteOffsetRecords()
{
	m_footer.sectionOffsetRecords.offset = GetCurrentOutputOffset();
	_ZARCHIVE::CompressionOffsetRecord::Serialize(m_compressionOffsetRecord.data(), m_compressionOffsetRecord.size(), m_compressionOffsetRecord.data()); // in-place
	OutputData(m_compressionOffsetRecord.data(), m_compressionOffsetRecord.size() * sizeof(_ZARCHIVE::CompressionOffsetRecord));
	m_footer.sectionOffsetRecords.size = GetCurrentOutputOffset() - m_footer.sectionOffsetRecords.offset;
}

void ZArchiveWriter::WriteNameTable()
{
	m_footer.sectionNames.offset = GetCurrentOutputOffset();
	uint32_t currentNameTableOffset = 0;
	m_nodeNameOffsets.resize(m_nodeNames.size());
	for (size_t i = 0; i < m_nodeNames.size(); i++)
	{
		m_nodeNameOffsets[i] = currentNameTableOffset;
		// Each node name is stored with a length prefix byte. The prefix byte's MSB is used to indicate if an extended 2-byte header is used. The lower 7 bits are used to store the lower bits of the name length
		// If MSB is set, add an extra byte which extends the 7 bit name length field to 15 bit
		std::string_view name = m_nodeNames[i];
		if (name.size() > 0x7FFF)
			name = name.substr(0, 0x7FFF); // cut-off after 2^15-1 characters
		if (name.size() >= 0x80)
		{
			uint8_t header[2];
			header[0] = (uint8_t)(name.size() & 0x7F) | 0x80;
			header[1] = (uint8_t)(name.size() >> 7);
			OutputData(header, 2);
			currentNameTableOffset += 2;
		}
		else
		{
			uint8_t header[1];
			header[0] = (uint8_t)name.size() & 0x7F;
			OutputData(header, 1);
			currentNameTableOffset += 1;
		}
		OutputData(name.data(), name.size());
		currentNameTableOffset += (uint32_t)name.size();
	}
	m_footer.sectionNames.size = GetCurrentOutputOffset() - m_footer.sectionNames.offset;
}

void ZArchiveWriter::WriteFileTree()
{
	std::queue<PathNode*> nodeQueue;
	// first pass - assign a node range to all directories
	nodeQueue.push(&m_rootNode);
	uint32_t currentIndex = 1; // root node is at index 0
	while (!nodeQueue.empty())
	{
		PathNode* node = nodeQueue.front();
		nodeQueue.pop();
		if (node->isFile)
		{
			node->nodeStartIndex = (uint32_t)0xFFFFFFFF;
			continue;
		}
		// order entries lexicographically so we can use binary search in the reader
		std::sort(node->subnodes.begin(), node->subnodes.end(),
			[&](ZArchiveWriter::PathNode*& a, ZArchiveWriter::PathNode*& b) -> int
			{
				return _ZARCHIVE::CompareNodeName(m_nodeNames[a->nameIndex], m_nodeNames[b->nameIndex]) > 0;
			});

		node->nodeStartIndex = currentIndex;
		currentIndex += (uint32_t)node->subnodes.size();
		for (auto& it : node->subnodes)
			nodeQueue.push(it);
	}
	// second pass - serialize to file
	m_footer.sectionFileTree.offset = GetCurrentOutputOffset();
	nodeQueue.push(&m_rootNode);
	while (!nodeQueue.empty())
	{
		PathNode* node = nodeQueue.front();
		nodeQueue.pop();

		_ZARCHIVE::FileDirectoryEntry tmp;
		if(node == &m_rootNode)
			tmp.SetTypeAndNameOffset(node->isFile, 0x7FFFFFFF);
		else
			tmp.SetTypeAndNameOffset(node->isFile, m_nodeNameOffsets[node->nameIndex]);
		if (node->isFile)
		{
			tmp.SetFileOffset(node->fileOffset);
			tmp.SetFileSize(node->fileSize);
		}
		else
		{
			tmp.directoryRecord.count = (uint32_t)node->subnodes.size();
			tmp.directoryRecord.nodeStartIndex = node->nodeStartIndex;
			tmp.directoryRecord._reserved = 0;
		}
		_ZARCHIVE::FileDirectoryEntry::Serialize(&tmp, 1, &tmp);
		OutputData(&tmp, sizeof(_ZARCHIVE::FileDirectoryEntry));
		for (auto& it : node->subnodes)
			nodeQueue.push(it);
	}
	m_footer.sectionFileTree.size = GetCurrentOutputOffset() - m_footer.sectionFileTree.offset;
}

void ZArchiveWriter::WriteMetaData()
{
	// todo
	m_footer.sectionMetaDirectory.offset = GetCurrentOutputOffset();
	m_footer.sectionMetaDirectory.size = 0;
	m_footer.sectionMetaData.offset = GetCurrentOutputOffset();
	m_footer.sectionMetaData.size = 0;
}

void ZArchiveWriter::WriteFooter()
{
	m_footer.magic = _ZARCHIVE::Footer::kMagic;
	m_footer.version = _ZARCHIVE::Footer::kVersion1;
	m_footer.totalSize = GetCurrentOutputOffset() + sizeof(_ZARCHIVE::Footer);

	_ZARCHIVE::Footer tmp;

	FlushOutput(); // make sure everything written so far has been hashed
	// serialize and hash the footer with all hash bytes set to zero
	memset(m_footer.integrityHash, 0, 32);
	_ZARCHIVE::Footer::Serialize(&m_footer, &tmp);
	sha_256_write(m_mainShaCtx, &tmp, sizeof(_ZARCHIVE::Footer));
	sha_256_close(m_mainShaCtx);
	free(m_mainShaCtx);
	m_mainShaCtx = nullptr;

	// set hash and write footer
	memcpy(m_footer.integrityHash, m_integritySha, 32);
	_ZARCHIVE::Footer::Serialize(&m_footer, &tmp);
	OutputData(&tmp, sizeof(_ZARCHIVE::Footer));
}
