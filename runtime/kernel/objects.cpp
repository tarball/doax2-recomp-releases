#include "kernel/objects.h"

#include "kernel/status.h"
#include "kernel/threads.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <list>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace kernel
{
    namespace
    {
        // Handle values look like the 360's (0xF8000000 | index << 2), never colliding with pseudo-handles.
        constexpr uint32_t kHandleBase = 0xF8000000;

        struct HandleTable
        {
            std::mutex lock;
            std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> handles;
            uint32_t next = 1;
        };

        HandleTable& Handles()
        {
            static HandleTable table;
            return table;
        }

        struct GuestObjects
        {
            std::mutex lock;
            std::unordered_map<uint32_t, std::weak_ptr<KernelObject>> objects;
        };

        GuestObjects& Objects()
        {
            static GuestObjects objects;
            return objects;
        }

        struct Waiter
        {
            std::span<const uint32_t> headers;
            bool waitAll;
            int32_t satisfied = -1;
            std::condition_variable_any wake;
        };

        std::list<Waiter*>& Waiters()
        {
            static std::list<Waiter*> waiters;
            return waiters;
        }

        DispatcherHeader* Header(uint32_t address)
        {
            return guest::Translate<DispatcherHeader>(address);
        }

        bool IsSignaled(const DispatcherHeader* header)
        {
            return header->signalState > 0;
        }

        // Applies the side effect of a satisfied wait.
        void Acquire(DispatcherHeader* header)
        {
            switch (DispatcherType(header->type))
            {
            case DispatcherType::SynchronizationEvent:
            case DispatcherType::SynchronizationTimer:
                header->signalState = 0;
                break;
            case DispatcherType::Semaphore:
                header->signalState = header->signalState - 1;
                break;
            default:
                break; // notification objects, threads and processes stay signaled
            }
        }

        // Returns the index of the satisfied object (0 for wait-all) after acquiring, or -1.
        int32_t TrySatisfy(std::span<const uint32_t> headers, bool waitAll)
        {
            if (waitAll)
            {
                for (uint32_t address : headers)
                {
                    if (!IsSignaled(Header(address)))
                        return -1;
                }
                for (uint32_t address : headers)
                    Acquire(Header(address));
                return 0;
            }

            for (size_t i = 0; i < headers.size(); i++)
            {
                DispatcherHeader* header = Header(headers[i]);
                if (IsSignaled(header))
                {
                    Acquire(header);
                    return int32_t(i);
                }
            }
            return -1;
        }

        uint64_t SystemTimeNow()
        {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            return (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        }
    }

    GuestBackedObject::GuestBackedObject(uint32_t size)
        : m_address(guest::KernelAlloc(size)), m_size(size)
    {
    }

    GuestBackedObject::~GuestBackedObject()
    {
        guest::KernelFree(m_address, m_size);
    }

    uint32_t CreateHandle(std::shared_ptr<KernelObject> object)
    {
        HandleTable& table = Handles();
        std::lock_guard lock(table.lock);
        const uint32_t handle = kHandleBase | (table.next++ << 2);
        table.handles.emplace(handle, std::move(object));
        return handle;
    }

    bool CloseHandle(uint32_t handle)
    {
        HandleTable& table = Handles();
        std::shared_ptr<KernelObject> released; // destroyed outside the lock
        std::lock_guard lock(table.lock);
        const auto it = table.handles.find(handle);
        if (it == table.handles.end())
            return false;
        released = std::move(it->second);
        table.handles.erase(it);
        return true;
    }

    std::shared_ptr<KernelObject> LookupHandle(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle)
            return CurrentThreadObject();

        HandleTable& table = Handles();
        std::lock_guard lock(table.lock);
        const auto it = table.handles.find(handle);
        return it != table.handles.end() ? it->second : nullptr;
    }

    void RegisterGuestObject(const std::shared_ptr<KernelObject>& object)
    {
        if (!object || !object->GuestObject())
            return;
        GuestObjects& objects = Objects();
        std::lock_guard lock(objects.lock);
        objects.objects[object->GuestObject()] = object;
    }

    void UnregisterGuestObject(uint32_t guestObject)
    {
        GuestObjects& objects = Objects();
        std::lock_guard lock(objects.lock);
        objects.objects.erase(guestObject);
    }

    std::shared_ptr<KernelObject> LookupGuestObject(uint32_t guestObject)
    {
        GuestObjects& objects = Objects();
        std::lock_guard lock(objects.lock);
        const auto it = objects.objects.find(guestObject);
        return it != objects.objects.end() ? it->second.lock() : nullptr;
    }

    std::recursive_mutex& DispatcherLock()
    {
        static std::recursive_mutex lock;
        return lock;
    }

    void SatisfyWaiters()
    {
        std::lock_guard lock(DispatcherLock());
        auto& waiters = Waiters();
        for (auto it = waiters.begin(); it != waiters.end();)
        {
            Waiter* waiter = *it;
            const int32_t index = TrySatisfy(waiter->headers, waiter->waitAll);
            if (index >= 0)
            {
                waiter->satisfied = index;
                it = waiters.erase(it);
                waiter->wake.notify_one();
            }
            else
            {
                ++it;
            }
        }
    }

    uint32_t WaitForHeaders(std::span<const uint32_t> headers, bool waitAll, const be<int64_t>* timeout)
    {
        using Clock = std::chrono::steady_clock;

        std::unique_lock lock(DispatcherLock());
        const int32_t immediate = TrySatisfy(headers, waitAll);
        if (immediate >= 0)
            return waitAll ? status::kSuccess : status::kSuccess + uint32_t(immediate);

        const bool infinite = timeout == nullptr;
        const int64_t interval = infinite ? 0 : int64_t(*timeout);
        if (!infinite && interval == 0)
            return status::kTimeout;

        Clock::time_point deadline{};
        if (!infinite)
        {
            // Negative: relative 100 ns units. Positive: absolute system time.
            const int64_t relative = interval < 0 ? -interval : std::max<int64_t>(0, interval - int64_t(SystemTimeNow()));
            deadline = Clock::now() + std::chrono::nanoseconds(relative * 100);
        }

        // The thread counts as waiting until it returns, and says what for (unless a critical section's wait already has).
        GuestThread* thread = CurrentThread();
        struct Waiting
        {
            GuestThread* thread;
            bool described = false;
            Waiting(GuestThread* t, std::span<const uint32_t> headers, bool waitAll) : thread(t)
            {
                if (!thread)
                    return;
                thread->waits++;
                if (thread->waitKind.load(std::memory_order_relaxed) == GuestThread::WaitKind::kNone)
                {
                    described = true;
                    if (headers.empty())
                        thread->waitObjects[0].store(0, std::memory_order_relaxed);
                    for (size_t i = 0; i < std::min(headers.size(), GuestThread::kWaitObjects); i++)
                        thread->waitObjects[i].store(headers[i], std::memory_order_relaxed);
                    thread->waitCount.store(uint32_t(headers.size()), std::memory_order_relaxed);
                    thread->waitKind.store(waitAll ? GuestThread::WaitKind::kAll : GuestThread::WaitKind::kAny, std::memory_order_release);
                }
            }
            ~Waiting()
            {
                if (!thread)
                    return;
                if (described)
                    thread->waitKind.store(GuestThread::WaitKind::kNone, std::memory_order_release);
                thread->waits--;
            }
        } waiting(thread, headers, waitAll);
        Waiter waiter{ headers, waitAll };
        auto& waiters = Waiters();
        waiters.push_back(&waiter);
        while (waiter.satisfied < 0)
        {
            if (infinite)
            {
                waiter.wake.wait(lock);
            }
            else if (waiter.wake.wait_until(lock, deadline) == std::cv_status::timeout && waiter.satisfied < 0)
            {
                waiters.remove(&waiter);
                return status::kTimeout;
            }
        }
        return waitAll ? status::kSuccess : status::kSuccess + uint32_t(waiter.satisfied);
    }

    int32_t SetEvent(uint32_t event)
    {
        std::lock_guard lock(DispatcherLock());
        DispatcherHeader* header = Header(event);
        const int32_t previous = header->signalState;
        header->signalState = 1;
        SatisfyWaiters();
        return previous;
    }

    int32_t ResetEvent(uint32_t event)
    {
        std::lock_guard lock(DispatcherLock());
        DispatcherHeader* header = Header(event);
        const int32_t previous = header->signalState;
        header->signalState = 0;
        return previous;
    }

    int32_t PulseEvent(uint32_t event)
    {
        // Releases whoever is waiting right now, then leaves the event non-signaled.
        std::lock_guard lock(DispatcherLock());
        DispatcherHeader* header = Header(event);
        const int32_t previous = header->signalState;
        header->signalState = 1;
        SatisfyWaiters();
        header->signalState = 0;
        return previous;
    }

    void SignalHeader(uint32_t address, int32_t state)
    {
        std::lock_guard lock(DispatcherLock());
        Header(address)->signalState = state;
        SatisfyWaiters();
    }
}
