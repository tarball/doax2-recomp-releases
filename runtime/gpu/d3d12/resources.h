#pragma once

// Per-frame transient resources: upload memory and shader-visible descriptors.
#include "gpu/d3d12/device.h"

#include <cstdint>
#include <vector>

namespace gpu::d3d12
{
    inline constexpr uint32_t kFramesInFlight = 3;

    // Linear allocation from persistently mapped upload-heap buffers; a frame's buffers are reused
    // once the GPU has finished that frame.
    class UploadAllocator
    {
    public:
        struct Allocation
        {
            uint8_t* cpu = nullptr;
            D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
            ID3D12Resource* buffer = nullptr;
            uint64_t offset = 0;
        };

        void Initialize(ID3D12Device* device, uint64_t pageSize);
        Allocation Allocate(uint64_t size, uint64_t alignment);
        // The GPU has finished with everything allocated during `frame`'s previous use.
        void BeginFrame(uint32_t frame);

    private:
        struct Page
        {
            ComPtr<ID3D12Resource> buffer;
            uint8_t* cpu = nullptr;
            uint64_t size = 0;
        };
        Page CreatePage(uint64_t size);

        ID3D12Device* m_device = nullptr;
        uint64_t m_pageSize = 0;
        std::vector<Page> m_free;
        std::vector<Page> m_used[kFramesInFlight];
        uint32_t m_frame = 0;
        uint64_t m_offset = 0; // in the last page of m_used[m_frame]
    };

    // A shader-visible descriptor heap: a persistent region at the start, then a ring split evenly
    // between the frames in flight.
    class DescriptorHeap
    {
    public:
        bool Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t persistentCount, uint32_t perFrameCount);
        ID3D12DescriptorHeap* Get() const { return m_heap.Get(); }
        uint32_t Increment() const { return m_increment; }

        // Index of `count` contiguous descriptors: persistent ones live forever, transient ones until
        // the frame is reused. Transient allocation returns UINT32_MAX when the frame's ring is full.
        uint32_t AllocatePersistent(uint32_t count);
        void FreePersistent(uint32_t index); // single descriptors only
        uint32_t AllocateTransient(uint32_t count);
        void BeginFrame(uint32_t frame);

        D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const { return { m_cpuStart.ptr + SIZE_T(index) * m_increment }; }
        D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const { return { m_gpuStart.ptr + UINT64(index) * m_increment }; }

    private:
        ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};
        uint32_t m_increment = 0;
        uint32_t m_persistentCount = 0, m_persistentUsed = 0;
        std::vector<uint32_t> m_persistentFree;
        uint32_t m_perFrameCount = 0;
        uint32_t m_frameStart = 0, m_frameUsed = 0;
    };

    // CPU-only descriptors (RTVs, DSVs, and staging copies of views) with a free list.
    class CpuDescriptorPool
    {
    public:
        bool Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count);
        D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
        void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle);

    private:
        ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE m_start{};
        uint32_t m_increment = 0;
        std::vector<uint32_t> m_free;
    };

    // Helpers.
    D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
        uint32_t subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource);
    ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
}
