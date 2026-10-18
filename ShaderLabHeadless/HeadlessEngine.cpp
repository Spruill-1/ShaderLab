#include "pch_engine.h"
#include "HeadlessEngine.h"

#include "../Mcp/McpRouter.h"
#include "Graph/ClockModel.h"
#include "../Support/EffectGraphFile.h"
#include "../Support/PackageRoutes.h"
#include "Rendering/FrameExecutor.h"
#include "Rendering/PipelineFormat.h"
#include "Rendering/SourceRuntime.h"
#include "Rendering/WorkingSpaceSync.h"
#include "Effects/Performance.h"

#include <cstdio>
#include <filesystem>

namespace ShaderLab::Headless
{
    namespace
    {
        void RemoveExtractDir(const std::wstring& dir)
        {
            if (dir.empty()) return;
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);   // best effort: a temp directory
        }

        // The engine defaults, overridden by --enable/--disable-gpu-bindings
        // and --skip-unneeded-readback.
        Rendering::EvaluatorOptions EvaluatorOptionsFor(const Args& args)
        {
            auto options = Performance::DefaultEvaluatorOptions();
            if (args.enableGpuBindings.has_value())
                options.gpuBindings = *args.enableGpuBindings;
            options.skipUnneededCpuReadback = args.skipUnneededReadback;
            return options;
        }

        // Applies the --display-* pin; without one the monitor keeps
        // tracking the live display. The overrides layer on the preset, or on
        // the live profile when no preset is named, and only move luminance
        // anchors, so the active color mode stays as the base set it.
        void ApplyDisplayPin(const Args& args, Rendering::DisplayMonitor& monitor)
        {
            const bool wantsPreset   = !args.displayProfile.empty();
            const bool wantsOverride = args.displaySdrWhite > 0.0f || args.displayPeakNits > 0.0f;
            if (!wantsPreset && !wantsOverride) return;

            // ParseArgs rejected unknown preset names.
            auto p = wantsPreset
                ? DisplayPresetByName(args.displayProfile).value_or(monitor.LiveProfile())
                : monitor.LiveProfile();

            if (args.displaySdrWhite > 0.0f)
                p.caps.sdrWhiteLevelNits = args.displaySdrWhite;
            if (args.displayPeakNits > 0.0f)
            {
                p.caps.maxLuminanceNits = args.displayPeakNits;
                // Keep maxFullFrame <= peak, as the presets do.
                if (p.caps.maxFullFrameLuminanceNits > args.displayPeakNits)
                    p.caps.maxFullFrameLuminanceNits = args.displayPeakNits;
            }

            p.isSimulated = true;
            p.profileName = p.profileName.empty()
                ? std::wstring(L"CLI display pin")
                : p.profileName + L" (CLI pin)";
            monitor.SetSimulatedProfile(p);
        }

