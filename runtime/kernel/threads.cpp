#include "kernel/threads.h"

#include "guest_abi.h"
#include "guest_memory.h"
#include "kernel/module.h"
#include "kernel/status.h"
#include "log.h"
#include "util/profiler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>

#include <windows.h>

namespace kernel
{
    namespace
    {
        // KPCR fields the game reads through r13.
        constexpr uint32_t kPcrSize = 0x2D8;
        constexpr uint32_t kPcrSelf = 0x30;
        constexpr uint32_t kPcrStackBase = 0x70; // highest address
        constexpr uint32_t kPcrStackLimit = 0x74; // lowest address
        constexpr uint32_t kPcrCurrentThread = 0x100;
        constexpr uint32_t kPcrCurrentCpu = 0x10C;

        // KTHREAD fields (see ANALYSIS.md for which ones the game uses).
        constexpr uint32_t kKThreadSize = 0xAB0;
        constexpr uint32_t kKThreadStackBase = 0x5C;
        constexpr uint32_t kKThreadStackLimit = 0x60;
        constexpr uint32_t kKThreadApcListHeads = 0x74; // two LIST_ENTRYs (KAPC_STATE)
        constexpr uint32_t kKThreadProcess = 0x84;      // KAPC_STATE.Process; XAPI writes the quantum through it
        constexpr uint32_t kKThreadThreadId = 0x14C;
        constexpr uint32_t kKThreadStartAddress = 0x150;
        constexpr uint32_t kKThreadLastError = 0x160;
        constexpr uint32_t kKThreadCreationFlags = 0x16C;

        constexpr uint32_t kXexHeaderDefaultStackSize = 0x00020200;
        constexpr uint32_t kHostStackSize = 16 * 1024 * 1024;
        constexpr uint32_t kCpuCount = 6;

        thread_local GuestThread* t_currentThread = nullptr;
        std::atomic<uint32_t> g_nextThreadId{ 1 };

        struct GuestThreadExit
        {
            uint32_t exitCode;
        };

        void Store16(uint32_t address, uint16_t value)
        {
            *guest::Translate<be<uint16_t>>(address) = value;
        }

        void InitListHead(uint32_t address)
        {
            guest::StoreU32(address, address);
            guest::StoreU32(address + 4, address);
        }

        // The title's KPROCESS (X_KPROCESS layout): every KTHREAD points at it.
        constexpr uint32_t kKProcessSize = 0x60;
        constexpr uint32_t kKProcessThreadList = 0x04;
        constexpr uint32_t kKProcessQuantum = 0x0C;
        constexpr uint32_t kKProcessType = 0x2F;
        constexpr uint32_t kDefaultQuantum = 60;

        uint32_t TitleProcess()
        {
            static const uint32_t process = [] {
                const uint32_t address = guest::KernelAlloc(kKProcessSize);
                InitListHead(address + kKProcessThreadList);
                guest::StoreU32(address + kKProcessQuantum, kDefaultQuantum);
                *guest::Translate<uint8_t>(address + kKProcessType) = 1; // title process
                return address;
            }();
            return process;
        }
    }

    uint32_t AllocateThreadStack(uint32_t requestedSize, uint32_t& actualSize)
    {
        uint32_t size = requestedSize;
        if (size == 0)
            size = kernel::GetXexOptionalHeader(kernel::ExecutableXexHeader(), kXexHeaderDefaultStackSize);
        if (size == 0)
            size = 0x40000;
        actualSize = (size + 0xFFFF) & ~0xFFFFu;
        return guest::AllocStack(actualSize);
    }

    namespace
    {
        std::mutex& ThreadRegistryLock()
        {
            static std::mutex lock;
            return lock;
        }

        std::vector<GuestThread*>& ThreadRegistry()
        {
            static std::vector<GuestThread*> threads;
            return threads;
        }

        // The threads created so far by start address and context (GuestThread::Instance), under the registry's lock.
        std::map<std::pair<uint32_t, uint32_t>, uint32_t>& ThreadsByStart()
        {
            static std::map<std::pair<uint32_t, uint32_t>, uint32_t> counts;
            return counts;
        }
    }

