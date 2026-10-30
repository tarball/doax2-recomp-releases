#pragma once

#include <cstdint>
#include <vector>

// Page-granular bookkeeping for one range of guest address space, modelled on the Xbox 360
// kernel's view of memory: pages are free, reserved or committed, and belong to allocation
// regions that are released as a whole. The host memory behind it is always committed, so this
// only decides which guest addresses are handed out.
//
// Not thread safe; callers hold guest::HeapLock().
class PageHeap
{
public:
    enum class State : uint8_t
    {
        Free,
        Reserved,
        Committed,
    };

    struct RegionInfo
    {
        uint32_t baseAddress;
        uint32_t allocationBase;
        uint32_t allocationProtect;
        uint32_t regionSize;
        uint32_t protect;
        State state;
    };

    PageHeap(uint32_t base, uint64_t size, uint32_t pageSize);

    uint32_t Base() const { return m_base; }
    uint64_t End() const { return m_end; }
    uint32_t PageSize() const { return m_pageSize; }
    bool Contains(uint32_t address) const { return address >= m_base && address < m_end; }

    // Allocates a new region of `size` bytes (rounded up to pages) whose address is a multiple of
    // `alignment` and lies entirely within [low, high). Returns 0 if nothing fits.
    uint32_t AllocRange(uint64_t low, uint64_t high, uint32_t size, uint32_t alignment, bool commit, uint32_t protect, bool topDown);

    uint32_t Alloc(uint32_t size, uint32_t alignment, bool commit, uint32_t protect, bool topDown)
    {
        return AllocRange(m_base, m_end, size, alignment, commit, protect, topDown);
    }

    // Reserves and/or commits the pages covering [address, address + size). Free pages become a new
    // region (requires `reserve`). Pages that are already allocated can only be committed (commit
    // without reserve, or reserve + commit), never reserved again.
    bool AllocFixed(uint32_t address, uint32_t size, bool reserve, bool commit, uint32_t protect);

    // Turns committed pages in the range back into reserved pages.
    bool Decommit(uint32_t address, uint32_t size);

    // Frees the whole region starting at `address` (which must be a region base).
    bool Release(uint32_t address, uint32_t* releasedSize = nullptr);

    // Changes the protection of committed pages in the range.
    bool Protect(uint32_t address, uint32_t size, uint32_t protect);

    // Describes the run of pages starting at `address` that share a state and protection.
    bool QueryRegion(uint32_t address, RegionInfo& info) const;

    // Protection of the page containing `address`, or 0 if it isn't committed.
    uint32_t QueryProtect(uint32_t address) const;

    // Whether the page containing `address` belongs to an allocation (reserved or committed).
    bool IsAllocated(uint32_t address) const
    {
        return Contains(address) && m_pages[(address - m_base) / m_pageSize].state != State::Free;
    }

private:
    struct Page
    {
        uint32_t regionBase = 0;  // page index of the region this page belongs to
        uint32_t regionPages = 0; // only valid on a region's first page
        uint32_t protect = 0;
        uint32_t allocationProtect = 0;
        State state = State::Free;
    };

    bool RangeToPages(uint64_t address, uint64_t size, uint32_t& first, uint32_t& end) const;
    uint32_t PageAddress(uint32_t index) const { return uint32_t(m_base + uint64_t(index) * m_pageSize); }
    void CreateRegion(uint32_t first, uint32_t count, bool commit, uint32_t protect);

    uint32_t m_base;
    uint64_t m_end;
    uint32_t m_pageSize;
    std::vector<Page> m_pages;
};
