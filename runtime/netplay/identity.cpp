#include "netplay/identity.h"

#include "config.h"
#include "log.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <intrin.h>
#include <windows.h>

#ifndef GAME_BUILD_CONFIG
#define GAME_BUILD_CONFIG "unknown"
#endif

namespace netplay::identity
{
    namespace
    {
        bool ReadFileBytes(const wchar_t* path, std::vector<uint8_t>& data)
        {
            const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            LARGE_INTEGER size{};
            bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 31);
            if (ok)
            {
                data.resize(size_t(size.QuadPart));
                DWORD read = 0;
                ok = ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr) && read == data.size();
            }
            CloseHandle(file);
            return ok;
        }

        // The file offset of an RVA, through the section headers.
        std::optional<size_t> FileOffset(const IMAGE_NT_HEADERS64& nt, uint32_t rva)
        {
            const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(&nt);
            for (uint32_t i = 0; i < nt.FileHeader.NumberOfSections; i++, section++)
            {
                if (rva >= section->VirtualAddress && rva < section->VirtualAddress + section->SizeOfRawData)
                    return size_t(section->PointerToRawData) + (rva - section->VirtualAddress);
            }
            return std::nullopt;
        }

        // Blanks what differs between links of the same code (BuildId).
        void BlankLinkDetails(std::vector<uint8_t>& data)
        {
            if (data.size() < sizeof(IMAGE_DOS_HEADER))
                return;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > data.size())
                return;
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(data.data() + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                return;
            nt->FileHeader.TimeDateStamp = 0;
            nt->OptionalHeader.CheckSum = 0;
            const IMAGE_DATA_DIRECTORY debug = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            const std::optional<size_t> offset = FileOffset(*nt, debug.VirtualAddress);
            if (!debug.VirtualAddress || !offset || *offset + debug.Size > data.size())
                return;
            for (size_t entry = *offset; entry + sizeof(IMAGE_DEBUG_DIRECTORY) <= *offset + debug.Size; entry += sizeof(IMAGE_DEBUG_DIRECTORY))
            {
                const auto* directory = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(data.data() + entry);
                if (directory->PointerToRawData && size_t(directory->PointerToRawData) + directory->SizeOfData <= data.size())
                    memset(data.data() + directory->PointerToRawData, 0, directory->SizeOfData);
                memset(data.data() + entry, 0, sizeof(IMAGE_DEBUG_DIRECTORY));
            }
        }

        // The disc image's hash: 0 not looked for yet, 1 being computed, 2 known.
        std::atomic<int> g_discState{ 0 };
        std::atomic<uint64_t> g_discHash{ 0 };

        std::string CachePath()
        {
            return config::DataDirectory() + "/disc_hash.txt";
        }

        // disc_hash.txt: a line per disc image, "<size> <modification time> <hash> <path>" (the time as a FILETIME).
        std::optional<uint64_t> CachedHash(const std::string& path, uint64_t size, uint64_t modified)
        {
            FILE* file = fopen(CachePath().c_str(), "r");
            if (!file)
                return std::nullopt;
            std::optional<uint64_t> found;
            char line[2048];
            while (!found && fgets(line, sizeof(line), file))
            {
                unsigned long long lineSize = 0, lineModified = 0, hash = 0;
                int consumed = 0;
                if (sscanf(line, "%llu %llu %llx %n", &lineSize, &lineModified, &hash, &consumed) < 3 || consumed <= 0)
                    continue;
                std::string linePath = line + consumed;
                while (!linePath.empty() && (linePath.back() == '\n' || linePath.back() == '\r'))
                    linePath.pop_back();
                if (lineSize == size && lineModified == modified && _stricmp(linePath.c_str(), path.c_str()) == 0)
                    found = hash;
            }
            fclose(file);
            return found;
        }

        void CacheHash(const std::string& path, uint64_t size, uint64_t modified, uint64_t hash)
        {
            std::vector<std::string> kept; // the other images' lines
            if (FILE* file = fopen(CachePath().c_str(), "r"))
            {
                char line[2048];
                while (fgets(line, sizeof(line), file))
                {
                    unsigned long long a = 0, b = 0, c = 0;
                    int consumed = 0;
                    if (sscanf(line, "%llu %llu %llx %n", &a, &b, &c, &consumed) < 3 || consumed <= 0)
                        continue;
                    std::string linePath = line + consumed;
                    while (!linePath.empty() && (linePath.back() == '\n' || linePath.back() == '\r'))
                        linePath.pop_back();
                    if (_stricmp(linePath.c_str(), path.c_str()) != 0)
                        kept.push_back(line);
                }
                fclose(file);
            }
            FILE* file = fopen(CachePath().c_str(), "w");
            if (!file)
                return;
            for (const std::string& line : kept)
                fputs(line.c_str(), file);
            fprintf(file, "%llu %llu %016llX %s\n", (unsigned long long)size, (unsigned long long)modified, (unsigned long long)hash, path.c_str());
            fclose(file);
        }

        // Hashes the whole image, unbuffered (so as not to push the game's own reads out of the file cache).
        std::optional<uint64_t> HashImage(const std::string& path, uint64_t size)
        {
            const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_NO_BUFFERING, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return std::nullopt;
            constexpr DWORD kChunk = 8 << 20; // a multiple of any sector size
            void* buffer = VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            XXH3_state_t* state = XXH3_createState();
            bool ok = buffer && state && XXH3_64bits_reset(state) == XXH_OK;
            uint64_t total = 0;
            while (ok)
            {
                DWORD read = 0;
                if (!ReadFile(file, buffer, kChunk, &read, nullptr))
                    ok = false;
                else if (read == 0)
                    break;
                else
                {
                    XXH3_64bits_update(state, buffer, read);
                    total += read;
                }
            }
            const uint64_t hash = ok && state ? XXH3_64bits_digest(state) : 0;
            if (state)
                XXH3_freeState(state);
            if (buffer)
                VirtualFree(buffer, 0, MEM_RELEASE);
            CloseHandle(file);
            if (!ok || total != size)
                return std::nullopt;
            return hash;
        }
    }

    uint64_t BuildId()
    {
        static const uint64_t id = [] {
            wchar_t path[MAX_PATH];
            const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::vector<uint8_t> data;
            if (!length || length >= MAX_PATH || !ReadFileBytes(path, data))
            {
                LOG_WARN("Netplay: can't read the game's executable to identify its build");
                return uint64_t(0);
            }
            BlankLinkDetails(data);
            uint64_t hash = XXH3_64bits(data.data(), data.size());
            // And the game's code module next to it (ANALYSIS.md, Phase 5, The game's code module), which every PC builds
            // from its own copy of the game, byte for byte the same file for the same build and disc. Without one (the
            // runtime's tests), the executable's alone.
            std::wstring module(path);
            module = module.substr(0, module.find_last_of(L"\\/") + 1) + L"" GAME_EXECUTABLE "_ppc.dll";
            if (std::vector<uint8_t> code; ReadFileBytes(module.c_str(), code))
            {
                BlankLinkDetails(code);
                hash = XXH3_64bits_withSeed(code.data(), code.size(), hash);
            }
            return hash;
        }();
        return id;
    }

    const char* BuildConfiguration()
    {
        return GAME_BUILD_CONFIG;
    }

    void StartDiscHash()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            const std::string path = config::DiscImage();
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (path.empty() || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attributes))
            {
                LOG_WARN("Netplay: can't find the disc image (\"%s\") to identify it", path.c_str());
                g_discState = 2;
                return;
            }
            const uint64_t size = uint64_t(attributes.nFileSizeHigh) << 32 | attributes.nFileSizeLow;
            const uint64_t modified = uint64_t(attributes.ftLastWriteTime.dwHighDateTime) << 32 | attributes.ftLastWriteTime.dwLowDateTime;
            if (const std::optional<uint64_t> cached = CachedHash(path, size, modified))
            {
                g_discHash = *cached;
                g_discState = 2;
                LOG_INFO("Netplay: the disc image's hash is %016llX (from %s)", (unsigned long long)*cached, CachePath().c_str());
                return;
            }
            g_discState = 1;
            LOG_INFO("Netplay: hashing the disc image %s (%.1f GB) to identify it; this happens once, the result is kept in %s",
                path.c_str(), size / 1e9, CachePath().c_str());
            std::thread([path, size, modified] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                const auto begin = std::chrono::steady_clock::now();
                const std::optional<uint64_t> hash = HashImage(path, size);
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
                if (hash)
                {
                    CacheHash(path, size, modified, *hash);
                    LOG_INFO("Netplay: the disc image's hash is %016llX (%.1f s)", (unsigned long long)*hash, seconds);
                }
                else
                    LOG_WARN("Netplay: can't read the disc image %s to identify it", path.c_str());
                g_discHash = hash.value_or(0);
                g_discState = 2;
            }).detach();
        });
    }

    std::optional<uint64_t> DiscHash()
    {
        if (g_discState.load() != 2)
            return std::nullopt;
        return g_discHash.load();
    }

    std::string Machine()
    {
        int info[4] = {};
        char brand[49] = {};
        __cpuid(info, int(0x80000000));
        if (uint32_t(info[0]) >= 0x80000004)
        {
            for (int i = 0; i < 3; i++)
            {
                __cpuid(info, int(0x80000002 + i));
                memcpy(brand + 16 * i, info, 16);
            }
        }
        const char* name = brand;
        while (*name == ' ')
            name++;
        for (size_t end = strlen(brand); end > 0 && brand[end - 1] == ' '; end--)
            brand[end - 1] = 0;
        __cpuid(info, 1);
        const bool fma3 = info[2] & (1 << 12);
        using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW version{ sizeof(version) };
        if (const auto get = reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
            get(&version);
        char text[160];
        snprintf(text, sizeof(text), "%s (FMA3 %s), Windows %lu.%lu.%lu", *name ? name : "unknown CPU", fma3 ? "yes" : "no",
            version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
        return text;
    }
}
