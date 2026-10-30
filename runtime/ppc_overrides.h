#pragma once

// Force-included into the generated code (see CMakeLists.txt) to replace XenonRecomp defaults.
#include <cstdint>

// mftb reads a 50 MHz time base, as on the 360, instead of the host cycle counter.
uint64_t DOA4QueryTimeBase();
#define PPC_QUERY_TIMEBASE() DOA4QueryTimeBase()

// Traps (tw, twi, td, tdi whose condition holds) and jump table indices out of range: reported by the runtime
// (runtime/guest_traps.cpp), which prints the guest's debug print service.
struct PPCContext;
void GuestTrap(PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t instruction);
[[noreturn]] void GuestSwitchOutOfRange(uint32_t address, uint32_t index);
#define PPC_TRAP(address, instruction) GuestTrap(ctx, base, address, instruction)
#define PPC_SWITCH_OUT_OF_RANGE(address, index) GuestSwitchOutOfRange(address, index)

// setjmp and longjmp: the host jmp_buf kept for each guest buffer, a table the runtime keeps (runtime/guest_setjmp.cpp)
// rather than XenonRecomp's default in the generated code, which needs the C++ standard library's containers.
extern "C" void* PPCJmpBuf(uint32_t guestBuffer);
#define PPC_JMP_BUF(guestBuffer) (*static_cast<jmp_buf*>(PPCJmpBuf(guestBuffer)))

// Every store checks the store map first (guest_memory.h): a byte for each 4 KB page, 1 MB below `base`, which is 0
// unless netplay's page log watches the page (netplay/page_log.h); then PPCStoreMiss copies the page out before the
// frame's first write to it. A store of n bytes checks the pages of its first and last bytes (unaligned stores can cross
// a page); vectors and cache blocks are aligned, so they lie in one. PPCStoreMiss preserves every register, so the
// check costs its callers nothing but the loads and the branch: the YMM registers whole too, which needs it compiled
// without clang's vzeroupper on return (runtime/store_miss.cpp).
extern "C" __attribute__((preserve_all, cold, noinline)) void PPCStoreMiss(uint32_t address, uint32_t size);

static inline __attribute__((always_inline)) const uint8_t* PPCStoreMap(const uint8_t* base)
{
    return base - 0x100000;
}

static inline __attribute__((always_inline)) void PPCStoreCheck(const uint8_t* base, uint32_t address, uint32_t size)
{
    const uint8_t* map = PPCStoreMap(base);
    if (__builtin_expect((map[address >> 12] | map[(address + (size - 1)) >> 12]) != 0, 0))
        PPCStoreMiss(address, size);
}

static inline __attribute__((always_inline)) void PPCStoreCheckAligned(const uint8_t* base, uint32_t address, uint32_t size)
{
    if (__builtin_expect(PPCStoreMap(base)[address >> 12] != 0, 0))
        PPCStoreMiss(address, size);
}

#define PPC_STORE_U8(x, y)                                                                                                     \
    do                                                                                                                         \
    {                                                                                                                          \
        const uint32_t ppcStoreAddress = uint32_t(x);                                                                          \
        PPCStoreCheckAligned(base, ppcStoreAddress, 1);                                                                        \
        *(volatile uint8_t*)(base + ppcStoreAddress) = (y);                                                                    \
    } while (0)
#define PPC_STORE_U16(x, y)                                                                                                    \
    do                                                                                                                         \
    {                                                                                                                          \
        const uint32_t ppcStoreAddress = uint32_t(x);                                                                          \
        PPCStoreCheck(base, ppcStoreAddress, 2);                                                                               \
        *(volatile uint16_t*)(base + ppcStoreAddress) = __builtin_bswap16(y);                                                  \
    } while (0)
#define PPC_STORE_U32(x, y)                                                                                                    \
    do                                                                                                                         \
    {                                                                                                                          \
        const uint32_t ppcStoreAddress = uint32_t(x);                                                                          \
        PPCStoreCheck(base, ppcStoreAddress, 4);                                                                               \
        *(volatile uint32_t*)(base + ppcStoreAddress) = __builtin_bswap32(y);                                                  \
    } while (0)
