#include "netplay/replay.h"

#include "gpu/command_processor.h"
#include "gpu/memory_watch.h"
#include "guest_abi.h"
#include "guest_memory.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/heaps.h"
#include "netplay/page_log.h"
#include "netplay/write_tracker.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace netplay::replay
{
    namespace
    {
        std::atomic<bool> g_active{ false };
        std::vector<void (*)()> g_endCallbacks;
    }

    bool Active()
    {
        return g_active.load(std::memory_order_relaxed);
    }

    void Begin()
    {
        if (g_active.exchange(true))
            return;
        gpu::BeginDiscard();
    }

    void End()
    {
        if (!g_active.exchange(false))
            return;
        gpu::EndDiscard();
        for (void (*callback)() : g_endCallbacks)
            callback();
    }

    void OnEnd(void (*callback)())
    {
        g_endCallbacks.push_back(callback);
    }
}

namespace netplay::journal
{
    namespace
    {
        enum class Mode
        {
            kOff,
            kRecord,
            kReplay,
        };

        constexpr uint32_t kPcrStackBase = 0x70;  // the running stack's highest address
        constexpr uint32_t kPcrStackLimit = 0x74; // and its lowest
        constexpr uint32_t kStackSpan = 4096;     // the caller's stack watched for changes, from its stack pointer up
        constexpr uint32_t kDeadSpan = 16384;     // and below it, where the call's own frames go
        constexpr uint32_t kPointerSpan = 256;    // and at each pointer argument into the game's memory
        constexpr uint32_t kMergeGap = 8;          // changed bytes closer than this make one change

        struct Change
        {
            uint32_t address = 0;
            std::vector<uint8_t> bytes;
        };

        // A file read recorded: where it wrote, the bytes there before and after, and the frame it was done in.
        struct Read
        {
            uint64_t frame = 0;
            uint32_t address = 0;
            std::vector<uint8_t> before, after;
        };

        struct Entry
        {
            uint64_t frame = 0;
            uint32_t function = 0, lr = 0, r1 = 0;
            uint32_t arguments[8] = {}; // r3-r10
            uint64_t floats[4] = {};    // f1-f4, as bits
            uint64_t key = 0;           // r3, the arguments the function reads and its input block's fields (Describe)
            std::vector<uint8_t> block; // the input block's fields
            uint64_t r3 = 0;
            std::vector<Change> changes;
            // The context's bytes the call changed (its results and the registers it left, which the caller may pass on
            // to the next call though it doesn't use them), by offset in PPCContext.
            std::vector<Change> context;
            std::vector<Read> reads; // the file reads done since the call recorded before, until this one returned
            bool readsWritten = false; // in the replay under way (its reads written as it was answered, or before a made call)
            bool deferred = false;     // a release, made once past rolling back (Defer)
            bool made = false;         // made in a replay: the record didn't hold it
            uint32_t answered = 0;     // times answered from the record in replays
            uint64_t firstFrame = 0;   // the frame it was first made in (answering it from another frame moves it)
        };

        // A release deferred until it's past rolling back: the function and its arguments (r3-r6).
        struct Span
        {
            uint32_t address = 0, size = 0;
            std::vector<uint8_t> before;
        };

        std::atomic<Mode> g_mode{ Mode::kOff };
        // The current timeline's calls, by frame; and, during a replay and for a few frames after it, the abandoned
        // timelines' calls that no replay has made yet (by frame), which calls of the new one may still match.
        std::vector<Entry> g_entries;
        std::vector<Entry> g_old;
        uint64_t g_replayFrom = 0; // the frame the replay under way was loaded at
        Statistics g_statistics;
        std::map<uint32_t, uint64_t> g_counts; // function -> calls recorded
        std::string g_abandoned;               // the first abandoned calls, for the dump
        size_t g_abandonedListed = 0;
        thread_local int t_depth = 0;          // inside a recorded call

        // The game's description of its functions (Describe, from its static initializers: a function's static).
        std::unordered_map<uint32_t, FunctionInfo>& Functions()
        {
            static std::unordered_map<uint32_t, FunctionInfo> functions;
            return functions;
        }

        // The file reads, from the threads that read (the file system's) to the main thread's calls.
        constexpr uint64_t kMaxReadBytes = 256ull * 1024 * 1024; // kept of the reads recorded, before and after
        std::mutex g_readLock;              // the following
        std::vector<Read> g_pendingReads;   // done, but not before a call recorded yet
        std::vector<Read> g_rewrite;        // pending reads a load undid, written as the replay finishes (or before a made call)
        bool g_rewritten = false;           // g_rewrite written in the replay under way
        uint64_t g_readBytesKept = 0;
        uint64_t g_reads = 0, g_readBytes = 0, g_readsLost = 0;
        thread_local Read t_read; // the read under way on this thread (BeforeHostWrite to AfterHostWrite)
        thread_local bool t_reading = false;

        using Clock = std::chrono::steady_clock;

