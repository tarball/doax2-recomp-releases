#include "gpu/d3d12/shared_memory.h"

#include "gpu/memory_watch.h"
#include "guest_memory.h"
#include "log.h"
#include "util/profiler.h"

#include <cstring>

namespace gpu::d3d12
{
    bool SharedMemory::Initialize(ID3D12Device* device)
    {
        m_buffer = CreateBuffer(device, kSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        m_state = D3D12_RESOURCE_STATE_COMMON;
        return m_buffer != nullptr;
    }

    void SharedMemory::BeginFrame(ID3D12GraphicsCommandList* uploadList)
    {
        m_uploadList = uploadList;
        m_framePages.clear();
        m_state = D3D12_RESOURCE_STATE_COMMON;
    }

    void SharedMemory::Transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state)
    {
        if (m_state == state)
            return;
        if (beforeWork)
            beforeWork();
        const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(m_buffer.Get(), m_state, state);
        list->ResourceBarrier(1, &barrier);
        m_state = state;
    }

    void SharedMemory::UavBarrier(ID3D12GraphicsCommandList* list)
    {
        if (beforeWork)
            beforeWork();
        const D3D12_RESOURCE_BARRIER barrier = d3d12::UavBarrier(m_buffer.Get());
        list->ResourceBarrier(1, &barrier);
    }

    void SharedMemory::UploadRun(void* context, uint32_t firstPage, uint32_t pageCount, uint32_t guestAddress)
    {
        auto* self = static_cast<SharedMemory*>(context);
        const uint8_t* source = guest::ToHost(guestAddress);
        const uint32_t end = firstPage + pageCount;
        for (uint32_t page = firstPage; page < end;)
        {
            const uint8_t* pageSource = source + (size_t(page - firstPage) << watch::kPageShift);
            if (auto staged = self->m_framePages.find(page); staged != self->m_framePages.end())
            {
                // Uploaded earlier this frame: the copy hasn't executed yet, so refresh its data.
                memcpy(staged->second, pageSource, watch::kPageSize);
                watch::RecordUpload(page, 1, staged->second);
                profile::Count(profile::Counter::kSharedUploadBytes, watch::kPageSize);
                page++;
                continue;
            }
            uint32_t runEnd = page + 1;
            while (runEnd < end && !self->m_framePages.contains(runEnd))
                runEnd++;
            const uint64_t size = uint64_t(runEnd - page) << watch::kPageShift;
            const UploadAllocator::Allocation allocation = self->m_upload->Allocate(size, 4096);
            memcpy(allocation.cpu, pageSource, size);
            watch::RecordUpload(page, runEnd - page, allocation.cpu);
            // (The buffer is in COMMON in the upload list, implicitly promoted to COPY_DEST.)
            self->m_uploadList->CopyBufferRegion(self->m_buffer.Get(), uint64_t(page) << watch::kPageShift, allocation.buffer, allocation.offset, size);
            for (uint32_t p = page; p < runEnd; p++)
                self->m_framePages[p] = allocation.cpu + (size_t(p - page) << watch::kPageShift);
            self->uploadedBytes += size;
            profile::Count(profile::Counter::kSharedUploadBytes, size);
            page = runEnd;
        }
    }

    void SharedMemory::RequestRange(UploadAllocator& upload, uint32_t physical, uint32_t length)
    {
        m_upload = &upload;
        watch::Validate(physical, length, &SharedMemory::UploadRun, this);
    }
}
