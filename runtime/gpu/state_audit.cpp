#include "gpu/state_audit.h"

#include "config.h"
#include "gpu/registers.h"
#include "gpu/renderer.h"
#include "log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include <dxgiformat.h>

namespace gpu::audit
{
    namespace
    {
        const bool kEnabled = [] {
            const char* setting = std::getenv(GAME_PREFIX "_AUDIT");
            return setting && std::atoi(setting) != 0;
        }();

        std::string Format(const char* format, ...)
        {
            char text[512];
            va_list arguments;
            va_start(arguments, format);
            vsnprintf(text, sizeof(text), format, arguments);
            va_end(arguments);
            return text;
        }

        // The registers registers.h names, and whether the renderer or the command processor reads them.
        struct Known
        {
            uint32_t index;
            const char* name;
            bool read;
        };
#define READ(name) { reg::name, #name, true }
#define UNREAD(name) { reg::name, #name, false }
        const Known kKnown[] = {
            READ(DC_LUT_RW_SELECT), READ(DC_LUT_RW_MODE), READ(DC_LUT_RW_INDEX), READ(DC_LUT_SEQ_COLOR), READ(DC_LUT_PWL_DATA), READ(DC_LUT_30_COLOR),
            READ(DC_LUT_WRITE_EN_MASK), READ(RB_SURFACE_INFO), READ(RB_COLOR_INFO), READ(RB_DEPTH_INFO), READ(RB_COLOR1_INFO), READ(RB_COLOR2_INFO),
            READ(RB_COLOR3_INFO), READ(PA_SC_SCREEN_SCISSOR_TL), READ(PA_SC_SCREEN_SCISSOR_BR), READ(PA_SC_WINDOW_OFFSET), READ(PA_SC_WINDOW_SCISSOR_TL),
            READ(PA_SC_WINDOW_SCISSOR_BR), READ(VGT_MAX_VTX_INDX), READ(VGT_MIN_VTX_INDX), READ(VGT_INDX_OFFSET), READ(VGT_MULTI_PRIM_IB_RESET_INDX),
            READ(RB_COLOR_MASK), READ(RB_BLEND_RED), READ(RB_BLEND_GREEN), READ(RB_BLEND_BLUE), READ(RB_BLEND_ALPHA), UNREAD(RB_STENCILREFMASK_BF),
            READ(RB_STENCILREFMASK), READ(RB_ALPHA_REF), READ(PA_CL_VPORT_XSCALE), READ(PA_CL_VPORT_XOFFSET), READ(PA_CL_VPORT_YSCALE),
            READ(PA_CL_VPORT_YOFFSET), READ(PA_CL_VPORT_ZSCALE), READ(PA_CL_VPORT_ZOFFSET), READ(SQ_PROGRAM_CNTL), READ(SQ_CONTEXT_MISC),
            READ(SQ_INTERPOLATOR_CNTL), UNREAD(SQ_WRAPPING_0), UNREAD(SQ_WRAPPING_1), READ(VGT_EVENT_INITIATOR), READ(VGT_DRAW_INITIATOR),
            READ(RB_DEPTHCONTROL), READ(RB_BLENDCONTROL0), READ(RB_COLORCONTROL), UNREAD(RB_HIZCONTROL), READ(PA_CL_CLIP_CNTL), READ(PA_SU_SC_MODE_CNTL),
            READ(PA_CL_VTE_CNTL), UNREAD(VGT_CURRENT_BIN_ID_MIN), READ(RB_MODECONTROL), READ(RB_BLENDCONTROL1), READ(RB_BLENDCONTROL2),
            READ(RB_BLENDCONTROL3), READ(PA_SU_POINT_SIZE), READ(PA_SU_POINT_MINMAX), UNREAD(PA_SU_LINE_CNTL), UNREAD(VGT_OUTPUT_PATH_CNTL),
            UNREAD(PA_SC_LINE_CNTL), UNREAD(PA_SC_AA_CONFIG), READ(PA_SU_VTX_CNTL), UNREAD(SQ_VS_CONST), UNREAD(SQ_PS_CONST), UNREAD(PA_SC_AA_MASK),
            READ(RB_COPY_CONTROL), READ(RB_COPY_DEST_BASE), READ(RB_COPY_DEST_PITCH), READ(RB_COPY_DEST_INFO), UNREAD(RB_TILE_CLEAR), READ(RB_DEPTH_CLEAR),
            READ(RB_COLOR_CLEAR), READ(RB_COLOR_CLEAR_LO), UNREAD(RB_COPY_FUNC), UNREAD(RB_COPY_REF), UNREAD(RB_COPY_MASK), UNREAD(RB_COPY_SURFACE_SLICE),
            UNREAD(RB_SAMPLE_COUNT_CTL), UNREAD(RB_SAMPLE_COUNT_ADDR), READ(PA_SU_POLY_OFFSET_FRONT_SCALE), READ(PA_SU_POLY_OFFSET_FRONT_OFFSET),
            READ(PA_SU_POLY_OFFSET_BACK_SCALE), READ(PA_SU_POLY_OFFSET_BACK_OFFSET),
        };
#undef READ
#undef UNREAD