    void ForEachThread(const std::function<void(GuestThread&)>& callback)
    {
        std::lock_guard lock(ThreadRegistryLock());
        for (GuestThread* thread : ThreadRegistry())
            callback(*thread);
    }

    namespace
    {
        // A guest thread by its KTHREAD, for the thread dumps: "thread <id>", or the address. (ThreadRegistryLock held: found
        // in the registry, not the object map, whose reference could outlast every other and destroy the thread here, its
        // destructor taking that lock.)
        std::string ThreadName(uint32_t kthread)
        {
            char text[32];
            const std::vector<GuestThread*>& threads = ThreadRegistry();
            if (const auto it = std::find_if(threads.begin(), threads.end(), [kthread](const GuestThread* t) { return t->KThread() == kthread; });
                it != threads.end())
                snprintf(text, sizeof(text), "thread %u", (*it)->ThreadId());
            else if (kthread & 0x80000000u && kthread < 0x90000000u)
                snprintf(text, sizeof(text), "host thread %u", kthread & 0x7FFFFFFFu); // RtlEnterCriticalSection's OwnerId
            else
                snprintf(text, sizeof(text), "KTHREAD 0x%08X", kthread);
            return text;
        }

        // A dispatcher object for the thread dumps: its kind, address and state.
        std::string DescribeObject(uint32_t address)
        {
            char text[96];
            if (!address || !guest::IsAllocated(address))
            {
                snprintf(text, sizeof(text), "object 0x%08X", address);
                return text;
            }
            const auto* header = guest::Translate<DispatcherHeader>(address);
            const int32_t state = header->signalState;
            switch (DispatcherType(header->type))
            {
            case DispatcherType::NotificationEvent:
            case DispatcherType::SynchronizationEvent:
                snprintf(text, sizeof(text), "%s event 0x%08X (%s)", header->type ? "synchronization" : "notification", address,
                    state ? "set" : "not set");
                break;
            case DispatcherType::Semaphore:
                snprintf(text, sizeof(text), "semaphore 0x%08X (count %d)", address, state);
                break;
            case DispatcherType::Thread:
                snprintf(text, sizeof(text), "the end of %s", ThreadName(address).c_str());
                break;
            case DispatcherType::NotificationTimer:
            case DispatcherType::SynchronizationTimer:
                snprintf(text, sizeof(text), "timer 0x%08X (%s)", address, state ? "signaled" : "not signaled");
                break;
            default:
                snprintf(text, sizeof(text), "object 0x%08X (type %u, state %d)", address, header->type, state);
                break;
            }
            return text;
        }

        // What a guest thread waits for (GuestThread::waitKind), for the thread dumps.
        std::string DescribeWait(const GuestThread& thread)
        {
            const GuestThread::WaitKind kind = thread.waitKind.load(std::memory_order_acquire);
            const uint32_t object = thread.waitObjects[0].load(std::memory_order_relaxed), count = thread.waitCount.load(std::memory_order_relaxed);
            char text[160];
            switch (kind)
            {
            case GuestThread::WaitKind::kAny:
            case GuestThread::WaitKind::kAll:
                if (count > 1)
                {
                    // Each object (the first kWaitObjects): in a wait for all, those set are the ones it doesn't wait for.
                    snprintf(text, sizeof(text), "waits for %s %u objects: ", kind == GuestThread::WaitKind::kAll ? "all of" : "one of", count);
                    std::string description = text;
                    for (uint32_t i = 0; i < std::min<uint32_t>(count, GuestThread::kWaitObjects); i++)
                        description += (i ? ", " : "") + DescribeObject(thread.waitObjects[i].load(std::memory_order_relaxed));
                    if (count > GuestThread::kWaitObjects)
                    {
                        snprintf(text, sizeof(text), " and %u more", count - uint32_t(GuestThread::kWaitObjects));
                        description += text;
                    }
                    return description;
                }
                snprintf(text, sizeof(text), "waits for %s", DescribeObject(object).c_str());
                return text;
            case GuestThread::WaitKind::kCriticalSection:
            {
                // RTL_CRITICAL_SECTION: its event's header, the lock count (-1 free), the recursion count, the owner's KTHREAD.
                const uint32_t owner = guest::IsAllocated(object) ? guest::LoadU32(object + 0x18) : 0;
                const int32_t lockCount = guest::IsAllocated(object) ? int32_t(guest::LoadU32(object + 0x10)) : 0;
                snprintf(text, sizeof(text), "waits for critical section 0x%08X, held by %s (lock count %d)", object,
                    owner ? ThreadName(owner).c_str() : "nobody", lockCount);
                return text;
            }
            case GuestThread::WaitKind::kDelay:
                snprintf(text, sizeof(text), "sleeps (%u ms)", count);
                return text;
            case GuestThread::WaitKind::kSuspended:
                return "suspended by itself";
            default:
                return thread.waits.load(std::memory_order_relaxed) ? "waiting" : "not waiting";
            }
        }
    }

