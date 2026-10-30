// Small kernel exports: process type, debugging, bug checks, status conversion, memory fill/compare.
#include "crash_handler.h"
#include "guest_abi.h"
#include "kernel/status.h"
#include "kernel/video_mode.h"
#include "log.h"
#include "ppc_config.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    constexpr uint32_t kProcessTypeTitle = 1;

    uint32_t KeGetCurrentProcessType()
    {
        return kProcessTypeTitle;
    }

    // Only the format string is printed; formatting guest varargs comes with the sprintf family.
    uint32_t DbgPrint(const char* format)
    {
        if (format)
        {
            std::string text(format);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            LOG_INFO("DbgPrint: %s", text.c_str());
        }
        return 0;
    }

    void DbgBreakPoint()
    {
        LOG_WARN("DbgBreakPoint");
    }

    [[noreturn]] void KeBugCheckEx(uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4)
    {
        char message[160];
        snprintf(message, sizeof(message), "The game stopped with a bug check: KeBugCheckEx(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X).", code, p1, p2,
            p3, p4);
        crash::Fatal(message);
    }

    [[noreturn]] void KeBugCheck(uint32_t code)
    {
        KeBugCheckEx(code, 0, 0, 0, 0);
    }

    // EXCEPTION_RECORD
    struct ExceptionRecord
    {
        be<uint32_t> code;
        be<uint32_t> flags;
        be<uint32_t> record;
        be<uint32_t> address;
        be<uint32_t> parameterCount;
        be<uint32_t> parameters[15];
    };

    // The type a C++ throw names: its ThrowInfo's first catchable type's TypeDescriptor (".?AVbad_alloc@std@@"), if the
    // structures are in the image where they belong.
    std::string ThrownType(uint32_t throwInfo)
    {
        const auto inImage = [](uint32_t address, uint32_t size) {
            return address >= PPC_IMAGE_BASE && uint64_t(address) + size <= PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
        };
        if (!inImage(throwInfo, 16))
            return {};
        const uint32_t catchableTypes = guest::LoadU32(throwInfo + 12);
        if (!inImage(catchableTypes, 8) || guest::LoadU32(catchableTypes) == 0)
            return {};
        const uint32_t catchableType = guest::LoadU32(catchableTypes + 4);
        if (!inImage(catchableType, 8))
            return {};
        const uint32_t typeDescriptor = guest::LoadU32(catchableType + 4);
        std::string name;
        for (uint32_t i = 0; i < 128 && inImage(typeDescriptor + 8 + i, 1); i++)
        {
            const char c = char(*guest::ToHost(typeDescriptor + 8 + i));
            if (!c)
                break;
            name += c;
        }
        return name;
    }

    // What the game raises: XAPI's heap when it's out of memory, C++ throws (the game's allocator and the standard
    // library, on their failure paths only) and the math library's floating-point errors. Nothing here runs the game's
    // handlers, and none of these would let it go on: the game ends with a report. A thread name for the debugger
    // (0x406D1388) needs no debugger, and the game goes on.
    void RtlRaiseException(const ExceptionRecord* record)
    {
        const uint32_t code = record ? uint32_t(record->code) : 0;
        if (code == 0x406D1388)
            return;
        char message[256];
        if (code == 0xE06D7363 && record->parameterCount >= 3)
        {
            const std::string type = ThrownType(record->parameters[2]);
            snprintf(message, sizeof(message), "The game threw a C++ exception (%s), which the runtime doesn't hand to the game's handlers, and can't go on.",
                type.empty() ? "of an unknown type" : type.c_str());
        }
        else if (code == status::kNoMemory)
        {
            snprintf(message, sizeof(message), "The game ran out of memory (its heap raised STATUS_NO_MEMORY), and can't go on.");
        }
        else
        {
            snprintf(message, sizeof(message), "The game raised exception 0x%08X at 0x%08X, which the runtime doesn't hand to the game's handlers, and "
                "can't go on.", code, record ? uint32_t(record->address) : 0);
        }
        crash::Fatal(message);
    }

    [[noreturn]] void HalReturnToFirmware(uint32_t routine)
    {
        LOG_INFO("HalReturnToFirmware(%u): title exited", routine);
        std::exit(0);
    }

    // Routines called when the title terminates (D3D, XAudio and the utility drive register some). The title's end
    // ends the process here, so they're never called.
    void ExRegisterTitleTerminateNotification(uint32_t /*registration*/, uint32_t /*create*/)
    {
    }

    uint32_t RtlNtStatusToDosError(uint32_t ntStatus)
    {
        switch (ntStatus)
        {
        case status::kSuccess: return 0;
        case status::kTimeout: return 1460;           // ERROR_TIMEOUT
        case status::kPending: return 997;            // ERROR_IO_PENDING
        case status::kBufferOverflow: return 234;     // ERROR_MORE_DATA
        case 0x80000006: return 18;                   // STATUS_NO_MORE_FILES -> ERROR_NO_MORE_FILES
        case status::kUnsuccessful: return 31;        // ERROR_GEN_FAILURE
        case status::kNotImplemented: return 1;       // ERROR_INVALID_FUNCTION
        case status::kInvalidHandle: return 6;        // ERROR_INVALID_HANDLE
        case status::kInvalidParameter: return 87;    // ERROR_INVALID_PARAMETER
        case status::kNoSuchFile: return 2;           // ERROR_FILE_NOT_FOUND
        case status::kEndOfFile: return 38;           // ERROR_HANDLE_EOF
        case status::kNoMemory: return 8;             // ERROR_NOT_ENOUGH_MEMORY
        case status::kAccessDenied: return 5;         // ERROR_ACCESS_DENIED
        case status::kObjectNameNotFound: return 2;   // ERROR_FILE_NOT_FOUND
        case 0xC0000035: return 183;                  // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
        case status::kObjectPathNotFound: return 3;   // ERROR_PATH_NOT_FOUND
        case status::kMemoryNotAllocated: return 487; // ERROR_INVALID_ADDRESS
        default:
            // HRESULT_FROM_WIN32 values carry the Win32 code directly.
            if ((ntStatus & 0xFFFF0000) == 0x80070000)
                return ntStatus & 0xFFFF;
            LOG_WARN("RtlNtStatusToDosError: unmapped status 0x%08X", ntStatus);
            return 317; // ERROR_MR_MID_NOT_FOUND, what NT returns for unknown codes
        }
    }

    void RtlFillMemoryUlong(be<uint32_t>* destination, uint32_t length, uint32_t pattern)
    {
        for (uint32_t i = 0; i < length / 4; i++)
            destination[i] = pattern;
    }

    uint32_t RtlCompareMemoryUlong(const be<uint32_t>* source, uint32_t length, uint32_t pattern)
    {
        uint32_t matched = 0;
        for (uint32_t i = 0; i < length / 4 && source[i] == pattern; i++)
            matched += 4;
        return matched;
    }

    // XAM system queries.

    void XGetVideoMode(kernel::VideoMode* mode)
    {
        if (mode)
            kernel::FillVideoMode(mode);
    }

    uint32_t XGetLanguage()
    {
        return 1; // English
    }

    uint32_t XGetGameRegion()
    {
        return 0x00FF; // North America, all
    }

    uint32_t XamGetSystemVersion()
    {
        return 0x20449700; // 2.0.17559.0, the final dashboard
    }

    // The AV cable: XAPI treats 3, 4, 6 and 8 as cables that can't be PAL 50 Hz. 8 as Xenia answers (HDMI).
    uint32_t XGetAVPack()
    {
        return 8;
    }
}

GUEST_EXPORT(KeGetCurrentProcessType, KeGetCurrentProcessType)
GUEST_EXPORT(DbgPrint, DbgPrint)
GUEST_EXPORT(DbgBreakPoint, DbgBreakPoint)
GUEST_EXPORT(KeBugCheckEx, KeBugCheckEx)
GUEST_EXPORT(KeBugCheck, KeBugCheck)
GUEST_EXPORT(RtlRaiseException, RtlRaiseException)
GUEST_EXPORT(HalReturnToFirmware, HalReturnToFirmware)
GUEST_EXPORT(ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotification)
GUEST_EXPORT(XGetAVPack, XGetAVPack)
GUEST_EXPORT(RtlNtStatusToDosError, RtlNtStatusToDosError)
GUEST_EXPORT(RtlFillMemoryUlong, RtlFillMemoryUlong)
GUEST_EXPORT(RtlCompareMemoryUlong, RtlCompareMemoryUlong)
GUEST_EXPORT(XGetVideoMode, XGetVideoMode)
GUEST_EXPORT(XGetLanguage, XGetLanguage)
GUEST_EXPORT(XGetGameRegion, XGetGameRegion)
GUEST_EXPORT(XamGetSystemVersion, XamGetSystemVersion)
