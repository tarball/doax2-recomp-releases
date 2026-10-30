#include "netplay/session.h"

#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include "config.h"
#include "gpu/command_processor.h"
#include "guest_memory.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/game_state.h"
#include "netplay/heaps.h"
#include "netplay/identity.h"
#include "netplay/replay.h"
#include "netplay/savestates.h"
#include "netplay/sync_test.h"
#include "ui/notices.h"
#include "ui/window.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <math.h>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace netplay::session
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using std::chrono::microseconds;

        constexpr uint16_t kVersion = 8;
        constexpr uint32_t kMaxPads = 64;       // pads a segment packet carries at most
        constexpr uint32_t kChecksumsSent = 8;  // the last checksums each segment packet carries
        constexpr auto kHelloInterval = std::chrono::milliseconds(250);
        constexpr auto kAliveInterval = std::chrono::milliseconds(250);
        constexpr auto kResendInterval = std::chrono::milliseconds(8);
        constexpr auto kWaitNotice = std::chrono::seconds(2);      // a wait this long is logged, and again every
        constexpr auto kNoticeInterval = std::chrono::seconds(10); // this long
        constexpr auto kSilenceLimit = std::chrono::seconds(10);   // nothing from the other machine this long: it's gone
        constexpr auto kWaitLimit = std::chrono::seconds(60);      // a wait this long, the other machine in a segment, ends it
        constexpr auto kApartLimit = std::chrono::seconds(10);     // both waiting in other segments this long: apart
        constexpr auto kCloseDelay = std::chrono::seconds(10);     // from the session's end to the game closing
        constexpr uint32_t kReportInterval = 600;    // fight frames between the statistics in the log
        constexpr auto kLateFrame = microseconds(25000); // a frame boundary this long after the last: a vblank missed
        constexpr uint32_t kSlackFrames = 32;        // the frames a machine's slack is averaged over
        constexpr size_t kInboxLimit = 4096;         // packets received and not taken yet
        constexpr int32_t kNoSlack = INT32_MIN;
        constexpr size_t kDesyncFrames = 128;        // the fight frames whose state is kept for a desync's dump
        constexpr size_t kEndedKept = 8;             // the segments this machine has ended that it still answers for
        constexpr size_t kEarlyKept = 64;            // the segments ahead whose packets are kept
        // A segment's key: its number among the flow's segments the session played (the same on both machines), with
        // kFightKey for a fight.
        constexpr uint32_t kFightKey = 0x80000000;
        constexpr uint32_t kNoKey = 0xFFFFFFFF;      // no segment
        constexpr uint32_t kGapPackets = 0xFFFFF;    // the pads' packet number's low bits between segments
        constexpr uint32_t kFrameWindow = 1u << 24;  // a segment's frames a packet may name (inputs, checksums)
        constexpr size_t kSettingsSize = GameState::kMaxPlayerSettings;
        // A segment packet's `received` from a machine that has ended the segment: it needs nothing more of it, so the
        // other doesn't answer (two machines that had both ended it would otherwise answer each other forever).
        constexpr uint32_t kNothingNeeded = 0xFFFFFFFF;
        // Rollback in fights (session.h): the input delay and the window (the most frames a machine runs past the last
        // frame whose input from the other it has) unless NETPLAY_FIGHT_DELAY and NETPLAY_WINDOW say otherwise, and their
        // bounds (the host's travel in its welcome).
        constexpr uint32_t kDefaultFightDelay = 2, kMaxDelay = 30;
        constexpr uint32_t kDefaultWindow = 8, kMaxWindow = 16;
        // The time sync: a frame advantage below this isn't corrected (ms); the most a vblank is delayed by (us: an eighth
        // of a frame, so a frame's advantage takes 8 frames to even out); the most corrected at once (GGPO's 9 frames, ms);
        // and the frames after a correction before the next is decided, while both machines' slacks take it in (each
        // averaged over kSlackFrames, and the other's comes a one-way trip later).
        constexpr double kSyncThresholdMs = 4.0;
        constexpr uint32_t kSyncStepMicroseconds = 2083;
        constexpr double kSyncMaxMs = 9 * 16.667;
        constexpr uint32_t kSyncSettleFrames = 64;
        constexpr uint64_t kSlackResetMicroseconds = 100000; // a wait this long in a fight that rolls back starts the slacks afresh
        // The input delays the host chooses for each segment of the flow (Choose; session.h). A frame (us); the least work
        // a frame is reckoned to take in the formulas (session 2's 2 ms: the time from a frame's start to its input being
        // needed on the other machine includes the work of the frame before); the round trips the choice looks at (the
        // last 10 s', or at least the last 16); the round trips the first segment waits for, at most 3 s; the replayed
        // frames (after a rollback's first) a machine must have measured before their time counts instead of the estimate
        // from its frames' work, and the frames' work measured before that counts; the frames a rollback fits when
        // nothing is measured (a cautious guess: a fast PC fits 6); a load's time before one is measured; the rollback
        // fights' least delay (session 9's default; fewer where the other's input comes sooner: DelaysFor); and the most
        // a measurement may say (us).
        constexpr double kFrameMicroseconds = 1e6 / 60.0;
        constexpr double kLeastWorkMicroseconds = 2000;
        constexpr auto kRoundTripWindow = std::chrono::seconds(10);
        constexpr size_t kRoundTripsKept = 4096, kLeastRoundTrips = 16;
        constexpr uint64_t kFirstRoundTrips = 8;
        constexpr auto kFirstRoundTripWait = std::chrono::seconds(3);
        constexpr uint64_t kReplayFramesMeasured = 20, kWorkFramesMeasured = 60;
        // A fight's frame (run as the present, or replayed) against a frame of the flow's menus, which is all a machine has
        // measured before its first fight: on this PC 2.1 ms against 1.6 on 16 logical processors and 5.2-5.5 against 3.3 on
        // 2 (the fights' frames are heavier, and more so with fewer processors): the cautious end.
        constexpr double kFightPerMenuWork = 1.6;
        constexpr uint32_t kDefaultFits = 4;
        // Before a machine's loads are measured: a load's time, and its wait for the command processor to draw the frames
        // recorded (the GPU's drawing of a frame after its vblank: 3.4-3.7 ms here). And what a rollback leaves the GPU to
        // draw the present frame before its vblank (Fits).
        constexpr double kDefaultLoadMicroseconds = 1000, kDefaultDrawWaitMicroseconds = 3500;
        constexpr double kDrawMarginMicroseconds = 3000;
        constexpr uint32_t kLeastFightDelay = 2;
        constexpr uint32_t kMaxMeasureMicroseconds = 1'000'000;
        constexpr uint32_t kMaxRoundTripMicroseconds = 10'000'000;
        constexpr int64_t kClockBucketSeconds = 5; // the two clocks' offsets, the least delayed packet's of each (DriftPpm)

        enum class Type : uint16_t
        {
            kHello = 1,    // joining -> host: the joining player's input delay and its machine
            kWelcome = 2,  // host -> joining: the session, its seed and input delay, the host's machine
            kAlive = 3,    // where the sender's game is, every kAliveInterval once connected
            kSegment = 4,  // a player's inputs, checksums and settings in a segment of the flow, and a fight's setup at first
            kBye = 5,      // the sender leaves the session
            kRefuse = 6,   // host -> joining: not the same build, disc or switches (with the host's machine)
        };

#pragma pack(push, 1)
        struct Header
        {
            char game[8];      // GAME_PREFIX: another game's packets are ignored
            uint16_t version;  // kVersion
            uint16_t type;     // Type
            uint32_t session;  // the host's session ID (0 in a Hello)
            uint32_t time;     // the sender's clock as it sent this, in microseconds (wrapping)
            uint32_t echo;     // the newest `time` the sender had from the receiver, plus how long it held it (0: none yet)
        };
        // What both machines must have alike to play together (identity.h), and for the log, what the machine is.
        struct Identity
        {
            uint64_t build;         // identity::BuildId
            uint64_t disc;          // identity::DiscHash
            char configuration[16]; // the build's configuration
            char switches[160];     // the switches that change how the game runs (Switches)
            char machine[96];       // the CPU and Windows build, for the log
        };
        // A list of parts (the game's settings, a fight's setup: game_state.h), as compared between machines: each part's
        // bytes, or a hash of them if longer than kShownBytes. The parts' names are the same on both machines (the same
        // build), so they aren't sent.
        constexpr uint32_t kMaxParts = 12;
        constexpr uint8_t kHashed = 0xFF;
        struct PartDigest
        {
            uint8_t length;               // the part's bytes, or kHashed: their hash in the first 8
            uint8_t bytes[kShownBytes];
        };
        struct Digests
        {
            uint8_t count;
            PartDigest parts[kMaxParts];
        };
        struct Hello
        {
            Header header;
            uint16_t delay;
            Identity identity;
        };
        // The host's input delays: fixed by its variables (kFixedLockstep, kFixedRollback), or chosen for each segment of
        // the flow (Delays, in its packets). The window always applies.
        constexpr uint8_t kFixedLockstep = 1, kFixedRollback = 2;
        struct Welcome
        {
            Header header;
            uint32_t seed;
            uint16_t delay;      // the lockstep segments' input delay, if fixed
            uint16_t fightDelay; // with rollback (session.h): the fights' input delay, if fixed
            uint16_t window;     // and the window
            uint8_t fixed;       // kFixedLockstep, kFixedRollback
            Identity identity;
        };
        // A machine's measures of its own pace, in its segment and alive packets: the host chooses the input delays from
        // both machines' (Choose), and each machine's log shows the other's. 0: not measured yet.
        struct Measures
        {
            // Its main thread's work a frame (netplay::LastFrameWork), mean, in the flow's menu screens and in its fights (the
            // frames run as the present).
            uint32_t menuWorkUs, fightWorkUs;
            uint32_t replayFrameUs; // in fights that roll back: a replayed frame's time, mean (a rollback's frames after its first)
            uint32_t replayFrames;  // and how many were measured
            uint32_t loadUs;        // a savestate's load, mean
            uint32_t drawWaitUs;    // and its wait for the command processor to draw the frames recorded, mean
        };
        // How a delay was chosen.
        enum class How : uint8_t
        {
            kDefault,   // nothing measured yet: a cautious default
            kChosen,    // from the round trip and the machines' measures
            kFixed,     // the host's variable (NETPLAY_DELAY, NETPLAY_FIGHT_DELAY)
        };
        // Where a machine's reckoning of the frames a rollback fits beside a frame came from (Fits).
        enum class FitsFrom : uint8_t
        {
            kDefault,   // nothing measured: kDefaultFits
            kFrameWork, // its frames' work, as an estimate of a replayed frame's
            kReplays,   // its replayed frames
        };
        // The input delays of a segment of the flow, which the host chooses as it ends the segment before (the first
        // segment's as it starts it, once connected): the host's packets carry its latest choice, and the joining machine
        // plays the segment with them. A choice is final: both machines play the segment with it (session.h).
        struct Delays
        {
            uint32_t segment = kNoKey;  // the segment's number among the flow's (Ordinal)
            uint8_t lockstep = 0;       // its delay if it runs in lockstep (a menu screen, or a fight without rollback)
            uint8_t rollback = 0;       // and if it's a fight that rolls back
            uint8_t lockstepHow = 0, rollbackHow = 0; // How
            // What they were chosen from: the delay 60 frames a second needs at the round trip in lockstep (without the
            // frame to spare); the frames after which the other's input comes; the frames a rollback fits beside a frame on
            // each machine, and where that came from (FitsFrom); the round trip (mean and 95th percentile) and the frames'
            // work (the larger machine's, at least kLeastWorkMicroseconds).
            uint8_t needed = 0, late = 0;
            uint8_t fitsHost = 0, fitsJoining = 0, fitsHostFrom = 0, fitsJoiningFrom = 0;
            uint32_t roundTripUs = 0, roundTripHighUs = 0, workUs = 0;
        };
        // What every segment and alive packet carries besides: the sender's measures, and from the host, its latest choice
        // of delays.
        struct Pace
        {
            Measures measures;
            Delays delays;
        };
        struct Refuse
        {
            Header header;
            Identity identity;
        };
        // Where the sender's game is: in a segment of the flow (and waiting there for the receiver's input), between
        // the flow's segments, or on its own (outside the flow: its own menus and modes, booting), and the flow's segment
        // it's in or was last in, with its frame there (its last, once it has left it).
        enum class Where : uint8_t
        {
            kOwnTime,
            kBetween,
            kInSegment,
            kWaiting,
        };
        struct Alive
        {
            Header header;
            uint32_t key;    // the segment (SegmentPacket::key), or kNoKey
            uint32_t frame;  // its frame
            uint32_t screen; // a menu screen's identity
            uint8_t where;   // Where
            Pace pace;
        };
        struct Bye
        {
            Header header;
            uint8_t reason; // Leaving
        };
        // Followed by `pads` pads (the segment's frames `first` on), `checksums` checksums, and with kSetupIncluded the
        // fight's setup at its first frame (Digests).
        constexpr uint8_t kSetupIncluded = 1, kSetupReceived = 2;
        struct SegmentPacket
        {
            Header header;
            uint32_t key;       // the segment: its number in the flow's (both machines'), with kFightKey for a fight
            uint32_t screen;    // which menu screen it is on the sender (GameState::MenuState), 0 in a fight
            uint32_t received;  // the sender has the receiver's inputs of the segment for every frame before this one
            uint32_t first;
            uint16_t pads;
            uint16_t checksums;
            int32_t slack;      // the sender's slack in a fight (LocalSlack), in microseconds, or kNoSlack
            uint8_t setup;      // kSetupIncluded: the sender's fight setup follows; kSetupReceived: the sender has the receiver's
            uint8_t delay;      // the input delay the sender plays the segment with
            // The sender's player's settings for the flow (GameState::PlayerSettings), as it entered the flow: `size`
            // bytes, 0 if none.
            uint8_t settingsSize;
            uint8_t settings[kSettingsSize];
            Pace pace;
        };
        struct Checksum
        {
            uint32_t frame;
            uint64_t checksum;
        };
