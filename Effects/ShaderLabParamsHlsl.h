#pragma once

// shaderlab_params.hlsli — content embedded as a string constant.
//
// This header defines a small set of macros that ShaderLab's built-in
// effects and Effect Designer-authored shaders use to declare
// parameters that may be either CPU-driven (cbuffer) or GPU-driven
// (StructuredBuffer<float4> bound by the host from an upstream
// IEngineComputeOutput effect).
//
// The macro file lives engine-side as a `const char*` so the GUI app
// and the headless host don't need to ship a separate file. The
// ShaderCompiler's ID3DInclude implementation resolves
//   #include "shaderlab_params.hlsli"
// to this string, no on-disk lookup needed.

#include "../EngineExport.h"

namespace ShaderLab::Effects
{
    // Filename ShaderCompiler's include handler responds to. Embedded
    // shaders use #include "shaderlab_params.hlsli" — case-sensitive,
    // matches by filename only (no path prefix).
    inline constexpr const char* kShaderLabParamsIncludeName = "shaderlab_params.hlsli";

    // Returns the embedded macro library content. Pointer remains
    // valid for the process lifetime; do not free.
    SHADERLAB_API const char* GetShaderLabParamsHLSL();

    // Length of the embedded content in bytes (excluding any trailing
    // null terminator). Matches strlen(GetShaderLabParamsHLSL()).
    SHADERLAB_API size_t GetShaderLabParamsHLSLLength();

    // Multi-group reduction contract, shared by the HLSL library below
    // (as SHADERLAB_REDUCE_* defines -- a test asserts they match) and
    // D3D11ComputeRunner, which binds the scratch buffer and sizes the
    // dispatch. A D3D11 compute shader opts in by writing
    // SHADERLAB_REDUCE_SCRATCH at file scope; reflection detects the
    // `_SLScratch` UAV, and the runner then
    //   * binds a uint scratch buffer of kReduceScratchUints at u2,
    //   * zeroes ONLY its first kReduceScratchClearedUints words before
    //     every dispatch (word 0 is the completion counter; the rest of
    //     that head is for InterlockedAdd accumulators), and
    //   * dispatches kReduceGroups x 1 x 1 groups when the caller asked for
    //     (1,1,1).
    // Everything above the cleared head is per-group partials, which each
    // group must write in full -- that is what keeps the clear small.
    inline constexpr uint32_t kReduceGroups              = 64;
    inline constexpr uint32_t kReduceScratchUints        = 1u << 19;   // 2 MB
    inline constexpr uint32_t kReduceScratchClearedUints = 16384;      // 64 KB
}
