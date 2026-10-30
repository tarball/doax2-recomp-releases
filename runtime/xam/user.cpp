// User/profile exports: one local (offline) profile signed in on controller slot 0.
#include "guest_abi.h"
#include "log.h"
#include "xam/xam.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace xam
{
    bool IsUserSignedIn(uint32_t userIndex)
    {
        return userIndex == 0;
    }
}

namespace
{
    constexpr uint32_t kSignedInLocally = 1;

    uint32_t XamUserGetSigninState(uint32_t userIndex)
    {
        return xam::IsUserSignedIn(userIndex) ? kSignedInLocally : 0;
    }

    uint32_t XamUserGetXUID(uint32_t userIndex, uint32_t /*typeMask*/, be<uint64_t>* xuid)
    {
        if (!xam::IsUserSignedIn(userIndex))
            return xam::kErrorNoSuchUser;
        if (xuid)
            *xuid = xam::kLocalXuid;
        return xam::kErrorSuccess;
    }

    uint32_t XamUserGetName(uint32_t userIndex, char* buffer, uint32_t bufferSize)
    {
        if (!xam::IsUserSignedIn(userIndex))
            return xam::kErrorNoSuchUser;
        if (buffer && bufferSize)
        {
            strncpy(buffer, xam::kLocalGamertag, bufferSize - 1);
            buffer[bufferSize - 1] = 0;
        }
        return xam::kErrorSuccess;
    }

    // Privileges (multiplayer, content, communications...) are granted, as Xenia does.
    uint32_t XamUserCheckPrivilege(uint32_t userIndex, uint32_t /*privilege*/, be<uint32_t>* result)
    {
        if (!xam::IsUserSignedIn(userIndex))
            return xam::kErrorNoSuchUser;
        if (result)
            *result = 1;
        return xam::kErrorSuccess;
    }

    uint32_t XamUserAreUsersFriends(uint32_t /*userIndex*/, uint32_t /*xuids*/, uint32_t count, be<uint32_t>* results, xam::Overlapped* overlapped)
    {
        for (uint32_t i = 0; results && i < count; i++)
            results[i] = 0;
        return xam::Complete(overlapped, xam::kErrorSuccess);
    }

    // ---------------------------------------------------------------------------------------------
    // Profile settings. A setting ID holds its data type (bits 28-31) and maximum size (bits 16-27). The profile has the
    // dashboard's defaults for the common settings; the others (the title's own among them) have no value, as for a
    // profile that never played the title.

    constexpr uint32_t kSourceNoValue = 0; // XSOURCE_NO_VALUE
    constexpr uint32_t kSourceDefault = 1; // XSOURCE_DEFAULT
    constexpr uint32_t kTypeUnicode = 4;   // XUSER_DATA_TYPE_UNICODE

    struct DefaultSetting
    {
        uint32_t id;
        int32_t value; // an int32 setting's value; a string setting is empty
    };
    constexpr DefaultSetting kDefaultSettings[] = {
        { 0x10040002, 0 }, // XPROFILE_GAMER_YAXIS_INVERSION: off
        { 0x10040003, 3 }, // XPROFILE_OPTION_CONTROLLER_VIBRATION: on
        { 0x10040004, 0 }, // XPROFILE_GAMERCARD_ZONE
        { 0x10040005, 0 }, // XPROFILE_GAMERCARD_REGION
        { 0x10040006, 0 }, // XPROFILE_GAMERCARD_CRED
        { 0x1004000C, 0 }, // XPROFILE_OPTION_VOICE_MUTED
        { 0x1004000D, 0 }, // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
        { 0x402C0011, 0 }, // XPROFILE_GAMERCARD_MOTTO: empty
        { 0x10040012, 1 }, // XPROFILE_GAMERCARD_TITLES_PLAYED
        { 0x10040013, 0 }, // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
        { 0x10040015, 0 }, // XPROFILE_GAMER_DIFFICULTY: normal
        { 0x10040018, 0 }, // XPROFILE_GAMER_CONTROL_SENSITIVITY: medium
        { 0x10040022, 1 }, // XPROFILE_GAMER_ACTION_AUTO_AIM: on
        { 0x10040023, 0 }, // XPROFILE_GAMER_ACTION_AUTO_CENTER: off
        { 0x10040024, 0 }, // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL: left stick
        { 0x10040026, 0 }, // XPROFILE_GAMER_RACE_TRANSMISSION: automatic
        { 0x10040027, 0 }, // XPROFILE_GAMER_RACE_CAMERA_LOCATION: behind
        { 0x10040028, 0 }, // XPROFILE_GAMER_RACE_BRAKE_CONTROL: trigger
        { 0x10040029, 0 }, // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL: trigger
        { 0x10040038, 0 }, // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
        { 0x10040039, 0 }, // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
    };

