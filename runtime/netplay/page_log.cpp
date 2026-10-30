#include "netplay/page_log.h"

#include "config.h"
#include "crash_handler.h"
#include "gpu/memory_watch.h"
#include "guest_memory.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/game_state.h"
#include "netplay/write_tracker.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>

#include <immintrin.h>
#include <intrin.h>
#include <windows.h>

namespace netplay::pagelog
{
    namespace
    {
        // A page's byte in the store map: kQuiet, nothing to do (not watched, or copied in this frame already); kArmed,
        // watched and not written since the frame began; kCopying, being copied by a thread now.
        constexpr uint8_t kQuiet = 0, kArmed = 1, kCopying = 2;
        constexpr uint32_t kChunkPages = 256;                       // a chunk of copies: 1 MB
        constexpr uint32_t kMaxLogPages = 1u << 16;                 // a frame's log holds at most 256 MB
        constexpr uint32_t kMaxChunks = kMaxLogPages / kChunkPages;
        constexpr uint32_t kViewA = 0xA0000, kViewC = 0xC0000, kViewEnd = 0xE0000; // physical views, in pages

        struct FrameLog
        {
            uint64_t frame = 0;                // the boundary its pages are as of
            std::atomic<uint32_t> count{ 0 };  // pages copied (and slots taken)
            std::atomic<bool> overflow{ false };
            std::unique_ptr<uint32_t[]> pages{ new uint32_t[kMaxLogPages] };
            std::atomic<uint8_t*> chunks[kMaxChunks] = {};
            uint32_t Count() const { return std::min(count.load(std::memory_order_acquire), kMaxLogPages); }
        };

        SRWLOCK g_lock = SRWLOCK_INIT; // shared: copying into the current log; exclusive: boundaries and loads
        std::atomic<bool> g_active{ false };
        uint32_t g_keep = 0;                               // logs kept (sealed), at most
        std::vector<std::unique_ptr<FrameLog>> g_logs;     // sealed, oldest first
        std::unique_ptr<FrameLog> g_currentLog;            // the frame's (exclusive lock to replace)
        std::atomic<FrameLog*> g_current{ nullptr };
        std::vector<uint64_t> g_watchBits;                 // watched pages (canonical), a bit each
        std::vector<std::pair<uint32_t, const Mask*>> g_masked; // sorted
        std::vector<std::pair<uint32_t, uint32_t>> g_lastRanges;
        std::vector<uint32_t> g_generation;                // Load's pages done, by generation
        uint32_t g_loadGeneration = 0;

        std::mutex g_poolLock;
        std::vector<uint8_t*> g_freeChunks;
        uint64_t g_chunksAllocated = 0;

        std::atomic<uint64_t> g_pagesCopied{ 0 }, g_hostPages{ 0 }, g_misses{ 0 }, g_crossings{ 0 }, g_overflows{ 0 }, g_framesSealed{ 0 };
        std::atomic<uint64_t> g_missCycles{ 0 }; // TSC cycles in Miss, all threads
        uint64_t g_tscStart = 0;                 // and the TSC and QueryPerformanceCounter from the first Start, to convert them
        LARGE_INTEGER g_qpcStart{};
        uint32_t g_lastPages = 0, g_maxPages = 0, g_watchedPages = 0;
        uint64_t g_regionPages[Statistics::kRegionCount] = {};

        Statistics::Region RegionOf(uint32_t page)
        {
            if (page >= kViewA)
                return Statistics::kPhysical;
            if (page >= 0x80000)
                return Statistics::kImage;
            if (page >= 0x70000)
                return Statistics::kStacks;
            return page >= 0x40000 ? Statistics::kHeaps : Statistics::kLow;
        }

        uint8_t* Map()
        {
            return guest::StoreMap();
        }

        bool IsWatched(uint32_t page)
        {
            return !g_watchBits.empty() && (g_watchBits[page >> 6] >> (page & 63) & 1);
        }

        // A page and its alias: the C0000000 view's page of an A0000000 one.
        uint32_t Alias(uint32_t canonical)
        {
            return canonical >= kViewA && canonical < kViewC ? canonical + (kViewC - kViewA) : canonical;
        }

