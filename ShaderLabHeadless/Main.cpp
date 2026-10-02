// ShaderLabHeadless — console host for the ShaderLab engine
// ============================================================
//
// Render any node of an .effectgraph (JSON) to a PNG file with no
// SwapChainPanel / WinUI dependency. Phase 7 deliverable. The Phase 7
// spike (Tests/TestRunner.cpp::TestHeadlessReadback, commit 9f14401)
// proved a single ID3D11Device + ID2D1DeviceContext is enough to
// evaluate a graph and read pixels back; this binary packages that
// capability as a CLI so the empirical fidelity loop (Working Space +
// Delta E + Luminance Statistics) can run without a logged-in user.
//
// Usage:
//     ShaderLabHeadless --graph PATH --node ID --output PNG_PATH [options]
//
// Required arguments:
//     --graph PATH    .effectgraph archive or bare graph JSON. A ZIP archive
//                     has its embedded media/ extracted to a temp directory
//                     and "media://" tokens rewritten, then cleaned up on exit.
//     --node ID       Numeric node id from the graph to render
//     --output PATH   Output image path. The extension picks the encoder:
//                     .jxr / .wdp -> JPEG XR, 64bpp RGBA half, lossless, HDR
//                     preserved (no clamp, no transfer encoding);
//                     anything else -> PNG, 8-bit sRGB, clamped to [0,1].
//
// Options:
//     --width N       Output width in pixels (default: 1024)
//     --height N      Output height in pixels (default: 1024)
//     --adapter X     'warp' or 'default' (default: 'default'; CI uses warp)
//     --pixels        FP32 RGBA readback to stdout instead of a PNG
//
// Batch / MCP modes (mutually exclusive with a plain --output render):
//     --script PATH --script-output PATH
//                     JSON batch script of MCP-shaped ops; results written as JSON.
//     --mcp-session [--pipe NAME] [--session-id ID] [--session-label TEXT]
//                     Register with the broker hub as an MCP session and serve
//                     requests until terminated.
//
// Other flags: --input-peak-nits / --output-peak-nits, --no-tonemap,
// --enable-gpu-bindings / --disable-gpu-bindings, --reap-shader-cache
// [--reap-shader-cache-stale-sec N], --clear-shader-cache, --help.
//
// Exit code: 0 on success, non-zero on any failure.
//
// What it DOES prove: the engine, graph evaluator, custom-effect cache,
// and pixel readback path all work without any UI thread or swap chain.

#include "pch_engine.h"
#include "EngineExport.h"
#include "Graph/EffectGraph.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/GpuTimer.h"
#include "Rendering/DisplayMonitor.h"
#include "Rendering/WorkingSpaceSync.h"
#include "Effects/EffectRegistry.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/BytecodeCache.h"
#include "Effects/Performance.h"
#include "Rendering/PixelReadback.h"
#include "Rendering/PipelineFormat.h"
#include "Rendering/EffectGraphFile.h"
#include "Rendering/VideoExport.h"

// XMConvertFloatToHalf for the JXR (64bpp RGBA half) encode path.
#include <DirectXPackedVector.h>
#include "Engine/Mcp/McpRouter.h"
#include "Engine/Mcp/McpJsonRpc.h"
#include "Engine/Mcp/McpSessionClient.h"
#include "Engine/Mcp/EngineMcpRoutes.h"

