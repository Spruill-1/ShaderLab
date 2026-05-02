#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/EffectNode.h"
#include "ImageLoader.h"
#include "VideoSourceProvider.h"

namespace ShaderLab::Effects
{
    // Factory for creating and preparing Source-type graph nodes.
    //
    // Three source types:
    //   1. Image source — loads a file via WIC, stores ID2D1Bitmap1 as cachedOutput.
    //   2. Flood source — creates a D2D1Flood effect that fills with a solid color.
    //   3. Video source — decodes video frames via MF Source Reader.
    //
    // PrepareSourceNode must be called before graph evaluation to ensure
    // the node's cachedOutput is populated for downstream effects.
    class SHADERLAB_API SourceNodeFactory
    {
    public:
        SourceNodeFactory();
        SourceNodeFactory(const SourceNodeFactory&) = delete;
        SourceNodeFactory& operator=(const SourceNodeFactory&) = delete;

        // Create a Source node configured for image file loading.
        static Graph::EffectNode CreateImageSourceNode(
            const std::wstring& filePath,
            const std::wstring& displayName = L"");

        // Create a Source node configured as a Flood (solid color) fill.
        static Graph::EffectNode CreateFloodSourceNode(
            const winrt::Windows::Foundation::Numerics::float4& color,
            const std::wstring& displayName = L"");

        // Create a Source node configured for video file playback.
        static Graph::EffectNode CreateVideoSourceNode(
            const std::wstring& filePath,
            const std::wstring& displayName = L"");

        // Returns true if the file extension indicates a video file.
        static bool IsVideoFile(const std::wstring& filePath);

        // Prepare a source node for evaluation: loads the image, creates
        // the Flood effect, or advances the video frame.
        void PrepareSourceNode(
            Graph::EffectNode& node,
            ID2D1DeviceContext5* dc,
            double deltaSeconds = 0.0,
            ID3D11Device* d3dDevice = nullptr,
            ID3D11DeviceContext* d3dContext = nullptr);

        // Release all cached bitmaps, flood effects, and video providers.
        void ReleaseCache();

        // Check if any video source is currently playing.
        bool HasPlayingVideo() const;

        // Tick all video sources and upload new frames.
        // Marks nodes dirty only when a new frame is actually uploaded.
        // Call BEFORE checking HasDirtyNodes. Returns true if any new frame was uploaded.
        bool TickAndUploadVideos(
            std::vector<Graph::EffectNode>& nodes,
            ID2D1DeviceContext5* dc,
            double deltaSeconds);

        // Get the video provider for a node (for UI controls).
        VideoSourceProvider* GetVideoProvider(uint32_t nodeId);

        // Get total video upload count across all providers.
        uint64_t TotalVideoUploads() const;

    private:
        ImageLoader m_imageLoader;

        // Cached loaded bitmaps: nodeId → bitmap.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Bitmap1>> m_bitmapCache;

        // Cached flood effects: nodeId → flood effect.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Effect>> m_floodCache;

        // Cached video providers: nodeId → video provider.
        std::unordered_map<uint32_t, std::unique_ptr<VideoSourceProvider>> m_videoCache;
    };
}
