#include "pch_engine.h"
#include "SourceNodeFactory.h"

using namespace ShaderLab::Graph;
namespace Numerics = winrt::Windows::Foundation::Numerics;

namespace ShaderLab::Effects
{
    SourceNodeFactory::SourceNodeFactory() = default;

    // -----------------------------------------------------------------------
    // Video file detection
    // -----------------------------------------------------------------------

    bool SourceNodeFactory::IsVideoFile(const std::wstring& filePath)
    {
        static const std::set<std::wstring> videoExts = {
            L".mp4", L".mkv", L".mov", L".avi", L".wmv", L".webm",
            L".m4v", L".ts", L".mts", L".flv", L".3gp"
        };
        auto ext = std::filesystem::path(filePath).extension().wstring();
        // Lowercase.
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        return videoExts.count(ext) > 0;
    }

    // -----------------------------------------------------------------------
    // Node creation (static — no device needed yet)
    // -----------------------------------------------------------------------

    EffectNode SourceNodeFactory::CreateImageSourceNode(
        const std::wstring& filePath,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty()
            ? std::filesystem::path(filePath).filename().wstring()
            : displayName;
        node.shaderPath = filePath;

        node.outputPins.push_back({ L"Image", 0 });
        return node;
    }

    EffectNode SourceNodeFactory::CreateFloodSourceNode(
        const Numerics::float4& color,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty() ? L"Flood Fill" : displayName;
        node.effectClsid = CLSID_D2D1Flood;
        node.properties[L"Color"] = color;
        node.outputPins.push_back({ L"Output", 0 });
        return node;
    }

    EffectNode SourceNodeFactory::CreateVideoSourceNode(
        const std::wstring& filePath,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty()
            ? (L"\U0001F3AC " + std::filesystem::path(filePath).filename().wstring())
            : displayName;
        node.shaderPath = filePath;

        // Video-specific properties.
        node.properties[L"IsVideo"] = true;
        node.properties[L"Loop"] = true;
        node.properties[L"Time"] = 0.0f;  // Clock-driven seek position (bound via data pin)

        node.outputPins.push_back({ L"Frame", 0 });

        // Pre-populate analysis output so data output pins are visible
        // in the node graph before the first frame is decoded.
        node.analysisOutput.type = AnalysisOutputType::Typed;
        {
            AnalysisFieldValue durFv;
            durFv.name = L"Duration"; durFv.type = AnalysisFieldType::Float;
            node.analysisOutput.fields.push_back(std::move(durFv));
            AnalysisFieldValue posFv;
            posFv.name = L"Position"; posFv.type = AnalysisFieldType::Float;
            node.analysisOutput.fields.push_back(std::move(posFv));
        }

        return node;
    }

    EffectNode SourceNodeFactory::CreateDxgiDuplicateOutputSourceNode(
        uint32_t adapterIndex,
        uint32_t outputIndex,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty()
            ? std::format(L"DXGI Output {}:{}", adapterIndex, outputIndex)
            : displayName;
        node.properties[L"IsDxgiDuplicateOutput"] = true;
        node.properties[L"AdapterIndex"] = static_cast<uint32_t>(adapterIndex);
        node.properties[L"OutputIndex"] = static_cast<uint32_t>(outputIndex);
        node.properties[L"RawFP16"] = true;
        node.outputPins.push_back({ L"Frame", 0 });

        node.analysisOutput.type = AnalysisOutputType::Typed;
        node.analysisOutput.fields.push_back({ L"Width", AnalysisFieldType::Float, {} });
        node.analysisOutput.fields.push_back({ L"Height", AnalysisFieldType::Float, {} });
        node.analysisOutput.fields.push_back({ L"Frames", AnalysisFieldType::Float, {} });
        return node;
    }