#define PPC_STORE_U64(x, y)                                                                                                    \
    do                                                                                                                         \
    {                                                                                                                          \
        const uint32_t ppcStoreAddress = uint32_t(x);                                                                          \
        PPCStoreCheck(base, ppcStoreAddress, 8);                                                                               \
        *(volatile uint64_t*)(base + ppcStoreAddress) = __builtin_bswap64(y);                                                  \
    } while (0)
// Stores XenonRecomp takes for MMIO (followed by eieio) are plain stores, and those into the XMA decoder's register page
// (0x7FEA0000-0x7FEAFFFF) also go to the decoder (audio/xma.cpp) with the register's value: the registers are little
// endian, so it's the stored word byte-swapped, whether stw stored it or stwbrx. (DOAX2's XAudio drives the decoder
// through them; DOA4's MMIO stores are D3D's, to the GPU's registers, which the command processor reads.)
void XmaRegisterWrite(uint32_t address, uint32_t value);
#define PPC_MM_STORE_U32(x, y)                                                                                                 \
    do                                                                                                                         \
    {                                                                                                                          \
        const uint32_t ppcMmioAddress = uint32_t(x), ppcMmioValue = uint32_t(y);                                               \
        PPC_STORE_U32(ppcMmioAddress, ppcMmioValue);                                                                           \
        if ((ppcMmioAddress & 0xFFFF0000) == 0x7FEA0000)                                                                       \
            XmaRegisterWrite(ppcMmioAddress, __builtin_bswap32(ppcMmioValue));                                                 \
    } while (0)
// Runs of stores off one base register in straight-line code have one check of the bytes they write before the first
// (XenonRecomp's group_stores, config/DOA4.toml): theirs are stores without one. Nothing between the check and the stores
// can arm a page again (a frame boundary comes only through a call).
#define PPC_CHECK_STORES(x, size) PPCStoreCheck(base, uint32_t(x), size)
#define PPC_STORE_COVERED_U8(x, y) *(volatile uint8_t*)(base + uint32_t(x)) = (y)
#define PPC_STORE_COVERED_U16(x, y) *(volatile uint16_t*)(base + uint32_t(x)) = __builtin_bswap16(y)
#define PPC_STORE_COVERED_U32(x, y) *(volatile uint32_t*)(base + uint32_t(x)) = __builtin_bswap32(y)
#define PPC_STORE_COVERED_U64(x, y) *(volatile uint64_t*)(base + uint32_t(x)) = __builtin_bswap64(y)
// dcbz and dcbzl: an aligned cache block (32 or 128 bytes).
#define PPC_STORE_ZERO(x, size)                                                                                                \
    do                                                                                                                         \
    {                                                                                                                          \
        const uint32_t ppcStoreAddress = uint32_t(x);                                                                          \
        PPCStoreCheckAligned(base, ppcStoreAddress, size);                                                                     \
        memset(base + ppcStoreAddress, 0, size);                                                                               \
    } while (0)
// stvx and its variants: an aligned vector.
#define PPC_STORE_V128(x, y)                                                                                                   \
    do                                                                                                                         \
    {                                                                                                                          \
        const uint32_t ppcStoreAddress = uint32_t(x);                                                                          \
        PPCStoreCheckAligned(base, ppcStoreAddress, 16);                                                                       \
        simde_mm_store_si128((simde__m128i*)(base + ppcStoreAddress), y);                                                      \
    } while (0)
// stwcx. and stdcx.: the page is copied out whether the store happens or not (it's aligned).
static inline __attribute__((always_inline)) bool PPCStoreConditionalU32(uint8_t* base, uint32_t address, uint32_t expected, uint32_t value)
{
    PPCStoreCheckAligned(base, address, 4);
    return __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + address), expected, value);
}
static inline __attribute__((always_inline)) bool PPCStoreConditionalU64(uint8_t* base, uint32_t address, uint64_t expected, uint64_t value)
{
    PPCStoreCheckAligned(base, address, 8);
    return __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(base + address), expected, value);
}
#define PPC_STORE_CONDITIONAL_U32(x, expected, y) PPCStoreConditionalU32(base, uint32_t(x), uint32_t(expected), uint32_t(y))
#define PPC_STORE_CONDITIONAL_U64(x, expected, y) PPCStoreConditionalU64(base, uint32_t(x), uint64_t(expected), uint64_t(y))
