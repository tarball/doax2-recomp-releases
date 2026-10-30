// Kernel variable exports that need real contents (imports.cpp gives each zeroed storage).
#include "kernel/variables.h"

#include "guest_abi.h"
#include "imports.h"
#include "log.h"
#include "util/profiler.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <windows.h>

namespace
{
    // X_TIME_STAMP_BUNDLE: updated every millisecond, as by the kernel's clock interrupt.
    struct TimeStampBundle
    {
        be<uint64_t> interruptTime; // 100 ns units since boot
        be<uint64_t> systemTime;    // FILETIME
        be<uint32_t> tickCount;     // milliseconds since boot
        be<uint32_t> padding;
    };

    void UpdateTimeStampBundle(uint32_t address)
    {
        profile::NameThread("Time stamp bundle");
        auto* bundle = guest::Translate<TimeStampBundle>(address);
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            bundle->interruptTime = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100);
            bundle->systemTime = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            bundle->tickCount = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

namespace kernel
{
    void InitializeKernelVariables()
    {
        if (const uint32_t clock = imports::VariableAddress("VdGpuClockInMHz"))
            guest::StoreU32(clock, 500);

        // A critical section the GPU code locks; zeroed memory would look permanently held.
        if (const uint32_t lock = imports::VariableAddress("VdHSIOCalibrationLock"))
        {
            PPCContext ctx{};
            ctx.r3.u64 = lock;
            __imp__RtlInitializeCriticalSection(ctx, guest::g_base);
        }

        if (const uint32_t commandLine = imports::VariableAddress("ExLoadedCommandLine"))
            strcpy(guest::Translate<char>(commandLine), "\"default.xex\"");

        if (const uint32_t bundle = imports::VariableAddress("KeTimeStampBundle"))
            std::thread(UpdateTimeStampBundle, bundle).detach();
    }
}