        void SetState(uint32_t canonical, uint8_t state)
        {
            std::atomic_ref<uint8_t>(Map()[canonical]).store(state, std::memory_order_release);
            if (const uint32_t alias = Alias(canonical); alias != canonical)
                std::atomic_ref<uint8_t>(Map()[alias]).store(state, std::memory_order_release);
        }

        uint8_t* TakeChunk()
        {
            std::lock_guard lock(g_poolLock);
            if (!g_freeChunks.empty())
            {
                uint8_t* chunk = g_freeChunks.back();
                g_freeChunks.pop_back();
                return chunk;
            }
            auto* chunk = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(kChunkPages) * kPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (chunk)
                g_chunksAllocated++;
            return chunk;
        }

        // A log's chunks back to the pool. (Exclusive lock held, or the log unreachable.)
        void Retire(FrameLog& log)
        {
            std::lock_guard lock(g_poolLock);
            for (std::atomic<uint8_t*>& chunk : log.chunks)
            {
                if (uint8_t* bytes = chunk.exchange(nullptr))
                    g_freeChunks.push_back(bytes);
            }
            log.count.store(0);
            log.overflow.store(false);
        }

        uint8_t* Slot(FrameLog& log, uint32_t index)
        {
            std::atomic<uint8_t*>& chunk = log.chunks[index / kChunkPages];
            uint8_t* bytes = chunk.load(std::memory_order_acquire);
            if (!bytes)
            {
                static std::mutex lock;
                std::lock_guard guard(lock);
                bytes = chunk.load(std::memory_order_acquire);
                if (!bytes)
                {
                    bytes = TakeChunk();
                    chunk.store(bytes, std::memory_order_release);
                }
            }
            return bytes ? bytes + size_t(index % kChunkPages) * kPageSize : nullptr;
        }

        std::unique_ptr<FrameLog> NewLog(uint64_t frame)
        {
            auto log = std::make_unique<FrameLog>();
            log->frame = frame;
            return log;
        }

        // A watched page's first write in the frame: copied into the frame's log, then quiet. (Shared lock held.)
        void Copy(uint32_t page, bool host)
        {
            const uint32_t canonical = CanonicalPage(page);
            std::atomic_ref<uint8_t> state(Map()[canonical]);
            for (;;)
            {
                uint8_t current = state.load(std::memory_order_acquire);
                if (current == kQuiet)
                    break;
                if (current == kArmed)
                {
                    if (!state.compare_exchange_weak(current, kCopying, std::memory_order_acq_rel))
                        continue;
                    FrameLog& log = *g_current.load(std::memory_order_acquire);
                    const uint32_t index = log.count.fetch_add(1, std::memory_order_acq_rel);
                    uint8_t* slot = index < kMaxLogPages ? Slot(log, index) : nullptr;
                    if (slot)
                    {
                        memcpy(slot, guest::ToHost(canonical << 12), kPageSize);
                        log.pages[index] = canonical;
                        g_pagesCopied.fetch_add(1, std::memory_order_relaxed);
                        if (host)
                            g_hostPages.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        log.overflow.store(true, std::memory_order_relaxed);
                        if (index < kMaxLogPages)
                            log.pages[index] = ~0u; // no memory for it
                    }
                    SetState(canonical, kQuiet);
                    return;
                }
                _mm_pause(); // another thread copies it
            }
            // Copied already (through the other view, perhaps): this view's byte too.
            if (canonical != page)
                std::atomic_ref<uint8_t>(Map()[page]).store(kQuiet, std::memory_order_release);
        }

        // The page census (CensusOnFrame): each frame's first stores to every page, with their writers.
        struct CensusEntry
        {
            uint32_t page = 0;     // canonical
            uint32_t thread = 0;   // the guest thread's start address (0x80000000 | its id for a host entry; 0: a host thread)
            uint32_t function = 0; // the guest function, or for host code its guest caller | 1
            bool host = false;     // host code's write
        };
        std::atomic<bool> g_census{ false };
        std::mutex g_censusLock;
        std::vector<CensusEntry> g_censusFrame;               // the current frame's
        std::vector<std::vector<CensusEntry>> g_censusFrames; // each frame's, from the census's first

