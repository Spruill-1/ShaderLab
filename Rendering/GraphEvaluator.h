#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/EffectGraph.h"
#include "../Effects/CustomPixelShaderEffect.h"
#include "../Effects/CustomComputeShaderEffect.h"
#include "../Effects/ShaderCompiler.h"
#include "GpuReduction.h"
#include "D3D11ComputeRunner.h"

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

        // Walk the graph in topological order and produce the final output image.
        // Returns nullptr if the graph is empty or has no Output node.
        ID2D1Image* Evaluate(Graph::EffectGraph& graph, ID2D1DeviceContext5* dc);

        // Run deferred D3D11 compute dispatches for statistics nodes.
        // Call AFTER Evaluate (and double-eval) when all D2D effects are initialized.
        // Returns true if any compute dispatches ran (analysis output changed).
        bool ProcessDeferredCompute(Graph::EffectGraph& graph, ID2D1DeviceContext5* dc);
        size_t DeferredComputeCount() const { return m_deferredCompute.size(); }

        // Resolve property bindings for Source nodes only (lightweight, no D2D).
        // Call before TickAndUploadVideos so video nodes see updated Time values.
        void ResolveSourceBindings(Graph::EffectGraph& graph);

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

        // Compute statistics for an arbitrary D2D image without mutating any
        // graph node.  Pre-renders the image to a fresh FP32 GPU bitmap (reusing
        // PreRenderInputBitmap) and runs a separate GpuReduction pass per
        // requested channel.
        //
        // channels: list of GpuReduction channel codes
        //   0 = luminance (Rec.709), 1 = R, 2 = G, 3 = B, 4 = A.
        // nonzeroOnly: if true, samples with all-zero RGB are excluded (matches
        //              the per-node analysis behaviour).
        //
        // Returns one ImageStats per channel in the order requested.  Returns an
        // empty vector if pre-render fails or any channel reduction fails.
        std::vector<ImageStats> ComputeStandaloneStats(
            ID2D1DeviceContext5* dc,
            ID2D1Image* inputImage,
            const std::vector<uint32_t>& channels,
            bool nonzeroOnly);

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

        // Apply bytecode and cbuffer to a custom effect node.
        void ApplyCustomEffect(
            ID2D1Effect* effect,
            Graph::EffectNode& node,
            const std::map<std::wstring, Graph::PropertyValue>& effectiveProps);

        // Force D2D to compute the histogram and read output data.
        void ReadHistogramOutput(
            ID2D1DeviceContext5* dc,
            ID2D1Effect* effect,
            Graph::EffectNode& node);

        // Read back key-value analysis data from custom compute effect output pixels.
        void ReadCustomAnalysisOutput(
            ID2D1DeviceContext5* dc,
            Graph::EffectNode& node);

        // GPU-accelerated image statistics via D3D11 compute shader reduction.
        void ComputeImageStatistics(
            ID2D1DeviceContext5* dc,
            Graph::EffectNode& node,
            ID2D1Image* inputImage);

        // Temp target for forcing effect computation.
        winrt::com_ptr<ID2D1Bitmap1> m_analysisTarget;
        uint32_t m_analysisTargetW{ 0 };
        uint32_t m_analysisTargetH{ 0 };
        DXGI_FORMAT m_analysisTargetFormat{ DXGI_FORMAT_UNKNOWN };

        // Tracks nodes whose D2D effects were created this frame.
        // Analysis readback is deferred by one frame for these nodes.
        std::unordered_set<uint32_t> m_justCreated;

        // Dummy 1x1 bitmap used as input for zero-input source effects.
        // D2D custom pixel shaders require at least 1 input for sizing,
        // but source effects generate their own content.
        winrt::com_ptr<ID2D1Bitmap1> m_dummySourceBitmap;
        void EnsureDummySourceBitmap(ID2D1DeviceContext5* dc);

        // GPU compute shader reduction for image statistics.
        GpuReduction m_gpuReduction;

        // Per-node D3D11 compute runners for user-authored D3D11 compute effects.
        std::unordered_map<uint32_t, std::unique_ptr<D3D11ComputeRunner>> m_d3d11RunnerCache;

        // Cached output bitmaps for image-producing compute effects.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Bitmap1>> m_imageComputeCache;
        std::unordered_map<uint32_t, winrt::com_ptr<ID3D11Texture2D>> m_imageComputeTexCache;

        // Generic D3D11 compute dispatch for user-authored shaders (analysis-only).
        void DispatchUserD3D11Compute(
            ID2D1DeviceContext5* dc,
            Graph::EffectNode& node,
            ID2D1Image* inputImage);

        // D3D11 compute dispatch that produces an image output (not analysis data).
        // Creates an output texture, dispatches compute, wraps as D2D bitmap → cachedOutput.
        void DispatchImageCompute(
            ID2D1DeviceContext5* dc,
            Graph::EffectNode& node,
            ID2D1Image* inputImage,
            ID2D1Bitmap1* preRenderedInput = nullptr);

        // Deferred D3D11 compute dispatches (node ID + upstream image).
        struct DeferredCompute {
            uint32_t nodeId;
            ID2D1Image* inputImage;  // non-owning, valid until next Evaluate
            winrt::com_ptr<ID2D1Bitmap1> preRenderedInput;  // owning, pre-rendered bitmap (optional)
        };
        std::vector<DeferredCompute> m_deferredCompute;

        // Pre-render a D2D image to an FP32 bitmap at 96 DPI.
        winrt::com_ptr<ID2D1Bitmap1> PreRenderInputBitmap(
            ID2D1DeviceContext5* dc, ID2D1Image* inputImage);
    };
}
