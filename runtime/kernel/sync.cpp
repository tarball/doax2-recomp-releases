// Synchronization exports: events, semaphores, timers, waits, critical sections, spinlocks/IRQL, TLS and handles.
#include "guest_abi.h"
#include "kernel/objects.h"
#include "kernel/status.h"
#include "kernel/threads.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <immintrin.h>
#include <windows.h>

namespace
{
    using namespace kernel;

    // ---------------------------------------------------------------------------------------------
    // Atomics on big-endian guest words, compatible with the guest's own lwarx/stwcx. (compare-and-swap).

    std::atomic_ref<uint32_t> AtomicWord(void* word)
    {
        guest::BeforeHostWriteTo(word, 4); // the store map's (guest_memory.h)
        return std::atomic_ref<uint32_t>(*static_cast<uint32_t*>(word));
    }

    // Adds `delta` and returns the new value.
    int32_t AtomicAdd(be<int32_t>* value, int32_t delta)
    {
        auto word = AtomicWord(value);
        uint32_t expected = word.load();
        for (;;)
        {
            const int32_t current = int32_t(__builtin_bswap32(expected));
            const uint32_t desired = __builtin_bswap32(uint32_t(current + delta));
            if (word.compare_exchange_weak(expected, desired))
                return current + delta;
        }
    }

    bool CompareExchange(void* word, uint32_t expected, uint32_t desired)
    {
        uint32_t raw = __builtin_bswap32(expected);
        return AtomicWord(word).compare_exchange_strong(raw, __builtin_bswap32(desired));
    }

    // ---------------------------------------------------------------------------------------------
    // Events

    class Event : public GuestBackedObject
    {
    public:
        Event(DispatcherType type, bool signaled) : GuestBackedObject(sizeof(DispatcherHeader))
        {
            Header()->type = uint8_t(type);
            Header()->size = sizeof(DispatcherHeader) / 4;
            Header()->signalState = signaled ? 1 : 0;
        }
        const char* TypeName() const override { return "Event"; }
    };

    uint32_t NtCreateEvent(be<uint32_t>* handle, uint32_t /*objectAttributes*/, uint32_t eventType, uint32_t initialState)
    {
        if (!handle)
            return status::kInvalidParameter;
        auto event = std::make_shared<Event>(eventType == 0 ? DispatcherType::NotificationEvent : DispatcherType::SynchronizationEvent, initialState != 0);
        RegisterGuestObject(event);
        *handle = CreateHandle(event);
        return status::kSuccess;
    }

    // Resolves a handle to its waitable guest header.
    uint32_t WaitableHeader(uint32_t handle)
    {
        const auto object = LookupHandle(handle);
        return object && object->IsWaitable() ? object->GuestObject() : 0;
    }

    template <int32_t (*Operation)(uint32_t)>
    uint32_t EventOperation(uint32_t handle, be<int32_t>* previousState)
    {
        const uint32_t header = WaitableHeader(handle);
        if (!header)
            return status::kInvalidHandle;
        const int32_t previous = Operation(header);
        if (previousState)
            *previousState = previous;
        return status::kSuccess;
    }

    uint32_t NtSetEvent(uint32_t handle, be<int32_t>* previousState) { return EventOperation<kernel::SetEvent>(handle, previousState); }
    uint32_t NtPulseEvent(uint32_t handle, be<int32_t>* previousState) { return EventOperation<kernel::PulseEvent>(handle, previousState); }
    uint32_t NtClearEvent(uint32_t handle) { return EventOperation<kernel::ResetEvent>(handle, nullptr); }

    int32_t KeSetEvent(uint32_t event, uint32_t /*increment*/, uint32_t /*wait*/)
    {
        return event ? kernel::SetEvent(event) : 0;
    }

    int32_t KeResetEvent(uint32_t event)
    {
        return event ? kernel::ResetEvent(event) : 0;
    }

    // ---------------------------------------------------------------------------------------------
    // Semaphores (KSEMAPHORE: the header's signal state is the count, then the limit). Waits take one each.

    struct Semaphore
    {
        DispatcherHeader header;
        be<int32_t> limit;
    };
    static_assert(sizeof(Semaphore) == 0x14);

    constexpr uint32_t kSemaphoreLimitExceeded = 0xC0000047;

