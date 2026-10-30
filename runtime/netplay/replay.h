#pragma once

// Replaying frames without side effects, as rollback replays them after loading a savestate (and the sync test's second
// run, netplay/sync_test.h). From Begin to End, at frame boundaries on the main thread, the frames run are replays:
// - The command processor discards their rendering (gpu::BeginDiscard): it executes everything in their command stream
//   the CPU can observe (register and memory writes, fences, interrupts, D3D's callbacks) but draws, resolves and
//   presents nothing, and their presents don't wait for a vblank. D3D still records them in its ring on the CPU.
// - The game's calls into libraries whose state stays outside savestates (the game's hooks say which: DOA4's sound
//   engine, XACT, XAudio, CRI's file system, its asynchronous file operations) are answered from the journal of the
//   frames' first run (netplay::journal, below) instead of being made again: nothing is played, prepared, released,
//   read or saved a second time, and the game gets the answers it got the first time.
// - Rumble isn't sent to the controllers; each controller's last state asked for is, as the replay ends.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct PPCContext;

namespace netplay::replay
{
    // Whether the frames being run are replays (from Begin to End).
    bool Active();
    // At a frame boundary, after a savestate was loaded: the frames from here on are replays.
    void Begin();
    // At a frame boundary: the replay is over; the frames from here on are run the first time.
    void End();
    // Called as each replay ends (xam's rumble, ...), on the main thread.
    void OnEnd(void (*callback)());
}

namespace netplay::journal
{
    // The game's calls into libraries whose state stays outside savestates, through which replayed frames would act a
    // second time (netplay::replay). While recording (Record, from a savestate's boundary), each call the game's hooks
    // pass to Call goes through and is recorded, with its frame: its function, return address, stack pointer and
    // arguments, the registers it changed and the bytes it changed in the caller's stack (above its stack pointer, to
    // the stack's base, at most 4 KB, and below it, where the call's own frames went) and at pointer arguments into the
    // game's heap or its arena (256 bytes each). Calls the library makes back into the game's code during a recorded call
    // aren't recorded on their own. Main thread only: the hooks pass only its calls.
    //
    // A call is the same call as a recorded one when it comes in the same frame, from the same call site with the same
    // stack pointer, to the same function, with the same arguments: the registers the function reads (the game describes
    // each function, Describe: r3, the object, mostly, and the others it takes; a function that takes fewer has its
    // caller's leftovers in the other registers, which needn't be the same in another timeline), and for some the fields
    // of an input block a pointer argument points to (a sound's parameters). Some may also match a recording a few frames
    // away (a sound started a frame earlier is the same sound).
    //
    // After a savestate's load (ReplayFrom), the record's calls from that frame on are the abandoned timeline's, and the
    // frames replayed make the new one's. A call of the replay that matches one of the abandoned timeline's is answered
    // from the record without being made: the bytes it changed, and the registers it left (the context's bytes it
    // changed, so that leftovers the caller passes on match too); its effect stands (a sound it started plays on). One
    // that doesn't is made, as in a first run. Either way it joins the new timeline's record, so a later replay of its
    // frame answers it. A sync test's replay, with the first run's input, matches every call; a rollback's, with other
    // input, matches the calls until the frame where the input differs, and after it those that are the same (the same
    // sound started at the same frame). The abandoned timeline's calls the replay doesn't make are undone where they can
    // be, as the game describes them (a sound it started stopped), once the replay reaches the present (FinishReplay)
    // or, for those that may match a few frames away, once the game has passed their frame by that many.
    //
    // Releases (Defer: a library's objects that the game's savestates may still point to) are recorded and answered as
    // other calls, but made only once their frame is past rolling back (OnFrame's `oldest`), or as the journal stops: a
    // release of the abandoned timeline that the new one doesn't make is dropped, and the new one's are deferred in turn.
    using Function = void (*)(PPCContext& ctx, uint8_t* base);

    // A recorded call, as a function's undo sees it.
    struct CallRecord
    {
        uint64_t frame = 0;
        uint32_t function = 0, lr = 0;
        uint32_t arguments[8] = {}; // r3-r10
        uint64_t result = 0;        // r3 as the call returned
        // The input block's fields (FunctionInfo::blockFields), as they were at the call, one after the other.
        const uint8_t* block = nullptr;
        size_t blockSize = 0;
    };

    inline constexpr uint32_t kArgumentR3 = 1u << 20;

    // What the game says about one of the functions it journals.
    struct FunctionInfo
    {
        uint32_t address = 0;
        // The argument registers it reads: bits 0-6 r4-r10, bits 7-19 f1-f13, kArgumentR3 r3.
        uint32_t arguments = 0;
        // A register (4 to 10, r4-r10) holding a pointer to an input block, and the block's fields (offset, length) that
        // tell one call from another; -1 for none.
        int blockRegister = -1;
        std::vector<std::pair<uint16_t, uint16_t>> blockFields;
        // Objects the function changes through a pointer argument, wherever they lie (register 3-10, length): the bytes it
        // changes there are recorded with the call, and written as it's answered (the journal watches 256 bytes at
        // pointers into the game's heap and arena only).
        std::vector<std::pair<int, uint32_t>> objects;
        // A call may match a recorded one up to this many frames away.
        uint32_t nearFrames = 0;
        // Undoes a call of an abandoned timeline that the new one didn't make (at a frame boundary, on its guest context,
        // which the caller saves and restores), or nullptr if it can't be undone. `current`: the new timeline's record,
        // to check whether it holds what the undo would take away (a stream of the same sound).
        void (*undo)(PPCContext& ctx, uint8_t* base, const CallRecord& call, const std::vector<CallRecord>& current) = nullptr;
        // It only asks the library (its answer is all it changes): an abandoned call has nothing to undo.
        bool query = false;
        const char* name = nullptr; // for reports
    };
    // The game's functions, before anything is recorded. Functions it doesn't describe are matched by r3 alone.
    void Describe(std::vector<FunctionInfo> functions);

