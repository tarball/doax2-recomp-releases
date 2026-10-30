#pragma once

#include <cstdint>
#include <string_view>

namespace imports
{
    // Points every import address table slot at something valid: function slots at their thunk
    // (so pointers to imported functions resolve through the lookup table) and variable slots at
    // zeroed storage in the kernel data region.
    void ResolveSlots();

    // Adds every import thunk to the indirect call lookup table.
    void RegisterThunks();

    // Guest address of a variable import's storage (after ResolveSlots), or 0 if it isn't imported.
    uint32_t VariableAddress(std::string_view name);

    // Guest address of the thunk for a function the game imports from `library` ("Xam" or "Kernel") by `ordinal`, or 0
    // if it doesn't import it.
    uint32_t FunctionThunk(std::string_view library, uint32_t ordinal);
}
