#include "page_heap.h"

#include <algorithm>
#include <cassert>

PageHeap::PageHeap(uint32_t base, uint64_t size, uint32_t pageSize)
    : m_base(base), m_end(uint64_t(base) + size), m_pageSize(pageSize), m_pages(size / pageSize)
{
    assert(pageSize != 0 && (pageSize & (pageSize - 1)) == 0);
    assert(base % pageSize == 0 && size % pageSize == 0);
}

bool PageHeap::RangeToPages(uint64_t address, uint64_t size, uint32_t& first, uint32_t& end) const
{
    if (size == 0 || address < m_base || address + size > m_end)
        return false;

    first = uint32_t((address - m_base) / m_pageSize);
    end = uint32_t((address + size - m_base + m_pageSize - 1) / m_pageSize);
    return true;
}

void PageHeap::CreateRegion(uint32_t first, uint32_t count, bool commit, uint32_t protect)
{
    for (uint32_t i = first; i < first + count; i++)
    {
        Page& page = m_pages[i];
        page.regionBase = first;
        page.regionPages = (i == first) ? count : 0;
        page.protect = commit ? protect : 0;
        page.allocationProtect = protect;
        page.state = commit ? State::Committed : State::Reserved;
    }
}

uint32_t PageHeap::AllocRange(uint64_t low, uint64_t high, uint32_t size, uint32_t alignment, bool commit, uint32_t protect, bool topDown)
{
    if (size == 0)
        return 0;

    const uint64_t pages = (uint64_t(size) + m_pageSize - 1) / m_pageSize;
    const uint64_t align = std::max<uint64_t>(alignment, m_pageSize);

    low = std::max<uint64_t>(low, m_base);
    high = std::min<uint64_t>(high, m_end);
    if (low >= high || pages * m_pageSize > high - low)
        return 0;

    // Candidate start addresses are multiples of `align`, so the alignment holds for the absolute address.
    const auto alignUp = [&](uint64_t value) { return (value + align - 1) / align * align; };
    const auto alignDown = [&](uint64_t value) { return value / align * align; };
    const auto indexOf = [&](uint64_t address) { return uint32_t((address - m_base) / m_pageSize); };

    // Returns the index of the highest allocated page in the candidate run, or -1 if the run is free.
    const auto blocker = [&](uint64_t start) -> int64_t {
        const uint32_t first = indexOf(start);
        for (int64_t i = int64_t(first + pages) - 1; i >= int64_t(first); i--)
        {
            if (m_pages[size_t(i)].state != State::Free)
                return i;
        }
        return -1;
    };

    if (!topDown)
    {
        for (uint64_t start = alignUp(low); start + pages * m_pageSize <= high;)
        {
            const int64_t blocked = blocker(start);
            if (blocked < 0)
            {
                CreateRegion(indexOf(start), uint32_t(pages), commit, protect);
                return uint32_t(start);
            }
            start = alignUp(m_base + uint64_t(blocked + 1) * m_pageSize);
        }
    }
    else
    {
        for (int64_t start = int64_t(alignDown(high - pages * m_pageSize)); start >= int64_t(alignUp(low));)
        {
            const int64_t blocked = blocker(uint64_t(start));
            if (blocked < 0)
            {
                CreateRegion(indexOf(uint64_t(start)), uint32_t(pages), commit, protect);
                return uint32_t(start);
            }
            // Next candidate must end at or below the blocking page.
            const int64_t limit = int64_t(m_base) + blocked * int64_t(m_pageSize) - int64_t(pages * m_pageSize);
            if (limit < 0)
                break;
            start = int64_t(alignDown(uint64_t(limit)));
        }
    }

    return 0;
}

bool PageHeap::AllocFixed(uint32_t address, uint32_t size, bool reserve, bool commit, uint32_t protect)
{
    uint32_t first, end;
    if ((!reserve && !commit) || !RangeToPages(address, size, first, end))
        return false;

    const bool allFree = std::all_of(m_pages.begin() + first, m_pages.begin() + end, [](const Page& p) { return p.state == State::Free; });
    if (allFree)
    {
        // Committing pages that were never reserved fails, as on the 360 (and Windows).
        if (!reserve)
            return false;
        CreateRegion(first, end - first, commit, protect);
        return true;
    }

    // Already-allocated pages can be committed, but only if every page in the range is allocated.
    const bool allAllocated = std::none_of(m_pages.begin() + first, m_pages.begin() + end, [](const Page& p) { return p.state == State::Free; });
    if (!commit || !allAllocated)
        return false;

    for (uint32_t i = first; i < end; i++)
    {
        m_pages[i].state = State::Committed;
        m_pages[i].protect = protect;
    }
    return true;
}

bool PageHeap::Decommit(uint32_t address, uint32_t size)
{
    uint32_t first, end;
    if (!RangeToPages(address, size, first, end))
        return false;

    for (uint32_t i = first; i < end; i++)
    {
        if (m_pages[i].state == State::Free)
            return false;
    }

    for (uint32_t i = first; i < end; i++)
    {
        m_pages[i].state = State::Reserved;
        m_pages[i].protect = 0;
    }
    return true;
}

bool PageHeap::Release(uint32_t address, uint32_t* releasedSize)
{
    if (!Contains(address) || (address - m_base) % m_pageSize != 0)
        return false;

    const uint32_t first = (address - m_base) / m_pageSize;
    const Page& head = m_pages[first];
    if (head.state == State::Free || head.regionBase != first)
        return false;

    const uint32_t count = head.regionPages;
    for (uint32_t i = first; i < first + count; i++)
        m_pages[i] = Page{};

    if (releasedSize)
        *releasedSize = count * m_pageSize;
    return true;
}

bool PageHeap::Protect(uint32_t address, uint32_t size, uint32_t protect)
{
    uint32_t first, end;
    if (!RangeToPages(address, size, first, end))
        return false;

    for (uint32_t i = first; i < end; i++)
    {
        if (m_pages[i].state != State::Committed)
            return false;
    }

    for (uint32_t i = first; i < end; i++)
        m_pages[i].protect = protect;
    return true;
}

bool PageHeap::QueryRegion(uint32_t address, RegionInfo& info) const
{
    if (!Contains(address))
        return false;

    const uint32_t first = (address - m_base) / m_pageSize;
    const Page& page = m_pages[first];

    uint32_t end = first + 1;
    while (end < m_pages.size())
    {
        const Page& next = m_pages[end];
        if (next.state != page.state || next.protect != page.protect)
            break;
        if (page.state != State::Free && next.regionBase != page.regionBase)
            break;
        end++;
    }

    info.baseAddress = PageAddress(first);
    info.regionSize = (end - first) * m_pageSize;
    info.state = page.state;
    info.protect = page.protect;
    if (page.state == State::Free)
    {
        info.allocationBase = 0;
        info.allocationProtect = 0;
    }
    else
    {
        info.allocationBase = PageAddress(page.regionBase);
        info.allocationProtect = page.allocationProtect;
    }
    return true;
}

uint32_t PageHeap::QueryProtect(uint32_t address) const
{
    if (!Contains(address))
        return 0;

    const Page& page = m_pages[(address - m_base) / m_pageSize];
    return page.state == State::Committed ? page.protect : 0;
}
