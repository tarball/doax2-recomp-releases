#pragma once

// Performance measurement (diagnostics).
//
// PROFILE=<seconds> reports, every that many seconds:
//  - CPU time per thread (runtime threads are named, guest threads by their start address);
//  - the GPU command processor's time per frame by phase, and zones inside draws and swaps;
//  - GPU time per frame (timestamp queries), and event counters per frame.
// PROFILE_SAMPLE=<hz> also samples the call stacks of the threads that used at least 3% of a CPU
// in the previous report's interval, and reports the functions they spend their time in (self and
// inclusive). Recompiled guest functions appear as sub_XXXXXXXX: every guest call is a host call.
//
// When PROFILE isn't set, every entry point returns after testing one flag.
#include <cstdint>

namespace profile
{
    // Names the calling thread (printf-style) for debuggers and the reports.
    void NameThread(const char* format, ...);

    bool Enabled();

    // Starts the report (and sample) threads when enabled. `framesPresented` returns the number of
    // frames presented so far.
    void Start(uint64_t (*framesPresented)());

    // Time on the command processor thread. Phases are exclusive: at any moment the thread is in
    // exactly one, so they add up to its wall time. Zones are inclusive timers inside a phase.
    enum class Zone : uint8_t
    {
        kIdle,          // ring buffer empty
        kExecute,       // PM4 packets, except the phases below
        kWaitRegMem,    // WAIT_REG_MEM polling
        kWaitInterrupt, // a command processor interrupt's guest callback
        kShaderLoad,    // IM_LOAD: hashing, and translating new shaders
        kDraw,          // the renderer's draw
        kSwap,          // the renderer's swap
        kPhaseCount,

        kDrawResolve = kPhaseCount, // EDRAM resolves (copy draws)
        kDrawPipeline,              // pipeline lookup and creation
        kDrawUploads,               // vertex and index buffers: watch validation and uploads
        kDrawTextures,              // texture and sampler bindings, including texture loads
        kDrawConstants,             // system, float, bool/loop/fetch constants
        kDrawRecord,                // render pass and D3D12 command recording
        kSwapSubmit,                // ExecuteCommandLists
        kSwapPresent,               // IDXGISwapChain::Present
        kSwapFenceWait,             // waiting for the GPU to free a frame's resources
        kCount
    };

    // Command processor thread only. Returns the previous phase.
    Zone SetPhase(Zone phase);
    // Command processor thread only: ends the frame's accounting (at each swap).
    void EndFrame();

    class ScopedPhase
    {
    public:
        explicit ScopedPhase(Zone phase) : m_previous(SetPhase(phase)) {}
        ~ScopedPhase() { SetPhase(m_previous); }
        ScopedPhase(const ScopedPhase&) = delete;
        ScopedPhase& operator=(const ScopedPhase&) = delete;

    private:
        Zone m_previous;
    };

    // Command processor thread only.
    class ScopedZone
    {
    public:
        explicit ScopedZone(Zone zone);
        ~ScopedZone() { Stop(); }
        // Ends the zone before the end of the scope.
        void Stop();
        ScopedZone(const ScopedZone&) = delete;
        ScopedZone& operator=(const ScopedZone&) = delete;

    private:
        Zone m_zone;
        uint64_t m_start;
    };

    // GPU time of a finished frame (renderer thread).
    void RecordGpuTime(double milliseconds);

    // Event counters, reported per frame. Any thread.
    enum class Counter : uint8_t
    {
        kWatchFaults,       // CPU writes to write-protected (uploaded) pages
        kProtectCalls,      // VirtualProtect calls by the memory watch
        kSharedUploadBytes, // guest memory copied to the GPU
        kUploadHeapBytes,   // all upload heap allocations (constants, staged guest memory, indices)
        kTextureLoads,
        kResolves,
        kDrawPackets,
        kDrawsRecorded,
        kPipelinesCreated,
        kShaderLoads,       // IM_LOAD packets
        kShaderLoadDwords,
        kInterrupts,        // guest interrupt callbacks posted
        kCount
    };
    void Count(Counter counter, uint64_t amount = 1);

    // Prints every thread's host call stack (but the caller's), up to `maxDepth` frames each, to stderr: for a hang, where
    // each thread waits, the runtime's waits too, which the guest threads' call chains don't show. Each stack is walked
    // with its thread suspended a moment, one thread at a time; functions are named from the PDBs in the executable's
    // folder (none in a release: then the module and the offset), the game's code by its guest address (sub_XXXXXXXX).
    // Works whether PROFILE is set or not.
    void DumpHostStacks(uint32_t maxDepth = 24);
    // The same on the calling thread, which must handle a bad frame's fault itself (crash::SuppressReportsOnThisThread).
    void DumpHostStacksHere(uint32_t maxDepth = 24);
}
