#include "netplay/write_tracker.h"

#include "crash_handler.h"
#include "guest_memory.h"
#include "kernel/threads.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <windows.h>

namespace netplay::tracker
{
    namespace
    {
        constexpr uint32_t kPageCount = kWatchEnd / kPageSize;
        constexpr uint32_t kKThreadFiber = 0x164; // XAPI's current fiber in the KTHREAD
        constexpr size_t kMaxWrites = size_t(1) << 20;
        constexpr uint32_t kStepWindow = 64;     // bytes compared around a single-stepped write
        constexpr size_t kMaxByteWriters = 8192; // (page, writer) pairs byte tracking records
        constexpr DWORD kTrapFlag = 0x100;

        std::vector<Write> g_writes;      // the current frame's first writes (preallocated: appended in the fault handler)
        size_t g_writeCount = 0;
        size_t g_resampled = 0;           // g_writes up to here are protected again (Resample)
        uint64_t g_lostWrites = 0;
        std::vector<uint8_t> g_protected; // per page: write-protected by the tracker and not written since
        std::vector<uint8_t> g_byBytes;   // per page: tracked by bytes (kept protected; each write single-stepped)
        std::vector<uint32_t> g_bytePages; // those pages
        std::vector<uint16_t> g_stepping; // per page: steps in progress writing it (it's protected again when none are)
        std::vector<ByteWrites> g_byteWrites; // preallocated (appended in the fault handler)
        uint64_t g_steps = 0, g_lostSteps = 0;
        BeforeWrite g_beforeWrite = nullptr;
        // The pages of host writes under way (PrepareHostWrite to FinishHostWrite), [first, last]: the host kernel writes
        // them, and a protected page fails its write rather than faulting, so they're never protected meanwhile.
        std::vector<std::pair<uint32_t, uint32_t>> g_hostWrites;
        std::mutex g_lock;                // the above, and protection changes
        std::atomic<bool> g_active{ false };

        bool InHostWrite(uint32_t page)
        {
            return std::any_of(g_hostWrites.begin(), g_hostWrites.end(),
                [page](const std::pair<uint32_t, uint32_t>& range) { return page >= range.first && page <= range.second; });
        }

        // Write-protects [first, end) but the pages of host writes under way, returning whether it protected them all.
        // (g_lock held)
        bool ProtectRange(uint32_t first, uint32_t end)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(guest::g_base);
            bool all = true;
            for (uint32_t page = first; page < end;)
            {
                if (InHostWrite(page))
                {
                    all = false;
                    page++;
                    continue;
                }
                uint32_t last = page + 1;
                while (last < end && !InHostWrite(last))
                    last++;
                DWORD old;
                if (VirtualProtect(reinterpret_cast<void*>(base + uintptr_t(page) * kPageSize), size_t(last - page) * kPageSize, PAGE_READONLY, &old))
                    std::fill(g_protected.begin() + ptrdiff_t(page), g_protected.begin() + ptrdiff_t(last), uint8_t(1));
                page = last;
            }
            return all;
        }

        // A write to a page tracked by bytes, being single-stepped on this thread.
        struct Step
        {
            bool active = false;
            uint32_t page = 0, offset = 0, length = 0;
            Writer writer;
            uint8_t before[kStepWindow];
            uint32_t extraPages[4]; // other such pages the stepped instruction faulted on, writable until it's done
            uint32_t extraCount = 0;
        };
        thread_local Step t_step;

        // The first guest function on the host stack above `skip` frames (runtime code's guest caller), with bit 0 set.
        uint32_t GuestCallerOnHostStack(unsigned skip)
        {
            void* frames[24];
            const USHORT count = RtlCaptureStackBackTrace(skip, 24, frames, nullptr);
            for (USHORT i = 0; i < count; i++)
            {
                if (const uint32_t function = crash::GuestFunctionAt(reinterpret_cast<uintptr_t>(frames[i])))
                    return function | 1;
            }
            return 0;
        }