    EffectNode SourceNodeFactory::CreateWindowsGraphicsCaptureSourceNode(
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty() ? L"Windows Graphics Capture" : displayName;
        node.properties[L"IsWindowsGraphicsCapture"] = true;
        node.properties[L"RawFP16"] = true;
        node.outputPins.push_back({ L"Frame", 0 });

        node.analysisOutput.type = AnalysisOutputType::Typed;
        node.analysisOutput.fields.push_back({ L"Width", AnalysisFieldType::Float, {} });
        node.analysisOutput.fields.push_back({ L"Height", AnalysisFieldType::Float, {} });
        node.analysisOutput.fields.push_back({ L"Frames", AnalysisFieldType::Float, {} });
        return node;
    }

    // -----------------------------------------------------------------------
    // Source preparation (needs D2D device context)
    // -----------------------------------------------------------------------

    void SourceNodeFactory::PrepareSourceNode(
        EffectNode& node,
        ID2D1DeviceContext5* dc,
        double deltaSeconds,
        ID3D11Device* d3dDevice,
        ID3D11DeviceContext* d3dContext)
    {
        if (!dc || node.type != NodeType::Source)
            return;

        auto isDxgiIt = node.properties.find(L"IsDxgiDuplicateOutput");
        if (isDxgiIt != node.properties.end())
        {
            auto* boolVal = std::get_if<bool>(&isDxgiIt->second);
            if (boolVal && *boolVal)
            {
                if (!d3dDevice)
                    return;

                uint32_t adapterIndex = 0;
                uint32_t outputIndex = 0;
                bool rawFP16 = true;
                if (auto it = node.properties.find(L"AdapterIndex"); it != node.properties.end())
                    if (auto* v = std::get_if<uint32_t>(&it->second)) adapterIndex = *v;
                if (auto it = node.properties.find(L"OutputIndex"); it != node.properties.end())
                    if (auto* v = std::get_if<uint32_t>(&it->second)) outputIndex = *v;
                if (auto it = node.properties.find(L"RawFP16"); it != node.properties.end())
                    if (auto* v = std::get_if<bool>(&it->second)) rawFP16 = *v;

                auto& provider = m_dxgiCaptureCache[node.id];
                if (provider && (provider->AdapterIndex() != adapterIndex || provider->OutputIndex() != outputIndex || provider->RawFP16() != rawFP16))
                    provider.reset();
                if (!provider)
                {
                    provider = std::make_unique<DxgiDuplicationSourceProvider>();
                    if (!provider->Open(dc, d3dDevice, adapterIndex, outputIndex, rawFP16))
                    {
                        node.runtimeError = L"Failed to open DXGI output capture: " + provider->LastError();
                        m_dxgiCaptureCache.erase(node.id);
                        return;
                    }
                    node.properties[L"OutputName"] = provider->OutputName();
                    node.runtimeError.clear();
                }
                node.cachedOutput = provider->CurrentBitmap();
                return;
            }
        }

        auto isWgcIt = node.properties.find(L"IsWindowsGraphicsCapture");
        if (isWgcIt != node.properties.end())
        {
            auto* boolVal = std::get_if<bool>(&isWgcIt->second);
            if (boolVal && *boolVal)
            {
                if (!d3dDevice)
                    return;

                auto& provider = m_wgcCaptureCache[node.id];
                if (!provider)
                {
                    bool rawFP16 = true;
                    if (auto it = node.properties.find(L"RawFP16"); it != node.properties.end())
                        if (auto* v = std::get_if<bool>(&it->second)) rawFP16 = *v;

                    auto pendingIt = m_pendingWgcItems.find(node.id);
                    if (pendingIt == m_pendingWgcItems.end() || !pendingIt->second.has_value() || !pendingIt->second.value())
                    {
                        node.runtimeError = L"Choose a window or monitor for this Windows Graphics Capture source.";
                        return;
                    }

                    provider = std::make_unique<WindowsGraphicsCaptureSourceProvider>();
                    if (!provider->Open(dc, d3dDevice, pendingIt->second.value(), rawFP16))
                    {
                        node.runtimeError = L"Failed to open Windows Graphics Capture source: " + provider->LastError();
                        m_wgcCaptureCache.erase(node.id);
                        return;
                    }
                    if (!provider->ItemName().empty())
                    {
                        node.name = provider->ItemName();
                        node.properties[L"CaptureItemName"] = provider->ItemName();
                    }
                    node.runtimeError.clear();
                }
                node.cachedOutput = provider->CurrentBitmap();
                return;
            }
        }

        // --- Video source ---
        auto isVideoIt = node.properties.find(L"IsVideo");
        if (isVideoIt != node.properties.end())
        {
            auto* boolVal = std::get_if<bool>(&isVideoIt->second);
            if (boolVal && *boolVal)
            {
              try
              {
                // Resolve video file path from node.shaderPath or properties map.
                std::wstring videoPath;
                if (node.shaderPath.has_value() && !node.shaderPath.value().empty())
                    videoPath = node.shaderPath.value();
                // Also check properties (MCP sets path here).
                auto pathIt = node.properties.find(L"shaderPath");
                if (pathIt != node.properties.end())
                {
                    auto* sv = std::get_if<std::wstring>(&pathIt->second);
                    if (sv && !sv->empty())
                    {
                        videoPath = *sv;
                        node.shaderPath = videoPath;
                    }
                }
                if (videoPath.empty())
                    return;

                auto& provider = m_videoCache[node.id];

                // If the path changed, close old provider so it re-opens.
                if (provider && provider->IsOpen() && provider->FilePath() != videoPath)
                {
                    provider.reset();
                }

                if (!provider)
                {
                    provider = std::make_unique<VideoSourceProvider>();
                    OutputDebugStringW(std::format(L"[VideoSource] Opening: {}\n", videoPath).c_str());
                    if (!provider->Open(videoPath, dc, d3dDevice, d3dContext))
                    {
                        node.runtimeError = L"Failed to open video: " + provider->LastError();
                        OutputDebugStringW(std::format(L"[VideoSource] {}\n", node.runtimeError).c_str());
                        m_videoCache.erase(node.id);
                        return;
                    }
                    node.runtimeError.clear();
                    OutputDebugStringW(std::format(L"[VideoSource] Opened: {}x{} @ {:.1f}fps, {:.1f}s, HDR={}\n",
                        provider->FrameWidth(), provider->FrameHeight(),
                        provider->FrameRate(), provider->Duration(),
                        provider->IsHDR()).c_str());
                }

                // Sync loop state from properties.
                auto loopIt = node.properties.find(L"Loop");
                if (loopIt != node.properties.end())
                {
                    auto* lv = std::get_if<bool>(&loopIt->second);
                    if (lv) provider->SetLoop(*lv);
                }

                // Always keep provider in "playing" state so Tick() processes
                // frames. The Clock node drives seeking; m_playing just means
                // the MF reader is in active decode mode.
                if (!provider->IsPlaying())
                    provider->Play();

                // Tick/Upload now handled by TickAndUploadVideos() — called before graph eval.
                // Just ensure cachedOutput is set.
                node.cachedOutput = provider->CurrentBitmap();
              }
              catch (...)
              {
                node.runtimeError = L"Video source exception";
                OutputDebugStringW(L"[VideoSource] Exception in PrepareSourceNode\n");
              }
              return;
            }
        }

        // --- Image source ---
        if (node.shaderPath.has_value() && !node.effectClsid.has_value())
        {
            auto it = m_bitmapCache.find(node.id);
            if (it != m_bitmapCache.end() && !node.dirty)
            {
                node.cachedOutput = it->second.get();
                return;
            }

            auto bitmap = m_imageLoader.LoadFromFile(node.shaderPath.value(), dc);
            if (bitmap)
            {
                node.cachedOutput = bitmap.get();
                m_bitmapCache[node.id] = std::move(bitmap);
                node.dirty = false;
            }
            return;
        }

        // --- Flood source ---
        if (node.effectClsid.has_value() && node.effectClsid.value() == CLSID_D2D1Flood)
        {
            auto it = m_floodCache.find(node.id);
            winrt::com_ptr<ID2D1Effect> flood;

            if (it != m_floodCache.end())
            {
                flood = it->second;
            }
            else
            {
                HRESULT hr = dc->CreateEffect(CLSID_D2D1Flood, flood.put());
                if (FAILED(hr))
                    return;
                m_floodCache[node.id] = flood;
            }

            auto colorIt = node.properties.find(L"Color");
            if (colorIt != node.properties.end())
            {
                auto* colorVal = std::get_if<Numerics::float4>(&colorIt->second);
                if (colorVal)
                {
                    D2D1_VECTOR_4F d2dColor{ colorVal->x, colorVal->y, colorVal->z, colorVal->w };
                    flood->SetValue(D2D1_FLOOD_PROP_COLOR, d2dColor);
                }
            }

            winrt::com_ptr<ID2D1Image> output;
            flood->GetOutput(output.put());
            node.cachedOutput = output.get();
            node.dirty = false;
            return;
        }
    }

