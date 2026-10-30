#include "netplay/sync_test.h"

#include "config.h"
#include "gpu/command_processor.h"
#include "gpu/memory_watch.h"
#include "guest_abi.h"
#include "guest_memory.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/game_state.h"
#include "netplay/heaps.h"
#include "netplay/input_log.h"
#include "netplay/page_log.h"
#include "netplay/replay.h"
#include "netplay/savestates.h"
#include "netplay/write_tracker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace netplay::synctest
{
    namespace
    {
        using tracker::Hex;
        using tracker::kPageSize;
        using tracker::Write;
        using tracker::Writer;
        using tracker::WriterText;

        constexpr uint32_t kPageCount = tracker::kWatchEnd / kPageSize;
        constexpr uint32_t kMaxSaved = 16384;  // pages a test keeps from before their first write (64 MB)
        constexpr uint64_t kLearnFrames = 60;  // frames of writes before a save, to tell whose pages are whose
        constexpr uint32_t kStacksStart = 0x70000000; // threads' and fibers' stacks, up to MMIO
        // Shared pages tracked by bytes: the image's (variables of the game and of libraries side by side). A heap page
        // also holds the heap's bookkeeping, which the libraries' threads change too.
        constexpr uint32_t kImageStart = 0x82000000, kImageEnd = 0x84000000;
        constexpr uint32_t kMaxSaveTries = 60; // boundaries to wait for the game's threads to be idle
        constexpr uint32_t kMmioStart = 0x7F000000, kMmioEnd = 0x80000000;
        constexpr size_t kDescribedPerBoundary = 24, kDescribedPerTest = 96; // pages described in the report

        constexpr uint64_t kUnknown = ~0ull; // a save's frame while the fight it counts in loads

        struct Settings
        {
            uint64_t first = 0, frames = 8, count = 1, interval = 120;
            bool set = false;
            bool fight = false; // `first` and the tests' starts count fight frames (netplay::FightFrame), not game frames
            // Starts listed one by one (first/second/...), in place of `count` starts `interval` apart.
            std::vector<uint64_t> starts;
            // Whether the replay runs in netplay::replay's mode (DEBUG_SYNC_TEST_REPLAY=0: drawn and heard again), and
            // with its writes tracked and its pages compared (=2: neither, so that it runs as fast as a rollback's would,
            // for timing it; the fight's checksum and the host state are compared still).
            bool replayMode = true, untrackedReplay = false;
            // Whether the test uses the savestates saved at every frame (DEBUG_SYNC_TEST_SAVESTATE=1, netplay/savestates.h)
            // instead of its own: each frame of the first run is saved, then the test loads 1 to `frames` frames back
            // from its end in turn, replaying each time.
            bool pages = false;
            Settings()
            {
                if (const char* mode = std::getenv(GAME_PREFIX "_DEBUG_SYNC_TEST_REPLAY"))
                {
                    replayMode = std::atoi(mode) != 0;
                    untrackedReplay = std::atoi(mode) == 2;
                }
                if (const char* savestate = std::getenv(GAME_PREFIX "_DEBUG_SYNC_TEST_SAVESTATE"))
                    pages = std::atoi(savestate) != 0;
                const char* setting = std::getenv(GAME_PREFIX "_DEBUG_SYNC_TEST");
                if (!setting)
                    return;
                set = true;
                if (*setting == 'F' || *setting == 'f')
                {
                    fight = true;
                    setting++;
                }
                char* end = nullptr;
                first = strtoull(setting, &end, 10);
                while (end && *end == '/')
                {
                    if (starts.empty())
                        starts.push_back(first);
                    starts.push_back(strtoull(end + 1, &end, 10));
                }
                if (!fight)
                {
                    first = std::max<uint64_t>(first, 2);
                    for (uint64_t& start : starts)
                        start = std::max<uint64_t>(start, 2);
                }
                if (end && *end == ',')
                    frames = std::max<uint64_t>(strtoull(end + 1, &end, 10), 1);
                if (end && *end == ',')
                    count = strtoull(end + 1, &end, 10);
                if (end && *end == ',')
                    interval = strtoull(end + 1, &end, 10);
                interval = std::max(interval, frames + 1);
                if (!starts.empty())
                    count = starts.size();
            }

            // The start of test `test` (0 the first) at the earliest.
            uint64_t Start(uint64_t test, uint64_t next) const
            {
                return test < starts.size() ? starts[test] : std::max(next, first);
            }
        };

        const Settings& GetSettings()
        {
            static const Settings settings;
            return settings;
        }

        // Whether the write tracker runs (learning, the first run, and with DEBUG_SYNC_TEST_REPLAY=1 the replay): always
        // but with the savestates saved at every frame and an untracked replay, which need nothing it records.
        bool Tracking()
        {
            return !(GetSettings().pages && GetSettings().replayMode && GetSettings().untrackedReplay);
        }

        // Where one of the game's threads other than the main one waits at a frame boundary.
        struct ThreadSite
        {
            uint32_t id = 0, start = 0, r1 = 0;
            uint32_t stackLow = 0, stackSize = 0;
            std::vector<uint32_t> chain; // guest return addresses, innermost first
            bool operator==(const ThreadSite& other) const
            {
                return id == other.id && start == other.start && r1 == other.r1 && chain == other.chain;
            }
        };

        std::string ChainText(const std::vector<uint32_t>& chain)
        {
            std::string text;
            for (uint32_t address : chain)
                text += (text.empty() ? "" : " ") + Hex(address);
            return text;
        }

        std::string SiteText(const ThreadSite& site)
        {
            return "thread " + Hex(site.start) + " (id " + std::to_string(site.id) + ") r1 " + Hex(site.r1) + ": " + ChainText(site.chain);
        }

        // The game's threads other than the calling one, each blocked in a wait. Returns an error if one isn't.
        std::string GameThreadSites(const GameState& game, std::vector<ThreadSite>& sites)
        {
            std::string error;
            const kernel::GuestThread* self = kernel::CurrentThread();
            kernel::ForEachThread([&](kernel::GuestThread& thread) {
                if (&thread == self || !game.isGameThread(thread.StartAddress()))
                    return;
                ThreadSite site;
                site.id = thread.ThreadId();
                site.start = thread.StartAddress();
                site.stackLow = thread.StackLow();
                site.stackSize = thread.StackSize();
                if (thread.waits.load() == 0)
                {
                    if (error.empty())
                        error = "thread " + Hex(site.start) + " (id " + std::to_string(site.id) + ") isn't waiting";
                    return;
                }
                if (const PPCContext* context = thread.Context())
                    site.r1 = context->r1.u32;
                site.chain = guest::CallChain(site.r1, 12);
                sites.push_back(std::move(site));
            });
            std::sort(sites.begin(), sites.end(), [](const ThreadSite& a, const ThreadSite& b) { return a.id < b.id; });
            return error;
        }

        bool IsGameWriter(const GameState& game, const Writer& writer)
        {
            if (!game.isGameThread(writer.thread) || !writer.function || writer.function == tracker::kFileRead)
                return false;
            return !game.isLibraryFunction(writer.function & ~1u);
        }

        // Each page's contents before its first write in a test's first run, kept by the tracker's fault handler (under
        // the tracker's lock, while it's the tracker's hook).
        std::vector<int32_t> g_savedSlot; // page -> slot in g_saved, or -1
        std::vector<uint8_t> g_saved;     // kMaxSaved pages
        uint32_t g_savedCount = 0;
        uint64_t g_savedLost = 0;

        void SaveBeforeWrite(uint32_t page)
        {
            if (g_savedSlot[page] >= 0)
                return;
            if (g_savedCount == kMaxSaved)
            {
                g_savedLost++;
                return;
            }
            memcpy(&g_saved[size_t(g_savedCount) * kPageSize], guest::ToHost(page * kPageSize), kPageSize);
            g_savedSlot[page] = int32_t(g_savedCount++);
        }

        const uint8_t* Saved(uint32_t page)
        {
            const int32_t slot = g_savedSlot[page];
            return slot >= 0 ? &g_saved[size_t(slot) * kPageSize] : nullptr;
        }

        enum class Phase
        {
            kIdle,
            kLearning, // tracking writes before the save
            kFirstRun,
            kReplay,
        };

        // A page's differences from the first run: bytes that differ, and words that differ only as pointers into the
        // stacks or into physical memory (one stack or buffer for another, as when a pool's threads take other jobs).
        struct Difference
        {
            uint32_t page = 0;
            uint32_t bytes = 0;
            uint32_t pointerWords = 0;
        };

        struct Test
        {
            Phase phase = Phase::kIdle;
            uint64_t start = 0, end = 0; // the boundaries saved and loaded: the frames replayed are [start, end)
            uint64_t fightStart = kUnknown; // with fight frames (Settings::fight): the save's, as planned
            std::optional<uint64_t> savedFightFrame; // the fight frame at the save, in a fight
            uint32_t saveTries = 0;
            netplay::HostState host;
            std::string hostNote; // what the load changed of the host state
            // The host state's parts (GameState::hostStates) at each boundary of the first run, and the boundaries of the
            // replay at which they differed, with the parts that did at the first.
            std::map<uint64_t, std::vector<std::pair<uint32_t, uint64_t>>> hostStates;
            std::vector<uint64_t> hostStatesDiffering;
            std::string hostStatesFirst;
            size_t hostStatesCompared = 0;
            std::vector<ThreadSite> threads; // the game's other threads at the save
            // The game's stacks (its threads' and fibers'), which are the game's whoever wrote them; and what of them
            // isn't compared: the parts below the stack pointers, and the worker pools' stacks.
            std::vector<std::pair<uint32_t, uint32_t>> stacks, uncompared;
            // The heaps' memory: the game heap's is the game's whoever wrote it (the heap's code keeps its books), the
            // process heap's the libraries' (netplay/heaps.h).
            std::vector<std::pair<uint32_t, uint32_t>> gameHeap, processHeap;
            // Every page written, with its writers: before the save, in the first run, in the replay.
            std::map<uint32_t, std::set<Writer>> knownWriters, firstWriters, replayWriters;
            // The first run's contents of the pages written, at each boundary after a frame that wrote them (by anyone: a
            // shared page's first writer in a frame may be another thread, and the game's writes after it unrecorded).
            std::map<uint32_t, std::vector<std::pair<uint64_t, std::vector<uint8_t>>>> after;
            // Boundaries at which some of the game's other threads weren't waiting (in either run), with the first.
            std::map<uint64_t, std::string> busy;
            std::vector<uint64_t> firstWrites; // pages written in each frame of the first run
            // The image's pages the game shares with others, tracked by bytes in the first run: the bytes each side
            // changed.
            std::vector<uint32_t> byteTracked;
            struct Owners
            {
                std::bitset<kPageSize> game, others;
                std::map<Writer, std::bitset<kPageSize>> otherWriters;
            };
            std::map<uint32_t, Owners> owners;
            uint64_t byteSteps = 0, byteLost = 0;
            // At the end of the first run: the game's pages (loaded and compared; of those it shares with others, only
            // the bytes the game changed while tracked and others didn't, `shared`), mixed pages (shared, not tracked by
            // bytes: neither), the others.
            std::vector<uint32_t> loaded, mixed, others;
            std::map<uint32_t, std::bitset<kPageSize>> shared; // page -> the bytes loaded and compared
            size_t unsaved = 0;
            std::vector<uint32_t> uncommitted; // the game's pages no longer committed at the load (decommitted, released)
            std::map<uint32_t, std::vector<uint8_t>> unchecked; // page -> per byte, 1 if not compared
            // The replay: differences at each boundary; pages the replay's game code wrote that weren't written before;
            // loaded pages that others wrote in the replay (so not the game's alone).
            std::map<uint64_t, std::vector<Difference>> differences;
            std::map<uint64_t, std::set<uint32_t>> newPages;
            // The fight's checksum (netplay::FightChecksum) at each boundary of the first run, and the boundaries of the
            // replay at which it differed, of those compared.
            std::map<uint64_t, uint64_t> checksums;
            std::vector<uint64_t> checksumsDiffering;
            size_t checksumsCompared = 0;
            std::set<uint32_t> othersWrote;
            std::string details;           // each boundary's pages that differ and didn't at the boundary before
            size_t described = 0;
            std::set<uint32_t> differing;  // the pages that differed at the last boundary
            // The replay in netplay::replay's mode (untracked: its pages not compared): when it started and ended (after
            // the load, steady clock), the load's time and the test's own work at the replay's boundaries (comparisons),
            // the journal's and the command processor's statistics.
            bool replayMode = false, untracked = false;
            std::chrono::steady_clock::time_point replayStart, replayEnd;
            double loadMs = 0, bookkeepingMs = 0;
            journal::Statistics journal;
            gpu::DiscardStatistics discardedAtSave, discardedBefore, discarded;
            // The game's state in physical memory (GameState::physicalState), as it was at the save, the pages the load
            // put back, and the time the save and the load took for it.
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> physical;
            size_t physicalPagesLoaded = 0;
            double physicalSaveMs = 0, physicalLoadMs = 0;
            std::string physicalBlocks; // physical memory's blocks at the save, by the function that allocated them
            // In replay mode, the byte ranges the first run's file reads wrote, [start, end) (merged): the replay's file
            // loader gets the first run's answers from the journal and reads nothing, the load puts back what the reads
            // overwrote and the replay writes what they read (netplay::journal), at its own pace (not compared).
            std::vector<std::pair<uint32_t, uint32_t>> fileReads;
            uint64_t fileReadBytes = 0;
            // With the savestates saved at every frame (Settings::pages): the rollback under way loads `back` frames
            // back from the first run's end, and each one's outcome.
            bool pages = false;
            uint32_t back = 0;
            double rollbackBookkeepingMs = 0; // the test's comparisons in the rollback's replay
            struct Rollback
            {
                uint32_t back = 0;
                uint64_t target = 0;
                double loadUs = 0, hostLoadUs = 0;
                size_t pagesLoaded = 0;
                size_t loadMismatches = 0;    // pages compared right after the load that weren't as at the target boundary
                std::string firstMismatch;
                double replayMs = 0;          // the replay's frames, but the test's comparisons
                size_t checksums = 0, checksumsDiffering = 0, hostStates = 0, hostStatesDiffering = 0, boundariesDiffering = 0;
            };
            std::vector<Rollback> rollbacks;
            // The structural description of the game's memory at the save (GameState::savedMemory), and its pages (any
            // view's), for comparisons.
            GameState::SavedMemory saved;
            std::vector<bool> watched;
            // The pages the page log copied in the first run, as they were at the save (from the logs, or for pages the
            // write tracker kept, those); the pages Classify's rule by writer makes the game's (learned), those of them
            // the savestate doesn't hold, and the pages it holds that others wrote in the first run.
            std::map<uint32_t, std::vector<uint8_t>> loggedSaved;
            std::vector<uint32_t> learned, learnedNotWatched, watchedByOthers;
            // Pages the write tracker saw written in a frame of the first run that the game's memory holds but the page
            // log didn't copy: writes it missed (none, if every write to the game's memory is seen), with their writers.
            std::map<uint32_t, std::set<Writer>> missed;
            std::map<uint32_t, uint64_t> missedFrame; // and the frame each was first missed in
        };

        Test g_test;
        bool g_startedSavestates = false; // the tests started the savestates saved at every frame (Settings::pages)
        uint64_t g_testsRun = 0;
        uint64_t g_nextStart = 0;
        std::atomic<bool> g_active{ false };
        // The image's pages' writers seen in every test so far (learning, first runs and replays), which a test learns
        // from too: a page the game shares with a library is often written by the library in some frames only (a sound
        // started, its lock taken), and tests a few frames apart learn for a few frames only.
        std::map<uint32_t, std::set<Writer>> g_imageWriters;

        bool InImage(uint32_t page)
        {
            return page * kPageSize >= kImageStart && page * kPageSize < kImageEnd;
        }

        // While learning, the tracker is resampled every millisecond: a page's writers are seen even where the game
        // writes first in every frame (the frame boundary samples) and others write later, while the update runs.
        std::thread g_sampler;
        std::atomic<bool> g_sampling{ false };

        void StartSampler()
        {
            g_sampling.store(true);
            g_sampler = std::thread([] {
                while (g_sampling.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    tracker::Resample();
                }
            });
        }

        void StopSampler()
        {
            g_sampling.store(false);
            if (g_sampler.joinable())
                g_sampler.join();
        }

        bool InRanges(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint32_t address)
        {
            return std::any_of(ranges.begin(), ranges.end(),
                [&](const std::pair<uint32_t, uint32_t>& range) { return address >= range.first && address < range.second; });
        }

        // The game's whoever writes it: its stacks and its heap.
        bool InGameMemory(const Test& test, uint32_t address)
        {
            return InRanges(test.stacks, address) || InRanges(test.gameHeap, address);
        }

        // The first run's contents of a page at a boundary.
        const uint8_t* Expected(const Test& test, uint32_t page, uint64_t boundary)
        {
            const uint8_t* expected = page < kPageCount ? Saved(page) : nullptr;
            if (!expected)
            {
                if (const auto it = test.loggedSaved.find(page); it != test.loggedSaved.end())
                    expected = it->second.data();
            }
            if (const auto it = test.after.find(page); it != test.after.end())
            {
                for (const auto& [at, contents] : it->second)
                {
                    if (at <= boundary)
                        expected = contents.data();
                }
            }
            return expected;
        }

        uint32_t LoadBigEndian(const uint8_t* bytes)
        {
            uint32_t word;
            memcpy(&word, bytes, sizeof(word));
            return __builtin_bswap32(word);
        }

        // Both values point into the stacks, or into allocated physical memory. (Physical addresses are also the bits of
        // negative floats from -1 to 0, hence the checks.)
        bool IsPointerPair(uint32_t a, uint32_t b)
        {
            const auto stack = [](uint32_t value) { return value >= 0x70000000 && value < 0x7F000000; };
            const auto physical = [](uint32_t value) {
                return value >= 0xA0000000 && !(value & 3) && guest::IsAllocated(value);
            };
            return (stack(a) && stack(b)) || (physical(a) && physical(b));
        }

        Difference Compare(const Test& test, uint32_t page, const uint8_t* expected, const uint8_t* actual)
        {
            Difference difference{ page };
            if (memcmp(actual, expected, kPageSize) == 0)
                return difference;
            const auto mask = test.unchecked.find(page);
            for (uint32_t offset = 0; offset < kPageSize; offset += 4)
            {
                if (memcmp(actual + offset, expected + offset, 4) == 0)
                    continue;
                uint32_t bytes = 0;
                for (uint32_t i = offset; i < offset + 4; i++)
                {
                    if (actual[i] != expected[i] && (mask == test.unchecked.end() || !mask->second[i]))
                        bytes++;
                }
                if (!bytes)
                    continue;
                if (IsPointerPair(LoadBigEndian(expected + offset), LoadBigEndian(actual + offset)))
                    difference.pointerWords++;
                else
                    difference.bytes += bytes;
            }
            return difference;
        }

        std::string WordText(uint32_t word)
        {
            std::string text = Hex(word);
            float value;
            memcpy(&value, &word, sizeof(value));
            const float magnitude = std::fabs(value);
            if (word != 0 && std::isfinite(value) && magnitude >= 1e-6f && magnitude < 1e7f)
            {
                char number[32];
                snprintf(number, sizeof(number), " (%g)", value);
                text += number;
            }
            return text;
        }

        std::string WritersText(const std::map<uint32_t, std::set<Writer>>& writers, uint32_t page)
        {
            const auto it = writers.find(page);
            if (it == writers.end())
                return "none";
            std::string text;
            for (const Writer& writer : it->second)
                text += "[" + WriterText(writer) + "] ";
            return text;
        }

        // A differing page: its runs of differing bytes (closer than 16 bytes merged), with their words, first run >
        // replay (big-endian, and as floats where they look like ones).
        std::string DescribePage(const Test& test, const Difference& difference, const uint8_t* expected, const uint8_t* actual)
        {
            const uint32_t page = difference.page;
            std::string text = "  " + Hex(page * kPageSize) + ": " + std::to_string(difference.bytes) + " bytes differ";
            if (difference.pointerWords)
                text += ", and " + std::to_string(difference.pointerWords) + " pointers";
            text += "\n    before: " + WritersText(test.knownWriters, page) + "\n";
            text += "    first run: " + WritersText(test.firstWriters, page) + "\n";
            text += "    replay: " + WritersText(test.replayWriters, page) + "\n";
            const auto mask = test.unchecked.find(page);
            auto differs = [&](uint32_t i) {
                return actual[i] != expected[i] && (mask == test.unchecked.end() || !mask->second[i]);
            };
            int runs = 0;
            for (uint32_t i = 0; i < kPageSize && runs < 16;)
            {
                if (!differs(i))
                {
                    i++;
                    continue;
                }
                uint32_t last = i;
                for (uint32_t j = i + 1; j < kPageSize && j <= last + 16; j++)
                {
                    if (differs(j))
                        last = j;
                }
                const uint32_t first = i & ~3u;
                text += "    " + Hex(page * kPageSize + i) + "-" + Hex(page * kPageSize + last) + ":";
                int words = 0;
                for (uint32_t offset = first; offset <= last && words < 8; offset += 4, words++)
                {
                    const uint32_t before = LoadBigEndian(expected + offset);
                    const uint32_t after = LoadBigEndian(actual + offset);
                    text += before == after ? " =" : " " + WordText(before) + ">" + WordText(after);
                }
                if (words == 8 && first + 32 <= last)
                    text += " ...";
                text += "\n";
                runs++;
                i = last + 1;
            }
            return text;
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

        // "frame 3242 (fight frame 100)", or "frame 3242" outside a fight.
        std::string StartText(const Test& test)
        {
            std::string text = "frame " + std::to_string(test.start);
            if (test.savedFightFrame)
                text += " (fight frame " + std::to_string(*test.savedFightFrame) + ")";
            else if (test.fightStart != kUnknown)
                text += " (fight frame " + std::to_string(test.fightStart) + ")";
            return text;
        }

        // How the replay ran: in netplay::replay's mode, the time it took (but the load and the test's comparisons), the
        // journal's calls and the frames the command processor discarded.
        // The frames replayed and their time but the test's comparisons (with the savestates saved at every frame, every
        // rollback's).
        void ReplayWork(const Test& test, uint64_t& frames, double& workMs)
        {
            if (test.pages)
            {
                frames = 0;
                workMs = 0;
                for (const Test::Rollback& rollback : test.rollbacks)
                {
                    frames += rollback.back;
                    workMs += rollback.replayMs;
                }
                return;
            }
            frames = test.end - test.start;
            workMs = std::chrono::duration<double, std::milli>(test.replayEnd - test.replayStart).count() - test.bookkeepingMs;
        }

        std::string ReplayText(const Test& test)
        {
            if (!test.replayMode)
                return "Replay: drawn and heard again (DEBUG_SYNC_TEST_REPLAY=0)";
            uint64_t frames = 0;
            double workMs = 0;
            ReplayWork(test, frames, workMs);
            char text[1024];
            snprintf(text, sizeof(text),
                "Replay: in replay mode, %llu frames in %.2f ms, %.2f ms a frame but the test's comparisons at its boundaries (%.2f ms in all; "
                "the load took %.2f ms). Journal: %llu calls recorded, %llu answered from the record, %llu not matched (made), %llu recorded but "
                "not made in the replay, %llu bytes of changes. Command processor: %llu frames, %llu draws and %llu frame pacing waits discarded.",
                (unsigned long long)frames, workMs, frames ? workMs / double(frames) : 0.0, test.bookkeepingMs, test.loadMs,
                (unsigned long long)test.journal.recorded, (unsigned long long)test.journal.replayed, (unsigned long long)test.journal.mismatched,
                (unsigned long long)test.journal.unreplayed, (unsigned long long)test.journal.bytes,
                (unsigned long long)(test.discarded.frames - test.discardedBefore.frames),
                (unsigned long long)(test.discarded.draws - test.discardedBefore.draws),
                (unsigned long long)(test.discarded.pacingWaits - test.discardedBefore.pacingWaits));
            std::string result = text;
            // The command processor thread's CPU time a frame: the replay's discarded, the first run's drawn.
            const uint64_t discardedFrames = test.discarded.frames - test.discardedBefore.frames;
            const uint64_t drawnFrames = test.discardedBefore.drawnFrames - test.discardedAtSave.drawnFrames;
            if (discardedFrames && drawnFrames)
            {
                snprintf(text, sizeof(text), " Its thread's CPU time: %.2f ms a frame discarded, %.2f ms a frame drawn in the first run.",
                    (test.discarded.discardedMicroseconds - test.discardedBefore.discardedMicroseconds) / 1000.0 / double(discardedFrames),
                    (test.discardedBefore.drawnMicroseconds - test.discardedAtSave.drawnMicroseconds) / 1000.0 / double(drawnFrames));
                result += text;
            }
            if (!test.journal.firstMismatch.empty())
                result += "\n  The first call not matched: " + test.journal.firstMismatch;
            if (!test.journal.firstUnreplayed.empty())
                result += "\n  The first call recorded that no replay made: " + test.journal.firstUnreplayed;
            if (test.journal.replayedNear)
                result += "\n  Calls answered from the record of another frame: " + std::to_string(test.journal.replayedNear);
            if (test.journal.deferred)
                result += "\n  Releases deferred until past rolling back: " + std::to_string(test.journal.deferred);
            if (!test.journal.functions.empty())
                result += "\n  Calls recorded, by function: " + test.journal.functions;
            if (!test.journal.calls.empty())
                result += "\n  The calls recorded (frame, function, return address, stack pointer, arguments; result):\n" + test.journal.calls;
            return result;
        }

        // With the savestates saved at every frame: their cost, each rollback's load and replay, and how the game's memory
        // as the game describes it compares with the writes the test saw.
        std::string PagesText(const Test& test)
        {
            std::string text = "Savestates saved at every frame (DEBUG_SYNC_TEST_SAVESTATE=1): " + savestates::StatisticsText() + ".\n  Their page log: " +
                pagelog::StatisticsText() + ".\n";
            text += "Rollbacks from the first run's end, frames back: the load's time (the host state's), the pages it wrote back and those then not "
                    "as at the target boundary; the replay's time a frame but the test's comparisons; boundaries where the fight's checksum, the "
                    "host state and the pages' bytes differed from the first run's:\n";
            char part[512];
            for (const Test::Rollback& rollback : test.rollbacks)
            {
                snprintf(part, sizeof(part), "  %u (to frame %llu): %.1f us (%.1f), %zu pages, %zu not as at the target; %.2f ms a frame; checksum %zu of %zu, "
                                             "host state %zu of %zu, bytes %zu of %u\n",
                    rollback.back, (unsigned long long)rollback.target, rollback.loadUs, rollback.hostLoadUs, rollback.pagesLoaded,
                    rollback.loadMismatches, rollback.back ? rollback.replayMs / rollback.back : 0.0, rollback.checksumsDiffering, rollback.checksums,
                    rollback.hostStatesDiffering, rollback.hostStates, rollback.boundariesDiffering, rollback.back);
                text += part;
                if (!rollback.firstMismatch.empty())
                    text += "    the first page not as at the target after the load:\n" + rollback.firstMismatch;
            }
            size_t masked = 0;
            for (uint32_t page : test.loaded)
                masked += test.shared.count(page);
            snprintf(part, sizeof(part), "Pages compared: the %zu the page log copied in the first run (%zu of them the game's only in part, masked)\n",
                test.loaded.size(), masked);
            text += part;
            if (Tracking())
            {
                snprintf(part, sizeof(part), "Writes the page log missed (pages of the game's memory the write tracker saw written in a frame of the first "
                                             "run that the frame's log doesn't hold): %zu\n", test.missed.size());
                text += part;
                size_t listed = 0;
                for (const auto& [page, writers] : test.missed)
                {
                    if (listed++ == 64)
                        break;
                    std::string who;
                    for (const Writer& writer : writers)
                        who += "[" + WriterText(writer) + "] ";
                    snprintf(part, sizeof(part), "  %08X (frame %llu): ", page * kPageSize, (unsigned long long)test.missedFrame.at(page));
                    text += part + who + "\n";
                }
                snprintf(part, sizeof(part), "Pages the writers make the game's (the test's own savestate's rule) that the game's memory doesn't hold: %zu\n",
                    test.learnedNotWatched.size());
                text += part;
                listed = 0;
                for (uint32_t page : test.learnedNotWatched)
                {
                    if (listed++ == 64)
                        break;
                    text += "  " + Hex(page * kPageSize) + " " + WritersText(test.firstWriters, page) + "\n";
                }
                snprintf(part, sizeof(part), "Pages the game's memory holds (whole) that others wrote in the first run: %zu\n", test.watchedByOthers.size());
                text += part;
                listed = 0;
                for (uint32_t page : test.watchedByOthers)
                {
                    if (listed++ == 64)
                        break;
                    text += "  " + Hex(page * kPageSize) + " " + WritersText(test.firstWriters, page) + "\n";
                }
            }
            return text;
        }

        void Report(const Test& test, const std::string& failure)
        {
            const std::string path = config::DumpPath("sync_test_" + std::to_string(test.start) + ".txt");
            g_reportFile = fopen(path.c_str(), "w");
            line("Sync test: saved at %s's boundary, %llu frames run, loaded, and run again (writes tracked for %llu frames before)\n\n",
                StartText(test).c_str(), (unsigned long long)(test.end - test.start), (unsigned long long)kLearnFrames);
            if (!failure.empty())
                line("Not loaded: %s\n\n", failure.c_str());
            if (!test.hostNote.empty())
                line("Host state at the load: %s\n", test.hostNote.c_str());
            if (test.hostStatesCompared)
            {
                if (test.hostStatesDiffering.empty())
                    line("Host state in the replay: the same as in the first run at all %zu boundaries\n", test.hostStatesCompared);
                else
                    line("Host state in the replay: differs from the first run's at %zu of %zu boundaries, first at %llu: %s\n",
                        test.hostStatesDiffering.size(), test.hostStatesCompared, (unsigned long long)test.hostStatesDiffering.front(),
                        test.hostStatesFirst.c_str());
            }
            if (!test.hostNote.empty() || test.hostStatesCompared)
                line("\n");
            if (failure.empty())
                line("%s\n\n", ReplayText(test).c_str());
            if (test.pages)
                line("%s\n", PagesText(test).c_str());
            if (!test.physical.empty())
            {
                size_t bytes = 0;
                for (const auto& [start, contents] : test.physical)
                    bytes += contents.size();
                line("The game's state in physical memory: %.1f MB saved in %zu ranges (in %.1f ms), %zu pages of it put back by the load (in "
                     "%.1f ms, comparing)\n\n",
                    bytes / (1024.0 * 1024), test.physical.size(), test.physicalSaveMs, test.physicalPagesLoaded, test.physicalLoadMs);
            }
            if (!test.physicalBlocks.empty())
                line("Physical memory's blocks from XPhysicalAlloc at the save, by the function that allocated them:\n%s\n", test.physicalBlocks.c_str());
            if (!test.fileReads.empty() || test.journal.readsLost)
                line("File reads in the first run: %llu (%llu bytes, in %zu ranges), %llu not kept; the load put back what they overwrote "
                     "(%llu reads), the replay wrote what they read (%llu, %llu bytes of them left as they were: others' memory); not "
                     "compared\n\n",
                    (unsigned long long)test.journal.reads, (unsigned long long)test.fileReadBytes, test.fileReads.size(),
                    (unsigned long long)test.journal.readsLost, (unsigned long long)test.journal.readsRewound,
                    (unsigned long long)test.journal.readsWritten, (unsigned long long)test.journal.readBytesLeft);
            line("First run: pages written per frame:");
            for (uint64_t count : test.firstWrites)
                line(" %llu", (unsigned long long)count);
            line("\n  %zu pages in all: %zu the game's (%.2f MB; loaded and compared), %zu mixed (the game's and others'), %zu others'",
                test.firstWriters.size(), test.loaded.size(), test.loaded.size() * double(kPageSize) / (1024 * 1024), test.mixed.size(),
                test.others.size());
            if (test.unsaved)
                line(", %zu of the game's not kept (more than %u pages)", test.unsaved, kMaxSaved);
            if (!test.uncommitted.empty())
            {
                line(", %zu of the game's no longer committed at the load, so not loaded:", test.uncommitted.size());
                for (uint32_t page : test.uncommitted)
                    line(" %08X", page * kPageSize);
            }
            const auto bytes = [](const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
                uint64_t total = 0;
                for (const auto& [start, end] : ranges)
                    total += end - start;
                return total;
            };
            line("\n  Heaps: the game heap's %.1f MB in %zu ranges (the game's), the process heap's %.1f MB in %zu (the libraries')",
                bytes(test.gameHeap) / (1024.0 * 1024), test.gameHeap.size(), bytes(test.processHeap) / (1024.0 * 1024), test.processHeap.size());
            line("\n\nThe game's other threads at the save:\n");
            for (const ThreadSite& site : test.threads)
                line("  %s\n", SiteText(site).c_str());
            if (failure.empty() && test.untracked)
                line("\nReplay: pad reads not in the record: %llu; untracked (DEBUG_SYNC_TEST_REPLAY=2), its pages not compared\n",
                    (unsigned long long)input::Misses());
            else if (failure.empty())
            {
                line("\nReplay: pad reads not in the record: %llu\n", (unsigned long long)input::Misses());
                for (uint64_t boundary = test.start + 1; boundary <= test.end; boundary++)
                {
                    size_t pages = 0, pointerPages = 0;
                    uint64_t bytes = 0, pointers = 0;
                    if (const auto it = test.differences.find(boundary); it != test.differences.end())
                    {
                        for (const Difference& difference : it->second)
                        {
                            (difference.bytes ? pages : pointerPages)++;
                            bytes += difference.bytes;
                            pointers += difference.pointerWords;
                        }
                    }
                    line("  boundary %llu: %zu pages differ (%llu bytes), %zu more only in pointers (%llu in all)", (unsigned long long)boundary, pages,
                        (unsigned long long)bytes, pointerPages, (unsigned long long)pointers);
                    if (const auto added = test.newPages.find(boundary); added != test.newPages.end())
                        line(", %zu pages written that weren't before", added->second.size());
                    if (const auto busy = test.busy.find(boundary); busy != test.busy.end())
                        line("; not all waiting (%s)", busy->second.c_str());
                    line("\n");
                }
                if (!test.details.empty())
                    line("\n%s", test.details.c_str());
                if (!test.differences.empty())
                {
                    line("\nEvery page that differs, at each boundary (address: bytes, or pointers only as p):\n");
                    for (const auto& [boundary, differences] : test.differences)
                    {
                        line("  %llu:", (unsigned long long)boundary);
                        for (const Difference& difference : differences)
                        {
                            if (difference.bytes)
                                line(" %05X:%u", difference.page, difference.bytes);
                            else
                                line(" %05X:p", difference.page);
                        }
                        line("\n");
                    }
                }
                if (!test.othersWrote.empty())
                {
                    line("\nPages loaded as the game's that others wrote in the replay:\n");
                    for (uint32_t page : test.othersWrote)
                        line("  %08X %s\n", page * kPageSize, WritersText(test.replayWriters, page).c_str());
                }
                if (!test.newPages.empty())
                {
                    line("\nPages the replay's game code wrote that weren't written before:\n");
                    for (const auto& [boundary, pages] : test.newPages)
                    {
                        for (uint32_t page : pages)
                            line("  frame %llu: %08X %s\n", (unsigned long long)(boundary - 1), page * kPageSize, WritersText(test.replayWriters, page).c_str());
                    }
                }
            }
            if (!test.owners.empty())
            {
                line("\nPages shared with others, tracked by bytes in the first run (%llu writes stepped, %llu not recorded); loaded and "
                     "compared: the bytes the game changed and others didn't. The others' bytes, and the other writers of the game's "
                     "bytes (not loaded):\n",
                    (unsigned long long)test.byteSteps, (unsigned long long)test.byteLost);
                // A page's bytes in `bytes`, as runs (32 at most).
                auto runs = [](uint32_t page, const std::bitset<kPageSize>& bytes) {
                    int count = 0;
                    for (uint32_t i = 0; i < kPageSize && count < 32;)
                    {
                        if (!bytes[i])
                        {
                            i++;
                            continue;
                        }
                        uint32_t end = i;
                        while (end < kPageSize && bytes[end])
                            end++;
                        line(" %08X-%08X", page * kPageSize + i, page * kPageSize + end - 1);
                        count++;
                        i = end;
                    }
                };
                for (const auto& [page, owners] : test.owners)
                {
                    const bool loaded = test.shared.count(page) != 0;
                    line("  %08X%s: the game's %zu bytes, others' %zu (%zu both):", page * kPageSize, loaded ? "" : " (not loaded)",
                        owners.game.count(), owners.others.count(), (owners.game & owners.others).count());
                    runs(page, owners.others);
                    line("\n");
                    for (const auto& [writer, bytes] : owners.otherWriters)
                    {
                        if ((bytes & owners.game).none())
                            continue;
                        line("    [%s]:", WriterText(writer).c_str());
                        runs(page, bytes & owners.game);
                        line("\n");
                    }
                }
            }
            line("\nMixed pages (not loaded):\n");
            for (uint32_t page : test.mixed)
            {
                std::map<uint32_t, std::set<Writer>> writers;
                for (const auto* source : { &test.knownWriters, &test.firstWriters })
                {
                    if (const auto it = source->find(page); it != source->end())
                        writers[page].insert(it->second.begin(), it->second.end());
                }
                line("  %08X %s\n", page * kPageSize, WritersText(writers, page).c_str());
            }
            line("\nOthers' pages written in the first run (not loaded; the process heap's, then by first-run writer):\n");
            for (uint32_t page : test.others)
            {
                const bool heap = InRanges(test.processHeap, page * kPageSize);
                line("  %08X%s %s\n", page * kPageSize, heap ? " (process heap)" : "", WritersText(test.firstWriters, page).c_str());
            }
            if (g_reportFile)
                fclose(g_reportFile);
            g_reportFile = nullptr;

            if (!failure.empty())
            {
                LOG_WARN("Sync test at %s: not loaded: %s; details in %s", StartText(test).c_str(), failure.c_str(), path.c_str());
                return;
            }
            size_t differing = 0, pointerOnly = 0;
            for (const auto& [boundary, differences] : test.differences)
            {
                const bool bytes = std::any_of(differences.begin(), differences.end(), [](const Difference& d) { return d.bytes != 0; });
                (bytes ? differing : pointerOnly)++;
            }
            // The fight's checksum, compared at each boundary of both runs, and the host state (the fibers').
            std::string checksums;
            if (test.checksumsCompared)
            {
                checksums = test.checksumsDiffering.empty()
                    ? "the fight's checksum the same at all " + std::to_string(test.checksumsCompared) + " boundaries; "
                    : "the fight's checksum differs at " + std::to_string(test.checksumsDiffering.size()) + " of " +
                        std::to_string(test.checksumsCompared) + " boundaries, first at " + std::to_string(test.checksumsDiffering.front()) + "; ";
            }
            if (test.hostStatesCompared)
            {
                checksums += test.hostStatesDiffering.empty()
                    ? "the host state the same at all " + std::to_string(test.hostStatesCompared) + "; "
                    : "the host state differs at " + std::to_string(test.hostStatesDiffering.size()) + ", first at " +
                        std::to_string(test.hostStatesDiffering.front()) + "; ";
            }
            // The load's note up to its details: how many fibers it put back elsewhere.
            if (!test.hostNote.empty())
                checksums += test.hostNote.substr(0, test.hostNote.find(';')) + "; ";
            if (test.replayMode)
            {
                uint64_t frames = 0;
                double workMs = 0;
                ReplayWork(test, frames, workMs);
                char replay[160];
                snprintf(replay, sizeof(replay), "replayed in replay mode at %.2f ms a frame, %llu journaled calls answered, %llu not matched; ",
                    frames ? workMs / double(frames) : 0.0, (unsigned long long)test.journal.replayed, (unsigned long long)test.journal.mismatched);
                checksums += replay;
            }
            if (test.pages)
            {
                double loadLow = 1e30, loadHigh = 0, replayLow = 1e30, replayHigh = 0;
                size_t pagesLow = ~size_t(0), pagesHigh = 0, mismatches = 0, boundaries = 0, bytesDiffering = 0;
                for (const Test::Rollback& rollback : test.rollbacks)
                {
                    loadLow = std::min(loadLow, rollback.loadUs);
                    loadHigh = std::max(loadHigh, rollback.loadUs);
                    pagesLow = std::min(pagesLow, rollback.pagesLoaded);
                    pagesHigh = std::max(pagesHigh, rollback.pagesLoaded);
                    const double perFrame = rollback.back ? rollback.replayMs / rollback.back : 0.0;
                    replayLow = std::min(replayLow, perFrame);
                    replayHigh = std::max(replayHigh, perFrame);
                    mismatches += rollback.loadMismatches;
                    boundaries += rollback.back;
                    bytesDiffering += rollback.boundariesDiffering;
                }
                const bool clean = !mismatches && test.missed.empty() && test.checksumsDiffering.empty() && test.hostStatesDiffering.empty();
                char summary[1024];
                snprintf(summary, sizeof(summary),
                    "Sync test at %s, savestates saved at every frame: %zu rollbacks of 1 to %zu frames; %sloads %.0f-%.0f us (%zu-%zu pages), "
                    "replays at %.2f-%.2f ms a frame; %zu pages not as at the target after the loads, %zu writes the page log missed%s; bytes differ "
                    "at %zu of %zu replayed boundaries; details in %s",
                    StartText(test).c_str(), test.rollbacks.size(), test.rollbacks.size(), checksums.c_str(), loadLow, loadHigh, pagesLow, pagesHigh,
                    replayLow, replayHigh, mismatches, test.missed.size(), Tracking() ? "" : " (untracked: not checked)", bytesDiffering, boundaries,
                    path.c_str());
                if (clean)
                    LOG_INFO("%s", summary);
                else
                    LOG_WARN("%s", summary);
                return;
            }
            if (test.untracked)
                LOG_INFO("Sync test at %s: %llu frames replayed untracked, their pages not compared (%sdetails in %s)", StartText(test).c_str(),
                    (unsigned long long)(test.end - test.start), checksums.c_str(), path.c_str());
            else if (!differing && !pointerOnly && test.newPages.empty())
                LOG_INFO("Sync test at %s: %llu frames replayed identically (%s%zu pages loaded, %zu mixed not loaded); details in %s",
                    StartText(test).c_str(), (unsigned long long)(test.end - test.start), checksums.c_str(), test.loaded.size(), test.mixed.size(),
                    path.c_str());
            else
                LOG_WARN("Sync test at %s: the replay differs at %zu of %llu boundaries, in pointers only at %zu more; %zu boundaries "
                         "with pages written that weren't before (%s%zu pages loaded, %zu mixed not loaded); details in %s",
                    StartText(test).c_str(), differing, (unsigned long long)(test.end - test.start), pointerOnly, test.newPages.size(),
                    checksums.c_str(), test.loaded.size(), test.mixed.size(), path.c_str());
        }

        void Finish(const std::string& failure)
        {
            // A test that wasn't loaded ends its journal here (one replayed ended it at its last boundary).
            replay::End();
            journal::Stop();
            Report(g_test, failure);
            for (const auto* writers : { &g_test.knownWriters, &g_test.firstWriters, &g_test.replayWriters })
            {
                for (const auto& [page, pageWriters] : *writers)
                {
                    if (InImage(page))
                        g_imageWriters[page].insert(pageWriters.begin(), pageWriters.end());
                }
            }
            input::Stop();
            g_active.store(false, std::memory_order_release);
            g_testsRun++;
            if (!GetSettings().fight)
                g_nextStart = g_test.start + GetSettings().interval;
            else if (g_test.fightStart != kUnknown)
                g_nextStart = g_test.fightStart + GetSettings().interval;
            g_test = Test{};
            if (g_testsRun >= GetSettings().count)
            {
                std::vector<uint8_t>().swap(g_saved);
                std::vector<int32_t>().swap(g_savedSlot);
                // The savestates saved at every frame, if the tests started them.
                if (g_startedSavestates)
                {
                    LOG_INFO("Sync test: savestates: %s", savestates::StatisticsText().c_str());
                    savestates::Stop();
                    g_startedSavestates = false;
                }
            }
        }

        // The pages written in the frame before a boundary and their writers, into `writers`; returns the game's.
        std::set<uint32_t> Collect(const GameState& game, const std::vector<Write>& writes, std::map<uint32_t, std::set<Writer>>& writers)
        {
            std::set<uint32_t> gameWrites;
            for (const Write& write : writes)
            {
                writers[write.page].insert(write.writer);
                if (IsGameWriter(game, write.writer))
                    gameWrites.insert(write.page);
            }
            return gameWrites;
        }

        // Starts tracking writes, to learn whose pages are whose before the save at game frame `start` (kUnknown while
        // the fight loads), fight frame `fightStart` with fight frames.
        void Learn(uint64_t start, uint64_t fightStart)
        {
            if (Tracking() && tracker::Active())
            {
                LOG_WARN("Sync test: the write tracker is busy (a state census?), so no sync test");
                g_testsRun = GetSettings().count;
                return;
            }
            g_test = Test{};
            g_test.phase = Phase::kLearning;
            g_test.start = start;
            g_test.fightStart = fightStart;
            g_test.knownWriters = g_imageWriters;
            g_test.pages = GetSettings().pages;
            g_active.store(true, std::memory_order_release);
            if (Tracking())
            {
                tracker::Start();
                StartSampler();
            }
        }

        void StopTracking()
        {
            if (!Tracking())
                return;
            StopSampler();
            tracker::Stop();
        }

        // Whether the game's writers and others both wrote a page of the image.
        bool Shared(const GameState& game, uint32_t page, const std::set<Writer>& writers)
        {
            const uint32_t address = page * kPageSize;
            if (address < kImageStart || address >= kImageEnd)
                return false;
            const bool byGame = std::any_of(writers.begin(), writers.end(), [&](const Writer& writer) { return IsGameWriter(game, writer); });
            const bool byOthers = std::any_of(writers.begin(), writers.end(), [&](const Writer& writer) { return !IsGameWriter(game, writer); });
            return byGame && byOthers;
        }

        // At a boundary while learning: saves once it's the start and the game's threads are idle. The pages known to be
        // shared then are tracked by bytes through the first run.
        void LearningBoundary(const GameState& game, uint64_t boundary)
        {
            Test& test = g_test;
            if (test.start == kUnknown)
            {
                // Learning while the fight loads: its first frame says when the save is.
                if (const std::optional<uint64_t> fightFrame = FightFrame())
                {
                    if (*fightFrame > test.fightStart)
                    {
                        StopTracking();
                        test.start = boundary;
                        Finish("fight frame " + std::to_string(test.fightStart) + " passed while learning");
                        return;
                    }
                    test.start = boundary + (test.fightStart - *fightFrame);
                }
                else if (game.fightState && game.fightState() == GameState::FightState::kNone)
                {
                    StopTracking();
                    test.start = boundary;
                    Finish("the fight ended while it loaded");
                    return;
                }
            }
            if (boundary < test.start)
            {
                if (Tracking())
                    Collect(game, tracker::NextFrame(), test.knownWriters);
                return;
            }
            std::vector<ThreadSite> threads;
            std::string error = GameThreadSites(game, threads);
            netplay::HostState host;
            if (error.empty())
                host = game.saveHost(error);
            if (!error.empty() || !host.load)
            {
                if (Tracking())
                    Collect(game, tracker::NextFrame(), test.knownWriters);
                // Try the next boundaries, for a while.
                if (++test.saveTries < kMaxSaveTries)
                    return;
                test.start = boundary;
                StopTracking();
                Finish("no save: " + error);
                return;
            }
            if (Tracking())
                StopSampler();
            if (g_saved.empty())
            {
                g_saved.resize(size_t(kMaxSaved) * kPageSize);
                g_savedSlot.resize(kPageCount);
            }
            std::fill(g_savedSlot.begin(), g_savedSlot.end(), -1);
            g_savedCount = 0;
            g_savedLost = 0;
            test.phase = Phase::kFirstRun;
            test.start = boundary;
            test.savedFightFrame = FightFrame();
            test.end = boundary + GetSettings().frames;
            test.threads = std::move(threads);
            test.stacks = host.stacks;
            test.uncompared = host.freeStack;
            test.host = std::move(host);
            // Not compared: the game's other threads' stacks, which hold leftovers of whichever jobs each of a pool's
            // threads took (loaded all the same), and the main thread's below its stack pointer (in the main loop).
            for (const ThreadSite& site : test.threads)
            {
                if (site.stackLow && site.stackSize)
                {
                    test.stacks.emplace_back(site.stackLow, site.stackLow + site.stackSize);
                    test.uncompared.emplace_back(site.stackLow, site.stackLow + site.stackSize);
                }
            }
            if (const kernel::GuestThread* self = kernel::CurrentThread(); self && self->Context())
            {
                test.stacks.emplace_back(self->StackLow(), self->StackLow() + self->StackSize());
                const uint32_t r1 = self->Context()->r1.u32;
                if (r1 > self->StackLow() && r1 <= self->StackLow() + self->StackSize())
                    test.uncompared.emplace_back(self->StackLow(), r1);
            }
            input::Record();
            if (test.pages)
            {
                // The savestates saved at every frame, from here on if they aren't already: this boundary's is saved.
                if (!savestates::Active())
                {
                    savestates::Start(uint32_t(GetSettings().frames));
                    g_startedSavestates = savestates::Active();
                }
                test.watched.assign(pagelog::kPageCount, false);
                if (game.savedMemory)
                {
                    test.saved = game.savedMemory();
                    for (const auto& [start, end] : test.saved.ranges)
                    {
                        for (uint64_t page = start / kPageSize; page < (uint64_t(end) + kPageSize - 1) / kPageSize; page++)
                            test.watched[pagelog::CanonicalPage(uint32_t(page))] = true;
                    }
                    for (const auto& [page, mask] : test.saved.partial)
                        test.watched[page / kPageSize] = true;
                }
            }
            else
            {
                // The game's state in physical memory, which the write tracker doesn't cover, whole.
                const auto physicalStart = std::chrono::steady_clock::now();
                if (game.physicalState)
                {
                    for (const auto& [start, end] : game.physicalState())
                    {
                        const uint8_t* contents = guest::ToHost(start);
                        test.physical.emplace_back(start, std::vector<uint8_t>(contents, contents + (end - start)));
                    }
                }
                test.physicalSaveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - physicalStart).count();
            }
            {
                std::map<std::pair<uint32_t, bool>, std::pair<size_t, uint64_t>> byFunction; // (function, the game's) -> blocks, bytes
                for (const heaps::PhysicalBlock& block : heaps::PhysicalBlocks())
                {
                    auto& [blocks, bytes] = byFunction[{ block.function, block.game }];
                    blocks++;
                    bytes += block.size;
                }
                for (const auto& [owner, count] : byFunction)
                {
                    char text[128];
                    snprintf(text, sizeof(text), "  sub_%08X (%s): %zu blocks, %.1f MB\n", owner.first, owner.second ? "the game's" : "a library's",
                        count.first, count.second / (1024.0 * 1024));
                    test.physicalBlocks += text;
                }
            }
            // In replay mode, the libraries' calls the replay mustn't make again are recorded (netplay::journal).
            test.replayMode = GetSettings().replayMode;
            test.untracked = (test.replayMode && GetSettings().untrackedReplay) || !Tracking();
            if (test.replayMode)
            {
                test.discardedAtSave = gpu::GetDiscardStatistics();
                journal::Record();
            }
            if (Tracking())
            {
                tracker::SetBeforeWrite(SaveBeforeWrite);
                // The pages the game shares with others are tracked by bytes, to load only the game's; the savestates
                // saved at every frame have the game's bytes from where they lie instead.
                if (!test.pages)
                {
                    for (const auto& [page, writers] : test.knownWriters)
                    {
                        if (Shared(game, page, writers))
                            test.byteTracked.push_back(page);
                    }
                    tracker::TrackBytes(test.byteTracked); // kept as they are now, then tracked by bytes
                }
                Collect(game, tracker::NextFrame(), test.knownWriters);
            }
            LOG_INFO("Sync test: saved at %s's boundary; running %llu frames%s", StartText(test).c_str(), (unsigned long long)GetSettings().frames,
                test.pages ? " (savestates saved at every frame)" : "");
        }

        // Sorts the pages written by writer: the game's alone are loaded and compared, and its stacks and heap; never the
        // process heap's.
        void Classify(const GameState& game, Test& test)
        {
            test.gameHeap = heaps::Ranges(heaps::GameHeap());
            test.processHeap = heaps::Ranges(heaps::ProcessHeap());
            // Shared pages tracked by bytes: the game's bytes, or nothing if the game changed none.
            for (uint32_t page : test.byteTracked)
            {
                const auto owners = test.owners.find(page);
                if (owners == test.owners.end() || owners->second.game.none() || !Saved(page))
                    continue;
                test.loaded.push_back(page);
                test.shared[page] = owners->second.game & ~owners->second.others;
            }
            for (const auto& [page, writers] : test.firstWriters)
            {
                if (std::binary_search(test.byteTracked.begin(), test.byteTracked.end(), page))
                    continue;
                const uint32_t address = page * kPageSize;
                if (InGameMemory(test, address))
                {
                    if (Saved(page))
                        test.loaded.push_back(page);
                    else
                        test.unsaved++;
                    continue;
                }
                if (InRanges(test.processHeap, address))
                {
                    test.others.push_back(page);
                    continue;
                }
                // In replay mode, file reads count for neither: the journal replays them (fileReads).
                bool byGame = false;
                bool byOthers = address >= kMmioStart && address < kMmioEnd;
                const auto count = [&](const Writer& writer) {
                    if (writer.function != tracker::kFileRead || !test.replayMode)
                        (IsGameWriter(game, writer) ? byGame : byOthers) = true;
                };
                for (const Writer& writer : writers)
                    count(writer);
                if (const auto known = test.knownWriters.find(page); known != test.knownWriters.end())
                {
                    for (const Writer& writer : known->second)
                        count(writer);
                }
                if (byGame && !byOthers)
                {
                    if (Saved(page))
                        test.loaded.push_back(page);
                    else
                        test.unsaved++;
                }
                else if (byGame)
                    test.mixed.push_back(page);
                else
                    test.others.push_back(page);
            }
            std::sort(test.loaded.begin(), test.loaded.end());
        }

        // The bytes of the pages compared that aren't: the game's timers, what file reads wrote, others' bytes on shared
        // pages, and the stacks below their pointers.
        void MarkUnchecked(const GameState& game, Test& test)
        {
            const auto skip = [&](uint32_t start, uint32_t end) {
                for (uint32_t address = start; address < end; address++)
                {
                    std::vector<uint8_t>& mask = test.unchecked[address / kPageSize];
                    mask.resize(kPageSize);
                    mask[address % kPageSize] = 1;
                }
            };
            for (const auto& [start, end] : game.unchecked)
                skip(start, end);
            // What the first run's file reads wrote, which the load leaves (and the replay doesn't read again).
            for (const auto& [start, end] : test.fileReads)
            {
                if (start < tracker::kWatchEnd)
                    skip(start, std::min(end, tracker::kWatchEnd));
            }
            for (const auto& [page, loadedBytes] : test.shared)
            {
                std::vector<uint8_t>& mask = test.unchecked[page];
                mask.resize(kPageSize);
                for (uint32_t i = 0; i < kPageSize; i++)
                    mask[i] |= !loadedBytes[i];
            }
            // Within the pages compared.
            for (const auto& [start, end] : test.uncompared)
            {
                for (uint32_t page = start / kPageSize; page <= (end - 1) / kPageSize; page++)
                {
                    if (std::binary_search(test.loaded.begin(), test.loaded.end(), page))
                        skip(std::max(start, page * kPageSize), std::min(end, (page + 1) * kPageSize));
                }
            }
        }

        // With the savestates saved at every frame: the pages compared are those the page log copied in the first run
        // (the game's memory as the game describes it, GameState::savedMemory), only the game's bytes where it shares a
        // page; and how that compares with the pages Classify's rule, by writer, makes the game's.
        void ClassifyPages(const GameState& game, Test& test)
        {
            Classify(game, test);
            test.learned = std::move(test.loaded);
            test.loaded.clear();
            test.shared.clear();
            for (const auto& [page, contents] : test.loggedSaved)
                test.loaded.push_back(page);
            std::sort(test.loaded.begin(), test.loaded.end());
            for (const auto& [page, mask] : test.saved.partial)
            {
                const uint32_t number = page / kPageSize;
                if (std::binary_search(test.loaded.begin(), test.loaded.end(), number))
                    test.shared[number] = *mask;
            }
            // The learned rule's pages the savestate doesn't hold, and the pages it holds that others wrote.
            for (uint32_t page : test.learned)
            {
                if (!test.watched[page])
                    test.learnedNotWatched.push_back(page);
            }
            for (const auto& [page, writers] : test.firstWriters)
            {
                if (page >= test.watched.size() || !test.watched[page] || test.shared.count(page))
                    continue;
                for (const Writer& writer : writers)
                {
                    const bool byGame = InGameMemory(test, page * kPageSize) ? game.isGameThread(writer.thread) : IsGameWriter(game, writer);
                    if (!byGame && writer.function != tracker::kFileRead)
                    {
                        test.watchedByOthers.push_back(page);
                        break;
                    }
                }
            }
        }

        // Notes a boundary at which the game's other threads weren't all waiting.
        void NoteBusy(const GameState& game, Test& test, uint64_t boundary, const char* run)
        {
            std::vector<ThreadSite> threads;
            if (const std::string error = GameThreadSites(game, threads); !error.empty() && !test.busy.count(boundary))
                test.busy[boundary] = std::string(run) + ": " + error;
        }

        // With the savestates saved at every frame (Settings::pages): loads `back` frames back from the first run's end and
        // replays from there, its boundaries compared as the replay of the test's own savestate's are.
        void StartRollback(const GameState& game, uint32_t back)
        {
            Test& test = g_test;
            test.back = back;
            Test::Rollback& rollback = test.rollbacks.emplace_back();
            rollback.back = back;
            rollback.target = test.end - back;
            std::string note;
            const std::string error = savestates::Load(rollback.target, test.hostNote.empty() ? &test.hostNote : &note);
            if (!error.empty())
            {
                if (Tracking() && tracker::Active())
                    tracker::Stop();
                Finish("loading " + std::to_string(back) + " frames back: " + error);
                return;
            }
            const savestates::Statistics loaded = savestates::GetStatistics();
            rollback.loadUs = loaded.loadUs;
            rollback.hostLoadUs = loaded.hostLoadUs;
            rollback.pagesLoaded = loaded.loadPages;
            // The load's own check: every page compared as it was at the target boundary.
            for (uint32_t page : test.loaded)
            {
                const uint8_t* expected = Expected(test, page, rollback.target);
                if (!expected)
                    continue;
                const Difference difference = Compare(test, page, expected, guest::ToHost(page * kPageSize));
                if (!difference.bytes)
                    continue;
                if (!rollback.loadMismatches++)
                    rollback.firstMismatch = DescribePage(test, difference, expected, guest::ToHost(page * kPageSize));
            }
            input::Replay(rollback.target, test.end);
            test.phase = Phase::kReplay;
            test.differing.clear();
            test.rollbackBookkeepingMs = 0;
            test.replayStart = std::chrono::steady_clock::now();
            if (test.replayMode)
            {
                if (back == 1)
                    test.discardedBefore = gpu::GetDiscardStatistics();
                journal::ReplayFrom(rollback.target);
                replay::Begin();
            }
        }

        void FirstRunBoundary(const GameState& game, uint64_t boundary)
        {
            Test& test = g_test;
            std::vector<Write> writes;
            if (Tracking())
            {
                writes = tracker::NextFrame();
                test.firstWrites.push_back(writes.size());
                Collect(game, writes, test.firstWriters);
            }
            if (const std::optional<uint64_t> checksum = FightChecksum())
                test.checksums[boundary] = *checksum;
            if (game.hostStates)
                test.hostStates[boundary] = game.hostStates();
            std::set<uint32_t> written(test.byteTracked.begin(), test.byteTracked.end());
            for (const Write& write : writes)
                written.insert(write.page);
            if (test.pages)
            {
                // The frame's pages as the page log copied them (the game's memory, physical too), each kept as it was at
                // the save; and the write tracker's writes to the game's memory the page log didn't see.
                std::set<uint32_t> logged;
                const auto frames = pagelog::FramePages();
                if (!frames.empty() && frames.back().first == boundary - 1)
                {
                    for (uint32_t page : frames.back().second)
                    {
                        if (page != ~0u)
                            logged.insert(page);
                    }
                }
                if (!Tracking())
                    test.firstWrites.push_back(logged.size());
                for (uint32_t page : logged)
                {
                    written.insert(page);
                    if (test.loggedSaved.count(page))
                        continue;
                    const uint8_t* contents = pagelog::PageAt(page, boundary - 1);
                    if (contents)
                        test.loggedSaved[page].assign(contents, contents + kPageSize);
                }
                for (const Write& write : writes)
                {
                    if (write.page < test.watched.size() && test.watched[write.page] && !logged.count(write.page) &&
                        write.writer.function != tracker::kFileRead)
                    {
                        test.missed[write.page].insert(write.writer);
                        test.missedFrame.emplace(write.page, boundary - 1);
                    }
                }
            }
            for (uint32_t page : written)
            {
                const uint8_t* contents = guest::ToHost(page * kPageSize);
                test.after[page].emplace_back(boundary, std::vector<uint8_t>(contents, contents + kPageSize));
            }
            if (boundary < test.end)
            {
                NoteBusy(game, test, boundary, "first run");
                return;
            }

            // The end of the first run: load the savestate, if the game's threads and fibers are where they were.
            if (Tracking())
            {
                tracker::ByteTracking tracking = tracker::StopTrackingBytes();
                test.byteSteps = tracking.steps;
                test.byteLost = tracking.lost;
                for (const tracker::ByteWrites& writes : tracking.writes)
                {
                    Test::Owners& owners = test.owners[writes.page];
                    if (IsGameWriter(game, writes.writer))
                        owners.game |= writes.bytes;
                    else
                    {
                        owners.others |= writes.bytes;
                        owners.otherWriters[writes.writer] |= writes.bytes;
                    }
                }
                tracker::SetBeforeWrite(nullptr);
            }
            // The first run's file reads, which the journal replays (merged).
            if (test.replayMode)
                test.fileReads = journal::ReadRanges();
            for (const auto& [start, end] : test.fileReads)
                test.fileReadBytes += end - start;
            if (test.pages)
                ClassifyPages(game, test);
            else
                Classify(game, test);
            MarkUnchecked(game, test);
            std::vector<ThreadSite> threads;
            std::string error = GameThreadSites(game, threads);
            if (error.empty() && threads != test.threads)
            {
                error = "the game's threads wait elsewhere:";
                for (const ThreadSite& site : threads)
                {
                    if (std::find(test.threads.begin(), test.threads.end(), site) == test.threads.end())
                        error += " " + SiteText(site) + ";";
                }
            }
            if (test.pages)
            {
                if (!error.empty())
                {
                    if (Tracking())
                        tracker::Stop();
                    Finish(error);
                    return;
                }
                // Untracked: the replays run without the tracker's faults.
                if (test.untracked && Tracking())
                    tracker::Stop();
                StartRollback(game, 1);
                return;
            }
            const auto loadStart = std::chrono::steady_clock::now();
            if (error.empty())
                error = test.host.load(&test.hostNote);
            if (!error.empty())
            {
                tracker::Stop();
                Finish(error);
                return;
            }
            // In replay mode, what the first run's file reads overwrote, back first (the game's pages, read into or not,
            // are the savestate's): the replay writes what they read where the first run's game found it.
            if (test.replayMode)
                journal::RewindReads();
            for (uint32_t page : test.loaded)
            {
                const auto shared = test.shared.find(page);
                // A heap may have decommitted or released it meanwhile: writing it would fault.
                MEMORY_BASIC_INFORMATION memory{};
                if (!VirtualQuery(guest::ToHost(page * kPageSize), &memory, sizeof(memory)) || memory.State != MEM_COMMIT)
                {
                    test.uncommitted.push_back(page);
                    continue;
                }
                tracker::WriteUntracked(page, Saved(page), shared != test.shared.end() ? &shared->second : nullptr);
            }
            // The physical memory's pages that differ (the GPU's memory watch may have them protected: it's told first).
            const auto physicalStart = std::chrono::steady_clock::now();
            for (const auto& [start, contents] : test.physical)
            {
                for (uint32_t offset = 0; offset < contents.size(); offset += kPageSize)
                {
                    const uint32_t size = std::min<uint32_t>(kPageSize, uint32_t(contents.size()) - offset);
                    uint8_t* page = guest::ToHost(start + offset);
                    if (memcmp(page, contents.data() + offset, size) == 0)
                        continue;
                    gpu::watch::PrepareHostWrite(start + offset, size);
                    memcpy(page, contents.data() + offset, size);
                    test.physicalPagesLoaded++;
                }
            }
            test.physicalLoadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - physicalStart).count();
            // Untracked: the replay runs without the tracker's faults (its pages aren't compared).
            if (test.untracked)
                tracker::Stop();
            SetFrame(test.start);
            input::Replay(test.start, test.end);
            test.phase = Phase::kReplay;
            // Replay mode: the frames from here to the test's end are replays (netplay::replay), whose journaled calls
            // the record answers.
            test.replayStart = std::chrono::steady_clock::now();
            test.loadMs = std::chrono::duration<double, std::milli>(test.replayStart - loadStart).count();
            if (test.replayMode)
            {
                test.discardedBefore = gpu::GetDiscardStatistics();
                journal::Replay();
                replay::Begin();
            }
        }

        void ReplayBoundary(const GameState& game, uint64_t boundary)
        {
            Test& test = g_test;
            const auto entered = std::chrono::steady_clock::now();
            // With the savestates saved at every frame, the last rollback's end ends the test.
            const bool last = !test.pages || test.back >= GetSettings().frames;
            if (boundary == test.end && test.replayMode)
            {
                // The replay's over: the frames from here on are run the first time. Its statistics once the command
                // processor has executed its frames (it runs behind the game: a frame or two).
                test.replayEnd = entered;
                replay::End();
                if (last)
                {
                    journal::Stop();
                    test.journal = journal::GetStatistics();
                    for (int i = 0; i < 100 && gpu::SwapsPending(); i++)
                        Sleep(1);
                    test.discarded = gpu::GetDiscardStatistics();
                }
                else
                    journal::FinishReplay();
            }
            Test::Rollback* rollback = test.pages && !test.rollbacks.empty() ? &test.rollbacks.back() : nullptr;
            if (rollback && boundary == test.end)
                rollback->replayMs = std::chrono::duration<double, std::milli>(entered - test.replayStart).count() - test.rollbackBookkeepingMs;
            NoteBusy(game, test, boundary, "replay");
            if (const auto expected = test.checksums.find(boundary); expected != test.checksums.end())
            {
                if (const std::optional<uint64_t> checksum = FightChecksum())
                {
                    test.checksumsCompared++;
                    if (rollback)
                        rollback->checksums++;
                    if (*checksum != expected->second)
                    {
                        test.checksumsDiffering.push_back(boundary);
                        if (rollback)
                            rollback->checksumsDiffering++;
                    }
                }
            }
            if (const auto expected = test.hostStates.find(boundary); expected != test.hostStates.end() && game.hostStates)
            {
                test.hostStatesCompared++;
                if (rollback)
                    rollback->hostStates++;
                const std::vector<std::pair<uint32_t, uint64_t>> states = game.hostStates();
                if (states != expected->second)
                {
                    if (test.hostStatesDiffering.empty())
                    {
                        // The parts that differ (a fiber's record), or that one run has and the other hasn't.
                        for (const auto& part : states)
                        {
                            if (std::find(expected->second.begin(), expected->second.end(), part) == expected->second.end())
                                test.hostStatesFirst += (test.hostStatesFirst.empty() ? "" : ", ") + Hex(part.first);
                        }
                        if (test.hostStatesFirst.empty())
                            test.hostStatesFirst = "fewer parts than in the first run";
                    }
                    test.hostStatesDiffering.push_back(boundary);
                    if (rollback)
                        rollback->hostStatesDiffering++;
                }
            }
            // The rollback ends here; the next starts at once, from this boundary.
            const auto next = [&]() {
                if (!last && boundary == test.end)
                {
                    StartRollback(game, test.back + 1);
                    return true;
                }
                return false;
            };
            if (test.untracked && !test.pages)
            {
                if (boundary == test.end)
                    Finish({});
                else
                    test.bookkeepingMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - entered).count();
                return;
            }
            if (!test.untracked)
            {
                const std::vector<Write> writes = boundary < test.end || !last ? tracker::NextFrame() : tracker::Stop();
                for (uint32_t page : Collect(game, writes, test.replayWriters))
                {
                    if (!test.firstWriters.count(page) && !test.knownWriters.count(page))
                        test.newPages[boundary].insert(page);
                }
                for (const Write& write : writes)
                {
                    // In the game's memory, the libraries' code on the game's threads is fine (a D3D call's frame on the main
                    // thread's stack, the heap's books): other threads aren't.
                    const bool other = InGameMemory(test, write.page * kPageSize) ? !game.isGameThread(write.writer.thread)
                                                                                   : !IsGameWriter(game, write.writer);
                    if (other && std::binary_search(test.loaded.begin(), test.loaded.end(), write.page) && !test.shared.count(write.page))
                        test.othersWrote.insert(write.page);
                }
            }
            std::vector<Difference> differences;
            for (uint32_t page : test.loaded)
            {
                const uint8_t* expected = Expected(test, page, boundary);
                if (!expected)
                    continue;
                const Difference difference = Compare(test, page, expected, guest::ToHost(page * kPageSize));
                if (difference.bytes || difference.pointerWords)
                    differences.push_back(difference);
            }
            if (rollback && std::any_of(differences.begin(), differences.end(), [](const Difference& d) { return d.bytes != 0; }))
                rollback->boundariesDiffering++;
            // The pages that differ now and didn't at the boundary before: with bytes that differ first, then those
            // that differ only in pointers.
            std::vector<Difference> fresh;
            for (const Difference& difference : differences)
            {
                if (!test.differing.count(difference.page))
                    fresh.push_back(difference);
            }
            // The image's pages first among those: its variables say more than large arrays elsewhere (vertices, bones).
            std::stable_sort(fresh.begin(), fresh.end(), [](const Difference& a, const Difference& b) {
                return std::pair(a.bytes != 0, InImage(a.page)) > std::pair(b.bytes != 0, InImage(b.page));
            });
            if (!fresh.empty() && test.described < kDescribedPerTest)
            {
                test.details += "At boundary " + std::to_string(boundary) + " (after frame " + std::to_string(boundary - 1) + ")" +
                    (rollback ? ", replaying " + std::to_string(rollback->back) + " frames back" : std::string()) + ", " +
                    std::to_string(fresh.size()) + " pages differ that didn't before, first run > replay:\n";
                size_t described = 0;
                for (const Difference& difference : fresh)
                {
                    if (described == kDescribedPerBoundary || test.described == kDescribedPerTest)
                    {
                        test.details += "  (" + std::to_string(fresh.size() - described) + " more pages)\n";
                        break;
                    }
                    test.details += DescribePage(test, difference, Expected(test, difference.page, boundary), guest::ToHost(difference.page * kPageSize));
                    described++;
                    test.described++;
                }
            }
            test.differing.clear();
            for (const Difference& difference : differences)
                test.differing.insert(difference.page);
            if (!differences.empty())
                test.differences[boundary] = std::move(differences);
            const double bookkeeping = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - entered).count();
            if (boundary == test.end)
            {
                if (next())
                    return;
                Finish({});
            }
            else
            {
                test.bookkeepingMs += bookkeeping;
                test.rollbackBookkeepingMs += bookkeeping;
            }
        }
    }

    bool Enabled()
    {
        return GetSettings().set && GetSettings().count != 0;
    }

    bool Active()
    {
        return g_active.load(std::memory_order_acquire);
    }

    void AfterUpdate()
    {
        if (g_test.phase != Phase::kIdle)
            tracker::Resample();
    }

    void OnFrame(uint64_t frame)
    {
        if (!Enabled())
            return;
        const GameState* game = GetGameState();
        switch (g_test.phase)
        {
        case Phase::kIdle:
        {
            const Settings& settings = GetSettings();
            if (g_testsRun >= settings.count)
                return;
            if (!game || !game->isGameThread || !game->isLibraryFunction || !game->saveHost)
            {
                LOG_ERROR("Sync test: the game doesn't describe its state (netplay::RegisterGameState)");
                g_testsRun = settings.count;
                return;
            }
            const uint64_t start = settings.Start(g_testsRun, g_nextStart);
            if (!settings.fight)
            {
                if (frame + kLearnFrames < start)
                    return;
                Learn(std::max(start, frame + 1), kUnknown);
                return;
            }
            // In fight frames: the save's game frame follows from the fight's frame now, or once it loads, from its first.
            if (const std::optional<uint64_t> fightFrame = FightFrame())
            {
                if (*fightFrame > start)
                {
                    // Passed (a test was set to start where this fight began, or before): the next start, in this fight.
                    LOG_WARN("Sync test: fight frame %llu passed before a test could start there", (unsigned long long)start);
                    g_testsRun++;
                    g_nextStart = start + settings.interval;
                    return;
                }
                const uint64_t at = frame + (start - *fightFrame);
                if (frame + kLearnFrames < at)
                    return;
                Learn(at, start);
                // A test can start where the last one ended, saving at once with what the tests before it learned.
                if (at == frame)
                    LearningBoundary(*game, frame);
            }
            else if (game->fightState && game->fightState() == GameState::FightState::kLoading)
                Learn(kUnknown, start);
            return;
        }
        case Phase::kLearning:
            LearningBoundary(*game, frame);
            return;
        case Phase::kFirstRun:
            FirstRunBoundary(*game, frame);
            return;
        case Phase::kReplay:
            ReplayBoundary(*game, frame);
            return;
        }
    }
}
