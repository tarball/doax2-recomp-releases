#pragma once

#include <cstdint>
#include <vector>

namespace kernel
{
    // Loads the XEX header into guest memory, builds the executable's loader entry and publishes
    // it through the XexExecutableModuleHandle variable. Call after imports::ResolveSlots().
    bool InitializeExecutableModule(const std::vector<uint8_t>& xex, uint32_t imageBase, uint32_t imageSize);
    // The same, from the XEX file.
    bool InitializeExecutableModule(const char* xexPath, uint32_t imageBase, uint32_t imageSize);

    // Guest address of the executable's XEX header.
    uint32_t ExecutableXexHeader();

    // Looks up an XEX optional header the way the kernel does: keys whose low byte is 0x00 return the
    // value itself, 0x01 the address of the value, anything else the address of the header's data.
    // Returns 0 if the header is absent.
    uint32_t GetXexOptionalHeader(uint32_t xexHeader, uint32_t key);
}
