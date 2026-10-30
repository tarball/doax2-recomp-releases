#pragma once

// The GPU's copy of guest physical memory: a 512 MB buffer indexed by physical address. Vertex
// fetch, index buffers and texture loads read it; resolves write it. Pages are uploaded from CPU
// memory on demand, as tracked by gpu::watch.
//
// Uploads are recorded into a separate command list that executes before the frame's. With copies
// recorded between draws, later draws sometimes read stale data on Qualcomm's driver, whatever the
// barriers (DOA4's HUD sprites, uploaded batch by batch, flickered). A page uploaded twice in a frame
// just gets its staged data refreshed, as its copy hasn't executed yet.
#include "gpu/d3d12/resources.h"

#include <functional>
#include <unordered_map>

namespace gpu::d3d12
{
    class SharedMemory
    {
    public:
        static constexpr uint32_t kSize = 0x20000000;

        bool Initialize(ID3D12Device* device);
        ID3D12Resource* Buffer() const { return m_buffer.Get(); }
        D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const { return m_buffer->GetGPUVirtualAddress(); }

        // Starts a frame: uploads go to `uploadList` (executed first), and as buffers decay to COMMON when
        // command lists finish executing, the frame's list starts with the buffer in that state.
        void BeginFrame(ID3D12GraphicsCommandList* uploadList);
        // Brings [physical, physical + length) up to date for the frame's work.
        void RequestRange(UploadAllocator& upload, uint32_t physical, uint32_t length);

        // Transitions the buffer (tracked state) in the frame's command list.
        void Transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state);
        // A UAV barrier between two writing passes.
        void UavBarrier(ID3D12GraphicsCommandList* list);

        // State for reading by shaders and the input assembler.
        static constexpr D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATES(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER);

        uint64_t uploadedBytes = 0; // statistics

        // Called before the buffer records a barrier in the frame's list, which must be outside render passes.
        std::function<void()> beforeWork;

    private:
        static void UploadRun(void* context, uint32_t firstPage, uint32_t pageCount, uint32_t guestAddress);

        ComPtr<ID3D12Resource> m_buffer;
        D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
        ID3D12GraphicsCommandList* m_uploadList = nullptr;
        UploadAllocator* m_upload = nullptr; // during RequestRange
        std::unordered_map<uint32_t, uint8_t*> m_framePages; // pages uploaded this frame -> staged data
    };
}
