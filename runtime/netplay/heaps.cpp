#include "netplay/heaps.h"

#include "config.h"
#include "crash_handler.h"
#include "guest_memory.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/game_state.h"
#include "netplay/rollback_test.h"
#include "netplay/session.h"
#include "netplay/sync_test.h"

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
#include <unordered_map>
#include <vector>

namespace netplay::heaps
{
    namespace
    {
        constexpr uint32_t kPageSize = 4096;
        constexpr uint32_t kBlockHeader = 16; // bytes before a block that the heap's bookkeeping uses (about)

        struct Settings
        {
            uint64_t first = 0, count = 0;
            Settings()
            {
                if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_HEAP_CENSUS"))
                {
                    char* end = nullptr;
                    first = std::max<uint64_t>(strtoull(setting, &end, 10), 1);
                    count = end && *end == ',' ? std::max<uint64_t>(strtoull(end + 1, nullptr, 10), 1) : 600;
                }
            }
        };

        const Settings& GetSettings()
        {
            static const Settings settings;
            return settings;
        }

        // Whose an allocation is: the game's (its code, on its threads), a library's (a library's code, or any code on
        // another thread), or unknown (allocators all the way up the chain).
        enum class Kind : uint8_t
        {
            kGame,
            kLibrary,
            kUnknown,
        };

        const char* KindText(Kind kind)
        {
            return kind == Kind::kGame ? "game" : kind == Kind::kLibrary ? "library" : "unknown";
        }

        constexpr int kFirstLibraryAllocatorId = 128; // XMemAlloc's IDs from here on are Microsoft's libraries'

        struct Owner
        {
            uint32_t function = 0; // the first guest function up the chain that isn't an allocator (0: none)
            uint32_t thread = 0;   // the guest thread's start address (0x80000000 | its id for a host entry; 0: a host thread)
            int allocatorId = kNoAllocatorId;
            Kind kind = Kind::kUnknown;
            bool chainDisagrees = false; // the allocator ID says otherwise than the call chain
            auto Key() const { return std::tie(function, thread, allocatorId); }
            bool operator<(const Owner& other) const { return Key() < other.Key(); }
        };

        Owner OwnerOf(uint32_t lr, uint32_t r1, int allocatorId, std::vector<uint32_t>* chainOut)
        {
            Owner owner;
            owner.allocatorId = allocatorId;
            if (const kernel::GuestThread* thread = kernel::CurrentThread())
                owner.thread = thread->StartAddress() ? thread->StartAddress() : 0x80000000u | thread->ThreadId();
            const GameState* game = GetGameState();
            const bool gameThread = game && ((game->isGameThread && game->isGameThread(owner.thread)) ||
                                                (game->allocatesForGame && game->allocatesForGame(owner.thread)));
            std::vector<uint32_t> chain = guest::CallChain(r1, 16);
            chain.insert(chain.begin(), lr);
            for (uint32_t address : chain)
            {
                if (game && game->isAllocatorFunction && game->isAllocatorFunction(address))
                    continue;
                owner.function = crash::GuestFunctionContaining(address);
                if (game && game->isLibraryFunction)
                    owner.kind = !game->isLibraryFunction(address) && gameThread ? Kind::kGame : Kind::kLibrary;
                break;
            }
            if (allocatorId != kNoAllocatorId)
            {
                Kind byId = allocatorId < kFirstLibraryAllocatorId && gameThread ? Kind::kGame : Kind::kLibrary;
                if (byId == Kind::kLibrary && gameThread && owner.function && game->makesForGame && game->makesForGame(owner.function))
                    byId = Kind::kGame;
                owner.chainDisagrees = owner.kind != Kind::kUnknown && owner.kind != byId;
                owner.kind = byId;
            }
            if (chainOut)
                *chainOut = std::move(chain);
            return owner;
        }

        struct Counts
        {
            uint64_t allocations = 0, frees = 0, reallocations = 0, bytes = 0;
        };

        struct Block
        {
            uint32_t heap = 0, size = 0;
            Owner owner;
        };

        struct HeapInfo
        {
            uint32_t flags = 0, reserve = 0;
            Owner creator;
        };

