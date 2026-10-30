// The vacation's random turns (diagnostics; DOAX2-specific addresses).
//
// Where the girls go at each time of day, and what they do there, comes from a Mersenne Twister (0x83DC6870, seeded from
// the time base at startup by sub_824707C0) that the game also draws from every frame (about 90 draws, effects and the
// like), so with scripted input on the wall clock each run takes different turns. When the time of day changes,
// sub_826CB1A0 places the girls (sub_82506938, sub_826B9290), drawing through the generator's helpers below.
//
// DEBUG_SEED=n reseeds that generator from n and the time of day whenever the time of day changes, so a seed gives the
// same turn every run: from the user's Day 3 save, the sweep's pool scene with DEBUG_SEED=7 finds Kokoro on her air
// mattress at the Pool in the daytime.
// DEBUG_RANDOM_LOG=first,count logs the helpers' draws during those frames, with their callers.
#include "gpu/command_processor.h"
#include "guest_abi.h"
#include "log.h"

#include <cstdlib>
#include <utility>

extern "C" PPC_FUNC(__imp__sub_824707C0); // the Mersenne Twister's init_genrand (r3 the generator, r4 the seed)
extern "C" PPC_FUNC(__imp__sub_826CB1A0); // the time of day changes
extern "C" PPC_FUNC(__imp__sub_8273F890); // the generator's helpers: floats, integers, and below an integer (sub_8273FB98)
extern "C" PPC_FUNC(__imp__sub_8273F948);
extern "C" PPC_FUNC(__imp__sub_8273FA10);
extern "C" PPC_FUNC(__imp__sub_8273FAB8);
extern "C" PPC_FUNC(__imp__sub_8273FB98);

namespace
{
    constexpr uint32_t kGenerator = 0x83DC6870;
    constexpr uint32_t kTimeOfDay = 0x83986D98 + 12559; // a byte: the vacation's time slot, 4 a day

    bool LogDraws()
    {
        static const std::pair<uint64_t, uint64_t> window = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_RANDOM_LOG");
            if (!setting)
                return std::pair<uint64_t, uint64_t>{ 0, 0 };
            char* end = nullptr;
            const uint64_t first = strtoull(setting, &end, 10);
            return std::pair<uint64_t, uint64_t>{ first, *end == ',' ? strtoull(end + 1, nullptr, 10) : 1 };
        }();
        if (!window.second)
            return false;
        const uint64_t frame = gpu::GetStatistics().swaps;
        return frame >= window.first && frame < window.first + window.second;
    }
}

PPC_FUNC(sub_826CB1A0)
{
    static const char* setting = std::getenv(GAME_PREFIX "_DEBUG_SEED");
    if (setting)
    {
        const uint32_t time = *guest::ToHost(kTimeOfDay);
        const uint32_t seed = uint32_t(strtoul(setting, nullptr, 10)) * 2654435761u + time;
        LOG_INFO("DEBUG_SEED: time of day %u, generator %08X seeded with %08X", time, kGenerator, seed);
        PPCContext saved = ctx;
        ctx.r3.u64 = kGenerator;
        ctx.r4.u64 = seed;
        __imp__sub_824707C0(ctx, base);
        ctx = saved;
    }
    __imp__sub_826CB1A0(ctx, base);
}

#define RANDOM_HELPER(name)                                                                                                       \
    PPC_FUNC(name)                                                                                                                \
    {                                                                                                                             \
        const uint32_t lr = uint32_t(ctx.lr), r3 = ctx.r3.u32;                                                                    \
        __imp__##name(ctx, base);                                                                                                 \
        if (LogDraws())                                                                                                           \
            LOG_INFO("RANDOM: frame %llu " #name " r3 %08X -> %08X f1 %g from %08X", (unsigned long long)gpu::GetStatistics().swaps.load(), \
                r3, ctx.r3.u32, ctx.f1.f64, lr);                                                                                  \
    }
RANDOM_HELPER(sub_8273F890)
RANDOM_HELPER(sub_8273F948)
RANDOM_HELPER(sub_8273FA10)
RANDOM_HELPER(sub_8273FAB8)
RANDOM_HELPER(sub_8273FB98)
