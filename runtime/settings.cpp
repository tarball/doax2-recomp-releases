#include "settings.h"

#include "config.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace settings
{
    namespace
    {
        std::mutex g_lock;
        Graphics g_graphics;
        std::string g_discImage;
        uint32_t g_version = 0;
        bool g_changed = false; // since the file was read or written (the environment's overrides aren't changes)

        std::string Path()
        {
            return config::DataDirectory() + "/settings.ini";
        }

        // Keeps values in the ranges the renderer supports.
        Graphics Valid(Graphics graphics)
        {
            graphics.resolutionScale = std::clamp(graphics.resolutionScale, 1u, 4u);
            graphics.msaaSamples = graphics.msaaSamples >= 8 ? 8 : graphics.msaaSamples >= 4 ? 4 : 0;
            graphics.anisotropy = graphics.anisotropy >= 16 ? 16 : graphics.anisotropy >= 8 ? 8 : graphics.anisotropy >= 4 ? 4 :
                graphics.anisotropy >= 2 ? 2 : 0;
            return graphics;
        }

        void Set(Graphics& graphics, const char* name, const char* value)
        {
            const uint32_t number = uint32_t(std::max(std::atoi(value), 0));
            if (!strcmp(name, "fullscreen"))
                graphics.fullscreen = number != 0;
            else if (!strcmp(name, "resolution_scale"))
                graphics.resolutionScale = number;
            else if (!strcmp(name, "msaa"))
                graphics.msaaSamples = number;
            else if (!strcmp(name, "alpha_to_coverage"))
                graphics.alphaToCoverage = number != 0;
            else if (!strcmp(name, "anisotropy"))
                graphics.anisotropy = number;
            else
                LOG_WARN("settings: unknown setting %s", name);
        }
    }

    void Load()
    {
        Graphics graphics;
        std::string discImage;
        if (FILE* file = fopen(Path().c_str(), "rb"))
        {
            // name = value (the rest of the line, trimmed: paths have spaces); ; comments and [sections] are skipped.
            char line[1024];
            while (fgets(line, sizeof(line), file))
            {
                const std::string text = line;
                const size_t equals = text.find('=');
                if (text.empty() || text[0] == ';' || text[0] == '[' || equals == std::string::npos)
                    continue;
                const auto trim = [](std::string s) {
                    const size_t first = s.find_first_not_of(" \t\r\n"), last = s.find_last_not_of(" \t\r\n");
                    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
                };
                const std::string name = trim(text.substr(0, equals)), value = trim(text.substr(equals + 1));
                if (name == "disc_image")
                    discImage = value;
                else
                    Set(graphics, name.c_str(), value.c_str());
            }
            fclose(file);
        }
        std::lock_guard lock(g_lock);
        g_discImage = discImage;
        const std::pair<const char*, const char*> overrides[] = {
            { GAME_PREFIX "_FULLSCREEN", "fullscreen" },
            { GAME_PREFIX "_RESOLUTION_SCALE", "resolution_scale" },
            { GAME_PREFIX "_MSAA", "msaa" },
            { GAME_PREFIX "_ALPHA_TO_COVERAGE", "alpha_to_coverage" },
            { GAME_PREFIX "_ANISOTROPY", "anisotropy" },
        };
        for (const auto& [variable, name] : overrides)
            if (const char* value = std::getenv(variable))
                Set(graphics, name, value);
        g_graphics = Valid(graphics);
        LOG_INFO("settings: %s, resolution scale %u, MSAA %u, alpha to coverage %s, anisotropy %u",
            g_graphics.fullscreen ? "fullscreen" : "windowed", g_graphics.resolutionScale, g_graphics.msaaSamples,
            g_graphics.alphaToCoverage ? "on" : "off", g_graphics.anisotropy);
    }

    void Save()
    {
        std::lock_guard lock(g_lock);
        if (!g_changed)
            return;
        FILE* file = fopen(Path().c_str(), "wb");
        if (!file)
        {
            LOG_WARN("settings: can't write %s", Path().c_str());
            return;
        }
        fputs("; " GAME_TITLE " recomp settings\n", file);
        if (!g_discImage.empty())
            fprintf(file, "[game]\n; The disc image the game runs from.\ndisc_image = %s\n", g_discImage.c_str());
        fprintf(file,
            "[graphics]\n"
            "; A borderless window covering the screen instead of a window (0 or 1; Alt+Enter switches).\n"
            "fullscreen = %u\n"
            "; Renders at this many times 1280x720 in each direction (1-4).\n"
            "resolution_scale = %u\n"
            "; MSAA samples for the surfaces the game multisamples (0: its own 2x, 4 or 8).\n"
            "msaa = %u\n"
            "; Smooths alpha-tested edges (foliage) with MSAA (0 or 1).\n"
            "alpha_to_coverage = %u\n"
            "; Anisotropic texture filtering (0: off, as the game has it, 2, 4, 8 or 16).\n"
            "anisotropy = %u\n",
            g_graphics.fullscreen ? 1u : 0u, g_graphics.resolutionScale, g_graphics.msaaSamples, g_graphics.alphaToCoverage ? 1u : 0u,
            g_graphics.anisotropy);
        fclose(file);
        g_changed = false;
        LOG_INFO("settings: saved %s", Path().c_str());
    }

    std::string DiscImage()
    {
        std::lock_guard lock(g_lock);
        return g_discImage;
    }

    Graphics GetGraphics()
    {
        std::lock_guard lock(g_lock);
        return g_graphics;
    }

    uint32_t GraphicsVersion()
    {
        std::lock_guard lock(g_lock);
        return g_version;
    }

    void SetGraphics(const Graphics& graphics)
    {
        std::lock_guard lock(g_lock);
        const Graphics valid = Valid(graphics);
        if (valid == g_graphics)
            return;
        g_graphics = valid;
        g_version++;
        g_changed = true;
    }
}
