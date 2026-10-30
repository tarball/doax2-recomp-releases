// OPTIONS > Video Settings with the host's graphics options (DOAX2-specific addresses).
//
// DOAX2's Video Settings submenu (menu 3 of the OPTIONS screen) has five rows: the Gamma, Brightness and Contrast
// sliders, Default and Exit. The host's options (settings::Graphics) go between Contrast and Default, drawn like the
// Language submenu's rows: a value, with arrows on the cursor's row, which left and right step through. The menu
// functions that give a menu's row count (sub_82591B68) and labels (sub_825919B0) answer for the ten rows, so the game's
// own code draws their panels, labels and cursor; the submenu's update (sub_825901E8, from the OPTIONS update) and draw
// (sub_824D40D0, from the OPTIONS draw) are ported with the added rows, and its description (sub_824D2F70) gives
// theirs. While OPTIONS is shown the submenu starts a row higher, so its ten rows end level with the OPTIONS list.
// Default resets the host's options too.
#include "guest_abi.h"
#include "settings.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>

extern "C" PPC_FUNC(__imp__sub_82591B68); // a menu's row count
extern "C" PPC_FUNC(__imp__sub_825919B0); // a menu row's label
extern "C" PPC_FUNC(__imp__sub_824D2F70); // the OPTIONS screen's description box
extern "C" PPC_FUNC(__imp__sub_824C4818); // a message's text
extern "C" PPC_FUNC(__imp__sub_824D5070); // the OPTIONS screen's start
extern "C" PPC_FUNC(__imp__sub_824D5870); // the OPTIONS screen's end

namespace
{
    // The submenu.
    constexpr uint32_t kVideoMenu = 3;          // for the menu functions, and the OPTIONS screen's open submenu
    constexpr uint32_t kSliders = 3;            // Gamma, Brightness, Contrast, then Default and Exit
    constexpr uint32_t kLayout = 0x82C09E90;    // menu, style, ?, x, y, width, height (u16), row spacing (u16), slide-in frames
    constexpr uint32_t kCursors = 0x834A3930;   // u16 for each menu level; the submenu's is level 1's
    constexpr uint32_t kLevel = 0x83984F4C;     // u16: the OPTIONS screen's menu level
    constexpr uint32_t kSubmenu = 0x83984F50;   // the open submenu (0 at level 0)
    constexpr uint32_t kRefreshDescription = 0x83984134; // nonzero: the description box takes its text again
    constexpr uint32_t kRowPanel = 0x833BB728;  // the row panel being drawn: +0 style, +1 selected, +2 x, +4 y (s16), +16 slide-in, +24 slider
    constexpr uint32_t kRowSlideIn = 0x833BB6E0; // the submenu's rows' slide-in (floats, 0-1)
    // The sliders' values, applied through a gamma ramp, and the save's settings that keep them in bytes 24-26:
    // gamma x 10, brightness / 5 and contrast x 2.
    constexpr uint32_t kRamp = 0x834A2C28;
    constexpr uint32_t kGamma = kRamp + 1540, kBrightness = kRamp - 12, kContrast = kRamp + 1560;
    constexpr uint32_t kGameSettings = 0x82E95C64; // -> the save's settings
    constexpr uint32_t kDevice = 0x83DC4F30;       // -> D3D's device
    // Floats in .rdata: each slider's maximum, minimum and step; the brightness byte's scale (0.2); 1.0; the sliders'
    // scale (100), and the value text's offset in the panel (18).
    constexpr uint32_t kMaximum[3] = { 0x821CE4A4, 0x822A77C4, 0x821CE7D4 };
    constexpr uint32_t kMinimum[3] = { 0x82063DE0, 0x822897E0, 0x82200960 };
    constexpr uint32_t kStep[3] = { 0x82064494, 0x821CE610, 0x820BD90C };
    constexpr uint32_t kBrightnessScale = 0x8228A5C0;
    constexpr uint32_t kOne = 0x82063DE0, kSliderScale = 0x823E9438, kValueOffset = 0x821CE688;
    constexpr uint32_t kValueFormat = 0x823BCEF4; // L"%2.1f"
    constexpr uint32_t kSounds = 0x839866A0, kSoundBank = 0x83986698; // -> sound IDs: +84 a slider's step
    constexpr uint32_t kGammaDescription = 858;   // "Sets the gamma value."
    // The Language submenu's values: centered at x 260 in the panel's 345, arrows (sprites 329 and 330) at 0.35 scale
    // 2 below the text's y, 12 before and 18 after it.
    constexpr int32_t kValueX = 260;
    constexpr uint32_t kLeftArrow = 329, kRightArrow = 330;

