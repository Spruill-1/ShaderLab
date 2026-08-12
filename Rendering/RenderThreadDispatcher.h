#pragma once

// RenderThreadDispatcher
//
// MPSC closure queue used to marshal work onto the render thread (or, until
// Phase 7 lands the dedicated worker, onto whichever thread calls Drain()).
//
// Producer model: any thread (UI, MCP HTTP listener, NodeGraphController)
// calls DispatchAsync(fn) for fire-and-forget work, or DispatchSync<T>(fn)
// when it needs the closure's return value (or just to know it's been
// applied). Closures run on the consumer thread, which is the single writer
// to EffectGraph -- so closure bodies can call m_graph.X(...) directly.
//
// Consumer model: the render thread (or its eventual replacement) calls
// Drain() once per loop iteration. Wait() blocks the consumer until at
// least one closure is enqueued or shutdown is signaled. RegisterConsumer()
// captures the consumer's std::thread::id so re-entrant DispatchSync calls
// from inside a closure run inline (avoiding self-deadlock).
//
// Headless / synchronous mode (constructor flag): both DispatchAsync and
// DispatchSync invoke the closure inline on the calling thread. The headless
// host needs this because there is no separate render thread there; the MCP
// listener thread is the sole consumer and must run closures synchronously.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace ShaderLab::Rendering
{
    class RenderThreadDispatcher
    {
    public:
        // synchronous=true: producer-side calls run the closure inline on the
        // calling thread. Used by ShaderLabHeadless and the existing 154-test
        // suite, where there is no separate render thread.
        explicit RenderThreadDispatcher(bool synchronous = false)
            : m_synchronous(synchronous) {}

        ~RenderThreadDispatcher() { Shutdown(); }

        RenderThreadDispatcher(const RenderThreadDispatcher&) = delete;
        RenderThreadDispatcher& operator=(const RenderThreadDispatcher&) = delete;

        // ------------------------------------------------------------------
        // Producer side (any thread)
        // ------------------------------------------------------------------

        // Enqueue a closure. Returns immediately. In synchronous mode (or when
        // called re-entrantly from the consumer thread), the closure runs
        // inline on the calling thread instead of being queued. Fire-and-
        // forget work is simply dropped once the dispatcher is shutting down.
        void DispatchAsync(std::function<void()> fn)
        {
            if (!fn) return;
            if (m_synchronous || IsConsumerThread())
            {
                fn();
                return;
            }
            // Wrap as the cancel-aware queue element: on cancel it is a no-op
            // (async work has no promise to fail).
            Item item = [fn = std::move(fn)](bool cancelled) { if (!cancelled) fn(); };
            {
                std::scoped_lock lock(m_mutex);
                if (m_shuttingDown) return;
                m_queue.push_back(std::move(item));
            }
            m_cv.notify_one();
        }

        // Enqueue a closure and block the calling thread until it has run.
        // Throws std::runtime_error on timeout or shutdown. Re-throws any
        // exception the closure produced.
        template <class F>
        auto DispatchSync(F&& fn,
                          std::chrono::milliseconds timeout = std::chrono::seconds(30))
            -> std::invoke_result_t<F>
        {
            using R = std::invoke_result_t<F>;

            // Re-entry from the consumer thread: run inline. The consumer is
            // single-threaded, so any call coming from inside an already-
            // executing closure is by definition safe to recurse.
            if (m_synchronous || IsConsumerThread())
            {
                if constexpr (std::is_void_v<R>) { fn(); return; }
                else                              return fn();
            }

            // Use shared_ptr so the promise survives if the calling thread
            // times out before the consumer runs the closure.
            auto prom = std::make_shared<std::promise<R>>();
            auto fut = prom->get_future();

            // Cancel-aware element: when the dispatcher shuts down (or an
            // adapter switch resets the consumer) the queued item is invoked
            // with cancelled=true so this promise FAILS FAST rather than the
            // caller eating its full timeout. This is what makes the
            // render < DispatchSync < shim < client timeout ladder
            // enforceable (see Engine/Mcp/McpTimeouts.h).
            Item item =
                [prom, fn = std::forward<F>(fn)](bool cancelled) mutable
                {
                    if (cancelled)
                    {
                        try { throw std::runtime_error(
                            "RenderThreadDispatcher::DispatchSync: dispatcher shut down"); }
                        catch (...) {
                            try { prom->set_exception(std::current_exception()); }
                            catch (...) {}
                        }
                        return;
                    }
                    try
                    {
                        if constexpr (std::is_void_v<R>) { fn(); prom->set_value(); }
                        else                              prom->set_value(fn());
                    }
                    catch (...)
                    {
                        try { prom->set_exception(std::current_exception()); }
                        catch (...) { /* promise already satisfied */ }
                    }
                };

            bool queued = false;
            {
                std::scoped_lock lock(m_mutex);
                if (!m_shuttingDown)
                {
                    m_queue.push_back(std::move(item));
                    queued = true;
                }
            }
            if (queued)
                m_cv.notify_one();
            else
                item(true);   // shutting down: fail the promise immediately

            if (fut.wait_for(timeout) != std::future_status::ready)
                throw std::runtime_error("RenderThreadDispatcher::DispatchSync: timed out");

            if constexpr (std::is_void_v<R>) { fut.get(); return; }
            else                              return fut.get();
        }

        // ------------------------------------------------------------------
        // Consumer side (render thread, or initially the UI thread)
        // ------------------------------------------------------------------

        // Capture this thread as the single consumer. Used so that re-entrant
        // DispatchSync calls from inside a closure run inline.
        void RegisterConsumer()
        {
            m_consumerId.store(std::this_thread::get_id(), std::memory_order_release);
        }

        // Drain all currently-queued closures. Returns the number drained.
        // Each closure runs without holding the queue mutex, so closures may
        // call DispatchAsync from inside (which queues for the *next* drain).
        std::size_t Drain()
        {
            if (m_synchronous) return 0;
            if (m_consumerId.load(std::memory_order_acquire) == std::thread::id{})
                RegisterConsumer();

            std::deque<Item> local;
            {
                std::scoped_lock lock(m_mutex);
                local.swap(m_queue);
            }
            for (auto& fn : local)
            {
                try { fn(false); }
                catch (...)
                {
                    // Closures own their own error reporting (e.g. promises).
                    // Suppress here so a single bad closure can't kill the
                    // consumer loop.
                }
            }
            return local.size();
        }

        // Block the consumer until at least one closure is enqueued or
        // Shutdown() is called. Spurious wakeups are fine -- callers should
        // follow with Drain() and then loop.
        void Wait()
        {
            if (m_synchronous) return;
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_shuttingDown || !m_queue.empty(); });
        }

        // Like Wait() but with a timeout so the consumer can periodically
        // run other work (e.g. pump animation) even when fully idle.
        bool WaitFor(std::chrono::milliseconds timeout)
        {
            if (m_synchronous) return true;
            std::unique_lock lock(m_mutex);
            return m_cv.wait_for(lock, timeout,
                [this] { return m_shuttingDown || !m_queue.empty(); });
        }

        // Wake the consumer (Wait/WaitFor return) without enqueueing work.
        // Useful when the consumer needs to re-check external state (dirty
        // bits, animation, resize) on demand.
        void Wake()
        {
            if (m_synchronous) return;
            m_cv.notify_all();
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        // Lifecycle: clear consumer registration. Useful when the consumer
        // thread exits and a new consumer is about to register (e.g. adapter
        // switch teardown -> new RenderEngineThread). Caller must guarantee
        // no thread is currently calling Drain/Wait. Pending DispatchSync
        // promises are FAILED (not silently dropped), so a request in flight
        // during an adapter switch returns an error instead of hanging.
        void ResetConsumer()
        {
            std::deque<Item> local;
            {
                std::scoped_lock lock(m_mutex);
                local.swap(m_queue);
                m_shuttingDown = false;
            }
            for (auto& fn : local) { try { fn(true); } catch (...) {} }
            m_consumerId.store(std::thread::id{}, std::memory_order_release);
        }

        // Stop accepting new work. Pending closures are invoked with
        // cancelled=true so their DispatchSync promises FAIL FAST (rather
        // than the caller eating a 30 s timeout). Wait()/WaitFor() return
        // immediately; subsequent DispatchSync calls also fail fast.
        void Shutdown()
        {
            if (m_synchronous) return;
            std::deque<Item> local;
            {
                std::scoped_lock lock(m_mutex);
                m_shuttingDown = true;
                local.swap(m_queue);
            }
            for (auto& fn : local) { try { fn(true); } catch (...) {} }
            m_cv.notify_all();
        }

        bool IsShuttingDown() const
        {
            std::scoped_lock lock(m_mutex);
            return m_shuttingDown;
        }

        std::size_t QueueDepth() const
        {
            if (m_synchronous) return 0;
            std::scoped_lock lock(m_mutex);
            return m_queue.size();
        }

        bool IsSynchronous() const { return m_synchronous; }

    private:
        bool IsConsumerThread() const
        {
            return m_consumerId.load(std::memory_order_acquire) ==
                   std::this_thread::get_id();
        }

        // Queue element carries a cancel flag: run(false) executes normally,
        // run(true) fails any attached promise (shutdown / consumer reset).
        using Item = std::function<void(bool)>;

        const bool m_synchronous;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<Item> m_queue;
        std::atomic<std::thread::id> m_consumerId{};
        bool m_shuttingDown{ false };
    };
}
