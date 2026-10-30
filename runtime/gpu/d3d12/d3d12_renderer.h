#pragma once

#include "gpu/renderer.h"

#include <memory>

namespace ui
{
    class Window;
}

namespace gpu::d3d12
{
    // Creates the D3D12 renderer presenting into `window`, or returns null if D3D12 is unavailable.
    std::unique_ptr<gpu::Renderer> CreateRenderer(ui::Window& window);
}
