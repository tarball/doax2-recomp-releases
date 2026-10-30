#include "gpu/d3d12/render_targets.h"

#include "gpu/memory_watch.h"
#include "gpu/registers.h"
#include "gpu/renderer.h"
#include "guest_memory.h"
#include "log.h"
#include "util/profiler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace gpu::d3d12
{
    namespace
    {
        constexpr uint32_t kEdramTiles = 2048;
        constexpr uint32_t kEdramTileBytes = 5120; // 80x16 samples of 32 bits

        // Copies a rectangle of a render target into guest memory as a tiled texture. Depth sources are
        // written as the EDRAM stores them: 24-bit depth (unorm or 20e4 float) above 8-bit stencil.
        // Compiled twice: with XE_MSAA for multisampled sources.
        const char kResolveShader[] = R"(
#ifdef XE_MSAA
Texture2DMS<float4> xe_source : register(t0);
#else
Texture2D<float4> xe_source : register(t0);
#endif
RWByteAddressBuffer xe_dest : register(u0);
// Resolution scaling (see scaled_resolves.h): guest page -> pool page + 1, and the pool of scaled copies.
ByteAddressBuffer xe_page_table : register(t1);
RWByteAddressBuffer xe_scaled : register(u1);
cbuffer XeResolveConstants : register(b0)
{
    uint2 xe_source_origin;
    uint2 xe_size;
    uint2 xe_dest_origin;
    uint xe_dest_base;
    uint xe_dest_pitch;    // pixels, multiple of 32
    uint xe_dest_format;   // Xenos texture format
    uint xe_dest_endian;
    uint xe_swap_red_blue;
    float xe_scale;        // 2^exp_bias (and render target range scaling)
    uint xe_sample_select; // 0-3 one sample, 4 average of 0 and 1, 5 of 2 and 3, 6 of all
    uint xe_samples;
    uint xe_source_depth;  // 0 color, 1 unorm depth, 2 20e4 float depth
    uint xe_resolution_scale; // host pixels per guest pixel in each direction
    uint xe_scaled_pass;      // 1: a thread per host pixel, writing the scaled copy
};

float4 XeLoadHost(int2 p)
{
#ifdef XE_MSAA
    uint last = xe_samples - 1u;
    if (xe_sample_select < 4u)
        return xe_source.Load(p, min(xe_sample_select, last));
    if (xe_sample_select == 4u)
        return (xe_source.Load(p, 0) + xe_source.Load(p, min(1u, last))) * 0.5;
    if (xe_sample_select == 5u)
        return (xe_source.Load(p, min(2u, last)) + xe_source.Load(p, min(3u, last))) * 0.5;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (uint i = 0u; i < xe_samples; i++)
        sum += xe_source.Load(p, i);
    return sum / float(xe_samples);
#else
    return xe_source.Load(int3(p, 0));
#endif
}

// A guest pixel of the source: with resolution scaling, the average of its block of host pixels (depth: the first).
float4 XeLoadSource(int2 p)
{
    int scale = int(xe_resolution_scale);
    if (scale == 1 || xe_source_depth != 0u)
        return XeLoadHost(p * scale);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (int y = 0; y < scale; y++)
    {
        for (int x = 0; x < scale; x++)
            sum += XeLoadHost(p * scale + int2(x, y));
    }
    return sum / float(scale * scale);
}

uint XeTiledOffset2D(uint x, uint y, uint pitch, uint log_bpp)
{
    uint macro = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << (log_bpp + 7u);
    uint micro = ((x & 7u) + ((y & 6u) << 2u)) << log_bpp;
    uint offset = macro + ((micro & ~15u) << 1u) + (micro & 15u) + ((y & 8u) << (3u + log_bpp)) + ((y & 1u) << 4u);
    return ((offset & ~511u) << 3u) + ((offset & 448u) << 2u) + (offset & 63u) + ((y & 16u) << 7u) + (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u);
}

uint XeSwap(uint v, uint endian)
{
    if (endian == 1u)
        return ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
    if (endian == 2u)
        return (v << 24u) | ((v & 0xFF00u) << 8u) | ((v >> 8u) & 0xFF00u) | (v >> 24u);
    if (endian == 3u)
        return (v << 16u) | (v >> 16u);
    return v;
}

uint XeUnorm(float v, float scale)
{
    return uint(saturate(v) * scale + 0.5);
}

// To Xenos 20e4 float depth (4-bit exponent, 20-bit mantissa), rounding to nearest even.
uint XeFloat32To20e4(float f)
{
    uint u = asuint(f);
    if (int(u) <= 0)
        return 0u;
    u = min(u, 0x3FFFFFF8u);
    if (u < 0x38800000u)
        u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23u), 24u);
    else
        u += 0xC8000000u;
    u += 3u + ((u >> 3u) & 1u);
    return (u >> 3u) & 0xFFFFFFu;
}

