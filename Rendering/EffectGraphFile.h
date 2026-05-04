#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"

namespace ShaderLab::Rendering
{
	// EffectGraphFile -- read/write of the .effectgraph container.
	//
	// The on-disk format is a standard ZIP archive (PKZIP, "store"
	// method, no compression) with two well-known entries:
	//
	//   graph.json   -- the EffectGraph JSON serialization.
	//   media/       -- (optional) embedded source files (images,
	//                   videos, ICC) referenced by the graph. When
	//                   present, the graph JSON's path properties use
	//                   the special prefix "media://<name>" so the
	//                   loader can rewrite them to extracted temp
	//                   paths without ambiguity.
	//
	// The writer / reader use only Win32 APIs (CreateFileW, ReadFile,
	// WriteFile) -- no third-party dependencies. Entries are stored
	// uncompressed; ZIP just gives us a single-file container with
	// CRC validation that any tool can inspect.

	class SHADERLAB_API EffectGraphFile
	{
	public:
		// One source file to embed under media/ in the zip.
		struct MediaEntry
		{
			// Name written into the archive (e.g. "media/foo.png").
			// Must start with "media/" and be unique within the save.
			std::wstring zipEntryName;
			// Absolute path to the source file on disk.
			std::wstring sourcePath;
		};

		// Per-file callback for save / load progress reporting.
		// current and total are 1-based; current==0 means "just
		// starting" before any file is processed. message is a
		// short human-readable status (filename or stage).
		// Return false from the callback to abort.
		using ProgressCallback = std::function<bool(uint32_t current, uint32_t total, const std::wstring& message)>;

		// Serialize the given JSON string into a .effectgraph zip at
		// the given file path. Overwrites if the file exists. Returns
		// true on success; false if the file could not be written.
		static bool Save(const std::wstring& path,
						 const std::wstring& graphJson,
						 const std::vector<MediaEntry>& media = {},
						 const ProgressCallback& progress = {});

		// Load result: graph JSON plus a map from "media://<name>"
		// tokens to the extracted absolute path on disk. Caller is
		// responsible for cleaning up the extraction directory; we
		// place everything under a single root (extractDir) so the
		// caller can blow it away wholesale on shutdown.
		struct LoadResult
		{
			std::wstring graphJson;
			std::wstring extractDir;                  // where files were extracted
			std::map<std::wstring, std::wstring> mediaMap; // "media://<name>" -> abs path
		};

		// Load a .effectgraph at path. extractDirRoot is a directory
		// under which a unique subdirectory is created to hold the
		// extracted media files. Pass GetTempPathW() result for the
		// standard location. Returns std::nullopt on failure.
		static std::optional<LoadResult> Load(const std::wstring& path,
											  const std::wstring& extractDirRoot,
											  const ProgressCallback& progress = {});
	};
}