    void DumpGuestThreads()
    {
        // The host thread's suspend count (NtQueryInformationThread's ThreadSuspendCount, Windows 8.1 on) beside the
        // guest's: the two must agree, or the thread can't be resumed as the guest asks.
        using QueryThreadFunction = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        static const auto queryThread =
            reinterpret_cast<QueryThreadFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        constexpr ULONG kThreadSuspendCount = 35;
        std::lock_guard lock(ThreadRegistryLock());
        for (GuestThread* thread : ThreadRegistry())
        {
            const PPCContext* ctx = thread->Context();
            if (!ctx)
                continue;
            ULONG hostSuspends = 0;
            const bool hostKnown = queryThread && thread->NativeHandle() &&
                queryThread(thread->NativeHandle(), kThreadSuspendCount, &hostSuspends, sizeof(hostSuspends), nullptr) >= 0;
            char host[16] = "?";
            if (hostKnown)
                snprintf(host, sizeof(host), "%lu", hostSuspends);
            fprintf(stderr, "[threads] thread %u (start 0x%08X): lr=%08X r1=%08X r3=%08X r4=%08X; suspend count %u (host %s); %s\n",
                thread->ThreadId(), guest::LoadU32(thread->KThread() + kKThreadStartAddress), uint32_t(ctx->lr), ctx->r1.u32, ctx->r3.u32,
                ctx->r4.u32, thread->SuspendCount(), host, DescribeWait(*thread).c_str());
            guest::PrintBacktrace(ctx->r1.u32, 8);
        }
        fflush(stderr);
    }

    uint32_t GuestThread::SuspendCount() const
    {
        // Without the lock: the dumps run when threads may be stuck, one holding it perhaps.
        return m_suspendCount.load(std::memory_order_relaxed);
    }

