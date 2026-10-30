// Guest threads that spin waiting for others, as the 360's dedicated hardware threads allowed: on a PC each would
// burn a host core, so these replacements sleep until there's something to do (DOAX2-specific addresses).
//
// DOAX2 has one: the main thread in D3D's waits for the GPU. (DOA4's engine also polled a job ring on a thread of its
// own; DOAX2's threads sleep when idle.)
#include "gpu/command_processor.h"
#include "guest_abi.h"

extern "C" PPC_FUNC(__imp__sub_827054E8); // D3D's wait step

// D3D's wait step, called on every turn of its loops that wait for the GPU (for the ring's read pointer before reusing
// command buffer space, and for fences, e.g. sub_82703DB0, sub_827043D0 and sub_82704C28): it spins a few hundred
// cycles, then checks for a GPU hang (5 s without the read pointer moving). It returns nonzero to go on waiting. The
// main thread waits there for much of each frame: let it sleep until the command processor makes progress first.
PPC_FUNC(sub_827054E8)
{
    gpu::WaitForProgress();
    __imp__sub_827054E8(ctx, base);
}
