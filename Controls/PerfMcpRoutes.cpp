#include "pch.h"
#include "PerfMcpRoutes.h"

#include "../Engine/Mcp/McpArgs.h"
#include "../Engine/Mcp/McpJsonWriter.h"
#include "../Effects/Performance.h"
#include "../Graph/GraphModel.h"
#include "../Rendering/GpuTimer.h"
#include "../Rendering/HostRenderSettings.h"
#include "../Rendering/PreviewRenderStats.h"

#include <memory>

namespace ShaderLab::Controls
{
    namespace
    {
        namespace WDJ = winrt::Windows::Data::Json;
        using Mcp::JsonWriter;
        using Mcp::Response;

        // An optional body; a missing or unparseable one reads as no arguments.
        WDJ::JsonObject OptionalBody(const std::string& body)
        {
            WDJ::JsonObject args{ nullptr };
            Mcp::ParseObjectBody(body, args);
            return args;
        }

        // ms, or null while there is no figure.
        void MsOrNull(JsonWriter& json, const char* key, double ms, int decimals)
        {
            json.Key(key);
            if (ms < 0.0) json.Null(); else json.Fixed(ms, decimals);
        }

        // GET /perf: the last published frame timings. Reads only published
        // copies and atomics, so it never waits on the render thread. fps is
        // 1/totalMs: per-frame work, not the achieved rate.
        void RegisterTimings(McpRouteTable& routes, std::shared_ptr<PerfRouteHost> host)
        {
            routes.AddAnyThreadRoute(L"GET", L"/perf",
                [host](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    const auto t = host->timings.Render();
                    const auto ui = host->timings.Ui();
                    const auto& timer = host->gpuTimer;
                    JsonWriter json;
                    json.BeginObject()
                        .Key("fps").Fixed(t.totalUs > 0 ? 1000000.0 / t.totalUs : 0.0, 1)
                        .Key("totalMs").Fixed(t.totalUs / 1000.0, 2)
                        .Key("frameStartMs").Fixed(t.frameStartUs / 1000.0, 2)
                        .Key("sourcesPrepMs").Fixed(t.sourcesPrepUs / 1000.0, 2)
                        .Key("evaluateMs").Fixed(t.evaluateUs / 1000.0, 2)
                        .Key("deferredComputeMs").Fixed(t.deferredComputeUs / 1000.0, 2)
                        .Key("drawMs").Fixed(t.drawUs / 1000.0, 2)
                        .Key("endDrawFlushMs").Fixed(t.endDrawFlushUs / 1000.0, 2)
                        .Key("uiTickMs").Fixed(ui.uiTickUs / 1000.0, 2)
                        .Key("outputWindowsMs").Fixed(ui.outputWindowsUs / 1000.0, 2)
                        // Not measured; kept so clients that read it still parse.
                        .Key("traceMs").Fixed(0.0, 2)
                        .Key("computeDispatches").Int(t.computeDispatches)
                        .Key("framesSampled").Int(t.framesSampled)
                        .Key("endDrawFailed").Int(t.endDrawFailed)
                        .Key("cachedEffects").Int(t.cachedEffects)
                        .Key("cacheInvalidations").Int(t.cacheInvalidations)
                        // Settings and GPU counters read live: a frame
                        // snapshot lags a toggle on an idle graph, which looks
                        // like the toggle failing.
                        .Key("unthrottled").Bool(Rendering::HostRenderSettings::IsUnthrottledRenderEnabled())
                        .Key("gpuAvailable").Bool(t.gpuAvailable)
                        .Key("gpuEnabled").Bool(timer.IsEnabled())
                        .Key("gpuFramesOpened").Int(timer.FramesOpened())
                        .Key("gpuFramesResolved").Int(timer.FramesResolved())
                        .Key("gpuDisjointDrops").Int(timer.DisjointDrops())
                        // GPU execution time; the CPU phases above cannot see shader cost.
                        .Key("gpuFrameMs").Fixed(t.gpuFrameMs, 3)
                        .Key("gpuSourcesPrepMs").Fixed(t.gpuSourcesPrepMs, 3)
                        .Key("gpuEvaluateMs").Fixed(t.gpuEvaluateMs, 3)
                        .Key("gpuDeferredComputeMs").Fixed(t.gpuDeferredComputeMs, 3)
                        .Key("gpuDrawMs").Fixed(t.gpuDrawMs, 3);
                    // null while the previewed node has not re-rendered recently.
                    MsOrNull(json, "previewRenderFps", host->previewStats.Fps(), 2);
                    MsOrNull(json, "previewRenderMs", host->previewStats.Ms(), 3);
                    MsOrNull(json, "previewRenderCpuMs", host->previewStats.CpuMs(), 3);
                    MsOrNull(json, "previewRenderGpuMs", host->previewStats.GpuMs(), 3);
                    return Mcp::JsonResponse(200, json.EndObject().Take());
                });
        }

