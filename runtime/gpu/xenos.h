#pragma once

// Xenos data formats and fetch constants (layouts as documented by Xenia).
#include <cstdint>

namespace gpu::xenos
{
    enum class TextureFormat : uint32_t
    {
        k_1_REVERSE = 0,
        k_1 = 1,
        k_8 = 2,
        k_1_5_5_5 = 3,
        k_5_6_5 = 4,
        k_6_5_5 = 5,
        k_8_8_8_8 = 6,
        k_2_10_10_10 = 7,
        k_8_A = 8,
        k_8_B = 9,
        k_8_8 = 10,
        k_Cr_Y1_Cb_Y0_REP = 11,
        k_Y1_Cr_Y0_Cb_REP = 12,
        k_16_16_EDRAM = 13,
        k_8_8_8_8_A = 14,
        k_4_4_4_4 = 15,
        k_10_11_11 = 16,
        k_11_11_10 = 17,
        k_DXT1 = 18,
        k_DXT2_3 = 19,
        k_DXT4_5 = 20,
        k_16_16_16_16_EDRAM = 21,
        k_24_8 = 22,
        k_24_8_FLOAT = 23,
        k_16 = 24,
        k_16_16 = 25,
        k_16_16_16_16 = 26,
        k_16_EXPAND = 27,
        k_16_16_EXPAND = 28,
        k_16_16_16_16_EXPAND = 29,
        k_16_FLOAT = 30,
        k_16_16_FLOAT = 31,
        k_16_16_16_16_FLOAT = 32,
        k_32 = 33,
        k_32_32 = 34,
        k_32_32_32_32 = 35,
        k_32_FLOAT = 36,
        k_32_32_FLOAT = 37,
        k_32_32_32_32_FLOAT = 38,
        k_32_AS_8 = 39,
        k_32_AS_8_8 = 40,
        k_16_MPEG = 41,
        k_16_16_MPEG = 42,
        k_8_INTERLACED = 43,
        k_32_AS_8_INTERLACED = 44,
        k_32_AS_8_8_INTERLACED = 45,
        k_16_INTERLACED = 46,
        k_16_MPEG_INTERLACED = 47,
        k_16_16_MPEG_INTERLACED = 48,
        k_DXN = 49,
        k_8_8_8_8_AS_16_16_16_16 = 50,
        k_DXT1_AS_16_16_16_16 = 51,
        k_DXT2_3_AS_16_16_16_16 = 52,
        k_DXT4_5_AS_16_16_16_16 = 53,
        k_2_10_10_10_AS_16_16_16_16 = 54,
        k_10_11_11_AS_16_16_16_16 = 55,
        k_11_11_10_AS_16_16_16_16 = 56,
        k_32_32_32_FLOAT = 57,
        k_DXT3A = 58,
        k_DXT5A = 59,
        k_CTX1 = 60,
        k_DXT3A_AS_1_1_1_1 = 61,
        k_8_8_8_8_GAMMA_EDRAM = 62,
        k_2_10_10_10_FLOAT_EDRAM = 63,
    };

    // EDRAM render target formats (RB_COLOR_INFO.color_format / RB_DEPTH_INFO.depth_format).
    enum class ColorFormat : uint32_t
    {
        k_8_8_8_8 = 0,
        k_8_8_8_8_GAMMA = 1,
        k_2_10_10_10 = 2,
        k_2_10_10_10_FLOAT = 3,
        k_16_16 = 4,
        k_16_16_16_16 = 5,
        k_16_16_FLOAT = 6,
        k_16_16_16_16_FLOAT = 7,
        k_2_10_10_10_AS_10_10_10_10 = 10,
        k_2_10_10_10_FLOAT_AS_16_16_16_16 = 12,
        k_32_FLOAT = 14,
        k_32_32_FLOAT = 15,
    };
    inline bool Is64bpp(ColorFormat format)
    {
        return format == ColorFormat::k_16_16_16_16 || format == ColorFormat::k_16_16_16_16_FLOAT || format == ColorFormat::k_32_32_FLOAT;
    }

    enum class DepthFormat : uint32_t
    {
        kD24S8 = 0,
        kD24FS8 = 1,
    };

    // A texture fetch constant (6 dwords, host endian).
    struct TextureFetch
    {
        uint32_t d[6];

        uint32_t Type() const { return d[0] & 3; } // 2 = texture
        // How component X, Y, Z or W (0-3) reads: 0 unsigned, 1 signed (two's complement, -1...1), 2 unsigned biased
        // to -1...1 (2x - 1), 3 gamma (linearized).
        uint32_t Sign(uint32_t component) const { return (d[0] >> (2 + component * 2)) & 3; }
        uint32_t ClampX() const { return (d[0] >> 10) & 7; }
        uint32_t ClampY() const { return (d[0] >> 13) & 7; }
        uint32_t ClampZ() const { return (d[0] >> 16) & 7; }
        uint32_t Pitch() const { return ((d[0] >> 22) & 0x1FF) << 5; } // texels
        bool Tiled() const { return (d[0] >> 31) & 1; }

        TextureFormat Format() const { return TextureFormat(d[1] & 0x3F); }
        uint32_t Endian() const { return (d[1] >> 6) & 3; }
        bool Stacked() const { return (d[1] >> 10) & 1; }
        uint32_t BaseAddress() const { return d[1] & 0xFFFFF000; } // physical