    // -----------------------------------------------------------------------
    // Cache management
    // -----------------------------------------------------------------------

    void SourceNodeFactory::ReleaseCache()
    {
        m_bitmapCache.clear();
        m_floodCache.clear();
        m_videoCache.clear();
        m_dxgiCaptureCache.clear();
        m_wgcCaptureCache.clear();
        m_pendingWgcItems.clear();
        m_lastClockTime.clear();
    }

    bool SourceNodeFactory::HasPlayingVideo() const
    {
        for (const auto& [id, provider] : m_videoCache)
        {
            if (provider && provider->IsPlaying())
                return true;
        }
        return false;
    }

    bool SourceNodeFactory::TickAndUploadVideos(
        std::vector<Graph::EffectNode>& nodes,
        ID2D1DeviceContext5* dc,
        double deltaSeconds)
    {
        bool anyNewFrame = false;
        for (auto& [id, provider] : m_videoCache)
        {
            if (!provider || !provider->IsOpen()) continue;

            // Find the corresponding node to check for Clock-driven Time binding.
            Graph::EffectNode* nodePtr = nullptr;
            for (auto& node : nodes)
                if (node.id == id) { nodePtr = &node; break; }

            bool clockDriven = false;
            double seekTime = 0.0;
            if (nodePtr)
            {
                auto timeIt = nodePtr->properties.find(L"Time");
                if (timeIt != nodePtr->properties.end())
                    if (auto* f = std::get_if<float>(&timeIt->second))
                        seekTime = static_cast<double>(*f);

                bool loop = true;
                auto loopIt = nodePtr->properties.find(L"Loop");
                if (loopIt != nodePtr->properties.end())
                    if (auto* b = std::get_if<bool>(&loopIt->second)) loop = *b;

                if (loop && provider->Duration() > 0)
                    seekTime = std::fmod(seekTime, provider->Duration());

                // Past end of video with looping off → show black.
                if (!loop && provider->Duration() > 0 && seekTime >= provider->Duration())
                {
                    nodePtr->cachedOutput = nullptr;
                    clockDriven = true;  // skip tick/seek below
                    // Still update analysis output.
                    nodePtr->analysisOutput.type = Graph::AnalysisOutputType::Typed;
                    nodePtr->analysisOutput.fields.clear();
                    Graph::AnalysisFieldValue durFv;
                    durFv.name = L"Duration"; durFv.type = Graph::AnalysisFieldType::Float;
                    durFv.components[0] = static_cast<float>(provider->Duration());
                    nodePtr->analysisOutput.fields.push_back(std::move(durFv));
                    Graph::AnalysisFieldValue posFv;
                    posFv.name = L"Position"; posFv.type = Graph::AnalysisFieldType::Float;
                    posFv.components[0] = static_cast<float>(seekTime);
                    nodePtr->analysisOutput.fields.push_back(std::move(posFv));
                    continue;
                }

                clockDriven = nodePtr->propertyBindings.count(L"Time") > 0;
            }

            double currentPos = provider->CurrentPosition();
            double diff = seekTime - currentPos;
            double frameDur = 1.0 / (std::max)(provider->FrameRate(), 1.0);

            if (clockDriven)
            {
                // Detect a paused Clock: bound Time hasn't advanced since
                // the last tick. Without this, Tick() free-runs the decoder
                // and the next iteration's Seek() snaps it back, producing
                // a ~1s loop while the user thinks the video is static.
                auto lcIt = m_lastClockTime.find(id);
                bool clockAdvanced = (lcIt == m_lastClockTime.end()) ||
                    std::abs(seekTime - lcIt->second) > 1e-6;
                m_lastClockTime[id] = seekTime;

                if (!clockAdvanced)
                {
                    // Hold the current frame at exactly seekTime.
                    if (std::abs(diff) > frameDur * 0.5)
                        provider->Seek(seekTime);
                }
                // Only seek for actual jumps: backward or very large skip (>5s).
                // Never seek for small forward gaps — sequential decode catches up
                // naturally and avoids expensive keyframe re-decode on scene changes.
                else if (diff < -frameDur || diff > 5.0)
                {
                    provider->Seek(seekTime);
                }
                else
                {
                    provider->Tick(deltaSeconds);
                }
            }
            else
            {
                // No clock: static frame at Time property value.
                // Only seek if position differs from target.
                if (std::abs(diff) > frameDur * 0.5)
                    provider->Seek(seekTime);
            }

            // Always update analysis output (Duration/Position) regardless
            // of whether a new frame was uploaded this tick.
            if (nodePtr)
            {
                nodePtr->analysisOutput.type = Graph::AnalysisOutputType::Typed;
                nodePtr->analysisOutput.fields.clear();
                Graph::AnalysisFieldValue durFv;
                durFv.name = L"Duration"; durFv.type = Graph::AnalysisFieldType::Float;
                durFv.components[0] = static_cast<float>(provider->Duration());
                nodePtr->analysisOutput.fields.push_back(std::move(durFv));
                Graph::AnalysisFieldValue posFv;
                posFv.name = L"Position"; posFv.type = Graph::AnalysisFieldType::Float;
                posFv.components[0] = static_cast<float>(provider->CurrentPosition());
                nodePtr->analysisOutput.fields.push_back(std::move(posFv));
                // Diagnostic counters
                Graph::AnalysisFieldValue decFv;
                decFv.name = L"Decodes"; decFv.type = Graph::AnalysisFieldType::Float;
                decFv.components[0] = static_cast<float>(provider->DecodeCount());
                nodePtr->analysisOutput.fields.push_back(std::move(decFv));
                Graph::AnalysisFieldValue uplFv;
                uplFv.name = L"Uploads"; uplFv.type = Graph::AnalysisFieldType::Float;
                uplFv.components[0] = static_cast<float>(provider->UploadSuccesses());
                nodePtr->analysisOutput.fields.push_back(std::move(uplFv));
                Graph::AnalysisFieldValue attFv;
                attFv.name = L"UploadAttempts"; attFv.type = Graph::AnalysisFieldType::Float;
                attFv.components[0] = static_cast<float>(provider->UploadAttempts());
                nodePtr->analysisOutput.fields.push_back(std::move(attFv));
                // Format: 0=RGB32, 1=NV12, 2=P010
                Graph::AnalysisFieldValue fmtFv;
                fmtFv.name = L"Format"; fmtFv.type = Graph::AnalysisFieldType::Float;
                fmtFv.components[0] = static_cast<float>(static_cast<int>(provider->GetOutputFormat()));
                nodePtr->analysisOutput.fields.push_back(std::move(fmtFv));
            }

            if (provider->UploadIfReady(dc))
            {
                anyNewFrame = true;
                if (nodePtr)
                {
                    nodePtr->cachedOutput = provider->CurrentBitmap();
                    nodePtr->dirty = true;
                }
            }
        }
        return anyNewFrame;
    }

