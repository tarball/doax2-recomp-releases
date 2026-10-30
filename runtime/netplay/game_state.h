#pragma once

// What netplay needs to know about a game's state, which the game's own code registers (runtime/game/): whose writes
// make up the state a savestate holds, and the state the game keeps outside guest memory.
#include <bitset>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct PPCContext;

namespace netplay
{
    // The state a game keeps outside guest memory, saved at a frame boundary.
    struct HostState
    {
        // Loads it at a later boundary. Returns an error message (and loads nothing) or an empty string; with `note`, also
        // what the load changed, for reports (fibers resumed elsewhere than they're suspended now). It reads guest memory
        // as it is (where the fibers are suspended now), so it comes before guest memory is loaded.
        std::function<std::string(std::string* note)> load;
        // The guest stacks of the code suspended (fibers), and their parts below the stack pointers, which hold nothing:
        // [start, end).
        std::vector<std::pair<uint32_t, uint32_t>> stacks, freeStack;
    };

    // A part of what two machines must have alike (a fight's setup, the game's settings): a name for the log, and the
    // bytes, as guest memory holds them.
    struct StatePart
    {
        const char* name = "";
        std::vector<uint8_t> bytes;
    };

    struct GameState
    {
        // The guest threads that run the game's frames, by start address: the main thread and the game's worker pools.
        // The others (the libraries' streaming, audio and GPU threads) run on their own time and stay out of savestates.
        bool (*isGameThread)(uint32_t startAddress) = nullptr;
        // Whether work the game hands a thread of its own and then polls for each frame is under way, running (a save's
        // file operations, a file read in a fight): it takes each machine its own time, so a session's frame boundary
        // lets it finish first (frame.h), and the frames that poll see it done at the same frame on both machines.
        bool (*asyncWorkRunning)() = nullptr;
        // Whether a guest function belongs to a library whose memory stays out of savestates though the game's threads
        // run it (D3D, audio): that state follows the hardware's, which doesn't roll back.
        bool (*isLibraryFunction)(uint32_t function) = nullptr;
        // Whether a guest function allocates memory for its callers (the heap's functions, the C runtime's malloc): an
        // allocation belongs to the first caller up the chain that isn't one.
        bool (*isAllocatorFunction)(uint32_t function) = nullptr;
        // Threads besides the game's (isGameThread) whose allocations by the game's code are the game's too, by start
        // address: they make objects the game's frames then work on (DOA4's job thread, which loads a stage's).
        bool (*allocatesForGame)(uint32_t startAddress) = nullptr;
        // Functions (the first up an allocation's chain that isn't an allocator) whose allocations with a library's
        // allocator ID (XMemAlloc's) are the game's on its threads all the same: they make a library's objects that the
        // game's frames make and free (DOA4: D3D's vertex declarations of its models).
        bool (*makesForGame)(uint32_t function) = nullptr;
        // Guest memory the game writes that differs between runs of the same frames without changing them, as
        // [start, end) ranges: timestamps and profiling timers. Sync tests don't compare it.
        std::vector<std::pair<uint32_t, uint32_t>> unchecked;
        // The game's state in physical memory (the views from 0xA0000000, which the write tracker doesn't cover), as
        // [start, end) ranges of guest addresses, at a frame boundary: a savestate holds them whole (DOA4: the ring the
        // fight records its frames in for the KO's replay, and its models' blocks, heaps::PhysicalBlocks).
        std::vector<std::pair<uint32_t, uint32_t>> (*physicalState)() = nullptr;
        // All of the game's state in guest memory, by where it lies rather than by who writes it, for savestates saved at
        // every frame boundary (netplay/savestates.h), at a frame boundary: whole pages as [start, end) ranges of guest
        // addresses (its heap, the memory it reserved itself, its threads' and fibers' stacks, its blocks of physical
        // memory, the image's pages that hold only its variables), and the image's pages that hold its variables beside
        // libraries' (by page address, with a mask of the bytes that are the game's).
        struct SavedMemory
        {
            std::vector<std::pair<uint32_t, uint32_t>> ranges;
            std::vector<std::pair<uint32_t, const std::bitset<4096>*>> partial;
        };
        SavedMemory (*savedMemory)() = nullptr;
        // Saves the state the game keeps outside guest memory (suspended fibers' contexts, host stacks and registers) at a
        // frame boundary, or returns one with no `load` and sets `error`.
        std::function<HostState(std::string& error)> saveHost;
        // At a frame boundary: a hash of each part of that state (each suspended fiber's host stack and registers, by its
        // record), for checking that frames replayed from a savestate leave it as they did the first time (sync tests).
        std::vector<std::pair<uint32_t, uint64_t>> (*hostStates)() = nullptr;
        // At a frame boundary, on the main thread: whether the frames about to run are a fight's, past its loading, which
        // netplay counts from the first (netplay::FightFrame: each machine gets there after loading in its own time), or
        // the fight's loading.
        enum class FightState
        {
            kNone,
            kLoading,
            kFighting,
        };
        FightState (*fightState)() = nullptr;
        // At a frame boundary outside a fight, on the main thread: what the menus are doing, for a session's lockstep
        // in them (netplay::CurrentSegment). kLocal: the game boots (logos, movies) or plays its attract sequence, on
        // its own time; kLoading: a screen loads or leaves and takes no input; kReady: the menu screen `screen` takes
        // input, and from then on, until it leaves, its frames play out the same on any machine given the same input.
        // `flow`: whether the screen belongs to the game's multiplayer flow, the part of its menus that a session plays
        // in lockstep (DOA4: Versus mode's, from its menu to stage select), which the fights started from it continue;
        // each machine plays the rest of its menus on its own. While a screen loads or leaves, `flowKnown` says whether
        // the game can tell (the screen it leaves or the one it loads belongs to the flow, or neither does).
        struct MenuState
        {
            enum Kind
            {
                kLocal,
                kLoading,
                kReady,
            } kind = kLoading;
            uint32_t screen = 0;
            bool flow = false;
            bool flowKnown = false;
        };
        MenuState (*menuState)() = nullptr;
        // The multiplayer flow's name for the player (DOA4: "Versus"), for the window's title while a machine waits there
        // for the other to enter it.
        const char* flowName = nullptr;
        // In a session, at a frame boundary outside the multiplayer flow, on the main thread: the buttons (XInput's) that
        // take the game into the flow from where it is, pressed for this machine's player in place of their own (DOA4:
        // Start at the title, then Versus at mode select), or false where the player's own input applies (DOA4: booting,
        // whose logos and movie the player may skip). A session is for the flow, and goes straight into it. `frame` is the
        // boundary's (netplay::Frame), for the presses' timing.
        bool (*intoFlow)(uint64_t frame, uint16_t& buttons) = nullptr;
        // In a session's flow, at a frame boundary, on the main thread, the same on both machines: the buttons (XInput's)
        // to take out of a player's pad that holds `buttons` this frame, because they'd take the game out of the flow
        // (DOA4: B on Versus's menu, A and Start on Quit in the pause menu and the match's result menu). With `stick` set,
        // the pad's left stick is centered too (DOA4: the directions while A or Start is held in those menus, which could
        // move the cursor onto Quit in the frame the press takes). Nobody leaves the flow during a session.
        uint16_t (*flowBlockedButtons)(uint16_t buttons, bool& stick) = nullptr;
        // At a frame boundary in a menu screen (kReady): a checksum of the menus' state, of values machines in step hold
        // alike, to find where two machines' menus went separate ways.
        uint64_t (*menuChecksum)() = nullptr;
        // In a session, at a menu screen's frame `delay` (the first that both machines' inputs reach, so both are surely
        // there): seeds the game's random numbers from `seed`, the same on both machines, so that what the menus decide at
        // random (RANDOM picks) agrees whatever each drew before. Guest calls on the main loop's context at the boundary
        // (netplay::BoundaryContext), its registers kept.
        void (*seedMenus)(PPCContext& ctx, uint8_t* base, uint64_t seed) = nullptr;
        // At a fight's first frame past loading, with a fight seed (netplay::FightSeed): seeds the game's random numbers
        // again, from `seed`, the same on both machines: the loading's last frames draw as its files come in, the
        // stage's update and the players' tasks starting a frame apart or not, which a slower disk changes. Guest calls
        // as seedMenus's.
        void (*seedFight)(PPCContext& ctx, uint8_t* base, uint64_t seed) = nullptr;
        // At a frame boundary in a fight, after a session has ended in it (the other player left): ends the match in the
        // favor of the player of game user `user` (0 or 1), as the game does online when the other fighter leaves.
        // Returns false while it can't yet (not fighting), to be asked again at the next boundary. Guest calls as above.
        bool (*abandonFight)(PPCContext& ctx, uint8_t* base, uint32_t user) = nullptr;
        // At a frame boundary in a fight: a checksum of the fight's state, of values both machines hold alike (no
        // addresses), to find desyncs (netplay::FightChecksum).
        uint64_t (*fightChecksum)() = nullptr;
        // At a frame boundary in a fight, on the main thread: whether the game is where a press can end the fight in
        // the frames ahead (DOA4: paused, and the match over, its result and the menu after it), where a session that
        // rolls back runs a frame only with the other player's input as it came (netplay/session.h): a fight must end
        // at the same frame on both machines, which a frame run on a prediction can't promise.
        bool (*fightMenu)() = nullptr;
        // At a frame boundary: the sounds the game's audio engine plays (DOA4: each sound's and stream's number), sorted,
        // for comparing two runs' audio (netplay/rollback_test.h).
        std::vector<uint32_t> (*soundsPlaying)() = nullptr;
        // At a fight's first frame past loading, on the main thread: what makes the fight the one it is (the stage, the
        // characters and costumes, the match's rules, the players' button layouts, the random number generators), by
        // part. Sessions compare it between the machines and name the parts that differ (netplay::FightSetup).
        std::vector<StatePart> (*fightSetup)() = nullptr;
        // A player's settings that shape a session's fights (DOA4: their button layout, and Versus Mode's rules, of which
        // the host's apply), as `size` bytes (at most kMaxPlayerSettings): each machine sends its own as it enters the
        // flow, and both apply both players' there (netplay/session.h).
        static constexpr size_t kMaxPlayerSettings = 32;
        struct PlayerSettings
        {
            size_t size = 0;
            // This machine's player's settings, or false while the game has none (no profile signed in yet).
            bool (*local)(uint8_t* bytes) = nullptr;
            // Whether settings from the other machine are ones the game can use (every value within its range).
            bool (*valid)(const uint8_t* bytes) = nullptr;
            // For the log.
            std::string (*describe)(const uint8_t* bytes) = nullptr;
            // At a frame boundary in the flow, on the main thread, the same on both machines: player 1's (the host's) and
            // player 2's settings for the flow's fights; both nullptr as the flow ends (the game's own again).
            void (*apply)(const uint8_t* host, const uint8_t* joining) = nullptr;
        };
        PlayerSettings playerSettings;
        // Guest memory a session keeps at each fight frame, written out at a desync for comparing two machines' states
        // (the checksummed state and more): [address, address + length), by name.
        struct Region
        {
            const char* name;
            uint32_t address, length;
        };
        std::vector<Region> desyncRegions;
        // The guest's math functions the fingerprint runs through the recompiled code (FINGERPRINT=1,
        // netplay/fingerprint.h): each takes a double in f1, with an exponent in [lowExponent, highExponent] and made
        // positive with `positive` (pow's base), and a second in f2 with one in [secondLowExponent, secondHighExponent]
        // if it takes two, and returns a double in f1.
        struct MathFunction
        {
            const char* name;
            uint32_t address;
            int lowExponent, highExponent;
            bool positive;
            int secondLowExponent, secondHighExponent;
        };
        std::vector<MathFunction> mathFunctions;
    };

    void RegisterGameState(GameState state);
    // nullptr if the game registered nothing (as in the runtime's tests).
    const GameState* GetGameState();
}
