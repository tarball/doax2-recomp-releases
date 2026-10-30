#pragma once

// Loads the game's code module (<executable>_ppc.dll next to the executable; ANALYSIS.md, Phase 5, The game's code
// module) and checks it before any of its code runs: that it was built for this runtime (module.h), that every function
// the runtime calls in it is there, and that every replacement of a game function the executable exports is what the
// module's code calls.
#include <string>

struct PPCFuncMapping;

namespace module
{
    struct Loaded
    {
        const PPCFuncMapping* functions = nullptr; // the generated code's function table
        std::string sha256;                        // of the module's file
        size_t replacements = 0;                   // the executable's replacements, all routed
    };

    // Empty, or what's wrong (for the player: a sentence or two, ending with what to do).
    std::string Load(Loaded& loaded);
    // The module's file name (doa4_ppc.dll) and full path next to the executable.
    const wchar_t* FileName();
    std::wstring Path();
}
