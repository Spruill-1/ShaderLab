#pragma once
#ifdef SHADERLAB_ENGINE_EXPORTS
#define SHADERLAB_API __declspec(dllexport)
#else
#define SHADERLAB_API __declspec(dllimport)
#endif

// Engine ABI version. Bump it by hand whenever an exported symbol's
// signature, an exported class's layout or a public behaviour changes in a
// way that breaks a host built against older headers; an unreleased version
// takes further changes without another bump. Independent of Version.h's
// app version and GraphFormatVersion. Hosts compare ShaderLab_GetAbiVersion()
// against this constant at startup; a mismatch means the DLL and headers come
// from different builds. 7 is unreleased. History of every version:
// docs/architecture/engine-host-split.md#engine-abi-version.
#define SHADERLAB_ENGINE_ABI_VERSION 7

// C-linkage entry so it can be GetProcAddress'd if a host wants to do a
// version check before dynamically loading the DLL.
extern "C" SHADERLAB_API uint32_t ShaderLab_GetAbiVersion();

