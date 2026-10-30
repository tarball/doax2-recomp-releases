#include "gpu/memory_watch.h"

#include "crash_handler.h"
#include "guest_abi.h"
#include "guest_memory.h"
#include "kernel/threads.h"
#include "log.h"
#include "util/profiler.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <windows.h>

namespace gpu::watch
{
    namespace
    {
        // Physical views (A0000000+) are the only guest addresses that alias physical memory.
        constexpr uint32_t kViewsStart = 0xA0000000;

        std::atomic<uint64_t> g_valid[kPageCount / 64];
        std::atomic<uint32_t> g_versions[kPageCount];
        thread_local FaultStatistics t_faults; // ThreadFaults
        // Serializes protection changes with validity changes, so a page is protected iff valid.
        // Never held while writing guest memory, and never while taking guest::HeapLock().
        std::mutex g_lock;

        // VERIFY_UPLOADS=1 (diagnostics): a copy of every page as uploaded. Pages believed
        // valid are compared with guest memory when requested, so writes the watch missed are
        // reported (and the pages uploaded again).
        uint8_t* g_shadow = nullptr;
        std::atomic<uint32_t> g_missedReports{ 0 };

        // DEBUG_WATCH_WRITERS=<low>-<high> (physical, hex): records which guest code first writes uploaded
        // pages in that range (the faulting function and its callers), for DumpWriters (diagnostics).
        struct Writer
        {
            uint32_t function, callers[3];
            uint64_t count;
        };
        constexpr uint32_t kMaxWriters = 64;
        Writer g_writers[kMaxWriters];
        uint32_t g_writerCount = 0;
        uint32_t g_writersLow = 1, g_writersHigh = 0;

        void RecordWriter(EXCEPTION_POINTERS* info)
        {
            Writer writer{};
            writer.function = crash::GuestFunctionAt(uintptr_t(info->ContextRecord->Rip));
            // The guest call chain from the thread's saved stack pointer (racy but harmless: diagnostics).
            if (const kernel::GuestThread* thread = kernel::CurrentThread(); thread && thread->Context())
            {
                uint32_t frame = thread->Context()->r1.u32;
                for (uint32_t i = 0; i < 3; i++)
                {
                    if (frame < 0x10000 || frame >= 0x7F000000 || (frame & 3))
                        break;
                    const uint32_t previous = guest::LoadU32(frame);
                    if (previous <= frame || previous - frame > 0x100000)
                        break;
                    writer.callers[i] = guest::LoadU32(previous - 8);
                    frame = previous;
                }
            }
            for (uint32_t i = 0; i < g_writerCount; i++)
            {
                Writer& known = g_writers[i];
                if (known.function == writer.function && memcmp(known.callers, writer.callers, sizeof(writer.callers)) == 0)
                {
                    known.count++;
                    return;
                }
            }
            if (g_writerCount < kMaxWriters)
            {
                writer.count = 1;
                g_writers[g_writerCount++] = writer;
            }
        }

        bool IsValid(uint32_t page)
        {
            return (g_valid[page >> 6].load(std::memory_order_acquire) >> (page & 63)) & 1;
        }

        void SetValid(uint32_t page)
        {
            g_valid[page >> 6].fetch_or(1ull << (page & 63), std::memory_order_acq_rel);
        }

        // Returns whether the page was valid.
        bool ClearValid(uint32_t page)
        {
            const uint64_t bit = 1ull << (page & 63);
            return (g_valid[page >> 6].fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
        }

        void SetProtection(uint32_t guestAddress, uint32_t size, DWORD protection)
        {
            DWORD old;
            VirtualProtect(guest::ToHost(guestAddress), size, protection, &old);
            profile::Count(profile::Counter::kProtectCalls);
        }

        // Changes the protection of guest pages holding physical memory, including the other alias of
        // pages in the shared A0000000/C0000000 views.
        void SetPhysicalProtection(uint32_t guestAddress, uint32_t size, DWORD protection)
        {
            SetProtection(guestAddress, size, protection);
            if (guestAddress >= guest::kSharedViewA && guestAddress < guest::kViewE)
                SetProtection(guestAddress ^ (guest::kSharedViewA ^ guest::kSharedViewC), size, protection);
        }

        void Unprotect(uint32_t guestAddress, uint32_t size)
        {
            SetPhysicalProtection(guestAddress, size, PAGE_READWRITE);
        }

        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            const EXCEPTION_RECORD* record = info->ExceptionRecord;
            if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1)
                return EXCEPTION_CONTINUE_SEARCH;
            const uintptr_t host = record->ExceptionInformation[1];
            const uintptr_t base = reinterpret_cast<uintptr_t>(guest::g_base);
            if (host < base + kViewsStart || host >= base + guest::kAddressSpaceSize)
                return EXCEPTION_CONTINUE_SEARCH;

