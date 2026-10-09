#pragma once

// FramePacer
//
// Frame deadlines for the throttled render worker. Deadlines sit on a fixed
// grid of 1/rate, so wake-up jitter does not lower the average rate. A frame
// that overruns its slot is followed by at most one frame after a short idle
// gap; the grid then restarts from that frame instead of bursting to repay
// missed slots.
//
// Pure scheduling: the caller waits (RenderThreadDispatcher::WaitUntil) and
// passes in the time each iteration starts.

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace ShaderLab::Rendering
{
    class FramePacer
    {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr uint32_t scMinRateHz = 60;
        static constexpr uint32_t scMaxRateHz = 240;
        // Idle time after a frame that overran its deadline. Without it the
        // loop re-takes its exclusive locks immediately, and an SRWLOCK
        // waiter (the UI thread's shared read) can lose that race forever.
        static constexpr Clock::duration scOverrunIdle = std::chrono::milliseconds(1);

        static uint32_t ClampRate(uint32_t rateHz)
        {
            return std::clamp(rateHz, scMinRateHz, scMaxRateHz);
        }

        explicit FramePacer(uint32_t rateHz = scMinRateHz) { SetRate(rateHz); }

        // Clamped to [scMinRateHz, scMaxRateHz]. A change takes effect from
        // the last frame start, so the pending deadline moves with it.
        void SetRate(uint32_t rateHz)
        {
            rateHz = ClampRate(rateHz);
            if (rateHz == m_rateHz) return;
            m_rateHz = rateHz;
            m_period = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / static_cast<double>(rateHz)));
            if (m_lastFrameStart != Clock::time_point{})
                m_deadline = m_lastFrameStart + m_period;
        }

        uint32_t RateHz() const { return m_rateHz; }
        Clock::duration Period() const { return m_period; }

        // When the next paced frame is due. Before the first frame this is
        // the clock's epoch, so the first wait returns at once.
        Clock::time_point Deadline() const { return m_deadline; }

        // What to wait for when the frame's work ends at `now`: the deadline,
        // or scOverrunIdle from now if the deadline has already passed.
        Clock::time_point WaitDeadline(Clock::time_point now) const
        {
            if (m_lastFrameStart == Clock::time_point{} || m_deadline > now) return m_deadline;
            return now + scOverrunIdle;
        }

        // Call at the top of every iteration. Returns true and schedules the
        // next deadline when this iteration is the paced frame; returns false
        // for an early wake (queued work) that leaves the deadline unchanged.
        bool BeginFrame(Clock::time_point now)
        {
            if (now < m_deadline) return false;
            m_lastFrameStart = now;
            m_deadline += m_period;
            if (m_deadline <= now)
                m_deadline = now + m_period;
            return true;
        }

    private:
        uint32_t m_rateHz{ 0 };
        Clock::duration m_period{};
        Clock::time_point m_deadline{};
        Clock::time_point m_lastFrameStart{};
    };
}