        std::shared_ptr<GuestThread> GuestThread::Create(const Params& params)
    {
        std::shared_ptr<GuestThread> thread(new GuestThread());
        thread->m_params = params;
        thread->m_threadId = g_nextThreadId++;
        thread->m_stack = AllocateThreadStack(params.stackSize, thread->m_stackSize);
        thread->m_pcr = guest::KernelAlloc(kPcrSize);
        thread->m_kthread = guest::KernelAlloc(kKThreadSize);
        if (!thread->m_stack || !thread->m_pcr || !thread->m_kthread)
        {
            LOG_ERROR("Out of guest memory creating a thread");
            return nullptr;
        }

        const uint32_t stackTop = thread->m_stack + thread->m_stackSize;
        const uint32_t pcr = thread->m_pcr;
        const uint32_t kthread = thread->m_kthread;

        guest::StoreU32(pcr + kPcrSelf, pcr);
        guest::StoreU32(pcr + kPcrStackBase, stackTop);
        guest::StoreU32(pcr + kPcrStackLimit, thread->m_stack);
        guest::StoreU32(pcr + kPcrCurrentThread, kthread);
        thread->SetAffinity(1u << ((thread->m_threadId - 1) % kCpuCount));

        auto* header = guest::Translate<DispatcherHeader>(kthread);
        header->type = uint8_t(DispatcherType::Thread);
        header->size = uint8_t(kKThreadSize / 4 > 0xFF ? 0xFF : kKThreadSize / 4);
        // Self-referencing list heads, as the 360 kernel initializes them.
        guest::StoreU32(kthread + 0x08, kthread + 0x08);
        guest::StoreU32(kthread + 0x0C, kthread + 0x08);
        guest::StoreU32(kthread + 0x10, kthread + 0x10);
        guest::StoreU32(kthread + 0x14, kthread + 0x10);
        guest::StoreU32(kthread + 0x40, kthread + 0x20);
        guest::StoreU32(kthread + 0x44, kthread + 0x20);
        guest::StoreU32(kthread + 0x48, kthread);
        guest::StoreU32(kthread + 0x4C, kthread + 0x18);
        Store16(kthread + 0x54, 16);
        Store16(kthread + 0x56, 0x102);
        guest::StoreU32(kthread + kKThreadStackBase, stackTop);
        guest::StoreU32(kthread + kKThreadStackLimit, thread->m_stack);
        InitListHead(kthread + kKThreadApcListHeads);
        InitListHead(kthread + kKThreadApcListHeads + 8);
        guest::StoreU32(kthread + kKThreadProcess, TitleProcess());
        guest::StoreU32(kthread + kKThreadThreadId, thread->m_threadId);
        guest::StoreU32(kthread + kKThreadStartAddress, params.startAddress);
        guest::StoreU32(kthread + kKThreadLastError, 0);
        guest::StoreU32(kthread + kKThreadCreationFlags, params.creationFlags);

        thread->m_suspendCount = (params.creationFlags & kCreateSuspended) ? 1 : 0;
        thread->m_self = thread;
        RegisterGuestObject(thread);
        {
            std::lock_guard lock(ThreadRegistryLock());
            ThreadRegistry().push_back(thread.get());
            thread->m_instance = ThreadsByStart()[{ params.startAddress, params.startContext }]++;
        }

        DWORD hostId = 0;
        thread->m_hostThread = CreateThread(nullptr, kHostStackSize, &GuestThread::HostEntry, thread.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, &hostId);
        if (!thread->m_hostThread)
        {
            LOG_ERROR("CreateThread failed (%lu)", GetLastError());
            thread->m_self.reset();
            return nullptr;
        }

        LOG_TRACE("Thread %u created: start 0x%08X context 0x%08X startup 0x%08X stack 0x%08X-0x%08X flags 0x%X",
            thread->m_threadId, params.startAddress, params.startContext, params.xapiStartup, thread->m_stack, stackTop, params.creationFlags);
        return thread;
    }

    GuestThread::~GuestThread()
    {
        {
            std::lock_guard lock(ThreadRegistryLock());
            auto& threads = ThreadRegistry();
            threads.erase(std::remove(threads.begin(), threads.end(), this), threads.end());
        }
        UnregisterGuestObject(m_kthread);
        if (m_hostThread)
            ::CloseHandle(m_hostThread);
        // The guest structures are freed only when nothing can reference them: a waiter or a
        // KTHREAD pointer held by the game may outlive the handle, so they are intentionally leaked.
    }

    void GuestThread::SetAffinity(uint32_t affinity)
    {
        m_affinity = affinity;
        uint8_t cpu = 0;
        while (cpu < kCpuCount - 1 && !(affinity & (1u << cpu)))
            cpu++;
        *guest::Translate<uint8_t>(m_pcr + kPcrCurrentCpu) = cpu;
    }

    unsigned long __stdcall GuestThread::HostEntry(void* parameter)
    {
        static_cast<GuestThread*>(parameter)->Run();
        return 0;
    }

