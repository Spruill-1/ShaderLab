#include "pch_engine.h"
#include "HeadlessArgs.h"

#include "../Graph/PropertyWrite.h"
#include "../Rendering/VideoExport.h"

#include <cstdio>

namespace ShaderLab::Headless
{
    const wchar_t* const cDisplayPresetNames =
        L"srgb-sdr, srgb-270, p3-600, p3-1000, bt2020-1000, bt2020-4000, adobergb";

    std::optional<Rendering::DisplayProfile> DisplayPresetByName(std::wstring_view name)
    {
        using namespace ShaderLab::Rendering;
        if (name == L"srgb-sdr")    return PresetSrgbSdr();
        if (name == L"srgb-270")    return PresetSrgb270();
        if (name == L"p3-600")      return PresetP3_600();
        if (name == L"p3-1000")     return PresetP3_1000();
        if (name == L"bt2020-1000") return PresetBT2020_1000();
        if (name == L"bt2020-4000") return PresetBT2020_4000();
        if (name == L"adobergb")    return PresetAdobeRGB();
        return std::nullopt;
    }

    const wchar_t* ModeName(Mode mode)
    {
        switch (mode)
        {
        case Mode::Image:       return L"image";
        case Mode::Pixels:      return L"pixels";
        case Mode::Video:       return L"video";
        case Mode::Script:      return L"script";
        case Mode::McpSession:  return L"mcp-session";
        case Mode::ShaderCache: return L"shader-cache";
        }
        return L"unknown";
    }

    namespace
    {
        // The modes a flag is read in, as a bit mask over Mode.
        constexpr uint32_t Bit(Mode mode) { return 1u << static_cast<uint32_t>(mode); }
        constexpr uint32_t cImage   = Bit(Mode::Image);
        constexpr uint32_t cPixels  = Bit(Mode::Pixels);
        constexpr uint32_t cVideo   = Bit(Mode::Video);
        constexpr uint32_t cScript  = Bit(Mode::Script);
        constexpr uint32_t cSession = Bit(Mode::McpSession);
        constexpr uint32_t cCache   = Bit(Mode::ShaderCache);
        constexpr uint32_t cGraphModes = cImage | cPixels | cVideo | cScript | cSession;

        std::wstring Got(std::wstring_view flag, std::wstring_view what, std::wstring_view value)
        {
            return std::wstring(flag) + L" expects " + std::wstring(what) + L" (got '" + std::wstring(value) + L"')";
        }

        // All of `text` as decimal digits that fit in uint64_t.
        std::optional<uint64_t> ParseUnsigned(std::wstring_view text)
        {
            if (text.empty() || text.size() > 20) return std::nullopt;
            uint64_t value = 0;
            for (wchar_t c : text)
            {
                if (c < L'0' || c > L'9') return std::nullopt;
                const uint64_t digit = static_cast<uint64_t>(c - L'0');
                if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
                value = value * 10 + digit;
            }
            return value;
        }

        std::optional<uint32_t> ParseU32(std::wstring_view text)
        {
            auto value = ParseUnsigned(text);
            if (!value || *value > UINT32_MAX) return std::nullopt;
            return static_cast<uint32_t>(*value);
        }

        std::optional<int32_t> ParseI32(std::wstring_view text)
        {
            const bool negative = !text.empty() && text.front() == L'-';
            if (negative) text.remove_prefix(1);
            auto value = ParseUnsigned(text);
            if (!value) return std::nullopt;
            const uint64_t limit = negative ? static_cast<uint64_t>(INT32_MAX) + 1 : static_cast<uint64_t>(INT32_MAX);
            if (*value > limit) return std::nullopt;
            const int64_t magnitude = static_cast<int64_t>(*value);
            return static_cast<int32_t>(negative ? -magnitude : magnitude);
        }

        // A finite number written in full; the whole argument must parse.
        std::optional<double> ParseFinite(std::wstring_view text)
        {
            return Graph::ParseNumber(text);
        }

