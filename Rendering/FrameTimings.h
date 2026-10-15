#pragma once

// The GUI's per-frame performance timings, split by the thread that writes
// them, and a board that publishes copies any thread may read. Times are
// microseconds, rolling averages unless noted.

#include <cstdint>
#include <mutex>

namespace ShaderLab::Rendering
{
    // One render-thread frame (RenderFrameToOffscreen). The phases add up to
    // totalUs, the cost of one graph evaluation.
    struct RenderFrameTimings
    {
        double totalUs{};            // whole frame
        double frameStartUs{};       // opening the GPU frame timers; waits while the GPU is behind
        double sourcesPrepUs{};      // PrepareSources (image / video upload)
        double evaluateUs{};         // GraphEvaluator::Evaluate
        double deferredComputeUs{};  // D3D11 compute dispatches and the frozen sweep
        double drawUs{};             // drawing the preview into the offscreen target
        double endDrawFlushUs{};     // EndDraw; Present happens on the UI thread
        uint32_t computeDispatches{};
        uint32_t framesSampled{};
        uint32_t endDrawFailed{};    // EndDraw failures (D2DERR_RECREATE_TARGET, ...)

        // Evaluator effect cache, as of this frame.
        size_t   cachedEffects{};
        uint64_t cacheInvalidations{};

        // GPU execution time from D3D11 timestamp queries. Everything above
        // is CPU time around an asynchronous API: it measures recording, not
        // execution. Not averaged, and late by up to GpuTimer::kLatency frames.
        bool     gpuAvailable{};        // the device supports timestamp queries
        bool     gpuEnabled{};          // timing is switched on
        uint64_t gpuFramesResolved{};
        uint32_t gpuDisjointDrops{};    // samples voided by a GPU clock change
        double   gpuFrameMs{};
        double   gpuSourcesPrepMs{};
        double   gpuEvaluateMs{};
        double   gpuDeferredComputeMs{};
        double   gpuDrawMs{};           // the effect chain executes here (D2D is lazy)
    };

    // UI-thread costs; not part of a frame's total.
    struct UiFrameTimings
    {
        double uiTickUs{};           // OnRenderTick: drain, blit, Present1, canvas redraw
        double outputWindowsUs{};    // presenting the output windows
    };

    // The last published timings of both threads. Each thread publishes its
    // own; any thread reads a consistent copy.
    class FrameTimingBoard
    {
    public:
        void Publish(const RenderFrameTimings& timings)
        {
            std::lock_guard lock(m_mutex);
            m_render = timings;
        }

        void Publish(const UiFrameTimings& timings)
        {
            std::lock_guard lock(m_mutex);
            m_ui = timings;
        }

        RenderFrameTimings Render() const
        {
            std::lock_guard lock(m_mutex);
            return m_render;
        }

        UiFrameTimings Ui() const
        {
            std::lock_guard lock(m_mutex);
            return m_ui;
        }

    private:
        mutable std::mutex  m_mutex;
        RenderFrameTimings  m_render;
        UiFrameTimings      m_ui;
    };
}