        std::mutex g_lock;
        std::unordered_map<uint32_t, Block> g_blocks;                // live blocks, by address
        std::map<uint32_t, HeapInfo> g_heaps;                        // the heaps created
        std::map<std::pair<uint32_t, Owner>, Counts> g_window;       // (heap, owner) -> what it did in the window
        std::map<uint32_t, std::vector<uint32_t>> g_samples;         // owner function -> a call chain of one of its calls
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_crossFrees; // (allocating, freeing function) of other kinds, in the window
        uint64_t g_unknownFrees = 0;                                 // frees in the window of blocks not seen allocated
        bool g_inWindow = false;
        bool g_reported = false;
        // The frames in the window with the game's allocations or frees: frame -> allocations, frees, bytes, owner functions.
        struct FrameActivity
        {
            uint32_t allocations = 0, frees = 0;
            uint64_t bytes = 0;
            std::set<uint32_t> functions;
        };
        std::map<uint64_t, FrameActivity> g_gameFrames;

        // (g_lock held)
        void NoteGameActivity(const Owner& owner, bool allocation, uint32_t size)
        {
            if (!g_inWindow || owner.kind != Kind::kGame)
                return;
            FrameActivity& activity = g_gameFrames[Frame()];
            (allocation ? activity.allocations : activity.frees)++;
            activity.bytes += allocation ? size : 0;
            if (activity.functions.size() < 8)
                activity.functions.insert(owner.function);
        }

        // (g_lock held)
        void NoteChain(const Owner& owner, std::vector<uint32_t>& chain)
        {
            if (g_samples.size() < 4096 && !g_samples.count(owner.function))
                g_samples[owner.function] = std::move(chain);
        }

        // (g_lock held)
        void Forget(uint32_t block, const Owner& freer)
        {
            const auto it = g_blocks.find(block);
            if (it == g_blocks.end())
            {
                if (g_inWindow)
                    g_unknownFrees++;
                return;
            }
            if (g_inWindow && it->second.owner.kind != freer.kind)
                g_crossFrees[{ it->second.owner.function, freer.function }]++;
            g_blocks.erase(it);
        }

        // The heaps, and the memory each holds: the virtual memory reserved while one of its functions ran.
        std::atomic<uint32_t> g_processHeap{ 0 }, g_gameHeap{ 0 };
        std::mutex g_rangesLock;
        std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> g_ranges; // heap -> [start, end), sorted
        std::vector<std::pair<uint32_t, uint32_t>> g_gameRegions;                // the game's own reservations, sorted
        thread_local int t_heapCalls = 0;                                        // heap functions running on this thread
        thread_local std::vector<std::pair<uint32_t, uint32_t>> t_reserved;      // reserved by them so far
        // Changes to the game's memory map since boot (GameMemoryChanges): its heap's and its own regions'.
        std::atomic<uint64_t> g_gameReserved{ 0 }, g_gameDecommitted{ 0 }, g_gameReleased{ 0 };

        bool InGameMemoryMap(uint32_t address)
        {
            for (const auto& [start, end] : g_gameRegions)
            {
                if (address >= start && address < end)
                    return true;
            }
            const auto it = g_ranges.find(g_gameHeap.load(std::memory_order_relaxed));
            if (it == g_ranges.end())
                return false;
            return std::any_of(it->second.begin(), it->second.end(),
                [&](const std::pair<uint32_t, uint32_t>& range) { return address >= range.first && address < range.second; });
        }

        // Physical memory's blocks, by address.
        std::mutex g_physicalLock;
        std::map<uint32_t, PhysicalBlock> g_physical;

        std::string Hex(uint32_t value)
        {
            char text[16];
            snprintf(text, sizeof(text), "%08X", value);
            return text;
        }

