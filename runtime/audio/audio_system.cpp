#include "audio/audio_system.h"

#include "config.h"
#include "guest_memory.h"
#include "kernel/threads.h"
#include "log.h"
#include "util/profiler.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

namespace audio
{
    namespace
    {
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined here to avoid ksmedia.h's GUID-definition setup.
        constexpr GUID kSubtypeIeeeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

        constexpr int kMaxClients = 8;
        constexpr uint32_t kFrameFloats = kFrameSamples * kChannels;
        constexpr uint32_t kQueueDepth = 4;     // frames buffered in the host voice
        constexpr uint32_t kBufferCount = 16;   // ring of host frame buffers (> queue depth)

        // AUDIO_DUMP=1 (diagnostics) writes the frames clients submit to <data>/dumps/audio.wav (6 channels
        // of 32-bit float, 48 kHz). The header's sizes are kept current, so the file is complete even when the
        // process is killed.
        class Dump
        {
        public:
            static Dump* Get()
            {
                static Dump* dump = [] {
                    const char* setting = std::getenv(GAME_PREFIX "_AUDIO_DUMP");
                    if (!setting || std::atoi(setting) == 0)
                        return static_cast<Dump*>(nullptr);
                    std::error_code error;
                    std::filesystem::create_directories(config::DataDirectory() + "/dumps", error);
                    FILE* file = fopen((config::DataDirectory() + "/dumps/audio.wav").c_str(), "wb");
                    return file ? new Dump(file) : nullptr;
                }();
                return dump;
            }

            void Write(const float* interleaved, uint32_t floats)
            {
                fwrite(interleaved, 4, floats, m_file);
                m_bytes += floats * 4;
                WriteHeader();
            }

            double Seconds() const { return double(m_bytes) / (kSampleRate * kChannels * 4); }

        private:
            explicit Dump(FILE* file) : m_file(file) { WriteHeader(); }

            void WriteHeader()
            {
                struct Header
                {
                    char riff[4]; uint32_t riffSize; char wave[4];
                    char fmt[4]; uint32_t fmtSize; uint16_t format, channels; uint32_t rate, byteRate; uint16_t blockAlign, bits;
                    char data[4]; uint32_t dataSize;
                } header{ { 'R', 'I', 'F', 'F' }, 36 + m_bytes, { 'W', 'A', 'V', 'E' }, { 'f', 'm', 't', ' ' }, 16, 3, uint16_t(kChannels), kSampleRate,
                    kSampleRate * kChannels * 4, uint16_t(kChannels * 4), 32, { 'd', 'a', 't', 'a' }, m_bytes };
                static_assert(sizeof(Header) == 44);
                const long end = ftell(m_file);
                fseek(m_file, 0, SEEK_SET);
                fwrite(&header, sizeof(header), 1, m_file);
                fseek(m_file, end > long(sizeof(header)) ? end : long(sizeof(header)), SEEK_SET);
                fflush(m_file);
            }

            FILE* m_file;
            std::atomic<uint32_t> m_bytes{ 0 };
        };

        // XAudio2 stops for good when its device goes away (unplugged or switched, or a remote session's audio
        // reconnecting), and says so here.
        class EngineCallback : public IXAudio2EngineCallback
        {
        public:
            void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {}
            void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {}
            void STDMETHODCALLTYPE OnCriticalError(HRESULT error) noexcept override { m_error = uint32_t(error); }
            uint32_t Error() const { return m_error; }

        private:
            std::atomic<uint32_t> m_error{ 0 };
        };

        // One host output voice per client, fed with interleaved little-endian floats.
        class Output
        {
        public:
            bool Initialize()
            {
                if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) && !m_comFallback)
                    m_comFallback = true; // already initialized differently; XAudio2 still works
                if (FAILED(XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
                    return false;
                m_xaudio->RegisterForCallbacks(&m_callback);
                if (FAILED(m_xaudio->CreateMasteringVoice(&m_master)))
                    return false;

                WAVEFORMATEXTENSIBLE format{};
                format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                format.Format.nChannels = kChannels;
                format.Format.nSamplesPerSec = kSampleRate;
                format.Format.wBitsPerSample = 32;
                format.Format.nBlockAlign = kChannels * 4;
                format.Format.nAvgBytesPerSec = kSampleRate * format.Format.nBlockAlign;
                format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                format.Samples.wValidBitsPerSample = 32;
                // The 360's 5.1 order (FL, FR, C, LFE, RL, RR) matches the Windows channel order.
                format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                    SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
                format.SubFormat = kSubtypeIeeeFloat;

                if (FAILED(m_xaudio->CreateSourceVoice(&m_voice, &format.Format)))
                    return false;
                m_voice->Start();
                return true;
            }

            ~Output()
            {
                if (m_voice)
                    m_voice->DestroyVoice();
                if (m_master)
                    m_master->DestroyVoice();
                if (m_xaudio)
                    m_xaudio->Release();
            }

            uint32_t Queued() const
            {
                XAUDIO2_VOICE_STATE state;
                m_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                return state.BuffersQueued;
            }

            // The critical error that stopped the engine, or 0.
            uint32_t Error() const { return m_callback.Error(); }

            void Submit(const float* planarBigEndian)
            {
                float* out = m_buffers[m_next].data();
                m_next = (m_next + 1) % kBufferCount;
                const auto* in = reinterpret_cast<const uint32_t*>(planarBigEndian);
                for (uint32_t sample = 0; sample < kFrameSamples; sample++)
                {
                    for (uint32_t channel = 0; channel < kChannels; channel++)
                    {
                        const uint32_t bits = __builtin_bswap32(in[channel * kFrameSamples + sample]);
                        memcpy(&out[sample * kChannels + channel], &bits, 4);
                    }
                }
                XAUDIO2_BUFFER buffer{};
                buffer.AudioBytes = kFrameFloats * 4;
                buffer.pAudioData = reinterpret_cast<const BYTE*>(out);
                m_voice->SubmitSourceBuffer(&buffer);
                if (Dump* dump = Dump::Get())
                    dump->Write(out, kFrameFloats);
            }

        private:
            EngineCallback m_callback; // outlives the engine, released first in the destructor
            IXAudio2* m_xaudio = nullptr;
            IXAudio2MasteringVoice* m_master = nullptr;
            IXAudio2SourceVoice* m_voice = nullptr;
            std::array<std::array<float, kFrameFloats>, kBufferCount> m_buffers{};
            uint32_t m_next = 0;
            bool m_comFallback = false;
        };

