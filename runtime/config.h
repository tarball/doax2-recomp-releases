#pragma once

#include <string>

// Process-wide settings decided at startup.
namespace config
{
    // The game data directory (default "private"): default.xex/.pe, hdd/ and content/ live here.
    void SetDataDirectory(std::string directory);
    const std::string& DataDirectory();
    // A diagnostics file's path in the data directory's dumps/, which is made if missing.
    std::string DumpPath(const std::string& name);
    // The disc image the game runs from.
    void SetDiscImage(std::string path);
    const std::string& DiscImage();
}
