#include "pch_engine.h"
#include "../TestSupport.h"
#include "Graph/GraphModel.h"
#include "Rendering/FramePacer.h"
#include "Rendering/GpuTimer.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/RenderThreadDispatcher.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// The render loop pieces hosts share: the frame pacer and the render-thread
// dispatcher.
namespace
{
    using namespace ShaderLab::Tests;

    SL_TEST_CASE(TestFramePacer, Tag::Fast | Tag::Timing)
    {
        printf("\n=== FramePacer + RenderThreadDispatcher::WaitUntil ===\n");
        using ShaderLab::Rendering::FramePacer;
        using Clock = std::chrono::steady_clock;
        using std::chrono::milliseconds;

        // ---- Deadline schedule (synthetic time, no waiting). -------------
        {
            FramePacer pacer(60);
            const auto period = pacer.Period();
            TEST("FramePacer first deadline is due immediately", pacer.Deadline() == Clock::time_point{});
            const auto start = Clock::now();
            TEST("FramePacer first frame is paced", pacer.BeginFrame(start));
            TEST("FramePacer next deadline is one period later", pacer.Deadline() == start + period);
            TEST("FramePacer early wake leaves the deadline",
                 !pacer.BeginFrame(start + period / 2) && pacer.Deadline() == start + period);
            TEST("FramePacer late wake stays on the grid",
                 pacer.BeginFrame(start + period + milliseconds(3)) && pacer.Deadline() == start + 2 * period);
            const auto overrun = start + 5 * period;
            TEST("FramePacer overrun schedules from now",
                 pacer.BeginFrame(overrun) && pacer.Deadline() == overrun + period);
            TEST("FramePacer overrun does not burst", !pacer.BeginFrame(overrun + milliseconds(1)));
            TEST("FramePacer waits for a deadline still ahead",
                 pacer.WaitDeadline(overrun + milliseconds(1)) == pacer.Deadline());
            const auto lateEnd = pacer.Deadline() + milliseconds(4);
            TEST("FramePacer idles after an overrun",
                 pacer.WaitDeadline(lateEnd) == lateEnd + FramePacer::scOverrunIdle);
            TEST("FramePacer first wait is immediate", FramePacer(60).WaitDeadline(start) == Clock::time_point{});

            const auto period60 = std::chrono::duration<double, std::milli>(period).count();
            TEST("FramePacer 60 Hz period is 16.67 ms", std::abs(period60 - 1000.0 / 60.0) < 1e-3);
            pacer.SetRate(144);
            const auto period144 = std::chrono::duration<double, std::milli>(pacer.Period()).count();
            TEST("FramePacer rate change keeps fractional periods", std::abs(period144 - 1000.0 / 144.0) < 1e-3);
            TEST("FramePacer rate change re-anchors on the last frame", pacer.Deadline() == overrun + pacer.Period());
            TEST("FramePacer clamps below 60 Hz", FramePacer(30).RateHz() == 60);
            TEST("FramePacer clamps above 240 Hz", FramePacer(1000).RateHz() == 240);
        }

        // ---- Wake() ends a predicate wait. --------------------------------
        {
            ShaderLab::Rendering::RenderThreadDispatcher dispatcher;
            std::atomic<bool> woke{ false };
            std::atomic<long long> waitedMs{ 0 };
            std::thread consumer([&] {
                dispatcher.RegisterConsumer();
                const auto begin = Clock::now();
                woke = dispatcher.WaitFor(std::chrono::seconds(5));
                waitedMs = std::chrono::duration_cast<milliseconds>(Clock::now() - begin).count();
            });
            std::this_thread::sleep_for(milliseconds(20));
            dispatcher.Wake();
            consumer.join();
            TEST("Wake() returns WaitFor early", woke.load() && waitedMs.load() < 1000);
        }

        // ---- WaitUntil: queued work wakes it before the deadline. ---------
        {
            ShaderLab::Rendering::RenderThreadDispatcher dispatcher;
            std::atomic<bool> woke{ false };
            std::atomic<long long> waitedMs{ 0 };
            std::thread consumer([&] {
                dispatcher.RegisterConsumer();
                const auto begin = Clock::now();
                woke = dispatcher.WaitUntil(begin + std::chrono::seconds(5));
                waitedMs = std::chrono::duration_cast<milliseconds>(Clock::now() - begin).count();
                dispatcher.Drain();
            });
            std::this_thread::sleep_for(milliseconds(20));
            dispatcher.DispatchAsync([] {});
            consumer.join();
            TEST("WaitUntil returns early for queued work", woke.load() && waitedMs.load() < 1000);
        }

        // ---- WaitUntil: timer precision. ----------------------------------
        // A 15.6 ms system-tick wait would overshoot a 5 ms deadline by
        // several milliseconds; the high-resolution timer should not.
        {
            ShaderLab::Rendering::RenderThreadDispatcher dispatcher;
            dispatcher.RegisterConsumer();
            std::vector<double> overshootMs;
            bool anyWoke = false;
            for (int i = 0; i < 20; ++i)
            {
                const auto deadline = Clock::now() + milliseconds(5);
                anyWoke |= dispatcher.WaitUntil(deadline);
                overshootMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - deadline).count());
            }
            std::sort(overshootMs.begin(), overshootMs.end());
            const double medianMs = overshootMs[overshootMs.size() / 2];
            printf("  [info] WaitUntil 5 ms deadline overshoot: min %.3f median %.3f max %.3f ms\n",
                   overshootMs.front(), medianMs, overshootMs.back());
            TEST("WaitUntil reports the deadline, not a wake", !anyWoke);
            TEST("WaitUntil never returns before the deadline", overshootMs.front() >= 0.0);
            TIMING_TEST("WaitUntil median overshoot < 2 ms", medianMs < 2.0);
        }

        // ---- The render worker's throttled loop holds 60 Hz. --------------
        // Pacer plus WaitUntil with 10 ms of spun work per frame.
        {
            ShaderLab::Rendering::RenderThreadDispatcher dispatcher;
            dispatcher.RegisterConsumer();
            FramePacer pacer(60);
            const auto begin = Clock::now();
            auto frameStart = begin;
            int frames = 0;
            while (Clock::now() - begin < std::chrono::seconds(1))
            {
                dispatcher.WaitUntil(pacer.WaitDeadline(Clock::now()));
                frameStart = Clock::now();
                if (!pacer.BeginFrame(frameStart)) continue;
                ++frames;
                const auto workEnd = frameStart + milliseconds(10);
                while (Clock::now() < workEnd) YieldProcessor();
            }
            const double hz = (frames - 1) / std::chrono::duration<double>(frameStart - begin).count();
            printf("  [info] paced loop, 10 ms work: %.1f Hz over %d frames\n", hz, frames);
            TIMING_TEST("Paced worker loop runs at 60 +- 2 Hz", hz >= 58.0 && hz <= 62.0);
        }
    }

    // A span closed without being opened in the same frame must not report:
    // its begin query would hold a timestamp from an earlier frame.
    SL_TEST_CASE(TestGpuTimerUnopenedSpan, Tag::Gpu)
    {
        using ShaderLab::Rendering::GpuSpan;
        ShaderLab::Rendering::GpuTimer timer;
        TEST("GpuTimer_Initializes", timer.Initialize(g_d3dDevice.get()));
        timer.SetEnabled(true);

        timer.BeginFrame();
        timer.Begin(GpuSpan::Evaluate);
        timer.End(GpuSpan::Evaluate);
        timer.EndFrame();
        timer.CollectBlocking();
        const double opened = timer.SpanMs(GpuSpan::Evaluate);

        // Cycle the ring back to the first slot, then close Evaluate unopened.
        for (uint32_t frame = 1; frame < ShaderLab::Rendering::GpuTimer::kLatency; ++frame)
        {
            timer.BeginFrame();
            timer.EndFrame();
            timer.CollectBlocking();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        timer.BeginFrame();
        timer.Begin(GpuSpan::Frame);
        timer.End(GpuSpan::Evaluate);
        timer.End(GpuSpan::Frame);
        timer.EndFrame();
        TEST("GpuTimer_FrameResolves", timer.CollectBlocking());
        TEST("GpuTimer_UnopenedSpanKeepsLastValue", timer.SpanMs(GpuSpan::Evaluate) == opened);
        timer.Shutdown();
    }

    SL_TEST_CASE(TestRenderThreadDispatcher, Tag::Fast)
    {
        printf("\n=== RenderThreadDispatcher ===\n");

        // ---- Synchronous mode runs inline, no threading. ------------------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d{ /*synchronous=*/true };
            int counter = 0;
            d.DispatchAsync([&]{ counter++; });
            TEST("synchronous mode runs DispatchAsync inline",
                 counter == 1);

            int sum = d.DispatchSync([]{ return 7 + 35; });
            TEST("synchronous mode DispatchSync returns value", sum == 42);

            // Re-entry from inside a closure runs inline.
            int reentry = 0;
            d.DispatchSync([&]{
                d.DispatchSync([&]{ reentry = 99; });
            });
            TEST("synchronous mode allows re-entrant DispatchSync",
                 reentry == 99);
        }

        // ---- Async-with-consumer-thread happy path. -----------------------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> stop{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                while (!stop.load(std::memory_order_acquire))
                {
                    d.WaitFor(std::chrono::milliseconds(50));
                    d.Drain();
                }
                d.Drain();
            });

            std::atomic<int> hits{ 0 };
            for (int i = 0; i < 100; ++i)
                d.DispatchAsync([&]{ hits.fetch_add(1, std::memory_order_relaxed); });

            // DispatchSync from a non-consumer thread blocks until done.
            int v = d.DispatchSync([&]{
                return hits.load(std::memory_order_relaxed);
            });
            TEST("async drain ran all enqueued closures (>=100)", v >= 100);
            TEST("async DispatchSync returned a value seen on consumer",
                 v == 100);

            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();
        }

        // ---- The after-sync hook runs before the caller is released. -----
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            int published = 0;
            int written = 0;
            d.SetAfterSyncClosure([&]{ published = written; });
            std::atomic<bool> stop{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                while (!stop.load(std::memory_order_acquire))
                {
                    d.WaitFor(std::chrono::milliseconds(50));
                    d.Drain();
                }
                d.Drain();
            });

            d.DispatchSync([&]{ written = 5; });
            TEST("after-sync hook sees the closure's write", published == 5);
            const int returned = d.DispatchSync([&]{ written = 9; return 1; });
            TEST("after-sync hook runs for value-returning closures",
                 returned == 1 && published == 9);

            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();
        }

        // ---- Re-entrant DispatchSync from inside a closure runs inline. ---
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> stop{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                while (!stop.load(std::memory_order_acquire))
                {
                    d.WaitFor(std::chrono::milliseconds(50));
                    d.Drain();
                }
                d.Drain();
            });

            int outer = 0, inner = 0;
            d.DispatchSync([&]{
                outer = 1;
                // From inside a closure (= consumer thread), DispatchSync
                // must NOT requeue and self-deadlock; it must run inline.
                d.DispatchSync([&]{ inner = 2; });
            });
            TEST("re-entrant DispatchSync did not deadlock",
                 outer == 1 && inner == 2);

            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();
        }

        // ---- Closure exception propagates back through DispatchSync. ------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d{ /*synchronous=*/true };
            bool threw = false;
            try
            {
                d.DispatchSync([]() -> int {
                    throw std::runtime_error("expected test failure");
                });
            }
            catch (const std::exception&) { threw = true; }
            TEST("DispatchSync re-throws closure exception", threw);
        }

        // ---- Shutdown cancels pending closures and unblocks waiters. ------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> consumerRan{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                d.Wait();          // returns when shutdown signaled
                consumerRan.store(true, std::memory_order_release);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            d.Shutdown();
            consumer.join();
            TEST("Shutdown wakes Wait()", consumerRan.load());
            TEST("Shutdown clears queue", d.QueueDepth() == 0);
            TEST("IsShuttingDown reflects state", d.IsShuttingDown());
        }

        // ---- P19: many concurrent producers, single consumer. ------------
        // Stress test: 8 producer threads each post 250 sync closures
        // returning a unique tag. All 2000 closures must run on the consumer
        // thread (single thread id seen) and each producer must observe its
        // own tag back.
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> stop{ false };
            std::atomic<int> totalRan{ 0 };
            std::atomic<std::thread::id> consumerId{};
            std::atomic<bool> mixedThread{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                consumerId.store(std::this_thread::get_id(), std::memory_order_release);
                while (!stop.load(std::memory_order_acquire)) {
                    d.WaitFor(std::chrono::milliseconds(5));
                    d.Drain();
                }
                d.Drain();
            });

            constexpr int producers = 8;
            constexpr int perProducer = 250;
            std::vector<std::thread> producerThreads;
            std::atomic<int> mismatches{ 0 };
            producerThreads.reserve(producers);
            for (int p = 0; p < producers; ++p) {
                producerThreads.emplace_back([&, p]{
                    for (int i = 0; i < perProducer; ++i) {
                        int tag = p * 1000 + i;
                        int got = d.DispatchSync([&, tag]{
                            if (std::this_thread::get_id() != consumerId.load())
                                mixedThread.store(true);
                            totalRan.fetch_add(1, std::memory_order_relaxed);
                            return tag;
                        });
                        if (got != tag) mismatches.fetch_add(1);
                    }
                });
            }
            for (auto& t : producerThreads) t.join();
            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();

            TEST("8 producers x 250 closures all ran",
                totalRan.load() == producers * perProducer);
            TEST("every closure ran on the consumer thread",
                !mixedThread.load());
            TEST("every DispatchSync returned its own tag",
                mismatches.load() == 0);
        }

        // ---- P19: shutdown with many pending closures unblocks all. ------
        // Producers that are blocked in DispatchSync must wake when Shutdown
        // is called. They get an exception (std::runtime_error from the
        // dispatcher), not a hang.
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::thread consumer([&]{
                d.RegisterConsumer();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                // never drain -- closures pile up in the queue
            });

            constexpr int producers = 4;
            std::vector<std::thread> producerThreads;
            std::atomic<int> threwCount{ 0 };
            std::atomic<int> noThrowCount{ 0 };
            for (int p = 0; p < producers; ++p) {
                producerThreads.emplace_back([&]{
                    try {
                        d.DispatchSync([]{ return 1; });
                        noThrowCount.fetch_add(1);
                    } catch (...) {
                        threwCount.fetch_add(1);
                    }
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            d.Shutdown();
            for (auto& t : producerThreads) t.join();
            consumer.join();

            TEST("Shutdown unblocks pending DispatchSync producers",
                threwCount.load() + noThrowCount.load() == producers);
            TEST("at least some producers observed shutdown",
                threwCount.load() > 0);
        }

        // ---- Shutdown fails pending promises fast (no timeout) ------------
        // A DispatchSync in flight when Shutdown() fires must throw promptly,
        // not eat its full timeout -- that is what makes the timeout ladder
        // enforceable during GUI shutdown / adapter switch.
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::thread consumer([&] { d.RegisterConsumer(); std::this_thread::sleep_for(
                std::chrono::milliseconds(10)); /* never drains */ });
            std::atomic<bool> threw{ false };
            auto t0 = std::chrono::steady_clock::now();
            std::thread producer([&] {
                try { d.DispatchSync([] { return 7; }, std::chrono::seconds(30)); }
                catch (...) { threw.store(true); }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            d.Shutdown();
            producer.join();
            consumer.join();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            TEST("Shutdown fails pending DispatchSync", threw.load());
            TEST("Shutdown fail is fast (<2s, not the 30s timeout)", elapsedMs < 2000);
        }

        // ---- DispatchSync after Shutdown fails fast, not queued -----------
        // (No consumer registered on this thread: a producer-side call takes
        // the queue path, where shutdown fails it fast. A re-entrant call
        // from the consumer thread would legitimately run inline instead.)
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            d.Shutdown();
            bool threw = false;
            auto t0 = std::chrono::steady_clock::now();
            try { d.DispatchSync([] { return 1; }, std::chrono::seconds(30)); }
            catch (...) { threw = true; }
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            TEST("DispatchSync after Shutdown throws immediately", threw && elapsedMs < 2000);
        }

        // ---- ResetConsumer (adapter switch) fails pending too -------------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::thread consumer([&] { d.RegisterConsumer(); std::this_thread::sleep_for(
                std::chrono::milliseconds(10)); });
            std::atomic<bool> threw{ false };
            std::thread producer([&] {
                try { d.DispatchSync([] { return 3; }, std::chrono::seconds(30)); }
                catch (...) { threw.store(true); }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            d.ResetConsumer();
            producer.join();
            consumer.join();
            TEST("ResetConsumer fails pending DispatchSync", threw.load());
            // After reset the dispatcher is reusable (adapter respawn).
            TEST("ResetConsumer leaves dispatcher usable", !d.IsShuttingDown());
        }

        // ---- A timed-out DispatchSync never runs its closure later. ------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> releaseSlowItem{ false };
            std::atomic<bool> stop{ false };
            std::thread consumer([&] {
                d.RegisterConsumer();
                while (!stop.load(std::memory_order_acquire))
                {
                    d.WaitFor(std::chrono::milliseconds(5));
                    d.Drain();
                }
                d.Drain();
            });

            // Block the consumer behind a slow item.
            d.DispatchAsync([&] {
                while (!releaseSlowItem.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });

            auto bodyRan = std::make_shared<std::atomic<bool>>(false);
            bool timedOut = false;
            try { d.DispatchSync([bodyRan] { bodyRan->store(true); }, std::chrono::milliseconds(1)); }
            catch (const std::runtime_error&) { timedOut = true; }

            releaseSlowItem.store(true, std::memory_order_release);
            d.DispatchSync([] {});   // the abandoned item has been drained by now
            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();

            TEST("DispatchSync timeout throws", timedOut);
            TEST("DispatchSync timeout leaves the closure unexecuted", !bodyRan->load());
        }
    }
}