// Where the host texel `sub` of the guest texel at `address` is in the scaled copy (0xFFFFFFFF if the page has none).
uint XeScaledAddress(uint address, uint2 sub, uint log_bpp)
{
    uint entry = xe_page_table.Load((address >> 12u) << 2u);
    if (entry == 0u)
        return 0xFFFFFFFFu;
    uint scale = xe_resolution_scale;
    return (((entry - 1u) << 12u) + (address & 4095u)) * scale * scale + ((sub.y * scale + sub.x) << log_bpp);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint scale = xe_resolution_scale;
    bool scaled = xe_scaled_pass != 0u;
    if (any(id.xy >= (scaled ? xe_size * scale : xe_size)))
        return;
    float4 c = scaled ? XeLoadHost(int2(xe_source_origin * scale + id.xy)) : XeLoadSource(int2(xe_source_origin + id.xy));
    uint2 d = xe_dest_origin + (scaled ? id.xy / scale : id.xy);
    uint2 packed = uint2(0u, 0u);
    uint log_bpp = 2u;
    if (xe_source_depth != 0u)
    {
        // (The stencil isn't read: it's 0.)
        packed.x = (xe_source_depth == 1u ? uint(saturate(c.x) * 16777215.0 + 0.5) : XeFloat32To20e4(c.x)) << 8u;
    }
    else
    {
    c *= xe_scale;
    if (xe_swap_red_blue != 0u)
        c = c.zyxw;
    switch (xe_dest_format)
    {
    case 2u: // k_8: alpha (DOAX2's 8-bit targets are D3D's A8, drawn with the value in alpha and sampled as 000X)
        packed.x = XeUnorm(c.w, 255.0);
        log_bpp = 0u;
        break;
    case 4u: // k_5_6_5 (X in the low bits, as textures load it)
        packed.x = XeUnorm(c.x, 31.0) | (XeUnorm(c.y, 63.0) << 5u) | (XeUnorm(c.z, 31.0) << 11u);
        log_bpp = 1u;
        break;
    case 6u: // k_8_8_8_8
    case 62u:
        packed.x = XeUnorm(c.x, 255.0) | (XeUnorm(c.y, 255.0) << 8u) | (XeUnorm(c.z, 255.0) << 16u) | (XeUnorm(c.w, 255.0) << 24u);
        break;
    case 7u: // k_2_10_10_10
        packed.x = XeUnorm(c.x, 1023.0) | (XeUnorm(c.y, 1023.0) << 10u) | (XeUnorm(c.z, 1023.0) << 20u) | (XeUnorm(c.w, 3.0) << 30u);
        break;
    case 25u: // k_16_16
        packed.x = XeUnorm(c.x, 65535.0) | (XeUnorm(c.y, 65535.0) << 16u);
        break;
    case 26u: // k_16_16_16_16
        packed = uint2(XeUnorm(c.x, 65535.0) | (XeUnorm(c.y, 65535.0) << 16u), XeUnorm(c.z, 65535.0) | (XeUnorm(c.w, 65535.0) << 16u));
        log_bpp = 3u;
        break;
    case 31u: // k_16_16_FLOAT
        packed.x = f32tof16(c.x) | (f32tof16(c.y) << 16u);
        break;
    case 32u: // k_16_16_16_16_FLOAT
        packed = uint2(f32tof16(c.x) | (f32tof16(c.y) << 16u), f32tof16(c.z) | (f32tof16(c.w) << 16u));
        log_bpp = 3u;
        break;
    case 36u: // k_32_FLOAT
        packed.x = asuint(c.x);
        break;
    case 37u: // k_32_32_FLOAT
        packed = asuint(c.xy);
        log_bpp = 3u;
        break;
    default:
        return;
    }
    }
    uint address = xe_dest_base + XeTiledOffset2D(d.x, d.y, xe_dest_pitch, log_bpp);
    if (log_bpp < 2u)
    {
        // A texel smaller than a dword, merged into the dword other pixels' threads write too: placed in the dword as
        // textures read it (the swap applied), with its mask. (Resolves to 8- and 16-bit formats have no scaled copy.)
        uint shift = (address & (log_bpp == 0u ? 3u : 2u)) << 3u;
        uint mask = XeSwap((log_bpp == 0u ? 0xFFu : 0xFFFFu) << shift, xe_dest_endian);
        xe_dest.InterlockedAnd(address & ~3u, ~mask);
        xe_dest.InterlockedOr(address & ~3u, XeSwap(packed.x << shift, xe_dest_endian));
        return;
    }
    uint2 words = uint2(XeSwap(packed.x, xe_dest_endian), 0u);
    if (log_bpp == 3u)
    {
        // 8-in-64 swaps the dwords too.
        uint endian = xe_dest_endian == 4u ? 2u : xe_dest_endian;
        words = uint2(XeSwap(packed.x, endian), XeSwap(packed.y, endian));
        if (xe_dest_endian == 4u)
            words = words.yx;
    }
    if (scaled)
    {
        address = XeScaledAddress(address, id.xy % scale, log_bpp);
        if (address == 0xFFFFFFFFu)
            return;
        if (log_bpp == 3u)
            xe_scaled.Store2(address, words);
        else
            xe_scaled.Store(address, words.x);
    }
    else if (log_bpp == 3u)
    {
        xe_dest.Store2(address, words);
    }
    else
    {
        xe_dest.Store(address, words.x);
    }
}
)";

        struct ResolveConstants
        {
            uint32_t sourceX, sourceY, width, height, destX, destY, destBase, destPitch, destFormat, destEndian, swapRedBlue;
            float scale;
            uint32_t sampleSelect, samples;
            uint32_t sourceDepth;
            uint32_t resolutionScale;
            uint32_t scaledPass;
        };

        // EDRAM transfers (see render_targets.h): a triangle over the destination, each of its samples (host pixels,
        // with resolution scaling) taking the source's sample at the same place in EDRAM. A tile holds 80x16 samples
        // (40x16 of 64 bits); a pixel's samples are 1x2 with 2x MSAA, 2x2 with 4x. Between formats, a sample goes through
        // the 32-bit word EDRAM holds (mode 1): 8-bit and 10-bit channels from the low bits up, 16-bit ones low half
        // first, depth in the high 24 bits above an 8-bit stencil. Compiled with XE_SOURCE_MSAA for a multisampled source,
        // XE_DEPTH for a depth destination, and XE_STENCIL to write the stencil too (SV_StencilRef). Where pixel shaders
        // can't write it, XE_STENCIL_BIT makes the passes that set the stencil a bit at a time: each writes only bit
        // xe_stencil_bit, and discards the samples whose source stencil has it clear.
        const char kTransferVertexShader[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    return float4(float2((id << 1u) & 2u, id & 2u) * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

        const char kTransferPixelShader[] = R"(
cbuffer XeTransferConstants : register(b0)
{
    uint xe_dest_base;          // tiles
    uint xe_dest_pitch_tiles;
    uint xe_dest_log2_samples;  // the guest's
    uint xe_dest_sample_ratio;  // host samples per guest sample
    uint xe_source_base;
    uint xe_source_pitch_tiles;
    uint xe_source_log2_samples;
    uint xe_source_sample_ratio;
    uint xe_tile_samples_x;     // 80, or 40 for 64-bit samples
    uint xe_scale;              // resolution scale
    uint xe_mode;               // 0: the same format, values as they are; 1: through EDRAM's 32-bit words
    uint xe_source_format;      // mode 1: RawFormat's codes
    uint xe_dest_format;
    uint xe_stencil_bit;        // XE_STENCIL_BIT
};
cbuffer XeTransferMask : register(b1)
{
    uint4 xe_mask[16]; // a bit per EDRAM tile: those to copy
};
#ifdef XE_SOURCE_MSAA
Texture2DMS<float4> xe_source : register(t0);
Texture2DMS<uint2> xe_source_stencil : register(t1);
#else
Texture2D<float4> xe_source : register(t0);
Texture2D<uint2> xe_source_stencil : register(t1);
#endif

uint XeUnorm(float v, float scale)
{
    return uint(saturate(v) * scale + 0.5);
}

uint XeSnorm16(float v)
{
    return uint(int(round(clamp(v, -1.0, 1.0) * 32767.0))) & 0xFFFFu;
}

// Float depth: Xenos 20e4 (4-bit exponent, 20-bit mantissa), rounded to nearest even, and back.
uint XeFloat32To20e4(float f)
{
    uint u = asuint(f);
    if (int(u) <= 0)
        return 0u;
    u = min(u, 0x3FFFFFF8u);
    if (u < 0x38800000u)
        u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23u), 24u);
    else
        u += 0xC8000000u;
    u += 3u + ((u >> 3u) & 1u);
    return (u >> 3u) & 0xFFFFFFu;
}

float XeFloat20e4To32(uint f)
{
    uint exponent = (f >> 20u) & 0xFu, mantissa = f & 0xFFFFFu;
    if (exponent == 0u)
        return float(mantissa) * exp2(-34.0);
    return (1.0 + float(mantissa) * exp2(-20.0)) * exp2(float(exponent) - 15.0);
}

// A sample as EDRAM holds it.
uint XeWord(float4 value, uint stencil)
{
    switch (xe_source_format)
    {
    case 0u:
        return XeUnorm(value.x, 255.0) | (XeUnorm(value.y, 255.0) << 8u) | (XeUnorm(value.z, 255.0) << 16u) | (XeUnorm(value.w, 255.0) << 24u);
    case 1u:
        return XeUnorm(value.x, 1023.0) | (XeUnorm(value.y, 1023.0) << 10u) | (XeUnorm(value.z, 1023.0) << 20u) | (XeUnorm(value.w, 3.0) << 30u);
    case 2u:
        return XeSnorm16(value.x) | (XeSnorm16(value.y) << 16u);
    case 3u:
        return f32tof16(value.x) | (f32tof16(value.y) << 16u);
    case 4u:
        return asuint(value.x);
    case 8u:
        return (XeUnorm(value.x, 16777215.0) << 8u) | (stencil & 0xFFu);
    default:
        return (XeFloat32To20e4(value.x) << 8u) | (stencil & 0xFFu);
    }
}

float XeSnorm16ToFloat(uint v)
{
    return max(float(int(v << 16u) >> 16) / 32767.0, -1.0);
}

float4 XeColorFromWord(uint w)
{
    switch (xe_dest_format)
    {
    case 0u:
        return float4(w & 0xFFu, (w >> 8u) & 0xFFu, (w >> 16u) & 0xFFu, w >> 24u) / 255.0;
    case 1u:
        return float4(w & 0x3FFu, (w >> 10u) & 0x3FFu, (w >> 20u) & 0x3FFu, w >> 30u) / float4(1023.0, 1023.0, 1023.0, 3.0);
    case 2u:
        return float4(XeSnorm16ToFloat(w & 0xFFFFu), XeSnorm16ToFloat(w >> 16u), 0.0, 0.0);
    case 3u:
        return float4(f16tof32(w & 0xFFFFu), f16tof32(w >> 16u), 0.0, 0.0);
    default:
        return float4(asfloat(w), 0.0, 0.0, 0.0);
    }
}

struct XeOutput
{
#ifdef XE_DEPTH
    float depth : SV_Depth;
#ifdef XE_STENCIL
    uint stencil : SV_StencilRef;
#endif
#else
    float4 color : SV_Target0;
#endif
};

