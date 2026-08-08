#pragma once

// miniz's CMake build generates this header via generate_export_header() to
// decorate the public API for shared-library builds. We link miniz statically
// into ShaderLabEngine.dll -- its symbols are internal and never re-exported --
// so an empty macro is the correct definition, and it lets us consume the miniz
// submodule without dragging a CMake toolchain into an MSBuild-only repo.
//
// This mirrors what miniz's own amalgamation step does (CMakeLists.txt injects
// the same empty #define into the generated single-file miniz.h).
#define MINIZ_EXPORT
