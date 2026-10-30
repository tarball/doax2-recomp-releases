#pragma once

// The game's executable from its disc image: the supported version's default.xex, checked by its SHA-256, then
// decrypted and decompressed in memory into the image the recompiled code was generated from. A release reads it from
// the disc image every time it starts, so nothing from the game is written out; a development checkout can keep
// default.xex and default.pe in private/ instead.
#include <cstdint>
#include <string>
#include <vector>

namespace disc
{
    enum class Check
    {
        kOk,
        kUnreadable, // not an Xbox 360 disc image, or no default.xex in it
        kOtherGame,  // a default.xex, but not the supported version's
    };

    // Reads default.xex from the disc image into `xex`, and checks it.
    Check ReadExecutable(const std::string& isoPath, std::vector<uint8_t>& xex);
    // Whether `xex` is the supported version's default.xex (see disc_image.cpp).
    bool IsSupported(const std::vector<uint8_t>& xex);
    // Decrypts (retail key) and decompresses (basic compression) an XEX2 into its image. False if it can't.
    bool Unpack(const std::vector<uint8_t>& xex, std::vector<uint8_t>& image);
    // SHA-256 as 64 lowercase hex digits (empty if it can't be computed).
    std::string Sha256(const uint8_t* data, size_t size);
}
