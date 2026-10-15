#include "pch_engine.h"
#include "../TestCommon.h"
#include "../../ShaderLabHeadless/HeadlessArgs.h"
#include "../../Rendering/VideoExport.h"

#include <initializer_list>
#include <vector>

// ShaderLabHeadless's command line: every value is checked in full, the
// selected mode decides which flags are required, and a flag the mode does
// not read is reported instead of silently dropped.
namespace
{
    using namespace ShaderLab::Tests;
    using namespace ShaderLab::Headless;

    ParseResult Parse(std::initializer_list<const wchar_t*> flags, Args& args)
    {
        std::vector<const wchar_t*> argv{ L"ShaderLabHeadless.exe" };
        argv.insert(argv.end(), flags.begin(), flags.end());
        return ParseArgs(static_cast<int>(argv.size()), argv.data(), args);
    }

    ParseResult Parse(std::initializer_list<const wchar_t*> flags)
    {
        Args args;
        return Parse(flags, args);
    }

    bool Rejects(std::initializer_list<const wchar_t*> flags, std::wstring_view mentioning)
    {
        const auto result = Parse(flags);
        return result.outcome == ParseOutcome::Error && result.error.find(mentioning) != std::wstring::npos;
    }

    bool Warns(const ParseResult& result, std::wstring_view flag)
    {
        for (const auto& warning : result.warnings)
            if (warning.rfind(flag, 0) == 0) return true;
        return false;
    }

    SL_TEST_CASE(TestHeadlessArgsAcceptsEveryMode, Tag::Fast)
    {
        printf("\n=== Headless arguments: modes ===\n");
        Args image;
        auto result = Parse({ L"--graph", L"g.json", L"--node", L"7", L"--output", L"o.png",
                              L"--width", L"640", L"--height", L"360", L"--adapter", L"warp",
                              L"--time", L"1.25", L"--output-peak-nits", L"203" }, image);
        TEST("image: parses", result.outcome == ParseOutcome::Run && result.mode == Mode::Image);
        TEST("image: no warnings", result.warnings.empty());
        TEST("image: values", image.nodeId == 7 && image.width == 640 && image.height == 360 && image.useWarp &&
             image.time == 1.25 && image.outputPeakNits == 203.0f && image.toneMapExplicit);

        Args pixels;
        result = Parse({ L"--graph", L"g.json", L"--node", L"1", L"--output", L"o.bin", L"--pixels", L"-2,3,4,5" }, pixels);
        TEST("pixels: parses", result.outcome == ParseOutcome::Run && result.mode == Mode::Pixels);
        TEST("pixels: region", pixels.pixelX == -2 && pixels.pixelY == 3 && pixels.pixelW == 4 && pixels.pixelH == 5);

        Args video;
        result = Parse({ L"--graph", L"g.json", L"--node", L"1", L"--video", L"o.mp4", L"--format", L"sdr",
                         L"--fps", L"29.97", L"--max-cll", L"1000,400", L"--crf", L"18" }, video);
        TEST("video: parses", result.outcome == ParseOutcome::Run && result.mode == Mode::Video);
        TEST("video: values", video.videoFormat == L"sdr" && video.videoFps == 29.97 &&
             video.videoMaxCll == 1000u && video.videoMaxFall == 400u && video.videoCrf == 18);

        result = Parse({ L"--graph", L"g.json", L"--script", L"s.json" });
        TEST("script: needs only --graph and --script", result.outcome == ParseOutcome::Run && result.mode == Mode::Script);
        result = Parse({ L"--graph", L"g.json", L"--mcp-session", L"--pipe", L"p" });
        TEST("session: parses", result.outcome == ParseOutcome::Run && result.mode == Mode::McpSession);
        result = Parse({ L"--reap-shader-cache", L"--reap-shader-cache-stale-sec", L"60" });
        TEST("shader cache: needs no graph", result.outcome == ParseOutcome::Run && result.mode == Mode::ShaderCache);
        TEST("--help", Parse({ L"--graph", L"g.json", L"--help" }).outcome == ParseOutcome::Help);
    }

