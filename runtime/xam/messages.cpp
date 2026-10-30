// XMsg*: messages to XAM's in-process apps (0xFA music player, 0xFB achievements, 0xFC Live base).
// The music player is xmp.cpp; other known messages are answered as the offline dashboard would (values per Xenia);
// others are logged.
#include "guest_abi.h"
#include "log.h"
#include "netplay/frame.h"
#include "xam/achievements.h"
#include "xam/xam.h"

namespace
{
    constexpr uint32_t kNotFound = 0x80070490;      // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
    constexpr uint32_t kInvalidArgument = 0x80070057; // E_INVALIDARG
    constexpr uint32_t kMaxAchievementsWritten = 64;

    uint32_t Dispatch(uint32_t app, uint32_t message, uint32_t buffer, uint32_t bufferLength)
    {
        if (app == 0xFA)
            return xam::XmpDispatch(message, buffer, bufferLength);

        if (app == 0xFC && message == 0x00058004)
        {
            // XLiveBase startup query: answers a single boolean.
            if (buffer)
                guest::StoreU32(buffer, 1);
            return xam::kErrorSuccess;
        }
        if (app == 0xFB && message == 0x000B0008)
        {
            // XUserWriteAchievements: the buffer holds the count and the address of the achievements, 8 bytes each: the
            // game user and the achievement's ID. Each newly earned one is recorded and shown (xam/achievements.h), and
            // the write succeeds, as the dashboard answers it.
            if (!buffer || bufferLength < 8)
                return kInvalidArgument;
            const uint32_t count = guest::LoadU32(buffer), list = guest::LoadU32(buffer + 4);
            if (count > kMaxAchievementsWritten ||
                (count && (list > 0xFFFFFFFFu - count * 8 || !guest::IsAllocated(list) || !guest::IsAllocated(list + count * 8 - 1))))
            {
                LOG_WARN("XMsg: XUserWriteAchievements with %u achievements at 0x%08X", count, list);
                return kInvalidArgument;
            }
            for (uint32_t i = 0; i < count; i++)
                achievements::Earn(guest::LoadU32(list + i * 8), guest::LoadU32(list + i * 8 + 4));
            return xam::kErrorSuccess;
        }

        // A small buffer's words too. (0x0007001A to 0xFA, Xenia's XMPSetPlaybackController, has three: DOA4 sends 2, 0, 1 as
        // its boot movie starts and 2, 4, 0 as it ends, and nothing waits for the answer.)
        char words[40] = "";
        if (buffer && bufferLength && bufferLength <= 16 && bufferLength % 4 == 0 && guest::IsAllocated(buffer) &&
            guest::IsAllocated(buffer + bufferLength - 1))
        {
            char* end = words;
            for (uint32_t i = 0; i < bufferLength; i += 4)
                end += snprintf(end, size_t(words + sizeof(words) - end), "%s%08X", i ? " " : ": ", guest::LoadU32(buffer + i));
        }
        LOG_WARN("XMsg: unhandled message 0x%08X to app 0x%02X (buffer 0x%08X, %u bytes%s) at frame %llu", message, app, buffer, bufferLength,
            words, (unsigned long long)netplay::Frame());
        return kNotFound;
    }

    uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, uint32_t argument1, uint32_t argument2)
    {
        return Dispatch(app, message, argument1, argument2);
    }

    uint32_t XMsgStartIORequest(uint32_t app, uint32_t message, xam::Overlapped* overlapped, uint32_t buffer, uint32_t bufferLength)
    {
        const uint32_t result = Dispatch(app, message, buffer, bufferLength);
        return xam::Complete(overlapped, result);
    }

    uint32_t XMsgStartIORequestEx(uint32_t app, uint32_t message, xam::Overlapped* overlapped, uint32_t buffer, uint32_t bufferLength, uint32_t /*extra*/)
    {
        const uint32_t result = Dispatch(app, message, buffer, bufferLength);
        return xam::Complete(overlapped, result);
    }

    // Requests complete immediately, so there's never anything to cancel.
    uint32_t XMsgCancelIORequest(xam::Overlapped* /*overlapped*/, uint32_t /*wait*/)
    {
        return xam::kErrorSuccess;
    }
}

GUEST_EXPORT(XMsgInProcessCall, XMsgInProcessCall)
GUEST_EXPORT(XMsgStartIORequest, XMsgStartIORequest)
GUEST_EXPORT(XMsgStartIORequestEx, XMsgStartIORequestEx)
GUEST_EXPORT(XMsgCancelIORequest, XMsgCancelIORequest)