    void InitializeSemaphore(Semaphore* semaphore, int32_t count, int32_t limit)
    {
        semaphore->header.type = uint8_t(DispatcherType::Semaphore);
        semaphore->header.absolute = 0;
        semaphore->header.size = sizeof(Semaphore) / 4;
        semaphore->header.inserted = 0;
        semaphore->header.signalState = count;
        semaphore->header.waitListFlink = guest::ToGuest(&semaphore->header.waitListFlink);
        semaphore->header.waitListBlink = guest::ToGuest(&semaphore->header.waitListFlink);
        semaphore->limit = limit;
    }

    // Adds `adjustment` to the count and wakes waiters. Returns false (changing nothing) past the limit.
    bool ReleaseSemaphore(Semaphore* semaphore, int32_t adjustment, int32_t& previous)
    {
        std::lock_guard lock(DispatcherLock());
        previous = semaphore->header.signalState;
        const int64_t count = int64_t(previous) + adjustment;
        if (adjustment <= 0 || count > int32_t(semaphore->limit))
            return false;
        semaphore->header.signalState = int32_t(count);
        SatisfyWaiters();
        return true;
    }

    void KeInitializeSemaphore(Semaphore* semaphore, int32_t count, int32_t limit)
    {
        if (semaphore)
            InitializeSemaphore(semaphore, count, limit);
    }

    int32_t KeReleaseSemaphore(Semaphore* semaphore, uint32_t /*increment*/, int32_t adjustment, uint32_t /*wait*/)
    {
        int32_t previous = 0;
        if (!semaphore || !ReleaseSemaphore(semaphore, adjustment, previous))
            LOG_WARN("KeReleaseSemaphore(%08X, %d): limit exceeded (count %d)", guest::ToGuest(semaphore), adjustment, previous);
        return previous;
    }

    class SemaphoreObject : public GuestBackedObject
    {
    public:
        SemaphoreObject(int32_t count, int32_t limit) : GuestBackedObject(sizeof(Semaphore))
        {
            InitializeSemaphore(guest::Translate<Semaphore>(GuestObject()), count, limit);
        }
        const char* TypeName() const override { return "Semaphore"; }
    };

    uint32_t NtCreateSemaphore(be<uint32_t>* handle, uint32_t /*objectAttributes*/, int32_t initialCount, int32_t maximumCount)
    {
        if (!handle || maximumCount <= 0 || initialCount < 0 || initialCount > maximumCount)
            return status::kInvalidParameter;
        auto semaphore = std::make_shared<SemaphoreObject>(initialCount, maximumCount);
        RegisterGuestObject(semaphore);
        *handle = CreateHandle(semaphore);
        return status::kSuccess;
    }

    uint32_t NtReleaseSemaphore(uint32_t handle, int32_t releaseCount, be<int32_t>* previousCount)
    {
        auto semaphore = LookupHandleAs<SemaphoreObject>(handle);
        if (!semaphore)
            return status::kInvalidHandle;
        int32_t previous = 0;
        if (!ReleaseSemaphore(guest::Translate<Semaphore>(semaphore->GuestObject()), releaseCount, previous))
            return kSemaphoreLimitExceeded;
        if (previousCount)
            *previousCount = previous;
        return status::kSuccess;
    }

    // ---------------------------------------------------------------------------------------------
    // Timers (KTIMER: a dispatcher header, the due time, a list entry, a DPC and the period). A host thread signals each
    // one when it's due, and again every period for periodic ones. Completion routines (APCs) aren't run: DOAX2's network
    // library (its only user) waits on its timer instead.

    constexpr uint32_t kTimerSize = 0x28;
    constexpr uint32_t kTimerResumeIgnored = 0x40000025; // STATUS_TIMER_RESUME_IGNORED

    class Timer : public GuestBackedObject
    {
    public:
        explicit Timer(DispatcherType type) : GuestBackedObject(kTimerSize)
        {
            Header()->type = uint8_t(type);
            Header()->size = kTimerSize / 4;
        }
        const char* TypeName() const override { return "Timer"; }

        // Under g_timerLock. `generation` changes whenever the timer is set or cancelled, which retires its earlier
        // entries in the schedule.
        uint64_t generation = 0;
        bool armed = false;
        std::chrono::steady_clock::duration period{};
    };

