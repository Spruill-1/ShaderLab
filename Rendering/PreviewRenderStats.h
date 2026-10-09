#pragma once

// PreviewRenderStats
//
// What the status bar reports for the previewed node: how often its output
// actually re-rendered, and how long the last such frame took through the
// GPU. The render loop's tick rate is not either of these: a clean graph
// evaluates to cached outputs, and a 24 fps video re-renders 24 times a
// second at any tick rate.
//
// Render-thread owned. The two published numbers are atomics, so the UI and
// MCP threads read them without touching the graph.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

#include <d3d11.h>
#include <winrt/base.h>

namespace ShaderLab::Rendering
{
    // Renders per second over a sliding window, from the times a node rendered.
    class RenderRateMeter
    {
    public:
        using Clock = std::chrono::steady_clock;

        // One second: long enough to hold ~24 intervals of a 24 fps clip, short
        // enough that a stopped graph reads N/A a second later.
        static constexpr Clock::duration scWindow = std::chrono::seconds(1);
        // Renders slower than the window are still a rate while they keep
        // coming, up to this interval.
        static constexpr Clock::duration scMaxInterval = std::chrono::seconds(5);

        void Reset() { m_renders.clear(); }

        void RecordRender(Clock::time_point when)
        {
            m_renders.push_back(when);
            while (m_renders.size() > 2 && when - m_renders.front() > scMaxInterval)
                m_renders.pop_front();
        }

        // Renders per second, or nullopt when the node has not rendered recently.
        std::optional<double> Rate(Clock::time_point now) const
        {
            if (m_renders.empty())
                return std::nullopt;
            const Clock::time_point newest = m_renders.back();

            // Renders inside the window.
            size_t count = 0;
            Clock::time_point oldest = newest;
            for (auto it = m_renders.rbegin(); it != m_renders.rend() && now - *it <= scWindow; ++it)
            {
                oldest = *it;
                ++count;
            }

            if (count >= 2)
            {
                // Still at its cadence: the exact rate from the span of renders.
                const double span = Seconds(newest - oldest);
                const double interval = span / static_cast<double>(count - 1);
                if (span > 0.0 && Seconds(now - newest) <= 2.0 * interval)
                    return static_cast<double>(count - 1) / span;
                // A burst that has stopped: count it over the window until it ages out.
                return static_cast<double>(count) / Seconds(scWindow);
            }

            // Slower than the window: the last interval, while the next render is due.
            if (m_renders.size() >= 2)
            {
                const auto interval = newest - m_renders[m_renders.size() - 2];
                if (interval > scWindow && interval <= scMaxInterval && now - newest <= interval)
                    return 1.0 / Seconds(interval);
            }
            return count == 1 ? std::optional<double>(1.0 / Seconds(scWindow)) : std::nullopt;
        }

    private:
        static double Seconds(Clock::duration duration)
        {
            return std::chrono::duration<double>(duration).count();
        }

        std::deque<Clock::time_point> m_renders;
    };

    // Whole-frame time from a pair of D3D11 timestamps, read back a few
    // frames late with no stall. The start timestamp is flushed at frame
    // start, the end one after the frame's last submission; both flushes sit
    // outside the frame's D2D work, so unlike the per-phase GpuTimer they do
    // not split D2D's batches.
    //
    // Drivers disagree on when the start timestamp runs. Most run it on
    // flush, and the span covers the frame's CPU recording plus the GPU's
    // tail. Adreno can hold it until the frame's first real GPU work, so the
    // span starts late and can miss the CPU time. The total is the longer of
    // the span and the frame's CPU time: exact when the span covers the CPU
    // time, a lower bound when it does not. Adding the two would double the
    // common case, where the span covers it to within microseconds.
    //
    // Some drivers retire a frame's queries only once later work is
    // submitted, so results arrive a frame or more late.
    class GpuFrameTimer
    {
    public:
        static constexpr uint32_t scSlots = 8;

        struct FrameTime
        {
            double cpuMs{ 0.0 };   // frame start to last submission
            double gpuMs{ 0.0 };   // the timestamp span
            double totalMs{ 0.0 }; // frame start to GPU done
        };

        bool Initialize(ID3D11Device* device)
        {
            Shutdown();
            if (!device) return false;
            winrt::com_ptr<ID3D11DeviceContext> context;
            device->GetImmediateContext(context.put());
            D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC timestampDesc{ D3D11_QUERY_TIMESTAMP, 0 };
            for (auto& slot : m_slots)
            {
                if (FAILED(device->CreateQuery(&disjointDesc, slot.disjoint.put())) ||
                    FAILED(device->CreateQuery(&timestampDesc, slot.begin.put())) ||
                    FAILED(device->CreateQuery(&timestampDesc, slot.end.put())))
                {
                    Shutdown();
                    return false;
                }
            }
            m_device.copy_from(device);
            m_context = std::move(context);
            return true;
        }

        void Shutdown()
        {
            for (auto& slot : m_slots)
                slot = {};
            m_context = nullptr;
            m_device = nullptr;
            m_frameOpen = false;
            m_writeIndex = 0;
        }

        bool IsInitialized() const { return m_context != nullptr; }
        ID3D11Device* Device() const { return m_device.get(); }

        void BeginFrame()
        {
            if (!m_context) return;
            Collect();
            auto& slot = m_slots[m_writeIndex];
            // Every slot still in flight: skip this frame rather than wait.
            if (slot.inFlight) return;
            m_context->Begin(slot.disjoint.get());
            m_context->End(slot.begin.get());
            m_context->Flush();
            m_frameOpen = true;
        }