        // Loads a .effectgraph package (media extracted to a temp directory,
        // "media://" tokens rewritten as File > Open does) or a bare JSON graph.
        bool LoadGraphFromPath(const std::wstring& path, Graph::GraphModel& graph,
                               std::wstring& extractDir, ExitCode& failure)
        {
            wchar_t tempRoot[MAX_PATH + 1]{};
            GetTempPathW(MAX_PATH + 1, tempRoot);
            std::wstring loadError;
            auto loaded = Support::EffectGraphFile::LoadAny(path, tempRoot, loadError);
            if (!loaded.has_value())
            {
                std::wprintf(L"FATAL: could not read graph file: %ls\n", loadError.c_str());
                failure = ExitCode::InputRead;
                return false;
            }

            std::wstring parseError;
            try {
                graph = Graph::GraphModel::FromJson(winrt::hstring(loaded->graphJson));
            } catch (winrt::hresult_error const& e) {
                parseError = std::format(L"0x{:08X}: {}", static_cast<uint32_t>(e.code()), std::wstring(e.message()));
            } catch (std::exception const& e) {
                parseError = winrt::to_hstring(e.what());
            }
            if (!parseError.empty())
            {
                std::wprintf(L"FATAL: graph JSON parse failed (%ls)\n", parseError.c_str());
                RemoveExtractDir(loaded->extractDir);
                failure = ExitCode::InputParse;
                return false;
            }

            Support::EffectGraphFile::ResolveMediaTokens(graph, loaded->mediaMap);
            extractDir = loaded->extractDir;
            return true;
        }
    }

    ExtractDirGuard::~ExtractDirGuard()
    {
        RemoveExtractDir(m_dir);
        for (const auto& leftover : m_retry) RemoveExtractDir(leftover);
    }

    void ExtractDirGuard::Replace(std::wstring next)
    {
        if (next == m_dir) return;
        RemoveExtractDir(m_dir);
        std::error_code ec;
        if (!m_dir.empty() && std::filesystem::exists(m_dir, ec)) m_retry.push_back(m_dir);
        m_dir = std::move(next);
    }

    std::unique_ptr<HeadlessEngine> HeadlessEngine::Open(const Args& args, ExitCode& failure)
    {
        std::unique_ptr<HeadlessEngine> engine(new HeadlessEngine());

        // The adapter is a host choice; the context builds D2D on its device.
        Rendering::DeviceStackOptions deviceOptions;
        deviceOptions.choice = args.useWarp ? Rendering::AdapterChoice::Warp : Rendering::AdapterChoice::Hardware;
        Rendering::DeviceStack device;
        std::wstring failedStep;
        HRESULT hr = Rendering::CreateDeviceStack(deviceOptions, device, &failedStep);
        if (SUCCEEDED(hr))
        {
            failedStep = L"EffectGraphCreateContext";
            hr = EffectGraphCreateContext(device.d3dDevice.get(), nullptr, __uuidof(IEffectGraphContextInternal),
                                          engine->m_context.put_void());
        }
        if (FAILED(hr))
        {
            std::wprintf(L"FATAL: %ls failed 0x%08X\n", failedStep.c_str(), static_cast<uint32_t>(hr));
            failure = ExitCode::Device;
            return nullptr;
        }
        engine->m_dc.copy_from(engine->m_context->DeviceContext());
        engine->m_adapterChoice = deviceOptions.choice;

        std::wstring extractDir;
        if (!LoadGraphFromPath(args.graphPath, engine->Model(), extractDir, failure))
            return nullptr;
        engine->m_extractDir.Replace(std::move(extractDir));

        engine->Evaluator().SetOptions(EvaluatorOptionsFor(args));

        // The GUI's per-node GPU span wiring; the timer records nothing
        // unless --gpu-timing enables it.
        engine->m_gpuTimer.Initialize(engine->D3DDevice());
        engine->m_gpuTimer.SetEnabled(args.gpuTiming);
        engine->Evaluator().SetGpuTimer(&engine->m_gpuTimer);
        if (args.gpuTiming && !engine->m_gpuTimer.IsInitialized())
            std::fwprintf(stderr, L"[ShaderLab] --gpu-timing requested but this device has "
                                  L"no timestamp queries; GPU times will be absent\n");

        // The primary monitor's advanced-color caps, read once: headless runs
        // no DispatcherQueue, so there are no change events. Falls back to
        // the struct defaults when no display is reachable (CI, session 0).
        engine->m_displayMonitor.InitializeForPrimaryMonitor();
        ApplyDisplayPin(args, engine->m_displayMonitor);
        engine->m_context->SetDisplayProfile(engine->m_displayMonitor.ActiveProfile());
        return engine;
    }

    HeadlessEngine::~HeadlessEngine()
    {
        // The evaluator, inside the context, outlives the timer it points at.
        if (m_context) Evaluator().SetGpuTimer(nullptr);
    }

    void HeadlessEngine::PrepareGraph(std::optional<double> clockTime)
    {
        // A source that fails records it on its runtimeError.
        Model().MarkAllDirty();
        Rendering::PrepareSources(Model(), Sources(), { m_dc.get(), D3DDevice(), D3DContext() },
                                  Rendering::SourceScope::All);
        if (clockTime.has_value())
            Graph::SetClocksToTime(Model(), *clockTime);
    }

    void HeadlessEngine::SyncFrameInputs()
    {
        // Each field is dead-banded, so a stable profile costs only a walk.
        const auto& profile = m_displayMonitor.ActiveProfile();
        m_context->SetDisplayProfile(profile);
        Rendering::UpdateWorkingSpaceNodes(Model(), profile);
        Rendering::SettleVideoSources(Model(), Sources(), Evaluator(), m_dc.get());
    }

    bool HeadlessEngine::RecoverDevice(HRESULT reason)
    {
        std::fwprintf(stderr, L"[ShaderLab] the device was removed (0x%08X); moving to a new one\n",
                      static_cast<uint32_t>(reason));
        Rendering::DeviceStackOptions options;
        options.choice = m_adapterChoice;
        Rendering::DeviceStack device;
        std::wstring failedStep;
        HRESULT hr = Rendering::CreateDeviceStack(options, device, &failedStep);
        if (SUCCEEDED(hr)) hr = m_context->MoveToDevice(device.d3dDevice.get());
        if (FAILED(hr))
        {
            std::fwprintf(stderr, L"[ShaderLab] no new device (0x%08X)\n", static_cast<uint32_t>(hr));
            return false;
        }
        m_dc.copy_from(m_context->DeviceContext());
        // The timer's queries belonged to the old device.
        const bool timing = m_gpuTimer.IsEnabled();
        Evaluator().SetGpuTimer(nullptr);
        m_gpuTimer.Shutdown();
        m_gpuTimer.Initialize(D3DDevice());
        m_gpuTimer.SetEnabled(timing);
        Evaluator().SetGpuTimer(&m_gpuTimer);
        return true;
    }

    Rendering::FrameResult HeadlessEngine::RunFrameRecovering(const Rendering::FrameRequest& request)
    {
        // RunFrame syncs Working Space nodes and settles videos itself.
        m_context->SetDisplayProfile(m_displayMonitor.ActiveProfile());
        auto result = m_context->RunFrame(request);
        if (FAILED(result.deviceRemoved) && RecoverDevice(result.deviceRemoved))
            result = m_context->RunFrame(request);
        return result;
    }

    void HeadlessEngine::RenderFrame()
    {
        RunFrameRecovering({});
    }

    HRESULT HeadlessEngine::RenderFrameFor(uint32_t nodeId)
    {
        Rendering::FrameRequest request;
        request.demand = Rendering::FrameDemand::Roots;
        request.roots = { nodeId };
        const auto result = RunFrameRecovering(request);
        return FAILED(result.deviceRemoved) ? result.deviceRemoved : result.endDraw;
    }

    void HeadlessEngine::RegisterRoutes(McpRouter& router)
    {
        Mcp::RegisterEngineRoutes(router, *this);
        Support::RegisterPackageRoutes(router, *this);
        Mcp::RegisterVideoExportRoute(router, *this);
    }

    Mcp::Response HeadlessEngine::Dispatch(std::function<Mcp::Response(Mcp::EngineContext&)> closure)
    {
        Mcp::EngineContext ctx(*m_context);
        ctx.displayMonitor = &m_displayMonitor;
        // Every node is demanded, so a full frame covers any node a route reads.
        ctx.renderFrame    = [this](uint32_t) { RenderFrame(); };
        // No swap chain to ask: headless always renders scRGB FP16.
        ctx.getPipelineFormatName = [] { return Rendering::FormatScRgbFP16.name; };
        return closure(ctx);
    }

    void HeadlessEngine::OnGraphMediaDirChanged(const std::wstring& extractDir)
    {
        m_extractDir.Replace(extractDir);
    }
}
