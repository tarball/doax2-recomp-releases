#include "xam/achievements.h"

#include "config.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/session.h"
#include "ui/notices.h"
#include "xam/spa.h"
#include "xam/xam.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace achievements
{
    namespace
    {
        // The console's language, as XGetLanguage reports it (kernel/misc.cpp): English.
        constexpr uint32_t kLanguage = 1;

        std::mutex g_lock;
        bool g_loaded = false;
        std::map<uint32_t, std::string> g_earned; // ID -> when

        // The file's achievements (under g_lock), read the first time.
        void Load()
        {
            if (g_loaded)
                return;
            g_loaded = true;
            g_earned.clear();
            std::ifstream file(FilePath());
            std::string line;
            while (std::getline(file, line))
            {
                if (line.empty() || line[0] == ';')
                    continue;
                char* end = nullptr;
                const unsigned long id = strtoul(line.c_str(), &end, 10);
                if (end == line.c_str() || id > 0xFFFF)
                    continue;
                while (*end == ' ' || *end == '\t')
                    end++;
                std::string when;
                while (*end && *end != ' ' && *end != '\t' && *end != '\r')
                    when += *end++;
                g_earned.emplace(uint32_t(id), when);
            }
            if (!g_earned.empty())
                LOG_INFO("Achievements: %zu earned with this profile (%s)", g_earned.size(), FilePath().c_str());
        }

        std::string Now()
        {
            const std::time_t now = std::time(nullptr);
            std::tm utc{};
            gmtime_s(&utc, &now);
            char text[32];
            strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
            return text;
        }
    }

    std::string FilePath()
    {
        char xuid[17], title[9];
        snprintf(xuid, sizeof(xuid), "%016llX", (unsigned long long)xam::kLocalXuid);
        snprintf(title, sizeof(title), "%08X", xam::kTitleId);
        return (std::filesystem::path(config::DataDirectory()) / "content" / xuid / title / "achievements.txt").string();
    }

    bool Earned(uint32_t id)
    {
        std::lock_guard lock(g_lock);
        Load();
        return g_earned.count(id) != 0;
    }

    void Reset()
    {
        std::lock_guard lock(g_lock);
        g_loaded = false;
        g_earned.clear();
    }

    bool Earn(uint32_t user, uint32_t id)
    {
        const spa::Achievement* achievement = spa::FindAchievement(id);
        const std::string name = achievement ? spa::String(achievement->nameId, kLanguage) : std::string();
        const uint32_t gamerscore = achievement ? achievement->gamerscore : 0;
        if (!xam::IsUserSignedIn(user))
        {
            LOG_INFO("Achievements: %u (%s) for user %u, who has no profile: not recorded", id, name.c_str(), user);
            return false;
        }
        if (netplay::session::Configured())
        {
            static std::mutex reportedLock;
            static std::map<uint32_t, bool> reported;
            std::lock_guard lock(reportedLock);
            if (!reported[id])
            {
                reported[id] = true;
                LOG_INFO("Achievements: %u (%s) not recorded in a netplay session, which has everything unlocked", id, name.c_str());
            }
            return false;
        }
        std::lock_guard lock(g_lock);
        Load();
        if (g_earned.count(id))
            return false;
        const std::string when = Now();
        g_earned.emplace(id, when);
        const std::string path = FilePath();
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        const bool exists = std::filesystem::exists(path, error);
        if (FILE* file = fopen(path.c_str(), "ab"))
        {
            if (!exists)
                fprintf(file, "; %s: the achievements earned with this profile: the achievement's ID, when (UTC), its gamerscore and its name.\r\n"
                              "; Remove a line to earn that achievement again.\r\n",
                    spa::TitleName(kLanguage).c_str());
            fprintf(file, "%u %s %u %s\r\n", id, when.c_str(), gamerscore, name.c_str());
            fclose(file);
        }
        else
        {
            LOG_WARN("Achievements: can't write %s", path.c_str());
        }
        LOG_INFO("Achievements: earned %u, %s (%uG), at %s, game frame %llu", id, achievement ? name.c_str() : "not in the title's data", gamerscore,
            when.c_str(), (unsigned long long)netplay::Frame());
        if (achievement)
        {
            ui::notices::Notice notice;
            notice.heading = "Achievement unlocked";
            notice.text = name;
            notice.detail = std::to_string(gamerscore) + "G";
            notice.picture = spa::Image(achievement->imageId);
            ui::notices::Show(std::move(notice));
        }
        return true;
    }
}
