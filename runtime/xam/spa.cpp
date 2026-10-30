#include "xam/spa.h"

#include "log.h"

#include <cstdio>
#include <cstring>

namespace spa
{
    namespace
    {
        constexpr uint32_t kXdbfMagic = 0x58444246; // "XDBF"
        constexpr uint32_t kXstrMagic = 0x58535452; // "XSTR"
        constexpr uint64_t kAchievementsId = 0x58414348; // "XACH"
        constexpr uint64_t kDefaultLanguageId = 0x58535443; // "XSTC"
        constexpr uint64_t kTitleId = 0x8000;
        constexpr uint16_t kMetadata = 1, kImages = 2, kStrings = 3;

        uint32_t U32(const uint8_t* p)
        {
            return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }

        uint16_t U16(const uint8_t* p)
        {
            return uint16_t(p[0] << 8 | p[1]);
        }

        uint64_t U64(const uint8_t* p)
        {
            return uint64_t(U32(p)) << 32 | U32(p + 4);
        }

        struct Entry
        {
            uint16_t section;
            uint64_t id;
            uint32_t offset, size; // within the data that follows the tables
        };

        struct Spa
        {
            bool loaded = false;
            std::vector<uint8_t> bytes;
            size_t dataStart = 0;
            std::vector<Entry> entries;
            std::vector<Achievement> achievements;
            uint32_t defaultLanguage = 1;
        };
        Spa g_spa;

        // An entry's bytes, or nullptr (with `size` 0).
        const uint8_t* Find(uint16_t section, uint64_t id, size_t& size)
        {
            for (const Entry& entry : g_spa.entries)
            {
                if (entry.section == section && entry.id == id)
                {
                    size = entry.size;
                    return g_spa.bytes.data() + g_spa.dataStart + entry.offset;
                }
            }
            size = 0;
            return nullptr;
        }

        // Parses an XDBF file: false if it isn't one, or its tables don't fit in it.
        bool Parse(std::vector<uint8_t> bytes)
        {
            Spa spa;
            spa.bytes = std::move(bytes);
            const uint8_t* p = spa.bytes.data();
            const size_t size = spa.bytes.size();
            if (size < 24 || U32(p) != kXdbfMagic)
                return false;
            const uint32_t tableLength = U32(p + 8), used = U32(p + 12), freeLength = U32(p + 16);
            if (tableLength > 0x10000 || freeLength > 0x10000 || used > tableLength)
                return false;
            spa.dataStart = 24 + size_t(tableLength) * 18 + size_t(freeLength) * 8;
            if (spa.dataStart > size)
                return false;
            for (uint32_t i = 0; i < used; i++)
            {
                const uint8_t* e = p + 24 + size_t(i) * 18;
                const Entry entry{ U16(e), U64(e + 2), U32(e + 10), U32(e + 14) };
                if (uint64_t(spa.dataStart) + entry.offset + entry.size > size)
                    continue; // (one that doesn't fit is left out)
                spa.entries.push_back(entry);
            }
            g_spa = std::move(spa);
            g_spa.loaded = true;

            size_t length = 0;
            if (const uint8_t* xstc = Find(kMetadata, kDefaultLanguageId, length); xstc && length >= 16)
                g_spa.defaultLanguage = U32(xstc + 12);
            // XACH: magic, version, size, a 16-bit count, then 0x24 bytes an achievement.
            if (const uint8_t* xach = Find(kMetadata, kAchievementsId, length); xach && length >= 14)
            {
                const uint32_t count = U16(xach + 12);
                for (uint32_t i = 0; i < count && 14 + (i + 1) * 0x24 <= length; i++)
                {
                    const uint8_t* a = xach + 14 + i * 0x24;
                    Achievement achievement;
                    achievement.id = U16(a);
                    achievement.nameId = U16(a + 2);
                    achievement.descriptionId = U16(a + 4);
                    achievement.unachievedId = U16(a + 6);
                    achievement.imageId = U32(a + 8);
                    achievement.gamerscore = U16(a + 12);
                    achievement.flags = U32(a + 16);
                    g_spa.achievements.push_back(achievement);
                }
            }
            return true;
        }
    }

