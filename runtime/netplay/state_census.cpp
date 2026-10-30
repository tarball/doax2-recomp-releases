#include "netplay/state_census.h"

#include "config.h"
#include "crash_handler.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/write_tracker.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <windows.h>

namespace netplay::census
{
    namespace
    {
        using tracker::Hex;
        using tracker::kFileRead;
        using tracker::kPageSize;
        using tracker::Write;
        using tracker::Writer;
        using tracker::WriterText;

        struct Settings
        {
            uint64_t first = 0, count = 0;
            Settings()
            {
                if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_STATE_CENSUS"))
                {
                    char* end = nullptr;
                    first = std::max<uint64_t>(strtoull(setting, &end, 10), 1);
                    count = end && *end == ',' ? strtoull(end + 1, nullptr, 10) : 60;
                }
            }
        };

        const Settings& GetSettings()
        {
            static const Settings settings;
            return settings;
        }

        std::atomic<bool> g_active{ false };

        // Results.
        std::map<uint32_t, std::set<Writer>> g_pageWriters;                   // every page written, with its writers
        std::map<std::string, std::vector<uint32_t>> g_categoryFrames;       // category -> pages written in each frame
        uint64_t g_framesCollected = 0;
        std::mutex g_readLock;
        std::map<std::tuple<std::string, uint32_t, std::string>, uint64_t> g_reads; // by source, thread and guest call chain

        const char* Region(uint32_t page)
        {
            const uint32_t address = page * kPageSize;
            if (address >= 0x82000000)
                return "image";
            if (address >= 0x70000000)
                return "stacks";
            return "heap";
        }

        // A frame's first writes into the results.
        void Collect(const std::vector<Write>& writes)
        {
            std::map<std::string, uint32_t> counts;
            for (const Write& write : writes)
            {
                g_pageWriters[write.page].insert(write.writer);
                std::string category = std::string(Region(write.page)) + ", " +
                    (write.writer.thread ? "thread " + Hex(write.writer.thread) : std::string("host thread"));
                if (write.writer.fiber)
                    category += " fiber " + Hex(write.writer.fiber);
                if (write.writer.function == kFileRead)
                    category += " (file reads)";
                counts[category]++;
                counts[std::string("all, ") + Region(write.page)]++;
                counts["all"]++;
            }
            for (auto& [category, frames] : g_categoryFrames)
                frames.push_back(0);
            for (const auto& [category, count] : counts)
            {
                auto& frames = g_categoryFrames[category];
                frames.resize(g_framesCollected + 1, 0);
                frames.back() = count;
            }
            g_framesCollected++;
        }

        FILE* g_reportFile = nullptr;

        __attribute__((format(printf, 1, 2))) void line(const char* format, ...)
        {
            if (!g_reportFile)
                return;
            va_list arguments;
            va_start(arguments, format);
            vfprintf(g_reportFile, format, arguments);
            va_end(arguments);
        }

