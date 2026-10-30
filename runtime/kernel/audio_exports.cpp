// XAudio render driver exports. Driver handles are 0x41550000 | client index, as on the 360.
#include "audio/audio_system.h"
#include "guest_abi.h"
#include "xam/xam.h"

#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kDriverHandleBase = 0x41550000;

    int ClientIndex(uint32_t driver)
    {
        return (driver & 0xFFFF0000) == kDriverHandleBase ? int(driver & 0xFFFF) : -1;
    }

    // callbackInfo points at { callback, callback argument }.
    uint32_t XAudioRegisterRenderDriverClient(const be<uint32_t>* callbackInfo, be<uint32_t>* driver)
    {
        if (!callbackInfo || !driver)
            return xam::kErrorInvalidParameter;
        const int index = audio::RegisterClient(callbackInfo[0], callbackInfo[1]);
        if (index < 0)
            return xam::kErrorFunctionFailed;
        *driver = kDriverHandleBase | uint32_t(index);
        return xam::kErrorSuccess;
    }

    uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver)
    {
        audio::UnregisterClient(ClientIndex(driver));
        return xam::kErrorSuccess;
    }

    uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, uint32_t samples)
    {
        audio::SubmitFrame(ClientIndex(driver), samples);
        return xam::kErrorSuccess;
    }

    // Analog stereo (bit 0) and Dolby Digital 5.1 (bit 16), as Xenia reports; SPEAKERS=stereo reports
    // stereo only. DOA4 then mixes for two speakers, and plays its movies' stereo soundtracks instead of 5.1.
    uint32_t XAudioGetSpeakerConfig(be<uint32_t>* config)
    {
        static const bool stereo = [] {
            const char* setting = std::getenv(GAME_PREFIX "_SPEAKERS");
            return setting && std::strcmp(setting, "stereo") == 0;
        }();
        if (config)
            *config = stereo ? 0x00000001 : 0x00010001;
        return xam::kErrorSuccess;
    }

    uint32_t XAudioGetVoiceCategoryVolumeChangeMask(uint32_t /*driver*/, be<uint32_t>* mask)
    {
        if (mask)
            *mask = 0; // no volume changes
        return xam::kErrorSuccess;
    }

    uint32_t XAudioGetVoiceCategoryVolume(uint32_t /*category*/, be<float>* volume)
    {
        if (volume)
            *volume = 1.0f;
        return xam::kErrorSuccess;
    }
}

GUEST_EXPORT(XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient)
GUEST_EXPORT(XAudioUnregisterRenderDriverClient, XAudioUnregisterRenderDriverClient)
GUEST_EXPORT(XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame)
GUEST_EXPORT(XAudioGetSpeakerConfig, XAudioGetSpeakerConfig)
GUEST_EXPORT(XAudioGetVoiceCategoryVolumeChangeMask, XAudioGetVoiceCategoryVolumeChangeMask)
GUEST_EXPORT(XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume)
