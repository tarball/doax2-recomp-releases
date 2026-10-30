// XAM odds and ends: notifications, background tasks, the title loader, voice and execution info. (System UIs: ui.cpp.)
#include "guest_abi.h"
#include "kernel/module.h"
#include "kernel/objects.h"
#include "kernel/threads.h"
#include "log.h"
#include "xam/xam.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace kernel;

    // A notification listener: the notifications of the areas in its mask (bit n for area n), in order. It's an event,
    // signaled while any are queued.
    class NotificationListener : public GuestBackedObject
    {
    public:
        explicit NotificationListener(uint64_t mask) : GuestBackedObject(sizeof(DispatcherHeader)), mask(mask)
        {
            Header()->type = uint8_t(DispatcherType::NotificationEvent);
            Header()->size = sizeof(DispatcherHeader) / 4;
        }
        const char* TypeName() const override { return "NotificationListener"; }
        uint64_t mask;
        std::deque<std::pair<uint32_t, uint32_t>> queue; // ID, parameter; under g_notificationLock
        // Heard a system UI open (XN_SYS_UI 1) and not yet that it closed: the close is queued once a poll finds nothing
        // else (see ui.cpp).
        bool uiOpen = false;
    };

    std::mutex g_notificationLock;
    std::vector<std::weak_ptr<NotificationListener>> g_listeners;

    // A listener of the system area hears at once who is signed in, as a console tells a title that starts: DOAX2 sets up
    // its signed-in users (their profile settings) on XN_SYS_SIGNINCHANGED and nowhere else.
    uint32_t XamNotifyCreateListener(uint64_t mask, uint32_t /*maxVersion*/)
    {
        auto listener = std::make_shared<NotificationListener>(mask);
        RegisterGuestObject(listener);
        {
            std::lock_guard lock(g_notificationLock);
            g_listeners.push_back(listener);
            if (mask & 1)
            {
                uint32_t signedIn = 0;
                for (uint32_t user = 0; user < xam::kMaxUsers; user++)
                    signedIn |= xam::IsUserSignedIn(user) ? 1u << user : 0;
                listener->queue.emplace_back(xam::kXnSysSigninChanged, signedIn);
                kernel::SetEvent(listener->GuestObject());
            }
        }
        return CreateHandle(listener);
    }

    // Returns TRUE and fills id/param if a notification was dequeued: the first one, or the first with ID `matchId`. A
    // system UI that opened closes (XN_SYS_UI 0 queued) on the first poll after its opening that finds nothing to give, so
    // the game sees it open for at least a poll however many notifications it takes at a time, at the same point of its
    // own polls on every machine (a session's lockstep menus may show a prompt on both, as DOA4's Versus menus do). The
    // queues are the host's, outside savestates: why DOA4 has no XN_SYS_UI (CMakeLists.txt's
    // GAME_SYSTEM_UI_NOTIFICATIONS).
    uint32_t XNotifyGetNext(uint32_t handle, uint32_t matchId, be<uint32_t>* id, be<uint32_t>* param)
    {
        auto listener = LookupHandleAs<NotificationListener>(handle);
        if (!listener)
            return 0;
        std::lock_guard lock(g_notificationLock);
        auto& queue = listener->queue;
        const auto it = std::find_if(queue.begin(), queue.end(), [matchId](const auto& entry) { return !matchId || entry.first == matchId; });
        if (it == queue.end())
        {
            if (listener->uiOpen)
            {
                listener->uiOpen = false;
                queue.emplace_back(xam::kXnSysUi, 0);
                kernel::SetEvent(listener->GuestObject());
            }
            return 0;
        }
        if (id)
            *id = it->first;
        if (param)
            *param = it->second;
        queue.erase(it);
        if (queue.empty())
            kernel::ResetEvent(listener->GuestObject());
        return 1;
    }

    // Background tasks run their callback on a new guest thread: XamTaskSchedule(callback, context, unknown, &handle).
    uint32_t XamTaskSchedule(uint32_t callback, uint32_t context, uint32_t /*unknown*/, be<uint32_t>* handle)
    {
        GuestThread::Params params;
        params.startAddress = callback;
        params.startContext = context;
        auto thread = GuestThread::Create(params);
        if (!thread)
            return xam::kErrorFunctionFailed;
        if (handle)
            *handle = CreateHandle(thread);
        return xam::kErrorSuccess;
    }

    uint32_t XamTaskShouldExit(uint32_t /*handle*/) { return 0; }

    uint32_t XamTaskCloseHandle(uint32_t handle)
    {
        return CloseHandle(handle) ? xam::kErrorSuccess : xam::kErrorInvalidParameter;
    }

    [[noreturn]] void XamLoaderTerminateTitle()
    {
        LOG_INFO("XamLoaderTerminateTitle: title exited");
        std::exit(0);
    }

    [[noreturn]] void XamLoaderLaunchTitle(const char* name, uint32_t flags)
    {
        LOG_INFO("XamLoaderLaunchTitle(\"%s\", 0x%X): launching another title isn't supported; exiting", name ? name : "", flags);
        std::exit(0);
    }

    // XamGetExecutionId(&info): points at the XEX's execution info header.
    uint32_t XamGetExecutionId(be<uint32_t>* info)
    {
        const uint32_t executionInfo = kernel::GetXexOptionalHeader(kernel::ExecutableXexHeader(), 0x00040006);
        if (info)
            *info = executionInfo;
        return executionInfo ? 0 : 0xC0000001;
    }

    // No headset is ever connected.
    uint32_t XamVoiceCreate(uint32_t /*userIndex*/, uint32_t /*flags*/, be<uint32_t>* voice)
    {
        if (voice)
            *voice = 0;
        return xam::kErrorDeviceNotConnected;
    }
    uint32_t XamVoiceHeadsetPresent(uint32_t /*voice*/) { return 0; }
    uint32_t XamVoiceClose(uint32_t /*voice*/) { return 0; }
    uint32_t XamVoiceSubmitPacket(uint32_t, uint32_t, uint32_t) { return xam::kErrorDeviceNotConnected; }
}

