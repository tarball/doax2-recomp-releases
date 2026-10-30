// Vd* video exports: display queries, GPU engine setup and VdSwap, backed by the command processor.
#include "gpu/command_processor.h"
#include "guest_abi.h"
#include "kernel/status.h"
#include "kernel/video_mode.h"
#include "log.h"
#include "netplay/frame.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

namespace
{
    uint32_t VdInitializeEngines(uint32_t /*unknown*/, uint32_t /*callback*/, uint32_t /*argument*/, uint32_t /*pfpMicrocode*/, uint32_t /*meMicrocode*/)
    {
        gpu::Start();
        return 1;
    }

    void VdShutdownEngines() {}

    uint32_t VdGetGraphicsAsicID() { return 0x10; }
    uint32_t VdEnableDisableClockGating(uint32_t /*enable*/) { return 0; }
    uint32_t VdSetDisplayMode(uint32_t /*mode*/) { return 0; }
    uint32_t VdInitializeEDRAM() { return 0; }
    uint32_t VdRetrainEDRAM(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
    uint32_t VdRetrainEDRAMWorker(uint32_t) { return 0; }
    uint32_t VdIsHSIOTrainingSucceeded() { return 1; }
    uint32_t VdCallGraphicsNotificationRoutines(uint32_t, uint32_t) { return 0; }
    void VdSetSystemCommandBufferGpuIdentifierAddress(uint32_t) {}

    void VdQueryVideoMode(kernel::VideoMode* mode)
    {
        if (mode)
            kernel::FillVideoMode(mode);
    }

    // Bit 0: widescreen; bit 1: 720p or better; bit 2: 1080p.
    uint32_t VdQueryVideoFlags()
    {
        return 0x1 | 0x2;
    }

    // X_DISPLAY_INFO (0x58 bytes, layout as in Xenia): front buffer size, scaler source rect and
    // output size, display size, refresh rate and actual width.
    void VdGetCurrentDisplayInformation(uint8_t* info)
    {
        if (!info)
            return;
        const auto u16 = [&](uint32_t offset, uint32_t value) { *reinterpret_cast<be<uint16_t>*>(info + offset) = uint16_t(value); };
        const auto u32 = [&](uint32_t offset, uint32_t value) { *reinterpret_cast<be<uint32_t>*>(info + offset) = value; };
        memset(info, 0, 0x58);
        u16(0x00, kernel::kDisplayWidth);  // front buffer width
        u16(0x02, kernel::kDisplayHeight); // front buffer height
        u32(0x10, kernel::kDisplayWidth);  // scaler source rect x2
        u32(0x14, kernel::kDisplayHeight); // scaler source rect y2
        u32(0x18, kernel::kDisplayWidth);  // scaled output width
        u32(0x1C, kernel::kDisplayHeight); // scaled output height
        u16(0x3C, kernel::kDisplayWidth);  // display width
        u16(0x3E, kernel::kDisplayHeight); // display height
        *reinterpret_cast<be<float>*>(info + 0x40) = 60.0f;
        u16(0x4A, kernel::kDisplayWidth);  // actual display width
    }

    // The display's transfer function, which D3D's gamma ramp functions (DOA4's sub_827610D8 and DOAX2's sub_8270CD28,
    // and for piecewise linear ramps sub_827612C8 and sub_8270CF08) adjust every ramp the game sets for: they take its
    // values as sRGB, decode them to linear light and encode that for the display. Type 1 (sRGB, a PC monitor) re-encodes as sRGB, so ramps apply unchanged; 2 (an HDTV,
    // as over HDMI or component) and any other but 3 use BT.709's curve, which darkens shadows; 3 a power of 1/`power`.
    // DISPLAY_GAMMA=type[,power] overrides it.
    void VdGetCurrentDisplayGamma(be<uint32_t>* type, be<float>* power)
    {
        static const auto setting = [] {
            std::pair<uint32_t, float> value{ 2, 2.22222233f };
            if (const char* override = std::getenv(GAME_PREFIX "_DISPLAY_GAMMA"))
            {
                char* end = nullptr;
                value.first = uint32_t(std::strtoul(override, &end, 10));
                if (end && *end == ',')
                    value.second = std::strtof(end + 1, nullptr);
                LOG_INFO("Display gamma: type %u, power %g", value.first, value.second);
            }
            return value;
        }();
        if (type)
            *type = setting.first;
        if (power)
            *power = setting.second;
    }

    void VdInitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2)
    {
        gpu::InitializeRingBuffer(physicalAddress, sizeLog2);
    }

    void VdEnableRingBufferRPtrWriteBack(uint32_t physicalAddress, uint32_t blockLog2)
    {
        gpu::EnableReadPointerWriteBack(physicalAddress, blockLog2);
    }

