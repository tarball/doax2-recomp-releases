#pragma once

// AUDIT=1: a census of the GPU state the game uses, against what the renderer implements (debugging). It records every
// value the game writes to the display's and the draw context's registers, and the texture fetch constants draws use
// (their fields other than addresses and sizes), with counts; checks each draw, resolve and texture fetch for state
// the renderer doesn't implement or only approximates, reporting each finding once as it first appears; and rewrites
// dumps/audit.txt with all of it every 600 frames. All calls come from the command processor thread.
#include "gpu/xenos.h"

#include <cstdint>
#include <string>

namespace gpu::audit
{
    bool Enabled();

    // Every register the command processor writes.
    void RegisterWritten(uint32_t index, uint32_t value);
    // A draw the renderer records: its primitive type, the color targets it writes (a bit each), their sample count, and
    // whether it has a depth/stencil target.
    void Draw(uint32_t primitive, uint32_t colorTargets, uint32_t samples, bool depthStencil);
    void Resolve();
    void TextureFetch(const xenos::TextureFetch& fetch);
    // State the renderer skips or approximates, found elsewhere.
    void Finding(const std::string& what);
    // Each swap: rewrites the report every 600 frames.
    void Frame(uint64_t frame);
}
