#pragma once

#include <cstdint>
#include <string>

struct PPCFuncMapping;

namespace crash
{
    // Reports host crashes in guest terms: faults inside guest memory print the guest address,
    // so a guest null-pointer dereference reads "guest address 0x0000000C" instead of a raw host address.
    // `mappings` (the recompiled function table) lets faults be attributed to guest functions.
    void Install(const PPCFuncMapping* mappings);

    // The calling thread handles its own faults (the profiler's stack walker): don't report them.
    void SuppressReportsOnThisThread();

    // The recompiled guest function containing a host code address (0 if none).
    uint32_t GuestFunctionAt(uintptr_t hostPc);
    // The recompiled guest function containing a guest code address: the nearest one starting at or below it (0 if none).
    uint32_t GuestFunctionContaining(uint32_t guestAddress);

    // Ends the game after something it can't go on from (an error the game itself treats as fatal, an import the runtime
    // doesn't implement): logs `message` and the calling guest thread's backtrace, shows the message in a message box
    // unless the game runs headless, and exits with code 1.
    [[noreturn]] void Fatal(const std::string& message);
}
