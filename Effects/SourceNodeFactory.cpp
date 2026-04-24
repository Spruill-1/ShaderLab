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
        node.properties[L"IsPlaying"] = true;
        node.properties[L"PlaybackSpeed"] = 1.0f;
        node.properties[L"Loop"] = true;

        node.outputPins.push_back({ L"Frame", 0 });
        return node;
    }

    // -----------------------------------------------------------------------
    // Source preparation (needs D2D device context)
    // -----------------------------------------------------------------------

    void SourceNodeFactory::PrepareSourceNode(
        EffectNode& node,
        ID2D1DeviceContext5* dc,
        double deltaSeconds)
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

                // If the path changed, re-open.
                if (provider && provider->IsOpen())
                {
                    // Keep existing provider.
                }

                if (!provider)
                {
                    provider = std::make_unique<VideoSourceProvider>();
                    OutputDebugStringW(std::format(L"[VideoSource] Opening: {}\n", videoPath).c_str());
                    if (!provider->Open(videoPath, dc))
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

                // Sync playback state from properties.
                auto playIt = node.properties.find(L"IsPlaying");
                if (playIt != node.properties.end())
                {
                    auto* pv = std::get_if<bool>(&playIt->second);
                    if (pv)
                    {
                        if (*pv && !provider->IsPlaying()) provider->Play();
                        else if (!*pv && provider->IsPlaying()) provider->Pause();
                    }
                }

                auto speedIt = node.properties.find(L"PlaybackSpeed");
                if (speedIt != node.properties.end())
                {
                    auto* sv = std::get_if<float>(&speedIt->second);
                    if (sv) provider->SetSpeed(*sv);
                }

                auto loopIt = node.properties.find(L"Loop");
                if (loopIt != node.properties.end())
                {
                    auto* lv = std::get_if<bool>(&loopIt->second);
                    if (lv) provider->SetLoop(*lv);
                }

                // Advance playback clock (cheap — just updates timer).
                provider->Tick(deltaSeconds);

                // Upload latest decoded frame to D2D bitmap (just a GPU memcpy).
                provider->UploadIfReady(dc);

                node.cachedOutput = provider->CurrentBitmap();
                if (provider->IsPlaying())
                    node.dirty = true;
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

    VideoSourceProvider* SourceNodeFactory::GetVideoProvider(uint32_t nodeId)
    {
        auto it = m_videoCache.find(nodeId);
        return (it != m_videoCache.end()) ? it->second.get() : nullptr;
    }
}
