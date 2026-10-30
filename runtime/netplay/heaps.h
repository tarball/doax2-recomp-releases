#pragma once

// The guest's heaps, for netplay's savestates. The game's hooks on its heap functions (runtime/game/) report each call
// here: whose an allocation is, which memory each heap holds, and the heap census.
//
// Whose: an allocation belongs to the first guest function up its call chain that isn't an allocator
// (GameState::isAllocatorFunction), which is the game's code or a library's, on one of the game's threads or another;
// for XMemAlloc's allocations, the allocator ID its caller passed decides (0-127 the game's, 128 and up Microsoft's
// libraries: D3D 128, XAudio 130, XAPI 131, XACT 132, ...), since a library's code can lie where the game's is expected,
// but for the functions that make a library's objects for the game (GameState::makesForGame).
//
// The game heap (GAME_HEAP=1, and with a sync test): the game's hooks give the game's allocations a heap of their own,
// a second instance of the game's heap code beside the process heap, which the libraries share with the game otherwise.
// Its memory, the blocks and the heap's bookkeeping alike, is then the game's alone, and a savestate can hold it whole:
// the game allocates and frees during play (effects in Watch mode), which a savestate of the blocks alone in a shared
// heap couldn't undo. The memory each heap holds is the virtual memory reserved while one of its functions runs.
//
// DEBUG_HEAP_CENSUS=first,count (diagnostics): every allocation and free is recorded from boot with its owner, and for
// `count` frames from game frame `first` they're counted by heap, owner and thread. When the window ends, the log has a
// summary and dumps/heap_census.txt the details: the heaps, the rates, the frames with the game's allocations or frees,
// the live blocks by owner and address, and the pages that hold both the game's blocks and the libraries'.
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace netplay::heaps
{
    inline constexpr int kNoAllocatorId = -1;

    // Whether the game heap is used (GAME_HEAP=1, or a sync test or the savestates saved at every frame, SAVESTATES:
    // the game's memory holds it).
    bool GameHeapEnabled();
    // Whether an allocation is the game's. `lr` and `r1` as at the call to the heap function: the caller's return address
    // and stack pointer, which give its call chain. `allocatorId`: XMemAlloc's, or kNoAllocatorId.
    bool IsGameAllocation(uint32_t lr, uint32_t r1, int allocatorId);

    // The heaps, as the game's hooks create them.
    void SetProcessHeap(uint32_t heap);
    void SetGameHeap(uint32_t heap);
    uint32_t ProcessHeap();
    uint32_t GameHeap();
    // Around each heap function's call, on the calling thread: virtual memory reserved in between is the heap's (for
    // RtlCreateHeap, `heap` is its result).
    void BeginHeapCall();
    void EndHeapCall(uint32_t heap);
    // From NtAllocateVirtualMemory and NtFreeVirtualMemory: a reservation (`lr` and `r1` as at the call, as for
    // IsGameAllocation), and a release.
    void VirtualReserved(uint32_t base, uint32_t size, uint32_t lr, uint32_t r1);
    void VirtualReleased(uint32_t base);
    // From NtFreeVirtualMemory: pages decommitted.
    void VirtualDecommitted(uint32_t base);
    // The changes to the game's memory map since boot: regions the game heap reserved (new segments, large blocks), and
    // pages of the game heap's or the game's regions decommitted and released. A savestate from before such a change
    // can't put that memory back (its pages are no longer there, or the heap's books from before don't know them), so a
    // rollback test reports those in its frames (netplay/rollback_test.h).
    struct GameMemoryChangeCounts
    {
        uint64_t reserved = 0, decommitted = 0, released = 0;
    };
    GameMemoryChangeCounts GameMemoryChanges();
    // The virtual memory the game reserved itself, outside the heaps (DOA4: the regions it reserves through XAPI's
    // VirtualAlloc and manages itself), [start, end).
    std::vector<std::pair<uint32_t, uint32_t>> GameRegions();
    // The memory a heap holds, [start, end), and whether an address lies in the game heap's.
    std::vector<std::pair<uint32_t, uint32_t>> Ranges(uint32_t heap);
    bool InGameHeap(uint32_t address);

    // Physical memory's blocks (XAPI's XPhysicalAlloc and XPhysicalFree, from the game's hooks), whose each is as for a
    // heap's allocation (`lr`, `r1` and `allocatorId` as for IsGameAllocation), for the game's description of its state
    // in physical memory, which the write tracker doesn't cover (GameState::physicalState). Not every block the game's
    // code allocates holds the game's state alone: it reads files there for libraries too (DOA4's sound settings for
    // XACT).
    struct PhysicalBlock
    {
        uint32_t block = 0, size = 0; // its address, and its size in whole pages
        uint32_t function = 0;        // the first function up its allocation's chain that isn't an allocator
        bool game = false;            // the game's, as an allocation from a heap would be
    };
    void PhysicalAllocated(uint32_t block, uint32_t size, uint32_t lr, uint32_t r1, int allocatorId);
    void PhysicalFreed(uint32_t block);
    // The blocks allocated and not freed, by address.
    std::vector<PhysicalBlock> PhysicalBlocks();

    // DEBUG_GAME_HEAP_CHECK=1 (debugging): the game's check of the game heap's books, run at each frame boundary while
    // no game thread uses the heap. It returns what's wrong, or an empty string; the first failure is logged, with the
    // game's report of what happened to the heap lately (`recent`).
    bool GameHeapCheckEnabled();
    void SetGameHeapCheck(std::function<std::string()> check, std::function<std::string()> recent);

    // The heap census (DEBUG_HEAP_CENSUS), from the game's hooks after each call.
    bool CensusEnabled();
    void Created(uint32_t heap, uint32_t flags, uint32_t reserve, uint32_t lr, uint32_t r1);
    void Allocated(uint32_t heap, uint32_t block, uint32_t size, uint32_t lr, uint32_t r1, int allocatorId);
    void Freed(uint32_t heap, uint32_t block, uint32_t lr, uint32_t r1);
    void Reallocated(uint32_t heap, uint32_t oldBlock, uint32_t newBlock, uint32_t size, uint32_t lr, uint32_t r1);
    // At each frame boundary.
    void OnFrame(uint64_t frame);
}
