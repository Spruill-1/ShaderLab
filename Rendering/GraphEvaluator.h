#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/EffectGraph.h"
#include "../Effects/CustomPixelShaderEffect.h"
#include "../Effects/CustomComputeShaderEffect.h"
#include "../Effects/CustomComputeBridgeEffect.h"
#include "../Effects/ShaderCompiler.h"
#include "D3D11ComputeRunner.h"
#include "GpuTimer.h"

namespace ShaderLab::Rendering
{
    // Evaluates an EffectGraph by walking nodes in topological order,
    // creating / caching D2D effects, wiring inputs from upstream outputs,
    // and returning the final ID2D1Image* for presentation.
    //
    // The evaluator keeps a per-node effect cache so that effects are only
    // recreated when the node type or CLSID changes (not every frame).
    // Properties are re-applied each frame on dirty nodes.
    //
    // Usage:
    //   auto* finalImage = evaluator.Evaluate(graph, deviceContext);
    //   if (finalImage) { dc->DrawImage(finalImage); }
    class SHADERLAB_API GraphEvaluator
    {
    public:
        GraphEvaluator() = default;
        GraphEvaluator(const GraphEvaluator&) = delete;
        GraphEvaluator& operator=(const GraphEvaluator&) = delete;

        // Optional GPU timer, owned by the host. When set and enabled, the
        // evaluator brackets each compute dispatch so per-node GPU time can be
        // attributed exactly -- a compute dispatch is its own submission, so
        // this costs nothing and perturbs nothing. Image nodes are NOT timed
        // here: Direct2D evaluates an effect chain lazily at DrawImage, so
        // they are not separately dispatched and have to be materialised one
        // at a time, which is a profiling mode rather than free.
        void SetGpuTimer(GpuTimer* timer) { m_gpuTimer = timer; }

        // Walk the graph in topological order and produce the final output image.
        // Returns nullptr if the graph is empty or has no Output node.
        ID2D1Image* Evaluate(Graph::EffectGraph& graph, ID2D1DeviceContext5* dc);

        // Run deferred D3D11 compute dispatches for statistics nodes.
        // Call AFTER Evaluate (and double-eval) when all D2D effects are initialized.
        // Returns true if any compute dispatches ran (analysis output changed).
        bool ProcessDeferredCompute(Graph::EffectGraph& graph, ID2D1DeviceContext5* dc);
        // QUEUE depth, not work done -- and it is empty by the time anyone
        // outside can read it, because ProcessDeferredCompute drains the list
        // before returning. Use DispatchesLastFrame() for "how many compute
        // dispatches actually ran"; this one reported 0 forever.
        size_t DeferredComputeCount() const { return m_deferredCompute.size(); }

        // Compute dispatches actually issued during the last
        // ProcessDeferredCompute. Survives the drain.
        uint32_t DispatchesLastFrame() const { return m_dispatchesLastFrame; }

        // Phase 8c: when set true, EvaluateNode for D3D11 compute effects
        // skips appending to `m_deferredCompute`. The host sets this before
        // the post-PDC re-evaluate pass (which exists only to re-apply
        // properties on D2D effects downstream of just-dispatched compute
        // bridges) so leftover compute entries don't leak into the next
        // frame's PDC and cause a duplicate dispatch with stale source.
        // Reset to false before pass 1 of each frame.
        void SetDeferredComputeFrozen(bool frozen) { m_deferredComputeFrozen = frozen; }

        // Async CPU readback (GUI live loop only; see
        // Performance::IsAsyncAnalysisReadbackEnabled). Values copied for a
        // node on one frame are collected here on a later one: returns true
        // when any node's analysis fields changed, and marks every consumer
        // bound to those fields dirty so it re-evaluates with them. Evaluate
        // calls it itself; hosts only need HasPendingReadbacks() to keep
        // rendering until the values have landed.
        bool PollAsyncReadbacks(Graph::EffectGraph& graph);
        bool HasPendingReadbacks() const { return !m_asyncReadbackPending.empty(); }

        // Resolve property bindings for Source nodes only (lightweight, no D2D).
        // Call before TickAndUploadVideos so video nodes see updated Time values.
        void ResolveSourceBindings(Graph::EffectGraph& graph);

