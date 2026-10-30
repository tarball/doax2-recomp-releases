#pragma once

// DEBUG_FLOAT_SEARCH (debugging) finds guest variables by how their values change, like a cheat search:
// "frame:lo:hi/frame:lo:hi/..." (frames ascending; several searches separated by ';'). At the first frame,
// every committed, readable guest word whose big-endian float is in [lo, hi] is a candidate; at each later
// frame, candidates outside that frame's range drop out. The survivors are logged with their values.
#include <cstdint>

namespace debug
{
    // Called once per presented frame.
    void FloatSearchOnFrame(uint64_t frame);

    // DEBUG_WATCH_CHANGES=address[,address...] (debugging; hex guest addresses): logs each of those 32-bit words
    // whenever it changed since the last presented frame, with the frame. Called once per presented frame.
    void WatchChangesOnFrame(uint64_t frame);
}