    float LoadF32(uint32_t address)
    {
        return std::bit_cast<float>(guest::LoadU32(address));
    }

    void StoreF32(uint32_t address, float value)
    {
        guest::StoreU32(address, std::bit_cast<uint32_t>(value));
    }

    uint16_t LoadU16(uint32_t address)
    {
        const uint8_t* bytes = guest::ToHost(address);
        return uint16_t(bytes[0] << 8 | bytes[1]);
    }

    void StoreU16(uint32_t address, uint16_t value)
    {
        guest::BeforeHostWrite(address, 2);
        uint8_t* bytes = guest::ToHost(address);
        bytes[0] = uint8_t(value >> 8);
        bytes[1] = uint8_t(value);
    }

    void StoreU8(uint32_t address, uint8_t value)
    {
        guest::BeforeHostWrite(address, 1);
        *guest::ToHost(address) = value;
    }

    // Calls guest code with integer arguments from r3 (floats go in ctx.f1 and on beforehand); returns r3.
    uint32_t Call(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> arguments = {})
    {
        PPCRegister* const registers[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
        size_t i = 0;
        for (const uint64_t argument : arguments)
            registers[i++]->u64 = argument;
        function(ctx, base);
        return ctx.r3.u32;
    }

    // Plays a menu sound: its ID's offset in the sound table (84: a slider's step).
    void PlaySound(PPCContext& ctx, uint8_t* base, uint32_t offset)
    {
        ctx.f1.f64 = LoadF32(kOne);
        Call(ctx, base, sub_82666EB0, { guest::LoadU32(kSoundBank), guest::LoadU32(guest::LoadU32(kSounds) + offset), 1, ~0ull });
    }

    // Text for guest code: UTF-16BE strings in guest memory, kept for the process (they're few and repeat).
    uint32_t GuestText(const std::u16string& text)
    {
        static std::map<std::u16string, uint32_t> strings;
        static uint32_t block = 0, used = 0, size = 0;
        if (const auto it = strings.find(text); it != strings.end())
            return it->second;
        const uint32_t bytes = uint32_t(text.size() + 1) * 2;
        if (!block || used + bytes > size)
        {
            size = std::max(bytes, 0x1000u);
            block = guest::SystemAlloc(size);
            used = 0;
            if (!block)
                return 0;
        }
        const uint32_t address = block + used;
        for (size_t i = 0; i < text.size(); i++)
            StoreU16(address + uint32_t(i) * 2, text[i]);
        StoreU16(address + uint32_t(text.size()) * 2, 0);
        used += (bytes + 3) & ~3u;
        strings.emplace(text, address);
        return address;
    }

    std::u16string Format(const char* format, ...)
    {
        char text[256];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(text, sizeof(text), format, arguments);
        va_end(arguments);
        return std::u16string(text, text + strlen(text));
    }

    // The host's options: each a list of choices that left and right step through.
    struct Option
    {
        const char* label;
        uint32_t choices;
        uint32_t (*get)(const settings::Graphics& graphics);
        void (*set)(settings::Graphics& graphics, uint32_t choice);
        std::u16string (*value)(uint32_t choice);
        std::u16string (*description)(uint32_t choice);
    };

    constexpr uint32_t kSamples[] = { 0, 4, 8 };
    constexpr uint32_t kAnisotropy[] = { 0, 2, 4, 8, 16 };

    template <size_t N>
    uint32_t IndexOf(const uint32_t (&values)[N], uint32_t value)
    {
        return uint32_t(std::find(values, values + N, value) - values) % N;
    }

    std::u16string OffOn(uint32_t choice)
    {
        return Format(choice ? "On" : "Off");
    }

    const Option kOptions[] = {
        {
            "Fullscreen", 2,
            [](const settings::Graphics& graphics) { return graphics.fullscreen ? 1u : 0u; },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.fullscreen = choice != 0; },
            OffOn,
            [](uint32_t choice) {
                return choice ? Format("Fills the screen with a borderless window. Alt+Enter switches back.")
                              : Format("Shows the game in a window. Alt+Enter switches to fullscreen.");
            },
        },
        {
            "Resolution", 4,
            [](const settings::Graphics& graphics) { return graphics.resolutionScale - 1; },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.resolutionScale = choice + 1; },
            [](uint32_t choice) { return Format("%ux", choice + 1); },
            [](uint32_t choice) {
                return choice == 0 ? Format("Renders at 1280x720, the original resolution.")
                                   : Format("Renders at %ux%u, sharper but harder on the graphics card.", 1280 * (choice + 1), 720 * (choice + 1));
            },
        },
        {
            "Anti-Aliasing", 3,
            [](const settings::Graphics& graphics) { return IndexOf(kSamples, graphics.msaaSamples); },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.msaaSamples = kSamples[choice]; },
            [](uint32_t choice) { return Format("%ux", choice == 0 ? 2 : kSamples[choice]); },
            [](uint32_t choice) {
                return choice == 0 ? Format("Smooths polygon edges with 2x MSAA, as the original.")
                                   : Format("Smooths polygon edges with %ux MSAA.", kSamples[choice]);
            },
        },
        {
            "Alpha to Coverage", 2,
            [](const settings::Graphics& graphics) { return graphics.alphaToCoverage ? 1u : 0u; },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.alphaToCoverage = choice != 0; },
            OffOn,
            [](uint32_t) { return Format("Smooths the edges of leaves and other cut-out shapes with MSAA."); },
        },
        {
            "Texture Filtering", 5,
            [](const settings::Graphics& graphics) { return IndexOf(kAnisotropy, graphics.anisotropy); },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.anisotropy = kAnisotropy[choice]; },
            [](uint32_t choice) { return choice == 0 ? Format("Off") : Format("%ux", kAnisotropy[choice]); },
            [](uint32_t choice) {
                return choice == 0 ? Format("Filters textures as the original.")
                                   : Format("Keeps textures sharp at an angle with %ux anisotropic filtering.", kAnisotropy[choice]);
            },
        },
    };
    constexpr uint32_t kHostRows = uint32_t(std::size(kOptions));
    constexpr uint32_t kRows = 5 + kHostRows;

    // The host's option on a row of the submenu, or null.
    const Option* HostOption(uint32_t row)
    {
        return row >= kSliders && row < kSliders + kHostRows ? &kOptions[row - kSliders] : nullptr;
    }

    // The description box's text for the gamma slider's message, while it shows a host option's.
    uint32_t g_description = 0;

    // The submenu's y while OPTIONS isn't shown (the layout is also another screen's).
    uint16_t g_layoutY = 0;
}