        // Values written to each register of the display (0x1800-0x1FFF) and the draw context (0x2000-0x23FF).
        constexpr uint32_t kFirstRegister = 0x1800, kRegisterCount = 0xC00;
        constexpr size_t kMaxValues = 24;
        struct Census
        {
            std::map<uint32_t, uint64_t> values; // the first kMaxValues distinct ones, with counts
            uint64_t writes = 0, otherWrites = 0; // writes of values beyond those
        };
        std::unique_ptr<Census[]> g_registers = kEnabled ? std::make_unique<Census[]>(kRegisterCount) : nullptr;

        struct Count
        {
            uint64_t count = 0, firstFrame = 0;
        };
        std::map<std::string, Count> g_findings, g_textures, g_resolves, g_interpolation;
        uint64_t g_frame = 0;

        void Add(std::map<std::string, Count>& map, const std::string& key)
        {
            auto [it, inserted] = map.try_emplace(key);
            if (inserted)
                it->second.firstFrame = g_frame;
            it->second.count++;
        }

        const char kComponents[] = "XYZW01??";

        void WriteReport()
        {
            std::error_code error;
            std::filesystem::create_directories(config::DataDirectory() + "/dumps", error);
            const std::string path = config::DataDirectory() + "/dumps/audit.txt";
            FILE* file = fopen(path.c_str(), "w");
            if (!file)
                return;
            fprintf(file, "GPU state audit through frame %llu (AUDIT=1): the state the game used, against what the renderer implements.\n\n",
                (unsigned long long)g_frame);
            const auto sorted = [](const std::map<std::string, Count>& map) {
                std::vector<std::pair<std::string, Count>> entries(map.begin(), map.end());
                std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
                return entries;
            };
            fprintf(file, "Findings: state the renderer doesn't implement or only approximates (count, first frame).\n");
            for (const auto& [what, count] : sorted(g_findings))
                fprintf(file, "  %10llu  frame %6llu  %s\n", (unsigned long long)count.count, (unsigned long long)count.firstFrame, what.c_str());

            fprintf(file, "\nRegisters written (display and draw context), with each value's writes; [unread] if the renderer doesn't read it.\n");
            for (uint32_t i = 0; i < kRegisterCount; i++)
            {
                const Census& census = g_registers[i];
                if (!census.writes)
                    continue;
                const uint32_t index = kFirstRegister + i;
                const Known* known = nullptr;
                for (const Known& entry : kKnown)
                {
                    if (entry.index == index)
                        known = &entry;
                }
                fprintf(file, "  %04X %s%s: %llu writes\n   ", index, known ? known->name : "(unnamed)", known && known->read ? "" : " [unread]",
                    (unsigned long long)census.writes);
                for (const auto& [value, count] : census.values)
                    fprintf(file, " %08X x%llu", value, (unsigned long long)count);
                if (census.otherWrites)
                    fprintf(file, " (and %llu writes of other values)", (unsigned long long)census.otherWrites);
                fprintf(file, "\n");
            }

            fprintf(file, "\nTexture fetches: each fetch constant's fields but addresses and sizes (fetches, first frame).\n");
            for (const auto& [what, count] : sorted(g_textures))
                fprintf(file, "  %10llu  frame %6llu  %s\n", (unsigned long long)count.count, (unsigned long long)count.firstFrame, what.c_str());
            fprintf(file, "\nInterpolation (draws, first frame).\n");
            for (const auto& [what, count] : sorted(g_interpolation))
                fprintf(file, "  %10llu  frame %6llu  %s\n", (unsigned long long)count.count, (unsigned long long)count.firstFrame, what.c_str());
            fprintf(file, "\nResolves (count, first frame).\n");
            for (const auto& [what, count] : sorted(g_resolves))
                fprintf(file, "  %10llu  frame %6llu  %s\n", (unsigned long long)count.count, (unsigned long long)count.firstFrame, what.c_str());
            fclose(file);
        }
    }

