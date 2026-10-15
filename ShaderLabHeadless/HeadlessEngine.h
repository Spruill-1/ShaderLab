#pragma once

// What every headless mode renders with: an EffectGraph context on the
// chosen adapter (it owns the graph, evaluator, sources and D2D context),
// plus the GPU timer and display monitor, which are host concerns. It is
// also the MCP command sink for script and session modes: Dispatch runs the
// closure synchronously on the calling thread, the only thread that touches
// the graph.

#include "HeadlessArgs.h"

#include "../Engine/Api/EffectGraphInternal.h"
#include "../Graph/GraphModel.h"
#include "../Rendering/DeviceStack.h"
#include "../Rendering/GraphEvaluator.h"
#include "../Effects/SourceNodeFactory.h"
#include "../Rendering/DisplayMonitor.h"
#include "../Rendering/GpuTimer.h"
#include "../Engine/Mcp/EngineMcpRoutes.h"

#include <memory>
#include <string>
#include <vector>

namespace ShaderLab
{
    class McpRouter;
}

namespace ShaderLab::Headless
{
    // Owns the temp directory a .effectgraph package's media was extracted
    // to, and deletes it on destruction. Replace() moves to the next loaded
    // graph's directory; one that cannot be deleted yet (a file still open)
    // is retried at destruction.
    class ExtractDirGuard
    {
    public:
        explicit ExtractDirGuard(std::wstring dir) : m_dir(std::move(dir)) {}
        ~ExtractDirGuard();
        ExtractDirGuard(const ExtractDirGuard&) = delete;
        ExtractDirGuard& operator=(const ExtractDirGuard&) = delete;

        void Replace(std::wstring next);

    private:
        std::wstring m_dir;
        std::vector<std::wstring> m_retry;
    };

    class HeadlessEngine final : public Mcp::IEngineCommandSink
    {
    public:
        // Creates the device, registers the engine's D2D effects and loads
        // --graph. Prints the reason and sets `failure` on error.
        static std::unique_ptr<HeadlessEngine> Open(const Args& args, ExitCode& failure);

        ~HeadlessEngine() override;
        HeadlessEngine(const HeadlessEngine&) = delete;
        HeadlessEngine& operator=(const HeadlessEngine&) = delete;

        // Loads every source and, when `clockTime` is set, sets every Clock
        // to it. Call once before the first frame.
        void PrepareGraph(std::optional<double> clockTime);

        // Brings Working Space nodes in line with the display profile and
        // waits for video sources to land on the clock time; every frame
        // does this first.
        void SyncFrameInputs();

        // One frame of the whole graph, so any node can be read next.
        void RenderFrame();
        // One frame demanding only `nodeId`. Returns EndDraw's result.
        HRESULT RenderFrameFor(uint32_t nodeId);

        // The engine routes, plus POST /render/video.
        void RegisterRoutes(McpRouter& router);

        Graph::GraphModel& Model() { return m_context->Model(); }
        Rendering::GraphEvaluator& Evaluator() { return m_context->Evaluator(); }
        Effects::SourceNodeFactory& Sources() { return m_context->Sources(); }
        Rendering::GpuTimer& Timer() { return m_gpuTimer; }
        ID2D1DeviceContext5* Dc() const { return m_dc.get(); }
        ID3D11Device* D3DDevice() const { return m_context->Devices().d3dDevice.get(); }
        ID3D11DeviceContext* D3DContext() const { return m_context->Devices().d3dContext.get(); }

        // IEngineCommandSink
        Mcp::Response Dispatch(std::function<Mcp::Response(Mcp::EngineContext&)> closure) override;
        void OnGraphMediaDirChanged(const std::wstring& extractDir) override;

    private:
        HeadlessEngine() = default;

        // Declaration order is teardown order reversed: the media directory
        // outlives the context, whose sources hold files in it.
        ExtractDirGuard                             m_extractDir{ std::wstring{} };
        winrt::com_ptr<IEffectGraphContextInternal> m_context;
        winrt::com_ptr<ID2D1DeviceContext5>         m_dc;   // the context's own
        Rendering::GpuTimer                         m_gpuTimer;
        Rendering::DisplayMonitor                   m_displayMonitor;
    };
}
