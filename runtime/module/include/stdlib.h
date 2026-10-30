// <stdlib.h> and <malloc.h> declare what clang's <mm_malloc.h> (through <immintrin.h>) refers to in _mm_malloc and
// _mm_free, which the generated code never calls: the module would fail to link if it did.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif
    void* __cdecl malloc(size_t size);
    void __cdecl free(void* block);
#ifdef __cplusplus
}
#endif
