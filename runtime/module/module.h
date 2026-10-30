#pragma once

// The game's code module (<executable>_ppc.dll, doa4_ppc.dll): the recompiled code, which isn't in the executable but
// in a module of its own beside it, built with clang and lld alone (ANALYSIS.md, Phase 5, The game's code module). This
// header is shared by the module (module.cpp) and the executable (loader.cpp): what the module tells the executable
// about itself, exported as PPCModule, which the loader checks against what it was compiled with before running any of
// the module's code.
#include <stddef.h>
#include <stdint.h>

// The generated code's options (PPCModuleConfigFlags), whichever header comes first.
#include "ppc_config.h"

struct PPCContext;
struct PPCFuncMapping;

// Bumped when anything the module and the executable share changes: this struct, the functions either exports to the
// other (doa4.def, doa4_ppc.def), the store map, what the shims declare.
constexpr uint32_t kPPCModuleInterface = 1;

struct PPCModuleInfo
{
    uint32_t interface;      // kPPCModuleInterface
    uint32_t configFlags;    // PPCModuleConfigFlags() of the ppc_config.h it was compiled with
    uint64_t imageBase, imageSize, codeBase, codeSize;
    // PPCContext's layout, which the generated code and the runtime both use.
    uint32_t contextSize, contextAlign;
    uint32_t r1Offset, r3Offset, r13Offset, fpscrOffset, f1Offset, v0Offset;
    PPCFuncMapping* functions; // PPCFuncMappings: every function's guest address and host code, ending with a null
    uintptr_t* securityCookie; // the module's /GS cookie, which the loader randomizes before any of its code runs
};

// The PPC_CONFIG_* switches in a mask, for comparing the module's with the runtime's.
constexpr uint32_t PPCModuleConfigFlags()
{
    uint32_t flags = 0;
#ifdef PPC_CONFIG_CTR_AS_LOCAL
    flags |= 1u << 0;
#endif
#ifdef PPC_CONFIG_XER_AS_LOCAL
    flags |= 1u << 1;
#endif
#ifdef PPC_CONFIG_RESERVED_AS_LOCAL
    flags |= 1u << 2;
#endif
#ifdef PPC_CONFIG_SKIP_LR
    flags |= 1u << 3;
#endif
#ifdef PPC_CONFIG_SKIP_MSR
    flags |= 1u << 4;
#endif
#ifdef PPC_CONFIG_CR_AS_LOCAL
    flags |= 1u << 5;
#endif
#ifdef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    flags |= 1u << 6;
#endif
#ifdef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
    flags |= 1u << 7;
#endif
#ifdef PPC_CONFIG_KEEP_CONTEXT_REGISTERS
    flags |= 1u << 8;
#endif
    return flags;
}

// The module's info as this compilation sees it (both sides compute it, after ppc_context.h).
#define PPC_MODULE_INFO(functions, cookie)                                                                                     \
    PPCModuleInfo                                                                                                              \
    {                                                                                                                          \
        kPPCModuleInterface, PPCModuleConfigFlags(), PPC_IMAGE_BASE, PPC_IMAGE_SIZE, PPC_CODE_BASE, PPC_CODE_SIZE,             \
            uint32_t(sizeof(PPCContext)), uint32_t(alignof(PPCContext)), uint32_t(offsetof(PPCContext, r1)),                   \
            uint32_t(offsetof(PPCContext, r3)), uint32_t(offsetof(PPCContext, r13)), uint32_t(offsetof(PPCContext, fpscr)),    \
            uint32_t(offsetof(PPCContext, f1)), uint32_t(offsetof(PPCContext, v0)), functions, cookie                          \
    }
