// Runtime unit tests: page heap bookkeeping and the memory exports called the way recompiled code calls them.
#include "audio/xma.h"
#include "config.h"
#include "disc_image.h"
#include "gpu/shader/hlsl.h"
#include "gpu/shader/shader.h"
#include "guest_abi.h"
#include "guest_memory.h"
#include "imports.h"
#include "kernel/module.h"
#include "kernel/objects.h"
#include "kernel/status.h"
#include "kernel/threads.h"
#include "module/crt_exports.h"
#include "netplay/frame.h"
#include "netplay/replay.h"
#include "netplay/session.h"
#include "page_heap.h"
#include "settings.h"
#include "util/image.h"
#include "vfs/iso_device.h"
#include "vfs/vfs.h"
#include "xam/achievements.h"
#include "xam/spa.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include <windows.h>
#include <d3dcompiler.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                    \
    do                                                      \
    {                                                       \
        g_checks++;                                         \
        if (!(cond))                                        \
        {                                                   \
            g_failures++;                                   \
            printf("FAIL %s:%d: %s", __func__, __LINE__, #cond); \
            printf(" | " __VA_ARGS__);                      \
            printf("\n");                                   \
        }                                                   \
    } while (0)

// ---------------------------------------------------------------------------------------------
// PageHeap

static void test_heap_alloc_alignment_and_order()
{
    PageHeap heap(0x40000000, 0x01000000, 0x10000); // 16 MB of 64 KB pages
    const uint32_t a = heap.Alloc(0x10000, 0, true, 4, false);
    const uint32_t b = heap.Alloc(0x18000, 0, true, 4, false); // rounds up to 2 pages
    CHECK(a == 0x40000000, "a=%08X", a);
    CHECK(b == 0x40010000, "b=%08X", b);
    const uint32_t c = heap.Alloc(0x10000, 0x100000, true, 4, false); // 1 MB alignment
    CHECK(c == 0x40100000, "c=%08X", c);
    const uint32_t top = heap.Alloc(0x20000, 0, true, 4, true);
    CHECK(top == 0x40FE0000, "top=%08X", top);
    const uint32_t top2 = heap.Alloc(0x10000, 0x100000, true, 4, true);
    CHECK(top2 == 0x40F00000, "top2=%08X", top2);
}

static void test_heap_range_and_exhaustion()
{
    PageHeap heap(0x70000000, 0x00100000, 0x10000); // 16 pages
    const uint32_t a = heap.AllocRange(0x70080000, 0x700A0000, 0x20000, 0, true, 4, true);
    CHECK(a == 0x70080000, "a=%08X", a);
    CHECK(heap.AllocRange(0x70080000, 0x700A0000, 0x10000, 0, true, 4, false) == 0, "range is full");
    CHECK(heap.Alloc(0x00100000, 0, true, 4, false) == 0, "whole heap no longer fits");
    CHECK(heap.Alloc(0x80000, 0, true, 4, false) == 0x70000000, "bottom half fits");
    CHECK(heap.Alloc(0x60000, 0, true, 4, true) == 0x700A0000, "top 6 pages fit");
    CHECK(heap.Alloc(0x10000, 0, true, 4, false) == 0, "heap exhausted");
}

static void test_heap_fixed_commit_release()
{
    PageHeap heap(0x00000000, 0x00100000, 0x1000);
    CHECK(!heap.AllocFixed(0x10000, 0x3000, false, true, 4), "commit without reserve on free pages fails");
    CHECK(heap.AllocFixed(0x10000, 0x3000, true, false, 4), "reserve");
    CHECK(heap.QueryProtect(0x10000) == 0, "reserved pages have no protection");
    CHECK(!heap.AllocFixed(0x10000, 0x1000, true, false, 4), "can't reserve twice");
    CHECK(heap.AllocFixed(0x11000, 0x1000, false, true, 2), "commit inside reservation");
    CHECK(heap.QueryProtect(0x11000) == 2, "committed page protect");
    CHECK(!heap.AllocFixed(0x12000, 0x2000, false, true, 4), "commit spilling past reservation fails");

    PageHeap::RegionInfo info{};
    CHECK(heap.QueryRegion(0x10000, info), "query");
    CHECK(info.baseAddress == 0x10000 && info.regionSize == 0x1000 && info.state == PageHeap::State::Reserved, "first page: %08X %X", info.baseAddress, info.regionSize);
    CHECK(heap.QueryRegion(0x11800, info) && info.baseAddress == 0x11000 && info.state == PageHeap::State::Committed && info.allocationBase == 0x10000,
        "committed page region base=%08X alloc=%08X", info.baseAddress, info.allocationBase);
    CHECK(heap.QueryRegion(0x13000, info) && info.state == PageHeap::State::Free && info.allocationBase == 0 && info.regionSize == 0xED000,
        "free run size=%X", info.regionSize);

    CHECK(!heap.Release(0x11000), "release needs the region base");
    uint32_t released = 0;
    CHECK(heap.Release(0x10000, &released) && released == 0x3000, "released=%X", released);
    CHECK(heap.QueryRegion(0x10000, info) && info.state == PageHeap::State::Free, "free after release");
}

static void test_heap_decommit_and_protect()
{
    PageHeap heap(0x00000000, 0x00010000, 0x1000);
    const uint32_t a = heap.Alloc(0x4000, 0, true, 4, false);
    CHECK(heap.Decommit(a + 0x1000, 0x1000), "decommit one page");
    CHECK(heap.QueryProtect(a + 0x1000) == 0 && heap.QueryProtect(a) == 4, "only that page decommitted");
    CHECK(!heap.Protect(a, 0x4000, 2), "can't protect a reserved page");
    CHECK(heap.Protect(a + 0x2000, 0x2000, 2) && heap.QueryProtect(a + 0x3000) == 2, "protect committed pages");
}

static void test_heap_top_of_address_space()
{
    // Heaps near the top of the address space; make sure 64-bit math handles them.
    PageHeap heap(0xF0000000, 0x0FD00000, 0x1000);
    const uint32_t a = heap.Alloc(0x1000, 0, true, 4, true);
    CHECK(a == 0xFFCFF000, "a=%08X", a);
    PageHeap full(0xFFFF0000, 0x10000, 0x1000);
    CHECK(full.Alloc(0x10000, 0, true, 4, false) == 0xFFFF0000, "heap ending exactly at 4 GB");
}

// ---------------------------------------------------------------------------------------------
// Exports, called through the generated-code ABI.

// Exports DOA4 doesn't import (DOAX2's), which the generated code doesn't declare.
PPC_EXTERN_FUNC(__imp__KeInitializeSemaphore);
PPC_EXTERN_FUNC(__imp__KeReleaseSemaphore);
PPC_EXTERN_FUNC(__imp__NtCreateSemaphore);
PPC_EXTERN_FUNC(__imp__NtReleaseSemaphore);
PPC_EXTERN_FUNC(__imp__KeWaitForMultipleObjects);
PPC_EXTERN_FUNC(__imp__KeResumeThread);
PPC_EXTERN_FUNC(__imp__XamCreateEnumeratorHandle);
PPC_EXTERN_FUNC(__imp__XamGetPrivateEnumStructureFromHandle);
PPC_EXTERN_FUNC(__imp__XexGetModuleHandle);
PPC_EXTERN_FUNC(__imp__XexGetProcedureAddress);

// A guest stack for calls with more than eight arguments.
static uint32_t ArgumentStack()
{
    static uint32_t stack = guest::SystemAlloc(0x1000);
    return stack;
}

struct Call
{
    std::unique_ptr<PPCContext> ctx = [] {
        auto context = std::make_unique<PPCContext>();
        context->fpscr.loadFromHost();
        return context;
    }();

    template <typename... Args>
    uint32_t operator()(PPCFunc* function, Args... args)
    {
        const uint64_t values[] = { uint64_t(args)... };
        PPCRegister* regs[] = { &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10 };
        for (size_t i = 0; i < sizeof...(Args) && i < 8; i++)
            regs[i]->u64 = values[i];
        // Later arguments go in the caller's parameter save area: 8-byte slots from r1 + 0x50, the value in the low word.
        if constexpr (sizeof...(Args) > 8)
        {
            ctx->r1.u64 = ArgumentStack() + 0x800;
            for (size_t i = 8; i < sizeof...(Args); i++)
                guest::StoreU32(uint32_t(ctx->r1.u32 + 0x54 + (i - 8) * 8), uint32_t(values[i]));
        }
        function(*ctx, guest::g_base);
        return ctx->r3.u32;
    }
};

// Scratch guest memory for in/out arguments.
static uint32_t Scratch()
{
    static uint32_t scratch = guest::SystemAlloc(0x1000);
    return scratch;
}

static void test_nt_allocate_and_free()
{
    Call call;
    const uint32_t basePtr = Scratch(), sizePtr = Scratch() + 4;

    guest::StoreU32(basePtr, 0);
    guest::StoreU32(sizePtr, 0x1801);
    uint32_t result = call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x3000 /* reserve|commit */, 4, 0);
    const uint32_t address = guest::LoadU32(basePtr);
    CHECK(result == status::kSuccess, "status=%08X", result);
    CHECK(address >= 0x10000 && address < 0x40000000 && address % 0x1000 == 0, "4 KB heap address %08X", address);
    // From the top of the heap: DOA4 relocates files loaded there, which only works above their offsets.
    CHECK(address >= 0x30000000, "4 KB pages are allocated top-down: %08X", address);
    CHECK(guest::LoadU32(sizePtr) == 0x2000, "size rounded to pages: %X", guest::LoadU32(sizePtr));
    CHECK(guest::ToHost(address)[0x1FFF] == 0, "zeroed");

    // Query
    const uint32_t infoPtr = Scratch() + 0x100;
    result = call(__imp__NtQueryVirtualMemory, address + 0x1000, infoPtr, 0);
    CHECK(result == status::kSuccess, "query status=%08X", result);
    CHECK(guest::LoadU32(infoPtr + 0) == address + 0x1000, "BaseAddress %08X", guest::LoadU32(infoPtr));
    CHECK(guest::LoadU32(infoPtr + 4) == address, "AllocationBase");
    CHECK(guest::LoadU32(infoPtr + 12) == 0x1000, "RegionSize %X", guest::LoadU32(infoPtr + 12));
    CHECK(guest::LoadU32(infoPtr + 16) == 0x1000, "State MEM_COMMIT");
    CHECK(guest::LoadU32(infoPtr + 20) == 4, "Protect");

    // Release
    guest::StoreU32(sizePtr, 0);
    result = call(__imp__NtFreeVirtualMemory, basePtr, sizePtr, 0x8000 /* release */, 0);
    CHECK(result == status::kSuccess && guest::LoadU32(sizePtr) == 0x2000, "release status=%08X size=%X", result, guest::LoadU32(sizePtr));
    CHECK(call(__imp__NtFreeVirtualMemory, basePtr, sizePtr, 0x8000, 0) == status::kMemoryNotAllocated, "double free fails");
}

static void test_nt_allocate_large_pages_and_fixed()
{
    Call call;
    const uint32_t basePtr = Scratch(), sizePtr = Scratch() + 4;

    guest::StoreU32(basePtr, 0);
    guest::StoreU32(sizePtr, 0x10000);
    uint32_t result = call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x20002000 /* large pages, reserve */, 4, 0);
    const uint32_t address = guest::LoadU32(basePtr);
    CHECK(result == status::kSuccess && address >= 0x40000000 && address < 0x7F000000 && address % 0x10000 == 0, "large-page reserve at %08X", address);
    CHECK(call(__imp__MmQueryAddressProtect, address) == 0, "reserved, not committed");

    // Commit the reservation in place.
    guest::StoreU32(basePtr, address + 0x10);
    guest::StoreU32(sizePtr, 0x100);
    result = call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x20001000 /* large pages, commit */, 4, 0);
    CHECK(result == status::kSuccess && guest::LoadU32(basePtr) == address && guest::LoadU32(sizePtr) == 0x10000,
        "commit in place: %08X %X", guest::LoadU32(basePtr), guest::LoadU32(sizePtr));
    CHECK(call(__imp__MmQueryAddressProtect, address + 0x8000) == 4, "now committed");

    // Committing outside any reservation fails.
    guest::StoreU32(basePtr, 0x3F000000);
    guest::StoreU32(sizePtr, 0x1000);
    CHECK(call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x1000, 4, 0) == status::kNoMemory, "commit of free pages without reserve");

    CHECK(call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x4000 /* decommit isn't an allocation type */, 4, 0) == status::kInvalidParameter, "bad type");

    // A 4 KB request at an address in the 64 KB heap is refused.
    guest::StoreU32(basePtr, 0x50000000);
    CHECK(call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x3000, 4, 0) == status::kAccessDenied, "page size mismatch");
}

static void test_physical_memory()
{
    Call call;
    // 64 KB pages (A0000000 view), 1 MB, 64 KB aligned.
    const uint32_t a = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x100000, 0x20000004, 0, 0xFFFFFFFF, 0);
    CHECK(a >= 0xA0000000 && a < 0xC0000000 && a % 0x10000 == 0, "A0 view: %08X", a);
    CHECK(call(__imp__MmGetPhysicalAddress, a) == a - 0xA0000000, "physical of A0 address");

    // 4 KB pages come from the A0000000 view too (on hardware, the E0000000 view) and never overlap
    // its other physical ranges.
    const uint32_t e = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x2000, 4, 0, 0xFFFFFFFF, 0);
    const uint32_t ePhys = call(__imp__MmGetPhysicalAddress, e);
    CHECK(e >= 0xA0000000 && e < 0xC0000000 && ePhys == e - 0xA0000000, "4 KB pages: %08X phys %08X", e, ePhys);
    const uint32_t aPhys = a - 0xA0000000;
    CHECK(ePhys + 0x2000 <= aPhys || ePhys >= aPhys + 0x100000, "physical ranges overlap: A %08X 4 KB %08X", aPhys, ePhys);

    // They're 4 KB granular: sixteen fit in a 64 KB physical window.
    uint32_t pages[16];
    bool allFit = true;
    for (uint32_t& page : pages)
    {
        page = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x1000, 4, 0x03000000, 0x0300FFFF, 0);
        allFit = allFit && page != 0;
    }
    CHECK(allFit, "sixteen 4 KB pages in 64 KB");
    for (const uint32_t page : pages)
        call(__imp__MmFreePhysicalMemory, 0, page);

    // Every view of a physical page is the same memory: D3D reads and patches vertex shaders through
    // C0000000 addresses of 4 KB-page allocations.
    guest::StoreU32(guest::kSharedViewC + ePhys + 0x10, 0x12345678);
    CHECK(guest::LoadU32(e + 0x10) == 0x12345678, "C0 alias of a 4 KB page: %08X", guest::LoadU32(e + 0x10));
    MEMORY_BASIC_INFORMATION memory{};
    CHECK(VirtualQuery(guest::ToHost(guest::kViewE), &memory, sizeof(memory)) && memory.State == MEM_RESERVE, "E0000000 isn't mapped: %lX",
        memory.State);

    // Constrained physical range.
    const uint32_t low = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x1000, 4, 0x01000000, 0x01FFFFFF, 0x10000);
    const uint32_t lowPhys = call(__imp__MmGetPhysicalAddress, low);
    CHECK(low != 0 && lowPhys >= 0x01000000 && lowPhys + 0x1000 <= 0x02000000 && lowPhys % 0x10000 == 0, "constrained: phys %08X", lowPhys);

    // 16 MB pages.
    const uint32_t c = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x100, 0x80000004, 0, 0xFFFFFFFF, 0);
    CHECK(c >= 0xC0000000 && c < 0xE0000000 && c % 0x1000000 == 0, "C0 view: %08X", c);

    CHECK(call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x1000, 0x01 /* no access */, 0, 0xFFFFFFFF, 0) == 0, "needs a readable protection");

    call(__imp__MmFreePhysicalMemory, 0, a);
    const uint32_t again = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x100000, 0x20000004, 0, 0xFFFFFFFF, 0);
    CHECK(again == a, "freed physical range is reused (top-down): %08X vs %08X", again, a);

    // The GPU sees physical memory through the A0000000 view.
    CHECK(guest::PhysicalToGuest(ePhys) == e, "4 KB page from the GPU: %08X", guest::PhysicalToGuest(ePhys));
    CHECK(guest::PhysicalToGuest(aPhys) == a, "A0 page from the GPU: %08X", guest::PhysicalToGuest(aPhys));

    // NtFreeVirtualMemory can't release physical memory (DOA4 tries, then keeps using it).
    const uint32_t basePtr = Scratch(), sizePtr = Scratch() + 4;
    guest::StoreU32(basePtr, e);
    guest::StoreU32(sizePtr, 0);
    CHECK(call(__imp__NtFreeVirtualMemory, basePtr, sizePtr, 0x8000, 0) == status::kMemoryNotAllocated, "NtFreeVirtualMemory of physical memory");
    CHECK(call(__imp__MmQueryAddressProtect, e) == 4, "still allocated after NtFreeVirtualMemory");
    call(__imp__MmFreePhysicalMemory, 0, e);
    CHECK(call(__imp__MmQueryAddressProtect, e) == 0, "freed by MmFreePhysicalMemory");
}