        // Lane-3 wrapper for a producer node, or nullptr if that node is not
        // publishing one. Populated during ProcessDeferredCompute; consumed by
        // the input-wiring step that attaches it to a pixel-shader effect.
        ID2D1Bitmap1* AnalysisBitmap(uint32_t producerNodeId) const
        {
            auto it = m_analysisBitmapCache.find(producerNodeId);
            return it == m_analysisBitmapCache.end() ? nullptr : it->second.get();
        }

        // Release all cached D2D effects (e.g., on device lost or graph clear).
        // The graph-aware overload also clears every node's non-owning cachedOutput
        // pointer so the render path can't dereference a freed effect.
        void ReleaseCache();
        void ReleaseCache(Graph::EffectGraph& graph);

        // Invalidate the cached effect for a specific node (e.g., CLSID changed).
        // The graph-aware overload also clears node.cachedOutput.
        void InvalidateNode(uint32_t nodeId);
        void InvalidateNode(Graph::EffectGraph& graph, uint32_t nodeId);

        // Update an existing cached effect's shader bytecode in-place (for recompile).
        // If the effect isn't cached yet, does nothing (next Evaluate will create it).
        void UpdateNodeShader(uint32_t nodeId, const Graph::EffectNode& node);

        // Phase 8c: host hint set for which nodes need their analysis
        // values on the CPU. UI integration: MainWindow updates this
        // each frame with the currently-selected node id (so its
        // Properties-panel display stays live) plus any node whose
        // values an MCP route is about to read. Default empty: when
        // empty AND the skip-readback flag is on, only nodes detected
        // as feeding a non-GPU-served binding will get readback.
        // When the skip-readback flag is off (the default), this set
        // is ignored and every compute dispatch reads back to CPU.
        //
        // Throttling: nodes that are newly added to interest get an
        // immediate readback; subsequent frames are throttled to
        // Performance::CpuAnalysisHintThrottleMs (default 2000 ms,
        // 0.5 Hz). Re-selecting a node after it left interest treats
        // it as fresh.
        // Clean-subgraph caching telemetry (see UpdateEffectCachePolicy).
        size_t   CachedEffectCount() const
        {
            size_t n = 0;
            for (const auto& [id, on] : m_cacheEnabled) if (on) ++n;
            return n;
        }
        uint64_t CacheInvalidations() const { return m_cacheInvalidations; }

        void SetCpuAnalysisInterest(std::unordered_set<uint32_t> ids)
        {
            // Drop the throttle timestamp for any node that left the
            // interest set so a subsequent re-selection reads back
            // immediately rather than waiting for the throttle window.
            for (auto it = m_lastHintReadbackTime.begin();
                 it != m_lastHintReadbackTime.end();)
            {
                if (!ids.count(it->first)) it = m_lastHintReadbackTime.erase(it);
                else ++it;
            }
            m_cpuAnalysisInterest = std::move(ids);
        }
        void AddCpuAnalysisInterest(uint32_t id)
        {
            m_cpuAnalysisInterest.insert(id);
        }
        void ClearCpuAnalysisInterest()
        {
            m_cpuAnalysisInterest.clear();
            m_lastHintReadbackTime.clear();
        }

    private:
        // Create or retrieve the cached D2D effect for a built-in effect node.
        ID2D1Effect* GetOrCreateEffect(
            ID2D1DeviceContext5* dc,
            const Graph::EffectNode& node);

        // Apply the node's property map to its D2D effect.
        // Uses effective properties (authored + binding overrides).
        void ApplyProperties(
            ID2D1Effect* effect,
            const Graph::EffectNode& node,
            const std::map<std::wstring, Graph::PropertyValue>& effectiveProps);

        // Resolve property bindings: build effective properties map from
        // authored defaults + upstream analysis output values.
        // Returns true if any binding produced a new value (node should be dirtied).
        bool ResolveBindings(
            Graph::EffectNode& node,
            const Graph::EffectGraph& graph,
            std::map<std::wstring, Graph::PropertyValue>& effectiveProps);

        // Wire input edges: for each input pin on destNode, find the upstream
        // node's cachedOutput and call effect->SetInput(pin, image).
        void WireInputs(
            ID2D1Effect* effect,
            const Graph::EffectNode& destNode,
            const Graph::EffectGraph& graph);

        // Per-node effect cache: nodeId → D2D effect.
        // Effects are reused across frames; only properties are updated.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Effect>> m_effectCache;

