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
    // parameters flagged gpuBindable, and *attempts* to route them
    // GPU-side instead of via Map() readback.
    //
    // For the v1.6 release this defaults OFF so behavior is unchanged
    // for end users. CLI flag --enable-gpu-bindings / --disable-gpu-
    // bindings flip it for headless. The status bar (future) will
    // expose a checkbox once enough effects are migrated.
    //
    // The actual GPU routing path is consumer-effect-specific:
    //   - D3D11 compute consumers (via CustomComputeBridgeEffect): can
    //     bind the upstream SRV to a t-slot via a future SetGpuBinding
    //     entry on the bridge (added when the first consumer needs it).
    //   - D2D pixel-shader consumers: cannot bind raw SRVs through
    //     D2D's input mechanism; would need a ResourceTexture-update
    //     path (deferred).
    //
    // When the flag is enabled but the consumer's effect class has no
    // GPU-binding implementation yet, the evaluator falls back to CPU
    // readback for that binding (no behavior change for that node).
    //
    // For v1.6, this scaffolding lets us:
    //   1. Track how many graphs *would* benefit from GPU binding via
    //      GpuBindingDetections() telemetry.
    //   2. Validate the macro-wrapped HLSL compiles and runs identically
    //      to today's path with the flag off.
    //   3. Flip the flag once a future commit adds the actual SRV
    //      routing for at least one consumer effect class.

    SHADERLAB_API bool IsGpuBindingsEnabled();
    SHADERLAB_API void SetGpuBindingsEnabled(bool enabled);

    // Telemetry: count of bindings the evaluator detected as GPU-routable
    // since process start (whether or not actually routed).
    SHADERLAB_API uint64_t GpuBindingDetections();

    // Internal: bumped by the evaluator when a binding is detected as
    // GPU-routable. Exported for engine-side use only.
    SHADERLAB_API void IncrementGpuBindingDetection();
}
