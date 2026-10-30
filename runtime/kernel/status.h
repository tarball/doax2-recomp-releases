#pragma once

#include <cstdint>

// NTSTATUS values returned by the Xbox 360 kernel.
namespace status
{
    inline constexpr uint32_t kSuccess = 0x00000000;
    inline constexpr uint32_t kTimeout = 0x00000102;
    inline constexpr uint32_t kPending = 0x00000103;
    inline constexpr uint32_t kBufferOverflow = 0x80000005;
    inline constexpr uint32_t kUnsuccessful = 0xC0000001;
    inline constexpr uint32_t kNotImplemented = 0xC0000002;
    inline constexpr uint32_t kInvalidHandle = 0xC0000008;
    inline constexpr uint32_t kInvalidParameter = 0xC000000D;
    inline constexpr uint32_t kNoSuchFile = 0xC000000F;
    inline constexpr uint32_t kEndOfFile = 0xC0000011;
    inline constexpr uint32_t kNoMemory = 0xC0000017;
    inline constexpr uint32_t kAccessDenied = 0xC0000022;
    inline constexpr uint32_t kObjectNameNotFound = 0xC0000034;
    inline constexpr uint32_t kObjectPathNotFound = 0xC000003A;
    inline constexpr uint32_t kMemoryNotAllocated = 0xC00000A0;
}

// Win32-style error codes returned by XAM.
namespace xerror
{
    inline constexpr uint32_t kSuccess = 0;
    inline constexpr uint32_t kNotEnoughMemory = 8;
    inline constexpr uint32_t kInvalidParameter = 87;
}
