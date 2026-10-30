// The executable module: XEX header in guest memory, its loader entry, and the exports that read them.
#include "kernel/module.h"

#include "guest_abi.h"
#include "imports.h"
#include "kernel/status.h"
#include "log.h"

#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace
{
    constexpr uint32_t kXexHeaderSystemFlags = 0x00030000;
    constexpr uint32_t kXexHeaderEntryPoint = 0x00010100;

    struct UnicodeString
    {
        be<uint16_t> length;
        be<uint16_t> maximumLength;
        be<uint32_t> buffer;
    };

    struct AnsiString
    {
        be<uint16_t> length;
        be<uint16_t> maximumLength;
        be<uint32_t> buffer;
    };

    // X_LDR_DATA_TABLE_ENTRY. The game reads xexHeaderBase (0x58) through XexExecutableModuleHandle.
    struct LoaderEntry
    {
        be<uint32_t> inLoadOrderLinks[2];         // 0x00
        be<uint32_t> inMemoryOrderLinks[2];       // 0x08
        be<uint32_t> inInitializationOrderLinks[2]; // 0x10
        be<uint32_t> dllBase;                     // 0x18
        be<uint32_t> imageBase;                   // 0x1C
        be<uint32_t> imageSize;                   // 0x20
        UnicodeString fullDllName;                // 0x24
        UnicodeString baseDllName;                // 0x2C
        be<uint32_t> flags;                       // 0x34
        be<uint32_t> fullImageSize;               // 0x38
        be<uint32_t> entryPoint;                  // 0x3C
        be<uint16_t> loadCount;                   // 0x40
        be<uint16_t> moduleIndex;                 // 0x42
        be<uint32_t> dllBaseOriginal;             // 0x44
        be<uint32_t> checksum;                    // 0x48
        be<uint32_t> loadFlags;                   // 0x4C
        be<uint32_t> timeDateStamp;               // 0x50
        be<uint32_t> loadedImports;               // 0x54
        be<uint32_t> xexHeaderBase;               // 0x58
        AnsiString loadFileName;                  // 0x5C
        be<uint32_t> closureRoot;                 // 0x64
        be<uint32_t> traversalParent;             // 0x68
    };
    static_assert(sizeof(LoaderEntry) == 0x6C);
    static_assert(offsetof(LoaderEntry, xexHeaderBase) == 0x58);

    uint32_t g_xexHeader = 0;

    // Writes a big-endian UTF-16 copy of an ASCII string and returns its guest address.
    uint32_t StoreWideString(uint32_t address, const char* text)
    {
        auto* out = guest::Translate<be<uint16_t>>(address);
        size_t i = 0;
        for (; text[i]; i++)
            out[i] = uint16_t(uint8_t(text[i]));
        out[i] = 0;
        return address;
    }
}

namespace kernel
{
    uint32_t ExecutableXexHeader()
    {
        return g_xexHeader;
    }

    uint32_t GetXexOptionalHeader(uint32_t xexHeader, uint32_t key)
    {
        if (!xexHeader)
            return 0;

        const uint32_t count = guest::LoadU32(xexHeader + 0x14);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t entry = xexHeader + 0x18 + i * 8;
            if (guest::LoadU32(entry) != key)
                continue;

            switch (key & 0xFF)
            {
            case 0x00:
                return guest::LoadU32(entry + 4); // value stored inline
            case 0x01:
                return entry + 4; // address of the inline value
            default:
                return xexHeader + guest::LoadU32(entry + 4); // offset to the data
            }
        }
        return 0;
    }

    bool InitializeExecutableModule(const char* xexPath, uint32_t imageBase, uint32_t imageSize)
    {
        std::ifstream file(xexPath, std::ios::binary | std::ios::ate);
        if (!file)
        {
            LOG_ERROR("Can't open %s", xexPath);
            return false;
        }
        std::vector<uint8_t> xex(size_t(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(xex.data()), std::streamsize(xex.size()));
        return file && InitializeExecutableModule(xex, imageBase, imageSize);
    }

    bool InitializeExecutableModule(const std::vector<uint8_t>& xex, uint32_t imageBase, uint32_t imageSize)
    {
        if (xex.size() < 0x18 || memcmp(xex.data(), "XEX2", 4) != 0)
        {
            LOG_ERROR("default.xex isn't an XEX2 file");
            return false;
        }

        const uint32_t headerSize = (uint32_t(xex[8]) << 24) | (uint32_t(xex[9]) << 16) | (uint32_t(xex[10]) << 8) | xex[11];
        if (headerSize > xex.size())
        {
            LOG_ERROR("default.xex is truncated");
            return false;
        }
        g_xexHeader = guest::SystemAlloc(headerSize);
        if (!g_xexHeader)
            return false;
        memcpy(guest::ToHost(g_xexHeader), xex.data(), headerSize);

        // Loader entry plus its name strings in one allocation.
        const uint32_t entryAddress = guest::SystemAlloc(0x200);
        if (!entryAddress)
            return false;
        auto* entry = guest::Translate<LoaderEntry>(entryAddress);
        const uint32_t nameAddress = StoreWideString(entryAddress + 0x100, "default.xex");
        constexpr uint16_t kNameBytes = 11 * 2;

        // A single-module list: links point back at the entry itself.
        for (be<uint32_t>* links : { entry->inLoadOrderLinks, entry->inMemoryOrderLinks, entry->inInitializationOrderLinks })
        {
            const uint32_t self = guest::ToGuest(links);
            links[0] = self;
            links[1] = self;
        }
        entry->dllBase = imageBase;
        entry->imageBase = imageBase;
        entry->imageSize = imageSize;
        entry->fullDllName = { kNameBytes, kNameBytes + 2, nameAddress };
        entry->baseDllName = { kNameBytes, kNameBytes + 2, nameAddress };
        entry->fullImageSize = imageSize;
        entry->entryPoint = GetXexOptionalHeader(g_xexHeader, kXexHeaderEntryPoint);
        entry->loadCount = 1;
        entry->dllBaseOriginal = imageBase;
        entry->xexHeaderBase = g_xexHeader;

        const uint32_t handleVariable = imports::VariableAddress("XexExecutableModuleHandle");
        if (handleVariable)
            guest::StoreU32(handleVariable, entryAddress);

        LOG_INFO("Executable module: XEX header at 0x%08X (0x%X bytes), loader entry at 0x%08X", g_xexHeader, headerSize, entryAddress);
        return true;
    }
}

