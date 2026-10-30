#pragma once

// The game's frame boundary, where netplay's input exchange and state saving belong. The game's main loop calls
// FrameBoundary at the top of each iteration (runtime/game/main_loop.cpp), on the main thread, before the frame reads
// input and runs its update: frame numbers count those calls.
//
// In a netplay session each boundary first waits until the game's other threads (GameState::isGameThread) are blocked in
// their waits, so that none is still working on the frame before as the next begins: their random numbers come from
// streams keyed by the frame (SplitRandom). At most 50 ms; a thread still running then is logged once, and the
// session's statistics count the boundaries that waited. Then, at most 10 s, until the game's asynchronous work is done
// (GameState::asyncWorkRunning: the profile's save at a fight's start, a file read in a fight), which takes each
// machine's disk its own time while the game polls it each frame; each such wait is logged. And it checks MXCSR against
// the main thread's cached FPSCR (the generated code changes MXCSR only when its cache says the mode changes): foreign
// code on the main thread (drivers, overlays, input hooks) that changed it would go unnoticed otherwise. A difference is
// logged once, and MXCSR restored.
#include "netplay/game_state.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct PPCContext;

namespace netplay
{
    // `ctx` is the main loop's guest context, which netplay may call the game's code on during the boundary.
    void FrameBoundary(PPCContext& ctx);
    // The main loop's guest context during a frame boundary (FrameBoundary), for calls into the game's code from netplay
    // (GameState::seedMenus, abandonFight), or nullptr outside one.
    PPCContext* BoundaryContext();
    // In the main loop after the frame's update and drawing, before it ends the frame (D3D's present).
    void AfterUpdate();
    // The number of frame boundaries passed: the current frame's number (0 before the first).
    uint64_t Frame();
    // The number of frame boundaries passed since boot, which loading a savestate (SetFrame) doesn't rewind.
    uint64_t Boundaries();
    // The boundaries at which a session waited for the game's other threads, and how long in all and at most (µs).
    struct ThreadWaits
    {
        uint64_t boundaries = 0, microseconds = 0, longest = 0;
    };
    ThreadWaits GameThreadWaits();
    // The main thread's work in the frame that ran before this boundary: the time from the last boundary's end to this
    // one's start, but for its waits for the GPU (gpu::ThreadWaitedMicroseconds), which pace it to the vblanks. What a
    // frame costs when it doesn't wait, as a replayed one runs (a session chooses its input delays from it: session.h).
    // `replayed`: the frame was a replay's (netplay/replay.h). `known` is false at the first boundary.
    struct FrameWork
    {
        bool known = false, replayed = false;
        uint64_t microseconds = 0;
    };
    FrameWork LastFrameWork();
    // The current frame's number in the current fight, counted from the fight's first frame past its loading (0), which
    // netplay counts input by, or nothing outside a fight (the game says when it's in one: GameState::fightState).
    std::optional<uint64_t> FightFrame();
    // A stretch of frames whose frames play out the same on any machine given the same input: a fight from its first
    // frame past its loading until it ends (FightFrame), or a menu screen from its first frame past its loading until it
    // starts to leave (GameState::menuState). Between them the game loads or leaves a screen, which takes each machine
    // its own number of frames. Fights and menu screens are numbered from 0 each, in the order this machine's game gets
    // to them (input scripts count them: S<screen>.<frame>); a segment's frames are counted from 0. `flow`: the segment
    // belongs to the game's multiplayer flow (InFlow), which a session's machines play in lockstep, numbering its
    // segments their own way (netplay/session.h), since each plays the rest of the game on its own.
    struct Segment
    {
        bool fight = false;
        uint32_t number = 0;
        uint32_t frame = 0;
        uint32_t screen = 0; // a menu screen's (GameState::MenuState)
        bool flow = false;
    };
    // The current frame's segment, or nothing between segments. On the main thread (FightFrame on any).
    std::optional<Segment> CurrentSegment();
    // Whether the game is in its multiplayer flow now (GameState::MenuState::flow): one of its screens, the loading or
    // leaving between them, or a fight started from it, until the game gets to a screen outside it (or boots, or plays
    // its attract sequence). A session plays the flow in lockstep, and each machine the rest on its own. On the main
    // thread.
    bool InFlow();
    // At a frame boundary in a fight: a checksum of the fight's state (GameState::fightChecksum), which machines in step
    // agree on, or nothing. DEBUG_FIGHT_CHECKSUMS=first,count logs it for the first fight's frames `first` to
    // `first + count` (dumps/fight_checksums.txt: the fight frame and the checksum), for comparing runs.
    std::optional<uint64_t> FightChecksum();
    // A fight frame's final checksum for DEBUG_FIGHT_CHECKSUMS, from a session that rolls back (its frames' checksums at
    // their boundaries may be predictions' or replays': it writes each once it's final, in order).
    void LogFightChecksum(uint64_t fightFrame, uint64_t checksum);
    // At a frame boundary, makes the frame about to run `frame` (a savestate from that frame's boundary was loaded).
    void SetFrame(uint64_t frame);
    // Whether the game sees a fixed frame time, one frame at 59.94 Hz, instead of the time measured
    // (FIXED_FRAME_TIME=1, and the default with a sync test): frames replayed must see the times they first saw. The
    // game's hooks also give the pad library's auto-repeat a clock that counts frames then. In a netplay session it's
    // always on, as are SplitRandom and NoVoiceWaits: their variables are ignored there, since both machines must
    // play alike (the session's handshake compares the switches: netplay/session.h).
    bool FixedFrameTime();
    // Whether the game's threads other than its main thread draw from random number generators of their own instead of
    // one they share with it (SPLIT_RANDOM=1, and the default with a sync test; the game's hooks): those threads draw in
    // an order that varies from run to run, and the main thread's draws after theirs would get other numbers in a replay.
    bool SplitRandom();
    // The seed of the game's random numbers in fights, the same on both machines (a netplay session's, which the host
    // picks, or FIGHT_SEED=n), if any: the game's hooks seed its generators from it as each fight starts, and take the
    // fight's numbers from the fight's own generator where the game does in an online fight.
    std::optional<uint32_t> FightSeed();
    // Whether a fight's presentation goes on without waiting for the players' voice lines to finish (NO_VOICE_WAITS=1, and
    // the default with a sync test; the game's hooks), as the game does online: voices play on the audio engine's time,
    // and an intro that waits for them takes a frame more or less from run to run.
    bool NoVoiceWaits();
    // At a fight's first frame: what makes the fight the one it is, by part (GameState::fightSetup), or nothing. Each
    // fight's is logged as it starts, so runs' logs show whether they fought the same fight.
    std::vector<StatePart> FightSetup();
    // A part for the log: its bytes in hex, or a hash of them when there are more than kShownBytes.
    inline constexpr size_t kShownBytes = 24;
    std::string PartText(const StatePart& part);
    uint64_t PartHash(const StatePart& part);
}
