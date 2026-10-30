#include "crash_handler.h"

#include "guest_memory.h"
#include "kernel/threads.h"
#include "ppc_recomp_shared.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <windows.h>


namespace
{
    // Host addresses of recompiled functions, sorted, for mapping a host pc back to guest code.
    struct HostFunction
    {
        uintptr_t host;
        uint32_t guest;
    };
    std::vector<HostFunction> g_functions;
    std::vector<uint32_t> g_guestStarts; // the recompiled functions' guest addresses, sorted
    thread_local bool t_suppressReports = false;

    // The recompiled function whose host code holds `pc`, or 0 for other host code. The host function holding pc comes
    // from its x64 unwind entry: the nearest recompiled function below pc isn't enough, since the runtime's code can
    // follow it. Leaf functions without unwind entries give 0.
    uint32_t LookupGuestFunction(uintptr_t pc)
    {
        DWORD64 imageBase = 0;
        const RUNTIME_FUNCTION* entry = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
        if (!entry)
            return 0;
        const uintptr_t begin = uintptr_t(imageBase + entry->BeginAddress);
        const auto it = std::lower_bound(g_functions.begin(), g_functions.end(), begin, [](const HostFunction& f, uintptr_t value) { return f.host < value; });
        return it != g_functions.end() && it->host == begin ? it->guest : 0;
    }

    // Fatal faults. C++ exceptions and debugger events pass through unreported. Float exceptions only
    // fire if MXCSR has unmasked them, which is a runtime bug.
    bool IsFault(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INEXACT_RESULT:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_DENORMAL_OPERAND:
        case STATUS_FLOAT_MULTIPLE_FAULTS:
        case STATUS_FLOAT_MULTIPLE_TRAPS:
            return true;
        default:
            return false;
        }
    }

    LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
    {
        const EXCEPTION_RECORD* record = info->ExceptionRecord;
        if (!IsFault(record->ExceptionCode) || t_suppressReports)
            return EXCEPTION_CONTINUE_SEARCH;

        const uintptr_t pc = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(pc), &module);
        wchar_t moduleName[MAX_PATH] = L"?";
        if (module)
            GetModuleFileNameW(module, moduleName, MAX_PATH);
        fprintf(stderr, "\n*** Host exception 0x%08lX at host pc %p (%ls+0x%llX)\n", record->ExceptionCode, record->ExceptionAddress, moduleName,
            (unsigned long long)(pc - reinterpret_cast<uintptr_t>(module)));

        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        {
            const auto address = reinterpret_cast<const uint8_t*>(record->ExceptionInformation[1]);
            const char* kind = record->ExceptionInformation[0] == 0 ? "read" : record->ExceptionInformation[0] == 1 ? "write" : "execute";
            if (guest::g_base && address >= guest::g_base && address < guest::g_base + guest::kAddressSpaceSize)
                fprintf(stderr, "*** Guest %s fault at guest address 0x%08X\n", kind, uint32_t(address - guest::g_base));
            else
                fprintf(stderr, "*** Host %s fault at %p (outside guest memory)\n", kind, address);
        }

        if (const uint32_t function = LookupGuestFunction(pc))
            fprintf(stderr, "*** In recompiled guest function sub_%08X (host offset +0x%llX)\n", function,
                (unsigned long long)(pc - std::prev(std::upper_bound(g_functions.begin(), g_functions.end(), pc,
                    [](uintptr_t value, const HostFunction& f) { return value < f.host; }))->host));

        if (const kernel::GuestThread* thread = kernel::CurrentThread(); thread && thread->Context())
        {
            // The registers the context keeps: with XenonRecomp's *_as_local options, the others are locals of the recompiled
            // functions (see the XenonRecomp config in config/).
            const PPCContext& ctx = *thread->Context();
            fprintf(stderr, "*** Guest thread %u (guest base %p):", thread->ThreadId(), guest::g_base);
#ifndef PPC_CONFIG_SKIP_LR
            fprintf(stderr, " lr=%08X", uint32_t(ctx.lr));
#endif
#ifndef PPC_CONFIG_CTR_AS_LOCAL
            fprintf(stderr, " ctr=%08X", ctx.ctr.u32);
#endif
            fprintf(stderr, "\n***   r1  %08X  r13 %08X\n", ctx.r1.u32, ctx.r13.u32);
            const uint32_t arguments[8] = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };
            for (int i = 0; i < 8; i += 4)
                fprintf(stderr, "***   r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X\n", i + 3, arguments[i], i + 4, arguments[i + 1], i + 5,
                    arguments[i + 2], i + 6, arguments[i + 3]);
            guest::PrintBacktrace(ctx.r1.u32);
        }

        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace crash
{
    void Install(const PPCFuncMapping* mappings)
    {
        for (const PPCFuncMapping* mapping = mappings; mapping && mapping->host; ++mapping)
        {
            g_functions.push_back({ reinterpret_cast<uintptr_t>(mapping->host), uint32_t(mapping->guest) });
            g_guestStarts.push_back(uint32_t(mapping->guest));
        }
        std::sort(g_functions.begin(), g_functions.end(), [](const HostFunction& a, const HostFunction& b) { return a.host < b.host; });
        std::sort(g_guestStarts.begin(), g_guestStarts.end());
        AddVectoredExceptionHandler(1, OnException);
    }

    void SuppressReportsOnThisThread()
    {
        t_suppressReports = true;
    }

    uint32_t GuestFunctionAt(uintptr_t hostPc)
    {
        return LookupGuestFunction(hostPc);
    }

    uint32_t GuestFunctionContaining(uint32_t guestAddress)
    {
        const auto it = std::upper_bound(g_guestStarts.begin(), g_guestStarts.end(), guestAddress);
        return it == g_guestStarts.begin() ? 0 : *std::prev(it);
    }

    void Fatal(const std::string& message)
    {
        fprintf(stderr, "\n*** Fatal: %s\n", message.c_str());
        if (const kernel::GuestThread* thread = kernel::CurrentThread(); thread && thread->Context())
            guest::PrintBacktrace(thread->Context()->r1.u32);
        fflush(stderr);
        const char* headless = std::getenv(GAME_PREFIX "_HEADLESS");
        if (!headless || std::atoi(headless) == 0)
            MessageBoxA(nullptr, message.c_str(), GAME_TITLE, MB_ICONERROR | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
        // Without running static destructors under the game's other threads.
        std::_Exit(1);
    }
}