    bool Enabled()
    {
        return kEnabled;
    }

    void Finding(const std::string& what)
    {
        if (!kEnabled)
            return;
        if (!g_findings.count(what))
            LOG_WARN("GPU audit: %s", what.c_str());
        Add(g_findings, what);
    }

    void RegisterWritten(uint32_t index, uint32_t value)
    {
        if (!kEnabled || index < kFirstRegister || index >= kFirstRegister + kRegisterCount)
            return;
        Census& census = g_registers[index - kFirstRegister];
        census.writes++;
        if (auto it = census.values.find(value); it != census.values.end())
            it->second++;
        else if (census.values.size() < kMaxValues)
            census.values.emplace(value, 1);
        else
            census.otherWrites++;
    }

    void Draw(uint32_t primitive, uint32_t colorTargets, uint32_t samples, bool depthStencil)
    {
        if (!kEnabled)
            return;
        const uint32_t colorControl = Register(reg::RB_COLORCONTROL);
        if (colorTargets && (colorControl & 0x10))
            Finding(Format("alpha to mask (RB_COLORCONTROL %08X)", colorControl));

        const uint32_t mode = Register(reg::PA_SU_SC_MODE_CNTL);
        if ((mode >> 3) & 3)
            Finding(Format("polygon fill modes (PA_SU_SC_MODE_CNTL poly_mode %u, front %u, back %u)", (mode >> 3) & 3, (mode >> 5) & 7, (mode >> 8) & 7));
        if ((mode >> 20) & 1)
            Finding("perspective correction off (PA_SU_SC_MODE_CNTL persp_corr_dis)");

        // Interpolation: SQ_INTERPOLATOR_CNTL's param_shade has a bit set for each interpolator shaded smoothly (D3D sets
        // the ones in use, a mask from bit 0: a gap would be a flat one), and sampling_pattern a bit set for each sampled at
        // the pixel's center rather than its centroid, which applies with MSAA when SQ_CONTEXT_MISC's sc_sample_cntl is 2
        // (0: all centroid, 1: all center). The shaders interpolate at centers.
        const uint32_t interpolators = Register(reg::SQ_INTERPOLATOR_CNTL);
        const uint32_t smooth = interpolators & 0xFFFF, centers = interpolators >> 16;
        const uint32_t sampleControl = (Register(reg::SQ_CONTEXT_MISC) >> 2) & 3;
        Add(g_interpolation, Format("SQ_INTERPOLATOR_CNTL %08X, sc_sample_cntl %u, %ux MSAA", interpolators, sampleControl, samples));
        if (smooth & (smooth + 1))
            Finding(Format("flat shading (SQ_INTERPOLATOR_CNTL param_shade %04X%s)", smooth, ((mode >> 19) & 1) ? ", last vertex provoking" : ""));
        if (samples > 1 && smooth)
        {
            const uint32_t centroid = sampleControl == 0 ? smooth : sampleControl == 2 ? smooth & ~centers : 0;
            if (centroid)
                Finding(Format("centroid sampling of interpolators %04X (SQ_INTERPOLATOR_CNTL %08X, sc_sample_cntl %u)", centroid, interpolators,
                    sampleControl));
        }
        if (Register(reg::SQ_WRAPPING_0) || Register(reg::SQ_WRAPPING_1))
            Finding(Format("cylindrical wrapping (SQ_WRAPPING %08X %08X)", Register(reg::SQ_WRAPPING_0), Register(reg::SQ_WRAPPING_1)));

        const uint32_t clip = Register(reg::PA_CL_CLIP_CNTL);
        if (clip & 0x3F)
            Finding(Format("user clip planes (PA_CL_CLIP_CNTL ucp_ena %02X, ps_ucp_mode %u)", clip & 0x3F, (clip >> 14) & 3));
        if (!((clip >> 19) & 1) && !((clip >> 16) & 1))
            Finding("OpenGL clip space (PA_CL_CLIP_CNTL dx_clip_space_def 0)");

        const uint32_t program = Register(reg::SQ_PROGRAM_CNTL);
        if ((program >> 24) & 7)
            Finding(Format("vertex shader export mode %u (SQ_PROGRAM_CNTL vs_export_mode)", (program >> 24) & 7));
        if ((program >> 19) & 1)
            Finding("primitive index in the pixel shader (SQ_PROGRAM_CNTL gen_index_pix)");
        if (Register(reg::SQ_VS_CONST) != 0x000FF000 || Register(reg::SQ_PS_CONST) != 0x000FF100)
            Finding(Format("float constant ranges other than 0-255 and 256-511 (SQ_VS_CONST %08X, SQ_PS_CONST %08X)", Register(reg::SQ_VS_CONST),
                Register(reg::SQ_PS_CONST)));

        if (samples > 1)
        {
            const uint32_t all = (1u << samples) - 1, mask = Register(reg::PA_SC_AA_MASK) & 0xFFFF;
            if ((mask & all) != all)
                Finding(Format("sample mask (PA_SC_AA_MASK %04X with %u samples)", mask, samples));
        }

        const uint32_t depthControl = Register(reg::RB_DEPTHCONTROL);
        if (depthStencil && (depthControl & 1) && (depthControl & 0x80) && Register(reg::RB_STENCILREFMASK_BF) != Register(reg::RB_STENCILREFMASK))
            Finding(Format("back faces' own stencil reference or masks (RB_STENCILREFMASK_BF %08X, front %08X)", Register(reg::RB_STENCILREFMASK_BF),
                Register(reg::RB_STENCILREFMASK)));

        static const uint32_t kBlendControls[4] = { reg::RB_BLENDCONTROL0, reg::RB_BLENDCONTROL1, reg::RB_BLENDCONTROL2, reg::RB_BLENDCONTROL3 };
        static const uint32_t kColorInfos[4] = { reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO, reg::RB_COLOR3_INFO };
        // Constant color and alpha factors in the same draw's color blending: Direct3D 12 has one constant for both (the
        // renderer gives color the constant's alpha when only the alpha factors are used).
        bool constantColor = false, constantAlpha = false;
        for (uint32_t i = 0; i < 4; i++)
        {
            if (!(colorTargets & (1u << i)))
                continue;
            const uint32_t blend = Register(kBlendControls[i]);
            const uint32_t factors[4] = { blend & 0x1F, (blend >> 8) & 0x1F, (blend >> 16) & 0x1F, (blend >> 24) & 0x1F };
            for (uint32_t j = 0; j < 4; j++)
            {
                const uint32_t factor = factors[j];
                if (factor == 2 || factor == 3 || factor > 16)
                    Finding(Format("blend factor %u (RB_BLENDCONTROL %08X)", factor, blend));
                if (j < 2)
                {
                    constantColor |= factor == 12 || factor == 13;
                    constantAlpha |= factor == 14 || factor == 15;
                }
            }
            if (((blend >> 5) & 7) > 4 || ((blend >> 21) & 7) > 4)
                Finding(Format("blend op (RB_BLENDCONTROL %08X)", blend));
            const uint32_t format = (Register(kColorInfos[i]) >> 16) & 0xF;
            if (format == 1)
                Finding("gamma render target (k_8_8_8_8_GAMMA: an sRGB view, not Xenos's piecewise linear curve)");
            else if (format == 8 || format == 9 || format == 11 || format == 13)
                Finding(Format("render target format %u", format));
        }

        if (constantColor && constantAlpha && (RegisterFloat(reg::RB_BLEND_RED) != RegisterFloat(reg::RB_BLEND_ALPHA) ||
                                                  RegisterFloat(reg::RB_BLEND_GREEN) != RegisterFloat(reg::RB_BLEND_ALPHA) ||
                                                  RegisterFloat(reg::RB_BLEND_BLUE) != RegisterFloat(reg::RB_BLEND_ALPHA)))
            Finding("constant color and constant alpha blend factors together, with a constant whose color isn't its alpha");

        if (primitive == kLineList || primitive == kLineStrip || primitive == kLineLoop)
            Finding(Format("lines (PA_SU_LINE_CNTL %08X)", Register(reg::PA_SU_LINE_CNTL)));
        if (Register(reg::VGT_OUTPUT_PATH_CNTL))
            Finding(Format("vertex grouper output path %08X (VGT_OUTPUT_PATH_CNTL)", Register(reg::VGT_OUTPUT_PATH_CNTL)));
    }