        struct Client
        {
            bool active = false;
            uint32_t callback = 0;
            uint32_t callbackArgument = 0;
            std::atomic<int> pending{ 0 }; // callbacks posted but not yet answered with a frame
            std::unique_ptr<Output> output;
            // Without host output: when the next frame is due (real time), and when to try opening the output again.
            std::chrono::steady_clock::time_point nextFrame{}, retry{};
        };

        // Opens a client's host output. Returns whether it has one; without, it runs silently in real time.
        bool OpenOutput(Client& client)
        {
            auto output = std::make_unique<Output>();
            if (!output->Initialize())
                return false;
            client.output = std::move(output);
            return true;
        }

        struct State
        {
            std::mutex lock;
            std::array<Client, kMaxClients> clients;
            std::unique_ptr<kernel::GuestCallQueue> callbacks;
            bool started = false;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        // Asks each client for frames, keeping each host voice kQueueDepth frames ahead. Without
        // host output, frames are requested at the real-time rate instead, and the output is reopened
        // every 2 s (it's lost when its device goes away).
        void DriverThread()
        {
            using namespace std::chrono;
            profile::NameThread("Audio driver");
            State& state = GetState();
            const auto framePeriod = duration_cast<steady_clock::duration>(duration<double>(double(kFrameSamples) / kSampleRate));
            auto nextTick = steady_clock::now();
            for (;;)
            {
                nextTick += framePeriod / 2;
                std::this_thread::sleep_until(nextTick);
                const auto now = steady_clock::now();

                std::lock_guard lock(state.lock);
                for (Client& client : state.clients)
                {
                    if (!client.active)
                        continue;
                    if (client.output && client.output->Error())
                    {
                        LOG_WARN("Audio: host output lost (XAudio2 error 0x%08X); running silently until it's back", client.output->Error());
                        client.output.reset();
                        client.retry = now;
                        client.nextFrame = now;
                    }
                    if (!client.output && now >= client.retry)
                    {
                        if (OpenOutput(client))
                            LOG_INFO("Audio: host output is back");
                        else
                            client.retry = now + seconds(2);
                    }
                    if (client.pending > 0)
                        continue;
                    bool wanted;
                    if (client.output)
                        wanted = client.output->Queued() < kQueueDepth;
                    else
                    {
                        wanted = now >= client.nextFrame;
                        if (wanted)
                            client.nextFrame = std::max(client.nextFrame + framePeriod, now - kQueueDepth * framePeriod);
                    }
                    if (wanted)
                    {
                        client.pending++;
                        state.callbacks->Post(client.callback, { client.callbackArgument });
                    }
                }
            }
        }
    }

    int RegisterClient(uint32_t callback, uint32_t callbackArgument)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        if (!state.started)
        {
            state.started = true;
            state.callbacks = std::make_unique<kernel::GuestCallQueue>("audio driver");
            std::thread(DriverThread).detach();
        }

        for (int i = 0; i < kMaxClients; i++)
        {
            Client& client = state.clients[i];
            if (client.active)
                continue;
            client.active = true;
            client.callback = callback;
            client.callbackArgument = callbackArgument;
            client.pending = 0;
            client.nextFrame = std::chrono::steady_clock::now();
            client.retry = client.nextFrame + std::chrono::seconds(2);
            if (!OpenOutput(client))
                LOG_WARN("Audio: no host output (XAudio2 unavailable); running silently in real time until there is");
            LOG_INFO("Audio: client %d registered (callback 0x%08X, argument 0x%08X)", i, callback, callbackArgument);
            return i;
        }
        return -1;
    }

    void UnregisterClient(int index)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        if (index >= 0 && index < kMaxClients)
        {
            state.clients[index].active = false;
            state.clients[index].output.reset();
        }
    }

    double DumpedSeconds()
    {
        const Dump* dump = Dump::Get();
        return dump ? dump->Seconds() : -1.0;
    }

    namespace
    {
        std::atomic<uint64_t> g_submittedFrames{ 0 };
    }

    uint64_t SubmittedFrames()
    {
        return g_submittedFrames;
    }

    void SubmitFrame(int index, uint32_t samples)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        if (index < 0 || index >= kMaxClients || !state.clients[index].active)
            return;
        g_submittedFrames++;
        Client& client = state.clients[index];
        if (client.pending > 0)
            client.pending--;
        if (client.output && samples)
            client.output->Submit(guest::Translate<const float>(samples));
    }
}