#include <winrt/Windows.Data.Json.h>
#include <wincodec.h>
#include <shlobj.h>
#include <KnownFolders.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    struct Args
    {
        std::wstring graphPath;
        uint32_t     nodeId{ 0 };
        bool         hasNodeId{ false };
        std::wstring outputPath;
        uint32_t     width{ 1024 };
        uint32_t     height{ 1024 };
        bool         useWarp{ false };
        // Enable D3D11 timestamp sampling so the gpu-bench script op can
        // report real GPU execution time. Off by default because closing a
        // GPU span around D2D work needs a Flush, which perturbs the frame.
        bool         gpuTiming{ false };
        // Model the GUI's readback policy. MainWindow enables
        // SetSkipUnneededCpuReadbackEnabled(true); headless deliberately
        // does not, because probing wants fresh analysis fields every
        // frame. That difference is fine for correctness work and WRONG
        // for perf work -- benchmarking headless without it measures a
        // GPU->CPU round trip the real app does not perform.
        bool         skipUnneededReadback{ false };
        // D2D HdrToneMap parameters: InputMaxLuminance is the peak nit
        // value of the source content; OutputMaxLuminance is the peak
        // nit value the SDR PNG can represent (80 == scRGB 1.0). The
        // tone mapper saturates inputs above 1000 nits to 80 nits SDR
        // by default, which is what we want for visual inspection of
        // HDR test patterns rendered through the engine.
        float        inputPeakNits{ 1000.0f };
        float        outputPeakNits{ 80.0f };
        // Skip the tone map entirely (raw scRGB -> sRGB clamp). Useful
        // when the graph already produced SDR-range output and we
        // don't want HdrToneMap's mid-tone lift muddying the result.
        bool         skipToneMap{ false };
        // Set when --input-peak-nits / --output-peak-nits was passed. A JXR
        // output skips the tone map by default (see RunRender), but an
        // explicit peak request means the caller wants tone mapping and is
        // choosing the target peak -- e.g. 4000-nit content into a 1000-nit
        // HDR deliverable -- so it must win over that default.
        bool         toneMapExplicit{ false };

        // --effects-dir: user effect libraries to register (repeatable).
        // Headless never reads the GUI's default folder, so a run does not
        // depend on what happens to be installed.
        std::vector<std::wstring> effectsDirs;

        // Display-profile pin. Without one, every headless mode binds to
        // the LIVE primary monitor (InitializeForPrimaryMonitor), so a
        // graph containing a Working Space node renders differently on
        // two machines -- and differently on the SAME machine after the
        // user drags the Windows "SDR content brightness" slider, which
        // moves sdrWhiteLevelNits. That is correct for interactive probing
        // and wrong for golden images: it makes a byte comparison against
        // a checked-in reference a test of the tester's display settings.
        // --display-profile picks a fully-determined preset; the two
        // override flags pin just the luminance anchors on top of whatever
        // base is in effect (the live profile when no preset is named).
        std::wstring displayProfile;            // preset id; empty -> live
        float        displaySdrWhite{ 0.0f };   // >0 -> override
        float        displayPeakNits{ 0.0f };   // >0 -> override
        // FP32 RGBA pixel-region readback (alternate output mode).
        // When set, --output is interpreted as a raw FP32 binary blob
        // (extension .bin / .raw) or a CSV file (extension .csv). No
        // PNG / tonemap path is used. Designed for MCP-driven full-
        // accuracy sampling and quantitative analysis.
        bool         pixelMode{ false };
        int32_t      pixelX{ 0 };
        int32_t      pixelY{ 0 };
        uint32_t     pixelW{ 1 };
        uint32_t     pixelH{ 1 };

        // Script batch mode. When set, the host loads a graph and then
        // walks an array of MCP-style operations (set-property, image-
        // stats, capture-node, pixel-region, ...) writing per-step
        // responses out as a JSON document. Designed for parameter
        // sweeps where the agent wants 50+ engine queries per session
        // without paying HTTP round-trip overhead each one.
        std::wstring scriptPath;
        std::wstring scriptOutputPath;  // empty -> stdout

        // MCP session mode (stdio-migration Step 6). Registers with the
        // broker hub as a session so a shim-fronted MCP client can select
        // it with use_session. --session-id is a persisted per-window
        // GUID (a fresh one is generated when omitted); --session-label
        // is the human name list_sessions surfaces; --pipe overrides the
        // broker pipe base (dev/CI isolation).
        bool         mcpSessionMode{ false };
        std::wstring sessionId;
        std::wstring sessionLabel;
        std::wstring pipeName;

        // p8-cache-reaper: bytecode-cache management modes. When set,
        // the headless host runs the requested op then exits without
        // loading a graph or starting MCP. Useful for CI / cleanup.
        bool          reapShaderCache{ false };
        bool          clearShaderCache{ false };
        uint64_t      reapStaleSec{ 90ull * 24 * 60 * 60 };  // 90 days default

        // p8-feature-flag: opt in to the Phase 8 GPU-binding fast path
        // for the duration of this run. Persists no state; affects only
        // this process. Defaults to engine default (off for v1.6).
        std::optional<bool> enableGpuBindings;

        // Video export (--video PATH). Frames are rendered on a fixed
        // timeline and encoded by an external ffmpeg; see
        // Rendering/VideoExport.h for the pipeline and its decisions.
        std::wstring videoPath;
        std::wstring videoFormat{ L"hdr10" };   // hdr10 | sdr
        std::wstring videoCodec;                // hevc | av1 | h264; empty -> per-format default
        double       videoFps{ 60.0 };
        double       videoStart{ 0.0 };
        std::optional<double> videoDuration;    // unset -> longest Clock
        int          videoCrf{ -1 };
        std::wstring videoPreset;
        bool         videoLossless{ false };
        std::wstring videoMastering{ L"p3-1000" };  // p3-1000 | working-space | none
        std::optional<uint32_t> videoMaxCll;
        std::optional<uint32_t> videoMaxFall;
        std::wstring ffmpegPath;                // empty -> search PATH

        // --time T: set every Clock to export-timeline time T before an
        // --output / --pixels render, so one frame of an animation renders
        // exactly as --video would render it (and can be compared with it).
        std::optional<double> time;
    };

    void PrintUsage(const wchar_t* exeName)
    {
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
L"                           and enable the gpu-bench script op. Off by\n"
L"                           default: it Flushes D2D, which perturbs the\n"
L"                           very frame being measured.\n"
L"  --input-peak-nits N      D2D HdrToneMap input peak (default: 1000)\n"
L"  --output-peak-nits N     D2D HdrToneMap output peak (default: 80 = SDR)\n"
L"  --no-tonemap             Skip HdrToneMap, raw scRGB -> sRGB clamp\n"
L"\n"
L"Display-profile pin (reproducible golden images):\n"
L"  --display-profile NAME   Pin the display profile a Working Space node\n"
L"                           reports instead of reading the live monitor.\n"
L"                           One of: srgb-sdr, srgb-270, p3-600, p3-1000,\n"
L"                           bt2020-1000, bt2020-4000, adobergb.\n"
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
L"                           Designed for MCP-driven full-accuracy\n"
L"                           sampling. Region is clipped to image bounds.\n"
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
L"  --time T                 For --output / --pixels: set every Clock to\n"
L"                           timeline time T first (one frame of --video).\n"
L"\n"
L"Script batch mode (parameter sweeps without HTTP round-trips):\n"
L"  --script PATH            JSON file with an array of MCP ops to run\n"
L"                           against the loaded graph. Each op is either\n"
L"                           {method,path,body} (raw HTTP shape) or\n"
L"                           {op,...} shorthand (set-property, pixel-region,\n"
L"                           pixel-region, capture-node, render).\n"
L"                           In script mode --node and --output are not\n"
L"                           required; instead provide --script-output for\n"
L"                           the per-step response document.\n"
L"  --script-output PATH     Write the JSON response document here\n"
L"                           (default: stdout).\n"
L"\n"
L"Bytecode cache management (Phase 8 cache reaper):\n"
L"  --reap-shader-cache      Delete cached shader bytecode (.cso) under\n"
L"                           %%LOCALAPPDATA%%\\ShaderLab\\bytecode\\ that has not\n"
L"                           been accessed within --reap-shader-cache-stale-sec\n"
L"                           seconds. Reports filesDeleted/bytesFreed JSON.\n"
L"                           Exits without loading a graph.\n"
L"  --clear-shader-cache     Delete every cached shader bytecode entry.\n"
L"                           Exits without loading a graph.\n"
L"  --reap-shader-cache-stale-sec N\n"
L"                           Threshold in seconds for --reap-shader-cache\n"
L"                           (default: 7776000 = 90 days).\n"
L"\n"
L"GPU-binding fast path (Phase 8 feature flag, default ON in v1.6):\n"
L"  --enable-gpu-bindings    Force GPU bindings on (default).\n"
L"                           The evaluator routes upstream-effect SRVs\n"
L"                           directly to D3D11 compute consumers'\n"
L"                           t-slots; pixel shader consumers receive\n"
L"                           them as a 1-row analysis texture on an\n"
L"                           extra input pin (lane 3). Either way the\n"
L"                           CPU readback is skipped.\n"
L"  --disable-gpu-bindings   Force GPU bindings off; every binding goes\n"
L"                           through CPU readback (the pre-v1.6 path).\n",
            exeName);
    }

    // Bare-bones argv parsing. Each flag takes one positional argument.
    // Unknown flags fail closed with a usage hint so typos don't silently
    // produce wrong output.

    // --display-profile preset table. Kept next to ParseArgs so an unknown
    // name is a usage error (exit 1, no GPU init) rather than a silent
    // fallback to the live display -- a silent fallback would defeat the
    // whole point of the flag, which is to make a render independent of
    // the machine it runs on.
    constexpr const wchar_t* kDisplayPresetNames =
        L"srgb-sdr, srgb-270, p3-600, p3-1000, bt2020-1000, bt2020-4000, adobergb";

    std::optional<ShaderLab::Rendering::DisplayProfile>
    DisplayPresetByName(std::wstring_view name)
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

    // Apply the --display-* pin to a freshly-initialized DisplayMonitor.
    // No-op when no pin was requested, so the default stays "track the
    // live monitor" for interactive probing.
    //
    // Layering: --display-profile picks a fully-determined base; the two
    // override flags then pin the luminance anchors on top of it, or on
    // top of the LIVE profile when no preset was named (so a caller who
    // pins only SDR white keeps the real panel's peak and primaries).
    //
    // The preset factories already call StampSimulatedColorMode, and a
    // live profile is coherent by construction, so nothing here re-derives
    // activeColorMode / the *Supported flags -- overriding a luminance
    // anchor does not change which advanced-color kind is active.
    void ApplyDisplayPin(const Args& args, ShaderLab::Rendering::DisplayMonitor& monitor)
    {
        const bool wantsPreset   = !args.displayProfile.empty();
        const bool wantsOverride = args.displaySdrWhite > 0.0f || args.displayPeakNits > 0.0f;
        if (!wantsPreset && !wantsOverride) return;

        // ParseArgs already rejected an unknown preset name, so the
        // value_or here can only fire if that validation is bypassed.
        auto p = wantsPreset
            ? DisplayPresetByName(args.displayProfile).value_or(monitor.LiveProfile())
            : monitor.LiveProfile();

        if (args.displaySdrWhite > 0.0f)
            p.caps.sdrWhiteLevelNits = args.displaySdrWhite;
        if (args.displayPeakNits > 0.0f)
        {
            p.caps.maxLuminanceNits = args.displayPeakNits;
            // Presets keep maxFullFrame <= peak; preserve that rather than
            // leaving a full-frame cap above the newly-lowered peak.
            if (p.caps.maxFullFrameLuminanceNits > args.displayPeakNits)
                p.caps.maxFullFrameLuminanceNits = args.displayPeakNits;
        }

        p.isSimulated = true;
        p.profileName = p.profileName.empty()
            ? std::wstring(L"CLI display pin")
            : p.profileName + L" (CLI pin)";
        monitor.SetSimulatedProfile(p);
    }

    bool ParseArgs(int argc, wchar_t* argv[], Args& out)
    {
        for (int i = 1; i < argc; ++i)
        {
            std::wstring_view a = argv[i];
            auto needNext = [&](const wchar_t* name) -> wchar_t* {
                if (i + 1 >= argc) {
                    std::wprintf(L"ERROR: %ls requires an argument\n", name);
                    return nullptr;
                }
                return argv[++i];
            };
            if (a == L"--graph")        { auto v = needNext(L"--graph"); if (!v) return false; out.graphPath = v; }
            else if (a == L"--node")    { auto v = needNext(L"--node"); if (!v) return false; out.nodeId = static_cast<uint32_t>(std::wcstoul(v, nullptr, 10)); out.hasNodeId = true; }
            else if (a == L"--output")  { auto v = needNext(L"--output"); if (!v) return false; out.outputPath = v; }
            else if (a == L"--width")   { auto v = needNext(L"--width"); if (!v) return false; out.width = static_cast<uint32_t>(std::wcstoul(v, nullptr, 10)); }
            else if (a == L"--height")  { auto v = needNext(L"--height"); if (!v) return false; out.height = static_cast<uint32_t>(std::wcstoul(v, nullptr, 10)); }
            else if (a == L"--gpu-timing") { out.gpuTiming = true; }
            else if (a == L"--effects-dir") { auto v = needNext(L"--effects-dir"); if (!v) return false; out.effectsDirs.push_back(v); }
            else if (a == L"--skip-unneeded-readback") { out.skipUnneededReadback = true; }
            else if (a == L"--adapter") { auto v = needNext(L"--adapter"); if (!v) return false; out.useWarp = (std::wstring_view{v} == L"warp"); }
            else if (a == L"--input-peak-nits")  { auto v = needNext(L"--input-peak-nits"); if (!v) return false; out.inputPeakNits = static_cast<float>(std::wcstod(v, nullptr)); out.toneMapExplicit = true; }
            else if (a == L"--output-peak-nits") { auto v = needNext(L"--output-peak-nits"); if (!v) return false; out.outputPeakNits = static_cast<float>(std::wcstod(v, nullptr)); out.toneMapExplicit = true; }
            else if (a == L"--no-tonemap") { out.skipToneMap = true; }
            else if (a == L"--display-profile")   { auto v = needNext(L"--display-profile"); if (!v) return false;
                                                   if (!DisplayPresetByName(v).has_value()) {
                                                       std::wprintf(L"ERROR: unknown --display-profile '%ls'. Expected one of: %ls\n", v, kDisplayPresetNames);
                                                       return false;
                                                   }
                                                   out.displayProfile = v; }
            else if (a == L"--display-sdr-white") { auto v = needNext(L"--display-sdr-white"); if (!v) return false; out.displaySdrWhite = static_cast<float>(std::wcstod(v, nullptr)); }
            else if (a == L"--display-peak-nits") { auto v = needNext(L"--display-peak-nits"); if (!v) return false; out.displayPeakNits = static_cast<float>(std::wcstod(v, nullptr)); }
            else if (a == L"--pixels")
            {
                auto v = needNext(L"--pixels"); if (!v) return false;
                // Parse "x,y,w,h" into the four fields.
                wchar_t* p = const_cast<wchar_t*>(v);
                int32_t  parsed[4] = { 0, 0, 0, 0 };
                for (int idx = 0; idx < 4; ++idx)
                {
                    wchar_t* end = nullptr;
                    long val = std::wcstol(p, &end, 10);
                    if (end == p) {
                        std::wprintf(L"ERROR: --pixels requires 'x,y,w,h' (got '%ls')\n", v);
                        return false;
                    }
                    parsed[idx] = static_cast<int32_t>(val);
                    p = end;
                    if (*p == L',') ++p;
                    else if (idx < 3) {
                        std::wprintf(L"ERROR: --pixels requires 4 comma-separated values (got '%ls')\n", v);
                        return false;
                    }
                }
                if (parsed[2] <= 0 || parsed[3] <= 0) {
                    std::wprintf(L"ERROR: --pixels w and h must be positive (got %d,%d)\n", parsed[2], parsed[3]);
                    return false;
                }
                out.pixelX = parsed[0];
                out.pixelY = parsed[1];
                out.pixelW = static_cast<uint32_t>(parsed[2]);
                out.pixelH = static_cast<uint32_t>(parsed[3]);
                out.pixelMode = true;
            }
            else if (a == L"--script")        { auto v = needNext(L"--script"); if (!v) return false; out.scriptPath = v; }
            else if (a == L"--script-output") { auto v = needNext(L"--script-output"); if (!v) return false; out.scriptOutputPath = v; }
            else if (a == L"--mcp-session")   { out.mcpSessionMode = true; }
            else if (a == L"--session-id")    { auto v = needNext(L"--session-id"); if (!v) return false; out.sessionId = v; }
            else if (a == L"--session-label") { auto v = needNext(L"--session-label"); if (!v) return false; out.sessionLabel = v; }
            else if (a == L"--pipe")          { auto v = needNext(L"--pipe"); if (!v) return false; out.pipeName = v; }
            else if (a == L"--reap-shader-cache")   { out.reapShaderCache = true; }
            else if (a == L"--clear-shader-cache")  { out.clearShaderCache = true; }
            else if (a == L"--reap-shader-cache-stale-sec")
            {
                auto v = needNext(L"--reap-shader-cache-stale-sec"); if (!v) return false;
                out.reapStaleSec = static_cast<uint64_t>(std::wcstoull(v, nullptr, 10));
            }
            else if (a == L"--enable-gpu-bindings")  { out.enableGpuBindings = true;  }
            else if (a == L"--disable-gpu-bindings") { out.enableGpuBindings = false; }
            else if (a == L"--video")    { auto v = needNext(L"--video"); if (!v) return false; out.videoPath = v; }
            else if (a == L"--format")   { auto v = needNext(L"--format"); if (!v) return false; out.videoFormat = v; }
            else if (a == L"--codec")    { auto v = needNext(L"--codec"); if (!v) return false; out.videoCodec = v; }
            else if (a == L"--fps")      { auto v = needNext(L"--fps"); if (!v) return false; out.videoFps = std::wcstod(v, nullptr); }
            else if (a == L"--start")    { auto v = needNext(L"--start"); if (!v) return false; out.videoStart = std::wcstod(v, nullptr); }
            else if (a == L"--duration") { auto v = needNext(L"--duration"); if (!v) return false; out.videoDuration = std::wcstod(v, nullptr); }
            else if (a == L"--crf")      { auto v = needNext(L"--crf"); if (!v) return false; out.videoCrf = static_cast<int>(std::wcstol(v, nullptr, 10)); }
            else if (a == L"--preset")   { auto v = needNext(L"--preset"); if (!v) return false; out.videoPreset = v; }
            else if (a == L"--lossless") { out.videoLossless = true; }
            else if (a == L"--mastering"){ auto v = needNext(L"--mastering"); if (!v) return false; out.videoMastering = v; }
            else if (a == L"--max-cll")
            {
                auto v = needNext(L"--max-cll"); if (!v) return false;
                unsigned cll = 0, fall = 0;
                if (swscanf_s(v, L"%u,%u", &cll, &fall) != 2) {
                    std::wprintf(L"ERROR: --max-cll requires 'CLL,FALL' (got '%ls')\n", v);
                    return false;
                }
                out.videoMaxCll = cll; out.videoMaxFall = fall;
            }
            else if (a == L"--ffmpeg")   { auto v = needNext(L"--ffmpeg"); if (!v) return false; out.ffmpegPath = v; }
            else if (a == L"--time")     { auto v = needNext(L"--time"); if (!v) return false; out.time = std::wcstod(v, nullptr); }
            else if (a == L"--help" || a == L"-h" || a == L"-?") { PrintUsage(argv[0]); return false; }
            else { std::wprintf(L"ERROR: unknown argument '%ls'\n", argv[i]); return false; }
        }
        // Required-arg policy depends on mode:
        //   cache mode   -> no required args
        //   script mode  -> --graph + --script   (--node, --output ignored)
        //   render mode  -> --graph + --node + --output
        if (out.reapShaderCache || out.clearShaderCache)
        {
            // No further validation -- cache modes don't need a graph.
        }
        else if (!out.scriptPath.empty() || out.mcpSessionMode)
        {
            if (out.graphPath.empty()) {
                std::wprintf(L"ERROR: --graph is required (script / mcp-session mode)\n");
                return false;
            }
        }
        else if (!out.videoPath.empty())
        {
            if (out.graphPath.empty() || !out.hasNodeId) {
                std::wprintf(L"ERROR: --video needs --graph and --node\n");
                return false;
            }
        }
        else if (out.graphPath.empty() || out.outputPath.empty() || !out.hasNodeId)
        {
            std::wprintf(L"ERROR: --graph, --node, and --output are required\n");
            return false;
        }
        return true;
    }

    // Read entire file into a string. Returns empty string on failure
    // (caller checks). Strips a leading UTF-8 BOM if present so the JSON
    // parser doesn't fail on it; .effectgraph files saved by the GUI app
    // start with a BOM.
    std::string ReadFileUtf8(const std::wstring& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string s = ss.str();
        if (s.size() >= 3 &&
            static_cast<uint8_t>(s[0]) == 0xEF &&
            static_cast<uint8_t>(s[1]) == 0xBB &&
            static_cast<uint8_t>(s[2]) == 0xBF)
        {
            s.erase(0, 3);
        }
        return s;
    }

    // Result of loading a graph from either container form.
    struct LoadedGraph
    {
        ShaderLab::Graph::EffectGraph graph;
        // Non-empty when the source was a zip: the temp directory holding
        // extracted media. The graph's source-node paths point into it, so it
        // must outlive rendering; RemoveExtractDir() clears it afterwards.
        std::wstring extractDir;
        bool ok{ false };
        int  exitCode{ 0 };     // meaningful only when !ok
    };

    void RemoveExtractDir(const std::wstring& dir)
    {
        if (dir.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);   // best effort: temp dir
    }

    // Deletes the extracted-media temp directory on every exit path.
    // Declare it BEFORE the evaluator / source factory so it destructs AFTER
    // them -- those hold file handles into the directory while rendering.
    // Replace() moves it to the next loaded graph's directory; one that cannot
    // be deleted yet (a file still open) is retried at destruction.
    struct ExtractDirGuard
    {
        std::wstring dir;
        std::vector<std::wstring> retry;
        explicit ExtractDirGuard(std::wstring d) : dir(std::move(d)) {}
        ~ExtractDirGuard()
        {
            RemoveExtractDir(dir);
            for (const auto& leftover : retry) RemoveExtractDir(leftover);
        }
        void Replace(std::wstring next)
        {
            if (next == dir) return;
            RemoveExtractDir(dir);
            std::error_code ec;
            if (!dir.empty() && std::filesystem::exists(dir, ec)) retry.push_back(dir);
            dir = std::move(next);
        }
        ExtractDirGuard(const ExtractDirGuard&) = delete;
        ExtractDirGuard& operator=(const ExtractDirGuard&) = delete;
    };

    // Load a graph from either container form (EffectGraphFile::LoadAny):
    //   * a .effectgraph ZIP (what the GUI's Save produces) -- graph.json plus
    //     optional embedded media under media/, and
    //   * a bare .json graph (what the test fixtures and older files are).
    // Source nodes' "media://<name>" tokens are rewritten to the extracted
    // temp paths, as the GUI's File > Open does.
    LoadedGraph LoadGraphFromPath(const std::wstring& path)
    {
        LoadedGraph result;

        wchar_t tempRoot[MAX_PATH + 1]{};
        GetTempPathW(MAX_PATH + 1, tempRoot);
        std::wstring loadError;
        auto loaded = ShaderLab::Rendering::EffectGraphFile::LoadAny(path, tempRoot, loadError);
        if (!loaded.has_value())
        {
            std::wprintf(L"FATAL: could not read graph file: %ls\n", loadError.c_str());
            result.exitCode = 4;
            return result;
        }
        result.extractDir = loaded->extractDir;

        std::wstring parseError;
        try {
            result.graph = ShaderLab::Graph::EffectGraph::FromJson(winrt::hstring(loaded->graphJson));
            ShaderLab::Effects::ShaderLabEffects::RestoreRuntimeFlags(result.graph);
        } catch (winrt::hresult_error const& e) {
            parseError = std::format(L"0x{:08X}: {}", static_cast<uint32_t>(e.code()), std::wstring(e.message()));
        } catch (std::exception const& e) {
            parseError = winrt::to_hstring(e.what());
        }
        if (!parseError.empty())
        {
            std::wprintf(L"FATAL: graph JSON parse failed (%ls)\n", parseError.c_str());
            RemoveExtractDir(result.extractDir);
            result.extractDir.clear();
            result.exitCode = 5;
            return result;
        }

        ShaderLab::Rendering::EffectGraphFile::ResolveMediaTokens(result.graph, loaded->mediaMap);
        result.ok = true;
        return result;
    }

    // Encode an FP32 RGBA buffer as PNG via WIC. PNG is 8-bit per channel;
    // values are gamma-encoded sRGB after a clamp to [0, 1]. This is lossy
    // for HDR scRGB output (anything above 1.0 saturates to 255) -- use a
    // .jxr output path for HDR fidelity (SaveFp32AsJxr below).
    HRESULT SaveFp32AsPng(IWICImagingFactory* wic,
        const float* rgba, uint32_t w, uint32_t h, uint32_t pitchBytes,
        const std::wstring& path)
    {
        // Convert FP32 RGBA to sRGB-gamma 8-bit BGRA.
        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
        auto sRgbEncode = [](float c) -> uint8_t {
            float clamped = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
            float enc = (clamped <= 0.0031308f)
                ? clamped * 12.92f
                : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
            int v = static_cast<int>(enc * 255.0f + 0.5f);
            return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        };
        for (uint32_t y = 0; y < h; ++y)
        {
            const float* srcRow = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(rgba) + y * pitchBytes);
            uint8_t* dstRow = bgra.data() + y * w * 4;
            for (uint32_t x = 0; x < w; ++x)
            {
                const float* px = srcRow + x * 4;
                dstRow[x * 4 + 0] = sRgbEncode(px[2]); // B
                dstRow[x * 4 + 1] = sRgbEncode(px[1]); // G
                dstRow[x * 4 + 2] = sRgbEncode(px[0]); // R
                dstRow[x * 4 + 3] = static_cast<uint8_t>(
                    px[3] < 0.0f ? 0 : (px[3] > 1.0f ? 255 : static_cast<int>(px[3] * 255.0f + 0.5f)));
            }
        }

        winrt::com_ptr<IWICStream> stream;
        HRESULT hr = wic->CreateStream(stream.put());
        if (FAILED(hr)) return hr;
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) return hr;

        winrt::com_ptr<IWICBitmapEncoder> encoder;
        hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
        if (FAILED(hr)) return hr;
        hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) return hr;

        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        hr = encoder->CreateNewFrame(frame.put(), nullptr);
        if (FAILED(hr)) return hr;
        hr = frame->Initialize(nullptr);
        if (FAILED(hr)) return hr;
        hr = frame->SetSize(w, h);
        if (FAILED(hr)) return hr;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&fmt);
        if (FAILED(hr)) return hr;
        hr = frame->WritePixels(h, w * 4, w * h * 4, bgra.data());
        if (FAILED(hr)) return hr;
        hr = frame->Commit();
        if (FAILED(hr)) return hr;
        return encoder->Commit();
    }

    // Encode an FP32 RGBA buffer as JPEG XR (.jxr / .wdp) via WIC, preserving
    // HDR. Unlike the PNG path there is NO clamp and NO transfer encoding: the
    // pipeline's scRGB linear values are written as-is into a 64bpp RGBA-half
    // frame, so values above 1.0 (above SDR white) and the negative components
    // that express wide-gamut colour both survive the round trip.
    //
    // FP32 -> FP16 is not a precision loss in practice: the pipeline is
    // R16G16B16A16_FLOAT, so these values originated as halves.
    //
    // Mirrors the GUI's OutputWindow::SaveImageAsync JXR branch
    // (GUID_ContainerFormatWmp + Lossless), so a node saved from an output
    // window and the same node captured headless produce the same file.
    HRESULT SaveFp32AsJxr(IWICImagingFactory* wic,
        const float* rgba, uint32_t w, uint32_t h, uint32_t pitchBytes,
        const std::wstring& path)
    {
        using DirectX::PackedVector::XMConvertFloatToHalf;

        // 64bpp RGBA half, tightly packed.
        std::vector<uint16_t> halfRgba(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
        {
            const float* srcRow = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(rgba) + y * pitchBytes);
            uint16_t* dstRow = halfRgba.data() + static_cast<size_t>(y) * w * 4;
            for (uint32_t x = 0; x < w * 4; ++x)
                dstRow[x] = XMConvertFloatToHalf(srcRow[x]);
        }

        winrt::com_ptr<IWICStream> stream;
        HRESULT hr = wic->CreateStream(stream.put());
        if (FAILED(hr)) return hr;
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) return hr;

        winrt::com_ptr<IWICBitmapEncoder> encoder;
        hr = wic->CreateEncoder(GUID_ContainerFormatWmp, nullptr, encoder.put());
        if (FAILED(hr)) return hr;
        hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) return hr;

        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::com_ptr<IPropertyBag2> encoderOptions;
        hr = encoder->CreateNewFrame(frame.put(), encoderOptions.put());
        if (FAILED(hr)) return hr;

        // Lossless: the point of this path is fidelity, not file size.
        if (encoderOptions)
        {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"Lossless");
            VARIANT val{};
            val.vt = VT_BOOL;
            val.boolVal = VARIANT_TRUE;
            encoderOptions->Write(1, &option, &val);
        }

        hr = frame->Initialize(encoderOptions.get());
        if (FAILED(hr)) return hr;
        hr = frame->SetSize(w, h);
        if (FAILED(hr)) return hr;

        WICPixelFormatGUID fmt = GUID_WICPixelFormat64bppRGBAHalf;
        hr = frame->SetPixelFormat(&fmt);
        if (FAILED(hr)) return hr;
        // WIC may negotiate a different format; refuse rather than silently
        // writing something that is not the half-float data we promised.
        if (fmt != GUID_WICPixelFormat64bppRGBAHalf) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;

        const uint32_t stride = w * 4 * sizeof(uint16_t);
        hr = frame->WritePixels(h, stride, stride * h,
            reinterpret_cast<BYTE*>(halfRgba.data()));
        if (FAILED(hr)) return hr;
        hr = frame->Commit();
        if (FAILED(hr)) return hr;
        return encoder->Commit();
    }

    // True when the output path asks for the HDR-preserving encoder.
    bool IsHdrOutputPath(const std::wstring& path)
    {
        auto dot = path.rfind(L'.');
        if (dot == std::wstring::npos) return false;
        std::wstring ext = path.substr(dot);
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        return ext == L".jxr" || ext == L".wdp";
    }

    // Pick the encoder from the output file's extension. PNG is the default
    // for anything unrecognized, matching the historical behavior.
    HRESULT SaveFp32Image(IWICImagingFactory* wic,
        const float* rgba, uint32_t w, uint32_t h, uint32_t pitchBytes,
        const std::wstring& path)
    {
        if (IsHdrOutputPath(path))
            return SaveFp32AsJxr(wic, rgba, w, h, pitchBytes, path);
        return SaveFp32AsPng(wic, rgba, w, h, pitchBytes, path);
    }
}

