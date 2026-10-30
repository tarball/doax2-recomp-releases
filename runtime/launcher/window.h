#pragma once

// The launcher's window: one small window that shows what the launcher is doing (checking, building the game's code)
// and lets the player choose the disc image and how to play (on this PC, or netplay: host or join, lockstep or
// rollback), then starts the game and closes.
#include "launcher/launcher.h"

namespace launcher
{
    // Runs the window until it closes. `disc` is a disc image to use (from the command line), or empty. The exit code.
    int RunWindow(const Folder& folder, const std::wstring& disc, const Choices& choices);
}
