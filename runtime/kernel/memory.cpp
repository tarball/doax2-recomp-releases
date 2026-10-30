// xboxkrnl and XAM memory management exports.
#include "guest_abi.h"
#include "kernel/status.h"
#include "log.h"
#include "netplay/heaps.h"

#include <cstring>

namespace
{
    constexpr uint32_t kMemCommit = 0x00001000;
    constexpr uint32_t kMemReserve = 0x00002000;
    constexpr uint32_t kMemDecommit = 0x00004000;
    constexpr uint32_t kMemRelease = 0x00008000;
    constexpr uint32_t kMemFree = 0x00010000;
    constexpr uint32_t kMemPrivate = 0x00020000;
    constexpr uint32_t kMemReset = 0x00080000;
    constexpr uint32_t kMemTopDown = 0x00100000;
    constexpr uint32_t kMemNoZero = 0x00800000;
    constexpr uint32_t kMemHeap = 0x40000000;

    constexpr uint32_t kAllowedAllocationFlags = kMemCommit | kMemReserve | kMemReset | kMemTopDown | kMemNoZero |
        guest::kMemLargePages | kMemHeap | guest::k16MBPages;

    // Page protection without the page-size bits that may be OR'ed into it.
    uint32_t PlainProtect(uint32_t protect)
    {
        return protect & ~(guest::kMemLargePages | guest::k16MBPages);
    }

    struct MemoryBasicInformation
    {
        be<uint32_t> baseAddress;
        be<uint32_t> allocationBase;
        be<uint32_t> allocationProtect;
        be<uint32_t> regionSize;
        be<uint32_t> state;
        be<uint32_t> protect;
        be<uint32_t> type;
    };
    static_assert(sizeof(MemoryBasicInformation) == 0x1C);

