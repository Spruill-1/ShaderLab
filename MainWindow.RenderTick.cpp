// MainWindow partial: the render loop -- the UI tick (OnRenderTick), the
// render worker (RenderWorkerLoop) and its frame, and output-window present.

#include "pch.h"
#include "MainWindow.xaml.h"

#include "Rendering/FramePacer.h"
#include "Rendering/FrameExecutor.h"
#include "Rendering/SourceRuntime.h"
#include "Graph/ClockModel.h"
#include "Graph/GraphEdits.h"
#include "Rendering/PipelineFormat.h"
#include "Effects/Performance.h"
#include "Rendering/HostRenderSettings.h"
#include "Controls/OutputSinkRenderer.h"

namespace
{
    using OutputSinkList = std::vector<std::shared_ptr<::ShaderLab::Controls::OutputSinkRenderState>>;

    // Node ids of the output windows still open. Read from the sinks: the
    // worker never touches the UI-owned OutputWindow list.
    std::vector<uint32_t> LiveSinkNodeIds(std::mutex& listMutex, const OutputSinkList& sinks)
    {
        OutputSinkList copy;
        {
            std::scoped_lock lock(listMutex);
            copy = sinks;
        }
        std::vector<uint32_t> nodeIds;
        for (const auto& sink : copy)
        {
            if (!sink) continue;
            std::scoped_lock viewLock(sink->viewMutex);
            if (!sink->closed) nodeIds.push_back(sink->nodeId);
        }
        return nodeIds;
    }
}

namespace winrt::ShaderLab::implementation
{
    // -----------------------------------------------------------------------
    // Render loop
    //
    // The render loop is split across two threads:
    //   - UI thread (m_renderTimer DispatcherQueueTimer): runs OnRenderTick.
    //     Handles XAML-touching work only -- editor-canvas redraw on the
    //     UI-side D2D context, FPS panel text update, video seek slider,
    //     properties panel refresh, MCP indicator, log windows.
    //   - Render-worker thread (m_renderWorker): runs RenderWorkerLoop, whose
    //     per-tick body handles all graph + GPU work -- working space sync,
    //     capture/clock/video upload, dirty propagation, RenderFrameToOffscreen
    //     (evaluates the graph and draws into the double-buffered offscreen),
    //     and snapshot publication.
    //
    // The two threads communicate through:
    //   - m_renderDispatcher: closures from UI/MCP land on the render thread
    //   - m_uiGraphSnapshot: render thread publishes per-frame; UI reads
    //
    // This split exists so that a slow GPU evaluation (heavy graph, expensive
    // synchronous compute readbacks) cannot block UI input handling. The
    // user-visible win is that buttons / flyouts / canvas pan-zoom stay
    // responsive even when the render side is at ~2 fps on a heavy graph.
    // -----------------------------------------------------------------------

