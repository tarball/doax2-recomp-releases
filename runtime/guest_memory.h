#pragma once

#include "page_heap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// Guest (Xbox 360) address space. The whole 32-bit space is reserved up front and committed below
// 0xE0000000, so a guest address is just an offset from g_base, which is what the recompiled code
// assumes. The heaps below only decide which addresses are handed out; they mirror the 360 kernel's
// layout:
//
//   0x00000000-0x3FFFFFFF  virtual, 4 KB pages      NtAllocateVirtualMemory default; system allocations
//   0x40000000-0x7EFFFFFF  virtual, 64 KB pages     MEM_LARGE_PAGES; thread stacks from 0x70000000
//   0x7F000000-0x7FFFFFFF  virtual, 64 KB pages     reserved for the system
//   0x80000000-0x8FFFFFFF  image, 64 KB pages       XEX image at 0x82000000 + indirect call table after it
//   0x90000000-0x9FFFFFFF  virtual, 4 KB pages
//   0xA0000000-0xBFFFFFFF  physical view, 4 KB and 64 KB pages  \  one 512 MB physical page pool,
//   0xC0000000-0xDFFFFFFF  physical view, 16 MB pages            /  both views mapping it
//   0xE0000000-0xFFFFFFFF  not mapped
//
// The A0000000 and C0000000 views map the same host memory (a 512 MB section), so a physical page
// can be accessed through either, as D3D does: it converts the addresses it keeps back to C0000000
// ones, then reads and patches vertex shaders through them. On hardware, 4 KB-page physical memory
// is handed out through a third view, E0000000, which maps the pool 4 KB further on. Windows can't
// map a view at that offset (its mapping granularity is 64 KB), so the A0000000 view hands out
// 4 KB-page memory as well and E0000000 stays inaccessible: a stray access there faults. Titles only
// get E0000000 addresses from the kernel's allocator, and D3D computes GPU addresses for both views.
//
// The store map lies just below g_base: a byte for each 4 KB page of the guest's 4 GB, which every store the guest
// makes checks first (runtime/ppc_overrides.h, which reaches it from the generated code's `base`), as host code does
// before it writes guest memory (BeforeHostWrite). A page's byte is 0 unless netplay's page log watches it
// (netplay/page_log.h): then the frame's first store to the page copies it out before it's written.
namespace guest
{
    inline constexpr size_t kAddressSpaceSize = 0x100000000ull;
    inline constexpr uint32_t kStorePageShift = 12;
    inline constexpr size_t kStoreMapSize = kAddressSpaceSize >> kStorePageShift; // 1 MB, at g_base - kStoreMapSize
    inline constexpr uint32_t kSharedViewA = 0xA0000000; // physical memory aliases
    inline constexpr uint32_t kSharedViewC = 0xC0000000;
    inline constexpr uint32_t kViewE = 0xE0000000; // not mapped; ends the shared views

    // Xbox 360 page protection and allocation flags.
    inline constexpr uint32_t kPageNoAccess = 0x01;
    inline constexpr uint32_t kPageReadOnly = 0x02;
    inline constexpr uint32_t kPageReadWrite = 0x04;
    inline constexpr uint32_t kMemLargePages = 0x20000000;
    inline constexpr uint32_t k16MBPages = 0x80000000;

    extern uint8_t* g_base;

    // Reserves the guest address space and sets up the heaps. Returns false if the host refuses.
    bool Initialize();

    // Held around any heap operation (guest threads allocate concurrently).
    std::recursive_mutex& HeapLock();

    // Heap owning a guest address (virtual heaps, image region, or physical views), or nullptr.
    PageHeap* LookupHeap(uint32_t address);

    // Default virtual heap for NtAllocateVirtualMemory with the given page size (4 KB or 64 KB).
    PageHeap* VirtualHeap(uint32_t pageSize);