        // The first guest function on the host stack (host code's guest caller), with bit 0 set.
        uint32_t GuestCallerOnHostStack()
        {
            void* frames[32];
            const USHORT count = RtlCaptureStackBackTrace(2, 32, frames, nullptr);
            for (USHORT i = 0; i < count; i++)
            {
                if (const uint32_t function = crash::GuestFunctionAt(reinterpret_cast<uintptr_t>(frames[i])))
                    return function | 1;
            }
            return 0;
        }

        void NoteCensus(uint32_t page, bool host, uintptr_t hostPc)
        {
            CensusEntry entry;
            entry.page = CanonicalPage(page);
            entry.host = host;
            if (const kernel::GuestThread* thread = kernel::CurrentThread())
                entry.thread = thread->StartAddress() ? thread->StartAddress() : 0x80000000u | thread->ThreadId();
            entry.function = host || !hostPc ? GuestCallerOnHostStack() : crash::GuestFunctionAt(hostPc);
            std::lock_guard lock(g_censusLock);
            g_censusFrame.push_back(entry);
        }

        void Miss(uint32_t address, uint64_t size, bool host, uintptr_t hostPc)
        {
            const uint64_t start = __rdtsc();
            struct Timer
            {
                uint64_t start;
                ~Timer() { g_missCycles.fetch_add(__rdtsc() - start, std::memory_order_relaxed); }
            } timer{ start };
            g_misses.fetch_add(1, std::memory_order_relaxed);
            const uint32_t first = address >> 12;
            const uint32_t last = uint32_t(std::min<uint64_t>(uint64_t(address) + std::max<uint64_t>(size, 1) - 1, 0xFFFFFFFFull) >> 12);
            if (last != first && !host)
                g_crossings.fetch_add(1, std::memory_order_relaxed);
            AcquireSRWLockShared(&g_lock);
            const bool logging = g_current.load(std::memory_order_acquire) != nullptr;
            const bool census = g_census.load(std::memory_order_relaxed);
            for (uint32_t page = first; page <= last && page >= first; page++)
            {
                if (std::atomic_ref<uint8_t>(Map()[page]).load(std::memory_order_acquire) == kQuiet)
                    continue;
                if (census)
                    NoteCensus(page, host, hostPc);
                if (logging && IsWatched(CanonicalPage(page)))
                    Copy(page, host);
                else
                    std::atomic_ref<uint8_t>(Map()[page]).store(kQuiet, std::memory_order_relaxed); // nothing to keep
            }
            ReleaseSRWLockShared(&g_lock);
        }

        void SetBits(std::vector<uint64_t>& bits, uint32_t first, uint32_t end)
        {
            end = std::min(end, kPageCount);
            for (uint32_t page = first; page < end;)
            {
                if ((page & 63) == 0 && page + 64 <= end)
                {
                    bits[page >> 6] = ~0ull;
                    page += 64;
                    continue;
                }
                bits[page >> 6] |= 1ull << (page & 63);
                page++;
            }
        }

        // The watched pages from now on: armed if newly watched, quiet if no longer. (Exclusive lock held.)
        void SetWatched(const Watched& watched)
        {
            std::vector<std::pair<uint32_t, uint32_t>> ranges = watched.ranges;
            for (const auto& [page, mask] : watched.masked)
                ranges.emplace_back(page, page + 1);
            // Physical pages under their A0000000 address.
            for (auto& [first, end] : ranges)
            {
                if (first >= kViewC && first < kViewEnd)
                {
                    const uint32_t length = end - first;
                    first -= kViewC - kViewA;
                    end = first + length;
                }
            }
            std::sort(ranges.begin(), ranges.end());
            g_masked = watched.masked;
            std::sort(g_masked.begin(), g_masked.end());
            if (ranges == g_lastRanges && !g_watchBits.empty())
                return;
            g_lastRanges = ranges;
            std::vector<uint64_t> bits(kPageCount / 64, 0);
            for (const auto& [first, end] : ranges)
                SetBits(bits, first, end);
            if (g_watchBits.empty())
                g_watchBits.assign(kPageCount / 64, 0);
            uint32_t watchedPages = 0;
            for (uint32_t word = 0; word < kPageCount / 64; word++)
            {
                watchedPages += uint32_t(__builtin_popcountll(bits[word]));
                uint64_t changed = bits[word] ^ g_watchBits[word];
                while (changed)
                {
                    const uint32_t bit = uint32_t(__builtin_ctzll(changed));
                    changed &= changed - 1;
                    const uint32_t page = word * 64 + bit;
                    SetState(page, (bits[word] >> bit & 1) ? kArmed : kQuiet);
                }
            }
            g_watchBits = std::move(bits);
            g_watchedPages = watchedPages;
        }

