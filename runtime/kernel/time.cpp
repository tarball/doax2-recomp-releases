// Time: the guest time base (mftb), system time and time-field conversion.
#include "guest_abi.h"
#include "netplay/state_census.h"
#include "ppc_overrides.h"

#include <windows.h>

namespace
{
    // The 360 kernel reports a 50 MHz performance counter, and mftb runs at the same rate.
    constexpr uint64_t kTimeBaseFrequency = 50000000;

    struct TimeBase
    {
        uint64_t hostFrequency;
        uint64_t hostStart;

        TimeBase()
        {
            LARGE_INTEGER value;
            QueryPerformanceFrequency(&value);
            hostFrequency = uint64_t(value.QuadPart);
            QueryPerformanceCounter(&value);
            hostStart = uint64_t(value.QuadPart);
        }
    };

    const TimeBase& GetTimeBase()
    {
        static const TimeBase timeBase;
        return timeBase;
    }

    // TIME_FIELDS
    struct TimeFields
    {
        be<uint16_t> year;
        be<uint16_t> month;
        be<uint16_t> day;
        be<uint16_t> hour;
        be<uint16_t> minute;
        be<uint16_t> second;
        be<uint16_t> milliseconds;
        be<uint16_t> weekday;
    };
    static_assert(sizeof(TimeFields) == 16);

    void KeQuerySystemTime(be<uint64_t>* time)
    {
        if (!time)
            return;
        if (netplay::census::Active())
            netplay::census::NoteRead("KeQuerySystemTime");
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        *time = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    }

    uint64_t KeQueryPerformanceFrequency()
    {
        return kTimeBaseFrequency;
    }

    void RtlTimeToTimeFields(const be<uint64_t>* time, TimeFields* fields)
    {
        if (!time || !fields)
            return;
        const uint64_t value = *time;
        const FILETIME fileTime{ DWORD(value), DWORD(value >> 32) };
        SYSTEMTIME systemTime{};
        FileTimeToSystemTime(&fileTime, &systemTime);
        fields->year = systemTime.wYear;
        fields->month = systemTime.wMonth;
        fields->day = systemTime.wDay;
        fields->hour = systemTime.wHour;
        fields->minute = systemTime.wMinute;
        fields->second = systemTime.wSecond;
        fields->milliseconds = systemTime.wMilliseconds;
        fields->weekday = systemTime.wDayOfWeek;
    }

    uint32_t RtlTimeFieldsToTime(const TimeFields* fields, be<uint64_t>* time)
    {
        if (!fields || !time)
            return 0;
        SYSTEMTIME systemTime{};
        systemTime.wYear = fields->year;
        systemTime.wMonth = fields->month;
        systemTime.wDay = fields->day;
        systemTime.wHour = fields->hour;
        systemTime.wMinute = fields->minute;
        systemTime.wSecond = fields->second;
        systemTime.wMilliseconds = fields->milliseconds;
        FILETIME fileTime;
        if (!SystemTimeToFileTime(&systemTime, &fileTime))
            return 0; // BOOLEAN FALSE: invalid fields
        *time = (uint64_t(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
        return 1;
    }
}

uint64_t DOA4QueryTimeBase()
{
    if (netplay::census::Active())
        netplay::census::NoteRead("mftb");
    const TimeBase& timeBase = GetTimeBase();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t elapsed = uint64_t(now.QuadPart) - timeBase.hostStart;
    // Whole seconds plus remainder avoids 128-bit division (a compiler-rt call clang-cl doesn't link);
    // remainder * 50 MHz fits in 64 bits for any host counter below ~3.7e11 Hz.
    const uint64_t seconds = elapsed / timeBase.hostFrequency;
    const uint64_t remainder = elapsed % timeBase.hostFrequency;
    return seconds * kTimeBaseFrequency + remainder * kTimeBaseFrequency / timeBase.hostFrequency;
}

GUEST_EXPORT(KeQuerySystemTime, KeQuerySystemTime)
GUEST_EXPORT(KeQueryPerformanceFrequency, KeQueryPerformanceFrequency)
GUEST_EXPORT(RtlTimeToTimeFields, RtlTimeToTimeFields)
GUEST_EXPORT(RtlTimeFieldsToTime, RtlTimeFieldsToTime)
