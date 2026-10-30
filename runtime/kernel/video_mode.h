#pragma once

#include "guest_abi.h"

#include <cstring>

namespace kernel
{
    // X_VIDEO_MODE, shared by XGetVideoMode and VdQueryVideoMode: 720p widescreen at 60 Hz.
    struct VideoMode
    {
        be<uint32_t> displayWidth;
        be<uint32_t> displayHeight;
        be<uint32_t> isInterlaced;
        be<uint32_t> isWidescreen;
        be<uint32_t> isHiDef;
        be<float> refreshRate;
        be<uint32_t> videoStandard;
        be<uint32_t> unknown0x8A;
        be<uint32_t> unknown0x01;
        be<uint32_t> reserved[3];
    };
    static_assert(sizeof(VideoMode) == 0x30);

    inline constexpr uint32_t kDisplayWidth = 1280;
    inline constexpr uint32_t kDisplayHeight = 720;

    inline void FillVideoMode(VideoMode* mode)
    {
        memset(mode, 0, sizeof(*mode));
        mode->displayWidth = kDisplayWidth;
        mode->displayHeight = kDisplayHeight;
        mode->isInterlaced = 0;
        mode->isWidescreen = 1;
        mode->isHiDef = 1;
        mode->refreshRate = 60.0f;
        mode->videoStandard = 1; // NTSC
        mode->unknown0x8A = 0x4A;
        mode->unknown0x01 = 0x01;
    }
}
