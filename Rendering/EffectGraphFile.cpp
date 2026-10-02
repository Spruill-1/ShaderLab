#include "pch_engine.h"
#include "EffectGraphFile.h"
#include "../Graph/EffectGraph.h"

// miniz: vendored at build time by EnsureMiniz.ps1 -> third_party/miniz.
// We use only its DEFLATE codec (tdefl_compress_mem_to_heap /
// tinfl_decompress_mem_to_heap); the surrounding ZIP container is still
// our own minimal writer/reader, which already tracks per-entry
// compression method, CRC32, and sizes.
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "miniz.h"

// Minimal ZIP "store" (method=0) reader/writer.
//
// We only ever read or write archives produced by this code, so the
// implementation focuses on the small subset of the ZIP spec that
// matters for our container:
//
//   * No compression (method 0).
//   * No encryption.
//   * No multi-disk volumes.
//   * No ZIP64 extensions; entries are well under 4 GB.
//   * UTF-8 file names (general purpose bit 11 set).
//
// References:
//   APPNOTE.TXT 6.3.x -- https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT
//   Wikipedia "ZIP (file format)" has a clear field-by-field layout.
//
// All multi-byte fields are little-endian on disk; that matches our
// in-memory layout on x86/x64/ARM64 Windows so we just reinterpret_cast
// pod structs. Each struct is #pragma pack(1) so the compiler doesn't
// pad them.
//
// Compression: every entry is offered to miniz DEFLATE. Per-entry
// fallback: if the deflated bytes aren't smaller than the raw bytes,
// Compress() keeps the entry at method 0 (stored) so the
// archive never grows. This means already-compressed inputs (MP4 /
// PNG / JPEG / JXR / ...) end up stored (large ones on the evidence
// of a sampled deflate, see WorthDeflating), while compressible inputs
// (BMP, uncompressed TIFF, raw HDR/EXR, ICC, JSON) get real savings
// without us maintaining a per-extension allowlist.
//
//   * Compression uses miniz's tdefl_compress_mem_to_heap with the
//     default probe count (~zlib level 6). The output is raw DEFLATE
//     (no zlib header), which is exactly what ZIP method 8 expects.
//   * Decompression uses miniz's tinfl_decompress_mem_to_heap. The
//     reader accepts both method 0 (forward-compatible with old
//     archives) and method 8.
//   * miniz is vendored at build time (third_party/miniz, MIT
//     licensed) by EnsureMiniz.ps1 -- the source is not checked in.

namespace
{
	// CRC-32/IEEE 802.3 polynomial 0xEDB88320 (reflected), as the ZIP headers require.
	// Slicing-by-8, because every save hashes each embedded media file.
	const uint32_t (&GetCrcTables())[8][256]
	{
		static uint32_t sTables[8][256];
		static std::once_flag sTablesBuilt;
		std::call_once(sTablesBuilt, []
		{
			for (uint32_t i = 0; i < 256; ++i)
			{
				uint32_t c = i;
				for (int k = 0; k < 8; ++k)
					c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				sTables[0][i] = c;
			}
			for (int t = 1; t < 8; ++t)
				for (uint32_t i = 0; i < 256; ++i)
					sTables[t][i] = (sTables[t - 1][i] >> 8) ^ sTables[0][sTables[t - 1][i] & 0xFF];
		});
		return sTables;
	}

