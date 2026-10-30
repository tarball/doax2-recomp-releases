// Thread exports: creation, termination, suspension, scheduling hints, delays and stacks.
#include "guest_abi.h"
#include "kernel/objects.h"
#include "kernel/status.h"
#include "kernel/threads.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <windows.h>

namespace
{
    using namespace kernel;

    std::shared_ptr<GuestThread> ThreadFromKThread(uint32_t kthread)
    {
        return std::dynamic_pointer_cast<GuestThread>(LookupGuestObject(kthread));
    }

    // ExCreateThread(&handle, stackSize, &threadId, xapiStartup, start, context, flags).
    // The flags' high byte is an affinity mask (1 << cpu) chosen by XAPI.
    uint32_t ExCreateThread(be<uint32_t>* handle, uint32_t stackSize, be<uint32_t>* threadId, uint32_t xapiStartup,
        uint32_t startAddress, uint32_t startContext, uint32_t creationFlags)
    {
        GuestThread::Params params;
        params.stackSize = stackSize;
        params.startAddress = startAddress;
        params.startContext = startContext;
        params.xapiStartup = xapiStartup;
        params.creationFlags = creationFlags;

        // Start suspended so the affinity can be applied before the thread runs.
        const bool suspended = creationFlags & kCreateSuspended;
        params.creationFlags |= kCreateSuspended;
        auto thread = GuestThread::Create(params);
        if (!thread)
            return status::kNoMemory;

        if (const uint32_t affinity = creationFlags >> 24)
            thread->SetAffinity(affinity);
        if (handle)
            *handle = CreateHandle(thread);
        if (threadId)
            *threadId = thread->ThreadId();
        if (!suspended)
            thread->Resume();
        return status::kSuccess;
    }

    [[noreturn]] void ExTerminateThread(uint32_t exitCode)
    {
        ExitCurrentThread(exitCode);
    }

    uint32_t NtResumeThread(uint32_t handle, be<uint32_t>* previousCount)
    {
        auto thread = LookupHandleAs<GuestThread>(handle);
        if (!thread)
            return status::kInvalidHandle;
        const uint32_t previous = thread->Resume();
        if (previousCount)
            *previousCount = previous;
        return status::kSuccess;
    }

    // Returns the previous suspend count.
    uint32_t KeResumeThread(uint32_t kthread)
    {
        auto thread = ThreadFromKThread(kthread);
        return thread ? thread->Resume() : 0;
    }

    uint32_t NtSuspendThread(uint32_t handle, be<uint32_t>* previousCount)
    {
        auto thread = LookupHandleAs<GuestThread>(handle);
        if (!thread)
            return status::kInvalidHandle;
        const uint32_t previous = thread->Suspend();
        if (previousCount)
            *previousCount = previous;
        return status::kSuccess;
    }

    // Unlike NT, the 360 returns the previous affinity through a pointer and returns a status.
    uint32_t KeSetAffinityThread(uint32_t kthread, uint32_t affinity, be<uint32_t>* previousAffinity)
    {
        auto thread = ThreadFromKThread(kthread);
        if (!thread)
            return status::kInvalidParameter;
        if (previousAffinity)
            *previousAffinity = thread->Affinity();
        if (affinity)
            thread->SetAffinity(affinity);
        return status::kSuccess;
    }

    // Priorities are recorded but not applied to host threads.
    int32_t KeSetBasePriorityThread(uint32_t kthread, int32_t priority)
    {
        auto thread = ThreadFromKThread(kthread);
        if (!thread)
            return 0;
        const int32_t previous = thread->basePriority;
        thread->basePriority = priority;
        return previous;
    }

    int32_t KeQueryBasePriorityThread(uint32_t kthread)
    {
        auto thread = ThreadFromKThread(kthread);
        return thread ? thread->basePriority : 0;
    }

    uint32_t KeSetDisableBoostThread(uint32_t kthread, uint32_t disable)
    {
        auto thread = ThreadFromKThread(kthread);
        if (!thread)
            return 0;
        const uint32_t previous = thread->disableBoost;
        thread->disableBoost = disable;
        return previous;
    }

