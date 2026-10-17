#include "pch_engine.h"
#include "SourceNodeFactory.h"
#include "../Graph/SourceKind.h"

#include <cassert>

using namespace ShaderLab::Graph;
namespace Numerics = winrt::Windows::Foundation::Numerics;

namespace ShaderLab::Effects
{
    namespace
    {
        constexpr std::wstring_view cCaptureLostPrefix = L"Capture lost, retrying: ";
        constexpr std::wstring_view cDxgiOpenFailedPrefix = L"Failed to open DXGI output capture: ";
        constexpr std::wstring_view cWgcOpenFailedPrefix = L"Failed to open Windows Graphics Capture source: ";
        constexpr std::wstring_view cVideoUploadErrorPrefix = L"Video upload failed: ";
        constexpr auto cCaptureRetryInterval = std::chrono::seconds(1);

        void ReportVideoUploadFailure(Graph::EffectNode* node, uint32_t nodeId, const std::wstring& reason)
        {
            std::fwprintf(stderr, L"[ShaderLab] node %u video upload failed: %s\n", nodeId, reason.c_str());
            if (node)
                node->runtimeError = std::wstring(cVideoUploadErrorPrefix) + reason;
        }

        double VideoTimeProperty(const Graph::EffectNode& node)
        {
            auto it = node.properties.find(L"Time");
            if (it != node.properties.end())
                if (auto* f = std::get_if<float>(&it->second))
                    return static_cast<double>(*f);
            return 0.0;
        }

        // Replaces the node's analysis fields with `names`, all Float, set to
        // `values` (zero where `values` is shorter).
        void WriteFloatFields(Graph::EffectNode& node, std::span<const wchar_t* const> names,
                              std::initializer_list<double> values = {})
        {
            node.analysisOutput.type = Graph::AnalysisOutputType::Typed;
            node.analysisOutput.fields.clear();
            node.analysisOutput.fields.reserve(names.size());
            auto value = values.begin();
            for (const wchar_t* name : names)
            {
                Graph::AnalysisFieldValue field;
                field.name = name;
                field.type = Graph::AnalysisFieldType::Float;
                if (value != values.end())
                    field.components[0] = static_cast<float>(*value++);
                node.analysisOutput.fields.push_back(std::move(field));
            }
        }

        void WriteVideoFields(Graph::EffectNode& node, const VideoSourceProvider& provider, double position)
        {
            WriteFloatFields(node, SourceNodeFactory::scVideoFields, {
                provider.Duration(),
                position,
                static_cast<double>(provider.DecodeCount()),
                static_cast<double>(provider.UploadSuccesses()),
                static_cast<double>(provider.UploadAttempts()),
                provider.UploadedFrameTime(),   // the frame cachedOutput holds
                static_cast<double>(static_cast<int>(provider.GetOutputFormat())),   // 0 RGB32, 1 NV12, 2 P010
            });
        }

        template <typename Provider>
        void WriteCaptureFields(Graph::EffectNode& node, const Provider& provider)
        {
            WriteFloatFields(node, SourceNodeFactory::scCaptureFields, {
                static_cast<double>(provider.Width()),
                static_cast<double>(provider.Height()),
                static_cast<double>(provider.FrameCount()),
                IsLinearScRgbFormat(provider.OutputFormat()) ? 1.0 : 0.0,
            });
        }

        bool IsCaptureOpenFailure(const std::wstring& error)
        {
            return error.starts_with(cDxgiOpenFailedPrefix) || error.starts_with(cWgcOpenFailedPrefix);
        }

        // Replaces the reason of an open-failure message with the latest one,
        // keeping its prefix. Any other message is left alone.
        void RefreshCaptureOpenFailure(std::wstring& error, const std::wstring& reason)
        {
            for (std::wstring_view prefix : { cDxgiOpenFailedPrefix, cWgcOpenFailedPrefix })
            {
                if (error.starts_with(prefix))
                {
                    if (!reason.empty())
                        error = std::wstring(prefix) + reason;
                    return;
                }
            }
        }

        // Names the node after what the capture opened on.
        void ApplyCaptureIdentity(Graph::EffectNode& node, const DxgiDuplicationSourceProvider& provider)
        {
            node.properties[L"OutputName"] = provider.OutputName();
        }

