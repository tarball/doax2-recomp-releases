#include "netplay/frame.h"

#include "gpu/command_processor.h"
#include "guest_abi.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/game_state.h"
#include "netplay/heaps.h"
#include "netplay/page_log.h"
#include "netplay/replay.h"
#include "netplay/rollback_test.h"
#include "netplay/savestates.h"
#include "netplay/session.h"
#include "netplay/state_census.h"
#include "netplay/sync_test.h"
#include "util/memory_trace.h"

#include "config.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <immintrin.h>
#include <windows.h>

namespace netplay
{
    namespace
    {
        constexpr auto kThreadWaitLimit = std::chrono::milliseconds(50);
        constexpr auto kAsyncWorkLimit = std::chrono::seconds(10);
        std::atomic<uint64_t> g_frame{ 0 };
        std::atomic<uint64_t> g_boundaries{ 0 };
        ThreadWaits g_threadWaits;                     // main thread
        // The current segment, the fights and menu screens started so far, and whether the game is in its multiplayer
        // flow (main thread), and the current fight's frame, or ~0 outside one, which the game's worker threads read too.
        std::optional<Segment> g_segment;
        uint32_t g_fights = 0, g_screens = 0;
        bool g_flow = false;
        std::atomic<uint64_t> g_fightFrame{ ~0ull };
        PPCContext* g_boundaryContext = nullptr;      // during a boundary (main thread)
        // The main thread's work a frame (LastFrameWork): the last boundary's end, the main thread's waits for the GPU
        // then, whether the frame after it was a replay's, and the last frame's work (main thread).
        std::chrono::steady_clock::time_point g_boundaryEnd{};
        uint64_t g_waitedAtEnd = 0;
        bool g_replayedFrame = false;
        FrameWork g_lastWork;

        // A game thread other than the calling one that isn't blocked in a wait, if any.
        struct RunningThread
        {
            uint32_t start = 0, context = 0;
        };
        std::optional<RunningThread> RunningGameThread(const GameState& game)
        {
            const kernel::GuestThread* self = kernel::CurrentThread();
            std::optional<RunningThread> running;
            kernel::ForEachThread([&](kernel::GuestThread& thread) {
                if (!running && &thread != self && game.isGameThread(thread.StartAddress()) && thread.waits.load(std::memory_order_acquire) == 0)
                    running = RunningThread{ thread.StartAddress(), thread.StartContext() };
            });
            return running;
        }

        // In a session, before the boundary: waits until the game's other threads are blocked in their waits (frame.h).
        void WaitForGameThreads()
        {
            const GameState* game = GetGameState();
            if (!game || !game->isGameThread)
                return;
            std::optional<RunningThread> running = RunningGameThread(*game);
            if (!running)
                return;
            using Clock = std::chrono::steady_clock;
            const Clock::time_point begin = Clock::now();
            for (uint32_t turn = 0; running && Clock::now() - begin < kThreadWaitLimit; turn++)
            {
                if (turn < 64)
                    SwitchToThread();
                else
                    Sleep(1);
                running = RunningGameThread(*game);
            }
            const uint64_t waited = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count());
            g_threadWaits.boundaries++;
            g_threadWaits.microseconds += waited;
            g_threadWaits.longest = std::max(g_threadWaits.longest, waited);
            static bool logged = false;
            if (running && !logged)
            {
                logged = true;
                LOG_WARN("Netplay: the game's thread %08X (context %08X) was still running %.1f ms into frame %llu's boundary, which "
                         "went on without it (logged once)", running->start, running->context, waited / 1000.0,
                    (unsigned long long)g_frame.load(std::memory_order_relaxed) + 1);
            }
        }

        // In a session, before the boundary: the game's asynchronous work done (GameState::asyncWorkRunning), at most
        // kAsyncWorkLimit; each wait is logged.
        void WaitForAsyncWork()
        {
            const GameState* game = GetGameState();
            if (!game || !game->asyncWorkRunning || !game->asyncWorkRunning())
                return;
            using Clock = std::chrono::steady_clock;
            const Clock::time_point begin = Clock::now();
            bool running = true;
            while (running && Clock::now() - begin < kAsyncWorkLimit)
            {
                Sleep(1);
                running = game->asyncWorkRunning();
            }
            const double waited = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            if (running)
                LOG_WARN("Netplay: frame %llu's boundary waited %.0f ms for the game's asynchronous work, which still runs; it went on without it",
                    (unsigned long long)g_frame.load(std::memory_order_relaxed) + 1, waited);
            else
                LOG_INFO("Netplay: frame %llu's boundary waited %.1f ms for the game's asynchronous work to finish",
                    (unsigned long long)g_frame.load(std::memory_order_relaxed) + 1, waited);
        }

