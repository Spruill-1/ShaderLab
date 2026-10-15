// ShaderLabHeadless: the engine's console host. It renders a node of a graph
// to an image, reads FP32 pixels back, exports video, runs a JSON script of
// MCP operations, or serves as an MCP session, with no WinUI and no swap
// chain. HeadlessArgs.h lists the flags and exit codes; docs/hosts/headless.md
// describes the modes.

#include "pch_engine.h"
#include "EngineExport.h"
#include "HeadlessArgs.h"
#include "HeadlessEngine.h"
#include "ImageOutput.h"
#include "ScriptRunner.h"

#include "../Effects/BytecodeCache.h"
#include "../Effects/ShaderLabEffects.h"
#include "../Engine/EngineBootstrap.h"
#include "../Engine/Mcp/McpJsonRpc.h"
#include "../Engine/Mcp/McpRouter.h"
#include "../Engine/Mcp/McpSessionClient.h"
#include "../Rendering/PixelReadback.h"
#include "../Rendering/VideoExport.h"

#include <shlobj.h>
#include <KnownFolders.h>
#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
    using namespace ShaderLab::Headless;
    namespace Rendering = ShaderLab::Rendering;

    // The requested node, or a printed failure.
    const ShaderLab::Graph::EffectNode* RenderedNode(HeadlessEngine& engine, uint32_t nodeId)
    {
        const HRESULT endDraw = engine.RenderFrameFor(nodeId);
        if (FAILED(endDraw))
            std::fwprintf(stderr, L"[ShaderLab] frame EndDraw failed 0x%08X\n", static_cast<uint32_t>(endDraw));

        const auto* node = engine.Model().FindNode(nodeId);
        if (!node || !node->cachedOutput)
        {
            std::wprintf(L"FATAL: node %u has no cachedOutput after evaluate (missing inputs or eval error)\n", nodeId);
            if (node && !node->runtimeError.empty())
                std::wprintf(L"        node.runtimeError = %ls\n", node->runtimeError.c_str());
            return nullptr;
        }
        return node;
    }

    // --pixels: the region as raw FP32 RGBA, no tone map.
    ExitCode RunPixels(HeadlessEngine& engine, const Args& args)
    {
        if (!RenderedNode(engine, args.nodeId)) return ExitCode::NoOutput;

        auto region = Rendering::ReadPixelRegion(engine.Model(), args.nodeId,
            args.pixelX, args.pixelY, args.pixelW, args.pixelH, engine.Dc());
        switch (region.status)
        {
        case Rendering::ReadPixelRegionStatus::Success: break;
        case Rendering::ReadPixelRegionStatus::NotFound:
            std::wprintf(L"FATAL: pixel readback NotFound for node %u\n", args.nodeId);
            return ExitCode::PixelRegion;
        case Rendering::ReadPixelRegionStatus::NotReady:
            std::wprintf(L"FATAL: pixel readback NotReady for node %u (dirty or missing inputs)\n", args.nodeId);
            return ExitCode::PixelRegion;
        case Rendering::ReadPixelRegionStatus::InvalidRegion:
            std::wprintf(L"FATAL: pixel readback InvalidRegion (clipped to nothing inside image)\n");
            return ExitCode::PixelRegion;
        case Rendering::ReadPixelRegionStatus::D2DError:
        default:
            std::wprintf(L"FATAL: pixel readback D2DError 0x%08X\n", static_cast<unsigned>(region.hr));
            return ExitCode::Readback;
        }

        bool csv = false;
        if (!WritePixelRegionFile(args.outputPath, region, args.pixelX, args.pixelY, csv))
        {
            std::wprintf(L"FATAL: could not open output '%ls'\n", args.outputPath.c_str());
            return ExitCode::OutputWrite;
        }
        std::wprintf(L"OK: read %ux%u pixel region (%s) from node %u -> %ls\n",
            region.actualWidth, region.actualHeight, csv ? L"csv" : L"binary",
            args.nodeId, args.outputPath.c_str());
        return ExitCode::Ok;
    }

    // --output: draw the node (through HdrToneMap unless skipped) into an
    // FP32 target, read it back and encode by extension.
    ExitCode RunImage(HeadlessEngine& engine, const Args& args)
    {
        const auto* node = RenderedNode(engine, args.nodeId);
        if (!node) return ExitCode::NoOutput;
        auto* dc = engine.Dc();

        D2D1_BITMAP_PROPERTIES1 targetProps{};
        targetProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        targetProps.dpiX = 96.0f;
        targetProps.dpiY = 96.0f;
        winrt::com_ptr<ID2D1Bitmap1> target;
        HRESULT hr = dc->CreateBitmap(D2D1::SizeU(args.width, args.height), nullptr, 0, targetProps, target.put());
        if (FAILED(hr))
        {
            std::wprintf(L"FATAL: CreateBitmap (target) failed 0x%08X\n", static_cast<uint32_t>(hr));
            return ExitCode::RenderTarget;
        }

        D2D1_BITMAP_PROPERTIES1 stagingProps = targetProps;
        stagingProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        winrt::com_ptr<ID2D1Bitmap1> staging;
        hr = dc->CreateBitmap(D2D1::SizeU(args.width, args.height), nullptr, 0, stagingProps, staging.put());
        if (FAILED(hr))
        {
            std::wprintf(L"FATAL: CreateBitmap (staging) failed 0x%08X\n", static_cast<uint32_t>(hr));
            return ExitCode::RenderTarget;
        }

        // The PNG gets HdrToneMap's fixed BT.2408-style mid-tone lift
        // (decision #52) for visual inspection. A .jxr / .wdp exists to keep
        // HDR, so it skips the tone map unless a peak was named: tone mapping
        // into an HDR deliverable is still a legitimate request.
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
            if (FAILED(hr))
            {
                std::wprintf(L"FATAL: CreateEffect(HdrToneMap) failed 0x%08X\n", static_cast<uint32_t>(hr));
                return ExitCode::Draw;
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
        if (FAILED(hr))
        {
            std::wprintf(L"FATAL: D2D EndDraw failed 0x%08X\n", static_cast<uint32_t>(hr));
            return ExitCode::Draw;
        }

        D2D1_POINT_2U dstPt = { 0, 0 };
        D2D1_RECT_U srcRect = { 0, 0, args.width, args.height };
        hr = staging->CopyFromBitmap(&dstPt, target.get(), &srcRect);
        if (FAILED(hr))
        {
            std::wprintf(L"FATAL: CopyFromBitmap failed 0x%08X\n", static_cast<uint32_t>(hr));
            return ExitCode::Readback;
        }

        D2D1_MAPPED_RECT mapped{};
        hr = staging->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr))
        {
            std::wprintf(L"FATAL: Map staging failed 0x%08X\n", static_cast<uint32_t>(hr));
            return ExitCode::Readback;
        }

        winrt::com_ptr<IWICImagingFactory> wic;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
        if (FAILED(hr))
        {
            std::wprintf(L"FATAL: WIC factory failed 0x%08X\n", static_cast<uint32_t>(hr));
            staging->Unmap();
            return ExitCode::Encode;
        }

        hr = SaveFp32Image(wic.get(), reinterpret_cast<const float*>(mapped.bits),
            args.width, args.height, mapped.pitch, args.outputPath);
        staging->Unmap();
        if (FAILED(hr))
        {
            std::wprintf(L"FATAL: image encode failed 0x%08X\n", static_cast<uint32_t>(hr));
            return ExitCode::Encode;
        }

        std::wprintf(L"OK: rendered node %u (%ux%u) -> %ls\n",
            args.nodeId, args.width, args.height, args.outputPath.c_str());
        return ExitCode::Ok;
    }

    ExitCode RunVideo(HeadlessEngine& engine, const Args& args)
    {
        Rendering::VideoExportRequest req;
        std::wstring optionError;
        if (!Rendering::ParseVideoOptions(args.videoFormat, args.videoCodec, args.videoMastering, req, optionError))
        {
            std::wprintf(L"FATAL: %ls\n", optionError.c_str());
            return ExitCode::Usage;
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

        engine.SyncFrameInputs();
        auto res = Rendering::ExportVideo(req, engine.Model(), engine.Evaluator(),
            engine.Dc(), engine.D3DDevice(), engine.D3DContext());
        for (const auto& w : res.warnings) std::wprintf(L"WARNING: %ls\n", w.c_str());
        if (!res.ok)
        {
            std::wprintf(L"FATAL: video export failed: %ls\n", res.error.c_str());
            if (!res.ffmpegLogTail.empty())
                std::printf("---- ffmpeg log (tail) ----\n%s\n", res.ffmpegLogTail.c_str());
            return ExitCode::VideoExport;
        }
        std::wprintf(L"OK: wrote %ls -- %ux%u, %u frames @ %.3f fps (%.3f s), %ls, MaxCLL %.1f / MaxFALL %.1f nits%ls, %.1f s\n",
            args.videoPath.c_str(), res.width, res.height, res.frames, res.fps, res.duration,
            res.encoder.c_str(), res.maxCll, res.maxFall,
            res.staticMetadataWritten ? L" (in headers)" : L"", res.seconds);
        return ExitCode::Ok;
    }

    // Registers with the broker hub as a session and serves sealed JSON-RPC
    // through `router` until the process is killed.
    ExitCode RunMcpSession(ShaderLab::McpRouter& router, const Args& args)
    {
        ShaderLab::Mcp::JsonRpcOptions rpcOptions;
        rpcOptions.hostKind = "headless";
        ShaderLab::Mcp::RegisterJsonRpcEndpoint(router, std::move(rpcOptions));

        ShaderLab::Mcp::SessionClientOptions options;
        options.pipeBaseName = args.pipeName;
        options.sessionId = args.sessionId;
        if (options.sessionId.empty())
        {
            GUID g{};
            CoCreateGuid(&g);
            wchar_t buf[64]{};
            StringFromGUID2(g, buf, ARRAYSIZE(buf));
            options.sessionId = buf;
        }
        options.label = args.sessionLabel.empty()
            ? std::format(L"headless {}", GetCurrentProcessId())
            : args.sessionLabel;

        std::wprintf(L"MCP session %ls (%ls). Kill the process to stop.\n",
            options.sessionId.c_str(), options.label.c_str());
        ShaderLab::Mcp::McpSessionClient client(router, std::move(options));
        client.Run();   // reconnects with backoff until the process is killed
        return ExitCode::Ok;
    }

    // Every mode that loads a graph. The engine lives only inside this call,
    // so its device objects are released before MFShutdown.
    ExitCode RunGraphMode(Mode mode, const Args& args)
    {
        ExitCode failure = ExitCode::Ok;
        auto engine = HeadlessEngine::Open(args, failure);
        if (!engine) return failure;

        const bool rendersNode = mode == Mode::Image || mode == Mode::Pixels || mode == Mode::Video;
        if (rendersNode && !engine->Model().FindNode(args.nodeId))
        {
            std::wprintf(L"FATAL: node id %u not found in graph\n", args.nodeId);
            return ExitCode::NodeMissing;
        }

        // --video sets the clocks itself for every frame.
        engine->PrepareGraph(mode == Mode::Video ? std::nullopt : args.time);

        switch (mode)
        {
        case Mode::Image:  return RunImage(*engine, args);
        case Mode::Pixels: return RunPixels(*engine, args);
        case Mode::Video:  return RunVideo(*engine, args);
        default: break;
        }

        // Script and session modes answer engine routes; the first frame
        // gives readback routes something to read.
        engine->RenderFrame();
        ShaderLab::McpRouter router;
        engine->RegisterRoutes(router);
        return mode == Mode::McpSession
            ? RunMcpSession(router, args)
            : RunScriptFile(*engine, router, args);
    }

    // Deletes stale (or all) cached shader bytecode under the GUI's
    // %LOCALAPPDATA% root, without a device or graph.
    ExitCode RunShaderCache(const Args& args)
    {
        wchar_t* localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) || !localAppData)
        {
            std::wprintf(L"ERROR: could not resolve %%LOCALAPPDATA%%\n");
            return ExitCode::Device;
        }
        std::wstring cacheRoot = std::wstring(localAppData) + L"\\ShaderLab\\bytecode";
        ::CoTaskMemFree(localAppData);
        auto& cache = ShaderLab::Effects::BytecodeCache::Instance();
        cache.SetDiskCacheRoot(cacheRoot);
        auto stats = args.clearShaderCache ? cache.ClearDisk() : cache.ReapDisk(args.reapStaleSec);
        std::wprintf(
            L"{\"filesDeleted\":%zu,\"bytesFreed\":%zu,\"errors\":%zu,\"mode\":\"%ls\"}\n",
            stats.filesDeleted, stats.bytesFreed, stats.errors,
            args.clearShaderCache ? L"clear" : L"reap");
        return ExitCode::Ok;
    }

    // Registers --effects-dir libraries before any graph loads, so a graph
    // holding an older copy sees it as upgradable and add-node finds it.
    void RegisterUserEffects(const Args& args)
    {
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
    }
}