	// Running CRC: pass 0xFFFFFFFF first, XOR the final value with it.
	uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t size)
	{
		const auto& tables = GetCrcTables();
		while (size >= 8)
		{
			uint32_t low, high;
			std::memcpy(&low, data, 4);
			std::memcpy(&high, data + 4, 4);
			low ^= crc;
			crc = tables[7][low & 0xFF] ^ tables[6][(low >> 8) & 0xFF] ^ tables[5][(low >> 16) & 0xFF] ^ tables[4][low >> 24] ^
				  tables[3][high & 0xFF] ^ tables[2][(high >> 8) & 0xFF] ^ tables[1][(high >> 16) & 0xFF] ^ tables[0][high >> 24];
			data += 8;
			size -= 8;
		}
		while (size--)
			crc = tables[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
		return crc;
	}

	uint32_t Crc32(const uint8_t* data, size_t size)
	{
		return Crc32Update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
	}

#pragma pack(push, 1)
	struct LocalFileHeader
	{
		uint32_t signature;          // 0x04034b50 'PK\3\4'
		uint16_t versionNeeded;      // 20 (2.0 = stored)
		uint16_t generalPurposeFlag; // bit 11 set => UTF-8 names
		uint16_t compressionMethod;  // 0 = stored
		uint16_t lastModFileTime;    // MS-DOS time
		uint16_t lastModFileDate;    // MS-DOS date
		uint32_t crc32;
		uint32_t compressedSize;
		uint32_t uncompressedSize;
		uint16_t fileNameLength;
		uint16_t extraFieldLength;
		// followed by file name (no NUL), extra field, file data
	};

	struct CentralDirectoryHeader
	{
		uint32_t signature;          // 0x02014b50
		uint16_t versionMadeBy;      // 20
		uint16_t versionNeeded;      // 20
		uint16_t generalPurposeFlag;
		uint16_t compressionMethod;
		uint16_t lastModFileTime;
		uint16_t lastModFileDate;
		uint32_t crc32;
		uint32_t compressedSize;
		uint32_t uncompressedSize;
		uint16_t fileNameLength;
		uint16_t extraFieldLength;
		uint16_t fileCommentLength;
		uint16_t diskNumberStart;
		uint16_t internalFileAttrs;
		uint32_t externalFileAttrs;
		uint32_t localHeaderOffset;
		// followed by file name, extra field, comment
	};

	struct EndOfCentralDirectory
	{
		uint32_t signature;            // 0x06054b50
		uint16_t diskNumber;
		uint16_t centralDirDisk;
		uint16_t entriesOnDisk;
		uint16_t totalEntries;
		uint32_t centralDirSize;
		uint32_t centralDirOffset;
		uint16_t commentLength;
	};
#pragma pack(pop)

	constexpr uint32_t kLocalSig = 0x04034b50;
	constexpr uint32_t kCentralSig = 0x02014b50;
	constexpr uint32_t kEocdSig = 0x06054b50;
	constexpr uint16_t cDosDate1980 = (1 << 9) | (1 << 5) | 1; // 1980-01-01, fixed

	// UTF-16 -> UTF-8 conversion via WideCharToMultiByte (no third-party
	// dependency). Used for ZIP entry names (which we mark as UTF-8 via
	// general-purpose bit 11) and for the JSON payload.
	std::string Utf16ToUtf8(std::wstring_view ws)
	{
		if (ws.empty()) return {};
		const int bytes = WideCharToMultiByte(
			CP_UTF8, 0,
			ws.data(), static_cast<int>(ws.size()),
			nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<size_t>(bytes), '\0');
		WideCharToMultiByte(
			CP_UTF8, 0,
			ws.data(), static_cast<int>(ws.size()),
			out.data(), bytes, nullptr, nullptr);
		return out;
	}

	std::wstring Utf8ToUtf16(const char* data, size_t size)
	{
		if (size == 0) return {};
		const int chars = MultiByteToWideChar(
			CP_UTF8, 0,
			data, static_cast<int>(size),
			nullptr, 0);
		std::wstring out(static_cast<size_t>(chars), L'\0');
		MultiByteToWideChar(
			CP_UTF8, 0,
			data, static_cast<int>(size),
			out.data(), chars);
		return out;
	}

	bool WriteAll(HANDLE h, const void* data, size_t size)
	{
		const uint8_t* p = static_cast<const uint8_t*>(data);
		while (size > 0)
		{
			DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 0x10000000));
			DWORD written = 0;
			if (!::WriteFile(h, p, chunk, &written, nullptr) || written == 0)
				return false;
			p += written;
			size -= written;
		}
		return true;
	}

	bool Seek(HANDLE h, uint64_t offset)
	{
		LARGE_INTEGER position{};
		position.QuadPart = static_cast<LONGLONG>(offset);
		return ::SetFilePointerEx(h, position, nullptr, FILE_BEGIN) != 0;
	}

	bool ReadAt(HANDLE h, uint64_t offset, void* dst, size_t size)
	{
		if (!Seek(h, offset)) return false;
		uint8_t* p = static_cast<uint8_t*>(dst);
		while (size > 0)
		{
			DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 0x10000000));
			DWORD got = 0;
			if (!::ReadFile(h, p, chunk, &got, nullptr) || got == 0) return false;
			p += got;
			size -= got;
		}
		return true;
	}

	// One entry as the central directory describes it.
	struct EntryRecord
	{
		std::string name;             // UTF-8, may contain '/' and end with '/'
		uint16_t method{ 0 };         // 0 = store, 8 = deflate
		uint32_t crc{ 0 };            // over the UNCOMPRESSED bytes, per the spec
		uint32_t compressedSize{ 0 };
		uint32_t uncompressedSize{ 0 };
		uint32_t localHeaderOffset{ 0 };
		uint64_t dataOffset{ 0 };     // existing archives only: where the payload starts
	};

	// Index an archive from its central directory. Empty when the file is not
	// an archive this writer produced; the save then starts fresh.
	// Our writer never emits an archive comment, so the end-of-central-directory
	// record is exactly the last 22 bytes.
	std::vector<EntryRecord> ReadDirectory(HANDLE h)
	{
		LARGE_INTEGER fileSize{};
		if (!::GetFileSizeEx(h, &fileSize) || fileSize.QuadPart < (LONGLONG)sizeof(EndOfCentralDirectory))
			return {};
		EndOfCentralDirectory eocd{};
		if (!ReadAt(h, fileSize.QuadPart - sizeof(eocd), &eocd, sizeof(eocd)) || eocd.signature != kEocdSig)
			return {};
		if (uint64_t(eocd.centralDirOffset) + eocd.centralDirSize > uint64_t(fileSize.QuadPart))
			return {};
		std::vector<uint8_t> centralDir(eocd.centralDirSize);
		if (!centralDir.empty() && !ReadAt(h, eocd.centralDirOffset, centralDir.data(), centralDir.size()))
			return {};

		std::vector<EntryRecord> entries;
		size_t pos = 0;
		for (uint16_t i = 0; i < eocd.totalEntries; ++i)
		{
			CentralDirectoryHeader header{};
			if (pos + sizeof(header) > centralDir.size()) return {};
			std::memcpy(&header, centralDir.data() + pos, sizeof(header));
			if (header.signature != kCentralSig) return {};
			if (pos + sizeof(header) + header.fileNameLength > centralDir.size()) return {};
			EntryRecord entry;
			entry.name.assign(reinterpret_cast<const char*>(centralDir.data() + pos + sizeof(header)), header.fileNameLength);
			entry.method = header.compressionMethod;
			entry.crc = header.crc32;
			entry.compressedSize = header.compressedSize;
			entry.uncompressedSize = header.uncompressedSize;
			entry.localHeaderOffset = header.localHeaderOffset;
			LocalFileHeader localHeader{};
			if (!ReadAt(h, header.localHeaderOffset, &localHeader, sizeof(localHeader)) || localHeader.signature != kLocalSig)
				return {};
			entry.dataOffset = uint64_t(header.localHeaderOffset) + sizeof(localHeader)
				+ localHeader.fileNameLength + localHeader.extraFieldLength;
			if (entry.dataOffset + entry.compressedSize > uint64_t(fileSize.QuadPart)) return {};
			entries.push_back(std::move(entry));
			pos += sizeof(header) + header.fileNameLength + header.extraFieldLength + header.fileCommentLength;
		}
		return entries;
	}

	// CRC-32 of a whole file, streamed. Returns false if it cannot be read.
	bool FileCrc32(const std::wstring& path, uint64_t& size, uint32_t& crc)
	{
		winrt::file_handle file{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
		if (!file) return false;
		LARGE_INTEGER fileSize{};
		bool ok = ::GetFileSizeEx(file.get(), &fileSize) != 0;
		size = ok ? uint64_t(fileSize.QuadPart) : 0;
		std::vector<uint8_t> buffer(8u << 20);
		uint32_t running = 0xFFFFFFFFu;
		while (ok)
		{
			DWORD got = 0;
			if (!::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) { ok = false; break; }
			if (got == 0) break;
			running = Crc32Update(running, buffer.data(), got);
		}
		crc = running ^ 0xFFFFFFFFu;
		return ok;
	}

	bool WriteLocalHeader(HANDLE h, const EntryRecord& e)
	{
		LocalFileHeader hdr{};
		hdr.signature = kLocalSig;
		hdr.versionNeeded = 20;
		hdr.generalPurposeFlag = 0x0800; // UTF-8 names
		hdr.compressionMethod = e.method;
		hdr.lastModFileDate = cDosDate1980;
		hdr.crc32 = e.crc;
		hdr.compressedSize = e.compressedSize;
		hdr.uncompressedSize = e.uncompressedSize;
		hdr.fileNameLength = static_cast<uint16_t>(e.name.size());
		return WriteAll(h, &hdr, sizeof(hdr)) && WriteAll(h, e.name.data(), e.name.size());
	}

	bool WriteCentralEntry(HANDLE h, const EntryRecord& e)
	{
		CentralDirectoryHeader cd{};
		cd.signature = kCentralSig;
		cd.versionMadeBy = 20;
		cd.versionNeeded = 20;
		cd.generalPurposeFlag = 0x0800;
		cd.compressionMethod = e.method;
		cd.lastModFileDate = cDosDate1980;
		cd.crc32 = e.crc;
		cd.compressedSize = e.compressedSize;
		cd.uncompressedSize = e.uncompressedSize;
		cd.fileNameLength = static_cast<uint16_t>(e.name.size());
		// External attributes: directory entry sets the MS-DOS dir bit
		// (0x10) so unzip tools render it as a folder.
		cd.externalFileAttrs = (!e.name.empty() && e.name.back() == '/') ? 0x10u : 0u;
		cd.localHeaderOffset = e.localHeaderOffset;
		return WriteAll(h, &cd, sizeof(cd)) && WriteAll(h, e.name.data(), e.name.size());
	}

	// Whether deflating a 1 MB sample from the middle of `data` saves at least 2%.
	// Deflate runs at tens of MB/s, so a large already-compressed file (video,
	// PNG, JPEG) is stored on the sample's evidence instead of deflated whole.
	// The middle, because an MP4's compressible index often sits at the start.
	bool WorthDeflating(const std::vector<uint8_t>& data)
	{
		constexpr size_t cSampleBytes = 1u << 20;
		if (data.size() < 8 * cSampleBytes) return true;
		const uint8_t* sample = data.data() + (data.size() - cSampleBytes) / 2;
		size_t deflatedSize = 0;
		void* deflated = tdefl_compress_mem_to_heap(sample, cSampleBytes, &deflatedSize, TDEFL_DEFAULT_MAX_PROBES);
		if (!deflated) return true;
		mz_free(deflated);
		return deflatedSize < cSampleBytes - cSampleBytes / 50;
	}

	// Deflate `data` with miniz (raw DEFLATE, no zlib header, as ZIP method 8 expects).
	// Store it (method 0) instead when deflate does not make it smaller.
	void Compress(const std::vector<uint8_t>& data, EntryRecord& entry, std::vector<uint8_t>& out)
	{
		entry.crc = Crc32(data.data(), data.size());
		entry.uncompressedSize = static_cast<uint32_t>(data.size());
		entry.method = 0;
		out.clear();
		if (!data.empty() && WorthDeflating(data))
		{
			size_t outSize = 0;
			void* deflated = tdefl_compress_mem_to_heap(data.data(), data.size(), &outSize, TDEFL_DEFAULT_MAX_PROBES);
			if (deflated && outSize > 0 && outSize < data.size())
			{
				out.assign(static_cast<const uint8_t*>(deflated), static_cast<const uint8_t*>(deflated) + outSize);
				entry.method = 8;
			}
			if (deflated) mz_free(deflated);
		}
		entry.compressedSize = static_cast<uint32_t>(entry.method == 8 ? out.size() : data.size());
	}

	// Sequential archive writer with a 4 GB cap (no ZIP64).
	struct ArchiveWriter
	{
		winrt::file_handle file;
		uint64_t cursor{ 0 };
		uint64_t written{ 0 };                // bytes this save actually wrote
		std::vector<EntryRecord> directory;   // every entry, in file order

		bool Fits(uint64_t more) const { return cursor + more <= 0xFFFFFFFFull; }

		// A new entry from bytes in memory.
		bool AddBytes(const std::string& name, const std::vector<uint8_t>& data)
		{
			EntryRecord entry;
			entry.name = name;
			std::vector<uint8_t> packed;
			Compress(data, entry, packed);
			const std::vector<uint8_t>& payload = (entry.method == 8) ? packed : data;
			const uint64_t entrySize = sizeof(LocalFileHeader) + entry.name.size() + payload.size();
			if (!Fits(entrySize)) return false;
			entry.localHeaderOffset = static_cast<uint32_t>(cursor);
			if (!WriteLocalHeader(file.get(), entry) || (!payload.empty() && !WriteAll(file.get(), payload.data(), payload.size())))
				return false;
			cursor += entrySize;
			written += entrySize;
			directory.push_back(std::move(entry));
			return true;
		}

		// An entry copied byte for byte from another archive, without recompressing.
		bool AddCopied(const EntryRecord& from, HANDLE source)
		{
			EntryRecord entry = from;
			const uint64_t entrySize = sizeof(LocalFileHeader) + entry.name.size() + entry.compressedSize;
			if (!Fits(entrySize)) return false;
			entry.localHeaderOffset = static_cast<uint32_t>(cursor);
			if (!WriteLocalHeader(file.get(), entry)) return false;
			const uint64_t payloadStart = cursor + sizeof(LocalFileHeader) + entry.name.size();
			std::vector<uint8_t> buffer(8u << 20);
			for (uint64_t done = 0; done < entry.compressedSize;)
			{
				const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), entry.compressedSize - done));
				if (!ReadAt(source, from.dataOffset + done, buffer.data(), chunk)) return false;
				if (!Seek(file.get(), payloadStart + done) || !WriteAll(file.get(), buffer.data(), chunk)) return false;
				done += chunk;
			}
			cursor += entrySize;
			written += entrySize;
			directory.push_back(std::move(entry));
			return true;
		}

		// An entry already in place in this file; it is only listed in the directory.
		void AddKept(const EntryRecord& entry)
		{
			directory.push_back(entry);
			cursor = (std::max)(cursor, entry.dataOffset + entry.compressedSize);
		}

		bool Finish()
		{
			const uint64_t cdStart = cursor;
			for (const auto& entry : directory)
			{
				if (!WriteCentralEntry(file.get(), entry)) return false;
				cursor += sizeof(CentralDirectoryHeader) + entry.name.size();
			}
			EndOfCentralDirectory eocd{};
			eocd.signature = kEocdSig;
			eocd.entriesOnDisk = static_cast<uint16_t>(directory.size());
			eocd.totalEntries = static_cast<uint16_t>(directory.size());
			eocd.centralDirSize = static_cast<uint32_t>(cursor - cdStart);
			eocd.centralDirOffset = static_cast<uint32_t>(cdStart);
			if (!Fits(sizeof(eocd)) || !WriteAll(file.get(), &eocd, sizeof(eocd))) return false;
			written += (cursor - cdStart) + sizeof(eocd);
			return ::SetEndOfFile(file.get()) != 0;
		}
	};

	bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
	{
		winrt::file_handle file{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if (!file) return false;
		LARGE_INTEGER fileSize{};
		const bool ok = ::GetFileSizeEx(file.get(), &fileSize) && fileSize.QuadPart <= 0xFFFFFFFFLL; // no ZIP64
		if (ok) out.resize(static_cast<size_t>(fileSize.QuadPart));
		return ok && (out.empty() || ReadAt(file.get(), 0, out.data(), out.size()));
	}
}

