#pragma once

#include <cstdint>
#include <string>

// Options the host adds to the game's own (which live in its save), chosen in SETTINGS > VIDEO
// (runtime/game/video_menu.cpp) and kept in <data>/settings.ini. Environment variables override the file:
// FULLSCREEN, RESOLUTION_SCALE, MSAA, ALPHA_TO_COVERAGE and ANISOTROPY.
namespace settings
{
    struct Graphics
    {
        bool fullscreen = false;      // a borderless window covering the monitor, instead of a window (Alt+Enter switches)
        uint32_t resolutionScale = 1; // renders at this many times 1280x720 in each direction (1-4)
        uint32_t msaaSamples = 0;     // samples for the surfaces DOA4 multisamples (0: its own 2x; 4 or 8)
        bool alphaToCoverage = false; // alpha test edges as partial coverage on multisampled surfaces
        uint32_t anisotropy = 0;      // anisotropic filtering of linearly minified textures (0: off, as DOA4 has it; 2-16)

        bool operator==(const Graphics&) const = default;
    };

    // Reads the file, then the environment. Call once, before the renderer starts.
    void Load();
    // The disc image the game runs from ([game] disc_image, which setup.ps1 writes), or empty.
    std::string DiscImage();
    // Writes the file if SetGraphics changed the settings since it was read or written.
    void Save();

    Graphics GetGraphics();
    // Counts changes: the renderer applies a change from its next frame.
    uint32_t GraphicsVersion();
    void SetGraphics(const Graphics& graphics);
}
