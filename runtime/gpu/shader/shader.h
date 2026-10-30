#pragma once

// Parsed Xenos shaders: the control flow program, decoded operands and the resources a shader uses.
#include "gpu/renderer.h"
#include "gpu/shader/ucode.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gpu::shader
{
    struct Operand
    {
        enum class Relative : uint8_t
        {
            kNone,
            kA0, // c[a0 + index]
            kAL, // r[aL + index] or c[aL + index]
        };

        bool isTemp = true;
        uint32_t index = 0;
        Relative relative = Relative::kNone;
        bool negate = false;
        bool absolute = false;
        uint32_t componentCount = 4;
        uint8_t components[4] = { 0, 1, 2, 3 };
    };

    // Vector operation source `i` (1-3).
    Operand VectorOperand(const ucode::AluInstruction& alu, uint32_t i);
    // Scalar operation sources; returns how many operands were filled. Most opcodes take a and b from
    // one register (out[0] with 1 or 2 components); mulsc/addsc/subsc take a constant (out[0]) and a
    // temp (out[1]), one component each.
    uint32_t ScalarOperands(const ucode::AluInstruction& alu, Operand out[2]);
    uint32_t VectorOperandCount(ucode::AluVectorOpcode opcode);

    // A texture binding: one fetch constant sampled with one dimension.
    struct TextureBinding
    {
        uint32_t fetchConstant;
        uint32_t dimension; // ucode dimension: 0 1D, 1 2D, 2 3D, 3 cube
    };

    // A sampler binding: one fetch constant with the instruction's filter overrides.
    struct SamplerBinding
    {
        uint32_t fetchConstant;
        uint32_t magFilter, minFilter, mipFilter, anisoFilter; // 3 / 7 = from the fetch constant
    };

    struct ParsedShader
    {
        ShaderStage stage = ShaderStage::kVertex;
        uint64_t hash = 0;
        std::vector<uint32_t> ucode;
        std::vector<ucode::CfInstruction> cf;

        // Analysis.
        bool structuredControlFlow = true; // no loops, calls or jumps
        bool forwardControlFlow = true;    // no loops or calls, and jumps that never close a cycle
        std::vector<uint32_t> forwardOrder; // with forwardControlFlow (and jumps): the CF instructions in an order where each
                                            // follows all that can lead to it
        uint32_t tempCount = 0;            // highest temp register used + 1
        bool relativeTemps = false;
        bool relativeConstants = false;
        uint32_t floatConstantCount = 0;   // highest float constant read with a static index + 1
        bool usesA0 = false;
        bool killsPixels = false;
        bool memoryExport = false;
        uint32_t interpolatorsWritten = 0; // vertex shader exports o0-o15 (bitmask)
        bool writesPosition = false;
        bool writesPointSize = false;
        uint32_t colorTargetsWritten = 0; // pixel shader oC0-oC3 (bitmask)
        bool writesDepth = false;
        std::vector<uint32_t> vertexFetchConstants; // vertex fetch constant indices used (2-dword slots)
        // The first full vertex fetch, where D3D's clears and blits read their screen-space position: its fetch constant
        // (UINT32_MAX if none), stride and offset in dwords, and format.
        uint32_t firstVertexConstant = UINT32_MAX, firstVertexStride = 0, firstVertexFormat = 0;
        int32_t firstVertexOffset = 0;
        std::vector<TextureBinding> textures;
        std::vector<SamplerBinding> samplers;

        bool valid = false;
        std::string error;

        uint32_t TextureBindingIndex(const ucode::FetchInstruction& fetch) const;
        uint32_t SamplerBindingIndex(const ucode::FetchInstruction& fetch) const;
        const uint32_t* Instruction(uint32_t address) const { return ucode.data() + address * 3; }
    };

    ParsedShader Parse(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount);
    std::string Disassemble(const ParsedShader& shader);

    uint64_t HashUcode(const uint32_t* ucode, uint32_t dwordCount);

    // A shader binary as the XDK compiler writes them (DOA4's 526 are compiled into default.pe): a big-endian
    // header with magic 0x102A0E00 (pixel) or 0x102A0E01 (vertex), or 0x102A11xx, then the microcode's offset
    // and size (bytes, from the header). Many have no constant table, so its version can't tell the stage.
    struct ShaderBinary
    {
        size_t offset = 0; // of the header
        ShaderStage stage = ShaderStage::kVertex;
        std::vector<uint32_t> ucode; // host-endian dwords, as Parse takes them
    };
    std::vector<ShaderBinary> FindShaderBinaries(const uint8_t* data, size_t size);
}
