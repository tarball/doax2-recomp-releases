#pragma once

#include <cstdint>

namespace util
{
    // Writes an 8-bit RGB PNG (uncompressed deflate) from RGBA rows; alpha is dropped. Debug output only.
    bool WritePng(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t rowPitch);
}
