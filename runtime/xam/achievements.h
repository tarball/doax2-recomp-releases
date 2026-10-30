#pragma once

// Achievements: the game writes them as players earn them (XUserWriteAchievements, through XMsg to the achievements app,
// xam/messages.cpp). The runtime keeps them in a plain text file next to the profile's saves, which nothing locks down:
//   <data>/content/<XUID>/<title ID>/achievements.txt
// a line an achievement earned: its ID, when (UTC, ISO 8601), its gamerscore and its name, the last two for whoever
// reads it. Lines starting with ';' are comments; removing an achievement's line lets it be earned again. Each one newly
// earned is logged and shown in a notice over the game (ui/notices.h) with its name, gamerscore and picture, from the
// title's SPA data (xam/spa.h). In a netplay session nothing is recorded: everything is unlocked in memory for its
// Versus (game/unlocks.cpp), which would earn the unlocking achievements without the player's own progress.
#include <cstdint>
#include <string>

namespace achievements
{
    // The game says game user `user` earned achievement `id`: recorded and shown if new (and the user has the
    // profile). Returns whether it was new. Any thread.
    bool Earn(uint32_t user, uint32_t id);
    // The file's path.
    std::string FilePath();
    // Whether achievement `id` has been earned (as the file says), loading it the first time. Any thread.
    bool Earned(uint32_t id);
    // Forgets what was read from the file, so the next call reads it again (for tests).
    void Reset();
}