// A menu's row count (menu r3).
PPC_FUNC(sub_82591B68)
{
    if (ctx.r3.u32 == kVideoMenu)
    {
        ctx.r3.u64 = kRows;
        return;
    }
    __imp__sub_82591B68(ctx, base);
}

// A menu row's label (row r3 of menu r4): the game's message for it, in the game's language.
PPC_FUNC(sub_825919B0)
{
    if (ctx.r4.u32 == kVideoMenu)
    {
        const uint32_t row = ctx.r3.u32;
        if (const Option* option = HostOption(row))
        {
            ctx.r3.u64 = GuestText(Format("%s", option->label));
            return;
        }
        if (row >= kSliders + kHostRows)
            ctx.r3.u64 = row - kHostRows;
    }
    __imp__sub_825919B0(ctx, base);
}

// The text of message r3 in language r4 (255: the game's).
PPC_FUNC(sub_824C4818)
{
    if (g_description && ctx.r3.u32 == kGammaDescription)
    {
        ctx.r3.u64 = g_description;
        return;
    }
    __imp__sub_824C4818(ctx, base);
}

// The OPTIONS screen's description box (submenu r3, row r4, fade f1): a host option's description is shown as the
// sliders' are (with the same button prompt), with its own text.
PPC_FUNC(sub_824D2F70)
{
    if (ctx.r3.u32 == kVideoMenu)
    {
        const uint32_t row = ctx.r4.u32;
        if (const Option* option = HostOption(row))
        {
            g_description = GuestText(option->description(option->get(settings::GetGraphics())));
            ctx.r4.u64 = 0;
            __imp__sub_824D2F70(ctx, base);
            g_description = 0;
            return;
        }
        if (row >= kSliders + kHostRows)
            ctx.r4.u64 = row - kHostRows;
    }
    __imp__sub_824D2F70(ctx, base);
}

