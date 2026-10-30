#pragma once

#include <csetjmp>
#include <cstddef>

// The C runtime's functions the game's code module calls (the shims in module/include declare them), which the
// executable exports from its own C runtime (ANALYSIS.md, Phase 5, The game's code module): each export is
// <name>=ModuleCrt_<name> (runtime_exports.def), a jump to the C runtime's function of that name, which the linker finds
// as it finds any other. The jump leaves the caller's return address, stack and registers as they were, so the C
// runtime's code runs as if called directly: setjmp saves its caller's context, and the results are the C runtime's.
//
// Not by the name alone: lld-link binds an export whose symbol is still undefined when it reads the exports to a C++
// function of that name if it finds one, and the C++ library (libcpmt.lib's special_math.obj) has sqrt's float and long
// double overloads. So the export sqrt was the long double one, ?sqrt@@YAOO@Z, which jumps to the C runtime's sqrt for a
// negative input (which sets errno), now itself: a loop without end. The CPU fights' particles took square roots of
// small negative numbers, and the game froze (ANALYSIS.md, Phase 6, The CPU fights' hang). An alias of the C runtime's
// function (/alternatename) instead of a jump trips lld-link's import library for the executable, which can't name such
// an export. tools/moduledefs.py refuses an export the executable's objects don't define by its name alone.
extern "C"
{
    // _setjmp's second argument is the frame longjmp unwinds to (clang passes the caller's; 0: no unwinding).
    __attribute__((returns_twice)) int ModuleCrt__setjmp(jmp_buf buffer, void* frame);
    [[noreturn]] void ModuleCrt_longjmp(jmp_buf buffer, int value);
    void* ModuleCrt_memcpy(void* destination, const void* source, size_t size);
    void* ModuleCrt_memset(void* destination, int value, size_t size);
    double ModuleCrt_sqrt(double x);
    float ModuleCrt_sqrtf(float x);
    double ModuleCrt_fma(double x, double y, double z);
    float ModuleCrt_exp2f(float x);
    float ModuleCrt_log2f(float x);
}