        const Mask* MaskOf(uint32_t page)
        {
            const auto it = std::lower_bound(g_masked.begin(), g_masked.end(), std::make_pair(page, static_cast<const Mask*>(nullptr)));
            return it != g_masked.end() && it->first == page ? it->second : nullptr;
        }

        // Copies a page, faulting quietly if it's no longer committed (a heap gave it back). No C++ objects here (SEH).
        bool GuardedCopy(void* to, const void* from, size_t size)
        {
            __try
            {
                memcpy(to, from, size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Compares a page with a log's copy of it, as false if it's no longer committed. No C++ objects here (SEH).
        bool GuardedEqual(const void* page, const void* copy, size_t size)
        {
            __try
            {
                return memcmp(page, copy, size) == 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Whether a page already holds a log's copy (its game's bytes, if masked).
        bool Unchanged(uint32_t page, const uint8_t* contents)
        {
            const uint8_t* now = guest::ToHost(page << 12);
            if (GuardedEqual(now, contents, kPageSize))
                return true;
            const Mask* mask = MaskOf(page);
            if (!mask)
                return false;
            alignas(16) uint8_t merged[kPageSize];
            if (!GuardedCopy(merged, now, kPageSize))
                return false;
            for (uint32_t i = 0; i < kPageSize; i++)
            {
                if ((*mask)[i])
                    merged[i] = contents[i];
            }
            return GuardedEqual(now, merged, kPageSize);
        }

        // Writes a page back from a log (its game's bytes, if masked), the GPU's memory watch told first for physical
        // memory (WritePages). (Exclusive lock held: no copy runs.)
        bool WritePage(uint32_t page, const uint8_t* contents)
        {
            const Mask* mask = MaskOf(page);
            const uint32_t address = page << 12;
            if (tracker::Active() && address < tracker::kWatchEnd)
            {
                tracker::WriteUntracked(page, contents, mask);
                return true;
            }
            if (!mask)
                return GuardedCopy(guest::ToHost(address), contents, kPageSize);
            alignas(16) uint8_t merged[kPageSize];
            if (!GuardedCopy(merged, guest::ToHost(address), kPageSize))
                return false;
            for (uint32_t i = 0; i < kPageSize; i++)
            {
                if ((*mask)[i])
                    merged[i] = contents[i];
            }
            return GuardedCopy(guest::ToHost(address), merged, kPageSize);
        }

        // The logs from the oldest kept (sealed, then the current one).
        template <typename F>
        void ForEachLog(F&& function)
        {
            for (const std::unique_ptr<FrameLog>& log : g_logs)
                function(*log);
            if (g_currentLog)
                function(*g_currentLog);
        }
    }

    uint32_t CanonicalPage(uint32_t page)
    {
        return page >= kViewC && page < kViewEnd ? page - (kViewC - kViewA) : page;
    }

    bool Watches(uint32_t page)
    {
        return Active() && page < kPageCount && IsWatched(CanonicalPage(page));
    }

    void Start(uint64_t frame, uint32_t frames, const Watched& watched)
    {
        AcquireSRWLockExclusive(&g_lock);
        g_keep = std::max<uint32_t>(frames, 1);
        g_logs.clear();
        g_currentLog = NewLog(frame);
        g_current.store(g_currentLog.get(), std::memory_order_release);
        g_watchBits.clear();
        g_lastRanges.clear();
        g_masked.clear();
        SetWatched(watched);
        g_active.store(true, std::memory_order_release);
        if (!g_tscStart)
        {
            QueryPerformanceCounter(&g_qpcStart);
            g_tscStart = __rdtsc();
        }
        ReleaseSRWLockExclusive(&g_lock);
    }

    void Stop()
    {
        AcquireSRWLockExclusive(&g_lock);
        g_active.store(false, std::memory_order_release);
        g_current.store(nullptr, std::memory_order_release);
        // Nothing watched: every byte quiet.
        for (uint32_t word = 0; word < g_watchBits.size(); word++)
        {
            for (uint64_t bits = g_watchBits[word]; bits; bits &= bits - 1)
                SetState(word * 64 + uint32_t(__builtin_ctzll(bits)), kQuiet);
        }
        g_watchBits.clear();
        g_lastRanges.clear();
        g_masked.clear();
        for (std::unique_ptr<FrameLog>& log : g_logs)
            Retire(*log);
        g_logs.clear();
        if (g_currentLog)
            Retire(*g_currentLog);
        g_currentLog.reset();
        ReleaseSRWLockExclusive(&g_lock);
    }

    bool Active()
    {
        return g_active.load(std::memory_order_acquire);
    }

    void Boundary(uint64_t frame, const Watched& watched)
    {
        if (!Active())
            return;
        AcquireSRWLockExclusive(&g_lock);
        // The frame's log is sealed, and its pages armed again (those still watched: SetWatched sorts out the rest).
        std::unique_ptr<FrameLog> sealed = std::move(g_currentLog);
        const uint32_t count = sealed->Count();
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t page = sealed->pages[i];
            if (page == ~0u)
                continue;
            g_regionPages[RegionOf(page)]++;
            if (IsWatched(page))
                SetState(page, kArmed);
        }
        g_lastPages = count;
        g_maxPages = std::max(g_maxPages, count);
        if (sealed->overflow.load())
            g_overflows.fetch_add(1, std::memory_order_relaxed);
        g_logs.push_back(std::move(sealed));
        g_framesSealed.fetch_add(1, std::memory_order_relaxed);
        while (g_logs.size() > g_keep)
        {
            Retire(*g_logs.front());
            g_logs.erase(g_logs.begin());
        }
        SetWatched(watched);
        g_currentLog = NewLog(frame);
        g_current.store(g_currentLog.get(), std::memory_order_release);
        ReleaseSRWLockExclusive(&g_lock);
    }

    bool CanLoad(uint64_t frame)
    {
        AcquireSRWLockShared(&g_lock);
        bool can = Active() && g_currentLog && frame <= g_currentLog->frame;
        if (can && frame < g_currentLog->frame)
        {
            // Every frame's log from `frame` on, complete.
            uint64_t expected = frame;
            bool found = false;
            for (const std::unique_ptr<FrameLog>& log : g_logs)
            {
                if (log->frame < frame)
                    continue;
                found = found || log->frame == frame;
                can = can && log->frame == expected && !log->overflow.load();
                expected = log->frame + 1;
            }
            can = can && found && expected == g_currentLog->frame;
        }
        ReleaseSRWLockShared(&g_lock);
        return can;
    }

    size_t Load(uint64_t frame)
    {
        AcquireSRWLockExclusive(&g_lock);
        if (g_generation.empty())
            g_generation.assign(kPageCount, 0);
        if (++g_loadGeneration == 0)
        {
            std::fill(g_generation.begin(), g_generation.end(), 0);
            g_loadGeneration = 1;
        }
        // Oldest first: a page's copy in the oldest log from `frame` on is how it was at `frame`'s boundary. Pages that
        // hold it already are left alone.
        std::vector<std::pair<uint32_t, const uint8_t*>> writes;
        ForEachLog([&](FrameLog& log) {
            if (log.frame < frame)
                return;
            const uint32_t count = log.Count();
            for (uint32_t i = 0; i < count; i++)
            {
                const uint32_t page = log.pages[i];
                if (page == ~0u || g_generation[page] == g_loadGeneration)
                    continue;
                g_generation[page] = g_loadGeneration;
                const uint8_t* contents = log.chunks[i / kChunkPages].load(std::memory_order_acquire) + size_t(i % kChunkPages) * kPageSize;
                if (!Unchanged(page, contents))
                    writes.emplace_back(page, contents);
            }
        });
        // Physical memory's pages the GPU's memory watch protects are made writable a run of pages at a time.
        std::sort(writes.begin(), writes.end());
        for (size_t i = 0; i < writes.size();)
        {
            const uint32_t first = writes[i].first;
            size_t j = i + 1;
            while (j < writes.size() && writes[j].first == writes[j - 1].first + 1)
                j++;
            if (first >= kViewA && first < kViewEnd)
                gpu::watch::PrepareHostWrite(first << 12, uint32_t(j - i) * kPageSize);
            i = j;
        }
        size_t written = 0;
        for (const auto& [page, contents] : writes)
            written += WritePage(page, contents);
        // The current frame's pages, copied since its boundary, are armed again; the logs from `frame` on go.
        if (g_currentLog)
        {
            const uint32_t count = g_currentLog->Count();
            for (uint32_t i = 0; i < count; i++)
            {
                const uint32_t page = g_currentLog->pages[i];
                if (page != ~0u && IsWatched(page))
                    SetState(page, kArmed);
            }
            Retire(*g_currentLog);
        }
        while (!g_logs.empty() && g_logs.back()->frame >= frame)
        {
            Retire(*g_logs.back());
            g_logs.pop_back();
        }
        g_currentLog = NewLog(frame);
        g_current.store(g_currentLog.get(), std::memory_order_release);
        ReleaseSRWLockExclusive(&g_lock);
        return written;
    }

    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> FramePages()
    {
        std::vector<std::pair<uint64_t, std::vector<uint32_t>>> frames;
        AcquireSRWLockShared(&g_lock);
        for (const std::unique_ptr<FrameLog>& log : g_logs)
        {
            auto& [frame, pages] = frames.emplace_back();
            frame = log->frame;
            const uint32_t count = log->Count();
            pages.assign(log->pages.get(), log->pages.get() + count);
        }
        ReleaseSRWLockShared(&g_lock);
        return frames;
    }

    const uint8_t* PageAt(uint32_t page, uint64_t frame)
    {
        page = CanonicalPage(page);
        const uint8_t* found = nullptr;
        AcquireSRWLockShared(&g_lock);
        // The sealed logs only: the current one may be taking copies now.
        for (const std::unique_ptr<FrameLog>& log : g_logs)
        {
            if (found || log->frame < frame)
                continue;
            const uint32_t count = log->Count();
            for (uint32_t i = 0; i < count; i++)
            {
                if (log->pages[i] == page)
                {
                    found = log->chunks[i / kChunkPages].load(std::memory_order_acquire) + size_t(i % kChunkPages) * kPageSize;
                    break;
                }
            }
        }
        ReleaseSRWLockShared(&g_lock);
        return found;
    }

    Statistics GetStatistics()
    {
        Statistics statistics;
        statistics.frames = g_framesSealed.load();
        statistics.pages = g_pagesCopied.load();
        statistics.hostPages = g_hostPages.load();
        statistics.misses = g_misses.load();
        statistics.crossings = g_crossings.load();
        statistics.overflows = g_overflows.load();
        AcquireSRWLockShared(&g_lock);
        std::copy(std::begin(g_regionPages), std::end(g_regionPages), statistics.regionPages);
        ReleaseSRWLockShared(&g_lock);
        if (g_tscStart)
        {
            LARGE_INTEGER now, frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const double elapsedUs = double(now.QuadPart - g_qpcStart.QuadPart) * 1e6 / double(frequency.QuadPart);
            const double cyclesPerUs = elapsedUs > 0 ? double(__rdtsc() - g_tscStart) / elapsedUs : 0;
            statistics.copyMicroseconds = cyclesPerUs > 0 ? double(g_missCycles.load()) / cyclesPerUs : 0;
        }
        AcquireSRWLockShared(&g_lock);
        statistics.lastPages = g_lastPages;
        statistics.maxPages = g_maxPages;
        statistics.watchedPages = g_watchedPages;
        {
            std::lock_guard lock(g_poolLock);
            statistics.bytesHeld = (g_chunksAllocated - g_freeChunks.size()) * uint64_t(kChunkPages) * kPageSize;
        }
        ReleaseSRWLockShared(&g_lock);
        return statistics;
    }

    std::string StatisticsText()
    {
        const Statistics s = GetStatistics();
        char text[320];
        snprintf(text, sizeof(text),
            "%llu frames logged, %.1f pages a frame (the last %u, at most %u), %llu of them before host writes; %llu store map misses (%llu "
            "across two pages); %u pages watched; the logs hold %.1f MB; %llu frames overflowed",
            (unsigned long long)s.frames, s.frames ? double(s.pages) / double(s.frames) : 0.0, s.lastPages, s.maxPages,
            (unsigned long long)s.hostPages, (unsigned long long)s.misses, (unsigned long long)s.crossings, s.watchedPages,
            s.bytesHeld / (1024.0 * 1024), (unsigned long long)s.overflows);
        return text;
    }

    namespace
    {
        struct CensusSettings
        {
            uint64_t first = 0, count = 0;
            CensusSettings()
            {
                if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_PAGE_CENSUS"))
                {
                    char* end = nullptr;
                    first = std::max<uint64_t>(strtoull(setting, &end, 10), 1);
                    count = end && *end == ',' ? std::max<uint64_t>(strtoull(end + 1, nullptr, 10), 1) : 120;
                }
            }
        };

        // Every guest page armed (with the page log's lock held), or every page but those the page log watches quiet.
        void ArmEverything(bool arm)
        {
            AcquireSRWLockExclusive(&g_lock);
            for (uint32_t page = 0; page < kPageCount; page++)
            {
                const bool watched = IsWatched(CanonicalPage(page)) && g_current.load(std::memory_order_relaxed);
                if (arm || !watched)
                    std::atomic_ref<uint8_t>(Map()[page]).store(arm || watched ? kArmed : kQuiet, std::memory_order_relaxed);
            }
            ReleaseSRWLockExclusive(&g_lock);
        }

        void CensusReport(const CensusSettings& settings)
        {
            const GameState* game = GetGameState();
            // The game's memory, as the savestates have it.
            std::vector<bool> watched(kPageCount, false);
            if (game && game->savedMemory)
            {
                const GameState::SavedMemory memory = game->savedMemory();
                for (const auto& [start, end] : memory.ranges)
                {
                    for (uint64_t page = start >> 12; page < (uint64_t(end) + 0xFFF) >> 12; page++)
                        watched[CanonicalPage(uint32_t(page))] = true;
                }
                for (const auto& [page, mask] : memory.partial)
                    watched[page >> 12] = true;
            }
            const auto writerKind = [&](const CensusEntry& entry) -> int {
                if (!entry.thread)
                    return 3; // a host thread
                const bool gameThread = game && game->isGameThread && game->isGameThread(entry.thread);
                if (!gameThread)
                    return 2; // another guest thread
                const bool library = game->isLibraryFunction && game->isLibraryFunction(entry.function & ~1u);
                return library ? 1 : 0;
            };
            static const char* const kKinds[] = { "the game's code on its threads", "libraries' code on the game's threads", "other threads",
                "host threads" };
            // Per frame: pages by region, held by the game's memory or not, and by their first writer's kind.
            const size_t frames = g_censusFrames.size();
            uint64_t totals[Statistics::kRegionCount][2][4] = {};
            std::map<uint32_t, std::set<std::tuple<uint32_t, uint32_t, bool>>> writers; // page -> (thread, function, host)
            for (const std::vector<CensusEntry>& frame : g_censusFrames)
            {
                std::set<uint32_t> seen;
                for (const CensusEntry& entry : frame)
                {
                    writers[entry.page].insert({ entry.thread, entry.function, entry.host });
                    if (!seen.insert(entry.page).second)
                        continue;
                    totals[RegionOf(entry.page)][watched[entry.page] ? 1 : 0][writerKind(entry)]++;
                }
            }
            const std::string path = config::DumpPath("page_census.txt");
            FILE* file = fopen(path.c_str(), "w");
            std::string summary;
            if (file)
            {
                fprintf(file, "Page census: frames %llu-%llu (%zu): each frame's pages written (each page's first write in the frame), by region, "
                              "whether the game's memory (GameState::savedMemory) holds them, and their first writer, per frame:\n",
                    (unsigned long long)settings.first, (unsigned long long)(settings.first + frames - 1), frames);
            }
            for (int region = 0; region < Statistics::kRegionCount; region++)
            {
                for (int held = 1; held >= 0; held--)
                {
                    double all = 0;
                    std::string kinds;
                    for (int kind = 0; kind < 4; kind++)
                    {
                        const double mean = frames ? double(totals[region][held][kind]) / double(frames) : 0.0;
                        all += mean;
                        if (mean > 0)
                        {
                            char part[96];
                            snprintf(part, sizeof(part), "%s%s %.1f", kinds.empty() ? "" : ", ", kKinds[kind], mean);
                            kinds += part;
                        }
                    }
                    if (all == 0)
                        continue;
                    char line[512];
                    snprintf(line, sizeof(line), "%s, %s: %.1f (%s)", kRegionNames[region], held ? "the game's memory" : "others'", all, kinds.c_str());
                    if (file)
                        fprintf(file, "  %s\n", line);
                    summary += (summary.empty() ? "" : "; ") + std::string(line);
                }
            }
            if (file)
            {
                fprintf(file, "\nPages written, as runs of pages with the same writers (thread, function; h: host code, called from):\n");
                for (auto it = writers.begin(); it != writers.end();)
                {
                    auto end = std::next(it);
                    uint32_t last = it->first;
                    while (end != writers.end() && end->first == last + 1 && end->second == it->second)
                        last = (end++)->first;
                    fprintf(file, "  %08X-%08X (%u pages, %s):", it->first << 12, ((last + 1) << 12) - 1, last - it->first + 1,
                        watched[it->first] ? "the game's" : "others'");
                    size_t listed = 0;
                    for (const auto& [thread, function, host] : it->second)
                    {
                        if (listed++ == 12)
                        {
                            fprintf(file, " ...");
                            break;
                        }
                        fprintf(file, " [%08X %s%08X]", thread, host ? "h " : "", function & ~1u);
                    }
                    fprintf(file, "\n");
                    it = end;
                }
                fclose(file);
            }
            LOG_INFO("Page census: %zu frames; pages written a frame by region: %s; details in %s", frames, summary.c_str(), path.c_str());
        }
    }

    void CensusOnFrame(uint64_t frame)
    {
        static const CensusSettings settings;
        static bool done = false;
        if (!settings.count || done || frame < settings.first)
            return;
        if (frame == settings.first || !g_census.load(std::memory_order_relaxed))
        {
            ArmEverything(true);
            g_census.store(true, std::memory_order_release);
            LOG_INFO("Page census: from frame %llu, for %llu frames", (unsigned long long)frame, (unsigned long long)settings.count);
            return;
        }
        // The frame's pages, and armed again for the next.
        std::vector<CensusEntry> entries;
        {
            std::lock_guard lock(g_censusLock);
            entries.swap(g_censusFrame);
        }
        AcquireSRWLockExclusive(&g_lock);
        for (const CensusEntry& entry : entries)
            SetState(entry.page, kArmed);
        ReleaseSRWLockExclusive(&g_lock);
        g_censusFrames.push_back(std::move(entries));
        if (g_censusFrames.size() >= settings.count)
        {
            g_census.store(false, std::memory_order_release);
            done = true;
            ArmEverything(false);
            CensusReport(settings);
            std::vector<std::vector<CensusEntry>>().swap(g_censusFrames);
        }
    }
}

// The store map's misses (guest_memory.h; the generated code's through PPCStoreMiss, store_miss.cpp).
namespace guest
{
    void StoreMiss(uint32_t address, uint32_t length)
    {
        netplay::pagelog::Miss(address, length, true, 0);
    }

    void GuestStoreMiss(uint32_t address, uint32_t size, uintptr_t returnAddress)
    {
        netplay::pagelog::Miss(address, size, false, returnAddress);
    }
}
