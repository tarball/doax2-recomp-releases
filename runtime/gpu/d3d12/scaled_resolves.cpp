#include "gpu/d3d12/scaled_resolves.h"

#include "gpu/memory_watch.h"
#include "log.h"

#include <algorithm>
#include <cstring>

namespace gpu::d3d12
{
    namespace
    {
        constexpr uint32_t kGuestPages = watch::kPhysicalSize >> ScaledResolves::kPageShift;
        constexpr uint32_t kPageSize = 1u << ScaledResolves::kPageShift;
    }

    void ScaledResolves::Initialize(ID3D12Device* device, uint32_t scale)
    {
        m_device = device;
        m_scale = scale;
    }

    void ScaledResolves::Reset(uint32_t scale)
    {
        m_scale = scale;
        m_pool.Reset();
        m_pageTable.Reset();
        m_poolState = m_pageTableState = D3D12_RESOURCE_STATE_COMMON;
        m_poolUsed = 0;
        m_reportedFull = false;
        m_entries.clear();
        m_versions.clear();
        m_texelSizes.clear();
    }

    bool ScaledResolves::Allocate(ID3D12GraphicsCommandList* list, UploadAllocator& upload, uint32_t physical, uint32_t length)
    {
        if (m_scale < 2 || !length || physical >= watch::kPhysicalSize)
            return false;
        if (!m_pool)
        {
            const uint64_t poolBytes = uint64_t(kPoolPages) * kPageSize * m_scale * m_scale;
            m_pool = CreateBuffer(m_device, poolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            m_pageTable = CreateBuffer(m_device, uint64_t(kGuestPages) * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
            if (!m_pool || !m_pageTable)
            {
                LOG_ERROR("D3D12: can't create the resolution scaling pool (%llu MB)", (unsigned long long)(poolBytes >> 20));
                m_scale = 1;
                m_pool.Reset();
                m_pageTable.Reset();
                return false;
            }
            m_entries.assign(kGuestPages, 0);
            m_versions.assign(kGuestPages, 0);
            m_texelSizes.assign(kGuestPages, 0);
            LOG_INFO("GPU: resolution scaling pool of %llu MB", (unsigned long long)(poolBytes >> 20));
        }

        const uint32_t first = physical >> kPageShift;
        const uint32_t last = uint32_t(std::min<uint64_t>(uint64_t(physical) + length - 1, watch::kPhysicalSize - 1)) >> kPageShift;
        for (uint32_t page = first; page <= last;)
        {
            if (m_entries[page])
            {
                page++;
                continue;
            }
            // A run of pages without storage: allocate them and upload their page table entries.
            uint32_t end = page;
            while (end <= last && !m_entries[end])
                end++;
            const uint32_t count = end - page;
            if (m_poolUsed + count > kPoolPages)
            {
                if (!m_reportedFull)
                {
                    m_reportedFull = true;
                    LOG_WARN("GPU: the resolution scaling pool is full (%u pages): further resolves stay at the guest's resolution", kPoolPages);
                }
                return false;
            }
            const auto allocation = upload.Allocate(uint64_t(count) * 4, 4);
            uint32_t* entries = reinterpret_cast<uint32_t*>(allocation.cpu);
            for (uint32_t i = 0; i < count; i++)
            {
                m_entries[page + i] = ++m_poolUsed;
                entries[i] = m_entries[page + i];
            }
            TransitionPageTable(list, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(m_pageTable.Get(), uint64_t(page) * 4, allocation.buffer, allocation.offset, uint64_t(count) * 4);
            page = end;
        }
        return true;
    }

    void ScaledResolves::NoteWritten(uint32_t physical, uint32_t length, uint32_t log2Bpp)
    {
        if (!m_pool || !length)
            return;
        const uint32_t first = physical >> kPageShift;
        const uint32_t last = uint32_t(std::min<uint64_t>(uint64_t(physical) + length - 1, watch::kPhysicalSize - 1)) >> kPageShift;
        for (uint32_t page = first; page <= last; page++)
        {
            if (!m_entries[page])
                continue;
            m_versions[page] = watch::PageVersion(page);
            m_texelSizes[page] = uint8_t(log2Bpp + 1);
        }
    }

    bool ScaledResolves::Covers(uint32_t physical, uint32_t length, uint32_t log2Bpp) const
    {
        if (!m_pool || !length || physical >= watch::kPhysicalSize)
            return false;
        const uint32_t first = physical >> kPageShift;
        const uint32_t last = uint32_t(std::min<uint64_t>(uint64_t(physical) + length - 1, watch::kPhysicalSize - 1)) >> kPageShift;
        for (uint32_t page = first; page <= last; page++)
        {
            if (!m_entries[page] || m_texelSizes[page] != log2Bpp + 1 || m_versions[page] != watch::PageVersion(page))
                return false;
        }
        return true;
    }

    void ScaledResolves::TransitionPool(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state)
    {
        if (!m_pool || m_poolState == state)
            return;
        const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(m_pool.Get(), m_poolState, state);
        list->ResourceBarrier(1, &barrier);
        m_poolState = state;
    }

    void ScaledResolves::PoolUavBarrier(ID3D12GraphicsCommandList* list)
    {
        if (!m_pool)
            return;
        const D3D12_RESOURCE_BARRIER barrier = UavBarrier(m_pool.Get());
        list->ResourceBarrier(1, &barrier);
    }

    void ScaledResolves::TransitionPageTable(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state)
    {
        if (!m_pageTable || m_pageTableState == state)
            return;
        const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(m_pageTable.Get(), m_pageTableState, state);
        list->ResourceBarrier(1, &barrier);
        m_pageTableState = state;
    }

    void ScaledResolves::BeginCommandList()
    {
        m_poolState = D3D12_RESOURCE_STATE_COMMON;
        m_pageTableState = D3D12_RESOURCE_STATE_COMMON;
    }
}
