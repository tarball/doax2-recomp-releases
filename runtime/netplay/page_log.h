#pragma once

// The page log: guest memory as it was at each frame boundary, kept cheaply enough for rollback's savestates
// (netplay/savestates.h). Every store the guest makes checks the store map first (guest_memory.h, ppc_overrides.h), a
// byte for each 4 KB page, and so does host code before it writes guest memory (guest::BeforeHostWrite). The pages the
// log watches (Watch: the game's state) have their byte set at each frame boundary, and the frame's first write to such
// a page copies it into the frame's log before it's written, then clears the byte, so the frame's later writes to it
// cost nothing more. So a frame's log holds every page of the game's state the frame wrote, as it was at the frame's
// start: loading the logs from the newest back to a boundary's (each page from the oldest log that has it) puts the
// game's memory back as it was there. Pages of the physical views are logged once, under their A0000000 address,
// whichever view writes them; image pages the game shares with libraries are logged whole but loaded only where a mask
// says they're the game's (Watch's masks).
//
// The log is kept from Start on, and each boundary (Boundary, on the main thread, while the game's threads are idle)
// seals the frame's log and starts the next, keeping the last `frames` logs and as much memory as they need.
#include <bitset>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace netplay::pagelog
{
    inline constexpr uint32_t kPageSize = 4096;
    inline constexpr uint32_t kPageCount = 1u << 20; // the guest's 4 GB

    // What a page's bytes that are the game's are, where a page holds others' too (the image's variables).
    using Mask = std::bitset<kPageSize>;

    // The game's state in guest memory: page ranges [first, end) (page numbers), and pages whose bytes are only partly the
    // game's (with the mask of those that are). Physical memory under any view's address.
    struct Watched
    {
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        std::vector<std::pair<uint32_t, const Mask*>> masked; // sorted by page; the masks outlive the log
    };

    struct Statistics
    {
        uint64_t frames = 0;        // logs sealed
        uint64_t pages = 0;         // pages copied, in all
        uint64_t hostPages = 0;     // of them, copied before host code wrote them
        uint64_t misses = 0;        // store map misses (stores that called the log), in all
        uint64_t crossings = 0;     // of them, stores across two pages
        uint32_t lastPages = 0;     // pages in the last sealed frame's log
        uint32_t maxPages = 0;      // the most in a frame's log
        uint64_t bytesHeld = 0;     // memory the logs hold now
        uint32_t watchedPages = 0;  // pages watched now
        uint64_t overflows = 0;     // frames whose log couldn't hold every page (not loadable)
        double copyMicroseconds = 0; // the time the misses took, in all (all threads)
        // The pages copied, in all, by region: virtual memory below 0x40000000 (the game's own reservations), from
        // 0x40000000 (the heaps), the stacks (0x70000000 on), the image, physical memory.
        enum Region
        {
            kLow,
            kHeaps,
            kStacks,
            kImage,
            kPhysical,
            kRegionCount
        };
        uint64_t regionPages[kRegionCount] = {};
    };
    static const char* const kRegionNames[Statistics::kRegionCount] = { "below 0x40000000", "heaps", "stacks", "image", "physical" };

    // Starts logging at a frame boundary (the log of the frame about to run is `frame`'s), keeping `frames` logs, and
    // watching `watched`.
    void Start(uint64_t frame, uint32_t frames, const Watched& watched);
    void Stop();
    bool Active();

    // At a frame boundary, on the main thread, with the game's threads idle: seals the log of the frame that just ended
    // and starts `frame`'s, watching `watched` from now on (pages no longer watched are forgotten).
    void Boundary(uint64_t frame, const Watched& watched);

    // Whether the logs can put memory back to `frame`'s boundary (from the current one): every log from there on is
    // kept and complete.
    bool CanLoad(uint64_t frame);
    // At a frame boundary (after Boundary), with the game's threads idle: puts back every page the frames since
    // `frame`'s boundary wrote, as it was there, and makes the current frame `frame` (the logs from it on are dropped:
    // its replay logs them again). Returns the pages written back (those that weren't as they were there already).
    size_t Load(uint64_t frame);

    // Each sealed frame's pages (for checks against other records of what each frame wrote): the frame and the page
    // numbers, oldest first.
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> FramePages();
    // A page's contents at `frame`'s boundary, from the logs, or nullptr if no log from `frame` on has it (it's the same
    // now, if nothing outside the log wrote it).
    const uint8_t* PageAt(uint32_t page, uint64_t frame);

    Statistics GetStatistics();
    std::string StatisticsText();

    // The page census (DEBUG_PAGE_CENSUS=first[,count], diagnostics): every page of the guest's memory written in each
    // frame, physical memory included, which the store map sees as the page log does: from game frame `first`, for
    // `count` frames (default 120), each frame's first store to every page is recorded with its writer (the guest
    // thread's start address, and the guest function, or for host code the guest caller). At each boundary, from the
    // frame boundary (frame.h). Then a report by region, by whether the game's memory holds the page
    // (GameState::savedMemory) and by writer: a summary in the log, the pages with their writers in dumps/page_census.txt.
    void CensusOnFrame(uint64_t frame);

    // The page a guest address is logged under (the A0000000 view's for physical memory), or the address's own.
    uint32_t CanonicalPage(uint32_t page);
    // Whether the log watches a page (any view's): the game's memory, whole or in part. On the main thread, at a frame
    // boundary or in a frame (Boundary changes it).
    bool Watches(uint32_t page);
}