namespace
{
    uint32_t RtlImageXexHeaderField(uint32_t xexHeader, uint32_t key)
    {
        return kernel::GetXexOptionalHeader(xexHeader, key);
    }

    uint32_t XexCheckExecutablePrivilege(uint32_t privilege)
    {
        // Privileges 0-31 are bits of the system flags header. The 32+ range lives in a header this XEX doesn't have.
        if (privilege >= 32)
            return 0;
        const uint32_t flags = kernel::GetXexOptionalHeader(g_xexHeader, kXexHeaderSystemFlags);
        return (flags >> privilege) & 1;
    }

    // The system modules a title can ask for by name, to look up their exports by ordinal. Each has a loader entry with
    // only its names filled in as its handle. Their procedures are the title's own import thunks, so only what the title
    // imports is found: DOAX2's XNet asks XAM for NetDll_XNetStartupEx and NetDll_WSAStartupEx, which it doesn't
    // import, and then uses NetDll_XNetStartup and NetDll_WSAStartup, which it does.
    struct SystemModule
    {
        const char* fileName;
        const char* library; // as imports.inc names it
        uint32_t entry = 0;
    };
    SystemModule g_systemModules[] = { { "xam.xex", "Xam" }, { "xboxkrnl.exe", "Kernel" } };

    constexpr uint32_t kStatusDllNotFound = 0xC0000135;
    constexpr uint32_t kStatusProcedureNotFound = 0xC000007A;

    uint32_t XexGetModuleHandle(const char* name, be<uint32_t>* handle)
    {
        if (!handle)
            return status::kInvalidParameter;
        *handle = 0;
        if (!name || _stricmp(name, "default.xex") == 0)
        {
            *handle = guest::LoadU32(imports::VariableAddress("XexExecutableModuleHandle"));
            return status::kSuccess;
        }
        for (SystemModule& module : g_systemModules)
        {
            if (_stricmp(name, module.fileName) != 0)
                continue;
            static std::mutex lock;
            std::lock_guard guard(lock);
            if (!module.entry)
            {
                module.entry = guest::KernelAlloc(0x100);
                auto* entry = guest::Translate<LoaderEntry>(module.entry);
                const uint16_t nameBytes = uint16_t(strlen(module.fileName) * 2);
                const uint32_t nameAddress = StoreWideString(module.entry + sizeof(LoaderEntry), module.fileName);
                entry->fullDllName = { nameBytes, uint16_t(nameBytes + 2), nameAddress };
                entry->baseDllName = { nameBytes, uint16_t(nameBytes + 2), nameAddress };
                entry->loadCount = 1;
            }
            *handle = module.entry;
            return status::kSuccess;
        }
        LOG_WARN("XexGetModuleHandle(\"%s\"): no such module", name);
        return kStatusDllNotFound;
    }

    uint32_t XexGetProcedureAddress(uint32_t handle, uint32_t ordinal, be<uint32_t>* address)
    {
        if (!address)
            return status::kInvalidParameter;
        *address = 0;
        for (const SystemModule& module : g_systemModules)
        {
            if (!handle || handle != module.entry)
                continue;
            const uint32_t thunk = imports::FunctionThunk(module.library, ordinal);
            if (!thunk)
            {
                LOG_TRACE("XexGetProcedureAddress(%s, %u): the title doesn't import it", module.fileName, ordinal);
                return kStatusProcedureNotFound;
            }
            *address = thunk;
            return status::kSuccess;
        }
        return kStatusProcedureNotFound; // the executable exports nothing
    }
}

GUEST_EXPORT(RtlImageXexHeaderField, RtlImageXexHeaderField)
GUEST_EXPORT(XexCheckExecutablePrivilege, XexCheckExecutablePrivilege)
GUEST_EXPORT(XexGetModuleHandle, XexGetModuleHandle)
GUEST_EXPORT(XexGetProcedureAddress, XexGetProcedureAddress)
