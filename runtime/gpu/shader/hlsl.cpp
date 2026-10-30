#include "gpu/shader/hlsl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gpu::shader
{
    using namespace ucode;

    namespace
    {
        const char kHlslCommon[] = R"(
#define XE_FLT_MAX 3.402823466e+38
cbuffer XeSystemConstants : register(b0)
{
    float3 xe_ndc_scale;
    uint xe_flags;
    float3 xe_ndc_offset;
    uint xe_vertex_index_endian;
    int xe_vertex_base_index;
    uint xe_vertex_index_min;
    uint xe_vertex_index_max;
    float xe_point_size;
    float2 xe_point_size_min_max;
    float2 xe_point_screen_to_ndc;
    float xe_alpha_ref;
    uint xe_alpha_func;
    uint xe_param_gen_register;
    float xe_resolution_scale;
    float4 xe_color_exp_bias;
    uint4 xe_min_max_factors;
    uint xe_alpha_coverage_samples;
};
cbuffer XeFloatConstants : register(b1)
{
    float4 xe_c[256];
};
cbuffer XeBoolLoopFetchConstants : register(b2)
{
    uint4 xe_bool_constants[2];
    uint4 xe_loop_constants[8];
    uint4 xe_fetch_constants[48];
};
ByteAddressBuffer xe_shared_memory : register(t0, space1);
// Bindless textures and samplers: the stage's bindings index the persistent descriptor heap.
cbuffer XeBindings : register(b3)
{
    uint4 xe_texture_indices[8];
    uint4 xe_sampler_indices[8];
};
Texture2D<float4> xe_textures_2d[] : register(t0, space2);
Texture3D<float4> xe_textures_3d[] : register(t0, space3);
TextureCube<float4> xe_textures_cube[] : register(t0, space4);
SamplerState xe_samplers[] : register(s0, space0);

struct XeVertexOutput
{
    float4 position : SV_Position;
    float4 interpolators[16] : TEXCOORD0;
    float2 point_coordinates : XEPOINTCOORD;
    float point_size : XEPOINTSIZE;
};

struct XePixelInput
{
    float4 position : SV_Position;
    float4 interpolators[16] : TEXCOORD0;
    float2 point_coordinates : XEPOINTCOORD;
    bool front_face : SV_IsFrontFace;
};

uint4 XeEndianSwap(uint4 v, uint endian)
{
    if (endian == 1u)
        v = ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
    else if (endian == 2u)
        v = (v << 24u) | ((v & 0xFF00u) << 8u) | ((v >> 8u) & 0xFF00u) | (v >> 24u);
    else if (endian == 3u)
        v = (v << 16u) | (v >> 16u);
    return v;
}

bool XeBoolConstant(uint index)
{
    return ((xe_bool_constants[index >> 7u][(index >> 5u) & 3u] >> (index & 31u)) & 1u) != 0u;
}

uint XeLoopConstant(uint index)
{
    return xe_loop_constants[index >> 2u][index & 3u];
}

float XeLogClamp(float a)
{
    float r = log2(a);
    return (isinf(r) && r < 0.0) ? -XE_FLT_MAX : r;
}

// Products. The Xenos ALU's, like Direct3D 9's, are 0 when either factor is 0, even times infinity or NaN (with
// XE_XENOS_PRODUCTS, see XenosProducts in hlsl.h); IEEE ones are NaN then. XE_DEBUG_NAN notes products where they differ,
// and the first and last ALU instruction (address) with one.
static bool xe_debug_product = false;
static uint xe_debug_instruction = 0u, xe_debug_first = 0xFFFFu, xe_debug_last = 0u;

// (XeMul and XeDot have no local variables: in some of DOAX2's state machine pixel shaders, FXC failed on any, at any
// optimization level, with "internal error: invalid access of unbound variable".)
float4 XeMul(float4 a, float4 b)
{
#ifdef XE_DEBUG_NAN
    if (any(min(abs(a), abs(b)) == 0.0 && (asuint(a * b) & 0x7F800000u) == 0x7F800000u))
    {
        xe_debug_product = true;
        xe_debug_first = min(xe_debug_first, xe_debug_instruction);
        xe_debug_last = max(xe_debug_last, xe_debug_instruction);
    }
#endif
#ifdef XE_XENOS_PRODUCTS
    return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b;
#else
    return a * b;
#endif
}

float XeMul(float a, float b)
{
    return XeMul(float4(a, 0.0, 0.0, 0.0), float4(b, 0.0, 0.0, 0.0)).x;
}

float XeSum(float4 v)
{
    return v.x + v.y + v.z + v.w;
}

float XeDot(float4 a, float4 b)
{
    return XeSum(XeMul(a, b));
}

#ifdef XE_DEBUG_NAN
RWByteAddressBuffer xe_debug_counts : register(u0);

bool4 XeNonFinite(float4 v)
{
    return (asuint(v) & 0x7F800000u) == 0x7F800000u;
}

// The non-finite components of an interpolator, as 4 bits (x lowest).
uint XeNonFiniteNibble(float4 v)
{
    return (XeNonFinite(v).x ? 1u : 0u) | (XeNonFinite(v).y ? 2u : 0u) | (XeNonFinite(v).z ? 4u : 0u) | (XeNonFinite(v).w ? 8u : 0u);
}

// Counts an invocation of the shader (by its hash) in its slot (kDebugCountSlots and kDebugSlotDwords in hlsl.h):
// stage is 1 for pixel shaders, outputs and inputs the kDebugNonFinite bits of what was non-finite, components the
// interpolator components that were (XeNonFiniteNibble's, 8 interpolators a dword).
void XeDebugCount(uint2 hash, uint stage, bool product, uint outputs, uint inputs, uint2 components)
{
    if (product || outputs != 0u || inputs != 0u)
    {
        uint slot = (hash.x & 4095u) * 64u;
        xe_debug_counts.Store2(slot, hash);
        xe_debug_counts.InterlockedOr(slot + 8u, stage | outputs | inputs);
        if (any(components != 0u))
        {
            xe_debug_counts.InterlockedOr(slot + 32u, components.x);
            xe_debug_counts.InterlockedOr(slot + 36u, components.y);
        }
        if (product)
        {
            xe_debug_counts.InterlockedAdd(slot + 12u, 1u);
            xe_debug_counts.InterlockedMax(slot + 20u, 0xFFFFu - xe_debug_first);
            xe_debug_counts.InterlockedMax(slot + 24u, xe_debug_last);
        }
        if (outputs != 0u)
            xe_debug_counts.InterlockedAdd(slot + 16u, 1u);
        if (inputs != 0u)
            xe_debug_counts.InterlockedAdd(slot + 28u, 1u);
    }
}
#endif

float XeClampMax(float r)
{
    return clamp(r, -XE_FLT_MAX, XE_FLT_MAX);
}

float XeFlushInfinity(float r)
{
    return isinf(r) ? 0.0 : r;
}

float XeMulsPrev2(float a, float b, float ps)
{
    return (ps == -XE_FLT_MAX || !(abs(ps) <= XE_FLT_MAX) || !(abs(b) <= XE_FLT_MAX) || b <= 0.0) ? -XE_FLT_MAX : a * ps;
}

int XeAddressFromFloat(float value)
{
    return int(clamp(value, -256.0, 255.0));
}

// Xenos cube: operand 0 is v.zzxy, operand 1 v.yxzz. Returns (t, s, 2 * major axis, face).
float4 XeCube(float4 src0, float4 src1)
{
    float3 v = float3(src1.y, src1.x, src0.x);
    float3 a = abs(v);
    if (a.z >= a.x && a.z >= a.y)
        return float4(-v.y, v.z < 0.0 ? -v.x : v.x, 2.0 * v.z, v.z < 0.0 ? 5.0 : 4.0);
    if (a.y >= a.x)
        return float4(v.y < 0.0 ? -v.z : v.z, v.x, 2.0 * v.y, v.y < 0.0 ? 3.0 : 2.0);
    return float4(-v.y, v.x < 0.0 ? v.z : -v.z, 2.0 * v.x, v.x < 0.0 ? 1.0 : 0.0);
}

// Cube fetch coordinates (s and t in [1, 2], face index) to a direction.
float3 XeCubeDirection(float3 c)
{
    float s = (c.x - 1.5) * 2.0, t = (c.y - 1.5) * 2.0;
    uint face = min(uint(max(c.z, 0.0) + 0.5), 5u);
    if (face == 0u) return float3(1.0, -t, -s);
    if (face == 1u) return float3(-1.0, -t, s);
    if (face == 2u) return float3(s, 1.0, t);
    if (face == 3u) return float3(s, -1.0, -t);
    if (face == 4u) return float3(s, -t, 1.0);
    return float3(-s, -t, -1.0);
}

// Xenos multiplies the source by its blend factor with the min and max blend ops too, which Direct3D 12
// doesn't, so the pixel shader does (xe_min_max_factors), for the factors that only take the source (others
// count as one).
float4 XeSourceFactor(uint factor, float4 color)
{
    if (factor == 0u)
        return 0.0;
    if (factor == 4u)
        return color;
    if (factor == 5u)
        return 1.0 - color;
    if (factor == 6u)
        return color.aaaa;
    if (factor == 7u)
        return 1.0 - color.aaaa;
    return 1.0;
}

float4 XeMinMaxFactors(float4 color, uint factors)
{
    float3 rgb = (factors & 0x20u) != 0u ? color.rgb * XeSourceFactor(factors & 0x1Fu, color).rgb : color.rgb;
    float a = (factors & 0x2000u) != 0u ? color.a * XeSourceFactor((factors >> 8) & 0x1Fu, color).a : color.a;
    return float4(rgb, a);
}
)";

        const char kComponentNames[] = "xyzw";

        std::string Format(const char* format, ...)
        {
            char buffer[1024];
            va_list arguments;
            va_start(arguments, format);
            vsnprintf(buffer, sizeof(buffer), format, arguments);
            va_end(arguments);
            return buffer;
        }

        std::string FloatLiteral(double value)
        {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%.9g", value);
            std::string text = buffer;
            if (text.find_first_of(".eEn") == std::string::npos)
                text += ".0";
            return text;
        }

        std::string MaskSuffix(uint32_t mask)
        {
            std::string text = ".";
            for (uint32_t i = 0; i < 4; i++)
            {
                if (mask & (1u << i))
                    text += kComponentNames[i];
            }
            return text;
        }

        // DEBUG_SHOW=<hash prefix>:tf<N>, :r<N> or :i<N> (diagnostics): the pixel shaders whose hash starts with those hex
        // digits output, as their color 0, the result of their last texture fetch from fetch constant N, temp register N at
        // their end, or interpolator N as they receive it (alpha 1).
        struct DebugShow
        {
            std::string hash;
            char mode = 0; // 't', 'r' or 'i'
            uint32_t index = 0;
        };

        const DebugShow& DebugShowSetting()
        {
            static const DebugShow setting = [] {
                DebugShow show;
                const char* value = std::getenv(GAME_PREFIX "_DEBUG_SHOW");
                const char* colon = value ? strchr(value, ':') : nullptr;
                if (!colon || (colon[1] != 't' && colon[1] != 'r' && colon[1] != 'i'))
                    return show;
                show.hash.assign(value, colon);
                for (char& c : show.hash)
                    c = char(toupper(uint8_t(c)));
                show.mode = colon[1];
                show.index = uint32_t(atoi(colon + (show.mode == 't' ? 3 : 2)));
                return show;
            }();
            return setting;
        }

        class Translator
        {
        public:
            Translator(const ParsedShader& shader, const TranslationOptions& options)
                : m_shader(shader), m_options(options), m_vertex(shader.stage == ShaderStage::kVertex)
            {
                const DebugShow& show = DebugShowSetting();
                char hash[17];
                snprintf(hash, sizeof(hash), "%016llX", (unsigned long long)shader.hash);
                m_debugShow = !m_vertex && !show.hash.empty() && strncmp(hash, show.hash.c_str(), show.hash.size()) == 0;
            }

            HlslResult Translate();

        private:
            void Line(const std::string& text)
            {
                m_body.append(size_t(m_indent) * 4, ' ');
                m_body += text;
                m_body += '\n';
            }
            void Open(const std::string& text)
            {
                if (!text.empty())
                    Line(text);
                Line("{");
                m_indent++;
            }
            void Close()
            {
                m_indent--;
                Line("}");
            }
            // Something the translation can't express: noted in the source, and once in the result.
            void Unsupported(const std::string& what)
            {
                Line("// " + what);
                if (std::find(m_unsupported.begin(), m_unsupported.end(), what) == m_unsupported.end())
                    m_unsupported.push_back(what);
            }

            std::string Temp(uint32_t index, bool relative) const
            {
                return relative ? Format("r[(xe_aL + %u) & 63]", index) : Format("r[%u]", index);
            }
            std::string Register(const Operand& operand) const;
            std::string VectorOperandText(const Operand& operand) const;
            std::string ScalarComponentText(const Operand& operand, uint32_t component) const;
            std::string ExportTarget(uint32_t index) const;
            std::string ConditionText(const CfInstruction& cf) const;
            // Products of two operands (a * b, mad, dot products) go through XeMul when it differs from IEEE (Xenos products, or
            // their check), unless both are the same operand: no 0 times infinity then.
            bool PlainProducts(const std::string& a, const std::string& b) const
            {
                return (!m_options.xenosProducts && !m_options.debugNan) || a == b;
            }
            // For DEBUG_NAN's check, a factor of a componentwise product with 1 in the components the instruction doesn't write.
            std::string Written(const std::string& factor, uint32_t mask) const
            {
                if (!m_options.debugNan || mask == 0xF)
                    return factor;
                return Format("(%s ? %s : 1.0)", BoolMask(mask).c_str(), factor.c_str());
            }
            static std::string BoolMask(uint32_t mask)
            {
                return Format("bool4(%s, %s, %s, %s)", mask & 1 ? "true" : "false", mask & 2 ? "true" : "false", mask & 4 ? "true" : "false",
                    mask & 8 ? "true" : "false");
            }
            // DEBUG_NAN: notes a fetch's non-finite result in the components its destination takes (`flag`, kDebugNonFinite*).
            void CheckFetched(const std::string& value, uint32_t destinationSwizzle, uint32_t flag)
            {
                uint32_t taken = 0;
                for (uint32_t i = 0; i < 4; i++)
                {
                    const uint32_t select = (destinationSwizzle >> (i * 3)) & 7;
                    if (select < 4)
                        taken |= 1u << select;
                }
                if (m_options.debugNan && taken)
                {
                    Line(Format("if (any(XeNonFinite(%s) && %s))", value.c_str(), BoolMask(taken).c_str()));
                    Line(Format("    xe_debug_inputs |= %uu;", flag));
                }
            }

            void EmitAlu(const AluInstruction& alu);
            void EmitFetch(const FetchInstruction& fetch);
            void EmitVertexFetch(const FetchInstruction& fetch);
            void EmitTextureFetch(const FetchInstruction& fetch);
            void EmitExecBody(const CfInstruction& cf);
            void EmitCf(uint32_t index);
            void EmitStructured();
            void EmitForward();
            void EmitStateMachine();

            const ParsedShader& m_shader;
            const TranslationOptions m_options;
            const bool m_vertex;
            bool m_debugShow = false; // DEBUG_SHOW names this shader
            std::string m_body;
            int m_indent = 1;
            FetchInstruction m_fullVertexFetch{};
            bool m_haveFullVertexFetch = false;
            std::vector<std::string> m_unsupported;
        };

        std::string Translator::Register(const Operand& operand) const
        {
            if (operand.isTemp)
                return Temp(operand.index, operand.relative == Operand::Relative::kAL);
            switch (operand.relative)
            {
            case Operand::Relative::kA0: return Format("xe_c[(xe_a0 + %u) & 255]", operand.index);
            case Operand::Relative::kAL: return Format("xe_c[(xe_aL + %u) & 255]", operand.index);
            default: return Format("xe_c[%u]", operand.index);
            }
        }

        std::string Translator::VectorOperandText(const Operand& operand) const
        {
            std::string text = Register(operand);
            if (!(operand.components[0] == 0 && operand.components[1] == 1 && operand.components[2] == 2 && operand.components[3] == 3))
            {
                text += '.';
                for (uint32_t i = 0; i < 4; i++)
                    text += kComponentNames[operand.components[i]];
            }
            if (operand.absolute)
                text = "abs(" + text + ")";
            if (operand.negate)
                text = "(-" + text + ")";
            return text;
        }

        std::string Translator::ScalarComponentText(const Operand& operand, uint32_t component) const
        {
            std::string text = Register(operand) + '.' + kComponentNames[operand.components[component]];
            if (operand.absolute)
                text = "abs(" + text + ")";
            if (operand.negate)
                text = "(-" + text + ")";
            return text;
        }

        std::string Translator::ExportTarget(uint32_t index) const
        {
            if (m_vertex)
            {
                if (index == kExportPosition)
                    return "xe_position";
                if (index == kExportPointSize)
                    return "xe_point_size_export";
                if (index < 16)
                    return Format("xe_interpolators[%u]", index);
            }
            else
            {
                if (index < 4)
                    return Format("xe_color[%u]", index);
                if (index == kExportDepth)
                    return "xe_depth_export";
            }
            return "xe_unused_export"; // memory export and unknown targets
        }

        std::string Translator::ConditionText(const CfInstruction& cf) const
        {
            switch (cf.Opcode())
            {
            // The "predicate clean" variants are also conditional on a bool constant (DOA4's shaders use
            // them for if/else on light and shadow switches: b10, b10, !b10).
            case CfOpcode::kCondExec:
            case CfOpcode::kCondExecEnd:
            case CfOpcode::kCondExecPredClean:
            case CfOpcode::kCondExecPredCleanEnd:
                return Format("XeBoolConstant(%uu) == %s", cf.BoolAddress(), cf.Condition() ? "true" : "false");
            case CfOpcode::kCondExecPred:
            case CfOpcode::kCondExecPredEnd:
                return Format("xe_p0 == %s", cf.Condition() ? "true" : "false");
            case CfOpcode::kCondCall:
            case CfOpcode::kCondJmp:
                if (cf.IsUnconditional())
                    return "true";
                if (cf.IsPredicated())
                    return Format("xe_p0 == %s", cf.Condition() ? "true" : "false");
                return Format("XeBoolConstant(%uu) == %s", cf.BoolAddress(), cf.Condition() ? "true" : "false");
            default:
                return "true";
            }
        }

        void Translator::EmitAlu(const AluInstruction& alu)
        {
            const bool predicated = alu.IsPredicated();
            if (predicated)
                Open(Format("if (xe_p0 == %s)", alu.PredicateCondition() ? "true" : "false"));
            else
                Open("");

            // Vector operation (all operands are read before either result is written).
            const AluVectorOpcode vectorOp = alu.VectorOpcode();
            const uint32_t vectorCount = VectorOperandCount(vectorOp);
            std::string a, b, c;
            if (vectorCount >= 1)
                a = VectorOperandText(VectorOperand(alu, 1));
            if (vectorCount >= 2)
                b = VectorOperandText(VectorOperand(alu, 2));
            if (vectorCount >= 3)
                c = VectorOperandText(VectorOperand(alu, 3));

            // The components the vector result goes to: products elsewhere don't matter.
            const uint32_t vectorMask = alu.IsExport() ? alu.VectorResultWriteMask() : alu.VectorWriteMask();
            const bool plainProducts = PlainProducts(a, b) || !vectorMask;
            std::string vector;
            std::string vectorSideEffect;
            switch (vectorOp)
            {
            case AluVectorOpcode::kAdd: vector = a + " + " + b; break;
            case AluVectorOpcode::kMul: vector = plainProducts ? a + " * " + b : "XeMul(" + Written(a, vectorMask) + ", " + b + ")"; break;
            case AluVectorOpcode::kMax: vector = "max(" + a + ", " + b + ")"; break;
            case AluVectorOpcode::kMin: vector = "min(" + a + ", " + b + ")"; break;
            case AluVectorOpcode::kSeq: vector = "float4(" + a + " == " + b + ")"; break;
            case AluVectorOpcode::kSgt: vector = "float4(" + a + " > " + b + ")"; break;
            case AluVectorOpcode::kSge: vector = "float4(" + a + " >= " + b + ")"; break;
            case AluVectorOpcode::kSne: vector = "float4(" + a + " != " + b + ")"; break;
            case AluVectorOpcode::kFrc: vector = "frac(" + a + ")"; break;
            case AluVectorOpcode::kTrunc: vector = "trunc(" + a + ")"; break;
            case AluVectorOpcode::kFloor: vector = "floor(" + a + ")"; break;
            case AluVectorOpcode::kMad:
                vector = plainProducts ? "mad(" + a + ", " + b + ", " + c + ")" : "XeMul(" + Written(a, vectorMask) + ", " + b + ") + " + c;
                break;
            case AluVectorOpcode::kCndEq: vector = "(" + a + " == 0.0) ? " + b + " : " + c; break;
            case AluVectorOpcode::kCndGe: vector = "(" + a + " >= 0.0) ? " + b + " : " + c; break;
            case AluVectorOpcode::kCndGt: vector = "(" + a + " > 0.0) ? " + b + " : " + c; break;
            case AluVectorOpcode::kDp4:
                vector = plainProducts ? "dot(" + a + ", " + b + ").xxxx" : "XeDot(" + a + ", " + b + ").xxxx";
                break;
            case AluVectorOpcode::kDp3:
                vector = plainProducts ? "dot((" + a + ").xyz, (" + b + ").xyz).xxxx"
                                       : "XeDot(float4((" + a + ").xyz, 0.0), float4((" + b + ").xyz, 0.0)).xxxx";
                break;
            case AluVectorOpcode::kDp2Add:
                vector = plainProducts ? "(dot((" + a + ").xy, (" + b + ").xy) + (" + c + ").x).xxxx"
                                       : "(XeDot(float4((" + a + ").xy, 0.0, 0.0), float4((" + b + ").xy, 0.0, 0.0)) + (" + c + ").x).xxxx";
                break;
            case AluVectorOpcode::kCube: vector = "XeCube(" + a + ", " + b + ")"; break;
            case AluVectorOpcode::kMax4: vector = "max(max((" + a + ").x, (" + a + ").y), max((" + a + ").z, (" + a + ").w)).xxxx"; break;
            case AluVectorOpcode::kSetpEqPush:
            case AluVectorOpcode::kSetpNePush:
            case AluVectorOpcode::kSetpGtPush:
            case AluVectorOpcode::kSetpGePush:
            {
                static const char* kCompare[] = { "==", "!=", ">", ">=" };
                const char* compare = kCompare[uint32_t(vectorOp) - uint32_t(AluVectorOpcode::kSetpEqPush)];
                Line("float4 xe_va = " + a + ", xe_vb = " + b + ";");
                vector = Format("((xe_va.x == 0.0 && xe_vb.x %s 0.0) ? 0.0 : xe_va.x + 1.0).xxxx", compare);
                vectorSideEffect = Format("xe_p0 = xe_va.w == 0.0 && xe_vb.w %s 0.0;", compare);
                break;
            }
            case AluVectorOpcode::kKillEq:
            case AluVectorOpcode::kKillGt:
            case AluVectorOpcode::kKillGe:
            case AluVectorOpcode::kKillNe:
            {
                static const char* kCompare[] = { "==", ">", ">=", "!=" };
                Line(Format("bool xe_vkill = any(%s %s %s);", a.c_str(), kCompare[uint32_t(vectorOp) - uint32_t(AluVectorOpcode::kKillEq)], b.c_str()));
                vector = "float4(xe_vkill.xxxx)";
                vectorSideEffect = m_vertex ? "" : "if (xe_vkill) discard;";
                break;
            }
            case AluVectorOpcode::kDst:
                Line("float4 xe_va = " + a + ", xe_vb = " + b + ";");
                vector = plainProducts || !(vectorMask & 2) ? "float4(1.0, xe_va.y * xe_vb.y, xe_va.z, xe_vb.w)"
                                                            : "float4(1.0, XeMul(xe_va.y, xe_vb.y), xe_va.z, xe_vb.w)";
                break;
            case AluVectorOpcode::kMaxA:
                Line("float4 xe_va = " + a + ", xe_vb = " + b + ";");
                vector = "max(xe_va, xe_vb)";
                vectorSideEffect = "xe_a0 = XeAddressFromFloat(floor(xe_va.w + 0.5));";
                break;
            default:
                vector = "float4(0.0, 0.0, 0.0, 0.0)";
                Unsupported(Format("unknown vector opcode %u", uint32_t(vectorOp)));
                break;
            }
            Line("float4 xe_v = " + vector + ";");
            if (alu.VectorClamp())
                Line("xe_v = saturate(xe_v);");

            // Scalar operation.
            const AluScalarOpcode scalarOp = alu.ScalarOpcode();
            Operand operands[2];
            const uint32_t operandCount = ScalarOperands(alu, operands);
            std::string sa, sb;
            if (operandCount == 2)
            {
                sa = ScalarComponentText(operands[0], 0);
                sb = ScalarComponentText(operands[1], 0);
            }
            else if (operandCount == 1)
            {
                sa = ScalarComponentText(operands[0], 0);
                if (operands[0].componentCount > 1)
                    sb = ScalarComponentText(operands[0], 1);
            }

            std::string scalar;
            std::string scalarSideEffect;
            switch (scalarOp)
            {
            case AluScalarOpcode::kAdds: scalar = sa + " + " + sb; break;
            case AluScalarOpcode::kAddsPrev: scalar = sa + " + xe_ps"; break;
            case AluScalarOpcode::kMuls: scalar = PlainProducts(sa, sb) ? sa + " * " + sb : "XeMul(" + sa + ", " + sb + ")"; break;
            case AluScalarOpcode::kMulsPrev: scalar = PlainProducts(sa, "xe_ps") ? sa + " * xe_ps" : "XeMul(" + sa + ", xe_ps)"; break;
            case AluScalarOpcode::kMulsPrev2: scalar = "XeMulsPrev2(" + sa + ", " + sb + ", xe_ps)"; break;
            case AluScalarOpcode::kMaxs: scalar = "max(" + sa + ", " + sb + ")"; break;
            case AluScalarOpcode::kMins: scalar = "min(" + sa + ", " + sb + ")"; break;
            case AluScalarOpcode::kSeqs: scalar = "float(" + sa + " == 0.0)"; break;
            case AluScalarOpcode::kSgts: scalar = "float(" + sa + " > 0.0)"; break;
            case AluScalarOpcode::kSges: scalar = "float(" + sa + " >= 0.0)"; break;
            case AluScalarOpcode::kSnes: scalar = "float(" + sa + " != 0.0)"; break;
            case AluScalarOpcode::kFrcs: scalar = "frac(" + sa + ")"; break;
            case AluScalarOpcode::kTruncs: scalar = "trunc(" + sa + ")"; break;
            case AluScalarOpcode::kFloors: scalar = "floor(" + sa + ")"; break;
            case AluScalarOpcode::kExp: scalar = "exp2(" + sa + ")"; break;
            case AluScalarOpcode::kLogc: scalar = "XeLogClamp(" + sa + ")"; break;
            case AluScalarOpcode::kLog: scalar = "log2(" + sa + ")"; break;
            case AluScalarOpcode::kRcpc: scalar = "XeClampMax(1.0 / " + sa + ")"; break;
            case AluScalarOpcode::kRcpf: scalar = "XeFlushInfinity(1.0 / " + sa + ")"; break;
            case AluScalarOpcode::kRcp: scalar = "1.0 / " + sa; break;
            case AluScalarOpcode::kRsqc: scalar = "XeClampMax(rsqrt(" + sa + "))"; break;
            case AluScalarOpcode::kRsqf: scalar = "XeFlushInfinity(rsqrt(" + sa + "))"; break;
            case AluScalarOpcode::kRsq: scalar = "rsqrt(" + sa + ")"; break;
            case AluScalarOpcode::kMaxAs:
            case AluScalarOpcode::kMaxAsf:
                Line("float xe_sa = " + sa + ", xe_sb = " + sb + ";");
                scalar = "max(xe_sa, xe_sb)";
                scalarSideEffect = scalarOp == AluScalarOpcode::kMaxAs ? "xe_a0 = XeAddressFromFloat(floor(xe_sa + 0.5));"
                                                                       : "xe_a0 = XeAddressFromFloat(floor(xe_sa));";
                break;
            case AluScalarOpcode::kSubs: scalar = sa + " - " + sb; break;
            case AluScalarOpcode::kSubsPrev: scalar = sa + " - xe_ps"; break;
            case AluScalarOpcode::kSetpEq:
            case AluScalarOpcode::kSetpNe:
            case AluScalarOpcode::kSetpGt:
            case AluScalarOpcode::kSetpGe:
            {
                static const char* kCompare[] = { "==", "!=", ">", ">=" };
                Line(Format("bool xe_sp = %s %s 0.0;", sa.c_str(), kCompare[uint32_t(scalarOp) - uint32_t(AluScalarOpcode::kSetpEq)]));
                scalar = "xe_sp ? 0.0 : 1.0";
                scalarSideEffect = "xe_p0 = xe_sp;";
                break;
            }
            case AluScalarOpcode::kSetpInv:
                Line("float xe_sa = " + sa + ";");
                scalar = "xe_sa == 1.0 ? 0.0 : (xe_sa == 0.0 ? 1.0 : xe_sa)";
                scalarSideEffect = "xe_p0 = xe_sa == 1.0;";
                break;
            case AluScalarOpcode::kSetpPop:
                Line("float xe_sa = " + sa + " - 1.0;");
                scalar = "xe_sa <= 0.0 ? 0.0 : xe_sa";
                scalarSideEffect = "xe_p0 = xe_sa <= 0.0;";
                break;
            case AluScalarOpcode::kSetpClr:
                scalar = "XE_FLT_MAX";
                scalarSideEffect = "xe_p0 = false;";
                break;
            case AluScalarOpcode::kSetpRstr:
                Line("float xe_sa = " + sa + ";");
                scalar = "xe_sa";
                scalarSideEffect = "xe_p0 = xe_sa == 0.0;";
                break;
            case AluScalarOpcode::kKillsEq:
            case AluScalarOpcode::kKillsGt:
            case AluScalarOpcode::kKillsGe:
            case AluScalarOpcode::kKillsNe:
            case AluScalarOpcode::kKillsOne:
            {
                static const char* kCompare[] = { "== 0.0", "> 0.0", ">= 0.0", "!= 0.0", "== 1.0" };
                Line(Format("bool xe_skill = %s %s;", sa.c_str(), kCompare[uint32_t(scalarOp) - uint32_t(AluScalarOpcode::kKillsEq)]));
                scalar = "xe_skill ? 1.0 : 0.0";
                scalarSideEffect = m_vertex ? "" : "if (xe_skill) discard;";
                break;
            }
            case AluScalarOpcode::kSqrt: scalar = "sqrt(" + sa + ")"; break;
            case AluScalarOpcode::kMulsc0:
            case AluScalarOpcode::kMulsc1: scalar = PlainProducts(sa, sb) ? sa + " * " + sb : "XeMul(" + sa + ", " + sb + ")"; break;
            case AluScalarOpcode::kAddsc0:
            case AluScalarOpcode::kAddsc1: scalar = sa + " + " + sb; break;
            case AluScalarOpcode::kSubsc0:
            case AluScalarOpcode::kSubsc1: scalar = sa + " - " + sb; break;
            case AluScalarOpcode::kSin: scalar = "sin(" + sa + ")"; break;
            case AluScalarOpcode::kCos: scalar = "cos(" + sa + ")"; break;
            case AluScalarOpcode::kRetainPrev: scalar = "xe_ps"; break;
            default:
                scalar = "0.0";
                Unsupported(Format("unknown scalar opcode %u", uint32_t(scalarOp)));
                break;
            }
            Line("float xe_s = " + scalar + ";");
            if (alu.ScalarClamp())
                Line("xe_s = saturate(xe_s);");

            // Side effects, then results.
            if (!vectorSideEffect.empty())
                Line(vectorSideEffect);
            if (!scalarSideEffect.empty())
                Line(scalarSideEffect);
            Line("xe_ps = xe_s;");

            if (alu.IsExport())
            {
                const std::string target = ExportTarget(alu.VectorDest());
                const uint32_t vectorMask = alu.VectorResultWriteMask();
                const uint32_t scalarMask = alu.ScalarResultWriteMask();
                const uint32_t const0 = alu.Constant0WriteMask(), const1 = alu.Constant1WriteMask();
                for (uint32_t i = 0; i < 4; i++)
                {
                    const char component = kComponentNames[i];
                    if (const1 & (1u << i))
                        Line(Format("%s.%c = 1.0;", target.c_str(), component));
                    else if (vectorMask & (1u << i))
                        Line(Format("%s.%c = xe_v.%c;", target.c_str(), component, component));
                    else if (scalarMask & (1u << i))
                        Line(Format("%s.%c = xe_s;", target.c_str(), component));
                    else if (const0 & (1u << i))
                        Line(Format("%s.%c = 0.0;", target.c_str(), component));
                }
            }
            else
            {
                if (alu.VectorWriteMask())
                {
                    const std::string mask = MaskSuffix(alu.VectorWriteMask());
                    Line(Temp(alu.VectorDest(), alu.VectorDestRelative()) + mask + " = xe_v" + mask + ";");
                }
                if (alu.ScalarWriteMask())
                    Line(Temp(alu.ScalarDest(), alu.ScalarDestRelative()) + MaskSuffix(alu.ScalarWriteMask()) + " = xe_s;");
            }
            Close();
        }

        // Component extraction for a vertex format: dword index, bit offset, bit count; 32-bit float
        // and half formats are handled separately.
        struct VertexComponent
        {
            uint32_t word, shift, bits;
        };

        uint32_t VertexFormatComponents(uint32_t format, VertexComponent out[4], bool& isFloat, bool& isHalf)
        {
            isFloat = isHalf = false;
            switch (format)
            {
            case kVertex_8_8_8_8:
                out[0] = { 0, 0, 8 }; out[1] = { 0, 8, 8 }; out[2] = { 0, 16, 8 }; out[3] = { 0, 24, 8 };
                return 4;
            case kVertex_2_10_10_10:
                out[0] = { 0, 0, 10 }; out[1] = { 0, 10, 10 }; out[2] = { 0, 20, 10 }; out[3] = { 0, 30, 2 };
                return 4;
            case kVertex_10_11_11:
                out[0] = { 0, 0, 11 }; out[1] = { 0, 11, 11 }; out[2] = { 0, 22, 10 };
                return 3;
            case kVertex_11_11_10:
                out[0] = { 0, 0, 10 }; out[1] = { 0, 10, 11 }; out[2] = { 0, 21, 11 };
                return 3;
            case kVertex_16_16:
                out[0] = { 0, 0, 16 }; out[1] = { 0, 16, 16 };
                return 2;
            case kVertex_16_16_16_16:
                out[0] = { 0, 0, 16 }; out[1] = { 0, 16, 16 }; out[2] = { 1, 0, 16 }; out[3] = { 1, 16, 16 };
                return 4;
            case kVertex_16_16_FLOAT:
                isHalf = true;
                out[0] = { 0, 0, 16 }; out[1] = { 0, 16, 16 };
                return 2;
            case kVertex_16_16_16_16_FLOAT:
                isHalf = true;
                out[0] = { 0, 0, 16 }; out[1] = { 0, 16, 16 }; out[2] = { 1, 0, 16 }; out[3] = { 1, 16, 16 };
                return 4;
            case kVertex_32:
            case kVertex_32_32:
            case kVertex_32_32_32_32:
            case kVertex_32_FLOAT:
            case kVertex_32_32_FLOAT:
            case kVertex_32_32_32_FLOAT:
            case kVertex_32_32_32_32_FLOAT:
            {
                isFloat = format >= kVertex_32_FLOAT;
                const uint32_t count = format == kVertex_32 || format == kVertex_32_FLOAT ? 1
                    : format == kVertex_32_32 || format == kVertex_32_32_FLOAT             ? 2
                    : format == kVertex_32_32_32_FLOAT                                     ? 3
                                                                                           : 4;
                for (uint32_t i = 0; i < count; i++)
                    out[i] = { i, 0, 32 };
                return count;
            }
            default:
                return 0;
            }
        }

        void Translator::EmitVertexFetch(const FetchInstruction& fetch)
        {
            // Mini fetches reuse the index, fetch constant and stride of the last full fetch.
            if (!fetch.VertexMiniFetch())
            {
                m_fullVertexFetch = fetch;
                m_haveFullVertexFetch = true;
            }
            else if (!m_haveFullVertexFetch)
            {
                Line("// mini vertex fetch without a preceding full fetch");
                return;
            }
            const FetchInstruction& full = m_fullVertexFetch;

            VertexComponent components[4];
            bool isFloat, isHalf;
            const uint32_t count = VertexFormatComponents(fetch.VertexFormat(), components, isFloat, isHalf);
            if (!count)
            {
                Unsupported(Format("unsupported vertex format %u", fetch.VertexFormat()));
                return;
            }
            uint32_t words = 0;
            for (uint32_t i = 0; i < count; i++)
                words = std::max(words, components[i].word + 1);

            const bool predicated = fetch.IsPredicated();
            Open(predicated ? Format("if (xe_p0 == %s)", fetch.PredicateCondition() ? "true" : "false") : std::string());

            const uint32_t constant = full.VertexConstantIndex();
            Line(Format("uint2 xe_vf = xe_fetch_constants[%u].%s;", constant / 2, (constant & 1) ? "zw" : "xy"));
            // The index is read once, by the full fetch: its mini fetches use it even when a fetch between them wrote the
            // index register (DOAX2's ground shaders fetch their last texture coordinates into it, so the lightmap's came
            // from the wrong vertex and the beaches came out white).
            if (!fetch.VertexMiniFetch())
            {
                const std::string index = Temp(full.SrcReg(), full.SrcRelative()) + '.' + kComponentNames[full.VertexSrcComponent()];
                Line(Format("xe_vfetch_index = int(%s);", full.VertexIndexRounded() ? ("floor(" + index + " + 0.5)").c_str() : ("floor(" + index + ")").c_str()));
            }
            Line(Format("uint xe_address = (xe_vf.x & 0xFFFFFFFCu) + uint(xe_vfetch_index * %u + %d) * 4u;", full.VertexStride(), fetch.VertexOffset()));
            static const char* kLoads[] = { "", "uint4(xe_shared_memory.Load(xe_address), 0u, 0u, 0u)",
                "uint4(xe_shared_memory.Load2(xe_address), 0u, 0u)", "uint4(xe_shared_memory.Load3(xe_address), 0u)",
                "xe_shared_memory.Load4(xe_address)" };
            Line(Format("uint4 xe_raw = XeEndianSwap(%s, xe_vf.y & 3u);", kLoads[words]));

            std::string values[4] = { "0.0", "0.0", "0.0", "1.0" };
            const double expScale = std::ldexp(1.0, fetch.VertexExpAdjust());
            for (uint32_t i = 0; i < count; i++)
            {
                const VertexComponent& component = components[i];
                const std::string word = Format("xe_raw.%c", kComponentNames[component.word]);
                if (isFloat)
                {
                    values[i] = "asfloat(" + word + ")";
                    continue;
                }
                if (isHalf)
                {
                    values[i] = Format("f16tof32((%s >> %uu) & 0xFFFFu)", word.c_str(), component.shift);
                    continue;
                }
                std::string value;
                if (fetch.VertexSigned())
                {
                    value = component.bits == 32 ? "float(asint(" + word + "))"
                                                 : Format("float(asint(%s << %uu) >> %u)", word.c_str(), 32 - component.shift - component.bits, 32 - component.bits);
                    if (!fetch.VertexInteger())
                        value = "max(" + value + " * " + FloatLiteral(1.0 / (std::ldexp(1.0, component.bits - 1) - 1.0)) + ", -1.0)";
                }
                else
                {
                    value = component.bits == 32 ? "float(" + word + ")"
                                                 : Format("float((%s >> %uu) & 0x%Xu)", word.c_str(), component.shift, (1u << component.bits) - 1);
                    if (!fetch.VertexInteger())
                        value += " * " + FloatLiteral(1.0 / (std::ldexp(1.0, component.bits) - 1.0));
                }
                if (expScale != 1.0)
                    value = "(" + value + ") * " + FloatLiteral(expScale);
                values[i] = value;
            }
            Line(Format("float4 xe_value = float4(%s, %s, %s, %s);", values[0].c_str(), values[1].c_str(), values[2].c_str(), values[3].c_str()));
            CheckFetched("xe_value", fetch.DstSwizzle(), kDebugNonFiniteVertexData);

            const std::string destination = Temp(fetch.DstReg(), fetch.DstRelative());
            for (uint32_t i = 0; i < 4; i++)
            {
                const uint32_t select = (fetch.DstSwizzle() >> (i * 3)) & 7;
                if (select == 7)
                    continue;
                const std::string source = select < 4 ? Format("xe_value.%c", kComponentNames[select]) : select == 5 ? "1.0" : "0.0";
                Line(Format("%s.%c = %s;", destination.c_str(), kComponentNames[i], source.c_str()));
            }
            Close();
        }

        void Translator::EmitTextureFetch(const FetchInstruction& fetch)
        {
            const bool predicated = fetch.IsPredicated();
            Open(predicated ? Format("if (xe_p0 == %s)", fetch.PredicateCondition() ? "true" : "false") : std::string());

            const std::string source = Temp(fetch.SrcReg(), fetch.SrcRelative());
            const uint32_t swizzle = fetch.TextureSrcSwizzle();
            const std::string coords = Format("%s.%c%c%c", source.c_str(), kComponentNames[swizzle & 3], kComponentNames[(swizzle >> 2) & 3],
                kComponentNames[(swizzle >> 4) & 3]);
            const FetchOpcode opcode = fetch.Opcode();

            if (opcode == FetchOpcode::kSetTextureLod)
            {
                Line("xe_texture_lod = " + coords + ".x;");
                Close();
                return;
            }
            if (opcode == FetchOpcode::kSetTextureGradientsHorz || opcode == FetchOpcode::kSetTextureGradientsVert)
            {
                Line(std::string(opcode == FetchOpcode::kSetTextureGradientsHorz ? "xe_gradient_h" : "xe_gradient_v") + " = " + coords + ";");
                Close();
                return;
            }

            const uint32_t texture = m_shader.TextureBindingIndex(fetch);
            const uint32_t sampler = m_shader.SamplerBindingIndex(fetch);
            const uint32_t dimension = fetch.Dimension();
            const std::string textureName = Format("xe_texture%u", texture);
            const std::string samplerName = Format("xe_sampler%u", sampler);

            // Coordinates for the host texture type.
            Line("float3 xe_coords = " + coords + ";");
            if (dimension == 3)
                Line("float3 xe_uv = XeCubeDirection(xe_coords);");
            else if (dimension == 2)
                Line("float3 xe_uv = xe_coords;");
            else if (dimension == 1)
                Line("float2 xe_uv = xe_coords.xy;");
            else
                Line("float2 xe_uv = float2(xe_coords.x, 0.5);");

            const bool unnormalized = fetch.TextureUnnormalized() && dimension != 3;
            const bool offset = fetch.OffsetX() != 0.0f || fetch.OffsetY() != 0.0f || fetch.OffsetZ() != 0.0f;
            if ((unnormalized || offset) && dimension != 3)
            {
                if (dimension == 2)
                {
                    Line("uint3 xe_size;");
                    Line(textureName + ".GetDimensions(xe_size.x, xe_size.y, xe_size.z);");
                    if (unnormalized)
                        Line("xe_uv /= float3(xe_size);");
                    if (offset)
                        Line(Format("xe_uv += float3(%s, %s, %s) / float3(xe_size);", FloatLiteral(fetch.OffsetX()).c_str(),
                            FloatLiteral(fetch.OffsetY()).c_str(), FloatLiteral(fetch.OffsetZ()).c_str()));
                }
                else
                {
                    Line("uint2 xe_size;");
                    Line(textureName + ".GetDimensions(xe_size.x, xe_size.y);");
                    if (unnormalized)
                        Line(dimension == 0 ? "xe_uv.x /= float(xe_size.x);" : "xe_uv /= float2(xe_size);");
                    if (offset)
                        Line(Format("xe_uv += float2(%s, %s) / float2(xe_size);", FloatLiteral(fetch.OffsetX()).c_str(), FloatLiteral(fetch.OffsetY()).c_str()));
                }
            }

            std::string result;
            if (opcode == FetchOpcode::kTextureFetch)
            {
                const std::string bias = FloatLiteral(fetch.LodBias());
                if (fetch.UseRegisterLod())
                    result = Format("%s.SampleLevel(%s, xe_uv, xe_texture_lod + %s)", textureName.c_str(), samplerName.c_str(), bias.c_str());
                else if (m_vertex || !fetch.UseComputedLod())
                    result = Format("%s.SampleLevel(%s, xe_uv, %s)", textureName.c_str(), samplerName.c_str(), bias.c_str());
                else if (fetch.UseRegisterGradients())
                    result = Format("%s.SampleGrad(%s, xe_uv, xe_gradient_h.%s, xe_gradient_v.%s)", textureName.c_str(), samplerName.c_str(),
                        dimension >= 2 ? "xyz" : "xy", dimension >= 2 ? "xyz" : "xy");
                else if (fetch.LodBias() != 0.0f)
                    result = Format("%s.SampleBias(%s, xe_uv, %s)", textureName.c_str(), samplerName.c_str(), bias.c_str());
                else
                    result = Format("%s.Sample(%s, xe_uv)", textureName.c_str(), samplerName.c_str());
            }
            else if (opcode == FetchOpcode::kGetTextureComputedLod && !m_vertex)
            {
                result = Format("%s.CalculateLevelOfDetail(%s, xe_uv).xxxx", textureName.c_str(), samplerName.c_str());
            }
            else if (opcode == FetchOpcode::kGetTextureWeights && dimension != 3)
            {
                Line("uint3 xe_weight_size = uint3(1u, 1u, 1u);");
                if (dimension == 2)
                    Line(textureName + ".GetDimensions(xe_weight_size.x, xe_weight_size.y, xe_weight_size.z);");
                else
                    Line(textureName + ".GetDimensions(xe_weight_size.x, xe_weight_size.y);");
                result = "float4(frac(xe_uv.xy * float2(xe_weight_size) - 0.5), 0.0, 0.0)";
            }
            else
            {
                result = "float4(0.0, 0.0, 0.0, 0.0)";
                Unsupported(Format("unsupported texture fetch opcode %u", uint32_t(opcode)));
            }
            Line("float4 xe_texel = " + result + ";");
            CheckFetched("xe_texel", fetch.DstSwizzle(), kDebugNonFiniteTexels);
            if (m_debugShow && DebugShowSetting().mode == 't' && fetch.TextureConstantIndex() == DebugShowSetting().index)
                Line("xe_debug_show = xe_texel;");

            const std::string destination = Temp(fetch.DstReg(), fetch.DstRelative());
            for (uint32_t i = 0; i < 4; i++)
            {
                const uint32_t select = (fetch.DstSwizzle() >> (i * 3)) & 7;
                if (select == 7)
                    continue;
                const std::string value = select < 4 ? Format("xe_texel.%c", kComponentNames[select]) : select == 5 ? "1.0" : "0.0";
                Line(Format("%s.%c = %s;", destination.c_str(), kComponentNames[i], value.c_str()));
            }
            Close();
        }

        void Translator::EmitFetch(const FetchInstruction& fetch)
        {
            if (fetch.Opcode() == FetchOpcode::kVertexFetch)
                EmitVertexFetch(fetch);
            else
                EmitTextureFetch(fetch);
        }

        void Translator::EmitExecBody(const CfInstruction& cf)
        {
            uint32_t sequence = cf.ExecSequence();
            for (uint32_t i = 0; i < cf.ExecCount(); i++, sequence >>= 2)
            {
                const uint32_t address = cf.ExecAddress() + i;
                if ((address + 1) * 3 > m_shader.ucode.size())
                    break;
                const uint32_t* words = m_shader.Instruction(address);
                if (sequence & 1)
                {
                    EmitFetch(FetchInstruction{ { words[0], words[1], words[2] } });
                }
                else
                {
                    if (m_options.debugNan)
                        Line(Format("xe_debug_instruction = %uu;", address));
                    EmitAlu(AluInstruction{ { words[0], words[1], words[2] } });
                }
            }
        }

        // Shaders without loops, calls or jumps: a run of (conditional) exec blocks; an executed end
        // leaves the do-while.
        void Translator::EmitStructured()
        {
            Open("do");
            for (const CfInstruction& cf : m_shader.cf)
            {
                if (!cf.IsExec())
                    continue;
                const CfOpcode op = cf.Opcode();
                const bool conditional = op != CfOpcode::kExec && op != CfOpcode::kExecEnd;
                if (conditional)
                    Open("if (" + ConditionText(cf) + ")");
                EmitExecBody(cf);
                if (cf.IsEnd())
                    Line("break;");
                if (conditional)
                    Close();
            }
            Close();
            Line("while (false);");
        }

        // One CF instruction: what it does, then where the program counter goes (the next instruction, a target, or 0xFFFF
        // at the end).
        void Translator::EmitCf(uint32_t index)
        {
            const uint32_t count = uint32_t(m_shader.cf.size());
            const CfInstruction& cf = m_shader.cf[index];
            const std::string next = Format("%u", index + 1 < count ? index + 1 : 0xFFFFu);
            switch (cf.Opcode())
            {
            case CfOpcode::kExec:
            case CfOpcode::kExecEnd:
                EmitExecBody(cf);
                Line("xe_pc = " + (cf.IsEnd() ? std::string("0xFFFF") : next) + ";");
                break;
            case CfOpcode::kCondExec:
            case CfOpcode::kCondExecEnd:
            case CfOpcode::kCondExecPred:
            case CfOpcode::kCondExecPredEnd:
            case CfOpcode::kCondExecPredClean:
            case CfOpcode::kCondExecPredCleanEnd:
                Line("xe_pc = " + next + ";");
                Open("if (" + ConditionText(cf) + ")");
                EmitExecBody(cf);
                if (cf.IsEnd())
                    Line("xe_pc = 0xFFFF;");
                Close();
                break;
            case CfOpcode::kLoopStart:
            {
                Line("xe_loop_stack_count[xe_loop_depth & 3u] = xe_loop_count;");
                Line("xe_loop_stack_aL[xe_loop_depth & 3u] = xe_aL;");
                Line("xe_loop_depth++;");
                Line(Format("uint xe_loop_constant = XeLoopConstant(%uu);", cf.LoopId()));
                Line("xe_loop_count = xe_loop_constant & 0xFFu;");
                if (!cf.IsRepeat())
                    Line("xe_aL = int((xe_loop_constant >> 8u) & 0xFFu);");
                Open("if (xe_loop_count == 0u)");
                Line("xe_loop_depth--;");
                Line("xe_loop_count = xe_loop_stack_count[xe_loop_depth & 3u];");
                Line("xe_aL = xe_loop_stack_aL[xe_loop_depth & 3u];");
                Line(Format("xe_pc = %u;", cf.TargetAddress()));
                Close();
                Open("else");
                Line("xe_pc = " + next + ";");
                Close();
                break;
            }
            case CfOpcode::kLoopEnd:
            {
                Line(Format("uint xe_loop_constant = XeLoopConstant(%uu);", cf.LoopId()));
                Line("xe_loop_count--;");
                Line("xe_aL += int(xe_loop_constant << 8u) >> 24;");
                const std::string breakCondition =
                    cf.IsPredicatedBreak() ? Format(" && xe_p0 != %s", cf.Condition() ? "true" : "false") : std::string();
                Open("if (xe_loop_count != 0u" + breakCondition + ")");
                Line(Format("xe_pc = %u;", cf.TargetAddress()));
                Close();
                Open("else");
                Line("xe_loop_depth--;");
                Line("xe_loop_count = xe_loop_stack_count[xe_loop_depth & 3u];");
                Line("xe_aL = xe_loop_stack_aL[xe_loop_depth & 3u];");
                Line("xe_pc = " + next + ";");
                Close();
                break;
            }
            case CfOpcode::kCondCall:
                Open("if (" + ConditionText(cf) + ")");
                Line("xe_call_stack[xe_call_depth & 3u] = " + next + ";");
                Line("xe_call_depth++;");
                Line(Format("xe_pc = %u;", cf.TargetAddress()));
                Close();
                Open("else");
                Line("xe_pc = " + next + ";");
                Close();
                break;
            case CfOpcode::kReturn:
                Open("if (xe_call_depth != 0u)");
                Line("xe_call_depth--;");
                Line("xe_pc = xe_call_stack[xe_call_depth & 3u];");
                Close();
                Open("else");
                Line("xe_pc = 0xFFFF;");
                Close();
                break;
            case CfOpcode::kCondJmp:
                Line(Format("xe_pc = (%s) ? %u : %s;", ConditionText(cf).c_str(), cf.TargetAddress(), next.c_str()));
                break;
            default:
                Line("xe_pc = " + next + ";");
                break;
            }
        }

        // Shaders whose jumps never close a cycle, without loops or calls (if/else trees, as over bool constants): the CF
        // instructions in an order where each comes after all that can lead to it (ParsedShader::forwardOrder: their own
        // order when the jumps all go forward, as in DOAX2's), each run if the program counter is on it, so that a jump
        // skips those before its target. FXC compiles the state machine of DOAX2's large vertex shaders of this kind
        // (100-150 KB of HLSL) in minutes, this in seconds.
        void Translator::EmitForward()
        {
            Line("int xe_pc = 0;");
            for (uint32_t index : m_shader.forwardOrder)
            {
                Open(Format("if (xe_pc == %u)", index));
                EmitCf(index);
                Close();
            }
        }

        // General control flow: a loop over a switch on the CF instruction index.
        void Translator::EmitStateMachine()
        {
            Line("int xe_pc = 0;");
            Line("uint xe_loop_count = 0u;");
            Line("uint xe_loop_depth = 0u;");
            Line("uint xe_loop_stack_count[4] = { 0u, 0u, 0u, 0u };");
            Line("int xe_loop_stack_aL[4] = { 0, 0, 0, 0 };");
            Line("uint xe_call_depth = 0u;");
            Line("int xe_call_stack[4] = { 0, 0, 0, 0 };");
            Line("[loop]");
            Open("while (xe_pc != 0xFFFF)");
            // The program counter is an int: with a uint selector and uint case labels, FXC fails with
            // "internal error: l-value expected" on some shaders.
            Line("[forcecase]");
            Open("switch (xe_pc)");
            for (uint32_t index = 0; index < uint32_t(m_shader.cf.size()); index++)
            {
                Open(Format("case %u:", index));
                EmitCf(index);
                Line("break;");
                Close();
            }
            Open("default:");
            Line("xe_pc = 0xFFFF;");
            Line("break;");
            Close();
            Close();
            Close();
        }

        HlslResult Translator::Translate()
        {
            if (m_shader.structuredControlFlow)
                EmitStructured();
            else if (m_shader.forwardControlFlow)
                EmitForward();
            else
                EmitStateMachine();

            std::string source;
            if (m_options.xenosProducts)
                source += "#define XE_XENOS_PRODUCTS 1\n";
            if (m_options.debugNan)
                source += "#define XE_DEBUG_NAN 1\n";
            source += kHlslCommon;
            // DEBUG_NAN: the shader's hash, for its counts.
            const std::string hash = Format("uint2(0x%08Xu, 0x%08Xu)", uint32_t(m_shader.hash), uint32_t(m_shader.hash >> 32));
            static const char* kComponents = "xyzw";
            for (uint32_t i = 0; i < m_shader.textures.size(); i++)
            {
                static const char* kArrays[] = { "xe_textures_2d", "xe_textures_2d", "xe_textures_3d", "xe_textures_cube" };
                source += Format("#define xe_texture%u %s[xe_texture_indices[%u].%c]\n", i, kArrays[m_shader.textures[i].dimension], i / 4,
                    kComponents[i % 4]);
            }
            for (uint32_t i = 0; i < m_shader.samplers.size(); i++)
                source += Format("#define xe_sampler%u xe_samplers[xe_sampler_indices[%u].%c]\n", i, i / 4, kComponents[i % 4]);

            const uint32_t temps = std::max(m_shader.tempCount, 1u);
            std::string prologue;
            prologue += Format("    float4 r[%u];\n", temps);
            prologue += Format("    [unroll] for (uint xe_i = 0u; xe_i < %uu; xe_i++)\n        r[xe_i] = float4(0.0, 0.0, 0.0, 0.0);\n", temps);
            prologue += "    bool xe_p0 = false;\n    int xe_a0 = 0;\n    int xe_aL = 0;\n    float xe_ps = 0.0;\n    int xe_vfetch_index = 0;\n";
            prologue += "    float xe_texture_lod = 0.0;\n    float3 xe_gradient_h = float3(0.0, 0.0, 0.0), xe_gradient_v = float3(0.0, 0.0, 0.0);\n";
            prologue += "    float4 xe_unused_export = float4(0.0, 0.0, 0.0, 0.0);\n";
            if (m_options.debugNan)
                prologue += "    uint xe_debug_inputs = 0u;\n";

            if (m_vertex)
            {
                source += "XeVertexOutput main(uint xe_vertex_id : SV_VertexID)\n{\n";
                source += prologue;
                source += "    float4 xe_position = float4(0.0, 0.0, 0.0, 1.0);\n";
                source += "    float4 xe_point_size_export = float4(0.0, 0.0, 0.0, 0.0);\n";
                source += "    float4 xe_interpolators[16];\n";
                source += "    [unroll] for (uint xe_j = 0u; xe_j < 16u; xe_j++)\n        xe_interpolators[xe_j] = float4(0.0, 0.0, 0.0, 0.0);\n";
                // The vertex index goes in r0.x: guest indices are byte-swapped, then offset and clamped.
                source += R"(    uint xe_vertex_index = xe_vertex_id;
    if (xe_flags & 8u)
        xe_vertex_index = XeEndianSwap(uint4(xe_vertex_index, 0u, 0u, 0u), xe_vertex_index_endian).x;
    xe_vertex_index = uint(int(xe_vertex_index) + xe_vertex_base_index);
    xe_vertex_index = clamp(xe_vertex_index, xe_vertex_index_min, xe_vertex_index_max);
    r[0].x = float(xe_vertex_index);
)";
                source += m_body;
                if (m_options.debugNan)
                {
                    source += Format("    uint xe_debug_outputs = any(XeNonFinite(xe_position)) ? %uu : 0u;\n", kDebugNonFinitePosition);
                    source += Format("    uint2 xe_debug_components = uint2(0u, 0u);\n"
                                     "    [unroll] for (uint xe_n = 0u; xe_n < 16u; xe_n++)\n"
                                     "        xe_debug_components[xe_n >> 3u] |= XeNonFiniteNibble(xe_interpolators[xe_n]) << ((xe_n & 7u) * 4u);\n"
                                     "    if (any(xe_debug_components != 0u))\n        xe_debug_outputs |= %uu;\n",
                        kDebugNonFiniteExports);
                    source += "    XeDebugCount(" + hash + ", 0u, xe_debug_product, xe_debug_outputs, xe_debug_inputs, xe_debug_components);\n";
                }
                source += R"(
    XeVertexOutput xe_output;
    float xe_w = (xe_flags & 4u) ? xe_position.w : 1.0 / xe_position.w;
    if (xe_flags & 1u)
        xe_position.xy *= xe_w;
    if (xe_flags & 2u)
        xe_position.z *= xe_w;
    xe_position.w = xe_w;
    xe_position.xyz = xe_position.xyz * xe_ndc_scale + xe_ndc_offset * xe_w;
    xe_output.position = xe_position;
    [unroll] for (uint xe_k = 0u; xe_k < 16u; xe_k++)
        xe_output.interpolators[xe_k] = xe_interpolators[xe_k];
    xe_output.point_coordinates = float2(0.0, 0.0);
)";
                source += m_shader.writesPointSize ? "    xe_output.point_size = xe_point_size_export.x;\n" : "    xe_output.point_size = -1.0;\n";
                source += "    return xe_output;\n}\n";
            }
            else
            {
                source += "struct XePixelOutput\n{\n";
                for (uint32_t i = 0; i < 4; i++)
                {
                    if (m_shader.colorTargetsWritten & (1u << i))
                        source += Format("    float4 color%u : SV_Target%u;\n", i, i);
                }
                if (m_shader.writesDepth)
                    source += "    float depth : SV_Depth;\n";
                if (!m_shader.colorTargetsWritten && !m_shader.writesDepth)
                    source += "    float4 color0 : SV_Target0;\n";
                if (m_options.alphaCoverage)
                    source += "    uint coverage : SV_Coverage;\n";
                source += "};\n\n";

                const uint32_t interpolatorCount = std::min(temps, 16u);
                if (m_options.centroidInterpolators)
                {
                    // The interpolators one by one, the centroid ones marked (in the same registers as XePixelInput's array).
                    source += "struct XeCentroidPixelInput\n{\n    float4 position : SV_Position;\n";
                    for (uint32_t i = 0; i < 16; i++)
                        source += Format("    %sfloat4 interpolator%u : TEXCOORD%u;\n", (m_options.centroidInterpolators >> i) & 1 ? "centroid " : "", i, i);
                    source += "    float2 point_coordinates : XEPOINTCOORD;\n    bool front_face : SV_IsFrontFace;\n};\n\n";
                    source += "XePixelOutput main(XeCentroidPixelInput xe_input)\n{\n";
                }
                else
                {
                    source += "XePixelOutput main(XePixelInput xe_input)\n{\n";
                }
                source += prologue;
                source += "    float4 xe_color[4] = { float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0) };\n";
                source += "    float4 xe_depth_export = float4(xe_input.position.z, 0.0, 0.0, 0.0);\n";
                if (m_debugShow)
                    source += "    float4 xe_debug_show = float4(1.0, 0.0, 1.0, 1.0);\n";
                if (m_options.centroidInterpolators)
                {
                    for (uint32_t i = 0; i < interpolatorCount; i++)
                        source += Format("    r[%u] = xe_input.interpolator%u;\n", i, i);
                }
                else
                {
                    source += Format("    [unroll] for (uint xe_j = 0u; xe_j < %uu; xe_j++)\n        r[xe_j] = xe_input.interpolators[xe_j];\n", interpolatorCount);
                }
                // Position (D3D9 pixel centers) and point sprite coordinates for ps_param_gen.
                source += Format(R"(    [unroll] for (uint xe_g = 0u; xe_g < %uu; xe_g++)
    {
        if (xe_g == xe_param_gen_register)
            r[xe_g] = float4((xe_input.position.xy / xe_resolution_scale - 0.5) * float2(xe_input.front_face ? 1.0 : -1.0, 1.0), xe_input.point_coordinates);
    }
)", temps);
                if (m_options.debugNan)
                    source += Format("    uint2 xe_debug_components = uint2(0u, 0u);\n"
                                     "    [unroll] for (uint xe_n = 0u; xe_n < %uu; xe_n++)\n"
                                     "        xe_debug_components[xe_n >> 3u] |= XeNonFiniteNibble(r[xe_n]) << ((xe_n & 7u) * 4u);\n"
                                     "    if (any(xe_debug_components != 0u))\n        xe_debug_inputs |= %uu;\n",
                        interpolatorCount, kDebugNonFiniteInterpolators);
                if (m_debugShow && DebugShowSetting().mode == 'i')
                    source += Format("    xe_debug_show = r[%u];\n", std::min(DebugShowSetting().index, temps - 1));
                source += m_body;
                // DEBUG_SOLID_PS=1 makes every pixel shader output a color encoding its depth (geometry diagnostics).
                static const bool solidPixels = [] {
                    const char* setting = std::getenv(GAME_PREFIX "_DEBUG_SOLID_PS");
                    return setting && std::atoi(setting) != 0;
                }();
                if (solidPixels)
                    source += "    [unroll] for (uint xe_d = 0u; xe_d < 4u; xe_d++)\n        xe_color[xe_d] = float4(frac(xe_input.position.z * 32.0), "
                              "frac(xe_input.position.z * 4.0), xe_input.position.z, 1.0);\n";
                if (m_debugShow)
                {
                    if (DebugShowSetting().mode == 'r')
                        source += Format("    xe_debug_show = r[%u];\n", std::min(DebugShowSetting().index, temps - 1));
                    source += "    xe_color[0] = float4(xe_debug_show.xyz, 1.0);\n";
                }
                if (m_options.debugNan)
                {
                    // Non-finite colors become magenta.
                    source += "    uint xe_debug_outputs = 0u;\n";
                    for (uint32_t i = 0; i < 4; i++)
                    {
                        if (m_shader.colorTargetsWritten & (1u << i))
                            source += Format("    if (any(XeNonFinite(xe_color[%u])))\n    {\n        xe_debug_outputs = %uu;\n"
                                             "        xe_color[%u] = float4(1.0, 0.0, 1.0, 1.0);\n    }\n",
                                i, kDebugNonFiniteColor, i);
                    }
                    if (m_shader.writesDepth)
                        source += Format("    if (XeNonFinite(xe_depth_export).x)\n        xe_debug_outputs = %uu;\n", kDebugNonFiniteColor);
                    source += "    XeDebugCount(" + hash + ", 1u, xe_debug_product, xe_debug_outputs, xe_debug_inputs, xe_debug_components);\n";
                    // DEBUG_NAN=2: non-finite interpolators paint the pixel green.
                    if (m_options.debugNan >= 2)
                        source += Format("    if (xe_debug_inputs & %uu)\n    {\n        [unroll] for (uint xe_m = 0u; xe_m < 4u; xe_m++)\n"
                                         "            xe_color[xe_m] = float4(0.0, 1.0, 0.0, 1.0);\n    }\n",
                            kDebugNonFiniteInterpolators);
                }
                if (m_options.alphaCoverage)
                {
                    // The alpha test's edge as coverage: the share of samples passing grows from none to all as alpha
                    // crosses the reference over about a pixel (its screen-space rate), on a run of samples rotated
                    // per pixel so partial coverage dithers. Comparisons that aren't above or below keep the hard test.
                    source += R"(
    uint xe_coverage = 0xFFFFFFFFu;
    if (xe_alpha_func != 7u)
    {
        float xe_alpha = xe_color[0].w;
        bool xe_above = xe_alpha_func == 4u || xe_alpha_func == 6u, xe_below = xe_alpha_func == 1u || xe_alpha_func == 3u;
        if (xe_alpha_coverage_samples > 1u && (xe_above || xe_below))
        {
            float xe_edge = saturate((xe_alpha - xe_alpha_ref) / max(fwidth(xe_alpha), 1.0 / 1024.0) + 0.5);
            uint xe_samples = xe_alpha_coverage_samples;
            uint xe_count = uint((xe_below ? 1.0 - xe_edge : xe_edge) * float(xe_samples) + 0.5);
            if (xe_count == 0u)
                discard;
            uint xe_rotate = (uint(xe_input.position.x) + uint(xe_input.position.y)) % xe_samples;
            uint xe_run = (1u << xe_count) - 1u;
            xe_coverage = ((xe_run << xe_rotate) | (xe_run >> (xe_samples - xe_rotate))) & ((1u << xe_samples) - 1u);
        }
        else
        {
            bool xe_pass = ((xe_alpha_func & 1u) != 0u && xe_alpha < xe_alpha_ref) || ((xe_alpha_func & 2u) != 0u && xe_alpha == xe_alpha_ref) ||
                ((xe_alpha_func & 4u) != 0u && xe_alpha > xe_alpha_ref);
            if (!xe_pass)
                discard;
        }
    }
    XePixelOutput xe_output;
    xe_output.coverage = xe_coverage;
)";
                }
                else
                {
                    source += R"(
    if (xe_alpha_func != 7u)
    {
        float xe_alpha = xe_color[0].w;
        bool xe_pass = ((xe_alpha_func & 1u) != 0u && xe_alpha < xe_alpha_ref) || ((xe_alpha_func & 2u) != 0u && xe_alpha == xe_alpha_ref) ||
            ((xe_alpha_func & 4u) != 0u && xe_alpha > xe_alpha_ref);
        if (!xe_pass)
            discard;
    }
    XePixelOutput xe_output;
)";
                }
                for (uint32_t i = 0; i < 4; i++)
                {
                    if (m_shader.colorTargetsWritten & (1u << i))
                        source += Format("    xe_output.color%u = XeMinMaxFactors(xe_color[%u] * xe_color_exp_bias[%u], xe_min_max_factors[%u]);\n", i,
                            i, i, i);
                }
                if (m_shader.writesDepth)
                    source += "    xe_output.depth = saturate(xe_depth_export.x);\n";
                if (!m_shader.colorTargetsWritten && !m_shader.writesDepth)
                    source += "    xe_output.color0 = float4(0.0, 0.0, 0.0, 0.0);\n";
                source += "    return xe_output;\n}\n";
            }

            HlslResult result;
            result.source = std::move(source);
            result.unsupported = std::move(m_unsupported);
            return result;
        }
    }

    bool XenosProducts()
    {
        static const bool enabled = [] {
            const char* setting = std::getenv(GAME_PREFIX "_XENOS_PRODUCTS");
            return !setting || std::atoi(setting) != 0;
        }();
        return enabled;
    }

    uint32_t DebugNan()
    {
        static const uint32_t level = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_NAN");
            return setting ? uint32_t(std::max(std::atoi(setting), 0)) : 0u;
        }();
        return level;
    }

    HlslResult TranslateToHlsl(const ParsedShader& shader, const TranslationOptions& options)
    {
        Translator translator(shader, options);
        return translator.Translate();
    }

    const char* RectangleListGeometryShader()
    {
        // The three vertices of a rectangle are corners; the fourth is the corner opposite the one
        // that isn't on the longest edge (the diagonal), extrapolated as a parallelogram.
        static const std::string source = std::string(kHlslCommon) + R"(
struct XeGeometryOutput
{
    float4 position : SV_Position;
    float4 interpolators[16] : TEXCOORD0;
    float2 point_coordinates : XEPOINTCOORD;
};

XeGeometryOutput XeCopyVertex(XeVertexOutput v)
{
    XeGeometryOutput o;
    o.position = v.position;
    [unroll] for (uint i = 0u; i < 16u; i++)
        o.interpolators[i] = v.interpolators[i];
    o.point_coordinates = v.point_coordinates;
    return o;
}

[maxvertexcount(4)]
void main(triangle XeVertexOutput xe_in[3], inout TriangleStream<XeGeometryOutput> xe_stream)
{
    float2 p0 = xe_in[0].position.xy / xe_in[0].position.w;
    float2 p1 = xe_in[1].position.xy / xe_in[1].position.w;
    float2 p2 = xe_in[2].position.xy / xe_in[2].position.w;
    float d01 = dot(p1 - p0, p1 - p0), d02 = dot(p2 - p0, p2 - p0), d12 = dot(p2 - p1, p2 - p1);
    uint corner = (d12 >= d01 && d12 >= d02) ? 0u : (d02 >= d01 ? 1u : 2u);
    uint a = corner == 0u ? 1u : 0u;
    uint b = corner == 2u ? 1u : 2u;
    XeGeometryOutput v3 = XeCopyVertex(xe_in[a]);
    v3.position = xe_in[a].position + xe_in[b].position - xe_in[corner].position;
    [unroll] for (uint i = 0u; i < 16u; i++)
        v3.interpolators[i] = xe_in[a].interpolators[i] + xe_in[b].interpolators[i] - xe_in[corner].interpolators[i];
    xe_stream.Append(XeCopyVertex(xe_in[corner]));
    xe_stream.Append(XeCopyVertex(xe_in[a]));
    xe_stream.Append(XeCopyVertex(xe_in[b]));
    xe_stream.Append(v3);
    xe_stream.RestartStrip();
}
)";
        return source.c_str();
    }

    const char* PointListGeometryShader()
    {
        static const std::string source = std::string(kHlslCommon) + R"(
struct XeGeometryOutput
{
    float4 position : SV_Position;
    float4 interpolators[16] : TEXCOORD0;
    float2 point_coordinates : XEPOINTCOORD;
};

[maxvertexcount(4)]
void main(point XeVertexOutput xe_in[1], inout TriangleStream<XeGeometryOutput> xe_stream)
{
    float size = xe_in[0].point_size > 0.0 ? xe_in[0].point_size : xe_point_size;
    size = clamp(size, xe_point_size_min_max.x, xe_point_size_min_max.y);
    float2 extent = 0.5 * size * xe_point_screen_to_ndc * xe_in[0].position.w;
    static const float2 kCorners[4] = { float2(-1.0, 1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0) };
    [unroll] for (uint i = 0u; i < 4u; i++)
    {
        XeGeometryOutput o;
        o.position = xe_in[0].position;
        o.position.xy += kCorners[i] * extent;
        [unroll] for (uint j = 0u; j < 16u; j++)
            o.interpolators[j] = xe_in[0].interpolators[j];
        o.point_coordinates = kCorners[i] * float2(0.5, -0.5) + 0.5;
        xe_stream.Append(o);
    }
    xe_stream.RestartStrip();
}
)";
        return source.c_str();
    }
}