        // POST /perf/render-mode { unthrottled?, forceRedraw?, analysisRefresh? }.
        // Each key changes only its own setting. Unthrottled drops the
        // worker's pacing wait, evaluates every tick and presents with no
        // vsync: right for measuring throughput, wrong for working. Answers
        // the settings read back, so a change that did not take shows.
        void RegisterRenderMode(McpRouteTable& routes, std::shared_ptr<PerfRouteHost> host)
        {
            routes.AddAnyThreadRoute(L"POST", L"/perf/render-mode",
                [host](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    const auto args = OptionalBody(body);
                    std::string argError;
                    const auto unthrottled = Mcp::ReadBoolArg(args, L"unthrottled", argError);
                    const auto forceRedraw = Mcp::ReadBoolArg(args, L"forceRedraw", argError);
                    const auto analysisRefresh = Mcp::ReadIntArg(args, L"analysisRefresh", argError);
                    if (!argError.empty())
                        return Mcp::ErrorResponse(400, argError);

                    // The analysis-readout preset index is 0..3
                    // (HostRenderSettings::SetAnalysisReadoutPreset).
                    if (analysisRefresh && !Rendering::HostRenderSettings::SetAnalysisReadoutPreset(*analysisRefresh))
                        return Mcp::ErrorResponse(400, "analysisRefresh must be 0..3");
                    if (unthrottled)
                    {
                        Rendering::HostRenderSettings::SetUnthrottledRenderEnabled(*unthrottled);
                        host->showSetting(PerfSetting::Unthrottled, *unthrottled);
                    }
                    if (forceRedraw)
                    {
                        host->forceContinuousRedraw.store(*forceRedraw, std::memory_order_release);
                        host->showSetting(PerfSetting::ForceRedraw, *forceRedraw);
                    }
                    if (analysisRefresh)
                        host->showSetting(PerfSetting::AnalysisRefresh, *analysisRefresh);
                    // Apply a new mode on this tick, not after the current frame wait.
                    host->renderDispatcher.Wake();

                    return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                        .Key("unthrottled").Bool(Rendering::HostRenderSettings::IsUnthrottledRenderEnabled())
                        .Key("forceRedraw").Bool(host->forceContinuousRedraw.load(std::memory_order_acquire))
                        .Key("analysisRefresh").Int(Rendering::HostRenderSettings::AnalysisReadoutPreset())
                        .EndObject().Take());
                });
        }

