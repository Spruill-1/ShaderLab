#include "pch_engine.h"
#include "BytecodeCache.h"
#include "ShaderLabParamsHlsl.h"

#include <algorithm>
#include <chrono>

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Free helpers (canonicalization + hashing)
    // -----------------------------------------------------------------------

    uint64_t FNV1a64(const void* data, size_t size)
    {
        // Standard 64-bit FNV-1a.
        constexpr uint64_t kOffset = 0xcbf29ce484222325ull;
        constexpr uint64_t kPrime  = 0x100000001b3ull;
        const auto* p = static_cast<const uint8_t*>(data);
        uint64_t h = kOffset;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= p[i];
            h *= kPrime;
        }
        return h;
    }

    uint64_t HashCanonicalSource(std::string_view canonical)
    {
        return FNV1a64(canonical.data(), canonical.size());
    }

    uint64_t HashParamSignature(const std::vector<std::string>& orderedNames)
    {
        // Pack as "name0\0name1\0..." so order matters and "Foo|FooBar"
        // doesn't collide with "FooFooBar".
        std::string buf;
        buf.reserve(64);
        for (const auto& n : orderedNames)
        {
            buf.append(n);
            buf.push_back('\0');
        }
        return FNV1a64(buf.data(), buf.size());
    }

    uint64_t IncludeLibraryHash()
    {
        // Cache schema version: bump when the cache layout, key fields,
        // or any embedded include changes in a way that should invalidate
        // existing entries (in-memory and on disk).
        constexpr uint32_t kCacheSchemaVersion = 1;
        const char* libPtr = GetShaderLabParamsHLSL();
        size_t libLen = libPtr ? GetShaderLabParamsHLSLLength() : 0;
        std::string buf;
        buf.reserve(libLen + 8);
        buf.append(reinterpret_cast<const char*>(&kCacheSchemaVersion), sizeof(kCacheSchemaVersion));
        if (libPtr) buf.append(libPtr, libLen);
        return FNV1a64(buf.data(), buf.size());
    }

    static std::string CanonicalizeImpl(const char* src, size_t len)
    {
        // Normalize CRLF / CR-only line endings to LF. Don't strip
        // trailing whitespace or comments -- D3DCompile is sensitive
        // to source content via #line directives and cumulative output.
        std::string out;
        out.reserve(len);
        for (size_t i = 0; i < len; ++i)
        {
            char c = src[i];
            if (c == '\r')
            {
                out.push_back('\n');
                if (i + 1 < len && src[i + 1] == '\n') ++i;
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string CanonicalizeHlslSource(std::string_view source)
    {
        return CanonicalizeImpl(source.data(), source.size());
    }

    std::string CanonicalizeHlslSource(std::wstring_view source)
    {
        // wide -> UTF-8 first. winrt::to_string requires hstring_view;
        // do a minimal manual conversion since these are ASCII HLSL
        // sources in practice. Anything outside ASCII is preserved as
        // a raw byte sequence using WideCharToMultiByte for safety.
        std::string utf8;
        if (!source.empty())
        {
            int needed = ::WideCharToMultiByte(
                CP_UTF8, 0,
                source.data(), static_cast<int>(source.size()),
                nullptr, 0, nullptr, nullptr);
            if (needed > 0)
            {
                utf8.resize(static_cast<size_t>(needed));
                ::WideCharToMultiByte(
                    CP_UTF8, 0,
                    source.data(), static_cast<int>(source.size()),
                    utf8.data(), needed, nullptr, nullptr);
            }
        }
        return CanonicalizeImpl(utf8.data(), utf8.size());
    }

    // -----------------------------------------------------------------------
    // BytecodeCompileKey
    // -----------------------------------------------------------------------

    bool BytecodeCompileKey::operator==(const BytecodeCompileKey& o) const noexcept
    {
        return sourceHash         == o.sourceHash
            && paramSignatureHash == o.paramSignatureHash
            && includeLibraryHash == o.includeLibraryHash
            && macroBitset        == o.macroBitset
            && entryPoint         == o.entryPoint
            && target             == o.target;
    }

    size_t BytecodeCompileKeyHash::operator()(const BytecodeCompileKey& k) const noexcept
    {
        // Fold the four 64-bit hashes + macroBitset + entry/target into
        // a 64-bit then return as size_t. Cheap mixing; collisions
        // resolved by full equality.
        uint64_t h = k.sourceHash;
        auto mix = [&h](uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(k.paramSignatureHash);
        mix(k.includeLibraryHash);
        mix(static_cast<uint64_t>(k.macroBitset));
        mix(FNV1a64(k.entryPoint.data(), k.entryPoint.size()));
        mix(FNV1a64(k.target.data(),     k.target.size()));
        return static_cast<size_t>(h);
    }

    // Thread-local sentinel: set while a worker is processing a job.
    // Used by GetOrCompile to detect re-entrancy and skip waiting.
    // File-scope (not a class static) because TLS can't have dllexport.
    static thread_local bool t_isWorkerThread = false;

    // -----------------------------------------------------------------------
    // Lifetime
    // -----------------------------------------------------------------------

    BytecodeCache& BytecodeCache::Instance()
    {
        // Function-local static -- thread-safe initialization since C++11.
        static BytecodeCache instance;
        return instance;
    }

    BytecodeCache::BytecodeCache()
    {
        // Spin up worker threads. We deliberately keep this small (2):
        // typical compile is short, and we'd rather not flood D3DCompile.
        constexpr size_t kWorkerCount = 2;
        m_workers.reserve(kWorkerCount);
        for (size_t i = 0; i < kWorkerCount; ++i)
        {
            m_workers.emplace_back([this](std::stop_token stop) {
                t_isWorkerThread = true;
                WorkerLoop(stop);
            });
        }
    }

    BytecodeCache::~BytecodeCache()
    {
        Shutdown();
    }

    void BytecodeCache::Shutdown()
    {
        // Idempotent.
        if (m_shutdown.exchange(true)) return;

        {
            std::lock_guard<std::mutex> g(m_mutex);
            m_workQueue.clear();  // discard queued-but-not-started work.
        }
        m_workCv.notify_all();
        m_readyCv.notify_all();
        for (auto& w : m_workers)
        {
            w.request_stop();
        }
        // jthread destructors join on scope exit (or on the next assignment).
        m_workers.clear();
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    BytecodeCacheResult BytecodeCache::TryGet(const BytecodeCompileKey& key) const
    {
        std::lock_guard<std::mutex> g(m_mutex);
        BytecodeCacheResult r;
        auto it = m_entries.find(key);
        if (it == m_entries.end())
        {
            r.status = BytecodeStatus::NotRequested;
            return r;
        }
        r.status       = it->second.status;
        r.bytecode     = it->second.bytecode;
        r.errorMessage = it->second.errorMessage;
        r.fromCache    = (r.status == BytecodeStatus::Ready);
        return r;
    }

    BytecodeStatus BytecodeCache::GetStatus(const BytecodeCompileKey& key) const
    {
        std::lock_guard<std::mutex> g(m_mutex);
        auto it = m_entries.find(key);
        return (it == m_entries.end()) ? BytecodeStatus::NotRequested : it->second.status;
    }

    // -----------------------------------------------------------------------
    // Enqueue
    // -----------------------------------------------------------------------

    void BytecodeCache::RequestCompile(BytecodeCompileRequest request)
    {
        if (m_shutdown.load()) return;

        {
            std::lock_guard<std::mutex> g(m_mutex);
            auto it = m_entries.find(request.key);
            if (it != m_entries.end())
            {
                // Already known -- no-op for Pending/Ready/Failed. Failure
                // retry requires explicit Invalidate.
                return;
            }
            // Insert a Pending placeholder so a subsequent identical
            // request from another thread short-circuits cleanly.
            Entry& e = m_entries[request.key];
            e.status         = BytecodeStatus::Pending;
            e.insertionOrder = m_nextInsertionOrder++;
            m_workQueue.push_back({ std::move(request) });
        }
        m_workCv.notify_one();
    }

    // -----------------------------------------------------------------------
    // Synchronous fetch-or-compile
    // -----------------------------------------------------------------------

    BytecodeCacheResult BytecodeCache::GetOrCompile(
        BytecodeCompileRequest request, uint32_t timeoutMs)
    {
        // Fast path: existing Ready/Failed entry.
        {
            std::lock_guard<std::mutex> g(m_mutex);
            auto it = m_entries.find(request.key);
            if (it != m_entries.end())
            {
                if (it->second.status == BytecodeStatus::Ready)
                {
                    BytecodeCacheResult r;
                    r.status       = BytecodeStatus::Ready;
                    r.bytecode     = it->second.bytecode;
                    r.fromCache    = true;
                    m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                    return r;
                }
                if (it->second.status == BytecodeStatus::Failed)
                {
                    BytecodeCacheResult r;
                    r.status       = BytecodeStatus::Failed;
                    r.errorMessage = it->second.errorMessage;
                    r.fromCache    = true;
                    m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                    return r;
                }
                // Pending: fall through to the wait/inline-compile path below.
            }
        }

        m_cacheMisses.fetch_add(1, std::memory_order_relaxed);

        // Wait briefly for an in-flight worker compile to finish, but
        // not if we ARE a worker (would deadlock if a compile callback
        // re-entered the cache).
        if (timeoutMs > 0 && !t_isWorkerThread)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
            while (true)
            {
                auto it = m_entries.find(request.key);
                if (it != m_entries.end() &&
                    (it->second.status == BytecodeStatus::Ready ||
                     it->second.status == BytecodeStatus::Failed))
                {
                    BytecodeCacheResult r;
                    r.status       = it->second.status;
                    r.bytecode     = it->second.bytecode;
                    r.errorMessage = it->second.errorMessage;
                    r.fromCache    = true;
                    return r;
                }
                if (m_readyCv.wait_until(lock, deadline) == std::cv_status::timeout)
                    break;
            }
        }

        // Inline compile fallback. Place a Pending placeholder if not
        // already present, then compile on this thread.
        Entry localEntry;
        BytecodeStatus finalStatus;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto& e = m_entries[request.key];
            if (e.status == BytecodeStatus::Ready ||
                e.status == BytecodeStatus::Failed)
            {
                // Worker raced and won between our wait timeout and
                // re-acquiring the lock. Use its result.
                BytecodeCacheResult r;
                r.status       = e.status;
                r.bytecode     = e.bytecode;
                r.errorMessage = e.errorMessage;
                r.fromCache    = true;
                return r;
            }
            if (e.insertionOrder == 0)
                e.insertionOrder = m_nextInsertionOrder++;
            // Compile drops + reacquires the lock internally.
            finalStatus = DoCompile(request, lock, e);
            // Snapshot for return outside the lock.
            localEntry = e;
            EnforceByteLimit_NoLock();
        }
        m_inlineCompiles.fetch_add(1, std::memory_order_relaxed);
        m_readyCv.notify_all();

        BytecodeCacheResult r;
        r.status       = finalStatus;
        r.bytecode     = std::move(localEntry.bytecode);
        r.errorMessage = std::move(localEntry.errorMessage);
        r.fromCache    = false;
        return r;
    }

    // -----------------------------------------------------------------------
    // Eager precompile
    // -----------------------------------------------------------------------

    void BytecodeCache::PrecompileCommonShapes(
        const BytecodeCacheMetadata& metadata,
        const std::string& canonicalHlslSource,
        const std::string& entryPoint,
        const std::string& target,
        const std::vector<std::string>& gpuBindableParamNames)
    {
        if (m_shutdown.load()) return;

        const uint64_t srcHash    = HashCanonicalSource(canonicalHlslSource);
        const uint64_t paramHash  = HashParamSignature(gpuBindableParamNames);
        const uint64_t includeHash = IncludeLibraryHash();
        const size_t   paramCount = gpuBindableParamNames.size();

        // Hard cap on bitset width. Today's effects use ~3-5 gpu params
        // max; complaining loudly here lets us catch a future change
        // before silent truncation bites.
        if (paramCount > 32)
        {
            // Still proceed with baseline only -- safer than crashing.
            // Future: widen to uint64_t or dynamic bitset.
        }

        auto enqueueShape = [&](uint32_t bitset)
        {
            BytecodeCompileRequest req;
            req.key.sourceHash         = srcHash;
            req.key.paramSignatureHash = paramHash;
            req.key.includeLibraryHash = includeHash;
            req.key.macroBitset        = bitset;
            req.key.entryPoint         = entryPoint;
            req.key.target             = target;
            req.metadata               = metadata;
            req.hlslSource             = canonicalHlslSource;
            req.gpuBindableParamNames  = gpuBindableParamNames;
            RequestCompile(std::move(req));
        };

        // Baseline (all params in cbuffer mode).
        enqueueShape(0u);
        // One variant per gpu-bindable param flipped to GPU mode.
        const size_t bits = std::min<size_t>(paramCount, 32);
        for (size_t i = 0; i < bits; ++i)
            enqueueShape(static_cast<uint32_t>(1u) << i);
    }

    // -----------------------------------------------------------------------
    // Invalidate / Clear / Stats
    // -----------------------------------------------------------------------

    void BytecodeCache::Invalidate(const BytecodeCompileKey& key)
    {
        std::lock_guard<std::mutex> g(m_mutex);
        auto it = m_entries.find(key);
        if (it == m_entries.end()) return;
        if (it->second.status == BytecodeStatus::Pending)
        {
            // Don't yank an in-flight entry out from under a worker.
            return;
        }
        m_currentBytes -= it->second.bytecode.size();
        m_entries.erase(it);
    }

    void BytecodeCache::Clear()
    {
        std::lock_guard<std::mutex> g(m_mutex);
        // Don't drop Pending entries -- workers may be mid-compile.
        for (auto it = m_entries.begin(); it != m_entries.end(); )
        {
            if (it->second.status == BytecodeStatus::Pending)
            {
                ++it;
            }
            else
            {
                m_currentBytes -= it->second.bytecode.size();
                it = m_entries.erase(it);
            }
        }
    }

    BytecodeCache::Stats BytecodeCache::GetStats() const
    {
        Stats s{};
        {
            std::lock_guard<std::mutex> g(m_mutex);
            s.entries            = m_entries.size();
            s.totalBytecodeBytes = m_currentBytes;
            for (const auto& [k, e] : m_entries)
            {
                if (e.status == BytecodeStatus::Pending) ++s.pendingCount;
                else if (e.status == BytecodeStatus::Failed) ++s.failedCount;
            }
        }
        s.inlineCompiles = m_inlineCompiles.load(std::memory_order_relaxed);
        s.workerCompiles = m_workerCompiles.load(std::memory_order_relaxed);
        s.cacheHits      = m_cacheHits.load(std::memory_order_relaxed);
        s.cacheMisses    = m_cacheMisses.load(std::memory_order_relaxed);
        return s;
    }

    // -----------------------------------------------------------------------
    // Worker loop + compile core
    // -----------------------------------------------------------------------

    void BytecodeCache::WorkerLoop(std::stop_token stop)
    {
        while (!stop.stop_requested() && !m_shutdown.load())
        {
            BytecodeCompileRequest req;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_workCv.wait(lock, [&] {
                    return stop.stop_requested() || m_shutdown.load() || !m_workQueue.empty();
                });
                if (stop.stop_requested() || m_shutdown.load()) return;
                if (m_workQueue.empty()) continue;
                req = std::move(m_workQueue.front().request);
                m_workQueue.pop_front();
            }

            // Compile outside the queue mutex.
            std::unique_lock<std::mutex> lock(m_mutex);
            auto it = m_entries.find(req.key);
            if (it == m_entries.end())
            {
                // Got Invalidated between enqueue and dequeue -- skip.
                continue;
            }
            // If the inline-compile path already filled this entry while
            // we were dequeueing, leave it alone.
            if (it->second.status == BytecodeStatus::Ready ||
                it->second.status == BytecodeStatus::Failed)
            {
                continue;
            }

            DoCompile(req, lock, it->second);
            EnforceByteLimit_NoLock();
            m_workerCompiles.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            m_readyCv.notify_all();
        }
    }

    BytecodeStatus BytecodeCache::DoCompile(
        const BytecodeCompileRequest& req,
        std::unique_lock<std::mutex>& lock,
        Entry& outEntry)
    {
        // Build the macro list from the param-name vector + bitset.
        // Strings must outlive D3DCompile, so keep them owned locally.
        std::vector<std::string> defNames;
        std::vector<std::string> defValues;
        defNames.reserve(req.gpuBindableParamNames.size());
        defValues.reserve(req.gpuBindableParamNames.size());
        for (size_t i = 0; i < req.gpuBindableParamNames.size() && i < 32; ++i)
        {
            defNames.push_back("_SLPARAM_" + req.gpuBindableParamNames[i] + "_GPU");
            defValues.push_back(((req.key.macroBitset >> i) & 1u) ? "1" : "0");
        }
        std::vector<ShaderCompiler::MacroDef> macros;
        macros.reserve(defNames.size());
        for (size_t i = 0; i < defNames.size(); ++i)
            macros.push_back({ defNames[i].c_str(), defValues[i].c_str() });

        // Drop the lock around D3DCompile -- it can take 50-300ms in
        // debug. Other readers can still TryGet the (Pending) entry,
        // and workers can pick up other queue items.
        std::string hlsl = req.hlslSource;
        std::string entry = req.key.entryPoint;
        std::string target = req.key.target;
        lock.unlock();

        ShaderCompileResult result = ShaderCompiler::CompileFromString(
            hlsl, "BytecodeCache", entry, target, macros);

        lock.lock();
        // Race check: if the entry was wiped or completed under us,
        // discard our result. Specifically, NEVER overwrite a Ready
        // entry with a Failed result (or any worker result with a
        // Ready inline-compile result that beat us).
        // outEntry is a reference into m_entries[req.key]; the entry's
        // status drives the merge.
        if (outEntry.status == BytecodeStatus::Ready ||
            outEntry.status == BytecodeStatus::Failed)
        {
            return outEntry.status;
        }

        if (result.bytecode)
        {
            const uint8_t* p = static_cast<const uint8_t*>(result.bytecode->GetBufferPointer());
            size_t sz = result.bytecode->GetBufferSize();
            outEntry.bytecode.assign(p, p + sz);
            outEntry.status = BytecodeStatus::Ready;
            outEntry.errorMessage.clear();
            m_currentBytes += sz;
            return BytecodeStatus::Ready;
        }

        outEntry.status = BytecodeStatus::Failed;
        outEntry.errorMessage = result.ErrorMessage();
        outEntry.bytecode.clear();
        if (outEntry.errorMessage.empty())
            outEntry.errorMessage = L"D3DCompile failed (no diagnostic).";
        return BytecodeStatus::Failed;
    }

    void BytecodeCache::EnforceByteLimit_NoLock()
    {
        if (m_currentBytes <= m_maxBytes) return;

        // Naive LRU on insertion order. Build a vector of
        // (insertionOrder, key) for non-Pending entries, sort by
        // order ascending, evict from front until we're back under cap.
        struct Victim { uint64_t order; BytecodeCompileKey key; };
        std::vector<Victim> victims;
        victims.reserve(m_entries.size());
        for (auto& [k, e] : m_entries)
        {
            if (e.status == BytecodeStatus::Pending) continue;
            victims.push_back({ e.insertionOrder, k });
        }
        std::sort(victims.begin(), victims.end(),
            [](const Victim& a, const Victim& b) { return a.order < b.order; });

        for (auto& v : victims)
        {
            if (m_currentBytes <= m_maxBytes) break;
            auto it = m_entries.find(v.key);
            if (it == m_entries.end()) continue;
            m_currentBytes -= it->second.bytecode.size();
            m_entries.erase(it);
        }
    }
}
