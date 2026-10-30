// ExGetXConfigSetting: console configuration (region, language, video, country), with Xenia's values.
#include "guest_abi.h"
#include "kernel/status.h"
#include "log.h"

#include <cstring>

namespace
{
    constexpr uint32_t kInvalidParameter1 = 0xC00000EF;
    constexpr uint32_t kInvalidParameter2 = 0xC00000F0;
    constexpr uint32_t kInvalidParameter3 = 0xC00000F1;
    constexpr uint32_t kBufferTooSmall = 0xC0000023;

    constexpr uint16_t kCategorySecured = 0x0002;
    constexpr uint16_t kCategoryUser = 0x0003;

    uint32_t ExGetXConfigSetting(uint16_t category, uint16_t setting, uint8_t* buffer, uint16_t bufferSize, be<uint16_t>* requiredSize)
    {
        uint8_t value[4] = {};
        uint16_t size = 0;
        const auto set32 = [&](uint32_t v) {
            *reinterpret_cast<be<uint32_t>*>(value) = v;
            size = 4;
        };
        const auto set8 = [&](uint8_t v) {
            value[0] = v;
            size = 1;
        };

        switch (category)
        {
        case kCategorySecured:
            switch (setting)
            {
            case 0x0002: set32(0x00001000); break; // AV region: NTSC-U
            default:
                LOG_WARN("ExGetXConfigSetting: unknown secured setting 0x%04X", setting);
                return kInvalidParameter2;
            }
            break;
        case kCategoryUser:
            switch (setting)
            {
            case 0x0001: // time zone bias
            case 0x0002: // standard name
            case 0x0003: // daylight name
            case 0x0004: // standard date
            case 0x0005: // daylight date
            case 0x0006: // standard bias
            case 0x0007: // daylight bias
                set32(0);
                break;
            case 0x0009: set32(1); break;          // language: English
            case 0x000A: set32(0x00040000); break; // video flags
            case 0x000C: set32(0x40); break;       // retail flags
            case 0x000E: set8(103); break;         // country: United States
            case 0x000F: set8(0); break;           // parental control flags: off
            default:
                LOG_WARN("ExGetXConfigSetting: unknown user setting 0x%04X", setting);
                return kInvalidParameter2;
            }
            break;
        default:
            LOG_WARN("ExGetXConfigSetting: unknown category 0x%04X (setting 0x%04X)", category, setting);
            return kInvalidParameter1;
        }

        if (buffer)
        {
            if (bufferSize < size)
                return kBufferTooSmall;
            memcpy(buffer, value, size);
        }
        else if (bufferSize)
        {
            return kInvalidParameter3;
        }
        if (requiredSize)
            *requiredSize = size;
        return status::kSuccess;
    }
}

GUEST_EXPORT(ExGetXConfigSetting, ExGetXConfigSetting)
