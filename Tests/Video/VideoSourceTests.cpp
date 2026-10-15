#include "pch_engine.h"
#include "../TestSupport.h"
#include "Graph/ClockModel.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/VideoExport.h"
#include "Graph/GraphEdits.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// Video sources and video export helpers.
namespace
{
    using namespace ShaderLab::Tests;

    // Video export's pure helpers: Clock time at a frame, the export duration,
    // codec resolution and the ffmpeg arguments.
    SL_TEST_CASE(TestVideoExportHelpers, Tag::Fast | Tag::Video)
    {
        printf("\n=== Video export helpers ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* clockDesc = registry.FindByName(L"Clock");
        if (!clockDesc) { TEST("VideoExport_ClockDescriptor", false); return; }

        ShaderLab::Graph::GraphModel g;
        auto cid = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*clockDesc));
        auto setP = [&](const wchar_t* k, float v) { ShaderLab::Graph::SetNodeProperty(*g.FindNode(cid), k, v); };
        auto timeAt = [&](double t) { ShaderLab::Graph::SetClocksToTime(g, t); return g.FindNode(cid)->clockTime; };

        // Defaults: Start 0, Stop 10, Speed 1, Loop on.
        TEST("VideoExport_ClockLoopWraps", std::abs(timeAt(12.5) - 2.5) < 1e-9);
        setP(L"Loop", 0.0f);
        TEST("VideoExport_ClockNoLoopClamps", std::abs(timeAt(12.0) - 10.0) < 1e-9);
        setP(L"Loop", 1.0f); setP(L"Speed", 2.0f);
        TEST("VideoExport_ClockSpeedScales", std::abs(timeAt(3.0) - 6.0) < 1e-9 && std::abs(timeAt(6.0) - 2.0) < 1e-9);
        TEST("VideoExport_LongestClockUsesSpeed", std::abs(ShaderLab::Graph::LongestClockDuration(g) - 5.0) < 1e-9);
        setP(L"Speed", -1.0f);
        TEST("VideoExport_ClockNegativeSpeedLoops", std::abs(timeAt(2.5) - 7.5) < 1e-9);

        // A graph loaded from JSON has working Clocks,
        // with no post-load call: a Clock is one by its saved node kind.
        auto loaded = ShaderLab::Graph::GraphModel::FromJson(g.ToJson());
        bool clockFlagged = false;
        for (const auto& n : loaded.Nodes()) if (n.IsClock()) clockFlagged = true;
        TEST("VideoExport_LoadedClockIsClock", clockFlagged);
        TEST("VideoExport_LoadedClockSteps", ShaderLab::Graph::SetClocksToTime(loaded, 1.0) == 1);

        std::wstring err;
        auto path = ShaderLab::Rendering::FindFfmpeg(L"C:\\definitely\\not\\here\\ffmpeg.exe", err);
        TEST("VideoExport_MissingFfmpegFailsWithInstructions",
             path.empty() && err.find(L"not found") != std::wstring::npos && err.find(L"PATH") != std::wstring::npos);

        // The ffmpeg command line is a pure function of the request.
        using namespace ShaderLab::Rendering;
        auto contains = [](const std::wstring& haystack, const wchar_t* needle) { return haystack.find(needle) != std::wstring::npos; };
        VideoExportRequest hdrRequest;
        hdrRequest.outputPath = L"C:\\out dir\\clip.mp4";
        hdrRequest.fps = 60.0;
        std::wstring codecError;
        const VideoCodec hdrCodec = ResolveVideoCodec(hdrRequest, codecError);
        TEST("VideoExport_HdrDefaultsToHevc",
             hdrCodec == VideoCodec::Hevc && codecError.empty() && std::wstring(EncoderName(hdrCodec)) == L"libx265");
        FfmpegEncodeSettings hdrSettings;
        hdrSettings.codec = hdrCodec;
        hdrSettings.width = 1920;
        hdrSettings.height = 1080;
        hdrSettings.mastering = DefaultMasteringDisplay();
        hdrSettings.maxCll = 1000;
        hdrSettings.maxFall = 400;
        const std::wstring hdrArgs = BuildFfmpegArgs(hdrRequest, hdrSettings);
        TEST("VideoExport_ArgsRawInput", contains(hdrArgs, L"-pix_fmt p010le -video_size 1920x1080 -framerate 60 "));
        TEST("VideoExport_ArgsHdr10Metadata", contains(hdrArgs,
            L"master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,50):max-cll=1000,400"));
        TEST("VideoExport_ArgsHevcMp4", contains(hdrArgs, L"-crf 18 ") && contains(hdrArgs, L"-tag:v hvc1 ") &&
             hdrArgs.ends_with(L"\"C:\\out dir\\clip.mp4\""));
        hdrSettings.mastering.reset();
        TEST("VideoExport_ArgsNoMetadataWhenNone", !contains(BuildFfmpegArgs(hdrRequest, hdrSettings), L"master-display"));

