#pragma once

#include "guest_abi.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace xam
{
    inline constexpr uint32_t kErrorSuccess = 0;
    inline constexpr uint32_t kErrorFunctionFailed = 1627;
    inline constexpr uint32_t kErrorNotEnoughMemory = 8;
    inline constexpr uint32_t kErrorInvalidParameter = 87;
    inline constexpr uint32_t kErrorIoPending = 997;
    inline constexpr uint32_t kErrorNoMoreFiles = 18;
    inline constexpr uint32_t kErrorDeviceNotConnected = 1167;
    inline constexpr uint32_t kErrorEmpty = 4306;
    inline constexpr uint32_t kErrorNoSuchUser = 1317;
    inline constexpr uint32_t kErrorNotLoggedOn = 1245;
    inline constexpr uint32_t kErrorCancelled = 1223;
    inline constexpr uint32_t kErrorInsufficientBuffer = 122;

    // Notifications of the system area.
    inline constexpr uint32_t kXnSysUi = 0x00000009;            // a system UI opened (1) or closed (0)
    inline constexpr uint32_t kXnSysSigninChanged = 0x0000000A; // the users signed in (a mask) changed

    inline constexpr uint32_t kTitleId = 0x544307D2; // Dead or Alive Xtreme 2
    inline constexpr uint64_t kLocalXuid = 0xB13EBABEBABEBABEull; // offline profile XUID (as Xenia uses)
    inline constexpr const char* kLocalGamertag = "Player";
    inline constexpr uint32_t kMaxUsers = 4;

    // XOVERLAPPED, used by asynchronous XAM calls.
    struct Overlapped
    {
        be<uint32_t> internalLow;     // result
        be<uint32_t> internalHigh;    // length
        be<uint32_t> internalContext;
        be<uint32_t> event;           // handle signalled on completion
        be<uint32_t> completionRoutine;
        be<uint32_t> completionContext;
        be<uint32_t> extendedError;
    };
    static_assert(sizeof(Overlapped) == 0x1C);

    // Completes an asynchronous request immediately. With an XOVERLAPPED it stores the results,
    // signals its event and returns ERROR_IO_PENDING; without one it returns `result`.
    uint32_t Complete(Overlapped* overlapped, uint32_t result, uint32_t length = 0, uint32_t extendedError = 0);

    bool IsUserSignedIn(uint32_t userIndex);

    // Queues a notification (XN_*) for every listener of its area (bits 25-30 of the ID) and signals them.
    void BroadcastNotification(uint32_t id, uint32_t parameter);

    // Runs a system UI without showing anything (ui.cpp): XN_SYS_UI says it opened, `answer` gives its result at once
    // (writing the UI's outputs), and XN_SYS_UI says it closed on each listener's next poll that finds nothing else
    // (misc.cpp), the notifications only where GAME_SYSTEM_UI_NOTIFICATIONS is 1 (CMakeLists.txt). With an XOVERLAPPED
    // the call completes it and returns ERROR_IO_PENDING; without one, it returns the answer.
    uint32_t ShowSystemUi(const char* name, std::function<uint32_t()> answer, Overlapped* overlapped);

    // An enumeration of `items` (each `itemSize` bytes) for XamEnumerate (content.cpp). Returns its handle.
    uint32_t CreateEnumerator(uint32_t itemSize, uint32_t itemsPerCall, std::vector<std::vector<uint8_t>> items = {});

    // A message to the music player, app 0xFA (xmp.cpp). Returns an HRESULT.
    uint32_t XmpDispatch(uint32_t message, uint32_t buffer, uint32_t length);
}
