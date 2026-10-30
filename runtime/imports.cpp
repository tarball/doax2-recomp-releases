// Default implementations for every Xbox 360 kernel and XAM import.
//
// Each import gets a weak stub that reports the call and aborts. A real implementation is just a
// normal (strong) definition of __imp__<Name> in another file; the linker then picks it over the stub.
#include "imports.h"

#include "crash_handler.h"
#include "guest_abi.h"
#include "guest_memory.h"
#include "kernel/threads.h"
#include "ppc_recomp_shared.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <windows.h>

namespace
{
    std::unordered_map<std::string_view, uint32_t> g_variables;

    [[noreturn]] __attribute__((noinline)) void UnimplementedImport(const char* name, const PPCContext& ctx)
    {
        fprintf(stderr, "Unimplemented import %s, called from 0x%08X\n", name, uint32_t(ctx.lr));
        crash::Fatal(std::string("The game called ") + name + ", which this runtime doesn't implement, and can't go on.");
    }
}

#define IMPORT_FUNC(lib, ordinal, name, thunk, slot) \
    PPC_WEAK_FUNC(__imp__##name) { (void)base; UnimplementedImport(#name, ctx); }
#define IMPORT_VAR(lib, ordinal, name, slot)
#include "imports.inc"
#undef IMPORT_FUNC
#undef IMPORT_VAR

namespace guest
{
    // TRACE_IMPORTS=<seconds>[,all] logs import calls from that many seconds after startup on
    // (0 = from the start). Frequent, uninteresting calls are left out unless "all" is given.
    namespace
    {
        struct TraceSettings
        {
            bool enabled = false;
            bool all = false;
            uint64_t startTicks = 0;

            TraceSettings()
            {
                const char* setting = std::getenv(GAME_PREFIX "_TRACE_IMPORTS");
                if (!setting)
                    return;
                enabled = true;
                char* end = nullptr;
                startTicks = GetTickCount64() + uint64_t(strtoull(setting, &end, 10)) * 1000;
                all = end && strstr(end, "all");
            }
        };

        const TraceSettings& Settings()
        {
            static const TraceSettings settings;
            return settings;
        }

        bool IsNoisy(const char* name)
        {
            static const char* const kNoisy[] = { "RtlEnterCriticalSection", "RtlLeaveCriticalSection", "RtlTryEnterCriticalSection",
                "KeGetCurrentProcessType", "KeQueryPerformanceFrequency", "KfAcquireSpinLock", "KfReleaseSpinLock",
                "KeAcquireSpinLockAtRaisedIrql", "KeReleaseSpinLockFromRaisedIrql", "KeTlsGetValue", "KeTlsSetValue" };
            if (Settings().all)
                return false;
            for (const char* noisy : kNoisy)
            {
                if (strcmp(name, noisy) == 0)
                    return true;
            }
            return false;
        }
    }

    bool TraceImports()
    {
        const TraceSettings& settings = Settings();
        return settings.enabled && GetTickCount64() >= settings.startTicks;
    }

    static uint32_t TraceThreadId()
    {
        const kernel::GuestThread* thread = kernel::CurrentThread();
        return thread ? thread->ThreadId() : 0;
    }

    void TraceImportCall(const char* name, const PPCContext& ctx)
    {
        if (IsNoisy(name))
            return;
        fprintf(stderr, "[import] t%u %s(%08X, %08X, %08X, %08X) from %08X\n", TraceThreadId(), name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
            ctx.r6.u32, uint32_t(ctx.lr));
    }

    void TraceImportResult(const char* name, const PPCContext& ctx)
    {
        if (IsNoisy(name))
            return;
        fprintf(stderr, "[import] t%u %s -> %08X\n", TraceThreadId(), name, ctx.r3.u32);
    }
}

namespace imports
{
    void ResolveSlots()
    {
        // Every variable import gets 0x100 zeroed bytes until it has a real implementation.
        constexpr uint32_t kVariableSize = 0x100;
        uint32_t count = 0;
#define IMPORT_FUNC(lib, ordinal, name, thunk, slot)
#define IMPORT_VAR(lib, ordinal, name, slot) count++;
#include "imports.inc"
#undef IMPORT_FUNC
#undef IMPORT_VAR

        uint32_t next = guest::SystemAlloc(count * kVariableSize);
        if (!next)
        {
            fprintf(stderr, "Can't allocate kernel variable exports\n");
            abort();
        }

#define IMPORT_FUNC(lib, ordinal, name, thunk, slot) \
        if (slot != 0) guest::StoreU32(slot, thunk);
#define IMPORT_VAR(lib, ordinal, name, slot) \
        guest::StoreU32(slot, next); \
        g_variables[#name] = next; \
        next += kVariableSize;
#include "imports.inc"
#undef IMPORT_FUNC
#undef IMPORT_VAR
    }

    uint32_t VariableAddress(std::string_view name)
    {
        const auto it = g_variables.find(name);
        return it != g_variables.end() ? it->second : 0;
    }

    uint32_t FunctionThunk(std::string_view library, uint32_t ordinal)
    {
#define IMPORT_FUNC(lib, importOrdinal, name, thunk, slot) \
        if (library == #lib && ordinal == importOrdinal) return thunk;
#define IMPORT_VAR(lib, ordinal, name, slot)
#include "imports.inc"
#undef IMPORT_FUNC
#undef IMPORT_VAR
        return 0;
    }

    void RegisterThunks()
    {
#define IMPORT_FUNC(lib, ordinal, name, thunk, slot) \
        PPC_LOOKUP_FUNC(guest::g_base, thunk) = __imp__##name;
#define IMPORT_VAR(lib, ordinal, name, slot)
#include "imports.inc"
#undef IMPORT_FUNC
#undef IMPORT_VAR
    }
}
