// Controller input through host XInput. The 360 controller is where XInput came from, so the
// state layout and button bits are identical; only byte order differs.
//
// Player 1 can also use the keyboard while the game window has focus (merged with controller 1;
// player 1 counts as connected without a controller):
//   arrows / WASD  D-pad        Enter  Start      Backspace  Back
//   Z / J / Space  A            X / K  B          C / U      X           V / I  Y
//   Q / E          LB / RB      1 / 3  LT / RT
// AUTO_PRESS=<buttons hex>[,<period ms>[,<start ms>[,<count>]]] presses buttons by itself (for
// testing): from the start time on, they are held for 100 ms every period (default 2000 ms), `count`
// times (default: forever). INPUT_SCRIPT=<ms>:<buttons hex>[,<ms>:<buttons hex>...] also presses
// buttons for 100 ms at those times (on the same clock, from the first input poll), e.g. to move the
// cursor in a menu: D-pad up 1, down 2, left 4, right 8, Start 10, A 1000, B 2000. A press written
// <ms>:<buttons hex>@<lx>,<ly> also deflects the left stick that far (-32768 to 32767). With INPUT_FRAMES=1 both count game
// frames (netplay::Frame, where the game's main loop starts each frame) instead of milliseconds, and presses last 6 frames.
// A press written F<frame>:<buttons hex> counts the fight's frames instead (netplay::FightFrame, from its first frame past
// loading, which takes each run its own time) and lasts 6 frames, and one written S<screen>.<frame>:<buttons hex> the
// frames of the menus' screen number <screen> (netplay::Segment: the menu screens the game gets to, counted from 0 at boot,
// each from its first frame past its loading), as a netplay session's lockstep counts them. Those are player 1's;
// INPUT_SCRIPT2 is player 2's, who then counts as connected without a controller.
// Pad reads go through netplay's input log (runtime/netplay/input_log.h), which records them or plays them back, and in a
// netplay session (runtime/netplay/session.h) players 1 and 2 are the session's: the local player is this machine's
// player 1 input above (controller 1, the keyboard and the scripts), player 1 on its own and in the session's multiplayer
// flow the host's player 1 and the other's player 2, whose input comes from the network.
#include "guest_abi.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/input_log.h"
#include "netplay/replay.h"
#include "netplay/rollback_test.h"
#include "netplay/session.h"
#include "ui/window.h"
#include "xam/xam.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <windows.h>
#include <xinput.h>

namespace
{
    constexpr uint32_t kAnyUser = 0xFF;

    struct Gamepad
    {
        be<uint16_t> buttons;
        uint8_t leftTrigger;
        uint8_t rightTrigger;
        be<int16_t> thumbLX;
        be<int16_t> thumbLY;
        be<int16_t> thumbRX;
        be<int16_t> thumbRY;
    };
    static_assert(sizeof(Gamepad) == 12);

    struct InputState
    {
        be<uint32_t> packetNumber;
        Gamepad gamepad;
    };
    static_assert(sizeof(InputState) == 16);

    struct Vibration
    {
        be<uint16_t> leftMotorSpeed;
        be<uint16_t> rightMotorSpeed;
    };

    struct Capabilities
    {
        uint8_t type;
        uint8_t subType;
        be<uint16_t> flags;
        Gamepad gamepad;
        Vibration vibration;
    };
    static_assert(sizeof(Capabilities) == 20);

    struct Keystroke
    {
        be<uint16_t> virtualKey;
        be<uint16_t> unicode;
        be<uint16_t> flags;
        uint8_t userIndex;
        uint8_t hidCode;
    };
    static_assert(sizeof(Keystroke) == 8);

    void CopyGamepad(const XINPUT_GAMEPAD& from, Gamepad& to)
    {
        to.buttons = from.wButtons;
        to.leftTrigger = from.bLeftTrigger;
        to.rightTrigger = from.bRightTrigger;
        to.thumbLX = from.sThumbLX;
        to.thumbLY = from.sThumbLY;
        to.thumbRX = from.sThumbRX;
        to.thumbRY = from.sThumbRY;
    }

    uint32_t HostUser(uint32_t userIndex)
    {
        return userIndex == kAnyUser ? 0 : userIndex;
    }