    const DefaultSetting* FindDefaultSetting(uint32_t id)
    {
        for (const DefaultSetting& setting : kDefaultSettings)
        {
            if (setting.id == id)
                return &setting;
        }
        return nullptr;
    }

    // XUSER_PROFILE_SETTING: where the value came from, whose it is (a user index, or a XUID filling both words), the
    // setting, and an XUSER_DATA: its type, then (8-byte aligned) the value: an int32 in the first word; a string's
    // byte count and pointer.
    struct ProfileSetting
    {
        be<uint32_t> source;
        be<uint32_t> padding;
        be<uint64_t> user;
        be<uint32_t> settingId;
        be<uint32_t> padding2;
        uint8_t type;
        uint8_t padding3[7];
        be<uint32_t> value[2];
    };
    static_assert(sizeof(ProfileSetting) == 40);

    // XUserReadProfileSettings(title, user, settings, &size, results, overlapped) reads its own user's settings;
    // XUserReadProfileSettingsByXuid gives XUIDs instead. The results are an XUSER_READ_PROFILE_SETTING_RESULT (a count and
    // a pointer) followed by the settings (users x settings), then the strings. With too small a buffer (or none), the
    // call gives the size it needs and ERROR_INSUFFICIENT_BUFFER.
    uint32_t XamUserReadProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t xuidCount, const be<uint64_t>* xuids, uint32_t settingCount,
        const be<uint32_t>* settingIds, be<uint32_t>* bufferSize, uint8_t* buffer, xam::Overlapped* overlapped)
    {
        if (!bufferSize || !settingIds || settingCount == 0 || (xuidCount && !xuids))
            return xam::Complete(overlapped, xam::kErrorInvalidParameter);
        if (!xuidCount && !xam::IsUserSignedIn(userIndex))
            return xam::Complete(overlapped, xam::kErrorNoSuchUser);

        const uint32_t users = xuidCount ? xuidCount : 1;
        const uint32_t results = users * settingCount;
        uint32_t needed = 8 + results * uint32_t(sizeof(ProfileSetting));
        for (uint32_t i = 0; i < settingCount; i++)
        {
            if (FindDefaultSetting(settingIds[i]) && (settingIds[i] >> 28) == kTypeUnicode)
                needed += 4 * users; // an empty string, aligned
        }
        if (!buffer || *bufferSize < needed)
        {
            *bufferSize = needed;
            return xam::kErrorInsufficientBuffer;
        }

        std::string ids;
        uint8_t* strings = buffer + 8 + results * sizeof(ProfileSetting);
        auto* settings = reinterpret_cast<ProfileSetting*>(buffer + 8);
        guest::StoreU32(guest::ToGuest(buffer), results);
        guest::StoreU32(guest::ToGuest(buffer) + 4, guest::ToGuest(settings));
        for (uint32_t user = 0; user < users; user++)
        {
            const bool ours = !xuidCount || xuids[user] == xam::kLocalXuid;
            for (uint32_t i = 0; i < settingCount; i++)
            {
                const uint32_t id = settingIds[i];
                ProfileSetting& setting = settings[user * settingCount + i];
                guest::BeforeHostWriteTo(&setting, sizeof(setting));
                memset(&setting, 0, sizeof(setting));
                setting.user = xuidCount ? uint64_t(xuids[user]) : uint64_t(userIndex) << 32;
                setting.settingId = id;
                setting.type = uint8_t(id >> 28);
                setting.source = kSourceNoValue;
                if (const DefaultSetting* value = FindDefaultSetting(id); value && ours)
                {
                    setting.source = kSourceDefault;
                    if (setting.type == kTypeUnicode)
                    {
                        guest::BeforeHostWriteTo(strings, 4);
                        memset(strings, 0, 4);
                        setting.value[0] = 2; // the terminator's bytes
                        setting.value[1] = guest::ToGuest(strings);
                        strings += 4;
                    }
                    else
                    {
                        setting.value[0] = uint32_t(value->value);
                    }
                }
                if (user == 0)
                {
                    char text[16];
                    snprintf(text, sizeof(text), "%s%08X%s", i ? " " : "", id, setting.source == kSourceNoValue ? "(none)" : "");
                    ids += text;
                }
            }
        }
        LOG_INFO("XamUserReadProfileSettings(title 0x%08X, %s %u): %s", titleId, xuidCount ? "XUIDs" : "user", xuidCount ? xuidCount : userIndex,
            ids.c_str());
        return xam::Complete(overlapped, xam::kErrorSuccess);
    }

    // ---------------------------------------------------------------------------------------------
    // Achievements and leaderboards: the runtime records the achievements a game earns (achievements.h) but doesn't
    // enumerate them, and leaderboards are on Xbox Live, so both enumerations are empty (XamEnumerate gives
    // ERROR_NO_MORE_FILES at once). DOAX2 only asks for the achievements of Xbox Live profiles, which the local profile
    // isn't (its XUID isn't an online one); DOA4 doesn't ask offline.

    // XUserCreateAchievementEnumerator(title, user, XUID, detail flags, first, count, &size, &handle). The size is that of
    // `count` XACHIEVEMENT_DETAILS (0x24 bytes) with the strings the flags ask for (label 33, description and unachieved
    // description 101 characters each).
    uint32_t XamUserCreateAchievementEnumerator(uint32_t titleId, uint32_t userIndex, uint64_t xuid, uint32_t detailFlags, uint32_t first,
        uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        if (!bufferSize || !handle || count == 0)
            return xam::kErrorInvalidParameter;
        const uint32_t itemSize = 0x24 + (detailFlags & 2 ? 33 * 2 : 0) + (detailFlags & 4 ? 101 * 2 : 0) + (detailFlags & 8 ? 101 * 2 : 0);
        *bufferSize = itemSize * count;
        *handle = xam::CreateEnumerator(itemSize, count);
        LOG_INFO("XamUserCreateAchievementEnumerator(title 0x%08X, user %u, XUID %016llX, from %u, %u): none", titleId, userIndex,
            (unsigned long long)xuid, first, count);
        return xam::kErrorSuccess;
    }

    // XUserCreateStatsEnumeratorByRank/ByXuid/ByRating(title, rank or XUID, rows, specs, &size, &handle). The size is that of
    // an XUSER_STATS_READ_RESULTS (8 bytes) with, for each XUSER_STATS_SPEC (a view, its column count and 64 column IDs),
    // a view (16 bytes) of `rows` rows (48 bytes each) of its columns (24 bytes each).
    uint32_t XamUserCreateStatsEnumerator(uint32_t titleId, uint32_t kind, uint64_t pivot, uint32_t rows, uint32_t specCount, const uint8_t* specs,
        be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        if (!bufferSize || !handle || (specCount && !specs))
            return xam::kErrorInvalidParameter;
        uint32_t size = 8;
        for (uint32_t i = 0; i < specCount; i++)
        {
            const uint32_t columns = guest::LoadU32(guest::ToGuest(specs + i * 136 + 4));
            size += 16 + rows * (48 + std::min(columns, 64u) * 24);
        }
        *bufferSize = size;
        *handle = xam::CreateEnumerator(size, 1);
        LOG_INFO("XamUserCreateStatsEnumerator(title 0x%08X, kind %u, from %llu, %u rows, %u views): leaderboards are on Xbox Live, none", titleId,
            kind, (unsigned long long)pivot, rows, specCount);
        return xam::kErrorSuccess;
    }
}