static void test_xam_alloc()
{
    Call call;
    const uint32_t out = Scratch() + 8;
    CHECK(call(__imp__XamAlloc, 0, 0x30, out) == xerror::kSuccess && guest::LoadU32(out) != 0, "XamAlloc");
    const uint32_t p = guest::LoadU32(out);
    CHECK(call(__imp__XamFree, p) == xerror::kSuccess, "XamFree");
    CHECK(call(__imp__XamAlloc, 0x00100000, 0x30, out) == xerror::kSuccess && guest::LoadU32(out) >= 0xA0000000 && guest::LoadU32(out) < 0xC0000000,
        "physical XamAlloc: %08X", guest::LoadU32(out));
}

// ---------------------------------------------------------------------------------------------
// Kernel basics

static uint32_t StoreAnsi(uint32_t address, const char* text)
{
    memcpy(guest::ToHost(address), text, strlen(text) + 1);
    return address;
}

static void test_strings()
{
    Call call;
    const uint32_t text = StoreAnsi(Scratch() + 0x200, "hello");
    const uint32_t ansi = Scratch() + 0x300;
    call(__imp__RtlInitAnsiString, ansi, text);
    CHECK(guest::LoadU32(ansi) == 0x00050006 && guest::LoadU32(ansi + 4) == text, "ANSI_STRING %08X %08X", guest::LoadU32(ansi), guest::LoadU32(ansi + 4));

    // Unicode "Hi!" (big-endian UTF-16) to ANSI with allocation.
    const uint32_t wide = Scratch() + 0x400;
    const uint16_t chars[] = { 'H', 'i', '!', 0 };
    for (int i = 0; i < 4; i++)
        *guest::Translate<be<uint16_t>>(wide + i * 2) = chars[i];
    const uint32_t unicode = Scratch() + 0x500;
    call(__imp__RtlInitUnicodeString, unicode, wide);
    CHECK(guest::LoadU32(unicode) == 0x00060008, "UNICODE_STRING lengths %08X", guest::LoadU32(unicode));
    CHECK(call(__imp__RtlUnicodeStringToAnsiString, ansi, unicode, 1) == status::kSuccess, "convert");
    const char* converted = guest::Translate<char>(guest::LoadU32(ansi + 4));
    CHECK(converted && strcmp(converted, "Hi!") == 0 && guest::LoadU32(ansi) >> 16 == 3, "converted '%s'", converted ? converted : "(null)");
    call(__imp__RtlFreeAnsiString, ansi);
    CHECK(guest::LoadU32(ansi + 4) == 0, "freed buffer cleared");
}

// vswprintf keeps UTF-16 outside ASCII, in the format (DOAX2 formats its money after its own currency sign), in wide
// %s strings and in %c, and still formats numbers and narrow (%hs) strings.
static void test_wide_printf()
{
    Call call;
    const uint32_t memory = guest::SystemAlloc(0x400);
    const auto storeWide = [](uint32_t address, const std::u16string& text) {
        for (size_t i = 0; i <= text.size(); i++)
            *guest::Translate<be<uint16_t>>(address + uint32_t(i) * 2) = i < text.size() ? uint16_t(text[i]) : 0;
        return address;
    };
    const uint32_t format = storeWide(memory, u"%d %s|%hs|%c|%5d");
    const uint32_t wideArgument = storeWide(memory + 0x80, u"Zéro");
    const uint32_t narrowArgument = StoreAnsi(memory + 0xC0, "abc");
    const uint32_t arguments = memory + 0x100, out = memory + 0x200;
    const uint64_t slots[] = { 300000, wideArgument, narrowArgument, 0x3042, 42 };
    for (size_t i = 0; i < std::size(slots); i++)
        *guest::Translate<be<uint64_t>>(arguments + uint32_t(i) * 8) = slots[i];
    const uint32_t length = uint32_t(call(__imp__vswprintf, out, format, arguments));
    const std::u16string expected = u"" u"300000 Zéro|abc|あ|   42";
    std::u16string text;
    for (uint32_t i = 0; i < 64 && *guest::Translate<be<uint16_t>>(out + i * 2) != 0; i++)
        text.push_back(char16_t(uint16_t(*guest::Translate<be<uint16_t>>(out + i * 2))));
    CHECK(text == expected && length == expected.size(), "%zu units (returned %u), first %04X", text.size(), length, text.empty() ? 0u : uint32_t(text[0]));
}

static void test_time()
{
    Call call;
    const uint32_t time = Scratch() + 0x600, fields = Scratch() + 0x610, back = Scratch() + 0x620;
    *guest::Translate<be<uint64_t>>(time) = 0x01C60C7E21ECFDB0ull; // 2005-12-29 13:45:30.123 (Thursday)
    call(__imp__RtlTimeToTimeFields, time, fields);
    const auto* f = guest::Translate<be<uint16_t>>(fields);
    CHECK(f[0] == 2005 && f[1] == 12 && f[2] == 29 && f[3] == 13 && f[4] == 45 && f[5] == 30 && f[6] == 123 && f[7] == 4,
        "%u-%u-%u %u:%u:%u.%u wd%u", uint16_t(f[0]), uint16_t(f[1]), uint16_t(f[2]), uint16_t(f[3]), uint16_t(f[4]), uint16_t(f[5]), uint16_t(f[6]), uint16_t(f[7]));
    CHECK(call(__imp__RtlTimeFieldsToTime, fields, back) == 1 && *guest::Translate<be<uint64_t>>(back) == 0x01C60C7E21ECFDB0ull, "round trip");

    // KeQueryPerformanceFrequency returns a 64-bit value in r3.
    call(__imp__KeQueryPerformanceFrequency);
    CHECK(call.ctx->r3.u64 == 50000000, "frequency %llu", (unsigned long long)call.ctx->r3.u64);

    const uint64_t start = DOA4QueryTimeBase();
    Sleep(100);
    const uint64_t ticks = DOA4QueryTimeBase() - start;
    CHECK(ticks > 4500000 && ticks < 6500000, "100 ms at 50 MHz = %llu ticks", (unsigned long long)ticks);
}

static void test_memory_ulong()
{
    Call call;
    const uint32_t buffer = Scratch() + 0x700;
    call(__imp__RtlFillMemoryUlong, buffer, 16, 0xDEADBEEF);
    CHECK(guest::ToHost(buffer)[0] == 0xDE && guest::LoadU32(buffer + 12) == 0xDEADBEEF, "fill is big-endian");
    guest::StoreU32(buffer + 8, 0);
    CHECK(call(__imp__RtlCompareMemoryUlong, buffer, 16, 0xDEADBEEF) == 8, "matches until the first difference");
}

static void test_module()
{
    Call call;
    imports::ResolveSlots();
    if (!kernel::InitializeExecutableModule("private/default.xex", uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_SIZE)))
    {
        printf("skipping module tests: private/default.xex not found (run from the repo root)\n");
        return;
    }

    const uint32_t handleVariable = imports::VariableAddress("XexExecutableModuleHandle");
    const uint32_t entry = guest::LoadU32(handleVariable);
    const uint32_t header = guest::LoadU32(entry + 0x58);
    CHECK(header == kernel::ExecutableXexHeader() && guest::LoadU32(header) == 0x58455832, "handle -> entry -> XEX2 header");
    CHECK(guest::LoadU32(entry + 0x3C) == 0x82784C18, "entry point in loader entry");

    CHECK(call(__imp__RtlImageXexHeaderField, header, 0x00010100) == 0x82784C18, "inline value (entry point)");
    const uint32_t execution = call(__imp__RtlImageXexHeaderField, header, 0x00040006);
    CHECK(execution && guest::LoadU32(execution + 12) == 0x544307D2, "execution info -> title id %08X", execution ? guest::LoadU32(execution + 12) : 0);
    CHECK(call(__imp__RtlImageXexHeaderField, header, 0x00020401) == 0, "absent header (default heap size)");

    CHECK(call(__imp__XexCheckExecutablePrivilege, 9) == 1, "system flags 0x200 -> privilege 9");
    CHECK(call(__imp__XexCheckExecutablePrivilege, 6) == 0, "privilege 6 not set");
}

// ---------------------------------------------------------------------------------------------
// Synchronization and threads. Host functions registered in the lookup table act as guest code.

static constexpr uint32_t kFakeCode = uint32_t(PPC_CODE_BASE); // inside PPC_CODE range, so the lookup table covers it

static uint32_t RegisterGuestFunction(uint32_t index, PPCFunc* function)
{
    const uint32_t address = kFakeCode + index * 4;
    PPC_LOOKUP_FUNC(guest::g_base, address) = function;
    return address;
}

static constexpr int64_t kNoWait = 0;

static uint32_t TimeoutPtr(int64_t value)
{
    const uint32_t address = Scratch() + 0x800;
    *guest::Translate<be<int64_t>>(address) = value;
    return address;
}