        // Clean-subgraph output caching (D2D1_PROPERTY_CACHED).
        //
        // Enables D2D's per-effect output cache on every effect-backed
        // node so that pulling the terminal image does NOT re-execute
        // pixel passes whose subtree is unchanged. D2D auto-invalidates
        // on property/topology changes it can see, but our two hidden
        // mutation channels — custom-effect cbuffer uploads (bypass the
        // D2D property system entirely) and in-place texture updates
        // (compute re-dispatch, video / live-capture uploads) — need the
        // manual invalidation in UpdateEffectCachePolicy, keyed off the
        // same wasDirty signal that gates property re-apply.
        //
        // m_cacheEnabled tracks the last state WE set so SetValue only
        // fires on transitions (a redundant SetValue may itself count as
        // a property change and drop the cache).
        void UpdateEffectCachePolicy(ID2D1Effect* effect, uint32_t nodeId,
                                     bool contentDirty);
        std::unordered_map<uint32_t, bool> m_cacheEnabled;
        uint64_t m_cacheInvalidations{ 0 };

        // Compute nodes queued for dispatch in the current eval cycle.
        // Used to chain GPU-binding freshness: a compute consumer whose
        // binding SOURCE is queued must redispatch too (its dispatch
        // reads the source's analysis SRV). Mirrors m_deferredCompute's
        // lifetime — cleared where it is cleared.
        std::unordered_set<uint32_t> m_queuedComputeThisEval;

        // Per-node owning reference to each effect's output image.
        // ID2D1Effect::GetOutput() returns an AddRef'd pointer; if we only stash
        // the raw pointer in EffectNode::cachedOutput, the local com_ptr releases
        // its ref at scope-exit. The image then survives only by whatever ref
        // the effect holds internally -- and D2D will release/recreate the
        // output proxy on operations like SetInput-toggle, leaving cachedOutput
        // dangling until the next GetOutput. We hold an owning ref here so the
        // image lives as long as the effect does. The D2D debug layer
        // (d2d1debug3.dll) flags use-after-release otherwise.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Image>> m_outputCache;

        // Per-node custom effect impl cache for host-side API access.
        struct CustomEffectEntry
        {
            Effects::CustomPixelShaderEffect* pixelImpl{ nullptr };
            Effects::CustomComputeShaderEffect* computeImpl{ nullptr };
        };
        std::unordered_map<uint32_t, CustomEffectEntry> m_customImplCache;

        // Phase 8: per-node CustomComputeBridgeEffect impl cache.
        // Populated when CreateOrGetEffect creates a bridge for a
        // D3D11ComputeShader node. ProcessDeferredCompute looks up the
        // bridge here and calls ICustomComputeBridge::Dispatch.
        // Bridge lifetime is owned by m_effectCache (the ID2D1Effect
        // outer); this raw pointer is valid as long as that entry is.
        std::unordered_map<uint32_t, Effects::CustomComputeBridgeEffect*> m_bridgeImplCache;

        // Apply bytecode and cbuffer to a custom effect node.
        void ApplyCustomEffect(
            ID2D1Effect* effect,
            Graph::EffectNode& node,
            const std::map<std::wstring, Graph::PropertyValue>& effectiveProps,
            const Graph::EffectGraph& graph,
            ID2D1DeviceContext5* dc);

        // 1x1 placeholder for reserved analysis pins with no binding this
        // frame. Direct2D will NOT render a custom effect that has an
        // unconnected input -- it produces an empty output rect, with no error
        // anywhere -- so a reserved pin cannot simply be left null. Excluded
        // from output bounds by CustomPixelShaderEffect::ImageInputCount.
        winrt::com_ptr<ID2D1Bitmap1> m_analysisDummyBitmap;
        void EnsureAnalysisDummy(ID2D1DeviceContext5* dc);

        // Last lane-3 mode bitset loaded per node. D2D ignores LoadPixelShader
        // for a GUID it already holds, so switching a parameter between
        // cbuffer and texture mode needs both a distinct GUID and a forced
        // reload; this is how the reload is detected.
        std::unordered_map<uint32_t, uint32_t> m_pixelGpuModeBits;
        GpuTimer* m_gpuTimer{ nullptr };

        // Nodes whose output changed during the current Evaluate pass, so a
        // node visited later can tell whether anything it reads has moved.
        // Rebuilt every pass; see the propagation comment in Evaluate for why
        // this is pulled at visit time rather than pushed up front.
        std::unordered_set<uint32_t> m_outputChangedThisEval;

        // Counted per ProcessDeferredCompute, read by the host's perf UI.
        uint32_t m_dispatchesLastFrame{ 0 };