        uint32_t Dimension() const { return (d[5] >> 9) & 3; } // 0 1D, 1 2D, 2 3D/stacked, 3 cube
        uint32_t Width() const
        {
            return Dimension() == 0 ? (d[2] & 0xFFFFFF) + 1 : Dimension() == 2 && !Stacked() ? (d[2] & 0x7FF) + 1 : (d[2] & 0x1FFF) + 1;
        }
        uint32_t Height() const
        {
            return Dimension() == 0 ? 1 : Dimension() == 2 && !Stacked() ? ((d[2] >> 11) & 0x7FF) + 1 : ((d[2] >> 13) & 0x1FFF) + 1;
        }
        uint32_t Depth() const
        {
            return Dimension() == 2 ? (Stacked() ? ((d[2] >> 26) & 0x3F) + 1 : ((d[2] >> 22) & 0x3FF) + 1) : 1;
        }

        bool NumFormatInteger() const { return d[3] & 1; }
        uint32_t Swizzle() const { return (d[3] >> 1) & 0xFFF; } // 3 bits per component
        int32_t ExpAdjust() const { return int32_t(d[3] << 13) >> 26; }
        uint32_t MagFilter() const { return (d[3] >> 19) & 3; }
        uint32_t MinFilter() const { return (d[3] >> 21) & 3; }
        uint32_t MipFilter() const { return (d[3] >> 23) & 3; }
        uint32_t AnisoFilter() const { return (d[3] >> 25) & 7; }

        uint32_t MipMinLevel() const { return (d[4] >> 2) & 0xF; }
        uint32_t MipMaxLevel() const { return (d[4] >> 6) & 0xF; }
        float LodBias() const { return float(int32_t(d[4] << 10) >> 22) / 32.0f; }

        uint32_t BorderColor() const { return d[5] & 3; }
        bool PackedMips() const { return (d[5] >> 11) & 1; }
        uint32_t MipAddress() const { return d[5] & 0xFFFFF000; }
    };

    struct FormatInfo
    {
        uint32_t bytesPerBlock = 0; // 0 = unsupported
        uint32_t blockWidth = 1, blockHeight = 1;
        // Host DXGI format (as a uint32_t to keep this header API-neutral).
        uint32_t dxgiFormat = 0;
        // For formats a fetch can read as signed (see TextureFetch::Sign): the typeless format the texture is created
        // with, and the SNORM format of its signed views. 0 if there's none.
        uint32_t typelessFormat = 0, snormFormat = 0;
        // Host channel (0 R, 1 G, 2 B, 3 A) holding each Xenos component X, Y, Z, W; 4 = constant 0,
        // 5 = constant 1 (component absent).
        uint8_t channels[4] = { 0, 1, 2, 3 };
        // Conversion while loading 32-bit texels (see FormatConversion).
        uint8_t conversion = 0;
    };
    // Depth texels (24-bit depth above 8-bit stencil, as resolved from EDRAM) load as float depth.
    enum FormatConversion : uint8_t
    {
        kConvertNone = 0,
        kConvertDepthUnorm24 = 1, // k_24_8
        kConvertDepthFloat20e4 = 2, // k_24_8_FLOAT
    };
    const FormatInfo& GetFormatInfo(TextureFormat format);

    // Where the levels and array slices (cube faces) of a 2D or cube texture are in guest memory.
    //
    // The base level is at the base address, the other levels at the mip address. Mips have power of
    // two sizes, levels are stored in whole 32x32-block tiles when tiled (rows padded to 256 bytes
    // when linear), and each array slice of a level is 4 KB aligned. With packed mips, levels whose
    // shorter side is 16 texels or less share one tail (the base level has its own if it's that
    // small), at offsets that depend only on the level.
    struct TextureLevel
    {
        uint32_t address = 0;       // physical address of array slice 0's storage
        uint32_t sliceBytes = 0;    // between array slices
        uint32_t widthBlocks = 0, heightBlocks = 0; // of the level as stored on the host (D3D sizes)
        uint32_t pitchBlocks = 0;   // row pitch of the storage (tiled: a multiple of 32)
        uint32_t rowBytes = 0;      // row pitch of the storage in bytes
        uint32_t offsetX = 0, offsetY = 0; // of the level in its storage, in blocks (packed mip tails)
    };
    struct TextureLayout
    {
        static constexpr uint32_t kMaxLevels = 14;
        uint32_t levels = 0, slices = 0;
        TextureLevel level[kMaxLevels];
        // Guest memory holding the base level, and the mips (0 bytes if none).
        uint32_t baseAddress = 0, baseBytes = 0, mipAddress = 0, mipBytes = 0;
    };
    struct FormatInfo;
    // `width` and `height` in texels, `pitchTexels` the base level's row pitch.
    TextureLayout GetTextureLayout(const FormatInfo& format, uint32_t baseAddress, uint32_t mipAddress, uint32_t width, uint32_t height,
        uint32_t pitchTexels, bool tiled, bool packedMips, uint32_t levels, uint32_t slices);

    // Byte offset of block (x, y) in a 2D tiled surface whose pitch is `pitchBlocks` (a multiple of
    // 32) and whose blocks are `bytesPerBlock` bytes (XGAddress2DTiledOffset).
    inline uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t bytesPerBlock)
    {
        const uint32_t logBpp = (bytesPerBlock >> 2) + ((bytesPerBlock >> 1) >> (bytesPerBlock >> 2));
        const uint32_t macro = ((x >> 5) + (y >> 5) * (pitchBlocks >> 5)) << (logBpp + 7);
        const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
        const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
        return ((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
    }
}