        std::string OwnerText(const Owner& owner)
        {
            std::string text = owner.function ? "sub_" + Hex(owner.function) : std::string("(allocators only)");
            text += std::string(" [") + KindText(owner.kind) + "]";
            text += owner.thread ? " thread " + Hex(owner.thread) : std::string(" host thread");
            if (owner.allocatorId != kNoAllocatorId)
                text += " XMemAlloc id " + std::to_string(owner.allocatorId) + (owner.chainDisagrees ? " (its chain says otherwise)" : "");
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

        // (g_lock held)
        void Report()
        {
            const Settings& settings = GetSettings();
            const double frames = double(settings.count);
            const std::string path = config::DumpPath("heap_census.txt");
            g_reportFile = fopen(path.c_str(), "w");
            line("Heap census: frames %llu-%llu\n\n", (unsigned long long)settings.first, (unsigned long long)(settings.first + settings.count - 1));

            // The heaps, and their live blocks by kind.
            struct Live
            {
                uint64_t blocks[3] = {}, bytes[3] = {};
                uint32_t low = ~0u, high = 0;
            };
            std::map<uint32_t, Live> live;
            for (const auto& [address, block] : g_blocks)
            {
                Live& entry = live[block.heap];
                entry.blocks[int(block.owner.kind)]++;
                entry.bytes[int(block.owner.kind)] += block.size;
                entry.low = std::min(entry.low, address);
                entry.high = std::max(entry.high, address + block.size);
            }
            line("Heaps (created, and any that blocks were allocated from), with their live blocks at the end: game's, libraries', unknown:\n");
            std::set<uint32_t> heaps;
            for (const auto& [heap, info] : g_heaps)
                heaps.insert(heap);
            for (const auto& [heap, entry] : live)
                heaps.insert(heap);
            for (uint32_t heap : heaps)
            {
                line("  heap %08X:", heap);
                if (const auto info = g_heaps.find(heap); info != g_heaps.end())
                    line(" flags %X, reserve %X, created by %s;", info->second.flags, info->second.reserve, OwnerText(info->second.creator).c_str());
                if (const auto entry = live.find(heap); entry != live.end())
                {
                    const Live& l = entry->second;
                    line(" %llu blocks (%.1f KB), %llu (%.1f KB), %llu (%.1f KB), in %08X-%08X", (unsigned long long)l.blocks[0], l.bytes[0] / 1024.0,
                        (unsigned long long)l.blocks[1], l.bytes[1] / 1024.0, (unsigned long long)l.blocks[2], l.bytes[2] / 1024.0, l.low, l.high);
                }
                line("\n");
            }

            line("\nPer frame in the window, by heap and owner (the first function up the chain that isn't an allocator, and the thread):\n");
            line("  %8s %8s %8s %10s  owner\n", "allocs", "frees", "reallocs", "bytes");
            std::vector<std::pair<std::pair<uint32_t, Owner>, Counts>> sorted(g_window.begin(), g_window.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                if (a.first.first != b.first.first)
                    return a.first.first < b.first.first;
                return a.second.allocations + a.second.frees + a.second.reallocations > b.second.allocations + b.second.frees + b.second.reallocations;
            });
            uint32_t heap = 0;
            uint64_t totals[3][2] = {}; // kind -> allocations, frees
            for (const auto& [key, counts] : sorted)
            {
                if (key.first != heap)
                {
                    heap = key.first;
                    line("  heap %08X:\n", heap);
                }
                line("  %8.2f %8.2f %8.2f %10.1f  %s\n", counts.allocations / frames, counts.frees / frames, counts.reallocations / frames,
                    counts.bytes / frames, OwnerText(key.second).c_str());
                totals[int(key.second.kind)][0] += counts.allocations + counts.reallocations;
                totals[int(key.second.kind)][1] += counts.frees;
            }

            line("\nA call chain of each owner function's (innermost first, from the allocator's caller):\n");
            std::set<uint32_t> owners;
            for (const auto& [key, counts] : g_window)
                owners.insert(key.second.function);
            for (uint32_t function : owners)
            {
                const auto it = g_samples.find(function);
                if (it == g_samples.end())
                    continue;
                line("  sub_%08X:", function);
                for (uint32_t address : it->second)
                    line(" %08X", address);
                line("\n");
            }

            // Pages that hold live blocks: the game's only, others' only, or both (the heap's bookkeeping just before
            // each block counted with it).
            struct PageUse
            {
                uint32_t game = 0, others = 0;
                std::set<uint32_t> functions;
            };
            std::map<uint32_t, std::map<uint32_t, PageUse>> pages; // heap -> page -> use
            for (const auto& [address, block] : g_blocks)
            {
                const uint32_t start = address >= kBlockHeader ? address - kBlockHeader : 0;
                const uint32_t end = address + std::max<uint32_t>(block.size, 1);
                for (uint32_t page = start / kPageSize; page <= (end - 1) / kPageSize; page++)
                {
                    PageUse& use = pages[block.heap][page];
                    (block.owner.kind == Kind::kGame ? use.game : use.others)++;
                    if (use.functions.size() < 6)
                        use.functions.insert(block.owner.function);
                }
            }
            line("\nPages holding live blocks at the end, by heap: the game's blocks only, others' only, both:\n");
            size_t mixedListed = 0;
            for (const auto& [pageHeap, uses] : pages)
            {
                size_t game = 0, others = 0, both = 0;
                for (const auto& [page, use] : uses)
                    (use.game && use.others ? both : use.game ? game : others)++;
                line("  heap %08X: %zu, %zu, %zu\n", pageHeap, game, others, both);
                for (const auto& [page, use] : uses)
                {
                    if (!use.game || !use.others || mixedListed == 64)
                        continue;
                    line("    %08X: %u of the game's, %u others':", page * kPageSize, use.game, use.others);
                    for (uint32_t function : use.functions)
                        line(" sub_%08X", function);
                    line("\n");
                    mixedListed++;
                }
            }

            line("\nIn the window, blocks freed by another kind of owner than allocated them (allocating > freeing function):\n");
            for (const auto& [functions, count] : g_crossFrees)
                line("  %llu: sub_%08X > sub_%08X\n", (unsigned long long)count, functions.first, functions.second);
            line("Frees of blocks not seen allocated: %llu\n", (unsigned long long)g_unknownFrees);

            line("\nFrames with the game's allocations or frees (%zu of %llu):\n", g_gameFrames.size(), (unsigned long long)settings.count);
            size_t framesListed = 0;
            for (const auto& [frame, activity] : g_gameFrames)
            {
                if (framesListed++ == 400)
                {
                    line("  ...\n");
                    break;
                }
                line("  frame %llu: %u allocations (%llu bytes), %u frees:", (unsigned long long)frame, activity.allocations,
                    (unsigned long long)activity.bytes, activity.frees);
                for (uint32_t function : activity.functions)
                    line(" sub_%08X", function);
                line("\n");
            }

            line("\nLive blocks at the end, by address (address, size, owner):\n");
            std::vector<std::pair<uint32_t, const Block*>> blocks;
            blocks.reserve(g_blocks.size());
            for (const auto& [address, block] : g_blocks)
                blocks.emplace_back(address, &block);
            std::sort(blocks.begin(), blocks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [address, block] : blocks)
                line("  %08X %8X %s\n", address, block->size, OwnerText(block->owner).c_str());
            if (g_reportFile)
                fclose(g_reportFile);
            g_reportFile = nullptr;

            size_t mixed = 0;
            for (const auto& [pageHeap, uses] : pages)
            {
                for (const auto& [page, use] : uses)
                    mixed += use.game && use.others;
            }
            LOG_INFO("Heap census: %zu heaps; per frame, the game %.2f allocations and %.2f frees, libraries %.2f and %.2f; %zu live blocks, %zu "
                     "pages with both the game's and others'; details in %s",
                heaps.size(), totals[0][0] / frames, totals[0][1] / frames, totals[1][0] / frames, totals[1][1] / frames, g_blocks.size(), mixed,
                path.c_str());
        }
    }