        bool SetPositiveU32(std::wstring_view flag, std::wstring_view v, uint32_t& out, std::wstring& error)
        {
            auto value = ParseU32(v);
            if (!value || *value == 0) { error = Got(flag, L"a positive integer", v); return false; }
            out = *value;
            return true;
        }

        bool SetPositiveFloat(std::wstring_view flag, std::wstring_view v, float& out, std::wstring& error)
        {
            auto value = ParseFinite(v);
            if (!value || *value <= 0.0 || !std::isfinite(static_cast<float>(*value)))
            {
                error = Got(flag, L"a positive number", v);
                return false;
            }
            out = static_cast<float>(*value);
            return true;
        }

        bool CheckVideoOption(std::wstring_view format, std::wstring_view codec, std::wstring_view mastering,
                              std::wstring& error)
        {
            Rendering::VideoExportRequest scratch;
            return Rendering::ParseVideoOptions(std::wstring(format), std::wstring(codec), std::wstring(mastering),
                                                scratch, error);
        }

        // "x,y,w,h": four integers, w and h positive, nothing else.
        bool ParsePixelRegion(std::wstring_view v, Args& a, std::wstring& error)
        {
            int32_t parsed[4]{};
            std::wstring_view rest = v;
            for (int i = 0; i < 4; ++i)
            {
                const size_t comma = rest.find(L',');
                const bool last = i == 3;
                if (last != (comma == std::wstring_view::npos))
                {
                    error = Got(L"--pixels", L"4 comma-separated integers 'x,y,w,h'", v);
                    return false;
                }
                auto value = ParseI32(rest.substr(0, comma));
                if (!value) { error = Got(L"--pixels", L"4 comma-separated integers 'x,y,w,h'", v); return false; }
                parsed[i] = *value;
                if (!last) rest.remove_prefix(comma + 1);
            }
            if (parsed[2] <= 0 || parsed[3] <= 0)
            {
                error = Got(L"--pixels", L"a positive width and height", v);
                return false;
            }
            a.pixelX = parsed[0];
            a.pixelY = parsed[1];
            a.pixelW = static_cast<uint32_t>(parsed[2]);
            a.pixelH = static_cast<uint32_t>(parsed[3]);
            a.pixelMode = true;
            return true;
        }

        using Apply = bool (*)(Args&, std::wstring_view value, std::wstring& error);

        struct FlagSpec
        {
            std::wstring_view name;
            uint32_t modes;      // where the flag is read
            bool     takesValue;
            Apply    apply;
        };

