#include "pch_engine.h"
#include "EffectGraphFile.h"

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

namespace
{
	// CRC-32/IEEE 802.3 polynomial 0xEDB88320 (reflected). The same
	// CRC is required by both the local file header and the central
	// directory record. Standard table-driven implementation; cached
	// on first use to avoid recomputing 256 entries per Save call.
	const uint32_t* GetCrcTable()
	{
		static uint32_t table[256];
		static bool initialized = false;
		if (!initialized)
		{
			for (uint32_t i = 0; i < 256; ++i)
			{
				uint32_t c = i;
				for (int k = 0; k < 8; ++k)
					c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				table[i] = c;
			}
			initialized = true;
		}
		return table;
	}

	uint32_t Crc32(const uint8_t* data, size_t size)
	{
		const uint32_t* table = GetCrcTable();
		uint32_t crc = 0xFFFFFFFFu;
		for (size_t i = 0; i < size; ++i)
			crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
		return crc ^ 0xFFFFFFFFu;
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

	// Append one stored ZIP entry (name + bytes). Tracks where it
	// landed in the file so the central directory can point back.
	struct PendingEntry
	{
		std::string name;            // UTF-8, may contain '/' and end with '/'
		std::vector<uint8_t> data;   // empty for directory markers
		uint32_t crc{};
		uint32_t localHeaderOffset{};
	};

	bool WriteLocalEntry(HANDLE h, PendingEntry& e, uint32_t& cursor)
	{
		e.localHeaderOffset = cursor;
		e.crc = Crc32(e.data.data(), e.data.size());

		LocalFileHeader hdr{};
		hdr.signature = kLocalSig;
		hdr.versionNeeded = 20;
		hdr.generalPurposeFlag = 0x0800; // UTF-8 names
		hdr.compressionMethod = 0;
		hdr.lastModFileTime = 0;
		hdr.lastModFileDate = (1 << 9) | (1 << 5) | 1; // 1980-01-01
		hdr.crc32 = e.crc;
		hdr.compressedSize = static_cast<uint32_t>(e.data.size());
		hdr.uncompressedSize = static_cast<uint32_t>(e.data.size());
		hdr.fileNameLength = static_cast<uint16_t>(e.name.size());
		hdr.extraFieldLength = 0;

		if (!WriteAll(h, &hdr, sizeof(hdr))) return false;
		if (!WriteAll(h, e.name.data(), e.name.size())) return false;
		if (!e.data.empty() && !WriteAll(h, e.data.data(), e.data.size())) return false;

		cursor += sizeof(hdr) + static_cast<uint32_t>(e.name.size())
				+ static_cast<uint32_t>(e.data.size());
		return true;
	}

	bool WriteCentralEntry(HANDLE h, const PendingEntry& e, uint32_t& cursor)
	{
		CentralDirectoryHeader cd{};
		cd.signature = kCentralSig;
		cd.versionMadeBy = 20;
		cd.versionNeeded = 20;
		cd.generalPurposeFlag = 0x0800;
		cd.compressionMethod = 0;
		cd.lastModFileTime = 0;
		cd.lastModFileDate = (1 << 9) | (1 << 5) | 1;
		cd.crc32 = e.crc;
		cd.compressedSize = static_cast<uint32_t>(e.data.size());
		cd.uncompressedSize = static_cast<uint32_t>(e.data.size());
		cd.fileNameLength = static_cast<uint16_t>(e.name.size());
		// External attributes: directory entry sets the MS-DOS dir bit
		// (0x10) so unzip tools render it as a folder.
		cd.externalFileAttrs = (!e.name.empty() && e.name.back() == '/') ? 0x10u : 0u;
		cd.localHeaderOffset = e.localHeaderOffset;

		if (!WriteAll(h, &cd, sizeof(cd))) return false;
		if (!WriteAll(h, e.name.data(), e.name.size())) return false;

		cursor += sizeof(cd) + static_cast<uint32_t>(e.name.size());
		return true;
	}
}

namespace ShaderLab::Rendering
{
	bool EffectGraphFile::Save(const std::wstring& path,
							   const std::wstring& graphJson,
							   const std::vector<MediaEntry>& media,
							   const ProgressCallback& progress)
	{
		const std::string jsonUtf8 = Utf16ToUtf8(graphJson);

		// Total step count for progress: 1 (graph.json) + N media files.
		const uint32_t total = 1 + static_cast<uint32_t>(media.size());
		uint32_t step = 0;
		auto report = [&](const std::wstring& msg) -> bool
		{
			if (progress) return progress(step, total, msg);
			return true;
		};

		// Build the in-memory entry list. graph.json + optional media.
		std::vector<PendingEntry> entries;
		entries.reserve(2 + media.size());

		++step;
		if (!report(L"graph.json")) return false;
		{
			PendingEntry e;
			e.name = "graph.json";
			e.data.assign(jsonUtf8.begin(), jsonUtf8.end());
			entries.push_back(std::move(e));
		}
		{
			// Always emit the media/ folder marker -- even when no
			// files are embedded -- so external zip tools render the
			// folder consistently.
			PendingEntry e;
			e.name = "media/";
			entries.push_back(std::move(e));
		}

		for (const auto& m : media)
		{
			++step;
			if (!report(std::filesystem::path(m.sourcePath).filename().wstring()))
				return false;

			PendingEntry e;
			e.name = Utf16ToUtf8(m.zipEntryName);

			// Read the source file into memory. Media files (images
			// / video) can be tens or hundreds of MB; we still slurp
			// them whole because writing a stored ZIP entry needs the
			// CRC and size up front. If this becomes a problem we
			// can stream by hashing first then re-reading.
			HANDLE hf = ::CreateFileW(m.sourcePath.c_str(),
				GENERIC_READ, FILE_SHARE_READ, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hf == INVALID_HANDLE_VALUE) return false;
			LARGE_INTEGER fsz{};
			if (!::GetFileSizeEx(hf, &fsz) || fsz.QuadPart > 0xFFFFFFFFLL)
			{
				::CloseHandle(hf);
				return false; // 4 GB cap (no ZIP64)
			}
			e.data.resize(static_cast<size_t>(fsz.QuadPart));
			DWORD rd = 0;
			if (!::ReadFile(hf, e.data.data(), static_cast<DWORD>(e.data.size()), &rd, nullptr)
				|| rd != e.data.size())
			{
				::CloseHandle(hf);
				return false;
			}
			::CloseHandle(hf);
			entries.push_back(std::move(e));
		}

		HANDLE h = ::CreateFileW(
			path.c_str(),
			GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;

		uint32_t cursor = 0;
		bool ok = true;

		for (auto& e : entries)
			ok = ok && WriteLocalEntry(h, e, cursor);

		const uint32_t cdStart = cursor;
		for (auto& e : entries)
			ok = ok && WriteCentralEntry(h, e, cursor);
		const uint32_t cdSize = cursor - cdStart;

		EndOfCentralDirectory eocd{};
		eocd.signature = kEocdSig;
		eocd.entriesOnDisk = static_cast<uint16_t>(entries.size());
		eocd.totalEntries = static_cast<uint16_t>(entries.size());
		eocd.centralDirSize = cdSize;
		eocd.centralDirOffset = cdStart;
		ok = ok && WriteAll(h, &eocd, sizeof(eocd));

		::CloseHandle(h);
		return ok;
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
		HANDLE h = ::CreateFileW(
			path.c_str(),
			GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return std::nullopt;

		LARGE_INTEGER size{};
		if (!::GetFileSizeEx(h, &size) || size.QuadPart < (LONGLONG)sizeof(EndOfCentralDirectory))
		{
			::CloseHandle(h);
			return std::nullopt;
		}

		std::vector<uint8_t> buf(static_cast<size_t>(size.QuadPart));
		DWORD read = 0;
		if (!::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)
			|| read != buf.size())
		{
			::CloseHandle(h);
			return std::nullopt;
		}
		::CloseHandle(h);

		// First pass: count entries and find graph.json so we can
		// report meaningful progress totals.
		struct ParsedEntry
		{
			std::string name;
			size_t dataOff;
			uint32_t size;
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
			if (hdr.compressionMethod != 0) return std::nullopt;

			const size_t nameOff = pos + sizeof(hdr);
			const size_t dataOff = nameOff + hdr.fileNameLength + hdr.extraFieldLength;
			const size_t dataEnd = dataOff + hdr.compressedSize;
			if (dataEnd > buf.size()) return std::nullopt;

			const char* nameP = reinterpret_cast<const char*>(buf.data() + nameOff);
			ParsedEntry pe;
			pe.name.assign(nameP, hdr.fileNameLength);
			pe.dataOff = dataOff;
			pe.size = hdr.compressedSize;
			parsed.push_back(std::move(pe));

			pos = dataEnd;
		}

		// Find graph.json.
		std::optional<std::wstring> graphJson;
		for (const auto& e : parsed)
			if (e.name == "graph.json")
				graphJson = Utf8ToUtf16(reinterpret_cast<const char*>(buf.data() + e.dataOff), e.size);
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
			std::filesystem::create_directories(extractDir);
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
			HANDLE hf = ::CreateFileW(outPath.c_str(),
				GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hf == INVALID_HANDLE_VALUE) return std::nullopt;
			DWORD wrote = 0;
			const bool ok = ::WriteFile(hf, buf.data() + e.dataOff, e.size, &wrote, nullptr)
							&& wrote == e.size;
			::CloseHandle(hf);
			if (!ok) return std::nullopt;

			// mediaMap key uses the canonical "media://<name>" token
			// that the saver wrote into shaderPath. Loader rewrites
			// any node whose path matches to outPath.
			result.mediaMap[L"media://" + fileName] = outPath;
		}

		return result;
	}
}