    using TimerClock = std::chrono::steady_clock;

    // Lock order: g_timerLock, then the dispatcher's.
    std::mutex g_timerLock;
    std::condition_variable g_timerWake;
    std::multimap<TimerClock::time_point, std::pair<std::weak_ptr<Timer>, uint64_t>> g_timerSchedule; // due -> timer, generation

    void TimerThread()
    {
        std::unique_lock lock(g_timerLock);
        for (;;)
        {
            if (g_timerSchedule.empty())
            {
                g_timerWake.wait(lock);
                continue;
            }
            const auto due = g_timerSchedule.begin()->first;
            if (TimerClock::now() < due)
            {
                g_timerWake.wait_until(lock, due);
                continue;
            }
            const auto [weakTimer, generation] = g_timerSchedule.begin()->second;
            g_timerSchedule.erase(g_timerSchedule.begin());
            const auto timer = weakTimer.lock();
            if (!timer || !timer->armed || timer->generation != generation)
                continue;
            if (timer->period.count() > 0)
            {
                // The next period, or one from now if the timer fell behind (no bursts of catching up).
                const auto now = TimerClock::now();
                const auto next = due + timer->period > now ? due + timer->period : now + timer->period;
                g_timerSchedule.emplace(next, std::make_pair(weakTimer, generation));
            }
            else
            {
                timer->armed = false;
            }
            kernel::SignalHeader(timer->GuestObject(), 1);
        }
    }

    // NtCreateTimer(&handle, attributes, type): a notification timer (0) stays signaled until it's set again; a
    // synchronization timer (1) releases one waiter.
    uint32_t NtCreateTimer(be<uint32_t>* handle, uint32_t /*objectAttributes*/, uint32_t timerType)
    {
        if (!handle || timerType > 1)
            return status::kInvalidParameter;
        auto timer = std::make_shared<Timer>(timerType == 0 ? DispatcherType::NotificationTimer : DispatcherType::SynchronizationTimer);
        RegisterGuestObject(timer);
        *handle = CreateHandle(timer);
        return status::kSuccess;
    }

    // NtSetTimerEx(timer, &due time, completion routine, its mode, its context, resume, period in ms, &previous state):
    // unsignals the timer and schedules it. The due time is relative when negative (100 ns units), else a system time.
    uint32_t NtSetTimerEx(uint32_t handle, const be<int64_t>* dueTime, uint32_t completionRoutine, uint32_t /*mode*/, uint32_t /*context*/,
        uint32_t resume, int32_t periodMs, uint8_t* previousState)
    {
        auto timer = LookupHandleAs<Timer>(handle);
        if (!timer)
            return status::kInvalidHandle;
        if (!dueTime || periodMs < 0)
            return status::kInvalidParameter;
        if (completionRoutine)
        {
            static std::once_flag warned;
            std::call_once(warned, [completionRoutine] { LOG_WARN("NtSetTimerEx: completion routines (0x%08X) aren't run", completionRoutine); });
        }

        constexpr uint64_t kMaxTicks = 365ull * 24 * 3600 * 10'000'000; // a year
        const int64_t value = *dueTime;
        uint64_t ticks = 0;
        if (value < 0)
        {
            ticks = 0 - uint64_t(value);
        }
        else
        {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            const uint64_t systemTime = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            ticks = uint64_t(value) > systemTime ? uint64_t(value) - systemTime : 0;
        }
        const auto due = TimerClock::now() + std::chrono::duration_cast<TimerClock::duration>(std::chrono::nanoseconds(std::min(ticks, kMaxTicks) * 100));

        static std::once_flag started;
        std::call_once(started, [] { std::thread(TimerThread).detach(); });
        {
            std::lock_guard lock(g_timerLock);
            {
                std::lock_guard dispatcher(DispatcherLock());
                if (previousState)
                    *previousState = timer->Header()->signalState > 0 ? 1 : 0;
                timer->Header()->signalState = 0;
            }
            timer->generation++;
            timer->armed = true;
            timer->period = std::chrono::milliseconds(periodMs);
            g_timerSchedule.emplace(due, std::make_pair(std::weak_ptr<Timer>(timer), timer->generation));
        }
        g_timerWake.notify_one();
        return resume ? kTimerResumeIgnored : status::kSuccess;
    }