// The OPTIONS screen's start and end: the submenu moves a row up for the host's rows while it's shown.
PPC_FUNC(sub_824D5070)
{
    __imp__sub_824D5070(ctx, base);
    if (!g_layoutY)
        g_layoutY = LoadU16(kLayout + 14);
    StoreU16(kLayout + 14, uint16_t(g_layoutY - LoadU16(kLayout + 20)));
}

PPC_FUNC(sub_824D5870)
{
    if (g_layoutY)
        StoreU16(kLayout + 14, g_layoutY);
    __imp__sub_824D5870(ctx, base);
}

// OPTIONS > Video Settings' update, each frame (from the OPTIONS update): the cursor, a slider or a host option, then
// the sliders' values kept and applied as the game does, and Exit or B (back to the OPTIONS list) or Default.
PPC_FUNC(sub_825901E8)
{
    const uint32_t caller = ctx.r1.u32;
    ctx.r1.u64 = caller - 128;
    guest::StoreU32(ctx.r1.u32, caller);
    const uint32_t value = ctx.r1.u32 + 80; // a slider's value, for the game's slider function

    const uint32_t level = LoadU16(kLevel);
    const uint32_t cursor = kCursors + level * 2;
    float values[kSliders] = { LoadF32(kGamma), LoadF32(kBrightness), LoadF32(kContrast) };
    Call(ctx, base, sub_8258F370, { cursor, kRows }); // up and down
    const uint32_t row = LoadU16(cursor);
    if (row < kSliders)
    {
        StoreF32(value, values[row]);
        ctx.f1.f64 = LoadF32(kMaximum[row]);
        ctx.f2.f64 = LoadF32(kMinimum[row]);
        ctx.f3.f64 = LoadF32(kStep[row]);
        Call(ctx, base, sub_8258F638, { value });
        values[row] = LoadF32(value);
    }
    else if (const Option* option = HostOption(row))
    {
        // Right (first) or left steps through the choices, stopping at the ends, as the sliders do.
        settings::Graphics graphics = settings::GetGraphics();
        const uint32_t choice = option->get(graphics);
        uint32_t next = choice;
        if (Call(ctx, base, sub_8258F2A8))
            next = std::min(choice + 1, option->choices - 1);
        else if (Call(ctx, base, sub_8258F1E0))
            next = choice > 0 ? choice - 1 : 0;
        if (next != choice)
        {
            option->set(graphics, next);
            settings::SetGraphics(graphics);
            PlaySound(ctx, base, 84);
            guest::StoreU32(kRefreshDescription, 1);
        }
    }

    // Keep the values, in the save's settings too, and apply them.
    StoreF32(kGamma, values[0]);
    StoreF32(kContrast, values[2]);
    StoreF32(kBrightness, values[1]);
    const uint32_t game = guest::LoadU32(kGameSettings);
    StoreU8(game + 24, uint8_t(int64_t(values[0] * LoadF32(kMaximum[2]))));
    StoreU8(game + 25, uint8_t(int32_t(values[1] * LoadF32(kBrightnessScale))));
    StoreU8(game + 26, uint8_t(int32_t(values[2] * LoadF32(kMaximum[0]))));
    Call(ctx, base, sub_8258BF50);
    Call(ctx, base, sub_82703A48, { guest::LoadU32(kDevice), 0, kRamp });

    const uint32_t current = LoadU16(cursor);
    if (Call(ctx, base, sub_8258F728, { current, kRows })) // B, or A on Exit
    {
        guest::StoreU32(kSubmenu, 0);
        StoreU16(kLevel, uint16_t(LoadU16(kLevel) - 1));
        settings::Save();
    }
    else if (Call(ctx, base, sub_8258F9E8, { current, kRows })) // A on Default
    {
        StoreU8(game + 25, 0);
        StoreU8(game + 26, 0);
        StoreU8(game + 24, 10);
        ctx.f1.f64 = float(10) * LoadF32(kStep[0]);
        ctx.f2.f64 = float(0) * LoadF32(kStep[2]);
        ctx.f3.f64 = float(0) * LoadF32(kStep[1]);
        Call(ctx, base, sub_8258BEF8);
        settings::SetGraphics(settings::Graphics{});
    }
    ctx.r1.u64 = caller;
}