        void ApplyCaptureIdentity(Graph::EffectNode& node, const WindowsGraphicsCaptureSourceProvider& provider)
        {
            if (!provider.ItemName().empty())
            {
                node.name = provider.ItemName();
                node.properties[L"CaptureItemName"] = provider.ItemName();
            }
        }

        std::wstring PrepareFailure(std::wstring_view detail)
        {
            std::wstring message = L"Source preparation failed";
            if (!detail.empty())
                message += std::wstring(L": ") + std::wstring(detail);
            return message;
        }

        template <typename T>
        bool ReadProperty(const Graph::EffectNode& node, const wchar_t* name, T& value)
        {
            auto it = node.properties.find(name);
            if (it == node.properties.end()) return false;
            if (auto* v = std::get_if<T>(&it->second)) { value = *v; return true; }
            return false;
        }

        // Nodes by id, built once per tick rather than scanned per provider.
        std::unordered_map<uint32_t, Graph::EffectNode*> NodesById(std::span<Graph::EffectNode> nodes)
        {
            std::unordered_map<uint32_t, Graph::EffectNode*> byId;
            byId.reserve(nodes.size());
            for (auto& node : nodes)
                byId.emplace(node.id, &node);
            return byId;
        }
    }

    std::optional<double> SourceNodeFactory::VideoTargetTime(const Graph::EffectNode& node, double duration)
    {
        double seconds = VideoTimeProperty(node);
        bool loop = true;
        ReadProperty(node, L"Loop", loop);
        if (duration <= 0.0) return (std::max)(seconds, 0.0);
        if (loop)
        {
            seconds = std::fmod(seconds, duration);
            if (seconds < 0.0) seconds += duration;
            return seconds;
        }
        if (seconds >= duration) return std::nullopt;
        return (std::max)(seconds, 0.0);
    }

    SourceNodeFactory::SourceNodeFactory() = default;

    void SourceNodeFactory::SetOwnerThread(std::thread::id owner)
    {
        m_ownerThread.store(owner, std::memory_order_release);
    }

    void SourceNodeFactory::AssertOwnerThread() const
    {
#ifndef NDEBUG
        const std::thread::id owner = m_ownerThread.load(std::memory_order_acquire);
        assert(owner == std::thread::id{} || owner == std::this_thread::get_id());
#endif
    }

    // -----------------------------------------------------------------------
    // Video file detection
    // -----------------------------------------------------------------------

    std::span<const std::wstring_view> SourceNodeFactory::VideoExtensions()
    {
        static constexpr std::wstring_view scExtensions[] = {
            L".mp4", L".mkv", L".mov", L".avi", L".wmv", L".webm",
            L".m4v", L".ts", L".mts", L".flv", L".3gp"
        };
        return scExtensions;
    }

    bool SourceNodeFactory::IsVideoFile(const std::wstring& filePath)
    {
        auto ext = std::filesystem::path(filePath).extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        const auto extensions = VideoExtensions();
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
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
        node.mediaPath = filePath;

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
        node.mediaPath = filePath;

        // Video-specific properties.
        node.properties[cVideoMarker] = true;
        node.properties[L"Loop"] = true;
        node.properties[L"Time"] = 0.0f;  // Clock-driven seek position (bound via data pin)

        node.outputPins.push_back({ L"Frame", 0 });

        // Every field exists before the first frame, so its data pins can
        // be bound at once.
        WriteFloatFields(node, scVideoFields);
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
        node.properties[cDesktopDuplicationMarker] = true;
        node.properties[L"AdapterIndex"] = static_cast<uint32_t>(adapterIndex);
        node.properties[L"OutputIndex"] = static_cast<uint32_t>(outputIndex);
        node.properties[L"RawFP16"] = true;
        node.outputPins.push_back({ L"Frame", 0 });
        WriteFloatFields(node, scCaptureFields);
        return node;
    }

    EffectNode SourceNodeFactory::CreateWindowsGraphicsCaptureSourceNode(
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty() ? L"Windows Graphics Capture" : displayName;
        node.properties[cGraphicsCaptureMarker] = true;
        node.properties[L"RawFP16"] = true;
        node.outputPins.push_back({ L"Frame", 0 });
        WriteFloatFields(node, scCaptureFields);
        return node;
    }

