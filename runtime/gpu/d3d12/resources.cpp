#include "gpu/d3d12/resources.h"

#include "log.h"
#include "util/profiler.h"

#include <algorithm>
#include <cstdlib>

namespace gpu::d3d12
{
    ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
        D3D12_RESOURCE_FLAGS flags)
    {
        D3D12_HEAP_PROPERTIES heap{ heapType };
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = size;
        description.Height = description.DepthOrArraySize = description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = flags;
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr, IID_PPV_ARGS(&buffer))))
        {
            LOG_ERROR("D3D12: can't create a %llu-byte buffer", (unsigned long long)size);
            return nullptr;
        }
        return buffer;
    }

    D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint32_t subresource)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        return barrier;
    }

    // UploadAllocator

    void UploadAllocator::Initialize(ID3D12Device* device, uint64_t pageSize)
    {
        m_device = device;
        m_pageSize = pageSize;
    }

    UploadAllocator::Page UploadAllocator::CreatePage(uint64_t size)
    {
        Page page;
        page.size = size;
        page.buffer = CreateBuffer(m_device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!page.buffer)
            std::abort();
        D3D12_RANGE noRead{ 0, 0 };
        page.buffer->Map(0, &noRead, reinterpret_cast<void**>(&page.cpu));
        return page;
    }

    UploadAllocator::Allocation UploadAllocator::Allocate(uint64_t size, uint64_t alignment)
    {
        auto& used = m_used[m_frame];
        uint64_t offset = (m_offset + alignment - 1) & ~(alignment - 1);
        if (used.empty() || offset + size > used.back().size)
        {
            // A new page: a pooled one if it is big enough, else a new one (oversized for big requests).
            Page page;
            auto pooled = std::find_if(m_free.begin(), m_free.end(), [&](const Page& p) { return p.size >= size; });
            if (pooled != m_free.end())
            {
                page = std::move(*pooled);
                m_free.erase(pooled);
            }
            else
            {
                page = CreatePage(std::max(m_pageSize, (size + 0xFFFF) & ~uint64_t(0xFFFF)));
            }
            used.push_back(std::move(page));
            offset = 0;
        }
        Page& page = used.back();
        m_offset = offset + size;
        profile::Count(profile::Counter::kUploadHeapBytes, size);
        return { page.cpu + offset, page.buffer->GetGPUVirtualAddress() + offset, page.buffer.Get(), offset };
    }

    void UploadAllocator::BeginFrame(uint32_t frame)
    {
        m_frame = frame;
        for (Page& page : m_used[frame])
        {
            // Oversized pages are released rather than pooled.
            if (page.size == m_pageSize)
                m_free.push_back(std::move(page));
        }
        m_used[frame].clear();
        m_offset = 0;
    }

    // DescriptorHeap

    bool DescriptorHeap::Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t persistentCount, uint32_t perFrameCount)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = type;
        description.NumDescriptors = persistentCount + perFrameCount * kFramesInFlight;
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&m_heap))))
        {
            LOG_ERROR("D3D12: can't create a shader-visible descriptor heap of %u descriptors", description.NumDescriptors);
            return false;
        }
        m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
        m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
        m_increment = device->GetDescriptorHandleIncrementSize(type);
        m_persistentCount = persistentCount;
        m_perFrameCount = perFrameCount;
        m_frameStart = persistentCount;
        return true;
    }

    uint32_t DescriptorHeap::AllocatePersistent(uint32_t count)
    {
        if (count == 1 && !m_persistentFree.empty())
        {
            const uint32_t index = m_persistentFree.back();
            m_persistentFree.pop_back();
            return index;
        }
        if (m_persistentUsed + count > m_persistentCount)
        {
            LOG_ERROR("D3D12: out of persistent descriptors");
            std::abort();
        }
        const uint32_t index = m_persistentUsed;
        m_persistentUsed += count;
        return index;
    }

    void DescriptorHeap::FreePersistent(uint32_t index)
    {
        m_persistentFree.push_back(index);
    }

    uint32_t DescriptorHeap::AllocateTransient(uint32_t count)
    {
        if (m_frameUsed + count > m_perFrameCount)
            return UINT32_MAX;
        const uint32_t index = m_frameStart + m_frameUsed;
        m_frameUsed += count;
        return index;
    }

    void DescriptorHeap::BeginFrame(uint32_t frame)
    {
        m_frameStart = m_persistentCount + frame * m_perFrameCount;
        m_frameUsed = 0;
    }

    // CpuDescriptorPool

    bool CpuDescriptorPool::Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = type;
        description.NumDescriptors = count;
        if (FAILED(device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&m_heap))))
            return false;
        m_start = m_heap->GetCPUDescriptorHandleForHeapStart();
        m_increment = device->GetDescriptorHandleIncrementSize(type);
        for (uint32_t i = count; i-- > 0;)
            m_free.push_back(i);
        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorPool::Allocate()
    {
        if (m_free.empty())
        {
            LOG_ERROR("D3D12: out of CPU descriptors");
            std::abort();
        }
        const uint32_t index = m_free.back();
        m_free.pop_back();
        return { m_start.ptr + SIZE_T(index) * m_increment };
    }

    void CpuDescriptorPool::Free(D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        m_free.push_back(uint32_t((handle.ptr - m_start.ptr) / m_increment));
    }
}
