#pragma once

// Xenos shader microcode encoding (field layouts as documented by Xenia and freedreno's a2xx).
//
// A shader is a control flow (CF) program followed by ALU and fetch instructions. CF instructions
// are 48 bits, packed in pairs into 3 dwords at the start of the shader; exec CF instructions
// point at runs of up to 6 ALU/fetch instructions, each 3 dwords, addressed in 3-dword units.
#include <cstdint>

namespace gpu::ucode
{
    enum class CfOpcode : uint32_t
    {
        kNop = 0,
        kExec = 1,
        kExecEnd = 2,
        kCondExec = 3,
        kCondExecEnd = 4,
        kCondExecPred = 5,
        kCondExecPredEnd = 6,
        kLoopStart = 7,
        kLoopEnd = 8,
        kCondCall = 9,
        kReturn = 10,
        kCondJmp = 11,
        kAlloc = 12,
        kCondExecPredClean = 13,
        kCondExecPredCleanEnd = 14,
        kMarkVsFetchDone = 15,
    };

    // One 48-bit CF instruction, as the low 48 bits of `bits`.
    struct CfInstruction
    {
        uint64_t bits = 0;

        uint32_t Field(uint32_t shift, uint32_t width) const { return uint32_t(bits >> shift) & ((1u << width) - 1); }
        CfOpcode Opcode() const { return CfOpcode(Field(44, 4)); }
        bool IsExec() const
        {
            const CfOpcode op = Opcode();
            return (op >= CfOpcode::kExec && op <= CfOpcode::kCondExecPredEnd) || op == CfOpcode::kCondExecPredClean ||
                op == CfOpcode::kCondExecPredCleanEnd;
        }
        bool IsEnd() const
        {
            const CfOpcode op = Opcode();
            return op == CfOpcode::kExecEnd || op == CfOpcode::kCondExecEnd || op == CfOpcode::kCondExecPredEnd ||
                op == CfOpcode::kCondExecPredCleanEnd;
        }

        // Exec variants.
        uint32_t ExecAddress() const { return Field(0, 12); }  // in 3-dword instruction units
        uint32_t ExecCount() const { return Field(12, 3); }
        uint32_t ExecSequence() const { return Field(16, 12); } // 2 bits per instruction: bit 0 = fetch
        uint32_t BoolAddress() const { return Field(34, 8); }   // cond_exec, cond_call, cond_jmp
        bool Condition() const { return Field(42, 1) != 0; }

        // Loops, calls and jumps; addresses are CF instruction indices.
        uint32_t TargetAddress() const { return Field(0, 13); }
        bool IsRepeat() const { return Field(13, 1) != 0; }       // loop_start
        uint32_t LoopId() const { return Field(16, 5); }
        bool IsPredicatedBreak() const { return Field(21, 1) != 0; } // loop_end
        bool IsUnconditional() const { return Field(13, 1) != 0; } // cond_call, cond_jmp
        bool IsPredicated() const { return Field(14, 1) != 0; }    // cond_call, cond_jmp

        // alloc
        uint32_t AllocSize() const { return Field(0, 3); }
        uint32_t AllocType() const { return Field(41, 2); } // 1 position, 2 interpolators/colors, 3 memory
    };

    // CF instructions come in pairs packed into 3 dwords.
    inline void UnpackCfPair(const uint32_t* dwords, CfInstruction out[2])
    {
        out[0].bits = uint64_t(dwords[0]) | (uint64_t(dwords[1] & 0xFFFF) << 32);
        out[1].bits = uint64_t((dwords[1] >> 16) | (dwords[2] << 16)) | (uint64_t(dwords[2] >> 16) << 32);
    }

    enum class FetchOpcode : uint32_t
    {
        kVertexFetch = 0,
        kTextureFetch = 1,
        kGetTextureBorderColorFrac = 16,
        kGetTextureComputedLod = 17,
        kGetTextureGradients = 18,
        kGetTextureWeights = 19,
        kSetTextureLod = 24,
        kSetTextureGradientsHorz = 25,
        kSetTextureGradientsVert = 26,
    };

    inline int32_t SignExtend(uint32_t value, uint32_t width)
    {
        return int32_t(value << (32 - width)) >> (32 - width);
    }

    struct FetchInstruction
    {
        uint32_t d[3];

        FetchOpcode Opcode() const { return FetchOpcode(d[0] & 0x1F); }
        uint32_t SrcReg() const { return (d[0] >> 5) & 0x3F; }
        bool SrcRelative() const { return (d[0] >> 11) & 1; }
        uint32_t DstReg() const { return (d[0] >> 12) & 0x3F; }
        bool DstRelative() const { return (d[0] >> 18) & 1; }
        uint32_t DstSwizzle() const { return d[1] & 0xFFF; } // 3 bits per component: 0-3 xyzw, 4 = 0, 5 = 1, 7 = keep
        bool IsPredicated() const { return (d[1] >> 31) & 1; }
        bool PredicateCondition() const { return (d[2] >> 31) & 1; }