    // -----------------------------------------------------------------------
    // Source preparation (needs D2D device context)
    // -----------------------------------------------------------------------

    void SourceNodeFactory::PrepareSourceNode(
        EffectNode& node,
        ID2D1DeviceContext5* dc,
        ID3D11Device* d3dDevice,
        ID3D11DeviceContext* d3dContext)
    {
        AssertOwnerThread();
        if (!dc || node.type != NodeType::Source)
            return;

        try
        {
            switch (ClassifySource(node))
            {
            case SourceKind::Image:              PrepareImage(node, dc); break;
            case SourceKind::Flood:              PrepareFlood(node, dc); break;
            case SourceKind::Video:              PrepareVideo(node, dc, d3dDevice, d3dContext); break;
            case SourceKind::DesktopDuplication: PrepareDesktopDuplication(node, dc, d3dDevice); break;
            case SourceKind::GraphicsCapture:    PrepareGraphicsCapture(node, dc, d3dDevice); break;
            case SourceKind::None:               break;
            }
            return;
        }
        catch (const winrt::hresult_error& ex)
        {
            node.runtimeError = PrepareFailure(std::wstring_view(ex.message()));
        }
        catch (const std::exception& ex)
        {
            node.runtimeError = PrepareFailure(std::wstring_view(winrt::to_hstring(ex.what())));
        }
        catch (...)
        {
            node.runtimeError = PrepareFailure({});
        }
        // Clean, so a failing source neither stops the others nor keeps the
        // graph re-rendering.
        node.dirty = false;
    }

    void SourceNodeFactory::PrepareDesktopDuplication(EffectNode& node, ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice)
    {
        if (!d3dDevice)
            return;

        uint32_t adapterIndex = 0;
        uint32_t outputIndex = 0;
        bool rawFP16 = true;
        ReadProperty(node, L"AdapterIndex", adapterIndex);
        ReadProperty(node, L"OutputIndex", outputIndex);
        ReadProperty(node, L"RawFP16", rawFP16);

        auto& provider = m_dxgiCaptureCache[node.id];
        if (provider && (provider->AdapterIndex() != adapterIndex || provider->OutputIndex() != outputIndex || provider->RawFP16() != rawFP16))
            provider.reset();
        if (!provider)
        {
            m_captureRetryAt.erase(node.id);
            provider = std::make_unique<DxgiDuplicationSourceProvider>();
            // A failed Open keeps its provider, so the live-capture tick
            // retries it like a lost capture.
            if (!provider->Open(dc, d3dDevice, adapterIndex, outputIndex, rawFP16))
            {
                node.runtimeError = std::wstring(cDxgiOpenFailedPrefix) + provider->LastError();
                node.cachedOutput = nullptr;
                return;
            }
            ApplyCaptureIdentity(node, *provider);
            node.runtimeError.clear();
        }
        node.cachedOutput = provider->CurrentBitmap();
    }

    void SourceNodeFactory::PrepareGraphicsCapture(EffectNode& node, ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice)
    {
        if (!d3dDevice)
            return;

        auto& provider = m_wgcCaptureCache[node.id];
        if (!provider)
        {
            bool rawFP16 = true;
            ReadProperty(node, L"RawFP16", rawFP16);

            auto pendingIt = m_pendingWgcItems.find(node.id);
            if (pendingIt == m_pendingWgcItems.end() || !pendingIt->second.has_value() || !pendingIt->second.value())
            {
                node.runtimeError = L"Choose a window or monitor for this Windows Graphics Capture source.";
                m_wgcCaptureCache.erase(node.id);
                return;
            }

            m_captureRetryAt.erase(node.id);
            provider = std::make_unique<WindowsGraphicsCaptureSourceProvider>();
            if (!provider->Open(dc, d3dDevice, pendingIt->second.value(), rawFP16))
            {
                node.runtimeError = std::wstring(cWgcOpenFailedPrefix) + provider->LastError();
                node.cachedOutput = nullptr;
                return;
            }
            ApplyCaptureIdentity(node, *provider);
            node.runtimeError.clear();
        }
        node.cachedOutput = provider->CurrentBitmap();
    }

