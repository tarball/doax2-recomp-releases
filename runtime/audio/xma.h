#pragma once

// XMA decoding, for tests (the XMA* kernel exports are in xma.cpp).
#include <cstdint>
#include <vector>

namespace audio
{
    // The decoder's registers (MMIO).
    inline constexpr uint32_t kXmaRegisterPage = 0x7FEA0000;
    inline constexpr uint32_t kXmaRegisterPageSize = 0x10000;

    // Allocates the context array and publishes its address in the register page (which the caller reserves), before
    // the game reads it (see xma.cpp).
    void InitializeXmaRegisters();

    struct XmaTestDecode
    {
        bool ok = false;              // FFmpeg is there and decoded every packet
        std::vector<int16_t> samples; // interleaved
        uint32_t frames = 0;          // frames read (when repacking)
    };

    // Decodes a standalone XMA stream (2 KB packets in guest physical memory at `physical`) as contexts decode
    // it, reading its frames and repacking them for FFmpeg, or, if `direct`, by passing its packets to FFmpeg as
    // they are.
    XmaTestDecode DecodeXmaForTest(uint32_t physical, uint32_t packets, uint32_t channels, uint32_t sampleRate, bool direct);
}
