#pragma once

// The pads' reads, frame by frame, recorded and played back: a sync test replays frames with the input they first saw.
// XamInputGetState asks Replayed before reading the host's pads, and passes what it read to Note.
#include <cstdint>

namespace netplay::input
{
    // One XamInputGetState: its result and the state it returned (packet number and gamepad, 16 bytes as the guest
    // sees them).
    struct PadRead
    {
        uint32_t result = 0;
        uint8_t state[16] = {};
    };

    // Records each frame's reads from now on (netplay::Frame).
    void Record();
    // Answers the reads in frames [from, to) from the record, then reads the host's pads again.
    void Replay(uint64_t from, uint64_t to);
    // Reads the host's pads and records nothing.
    void Stop();

    // True if this read is answered from the record (`read` filled in); then the host's pads aren't read.
    bool Replayed(uint32_t user, PadRead& read);
    // A read from the host's pads, recorded when recording.
    void Note(uint32_t user, const PadRead& read);
    // Reads while replaying that the record doesn't have (another pad, or more reads in a frame than recorded), which
    // read the host's pads instead.
    uint64_t Misses();
}