namespace xam
{
    uint32_t Complete(Overlapped* overlapped, uint32_t result, uint32_t length, uint32_t extendedError)
    {
        if (!overlapped)
            return result;
        overlapped->internalLow = result;
        overlapped->internalHigh = length;
        overlapped->extendedError = extendedError ? extendedError : (result == kErrorSuccess ? 0 : 0x80070000 | result);
        if (overlapped->completionRoutine != 0)
            LOG_WARN("XOVERLAPPED completion routine 0x%08X not called", uint32_t(overlapped->completionRoutine));
        if (overlapped->event != 0)
        {
            // Signal the event through the handle-based export so it wakes waiters.
            PPCContext ctx{};
            ctx.r3.u64 = overlapped->event;
            ctx.r4.u64 = 0;
            __imp__NtSetEvent(ctx, guest::g_base);
        }
        return kErrorIoPending;
    }
}

GUEST_EXPORT(XamUserGetSigninState, XamUserGetSigninState)
GUEST_EXPORT(XamUserGetXUID, XamUserGetXUID)
GUEST_EXPORT(XamUserGetName, XamUserGetName)
GUEST_EXPORT(XamUserCheckPrivilege, XamUserCheckPrivilege)
GUEST_EXPORT(XamUserAreUsersFriends, XamUserAreUsersFriends)
GUEST_EXPORT(XamUserReadProfileSettings, XamUserReadProfileSettings)
GUEST_EXPORT(XamUserCreateAchievementEnumerator, XamUserCreateAchievementEnumerator)
GUEST_EXPORT(XamUserCreateStatsEnumerator, XamUserCreateStatsEnumerator)
