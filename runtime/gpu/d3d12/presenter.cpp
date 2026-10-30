#include "gpu/d3d12/presenter.h"

#include "audio/audio_system.h"
#include "gpu/d3d12/overlay.h"
#include "gpu/d3d12/resources.h"
#include "log.h"
#include "ui/window.h"
#include "util/png.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace gpu::d3d12
{
    namespace
    {
        constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

        const char kShader[] = R"(
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Constants : register(b0) { uint4 swizzle; };
// The display's gamma ramp: 256 entries of 10-bit blue, green and red (bits 0-9, 10-19, 20-29).
cbuffer GammaRamp : register(b1) { uint4 ramp[64]; };

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// A triangle covering the viewport.
VertexOutput VSMain(uint id : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float Select(float4 color, uint component)
{
    return component < 4 ? color[component] : (component == 5 ? 1.0 : 0.0);
}

uint RampEntry(uint index)
{
    uint4 entries = ramp[index >> 2u];
    uint component = index & 3u;
    return component == 0u ? entries.x : (component == 1u ? entries.y : (component == 2u ? entries.z : entries.w));
}

// A component as the display shows it: its 8-bit value through the gamma ramp (a filtered value between two entries
// interpolates them).
float Ramp(float value, uint shift)
{
    float x = saturate(value) * 255.0;
    uint i = min(uint(x), 254u);
    float low = float((RampEntry(i) >> shift) & 0x3FFu), high = float((RampEntry(i + 1u) >> shift) & 0x3FFu);
    return lerp(low, high, x - float(i)) / 1023.0;
}

float3 Displayed(float4 texel)
{
    return float3(Ramp(Select(texel, swizzle.x), 20u), Ramp(Select(texel, swizzle.y), 10u), Ramp(Select(texel, swizzle.z), 0u));
}

// Shrinking (a resolution-scaled front buffer in a smaller window): the average of bilinear taps spread evenly over the
// window pixel, up to 4x4 of them, approximating its box; enlarging or 1:1, one bilinear tap. Each tap goes through
// the gamma ramp before they're averaged, as the display would show the front buffer's pixels.
float4 PSMain(VertexOutput input) : SV_Target
{
    float2 size;
    source.GetDimensions(size.x, size.y);
    float2 pixel = float2(ddx(input.uv.x), ddy(input.uv.y)); // UV per window pixel
    uint2 taps = clamp(uint2(ceil(abs(pixel) * size - 0.001)), 1u, 4u);
    float3 color = float3(0.0, 0.0, 0.0);
    for (uint y = 0u; y < taps.y; y++)
    {
        for (uint x = 0u; x < taps.x; x++)
            color += Displayed(source.SampleLevel(linearClamp, input.uv + pixel * ((float2(x, y) + 0.5) / float2(taps) - 0.5), 0.0));
    }
    return float4(color / float(taps.x * taps.y), 1.0);
}
)";
    }

    bool Presenter::Initialize(Device& device, ui::Window& window)
    {
        m_device = &device;
        m_window = &window;
        m_vsync = std::getenv(GAME_PREFIX "_VSYNC") && std::atoi(std::getenv(GAME_PREFIX "_VSYNC")) != 0;

        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(device.Factory()->QueryInterface(IID_PPV_ARGS(&factory5))))
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        m_tearing = tearing != FALSE;

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Format = kBackBufferFormat;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = kBufferCount;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> swapChain1;
        if (FAILED(device.Factory()->CreateSwapChainForHwnd(device.Queue(), window.Handle(), &description, nullptr, nullptr, &swapChain1)) ||
            FAILED(swapChain1.As(&m_swapChain)))
        {
            LOG_ERROR("D3D12: can't create the swap chain");
            return false;
        }
        device.Factory()->MakeWindowAssociation(window.Handle(), DXGI_MWA_NO_ALT_ENTER);

        D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
        heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDescription.NumDescriptors = kBufferCount;
        device.Get()->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&m_rtvHeap));
        m_rtvSize = device.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (!CreateBackBuffers())
            return false;

        // Root signature: swizzle constants, the source texture, the gamma ramp and a linear clamp sampler.
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = { 0, 0, 4 };
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[2].Descriptor = { 1, 0 };
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDescription{ 3, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device.Get()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature))))
        {
            LOG_ERROR("D3D12: can't create the presenter root signature");
            return false;
        }

        auto vertexShader = CompileShader(kShader, "VSMain", "vs_5_1", "present");
        auto pixelShader = CompileShader(kShader, "PSMain", "ps_5_1", "present");
        if (!vertexShader || !pixelShader)
            return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = m_rootSignature.Get();
        pipeline.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        pipeline.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = kBackBufferFormat;
        pipeline.SampleDesc.Count = 1;
        if (FAILED(device.Get()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_pipeline))))
        {
            LOG_ERROR("D3D12: can't create the presenter pipeline");
            return false;
        }
        return true;
    }

    bool Presenter::CreateBackBuffers()
    {
        DXGI_SWAP_CHAIN_DESC1 description;
        m_swapChain->GetDesc1(&description);
        m_width = description.Width;
        m_height = description.Height;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < kBufferCount; i++)
        {
            if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]))))
            {
                LOG_ERROR("D3D12: can't get swap chain buffer %u", i);
                return false;
            }
            m_device->Get()->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
            rtv.ptr += m_rtvSize;
        }
        return true;
    }

    void Presenter::ResizeIfNeeded()
    {
        const uint32_t width = m_window->ClientWidth(), height = m_window->ClientHeight();
        if (width == 0 || height == 0 || (width == m_width && height == m_height))
            return;
        m_device->WaitIdle();
        for (auto& buffer : m_backBuffers)
            buffer.Reset();
        if (FAILED(m_swapChain->ResizeBuffers(kBufferCount, width, height, kBackBufferFormat, m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
            LOG_ERROR("D3D12: ResizeBuffers(%u x %u) failed", width, height);
        else
            LOG_INFO("GPU: window %ux%u", width, height);
        CreateBackBuffers();
    }

    uint32_t Presenter::WindowImageHeight(const Source& source) const
    {
        const uint32_t width = m_window->ClientWidth(), height = m_window->ClientHeight();
        if (!source.width || !source.height || !width || !height)
            return 0;
        return uint32_t(std::min(float(width) / source.width, float(height) / source.height) * source.height + 0.5f);
    }

    void Presenter::Record(ID3D12GraphicsCommandList* list, const Source* source)
    {
        ResizeIfNeeded();
        const uint32_t index = m_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = m_backBuffers[index].Get();

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(index) * m_rtvSize;
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        list->ClearRenderTargetView(rtv, black, 0, nullptr);

        if (source && source->width && source->height)
        {
            // Letterbox: the largest rectangle with the source's aspect ratio, centered.
            const float scale = std::min(float(m_width) / source->width, float(m_height) / source->height);
            const float width = source->width * scale, height = source->height * scale;
            D3D12_VIEWPORT viewport{ (m_width - width) * 0.5f, (m_height - height) * 0.5f, width, height, 0.0f, 1.0f };
            D3D12_RECT scissor{ 0, 0, LONG(m_width), LONG(m_height) };
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRootSignature(m_rootSignature.Get());
            list->SetPipelineState(m_pipeline.Get());
            list->SetGraphicsRoot32BitConstants(0, 4, source->swizzle.data(), 0);
            list->SetGraphicsRootDescriptorTable(1, source->srv);
            list->SetGraphicsRootConstantBufferView(2, source->gammaRamp);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
            if (m_overlay)
                m_overlay->Draw(list, viewport);
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);
    }

    void Presenter::RecordDump(ID3D12GraphicsCommandList* list, const Source* source)
    {
        ID3D12Resource* image = m_backBuffers[m_swapChain->GetCurrentBackBufferIndex()].Get();
        D3D12_RESOURCE_STATES imageState = D3D12_RESOURCE_STATE_PRESENT;
        // DUMP_WINDOW=1 dumps the window's image (letterboxed, scaled to the window) instead.
        static const bool window = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DUMP_WINDOW");
            return setting && std::atoi(setting) != 0;
        }();
        if (source && source->width && source->height && !window)
        {
            // The front buffer at its own size, through the present shader (for the fetch constant's swizzle and the gamma
            // ramp: as the display shows it).
            const uint32_t width = source->width * source->scale, height = source->height * source->scale;
            if (!m_dumpRtvHeap)
            {
                D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
                heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                heapDescription.NumDescriptors = 1;
                m_device->Get()->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&m_dumpRtvHeap));
            }
            if (!m_dumpTarget || m_dumpTarget->GetDesc().Width != width || m_dumpTarget->GetDesc().Height != height)
            {
                D3D12_RESOURCE_DESC target{};
                target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                target.Width = width;
                target.Height = height;
                target.DepthOrArraySize = target.MipLevels = 1;
                target.Format = kBackBufferFormat;
                target.SampleDesc.Count = 1;
                target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                m_dumpTarget.Reset();
                if (FAILED(m_device->Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                        IID_PPV_ARGS(&m_dumpTarget))))
                    return;
                m_device->Get()->CreateRenderTargetView(m_dumpTarget.Get(), nullptr, m_dumpRtvHeap->GetCPUDescriptorHandleForHeapStart());
            }
            D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(m_dumpTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &barrier);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_dumpRtvHeap->GetCPUDescriptorHandleForHeapStart();
            const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
            const D3D12_RECT scissor{ 0, 0, LONG(width), LONG(height) };
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRootSignature(m_rootSignature.Get());
            list->SetPipelineState(m_pipeline.Get());
            list->SetGraphicsRoot32BitConstants(0, 4, source->swizzle.data(), 0);
            list->SetGraphicsRootDescriptorTable(1, source->srv);
            list->SetGraphicsRootConstantBufferView(2, source->gammaRamp);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
            if (m_overlay)
                m_overlay->Draw(list, viewport);
            barrier = TransitionBarrier(m_dumpTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
            image = m_dumpTarget.Get();
            imageState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }

        const D3D12_RESOURCE_DESC description = image->GetDesc();
        UINT64 size = 0;
        m_device->Get()->GetCopyableFootprints(&description, 0, 1, 0, &m_dumpFootprint, nullptr, nullptr, &size);
        if (!m_dumpBuffer || m_dumpBuffer->GetDesc().Width < size)
        {
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = size;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            m_dumpBuffer.Reset();
            m_device->Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_dumpBuffer));
        }

        D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(image, imageState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (imageState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION destination{ m_dumpBuffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        destination.PlacedFootprint = m_dumpFootprint;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{ image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        list->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
        if (imageState != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
        }
    }

    void Presenter::FinishDump(const std::string& path)
    {
        void* data = nullptr;
        // (The last row isn't padded to the row pitch.)
        D3D12_RANGE range{ 0, SIZE_T(m_dumpFootprint.Footprint.RowPitch) * (m_dumpFootprint.Footprint.Height - 1) + m_dumpFootprint.Footprint.Width * 4 };
        if (const HRESULT result = m_dumpBuffer->Map(0, &range, &data); FAILED(result))
        {
            LOG_WARN("D3D12: can't map the frame dump buffer (0x%08lX, device removed reason 0x%08lX)", result,
                m_device->Get()->GetDeviceRemovedReason());
            return;
        }
        if (!util::WritePng(path.c_str(), static_cast<const uint8_t*>(data), m_dumpFootprint.Footprint.Width, m_dumpFootprint.Footprint.Height,
                m_dumpFootprint.Footprint.RowPitch))
        {
            LOG_WARN("Can't write %s", path.c_str());
        }
        else
        {
            const double audioSeconds = audio::DumpedSeconds();
            if (audioSeconds >= 0)
                LOG_INFO("Frame dumped to %s (audio dump at %.3f s)", path.c_str(), audioSeconds);
            else
                LOG_INFO("Frame dumped to %s", path.c_str());
        }
        D3D12_RANGE written{ 0, 0 };
        m_dumpBuffer->Unmap(0, &written);
    }

    void Presenter::Present()
    {
        // Without vsync, flip model presents replace any frame still queued, so this never blocks
        // the command processor; VSYNC=1 waits for the display instead.
        const HRESULT result = m_swapChain->Present(m_vsync ? 1 : 0, 0);
        if (FAILED(result))
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                LOG_ERROR("D3D12: Present failed (0x%08lX, device removed reason 0x%08lX)", result, m_device->Get()->GetDeviceRemovedReason());
            }
        }
    }
}
