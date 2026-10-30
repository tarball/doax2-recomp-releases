// Loads and checks the game's code module (loader.h, ANALYSIS.md, Phase 5, The game's code module).
//
// The executable and the module import from each other: the module imports from the executable (by its file name, so
// the executable must keep its name) the runtime's functions, the kernel's and XAM's imports, the C runtime's functions
// it calls and the runtime's replacements of game functions; the executable delay-loads the module (/DELAYLOAD) for the
// generated functions the runtime calls by name. Load binds the latter up front, so a function missing from either side
// is reported here rather than at its first call.
#include "module/loader.h"

#include "disc_image.h"
#include "log.h"
#include "module/module.h"
#include "ppc_config.h"
#include "ppc_context.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <delayimp.h>

namespace module
{
    namespace
    {
        constexpr char kFileNameA[] = GAME_EXECUTABLE "_ppc.dll";
        constexpr wchar_t kFileName[] = L"" GAME_EXECUTABLE "_ppc.dll";
        constexpr char kSignature[] = "@@YAXAEIAUPPCContext@@PEAE@Z"; // a generated function's, in the Microsoft C++ ABI

        HMODULE g_module = nullptr;
        std::vector<std::string> g_missing; // the runtime's calls into the module that it doesn't export

        // A function the runtime calls in the module that the module doesn't have: bound to this, so the rest can be
        // bound and all of them reported (Load refuses the module, so this never runs).
        [[noreturn]] void MissingFunction()
        {
            fprintf(stderr, "A function missing from the game's code was called\n");
            fflush(stderr);
            __fastfail(FAST_FAIL_FATAL_APP_EXIT);
        }

        FARPROC WINAPI OnDelayLoadNotify(unsigned notify, PDelayLoadInfo info)
        {
            // The module Load loaded, whatever the search order would find.
            if (notify == dliNotePreLoadLibrary && g_module && _stricmp(info->szDll, kFileNameA) == 0)
                return reinterpret_cast<FARPROC>(g_module);
            return nullptr;
        }

        FARPROC WINAPI OnDelayLoadFailure(unsigned notify, PDelayLoadInfo info)
        {
            if (notify == dliFailGetProc && _stricmp(info->szDll, kFileNameA) == 0)
            {
                char ordinal[32];
                snprintf(ordinal, sizeof(ordinal), "#%lu", info->dlp.dwOrdinal);
                g_missing.push_back(info->dlp.fImportByName ? info->dlp.szProcName : ordinal);
                return reinterpret_cast<FARPROC>(&MissingFunction);
            }
            return nullptr;
        }

        std::wstring ExecutablePath()
        {
            std::wstring path(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
                if (length == 0)
                    return {};
                if (length < path.size())
                {
                    path.resize(length);
                    return path;
                }
                path.resize(path.size() * 2);
            }
        }

        // The code an import thunk jumps to (jmp qword ptr [rip+disp32], as lld makes them), or null if `code` isn't one.
        const void* ThunkTarget(const void* code)
        {
            const auto* bytes = static_cast<const uint8_t*>(code);
            if (bytes[0] != 0xFF || bytes[1] != 0x25)
                return nullptr;
            int32_t displacement;
            memcpy(&displacement, bytes + 2, sizeof(displacement));
            return *reinterpret_cast<const void* const*>(bytes + 6 + displacement);
        }

        struct Replacement
        {
            uint32_t guest;
            const void* host;
        };

        // The executable's exports that replace game functions, ?sub_XXXXXXXX@@<signature> (tools/moduledefs.py), from
        // its export directory.
        std::vector<Replacement> ExportedReplacements()
        {
            std::vector<Replacement> replacements;
            const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!directory.VirtualAddress)
                return replacements;
            const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
            const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
            const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
            const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
            for (DWORD i = 0; i < exports->NumberOfNames; i++)
            {
                const char* name = reinterpret_cast<const char*>(base + names[i]);
                if (strncmp(name, "?sub_", 5) != 0 || strlen(name) != 13 + strlen(kSignature) || strcmp(name + 13, kSignature) != 0)
                    continue;
                char* end = nullptr;
                const std::string digits(name + 5, 8);
                const unsigned long guest = strtoul(digits.c_str(), &end, 16);
                if (end != digits.c_str() + 8)
                    continue;
                replacements.push_back({ uint32_t(guest), base + functions[ordinals[i]] });
            }
            return replacements;
        }

        // What to do about a module that's missing or wrong: it comes with the executable, in a release's folder, or from
        // build.cmd, in a development checkout.
        constexpr char kReinstall[] = " It comes with the game: extract the whole release again, to a folder of its own (in a development "
                                      "checkout, build it with build.cmd).";

