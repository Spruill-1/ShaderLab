#pragma once

#include "../EngineExport.h"
#include <atomic>

namespace ShaderLab::Performance
{
    // Phase 8 GPU-binding feature flag
    // ==================================
    //
    // When enabled, the evaluator's binding-resolution pass detects
    // upstream effects that publish IEngineComputeOutput AND consumer
    // parameters flagged gpuBindable, and routes them GPU-side via the
    // CustomComputeBridgeEffect's SetGpuBinding entry instead of via
    // Map() readback. v1.6 ships with this default ON.
    //
    // CLI flag --enable-gpu-bindings / --disable-gpu-bindings flips it
    // for headless. Tests can flip it via SetGpuBindingsEnabled to
    // exercise both paths.
    //
    // Routing behavior:
    //   - D3D11 compute consumers (via CustomComputeBridgeEffect): bind
    //     the upstream IEngineComputeOutput SRV at the consumer's
    //     t-slot via the bridge's SetGpuBinding entry. Variant bytecode
    //     (compiled with _SLPARAM_<name>_GPU=1 macros) comes from the
    //     BytecodeCache (eager precompile fills the +N variants on
    //     first encounter so the swap is a cache hit).
    //   - D2D pixel-shader consumers (no bridge entry): the m_bridgeImplCache
    //     miss naturally skips the GPU plan; CPU readback path runs as
    //     graceful fallback. A compute upstream feeding a pixel shader
    //     downstream still works -- just at today's cbuffer-pack speed.

    SHADERLAB_API bool IsGpuBindingsEnabled();
    SHADERLAB_API void SetGpuBindingsEnabled(bool enabled);

    // Telemetry: count of bindings the evaluator detected as GPU-routable
    // since process start (whether or not actually routed).
    SHADERLAB_API uint64_t GpuBindingDetections();

    // Internal: bumped by the evaluator when a binding is detected as
    // GPU-routable. Exported for engine-side use only.
    SHADERLAB_API void IncrementGpuBindingDetection();
}
