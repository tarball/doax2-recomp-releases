#pragma once

namespace kernel
{
    // Fills in kernel variable exports (clock rate, locks, command line, time stamp bundle).
    // Call after imports::ResolveSlots().
    void InitializeKernelVariables();
}
