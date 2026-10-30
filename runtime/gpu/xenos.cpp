#include "gpu/xenos.h"

#include <algorithm>
#include <bit>

#include <dxgiformat.h>

namespace gpu::xenos
{
    namespace
    {
        constexpr uint8_t k0 = 4, k1 = 5;
        constexpr uint8_t R = 0, G = 1, B = 2, A = 3;

        FormatInfo Info(uint32_t bytesPerBlock, uint32_t blockSize, DXGI_FORMAT format, uint8_t x, uint8_t y, uint8_t z, uint8_t w)
        {
            FormatInfo info;
            info.bytesPerBlock = bytesPerBlock;
            info.blockWidth = info.blockHeight = blockSize;
            info.dxgiFormat = format;
            info.channels[0] = x;
            info.channels[1] = y;
            info.channels[2] = z;
            info.channels[3] = w;
            return info;
        }

        // A format fetches can also read as signed: created typeless, viewed as `snorm` when they do.
        FormatInfo Signable(FormatInfo info, DXGI_FORMAT typeless, DXGI_FORMAT snorm)
        {
            info.typelessFormat = typeless;
            info.snormFormat = snorm;
            return info;
        }

        struct FormatTable
        {
            FormatInfo entries[64];

            FormatTable()
            {
                auto set = [&](TextureFormat format, FormatInfo info) { entries[uint32_t(format)] = info; };
                const FormatInfo r8 = Signable(Info(1, 1, DXGI_FORMAT_R8_UNORM, R, k0, k0, k1), DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SNORM);
                set(TextureFormat::k_8, r8);
                set(TextureFormat::k_8_A, r8);
                set(TextureFormat::k_8_B, r8);
                set(TextureFormat::k_1_5_5_5, Info(2, 1, DXGI_FORMAT_B5G5R5A1_UNORM, B, G, R, A));
                set(TextureFormat::k_5_6_5, Info(2, 1, DXGI_FORMAT_B5G6R5_UNORM, B, G, R, k1));
                set(TextureFormat::k_4_4_4_4, Info(2, 1, DXGI_FORMAT_B4G4R4A4_UNORM, B, G, R, A));
                set(TextureFormat::k_8_8, Signable(Info(2, 1, DXGI_FORMAT_R8G8_UNORM, R, G, k0, k1), DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SNORM));
                const FormatInfo rgba8 =
                    Signable(Info(4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, R, G, B, A), DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SNORM);
                set(TextureFormat::k_8_8_8_8, rgba8);
                set(TextureFormat::k_8_8_8_8_A, rgba8);
                set(TextureFormat::k_8_8_8_8_AS_16_16_16_16, rgba8);
                set(TextureFormat::k_8_8_8_8_GAMMA_EDRAM, rgba8);
                set(TextureFormat::k_2_10_10_10, Info(4, 1, DXGI_FORMAT_R10G10B10A2_UNORM, R, G, B, A));
                set(TextureFormat::k_2_10_10_10_AS_16_16_16_16, Info(4, 1, DXGI_FORMAT_R10G10B10A2_UNORM, R, G, B, A));
                set(TextureFormat::k_DXT1, Info(8, 4, DXGI_FORMAT_BC1_UNORM, R, G, B, A));
                set(TextureFormat::k_DXT1_AS_16_16_16_16, Info(8, 4, DXGI_FORMAT_BC1_UNORM, R, G, B, A));
                set(TextureFormat::k_DXT2_3, Info(16, 4, DXGI_FORMAT_BC2_UNORM, R, G, B, A));
                set(TextureFormat::k_DXT2_3_AS_16_16_16_16, Info(16, 4, DXGI_FORMAT_BC2_UNORM, R, G, B, A));
                set(TextureFormat::k_DXT4_5, Info(16, 4, DXGI_FORMAT_BC3_UNORM, R, G, B, A));
                set(TextureFormat::k_DXT4_5_AS_16_16_16_16, Info(16, 4, DXGI_FORMAT_BC3_UNORM, R, G, B, A));
                set(TextureFormat::k_DXN, Signable(Info(16, 4, DXGI_FORMAT_BC5_UNORM, R, G, k0, k1), DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_SNORM));
                set(TextureFormat::k_DXT5A, Signable(Info(8, 4, DXGI_FORMAT_BC4_UNORM, R, R, R, R), DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_SNORM));
                set(TextureFormat::k_16, Signable(Info(2, 1, DXGI_FORMAT_R16_UNORM, R, k0, k0, k1), DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SNORM));
                set(TextureFormat::k_16_16,
                    Signable(Info(4, 1, DXGI_FORMAT_R16G16_UNORM, R, G, k0, k1), DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SNORM));
                set(TextureFormat::k_16_16_16_16, Signable(Info(8, 1, DXGI_FORMAT_R16G16B16A16_UNORM, R, G, B, A), DXGI_FORMAT_R16G16B16A16_TYPELESS,
                                                      DXGI_FORMAT_R16G16B16A16_SNORM));
                set(TextureFormat::k_16_FLOAT, Info(2, 1, DXGI_FORMAT_R16_FLOAT, R, k0, k0, k1));
                set(TextureFormat::k_16_16_FLOAT, Info(4, 1, DXGI_FORMAT_R16G16_FLOAT, R, G, k0, k1));
                set(TextureFormat::k_16_16_16_16_FLOAT, Info(8, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, R, G, B, A));
                set(TextureFormat::k_32_FLOAT, Info(4, 1, DXGI_FORMAT_R32_FLOAT, R, k0, k0, k1));
                // The depth formats read as the depth in every channel, as Xenia samples them. DOA4's shaders decode it from X
                // and Y (their fetch constants' swizzle is X, Y, 1, 1) as if they held its top and middle bytes, with weights
                // that give the depth back from it in both too: Alpha-152's in her chamber (ps 5CCD9543, C889BABA) as
                // (255 X + Y + 1/255) / 256, the depth of field behind the menus (ps 3B7D89D7) as (2^24 X + 2^16 Y + 2^8) /
                // (2^24 + 2^16 + 2^8). With the depth in X alone, both came out 0.4% too near, which hid her from afar.
                FormatInfo depth = Info(4, 1, DXGI_FORMAT_R32_FLOAT, R, R, R, R);
                depth.conversion = kConvertDepthUnorm24;
                set(TextureFormat::k_24_8, depth);
                depth.conversion = kConvertDepthFloat20e4;
                set(TextureFormat::k_24_8_FLOAT, depth);
                set(TextureFormat::k_32_32_FLOAT, Info(8, 1, DXGI_FORMAT_R32G32_FLOAT, R, G, k0, k1));
                set(TextureFormat::k_32_32_32_32_FLOAT, Info(16, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, R, G, B, A));
            }
        };
    }