int wmain(int argc, wchar_t* argv[]);

int RunScript(const Args& args);

// Render path extracted into a function so all locals (D2D bitmaps,
// GraphEvaluator, com_ptrs) destruct cleanly before wmain calls
// MFShutdown / uninit_apartment. The dangling order otherwise produced
// hangs on D2D device teardown.
namespace
{
    // The time a video node shows, as SourceNodeFactory seeks it: looped into
    // [0, duration), or clamped at 0 without looping. Nullopt past the end of a
    // non-looping video, which shows nothing.
    std::optional<double> VideoTargetTime(const ShaderLab::Graph::EffectNode& node, double duration)
    {
        double seconds = 0.0;
        if (auto it = node.properties.find(L"Time"); it != node.properties.end())
            if (auto* value = std::get_if<float>(&it->second)) seconds = *value;
        bool loop = true;
        if (auto it = node.properties.find(L"Loop"); it != node.properties.end())
            if (auto* value = std::get_if<bool>(&it->second)) loop = *value;
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

    // True when the uploaded frame is the one shown at `target`: the frame
    // whose [start, start + duration) contains it. The first frame also covers
    // any time before it, and the last any gap to the end of the stream.
    bool VideoShowsTime(const ShaderLab::Effects::VideoSourceProvider& provider, double target)
    {
        const double frameStart = provider.UploadedFrameTime();
        if (frameStart < 0.0) return false;
        target = (std::max)(target, provider.FirstFrameTime());
        constexpr double cTolerance = 1e-4;
        double frameEnd = frameStart + (std::max)(provider.UploadedFrameDuration(), cTolerance);
        const double nominalFrame = 1.0 / (std::max)(provider.FrameRate(), 1.0);
        if (frameEnd + nominalFrame > provider.Duration()) frameEnd = (std::max)(frameEnd, provider.Duration());
        return target >= frameStart - cTolerance && target < frameEnd + cTolerance;
    }

    // Headless has no render loop to tick video sources, so tick them here and
    // wait (up to 5 s) until each video shows the frame for its Time. Wait on
    // the uploaded frame (UploadedFrameTime): Position jumps to the target as
    // soon as a seek is requested, and the first upload after a seek can still
    // be the previous frame. A Time driven by a binding is resolved first, with
    // a frozen evaluation pass, so the wait is for the time the render will use.
    // Returns false, after a [ShaderLab] line, if a video never got there.
    bool SettleVideoSources(ShaderLab::Effects::SourceNodeFactory& factory,
                            ShaderLab::Rendering::GraphEvaluator& evaluator,
                            ShaderLab::Graph::EffectGraph& graph,
                            ID2D1DeviceContext5* dc)
    {
        // Videos that already timed out at a time, keyed by node, file and time,
        // so a video that cannot deliver does not stall every later evaluation.
        static std::set<std::tuple<uint32_t, std::wstring, double>> sGaveUp;

        auto& nodes = const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(graph.Nodes());
        auto isVideo = [](const ShaderLab::Graph::EffectNode& node)
        {
            if (node.type != ShaderLab::Graph::NodeType::Source) return false;
            auto it = node.properties.find(L"IsVideo");
            return it != node.properties.end() && std::holds_alternative<bool>(it->second) && std::get<bool>(it->second);
        };

        // Resolve bound Times. Upstream values (a Clock's Time) are only computed
        // by evaluation, so run one frozen pass and put back the dirty flags so
        // the caller's evaluation still sees every pending change.
        const bool anyBoundTime = std::any_of(nodes.begin(), nodes.end(), [&](const auto& node)
            { return isVideo(node) && node.propertyBindings.count(L"Time"); });
        if (anyBoundTime)
        {
            std::vector<uint32_t> dirtyIds;
            for (const auto& node : nodes)
                if (node.dirty) dirtyIds.push_back(node.id);
            dc->SetTarget(nullptr);
            evaluator.SetDeferredComputeFrozen(true);
            evaluator.Evaluate(graph, dc);
            evaluator.SetDeferredComputeFrozen(false);
            evaluator.ResolveSourceBindings(graph);
            for (uint32_t id : dirtyIds)
                if (auto* node = graph.FindNode(id)) node->dirty = true;
        }

        struct Pending
        {
            uint32_t nodeId;
            std::wstring name;
            std::wstring path;
            double target;
        };
        auto collectPending = [&]
        {
            std::vector<Pending> pending;
            for (const auto& node : nodes)
            {
                if (!isVideo(node)) continue;
                auto* provider = factory.GetVideoProvider(node.id);
                if (!provider || !provider->IsOpen()) continue;
                const auto target = VideoTargetTime(node, provider->Duration());
                if (!target || VideoShowsTime(*provider, *target)) continue;
                const std::wstring path = node.shaderPath.value_or(L"");
                if (sGaveUp.count({ node.id, path, *target })) continue;
                pending.push_back({ node.id, node.name, path, *target });
            }
            return pending;
        };

        factory.TickAndUploadVideos(nodes, dc, 0.0);
        auto pending = collectPending();
        for (int i = 0; i < 500 && !pending.empty(); ++i)   // 5 s; a seek decodes from the last keyframe
        {
            ::Sleep(10);
            factory.TickAndUploadVideos(nodes, dc, 0.0);
            pending = collectPending();
        }
        for (const auto& video : pending)
        {
            sGaveUp.insert({ video.nodeId, video.path, video.target });
            auto* provider = factory.GetVideoProvider(video.nodeId);
            std::fwprintf(stderr, L"[ShaderLab] video '%ls' did not deliver the frame for %.3f s within 5 s "
                                  L"(frame on screen %.3f s); this run shows a different frame\n",
                          video.name.c_str(), video.target, provider ? provider->UploadedFrameTime() : -1.0);
        }
        return pending.empty();
    }

    // Shared by --video and the /render/video route: strings -> enums, with a
    // readable error for anything unrecognised.
    bool ParseVideoEnums(const std::wstring& format, const std::wstring& codec, const std::wstring& mastering,
                         ShaderLab::Rendering::VideoExportRequest& req, std::wstring& error)
    {
        using namespace ShaderLab::Rendering;
        if (format == L"hdr10" || format.empty()) req.format = VideoFormat::Hdr10;
        else if (format == L"sdr") req.format = VideoFormat::Sdr;
        else { error = L"format must be hdr10 or sdr (got '" + format + L"')"; return false; }
        if (codec.empty()) req.codec = VideoCodec::Default;
        else if (codec == L"hevc" || codec == L"h265") req.codec = VideoCodec::Hevc;
        else if (codec == L"av1") req.codec = VideoCodec::Av1;
        else if (codec == L"h264" || codec == L"avc") req.codec = VideoCodec::H264;
        else { error = L"codec must be hevc, av1 or h264 (got '" + codec + L"')"; return false; }
        if (mastering == L"p3-1000" || mastering.empty()) req.mastering = MasteringMode::P3D65_1000;
        else if (mastering == L"working-space") req.mastering = MasteringMode::WorkingSpace;
        else if (mastering == L"none") req.mastering = MasteringMode::None;
        else { error = L"mastering must be p3-1000, working-space or none (got '" + mastering + L"')"; return false; }
        return true;
    }

    std::string VideoResultJson(const ShaderLab::Rendering::VideoExportResult& r)
    {
        namespace WDJ = winrt::Windows::Data::Json;
        WDJ::JsonObject o;
        o.Insert(L"ok", WDJ::JsonValue::CreateBooleanValue(r.ok));
        if (!r.ok) o.Insert(L"error", WDJ::JsonValue::CreateStringValue(r.error));
        o.Insert(L"ffmpegPath", WDJ::JsonValue::CreateStringValue(r.ffmpegPath));
        o.Insert(L"encoder", WDJ::JsonValue::CreateStringValue(r.encoder));
        o.Insert(L"commandLine", WDJ::JsonValue::CreateStringValue(r.commandLine));
        o.Insert(L"width", WDJ::JsonValue::CreateNumberValue(r.width));
        o.Insert(L"height", WDJ::JsonValue::CreateNumberValue(r.height));
        o.Insert(L"frames", WDJ::JsonValue::CreateNumberValue(r.frames));
        o.Insert(L"fps", WDJ::JsonValue::CreateNumberValue(r.fps));
        o.Insert(L"start", WDJ::JsonValue::CreateNumberValue(r.start));
        o.Insert(L"duration", WDJ::JsonValue::CreateNumberValue(r.duration));
        o.Insert(L"maxCll", WDJ::JsonValue::CreateNumberValue(r.maxCll));
        o.Insert(L"maxFall", WDJ::JsonValue::CreateNumberValue(r.maxFall));
        o.Insert(L"staticMetadataWritten", WDJ::JsonValue::CreateBooleanValue(r.staticMetadataWritten));
        o.Insert(L"seconds", WDJ::JsonValue::CreateNumberValue(r.seconds));
        WDJ::JsonArray w;
        for (const auto& s : r.warnings) w.Append(WDJ::JsonValue::CreateStringValue(s));
        o.Insert(L"warnings", w);
        if (!r.ok && !r.ffmpegLogTail.empty())
            o.Insert(L"ffmpegLogTail", WDJ::JsonValue::CreateStringValue(winrt::to_hstring(r.ffmpegLogTail)));
        return winrt::to_string(o.Stringify());
    }
}

int RunRender(const Args& args)
{
    UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    winrt::com_ptr<ID3D11Device> baseDevice;
    winrt::com_ptr<ID3D11DeviceContext> baseCtx;
    HRESULT hr = D3D11CreateDevice(nullptr,
        args.useWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, d3dFlags,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseCtx.put());
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: D3D11CreateDevice failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 3;
    }
    auto d3dDevice = baseDevice.as<ID3D11Device5>();
    auto d3dContext = baseCtx.as<ID3D11DeviceContext4>();

