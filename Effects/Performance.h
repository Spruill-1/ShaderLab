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

    // Phase 8c: skip-CPU-readback opt-in flag.
    // ============================================
    //
    // When enabled, the evaluator skips the CPU `Map(D3D11_MAP_READ)` of
    // each compute node's analysis structured buffer when no consumer
    // needs the values on the CPU this frame. "Needs CPU" =
    //   (1) any property binding that consumes this node's analysis is
    //       NOT served via GPU SRV (covers pixel-shader consumers, multi-
    //       component bindings, gpuBindable=false params, GpuBindings
    //       feature flag off);
    //   (2) the host explicitly hinted the node via
    //       GraphEvaluator::SetCpuAnalysisInterest (UI selected node,
    //       MCP read_analysis_output target, etc.).
    //
    // When skipping is in effect for a node, `EffectNode::analysisOutput.fields`
    // retains the previous-frame values (or empty if never read). Callers
    // that need fresh values must either include the node in the host
    // hint set or disable the feature for that frame.
    //
    // Default OFF for safe rollout. Tests run with default off.
    SHADERLAB_API bool IsSkipUnneededCpuReadbackEnabled();
    SHADERLAB_API void SetSkipUnneededCpuReadbackEnabled(bool enabled);

    // Async CPU analysis readback. Only takes effect while skip-readback is
    // also on (the GUI's live loop). A readback that is still needed is then
    // QUEUED instead of Mapped straight after the dispatch: each synchronous
    // Map waited for the whole GPU pipeline -- the upstream D2D pre-render
    // included -- once per compute node, in series. Values land one or two
    // frames later, and every consumer bound to them is re-dirtied when they
    // do. Off by default: headless, tests and MCP-forced frames (which turn
    // skip-readback off for the frame) all keep blocking, fresh reads.
    SHADERLAB_API bool IsAsyncAnalysisReadbackEnabled();
    SHADERLAB_API void SetAsyncAnalysisReadbackEnabled(bool enabled);

    // Telemetry: number of dispatches whose CPU readback was skipped
    // since process start. Useful for confirming the optimization
    // actually fires under workload.
    SHADERLAB_API uint64_t SkippedCpuReadbacks();
    SHADERLAB_API void IncrementSkippedCpuReadbacks();

    // Phase 8c throttle: when skip-readback is enabled, host-hinted CPU
    // analysis reads (the selected-node Properties panel display, the
    // node-graph canvas value labels) are throttled to this interval.
    // CPU-routed *property bindings* are unaffected -- they still get
    // a fresh value every frame because their consumer needs it.
    //
    // Default: 2000 ms (0.5 Hz). Static graphs effectively read once
    // per selection because the values dont change between throttled
    // readbacks; video graphs update twice per second which matches
    // human-readable refresh rate. Set to 0 to disable throttling
    // (every frame for hinted nodes).
    SHADERLAB_API uint32_t CpuAnalysisHintThrottleMs();
    SHADERLAB_API void SetCpuAnalysisHintThrottleMs(uint32_t ms);

    // Scope of those host hints. Off (default): the host hints only the
    // selected node and the nodes its bindings read. On: every compute
    // node is hinted, so the canvas value labels on unselected nodes
    // refresh too, at the throttle above. The host reads this when it
    // builds the hint set; the evaluator never does. Together with the
    // throttle this replaces the old "keep analysis values live" switch,
    // which turned the skip off and so also undid the GPU-binding win.
    SHADERLAB_API bool IsCpuAnalysisHintAllNodesEnabled();
    SHADERLAB_API void SetCpuAnalysisHintAllNodesEnabled(bool enabled);

    // The flyout's "Refresh analysis readouts" presets, shared with the
    // perf_render_mode MCP route so the two cannot drift:
    //   0 = selected node (+ its binding sources), every 2 s   (default)
    //   1 = every compute node, every 2 s
    //   2 = every compute node, every 250 ms
    //   3 = every compute node, every frame (a Map() stall per frame)
    // Sets the two settings above; returns false for an unknown preset.
    // Current preset is reported back by AnalysisReadoutPreset() (-1 when
    // the two settings match no preset).
    SHADERLAB_API bool SetAnalysisReadoutPreset(int32_t preset);
    SHADERLAB_API int32_t AnalysisReadoutPreset();

    // Telemetry: count of bindings the evaluator detected as GPU-routable
    // since process start (whether or not actually routed).
    SHADERLAB_API uint64_t GpuBindingDetections();

    // Internal: bumped by the evaluator when a binding is detected as
    // GPU-routable. Exported for engine-side use only.
    SHADERLAB_API void IncrementGpuBindingDetection();

    // Clean-subgraph output caching: when enabled, the evaluator sets
    // D2D1_PROPERTY_CACHED on per-node effect outputs so re-drawing the
    // terminal does not re-execute pixel passes whose subtree is
    // unchanged, and invalidates those caches off the dirty walk (the
    // D2D-invisible in-place texture updates: compute re-dispatch, video
    // and live-capture uploads). Costs one GPU intermediate per cached
    // node at its output resolution.
    //
    // Default ON; the flag exists as a kill switch if a stale-frame
    // regression is suspected.
    SHADERLAB_API bool IsEffectOutputCachingEnabled();
    SHADERLAB_API void SetEffectOutputCachingEnabled(bool enabled);

    // Unthrottled render loop -- the benchmark mode.
    //
    // Normally the render worker paces itself to frame deadlines at the
    // monitor refresh rate (FramePacer, 60-240 Hz), and the UI presents with
    // a vsync interval of 1. Neither limit exists to protect correctness: they exist
    // so an editor does not spin a core and peg a GPU while someone is
    // reading the screen.
    //
    // For measuring throughput both are exactly wrong. They cap the number
    // being measured at the display's cadence, which says nothing about how
    // fast the pipeline can actually go -- the same way judging an HDR image
    // on an SDR panel tells you about the panel, not the image. With this on:
    //
    //   * the worker drains its queue without blocking and loops immediately,
    //   * the graph re-evaluates every tick whether or not anything is dirty
    //     (an unthrottled loop over a clean graph would otherwise spin doing
    //     nothing, which is a pure waste rather than a measurement),
    //   * the UI presents with interval 0, so a Present does not block the UI
    //     thread waiting for scanout.
    //
    // Expect tearing, a pegged GPU and fans. That is the intended trade: this
    // is a development and benchmarking tool, and the honest throughput number
    // is worth more here than a smooth 60. Default OFF.
    SHADERLAB_API bool IsUnthrottledRenderEnabled();
    SHADERLAB_API void SetUnthrottledRenderEnabled(bool enabled);

    // Sub-rect input demand in custom pixel-shader transforms.
    //
    // `MapOutputRectToInputRects` can answer a tile query either with the
    // tile it was asked about, or with the whole image. Whole-image is only
    // free while D2D renders in a single pass; past ~2048 px WIDE it tiles,
    // and a whole-image answer makes it re-render the entire upstream chain
    // once per tile. Measured on a 2880x1800 desktop capture through a
    // gamut-mapping tone mapper: 53 ms -> 2.6 ms, with the cliff sitting between
    // 2048 (1.10 ms) and 2100 (56.05 ms) px wide.
    //
    // TRIVIAL_SAMPLING is declared, so output pixel N depends only on input
    // pixel N and the sub-rect answer is the correct one. The flag exists
    // because the whole-image answer is what shipped, and a kill switch is
    // cheaper than a revert if some effect turns out to read `uv0` as if it
    // spanned the whole image.
    //
    // Default ON.
    SHADERLAB_API bool IsSubRectInputDemandEnabled();
    SHADERLAB_API void SetSubRectInputDemandEnabled(bool enabled);

    // Compute dispatches recorded on the runner's own deferred context and
    // submitted as one ExecuteCommandList (default ON). Off issues them on the
    // shared immediate context call by call, as before -- which is racy
    // against Direct2D on other threads, so OFF exists only to A/B the cost.
    SHADERLAB_API bool IsComputeCommandListEnabled();
    SHADERLAB_API void SetComputeCommandListEnabled(bool enabled);
}