    bool SourceNodeFactory::TickAndUploadLiveCaptures(
        std::vector<Graph::EffectNode>& nodes,
        ID2D1DeviceContext5* dc)
    {
        bool anyNewFrame = false;

        auto updateAnalysis = [](Graph::EffectNode& node, uint32_t width, uint32_t height, uint64_t frames)
        {
            node.analysisOutput.type = Graph::AnalysisOutputType::Typed;
            node.analysisOutput.fields.clear();
            Graph::AnalysisFieldValue widthFv;
            widthFv.name = L"Width"; widthFv.type = Graph::AnalysisFieldType::Float;
            widthFv.components[0] = static_cast<float>(width);
            node.analysisOutput.fields.push_back(std::move(widthFv));
            Graph::AnalysisFieldValue heightFv;
            heightFv.name = L"Height"; heightFv.type = Graph::AnalysisFieldType::Float;
            heightFv.components[0] = static_cast<float>(height);
            node.analysisOutput.fields.push_back(std::move(heightFv));
            Graph::AnalysisFieldValue framesFv;
            framesFv.name = L"Frames"; framesFv.type = Graph::AnalysisFieldType::Float;
            framesFv.components[0] = static_cast<float>(frames);
            node.analysisOutput.fields.push_back(std::move(framesFv));
        };

        for (auto& [id, provider] : m_dxgiCaptureCache)
        {
            if (!provider || !provider->IsOpen()) continue;

            Graph::EffectNode* nodePtr = nullptr;
            for (auto& node : nodes)
                if (node.id == id) { nodePtr = &node; break; }

            if (provider->CaptureNextFrame(dc))
            {
                anyNewFrame = true;
                if (nodePtr)
                {
                    nodePtr->cachedOutput = provider->CurrentBitmap();
                    nodePtr->dirty = true;
                }
            }

            if (nodePtr)
                updateAnalysis(*nodePtr, provider->Width(), provider->Height(), provider->FrameCount());
        }

        for (auto& [id, provider] : m_wgcCaptureCache)
        {
            if (!provider || !provider->IsOpen()) continue;

            Graph::EffectNode* nodePtr = nullptr;
            for (auto& node : nodes)
                if (node.id == id) { nodePtr = &node; break; }

            if (provider->CaptureNextFrame(dc))
            {
                anyNewFrame = true;
                if (nodePtr)
                {
                    nodePtr->cachedOutput = provider->CurrentBitmap();
                    nodePtr->dirty = true;
                }
            }

            if (nodePtr)
                updateAnalysis(*nodePtr, provider->Width(), provider->Height(), provider->FrameCount());
        }

        return anyNewFrame;
    }

    void SourceNodeFactory::RegisterGraphicsCaptureItem(
        uint32_t nodeId,
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item)
    {
        if (item)
        {
            m_pendingWgcItems[nodeId] = item;
            m_wgcCaptureCache.erase(nodeId);
        }
        else
        {
            m_pendingWgcItems.erase(nodeId);
            m_wgcCaptureCache.erase(nodeId);
        }
    }

    VideoSourceProvider* SourceNodeFactory::GetVideoProvider(uint32_t nodeId)
    {
        auto it = m_videoCache.find(nodeId);
        return (it != m_videoCache.end()) ? it->second.get() : nullptr;
    }

    uint64_t SourceNodeFactory::TotalVideoUploads() const
    {
        uint64_t total = 0;
        for (const auto& [id, provider] : m_videoCache)
            if (provider) total += provider->UploadSuccesses();
        return total;
    }
}
