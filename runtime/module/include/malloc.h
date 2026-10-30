// See <stdlib.h>.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif
    void* __cdecl _aligned_malloc(size_t size, size_t alignment);
    void __cdecl _aligned_free(void* block);
#ifdef __cplusplus
}
#endif