    // NtCancelTimer(timer, &current state): unschedules it, leaving its state as it is.
    uint32_t NtCancelTimer(uint32_t handle, uint8_t* currentState)
    {
        auto timer = LookupHandleAs<Timer>(handle);
        if (!timer)
            return status::kInvalidHandle;
        std::lock_guard lock(g_timerLock);
        timer->generation++;
        timer->armed = false;
        if (currentState)
        {
            std::lock_guard dispatcher(DispatcherLock());
            *currentState = timer->Header()->signalState > 0 ? 1 : 0;
        }
        return status::kSuccess;
    }

    // ---------------------------------------------------------------------------------------------
    // Waits

    uint32_t KeWaitForSingleObject(uint32_t object, uint32_t /*waitReason*/, uint32_t /*processorMode*/, uint32_t /*alertable*/, const be<int64_t>* timeout)
    {
        if (!object)
            return status::kInvalidParameter;
        const uint32_t headers[] = { object };
        return WaitForHeaders(headers, false, timeout);
    }

    // KeWaitForMultipleObjects(count, objects, waitType, waitReason, mode, alertable, timeout, waitBlocks): the objects
    // are dispatcher headers. WaitType: 0 = WaitAll, 1 = WaitAny.
    uint32_t KeWaitForMultipleObjects(uint32_t count, const be<uint32_t>* objects, uint32_t waitType, uint32_t /*waitReason*/,
        uint32_t /*processorMode*/, uint32_t /*alertable*/, const be<int64_t>* timeout, uint32_t /*waitBlocks*/)
    {
        if (!objects || count == 0 || count > 64)
            return status::kInvalidParameter;
        std::vector<uint32_t> headers(count);
        for (uint32_t i = 0; i < count; i++)
        {
            headers[i] = objects[i];
            if (!headers[i])
                return status::kInvalidParameter;
        }
        return WaitForHeaders(headers, waitType == 0, timeout);
    }

    uint32_t NtWaitForSingleObjectEx(uint32_t handle, uint32_t /*waitMode*/, uint32_t /*alertable*/, const be<int64_t>* timeout)
    {
        const uint32_t header = WaitableHeader(handle);
        if (!header)
            return status::kInvalidHandle;
        const uint32_t headers[] = { header };
        return WaitForHeaders(headers, false, timeout);
    }

    uint32_t NtWaitForMultipleObjectsEx(uint32_t count, const be<uint32_t>* handles, uint32_t waitType, uint32_t /*waitMode*/, uint32_t /*alertable*/, const be<int64_t>* timeout)
    {
        if (!handles || count == 0 || count > 64)
            return status::kInvalidParameter;
        std::vector<uint32_t> headers(count);
        for (uint32_t i = 0; i < count; i++)
        {
            headers[i] = WaitableHeader(handles[i]);
            if (!headers[i])
                return status::kInvalidHandle;
        }
        // WaitType: 0 = WaitAll, 1 = WaitAny.
        return WaitForHeaders(headers, waitType == 0, timeout);
    }

    uint32_t NtSignalAndWaitForSingleObjectEx(uint32_t signalHandle, uint32_t waitHandle, uint32_t /*waitMode*/, uint32_t /*alertable*/, const be<int64_t>* timeout)
    {
        const uint32_t signalHeader = WaitableHeader(signalHandle);
        const uint32_t waitHeader = WaitableHeader(waitHandle);
        if (!signalHeader || !waitHeader)
            return status::kInvalidHandle;
        kernel::SetEvent(signalHeader);
        const uint32_t headers[] = { waitHeader };
        return WaitForHeaders(headers, false, timeout);
    }

    // Owner identity for locks: the caller's KTHREAD, or a unique nonzero value for host-only threads
    // (which have no KTHREAD) so recursion checks still work when the runtime itself takes a guest lock.
    uint32_t OwnerId()
    {
        if (const uint32_t kthread = CurrentKThread())
            return kthread;
        return 0x80000000u | (GetCurrentThreadId() & 0x7FFFFFFFu);
    }

    // ---------------------------------------------------------------------------------------------
    // Critical sections (X_RTL_CRITICAL_SECTION), waiting on their embedded synchronization event.