        double Microseconds(Clock::duration duration)
        {
            return std::chrono::duration<double, std::micro>(duration).count();
        }

        // DEBUG_JOURNAL=<function>[,<function>...] (hex addresses, diagnostics): each call of those functions is logged
        // with how the journal handled it.
        bool Traced(uint32_t function)
        {
            static const std::vector<uint32_t> traced = [] {
                std::vector<uint32_t> functions;
                if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_JOURNAL"))
                {
                    for (const char* p = setting; *p;)
                    {
                        char* end = nullptr;
                        functions.push_back(uint32_t(strtoul(p, &end, 16)));
                        if (!end || end == p)
                            break;
                        p = *end == ',' ? end + 1 : end;
                    }
                }
                return functions;
            }();
            return !traced.empty() && std::find(traced.begin(), traced.end(), function) != traced.end();
        }

        const FunctionInfo* Info(uint32_t function)
        {
            const auto& functions = Functions();
            const auto it = functions.find(function);
            return it == functions.end() ? nullptr : &it->second;
        }

        uint32_t NearFrames(uint32_t function)
        {
            const FunctionInfo* info = Info(function);
            return info ? info->nearFrames : 0;
        }

        std::vector<Read> TakePendingReads()
        {
            std::lock_guard lock(g_readLock);
            return std::exchange(g_pendingReads, {});
        }

        uint64_t ReadBytes(const std::vector<Read>& reads)
        {
            uint64_t bytes = 0;
            for (const Read& read : reads)
                bytes += read.before.size() + read.after.size();
            return bytes;
        }

        // Writes guest memory as a savestate's load does: past the write tracker's protection, unrecorded (a page at a
        // time), and the GPU's memory watch (told first, as for a file read). Pages no longer committed (a heap gave
        // them back meanwhile) are left out; returns false if any was.
        bool WriteGuest(uint32_t address, const uint8_t* bytes, size_t size)
        {
            bool whole = true;
            const uint64_t end = uint64_t(address) + size;
            for (uint64_t at = address; at < end;)
            {
                MEMORY_BASIC_INFORMATION memory{};
                if (!VirtualQuery(guest::ToHost(uint32_t(at)), &memory, sizeof(memory)))
                    return false;
                const uint64_t regionEnd = std::min<uint64_t>(
                    end, at + (reinterpret_cast<uint8_t*>(memory.BaseAddress) + memory.RegionSize - guest::ToHost(uint32_t(at))));
                if (memory.State != MEM_COMMIT)
                {
                    whole = false;
                    at = regionEnd;
                    continue;
                }
                guest::BeforeHostWrite(uint32_t(at), regionEnd - at);
                gpu::watch::PrepareHostWrite(uint32_t(at), uint32_t(regionEnd - at));
                uint64_t next = at;
                if (tracker::Active())
                {
                    std::vector<uint8_t> contents(tracker::kPageSize);
                    std::bitset<tracker::kPageSize> mask;
                    while (next < regionEnd && next < tracker::kWatchEnd)
                    {
                        const uint32_t page = uint32_t(next / tracker::kPageSize);
                        const uint64_t pageStart = uint64_t(page) * tracker::kPageSize;
                        const uint64_t pageEnd = std::min<uint64_t>(pageStart + tracker::kPageSize, regionEnd);
                        const uint8_t* source = bytes + (next - address);
                        if (next == pageStart && pageEnd == pageStart + tracker::kPageSize)
                            tracker::WriteUntracked(page, source);
                        else
                        {
                            mask.reset();
                            for (uint64_t i = next; i < pageEnd; i++)
                                mask.set(size_t(i - pageStart));
                            memcpy(contents.data() + (next - pageStart), source, size_t(pageEnd - next));
                            tracker::WriteUntracked(page, contents.data(), &mask);
                        }
                        next = pageEnd;
                    }
                }
                if (next < regionEnd)
                    memcpy(guest::ToHost(uint32_t(next)), bytes + (next - address), size_t(regionEnd - next));
                at = regionEnd;
            }
            return whole;
        }

        // The replay's writes of what reads read; with the page log, into the game's memory only (BeforeHostWrite in
        // replay.h), in runs of pages it watches.
        void WriteReads(const std::vector<Read>& reads)
        {
            const bool gameOnly = pagelog::Active();
            for (const Read& read : reads)
            {
                g_statistics.readsWritten++;
                if (!gameOnly)
                {
                    WriteGuest(read.address, read.after.data(), read.after.size());
                    continue;
                }
                const uint64_t end = uint64_t(read.address) + read.after.size();
                for (uint64_t at = read.address; at < end;)
                {
                    const bool watched = pagelog::Watches(uint32_t(at >> 12));
                    uint64_t runEnd = std::min<uint64_t>((at | 0xFFF) + 1, end);
                    while (runEnd < end && pagelog::Watches(uint32_t(runEnd >> 12)) == watched)
                        runEnd = std::min<uint64_t>(runEnd + 0x1000, end);
                    if (watched)
                        WriteGuest(uint32_t(at), read.after.data() + (at - read.address), size_t(runEnd - at));
                    else
                        g_statistics.readBytesLeft += runEnd - at;
                    at = runEnd;
                }
            }
        }