    SL_TEST_CASE(TestHeadlessArgsRejectMalformedValues, Tag::Fast)
    {
        printf("\n=== Headless arguments: malformed values fail closed ===\n");
        TEST("--width abc", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--width", L"abc" }, L"--width"));
        TEST("--width 12px", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--width", L"12px" }, L"--width"));
        TEST("--width 0", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--width", L"0" }, L"--width"));
        TEST("--node x", Rejects({ L"--graph", L"g", L"--node", L"x", L"--output", L"o" }, L"--node"));
        TEST("--node -1", Rejects({ L"--graph", L"g", L"--node", L"-1", L"--output", L"o" }, L"--node"));
        TEST("--node 4294967296", Rejects({ L"--graph", L"g", L"--node", L"4294967296", L"--output", L"o" }, L"--node"));
        TEST("--adapter wrap (typo)", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--adapter", L"wrap" }, L"'warp' or 'default'"));
        TEST("--time 1s", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--time", L"1s" }, L"--time"));
        TEST("--input-peak-nits -5", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--input-peak-nits", L"-5" }, L"--input-peak-nits"));
        TEST("--pixels trailing text", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--pixels", L"0,0,4,4x" }, L"--pixels"));
        TEST("--pixels five values", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--pixels", L"0,0,4,4,4" }, L"--pixels"));
        TEST("--pixels zero width", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--pixels", L"0,0,0,4" }, L"--pixels"));
        TEST("--max-cll trailing text", Rejects({ L"--graph", L"g", L"--node", L"1", L"--video", L"v", L"--max-cll", L"1000,400nits" }, L"--max-cll"));
        TEST("--format names the valid values", Rejects({ L"--graph", L"g", L"--node", L"1", L"--video", L"v", L"--format", L"hdr" }, L"hdr10 or sdr"));
        TEST("--codec names the valid values", Rejects({ L"--graph", L"g", L"--node", L"1", L"--video", L"v", L"--codec", L"vp9" }, L"hevc, av1 or h264"));
        TEST("--mastering names the valid values", Rejects({ L"--graph", L"g", L"--node", L"1", L"--video", L"v", L"--mastering", L"p3" }, L"working-space"));
        TEST("--display-profile names the presets", Rejects({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--display-profile", L"p3" }, L"bt2020-1000"));
        TEST("--fps 0", Rejects({ L"--graph", L"g", L"--node", L"1", L"--video", L"v", L"--fps", L"0" }, L"--fps"));
        TEST("missing value", Rejects({ L"--graph" }, L"requires an argument"));
        TEST("unknown flag", Rejects({ L"--graph", L"g", L"--nodes", L"1" }, L"unknown argument"));
        TEST("image needs --output", Rejects({ L"--graph", L"g", L"--node", L"1" }, L"--output"));
        TEST("video needs --node", Rejects({ L"--graph", L"g", L"--video", L"v" }, L"--node"));
        TEST("script needs --graph", Rejects({ L"--script", L"s" }, L"--graph"));
    }

    SL_TEST_CASE(TestHeadlessArgsReportUnusedFlags, Tag::Fast)
    {
        printf("\n=== Headless arguments: flags the mode does not read ===\n");
        auto script = Parse({ L"--graph", L"g", L"--script", L"s", L"--output", L"o.png", L"--node", L"3",
                              L"--width", L"256", L"--time", L"2" });
        TEST("script: --output reported", Warns(script, L"--output"));
        TEST("script: gpu-bench defaults and --time not reported",
             !Warns(script, L"--node") && !Warns(script, L"--width") && !Warns(script, L"--time"));

        auto video = Parse({ L"--graph", L"g", L"--node", L"1", L"--video", L"v", L"--time", L"2", L"--no-tonemap" });
        TEST("video: --time reported (the export sets every frame's time)", Warns(video, L"--time"));
        TEST("video: --no-tonemap reported", Warns(video, L"--no-tonemap"));

        auto image = Parse({ L"--graph", L"g", L"--node", L"1", L"--output", L"o", L"--gpu-timing", L"--fps", L"30" });
        TEST("image: --gpu-timing reported", Warns(image, L"--gpu-timing"));
        TEST("image: video flags reported", Warns(image, L"--fps"));

        auto session = Parse({ L"--graph", L"g", L"--mcp-session", L"--script", L"s", L"--node", L"1" });
        TEST("session wins over --script, which is reported", session.mode == Mode::McpSession && Warns(session, L"--script"));
        TEST("session: --node reported", Warns(session, L"--node"));
    }

    SL_TEST_CASE(TestParseVideoOptions, Tag::Fast)
    {
        printf("\n=== ParseVideoOptions ===\n");
        using namespace ShaderLab::Rendering;
        VideoExportRequest request;
        std::wstring error;
        TEST("defaults", ParseVideoOptions(L"", L"", L"", request, error) && request.format == VideoFormat::Hdr10 &&
             request.codec == VideoCodec::Default && request.mastering == MasteringMode::P3D65_1000);
        TEST("aliases", ParseVideoOptions(L"sdr", L"avc", L"none", request, error) && request.format == VideoFormat::Sdr &&
             request.codec == VideoCodec::H264 && request.mastering == MasteringMode::None);
        TEST("h265 is HEVC", ParseVideoOptions(L"hdr10", L"h265", L"working-space", request, error) &&
             request.codec == VideoCodec::Hevc && request.mastering == MasteringMode::WorkingSpace);
        error.clear();
        TEST("unknown codec refused with the valid values", !ParseVideoOptions(L"hdr10", L"vp9", L"", request, error) &&
             error.find(L"hevc, av1 or h264") != std::wstring::npos);
    }
}