static void test_events()
{
    Call call;
    const uint32_t out = Scratch() + 0x810;
    CHECK(call(__imp__NtCreateEvent, out, 0, 1 /* synchronization */, 0) == status::kSuccess, "create sync event");
    const uint32_t sync = guest::LoadU32(out);
    CHECK(call(__imp__NtCreateEvent, out, 0, 0 /* notification */, 1) == status::kSuccess, "create notification event");
    const uint32_t manual = guest::LoadU32(out);

    CHECK(call(__imp__NtWaitForSingleObjectEx, sync, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "unsignaled poll times out");
    call(__imp__NtSetEvent, sync, 0);
    CHECK(call(__imp__NtWaitForSingleObjectEx, sync, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess, "signaled");
    CHECK(call(__imp__NtWaitForSingleObjectEx, sync, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "auto-reset after one wait");

    CHECK(call(__imp__NtWaitForSingleObjectEx, manual, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess &&
          call(__imp__NtWaitForSingleObjectEx, manual, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess, "manual reset stays signaled");

    // Wait any returns the index of the first signaled object; wait all needs every one.
    const uint32_t handles = Scratch() + 0x820;
    guest::StoreU32(handles, sync);
    guest::StoreU32(handles + 4, manual);
    CHECK(call(__imp__NtWaitForMultipleObjectsEx, 2, handles, 1, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess + 1, "wait any -> index 1");
    CHECK(call(__imp__NtWaitForMultipleObjectsEx, 2, handles, 0, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "wait all needs both");

    const auto start = GetTickCount64();
    CHECK(call(__imp__NtWaitForSingleObjectEx, sync, 0, 0, TimeoutPtr(-500000 /* 50 ms */)) == status::kTimeout, "relative timeout");
    const auto elapsed = GetTickCount64() - start;
    CHECK(elapsed >= 40 && elapsed < 500, "waited %llu ms", (unsigned long long)elapsed);

    CHECK(call(__imp__NtClose, sync) == status::kSuccess && call(__imp__NtClose, sync) == status::kInvalidHandle, "close once");
    call(__imp__NtClose, manual);
}

// Guest thread bodies.
static std::atomic<int> g_threadRuns{ 0 };

static void ThreadReturnsContextPlusOne(PPCContext& ctx, uint8_t*)
{
    g_threadRuns++;
    // r13 points at this thread's PCR, whose current-thread field is its KTHREAD.
    const uint32_t kthread = guest::LoadU32(ctx.r13.u32 + 0x100);
    guest::StoreU32(Scratch() + 0x900, kthread == kernel::CurrentKThread() ? 1 : 0);
    guest::StoreU32(Scratch() + 0x904, guest::LoadU32(kthread + 0x14C)); // thread id
    ctx.r3.u64 = ctx.r3.u64 + 1;
}

static void XapiStartupCallsStart(PPCContext& ctx, uint8_t* base)
{
    // xapiStartup(start, context): call start(context), then exit with its result.
    const uint32_t start = ctx.r3.u32;
    ctx.r3.u64 = ctx.r4.u64;
    (PPC_LOOKUP_FUNC(base, start))(ctx, base);
}

static void ThreadTerminatesEarly(PPCContext& ctx, uint8_t* base)
{
    ctx.r3.u64 = 0x1234;
    __imp__ExTerminateThread(ctx, base);
    ctx.r3.u64 = 0xBAD; // unreachable
}

static uint32_t g_section = 0, g_counter = 0;

static void ThreadIncrementsUnderLock(PPCContext& ctx, uint8_t* base)
{
    for (int i = 0; i < 50000; i++)
    {
        ctx.r3.u64 = g_section;
        __imp__RtlEnterCriticalSection(ctx, base);
        guest::StoreU32(g_counter, guest::LoadU32(g_counter) + 1); // non-atomic on purpose
        ctx.r3.u64 = g_section;
        __imp__RtlLeaveCriticalSection(ctx, base);
    }
    ctx.r3.u64 = 0;
}

static void ThreadUsesTls(PPCContext& ctx, uint8_t* base)
{
    const uint32_t slot = ctx.r3.u32;
    ctx.r3.u64 = slot;
    ctx.r4.u64 = 0x7777;
    __imp__KeTlsSetValue(ctx, base);
    ctx.r3.u64 = slot;
    __imp__KeTlsGetValue(ctx, base);
}

static uint32_t WaitForThread(Call& call, uint32_t handle)
{
    return call(__imp__NtWaitForSingleObjectEx, handle, 0, 0, TimeoutPtr(-50000000 /* 5 s */));
}

static uint32_t ThreadExitCode(uint32_t handle)
{
    auto thread = kernel::LookupHandleAs<kernel::GuestThread>(handle);
    return thread ? thread->ExitCode() : 0xFFFFFFFF;
}

static void test_threads()
{
    Call call;
    const uint32_t handlePtr = Scratch() + 0x830, idPtr = Scratch() + 0x834;
    const uint32_t body = RegisterGuestFunction(0, ThreadReturnsContextPlusOne);
    const uint32_t startup = RegisterGuestFunction(1, XapiStartupCallsStart);

    // Plain thread: start(context).
    CHECK(call(__imp__ExCreateThread, handlePtr, 0, idPtr, 0, body, 41, 0) == status::kSuccess, "create");
    uint32_t handle = guest::LoadU32(handlePtr);
    CHECK(WaitForThread(call, handle) == status::kSuccess, "thread handle signaled on exit");
    CHECK(ThreadExitCode(handle) == 42, "exit code %u", ThreadExitCode(handle));
    CHECK(guest::LoadU32(Scratch() + 0x900) == 1, "PCR current thread is the thread's KTHREAD");
    CHECK(guest::LoadU32(Scratch() + 0x904) == guest::LoadU32(idPtr), "KTHREAD thread id matches ExCreateThread's");
    call(__imp__NtClose, handle);

    // Through the XAPI startup routine, with an affinity byte in the flags.
    CHECK(call(__imp__ExCreateThread, handlePtr, 0x10000, idPtr, startup, body, 99, 0x04000000) == status::kSuccess, "create via startup");
    handle = guest::LoadU32(handlePtr);
    CHECK(WaitForThread(call, handle) == status::kSuccess && ThreadExitCode(handle) == 100, "startup path exit %u", ThreadExitCode(handle));
    call(__imp__NtClose, handle);

    // Created suspended: doesn't run until resumed.
    const int runsBefore = g_threadRuns;
    CHECK(call(__imp__ExCreateThread, handlePtr, 0, idPtr, 0, body, 1, kernel::kCreateSuspended) == status::kSuccess, "create suspended");
    handle = guest::LoadU32(handlePtr);
    Sleep(50);
    CHECK(g_threadRuns == runsBefore, "suspended thread didn't run");
    CHECK(call(__imp__NtWaitForSingleObjectEx, handle, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "not finished");
    const uint32_t countPtr = Scratch() + 0x838;
    CHECK(call(__imp__NtResumeThread, handle, countPtr) == status::kSuccess && guest::LoadU32(countPtr) == 1, "resume (previous count 1)");
    CHECK(WaitForThread(call, handle) == status::kSuccess && g_threadRuns == runsBefore + 1, "ran after resume");
    call(__imp__NtClose, handle);

    // The same through its KTHREAD.
    CHECK(call(__imp__ExCreateThread, handlePtr, 0, idPtr, 0, body, 1, kernel::kCreateSuspended) == status::kSuccess, "create suspended");
    handle = guest::LoadU32(handlePtr);
    CHECK(call(__imp__ObReferenceObjectByHandle, handle, 0, countPtr) == status::kSuccess, "KTHREAD of the handle");
    const uint32_t kthread = guest::LoadU32(countPtr);
    CHECK(call(__imp__KeResumeThread, kthread) == 1, "KeResumeThread: previous count 1");
    CHECK(WaitForThread(call, handle) == status::kSuccess && g_threadRuns == runsBefore + 2, "ran after KeResumeThread");
    call(__imp__ObDereferenceObject, kthread);
    call(__imp__NtClose, handle);

    // ExTerminateThread unwinds out of the thread with its exit code.
    const uint32_t terminator = RegisterGuestFunction(2, ThreadTerminatesEarly);
    call(__imp__ExCreateThread, handlePtr, 0, idPtr, 0, terminator, 0, 0);
    handle = guest::LoadU32(handlePtr);
    CHECK(WaitForThread(call, handle) == status::kSuccess && ThreadExitCode(handle) == 0x1234, "ExTerminateThread exit 0x%X", ThreadExitCode(handle));
    call(__imp__NtClose, handle);

    // TLS values are per thread.
    const uint32_t slot = call(__imp__KeTlsAlloc);
    CHECK(slot < kernel::kTlsSlotCount, "TLS slot %u", slot);
    call(__imp__KeTlsSetValue, slot, 0x1111);
    const uint32_t tlsBody = RegisterGuestFunction(3, ThreadUsesTls);
    call(__imp__ExCreateThread, handlePtr, 0, idPtr, 0, tlsBody, slot, 0);
    handle = guest::LoadU32(handlePtr);
    CHECK(WaitForThread(call, handle) == status::kSuccess && ThreadExitCode(handle) == 0x7777, "thread sees its own TLS value");
    CHECK(call(__imp__KeTlsGetValue, slot) == 0x1111, "caller's TLS value unchanged");
    call(__imp__KeTlsFree, slot);
    call(__imp__NtClose, handle);
}

static void test_critical_section_contention()
{
    Call call;
    g_section = guest::KernelAlloc(0x1C);
    g_counter = guest::KernelAlloc(4);
    call(__imp__RtlInitializeCriticalSection, g_section);
    CHECK(int32_t(guest::LoadU32(g_section + 0x10)) == -1, "lock count starts at -1");

    const uint32_t body = RegisterGuestFunction(4, ThreadIncrementsUnderLock);
    uint32_t handles[4];
    for (uint32_t& handle : handles)
    {
        call(__imp__ExCreateThread, Scratch() + 0x840, 0, 0, 0, body, 0, 0);
        handle = guest::LoadU32(Scratch() + 0x840);
    }
    for (uint32_t handle : handles)
    {
        CHECK(WaitForThread(call, handle) == status::kSuccess, "worker finished");
        call(__imp__NtClose, handle);
    }
    CHECK(guest::LoadU32(g_counter) == 4 * 50000, "counter %u (lost updates mean the lock failed)", guest::LoadU32(g_counter));
    CHECK(int32_t(guest::LoadU32(g_section + 0x10)) == -1 && guest::LoadU32(g_section + 0x18) == 0, "released: lock count -1, no owner");

    // Recursion on one thread.
    call(__imp__RtlEnterCriticalSection, g_section);
    call(__imp__RtlEnterCriticalSection, g_section);
    CHECK(int32_t(guest::LoadU32(g_section + 0x14)) == 2, "recursion count 2");
    call(__imp__RtlLeaveCriticalSection, g_section);
    call(__imp__RtlLeaveCriticalSection, g_section);
    CHECK(int32_t(guest::LoadU32(g_section + 0x10)) == -1, "fully released");
    CHECK(call(__imp__RtlTryEnterCriticalSection, g_section) == 1, "try-enter when free");
    call(__imp__RtlLeaveCriticalSection, g_section);
}

static uint32_t g_pulseEvent = 0;
static std::atomic<int> g_pulseWoken{ 0 };

static void ThreadWaitsForPulse(PPCContext& ctx, uint8_t* base)
{
    ctx.r3.u64 = g_pulseEvent;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = TimeoutPtr(-20000000); // 2 s
    __imp__NtWaitForSingleObjectEx(ctx, base);
    if (ctx.r3.u32 == status::kSuccess)
        g_pulseWoken++;
}

// The runtime implements it, but games that don't import it (DOAX2) don't have it in the generated declarations.
PPC_EXTERN_FUNC(__imp__KeTryToAcquireSpinLockAtRaisedIrql);

static void test_pulse_and_spinlock()
{
    Call call;
    call(__imp__NtCreateEvent, Scratch() + 0x850, 0, 0 /* notification */, 0);
    g_pulseEvent = guest::LoadU32(Scratch() + 0x850);
    const uint32_t body = RegisterGuestFunction(5, ThreadWaitsForPulse);
    uint32_t handles[2];
    for (uint32_t& handle : handles)
    {
        call(__imp__ExCreateThread, Scratch() + 0x854, 0, 0, 0, body, 0, 0);
        handle = guest::LoadU32(Scratch() + 0x854);
    }
    Sleep(100); // let both start waiting
    call(__imp__NtPulseEvent, g_pulseEvent, 0);
    for (uint32_t handle : handles)
    {
        WaitForThread(call, handle);
        call(__imp__NtClose, handle);
    }
    CHECK(g_pulseWoken == 2, "pulse woke %d of 2 waiters", g_pulseWoken.load());
    CHECK(call(__imp__NtWaitForSingleObjectEx, g_pulseEvent, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "event non-signaled after pulse");
    call(__imp__NtClose, g_pulseEvent);

    const uint32_t lock = guest::KernelAlloc(4);
    const uint32_t irql = call(__imp__KfAcquireSpinLock, lock);
    CHECK(guest::LoadU32(lock) != 0, "spinlock held");
    CHECK(call(__imp__KeTryToAcquireSpinLockAtRaisedIrql, lock) == 0, "try-acquire fails while held");
    call(__imp__KfReleaseSpinLock, lock, irql);
    CHECK(guest::LoadU32(lock) == 0 && call(__imp__KeTryToAcquireSpinLockAtRaisedIrql, lock) == 1, "released and re-acquirable");
}

// A thread that suspends itself, as CRI's server thread does at the end of each pass (NtSuspendThread on itself), and one
// that resumes it, as CRI's vsync thread does at each vsync: a resume that comes as the thread suspends itself mustn't be
// lost. The suspending thread counts its rounds; the test resumes it as fast as it can until it has done them all.
PPC_EXTERN_FUNC(__imp__NtSuspendThread); // (not in the generated declarations of a game that doesn't import it)

static std::atomic<uint32_t> g_selfSuspensions{ 0 };
static std::atomic<bool> g_spinnerStop{ false };
static std::atomic<uint64_t> g_spins{ 0 };
static constexpr uint32_t kSelfSuspensionRounds = 20000;

static void ThreadSuspendsItself(PPCContext& ctx, uint8_t* base)
{
    for (uint32_t round = 0; round < kSelfSuspensionRounds; round++)
    {
        ctx.r3.u64 = kernel::kCurrentThreadHandle;
        ctx.r4.u64 = 0;
        __imp__NtSuspendThread(ctx, base);
        g_selfSuspensions++;
    }
}

static void ThreadSpins(PPCContext&, uint8_t*)
{
    while (!g_spinnerStop)
        g_spins++;
}

static void test_suspension()
{
    Call call;
    const uint32_t handlePtr = Scratch() + 0x860, countPtr = Scratch() + 0x864;
    const uint32_t body = RegisterGuestFunction(6, ThreadSuspendsItself);
    CHECK(call(__imp__ExCreateThread, handlePtr, 0, 0, 0, body, 0, 0) == status::kSuccess, "create the self-suspending thread");
    uint32_t handle = guest::LoadU32(handlePtr);
    // A resume lost as the thread suspends itself would leave it suspended for good, which the 10 s ends.
    uint32_t mostPrevious = 0;
    for (const uint64_t deadline = GetTickCount64() + 10000; g_selfSuspensions < kSelfSuspensionRounds && GetTickCount64() < deadline;)
    {
        call(__imp__NtResumeThread, handle, countPtr);
        mostPrevious = std::max(mostPrevious, guest::LoadU32(countPtr));
    }
    const uint32_t rounds = g_selfSuspensions;
    CHECK(rounds == kSelfSuspensionRounds, "the thread got through %u of its %u self-suspensions before a resume was lost", rounds,
        kSelfSuspensionRounds);
    CHECK(mostPrevious <= 1, "a resume found the suspend count at %u (at most 1: a resume of a running thread does nothing)", mostPrevious);
    if (rounds == kSelfSuspensionRounds)
        CHECK(WaitForThread(call, handle) == status::kSuccess, "the thread finished");
    call(__imp__NtClose, handle);

    // Another thread suspended and resumed: it stops, then goes on.
    const uint32_t spinner = RegisterGuestFunction(7, ThreadSpins);
    CHECK(call(__imp__ExCreateThread, handlePtr, 0, 0, 0, spinner, 0, 0) == status::kSuccess, "create the spinning thread");
    handle = guest::LoadU32(handlePtr);
    for (const uint64_t started = GetTickCount64() + 5000; g_spins == 0 && GetTickCount64() < started;)
        Sleep(1);
    CHECK(g_spins != 0, "the spinning thread runs");
    CHECK(call(__imp__NtSuspendThread, handle, countPtr) == status::kSuccess && guest::LoadU32(countPtr) == 0, "suspend (previous count 0)");
    CHECK(call(__imp__NtSuspendThread, handle, countPtr) == status::kSuccess && guest::LoadU32(countPtr) == 1, "suspend again (previous count 1)");
    Sleep(20);
    const uint64_t spinsSuspended = g_spins;
    Sleep(50);
    CHECK(g_spins == spinsSuspended, "the suspended thread stopped (%llu spins, then %llu)", (unsigned long long)spinsSuspended,
        (unsigned long long)g_spins.load());
    CHECK(call(__imp__NtResumeThread, handle, countPtr) == status::kSuccess && guest::LoadU32(countPtr) == 2, "resume (previous count 2)");
    Sleep(50);
    CHECK(g_spins == spinsSuspended, "still suspended at count 1");
    CHECK(call(__imp__NtResumeThread, handle, countPtr) == status::kSuccess && guest::LoadU32(countPtr) == 1, "resume (previous count 1)");
    const uint64_t deadline = GetTickCount64() + 5000;
    while (g_spins == spinsSuspended && GetTickCount64() < deadline)
        Sleep(1);
    CHECK(g_spins != spinsSuspended, "the resumed thread runs again");
    CHECK(call(__imp__NtResumeThread, handle, countPtr) == status::kSuccess && guest::LoadU32(countPtr) == 0, "resume of a running thread (previous count 0)");
    g_spinnerStop = true;
    CHECK(WaitForThread(call, handle) == status::kSuccess, "the spinning thread finished");
    call(__imp__NtClose, handle);
}

// Semaphores: each wait takes one, releases can't pass the limit; by handle, and in guest memory (KSEMAPHORE) waited
// for together with an event.
static void test_semaphores()
{
    Call call;
    const uint32_t out = Scratch() + 0x860, previous = Scratch() + 0x864;
    CHECK(call(__imp__NtCreateSemaphore, out, 0, 1, 2) == status::kSuccess, "create (count 1, limit 2)");
    const uint32_t semaphore = guest::LoadU32(out);
    CHECK(call(__imp__NtWaitForSingleObjectEx, semaphore, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess, "takes the one");
    CHECK(call(__imp__NtWaitForSingleObjectEx, semaphore, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "none left");
    CHECK(call(__imp__NtReleaseSemaphore, semaphore, 2, previous) == status::kSuccess && guest::LoadU32(previous) == 0, "release 2 (previous 0)");
    CHECK(call(__imp__NtReleaseSemaphore, semaphore, 1, previous) == 0xC0000047, "past the limit");
    CHECK(call(__imp__NtWaitForSingleObjectEx, semaphore, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess &&
          call(__imp__NtWaitForSingleObjectEx, semaphore, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess &&
          call(__imp__NtWaitForSingleObjectEx, semaphore, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "two, then none");
    CHECK(call(__imp__NtCreateSemaphore, out, 0, 3, 2) == status::kInvalidParameter, "count above the limit");
    call(__imp__NtClose, semaphore);

    const uint32_t ksemaphore = guest::KernelAlloc(0x14);
    call(__imp__KeInitializeSemaphore, ksemaphore, 0, 5);
    CHECK(guest::ToHost(ksemaphore)[0] == 5 && guest::LoadU32(ksemaphore + 4) == 0 && guest::LoadU32(ksemaphore + 0x10) == 5, "type 5, count 0, limit 5");
    call(__imp__NtCreateEvent, out, 0, 0 /* notification */, 1);
    const uint32_t event = guest::LoadU32(out);
    CHECK(call(__imp__ObReferenceObjectByHandle, event, 0, out) == status::kSuccess, "event's KEVENT");
    const uint32_t objects = Scratch() + 0x870;
    guest::StoreU32(objects, ksemaphore);
    guest::StoreU32(objects + 4, guest::LoadU32(out));
    // KeWaitForMultipleObjects(count, objects, waitType (0 all, 1 any), reason, mode, alertable, timeout, wait blocks).
    CHECK(call(__imp__KeWaitForMultipleObjects, 2, objects, 1, 0, 0, 0, TimeoutPtr(kNoWait), 0) == status::kSuccess + 1, "any: the event");
    CHECK(call(__imp__KeWaitForMultipleObjects, 2, objects, 0, 0, 0, 0, TimeoutPtr(kNoWait), 0) == status::kTimeout, "all: needs the semaphore");
    CHECK(int32_t(call(__imp__KeReleaseSemaphore, ksemaphore, 0, 3, 0)) == 0, "KeReleaseSemaphore returns the previous count");
    CHECK(call(__imp__KeWaitForMultipleObjects, 2, objects, 0, 0, 0, 0, TimeoutPtr(kNoWait), 0) == status::kSuccess, "all");
    CHECK(guest::LoadU32(ksemaphore + 4) == 2, "the wait took one of 3");
    CHECK(int32_t(call(__imp__KeReleaseSemaphore, ksemaphore, 0, 4, 0)) == 2 && guest::LoadU32(ksemaphore + 4) == 2, "past the limit: unchanged");
    call(__imp__ObDereferenceObject, guest::LoadU32(objects + 4));
    call(__imp__NtClose, event);
}

// Hard disk partitions: the partition itself (named without a trailing backslash) is its raw sectors, which read back
// what was written and zeros elsewhere and answer the disk queries; a FATX superblock written there sets the cluster
// size its file system reports; and an STFS device over it (XAPI's cache:) is its file system under another name.
static void test_hard_disk_partition()
{
    const std::string folder = "build/test_hdd";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    vfs::Mount("\\Device\\Harddisk0\\TestCache", vfs::OpenHardDiskPartition("test cache", folder + "/TestCache", folder + "/TestCache.raw", { 0x80000, 0x80000000 }));
    vfs::Mount("\\Device\\Harddisk0\\TestDisk", vfs::OpenHardDiskPartition("test disk", "", folder + "/TestDisk.raw", { 0, 20ull << 30 }));

    Call call;
    const uint32_t base = Scratch() + 0xA00;
    const uint32_t name = base, attributes = base + 0x10, iosb = base + 0x20, handlePtr = base + 0x28, offset = base + 0x30;
    const uint32_t text = base + 0x40, buffer = base + 0x100, stack = base + 0x200;
    // ANSI_STRING and OBJECT_ATTRIBUTES (root, name, attributes) for a path.
    const auto setName = [&](const char* path) {
        strcpy(reinterpret_cast<char*>(guest::ToHost(text)), path);
        *guest::Translate<be<uint16_t>>(name) = uint16_t(strlen(path));
        *guest::Translate<be<uint16_t>>(name + 2) = uint16_t(strlen(path) + 1);
        guest::StoreU32(name + 4, text);
        guest::StoreU32(attributes, 0);
        guest::StoreU32(attributes + 4, name);
        guest::StoreU32(attributes + 8, 0x40);
    };
    // NtOpenFile(&handle, access, attributes, iosb, share, options): the handle, or 0.
    const auto open = [&](const char* path, uint32_t options, uint32_t* result = nullptr) -> uint32_t {
        setName(path);
        const uint32_t status = call(__imp__NtOpenFile, handlePtr, 0xC0100000, attributes, iosb, 3, options);
        if (result)
            *result = status;
        return status == status::kSuccess ? guest::LoadU32(handlePtr) : 0;
    };
    const auto at = [&](uint64_t value) {
        *guest::Translate<be<uint64_t>>(offset) = value;
        return offset;
    };

    uint32_t raw = open("\\Device\\Harddisk0\\TestCache", 0x20);
    CHECK(raw != 0, "raw partition");
    memset(guest::ToHost(buffer), 0xCC, 16);
    CHECK(call(__imp__NtReadFile, raw, 0, 0, 0, iosb, buffer, 16, at(2048)) == status::kSuccess && guest::LoadU32(iosb + 4) == 16, "read");
    CHECK(guest::LoadU32(buffer) == 0 && guest::LoadU32(buffer + 12) == 0, "never written: zeros");
    memcpy(guest::ToHost(buffer), "Josh", 4);
    CHECK(call(__imp__NtWriteFile, raw, 0, 0, 0, iosb, buffer, 4, at(2048)) == status::kSuccess, "write");
    call(__imp__NtClose, raw);
    raw = open("\\Device\\Harddisk0\\TestCache", 0x20);
    memset(guest::ToHost(buffer), 0, 16);
    call(__imp__NtReadFile, raw, 0, 0, 0, iosb, buffer, 8, at(2046));
    CHECK(memcmp(guest::ToHost(buffer), "\0\0Josh\0\0", 8) == 0, "reads back what was written");

    // NtDeviceIoControlFile(handle, event, apc, apcContext, iosb, code, input, inputLength, output, outputLength): the
    // last two are on the stack.
    call.ctx->r1.u64 = stack;
    guest::StoreU32(stack + 0x54, buffer);
    guest::StoreU32(stack + 0x5C, 16);
    CHECK(call(__imp__NtDeviceIoControlFile, raw, 0, 0, 0, iosb, 0x70000, 0, 0) == status::kSuccess, "drive geometry");
    CHECK(guest::LoadU32(buffer) == 0x80000000 / 512 && guest::LoadU32(buffer + 4) == 512, "%08X sectors of %u bytes", guest::LoadU32(buffer), guest::LoadU32(buffer + 4));
    CHECK(call(__imp__NtDeviceIoControlFile, raw, 0, 0, 0, iosb, 0x74004, 0, 0) == status::kSuccess, "partition info");
    CHECK(*guest::Translate<be<uint64_t>>(buffer) == 0x80000 && *guest::Translate<be<uint64_t>>(buffer + 8) == 0x80000000, "offset and length");

    // FileFsSizeInformation of the file system: { total units, free units, sectors per unit, bytes per sector }.
    uint32_t root = open("\\Device\\Harddisk0\\TestCache\\", 0x21);
    CHECK(root != 0, "file system root");
    CHECK(call(__imp__NtQueryVolumeInformationFile, root, iosb, buffer, 24, 3) == status::kSuccess, "size information");
    CHECK(guest::LoadU32(buffer + 0x10) * guest::LoadU32(buffer + 0x14) == 0x4000, "16 KB clusters before a format");
    CHECK(*guest::Translate<be<uint64_t>>(buffer) == 0x80000000 / 0x4000, "the partition's size in clusters");
    call(__imp__NtClose, root);
    memset(guest::ToHost(buffer), 0, 16);
    memcpy(guest::ToHost(buffer), "XTAF", 4);
    guest::StoreU32(buffer + 8, 128); // sectors per cluster
    call(__imp__NtWriteFile, raw, 0, 0, 0, iosb, buffer, 16, at(0));
    root = open("\\Device\\Harddisk0\\TestCache\\", 0x21);
    call(__imp__NtQueryVolumeInformationFile, root, iosb, buffer, 24, 3);
    CHECK(guest::LoadU32(buffer + 0x10) == 128 && guest::LoadU32(buffer + 0x14) == 512, "formatted with 64 KB clusters");
    call(__imp__NtClose, root);

    // StfsCreateDevice(parameters, size): device name at 0, the raw partition's handle at 0x18; returns the device at 0x48.
    const uint32_t parameters = base + 0x300, stfsName = base + 0x360;
    memset(guest::ToHost(parameters), 0, 0x58);
    strcpy(reinterpret_cast<char*>(guest::ToHost(stfsName)), "\\Device\\testcache0");
    *guest::Translate<be<uint16_t>>(parameters) = uint16_t(strlen("\\Device\\testcache0"));
    guest::StoreU32(parameters + 4, stfsName);
    guest::StoreU32(parameters + 0x18, raw);
    CHECK(call(__imp__StfsCreateDevice, parameters, 0x58) == status::kSuccess && guest::LoadU32(parameters + 0x48) != 0, "STFS device");
    setName("\\Device\\testcache0\\file.bin");
    guest::StoreU32(stack + 0x54, 0x60); // create options: a synchronous non-directory file
    CHECK(call(__imp__NtCreateFile, handlePtr, 0xC0100000, attributes, iosb, 0, 0x80, 0, 2 /* create */) == status::kSuccess, "a file on it");
    call(__imp__NtClose, guest::LoadU32(handlePtr));
    CHECK(std::filesystem::exists(folder + "/TestCache/file.bin", error), "is the partition's host file");

    // Its times can be set (FileBasicInformation: creation, access, write, change; 0 leaves one), through a read-only
    // handle too, as DOAX2 sets those of the files it copies; FileNetworkOpenInformation reads them back.
    const uint32_t file = open("\\Device\\testcache0\\file.bin", 0x60);
    CHECK(file != 0, "reopened read-only");
    const uint64_t mastered = 0x01C6F5F92CCA38F0ull; // 2006-10-22
    memset(guest::ToHost(buffer), 0, 0x38);
    *guest::Translate<be<uint64_t>>(buffer) = mastered;
    *guest::Translate<be<uint64_t>>(buffer + 16) = mastered + 10000000; // a second later
    CHECK(call(__imp__NtSetInformationFile, file, iosb, buffer, 0x28, 4) == status::kSuccess, "set creation and write times");
    memset(guest::ToHost(buffer), 0, 0x38);
    CHECK(call(__imp__NtQueryInformationFile, file, iosb, buffer, 0x38, 34) == status::kSuccess, "network open information");
    CHECK(*guest::Translate<be<uint64_t>>(buffer) == mastered && *guest::Translate<be<uint64_t>>(buffer + 16) == mastered + 10000000,
        "creation %016llX, write %016llX", (unsigned long long)*guest::Translate<be<uint64_t>>(buffer),
        (unsigned long long)*guest::Translate<be<uint64_t>>(buffer + 16));
    CHECK(*guest::Translate<be<uint64_t>>(buffer + 8) > mastered + 10000000, "access time unchanged (0)");
    call(__imp__NtClose, file);
    call(__imp__NtClose, raw);

    // Partition0, the whole disk, has no file system.
    raw = open("\\Device\\Harddisk0\\TestDisk", 0x20);
    CHECK(raw != 0, "raw disk");
    call(__imp__NtClose, raw);
    uint32_t result = 0;
    CHECK(open("\\Device\\Harddisk0\\TestDisk\\", 0x21, &result) == 0 && result == 0xC000014F, "no file system: 0x%08X", result);

    vfs::RemoveSymbolicLink("\\Device\\testcache0");
    vfs::Unmount("\\Device\\Harddisk0\\TestCache");
    vfs::Unmount("\\Device\\Harddisk0\\TestDisk");
}

// SHA-1, and console signatures that verify exactly what was signed.
static void test_crypto()
{
    Call call;
    const uint32_t data = Scratch() + 0xB00, digest = Scratch() + 0xB40, valid = Scratch() + 0xB60, signature = Scratch() + 0xC00;
    memcpy(guest::ToHost(data), "abc", 3);
    static const uint8_t kAbc[20] = { 0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E, 0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D };
    // XeCryptSha(input1, size1, input2, size2, input3, size3, digest, digestSize).
    call(__imp__XeCryptSha, data, 3, 0, 0, 0, 0, digest, 20);
    CHECK(memcmp(guest::ToHost(digest), kAbc, 20) == 0, "SHA-1 of \"abc\"");
    memset(guest::ToHost(digest), 0, 20);
    call(__imp__XeCryptSha, data, 1, data + 1, 1, data + 2, 1, digest, 20);
    CHECK(memcmp(guest::ToHost(digest), kAbc, 20) == 0, "the same in three parts");

    CHECK(call(__imp__XeKeysConsolePrivateKeySign, digest, signature) == 1, "sign");
    CHECK(call(__imp__XeKeysConsoleSignatureVerification, digest, signature, valid) == 1 && guest::LoadU32(valid) == 1, "verifies");
    guest::ToHost(signature + 0x1A8)[3] ^= 1;
    CHECK(call(__imp__XeKeysConsoleSignatureVerification, digest, signature, valid) == 1 && guest::LoadU32(valid) == 0, "a changed signature doesn't");
}

// An enumerator an XAM app serves, as DOAX2's XMP library makes one for the user's playlists: its private structure (a
// 24-byte header, then the bytes the title and the app share), referenced until ObDereferenceObject, and nothing to
// enumerate (success with no items, which ends the radio menu's search).
static void test_app_enumerator()
{
    Call call;
    const uint32_t memory = guest::SystemAlloc(0x100);
    const uint32_t handleOut = memory, structureOut = memory + 4, countOut = memory + 8, buffer = memory + 0x10;
    const uint64_t created = call(__imp__XamCreateEnumeratorHandle, 0xFFFFFFFF, 0xFA, 0x70026, 0x70027, 5544, 1, 0, handleOut);
    CHECK(created == 0, "create: %llu", (unsigned long long)created);
    const uint32_t handle = guest::LoadU32(handleOut);
    CHECK(call(__imp__XamGetPrivateEnumStructureFromHandle, handle, structureOut) == 0, "private structure");
    const uint32_t structure = guest::LoadU32(structureOut);
    CHECK(structure && guest::LoadU32(structure) == 0xFA && guest::LoadU32(structure + 4) == 0x70026 && guest::LoadU32(structure + 16) == 1,
        "header at %08X: app %X, open %X", structure, structure ? guest::LoadU32(structure) : 0, structure ? guest::LoadU32(structure + 4) : 0);
    CHECK(structure && guest::LoadU32(structure + 24) == 0 && guest::LoadU32(structure + 24 + 5540) == 0, "shared bytes zeroed");
    CHECK(call(__imp__XamEnumerate, handle, 0, buffer, 820, countOut, 0) == 0 && guest::LoadU32(countOut) == 0, "nothing");
    call(__imp__ObDereferenceObject, structure);
    CHECK(call(__imp__NtClose, handle) == status::kSuccess, "close");
}

// Writes a NUL-terminated ASCII string as big-endian UTF-16 and returns its address.
static uint32_t WriteWide(uint32_t address, const char* string)
{
    for (size_t i = 0;; i++)
    {
        *guest::Translate<be<uint16_t>>(address + uint32_t(i) * 2) = uint16_t(string[i]);
        if (!string[i])
            return address;
    }
}

// The next notification a listener gets within `milliseconds`: { ID, parameter }, or { 0xFFFFFFFF, 0 }.
static std::pair<uint32_t, uint32_t> NextNotification(uint32_t listener, uint32_t milliseconds)
{
    Call call;
    const uint32_t out = Scratch() + 0x900;
    for (uint32_t waited = 0;; waited += 5)
    {
        if (call(__imp__XNotifyGetNext, listener, 0, out, out + 4) == 1)
            return { guest::LoadU32(out), guest::LoadU32(out + 4) };
        if (waited >= milliseconds)
            return { 0xFFFFFFFF, 0 };
        Sleep(5);
    }
}

// System UIs (ui.cpp) answer at once. Where the game has their XN_SYS_UI notifications (GAME_SYSTEM_UI_NOTIFICATIONS:
// DOAX2's XBOX LIVE menu waits for them), opened with the call and closed on the listener's first poll after that finds
// nothing else (at the game's own pace, so two machines in a netplay session go through them alike); DOA4 has none. A new
// listener of the system area first hears who is signed in. A message box asking a player who isn't signed in (DOA4's
// player 2, asked to sign in to save) is answered with its last button, declining.
static void test_system_ui()
{
    Call call;
    const uint32_t memory = guest::SystemAlloc(0x400);
    const uint32_t overlapped = memory, result = memory + 0x20, eventOut = memory + 0x24, buttons = memory + 0x28, text = memory + 0x100,
                   buffer = memory + 0x300;
    using Notification = std::pair<uint32_t, uint32_t>;
    const Notification none(0xFFFFFFFF, 0);

    const uint32_t listener = call(__imp__XamNotifyCreateListener, 1 /* system */, 0);
    CHECK(NextNotification(listener, 0) == Notification(0x0A, 1), "XN_SYS_SIGNINCHANGED: user 0 is signed in");
    CHECK(NextNotification(listener, 0) == none, "nothing else");
    // A UI's notifications: open on the next poll, closed on the poll after one that finds nothing; or none.
    const auto openedThenClosed = [&]() {
        if (!GAME_SYSTEM_UI_NOTIFICATIONS)
            return NextNotification(listener, 0) == none;
        return NextNotification(listener, 0) == Notification(0x09, 1) && NextNotification(listener, 0) == none &&
               NextNotification(listener, 0) == Notification(0x09, 0) && NextNotification(listener, 0) == none;
    };

    // The sign-in UI: the player backs out.
    CHECK(call(__imp__XamShowSigninUI, 1, 2) == 0, "sign-in UI");
    CHECK(openedThenClosed(), "sign-in UI: XN_SYS_UI opened, then closed (%s)", GAME_SYSTEM_UI_NOTIFICATIONS ? "notified" : "none");

    // A message box with an XOVERLAPPED: completed before the call returns, with the focused button.
    CHECK(call(__imp__NtCreateEvent, eventOut, 0, 0 /* notification */, 0) == status::kSuccess, "event");
    const uint32_t event = guest::LoadU32(eventOut);
    memset(guest::ToHost(overlapped), 0, 0x1C);
    guest::StoreU32(overlapped + 12, event);
    guest::StoreU32(buttons, WriteWide(text + 0x40, "OK"));
    guest::StoreU32(buttons + 4, WriteWide(text + 0x60, "Cancel"));
    guest::StoreU32(result, 0xFFFFFFFF);
    const uint32_t shown = call(__imp__XamShowMessageBoxUI, 0, WriteWide(text, "Title"), WriteWide(text + 0x20, "Text"), 2, buttons, 1, 0, result,
        overlapped);
    CHECK(shown == 997 && guest::LoadU32(overlapped) == 0 && guest::LoadU32(result) == 1, "answered at once: %u, %u, button %u", shown,
        guest::LoadU32(overlapped), guest::LoadU32(result));
    CHECK(call(__imp__NtWaitForSingleObjectEx, event, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess, "its event set");
    CHECK(openedThenClosed(), "message box: opened, then closed");

    // Player 2 isn't signed in: the last button, declining.
    guest::StoreU32(result, 0xFFFFFFFF);
    call(__imp__XamShowMessageBoxUI, 1, WriteWide(text, "Sign In"), WriteWide(text + 0x20, "Player2"), 2, buttons, 0, 3, result, 0);
    CHECK(guest::LoadU32(result) == 1, "player 2: button %u", guest::LoadU32(result));
    CHECK(openedThenClosed(), "player 2's message box: opened and closed");

    // The keyboard gives its default text, cut to the buffer.
    memset(guest::ToHost(overlapped), 0, 0x1C);
    const uint32_t typed = call(__imp__XamShowKeyboardUI, 0, 0, WriteWide(text, "Default name"), 0, 0, buffer, 8, overlapped);
    bool same = true;
    for (uint32_t i = 0; i < 7; i++)
        same = same && *guest::Translate<be<uint16_t>>(buffer + i * 2) == uint16_t("Default"[i]);
    CHECK(typed == 997 && guest::LoadU32(overlapped) == 0 && same && *guest::Translate<be<uint16_t>>(buffer + 14) == 0, "keyboard: \"Default\"");

    // Two UIs before a poll: both open, and one close after the queue runs dry.
    call(__imp__XamShowSigninUI, 1, 2);
    if (GAME_SYSTEM_UI_NOTIFICATIONS)
        CHECK(NextNotification(listener, 0) == Notification(0x09, 1), "the keyboard opened");
    CHECK(openedThenClosed(), "two UIs: closed once");

    call(__imp__NtClose, event);
    call(__imp__NtClose, listener);
}

// Profile settings: the dashboard's defaults, no value for the others; the size first (ERROR_INSUFFICIENT_BUFFER).
static void test_profile_settings()
{
    Call call;
    const uint32_t memory = guest::SystemAlloc(0x400);
    const uint32_t ids = memory, size = memory + 0x10, buffer = memory + 0x20;
    const uint32_t settingIds[] = { 0x10040003 /* vibration */, 0x402C0011 /* motto */, 0x10040015 /* difficulty */, 0x63E83FFF /* title's own */ };
    for (uint32_t i = 0; i < 4; i++)
        guest::StoreU32(ids + i * 4, settingIds[i]);
    guest::StoreU32(size, 0);
    CHECK(call(__imp__XamUserReadProfileSettings, 0, 0, 0, 0, 4, ids, size, 0, 0) == 122 && guest::LoadU32(size) == 8 + 4 * 40 + 4,
        "size: %u", guest::LoadU32(size));
    CHECK(call(__imp__XamUserReadProfileSettings, 0, 0, 0, 0, 4, ids, size, buffer, 0) == 0, "read");
    CHECK(guest::LoadU32(buffer) == 4 && guest::LoadU32(buffer + 4) == buffer + 8, "4 settings after the header");
    const auto setting = [&](uint32_t i) { return buffer + 8 + i * 40; };
    CHECK(guest::LoadU32(setting(0)) == 1 && guest::LoadU32(setting(0) + 8) == 0 && guest::LoadU32(setting(0) + 16) == 0x10040003 &&
        *guest::ToHost(setting(0) + 24) == 1 && guest::LoadU32(setting(0) + 32) == 3, "vibration: on (default)");
    const uint32_t motto = guest::LoadU32(setting(1) + 36);
    CHECK(guest::LoadU32(setting(1)) == 1 && *guest::ToHost(setting(1) + 24) == 4 && guest::LoadU32(setting(1) + 32) == 2 &&
        motto >= buffer + 8 + 160 && motto < buffer + 8 + 164 && *guest::Translate<be<uint16_t>>(motto) == 0, "motto: empty");
    CHECK(guest::LoadU32(setting(2)) == 1 && guest::LoadU32(setting(2) + 32) == 0, "difficulty: normal");
    CHECK(guest::LoadU32(setting(3)) == 0 && *guest::ToHost(setting(3) + 24) == 6 && guest::LoadU32(setting(3) + 32) == 0, "the title's own: none");
    CHECK(call(__imp__XamUserReadProfileSettings, 0, 1, 0, 0, 4, ids, size, buffer, 0) == 1317, "user 1 isn't signed in");
}

// Timers: due once or every period, set and cancelled.
static void test_timers()
{
    Call call;
    const uint32_t out = Scratch() + 0x810, due = Scratch() + 0x830, state = Scratch() + 0x838;
    CHECK(call(__imp__NtCreateTimer, out, 0, 1 /* synchronization */) == status::kSuccess, "create a synchronization timer");
    const uint32_t periodic = guest::LoadU32(out);
    *guest::Translate<be<int64_t>>(due) = -200000; // 20 ms from now
    CHECK(call(__imp__NtSetTimerEx, periodic, due, 0, 1, 0, 0, 20, state) == status::kSuccess && *guest::ToHost(state) == 0, "set: 20 ms, every 20 ms");
    CHECK(call(__imp__NtWaitForSingleObjectEx, periodic, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "not due yet");
    const auto start = GetTickCount64();
    CHECK(call(__imp__NtWaitForSingleObjectEx, periodic, 0, 0, TimeoutPtr(-10000000 /* 1 s */)) == status::kSuccess, "due");
    CHECK(call(__imp__NtWaitForSingleObjectEx, periodic, 0, 0, TimeoutPtr(-10000000)) == status::kSuccess, "due again");
    const auto elapsed = GetTickCount64() - start;
    CHECK(elapsed >= 25 && elapsed < 500, "two periods in %llu ms", (unsigned long long)elapsed);
    CHECK(call(__imp__NtCancelTimer, periodic, state) == status::kSuccess, "cancel");
    CHECK(call(__imp__NtWaitForSingleObjectEx, periodic, 0, 0, TimeoutPtr(-1000000 /* 100 ms */)) == status::kTimeout, "not due after that");

    CHECK(call(__imp__NtCreateTimer, out, 0, 0 /* notification */) == status::kSuccess, "create a notification timer");
    const uint32_t once = guest::LoadU32(out);
    *guest::Translate<be<int64_t>>(due) = -100000; // 10 ms
    CHECK(call(__imp__NtSetTimerEx, once, due, 0, 1, 0, 0, 0, 0) == status::kSuccess, "set: 10 ms, once");
    CHECK(call(__imp__NtWaitForSingleObjectEx, once, 0, 0, TimeoutPtr(-10000000)) == status::kSuccess &&
        call(__imp__NtWaitForSingleObjectEx, once, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess, "due, and stays signaled");
    *guest::Translate<be<int64_t>>(due) = -100000000; // 10 s
    CHECK(call(__imp__NtSetTimerEx, once, due, 0, 1, 0, 0, 0, state) == status::kSuccess && *guest::ToHost(state) == 1, "set again: was signaled");
    CHECK(call(__imp__NtWaitForSingleObjectEx, once, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "and isn't now");
    CHECK(call(__imp__NtClose, periodic) == status::kSuccess && call(__imp__NtClose, once) == status::kSuccess, "close");
}

// Xbox Live: session objects (whose creation XAM's Live app then refuses), and empty achievement and leaderboard enumerations.
static void test_live_offline()
{
    Call call;
    const uint32_t memory = guest::SystemAlloc(0x400);
    const uint32_t handleOut = memory, objectOut = memory + 4, sizeOut = memory + 8, countOut = memory + 12, buffer = memory + 0x10,
                   spec = memory + 0x100;
    CHECK(call(__imp__XamSessionCreateHandle, handleOut) == 0, "session handle");
    const uint32_t session = guest::LoadU32(handleOut);
    CHECK(call(__imp__XamSessionRefObjByHandle, session, objectOut) == 0 && guest::LoadU32(objectOut) != 0, "its object");
    call(__imp__ObDereferenceObject, guest::LoadU32(objectOut));
    CHECK(call(__imp__NtClose, session) == status::kSuccess && call(__imp__XamSessionRefObjByHandle, session, objectOut) == 6, "closed");

    CHECK(call(__imp__XamUserCreateAchievementEnumerator, 0, 0, 0x0009000000000001ull, 0xFFFFFFFF, 0, 50, sizeOut, handleOut) == 0 &&
        guest::LoadU32(sizeOut) == 50 * (0x24 + 66 + 202 + 202), "achievements: %u bytes for 50", guest::LoadU32(sizeOut));
    uint32_t handle = guest::LoadU32(handleOut);
    CHECK(call(__imp__XamEnumerate, handle, 0, buffer, 0x40, countOut, 0) == 18 && guest::LoadU32(countOut) == 0, "none");
    call(__imp__NtClose, handle);

    memset(guest::ToHost(spec), 0, 136);
    guest::StoreU32(spec, 1);     // view
    guest::StoreU32(spec + 4, 2); // columns
    CHECK(call(__imp__XamUserCreateStatsEnumerator, 0, 1, 1, 10, 1, spec, sizeOut, handleOut) == 0 &&
        guest::LoadU32(sizeOut) == 8 + 16 + 10 * (48 + 2 * 24), "leaderboard: %u bytes", guest::LoadU32(sizeOut));
    handle = guest::LoadU32(handleOut);
    CHECK(call(__imp__XamEnumerate, handle, 0, buffer, 0x40, countOut, 0) == 18 && guest::LoadU32(countOut) == 0, "no rows");
    call(__imp__NtClose, handle);
}

// RtlRaiseException: a thread name for the debugger (MSVC's 0x406D1388) goes on. (The others end the game.)
static void test_raise_exception()
{
    Call call;
    const uint32_t record = guest::SystemAlloc(0x60);
    memset(guest::ToHost(record), 0, 0x50);
    guest::StoreU32(record, 0x406D1388);
    guest::StoreU32(record + 16, 4);
    call(__imp__RtlRaiseException, record);
    CHECK(true, "returned");
}

// The music player (XAM app 0xFA, through XMsg): a title playlist played, its status and current song, next, stop and
// delete; and the state changes notified to listeners of the XMP area.
static void test_music_player()
{
    Call call;
    const uint32_t memory = guest::SystemAlloc(0x2000);
    const uint32_t args = memory, out = memory + 0x40, storage = memory + 0x100, songs = memory + 0x300, text = memory + 0x400;
    const uint32_t info = memory + 0x800, notification = memory + 0x60;
    const auto wide = [](uint32_t address, const char* string) {
        for (size_t i = 0;; i++)
        {
            *guest::Translate<be<uint16_t>>(address + uint32_t(i) * 2) = uint16_t(string[i]);
            if (!string[i])
                return address;
        }
    };
    const auto message = [&](uint32_t id, std::initializer_list<uint32_t> values) {
        uint32_t i = 0;
        for (uint32_t value : values)
            guest::StoreU32(args + 4 * i++, value);
        return call(__imp__XMsgInProcessCall, 0xFA, id, args, 4 * i);
    };

    const uint32_t listener = call(__imp__XamNotifyCreateListener, 0x20 /* XMP */, 0);
    CHECK(listener != 0, "listener");

    CHECK(message(0x7002E, { 2, 2, out }) == 0 && guest::LoadU32(out) == 4 + 2 * 128, "storage for 2 songs: %u", guest::LoadU32(out));
    // XMP_SONGDESCRIPTOR: path, title, artist, album, album artist, genre, track, duration, format.
    const char* titles[] = { "First", "Second" };
    for (uint32_t i = 0; i < 2; i++)
    {
        const uint32_t descriptor = songs + i * 36;
        memset(guest::ToHost(descriptor), 0, 36);
        guest::StoreU32(descriptor, wide(text + i * 0x100, i ? "GAME:\\b.mp3" : "GAME:\\a.mp3"));
        guest::StoreU32(descriptor + 4, wide(text + i * 0x100 + 0x40, titles[i]));
        guest::StoreU32(descriptor + 24, i + 1);
        guest::StoreU32(descriptor + 28, 1000 * (i + 1));
    }
    const uint32_t handles = out + 4, playlistOut = out + 12;
    CHECK(message(0x7000D, { 2, storage, 260, songs, 2, 0, 0, handles, playlistOut }) == 0, "create the playlist");
    const uint32_t playlist = guest::LoadU32(playlistOut), second = guest::LoadU32(handles + 4);
    CHECK(playlist == storage && guest::LoadU32(handles) != 0 && second != 0 && second != guest::LoadU32(handles), "its handle and the songs'");

    CHECK(message(0x70002, { 2, playlist, second }) == 0, "play from the second song");
    CHECK(message(0x70009, { 2, out }) == 0 && guest::LoadU32(out) == 1, "playing: %u", guest::LoadU32(out));
    CHECK(call(__imp__XNotifyGetNext, listener, 0, notification, notification + 4) == 1 && guest::LoadU32(notification) == 0x0A000001 &&
          guest::LoadU32(notification + 4) == 1, "notified: state changed to playing");
    CHECK(call(__imp__XNotifyGetNext, listener, 0, notification, notification + 4) == 0, "only once");
    const auto infoTitle = [&] {
        std::string title;
        for (uint32_t i = 0; i < 40 && *guest::Translate<be<uint16_t>>(info + 576 + i * 2) != 0; i++)
            title.push_back(char(uint16_t(*guest::Translate<be<uint16_t>>(info + 576 + i * 2))));
        return title;
    };
    CHECK(message(0x7000E, { 2, 0, info }) == 0 && infoTitle() == "Second" && guest::LoadU32(info + 980) == 2000, "current song: \"%s\"", infoTitle().c_str());
    CHECK(message(0x70006, { 2 }) == 0 && message(0x7000E, { 2, 0, info }) == 0 && infoTitle() == "First", "next wraps around: \"%s\"", infoTitle().c_str());
    CHECK(message(0x70005, { 2 }) == 0 && message(0x70009, { 2, out }) == 0 && guest::LoadU32(out) == 2, "paused");
    CHECK(message(0x70004, { 2, 0 }) == 0 && message(0x70009, { 2, out }) == 0 && guest::LoadU32(out) == 0, "stopped");
    CHECK(message(0x7000E, { 2, 0, info }) != 0, "no current song");
    CHECK(message(0x70013, { 2, playlist }) == 0, "delete");
    CHECK(message(0x70013, { 2, playlist }) == 0x80070490 && message(0x70002, { 2, playlist, 0 }) == 0x80070490, "gone");
    uint32_t notifications = 0;
    while (call(__imp__XNotifyGetNext, listener, 0x0A000001, notification, notification + 4) == 1)
        notifications++;
    CHECK(notifications == 3, "state changes after the first: next, pause, stop (%u)", notifications);
    call(__imp__NtClose, listener);
    guest::SystemFree(memory);
}

// XMA contexts through the decoder's registers, as DOAX2's XAudio drives them: the context array's physical address
// in ContextArrayAddress gives a context's index, a kick fills its output ring (with silence here) and uses up its
// input, and a clear resets its buffers.
static void test_xma_registers()
{
    Call call;
    // Decoded as silence (the input here is zeros, which FFmpeg can't decode); set before the first kick reads it.
    _putenv_s(GAME_PREFIX "_XMA_SILENT", "1");
    guest::ReserveFixed(audio::kXmaRegisterPage, audio::kXmaRegisterPageSize, guest::kPageReadWrite);
    audio::InitializeXmaRegisters();
    const uint32_t arrayPhysical = *reinterpret_cast<const uint32_t*>(guest::ToHost(audio::kXmaRegisterPage + 0x1800)); // little endian
    const uint32_t out = Scratch() + 0x880;
    CHECK(call(__imp__XMACreateContext, out) == status::kSuccess, "create");
    const uint32_t context = guest::LoadU32(out);
    const uint32_t index = (guest::PhysicalAddress(context) - arrayPhysical) >> 6;
    CHECK(index < 320 && guest::PhysicalAddress(context) == arrayPhysical + index * 64, "index %u from the register", index);

    // Mono, 48 kHz: input buffer 0 of 2 packets, valid; an output ring of 8 blocks (2 frames of 512 samples), valid.
    const uint32_t input = guest::AllocPhysical(0x1000, 0x1000, guest::kPageReadWrite, 0, 0x1FFFFFFF, 0x1000);
    const uint32_t output = guest::AllocPhysical(0x1000, 0x1000, guest::kPageReadWrite, 0, 0x1FFFFFFF, 0x1000);
    memset(guest::ToHost(output), 0xCC, 0x800);
    const auto word = [context](uint32_t i) { return guest::LoadU32(context + i * 4); };
    guest::StoreU32(context + 0, 2 | 1u << 20 | 8u << 22); // input 0: 2 packets, valid; 8 output blocks
    guest::StoreU32(context + 4, 3u << 27 | 1u << 31);     // 48 kHz, mono, output valid
    guest::StoreU32(context + 8, 32);                       // read offset: after the first packet's header
    guest::StoreU32(context + 20, guest::PhysicalAddress(input));
    guest::StoreU32(context + 28, guest::PhysicalAddress(output));
    const uint32_t kick = audio::kXmaRegisterPage + 0x1940 + index / 32 * 4, clear = audio::kXmaRegisterPage + 0x1A80 + index / 32 * 4;
    XmaRegisterWrite(kick, 1u << (index % 32));
    CHECK(guest::ToHost(output)[0] == 0 && guest::ToHost(output)[0x7FF] == 0, "the ring is full of silence");
    CHECK((word(0) >> 27) == 0 && !(word(1) >> 31), "write offset wrapped to the read offset: full, output invalid (%08X %08X)", word(0), word(1));
    CHECK(word(2) == 32, "two frames haven't used a packet up yet: read offset %u", word(2));

    // The game reads both frames (its read offset stays at the write offset) and makes the output valid again: two
    // frames more, and the 4th uses the first packet up.
    guest::StoreU32(context + 4, word(1) | 1u << 31);
    memset(guest::ToHost(output), 0xCC, 0x800);
    XmaRegisterWrite(kick, 1u << (index % 32));
    CHECK(guest::ToHost(output)[0] == 0 && guest::ToHost(output)[0x7FF] == 0 && (word(0) >> 27) == 0 && !(word(1) >> 31), "two frames more (%08X)", word(0));
    CHECK(word(2) == 16384 + 32, "the 4th frame moved on to packet 2: read offset %u", word(2));

    XmaRegisterWrite(clear, 1u << (index % 32));
    CHECK(!(word(0) & (3u << 20)) && (word(0) >> 27) == 0 && !(word(1) >> 31) && (word(9) & 0x1F) == 0, "cleared: no valid buffers, offsets 0");
    call(__imp__XMAReleaseContext, context);
    guest::FreePhysical(input);
    guest::FreePhysical(output);
}

// XAM and the kernel by name, and their exports by ordinal: the title's import thunks.
static void test_system_modules()
{
    Call call;
    const uint32_t text = Scratch() + 0xB80, handle = Scratch() + 0xBA0, procedure = Scratch() + 0xBA4;
    strcpy(reinterpret_cast<char*>(guest::ToHost(text)), "xam.xex");
    CHECK(call(__imp__XexGetModuleHandle, text, handle) == status::kSuccess && guest::LoadU32(handle) != 0, "xam.xex");
    const uint32_t xam = guest::LoadU32(handle);
    const uint32_t thunk = imports::FunctionThunk("Xam", 1); // NetDll_WSAStartup
    CHECK(thunk != 0, "the game imports XAM's ordinal 1");
    CHECK(call(__imp__XexGetProcedureAddress, xam, 1, procedure) == status::kSuccess && guest::LoadU32(procedure) == thunk, "its thunk");
    CHECK(call(__imp__XexGetProcedureAddress, xam, 0x50, procedure) == 0xC000007A && guest::LoadU32(procedure) == 0, "an ordinal it doesn't import");
    strcpy(reinterpret_cast<char*>(guest::ToHost(text)), "XBOXKRNL.EXE");
    CHECK(call(__imp__XexGetModuleHandle, text, handle) == status::kSuccess && guest::LoadU32(handle) != 0 && guest::LoadU32(handle) != xam, "xboxkrnl.exe");
    strcpy(reinterpret_cast<char*>(guest::ToHost(text)), "xbdm.xex");
    CHECK(call(__imp__XexGetModuleHandle, text, handle) == 0xC0000135, "no such module");
}

// ---------------------------------------------------------------------------------------------
// Shaders

// The shader binaries precompilation finds in the executable: with and without constant tables, all parsed and
// translated, with nothing the translation can't express but the format 0 fetches of D3D's vertex shader templates
// (bound copies get their formats), and vertex fetches addressing the vertex their full fetch indexed. Pixel shaders'
// centroid variants (their last two interpolators) declare those interpolators so and no others, and the smallest
// compiles, as do the smallest vertex and pixel shaders with products translated with Xenos products and DEBUG_NAN's
// counts. Uses the game's executable, when private/default.pe is there.
static void test_shader_binaries()
{
    std::ifstream file("private/default.pe", std::ios::binary);
    if (!file)
    {
        printf("  skipped: no private/default.pe\n");
        return;
    }
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    uint32_t vertex = 0, pixel = 0, translated = 0, centroids = 0;
    bool groundShader = false;
    std::string smallestCentroid, smallestProducts[2];
    for (const gpu::shader::ShaderBinary& binary : gpu::shader::FindShaderBinaries(image.data(), image.size()))
    {
        const bool isVertex = binary.stage == gpu::ShaderStage::kVertex;
        (isVertex ? vertex : pixel)++;
        const gpu::shader::ParsedShader parsed = gpu::shader::Parse(binary.stage, binary.ucode.data(), uint32_t(binary.ucode.size()));
        CHECK(parsed.valid, "%s at 0x%zX: %s", isVertex ? "vs" : "ps", binary.offset, parsed.error.c_str());
        if (!parsed.valid)
            continue;
        const gpu::shader::HlslResult result = gpu::shader::TranslateToHlsl(parsed);
        translated += !result.source.empty();
        for (const std::string& what : result.unsupported)
            CHECK(isVertex && what == "unsupported vertex format 0", "%s at 0x%zX: %s", isVertex ? "vs" : "ps", binary.offset, what.c_str());
        // Vertex fetches address the vertex the last full fetch indexed: mini fetches don't re-read the index register,
        // which a fetch between them may have written. DOA4's particle shader BCE21477 (behind the menus and in attract
        // mode) writes 1.0 into its index register r0.x with its first mini fetch, so its second read every vertex's
        // texture coordinates from vertex 1. (DOAX2's ground shader 1427337B fetches its lightmap coordinates after
        // writing its index register: Tranquil Beach's sand came out white.)
        const auto count = [&](const char* text) {
            size_t n = 0;
            for (size_t at = result.source.find(text); at != std::string::npos; at = result.source.find(text, at + 1))
                n++;
            return n;
        };
        const size_t fetches = count("uint xe_address = ");
        CHECK(count("uint xe_address = (xe_vf.x & 0xFFFFFFFCu) + uint(xe_vfetch_index * ") == fetches, "%s at 0x%zX: a vertex fetch not using the full fetch's index",
            isVertex ? "vs" : "ps", binary.offset);
        if (parsed.hash == 0x1427337BD0232A66ull)
        {
            groundShader = true;
            CHECK(fetches == 6 && count("xe_vfetch_index = int(") == 1, "vs 1427337B: %zu fetches, %zu index reads (expected 6 and 1)", fetches,
                count("xe_vfetch_index = int("));
        }
        if (!isVertex && parsed.tempCount >= 3)
        {
            const uint32_t last = std::min(parsed.tempCount, 16u) - 1; // the last interpolator it loads
            gpu::shader::TranslationOptions options;
            options.centroidInterpolators = 3u << (last - 1);
            const std::string source = gpu::shader::TranslateToHlsl(parsed, options).source;
            const auto declaration = [](bool centroid, uint32_t i) {
                char text[64];
                snprintf(text, sizeof(text), "%sfloat4 interpolator%u : TEXCOORD%u;", centroid ? "centroid " : "    ", i, i);
                return std::string(text);
            };
            const bool declared = source.find(declaration(true, last - 1)) != std::string::npos &&
                source.find(declaration(true, last)) != std::string::npos && source.find(declaration(false, last - 2)) != std::string::npos;
            CHECK(declared, "ps at 0x%zX: centroid interpolators %u and %u not declared so", binary.offset, last - 1, last);
            centroids += declared;
            if (smallestCentroid.empty() || source.size() < smallestCentroid.size())
                smallestCentroid = source;
        }
        gpu::shader::TranslationOptions products;
        products.xenosProducts = true;
        products.debugNan = 2;
        const std::string source = gpu::shader::TranslateToHlsl(parsed, products).source;
        std::string& smallest = smallestProducts[isVertex ? 0 : 1];
        if (source.find("XeMul(") != std::string::npos && source.find("XeDot(") != std::string::npos &&
            (smallest.empty() || source.size() < smallest.size()))
            smallest = source;
    }
    // DOAX2's: 328 vertex and 164 pixel shaders, in the newer 0x102A11xx container.
    CHECK(vertex == 328 && pixel == 164 && translated == vertex + pixel, "%u vertex, %u pixel, %u translated", vertex, pixel, translated);
    CHECK(centroids > 0, "no pixel shader with 3 interpolators or more");
    CHECK(groundShader, "vertex shader 1427337B (the island's ground) not found");
    CHECK(!smallestProducts[0].empty() && !smallestProducts[1].empty(), "no vertex or pixel shader with products");
    auto compile = [](const std::string& source, const char* target, const char* what) {
        ID3DBlob* bytecode = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT compiled = D3DCompile(source.data(), source.size(), what, nullptr, nullptr, "main", target,
            D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &bytecode, &errors);
        CHECK(SUCCEEDED(compiled), "the smallest %s doesn't compile: %s", what, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (bytecode)
            bytecode->Release();
        if (errors)
            errors->Release();
    };
    compile(smallestCentroid, "ps_5_1", "centroid variant");
    compile(smallestProducts[0], "vs_5_1", "vertex shader with Xenos products and DEBUG_NAN");
    compile(smallestProducts[1], "ps_5_1", "pixel shader with Xenos products and DEBUG_NAN");
    printf("  %u pixel shaders' centroid variants declared\n", centroids);
}

// ---------------------------------------------------------------------------------------------
// Audio

// XMA streams in private/dumps (DUMP_XMA=1 records them), decoded the way contexts decode them (frames read
// and repacked) and from their packets as they are, give the same samples: every frame's 512 per channel, less
// what FFmpeg's decoder trims at the start (64) and at the end (as the last frame says, under 2048).
static void test_xma_decoding()
{
#if HAVE_FFMPEG
    struct Dump
    {
        std::filesystem::path path;
        uint32_t packets, channels, rate;
    };
    std::vector<Dump> dumps;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("private/dumps", error))
    {
        Dump dump{ entry.path() };
        uint32_t physical = 0;
        if (sscanf(entry.path().filename().string().c_str(), "xma_%8X_%u_%uch_%u.bin", &physical, &dump.packets, &dump.channels, &dump.rate) == 4)
            dumps.push_back(dump);
    }
    if (dumps.empty())
    {
        printf("  skipped: no XMA streams in private/dumps\n");
        return;
    }
    uint32_t frames = 0;
    for (const Dump& dump : dumps)
    {
        std::ifstream file(dump.path, std::ios::binary);
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::string name = dump.path.filename().string();
        if (data.size() != size_t(dump.packets) * 2048 || (dump.channels != 1 && dump.channels != 2))
        {
            CHECK(false, "%s: %zu bytes", name.c_str(), data.size());
            continue;
        }
        const uint32_t buffer = guest::AllocPhysical(uint32_t(data.size()), 0x1000, guest::kPageReadWrite, 0, 0x1FFFFFFF, 0x1000);
        memcpy(guest::ToHost(buffer), data.data(), data.size());
        const uint32_t physical = guest::PhysicalAddress(buffer);
        const audio::XmaTestDecode direct = audio::DecodeXmaForTest(physical, dump.packets, dump.channels, dump.rate, true);
        const audio::XmaTestDecode repacked = audio::DecodeXmaForTest(physical, dump.packets, dump.channels, dump.rate, false);
        guest::FreePhysical(buffer);
        CHECK(direct.ok && repacked.ok, "%s: FFmpeg can't decode it (direct %d, repacked %d)", name.c_str(), direct.ok, repacked.ok);
        size_t difference = 0;
        while (difference < direct.samples.size() && difference < repacked.samples.size() && direct.samples[difference] == repacked.samples[difference])
            difference++;
        CHECK(repacked.samples == direct.samples, "%s: %zu samples repacked, %zu direct, first difference at %zu", name.c_str(), repacked.samples.size(),
            direct.samples.size(), difference);
        const size_t samples = repacked.samples.size() / dump.channels, whole = size_t(repacked.frames) * 512;
        CHECK(repacked.frames && samples + 64 <= whole && samples + 64 + 2048 > whole, "%s: %u frames, %zu samples per channel", name.c_str(),
            repacked.frames, samples);
        frames += repacked.frames;
    }
    printf("  %zu streams, %u frames\n", dumps.size(), frames);
#else
    printf("  skipped: built without FFmpeg\n");
#endif
}

// ---------------------------------------------------------------------------------------------
// Settings file

static void test_settings_file()
{
    const std::string previous = config::DataDirectory();
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "doa4_settings_test";
    std::filesystem::create_directories(directory);
    config::SetDataDirectory(directory.string());
    const std::filesystem::path file = directory / "settings.ini";
    for (const char* variable : { GAME_PREFIX "_FULLSCREEN", GAME_PREFIX "_RESOLUTION_SCALE", GAME_PREFIX "_MSAA", GAME_PREFIX "_ALPHA_TO_COVERAGE", GAME_PREFIX "_ANISOTROPY" })
        _putenv_s(variable, "");

    // Values out of range are brought into it; unknown lines are ignored.
    std::ofstream(file) << "; comment\n[game]\ndisc_image = D:\\Games\\Dead or Alive 4 (Rev 1).iso  \r\n[graphics]\nfullscreen = 1\n"
                           "resolution_scale = 9\nmsaa = 6\nalpha_to_coverage = 1\nanisotropy = 12\nunknown = 3\n";
    settings::Load();
    CHECK(settings::DiscImage() == "D:\\Games\\Dead or Alive 4 (Rev 1).iso", "disc image \"%s\"", settings::DiscImage().c_str());
    settings::Graphics graphics = settings::GetGraphics();
    CHECK(graphics.fullscreen && graphics.resolutionScale == 4 && graphics.msaaSamples == 4 && graphics.alphaToCoverage &&
            graphics.anisotropy == 8,
        "scale %u, MSAA %u, anisotropy %u", graphics.resolutionScale, graphics.msaaSamples, graphics.anisotropy);

    // The environment overrides the file, and isn't saved unless the settings change.
    _putenv_s(GAME_PREFIX "_ANISOTROPY", "2");
    settings::Load();
    _putenv_s(GAME_PREFIX "_ANISOTROPY", "");
    CHECK(settings::GetGraphics().anisotropy == 2, "anisotropy %u", settings::GetGraphics().anisotropy);
    std::filesystem::remove(file);
    settings::Save();
    CHECK(!std::filesystem::exists(file), "saved without a change");

    // A change counts as a new version, and is what Save writes.
    const uint32_t version = settings::GraphicsVersion();
    graphics = settings::GetGraphics();
    graphics.fullscreen = false;
    graphics.resolutionScale = 2;
    graphics.alphaToCoverage = false;
    settings::SetGraphics(graphics);
    CHECK(settings::GraphicsVersion() == version + 1, "version %u after %u", settings::GraphicsVersion(), version);
    settings::SetGraphics(graphics);
    CHECK(settings::GraphicsVersion() == version + 1, "setting the same values isn't a change");
    settings::Save();
    settings::Load();
    graphics = settings::GetGraphics();
    CHECK(!graphics.fullscreen && graphics.resolutionScale == 2 && graphics.msaaSamples == 4 && !graphics.alphaToCoverage &&
            graphics.anisotropy == 2,
        "read back: scale %u, MSAA %u, anisotropy %u", graphics.resolutionScale, graphics.msaaSamples, graphics.anisotropy);
    CHECK(settings::DiscImage() == "D:\\Games\\Dead or Alive 4 (Rev 1).iso", "disc image kept: \"%s\"", settings::DiscImage().c_str());

    std::filesystem::remove_all(directory);
    config::SetDataDirectory(previous);
    settings::SetGraphics(settings::Graphics{});
}

// ---------------------------------------------------------------------------------------------
// The game's executable from its disc image

static void test_disc_image()
{
    // Something that isn't a disc image.
    const std::filesystem::path fake = std::filesystem::temp_directory_path() / "doa4_not_a_disc.iso";
    {
        std::ofstream out(fake, std::ios::binary);
        const std::vector<char> zeros(256 * 1024, 0);
        out.write(zeros.data(), std::streamsize(zeros.size()));
    }
    std::vector<uint8_t> xex;
    CHECK(disc::ReadExecutable(fake.string(), xex) == disc::Check::kUnreadable, "a file of zeros");
    std::filesystem::remove(fake);

    // The real executable (development checkouts have it in private/): checked, and unpacked into default.pe.
    std::ifstream file("private/default.xex", std::ios::binary | std::ios::ate);
    if (!file)
    {
        printf("  skipped the rest: no private/default.xex\n");
        return;
    }
    xex.resize(size_t(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(xex.data()), std::streamsize(xex.size()));
    CHECK(disc::IsSupported(xex), "private/default.xex isn't the supported version's");
    std::vector<uint8_t> image;
    CHECK(disc::Unpack(xex, image), "unpacking");
    CHECK(image.size() == PPC_IMAGE_SIZE, "image size 0x%zX", image.size());
    const std::string imageHash = disc::Sha256(image.data(), image.size());
    CHECK(imageHash == "f4ff72eb3f46f7b6ef13fcddbbc193aa2307adb8df1d1651197c73b694b12c2a", "image SHA-256 %s", imageHash.c_str());
    std::vector<uint8_t> changed = xex;
    changed[changed.size() / 2] ^= 1;
    CHECK(!disc::IsSupported(changed), "a changed byte");

    // The disc image itself, if it's in the working directory.
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(".", error))
    {
        if (entry.path().extension() != ".iso")
            continue;
        std::vector<uint8_t> fromDisc;
        CHECK(disc::ReadExecutable(entry.path().string(), fromDisc) == disc::Check::kOk, "%s", entry.path().string().c_str());
        CHECK(fromDisc == xex, "default.xex from the disc image differs from private/default.xex");
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// The title's SPA data (its achievements, strings and images) from the executable's resources

static bool ReadFile(const char* path, std::vector<uint8_t>& data)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    data.resize(size_t(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    return bool(file);
}

// A string's FNV-1a hash: the tests check the game's own text by it rather than spell it out.
static uint32_t Fnv1a(const std::string& text)
{
    uint32_t hash = 2166136261u;
    for (unsigned char c : text)
        hash = (hash ^ c) * 16777619u;
    return hash;
}

static void test_spa()
{
    std::vector<uint8_t> xex, image;
    if (!ReadFile("private/default.xex", xex) || !ReadFile("private/default.pe", image))
    {
        printf("  skipped: no private/default.xex and default.pe\n");
        return;
    }
    CHECK(spa::Load(xex, image), "the SPA data");
    CHECK(spa::TitleName(1) == "DEAD OR ALIVE Xtreme 2", "title \"%s\"", spa::TitleName(1).c_str());
    CHECK(spa::DefaultLanguage() == 1, "default language %u", spa::DefaultLanguage());
    uint32_t gamerscore = 0;
    for (const spa::Achievement& achievement : spa::Achievements())
        gamerscore += achievement.gamerscore;
    CHECK(spa::Achievements().size() == 46 && gamerscore == 1000, "%zu achievements, %u gamerscore", spa::Achievements().size(), gamerscore);
    const spa::Achievement* friends = spa::FindAchievement(15);
    // Its English name, four friends' swimsuits collected with Ayane (by its hash).
    CHECK(friends && Fnv1a(spa::String(friends->nameId, 1)) == 0x5A0DD5C1 && friends->gamerscore == 20 && friends->imageId == 4,
        "achievement 15");
    CHECK(friends && spa::String(friends->nameId, 2) != spa::String(friends->nameId, 1) && !spa::String(friends->nameId, 2).empty(),
        "achievement 15's name in Japanese");
    // They're 8 to 53.
    CHECK(!spa::FindAchievement(7) && !spa::FindAchievement(54), "no achievements 7 and 54");
    // The title's image, the window's icon: a 64x64 PNG.
    const std::vector<uint8_t> title = spa::TitleImage();
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
    CHECK(util::DecodeImage(title.data(), title.size(), width, height, false, bgra) && width == 64 && height == 64, "title image %ux%u",
        width, height);
    width = height = 20;
    CHECK(util::DecodeImage(title.data(), title.size(), width, height, true, bgra) && bgra.size() == 20 * 20 * 4, "scaled to 20x20");
    const std::vector<uint8_t> picture = friends ? spa::Image(friends->imageId) : std::vector<uint8_t>();
    width = height = 0;
    CHECK(util::DecodeImage(picture.data(), picture.size(), width, height, true, bgra) && width == 64 && height == 64,
        "achievement 15's picture %ux%u", width, height);
}

// The game writing achievements (XUserWriteAchievements through XMsg), recorded in the profile's file.
static void test_achievements()
{
    if (!spa::Loaded())
    {
        printf("  skipped: no SPA data\n");
        return;
    }
    Call call;
    const std::string previous = config::DataDirectory();
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "doa4_achievements_test";
    std::filesystem::remove_all(directory);
    config::SetDataDirectory(directory.string());
    achievements::Reset();
    // As the game writes them: XMsgStartIORequest(0xFB, 0xB0008, overlapped, { count, list }, 8), 8 bytes an achievement
    // (the game user, its ID). User 1 has no profile.
    const uint32_t request = Scratch() + 0x200, list = Scratch() + 0x210, overlapped = Scratch() + 0x240;
    const uint32_t written[3][2] = { { 0, 15 }, { 1, 16 }, { 0, 19 } };
    guest::StoreU32(request, 3);
    guest::StoreU32(request + 4, list);
    for (uint32_t i = 0; i < 3; i++)
    {
        guest::StoreU32(list + i * 8, written[i][0]);
        guest::StoreU32(list + i * 8 + 4, written[i][1]);
    }
    memset(guest::ToHost(overlapped), 0, 0x1C);
    CHECK(call(__imp__XMsgStartIORequest, 0xFB, 0xB0008, overlapped, request, 8) == 997 && guest::LoadU32(overlapped) == 0, "result %08X",
        guest::LoadU32(overlapped));
    CHECK(achievements::Earned(15) && achievements::Earned(19) && !achievements::Earned(16), "earned 15 and 19, not 16");
    CHECK(!achievements::Earn(0, 15), "15 again isn't new");
    achievements::Reset();
    CHECK(achievements::Earned(15) && achievements::Earned(19) && !achievements::Earned(16), "read back from the file");
    std::ifstream file(achievements::FilePath());
    std::string line, lines;
    int count = 0;
    while (std::getline(file, line))
    {
        if (!line.empty() && line[0] != ';')
            count++, lines += line + "|";
    }
    // Each line has the achievement's gamerscore and English name, as the SPA data has them.
    const spa::Achievement* fifteen = spa::FindAchievement(15);
    CHECK(count == 2 && lines.find("15 ") == 0 && fifteen && lines.find("20 " + spa::String(fifteen->nameId, 1) + "|") != std::string::npos &&
            lines.find("|19 ") != std::string::npos,
        "the file's lines: %s", lines.c_str());
    // Bounded: a count the game never writes is refused.
    guest::StoreU32(request, 100000);
    CHECK(call(__imp__XMsgStartIORequest, 0xFB, 0xB0008, overlapped, request, 8) == 997 && guest::LoadU32(overlapped) == 0x80070057,
        "too many: %08X", guest::LoadU32(overlapped));
    file.close();
    achievements::Reset();
    std::filesystem::remove_all(directory);
    config::SetDataDirectory(previous);
}

// ---------------------------------------------------------------------------------------------
// netplay::journal: a replay with other calls than the first run's (a rollback's), answered where they're the same

static int g_journalPlays = 0, g_journalQueries = 0, g_journalReleases = 0;
static uint32_t g_journalNextId = 100;

static void JournalPlay(PPCContext& ctx, uint8_t*)
{
    g_journalPlays++;
    ctx.r3.u64 = g_journalNextId++;
}

static void JournalQuery(PPCContext& ctx, uint8_t*)
{
    g_journalQueries++;
    guest::StoreU32(ctx.r5.u32, 0x12345678); // an out argument on the caller's stack
    ctx.r3.u64 = 7;
}

static void JournalRelease(PPCContext&, uint8_t*)
{
    g_journalReleases++;
}

static void test_journal_divergence()
{
    namespace journal = netplay::journal;
    // A stack, and a processor control region giving its bounds, for the spans the journal watches.
    const uint32_t stack = guest::SystemAlloc(0x10000), pcr = guest::SystemAlloc(0x1000);
    guest::StoreU32(pcr + 0x70, stack + 0x10000);
    guest::StoreU32(pcr + 0x74, stack);
    PPCContext ctx{};
    ctx.r1.u64 = stack + 0x8000;
    ctx.r13.u64 = pcr;
    const uint32_t block = ctx.r1.u32 + 96, out = ctx.r1.u32 + 64;
    constexpr uint32_t kPlay = 0x1000, kQuery = 0x2000, kRelease = 0x3000, kObject = 0x5000;
    journal::FunctionInfo play, query, release;
    play.address = kPlay;
    play.arguments = journal::kArgumentR3;
    play.blockRegister = 4;
    play.blockFields = { { 0, 4 } }; // the sound's number
    play.nearFrames = 2;
    query.address = kQuery;
    query.arguments = journal::kArgumentR3 | 0x1; // r3, r4
    release.address = kRelease;
    journal::Describe({ play, query, release });
    const auto Play = [&](uint32_t sound) {
        guest::StoreU32(block, sound);
        ctx.r3.u64 = kObject;
        ctx.r4.u64 = block;
        ctx.r5.u64 = 0xDEAD; // a leftover the function doesn't read
        ctx.lr = 0x82000100;
        journal::Call(kPlay, JournalPlay, ctx, guest::g_base);
        return ctx.r3.u32;
    };
    const auto Query = [&](uint32_t which) {
        guest::StoreU32(out, 0);
        ctx.r3.u64 = kObject;
        ctx.r4.u64 = which;
        ctx.r5.u64 = out;
        ctx.lr = 0x82000200;
        journal::Call(kQuery, JournalQuery, ctx, guest::g_base);
        return ctx.r3.u32;
    };
    const auto Release = [&](uint32_t object) {
        ctx.r3.u64 = object;
        ctx.lr = 0x82000300;
        journal::Defer(kRelease, JournalRelease, ctx, guest::g_base);
    };

    // The first run: frame 10 plays sound 5 and queries, frame 11 plays sound 6 and releases an object.
    netplay::SetFrame(10);
    journal::Record();
    CHECK(Play(5) == 100 && g_journalPlays == 1, "first run's sound 5: %u plays", g_journalPlays);
    CHECK(Query(1) == 7 && g_journalQueries == 1 && guest::LoadU32(out) == 0x12345678, "first run's query");
    netplay::SetFrame(11);
    CHECK(Play(6) == 101, "first run's sound 6");
    Release(0x7000);
    CHECK(g_journalReleases == 0, "a release is deferred");

    // Rolled back to frame 10 and replayed with other input: the same query (answered, its out argument written),
    // another sound (made), no sound 5; at frame 11 sound 6 again (answered) and no release.
    netplay::SetFrame(10);
    journal::ReplayFrom(10);
    CHECK(Query(1) == 7 && g_journalQueries == 1 && guest::LoadU32(out) == 0x12345678, "the same query answered from the record");
    CHECK(!journal::MadeInReplay(kQuery), "an answered call isn't one the replay made");
    CHECK(Query(2) == 7 && g_journalQueries == 2, "a query with another argument made");
    CHECK(journal::MadeInReplay(kQuery), "the replay made the query");
    CHECK(Play(9) == 102 && g_journalPlays == 3, "a sound the record doesn't hold made");
    CHECK(journal::MadeInReplay(kPlay), "the replay made the sound");
    netplay::SetFrame(11);
    CHECK(Play(6) == 101 && g_journalPlays == 3, "the same sound at the same frame answered");
    CHECK(!journal::MadeInReplay(kPlay), "the latest sound answered");
    netplay::SetFrame(12);
    journal::FinishReplay();
    CHECK(!journal::MadeInReplay(kQuery), "no replay under way");
    journal::Statistics statistics = journal::GetStatistics();
    CHECK(statistics.replayed == 2 && statistics.mismatched == 2, "answered %llu, made %llu", (unsigned long long)statistics.replayed,
        (unsigned long long)statistics.mismatched);
    // Sound 5 (frame 10) may still match a sound up to 2 frames away, until frame 13; the release is dropped at once.
    CHECK(statistics.unreplayed == 1 && statistics.releasesDropped == 1, "abandoned %llu, releases dropped %llu",
        (unsigned long long)statistics.unreplayed, (unsigned long long)statistics.releasesDropped);
    journal::OnFrame(13, 0);
    statistics = journal::GetStatistics();
    CHECK(statistics.unreplayed == 2, "sound 5 abandoned once passed: %llu", (unsigned long long)statistics.unreplayed);

    // A sound replayed a frame later than recorded is the same sound.
    netplay::SetFrame(20);
    CHECK(Play(8) == 103, "first run's sound 8");
    netplay::SetFrame(20);
    journal::ReplayFrom(20);
    netplay::SetFrame(21);
    CHECK(Play(8) == 103 && g_journalPlays == 4, "sound 8 a frame later answered");
    netplay::SetFrame(22);
    journal::FinishReplay();
    statistics = journal::GetStatistics();
    CHECK(statistics.replayedNear == 1, "answered from another frame's record: %llu", (unsigned long long)statistics.replayedNear);

    // A replay of the same frames, as a sync test's: everything answered, nothing made or abandoned.
    netplay::SetFrame(30);
    Play(3);
    Query(1);
    netplay::SetFrame(30);
    journal::ReplayFrom(30);
    const journal::Statistics before = journal::GetStatistics();
    Play(3);
    Query(1);
    netplay::SetFrame(31);
    journal::FinishReplay();
    statistics = journal::GetStatistics();
    CHECK(statistics.replayed == before.replayed + 2 && statistics.mismatched == before.mismatched && statistics.unreplayed == before.unreplayed,
        "the same calls all answered");
    journal::Stop();
    CHECK(g_journalReleases == 0, "the dropped release never made");
    journal::Describe({});
}

// A session's input delays (netplay/session.h: DelaysFor and FitsFor), at the points session 2's and session 9's
// matrices measured on this PC (ANALYSIS.md, Phase 6, Route 2 under network conditions; Rollback in sessions' fights):
// lockstep's delay with a frame to spare, and a rollback fight's from what fits.
static void test_netplay_delays()
{
    namespace session = netplay::session;
    // What fits beside a frame, as measured (the matrix of The input delays a session chooses): two instances on 16 logical
    // processors, a replayed frame 2.18 ms, a present one 2.13, a load 0.92 after 3.67 ms of waiting for the GPU's draws:
    // 6 frames; on 2 logical processors, 5.49, 5.21, 0.82 and 3.57 ms: 2; one instance a PC (a replayed frame 1.75 ms): 7.
    CHECK(session::FitsFor(2180, 2130, 920, 3670, 8) == 6, "%u", session::FitsFor(2180, 2130, 920, 3670, 8));
    CHECK(session::FitsFor(5490, 5210, 820, 3570, 8) == 2, "%u", session::FitsFor(5490, 5210, 820, 3570, 8));
    CHECK(session::FitsFor(1750, 1800, 800, 3500, 8) == 7, "%u", session::FitsFor(1750, 1800, 800, 3500, 8));
    // The wait, the load and a first replayed frame longer than a frame: none fits; nor with a present frame that leaves
    // the GPU no time; at most the window.
    CHECK(session::FitsFor(13000, 13000, 1000, 3500, 8) == 0, "%u", session::FitsFor(13000, 13000, 1000, 3500, 8));
    CHECK(session::FitsFor(8000, 14000, 1000, 3500, 8) == 0, "%u", session::FitsFor(8000, 14000, 1000, 3500, 8));
    CHECK(session::FitsFor(500, 500, 500, 2000, 8) == 8, "%u", session::FitsFor(500, 500, 500, 2000, 8));
    // Round trips (mean, 95th percentile) with 2 ms of work: lockstep needs ceil((RTT / 2 + 2) / 16.7) - 1, and takes one
    // more (session 9's best: 3 at 70 ms, 4-5 at 130, 7 at 210, 10 at 310); rollback with 6 fitting, at least 2 (or the
    // frames the other's input takes, if fewer), and 4 at 310 ms (session 9's best).
    struct Point
    {
        double rtt, high;
        uint32_t late, needed, lockstep, rollback6, rollback1;
    };
    const Point points[] = {
        { 100, 300, 1, 0, 1, 1, 1 },               // no latency: no rollback at 1
        { 70000, 78000, 3, 2, 3, 2, 2 },           // 30 ms, 10 ms of jitter
        { 130000, 138000, 5, 4, 5, 2, 4 },         // 60 ms
        { 210000, 218000, 7, 6, 7, 2, 6 },         // 100 ms
        { 310000, 318000, 10, 9, 10, 4, 9 },       // 150 ms
        { 410000, 418000, 13, 12, 13, 7, 12 },     // 200 ms
        { 210000, 280000, 7, 6, 8, 2, 6 },         // a 95th percentile far above the mean: what it needs
    };
    for (const Point& p : points)
    {
        const session::DelayChoice choice = session::DelaysFor(p.rtt, p.high, 2000, 6);
        CHECK(choice.late == p.late && choice.needed == p.needed && choice.lockstep == p.lockstep && choice.rollback == p.rollback6,
            "%.0f us: late %u, needed %u, lockstep %u, rollback %u", p.rtt, choice.late, choice.needed, choice.lockstep, choice.rollback);
        CHECK(session::DelaysFor(p.rtt, p.high, 2000, 1).rollback == p.rollback1, "%.0f us: rollback %u with 1 fitting", p.rtt,
            session::DelaysFor(p.rtt, p.high, 2000, 1).rollback);
    }
    // Bounded: a round trip of 10 s takes the most delay there is (30 frames).
    CHECK(session::DelaysFor(10e6, 10e6, 2000, 0).lockstep == 30 && session::DelaysFor(10e6, 10e6, 2000, 0).rollback == 30);
}

// The store map's miss handler (ppc_overrides.h), which the generated code calls with every register live
// (preserve_all): it must give them all back whole, the YMM registers' upper halves too, where the generated code keeps
// VMX's floating point in double precision. Clang ended it with a vzeroupper, which cleared them after restoring them:
// whenever the page log watched pages (savestates, rollback), the cloth and hair simulations' vectors lost two of their
// lanes, and Kasumi's sash and hair stretched across the screen (ANALYSIS.md, Phase 6).
static void test_store_miss_preserves_registers()
{
    // The 16 YMM registers, 4 words each, then rax, rcx, rdx, rsi, rdi, r8, r9 and r10 (preserve_all keeps every
    // general purpose register but r11; rcx and rdx are set to the arguments after their own check).
    constexpr size_t kWords = 16 * 4 + 8;
    uint64_t in[kWords], out[kWords] = {};
    for (size_t i = 0; i < kWords; i++)
        in[i] = 0x0123456789ABCDEFull * uint64_t(i + 1) ^ (uint64_t(i) << 56);
    const uint32_t address = guest::SystemAlloc(0x1000);
    for (int armed = 0; armed < 2; armed++)
    {
        // A miss on a page the page log armed (it isn't logging here, so the miss just quiets the page), and one on a quiet page.
        guest::StoreMap()[address >> guest::kStorePageShift] = uint8_t(armed);
        in[64 + 1] = address; // rcx, rdx: the arguments, as the generated code passes them
        in[64 + 2] = 4;
        __asm__ volatile(
            "vmovdqu 0x000(%[in]), %%ymm0\n\t"
            "vmovdqu 0x020(%[in]), %%ymm1\n\t"
            "vmovdqu 0x040(%[in]), %%ymm2\n\t"
            "vmovdqu 0x060(%[in]), %%ymm3\n\t"
            "vmovdqu 0x080(%[in]), %%ymm4\n\t"
            "vmovdqu 0x0A0(%[in]), %%ymm5\n\t"
            "vmovdqu 0x0C0(%[in]), %%ymm6\n\t"
            "vmovdqu 0x0E0(%[in]), %%ymm7\n\t"
            "vmovdqu 0x100(%[in]), %%ymm8\n\t"
            "vmovdqu 0x120(%[in]), %%ymm9\n\t"
            "vmovdqu 0x140(%[in]), %%ymm10\n\t"
            "vmovdqu 0x160(%[in]), %%ymm11\n\t"
            "vmovdqu 0x180(%[in]), %%ymm12\n\t"
            "vmovdqu 0x1A0(%[in]), %%ymm13\n\t"
            "vmovdqu 0x1C0(%[in]), %%ymm14\n\t"
            "vmovdqu 0x1E0(%[in]), %%ymm15\n\t"
            "movq 0x200(%[in]), %%rax\n\t"
            "movq 0x208(%[in]), %%rcx\n\t"
            "movq 0x210(%[in]), %%rdx\n\t"
            "movq 0x218(%[in]), %%rsi\n\t"
            "movq 0x220(%[in]), %%rdi\n\t"
            "movq 0x228(%[in]), %%r8\n\t"
            "movq 0x230(%[in]), %%r9\n\t"
            "movq 0x238(%[in]), %%r10\n\t"
            // A call as the generated code makes it: the stack aligned, with the shadow space Windows' convention has.
            "movq %%rsp, %%rbx\n\t"
            "andq $-16, %%rsp\n\t"
            "subq $32, %%rsp\n\t"
            "callq PPCStoreMiss\n\t"
            "movq %%rbx, %%rsp\n\t"
            "vmovdqu %%ymm0, 0x000(%[out])\n\t"
            "vmovdqu %%ymm1, 0x020(%[out])\n\t"
            "vmovdqu %%ymm2, 0x040(%[out])\n\t"
            "vmovdqu %%ymm3, 0x060(%[out])\n\t"
            "vmovdqu %%ymm4, 0x080(%[out])\n\t"
            "vmovdqu %%ymm5, 0x0A0(%[out])\n\t"
            "vmovdqu %%ymm6, 0x0C0(%[out])\n\t"
            "vmovdqu %%ymm7, 0x0E0(%[out])\n\t"
            "vmovdqu %%ymm8, 0x100(%[out])\n\t"
            "vmovdqu %%ymm9, 0x120(%[out])\n\t"
            "vmovdqu %%ymm10, 0x140(%[out])\n\t"
            "vmovdqu %%ymm11, 0x160(%[out])\n\t"
            "vmovdqu %%ymm12, 0x180(%[out])\n\t"
            "vmovdqu %%ymm13, 0x1A0(%[out])\n\t"
            "vmovdqu %%ymm14, 0x1C0(%[out])\n\t"
            "vmovdqu %%ymm15, 0x1E0(%[out])\n\t"
            "movq %%rax, 0x200(%[out])\n\t"
            "movq %%rcx, 0x208(%[out])\n\t"
            "movq %%rdx, 0x210(%[out])\n\t"
            "movq %%rsi, 0x218(%[out])\n\t"
            "movq %%rdi, 0x220(%[out])\n\t"
            "movq %%r8, 0x228(%[out])\n\t"
            "movq %%r9, 0x230(%[out])\n\t"
            "movq %%r10, 0x238(%[out])\n\t"
            "vzeroupper\n\t"
            :
            : [in] "r"(in), [out] "r"(out)
            : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
            "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
        static const char* const kGprs[] = { "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10" };
        for (size_t i = 0; i < kWords; i++)
        {
            if (i < 64)
                CHECK(out[i] == in[i], "%s miss: ymm%zu's word %zu %016llX, was %016llX", armed ? "an armed page's" : "a quiet page's", i / 4, i % 4,
                    (unsigned long long)out[i], (unsigned long long)in[i]);
            else
                CHECK(out[i] == in[i], "%s miss: %s %016llX, was %016llX", armed ? "an armed page's" : "a quiet page's", kGprs[i - 64],
                    (unsigned long long)out[i], (unsigned long long)in[i]);
        }
        CHECK(guest::StoreMap()[address >> guest::kStorePageShift] == 0, "the page quiet after its miss");
    }
}

// The C runtime's functions the executable exports to the game's code module (module/crt_exports.h): each export is a
// jump to the C runtime's function. lld-link had bound the export sqrt to the C++ library's sqrt(long double), which
// looped without end for a negative input, and the CPU fights froze when their particles took the square root of
// -0.000184 (ANALYSIS.md, Phase 6, The CPU fights' hang). The executable beside the tests must export each from its
// jump, and the jumps must give what the C runtime's functions give, bit for bit, for every kind of input.
static void test_module_crt_exports()
{
    static const char* const kNames[] = { "_setjmp", "longjmp", "memcpy", "memset", "sqrt", "sqrtf", "fma", "exp2f", "log2f" };
    wchar_t self[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::filesystem::path executable = std::filesystem::path(std::wstring(self, length)).parent_path() / (GAME_EXECUTABLE ".exe");
    if (HMODULE image = LoadLibraryExW(executable.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE))
    {
        // Mapped as an image (sections at their addresses), not loaded: the handle's low bits say so.
        const auto* base = reinterpret_cast<const uint8_t*>(uintptr_t(image) & ~uintptr_t(3));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        CHECK(directory.VirtualAddress != 0, "%ls exports nothing", executable.c_str());
        if (directory.VirtualAddress)
        {
            const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
            const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
            const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
            const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
            for (const char* name : kNames)
            {
                DWORD rva = 0;
                for (DWORD i = 0; i < exports->NumberOfNames && !rva; i++)
                    if (strcmp(reinterpret_cast<const char*>(base + names[i]), name) == 0)
                        rva = functions[ordinals[i]];
                CHECK(rva != 0, "%s not exported", name);
                if (!rva)
                    continue;
                // jmp rel32 to another function (the export of sqrt was xorps, ucomisd, ja, then a jump to its own start).
                const uint8_t* code = base + rva;
                int32_t displacement;
                memcpy(&displacement, code + 1, sizeof(displacement));
                CHECK(code[0] == 0xE9 && displacement != -5, "the export %s isn't a jump to the C runtime's (%02X %02X %02X %02X)", name,
                    code[0], code[1], code[2], code[3]);
            }
        }
        FreeLibrary(image);
    }
    else
        printf("  (no %ls: its exports aren't checked)\n", executable.c_str());

    // The jumps against the C runtime's functions themselves, called through pointers (this executable exports nothing,
    // so they're the C runtime's), on finite, zero, negative, denormal, infinite and NaN inputs.
    double (*volatile crtSqrt)(double) = &::sqrt;
    float (*volatile crtSqrtf)(float) = &::sqrtf;
    double (*volatile crtFma)(double, double, double) = &::fma;
    float (*volatile crtExp2f)(float) = &::exp2f;
    float (*volatile crtLog2f)(float) = &::log2f;
    const uint64_t doubles[] = { 0x4010000000000000, 0x4000000000000000, 0, 0x8000000000000000, 0xBFF0000000000000,
        0xBF281F9A20000000 /* the fight's */, 0xFFF0000000000000, 0x7FF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000,
        0x7FF0000000000001, 0x0000000000000001, 0x800FFFFFFFFFFFFF, 0x7FEFFFFFFFFFFFFF, 0x3FB999999999999A };
    int mismatches = 0;
    for (uint64_t a : doubles)
    {
        const double x = std::bit_cast<double>(a);
        CHECK(std::bit_cast<uint64_t>(ModuleCrt_sqrt(x)) == std::bit_cast<uint64_t>(crtSqrt(x)), "sqrt of %016llX: %016llX, the C runtime's %016llX",
            (unsigned long long)a, (unsigned long long)std::bit_cast<uint64_t>(ModuleCrt_sqrt(x)), (unsigned long long)std::bit_cast<uint64_t>(crtSqrt(x)));
        const float f = float(x);
        CHECK(std::bit_cast<uint32_t>(ModuleCrt_sqrtf(f)) == std::bit_cast<uint32_t>(crtSqrtf(f)), "sqrtf of %08X", std::bit_cast<uint32_t>(f));
        for (uint64_t b : doubles)
            for (uint64_t c : doubles)
            {
                const double y = std::bit_cast<double>(b), z = std::bit_cast<double>(c);
                mismatches += std::bit_cast<uint64_t>(ModuleCrt_fma(x, y, z)) != std::bit_cast<uint64_t>(crtFma(x, y, z));
            }
    }
    CHECK(mismatches == 0, "%d of fma's 3,375 results differ from the C runtime's", mismatches);
    CHECK(std::isnan(ModuleCrt_sqrt(std::bit_cast<double>(0xBF281F9A20000000ull))), "the fight's square root isn't NaN");
    mismatches = 0;
    for (uint32_t i = 0; i < 4096; i++)
    {
        const float f = std::bit_cast<float>(i * 0x00100801u); // every exponent, both signs, NaNs and infinities among them
        mismatches += std::bit_cast<uint32_t>(ModuleCrt_exp2f(f)) != std::bit_cast<uint32_t>(crtExp2f(f));
        mismatches += std::bit_cast<uint32_t>(ModuleCrt_log2f(f)) != std::bit_cast<uint32_t>(crtLog2f(f));
    }
    CHECK(mismatches == 0, "%d of exp2f's and log2f's 8,192 results differ from the C runtime's", mismatches);

    uint8_t source[67], destination[67];
    for (size_t i = 0; i < sizeof(source); i++)
        source[i] = uint8_t(i * 37 + 1);
    CHECK(ModuleCrt_memcpy(destination, source, sizeof(source)) == destination && memcmp(destination, source, sizeof(source)) == 0);
    CHECK(ModuleCrt_memset(destination + 3, 0xA5, 60) == destination + 3);
    CHECK(destination[2] == source[2] && destination[3] == 0xA5 && destination[62] == 0xA5 && destination[63] == source[63]);

    // setjmp and longjmp through the jumps, as clang calls _setjmp (with the frame longjmp unwinds to): the context saved
    // is the caller's, since a jump leaves its return address and stack as they were.
    jmp_buf buffer;
    volatile int jumps = 0;
    const int value = ModuleCrt__setjmp(buffer, __builtin_frame_address(0));
    if (value == 0)
    {
        jumps = jumps + 1;
        ModuleCrt_longjmp(buffer, 7);
    }
    CHECK(value == 7 && jumps == 1, "setjmp returned %d after %d jumps", value, int(jumps));
}

#define RUN(test) (printf("- %s\n", #test), test())

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    RUN(test_heap_alloc_alignment_and_order);
    RUN(test_heap_range_and_exhaustion);
    RUN(test_heap_fixed_commit_release);
    RUN(test_heap_decommit_and_protect);
    RUN(test_heap_top_of_address_space);
    RUN(test_settings_file);
    RUN(test_disc_image);
    RUN(test_spa);

    if (!guest::Initialize())
    {
        printf("can't reserve guest memory\n");
        return 1;
    }
    RUN(test_nt_allocate_and_free);
    RUN(test_nt_allocate_large_pages_and_fixed);
    RUN(test_physical_memory);
    RUN(test_xam_alloc);
    RUN(test_strings);
    RUN(test_wide_printf);
    RUN(test_time);
    RUN(test_memory_ulong);
    RUN(test_module);
    RUN(test_achievements);
    RUN(test_events);
    RUN(test_threads);
    RUN(test_critical_section_contention);
    RUN(test_pulse_and_spinlock);
    RUN(test_suspension);
    RUN(test_semaphores);
    RUN(test_hard_disk_partition);
    RUN(test_crypto);
    RUN(test_app_enumerator);
    RUN(test_system_ui);
    RUN(test_profile_settings);
    RUN(test_timers);
    RUN(test_live_offline);
    RUN(test_raise_exception);
    RUN(test_music_player);
    RUN(test_xma_registers);
    RUN(test_system_modules);
    RUN(test_shader_binaries);
    RUN(test_xma_decoding);
    RUN(test_journal_divergence);
    RUN(test_store_miss_preserves_registers);
    RUN(test_module_crt_exports);
    RUN(test_netplay_delays);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
