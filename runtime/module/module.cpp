// Compiled into the game's code module with the generated code, and like it without the C runtime or any library
// (ANALYSIS.md, Phase 5, The game's code module): what the compiler's code refers to that the C runtime would otherwise
// define, and the module's info for the executable that loads it (module.h).
#include "ppc_config.h"
#include "ppc_context.h"

#include "module.h"

#include <intrin.h>

// Floating point is used (the C runtime's marker symbol, which every object using floating point refers to).
extern "C" int _fltused = 0;

// The /GS stack cookie of the few generated functions with arrays on their stack (a PPC context kept across a setjmp,
// among them), and its check, as the C runtime's: a mismatch ends the process at once. The loader randomizes the cookie
// before any of the module's code runs; until then it's the C runtime's initial value.
extern "C" uintptr_t __security_cookie = 0x00002B992DDFA232ull;

extern "C" void __cdecl __security_check_cookie(uintptr_t cookie)
{
    if (cookie != __security_cookie)
        __fastfail(2); // FAST_FAIL_STACK_COOKIE_CHECK_FAILURE
}

extern "C" __declspec(dllexport) const PPCModuleInfo PPCModule = PPC_MODULE_INFO(PPCFuncMappings, &__security_cookie);
