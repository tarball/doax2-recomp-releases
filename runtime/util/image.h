#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace util
{
    // Decodes an image file held in memory (a PNG, or any format Windows' imaging component reads) into 32-bit BGRA rows,
    // top to bottom, `width` x `height` pixels: scaled to that size if both are given (as premultiplied colors, so that
    // transparent pixels don't bleed into others), else at its own, which they're set to. `premultiplied`: the colors
    // multiplied by alpha (for blending), else straight (as icons have them). False if it can't be decoded. Any thread
    // (COM is initialized for the call if the thread hasn't done it).
    bool DecodeImage(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height, bool premultiplied, std::vector<uint8_t>& bgra);
}