        // Vertex fetch.
        uint32_t VertexConstantIndex() const { return ((d[0] >> 20) & 0x1F) * 3 + ((d[0] >> 25) & 3); } // 2-dword slot
        uint32_t VertexSrcComponent() const { return (d[0] >> 30) & 3; }
        bool VertexSigned() const { return (d[1] >> 12) & 1; }
        bool VertexInteger() const { return (d[1] >> 13) & 1; } // else normalized
        bool VertexIndexRounded() const { return (d[1] >> 15) & 1; }
        uint32_t VertexFormat() const { return (d[1] >> 16) & 0x3F; }
        int32_t VertexExpAdjust() const { return SignExtend((d[1] >> 24) & 0x3F, 6); }
        bool VertexMiniFetch() const { return (d[1] >> 30) & 1; }
        uint32_t VertexStride() const { return d[2] & 0xFF; } // dwords
        int32_t VertexOffset() const { return SignExtend((d[2] >> 8) & 0x7FFFFF, 23); } // dwords

        // Texture fetch.
        uint32_t TextureConstantIndex() const { return (d[0] >> 20) & 0x1F; }
        bool TextureUnnormalized() const { return (d[0] >> 25) & 1; }
        uint32_t TextureSrcSwizzle() const { return (d[0] >> 26) & 0x3F; } // 2 bits per component, absolute
        uint32_t MagFilter() const { return (d[1] >> 12) & 3; } // 0 point, 1 linear, 2 base map, 3 from fetch constant
        uint32_t MinFilter() const { return (d[1] >> 14) & 3; }
        uint32_t MipFilter() const { return (d[1] >> 16) & 3; }
        uint32_t AnisoFilter() const { return (d[1] >> 18) & 7; } // 7 = from fetch constant
        bool UseComputedLod() const { return (d[1] >> 28) & 1; }
        bool UseRegisterLod() const { return (d[1] >> 29) & 1; }
        bool UseRegisterGradients() const { return d[2] & 1; }
        float LodBias() const { return SignExtend((d[2] >> 2) & 0x7F, 7) / 16.0f; }
        uint32_t Dimension() const { return (d[2] >> 14) & 3; } // 0 1D, 1 2D, 2 3D/stacked, 3 cube
        float OffsetX() const { return SignExtend((d[2] >> 16) & 0x1F, 5) * 0.5f; }
        float OffsetY() const { return SignExtend((d[2] >> 21) & 0x1F, 5) * 0.5f; }
        float OffsetZ() const { return SignExtend((d[2] >> 26) & 0x1F, 5) * 0.5f; }
    };

    enum class AluVectorOpcode : uint32_t
    {
        kAdd = 0,
        kMul = 1,
        kMax = 2,
        kMin = 3,
        kSeq = 4,
        kSgt = 5,
        kSge = 6,
        kSne = 7,
        kFrc = 8,
        kTrunc = 9,
        kFloor = 10,
        kMad = 11,
        kCndEq = 12,
        kCndGe = 13,
        kCndGt = 14,
        kDp4 = 15,
        kDp3 = 16,
        kDp2Add = 17,
        kCube = 18,
        kMax4 = 19,
        kSetpEqPush = 20,
        kSetpNePush = 21,
        kSetpGtPush = 22,
        kSetpGePush = 23,
        kKillEq = 24,
        kKillGt = 25,
        kKillGe = 26,
        kKillNe = 27,
        kDst = 28,
        kMaxA = 29,
    };

    enum class AluScalarOpcode : uint32_t
    {
        kAdds = 0,
        kAddsPrev = 1,
        kMuls = 2,
        kMulsPrev = 3,
        kMulsPrev2 = 4,
        kMaxs = 5,
        kMins = 6,
        kSeqs = 7,
        kSgts = 8,
        kSges = 9,
        kSnes = 10,
        kFrcs = 11,
        kTruncs = 12,
        kFloors = 13,
        kExp = 14,
        kLogc = 15,
        kLog = 16,
        kRcpc = 17,
        kRcpf = 18,
        kRcp = 19,
        kRsqc = 20,
        kRsqf = 21,
        kRsq = 22,
        kMaxAs = 23,
        kMaxAsf = 24,
        kSubs = 25,
        kSubsPrev = 26,
        kSetpEq = 27,
        kSetpNe = 28,
        kSetpGt = 29,
        kSetpGe = 30,
        kSetpInv = 31,
        kSetpPop = 32,
        kSetpClr = 33,
        kSetpRstr = 34,
        kKillsEq = 35,
        kKillsGt = 36,
        kKillsGe = 37,
        kKillsNe = 38,
        kKillsOne = 39,
        kSqrt = 40,
        kMulsc0 = 42,
        kMulsc1 = 43,
        kAddsc0 = 44,
        kAddsc1 = 45,
        kSubsc0 = 46,
        kSubsc1 = 47,
        kSin = 48,
        kCos = 49,
        kRetainPrev = 50,
    };

