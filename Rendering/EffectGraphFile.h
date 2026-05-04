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
	//   media/       -- empty directory marker reserved for embedded
	//                   media (images, video, ICC profiles, ...).
	//                   Currently not populated; kept so future
	//                   versions can drop files in without changing
	//                   the container layout.
	//
	// We write uncompressed entries because the JSON payload is small
	// (kB range) and we want to avoid pulling in a deflate library for
	// a single file. The reader still accepts only stored entries; if
	// we ever add compression we'll bump that gate too.
	//
	// The writer / reader use only Win32 APIs (CreateFileW, ReadFile,
	// WriteFile) -- no third-party dependencies.

	class SHADERLAB_API EffectGraphFile
	{
	public:
		// Serialize the given JSON string into a .effectgraph zip at
		// the given file path. Overwrites if the file exists. Returns
		// true on success; false if the file could not be written.
		static bool Save(const std::wstring& path, const std::wstring& graphJson);

		// Load and return the graph JSON string from a .effectgraph
		// zip at the given path. Returns std::nullopt if the file is
		// unreadable, not a zip, or the graph.json entry is missing.
		static std::optional<std::wstring> Load(const std::wstring& path);
	};
}