        // Force D2D to compute the histogram and read output data.
        void ReadHistogramOutput(
            ID2D1DeviceContext5* dc,
            ID2D1Effect* effect,
            Graph::EffectNode& node);

        // Read back key-value analysis data from custom compute effect output pixels.
        void ReadCustomAnalysisOutput(
            ID2D1DeviceContext5* dc,
            Graph::EffectNode& node);

        // Temp targets for forcing effect computation. Separate per reader:
        // the histogram wants BGRA and the custom-analysis readback FP32, and
        // one shared target was destroyed and recreated on every call in a
        // graph holding both kinds.
        winrt::com_ptr<ID2D1Bitmap1> m_histogramTarget;
        uint32_t m_histogramTargetW{ 0 };
        uint32_t m_histogramTargetH{ 0 };
        winrt::com_ptr<ID2D1Bitmap1> m_analysisTarget;
        uint32_t m_analysisTargetW{ 0 };
        uint32_t m_analysisTargetH{ 0 };
        winrt::com_ptr<ID2D1Bitmap1> m_analysisCpuBitmap;
        uint32_t m_analysisCpuBitmapW{ 0 };

        // Tracks nodes whose D2D effects were created this frame.
        // Analysis readback is deferred by one frame for these nodes.
        std::unordered_set<uint32_t> m_justCreated;

        // Dummy 1x1 bitmap used as input for zero-input source effects.
        // D2D custom pixel shaders require at least 1 input for sizing,
        // but source effects generate their own content.
        winrt::com_ptr<ID2D1Bitmap1> m_dummySourceBitmap;
        void EnsureDummySourceBitmap(ID2D1DeviceContext5* dc);

        // Phase 8 unified bridge dispatch. Replaces the pre-Phase-8
        // DispatchUserD3D11Compute (analysis-only) and DispatchImageCompute
        // (image-producing) paths -- both now route through
        // CustomComputeBridgeEffect::Dispatch.
        void DispatchViaBridge(
            ID2D1DeviceContext5* dc,
            const Graph::EffectGraph& graph,
            Graph::EffectNode& node,
            const std::vector<ID2D1Image*>& inputImages,
            const std::vector<ID2D1Bitmap1*>& preRenderedInputs,
            Effects::CustomComputeBridgeEffect* bridge,
            bool readbackToCpu);

        // Phase 8c skip-readback predicate. Returns true iff the binding
        // (consumer node `consumer`, parameter `paramName`, source pin
        // described by the binding's first ComponentSource) will be
        // served entirely via the GPU SRV path during DispatchViaBridge.
        // The predicate is conservative: anything that would cause the
        // bindingPlan to be cleared inside DispatchViaBridge (variant
        // bytecode missing, multi-component sources, gpuBindable=false,
        // bridge missing, SRV unavailable) returns false here so the
        // pre-pass falls back to "needs CPU readback" for the source.
        // Which of the three publication lanes could carry this binding.
        // Mirrors the _SLPARAM_<name>_GPU values the shader is compiled
        // against, so the plan and the bytecode cannot disagree about what a
        // parameter is reading from.
        enum class GpuBindMode : uint32_t
        {
            None = 0,             // cbuffer: the value arrives via CPU readback
            StructuredBuffer = 1, // lane 1: consumer is a D3D11 compute effect
            Texture = 2,          // lane 3: consumer is a Direct2D pixel shader
        };