            const uint32_t guestAddress = uint32_t(host - base) & ~(kPageSize - 1);
            const uint32_t physical = guest::PhysicalAddress(guestAddress);
            if (physical >= kPhysicalSize)
                return EXCEPTION_CONTINUE_SEARCH;
            const uint32_t page = physical >> kPageShift;

            LARGE_INTEGER start;
            QueryPerformanceCounter(&start);
            struct Timer
            {
                LARGE_INTEGER start;
                ~Timer()
                {
                    LARGE_INTEGER end;
                    QueryPerformanceCounter(&end);
                    t_faults.faults++;
                    t_faults.handlerTicks += end.QuadPart - start.QuadPart;
                }
            } timer{ start };
            std::lock_guard lock(g_lock);
            if (!ClearValid(page))
            {
                // Not ours, unless another thread just handled the same page.
                MEMORY_BASIC_INFORMATION memory;
                if (VirtualQuery(reinterpret_cast<void*>(host), &memory, sizeof(memory)) && memory.Protect == PAGE_READWRITE)
                    return EXCEPTION_CONTINUE_EXECUTION;
                return EXCEPTION_CONTINUE_SEARCH;
            }
            g_versions[page].fetch_add(1, std::memory_order_relaxed);
            Unprotect(guestAddress, kPageSize);
            profile::Count(profile::Counter::kWatchFaults);
            if (physical >= g_writersLow && physical <= g_writersHigh)
                RecordWriter(info);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    void Install()
    {
        // First in line: this fault is routine and must be handled before the crash reporter sees it.
        AddVectoredExceptionHandler(1, OnException);
        if (const char* verify = std::getenv(GAME_PREFIX "_VERIFY_UPLOADS"); verify && std::atoi(verify) != 0)
            g_shadow = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPhysicalSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (const char* writers = std::getenv(GAME_PREFIX "_DEBUG_WATCH_WRITERS"))
        {
            char* end = nullptr;
            g_writersLow = uint32_t(strtoul(writers, &end, 16));
            g_writersHigh = *end == '-' ? uint32_t(strtoul(end + 1, nullptr, 16)) : g_writersLow;
        }
    }

    void DumpWriters()
    {
        if (g_writersLow > g_writersHigh)
            return;
        std::lock_guard lock(g_lock);
        for (uint32_t i = 0; i < g_writerCount; i++)
        {
            const Writer& w = g_writers[i];
            LOG_INFO("GPU watch: %llu first writes to %08X-%08X by sub_%08X (called from %08X <- %08X <- %08X)", (unsigned long long)w.count,
                g_writersLow, g_writersHigh, w.function, w.callers[0], w.callers[1], w.callers[2]);
        }
    }

    bool Verifying()
    {
        return g_shadow != nullptr;
    }

    void RecordUpload(uint32_t firstPage, uint32_t pageCount, const void* data)
    {
        if (g_shadow)
            memcpy(g_shadow + (size_t(firstPage) << kPageShift), data, size_t(pageCount) << kPageShift);
    }

    const uint8_t* Uploaded(uint32_t physical)
    {
        return g_shadow ? g_shadow + physical : nullptr;
    }

    namespace
    {
        // Verification: false (after invalidating the page) if guest memory differs from the copy uploaded.
        bool VerifyPage(uint32_t page)
        {
            const uint32_t guestAddress = guest::PhysicalToGuest(page << kPageShift);
            const uint8_t* current = guest::ToHost(guestAddress);
            const uint8_t* uploaded = g_shadow + (size_t(page) << kPageShift);
            if (memcmp(current, uploaded, kPageSize) == 0)
                return true;
            uint32_t first = 0, last = kPageSize - 1;
            while (current[first] == uploaded[first])
                first++;
            while (current[last] == uploaded[last])
                last--;
            if (g_missedReports.fetch_add(1, std::memory_order_relaxed) < 200)
            {
                MEMORY_BASIC_INFORMATION memory{};
                VirtualQuery(current, &memory, sizeof(memory));
                LOG_WARN("GPU watch: missed write to physical %08X (guest %08X, bytes %03X-%03X, protection %lX)", page << kPageShift, guestAddress,
                    first, last, memory.Protect);
            }
            std::lock_guard lock(g_lock);
            if (ClearValid(page))
            {
                g_versions[page].fetch_add(1, std::memory_order_relaxed);
                Unprotect(guestAddress, kPageSize);
            }
            return false;
        }
    }

