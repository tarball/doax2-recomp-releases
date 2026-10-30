#include "netplay/savestates.h"

#include "config.h"
#include "gpu/command_processor.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/game_state.h"
#include "netplay/page_log.h"
#include "netplay/replay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace netplay::savestates
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct Saved
        {
            uint64_t frame = 0;
            HostState host;
            std::string error; // why it holds no host state (a fiber running at the boundary)
        };

        bool g_active = false;
        uint32_t g_frames = 0;
        std::deque<Saved> g_saved; // the boundaries kept, oldest first; the last is the current frame's

        // Statistics: in all, and over the window since the last log line.
        struct Totals
        {
            uint64_t saves = 0;
            double boundaryUs = 0, boundaryMaxUs = 0, hostUs = 0;
            uint64_t pages = 0, copyCycles = 0;
        };
        Totals g_totals, g_window;
        pagelog::Statistics g_logAtWindow;
        uint64_t g_loads = 0;
        double g_loadUs = 0, g_hostLoadUs = 0;
        size_t g_loadPages = 0;
        double g_drawWaitUs = 0, g_drawWaitTotalUs = 0, g_drawWaitMaxUs = 0;
        uint64_t g_drawWaits = 0;

        // DEBUG_LOAD_NO_DRAW_WAIT=1 (diagnostics): a load doesn't wait for the command processor's draws (Load).
        bool WaitsForDraws()
        {
            static const bool wait = [] {
                const char* setting = std::getenv(GAME_PREFIX "_DEBUG_LOAD_NO_DRAW_WAIT");
                return !setting || std::atoi(setting) == 0;
            }();
            return wait;
        }

        double Microseconds(Clock::duration duration)
        {
            return std::chrono::duration<double, std::micro>(duration).count();
        }

        // SAVESTATES=frames: savestates from the first boundary on (diagnostics).
        uint32_t ConfiguredFrames()
        {
            static const uint32_t frames = [] {
                const char* setting = std::getenv(GAME_PREFIX "_SAVESTATES");
                return setting ? std::max(uint32_t(std::atoi(setting)), 1u) : 0u;
            }();
            return frames;
        }

        // DEBUG_PAGE_LOG=first[,count] (diagnostics): each frame's pages the page log copied, as runs, for `count` frames
        // (default 60) from game frame `first`, to dumps/page_log.txt.
        void DumpFramePages(uint64_t frame)
        {
            static const auto [first, count] = [] {
                uint64_t first = 0, count = 0;
                if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_PAGE_LOG"))
                {
                    char* end = nullptr;
                    first = strtoull(setting, &end, 10);
                    count = end && *end == ',' ? strtoull(end + 1, nullptr, 10) : 60;
                }
                return std::pair{ first, count };
            }();
            static FILE* file = nullptr;
            if (!count || frame <= first || frame > first + count)
                return;
            if (!file)
            {
                file = fopen(config::DumpPath("page_log.txt").c_str(), "w");
                if (!file)
                    return;
                fprintf(file, "Each frame's pages the page log copied (as they were at the frame's start), as runs of pages:\n");
            }
            const auto frames = pagelog::FramePages();
            if (frames.empty())
                return;
            std::vector<uint32_t> pages = frames.back().second;
            std::sort(pages.begin(), pages.end());
            fprintf(file, "frame %llu: %zu pages:", (unsigned long long)frames.back().first, pages.size());
            for (size_t i = 0; i < pages.size();)
            {
                size_t j = i + 1;
                while (j < pages.size() && pages[j] == pages[j - 1] + 1)
                    j++;
                fprintf(file, " %08X+%zu", pages[i] << 12, j - i);
                i = j;
            }
            fprintf(file, "\n");
            if (frame == first + count)
            {
                fclose(file);
                file = nullptr;
                LOG_INFO("Page log: frames %llu-%llu's pages in %s", (unsigned long long)first, (unsigned long long)(first + count - 1),
                    config::DumpPath("page_log.txt").c_str());
            }
        }

        // The game's memory to watch, as page ranges.
        pagelog::Watched Watched(const GameState& game)
        {
            pagelog::Watched watched;
            if (!game.savedMemory)
                return watched;
            const GameState::SavedMemory memory = game.savedMemory();
            watched.ranges.reserve(memory.ranges.size());
            for (const auto& [start, end] : memory.ranges)
            {
                if (end > start)
                    watched.ranges.emplace_back(start >> 12, uint32_t((uint64_t(end) + 0xFFF) >> 12));
            }
            for (const auto& [page, mask] : memory.partial)
                watched.masked.emplace_back(page >> 12, mask);
            return watched;
        }

        Saved SaveHost(const GameState& game, uint64_t frame)
        {
            Saved saved;
            saved.frame = frame;
            if (game.saveHost)
            {
                saved.host = game.saveHost(saved.error);
                if (!saved.host.load && saved.error.empty())
                    saved.error = "no host state";
            }
            return saved;
        }

        void LogWindow()
        {
            const pagelog::Statistics log = pagelog::GetStatistics();
            const uint64_t saves = g_window.saves;
            if (!saves)
                return;
            const uint64_t pages = log.pages - g_logAtWindow.pages;
            const uint64_t misses = log.misses - g_logAtWindow.misses;
            const double copyUs = log.copyMicroseconds - g_logAtWindow.copyMicroseconds;
            std::string regions;
            for (int region = 0; region < pagelog::Statistics::kRegionCount; region++)
            {
                char part[64];
                snprintf(part, sizeof(part), "%s%s %.1f", regions.empty() ? "" : ", ", pagelog::kRegionNames[region],
                    double(log.regionPages[region] - g_logAtWindow.regionPages[region]) / double(saves));
                regions += part;
            }
            LOG_INFO("Savestates: %llu frames saved: %.1f us a frame at the boundary (at most %.1f; the host state %.1f), %.1f pages a frame copied "
                     "as first written (%s; %.1f us a frame, all threads), %.1f store map misses a frame; %u pages watched, the logs hold %.1f MB",
                (unsigned long long)saves, g_window.boundaryUs / double(saves), g_window.boundaryMaxUs, g_window.hostUs / double(saves),
                double(pages) / double(saves), regions.c_str(), copyUs / double(saves), double(misses) / double(saves), log.watchedPages,
                log.bytesHeld / (1024.0 * 1024));
            g_window = Totals{};
            g_logAtWindow = log;
        }
    }

    void Start(uint32_t frames)
    {
        const GameState* game = GetGameState();
        if (!game || !game->savedMemory)
        {
            LOG_ERROR("Savestates: the game doesn't describe its memory (GameState::savedMemory)");
            return;
        }
        g_frames = std::max(frames, 1u);
        g_saved.clear();
        pagelog::Start(Frame(), g_frames, Watched(*game));
        g_saved.push_back(SaveHost(*game, Frame()));
        g_active = true;
        g_window = Totals{};
        g_logAtWindow = pagelog::GetStatistics();
        LOG_INFO("Savestates: saving every frame from frame %llu, keeping %u", (unsigned long long)Frame(), g_frames);
    }

    void Stop()
    {
        if (!g_active)
            return;
        LogWindow();
        pagelog::Stop();
        g_saved.clear();
        g_active = false;
    }

    bool Active()
    {
        return g_active;
    }

    void OnFrame(uint64_t frame)
    {
        const GameState* game = GetGameState();
        if (!g_active)
        {
            if (const uint32_t frames = ConfiguredFrames(); frames && game && game->savedMemory)
                Start(frames);
            return;
        }
        const Clock::time_point start = Clock::now();
        pagelog::Boundary(frame, Watched(*game));
        const Clock::time_point sealed = Clock::now();
        DumpFramePages(frame);
        g_saved.push_back(SaveHost(*game, frame));
        while (g_saved.size() > g_frames + 1)
            g_saved.pop_front();
        // The journal's calls before the oldest boundary kept are past rolling back: its releases among them are made.
        journal::OnFrame(frame, g_saved.front().frame);
        const Clock::time_point end = Clock::now();
        const double us = Microseconds(end - start), hostUs = Microseconds(end - sealed);
        for (Totals* totals : { &g_totals, &g_window })
        {
            totals->saves++;
            totals->boundaryUs += us;
            totals->boundaryMaxUs = std::max(totals->boundaryMaxUs, us);
            totals->hostUs += hostUs;
        }
        if (ConfiguredFrames() && g_window.saves >= 600)
            LogWindow();
    }

    bool CanLoad(uint64_t frame)
    {
        if (!g_active || !pagelog::CanLoad(frame))
            return false;
        return std::any_of(g_saved.begin(), g_saved.end(), [&](const Saved& saved) { return saved.frame == frame && saved.host.load; });
    }

    std::string Load(uint64_t frame, std::string* note)
    {
        if (!g_active)
            return "no savestates";
        const auto it = std::find_if(g_saved.begin(), g_saved.end(), [&](const Saved& saved) { return saved.frame == frame; });
        if (it == g_saved.end())
            return "frame " + std::to_string(frame) + "'s boundary isn't kept";
        if (!it->host.load)
            return "frame " + std::to_string(frame) + "'s boundary has no host state: " + it->error;
        if (!pagelog::CanLoad(frame))
            return "the page log can't go back to frame " + std::to_string(frame);
        // The frames recorded drawn first: their draws read what the load puts back (savestates.h). Two frames at most.
        g_drawWaitUs = WaitsForDraws() ? double(gpu::WaitForRecordedDraws(33333)) : 0.0;
        g_drawWaitTotalUs += g_drawWaitUs;
        g_drawWaitMaxUs = std::max(g_drawWaitMaxUs, g_drawWaitUs);
        if (g_drawWaitUs >= 1)
            g_drawWaits++;
        const Clock::time_point start = Clock::now();
        // The host state first: it reads guest memory as it is (where the fibers are suspended now).
        if (std::string error = it->host.load(note); !error.empty())
            return error;
        const Clock::time_point hostLoaded = Clock::now();
        g_loadPages = pagelog::Load(frame);
        // The boundaries after it go; it's the current one again.
        g_saved.erase(std::next(it), g_saved.end());
        SetFrame(frame);
        const Clock::time_point end = Clock::now();
        g_loads++;
        g_loadUs = Microseconds(end - start);
        g_hostLoadUs = Microseconds(hostLoaded - start);
        return {};
    }

    Statistics GetStatistics()
    {
        Statistics statistics;
        const pagelog::Statistics log = pagelog::GetStatistics();
        statistics.saves = g_totals.saves;
        if (g_totals.saves)
        {
            statistics.boundaryUs = g_totals.boundaryUs / double(g_totals.saves);
            statistics.hostUs = g_totals.hostUs / double(g_totals.saves);
            statistics.copyUs = log.copyMicroseconds / double(g_totals.saves);
            statistics.pages = double(log.pages) / double(g_totals.saves);
        }
        statistics.boundaryMaxUs = g_totals.boundaryMaxUs;
        statistics.loads = g_loads;
        statistics.loadUs = g_loadUs;
        statistics.loadPages = g_loadPages;
        statistics.hostLoadUs = g_hostLoadUs;
        statistics.drawWaitUs = g_drawWaitUs;
        statistics.drawWaitTotalUs = g_drawWaitTotalUs;
        statistics.drawWaitMaxUs = g_drawWaitMaxUs;
        statistics.drawWaits = g_drawWaits;
        return statistics;
    }

    std::string StatisticsText()
    {
        const Statistics s = GetStatistics();
        char text[384];
        snprintf(text, sizeof(text),
            "%llu frames saved: %.1f us a frame at the boundary (at most %.1f; the host state %.1f), %.1f pages a frame copied as first written "
            "(%.1f us a frame, all threads); %llu loads, the last %.1f us (%zu pages, the host state %.1f us)",
            (unsigned long long)s.saves, s.boundaryUs, s.boundaryMaxUs, s.hostUs, s.pages, s.copyUs, (unsigned long long)s.loads, s.loadUs,
            s.loadPages, s.hostLoadUs);
        return text;
    }
}