    struct CriticalSection
    {
        DispatcherHeader header;
        be<int32_t> lockCount;      // -1 when free
        be<int32_t> recursionCount;
        be<uint32_t> owningThread;  // KTHREAD of the owner
    };
    static_assert(sizeof(CriticalSection) == 0x1C);

    void RtlInitializeCriticalSection(CriticalSection* section)
    {
        if (!section)
            return;
        section->header.type = uint8_t(DispatcherType::SynchronizationEvent);
        section->header.absolute = 0; // spin count / 256
        section->header.size = sizeof(DispatcherHeader) / 4;
        section->header.inserted = 0;
        section->header.signalState = 0;
        section->header.waitListFlink = guest::ToGuest(&section->header.waitListFlink);
        section->header.waitListBlink = guest::ToGuest(&section->header.waitListFlink);
        section->lockCount = -1;
        section->recursionCount = 0;
        section->owningThread = 0;
    }

    void RtlEnterCriticalSection(CriticalSection* section)
    {
        const uint32_t self = OwnerId();
        if (section->owningThread == self)
        {
            AtomicAdd(&section->lockCount, 1);
            section->recursionCount = section->recursionCount + 1;
            return;
        }

        for (uint32_t spin = uint32_t(section->header.absolute) * 256; spin > 0; spin--)
        {
            if (CompareExchange(&section->lockCount, uint32_t(-1), 0))
            {
                section->owningThread = self;
                section->recursionCount = 1;
                return;
            }
            _mm_pause();
        }

        if (AtomicAdd(&section->lockCount, 1) != 0)
        {
            // Contended: wait for the owner's release to signal the embedded event (the thread dumps name the section).
            const uint32_t headers[] = { guest::ToGuest(&section->header) };
            GuestThread* thread = CurrentThread();
            if (thread)
            {
                thread->waitObjects[0].store(headers[0], std::memory_order_relaxed);
                thread->waitCount.store(1, std::memory_order_relaxed);
                thread->waitKind.store(GuestThread::WaitKind::kCriticalSection, std::memory_order_release);
            }
            WaitForHeaders(headers, false, nullptr);
            if (thread)
                thread->waitKind.store(GuestThread::WaitKind::kNone, std::memory_order_release);
        }
        section->owningThread = self;
        section->recursionCount = 1;
    }

    uint32_t RtlTryEnterCriticalSection(CriticalSection* section)
    {
        const uint32_t self = OwnerId();
        if (CompareExchange(&section->lockCount, uint32_t(-1), 0))
        {
            section->owningThread = self;
            section->recursionCount = 1;
            return 1;
        }
        if (section->owningThread == self)
        {
            AtomicAdd(&section->lockCount, 1);
            section->recursionCount = section->recursionCount + 1;
            return 1;
        }
        return 0;
    }

    void RtlLeaveCriticalSection(CriticalSection* section)
    {
        const int32_t recursion = section->recursionCount - 1;
        section->recursionCount = recursion;
        if (recursion != 0)
        {
            AtomicAdd(&section->lockCount, -1);
            return;
        }

        section->owningThread = 0;
        if (AtomicAdd(&section->lockCount, -1) != -1)
            kernel::SetEvent(guest::ToGuest(&section->header)); // wake one waiter (synchronization event)
    }

    // ---------------------------------------------------------------------------------------------
    // Spinlocks and IRQL. A held spinlock contains the owner's KTHREAD (never 0).

    constexpr uint8_t kDispatchLevel = 2;

    uint8_t& CurrentIrql()
    {
        static thread_local uint8_t hostIrql = 0;
        GuestThread* thread = CurrentThread();
        return thread ? thread->irql : hostIrql;
    }

    void AcquireSpinLock(be<uint32_t>* lock)
    {
        const uint32_t owner = OwnerId();
        while (!CompareExchange(lock, 0, owner))
        {
            for (int i = 0; i < 64 && uint32_t(*lock) != 0; i++)
                _mm_pause();
            if (uint32_t(*lock) != 0)
                SwitchToThread();
        }
    }

    void ReleaseSpinLock(be<uint32_t>* lock)
    {
        AtomicWord(lock).store(0);
    }

