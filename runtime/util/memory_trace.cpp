#include "util/memory_trace.h"

#include "config.h"
#include "guest_memory.h"
#include "log.h"
#include "netplay/frame.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace debug
{
    namespace
    {
        struct Settings
        {
            uint64_t first = 0, count = 0;
            bool fight = false; // `first` counts the first fight's frames
            std::vector<std::pair<uint32_t, uint32_t>> ranges; // address, length
            Settings()
            {
                const char* setting = std::getenv(GAME_PREFIX "_DEBUG_MEMORY_TRACE");
                if (!setting)
                    return;
                fight = *setting == 'F';
                char* end = nullptr;
                first = strtoull(setting + (fight ? 1 : 0), &end, 10);
                if (*end == ',')
                    count = strtoull(end + 1, &end, 10);
                while (*end == ':' || *end == ',')
                {
                    const uint32_t address = uint32_t(strtoul(end + 1, &end, 16));
                    if (*end != '+')
                        break;
                    const uint32_t length = uint32_t(strtoul(end + 1, &end, 16));
                    if (length)
                        ranges.emplace_back(address, length);
                }
            }
        };

        FILE* g_file = nullptr;
        bool g_done = false;
    }

    void MemoryTraceOnFrame(uint64_t frame)
    {
        static const Settings settings;
        if (!settings.count || settings.ranges.empty() || g_done)
            return;
        const std::optional<uint64_t> fightFrame = netplay::FightFrame();
        if (settings.fight && !fightFrame)
            return;
        const uint64_t position = settings.fight ? *fightFrame : frame;
        if (position < settings.first)
            return;
        if (!g_file)
        {
            const std::string path = config::DumpPath("memory_trace.bin");
            g_file = fopen(path.c_str(), "wb");
            if (!g_file)
            {
                g_done = true;
                return;
            }
            fwrite("MTRC", 1, 4, g_file);
            const uint32_t ranges = uint32_t(settings.ranges.size());
            fwrite(&ranges, 4, 1, g_file);
            for (const auto& [address, length] : settings.ranges)
            {
                fwrite(&address, 4, 1, g_file);
                fwrite(&length, 4, 1, g_file);
            }
            LOG_INFO("Memory trace: %u ranges for %llu frames from %sframe %llu (game frame %llu), in %s", ranges,
                (unsigned long long)settings.count, settings.fight ? "the fight's " : "", (unsigned long long)position,
                (unsigned long long)frame, path.c_str());
        }
        if (position >= settings.first + settings.count)
        {
            fclose(g_file);
            g_file = nullptr;
            g_done = true;
            LOG_INFO("Memory trace: done");
            return;
        }
        const int64_t fight = fightFrame ? int64_t(*fightFrame) : -1;
        fwrite(&frame, 8, 1, g_file);
        fwrite(&fight, 8, 1, g_file);
        for (const auto& [address, length] : settings.ranges)
            fwrite(guest::ToHost(address), 1, length, g_file);
    }
}
