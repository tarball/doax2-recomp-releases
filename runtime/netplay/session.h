#pragma once

// Netplay sessions (route 2, ANALYSIS.md Phase 6): two players on two PCs, each with one controller, play the game's
// local versus fights with the other player's input fed from the network. NETPLAY=host:<port> waits for the other player
// on that UDP port, and NETPLAY=join:<address>:<port> connects to them; the host plays player 1, the other player 2.
// The host chooses each segment's input delay (below); NETPLAY_DELAY=<frames> on the host fixes it instead.
//
// A session is for the game's multiplayer flow (netplay::InFlow: Versus mode, from its menu through the entry,
// character and stage select, the fights and the screens after them), and goes straight into it. Each machine boots on
// its own, its controller as player 1 and player 2's at rest (its player may skip the logos and the movie), and from
// there the game presses for its player until it's in the flow (GameState::intoFlow: DOA4's title and mode select, which
// the player doesn't see for long). Nothing of that is exchanged. The flow both machines play in lockstep, the host's
// controller as player 1 and the other's as player 2 on both. The flow's segments (netplay::CurrentSegment): its menu
// screens, from each one's first frame past its loading until it starts to leave, and its fights, from their first
// frame past loading until they end. In a segment each frame's local input is sent and applied `delay` frames later on
// both machines (the segment's input delay), and a machine waits at the frame boundary for the other's when it hasn't
// come. A segment's first `delay` frames get no one's input, so each machine gets to it in its own time and the faster
// one waits there: the first to get to Versus waits at its menu's frame `delay`, its window saying so, until the other
// gets there too. Between the flow's segments, while a screen or a fight loads or a screen leaves, both players' pads
// are at rest. The session numbers the flow's segments from 0 in the order both play them (each machine numbers all of
// its own from boot: Segment::number). Nobody leaves the flow: what would take the game out of it
// (GameState::flowBlockedButtons: DOA4's B on Versus's menu, Quit in the pause and result menus) is taken out of both
// players' input on both machines. Should the game leave it all the same, both leave together (a press is both
// players' at the same frame), and the game's presses take each back into it.
//
// The input delays (session.cpp: Choose). Both machines play a segment with the same delay, the host's choice, which it
// makes for each segment from what both measure: as it ends the segment before (the joining machine hears of it while
// both load the next; one that gets there first waits at its frame 0 until it has), the flow's first once connected
// and a few round trips measured. Choices are final, and the host's packets carry its latest: the round trip, measured
// from the handshake on (the alive packets echo the other's clock too), its mean and 95th percentile over the last 10 s;
// and each machine's measures, which every packet of its carries: its main thread's work a frame in the flow's menus and
// in its fights (netplay::LastFrameWork, without its waits for the GPU) and with rollback a replayed frame's time, a
// load's and the load's wait for the GPU's draws. A segment in lockstep (a menu screen, a fight without rollback) takes
// the delay 60 frames a second needs at the mean round trip, ceil((RTT / 2 + work) / frame) - 1, and a frame to spare, or
// what the 95th percentile needs if more; a fight that rolls back the smallest that keeps its rollbacks (the frames after
// which the other's input comes, less the delay) within what the slower machine fits beside a frame (FitsFor: the first
// replayed frame where the present frame would have waited for its vblank, and the others with the present frame in the
// frame after, leaving the GPU 3 ms; a replayed frame as measured, else as a fight's frame's work, before the first fight
// the menus' times 1.6), and at least 2, or fewer where the other's input takes fewer frames (no rollback is needed then:
// 1 on a local network). A delay never changes inside a segment.
// NETPLAY_DELAY and NETPLAY_FIGHT_DELAY on the host fix them; the joining machine's don't apply. Each segment's start
// logs its delay and why, on both machines; each packet also says the delay its sender plays the segment with, and
// machines that differ there end the session.
//
// Each segment's frames carry a checksum (netplay::FightChecksum in fights, GameState::menuChecksum in the menus), and
// the segments' identities (which fight or screen) are compared. A fight that differs is logged as a desync; menus that
// differ, or machines on different screens, or one that left a screen the other is still in, mean the two games went
// separate ways: the session ends, saying so in both logs and window titles. As each menu screen of the flow starts, the
// game's random numbers are seeded from the session's seed (GameState::seedMenus), so its RANDOM picks agree.
// The host picks the session's seed (netplay::FightSeed, which also seeds each fight), and a session turns on what fights
// need to play out the same on both machines (FixedFrameTime, SplitRandom, NoVoiceWaits, whatever their variables say).
// In a session the game sees no keystrokes and standard controllers for players 1 and 2 from boot (xam/input.cpp): the
// controller's type picks a default button layout.
//
// Each player's settings that shape the flow's fights (GameState::playerSettings: their button layout, and Versus
// Mode's rules) travel with every packet of the flow, as they were when the sender entered it: the save loads after the
// handshake, at the title (a player sets them in the game's settings without a session). Each machine checks
// the other's (an out-of-range value ends the session), and once both machines are in the flow, both apply the host's
// as player 1's and the other's as player 2's, until the flow ends: each player keeps their own controls, and the host's
// rules apply. The game's save is never changed (versus.cpp).
//
// The handshake checks that both machines play alike (netplay/identity.h): the hello and the welcome carry each machine's
// build (a hash of the executable), the disc image's hash and the switches that change how the game runs (the session's
// own, which a session always has on, the game heap, a sync test, the C runtime's FMA3 paths, and the debugging vblank
// rate and file redirections). The host refuses another build, disc or switches, saying which in both machines' logs.
// At each fight's first frame both machines send its setup (GameState::fightSetup: the stage, characters and costumes,
// the match's rules, the button layouts and the random number generators) until the other has it, and a fight that
// isn't the same one names the parts that differ. At the first desync, both write their state at that frame
// (GameState::desyncRegions) to dumps/desync_<fight>_<frame>.bin, which tools\netplay\desyncdiff.py compares.
//
// A thread receives the packets and times their arrival; each carries the sender's clock and an echo of the other's, for
// the round trip. Another sends a packet every 250 ms once connected, whatever the game does, so that a machine knows the
// other's game still runs (and where it is: on its own, or where in the flow) while it waits. A session ends when the
// other player leaves: its game sends a goodbye as its window closes (a player who wants to stop closes the game), and
// one that stops answering for 10 s counts as gone; or when the games went separate ways, or never could play together
// (refused). The game then goes on with this machine's player alone for a while: a fight under way is won by that player
// (GameState::abandonFight), the player is told why in a notice on screen (ui/notices.h) and in the window's title, and
// 10 s later the game closes, as its player would close it: the session was what it ran for. Every 600 fight frames,
// and as a fight ends, the log gets its statistics: the frame rate, vblanks without a new frame, the waits for the other
// player's input, how early its inputs came, the round trip (with the input delay it needs for 60 frames a second), the
// frame boundaries that waited for the game's threads (netplay/frame.h) and the packets; and as each menu screen ends,
// its waits. DEBUG_NET_CONDITIONS, DEBUG_NET_OUTAGE and DEBUG_STALL (session.cpp) make the packets go out as over a
// network, or the game stall.
//
// Rollback in fights (the default; NETPLAY_ROLLBACK=0 on both machines plays them in lockstep instead: a switch the
// handshake compares), as GGPO plays: the menus and a fight's first frames stay in lockstep, and from the fight's frame
// `delay` on (the host's choice, above), where the first machine there waits for the other as in lockstep, each frame
// runs at once, with the other player's input where it has come and a prediction where it hasn't (the last input that
// came, as GGPO predicts). The game is saved at every frame boundary (netplay/savestates.h). When the other player's
// input comes for a frame that ran on another, the game loads that frame's savestate and replays the frames since to
// the present with the input as it came (netplay/replay.h: their rendering discarded, the libraries' calls answered
// from the journal where the replay makes the same, made where it doesn't, the abandoned timeline's sounds faded out),
// then runs the present frame. A machine runs at most NETPLAY_WINDOW frames (default 8, the host's) past the last frame
// whose input from the other it has; further ahead, it waits, as GGPO does. The frames in which a press can end the
// fight (GameState::fightMenu: DOA4's pause menu, and the match's result with the menu after it) run only with the
// other's input as it came (they wait for it, lockstep), so a fight ends at the same frame on both machines. Time sync,
// as GGPO's: each machine measures how long before it needed them the other's inputs come on average (its slack, sent
// with its packets), and the machine whose slack is the smaller by more than a few milliseconds runs ahead by half the
// difference: it slows down by that much, its vblanks (which pace the game's frames) coming later by at most 2 ms each
// (gpu::DelayVblanks), so that neither runs ahead for long and both roll back alike; after a wait of more than 100 ms
// (an outage, a stall) both machines' slacks start afresh. Checksums are compared at the frames whose inputs from both
// machines are final (a frame's checksum, at its boundary, once the inputs of the frames before it came and the frames
// ran with them), and desyncs are dumped as in lockstep. Rollback is the default (the user's choice, 2026-10-28;
// ANALYSIS.md, Phase 6, The input delays a session chooses: with the delays chosen, both modes keep 60 frames a second,
// and rollback shows a press sooner from about 30 ms on).
#include <cstdint>
#include <optional>