    // Match the test runner: enable D3D10 multithread protection so any
    // background-thread DXVA2 work the engine might spin up doesn't crash.
    winrt::com_ptr<ID3D10Multithread> mt;
    d3dDevice.as(mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    winrt::com_ptr<ID2D1Factory7> d2dFactory;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory7), reinterpret_cast<void**>(d2dFactory.put()));

    winrt::com_ptr<IDXGIDevice> dxgiDev;
    baseDevice->QueryInterface(dxgiDev.put());
    winrt::com_ptr<ID2D1Device6> d2dDevice;
    d2dFactory->CreateDevice(dxgiDev.as<IDXGIDevice>().get(),
        reinterpret_cast<ID2D1Device**>(d2dDevice.put()));
    winrt::com_ptr<ID2D1DeviceContext5> dc;
    d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        reinterpret_cast<ID2D1DeviceContext**>(dc.put()));

    // Register custom effect classes with the D2D factory (CustomPixelShader,
    // CustomComputeShader). Without this, custom-effect
    // nodes in the graph fail to instantiate.
    winrt::com_ptr<ID2D1Factory1> factory1;
    d2dFactory->QueryInterface(factory1.put());
    ShaderLab::Effects::RegisterEngineD2DEffects(factory1.get());

    // ---- Load the graph ----------------------------------------------------
    // Handles both a .effectgraph ZIP (media extracted to a temp dir, which
    // must survive until rendering is done) and a bare JSON graph.
    auto loaded = LoadGraphFromPath(args.graphPath);
    if (!loaded.ok) return loaded.exitCode;
    auto& graph = loaded.graph;
    ExtractDirGuard extractGuard{ loaded.extractDir };

    if (!graph.FindNode(args.nodeId)) {
        std::wprintf(L"FATAL: node id %u not found in graph\n", args.nodeId);
        return 6;
    }

    // ---- Evaluate the graph (long-lived evaluator!) ------------------------
    // The evaluator owns the per-node ID2D1Effect cache; each node's
    // cachedOutput is a non-owning pointer into that cache. Keep it alive
    // until after readback. (See Phase 7 spike for the lifetime gotcha.)
    ShaderLab::Effects::SourceNodeFactory sourceFactory;
    ShaderLab::Rendering::GraphEvaluator evaluator;

    graph.MarkAllDirty();
    for (auto& node : const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(graph.Nodes()))
    {
        if (node.type == ShaderLab::Graph::NodeType::Source)
        {
            try {
                sourceFactory.PrepareSourceNode(node, dc.get(), 0.0,
                    d3dDevice.get(), d3dContext.get());
            } catch (...) {
                // Source prep can fail (missing media, etc); the evaluator
                // will report cachedOutput=nullptr below if so.
            }
        }
    }
    // Working Space nodes need the active display profile here too. RunScript
    // pumps this in runEval; this path had no DisplayMonitor at all, so a saved
    // graph containing a Working Space node rendered against the 80-nit
    // hiddenDefaults -- and this is the `--output` / `--pixels` path, i.e.
    // exactly the golden-image case.
    ShaderLab::Rendering::DisplayMonitor renderDisplayMonitor;
    renderDisplayMonitor.InitializeForPrimaryMonitor();
    ApplyDisplayPin(args, renderDisplayMonitor);
    ShaderLab::Rendering::UpdateWorkingSpaceNodes(graph, renderDisplayMonitor);

    if (args.time.has_value())
        ShaderLab::Rendering::SetClocksToTime(graph, *args.time);
    SettleVideoSources(sourceFactory, evaluator, graph, dc.get());

    if (!args.videoPath.empty())
    {
        ShaderLab::Rendering::VideoExportRequest req;
        std::wstring perr;
        if (!ParseVideoEnums(args.videoFormat, args.videoCodec, args.videoMastering, req, perr)) {
            std::wprintf(L"FATAL: %ls\n", perr.c_str());
            return 2;
        }
        req.nodeId = args.nodeId;
        req.outputPath = args.videoPath;
        req.fps = args.videoFps;
        req.start = args.videoStart;
        req.duration = args.videoDuration;
        req.crf = args.videoCrf;
        req.preset = args.videoPreset;
        req.lossless = args.videoLossless;
        req.maxCll = args.videoMaxCll;
        req.maxFall = args.videoMaxFall;
        req.ffmpegPath = args.ffmpegPath;
        auto res = ShaderLab::Rendering::ExportVideo(req, graph, evaluator, sourceFactory,
            &renderDisplayMonitor, dc.get(), d3dDevice.get(), d3dContext.get());
        for (const auto& w : res.warnings) std::wprintf(L"WARNING: %ls\n", w.c_str());
        if (!res.ok) {
            std::wprintf(L"FATAL: video export failed: %ls\n", res.error.c_str());
            if (!res.ffmpegLogTail.empty())
                std::printf("---- ffmpeg log (tail) ----\n%s\n", res.ffmpegLogTail.c_str());
            return 8;
        }
        std::wprintf(L"OK: wrote %ls -- %ux%u, %u frames @ %.3f fps (%.3f s), %ls, MaxCLL %.1f / MaxFALL %.1f nits%ls, %.1f s\n",
            args.videoPath.c_str(), res.width, res.height, res.frames, res.fps, res.duration,
            res.encoder.c_str(), res.maxCll, res.maxFall,
            res.staticMetadataWritten ? L" (in headers)" : L"", res.seconds);
        return 0;
    }

    // Evaluate OUTSIDE the draw session, ProcessDeferredCompute INSIDE it --
    // the same split RunScript's runEval uses, and the structure of
    // MainWindow::RenderFrameToOffscreen. Evaluate() can open its OWN
    // BeginDraw/EndDraw (histogram and custom-analysis readbacks), and
    // nesting that inside an outer session fails with D2DERR_WRONG_STATE
    // (0x88990001). ProcessDeferredCompute itself DOES need an active
    // session -- it calls dc->DrawImage internally, which silently no-ops
    // outside one (CLAUDE.md records this trap).
    dc->SetTarget(nullptr);
    evaluator.Evaluate(graph, dc.get());
    if (graph.HasDirtyNodes())
        evaluator.Evaluate(graph, dc.get());
    dc->BeginDraw();
    evaluator.ProcessDeferredCompute(graph, dc.get());
    // Frozen so this in-session sweep cannot re-queue a compute node and
    // re-enter the nested-BeginDraw path just avoided.
    if (graph.HasDirtyNodes())
    {
        evaluator.SetDeferredComputeFrozen(true);
        evaluator.Evaluate(graph, dc.get());
        evaluator.SetDeferredComputeFrozen(false);
    }
    dc->EndDraw();

    auto* node = graph.FindNode(args.nodeId);
    if (!node->cachedOutput) {
        std::wprintf(L"FATAL: node %u has no cachedOutput after evaluate (missing inputs or eval error)\n", args.nodeId);
        if (!node->runtimeError.empty()) {
            std::wprintf(L"        node.runtimeError = %ls\n", node->runtimeError.c_str());
        }
        return 7;
    }

    // ---- Pixel-region readback mode (alternate output path) ----------------
    // No tonemap, no PNG encode -- raw FP32 RGBA pixels for full-accuracy
    // analysis (designed to be the engine path the future MCP
    // read_pixel_region route uses; see p7-mcp-move).
    if (args.pixelMode)
    {
        auto rr = ::ShaderLab::Rendering::ReadPixelRegion(
            graph, args.nodeId,
            args.pixelX, args.pixelY, args.pixelW, args.pixelH,
            dc.get());
        switch (rr.status)
        {
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::Success: break;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::NotFound:
            std::wprintf(L"FATAL: pixel readback NotFound for node %u\n", args.nodeId);
            return 7;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::NotReady:
            std::wprintf(L"FATAL: pixel readback NotReady for node %u (dirty or missing inputs)\n", args.nodeId);
            return 7;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::InvalidRegion:
            std::wprintf(L"FATAL: pixel readback InvalidRegion (clipped to nothing inside image)\n");
            return 8;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::D2DError:
        default:
            std::wprintf(L"FATAL: pixel readback D2DError\n");
            return 10;
        }

        // Pick output format from extension. .csv -> text rows.
        // .bin/.raw/anything-else -> packed binary (uint32 W, uint32 H,
        // then float[W*H*4] RGBA row-major).
        auto endsWith = [](const std::wstring& s, const wchar_t* suffix) {
            size_t sl = std::wcslen(suffix);
            return s.size() >= sl && std::equal(s.end() - sl, s.end(), suffix);
        };
        bool csv = endsWith(args.outputPath, L".csv") || endsWith(args.outputPath, L".CSV");

        std::ofstream f(args.outputPath, std::ios::binary);
        if (!f) {
            std::wprintf(L"FATAL: could not open output '%ls'\n", args.outputPath.c_str());
            return 13;
        }
        if (csv)
        {
            f << "x,y,r,g,b,a\n";
            char buf[256];
            for (uint32_t row = 0; row < rr.actualHeight; ++row) {
                for (uint32_t col = 0; col < rr.actualWidth; ++col) {
                    const float* px = rr.pixels.data()
                        + (static_cast<size_t>(row) * rr.actualWidth + col) * 4;
                    int n = std::snprintf(buf, sizeof(buf),
                        "%d,%d,%.9g,%.9g,%.9g,%.9g\n",
                        args.pixelX + static_cast<int32_t>(col),
                        args.pixelY + static_cast<int32_t>(row),
                        px[0], px[1], px[2], px[3]);
                    f.write(buf, n);
                }
            }
        }
        else
        {
            // Binary header so consumers can self-describe (uint32 W,
            // uint32 H = the *actual* clipped region, in case the
            // request extended past the image edge).
            uint32_t hdr[2] = { rr.actualWidth, rr.actualHeight };
            f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
            f.write(reinterpret_cast<const char*>(rr.pixels.data()),
                static_cast<std::streamsize>(rr.pixels.size() * sizeof(float)));
        }
        f.close();

        std::wprintf(L"OK: read %ux%u pixel region (%s) from node %u -> %ls\n",
            rr.actualWidth, rr.actualHeight, csv ? L"csv" : L"binary",
            args.nodeId, args.outputPath.c_str());
        return 0;
    }

    // ---- Render to FP32 target + readback ----------------------------------
    D2D1_BITMAP_PROPERTIES1 targetProps{};
    targetProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
    targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    targetProps.dpiX = 96.0f;
    targetProps.dpiY = 96.0f;
    winrt::com_ptr<ID2D1Bitmap1> target;
    hr = dc->CreateBitmap(D2D1::SizeU(args.width, args.height),
        nullptr, 0, targetProps, target.put());
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: CreateBitmap (target) failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 8;
    }

    D2D1_BITMAP_PROPERTIES1 stagingProps = targetProps;
    stagingProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    winrt::com_ptr<ID2D1Bitmap1> staging;
    hr = dc->CreateBitmap(D2D1::SizeU(args.width, args.height),
        nullptr, 0, stagingProps, staging.put());
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: CreateBitmap (staging) failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 9;
    }

    // Build the input image for the render pass: either the cached
    // output directly (--no-tonemap) or the same image piped through
    // a CLSID_D2D1HdrToneMap effect (default). The HdrToneMap effect's
    // documented behavior is "fixed BT.2408-style mid-tone lift" -- see
    // README decision log #52 for the empirical analysis. For PNG
    // visual-inspection output we want the lift; for raw scRGB pixel
    // sampling use --no-tonemap (or, future work, the FP16 readback
    // path tracked as p7-headless-fp16-pixel-readback).
    // A .jxr / .wdp output exists to preserve HDR, so tone mapping to SDR
    // would defeat it: the default HdrToneMap (OUTPUT_MAX_LUMINANCE = 80)
    // maps a 800-nit source down to ~1.0 scRGB, and the encoder would then
    // faithfully store an SDR image in an HDR container. So HDR output
    // implies --no-tonemap, UNLESS the caller named a peak explicitly --
    // tone mapping INTO an HDR deliverable (e.g. 4000-nit source to a
    // 1000-nit target) is a legitimate request and must still win.
    const bool hdrOutput = IsHdrOutputPath(args.outputPath);
    const bool skipToneMap = args.skipToneMap || (hdrOutput && !args.toneMapExplicit);
    if (hdrOutput && !args.skipToneMap && !args.toneMapExplicit)
        std::wprintf(L"NOTE: HDR output (%ls) -- skipping HdrToneMap to preserve "
                     L"values above 1.0. Pass --output-peak-nits to tone map anyway.\n",
                     args.outputPath.c_str());

    winrt::com_ptr<ID2D1Effect> toneMap;
    winrt::com_ptr<ID2D1Image> toneMappedOut;
    ID2D1Image* renderInput = node->cachedOutput;
    if (!skipToneMap)
    {
        hr = dc->CreateEffect(CLSID_D2D1HdrToneMap, toneMap.put());
        if (FAILED(hr)) {
            std::wprintf(L"FATAL: CreateEffect(HdrToneMap) failed 0x%08X\n", static_cast<uint32_t>(hr));
            return 10;
        }
        toneMap->SetInput(0, node->cachedOutput);
        toneMap->SetValue(D2D1_HDRTONEMAP_PROP_INPUT_MAX_LUMINANCE,  args.inputPeakNits);
        toneMap->SetValue(D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE, args.outputPeakNits);
        toneMap->SetValue(D2D1_HDRTONEMAP_PROP_DISPLAY_MODE,
            (args.outputPeakNits <= 80.0f)
                ? D2D1_HDRTONEMAP_DISPLAY_MODE_SDR
                : D2D1_HDRTONEMAP_DISPLAY_MODE_HDR);
        toneMap->GetOutput(toneMappedOut.put());
        renderInput = toneMappedOut.get();
    }

    float oldDpiX, oldDpiY;
    dc->GetDpi(&oldDpiX, &oldDpiY);
    dc->SetDpi(96.0f, 96.0f);
    dc->SetTarget(target.get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawImage(renderInput);
    hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    dc->SetDpi(oldDpiX, oldDpiY);
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: D2D EndDraw failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 10;
    }

    D2D1_POINT_2U dstPt = { 0, 0 };
    D2D1_RECT_U srcRect = { 0, 0, args.width, args.height };
    hr = staging->CopyFromBitmap(&dstPt, target.get(), &srcRect);
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: CopyFromBitmap failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 11;
    }

    D2D1_MAPPED_RECT mapped{};
    hr = staging->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: Map staging failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 12;
    }

    // ---- Encode PNG via WIC ------------------------------------------------
    winrt::com_ptr<IWICImagingFactory> wic;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wic.put()));
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: WIC factory failed 0x%08X\n", static_cast<uint32_t>(hr));
        staging->Unmap();
        return 13;
    }

    hr = SaveFp32Image(wic.get(),
        reinterpret_cast<const float*>(mapped.bits),
        args.width, args.height, mapped.pitch,
        args.outputPath);
    staging->Unmap();
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: image encode failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 14;
    }

    std::wprintf(L"OK: rendered node %u (%ux%u) -> %ls\n",
        args.nodeId, args.width, args.height, args.outputPath.c_str());

    // Explicit teardown order: drop the D2D/D3D resources before
    // ::MFShutdown / uninit_apartment so the destructors don't race
    // shutdown. Symptom that motivated this: the process hung after the
    // OK print on first runs.
    staging = nullptr;
    target = nullptr;
    wic = nullptr;
    dc = nullptr;
    d2dDevice = nullptr;
    dxgiDev = nullptr;
    d2dFactory = nullptr;
    factory1 = nullptr;
    d3dContext = nullptr;
    d3dDevice = nullptr;
    mt = nullptr;
    baseCtx = nullptr;
    baseDevice = nullptr;
    return 0;
}