    void GuestThread::Run()
    {
        t_currentThread = this;
        if (m_params.hostEntry)
            profile::NameThread("Guest %u (runtime service)", m_threadId);
        else
            profile::NameThread("Guest %u (0x%08X)", m_threadId, m_params.startAddress);
        {
            std::unique_lock lock(m_lock);
            m_resumed.wait(lock, [this] { return m_suspendCount == 0; });
            m_started = true;
        }

        auto context = std::make_unique<PPCContext>();
        m_context = context.get();
        // The FPSCR caches MXCSR; zero would unmask every float exception the first time the
        // generated code sets the flush mode.
        context->fpscr.loadFromHost();
        // 16-byte aligned stack pointer below the top, with a zero back chain.
        const uint32_t stackPointer = ((m_stack + m_stackSize) - 0x100) & ~0xFu;
        guest::StoreU32(stackPointer, 0);
        context->r1.u64 = stackPointer;
        context->r13.u64 = m_pcr;

        uint32_t exitCode = 0;
        try
        {
            if (m_params.hostEntry)
            {
                m_params.hostEntry(*context);
                m_context = nullptr;
                Finish(0);
                t_currentThread = nullptr;
                return;
            }

            uint32_t target;
            if (m_params.xapiStartup)
            {
                context->r3.u64 = m_params.startAddress;
                context->r4.u64 = m_params.startContext;
                target = m_params.xapiStartup;
            }
            else
            {
                context->r3.u64 = m_params.startContext;
                target = m_params.startAddress;
            }

            PPCFunc* function = PPC_LOOKUP_FUNC(guest::g_base, target);
            if (!function)
            {
                LOG_ERROR("Thread %u: start address 0x%08X isn't a known function", m_threadId, target);
                std::abort();
            }
            function(*context, guest::g_base);
            exitCode = context->r3.u32;
        }
        catch (const GuestThreadExit& exit)
        {
            exitCode = exit.exitCode;
        }

        m_context = nullptr;
        Finish(exitCode);
        t_currentThread = nullptr;
    }

    void GuestThread::Finish(uint32_t exitCode)
    {
        LOG_TRACE("Thread %u exited with 0x%08X", m_threadId, exitCode);
        m_exitCode = exitCode;
        SignalHeader(m_kthread, 1);
        // Drop the self-reference last; the object may be destroyed here if no handles remain.
        std::shared_ptr<GuestThread> self = std::move(m_self);
    }

    // The suspend count, as the 360's kernel keeps it: the thread runs only at 0; a resume at 0 does nothing (the 360
    // doesn't bank it). Its changes and what they do happen under m_lock, so a suspend and a resume that come together are
    // taken in some order, each in full.
    uint32_t GuestThread::Resume()
    {
        std::lock_guard lock(m_lock);
        const uint32_t previous = m_suspendCount;
        if (m_suspendCount > 0 && --m_suspendCount == 0)
        {
            // Stopped by another thread: the host thread goes on. Suspended by itself, or not started: it wakes from its
            // wait in Suspend or Run.
            if (m_hostSuspended)
            {
                ResumeThread(m_hostThread);
                m_hostSuspended = false;
            }
            m_resumed.notify_all();
        }
        return previous;
    }

    uint32_t GuestThread::Suspend()
    {
        std::unique_lock lock(m_lock);
        const uint32_t previous = m_suspendCount++;
        if (t_currentThread == this)
        {
            // The thread suspends itself, as CRI's server thread does at the end of each pass, its vsync thread resuming it
            // each vsync: it waits until resumed, the count checked and the wait begun under the lock Resume takes. (Its
            // host thread used to suspend itself after the lock was released, and a resume between the two found the
            // count at 1 and resumed a host thread that wasn't suspended yet: the count back at 0 with the thread
            // stopped for good, which no later resume would start, since none found it suspended. CRI's server stopped
            // so, which froze the boot's movie: ANALYSIS.md, Phase 6, The two rare freezes.)
            waits++;
            waitKind.store(WaitKind::kSuspended, std::memory_order_release);
            m_resumed.wait(lock, [this] { return m_suspendCount == 0; });
            waitKind.store(WaitKind::kNone, std::memory_order_release);
            waits--;
        }
        else if (previous == 0 && m_started)
        {
            // Another thread, running: stopped on the host, under the lock, so that Resume finds it so. One that hasn't
            // started stays in its wait in Run.
            SuspendThread(m_hostThread);
            m_hostSuspended = true;
        }
        return previous;
    }

    void GuestThread::Join()
    {
        WaitForSingleObject(m_hostThread, INFINITE);
    }

