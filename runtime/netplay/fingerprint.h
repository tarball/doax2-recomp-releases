#pragma once

// FINGERPRINT=1: what this PC computes where two PCs could differ, for comparing two before they play netplay together
// (ANALYSIS.md, Phase 6, Determinism between PCs). It runs instead of the game, once the game's image is loaded, writes
// dumps/fingerprint.txt (and the log), and exits:
// - the CPU, the Windows build, the build of the game (netplay/identity.h) and the C runtime's CPU level and FMA3 setting;
// - MXCSR on the main thread, on a new thread and on a new fiber (created while its creator runs other modes);
// - a hash, and the number of results hashed, of each operation whose result could depend on the CPU: the C runtime's
//   fma by kind of operands (the generated code calls it for every guest multiply-add), its exp2f and log2f (every 7th
//   float), DPPS (vmsum4fp128, vmsum3fp128) on finite, nearly denormal and NaN or infinite lanes, and the generated
//   code's other float patterns;
// - a hash of the guest's math functions (the game registers them: GameState::mathFunctions, such as its C runtime's
//   sin, cos, tan, atan, atan2 and pow), run through the recompiled code on a guest thread.
// Every hashed section runs twice: with the C runtime's FMA3 paths as the CPU allows them, then off, as the game runs
// them (main.cpp), which is what a CPU without FMA3 runs. Run it on both PCs and compare the files: the first lines name
// the machines, and every line from the first "--" on should be the same; the first that isn't names what differs.
namespace netplay::fingerprint
{
    // Returns the process's exit code.
    int Run();
}