namespace netplay::session
{
    struct Pad
    {
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
        bool operator==(const Pad&) const = default;
    };
    static_assert(sizeof(Pad) == 12);

    // Whether NETPLAY is set.
    bool Configured();
    // Whether NETPLAY is set without NETPLAY_ROLLBACK=0: fights roll back, which needs the game heap from boot
    // (netplay/heaps.h, for the savestates).
    bool RollbackConfigured();
    // The input module's sample of the local player's controller (pad 1, the keyboard and scripted presses).
    void SetLocalPad(bool (*sample)(Pad& pad));
    // At each frame boundary, on the main thread, once the frame's segment is known: exchanges input, and in a segment
    // of the flow waits for the other player's.
    void OnFrame(uint64_t frame);
    // At each frame boundary, on the main thread, after the savestates (netplay/frame.h): in a fight that rolls back,
    // loads the savestate of a frame that ran with another input than the other player's that came, waits where the
    // frame must (the window, the fight's menus), and decides the pads of the frame about to run.
    void AfterSavestates(uint64_t frame);
    // The pad a game user reads this frame, if the session decides it (user 0, player 1: this machine's player, or in the
    // flow the host's; user 1, player 2: at rest, or in the flow the other's; users 2 and 3 not connected): true, with
    // `connected` and the state. False: read the controllers as usual.
    bool PadFor(uint32_t user, bool& connected, Pad& pad, uint32_t& packetNumber);
    // The seed the host picked for the fights' random numbers, once connected.
    std::optional<uint32_t> Seed();
    // Whether this machine plays the session's multiplayer flow: from the first segment of it the session started until
    // the game leaves the flow (though the session may end in between). On the main thread.
    bool PlaysFlow();
    // As the game's window closes: tells the other player this one leaves.
    void Shutdown();