        void WriteEntryReads(Entry& entry)
        {
            if (entry.readsWritten)
                return;
            entry.readsWritten = true;
            WriteReads(entry.reads);
        }

        // The pending reads the load undid, written once a replay (before a made call, or as it finishes).
        void WriteRewrite()
        {
            std::lock_guard lock(g_readLock);
            if (g_rewritten)
                return;
            g_rewritten = true;
            WriteReads(g_rewrite);
        }

        double Value(uint64_t bits)
        {
            double value;
            memcpy(&value, &bits, sizeof(value));
            return value;
        }

        uint64_t Mix(uint64_t hash, uint64_t value)
        {
            for (int i = 0; i < 8; i++)
                hash = (hash ^ ((value >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
            return hash;
        }

        // The call as the journal compares it: its frame, function, call site and stack pointer, and its key: r3, the
        // other registers the function reads (FunctionInfo::arguments) and its input block's fields.
        Entry Describe(uint32_t address, const PPCContext& ctx)
        {
            Entry entry;
            entry.frame = Frame();
            entry.firstFrame = entry.frame;
            entry.function = address;
            entry.lr = uint32_t(ctx.lr);
            entry.r1 = ctx.r1.u32;
            const PPCRegister* registers[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
            for (size_t i = 0; i < 8; i++)
                entry.arguments[i] = registers[i]->u32;
            const PPCRegister* floats[] = { &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7, &ctx.f8, &ctx.f9, &ctx.f10, &ctx.f11,
                &ctx.f12, &ctx.f13 };
            for (size_t i = 0; i < 4; i++)
                entry.floats[i] = floats[i]->u64;
            uint64_t key = 0xCBF29CE484222325ull;
            const FunctionInfo* info = Info(address);
            // r3 when the function reads it (the object, mostly; one that doesn't has a leftover there), or for a function
            // the game didn't describe.
            if (!info || (info->arguments & kArgumentR3))
                key = Mix(key, entry.arguments[0]);
            if (info)
            {
                for (uint32_t i = 0; i < 7; i++)
                {
                    if (info->arguments & (1u << i))
                        key = Mix(key, entry.arguments[1 + i]);
                }
                for (uint32_t i = 0; i < 13; i++)
                {
                    if (info->arguments & (1u << (7 + i)))
                        key = Mix(key, floats[i]->u64);
                }
                if (info->blockRegister >= 4 && info->blockRegister <= 10)
                {
                    const uint32_t block = entry.arguments[info->blockRegister - 3];
                    for (const auto& [offset, length] : info->blockFields)
                    {
                        const uint32_t start = block + offset;
                        if (!block || !guest::IsAllocated(start) || !guest::IsAllocated(start + length - 1))
                        {
                            entry.block.insert(entry.block.end(), length, 0);
                            key = Mix(key, ~0ull);
                            continue;
                        }
                        const uint8_t* bytes = guest::ToHost(start);
                        entry.block.insert(entry.block.end(), bytes, bytes + length);
                        for (uint32_t i = 0; i < length; i++)
                            key = (key ^ bytes[i]) * 0x100000001B3ull;
                    }
                }
            }
            entry.key = key;
            return entry;
        }

        bool SameCall(const Entry& a, const Entry& b)
        {
            return a.function == b.function && a.lr == b.lr && a.r1 == b.r1 && a.key == b.key && a.deferred == b.deferred;
        }

        std::string Text(const Entry& entry)
        {
            char text[320];
            int length = snprintf(text, sizeof(text), "frame %llu: sub_%08X from %08X (r1 %08X; r3-r8 %08X %08X %08X %08X %08X %08X; f1 %g",
                (unsigned long long)entry.frame, entry.function, entry.lr, entry.r1, entry.arguments[0], entry.arguments[1], entry.arguments[2],
                entry.arguments[3], entry.arguments[4], entry.arguments[5], Value(entry.floats[0]));
            if (!entry.block.empty() && length > 0 && length < int(sizeof(text)))
            {
                length += snprintf(text + length, sizeof(text) - length, "; block");
                for (size_t i = 0; i < std::min<size_t>(entry.block.size(), 12) && length < int(sizeof(text)) - 4; i++)
                    length += snprintf(text + length, sizeof(text) - length, "%s%02X", i % 4 ? "" : " ", entry.block[i]);
            }
            if (length > 0 && length < int(sizeof(text)) - 2)
                snprintf(text + length, sizeof(text) - length, ")");
            return text;
        }

        // Into g_entries, by frame (a replay's calls come in order, after the record's earlier frames).
        void Insert(Entry&& entry)
        {
            if (g_entries.empty() || g_entries.back().frame <= entry.frame)
            {
                g_entries.push_back(std::move(entry));
                return;
            }
            const auto at = std::upper_bound(g_entries.begin(), g_entries.end(), entry.frame,
                [](uint64_t frame, const Entry& other) { return frame < other.frame; });
            g_entries.insert(at, std::move(entry));
        }

        bool InGameMemory(uint32_t address)
        {
            return (address >= 0x3D000000 && address < 0x40000000) || heaps::InGameHeap(address);
        }

        // The memory a call may change that the game's state holds: the caller's stack from its stack pointer up, and
        // what pointer arguments point to in the game's heap and arena, or further up its stack. And the stack below the
        // stack pointer, where the call's frames leave what they held: the caller's later frames copy such leftovers
        // into the game's records where they don't initialize a field (DOA4's sub_82628318, the return addresses of
        // CRI's functions), which a replay then leaves the same.
        std::vector<Span> Spans(const PPCContext& ctx, const FunctionInfo* info)
        {
            std::vector<Span> spans;
            const uint32_t r1 = ctx.r1.u32;
            uint32_t top = guest::LoadU32(ctx.r13.u32 + kPcrStackBase);
            if (top <= r1 || top - r1 > 64 * 1024 * 1024)
                top = r1 + kStackSpan;
            const uint32_t stackEnd = std::min(top, r1 + kStackSpan);
            spans.push_back({ r1, stackEnd - r1 });
            const uint32_t limit = guest::LoadU32(ctx.r13.u32 + kPcrStackLimit);
            const uint32_t below = limit < r1 && r1 - limit < 64 * 1024 * 1024 ? std::min(r1 - limit, kDeadSpan) : 0;
            if (below)
                spans.push_back({ r1 - below, below });
            const PPCRegister* registers[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
            for (const PPCRegister* reg : registers)
            {
                const uint32_t value = reg->u32;
                const bool stack = value >= stackEnd && value < top;
                if (!stack && !InGameMemory(value))
                    continue;
                const uint32_t end = stack ? std::min(top, value + kPointerSpan) : value + kPointerSpan;
                if (std::none_of(spans.begin(), spans.end(), [&](const Span& span) { return value >= span.address && end <= span.address + span.size; }))
                    spans.push_back({ value, end - value });
            }
            // The objects the game says the function changes (FunctionInfo::objects), wherever they lie.
            if (info)
            {
                for (const auto& [index, length] : info->objects)
                {
                    if (index < 3 || index > 10 || !length)
                        continue;
                    const uint32_t value = registers[index - 3]->u32;
                    if (!value || !guest::IsAllocated(value) || !guest::IsAllocated(value + length - 1))
                        continue;
                    if (std::none_of(spans.begin(), spans.end(),
                            [&](const Span& span) { return value >= span.address && value + length <= span.address + span.size; }))
                        spans.push_back({ value, length });
                }
            }
            for (Span& span : spans)
            {
                const uint8_t* bytes = guest::ToHost(span.address);
                span.before.assign(bytes, bytes + span.size);
            }
            return spans;
        }

        void Collect(const std::vector<Span>& spans, std::vector<Change>& changes)
        {
            for (const Span& span : spans)
            {
                const uint8_t* now = guest::ToHost(span.address);
                for (uint32_t i = 0; i < span.size;)
                {
                    if (now[i] == span.before[i])
                    {
                        i++;
                        continue;
                    }
                    uint32_t last = i;
                    for (uint32_t j = i + 1; j < span.size && j <= last + kMergeGap; j++)
                    {
                        if (now[j] != span.before[j])
                            last = j;
                    }
                    changes.push_back({ span.address + i, std::vector<uint8_t>(now + i, now + last + 1) });
                    i = last + 1;
                }
            }
        }

        // The bytes of the context a call changed, in 8-byte words (the registers), as changes by offset.
        void CollectContext(const PPCContext& before, const PPCContext& after, std::vector<Change>& changes)
        {
            const auto* a = reinterpret_cast<const uint8_t*>(&before);
            const auto* b = reinterpret_cast<const uint8_t*>(&after);
            for (size_t offset = 0; offset < sizeof(PPCContext);)
            {
                if (memcmp(a + offset, b + offset, 8) == 0)
                {
                    offset += 8;
                    continue;
                }
                size_t end = offset + 8;
                while (end < sizeof(PPCContext) && memcmp(a + end, b + end, 8) != 0)
                    end += 8;
                changes.push_back({ uint32_t(offset), std::vector<uint8_t>(b + offset, b + end) });
                offset = end;
            }
        }

        // Makes the call and records it: as a first run's, or in a replay the new timeline's (`made`).
        void RecordCall(Entry&& entry, Function function, PPCContext& ctx, uint8_t* base, bool made)
        {
            static_assert(sizeof(PPCContext) % 8 == 0);
            const std::vector<Span> spans = Spans(ctx, Info(entry.function));
            const PPCContext before = ctx;
            const Clock::time_point start = made ? Clock::now() : Clock::time_point{};
            t_depth++;
            function(ctx, base);
            t_depth--;
            if (made)
                g_statistics.madeCallMicroseconds += Microseconds(Clock::now() - start);
            Collect(spans, entry.changes);
            CollectContext(before, ctx, entry.context);
            entry.r3 = ctx.r3.u64;
            entry.reads = TakePendingReads();
            entry.readsWritten = true; // done for real
            entry.made = made;
            for (const Change& change : entry.changes)
                g_statistics.bytes += change.bytes.size();
            if (!made)
                g_statistics.recorded++;
            g_counts[entry.function]++;
            Insert(std::move(entry));
        }

        // An abandoned timeline's call that's the same as `call`: one of the same frame, or for functions that may match
        // a few frames away, the nearest within them (the earlier first).
        std::vector<Entry>::iterator FindOld(const Entry& call, bool& nearby)
        {
            nearby = false;
            const auto first = std::lower_bound(g_old.begin(), g_old.end(), call.frame,
                [](const Entry& entry, uint64_t frame) { return entry.frame < frame; });
            for (auto it = first; it != g_old.end() && it->frame == call.frame; ++it)
            {
                if (SameCall(*it, call))
                    return it;
            }
            const uint32_t nearFrames = NearFrames(call.function);
            if (!nearFrames)
                return g_old.end();
            auto best = g_old.end();
            uint64_t bestDistance = ~0ull;
            const uint64_t low = call.frame > nearFrames ? call.frame - nearFrames : 0;
            for (auto it = std::lower_bound(g_old.begin(), g_old.end(), low, [](const Entry& entry, uint64_t frame) { return entry.frame < frame; });
                 it != g_old.end() && it->frame <= call.frame + nearFrames; ++it)
            {
                if (!SameCall(*it, call))
                    continue;
                const uint64_t distance = it->frame > call.frame ? it->frame - call.frame : call.frame - it->frame;
                if (distance < bestDistance)
                {
                    best = it;
                    bestDistance = distance;
                }
            }
            nearby = best != g_old.end();
            return best;
        }

        // Answers a call from the abandoned timeline's record of it: what it changed, and the registers it left. It joins
        // the new timeline's record, at the call's frame.
        void Answer(std::vector<Entry>::iterator it, PPCContext& ctx, uint64_t frame, bool nearby)
        {
            const Clock::time_point start = Clock::now();
            Entry entry = std::move(*it);
            g_old.erase(it);
            WriteEntryReads(entry);
            for (const Change& change : entry.changes)
            {
                guest::BeforeHostWrite(change.address, change.bytes.size());
                memcpy(guest::ToHost(change.address), change.bytes.data(), change.bytes.size());
            }
            for (const Change& change : entry.context)
                memcpy(reinterpret_cast<uint8_t*>(&ctx) + change.address, change.bytes.data(), change.bytes.size());
            entry.answered++;
            entry.frame = frame;
            g_statistics.replayed++;
            if (nearby)
                g_statistics.replayedNear++;
            Insert(std::move(entry));
            g_statistics.answeredMicroseconds += Microseconds(Clock::now() - start);
        }

        // Before a call the record doesn't hold is made in a replay: every read the abandoned timeline did is written,
        // since the file system is in the present and the call may tell the game of them.
        void WriteAbandonedReads()
        {
            for (Entry& entry : g_old)
                WriteEntryReads(entry);
            WriteRewrite();
        }

        CallRecord RecordOf(const Entry& entry)
        {
            CallRecord record;
            record.frame = entry.frame;
            record.function = entry.function;
            record.lr = entry.lr;
            memcpy(record.arguments, entry.arguments, sizeof(record.arguments));
            record.result = entry.r3;
            record.block = entry.block.empty() ? nullptr : entry.block.data();
            record.blockSize = entry.block.size();
            return record;
        }

        // A call of an abandoned timeline that no replay made: undone where the game says how (a sound stopped), on the
        // boundary's guest context; a release is dropped (never made).
        void Abandon(Entry& entry, const std::vector<CallRecord>& current)
        {
            g_statistics.unreplayed++;
            if (g_statistics.firstUnreplayed.empty())
                g_statistics.firstUnreplayed = Text(entry);
            if (g_abandonedListed < 100)
            {
                g_abandonedListed++;
                g_abandoned += "  " + Text(entry) + (entry.deferred ? ": a release, dropped" : "") + "\n";
            }
            {
                std::lock_guard lock(g_readLock);
                g_readBytesKept -= std::min(g_readBytesKept, ReadBytes(entry.reads));
            }
            if (entry.deferred)
            {
                g_statistics.releasesDropped++;
                return;
            }
            // Its reads, if no replay wrote them: the game never learned of them in the new timeline, but they're in the
            // memory the game gave the file system for them, as the file system has them.
            WriteEntryReads(entry);
            const FunctionInfo* info = Info(entry.function);
            if (info && info->query)
            {
                g_statistics.unreplayedQueries++;
                return;
            }
            PPCContext* ctx = BoundaryContext();
            if (!info || !info->undo || !ctx)
                return;
            const PPCContext saved = *ctx;
            info->undo(*ctx, guest::g_base, RecordOf(entry), current);
            *ctx = saved;
            g_statistics.undone++;
        }

        // The abandoned calls whose frame the game has passed (by more than the frames they may match from), undone; all
        // of them with `all`.
        void AbandonPassed(uint64_t frame, bool all)
        {
            if (g_old.empty())
                return;
            std::vector<CallRecord> current;
            bool listed = false;
            for (auto it = g_old.begin(); it != g_old.end();)
            {
                if (!all && it->frame + NearFrames(it->function) >= frame)
                {
                    ++it;
                    continue;
                }
                if (!listed)
                {
                    listed = true;
                    current.reserve(g_entries.size());
                    for (const Entry& entry : g_entries)
                        current.push_back(RecordOf(entry));
                }
                Abandon(*it, current);
                it = g_old.erase(it);
            }
        }

        // A release made, now that no savestate from its frame can be loaded, on the frame boundary's guest context (its
        // registers kept).
        void MakeRelease(const Entry& entry)
        {
            PPCContext* ctx = BoundaryContext();
            if (!ctx)
            {
                LOG_WARN("Journal: a deferred release of sub_%08X dropped: not made at a frame boundary", entry.function);
                return;
            }
            const PPCContext saved = *ctx;
            kernel::CallGuest(*ctx, entry.function, { entry.arguments[0], entry.arguments[1], entry.arguments[2], entry.arguments[3] });
            *ctx = saved;
            g_statistics.deferred++;
        }

        // The record, with how replays went with each call, for reports.
        std::string g_dump;
        constexpr size_t kDumpedCalls = 400;

        void DumpRecord()
        {
            g_dump.clear();
            size_t dumped = 0;
            for (const Entry& entry : g_entries)
            {
                if (dumped++ == kDumpedCalls)
                {
                    g_dump += "  (" + std::to_string(g_entries.size() - kDumpedCalls) + " more)\n";
                    break;
                }
                char result[200];
                const char* how = entry.made ? "made in a replay (not in the record)" : entry.answered ? "answered" : "not made in a replay";
                char moved[48] = "";
                if (entry.frame != entry.firstFrame)
                    snprintf(moved, sizeof(moved), " (recorded at frame %llu)", (unsigned long long)entry.firstFrame);
                if (entry.deferred)
                    snprintf(result, sizeof(result), ": a release, deferred; %s%s\n", how, moved);
                else
                    snprintf(result, sizeof(result), " = %08X, %zu changes; %s%s\n", uint32_t(entry.r3), entry.changes.size(), how, moved);
                g_dump += "  " + Text(entry) + result;
                for (const Read& read : entry.reads)
                {
                    snprintf(result, sizeof(result), "    after a file read of %zu bytes to %08X\n", read.after.size(), read.address);
                    g_dump += result;
                }
            }
            if (!g_abandoned.empty())
                g_dump += "  Calls of abandoned timelines no replay made (undone where they could be):\n" + g_abandoned;
        }
    }

    void Describe(std::vector<FunctionInfo> functions)
    {
        auto& described = Functions();
        described.clear();
        for (FunctionInfo& info : functions)
        {
            const uint32_t address = info.address;
            described[address] = std::move(info);
        }
    }

    bool Enabled()
    {
        return g_mode.load(std::memory_order_relaxed) != Mode::kOff;
    }

    void Call(uint32_t address, Function function, PPCContext& ctx, uint8_t* base)
    {
        const Mode mode = g_mode.load(std::memory_order_relaxed);
        if (mode == Mode::kOff || t_depth > 0)
        {
            function(ctx, base);
            return;
        }
        Entry call = Describe(address, ctx);
        const bool traced = Traced(address);
        // The same call of an abandoned timeline (in a replay, or a few frames after one): its effect stands.
        if (!g_old.empty())
        {
            bool nearby = false;
            if (const auto it = FindOld(call, nearby); it != g_old.end())
            {
                if (traced)
                    LOG_INFO("Journal: answered %s", Text(call).c_str());
                Answer(it, ctx, call.frame, nearby);
                return;
            }
        }
        if (mode == Mode::kRecord)
        {
            if (traced)
                LOG_INFO("Journal: recorded %s", Text(call).c_str());
            RecordCall(std::move(call), function, ctx, base, false);
            return;
        }
        // Not in the record: made, as in a first run, and the new timeline's from here. The first few are logged, with the
        // abandoned timeline's calls of the same function nearby, for telling why.
        if (!g_statistics.mismatched)
            g_statistics.firstMismatch = Text(call);
        if (g_statistics.mismatched < 24 || traced)
        {
            std::string nearby;
            for (const Entry& entry : g_old)
            {
                if (entry.function == call.function && entry.frame + 8 >= call.frame && entry.frame <= call.frame + 8)
                    nearby += "; the record's " + Text(entry) + (entry.key == call.key ? "" : " (other arguments)");
            }
            LOG_INFO("Journal: a replayed call the record doesn't hold, made: %s%s", Text(call).c_str(),
                nearby.empty() ? "; none of that function's within 8 frames" : nearby.c_str());
        }
        g_statistics.mismatched++;
        WriteAbandonedReads();
        const Clock::time_point start = Clock::now();
        RecordCall(std::move(call), function, ctx, base, true);
        g_statistics.madeMicroseconds += Microseconds(Clock::now() - start);
    }

    void Defer(uint32_t address, Function function, PPCContext& ctx, uint8_t* base)
    {
        const Mode mode = g_mode.load(std::memory_order_relaxed);
        if (mode == Mode::kOff || t_depth > 0)
        {
            function(ctx, base);
            return;
        }
        Entry call = Describe(address, ctx);
        call.deferred = true;
        if (!g_old.empty())
        {
            bool nearby = false;
            if (const auto it = FindOld(call, nearby); it != g_old.end())
            {
                Answer(it, ctx, call.frame, nearby);
                return;
            }
        }
        if (mode == Mode::kReplay)
        {
            if (!g_statistics.mismatched)
                g_statistics.firstMismatch = Text(call) + " (a release, deferred too)";
            g_statistics.mismatched++;
        }
        else
            g_statistics.recorded++;
        call.reads = TakePendingReads();
        call.readsWritten = true;
        call.made = mode == Mode::kReplay;
        g_counts[address]++;
        Insert(std::move(call));
    }

    void Record()
    {
        if (g_mode.load(std::memory_order_relaxed) != Mode::kOff)
            Stop();
        g_entries.clear();
        g_old.clear();
        g_statistics = Statistics{};
        g_counts.clear();
        g_dump.clear();
        g_abandoned.clear();
        g_abandonedListed = 0;
        {
            std::lock_guard lock(g_readLock);
            g_pendingReads.clear();
            g_rewrite.clear();
            g_rewritten = false;
            g_readBytesKept = g_reads = g_readBytes = g_readsLost = 0;
        }
        g_mode.store(Mode::kRecord, std::memory_order_relaxed);
    }

    void Replay()
    {
        ReplayFrom(0);
    }

    void ReplayFrom(uint64_t frame)
    {
        if (g_mode.load(std::memory_order_relaxed) == Mode::kOff)
            return;
        if (g_mode.load(std::memory_order_relaxed) == Mode::kReplay)
            FinishReplay();
        // The record's calls from `frame` on, abandoned (with earlier abandoned timelines' that no replay made yet).
        const auto first = std::lower_bound(g_entries.begin(), g_entries.end(), frame, [](const Entry& entry, uint64_t value) { return entry.frame < value; });
        for (auto it = first; it != g_entries.end(); ++it)
        {
            it->readsWritten = false;
            g_old.push_back(std::move(*it));
        }
        g_entries.erase(first, g_entries.end());
        std::stable_sort(g_old.begin(), g_old.end(), [](const Entry& a, const Entry& b) { return a.frame < b.frame; });
        {
            // The reads done since the last call recorded, in the frames the load undid: written again as the replay
            // finishes, then still waiting for a call to come with.
            std::lock_guard lock(g_readLock);
            for (auto it = g_pendingReads.begin(); it != g_pendingReads.end();)
            {
                if (it->frame >= frame)
                {
                    g_rewrite.push_back(std::move(*it));
                    it = g_pendingReads.erase(it);
                }
                else
                    ++it;
            }
            g_rewritten = false;
        }
        g_replayFrom = frame;
        g_mode.store(Mode::kReplay, std::memory_order_relaxed);
    }

    void FinishReplay()
    {
        if (g_mode.load(std::memory_order_relaxed) != Mode::kReplay)
            return;
        // The reads the load undid that no answered call wrote: those of calls not made (the abandoned timeline's), and
        // those after the record's last call (again waiting for a call to come with).
        for (Entry& entry : g_old)
        {
            if (entry.frame >= g_replayFrom)
                WriteEntryReads(entry);
        }
        WriteRewrite();
        {
            std::lock_guard lock(g_readLock);
            for (Read& read : g_rewrite)
                g_pendingReads.push_back(std::move(read));
            g_rewrite.clear();
            g_rewritten = false;
        }
        g_mode.store(Mode::kRecord, std::memory_order_relaxed);
        AbandonPassed(Frame(), false);
    }

    void OnFrame(uint64_t frame, uint64_t oldest)
    {
        const Mode mode = g_mode.load(std::memory_order_relaxed);
        if (mode == Mode::kOff)
            return;
        if (mode == Mode::kRecord)
            AbandonPassed(frame, false);
        // Past rolling back: forgotten, the releases among them made.
        const auto end = std::lower_bound(g_entries.begin(), g_entries.end(), oldest, [](const Entry& entry, uint64_t value) { return entry.frame < value; });
        if (end == g_entries.begin())
            return;
        uint64_t freed = 0;
        for (auto it = g_entries.begin(); it != end; ++it)
        {
            if (it->deferred)
                MakeRelease(*it);
            freed += ReadBytes(it->reads);
        }
        g_entries.erase(g_entries.begin(), end);
        std::lock_guard lock(g_readLock);
        g_readBytesKept -= std::min(g_readBytesKept, freed);
    }

    void Stop()
    {
        const Mode mode = g_mode.load(std::memory_order_relaxed);
        if (mode == Mode::kOff)
            return;
        if (mode == Mode::kReplay)
            FinishReplay();
        AbandonPassed(0, true);
        DumpRecord();
        g_mode.store(Mode::kOff, std::memory_order_relaxed);
        // The deferred releases, now that no savestate from their frames will be loaded.
        for (const Entry& entry : g_entries)
        {
            if (entry.deferred)
                MakeRelease(entry);
        }
        g_entries.clear();
        g_old.clear();
        {
            std::lock_guard lock(g_readLock);
            g_statistics.reads = g_reads;
            g_statistics.readBytes = g_readBytes;
            g_statistics.readsLost = g_readsLost;
            g_pendingReads.clear();
            g_rewrite.clear();
            g_readBytesKept = 0;
        }
    }

    bool MadeInReplay(uint32_t function)
    {
        if (g_mode.load(std::memory_order_relaxed) != Mode::kReplay)
            return false;
        // The record from the replay's frame on is the replay's (ReplayFrom moved the rest to the abandoned calls).
        for (auto it = g_entries.rbegin(); it != g_entries.rend() && it->frame >= g_replayFrom; ++it)
        {
            if (it->function == function)
                return it->made && !it->answered;
        }
        return false;
    }

    void BeforeHostWrite(uint32_t address, uint32_t length)
    {
        t_reading = false;
        if (!length || g_mode.load(std::memory_order_relaxed) == Mode::kOff)
            return;
        const uint64_t end = uint64_t(address) + length;
        if (end > 0x100000000ull || !guest::IsAllocated(address) || !guest::IsAllocated(uint32_t(end - 1)))
            return;
        {
            std::lock_guard lock(g_readLock);
            if (g_readBytesKept + 2ull * length > kMaxReadBytes)
            {
                g_readsLost++;
                return;
            }
            g_readBytesKept += 2ull * length;
        }
        const uint8_t* bytes = guest::ToHost(address);
        t_read.address = address;
        t_read.before.assign(bytes, bytes + length);
        t_reading = true;
    }

    void AfterHostWrite(uint32_t address, uint32_t length)
    {
        if (!t_reading)
            return;
        t_reading = false;
        Read read = std::move(t_read);
        t_read = Read{};
        if (read.address != address || read.before.size() != length)
            return;
        const uint8_t* bytes = guest::ToHost(address);
        read.after.assign(bytes, bytes + length);
        read.frame = Frame();
        std::lock_guard lock(g_readLock);
        if (g_mode.load(std::memory_order_relaxed) == Mode::kOff)
        {
            // Under way as the record ended (a load), or as it began again.
            g_readsLost++;
            return;
        }
        g_reads++;
        g_readBytes += length;
        g_pendingReads.push_back(std::move(read));
    }

    void RewindReads()
    {
        std::vector<const std::vector<Read>*> lists;
        for (const Entry& entry : g_entries)
            lists.push_back(&entry.reads);
        std::lock_guard lock(g_readLock);
        lists.push_back(&g_rewrite);
        lists.push_back(&g_pendingReads);
        for (auto list = lists.rbegin(); list != lists.rend(); ++list)
        {
            for (auto read = (*list)->rbegin(); read != (*list)->rend(); ++read)
            {
                WriteGuest(read->address, read->before.data(), read->before.size());
                g_statistics.readsRewound++;
            }
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> ReadRanges()
    {
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        const auto add = [&](const std::vector<Read>& reads) {
            for (const Read& read : reads)
                ranges.emplace_back(read.address, uint32_t(std::min<uint64_t>(uint64_t(read.address) + read.after.size(), 0xFFFFFFFFull)));
        };
        for (const Entry& entry : g_entries)
            add(entry.reads);
        {
            std::lock_guard lock(g_readLock);
            add(g_rewrite);
            add(g_pendingReads);
        }
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<uint32_t, uint32_t>> merged;
        for (const auto& range : ranges)
        {
            if (!merged.empty() && range.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, range.second);
            else
                merged.push_back(range);
        }
        return merged;
    }

    Statistics GetStatistics()
    {
        Statistics statistics = g_statistics;
        if (g_mode.load(std::memory_order_relaxed) != Mode::kOff)
        {
            std::lock_guard lock(g_readLock);
            statistics.reads = g_reads;
            statistics.readBytes = g_readBytes;
            statistics.readsLost = g_readsLost;
        }
        std::string functions;
        char part[48];
        for (const auto& [function, count] : g_counts)
        {
            snprintf(part, sizeof(part), "%ssub_%08X %llu", functions.empty() ? "" : ", ", function, (unsigned long long)count);
            functions += part;
        }
        statistics.functions = functions;
        statistics.calls = g_dump;
        return statistics;
    }
}