        // In a session, at the boundary: MXCSR as the main thread's cached FPSCR says (frame.h), but for the flags.
        void CheckFloatModes()
        {
            const kernel::GuestThread* thread = kernel::CurrentThread();
            PPCContext* ctx = thread ? thread->Context() : nullptr;
            if (!ctx)
                return;
            const uint32_t actual = _mm_getcsr();
            if ((actual & ~0x3Fu) == (ctx->fpscr.csr & ~0x3Fu))
                return;
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LOG_WARN("Netplay: MXCSR was %08X at frame %llu's boundary, where the game's code expects %08X: changed by code outside "
                         "the game's, and restored (logged once)", actual, (unsigned long long)g_frame.load(std::memory_order_relaxed) + 1,
                    ctx->fpscr.csr & ~0x3Fu);
            }
            _mm_setcsr(ctx->fpscr.csr);
        }

        // A switch that a netplay session turns on whatever its variable says, and a sync test unless it says otherwise.
        bool SessionSwitch(const char* name)
        {
            const char* setting = std::getenv(name);
            if (session::Configured())
            {
                if (setting && std::atoi(setting) == 0)
                    LOG_WARN("Netplay: %s=%s is ignored: a netplay session always has it on", name, setting);
                return true;
            }
            if (setting)
                return std::atoi(setting) != 0;
            return synctest::Enabled() || rollbacktest::Enabled();
        }

        // Each fight's setup in the log as it starts.
        void LogSetup()
        {
            std::string text;
            for (const StatePart& part : FightSetup())
                text += std::string(text.empty() ? " " : "; ") + part.name + " " + PartText(part);
            if (!text.empty())
                LOG_INFO("Fight setup:%s", text.c_str());
        }

        // DEBUG_FIGHT_CHECKSUMS=first,count: the first fight's checksums from its frame `first`, for `count` frames: a
        // fight frame's checksum (a fight frame past `first + count` closes the file).
        void WriteChecksum(uint64_t fightFrame, std::optional<uint64_t> checksum)
        {
            static const auto [first, count] = [] {
                uint64_t first = 0, count = 0;
                if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_FIGHT_CHECKSUMS"))
                {
                    char* end = nullptr;
                    first = strtoull(setting, &end, 10);
                    count = end && *end == ',' ? strtoull(end + 1, nullptr, 10) : 600;
                }
                return std::pair{ first, count };
            }();
            static FILE* file = nullptr;
            static bool done = false;
            if (!count || done || fightFrame < first)
                return;
            if (!file)
            {
                const std::string path = config::DumpPath("fight_checksums.txt");
                file = fopen(path.c_str(), "w");
                if (!file)
                {
                    LOG_WARN("Fight checksums: can't write %s", path.c_str());
                    done = true;
                    return;
                }
                LOG_INFO("Fight checksums: from fight frame %llu, for %llu frames, in %s", (unsigned long long)fightFrame,
                    (unsigned long long)count, path.c_str());
            }
            if (fightFrame >= first + count)
            {
                fclose(file);
                file = nullptr;
                done = true;
                LOG_INFO("Fight checksums: done");
                return;
            }
            if (checksum)
                fprintf(file, "%llu %016llX\n", (unsigned long long)fightFrame, (unsigned long long)*checksum);
        }

        // At each boundary in a fight. A session that rolls back writes them itself, once final (LogFightChecksum): a
        // boundary's may be a prediction's, or a replay's.
        void LogChecksum()
        {
            if (session::RollbackConfigured())
                return;
            if (const std::optional<uint64_t> fightFrame = FightFrame())
                WriteChecksum(*fightFrame, FightChecksum());
        }

        // The game enters or leaves its multiplayer flow (frame.h), for the log.
        void SetFlow(bool flow, uint64_t frame)
        {
            if (flow == g_flow)
                return;
            g_flow = flow;
            const GameState* game = GetGameState();
            LOG_INFO("Netplay: the game %s its multiplayer flow (%s) at frame %llu", flow ? "enters" : "leaves",
                game && game->flowName ? game->flowName : "", (unsigned long long)frame);
        }

        // At each boundary, from the game's state: the frame's segment (frame.h). A fight's frames count from its first
        // past loading; a menu screen's from its first that takes input, for as long as the same screen does without a
        // break (a screen loaded again is another segment). A fight belongs to the multiplayer flow if the game was in
        // it as the fight started (it stays in it through the fight); a menu screen if the game says so, and while a
        // screen loads or leaves, the flow is as the game says if it can tell, or else as it was.
        void UpdateSegment(uint64_t frame)
        {
            const GameState* game = GetGameState();
            if (!game || !game->fightState)
                return;
            const GameState::FightState fight = game->fightState();
            const bool fighting = fight == GameState::FightState::kFighting;
            const bool wasFight = g_segment && g_segment->fight;
            if (fighting)
            {
                if (wasFight)
                    g_segment->frame++;
                else
                {
                    g_segment = Segment{ true, g_fights++, 0, 0, g_flow };
                    LOG_INFO("Netplay: a fight's first frame past its loading, frame %llu%s", (unsigned long long)frame,
                        g_flow ? ", in the multiplayer flow" : "");
                    // The random numbers again, whatever the loading drew (GameState::seedFight).
                    if (const std::optional<uint32_t> seed = FightSeed(); seed && game->seedFight && g_boundaryContext)
                        game->seedFight(*g_boundaryContext, guest::g_base, uint64_t(*seed) << 32 ^ 0x46494748545F30ull);
                    LogSetup();
                }
                g_fightFrame.store(g_segment->frame, std::memory_order_relaxed);
                return;
            }
            if (wasFight)
            {
                LOG_INFO("Netplay: the fight ended at frame %llu, its frame %u", (unsigned long long)frame, g_segment->frame + 1);
                g_fightFrame.store(~0ull, std::memory_order_relaxed);
            }
            if (fight == GameState::FightState::kLoading)
            {
                g_segment.reset();
                return; // a fight loads: in the flow if the game was
            }
            const GameState::MenuState menu = game->menuState ? game->menuState() : GameState::MenuState{};
            if (menu.kind == GameState::MenuState::kLocal)
                SetFlow(false, frame);
            else if (menu.kind == GameState::MenuState::kReady || menu.flowKnown)
                SetFlow(menu.flow, frame);
            if (menu.kind != GameState::MenuState::kReady)
            {
                g_segment.reset();
                return;
            }
            if (g_segment && !g_segment->fight && g_segment->screen == menu.screen)
                g_segment->frame++;
            else
            {
                g_segment = Segment{ false, g_screens++, 0, menu.screen, g_flow };
                LOG_INFO("Netplay: menu screen %u (%03X): its first frame past its loading, frame %llu%s", g_segment->number, menu.screen,
                    (unsigned long long)frame, g_flow ? ", in the multiplayer flow" : "");
            }
        }
    }

    void FrameBoundary(PPCContext& ctx)
    {
        // The frame that just ran: its work, without the main thread's waits for the GPU.
        {
            const auto now = std::chrono::steady_clock::now();
            const uint64_t waited = gpu::ThreadWaitedMicroseconds();
            g_lastWork = FrameWork{};
            if (g_boundaryEnd != std::chrono::steady_clock::time_point{})
            {
                const int64_t wall = std::chrono::duration_cast<std::chrono::microseconds>(now - g_boundaryEnd).count();
                g_lastWork.known = true;
                g_lastWork.replayed = g_replayedFrame;
                g_lastWork.microseconds = uint64_t(std::max<int64_t>(wall - int64_t(waited - g_waitedAtEnd), 0));
            }
        }
        g_boundaryContext = &ctx;
        // A session's (and a rollback test's, which must play as a session would: its runs compare by fight frame).
        if (session::Configured() || rollbacktest::Enabled())
        {
            WaitForGameThreads();
            WaitForAsyncWork();
            CheckFloatModes();
        }
        g_boundaries.fetch_add(1, std::memory_order_relaxed);
        const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        UpdateSegment(frame);
        session::OnFrame(frame);
        LogChecksum();
        debug::MemoryTraceOnFrame(frame);
        census::OnFrame(frame);
        pagelog::CensusOnFrame(frame);
        heaps::OnFrame(frame);
        // Last, so that the boundary's state is the game's as its frame begins: savestates, then the sync test, which
        // may load one (and makes this boundary another frame's).
        savestates::OnFrame(frame);
        synctest::OnFrame(frame);
        rollbacktest::OnFrame(frame);
        // A session's rollback (which may load a savestate too) and the pads of the frame about to run.
        session::AfterSavestates(frame);
        g_boundaryContext = nullptr;
        g_replayedFrame = replay::Active();
        g_waitedAtEnd = gpu::ThreadWaitedMicroseconds();
        g_boundaryEnd = std::chrono::steady_clock::now();
    }

    FrameWork LastFrameWork()
    {
        return g_lastWork;
    }

    void LogFightChecksum(uint64_t fightFrame, uint64_t checksum)
    {
        WriteChecksum(fightFrame, checksum);
    }

    PPCContext* BoundaryContext()
    {
        return g_boundaryContext;
    }

    void AfterUpdate()
    {
        synctest::AfterUpdate();
    }

    uint64_t Frame()
    {
        return g_frame.load(std::memory_order_relaxed);
    }

    uint64_t Boundaries()
    {
        return g_boundaries.load(std::memory_order_relaxed);
    }

    ThreadWaits GameThreadWaits()
    {
        return g_threadWaits;
    }

    void SetFrame(uint64_t frame)
    {
        // The segment's frames go back as far (a savestate is saved and loaded within one fight).
        const uint64_t back = g_frame.load(std::memory_order_relaxed) - frame;
        if (g_segment && g_segment->frame >= back)
        {
            g_segment->frame -= uint32_t(back);
            if (g_segment->fight)
                g_fightFrame.store(g_segment->frame, std::memory_order_relaxed);
        }
        g_frame.store(frame, std::memory_order_relaxed);
    }

    std::optional<uint64_t> FightFrame()
    {
        const uint64_t frame = g_fightFrame.load(std::memory_order_relaxed);
        if (frame == ~0ull)
            return std::nullopt;
        return frame;
    }

    std::optional<Segment> CurrentSegment()
    {
        return g_segment;
    }

    bool InFlow()
    {
        return g_flow;
    }

    std::optional<uint64_t> FightChecksum()
    {
        const GameState* game = GetGameState();
        if (!game || !game->fightChecksum || !FightFrame())
            return std::nullopt;
        return game->fightChecksum();
    }

    bool FixedFrameTime()
    {
        static const bool fixed = SessionSwitch(GAME_PREFIX "_FIXED_FRAME_TIME");
        return fixed;
    }

    bool SplitRandom()
    {
        static const bool split = SessionSwitch(GAME_PREFIX "_SPLIT_RANDOM");
        return split;
    }

    bool NoVoiceWaits()
    {
        static const bool on = SessionSwitch(GAME_PREFIX "_NO_VOICE_WAITS");
        return on;
    }

    std::vector<StatePart> FightSetup()
    {
        const GameState* game = GetGameState();
        if (!game || !game->fightSetup)
            return {};
        return game->fightSetup();
    }

    uint64_t PartHash(const StatePart& part)
    {
        return XXH3_64bits(part.bytes.data(), part.bytes.size());
    }

    std::string PartText(const StatePart& part)
    {
        char text[24];
        if (part.bytes.size() > kShownBytes)
        {
            snprintf(text, sizeof(text), "#%016llX", (unsigned long long)PartHash(part));
            return text;
        }
        std::string result;
        for (const uint8_t byte : part.bytes)
        {
            snprintf(text, sizeof(text), "%02X", byte);
            result += text;
        }
        return result.empty() ? "-" : result;
    }

    std::optional<uint32_t> FightSeed()
    {
        if (const std::optional<uint32_t> seed = session::Seed())
            return seed;
        static const std::optional<uint32_t> seed = []() -> std::optional<uint32_t> {
            if (const char* setting = std::getenv(GAME_PREFIX "_FIGHT_SEED"))
                return uint32_t(strtoul(setting, nullptr, 0));
            return std::nullopt;
        }();
        return seed;
    }
}
