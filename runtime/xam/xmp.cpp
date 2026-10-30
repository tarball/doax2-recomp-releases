// The music player (XMP), XAM app 0xFA, reached through XMsg. A title makes playlists of its own songs and plays them
// through it (DOAX2's radio: 18 MP3s on the disc, played by XACT's music engine), or leaves the player's own music on.
// This keeps the player's state as Xenia does (state, playlists, current song, volume, playback behavior and controller)
// and notifies listeners of the XMP area.
//
// Playing: a title that mixes the music itself gives XMP somewhere to play it (0x7003D: a callback and its context).
// The player decodes the current song with FFmpeg and calls callback(packet, context, 2048) from a guest thread for each
// audio frame's worth: 256 samples of 48 kHz stereo 32-bit float (big endian), 2048 bytes, the format XAudio mixes in.
// DOAX2's callback submits the packet, 2048 bytes whatever the size, to an XAudio voice it made for that format. Packets
// go a few frames ahead of the game's audio output (its clock: the frames it submits), and the voice reads a packet
// after the call, so they are a ring of 32 in guest memory. At a song's end the player goes on to the next (in order or
// shuffled), or stops at the playlist's end without repeat.
//
// Message layouts are XDK 2.0.3529's (DOAX2's XMP library); each starts with the client (2 for the title). A playlist's
// handle is the storage the library allocated for it (4 bytes + 128 per song, as asked with 0x7002E), which the
// library frees once the playlist is deleted.
#include "audio/audio_system.h"
#include "guest_abi.h"
#include "kernel/threads.h"
#include "log.h"
#include "util/profiler.h"
#include "vfs/vfs.h"
#include "xam/xam.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if HAVE_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#endif

namespace
{
    constexpr uint32_t kStateChanged = 0x0A000001;      // XN_XMP_STATECHANGED, with the new state
    constexpr uint32_t kBehaviorChanged = 0x0A000002;   // XN_XMP_PLAYBACKBEHAVIORCHANGED
    constexpr uint32_t kControllerChanged = 0x0A000003; // XN_XMP_PLAYBACKCONTROLLERCHANGED

    constexpr uint32_t kFail = 0x80004005;     // E_FAIL
    constexpr uint32_t kNotFound = 0x80070490; // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)

    enum class State : uint32_t
    {
        Idle = 0,
        Playing = 1,
        Paused = 2,
    };

    // From an XMP_SONGDESCRIPTOR: file path, title, artist, album, album artist and genre (UTF-16 strings), track
    // number, duration (ms) and format.
    struct Song
    {
        std::u16string path, title, artist, album, albumArtist, genre;
        uint32_t trackNumber = 0;
        uint32_t duration = 0;
        uint32_t handle = 0;
    };

    struct Player
    {
        std::mutex lock;
        State state = State::Idle;
        std::map<uint32_t, std::vector<Song>> playlists; // by handle
        uint32_t playlist = 0;                           // the playing playlist's handle, or 0
        size_t song = 0;
        uint64_t songStarts = 0; // counts the songs started (played, skipped to, or reached), so playback restarts
        float volume = 1.0f;
        uint32_t playbackMode = 0, repeatMode = 0, behaviorFlags = 0; // mode: 0 in order, 1 shuffle; repeat: 0 playlist, 1 none
        uint32_t playbackClient = 0;
        uint32_t captureCallback = 0, captureContext = 0;
    };

    Player& GetPlayer()
    {
        static Player player;
        return player;
    }

    std::u16string ReadWide(uint32_t address)
    {
        std::u16string text;
        for (const auto* c = guest::Translate<be<uint16_t>>(address); c && *c != 0 && text.size() < 1024; c++)
            text.push_back(char16_t(uint16_t(*c)));
        return text;
    }

    // For the log: ASCII, others as '?'.
    std::string Narrow(const std::u16string& text)
    {
        std::string result;
        for (char16_t c : text)
            result.push_back(c >= 0x20 && c < 0x7F ? char(c) : '?');
        return result;
    }

    // A string into a fixed field of `capacity` UTF-16 characters, truncated, with its terminator.
    void WriteWide(uint32_t address, const std::u16string& text, size_t capacity)
    {
        auto* out = guest::Translate<be<uint16_t>>(address);
        const size_t length = std::min(text.size(), capacity - 1);
        for (size_t i = 0; i < length; i++)
            out[i] = uint16_t(text[i]);
        out[length] = 0;
    }

