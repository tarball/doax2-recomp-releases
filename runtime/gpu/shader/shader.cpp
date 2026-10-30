#include "gpu/shader/shader.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <queue>

namespace gpu::shader
{
    using namespace ucode;

    uint64_t HashUcode(const uint32_t* ucode, uint32_t dwordCount)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < dwordCount; i++)
        {
            for (int b = 0; b < 4; b++)
                hash = (hash ^ ((ucode[i] >> (b * 8)) & 0xFF)) * 0x100000001B3ull;
        }
        return hash;
    }

    std::vector<ShaderBinary> FindShaderBinaries(const uint8_t* data, size_t size)
    {
        const auto load = [&](size_t offset) {
            return offset + 4 <= size ? (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) | (uint32_t(data[offset + 2]) << 8) | data[offset + 3]
                                      : 0u;
        };
        std::vector<ShaderBinary> binaries;
        for (size_t offset = 0; offset + 0x24 <= size; offset += 4)
        {
            const uint32_t magic = load(offset);
            if ((magic & 0xFFFFFF00) != 0x102A0E00 && (magic & 0xFFFFFF00) != 0x102A1100)
                continue;
            if ((magic & 0xFF) > 1)
                continue;
            // The microcode: whole instructions (3 dwords each). In the older container (DOA4's, 0x102A0E0x) the header
            // gives its offset and size. In the newer one (DOAX2's, 0x102A110x, as XenosRecomp reads it), the header is
            // { flags, virtual size, physical size, ?, constant table, definition table, shader offset, ?, ? } and the
            // shader record at its offset starts with the microcode's offset after the virtual part, and its size.
            uint32_t ucodeOffset, ucodeSize;
            if ((magic & 0xFFFFFF00) == 0x102A1100)
            {
                const uint32_t virtualSize = load(offset + 4), shaderOffset = load(offset + 0x18);
                if (!shaderOffset || shaderOffset % 4 || shaderOffset > 0x10000 || offset + shaderOffset + 8 > size || virtualSize > 0x100000)
                    continue;
                ucodeOffset = virtualSize + load(offset + shaderOffset);
                ucodeSize = load(offset + shaderOffset + 4);
            }
            else
            {
                ucodeOffset = load(offset + 4);
                ucodeSize = load(offset + 8);
            }
            if (!ucodeSize || ucodeSize % 12 || ucodeOffset < 0x18 || ucodeOffset % 4 || ucodeOffset > 0x110000 || ucodeSize > 0x40000 ||
                offset + ucodeOffset + ucodeSize > size)
                continue;
            ShaderBinary& binary = binaries.emplace_back();
            binary.offset = offset;
            binary.stage = (magic & 0xFF) == 1 ? ShaderStage::kVertex : ShaderStage::kPixel;
            binary.ucode.resize(ucodeSize / 4);
            for (size_t i = 0; i < binary.ucode.size(); i++)
                binary.ucode[i] = load(offset + ucodeOffset + i * 4);
        }
        return binaries;
    }

    uint32_t VectorOperandCount(AluVectorOpcode opcode)
    {
        switch (opcode)
        {
        case AluVectorOpcode::kFrc:
        case AluVectorOpcode::kTrunc:
        case AluVectorOpcode::kFloor:
        case AluVectorOpcode::kMax4:
            return 1;
        case AluVectorOpcode::kMad:
        case AluVectorOpcode::kCndEq:
        case AluVectorOpcode::kCndGe:
        case AluVectorOpcode::kCndGt:
        case AluVectorOpcode::kDp2Add:
            return 3;
        default:
            return 2;
        }
    }

    namespace
    {
        // Constants and temps use different encodings of the 8-bit register field.
        Operand DecodeRegister(const AluInstruction& alu, uint32_t reg, bool isTemp, uint32_t constSlot)
        {
            Operand operand;
            operand.isTemp = isTemp;
            if (isTemp)
            {
                operand.index = reg & 0x3F;
                operand.relative = (reg & 0x40) ? Operand::Relative::kAL : Operand::Relative::kNone;
                operand.absolute = (reg & 0x80) != 0;
            }
            else
            {
                operand.index = reg;
                const bool relative = constSlot == 0 ? alu.Const0Relative() : alu.Const1Relative();
                if (relative)
                    operand.relative = alu.AddressAbsolute() ? Operand::Relative::kA0 : Operand::Relative::kAL;
                operand.absolute = alu.AbsConstants();
            }
            return operand;
        }

        uint32_t ConstSlot(const AluInstruction& alu, uint32_t i)
        {
            switch (i)
            {
            case 2: return alu.SrcIsTemp(1) ? 0 : 1;
            case 3: return alu.SrcIsTemp(1) && alu.SrcIsTemp(2) ? 0 : 1;
            default: return 0;
            }
        }

        // Scalar opcodes reading two components (a, b) of source 3, or none.
        uint32_t ScalarSourceCount(AluScalarOpcode opcode)
        {
            switch (opcode)
            {
            case AluScalarOpcode::kAdds:
            case AluScalarOpcode::kMuls:
            case AluScalarOpcode::kMulsPrev2:
            case AluScalarOpcode::kMaxs:
            case AluScalarOpcode::kMins:
            case AluScalarOpcode::kMaxAs:
            case AluScalarOpcode::kMaxAsf:
            case AluScalarOpcode::kSubs:
                return 2;
            case AluScalarOpcode::kSetpClr:
            case AluScalarOpcode::kRetainPrev:
                return 0;
            default:
                return 1;
            }
        }

        bool IsScalarConstOp(AluScalarOpcode opcode)
        {
            return opcode >= AluScalarOpcode::kMulsc0 && opcode <= AluScalarOpcode::kSubsc1;
        }
    }

    Operand VectorOperand(const AluInstruction& alu, uint32_t i)
    {
        Operand operand = DecodeRegister(alu, alu.SrcReg(i), alu.SrcIsTemp(i), ConstSlot(alu, i));
        operand.negate = alu.SrcNegate(i);
        // Each 2-bit swizzle field is relative to its component.
        const uint32_t swizzle = alu.SrcSwizzle(i);
        operand.componentCount = 4;
        for (uint32_t c = 0; c < 4; c++)
            operand.components[c] = uint8_t(((swizzle >> (c * 2)) + c) & 3);
        return operand;
    }

    uint32_t ScalarOperands(const AluInstruction& alu, Operand out[2])
    {
        const AluScalarOpcode opcode = alu.ScalarOpcode();
        const uint32_t swizzle = alu.SrcSwizzle(3);
        if (IsScalarConstOp(opcode))
        {
            // A constant (source 3's register) and a temp whose number is spread over other fields.
            out[0] = DecodeRegister(alu, alu.SrcReg(3), false, ConstSlot(alu, 3));
            out[0].negate = alu.SrcNegate(3);
            out[0].componentCount = 1;
            out[0].components[0] = uint8_t(((swizzle >> 6) + 3) & 3);
            const uint32_t temp = (uint32_t(opcode) & 1) | (alu.SrcIsTemp(3) ? 2 : 0) | (swizzle & 0x3C);
            out[1] = Operand{};
            out[1].isTemp = true;
            out[1].index = temp;
            out[1].negate = alu.SrcNegate(3);
            out[1].componentCount = 1;
            out[1].components[0] = uint8_t(swizzle & 3);
            return 2;
        }

        const uint32_t count = ScalarSourceCount(opcode);
        if (count == 0)
            return 0;
        Operand operand = DecodeRegister(alu, alu.SrcReg(3), alu.SrcIsTemp(3), ConstSlot(alu, 3));
        operand.negate = alu.SrcNegate(3);
        // a is the w field (relative to component 3), b the x field.
        operand.componentCount = count;
        operand.components[0] = uint8_t(((swizzle >> 6) + 3) & 3);
        operand.components[1] = uint8_t(swizzle & 3);
        out[0] = operand;
        return 1;
    }

    uint32_t ParsedShader::TextureBindingIndex(const FetchInstruction& fetch) const
    {
        for (uint32_t i = 0; i < textures.size(); i++)
        {
            if (textures[i].fetchConstant == fetch.TextureConstantIndex() && textures[i].dimension == fetch.Dimension())
                return i;
        }
        return UINT32_MAX;
    }

    uint32_t ParsedShader::SamplerBindingIndex(const FetchInstruction& fetch) const
    {
        for (uint32_t i = 0; i < samplers.size(); i++)
        {
            const SamplerBinding& s = samplers[i];
            if (s.fetchConstant == fetch.TextureConstantIndex() && s.magFilter == fetch.MagFilter() && s.minFilter == fetch.MinFilter() &&
                s.mipFilter == fetch.MipFilter() && s.anisoFilter == fetch.AnisoFilter())
                return i;
        }
        return UINT32_MAX;
    }

    namespace
    {
        void NoteTemp(ParsedShader& shader, uint32_t index, bool relative)
        {
            shader.tempCount = std::max(shader.tempCount, index + 1);
            if (relative)
                shader.relativeTemps = true;
        }

        void NoteOperand(ParsedShader& shader, const Operand& operand)
        {
            if (operand.isTemp)
                NoteTemp(shader, operand.index, operand.relative != Operand::Relative::kNone);
            else if (operand.relative != Operand::Relative::kNone)
            {
                shader.relativeConstants = true;
                if (operand.relative == Operand::Relative::kA0)
                    shader.usesA0 = true;
            }
            else
            {
                shader.floatConstantCount = std::max(shader.floatConstantCount, operand.index + 1);
            }
        }

        void NoteExport(ParsedShader& shader, uint32_t index)
        {
            if (shader.stage == ShaderStage::kVertex)
            {
                if (index == kExportPosition)
                    shader.writesPosition = true;
                else if (index == kExportPointSize)
                    shader.writesPointSize = true;
                else if (index < 16)
                    shader.interpolatorsWritten |= 1u << index;
                else if (index >= kExportMemoryAddress && index <= kExportMemoryData0 + 4)
                    shader.memoryExport = true;
            }
            else
            {
                if (index < 4)
                    shader.colorTargetsWritten |= 1u << index;
                else if (index == kExportDepth)
                    shader.writesDepth = true;
                else if (index >= kExportMemoryAddress && index <= kExportMemoryData0 + 4)
                    shader.memoryExport = true;
            }
        }

        void AnalyzeAlu(ParsedShader& shader, const AluInstruction& alu)
        {
            const AluVectorOpcode vectorOp = alu.VectorOpcode();
            const uint32_t vectorCount = VectorOperandCount(vectorOp);
            for (uint32_t i = 1; i <= vectorCount; i++)
                NoteOperand(shader, VectorOperand(alu, i));
            Operand scalarOperands[2];
            const uint32_t scalarCount = ScalarOperands(alu, scalarOperands);
            for (uint32_t i = 0; i < scalarCount; i++)
                NoteOperand(shader, scalarOperands[i]);

            if (vectorOp == AluVectorOpcode::kMaxA)
                shader.usesA0 = true;
            const AluScalarOpcode scalarOp = alu.ScalarOpcode();
            if (scalarOp == AluScalarOpcode::kMaxAs || scalarOp == AluScalarOpcode::kMaxAsf)
                shader.usesA0 = true;
            if ((vectorOp >= AluVectorOpcode::kKillEq && vectorOp <= AluVectorOpcode::kKillNe) ||
                (scalarOp >= AluScalarOpcode::kKillsEq && scalarOp <= AluScalarOpcode::kKillsOne))
                shader.killsPixels = true;

            if (alu.IsExport())
            {
                if (alu.VectorWriteMask() || alu.ScalarWriteMask() || alu.Constant0WriteMask())
                    NoteExport(shader, alu.VectorDest());
            }
            else
            {
                if (alu.VectorWriteMask())
                    NoteTemp(shader, alu.VectorDest(), alu.VectorDestRelative());
                if (alu.ScalarWriteMask())
                    NoteTemp(shader, alu.ScalarDest(), alu.ScalarDestRelative());
            }
        }

        void AnalyzeFetch(ParsedShader& shader, const FetchInstruction& fetch)
        {
            switch (fetch.Opcode())
            {
            case FetchOpcode::kVertexFetch:
                NoteTemp(shader, fetch.SrcReg(), fetch.SrcRelative());
                NoteTemp(shader, fetch.DstReg(), fetch.DstRelative());
                if (!fetch.VertexMiniFetch())
                {
                    const uint32_t constant = fetch.VertexConstantIndex();
                    if (shader.firstVertexConstant == UINT32_MAX)
                    {
                        shader.firstVertexConstant = constant;
                        shader.firstVertexStride = fetch.VertexStride();
                        shader.firstVertexOffset = fetch.VertexOffset();
                        shader.firstVertexFormat = fetch.VertexFormat();
                    }
                    if (std::find(shader.vertexFetchConstants.begin(), shader.vertexFetchConstants.end(), constant) == shader.vertexFetchConstants.end())
                        shader.vertexFetchConstants.push_back(constant);
                }
                break;
            case FetchOpcode::kTextureFetch:
            case FetchOpcode::kGetTextureComputedLod:
            case FetchOpcode::kGetTextureWeights:
            case FetchOpcode::kGetTextureBorderColorFrac:
                NoteTemp(shader, fetch.SrcReg(), fetch.SrcRelative());
                NoteTemp(shader, fetch.DstReg(), fetch.DstRelative());
                if (shader.TextureBindingIndex(fetch) == UINT32_MAX)
                    shader.textures.push_back({ fetch.TextureConstantIndex(), fetch.Dimension() });
                if (shader.SamplerBindingIndex(fetch) == UINT32_MAX)
                    shader.samplers.push_back(
                        { fetch.TextureConstantIndex(), fetch.MagFilter(), fetch.MinFilter(), fetch.MipFilter(), fetch.AnisoFilter() });
                break;
            case FetchOpcode::kGetTextureGradients:
                NoteTemp(shader, fetch.SrcReg(), fetch.SrcRelative());
                NoteTemp(shader, fetch.DstReg(), fetch.DstRelative());
                break;
            default:
                NoteTemp(shader, fetch.SrcReg(), fetch.SrcRelative());
                break;
            }
        }

        // For a shader with jumps but no loops or calls: an order of its CF instructions in which each comes after every
        // instruction that can lead to it (where control falls through or jumps), into shader.forwardOrder. The lowest
        // index goes first wherever the order allows, so shaders whose jumps all go forward keep their own order. Jumps
        // back that never close a cycle (an else block after the end that jumps back to the code after the if, as in 271
        // of DOA4's shaders) are fine. Returns false if the instructions form a cycle.
        bool OrderForward(ParsedShader& shader)
        {
            const uint32_t count = uint32_t(shader.cf.size());
            std::vector<std::vector<uint32_t>> successors(count);
            std::vector<uint32_t> predecessors(count, 0);
            for (uint32_t index = 0; index < count; index++)
            {
                const CfInstruction& cf = shader.cf[index];
                std::vector<uint32_t>& next = successors[index];
                switch (cf.Opcode())
                {
                case CfOpcode::kCondJmp:
                    next.push_back(cf.TargetAddress());
                    if (!cf.IsUnconditional())
                        next.push_back(index + 1);
                    break;
                case CfOpcode::kExec:
                case CfOpcode::kExecEnd:
                    if (!cf.IsEnd())
                        next.push_back(index + 1);
                    break;
                default: // (conditional execs go on whether or not they end the shader)
                    next.push_back(index + 1);
                    break;
                }
                next.erase(std::remove_if(next.begin(), next.end(), [count](uint32_t target) { return target >= count; }), next.end());
                for (uint32_t target : next)
                    predecessors[target]++;
            }
            std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
            for (uint32_t index = 0; index < count; index++)
            {
                if (!predecessors[index])
                    ready.push(index);
            }
            shader.forwardOrder.clear();
            while (!ready.empty())
            {
                const uint32_t index = ready.top();
                ready.pop();
                shader.forwardOrder.push_back(index);
                for (uint32_t target : successors[index])
                {
                    if (--predecessors[target] == 0)
                        ready.push(target);
                }
            }
            if (shader.forwardOrder.size() == count)
                return true;
            shader.forwardOrder.clear();
            return false;
        }
    }

    ParsedShader Parse(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount)
    {
        ParsedShader shader;
        shader.stage = stage;
        shader.ucode.assign(ucode, ucode + dwordCount);
        shader.hash = HashUcode(ucode, dwordCount);

        // The CF program runs up to the first instruction address any exec refers to.
        uint32_t cfEnd = dwordCount;
        for (uint32_t i = 0; i + 3 <= cfEnd; i += 3)
        {
            CfInstruction pair[2];
            UnpackCfPair(ucode + i, pair);
            for (const CfInstruction& cf : pair)
            {
                if (cf.IsExec() && cf.ExecCount())
                    cfEnd = std::min(cfEnd, cf.ExecAddress() * 3);
                shader.cf.push_back(cf);
            }
        }

        for (size_t index = 0; index < shader.cf.size(); index++)
        {
            const CfInstruction& cf = shader.cf[index];
            switch (cf.Opcode())
            {
            case CfOpcode::kLoopStart:
            case CfOpcode::kLoopEnd:
            case CfOpcode::kCondCall:
            case CfOpcode::kReturn:
                shader.structuredControlFlow = false;
                shader.forwardControlFlow = false;
                break;
            case CfOpcode::kCondJmp:
                shader.structuredControlFlow = false;
                break;
            default:
                break;
            }
            if (!cf.IsExec())
                continue;
            uint32_t sequence = cf.ExecSequence();
            for (uint32_t i = 0; i < cf.ExecCount(); i++, sequence >>= 2)
            {
                const uint32_t address = cf.ExecAddress() + i;
                if ((address + 1) * 3 > dwordCount)
                {
                    shader.error = "instruction address out of range";
                    return shader;
                }
                const uint32_t* words = shader.Instruction(address);
                if (sequence & 1)
                    AnalyzeFetch(shader, FetchInstruction{ { words[0], words[1], words[2] } });
                else
                    AnalyzeAlu(shader, AluInstruction{ { words[0], words[1], words[2] } });
            }
        }
        if (!shader.structuredControlFlow && shader.forwardControlFlow)
            shader.forwardControlFlow = OrderForward(shader);
        if (shader.relativeTemps)
            shader.tempCount = 64;
        shader.valid = true;
        return shader;
    }

    // Disassembly (for debugging; the syntax loosely follows the XDK's).

    namespace
    {
        const char* kVectorNames[] = { "add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad", "cndeq", "cndge",
            "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push", "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq",
            "kill_gt", "kill_ge", "kill_ne", "dst", "maxa", "op30", "op31" };
        const char* kScalarNames[] = { "adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts", "sges", "snes",
            "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq", "maxas", "maxasf", "subs",
            "subs_prev", "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop", "setp_clr", "setp_rstr", "kills_eq", "kills_gt",
            "kills_ge", "kills_ne", "kills_one", "sqrt", "op41", "mulsc", "mulsc", "addsc", "addsc", "subsc", "subsc", "sin", "cos",
            "retain_prev", "op51", "op52", "op53", "op54", "op55", "op56", "op57", "op58", "op59", "op60", "op61", "op62", "op63" };
        const char kComponents[] = "xyzw01?_";

        std::string OperandText(const Operand& operand)
        {
            std::string text;
            if (operand.negate)
                text += '-';
            if (operand.absolute)
                text += '|';
            text += operand.isTemp ? 'r' : 'c';
            if (operand.relative == Operand::Relative::kNone)
                text += std::to_string(operand.index);
            else
                text += std::string("[") + std::to_string(operand.index) + (operand.relative == Operand::Relative::kA0 ? "+a0]" : "+aL]");
            if (operand.absolute)
                text += '|';
            text += '.';
            for (uint32_t i = 0; i < operand.componentCount; i++)
                text += kComponents[operand.components[i]];
            return text;
        }

        std::string DestText(ShaderStage stage, bool isExport, uint32_t index, bool relative, uint32_t mask, uint32_t const0, uint32_t const1)
        {
            std::string text;
            if (isExport)
            {
                if (stage == ShaderStage::kVertex)
                    text = index == kExportPosition ? "oPos" : index == kExportPointSize ? "oPts" : index >= 32 ? "eM" + std::to_string(index - 32)
                                                                                                      : "o" + std::to_string(index);
                else
                    text = index == kExportDepth ? "oDepth" : index >= 32 ? "eM" + std::to_string(index - 32) : "oC" + std::to_string(index);
            }
            else
            {
                text = relative ? "r[" + std::to_string(index) + "+aL]" : "r" + std::to_string(index);
            }
            text += '.';
            for (uint32_t i = 0; i < 4; i++)
                text += (mask & (1u << i)) ? kComponents[i] : (const0 & (1u << i)) ? '0' : (const1 & (1u << i)) ? '1' : '_';
            return text;
        }

        std::string SwizzleText(uint32_t swizzle)
        {
            std::string text;
            for (uint32_t i = 0; i < 4; i++)
                text += kComponents[(swizzle >> (i * 3)) & 7];
            return text;
        }

        void DisassembleAlu(std::string& out, ShaderStage stage, const AluInstruction& alu)
        {
            char line[512];
            const char* predicate = alu.IsPredicated() ? (alu.PredicateCondition() ? "(p0) " : "(!p0) ") : "";
            const AluVectorOpcode vectorOp = alu.VectorOpcode();
            std::string vector = std::string(kVectorNames[uint32_t(vectorOp)]) + (alu.VectorClamp() ? "_sat " : " ") +
                DestText(stage, alu.IsExport(), alu.VectorDest(), alu.VectorDestRelative(), alu.VectorResultWriteMask(), alu.Constant0WriteMask(),
                    alu.Constant1WriteMask());
            for (uint32_t i = 1; i <= VectorOperandCount(vectorOp); i++)
                vector += ", " + OperandText(VectorOperand(alu, i));

            const AluScalarOpcode scalarOp = alu.ScalarOpcode();
            std::string scalar = std::string(kScalarNames[uint32_t(scalarOp)]) + (alu.ScalarClamp() ? "_sat " : " ") +
                DestText(stage, alu.IsExport(), alu.IsExport() ? alu.VectorDest() : alu.ScalarDest(), alu.ScalarDestRelative() && !alu.IsExport(),
                    alu.ScalarResultWriteMask(), 0, 0);
            Operand operands[2];
            const uint32_t count = ScalarOperands(alu, operands);
            for (uint32_t i = 0; i < count; i++)
                scalar += ", " + OperandText(operands[i]);

            snprintf(line, sizeof(line), "      %salu  %s\n      %s     + %s\n", predicate, vector.c_str(), predicate, scalar.c_str());
            out += line;
        }

        void DisassembleFetch(std::string& out, const FetchInstruction& fetch)
        {
            char line[512];
            const char* predicate = fetch.IsPredicated() ? (fetch.PredicateCondition() ? "(p0) " : "(!p0) ") : "";
            const std::string dst = (fetch.DstRelative() ? "r[" + std::to_string(fetch.DstReg()) + "+aL]" : "r" + std::to_string(fetch.DstReg())) +
                "." + SwizzleText(fetch.DstSwizzle());
            const std::string src = fetch.SrcRelative() ? "r[" + std::to_string(fetch.SrcReg()) + "+aL]" : "r" + std::to_string(fetch.SrcReg());
            switch (fetch.Opcode())
            {
            case FetchOpcode::kVertexFetch:
                snprintf(line, sizeof(line), "      %svfetch%s %s, %s.%c, vf%u (format %u, %s, %s, exp %d, stride %u, offset %d%s)\n", predicate,
                    fetch.VertexMiniFetch() ? "_mini" : "", dst.c_str(), src.c_str(), kComponents[fetch.VertexSrcComponent()],
                    fetch.VertexConstantIndex(), fetch.VertexFormat(), fetch.VertexSigned() ? "signed" : "unsigned",
                    fetch.VertexInteger() ? "integer" : "normalized", fetch.VertexExpAdjust(), fetch.VertexStride(), fetch.VertexOffset(),
                    fetch.VertexIndexRounded() ? ", rounded" : "");
                break;
            case FetchOpcode::kTextureFetch:
            {
                static const char* kDimensions[] = { "1D", "2D", "3D", "Cube" };
                std::string coords;
                for (uint32_t i = 0; i < 3; i++)
                    coords += kComponents[(fetch.TextureSrcSwizzle() >> (i * 2)) & 3];
                snprintf(line, sizeof(line),
                    "      %stfetch%s %s, %s.%s, tf%u (filters %u/%u/%u aniso %u%s%s%s, lod bias %.3f, offset %.1f %.1f %.1f)\n", predicate,
                    kDimensions[fetch.Dimension()], dst.c_str(), src.c_str(), coords.c_str(), fetch.TextureConstantIndex(), fetch.MagFilter(),
                    fetch.MinFilter(), fetch.MipFilter(), fetch.AnisoFilter(), fetch.TextureUnnormalized() ? ", unnormalized" : "",
                    fetch.UseComputedLod() ? ", computed lod" : "", fetch.UseRegisterLod() ? ", register lod" : "", fetch.LodBias(),
                    fetch.OffsetX(), fetch.OffsetY(), fetch.OffsetZ());
                break;
            }
            default:
                snprintf(line, sizeof(line), "      %sfetch op %u %s, %s, tf%u\n", predicate, uint32_t(fetch.Opcode()), dst.c_str(), src.c_str(),
                    fetch.TextureConstantIndex());
                break;
            }
            out += line;
        }
    }

    std::string Disassemble(const ParsedShader& shader)
    {
        std::string out;
        char line[256];
        snprintf(line, sizeof(line), "// %s shader %016llX, %zu dwords, %zu CF instructions, %u temps%s%s\n",
            shader.stage == ShaderStage::kVertex ? "vertex" : "pixel", (unsigned long long)shader.hash, shader.ucode.size(), shader.cf.size(),
            shader.tempCount, shader.structuredControlFlow ? "" : ", unstructured control flow", shader.relativeConstants ? ", relative constants" : "");
        out += line;

        for (size_t index = 0; index < shader.cf.size(); index++)
        {
            const CfInstruction& cf = shader.cf[index];
            static const char* kCfNames[] = { "nop", "exec", "exece", "cexec", "cexece", "cexec_pred", "cexece_pred", "loop_start", "loop_end",
                "ccall", "ret", "cjmp", "alloc", "cexec_pred_clean", "cexece_pred_clean", "mark_vs_fetch_done" };
            const CfOpcode op = cf.Opcode();
            switch (op)
            {
            case CfOpcode::kCondExec:
            case CfOpcode::kCondExecEnd:
                snprintf(line, sizeof(line), "%3zu: %s %sb%u (address %u, count %u)\n", index, kCfNames[uint32_t(op)], cf.Condition() ? "" : "!",
                    cf.BoolAddress(), cf.ExecAddress(), cf.ExecCount());
                break;
            case CfOpcode::kCondExecPred:
            case CfOpcode::kCondExecPredEnd:
                snprintf(line, sizeof(line), "%3zu: %s %sp0 (address %u, count %u)\n", index, kCfNames[uint32_t(op)], cf.Condition() ? "" : "!",
                    cf.ExecAddress(), cf.ExecCount());
                break;
            case CfOpcode::kCondExecPredClean:
            case CfOpcode::kCondExecPredCleanEnd:
                snprintf(line, sizeof(line), "%3zu: %s %sb%u (address %u, count %u)\n", index, kCfNames[uint32_t(op)], cf.Condition() ? "" : "!",
                    cf.BoolAddress(), cf.ExecAddress(), cf.ExecCount());
                break;
            case CfOpcode::kExec:
            case CfOpcode::kExecEnd:
                snprintf(line, sizeof(line), "%3zu: %s (address %u, count %u)\n", index, kCfNames[uint32_t(op)], cf.ExecAddress(), cf.ExecCount());
                break;
            case CfOpcode::kLoopStart:
            case CfOpcode::kLoopEnd:
                snprintf(line, sizeof(line), "%3zu: %s i%u -> %u%s\n", index, kCfNames[uint32_t(op)], cf.LoopId(), cf.TargetAddress(),
                    op == CfOpcode::kLoopStart ? (cf.IsRepeat() ? " (repeat)" : "") : (cf.IsPredicatedBreak() ? (cf.Condition() ? " (break p0)" : " (break !p0)") : ""));
                break;
            case CfOpcode::kCondCall:
            case CfOpcode::kCondJmp:
                if (cf.IsUnconditional())
                    snprintf(line, sizeof(line), "%3zu: %s -> %u\n", index, kCfNames[uint32_t(op)], cf.TargetAddress());
                else if (cf.IsPredicated())
                    snprintf(line, sizeof(line), "%3zu: %s %sp0 -> %u\n", index, kCfNames[uint32_t(op)], cf.Condition() ? "" : "!", cf.TargetAddress());
                else
                    snprintf(line, sizeof(line), "%3zu: %s %sb%u -> %u\n", index, kCfNames[uint32_t(op)], cf.Condition() ? "" : "!", cf.BoolAddress(),
                        cf.TargetAddress());
                break;
            case CfOpcode::kAlloc:
            {
                static const char* kAllocTypes[] = { "none", "position", "interpolators", "memory" };
                snprintf(line, sizeof(line), "%3zu: alloc %s, %u\n", index, kAllocTypes[cf.AllocType()], cf.AllocSize());
                break;
            }
            default:
                snprintf(line, sizeof(line), "%3zu: %s\n", index, kCfNames[uint32_t(op)]);
                break;
            }
            out += line;

            if (!cf.IsExec())
                continue;
            uint32_t sequence = cf.ExecSequence();
            for (uint32_t i = 0; i < cf.ExecCount(); i++, sequence >>= 2)
            {
                const uint32_t address = cf.ExecAddress() + i;
                if ((address + 1) * 3 > shader.ucode.size())
                    break;
                const uint32_t* words = shader.Instruction(address);
                if (sequence & 1)
                    DisassembleFetch(out, FetchInstruction{ { words[0], words[1], words[2] } });
                else
                    DisassembleAlu(out, shader.stage, AluInstruction{ { words[0], words[1], words[2] } });
            }
        }
        return out;
    }
}
