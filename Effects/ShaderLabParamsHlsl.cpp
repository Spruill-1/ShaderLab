#include "pch_engine.h"
#include "ShaderLabParamsHlsl.h"

#include <cstring>

namespace ShaderLab::Effects
{
    // The macro file content. See ShaderLabParamsHlsl.h for usage notes.
    //
    // Per-parameter binding mode is injected by the host as a numeric
    // macro: _SLPARAM_<name>_GPU == 0 (cbuffer), 1 (StructuredBuffer SRV) or
    // 2 (smuggled through a Texture2D effect input).
    //
    // Mode 2 exists because Direct2D will not let a custom PIXEL shader bind
    // an arbitrary SRV: it maps a transform's INPUTS to Texture2D at t0, t1,
    // ... and nothing else. The obvious alternative, ID2D1EffectContext::
    // CreateResourceTexture, takes `const BYTE*` CPU memory -- so routing an
    // analysis result through one would require the GPU->CPU readback the GPU
    // binding exists to avoid. Publishing the values as a 1-row RGBA32F image
    // and wiring it in as an ordinary effect input keeps them GPU-resident.
    // The host always injects the macro for every gpu-bindable param,
    // even when no binding is active, so the effect's source compiles
    // identically regardless of binding state.
    //
    // Authors write three pieces (file scope, cbuffer body, function body):
    //   SHADERLAB_GPU_BUFFER(TargetPeakNits, t1)        // file scope
    //   SHADERLAB_PARAM(float, TargetPeakNits)          // inside cbuffer
    //   SHADERLAB_LOAD_PARAM(float, TargetPeakNits)     // inside main()
    // and use TargetPeakNits as a normal local downstream. The host
    // does the rest: emits _SLPARAM_TargetPeakNits_GPU=0|1 and, when 1,
    // binds an SRV to the right t-slot.
    static constexpr const char* kSource = R"HLSL(
// shaderlab_params.hlsli — engine-embedded macro library.
// See Effects/ShaderLabParamsHlsl.h for documentation.
#ifndef SHADERLAB_PARAMS_HLSLI_INCLUDED
#define SHADERLAB_PARAMS_HLSLI_INCLUDED

// Token-paste indirection for proper macro argument expansion.
#define _SLPARAM_CAT(a, b) _SLPARAM_CAT_(a, b)
#define _SLPARAM_CAT_(a, b) a##b

// Type-aware swizzle helpers: pull the right components from a float4
// storage slot for any of float / float2 / float3 / float4.
#define _SL_VEC_float(v)  (v).x
#define _SL_VEC_float2(v) (v).xy
#define _SL_VEC_float3(v) (v).xyz
#define _SL_VEC_float4(v) (v)

// File-scope buffer declaration. Expands to a StructuredBuffer<float4>
// when the parameter is GPU-bound; expands to nothing when the value
// lives in the cbuffer.
#define SHADERLAB_GPU_BUFFER(name, slot) \
    _SLPARAM_CAT(_SLBUF_, _SLPARAM_##name##_GPU)(name, slot)
#define _SLBUF_0(name, slot)
#define _SLBUF_1(name, slot) StructuredBuffer<float4> _SLBuf_##name : register(slot);
// Mode 2: the values arrive as an effect INPUT image, so the declaration is a
// Texture2D at the input's t-slot. One row, one texel per analysis field.
#define _SLBUF_2(name, slot) Texture2D<float4> _SLTex_##name : register(slot);

// Inside cbuffer block. Expands to a value slot when cbuffer-bound;
// expands to a uint INDEX slot when GPU-bound (the host packs the
// upstream field's index in the analysis SRV at this offset).
#define SHADERLAB_PARAM(type, name) \
    _SLPARAM_CAT(_SLPARAM_, _SLPARAM_##name##_GPU)(type, name)
#define _SLPARAM_0(type, name) type name;
#define _SLPARAM_1(type, name) uint _SLIdx_##name;
// Mode 2 packs the same index slot as mode 1 -- only the fetch differs.
#define _SLPARAM_2(type, name) uint _SLIdx_##name;

// Inside function body, before any use of the parameter. Expands to
// a local-variable assignment when GPU-bound (read float4 at the
// host-supplied index, swizzle to the requested type); expands to
// nothing when cbuffer-bound (the cbuffer slot is already in scope
// as a global).
#define SHADERLAB_LOAD_PARAM(type, name) \
    _SLPARAM_CAT(_SLLOAD_, _SLPARAM_##name##_GPU)(type, name)
#define _SLLOAD_0(type, name)
#define _SLLOAD_1(type, name) type name = _SL_VEC_##type(_SLBuf_##name[_SLIdx_##name]);
// Load(), not Sample(): these are exact values at known texel positions, and
// a sampler would filter neighbouring FIELDS together. mip 0, row 0, one texel
// per field, matching the layout the compute bridge publishes.
#define _SLLOAD_2(type, name) \
    type name = _SL_VEC_##type(_SLTex_##name.Load(int3((int)_SLIdx_##name, 0, 0)));

// Direct access, for an effect that wants a whole analysis row rather than
// one parameter at a time. `index` is the field's position in the producing
// node's analysisFields list.
float4 ShaderLabAnalysisTexel(Texture2D<float4> tex, uint index)
{
    return tex.Load(int3((int)index, 0, 0));
}
float ShaderLabAnalysisField(Texture2D<float4> tex, uint index)
{
    return ShaderLabAnalysisTexel(tex, index).x;
}

// ---- Multi-group reductions -------------------------------------------------
// A whole-image reduction written as ONE thread group runs on one CU/SM, so a
// 4K frame walked by 1024 threads uses a sliver of the GPU (1-6 ms measured).
// Opt in with SHADERLAB_REDUCE_SCRATCH and the host dispatches
// SHADERLAB_REDUCE_GROUPS groups instead, with a scratch buffer at u2:
//   words [0, SHADERLAB_REDUCE_CLEARED)  zeroed before every dispatch;
//                                         word 0 is the completion counter
//   words [SHADERLAB_REDUCE_CLEARED, SHADERLAB_REDUCE_SCRATCH_UINTS)
//                                         NOT cleared -- per-group partials,
//                                         each group writes all of its own
// Pattern: every group reduces its share into groupshared, writes its partial
// block, then calls ShaderLabReduceIsLastGroup(tid) (from ALL threads). That
// returns true in exactly one group -- the last to finish -- which reads every
// group's partials back and writes Result. Values must match
// Effects/ShaderLabParamsHlsl.h (a test checks).
#define SHADERLAB_REDUCE_GROUPS        64
#define SHADERLAB_REDUCE_SCRATCH_UINTS 524288
#define SHADERLAB_REDUCE_CLEARED       16384
#define SHADERLAB_REDUCE_SCRATCH \
    globallycoherent RWByteAddressBuffer _SLScratch : register(u2); \
    groupshared uint _SLIsLastGroup; \
    bool ShaderLabReduceIsLastGroup(uint tid) \
    { \
        DeviceMemoryBarrierWithGroupSync(); \
        if (tid == 0) \
        { \
            uint prev; \
            _SLScratch.InterlockedAdd(0, 1u, prev); \
            _SLIsLastGroup = (prev == SHADERLAB_REDUCE_GROUPS - 1) ? 1u : 0u; \
        } \
        GroupMemoryBarrierWithGroupSync(); \
        return _SLIsLastGroup != 0; \
    }
#define ShaderLabScratchStore(word, value)  _SLScratch.Store((word) * 4, (value))
#define ShaderLabScratchLoad(word)          _SLScratch.Load((word) * 4)

#endif // SHADERLAB_PARAMS_HLSLI_INCLUDED
)HLSL";

    const char* GetShaderLabParamsHLSL() { return kSource; }
    size_t      GetShaderLabParamsHLSLLength() { return std::strlen(kSource); }
}