namespace ShaderLab::Rendering
{
	// Layout: media entries, then the "media/" folder marker, then graph.json.
	// With the media first, an unchanged media set is a prefix of the old file,
	// so a re-save only rewrites the tail. Load accepts entries in any order.
	bool EffectGraphFile::Save(const std::wstring& path,
							   const std::wstring& graphJson,
							   const std::vector<MediaEntry>& media,
							   const ProgressCallback& progress,
							   SaveStats* stats)
	{
		SaveStats localStats;
		SaveStats& result = stats ? *stats : localStats;
		result = {};
		const std::string jsonUtf8 = Utf16ToUtf8(graphJson);
		const std::vector<uint8_t> jsonBytes(jsonUtf8.begin(), jsonUtf8.end());

		// Steps: check each media file, write or copy each one, graph.json, done.
		const uint32_t mediaCount = static_cast<uint32_t>(media.size());
		const uint32_t total = 2 * mediaCount + 2;
		uint32_t step = 0;
		auto report = [&](const std::wstring& msg) -> bool
		{
			return progress ? progress(step, total, msg) : true;
		};

		// Index the file being overwritten, if there is one.
		winrt::file_handle oldFile{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
		std::vector<EntryRecord> existing;
		if (oldFile) existing = ReadDirectory(oldFile.get());

		// A media file is unchanged when an entry has the same name, size and CRC-32.
		std::vector<const EntryRecord*> reuse(media.size(), nullptr);
		for (size_t i = 0; i < media.size(); ++i)
		{
			++step;
			const std::wstring fileName = std::filesystem::path(media[i].sourcePath).filename().wstring();
			if (!report(L"Checking " + fileName)) return false;
			const std::string entryName = Utf16ToUtf8(media[i].zipEntryName);
			auto match = std::find_if(existing.begin(), existing.end(),
				[&](const EntryRecord& entry) { return entry.name == entryName; });
			if (match == existing.end()) continue;
			std::error_code ec;
			const uint64_t onDisk = std::filesystem::file_size(media[i].sourcePath, ec);
			if (ec || onDisk != match->uncompressedSize) continue;   // size differs, so skip the hash
			uint64_t hashedSize = 0;
			uint32_t hashedCrc = 0;
			if (FileCrc32(media[i].sourcePath, hashedSize, hashedCrc) && hashedSize == match->uncompressedSize && hashedCrc == match->crc)
				reuse[i] = &*match;
		}

		// Update in place when every media file is unchanged and the old file
		// starts with exactly those entries, in order, back to back.
		bool inPlace = !media.empty() && std::all_of(reuse.begin(), reuse.end(), [](auto* entry) { return entry != nullptr; });
		if (inPlace)
		{
			std::vector<const EntryRecord*> byOffset;
			for (const auto& entry : existing) byOffset.push_back(&entry);
			std::sort(byOffset.begin(), byOffset.end(),
				[](auto* a, auto* b) { return a->localHeaderOffset < b->localHeaderOffset; });
			uint64_t expectedOffset = 0;
			for (size_t i = 0; inPlace && i < media.size(); ++i)
			{
				inPlace = i < byOffset.size() && byOffset[i] == reuse[i] && byOffset[i]->localHeaderOffset == expectedOffset;
				if (inPlace) expectedOffset = byOffset[i]->dataOffset + byOffset[i]->compressedSize;
			}
		}

		// Deletes the temp file on any exit that does not rename it into place.
		// Declared before the writer so the writer's handle closes first.
		struct TempFile
		{
			std::wstring path;
			~TempFile() { if (!path.empty()) ::DeleteFileW(path.c_str()); }
		} tempFile;

		ArchiveWriter writer;
		if (inPlace)
		{
			// Rewrite only the tail. A crash during this small write leaves the file without a directory.
			oldFile.close();
			writer.file.attach(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!writer.file) return false;
			for (const auto* entry : reuse) writer.AddKept(*entry);
			if (!Seek(writer.file.get(), writer.cursor)) return false;
			step += mediaCount;
		}
		else
		{
			// Build a new file beside the old one and swap it in once complete,
			// so an interrupted save leaves the previous file intact.
			const std::wstring tempPath = path + L".saving";
			writer.file.attach(::CreateFileW(tempPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!writer.file) return false;
			tempFile.path = tempPath;
			for (size_t i = 0; i < media.size(); ++i)
			{
				++step;
				const std::wstring fileName = std::filesystem::path(media[i].sourcePath).filename().wstring();
				if (reuse[i])
				{
					if (!report(L"Copying " + fileName + L" (unchanged)") || !writer.AddCopied(*reuse[i], oldFile.get()))
						return false;
				}
				else
				{
					std::vector<uint8_t> data;
					if (!report(L"Writing " + fileName) || !ReadWholeFile(media[i].sourcePath, data)
						|| !writer.AddBytes(Utf16ToUtf8(media[i].zipEntryName), data))
						return false;
				}
			}
		}

		// The tail. The folder marker is always written so unzip tools show the folder.
		++step;
		if (!report(L"graph.json")
			|| !writer.AddBytes("media/", {})
			|| !writer.AddBytes("graph.json", jsonBytes)
			|| !writer.Finish()
			|| !::FlushFileBuffers(writer.file.get()))
			return false;
		writer.file.close();
		oldFile.close();
		if (!inPlace)
		{
			if (!::MoveFileExW(tempFile.path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				return false;
			tempFile.path.clear();
		}

		result.inPlace = inPlace;
		result.bytesWritten = writer.written;
		for (auto* entry : reuse) (entry ? result.mediaUnchanged : result.mediaWritten)++;
		step = total;
		report(L"Saved");   // the save is already complete, so a cancel here is ignored
		return true;
	}

	std::optional<EffectGraphFile::LoadResult> EffectGraphFile::Load(
		const std::wstring& path,
		const std::wstring& extractDirRoot,
		const ProgressCallback& progress)
	{
		// Slurp the whole file. Effect graphs with embedded media can
		// hit hundreds of MB (HDR clips, etc.) but we still expect to
		// fit in RAM -- streaming would complicate the central-dir
		// walk for no real benefit on modern hardware.
		winrt::file_handle file{ ::CreateFileW(
			path.c_str(),
			GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if (!file) return std::nullopt;

		LARGE_INTEGER size{};
		if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart < (LONGLONG)sizeof(EndOfCentralDirectory))
			return std::nullopt;

		std::vector<uint8_t> buf(static_cast<size_t>(size.QuadPart));
		DWORD read = 0;
		if (!::ReadFile(file.get(), buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)
			|| read != buf.size())
			return std::nullopt;
		file.close();

		// First pass: count entries and find graph.json so we can
		// report meaningful progress totals.
		struct ParsedEntry
		{
			std::string name;
			size_t dataOff;
			uint32_t size;             // compressed size on disk
			uint32_t uncompressedSize; // logical size after inflate
			uint16_t method;           // 0 = stored, 8 = deflate
		};
		std::vector<ParsedEntry> parsed;
		parsed.reserve(8);

		size_t pos = 0;
		while (pos + sizeof(LocalFileHeader) <= buf.size())
		{
			uint32_t sig = 0;
			std::memcpy(&sig, buf.data() + pos, 4);
			if (sig != kLocalSig) break;

			LocalFileHeader hdr{};
			std::memcpy(&hdr, buf.data() + pos, sizeof(hdr));
			// Supported methods: 0 (store) and 8 (deflate). Anything
			// else is from a future revision and we refuse the archive
			// rather than silently mis-decoding.
			if (hdr.compressionMethod != 0 && hdr.compressionMethod != 8)
				return std::nullopt;

			const size_t nameOff = pos + sizeof(hdr);
			const size_t dataOff = nameOff + hdr.fileNameLength + hdr.extraFieldLength;
			const size_t dataEnd = dataOff + hdr.compressedSize;
			if (dataEnd > buf.size()) return std::nullopt;

			const char* nameP = reinterpret_cast<const char*>(buf.data() + nameOff);
			ParsedEntry pe;
			pe.name.assign(nameP, hdr.fileNameLength);
			pe.dataOff = dataOff;
			pe.size = hdr.compressedSize;
			pe.uncompressedSize = hdr.uncompressedSize;
			pe.method = hdr.compressionMethod;
			parsed.push_back(std::move(pe));

			pos = dataEnd;
		}

		// Helper: produce the uncompressed bytes of a parsed entry.
		// Stored entries (method 0) are returned as a view-by-copy of the
		// raw payload; deflated entries (method 8) are inflated through
		// miniz into a freshly-allocated buffer. Returns empty on error.
		auto inflateEntry = [&](const ParsedEntry& e) -> std::vector<uint8_t>
		{
			std::vector<uint8_t> out;
			if (e.method == 0)
			{
				out.assign(buf.data() + e.dataOff,
					buf.data() + e.dataOff + e.size);
				return out;
			}
			// Method 8: raw DEFLATE. miniz's tinfl_decompress_mem_to_heap
			// is the symmetric inverse of tdefl_compress_mem_to_heap that
			// we used on save -- no zlib header, no checksum trailer.
			size_t outSize = 0;
			void* inflated = tinfl_decompress_mem_to_heap(
				buf.data() + e.dataOff, e.size, &outSize, 0);
			if (!inflated) return out;
			if (e.uncompressedSize != 0 && outSize != e.uncompressedSize)
			{
				mz_free(inflated);
				return out;
			}
			out.assign(
				static_cast<const uint8_t*>(inflated),
				static_cast<const uint8_t*>(inflated) + outSize);
			mz_free(inflated);
			return out;
		};

		// Find graph.json.
		std::optional<std::wstring> graphJson;
		for (const auto& e : parsed)
		{
			if (e.name != "graph.json") continue;
			auto bytes = inflateEntry(e);
			if (bytes.empty() && e.uncompressedSize > 0) return std::nullopt;
			graphJson = Utf8ToUtf16(
				reinterpret_cast<const char*>(bytes.data()), bytes.size());
		}
		if (!graphJson.has_value())
			return std::nullopt;

		// Allocate a unique extraction directory under extractDirRoot.
		// GUID-based name to avoid clashes between concurrent loads.
		std::wstring extractDir;
		{
			GUID g{};
			::CoCreateGuid(&g);
			wchar_t guidStr[64]{};
			swprintf_s(guidStr, L"ShaderLab-%08X%04X%04X-%02X%02X",
				g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1]);
			extractDir = extractDirRoot;
			if (!extractDir.empty() && extractDir.back() != L'\\' && extractDir.back() != L'/')
				extractDir.push_back(L'\\');
			extractDir += guidStr;
		}

		LoadResult result;
		result.graphJson = std::move(*graphJson);
		result.extractDir = extractDir;
		result.package = true;

		// Removes a half-extracted directory on any failure below.
		struct ExtractCleanup
		{
			std::wstring dir;
			~ExtractCleanup()
			{
				std::error_code ec;
				if (!dir.empty()) std::filesystem::remove_all(dir, ec);   // best effort: temp dir
			}
		} cleanup;

		// Count media files for progress reporting.
		uint32_t mediaCount = 0;
		for (const auto& e : parsed)
			if (e.name.starts_with("media/") && e.name.size() > 6 && e.name.back() != '/')
				++mediaCount;

		const uint32_t total = 1 + mediaCount; // graph.json + media files
		uint32_t step = 0;
		auto report = [&](const std::wstring& msg) -> bool
		{
			if (progress) return progress(step, total, msg);
			return true;
		};

		++step;
		if (!report(L"graph.json")) return std::nullopt;

		if (mediaCount > 0)
		{
			// Create the extraction directory only if we actually
			// have files to extract -- avoids spamming temp.
			std::error_code ec;
			std::filesystem::create_directories(extractDir, ec);
			if (ec) return std::nullopt;
			cleanup.dir = extractDir;
		}

		for (const auto& e : parsed)
		{
			if (!e.name.starts_with("media/")) continue;
			if (e.name.size() <= 6 || e.name.back() == '/') continue; // skip the dir marker

			const std::wstring nameW = Utf8ToUtf16(e.name.c_str(), e.name.size());
			// strip the "media/" prefix (6 chars) for the on-disk name
			const std::wstring fileName = nameW.substr(6);

			++step;
			if (!report(fileName)) return std::nullopt;

			const std::wstring outPath = extractDir + L'\\' + fileName;
			auto bytes = inflateEntry(e);
			if (bytes.empty() && e.uncompressedSize > 0) return std::nullopt;
			HANDLE hf = ::CreateFileW(outPath.c_str(),
				GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hf == INVALID_HANDLE_VALUE) return std::nullopt;
			DWORD wrote = 0;
			const bool ok = ::WriteFile(hf, bytes.data(),
								static_cast<DWORD>(bytes.size()), &wrote, nullptr)
							&& wrote == bytes.size();
			::CloseHandle(hf);
			if (!ok) return std::nullopt;

			// mediaMap key uses the canonical "media://<name>" token
			// that the saver wrote into shaderPath. Loader rewrites
			// any node whose path matches to outPath.
			result.mediaMap[L"media://" + fileName] = outPath;
		}

		cleanup.dir.clear();
		return result;
	}

	std::optional<EffectGraphFile::LoadResult> EffectGraphFile::LoadAny(
		const std::wstring& path,
		const std::wstring& extractDirRoot,
		std::wstring& error,
		const ProgressCallback& progress)
	{
		error.clear();

		// Sniff the PKZIP local-file-header magic.
		uint32_t magic = 0;
		{
			winrt::file_handle file{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
			if (!file)
			{
				error = std::format(L"cannot open '{}' (Win32 error {})", path, ::GetLastError());
				return std::nullopt;
			}
			LARGE_INTEGER fileSize{};
			if (::GetFileSizeEx(file.get(), &fileSize) && fileSize.QuadPart == 0)
			{
				error = std::format(L"'{}' is empty", path);
				return std::nullopt;
			}
			if (fileSize.QuadPart >= 4)
				ReadAt(file.get(), 0, &magic, sizeof(magic));
		}
		if (magic == kLocalSig)
		{
			auto loaded = Load(path, extractDirRoot, progress);
			if (!loaded)
				error = std::format(L"'{}' is not a readable .effectgraph package", path);
			return loaded;
		}

		std::vector<uint8_t> bytes;
		if (!ReadWholeFile(path, bytes))
		{
			error = std::format(L"cannot read '{}' (Win32 error {})", path, ::GetLastError());
			return std::nullopt;
		}

		// Bare JSON; skip a UTF-8 byte order mark.
		size_t start = 0;
		if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
			start = 3;
		LoadResult result;
		result.graphJson = Utf8ToUtf16(reinterpret_cast<const char*>(bytes.data()) + start, bytes.size() - start);
		return result;
	}

	void EffectGraphFile::ResolveMediaTokens(Graph::EffectGraph& graph,
		const std::map<std::wstring, std::wstring>& mediaMap)
	{
		if (mediaMap.empty()) return;
		for (auto& node : const_cast<std::vector<Graph::EffectNode>&>(graph.Nodes()))
		{
			if (node.type != Graph::NodeType::Source || !node.shaderPath.has_value()) continue;
			auto it = mediaMap.find(*node.shaderPath);
			if (it == mediaMap.end()) continue;
			node.shaderPath = it->second;
			auto propertyIt = node.properties.find(L"shaderPath");
			if (propertyIt != node.properties.end())
				propertyIt->second = it->second;
		}
	}

	std::wstring EffectGraphFile::SerializeForSave(Graph::EffectGraph graph, bool embedMedia,
		std::vector<MediaEntry>& media)
	{
		if (!embedMedia)
			return std::wstring(graph.ToJson());

		std::set<std::wstring> usedNames;
		for (auto& node : const_cast<std::vector<Graph::EffectNode>&>(graph.Nodes()))
		{
			if (node.type != Graph::NodeType::Source) continue;
			if (!node.shaderPath.has_value()) continue;
			const std::wstring path = node.shaderPath.value();
			if (path.empty() || path.starts_with(L"media://")) continue;
			// Skip missing files rather than failing the whole save.
			std::error_code ec;
			if (!std::filesystem::exists(path, ec)) continue;

			// Keep the basename when possible; suffix -2, -3 on collision.
			std::wstring base = std::filesystem::path(path).filename().wstring();
			std::wstring name = base;
			int suffix = 2;
			while (usedNames.count(name))
			{
				auto stem = std::filesystem::path(base).stem().wstring();
				auto extension = std::filesystem::path(base).extension().wstring();
				name = stem + L"-" + std::to_wstring(suffix++) + extension;
			}
			usedNames.insert(name);

			MediaEntry entry;
			entry.zipEntryName = L"media/" + name;
			entry.sourcePath = path;
			media.push_back(std::move(entry));

			const std::wstring token = L"media://" + name;
			node.shaderPath = token;
			auto propertyIt = node.properties.find(L"shaderPath");
			if (propertyIt != node.properties.end())
				propertyIt->second = token;
		}
		return std::wstring(graph.ToJson());
	}

	bool EffectGraphFile::SaveJson(const std::wstring& path, const std::wstring& graphJson,
		uint64_t* bytesWritten)
	{
		const std::string utf8 = Utf16ToUtf8(graphJson);
		const std::wstring tempPath = path + L".saving";
		{
			winrt::file_handle file{ ::CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
			if (!file) return false;
			if (!WriteAll(file.get(), utf8.data(), utf8.size()) || !::FlushFileBuffers(file.get()))
			{
				const DWORD writeError = ::GetLastError();
				file.close();
				::DeleteFileW(tempPath.c_str());
				::SetLastError(writeError);
				return false;
			}
		}
		if (!::MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const DWORD moveError = ::GetLastError();
			::DeleteFileW(tempPath.c_str());
			::SetLastError(moveError);
			return false;
		}
		if (bytesWritten) *bytesWritten = utf8.size();
		return true;
	}
}