    struct AluInstruction
    {
        uint32_t d[3];

        uint32_t VectorDest() const { return d[0] & 0x3F; }
        bool VectorDestRelative() const { return (d[0] >> 6) & 1; }
        bool AbsConstants() const { return (d[0] >> 7) & 1; }
        uint32_t ScalarDest() const { return (d[0] >> 8) & 0x3F; }
        bool ScalarDestRelative() const { return (d[0] >> 14) & 1; }
        bool IsExport() const { return (d[0] >> 15) & 1; }
        uint32_t VectorWriteMask() const { return (d[0] >> 16) & 0xF; }
        uint32_t ScalarWriteMask() const { return (d[0] >> 20) & 0xF; }
        bool VectorClamp() const { return (d[0] >> 24) & 1; }
        bool ScalarClamp() const { return (d[0] >> 25) & 1; }
        AluScalarOpcode ScalarOpcode() const { return AluScalarOpcode((d[0] >> 26) & 0x3F); }

        // Sources are numbered 1-3 as in the documentation; scalar operations read source 3.
        uint32_t SrcSwizzle(uint32_t i) const { return (d[1] >> (8 * (3 - i))) & 0xFF; }
        bool SrcNegate(uint32_t i) const { return (d[1] >> (24 + (3 - i))) & 1; }
        bool PredicateCondition() const { return (d[1] >> 27) & 1; }
        bool IsPredicated() const { return (d[1] >> 28) & 1; }
        bool AddressAbsolute() const { return (d[1] >> 29) & 1; } // relative constants use a0 (else aL)
        bool Const1Relative() const { return (d[1] >> 30) & 1; }
        bool Const0Relative() const { return (d[1] >> 31) & 1; }
        uint32_t SrcReg(uint32_t i) const { return (d[2] >> (8 * (3 - i))) & 0xFF; }
        AluVectorOpcode VectorOpcode() const { return AluVectorOpcode((d[2] >> 24) & 0x1F); }
        bool SrcIsTemp(uint32_t i) const { return (d[2] >> (29 + (3 - i))) & 1; }

        // Export instructions write constant 0/1 to components selected by mask combinations.
        uint32_t Constant1WriteMask() const { return IsExport() ? VectorWriteMask() & ScalarWriteMask() : 0; }
        uint32_t Constant0WriteMask() const
        {
            return IsExport() && ScalarDestRelative() ? 0xF & ~(VectorWriteMask() | ScalarWriteMask()) : 0;
        }
        uint32_t VectorResultWriteMask() const { return VectorWriteMask() & ~Constant1WriteMask(); }
        uint32_t ScalarResultWriteMask() const { return IsExport() ? ScalarWriteMask() & ~VectorWriteMask() : ScalarWriteMask(); }
    };

    // Vertex fetch formats.
    enum VertexFormat : uint32_t
    {
        kVertex_8_8_8_8 = 6,
        kVertex_2_10_10_10 = 7,
        kVertex_10_11_11 = 16,
        kVertex_11_11_10 = 17,
        kVertex_16_16 = 25,
        kVertex_16_16_16_16 = 26,
        kVertex_16_16_FLOAT = 31,
        kVertex_16_16_16_16_FLOAT = 32,
        kVertex_32 = 33,
        kVertex_32_32 = 34,
        kVertex_32_32_32_32 = 35,
        kVertex_32_FLOAT = 36,
        kVertex_32_32_FLOAT = 37,
        kVertex_32_32_32_32_FLOAT = 38,
        kVertex_32_32_32_FLOAT = 57,
    };

    // Export register indices.
    inline constexpr uint32_t kExportPosition = 62;     // vertex shader
    inline constexpr uint32_t kExportPointSize = 63;    // vertex shader: x point size, y edge flag, z kill vertex
    inline constexpr uint32_t kExportDepth = 61;        // pixel shader
    inline constexpr uint32_t kExportMemoryAddress = 32; // eA
    inline constexpr uint32_t kExportMemoryData0 = 33;   // eM0-eM4
}
