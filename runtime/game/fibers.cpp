// Host fibers for XAPI fibers (DOAX2-specific addresses).
//
// XAPI's SwitchToFiber saves the caller's registers into its fiber record, loads the target's, and
// tail-calls KeSetCurrentStackPointers to move r1 and "return" into the target's saved LR. Recompiled
// code can't do that: a guest blr is a host return. Instead, each guest fiber runs on its own host
// fiber with its own PPCContext, and the switch is a host SwitchToFiber.
//
// XAPI fiber record (allocated by CreateFiber / ConvertThreadToFiber):
//   [0] parameter  [4] stack allocation base  [8] stack base (top)  [12] stack limit
//   [28] saved LR (0x82785660, the startup trampoline, for a fiber that hasn't run)
//   [48] saved r1 (64-bit)  [288] saved r31 (64-bit; the start address for a new fiber)
#include "guest_abi.h"
#include "kernel/threads.h"
#include "log.h"

#include <exception>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <windows.h>

extern "C" PPC_FUNC(__imp__sub_827831B0); // XAPI DeleteFiber

namespace
{
    constexpr uint32_t kPcrCurrentThread = 0x100;
    constexpr uint32_t kStartupTrampoline = 0x82785660; // a new fiber's saved LR
    constexpr uint32_t kStartFiber = 0x82783220;        // what the trampoline calls with the start routine: runs it, then exits
    constexpr uint32_t kPcrStackBase = 0x70;
    constexpr uint32_t kPcrStackLimit = 0x74;
    constexpr uint32_t kKThreadStackBase = 0x5C;
    constexpr uint32_t kKThreadStackLimit = 0x60;
    constexpr uint32_t kKThreadFiber = 0x164;
    constexpr SIZE_T kHostFiberStack = 16 * 1024 * 1024;

    struct HostFiber
    {
        void* fiber = nullptr;
        uint32_t record = 0;
        std::unique_ptr<PPCContext> context; // only for fibers started by the runtime
    };

    std::mutex g_lock;
    std::unordered_map<uint32_t, std::unique_ptr<HostFiber>> g_fibers; // guest record -> host fiber

    // Per host thread: its original (converted) fiber, and an exit to rethrow there.
    thread_local void* t_threadFiber = nullptr;
    thread_local std::exception_ptr t_pendingExit;

    uint64_t Load64(uint32_t address)
    {
        return *guest::Translate<be<uint64_t>>(address);
    }

    void* CurrentHostFiber()
    {
        if (!IsThreadAFiber())
            t_threadFiber = ConvertThreadToFiber(nullptr);
        return GetCurrentFiber();
    }

    void __stdcall FiberEntry(void* parameter)
    {
        auto* fiber = static_cast<HostFiber*>(parameter);
        try
        {
            // First run: the startup trampoline (the record's saved LR) passes the start routine, which XAPI keeps in the saved
            // r31, to kStartFiber. That's called directly, on the record's stack: recompiled code may keep r31 in a local
            // rather than the context.
            PPCContext& ctx = *fiber->context;
            ctx.r1.u64 = uint32_t(Load64(fiber->record + 48));
            if (const uint32_t start = guest::LoadU32(fiber->record + 28); start != kStartupTrampoline)
                LOG_ERROR("Fiber 0x%08X starts at %08X, not the startup trampoline", fiber->record, start);
            kernel::CallGuest(ctx, kStartFiber, { Load64(fiber->record + 288) });
            LOG_ERROR("Fiber 0x%08X returned from its start routine", fiber->record);
        }
        catch (...)
        {
            // The thread is exiting from this fiber: finish it on the thread's own fiber.
            t_pendingExit = std::current_exception();
        }
        SwitchToFiber(t_threadFiber);
    }

    HostFiber* FiberFor(uint32_t record, const PPCContext& creator)
    {
        std::lock_guard lock(g_lock);
        auto& entry = g_fibers[record];
        if (!entry)
        {
            entry = std::make_unique<HostFiber>();
            entry->record = record;
            entry->context = std::make_unique<PPCContext>(creator);
            entry->fiber = CreateFiberEx(kHostFiberStack, kHostFiberStack, FIBER_FLAG_FLOAT_SWITCH, FiberEntry, entry.get());
            if (!entry->fiber)
            {
                LOG_ERROR("CreateFiberEx failed (%lu)", GetLastError());
                std::abort();
            }
        }
        return entry.get();
    }

    void AdoptCurrentFiber(uint32_t record)
    {
        void* current = CurrentHostFiber();
        std::lock_guard lock(g_lock);
        auto& entry = g_fibers[record];
        if (!entry)
        {
            entry = std::make_unique<HostFiber>();
            entry->record = record;
            entry->fiber = current;
        }
    }
}

// XAPI SwitchToFiber's core (SwitchToFiber, 0x82783210, branches to it): r3 = target fiber record.
PPC_FUNC(sub_82785670)
{
    const uint32_t target = ctx.r3.u32;
    const uint32_t pcr = ctx.r13.u32;
    const uint32_t kthread = guest::LoadU32(pcr + kPcrCurrentThread);
    const uint32_t current = guest::LoadU32(kthread + kKThreadFiber);

    // The bookkeeping XAPI does around the switch: current fiber and stack bounds.
    guest::StoreU32(kthread + kKThreadFiber, target);
    guest::StoreU32(kthread + kKThreadStackBase, guest::LoadU32(target + 8));
    guest::StoreU32(kthread + kKThreadStackLimit, guest::LoadU32(target + 12));
    guest::StoreU32(pcr + kPcrStackBase, guest::LoadU32(target + 8));
    guest::StoreU32(pcr + kPcrStackLimit, guest::LoadU32(target + 12));

    if (current)
        AdoptCurrentFiber(current);
    HostFiber* next = FiberFor(target, ctx);
    SwitchToFiber(next->fiber);

    // Resumed by a switch back to this fiber.
    if (t_pendingExit && GetCurrentFiber() == t_threadFiber)
    {
        std::exception_ptr exit = std::exchange(t_pendingExit, nullptr);
        std::rethrow_exception(exit);
    }
    (void)base;
}

// XAPI DeleteFiber: r3 = fiber record. Frees the host fiber, then XAPI frees the stack and record.
PPC_FUNC(sub_827831B0)
{
    const uint32_t record = ctx.r3.u32;
    {
        std::lock_guard lock(g_lock);
        const auto it = g_fibers.find(record);
        if (it != g_fibers.end() && it->second->fiber != GetCurrentFiber())
        {
            if (it->second->context) // created by the runtime (not a converted thread)
                DeleteFiber(it->second->fiber);
            g_fibers.erase(it);
        }
    }
    __imp__sub_827831B0(ctx, base);
}