    void Resolve()
    {
        if (!kEnabled)
            return;
        const uint32_t copyControl = Register(reg::RB_COPY_CONTROL), destInfo = Register(reg::RB_COPY_DEST_INFO);
        const uint32_t sourceSelect = copyControl & 7, command = (copyControl >> 20) & 3;
        const uint32_t surface = Register(reg::RB_SURFACE_INFO);
        const uint32_t sourceFormat = sourceSelect < 4 ? (Register(sourceSelect == 0   ? reg::RB_COLOR_INFO
                                                                          : sourceSelect == 1 ? reg::RB_COLOR1_INFO
                                                                          : sourceSelect == 2 ? reg::RB_COLOR2_INFO
                                                                                              : reg::RB_COLOR3_INFO) >>
                                                             16) &
                                                             0xF
                                                       : (Register(reg::RB_DEPTH_INFO) >> 16) & 1;
        const uint32_t destFormat = (destInfo >> 7) & 0x3F, number = (destInfo >> 13) & 7;
        Add(g_resolves, Format("source %s format %u, %ux MSAA, sample select %u; command %u%s%s; dest format %u, endian %u, number %u, exp bias %d, swap %u%s",
                            sourceSelect < 4 ? "color" : "depth", sourceFormat, 1u << std::min((surface >> 16) & 3, 2u), (copyControl >> 4) & 7, command,
                            (copyControl >> 8) & 1 ? ", color clear" : "", (copyControl >> 9) & 1 ? ", depth clear" : "", destFormat, destInfo & 7,
                            number, int32_t(destInfo << 10) >> 26, (destInfo >> 24) & 1, (destInfo >> 3) & 0xF ? ", array/slice" : ""));
        // (Depth is copied as the EDRAM's words whatever the command.)
        if (sourceSelect < 4 && (command == 0 || command == 2))
            Finding(Format("resolve copy command %u of color format %u to format %u (raw or constant; treated as a conversion)", command,
                sourceFormat, destFormat));
        if ((destInfo >> 3) & 0xF)
            Finding(Format("resolve into an array or a 3D slice (RB_COPY_DEST_INFO %08X)", destInfo));
        const bool floatDest = destFormat == 31 || destFormat == 32 || destFormat == 36 || destFormat == 37;
        // (The formats color resolves write: k_8 and k_5_6_5 below a dword, 8_8_8_8 and its gamma version, 2_10_10_10,
        // 16_16, 16_16_16_16 and the float ones.)
        if (destFormat != 2 && destFormat != 4 && destFormat != 6 && destFormat != 7 && destFormat != 25 && destFormat != 26 && !floatDest &&
            destFormat != 62 && sourceSelect < 4)
            Finding(Format("resolve to format %u (not written)", destFormat));
        if (number != 0 && !(floatDest && number == 7))
            Finding(Format("resolve number format %u to format %u (RB_COPY_DEST_INFO %08X)", number, destFormat, destInfo));
        if (Register(reg::RB_COPY_SURFACE_SLICE))
            Finding(Format("resolve from a surface slice (RB_COPY_SURFACE_SLICE %08X)", Register(reg::RB_COPY_SURFACE_SLICE)));
        if (Register(reg::RB_COPY_FUNC) || Register(reg::RB_COPY_MASK))
            Finding(Format("copy function or mask (RB_COPY_FUNC %08X, RB_COPY_REF %08X, RB_COPY_MASK %08X)", Register(reg::RB_COPY_FUNC),
                Register(reg::RB_COPY_REF), Register(reg::RB_COPY_MASK)));
    }

