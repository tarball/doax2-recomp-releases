// The C runtime's functions the executable exports to the game's code module (module/crt_exports.h): a jump each to the
// C runtime's function of the name, as an import thunk jumps to its import.
#include "module/crt_exports.h"

#define MODULE_CRT_JUMP(name)                                                                                          \
    "    .globl ModuleCrt_" #name "\n"                                                                                 \
    "    .def ModuleCrt_" #name "; .scl 2; .type 32; .endef\n"                                                         \
    "    .p2align 4\n"                                                                                                 \
    "ModuleCrt_" #name ":\n"                                                                                           \
    "    jmp " #name "\n"

__asm__("    .text\n"
    MODULE_CRT_JUMP(_setjmp)
    MODULE_CRT_JUMP(longjmp)
    MODULE_CRT_JUMP(memcpy)
    MODULE_CRT_JUMP(memset)
    MODULE_CRT_JUMP(sqrt)
    MODULE_CRT_JUMP(sqrtf)
    MODULE_CRT_JUMP(fma)
    MODULE_CRT_JUMP(exp2f)
    MODULE_CRT_JUMP(log2f));