        // POST /perf/gpu-bindings { enabled? }: the "Keep analysis on the GPU"
        // switch; an empty body only reports it.
        void RegisterGpuBindings(McpRouteTable& routes, std::shared_ptr<PerfRouteHost> host)
        {
            routes.AddAnyThreadRoute(L"POST", L"/perf/gpu-bindings",
                [host](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    const auto args = OptionalBody(body);
                    std::string argError;
                    const auto enabled = Mcp::ReadBoolArg(args, L"enabled", argError);
                    if (!argError.empty())
                        return Mcp::ErrorResponse(400, argError);
                    if (enabled)
                    {
                        if (Performance::IsGpuBindingsEnabled() != *enabled)
                            host->applyGpuBindings(*enabled);
                        host->showSetting(PerfSetting::GpuBindings, *enabled);
                    }
                    return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                        .Key("enabled").Bool(Performance::IsGpuBindingsEnabled()).EndObject().Take());
                });
        }

        // POST /perf/compute-submit { commandList? }: compute dispatches as one
        // command list on the runner's deferred context (default), or call by
        // call on the shared immediate context, for measuring the difference.
        void RegisterComputeSubmit(McpRouteTable& routes)
        {
            routes.AddAnyThreadRoute(L"POST", L"/perf/compute-submit",
                [](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    const auto args = OptionalBody(body);
                    std::string argError;
                    const bool enable = Mcp::ReadBoolArg(args, L"commandList", argError).value_or(true);
                    if (!argError.empty())
                        return Mcp::ErrorResponse(400, argError);
                    Performance::SetComputeCommandListEnabled(enable);
                    return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                        .Key("commandList").Bool(Performance::IsComputeCommandListEnabled()).EndObject().Take());
                });
        }

        // POST /perf/subrect-demand { enabled? }: custom pixel shaders answer a
        // tile query with that tile (on, default) or with the whole image.
        // The next frame pushes the option into existing effects; the cache is
        // not released (tearing it down live crashes inside d2d1.dll).
        void RegisterSubRectDemand(McpRouteTable& routes, std::shared_ptr<PerfRouteHost> host)
        {
            routes.AddRenderRoute(L"POST", L"/perf/subrect-demand",
                [host](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    const auto args = OptionalBody(body);
                    std::string argError;
                    const bool enable = Mcp::ReadBoolArg(args, L"enabled", argError).value_or(true);
                    if (!argError.empty())
                        return Mcp::ErrorResponse(400, argError);
                    Performance::SetSubRectInputDemandEnabled(enable);
                    host->graph.MarkAllDirty();
                    host->forceRender = true;
                    return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                        .Key("subRectInputDemand").Bool(Performance::IsSubRectInputDemandEnabled()).EndObject().Take());
                });
        }

        // POST /perf/gpu-timing { enabled? }: GPU timestamp sampling, off by
        // default because closing a span around D2D work needs a Flush, which
        // perturbs the frame measured. Runs on the render thread, which owns
        // the timer's device state.
        void RegisterGpuTiming(McpRouteTable& routes, std::shared_ptr<PerfRouteHost> host)
        {
            routes.AddRenderRoute(L"POST", L"/perf/gpu-timing",
                [host](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    const auto args = OptionalBody(body);
                    std::string argError;
                    const bool enable = Mcp::ReadBoolArg(args, L"enabled", argError).value_or(true);
                    if (!argError.empty())
                        return Mcp::ErrorResponse(400, argError);
                    auto& timer = host->gpuTimer;
                    if (!timer.IsInitialized())
                        return Mcp::JsonResponse(200, R"({"gpuAvailable":false,"gpuEnabled":false,)"
                            R"("note":"device does not support timestamp queries"})");
                    timer.SetEnabled(enable);
                    return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                        .Key("gpuAvailable").Bool(true)
                        .Key("gpuEnabled").Bool(timer.IsEnabled())
                        .Key("latencyFrames").Int(Rendering::GpuTimer::kLatency)
                        .EndObject().Take());
                });
        }
    }

    void RegisterPerfRoutes(McpRouteTable& routes, PerfRouteHost host)
    {
        auto shared = std::make_shared<PerfRouteHost>(std::move(host));
        RegisterTimings(routes, shared);
        RegisterRenderMode(routes, shared);
        RegisterGpuBindings(routes, shared);
        RegisterComputeSubmit(routes);
        RegisterSubRectDemand(routes, shared);
        RegisterGpuTiming(routes, shared);
    }
}