    GuestThread* CurrentThread()
    {
        return t_currentThread;
    }

    uint32_t CurrentKThread()
    {
        return t_currentThread ? t_currentThread->KThread() : 0;
    }

    std::shared_ptr<KernelObject> CurrentThreadObject()
    {
        return t_currentThread ? t_currentThread->shared_from_this() : nullptr;
    }

    void ExitCurrentThread(uint32_t exitCode)
    {
        throw GuestThreadExit{ exitCode };
    }

    void CallGuest(PPCContext& context, uint32_t function, std::initializer_list<uint64_t> arguments)
    {
        PPCRegister* registers[] = { &context.r3, &context.r4, &context.r5, &context.r6, &context.r7, &context.r8, &context.r9, &context.r10 };
        size_t i = 0;
        for (uint64_t argument : arguments)
        {
            if (i < 8)
                registers[i++]->u64 = argument;
        }
        PPCFunc* target = PPC_LOOKUP_FUNC(guest::g_base, function);
        if (!target)
        {
            LOG_ERROR("CallGuest: 0x%08X isn't a known function", function);
            std::abort();
        }
        target(context, guest::g_base);
    }

    GuestCallQueue::GuestCallQueue(const char* name, std::function<void()> afterCall) : m_name(name), m_afterCall(std::move(afterCall))
    {
        GuestThread::Params params;
        params.stackSize = 0x10000;
        params.hostEntry = [this](PPCContext& context) { Run(context); };
        m_thread = GuestThread::Create(params);
    }

    GuestCallQueue::~GuestCallQueue()
    {
        {
            std::lock_guard lock(m_lock);
            m_stopping = true;
        }
        m_ready.notify_all();
        m_done.notify_all();
        if (m_thread)
            m_thread->Join();
    }

    uint64_t GuestCallQueue::Post(uint32_t function, std::initializer_list<uint64_t> arguments, int cpu)
    {
        Call call{ function, {}, 0, cpu };
        for (uint64_t argument : arguments)
        {
            if (call.count < call.arguments.size())
                call.arguments[call.count++] = argument;
        }
        uint64_t ticket;
        {
            std::lock_guard lock(m_lock);
            m_calls.push_back(call);
            ticket = ++m_posted;
        }
        m_ready.notify_one();
        return ticket;
    }

    void GuestCallQueue::Wait(uint64_t ticket)
    {
        std::unique_lock lock(m_lock);
        m_done.wait(lock, [&] { return m_completed >= ticket || m_stopping; });
    }

    void GuestCallQueue::Run(PPCContext& context)
    {
        LOG_TRACE("%s: guest call queue running on thread %u", m_name, CurrentThread()->ThreadId());
        profile::NameThread("Guest %u (%s)", CurrentThread()->ThreadId(), m_name);
        const uint64_t stackPointer = context.r1.u64;
        for (;;)
        {
            std::vector<Call> calls;
            {
                std::unique_lock lock(m_lock);
                m_ready.wait(lock, [this] { return m_stopping || !m_calls.empty(); });
                if (m_stopping && m_calls.empty())
                    return;
                calls.swap(m_calls);
            }
            for (const Call& call : calls)
            {
                context.r1.u64 = stackPointer; // each call starts on a clean stack
                if (call.cpu >= 0)
                    CurrentThread()->SetAffinity(1u << call.cpu);
                PPCRegister* registers[] = { &context.r3, &context.r4, &context.r5, &context.r6, &context.r7, &context.r8, &context.r9, &context.r10 };
                for (size_t i = 0; i < call.count; i++)
                    registers[i]->u64 = call.arguments[i];
                PPCFunc* target = PPC_LOOKUP_FUNC(guest::g_base, call.function);
                if (target)
                    target(context, guest::g_base);
                else
                    LOG_ERROR("%s: 0x%08X isn't a known function", m_name, call.function);
                {
                    std::lock_guard lock(m_lock);
                    m_completed++;
                }
                m_done.notify_all();
                if (m_afterCall)
                    m_afterCall();
            }
        }
    }
}