        void Report(uint64_t lostWrites)
        {
            const std::string path = config::DumpPath("state_census.txt");
            FILE* file = fopen(path.c_str(), "w");
            g_reportFile = file;
            const uint64_t frames = std::max<uint64_t>(g_framesCollected, 1);
            line("State census: frames %llu-%llu, %zu pages written in all (%.1f MB), %llu writes not recorded\n\n",
                (unsigned long long)GetSettings().first, (unsigned long long)(GetSettings().first + g_framesCollected - 1), g_pageWriters.size(),
                g_pageWriters.size() * 4096.0 / (1024 * 1024), (unsigned long long)lostWrites);
            line("Pages written per frame (mean, min, max) and in all, by region and writer (thread start address, fiber):\n");
            std::vector<std::pair<double, std::string>> sorted;
            for (const auto& [category, counts] : g_categoryFrames)
            {
                uint64_t sum = 0;
                for (uint32_t count : counts)
                    sum += count;
                sorted.emplace_back(double(sum) / double(frames), category);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            for (const auto& [mean, category] : sorted)
            {
                const auto& counts = g_categoryFrames[category];
                std::vector<uint32_t> full(counts);
                full.resize(frames, 0);
                const auto [low, high] = std::minmax_element(full.begin(), full.end());
                line("  %8.1f %6u %6u  %s\n", mean, *low, *high, category.c_str());
            }
            line("\nReads of clocks and random numbers (per frame, in all) by thread (start address) and guest call chain, innermost "
                 "first:\n");
            {
                std::lock_guard lock(g_readLock);
                for (const auto& [key, count] : g_reads)
                {
                    const auto& [source, thread, chain] = key;
                    line("  %8.2f %8llu  %s, %s: %s\n", double(count) / double(frames), (unsigned long long)count, source.c_str(),
                        thread ? ("thread " + Hex(thread)).c_str() : "host thread", chain.c_str());
                }
            }
            line("\nPages written, as runs of pages with the same writers:\n");
            for (auto it = g_pageWriters.begin(); it != g_pageWriters.end();)
            {
                auto end = std::next(it);
                uint32_t last = it->first;
                while (end != g_pageWriters.end() && end->first == last + 1 && end->second == it->second)
                    last = (end++)->first;
                line("  %08X-%08X (%u pages):", it->first * kPageSize, (last + 1) * kPageSize - 1, last - it->first + 1);
                for (const Writer& writer : it->second)
                    line(" [%s]", WriterText(writer).c_str());
                line("\n");
                it = end;
            }
            if (file)
                fclose(file);
            g_reportFile = nullptr;
            auto mean = [&](const char* category) {
                uint64_t sum = 0;
                if (const auto it = g_categoryFrames.find(category); it != g_categoryFrames.end())
                {
                    for (uint32_t count : it->second)
                        sum += count;
                }
                return double(sum) / double(frames);
            };
            LOG_INFO("State census: %llu frames, %zu pages written in all (%.1f MB), per frame: %.1f pages (heap %.1f, stacks %.1f, image %.1f); "
                     "details in %s",
                (unsigned long long)g_framesCollected, g_pageWriters.size(), g_pageWriters.size() * 4096.0 / (1024 * 1024), mean("all"),
                mean("all, heap"), mean("all, stacks"), mean("all, image"), path.c_str());
        }
    }

    void OnFrame(uint64_t frame)
    {
        const Settings& settings = GetSettings();
        if (!settings.count || frame < settings.first)
            return;
        if (frame == settings.first)
        {
            if (tracker::Active())
            {
                LOG_WARN("State census: the write tracker is busy (a sync test?), so no census");
                return;
            }
            tracker::Start();
            g_active.store(true, std::memory_order_release);
            LOG_INFO("State census: from frame %llu, for %llu frames", (unsigned long long)frame, (unsigned long long)settings.count);
            return;
        }
        if (!g_active.load(std::memory_order_acquire))
            return;
        if (frame >= settings.first + settings.count)
        {
            g_active.store(false, std::memory_order_release);
            const uint64_t lost = tracker::LostWrites();
            Collect(tracker::Stop());
            Report(lost);
            return;
        }
        Collect(tracker::NextFrame());
    }

    void NoteRead(const char* source, uint64_t count)
    {
        uint32_t thread = 0;
        if (const kernel::GuestThread* guestThread = kernel::CurrentThread())
            thread = guestThread->StartAddress() ? guestThread->StartAddress() : 0x80000000u | guestThread->ThreadId();
        // The guest functions on the host stack, innermost first (four at most).
        void* frames[32];
        const USHORT depth = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
        std::string chain;
        int found = 0;
        for (USHORT i = 0; i < depth && found < 4; i++)
        {
            if (const uint32_t function = crash::GuestFunctionAt(reinterpret_cast<uintptr_t>(frames[i])))
                chain += (found++ ? " <- sub_" : "sub_") + Hex(function);
        }
        std::lock_guard lock(g_readLock);
        g_reads[{ source, thread, chain }] += count;
    }

    bool Active()
    {
        return g_active.load(std::memory_order_relaxed);
    }
}
