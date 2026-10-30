#pragma once

// Kernel objects, handles and the dispatcher (waiting and signalling).
//
// Waitable state lives in the object's DISPATCHER_HEADER in guest memory, exactly where the guest
// expects it, so pointer-based calls (KeSetEvent(PKEVENT)) and handle-based calls (NtSetEvent(HANDLE))
// operate on the same state. All header changes happen under DispatcherLock(), followed by
// SatisfyWaiters(), which wakes waiters in FIFO order the way NT's dispatcher does.
#include "guest_abi.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace kernel
{
    enum class DispatcherType : uint8_t
    {
        NotificationEvent = 0,
        SynchronizationEvent = 1,
        Mutant = 2,
        Process = 3,
        Queue = 4,
        Semaphore = 5,
        Thread = 6,
        NotificationTimer = 8,
        SynchronizationTimer = 9,
    };

    struct DispatcherHeader
    {
        uint8_t type;
        uint8_t absolute;
        uint8_t size;
        uint8_t inserted;
        be<int32_t> signalState;
        be<uint32_t> waitListFlink;
        be<uint32_t> waitListBlink;
    };
    static_assert(sizeof(DispatcherHeader) == 16);

    class KernelObject : public std::enable_shared_from_this<KernelObject>
    {
    public:
        virtual ~KernelObject() = default;
        virtual const char* TypeName() const = 0;

        // Guest address of the object's body (starting with its dispatcher header if waitable), or 0.
        virtual uint32_t GuestObject() const { return 0; }
        virtual bool IsWaitable() const { return GuestObject() != 0; }
    };

    // A kernel object whose body (e.g. a KEVENT) was allocated in guest kernel memory by the runtime.
    class GuestBackedObject : public KernelObject
    {
    public:
        GuestBackedObject(uint32_t size);
        ~GuestBackedObject() override;
        uint32_t GuestObject() const override { return m_address; }
        DispatcherHeader* Header() const { return guest::Translate<DispatcherHeader>(m_address); }

    private:
        uint32_t m_address;
        uint32_t m_size;
    };

    // Handles. 0xFFFFFFFE is the pseudo-handle for the current thread.
    inline constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;
    inline constexpr uint32_t kCurrentProcessHandle = 0xFFFFFFFF;

    uint32_t CreateHandle(std::shared_ptr<KernelObject> object);
    std::shared_ptr<KernelObject> LookupHandle(uint32_t handle);
    // The calling guest thread's object (defined in threads.cpp); backs kCurrentThreadHandle.
    std::shared_ptr<KernelObject> CurrentThreadObject();
    bool CloseHandle(uint32_t handle);

    template <typename T>
    std::shared_ptr<T> LookupHandleAs(uint32_t handle)
    {
        return std::dynamic_pointer_cast<T>(LookupHandle(handle));
    }

    // Objects referenced by guest pointer (ObReferenceObjectByHandle hands out their GuestObject()).
    void RegisterGuestObject(const std::shared_ptr<KernelObject>& object);
    // Takes a reference to a handle's object, as ObReferenceObjectByHandle does (ObDereferenceObject releases it).
    // Returns its guest object, or 0 if the handle has none.
    uint32_t ReferenceObject(uint32_t handle);
    void UnregisterGuestObject(uint32_t guestObject);
    std::shared_ptr<KernelObject> LookupGuestObject(uint32_t guestObject);

    // Dispatcher.
    std::recursive_mutex& DispatcherLock();
    // Re-evaluates every waiter against current header states. Call after changing any state.
    void SatisfyWaiters();

    // Waits on guest dispatcher headers. Timeout follows NT conventions: null = infinite,
    // 0 = poll, negative = relative 100 ns units, positive = absolute system time.
    // Returns STATUS_WAIT_0 + index (wait any), STATUS_SUCCESS (wait all) or STATUS_TIMEOUT.
    // Must not be called with DispatcherLock() held (the wait releases only one lock level).
    uint32_t WaitForHeaders(std::span<const uint32_t> headers, bool waitAll, const be<int64_t>* timeout);

    // Event operations on a guest KEVENT; each returns the previous signal state.
    int32_t SetEvent(uint32_t event);
    int32_t ResetEvent(uint32_t event);
    int32_t PulseEvent(uint32_t event);

    // Sets a header's signal state (e.g. a thread exiting) and wakes waiters.
    void SignalHeader(uint32_t header, int32_t state);
}
