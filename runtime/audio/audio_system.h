#pragma once

// The 360 system audio driver. Clients (the game's XAudio) register a callback that the driver calls
// whenever it wants another frame; the client renders one and submits it. A frame is 256 samples of
// 5.1 audio (6 channels, 48 kHz) as big-endian floats stored channel after channel (planar).
#include <cstdint>

namespace audio
{
    inline constexpr uint32_t kFrameSamples = 256;
    inline constexpr uint32_t kChannels = 6;
    inline constexpr uint32_t kSampleRate = 48000;

    // Returns a client index, or -1 if all slots are taken.
    int RegisterClient(uint32_t callback, uint32_t callbackArgument);
    void UnregisterClient(int index);
    void SubmitFrame(int index, uint32_t samples);

    // With AUDIO_DUMP, how many seconds of audio the dump holds so far (else -1): frame dumps log it, to
    // line pictures up with sound.
    double DumpedSeconds();

    // Frames clients have submitted (the status line shows it: the game's audio clock, which movies follow).
    uint64_t SubmittedFrames();
}
