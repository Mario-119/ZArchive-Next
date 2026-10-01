# ZArchive-Next

> **This is a fork of [Exzap/ZArchive](https://github.com/Exzap/ZArchive).** The archive format and the reader are unchanged. This fork intends to speed up compression/decompression of .zar files while also being more efficiant. Please report issues with the changes to this repo, not the upstream one.

- AI (Claude) was used to help with some of the optimizations. If you find this to be not to your liking, I suggest that you use the original as that remains stable and works fine (credit to Exzap!)

## What's different in this fork

- **Parallel compression.** Blocks are compressed on multiple threads (one per logical CPU core by default) and written back in order.
- **Hardware-accelerated SHA-256** on x86 CPUs with the SHA extensions, with automatic fallback to the portable implementation.
- **Read-ahead and write-behind I/O.** Input files are read on a separate thread in large sequential chunks, and output is written in large sequential chunks, so the CPU and the disk can work at the same time.
- **Reused zstd contexts** instead of creating a new one for every 64 KiB block.
- **Better error handling** when packing: read errors and a full disk now abort the operation instead of silently producing a bad archive.
- **New command line options** `-t` (threads) and `-l` (compression level). Default -t level uses all available logical cores.  


## Overview
ZArchive is yet another file archive format. Think of zip, tar, 7z, etc. but with the requirement of allowing random-access reads and supporting compression.

## Features / Specifications
- Supports random-access reads within stored files
- Uses zstd compression (64KiB blocks)
- Scales reasonably well up to multiple terabytes with millions of files
- The theoretical size limit per-file is 2^48-1 (256 Terabyte)
- The encoding for paths within the archive is Windows-1252 (case-insensitive)
- Contains a SHA256 hash of the whole archive for integrity checks
- Endian-independent. The format always uses big-endian internally
- Stateless file and directory iterator handles which don't require memory allocation

## Command line tool

```
zarchive input_path [output_path] [-t threads] [-l level]
```

- If `input_path` is a directory, it is packed into a ZArchive file at `output_path`.
- If `input_path` is a ZArchive file, it is extracted into the directory `output_path`.
- `output_path` is optional.

Options (only used when packing):

| Option | Description |
| --- | --- |
| `-t`, `--threads N` | Number of compression threads. Defaults to the number of logical CPU cores. |
| `-l`, `--level N` | zstd compression level. Defaults to 6. Lower is faster, higher gives smaller archives. Levels above roughly 12 are very slow. |

The compression level does not change the format, so archives created with any level can be read by any version.

Example:

```
zarchive path/to/game_dump game.zar -t 12 -l 6
```

Extraction is unchanged from upstream and is not multi-threaded.

## Example - Basic read file

```c++
#include "zarchive/zarchivereader.h"

int main()
{
	ZArchiveReader* reader = ZArchiveReader::OpenFromFile("archive.zar");
	if (!reader)
	 	return -1;
	ZArchiveNodeHandle fileHandle = reader->LookUp("myfolder/example.bin");
	if (reader->IsFile(fileHandle))
	{
		uint8_t buffer[1000];
		uint64_t n = reader->ReadFromFile(fileHandle, 0, 1000, buffer);
		// buffer now contains the first n (up to 1000) bytes of example.bin
	}
	delete reader;
	return 0;
}
```

For a more detailed example see [main.cpp](/src/main.cpp)

## Example - Multi-threaded writer

`ZArchiveWriter` takes an optional `ZArchiveWriter::Options`:

```c++
ZArchiveWriter::Options options;
options.numThreads = 8;        // 0 (default) = synchronous, single-threaded, as in upstream
options.compressionLevel = 6;  // zstd level, default 6

ZArchiveWriter writer(cbNewOutputFile, cbWriteOutputData, ctx, options);
```

With the default options the writer behaves exactly as before, and the callbacks run on the calling thread. With `numThreads > 0` the output callbacks (`cbNewOutputFile` is still called from the constructor) are invoked from a background thread. They are never called concurrently and always receive the data in order, but they must be safe to call from a thread other than the one that created the writer. Data is delivered in large chunks (several MiB), not per block.

## Limitations
- Not designed for adding, removing or modifying files after the archive has been created

## No-seek creation
When creating new archives only byte append operations are used. No file seeking is necessary. This makes it possible to create archives on storage which is write-once. It also simplifies streaming ZArchive creation over network.

## UTF8 paths
UTF8 for file and folder paths is theoretically supported as paths are just binary blobs. But the case-insensitive comparison only applies to latin letters (a-z).

## Wii U specifics
Originally this format was created to store Wii U games dumps. These use the file extension .wua (Wii U Archive) but are otherwise regular ZArchive files. To allow multiple Wii U titles to be stored inside a single archive, each title must be placed in a subfolder following the naming scheme: 16-digit titleId followed by \_v and then the version as decimal. For example: 0005000e10102000_v32

## PS5 dumps
ZArchive-Nezt can also be used to store PS5 game dumps for the KytyPS5 emulator. This allows you to compress large games into dumps into a single, reversible file without affecting performance.

## Credits
ZArchive was created by [Exzap](https://github.com/Exzap). This fork adds the multi-threaded packing pipeline, hardware SHA-256 support, and a bit more.

## License
The ZArchive library is licensed under [MIT No Attribution](https://github.com/Exzap/ZArchive/blob/master/LICENSE), with the exception of [sha_256.c](/src/sha_256.c) and [sha_256.h](/src/sha_256.h) which are public domain, see: [ https://github.com/amosnier/sha-2]( https://github.com/amosnier/sha-2).