    void MainWindow::OnRenderTick(
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& /*sender*/,
        winrt::Windows::Foundation::IInspectable const& /*args*/)
    {
        if (m_isShuttingDown) return;
        if (!m_renderEngine.IsInitialized()) return;

        try
        {
        m_compileProgress.Update(CurrentGraphSnapshot(), Content().XamlRoot());
        // Compute frame delta time (used by the render-tick body for clock
        // node advancement and frame timing).
        auto now = std::chrono::steady_clock::now();
        double deltaSec = std::chrono::duration<double>(now - m_lastRenderTick).count();
        m_lastRenderTick = now;
        if (deltaSec > 0.1) deltaSec = 0.016;

        auto tTickStart = std::chrono::high_resolution_clock::now();

        // Cache the preview panel's DIP size (UI-thread-only XAML read) so the
        // render worker can fit the preview after eval without touching XAML.
        if (auto panel = PreviewPanel())
        {
            Preview().SetViewport(static_cast<float>(panel.ActualWidth()), static_cast<float>(panel.ActualHeight()));
            m_previewPixelScale.store((std::max)(1e-3f, static_cast<float>(panel.CompositionScaleX())),
                                      std::memory_order_relaxed);
        }

        // The dispatcher is drained by the worker only: a closure run here
        // would race the graph and hold up input.

        // Blit the most recently published offscreen frame into the
        // SwapChainPanel-bound swap chain and Present.
        BlitOffscreenToSwapChain();

        // Editor canvas redraw (UI-side D2D context).
        // Trigger a redraw whenever the worker has published a new snapshot
        // since our last UI tick -- the canvas reads runtime fields like
        // clockTime / analysisOutput from the live graph, but doesn't
        // self-invalidate when those change. Without this, the canvas only
        // redraws on UI-side interaction, so a playing Clock or Video looks
        // frozen even though the worker is ticking and republishing.
        const uint64_t curGen = m_frameGeneration.load(std::memory_order_acquire);
        if (curGen != m_lastSeenFrameGeneration)
        {
            m_nodeGraphController.SetNeedsRedraw();
            m_lastSeenFrameGeneration = curGen;
        }
        RenderNodeGraph();

        // Output windows blit what the worker drew into their sinks
        // (RenderOutputSinks) and present.
        PresentOutputWindows();
        auto tNodeGraphEnd = std::chrono::high_resolution_clock::now();

        // Accumulate UI-tick timing.
        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_uiTiming;
            t.uiTickUs      = t.uiTickUs * (1-a) + usec(tTickStart, tNodeGraphEnd) * a;
            m_frameTimings.Publish(t);
        }

        // Periodic UI updates at 250 ms (log windows, properties panel,
        // MCP activity indicator, FPS tooltip) and 1 s (FPS counter).
        auto fpsNow = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(fpsNow - m_fpsTimePoint).count();
        if (elapsed >= 250)
        {
            if (!m_logWindows.empty())
                UpdateLogWindows();
            // Bound values change without an edit: while the graph has
            // pending changes, refresh a selected node that has bindings.
            if (SelectedNodeId() != 0)
                if (const auto snapshot = CurrentGraphSnapshot(); snapshot && snapshot->hasDirtyNodes)
                    m_propertiesPanel.RefreshBoundValues();
            UpdateMcpActivityIndicator();
            UpdateFpsTooltip();
        }
        if (elapsed >= 1000)
        {
            uint64_t framesSeen = m_frameCount.exchange(0, std::memory_order_relaxed);
            float fps = static_cast<float>(framesSeen) * 1000.0f / static_cast<float>(elapsed);

            const auto snapshot = CurrentGraphSnapshot();
            const uint64_t currentVideoUploads = snapshot ? snapshot->totalVideoUploads : m_lastVideoUploadCount;
            float videoFps = static_cast<float>(currentVideoUploads - m_lastVideoUploadCount) * 1000.0f / static_cast<float>(elapsed);
            m_lastVideoUploadCount = currentVideoUploads;
            m_lastVideoFps = videoFps;
            m_lastFps = fps;

            FpsText().Text(winrt::hstring(BuildFpsStatusText()));
            UpdateFpsTooltip();
            m_fpsTimePoint = fpsNow;
        }

        } // end try
        catch (const winrt::hresult_error& ex)
        {
            OutputDebugStringW(std::format(L"[OnRenderTick] Exception: 0x{:08X}\n",
                static_cast<uint32_t>(ex.code())).c_str());
        }
        catch (const std::exception& ex)
        {
            OutputDebugStringW(std::format(L"[OnRenderTick] std::exception: {}\n",
                std::wstring(ex.what(), ex.what() + strlen(ex.what()))).c_str());
        }
        catch (...)
        {
            OutputDebugStringW(L"[OnRenderTick] Unknown exception\n");
        }
    }

    // ---------------------------------------------------------------------
    // RenderWorkerLoop -- render thread entry point.
    //
    // Runs the offscreen render path: each iteration evaluates the graph
    // and draws the preview image into a double-buffered offscreen target,
    // then publishes the buffer index for UI thread to blit.
    // ---------------------------------------------------------------------
    void MainWindow::RenderWorkerLoop(std::stop_token stop)
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        m_renderDispatcher.RegisterConsumer();
        // The source runtime is this thread's while it runs.
        m_sourceFactory.SetOwnerThread(std::this_thread::get_id());

        auto last = std::chrono::steady_clock::now();
        ::ShaderLab::Rendering::FramePacer pacer(m_targetRefreshHz.load(std::memory_order_relaxed));
        while (!stop.stop_requested() && !m_renderShouldStop.load(std::memory_order_acquire))
        {
            // Pacing. In both modes a queued MCP closure wakes the worker
            // early, so MCP traffic raises the tick rate, which matters when
            // benchmarking over MCP.
            const bool unthrottled = ::ShaderLab::Rendering::HostRenderSettings::IsUnthrottledRenderEnabled();
            if (unthrottled)
            {
                // Drain without blocking, then hand the timeslice to any
                // ready thread on this processor before the next iteration.
                m_renderDispatcher.WaitFor(std::chrono::milliseconds(0));
                std::this_thread::yield();
            }
            else
            {
                // Wait for the next frame deadline at the display refresh
                // rate (60 Hz minimum), so a static graph does not spin a
                // core and the frame's own work is absorbed in the period.
                // A frame that overran still idles briefly (FramePacer).
                pacer.SetRate(m_targetRefreshHz.load(std::memory_order_relaxed));
                m_renderDispatcher.WaitUntil(pacer.WaitDeadline(std::chrono::steady_clock::now()));
                pacer.BeginFrame(std::chrono::steady_clock::now());
            }
            // Every UI and MCP mutation arrives as a closure drained here.
            m_renderDispatcher.Drain();
            if (stop.stop_requested() || m_renderShouldStop.load(std::memory_order_acquire))
                break;
            if (m_isShuttingDown) break;
            if (!m_renderEngine.IsInitialized()) continue;

            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            if (dt > 0.1) dt = 0.016;

            try
            {
                // Per-tick non-GPU work: working space sync, capture/clock tick, video upload, dirty
                // propagation. Then the offscreen render itself.
                UpdateWorkingSpaceNodes();

                // Use the render-thread D2D context for source uploads. They
                // create D2D bitmaps that the evaluator (also using the
                // render context) will draw -- everything stays on one
                // context to avoid cross-context state races.
                if (auto* dc5 = static_cast<ID2D1DeviceContext5*>(m_renderEngine.RenderD2DContext()))
                {
                    auto nodes = m_graph.MutableNodes();
                    if (m_sourceFactory.TickAndUploadLiveCaptures(nodes, dc5))
                        m_forceRender = true;
                }

                ::ShaderLab::Graph::AdvanceClocks(m_graph, dt);

                m_graphEvaluator.ResolveSourceBindings(m_graph);

                if (auto* dc = m_renderEngine.RenderD2DContext())
                {
                    // Per-video failures land on their nodes; anything else
                    // reaches the tick's own handler.
                    m_sourceFactory.TickAndUploadVideos(m_graph.MutableNodes(), dc);
                }

                // Before the dirty check, so a change counts for the nodes it reaches.
                m_graph.PropagateDirtyDownstream();

                bool wasForceRender = m_forceRender;
                bool hasDirty = m_graph.HasDirtyNodes();
                // Open output windows need a frame every tick even when
                // nothing is dirty (a static source feeding an Output node).
                const bool hasOutputWindows = !LiveSinkNodeIds(m_outputSinksMutex, m_outputSinks).empty();
                // GPU timing implies "keep rendering", as does the flyout's
                // Force-continuous-redraw box. With a static graph nothing is
                // dirty, so the worker would idle, the timer's 4-deep ring
                // would never retire, and every span would read 0.00 ms --
                // indistinguishable from a broken timer.
                bool gpuTiming = m_renderEngine.Timer().IsEnabled();
                bool forceRedraw = m_forceContinuousRedraw.load(std::memory_order_acquire);
                // `unthrottled` implies evaluate-every-tick. Without that
                // the loop would free-run over a clean graph and do nothing
                // at all -- burning a core to measure zero. The point of the
                // mode is to answer "how fast can this pipeline run", which
                // requires actually running it.
                // An async analysis readback still in flight needs frames to
                // land: its values arrive on a later Evaluate, which then
                // re-dirties whatever is bound to them.
                bool pendingReadback = m_graphEvaluator.HasPendingReadbacks();
                bool needsEval = hasDirty || Preview().FitPending() || m_forceRender
                              || hasOutputWindows || gpuTiming || forceRedraw
                              || unthrottled || pendingReadback;
                // Force redraw means the WORST case, every frame: every node
                // regenerates as if all of its inputs had just changed. Only
                // evaluating is not that -- a clean graph evaluates to cached
                // outputs (D2D output caching serves the effects, clean compute
                // nodes skip their dispatch, the gamut LUT is reused), which
                // measured 0.24 ms / 3671 fps on the bird graph: the cost of
                // doing nothing. Dirtying every node forces all of it to run:
                // D2D output caches are dropped, every compute node dispatches,
                // generators rebuild.
                //
                // Deliberately AFTER `hasDirty` is read, so forced dirtiness
                // does not bump m_graphGeneration -- that counter means "the
                // graph was edited", and the UI rebuilds on it. Image sources
                // stay decoded: a dirty image re-evaluates downstream but only
                // re-reads the file when its path changes (SourceNodeFactory),
                // so this measures the pipeline, not disk and WIC.
                if (needsEval && forceRedraw)
                    m_graph.MarkAllDirty();
                if (needsEval)
                {
                    // Taken before the frame, so an edit made while it renders
                    // wins over the fit computed from it.
                    const auto previewFrame = Preview().Frame();
                    RenderFrameToOffscreen();
                    m_forceRender = false;
                    m_frameCount.fetch_add(1, std::memory_order_relaxed);
                    if (hasDirty || wasForceRender)
                        ++m_graphGeneration;
                    // Fit after the frame, from the bounds it measured. Auto-fit
                    // re-fits every evaluated frame, so a further frame is
                    // forced only when the fit moved the view; otherwise a
                    // static graph would keep rendering.
                    if (previewFrame.fitWanted && Preview().FitTo(previewFrame, GetPreviewImageBounds()))
                        m_forceRender = true;
                }

                // Every iteration, rendered or not, so the rate ages to N/A.
                m_previewRenderStats.Publish(std::chrono::steady_clock::now());

                // Publish snapshot.
                m_frameGeneration.fetch_add(1, std::memory_order_release);
                PublishGraphSnapshot();
            }
            catch (const winrt::hresult_error& ex)
            {
                OutputDebugStringW(std::format(L"[RenderWorker] hresult: 0x{:08X}\n",
                    static_cast<uint32_t>(ex.code())).c_str());
            }
            catch (...)
            {
                OutputDebugStringW(L"[RenderWorker] tick exception\n");
            }
        }

        m_renderDispatcher.Drain();
        m_sourceFactory.SetOwnerThread({});
    }

    void MainWindow::PublishGraphSnapshot()
    {
        auto snapshot = ::ShaderLab::Graph::BuildGraphUiSnapshot(
            m_graph, Preview().NodeId(), m_graphGeneration,
            m_frameGeneration.load(std::memory_order_acquire),
            m_sourceFactory.VideoSourceStatuses());
        std::atomic_store(&m_uiGraphSnapshot,
            std::shared_ptr<const ::ShaderLab::Graph::GraphUiSnapshot>(std::move(snapshot)));
    }

    // -------------------------------------------------------------------------
    // RenderFrameToOffscreen / BlitOffscreenToSwapChain
    //
    // The render thread renders the preview image into a double-buffered
    // offscreen D2D bitmap (no swap-chain Present); the UI thread blits the
    // most recently published buffer into the SwapChainPanel-bound swap
    // chain and presents it.
    //
    // The two-buffer publish protocol uses m_offscreenPublishedIdx (atomic
    // int32 with -1 = nothing published yet) and m_offscreenPublishedVersion
    // (atomic uint64 monotonic). Render thread writes index N (where N is
    // the buffer it just rendered to), then UI thread reads that index and
    // blits. Render thread then writes the OTHER index next time.
    // -------------------------------------------------------------------------

    bool MainWindow::EnsureOffscreenUiWrappers()
    {
        // UI thread only: rebuild m_offscreenSourceBitmapUi[0,1] when the
        // render engine's offscreen size changes (or when context is
        // recreated, e.g. adapter switch).
        EnsureUiD2dContext();
        if (!m_uiD2dContext) return false;

        uint32_t w = m_renderEngine.OffscreenWidth();
        uint32_t h = m_renderEngine.OffscreenHeight();
        if (w == 0 || h == 0) return false;

        if (w == m_offscreenWrapperWidth && h == m_offscreenWrapperHeight &&
            m_offscreenSourceBitmapUi[0] && m_offscreenSourceBitmapUi[1])
        {
            return true;
        }

        for (uint32_t i = 0; i < 2; ++i)
        {
            m_offscreenSourceBitmapUi[i] = nullptr;
            auto* tex = m_renderEngine.OffscreenTexture(i);
            if (!tex) return false;
            winrt::com_ptr<IDXGISurface> surface;
            if (FAILED(tex->QueryInterface(IID_PPV_ARGS(surface.put()))))
                return false;

            // Source-side wrapper: no TARGET option, no CANNOT_DRAW (UI uses
            // it as DrawImage source). Format must match what RenderEngine
            // created the textures with (scRGB FP16 by default).
            const auto& fmt = m_renderEngine.ActiveFormat();
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(fmt.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            if (FAILED(m_uiD2dContext->CreateBitmapFromDxgiSurface(
                    surface.get(), bp, m_offscreenSourceBitmapUi[i].put())))
                return false;
        }
        m_offscreenWrapperWidth = w;
        m_offscreenWrapperHeight = h;
        return true;
    }

    void MainWindow::RenderFrameToOffscreen()
    {
        // Render worker only: it writes m_graph and uses the render context.
        if (m_isShuttingDown) return;
        if (!m_renderEngine.IsInitialized()) return;

        // The offscreen target matches the swap chain's back buffer.
        uint32_t w = m_renderEngine.BackBufferWidth();
        uint32_t h = m_renderEngine.BackBufferHeight();
        if (w == 0 || h == 0) return;

        if (!m_renderEngine.EnsureOffscreenTargets(w, h))
            return;

        auto tFrameStart = std::chrono::steady_clock::now();

        // The preview's node, view and selection, read once for the frame.
        const auto preview = Preview().Frame();

        // GPU spans. No-ops unless GPU timing is switched on, which it is not
        // by default: closing a span around D2D work needs a Flush, and that
        // breaks D2D's batching and perturbs the frame being measured.
        auto& gpu = m_renderEngine.Timer();

        // Pick the buffer to write to. We use the OPPOSITE of whatever was
        // just published, so UI thread can keep reading the other one
        // concurrently without contention.
        int32_t lastPub = m_offscreenPublishedIdx.load(std::memory_order_acquire);
        int32_t writeIdx = (lastPub == 0) ? 1 : 0;

        // Use the render-thread-dedicated D2D context (not the default one
        // -- that one is shared with capture / pixel-inspector paths that
        // run on UI thread, and concurrent BeginDraw on it would put it
        // into a wrong-state error mid-tick).
        auto* dc = m_renderEngine.RenderD2DContext();
        if (!dc) return;
        auto* targetBitmap = m_renderEngine.OffscreenRenderBitmap(writeIdx);
        if (!targetBitmap) return;

        // Open the GPU frame only AFTER every early return above. A BeginFrame
        // with no matching EndFrame leaves the slot's disjoint query begun and
        // never ended, and m_writeIndex never advances -- so the ring wedges on
        // that slot and nothing ever retires again. The symptom is
        // framesResolved stuck at 0 with no error anywhere, which reads as
        // "the GPU did no work" rather than "the timer is jammed".
        gpu.BeginFrame();
        gpu.Begin(::ShaderLab::Rendering::GpuSpan::Frame);

        // Whether the previewed node renders in this frame: its output-change
        // count moves in any of the frame's evaluation passes.
        m_previewRenderStats.Initialize(m_renderEngine.D3DDevice());
        m_previewRenderStats.SetNode(preview.nodeId);
        const uint64_t previewChangesBefore = m_graphEvaluator.OutputChangeCount(preview.nodeId);
        m_previewRenderStats.BeginFrame(std::chrono::steady_clock::now());
        gpu.Begin(::ShaderLab::Rendering::GpuSpan::SourcesPrep);

        // The timer flushes above wait here when the GPU is still busy with
        // earlier frames; that wait is reported as frameStartUs, not as
        // source preparation.
        auto tSourcesStart = std::chrono::steady_clock::now();

        ::ShaderLab::Rendering::PrepareSources(m_graph, m_sourceFactory,
            { dc, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext() }, ::ShaderLab::Rendering::SourceScope::Changed);

        auto tSourcesEnd = std::chrono::steady_clock::now();
        gpu.End(::ShaderLab::Rendering::GpuSpan::SourcesPrep);

        ::ShaderLab::Rendering::FrameRequest request;
        request.target = targetBitmap;
        request.timer = &gpu;

        // Demand: what is on screen or about to be read, besides the Output
        // nodes and pending analysis the graph demands by itself.
        request.demand = ::ShaderLab::Rendering::FrameDemand::Roots;
        if (preview.nodeId != 0)
            request.roots.push_back(preview.nodeId);
        if (m_readbackNodeId != 0)
            request.roots.push_back(m_readbackNodeId);
        for (uint32_t sinkNodeId : LiveSinkNodeIds(m_outputSinksMutex, m_outputSinks))
            request.roots.push_back(sinkNodeId);

        // CPU-analysis interest: the selected node and what it is bound to,
        // or every compute node when "Refresh analysis readouts" says so, so
        // canvas labels refresh at the throttle interval.
        {
            std::unordered_set<uint32_t> interest;
            if (::ShaderLab::Rendering::HostRenderSettings::IsCpuAnalysisHintAllNodesEnabled())
            {
                for (const auto& n : m_graph.Nodes())
                    if (n.type == ::ShaderLab::Graph::NodeType::ComputeShader)
                        interest.insert(n.id);
            }
            if (preview.selectedNodeId != 0)
            {
                interest.insert(preview.selectedNodeId);
                if (auto* sel = m_graph.FindNode(preview.selectedNodeId))
                {
                    ::ShaderLab::Graph::ForEachBindingSource(*sel,
                        [&](uint32_t sourceNodeId) { interest.insert(sourceNodeId); });
                }
            }
            request.analysisInterest = std::move(interest);
        }

        request.draw = [&](ID2D1DeviceContext5* ctx)
        {
            // Set DPI to 96 to match WinUI DIPs -- but only when the context
            // isn't already there: a real per-frame DPI flip invalidates every
            // D2D1_PROPERTY_CACHED effect intermediate in the context. The
            // render context is pinned at 96 (RenderEngine), so this is
            // normally a no-op kept as a safety net.
            float oldDpiX, oldDpiY;
            ctx->GetDpi(&oldDpiX, &oldDpiY);
            const bool dpiFlip = (oldDpiX != 96.0f || oldDpiY != 96.0f);
            if (dpiFlip)
                ctx->SetDpi(96.0f, 96.0f);

            ctx->Clear(D2D1::ColorF(D2D1::ColorF::Black));

            // Pan / zoom are in DIPs (they follow pointer positions); the back
            // buffer is in physical pixels, shown 1:1 via the swap chain's
            // inverse composition-scale matrix. The final Scale maps DIPs onto
            // those pixels.
            const float pxPerDip = m_previewPixelScale.load(std::memory_order_relaxed);
            ctx->SetTransform(
                D2D1::Matrix3x2F::Scale(preview.view.zoom, preview.view.zoom) *
                D2D1::Matrix3x2F::Translation(preview.view.panX, preview.view.panY) *
                D2D1::Matrix3x2F::Scale(pxPerDip, pxPerDip));

            auto* previewImage = ResolveDisplayImage(preview.nodeId);
            if (previewImage)
                ctx->DrawImage(previewImage);

            // Publish the preview image's bounds for the fit, pointer mapping,
            // Pixel Trace and MCP. Measured here, on the render context that
            // owns the image: an effect image is bound to the context that
            // built it, and asking the UI context returns an empty rect.
            {
                D2D1_RECT_F pb{};
                if (!previewImage || FAILED(ctx->GetImageLocalBounds(previewImage, &pb)))
                    pb = D2D1_RECT_F{};
                std::scoped_lock lock(m_previewBoundsMutex);
                m_previewBounds = pb;
            }

            ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            if (dpiFlip)
                ctx->SetDpi(oldDpiX, oldDpiY);
        };

        const auto frame = ::ShaderLab::Rendering::FrameExecutor::Run(m_graph, m_graphEvaluator, dc, request);
        if (frame.computeRan)
            m_nodeGraphController.SetNeedsRedraw();
        const HRESULT hrEnd = frame.endDraw;
        const uint32_t computeCount = frame.dispatches;
        const auto tEvalEnd = frame.evaluateEnd;
        const auto tComputeEnd = frame.computeEnd;
        const auto tDrawEnd = frame.drawEnd;

        gpu.End(::ShaderLab::Rendering::GpuSpan::Frame);
        gpu.EndFrame();
        m_previewRenderStats.EndFrame(
            preview.nodeId != 0 && m_graphEvaluator.OutputChangeCount(preview.nodeId) != previewChangesBefore,
            std::chrono::steady_clock::now());

        // Publish per-node GPU results onto the nodes, where the canvas picks
        // them up through the ordinary GraphUiSnapshot copy.
        //
        // Two kinds of number, and the difference is worth keeping straight:
        //
        //  * A COMPUTE node reports its own dispatch. That is exact -- each
        //    bridge dispatch is its own D3D11 submission -- and free.
        //
        //  * A D2D IMAGE node reports nothing, EXCEPT the one at the end of
        //    the chain, which carries the whole fused chain's draw cost.
        //    Direct2D evaluates a chain lazily at DrawImage, so the
        //    intermediate effects are never separately dispatched and cannot
        //    be attributed without materialising each one -- which would
        //    change the very workload being measured. Attributing the Draw
        //    span to the drawn node instead is honest and costs nothing: it
        //    says "everything feeding this node cost X together", which is
        //    the true shape of the work.
        {
            using ::ShaderLab::Graph::GpuNodeState;
            const auto& nodeMs = gpu.NodeResults();
            const double chainMs = gpu.SpanMs(::ShaderLab::Rendering::GpuSpan::Draw);
            const bool timing = gpu.IsEnabled();

            for (auto& n : m_graph.MutableNodes())
            {
                // Same rule the evaluator uses to route a node to the D3D11
                // bridge. Kept in sync deliberately: a node the evaluator
                // dispatches is a node the timer can bracket, and one it
                // doesn't is a node whose cost lives inside a D2D chain.
                const bool isBridgeCompute =
                    (n.type == ::ShaderLab::Graph::NodeType::PixelShader ||
                     n.type == ::ShaderLab::Graph::NodeType::ComputeShader) &&
                    n.customEffect.has_value() &&
                    n.customEffect->shaderType ==
                        ::ShaderLab::Graph::CustomShaderType::D3D11ComputeShader;

                auto it = nodeMs.find(n.id);
                double ms = (it == nodeMs.end()) ? -1.0 : it->second;
                GpuNodeState state = GpuNodeState::Unmeasured;

                if (!timing)
                {
                    ms = -1.0;
                }
                else if (it != nodeMs.end())
                {
                    // Its own bracketed dispatch.
                    state = GpuNodeState::Measured;
                }
                else if (n.id == preview.nodeId && chainMs > 0.0)
                {
                    // The drawn node is the chain end, so it carries the cost
                    // of everything D2D fused into the draw that produced it.
                    ms = chainMs;
                    state = GpuNodeState::Measured;
                }
                else if (!n.needed)
                {
                    // The evaluator skipped it: nothing downstream consumes
                    // its output. Worth saying out loud -- an analysis node
                    // with no consumer reads as "broken measurement" when it
                    // is really "not wired to anything".
                    state = GpuNodeState::Idle;
                }
                else if (isBridgeCompute)
                {
                    // Needed, but the evaluator did not dispatch it -- clean,
                    // so it served its cached result. Zero, not unknown.
                    ms = 0.0;
                    state = GpuNodeState::Cached;
                }
                else if (n.outputPins.empty())
                {
                    // No image output and not a compute dispatch: a parameter
                    // node. It cannot be fused into a D2D chain because it
                    // contributes nothing to one.
                    ms = 0.0;
                    state = GpuNodeState::CpuOnly;
                }
                else
                {
                    // A D2D image node upstream of a chain end. Its cost is
                    // inside that end's figure and cannot be split out without
                    // materialising it separately.
                    state = GpuNodeState::Fused;
                }

                ::ShaderLab::Graph::RecordNodeGpuTime(n, ms, state);
            }

            // The figures change every frame, so the canvas has to be told
            // it is stale -- otherwise the annotations freeze at whatever was
            // on screen when the last topology change happened, which looks
            // exactly like a broken measurement.
            if (m_nodeGraphController.ShowNodeGpuStats())
                m_nodeGraphController.SetNeedsRedraw();
        }

        auto tEndDraw = std::chrono::steady_clock::now();

        // Always bump framesSampled BEFORE deciding whether to publish, so
        // /perf accurately reflects worker activity even when EndDraw fails
        // (device-lost, transient state, etc.). Without this, a single
        // EndDraw glitch would freeze the displayed FPS at a stale value.
        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_renderTiming;
            t.frameStartUs      = t.frameStartUs      * (1-a) + usec(tFrameStart,  tSourcesStart) * a;
            t.sourcesPrepUs     = t.sourcesPrepUs     * (1-a) + usec(tSourcesStart, tSourcesEnd) * a;
            t.evaluateUs        = t.evaluateUs        * (1-a) + usec(tSourcesEnd,  tEvalEnd)    * a;
            t.deferredComputeUs = t.deferredComputeUs * (1-a) + usec(tEvalEnd,     tComputeEnd) * a;
            t.drawUs            = t.drawUs            * (1-a) + usec(tComputeEnd,  tDrawEnd)    * a;
            t.endDrawFlushUs    = t.endDrawFlushUs    * (1-a) + usec(tDrawEnd,     tEndDraw)    * a;
            t.computeDispatches = computeCount;
            t.cachedEffects      = m_graphEvaluator.CachedEffectCount();
            t.cacheInvalidations = m_graphEvaluator.CacheInvalidations();
            using GS = ::ShaderLab::Rendering::GpuSpan;
            t.gpuAvailable       = gpu.IsInitialized();
            t.gpuEnabled         = gpu.IsEnabled();
            t.gpuFramesResolved  = gpu.FramesResolved();
            t.gpuDisjointDrops   = gpu.DisjointDrops();
            // GPU spans are NOT exponentially averaged like the CPU ones: they
            // already lag by up to kLatency frames, and smoothing a lagged
            // signal makes a step change (the thing a perf A/B is looking for)
            // take ~30 frames to appear and read as drift.
            t.gpuFrameMs         = gpu.SpanMs(GS::Frame);
            t.gpuSourcesPrepMs   = gpu.SpanMs(GS::SourcesPrep);
            t.gpuEvaluateMs      = gpu.SpanMs(GS::Evaluate);
            t.gpuDeferredComputeMs = gpu.SpanMs(GS::DeferredCompute);
            t.gpuDrawMs          = gpu.SpanMs(GS::Draw);
            t.totalUs           = t.totalUs           * (1-a) + usec(tFrameStart,  tEndDraw)    * a;
            t.framesSampled++;
            t.endDrawFailed     = FAILED(hrEnd) ? (t.endDrawFailed + 1) : t.endDrawFailed;
            m_frameTimings.Publish(t);
        }

        if (FAILED(hrEnd))
            return;

        // Publish: store this buffer's index with release semantics so the
        // UI thread sees a fully-rendered frame before reading.
        m_offscreenPublishedIdx.store(writeIdx, std::memory_order_release);
        m_offscreenPublishedVersion.fetch_add(1, std::memory_order_release);

        RenderOutputSinks();
    }

    // Render thread: draws each open output window's node into its sink
    // (Controls/OutputSinkRenderer); the windows blit on the UI thread.
    void MainWindow::RenderOutputSinks()
    {
        std::vector<std::shared_ptr<::ShaderLab::Controls::OutputSinkRenderState>> sinks;
        {
            std::scoped_lock lock(m_outputSinksMutex);
            sinks = m_outputSinks;
        }
        auto* dc = m_renderEngine.RenderD2DContext();
        auto* device = m_renderEngine.D3DDevice();
        for (auto& sink : sinks)
        {
            if (!sink) continue;
            {
                std::scoped_lock lock(sink->viewMutex);
                if (sink->closed) continue;
            }
            const HRESULT hr = ::ShaderLab::Controls::RenderOutputSink(*sink, ResolveDisplayImage(sink->nodeId),
                                                                       dc, device, m_renderEngine.ActiveFormat());
            if (FAILED(hr))
                OutputDebugStringW(std::format(L"[OutputWindow] node {} render failed 0x{:08X}\n",
                    sink->nodeId, static_cast<uint32_t>(hr)).c_str());
        }
    }

    void MainWindow::BlitOffscreenToSwapChain()
    {
        // UI thread only. Blits the latest published offscreen buffer to the
        // SwapChainPanel-bound swap chain via a UI-side D2D context that shares
        // the engine's multi-threaded D2D device.
        if (m_isShuttingDown) return;
        if (!m_renderEngine.IsInitialized()) return;
        if (!EnsureOffscreenUiWrappers()) return;

        int32_t idx = m_offscreenPublishedIdx.load(std::memory_order_acquire);
        if (idx < 0 || idx > 1) return;

        // Skip blit when there's no new published frame since last UI tick.
        // Without this the UI thread vsync-waits in Present1(1, 0) on EVERY
        // 16 ms tick (60 Hz) even when the worker is producing frames at,
        // say, 10 Hz. Each Present1 wait blocks input event delivery, so
        // dropdowns / hover highlights get starved during heavy graph eval.
        // The compositor keeps showing the last presented frame on its own;
        // we only need to re-Present when the worker has actually published
        // a new offscreen.
        uint64_t version = m_offscreenPublishedVersion.load(std::memory_order_acquire);
        if (version == m_lastBlittedVersion) return;
        m_lastBlittedVersion = version;

        auto* sourceBitmap = m_offscreenSourceBitmapUi[idx].get();
        if (!sourceBitmap) return;

        auto* swap = m_renderEngine.SwapChain();
        if (!swap) return;

        winrt::com_ptr<IDXGISurface> backBuffer;
        HRESULT hr = swap->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
        if (FAILED(hr)) return;

        const auto& fmt = m_renderEngine.ActiveFormat();
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(fmt.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        winrt::com_ptr<ID2D1Bitmap1> backBufferBitmap;
        hr = m_uiD2dContext->CreateBitmapFromDxgiSurface(
                backBuffer.get(), bp, backBufferBitmap.put());
        if (FAILED(hr)) return;

        m_uiD2dContext->SetTarget(backBufferBitmap.get());
        // A 1:1 pixel copy: pin the DPI and transform instead of inheriting
        // them. This context is SHARED with the node-graph canvas, which sets
        // it to 96 * CompositionScale on every repaint and leaves it there;
        // inheriting that would scale the finished frame a second time.
        m_uiD2dContext->SetDpi(96.0f, 96.0f);
        m_uiD2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        m_uiD2dContext->BeginDraw();
        m_uiD2dContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        m_uiD2dContext->DrawImage(sourceBitmap);
        hr = m_uiD2dContext->EndDraw();
        m_uiD2dContext->SetTarget(nullptr);
        if (FAILED(hr)) return;

        // Interval 0 in unthrottled mode. Interval 1 blocks this thread until
        // scanout, and with the worker free-running there is a new frame to
        // blit on nearly every UI tick -- so the UI thread would spend most
        // of its time parked in Present1 and input would starve. Tearing is
        // the accepted cost; see HostRenderSettings::IsUnthrottledRenderEnabled.
        const UINT interval =
            ::ShaderLab::Rendering::HostRenderSettings::IsUnthrottledRenderEnabled() ? 0u : 1u;
        DXGI_PRESENT_PARAMETERS params{};
        swap->Present1(interval, 0, &params);
    }

}
