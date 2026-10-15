#pragma once

// ShaderLabHeadless's command line: the flags, the mode they select, and the
// process exit codes. Parsing is pure (no device, no printing) so the tests
// can drive it.

#include "../Rendering/DisplayProfile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ShaderLab::Headless
{
    // Process exit codes; docs/hosts/headless.md lists them. Each failure
    // kind has its own code so a caller can tell them apart.
    enum class ExitCode : int
    {
        Ok            = 0,
        Usage         = 1,   // bad or missing argument
        AbiMismatch   = 2,   // ShaderLabEngine.dll does not match these headers
        Device        = 3,   // D3D11 / D2D device, effect registration, %LOCALAPPDATA%
        InputRead     = 4,   // --graph or --script file unreadable
        InputParse    = 5,   // --graph or --script file is not valid
        NodeMissing   = 6,   // --node is not in the graph
        NoOutput      = 7,   // the node has no output after the frame
        VideoExport   = 8,   // --video failed
        RenderTarget  = 9,   // creating the FP32 target or staging bitmap
        Draw          = 10,  // HdrToneMap creation or the draw itself
        Readback      = 11,  // copying or mapping pixels back to the CPU
        PixelRegion   = 12,  // --pixels: region not found, not ready or empty
        OutputWrite   = 13,  // --output / --script-output cannot be opened
        Encode        = 14,  // WIC factory or image encode
    };

    constexpr int ToInt(ExitCode code) { return static_cast<int>(code); }

    // What a run does; flags that the mode does not read are reported.
    enum class Mode
    {
        Image,          // --output IMAGE
        Pixels,         // --pixels x,y,w,h --output FILE
        Video,          // --video PATH
        Script,         // --script PATH
        McpSession,     // --mcp-session
        ShaderCache,    // --reap-shader-cache / --clear-shader-cache
    };

    const wchar_t* ModeName(Mode mode);

    struct Args
    {
        std::wstring graphPath;
        uint32_t     nodeId{ 0 };
        bool         hasNodeId{ false };
        std::wstring outputPath;
        uint32_t     width{ 1024 };     // Image size; script mode's gpu-bench fallback
        uint32_t     height{ 1024 };
        bool         useWarp{ false };
        // D3D11 timestamp sampling for the gpu-bench script op. Off by
        // default: closing a GPU span needs a Flush, which perturbs the frame.
        bool         gpuTiming{ false };
        // Models the GUI's readback policy for perf work; probing wants
        // fresh analysis fields every frame, so it is off by default.
        bool         skipUnneededReadback{ false };

        // D2D HdrToneMap for the 8-bit PNG: source peak and target peak in
        // nits (80 = scRGB 1.0, SDR).
        float        inputPeakNits{ 1000.0f };
        float        outputPeakNits{ 80.0f };
        bool         skipToneMap{ false };
        // A peak was named, so tone mapping wins over a JXR output's
        // default of skipping it (e.g. 4000-nit content to a 1000-nit target).
        bool         toneMapExplicit{ false };

        // User effect libraries to register (repeatable). Headless never
        // reads the GUI's default folder.
        std::vector<std::wstring> effectsDirs;

        // Display-profile pin. Without one, a Working Space node follows the
        // live primary monitor, so a golden image would depend on the
        // machine's display settings. The preset is fully determined; the
        // two overrides pin luminance anchors on top of the preset, or on
        // top of the live profile when no preset is named.
        std::wstring displayProfile;            // preset id; empty -> live
        float        displaySdrWhite{ 0.0f };   // >0 -> override
        float        displayPeakNits{ 0.0f };   // >0 -> override

        // --pixels: FP32 RGBA region written raw (.bin/.raw) or as CSV.
        bool         pixelMode{ false };
        int32_t      pixelX{ 0 };
        int32_t      pixelY{ 0 };
        uint32_t     pixelW{ 1 };
        uint32_t     pixelH{ 1 };

        // Script batch mode: MCP-shaped ops run against the graph.
        std::wstring scriptPath;
        std::wstring scriptOutputPath;  // empty -> stdout

        // MCP session mode: register with the broker hub and serve requests.
        bool         mcpSessionMode{ false };
        std::wstring sessionId;         // empty -> a fresh GUID
        std::wstring sessionLabel;
        std::wstring pipeName;          // broker pipe base override

        // Bytecode-cache maintenance; runs without a device or graph.
        bool          reapShaderCache{ false };
        bool          clearShaderCache{ false };
        uint64_t      reapStaleSec{ 90ull * 24 * 60 * 60 };

        // --enable/--disable-gpu-bindings; unset follows
        // Performance::DefaultEvaluatorOptions().
        std::optional<bool> enableGpuBindings;

        // --video: fixed-timeline export through ffmpeg (Rendering/VideoExport.h).
        std::wstring videoPath;
        std::wstring videoFormat{ L"hdr10" };
        std::wstring videoCodec;                // empty -> per-format default
        double       videoFps{ 60.0 };
        double       videoStart{ 0.0 };
        std::optional<double> videoDuration;    // unset -> longest Clock
        int          videoCrf{ -1 };            // -1 -> per-codec default
        std::wstring videoPreset;
        bool         videoLossless{ false };
        std::wstring videoMastering{ L"p3-1000" };
        std::optional<uint32_t> videoMaxCll;
        std::optional<uint32_t> videoMaxFall;
        std::wstring ffmpegPath;                // empty -> search PATH

        // --time T: every Clock is set to export-timeline time T before the
        // first frame, so one frame renders exactly as --video renders it.
        std::optional<double> time;
    };

    enum class ParseOutcome
    {
        Run,    // args are complete; run `mode`
        Help,   // --help was asked for
        Error,  // `error` says why
    };

    struct ParseResult
    {
        ParseOutcome outcome{ ParseOutcome::Run };
        Mode         mode{ Mode::Image };
        std::wstring error;
        // Flags that were accepted but that `mode` does not read.
        std::vector<std::wstring> warnings;
    };

    // Parses argv[1..argc). Every value is checked in full: a number with
    // trailing text, an unknown enum value or a missing required flag is an
    // error, never a silent default.
    ParseResult ParseArgs(int argc, const wchar_t* const argv[], Args& out);

    // The --display-profile presets, by CLI name.
    std::optional<Rendering::DisplayProfile> DisplayPresetByName(std::wstring_view name);
    extern const wchar_t* const cDisplayPresetNames;

    // `scriptOps` lists the script shorthands (ScriptRunner owns the table).
    void PrintUsage(const wchar_t* exeName, std::wstring_view scriptOps);
}