    // Whether calls are being recorded or replayed (cheap: the hooks ask before anything else).
    bool Enabled();
    // From the game's hooks: a journaled call of `function`, the guest function at `address`.
    void Call(uint32_t address, Function function, PPCContext& ctx, uint8_t* base);
    // From the game's hooks: a call that releases a library's object (DOA4's of XACT's banks), which the game's
    // savestates may still point to: recorded and answered as Call does (it returns nothing), but made only once its
    // frame is past rolling back (OnFrame) or as the journal stops, on the boundary's guest context.
    void Defer(uint32_t address, Function function, PPCContext& ctx, uint8_t* base);
    // Records the calls from now on (forgetting earlier ones; releases still deferred are made first).
    void Record();
    // Answers the calls from now on from the record, from its first (the sync test's own savestate).
    void Replay();
    // A savestate from frame `frame`'s boundary was loaded (netplay/savestates.h): the record's calls from `frame` on are
    // the abandoned timeline's, which the frames replayed from here answer where they make the same calls.
    void ReplayFrom(uint64_t frame);
    // The replay that ReplayFrom started reached the frame it was started from (the present): the abandoned timeline's
    // calls it didn't make are undone where they can be (those that may match a few frames away once the game has passed
    // them), and the reads it hasn't written yet are written (those that came with calls it didn't make, and after the
    // record's last call). Recording goes on, the record now the new timeline's.
    void FinishReplay();
    // At each frame boundary (savestates, while the journal runs), `frame` the frame about to run: the abandoned calls the
    // game has passed are undone; the calls before `oldest`, the oldest boundary a savestate can still be loaded from,
    // are forgotten, and the releases among them made.
    void OnFrame(uint64_t frame, uint64_t oldest);
    // Calls go through again, unrecorded; the releases still deferred are made.
    void Stop();
    // In a replay: whether the latest call of `function` was made in it (the record didn't hold it: the new timeline's
    // call, which the library ran for real), rather than answered from the record or made before it.
    bool MadeInReplay(uint32_t function);

    // File reads (the host kernel writing guest memory for NtReadFile, on any thread), around the write. While the
    // journal runs, each read's bytes before and after it are kept (256 MB in all at most), with the journaled call the
    // main thread made next once it was done (the first that could have told the game: the file system's polls are
    // journaled). A replay reads nothing again (the file system's calls are answered from the record), so the load
    // puts back what the first run's reads overwrote (RewindReads, or the page log), and the replay writes what they
    // read as it answers the call they came before: the game finds the data where and when it did, and makes of it what
    // it did (a model's offsets made pointers in place, by the job thread). A replay that makes a call the record
    // doesn't hold first writes every read the abandoned timeline did (the file system is in the present: the game may
    // learn of them through such a call). With the savestates saved at every frame (the page log), only into the game's
    // memory: a read into others' memory is theirs, which a load leaves as it is, and so does the replay (the audio
    // library reads a stream's first packet into a buffer of its own, which it frees a few frames later, and the memory
    // goes to another of its objects: written again, the object was lost).
    void BeforeHostWrite(uint32_t address, uint32_t length);
    void AfterHostWrite(uint32_t address, uint32_t length);
    // At a savestate's load, before its pages are loaded (the game's pages are the savestate's, read into or not):
    // the bytes the reads recorded overwrote, put back, the last read's first.
    void RewindReads();
    // The byte ranges the reads recorded wrote, [start, end), sorted and merged.
    std::vector<std::pair<uint32_t, uint32_t>> ReadRanges();

    struct Statistics
    {
        uint64_t recorded = 0;      // calls recorded (made while recording)
        uint64_t replayed = 0;      // calls answered from the record in replays
        uint64_t replayedNear = 0;  // of them, from a record of another frame
        uint64_t mismatched = 0;    // calls in replays that the record doesn't hold, made
        uint64_t unreplayed = 0;    // recorded calls of abandoned timelines that no replay made
        uint64_t undone = 0;        // of them, undone (a sound stopped)
        uint64_t unreplayedQueries = 0; // of them, queries (FunctionInfo::query: nothing to undo)
        uint64_t deferred = 0;          // releases made once past rolling back, or as the journal stopped
        uint64_t releasesDropped = 0;   // releases of abandoned timelines, never made
        uint64_t bytes = 0;             // bytes of recorded changes
        uint64_t reads = 0, readBytes = 0; // file reads recorded, and the bytes they wrote
        uint64_t readsRewound = 0, readsWritten = 0; // put back at the load, and written in the replay
        uint64_t readBytesLeft = 0;     // of those written, bytes left as they were: others' memory (with the page log)
        uint64_t readsLost = 0;         // reads not kept (past 256 MB, or under way as the record ended)
        // The calls' time in replays, in microseconds: those made (not in the record), with the journal's recording of
        // them, and of that the calls themselves (the libraries' work); those answered.
        double madeMicroseconds = 0, madeCallMicroseconds = 0, answeredMicroseconds = 0;
        std::string firstMismatch, firstUnreplayed;
        std::string functions;      // the functions recorded, with their counts
        std::string calls;          // each call recorded (the first 400), and how replays went with it, a line each
    };
    // Since the last Record.
    Statistics GetStatistics();
}
