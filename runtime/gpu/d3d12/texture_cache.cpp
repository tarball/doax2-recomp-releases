#include "gpu/d3d12/texture_cache.h"

#include "gpu/memory_watch.h"
#include "log.h"
#include "util/profiler.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace gpu::d3d12
{
    namespace
    {
        // Copies one level (of one array slice) of a tiled or linear texture from guest memory into a
        // row-major buffer. A thread handles one dword column (up to 4 small blocks) or one block of 4+ bytes.
        const char kUntileShader[] = R"(
ByteAddressBuffer xe_source : register(t0);
RWByteAddressBuffer xe_dest : register(u0);
cbuffer XeUntileConstants : register(b0)
{
    uint xe_base;          // byte address of the level
    uint xe_pitch;         // row pitch in blocks (multiple of 32 if tiled)
    uint xe_width;         // in blocks
    uint xe_height;        // in blocks
    uint xe_log2_bpb;      // log2 of bytes per block
    uint xe_endian;
    uint xe_tiled;
    uint xe_dest_row_pitch;
    uint2 xe_offset;       // block offset of the level in its storage (packed mips)
    uint xe_linear_row_bytes;
    uint xe_conversion;    // xenos::FormatConversion
    uint xe_dest_offset;   // of the level in the staging buffer
};

uint XeTiledOffset2D(uint x, uint y, uint pitch, uint log_bpp)
{
    uint macro = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << (log_bpp + 7u);
    uint micro = ((x & 7u) + ((y & 6u) << 2u)) << log_bpp;
    uint offset = macro + ((micro & ~15u) << 1u) + (micro & 15u) + ((y & 8u) << (3u + log_bpp)) + ((y & 1u) << 4u);
    return ((offset & ~511u) << 3u) + ((offset & 448u) << 2u) + (offset & 63u) + ((y & 16u) << 7u) + (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u);
}

uint XeSwap(uint v)
{
    if (xe_endian == 1u)
        return ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
    if (xe_endian == 2u)
        return (v << 24u) | ((v & 0xFF00u) << 8u) | ((v >> 8u) & 0xFF00u) | (v >> 24u);
    if (xe_endian == 3u)
        return (v << 16u) | (v >> 16u);
    return v;
}

// Xenos 20e4 float depth (4-bit exponent, 20-bit mantissa).
float XeFloat20e4ToFloat32(uint v)
{
    uint e = (v >> 20u) & 15u, m = v & 0xFFFFFu;
    return e == 0u ? float(m) * exp2(-34.0) : (1.0 + float(m) / 1048576.0) * exp2(float(e) - 15.0);
}

uint XeConvert(uint v)
{
    if (xe_conversion == 1u)
        return asuint(float(v >> 8u) / 16777215.0);
    if (xe_conversion == 2u)
        return asuint(XeFloat20e4ToFloat32(v >> 8u));
    return v;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint blocks_per_thread = xe_log2_bpb < 2u ? (4u >> xe_log2_bpb) : 1u;
    uint dwords = xe_log2_bpb > 2u ? (1u << (xe_log2_bpb - 2u)) : 1u;
    uint x = id.x * blocks_per_thread, y = id.y;
    if (x >= xe_width || y >= xe_height)
        return;
    uint sx = x + xe_offset.x, sy = y + xe_offset.y;
    uint source = xe_base + (xe_tiled != 0u ? XeTiledOffset2D(sx, sy, xe_pitch, xe_log2_bpb) : sy * xe_linear_row_bytes + (sx << xe_log2_bpb));
    uint dest = xe_dest_offset + y * xe_dest_row_pitch + (x << xe_log2_bpb);
    for (uint i = 0u; i < dwords; i++)
        xe_dest.Store(dest + i * 4u, XeConvert(XeSwap(xe_source.Load(source + i * 4u))));
}
)";

        struct UntileConstants
        {
            uint32_t base, pitch, width, height, log2Bpb, endian, tiled, destRowPitch, offsetX, offsetY, linearRowBytes, conversion, destOffset;
            uint32_t scale;
        };

        // Copies a level of a tiled texture (4 or 8 bytes per texel) from the scaled copies resolves wrote (see
        // ScaledResolves) into a row-major buffer, a thread per host texel.
        const char kScaledUntileShader[] = R"(
ByteAddressBuffer xe_page_table : register(t0);
ByteAddressBuffer xe_scaled : register(t1);
RWByteAddressBuffer xe_dest : register(u0);
cbuffer XeUntileConstants : register(b0)
{
    uint xe_base;
    uint xe_pitch;         // row pitch in texels (multiple of 32)
    uint xe_width;         // guest texels
    uint xe_height;
    uint xe_log2_bpb;      // 2 or 3
    uint xe_endian;
    uint xe_tiled;
    uint xe_dest_row_pitch;
    uint2 xe_offset;
    uint xe_linear_row_bytes;
    uint xe_conversion;    // xenos::FormatConversion
    uint xe_dest_offset;
    uint xe_scale;         // host texels per guest texel in each direction
};

uint XeTiledOffset2D(uint x, uint y, uint pitch, uint log_bpp)
{
    uint macro = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << (log_bpp + 7u);
    uint micro = ((x & 7u) + ((y & 6u) << 2u)) << log_bpp;
    uint offset = macro + ((micro & ~15u) << 1u) + (micro & 15u) + ((y & 8u) << (3u + log_bpp)) + ((y & 1u) << 4u);
    return ((offset & ~511u) << 3u) + ((offset & 448u) << 2u) + (offset & 63u) + ((y & 16u) << 7u) + (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u);
}

uint XeSwap(uint v)
{
    if (xe_endian == 1u)
        return ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
    if (xe_endian == 2u)
        return (v << 24u) | ((v & 0xFF00u) << 8u) | ((v >> 8u) & 0xFF00u) | (v >> 24u);
    if (xe_endian == 3u)
        return (v << 16u) | (v >> 16u);
    return v;
}

float XeFloat20e4ToFloat32(uint v)
{
    uint e = (v >> 20u) & 15u, m = v & 0xFFFFFu;
    return e == 0u ? float(m) * exp2(-34.0) : (1.0 + float(m) / 1048576.0) * exp2(float(e) - 15.0);
}

uint XeConvert(uint v)
{
    if (xe_conversion == 1u)
        return asuint(float(v >> 8u) / 16777215.0);
    if (xe_conversion == 2u)
        return asuint(XeFloat20e4ToFloat32(v >> 8u));
    return v;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint scale = xe_scale;
    if (id.x >= xe_width * scale || id.y >= xe_height * scale)
        return;
    uint guest = xe_base + XeTiledOffset2D(id.x / scale + xe_offset.x, id.y / scale + xe_offset.y, xe_pitch, xe_log2_bpb);
    uint entry = xe_page_table.Load((guest >> 12u) << 2u);
    uint dest = xe_dest_offset + id.y * xe_dest_row_pitch + (id.x << xe_log2_bpb);
    uint dwords = 1u << (xe_log2_bpb - 2u);
    uint2 sub = id.xy % scale;
    uint source = (((entry - 1u) << 12u) + (guest & 4095u)) * scale * scale + ((sub.y * scale + sub.x) << xe_log2_bpb);
    for (uint i = 0u; i < dwords; i++)
        xe_dest.Store(dest + i * 4u, entry != 0u ? XeConvert(XeSwap(xe_scaled.Load(source + i * 4u))) : 0u);
}
)";


        D3D12_TEXTURE_ADDRESS_MODE AddressMode(uint32_t clamp)
        {
            switch (clamp)
            {
            case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case 2:
            case 4: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            case 6: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            default: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
            }
        }
    }

    size_t TextureCache::KeyHash::operator()(const Key& key) const
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        const uint32_t* words = reinterpret_cast<const uint32_t*>(&key);
        for (size_t i = 0; i < sizeof(Key) / 4; i++)
            hash = (hash ^ words[i]) * 0x100000001B3ull;
        return size_t(hash);
    }

    bool TextureCache::Initialize(ID3D12Device* device, DescriptorHeap& views, DescriptorHeap& samplers, SharedMemory& sharedMemory,
        ScaledResolves& scaledResolves)
    {
        m_device = device;
        m_views = &views;
        m_samplers = &samplers;
        m_sharedMemory = &sharedMemory;
        m_scaledResolves = &scaledResolves;

        // Null views for each declared dimension, then a default sampler.
        static const D3D12_SRV_DIMENSION kDimensions[4] = { D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2D,
            D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_SRV_DIMENSION_TEXTURECUBE };
        for (uint32_t i = 0; i < 4; i++)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = kDimensions[i];
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            m_nullSrvs[i] = views.AllocatePersistent(1);
            device->CreateShaderResourceView(nullptr, &srv, views.Cpu(m_nullSrvs[i]));
        }
        D3D12_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        m_defaultSampler = samplers.AllocatePersistent(1);
        device->CreateSampler(&sampler, samplers.Cpu(m_defaultSampler));

        // Untiling pipeline: constants, the shared memory (raw SRV) and the staging buffer (raw UAV).
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = { 0, 0, sizeof(UntileConstants) / 4 };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].Descriptor = { 0, 0 };
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[2].Descriptor = { 0, 0 };
        D3D12_ROOT_SIGNATURE_DESC rootDescription{ 3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_untileRootSignature))))
        {
            LOG_ERROR("D3D12: can't create the untiling root signature");
            return false;
        }
        auto shader = CompileShader(kUntileShader, "main", "cs_5_1", "untile");
        if (!shader)
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = m_untileRootSignature.Get();
        pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_untilePipeline))))
        {
            LOG_ERROR("D3D12: can't create the untiling pipeline");
            return false;
        }

        // Untiling from scaled resolve copies: constants, the page table and the pool (raw SRVs), the staging buffer.
        D3D12_ROOT_PARAMETER scaledParameters[4]{};
        scaledParameters[0] = parameters[0];
        scaledParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        scaledParameters[1].Descriptor = { 0, 0 };
        scaledParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        scaledParameters[2].Descriptor = { 1, 0 };
        scaledParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        scaledParameters[3].Descriptor = { 0, 0 };
        D3D12_ROOT_SIGNATURE_DESC scaledDescription{ 4, scaledParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        blob.Reset();
        error.Reset();
        if (FAILED(D3D12SerializeRootSignature(&scaledDescription, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_scaledUntileRootSignature))))
        {
            LOG_ERROR("D3D12: can't create the scaled untiling root signature");
            return false;
        }
        auto scaledShader = CompileShader(kScaledUntileShader, "main", "cs_5_1", "untile (scaled)");
        if (!scaledShader)
            return false;
        pipeline.pRootSignature = m_scaledUntileRootSignature.Get();
        pipeline.CS = { scaledShader->GetBufferPointer(), scaledShader->GetBufferSize() };
        if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_scaledUntilePipeline))))
        {
            LOG_ERROR("D3D12: can't create the scaled untiling pipeline");
            return false;
        }
        return true;
    }

    uint32_t TextureCache::NullSrv(uint32_t shaderDimension) const
    {
        return m_nullSrvs[shaderDimension & 3];
    }

    TextureCache::Texture* TextureCache::CreateTexture(const Key& key)
    {
        const xenos::FormatInfo& format = xenos::GetFormatInfo(xenos::TextureFormat(key.format));
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        // Block-compressed textures need block-aligned sizes; scaled ones (never compressed) are `scale` times larger.
        description.Width = (key.width + format.blockWidth - 1) / format.blockWidth * format.blockWidth * key.scale;
        description.Height = (key.height + format.blockHeight - 1) / format.blockHeight * format.blockHeight * key.scale;
        description.DepthOrArraySize = uint16_t(key.dimension == 3 ? 6 : 1);
        description.MipLevels = uint16_t(key.levels);
        // (Typeless when fetches may read it as signed, so its views can be UNORM or SNORM.)
        description.Format = DXGI_FORMAT(format.typelessFormat ? format.typelessFormat : format.dxgiFormat);
        description.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        auto texture = std::make_unique<Texture>();
        texture->key = key;
        texture->layout = xenos::GetTextureLayout(format, key.base, key.mipAddress, key.width, key.height, key.pitch, key.tiled != 0,
            key.packed != 0, key.levels, description.DepthOrArraySize);
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&texture->resource))))
        {
            LOG_WARN("GPU: can't create a %ux%u texture of format %u", key.width, key.height, key.format);
            return nullptr;
        }
        texture->state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (key.dimension == 3)
            LOG_INFO("GPU: cube texture %ux%u, format %u, %u level%s%s, at %08X (mips at %08X, %u bytes per face)", key.width, key.height, key.format,
                key.levels, key.levels == 1 ? "" : "s", key.tiled ? ", tiled" : "", key.base, key.mipAddress, texture->layout.level[0].sliceBytes);
        Texture* result = texture.get();
        m_textures[key] = std::move(texture);
        return result;
    }

    void TextureCache::Load(ID3D12GraphicsCommandList* list, UploadAllocator& upload, Texture& texture)
    {
        if (beforeLoad)
            beforeLoad();
        const Key& key = texture.key;
        const xenos::FormatInfo& format = xenos::GetFormatInfo(xenos::TextureFormat(key.format));
        const xenos::TextureLayout& layout = texture.layout;
        const uint32_t log2Bpb = std::countr_zero(format.bytesPerBlock);

        texture.version = Version(texture);
        m_sharedMemory->RequestRange(upload, layout.baseAddress, layout.baseBytes);
        if (layout.mipBytes)
            m_sharedMemory->RequestRange(upload, layout.mipAddress, layout.mipBytes);
        if (afterLoadRequest)
        {
            afterLoadRequest(layout.baseAddress, layout.baseBytes);
            if (layout.mipBytes)
                afterLoadRequest(layout.mipAddress, layout.mipBytes);
        }

        // Staging buffer laid out as the texture's copyable footprints (subresource = level + slice * levels).
        D3D12_RESOURCE_DESC description = texture.resource->GetDesc();
        const uint32_t subresources = layout.levels * layout.slices;
        m_footprints.resize(subresources);
        UINT64 totalSize = 0;
        m_device->GetCopyableFootprints(&description, 0, subresources, 0, m_footprints.data(), nullptr, nullptr, &totalSize);
        if (totalSize > m_stagingSize)
        {
            if (m_staging)
                m_retiredStaging.push_back(std::move(m_staging));
            m_stagingSize = std::max<uint64_t>((totalSize + 0xFFFFF) & ~uint64_t(0xFFFFF), 16ull << 20);
            m_staging = CreateBuffer(m_device, m_stagingSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            m_stagingState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        D3D12_RESOURCE_BARRIER barriers[2];
        uint32_t barrierCount = 0;
        if (m_stagingState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            barriers[barrierCount++] = TransitionBarrier(m_staging.Get(), m_stagingState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_stagingState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (barrierCount)
            list->ResourceBarrier(barrierCount, barriers);
        m_sharedMemory->Transition(list, SharedMemory::kReadState);

        if (key.scale > 1)
        {
            // From the scaled resolve copies: one level, one slice, 4 or 8 bytes per texel.
            m_scaledResolves->TransitionPageTable(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_scaledResolves->TransitionPool(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->SetComputeRootSignature(m_scaledUntileRootSignature.Get());
            list->SetPipelineState(m_scaledUntilePipeline.Get());
            list->SetComputeRootShaderResourceView(1, m_scaledResolves->PageTableAddress());
            list->SetComputeRootShaderResourceView(2, m_scaledResolves->PoolAddress());
            list->SetComputeRootUnorderedAccessView(3, m_staging->GetGPUVirtualAddress());
            const xenos::TextureLevel& guest = layout.level[0];
            UntileConstants constants{};
            constants.base = guest.address;
            constants.pitch = guest.pitchBlocks;
            constants.width = guest.widthBlocks;
            constants.height = guest.heightBlocks;
            constants.log2Bpb = log2Bpb;
            constants.endian = key.endian;
            constants.tiled = 1;
            constants.destRowPitch = m_footprints[0].Footprint.RowPitch;
            constants.offsetX = guest.offsetX;
            constants.offsetY = guest.offsetY;
            constants.conversion = format.conversion;
            constants.destOffset = uint32_t(m_footprints[0].Offset);
            constants.scale = key.scale;
            list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            list->Dispatch((guest.widthBlocks * key.scale + 7) / 8, (guest.heightBlocks * key.scale + 7) / 8, 1);
        }
        else
        {
        list->SetComputeRootSignature(m_untileRootSignature.Get());
        list->SetPipelineState(m_untilePipeline.Get());
        list->SetComputeRootShaderResourceView(1, m_sharedMemory->GpuAddress());
        list->SetComputeRootUnorderedAccessView(2, m_staging->GetGPUVirtualAddress());
        for (uint32_t slice = 0; slice < layout.slices; slice++)
        {
            for (uint32_t level = 0; level < layout.levels; level++)
            {
                const xenos::TextureLevel& guest = layout.level[level];
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = m_footprints[level + slice * layout.levels];
                UntileConstants constants{};
                constants.base = guest.address + slice * guest.sliceBytes;
                constants.pitch = guest.pitchBlocks;
                constants.width = guest.widthBlocks;
                constants.height = guest.heightBlocks;
                constants.log2Bpb = log2Bpb;
                constants.endian = key.endian;
                constants.tiled = key.tiled;
                constants.destRowPitch = footprint.Footprint.RowPitch;
                constants.offsetX = guest.offsetX;
                constants.offsetY = guest.offsetY;
                constants.linearRowBytes = guest.rowBytes;
                constants.conversion = format.conversion;
                constants.destOffset = uint32_t(footprint.Offset);
                list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
                const uint32_t threadsX = log2Bpb < 2 ? (guest.widthBlocks + (4 >> log2Bpb) - 1) / (4 >> log2Bpb) : guest.widthBlocks;
                list->Dispatch((threadsX + 7) / 8, (guest.heightBlocks + 7) / 8, 1);
            }
        }
        }

        barrierCount = 0;
        barriers[barrierCount++] = TransitionBarrier(m_staging.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_stagingState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (texture.state != D3D12_RESOURCE_STATE_COPY_DEST)
            barriers[barrierCount++] = TransitionBarrier(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(barrierCount, barriers);
        for (uint32_t subresource = 0; subresource < subresources; subresource++)
        {
            D3D12_TEXTURE_COPY_LOCATION destination{ texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            destination.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION source{ m_staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            source.PlacedFootprint = m_footprints[subresource];
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_STATES readState =
            D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers[0] = TransitionBarrier(texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readState);
        list->ResourceBarrier(1, barriers);
        texture.state = readState;
        loads++;
        profile::Count(profile::Counter::kTextureLoads);
    }

    uint64_t TextureCache::Version(const Texture& texture)
    {
        const xenos::TextureLayout& layout = texture.layout;
        return watch::RangeVersion(layout.baseAddress, layout.baseBytes) + (layout.mipBytes ? watch::RangeVersion(layout.mipAddress, layout.mipBytes) : 0);
    }

    uint32_t TextureCache::GetSrv(ID3D12GraphicsCommandList* list, UploadAllocator& upload, const xenos::TextureFetch& fetch, uint32_t shaderDimension,
        uint32_t* scale)
    {
        if (scale)
            *scale = 1;
        // 2D (1D as 2D) and cube textures, viewed as the shader declares them (no 3D or stacked yet).
        const uint32_t dimension = fetch.Dimension();
        const bool cube = dimension == 3;
        if (fetch.Type() == 2 && dimension == 2)
        {
            static std::unordered_set<uint32_t> reported;
            if (reported.insert(fetch.BaseAddress()).second)
                LOG_WARN("GPU: %s texture %ux%ux%u, format %u, %s, pitch %u, endian %u, mips %u-%u at %08X, base %08X (bound as null)",
                    fetch.Stacked() ? "stacked" : "3D", fetch.Width(), fetch.Height(), fetch.Depth(), uint32_t(fetch.Format()),
                    fetch.Tiled() ? "tiled" : "linear", fetch.Pitch(), fetch.Endian(), fetch.MipMinLevel(), fetch.MipMaxLevel(), fetch.MipAddress(),
                    fetch.BaseAddress());
        }
        if (fetch.Type() != 2 || dimension == 2 || shaderDimension == 2 || cube != (shaderDimension == 3))
        {
            static bool reported[4][4];
            if (fetch.Type() == 2 && (cube || shaderDimension == 3 || shaderDimension == 2) && dimension != 2 && !reported[dimension][shaderDimension & 3])
            {
                reported[dimension][shaderDimension & 3] = true;
                LOG_WARN("GPU: texture at %08X has dimension %u but is sampled as dimension %u (bound as null)", fetch.BaseAddress(), dimension,
                    shaderDimension);
            }
            return NullSrv(shaderDimension);
        }
        const xenos::FormatInfo& format = xenos::GetFormatInfo(fetch.Format());
        if (!format.bytesPerBlock)
        {
            static bool reported[64];
            if (!reported[uint32_t(fetch.Format())])
            {
                reported[uint32_t(fetch.Format())] = true;
                LOG_WARN("GPU: texture format %u not supported", uint32_t(fetch.Format()));
            }
            return NullSrv(shaderDimension);
        }

        Key key{};
        key.scale = 1;
        key.base = fetch.BaseAddress();
        key.format = uint32_t(fetch.Format());
        key.endian = fetch.Endian();
        key.tiled = fetch.Tiled();
        key.pitch = fetch.Pitch();
        key.width = fetch.Width();
        key.height = fetch.Height();
        key.dimension = cube ? 3 : 1;
        key.packed = fetch.PackedMips();
        // Mips (when there's a mip address), up to the maximum level the fetch allows.
        key.levels = 1;
        if (fetch.MipAddress() && fetch.MipMaxLevel())
        {
            const uint32_t sizeLevels = 32 - std::countl_zero(std::max(key.width, key.height));
            key.levels = std::min({ fetch.MipMaxLevel() + 1, sizeLevels, xenos::TextureLayout::kMaxLevels });
            key.mipAddress = fetch.MipAddress();
        }
        // Resolution scaling: a texture resolves wrote (a render target sampled later, the front buffer) loads from the
        // scaled copies when every page of it has a current one with texels of its size.
        const uint32_t resolutionScale = m_scaledResolves->Scale();
        if (resolutionScale > 1 && key.levels == 1 && key.tiled && !cube && format.blockWidth == 1 && format.blockHeight == 1 &&
            (format.bytesPerBlock == 4 || format.bytesPerBlock == 8))
        {
            const xenos::TextureLayout layout =
                xenos::GetTextureLayout(format, key.base, 0, key.width, key.height, key.pitch, true, false, 1, 1);
            if (m_scaledResolves->Covers(layout.baseAddress, layout.baseBytes, uint32_t(std::countr_zero(format.bytesPerBlock))))
                key.scale = resolutionScale;
        }
        if (scale)
            *scale = key.scale;

        auto it = m_textures.find(key);
        Texture* texture = it != m_textures.end() ? it->second.get() : CreateTexture(key);
        if (!texture)
            return NullSrv(shaderDimension);
        // (When verifying the watch, requesting the range checks the pages and bumps stale ones' versions.)
        if (watch::Verifying() && texture->version != UINT64_MAX)
        {
            m_sharedMemory->RequestRange(upload, texture->layout.baseAddress, texture->layout.baseBytes);
            if (texture->layout.mipBytes)
                m_sharedMemory->RequestRange(upload, texture->layout.mipAddress, texture->layout.mipBytes);
        }
        if (texture->version == UINT64_MAX || Version(*texture) != texture->version)
            Load(list, upload, *texture);

        // Signed components (the fetch's sign modes) read through the format's SNORM view: e.g. DOA4's water normal maps,
        // k_8_8 centered on 0x40, +0.5 signed. That needs the format's components all signed and a SNORM format; other
        // modes (biased, gamma) and mixes read unsigned.
        uint32_t signs[4] = {}, present = 0, signedCount = 0;
        for (uint32_t i = 0; i < 4; i++)
        {
            if (format.channels[i] >= 4)
                continue;
            signs[present] = fetch.Sign(i);
            signedCount += signs[present] == 1;
            present++;
        }
        const bool snorm = signedCount == present && present && format.snormFormat;
        // (Float formats are signed anyway, whatever their sign modes say.)
        const DXGI_FORMAT host = DXGI_FORMAT(format.dxgiFormat);
        const bool floating = host == DXGI_FORMAT_R16_FLOAT || host == DXGI_FORMAT_R16G16_FLOAT || host == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            host == DXGI_FORMAT_R32_FLOAT || host == DXGI_FORMAT_R32G32_FLOAT || host == DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (!snorm && !floating && std::any_of(signs, signs + present, [](uint32_t sign) { return sign != 0; }))
        {
            static std::unordered_set<uint32_t> reported;
            const uint32_t pattern = uint32_t(fetch.Format()) | (((fetch.d[0] >> 2) & 0xFF) << 8);
            if (reported.insert(pattern).second)
                LOG_WARN("GPU: texture format %u with sign modes %u %u %u %u read unsigned (at %08X)", uint32_t(fetch.Format()), fetch.Sign(0),
                    fetch.Sign(1), fetch.Sign(2), fetch.Sign(3), fetch.BaseAddress());
        }

        // (Not applied: the fetch's exponent adjustment, a power of two the results are scaled by. DOA4's are 0.)
        if (fetch.ExpAdjust() != 0)
        {
            static std::unordered_set<uint32_t> reported;
            if (reported.insert(uint32_t(fetch.Format()) | (uint32_t(fetch.ExpAdjust() & 0x3F) << 8)).second)
                LOG_WARN("GPU: texture format %u with exponent adjustment %d read without it (at %08X)", uint32_t(fetch.Format()),
                    fetch.ExpAdjust(), fetch.BaseAddress());
        }

        const uint32_t swizzle = fetch.Swizzle();
        const uint32_t viewKey = swizzle | (snorm ? 1u << 12 : 0u);
        auto srv = texture->srvs.find(viewKey);
        if (srv != texture->srvs.end())
            return srv->second;

        uint32_t mapping[4];
        for (uint32_t i = 0; i < 4; i++)
        {
            const uint32_t select = (swizzle >> (i * 3)) & 7;
            mapping[i] = select < 4 ? format.channels[select] : select == 5 ? 5 : 4;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.Format = DXGI_FORMAT(snorm ? format.snormFormat : format.dxgiFormat);
        description.ViewDimension = cube ? D3D12_SRV_DIMENSION_TEXTURECUBE : D3D12_SRV_DIMENSION_TEXTURE2D;
        description.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(mapping[0], mapping[1], mapping[2], mapping[3]);
        if (cube)
            description.TextureCube.MipLevels = key.levels;
        else
            description.Texture2D.MipLevels = key.levels;
        const uint32_t index = m_views->AllocatePersistent(1);
        m_device->CreateShaderResourceView(texture->resource.Get(), &description, m_views->Cpu(index));
        texture->srvs[viewKey] = index;
        return index;
    }

    void TextureCache::DropScaled()
    {
        for (auto it = m_textures.begin(); it != m_textures.end();)
        {
            if (it->first.scale > 1)
            {
                for (const auto& [swizzle, srv] : it->second->srvs)
                    m_views->FreePersistent(srv);
                it = m_textures.erase(it);
            }
            else
                ++it;
        }
    }

    uint32_t TextureCache::GetSampler(const xenos::TextureFetch& fetch, const shader::SamplerBinding& binding)
    {
        const uint32_t mag = binding.magFilter == 3 ? fetch.MagFilter() : binding.magFilter;
        const uint32_t min = binding.minFilter == 3 ? fetch.MinFilter() : binding.minFilter;
        const uint32_t mip = binding.mipFilter == 3 ? fetch.MipFilter() : binding.mipFilter;
        const uint32_t aniso = binding.anisoFilter == 7 ? fetch.AnisoFilter() : binding.anisoFilter;

        D3D12_SAMPLER_DESC description{};
        if (aniso > 1)
        {
            description.Filter = D3D12_FILTER_ANISOTROPIC;
            description.MaxAnisotropy = std::min(1u << (aniso - 1), 16u);
        }
        else
        {
            description.Filter = D3D12_ENCODE_BASIC_FILTER(min == 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                mag == 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT, mip == 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                D3D12_FILTER_REDUCTION_TYPE_STANDARD);
            description.MaxAnisotropy = 1;
        }
        description.AddressU = AddressMode(fetch.ClampX());
        description.AddressV = AddressMode(fetch.ClampY());
        description.AddressW = AddressMode(fetch.ClampZ());
        description.MipLODBias = fetch.LodBias();
        description.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        const float white = fetch.BorderColor() == 1 ? 1.0f : 0.0f;
        description.BorderColor[0] = description.BorderColor[1] = description.BorderColor[2] = white;
        description.BorderColor[3] = fetch.BorderColor() == 0 ? 0.0f : 1.0f;
        description.MinLOD = float(fetch.MipMinLevel());
        description.MaxLOD = mip == 2 ? description.MinLOD : float(std::max(fetch.MipMaxLevel(), fetch.MipMinLevel()));
        // Enhancements: `anisotropy` (2-16) filters linearly minified textures anisotropically (which also makes mip
        // filtering linear); LOD_BIAS=x adds x to every texture's LOD bias (DOA4's stage textures use -1.5).
        static const float lodBias = [] {
            const char* setting = std::getenv(GAME_PREFIX "_LOD_BIAS");
            return setting ? float(std::atof(setting)) : 0.0f;
        }();
        if (anisotropy > description.MaxAnisotropy && min == 1)
        {
            description.Filter = D3D12_FILTER_ANISOTROPIC;
            description.MaxAnisotropy = anisotropy;
        }
        description.MipLODBias += lodBias;
        // DEBUG_MIP_LEVEL=n samples level n (or the last there is) of every texture, to check mip layouts.
        static const int forcedLevel = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_MIP_LEVEL");
            return setting ? std::atoi(setting) : -1;
        }();
        if (forcedLevel >= 0)
            description.MinLOD = description.MaxLOD = float(forcedLevel);

        uint64_t hash = 0xCBF29CE484222325ull;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&description);
        for (size_t i = 0; i < sizeof(description); i++)
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        auto it = m_samplerSlots.find(hash);
        if (it != m_samplerSlots.end())
            return it->second;
        const uint32_t index = m_samplers->AllocatePersistent(1);
        m_device->CreateSampler(&description, m_samplers->Cpu(index));
        m_samplerSlots[hash] = index;
        return index;
    }
}
