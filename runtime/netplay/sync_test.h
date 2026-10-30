#pragma once

// A sync test of savestates, as GGPO's sync test mode does. DEBUG_SYNC_TEST=first[,frames[,count[,interval]]]: at game
// frame `first`'s boundary (netplay/frame.h) it saves the game's state, runs `frames` frames (default 8) recording
// their pad reads and what they write, then loads the savestate and runs the same frames again with the same input,
// comparing the game's memory at each frame boundary with the first run's. Differences show state the savestate
// misses, or nondeterminism. `count` tests (default 1), `interval` frames apart (default 120, at least `frames` + 1: a
// test can start where the last ended). With F before `first` (F0,30,10,40), the starts count fight frames
// (netplay::FightFrame), and a start under 60 learns while the fight loads; starts can also be listed, first/second/...
// (F1810/2250,30), in place of `count` and `interval`. Results in the log, details in dumps/sync_test_<frame>.txt: at each
// boundary, the pages that newly differ (the image's first), with their writers and words, and every page that differs;
// the fibers the load put back elsewhere than they were; and the pages left out (mixed, others').
//
// The savestate is what rollback would save at a frame boundary:
// - Guest memory: the game's stacks, its heap (netplay/heaps.h) whole, and the pages the test's frames write if only
//   the game's threads write them, in the game's own code (GameState in netplay/game_state.h). Whose pages are whose is
//   learned from the writes of the 60 frames before the save, which the write tracker (netplay/write_tracker.h)
//   samples every millisecond as well as at frame boundaries and after each frame's update. The tracker also keeps
//   each page's contents before its first write in the test. Of the image's pages that libraries or other threads
//   write too, the savestate holds the bytes the game changed and others didn't in the first run, during which every
//   write to them is single-stepped; other such pages ("mixed") are neither loaded nor compared, nor are the stacks
//   below their stack pointers, the worker pools' stacks, or what the game lists as unchecked (timers).
//   The image's pages' writers seen in earlier tests count as learned too.
// - Host state: the game's suspended fibers' contexts, host stacks and registers (GameState::saveHost), which a load
//   puts back wherever the fibers are suspended then; at each boundary of the replay their hashes are compared with the
//   first run's (GameState::hostStates).
// The game's other threads must be blocked in waits at both boundaries, in the same places. Frame times are fixed
// (netplay::FixedFrameTime), the game's other threads draw random numbers from streams of their own
// (netplay::SplitRandom), and replayed frames get the pad reads recorded the first time (netplay/input_log.h).
// Side effects aren't undone: replayed frames draw and play sounds again. Words that differ only as pointers into the
// stacks or physical memory are counted apart (a pool's threads taking other jobs, buffers handed out in another order).
#include <cstdint>

namespace netplay::synctest
{
    // Whether a sync test is set up (DEBUG_SYNC_TEST).
    bool Enabled();
    // Whether one is running (from the save to the end of the replay).
    bool Active();
    // At each frame boundary, on the main thread.
    void OnFrame(uint64_t frame);
    // After each frame's update (netplay::AfterUpdate): the writes later in the frame are sampled too.
    void AfterUpdate();
}
