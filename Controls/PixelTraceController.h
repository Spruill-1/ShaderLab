#pragma once

#include "pch.h"
#include "PixelInspectorController.h"
#include "../Graph/EffectGraph.h"

namespace ShaderLab::Controls
{
    // A single node in the pixel trace tree.  Mirrors the DAG structure
    // of the effect graph, rooted at the output node being inspected.
    struct PixelTraceNode
    {
        uint32_t    nodeId{ 0 };
        std::wstring nodeName;
        uint32_t    inputPin{ 0 };      // which pin of the PARENT this feeds into
        std::wstring pinName;            // e.g. "Source", "Destination", "Input 0"
        InspectedPixel pixel;            // color values read at this node
        std::vector<PixelTraceNode> inputs;  // upstream nodes (children in the tree)
    };

    // Walks backward from a target node through the effect graph, reading
    // the pixel value at each node and producing a tree of PixelTraceNode.
    //
    // Unlike PixelInspectorController (which samples one node at a time),
    // this class creates the 1×1 target/readback bitmaps ONCE and reuses
    // them for every node in the recursive walk.
    class PixelTraceController
    {
    public:
        PixelTraceController() = default;

        // Store D3D device for bitmap creation.
        void Initialize(ID3D11Device* device);

        // Build the full trace tree from the target node backward.
        // normalizedX/Y are in [0,1]; imageWidth/Height give the
        // output dimensions used to compute the actual pixel coordinate.
        bool BuildTrace(
            ID2D1DeviceContext* dc,
            const Graph::EffectGraph& graph,
            uint32_t targetNodeId,
            float normalizedX,
            float normalizedY,
            uint32_t imageWidth,
            uint32_t imageHeight);

        // Root of the last successful trace.
        const PixelTraceNode& Root() const { return m_root; }

        // Whether a valid trace exists.
        bool HasTrace() const { return m_hasTrace; }

        // Set tracked position (normalized 0–1).
        void SetTrackPosition(float normX, float normY);
        float TrackNormX() const { return m_trackNormX; }
        float TrackNormY() const { return m_trackNormY; }

        // Rebuild trace at the currently tracked position.
        bool ReTrace(
            ID2D1DeviceContext* dc,
            const Graph::EffectGraph& graph,
            uint32_t targetNodeId,
            uint32_t imageWidth,
            uint32_t imageHeight);

    private:
        // Create the shared 1×1 bitmaps if they don't exist yet.
        bool EnsureBitmaps(ID2D1DeviceContext* dc);

        // Read a single pixel from an image into an InspectedPixel.
        bool ReadPixel(
            ID2D1DeviceContext* dc,
            ID2D1Image* image,
            uint32_t pixelX,
            uint32_t pixelY,
            InspectedPixel& out);

        // Recursive trace builder.
        PixelTraceNode BuildTraceNode(
            ID2D1DeviceContext* dc,
            const Graph::EffectGraph& graph,
            uint32_t nodeId,
            uint32_t pixelX,
            uint32_t pixelY);

        // Color-space conversion helpers (duplicated from PixelInspectorController
        // to keep this controller fully self-contained).
        static uint8_t LinearToSRGB(float linear);
        static float   LinearToPQ(float linear);
        static float   ComputeLuminance(float r, float g, float b);

        winrt::com_ptr<ID3D11Device> m_device;

        // Reusable 1×1 bitmaps for the trace walk.
        winrt::com_ptr<ID2D1Bitmap1> m_targetBitmap;
        winrt::com_ptr<ID2D1Bitmap1> m_readbackBitmap;

        PixelTraceNode m_root;
        bool m_hasTrace{ false };

        float m_trackNormX{ 0.0f };
        float m_trackNormY{ 0.0f };
    };
}
