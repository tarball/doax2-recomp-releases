#include "util/write_watch.h"

#include "crash_handler.h"
#include "guest_memory.h"
#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

namespace debug
{
    namespace
    {
        uint32_t g_address = 0, g_length = 4;
        uint64_t g_frame = 0;
        bool g_boot = false;        // armed once the image is loaded instead
        uint8_t* g_page = nullptr; // host page (4 KB)
        bool g_configured = false;  // an address was given (armed at its frame)
        std::atomic<bool> g_armed{ false };
        std::atomic<int> g_chains{ 0 };
        uint64_t g_seen[64] = {}; // chain hashes logged
        thread_local bool t_stepping = false;
        thread_local bool t_report = false; // the stepped write was in range: log the value after it

        constexpr int kMaxFrames = 12;

        // The guest functions on this thread's stack at the fault (every guest call is a host call).
        int GuestChain(const CONTEXT& faultContext, uint32_t* functions)
        {
            CONTEXT context = faultContext;
            int count = 0;
            for (int depth = 0; depth < 64 && count < kMaxFrames && context.Rip; depth++)
            {
                if (const uint32_t function = crash::GuestFunctionAt(context.Rip))
                {
                    if (!count || functions[count - 1] != function)
                        functions[count++] = function;
                }
                DWORD64 imageBase = 0;
                RUNTIME_FUNCTION* entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (!entry)
                {
                    context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                    continue;
                }
                void* handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData, &establisherFrame, nullptr);
            }
            return count;
        }

        void Protect(DWORD protection)
        {
            DWORD old = 0;
            VirtualProtect(g_page, 4096, protection, &old);
        }

        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            const EXCEPTION_RECORD* record = info->ExceptionRecord;
            if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_stepping)
            {
                t_stepping = false;
                if (t_report)
                {
                    t_report = false;
                    const uint32_t bits = guest::LoadU32(g_address);
                    float value;
                    memcpy(&value, &bits, 4);
                    fprintf(stderr, "[watch] %08X = %08X (%g)\n", g_address, bits, value);
                }
                Protect(PAGE_READONLY);
                info->ContextRecord->EFlags &= ~0x100u;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->ExceptionInformation[0] != 1 || !g_armed)
                return EXCEPTION_CONTINUE_SEARCH;
            uint8_t* const address = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
            if (address < g_page || address >= g_page + 4096)
                return EXCEPTION_CONTINUE_SEARCH;

            // A write of up to 16 bytes starting here overlaps the watched range?
            const uint32_t guestAddress = uint32_t(address - guest::g_base);
            if (guestAddress < g_address + g_length && guestAddress + 16 > g_address)
            {
                uint32_t functions[kMaxFrames];
                const int count = GuestChain(*info->ContextRecord, functions);
                uint64_t hash = 1469598103934665603ull;
                for (int i = 0; i < count; i++)
                    hash = (hash ^ functions[i]) * 1099511628211ull;
                bool seen = false;
                const int logged = g_chains.load();
                for (int i = 0; i < logged && i < 64; i++)
                    seen = seen || g_seen[i] == hash;
                if (!seen && logged < 64)
                {
                    g_seen[g_chains++] = hash;
                    fprintf(stderr, "[watch] write to %08X at host %p:", guestAddress, reinterpret_cast<void*>(info->ContextRecord->Rip));
                    for (int i = 0; i < count; i++)
                        fprintf(stderr, " sub_%08X", functions[i]);
                    fprintf(stderr, "\n");
                }
                t_report = true;
            }
            Protect(PAGE_READWRITE);
            t_stepping = true;
            info->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    void InstallWriteWatch()
    {
        const char* setting = std::getenv(GAME_PREFIX "_DEBUG_WATCH_WRITE");
        if (!setting)
            return;
        char* end = nullptr;
        g_address = uint32_t(strtoul(setting, &end, 16));
        if (*end == ',')
            g_length = uint32_t(strtoul(end + 1, &end, 10));
        if (*end == ',')
        {
            if (strcmp(end + 1, "boot") == 0)
                g_boot = true;
            else
                g_frame = strtoull(end + 1, &end, 10);
        }
        g_configured = true;
        AddVectoredExceptionHandler(1, OnException);
        if (g_boot)
            LOG_INFO("write watch: %08X, %u bytes, from boot", g_address, g_length);
        else
            LOG_INFO("write watch: %08X, %u bytes, from frame %llu", g_address, g_length, static_cast<unsigned long long>(g_frame));
    }

    void WriteWatchHostWrite(uint32_t address, uint32_t length, bool done)
    {
        if (!g_armed || !g_page)
            return;
        const uint32_t page = uint32_t(g_page - guest::g_base);
        if (address >= page + 4096 || address + length <= page)
            return;
        if (done)
        {
            Protect(PAGE_READONLY);
            fprintf(stderr, "[watch] file read into %08X-%08X; %08X = %08X\n", address, address + length, g_address, guest::LoadU32(g_address));
        }
        else
            Protect(PAGE_READWRITE);
    }

    void WriteWatchOnImageLoaded()
    {
        if (!g_configured || !g_boot || g_armed)
            return;
        g_page = guest::g_base + (g_address & ~0xFFFu);
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(g_page, &memory, sizeof(memory)) || memory.State != MEM_COMMIT)
        {
            LOG_INFO("write watch: %08X isn't mapped at boot", g_address);
            return;
        }
        g_armed = true;
        Protect(PAGE_READONLY);
        LOG_INFO("write watch: armed at boot");
    }

    void WriteWatchOnFrame(uint64_t frame)
    {
        if (!g_configured || g_boot || g_armed || frame < g_frame)
            return;
        g_page = guest::g_base + (g_address & ~0xFFFu); // guest memory exists by now
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(g_page, &memory, sizeof(memory)) || memory.State != MEM_COMMIT)
        {
            static bool reported = false;
            if (!reported)
                LOG_INFO("write watch: %08X isn't mapped yet", g_address);
            reported = true;
            return;
        }
        g_armed = true;
        Protect(PAGE_READONLY);
        LOG_INFO("write watch: armed at frame %llu", static_cast<unsigned long long>(frame));
    }
}