    void StoreFloat(uint32_t address, float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, 4);
        guest::StoreU32(address, bits);
    }

    float LoadFloat(uint32_t address)
    {
        const uint32_t bits = guest::LoadU32(address);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }

    // The next song at a song's end, or a stop at the playlist's end without repeat. Under the player's lock.
    void AdvanceSong(Player& player, std::vector<std::pair<uint32_t, uint32_t>>& notifications)
    {
        const auto it = player.playlists.find(player.playlist);
        const size_t count = it != player.playlists.end() ? it->second.size() : 0;
        if (count && player.playbackMode == 1)
        {
            static std::mt19937 random{ std::random_device{}() };
            player.song = std::uniform_int_distribution<size_t>(0, count - 1)(random);
        }
        else if (player.song + 1 < count)
            player.song++;
        else if (count && player.repeatMode == 0)
            player.song = 0;
        else
        {
            player.playlist = 0;
            player.song = 0;
            player.state = State::Idle;
            notifications.emplace_back(kStateChanged, uint32_t(State::Idle));
        }
        player.songStarts++;
    }

#if HAVE_FFMPEG
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kPacketSamples = audio::kFrameSamples; // an audio frame: 2048 bytes of stereo float
    constexpr uint32_t kPacketBytes = kPacketSamples * 2 * 4;
    constexpr uint32_t kRingPackets = 32;
    constexpr uint32_t kLeadPackets = 4; // submitted ahead of the game's audio output

    // A song (an MP3 on the disc) decoded to 48 kHz stereo float samples.
    class SongDecoder
    {
    public:
        ~SongDecoder() { Close(); }

        bool Open(const std::string& guestPath)
        {
            Close();
            std::string relative;
            vfs::Device* device = vfs::Resolve(guestPath, relative);
            vfs::OpenRequest request;
            request.path = relative;
            std::unique_ptr<vfs::File> file;
            vfs::OpenResult result;
            if (!device || device->Open(request, file, result) != 0)
                return false;
            const uint64_t size = file->Info().size;
            uint32_t transferred = 0;
            m_file.assign(size + AV_INPUT_BUFFER_PADDING_SIZE, 0); // the parser reads past the end
            m_size = size_t(size);
            if (size > 64u << 20 || file->Read(m_file.data(), uint32_t(size), 0, transferred) != 0 || transferred != size)
                return false;
            const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
            m_parser = av_parser_init(AV_CODEC_ID_MP3);
            m_codec = codec ? avcodec_alloc_context3(codec) : nullptr;
            m_packet = av_packet_alloc();
            m_frame = av_frame_alloc();
            if (!m_parser || !m_codec || !m_packet || !m_frame || avcodec_open2(m_codec, codec, nullptr) < 0)
            {
                Close();
                return false;
            }
            return true;
        }

        // Fills `out` with up to `count` stereo samples (interleaved); fewer only at the song's end.
        size_t Read(float* out, size_t count)
        {
            size_t done = 0;
            while (done < count)
            {
                const size_t available = (m_pending.size() - m_pendingOffset) / 2;
                if (available)
                {
                    const size_t n = std::min(available, count - done);
                    memcpy(out + done * 2, m_pending.data() + m_pendingOffset, n * 2 * sizeof(float));
                    m_pendingOffset += n * 2;
                    done += n;
                    continue;
                }
                m_pending.clear();
                m_pendingOffset = 0;
                if (!Decode())
                    break;
            }
            return done;
        }

    private:
        void Close()
        {
            av_parser_close(m_parser);
            m_parser = nullptr;
            avcodec_free_context(&m_codec);
            av_packet_free(&m_packet);
            av_frame_free(&m_frame);
            swr_free(&m_resampler);
            m_file.clear();
            m_size = m_offset = 0;
            m_parserFlushed = m_decoderFlushed = m_resamplerFlushed = false;
            m_pending.clear();
            m_pendingOffset = 0;
        }

        // Decodes more of the song into m_pending. Returns false at its end.
        bool Decode()
        {
            while (m_pending.empty())
            {
                const int received = avcodec_receive_frame(m_codec, m_frame);
                if (received == 0)
                {
                    Resample(m_frame);
                    av_frame_unref(m_frame);
                    continue;
                }
                if (received == AVERROR_EOF)
                {
                    if (m_resamplerFlushed || !m_resampler)
                        return false;
                    m_resamplerFlushed = true;
                    Resample(nullptr); // what the resampler holds back
                    return !m_pending.empty();
                }
                if (received != AVERROR(EAGAIN))
                    return false;
                if (m_parserFlushed)
                {
                    if (m_decoderFlushed)
                        return false;
                    m_decoderFlushed = true;
                    avcodec_send_packet(m_codec, nullptr);
                    continue;
                }
                // The next MP3 frame, as the parser splits them (a last call without data flushes it).
                uint8_t* data = nullptr;
                int size = 0;
                const int remaining = int(std::min<size_t>(m_size - m_offset, 1 << 16));
                const int used = av_parser_parse2(m_parser, m_codec, &data, &size, remaining ? m_file.data() + m_offset : nullptr, remaining,
                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                if (used < 0)
                    return false;
                m_offset += size_t(used);
                if (!remaining)
                    m_parserFlushed = true;
                if (size)
                {
                    m_packet->data = data;
                    m_packet->size = size;
                    avcodec_send_packet(m_codec, m_packet); // a broken frame is skipped
                }
            }
            return true;
        }

        void Resample(const AVFrame* frame)
        {
            if (frame && !m_resampler)
            {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&m_resampler, &stereo, AV_SAMPLE_FMT_FLT, kSampleRate, &frame->ch_layout, AVSampleFormat(frame->format),
                        frame->sample_rate, 0, nullptr) < 0 ||
                    swr_init(m_resampler) < 0)
                {
                    swr_free(&m_resampler);
                    return;
                }
            }
            if (!m_resampler)
                return;
            const int capacity = swr_get_out_samples(m_resampler, frame ? frame->nb_samples : 0);
            if (capacity <= 0)
                return;
            m_pending.resize(size_t(capacity) * 2);
            uint8_t* out = reinterpret_cast<uint8_t*>(m_pending.data());
            const int converted = swr_convert(m_resampler, &out, capacity, frame ? const_cast<const uint8_t**>(frame->extended_data) : nullptr,
                frame ? frame->nb_samples : 0);
            m_pending.resize(size_t(std::max(converted, 0)) * 2);
        }

        std::vector<uint8_t> m_file;
        size_t m_size = 0, m_offset = 0;
        AVCodecParserContext* m_parser = nullptr;
        AVCodecContext* m_codec = nullptr;
        AVPacket* m_packet = nullptr;
        AVFrame* m_frame = nullptr;
        SwrContext* m_resampler = nullptr;
        bool m_parserFlushed = false, m_decoderFlushed = false, m_resamplerFlushed = false;
        std::vector<float> m_pending; // decoded, not yet read
        size_t m_pendingOffset = 0;
    };

    // Feeds the current song to the title's callback, kLeadPackets ahead of the game's audio output.
    void PlaybackThread()
    {
        using namespace std::chrono;
        profile::NameThread("Music player");
        Player& player = GetPlayer();
        kernel::GuestCallQueue calls("music player");
        const uint32_t ring = guest::SystemAlloc(kRingPackets * kPacketBytes);
        SongDecoder decoder;
        uint64_t decoding = 0; // songStarts of the song decoded
        uint64_t packets = 0;  // submitted
        const uint64_t start = audio::SubmittedFrames();
        std::vector<float> samples(kPacketSamples * 2);
        for (;;)
        {
            std::this_thread::sleep_for(milliseconds(5));
            const uint64_t due = audio::SubmittedFrames() - start + kLeadPackets;
            while (packets < due)
            {
                std::string path;
                uint64_t songStarts;
                float volume;
                uint32_t callback, context;
                {
                    std::lock_guard lock(player.lock);
                    const auto it = player.playlists.find(player.playlist);
                    if (player.state != State::Playing || !player.captureCallback || it == player.playlists.end() || player.song >= it->second.size())
                    {
                        packets = due; // keeps time while there's nothing to play
                        break;
                    }
                    path = Narrow(it->second[player.song].path);
                    songStarts = player.songStarts;
                    volume = std::clamp(player.volume, 0.0f, 1.0f);
                    callback = player.captureCallback;
                    context = player.captureContext;
                }
                std::vector<std::pair<uint32_t, uint32_t>> notifications;
                if (songStarts != decoding)
                {
                    decoding = songStarts;
                    if (!decoder.Open(path))
                    {
                        LOG_WARN("XMP: can't play %s", path.c_str());
                        std::lock_guard lock(player.lock);
                        if (player.songStarts == songStarts)
                            AdvanceSong(player, notifications);
                    }
                    else
                        LOG_INFO("XMP: decoding %s", path.c_str());
                }
                const size_t got = notifications.empty() ? decoder.Read(samples.data(), kPacketSamples) : 0;
                if (got < kPacketSamples && notifications.empty())
                {
                    std::lock_guard lock(player.lock);
                    if (player.songStarts == songStarts)
                        AdvanceSong(player, notifications);
                }
                for (const auto& [id, parameter] : notifications)
                    xam::BroadcastNotification(id, parameter);
                if (!got)
                    continue;
                std::fill(samples.begin() + got * 2, samples.end(), 0.0f);
                auto* packet = guest::Translate<be<uint32_t>>(ring + uint32_t(packets % kRingPackets) * kPacketBytes);
                for (size_t i = 0; i < samples.size(); i++)
                {
                    uint32_t bits;
                    const float sample = samples[i] * volume;
                    memcpy(&bits, &sample, 4);
                    packet[i] = bits;
                }
                calls.Post(callback, { ring + uint32_t(packets % kRingPackets) * kPacketBytes, context, kPacketBytes });
                packets++;
            }
        }
    }