    void TextureFetch(const xenos::TextureFetch& fetch)
    {
        if (!kEnabled)
            return;
        const uint32_t* d = fetch.d;
        char swizzle[5] = {};
        for (uint32_t i = 0; i < 4; i++)
            swizzle[i] = kComponents[(fetch.Swizzle() >> (i * 3)) & 7];
        const int32_t gradH = int32_t(d[4] << 5) >> 27, gradV = int32_t(d[4]) >> 27;
        Add(g_textures, Format("format %u, signs %u%u%u%u%s, swizzle %s, clamp %u/%u/%u, border color %u%s, filters %u/%u/%u aniso %u%s%s, "
                               "mips %u-%u, LOD bias %g%s, number %s, exp %d, dimension %u%s%s, endian %u, %s%s",
            uint32_t(fetch.Format()), fetch.Sign(0), fetch.Sign(1), fetch.Sign(2), fetch.Sign(3), (d[0] >> 19) & 1 ? " (no zero)" : "", swizzle,
            fetch.ClampX(), fetch.ClampY(), fetch.ClampZ(), fetch.BorderColor(), (d[3] >> 31) ? ", border size 1" : "", fetch.MagFilter(),
            fetch.MinFilter(), fetch.MipFilter(), fetch.AnisoFilter(), (d[3] >> 28) & 7 ? Format(" arbitrary %u", (d[3] >> 28) & 7).c_str() : "",
            (d[4] >> 10) & 3 ? Format(" walk %u", (d[4] >> 10) & 3).c_str() : "", fetch.MipMinLevel(), fetch.MipMaxLevel(), fetch.LodBias(),
            gradH || gradV ? Format(", gradient exp %d/%d", gradH, gradV).c_str() : "", fetch.NumFormatInteger() ? "integer" : "fraction",
            fetch.ExpAdjust(), fetch.Dimension(), fetch.PackedMips() ? ", packed mips" : "",
            (d[5] >> 2) & 0x7F ? Format(", tri clamp %u aniso bias %u force bc %u", (d[5] >> 3) & 3, (d[5] >> 5) & 0xF, (d[5] >> 2) & 1).c_str() : "",
            fetch.Endian(), fetch.Tiled() ? "tiled" : "linear", (d[1] >> 11) & 1 ? ", nearest clamp policy 1" : ""));

        // (Float formats are what they are, whatever their number format and sign modes say.)
        const uint32_t host = xenos::GetFormatInfo(fetch.Format()).dxgiFormat;
        const bool floating = host == DXGI_FORMAT_R32G32B32A32_FLOAT || host == DXGI_FORMAT_R16G16B16A16_FLOAT || host == DXGI_FORMAT_R32G32_FLOAT ||
            host == DXGI_FORMAT_R16G16_FLOAT || host == DXGI_FORMAT_R32_FLOAT || host == DXGI_FORMAT_R16_FLOAT;
        if (fetch.NumFormatInteger() && !floating)
            Finding(Format("integer texture fetch (format %u)", uint32_t(fetch.Format())));
        if (fetch.ExpAdjust())
            Finding(Format("texture exponent adjustment %d (format %u)", fetch.ExpAdjust(), uint32_t(fetch.Format())));
        if (d[3] >> 31)
            Finding("texture border size 1");
        for (uint32_t clamp : { fetch.ClampX(), fetch.ClampY(), fetch.ClampZ() })
        {
            if (clamp == 4 || clamp == 5 || clamp == 7)
                Finding(Format("texture address mode %u (approximated: halfway clamps as edges, mirrored border as mirror once)", clamp));
        }
        if (gradH || gradV)
            Finding(Format("texture gradient exponent adjustment %d/%d", gradH, gradV));
        if ((d[5] >> 3) & 0x3F)
            Finding(Format("texture tri clamp %u or anisotropy bias %u", (d[5] >> 3) & 3, (d[5] >> 5) & 0xF));
        const bool border = fetch.ClampX() >= 6 || fetch.ClampY() >= 6 || fetch.ClampZ() >= 6;
        if (((d[5] >> 2) & 1) && border && fetch.BorderColor() == 0)
            Finding("transparent border color forced opaque (force_bc_w_to_max)");
        const bool signedComponent = fetch.Sign(0) == 1 || fetch.Sign(1) == 1 || fetch.Sign(2) == 1 || fetch.Sign(3) == 1;
        if (signedComponent && ((d[0] >> 19) & 1))
            Finding("signed texture in the no-zero mapping (signed_rf_mode_all 1)");
    }

    void Frame(uint64_t frame)
    {
        if (!kEnabled)
            return;
        g_frame = frame;
        if (frame && frame % 600 == 0)
            WriteReport();
    }
}
