#pragma once

// Tracks which 4 KB pages of guest physical memory the GPU's copy holds up to date.
//
// A page becomes valid when the renderer uploads it (or the GPU writes it, e.g. a resolve); valid
// pages are write-protected on the host, so the first CPU write faults, invalidates the page and
// unprotects it again. Writes the host kernel makes into guest memory (ReadFile into a guest buffer)
// don't fault but fail, so such callers prepare the range with PrepareHostWrite first.
#include <cstdint>

namespace gpu::watch
{
    inline constexpr uint32_t kPageShift = 12;
    inline constexpr uint32_t kPageSize = 1u << kPageShift;
    inline constexpr uint32_t kPhysicalSize = 0x20000000;
    inline constexpr uint32_t kPageCount = kPhysicalSize >> kPageShift;

    // Installs the write fault handler (call once at startup, before guest code runs).
    void Install();
    // DEBUG_WATCH_WRITERS: logs the guest functions that wrote uploaded pages in the chosen range.
    void DumpWriters();

    // VERIFY_UPLOADS=1: Validate also checks pages it believes valid against a copy of what was
    // uploaded, reporting writes the watch missed.
    bool Verifying();
    // When verifying: records the bytes uploaded for pages (what the GPU's copy now holds), and reads them.
    void RecordUpload(uint32_t firstPage, uint32_t pageCount, const void* data);
    const uint8_t* Uploaded(uint32_t physical);

    // Makes the pages covering [physical, physical + length) valid. For each run of pages that
    // weren't, `upload(firstPage, pageCount, guestAddress)` is called after the run is protected, so
    // a concurrent CPU write invalidates it again. Called by the renderer thread only.
    using UploadCallback = void (*)(void* context, uint32_t firstPage, uint32_t pageCount, uint32_t guestAddress);
    void Validate(uint32_t physical, uint32_t length, UploadCallback upload, void* context);

    // The GPU wrote the range (its copy is now newer than CPU memory): bumps the page versions.
    void NoteGpuWrite(uint32_t physical, uint32_t length);

    // Host code is about to write guest memory in kernel mode: unprotects and invalidates the range.
    void PrepareHostWrite(uint32_t guestAddress, uint32_t length);

    // The physical range was freed: its contents are meaningless and its pages may be reallocated
    // through another view.
    void InvalidatePhysical(uint32_t physical, uint32_t length);

    // Sum of the version counters over the pages of a range; changes whenever any page is
    // invalidated or written by the GPU. Used to detect stale cached textures.
    uint64_t RangeVersion(uint32_t physical, uint32_t length);
    // One page's version counter.
    uint32_t PageVersion(uint32_t page);

    // The write faults the calling thread has taken (diagnostics): how many, and the time spent in the handler
    // (QueryPerformanceCounter ticks; the exception's dispatch before it isn't counted).
    struct FaultStatistics
    {
        uint64_t faults = 0;
        int64_t handlerTicks = 0;
    };
    FaultStatistics ThreadFaults();
}
