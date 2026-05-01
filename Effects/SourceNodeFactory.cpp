#include "pch.h"
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
                // Only use expensive Seek for actual jumps (backward or large skip).
                if (diff < -frameDur || diff > frameDur * 30.0)
                {
                    provider->Seek(seekTime);
                }
                else
                {
                    // Sequential forward: advance at wall-clock speed.
                    // Tick uses the frame accumulator to request exactly one
                    // decode per video frame duration, giving smooth playback.
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

    VideoSourceProvider* SourceNodeFactory::GetVideoProvider(uint32_t nodeId)
    {
        auto it = m_videoCache.find(nodeId);
        return (it != m_videoCache.end()) ? it->second.get() : nullptr;
    }
}
