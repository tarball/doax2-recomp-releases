// The store map's misses from the generated code (ppc_overrides.h): PPCStoreMiss, which the generated code calls with
// every register live (preserve_all), so the call costs its callers nothing but the check. It must give every register
// back whole, the YMM registers' upper halves too: the generated code keeps VMX's floating point there, in double
// precision (vmaddfp and the like convert four floats to four doubles), across stores. Clang ends a function whose code
// leaves the YMM registers' upper halves in use with a vzeroupper, and in a preserve_all function that comes after the
// registers were restored: every miss cleared the upper halves the caller kept. This file is compiled without
// vzeroupper insertion (CMakeLists.txt) for that. Whenever the page log watched pages (savestates, a session's
// rollback, the page census), the frame's first store to each page cleared them, and the cloth and hair simulations'
// vectors lost two of their lanes: Kasumi's sash and hair stretched across the screen (ANALYSIS.md, Phase 6).
// runtime_tests checks that a miss keeps every register.
#include "guest_memory.h"

#include <immintrin.h>

extern "C" __attribute__((preserve_all, cold, noinline)) void PPCStoreMiss(uint32_t address, uint32_t size)
{
    // The handler and Windows' code it calls get the upper halves clean, as clang's own vzeroupper before a call would
    // make them (no transitions to legacy SSE with them in use): preserve_all saved them, and restores them whole.
    _mm256_zeroupper();
    guest::GuestStoreMiss(address, size, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}
