#pragma once

// Interface between the command processor and a host rendering backend (D3D12 now, possibly Vulkan
// later). All calls come from the command processor thread, in command stream order; the backend
// reads the rest of the GPU state from the register file (gpu::Register).
#include "gpu/registers.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace gpu
{
    enum class ShaderStage : uint32_t
    {
        kVertex = 0,
        kPixel = 1,
    };

    struct DrawCommand
    {
        DrawInitiator initiator{ 0 };
        // DMA index buffer (initiator source select 0).
        uint32_t indexBase = 0;   // physical address
        uint32_t indexSize = 0;   // in indices
        uint32_t indexEndian = 0; // endian mode of the index data
    };

    struct SwapCommand
    {
        uint32_t frontBuffer = 0; // physical address
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fetch[6] = {};   // the front buffer's texture fetch constant (base address physical)
        // The game frame that presents it (netplay::Frame), and in a fight its fight frame (netplay::FightFrame; else -1).
        uint32_t gameFrame = 0;
        int64_t fightFrame = -1;
        // The display's gamma ramp, which the scanout applies to the front buffer's 8-bit components: 256 entries of
        // 10-bit blue, green and red (bits 0-9, 10-19, 20-29), valid during the call. Identity until the game sets it.
        const uint32_t* gammaRamp = nullptr;
    };

    class Renderer
    {
    public:
        virtual ~Renderer() = default;

        // IM_LOAD / IM_LOAD_IMMEDIATE: `ucode` becomes the active shader for `stage`. The dwords are in guest
        // byte order (big-endian), as the GPU reads them; often straight from guest memory.
        virtual void LoadShader(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount) = 0;
        // DRAW_INDX / DRAW_INDX_2, including resolves (draws with RB_MODECONTROL in copy mode).
        virtual void Draw(const DrawCommand& draw) = 0;
        // XE_SWAP: shows the front buffer and ends the frame.
        virtual void Swap(const SwapCommand& swap) = 0;
        // Called once at startup, before the game runs (from the main thread): the executable image, whose
        // shaders the backend may prepare in the background. `image` stays valid and unchanged.
        virtual void PrecompileShaders(const uint8_t* /*image*/, size_t /*size*/) {}
    };

    // The renderer the command processor drives; set before the game initializes the GPU. Without
    // one (headless runs) draws and swaps are only counted.
    void SetRenderer(std::unique_ptr<Renderer> renderer);

    // Diagnostics: the next presented frame is written to dumps/ as DUMP_FRAMES would (from any thread).
    void RequestFrameDump();

    // The register file (shared with the guest's view of the GPU registers).
    uint32_t Register(uint32_t index);
    inline float RegisterFloat(uint32_t index)
    {
        const uint32_t value = Register(index);
        float result;
        static_assert(sizeof(result) == sizeof(value));
        __builtin_memcpy(&result, &value, sizeof(result));
        return result;
    }
}