    uint32_t KfAcquireSpinLock(be<uint32_t>* lock)
    {
        uint8_t& irql = CurrentIrql();
        const uint8_t previous = irql;
        irql = kDispatchLevel;
        AcquireSpinLock(lock);
        return previous;
    }

    void KfReleaseSpinLock(be<uint32_t>* lock, uint32_t previousIrql)
    {
        ReleaseSpinLock(lock);
        CurrentIrql() = uint8_t(previousIrql);
    }

    void KeAcquireSpinLockAtRaisedIrql(be<uint32_t>* lock) { AcquireSpinLock(lock); }
    void KeReleaseSpinLockFromRaisedIrql(be<uint32_t>* lock) { ReleaseSpinLock(lock); }

    uint32_t KeTryToAcquireSpinLockAtRaisedIrql(be<uint32_t>* lock)
    {
        return CompareExchange(lock, 0, OwnerId()) ? 1 : 0;
    }

    uint32_t KeRaiseIrqlToDpcLevel()
    {
        uint8_t& irql = CurrentIrql();
        const uint8_t previous = irql;
        irql = kDispatchLevel;
        return previous;
    }

    void KfLowerIrql(uint32_t newIrql)
    {
        CurrentIrql() = uint8_t(newIrql);
    }

    // Critical regions only disable APCs, which aren't delivered here.
    void KeEnterCriticalRegion() {}
    void KeLeaveCriticalRegion() {}

    // L2 cache locking has no host equivalent.
    uint32_t KeLockL2(uint32_t /*mode*/) { return 0; }
    void KeUnlockL2(uint32_t /*mode*/) {}

    // ---------------------------------------------------------------------------------------------
    // TLS. Slot allocation is process-wide; values are per guest thread.

    std::mutex g_tlsLock;
    std::bitset<kTlsSlotCount> g_tlsSlots;

    std::array<uint32_t, kTlsSlotCount>& CurrentTls()
    {
        static thread_local std::array<uint32_t, kTlsSlotCount> hostTls{};
        GuestThread* thread = CurrentThread();
        return thread ? thread->tls : hostTls;
    }

    uint32_t KeTlsAlloc()
    {
        std::lock_guard lock(g_tlsLock);
        for (uint32_t i = 0; i < kTlsSlotCount; i++)
        {
            if (!g_tlsSlots[i])
            {
                g_tlsSlots[i] = true;
                return i;
            }
        }
        return 0xFFFFFFFF; // TLS_OUT_OF_INDEXES
    }

    uint32_t KeTlsFree(uint32_t index)
    {
        std::lock_guard lock(g_tlsLock);
        if (index >= kTlsSlotCount || !g_tlsSlots[index])
            return 0;
        g_tlsSlots[index] = false;
        return 1;
    }

    uint32_t KeTlsGetValue(uint32_t index)
    {
        return index < kTlsSlotCount ? CurrentTls()[index] : 0;
    }

    uint32_t KeTlsSetValue(uint32_t index, uint32_t value)
    {
        if (index >= kTlsSlotCount)
            return 0;
        CurrentTls()[index] = value;
        return 1;
    }

    // ---------------------------------------------------------------------------------------------
    // Handles and object references.

    uint32_t NtClose(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle || handle == kCurrentProcessHandle)
            return status::kSuccess;
        return kernel::CloseHandle(handle) ? status::kSuccess : status::kInvalidHandle;
    }

    uint32_t NtDuplicateObject(uint32_t sourceHandle, be<uint32_t>* targetHandle, uint32_t options)
    {
        auto object = LookupHandle(sourceHandle);
        if (!object)
            return status::kInvalidHandle;
        if (targetHandle)
            *targetHandle = CreateHandle(object);
        constexpr uint32_t kDuplicateCloseSource = 0x1;
        if (options & kDuplicateCloseSource)
            kernel::CloseHandle(sourceHandle);
        return status::kSuccess;
    }

    // References handed out by ObReferenceObjectByHandle, released by ObDereferenceObject.
    std::mutex g_referenceLock;
    std::unordered_multimap<uint32_t, std::shared_ptr<KernelObject>> g_references;

    uint32_t ObReferenceObjectByHandle(uint32_t handle, uint32_t /*objectType*/, be<uint32_t>* object)
    {
        const uint32_t guestObject = kernel::ReferenceObject(handle);
        if (!guestObject)
            return status::kInvalidHandle;
        if (object)
            *object = guestObject;
        return status::kSuccess;
    }

    void ObDereferenceObject(uint32_t object)
    {
        std::shared_ptr<KernelObject> released;
        std::lock_guard lock(g_referenceLock);
        const auto it = g_references.find(object);
        if (it != g_references.end())
        {
            released = std::move(it->second);
            g_references.erase(it);
        }
    }
}