    bool GameHeapEnabled()
    {
        static const bool enabled = [] {
            if (const char* setting = std::getenv(GAME_PREFIX "_GAME_HEAP"))
                return std::atoi(setting) != 0;
            // (A session that rolls back saves the game at every frame of its fights: session.h.)
            return synctest::Enabled() || rollbacktest::Enabled() || session::RollbackConfigured() ||
                std::getenv(GAME_PREFIX "_SAVESTATES") != nullptr;
        }();
        return enabled;
    }

    bool IsGameAllocation(uint32_t lr, uint32_t r1, int allocatorId)
    {
        return OwnerOf(lr, r1, allocatorId, nullptr).kind == Kind::kGame;
    }

    void SetProcessHeap(uint32_t heap)
    {
        g_processHeap.store(heap);
    }

    void SetGameHeap(uint32_t heap)
    {
        g_gameHeap.store(heap);
    }

    uint32_t ProcessHeap()
    {
        return g_processHeap.load(std::memory_order_relaxed);
    }

    uint32_t GameHeap()
    {
        return g_gameHeap.load(std::memory_order_relaxed);
    }

    void BeginHeapCall()
    {
        t_heapCalls++;
    }

    void EndHeapCall(uint32_t heap)
    {
        if (t_heapCalls > 0)
            t_heapCalls--;
        if (t_reserved.empty())
            return;
        if (heap && heap == g_gameHeap.load(std::memory_order_relaxed))
            g_gameReserved += t_reserved.size();
        std::lock_guard lock(g_rangesLock);
        auto& ranges = g_ranges[heap];
        ranges.insert(ranges.end(), t_reserved.begin(), t_reserved.end());
        std::sort(ranges.begin(), ranges.end());
        t_reserved.clear();
    }

