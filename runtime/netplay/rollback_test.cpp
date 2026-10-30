#include "netplay/rollback_test.h"

#include "config.h"
#include "gpu/command_processor.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/game_state.h"
#include "netplay/heaps.h"
#include "netplay/replay.h"
#include "netplay/savestates.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace netplay::rollbacktest
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct Settings
        {
            bool set = false;
            uint32_t latency = 0; // k
            uint64_t first = 0, last = ~0ull;
            uint32_t remote = 1;  // the remote player's game user (DEBUG_ROLLBACK_REMOTE=1 or 2, the player)
            std::string reference;
            // DEBUG_ROLLBACK_SAVESTATES=0 with k = 0: neither savestates nor the journal, the fight as the game plays it
            // without rollback's machinery (which must change nothing in it, its pictures included).
            bool savestates = true;
            Settings()
            {
                const char* setting = std::getenv(GAME_PREFIX "_DEBUG_ROLLBACK");
                if (!setting)
                    return;
                set = true;
                char* end = nullptr;
                latency = uint32_t(std::min<unsigned long>(strtoul(setting, &end, 10), 64));
                if (end && *end == ',')
                    first = strtoull(end + 1, &end, 10);
                if (end && *end == ',')
                    last = strtoull(end + 1, &end, 10);
                if (const char* path = std::getenv(GAME_PREFIX "_DEBUG_ROLLBACK_REFERENCE"))
                    reference = path;
                if (const char* player = std::getenv(GAME_PREFIX "_DEBUG_ROLLBACK_REMOTE"))
                    remote = std::atoi(player) == 1 ? 0 : 1;
                if (const char* saving = std::getenv(GAME_PREFIX "_DEBUG_ROLLBACK_SAVESTATES"); saving && std::atoi(saving) == 0 && !latency)
                    savestates = false;
            }
        };

        const Settings& GetSettings()
        {
            static const Settings settings;
            return settings;
        }

        uint16_t (*g_scripts)(int script, uint64_t fightFrame) = nullptr;

        uint16_t Script(int script, uint64_t fightFrame)
        {
            return g_scripts ? g_scripts(script, fightFrame) : 0;
        }

        struct Rollback
        {
            uint64_t fightFrame = 0; // the frame rolled back to
            double drawWaitUs = 0;   // the wait for the command processor's draws before the load
            double loadUs = 0;
            size_t pages = 0;
            double replayMs = 0;     // the replayed frames, from the load to the present's boundary
        };

        struct State
        {
            enum class Phase
            {
                kWaiting, // for the first fight's frame `first`
                kRunning,
                kDone,
            } phase = Phase::kWaiting;
            uint32_t fight = 0;            // the segment number of the fight tested
            bool startedSavestates = false;
            int64_t known = -1;            // the last fight frame whose remote input is known
            std::map<uint64_t, uint16_t> used;      // fight frame -> player 2's buttons it last ran with
            std::map<uint64_t, uint64_t> checksums; // fight frame -> checksum at its boundary, the latest timeline's
            int64_t emitted = -1;                   // the last fight frame whose final checksum was written
            // A replay under way: the game frame of the boundary it started at (the present), and when.
            bool replaying = false;
            uint64_t replayEnd = 0;
            Clock::time_point replayStart;
            std::vector<Rollback> rollbacks;
            uint64_t framesReplayed = 0;
            // The reference's checksums, and how ours compared.
            std::map<uint64_t, uint64_t> reference;
            uint64_t compared = 0, differing = 0;
            int64_t firstDiffering = -1;
            FILE* checksumFile = nullptr;
            FILE* soundFile = nullptr;
            FILE* predictedFile = nullptr; // the frames shown on a wrong prediction
            uint64_t shownExactly = 0, shownPredicted = 0;
            uint64_t lastReport = 0;
            // The frames that ran as the present (not replays): how many, the vblank count as the first and the last
            // started (each vblank past one a frame is a frame shown twice: a rollback that didn't fit in a frame's
            // time), and the longest gap between two.
            Clock::time_point lastPresent;
            bool presented = false;
            uint64_t presentFrames = 0, firstVblank = 0, lastVblank = 0;
            double longestMs = 0;
            std::string error;
            heaps::GameMemoryChangeCounts memoryAtStart;
        };

        State g_state;

        void LoadReference(State& state, const std::string& path)
        {
            std::ifstream file(path);
            if (!file)
            {
                LOG_WARN("Rollback test: can't read the reference %s", path.c_str());
                return;
            }
            std::string line;
            while (std::getline(file, line))
            {
                std::istringstream fields(line);
                uint64_t frame = 0;
                std::string checksum;
                if (fields >> frame >> checksum && checksum.size() == 16)
                    state.reference[frame] = strtoull(checksum.c_str(), nullptr, 16);
            }
            LOG_INFO("Rollback test: %zu reference checksums from %s", state.reference.size(), path.c_str());
        }

        // The prediction for a frame whose remote input isn't known: the prediction script's press, or the last input known.
        uint16_t Predicted(uint64_t fightFrame, int64_t known)
        {
            if (const uint16_t predicted = Script(2, fightFrame))
                return predicted;
            return known >= 0 ? Script(int(GetSettings().remote), uint64_t(known)) : 0;
        }

        // The fight frames whose checksums are final (`upTo` and those before), written and compared once each.
        void Emit(State& state, int64_t upTo)
        {
            for (int64_t frame = std::max<int64_t>(state.emitted + 1, 0); frame <= upTo; frame++)
            {
                state.emitted = frame;
                const auto it = state.checksums.find(uint64_t(frame));
                if (it == state.checksums.end())
                    continue;
                if (state.checksumFile)
                    fprintf(state.checksumFile, "%lld %016llX\n", (long long)frame, (unsigned long long)it->second);
                if (const auto reference = state.reference.find(uint64_t(frame)); reference != state.reference.end())
                {
                    state.compared++;
                    if (reference->second != it->second)
                    {
                        if (!state.differing++)
                        {
                            state.firstDiffering = frame;
                            LOG_WARN("Rollback test: the fight's checksum differs from the reference's at fight frame %lld", (long long)frame);
                        }
                    }
                }
            }
            // Forget what's past rolling back.
            while (!state.checksums.empty() && int64_t(state.checksums.begin()->first) < state.emitted - 64)
                state.checksums.erase(state.checksums.begin());
            while (!state.used.empty() && int64_t(state.used.begin()->first) < state.known - 64)
                state.used.erase(state.used.begin());
        }

        // Whether fight frame `f`, about to run as the present, is shown as the reference shows it: whether the remote
        // inputs it runs on that aren't known yet were predicted right (the frames since the last known ran with their
        // real inputs, and its own prediction is). Otherwise it's shown on a wrong prediction, as GGPO shows a frame until
        // its rollback: its picture may differ from the reference's (tools\netplay\rollback.ps1 -Frames leaves it out).
        bool ShownExactly(const State& state, uint64_t f)
        {
            const Settings& settings = GetSettings();
            for (int64_t c = std::max<int64_t>(state.known + 1, int64_t(settings.first)); c <= int64_t(f); c++)
            {
                const uint16_t real = Script(int(settings.remote), uint64_t(c));
                uint16_t ran = real;
                if (c == int64_t(f))
                    ran = Predicted(f, state.known);
                else if (const auto used = state.used.find(uint64_t(c)); used != state.used.end())
                    ran = used->second;
                if (ran != real)
                    return false;
            }
            return true;
        }

        // A frame starts as the present (after a rollback's replay, or with none): fight frame `f`.
        void Present(State& state, uint64_t f)
        {
            if (ShownExactly(state, f))
                state.shownExactly++;
            else
            {
                state.shownPredicted++;
                if (state.predictedFile)
                    fprintf(state.predictedFile, "%llu\n", (unsigned long long)f);
            }
            const Clock::time_point now = Clock::now();
            const uint64_t vblank = gpu::GetStatistics().vblanks.load(std::memory_order_relaxed);
            if (state.presented)
            {
                const double ms = std::chrono::duration<double, std::milli>(now - state.lastPresent).count();
                state.presentFrames++;
                state.longestMs = std::max(state.longestMs, ms);
            }
            else
                state.firstVblank = vblank;
            state.lastVblank = vblank;
            state.lastPresent = now;
            state.presented = true;
        }

        void WriteSounds(State& state, uint64_t fightFrame)
        {
            const GameState* game = GetGameState();
            if (!state.soundFile || !game || !game->soundsPlaying)
                return;
            std::vector<uint32_t> sounds = game->soundsPlaying();
            fprintf(state.soundFile, "%llu", (unsigned long long)fightFrame);
            for (uint32_t sound : sounds)
                fprintf(state.soundFile, " %08X", sound);
            fprintf(state.soundFile, "\n");
        }

        std::string Summary(const State& state)
        {
            const journal::Statistics journal = journal::GetStatistics();
            double loadSum = 0, loadMax = 0, replaySum = 0;
            std::vector<double> perFrame;
            for (const Rollback& rollback : state.rollbacks)
            {
                loadSum += rollback.loadUs;
                loadMax = std::max(loadMax, rollback.loadUs);
                replaySum += rollback.replayMs;
                if (GetSettings().latency)
                    perFrame.push_back(rollback.replayMs / GetSettings().latency);
            }
            std::sort(perFrame.begin(), perFrame.end());
            const double p95 = perFrame.empty() ? 0.0 : perFrame[std::min(perFrame.size() - 1, size_t(double(perFrame.size()) * 0.95))];
            const size_t count = state.rollbacks.size();
            char text[1400];
            snprintf(text, sizeof(text),
                "%zu rollbacks of %u frames (%llu frames replayed): loads %.0f us on average (at most %.0f), replays %.2f ms a frame (%.2f at the "
                "95th percentile); the journal answered %llu calls from the record (%llu from another frame's), made %llu the record didn't hold "
                "(%.1f us each, %.1f of it the call's own, against %.1f us each answered), and of the abandoned timelines' calls no replay made, "
                "undid %llu of %llu (%llu queries, with nothing to undo; %llu releases dropped); %llu releases made once past rolling back; final "
                "checksums written to fight frame %lld%s",
                count, GetSettings().latency, (unsigned long long)state.framesReplayed, count ? loadSum / double(count) : 0.0, loadMax,
                state.framesReplayed ? replaySum / double(state.framesReplayed) : 0.0, p95, (unsigned long long)journal.replayed,
                (unsigned long long)journal.replayedNear, (unsigned long long)journal.mismatched,
                journal.mismatched ? journal.madeMicroseconds / double(journal.mismatched) : 0.0,
                journal.mismatched ? journal.madeCallMicroseconds / double(journal.mismatched) : 0.0,
                journal.replayed ? journal.answeredMicroseconds / double(journal.replayed) : 0.0, (unsigned long long)journal.undone,
                (unsigned long long)journal.unreplayed, (unsigned long long)journal.unreplayedQueries, (unsigned long long)journal.releasesDropped,
                (unsigned long long)journal.deferred,
                (long long)state.emitted, "");
            std::string result = text;
            const uint64_t vblanks = state.lastVblank - state.firstVblank;
            snprintf(text, sizeof(text), "; %llu frames took %llu vblanks (%llu frames shown twice; the longest gap %.1f ms)",
                (unsigned long long)state.presentFrames, (unsigned long long)vblanks,
                (unsigned long long)(vblanks > state.presentFrames ? vblanks - state.presentFrames : 0), state.longestMs);
            result += text;
            snprintf(text, sizeof(text), "; %llu frames shown on the inputs as they came, %llu on a wrong prediction",
                (unsigned long long)state.shownExactly, (unsigned long long)state.shownPredicted);
            result += text;
            if (count)
            {
                double waitSum = 0, waitMax = 0;
                size_t waited = 0;
                for (const Rollback& rollback : state.rollbacks)
                {
                    waitSum += rollback.drawWaitUs;
                    waitMax = std::max(waitMax, rollback.drawWaitUs);
                    waited += rollback.drawWaitUs >= 1 ? 1 : 0;
                }
                snprintf(text, sizeof(text), "; the loads waited for the command processor's draws %.0f us on average (at most %.0f; %zu of %zu waited)",
                    waitSum / double(count), waitMax, waited, count);
                result += text;
            }
            if (!GetSettings().savestates)
                result += "; without savestates or the journal";
            if (!state.reference.empty())
            {
                if (!state.differing)
                    snprintf(text, sizeof(text), "; the same as the reference at all %llu frames compared", (unsigned long long)state.compared);
                else
                    snprintf(text, sizeof(text), "; differs from the reference at %llu of %llu frames compared, first at fight frame %lld",
                        (unsigned long long)state.differing, (unsigned long long)state.compared, (long long)state.firstDiffering);
                result += text;
            }
            // The game's memory map changed in the test's frames: a load from before such a change can't put that memory
            // back (netplay/heaps.h).
            const heaps::GameMemoryChangeCounts changes = heaps::GameMemoryChanges();
            if (changes.reserved != state.memoryAtStart.reserved || changes.decommitted != state.memoryAtStart.decommitted ||
                changes.released != state.memoryAtStart.released)
            {
                snprintf(text, sizeof(text), "; the game's memory map changed: %llu regions reserved, %llu decommits, %llu releases",
                    (unsigned long long)(changes.reserved - state.memoryAtStart.reserved),
                    (unsigned long long)(changes.decommitted - state.memoryAtStart.decommitted),
                    (unsigned long long)(changes.released - state.memoryAtStart.released));
                result += text;
            }
            return result;
        }

        void Finish(State& state, const char* why)
        {
            if (state.phase != State::Phase::kRunning)
                return;
            if (state.replaying)
            {
                replay::End();
                state.replaying = false;
            }
            Emit(state, state.known + 1);
            journal::Stop();
            const journal::Statistics journal = journal::GetStatistics();
            const std::string summary = Summary(state);
            if (state.differing || !state.error.empty())
                LOG_WARN("Rollback test (%s): %s%s%s", why, summary.c_str(), state.error.empty() ? "" : "; ", state.error.c_str());
            else
                LOG_INFO("Rollback test (%s): %s", why, summary.c_str());
            if (FILE* file = fopen(config::DumpPath("rollback_test.txt").c_str(), "w"))
            {
                fprintf(file, "Rollback test, %u frames of latency, from fight frame %llu (%s)\n\n%s\n\n", GetSettings().latency,
                    (unsigned long long)GetSettings().first, why, summary.c_str());
                fprintf(file, "Rollbacks (the fight frame loaded, the wait for the command processor's draws, the load's time and pages, the "
                              "replay's time):\n");
                for (const Rollback& rollback : state.rollbacks)
                    fprintf(file, "  %llu: %.0f us, %.0f us, %zu pages, %.2f ms\n", (unsigned long long)rollback.fightFrame, rollback.drawWaitUs,
                        rollback.loadUs, rollback.pages, rollback.replayMs);
                fprintf(file, "\nJournal: %s\n", journal.functions.c_str());
                if (!journal.firstMismatch.empty())
                    fprintf(file, "The first call a replay made that the record didn't hold: %s\n", journal.firstMismatch.c_str());
                if (!journal.firstUnreplayed.empty())
                    fprintf(file, "The first call of an abandoned timeline no replay made: %s\n", journal.firstUnreplayed.c_str());
                fprintf(file, "\nThe record at the end (the last frames' calls, and the abandoned ones):\n%s", journal.calls.c_str());
                fclose(file);
            }
            if (state.checksumFile)
                fclose(state.checksumFile);
            if (state.soundFile)
                fclose(state.soundFile);
            if (state.predictedFile)
                fclose(state.predictedFile);
            state.checksumFile = state.soundFile = state.predictedFile = nullptr;
            if (state.startedSavestates)
                savestates::Stop();
            state.phase = State::Phase::kDone;
        }

        void Start(State& state, uint64_t fightFrame)
        {
            const Settings& settings = GetSettings();
            if (settings.savestates && !savestates::Active())
            {
                savestates::Start(std::max<uint32_t>(settings.latency, 1));
                state.startedSavestates = savestates::Active();
                if (!state.startedSavestates)
                {
                    LOG_ERROR("Rollback test: no savestates");
                    state.phase = State::Phase::kDone;
                    return;
                }
            }
            if (settings.savestates)
                journal::Record();
            state.memoryAtStart = heaps::GameMemoryChanges();
            state.phase = State::Phase::kRunning;
            state.known = int64_t(fightFrame) - 1;
            state.emitted = int64_t(fightFrame) - 1;
            state.checksumFile = fopen(config::DumpPath("rollback_checksums.txt").c_str(), "w");
            state.soundFile = fopen(config::DumpPath("rollback_sounds.txt").c_str(), "w");
            state.predictedFile = fopen(config::DumpPath("rollback_predicted.txt").c_str(), "w");
            if (!settings.reference.empty())
                LoadReference(state, settings.reference);
            LOG_INFO("Rollback test: player %u's input %u frames late from fight frame %llu to %llu, at frame %llu%s", settings.remote + 1,
                settings.latency, (unsigned long long)fightFrame, (unsigned long long)settings.last, (unsigned long long)Frame(),
                settings.savestates ? "" : ", without savestates or the journal");
        }

        void Report(State& state, uint64_t fightFrame)
        {
            if (fightFrame < state.lastReport + 600)
                return;
            state.lastReport = fightFrame;
            LOG_INFO("Rollback test at fight frame %llu: %s", (unsigned long long)fightFrame, Summary(state).c_str());
        }
    }

    bool Enabled()
    {
        return GetSettings().set;
    }

    void SetScripts(uint16_t (*buttons)(int script, uint64_t fightFrame))
    {
        g_scripts = buttons;
    }

    bool PadFor(uint32_t user, uint16_t& buttons, uint32_t& packetNumber)
    {
        const State& state = g_state;
        if (state.phase != State::Phase::kRunning || user > 1)
            return false;
        const std::optional<uint64_t> fightFrame = FightFrame();
        if (!fightFrame)
            return false;
        const uint64_t frame = *fightFrame;
        if (user != GetSettings().remote)
            buttons = Script(int(user), frame);
        else
        {
            buttons = int64_t(frame) <= state.known ? Script(int(user), frame) : Predicted(frame, state.known);
            g_state.used[frame] = buttons;
        }
        // Packet numbers that follow the fight frame: the same in every timeline, and in the reference's run.
        packetNumber = 0x40000000u | uint32_t(frame + 1);
        return true;
    }

    void OnFrame(uint64_t frame)
    {
        const Settings& settings = GetSettings();
        if (!settings.set)
            return;
        State& state = g_state;
        const std::optional<uint64_t> fightFrame = FightFrame();
        const std::optional<Segment> segment = CurrentSegment();
        if (state.phase == State::Phase::kDone)
            return;
        if (state.phase == State::Phase::kWaiting)
        {
            if (!fightFrame || !segment || !segment->fight || *fightFrame < settings.first)
                return;
            state.fight = segment->number;
            Start(state, *fightFrame);
            if (state.phase != State::Phase::kRunning)
                return;
        }
        if (!fightFrame || !segment || !segment->fight || segment->number != state.fight)
        {
            Finish(state, "the fight ended");
            return;
        }
        const uint64_t f = *fightFrame;
        if (const std::optional<uint64_t> checksum = FightChecksum())
            state.checksums[f] = *checksum;
        if (state.replaying)
        {
            if (frame < state.replayEnd)
                return; // a replayed boundary
            // The replay reached the present.
            replay::End();
            journal::FinishReplay();
            state.replaying = false;
            Rollback& rollback = state.rollbacks.back();
            rollback.replayMs = std::chrono::duration<double, std::milli>(Clock::now() - state.replayStart).count();
            Present(state, f);
            Emit(state, std::min<int64_t>(int64_t(f), int64_t(f) - int64_t(settings.latency) + 1));
            WriteSounds(state, f);
            Report(state, f);
            return;
        }
        if (f > settings.last)
        {
            Finish(state, ("its last frame, " + std::to_string(settings.last) + ", passed: fight frame " + std::to_string(f) + " at frame " +
                std::to_string(frame)).c_str());
            return;
        }
        // The remote input of the frame `latency` back becomes known: if that frame ran with another, roll back to it.
        const int64_t arriving = int64_t(f) - int64_t(settings.latency);
        if (settings.latency && arriving > state.known && arriving >= int64_t(settings.first))
        {
            const uint64_t c = uint64_t(arriving);
            const uint16_t real = Script(int(settings.remote), c);
            const auto used = state.used.find(c);
            state.known = arriving;
            if (used != state.used.end() && used->second != real)
            {
                const uint64_t target = frame - settings.latency;
                const std::string error = savestates::Load(target);
                if (!error.empty())
                {
                    state.error = "loading fight frame " + std::to_string(c) + ": " + error;
                    LOG_ERROR("Rollback test: %s", state.error.c_str());
                    Finish(state, "a load failed");
                    return;
                }
                const savestates::Statistics loaded = savestates::GetStatistics();
                Rollback& rollback = state.rollbacks.emplace_back();
                rollback.fightFrame = c;
                rollback.drawWaitUs = loaded.drawWaitUs;
                rollback.loadUs = loaded.loadUs;
                rollback.pages = loaded.loadPages;
                state.framesReplayed += settings.latency;
                journal::ReplayFrom(target);
                replay::Begin();
                state.replaying = true;
                state.replayEnd = frame;
                state.replayStart = Clock::now();
                return;
            }
        }
        else if (!settings.latency)
            state.known = int64_t(f);
        Present(state, f);
        Emit(state, std::min<int64_t>(int64_t(f), int64_t(f) - int64_t(settings.latency) + 1));
        WriteSounds(state, f);
        Report(state, f);
    }
}
