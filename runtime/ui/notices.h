#pragma once

// Notices the runtime shows over the game for a few seconds, one at a time in the order they come: an achievement earned
// (xam/achievements.h), a netplay session's end. Each is a card at the bottom of the frame's title-safe area, centered,
// where the console's own notices appeared: a picture (if any), a heading with a detail right of it, and a line of
// text. The renderer draws it as it presents each frame (gpu/d3d12/overlay.h), over the game's picture after the
// display's gamma ramp, in the window and in frame dumps alike, at any resolution scale and in fullscreen. Its card is
// drawn with GDI at the size the frame is shown at (the largest of the window's and the dump's), so its text stays
// sharp. Without a renderer (HEADLESS) notices are only logged by their callers.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ui::notices
{
    struct Notice
    {
        std::string heading;          // UTF-8, small, on the first line
        std::string text;             // UTF-8, larger, on the second
        std::string detail;           // UTF-8, right of the heading (an achievement's gamerscore)
        std::vector<uint8_t> picture; // an image file (a PNG), or empty
        uint32_t frames = 300;        // how long it shows, in frames presented (300: 5 s)
    };
    // Queues a notice, shown after those before it (at most 16 wait). Any thread.
    void Show(Notice notice);

    // A notice's card as drawn: premultiplied BGRA rows, and where it goes in a 1280x720 frame (it's drawn scaled to the
    // frame's size).
    struct Card
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> bgra;
        float x = 0, y = 0, w = 0, h = 0;
    };
    struct Frame
    {
        std::shared_ptr<const Card> card; // nullptr: nothing to show
        float opacity = 0;                // it fades in and out
    };
    // Once a frame presented, on the renderer's thread: the card to draw over it, for a frame shown at most `pixelHeight`
    // pixels high, and its opacity.
    Frame Next(uint32_t pixelHeight);
}