        Writer CurrentWriter(uintptr_t hostPc)
        {
            Writer writer;
            if (const kernel::GuestThread* thread = kernel::CurrentThread())
            {
                writer.thread = thread->StartAddress() ? thread->StartAddress() : 0x80000000u | thread->ThreadId();
                if (const uint32_t kthread = thread->KThread())
                    writer.fiber = guest::LoadU32(kthread + kKThreadFiber);
            }
            writer.function = crash::GuestFunctionAt(hostPc);
            if (!writer.function)
                writer.function = GuestCallerOnHostStack(2);
            return writer;
        }

        void ProtectPage(uint32_t page, DWORD protection)
        {
            DWORD old;
            VirtualProtect(guest::ToHost(page * kPageSize), kPageSize, protection, &old);
        }

        bool ByBytes(uint32_t page)
        {
            return page < g_byBytes.size() && g_byBytes[page];
        }

        // Records the page's first write and makes it writable. (g_lock held)
        void Unprotect(uint32_t page, const Writer& writer)
        {
            if (g_beforeWrite)
                g_beforeWrite(page);
            g_protected[page] = 0;
            ProtectPage(page, PAGE_READWRITE);
            if (g_writeCount < g_writes.size())
                g_writes[g_writeCount++] = Write{ page, writer };
            else
                g_lostWrites++;
        }

        // A page's record of one writer's bytes. (g_lock held)
        ByteWrites* FindByteWrites(uint32_t page, const Writer& writer)
        {
            for (ByteWrites& entry : g_byteWrites)
            {
                if (entry.page == page && entry.writer == writer)
                    return &entry;
            }
            if (g_byteWrites.size() == g_byteWrites.capacity())
                return nullptr;
            ByteWrites& entry = g_byteWrites.emplace_back();
            entry.page = page;
            entry.writer = writer;
            return &entry;
        }

        // A step stops writing a page: protected again once no step writes it. (g_lock held)
        void EndStepOn(uint32_t page)
        {
            if (g_stepping[page] && --g_stepping[page] == 0 && ByBytes(page))
                ProtectPage(page, PAGE_READONLY);
        }

