#pragma once

// Translates parsed Xenos shaders to HLSL (shader model 5.1), compiled with d3dcompiler for D3D12
// (and compilable to SPIR-V with DXC, should a Vulkan backend need it).
//
// Interface shared by all translated shaders (see kHlslCommon in hlsl.cpp):
//   b0          system constants (viewport transform, vertex index handling, alpha test, ...)
//   b1          the stage's 256 float constants
//   b2          bool and loop constants, then the 32 fetch constants (6 dwords each)
//   b3          the stage's binding indices: texture binding i is descriptor xe_texture_indices[i]
//               of the persistent heap, sampler binding j sampler xe_sampler_indices[j]
//   t0, space1  guest physical memory as a ByteAddressBuffer (vertex fetch)
//   space2-4    bindless 2D, 3D and cube textures (unbounded tables over the descriptor heap)
//   s0          bindless samplers
//   u0          with DEBUG_NAN only, its counts (see DebugNan below)
// Vertex shaders output SV_Position, 16 interpolators (TEXCOORD0-15) and the point size; pixel
// shaders receive the interpolators in r0-r15.
#include "gpu/shader/shader.h"

#include <string>
#include <vector>

namespace gpu::shader
{
    // System constant flags (SystemConstants::flags).
    inline constexpr uint32_t kSysFlagXYDividedByW = 1u << 0;  // PA_CL_VTE_CNTL.vtx_xy_fmt
    inline constexpr uint32_t kSysFlagZDividedByW = 1u << 1;   // vtx_z_fmt
    inline constexpr uint32_t kSysFlagWNotReciprocal = 1u << 2; // vtx_w0_fmt
    inline constexpr uint32_t kSysFlagIndexed = 1u << 3;       // SV_VertexID is a guest index needing an endian swap

    // Layout of the b0 constant buffer (16-byte rows, matching the HLSL declaration).
    struct SystemConstants
    {
        float ndcScale[3];
        uint32_t flags;
        float ndcOffset[3];
        uint32_t vertexIndexEndian;
        int32_t vertexBaseIndex;
        uint32_t vertexIndexMin;
        uint32_t vertexIndexMax;
        float pointSize;           // diameter in pixels when the shader doesn't export one
        float pointSizeMinMax[2];  // diameter limits in pixels
        float pointScreenToNdc[2]; // 2 / render target size
        float alphaRef;
        uint32_t alphaFunc;        // compare function; 7 = always (alpha test off)
        uint32_t paramGenRegister; // pixel shader: register receiving the position (0xFFFFFFFF = none)
        float resolutionScale;     // host pixels per guest pixel in each direction
        float colorExpBias[4];     // per render target, multiplies the output color
        // Per render target, for the min and max blend ops: Xenos multiplies the source by its blend factor
        // there too, which Direct3D 12 doesn't, so the shader does. Bits 0-4: the color's factor, 5: color op
        // is min or max; 8-12: the alpha's factor, 13: alpha op is min or max.
        uint32_t minMaxFactors[4];
        // Pixel shaders translated with alphaCoverage: the target's samples, over which alpha test edges spread (0: off).
        uint32_t alphaCoverageSamples;
        uint32_t padding[3];
    };
    static_assert(sizeof(SystemConstants) % 16 == 0);

    struct HlslResult
    {
        std::string source;
        std::string entryPoint = "main";
        // What the translation couldn't express, each once; it emits a placeholder instead (zeros, or nothing).
        std::vector<std::string> unsupported;
    };

    // Layout of the b3 constant buffer.
    struct BindingConstants
    {
        uint32_t textures[32];
        uint32_t samplers[32];
    };

    // Products (mul, mad, dot products, scalar multiplies) as the Xenos ALU computes them: 0 when either factor is 0, even
    // times infinity or NaN (Direct3D 9's rule), where IEEE arithmetic gives NaN. Some of DOA4's characters have vertices
    // whose tangent is 0, which their shaders normalize (0 times the reciprocal square root of 0). XENOS_PRODUCTS=0 makes
    // translated shaders use IEEE products instead.
    bool XenosProducts();
    // DEBUG_NAN=1: translated shaders count, per shader, invocations with a product of 0 and infinity or NaN (where the
    // two kinds of product differ), with non-finite outputs (positions, interpolators, colors or depth) and with
    // non-finite inputs (vertex data, interpolators or texels), in the buffer at u0 (a root UAV the renderer adds and
    // reads back). Pixel shaders also output magenta for non-finite colors; DEBUG_NAN=2 also paints pixels with
    // non-finite interpolators green.
    uint32_t DebugNan();
    // The counts buffer: kDebugCountSlots slots of 16 dwords, the slot chosen by the shader's hash: hash (low, high),
    // flags (bit 0: a pixel shader, and the kDebugNonFinite bits seen), invocations with such a product, with non-finite
    // outputs, 0xFFFF minus the lowest address of an ALU instruction with such a product, the highest, invocations with
    // non-finite inputs, and the interpolator components seen non-finite (a vertex shader's outputs, a pixel shader's
    // inputs: 4 bits an interpolator, x lowest, in two dwords).
    inline constexpr uint32_t kDebugCountSlots = 4096;
    inline constexpr uint32_t kDebugSlotDwords = 16;
    inline constexpr uint32_t kDebugCountBytes = kDebugCountSlots * kDebugSlotDwords * 4;
    inline constexpr uint32_t kDebugNonFiniteVertexData = 1u << 1;
    inline constexpr uint32_t kDebugNonFiniteInterpolators = 1u << 2; // pixel shader inputs
    inline constexpr uint32_t kDebugNonFiniteTexels = 1u << 3;
    inline constexpr uint32_t kDebugNonFinitePosition = 1u << 4;
    inline constexpr uint32_t kDebugNonFiniteExports = 1u << 5;       // vertex shader interpolators
    inline constexpr uint32_t kDebugNonFiniteColor = 1u << 6;         // or depth

    struct TranslationOptions
    {
        // From the environment (above).
        bool xenosProducts = XenosProducts();
        uint32_t debugNan = DebugNan();
        // Pixel shaders: an alpha test that passes with a comparison of alpha above or below the reference turns its edge
        // into partial coverage (SV_Coverage) instead of a hard cut, over about a pixel, for multisampled targets
        // (alphaCoverageSamples), smoothing alpha-tested hair and foliage. The test itself is unchanged elsewhere.
        bool alphaCoverage = false;
        // Pixel shaders: the interpolators (a bit each) sampled at the centroid of the pixel's covered samples rather than at
        // its center, as SQ_INTERPOLATOR_CNTL and SQ_CONTEXT_MISC ask for multisampled targets.
        uint32_t centroidInterpolators = 0;
    };

    HlslResult TranslateToHlsl(const ParsedShader& shader, const TranslationOptions& options = {});

    // Geometry shaders expanding rectangle lists (3 vertices -> a quad) and point lists (-> sprites).
    const char* RectangleListGeometryShader();
    const char* PointListGeometryShader();
}
