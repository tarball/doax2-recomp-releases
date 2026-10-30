#pragma once

// Resolution-scaled copies of what resolves write, for rendering above the guest's resolution: render targets are
// `scale` times larger in each direction, and textures that sample resolved memory (the scene before the HUD goes on,
// shadow maps, the front buffer) are loaded from these copies at the same scale.
//
// Each 4 KB page of guest physical memory that resolves write gets 4 KB x scale^2 of storage in a pool, through a
// page table the shaders read: guest texel bytes at page offset o hold their scale x scale block of host texels at
// o x scale^2, row by row. A page's copy is current while its memory version (gpu::watch) is the one the resolve left,
// so a CPU write to the page makes textures fall back to its guest data.
#include "gpu/d3d12/resources.h"

#include <cstdint>
#include <vector>

namespace gpu::d3d12
{
    class ScaledResolves
    {
    public:
        // Nothing is allocated until the first resolve.
        void Initialize(ID3D12Device* device, uint32_t scale);
        // Drops every copy and the pool, for another scale (with the GPU idle).
        void Reset(uint32_t scale);
        uint32_t Scale() const { return m_scale; }

        // Gives the pages of [physical, physical + length) storage, recording page table updates. False if the pool is full.
        bool Allocate(ID3D12GraphicsCommandList* list, UploadAllocator& upload, uint32_t physical, uint32_t length);
        // A resolve wrote these pages' copies (after watch::NoteGpuWrite) with texels of 2^log2Bpp bytes.
        void NoteWritten(uint32_t physical, uint32_t length, uint32_t log2Bpp);
        // Whether every page of [physical, physical + length) has a current copy with texels of 2^log2Bpp bytes.
        bool Covers(uint32_t physical, uint32_t length, uint32_t log2Bpp) const;

        bool Ready() const { return m_pool != nullptr; }
        D3D12_GPU_VIRTUAL_ADDRESS PoolAddress() const { return m_pool->GetGPUVirtualAddress(); }
        D3D12_GPU_VIRTUAL_ADDRESS PageTableAddress() const { return m_pageTable->GetGPUVirtualAddress(); }
        void TransitionPool(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state);
        void PoolUavBarrier(ID3D12GraphicsCommandList* list);
        void TransitionPageTable(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state);
        // Buffers decay to COMMON when a command list finishes executing: call before recording the next.
        void BeginCommandList();

        static constexpr uint32_t kPageShift = 12;
        // Guest pages the pool holds: 64 MB of guest memory (DOA4 resolves into about 45 MB).
        static constexpr uint32_t kPoolPages = 16384;

    private:
        ID3D12Device* m_device = nullptr;
        uint32_t m_scale = 1;
        ComPtr<ID3D12Resource> m_pool, m_pageTable;
        D3D12_RESOURCE_STATES m_poolState = D3D12_RESOURCE_STATE_COMMON, m_pageTableState = D3D12_RESOURCE_STATE_COMMON;
        uint32_t m_poolUsed = 0;
        bool m_reportedFull = false;
        std::vector<uint32_t> m_entries;   // per guest page: pool page + 1, or 0
        std::vector<uint32_t> m_versions;  // per guest page: memory version its copy matches
        std::vector<uint8_t> m_texelSizes; // per guest page: log2 bytes per texel + 1, or 0 for no current copy
    };
}
