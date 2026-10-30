#include "gpu/d3d12/overlay.h"

#include "log.h"
#include "ui/notices.h"

#include <cstring>

namespace gpu::d3d12
{
    namespace
    {
        const char kShader[] = R"(
Texture2D<float4> card : register(t0);
SamplerState linearClamp : register(s0);
// The card's corners in clip space (left, top, right, bottom), and its opacity.
cbuffer Constants : register(b0) { float4 rect; float4 opacity; };

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// A strip of 4 vertices over the card.
VertexOutput VSMain(uint id : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2(id & 1u, id >> 1u);
    output.position = float4(lerp(rect.xy, rect.zw, output.uv), 0.0, 1.0);
    return output;
}

// The card's premultiplied colors, averaged over the frame's pixel when the card is shown smaller than it was drawn (up
// to 4x4 bilinear taps), as the present pass shrinks the frame.
float4 PSMain(VertexOutput input) : SV_Target
{
    float2 size;
    card.GetDimensions(size.x, size.y);
    float2 pixel = float2(ddx(input.uv.x), ddy(input.uv.y));
    uint2 taps = clamp(uint2(ceil(abs(pixel) * size - 0.001)), 1u, 4u);
    float4 color = float4(0.0, 0.0, 0.0, 0.0);
    for (uint y = 0u; y < taps.y; y++)
    {
        for (uint x = 0u; x < taps.x; x++)
            color += card.SampleLevel(linearClamp, input.uv + pixel * ((float2(x, y) + 0.5) / float2(taps) - 0.5), 0.0);
    }
    return color / float(taps.x * taps.y) * opacity.x;
}
)";
    }

    bool Overlay::Initialize(Device& device, DescriptorHeap& views)
    {
        m_device = &device;
        m_views = &views;
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = { 0, 0, 8 };
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDescription{ 2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device.Get()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature))))
        {
            LOG_ERROR("D3D12: can't create the overlay's root signature");
            return false;
        }
        auto vertexShader = CompileShader(kShader, "VSMain", "vs_5_1", "overlay");
        auto pixelShader = CompileShader(kShader, "PSMain", "ps_5_1", "overlay");
        if (!vertexShader || !pixelShader)
            return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = m_rootSignature.Get();
        pipeline.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        pipeline.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
        // Premultiplied alpha.
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pipeline.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        if (FAILED(device.Get()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_pipeline))))
        {
            LOG_ERROR("D3D12: can't create the overlay's pipeline");
            return false;
        }
        return true;
    }

    void Overlay::Prepare(ID3D12GraphicsCommandList* list, UploadAllocator& upload, uint64_t frame, uint32_t pixelHeight)
    {
        // Textures the GPU is done with (their frames' slots have come round again).
        while (!m_retired.empty() && m_retired.front().frame + kFramesInFlight < frame)
        {
            m_views->FreePersistent(m_retired.front().view);
            m_retired.pop_front();
        }
        const ui::notices::Frame next = ui::notices::Next(pixelHeight);
        m_opacity = next.opacity;
        if (next.card == m_card)
            return;
        if (m_texture)
            m_retired.push_back({ frame, std::move(m_texture), m_view });
        m_texture.Reset();
        m_view = UINT32_MAX;
        m_card = next.card;
        if (!m_card || !m_card->width || !m_card->height)
            return;

        const ui::notices::Card& card = *m_card;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = card.width;
        description.Height = card.height;
        description.DepthOrArraySize = description.MipLevels = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        ID3D12Device* device = m_device->Get();
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&m_texture))))
        {
            LOG_WARN("D3D12: can't create a %ux%u texture for a notice", card.width, card.height);
            m_card.reset();
            return;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 size = 0;
        device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        const UploadAllocator::Allocation allocation = upload.Allocate(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        for (uint32_t y = 0; y < card.height; y++)
            memcpy(allocation.cpu + size_t(footprint.Footprint.RowPitch) * y, card.bgra.data() + size_t(y) * card.width * 4, size_t(card.width) * 4);
        footprint.Offset = allocation.offset;
        D3D12_TEXTURE_COPY_LOCATION destination{ m_texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source{ allocation.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        source.PlacedFootprint = footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(m_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &barrier);
        m_view = m_views->AllocatePersistent(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = description.Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_texture.Get(), &view, m_views->Cpu(m_view));
    }

    void Overlay::Draw(ID3D12GraphicsCommandList* list, const D3D12_VIEWPORT& viewport) const
    {
        if (!m_card || m_view == UINT32_MAX || m_opacity <= 0.0f)
            return;
        // The card's place in a 1280x720 frame, in clip space.
        const ui::notices::Card& card = *m_card;
        const float constants[8] = { card.x / 640.0f - 1.0f, 1.0f - card.y / 360.0f, (card.x + card.w) / 640.0f - 1.0f,
            1.0f - (card.y + card.h) / 360.0f, m_opacity, 0.0f, 0.0f, 0.0f };
        list->RSSetViewports(1, &viewport);
        list->SetGraphicsRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipeline.Get());
        list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
        list->SetGraphicsRootDescriptorTable(1, m_views->Gpu(m_view));
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        list->DrawInstanced(4, 1, 0, 0);
    }
}