    // The input delays' formulas (session.cpp: Choose), apart for the runtime's tests. At a round trip (its mean and 95th
    // percentile) and a frame's work, in microseconds: the frames after which an input is on the other machine (`late`),
    // the delay lockstep needs for 60 frames a second (`needed`) and takes (with a frame to spare, or what the 95th
    // percentile needs if more), and a fight that rolls back takes when a rollback fits `fits` frames beside a frame on
    // the slower machine (at least 2, or `late` if fewer).
    struct DelayChoice
    {
        uint32_t late = 0, needed = 0, lockstep = 0, rollback = 0;
    };
    DelayChoice DelaysFor(double roundTripUs, double roundTripHighUs, double workUs, uint32_t fits);
    // The frames a rollback fits beside a frame on a machine whose replayed frame takes `replayFrameUs`, its present frame's
    // work `presentWorkUs`, a load `loadUs` and the load's wait for the command processor to draw the frames recorded
    // `drawWaitUs`, at most `window`: the wait, the load and the first replayed frame take the time the present frame would
    // have waited for its vblank (none fits if they don't), and the others and the present frame the next frame, less
    // 3 ms for the GPU to draw it.
    uint32_t FitsFor(double replayFrameUs, double presentWorkUs, double loadUs, double drawWaitUs, uint32_t window);
}