int wmain(int argc, wchar_t* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::wprintf(L"ShaderLabHeadless (engine ABI %u)\n",
        static_cast<unsigned>(::ShaderLab_GetAbiVersion()));

    Args args;
    const ParseResult parsed = ParseArgs(argc, argv, args);
    if (parsed.outcome == ParseOutcome::Help || argc <= 1)
    {
        PrintUsage(argv[0], ScriptRunner::ShorthandNames());
        return ToInt(parsed.outcome == ParseOutcome::Help ? ExitCode::Ok : ExitCode::Usage);
    }
    if (parsed.outcome == ParseOutcome::Error)
    {
        std::wprintf(L"ERROR: %ls\n", parsed.error.c_str());
        return ToInt(ExitCode::Usage);
    }
    for (const auto& warning : parsed.warnings)
        std::fwprintf(stderr, L"WARNING: %ls\n", warning.c_str());

    if (::ShaderLab_GetAbiVersion() != SHADERLAB_ENGINE_ABI_VERSION)
    {
        std::wprintf(L"FATAL: engine ABI mismatch (header %u, DLL %u)\n",
            static_cast<unsigned>(SHADERLAB_ENGINE_ABI_VERSION),
            static_cast<unsigned>(::ShaderLab_GetAbiVersion()));
        return ToInt(ExitCode::AbiMismatch);
    }
    // Joins the engine's worker threads on every return path below.
    struct EngineServicesScope
    {
        ~EngineServicesScope() { ShaderLab::Engine::ShutdownEngineServices(); }
    } engineServices;

    RegisterUserEffects(args);

    if (parsed.mode == Mode::ShaderCache)
        return ToInt(RunShaderCache(args));

    winrt::init_apartment();
    ::MFStartup(MF_VERSION);
    const ExitCode result = RunGraphMode(parsed.mode, args);
    ::MFShutdown();
    winrt::uninit_apartment();
    return ToInt(result);
}
