#pragma once

// The runtime's notices over the game (ui/notices.h): each frame presented, the card to show is drawn over the game's
// picture, in the window and in frame dumps, after the display's gamma ramp, blended by its premultiplied alpha.
#include "gpu/d3d12/device.h"
#include "gpu/d3d12/resources.h"

#include <deque>
#include <memory>

namespace ui::notices
{
    struct Card;
}

namespace gpu::d3d12
{
    class Overlay
    {
    public:
        bool Initialize(Device& device, DescriptorHeap& views);

        // Before the frame's present pass, once a frame presented: takes the notice to show (ui::notices::Next) for a
        // frame shown at most `pixelHeight` pixels high, and uploads its card when it's new. `frame`: the frame's number.
        void Prepare(ID3D12GraphicsCommandList* list, UploadAllocator& upload, uint64_t frame, uint32_t pixelHeight);
        // Draws it over the game's picture, which covers `viewport` of the bound render target (R8G8B8A8_UNORM), if
        // there's one to show this frame. The list has the shader-visible heap bound.
        void Draw(ID3D12GraphicsCommandList* list, const D3D12_VIEWPORT& viewport) const;

    private:
        Device* m_device = nullptr;
        DescriptorHeap* m_views = nullptr;
        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipeline;
        // The card shown, its texture and view, and this frame's opacity.
        std::shared_ptr<const ui::notices::Card> m_card;
        ComPtr<ID3D12Resource> m_texture;
        uint32_t m_view = UINT32_MAX;
        float m_opacity = 0;
        // Textures and views of cards no longer shown, kept until the GPU is done with the frames that drew them.
        struct Retired
        {
            uint64_t frame;
            ComPtr<ID3D12Resource> texture;
            uint32_t view;
        };
        std::deque<Retired> m_retired;
    };
}