        // ---- lane-3 pin layout (pixel-shader consumers) -------------------
        //
        // Direct2D maps a transform's INPUTS to t0, t1, ... so a pixel shader
        // receiving analysis values as textures needs real input pins for
        // them. The layout is fixed and shared by everything that reasons
        // about it -- registration, creation, wiring and the shader's own
        // SHADERLAB_GPU_BUFFER slot numbers must all agree, and a mismatch
        // shows up as a shader sampling the wrong texture rather than as an
        // error:
        //
        //     pin 0 .. N-1   declared image inputs (inputNames)
        //     pin N + i      gpu-bindable parameter i
        //
        // Pins are reserved for EVERY gpu-bindable parameter whether or not
        // it is bound this frame, because the input count is fixed at effect
        // registration and varying it would mean re-registering the CLSID
        // whenever a binding is added. Unbound pins stay null, which D2D
        // accepts and the shader never samples (it is compiled in cbuffer
        // mode for that parameter). Effects declaring no gpu-bindable
        // parameters are completely unaffected.
        static uint32_t DeclaredInputCount(const Graph::CustomEffectDefinition& def)
        {
            return static_cast<uint32_t>((std::max)(size_t(1), def.inputNames.size()));
        }
        // Declared inputs that are IMAGES -- the ones that decide where the
        // effect draws and that D2D may hand over one tile at a time. The rest
        // of the declared inputs are lookup tables (see
        // CustomEffectDefinition::lookupInputCount), which occupy the pins
        // just before the gpu-bindable ones:
        //
        //     pin 0 .. G-1   geometric image inputs
        //     pin G .. N-1   lookup inputs (read at computed coordinates)
        //     pin N + i      gpu-bindable parameter i
        //
        // At least one geometric input always remains.
        static uint32_t GeometricInputCount(const Graph::CustomEffectDefinition& def)
        {
            const uint32_t declared = DeclaredInputCount(def);
            const uint32_t lookups  = (std::min)(def.lookupInputCount, declared - 1);
            return declared - lookups;
        }
        static uint32_t GpuBindablePinCount(const Graph::CustomEffectDefinition& def)
        {
            uint32_t n = 0;
            for (const auto& p : def.parameters) if (p.gpuBindable) ++n;
            return n;
        }
        // Total D2D input count. Only pixel shaders reserve analysis pins:
        // a compute consumer binds its upstream SRV directly at a t-slot and
        // needs no pin at all.
        static uint32_t TotalInputCount(const Graph::EffectNode& node,
                                        const Graph::CustomEffectDefinition& def)
        {
            uint32_t total = DeclaredInputCount(def);
            if (node.type == Graph::NodeType::PixelShader)
                total += GpuBindablePinCount(def);
            return total;
        }

        GpuBindMode CanServeBindingViaGpu(
            const Graph::EffectNode&         consumer,
            const std::wstring&              paramName,
            const Graph::PropertyBinding&    binding,
            const Graph::EffectGraph&        graph) const;

        // NOTE on "is this binding served on the GPU?": the answer is not a
        // property of the mode alone. StructuredBuffer mode is served as soon
        // as the predicate says so, but Texture mode only engages once the
        // producer has actually published a bitmap -- before that the consumer
        // is compiled in cbuffer mode and still needs the CPU value. The
        // readback pre-pass therefore checks AnalysisBitmap() as well, and
        // must keep agreeing with what ApplyCustomEffect decides. Getting
        // that wrong starves exactly the frames that have no texture yet,
        // which is a silent wrong value rather than a slow one.

        // Lane 3 plumbing. `m_analysisTextureWanted` is the set of producer
        // nodes some pixel-shader consumer needs a texture from this frame;
        // `m_analysisBitmapCache` holds the D2D wrapper around each producer's
        // texture, keyed by producer node id. The wrapper is a view onto the
        // SAME D3D texture the copy pass writes, so it does not need
        // recreating per frame -- only when the texture itself is replaced.
        std::unordered_set<uint32_t> m_analysisTextureWanted;
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Bitmap1>> m_analysisBitmapCache;
        std::unordered_map<uint32_t, ID3D11Texture2D*> m_analysisBitmapSource;

        // Wraps a producer's lane-3 texture as a D2D bitmap, reusing the
        // cached wrapper when the underlying texture has not changed.
        // Returns nullptr if the producer has no lane-3 texture yet.
        ID2D1Bitmap1* EnsureAnalysisBitmap(uint32_t producerNodeId,
                                           ID2D1DeviceContext5* dc);

        // Phase 8c host hint: nodes whose analysis values must be
        // populated on the CPU this frame (currently-selected node, MCP
        // targets, etc). Default empty.
        std::unordered_set<uint32_t> m_cpuAnalysisInterest;

        // Phase 8c hint throttle: per-node timestamp of the last
        // host-hint-driven readback. Used to rate-limit the selected-
        // node / canvas-label readback to ~0.5 Hz (configurable via
        // Performance::CpuAnalysisHintThrottleMs). Newly hinted nodes
        // (added to interest after not being there) get an immediate
        // readback because their entry is missing from this map.
        // Hinted nodes that drop out of interest have their timestamp
        // forgotten so re-selecting them later behaves as a fresh hint.
        // CPU-routed bindings bypass this map entirely -- they always
        // need a fresh value because the consumer reads it directly
        // from `analysisOutput.fields` every frame.
        std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>
            m_lastHintReadbackTime;