    const FormatInfo& GetFormatInfo(TextureFormat format)
    {
        static const FormatTable table;
        return table.entries[uint32_t(format) & 63];
    }

    namespace
    {
        uint32_t Log2Ceil(uint32_t value)
        {
            return value <= 1 ? 0 : 32 - std::countl_zero(value - 1);
        }

        uint32_t AlignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        // Offset in texels of a level in its packed mip tail (as Xenia's GetPackedMipOffset): the first
        // packed levels go down one side of the tile, the smaller ones along the other.
        void PackedMipOffset(uint32_t log2Width, uint32_t log2Height, uint32_t packedLevel, uint32_t level, uint32_t& x, uint32_t& y)
        {
            const uint32_t packed = level - packedLevel;
            const bool wide = log2Width > log2Height;
            x = y = 0;
            if (packed < 3)
            {
                (wide ? y : x) = 16u >> packed;
            }
            else
            {
                const uint32_t log2Size = wide ? log2Width : log2Height;
                (wide ? x : y) = (1u << (std::max(log2Size, packedLevel) - packedLevel)) >> (packed - 2);
            }
        }
    }

    TextureLayout GetTextureLayout(const FormatInfo& format, uint32_t baseAddress, uint32_t mipAddress, uint32_t width, uint32_t height,
        uint32_t pitchTexels, bool tiled, bool packedMips, uint32_t levels, uint32_t slices)
    {
        TextureLayout layout;
        layout.levels = std::clamp(levels, 1u, TextureLayout::kMaxLevels);
        layout.slices = slices;
        layout.baseAddress = baseAddress;
        layout.mipAddress = mipAddress;
        const uint32_t blockWidth = format.blockWidth, blockHeight = format.blockHeight, bytesPerBlock = format.bytesPerBlock;
        const uint32_t log2Width = Log2Ceil(width), log2Height = Log2Ceil(height);
        const uint32_t widthPow2 = 1u << log2Width, heightPow2 = 1u << log2Height;
        const uint32_t log2Size = std::min(log2Width, log2Height);
        const uint32_t packedLevel = packedMips ? (log2Size > 4 ? log2Size - 4 : 0) : UINT32_MAX;
        // Host level sizes, as D3D12 derives them from the block-aligned base.
        const uint32_t hostWidth = AlignUp(width, blockWidth), hostHeight = AlignUp(height, blockHeight);

        uint32_t mipOffset = 0, tailAddress = 0;
        for (uint32_t level = 0; level < layout.levels; level++)
        {
            TextureLevel& out = layout.level[level];
            out.widthBlocks = (std::max(hostWidth >> level, 1u) + blockWidth - 1) / blockWidth;
            out.heightBlocks = (std::max(hostHeight >> level, 1u) + blockHeight - 1) / blockHeight;

            // Storage size in texels: the level's own, or for a packed tail, the first packed level's.
            const bool inTail = level >= packedLevel;
            uint32_t storageWidth, storageHeight;
            if (level == 0)
            {
                storageWidth = std::max(width, pitchTexels);
                storageHeight = height;
            }
            else
            {
                const uint32_t storageLevel = inTail ? packedLevel : level;
                storageWidth = std::max(widthPow2 >> storageLevel, 1u);
                storageHeight = std::max(heightPow2 >> storageLevel, 1u);
            }
            const uint32_t storageWidthBlocks = (storageWidth + blockWidth - 1) / blockWidth;
            const uint32_t storageRows = (storageHeight + blockHeight - 1) / blockHeight;
            out.pitchBlocks = tiled ? AlignUp(storageWidthBlocks, 32) : storageWidthBlocks;
            out.rowBytes = tiled ? out.pitchBlocks * bytesPerBlock : AlignUp(storageWidthBlocks * bytesPerBlock, 256);
            out.sliceBytes = AlignUp(out.rowBytes * (tiled ? AlignUp(storageRows, 32) : storageRows), 4096);
            if (inTail)
            {
                uint32_t x, y;
                PackedMipOffset(log2Width, log2Height, packedLevel, level, x, y);
                out.offsetX = x / blockWidth;
                out.offsetY = y / blockHeight;
            }

            if (level == 0)
            {
                out.address = baseAddress;
                layout.baseBytes = out.sliceBytes * slices;
            }
            else if (!inTail || !tailAddress)
            {
                out.address = mipAddress + mipOffset;
                mipOffset += out.sliceBytes * slices;
                if (inTail)
                    tailAddress = out.address;
            }
            else
            {
                out.address = tailAddress;
            }
        }
        layout.mipBytes = mipOffset;
        return layout;
    }
}
