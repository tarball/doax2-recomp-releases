#pragma once

// DEBUG_MEMORY_TRACE=[F]first,count:address+length[,address+length...] (debugging; addresses and lengths in hex) writes
// the guest memory in those ranges at each game frame boundary (netplay::Frame) for `count` frames from frame `first` (with
// F, from the first fight's frame `first`: netplay::FightFrame, which two runs or machines count alike), to
// dumps/memory_trace.bin, for finding a game's variables by how they change: "MTRC", the number of ranges and each
// range's address and length (little-endian 32-bit words), then for each frame its number (64-bit), its fight frame
// (netplay::FightFrame, 64-bit, -1 outside a fight) and the ranges' bytes, as guest memory holds them (big-endian).
#include <cstdint>

namespace debug
{
    // At a game frame boundary, on the main thread.
    void MemoryTraceOnFrame(uint64_t frame);
}
