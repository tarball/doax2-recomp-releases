// Guest traps and jump table indices out of range, which the generated code reports (see ppc_overrides.h).
#include "crash_handler.h"
#include "guest_abi.h"
#include "log.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

namespace
{
    constexpr uint32_t kDebugPrint = 0x0FE00014; // twi 31,r0,20: the debug print service (a string at r3, r4 bytes long)

    // What a trap's instruction word says it is.
    std::string Describe(uint32_t instruction)
    {
        const uint32_t opcode = instruction >> 26;
        const uint32_t to = (instruction >> 21) & 0x1F;
        const bool immediate = opcode == 2 || opcode == 3;
        char text[64];
        if (to == 31 && immediate && ((instruction >> 16) & 0x1F) == 0)
        {
            const int code = int16_t(instruction & 0xFFFF);
            snprintf(text, sizeof(text), code == 22 ? "a debug break" : "debug service %d", code);
        }
        else
        {
            // The compiler's checks: rA <= 0 unsigned before a divide (by zero), rA >= -1 unsigned (an overflow).
            snprintf(text, sizeof(text), "a failed check (TO %u)", to);
        }
        return text;
    }
}

// PPC_TRAP: a tw, twi, td or tdi whose condition held. The debug print service is printed; anything else is reported
// once per site with its guest call chain: a debug break or a failed check in the game, which on the console would
// have stopped it, so most likely a bug in its recompilation or in the runtime. The game goes on.
void GuestTrap(PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t instruction)
{
    (void)base;
    if (instruction == kDebugPrint)
    {
        static std::atomic<uint32_t> printed{ 0 };
        if (printed.fetch_add(1, std::memory_order_relaxed) >= 1000)
            return;
        const uint32_t length = ctx.r4.u32 < 1024 ? ctx.r4.u32 : 1024;
        std::string text(length, '\0');
        if (ctx.r3.u32 && length)
            memcpy(text.data(), guest::ToHost(ctx.r3.u32), length);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '\0'))
            text.pop_back();
        LOG_INFO("guest: %s", text.c_str());
        return;
    }

    static std::mutex lock;
    static std::unordered_set<uint32_t> reported;
    std::lock_guard guard(lock);
    if (!reported.insert(address).second)
        return;
    LOG_WARN("Guest trap at %08X: %s (instruction %08X); called from:", address, Describe(instruction).c_str(), instruction);
    guest::PrintBacktrace(ctx.r1.u32, 8);
}

// PPC_SWITCH_OUT_OF_RANGE: a jump table's index beyond the table, which the guest's bounds check should have excluded. The
// game ends with a message (crash::Fatal), as for the other errors it can't go on from.
[[noreturn]] void GuestSwitchOutOfRange(uint32_t address, uint32_t index)
{
    char message[160];
    snprintf(message, sizeof(message), "Jump table at %08X: index %u is beyond the table, and the game can't go on.", address, index);
    crash::Fatal(message);
}