// Headless command sink — synchronous Dispatch (no UI thread to marshal
// to). The UI event hooks are no-ops since headless doesn't have a canvas
// or status bar to keep in sync; the media-directory hook hands the
// directory to the guard that owns the loaded graph's extracted media.
// The engine routes were designed so that with this sink + a properly
// populated EngineContext, every route migrated in Phase 7 works
// identically to the GUI host.
struct HeadlessSink : ShaderLab::Mcp::IEngineCommandSink
{
    std::function<void(ShaderLab::Mcp::EngineContext&)> populateContext;
    ExtractDirGuard* mediaDir{ nullptr };

    void OnGraphMediaDirChanged(const std::wstring& extractDir) override
    {
        if (mediaDir) mediaDir->Replace(extractDir);
    }

    ShaderLab::Mcp::Response Dispatch(
        std::function<ShaderLab::Mcp::Response(
            ShaderLab::Mcp::EngineContext&)> closure) override
    {
        ShaderLab::Mcp::EngineContext ctx{};
        populateContext(ctx);
        return closure(ctx);
    }
};

// Walk a script JSON document of MCP-style operations, dispatching each
// through the engine route registry. Output is a JSON document with one
// {step, status, body} entry per operation.
//
// Accepted step shapes:
//   {"method":"GET|POST", "path":"/...", "body": <object|string>}
//   {"op":"<shorthand>", ...key/value args...}
//
// Shorthand mappings (so common sweep ops don't need the verbose form):
//   set-property   -> POST /graph/set-property
//   pixel-region   -> POST /render/pixel-region
//   capture-node   -> POST /render/capture-node
//   render         -> internal: forces a fresh evaluation (useful as a
//                     barrier between mutations and readbacks).
//   get-graph      -> GET  /graph
//   get-node       -> GET  /graph/node/{nodeId}
//   analysis       -> GET  /analysis/{nodeId}
int RunScript(const Args& args)
{
    namespace WDJ = winrt::Windows::Data::Json;

    // ---- Engine setup (same shape as RunRender) ---------------------------
    UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    winrt::com_ptr<ID3D11Device> baseDevice;
    winrt::com_ptr<ID3D11DeviceContext> baseCtx;
    HRESULT hr = D3D11CreateDevice(nullptr,
        args.useWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, d3dFlags,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseCtx.put());
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: D3D11CreateDevice failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 3;
    }
    auto d3dDevice = baseDevice.as<ID3D11Device5>();
    auto d3dContext = baseCtx.as<ID3D11DeviceContext4>();

    winrt::com_ptr<ID3D10Multithread> mt;
    d3dDevice.as(mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    winrt::com_ptr<ID2D1Factory7> d2dFactory;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory7), reinterpret_cast<void**>(d2dFactory.put()));

    winrt::com_ptr<IDXGIDevice> dxgiDev;
    baseDevice->QueryInterface(dxgiDev.put());
    winrt::com_ptr<ID2D1Device6> d2dDevice;
    d2dFactory->CreateDevice(dxgiDev.as<IDXGIDevice>().get(),
        reinterpret_cast<ID2D1Device**>(d2dDevice.put()));
    winrt::com_ptr<ID2D1DeviceContext5> dc;
    d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        reinterpret_cast<ID2D1DeviceContext**>(dc.put()));

    winrt::com_ptr<ID2D1Factory1> factory1;
    d2dFactory->QueryInterface(factory1.put());
    ShaderLab::Effects::RegisterEngineD2DEffects(factory1.get());

    // ---- Load graph -------------------------------------------------------
    // Same dual-form loader as RunRender: .effectgraph ZIP or bare JSON.
    auto loaded = LoadGraphFromPath(args.graphPath);
    if (!loaded.ok) return loaded.exitCode;
    auto& graph = loaded.graph;
    ExtractDirGuard extractGuard{ loaded.extractDir };

    ShaderLab::Effects::SourceNodeFactory sourceFactory;
    ShaderLab::Rendering::GraphEvaluator evaluator;
    // GPU-side span timing. Headless is where perf sweeps actually run, so it
    // gets the same instrument as the GUI. Off unless --gpu-timing.
    ShaderLab::Rendering::GpuTimer gpuTimer;
    gpuTimer.Initialize(d3dDevice.get());
    gpuTimer.SetEnabled(args.gpuTiming);
    // Same wiring the GUI uses, so a crash in the per-node path reproduces
    // here -- where there is stderr and a fast edit/run loop -- instead of
    // only inside a packaged app.
    evaluator.SetGpuTimer(&gpuTimer);
    if (args.gpuTiming && !gpuTimer.IsInitialized())
        std::fwprintf(stderr, L"[ShaderLab] --gpu-timing requested but this device has "
                              L"no timestamp queries; GPU times will be absent\n");
    // Snapshot the primary monitor's real advanced-color caps (no change
    // events — headless runs no DispatcherQueue). Falls back to struct
    // defaults when no display is reachable (CI, session 0).
    ShaderLab::Rendering::DisplayMonitor displayMonitor;
    displayMonitor.InitializeForPrimaryMonitor();
    ApplyDisplayPin(args, displayMonitor);

    // Prep source nodes once (loads media off disk). Properties on
    // source nodes are typically static (file path); set-property on
    // them won't trigger re-load, but most sweeps target downstream
    // effect properties.
    graph.MarkAllDirty();
    for (auto& node : const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(graph.Nodes()))
    {
        if (node.type == ShaderLab::Graph::NodeType::Source)
        {
            try {
                sourceFactory.PrepareSourceNode(node, dc.get(), 0.0,
                    d3dDevice.get(), d3dContext.get());
            } catch (...) {}
        }
    }

    auto runEval = [&]() {
        // Refresh Working Space nodes from the active display profile, the
        // same way MainWindow::RenderWorkerLoop does (the pump lives on the
        // render worker, not in the frame draw). The /display/profile
        // routes sync on change, but a Working Space node ADDED while a
        // profile is already active never saw one -- it kept its 80-nit
        // hiddenDefaults, so anything bound to SdrWhiteNits silently
        // anchored SDR white 2.5x too low. Pumping it here is what the
        // header prescribes: the helper dead-bands each field and only
        // marks the node dirty when a value actually changed, so a stable
        // profile costs a graph walk and nothing else. Must run BEFORE the
        // BFS below so a changed field propagates to binding consumers in
        // the same evaluation.
        ShaderLab::Rendering::UpdateWorkingSpaceNodes(graph, displayMonitor);
        SettleVideoSources(sourceFactory, evaluator, graph, dc.get());

        // Propagate dirty flags downstream so D3D11 compute effects
        // re-dispatch when upstream sources change. Mirrors the BFS
        // walk in MainWindow::RenderFrameToOffscreen -- without this, a
        // set-property on an upstream node only re-evaluates that
        // node, leaving downstream analysis nodes' cached output
        // stale.
        {
            std::vector<uint32_t> queue;
            for (const auto& node : graph.Nodes())
                if (node.dirty) queue.push_back(node.id);
            for (size_t i = 0; i < queue.size(); ++i)
            {
                for (const auto* edge : graph.GetOutputEdges(queue[i]))
                {
                    auto* dn = graph.FindNode(edge->destNodeId);
                    if (dn && !dn->dirty)
                    {
                        dn->dirty = true;
                        queue.push_back(edge->destNodeId);
                    }
                }
            }
        }

        // Evaluate OUTSIDE the draw session, ProcessDeferredCompute INSIDE
        // it -- the exact structure of MainWindow::RenderFrameToOffscreen.
        //
        // Evaluate() can open its OWN BeginDraw/EndDraw (the D2D Histogram
        // and custom-analysis readbacks do), and nesting that inside an
        // outer session fails with D2DERR_WRONG_STATE. It used to be worse:
        // Evaluate also pre-rendered compute inputs that way, and every
        // compute effect then read black after the first set-property in a
        // --script run. That pre-render now happens only inside
        // ProcessDeferredCompute, which DOES need an active draw session --
        // it calls dc->DrawImage internally, which silently no-ops outside
        // one (CLAUDE.md records this as a trap). Hence the split.
        //
        // Second pass only when the first left something dirty, like the
        // GUI: an unconditional one re-queued every binding consumer of a
        // compute node queued in the first, dispatching it twice.
        dc->SetTarget(nullptr);
        evaluator.Evaluate(graph, dc.get());
        if (graph.HasDirtyNodes())
            evaluator.Evaluate(graph, dc.get());  // second pass for new effects
        dc->BeginDraw();
        evaluator.ProcessDeferredCompute(graph, dc.get());
        // Some compute nodes mark downstream dirty; one more sweep picks
        // those up. Frozen so this pass cannot re-queue a compute node and
        // re-enter the nested-BeginDraw path just avoided -- again mirroring
        // MainWindow::RenderFrameToOffscreen.
        if (graph.HasDirtyNodes())
        {
            evaluator.SetDeferredComputeFrozen(true);
            evaluator.Evaluate(graph, dc.get());
            evaluator.SetDeferredComputeFrozen(false);
        }
        dc->EndDraw();
    };
    runEval();  // initial frame so capture/pixel-region routes have something to read

    // ---- Build server + sink + register routes ----------------------------
    HeadlessSink sink;
    sink.mediaDir = &extractGuard;
    sink.populateContext = [&](ShaderLab::Mcp::EngineContext& ctx) {
        ctx.graph          = &graph;
        ctx.evaluator      = &evaluator;
        ctx.displayMonitor = &displayMonitor;
        ctx.sourceFactory  = &sourceFactory;
        ctx.dc             = dc.get();
        ctx.d3dDevice      = d3dDevice.get();
        ctx.d3dContext     = d3dContext.get();
        ctx.renderFrame    = runEval;  // routes that need fresh eval call this
        // Headless always renders scRGB FP16 (there is no swap chain or
        // RenderEngine to ask); /display/info reports this name.
        ctx.getPipelineFormatName = [] {
            return ShaderLab::Rendering::FormatScRgbFP16.name;
        };
    };

    ShaderLab::McpRouter server;
    ShaderLab::Mcp::RegisterEngineRoutes(server, sink);

    // POST /render/video -- headless only. The GUI does not register it: an
    // export holds the render thread for the whole encode, which the live
    // app cannot give up (and its dispatches time out at 30 s).
    server.AddRoute(L"POST", L"/render/video",
        [&](const std::wstring&, const std::wstring&, const std::string& body) -> ShaderLab::Mcp::Response
        {
            namespace WDJ = winrt::Windows::Data::Json;
            WDJ::JsonObject j{ nullptr };
            if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), j))
                return { 400, R"({"ok":false,"error":"body must be a JSON object"})" };
            // Untyped MCP args may arrive as strings ("60", "true") -- the
            // coercion rule in CLAUDE.md -- so every getter accepts both.
            auto str = [&](const wchar_t* k) -> std::wstring {
                if (!j.HasKey(k)) return {};
                auto v = j.GetNamedValue(k);
                if (v.ValueType() == WDJ::JsonValueType::String) return std::wstring(v.GetString());
                if (v.ValueType() == WDJ::JsonValueType::Number) return std::to_wstring(v.GetNumber());
                return {};
            };
            auto num = [&](const wchar_t* k) -> std::optional<double> {
                if (!j.HasKey(k)) return std::nullopt;
                auto v = j.GetNamedValue(k);
                if (v.ValueType() == WDJ::JsonValueType::Number) return v.GetNumber();
                if (v.ValueType() == WDJ::JsonValueType::String) {
                    std::wstring sv(v.GetString());
                    wchar_t* end = nullptr;
                    double d = std::wcstod(sv.c_str(), &end);
                    if (end && end != sv.c_str()) return d;
                }
                return std::nullopt;
            };
            auto flag = [&](const wchar_t* k) -> bool {
                if (!j.HasKey(k)) return false;
                auto v = j.GetNamedValue(k);
                if (v.ValueType() == WDJ::JsonValueType::Boolean) return v.GetBoolean();
                if (v.ValueType() == WDJ::JsonValueType::String) { auto sv = std::wstring(v.GetString()); return sv == L"true" || sv == L"1"; }
                if (v.ValueType() == WDJ::JsonValueType::Number) return v.GetNumber() != 0.0;
                return false;
            };

            ShaderLab::Rendering::VideoExportRequest req;
            std::wstring perr;
            if (!ParseVideoEnums(str(L"format"), str(L"codec"), str(L"mastering"), req, perr))
                return { 400, VideoResultJson({ false, perr }) };
            auto node = num(L"nodeId");
            req.outputPath = str(L"outputPath");
            if (!node || req.outputPath.empty())
                return { 400, R"({"ok":false,"error":"nodeId and outputPath are required"})" };
            req.nodeId = static_cast<uint32_t>(*node);
            if (auto v = num(L"fps")) req.fps = *v;
            if (auto v = num(L"start")) req.start = *v;
            if (auto v = num(L"duration")) req.duration = *v;
            if (auto v = num(L"crf")) req.crf = static_cast<int>(*v);
            req.preset = str(L"preset");
            req.lossless = flag(L"lossless");
            auto cll = num(L"maxCll"), fall = num(L"maxFall");
            if (cll && fall) { req.maxCll = static_cast<uint32_t>(*cll); req.maxFall = static_cast<uint32_t>(*fall); }
            req.ffmpegPath = str(L"ffmpegPath");

            ShaderLab::Mcp::EngineContext ctx;
            sink.populateContext(ctx);
            auto res = ShaderLab::Rendering::ExportVideo(req, *ctx.graph, *ctx.evaluator, *ctx.sourceFactory,
                ctx.displayMonitor, ctx.dc, ctx.d3dDevice, ctx.d3dContext);
            return { static_cast<uint16_t>(res.ok ? 200 : 500), VideoResultJson(res) };
        });

    // ---- MCP session mode (stdio-migration Step 6) ------------------------
    // Register with the broker hub as a session; the McpSessionClient
    // serves incoming channel requests by routing sealed JSON-RPC through
    // this same `server` router (its POST / dispatcher), so a shim-fronted
    // MCP client drives the graph. (The old --serve HTTP mode was removed in
    // Step 9 with the rest of the HTTP transport; this is the only MCP host
    // mode now, and what CI drives via a shim.)
    if (args.mcpSessionMode)
    {
        ShaderLab::Mcp::JsonRpcOptions rpcOptions;
        rpcOptions.hostKind = "headless";
        ShaderLab::Mcp::RegisterJsonRpcEndpoint(server, std::move(rpcOptions));

        ShaderLab::Mcp::SessionClientOptions sopts;
        sopts.pipeBaseName = args.pipeName;
        sopts.sessionId = args.sessionId;
        if (sopts.sessionId.empty())
        {
            GUID g{};
            CoCreateGuid(&g);
            wchar_t buf[64]{};
            StringFromGUID2(g, buf, ARRAYSIZE(buf));
            sopts.sessionId = buf;   // {....} form
        }
        sopts.label = args.sessionLabel.empty()
            ? std::format(L"headless {}", GetCurrentProcessId())
            : args.sessionLabel;

        std::wprintf(L"MCP session %ls (%ls). Kill the process to stop.\n",
            sopts.sessionId.c_str(), sopts.label.c_str());
        ShaderLab::Mcp::McpSessionClient client(server, std::move(sopts));
        client.Run();   // blocks until killed (reconnects with backoff)
        return 0;
    }

    // ---- Read script JSON -------------------------------------------------
    auto scriptText = ReadFileUtf8(args.scriptPath);
    if (scriptText.empty()) {
        std::wprintf(L"FATAL: could not read script file '%ls'\n", args.scriptPath.c_str());
        return 4;
    }
    int scriptWc = MultiByteToWideChar(CP_UTF8, 0, scriptText.data(),
        static_cast<int>(scriptText.size()), nullptr, 0);
    std::wstring scriptW(scriptWc, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, scriptText.data(),
        static_cast<int>(scriptText.size()), scriptW.data(), scriptWc);

    // Script can be either a top-level array of steps OR an object with
    // a "steps" array. The latter leaves room for future top-level
    // metadata (description, version, env hints) without a breaking
    // schema change.
    WDJ::JsonArray steps{ nullptr };
    {
        WDJ::JsonValue v{ nullptr };
        if (!WDJ::JsonValue::TryParse(winrt::hstring(scriptW), v)) {
            std::wprintf(L"FATAL: script is not valid JSON\n");
            return 5;
        }
        if (v.ValueType() == WDJ::JsonValueType::Array) {
            steps = v.GetArray();
        } else if (v.ValueType() == WDJ::JsonValueType::Object) {
            auto obj = v.GetObject();
            if (!obj.HasKey(L"steps")) {
                std::wprintf(L"FATAL: script object must have a 'steps' array\n");
                return 5;
            }
            steps = obj.GetNamedArray(L"steps");
        } else {
            std::wprintf(L"FATAL: script must be a JSON array or object with 'steps'\n");
            return 5;
        }
    }

    // ---- Walk steps -------------------------------------------------------
    // Translate each step to (method, path, body string), invoke the
    // server, and accumulate {step, status, body} into a result array.
    // body is parsed as JSON when possible so consumers can navigate
    // structured fields without re-parsing.
    auto stepBodyToString = [](const WDJ::JsonValue& v) -> std::string {
        if (v.ValueType() == WDJ::JsonValueType::String)
        {
            // Already-stringified body (escape-hatch when the agent
            // wants byte-exact control over the request body).
            auto h = v.GetString();
            int n = WideCharToMultiByte(CP_UTF8, 0, h.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, h.c_str(), -1, s.data(), n, nullptr, nullptr);
            return s;
        }
        // Object / number / bool / null -> stringify.
        auto h = v.Stringify();
        int n = WideCharToMultiByte(CP_UTF8, 0, h.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, h.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    };

    WDJ::JsonArray results;
    for (uint32_t i = 0; i < steps.Size(); ++i)
    {
        WDJ::JsonObject stepObj{ nullptr };
        if (steps.GetAt(i).ValueType() != WDJ::JsonValueType::Object)
        {
            WDJ::JsonObject err;
            err.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
            err.Insert(L"status", WDJ::JsonValue::CreateNumberValue(400));
            err.Insert(L"error", WDJ::JsonValue::CreateStringValue(L"Step is not an object"));
            results.Append(err);
            continue;
        }
        stepObj = steps.GetObjectAt(i);

        std::wstring method, path;
        std::string  body;

        if (stepObj.HasKey(L"op"))
        {
            // Shorthand: translate to (method, path, body).
            auto op = std::wstring(stepObj.GetNamedString(L"op"));
            WDJ::JsonObject bodyObj;
            for (auto kv : stepObj)
            {
                if (kv.Key() == L"op") continue;
                bodyObj.Insert(kv.Key(), kv.Value());
            }
            // Stringify the body once; reused for every shorthand that
            // mirrors a POST route. Stringify() returns the canonical
            // JSON encoding so the engine routes parse it identically
            // to a real HTTP request body.
            auto bodyHstr = bodyObj.Stringify();
            int bn = WideCharToMultiByte(CP_UTF8, 0, bodyHstr.c_str(), -1,
                nullptr, 0, nullptr, nullptr);
            std::string bodyStr(bn > 0 ? bn - 1 : 0, '\0');
            if (bn > 0) WideCharToMultiByte(CP_UTF8, 0, bodyHstr.c_str(), -1,
                bodyStr.data(), bn, nullptr, nullptr);

            if      (op == L"set-property")  { method = L"POST"; path = L"/graph/set-property";  body = bodyStr; }
            else if (op == L"pixel-region")  { method = L"POST"; path = L"/render/pixel-region"; body = bodyStr; }
            else if (op == L"capture-node")  { method = L"POST"; path = L"/render/capture-node"; body = bodyStr; }
            else if (op == L"get-graph")     { method = L"GET";  path = L"/graph";               body.clear(); }
            else if (op == L"get-node")      {
                method = L"GET";
                uint32_t id = stepObj.HasKey(L"nodeId")
                    ? static_cast<uint32_t>(stepObj.GetNamedNumber(L"nodeId")) : 0;
                path = L"/graph/node/" + std::to_wstring(id);
                body.clear();
            }
            else if (op == L"analysis")      {
                method = L"GET";
                uint32_t id = stepObj.HasKey(L"nodeId")
                    ? static_cast<uint32_t>(stepObj.GetNamedNumber(L"nodeId")) : 0;
                path = L"/analysis/" + std::to_wstring(id);
                body.clear();
            }
            else if (op == L"gpu-bench")     {
                // Purpose-built shader benchmark. Answers "how many GPU
                // milliseconds does this node chain cost per frame", which is
                // the question none of the pre-existing timings can answer.
                //
                // Three things it does that the obvious approach gets wrong:
                //  1. It DRAWS the node. D2D evaluates an effect chain lazily
                //     at DrawImage time, so a bare `render` rasterizes nothing
                //     and a naive sweep ends up timing an empty command stream.
                //  2. It draws into a full-size offscreen target, not a small
                //     readback tile -- D2D shades only the pixels asked for, so
                //     reading a 2x1 pixel-region makes any shader look free.
                //  3. It re-dirties the SOURCE every iteration, which is the
                //     real video workload: a new frame invalidates the whole
                //     chain so nothing can be served from cache.
                // It reports GPU time, so the PNG encode and process noise that
                // swamped earlier wall-clock attempts are simply absent.
                uint32_t benchNode = args.nodeId;
                if (stepObj.HasKey(L"nodeId"))
                    benchNode = static_cast<uint32_t>(stepObj.GetNamedNumber(L"nodeId"));
                uint32_t iters = 20;
                if (stepObj.HasKey(L"iterations"))
                    iters = static_cast<uint32_t>(stepObj.GetNamedNumber(L"iterations"));
                iters = (std::max)(1u, (std::min)(iters, 500u));

                WDJ::JsonObject body;
                auto* bn = graph.FindNode(benchNode);
                if (!gpuTimer.IsEnabled())
                {
                    body.Insert(L"error", WDJ::JsonValue::CreateStringValue(
                        L"GPU timing is off -- pass --gpu-timing"));
                }
                else if (!bn)
                {
                    body.Insert(L"error", WDJ::JsonValue::CreateStringValue(L"node not found"));
                }
                else
                {
                    // Target sized to the node output so we measure the real
                    // pixel count; overridable to project the same content to
                    // another resolution (4K, say).
                    uint32_t bw = 0, bh = 0;
                    if (bn->cachedOutput)
                    {
                        D2D1_RECT_F lb{};
                        if (SUCCEEDED(dc->GetImageLocalBounds(bn->cachedOutput, &lb)))
                        {
                            bw = static_cast<uint32_t>((std::max)(0.0f, lb.right - lb.left));
                            bh = static_cast<uint32_t>((std::max)(0.0f, lb.bottom - lb.top));
                        }
                    }
                    if (stepObj.HasKey(L"width"))
                        bw = static_cast<uint32_t>(stepObj.GetNamedNumber(L"width"));
                    if (stepObj.HasKey(L"height"))
                        bh = static_cast<uint32_t>(stepObj.GetNamedNumber(L"height"));
                    if (bw == 0 || bh == 0) { bw = args.width; bh = args.height; }

                    winrt::com_ptr<ID2D1Bitmap1> target;
                    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_TARGET,
                        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          D2D1_ALPHA_MODE_PREMULTIPLIED));
                    HRESULT hrT = dc->CreateBitmap(D2D1::SizeU(bw, bh), nullptr, 0, &bp,
                                                   target.put());
                    if (FAILED(hrT))
                    {
                        body.Insert(L"error", WDJ::JsonValue::CreateStringValue(
                            L"could not create bench target"));
                    }
                    else
                    {
                        std::vector<double> samples, frameSamples, evalSamples, cpuEvalMs, cpuFrameMs;
                        // Per compute node: exact dispatch time (the evaluator
                        // brackets each dispatch as its own submission).
                        std::map<uint32_t, std::vector<double>> nodeSamples;
                        samples.reserve(iters);
                        frameSamples.reserve(iters);
                        evalSamples.reserve(iters);
                        for (uint32_t k = 0; k < iters; ++k)
                        {
                            for (auto& nd : const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(graph.Nodes()))
                                if (nd.type == ShaderLab::Graph::NodeType::Source) nd.dirty = true;

                            // Frame span covers the WHOLE per-frame cost a video
                            // would pay: source upload, graph evaluation and the
                            // compute reductions in ProcessDeferredCompute, plus
                            // the draw. The Draw span nested inside it isolates
                            // just the pixel-shader chain.
                            gpuTimer.BeginFrame();
                            gpuTimer.Begin(ShaderLab::Rendering::GpuSpan::Frame);
                            // CPU wall-clock of the evaluation walk, alongside its
                            // GPU span. These measure different things and the gap
                            // between them is the point: the walk is mostly CPU
                            // work (property re-application, cache lookups, binding
                            // resolution) that submits very little to the GPU.
                            auto cpuEvalT0 = std::chrono::steady_clock::now();
                            runEval();
                            auto cpuEvalT1 = std::chrono::steady_clock::now();
                            cpuEvalMs.push_back(
                                std::chrono::duration<double, std::milli>(cpuEvalT1 - cpuEvalT0).count());
                            gpuTimer.End(ShaderLab::Rendering::GpuSpan::Evaluate, dc.get());

                            auto* img = bn->cachedOutput;
                            if (!img) break;
                            winrt::com_ptr<ID2D1Image> prev;
                            dc->GetTarget(prev.put());
                            dc->SetTarget(target.get());
                            gpuTimer.Begin(ShaderLab::Rendering::GpuSpan::Draw);
                            dc->BeginDraw();
                            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                            dc->DrawImage(img);
                            HRESULT hrE = dc->EndDraw();   // flushes, so the span can close
                            // CPU wall-clock of eval + draw submission. EndDraw does
                            // not wait for the GPU, so this is CPU work plus any
                            // synchronous GPU waits inside the frame (readback Maps).
                            cpuFrameMs.push_back(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - cpuEvalT0).count());
                            gpuTimer.End(ShaderLab::Rendering::GpuSpan::Draw);
                            gpuTimer.End(ShaderLab::Rendering::GpuSpan::Frame);
                            gpuTimer.EndFrame();
                            dc->SetTarget(prev.get());
                            if (FAILED(hrE)) break;
                            // Block for THIS iteration's result. Sampling
                            // without waiting records the previous frame's
                            // number repeatedly (min == median == max), and
                            // polling with DONOTFLUSH at a 4-frame lag lands
                            // only a handful of times in 60 iterations.
                            if (gpuTimer.CollectBlocking())
                            {
                                samples.push_back(gpuTimer.SpanMs(ShaderLab::Rendering::GpuSpan::Draw));
                                frameSamples.push_back(gpuTimer.SpanMs(ShaderLab::Rendering::GpuSpan::Frame));
                                evalSamples.push_back(gpuTimer.SpanMs(ShaderLab::Rendering::GpuSpan::Evaluate));
                                for (const auto& nd : graph.Nodes())
                                    if (nd.type == ShaderLab::Graph::NodeType::ComputeShader)
                                        if (double ms = gpuTimer.NodeMs(nd.id); ms > 0.0)
                                            nodeSamples[nd.id].push_back(ms);
                            }
                        }
                        double mpx = static_cast<double>(bw) * static_cast<double>(bh) / 1e6;
                        body.Insert(L"nodeId", WDJ::JsonValue::CreateNumberValue(benchNode));
                        body.Insert(L"width", WDJ::JsonValue::CreateNumberValue(bw));
                        body.Insert(L"height", WDJ::JsonValue::CreateNumberValue(bh));
                        body.Insert(L"megapixels", WDJ::JsonValue::CreateNumberValue(mpx));
                        body.Insert(L"iterations", WDJ::JsonValue::CreateNumberValue(iters));
                        body.Insert(L"samples", WDJ::JsonValue::CreateNumberValue(
                            static_cast<double>(samples.size())));
                        body.Insert(L"disjointDrops", WDJ::JsonValue::CreateNumberValue(
                            gpuTimer.DisjointDrops()));
                        if (!samples.empty())
                        {
                            std::vector<double> sorted = samples;
                            std::sort(sorted.begin(), sorted.end());
                            double sum = 0.0;
                            for (double v : sorted) sum += v;
                            body.Insert(L"gpuMinMs", WDJ::JsonValue::CreateNumberValue(sorted.front()));
                            body.Insert(L"gpuMedianMs", WDJ::JsonValue::CreateNumberValue(
                                sorted[sorted.size() / 2]));
                            body.Insert(L"gpuMeanMs", WDJ::JsonValue::CreateNumberValue(
                                sum / static_cast<double>(sorted.size())));
                            body.Insert(L"gpuMaxMs", WDJ::JsonValue::CreateNumberValue(sorted.back()));
                            if (mpx > 0.0)
                                body.Insert(L"gpuMsPerMpx", WDJ::JsonValue::CreateNumberValue(
                                    sorted.front() / mpx));
                        }
                        auto minOf = [](std::vector<double>& v) {
                            return v.empty() ? 0.0 : *std::min_element(v.begin(), v.end());
                        };
                        if (!frameSamples.empty())
                        {
                            // Whole video frame: eval + compute reductions + draw.
                            body.Insert(L"gpuFrameMinMs", WDJ::JsonValue::CreateNumberValue(
                                minOf(frameSamples)));
                            body.Insert(L"gpuEvalMinMs", WDJ::JsonValue::CreateNumberValue(
                                minOf(evalSamples)));
                            body.Insert(L"cpuEvalMinMs", WDJ::JsonValue::CreateNumberValue(
                                minOf(cpuEvalMs)));
                            auto medianOf = [](std::vector<double> v) {
                                if (v.empty()) return 0.0;
                                std::sort(v.begin(), v.end());
                                return v[v.size() / 2];
                            };
                            body.Insert(L"gpuFrameMedianMs", WDJ::JsonValue::CreateNumberValue(
                                medianOf(frameSamples)));
                            body.Insert(L"cpuEvalMedianMs", WDJ::JsonValue::CreateNumberValue(
                                medianOf(cpuEvalMs)));
                            body.Insert(L"cpuFrameMedianMs", WDJ::JsonValue::CreateNumberValue(
                                medianOf(cpuFrameMs)));
                            WDJ::JsonObject perNode;
                            for (auto& [nid, v] : nodeSamples)
                                perNode.Insert(std::to_wstring(nid),
                                    WDJ::JsonValue::CreateNumberValue(medianOf(v)));
                            body.Insert(L"computeNodeMedianMs", perNode);
                        }
                    }
                }
                WDJ::JsonObject ok;
                ok.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
                ok.Insert(L"status", WDJ::JsonValue::CreateNumberValue(200));
                ok.Insert(L"body", body);
                results.Append(ok);
                continue;
            }
            else if (op == L"render")        {
                // Internal: force a fresh evaluation. Useful as a
                // barrier between a batch of property mutations and a
                // subsequent readback when the agent wants to be
                // explicit about ordering. Image-stats / pixel-region /
                // capture-node already trigger evaluation themselves,
                // so this is rarely needed.
                runEval();
                WDJ::JsonObject ok;
                ok.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
                ok.Insert(L"status", WDJ::JsonValue::CreateNumberValue(200));
                WDJ::JsonObject inner;
                inner.Insert(L"ok", WDJ::JsonValue::CreateBooleanValue(true));
                ok.Insert(L"body", inner);
                results.Append(ok);
                continue;
            }
            else
            {
                WDJ::JsonObject err;
                err.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
                err.Insert(L"status", WDJ::JsonValue::CreateNumberValue(400));
                err.Insert(L"error", WDJ::JsonValue::CreateStringValue(
                    winrt::hstring(L"Unknown op: " + op)));
                results.Append(err);
                continue;
            }
        }
        else if (stepObj.HasKey(L"path"))
        {
            method = stepObj.HasKey(L"method")
                ? std::wstring(stepObj.GetNamedString(L"method"))
                : L"GET";
            path = std::wstring(stepObj.GetNamedString(L"path"));
            if (stepObj.HasKey(L"body"))
                body = stepBodyToString(stepObj.GetNamedValue(L"body"));
        }
        else
        {
            WDJ::JsonObject err;
            err.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
            err.Insert(L"status", WDJ::JsonValue::CreateNumberValue(400));
            err.Insert(L"error", WDJ::JsonValue::CreateStringValue(
                L"Step must have either 'op' or 'path'"));
            results.Append(err);
            continue;
        }

        auto resp = server.RouteRequest(method, path, body);

        WDJ::JsonObject entry;
        entry.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
        entry.Insert(L"method", WDJ::JsonValue::CreateStringValue(winrt::hstring(method)));
        entry.Insert(L"path", WDJ::JsonValue::CreateStringValue(winrt::hstring(path)));
        entry.Insert(L"status", WDJ::JsonValue::CreateNumberValue(resp.statusCode));

        // Attempt to parse response body as JSON; fall back to raw
        // string if parsing fails (e.g. plain text error responses).
        if (!resp.body.empty())
        {
            int rwc = MultiByteToWideChar(CP_UTF8, 0, resp.body.data(),
                static_cast<int>(resp.body.size()), nullptr, 0);
            std::wstring rwstr(rwc, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, resp.body.data(),
                static_cast<int>(resp.body.size()), rwstr.data(), rwc);

            WDJ::JsonValue parsed{ nullptr };
            if (WDJ::JsonValue::TryParse(winrt::hstring(rwstr), parsed))
                entry.Insert(L"body", parsed);
            else
                entry.Insert(L"body", WDJ::JsonValue::CreateStringValue(winrt::hstring(rwstr)));
        }
        else
        {
            entry.Insert(L"body", WDJ::JsonValue::CreateNullValue());
        }
        results.Append(entry);
    }

    // ---- Emit response document ------------------------------------------
    WDJ::JsonObject outDoc;
    outDoc.Insert(L"abiVersion", WDJ::JsonValue::CreateNumberValue(
        static_cast<double>(::ShaderLab_GetAbiVersion())));
    outDoc.Insert(L"stepCount", WDJ::JsonValue::CreateNumberValue(
        static_cast<double>(steps.Size())));
    outDoc.Insert(L"results", results);

    auto outH = outDoc.Stringify();
    int outN = WideCharToMultiByte(CP_UTF8, 0, outH.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string outUtf8(outN > 0 ? outN - 1 : 0, '\0');
    if (outN > 0) WideCharToMultiByte(CP_UTF8, 0, outH.c_str(), -1, outUtf8.data(), outN, nullptr, nullptr);

    if (!args.scriptOutputPath.empty())
    {
        std::ofstream f(args.scriptOutputPath, std::ios::binary);
        if (!f) {
            std::wprintf(L"FATAL: cannot open --script-output '%ls' for write\n",
                args.scriptOutputPath.c_str());
            return 7;
        }
        f.write(outUtf8.data(), static_cast<std::streamsize>(outUtf8.size()));
        std::wprintf(L"OK: ran %u step(s) -> %ls\n",
            steps.Size(), args.scriptOutputPath.c_str());
    }
    else
    {
        std::fwrite(outUtf8.data(), 1, outUtf8.size(), stdout);
        std::fputc('\n', stdout);
    }

    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::wprintf(L"ShaderLabHeadless (engine ABI %u)\n",
        static_cast<unsigned>(::ShaderLab_GetAbiVersion()));

    Args args;
    if (!ParseArgs(argc, argv, args)) {
        if (argc <= 1) PrintUsage(argv[0]);
        return 1;
    }

    if (::ShaderLab_GetAbiVersion() != SHADERLAB_ENGINE_ABI_VERSION) {
        std::wprintf(L"FATAL: engine ABI mismatch (header %u, DLL %u)\n",
            static_cast<unsigned>(SHADERLAB_ENGINE_ABI_VERSION),
            static_cast<unsigned>(::ShaderLab_GetAbiVersion()));
        return 2;
    }

    // Register user effects before any graph loads, so a graph holding an
    // older copy sees it as upgradable and add-node by name finds it.
    for (const auto& dir : args.effectsDirs)
    {
        auto& library = ShaderLab::Effects::ShaderLabEffects::Instance();
        const size_t loadedBefore = library.UserEffectReport().loaded.size();
        const size_t errorsBefore = library.UserEffectReport().errors.size();
        const auto& report = library.LoadUserEffects(dir);
        for (size_t i = loadedBefore; i < report.loaded.size(); ++i)
            std::fwprintf(stderr, L"User effect: %ls\n", report.loaded[i].c_str());
        for (size_t i = errorsBefore; i < report.errors.size(); ++i)
            std::fwprintf(stderr, L"[ShaderLab] user effect not loaded: %ls\n", report.errors[i].c_str());
        std::error_code directoryError;
        if (!std::filesystem::is_directory(dir, directoryError))
            std::fwprintf(stderr, L"[ShaderLab] --effects-dir %ls is not a directory\n", dir.c_str());
    }

    // Cache management modes (Phase 8 cache reaper). Run before any
    // engine init so we don't pay the D3D startup cost for a one-shot
    // cleanup. Wires the same %LOCALAPPDATA% root the GUI app uses.
    if (args.reapShaderCache || args.clearShaderCache)
    {
        wchar_t* localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) ||
            !localAppData)
        {
            std::wprintf(L"ERROR: could not resolve %%LOCALAPPDATA%%\n");
            return 3;
        }
        std::wstring cacheRoot = std::wstring(localAppData) + L"\\ShaderLab\\bytecode";
        ::CoTaskMemFree(localAppData);
        ShaderLab::Effects::BytecodeCache::Instance().SetDiskCacheRoot(cacheRoot);
        auto stats = args.clearShaderCache
            ? ShaderLab::Effects::BytecodeCache::Instance().ClearDisk()
            : ShaderLab::Effects::BytecodeCache::Instance().ReapDisk(args.reapStaleSec);
        std::wprintf(
            L"{\"filesDeleted\":%zu,\"bytesFreed\":%zu,\"errors\":%zu,\"mode\":\"%ls\"}\n",
            stats.filesDeleted, stats.bytesFreed, stats.errors,
            args.clearShaderCache ? L"clear" : L"reap");
        return 0;
    }

    winrt::init_apartment();
    ::MFStartup(MF_VERSION);

    // p8-feature-flag: apply --enable-gpu-bindings / --disable-gpu-bindings
    // before any engine work so the evaluator's binding-resolution pass
    // sees the correct flag state. Engine default is OFF for v1.6.
    if (args.enableGpuBindings.has_value())
        ShaderLab::Performance::SetGpuBindingsEnabled(*args.enableGpuBindings);

    // Model the GUI's readback policy for perf work. MainWindow turns this on
    // at startup; headless leaves it off so probing sees fresh analysis fields
    // every frame. Benchmarking without it measures a GPU->CPU round trip the
    // shipping app does not perform, which inflates every frame time that
    // involves a compute-analysis node.
    if (args.skipUnneededReadback)
        ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);

    // Run all engine work inside an inner scope so the GraphEvaluator
    // and other engine objects destruct before MFShutdown / apartment
    // teardown. (GraphEvaluator owns com_ptrs to D2D effects that need
    // the factory alive at destruction time.)
    int rc = (!args.scriptPath.empty() || args.mcpSessionMode)
        ? RunScript(args) : RunRender(args);

    ::MFShutdown();
    winrt::uninit_apartment();
    return rc;
}
