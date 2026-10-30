#pragma once

// Savestates at every frame boundary, cheap enough for rollback: the game's memory through netplay's page log (each
// frame's pages as they were at its start, copied as the frame first writes them: netplay/page_log.h), the pages it
// watches being the game's state as the game describes it (GameState::savedMemory), and its host state saved at each
// boundary (GameState::saveHost: the fibers' host stacks and registers). A load puts the game back as it was at a
// boundary up to `frames` back, at a boundary.
//
// SAVESTATES=frames (diagnostics; a sync test with DEBUG_SYNC_TEST_SAVESTATE=1 starts them itself) saves every frame from
// the game's first boundary on, keeping `frames` (at least 1), and logs the cost every 600 frames; it turns the game heap
// on, as a sync test does (netplay/heaps.h: the game's memory holds it).
#include <cstdint>
#include <string>

namespace netplay::savestates
{
    // Starts saving at every boundary from this one (at a frame boundary, after OnFrame), keeping `frames`.
    void Start(uint32_t frames);
    void Stop();
    bool Active();

    // At each frame boundary, last in it (frame.h): seals the frame's page log and saves the host state.
    void OnFrame(uint64_t frame);

    // Whether `frame`'s boundary can be loaded now: saved, with every frame's log since.
    bool CanLoad(uint64_t frame);
    // At a frame boundary (after OnFrame): loads `frame`'s boundary, and makes it the current frame (SetFrame). Returns an
    // error (and loads nothing) or an empty string; with `note`, what the load changed of the host state.
    // It first waits for the command processor to execute the draws of the frames recorded (gpu::WaitForRecordedDraws):
    // it runs up to a frame behind, and the frame it draws reads the vertices the game's memory holds (the skinning's, the
    // cloth's and hair's, the stage's bridge), which the load puts back and the replay writes again, so the last frame
    // shown before each rollback was drawn with the replay's (DEBUG_LOAD_NO_DRAW_WAIT=1 doesn't wait, for comparison).
    std::string Load(uint64_t frame, std::string* note = nullptr);

    struct Statistics
    {
        uint64_t saves = 0;            // boundaries saved
        double boundaryUs = 0;         // a save's work at the boundary, mean (the page log's seal, the host state)
        double boundaryMaxUs = 0;
        double hostUs = 0;             // of it, the host state's
        double copyUs = 0;             // the page log's copies during the frame, a frame's mean (all threads)
        double pages = 0;              // pages copied a frame, mean
        uint64_t loads = 0;
        double loadUs = 0;             // the last load's time
        size_t loadPages = 0;          // and the pages it wrote back
        double hostLoadUs = 0;         // of it, the host state's
        double drawWaitUs = 0;         // the last load's wait for the command processor's draws (before loadUs)
        double drawWaitTotalUs = 0;    // the loads' waits in all, and the longest
        double drawWaitMaxUs = 0;
        uint64_t drawWaits = 0;        // loads that waited at all
    };
    Statistics GetStatistics();
    std::string StatisticsText();
}