#endif

    // Starts playing into the title's callback (once it has given one).
    void StartPlayback()
    {
#if HAVE_FFMPEG
        static std::once_flag started;
        std::call_once(started, [] { std::thread(PlaybackThread).detach(); });
#else
        static bool reported = false;
        if (!std::exchange(reported, true))
            LOG_WARN("XMP: no FFmpeg, so songs don't play");
#endif
    }
}

namespace xam
{
    uint32_t XmpDispatch(uint32_t message, uint32_t buffer, uint32_t /*length*/)
    {
        const auto argument = [buffer](uint32_t index) { return guest::LoadU32(buffer + index * 4); };
        Player& player = GetPlayer();
        std::vector<std::pair<uint32_t, uint32_t>> notifications; // sent once the player is unlocked
        uint32_t result = kErrorSuccess;
        {
            std::lock_guard lock(player.lock);
            const auto setState = [&](State state) {
                player.state = state;
                notifications.emplace_back(kStateChanged, uint32_t(state));
            };
            const auto stop = [&] {
                player.playlist = 0;
                player.song = 0;
                player.songStarts++;
                setState(State::Idle);
            };

            switch (message)
            {
            case 0x00070002: // XMPPlayTitlePlaylist { client, playlist, song (0: the first) }
            {
                const auto it = player.playlists.find(argument(1));
                if (it == player.playlists.end())
                {
                    result = kNotFound;
                    break;
                }
                const auto song = std::find_if(it->second.begin(), it->second.end(), [&](const Song& s) { return s.handle == argument(2); });
                player.playlist = it->first;
                player.song = song != it->second.end() ? size_t(song - it->second.begin()) : 0;
                player.songStarts++;
                LOG_INFO("XMP: playing playlist %08X from song %zu (\"%s\")", player.playlist, player.song,
                    it->second.empty() ? "" : Narrow(it->second[player.song].title).c_str());
                setState(State::Playing);
                break;
            }
            case 0x00070003: // XMPContinue { client }
                if (player.state == State::Paused)
                    setState(State::Playing);
                break;
            case 0x00070004: // XMPStop { client, flags }
                stop();
                break;
            case 0x00070005: // XMPPause { client }
                if (player.state == State::Playing)
                    setState(State::Paused);
                break;
            case 0x00070006: // XMPNext { client }
            case 0x00070007: // XMPPrevious { client }
            {
                const auto it = player.playlists.find(player.playlist);
                if (it == player.playlists.end() || it->second.empty())
                {
                    result = kFail;
                    break;
                }
                const size_t count = it->second.size();
                player.song = message == 0x00070006 ? (player.song + 1) % count : (player.song + count - 1) % count;
                player.songStarts++;
                setState(player.state);
                break;
            }
            case 0x00070008: // XMPSetPlaybackBehavior { client, mode, repeat mode, flags }
                player.playbackMode = argument(1);
                player.repeatMode = argument(2);
                player.behaviorFlags = argument(3);
                notifications.emplace_back(kBehaviorChanged, 0);
                break;
            case 0x00070009: // XMPGetStatus { client, &state }
                if (argument(1))
                    guest::StoreU32(argument(1), uint32_t(player.state));
                break;
            case 0x0007000B: // XMPGetVolume { client, &volume }
                if (argument(1))
                    StoreFloat(argument(1), player.volume);
                break;
            case 0x0007000C: // XMPSetVolume { client, volume }
                player.volume = LoadFloat(buffer + 4);
                break;
            case 0x0007000D: // XMPCreateTitlePlaylist { client, storage, storage size, songs, count, name, flags, &song handles, &playlist }
            {
                const uint32_t storage = argument(1), songs = argument(3), count = argument(4), songHandles = argument(7), handle = argument(8);
                if (!storage || argument(2) < 4 + count * 128)
                {
                    result = kFail;
                    break;
                }
                std::vector<Song>& playlist = player.playlists[storage];
                playlist.clear();
                for (uint32_t i = 0; i < count; i++)
                {
                    const uint32_t descriptor = songs + i * 36;
                    const auto field = [descriptor](uint32_t index) { return guest::LoadU32(descriptor + index * 4); };
                    Song song;
                    song.path = ReadWide(field(0));
                    song.title = ReadWide(field(1));
                    song.artist = ReadWide(field(2));
                    song.album = ReadWide(field(3));
                    song.albumArtist = ReadWide(field(4));
                    song.genre = ReadWide(field(5));
                    song.trackNumber = field(6);
                    song.duration = field(7);
                    song.handle = storage + 4 + i * 128; // unique, nonzero
                    if (songHandles)
                        guest::StoreU32(songHandles + i * 4, song.handle);
                    playlist.push_back(std::move(song));
                }
                if (handle)
                    guest::StoreU32(handle, storage);
                LOG_INFO("XMP: playlist %08X \"%s\": %u songs, the first %s", storage, Narrow(ReadWide(argument(5))).c_str(), count,
                    playlist.empty() ? "-" : Narrow(playlist[0].path).c_str());
                break;
            }
            case 0x0007000E: // XMPGetInfo { client, 0, &info }: the current song
            {
                const auto it = player.playlists.find(player.playlist);
                const uint32_t info = argument(2);
                if (it == player.playlists.end() || player.song >= it->second.size() || !info)
                {
                    result = kFail;
                    break;
                }
                // Title, artist, album, album artist and genre, 40 characters each from offset 576, then the track
                // number and the duration (DOAX2's music engine reads these).
                const Song& song = it->second[player.song];
                guest::BeforeHostWrite(info, 984);
                memset(guest::ToHost(info), 0, 984);
                const std::u16string* strings[] = { &song.title, &song.artist, &song.album, &song.albumArtist, &song.genre };
                for (uint32_t i = 0; i < 5; i++)
                    WriteWide(info + 576 + i * 80, *strings[i], 40);
                guest::StoreU32(info + 976, song.trackNumber);
                guest::StoreU32(info + 980, song.duration);
                break;
            }
            case 0x00070013: // XMPDeleteTitlePlaylist { client, playlist }
                if (!player.playlists.erase(argument(1)))
                {
                    result = kNotFound;
                    break;
                }
                if (player.playlist == argument(1))
                    stop();
                break;
            case 0x0007001A: // XMPSetPlaybackController { client, controller, playback client }
                player.playbackClient = argument(2);
                notifications.emplace_back(kControllerChanged, player.playbackClient ? 0 : 1);
                break;
            case 0x0007001B: // XMPGetPlaybackController { client, &controller, &locked }: the title has control
                if (argument(1))
                    guest::StoreU32(argument(1), 0);
                if (argument(2))
                    guest::StoreU32(argument(2), 0);
                break;
            case 0x00070029: // XMPGetPlaybackBehavior { client, &mode, &repeat mode, &flags }
                if (argument(1))
                    guest::StoreU32(argument(1), player.playbackMode);
                if (argument(2))
                    guest::StoreU32(argument(2), player.repeatMode);
                if (argument(3))
                    guest::StoreU32(argument(3), player.behaviorFlags);
                break;
            case 0x0007002E: // the storage a playlist needs { client, songs, &size }
                if (argument(2))
                    guest::StoreU32(argument(2), 4 + argument(1) * 128);
                break;
            case 0x00070025: // the user's playlists, into an enumerator's shared structure { client, flags, structure }: none
                LOG_INFO("XMP: the user's playlists were asked for: there are none");
                break;
            case 0x0007003B: // play one of the user's playlists { client, its 820-byte description }: there are none
                LOG_WARN("XMP: a user playlist was asked for, and there are none");
                result = kNotFound;
                break;
            case 0x0007003D: // the title's sink for the music { client, callback, context, flags }
                player.captureCallback = argument(1);
                player.captureContext = argument(2);
                LOG_INFO("XMP: music goes to callback %08X (context %08X, flags %u)", argument(1), argument(2), argument(3));
                if (player.captureCallback)
                    StartPlayback();
                break;
            default:
                LOG_WARN("XMP: unhandled message 0x%08X (buffer 0x%08X)", message, buffer);
                result = kNotFound;
                break;
            }
        }
        for (const auto& [id, parameter] : notifications)
            BroadcastNotification(id, parameter);
        return result;
    }
}
