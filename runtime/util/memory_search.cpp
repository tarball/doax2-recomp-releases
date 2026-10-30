#include "util/memory_search.h"

#include "guest_memory.h"
#include "log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace debug
{
    namespace
    {
        struct Step
        {
            uint64_t frame;
            float lo, hi; // "nan" for both: a computed NaN (the default quiet NaN, either sign: x86's or PowerPC's)
        };

        struct Search
        {
            std::vector<Step> steps;
            size_t next = 0;
            std::vector<uint32_t> candidates;
            std::vector<std::vector<float>> values; // per candidate, at each step so far
        };

        std::vector<Search> Parse(const char* setting)
        {
            std::vector<Search> searches(1);
            for (const char* p = setting; *p;)
            {
                char* end = nullptr;
                Step step{};
                step.frame = strtoull(p, &end, 10);
                if (*end != ':')
                    break;
                step.lo = strtof(end + 1, &end);
                if (*end != ':')
                    break;
                step.hi = strtof(end + 1, &end);
                searches.back().steps.push_back(step);
                if (*end == ';')
                    searches.emplace_back();
                p = *end ? end + 1 : end;
            }
            std::erase_if(searches, [](const Search& s) { return s.steps.empty(); });
            return searches;
        }

        float LoadFloat(uint32_t address)
        {
            const uint32_t bits = guest::LoadU32(address);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        }

        bool Matches(const Step& step, uint32_t address)
        {
            if (step.lo != step.lo)
                return (guest::LoadU32(address) & 0x7FFFFFFFu) == 0x7FC00000u;
            const float value = LoadFloat(address);
            return value >= step.lo && value <= step.hi;
        }

        // Every committed, readable guest word below 0xC0000000 (C0000000-DFFFFFFF aliases A0000000-BFFFFFFF,
        // and E0000000 and up isn't mapped).
        template <typename F>
        void ForEachGuestWord(F&& visit)
        {
            uint8_t* p = guest::g_base;
            uint8_t* const end = guest::g_base + 0xC0000000ull;
            while (p < end)
            {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(p, &info, sizeof(info)))
                    break;
                uint8_t* const regionEnd = std::min(static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize, end);
                const DWORD access = info.Protect & 0xFF;
                const bool readable = info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) &&
                    (access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE ||
                        access == PAGE_WRITECOPY || access == PAGE_EXECUTE_WRITECOPY);
                if (readable)
                {
                    for (uint8_t* word = p; word + 4 <= regionEnd; word += 4)
                        visit(uint32_t(word - guest::g_base));
                }
                p = regionEnd;
            }
        }
    }

    void FloatSearchOnFrame(uint64_t frame)
    {
        static std::vector<Search> searches = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_FLOAT_SEARCH");
            return setting ? Parse(setting) : std::vector<Search>{};
        }();
        for (size_t index = 0; index < searches.size(); index++)
        {
            Search& search = searches[index];
            if (search.next >= search.steps.size() || search.steps[search.next].frame != frame)
                continue;
            const Step& step = search.steps[search.next];
            if (search.next == 0)
            {
                ForEachGuestWord([&](uint32_t address) {
                    if (Matches(step, address))
                    {
                        search.candidates.push_back(address);
                        search.values.push_back({ LoadFloat(address) });
                    }
                });
            }
            else
            {
                size_t kept = 0;
                for (size_t i = 0; i < search.candidates.size(); i++)
                {
                    const float value = LoadFloat(search.candidates[i]);
                    if (!Matches(step, search.candidates[i]))
                        continue;
                    search.candidates[kept] = search.candidates[i];
                    search.values[kept] = std::move(search.values[i]);
                    search.values[kept].push_back(value);
                    kept++;
                }
                search.candidates.resize(kept);
                search.values.resize(kept);
            }
            search.next++;
            LOG_INFO("float search %zu: frame %llu: %zu candidates", index, static_cast<unsigned long long>(frame), search.candidates.size());
            if (search.next < search.steps.size())
                continue;
            for (size_t i = 0; i < search.candidates.size() && i < 5000; i++)
            {
                std::string line;
                for (const float value : search.values[i])
                    line += " " + std::to_string(value);
                LOG_INFO("float search %zu: %08X:%s", index, search.candidates[i], line.c_str());
            }
        }
    }

    void WatchChangesOnFrame(uint64_t frame)
    {
        struct Watch
        {
            uint32_t address, value;
        };
        static std::vector<Watch> watches = [] {
            std::vector<Watch> parsed;
            if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_WATCH_CHANGES"))
            {
                for (const char* p = setting; *p;)
                {
                    char* end = nullptr;
                    const uint32_t address = uint32_t(strtoul(p, &end, 16)) & ~3u;
                    parsed.push_back({ address, guest::LoadU32(address) });
                    p = *end ? end + 1 : end;
                }
            }
            return parsed;
        }();
        for (Watch& watch : watches)
        {
            const uint32_t value = guest::LoadU32(watch.address);
            if (value == watch.value)
                continue;
            LOG_INFO("watch: frame %llu: [%08X] %08X -> %08X", static_cast<unsigned long long>(frame), watch.address, watch.value, value);
            watch.value = value;
        }
    }
}
