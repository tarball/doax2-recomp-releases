#pragma once

// The title's SPA data: an XDBF file among the executable's resources (the XEX header's resource info names it by the
// title ID), which the image holds once loaded. XAM reads the title's achievements, strings and images from it; the
// runtime takes the window's icon (the title's image) and the achievements' names and pictures for its notices from it.
//
// XDBF (big-endian): a header (magic "XDBF", version, the entry table's length and its entries used, the free table's
// length and its entries used), the entry table (18 bytes an entry: section, a 64-bit ID, offset and size, the offset
// counted from the end of the free table), then the free table (8 bytes an entry). Sections: 1 metadata (by ID: XACH the
// achievements, XSTC the default language, XTHD the title, XPRP, XCXT, XRPT, XMAT, XVC2...), 2 images (PNGs by image ID,
// the title's 0x8000), 3 string tables (XSTR, one a language, by XLanguage: 1 English, 2 Japanese, 3 German, 4 French,
// 5 Spanish, 6 Italian, 7 Korean, 8 Traditional Chinese...; strings by ID, UTF-8, the title's name 0x8000).
// XACH: magic, version, size, a 16-bit count, then 0x24 bytes an achievement: its ID, the string IDs of its name, its
// description once earned and its description before (0xFFFF: none), its image ID (32-bit), its gamerscore, 2 bytes,
// its flags (32-bit: the type in bits 0-2, 1 completion, 2 leveling, 3 unlock, 4 event, ...; 8 shown before it's
// earned), and 16 bytes.
#include <cstdint>
#include <string>
#include <vector>

namespace spa
{
    // Takes the SPA data from the game's image (unpacked, as loaded at its base address), where the executable's header
    // (default.xex's first bytes) says its resources are. At startup, before the game runs: the rest only reads it.
    // Returns false (and logs why) if the executable has none.
    bool Load(const std::vector<uint8_t>& xex, const std::vector<uint8_t>& image);
    bool Loaded();

    // An image (a PNG) by its ID, or an empty vector.
    std::vector<uint8_t> Image(uint64_t id);
    // The title's image, the dashboard's tile (64x64).
    std::vector<uint8_t> TitleImage();
    // A string by its ID in a language's table (XLanguage), else in the default language's (XSTC), else empty. UTF-8.
    std::string String(uint16_t id, uint32_t language);
    // The title's name.
    std::string TitleName(uint32_t language);
    // The default language (XSTC), 1 (English) if it doesn't say.
    uint32_t DefaultLanguage();

    struct Achievement
    {
        uint16_t id = 0;
        uint16_t nameId = 0, descriptionId = 0, unachievedId = 0;
        uint32_t imageId = 0;
        uint16_t gamerscore = 0;
        uint32_t flags = 0;
    };
    const std::vector<Achievement>& Achievements();
    // nullptr if the title has no such achievement.
    const Achievement* FindAchievement(uint32_t id);
}