namespace kernel
{
    uint32_t ReferenceObject(uint32_t handle)
    {
        auto target = LookupHandle(handle);
        if (!target || !target->GuestObject())
            return 0;
        std::lock_guard lock(g_referenceLock);
        g_references.emplace(target->GuestObject(), target);
        return target->GuestObject();
    }
}

GUEST_EXPORT(NtCreateEvent, NtCreateEvent)
GUEST_EXPORT(NtSetEvent, NtSetEvent)
GUEST_EXPORT(NtPulseEvent, NtPulseEvent)
GUEST_EXPORT(NtClearEvent, NtClearEvent)
GUEST_EXPORT(KeSetEvent, KeSetEvent)
GUEST_EXPORT(KeResetEvent, KeResetEvent)
GUEST_EXPORT(KeInitializeSemaphore, KeInitializeSemaphore)
GUEST_EXPORT(KeReleaseSemaphore, KeReleaseSemaphore)
GUEST_EXPORT(NtCreateSemaphore, NtCreateSemaphore)
GUEST_EXPORT(NtReleaseSemaphore, NtReleaseSemaphore)
GUEST_EXPORT(NtCreateTimer, NtCreateTimer)
GUEST_EXPORT(NtSetTimerEx, NtSetTimerEx)
GUEST_EXPORT(NtCancelTimer, NtCancelTimer)
GUEST_EXPORT(KeWaitForSingleObject, KeWaitForSingleObject)
GUEST_EXPORT(KeWaitForMultipleObjects, KeWaitForMultipleObjects)
GUEST_EXPORT(NtWaitForSingleObjectEx, NtWaitForSingleObjectEx)
GUEST_EXPORT(NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx)
GUEST_EXPORT(NtSignalAndWaitForSingleObjectEx, NtSignalAndWaitForSingleObjectEx)
GUEST_EXPORT(RtlInitializeCriticalSection, RtlInitializeCriticalSection)
GUEST_EXPORT(RtlEnterCriticalSection, RtlEnterCriticalSection)
GUEST_EXPORT(RtlTryEnterCriticalSection, RtlTryEnterCriticalSection)
GUEST_EXPORT(RtlLeaveCriticalSection, RtlLeaveCriticalSection)
GUEST_EXPORT(KfAcquireSpinLock, KfAcquireSpinLock)
GUEST_EXPORT(KfReleaseSpinLock, KfReleaseSpinLock)
GUEST_EXPORT(KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrql)
GUEST_EXPORT(KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrql)
GUEST_EXPORT(KeTryToAcquireSpinLockAtRaisedIrql, KeTryToAcquireSpinLockAtRaisedIrql)
GUEST_EXPORT(KeRaiseIrqlToDpcLevel, KeRaiseIrqlToDpcLevel)
GUEST_EXPORT(KfLowerIrql, KfLowerIrql)
GUEST_EXPORT(KeEnterCriticalRegion, KeEnterCriticalRegion)
GUEST_EXPORT(KeLeaveCriticalRegion, KeLeaveCriticalRegion)
GUEST_EXPORT(KeLockL2, KeLockL2)
GUEST_EXPORT(KeUnlockL2, KeUnlockL2)
GUEST_EXPORT(KeTlsAlloc, KeTlsAlloc)
GUEST_EXPORT(KeTlsFree, KeTlsFree)
GUEST_EXPORT(KeTlsGetValue, KeTlsGetValue)
GUEST_EXPORT(KeTlsSetValue, KeTlsSetValue)
GUEST_EXPORT(NtClose, NtClose)
GUEST_EXPORT(NtDuplicateObject, NtDuplicateObject)
GUEST_EXPORT(ObReferenceObjectByHandle, ObReferenceObjectByHandle)
GUEST_EXPORT(ObDereferenceObject, ObDereferenceObject)