        // `keep` marks a frame whose time should be reported; `cpuMs` is its
        // CPU time from frame start to now.
        void EndFrame(bool keep, double cpuMs)
        {
            if (!m_context || !m_frameOpen) return;
            auto& slot = m_slots[m_writeIndex];
            m_context->End(slot.end.get());
            m_context->End(slot.disjoint.get());
            // Submit now, or the end timestamp waits for whatever flushes next.
            m_context->Flush();
            slot.inFlight = true;
            slot.keep = keep;
            slot.cpuMs = cpuMs;
            m_writeIndex = (m_writeIndex + 1) % scSlots;
            m_frameOpen = false;
        }

        // Reads every retired frame, oldest first, without flushing.
        void Collect()
        {
            if (!m_context) return;
            for (uint32_t i = 0; i < scSlots; ++i)
            {
                auto& slot = m_slots[(m_writeIndex + i) % scSlots];
                if (!slot.inFlight) continue;
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
                if (m_context->GetData(slot.disjoint.get(), &disjoint, sizeof(disjoint),
                                       D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                    continue;
                uint64_t begin = 0, end = 0;
                if (m_context->GetData(slot.begin.get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                    m_context->GetData(slot.end.get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                    continue;
                slot.inFlight = false;
                if (disjoint.Disjoint) ++m_disjointFrames;
                if (slot.keep && !disjoint.Disjoint && disjoint.Frequency > 0 && end >= begin)
                {
                    FrameTime frame;
                    frame.cpuMs = slot.cpuMs;
                    frame.gpuMs = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
                    frame.totalMs = (std::max)(frame.gpuMs, frame.cpuMs);
                    m_last = frame;
                    ++m_keptFrames;
                }
            }
        }

        // The most recent kept frame that has retired.
        std::optional<FrameTime> Last() const { return m_keptFrames ? std::optional<FrameTime>(m_last) : std::nullopt; }
        uint64_t KeptFrames() const { return m_keptFrames; }
        // Retired frames whose timestamps the driver flagged unreliable.
        uint64_t DisjointFrames() const { return m_disjointFrames; }

    private:
        struct Slot
        {
            winrt::com_ptr<ID3D11Query> disjoint;
            winrt::com_ptr<ID3D11Query> begin;
            winrt::com_ptr<ID3D11Query> end;
            bool inFlight{ false };
            bool keep{ false };
            double cpuMs{ 0.0 };
        };

        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        Slot m_slots[scSlots];
        uint32_t m_writeIndex{ 0 };
        bool m_frameOpen{ false };
        FrameTime m_last;
        uint64_t m_keptFrames{ 0 };
        uint64_t m_disjointFrames{ 0 };
    };

    // The previewed node's render rate and frame time. The render thread
    // brackets each evaluated frame and says whether the node rendered in it,
    // then publishes once per loop iteration, rendered or not, so the rate
    // ages to N/A on a still graph.
    class PreviewRenderStats
    {
    public:
        using Clock = std::chrono::steady_clock;

        void Initialize(ID3D11Device* device)
        {
            if (device && device != m_gpu.Device())
                m_gpu.Initialize(device);
        }

        // Starts a new measurement when the previewed node changes.
        void SetNode(uint32_t nodeId)
        {
            if (nodeId == m_nodeId) return;
            m_nodeId = nodeId;
            m_meter.Reset();
            m_lastCpuMs.reset();
        }
        uint32_t Node() const { return m_nodeId; }

        void BeginFrame(Clock::time_point now)
        {
            m_frameStart = now;
            m_gpu.BeginFrame();
        }

        void EndFrame(bool rendered, Clock::time_point now)
        {
            const double cpuMs = std::chrono::duration<double, std::milli>(now - m_frameStart).count();
            m_gpu.EndFrame(rendered, cpuMs);
            if (!rendered) return;
            m_meter.RecordRender(now);
            m_lastCpuMs = cpuMs;
        }

        // Collects retired GPU frames and publishes the numbers.
        void Publish(Clock::time_point now)
        {
            m_gpu.Collect();
            const auto rate = m_meter.Rate(now);
            m_fps.store(rate ? *rate : -1.0, std::memory_order_release);
            double ms = -1.0, cpuMs = -1.0, gpuMs = -1.0;
            if (rate)
            {
                if (auto frame = m_gpu.Last())
                {
                    ms = frame->totalMs;
                    cpuMs = frame->cpuMs;
                    gpuMs = frame->gpuMs;
                }
                else if (m_lastCpuMs)
                {
                    ms = cpuMs = *m_lastCpuMs;
                }
            }
            m_ms.store(ms, std::memory_order_release);
            m_cpuMs.store(cpuMs, std::memory_order_release);
            m_gpuMs.store(gpuMs, std::memory_order_release);
        }

        // Renders per second of the previewed node; negative when N/A.
        double Fps() const { return m_fps.load(std::memory_order_acquire); }
        // Milliseconds of its last render, frame start to GPU done; negative when N/A.
        double Ms() const { return m_ms.load(std::memory_order_acquire); }
        // The parts of Ms: CPU time to the last submission, and the GPU
        // timestamp span (negative without GPU timestamps).
        double CpuMs() const { return m_cpuMs.load(std::memory_order_acquire); }
        double GpuMs() const { return m_gpuMs.load(std::memory_order_acquire); }
        bool GpuTimed() const { return m_gpu.IsInitialized(); }

    private:
        RenderRateMeter m_meter;
        GpuFrameTimer m_gpu;
        uint32_t m_nodeId{ 0 };
        Clock::time_point m_frameStart{};
        std::optional<double> m_lastCpuMs;
        std::atomic<double> m_fps{ -1.0 };
        std::atomic<double> m_ms{ -1.0 };
        std::atomic<double> m_cpuMs{ -1.0 };
        std::atomic<double> m_gpuMs{ -1.0 };
    };
}