    bool Load(const std::vector<uint8_t>& xex, const std::vector<uint8_t>& image)
    {
        // The XEX header's optional headers: the resource info (0x2FF), the execution info (0x40006: the title ID at
        // +12) and the image's base address (0x10201).
        if (xex.size() < 0x18 || U32(xex.data()) != 0x58455832) // "XEX2"
            return false;
        const uint32_t headerSize = U32(xex.data() + 8), count = U32(xex.data() + 0x14);
        if (headerSize > xex.size() || count > (headerSize - 0x18) / 8)
            return false;
        uint32_t resources = 0, execution = 0, imageBase = 0x82000000;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* h = xex.data() + 0x18 + i * 8;
            const uint32_t key = U32(h), value = U32(h + 4);
            if (key == 0x000002FF)
                resources = value;
            else if (key == 0x00040006)
                execution = value;
            else if (key == 0x00010201)
                imageBase = value;
        }
        char titleId[9] = "";
        if (execution && uint64_t(execution) + 16 <= headerSize)
            snprintf(titleId, sizeof(titleId), "%08X", U32(xex.data() + execution + 12));
        if (!resources || uint64_t(resources) + 4 > headerSize)
        {
            LOG_WARN("SPA: the executable has no resources");
            return false;
        }
        const uint32_t infoSize = U32(xex.data() + resources);
        if (infoSize < 4 || uint64_t(resources) + infoSize > headerSize)
            return false;
        // 16 bytes a resource: its name (8 characters), address and size. The title's is named by its ID.
        std::vector<uint8_t> chosen;
        for (uint32_t i = 0; i < (infoSize - 4) / 16; i++)
        {
            const uint8_t* r = xex.data() + resources + 4 + i * 16;
            const std::string name(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 8));
            const uint32_t address = U32(r + 8), size = U32(r + 12);
            if (address < imageBase || uint64_t(address - imageBase) + size > image.size() || size < 24)
                continue;
            const uint8_t* data = image.data() + (address - imageBase);
            if (U32(data) != kXdbfMagic)
                continue;
            if (chosen.empty() || name == titleId)
                chosen.assign(data, data + size);
            if (name == titleId)
                break;
        }
        if (chosen.empty() || !Parse(std::move(chosen)))
        {
            LOG_WARN("SPA: no title data among the executable's resources");
            return false;
        }
        uint32_t gamerscore = 0;
        for (const Achievement& achievement : g_spa.achievements)
            gamerscore += achievement.gamerscore;
        LOG_INFO("SPA: %s, %zu entries, %zu achievements (%u gamerscore), default language %u", TitleName(g_spa.defaultLanguage).c_str(),
            g_spa.entries.size(), g_spa.achievements.size(), gamerscore, g_spa.defaultLanguage);
        return true;
    }

    bool Loaded()
    {
        return g_spa.loaded;
    }

    std::vector<uint8_t> Image(uint64_t id)
    {
        size_t size = 0;
        const uint8_t* data = Find(kImages, id, size);
        return data ? std::vector<uint8_t>(data, data + size) : std::vector<uint8_t>();
    }

    std::vector<uint8_t> TitleImage()
    {
        return Image(kTitleId);
    }

    std::string String(uint16_t id, uint32_t language)
    {
        for (const uint32_t table : { language, g_spa.defaultLanguage })
        {
            size_t size = 0;
            const uint8_t* xstr = Find(kStrings, table, size);
            if (!xstr || size < 14 || U32(xstr) != kXstrMagic)
                continue;
            const uint32_t count = U16(xstr + 12);
            size_t offset = 14;
            for (uint32_t i = 0; i < count && offset + 4 <= size; i++)
            {
                const uint16_t stringId = U16(xstr + offset), length = U16(xstr + offset + 2);
                offset += 4;
                if (offset + length > size)
                    break;
                if (stringId == id)
                    return std::string(reinterpret_cast<const char*>(xstr + offset), length);
                offset += length;
            }
        }
        return {};
    }

    std::string TitleName(uint32_t language)
    {
        return String(uint16_t(kTitleId), language);
    }

    uint32_t DefaultLanguage()
    {
        return g_spa.defaultLanguage;
    }

    const std::vector<Achievement>& Achievements()
    {
        return g_spa.achievements;
    }

    const Achievement* FindAchievement(uint32_t id)
    {
        for (const Achievement& achievement : g_spa.achievements)
        {
            if (achievement.id == id)
                return &achievement;
        }
        return nullptr;
    }
}
