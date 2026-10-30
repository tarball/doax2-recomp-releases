#pragma once

// Swap chain and the final pass that draws the guest front buffer into the window, letterboxed to
// keep its aspect ratio, through the display's gamma ramp.
#include "gpu/d3d12/device.h"

#include <array>
#include <cstdint>
#include <string>

namespace ui
{
    class Window;
}

namespace gpu::d3d12
{
    class Overlay;

    class Presenter
    {
    public:
        struct Source
        {
            D3D12_GPU_DESCRIPTOR_HANDLE srv{}; // Texture2D in the bound shader-visible heap
            uint32_t width = 0;  // guest texels
            uint32_t height = 0;
            uint32_t scale = 1;  // host texels per guest texel (resolution scaling)
            // Output component -> source component (0-3), or 4 = 0.0, 5 = 1.0 (fetch constant swizzle).
            std::array<uint32_t, 4> swizzle = { 0, 1, 2, 3 };
            // Constant buffer holding the display's gamma ramp (gpu::SwapCommand::gammaRamp's 256 entries).
            D3D12_GPU_VIRTUAL_ADDRESS gammaRamp = 0;
        };

        bool Initialize(Device& device, ui::Window& window);
        // The runtime's notices, drawn over the front buffer in the window and in dumps (overlay.h).
        void SetOverlay(const Overlay* overlay) { m_overlay = overlay; }
        // The height in pixels the front buffer is shown at in the window (letterboxed), for the overlay.
        uint32_t WindowImageHeight(const Source& source) const;

        // Records the present pass into `list` (which must have the shader-visible heap bound).
        // `source` may be null (black frame). The device must be idle-able: resizing waits for the GPU.
        void Record(ID3D12GraphicsCommandList* list, const Source* source);

        // After `Record`, also copies the frame for a PNG dump, written by `FinishDump` once the GPU has executed the
        // list: the front buffer at its own size (the guest's times the resolution scale), or without one the back buffer.
        void RecordDump(ID3D12GraphicsCommandList* list, const Source* source);
        void FinishDump(const std::string& path);

        void Present();

    private:
        bool CreateBackBuffers();
        void ResizeIfNeeded();

        Device* m_device = nullptr;
        ui::Window* m_window = nullptr;
        const Overlay* m_overlay = nullptr;
        ComPtr<IDXGISwapChain3> m_swapChain;
        static constexpr uint32_t kBufferCount = 3;
        ComPtr<ID3D12Resource> m_backBuffers[kBufferCount];
        ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        uint32_t m_rtvSize = 0;
        uint32_t m_width = 0, m_height = 0;
        bool m_tearing = false;
        bool m_vsync = false;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipeline;

        ComPtr<ID3D12Resource> m_dumpBuffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_dumpFootprint{};
        ComPtr<ID3D12Resource> m_dumpTarget; // the front buffer drawn at its own size
        ComPtr<ID3D12DescriptorHeap> m_dumpRtvHeap;
    };
}