    void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t userData)
    {
        gpu::SetInterruptCallback(callback, userData);
    }

    void VdGetSystemCommandBuffer(be<uint32_t>* first, be<uint32_t>* second)
    {
        if (first)
            *first = 0xBEEF0000;
        if (second)
            *second = 0xBEEF0001;
    }

    // Fills the scaler command buffer (the 9th argument) with NOPs and returns its length in dwords.
    uint32_t VdInitializeScalerCommandBuffer(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, be<uint32_t>* destination)
    {
        constexpr uint32_t kDwords = 0x1CC / 4;
        for (uint32_t i = 0; destination && i < kDwords; i++)
            destination[i] = gpu::kType2Nop;
        return kDwords;
    }

    // The caller frees the returned block with MmFreePhysicalMemory.
    uint32_t VdPersistDisplay(uint32_t /*unknown*/, be<uint32_t>* result)
    {
        if (result)
            *result = guest::AllocPhysical(64, 0x1000, guest::kPageReadWrite, 0, 0xFFFFFFFF, 32);
        return 0;
    }

    // A front buffer address as the GPU sees it. D3D passes CPU addresses (e.g. 0xE9264000 in the
    // 4 KB physical view, which is physical 0x09265000); resolves target the physical address.
    uint32_t GpuAddress(uint32_t address)
    {
        return address >= 0xA0000000 ? guest::PhysicalAddress(address) : address & 0x1FFFFFFF;
    }

    // Writes a 64-dword swap packet into the command space the caller reserved:
    //   [0] XE_SWAP header  [1] 'SWAP'  [2] front buffer physical address  [3] width  [4] height
    //   [5..10] the front buffer's texture fetch constant (format, tiling, pitch; base address made
    //   physical)  [11] 'FRAM'  [12] the game frame presenting  [13] its fight frame + 1, or 0 outside a fight
    //   (netplay::Frame, FightFrame: for frame dumps that count fight frames)  [14..63] NOPs
    uint32_t VdSwap(be<uint32_t>* buffer, const be<uint32_t>* fetchConstant, uint32_t, uint32_t, uint32_t, const be<uint32_t>* frontBuffer,
        const be<uint32_t>* /*textureFormat*/, const be<uint32_t>* /*colorSpace*/, const be<uint32_t>* width, const be<uint32_t>* height)
    {
        if (!buffer)
            return 0;
        buffer[0] = gpu::MakeType3(gpu::kOpcodeXeSwap, 63);
        buffer[1] = 0x53574150; // 'SWAP'
        buffer[2] = frontBuffer ? GpuAddress(*frontBuffer) : 0;
        buffer[3] = width ? uint32_t(*width) : 0;
        buffer[4] = height ? uint32_t(*height) : 0;
        for (int i = 0; i < 6; i++)
            buffer[5 + i] = fetchConstant ? uint32_t(fetchConstant[i]) : 0;
        if (fetchConstant)
        {
            const uint32_t dword1 = fetchConstant[1];
            buffer[6] = (dword1 & 0xFFF) | (GpuAddress(dword1 & 0xFFFFF000) & 0xFFFFF000);
        }
        buffer[11] = gpu::kSwapFrameTag;
        buffer[12] = uint32_t(netplay::Frame());
        const std::optional<uint64_t> fightFrame = netplay::FightFrame();
        buffer[13] = fightFrame ? uint32_t(*fightFrame + 1) : 0;
        for (int i = 14; i < 64; i++)
            buffer[i] = gpu::kType2Nop;
        gpu::NoteSwapRecorded();
        return 0;
    }
}

GUEST_EXPORT(VdInitializeEngines, VdInitializeEngines)
GUEST_EXPORT(VdShutdownEngines, VdShutdownEngines)
GUEST_EXPORT(VdGetGraphicsAsicID, VdGetGraphicsAsicID)
GUEST_EXPORT(VdEnableDisableClockGating, VdEnableDisableClockGating)
GUEST_EXPORT(VdSetDisplayMode, VdSetDisplayMode)
GUEST_EXPORT(VdInitializeEDRAM, VdInitializeEDRAM)
GUEST_EXPORT(VdRetrainEDRAM, VdRetrainEDRAM)
GUEST_EXPORT(VdRetrainEDRAMWorker, VdRetrainEDRAMWorker)
GUEST_EXPORT(VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded)
GUEST_EXPORT(VdCallGraphicsNotificationRoutines, VdCallGraphicsNotificationRoutines)
GUEST_EXPORT(VdSetSystemCommandBufferGpuIdentifierAddress, VdSetSystemCommandBufferGpuIdentifierAddress)
GUEST_EXPORT(VdQueryVideoMode, VdQueryVideoMode)
GUEST_EXPORT(VdQueryVideoFlags, VdQueryVideoFlags)
GUEST_EXPORT(VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation)
GUEST_EXPORT(VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma)
GUEST_EXPORT(VdInitializeRingBuffer, VdInitializeRingBuffer)
GUEST_EXPORT(VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack)
GUEST_EXPORT(VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback)
GUEST_EXPORT(VdGetSystemCommandBuffer, VdGetSystemCommandBuffer)
GUEST_EXPORT(VdInitializeScalerCommandBuffer, VdInitializeScalerCommandBuffer)
GUEST_EXPORT(VdPersistDisplay, VdPersistDisplay)
GUEST_EXPORT(VdSwap, VdSwap)