    // Allocates physical memory with `pageSize` pages (4 KB, 64 KB or 16 MB; size and alignment are
    // rounded up to a page), constrained to the physical range [minPhysical, maxPhysical]. Returns its
    // address in the A0000000 view (C0000000 for 16 MB pages), or 0.
    uint32_t AllocPhysical(uint32_t size, uint32_t pageSize, uint32_t protect, uint32_t minPhysical, uint32_t maxPhysical, uint32_t alignment);
    bool FreePhysical(uint32_t address);
    // Called (under the heap lock) with each freed physical range, e.g. to drop GPU copies of it.
    void SetPhysicalFreeHook(void (*hook)(uint32_t physical, uint32_t size));

    // Physical address of a guest address in one of the physical views (identity otherwise).
    uint32_t PhysicalAddress(uint32_t address);

    // Guest address for a physical address as seen by the GPU (its A0000000 alias). Values of
    // 0x20000000 and above aren't physical (PhysicalAddress is identity for them) and pass through.
    uint32_t PhysicalToGuest(uint32_t physical);

    // Zeroed, page-granular allocations for runtime use (PCRs, kernel variables, large buffers).
    uint32_t SystemAlloc(uint32_t size);
    void SystemFree(uint32_t address);

    // Zeroed small allocations (up to 4 KB, 16-byte aligned) for kernel objects such as events.
    // `size` must be passed back to KernelFree.
    uint32_t KernelAlloc(uint32_t size);
    void KernelFree(uint32_t address, uint32_t size);

    // Allocates a committed thread stack in 0x70000000-0x7EFFFFFF. Returns its lowest address, or 0.
    uint32_t AllocStack(uint32_t size);

    // Marks a fixed range (e.g. the image) as committed so it's never handed out and queries work.
    bool ReserveFixed(uint32_t address, uint32_t size, uint32_t protect);

    // Whether the page containing `address` belongs to an allocation (reserved or committed), in any heap or view.
    bool IsAllocated(uint32_t address);

    // The guest call chain from `stackPointer` up: the return addresses saved in its stack frames. Each frame's back
    // chain is at 0(r1), and the caller's return address is saved 8 bytes below the previous frame's top.
    std::vector<uint32_t> CallChain(uint32_t stackPointer, int maxFrames = 16);
    // Prints CallChain.
    void PrintBacktrace(uint32_t stackPointer, int maxFrames = 16);

    inline uint8_t* ToHost(uint32_t address)
    {
        return g_base + address;
    }

    // The store map's byte for a page (guest address >> kStorePageShift).
    inline uint8_t* StoreMap()
    {
        return g_base - kStoreMapSize;
    }

    // A write the store map's bytes ask to see first (netplay's page log): [address, address + length), whose pages it
    // copies out as needed before the write. Any thread.
    void StoreMiss(uint32_t address, uint32_t length);
    // The same for a store of the generated code's, from PPCStoreMiss (store_miss.cpp), with its return address.
    void GuestStoreMiss(uint32_t address, uint32_t size, uintptr_t returnAddress);

    // Host code is about to write guest memory: [address, address + length). Cheap when nothing watches those pages
    // (a load of the store map's byte for each), as for the guest's own stores.
    inline void BeforeHostWrite(uint32_t address, uint64_t length)
    {
        if (!length)
            return;
        const uint8_t* map = StoreMap();
        const uint64_t last = std::min<uint64_t>(uint64_t(address) + length - 1, kAddressSpaceSize - 1) >> kStorePageShift;
        for (uint64_t page = address >> kStorePageShift; page <= last; page++)
        {
            if (map[page])
            {
                StoreMiss(address, uint32_t(std::min<uint64_t>(length, kAddressSpaceSize - address)));
                return;
            }
        }
    }

    // The same for a host pointer, which may not point into guest memory (then nothing happens).
    inline void BeforeHostWriteTo(const void* host, size_t length)
    {
        const uintptr_t offset = uintptr_t(static_cast<const uint8_t*>(host)) - uintptr_t(g_base);
        if (g_base && offset < kAddressSpaceSize)
            BeforeHostWrite(uint32_t(offset), length);
    }

    inline void StoreU32(uint32_t address, uint32_t value)
    {
        BeforeHostWrite(address, 4);
        *reinterpret_cast<uint32_t*>(ToHost(address)) = __builtin_bswap32(value);
    }

    inline uint32_t LoadU32(uint32_t address)
    {
        return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(ToHost(address)));
    }
}