// OPTIONS > Video Settings' draw, each frame (task r3: +16 fade; r4 3 when it has the focus): the sliders with their
// values and the host's values with their arrows, then the rows' panels and labels (the game's code, for all ten rows).
// Returns 1 once the rows have slid in.
PPC_FUNC(sub_824D40D0)
{
    const uint32_t task = ctx.r3.u32;
    const uint32_t focused = ctx.r4.u32 == 3 ? 1 : 0;
    const uint32_t caller = ctx.r1.u32;
    ctx.r1.u64 = caller - 416;
    guest::StoreU32(ctx.r1.u32, caller);
    const uint32_t text = ctx.r1.u32 + 112;  // a slider's value (32 characters)
    const uint32_t item = ctx.r1.u32 + 192;  // a text item (36 bytes)
    const uint32_t arrow = ctx.r1.u32 + 240; // a sprite item (40 bytes)
    const uint32_t width = ctx.r1.u32 + 288; // a text to measure (12 bytes)

    const uint32_t cursor = LoadU16(kCursors + 2);
    uint32_t done = 0;
    if (const uint32_t frames = guest::LoadU32(kLayout + 24); frames < 3)
        guest::StoreU32(kLayout + 24, frames + 1);
    else
        done = 1;
    Call(ctx, base, sub_824D22E0, { kLayout, cursor, 1, 1 });

    const float fade = LoadF32(task + 16);
    const float one = LoadF32(kOne);
    const float values[kSliders] = { LoadF32(kGamma), LoadF32(kBrightness), LoadF32(kContrast) };
    const float scale = LoadF32(kSliderScale);
    const float fractions[kSliders] = {
        (values[0] - one) * scale,
        (values[1] + LoadF32(kMaximum[1])) * 0.01f * scale,
        (values[2] + LoadF32(kMaximum[2])) * 0.05f * scale,
    };
    const auto alphaOf = [&](uint32_t row) {
        ctx.f1.f64 = fade;
        Call(ctx, base, sub_824D15A8, { kLayout, kLayout, row, 1 });
        return float(ctx.f1.f64);
    };
    const auto colorOf = [&](float alpha) {
        ctx.f1.f64 = one;
        ctx.f2.f64 = alpha;
        return Call(ctx, base, sub_824C48C8, { 0xFFFFFFFFu });
    };

    // The sliders: their slider bars, and their values right-aligned in the panel.
    for (uint32_t row = 0; row < kSliders; row++)
    {
        guest::StoreU32(kRowPanel + 28, 0);
        StoreU8(kRowPanel + 1, row == cursor);
        StoreF32(kRowPanel + 24, fractions[row]);
        Call(ctx, base, sub_824D1D60, { task, kLayout, row, focused, 0 });
        ctx.f1.f64 = values[row];
        Call(ctx, base, sub_82781950, { text, kValueFormat, std::bit_cast<uint64_t>(double(values[row])) });
        const float slideIn = LoadF32(kRowPanel + 16);
        const float x = float(int16_t(LoadU16(kRowPanel + 2)));
        const float y = float(int16_t(LoadU16(kRowPanel + 4)));
        StoreU16(item + 0, 36);
        StoreU16(item + 2, 265);
        guest::StoreU32(item + 4, text);
        StoreF32(item + 8, slideIn * LoadF32(kValueOffset) + x);
        StoreF32(item + 12, y);
        StoreU8(item + 20, 144);
        StoreU8(item + 21, *guest::ToHost(kRowPanel));
        StoreU8(item + 22, 1);
        StoreU8(item + 23, 0xF0);
        StoreF32(item + 24, 0.0f);
        StoreF32(item + 28, slideIn);
        StoreF32(item + 32, slideIn);
        guest::StoreU32(item + 16, colorOf(alphaOf(row)));
        Call(ctx, base, sub_824DF368, { item, 0 });
    }

    // The host's options: their values as the Language submenu's, with arrows on the cursor's row.
    const settings::Graphics graphics = settings::GetGraphics();
    const float layoutX = float(int16_t(LoadU16(kLayout + 12)));
    const int32_t halfWidth = int16_t(LoadU16(kLayout + 16)) / 2;
    for (uint32_t row = kSliders; row < kSliders + kHostRows; row++)
    {
        const Option& option = *HostOption(row);
        const uint32_t value = GuestText(option.value(option.get(graphics)));
        const float slideIn = LoadF32(kRowSlideIn + row * 4);
        const float y = float(int16_t(LoadU16(kLayout + 14)) + int16_t(LoadU16(kLayout + 20)) * int32_t(row));
        const float alpha = alphaOf(row);
        StoreU16(item + 0, 36);
        StoreU16(item + 2, 265);
        guest::StoreU32(item + 4, value);
        StoreF32(item + 8, float(kValueX - halfWidth) * slideIn + layoutX);
        StoreF32(item + 12, y);
        guest::StoreU32(item + 16, colorOf(alpha));
        StoreU8(item + 20, 80);
        StoreU8(item + 21, 3);
        StoreU8(item + 22, 1);
        StoreU8(item + 23, 0x90);
        StoreF32(item + 24, float(kValueX - 50) * slideIn);
        StoreF32(item + 28, slideIn);
        StoreF32(item + 32, slideIn);
        Call(ctx, base, sub_824DF368, { item, 0 });
        if (row != cursor)
            continue;

        // The arrows, as far apart as the value is wide.
        guest::StoreU32(width + 0, 0);
        guest::StoreU32(width + 4, value);
        StoreU8(width + 8, 1);
        StoreU8(width + 9, 1);
        StoreU8(width + 10, 1);
        StoreU8(width + 11, 0);
        ctx.f1.f64 = one;
        Call(ctx, base, sub_824E2410, { width });
        const float span = float(ctx.f1.f64);
        const uint32_t color = uint32_t(uint8_t(int64_t(alpha * LoadF32(0x82289470)))) << 24 | 0x00FFFFFF; // alpha x 255
        for (const uint32_t sprite : { kLeftArrow, kRightArrow })
        {
            const float offset = sprite == kLeftArrow ? -span / 2 - 12 : span / 2 + 18;
            for (uint32_t i = 0; i < 40; i += 4)
                guest::StoreU32(arrow + i, 0);
            StoreU16(arrow + 0, 40);
            StoreU16(arrow + 2, 257);
            guest::StoreU32(arrow + 4, sprite);
            StoreF32(arrow + 12, (float(kValueX - halfWidth) + offset) * slideIn + layoutX);
            StoreF32(arrow + 16, y + LoadF32(kMaximum[0])); // 2 below
            StoreU8(arrow + 20, 1);
            StoreU8(arrow + 21, 80);
            StoreU8(arrow + 23, 12);
            StoreF32(arrow + 24, slideIn * LoadF32(0x821CD678)); // 0.35
            StoreF32(arrow + 28, slideIn * LoadF32(0x821CD678));
            guest::StoreU32(arrow + 32, 0xFFFFFFFF);
            guest::StoreU32(arrow + 36, color);
            Call(ctx, base, sub_824DF090, { arrow, 0, 16, 0 });
        }
    }

    // Every row's panel, then the labels.
    Call(ctx, base, sub_824D2CF8, { task, kLayout, cursor, focused });
    ctx.f1.f64 = fade;
    Call(ctx, base, sub_824D24E0, { kLayout, kVideoMenu, cursor, 1 });
    ctx.r1.u64 = caller;
    ctx.r3.u64 = done;
}