#pragma pack(pop)

        enum class State
        {
            kOff,
            kConnecting,
            kConnected,
            kEnded,
        };

        // The other player's input for a segment's frame, and when it arrived.
        struct RemoteInput
        {
            Pad pad;
            Clock::time_point arrival;
        };

        // A fight's statistics, in the log every kReportInterval fight frames and as it ends.
        struct Statistics
        {
            Clock::time_point lastBoundary{};
            // From the first frame that needs the other player's input (fight frame `delay`), where the machine that got
            // to the fight first waits for the other: the frames since and the time they took.
            Clock::time_point settled{};
            uint64_t startWaitMicroseconds = 0;
            // The GPU's vblanks and the frames it showed since then (its swaps, less those of frames replayed, which it
            // discards): a vblank without a new frame shows one twice.
            uint64_t vblanks = 0, swaps = 0, discarded = 0;
            // Frame boundaries that came more than kLateFrame after the last (the game missed a vblank), and the longest.
            uint64_t late = 0, longestFrameMicroseconds = 0;
            // Waits for the other player's input after the first.
            uint64_t waits = 0, waitedMicroseconds = 0, longestWaitMicroseconds = 0;
            // Round trips measured.
            uint64_t roundTrips = 0, roundTripMicroseconds = 0, shortestRoundTrip = UINT64_MAX, longestRoundTrip = 0;
            uint64_t compared = 0, packetsSent = 0, bytesSent = 0, resent = 0, dropped = 0, packetsReceived = 0;
            // The frame boundaries' waits for the game's threads (netplay::GameThreadWaits) as the fight started.
            ThreadWaits threadWaits;
        };

        // The fight frames' state for a desync's dump (GameState::desyncRegions), the last kDesyncFrames.
        struct DesyncRing
        {
            std::vector<uint32_t> frames;           // the fight frame in each slot, or UINT32_MAX
            std::vector<std::vector<uint8_t>> data; // the regions' bytes, one after the other
        };

        // A player's settings for the flow as they travel (SegmentPacket::settings).
        struct Settings
        {
            bool known = false;
            uint8_t bytes[kSettingsSize] = {};
        };

        // The current segment: its frames' inputs (frame -> pad) and checksums on both machines.
        struct Lockstep
        {
            bool active = false;
            uint32_t key = kNoKey, screen = 0;
            bool fight = false;
            uint32_t number = 0;            // this machine's number for it (netplay::Segment)
            uint64_t startFrame = 0;        // the game frame of its frame 0
            uint32_t ordinal = 0;           // the flow's segments the session started before it
            uint32_t delay = 0;             // its input delay: the host's choice for it (Delays), lockstep's or a rollback fight's
            bool delayKnown = false;        // (ResolveDelay, once connected)
            bool peerDelayKnown = false;    // the joining machine: the host's delay for it, in the host's packets of it
            uint32_t peerDelay = 0;
            std::map<uint32_t, Pad> local;
            std::map<uint32_t, RemoteInput> remote;
            std::map<uint32_t, uint64_t> localChecksums, remoteChecksums;
            uint32_t peerReceived = 0;      // the other machine has this one's inputs for every frame before this
            uint32_t receivedFrom = 0;      // this one has the other's for every frame before this (Received)
            uint32_t lastFrame = 0;
            bool remoteScreenKnown = false;
            uint32_t remoteScreen = 0;
            bool differs = false, apart = false; // a checksum, or the screen, differed (logged once each)
            // A menu screen's waits for the other player's input after its frame `delay`, and the one at it, and its
            // checksums compared.
            uint64_t startWaitMicroseconds = 0, waits = 0, waitedMicroseconds = 0, longestWaitMicroseconds = 0, compared = 0;
            // A menu screen's frame rate from its frame `delay` on (as a fight's: Statistics), and the round trips measured
            // in it.
            Clock::time_point settled{}, lastBoundary{};
            uint64_t vblanks = 0, swaps = 0;
            uint64_t roundTrips = 0, roundTripMicroseconds = 0;
        };

        // What the other machine sent of a segment this machine isn't in yet (it may be loading it, or still be on its
        // own, outside the flow).
        struct Early
        {
            std::map<uint32_t, RemoteInput> inputs;
            std::map<uint32_t, uint64_t> checksums;
            bool screenKnown = false;
            uint32_t screen = 0;
            Settings settings;
            bool delayKnown = false; // the input delay the other machine plays it with
            uint32_t delay = 0;
        };

        // What this machine sent of a segment it has ended, to answer the other machine while that is still in it: the
        // segment's last inputs may have been lost, and once this machine is out of it, it sends no more of them.
        struct EndedSegment
        {
            uint32_t key = kNoKey, screen = 0, delay = 0;
            uint32_t peerReceived = 0; // the other machine had this one's inputs for every frame before this
            std::map<uint32_t, Pad> local;
            std::map<uint32_t, uint64_t> checksums;
        };

        // A fight that rolls back (session.h), from its first frame that needs the other player's input (its frame
        // `delay`, where both machines meet as in lockstep) on: what each frame ran with, the savestates' and replays'
        // state, and statistics.
        struct Rollback
        {
            bool on = false;                // savestates and the journal started
            bool ownSavestates = false;     // started for it (and stopped as it ends)
            // The frames run: the other player's pad each ran with last (fight frame ->), its checksum at its boundary in
            // the timeline that ran it last, and the time this machine first got to its boundary (for the slack).
            std::map<uint32_t, Pad> used;
            std::map<uint32_t, uint64_t> checksums;
            std::map<uint32_t, Clock::time_point> reached;
            int64_t present = -1;           // the furthest fight frame run as the present (not a replay's)
            int64_t lastRun = -1;           // the fight frame run last (its pads decided), a replay's or the present
            int64_t verified = -1;          // every frame up to this one ran with the other's input as it came
            int64_t emitted = -1;           // final checksums sent (and compared, and logged) up to this frame
            // A replay under way: the game frame it returns to (the present), when it started, and whether its first frame
            // (which waits out the vblank the present frame would have, D3D letting the CPU run a frame ahead) has run.
            bool replaying = false, firstReplayed = false;
            uint64_t replayEnd = 0;
            Clock::time_point replayStart{};
            // Statistics: rollbacks (and those that started during a replay), the frames replayed, how many frames each
            // went back (by count), the loads' and replays' time; the frames run ahead of the other's input (each
            // present frame's distance past the last frame whose input came: sum and most); the waits at the window and
            // in the fight's menus; the frame advantage measured, and the time sync's corrections.
            uint64_t rollbacks = 0, nested = 0, framesReplayed = 0;
            uint64_t depths[kMaxWindow + 2] = {};
            uint32_t deepest = 0;
            double loadMicroseconds = 0, longestLoadMicroseconds = 0, replayMilliseconds = 0, firstFrameMilliseconds = 0;
            // A replayed frame's time, for the input delays (Measures): the replays' frames after their first, run back to
            // back (the first waits out a vblank), and their time; the replay under way's frames run so far, its first
            // frame's time, and whether it ran without waits or a rollback inside it (only those count).
            double laterMilliseconds = 0;
            uint64_t laterFrames = 0;
            uint64_t replayRunFrames = 0;
            double replayFirstMilliseconds = 0;
            bool replayClean = false;
            double drawWaitMicroseconds = 0, longestDrawWaitMicroseconds = 0; // the loads' waits for the command processor
            uint64_t aheadFrames = 0, aheadSum = 0;
            uint32_t furthestAhead = 0;
            uint64_t windowWaits = 0, windowWaitMicroseconds = 0, longestWindowWait = 0;
            uint64_t menuWaits = 0, menuWaitMicroseconds = 0, longestMenuWait = 0;
            double advantageSum = 0, advantageLow = 0, advantageHigh = 0;
            uint64_t advantageSamples = 0;
            uint64_t corrections = 0;
            double correctedMilliseconds = 0;
            uint32_t nextSync = 0;          // the fight frame from which the time sync may decide again
            uint32_t slackFrom = 0;         // the first fight frame whose slack counts (after a long wait, LocalSlack)
            uint64_t vblankDelayAtStart = 0;
            bool endLogged = false;
        };

        struct Session
        {
            State state = State::kOff;
            bool host = false;
            SOCKET socket = INVALID_SOCKET;
            sockaddr_in peer{};
            std::string peerName;
            uint32_t id = 0, seed = 0, delay = 3;
            // Rollback in fights (session.h): the fights' input delay and the window, the host's.
            bool rollback = false;
            uint32_t fightDelay = kDefaultFightDelay, window = kDefaultWindow;
            Rollback rb; // the current fight's
            // The input delays (Choose): whether the host's variables fix lockstep's and the rollback fights' (`delay` and
            // `fightDelay` then), the round trips measured from the handshake on (their arrival and microseconds, the
            // newest last), this machine's measures (the flow's frames' work, a rollback's replayed frames after its
            // first and the loads, before the current fight's), the other's from its packets, and the delays of the
            // flow's segments, by number: the host's choices (on the joining machine, those it has heard of), the latest
            // in the host's packets.
            bool lockstepFixed = false, rollbackFixed = false;
            struct RoundTrip
            {
                Clock::time_point arrival;
                uint32_t microseconds;
            };
            std::deque<RoundTrip> roundTrips;
            uint64_t roundTripCount = 0;
            double menuWorkMicroseconds = 0, fightWorkMicroseconds = 0, replayMicroseconds = 0, loadMicroseconds = 0, drawWaitMicroseconds = 0;
            uint64_t menuWorkFrames = 0, fightWorkFrames = 0, replayFrames = 0, loads = 0;
            bool peerMeasuresKnown = false;
            Measures peerMeasures{};
            std::map<uint32_t, Delays> delays;
            Delays latestDelays{};
            bool delaysMismatchLogged = false, ignoredDelaysLogged = false;
            // The two clocks (DriftPpm): the first offset of the other machine's clock from this one's in a packet, and the
            // largest offset of every kClockBucketSeconds since (the least delayed packet's), relative to it: (seconds
            // since, microseconds), and the bucket under way.
            bool clockKnown = false;
            uint32_t flowsEntered = 0; // (from the first time this machine enters the flow)
            uint32_t firstOffset = 0;
            Clock::time_point clockStart{};
            int64_t clockBucket = -1, clockBucketMost = 0;
            std::vector<std::pair<double, double>> clockPoints;
            Clock::time_point lastHello{};
            bool (*sampleLocal)(Pad&) = nullptr;
            // The other machine's clock (its newest time and when it arrived), and the round trip, smoothed.
            bool peerTimeKnown = false;
            uint32_t peerTime = 0;
            Clock::time_point peerTimeArrival{};
            double roundTripMicroseconds = 0;
            // The newest packet from the other machine, and where its game was (its newest Alive).
            Clock::time_point lastArrival{};
            bool peerWhereKnown = false;
            Alive peerWhere{};
            Clock::time_point peerWhereArrival{};
            // The current segment, the flow's segments started, those the other machine sent ahead of this one, and those
            // this one has ended.
            Lockstep step;
            uint32_t segmentsStarted = 0;
            std::map<uint32_t, Early> early;
            std::deque<EndedSegment> ended;
            Clock::time_point endedAt{};
            // The flow: whether this machine is in it (its segments started since it entered), the first segment's key,
            // this machine's player's settings as it entered, the other's (from its packets of the flow), and whether
            // both are applied (GameState::PlayerSettings).
            bool flow = false;
            uint32_t flowFirstKey = kNoKey;
            uint64_t flowEntered = 0; // the game frame it entered
            Settings localSettings, remoteSettings;
            bool settingsApplied = false;
            // This frame's pads (game users 0 and 1) and packet numbers, when the session decides them, and the last
            // packet number given (packet numbers change whenever the pad does).
            bool padsSet = false;
            Pad pads[2]{};
            uint32_t packets[2]{};
            Pad lastPads[2]{};
            // The buttons the flow last took out of each pad because they'd leave it (GameState::flowBlockedButtons).
            uint16_t blockedLogged[2]{};
            // Fights: the fight's number among the flow's (and how many have started), setups, statistics and slack.
            uint32_t fight = 0, fightsStarted = 0;
            uint32_t lastFightFrame = 0;
            bool desync = false;
            Clock::time_point lastSent{};
            std::deque<int64_t> slacks;
            int32_t remoteSlack = kNoSlack;
            Statistics statistics;
            // This machine's identity, once the disc image's hash is known, and the other's refusal, logged once.
            bool identityKnown = false;
            Identity identity{};
            std::string refused;
            // The current fight's setup at its first frame here, the other machine's (by the fight's key: of fights this
            // one may not be in yet), whether the other has this one's, and whether they've been compared.
            bool localSetupKnown = false, peerHasSetup = false, setupCompared = false, setupsDiffer = false;
            Digests localSetup{};
            std::vector<const char*> setupNames;
            std::map<uint32_t, Digests> remoteSetups;
            DesyncRing desyncRing;
            // Once the session has ended: why, whether that's been handled, and the fight the other player left, to give
            // to this machine's player (its number on this machine, Segment::number; -1: none, or given), and the last
            // fight's number seen; when the game closes, and whether it's closing.
            std::string endReason;
            bool aloneHandled = false;
            Clock::time_point closeAt{};
            bool closing = false;
            int64_t fightLeft = -1;
            bool fightSeen = false;
            uint32_t lastFightNumber = 0;
            // The window title's status, as last set.
            std::string status;
            Clock::time_point statusTime{};
        };

        Session g_session;

        bool g_configured = std::getenv(GAME_PREFIX "_NETPLAY") != nullptr;

        // Whether a session's fights roll back: the default, unless NETPLAY_ROLLBACK=0 (session.h; read where it's first
        // asked, which may be before this file's statics are).
        bool RollbackSetting()
        {
            static const bool rollback = [] {
                const char* setting = std::getenv(GAME_PREFIX "_NETPLAY_ROLLBACK");
                return std::getenv(GAME_PREFIX "_NETPLAY") != nullptr && !(setting && *setting && std::atoi(setting) == 0);
            }();
            return rollback;
        }

        // Packets as they arrive, timed by the receiving thread, for the main thread. Never destroyed: the thread may still
        // use it as the process exits.
        struct Inbox
        {
            struct Packet
            {
                Clock::time_point arrival;
                sockaddr_in from;
                std::vector<uint8_t> data;
            };
            std::mutex mutex;
            std::vector<Packet> packets;
            HANDLE arrived = nullptr; // set as a packet arrives
        };
        Inbox& g_inbox = *new Inbox;

        // Where this machine's game is, for its Alive packets (the main thread writes it, the alive thread reads it).
        struct Whereabouts
        {
            std::atomic<uint32_t> key{ kNoKey }, frame{ 0 }, screen{ 0 };
            std::atomic<uint8_t> where{ uint8_t(Where::kOwnTime) };
        };
        Whereabouts& g_where = *new Whereabouts;

        // What the alive thread's packets carry besides (the main thread writes it): the other machine's newest clock and
        // when it came, for the echo (the round trip is measured from the handshake on), and this machine's pace.
        struct AliveShared
        {
            std::mutex mutex;
            bool peerTimeKnown = false;
            uint32_t peerTime = 0;
            std::chrono::steady_clock::time_point peerTimeArrival{};
            Pace pace{};
        };
        AliveShared& g_alive = *new AliveShared;

        // DEBUG_NET_CONDITIONS=<latency ms>[,<jitter ms>[,<loss percent>[,<seed>]]]: a network's conditions for the
        // packets this machine sends (set on both machines, for both ways). Each packet is dropped with the loss's
        // probability, or else held for the latency plus a jitter drawn evenly from [0, jitter] ms, and then sent by a
        // thread of its own, so packets can overtake each other. The draws come from a generator of their own (SplitMix64,
        // from the seed, default 1, and the player), two a packet, so runs repeat as far as the packets do, and a lower
        // loss drops a subset of a higher one's packets.
        // DEBUG_NET_OUTAGE=<when>,<ms>: the link goes down for `ms` ms from then: every packet this machine sends meanwhile
        // is dropped. DEBUG_STALL=<when>,<ms>: the game stalls for `ms` ms then (its main thread sleeps at the frame
        // boundary, as a machine that hangs would). DEBUG_NET_DESYNC=<when>: this machine's checksum of the flow's segment
        // differs then, as if its game had gone another way (a menu screen's after its frame NETPLAY_DELAY), to see a
        // session find it. <when> is a game frame, F<frame> a frame of the first fight, or S<screen>.<frame> a frame of
        // the menus' screen number <screen> (netplay::Segment, counted from 0 at boot).
        struct When
        {
            enum Kind
            {
                kGameFrame,
                kFightFrame,
                kScreenFrame,
            } kind = kGameFrame;
            uint32_t screen = 0;
            uint64_t frame = 0;
        };

        // Parses a <when>,<ms> setting.
        bool ParseWhen(const char* setting, When& when, std::chrono::milliseconds& length)
        {
            const char* p = setting;
            if (*p == 'F')
            {
                when.kind = When::kFightFrame;
                p++;
            }
            else if (*p == 'S')
            {
                when.kind = When::kScreenFrame;
                char* end = nullptr;
                when.screen = uint32_t(strtoul(p + 1, &end, 10));
                if (!end || *end != '.')
                    return false;
                p = end + 1;
            }
            char* end = nullptr;
            when.frame = strtoull(p, &end, 10);
            length = std::chrono::milliseconds(end && *end == ',' ? strtoll(end + 1, nullptr, 10) : 1000);
            return end != p;
        }

        std::string WhenText(const When& when)
        {
            char text[64];
            switch (when.kind)
            {
            case When::kFightFrame:
                snprintf(text, sizeof(text), "the first fight's frame %llu", (unsigned long long)when.frame);
                break;
            case When::kScreenFrame:
                snprintf(text, sizeof(text), "menu screen %u's frame %llu", when.screen, (unsigned long long)when.frame);
                break;
            default:
                snprintf(text, sizeof(text), "game frame %llu", (unsigned long long)when.frame);
                break;
            }
            return text;
        }

        bool Reached(const When& when, uint64_t frame, const std::optional<Segment>& segment)
        {
            switch (when.kind)
            {
            case When::kFightFrame:
                return segment && segment->fight && segment->number == 0 && segment->frame >= when.frame;
            case When::kScreenFrame:
                return segment && !segment->fight && segment->number == when.screen && segment->frame >= when.frame;
            default:
                return frame >= when.frame;
            }
        }

        struct Conditions
        {
            bool on = false;
            double latency = 0, jitter = 0, loss = 0; // ms, ms, a fraction
            uint64_t random = 0;
            struct Held
            {
                sockaddr_in to;
                std::vector<uint8_t> data;
            };
            std::mutex mutex;
            std::multimap<Clock::time_point, Held> held; // by the time each goes out, equal times in order
            HANDLE wake = nullptr, timer = nullptr;
            // The outage.
            bool outagePending = false;
            When outageWhen;
            std::chrono::milliseconds outageLength{ 0 };
            Clock::time_point outageStart{}, outageEnd{};
            // The stall.
            bool stallPending = false;
            When stallWhen;
            std::chrono::milliseconds stallLength{ 0 };
            // The checksum made to differ (DEBUG_NET_DESYNC).
            bool desyncPending = false;
            When desyncWhen;
        };

        // Never destroyed: the sending thread may still use it as the process exits.
        Conditions& g_conditions = *new Conditions;

        uint32_t Microseconds(Clock::time_point time)
        {
            return uint32_t(std::chrono::duration_cast<microseconds>(time.time_since_epoch()).count());
        }

        int64_t MicrosecondsBetween(Clock::time_point from, Clock::time_point to)
        {
            return std::chrono::duration_cast<microseconds>(to - from).count();
        }

        double SecondsBetween(Clock::time_point from, Clock::time_point to)
        {
            return std::chrono::duration<double>(to - from).count();
        }

        uint64_t NextRandom(uint64_t& state)
        {
            uint64_t z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        // In [0, 1).
        double UniformRandom(uint64_t& state)
        {
            return double(NextRandom(state) >> 11) * (1.0 / 9007199254740992.0);
        }

        // The conditions' thread: sends each held packet when its time comes.
        void SendHeld()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            Conditions& c = g_conditions;
            for (;;)
            {
                bool any = false;
                Clock::time_point next{};
                {
                    std::unique_lock lock(c.mutex);
                    while (!c.held.empty() && c.held.begin()->first <= Clock::now())
                    {
                        Conditions::Held packet = std::move(c.held.begin()->second);
                        c.held.erase(c.held.begin());
                        lock.unlock();
                        sendto(g_session.socket, reinterpret_cast<const char*>(packet.data.data()), int(packet.data.size()), 0,
                            reinterpret_cast<const sockaddr*>(&packet.to), sizeof(packet.to));
                        lock.lock();
                    }
                    if (!c.held.empty())
                    {
                        any = true;
                        next = c.held.begin()->first;
                    }
                }
                if (!any)
                {
                    WaitForSingleObject(c.wake, INFINITE);
                    continue;
                }
                LARGE_INTEGER due;
                due.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(next - Clock::now()).count() / 100);
                SetWaitableTimer(c.timer, &due, 0, nullptr, nullptr, FALSE);
                const HANDLE handles[] = { c.wake, c.timer };
                WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            }
        }

        void StartConditions(bool host)
        {
            Conditions& c = g_conditions;
            if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_NET_OUTAGE"))
            {
                c.outagePending = ParseWhen(setting, c.outageWhen, c.outageLength);
                if (c.outagePending)
                    LOG_INFO("Netplay: the link will go down for %lld ms at %s", (long long)c.outageLength.count(), WhenText(c.outageWhen).c_str());
            }
            if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_STALL"))
            {
                c.stallPending = ParseWhen(setting, c.stallWhen, c.stallLength);
                if (c.stallPending)
                    LOG_INFO("Netplay: the game will stall for %lld ms at %s", (long long)c.stallLength.count(), WhenText(c.stallWhen).c_str());
            }
            if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_NET_DESYNC"))
            {
                std::chrono::milliseconds unused{ 0 };
                c.desyncPending = ParseWhen(setting, c.desyncWhen, unused);
                if (c.desyncPending)
                    LOG_INFO("Netplay: this machine's checksum will differ at %s", WhenText(c.desyncWhen).c_str());
            }
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_NET_CONDITIONS");
            if (!setting)
                return;
            char* end = nullptr;
            c.latency = std::max(0.0, strtod(setting, &end));
            if (end && *end == ',')
                c.jitter = std::max(0.0, strtod(end + 1, &end));
            if (end && *end == ',')
                c.loss = std::clamp(strtod(end + 1, &end), 0.0, 100.0) / 100.0;
            uint64_t seed = 1;
            if (end && *end == ',')
                seed = strtoull(end + 1, &end, 0);
            c.random = seed ^ (host ? 0 : 0xD1B54A32D192ED03ull);
            c.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            c.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!c.timer)
                c.timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            c.on = true;
            std::thread(SendHeld).detach();
            LOG_INFO("Netplay: simulating a network: the packets sent held %.1f ms plus up to %.1f ms, %.1f%% dropped (seed %llu)",
                c.latency, c.jitter, c.loss * 100.0, (unsigned long long)seed);
        }

        // Under DEBUG_NET_CONDITIONS or DEBUG_NET_OUTAGE: whether the packet is dropped, and if not, whether it's held
        // for the conditions' thread to send. (The main thread and the alive thread send.)
        bool Dropped(const void* data, size_t length, const sockaddr_in& to, bool& held)
        {
            Conditions& c = g_conditions;
            held = false;
            std::lock_guard lock(c.mutex);
            const Clock::time_point now = Clock::now();
            const bool down = now >= c.outageStart && now < c.outageEnd;
            if (!c.on)
                return down;
            const double lost = UniformRandom(c.random), jitter = UniformRandom(c.random);
            if (down || lost < c.loss)
                return true;
            const Clock::time_point due = now + microseconds(int64_t((c.latency + c.jitter * jitter) * 1000.0));
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            c.held.emplace(due, Conditions::Held{ to, std::vector<uint8_t>(bytes, bytes + length) });
            SetEvent(c.wake);
            held = true;
            return false;
        }

        // Starts the outage when its frame comes, and stalls.
        void CheckConditions(uint64_t frame, const std::optional<Segment>& segment)
        {
            Conditions& c = g_conditions;
            if (c.outagePending && Reached(c.outageWhen, frame, segment))
            {
                std::lock_guard lock(c.mutex);
                c.outagePending = false;
                c.outageStart = Clock::now();
                c.outageEnd = c.outageStart + c.outageLength;
                LOG_INFO("Netplay: the link goes down for %lld ms at game frame %llu", (long long)c.outageLength.count(), (unsigned long long)frame);
            }
            if (c.stallPending && Reached(c.stallWhen, frame, segment))
            {
                c.stallPending = false;
                LOG_INFO("Netplay: the game stalls for %lld ms at game frame %llu", (long long)c.stallLength.count(), (unsigned long long)frame);
                Sleep(DWORD(c.stallLength.count()));
            }
        }

        // The receiving thread: times each packet as it arrives.
        void ReceiveThread()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            std::vector<uint8_t> buffer(2048);
            for (;;)
            {
                sockaddr_in from{};
                int fromLength = sizeof(from);
                const int length = recvfrom(g_session.socket, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                    reinterpret_cast<sockaddr*>(&from), &fromLength);
                const Clock::time_point arrival = Clock::now();
                if (length < 0)
                {
                    Sleep(1); // a packet too long, or the like
                    continue;
                }
                {
                    // While the main thread doesn't take them (a long stall), as many as a socket's buffer would hold.
                    std::lock_guard lock(g_inbox.mutex);
                    if (g_inbox.packets.size() >= kInboxLimit)
                        continue;
                    g_inbox.packets.push_back({ arrival, from, std::vector<uint8_t>(buffer.begin(), buffer.begin() + length) });
                }
                SetEvent(g_inbox.arrived);
            }
        }

        std::string Describe(const sockaddr_in& address)
        {
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
            return std::string(text) + ":" + std::to_string(ntohs(address.sin_port));
        }

        void FillHeader(Header& header, Type type)
        {
            const Session& s = g_session;
            memset(header.game, 0, sizeof(header.game));
            strncpy(header.game, GAME_PREFIX, sizeof(header.game));
            header.version = kVersion;
            header.type = uint16_t(type);
            header.session = s.id;
            const Clock::time_point now = Clock::now();
            header.time = Microseconds(now);
            header.echo = 0;
            if (s.peerTimeKnown)
                header.echo = std::max<uint32_t>(1, s.peerTime + uint32_t(MicrosecondsBetween(s.peerTimeArrival, now)));
        }

        // The two clocks: the other machine's in a packet against this one's as it arrived, the largest offset each second
        // (the least delayed packet's: the offset is the clocks' difference less the packet's way), relative to the first
        // (DriftPpm).
        void NoteClock(const Header& header, Clock::time_point arrival)
        {
            Session& s = g_session;
            const uint32_t offset = header.time - Microseconds(arrival);
            if (!s.clockKnown)
            {
                s.clockKnown = true;
                s.firstOffset = offset;
                s.clockStart = arrival;
            }
            const int64_t relative = int32_t(offset - s.firstOffset);
            // Buckets of kClockBucketSeconds: even the alive packets' (4 a second) find a packet that took little longer
            // than the least.
            const int64_t bucket = std::chrono::duration_cast<std::chrono::seconds>(arrival - s.clockStart).count() / kClockBucketSeconds;
            if (bucket != s.clockBucket)
            {
                if (s.clockBucket >= 0)
                {
                    s.clockPoints.emplace_back((double(s.clockBucket) + 0.5) * kClockBucketSeconds, double(s.clockBucketMost));
                    // Two hours' at most: the oldest half goes.
                    if (s.clockPoints.size() > 1440)
                        s.clockPoints.erase(s.clockPoints.begin(), s.clockPoints.begin() + 720);
                }
                s.clockBucket = bucket;
                s.clockBucketMost = relative;
            }
            else
                s.clockBucketMost = std::max(s.clockBucketMost, relative);
        }

        // How much faster the other machine's clock runs than this one's, in parts per million, from the offsets of the
        // least delayed packet of every kClockBucketSeconds (a line's slope through them), with the slope's standard error
        // (`error`), over `seconds`; nothing before 30 s of them. On one PC (both instances share its clock), 0 within the
        // error; two PCs' clocks differ by tens of ppm at most.
        std::optional<double> DriftPpm(double& seconds, double& error)
        {
            const Session& s = g_session;
            const std::vector<std::pair<double, double>>& points = s.clockPoints;
            if (points.size() < size_t(30 / kClockBucketSeconds))
                return std::nullopt;
            double sumT = 0, sumO = 0;
            for (const auto& [t, o] : points)
            {
                sumT += t;
                sumO += o;
            }
            const double meanT = sumT / double(points.size()), meanO = sumO / double(points.size());
            double covariance = 0, variance = 0;
            for (const auto& [t, o] : points)
            {
                covariance += (t - meanT) * (o - meanO);
                variance += (t - meanT) * (t - meanT);
            }
            if (variance <= 0)
                return std::nullopt;
            seconds = points.back().first - points.front().first;
            const double slope = covariance / variance; // microseconds a second: ppm
            double residuals = 0;
            for (const auto& [t, o] : points)
            {
                const double residual = o - meanO - slope * (t - meanT);
                residuals += residual * residual;
            }
            error = std::sqrt(residuals / double(std::max<size_t>(points.size() - 2, 1)) / variance);
            return slope;
        }

        // The other machine's clock in a packet from it, and the round trip of the echo of ours.
        void NoteTimes(const Header& header, Clock::time_point arrival)
        {
            Session& s = g_session;
            if (!s.peerTimeKnown || int32_t(header.time - s.peerTime) > 0)
            {
                s.peerTime = header.time;
                s.peerTimeArrival = arrival;
                s.peerTimeKnown = true;
                std::lock_guard lock(g_alive.mutex);
                g_alive.peerTimeKnown = true;
                g_alive.peerTime = s.peerTime;
                g_alive.peerTimeArrival = arrival;
            }
            NoteClock(header, arrival);
            if (header.echo == 0)
                return;
            const uint32_t roundTrip = Microseconds(arrival) - header.echo;
            if (roundTrip > kMaxRoundTripMicroseconds)
                return;
            s.roundTripMicroseconds = s.roundTripMicroseconds == 0 ? roundTrip : s.roundTripMicroseconds * 0.9 + roundTrip * 0.1;
            Statistics& f = s.statistics;
            f.roundTrips++;
            f.roundTripMicroseconds += roundTrip;
            f.shortestRoundTrip = std::min<uint64_t>(f.shortestRoundTrip, roundTrip);
            f.longestRoundTrip = std::max<uint64_t>(f.longestRoundTrip, roundTrip);
            // For the input delays (Choose), and the menu screen's statistics.
            s.roundTrips.push_back({ arrival, roundTrip });
            if (s.roundTrips.size() > kRoundTripsKept)
                s.roundTrips.pop_front();
            s.roundTripCount++;
            s.step.roundTrips++;
            s.step.roundTripMicroseconds += roundTrip;
        }

        // The round trips of the last kRoundTripWindow (at least the last kLeastRoundTrips): how many, their mean and their
        // 95th percentile (us).
        struct RoundTrips
        {
            uint64_t count = 0;
            double mean = 0, high = 0;
        };
        RoundTrips RecentRoundTrips()
        {
            const Session& s = g_session;
            RoundTrips result;
            if (s.roundTrips.empty())
                return result;
            const Clock::time_point since = s.roundTrips.back().arrival - kRoundTripWindow;
            std::vector<uint32_t> values;
            for (auto it = s.roundTrips.rbegin(); it != s.roundTrips.rend(); ++it)
            {
                if (it->arrival < since && values.size() >= kLeastRoundTrips)
                    break;
                values.push_back(it->microseconds);
            }
            double sum = 0;
            for (const uint32_t value : values)
                sum += value;
            std::sort(values.begin(), values.end());
            result.count = values.size();
            result.mean = sum / double(values.size());
            result.high = values[std::min(values.size() - 1, size_t(double(values.size()) * 0.95))];
            return result;
        }

        // Sends a packet to the other machine (under the simulated conditions, if any). `counted`: in the statistics.
        void Send(const void* data, size_t length, bool counted = true)
        {
            Session& s = g_session;
            if (s.socket == INVALID_SOCKET || s.peer.sin_port == 0)
                return;
            if (counted)
            {
                s.statistics.packetsSent++;
                s.statistics.bytesSent += length;
            }
            bool held = false;
            if (Dropped(data, length, s.peer, held))
            {
                if (counted)
                    s.statistics.dropped++;
            }
            else if (!held)
                sendto(s.socket, static_cast<const char*>(data), int(length), 0, reinterpret_cast<const sockaddr*>(&s.peer), sizeof(s.peer));
        }

        // The alive thread: once connected, tells the other machine where this one's game is every kAliveInterval,
        // whatever its main thread does (loading a long while, stalled), so the other knows it's still there.
        void AliveThread()
        {
            for (;;)
            {
                Sleep(DWORD(std::chrono::duration_cast<std::chrono::milliseconds>(kAliveInterval).count()));
                const Session& s = g_session;
                if (s.state == State::kEnded)
                    return;
                if (s.state != State::kConnected)
                    continue;
                Alive packet{};
                memset(packet.header.game, 0, sizeof(packet.header.game));
                strncpy(packet.header.game, GAME_PREFIX, sizeof(packet.header.game));
                packet.header.version = kVersion;
                packet.header.type = uint16_t(Type::kAlive);
                packet.header.session = s.id;
                const Clock::time_point now = Clock::now();
                packet.header.time = Microseconds(now);
                {
                    // The echo of the other's clock (FillHeader's), so that the round trip is measured while no segment
                    // runs (the handshake on), and this machine's pace.
                    std::lock_guard lock(g_alive.mutex);
                    if (g_alive.peerTimeKnown)
                        packet.header.echo = std::max<uint32_t>(1, g_alive.peerTime + uint32_t(MicrosecondsBetween(g_alive.peerTimeArrival, now)));
                    packet.pace = g_alive.pace;
                }
                packet.key = g_where.key.load();
                packet.frame = g_where.frame.load();
                packet.screen = g_where.screen.load();
                packet.where = g_where.where.load();
                Send(&packet, sizeof(packet), false);
            }
        }

        void SetWhere(Where where)
        {
            g_where.where.store(uint8_t(where));
        }

        // Parts as compared between machines, and their names here.
        Digests Digest(const std::vector<StatePart>& parts, std::vector<const char*>& names)
        {
            Digests digests{};
            names.clear();
            for (const StatePart& part : parts)
            {
                if (digests.count == kMaxParts)
                    break;
                PartDigest& digest = digests.parts[digests.count++];
                names.push_back(part.name);
                if (part.bytes.size() <= kShownBytes)
                {
                    digest.length = uint8_t(part.bytes.size());
                    memcpy(digest.bytes, part.bytes.data(), part.bytes.size());
                }
                else
                {
                    digest.length = kHashed;
                    const uint64_t hash = PartHash(part);
                    memcpy(digest.bytes, &hash, sizeof(hash));
                }
            }
            return digests;
        }

        // Whether digests from the other machine stay within their arrays.
        bool ValidDigests(const Digests& digests)
        {
            if (digests.count > kMaxParts)
                return false;
            for (uint32_t i = 0; i < digests.count; i++)
            {
                if (digests.parts[i].length > kShownBytes && digests.parts[i].length != kHashed)
                    return false;
            }
            return true;
        }

        bool SameDigest(const PartDigest& a, const PartDigest& b)
        {
            return a.length == b.length && memcmp(a.bytes, b.bytes, a.length == kHashed ? 8 : a.length) == 0;
        }

        std::string DigestText(const PartDigest& digest)
        {
            char text[8];
            std::string result = digest.length == kHashed ? "#" : "";
            for (uint32_t i = 0; i < (digest.length == kHashed ? 8u : digest.length); i++)
            {
                snprintf(text, sizeof(text), "%02X", digest.bytes[digest.length == kHashed ? 7 - i : i]);
                result += text;
            }
            return result.empty() ? "-" : result;
        }

        // The parts that differ, named, with this machine's and the other's (as bytes in hex, or # and a hash), or an
        // empty string.
        std::string Differences(const std::vector<const char*>& names, const Digests& local, const Digests& remote)
        {
            std::string text;
            for (uint32_t i = 0; i < std::max(local.count, remote.count) && i < kMaxParts; i++)
            {
                if (i < local.count && i < remote.count && SameDigest(local.parts[i], remote.parts[i]))
                    continue;
                text += text.empty() ? "" : "; ";
                text += i < names.size() ? names[i] : "a part";
                text += " (this machine's " + (i < local.count ? DigestText(local.parts[i]) : std::string("none")) + ", the other's " +
                    (i < remote.count ? DigestText(remote.parts[i]) : std::string("none")) + ")";
            }
            return text;
        }

        // The switches that change how the game runs, which both machines must have alike (frame.h): the session's own
        // (on in every session, whatever the variables say), the game heap, a sync test, the C runtime's FMA3 paths
        // (main.cpp), and the vblank rate and file redirections set for debugging.
        std::string Switches()
        {
            const char* vblank = std::getenv(GAME_PREFIX "_VBLANK_HZ");
            const char* redirect = std::getenv(GAME_PREFIX "_DEBUG_REDIRECT");
            char redirection[24] = "none";
            if (redirect && *redirect)
                snprintf(redirection, sizeof(redirection), "%016llX", (unsigned long long)XXH3_64bits(redirect, strlen(redirect)));
            char text[160];
            snprintf(text, sizeof(text), "fixed frame time %d, split random %d, no voice waits %d, game heap %d, sync test %d, FMA3 paths %d, "
                "vblank %s, redirect %s, rollback %d", FixedFrameTime(), SplitRandom(), NoVoiceWaits(), heaps::GameHeapEnabled(), synctest::Enabled(),
                _get_FMA3_enable(), vblank && *vblank ? vblank : "60", redirection, RollbackSetting());
            return text;
        }

        void CopyText(char* destination, size_t size, const std::string& text)
        {
            memset(destination, 0, size);
            memcpy(destination, text.data(), std::min(text.size(), size - 1));
        }

        std::string Text(const char* field, size_t size)
        {
            return std::string(field, strnlen(field, size));
        }

        // This machine's identity, once the disc image's hash is known.
        bool LocalIdentity()
        {
            Session& s = g_session;
            if (s.identityKnown)
                return true;
            const std::optional<uint64_t> disc = identity::DiscHash();
            if (!disc)
                return false;
            Identity& id = s.identity;
            id.build = identity::BuildId();
            id.disc = *disc;
            CopyText(id.configuration, sizeof(id.configuration), identity::BuildConfiguration());
            CopyText(id.switches, sizeof(id.switches), Switches());
            CopyText(id.machine, sizeof(id.machine), identity::Machine());
            s.identityKnown = true;
            LOG_INFO("Netplay: this machine: build %016llX (%s), disc %016llX, %s; switches: %s", (unsigned long long)id.build,
                Text(id.configuration, sizeof(id.configuration)).c_str(), (unsigned long long)id.disc, Text(id.machine, sizeof(id.machine)).c_str(),
                Text(id.switches, sizeof(id.switches)).c_str());
            return true;
        }

        // Why the other machine can't play with this one, or an empty string.
        std::string Mismatch(const Identity& other)
        {
            const Identity& id = g_session.identity;
            std::string why;
            char text[160];
            if (other.build != id.build)
            {
                snprintf(text, sizeof(text), "another build of the game (this machine's %016llX %s, the other's %016llX %s)",
                    (unsigned long long)id.build, Text(id.configuration, sizeof(id.configuration)).c_str(), (unsigned long long)other.build,
                    Text(other.configuration, sizeof(other.configuration)).c_str());
                why += text;
            }
            if (other.disc != id.disc)
            {
                snprintf(text, sizeof(text), "another disc image (this machine's %016llX, the other's %016llX)", (unsigned long long)id.disc,
                    (unsigned long long)other.disc);
                why += (why.empty() ? "" : "; ") + std::string(text);
            }
            if (Text(other.switches, sizeof(other.switches)) != Text(id.switches, sizeof(id.switches)))
                why += (why.empty() ? "" : "; ") + std::string("other switches (this machine's: ") + Text(id.switches, sizeof(id.switches)) +
                    "; the other's: " + Text(other.switches, sizeof(other.switches)) + ")";
            return why;
        }

        // A player's settings for the flow (GameState::playerSettings), for the log.
        std::string DescribeSettings(const Settings& settings)
        {
            const GameState* game = GetGameState();
            if (!settings.known)
                return "none";
            if (!game || !game->playerSettings.describe)
                return "known";
            return game->playerSettings.describe(settings.bytes);
        }

        // This machine's player's settings for the flow, as it enters it.
        Settings LocalPlayerSettings()
        {
            Settings settings;
            const GameState* game = GetGameState();
            if (game && game->playerSettings.local && game->playerSettings.size <= kSettingsSize)
                settings.known = game->playerSettings.local(settings.bytes);
            return settings;
        }

        // The settings in the other machine's segment packet, if any: false if they aren't ones the game can use.
        bool ReadSettings(const SegmentPacket& packet, Settings& settings)
        {
            const GameState* game = GetGameState();
            settings.known = false;
            if (packet.settingsSize == 0)
                return true;
            if (!game || !game->playerSettings.valid || packet.settingsSize != game->playerSettings.size || packet.settingsSize > kSettingsSize ||
                !game->playerSettings.valid(packet.settings))
                return false;
            memcpy(settings.bytes, packet.settings, packet.settingsSize);
            settings.known = true;
            return true;
        }

        // Compares the fight's setups once both are known.
        void CompareSetups()
        {
            Session& s = g_session;
            const auto remote = s.remoteSetups.find(s.step.key);
            if (!s.step.active || !s.step.fight || s.setupCompared || !s.localSetupKnown || remote == s.remoteSetups.end())
                return;
            s.setupCompared = true;
            const std::string differences = Differences(s.setupNames, s.localSetup, remote->second);
            s.setupsDiffer = !differences.empty();
            if (s.setupsDiffer)
                LOG_WARN("Netplay: fight %u isn't the same fight on both machines: %s", s.fight, differences.c_str());
            else
                LOG_INFO("Netplay: fight %u: the same setup on both machines", s.fight);
        }

        // A fight frame's state for a desync's dump (GameState::desyncRegions).
        void KeepDesyncState(uint32_t fightFrame)
        {
            Session& s = g_session;
            const GameState* game = GetGameState();
            if (!game || game->desyncRegions.empty())
                return;
            DesyncRing& ring = s.desyncRing;
            if (ring.frames.empty())
            {
                ring.frames.assign(kDesyncFrames, UINT32_MAX);
                ring.data.resize(kDesyncFrames);
            }
            const size_t slot = fightFrame % kDesyncFrames;
            std::vector<uint8_t>& data = ring.data[slot];
            data.clear();
            for (const GameState::Region& region : game->desyncRegions)
            {
                const uint8_t* bytes = guest::ToHost(region.address);
                data.insert(data.end(), bytes, bytes + region.length);
            }
            ring.frames[slot] = fightFrame;
        }

        // At the fight's first desync: its frame's state here (KeepDesyncState) to dumps/desync_<fight>_<frame>.bin, for
        // comparing with the other machine's (tools\netplay\desyncdiff.py). The file: "DSYN", the format's version (1), the
        // fight, the frame and the number of regions (little-endian 32-bit words), then each region's name (32 bytes), guest
        // address and length, and its bytes as guest memory holds them (big-endian).
        void DumpDesync(uint32_t fightFrame)
        {
            Session& s = g_session;
            const GameState* game = GetGameState();
            DesyncRing& ring = s.desyncRing;
            const size_t slot = fightFrame % kDesyncFrames;
            if (!game || ring.frames.empty() || ring.frames[slot] != fightFrame)
            {
                LOG_WARN("Netplay: fight %u, frame %u: no state kept to dump", s.fight, fightFrame);
                return;
            }
            char name[64];
            snprintf(name, sizeof(name), "desync_%u_%u.bin", s.fight, fightFrame);
            const std::string path = config::DumpPath(name);
            FILE* file = fopen(path.c_str(), "wb");
            if (!file)
            {
                LOG_WARN("Netplay: can't write %s", path.c_str());
                return;
            }
            const uint32_t header[] = { 1, s.fight, fightFrame, uint32_t(game->desyncRegions.size()) };
            fwrite("DSYN", 1, 4, file);
            fwrite(header, sizeof(header), 1, file);
            size_t offset = 0;
            const std::vector<uint8_t>& data = ring.data[slot];
            for (const GameState::Region& region : game->desyncRegions)
            {
                char regionName[32] = {};
                strncpy(regionName, region.name, sizeof(regionName) - 1);
                fwrite(regionName, 1, sizeof(regionName), file);
                fwrite(&region.address, 4, 1, file);
                fwrite(&region.length, 4, 1, file);
                fwrite(data.data() + offset, 1, region.length, file);
                offset += region.length;
            }
            fclose(file);
            LOG_INFO("Netplay: fight %u, frame %u: this machine's state in %s, to compare with the other's (tools\\netplay\\desyncdiff.py)",
                s.fight, fightFrame, path.c_str());
        }

        // A segment of the flow, for the log: "segment 7 (a fight)" or "segment 5 (menu screen 304)".
        std::string Named(uint32_t key, uint32_t screen)
        {
            char text[64];
            if (key == kNoKey)
                snprintf(text, sizeof(text), "no segment");
            else if (key & kFightKey)
                snprintf(text, sizeof(text), "segment %u (a fight)", key & ~kFightKey);
            else
                snprintf(text, sizeof(text), "segment %u (menu screen %03X)", key, screen);
            return text;
        }

        // The flow's segment a key names, its number among them.
        uint32_t Ordinal(uint32_t key)
        {
            return key & ~kFightKey;
        }

        // Ends the session as the two games went separate ways in the flow: logged, and in the window's title.
        void Apart(const std::string& why);
        // The window title's status in a session (while it waits, how long since `waitStart`).
        void UpdateStatus(bool waiting, Clock::time_point waitStart);
        // The other player's input for the current segment's frame `frame` came (the first time), in a fight that rolls
        // back: its slack, if this machine has run that frame already.
        void NoteArrival(uint32_t frame, Clock::time_point arrival);

        // The other player's inputs we have in the current segment, as the first frame we lack (frames before the delay
        // are neutral, not sent). Counted on from where it got to before: inputs both machines have are dropped as the
        // segment goes on.
        uint32_t Received()
        {
            Session& s = g_session;
            uint32_t frame = std::max(s.step.receivedFrom, s.step.delay);
            while (s.step.remote.count(frame))
                frame++;
            s.step.receivedFrom = frame;
            return frame;
        }

        // This machine's slack in a fight: how long before it needed them the other player's inputs came, on average over
        // the last kSlackFrames frames (negative when it waits for them), or kNoSlack. The machine that got to the fight
        // first waits there for the other, and is then as far ahead as the lockstep lets it be: the other's inputs come
        // just as it needs them, and it waits for them at every frame, while the other machine has the slack of both.
        int32_t LocalSlack()
        {
            const Session& s = g_session;
            if (s.slacks.size() < kSlackFrames)
                return kNoSlack;
            int64_t sum = 0;
            for (const int64_t slack : s.slacks)
                sum += slack;
            return int32_t(std::clamp<int64_t>(sum / int64_t(s.slacks.size()), INT32_MIN + 1, INT32_MAX));
        }

        // This machine's measures (Measures), the current fight's rollbacks counted in.
        Measures LocalMeasures()
        {
            const Session& s = g_session;
            const Rollback& rb = s.rb;
            const auto bounded = [](double microseconds) {
                return uint32_t(std::clamp(std::lround(microseconds), 1l, long(kMaxMeasureMicroseconds)));
            };
            Measures m{};
            if (s.menuWorkFrames >= kWorkFramesMeasured)
                m.menuWorkUs = bounded(s.menuWorkMicroseconds / double(s.menuWorkFrames));
            if (s.fightWorkFrames >= kWorkFramesMeasured)
                m.fightWorkUs = bounded(s.fightWorkMicroseconds / double(s.fightWorkFrames));
            const uint64_t replayed = s.replayFrames + rb.laterFrames;
            if (replayed)
            {
                m.replayFrameUs = bounded((s.replayMicroseconds + rb.laterMilliseconds * 1000.0) / double(replayed));
                m.replayFrames = uint32_t(std::min<uint64_t>(replayed, UINT32_MAX));
            }
            if (const uint64_t loads = s.loads + rb.rollbacks)
            {
                m.loadUs = bounded((s.loadMicroseconds + rb.loadMicroseconds) / double(loads));
                m.drawWaitUs = bounded((s.drawWaitMicroseconds + rb.drawWaitMicroseconds) / double(loads));
            }
            return m;
        }

        // Measures from the other machine, within what a machine measures.
        Measures BoundedMeasures(const Measures& m)
        {
            Measures bounded = m;
            bounded.menuWorkUs = std::min(m.menuWorkUs, kMaxMeasureMicroseconds);
            bounded.fightWorkUs = std::min(m.fightWorkUs, kMaxMeasureMicroseconds);
            bounded.replayFrameUs = std::min(m.replayFrameUs, kMaxMeasureMicroseconds);
            bounded.loadUs = std::min(m.loadUs, kMaxMeasureMicroseconds);
            bounded.drawWaitUs = std::min(m.drawWaitUs, kMaxMeasureMicroseconds);
            return bounded;
        }

        // The frames a rollback can replay beside the present frame on a machine with these measures (FitsFor; ANALYSIS.md,
        // Phase 6, The input delays a session chooses). A replayed frame's time as measured once kReplayFramesMeasured
        // are, else a fight's frame's work (a replayed frame runs the same game work, its rendering discarded), else the
        // menus' times kFightPerMenuWork, else kDefaultFits; the present frame's work as a fight's frame's, or the menus'
        // times kFightPerMenuWork.
        uint32_t Fits(const Measures& m, FitsFrom& from)
        {
            const Session& s = g_session;
            const double fightWork = m.fightWorkUs ? double(m.fightWorkUs) : m.menuWorkUs * kFightPerMenuWork;
            double replay = 0;
            if (m.replayFrames >= kReplayFramesMeasured && m.replayFrameUs)
            {
                replay = m.replayFrameUs;
                from = FitsFrom::kReplays;
            }
            else if (fightWork > 0)
            {
                replay = fightWork;
                from = FitsFrom::kFrameWork;
            }
            else
            {
                from = FitsFrom::kDefault;
                return std::min(kDefaultFits, s.window);
            }
            return FitsFor(replay, fightWork > 0 ? fightWork : replay, m.loadUs ? double(m.loadUs) : kDefaultLoadMicroseconds,
                m.drawWaitUs ? double(m.drawWaitUs) : kDefaultDrawWaitMicroseconds, s.window);
        }

        // The frames after which an input sent at a frame boundary is on the other machine, a round trip and a frame's
        // work given (ANALYSIS.md, Phase 6, Route 2 under network conditions): ceil((RTT / 2 + work) / frame).
        uint32_t Late(double roundTripUs, double workUs)
        {
            return uint32_t(std::clamp(std::ceil((roundTripUs / 2 + workUs) / kFrameMicroseconds), 0.0, double(kMaxDelay)));
        }

        // The larger machine's frames' work, as the formulas reckon it (at least kLeastWorkMicroseconds, at most a frame).
        double FramesWork(const Measures& local)
        {
            const Session& s = g_session;
            double work = std::max<double>({ kLeastWorkMicroseconds, double(local.menuWorkUs), double(local.fightWorkUs) });
            if (s.peerMeasuresKnown)
                work = std::max<double>({ work, double(s.peerMeasures.menuWorkUs), double(s.peerMeasures.fightWorkUs) });
            return std::min(work, kFrameMicroseconds);
        }

        // The host's choice of a segment's input delays (session.h), from the round trips of the last 10 s and both
        // machines' measures: in lockstep (a menu screen, or a fight without rollback), the delay 60 frames a second needs at
        // the mean round trip with a frame to spare, or at its 95th percentile if more (ANALYSIS.md, Phase 6, Route 2 under
        // network conditions: the frames after which the other's input comes, less one, the GPU's queue holding a frame);
        // in a fight that rolls back, the smallest that keeps a rollback (the frames after which the other's input comes,
        // less the delay) within what the slower machine fits beside a frame (Fits), and at least kLeastFightDelay. The
        // host's variables fix them; without a round trip measured, the defaults (3 and 2).
        Delays Choose(uint32_t segment)
        {
            const Session& s = g_session;
            Delays d;
            d.segment = segment;
            const RoundTrips trips = RecentRoundTrips();
            const Measures local = LocalMeasures();
            const double work = FramesWork(local);
            d.roundTripUs = uint32_t(trips.mean);
            d.roundTripHighUs = uint32_t(trips.high);
            d.workUs = uint32_t(work);
            if (s.lockstepFixed)
            {
                d.lockstep = uint8_t(s.delay);
                d.lockstepHow = uint8_t(How::kFixed);
            }
            else if (!trips.count)
            {
                d.lockstep = 3;
                d.lockstepHow = uint8_t(How::kDefault);
            }
            FitsFrom hostFrom = FitsFrom::kDefault, joiningFrom = FitsFrom::kDefault;
            const Measures peer = s.peerMeasuresKnown ? s.peerMeasures : Measures{};
            d.fitsHost = uint8_t(Fits(local, hostFrom));
            d.fitsJoining = uint8_t(Fits(peer, joiningFrom));
            d.fitsHostFrom = uint8_t(hostFrom);
            d.fitsJoiningFrom = uint8_t(joiningFrom);
            const DelayChoice choice = DelaysFor(trips.mean, trips.high, work, std::min(d.fitsHost, d.fitsJoining));
            d.late = uint8_t(choice.late);
            d.needed = uint8_t(choice.needed);
            if (!s.lockstepFixed && trips.count)
            {
                d.lockstep = uint8_t(choice.lockstep);
                d.lockstepHow = uint8_t(How::kChosen);
            }
            if (s.rollbackFixed)
            {
                d.rollback = uint8_t(s.fightDelay);
                d.rollbackHow = uint8_t(How::kFixed);
            }
            else if (!trips.count)
            {
                d.rollback = uint8_t(kLeastFightDelay);
                d.rollbackHow = uint8_t(How::kDefault);
            }
            else
            {
                d.rollback = uint8_t(choice.rollback);
                d.rollbackHow = uint8_t(How::kChosen);
            }
            return d;
        }

        const char* FitsFromText(uint8_t from)
        {
            switch (FitsFrom(from))
            {
            case FitsFrom::kReplays:
                return "its replayed frames";
            case FitsFrom::kFrameWork:
                return "its frames' work, no replay measured yet";
            case FitsFrom::kDefault:
                return "nothing measured yet";
            default:
                return "?";
            }
        }

        // Why a segment plays with its delay, for the log (both machines: the joining one from the host's Delays).
        std::string DelaysText(const Delays& d, bool rollbackFight)
        {
            char text[512];
            const How how = How(rollbackFight ? d.rollbackHow : d.lockstepHow);
            if (how == How::kFixed)
                snprintf(text, sizeof(text), "fixed by the host's %s", rollbackFight ? GAME_PREFIX "_NETPLAY_FIGHT_DELAY" : GAME_PREFIX "_NETPLAY_DELAY");
            else if (how == How::kDefault)
                snprintf(text, sizeof(text), "the default: no round trip measured yet");
            else if (!rollbackFight)
                snprintf(text, sizeof(text), "chosen for a round trip of %.1f ms (95th percentile %.1f) and a frame's work of %.1f ms: 60 frames a "
                    "second needs %u in lockstep, and one to spare%s", d.roundTripUs / 1000.0, d.roundTripHighUs / 1000.0, d.workUs / 1000.0,
                    d.needed, d.lockstep > d.needed + 1 ? " (more for the 95th percentile)" : "");
            else
                snprintf(text, sizeof(text), "chosen for a round trip of %.1f ms and a frame's work of %.1f ms, at which the other player's "
                    "input comes %u frames after the frame it's for; rollbacks of %u frames fit beside a frame on the host (from %s) and %u on "
                    "the joining machine (from %s), so %u - %u, and at least %u", d.roundTripUs / 1000.0, d.workUs / 1000.0, d.late, d.fitsHost,
                    FitsFromText(d.fitsHostFrom), d.fitsJoining, FitsFromText(d.fitsJoiningFrom), d.late, std::min(d.fitsHost, d.fitsJoining),
                    std::min(kLeastFightDelay, std::max<uint32_t>(d.late, 1)));
            return text;
        }

        // A machine's measures, for the log.
        std::string MeasuresText(const Measures& m)
        {
            char text[256];
            snprintf(text, sizeof(text), "a frame's work %.2f ms in the menus and %.2f ms in fights, a replayed frame %.2f ms (%u measured), a "
                "load %.2f ms after a wait of %.2f ms for the GPU's draws", m.menuWorkUs / 1000.0, m.fightWorkUs / 1000.0, m.replayFrameUs / 1000.0,
                m.replayFrames, m.loadUs / 1000.0, m.drawWaitUs / 1000.0);
            return text;
        }

        // What the alive thread's packets carry: this machine's measures, and the host's latest choice.
        void PublishPace()
        {
            Session& s = g_session;
            Pace pace{};
            pace.measures = LocalMeasures();
            pace.delays = s.latestDelays;
            std::lock_guard lock(g_alive.mutex);
            g_alive.pace = pace;
        }

        // The host chooses a segment's delays (Choose): final once chosen, in its packets from then on.
        void ChooseDelays(uint32_t segment)
        {
            Session& s = g_session;
            if (!s.host || s.delays.count(segment))
                return;
            const Delays d = Choose(segment);
            s.delays[segment] = d;
            s.latestDelays = d;
            while (s.delays.size() > 4)
                s.delays.erase(s.delays.begin());
            PublishPace();
            LOG_INFO("Netplay: the input delays of the flow's segment %u: %u frames in lockstep (%s)%s%s", segment, d.lockstep,
                DelaysText(d, false).c_str(), s.rollback ? ", and if it's a fight, " : "",
                s.rollback ? (std::to_string(d.rollback) + " frames rolling back (" + DelaysText(d, true) + ")").c_str() : "");
        }

        // The host's choice of delays in its packet (the joining machine's view), within what a host chooses: kept the
        // first time a segment's comes, for segments this machine hasn't played (choices are final).
        void NoteDelays(const Delays& received)
        {
            Session& s = g_session;
            if (s.host || received.segment == kNoKey || received.lockstepHow > uint8_t(How::kFixed) || received.rollbackHow > uint8_t(How::kFixed))
                return;
            // The segment this machine is in (its delay may still be unknown) or one after it.
            const uint32_t first = s.segmentsStarted > 0 ? s.segmentsStarted - 1 : 0;
            if (received.segment < first || received.segment >= s.segmentsStarted + uint32_t(kEarlyKept))
                return;
            Delays d = received;
            d.lockstep = uint8_t(std::min<uint32_t>(d.lockstep, kMaxDelay));
            d.rollback = uint8_t(std::min<uint32_t>(d.rollback, kMaxDelay));
            d.needed = uint8_t(std::min<uint32_t>(d.needed, kMaxDelay));
            d.late = uint8_t(std::min<uint32_t>(d.late, kMaxDelay));
            d.fitsHost = uint8_t(std::min<uint32_t>(d.fitsHost, kMaxWindow));
            d.fitsJoining = uint8_t(std::min<uint32_t>(d.fitsJoining, kMaxWindow));
            d.fitsHostFrom = uint8_t(std::min<uint32_t>(d.fitsHostFrom, uint32_t(FitsFrom::kReplays)));
            d.fitsJoiningFrom = uint8_t(std::min<uint32_t>(d.fitsJoiningFrom, uint32_t(FitsFrom::kReplays)));
            d.roundTripUs = std::min(d.roundTripUs, kMaxRoundTripMicroseconds);
            d.roundTripHighUs = std::min(d.roundTripHighUs, kMaxRoundTripMicroseconds);
            d.workUs = std::min(d.workUs, kMaxMeasureMicroseconds);
            const auto known = s.delays.find(d.segment);
            if (known == s.delays.end())
            {
                s.delays[d.segment] = d;
                while (!s.delays.empty() && s.delays.begin()->first < first)
                    s.delays.erase(s.delays.begin());
                while (s.delays.size() > kEarlyKept)
                    s.delays.erase(std::prev(s.delays.end()));
            }
            else if ((known->second.lockstep != d.lockstep || known->second.rollback != d.rollback) && !s.delaysMismatchLogged)
            {
                s.delaysMismatchLogged = true;
                LOG_WARN("Netplay: the host chose the flow's segment %u's delays again, otherwise (%u and %u frames, before %u and %u): the first "
                         "choice stands (logged once)", d.segment, d.lockstep, d.rollback, known->second.lockstep, known->second.rollback);
            }
        }

        // The pace in a packet from the other machine: its measures (the host chooses from them), and from the host, its
        // choice of delays.
        void NotePace(const Pace& pace)
        {
            Session& s = g_session;
            s.peerMeasures = BoundedMeasures(pace.measures);
            s.peerMeasuresKnown = true;
            NoteDelays(pace.delays);
        }

        // This machine's player's frames in the flow: the main thread's work in the frame that ran before this boundary
        // (netplay::LastFrameWork), a frame run as the present, not a replay's, a menu screen's or a fight's.
        void NoteFrameWork(bool fight)
        {
            Session& s = g_session;
            const FrameWork work = LastFrameWork();
            if (!work.known || work.replayed)
                return;
            const double microseconds = double(std::min<uint64_t>(work.microseconds, kMaxMeasureMicroseconds));
            (fight ? s.fightWorkMicroseconds : s.menuWorkMicroseconds) += microseconds;
            (fight ? s.fightWorkFrames : s.menuWorkFrames)++;
        }

        // A segment's inputs from frame `peerReceived` on (those the other player lacks; the segment's first `delay` frames
        // have none) and its last checksums, with the fight's setup here if `setup` isn't null, kSetupReceived if
        // `setupReceived`, and this player's settings for the flow if `settings` isn't null.
        void SendSegment(uint32_t key, uint32_t screen, uint32_t delay, const std::map<uint32_t, Pad>& local,
            const std::map<uint32_t, uint64_t>& localChecksums, uint32_t peerReceived, uint32_t received, int32_t slack,
            const Digests* setup = nullptr, bool setupReceived = false, const Settings* settings = nullptr)
        {
            Session& s = g_session;
            uint8_t buffer[sizeof(SegmentPacket) + kMaxPads * sizeof(Pad) + kChecksumsSent * sizeof(Checksum) + sizeof(Digests)];
            SegmentPacket& packet = *reinterpret_cast<SegmentPacket*>(buffer);
            memset(&packet, 0, sizeof(packet));
            FillHeader(packet.header, Type::kSegment);
            packet.key = key;
            packet.screen = screen;
            packet.received = received;
            packet.slack = slack;
            packet.setup = uint8_t((setup ? kSetupIncluded : 0) | (setupReceived ? kSetupReceived : 0));
            packet.delay = uint8_t(delay);
            packet.pace.measures = LocalMeasures();
            packet.pace.delays = s.latestDelays;
            const GameState* game = GetGameState();
            if (settings && settings->known && game && game->playerSettings.size <= kSettingsSize)
            {
                packet.settingsSize = uint8_t(game->playerSettings.size);
                memcpy(packet.settings, settings->bytes, game->playerSettings.size);
            }
            // Our inputs the other player lacks, oldest first.
            uint32_t first = std::max(peerReceived, delay);
            const uint32_t last = local.empty() ? first : std::max(first, local.rbegin()->first + 1);
            if (last - first > kMaxPads)
                first = last - kMaxPads;
            packet.first = first;
            uint8_t* cursor = buffer + sizeof(SegmentPacket);
            uint16_t pads = 0;
            for (uint32_t frame = first; frame < last; frame++)
            {
                const auto input = local.find(frame);
                if (input == local.end())
                    break;
                memcpy(cursor, &input->second, sizeof(Pad));
                cursor += sizeof(Pad);
                pads++;
            }
            packet.pads = pads;
            uint16_t checksums = 0;
            for (auto it = localChecksums.rbegin(); it != localChecksums.rend() && checksums < kChecksumsSent; ++it, checksums++)
            {
                const Checksum checksum{ it->first, it->second };
                memcpy(cursor, &checksum, sizeof(checksum));
                cursor += sizeof(checksum);
            }
            packet.checksums = checksums;
            if (setup)
            {
                memcpy(cursor, setup, sizeof(Digests));
                cursor += sizeof(Digests);
            }
            Send(buffer, size_t(cursor - buffer));
        }

        void SendSegment()
        {
            Session& s = g_session;
            const Lockstep& step = s.step;
            if (!step.active)
                return;
            // A fight's setup until the other machine has it.
            const Digests* setup = step.fight && s.localSetupKnown && !s.peerHasSetup ? &s.localSetup : nullptr;
            const bool setupReceived = step.fight && s.remoteSetups.count(step.key) != 0;
            SendSegment(step.key, step.screen, step.delay, step.local, step.localChecksums, step.peerReceived, Received(),
                step.fight ? LocalSlack() : kNoSlack, setup, setupReceived, &s.localSettings);
            s.lastSent = Clock::now();
        }

        // The window title's status (ui::SetTitleStatus), at most every half second unless `now`.
        void ShowStatus(const std::string& status, bool now = false)
        {
            Session& s = g_session;
            const Clock::time_point time = Clock::now();
            if (status == s.status || (!now && time - s.statusTime < std::chrono::milliseconds(500)))
                return;
            s.status = status;
            s.statusTime = time;
            ui::SetTitleStatus(status);
        }

        // What the other machine's game is doing, as its newest Alive says, for the log.
        std::string PeerWhere()
        {
            const Session& s = g_session;
            if (!s.peerWhereKnown)
                return "its game's whereabouts aren't known yet";
            const Alive& alive = s.peerWhere;
            std::string where;
            switch (Where(alive.where))
            {
            case Where::kOwnTime:
                where = "its player plays on their own, outside the multiplayer flow";
                break;
            case Where::kBetween:
                where = "its game loads or leaves a screen, after " + Named(alive.key, alive.screen);
                break;
            case Where::kInSegment:
                where = "its game is in " + Named(alive.key, alive.screen) + " at frame " + std::to_string(alive.frame);
                break;
            case Where::kWaiting:
                where = "its game waits for this one's input in " + Named(alive.key, alive.screen) + " at frame " + std::to_string(alive.frame);
                break;
            }
            char age[48];
            snprintf(age, sizeof(age), " (%.1f s ago)", SecondsBetween(s.peerWhereArrival, Clock::now()));
            return where + age;
        }

        void CompareChecksum(uint32_t frame)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            const auto local = step.localChecksums.find(frame);
            const auto remote = step.remoteChecksums.find(frame);
            if (local == step.localChecksums.end() || remote == step.remoteChecksums.end())
                return;
            if (step.fight)
                s.statistics.compared++;
            else
                step.compared++;
            if (local->second == remote->second || step.differs)
                return;
            step.differs = true;
            if (!step.fight)
            {
                char why[160];
                snprintf(why, sizeof(why), "the menus went out of step: %s differs from the other machine's from its frame %u",
                    Named(step.key, step.screen).c_str(), frame);
                Apart(why);
                return;
            }
            s.desync = true;
            CompareSetups();
            if (frame == 0)
                LOG_WARN("Netplay: fight %u differs from the other player's from its first frame: not the same fight%s", s.fight,
                    !s.setupCompared ? "" : s.setupsDiffer ? " (its setup differs, above)" : ", though its setup is the same");
            else
                LOG_WARN("Netplay: fight %u differs from the other player's from its frame %u (a desync)", s.fight, frame);
            DumpDesync(frame);
        }

        // Whether the other machine's menu screen of this segment is the same as this one's.
        void CompareScreens()
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            if (step.fight || !step.remoteScreenKnown || step.apart || step.remoteScreen == step.screen)
                return;
            step.apart = true;
            char why[160];
            snprintf(why, sizeof(why), "the menus went out of step: the flow's segment %u is screen %03X here and %03X on the other machine",
                Ordinal(step.key), step.screen, step.remoteScreen);
            Apart(why);
        }

        // Why a machine leaves the session (Bye::reason).
        enum class Leaving : uint8_t
        {
            kLeaves,    // its player leaves (the window closes), or the session ended there otherwise
            kApart,     // the two games went separate ways in the flow
        };

        void SendBye(Leaving reason)
        {
            Session& s = g_session;
            Bye bye;
            FillHeader(bye.header, Type::kBye);
            bye.reason = uint8_t(reason);
            // Sent a few times, directly: it may be the last thing this machine sends.
            for (int i = 0; i < 3; i++)
                sendto(s.socket, reinterpret_cast<const char*>(&bye), sizeof(bye), 0, reinterpret_cast<const sockaddr*>(&s.peer), sizeof(s.peer));
        }

        void End(const char* why, Leaving reason = Leaving::kLeaves)
        {
            Session& s = g_session;
            if (s.state == State::kEnded)
                return;
            LOG_WARN("Netplay: the session ends: %s", why);
            if (s.state == State::kConnected)
                SendBye(reason);
            s.endReason = why;
            s.state = State::kEnded;
        }

        void Apart(const std::string& why)
        {
            if (g_session.state == State::kEnded)
                return;
            End(why.c_str(), Leaving::kApart);
            ShowStatus("netplay: " + why + "; the game closes", true);
        }

        // A segment packet from the other machine: inputs and checksums of its segment, wherever this one is.
        void ReceiveSegment(const uint8_t* data, int length, Clock::time_point arrival)
        {
            Session& s = g_session;
            if (length < int(sizeof(SegmentPacket)))
                return;
            SegmentPacket packet;
            memcpy(&packet, data, sizeof(packet));
            const size_t setupBytes = (packet.setup & kSetupIncluded) ? sizeof(Digests) : 0;
            // Every count and frame within what a sender in step sends.
            if (packet.pads > kMaxPads || packet.checksums > kChecksumsSent || packet.first >= kFrameWindow || packet.settingsSize > kSettingsSize ||
                length < int(sizeof(SegmentPacket) + packet.pads * sizeof(Pad) + packet.checksums * sizeof(Checksum) + setupBytes))
                return;
            NotePace(packet.pace);
            Lockstep& step = s.step;
            const bool current = step.active && packet.key == step.key;
            // The same segment of the flow, but a fight on one machine and a menu screen on the other.
            if (step.active && !current && Ordinal(packet.key) == Ordinal(step.key))
            {
                char why[160];
                snprintf(why, sizeof(why), "the games went out of step: the flow's segment %u is %s here and %s on the other machine",
                    Ordinal(step.key), step.fight ? "a fight" : "a menu screen", (packet.key & kFightKey) ? "a fight" : "a menu screen");
                Apart(why);
                return;
            }
            if (!current)
            {
                // The other machine is still in a segment this one has ended, and lacks inputs of it: they may have been
                // lost, and this machine sends no more of them on its own.
                for (const EndedSegment& ended : s.ended)
                {
                    if (ended.key != packet.key)
                        continue;
                    if (!ended.local.empty() && packet.received <= ended.local.rbegin()->first)
                        SendSegment(ended.key, ended.screen, ended.delay, ended.local, ended.checksums, packet.received, kNothingNeeded, kNoSlack);
                    return;
                }
                // A segment this machine has played and forgotten.
                if (Ordinal(packet.key) < s.segmentsStarted)
                    return;
            }
            // The other player's settings for the flow, as they entered it.
            Settings settings;
            if (!ReadSettings(packet, settings))
            {
                Apart("the other player's settings for the flow aren't ones this game can use");
                return;
            }
            // A fight's setup (before its checksums, which a different setup explains), and whether the other has this
            // machine's.
            if (setupBytes && (packet.key & kFightKey))
            {
                Digests setup;
                memcpy(&setup, data + sizeof(SegmentPacket) + packet.pads * sizeof(Pad) + packet.checksums * sizeof(Checksum), sizeof(setup));
                if (ValidDigests(setup) && (s.remoteSetups.size() < kEarlyKept || s.remoteSetups.count(packet.key)))
                {
                    s.remoteSetups.emplace(packet.key, setup);
                    CompareSetups();
                }
            }
            if (current && step.fight && (packet.setup & kSetupReceived))
                s.peerHasSetup = true;
            // The input delay the other machine plays the segment with: the host's choice (Delays), the same on both, or
            // their frames would go separate ways. The joining machine that hasn't heard of the choice yet takes it.
            const uint32_t peerDelay = std::min<uint32_t>(packet.delay, kMaxDelay);
            if (current && step.delayKnown && peerDelay != step.delay)
            {
                char why[192];
                snprintf(why, sizeof(why), "the two machines play %s with different input delays (this one %u frames, the other %u)",
                    Named(step.key, step.screen).c_str(), step.delay, peerDelay);
                Apart(why);
                return;
            }
            if (current && !step.delayKnown && !s.host)
            {
                step.peerDelayKnown = true;
                step.peerDelay = peerDelay;
            }
            // Inputs for a segment this machine isn't in yet (it may be loading it, or be on its own still) wait until it
            // starts.
            Early* early = nullptr;
            if (!current)
            {
                if (s.early.size() >= kEarlyKept && !s.early.count(packet.key))
                    s.early.erase(s.early.begin());
                early = &s.early[packet.key];
                early->screenKnown = true;
                early->screen = packet.screen;
                if (settings.known && !early->settings.known)
                    early->settings = settings;
                early->delayKnown = true;
                early->delay = peerDelay;
            }
            else
            {
                if (settings.known && !s.remoteSettings.known)
                    s.remoteSettings = settings;
                if (!step.fight)
                {
                    step.remoteScreenKnown = true;
                    step.remoteScreen = packet.screen;
                    CompareScreens();
                }
            }
            std::map<uint32_t, RemoteInput>& inputs = current ? step.remote : early->inputs;
            const uint8_t* cursor = data + sizeof(SegmentPacket);
            for (uint32_t i = 0; i < packet.pads; i++, cursor += sizeof(Pad))
            {
                RemoteInput input;
                memcpy(&input.pad, cursor, sizeof(input.pad));
                input.arrival = arrival;
                if (inputs.emplace(packet.first + i, input).second && current)
                    NoteArrival(packet.first + i, arrival);
            }
            std::map<uint32_t, uint64_t>& checksums = current ? step.remoteChecksums : early->checksums;
            for (uint32_t i = 0; i < packet.checksums; i++, cursor += sizeof(Checksum))
            {
                Checksum checksum;
                memcpy(&checksum, cursor, sizeof(checksum));
                if (checksum.frame >= kFrameWindow)
                    continue;
                if (checksums.emplace(checksum.frame, checksum.checksum).second && current)
                    CompareChecksum(checksum.frame);
            }
            if (current)
            {
                step.peerReceived = std::max(step.peerReceived, packet.received);
                // (With rollback, kNoSlack too: the other machine started its slack afresh after a long wait, and the
                // time sync waits for it.)
                if (packet.slack != kNoSlack || (s.rollback && step.fight))
                    s.remoteSlack = packet.slack;
            }
        }

        // The input delays, for the log: fixed, or chosen for each segment.
        std::string DelayText()
        {
            const Session& s = g_session;
            if (s.lockstepFixed)
                return "input delay " + std::to_string(s.delay) + " frames";
            return "input delays chosen for each segment from the round trip";
        }

        // How fights play, for the log.
        std::string RollbackText()
        {
            const Session& s = g_session;
            if (!s.rollback)
                return "; fights in lockstep";
            char text[160];
            if (s.rollbackFixed)
                snprintf(text, sizeof(text), "; fights roll back, with an input delay of %u frames and a window of %u", s.fightDelay, s.window);
            else
                snprintf(text, sizeof(text), "; fights roll back, with an input delay chosen for each fight and a window of %u", s.window);
            return text;
        }

        // The joining machine takes the host's window, and learns whether the host's variables fix its input delays (it has
        // played no segment's frame yet: WaitForConnection). Its own delays' variables don't apply: the host chooses each
        // segment's (Delays).
        void AdoptDelay(uint32_t delay, uint32_t fightDelay, uint32_t window, uint8_t fixed)
        {
            Session& s = g_session;
            if ((s.lockstepFixed || (s.rollback && s.rollbackFixed)) && !s.ignoredDelaysLogged)
            {
                s.ignoredDelaysLogged = true;
                LOG_INFO("Netplay: this machine's %s ignored: the host's input delays apply", s.rollbackFixed ? GAME_PREFIX "_NETPLAY_DELAY and "
                    GAME_PREFIX "_NETPLAY_FIGHT_DELAY are" : GAME_PREFIX "_NETPLAY_DELAY is");
            }
            s.lockstepFixed = (fixed & kFixedLockstep) != 0;
            s.rollbackFixed = (fixed & kFixedRollback) != 0;
            s.delay = std::min<uint32_t>(delay, kMaxDelay);
            s.fightDelay = std::min<uint32_t>(fightDelay, kMaxDelay);
            if (!s.rollback)
                return;
            window = std::clamp<uint32_t>(window, 1, kMaxWindow);
            if (window != s.window)
                LOG_INFO("Netplay: the host's window in fights, %u frames, instead of this machine's %u", window, s.window);
            s.window = window;
        }

        void Receive(const uint8_t* data, int length, const sockaddr_in& from, Clock::time_point arrival)
        {
            Session& s = g_session;
            if (length < int(sizeof(Header)))
                return;
            Header header;
            memcpy(&header, data, sizeof(header));
            char game[sizeof(header.game) + 1] = {};
            memcpy(game, header.game, sizeof(header.game));
            if (strcmp(game, GAME_PREFIX) != 0)
                return;
            if (header.version != kVersion)
            {
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    LOG_WARN("Netplay: %s runs another version of the game's netplay (protocol %u, this one's %u), which can't play "
                             "with this one", Describe(from).c_str(), header.version, kVersion);
                }
                return;
            }
            if (Type(header.type) != Type::kAlive)
                s.statistics.packetsReceived++; // (as the packets sent count, the alive thread's aren't)
            switch (Type(header.type))
            {
            case Type::kHello:
            {
                // Answered once this machine knows its identity (the disc image's hash may still be being computed): the
                // joining one sends hellos until then.
                if (!s.host || length < int(sizeof(Hello)) || !LocalIdentity())
                    return;
                Hello hello;
                memcpy(&hello, data, sizeof(hello));
                const bool known = s.state == State::kConnected && from.sin_addr.s_addr == s.peer.sin_addr.s_addr && from.sin_port == s.peer.sin_port;
                if (s.state == State::kConnecting)
                {
                    // Another build, disc or switches: refused, with this machine's identity, so the other can say why too.
                    if (const std::string why = Mismatch(hello.identity); !why.empty())
                    {
                        if (why != s.refused)
                            LOG_ERROR("Netplay: refusing the player at %s (%s): %s", Describe(from).c_str(),
                                Text(hello.identity.machine, sizeof(hello.identity.machine)).c_str(), why.c_str());
                        s.refused = why;
                        Refuse refuse;
                        FillHeader(refuse.header, Type::kRefuse);
                        refuse.identity = s.identity;
                        sendto(s.socket, reinterpret_cast<const char*>(&refuse), sizeof(refuse), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
                        return;
                    }
                    s.peer = from;
                    s.peerName = Describe(from);
                    s.state = State::kConnected;
                    s.lastArrival = arrival;
                    LOG_INFO("Netplay: player 2 at %s (%s); %s, seed %u%s", s.peerName.c_str(),
                        Text(hello.identity.machine, sizeof(hello.identity.machine)).c_str(), DelayText().c_str(), s.seed, RollbackText().c_str());
                }
                else if (!known)
                    return;
                NoteTimes(header, arrival);
                Welcome welcome;
                FillHeader(welcome.header, Type::kWelcome);
                welcome.seed = s.seed;
                welcome.delay = uint16_t(s.delay);
                welcome.fightDelay = uint16_t(s.fightDelay);
                welcome.window = uint16_t(s.window);
                welcome.fixed = uint8_t((s.lockstepFixed ? kFixedLockstep : 0) | (s.rollbackFixed ? kFixedRollback : 0));
                welcome.identity = s.identity;
                Send(&welcome, sizeof(welcome));
                return;
            }
            case Type::kWelcome:
            {
                if (s.host || length < int(sizeof(Welcome)) || s.state != State::kConnecting || !LocalIdentity())
                    return;
                Welcome welcome;
                memcpy(&welcome, data, sizeof(welcome));
                if (const std::string why = Mismatch(welcome.identity); !why.empty())
                {
                    LOG_ERROR("Netplay: the host at %s (%s) can't play with this machine: %s", s.peerName.c_str(),
                        Text(welcome.identity.machine, sizeof(welcome.identity.machine)).c_str(), why.c_str());
                    End("the host's game isn't the same");
                    return;
                }
                s.id = welcome.header.session;
                s.seed = welcome.seed;
                AdoptDelay(welcome.delay, welcome.fightDelay, welcome.window, welcome.fixed);
                s.state = State::kConnected;
                s.lastArrival = arrival;
                NoteTimes(header, arrival);
                LOG_INFO("Netplay: joined %s (%s) as player 2; %s, seed %u%s", s.peerName.c_str(),
                    Text(welcome.identity.machine, sizeof(welcome.identity.machine)).c_str(), DelayText().c_str(), s.seed, RollbackText().c_str());
                return;
            }
            case Type::kRefuse:
            {
                if (s.host || length < int(sizeof(Refuse)) || s.state != State::kConnecting || !LocalIdentity())
                    return;
                Refuse refuse;
                memcpy(&refuse, data, sizeof(refuse));
                const std::string why = Mismatch(refuse.identity);
                LOG_ERROR("Netplay: the host at %s (%s) refused to play with this machine: %s", s.peerName.c_str(),
                    Text(refuse.identity.machine, sizeof(refuse.identity.machine)).c_str(),
                    why.empty() ? "it says the games aren't the same" : why.c_str());
                End("the host refused this machine");
                return;
            }
            default:
                break;
            }
            if (s.state != State::kConnected || header.session != s.id)
                return;
            s.lastArrival = std::max(s.lastArrival, arrival);
            switch (Type(header.type))
            {
            case Type::kAlive:
                if (length < int(sizeof(Alive)))
                    return;
                memcpy(&s.peerWhere, data, sizeof(Alive));
                s.peerWhereKnown = true;
                s.peerWhereArrival = arrival;
                NoteTimes(header, arrival);
                NotePace(s.peerWhere.pace);
                return;
            case Type::kSegment:
                NoteTimes(header, arrival);
                ReceiveSegment(data, length, arrival);
                return;
            case Type::kBye:
            {
                Bye bye{};
                if (length >= int(sizeof(Bye)))
                    memcpy(&bye, data, sizeof(bye));
                if (Leaving(bye.reason) == Leaving::kApart)
                {
                    LOG_WARN("Netplay: the other machine found the two games out of step in the flow");
                    s.endReason = "the other machine found the two games out of step";
                    ShowStatus("netplay: " + s.endReason + "; the game closes", true);
                }
                else
                {
                    LOG_WARN("Netplay: the other player left");
                    s.endReason = "the other player left";
                }
                s.state = State::kEnded;
                return;
            }
            default:
                return;
            }
        }

        void ReceiveAll()
        {
            std::vector<Inbox::Packet> packets;
            {
                std::lock_guard lock(g_inbox.mutex);
                packets.swap(g_inbox.packets);
            }
            for (const Inbox::Packet& packet : packets)
                Receive(packet.data.data(), int(packet.data.size()), packet.from, packet.arrival);
        }

        // Waits up to a millisecond for a packet, and takes in what came.
        void WaitForPackets()
        {
            WaitForSingleObject(g_inbox.arrived, 1);
            ReceiveAll();
        }

        // The joining machine's hellos, until the host welcomes it, once this machine knows its identity (the disc
        // image's hash).
        void SendHelloIfDue()
        {
            Session& s = g_session;
            if (s.state != State::kConnecting || s.host || Clock::now() - s.lastHello < kHelloInterval || !LocalIdentity())
                return;
            Hello hello;
            FillHeader(hello.header, Type::kHello);
            hello.delay = uint16_t(s.delay);
            hello.identity = s.identity;
            Send(&hello, sizeof(hello));
            s.lastHello = Clock::now();
        }

        // The other machine stopped answering: gone.
        void CheckSilence(Clock::time_point now)
        {
            Session& s = g_session;
            if (s.state == State::kConnected && now - s.lastArrival >= kSilenceLimit)
            {
                char why[96];
                snprintf(why, sizeof(why), "nothing came from the other player's game for %.0f s", SecondsBetween(s.lastArrival, now));
                End(why);
            }
        }

        // From NETPLAY: hosting, or joining an address.
        void Start()
        {
            Session& s = g_session;
            const std::string setting = std::getenv(GAME_PREFIX "_NETPLAY");
            // The input delays: the host chooses each segment's unless its variables fix them (Choose).
            if (const char* delay = std::getenv(GAME_PREFIX "_NETPLAY_DELAY"); delay && *delay)
            {
                s.delay = std::clamp<uint32_t>(uint32_t(strtoul(delay, nullptr, 10)), 0, kMaxDelay);
                s.lockstepFixed = true;
            }
            s.rollback = RollbackSetting();
            if (const char* delay = std::getenv(GAME_PREFIX "_NETPLAY_FIGHT_DELAY"); delay && *delay)
            {
                s.fightDelay = std::clamp<uint32_t>(uint32_t(strtoul(delay, nullptr, 10)), 0, kMaxDelay);
                s.rollbackFixed = true;
            }
            if (const char* window = std::getenv(GAME_PREFIX "_NETPLAY_WINDOW"))
                s.window = std::clamp<uint32_t>(uint32_t(strtoul(window, nullptr, 10)), 1, kMaxWindow);
            s.state = State::kEnded;
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                LOG_ERROR("Netplay: Winsock didn't start");
                s.endReason = "the network didn't start";
                return;
            }
            s.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            // A hello sent before the host listens would come back as an error on the next receive.
            BOOL reportReset = FALSE;
            DWORD returned = 0;
            WSAIoctl(s.socket, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned, nullptr, nullptr);
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            if (setting.rfind("host:", 0) == 0)
            {
                s.host = true;
                local.sin_port = htons(uint16_t(strtoul(setting.c_str() + 5, nullptr, 10)));
                std::random_device random;
                s.id = random() | 1;
                s.seed = random();
                if (const char* seed = std::getenv(GAME_PREFIX "_FIGHT_SEED"))
                    s.seed = uint32_t(strtoul(seed, nullptr, 0));
            }
            else if (setting.rfind("join:", 0) == 0)
            {
                const std::string target = setting.substr(5);
                const size_t colon = target.rfind(':');
                s.peer.sin_family = AF_INET;
                s.peer.sin_port = htons(uint16_t(strtoul(target.c_str() + colon + 1, nullptr, 10)));
                if (colon == std::string::npos || inet_pton(AF_INET, target.substr(0, colon).c_str(), &s.peer.sin_addr) != 1)
                {
                    LOG_ERROR("Netplay: NETPLAY=join:<address>:<port> needs an IPv4 address, not \"%s\"", target.c_str());
                    s.endReason = "NETPLAY's address isn't right";
                    return;
                }
                s.peerName = Describe(s.peer);
            }
            else
            {
                LOG_ERROR("Netplay: NETPLAY is host:<port> or join:<address>:<port>, not \"%s\"", setting.c_str());
                s.endReason = "NETPLAY isn't right";
                return;
            }
            if (bind(s.socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
            {
                LOG_ERROR("Netplay: can't use UDP port %u (error %d)", ntohs(local.sin_port), WSAGetLastError());
                s.endReason = "UDP port " + std::to_string(ntohs(local.sin_port)) + " is in use";
                return;
            }
            g_inbox.arrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            std::thread(ReceiveThread).detach();
            std::thread(AliveThread).detach();
            StartConditions(s.host);
            // The handshake needs the disc image's hash, which may take a few seconds the first time.
            identity::StartDiscHash();
            LocalIdentity();
            s.state = State::kConnecting;
            if (s.host)
                LOG_INFO("Netplay: hosting on UDP port %u as player 1, waiting for player 2%s", ntohs(local.sin_port), RollbackText().c_str());
            else
                LOG_INFO("Netplay: joining %s as player 2%s", s.peerName.c_str(), RollbackText().c_str());
        }

        // A fight's rollback statistics (Rollback), for the log.
        std::string RollbackSummary(double seconds)
        {
            const Session& s = g_session;
            const Rollback& rb = s.rb;
            std::string depths;
            for (uint32_t depth = 1; depth <= std::min<uint32_t>(rb.deepest, kMaxWindow + 1); depth++)
            {
                if (!rb.depths[depth])
                    continue;
                char part[32];
                snprintf(part, sizeof(part), "%s%u: %llu", depths.empty() ? "" : ", ", depth, (unsigned long long)rb.depths[depth]);
                depths += part;
            }
            // A replay's first frame waits out the vblank the present frame would have waited for (D3D lets the CPU run a
            // frame ahead); the frames after it run back to back.
            const uint64_t later = rb.framesReplayed > rb.rollbacks ? rb.framesReplayed - rb.rollbacks : 0;
            char text[1024];
            snprintf(text, sizeof(text),
                "rollback: %llu rollbacks (%.2f a second, %llu during a replay), %llu frames replayed (%.2f a rollback, at most %u; by frames "
                "back: %s), loads %.0f us on average (at most %.0f), replays %.2f ms from the load to the present (the first frame, which "
                "waits out a vblank, %.2f ms; then %.2f ms a frame); the frames ran %.2f frames ahead of the other's "
                "input on average (at most %u), %llu waits at the window of %u frames (%.1f ms in all, %.1f ms at most), %llu in the fight's "
                "menus (%.1f ms in all); the frame advantage %.1f ms on average (%.1f to %.1f), %llu time sync corrections (%.1f ms slowed down)",
                (unsigned long long)rb.rollbacks, seconds > 0 ? double(rb.rollbacks) / seconds : 0.0, (unsigned long long)rb.nested,
                (unsigned long long)rb.framesReplayed, rb.rollbacks ? double(rb.framesReplayed) / double(rb.rollbacks) : 0.0, rb.deepest,
                depths.empty() ? "none" : depths.c_str(), rb.rollbacks ? rb.loadMicroseconds / double(rb.rollbacks) : 0.0, rb.longestLoadMicroseconds,
                rb.rollbacks ? rb.replayMilliseconds / double(rb.rollbacks) : 0.0, rb.rollbacks ? rb.firstFrameMilliseconds / double(rb.rollbacks) : 0.0,
                later ? (rb.replayMilliseconds - rb.firstFrameMilliseconds) / double(later) : 0.0,
                rb.aheadFrames ? double(rb.aheadSum) / double(rb.aheadFrames) : 0.0, rb.furthestAhead, (unsigned long long)rb.windowWaits, s.window,
                rb.windowWaitMicroseconds / 1000.0, rb.longestWindowWait / 1000.0, (unsigned long long)rb.menuWaits, rb.menuWaitMicroseconds / 1000.0,
                rb.advantageSamples ? rb.advantageSum / double(rb.advantageSamples) : 0.0, rb.advantageLow, rb.advantageHigh,
                (unsigned long long)rb.corrections, rb.correctedMilliseconds);
            std::string result = text;
            // Before each load, the wait for the command processor to draw the frames recorded (savestates.h).
            snprintf(text, sizeof(text), "; the loads waited for the command processor's draws %.0f us on average (at most %.0f)",
                rb.rollbacks ? rb.drawWaitMicroseconds / double(rb.rollbacks) : 0.0, rb.longestDrawWaitMicroseconds);
            return result + text;
        }

        // What a segment's statistics end with: both machines' measures, and the two clocks.
        std::string PaceSummary()
        {
            const Session& s = g_session;
            std::string text = "; this machine's measures: " + MeasuresText(LocalMeasures());
            if (s.peerMeasuresKnown)
                text += "; the other's: " + MeasuresText(s.peerMeasures);
            double seconds = 0, error = 0;
            if (const std::optional<double> ppm = DriftPpm(seconds, error))
            {
                char drift[128];
                snprintf(drift, sizeof(drift), "; the other machine's clock %+.1f ppm (+-%.1f) against this one's (over %.0f s)", *ppm, error, seconds);
                text += drift;
            }
            return text;
        }

        // The fight's statistics, for the log.
        std::string Summary()
        {
            const Session& s = g_session;
            const auto& f = s.statistics;
            const auto milliseconds = [](double value) { return value / 1000.0; };
            const uint32_t delay = s.step.delay;
            const bool rollback = s.rollback && s.step.fight;
            char text[2560];
            if (s.lastFightFrame <= delay || f.settled == Clock::time_point{})
            {
                snprintf(text, sizeof(text), "%llu checksums compared, %s", (unsigned long long)f.compared, s.desync ? "a desync" : "the same");
                return text;
            }
            const uint32_t frames = s.lastFightFrame - delay;
            const double seconds = std::chrono::duration<double>(f.lastBoundary - f.settled).count();
            const uint64_t vblanks = gpu::GetStatistics().vblanks.load() - f.vblanks, swaps = gpu::GetStatistics().swaps.load() - f.swaps;
            // Frames shown: the swaps the command processor presented, not those of frames replayed (discarded).
            const uint64_t shown = swaps - std::min(swaps, gpu::GetDiscardStatistics().frames - f.discarded);
            const int32_t localSlack = LocalSlack();
            char slack[160] = "";
            if (localSlack != kNoSlack && s.remoteSlack != kNoSlack)
                snprintf(slack, sizeof(slack), "the other player's inputs came %.1f ms before they were needed on average (this player's %.1f ms); ",
                    milliseconds(localSlack), milliseconds(s.remoteSlack));
            char roundTrip[200] = "";
            if (f.roundTrips)
            {
                // A machine sends its input for frame f + delay as frame f starts, and the other needs it as frame f + delay
                // starts, after its own frame's work: the lockstep keeps 60 frames a second while half the round trip and
                // that work (about 2 ms on a fast PC) fit in delay + 1 frames (ANALYSIS.md, Phase 6). With rollback the
                // other's input comes that many frames, less the delay, after the frame that needed it ran on a
                // prediction, and a rollback replays as many.
                // (The frames' work and what fits as the host's choice reckons them: Choose.)
                const double average = double(f.roundTripMicroseconds) / f.roundTrips;
                const Measures local = LocalMeasures();
                const int late = int(Late(average, FramesWork(local)));
                FitsFrom from = FitsFrom::kDefault;
                const int fits = int(std::min(Fits(local, from), Fits(s.peerMeasuresKnown ? s.peerMeasures : Measures{}, from)));
                if (rollback)
                    snprintf(roundTrip, sizeof(roundTrip), "the round trip %.1f ms (%.1f-%.1f), for which rollbacks of at most %d frames need a "
                        "delay of %d frames or more; ", milliseconds(average), milliseconds(double(f.shortestRoundTrip)),
                        milliseconds(double(f.longestRoundTrip)), fits, std::max(0, late - fits));
                else
                    snprintf(roundTrip, sizeof(roundTrip), "the round trip %.1f ms (%.1f-%.1f), for which 60 frames a second needs a delay of %d "
                        "frames or more; ", milliseconds(average), milliseconds(double(f.shortestRoundTrip)),
                        milliseconds(double(f.longestRoundTrip)), std::max(0, late - 1));
            }
            // Frame boundaries that waited for the game's threads to block in their waits (frame.h).
            char threads[160] = "";
            const ThreadWaits waits = GameThreadWaits();
            if (const uint64_t boundaries = waits.boundaries - f.threadWaits.boundaries)
                snprintf(threads, sizeof(threads), "%llu boundaries waited for the game's threads (%.2f ms in all, %.2f ms at most over the "
                    "session); ", (unsigned long long)boundaries, milliseconds(double(waits.microseconds - f.threadWaits.microseconds)),
                    milliseconds(double(waits.longest)));
            // The waits for the other's input after the first: with rollback, those at the window and in the fight's menus.
            const Rollback& rb = s.rb;
            const uint64_t waitCount = rollback ? rb.windowWaits + rb.menuWaits : f.waits;
            const uint64_t waitLongest = rollback ? std::max({ rb.longestWindowWait, rb.longestMenuWait, f.longestWaitMicroseconds })
                                                  : f.longestWaitMicroseconds;
            const uint64_t waitAll = rollback ? rb.windowWaitMicroseconds + rb.menuWaitMicroseconds : f.waitedMicroseconds;
            snprintf(text, sizeof(text),
                "%u frames in %.2f s from its frame %u (%.2f a second), %llu late (the longest %.1f ms), %llu of %llu vblanks without a "
                "new frame; %.1f ms waited at frame %u for the other player, then %llu waits for its input (%.1f ms at most, %.1f ms in "
                "all); %s%s%s%llu checksums compared, %s; %llu packets sent (%llu resent, %llu dropped), %llu received; %.0f bytes a "
                "packet sent%s%s",
                frames, seconds, delay, seconds > 0 ? frames / seconds : 0.0, (unsigned long long)f.late,
                milliseconds(double(f.longestFrameMicroseconds)), (unsigned long long)(vblanks > shown ? vblanks - shown : 0),
                (unsigned long long)vblanks, milliseconds(double(f.startWaitMicroseconds)), delay, (unsigned long long)waitCount,
                milliseconds(double(waitLongest)), milliseconds(double(waitAll)), slack, roundTrip, threads,
                (unsigned long long)f.compared, s.desync ? "a desync" : "the same", (unsigned long long)f.packetsSent,
                (unsigned long long)f.resent, (unsigned long long)f.dropped, (unsigned long long)f.packetsReceived,
                f.packetsSent ? double(f.bytesSent) / double(f.packetsSent) : 0.0, rollback ? "; " : "",
                rollback ? RollbackSummary(seconds).c_str() : "");
            return text + PaceSummary();
        }

        // A segment of the flow starts (its first frame past loading): the other player's inputs and checksums (and
        // settings) that came early, and for a fight, its setup here. The first as this machine enters the flow takes its
        // player's settings, which its packets carry until it leaves the flow.
        void StartSegment(const Segment& segment, uint64_t frame)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            step = Lockstep{};
            step.active = true;
            step.ordinal = s.segmentsStarted++;
            step.key = segment.fight ? kFightKey | step.ordinal : step.ordinal;
            step.screen = segment.fight ? 0 : segment.screen;
            step.number = segment.number;
            step.fight = segment.fight;
            step.startFrame = frame;
            // Its input delay, the host's choice, once connected (ResolveDelay).
            step.delay = 0;
            step.delayKnown = false;
            if (!s.flow)
            {
                s.flow = true;
                s.flowFirstKey = step.key;
                s.flowEntered = frame;
                // The two clocks from here, the boot's load left out (it slows the threads that send and receive).
                if (s.flowsEntered++ == 0)
                {
                    s.clockKnown = false;
                    s.clockBucket = -1;
                    s.clockPoints.clear();
                }
                s.localSettings = LocalPlayerSettings();
                s.remoteSettings = Settings{};
                s.settingsApplied = false;
                LOG_INFO("Netplay: this machine enters the multiplayer flow at %s, game frame %llu; its player's settings: %s",
                    Named(step.key, step.screen).c_str(), (unsigned long long)frame, DescribeSettings(s.localSettings).c_str());
            }
            if (const auto early = s.early.find(step.key); early != s.early.end())
            {
                step.remote = std::move(early->second.inputs);
                step.remoteChecksums = std::move(early->second.checksums);
                step.remoteScreenKnown = early->second.screenKnown;
                step.remoteScreen = early->second.screen;
                if (early->second.settings.known && !s.remoteSettings.known)
                    s.remoteSettings = early->second.settings;
                if (early->second.delayKnown && !s.host)
                {
                    step.peerDelayKnown = true;
                    step.peerDelay = early->second.delay;
                }
                s.early.erase(early);
            }
            // Packets of earlier segments won't be needed.
            for (auto it = s.early.begin(); it != s.early.end();)
                it = Ordinal(it->first) < step.ordinal ? s.early.erase(it) : std::next(it);
            g_where.key.store(step.key);
            g_where.screen.store(step.screen);
            g_where.frame.store(0);
            if (!step.fight)
            {
                CompareScreens(); // (frame.cpp logs the screen's start)
                return;
            }
            s.fight = s.fightsStarted++;
            s.desync = false;
            s.slacks.clear();
            s.remoteSlack = kNoSlack;
            s.statistics = Statistics{};
            s.rb = Rollback{};
            s.statistics.threadWaits = GameThreadWaits();
            for (auto it = s.remoteSetups.begin(); it != s.remoteSetups.end();)
                it = Ordinal(it->first) < step.ordinal ? s.remoteSetups.erase(it) : std::next(it);
            s.localSetup = Digest(FightSetup(), s.setupNames);
            s.localSetupKnown = s.localSetup.count != 0;
            s.peerHasSetup = s.setupCompared = s.setupsDiffer = false;
            if (!s.settingsApplied)
                LOG_ERROR("Netplay: fight %u starts before both players' settings were applied", s.fight);
            LOG_INFO("Netplay: fight %u: its first frame at game frame %llu", s.fight, (unsigned long long)frame);
            CompareSetups();
        }

        // The host, at the flow's first segment: a few round trips measured before it chooses the segment's delays (from
        // the handshake on, the alive packets measure it; a host that waited for the other to join has just connected),
        // at most kFirstRoundTripWait.
        void WaitForRoundTrips()
        {
            Session& s = g_session;
            const Clock::time_point start = Clock::now();
            if (s.roundTripCount >= kFirstRoundTrips)
                return;
            SetWhere(Where::kWaiting);
            while (s.roundTripCount < kFirstRoundTrips && s.state == State::kConnected && Clock::now() - start < kFirstRoundTripWait)
            {
                WaitForPackets();
                CheckSilence(Clock::now());
                UpdateStatus(false, start);
            }
            SetWhere(Where::kInSegment);
            LOG_INFO("Netplay: %s: %llu round trips measured, after %.1f s, for its input delays", Named(s.step.key, s.step.screen).c_str(),
                (unsigned long long)s.roundTripCount, SecondsBetween(start, Clock::now()));
        }

        // The joining machine at a segment's first frame, before it knows the host's choice of its delays (the host chooses
        // them as it ends the segment before, and its packets carry them): waits for them. The host's packets of the
        // segment say its delay too. As long as for the other's input at most (WaitForInput). False if the session ended.
        bool WaitForDelays()
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            const Clock::time_point start = Clock::now();
            Clock::time_point nextNotice = start + kWaitNotice, stuckSince = start;
            SetWhere(Where::kWaiting);
            while (!s.delays.count(step.ordinal) && !step.peerDelayKnown && s.state == State::kConnected)
            {
                WaitForPackets();
                const Clock::time_point now = Clock::now();
                CheckSilence(now);
                UpdateStatus(true, start);
                if (now >= nextNotice)
                {
                    nextNotice = now + kNoticeInterval;
                    LOG_INFO("Netplay: %s, frame 0: waiting %.0f s for the host's input delays for it; %s", Named(step.key, step.screen).c_str(),
                        SecondsBetween(start, now), PeerWhere().c_str());
                }
                // While the other machine's game loads, or its player plays on their own, it's on its way.
                const bool fresh = s.peerWhereKnown && now - s.peerWhereArrival < kAliveInterval * 4;
                if (fresh && (Where(s.peerWhere.where) == Where::kOwnTime || Where(s.peerWhere.where) == Where::kBetween))
                    stuckSince = now;
                if (now - stuckSince >= kWaitLimit)
                    End("the host's input delays didn't come");
            }
            SetWhere(Where::kInSegment);
            if (const double waited = SecondsBetween(start, Clock::now()); s.state == State::kConnected && waited >= 0.1)
                LOG_INFO("Netplay: %s, frame 0: the host's input delays came after %.1f s", Named(step.key, step.screen).c_str(), waited);
            return s.state == State::kConnected;
        }

        // A segment's input delay, once connected: the host's choice (ChooseDelays: the host chooses the first segment's
        // now, a few round trips measured), which the joining machine may have to wait for; logged with why. False if the
        // session ended meanwhile.
        bool ResolveDelay()
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            if (step.delayKnown)
                return true;
            if (s.host && !s.delays.count(step.ordinal))
            {
                WaitForRoundTrips();
                if (s.state != State::kConnected)
                    return false;
                ChooseDelays(step.ordinal);
            }
            if (!s.host && !s.delays.count(step.ordinal) && !step.peerDelayKnown && !WaitForDelays())
                return false;
            const bool rollbackFight = step.fight && s.rollback;
            const auto chosen = s.delays.find(step.ordinal);
            std::string why;
            if (chosen != s.delays.end())
            {
                step.delay = rollbackFight ? chosen->second.rollback : chosen->second.lockstep;
                why = DelaysText(chosen->second, rollbackFight);
                if (!s.host)
                    why = "the host's choice: " + why;
            }
            else
            {
                step.delay = step.peerDelay;
                why = "the host's, as its packets of it say";
            }
            step.delay = std::min(step.delay, kMaxDelay);
            step.delayKnown = true;
            if (step.fight)
                LOG_INFO("Netplay: fight %u (%s): input delay %u frames%s, %s", s.fight, Named(step.key, step.screen).c_str(), step.delay,
                    rollbackFight ? ", rolling back" : ", in lockstep", why.c_str());
            else
                LOG_INFO("Netplay: %s: input delay %u frames, %s", Named(step.key, step.screen).c_str(), step.delay, why.c_str());
            return true;
        }

        // Both players' settings for the flow, once both are known (at the flow's first frame that needs the other's
        // input, whose packets carry them): the host's as player 1's, the other's as player 2's, on both machines.
        void ApplySettings()
        {
            Session& s = g_session;
            const GameState* game = GetGameState();
            if (s.settingsApplied)
                return;
            if (!game || !game->playerSettings.apply || game->playerSettings.size == 0)
            {
                s.settingsApplied = true; // a game without any
                return;
            }
            if (!s.localSettings.known || !s.remoteSettings.known)
                return;
            const Settings& host = s.host ? s.localSettings : s.remoteSettings;
            const Settings& joining = s.host ? s.remoteSettings : s.localSettings;
            game->playerSettings.apply(host.bytes, joining.bytes);
            s.settingsApplied = true;
            LOG_INFO("Netplay: the flow's settings: player 1's (the host's) %s; player 2's %s; the host's rules apply",
                DescribeSettings(host).c_str(), DescribeSettings(joining).c_str());
        }

        // This machine leaves the multiplayer flow (its game is on its own again): the game's own settings again.
        void LeaveFlow(uint64_t frame)
        {
            Session& s = g_session;
            if (!s.flow)
                return;
            s.flow = false;
            const GameState* game = GetGameState();
            if (s.settingsApplied && game && game->playerSettings.apply)
                game->playerSettings.apply(nullptr, nullptr);
            s.settingsApplied = false;
            LOG_INFO("Netplay: this machine leaves the multiplayer flow at game frame %llu, after %.0f s in it: its menus are its own again",
                (unsigned long long)frame, double(frame - s.flowEntered) / 60.0);
        }

        // The slack (LocalSlack) of a frame of a fight that rolls back, once both the other's input for it has come and this
        // machine has got to the frame: from the frame after the other machine's first inputs it sent from the fight (as
        // LockstepFrame's), how long before this machine got to the frame its input came (negative: after).
        void NoteSlack(uint32_t frame, int64_t microseconds)
        {
            Session& s = g_session;
            if (frame <= 2 * s.step.delay || (s.rollback && s.step.fight && frame < s.rb.slackFrom))
                return;
            s.slacks.push_back(microseconds);
            if (s.slacks.size() > kSlackFrames)
                s.slacks.pop_front();
        }

        void NoteArrival(uint32_t frame, Clock::time_point arrival)
        {
            Session& s = g_session;
            if (!s.rollback || !s.step.fight || !s.rb.on)
                return;
            // A frame this machine got to already: the input came late, after the frame ran on a prediction.
            if (const auto reached = s.rb.reached.find(frame); reached != s.rb.reached.end())
                NoteSlack(frame, MicrosecondsBetween(arrival, reached->second));
        }

        // Whether the frames of a fight that rolls back from the first not known to be final up to `upTo` all ran with the
        // other player's input as it came (it has come for each, and is the one the frame ran with).
        bool RanAsItCame(uint32_t upTo)
        {
            const Session& s = g_session;
            for (int64_t g = s.rb.verified + 1; g <= int64_t(upTo); g++)
            {
                const auto used = s.rb.used.find(uint32_t(g));
                const auto real = s.step.remote.find(uint32_t(g));
                if (used == s.rb.used.end() || real == s.step.remote.end() || !(used->second == real->second.pad))
                    return false;
            }
            return true;
        }

        // A fight that rolls back ends, or the session does in it: a replay under way ends where it is (the frames from
        // here are the present's), and the journal and the savestates stop.
        void StopRollback(uint64_t frame)
        {
            Session& s = g_session;
            Rollback& rb = s.rb;
            if (!rb.on)
                return;
            const bool replaying = rb.replaying;
            if (rb.replaying)
            {
                replay::End();
                rb.replaying = false;
            }
            journal::Stop();
            if (rb.ownSavestates)
                savestates::Stop();
            rb.on = false;
            // A fight ends in a frame that ran with the other's input as it came, through its menus (GameState::fightMenu),
            // so at the same frame on both machines. The session over, it may end anywhere.
            if (s.state != State::kEnded && rb.lastRun >= 0 && !RanAsItCame(uint32_t(rb.lastRun)))
                LOG_WARN("Netplay: fight %u ended at game frame %llu, after its frame %lld, which ran on a prediction of the other player's input "
                         "or after one%s (the inputs final to fight frame %lld): the two games may go separate ways", s.fight,
                    (unsigned long long)frame, (long long)rb.lastRun, replaying ? ", in a replay" : "", (long long)rb.verified);
        }

        // A segment ends: its statistics in the log, and what this machine sent of it kept, to answer the other machine.
        void EndSegment(uint64_t frame)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            if (!step.active)
                return;
            if (step.fight)
            {
                StopRollback(frame);
                LOG_INFO("Netplay: fight %u ended at its frame %u: %s", s.fight, s.lastFightFrame, Summary().c_str());
                // Its rollbacks' measures join the session's (Measures).
                Rollback& rb = s.rb;
                s.replayMicroseconds += rb.laterMilliseconds * 1000.0;
                s.replayFrames += rb.laterFrames;
                s.loadMicroseconds += rb.loadMicroseconds;
                s.drawWaitMicroseconds += rb.drawWaitMicroseconds;
                s.loads += rb.rollbacks;
                rb = Rollback{};
            }
            else
            {
                // Its frame rate from its frame `delay` on, as a fight's.
                char rate[192] = "";
                if (step.settled != Clock::time_point{} && step.lastFrame > step.delay)
                {
                    const uint32_t frames = step.lastFrame - step.delay;
                    const double seconds = std::chrono::duration<double>(step.lastBoundary - step.settled).count();
                    const uint64_t vblanks = gpu::GetStatistics().vblanks.load() - step.vblanks, swaps = gpu::GetStatistics().swaps.load() - step.swaps;
                    snprintf(rate, sizeof(rate), "%u frames in %.2f s from its frame %u (%.2f a second), %llu of %llu vblanks without a new frame; ",
                        frames, seconds, step.delay, seconds > 0 ? frames / seconds : 0.0, (unsigned long long)(vblanks > swaps ? vblanks - swaps : 0),
                        (unsigned long long)vblanks);
                }
                char roundTrip[64] = "";
                if (step.roundTrips)
                    snprintf(roundTrip, sizeof(roundTrip), "the round trip %.1f ms; ", double(step.roundTripMicroseconds) / double(step.roundTrips) / 1000.0);
                LOG_INFO("Netplay: %s ended at its frame %u (game frame %llu): %s%.1f ms waited at frame %u for the other player, then %llu "
                         "waits for its input (%.1f ms at most, %.1f ms in all); %s%llu checksums compared, %s%s", Named(step.key, step.screen).c_str(),
                    step.lastFrame, (unsigned long long)frame - 1, rate, step.startWaitMicroseconds / 1000.0, step.delay, (unsigned long long)step.waits,
                    step.longestWaitMicroseconds / 1000.0, step.waitedMicroseconds / 1000.0, roundTrip, (unsigned long long)step.compared,
                    step.differs || step.apart ? "the menus differed" : "the same", PaceSummary().c_str());
            }
            // The host chooses the next segment's delays now: the joining machine hears of them while both load it.
            if (s.host && step.delayKnown && s.state == State::kConnected)
                ChooseDelays(step.ordinal + 1);
            EndedSegment ended;
            ended.key = step.key;
            ended.screen = step.screen;
            ended.delay = step.delay;
            ended.peerReceived = step.peerReceived;
            ended.local = std::move(step.local);
            ended.checksums = std::move(step.localChecksums);
            s.ended.push_back(std::move(ended));
            if (s.ended.size() > kEndedKept)
                s.ended.pop_front();
            s.endedAt = Clock::now();
            if (step.fight)
            {
                s.localSetupKnown = false;
                for (auto it = s.remoteSetups.begin(); it != s.remoteSetups.end();)
                    it = Ordinal(it->first) <= step.ordinal ? s.remoteSetups.erase(it) : std::next(it);
            }
            step.active = false;
            step.local.clear();
            step.remote.clear();
            step.localChecksums.clear();
            step.remoteChecksums.clear();
        }

        // This frame's pads for both game users, with packet numbers that change whenever a pad does.
        void SetPads(const Pad& user0, const Pad& user1, uint32_t packets0, uint32_t packets1)
        {
            Session& s = g_session;
            s.pads[0] = user0;
            s.pads[1] = user1;
            s.packets[0] = packets0;
            s.packets[1] = packets1;
            s.padsSet = true;
        }

        // A pad's packet number on its own (each machine's own time, or the session over): changed when the pad does.
        uint32_t OwnPacket(int user, const Pad& pad)
        {
            Session& s = g_session;
            if (!(pad == s.lastPads[user]))
                s.packets[user]++;
            s.lastPads[user] = pad;
            return s.packets[user];
        }

        // After the session: this machine's player goes on alone, the other's controller at rest; in the flow as the
        // player it was there (the host player 1, the other player 2), outside it as player 1. A fight of the flow under
        // way is given to this machine's player (GameState::abandonFight), so it ends the way the game's flow goes. The
        // player is told why in a notice on screen (and the window's title), and kCloseDelay later the game closes.
        void Alone(const std::optional<Segment>& segment)
        {
            Session& s = g_session;
            const bool flow = netplay::InFlow(); // the game's (frame.h)
            const int localUser = flow && !s.host ? 1 : 0;
            const GameState* game = GetGameState();
            const bool inFight = segment && segment->fight && segment->flow;
            if (!s.aloneHandled)
            {
                s.aloneHandled = true;
                s.closeAt = Clock::now() + kCloseDelay;
                LOG_INFO("Netplay: the session is over (%s): this machine's player goes on alone until the game closes, in %lld s",
                    s.endReason.c_str(), (long long)std::chrono::duration_cast<std::chrono::seconds>(kCloseDelay).count());
                g_where.where.store(uint8_t(Where::kOwnTime));
                // The fight under way, or loading, as the session ended: the other player left it.
                if (inFight)
                    s.fightLeft = int64_t(segment->number);
                else if (flow && game && game->fightState && game->fightState() == GameState::FightState::kLoading)
                    s.fightLeft = s.fightSeen ? int64_t(s.lastFightNumber) + 1 : 0;
                // On screen too: the window's title isn't seen in fullscreen.
                ui::notices::Notice notice;
                notice.heading = "Netplay: the game closes in " +
                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kCloseDelay).count()) + " seconds";
                notice.text = s.endReason;
                if (!notice.text.empty())
                    notice.text[0] = char(toupper(uint8_t(notice.text[0])));
                notice.frames = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(kCloseDelay).count() * 60 / 1000);
                ui::notices::Show(std::move(notice));
            }
            ShowStatus("netplay: " + s.endReason + "; the game closes", true);
            // The session was what the game ran for (it went straight into Versus): once the player has been told, it
            // closes, as the player would close it (main.cpp: the settings saved, the process ended). Any save the game
            // was writing has finished: a session's frame boundary waits for its asynchronous work (frame.h).
            if (!s.closing && Clock::now() >= s.closeAt)
            {
                s.closing = true;
                LOG_INFO("Netplay: the game closes, the session being over");
                if (!ui::CloseGameWindow())
                    LOG_WARN("Netplay: no window to close (HEADLESS): the game goes on");
            }
            if (inFight && int64_t(segment->number) == s.fightLeft && game && game->abandonFight && BoundaryContext() &&
                game->abandonFight(*BoundaryContext(), guest::g_base, uint32_t(localUser)))
            {
                s.fightLeft = -1;
                LOG_INFO("Netplay: fight %u given to this machine's player", s.fight);
            }
            Pad local{};
            if (s.sampleLocal)
                s.sampleLocal(local);
            Pad pads[2]{};
            pads[localUser] = local;
            SetPads(pads[0], pads[1], OwnPacket(0, pads[0]), OwnPacket(1, pads[1]));
        }

        // The window title's status in a session.
        void UpdateStatus(bool waiting, Clock::time_point waitStart)
        {
            Session& s = g_session;
            char text[160];
            const GameState* game = GetGameState();
            const char* flowName = game && game->flowName ? game->flowName : "the multiplayer mode";
            // The other machine's player plays on their own, outside the flow, where this one waits for them (the sync-in).
            const bool syncingIn = waiting && s.peerWhereKnown && Where(s.peerWhere.where) == Where::kOwnTime;
            if (s.state == State::kConnecting)
            {
                if (s.host)
                    snprintf(text, sizeof(text), "netplay: waiting for player 2 to join");
                else
                    snprintf(text, sizeof(text), "netplay: joining %s", s.peerName.c_str());
            }
            else if (syncingIn)
                snprintf(text, sizeof(text), "netplay: waiting for the other player to enter %s (%.0f s)", flowName, SecondsBetween(waitStart, Clock::now()));
            else if (waiting && Clock::now() - waitStart >= kWaitNotice)
                snprintf(text, sizeof(text), "netplay: waiting for the other player (%.0f s)", SecondsBetween(waitStart, Clock::now()));
            else
                snprintf(text, sizeof(text), "netplay: player %d with %s, round trip %.0f ms", s.host ? 1 : 2, s.peerName.c_str(),
                    s.roundTripMicroseconds / 1000.0);
            ShowStatus(text);
        }

        // The other player's input for the current segment's frame: waits for it, sending this machine's again while it
        // waits. Ends the session if the other machine stops answering, or answers without moving on. Returns how long it
        // waited (microseconds); with `counted`, a lockstep frame's wait, in the segment's statistics.
        uint64_t WaitForInput(uint32_t frame, Clock::time_point boundary, bool counted = true)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            bool noticed = false, syncIn = false;
            Clock::time_point nextNotice = boundary + kWaitNotice;
            Clock::time_point apartSince{}, stuckSince = boundary;
            SetWhere(Where::kWaiting);
            while (frame >= step.delay && !step.remote.count(frame) && s.state != State::kEnded)
            {
                WaitForPackets();
                SendHelloIfDue();
                const Clock::time_point now = Clock::now();
                if (now - s.lastSent >= kResendInterval)
                {
                    SendSegment();
                    if (step.fight)
                        s.statistics.resent++;
                }
                // The other player plays on their own: this machine entered the flow first, and waits for them (the
                // sync-in).
                const bool fresh = s.peerWhereKnown && now - s.peerWhereArrival < kAliveInterval * 4;
                if (!syncIn && s.state == State::kConnected && fresh && Where(s.peerWhere.where) == Where::kOwnTime)
                {
                    syncIn = noticed = true;
                    LOG_INFO("Netplay: %s, frame %u: waiting for the other player to enter the multiplayer flow; %s", Named(step.key, step.screen).c_str(),
                        frame, PeerWhere().c_str());
                }
                if (now >= nextNotice)
                {
                    noticed = true;
                    nextNotice = now + kNoticeInterval;
                    if (s.state == State::kConnecting)
                        LOG_INFO("Netplay: %s, frame %u: %s", Named(step.key, step.screen).c_str(), frame,
                            s.host ? "waiting for player 2 to join" : "waiting for the host to answer");
                    else if (syncIn)
                        LOG_INFO("Netplay: %s, frame %u: waiting for the other player to enter the multiplayer flow %.0f s; %s",
                            Named(step.key, step.screen).c_str(), frame, SecondsBetween(boundary, now), PeerWhere().c_str());
                    else
                        LOG_WARN("Netplay: %s, frame %u: waiting for the other player's input %.0f s; %s", Named(step.key, step.screen).c_str(),
                            frame, SecondsBetween(boundary, now), PeerWhere().c_str());
                }
                UpdateStatus(true, boundary);
                if (s.state != State::kConnected)
                    continue; // waiting for the other player to join has no limit
                CheckSilence(now);
                // The other machine left this segment before the frame this one is at: they went separate ways.
                const Where peerWhere = Where(s.peerWhere.where);
                if (s.peerWhereKnown && s.peerWhere.key == step.key && (peerWhere == Where::kBetween || peerWhere == Where::kOwnTime) &&
                    s.peerWhere.frame < frame)
                {
                    char why[192];
                    snprintf(why, sizeof(why), "the games went out of step: the other machine left %s at its frame %u, and this one is at frame %u",
                        Named(step.key, step.screen).c_str(), s.peerWhere.frame, frame);
                    Apart(why);
                    break;
                }
                // Both machines waiting for each other in segments that aren't the same: they went separate ways.
                const bool apart = fresh && peerWhere == Where::kWaiting && s.peerWhere.key != step.key;
                if (!apart)
                    apartSince = Clock::time_point{};
                else if (apartSince == Clock::time_point{})
                    apartSince = now;
                else if (now - apartSince >= kApartLimit)
                {
                    char why[192];
                    snprintf(why, sizeof(why), "the games went separate ways: this machine waits in %s and the other in %s",
                        Named(step.key, step.screen).c_str(), Named(s.peerWhere.key, s.peerWhere.screen).c_str());
                    Apart(why);
                    break;
                }
                // While the other machine's game loads, or its player plays on their own (outside the flow, or booting),
                // it's on its way: no limit. In a segment, it should come soon.
                const bool onItsWay = fresh && (peerWhere == Where::kOwnTime || peerWhere == Where::kBetween);
                if (onItsWay)
                    stuckSince = now;
                if (now - stuckSince >= kWaitLimit)
                {
                    char why[128];
                    snprintf(why, sizeof(why), "the other player's input stopped coming for %.0f s", SecondsBetween(boundary, now));
                    End(why);
                }
            }
            SetWhere(Where::kInSegment);
            const uint64_t waited = uint64_t(MicrosecondsBetween(boundary, Clock::now()));
            if (counted && step.fight)
            {
                Statistics& f = s.statistics;
                if (frame == step.delay)
                    f.startWaitMicroseconds = waited;
                else
                {
                    f.waits++;
                    f.waitedMicroseconds += waited;
                    f.longestWaitMicroseconds = std::max(f.longestWaitMicroseconds, waited);
                }
            }
            else if (counted && frame == step.delay)
                step.startWaitMicroseconds = waited;
            else if (counted)
            {
                step.waits++;
                step.waitedMicroseconds += waited;
                step.longestWaitMicroseconds = std::max(step.longestWaitMicroseconds, waited);
            }
            if (noticed && s.state != State::kEnded)
                LOG_INFO("Netplay: %s, frame %u: the other player's input came after %.1f s", Named(step.key, step.screen).c_str(), frame,
                    waited / 1e6);
            return waited;
        }

        // Before the other machine is there (the host waits for the other player to join, the joining machine for the
        // host's welcome), a segment plays no frame: they'd need the session's input delay and seed, and the other's input.
        // Each machine gets to its first segment (the title) in its own time and waits there, without a limit.
        void WaitForConnection(const Segment& segment, uint64_t frame)
        {
            Session& s = g_session;
            const Clock::time_point start = Clock::now();
            Clock::time_point nextNotice = start + kWaitNotice;
            SetWhere(Where::kWaiting);
            while (s.state == State::kConnecting)
            {
                WaitForPackets();
                SendHelloIfDue();
                UpdateStatus(false, start);
                const Clock::time_point now = Clock::now();
                if (now >= nextNotice)
                {
                    nextNotice = now + kNoticeInterval;
                    LOG_INFO("Netplay: %s, frame %u (game frame %llu): %s for %.0f s", Named(s.step.key, s.step.screen).c_str(), segment.frame,
                        (unsigned long long)frame, s.host ? "waiting for player 2 to join" : "waiting for the host to answer",
                        SecondsBetween(start, now));
                }
            }
            SetWhere(Where::kInSegment);
            if (s.state == State::kConnected)
                LOG_INFO("Netplay: %s, frame %u: connected after %.1f s", Named(s.step.key, s.step.screen).c_str(), segment.frame,
                    SecondsBetween(start, Clock::now()));
        }

        // DEBUG_NETPLAY_SOUNDS=1 (diagnostics): the sounds the audio engine plays as each frame of the session's first fight
        // runs as the present (with rollback, after any replay; GameState::soundsPlaying), a fight frame and the sounds a
        // line, to dumps/netplay_sounds.txt, as a rollback test's rollback_sounds.txt: tools\netplay\rollbacksounds.py
        // compares two runs' (one that rolled back and one that didn't, with the same fight seed).
        void WriteSounds(uint32_t fightFrame)
        {
            static const bool on = [] {
                const char* setting = std::getenv(GAME_PREFIX "_DEBUG_NETPLAY_SOUNDS");
                return setting && std::atoi(setting) != 0;
            }();
            static FILE* file = nullptr;
            static bool done = false;
            const Session& s = g_session;
            if (!on || done)
                return;
            if (s.fight != 0)
            {
                if (file)
                    fclose(file);
                file = nullptr;
                done = true;
                return;
            }
            const GameState* game = GetGameState();
            if (!game || !game->soundsPlaying)
                return;
            if (!file)
            {
                file = fopen(config::DumpPath("netplay_sounds.txt").c_str(), "w");
                if (!file)
                {
                    done = true;
                    return;
                }
            }
            fprintf(file, "%u", fightFrame);
            for (const uint32_t sound : game->soundsPlaying())
                fprintf(file, " %08X", sound);
            fprintf(file, "\n");
            fflush(file); // a pair test ends its instances by killing them
        }

        // DEBUG_NETPLAY_PREDICTED=1 (diagnostics): the frames of the session's first fight this machine showed on a wrong
        // prediction of the other player's input, [from, to), written as a rollback to `from` finds them (a fight frame a
        // line, to dumps/netplay_predicted.txt, as a rollback test's rollback_predicted.txt): the others show what every
        // machine shows at that frame, so frame dumps (DUMP_FRAMES=F...) compare between machines and runs on those.
        void WritePredicted(uint32_t from, uint32_t to)
        {
            static const bool on = [] {
                const char* setting = std::getenv(GAME_PREFIX "_DEBUG_NETPLAY_PREDICTED");
                return setting && std::atoi(setting) != 0;
            }();
            static FILE* file = nullptr;
            static bool done = false;
            if (!on || done || from >= to)
                return;
            if (g_session.fight != 0)
            {
                if (file)
                    fclose(file);
                file = nullptr;
                done = true;
                return;
            }
            if (!file && !(file = fopen(config::DumpPath("netplay_predicted.txt").c_str(), "w")))
            {
                done = true;
                return;
            }
            for (uint32_t f = from; f < to; f++)
                fprintf(file, "%u\n", f);
            fflush(file);
        }

        // A fight frame run as the present (in lockstep as its boundary starts, with rollback as it runs, after a replay
        // or waits): the frame pacing's statistics.
        void NotePresentFrame(uint32_t f, Clock::time_point when)
        {
            Session& s = g_session;
            Statistics& stats = s.statistics;
            if (stats.lastBoundary != Clock::time_point{} && f > s.step.delay)
            {
                const auto interval = when - stats.lastBoundary;
                stats.longestFrameMicroseconds = std::max<uint64_t>(stats.longestFrameMicroseconds,
                    uint64_t(std::chrono::duration_cast<microseconds>(interval).count()));
                if (interval > kLateFrame)
                    stats.late++;
            }
            stats.lastBoundary = when;
            s.lastFightFrame = f;
        }

        // The pads of a segment's frame `f` for both game users: this player's and the other's.
        void DecidePads(uint32_t f, Pad local, Pad remote)
        {
            Session& s = g_session;
            const Lockstep& step = s.step;
            const GameState* game = GetGameState();
            const int localUser = s.host ? 0 : 1, remoteUser = 1 - localUser;
            Pad pads[2]{};
            pads[localUser] = local;
            pads[remoteUser] = remote;
            // Nobody leaves the flow: what would take the game out of it this frame is out of both players' input, on both
            // machines alike (the game's state at the boundary is the same on both).
            if (game && game->flowBlockedButtons)
            {
                for (int user = 0; user < 2; user++)
                {
                    Pad& pad = pads[user];
                    bool stick = false;
                    const uint16_t blocked = game->flowBlockedButtons(pad.buttons, stick);
                    const uint16_t taken = pad.buttons & blocked;
                    pad.buttons &= uint16_t(~blocked);
                    if (stick)
                        pad.thumbLX = pad.thumbLY = 0;
                    // Logged as a player presses them, not at every frame they're held.
                    if (taken && taken != s.blockedLogged[user])
                        LOG_INFO("Netplay: %s, frame %u: buttons %04X out of player %d's input, which would leave the flow",
                            Named(step.key, step.screen).c_str(), f, taken, user + 1);
                    s.blockedLogged[user] = taken;
                }
            }
            // Packet numbers that change every frame, the same on both machines.
            const uint32_t packet = (step.ordinal << 20) + f + 1;
            SetPads(pads[0], pads[1], packet, packet);
            s.lastPads[0] = pads[0];
            s.lastPads[1] = pads[1];
        }

        // What both machines have: the inputs before the frame both have passed (`keep`: the first this machine may still
        // need, with rollback the first not final), and the checksums 256 frames before `f`.
        void Forget(uint32_t keep, uint32_t f)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            const uint32_t settled = std::min({ step.peerReceived, Received(), keep });
            if (settled > 64)
            {
                step.local.erase(step.local.begin(), step.local.lower_bound(settled - 64));
                step.remote.erase(step.remote.begin(), step.remote.lower_bound(settled - 64));
            }
            if (f > 256)
            {
                step.localChecksums.erase(step.localChecksums.begin(), step.localChecksums.lower_bound(f - 256));
                step.remoteChecksums.erase(step.remoteChecksums.begin(), step.remoteChecksums.lower_bound(f - 256));
            }
        }

        // A segment's frame: the local input, applied `delay` frames on, and the other player's for this frame.
        void LockstepFrame(const Segment& segment, uint64_t frame)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            if (!step.active)
                StartSegment(segment, frame);
            if (s.state == State::kConnecting)
                WaitForConnection(segment, frame);
            if (s.state == State::kEnded || !ResolveDelay())
            {
                Alone(segment);
                return;
            }
            const uint32_t f = segment.frame;
            const uint32_t d = step.delay;
            const Clock::time_point boundary = Clock::now();
            g_where.frame.store(f);
            SetWhere(Where::kInSegment);
            if (step.fight)
                NotePresentFrame(f, boundary);
            // The frame before this one, the segment's too: its work (the input delays' measures).
            if (f >= 1 && step.lastFrame == f - 1)
                NoteFrameWork(step.fight);
            step.lastFrame = f;
            step.lastBoundary = boundary;
            Pad local{};
            if (s.sampleLocal)
                s.sampleLocal(local);
            step.local[f + d] = local;
            const GameState* game = GetGameState();
            // A menu screen's checksum from the frame after its random numbers are seeded (at frame `delay`).
            std::optional<uint64_t> checksum;
            if (step.fight)
                checksum = FightChecksum();
            else if (game && game->menuChecksum && f > d)
                checksum = game->menuChecksum();
            if (checksum)
            {
                // DEBUG_NET_DESYNC: as if this machine's game had gone another way.
                if (Conditions& c = g_conditions; c.desyncPending && Reached(c.desyncWhen, frame, segment))
                {
                    c.desyncPending = false;
                    *checksum ^= 0xDE5E7C;
                    LOG_INFO("Netplay: this machine's checksum made to differ at %s's frame %u", Named(step.key, step.screen).c_str(), f);
                }
                if (step.fight)
                    KeepDesyncState(f);
                step.localChecksums[f] = *checksum;
                CompareChecksum(f);
                // A fight that rolls back runs its first frames in lockstep, their checksums final as they come (and logs
                // them itself: netplay::LogFightChecksum).
                if (step.fight && s.rollback)
                    LogFightChecksum(f, *checksum);
            }
            SendSegment();

            // The other player's input for this frame: wait for it.
            if (f >= d && !step.remote.count(f))
                WaitForInput(f, boundary);
            if (s.state == State::kEnded)
            {
                Alone(segment);
                return;
            }
            // Both players' settings for the flow, which the other's packets carry with its inputs.
            if (f >= d && !s.settingsApplied)
            {
                ApplySettings();
                if (!s.settingsApplied)
                {
                    Apart(!s.localSettings.known ? "this machine's player has no settings for the flow (no profile)"
                                                 : "the other player's settings for the flow didn't come with its input");
                    Alone(segment);
                    return;
                }
            }
            if (f == d)
            {
                if (step.fight)
                {
                    Statistics& stats = s.statistics;
                    stats.settled = stats.lastBoundary = Clock::now();
                    stats.vblanks = gpu::GetStatistics().vblanks.load();
                    stats.swaps = gpu::GetStatistics().swaps.load();
                    stats.discarded = gpu::GetDiscardStatistics().frames;
                }
                else
                {
                    // The menu screen's frame rate from here (EndSegment).
                    step.settled = Clock::now();
                    step.vblanks = gpu::GetStatistics().vblanks.load();
                    step.swaps = gpu::GetStatistics().swaps.load();
                    if (game && game->seedMenus && BoundaryContext())
                    {
                        // Both machines are here now, connected: the menus' random numbers from the session, the same on both.
                        game->seedMenus(*BoundaryContext(), guest::g_base, (uint64_t(s.seed) << 32) ^ (uint64_t(step.key) * 0x9E3779B97F4A7C15ull));
                    }
                }
            }
            const auto remote = step.remote.find(f);
            // The slack, from fight frame 2 * delay + 1 on, whose inputs the other player sent after this one got to it.
            if (step.fight && remote != step.remote.end() && f > 2 * d)
            {
                s.slacks.push_back(MicrosecondsBetween(remote->second.arrival, boundary));
                if (s.slacks.size() > kSlackFrames)
                    s.slacks.pop_front();
            }
            DecidePads(f, f >= d ? step.local[f] : Pad{}, f >= d && remote != step.remote.end() ? remote->second.pad : Pad{});
            if (step.fight)
                WriteSounds(f);
            if (step.fight && f > d && f % kReportInterval == 0)
                LOG_INFO("Netplay: fight %u at its frame %u: %s", s.fight, f, Summary().c_str());
            UpdateStatus(false, boundary);
            Forget(UINT32_MAX, f);
        }

        // A fight that rolls back (session.h): savestates and the journal start at its frame `delay`, which ran in
        // lockstep (LockstepFrame: the first machine there waited for the other).
        void StartRollback(uint64_t frame)
        {
            Session& s = g_session;
            Rollback& rb = s.rb;
            const uint32_t d = s.step.delay;
            if (!savestates::Active())
            {
                // As many boundaries back as the window lets a rollback go, and two to spare.
                savestates::Start(s.window + 2);
                rb.ownSavestates = savestates::Active();
            }
            else
                LOG_WARN("Netplay: fight %u: savestates were saved already (SAVESTATES?): rollbacks need the window's, %u boundaries", s.fight,
                    s.window + 2);
            if (!savestates::Active())
            {
                LOG_ERROR("Netplay: fight %u: no savestates, so it plays in lockstep", s.fight);
                return;
            }
            journal::Record();
            rb.on = true;
            rb.present = rb.lastRun = rb.verified = rb.emitted = int64_t(d);
            rb.vblankDelayAtStart = gpu::VblankDelayGiven();
            LOG_INFO("Netplay: fight %u rolls back from its frame %u (game frame %llu): input delay %u frames, window %u frames", s.fight, d,
                (unsigned long long)frame, d, s.window);
        }

        // The other player's pad for a fight frame that rolls back: the one that came, or the prediction, the last that
        // came (as GGPO predicts: players hold their buttons more often than they change them), or none before any came.
        Pad RemotePad(uint32_t f, bool& known)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            known = false;
            if (f < step.delay)
            {
                known = true;
                return Pad{};
            }
            if (const auto remote = step.remote.find(f); remote != step.remote.end())
            {
                known = true;
                return remote->second.pad;
            }
            const uint32_t received = Received();
            if (received > step.delay)
            {
                if (const auto last = step.remote.find(received - 1); last != step.remote.end())
                    return last->second.pad;
            }
            return Pad{};
        }

        // The pads of the fight frame `f` about to run (a replay's or the present), with the other's input as it came or
        // predicted, which the frame then ran with.
        void RollbackPads(uint32_t f)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            Rollback& rb = s.rb;
            Pad local{};
            if (f >= step.delay)
            {
                if (const auto it = step.local.find(f); it != step.local.end())
                    local = it->second;
                else
                    LOG_WARN("Netplay: fight %u, frame %u: this player's input isn't kept", s.fight, f);
            }
            bool known = false;
            const Pad remote = RemotePad(f, known);
            rb.used[f] = remote;
            rb.lastRun = f;
            DecidePads(f, local, remote);
        }

        // The first frame since those known to be final that ran with another input of the other player's than the one
        // that came, if any; the frames before it that ran with the ones that came are final.
        std::optional<uint32_t> FirstWrong(uint32_t f)
        {
            Session& s = g_session;
            Rollback& rb = s.rb;
            const int64_t last = std::min<int64_t>(int64_t(Received()) - 1, int64_t(f) - 1);
            for (int64_t g = rb.verified + 1; g <= last; g++)
            {
                const auto used = rb.used.find(uint32_t(g));
                const auto real = s.step.remote.find(uint32_t(g));
                if (used == rb.used.end() || real == s.step.remote.end() || !(used->second == real->second.pad))
                    return uint32_t(g);
                rb.verified = g;
            }
            return std::nullopt;
        }

        // Rolls back to fight frame `target`: its savestate loaded, and the frames from it replayed to the present
        // (netplay/replay.h). In a replay already (a frame of the fight's menus found a wrong frame before it), the replay
        // goes on from the earlier frame. False if the savestate couldn't be loaded: the games can't be put in step again.
        bool RollBackTo(uint32_t target, uint64_t frame)
        {
            Session& s = g_session;
            Rollback& rb = s.rb;
            const uint64_t targetFrame = s.step.startFrame + target;
            const std::string error = savestates::Load(targetFrame);
            if (!error.empty())
            {
                LOG_ERROR("Netplay: fight %u: can't roll back to its frame %u (game frame %llu): %s", s.fight, target, (unsigned long long)targetFrame,
                    error.c_str());
                Apart("this machine couldn't roll back to the other player's input: " + error);
                return false;
            }
            journal::ReplayFrom(targetFrame);
            const uint32_t depth = uint32_t(std::max<int64_t>(rb.present - int64_t(target), 0));
            WritePredicted(target, uint32_t(std::max<int64_t>(rb.present, int64_t(target))));
            rb.rollbacks++;
            rb.framesReplayed += depth;
            rb.depths[std::min<uint32_t>(depth, kMaxWindow + 1)]++;
            rb.deepest = std::max(rb.deepest, depth);
            const savestates::Statistics loaded = savestates::GetStatistics();
            const double loadUs = loaded.loadUs;
            rb.loadMicroseconds += loadUs;
            rb.longestLoadMicroseconds = std::max(rb.longestLoadMicroseconds, loadUs);
            rb.drawWaitMicroseconds += loaded.drawWaitUs;
            rb.longestDrawWaitMicroseconds = std::max(rb.longestDrawWaitMicroseconds, loaded.drawWaitUs);
            if (rb.replaying)
            {
                rb.nested++;
                rb.replayClean = false;
            }
            else
            {
                replay::Begin();
                rb.replaying = true;
                rb.firstReplayed = false;
                rb.replayEnd = frame;
                rb.replayRunFrames = 1; // (the frame about to run)
                rb.replayClean = true;
                rb.replayStart = Clock::now();
            }
            return true;
        }

        // A wait in a fight that rolls back for the other player's input of frame `frame`: at the window, or in the
        // fight's menus. False if the session ended meanwhile.
        bool WaitInFight(uint32_t frame, bool window)
        {
            Session& s = g_session;
            Rollback& rb = s.rb;
            const uint64_t waited = WaitForInput(frame, Clock::now(), false);
            // A long wait (the other machine stalled, or the link was down) fills both machines' slacks with it for their
            // next kSlackFrames frames, which would have the time sync slow down for what has passed: both start their
            // slacks afresh (LocalSlack has none until it has kSlackFrames again, and says so in the packets).
            if (waited > kSlackResetMicroseconds)
            {
                s.slacks.clear();
                s.remoteSlack = kNoSlack;
                // (Nor do the frames it got to before the wait ended count, whose inputs come late by the wait.)
                rb.slackFrom = uint32_t(std::max<int64_t>(rb.present + 1, 0));
            }
            if (window)
            {
                rb.windowWaits++;
                rb.windowWaitMicroseconds += waited;
                rb.longestWindowWait = std::max(rb.longestWindowWait, waited);
            }
            else
            {
                rb.menuWaits++;
                rb.menuWaitMicroseconds += waited;
                rb.longestMenuWait = std::max(rb.longestMenuWait, waited);
            }
            return s.state != State::kEnded;
        }

        // Whether the fight is where a press can end it (GameState::fightMenu): its frames run only with the other's input.
        bool InFightMenu()
        {
            const GameState* game = GetGameState();
            return game && game->fightMenu && game->fightMenu();
        }

        // The final checksums (those of frames up to `upTo`, whose inputs before them came and they ran with): sent,
        // compared and logged once each, in order.
        void EmitFinal(uint32_t upTo)
        {
            Session& s = g_session;
            Rollback& rb = s.rb;
            Lockstep& step = s.step;
            for (int64_t g = rb.emitted + 1; g <= int64_t(upTo); g++)
            {
                rb.emitted = g;
                const auto it = rb.checksums.find(uint32_t(g));
                if (it == rb.checksums.end())
                    continue;
                uint64_t checksum = it->second;
                // DEBUG_NET_DESYNC: as if this machine's game had gone another way.
                if (Conditions& c = g_conditions; c.desyncPending)
                {
                    Segment at{ true, step.number, uint32_t(g), 0, true };
                    if (Reached(c.desyncWhen, step.startFrame + uint64_t(g), at))
                    {
                        c.desyncPending = false;
                        checksum ^= 0xDE5E7C;
                        LOG_INFO("Netplay: this machine's checksum made to differ at fight %u's frame %lld", s.fight, (long long)g);
                    }
                }
                step.localChecksums[uint32_t(g)] = checksum;
                CompareChecksum(uint32_t(g));
                LogFightChecksum(uint64_t(g), checksum);
            }
            while (!rb.checksums.empty() && int64_t(rb.checksums.begin()->first) + 8 < rb.emitted)
                rb.checksums.erase(rb.checksums.begin());
        }

        // GGPO's time sync: this machine's frame advantage, half how much sooner the other's inputs come to the other
        // machine than its to this one (both slacks averaged over kSlackFrames frames); the machine ahead by more than
        // kSyncThresholdMs slows down by that much, a little a vblank, then lets both slacks take it in.
        void TimeSync(uint32_t f)
        {
            Session& s = g_session;
            Rollback& rb = s.rb;
            const int32_t local = LocalSlack();
            if (local == kNoSlack || s.remoteSlack == kNoSlack)
                return;
            const double advantage = (double(s.remoteSlack) - double(local)) / 2000.0;
            if (!rb.advantageSamples)
                rb.advantageLow = rb.advantageHigh = advantage;
            rb.advantageSum += advantage;
            rb.advantageSamples++;
            rb.advantageLow = std::min(rb.advantageLow, advantage);
            rb.advantageHigh = std::max(rb.advantageHigh, advantage);
            if (f < rb.nextSync || gpu::PendingVblankDelay() != 0 || advantage < kSyncThresholdMs)
                return;
            const double ms = std::min(advantage, kSyncMaxMs);
            gpu::DelayVblanks(uint64_t(ms * 1000.0), kSyncStepMicroseconds);
            rb.corrections++;
            rb.correctedMilliseconds += ms;
            const uint32_t frames = uint32_t(std::ceil(ms * 1000.0 / kSyncStepMicroseconds));
            rb.nextSync = f + frames + kSyncSettleFrames;
            if (rb.corrections <= 16)
                LOG_INFO("Netplay: fight %u, frame %u: this machine runs %.1f ms ahead of the other (its slack %.1f ms, the other's %.1f): it slows "
                         "down by %.1f ms over %u frames", s.fight, f, advantage, local / 1000.0, s.remoteSlack / 1000.0, ms, frames);
        }

        // A fight frame from `delay` on, which rolls back, before the savestates: its checksum in the timeline running,
        // and as this machine first gets to it (the present), its input sampled and sent. AfterSavestates rolls back,
        // waits and decides the frame's pads.
        void RollbackFrame(const Segment& segment, uint64_t frame)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            Rollback& rb = s.rb;
            const uint32_t f = segment.frame;
            if (std::optional<uint64_t> checksum = FightChecksum())
            {
                rb.checksums[f] = *checksum;
                KeepDesyncState(f);
            }
            if (int64_t(f) <= rb.present)
                return; // a replay's frame, or the present again as a replay ends
            const Clock::time_point boundary = Clock::now();
            rb.present = f;
            rb.reached[f] = boundary;
            while (!rb.reached.empty() && int64_t(rb.reached.begin()->first) + 128 < int64_t(f))
                rb.reached.erase(rb.reached.begin());
            g_where.frame.store(f);
            SetWhere(Where::kInSegment);
            // The present frame before this one: its work (the input delays' measures).
            NoteFrameWork(true);
            step.lastFrame = f;
            Pad local{};
            if (s.sampleLocal)
                s.sampleLocal(local);
            step.local[f + step.delay] = local;
            if (const auto remote = step.remote.find(f); remote != step.remote.end())
                NoteSlack(f, MicrosecondsBetween(remote->second.arrival, boundary));
            SendSegment();
        }

        // After the savestates, at a fight frame that rolls back: a replayed frame's pads (in the fight's menus once the
        // other's input for it has come), or for the present, a rollback to the first frame that ran with another input
        // than the other's that came, or the waits at the window and in the fight's menus, then the frame's pads.
        void RollbackBoundary(const Segment& segment, uint64_t frame)
        {
            Session& s = g_session;
            Lockstep& step = s.step;
            Rollback& rb = s.rb;
            const uint32_t f = segment.frame;
            if (rb.replaying && !rb.firstReplayed)
            {
                rb.firstReplayed = true;
                rb.replayFirstMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - rb.replayStart).count();
                rb.firstFrameMilliseconds += rb.replayFirstMilliseconds;
            }
            if (rb.replaying && frame < rb.replayEnd)
            {
                rb.replayRunFrames++;
                // A replayed frame. In the fight's menus it runs with the other's input as it came, and after frames that
                // did: should the inputs show a frame of this replay wrong, the replay goes back further.
                if (InFightMenu())
                {
                    if (!step.remote.count(f))
                        rb.replayClean = false;
                    if (!step.remote.count(f) && !WaitInFight(f, false))
                    {
                        StopRollback(frame);
                        Alone(segment);
                        return;
                    }
                    if (const std::optional<uint32_t> wrong = FirstWrong(f))
                    {
                        if (RollBackTo(*wrong, frame))
                            RollbackPads(*wrong);
                        else
                        {
                            StopRollback(frame);
                            Alone(segment);
                        }
                        return;
                    }
                }
                RollbackPads(f);
                return;
            }
            // As GGPO, a rollback a frame at most: inputs that came during a replay and show a frame of it wrong roll back
            // at the next frame. But in the fight's menus a frame runs only in step.
            bool replayed = false;
            if (rb.replaying)
            {
                // The replay is back at the present.
                replay::End();
                journal::FinishReplay();
                rb.replaying = false;
                const double replayMs = std::chrono::duration<double, std::milli>(Clock::now() - rb.replayStart).count();
                rb.replayMilliseconds += replayMs;
                // Its frames after the first, back to back: a replayed frame's time (Measures).
                if (rb.replayClean && rb.replayRunFrames > 1 && replayMs > rb.replayFirstMilliseconds)
                {
                    rb.laterMilliseconds += replayMs - rb.replayFirstMilliseconds;
                    rb.laterFrames += rb.replayRunFrames - 1;
                }
                replayed = true;
            }
            for (;;)
            {
                // In the fight's menus, the other's input to this frame first, then a rollback if it shows a frame wrong
                // (whose replay then runs with the inputs as they came).
                const bool menu = InFightMenu();
                if (menu && int64_t(Received()) - 1 < int64_t(f))
                {
                    if (!WaitInFight(f, false))
                    {
                        StopRollback(frame);
                        Alone(segment);
                        return;
                    }
                    replayed = false;
                    continue;
                }
                const std::optional<uint32_t> wrong = !replayed || menu ? FirstWrong(f) : std::nullopt;
                if (wrong)
                {
                    if (RollBackTo(*wrong, frame))
                        RollbackPads(*wrong);
                    else
                    {
                        StopRollback(frame);
                        Alone(segment);
                    }
                    return;
                }
                // The window: the other's input for frame f - window at least.
                if (int64_t(f) - (int64_t(Received()) - 1) <= int64_t(s.window))
                    break;
                if (!WaitInFight(f - s.window, true))
                {
                    StopRollback(frame);
                    Alone(segment);
                    return;
                }
                replayed = false; // late already: what came while waiting is taken in now
            }
            // The present frame runs now: its statistics, the checksums now final, the time sync.
            NotePresentFrame(f, Clock::now());
            const uint32_t ahead = uint32_t(std::max<int64_t>(int64_t(f) - (int64_t(Received()) - 1), 0));
            rb.aheadFrames++;
            rb.aheadSum += ahead;
            rb.furthestAhead = std::max(rb.furthestAhead, ahead);
            EmitFinal(uint32_t(std::min<int64_t>(rb.verified + 1, int64_t(f))));
            TimeSync(f);
            RollbackPads(f);
            WriteSounds(f);
            if (f % kReportInterval == 0)
                LOG_INFO("Netplay: fight %u at its frame %u: %s", s.fight, f, Summary().c_str());
            UpdateStatus(false, Clock::now());
            Forget(uint32_t(std::max<int64_t>(rb.verified + 1, 0)), f);
            while (!rb.used.empty() && int64_t(rb.used.begin()->first) + 64 < rb.verified)
                rb.used.erase(rb.used.begin());
        }
    }

    DelayChoice DelaysFor(double roundTripUs, double roundTripHighUs, double workUs, uint32_t fits)
    {
        DelayChoice choice;
        choice.late = Late(roundTripUs, workUs);
        choice.needed = std::max<uint32_t>(choice.late, 1) - 1;
        const uint32_t high = std::max<uint32_t>(Late(roundTripHighUs, workUs), 1) - 1;
        choice.lockstep = std::min(std::max(choice.needed + 1, high), kMaxDelay);
        // At least kLeastFightDelay, or as many frames as the other's input takes if fewer (at such a round trip no rollback
        // is needed: 1 on a local network, where 2 played the same but a frame later).
        const uint32_t least = std::min(kLeastFightDelay, std::max<uint32_t>(choice.late, 1));
        choice.rollback = std::min(std::max(least, choice.late > fits ? choice.late - fits : 0u), kMaxDelay);
        return choice;
    }

    uint32_t FitsFor(double replayFrameUs, double presentWorkUs, double loadUs, double drawWaitUs, uint32_t window)
    {
        // The load (after its wait for the command processor's draws) and the first replayed frame take the time the
        // present frame would have waited for its vblank, which that first frame then waits out (D3D lets the CPU run a
        // frame ahead); if they don't fit in a frame, nothing does.
        const double replay = std::max(replayFrameUs, 100.0);
        if (drawWaitUs + loadUs + replay > kFrameMicroseconds)
            return 0;
        // The others, and the present frame, in the frame after, leaving the GPU its time to draw the present frame.
        const double rest = kFrameMicroseconds - kDrawMarginMicroseconds - presentWorkUs;
        if (rest < 0)
            return 0;
        return uint32_t(std::clamp(1 + std::floor(rest / replay), 0.0, double(window)));
    }

    bool Configured()
    {
        return g_configured;
    }

    bool RollbackConfigured()
    {
        return RollbackSetting();
    }

    void AfterSavestates(uint64_t frame)
    {
        if (!g_configured)
            return;
        Session& s = g_session;
        if (!s.rollback || !s.step.active || !s.step.fight || s.state == State::kEnded)
            return;
        const std::optional<Segment> segment = CurrentSegment();
        if (!segment || !segment->fight || !segment->flow || segment->number != s.step.number)
            return;
        if (s.rb.on)
            RollbackBoundary(*segment, frame);
        else if (segment->frame == s.step.delay && s.settingsApplied)
            StartRollback(frame); // its frame `delay` ran in lockstep, which set its pads
    }

    void SetLocalPad(bool (*sample)(Pad& pad))
    {
        g_session.sampleLocal = sample;
    }

    void OnFrame(uint64_t frame)
    {
        if (!g_configured)
            return;
        Session& s = g_session;
        if (s.state == State::kOff)
            Start();
        const std::optional<Segment> segment = CurrentSegment();
        const bool flow = netplay::InFlow(); // the game's (frame.h)
        if (segment && segment->fight)
        {
            s.fightSeen = true;
            s.lastFightNumber = segment->number;
        }
        CheckConditions(frame, segment);
        ReceiveAll();
        s.padsSet = false;
        SendHelloIfDue();
        // A segment of the flow: lockstep.
        const bool lockstep = flow && segment && segment->flow;
        if (s.step.active && (!lockstep || s.step.number != segment->number || s.step.fight != segment->fight))
            EndSegment(frame);
        if (s.flow && !flow)
            LeaveFlow(frame);
        if (s.state == State::kConnected)
        {
            CheckSilence(Clock::now());
            PublishPace(); // (the alive packets' measures)
        }
        if (s.state == State::kEnded)
        {
            if (s.step.active)
                EndSegment(frame);
            Alone(segment);
            return;
        }
        if (lockstep)
        {
            // A fight that rolls back, past its first frames (in lockstep, to its frame `delay`, where both machines meet).
            if (s.rollback && segment->fight && s.step.active && s.rb.on)
                RollbackFrame(*segment, frame);
            else
                LockstepFrame(*segment, frame);
            return;
        }

        // Between segments: answer for the segments ended, and the pads.
        if (!s.ended.empty() && s.state == State::kConnected && Clock::now() - s.lastSent >= std::chrono::milliseconds(50) &&
            Clock::now() - s.endedAt < std::chrono::seconds(2))
        {
            // The other machine may still lack this one's last inputs of the segment it ended last; it asks for them by
            // sending its own, but say what this one has, in case its packets were the ones lost.
            const EndedSegment& last = s.ended.back();
            SendSegment(last.key, last.screen, last.delay, last.local, last.checksums, last.peerReceived, kNothingNeeded, kNoSlack);
            s.lastSent = Clock::now();
        }
        UpdateStatus(false, Clock::now());
        if (!flow)
        {
            // Outside the flow, each machine's game is its own: on its way into the flow, which a session is for (the
            // game's presses for its player), or where the game leaves it to them (booting), its player's input, as
            // player 1.
            SetWhere(Where::kOwnTime);
            Pad local{};
            uint16_t buttons = 0;
            if (const GameState* game = GetGameState(); game && game->intoFlow && game->intoFlow(frame, buttons))
                local.buttons = buttons;
            else if (s.sampleLocal)
                s.sampleLocal(local);
            SetPads(local, Pad{}, OwnPacket(0, local), OwnPacket(1, Pad{}));
            return;
        }
        // Loading or leaving a screen of the flow, or loading its fight: nobody's input, on both machines alike, so each
        // may take its own time.
        SetWhere(Where::kBetween);
        const uint32_t packet = (s.segmentsStarted << 20) | kGapPackets;
        SetPads(Pad{}, Pad{}, packet, packet);
        s.lastPads[0] = s.lastPads[1] = Pad{};
    }

    bool PadFor(uint32_t user, bool& connected, Pad& pad, uint32_t& packetNumber)
    {
        const Session& s = g_session;
        if (!s.padsSet)
            return false;
        connected = user < 2;
        if (connected)
        {
            pad = s.pads[user];
            packetNumber = s.packets[user];
        }
        return true;
    }

    bool PlaysFlow()
    {
        return g_configured && g_session.flow;
    }

    std::optional<uint32_t> Seed()
    {
        const Session& s = g_session;
        if (s.state == State::kConnected || s.state == State::kEnded)
        {
            if (s.host || s.id != 0)
                return s.seed;
        }
        return std::nullopt;
    }

    void Shutdown()
    {
        Session& s = g_session;
        if (!g_configured || s.state != State::kConnected)
            return;
        LOG_INFO("Netplay: leaving the session");
        SendBye(Leaving::kLeaves);
        s.state = State::kEnded;
    }
}