    uint32_t NtAllocateVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t allocationType, uint32_t protect, uint32_t /*debugMemory*/,
        PPCContext* ctx)
    {
        if (!baseAddress || !regionSize || *regionSize == 0)
            return status::kInvalidParameter;
        if (!(allocationType & (kMemCommit | kMemReserve)) || (allocationType & ~kAllowedAllocationFlags))
            return status::kInvalidParameter;
        if (allocationType & kMemReset)
            return status::kSuccess; // contents may be discarded; nothing to do

        const uint32_t pageSize = (allocationType & guest::kMemLargePages) ? 0x10000 : 0x1000;
        const uint32_t requested = *baseAddress;
        const uint32_t adjustedBase = requested - requested % pageSize;
        const uint64_t adjustedSize = (uint64_t(*regionSize) + (requested - adjustedBase) + pageSize - 1) / pageSize * pageSize;
        if (adjustedSize > 0xFFFFFFFFull)
            return status::kNoMemory;

        const bool reserve = allocationType & kMemReserve;
        const bool commit = allocationType & kMemCommit;
        const uint32_t pageProtect = PlainProtect(protect);

        std::lock_guard lock(guest::HeapLock());
        uint32_t address = 0;
        if (adjustedBase != 0)
        {
            // Physical views are only allocated by MmAllocatePhysicalMemoryEx (which owns their pages).
            if (adjustedBase >= guest::kSharedViewA)
                return status::kInvalidParameter;
            PageHeap* heap = guest::LookupHeap(adjustedBase);
            if (!heap)
                return status::kInvalidParameter;
            // The page size flag must match the heap at that address.
            if (heap->PageSize() != pageSize)
                return status::kAccessDenied;
            if (!heap->AllocFixed(adjustedBase, uint32_t(adjustedSize), reserve, commit, pageProtect))
                return status::kNoMemory;
            address = adjustedBase;
        }
        else
        {
            // 4 KB pages come from the top of 00000000-3FFFFFFF, not from 0x10000 up. DOA4 loads files into a
            // 4 KB-page arena and relocates their offset tables in place, taking a value at or above the buffer's
            // address for one already relocated (sub_823FB900), so on hardware the arena must lie above every
            // offset. At 0x10000, Brad Wong's motion scripts (a table past offset 0x10000 in his animation file)
            // ran on into other data: in character select he and Ryu Hayabusa squashed into nothing with an
            // explosion sound, and selecting Brad crashed.
            const bool topDown = (allocationType & kMemTopDown) || pageSize == 0x1000;
            address = guest::VirtualHeap(pageSize)->Alloc(uint32_t(adjustedSize), pageSize, commit, pageProtect, topDown);
            if (address == 0)
                return status::kNoMemory;
        }

        if (commit && !(allocationType & kMemNoZero))
        {
            guest::BeforeHostWrite(address, adjustedSize);
            memset(guest::ToHost(address), 0, size_t(adjustedSize));
        }

        *baseAddress = address;
        *regionSize = uint32_t(adjustedSize);
        LOG_TRACE("NtAllocateVirtualMemory(%08X, 0x%X, type 0x%X) -> %08X (from %08X)", requested, uint32_t(adjustedSize), allocationType, address,
            uint32_t(ctx->lr));
        // A new region (reserved, or at an address chosen here): a heap's memory, if a heap function asked.
        if (reserve || adjustedBase == 0)
            netplay::heaps::VirtualReserved(address, uint32_t(adjustedSize), uint32_t(ctx->lr), ctx->r1.u32);
        return status::kSuccess;
    }

    uint32_t NtFreeVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t freeType, uint32_t /*debugMemory*/)
    {
        if (!baseAddress || *baseAddress == 0)
            return status::kMemoryNotAllocated;

        // Physical memory is only released by MmFreePhysicalMemory. DOA4 VirtualFrees its physical
        // level heap and keeps using it, so the 360 kernel must refuse this.
        if (*baseAddress >= guest::kSharedViewA)
            return status::kMemoryNotAllocated;

        std::lock_guard lock(guest::HeapLock());
        PageHeap* heap = guest::LookupHeap(*baseAddress);
        if (!heap)
            return status::kMemoryNotAllocated;

        const uint32_t pageSize = heap->PageSize();
        const uint32_t address = *baseAddress - *baseAddress % pageSize;

        if (freeType == kMemDecommit)
        {
            const uint32_t size = regionSize ? uint32_t(*regionSize) : 0;
            if (size == 0 || !heap->Decommit(address, size))
                return status::kMemoryNotAllocated;
            netplay::heaps::VirtualDecommitted(address);
            *baseAddress = address;
            if (regionSize)
                *regionSize = (size + pageSize - 1) / pageSize * pageSize;
            return status::kSuccess;
        }

        if (freeType != kMemRelease)
            return status::kInvalidParameter;

        uint32_t released = 0;
        if (!heap->Release(address, &released))
            return status::kMemoryNotAllocated;
        netplay::heaps::VirtualReleased(address);

        *baseAddress = address;
        if (regionSize)
            *regionSize = released;
        return status::kSuccess;
    }

    uint32_t NtQueryVirtualMemory(uint32_t baseAddress, MemoryBasicInformation* info, uint32_t /*debugMemory*/)
    {
        if (!info)
            return status::kInvalidParameter;

        std::lock_guard lock(guest::HeapLock());
        PageHeap* heap = guest::LookupHeap(baseAddress);
        PageHeap::RegionInfo region;
        if (!heap || !heap->QueryRegion(baseAddress, region))
            return status::kInvalidParameter;

        info->baseAddress = region.baseAddress;
        info->allocationBase = region.allocationBase;
        info->allocationProtect = region.allocationProtect;
        info->regionSize = region.regionSize;
        info->state = region.state == PageHeap::State::Free ? kMemFree
            : region.state == PageHeap::State::Reserved      ? kMemReserve
                                                              : kMemCommit;
        info->protect = region.protect;
        info->type = region.state == PageHeap::State::Free ? 0 : kMemPrivate;
        return status::kSuccess;
    }

    uint32_t MmAllocatePhysicalMemoryEx(uint32_t /*flags*/, uint32_t regionSize, uint32_t protect, uint32_t minAddress, uint32_t maxAddress, uint32_t alignment)
    {
        const uint32_t pageProtect = PlainProtect(protect);
        if (!(pageProtect & (guest::kPageReadOnly | guest::kPageReadWrite)) || regionSize == 0)
            return 0;

        const uint32_t pageSize = (protect & guest::kMemLargePages) ? 0x10000 : (protect & guest::k16MBPages) ? 0x1000000 : 0x1000;
        return guest::AllocPhysical(regionSize, pageSize, pageProtect, minAddress, maxAddress, alignment);
    }

    void MmFreePhysicalMemory(uint32_t /*type*/, uint32_t baseAddress)
    {
        guest::FreePhysical(baseAddress);
    }

    uint32_t MmGetPhysicalAddress(uint32_t address)
    {
        return guest::PhysicalAddress(address);
    }

    uint32_t MmQueryAddressProtect(uint32_t address)
    {
        std::lock_guard lock(guest::HeapLock());
        PageHeap* heap = guest::LookupHeap(address);
        return heap ? heap->QueryProtect(address) : 0;
    }

    void MmSetAddressProtect(uint32_t address, uint32_t size, uint32_t protect)
    {
        std::lock_guard lock(guest::HeapLock());
        if (PageHeap* heap = guest::LookupHeap(address))
            heap->Protect(address, size, PlainProtect(protect));
    }

    // XAM's allocator. Bit 0x00100000 in flags requests physical memory.
    uint32_t XamAlloc(uint32_t flags, uint32_t size, be<uint32_t>* result)
    {
        if (!result)
            return xerror::kInvalidParameter;

        const uint32_t address = (flags & 0x00100000)
            ? guest::AllocPhysical(size, 0x1000, guest::kPageReadWrite, 0, 0xFFFFFFFF, 0x1000)
            : guest::SystemAlloc(size);
        *result = address;
        return address ? xerror::kSuccess : xerror::kNotEnoughMemory;
    }

    uint32_t XamFree(uint32_t address)
    {
        if (address >= 0xA0000000)
            guest::FreePhysical(address);
        else
            guest::SystemFree(address);
        return xerror::kSuccess;
    }

    // The kernel's pool, from the runtime's system heap (whole pages; XAPI only uses it for a file copy's buffer).
    uint32_t ExAllocatePool(uint32_t size)
    {
        return size ? guest::SystemAlloc(size) : 0;
    }

    void ExFreePool(uint32_t address)
    {
        if (address)
            guest::SystemFree(address);
    }
}

GUEST_EXPORT(NtAllocateVirtualMemory, NtAllocateVirtualMemory)
GUEST_EXPORT(NtFreeVirtualMemory, NtFreeVirtualMemory)
GUEST_EXPORT(NtQueryVirtualMemory, NtQueryVirtualMemory)
GUEST_EXPORT(MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryEx)
GUEST_EXPORT(MmFreePhysicalMemory, MmFreePhysicalMemory)
GUEST_EXPORT(MmGetPhysicalAddress, MmGetPhysicalAddress)
GUEST_EXPORT(MmQueryAddressProtect, MmQueryAddressProtect)
GUEST_EXPORT(MmSetAddressProtect, MmSetAddressProtect)
GUEST_EXPORT(XamAlloc, XamAlloc)
GUEST_EXPORT(XamFree, XamFree)
GUEST_EXPORT(ExAllocatePool, ExAllocatePool)
GUEST_EXPORT(ExFreePool, ExFreePool)