        // After the single-stepped write: the bytes it changed, and the page protected again.
        void FinishStep()
        {
            Step& step = t_step;
            step.active = false;
            std::lock_guard lock(g_lock);
            const uint8_t* now = guest::ToHost(step.page * kPageSize) + step.offset;
            ByteWrites* entry = nullptr;
            for (uint32_t i = 0; i < step.length; i++)
            {
                if (now[i] == step.before[i])
                    continue;
                if (!entry && !(entry = FindByteWrites(step.page, step.writer)))
                {
                    g_lostSteps++;
                    break;
                }
                entry->bytes.set(step.offset + i);
            }
            EndStepOn(step.page);
            for (uint32_t i = 0; i < step.extraCount; i++)
                EndStepOn(step.extraPages[i]);
            step.extraCount = 0;
        }

        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            const EXCEPTION_RECORD* record = info->ExceptionRecord;
            if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_step.active)
            {
                FinishStep();
                info->ContextRecord->EFlags &= ~kTrapFlag;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1)
                return EXCEPTION_CONTINUE_SEARCH;
            const uintptr_t host = record->ExceptionInformation[1];
            const uintptr_t base = reinterpret_cast<uintptr_t>(guest::g_base);
            if (host < base + kWatchStart || host >= base + kWatchEnd)
                return EXCEPTION_CONTINUE_SEARCH;
            const uint32_t address = uint32_t(host - base);
            const uint32_t page = address / kPageSize;
            const Writer writer = CurrentWriter(uintptr_t(info->ContextRecord->Rip));
            std::lock_guard lock(g_lock);
            if (ByBytes(page))
            {
                Step& step = t_step;
                if (step.active)
                {
                    // A page this step already writes, protected meanwhile (a frame boundary): writable again.
                    if (page == step.page || std::find(step.extraPages, step.extraPages + step.extraCount, page) != step.extraPages + step.extraCount)
                    {
                        ProtectPage(page, PAGE_READWRITE);
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                    // The instruction being stepped writes a second such page: let it, without its bytes.
                    if (step.extraCount == 4)
                    {
                        fprintf(stderr, "*** write tracker: %08X faulted while stepping a write to %08X (and %08X %08X %08X %08X)\n", address,
                            step.page * kPageSize, step.extraPages[0] * kPageSize, step.extraPages[1] * kPageSize, step.extraPages[2] * kPageSize,
                            step.extraPages[3] * kPageSize);
                        return EXCEPTION_CONTINUE_SEARCH;
                    }
                    step.extraPages[step.extraCount++] = page;
                    g_stepping[page]++;
                    g_lostSteps++;
                    ProtectPage(page, PAGE_READWRITE);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                // Single-step the write with the page writable, comparing the bytes from where it starts.
                step.active = true;
                step.page = page;
                step.offset = address % kPageSize;
                step.length = std::min(kStepWindow, kPageSize - step.offset);
                step.writer = writer;
                memcpy(step.before, guest::ToHost(address), step.length);
                g_steps++;
                g_stepping[page]++;
                ProtectPage(page, PAGE_READWRITE);
                info->ContextRecord->EFlags |= kTrapFlag;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (g_protected.empty() || !g_protected[page])
            {
                // Not ours, unless another thread just made it writable.
                MEMORY_BASIC_INFORMATION memory;
                if (VirtualQuery(reinterpret_cast<void*>(host), &memory, sizeof(memory)) && memory.Protect == PAGE_READWRITE)
                    return EXCEPTION_CONTINUE_EXECUTION;
                fprintf(stderr, "*** write tracker: %08X isn't the tracker's (tracking %d, protected %d, by bytes %d, stepping %d, protection %lX)\n",
                    address, int(g_active.load()), g_protected.empty() ? -1 : int(g_protected[page]), int(ByBytes(page)), int(t_step.active),
                    (unsigned long)memory.Protect);
                return EXCEPTION_CONTINUE_SEARCH;
            }
            Unprotect(page, writer);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Write-protects every writable committed page in the watched range. (g_lock held)
        void ProtectAll()
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(guest::g_base);
            for (uint64_t address = kWatchStart; address < kWatchEnd;)
            {
                MEMORY_BASIC_INFORMATION memory;
                if (!VirtualQuery(reinterpret_cast<void*>(base + address), &memory, sizeof(memory)))
                    break;
                const uint64_t start = uintptr_t(memory.BaseAddress) - base;
                const uint64_t end = std::min<uint64_t>(start + memory.RegionSize, kWatchEnd);
                if (memory.State == MEM_COMMIT && memory.Protect == PAGE_READWRITE)
                {
                    if (g_hostWrites.empty())
                    {
                        DWORD old;
                        if (VirtualProtect(memory.BaseAddress, size_t(end - start), PAGE_READONLY, &old))
                            std::fill(g_protected.begin() + ptrdiff_t(start / kPageSize), g_protected.begin() + ptrdiff_t(end / kPageSize), uint8_t(1));
                    }
                    else
                        ProtectRange(uint32_t(start / kPageSize), uint32_t(end / kPageSize));
                }
                address = std::max(end, address + kPageSize);
            }
        }

        // Makes every page the tracker protected writable again. (g_lock held)
        void UnprotectAll()
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(guest::g_base);
            for (uint32_t page = 0; page < kPageCount;)
            {
                if (!g_protected[page])
                {
                    page++;
                    continue;
                }
                uint32_t end = page;
                while (end < kPageCount && g_protected[end])
                    g_protected[end++] = 0;
                DWORD old;
                VirtualProtect(reinterpret_cast<void*>(base + uintptr_t(page) * kPageSize), size_t(end - page) * kPageSize, PAGE_READWRITE, &old);
                page = end;
            }
        }

        std::vector<Write> TakeWrites()
        {
            // (g_lock held)
            std::vector<Write> writes(g_writes.begin(), g_writes.begin() + ptrdiff_t(g_writeCount));
            g_writeCount = 0;
            g_resampled = 0;
            return writes;
        }
    }

    void Start(BeforeWrite beforeWrite)
    {
        static const bool installed = [] {
            // First in line, ahead of the GPU's memory watch and the crash reporter, and before any page is protected.
            AddVectoredExceptionHandler(1, OnException);
            return true;
        }();
        (void)installed;
        std::lock_guard lock(g_lock);
        g_writes.resize(kMaxWrites);
        g_writeCount = 0;
        g_resampled = 0;
        g_lostWrites = 0;
        g_protected.assign(kPageCount, 0);
        g_byBytes.clear();
        g_bytePages.clear();
        if (g_stepping.empty())
            g_stepping.assign(kPageCount, 0); // never reset: steps in progress count down what they counted up
        g_beforeWrite = beforeWrite;
        g_active.store(true, std::memory_order_release);
        ProtectAll();
    }

    void SetBeforeWrite(BeforeWrite beforeWrite)
    {
        std::lock_guard lock(g_lock);
        g_beforeWrite = beforeWrite;
    }

    std::vector<Write> NextFrame()
    {
        std::lock_guard lock(g_lock);
        std::vector<Write> writes = TakeWrites();
        ProtectAll();
        // Pages tracked by bytes that a step is writing stay writable until it's done.
        for (uint32_t page : g_bytePages)
        {
            if (g_stepping[page])
                ProtectPage(page, PAGE_READWRITE);
        }
        return writes;
    }

    void Resample()
    {
        std::lock_guard lock(g_lock);
        if (!g_active.load(std::memory_order_relaxed))
            return;
        for (; g_resampled < g_writeCount; g_resampled++)
        {
            const uint32_t page = g_writes[g_resampled].page;
            if (g_protected[page] || InHostWrite(page))
                continue;
            DWORD old;
            if (VirtualProtect(guest::ToHost(page * kPageSize), kPageSize, PAGE_READONLY, &old))
                g_protected[page] = 1;
        }
    }

    std::vector<Write> Stop()
    {
        std::lock_guard lock(g_lock);
        g_active.store(false, std::memory_order_release);
        g_byBytes.clear();
        g_bytePages.clear();
        UnprotectAll();
        g_beforeWrite = nullptr;
        return TakeWrites();
    }

    bool Active()
    {
        return g_active.load(std::memory_order_acquire);
    }

    uint64_t LostWrites()
    {
        std::lock_guard lock(g_lock);
        return g_lostWrites;
    }

    void TrackBytes(const std::vector<uint32_t>& pages)
    {
        std::lock_guard lock(g_lock);
        if (!g_active.load(std::memory_order_relaxed))
            return;
        g_byBytes.assign(kPageCount, 0);
        g_bytePages.clear();
        g_byteWrites.clear();
        g_byteWrites.reserve(kMaxByteWriters);
        g_steps = 0;
        g_lostSteps = 0;
        for (uint32_t page : pages)
        {
            if (page >= kPageCount || InHostWrite(page))
                continue;
            if (g_beforeWrite)
                g_beforeWrite(page);
            if (!g_protected[page])
            {
                ProtectPage(page, PAGE_READONLY);
                g_protected[page] = 1;
            }
            g_byBytes[page] = 1;
            g_bytePages.push_back(page);
        }
    }

    ByteTracking StopTrackingBytes()
    {
        std::lock_guard lock(g_lock);
        g_byBytes.clear();
        g_bytePages.clear();
        ByteTracking tracking;
        tracking.writes = std::move(g_byteWrites);
        g_byteWrites = {};
        tracking.steps = g_steps;
        tracking.lost = g_lostSteps;
        return tracking;
    }

    void PrepareHostWrite(uint32_t guestAddress, uint32_t length)
    {
        if (!length || guestAddress >= kWatchEnd)
            return;
        const uint64_t end = std::min<uint64_t>(uint64_t(guestAddress) + length, kWatchEnd);
        // Under way from now on, whether tracking or not: tracking may start before it's done.
        std::lock_guard lock(g_lock);
        g_hostWrites.emplace_back(guestAddress / kPageSize, uint32_t((end - 1) / kPageSize));
        if (!g_active.load(std::memory_order_acquire))
            return;
        Writer writer = CurrentWriter(0);
        writer.function = kFileRead;
        for (uint64_t address = guestAddress & ~(kPageSize - 1); address < end && !g_protected.empty(); address += kPageSize)
        {
            const uint32_t page = uint32_t(address / kPageSize);
            if (ByBytes(page))
            {
                // A file read into a page tracked by bytes: its bytes are the read's from now on.
                g_byBytes[page] = 0;
                if (ByteWrites* entry = FindByteWrites(page, writer))
                    entry->bytes.set();
            }
            if (g_protected[page])
                Unprotect(page, writer);
        }
    }

    void FinishHostWrite(uint32_t guestAddress, uint32_t length)
    {
        if (!length || guestAddress >= kWatchEnd)
            return;
        const uint64_t end = std::min<uint64_t>(uint64_t(guestAddress) + length, kWatchEnd);
        const std::pair<uint32_t, uint32_t> range{ guestAddress / kPageSize, uint32_t((end - 1) / kPageSize) };
        std::lock_guard lock(g_lock);
        if (const auto it = std::find(g_hostWrites.begin(), g_hostWrites.end(), range); it != g_hostWrites.end())
            g_hostWrites.erase(it);
        if (!g_active.load(std::memory_order_acquire) || g_protected.empty())
            return;
        // Pages a boundary passed by during the write, left writable: written by it in this frame too.
        Writer writer = CurrentWriter(0);
        writer.function = kFileRead;
        for (uint32_t page = range.first; page <= range.second; page++)
        {
            if (g_protected[page] || ByBytes(page) || InHostWrite(page))
                continue;
            if (g_writeCount < g_writes.size())
                g_writes[g_writeCount++] = Write{ page, writer };
            else
                g_lostWrites++;
        }
    }

    void WriteUntracked(uint32_t page, const void* contents, const std::bitset<kPageSize>* mask)
    {
        std::lock_guard lock(g_lock);
        uint8_t* host = guest::ToHost(page * kPageSize);
        const bool isProtected = page < g_protected.size() && g_protected[page];
        if (isProtected)
            ProtectPage(page, PAGE_READWRITE);
        if (!mask)
            memcpy(host, contents, kPageSize);
        else
        {
            const auto* bytes = static_cast<const uint8_t*>(contents);
            for (uint32_t i = 0; i < kPageSize; i++)
            {
                if ((*mask)[i])
                    host[i] = bytes[i];
            }
        }
        if (isProtected)
            ProtectPage(page, PAGE_READONLY);
    }

    std::string Hex(uint32_t value)
    {
        char text[16];
        snprintf(text, sizeof(text), "%08X", value);
        return text;
    }

    std::string WriterText(const Writer& writer)
    {
        std::string text = writer.thread ? "thread " + Hex(writer.thread) : std::string("host thread");
        if (writer.fiber)
            text += " fiber " + Hex(writer.fiber);
        if (writer.function == kFileRead)
            text += " file read";
        else if (writer.function & 1)
            text += " runtime, called from " + Hex(writer.function & ~1u);
        else if (writer.function)
            text += " sub_" + Hex(writer.function);
        return text;
    }
}