        VideoExportRequest sdrRequest;
        sdrRequest.format = VideoFormat::Sdr;
        sdrRequest.outputPath = L"clip.mkv";
        sdrRequest.fps = 30000.0 / 1001.0;
        FfmpegEncodeSettings sdrSettings;
        sdrSettings.codec = ResolveVideoCodec(sdrRequest, codecError);
        sdrSettings.width = 640;
        sdrSettings.height = 360;
        const std::wstring sdrArgs = BuildFfmpegArgs(sdrRequest, sdrSettings);
        TEST("VideoExport_SdrDefaultsToH264", sdrSettings.codec == VideoCodec::H264 && codecError.empty());
        TEST("VideoExport_ArgsSdr", contains(sdrArgs, L"-pix_fmt nv12 ") && contains(sdrArgs, L"-framerate 30000/1001 ") &&
             contains(sdrArgs, L"-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p ") && !contains(sdrArgs, L"hvc1"));

        VideoExportRequest refused = hdrRequest;
        refused.codec = VideoCodec::H264;
        std::wstring refusedError;
        ResolveVideoCodec(refused, refusedError);
        TEST("VideoExport_Hdr10RefusesH264", contains(refusedError, L"HDR10 needs HEVC or AV1"));
        refused.codec = VideoCodec::Av1;
        refused.lossless = true;
        refusedError.clear();
        ResolveVideoCodec(refused, refusedError);
        TEST("VideoExport_Av1RefusesLossless", contains(refusedError, L"not supported with AV1"));
    }

    // Video source: a frame-accurate seek, no re-seek once satisfied, and the
    // zero-copy (hardware decode) upload producing exactly what the CPU Lock2D
    // path does. On WARP there is no hardware decoder, so both providers take
    // the CPU path; the zero-copy half needs a hardware run.
    SL_TEST_CASE(TestVideoSourceSeekAndZeroCopy, Tag::Gpu | Tag::Video)
    {
        printf("\n=== Video source: frame-accurate seek + zero-copy upload ===\n");
        struct Clip { const wchar_t* file; double frame; const char* tag; };
        const Clip clips[] = {
            { L"video_hdr10_hevc_30f.mp4", 17.5, "HEVC Main10 (P010)" },
            { L"video_sdr_h264_30f.mp4",   17.5, "H.264 High, B-frames (NV12)" },
        };
        auto readback = [&](ID3D11Texture2D* tex, std::vector<uint8_t>& out) -> bool
        {
            if (!tex) return false;
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.MiscFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::com_ptr<ID3D11Texture2D> staging;
            if (FAILED(g_d3dDevice->CreateTexture2D(&d, nullptr, staging.put()))) return false;
            g_d3dContext->CopyResource(staging.get(), tex);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
            const size_t row = static_cast<size_t>(d.Width) * 8;   // RGBA16F
            out.resize(row * d.Height);
            for (UINT y = 0; y < d.Height; ++y)
                std::memcpy(out.data() + y * row, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, row);
            g_d3dContext->Unmap(staging.get(), 0);
            return true;
        };
        for (const auto& clip : clips)
        {
            const std::wstring path = FindFixture(clip.file);
            if (path.empty())
            {
                // Generated, not checked in -- and CI has no ffmpeg. Neither a
                // pass nor a fail: say so loudly instead.
                SkipMissingFixture(clip.file);
                continue;
            }

            ShaderLab::Effects::VideoSourceProvider zc, cpu;
            cpu.SetZeroCopyEnabled(false);
            const bool opened =
                zc.Open(path, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get()) &&
                cpu.Open(path, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
            TEST("Video_Opens", opened);
            if (!opened) continue;

            const double fps = zc.FrameRate() > 0 ? zc.FrameRate() : 30.0;
            const double target = clip.frame / fps;   // mid-frame, far from the only keyframe
            // Open decodes the first frame synchronously; consume it so the
            // upload below is the seek's own frame.
            zc.UploadIfReady(g_dc.get());
            cpu.UploadIfReady(g_dc.get());
            const uint64_t dz = zc.DecodeCount(), dcpu = cpu.DecodeCount();
            bool uploadedZc = false, uploadedCpu = false;
            zc.Seek(target);
            cpu.Seek(target);
            for (int i = 0; i < 600 && !(uploadedZc && uploadedCpu); ++i)
            {
                if (!uploadedZc && zc.DecodeCount() > dz)    uploadedZc  = zc.UploadIfReady(g_dc.get());
                if (!uploadedCpu && cpu.DecodeCount() > dcpu) uploadedCpu = cpu.UploadIfReady(g_dc.get());
                if (!(uploadedZc && uploadedCpu)) ::Sleep(5);
            }
            TEST("Video_SeekDeliversAFrame", uploadedZc && uploadedCpu);
            if (!(uploadedZc && uploadedCpu))
                printf("    (debug) decodes zc %llu->%llu cpu %llu->%llu, err '%ls'\n",
                    (unsigned long long)dz, (unsigned long long)zc.DecodeCount(),
                    (unsigned long long)dcpu, (unsigned long long)cpu.DecodeCount(),
                    zc.LastError().c_str());

            // The frame shown must be the one covering the target, not the
            // keyframe MF lands on.
            const double pos = zc.CurrentPosition();
            const double expected = std::floor(clip.frame) / fps;
            printf("  [info] %s: %.2f fps, target %.4f s -> frame at %.4f s (expected %.4f), zero-copy %s\n",
                clip.tag, fps, target, pos, expected, zc.LastUploadWasZeroCopy() ? "yes" : "no");
            TEST("Video_SeekIsFrameAccurate", std::abs(pos - expected) < 0.25 / fps);
            TEST("Video_SeekTargetRemembered", zc.LastSeekTarget() == target);

            std::vector<uint8_t> a, b;
            const bool readA = readback(zc.OutputTexture(), a);
            const bool readB = readback(cpu.OutputTexture(), b);
            TEST("Video_ZeroCopyMatchesCpuPath", readA && readB && a == b);
            if (readA && readB && a != b && a.size() == b.size())
            {
                const size_t row = static_cast<size_t>(zc.FrameWidth()) * 8;
                size_t firstRow = SIZE_MAX, rowsDiff = 0;
                for (size_t y = 0; y * row < a.size(); ++y)
                    if (std::memcmp(a.data() + y * row, b.data() + y * row, row) != 0)
                    { ++rowsDiff; if (firstRow == SIZE_MAX) firstRow = y; }
                printf("    (debug) %ux%u: %zu rows differ, first %zu\n",
                    zc.FrameWidth(), zc.FrameHeight(), rowsDiff, firstRow);
            }
            // Informational: decode + upload wall time per frame, both paths.
            auto timeFrames = [&](ShaderLab::Effects::VideoSourceProvider& p, int frames) -> double
            {
                p.Seek(0.0);
                auto t0 = std::chrono::steady_clock::now();
                int got = 0;
                while (got < frames &&
                       std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20))
                {
                    if (p.UploadIfReady(g_dc.get())) { ++got; p.RequestNextFrame(); }
                    else std::this_thread::yield();
                }
                g_d3dContext->Flush();
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                return got ? ms / got : 0.0;
            };
            const double msZc = timeFrames(zc, 20);
            const double msCpu = timeFrames(cpu, 20);
            printf("  [info] %s %ux%u: %.2f ms/frame zero-copy vs %.2f ms/frame CPU path\n",
                clip.tag, zc.FrameWidth(), zc.FrameHeight(), msZc, msCpu);
            zc.Close();
            cpu.Close();
        }
    }

    // Video frame conversion must survive another D2D context drawing on the
    // same D3D11 device, as the GUI's UI thread does. The output is cleared to
    // transparent black before each upload, so a lost dispatch reads as alpha 0.
    SL_TEST_CASE(TestVideoUploadUnderD2DContention, Tag::Gpu | Tag::Video)
    {
        printf("\n=== Video source: frame upload under concurrent D2D drawing ===\n");
        const std::wstring path = FindFixture(L"video_sdr_h264_30f.mp4");
        if (path.empty())
        {
            SkipMissingFixture(L"video_sdr_h264_30f.mp4");
            return;
        }
        winrt::com_ptr<ID2D1Factory7> factory;
        winrt::com_ptr<ID2D1Device6> device;
        winrt::com_ptr<ID2D1DeviceContext5> dcWorker, dcUi;
        bool ok = SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
            __uuidof(ID2D1Factory7), nullptr, factory.put_void()));
        ok = ok && SUCCEEDED(factory->CreateDevice(g_d3dDevice.as<IDXGIDevice>().get(),
            reinterpret_cast<ID2D1Device**>(device.put())));
        ok = ok && SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            reinterpret_cast<ID2D1DeviceContext**>(dcWorker.put())));
        ok = ok && SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            reinterpret_cast<ID2D1DeviceContext**>(dcUi.put())));
        TEST("VideoContention_Setup", ok);
        if (!ok) return;

        ShaderLab::Effects::VideoSourceProvider video;
        video.SetLoop(true);
        const bool opened = video.Open(path, dcWorker.get(), g_d3dDevice.get(), g_d3dContext.get());
        TEST("VideoContention_Opens", opened);
        if (!opened) return;

        // Stand-in for the UI thread: draw a blurred flood into its own target
        // as fast as it can until told to stop.
        std::atomic<bool> stop{ false };
        std::atomic<uint32_t> uiDraws{ 0 }, uiEndDrawFailures{ 0 };
        std::thread uiThread([&] {
            winrt::com_ptr<ID2D1Bitmap1> target;
            D2D1_BITMAP_PROPERTIES1 targetProps{};
            targetProps.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            targetProps.dpiX = targetProps.dpiY = 96.0f;
            targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            if (FAILED(dcUi->CreateBitmap({ 512, 512 }, nullptr, 0, targetProps, target.put()))) return;
            winrt::com_ptr<ID2D1Effect> flood, blur;
            dcUi->CreateEffect(CLSID_D2D1Flood, flood.put());
            dcUi->CreateEffect(CLSID_D2D1GaussianBlur, blur.put());
            if (!flood || !blur) return;
            blur->SetInputEffect(0, flood.get());
            dcUi->SetTarget(target.get());
            for (uint32_t frame = 0; !stop.load(); ++frame)
            {
                flood->SetValue(D2D1_FLOOD_PROP_COLOR, D2D1::Vector4F(0.2f, 0.5f, float(frame % 7) / 7.0f, 1.0f));
                dcUi->BeginDraw();
                dcUi->DrawImage(blur.get());
                if (FAILED(dcUi->EndDraw())) ++uiEndDrawFailures;
                ++uiDraws;
            }
            dcUi->SetTarget(nullptr);
        });

        // Stand-in for the render worker: clear, upload the next frame, read
        // back the centre texel.
        winrt::com_ptr<ID3D11UnorderedAccessView> clearUav;
        g_d3dDevice->CreateUnorderedAccessView(video.OutputTexture(), nullptr, clearUav.put());
        D3D11_TEXTURE2D_DESC texelDesc{};
        video.OutputTexture()->GetDesc(&texelDesc);
        texelDesc.Width = texelDesc.Height = 1; texelDesc.Usage = D3D11_USAGE_STAGING; texelDesc.BindFlags = 0;
        texelDesc.MiscFlags = 0; texelDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> texel;
        g_d3dDevice->CreateTexture2D(&texelDesc, nullptr, texel.put());
        const UINT centerX = video.FrameWidth() / 2, centerY = video.FrameHeight() / 2;

        int uploads = 0, lost = 0;
        const auto start = std::chrono::steady_clock::now();
        while (uploads < 400 && std::chrono::steady_clock::now() - start < std::chrono::seconds(60))
        {
            const float zero[4] = { 0, 0, 0, 0 };
            g_d3dContext->ClearUnorderedAccessViewFloat(clearUav.get(), zero);
            video.RequestNextFrame();
            bool uploaded = false;
            for (int attempt = 0; attempt < 400 && !uploaded; ++attempt)
            {
                uploaded = video.UploadIfReady(dcWorker.get());
                if (!uploaded) ::Sleep(1);
            }
            if (!uploaded) continue;
            ++uploads;
            D3D11_BOX box = { centerX, centerY, 0, centerX + 1, centerY + 1, 1 };
            g_d3dContext->CopySubresourceRegion(texel.get(), 0, 0, 0, 0, video.OutputTexture(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(g_d3dContext->Map(texel.get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                const uint16_t alphaHalf = static_cast<const uint16_t*>(mapped.pData)[3];
                g_d3dContext->Unmap(texel.get(), 0);
                if (alphaHalf != 0x3C00) ++lost;   // 1.0 in FP16
            }
        }
        stop = true;
        uiThread.join();
        video.Close();
        printf("  [info] %d uploads while the UI context drew %u frames: %d lost (output left at alpha 0); "
               "UI EndDraw failures %u\n", uploads, uiDraws.load(), lost, uiEndDrawFailures.load());
        // WARP rasterises on the CPU, so the drawing thread starves the uploads
        // and the race rarely shows; there the test only has to show it ran.
        const int minUploads = g_useWarp ? 20 : 100;
        if (g_useWarp)
            printf("  [info] WARP: contention evidence is weak here; run with --adapter default for the real check\n");
        TEST("VideoContention_EnoughWork", uploads >= minUploads && uiDraws.load() >= 100);
        TEST("VideoContention_NoLostConversion", lost == 0);
        TEST("VideoContention_NoUiEndDrawFailures", uiEndDrawFailures.load() == 0);
    }

    // Video FrameTime: the analysis field matches the frame actually uploaded
    // on the same tick, resets on Close, and a negative looped Time wraps to
    // the end of the clip instead of clamping to the first frame.
    SL_TEST_CASE(TestVideoFrameTimeReporting, Tag::Gpu | Tag::Video)
    {
        printf("\n=== Video source: FrameTime reporting ===\n");
        const std::wstring path = FindFixture(L"video_sdr_h264_30f.mp4");
        if (path.empty())
        {
            SkipMissingFixture(L"video_sdr_h264_30f.mp4");
            return;
        }

        // Provider: -1 before the first upload and again after Close.
        {
            ShaderLab::Effects::VideoSourceProvider video;
            const bool opened = video.Open(path, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
            TEST("VideoFrameTime_Opens", opened);
            if (!opened) return;
            const double beforeUpload = video.UploadedFrameTime();
            const bool uploaded = video.UploadIfReady(g_dc.get());
            const double firstFrame = video.UploadedFrameTime();
            const double firstDuration = video.UploadedFrameDuration();
            printf("  [info] first upload %s: frame at %.4f s, shown for %.4f s\n",
                   uploaded ? "ok" : "failed", firstFrame, firstDuration);
            TEST("VideoFrameTime_UnsetBeforeFirstUpload", beforeUpload == -1.0);
            TEST("VideoFrameTime_FirstUploadIsFirstFrame", uploaded && firstFrame >= 0.0 && firstFrame == video.FirstFrameTime());
            TEST("VideoFrameTime_DurationIsOneFrame", std::abs(firstDuration - 1.0 / 30.0) < 1e-3);
            video.Close();
            TEST("VideoFrameTime_ResetOnClose", video.UploadedFrameTime() == -1.0 && video.UploadedFrameDuration() == 0.0);
        }

        // Factory: the FrameTime field written on a tick is the frame uploaded on that tick.
        using ShaderLab::Effects::SourceNodeFactory;
        SourceNodeFactory factory;
        ShaderLab::Graph::GraphModel graph;
        const uint32_t videoId = graph.AddNode(SourceNodeFactory::CreateVideoSourceNode(path));
        auto nodes = graph.MutableNodes();
        auto* videoNode = graph.FindNode(videoId);
        factory.PrepareSourceNode(*videoNode, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        auto* provider = factory.VideoProviderForRenderThread(videoId);
        TEST("VideoFrameTime_FactoryOpens", provider != nullptr);
        if (!provider) return;
        auto frameTimeField = [&]
        {
            for (const auto& field : graph.FindNode(videoId)->analysisOutput.fields)
                if (field.name == L"FrameTime") return static_cast<double>(field.components[0]);
            return -2.0;
        };
        // Tick until the uploaded frame is the one for `seconds`; every tick
        // must report exactly the uploaded frame.
        auto tickUntil = [&](float seconds, double expectedFrame, bool& fieldAlwaysMatched)
        {
            ShaderLab::Graph::SetNodeProperty(*graph.FindNode(videoId), L"Time", seconds);
            for (int i = 0; i < 500; ++i)
            {
                factory.TickAndUploadVideos(nodes, g_dc.get());
                if (std::abs(frameTimeField() - static_cast<float>(provider->UploadedFrameTime())) > 1e-6)
                    fieldAlwaysMatched = false;
                if (std::abs(provider->UploadedFrameTime() - expectedFrame) < 1e-3) return true;
                ::Sleep(10);
            }
            return false;
        };
        bool fieldMatched = true;
        const bool reachedMid = tickUntil(17.5f / 30.0f, 17.0 / 30.0, fieldMatched);
        TEST("VideoFrameTime_FactoryReachesSeekTarget", reachedMid);
        TEST("VideoFrameTime_FieldMatchesUploadSameTick", fieldMatched);
        // Half a frame before 0, looped, is inside the last frame.
        const double lastFrame = std::floor(provider->Duration() * 30.0 - 0.5) / 30.0;
        bool wrapMatched = true;
        const bool reachedWrap = tickUntil(-0.5f / 30.0f, lastFrame, wrapMatched);
        printf("  [info] Time -0.5 frame, looped: frame at %.4f s (expected %.4f s)\n",
               provider->UploadedFrameTime(), lastFrame);
        TEST("VideoFrameTime_NegativeLoopedTimeWraps", reachedWrap && wrapMatched);
    }

    // A video node re-pointed at a file that does not open drops the old
    // provider's bitmap instead of keeping a pointer to it. The factory is
    // owned by this thread, so a Debug build also checks every call's thread.
    SL_TEST_CASE(TestVideoPathChangeToMissingFile, Tag::Gpu | Tag::Video)
    {
        printf("\n=== Video source: path changed to a missing file ===\n");
        const std::wstring path = FindFixture(L"video_sdr_h264_30f.mp4");
        if (path.empty())
        {
            SkipMissingFixture(L"video_sdr_h264_30f.mp4");
            return;
        }
        using ShaderLab::Effects::SourceNodeFactory;
        SourceNodeFactory factory;
        factory.SetOwnerThread(std::this_thread::get_id());
        ShaderLab::Graph::GraphModel graph;
        const uint32_t videoId = graph.AddNode(SourceNodeFactory::CreateVideoSourceNode(path));
        auto* videoNode = graph.FindNode(videoId);
        factory.PrepareSourceNode(*videoNode, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        factory.TickAndUploadVideos(graph.MutableNodes(), g_dc.get());
        const bool opened = videoNode->cachedOutput != nullptr;

        ShaderLab::Graph::SetNodeMedia(*videoNode, (std::filesystem::temp_directory_path() / L"ShaderLabAbsentClip.mp4").wstring());
        ShaderLab::Graph::MarkNodeChanged(*videoNode);
        factory.PrepareSourceNode(*videoNode, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        printf("  [info] after re-point: '%ls'\n", videoNode->runtimeError.c_str());
        TEST("VideoSource_FirstClipShowsAFrame", opened);
        TEST("VideoSource_FailedReopenDropsOutput",
             videoNode->cachedOutput == nullptr && videoNode->runtimeError.starts_with(L"Failed to open video:") &&
             !factory.HasVideoProvider(videoId));
        factory.ReleaseCache();
        factory.SetOwnerThread({});
    }
}