    // Interval: negative = relative 100 ns units, positive = absolute system time.
    uint32_t KeDelayExecutionThread(uint32_t /*processorMode*/, uint32_t /*alertable*/, const be<int64_t>* interval)
    {
        int64_t ticks = interval ? int64_t(*interval) : 0;
        if (ticks > 0)
        {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ticks = int64_t((uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime) - ticks; // negative = still in the future
        }

        if (ticks >= 0)
            SwitchToThread();
        else
        {
            GuestThread* thread = CurrentThread();
            if (thread)
            {
                thread->waits++;
                thread->waitObjects[0].store(0, std::memory_order_relaxed);
                thread->waitCount.store(uint32_t(std::min<int64_t>(-ticks / 10000, 0xFFFFFFFF)), std::memory_order_relaxed); // ms
                thread->waitKind.store(GuestThread::WaitKind::kDelay, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::nanoseconds(-ticks * 100));
            if (thread)
            {
                thread->waitKind.store(GuestThread::WaitKind::kNone, std::memory_order_release);
                thread->waits--;
            }
        }
        return status::kSuccess;
    }

    uint32_t NtYieldExecution()
    {
        constexpr uint32_t kNoYieldPerformed = 0x40000024;
        return SwitchToThread() ? status::kSuccess : kNoYieldPerformed;
    }

    // Used by fiber switching: updates the thread's stack bounds and moves r1 to the new stack.
    void KeSetCurrentStackPointers(uint32_t stackPointer, uint32_t kthread, uint32_t /*stackAllocBase*/, uint32_t stackBase, uint32_t stackLimit, PPCContext* context)
    {
        GuestThread* current = CurrentThread();
        if (kthread)
        {
            guest::StoreU32(kthread + 0x5C, stackBase);
            guest::StoreU32(kthread + 0x60, stackLimit);
        }
        if (current)
        {
            guest::StoreU32(current->Pcr() + 0x70, stackBase);
            guest::StoreU32(current->Pcr() + 0x74, stackLimit);
        }
        context->r1.u64 = stackPointer;
    }

    void KeEnableFpuExceptions(uint32_t /*enable*/)
    {
    }

    // Kernel stacks for fibers. Returns the top (highest address) of the new stack.
    uint32_t MmCreateKernelStack(uint32_t size, uint32_t /*unknown*/)
    {
        uint32_t actualSize = 0;
        const uint32_t stack = AllocateThreadStack(size, actualSize);
        return stack ? stack + actualSize : 0;
    }

    void MmDeleteKernelStack(uint32_t /*stackBase*/, uint32_t stackLimit)
    {
        std::lock_guard lock(guest::HeapLock());
        if (PageHeap* heap = guest::LookupHeap(stackLimit))
            heap->Release(stackLimit);
    }
}

GUEST_EXPORT(ExCreateThread, ExCreateThread)
GUEST_EXPORT(ExTerminateThread, ExTerminateThread)
GUEST_EXPORT(NtResumeThread, NtResumeThread)
GUEST_EXPORT(KeResumeThread, KeResumeThread)
GUEST_EXPORT(NtSuspendThread, NtSuspendThread)
GUEST_EXPORT(KeSetAffinityThread, KeSetAffinityThread)
GUEST_EXPORT(KeSetBasePriorityThread, KeSetBasePriorityThread)
GUEST_EXPORT(KeQueryBasePriorityThread, KeQueryBasePriorityThread)
GUEST_EXPORT(KeSetDisableBoostThread, KeSetDisableBoostThread)
GUEST_EXPORT(KeDelayExecutionThread, KeDelayExecutionThread)
GUEST_EXPORT(NtYieldExecution, NtYieldExecution)
GUEST_EXPORT(KeSetCurrentStackPointers, KeSetCurrentStackPointers)
GUEST_EXPORT(KeEnableFpuExceptions, KeEnableFpuExceptions)
GUEST_EXPORT(MmCreateKernelStack, MmCreateKernelStack)
GUEST_EXPORT(MmDeleteKernelStack, MmDeleteKernelStack)