    void Validate(uint32_t physical, uint32_t length, UploadCallback upload, void* context)
    {
        if (!length || physical >= kPhysicalSize)
            return;
        const uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(physical) + length, kPhysicalSize));
        const uint32_t firstPage = physical >> kPageShift, lastPage = (end - 1) >> kPageShift;

        for (uint32_t page = firstPage; page <= lastPage;)
        {
            if (IsValid(page) && (!g_shadow || VerifyPage(page)))
            {
                page++;
                continue;
            }
            // A run of invalid pages that are contiguous in one view (translated before locking:
            // PhysicalToGuest takes the heap lock).
            const uint32_t runStart = page;
            const uint32_t guestStart = guest::PhysicalToGuest(page << kPageShift);
            uint32_t runEnd = page + 1;
            while (runEnd <= lastPage && !IsValid(runEnd) && guest::PhysicalToGuest(runEnd << kPageShift) == guestStart + ((runEnd - runStart) << kPageShift))
                runEnd++;

            {
                std::lock_guard lock(g_lock);
                for (uint32_t p = runStart; p < runEnd; p++)
                    SetValid(p);
                SetPhysicalProtection(guestStart, (runEnd - runStart) << kPageShift, PAGE_READONLY);
            }
            upload(context, runStart, runEnd - runStart, guestStart);
            page = runEnd;
        }
    }

    void NoteGpuWrite(uint32_t physical, uint32_t length)
    {
        if (!length || physical >= kPhysicalSize)
            return;
        const uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(physical) + length, kPhysicalSize));
        for (uint32_t page = physical >> kPageShift; page <= (end - 1) >> kPageShift; page++)
            g_versions[page].fetch_add(1, std::memory_order_relaxed);
    }

    void PrepareHostWrite(uint32_t guestAddress, uint32_t length)
    {
        if (!length || guestAddress < kViewsStart)
            return;
        const uint32_t start = guestAddress & ~(kPageSize - 1);
        const uint64_t end = uint64_t(guestAddress) + length;
        std::lock_guard lock(g_lock);
        // Runs of pages that were valid are unprotected with a call each (a savestate's load writes hundreds).
        uint64_t runStart = 0, runEnd = 0;
        const auto flush = [&]() {
            if (runEnd > runStart)
                Unprotect(uint32_t(runStart), uint32_t(runEnd - runStart));
            runStart = runEnd = 0;
        };
        for (uint64_t address = start; address < end; address += kPageSize)
        {
            const uint32_t physical = guest::PhysicalAddress(uint32_t(address));
            if (physical >= kPhysicalSize)
            {
                flush();
                continue;
            }
            const uint32_t page = physical >> kPageShift;
            if (!ClearValid(page))
            {
                flush();
                continue;
            }
            g_versions[page].fetch_add(1, std::memory_order_relaxed);
            if (runEnd != address)
            {
                flush();
                runStart = address;
            }
            runEnd = address + kPageSize;
        }
        flush();
    }

    void InvalidatePhysical(uint32_t physical, uint32_t length)
    {
        if (!length || physical >= kPhysicalSize)
            return;
        const uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(physical) + length, kPhysicalSize));
        std::lock_guard lock(g_lock);
        for (uint32_t page = physical >> kPageShift; page <= (end - 1) >> kPageShift; page++)
        {
            if (!ClearValid(page))
                continue;
            g_versions[page].fetch_add(1, std::memory_order_relaxed);
            Unprotect(guest::kSharedViewA + (page << kPageShift), kPageSize);
        }
    }

    uint64_t RangeVersion(uint32_t physical, uint32_t length)
    {
        if (!length || physical >= kPhysicalSize)
            return 0;
        const uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(physical) + length, kPhysicalSize));
        uint64_t sum = 0;
        for (uint32_t page = physical >> kPageShift; page <= (end - 1) >> kPageShift; page++)
            sum += g_versions[page].load(std::memory_order_relaxed);
        return sum;
    }

    uint32_t PageVersion(uint32_t page)
    {
        return page < kPageCount ? g_versions[page].load(std::memory_order_relaxed) : 0;
    }

    FaultStatistics ThreadFaults()
    {
        return t_faults;
    }
}
