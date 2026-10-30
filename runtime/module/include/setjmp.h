// <setjmp.h> for the generated code (and clang's <intrin.h>, which includes it): the x64 jmp_buf and the C runtime's
// _setjmp and longjmp, as Visual C++ declares them. clang calls _setjmp with the frame address, so longjmp unwinds.
#pragma once

typedef struct __declspec(align(16)) _SETJMP_FLOAT128
{
    unsigned __int64 Part[2];
} SETJMP_FLOAT128;

#define _JBLEN 16
typedef SETJMP_FLOAT128 _JBTYPE;
typedef _JBTYPE jmp_buf[_JBLEN];

#ifdef __cplusplus
extern "C"
{
#endif
    int __cdecl _setjmp(jmp_buf buffer);
#ifdef __cplusplus
    __declspec(noreturn) void __cdecl longjmp(jmp_buf buffer, int value) noexcept(false);
#else
    __declspec(noreturn) void __cdecl longjmp(jmp_buf buffer, int value);
#endif
#ifdef __cplusplus
}
#endif

#define setjmp _setjmp
