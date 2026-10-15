#pragma once

// Offline video export of one graph node through an external ffmpeg.
//
// The graph is stepped on a FIXED timeline -- frame n renders with every Clock
// at start + n / fps -- so a frame that takes 200 ms to render is exactly as
// correct as one that takes 2 ms. Wall time plays no part. Each frame sets the
// clocks with Graph::SetClocksToTime and runs one FrameExecutor frame with every
// node dirty.
//
// Colour conversion is ours, on the GPU, not ffmpeg's: the node's scRGB FP16
// output becomes limited-range 4:2:0 -- p010le (HDR10: BT.2020 primaries, PQ in
// absolute nits, scRGB 1.0 = 80 nits) or nv12 (SDR: BT.709, sRGB curve) -- with
// left-cosited chroma, and ffmpeg receives finished planes over stdin. Its
// scaler never touches colour, so matrix, range, transfer and chroma siting
// are all decided here and all tested here.
//
// HDR10 static metadata: the mastering display comes from a preset or the
// graph's Working Space; MaxCLL / MaxFALL are MEASURED from the exported
// frames. x265 needs them before the first frame (they go in SEI), so measured
// metadata costs a first render-only pass. Rendering is deterministic, and the
// second pass re-measures and reports any disagreement.
//
// Host-agnostic. Headless exposes it as --video and registers the
// /render/video route (Mcp::RegisterVideoExportRoute); the GUI does not.

#include "../EngineExport.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ID2D1DeviceContext;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace ShaderLab::Graph { class GraphModel; }

namespace ShaderLab::Rendering
{
    class GraphEvaluator;

    enum class VideoFormat { Hdr10, Sdr };
    // Default: HEVC for both formats' 10/8-bit variants except SDR, which
    // defaults to H.264 (the most widely playable SDR codec).
    enum class VideoCodec { Default, Hevc, Av1, H264 };
    enum class MasteringMode
    {
        P3D65_1000,     // P3-D65 primaries, 1000 / 0.005 nits -- the common HDR10 reference
        WorkingSpace,   // primaries + peak/min from the graph's Working Space node
        None,           // no static metadata at all; only the BT.2020/PQ signalling
    };

    struct VideoExportRequest
    {
        uint32_t        nodeId{ 0 };
        std::wstring    outputPath;
        VideoFormat     format{ VideoFormat::Hdr10 };
        VideoCodec      codec{ VideoCodec::Default };
        double          fps{ 60.0 };
        double          start{ 0.0 };                 // seconds on the export timeline
        std::optional<double> duration;               // unset -> longest Clock's span
        int             crf{ -1 };                    // -1 -> per-codec default
        std::wstring    preset;                       // empty -> per-codec default
        bool            lossless{ false };            // HEVC / H.264 only
        MasteringMode   mastering{ MasteringMode::P3D65_1000 };
        std::optional<uint32_t> maxCll;               // both set -> skip measuring
        std::optional<uint32_t> maxFall;
        std::wstring    ffmpegPath;                   // empty -> search PATH
    };

    struct VideoExportResult
    {
        bool            ok{ false };
        std::wstring    error;
        std::wstring    ffmpegPath;
        std::wstring    encoder;                      // e.g. "libx265"
        std::wstring    commandLine;                  // the ffmpeg invocation, for the record
        uint32_t        width{ 0 };
        uint32_t        height{ 0 };
        uint32_t        frames{ 0 };
        double          fps{ 0.0 };
        double          start{ 0.0 };
        double          duration{ 0.0 };
        // Measured on the exported (encoded) signal: BT.2020 linear nits,
        // clamped to [0, 10000]. Also filled for SDR (in nits at 80/unit).
        double          maxCll{ 0.0 };
        double          maxFall{ 0.0 };
        bool            staticMetadataWritten{ false };
        double          seconds{ 0.0 };               // wall time of the whole export
        std::vector<std::wstring> warnings;
        std::string     ffmpegLogTail;                // last ~8 KB of ffmpeg's stderr
    };

    // Resolve ffmpeg: an explicit path must exist; otherwise search PATH.
    // Returns empty and fills `error` with instructions when neither works.
    SHADERLAB_API std::wstring FindFfmpeg(const std::wstring& explicitPath, std::wstring& error);

    // The conversion math as HLSL functions (VideoEncodeHdr10 / VideoEncodeSdr
    // / VideoChromaHdr10 / VideoChromaSdr), without the kernel -- exposed so
    // the shader test bench can check them against reference values.
    SHADERLAB_API const std::string& VideoConvertHelpersHLSL();

    // Sets request.format, codec and mastering from their --video / MCP
    // spellings (hdr10|sdr, hevc|h265|av1|h264|avc, p3-1000|working-space|none;
    // empty picks the default). False, with `error` naming the valid values,
    // for anything else.
    SHADERLAB_API bool ParseVideoOptions(const std::wstring& format, const std::wstring& codec,
                                         const std::wstring& mastering, VideoExportRequest& request,
                                         std::wstring& error);

    // The codec an export encodes with: Default is HEVC for HDR10 and H.264
    // for SDR. `error` says why the request's combination is refused.
    SHADERLAB_API VideoCodec ResolveVideoCodec(const VideoExportRequest& request, std::wstring& error);
    // ffmpeg's encoder for a codec: libx265, libsvtav1 or libx264.
    SHADERLAB_API const wchar_t* EncoderName(VideoCodec codec);

    // An HDR10 mastering display: CIE xy primaries and white point, and its
    // luminance range in nits.
    struct MasteringDisplay
    {
        float rx, ry, gx, gy, bx, by, wx, wy, maxNits, minNits;
    };
    // P3-D65 at 1000 / 0.005 nits (MasteringMode::P3D65_1000).
    constexpr MasteringDisplay DefaultMasteringDisplay()
    {
        return { 0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f, 1000.0f, 0.005f };
    }

    // What the encode needs beyond the request.
    struct FfmpegEncodeSettings
    {
        VideoCodec codec{ VideoCodec::Hevc };   // resolved (ResolveVideoCodec)
        uint32_t width{ 0 };
        uint32_t height{ 0 };
        std::optional<MasteringDisplay> mastering;   // unset: no static metadata
        uint32_t maxCll{ 0 };
        uint32_t maxFall{ 0 };
    };

    // ffmpeg's arguments, without the executable, for encoding raw p010le /
    // nv12 frames from stdin into request.outputPath.
    SHADERLAB_API std::wstring BuildFfmpegArgs(const VideoExportRequest& request,
                                               const FfmpegEncodeSettings& settings);

    SHADERLAB_API VideoExportResult ExportVideo(
        const VideoExportRequest& request,
        Graph::GraphModel& graph,
        GraphEvaluator& evaluator,
        ID2D1DeviceContext* dc,
        ID3D11Device* device,
        ID3D11DeviceContext* immediateContext);
}
