// MainWindow partial (Phase 4 split): the OnRenderTick / RenderWorkerLoop
// render loop, including the dirty-propagation pre-pass, video tick,
// and output-window present. All methods are members of
// `winrt::ShaderLab::implementation::MainWindow`. Extracted from
// MainWindow.xaml.cpp at commit c177770. (The pre-worker RenderTickBody /
// RenderFrame were removed in stdio-migration Step 7 — dead since v1.7.0.)

#include "pch.h"
#include "MainWindow.xaml.h"

#include "Rendering/FramePacer.h"
#include "Rendering/PipelineFormat.h"
#include "Effects/Performance.h"

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
        // render worker can fit a newly-selected node to view after eval
        // without touching XAML. See FitPreviewToView / m_needsFitPreview.
        if (auto panel = PreviewPanel())
        {
            m_previewViewportW = static_cast<float>(panel.ActualWidth());
            m_previewViewportH = static_cast<float>(panel.ActualHeight());
            m_previewPixelScale.store((std::max)(1e-3f, static_cast<float>(panel.CompositionScaleX())),
                                      std::memory_order_relaxed);
        }

        // Drain pending dispatcher closures: NO-OP from the UI side post-P7.
        // The worker thread is the registered consumer and drains its own
        // queue. UI thread reading the queue would race graph mutations and
        // process closures slowly because OnRenderTick also does blit /
        // canvas redraw / event handling. Leaving Drain() here is the
        // primary cause of UI dropdown / hover input lag under load:
        // long-running MCP closures end up running on the UI thread,
        // blocking input event delivery for the duration. Worker calls
        // m_renderDispatcher.Drain() in RenderWorkerLoop -- that's the only
        // path closures should run on.
        // m_renderDispatcher.Drain();   // <-- removed in 7fa7021+

        // Blit the most recently published offscreen frame into the
        // SwapChainPanel-bound swap chain and Present.
        BlitOffscreenToSwapChain();

        // Editor canvas redraw (UI-side D2D context, P4).
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

        // P7: present any open output windows on the UI thread. Render
        // worker has already drawn into each sink's offscreen pair (see
        // MainWindow::RenderOutputSinks called from RenderFrameToOffscreen);
        // here we just sync UI view state + blit + Present1 per window.
        PresentOutputWindows();
        auto tNodeGraphEnd = std::chrono::high_resolution_clock::now();

        // Accumulate UI-tick timing.
        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_frameTiming;
            t.uiTickUs      = t.uiTickUs * (1-a) + usec(tTickStart, tNodeGraphEnd) * a;
        }

        // Update video seek slider and position label while playing.
        if (m_videoSeekSlider && m_videoSeekNodeId != 0)
        {
            auto* vp = m_sourceFactory.GetVideoProvider(m_videoSeekNodeId);
            if (vp && vp->IsOpen())
            {
                double pos = vp->CurrentPosition();
                m_videoSeekSuppressEvents = true;
                m_videoSeekSlider.Value(pos);
                m_videoSeekSuppressEvents = false;
                if (m_videoPositionLabel)
                    m_videoPositionLabel.Text(std::format(L"Position: {:.1f}s / {:.1f}s", pos, vp->Duration()));
            }
        }

        // Periodic UI updates at 250 ms (log windows, properties panel,
        // MCP activity indicator, FPS tooltip) and 1 s (FPS counter).
        auto fpsNow = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(fpsNow - m_fpsTimePoint).count();
        if (elapsed >= 250)
        {
            if (!m_logWindows.empty())
                UpdateLogWindows();
            // Decide whether the selected node's Properties panel needs a
            // refresh WITHOUT holding a live-graph pointer on the UI thread
            // (the decision-#70 race). Read under a shared lock on
            // m_graphMutex, capture a plain bool, release, THEN act -- never
            // hold the lock across UpdatePropertiesPanel, which dispatches to
            // the render worker (m_graphMutex is taken exclusively there;
            // holding it across the dispatch would deadlock). Step 7 residual.
            bool refreshBoundProps = false;
            if (m_selectedNodeId != 0)
            {
                std::shared_lock<std::shared_mutex> graphLock(m_graphMutex);
                if (m_graph.HasDirtyNodes())
                {
                    auto* selNode = m_graph.FindNode(m_selectedNodeId);
                    refreshBoundProps = selNode && !selNode->propertyBindings.empty();
                }
            }
            if (refreshBoundProps && !IsPropertiesPanelInteracting())
                UpdatePropertiesPanel();
            UpdateMcpActivityIndicator();
            UpdateFpsTooltip();
        }
        if (elapsed >= 1000)
        {
            uint64_t framesSeen = m_frameCount.exchange(0, std::memory_order_relaxed);
            float fps = static_cast<float>(framesSeen) * 1000.0f / static_cast<float>(elapsed);

            uint64_t currentVideoUploads = m_sourceFactory.TotalVideoUploads();
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

        auto last = std::chrono::steady_clock::now();
        ::ShaderLab::Rendering::FramePacer pacer(m_targetRefreshHz.load(std::memory_order_relaxed));
        while (!stop.stop_requested() && !m_renderShouldStop.load(std::memory_order_acquire))
        {
            // Pacing. In both modes a queued MCP closure wakes the worker
            // early, so MCP traffic raises the tick rate, which matters when
            // benchmarking over MCP.
            const bool unthrottled = ::ShaderLab::Performance::IsUnthrottledRenderEnabled();
            if (unthrottled)
            {
                // Drain without blocking, then hand the timeslice over before
                // re-entering the loop.
                //
                // The yield is not politeness, it is required. Each iteration
                // takes m_graphMutex EXCLUSIVELY twice, and with no wait at
                // all the next acquire follows the previous release by
                // essentially zero time. std::shared_mutex is an SRWLOCK and
                // SRWLOCK is not fair, so the UI thread's shared read for a
                // canvas paint can lose that race indefinitely -- the app
                // stays alive and keeps rendering while its UI stops
                // responding, which is indistinguishable from a hang.
                // SwitchToThread gives a ready thread on this processor its
                // chance between iterations.
                m_renderDispatcher.WaitFor(std::chrono::milliseconds(0));
                std::this_thread::yield();
            }
            else
            {
                // Wait for the next frame deadline at the display refresh
                // rate (60 Hz minimum), so a static graph does not spin a
                // core and the frame's own work is absorbed in the period.
                // A frame that overran still idles briefly, for the same
                // lock-fairness reason as the yield above.
                pacer.SetRate(m_targetRefreshHz.load(std::memory_order_relaxed));
                m_renderDispatcher.WaitUntil(pacer.WaitDeadline(std::chrono::steady_clock::now()));
                pacer.BeginFrame(std::chrono::steady_clock::now());
            }
            {
                // Every MCP mutation arrives as a closure drained here. Hold the
                // graph exclusively across the whole drain rather than per
                // closure: RenderThreadDispatcher runs a nested DispatchSync
                // inline when already on the consumer thread, which would
                // self-deadlock a non-recursive mutex taken per closure.
                std::unique_lock<std::shared_mutex> graphLock(m_graphMutex);
                m_renderDispatcher.Drain();
            }
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
                // Per-tick non-GPU work that previously lived in OnRenderTick:
                // working space sync, capture/clock tick, video upload, dirty
                // propagation. Then the offscreen render itself.
                //
                // This whole body mutates m_graph -- UpdateWorkingSpaceNodes
                // writes Working Space properties, the clock tick does
                // node.properties[...] = (a std::map INSERT), and the evaluator
                // walks and dirties nodes. Hold the graph exclusively so the UI
                // thread's canvas paint cannot read a half-mutated node. Taken
                // in a separate scope from the Drain() lock above so the two
                // never nest.
                std::unique_lock<std::shared_mutex> graphLock(m_graphMutex);

                UpdateWorkingSpaceNodes();

                // Use the render-thread D2D context for source uploads. They
                // create D2D bitmaps that the evaluator (also using the
                // render context) will draw -- everything stays on one
                // context to avoid cross-context state races.
                if (auto* dc5 = static_cast<ID2D1DeviceContext5*>(m_renderEngine.RenderD2DContext()))
                {
                    auto& nodes = const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes());
                    if (m_sourceFactory.TickAndUploadLiveCaptures(nodes, dc5))
                        m_forceRender = true;
                }

                // Tick clock nodes: advance time. (Same code as OnRenderTick's
                // body uses; safe to call from render thread because m_graph
                // is single-writer in this design.)
                for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
                {
                    if (!node.isClock) continue;
                    auto getF = [&](const std::wstring& k, float def) {
                        auto it = node.properties.find(k);
                        if (it != node.properties.end())
                            if (auto* f = std::get_if<float>(&it->second)) return *f;
                        return def;
                    };
                    bool autoDuration = getF(L"AutoDuration", 1.0f) > 0.5f;
                    if (autoDuration && node.propertyBindings.count(L"StopTime"))
                    {
                        autoDuration = false;
                        node.properties[L"AutoDuration"] = 0.0f;
                    }
                    if (autoDuration)
                    {
                        float maxDur = 0.0f;
                        for (const auto& other : m_graph.Nodes())
                        {
                            if (other.id == node.id) continue;
                            bool boundToThisClock = false;
                            for (const auto& [propName, binding] : other.propertyBindings)
                            {
                                for (const auto& src : binding.sources)
                                {
                                    if (src && src->sourceNodeId == node.id)
                                    { boundToThisClock = true; break; }
                                }
                                if (boundToThisClock) break;
                            }
                            if (!boundToThisClock) continue;
                            for (const auto& field : other.analysisOutput.fields)
                            {
                                if (field.name == L"Duration" && field.components[0] > 0.0f)
                                    if (field.components[0] > maxDur) maxDur = field.components[0];
                            }
                        }
                        if (maxDur > 0.0f)
                            node.properties[L"StopTime"] = maxDur;
                    }
                    if (node.isPlaying)
                    {
                        float startTime = getF(L"StartTime", 0.0f);
                        float stopTime = getF(L"StopTime", 10.0f);
                        float speed = getF(L"Speed", 1.0f);
                        bool loop = getF(L"Loop", 1.0f) > 0.5f;
                        double duration = static_cast<double>(stopTime - startTime);
                        if (duration <= 0.0) duration = 1.0;
                        node.clockTime += dt * speed;
                        if (loop)
                        {
                            while (node.clockTime >= duration) node.clockTime -= duration;
                            while (node.clockTime < 0.0) node.clockTime += duration;
                        }
                        else
                        {
                            node.clockTime = std::clamp(node.clockTime, 0.0, duration);
                            if (node.clockTime >= duration) node.isPlaying = false;
                        }

                        // UpdateRate gates the DIRTY, not just the value. At 0
                        // the clock behaves as it always has: every frame is a
                        // tick, and every consumer re-evaluates. Above 0 it
                        // only ticks when clockTime crosses into a new 1/rate
                        // bucket, so a 10 Hz clock invalidates downstream work
                        // ten times a second however fast the renderer runs.
                        // GraphEvaluator quantises Time/Progress with the same
                        // rule, so the value a consumer sees always matches the
                        // tick it was woken for.
                        float updateRate = getF(L"UpdateRate", 0.0f);
                        bool emit = true;
                        if (updateRate > 0.0f)
                        {
                            const double step = 1.0 / static_cast<double>(updateRate);
                            const long long bucket =
                                static_cast<long long>(std::floor(node.clockTime / step));
                            emit = (bucket != node.clockTickBucket);
                            node.clockTickBucket = bucket;
                        }
                        if (emit) node.dirty = true;
                    }
                }

                m_graphEvaluator.ResolveSourceBindings(m_graph);

                if (auto* dc = m_renderEngine.RenderD2DContext())
                {
                    try {
                        m_sourceFactory.TickAndUploadVideos(
                            const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()),
                            dc, dt);
                    } catch (...) {}
                }

                // Dirty propagation downstream.
                {
                    std::vector<uint32_t> queue;
                    for (const auto& node : m_graph.Nodes())
                        if (node.dirty) queue.push_back(node.id);
                    for (size_t i = 0; i < queue.size(); ++i)
                    {
                        for (const auto* edge : m_graph.GetOutputEdges(queue[i]))
                        {
                            auto* dn = m_graph.FindNode(edge->destNodeId);
                            if (dn && !dn->dirty)
                            {
                                dn->dirty = true;
                                queue.push_back(edge->destNodeId);
                            }
                        }
                    }
                }

                bool wasForceRender = m_forceRender;
                bool hasDirty = m_graph.HasDirtyNodes();
                // P7: when output windows are open, force eval every frame
                // so the worker keeps producing frames into their offscreen
                // pairs even if no graph node is dirty (e.g. static Gamut
                // Source feeding an Output node). Same gate as the legacy
                // RenderFrame had.
                bool hasOutputWindows = false;
                {
                    std::scoped_lock lk(m_outputSinksMutex);
                    hasOutputWindows = !m_outputSinks.empty();
                }
                // GPU timing implies "keep rendering". With a static graph
                // nothing is dirty, so the worker idles and RenderFrameToOffscreen
                // never runs -- the timer opens ONE frame, the 4-deep ring never
                // retires, and every span reads 0.00 ms. That looks exactly like
                // a broken timer rather than an idle renderer, and cost real time
                // to diagnose. Asking to measure GPU time is asking for frames to
                // measure, so supply them.
                // GPU timing implies "keep rendering", and the flyout's
                // Force-continuous-redraw box says so explicitly. With a static
                // graph nothing is dirty, so the worker idles and
                // RenderFrameToOffscreen never runs -- the timer opens ONE
                // frame, the 4-deep ring never retires, and every span reads
                // 0.00 ms. That looks exactly like a broken timer rather than
                // an idle renderer, and cost real time to diagnose.
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
                bool needsEval = hasDirty || m_needsFitPreview || m_forceRender
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
                    RenderFrameToOffscreen(dt);
                    m_forceRender = false;
                    m_frameCount.fetch_add(1, std::memory_order_relaxed);
                    if (hasDirty || wasForceRender)
                        ++m_graphGeneration;
                    // Fit-after-eval: the selected node's cachedOutput now has
                    // valid bounds, so a pending fit (set by SelectPreviewNode)
                    // computes zoom/pan here on the worker -- worker-owned
                    // bounds + the UI-cached viewport, no XAML. Force one more
                    // frame so the fitted transform actually renders.
                    // Auto-fit re-runs the fit every evaluated frame, so only
                    // force a further frame when the fit actually MOVED the
                    // view -- forcing unconditionally would keep a static
                    // graph rendering forever.
                    if (m_needsFitPreview || m_previewAutoFit.load(std::memory_order_acquire))
                    {
                        const float z0 = m_previewZoom, x0 = m_previewPanX, y0 = m_previewPanY;
                        if (FitPreviewToView())
                        {
                            m_needsFitPreview = false;
                            if (m_previewZoom != z0 || m_previewPanX != x0 || m_previewPanY != y0)
                                m_forceRender = true;
                        }
                    }
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
    }

    void MainWindow::PublishGraphSnapshot()
    {
        auto snapshot = ::ShaderLab::Graph::BuildGraphUiSnapshot(
            m_graph, m_previewNodeId, m_graphGeneration,
            m_frameGeneration.load(std::memory_order_acquire));
        std::atomic_store(&m_uiGraphSnapshot,
            std::shared_ptr<const ::ShaderLab::Graph::GraphUiSnapshot>(std::move(snapshot)));
    }

    // -------------------------------------------------------------------------
    // RenderFrameToOffscreen / BlitOffscreenToSwapChain
    //
    // Phase 7 split: render thread renders the preview image into a double-
    // buffered offscreen D2D bitmap (no swap-chain Present); UI thread later
    // blits the most recently published buffer into the SwapChainPanel-bound
    // swap chain and Presents it.
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

    void MainWindow::RenderFrameToOffscreen(double deltaSec)
    {
        // Runs on render thread once that path is enabled. Currently still
        // safe to call from UI thread for the inline-fallback case (the
        // synchronous dispatcher mode preserves today's behaviour).
        if (m_isShuttingDown) return;
        if (!m_renderEngine.IsInitialized()) return;

        // Pick offscreen size = swap chain back buffer size for now.
        uint32_t w = m_renderEngine.BackBufferWidth();
        uint32_t h = m_renderEngine.BackBufferHeight();
        if (w == 0 || h == 0) return;

        if (!m_renderEngine.EnsureOffscreenTargets(w, h))
            return;

        auto tFrameStart = std::chrono::high_resolution_clock::now();

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
        m_previewRenderStats.SetNode(m_previewNodeId);
        const uint64_t previewChangesBefore = m_graphEvaluator.OutputChangeCount(m_previewNodeId);
        m_previewRenderStats.BeginFrame(std::chrono::steady_clock::now());
        gpu.Begin(::ShaderLab::Rendering::GpuSpan::SourcesPrep);

        // The timer flushes above wait here when the GPU is still busy with
        // earlier frames; that wait is reported as frameStartUs, not as
        // source preparation.
        auto tSourcesStart = std::chrono::high_resolution_clock::now();

        // ---- Source preparation + graph evaluation (same as RenderFrame) ----
        for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
        {
            if (node.type == ::ShaderLab::Graph::NodeType::Source &&
                (node.dirty || m_sourceFactory.GetVideoProvider(node.id)))
            {
                try {
                    m_sourceFactory.PrepareSourceNode(node, dc, deltaSec,
                        m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
                } catch (...) {
                    node.runtimeError = L"Source preparation failed";
                    node.dirty = false;
                }
            }
        }

        auto tSourcesEnd = std::chrono::high_resolution_clock::now();
        gpu.End(::ShaderLab::Rendering::GpuSpan::SourcesPrep);
        gpu.Begin(::ShaderLab::Rendering::GpuSpan::Evaluate);

        // Compute which nodes are needed (mark roots + propagate upstream).
        {
            for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
                node.needed = false;
            std::vector<uint32_t> roots;
            for (const auto& node : m_graph.Nodes())
            {
                if (node.type == ::ShaderLab::Graph::NodeType::Output)
                    roots.push_back(node.id);
                if (node.dirty && node.customEffect.has_value() &&
                    node.customEffect->analysisOutputType == ::ShaderLab::Graph::AnalysisOutputType::Typed)
                    roots.push_back(node.id);
            }
            if (m_previewNodeId != 0)
                roots.push_back(m_previewNodeId);
            if (m_readbackNodeId != 0)
                roots.push_back(m_readbackNodeId);
            for (const auto& window : m_outputWindows)
                roots.push_back(window->NodeId());
            std::unordered_set<uint32_t> visited;
            std::vector<uint32_t> queue = roots;
            while (!queue.empty())
            {
                uint32_t id = queue.back();
                queue.pop_back();
                if (visited.count(id)) continue;
                visited.insert(id);
                auto* node = m_graph.FindNode(id);
                if (node) node->needed = true;
                for (const auto* edge : m_graph.GetInputEdges(id))
                    queue.push_back(edge->sourceNodeId);
                if (node)
                {
                    for (const auto& [propName, binding] : node->propertyBindings)
                    {
                        if (binding.wholeArray)
                            queue.push_back(binding.wholeArraySourceNodeId);
                        for (const auto& src : binding.sources)
                            if (src.has_value()) queue.push_back(src->sourceNodeId);
                    }
                }
            }
        }

        m_graphEvaluator.Evaluate(m_graph, dc);
        if (m_graph.HasDirtyNodes())
            m_graphEvaluator.Evaluate(m_graph, dc); // second pass for new effects

        auto tEvalEnd = std::chrono::high_resolution_clock::now();
        gpu.End(::ShaderLab::Rendering::GpuSpan::Evaluate);

        // ---- BeginDraw on offscreen + ProcessDeferredCompute + draw preview --
        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());
        dc->SetTarget(targetBitmap);
        dc->BeginDraw();
        gpu.Begin(::ShaderLab::Rendering::GpuSpan::DeferredCompute);

        // CPU-analysis interest set (same as old RenderFrame).
        {
            std::unordered_set<uint32_t> interest;
            // "Refresh analysis readouts" set to all nodes: hint every
            // compute node so the canvas labels on unselected nodes refresh
            // at the throttle interval, not only the selected node's.
            if (::ShaderLab::Performance::IsCpuAnalysisHintAllNodesEnabled())
            {
                for (const auto& n : m_graph.Nodes())
                    if (n.type == ::ShaderLab::Graph::NodeType::ComputeShader)
                        interest.insert(n.id);
            }
            if (m_selectedNodeId != 0)
            {
                interest.insert(m_selectedNodeId);
                if (auto* sel = m_graph.FindNode(m_selectedNodeId))
                {
                    for (const auto& [propName, binding] : sel->propertyBindings)
                    {
                        if (binding.wholeArray)
                            interest.insert(binding.wholeArraySourceNodeId);
                        for (const auto& srcOpt : binding.sources)
                            if (srcOpt.has_value())
                                interest.insert(srcOpt->sourceNodeId);
                    }
                }
            }
            m_graphEvaluator.SetCpuAnalysisInterest(std::move(interest));
        }

        if (m_graphEvaluator.ProcessDeferredCompute(m_graph, dc))
        {
            m_nodeGraphController.SetNeedsRedraw();
            if (m_graph.HasDirtyNodes())
            {
                m_graphEvaluator.SetDeferredComputeFrozen(true);
                m_graphEvaluator.Evaluate(m_graph, dc);
                m_graphEvaluator.SetDeferredComputeFrozen(false);
            }
        }

        gpu.End(::ShaderLab::Rendering::GpuSpan::DeferredCompute, dc);
        auto tComputeEnd = std::chrono::high_resolution_clock::now();
        // The Draw span is where the tone mapper actually costs something:
        // D2D evaluates the effect chain lazily at DrawImage/EndDraw, not
        // during Evaluate, so shader time lands here and nowhere else.
        gpu.Begin(::ShaderLab::Rendering::GpuSpan::Draw);
        // Dispatches actually issued, not queue depth: ProcessDeferredCompute
        // drains its queue before returning, so DeferredComputeCount() here
        // read 0 on every frame the app has ever run.
        uint32_t computeCount = m_graphEvaluator.DispatchesLastFrame();

        // Set DPI to 96 to match WinUI DIPs — but only when the context
        // isn't already there: a real per-frame DPI flip invalidates every
        // D2D1_PROPERTY_CACHED effect intermediate in the context. The
        // render context is pinned at 96 (RenderEngine), so this is
        // normally a no-op kept as a safety net.
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        const bool dpiFlip = (oldDpiX != 96.0f || oldDpiY != 96.0f);
        if (dpiFlip)
            dc->SetDpi(96.0f, 96.0f);

        dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        // Pan / zoom are in DIPs (they follow pointer positions); the back
        // buffer is in physical pixels, shown 1:1 via the swap chain's inverse
        // composition-scale matrix. The final Scale maps DIPs onto those
        // pixels. Without it -- and without the matrix -- the preview was
        // magnified by the display scale and anchored top-left.
        const float pxPerDip = m_previewPixelScale.load(std::memory_order_relaxed);
        D2D1_MATRIX_3X2_F previewTransform =
            D2D1::Matrix3x2F::Scale(m_previewZoom, m_previewZoom) *
            D2D1::Matrix3x2F::Translation(m_previewPanX, m_previewPanY) *
            D2D1::Matrix3x2F::Scale(pxPerDip, pxPerDip);
        dc->SetTransform(previewTransform);

        auto* previewImage = ResolveDisplayImage(m_previewNodeId);
        if (previewImage)
            dc->DrawImage(previewImage);

        // Publish the preview image's bounds for everyone else (the fit below
        // this frame, pointer mapping, Pixel Trace, MCP). They must be measured
        // HERE, on the render context that owns the image: an effect image is
        // bound to the context that built it, and the old code asked the UI
        // context, which failed and returned an empty rect -- so the fit
        // deferred forever and the preview sat at zoom 1 on the top-left of the
        // image, and pointer/trace mapping fell back to the viewport size.
        {
            D2D1_RECT_F pb{};
            if (!previewImage || FAILED(dc->GetImageLocalBounds(previewImage, &pb)))
                pb = D2D1_RECT_F{};
            std::scoped_lock lock(m_previewBoundsMutex);
            m_previewBounds = pb;
        }

        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        if (dpiFlip)
            dc->SetDpi(oldDpiX, oldDpiY);

        auto tDrawEnd = std::chrono::high_resolution_clock::now();

        HRESULT hrEnd = dc->EndDraw();
        dc->SetTarget(oldTarget.get());

        gpu.End(::ShaderLab::Rendering::GpuSpan::Draw);
        gpu.End(::ShaderLab::Rendering::GpuSpan::Frame);
        gpu.EndFrame();
        m_previewRenderStats.EndFrame(
            m_previewNodeId != 0 && m_graphEvaluator.OutputChangeCount(m_previewNodeId) != previewChangesBefore,
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

            for (auto& n : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
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
                else if (n.id == m_previewNodeId && chainMs > 0.0)
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

                n.lastGpuMs = ms;
                n.gpuState = state;
            }

            // The figures change every frame, so the canvas has to be told
            // it is stale -- otherwise the annotations freeze at whatever was
            // on screen when the last topology change happened, which looks
            // exactly like a broken measurement.
            if (m_nodeGraphController.ShowNodeGpuStats())
                m_nodeGraphController.SetNeedsRedraw();
        }

        auto tEndDraw = std::chrono::high_resolution_clock::now();

        // Always bump framesSampled BEFORE deciding whether to publish, so
        // /perf accurately reflects worker activity even when EndDraw fails
        // (device-lost, transient state, etc.). Without this, a single
        // EndDraw glitch would freeze the displayed FPS at a stale value.
        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_frameTiming;
            t.frameStartUs      = t.frameStartUs      * (1-a) + usec(tFrameStart,  tSourcesStart) * a;
            t.sourcesPrepUs     = t.sourcesPrepUs     * (1-a) + usec(tSourcesStart, tSourcesEnd) * a;
            t.evaluateUs        = t.evaluateUs        * (1-a) + usec(tSourcesEnd,  tEvalEnd)    * a;
            t.deferredComputeUs = t.deferredComputeUs * (1-a) + usec(tEvalEnd,     tComputeEnd) * a;
            t.drawUs            = t.drawUs            * (1-a) + usec(tComputeEnd,  tDrawEnd)    * a;
            t.endDrawFlushUs    = t.endDrawFlushUs    * (1-a) + usec(tDrawEnd,     tEndDraw)    * a;
            t.computeDispatches = computeCount;
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
            if (t.framesSampled % 30 == 0)
                m_lastFrameTiming = t;
        }

        if (FAILED(hrEnd))
            return;

        // Publish: store this buffer's index with release semantics so the
        // UI thread sees a fully-rendered frame before reading.
        m_offscreenPublishedIdx.store(writeIdx, std::memory_order_release);
        m_offscreenPublishedVersion.fetch_add(1, std::memory_order_release);

        // P7: render any open output windows into THEIR offscreen pairs.
        // Each sink has its own double-buffered offscreen managed by this
        // method. UI thread blits the published buffer in BlitAndPresent.
        RenderOutputSinks();
    }

    // ---------------------------------------------------------------------
    // RenderOutputSinks -- render-thread output-window rendering. Iterates
    // a snapshot of m_outputSinks (so we don't hold m_outputSinksMutex while
    // doing GPU work), and for each non-closed sink:
    //   - reads view state under sink->viewMutex
    //   - ensures buffers exist at requested size (creates D3D textures +
    //     render-side D2D bitmap targets, bumps bufferGen on size change)
    //   - resolves the node's cachedOutput
    //   - picks the write idx (opposite of publishedIdx)
    //   - BeginDraw on render-side bitmap, Clear, apply pan/zoom transform,
    //     DrawImage, EndDraw
    //   - publishes the new idx + version
    // The actual blit-to-swap-chain + Present1 happens on the UI thread.
    // ---------------------------------------------------------------------
    void MainWindow::RenderOutputSinks()
    {
        std::vector<std::shared_ptr<::ShaderLab::Controls::OutputSinkRenderState>> snapshot;
        {
            std::scoped_lock lock(m_outputSinksMutex);
            snapshot = m_outputSinks;
        }
        if (snapshot.empty()) return;

        auto* dc = m_renderEngine.RenderD2DContext();
        auto* d3dDevice = m_renderEngine.D3DDevice();
        if (!dc || !d3dDevice) return;
        const auto& fmt = m_renderEngine.ActiveFormat();

        for (auto& sink : snapshot)
        {
            if (!sink) continue;

            bool closed = false;
            {
                std::scoped_lock lock(sink->viewMutex);
                closed = sink->closed;
            }
            if (closed) continue;

            // Resolve the source image (the node's cachedOutput).
            auto* image = ResolveDisplayImage(sink->nodeId);
            if (!image) continue;

            // Get the image's natural bounds in DIPs at 96 DPI.
            float oldDpiX, oldDpiY;
            dc->GetDpi(&oldDpiX, &oldDpiY);
            dc->SetDpi(96.0f, 96.0f);
            D2D1_RECT_F bounds{};
            HRESULT bhr = dc->GetImageLocalBounds(image, &bounds);
            if (FAILED(bhr))
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                continue;
            }
            uint32_t imgW = static_cast<uint32_t>(bounds.right - bounds.left);
            uint32_t imgH = static_cast<uint32_t>(bounds.bottom - bounds.top);
            if (imgW == 0 || imgH == 0)
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                continue;
            }
            // Cap to a sensible max so a runaway-bounds source can't OOM
            // us. 8192 matches the per-effect cap in PreRenderInputBitmap.
            imgW = (std::min)(imgW, 8192u);
            imgH = (std::min)(imgH, 8192u);

            // Allocate the offscreen at the IMAGE's native size. This
            // keeps the render path stateless about the panel's display
            // size / DPI -- UI's BlitAndPresent does the fit when blitting
            // into the swap chain back buffer (which it knows the size of
            // first-hand). Avoids the cross-thread DPI/scale math that
            // produced misaligned output earlier.
            if (sink->bufW != imgW || sink->bufH != imgH ||
                !sink->textures[0] || !sink->textures[1])
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = imgW;
                td.Height = imgH;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = fmt.dxgiFormat;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

                bool ok = true;
                for (uint32_t i = 0; i < 2 && ok; ++i)
                {
                    sink->renderTargets[i] = nullptr;
                    sink->textures[i] = nullptr;
                    if (FAILED(d3dDevice->CreateTexture2D(&td, nullptr,
                            sink->textures[i].put())))
                    { ok = false; break; }
                    winrt::com_ptr<IDXGISurface> surface;
                    if (FAILED(sink->textures[i]->QueryInterface(
                            IID_PPV_ARGS(surface.put()))))
                    { ok = false; break; }
                    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                        D2D1::PixelFormat(fmt.dxgiFormat,
                            D2D1_ALPHA_MODE_PREMULTIPLIED),
                        96.0f, 96.0f);
                    if (FAILED(dc->CreateBitmapFromDxgiSurface(
                            surface.get(), bp, sink->renderTargets[i].put())))
                    { ok = false; break; }
                }
                if (!ok)
                {
                    for (uint32_t i = 0; i < 2; ++i)
                    {
                        sink->renderTargets[i] = nullptr;
                        sink->textures[i] = nullptr;
                    }
                    sink->bufW = sink->bufH = 0;
                    dc->SetDpi(oldDpiX, oldDpiY);
                    continue;
                }
                sink->bufW = imgW;
                sink->bufH = imgH;
                sink->bufferGen.fetch_add(1, std::memory_order_release);
            }

            // Pick write idx = opposite of last published.
            int32_t lastPub = sink->publishedIdx.load(std::memory_order_acquire);
            int32_t writeIdx = (lastPub == 0) ? 1 : 0;
            auto* target = sink->renderTargets[writeIdx].get();
            if (!target)
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                continue;
            }

            winrt::com_ptr<ID2D1Image> prevTarget;
            dc->GetTarget(prevTarget.put());
            dc->SetTarget(target);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(-bounds.left, -bounds.top));
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));
            dc->DrawImage(image);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            HRESULT hr = dc->EndDraw();
            dc->SetTarget(prevTarget.get());
            dc->SetDpi(oldDpiX, oldDpiY);
            if (FAILED(hr)) continue;

            sink->publishedIdx.store(writeIdx, std::memory_order_release);
            sink->publishedVersion.fetch_add(1, std::memory_order_release);
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
        // it to 96 * CompositionScale on every repaint and leaves it there; the
        // preview blit used to inherit that, upscaling the finished frame 1.5x
        // at 150% scaling on top of the panel's own composition scale -- the
        // preview showed a zoomed-in corner of a correctly fitted image.
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
        // the accepted cost; see Performance::IsUnthrottledRenderEnabled.
        const UINT interval =
            ::ShaderLab::Performance::IsUnthrottledRenderEnabled() ? 0u : 1u;
        DXGI_PRESENT_PARAMETERS params{};
        swap->Present1(interval, 0, &params);
    }

}