#ifdef XE_STENCIL_BIT
void main(float4 position : SV_Position, uint host_sample : SV_SampleIndex)
#else
XeOutput main(float4 position : SV_Position, uint host_sample : SV_SampleIndex)
#endif
{
    uint2 host = uint2(position.xy);
    uint2 pixel = host / xe_scale, sub = host % xe_scale;
    uint sample = host_sample / xe_dest_sample_ratio;
    // This sample's tile in the destination, and its place in the tile.
    uint tile_width = xe_dest_log2_samples == 2u ? xe_tile_samples_x >> 1u : xe_tile_samples_x;
    uint2 tile = uint2(pixel.x / tile_width, pixel.y >> (xe_dest_log2_samples == 0u ? 4u : 3u));
    uint2 in_tile;
    if (xe_dest_log2_samples == 0u)
        in_tile = uint2(pixel.x % tile_width, pixel.y & 15u);
    else if (xe_dest_log2_samples == 1u)
        in_tile = uint2(pixel.x % tile_width, ((pixel.y & 7u) << 1u) | (sample & 1u));
    else
        in_tile = uint2(((pixel.x % tile_width) << 1u) | (sample & 1u), ((pixel.y & 7u) << 1u) | ((sample >> 1u) & 1u));
    uint index = xe_dest_base + tile.y * xe_dest_pitch_tiles + tile.x;
    if (tile.x >= xe_dest_pitch_tiles || index >= 2048u || (xe_mask[index >> 7u][(index >> 5u) & 3u] & (1u << (index & 31u))) == 0u)
        discard;
    // The same place in the source.
    uint offset = index - xe_source_base;
    uint2 source_tile = uint2(offset % xe_source_pitch_tiles, offset / xe_source_pitch_tiles);
    uint2 source_pixel;
    uint source_sample;
    if (xe_source_log2_samples == 0u)
    {
        source_pixel = source_tile * uint2(xe_tile_samples_x, 16u) + in_tile;
        source_sample = 0u;
    }
    else if (xe_source_log2_samples == 1u)
    {
        source_pixel = source_tile * uint2(xe_tile_samples_x, 8u) + uint2(in_tile.x, in_tile.y >> 1u);
        source_sample = in_tile.y & 1u;
    }
    else
    {
        source_pixel = source_tile * uint2(xe_tile_samples_x >> 1u, 8u) + (in_tile >> 1u);
        source_sample = (in_tile.x & 1u) | ((in_tile.y & 1u) << 1u);
    }
    int2 p = int2(source_pixel * xe_scale + sub);
    uint s = source_sample * xe_source_sample_ratio;
#ifdef XE_SOURCE_MSAA
    float4 value = xe_source.Load(p, s);
#else
    float4 value = xe_source.Load(int3(p, 0));
#endif
    // The source's stencil (depth sources: the same format into depth, or words from depth).
    uint stencil = 0u;
#ifdef XE_DEPTH
    if (xe_mode == 0u || xe_source_format >= 8u)
#else
    if (xe_mode != 0u && xe_source_format >= 8u)
#endif
    {
#ifdef XE_SOURCE_MSAA
        stencil = xe_source_stencil.Load(p, s).y;
#else
        stencil = xe_source_stencil.Load(int3(p, 0)).y;
#endif
    }
#ifdef XE_STENCIL_BIT
    if (((xe_mode == 0u ? stencil : XeWord(value, stencil)) & (1u << xe_stencil_bit)) == 0u)
        discard;
#else
    XeOutput output;
    if (xe_mode == 0u)
    {
#ifdef XE_DEPTH
        output.depth = value.x;
#ifdef XE_STENCIL
        output.stencil = stencil;
#endif
#else
        output.color = value;
#endif
    }
    else
    {
        uint word = XeWord(value, stencil);
#ifdef XE_DEPTH
        output.depth = xe_dest_format == 8u ? float(word >> 8u) / 16777215.0 : XeFloat20e4To32(word >> 8u);
#ifdef XE_STENCIL
        output.stencil = word & 0xFFu;
#endif
#else
        output.color = XeColorFromWord(word);
#endif
    }
    return output;
#endif
}
)";

        struct TransferConstants
        {
            uint32_t destBase, destPitchTiles, destLog2Samples, destSampleRatio;
            uint32_t sourceBase, sourcePitchTiles, sourceLog2Samples, sourceSampleRatio;
            uint32_t tileSamplesX;
            uint32_t scale;
            uint32_t mode, sourceFormat, destFormat;
            uint32_t stencilBit;
        };

        // A target's EDRAM tiles: their size in pixels, and how many make a row of the surface.
        struct TileLayout
        {
            uint32_t width, height, pitchTiles;
        };

        TileLayout LayoutOf(const RenderTargetCache::Target& target)
        {
            const uint32_t samplesX = !target.depth && xenos::Is64bpp(xenos::ColorFormat(target.format)) ? 40 : 80;
            const uint32_t width = target.samples >= 4 ? samplesX / 2 : samplesX;
            const uint32_t height = target.samples >= 2 ? 8 : 16;
            return { width, height, (target.pitch + width - 1) / width };
        }

        // A target's samples as EDRAM words, for transfers between formats (the shader's codes): 0 8_8_8_8, 1
        // 2_10_10_10, 2 16_16 (fixed), 3 16_16 float, 4 32 float, 8 24-bit depth, 9 20e4 float depth. UINT32_MAX for
        // 64-bit formats and 2_10_10_10_FLOAT, which the host keeps in 16-bit floats.
        uint32_t RawFormat(const RenderTargetCache::Target& target)
        {
            if (target.depth)
                return xenos::DepthFormat(target.format) == xenos::DepthFormat::kD24FS8 ? 9 : 8;
            switch (xenos::ColorFormat(target.format))
            {
            case xenos::ColorFormat::k_8_8_8_8:
            case xenos::ColorFormat::k_8_8_8_8_GAMMA:
                return 0;
            case xenos::ColorFormat::k_2_10_10_10:
            case xenos::ColorFormat::k_2_10_10_10_AS_10_10_10_10:
                return 1;
            case xenos::ColorFormat::k_16_16:
                return 2;
            case xenos::ColorFormat::k_16_16_FLOAT:
                return 3;
            case xenos::ColorFormat::k_32_FLOAT:
                return 4;
            default:
                return UINT32_MAX;
            }
        }

        // How one target's EDRAM contents come into another: values as they are (the same format), through EDRAM's
        // words (other 32-bit formats, color or depth: DOAX2 clears a color buffer with a depth clear of the same
        // EDRAM), or not at all (the gamma format's views encode on write, and 64-bit samples between formats).
        enum class TransferMode
        {
            kNone,
            kDirect,
            kRaw,
        };

        TransferMode ModeOf(const RenderTargetCache::Target& source, const RenderTargetCache::Target& dest)
        {
            if (dest.viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                return TransferMode::kNone;
            if (source.depth == dest.depth && source.format == dest.format)
                return TransferMode::kDirect;
            return RawFormat(source) != UINT32_MAX && RawFormat(dest) != UINT32_MAX ? TransferMode::kRaw : TransferMode::kNone;
        }

        uint32_t SwapForEndian(uint32_t value, uint32_t endian)
        {
            switch (endian)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
            case 2: return __builtin_bswap32(value);
            case 3: return (value << 16) | (value >> 16);
            default: return value;
            }
        }

        // Xenos 20e4 float depth (4-bit exponent, 20-bit mantissa) to float.
        float Float20e4ToFloat(uint32_t value)
        {
            const uint32_t exponent = (value >> 20) & 0xF, mantissa = value & 0xFFFFF;
            if (exponent == 0)
                return std::ldexp(float(mantissa) / float(1 << 20), -14);
            return std::ldexp(1.0f + float(mantissa) / float(1 << 20), int(exponent) - 15);
        }

        uint32_t ColorInfoRegister(uint32_t index)
        {
            static const uint32_t kRegisters[4] = { reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO, reg::RB_COLOR3_INFO };
            return kRegisters[index & 3];
        }
    }

    DXGI_FORMAT RenderTargetCache::HostColorFormat(xenos::ColorFormat format)
    {
        switch (format)
        {
        case xenos::ColorFormat::k_8_8_8_8:
        case xenos::ColorFormat::k_8_8_8_8_GAMMA:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case xenos::ColorFormat::k_2_10_10_10:
        case xenos::ColorFormat::k_2_10_10_10_AS_10_10_10_10:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case xenos::ColorFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        case xenos::ColorFormat::k_16_16_16_16_FLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case xenos::ColorFormat::k_16_16:
            return DXGI_FORMAT_R16G16_SNORM;
        case xenos::ColorFormat::k_16_16_16_16:
            return DXGI_FORMAT_R16G16B16A16_SNORM;
        case xenos::ColorFormat::k_16_16_FLOAT:
            return DXGI_FORMAT_R16G16_FLOAT;
        case xenos::ColorFormat::k_32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case xenos::ColorFormat::k_32_32_FLOAT:
            return DXGI_FORMAT_R32G32_FLOAT;
        default:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        }
    }

    bool RenderTargetCache::Initialize(ID3D12Device* device, DescriptorHeap& views, SharedMemory& sharedMemory, ScaledResolves& scaledResolves)
    {
        m_device = device;
        m_views = &views;
        m_sharedMemory = &sharedMemory;
        m_scaledResolves = &scaledResolves;
        if (!m_rtvs.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024) || !m_dsvs.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256))
            return false;
        m_nullView = m_rtvs.Allocate();
        D3D12_RENDER_TARGET_VIEW_DESC nullRtv{};
        nullRtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(nullptr, &nullRtv, m_nullView);

        // Resolve pipeline: constants, the source texture (table), the shared memory (raw UAV), and for resolution
        // scaling the page table (raw SRV) and the scaled pool (raw UAV).
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER parameters[5]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = { 0, 0, sizeof(ResolveConstants) / 4 };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[2].Descriptor = { 0, 0 };
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[3].Descriptor = { 1, 0 };
        parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[4].Descriptor = { 1, 0 };
        D3D12_ROOT_SIGNATURE_DESC rootDescription{ 5, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_resolveRootSignature))))
        {
            LOG_ERROR("D3D12: can't create the resolve root signature");
            return false;
        }
        for (int msaa = 0; msaa < 2; msaa++)
        {
            const std::string source = std::string(msaa ? "#define XE_MSAA 1\n" : "") + kResolveShader;
            auto shader = CompileShader(source.c_str(), "main", "cs_5_1", msaa ? "resolve (MSAA)" : "resolve");
            if (!shader)
                return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = m_resolveRootSignature.Get();
            pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
            if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_resolvePipelines[msaa]))))
            {
                LOG_ERROR("D3D12: can't create the resolve pipeline");
                return false;
            }
        }

        // EDRAM transfers: constants, the tile mask (CBV), the source and its stencil (tables).
        D3D12_DESCRIPTOR_RANGE sourceRanges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 } };
        D3D12_ROOT_PARAMETER transferParameters[4]{};
        transferParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transferParameters[0].Constants = { 0, 0, sizeof(TransferConstants) / 4 };
        transferParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        transferParameters[1].Descriptor = { 1, 0 };
        for (int i = 0; i < 2; i++)
        {
            transferParameters[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            transferParameters[2 + i].DescriptorTable = { 1, &sourceRanges[i] };
        }
        for (D3D12_ROOT_PARAMETER& parameter : transferParameters)
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC transferDescription{ 4, transferParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        if (FAILED(D3D12SerializeRootSignature(&transferDescription, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_transferRootSignature))) ||
            !(m_transferVertexShader = CompileShader(kTransferVertexShader, "main", "vs_5_1", "EDRAM transfer")))
        {
            LOG_ERROR("D3D12: can't create the EDRAM transfer root signature");
            return false;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const bool stencilRef = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
                                options.PSSpecifiedStencilRefSupported;
        m_stencilTransfer = stencilRef ? StencilTransfer::kShader : StencilTransfer::kBits;
        if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_STENCIL_TRANSFER"))
            m_stencilTransfer = std::atoi(setting) == 0 ? StencilTransfer::kNone : StencilTransfer::kBits;
        LOG_INFO("D3D12: pixel shaders %s the stencil; EDRAM transfers into depth copy it %s", stencilRef ? "can write" : "can't write",
            m_stencilTransfer == StencilTransfer::kShader ? "with the depth" : m_stencilTransfer == StencilTransfer::kBits ? "a bit at a time" : "not at all");
        return true;
    }

    void RenderTargetCache::Reset()
    {
        for (const auto& [key, target] : m_targets)
        {
            if (!target) // creation failed
                continue;
            (target->depth ? m_dsvs : m_rtvs).Free(target->view);
            m_views->FreePersistent(target->srv);
            if (target->depth)
                m_views->FreePersistent(target->stencilSrv);
        }
        m_targets.clear();
        m_bound = {};
        m_owners.fill(nullptr);
        m_ownerEpoch++;
    }

    RenderTargetCache::Target* RenderTargetCache::GetTarget(uint32_t base, uint32_t pitch, uint32_t samples, uint32_t format, bool depth)
    {
        if (pitch == 0 || base >= kEdramTiles)
            return nullptr;
        const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(samples) << 28) | (uint64_t(format) << 32) | (uint64_t(depth) << 40);
        auto& slot = m_targets[key];
        if (slot)
            return slot.get();

        auto target = std::make_unique<Target>();
        target->base = base;
        target->pitch = pitch;
        target->samples = samples;
        target->hostSamples = samples;
        target->format = format;
        target->depth = depth;
        const uint32_t bytesPerSample = !depth && xenos::Is64bpp(xenos::ColorFormat(format)) ? 8 : 4;
        const uint64_t available = uint64_t(kEdramTiles - base) * kEdramTileBytes;
        target->width = pitch;
        target->height = std::clamp<uint32_t>(uint32_t(available / (uint64_t(pitch) * samples * bytesPerSample)) & ~15u, 16, 2048);

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = target->width * scale;
        description.Height = target->height * scale;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = samples;
        D3D12_CLEAR_VALUE clear{};
        DXGI_FORMAT srvFormat;
        const uint32_t requestedSamples = samples > 1 ? std::max(samples, msaaSamples) : samples;
        if (depth)
        {
            const bool floatDepth = xenos::DepthFormat(format) == xenos::DepthFormat::kD24FS8;
            description.Format = floatDepth ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            target->viewFormat = floatDepth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
            srvFormat = floatDepth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            clear.Format = target->viewFormat;
            clear.DepthStencil = { 1.0f, 0 };
            target->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }
        else
        {
            const xenos::ColorFormat colorFormat = xenos::ColorFormat(format);
            description.Format = HostColorFormat(colorFormat);
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (description.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            {
                target->viewFormat = colorFormat == xenos::ColorFormat::k_8_8_8_8_GAMMA ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
                srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            }
            else
            {
                target->viewFormat = srvFormat = description.Format;
            }
            clear.Format = target->viewFormat;
            target->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        // An MSAA override, if the device supports that sample count for the format.
        if (requestedSamples > samples)
        {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{ target->viewFormat, requestedSamples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0 };
            if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) && levels.NumQualityLevels)
                target->hostSamples = requestedSamples;
        }
        description.SampleDesc.Count = target->hostSamples;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, target->state, &clear, IID_PPV_ARGS(&target->resource))))
        {
            LOG_ERROR("D3D12: can't create a %ux%u render target (format %u%s)", target->width * scale, target->height * scale, format, depth ? ", depth" : "");
            return nullptr;
        }

        if (depth)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = target->viewFormat;
            dsv.ViewDimension = target->hostSamples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
            target->view = m_dsvs.Allocate();
            m_device->CreateDepthStencilView(target->resource.Get(), &dsv, target->view);
        }
        else
        {
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = target->viewFormat;
            rtv.ViewDimension = target->hostSamples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
            target->view = m_rtvs.Allocate();
            m_device->CreateRenderTargetView(target->resource.Get(), &rtv, target->view);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = srvFormat;
        srv.ViewDimension = target->hostSamples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        target->srv = m_views->AllocatePersistent(1);
        m_device->CreateShaderResourceView(target->resource.Get(), &srv, m_views->Cpu(target->srv));
        if (depth)
        {
            srv.Format = srvFormat == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            if (target->hostSamples == 1)
                srv.Texture2D.PlaneSlice = 1;
            target->stencilSrv = m_views->AllocatePersistent(1);
            m_device->CreateShaderResourceView(target->resource.Get(), &srv, m_views->Cpu(target->stencilSrv));
        }

        LOG_INFO("GPU: render target at tile %u, pitch %u, %ux MSAA, format %u%s -> %ux%u%s", base, pitch, samples, format, depth ? " (depth)" : "",
            target->width * scale, target->height * scale, target->hostSamples != samples ? (" with " + std::to_string(target->hostSamples) + " samples").c_str() : "");
        slot = std::move(target);
        return slot.get();
    }

    void RenderTargetCache::Transition(ID3D12GraphicsCommandList* list, Target& target, D3D12_RESOURCE_STATES state)
    {
        if (target.state == state)
            return;
        const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(target.resource.Get(), target.state, state);
        list->ResourceBarrier(1, &barrier);
        target.state = state;
    }

    bool RenderTargetCache::Lookup(uint32_t colorMask, bool depth, Bindings& out)
    {
        const uint32_t surfaceInfo = Register(reg::RB_SURFACE_INFO);
        const uint32_t pitch = surfaceInfo & 0x3FFF;
        const uint32_t samples = 1u << std::min((surfaceInfo >> 16) & 3, 2u);

        out = Bindings{};
        out.samples = samples;
        out.width = UINT32_MAX;
        out.height = UINT32_MAX;
        for (uint32_t i = 0; i < 4; i++)
        {
            if (!(colorMask & (1u << i)))
                continue;
            const uint32_t info = Register(ColorInfoRegister(i));
            Target* target = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 0xF, false);
            if (!target)
                continue;
            out.color[i] = target;
            out.samples = target->hostSamples;
            out.width = std::min(out.width, target->width);
            out.height = std::min(out.height, target->height);
        }
        if (depth)
        {
            const uint32_t info = Register(reg::RB_DEPTH_INFO);
            Target* target = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 1, true);
            if (target)
            {
                out.depth = target;
                out.samples = target->hostSamples;
                out.width = std::min(out.width, target->width);
                out.height = std::min(out.height, target->height);
            }
        }
        return out.width != UINT32_MAX;
    }

    void RenderTargetCache::Bind(ID3D12GraphicsCommandList* list, UploadAllocator& upload, const Bindings& targets, uint32_t left, uint32_t top,
        uint32_t right, uint32_t bottom)
    {
        UseList(list);
        // A surface's height isn't in the registers. With color and depth bound, each is at most the space between their
        // bases (D3D places them back to back), which bounds draws in screen space.
        if (targets.depth)
        {
            for (Target* color : targets.color)
            {
                if (!color || color->base == targets.depth->base)
                    continue;
                const uint32_t gap = color->base < targets.depth->base ? targets.depth->base - color->base : color->base - targets.depth->base;
                for (const Target* target : { color, targets.depth })
                {
                    const TileLayout layout = LayoutOf(*target);
                    bottom = std::min(bottom, gap / layout.pitchTiles * layout.height);
                }
            }
        }
        for (Target* target : targets.color)
        {
            if (target)
                Acquire(list, upload, *target, left, top, right, bottom);
        }
        if (targets.depth)
            Acquire(list, upload, *targets.depth, left, top, right, bottom);
        if (m_passActive)
        {
            bool same = targets.depth == m_bound.depth;
            for (uint32_t i = 0; same && i < 4; i++)
                same = targets.color[i] == m_bound.color[i];
            if (same)
                return;
        }
        EndPass(list);

        D3D12_RENDER_PASS_RENDER_TARGET_DESC colors[4]{};
        uint32_t count = 0;
        for (uint32_t i = 0; i < 4; i++)
        {
            if (targets.color[i])
                count = i + 1;
        }
        for (uint32_t i = 0; i < count; i++)
        {
            if (Target* target = targets.color[i])
            {
                Transition(list, *target, D3D12_RESOURCE_STATE_RENDER_TARGET);
                colors[i].cpuDescriptor = target->view;
                colors[i].BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                colors[i].EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            }
            else
            {
                colors[i].cpuDescriptor = m_nullView;
                colors[i].BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
                colors[i].EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
            }
        }
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth{};
        if (targets.depth)
        {
            Transition(list, *targets.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            depth.cpuDescriptor = targets.depth->view;
            depth.DepthBeginningAccess.Type = depth.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            depth.DepthEndingAccess.Type = depth.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
        }
        m_passList4->BeginRenderPass(count, count ? colors : nullptr, targets.depth ? &depth : nullptr, D3D12_RENDER_PASS_FLAG_NONE);
        m_passActive = true;
        m_bound = targets;
    }

    void RenderTargetCache::Acquire(ID3D12GraphicsCommandList* list, UploadAllocator& upload, Target& target, uint32_t left, uint32_t top,
        uint32_t right, uint32_t bottom)
    {
        if (right <= left || bottom <= top)
            return;
        const TileLayout layout = LayoutOf(target);
        const uint32_t c0 = left / layout.width, c1 = std::min((right - 1) / layout.width, layout.pitchTiles - 1);
        const uint32_t r0 = top / layout.height, r1 = (bottom - 1) / layout.height;
        if (target.checkedEpoch == m_ownerEpoch && c0 >= target.checked[0] && c1 <= target.checked[1] && r0 >= target.checked[2] &&
            r1 <= target.checked[3])
            return;

        // The tiles other targets hold, by holder.
        std::vector<std::pair<Target*, std::array<uint32_t, 64>>> sources;
        bool changed = false;
        for (uint32_t row = r0; row <= r1; row++)
        {
            for (uint32_t column = c0; column <= c1; column++)
            {
                const uint32_t tile = target.base + row * layout.pitchTiles + column;
                if (tile >= kEdramTiles)
                    break;
                Target* owner = m_owners[tile];
                if (owner == &target)
                    continue;
                m_owners[tile] = &target;
                changed = true;
                if (!owner)
                    continue;
                auto it = std::find_if(sources.begin(), sources.end(), [owner](const auto& entry) { return entry.first == owner; });
                if (it == sources.end())
                    it = sources.insert(sources.end(), { owner, {} });
                it->second[tile >> 5] |= 1u << (tile & 31);
            }
        }
        // DEBUG_NO_EDRAM_TRANSFERS (diagnostics): 1, ownership is kept but contents aren't copied; 2, only between the
        // same formats (not as EDRAM words).
        static const int noTransfers = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_NO_EDRAM_TRANSFERS");
            return setting ? std::atoi(setting) : 0;
        }();
        for (const auto& [source, mask] : sources)
        {
            const uint64_t pair = (uint64_t(reinterpret_cast<uintptr_t>(source)) << 20) ^ reinterpret_cast<uintptr_t>(&target);
            TransferMode mode = noTransfers == 1 ? TransferMode::kNone : ModeOf(*source, target);
            if (noTransfers == 2 && mode == TransferMode::kRaw)
                mode = TransferMode::kNone;
            if (logTransfers)
            {
                uint32_t first = UINT32_MAX, last = 0, count = 0;
                for (uint32_t tile = 0; tile < kEdramTiles; tile++)
                {
                    if (mask[tile >> 5] & (1u << (tile & 31)))
                    {
                        first = std::min(first, tile);
                        last = tile;
                        count++;
                    }
                }
                LOG_INFO("[transfer] f%llu: %u tiles (%u-%u) from tile %u pitch %u %ux format %u%s to tile %u pitch %u %ux format %u%s, for %u,%u-%u,%u%s",
                    (unsigned long long)frameNumber, count, first, last, source->base, source->pitch, source->samples, source->format, source->depth ? " depth" : "", target.base,
                    target.pitch, target.samples, target.format, target.depth ? " depth" : "", left, top, right, bottom,
                    mode == TransferMode::kNone ? " (not copied)" : mode == TransferMode::kRaw ? " (as words)" : "");
            }
            if (std::find(m_reportedTransfers.begin(), m_reportedTransfers.end(), pair) == m_reportedTransfers.end())
            {
                m_reportedTransfers.push_back(pair);
                LOG_INFO("GPU: EDRAM at tile %u, pitch %u, %ux MSAA, format %u%s goes to pitch %u, %ux MSAA, format %u%s%s", source->base, source->pitch,
                    source->samples, source->format, source->depth ? " (depth)" : "", target.pitch, target.samples, target.format,
                    target.depth ? " (depth)" : "",
                    mode == TransferMode::kNone ? " (not copied)" : mode == TransferMode::kRaw ? " (as EDRAM words)" : "");
            }
            if (mode != TransferMode::kNone)
                Transfer(list, upload, *source, target, mask);
        }
        if (changed)
            m_ownerEpoch++;
        target.checkedEpoch = m_ownerEpoch;
        target.checked[0] = c0;
        target.checked[1] = c1;
        target.checked[2] = r0;
        target.checked[3] = r1;
    }

    // stencilBit 0-7: the pass setting that bit of a depth destination's stencil (see m_stencilTransfer).
    ID3D12PipelineState* RenderTargetCache::TransferPipeline(const Target& dest, bool sourceMultisampled, int stencilBit)
    {
        const bool bitPass = dest.depth && stencilBit >= 0;
        const bool stencil = dest.depth && m_stencilTransfer == StencilTransfer::kShader && !bitPass;
        const uint64_t key = uint64_t(dest.viewFormat) | (uint64_t(dest.hostSamples) << 16) | (uint64_t(dest.depth) << 24) | (uint64_t(sourceMultisampled) << 25) |
                             (uint64_t(bitPass ? stencilBit + 1 : 0) << 26);
        ComPtr<ID3D12PipelineState>& pipeline = m_transferPipelines[key];
        if (pipeline)
            return pipeline.Get();
        const uint32_t variant = (sourceMultisampled ? 1 : 0) | (dest.depth ? 2 : 0) | (stencil ? 4 : 0) | (bitPass ? 8 : 0);
        ComPtr<ID3DBlob>& shader = m_transferPixelShaders[variant];
        if (!shader)
        {
            const std::string source = std::string(sourceMultisampled ? "#define XE_SOURCE_MSAA 1\n" : "") + (dest.depth ? "#define XE_DEPTH 1\n" : "") +
                                       (stencil ? "#define XE_STENCIL 1\n" : "") + (bitPass ? "#define XE_STENCIL_BIT 1\n" : "") + kTransferPixelShader;
            shader = CompileShader(source.c_str(), "main", "ps_5_1", "EDRAM transfer");
            if (!shader)
                return nullptr;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = m_transferRootSignature.Get();
        description.VS = { m_transferVertexShader->GetBufferPointer(), m_transferVertexShader->GetBufferSize() };
        description.PS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        description.SampleMask = UINT_MAX;
        description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        description.RasterizerState.DepthClipEnable = TRUE;
        if (dest.depth)
        {
            // The stencil is replaced: by the shader's (SV_StencilRef), or by the reference (0, then 0xFF for each bit pass).
            description.DepthStencilState.DepthEnable = !bitPass;
            description.DepthStencilState.DepthWriteMask = bitPass ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
            description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            description.DepthStencilState.StencilEnable = bitPass || m_stencilTransfer != StencilTransfer::kNone;
            description.DepthStencilState.StencilReadMask = 0xFF;
            description.DepthStencilState.StencilWriteMask = bitPass ? uint8_t(1u << stencilBit) : 0xFF;
            const D3D12_DEPTH_STENCILOP_DESC replace{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS };
            description.DepthStencilState.FrontFace = description.DepthStencilState.BackFace = replace;
            description.DSVFormat = dest.viewFormat;
        }
        else
        {
            description.NumRenderTargets = 1;
            description.RTVFormats[0] = dest.viewFormat;
        }
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.SampleDesc.Count = dest.hostSamples;
        if (FAILED(m_device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline))))
        {
            LOG_ERROR("D3D12: can't create an EDRAM transfer pipeline (format %u, %u samples)", uint32_t(dest.viewFormat), dest.hostSamples);
            return nullptr;
        }
        return pipeline.Get();
    }

    void RenderTargetCache::Transfer(ID3D12GraphicsCommandList* list, UploadAllocator& upload, Target& source, Target& dest,
        const std::array<uint32_t, 64>& mask)
    {
        ID3D12PipelineState* pipeline = TransferPipeline(dest, source.hostSamples > 1);
        if (!pipeline)
            return;
        EndPass(list);
        transfers++;

        // The tiles' bounding box in the destination (host pixels).
        const TileLayout layout = LayoutOf(dest), sourceLayout = LayoutOf(source);
        uint32_t minColumn = UINT32_MAX, maxColumn = 0, minRow = UINT32_MAX, maxRow = 0;
        for (uint32_t tile = dest.base; tile < kEdramTiles; tile++)
        {
            if (!(mask[tile >> 5] & (1u << (tile & 31))))
                continue;
            const uint32_t row = (tile - dest.base) / layout.pitchTiles, column = (tile - dest.base) % layout.pitchTiles;
            minColumn = std::min(minColumn, column);
            maxColumn = std::max(maxColumn, column);
            minRow = std::min(minRow, row);
            maxRow = std::max(maxRow, row);
        }
        if (minRow == UINT32_MAX)
            return;
        const D3D12_RECT scissor{ LONG(minColumn * layout.width * scale), LONG(minRow * layout.height * scale),
            LONG(std::min((maxColumn + 1) * layout.width, dest.width) * scale), LONG(std::min((maxRow + 1) * layout.height, dest.height) * scale) };
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
            return;

        Transition(list, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_RENDER_PASS_RENDER_TARGET_DESC color{};
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth{};
        if (dest.depth)
        {
            Transition(list, dest, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            depth.cpuDescriptor = dest.view;
            depth.DepthBeginningAccess.Type = depth.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            depth.DepthEndingAccess.Type = depth.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            m_passList4->BeginRenderPass(0, nullptr, &depth, D3D12_RENDER_PASS_FLAG_NONE);
        }
        else
        {
            Transition(list, dest, D3D12_RESOURCE_STATE_RENDER_TARGET);
            color.cpuDescriptor = dest.view;
            color.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            color.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            m_passList4->BeginRenderPass(1, &color, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
        }

        TransferConstants constants{};
        constants.destBase = dest.base;
        constants.destPitchTiles = layout.pitchTiles;
        constants.destLog2Samples = uint32_t(std::countr_zero(dest.samples));
        constants.destSampleRatio = std::max(dest.hostSamples / dest.samples, 1u);
        constants.sourceBase = source.base;
        constants.sourcePitchTiles = sourceLayout.pitchTiles;
        constants.sourceLog2Samples = uint32_t(std::countr_zero(source.samples));
        constants.sourceSampleRatio = std::max(source.hostSamples / source.samples, 1u);
        constants.tileSamplesX = !dest.depth && xenos::Is64bpp(xenos::ColorFormat(dest.format)) ? 40 : 80;
        constants.scale = scale;
        constants.mode = ModeOf(source, dest) == TransferMode::kRaw ? 1 : 0;
        constants.sourceFormat = RawFormat(source);
        constants.destFormat = RawFormat(dest);
        const UploadAllocator::Allocation maskData = upload.Allocate(sizeof(mask), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        memcpy(maskData.cpu, mask.data(), sizeof(mask));
        list->SetGraphicsRootSignature(m_transferRootSignature.Get());
        list->SetPipelineState(pipeline);
        list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
        list->SetGraphicsRootConstantBufferView(1, maskData.gpu);
        list->SetGraphicsRootDescriptorTable(2, m_views->Gpu(source.srv));
        list->SetGraphicsRootDescriptorTable(3, m_views->Gpu(source.depth ? source.stencilSrv : source.srv));
        const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(dest.width * scale), float(dest.height * scale), 0.0f, 1.0f };
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (dest.depth)
            list->OMSetStencilRef(0);
        list->DrawInstanced(3, 1, 0, 0);
        // Without SV_StencilRef, the stencil the pass above zeroed gets the source's a bit at a time. (DOAX2's D3D clears
        // depth and stencil as a surface of half the pitch with 4x MSAA, so its stencil effects depend on this. The Marine
        // Race counts the sea's faces in the stencil and blends a teal color where the count is above the cleared value:
        // without the clear, counts piled up from frame to frame, and a teal wedge covered the race.)
        if (dest.depth && m_stencilTransfer == StencilTransfer::kBits)
        {
            list->OMSetStencilRef(0xFF);
            for (int bit = 0; bit < 8; bit++)
            {
                ID3D12PipelineState* bitPipeline = TransferPipeline(dest, source.hostSamples > 1, bit);
                if (!bitPipeline)
                    break;
                list->SetPipelineState(bitPipeline);
                list->SetGraphicsRoot32BitConstant(0, uint32_t(bit), offsetof(TransferConstants, stencilBit) / 4);
                list->DrawInstanced(3, 1, 0, 0);
            }
        }
        m_passList4->EndRenderPass();
    }

    void RenderTargetCache::UseList(ID3D12GraphicsCommandList* list)
    {
        if (m_passList == list)
            return;
        EndPass(list); // (a pass on the list before ended with it)
        m_passList = list;
        m_passList4.Reset();
        list->QueryInterface(IID_PPV_ARGS(&m_passList4));
    }

    void RenderTargetCache::EndPass(ID3D12GraphicsCommandList* list)
    {
        if (!m_passActive)
            return;
        if (m_passList == list)
            m_passList4->EndRenderPass();
        m_passActive = false;
    }

    bool RenderTargetCache::CaptureBoundColor(ID3D12GraphicsCommandList* list, uint32_t maxHeight, Capture& out)
    {
        Target* target = m_passActive ? m_bound.color[0] : nullptr;
        if (!target || (target->viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM && target->viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
            return false;
        EndPass(list);
        out.width = target->width * scale;
        out.height = std::min(target->height, maxHeight) * scale;
        out.base = target->base;
        out.pitch = target->pitch;
        out.samples = target->hostSamples;

        // A single-sampled copy of the whole target.
        D3D12_RESOURCE_DESC description = target->resource->GetDesc();
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Alignment = 0; // multisampled targets carry the 4 MB MSAA alignment
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        ComPtr<ID3D12Resource> copy;
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy))))
            return false;
        description.Height = out.height;
        UINT64 size = 0;
        m_device->GetCopyableFootprints(&description, 0, 1, 0, &out.footprint, nullptr, nullptr, &size);
        out.readback = CreateBuffer(m_device, size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!out.readback)
            return false;

        const D3D12_RESOURCE_STATES previous = target->state;
        if (target->hostSamples > 1)
        {
            Transition(list, *target, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            list->ResourceBarrier(1, &barrier);
            list->ResolveSubresource(copy.Get(), 0, target->resource.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
            barrier = TransitionBarrier(copy.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
        }
        else
        {
            Transition(list, *target, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(copy.Get(), target->resource.Get());
            const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
        }
        D3D12_TEXTURE_COPY_LOCATION destination{ out.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        destination.PlacedFootprint = out.footprint;
        D3D12_TEXTURE_COPY_LOCATION source{ copy.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        const D3D12_BOX box{ 0, 0, 0, out.width, out.height, 1 };
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        Transition(list, *target, previous);
        m_captureCopies.push_back(copy); // kept alive until the frame has executed (released with the cache)
        return true;
    }

    void RenderTargetCache::Resolve(ID3D12GraphicsCommandList* list, UploadAllocator& upload)
    {
        UseList(list);
        EndPass(list);
        const uint32_t copyControl = Register(reg::RB_COPY_CONTROL);
        const uint32_t sourceSelect = copyControl & 7;
        const bool colorClear = (copyControl >> 8) & 1;
        const bool depthClear = (copyControl >> 9) & 1;
        const uint32_t command = (copyControl >> 20) & 3;

        const uint32_t surfaceInfo = Register(reg::RB_SURFACE_INFO);
        const uint32_t pitch = surfaceInfo & 0x3FFF;
        const uint32_t samples = 1u << std::min((surfaceInfo >> 16) & 3, 2u);

        // The rectangle is the bounding box of the resolve draw's three vertices (fetch constant 0).
        const uint32_t fetch0 = Register(reg::SHADER_CONSTANT_FETCH_00_0), fetch1 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + 1);
        const uint32_t vertices = guest::PhysicalToGuest(fetch0 & 0xFFFFFFFC);
        float x[3], y[3];
        for (int i = 0; i < 3; i++)
        {
            const uint32_t rawX = SwapForEndian(*reinterpret_cast<const uint32_t*>(guest::ToHost(vertices + i * 8)), fetch1 & 3);
            const uint32_t rawY = SwapForEndian(*reinterpret_cast<const uint32_t*>(guest::ToHost(vertices + i * 8 + 4)), fetch1 & 3);
            memcpy(&x[i], &rawX, 4);
            memcpy(&y[i], &rawY, 4);
        }
        int32_t x0 = int32_t(std::lround(std::min({ x[0], x[1], x[2] }))), x1 = int32_t(std::lround(std::max({ x[0], x[1], x[2] })));
        int32_t y0 = int32_t(std::lround(std::min({ y[0], y[1], y[2] }))), y1 = int32_t(std::lround(std::max({ y[0], y[1], y[2] })));
        if (Register(reg::PA_SU_SC_MODE_CNTL) & (1u << 16))
        {
            const uint32_t offset = Register(reg::PA_SC_WINDOW_OFFSET);
            const int32_t offsetX = int32_t(offset << 17) >> 17, offsetY = int32_t(offset << 1) >> 17;
            x0 += offsetX;
            x1 += offsetX;
            y0 += offsetY;
            y1 += offsetY;
        }

        Target* source;
        if (sourceSelect < 4)
        {
            const uint32_t info = Register(ColorInfoRegister(sourceSelect));
            source = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 0xF, false);
        }
        else
        {
            const uint32_t info = Register(reg::RB_DEPTH_INFO);
            source = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 1, true);
        }
        if (!source)
            return;
        x0 = std::clamp<int32_t>(x0, 0, int32_t(source->width));
        x1 = std::clamp<int32_t>(x1, 0, int32_t(source->width));
        y0 = std::clamp<int32_t>(y0, 0, int32_t(source->height));
        y1 = std::clamp<int32_t>(y1, 0, int32_t(source->height));
        if (x1 <= x0 || y1 <= y0)
            return;
        resolves++;
        profile::Count(profile::Counter::kResolves);
        Acquire(list, upload, *source, uint32_t(x0), uint32_t(y0), uint32_t(x1), uint32_t(y1));

        // Copy.
        const uint32_t destBase = Register(reg::RB_COPY_DEST_BASE);
        const uint32_t destPitchInfo = Register(reg::RB_COPY_DEST_PITCH);
        const uint32_t destInfo = Register(reg::RB_COPY_DEST_INFO);
        const uint32_t destFormat = (destInfo >> 7) & 0x3F;
        // Depth goes to any 32-bit format (raw EDRAM words, sampled as k_24_8 / k_24_8_FLOAT or k_8_8_8_8).
        const bool depthSource = sourceSelect >= 4;
        const bool wideDest = destFormat == 26 || destFormat == 32 || destFormat == 37;
        const uint32_t smallBytes = destFormat == 2 ? 1 : destFormat == 4 ? 2 : 0; // k_8, k_5_6_5 (DOAX2)
        const bool supported = depthSource ? !wideDest && !smallBytes
                                           : (smallBytes || destFormat == 6 || destFormat == 7 || destFormat == 25 || destFormat == 26 ||
                                                 destFormat == 31 || destFormat == 32 || destFormat == 36 || destFormat == 37 || destFormat == 62);
        if (command != 3 && destBase && supported)
        {
            const uint32_t destPitch = std::max((destPitchInfo & 0x3FFF), 1u);
            const uint32_t alignedPitch = (destPitch + 31) & ~31u;
            const uint32_t originX = uint32_t(x0) & 31, originY = uint32_t(y0) & 31;
            const uint32_t width = uint32_t(x1 - x0), height = uint32_t(y1 - y0);
            const uint32_t bytesPerPixel = wideDest ? 8 : smallBytes ? smallBytes : 4;
            const uint32_t rows = (originY + height + 31) & ~31u;
            const uint32_t destSize = alignedPitch * rows * bytesPerPixel;
            // LOG_RESOLVES=<hex physical address> (diagnostics): logs the resolves whose destination covers it; =1, the
            // first 2000 resolves.
            static const uint32_t logAddress = [] {
                const char* setting = std::getenv(GAME_PREFIX "_LOG_RESOLVES");
                return setting ? uint32_t(strtoul(setting, nullptr, 16)) : 0u;
            }();
            static uint32_t logged = 0;
            if (logAddress == 1 ? logged < 2000 : logAddress - destBase < destSize)
            {
                logged++;
                LOG_INFO("GPU: resolve %ux%u at %d,%d of tile %u (pitch %u, %ux, format %u%s) to %08X (pitch %u, format %u)", width, height, x0, y0,
                    source->base, pitch, samples, source->format, depthSource ? ", depth" : "", destBase, destPitch, destFormat);
            }

            // Pages partly covered must hold current data before the GPU overwrites the rest.
            m_sharedMemory->RequestRange(upload, destBase, destSize);
            m_sharedMemory->Transition(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list, *source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            ResolveConstants constants{};
            constants.sourceX = uint32_t(x0);
            constants.sourceY = uint32_t(y0);
            constants.width = width;
            constants.height = height;
            constants.destX = originX;
            constants.destY = originY;
            constants.destBase = destBase;
            constants.destPitch = alignedPitch;
            constants.destFormat = destFormat;
            constants.destEndian = destInfo & 7;
            constants.swapRedBlue = (destInfo >> 24) & 1;
            const int32_t expBias = int32_t(destInfo << 10) >> 26;
            const xenos::ColorFormat sourceFormat = xenos::ColorFormat(source->format);
            // 16-bit fixed render targets hold -32...32 as snorm.
            const float rangeScale = (sourceFormat == xenos::ColorFormat::k_16_16 || sourceFormat == xenos::ColorFormat::k_16_16_16_16) ? 32.0f : 1.0f;
            constants.scale = std::ldexp(rangeScale, expBias);
            constants.sampleSelect = (copyControl >> 4) & 7;
            constants.samples = source->hostSamples;
            if (source->hostSamples > samples && constants.sampleSelect >= 4)
                constants.sampleSelect = 6; // (the guest's 2 samples are all of a pixel)
            if (depthSource)
            {
                constants.sourceDepth = xenos::DepthFormat(source->format) == xenos::DepthFormat::kD24FS8 ? 2 : 1;
                constants.sampleSelect = std::min(constants.sampleSelect, 3u); // samples aren't averaged
            }
            constants.resolutionScale = scale;

            // The 32x32 macro tiles the resolve writes, as one byte range per row of them.
            const uint32_t log2Bpp = wideDest ? 3 : smallBytes == 1 ? 0 : smallBytes == 2 ? 1 : 2;
            const uint32_t tileBytes = 1u << (log2Bpp + 10);
            const uint32_t firstColumn = originX >> 5, columns = ((originX + width - 1) >> 5) - firstColumn + 1;
            std::vector<std::pair<uint32_t, uint32_t>> written;
            for (uint32_t row = originY >> 5; row <= (originY + height - 1) >> 5; row++)
                written.emplace_back(destBase + (row * (alignedPitch >> 5) + firstColumn) * tileBytes, columns * tileBytes);

            list->SetComputeRootSignature(m_resolveRootSignature.Get());
            list->SetPipelineState(m_resolvePipelines[source->hostSamples > 1 ? 1 : 0].Get());
            list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            list->SetComputeRootDescriptorTable(1, m_views->Gpu(source->srv));
            list->SetComputeRootUnorderedAccessView(2, m_sharedMemory->GpuAddress());
            // (Unused without the scaled pass; root descriptors just need an address.)
            list->SetComputeRootShaderResourceView(3, m_sharedMemory->GpuAddress());
            list->SetComputeRootUnorderedAccessView(4, m_sharedMemory->GpuAddress());
            list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            m_sharedMemory->UavBarrier(list);
            for (const auto& [start, length] : written)
                watch::NoteGpuWrite(start, length);

            // Resolution scaling: the scaled copy, a thread per host pixel (see ScaledResolves), of 4- and 8-byte texels.
            bool scaledStorage = scale > 1 && !smallBytes;
            for (const auto& [start, length] : written)
                scaledStorage = scaledStorage && m_scaledResolves->Allocate(list, upload, start, length);
            if (scaledStorage)
            {
                m_scaledResolves->TransitionPageTable(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                m_scaledResolves->TransitionPool(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                constants.scaledPass = 1;
                list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
                list->SetComputeRootShaderResourceView(3, m_scaledResolves->PageTableAddress());
                list->SetComputeRootUnorderedAccessView(4, m_scaledResolves->PoolAddress());
                list->Dispatch((width * scale + 7) / 8, (height * scale + 7) / 8, 1);
                m_scaledResolves->PoolUavBarrier(list);
                for (const auto& [start, length] : written)
                    m_scaledResolves->NoteWritten(start, length, log2Bpp);
            }
        }
        else if (command != 3 && destBase)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                LOG_WARN("GPU: unsupported resolve (source %u, %ux MSAA, dest format %u)", sourceSelect, samples, destFormat);
            }
        }

        // Clears of the resolved area (in host pixels).
        const int32_t hostScale = int32_t(scale);
        const D3D12_RECT rect{ x0 * hostScale, y0 * hostScale, x1 * hostScale, y1 * hostScale };
        if (colorClear)
        {
            const uint32_t index = sourceSelect < 4 ? sourceSelect : 0;
            const uint32_t info = Register(ColorInfoRegister(index));
            if (Target* color = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 0xF, false))
            {
                const uint32_t value = Register(reg::RB_COLOR_CLEAR), low = Register(reg::RB_COLOR_CLEAR_LO);
                float rgba[4];
                switch (xenos::ColorFormat(color->format))
                {
                case xenos::ColorFormat::k_8_8_8_8:
                case xenos::ColorFormat::k_8_8_8_8_GAMMA:
                    for (int i = 0; i < 4; i++)
                        rgba[i] = float((value >> (8 * i)) & 0xFF) / 255.0f;
                    break;
                case xenos::ColorFormat::k_32_FLOAT:
                    memcpy(&rgba[0], &value, 4);
                    rgba[1] = rgba[2] = rgba[3] = 0.0f;
                    break;
                default:
                    // 64bpp formats split the value over two registers; approximate with the high one.
                    for (int i = 0; i < 4; i++)
                        rgba[i] = float(((i < 2 ? low : value) >> (16 * (i & 1))) & 0xFFFF) / 65535.0f;
                    break;
                }
                Acquire(list, upload, *color, uint32_t(x0), uint32_t(y0), uint32_t(x1), uint32_t(y1));
                Transition(list, *color, D3D12_RESOURCE_STATE_RENDER_TARGET);
                list->ClearRenderTargetView(color->view, rgba, 1, &rect);
            }
        }
        if (depthClear)
        {
            const uint32_t info = Register(reg::RB_DEPTH_INFO);
            if (Target* depth = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 1, true))
            {
                const uint32_t value = Register(reg::RB_DEPTH_CLEAR);
                const float depthValue = xenos::DepthFormat(depth->format) == xenos::DepthFormat::kD24FS8 ? Float20e4ToFloat(value >> 8)
                                                                                                        : float(value >> 8) / float(0xFFFFFF);
                Acquire(list, upload, *depth, uint32_t(x0), uint32_t(y0), uint32_t(x1), uint32_t(y1));
                Transition(list, *depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                list->ClearDepthStencilView(depth->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depthValue, uint8_t(value & 0xFF), 1, &rect);
            }
        }
    }
}
