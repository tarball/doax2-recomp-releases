#pragma once

// DEBUG_STATE_CENSUS=first,count (diagnostics for netplay's state saving): which guest pages each game frame writes, and
// who writes them, for `count` frames from frame `first` (netplay::Frame), through the write tracker (write_tracker.h).
// Reads of what frames may depend on besides guest memory and input are counted by thread and caller too: clocks (mftb,
// KeQuerySystemTime, and the tick count the game's hooks report), and draws from the game's random number generators
// (the game's hooks). When the window ends, the log has a summary and dumps/state_census.txt the details. Not covered:
// the physical memory views (A0000000 and up).
#include <cstdint>

namespace netplay::census
{
    // At a frame boundary, on the main thread.
    void OnFrame(uint64_t frame);

    // A guest read of `count` values from `source`, a clock (mftb, KeQuerySystemTime; the game's hooks report the tick
    // count, KeTimeStampBundle, which a host thread updates every millisecond) or a random number generator (the
    // game's hooks), by name. The guest function making it is the first one on the host stack.
    void NoteRead(const char* source, uint64_t count = 1);
    // Whether a census is running (cheap: checked by the sources on every read).
    bool Active();
}