    void SourceNodeFactory::PrepareVideo(EffectNode& node, ID2D1DeviceContext5* dc,
                                         ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
    {
        const std::wstring videoPath = node.mediaPath.value_or(L"");
        if (videoPath.empty())
        {
            node.cachedOutput = nullptr;
            return;
        }

        // A node from a saved graph has no fields until now.
        if (node.analysisOutput.fields.empty())
            WriteFloatFields(node, scVideoFields);

        auto& provider = m_videoCache[node.id];

        // A new path re-opens; the old provider's bitmap goes with it.
        if (provider && provider->IsOpen() && provider->FilePath() != videoPath)
        {
            provider.reset();
            node.cachedOutput = nullptr;
        }

        if (!provider)
        {
            provider = std::make_unique<VideoSourceProvider>();
            OutputDebugStringW(std::format(L"[VideoSource] Opening: {}\n", videoPath).c_str());
            if (!provider->Open(videoPath, dc, d3dDevice, d3dContext))
            {
                node.runtimeError = L"Failed to open video: " + provider->LastError();
                node.cachedOutput = nullptr;
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

        bool loop = true;
        if (ReadProperty(node, L"Loop", loop))
            provider->SetLoop(loop);

        // Frames are uploaded by TickAndUploadVideos before evaluation; here
        // the node just points at the current image.
        auto pastEnd = m_videoPastEnd.find(node.id);
        if (pastEnd != m_videoPastEnd.end())
        {
            winrt::com_ptr<ID2D1Image> black;
            pastEnd->second->GetOutput(black.put());
            node.cachedOutput = black.get();
        }
        else
        {
            node.cachedOutput = provider->CurrentBitmap();
        }
    }

    SourceNodeFactory::ImageFileStamp SourceNodeFactory::StampOf(const std::wstring& path)
    {
        ImageFileStamp stamp;
        stamp.path = path;
        std::error_code ec;
        stamp.writeTime = std::filesystem::last_write_time(path, ec);
        if (ec) stamp.writeTime = {};
        stamp.size = std::filesystem::file_size(path, ec);
        if (ec) stamp.size = 0;
        return stamp;
    }

    void SourceNodeFactory::PrepareImage(EffectNode& node, ID2D1DeviceContext5* dc)
    {
        // No file chosen yet: nothing to decode, and nothing wrong.
        if (node.mediaPath->empty())
        {
            node.runtimeError.clear();
            node.cachedOutput = nullptr;
            node.dirty = false;
            return;
        }

        // Decode is keyed on the file, not on node.dirty: MarkAllDirty fires
        // on load, display-profile change and forced redraw, and none of
        // those may re-decode. Re-saving the file does.
        const auto stamp = StampOf(node.mediaPath.value());
        auto it = m_images.find(node.id);
        if (it != m_images.end() && it->second.bitmap && it->second.stamp == stamp)
        {
            node.cachedOutput = it->second.bitmap.get();
            node.dirty = false;
            return;
        }

        auto bitmap = m_imageLoader.LoadFromFile(stamp.path, dc);
        if (bitmap)
        {
            node.cachedOutput = bitmap.get();
            m_images[node.id] = { std::move(bitmap), stamp };
            node.runtimeError.clear();
            node.dirty = false;
        }
        else
        {
            // Not every file a WIC filter offers decodes (camera RAW, a
            // truncated file); runtimeError reaches the Properties panel
            // and graph_get_node.
            node.runtimeError = std::format(L"Failed to decode image: {} ({})", stamp.path, m_imageLoader.LastError());
        }
    }

    void SourceNodeFactory::PrepareFlood(EffectNode& node, ID2D1DeviceContext5* dc)
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
            {
                node.runtimeError = L"Failed to create the Flood effect: " + HResultMessage(hr);
                return;
            }
            m_floodCache[node.id] = flood;
        }

        Numerics::float4 color{};
        if (ReadProperty(node, L"Color", color))
            flood->SetValue(D2D1_FLOOD_PROP_COLOR, D2D1_VECTOR_4F{ color.x, color.y, color.z, color.w });

        winrt::com_ptr<ID2D1Image> output;
        flood->GetOutput(output.put());
        node.cachedOutput = output.get();
        node.dirty = false;
    }

    // -----------------------------------------------------------------------
    // Cache management
    // -----------------------------------------------------------------------

    void SourceNodeFactory::ReleaseCache()
    {
        AssertOwnerThread();
        m_images.clear();
        m_floodCache.clear();
        m_videoCache.clear();
        m_videoPastEnd.clear();
        m_dxgiCaptureCache.clear();
        m_wgcCaptureCache.clear();
        m_captureRetryAt.clear();
        m_videoSettleTimeouts.clear();
    }

    void SourceNodeFactory::PruneOrphans(std::span<const Graph::EffectNode> nodes)
    {
        AssertOwnerThread();
        std::unordered_map<uint32_t, const EffectNode*> sources;
        for (const auto& n : nodes)
            if (n.type == NodeType::Source) sources[n.id] = &n;
        auto isKind = [&](uint32_t id, SourceKind kind)
        {
            auto it = sources.find(id);
            return it != sources.end() && ClassifySource(*it->second) == kind;
        };
        auto isCapture = [&](uint32_t id)
        {
            auto it = sources.find(id);
            return it != sources.end() && IsLiveCapture(ClassifySource(*it->second));
        };
        std::erase_if(m_videoCache,       [&](const auto& kv) { return !isKind(kv.first, SourceKind::Video); });
        std::erase_if(m_dxgiCaptureCache, [&](const auto& kv) { return !isKind(kv.first, SourceKind::DesktopDuplication); });
        std::erase_if(m_wgcCaptureCache,  [&](const auto& kv) { return !isKind(kv.first, SourceKind::GraphicsCapture); });
        std::erase_if(m_captureRetryAt,   [&](const auto& kv) { return !isCapture(kv.first); });
        // Plain per-id state: gone with the node. (m_pendingWgcItems is left
        // alone -- an item can be registered before its node exists.)
        auto gone = [&](const auto& kv) { return !sources.count(kv.first); };
        std::erase_if(m_images, gone);
        std::erase_if(m_floodCache, gone);
        std::erase_if(m_videoPastEnd, [&](const auto& kv) { return !isKind(kv.first, SourceKind::Video); });
    }

    bool SourceNodeFactory::TickAndUploadVideos(
        std::span<Graph::EffectNode> nodes,
        ID2D1DeviceContext5* dc)
    {
        PruneOrphans(nodes);
        bool anyNewFrame = false;
        const auto nodesById = NodesById(nodes);
        for (auto& [id, provider] : m_videoCache)
        {
            if (!provider || !provider->IsOpen()) continue;

            // The node, for its (possibly Clock-driven) Time.
            auto nodeIt = nodesById.find(id);
            Graph::EffectNode* nodePtr = nodeIt == nodesById.end() ? nullptr : nodeIt->second;

            // A failing decoder or upload is reported on its own node and
            // leaves the other videos ticking.
            try
            {
                double seekTime = 0.0;
                if (nodePtr)
                {
                    const auto target = VideoTargetTime(*nodePtr, provider->Duration());
                    if (target)
                        seekTime = *target;

                    // Past end of video with looping off → show black.
                    if (!target)
                    {
                        seekTime = VideoTimeProperty(*nodePtr);
                        auto [pastEnd, entered] = m_videoPastEnd.try_emplace(id);
                        if (entered)
                        {
                            // A null cachedOutput would stop the whole graph from
                            // evaluating, so black is an image: a flood cropped to
                            // the frame.
                            winrt::com_ptr<ID2D1Effect> flood, crop;
                            if (FAILED(dc->CreateEffect(CLSID_D2D1Flood, flood.put())) ||
                                FAILED(dc->CreateEffect(CLSID_D2D1Crop, crop.put())))
                            {
                                m_videoPastEnd.erase(pastEnd);
                                nodePtr->runtimeError = L"Could not create the black past-the-end image";
                                continue;
                            }
                            flood->SetValue(D2D1_FLOOD_PROP_COLOR, D2D1_VECTOR_4F{ 0.0f, 0.0f, 0.0f, 1.0f });
                            crop->SetInputEffect(0, flood.get());
                            crop->SetValue(D2D1_CROP_PROP_RECT, D2D1_VECTOR_4F{ 0.0f, 0.0f,
                                static_cast<float>(provider->FrameWidth()), static_cast<float>(provider->FrameHeight()) });
                            pastEnd->second = crop;
                            nodePtr->dirty = true;
                        }
                        winrt::com_ptr<ID2D1Image> black;
                        pastEnd->second->GetOutput(black.put());
                        nodePtr->cachedOutput = black.get();
                        WriteVideoFields(*nodePtr, *provider, seekTime);
                        continue;
                    }
                }

                // Back from past the end: show the provider's frame again.
                if (m_videoPastEnd.erase(id) > 0 && nodePtr)
                {
                    nodePtr->cachedOutput = provider->CurrentBitmap();
                    nodePtr->dirty = true;
                }

                // Show the frame for Time, whether a Clock drives it or it is a
                // static value. The provider decodes forward (dropping frames a
                // slow tick has passed) or seeks, whichever is cheaper, and holds
                // the frame while Time stands still.
                provider->PlayTo(seekTime);

                if (provider->UploadIfReady(dc))
                {
                    anyNewFrame = true;
                    if (nodePtr)
                    {
                        nodePtr->cachedOutput = provider->CurrentBitmap();
                        nodePtr->dirty = true;
                        if (nodePtr->runtimeError.starts_with(cVideoUploadErrorPrefix))
                            nodePtr->runtimeError.clear();
                    }
                }

                // After the upload, so FrameTime matches cachedOutput.
                if (nodePtr)
                    WriteVideoFields(*nodePtr, *provider, provider->CurrentPosition());
            }
            catch (const winrt::hresult_error& ex)
            {
                ReportVideoUploadFailure(nodePtr, id, std::wstring(ex.message()));
            }
            catch (const std::exception& ex)
            {
                ReportVideoUploadFailure(nodePtr, id, std::wstring(winrt::to_hstring(ex.what())));
            }
        }
        return anyNewFrame;
    }

    bool SourceNodeFactory::TickAndUploadLiveCaptures(
        std::span<Graph::EffectNode> nodes,
        ID2D1DeviceContext5* dc)
    {
        PruneOrphans(nodes);
        bool anyNewFrame = false;
        const auto now = std::chrono::steady_clock::now();

        // Point the node at the provider's image every tick, not only on a
        // new frame: a lost or resized capture releases the image the node
        // still references.
        auto refreshOutput = [](Graph::EffectNode& node, ID2D1Image* current, bool failed, const std::wstring& reason)
        {
            if (node.cachedOutput != current)
            {
                node.cachedOutput = current;
                node.dirty = true;
            }
            // An open failure keeps its own message until a retry succeeds.
            if (failed || !current)
            {
                if (!IsCaptureOpenFailure(node.runtimeError))
                    node.runtimeError = std::wstring(cCaptureLostPrefix) + (reason.empty() ? L"no image" : reason);
            }
            else if (node.runtimeError.starts_with(cCaptureLostPrefix) || IsCaptureOpenFailure(node.runtimeError))
            {
                node.runtimeError.clear();
            }
        };

        // Both provider kinds expose the same capture surface.
        const auto nodesById = NodesById(nodes);
        auto tick = [&](auto& cache)
        {
            for (auto& [id, provider] : cache)
            {
                if (!provider) continue;
                auto nodeIt = nodesById.find(id);
                Graph::EffectNode* nodePtr = nodeIt == nodesById.end() ? nullptr : nodeIt->second;

                // A throwing capture is reported on its own node and leaves
                // the other captures ticking.
                auto reportThrow = [&](std::wstring_view message)
                {
                    if (nodePtr)
                        refreshOutput(*nodePtr, provider->CurrentBitmap(), true, std::wstring(message));
                };
                try
                {
                    CaptureResult result = provider->IsOpen() ? provider->CaptureNextFrame(dc) : CaptureResult::Failed;
                    // A capture with no image is broken even when no frame is
                    // pending, so the retry below still reaches Reopen.
                    if (result == CaptureResult::NoFrame && !provider->CurrentBitmap())
                        result = CaptureResult::Failed;
                    if (result == CaptureResult::Failed)
                    {
                        // Reopen once the retry interval has passed since the failure.
                        auto [retry, first] = m_captureRetryAt.try_emplace(id, now + cCaptureRetryInterval);
                        if (!first && now >= retry->second)
                        {
                            if (provider->Reopen(dc))
                            {
                                m_captureRetryAt.erase(retry);
                                result = CaptureResult::NoFrame;
                                if (nodePtr && IsCaptureOpenFailure(nodePtr->runtimeError))
                                    ApplyCaptureIdentity(*nodePtr, *provider);
                            }
                            else
                            {
                                retry->second = now + cCaptureRetryInterval;
                                if (nodePtr)
                                    RefreshCaptureOpenFailure(nodePtr->runtimeError, provider->LastError());
                            }
                        }
                    }
                    else if (!m_captureRetryAt.empty())
                    {
                        m_captureRetryAt.erase(id);
                    }

                    if (result == CaptureResult::NewFrame)
                    {
                        anyNewFrame = true;
                        if (nodePtr)
                            nodePtr->dirty = true;
                    }
                    if (nodePtr)
                    {
                        const bool failed = result == CaptureResult::Failed;
                        refreshOutput(*nodePtr, provider->CurrentBitmap(), failed,
                                      failed ? provider->LastError() : std::wstring{});
                        if (provider->IsOpen())
                            WriteCaptureFields(*nodePtr, *provider);
                    }
                }
                catch (const winrt::hresult_error& ex)
                {
                    reportThrow(std::wstring_view(ex.message()));
                }
                catch (const std::exception& ex)
                {
                    reportThrow(std::wstring_view(winrt::to_hstring(ex.what())));
                }
            }
        };
        tick(m_dxgiCaptureCache);
        tick(m_wgcCaptureCache);

        return anyNewFrame;
    }

    void SourceNodeFactory::RegisterGraphicsCaptureItem(
        uint32_t nodeId,
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item)
    {
        AssertOwnerThread();
        if (item)
            m_pendingWgcItems[nodeId] = item;
        else
            m_pendingWgcItems.erase(nodeId);
        m_wgcCaptureCache.erase(nodeId);
        m_captureRetryAt.erase(nodeId);
    }

    VideoSourceProvider* SourceNodeFactory::VideoProviderForRenderThread(uint32_t nodeId)
    {
        AssertOwnerThread();
        auto it = m_videoCache.find(nodeId);
        return (it != m_videoCache.end()) ? it->second.get() : nullptr;
    }

    bool SourceNodeFactory::HasVideoProvider(uint32_t nodeId) const
    {
        AssertOwnerThread();
        auto it = m_videoCache.find(nodeId);
        return it != m_videoCache.end() && it->second;
    }

    std::vector<Graph::VideoSourceStatus> SourceNodeFactory::VideoSourceStatuses() const
    {
        AssertOwnerThread();
        std::vector<Graph::VideoSourceStatus> statuses;
        statuses.reserve(m_videoCache.size());
        for (const auto& [id, provider] : m_videoCache)
        {
            if (!provider) continue;
            Graph::VideoSourceStatus status;
            status.nodeId = id;
            status.open = provider->IsOpen();
            status.loop = provider->IsLooping();
            status.width = provider->FrameWidth();
            status.height = provider->FrameHeight();
            status.frameRate = provider->FrameRate();
            status.duration = provider->Duration();
            status.position = provider->CurrentPosition();
            status.uploads = provider->UploadSuccesses();
            statuses.push_back(status);
        }
        return statuses;
    }

    bool SourceNodeFactory::VideoSettleTimedOut(uint32_t nodeId, const std::wstring& path, double time) const
    {
        AssertOwnerThread();
        return m_videoSettleTimeouts.count({ nodeId, path, time }) > 0;
    }

    void SourceNodeFactory::RecordVideoSettleTimeout(uint32_t nodeId, const std::wstring& path, double time)
    {
        AssertOwnerThread();
        m_videoSettleTimeouts.insert({ nodeId, path, time });
    }

    void SourceNodeFactory::ForgetVideoSettleTimeouts()
    {
        AssertOwnerThread();
        m_videoSettleTimeouts.clear();
    }
}