        // What differs between the module's info and the runtime's, or empty.
        std::string Mismatch(const PPCModuleInfo& module, const PPCModuleInfo& runtime)
        {
            std::string problems;
            const auto check = [&](const char* what, uint64_t a, uint64_t b) {
                if (a != b)
                {
                    char text[160];
                    snprintf(text, sizeof(text), "%s%s 0x%llX (the runtime's 0x%llX)", problems.empty() ? "" : ", ", what,
                        (unsigned long long)a, (unsigned long long)b);
                    problems += text;
                }
            };
            check("interface", module.interface, runtime.interface);
            if (module.interface != runtime.interface)
                return problems; // the rest may not be where this build expects
            check("options", module.configFlags, runtime.configFlags);
            check("image base", module.imageBase, runtime.imageBase);
            check("image size", module.imageSize, runtime.imageSize);
            check("code base", module.codeBase, runtime.codeBase);
            check("code size", module.codeSize, runtime.codeSize);
            check("context size", module.contextSize, runtime.contextSize);
            check("context alignment", module.contextAlign, runtime.contextAlign);
            check("r1", module.r1Offset, runtime.r1Offset);
            check("r3", module.r3Offset, runtime.r3Offset);
            check("r13", module.r13Offset, runtime.r13Offset);
            check("fpscr", module.fpscrOffset, runtime.fpscrOffset);
            check("f1", module.f1Offset, runtime.f1Offset);
            check("v0", module.v0Offset, runtime.v0Offset);
            return problems;
        }
    }

    const wchar_t* FileName()
    {
        return kFileName;
    }

    std::wstring Path()
    {
        const std::wstring executable = ExecutablePath();
        if (executable.empty())
            return kFileName;
        return (std::filesystem::path(executable).parent_path() / kFileName).wstring();
    }

    std::string Load(Loaded& loaded)
    {
        // The module imports from the executable by its name.
        const std::wstring executable = ExecutablePath();
        if (_wcsicmp(std::filesystem::path(executable).filename().c_str(), L"" GAME_EXECUTABLE ".exe") != 0)
            return "The game's executable must be named " GAME_EXECUTABLE ".exe, which the game's code calls into by name.";

        const std::wstring path = Path();
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
            return std::string("The game's code, ") + kFileNameA + ", isn't next to " GAME_EXECUTABLE ".exe." + kReinstall;
        g_module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_module)
        {
            const DWORD code = GetLastError();
            char text[256] = "";
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text, sizeof(text), nullptr);
            for (size_t length = strlen(text); length && (text[length - 1] == '\n' || text[length - 1] == '\r'); length--)
                text[length - 1] = '\0';
            // 127: it imports a function this executable doesn't export (built for another build of the game).
            return std::string("Can't load the game's code, ") + kFileNameA + ": " + text + " (error " + std::to_string(code) + ")." + kReinstall;
        }

        const auto* info = reinterpret_cast<const PPCModuleInfo*>(GetProcAddress(g_module, "PPCModule"));
        if (!info)
            return std::string(kFileNameA) + " isn't the game's code." + kReinstall;
        const PPCModuleInfo expected = PPC_MODULE_INFO(nullptr, nullptr);
        if (const std::string mismatch = Mismatch(*info, expected); !mismatch.empty())
        {
            LOG_ERROR("Game code: %s was built for another runtime: %s", kFileNameA, mismatch.c_str());
            return std::string("The game's code, ") + kFileNameA + ", was built for another build of the game." + kReinstall;
        }

        // A random stack cookie for its /GS checks, before any of its code runs (the C runtime's top 16 bits clear).
        uintptr_t cookie = 0;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&cookie), sizeof(cookie), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            cookie = uintptr_t(GetTickCount64()) ^ uintptr_t(&cookie);
        cookie &= 0x0000FFFFFFFFFFFFull;
        if (cookie)
            *info->securityCookie = cookie;

        // The generated functions the runtime calls, bound now.
        if (FAILED(__HrLoadAllImportsForDll(kFileNameA)) || !g_missing.empty())
        {
            for (const std::string& name : g_missing)
                LOG_ERROR("Game code: %s doesn't have %s, which the runtime calls", kFileNameA, name.c_str());
            return std::string("The game's code, ") + kFileNameA + ", lacks " + std::to_string(g_missing.size()) +
                " functions this build of the game calls." + kReinstall;
        }

        // Every replacement the executable exports must be what the module's code calls: its function table's entry
        // for that address is its import thunk for the executable's function. A missed one would silently run the
        // game's own code instead of the runtime's.
        std::unordered_map<uint32_t, const void*> functions;
        size_t count = 0;
        for (const PPCFuncMapping* mapping = info->functions; mapping->host; ++mapping, ++count)
            functions.emplace(uint32_t(mapping->guest), reinterpret_cast<const void*>(mapping->host));
        const std::vector<Replacement> replacements = ExportedReplacements();
        size_t missed = 0;
        for (const Replacement& replacement : replacements)
        {
            const auto it = functions.find(replacement.guest);
            const char* problem = it == functions.end() ? "has no function there"
                : ThunkTarget(it->second) != replacement.host ? "calls its own code there, not the runtime's" : nullptr;
            if (problem)
            {
                LOG_ERROR("Game code: the runtime replaces sub_%08X, but %s %s", replacement.guest, kFileNameA, problem);
                missed++;
            }
        }
        if (missed)
            return std::string("The game's code, ") + kFileNameA + ", doesn't call " + std::to_string(missed) +
                " of this build's replacements of its functions." + kReinstall;

        // The file's SHA-256, for the log: which build of the game's code this is.
        std::ifstream file(path, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        loaded.functions = info->functions;
        loaded.sha256 = disc::Sha256(bytes.data(), bytes.size());
        loaded.replacements = replacements.size();
        LOG_INFO("Game code: %s, %zu bytes, SHA-256 %s: %zu functions, %zu of them the runtime's", kFileNameA, bytes.size(),
            loaded.sha256.c_str(), count, replacements.size());
        return {};
    }
}

ExternC const PfnDliHook __pfnDliNotifyHook2 = module::OnDelayLoadNotify;
ExternC const PfnDliHook __pfnDliFailureHook2 = module::OnDelayLoadFailure;