    void VirtualReserved(uint32_t base, uint32_t size, uint32_t lr, uint32_t r1)
    {
        if (t_heapCalls > 0)
        {
            t_reserved.emplace_back(base, base + size);
            return;
        }
        // Outside the heaps: the game's own, if the game's code reserved it (through its allocators) on its threads.
        if (!IsGameAllocation(lr, r1, kNoAllocatorId))
            return;
        std::lock_guard lock(g_rangesLock);
        g_gameRegions.emplace_back(base, base + size);
        std::sort(g_gameRegions.begin(), g_gameRegions.end());
    }

    void VirtualDecommitted(uint32_t base)
    {
        std::lock_guard lock(g_rangesLock);
        if (InGameMemoryMap(base))
            g_gameDecommitted++;
    }

    GameMemoryChangeCounts GameMemoryChanges()
    {
        return { g_gameReserved.load(), g_gameDecommitted.load(), g_gameReleased.load() };
    }

    void VirtualReleased(uint32_t base)
    {
        std::lock_guard lock(g_rangesLock);
        if (InGameMemoryMap(base))
            g_gameReleased++;
        for (auto& [heap, ranges] : g_ranges)
        {
            ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [&](const std::pair<uint32_t, uint32_t>& range) { return range.first == base; }),
                ranges.end());
        }
        g_gameRegions.erase(std::remove_if(g_gameRegions.begin(), g_gameRegions.end(),
                                [&](const std::pair<uint32_t, uint32_t>& range) { return range.first == base; }),
            g_gameRegions.end());
    }

    std::vector<std::pair<uint32_t, uint32_t>> GameRegions()
    {
        std::lock_guard lock(g_rangesLock);
        return g_gameRegions;
    }

    void PhysicalAllocated(uint32_t block, uint32_t size, uint32_t lr, uint32_t r1, int allocatorId)
    {
        if (!block || !size)
            return;
        const Owner owner = OwnerOf(lr, r1, allocatorId, nullptr);
        PhysicalBlock entry;
        entry.block = block;
        entry.size = uint32_t(std::min<uint64_t>((uint64_t(size) + kPageSize - 1) & ~uint64_t(kPageSize - 1), 0x100000000ull - block));
        entry.function = owner.function;
        entry.game = owner.kind == Kind::kGame;
        std::lock_guard lock(g_physicalLock);
        g_physical[block] = entry;
    }

    void PhysicalFreed(uint32_t block)
    {
        std::lock_guard lock(g_physicalLock);
        g_physical.erase(block);
    }

    std::vector<PhysicalBlock> PhysicalBlocks()
    {
        std::vector<PhysicalBlock> blocks;
        std::lock_guard lock(g_physicalLock);
        for (const auto& [block, entry] : g_physical)
            blocks.push_back(entry);
        return blocks;
    }

    std::vector<std::pair<uint32_t, uint32_t>> Ranges(uint32_t heap)
    {
        std::lock_guard lock(g_rangesLock);
        const auto it = g_ranges.find(heap);
        return it != g_ranges.end() ? it->second : std::vector<std::pair<uint32_t, uint32_t>>{};
    }

    bool InGameHeap(uint32_t address)
    {
        const uint32_t heap = GameHeap();
        if (!heap)
            return false;
        std::lock_guard lock(g_rangesLock);
        const auto it = g_ranges.find(heap);
        if (it == g_ranges.end())
            return false;
        const auto& ranges = it->second;
        auto range = std::upper_bound(ranges.begin(), ranges.end(), std::make_pair(address, ~0u));
        if (range == ranges.begin())
            return false;
        --range;
        return address >= range->first && address < range->second;
    }

    bool GameHeapCheckEnabled()
    {
        static const bool enabled = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_GAME_HEAP_CHECK");
            return setting && std::atoi(setting) != 0;
        }();
        return enabled;
    }

    namespace
    {
        std::function<std::string()> g_check, g_recent;
        bool g_checkFailed = false;
    }

    void SetGameHeapCheck(std::function<std::string()> check, std::function<std::string()> recent)
    {
        g_check = std::move(check);
        g_recent = std::move(recent);
    }

    bool CensusEnabled()
    {
        return GetSettings().count != 0;
    }

    void Created(uint32_t heap, uint32_t flags, uint32_t reserve, uint32_t lr, uint32_t r1)
    {
        if (!CensusEnabled() || !heap)
            return;
        const Owner owner = OwnerOf(lr, r1, kNoAllocatorId, nullptr);
        std::lock_guard lock(g_lock);
        g_heaps[heap] = HeapInfo{ flags, reserve, owner };
    }

    void Allocated(uint32_t heap, uint32_t block, uint32_t size, uint32_t lr, uint32_t r1, int allocatorId)
    {
        if (!CensusEnabled() || !block)
            return;
        std::vector<uint32_t> chain;
        const Owner owner = OwnerOf(lr, r1, allocatorId, &chain);
        std::lock_guard lock(g_lock);
        g_blocks[block] = Block{ heap, size, owner };
        NoteGameActivity(owner, true, size);
        if (g_inWindow)
        {
            Counts& counts = g_window[{ heap, owner }];
            counts.allocations++;
            counts.bytes += size;
            NoteChain(owner, chain);
        }
    }

    void Freed(uint32_t heap, uint32_t block, uint32_t lr, uint32_t r1)
    {
        if (!CensusEnabled() || !block)
            return;
        std::vector<uint32_t> chain;
        const Owner owner = OwnerOf(lr, r1, kNoAllocatorId, &chain);
        std::lock_guard lock(g_lock);
        Forget(block, owner);
        NoteGameActivity(owner, false, 0);
        if (g_inWindow)
        {
            g_window[{ heap, owner }].frees++;
            NoteChain(owner, chain);
        }
    }

    void Reallocated(uint32_t heap, uint32_t oldBlock, uint32_t newBlock, uint32_t size, uint32_t lr, uint32_t r1)
    {
        if (!CensusEnabled())
            return;
        std::vector<uint32_t> chain;
        const Owner owner = OwnerOf(lr, r1, kNoAllocatorId, &chain);
        std::lock_guard lock(g_lock);
        if (oldBlock && newBlock)
            Forget(oldBlock, owner);
        if (newBlock)
            g_blocks[newBlock] = Block{ heap, size, owner };
        NoteGameActivity(owner, true, size);
        if (g_inWindow)
        {
            Counts& counts = g_window[{ heap, owner }];
            counts.reallocations++;
            counts.bytes += size;
            NoteChain(owner, chain);
        }
    }

    void OnFrame(uint64_t frame)
    {
        if (GameHeapCheckEnabled() && g_check && !g_checkFailed)
        {
            if (const std::string problem = g_check(); !problem.empty())
            {
                g_checkFailed = true;
                LOG_ERROR("Game heap check at frame %llu: %s", (unsigned long long)frame, problem.c_str());
                if (g_recent)
                    LOG_ERROR("Game heap, lately:\n%s", g_recent().c_str());
            }
        }
        const Settings& settings = GetSettings();
        if (!settings.count || g_reported)
            return;
        std::lock_guard lock(g_lock);
        if (frame == settings.first)
        {
            g_inWindow = true;
            LOG_INFO("Heap census: from frame %llu, for %llu frames", (unsigned long long)frame, (unsigned long long)settings.count);
        }
        else if (g_inWindow && frame >= settings.first + settings.count)
        {
            g_inWindow = false;
            g_reported = true;
            Report();
        }
    }
}
