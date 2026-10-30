#pragma once

// Guest threads: each has a guest stack, a PCR (pointed to by r13) and a KTHREAD in guest memory,
// and runs on its own host thread.
#include "kernel/objects.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <vector>

struct PPCContext;

namespace kernel
{
    inline constexpr uint32_t kCreateSuspended = 0x1;
    inline constexpr uint32_t kTlsSlotCount = 64;

    class GuestThread : public KernelObject
    {
    public:
        struct Params
        {
            uint32_t stackSize = 0;      // 0 = the XEX's default stack size
            uint32_t startAddress = 0;
            uint32_t startContext = 0;
            uint32_t xapiStartup = 0;    // if set, called as xapiStartup(startAddress, startContext)
            uint32_t creationFlags = 0;
            // If set, runs instead of a guest start address: a runtime service loop on a guest thread
            // (its own PCR/KTHREAD/stack), e.g. for delivering GPU interrupts to guest callbacks.
            std::function<void(PPCContext&)> hostEntry;
        };

        // Creates the thread. It runs once its suspend count is zero (immediately unless kCreateSuspended).
        static std::shared_ptr<GuestThread> Create(const Params& params);
        ~GuestThread() override;

        const char* TypeName() const override { return "Thread"; }
        uint32_t GuestObject() const override { return m_kthread; }

        uint32_t ThreadId() const { return m_threadId; }
        uint32_t StartAddress() const { return m_params.startAddress; } // 0 for a host entry
        uint32_t StartContext() const { return m_params.startContext; }
        // How many threads were created before this one with the same start address and context. With those two it
        // tells a thread apart the same way on every PC, which its ID, counting all threads, doesn't (netplay).
        uint32_t Instance() const { return m_instance; }
        uint32_t KThread() const { return m_kthread; }
        uint32_t Pcr() const { return m_pcr; }
        // The guest stack: its lowest address and size.
        uint32_t StackLow() const { return m_stack; }
        uint32_t StackSize() const { return m_stackSize; }
        uint32_t ExitCode() const { return m_exitCode; }
        PPCContext* Context() const { return m_context; }

        // Return the previous suspend count.
        uint32_t Resume();
        uint32_t Suspend();
        // The suspend count now (diagnostics).
        uint32_t SuspendCount() const;

        // Blocks the calling host thread until this thread has finished.
        void Join();
        // The host thread handle (signaled when the thread has finished).
        void* NativeHandle() const { return m_hostThread; }

        void SetAffinity(uint32_t affinity);
        uint32_t Affinity() const { return m_affinity; }
        int32_t basePriority = 0;
        uint32_t disableBoost = 0;

        // Blocking waits the thread is in (kernel waits and delays): netplay checks that the game's workers are idle at
        // a frame boundary.
        std::atomic<uint32_t> waits{ 0 };
        // What it waits for now, for the thread dumps (DumpGuestThreads: written by the thread, read racily): the kind of
        // wait, the objects' guest addresses (the first kWaitObjects of them; a critical section's address) and their
        // count (a sleep's milliseconds).
        static constexpr size_t kWaitObjects = 8;
        enum class WaitKind : uint8_t
        {
            kNone,
            kAny,             // one of the objects (a single object's wait too)
            kAll,             // all of them
            kCriticalSection, // its embedded event, the section held by another thread
            kDelay,           // a sleep
            kSuspended,       // suspended by itself, until another thread resumes it
        };
        std::atomic<WaitKind> waitKind{ WaitKind::kNone };
        std::array<std::atomic<uint32_t>, kWaitObjects> waitObjects{};
        std::atomic<uint32_t> waitCount{ 0 };

        // Per-thread kernel state used by exports.
        std::array<uint32_t, kTlsSlotCount> tls{};
        uint8_t irql = 0;

    private:
        GuestThread() = default;
        static unsigned long __stdcall HostEntry(void* parameter);
        void Run();
        void Finish(uint32_t exitCode);

        Params m_params;
        uint32_t m_threadId = 0;
        uint32_t m_instance = 0;
        uint32_t m_stack = 0;     // lowest address
        uint32_t m_stackSize = 0;
        uint32_t m_pcr = 0;
        uint32_t m_kthread = 0;
        uint32_t m_affinity = 0;
        uint32_t m_exitCode = 0;
        PPCContext* m_context = nullptr;
        void* m_hostThread = nullptr;

        std::mutex m_lock;
        std::condition_variable m_resumed; // the suspend count back at 0: for a thread not started, or suspended by itself
        std::atomic<uint32_t> m_suspendCount{ 0 }; // changed under m_lock; read without it by SuspendCount
        bool m_started = false;
        bool m_hostSuspended = false;      // the host thread stopped by another thread's Suspend (SuspendThread)
        std::shared_ptr<GuestThread> m_self; // keeps the object alive while the host thread runs
    };

    // The calling guest thread, or nullptr on a host-only thread.
    GuestThread* CurrentThread();

    // Calls `callback` for every guest thread, with the registry locked (no thread starts or ends meanwhile).
    void ForEachThread(const std::function<void(GuestThread&)>& callback);

    // Prints every guest thread's registers and call chain (diagnostics; the state is read racily).
    void DumpGuestThreads();
    // Guest address of the calling thread's KTHREAD (0 on a host-only thread).
    uint32_t CurrentKThread();

    // Ends the calling guest thread with `exitCode`, unwinding back to its entry point.
    [[noreturn]] void ExitCurrentThread(uint32_t exitCode);

    // Allocates a guest stack for a thread or fiber. Returns the lowest address, or 0.
    uint32_t AllocateThreadStack(uint32_t requestedSize, uint32_t& actualSize);

    // Calls a guest function on the calling guest thread's context with up to 8 integer arguments.
    void CallGuest(PPCContext& context, uint32_t function, std::initializer_list<uint64_t> arguments);

    // A guest thread that runs posted calls to guest functions in order (e.g. interrupt callbacks).
    class GuestCallQueue
    {
    public:
        // `afterCall`, if set, runs on the queue's thread after each call returns.
        explicit GuestCallQueue(const char* name, std::function<void()> afterCall = {});
        ~GuestCallQueue();
        // `cpu` >= 0 runs the call with that CPU as the thread's current processor (PCR+0x10C).
        // Returns a ticket for Wait.
        uint64_t Post(uint32_t function, std::initializer_list<uint64_t> arguments, int cpu = -1);
        // Blocks until the call with `ticket` (and every call posted before it) has returned.
        void Wait(uint64_t ticket);

    private:
        struct Call
        {
            uint32_t function;
            std::array<uint64_t, 8> arguments;
            size_t count;
            int cpu;
        };
        void Run(PPCContext& context);

        const char* m_name;
        std::function<void()> m_afterCall;
        std::mutex m_lock;
        std::condition_variable m_ready;
        std::condition_variable m_done;
        std::vector<Call> m_calls;
        uint64_t m_posted = 0, m_completed = 0;
        bool m_stopping = false;
        std::shared_ptr<GuestThread> m_thread;
    };
}