namespace xam
{
    void BroadcastNotification(uint32_t id, uint32_t parameter)
    {
        const uint32_t area = (id >> 25) & 0x3F;
        std::lock_guard lock(g_notificationLock);
        for (auto it = g_listeners.begin(); it != g_listeners.end();)
        {
            const auto listener = it->lock();
            if (!listener)
            {
                it = g_listeners.erase(it);
                continue;
            }
            if (listener->mask & (1ull << area))
            {
                listener->queue.emplace_back(id, parameter);
                kernel::SetEvent(listener->GuestObject());
                if (id == kXnSysUi && parameter != 0)
                    listener->uiOpen = true; // closes on a poll that finds nothing else (XNotifyGetNext)
            }
            ++it;
        }
    }
}

GUEST_EXPORT(XamNotifyCreateListener, XamNotifyCreateListener)
GUEST_EXPORT(XNotifyGetNext, XNotifyGetNext)
GUEST_EXPORT(XamTaskSchedule, XamTaskSchedule)
GUEST_EXPORT(XamTaskShouldExit, XamTaskShouldExit)
GUEST_EXPORT(XamTaskCloseHandle, XamTaskCloseHandle)
GUEST_EXPORT(XamLoaderTerminateTitle, XamLoaderTerminateTitle)
GUEST_EXPORT(XamLoaderLaunchTitle, XamLoaderLaunchTitle)
GUEST_EXPORT(XamGetExecutionId, XamGetExecutionId)
GUEST_EXPORT(XamVoiceCreate, XamVoiceCreate)
GUEST_EXPORT(XamVoiceHeadsetPresent, XamVoiceHeadsetPresent)
GUEST_EXPORT(XamVoiceClose, XamVoiceClose)
GUEST_EXPORT(XamVoiceSubmitPacket, XamVoiceSubmitPacket)
