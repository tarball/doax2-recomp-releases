#pragma once

// Which guest pages are written, and by whom, frame by frame (netplay's diagnostics: the state census and the sync test).
// While tracking, every writable page of the guest's virtual memory (below 0x80000000) and of the executable image is
// write-protected at each frame boundary. A frame's first write to a page faults into a handler that records it with
// its writer (the guest thread, its current fiber, and the guest function, or for runtime code the guest call site) and
// makes the page writable again. Byte tracking goes further for chosen pages: every write to them is single-stepped and
// the bytes it changed recorded with its writer. Not covered: the physical memory views (A0000000 and up), which the
// GPU's memory watch protects.
#include <bitset>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace netplay::tracker
{
    inline constexpr uint32_t kPageSize = 4096;
    inline constexpr uint32_t kWatchStart = 0x10000;
    inline constexpr uint32_t kWatchEnd = 0x90000000; // virtual memory and the image (the physical views start at A0000000)
    inline constexpr uint32_t kFileRead = 0xFFFFFFFE;  // Writer::function of a file read into guest memory

    struct Writer
    {
        uint32_t thread = 0;   // the guest thread's start address (0x80000000 | its id for a host entry); 0: a host thread
        uint32_t fiber = 0;    // the thread's current XAPI fiber record (0: none)
        uint32_t function = 0; // the guest function writing; runtime code: the guest caller found on the host stack | 1
        auto Key() const { return std::tie(thread, fiber, function); }
        bool operator<(const Writer& other) const { return Key() < other.Key(); }
        bool operator==(const Writer& other) const { return Key() == other.Key(); }
    };

    struct Write
    {
        uint32_t page;
        Writer writer;
    };

    // Called before a page's first write after a frame boundary, with the page still unwritten (readable), on the
    // writing thread, with the tracker's lock held.
    using BeforeWrite = void (*)(uint32_t page);

    // Starts tracking, at a frame boundary on the main thread (the fault handler is installed once, first in line).
    void Start(BeforeWrite beforeWrite = nullptr);
    // Replaces the hook; once this returns, the old one isn't running and won't be called again.
    void SetBeforeWrite(BeforeWrite beforeWrite);
    // At a frame boundary: the writes since the last one (each page's first), then every page is protected again.
    std::vector<Write> NextFrame();
    // Within a frame: protects the pages written since the frame began (or since the last Resample) again, so a later
    // write to one is recorded too, with its writer. A frame's writes then hold a page as often as it was sampled.
    void Resample();
    // Stops tracking (every page writable again) and returns the writes since the last boundary.
    std::vector<Write> Stop();
    bool Active();
    // Writes since the last boundary that couldn't be recorded (the buffer was full).
    uint64_t LostWrites();

    // The bytes of a page one writer changed while its bytes were tracked.
    struct ByteWrites
    {
        uint32_t page = 0;
        Writer writer;
        std::bitset<kPageSize> bytes;
    };

    // Byte tracking (while tracking, at a frame boundary): from now on every write to these pages is single-stepped,
    // and the bytes it changed (within 64 bytes of where it starts) are recorded with its writer; their writes aren't
    // recorded as Writes meanwhile, and the BeforeWrite hook sees each page once, now. Slow: thousands of writes a frame
    // to a page cost milliseconds.
    void TrackBytes(const std::vector<uint32_t>& pages);
    struct ByteTracking
    {
        std::vector<ByteWrites> writes;
        uint64_t steps = 0; // writes single-stepped
        uint64_t lost = 0;  // writes whose bytes weren't recorded (one that also wrote a second such page, or too many writers)
    };
    // Stops byte tracking, after which the pages are tracked as the others are, and returns what it recorded.
    ByteTracking StopTrackingBytes();

    // The host kernel writes guest memory (a file read): pages there must be writable first, and stay so until
    // FinishHostWrite, since a write by the kernel to a protected page fails rather than faulting (boundaries and
    // resamples meanwhile leave them be). Called whether tracking or not: tracking may start during the write.
    void PrepareHostWrite(uint32_t guestAddress, uint32_t length);
    void FinishHostWrite(uint32_t guestAddress, uint32_t length);
    // Writes a page's contents without recording a write (a savestate's load), leaving its protection as it was; with
    // `mask`, only the bytes it has set.
    void WriteUntracked(uint32_t page, const void* contents, const std::bitset<kPageSize>* mask = nullptr);

    // "thread 82370638 fiber 400D6F30 sub_82357438", "... runtime, called from 827921C0", "... file read".
    std::string WriterText(const Writer& writer);
    std::string Hex(uint32_t value);
}