    struct KeyBinding
    {
        int key;
        WORD button;
    };
    constexpr KeyBinding kKeyBindings[] = {
        { VK_UP, XINPUT_GAMEPAD_DPAD_UP }, { 'W', XINPUT_GAMEPAD_DPAD_UP },
        { VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN }, { 'S', XINPUT_GAMEPAD_DPAD_DOWN },
        { VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT }, { 'A', XINPUT_GAMEPAD_DPAD_LEFT },
        { VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT }, { 'D', XINPUT_GAMEPAD_DPAD_RIGHT },
        { VK_RETURN, XINPUT_GAMEPAD_START }, { VK_BACK, XINPUT_GAMEPAD_BACK },
        { 'Z', XINPUT_GAMEPAD_A }, { 'J', XINPUT_GAMEPAD_A }, { VK_SPACE, XINPUT_GAMEPAD_A },
        { 'X', XINPUT_GAMEPAD_B }, { 'K', XINPUT_GAMEPAD_B },
        { 'C', XINPUT_GAMEPAD_X }, { 'U', XINPUT_GAMEPAD_X },
        { 'V', XINPUT_GAMEPAD_Y }, { 'I', XINPUT_GAMEPAD_Y },
        { 'Q', XINPUT_GAMEPAD_LEFT_SHOULDER }, { 'E', XINPUT_GAMEPAD_RIGHT_SHOULDER },
    };

    // A player's scripted presses (INPUT_SCRIPT, INPUT_SCRIPT2).
    struct Script
    {
        struct Press
        {
            enum Clock
            {
                kTime,   // milliseconds, or game frames with INPUT_FRAMES
                kFight,  // F<frame>: a fight's frames
                kScreen, // S<screen>.<frame>: the frames of the menus' screen number <screen>
            } clock = kTime;
            uint32_t screen = 0;
            long long time = 0;
            WORD buttons = 0;
            SHORT lx = 0, ly = 0; // the left stick, if deflected (@lx,ly)
        };
        std::vector<Press> presses;
        std::vector<uint8_t> logged; // each press is logged as it starts
        explicit Script(const char* setting)
        {
            for (const char* p = setting; p && *p;)
            {
                Press press;
                char* end = nullptr;
                if (*p == 'F')
                {
                    press.clock = Press::kFight;
                    p++;
                }
                else if (*p == 'S')
                {
                    press.clock = Press::kScreen;
                    press.screen = uint32_t(strtoul(p + 1, &end, 10));
                    if (*end != '.')
                        break;
                    p = end + 1;
                }
                press.time = strtoll(p, &end, 10);
                if (*end != ':')
                    break;
                press.buttons = WORD(strtoul(end + 1, &end, 16));
                if (*end == '@')
                {
                    press.lx = SHORT(std::clamp(strtol(end + 1, &end, 10), -32768l, 32767l));
                    if (*end == ',')
                        press.ly = SHORT(std::clamp(strtol(end + 1, &end, 10), -32768l, 32767l));
                }
                presses.push_back(press);
                p = *end ? end + 1 : end;
            }
            logged.resize(presses.size());
        }
    };

    // The virtual input's settings, read at the first input poll (the scripts' clock starts then).
    struct VirtualInput
    {
        WORD buttons = 0; // AUTO_PRESS
        long long period = 2000, start = 0, count = -1;
        Script scripts[2]{ Script(std::getenv(GAME_PREFIX "_INPUT_SCRIPT")), Script(std::getenv(GAME_PREFIX "_INPUT_SCRIPT2")) };
        // A rollback test's predicted presses for player 2 (DEBUG_ROLLBACK_PREDICT, netplay/rollback_test.h).
        Script predicted{ std::getenv(GAME_PREFIX "_DEBUG_ROLLBACK_PREDICT") };
        std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
        bool frames = false; // INPUT_FRAMES: times are game frames
        VirtualInput()
        {
            if (const char* setting = std::getenv(GAME_PREFIX "_INPUT_FRAMES"))
                frames = std::atoi(setting) != 0;
            if (const char* setting = std::getenv(GAME_PREFIX "_AUTO_PRESS"))
            {
                char* end = nullptr;
                buttons = WORD(strtoul(setting, &end, 16));
                if (end && *end == ',')
                    period = strtoll(end + 1, &end, 10);
                if (end && *end == ',')
                    start = strtoll(end + 1, &end, 10);
                if (end && *end == ',')
                    count = strtoll(end + 1, &end, 10);
            }
        }
    };

    VirtualInput& Virtual()
    {
        static VirtualInput input;
        return input;
    }

    // Whether player 2 is scripted, and so connected without a controller.
    bool ScriptedPlayer2()
    {
        return !Virtual().scripts[1].presses.empty();
    }