        // Persistent FP32 pre-render targets for ProcessDeferredCompute,
        // keyed by the PRODUCING node and its output pin -- (nodeId << 32) |
        // pin -- not by image pointer. A pointer key leaked one full-size
        // FP32 surface (133 MB at 4K) every time an upstream effect was
        // recreated, because the new effect's output has a new address and
        // nothing ever erased the old one. A node key reuses the same
        // allocation across recreation, and entries for nodes that have left
        // the graph are pruned at the end of each ProcessDeferredCompute.
        struct SharedPreRenderEntry
        {
            winrt::com_ptr<ID2D1Bitmap1> bitmap;
            UINT32                       width{ 0 };
            UINT32                       height{ 0 };
        };
        std::unordered_map<uint64_t, SharedPreRenderEntry> m_sharedPreRenderCache;

        // Content-addressed memos for the compute / pixel-shader variant
        // path. Keys are the DXBC checksum of the bytecode (bytes 4..19 of
        // the blob, written by the compiler), so they can never go stale: a
        // recompile produces a new checksum. Before these, every dispatch of
        // a GPU-bound consumer re-canonicalized and re-hashed ~30 KB of HLSL,
        // copied the bytecode three times, and ran D3DReflect.
        using BytecodeId = std::array<uint8_t, 16>;
        static BytecodeId IdOfBytecode(const std::vector<uint8_t>& bytes);
        struct VariantKey
        {
            BytecodeId baseline{};
            uint32_t   bits{ 0 };
            bool operator<(const VariantKey& o) const
            {
                if (baseline != o.baseline) return baseline < o.baseline;
                return bits < o.bits;
            }
        };
        std::map<VariantKey, std::shared_ptr<const std::vector<uint8_t>>> m_variantMemo;
        // Returns the variant bytecode for (def, bits), compiling through the
        // BytecodeCache only on the first request. nullptr if unavailable.
        std::shared_ptr<const std::vector<uint8_t>> GetVariantBytecode(
            const Graph::EffectNode& node, const std::wstring& effectId,
            uint32_t effectVersion, const std::string& target, uint32_t bits);

        struct ReflectedCbVar
        {
            std::wstring               name;
            UINT                       offset{ 0 };
            D3D_SHADER_VARIABLE_TYPE   type{ D3D_SVT_FLOAT };
            UINT                       cols{ 1 };
        };
        struct ReflectedCb
        {
            UINT                        sizeBytes{ 0 };
            std::vector<ReflectedCbVar> vars;
        };
        std::map<BytecodeId, std::shared_ptr<const ReflectedCb>> m_computeReflectMemo;
        std::shared_ptr<const ReflectedCb> ReflectComputeCb(const std::vector<uint8_t>& bytes);
        std::map<BytecodeId, std::shared_ptr<const Effects::ShaderReflectionResult>> m_pixelReflectMemo;

        // Async readback bookkeeping: node ids with a copy in flight.
        std::unordered_set<uint32_t> m_asyncReadbackPending;
        // Unpacks a flat float4 analysis block into node.analysisOutput.
        static void UnpackAnalysisFloats(Graph::EffectNode& node,
                                         const std::vector<float>& floats);
        // Marks every node bound to `producerId`'s analysis fields dirty,
        // plus everything downstream of those consumers by edge.
        static void MarkBindingConsumersDirty(Graph::EffectGraph& graph,
                                              uint32_t producerId);

        // Deferred D3D11 compute dispatches (node ID + upstream image).
        struct DeferredCompute {
            uint32_t nodeId;
            // One entry per input slot (t0..tN-1), in declaration order
            // matching customEffect.inputNames. Multi-input compute
            // shaders (e.g. Delta E Comparator: Reference + Test) drive
            // the bridge with all of them so each input texture binds
            // at its own t-slot.
            //
            // OWNING refs (changed from raw ID2D1Image* in mid-2026 P7):
            // an evaluator pass between Evaluate (which records these)
            // and ProcessDeferredCompute (which reads them) can rebuild
            // an upstream effect, releasing the OLD output image. Holding
            // a com_ptr keeps the image alive for the duration of the
            // deferred entry's lifetime.
            std::vector<winrt::com_ptr<ID2D1Image>>     inputImages;
        };
        std::vector<DeferredCompute> m_deferredCompute;
        bool m_deferredComputeFrozen{ false };
    };
}
