#include "pch_engine.h"
#include "../ShaderTestBench.h"
#include "../TestSupport.h"
#include "Effects/ShaderLabEffects.h"
#include "Graph/ClockModel.h"
#include "Graph/GraphEdits.h"
#include "Graph/GraphModel.h"
#include "Rendering/VideoExport.h"

#include <cmath>
#include <cstdio>
#include <string>

// Video export: the Clock-driven timeline, codec choice, ffmpeg arguments and
// the Y'CbCr conversion shader.
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

    void TestVideoConvertHlsl(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: video export conversion ===\n");
        // Video export conversion (Rendering/VideoExport.cpp) against
        // values any Y'CbCr table gives. BT.709 limited 8-bit: red is
        // (63, 102, 240), white (235, 128, 128), black (16, 128, 128).
        // BT.2020 PQ limited 10-bit: black 64, a 100-nit neutral
        // floor(64 + 876 * PQ(100) + 0.5) = floor(64 + 876*0.5080784 + 0.5)
        // = 509, 10000 nits 940; neutrals carry chroma 512.
        {
            auto r = bench.Run(R"(
                float3 red  = VideoEncodeSdr(float3(1, 0, 0));
                float3 wht  = VideoEncodeSdr(float3(1, 1, 1));
                float3 blk  = VideoEncodeSdr(float3(0, 0, 0));
                Result[0] = float4(VideoLumaCode(red, 1), VideoChromaCode(VideoChromaDiff(red, 1), 1), 0);
                Result[1] = float4(VideoLumaCode(wht, 1), VideoChromaCode(VideoChromaDiff(wht, 1), 1), 0);
                Result[2] = float4(VideoLumaCode(blk, 1), VideoChromaCode(VideoChromaDiff(blk, 1), 1), 0);
                float3 h100 = VideoEncodeHdr10(float3(1.25, 1.25, 1.25));
                float3 hMax = VideoEncodeHdr10(float3(125, 125, 125));
                float3 hBlk = VideoEncodeHdr10(float3(0, 0, 0));
                Result[3] = float4(VideoLumaCode(h100, 0), VideoChromaCode(VideoChromaDiff(h100, 0), 0), 0);
                Result[4] = float4(VideoLumaCode(hMax, 0), VideoLumaCode(hBlk, 0), VideoChromaCode(VideoChromaDiff(hBlk, 0), 0));
                Result[5] = float4(VideoContentNits(float3(1.25, 1.25, 1.25)), VideoContentNits(float3(500, 0, 0)),
                                   VideoContentNits(float3(-1, -1, -1)), 0);
            )", 6, ShaderLab::Rendering::VideoConvertHelpersHLSL());
            const bool ok = r.size() >= 6;
            TEST("Video SDR: BT.709 red encodes to (63, 102, 240)",
                ok && r[0].x == 63.0f && r[0].y == 102.0f && r[0].z == 240.0f);
            TEST("Video SDR: white (235,128,128), black (16,128,128)",
                ok && r[1].x == 235.0f && r[1].y == 128.0f && r[1].z == 128.0f
                   && r[2].x == 16.0f && r[2].y == 128.0f && r[2].z == 128.0f);
            TEST("Video HDR10: 100-nit neutral is Y 509, chroma 512",
                ok && r[3].x == 509.0f && r[3].y == 512.0f && r[3].z == 512.0f);
            TEST("Video HDR10: 10000 nits is Y 940, black Y 64 / chroma 512",
                ok && r[4].x == 940.0f && r[4].y == 64.0f && r[4].z == 512.0f && r[4].w == 512.0f);
            TEST("Video content light: 100-nit neutral reads 100, clamps at 10000 and 0",
                ok && Near(r[5].x, 100.0f, 0.05f) && r[5].y == 10000.0f && r[5].z == 0.0f);
        }
    }

    SL_REGISTER_TEST(TestVideoConvertHlsl, Tag::Gpu | Tag::Math, [] { TestVideoConvertHlsl(Bench()); });
}