    // The buttons a script's presses counted in fight frames hold at fight frame `fightFrame` (each lasts 6 frames), for a
    // rollback test: script 0 is player 1's, 1 player 2's, 2 the predicted presses.
    uint16_t ScriptButtons(int which, uint64_t fightFrame)
    {
        VirtualInput& input = Virtual();
        const Script& script = which == 2 ? input.predicted : input.scripts[which & 1];
        uint16_t buttons = 0;
        for (const Script::Press& press : script.presses)
        {
            if (press.clock == Script::Press::kFight && (long long)fightFrame >= press.time && (long long)fightFrame < press.time + 6)
                buttons |= press.buttons;
        }
        return buttons;
    }

    [[maybe_unused]] const bool g_rollbackScripts = (netplay::rollbacktest::SetScripts(ScriptButtons), true);

    // Keyboard and automatic presses for player 1, and scripted ones for players 1 and 2, merged into `pad`. Returns
    // whether anything is pressed.
    bool ApplyVirtualInput(uint32_t user, XINPUT_GAMEPAD& pad)
    {
        bool active = false;
        // Not while Alt is held: Alt+Enter switches fullscreen, and Enter is Start.
        if (user == 0 && ui::GameWindowFocused() && !(GetAsyncKeyState(VK_MENU) & 0x8000))
        {
            for (const KeyBinding& binding : kKeyBindings)
            {
                if (GetAsyncKeyState(binding.key) & 0x8000)
                {
                    pad.wButtons |= binding.button;
                    active = true;
                }
            }
            if (GetAsyncKeyState('1') & 0x8000)
                pad.bLeftTrigger = 255, active = true;
            if (GetAsyncKeyState('3') & 0x8000)
                pad.bRightTrigger = 255, active = true;
        }

        VirtualInput& input = Virtual();
        const long long now = input.frames
            ? (long long)netplay::Frame()
            : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - input.origin).count();
        const long long hold = input.frames ? 6 : 100;
        if (user == 0 && input.buttons)
        {
            const long long elapsed = now - input.start;
            const long long period = std::max(input.period, 2 * hold);
            if (elapsed >= 0 && elapsed % period < hold && (input.count < 0 || elapsed / period < input.count))
            {
                pad.wButtons |= input.buttons;
                active = true;
            }
        }
        // Each scripted press is logged when it starts, with the game frame (for turning scripts into frames).
        const std::optional<uint64_t> fightFrame = netplay::FightFrame();
        const std::optional<netplay::Segment> segment = netplay::CurrentSegment();
        Script& script = input.scripts[user];
        for (size_t i = 0; i < script.presses.size(); i++)
        {
            const Script::Press& press = script.presses[i];
            long long at = now, length = hold;
            if (press.clock == Script::Press::kFight)
            {
                if (!fightFrame)
                    continue;
                at = (long long)*fightFrame;
                length = 6;
            }
            else if (press.clock == Script::Press::kScreen)
            {
                if (!segment || segment->fight || segment->number != press.screen)
                    continue;
                at = segment->frame;
                length = 6;
            }
            if (at >= press.time && at < press.time + length)
            {
                pad.wButtons |= press.buttons;
                if (press.lx || press.ly)
                {
                    pad.sThumbLX = press.lx;
                    pad.sThumbLY = press.ly;
                }
                active = true;
                if (!script.logged[i])
                {
                    script.logged[i] = 1;
                    char when[48];
                    if (press.clock == Script::Press::kFight)
                        snprintf(when, sizeof(when), "F%lld fight frames", press.time);
                    else if (press.clock == Script::Press::kScreen)
                        snprintf(when, sizeof(when), "S%u.%lld (screen %03X)", press.screen, press.time, segment->screen);
                    else
                        snprintf(when, sizeof(when), "%lld %s", press.time, input.frames ? "frames" : "ms");
                    LOG_INFO("input: player %u script press %X at %s, game frame %llu", user + 1, press.buttons, when,
                        (unsigned long long)netplay::Frame());
                }
            }
        }
        return active;
    }

    // The host state of a player, with virtual input for players 1 and 2. False if not connected.
    bool GetHostState(uint32_t user, XINPUT_STATE& state)
    {
        const bool connected = XInputGetState(user, &state) == ERROR_SUCCESS;
        if (user > 1 || (user == 1 && !ScriptedPlayer2()))
            return connected;
        if (!connected)
            state = XINPUT_STATE{};
        // Packet numbers must change whenever the state does.
        static std::atomic<DWORD> virtualPacket[2]{};
        static std::atomic<uint64_t> lastState[2]{}; // buttons and left stick
        ApplyVirtualInput(user, state.Gamepad);
        const uint64_t current = state.Gamepad.wButtons | (uint64_t(uint16_t(state.Gamepad.sThumbLX)) << 16) |
                                 (uint64_t(uint16_t(state.Gamepad.sThumbLY)) << 32);
        if (current != lastState[user].exchange(current))
            virtualPacket[user]++;
        state.dwPacketNumber += virtualPacket[user].load();
        return true;
    }

    // The local player's controller for a netplay session.
    bool SampleLocalPad(netplay::session::Pad& pad)
    {
        XINPUT_STATE state{};
        if (!GetHostState(0, state))
            return false;
        pad.buttons = state.Gamepad.wButtons;
        pad.leftTrigger = state.Gamepad.bLeftTrigger;
        pad.rightTrigger = state.Gamepad.bRightTrigger;
        pad.thumbLX = state.Gamepad.sThumbLX;
        pad.thumbLY = state.Gamepad.sThumbLY;
        pad.thumbRX = state.Gamepad.sThumbRX;
        pad.thumbRY = state.Gamepad.sThumbRY;
        return true;
    }

    [[maybe_unused]] const bool g_sessionPad = (netplay::session::SetLocalPad(SampleLocalPad), true);

    uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t /*flags*/, Capabilities* capabilities)
    {
        const uint32_t user = HostUser(userIndex);
        if (!capabilities || user >= XUSER_MAX_COUNT)
            return xam::kErrorInvalidParameter;
        XINPUT_CAPABILITIES host{};
        bool sessionConnected = false;
        netplay::session::Pad sessionPad;
        uint32_t sessionPacket = 0;
        // In a netplay session, from boot: standard gamepads for players 1 and 2 and nothing else, on both machines alike
        // (a game may pick a button layout by the controller's type, an arcade stick's another).
        const bool session = netplay::session::Configured() || netplay::session::PadFor(user, sessionConnected, sessionPad, sessionPacket);
        if (netplay::session::Configured())
            sessionConnected = user < 2;
        if (session && !sessionConnected)
            return xam::kErrorDeviceNotConnected;
        if (session || XInputGetCapabilities(user, 0, &host) != ERROR_SUCCESS)
        {
            if (!session && user != 0 && !(user == 1 && ScriptedPlayer2()))
                return xam::kErrorDeviceNotConnected;
            // Player 1 without a controller (the keyboard stands in), or player 2 scripted: a standard gamepad.
            host.Type = XINPUT_DEVTYPE_GAMEPAD;
            host.SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
            host.Gamepad.wButtons = 0xF3FF;
            host.Gamepad.bLeftTrigger = host.Gamepad.bRightTrigger = 0xFF;
        }
        capabilities->type = host.Type;
        capabilities->subType = host.SubType;
        capabilities->flags = host.Flags;
        CopyGamepad(host.Gamepad, capabilities->gamepad);
        capabilities->vibration.leftMotorSpeed = host.Vibration.wLeftMotorSpeed;
        capabilities->vibration.rightMotorSpeed = host.Vibration.wRightMotorSpeed;
        return xam::kErrorSuccess;
    }

    uint32_t XamInputGetState(uint32_t userIndex, uint32_t /*flags*/, InputState* state)
    {
        const uint32_t user = HostUser(userIndex);
        if (user >= XUSER_MAX_COUNT)
            return xam::kErrorInvalidParameter;
        static_assert(sizeof(netplay::input::PadRead::state) == sizeof(InputState));
        // In a netplay session, players 1 and 2 are the session's, and players 3 and 4 never connected (as their
        // capabilities say).
        bool sessionConnected = false;
        netplay::session::Pad sessionPad;
        uint32_t sessionPacket = 0;
        if (netplay::session::Configured() && user >= 2)
            return xam::kErrorDeviceNotConnected;
        if (netplay::session::PadFor(user, sessionConnected, sessionPad, sessionPacket))
        {
            if (!sessionConnected)
                return xam::kErrorDeviceNotConnected;
            if (state)
            {
                state->packetNumber = sessionPacket;
                state->gamepad.buttons = sessionPad.buttons;
                state->gamepad.leftTrigger = sessionPad.leftTrigger;
                state->gamepad.rightTrigger = sessionPad.rightTrigger;
                state->gamepad.thumbLX = sessionPad.thumbLX;
                state->gamepad.thumbLY = sessionPad.thumbLY;
                state->gamepad.thumbRX = sessionPad.thumbRX;
                state->gamepad.thumbRY = sessionPad.thumbRY;
            }
            return xam::kErrorSuccess;
        }
        // In a rollback test's fight, the players' pads are the test's: the scripts', the remote player's on the test's
        // timeline (player 2's only if scripted, as it's connected only then).
        uint16_t testButtons = 0;
        uint32_t testPacket = 0;
        if ((user == 0 || ScriptedPlayer2()) && netplay::rollbacktest::PadFor(user, testButtons, testPacket))
        {
            if (state)
            {
                guest::BeforeHostWriteTo(state, sizeof(InputState));
                memset(state, 0, sizeof(InputState));
                state->packetNumber = testPacket;
                state->gamepad.buttons = testButtons;
            }
            return xam::kErrorSuccess;
        }
        netplay::input::PadRead read;
        if (!netplay::input::Replayed(user, read))
        {
            XINPUT_STATE host{};
            read.result = GetHostState(user, host) ? xam::kErrorSuccess : xam::kErrorDeviceNotConnected;
            if (read.result == xam::kErrorSuccess)
            {
                InputState guest;
                guest.packetNumber = host.dwPacketNumber;
                CopyGamepad(host.Gamepad, guest.gamepad);
                memcpy(read.state, &guest, sizeof(guest));
            }
            netplay::input::Note(user, read);
        }
        if (state && read.result == xam::kErrorSuccess)
        {
            guest::BeforeHostWriteTo(state, sizeof(InputState));
            memcpy(state, read.state, sizeof(InputState));
        }
        return read.result;
    }

    // Rumble asked for in replayed frames (netplay/replay.h), which the controllers get as the replay ends: the game
    // asks again in each frame it replays, and the last state asked for is the one it ends with.
    std::mutex g_replayRumbleLock;
    std::array<std::optional<XINPUT_VIBRATION>, XUSER_MAX_COUNT> g_replayRumble;

    void SendReplayRumble()
    {
        std::lock_guard lock(g_replayRumbleLock);
        for (DWORD user = 0; user < XUSER_MAX_COUNT; user++)
        {
            if (g_replayRumble[user])
                XInputSetState(user, &*g_replayRumble[user]);
            g_replayRumble[user].reset();
        }
    }

    [[maybe_unused]] const bool g_replayRumbleRegistered = (netplay::replay::OnEnd(SendReplayRumble), true);

    uint32_t XamInputSetState(uint32_t userIndex, uint32_t /*unknown*/, const Vibration* vibration)
    {
        const uint32_t user = HostUser(userIndex);
        if (!vibration || user >= XUSER_MAX_COUNT)
            return xam::kErrorInvalidParameter;
        XINPUT_VIBRATION host{ vibration->leftMotorSpeed, vibration->rightMotorSpeed };
        if (netplay::replay::Active())
        {
            XINPUT_CAPABILITIES capabilities{};
            if (XInputGetCapabilities(user, 0, &capabilities) != ERROR_SUCCESS)
                return xam::kErrorDeviceNotConnected;
            std::lock_guard lock(g_replayRumbleLock);
            g_replayRumble[user] = host;
            return xam::kErrorSuccess;
        }
        return XInputSetState(user, &host) == ERROR_SUCCESS ? xam::kErrorSuccess : xam::kErrorDeviceNotConnected;
    }

    uint32_t XamInputGetKeystrokeEx(be<uint32_t>* userIndex, uint32_t /*flags*/, Keystroke* keystroke)
    {
        // In a netplay session the game gets no keystrokes, which come from this machine's controllers alone: both
        // machines' games must see the same input.
        if (netplay::session::Configured())
            return xam::kErrorEmpty;
        const uint32_t requested = userIndex ? uint32_t(*userIndex) : kAnyUser;
        XINPUT_KEYSTROKE host{};
        const DWORD result = XInputGetKeystroke(requested == kAnyUser ? XUSER_INDEX_ANY : requested, 0, &host);
        if (result == ERROR_EMPTY)
            return xam::kErrorEmpty;
        if (result != ERROR_SUCCESS)
            return xam::kErrorDeviceNotConnected;
        if (keystroke)
        {
            keystroke->virtualKey = host.VirtualKey;
            keystroke->unicode = host.Unicode;
            keystroke->flags = host.Flags;
            keystroke->userIndex = host.UserIndex;
            keystroke->hidCode = host.HidCode;
        }
        if (userIndex)
            *userIndex = host.UserIndex;
        return xam::kErrorSuccess;
    }
}

GUEST_EXPORT(XamInputGetCapabilities, XamInputGetCapabilities)
GUEST_EXPORT(XamInputGetState, XamInputGetState)
GUEST_EXPORT(XamInputSetState, XamInputSetState)
GUEST_EXPORT(XamInputGetKeystrokeEx, XamInputGetKeystrokeEx)