        // Every flag, the modes that read it, and how its value is checked.
        const FlagSpec cFlags[] =
        {
            { L"--graph", cGraphModes, true, [](Args& a, std::wstring_view v, std::wstring&) { a.graphPath = v; return true; } },
            { L"--node", cImage | cPixels | cVideo | cScript, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                auto id = ParseU32(v);
                if (!id) { e = Got(L"--node", L"a numeric node id", v); return false; }
                a.nodeId = *id; a.hasNodeId = true; return true; } },
            { L"--output", cImage | cPixels, true, [](Args& a, std::wstring_view v, std::wstring&) { a.outputPath = v; return true; } },
            { L"--width", cImage | cScript, true, [](Args& a, std::wstring_view v, std::wstring& e) { return SetPositiveU32(L"--width", v, a.width, e); } },
            { L"--height", cImage | cScript, true, [](Args& a, std::wstring_view v, std::wstring& e) { return SetPositiveU32(L"--height", v, a.height, e); } },
            { L"--adapter", cGraphModes, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                if (v == L"warp") a.useWarp = true;
                else if (v == L"default") a.useWarp = false;
                else { e = Got(L"--adapter", L"'warp' or 'default'", v); return false; }
                return true; } },
            { L"--gpu-timing", cScript, false, [](Args& a, std::wstring_view, std::wstring&) { a.gpuTiming = true; return true; } },
            { L"--effects-dir", cGraphModes, true, [](Args& a, std::wstring_view v, std::wstring&) { a.effectsDirs.emplace_back(v); return true; } },
            { L"--skip-unneeded-readback", cGraphModes, false, [](Args& a, std::wstring_view, std::wstring&) { a.skipUnneededReadback = true; return true; } },
            { L"--input-peak-nits", cImage, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                a.toneMapExplicit = true; return SetPositiveFloat(L"--input-peak-nits", v, a.inputPeakNits, e); } },
            { L"--output-peak-nits", cImage, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                a.toneMapExplicit = true; return SetPositiveFloat(L"--output-peak-nits", v, a.outputPeakNits, e); } },
            { L"--no-tonemap", cImage, false, [](Args& a, std::wstring_view, std::wstring&) { a.skipToneMap = true; return true; } },
            { L"--display-profile", cGraphModes, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                if (!DisplayPresetByName(v)) { e = L"unknown --display-profile '" + std::wstring(v) + L"'. Expected one of: " + cDisplayPresetNames; return false; }
                a.displayProfile = v; return true; } },
            { L"--display-sdr-white", cGraphModes, true, [](Args& a, std::wstring_view v, std::wstring& e) { return SetPositiveFloat(L"--display-sdr-white", v, a.displaySdrWhite, e); } },
            { L"--display-peak-nits", cGraphModes, true, [](Args& a, std::wstring_view v, std::wstring& e) { return SetPositiveFloat(L"--display-peak-nits", v, a.displayPeakNits, e); } },
            { L"--pixels", cPixels, true, [](Args& a, std::wstring_view v, std::wstring& e) { return ParsePixelRegion(v, a, e); } },
            { L"--script", cScript, true, [](Args& a, std::wstring_view v, std::wstring&) { a.scriptPath = v; return true; } },
            { L"--script-output", cScript, true, [](Args& a, std::wstring_view v, std::wstring&) { a.scriptOutputPath = v; return true; } },
            { L"--mcp-session", cSession, false, [](Args& a, std::wstring_view, std::wstring&) { a.mcpSessionMode = true; return true; } },
            { L"--session-id", cSession, true, [](Args& a, std::wstring_view v, std::wstring&) { a.sessionId = v; return true; } },
            { L"--session-label", cSession, true, [](Args& a, std::wstring_view v, std::wstring&) { a.sessionLabel = v; return true; } },
            { L"--pipe", cSession, true, [](Args& a, std::wstring_view v, std::wstring&) { a.pipeName = v; return true; } },
            { L"--reap-shader-cache", cCache, false, [](Args& a, std::wstring_view, std::wstring&) { a.reapShaderCache = true; return true; } },
            { L"--clear-shader-cache", cCache, false, [](Args& a, std::wstring_view, std::wstring&) { a.clearShaderCache = true; return true; } },
            { L"--reap-shader-cache-stale-sec", cCache, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                auto seconds = ParseUnsigned(v);
                if (!seconds) { e = Got(L"--reap-shader-cache-stale-sec", L"a whole number of seconds", v); return false; }
                a.reapStaleSec = *seconds; return true; } },
            { L"--enable-gpu-bindings", cGraphModes, false, [](Args& a, std::wstring_view, std::wstring&) { a.enableGpuBindings = true; return true; } },
            { L"--disable-gpu-bindings", cGraphModes, false, [](Args& a, std::wstring_view, std::wstring&) { a.enableGpuBindings = false; return true; } },
            { L"--video", cVideo, true, [](Args& a, std::wstring_view v, std::wstring&) { a.videoPath = v; return true; } },
            { L"--format", cVideo, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                if (!CheckVideoOption(v, L"", L"", e)) return false;
                a.videoFormat = v; return true; } },
            { L"--codec", cVideo, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                if (!CheckVideoOption(L"", v, L"", e)) return false;
                a.videoCodec = v; return true; } },
            { L"--mastering", cVideo, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                if (!CheckVideoOption(L"", L"", v, e)) return false;
                a.videoMastering = v; return true; } },
            { L"--fps", cVideo, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                auto fps = ParseFinite(v);
                if (!fps || *fps <= 0.0) { e = Got(L"--fps", L"a positive frame rate", v); return false; }
                a.videoFps = *fps; return true; } },
            { L"--start", cVideo, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                auto start = ParseFinite(v);
                if (!start) { e = Got(L"--start", L"a time in seconds", v); return false; }
                a.videoStart = *start; return true; } },
            { L"--duration", cVideo, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                auto duration = ParseFinite(v);
                if (!duration || *duration <= 0.0) { e = Got(L"--duration", L"a positive time in seconds", v); return false; }
                a.videoDuration = *duration; return true; } },
            { L"--crf", cVideo, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                auto crf = ParseU32(v);
                if (!crf || *crf > static_cast<uint32_t>(INT32_MAX)) { e = Got(L"--crf", L"a non-negative integer", v); return false; }
                a.videoCrf = static_cast<int>(*crf); return true; } },
            { L"--preset", cVideo, true, [](Args& a, std::wstring_view v, std::wstring&) { a.videoPreset = v; return true; } },
            { L"--lossless", cVideo, false, [](Args& a, std::wstring_view, std::wstring&) { a.videoLossless = true; return true; } },
            { L"--max-cll", cVideo, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                const size_t comma = v.find(L',');
                auto cll = comma == std::wstring_view::npos ? std::nullopt : ParseU32(v.substr(0, comma));
                auto fall = comma == std::wstring_view::npos ? std::nullopt : ParseU32(v.substr(comma + 1));
                if (!cll || !fall) { e = Got(L"--max-cll", L"'CLL,FALL' in whole nits", v); return false; }
                a.videoMaxCll = *cll; a.videoMaxFall = *fall; return true; } },
            { L"--ffmpeg", cVideo, true, [](Args& a, std::wstring_view v, std::wstring&) { a.ffmpegPath = v; return true; } },
            { L"--time", cImage | cPixels | cScript | cSession, true, [](Args& a, std::wstring_view v, std::wstring& e) {
                auto time = ParseFinite(v);
                if (!time) { e = Got(L"--time", L"a time in seconds", v); return false; }
                a.time = *time; return true; } },
        };

        const FlagSpec* FindFlag(std::wstring_view name)
        {
            for (const auto& flag : cFlags)
                if (flag.name == name) return &flag;
            return nullptr;
        }

        Mode SelectMode(const Args& a)
        {
            if (a.reapShaderCache || a.clearShaderCache) return Mode::ShaderCache;
            if (a.mcpSessionMode) return Mode::McpSession;
            if (!a.scriptPath.empty()) return Mode::Script;
            if (!a.videoPath.empty()) return Mode::Video;
            if (a.pixelMode) return Mode::Pixels;
            return Mode::Image;
        }

        // Empty when the mode has what it needs.
        std::wstring MissingRequired(Mode mode, const Args& a)
        {
            switch (mode)
            {
            case Mode::ShaderCache:
                return {};
            case Mode::Script:
            case Mode::McpSession:
                return a.graphPath.empty() ? L"--graph is required (script / mcp-session mode)" : L"";
            case Mode::Video:
                return (a.graphPath.empty() || !a.hasNodeId) ? L"--video needs --graph and --node" : L"";
            case Mode::Image:
            case Mode::Pixels:
                return (a.graphPath.empty() || a.outputPath.empty() || !a.hasNodeId)
                    ? L"--graph, --node, and --output are required" : L"";
            }
            return {};
        }
    }

    ParseResult ParseArgs(int argc, const wchar_t* const argv[], Args& out)
    {
        ParseResult result;
        std::vector<const FlagSpec*> seen;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            if (arg == L"--help" || arg == L"-h" || arg == L"-?")
            {
                result.outcome = ParseOutcome::Help;
                return result;
            }
            const FlagSpec* flag = FindFlag(arg);
            if (!flag)
            {
                result.outcome = ParseOutcome::Error;
                result.error = L"unknown argument '" + std::wstring(arg) + L"'";
                return result;
            }
            std::wstring_view value;
            if (flag->takesValue)
            {
                if (i + 1 >= argc)
                {
                    result.outcome = ParseOutcome::Error;
                    result.error = std::wstring(flag->name) + L" requires an argument";
                    return result;
                }
                value = argv[++i];
            }
            if (!flag->apply(out, value, result.error))
            {
                result.outcome = ParseOutcome::Error;
                return result;
            }
            if (std::find(seen.begin(), seen.end(), flag) == seen.end())
                seen.push_back(flag);
        }

        result.mode = SelectMode(out);
        result.error = MissingRequired(result.mode, out);
        if (!result.error.empty())
        {
            result.outcome = ParseOutcome::Error;
            return result;
        }
        for (const FlagSpec* flag : seen)
            if ((flag->modes & Bit(result.mode)) == 0)
                result.warnings.push_back(std::wstring(flag->name) + L" has no effect in " +
                                          ModeName(result.mode) + L" mode");
        return result;
    }

    void PrintUsage(const wchar_t* exeName, std::wstring_view scriptOps)
    {
        const std::wstring ops(scriptOps);
        std::wprintf(
L"Usage: %ls --graph PATH --node ID --output IMAGE_PATH [options]\n"
L"\n"
L"Required:\n"
L"  --graph PATH    .effectgraph archive (ZIP; embedded media supported)\n"
L"                  or a bare graph JSON file\n"
L"  --node ID       Numeric node id to render\n"
L"  --output PATH   Output image. The extension picks the encoder:\n"
L"                    .jxr/.wdp  JPEG XR, 64bpp RGBA half, lossless, HDR\n"
L"                               preserved (implies --no-tonemap unless a\n"
L"                               peak is named explicitly)\n"
L"                    otherwise  PNG, 8-bit sRGB, clamped to [0,1]\n"
L"\n"
L"Options:\n"
L"  --width N                Output width (default: 1024)\n"
L"  --height N               Output height (default: 1024)\n"
L"  --adapter X              'warp' or 'default' (default: default)\n"
L"  --effects-dir DIR        Register user effects from DIR (every *.json saved\n"
L"                           graph in it; see docs/effects/user-effects.md).\n"
L"                           Repeatable. Load failures print [ShaderLab] lines.\n"
L"  --gpu-timing             Sample REAL GPU time via D3D11 timestamp queries\n"
L"                           for the gpu-bench script op. Off by default: it\n"
L"                           Flushes D2D, which perturbs the frame measured.\n"
L"  --input-peak-nits N      D2D HdrToneMap input peak (default: 1000)\n"
L"  --output-peak-nits N     D2D HdrToneMap output peak (default: 80 = SDR)\n"
L"  --no-tonemap             Skip HdrToneMap, raw scRGB -> sRGB clamp\n"
L"  --time T                 Set every Clock to timeline time T before the\n"
L"                           first frame (image, pixels, script and\n"
L"                           mcp-session modes; --video uses --start).\n"
L"\n"
L"Display-profile pin (reproducible golden images):\n"
L"  --display-profile NAME   Pin the display profile a Working Space node\n"
L"                           reports instead of reading the live monitor.\n"
L"                           One of: %ls.\n"
L"  --display-sdr-white N    Override SDR white level (nits) on top of the\n"
L"                           preset, or on top of the live profile when no\n"
L"                           --display-profile is given.\n"
L"  --display-peak-nits N    Override display peak luminance (nits); same\n"
L"                           layering as --display-sdr-white.\n"
L"\n"
L"Pixel-region readback mode (raw FP32 RGBA, no tonemap):\n"
L"  --pixels x,y,w,h         Read a region from the node's output as\n"
L"                           FP32 RGBA, write to --output. Output\n"
L"                           extension .bin/.raw -> packed binary\n"
L"                           (W,H header as two uint32, then floats);\n"
L"                           .csv -> text 'x,y,r,g,b,a' rows.\n"
L"                           Region is clipped to image bounds.\n"
L"\n"
L"Video export (fixed timeline, encoded by an external ffmpeg):\n"
L"  --video PATH             Render --node over time and write a video\n"
L"                           (.mp4 / .mkv / .mov). Needs --graph and --node;\n"
L"                           --output is not used.\n"
L"  --format hdr10|sdr       hdr10 (default): BT.2020 + PQ, 10-bit 4:2:0,\n"
L"                           absolute nits (scRGB 1.0 = 80 nits).\n"
L"                           sdr: BT.709, sRGB curve on [0,1], 8-bit.\n"
L"                           No tone mapping: export the node you want.\n"
L"  --codec hevc|av1|h264    Default hevc for hdr10, h264 for sdr.\n"
L"  --fps N                  Frame rate (default 60; 29.97 etc. accepted).\n"
L"  --start S                Export-timeline start in seconds (default 0).\n"
L"  --duration S             Length in seconds (default: the longest\n"
L"                           Clock's Start..Stop span at its Speed).\n"
L"  --crf N / --preset P     Encoder quality / speed (per-codec defaults).\n"
L"  --lossless               Lossless HEVC / H.264 (for verification).\n"
L"  --mastering M            HDR10 static metadata: p3-1000 (default,\n"
L"                           P3-D65 1000/0.005-nit display), working-space\n"
L"                           (the graph's Working Space node), or none.\n"
L"                           MaxCLL/MaxFALL are MEASURED from the frames\n"
L"                           (one extra render-only pass) unless given:\n"
L"  --max-cll CLL,FALL       Use these instead of measuring.\n"
L"  --ffmpeg PATH            ffmpeg.exe (or its folder). Default: PATH.\n"
L"\n"
L"Script batch mode (parameter sweeps in one process):\n"
L"  --script PATH            JSON file with an array of MCP ops to run\n"
L"                           against the loaded graph. Each op is either\n"
L"                           {method,path,body} (raw route shape) or\n"
L"                           {op,...} shorthand, one of:\n"
L"                             %ls.\n"
L"                           --node, --width and --height are gpu-bench's\n"
L"                           defaults; --output is not used.\n"
L"  --script-output PATH     Write the JSON response document here\n"
L"                           (default: stdout).\n"
L"\n"
L"MCP session mode:\n"
L"  --mcp-session            Register with the broker hub as a session and\n"
L"                           serve MCP requests until the process is killed.\n"
L"  --session-id ID          Session id (default: a fresh GUID).\n"
L"  --session-label TEXT     Name list_sessions shows (default: headless PID).\n"
L"  --pipe NAME              Broker pipe base (dev / CI isolation).\n"
L"\n"
L"Bytecode cache management (runs without a graph or device):\n"
L"  --reap-shader-cache      Delete cached shader bytecode (.cso) under\n"
L"                           %%LOCALAPPDATA%%\\ShaderLab\\bytecode\\ that has not\n"
L"                           been accessed within --reap-shader-cache-stale-sec\n"
L"                           seconds. Reports filesDeleted/bytesFreed JSON.\n"
L"  --clear-shader-cache     Delete every cached shader bytecode entry.\n"
L"  --reap-shader-cache-stale-sec N\n"
L"                           Threshold in seconds for --reap-shader-cache\n"
L"                           (default: 7776000 = 90 days).\n"
L"\n"
L"GPU bindings (default: Performance::DefaultEvaluatorOptions, on):\n"
L"  --enable-gpu-bindings    Force GPU bindings on. Upstream-effect SRVs go\n"
L"                           straight to D3D11 compute consumers' t-slots;\n"
L"                           pixel shader consumers receive them as a 1-row\n"
L"                           analysis texture on an extra input pin (lane 3).\n"
L"                           Either way the CPU readback is skipped.\n"
L"  --disable-gpu-bindings   Force GPU bindings off; every binding goes\n"
L"                           through CPU readback.\n"
L"  --skip-unneeded-readback Skip CPU readback nothing reads, as the GUI\n"
L"                           does (for perf measurements).\n"
L"\n"
L"A flag the selected mode does not read is reported on stderr.\n"
L"Exit codes are listed in docs/hosts/headless.md.\n",
            exeName, cDisplayPresetNames, ops.c_str());
    }
}
