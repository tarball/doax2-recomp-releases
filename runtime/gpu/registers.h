#pragma once

// Xenos register indices (dword offsets in the register file) and packet constants used by the
// command processor and renderer. Names follow Xenia's register table.
#include <cstdint>

namespace gpu::reg
{
    // Display controller: the gamma lookup table the scanout applies to the front buffer (registers as on AMD's R5xx
    // display engine, which Xenos's follows): 256 entries of 10-bit blue, green and red (DC_LUT_30_COLOR: bits 0-9,
    // 10-19 and 20-29), or a piecewise linear ramp. Its index advances with each 30-bit color written.
    inline constexpr uint32_t DC_LUT_RW_SELECT = 0x1920; // 0: LUT A (the one the front buffer goes through), 1: LUT B
    inline constexpr uint32_t DC_LUT_RW_MODE = 0x1921;   // bit 0: 0 the 256-entry table, 1 piecewise linear
    inline constexpr uint32_t DC_LUT_RW_INDEX = 0x1922;
    inline constexpr uint32_t DC_LUT_SEQ_COLOR = 0x1923; // sequential writes, a component at a time
    inline constexpr uint32_t DC_LUT_PWL_DATA = 0x1924;
    inline constexpr uint32_t DC_LUT_30_COLOR = 0x1925;
    inline constexpr uint32_t DC_LUT_WRITE_EN_MASK = 0x1927; // bit 0 blue, 1 green, 2 red

    // Render backend: render targets and EDRAM.
    inline constexpr uint32_t RB_SURFACE_INFO = 0x2000;
    inline constexpr uint32_t RB_COLOR_INFO = 0x2001;
    inline constexpr uint32_t RB_DEPTH_INFO = 0x2002;
    inline constexpr uint32_t RB_COLOR1_INFO = 0x2003;
    inline constexpr uint32_t RB_COLOR2_INFO = 0x2004;
    inline constexpr uint32_t RB_COLOR3_INFO = 0x2005;

    // Scissor, window offset and viewport.
    inline constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x200E;
    inline constexpr uint32_t PA_SC_SCREEN_SCISSOR_BR = 0x200F;
    inline constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080;
    inline constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL = 0x2081;
    inline constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR = 0x2082;
    inline constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100;
    inline constexpr uint32_t VGT_MIN_VTX_INDX = 0x2101;
    inline constexpr uint32_t VGT_INDX_OFFSET = 0x2102;
    inline constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
    inline constexpr uint32_t RB_COLOR_MASK = 0x2104;
    inline constexpr uint32_t RB_BLEND_RED = 0x2105;
    inline constexpr uint32_t RB_BLEND_GREEN = 0x2106;
    inline constexpr uint32_t RB_BLEND_BLUE = 0x2107;
    inline constexpr uint32_t RB_BLEND_ALPHA = 0x2108;
    inline constexpr uint32_t RB_STENCILREFMASK_BF = 0x210C;
    inline constexpr uint32_t RB_STENCILREFMASK = 0x210D;
    inline constexpr uint32_t RB_ALPHA_REF = 0x210E;
    inline constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;
    inline constexpr uint32_t PA_CL_VPORT_XOFFSET = 0x2110;
    inline constexpr uint32_t PA_CL_VPORT_YSCALE = 0x2111;
    inline constexpr uint32_t PA_CL_VPORT_YOFFSET = 0x2112;
    inline constexpr uint32_t PA_CL_VPORT_ZSCALE = 0x2113;
    inline constexpr uint32_t PA_CL_VPORT_ZOFFSET = 0x2114;

    // Shader sequencer.
    inline constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180;
    inline constexpr uint32_t SQ_CONTEXT_MISC = 0x2181;
    inline constexpr uint32_t SQ_INTERPOLATOR_CNTL = 0x2182;
    inline constexpr uint32_t SQ_WRAPPING_0 = 0x2183;
    inline constexpr uint32_t SQ_WRAPPING_1 = 0x2184;

    inline constexpr uint32_t VGT_EVENT_INITIATOR = 0x21F9;
    inline constexpr uint32_t VGT_DRAW_INITIATOR = 0x21FC;

