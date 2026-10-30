#pragma once

// Xenos command processor. The game's statically linked Direct3D writes PM4 packets into a ring
// buffer and publishes the write pointer through the GPU register window at 0x7FC80000. This
// processor consumes the ring and executes every packet with an effect the CPU can observe
// (register writes, memory writes, fences, waits, interrupts). Draws are counted but not rendered.
#include <atomic>
#include <cstdint>

namespace gpu
{
    inline constexpr uint32_t kRegisterWindow = 0x7FC80000; // CPU-visible GPU registers (MMIO)
    inline constexpr uint32_t kRegisterWindowSize = 0x80000;

    // Called from the Vd* exports.
    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t userData);

    // Starts the command processor and vblank threads (idempotent).
    void Start();

    struct Statistics
    {
        std::atomic<uint64_t> swaps{ 0 };
        std::atomic<uint64_t> draws{ 0 };
        std::atomic<uint64_t> packets{ 0 };
        std::atomic<uint64_t> vblanks{ 0 };
        std::atomic<uint64_t> predicatedSkips{ 0 }; // type 3 packets skipped by predication (bin mask/select)
    };
    Statistics& GetStatistics();

    // Netplay's time sync (netplay/session.h): the game's frames are paced by the vblanks, so a machine whose frames run
    // ahead of the other's slows down by having its vblanks come later, `microseconds` in all, at most `perVblank` a vblank,
    // a little over a few frames rather than a frame's stop. Adds to the delay still to come.
    void DelayVblanks(uint64_t microseconds, uint32_t perVblank);
    // The delay still to come, in microseconds, and the delay given in all.
    uint64_t PendingVblankDelay();
    uint64_t VblankDelayGiven();

    // Prints the most recent type 3 packets the command processor executed and its ring state
    // (diagnostics; recording costs a few stores per packet).
    void DumpRecentPackets(uint32_t count);

    // For guest code that spins waiting for the GPU (D3D's wait loops): blocks until the command processor makes
    // progress (advances the ring's read pointer, writes memory, delivers an interrupt) since the calling thread's
    // last call, or for a millisecond at most.
    void WaitForProgress();
    // The calling thread's time blocked in WaitForProgress, in all (microseconds): netplay measures the main thread's work
    // a frame without its waits for the GPU (netplay/frame.h).
    uint64_t ThreadWaitedMicroseconds();

    // Frames whose rendering is discarded (netplay's replays, netplay/replay.h): the frames the guest records from
    // BeginDiscard until EndDiscard, counted by their swaps (VdSwap: NoteSwapRecorded), from the first swap recorded after
    // BeginDiscard to the last before EndDiscard. The command processor still executes everything in them the CPU
    // observes (register and memory writes, fences, interrupts and so D3D's callbacks, shader loads), but renders no
    // draw or resolve and presents no swap, and their swaps don't wait for a vblank: the frame pacing's wait before each
    // swap is released at once (WAIT_REG_MEM on memory before XE_SWAP), as D3D's vblank handler would release it. The
    // command processor runs behind the guest, so it tells frames apart by the swaps it has executed. On the main thread.
    void BeginDiscard();
    void EndDiscard();
    void NoteSwapRecorded();
    // Whether the frame the command processor executes now (its next swap's) is discarded: for the guest's interrupt
    // callbacks, which it waits for (D3D's swap callback, whose frame's pacing a discarded frame releases at once).
    bool DiscardingCurrentFrame();
    // The swaps the guest has recorded that the command processor hasn't executed yet.
    uint64_t SwapsPending();
    // Blocks until the command processor has executed the draws of every frame the guest has recorded: the last
    // recorded frame's swap, or the frame pacing's wait before it, which comes after the frame's draws. Draws read guest
    // memory (vertices, textures) as the command processor executes them, up to a frame behind the guest, so a
    // savestate's load waits for them before it puts pages back (netplay/savestates.h). Gives up after
    // `limitMicroseconds`; returns the microseconds waited. On the main thread.
    uint64_t WaitForRecordedDraws(uint64_t limitMicroseconds);
    struct DiscardStatistics
    {
        uint64_t frames = 0, draws = 0, pacingWaits = 0; // swaps, draws and frame pacing waits discarded
        // The command processor thread's CPU time (microseconds) executing the frames discarded, and the frames drawn
        // and theirs (each frame's from the swap before it to its own, the renderer's work on the thread included).
        double discardedMicroseconds = 0;
        uint64_t drawnFrames = 0;
        double drawnMicroseconds = 0;
    };
    DiscardStatistics GetDiscardStatistics();

    // PM4 type 3 opcode for the runtime's own swap packet (written by VdSwap), and the tag before the frame numbers in it.
    inline constexpr uint32_t kOpcodeXeSwap = 0x64;
    inline constexpr uint32_t kSwapFrameTag = 0x4652414D; // 'FRAM'
    inline constexpr uint32_t MakeType3(uint32_t opcode, uint32_t count)
    {
        return (3u << 30) | (((count - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8);
    }
    inline constexpr uint32_t kType2Nop = 0x80000000;
}