    // Render state.
    inline constexpr uint32_t RB_DEPTHCONTROL = 0x2200;
    inline constexpr uint32_t RB_BLENDCONTROL0 = 0x2201;
    inline constexpr uint32_t RB_COLORCONTROL = 0x2202;
    inline constexpr uint32_t RB_HIZCONTROL = 0x2203;
    inline constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204;
    inline constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
    inline constexpr uint32_t PA_CL_VTE_CNTL = 0x2206;
    inline constexpr uint32_t VGT_CURRENT_BIN_ID_MIN = 0x2207;
    inline constexpr uint32_t RB_MODECONTROL = 0x2208;
    inline constexpr uint32_t RB_BLENDCONTROL1 = 0x2209;
    inline constexpr uint32_t RB_BLENDCONTROL2 = 0x220A;
    inline constexpr uint32_t RB_BLENDCONTROL3 = 0x220B;
    inline constexpr uint32_t PA_SU_POINT_SIZE = 0x2280;
    inline constexpr uint32_t PA_SU_POINT_MINMAX = 0x2281;
    inline constexpr uint32_t PA_SU_LINE_CNTL = 0x2282;
    inline constexpr uint32_t VGT_OUTPUT_PATH_CNTL = 0x2284;
    inline constexpr uint32_t PA_SC_LINE_CNTL = 0x2300;
    inline constexpr uint32_t PA_SC_AA_CONFIG = 0x2301;
    inline constexpr uint32_t PA_SU_VTX_CNTL = 0x2302;
    inline constexpr uint32_t SQ_VS_CONST = 0x2307;
    inline constexpr uint32_t SQ_PS_CONST = 0x2308;
    inline constexpr uint32_t PA_SC_AA_MASK = 0x2312;
    inline constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380;
    inline constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2381;
    inline constexpr uint32_t PA_SU_POLY_OFFSET_BACK_SCALE = 0x2382;
    inline constexpr uint32_t PA_SU_POLY_OFFSET_BACK_OFFSET = 0x2383;

    // Resolves (EDRAM -> memory copies).
    inline constexpr uint32_t RB_COPY_CONTROL = 0x2318;
    inline constexpr uint32_t RB_COPY_DEST_BASE = 0x2319;
    inline constexpr uint32_t RB_COPY_DEST_PITCH = 0x231A;
    inline constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;
    inline constexpr uint32_t RB_TILE_CLEAR = 0x231C;
    inline constexpr uint32_t RB_DEPTH_CLEAR = 0x231D;
    inline constexpr uint32_t RB_COLOR_CLEAR = 0x231E;
    inline constexpr uint32_t RB_COLOR_CLEAR_LO = 0x231F;
    inline constexpr uint32_t RB_COPY_FUNC = 0x2320;
    inline constexpr uint32_t RB_COPY_REF = 0x2321;
    inline constexpr uint32_t RB_COPY_MASK = 0x2322;
    inline constexpr uint32_t RB_COPY_SURFACE_SLICE = 0x2323;
    inline constexpr uint32_t RB_SAMPLE_COUNT_CTL = 0x2324;
    inline constexpr uint32_t RB_SAMPLE_COUNT_ADDR = 0x2325;

    // Constant files.
    inline constexpr uint32_t SHADER_CONSTANT_000_X = 0x4000;     // 512 float4 (VS 0-255, PS 256-511)
    inline constexpr uint32_t SHADER_CONSTANT_FETCH_00_0 = 0x4800; // 32 fetch constants x 6 dwords
    inline constexpr uint32_t SHADER_CONSTANT_BOOL_000_031 = 0x4900; // 8 dwords = 256 bools
    inline constexpr uint32_t SHADER_CONSTANT_LOOP_00 = 0x4908;    // 32 loop constants
}

namespace gpu
{
    // VGT_DRAW_INITIATOR fields.
    struct DrawInitiator
    {
        uint32_t value;
        uint32_t PrimitiveType() const { return value & 0x3F; }
        uint32_t SourceSelect() const { return (value >> 6) & 0x3; } // 0 DMA (index buffer), 1 immediate, 2 auto-index
        bool Index32() const { return (value >> 11) & 1; }
        uint32_t NumIndices() const { return value >> 16; }
    };

    enum PrimitiveType : uint32_t
    {
        kPointList = 0x01,
        kLineList = 0x02,
        kLineStrip = 0x03,
        kTriangleList = 0x04,
        kTriangleFan = 0x05,
        kTriangleStrip = 0x06,
        kRectangleList = 0x08,
        kLineLoop = 0x0C,
        kQuadList = 0x0D,
        kQuadStrip = 0x0E,
    };

    // RB_MODECONTROL.edram_mode
    enum EdramMode : uint32_t
    {
        kEdramIgnore = 0,
        kEdramColorDepth = 4,
        kEdramDepth = 5,
        kEdramCopy = 6,
    };
}
