// XMA decoder contexts (the Xbox 360's hardware audio decoder) and the XMA* kernel exports.
//
// A context is 64 bytes of guest physical memory (layout as in Xenia's XMA_CONTEXT_DATA): two
// double-buffered input buffers of 2 KB XMA packets, an output ring of 256-byte blocks of 16-bit
// PCM, and loop data. The hardware decodes enabled contexts asynchronously; games refill input
// buffers as they are consumed and read PCM from the output ring.
//
// XMA data is a stream of frames (each starting with its length in bits) in 2 KB packets, which frames
// continue across after the next packet's 32-bit header. A worker thread reads an enabled context's
// frames in order, across packets and from one input buffer into the other, following its loop (the
// read offset jumps from the loop end back to the loop start), and decodes them with FFmpeg's XMA2
// decoder (libavcodec) while the output ring has room. The frames are repacked into packets of the
// decoder's own, so it sees one continuous stream through loop jumps, as the hardware decodes. It also
// takes DOA4's packets, whose headers are XMA1 style (a sequence number where XMA2 has a frame count).
// Loops are frame accurate: their subframe trims (128-sample steps) are ignored.
// The decoder holds back its last 4096 samples until it's told the stream ended, which it is once the
// input runs out (so a short sound comes out whole). An input buffer read to its end stays valid for the
// game until its samples are in the output ring, as on hardware.
// runtime_tests checks the repacking: recorded streams decode to the same samples from their own packets.
// Without FFmpeg (see CMakeLists.txt), or with XMA_SILENT=1 (to tell decoding problems from others),
// output rings are filled with silence at a plausible rate, which keeps a game's streaming logic running as
// it would on hardware.
// TRACE_XMA=1 logs contexts as they're set up, with the guest call chain (which game code plays a sound);
// DUMP_XMA=1 writes each new input buffer (the first 64)
// to <data>/dumps/xma_<physical>_<packets>_<channels>ch_<rate>.bin, which runtime_tests decodes.
#include "audio/xma.h"
#include "config.h"
#include "guest_abi.h"
#include "guest_memory.h"
#include "kernel/status.h"
#include "log.h"
#include "util/profiler.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#if HAVE_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}
#endif

namespace
{
    constexpr uint32_t kContextCount = 320;
    constexpr uint32_t kContextSize = 64;
    constexpr uint32_t kPacketBytes = 2048;
    constexpr uint32_t kOutputBlockBytes = 256;
    constexpr uint32_t kSamplesPerFrame = 512;
    constexpr uint32_t kPacketBits = kPacketBytes * 8;
    constexpr uint32_t kSampleRates[4] = { 24000, 32000, 44100, 48000 };
    // Silent decoding consumes one input packet per this many output frames (512 samples each);
    // XMA packets typically hold a few frames.
    constexpr uint32_t kFramesPerPacket = 4;

    bool Tracing()
    {
        static const bool tracing = [] {
            const char* setting = std::getenv(GAME_PREFIX "_TRACE_XMA");
            return setting && std::atoi(setting) != 0;
        }();
        return tracing;
    }

    // Context dwords (host values of the big-endian words) and their bitfields.
    struct Context
    {
        uint32_t d[10];

        static uint32_t Get(uint32_t word, uint32_t shift, uint32_t bits) { return (word >> shift) & ((1u << bits) - 1); }
        static void Set(uint32_t& word, uint32_t shift, uint32_t bits, uint32_t value)
        {
            const uint32_t mask = ((1u << bits) - 1) << shift;
            word = (word & ~mask) | ((value << shift) & mask);
        }

        uint32_t Input0PacketCount() const { return Get(d[0], 0, 12); }
        bool Input0Valid() const { return Get(d[0], 20, 1); }
        bool Input1Valid() const { return Get(d[0], 21, 1); }
        uint32_t OutputBlockCount() const { return Get(d[0], 22, 5); }
        uint32_t OutputWriteOffset() const { return Get(d[0], 27, 5); }
        uint32_t Input1PacketCount() const { return Get(d[1], 0, 12); }
        uint32_t LoopCount() const { return Get(d[0], 12, 8); }
        uint32_t LoopStart() const { return Get(d[3], 0, 26); } // bits into the input buffer
        uint32_t LoopEnd() const { return Get(d[4], 0, 26); }
        void SetLoopCount(uint32_t value) { Set(d[0], 12, 8, value); }
        uint32_t SampleRate() const { return kSampleRates[Get(d[1], 27, 2)]; }
        bool IsStereo() const { return Get(d[1], 29, 1); }
        bool OutputValid() const { return Get(d[1], 31, 1); }
        uint32_t InputReadOffset() const { return Get(d[2], 0, 26); } // bits
        uint32_t CurrentBuffer() const { return Get(d[4], 31, 1); }
        uint32_t Input0Pointer() const { return d[5]; }
        uint32_t Input1Pointer() const { return d[6]; }
        uint32_t OutputPointer() const { return d[7]; }
        uint32_t OutputReadOffset() const { return Get(d[9], 0, 5); }

        void SetInput0Valid(bool value) { Set(d[0], 20, 1, value); }
        void SetInput1Valid(bool value) { Set(d[0], 21, 1, value); }
        void SetOutputWriteOffset(uint32_t value) { Set(d[0], 27, 5, value); }
        void SetOutputValid(bool value) { Set(d[1], 31, 1, value); }
        void SetInputReadOffset(uint32_t value) { Set(d[2], 0, 26, value); }
        void SetCurrentBuffer(uint32_t value) { Set(d[4], 31, 1, value); }
        void SetOutputReadOffset(uint32_t value) { Set(d[9], 0, 5, value); }
    };

    Context Load(uint32_t address)
    {
        Context context;
        for (uint32_t i = 0; i < 10; i++)
            context.d[i] = guest::LoadU32(address + i * 4);
        return context;
    }

    void Store(uint32_t address, const Context& context)
    {
        for (uint32_t i = 0; i < 10; i++)
            guest::StoreU32(address + i * 4, context.d[i]);
    }

#if HAVE_FFMPEG
    constexpr uint32_t kInfiniteLoop = 255;

    // FFmpeg's decoder drops the first frame's samples (its overlap with nothing before) and 64 more.
    constexpr uint32_t kDecoderDelay = kSamplesPerFrame + 64;

    // `count` (1-32) bits of XMA data, big endian and most significant bit first. Reads only the bytes they're in.
    uint32_t GetBits(const uint8_t* data, uint32_t bit, uint32_t count)
    {
        const uint8_t* bytes = data + (bit >> 3);
        const uint32_t shift = bit & 7, size = (shift + count + 7) / 8;
        uint64_t window = 0;
        for (uint32_t i = 0; i < size; i++)
            window |= uint64_t(bytes[i]) << (56 - 8 * i);
        return uint32_t((window << shift) >> (64 - count));
    }

    // Writes `count` (1-32) bits into zeroed data.
    void PutBits(uint8_t* data, uint32_t bit, uint32_t value, uint32_t count)
    {
        for (uint32_t i = 0; i < count; i++, bit++)
        {
            if ((value >> (count - 1 - i)) & 1)
                data[bit >> 3] |= uint8_t(0x80 >> (bit & 7));
        }
    }

    // Copies `count` bits into zeroed data.
    void CopyBits(uint8_t* to, uint32_t toBit, const uint8_t* from, uint32_t fromBit, uint32_t count)
    {
        for (uint32_t done = 0; done < count;)
        {
            const uint32_t part = std::min(count - done, 32u);
            PutBits(to, toBit + done, GetBits(from, fromBit + done, part), part);
            done += part;
        }
    }

    // Packet header: frame count or sequence number (6 bits), how many bits at the start continue the
    // previous packet's last frame (15), metadata (3), and how many packets of other streams follow (8).
    uint32_t ContinuationBits(const uint8_t* packet) { return GetBits(packet, 6, 15); }
    uint32_t PacketSkip(const uint8_t* packet) { return packet[3]; }

    // Host decoding state of one context.
    struct Stream
    {
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        uint32_t channels = 0, sampleRate = 0; // what the decoder was opened for
        std::vector<uint8_t> pending;          // decoded PCM (16-bit big-endian, interleaved) not yet output
        size_t pendingOffset = 0;
        // Where the next frame starts: a packet of the current input buffer, and a bit in it.
        uint32_t packetIndex = 0, bit = 32;
        // Whether a frame can start there: the frame before ended in this packet with its last bit set, or
        // continued into it.
        bool more = true;
        // The end of the packet before, after its frames: the start of a frame the next packet continues.
        std::vector<uint8_t> partial;
        uint32_t partialBits = 0;
        // A new sound or a seek: the decoder starts over before the next frame.
        bool restart = true;
        // The input ran out, so the decoder was told the stream ended and gave up all of its samples. It's
        // reopened if more input comes.
        bool drained = false;
        // Since the decoder was opened: frames sent to it, and samples (per channel) written to the output ring.
        uint32_t frames = 0;
        uint64_t samplesWritten = 0;
        // Input buffers read to their end. They stay valid for the game until their samples are in the output
        // ring, as on hardware, where decoding a frame writes its samples. The decoder holds back its last 4096
        // samples (and the frame before) until the stream ends, so here that's once its output has reached the
        // end of frame `consumedFrames`, or it has been drained.
        bool consumed[2] = {};
        uint32_t consumedFrames[2] = {};
        // The frame being moved, and the packet being assembled for the decoder: frames back to back after
        // a header whose only field set is the number of bits continuing the previous packet's last frame.
        std::vector<uint8_t> frameBits;
        uint8_t assembled[kPacketBytes + AV_INPUT_BUFFER_PADDING_SIZE] = {};
        uint32_t assembledBit = 32;
        bool reportedError = false;

        void Close()
        {
            avcodec_free_context(&codec);
            av_packet_free(&packet);
            av_frame_free(&frame);
            channels = sampleRate = 0;
        }

        // A new sound from bit `offset` of the current input buffer.
        void Restart(uint32_t offset)
        {
            restart = true;
            drained = false;
            frames = 0;
            samplesWritten = 0;
            consumed[0] = consumed[1] = false;
            packetIndex = offset / kPacketBits;
            bit = offset % kPacketBits;
            more = true;
            partialBits = 0;
            pending.clear();
            pendingOffset = 0;
            StartPacket(0);
        }

        // Whether frames can be read from an input buffer: it's valid, and not read to its end.
        bool Readable(const Context& context, uint32_t buffer) const
        {
            return (buffer ? context.Input1Valid() : context.Input0Valid()) && !consumed[buffer];
        }

        // Input buffers read to their end become invalid once their samples are in the output ring. Returns
        // whether any did.
        bool Retire(Context& context)
        {
            // Nothing held back: everything decoded is in the ring, and the decoder was drained or has nothing.
            const bool allWritten = pendingOffset == pending.size() && (drained || !frames);
            bool retired = false;
            for (uint32_t buffer = 0; buffer < 2; buffer++)
            {
                const uint64_t end = uint64_t(consumedFrames[buffer]) * kSamplesPerFrame;
                if (!consumed[buffer] || !(allWritten || (samplesWritten && samplesWritten + kDecoderDelay >= end)))
                    continue;
                consumed[buffer] = false;
                if (buffer)
                    context.SetInput1Valid(false);
                else
                    context.SetInput0Valid(false);
                retired = true;
            }
            return retired;
        }

        // Opens the decoder (anew if `reopen`: closing it is the way to drop all of its state, since a flush
        // makes it skip the next frame).
        bool Open(uint32_t channelCount, uint32_t rate, bool reopen)
        {
            if (!reopen && codec && channels == channelCount && sampleRate == rate)
                return true;
            Close();
            const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_XMA2);
            codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
            packet = av_packet_alloc();
            frame = av_frame_alloc();
            if (!codec || !packet || !frame)
            {
                Close();
                return false;
            }
            codec->sample_rate = int(rate);
            av_channel_layout_default(&codec->ch_layout, int(channelCount));
            codec->block_align = kPacketBytes;
            // The XMA2WAVEFORMATEX fields after WAVEFORMATEX (little endian): one stream, its channel mask.
            codec->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
            codec->extradata_size = 34;
            const uint32_t mask = channelCount == 2 ? 3 : 4; // front left and right, or front center
            codec->extradata[0] = 1;
            memcpy(codec->extradata + 2, &mask, 4);
            if (avcodec_open2(codec, decoder, nullptr) < 0)
            {
                Close();
                return false;
            }
            channels = channelCount;
            sampleRate = rate;
            return true;
        }

        void StartPacket(uint32_t continuation)
        {
            memset(assembled, 0, sizeof(assembled));
            PutBits(assembled, 6, std::min(continuation, 0x7FFFu), 15);
            assembledBit = 32;
        }

        // Decodes the assembled packet, appending its frames to `pending`, and starts the next one.
        bool SendPacket(uint32_t continuation)
        {
            bool ok = av_new_packet(packet, kPacketBytes) >= 0;
            if (ok)
            {
                memcpy(packet->data, assembled, kPacketBytes);
                ok = avcodec_send_packet(codec, packet) >= 0;
                av_packet_unref(packet);
            }
            StartPacket(continuation);
            return ok && Receive();
        }

        // Ends the stream: the decoder gives up the samples it holds back (all but the last 4096 until then, and
        // the end of the last frame). It then needs reopening.
        bool Drain()
        {
            const bool ok = avcodec_send_packet(codec, nullptr) >= 0;
            return Receive() && ok;
        }

        // Appends what the decoder has decoded to `pending`.
        bool Receive()
        {
            int result = 0;
            while ((result = avcodec_receive_frame(codec, frame)) >= 0)
            {
                const int samples = frame->nb_samples, count = frame->ch_layout.nb_channels;
                if (pendingOffset && pendingOffset == pending.size())
                {
                    pending.clear();
                    pendingOffset = 0;
                }
                const size_t start = pending.size();
                pending.resize(start + size_t(samples) * count * 2);
                uint8_t* out = pending.data() + start;
                for (int i = 0; i < samples; i++)
                {
                    for (int c = 0; c < count; c++)
                    {
                        float value = 0.0f;
                        if (frame->format == AV_SAMPLE_FMT_FLTP)
                            value = reinterpret_cast<const float*>(frame->extended_data[c])[i];
                        else if (frame->format == AV_SAMPLE_FMT_FLT)
                            value = reinterpret_cast<const float*>(frame->extended_data[0])[i * count + c];
                        const int sample = std::clamp(int(std::lrint(value * 32767.0f)), -32768, 32767);
                        *out++ = uint8_t(sample >> 8);
                        *out++ = uint8_t(sample);
                    }
                }
                av_frame_unref(frame);
            }
            return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
        }

        // Appends the frame in `frameBits` to the assembled packet, sending packets as they fill. A frame
        // doesn't start in the last 15 bits of a packet (the encoder pads those, and so does this). Its last
        // bit says whether another frame can start after it in the packet it ends in: FFmpeg's decoder takes
        // the rest of a packet after a frame without it for the start of a frame continued in the next packet.
        // That changes with the packets, so it's set for these.
        bool AppendFrame(uint32_t length)
        {
            bool ok = true;
            if (assembledBit + 16 > kPacketBits)
                ok = SendPacket(0);
            uint32_t end = assembledBit + length;
            while (end > kPacketBits)
                end -= kPacketBits - 32; // continued after the next packet's header
            uint8_t& last = frameBits[(length - 1) >> 3];
            const uint8_t mask = uint8_t(0x80 >> ((length - 1) & 7));
            last = uint8_t(end + 16 <= kPacketBits ? last | mask : last & ~mask);
            frames++;
            for (uint32_t done = 0; done < length;)
            {
                const uint32_t count = std::min({ length - done, kPacketBits - assembledBit, 32u });
                PutBits(assembled, assembledBit, GetBits(frameBits.data(), done, count), count);
                assembledBit += count;
                done += count;
                if (assembledBit == kPacketBits)
                    ok = SendPacket(length - done) && ok;
            }
            return ok;
        }

        // Sends what's assembled, if anything, so its frames are decoded.
        bool Flush() { return assembledBit > 32 ? SendPacket(0) : true; }
    };
#endif

    struct Decoder
    {
        std::mutex lock; // guards contexts against concurrent read-modify-write by the worker and exports
        std::condition_variable wake;
        uint32_t array = 0; // guest address of the context array
        std::bitset<kContextCount> allocated;
        std::bitset<kContextCount> enabled;
        uint32_t framesSincePacket[kContextCount] = {};
        // Contexts the game kicks through the registers: where decoding left the input (read offset, current buffer),
        // to tell when the game has moved it (a new sound, a seek) and the stream must restart.
        std::bitset<kContextCount> positionKnown;
        uint32_t readOffset[kContextCount] = {};
        uint32_t currentBuffer[kContextCount] = {};
#if HAVE_FFMPEG
        Stream streams[kContextCount];
#endif
        bool started = false;

        uint32_t Index(uint32_t context) const { return (context - array) / kContextSize; }
        bool Valid(uint32_t context) const { return array && context >= array && context < array + kContextCount * kContextSize; }
    };

    Decoder& GetDecoder()
    {
        static Decoder decoder;
        return decoder;
    }

#if HAVE_FFMPEG
    // Packet `index` of input buffer `buffer`, or null if that buffer isn't readable or is shorter.
    const uint8_t* InputPacket(const Context& context, const Stream& stream, uint32_t buffer, uint32_t index)
    {
        const uint32_t count = buffer ? context.Input1PacketCount() : context.Input0PacketCount();
        const uint32_t base = buffer ? context.Input1Pointer() : context.Input0Pointer();
        if (!stream.Readable(context, buffer) || !base || index >= count)
            return nullptr;
        return guest::ToHost(guest::PhysicalToGuest(base + index * kPacketBytes));
    }

    // The current input buffer is read to its end, its last bits in frame `frames`: reading continues in the
    // other one, and it becomes invalid once that frame's samples are in the output ring.
    void FinishBuffer(Context& context, Stream& stream, uint32_t frames)
    {
        const uint32_t buffer = context.CurrentBuffer();
        stream.consumed[buffer] = true;
        stream.consumedFrames[buffer] = frames;
        context.SetCurrentBuffer(buffer ^ 1);
        stream.packetIndex = 0;
        stream.bit = 32;
    }

    enum class ReadResult
    {
        kFrame, // in stream.frameBits; returns its length
        kEnd,   // no input
    };

    // Reads the next frame into stream.frameBits, following the loop, packets and input buffers. Packets are
    // parsed as FFmpeg's decoder parses them: a frame in a packet follows its start, or a frame that ended there
    // with its last bit set, and fits. Otherwise the rest of the packet starts a frame that the next packet
    // continues (by the number of bits in its header: none, and it was padding). A frame's length field can be
    // split between packets.
    ReadResult ReadFrame(Context& context, Stream& stream, uint32_t& length)
    {
        for (int guard = 0; guard < 8192; guard++)
        {
            const uint32_t buffer = context.CurrentBuffer();
            const uint8_t* packet = InputPacket(context, stream, buffer, stream.packetIndex);
            if (!packet)
            {
                if (stream.Readable(context, buffer))
                    FinishBuffer(context, stream, stream.frames); // read to its end
                else if (stream.Readable(context, buffer ^ 1))
                {
                    context.SetCurrentBuffer(buffer ^ 1);
                    stream.packetIndex = 0;
                    stream.bit = 32;
                }
                else
                    return ReadResult::kEnd;
                continue;
            }
            // Entering a packet: its first bits end the frame started at the end of the packet before (or one
            // before the sound, which isn't read).
            if (stream.bit == 32)
            {
                const uint32_t continuation = std::min(ContinuationBits(packet), kPacketBits - 32);
                const uint32_t started = stream.partialBits;
                stream.bit += continuation;
                stream.more = true;
                stream.partialBits = 0;
                if (started && continuation)
                {
                    // The count can be more than the frame needs: the last packet of a sound, where no frame
                    // starts, has the packet's size.
                    stream.frameBits.assign((started + continuation + 7) / 8 + 8, 0);
                    CopyBits(stream.frameBits.data(), 0, stream.partial.data(), 0, started);
                    CopyBits(stream.frameBits.data(), started, packet, 32, continuation);
                    // A frame longer than that (over three packets) isn't decoded, as by FFmpeg.
                    const uint32_t frameLength = GetBits(stream.frameBits.data(), 0, 15);
                    if (frameLength > 15 && frameLength <= started + continuation)
                    {
                        context.SetInputReadOffset(stream.packetIndex * kPacketBits + stream.bit);
                        length = frameLength;
                        return ReadResult::kFrame;
                    }
                }
            }

            // The loop: at its end, back to its start.
            const uint32_t position = stream.packetIndex * kPacketBits + stream.bit;
            const uint32_t loops = context.LoopCount(), loopStart = context.LoopStart(), loopEnd = context.LoopEnd();
            if (loops && loopEnd > loopStart && position >= loopEnd)
            {
                if (loops != kInfiniteLoop)
                    context.SetLoopCount(loops - 1);
                stream.packetIndex = loopStart / kPacketBits;
                stream.bit = std::max(loopStart % kPacketBits, 32u);
                stream.more = true; // a frame starts there
                stream.partialBits = 0;
                continue;
            }

            // A frame in this packet.
            const uint32_t rest = kPacketBits - stream.bit;
            const uint32_t frameLength = rest > 15 ? GetBits(packet, stream.bit, 15) : 0;
            if (stream.more && frameLength > 15 && frameLength <= rest)
            {
                stream.frameBits.assign((frameLength + 7) / 8 + 8, 0);
                CopyBits(stream.frameBits.data(), 0, packet, stream.bit, frameLength);
                stream.more = GetBits(packet, stream.bit + frameLength - 1, 1);
                stream.bit += frameLength;
                context.SetInputReadOffset(stream.packetIndex * kPacketBits + stream.bit);
                length = frameLength;
                return ReadResult::kFrame;
            }

            // The rest of the packet, for the next one (in this buffer, or the other).
            stream.partial.assign((rest + 7) / 8 + 8, 0);
            CopyBits(stream.partial.data(), 0, packet, stream.bit, rest);
            stream.partialBits = rest;
            stream.packetIndex += 1 + PacketSkip(packet);
            stream.bit = 32;
        }
        return ReadResult::kEnd; // malformed data
    }

    // Decodes one context while it has input and its output ring has room, writing a frame (512 samples) at a
    // time. Returns whether the context changed.
    bool Work(Decoder& decoder, uint32_t index)
    {
        const uint32_t address = decoder.array + index * kContextSize;
        Context context = Load(address);
        const uint32_t blockCount = context.OutputBlockCount();
        if (!context.OutputValid() || !blockCount || !context.OutputPointer())
            return false;
        Stream& stream = decoder.streams[index];
        const uint32_t channels = context.IsStereo() ? 2 : 1;
        if (!stream.Open(channels, context.SampleRate(), stream.restart))
        {
            if (!stream.reportedError)
                LOG_WARN("XMA: can't open FFmpeg's XMA2 decoder (%u channels, %u Hz)", channels, context.SampleRate());
            stream.reportedError = true;
            return false;
        }
        stream.restart = false;

        const uint32_t output = guest::PhysicalToGuest(context.OutputPointer());
        const uint32_t frameBytes = kSamplesPerFrame * 2 * channels;
        uint32_t write = context.OutputWriteOffset() % blockCount;
        const uint32_t read = context.OutputReadOffset() % blockCount;
        bool changed = false, wrote = false;
        for (;;)
        {
            // Decoded PCM goes into the ring first, a frame at a time, in whole blocks but at the end of the
            // stream (where the rest of the last block is silence).
            const size_t available = stream.pending.size() - stream.pendingOffset;
            uint32_t bytes = uint32_t(std::min<size_t>(available, frameBytes));
            if (!stream.drained)
                bytes -= bytes % kOutputBlockBytes;
            if (bytes)
            {
                const uint32_t blocks = (bytes + kOutputBlockBytes - 1) / kOutputBlockBytes;
                const uint32_t free = wrote && write == read ? 0 : write < read ? read - write : blockCount - write + read;
                if (free < blocks)
                    break;
                for (uint32_t b = 0, copied = 0; b < blocks; b++)
                {
                    guest::BeforeHostWrite(output + write * kOutputBlockBytes, kOutputBlockBytes);
                    uint8_t* block = guest::ToHost(output + write * kOutputBlockBytes);
                    const uint32_t part = std::min(kOutputBlockBytes, bytes - copied);
                    memcpy(block, stream.pending.data() + stream.pendingOffset + copied, part);
                    memset(block + part, 0, kOutputBlockBytes - part);
                    copied += part;
                    write = (write + 1) % blockCount;
                }
                stream.pendingOffset += bytes;
                stream.samplesWritten += bytes / (2 * channels);
                changed = wrote = true;
                continue;
            }

            // More input after the end of the stream: the decoder starts over (what it had is all written, so
            // the input buffers before retire).
            if (stream.drained)
            {
                if (!stream.Readable(context, 0) && !stream.Readable(context, 1))
                    break;
                changed = stream.Retire(context) || changed;
                if (!stream.Open(channels, context.SampleRate(), true))
                    break;
                stream.drained = false;
                stream.frames = 0;
                stream.samplesWritten = 0;
            }

            // Then more frames. Once there are none, what's assembled is decoded, and at the end of the input,
            // what the decoder holds back.
            uint32_t length = 0;
            const ReadResult result = ReadFrame(context, stream, length);
            bool ok = true;
            if (result == ReadResult::kFrame)
                ok = stream.AppendFrame(length);
            else if (stream.assembledBit > 32)
                ok = stream.Flush();
            else if (result == ReadResult::kEnd && stream.frames)
            {
                ok = stream.Drain();
                stream.drained = true;
            }
            else
                break;
            changed = true;
            if (!ok && !stream.reportedError)
            {
                stream.reportedError = true;
                LOG_WARN("XMA: context %u: FFmpeg can't decode a packet (read offset %u)", index, context.InputReadOffset());
            }
        }
        changed = stream.Retire(context) || changed;
        if (!changed)
            return false;
        context.SetOutputWriteOffset(write);
        // A full ring invalidates the output buffer until the game makes room.
        if (wrote && write == read)
            context.SetOutputValid(false);
        Store(address, context);
        return true;
    }
#endif

    // Produces silent output for one context while it has input and output space. Returns whether
    // anything was written.
    bool WorkSilent(Decoder& decoder, uint32_t index)
    {
        const uint32_t address = decoder.array + index * kContextSize;
        Context context = Load(address);
        if (!context.OutputValid() || !(context.Input0Valid() || context.Input1Valid()))
            return false;
        const uint32_t blockCount = context.OutputBlockCount();
        if (!blockCount || !context.OutputPointer())
            return false;

        // Output ring in 256-byte blocks; write until the write offset reaches the read offset.
        const uint32_t frameBytes = kSamplesPerFrame * 2 * (context.IsStereo() ? 2 : 1);
        const uint32_t frameBlocks = frameBytes / kOutputBlockBytes;
        const uint32_t output = guest::PhysicalToGuest(context.OutputPointer());
        uint32_t write = context.OutputWriteOffset() % blockCount;
        const uint32_t read = context.OutputReadOffset() % blockCount;
        bool wrote = false;
        for (;;)
        {
            // The ring is empty when the offsets meet at first (its output buffer is valid), full once they meet again.
            const uint32_t free = wrote && write == read ? 0 : write < read ? read - write : blockCount - write + read;
            if (free < frameBlocks || !(context.Input0Valid() || context.Input1Valid()))
                break;
            for (uint32_t b = 0; b < frameBlocks; b++)
            {
                guest::BeforeHostWrite(output + write * kOutputBlockBytes, kOutputBlockBytes);
                memset(guest::ToHost(output + write * kOutputBlockBytes), 0, kOutputBlockBytes);
                write = (write + 1) % blockCount;
            }
            wrote = true;

            // Consume input: advance a packet every few frames; a finished buffer becomes invalid
            // and decoding continues in the other one.
            if (++decoder.framesSincePacket[index] < kFramesPerPacket)
                continue;
            decoder.framesSincePacket[index] = 0;
            uint32_t buffer = context.CurrentBuffer();
            if (!(buffer ? context.Input1Valid() : context.Input0Valid()))
            {
                buffer ^= 1;
                context.SetCurrentBuffer(buffer);
            }
            const uint32_t packets = buffer ? context.Input1PacketCount() : context.Input0PacketCount();
            const uint32_t packet = context.InputReadOffset() / (kPacketBytes * 8) + 1;
            if (packet < packets)
            {
                context.SetInputReadOffset(packet * kPacketBytes * 8 + 32);
            }
            else
            {
                if (buffer)
                    context.SetInput1Valid(false);
                else
                    context.SetInput0Valid(false);
                context.SetCurrentBuffer(buffer ^ 1);
                context.SetInputReadOffset(32); // past the first packet's header
            }
        }
        if (!wrote)
            return false;
        context.SetOutputWriteOffset(write);
        // A full ring invalidates the output buffer until the game makes room.
        if (write == read)
            context.SetOutputValid(false);
        Store(address, context);
        return true;
    }

    void WorkerThread()
    {
        profile::NameThread("XMA decoder");
#if HAVE_FFMPEG
        const char* setting = std::getenv(GAME_PREFIX "_XMA_SILENT");
        const bool silent = setting && std::atoi(setting) != 0;
#else
        const bool silent = true;
#endif
        Decoder& decoder = GetDecoder();
        std::unique_lock lock(decoder.lock);
        for (;;)
        {
            for (uint32_t i = 0; i < kContextCount; i++)
            {
                if (!decoder.enabled[i])
                    continue;
#if HAVE_FFMPEG
                if (!silent)
                {
                    Work(decoder, i);
                    continue;
                }
#endif
                WorkSilent(decoder, i);
            }
            decoder.wake.wait_for(lock, std::chrono::milliseconds(2));
        }
    }

    void EnsureStarted(Decoder& decoder)
    {
        if (decoder.started)
            return;
        decoder.started = true;
#if HAVE_FFMPEG
        av_log_set_level(AV_LOG_ERROR);
#endif
        decoder.array = guest::AllocPhysical(kContextCount * kContextSize, 0x1000, guest::kPageReadWrite, 0, 0x1FFFFFFF, 256);
        if (decoder.array)
            memset(guest::ToHost(decoder.array), 0, kContextCount * kContextSize);
        std::thread(WorkerThread).detach();
    }

    // Applies `update` to a context under the decoder lock.
    template <typename F>
    uint32_t WithContext(uint32_t address, F&& update)
    {
        Decoder& decoder = GetDecoder();
        std::lock_guard lock(decoder.lock);
        if (!decoder.Valid(address))
            return 0;
        Context context = Load(address);
        const uint32_t result = update(context);
        Store(address, context);
        decoder.wake.notify_one();
        return result;
    }

    uint32_t PhysicalOf(uint32_t address)
    {
        return address ? guest::PhysicalAddress(address) & 0x1FFFFFFF : 0;
    }

    uint32_t XMACreateContext(be<uint32_t>* contextOut)
    {
        Decoder& decoder = GetDecoder();
        std::lock_guard lock(decoder.lock);
        EnsureStarted(decoder);
        for (uint32_t i = 0; decoder.array && i < kContextCount; i++)
        {
            if (decoder.allocated[i])
                continue;
            decoder.allocated[i] = true;
            decoder.enabled[i] = false;
            decoder.framesSincePacket[i] = 0;
            decoder.positionKnown[i] = false;
#if HAVE_FFMPEG
            decoder.streams[i].Restart(32);
#endif
            const uint32_t address = decoder.array + i * kContextSize;
            guest::BeforeHostWrite(address, kContextSize);
            memset(guest::ToHost(address), 0, kContextSize);
            if (contextOut)
                *contextOut = address;
            return status::kSuccess;
        }
        if (contextOut)
            *contextOut = 0;
        return status::kNoMemory;
    }

    void XMAReleaseContext(uint32_t context)
    {
        Decoder& decoder = GetDecoder();
        std::lock_guard lock(decoder.lock);
        if (!decoder.Valid(context))
            return;
        const uint32_t index = decoder.Index(context);
        decoder.allocated[index] = false;
        decoder.enabled[index] = false;
        decoder.positionKnown[index] = false;
#if HAVE_FFMPEG
        decoder.streams[index].Close();
        decoder.streams[index].Restart(32);
#endif
        guest::BeforeHostWrite(context, kContextSize);
        memset(guest::ToHost(context), 0, kContextSize);
    }

    struct LoopData
    {
        be<uint32_t> loopStart;
        be<uint32_t> loopEnd;
        uint8_t loopCount;
        uint8_t subframeEnd;
        uint8_t subframeSkip;
    };

    struct ContextInit
    {
        be<uint32_t> input0;
        be<uint32_t> input0PacketCount;
        be<uint32_t> input1;
        be<uint32_t> input1PacketCount;
        be<uint32_t> inputReadOffset;
        be<uint32_t> output;
        be<uint32_t> outputBlockCount;
        be<uint32_t> workBuffer;
        be<uint32_t> subframeDecodeCount;
        be<uint32_t> channelCount;
        be<uint32_t> sampleRate;
        LoopData loop;
    };

    void ApplyLoopData(Context& context, const LoopData& loop)
    {
        Context::Set(context.d[3], 0, 26, loop.loopStart);
        Context::Set(context.d[4], 0, 26, loop.loopEnd);
        Context::Set(context.d[0], 12, 8, loop.loopCount);
        Context::Set(context.d[1], 14, 3, loop.subframeEnd);
        Context::Set(context.d[1], 17, 3, loop.subframeSkip);
    }

    // A new sound or a seek: decoding restarts at `offset` (bits into the current input buffer).
    void RestartStream(uint32_t context, uint32_t offset)
    {
#if HAVE_FFMPEG
        Decoder& decoder = GetDecoder();
        if (decoder.Valid(context))
            decoder.streams[decoder.Index(context)].Restart(offset);
#else
        (void)context;
        (void)offset;
#endif
    }

    uint32_t XMAInitializeContext(uint32_t context, const ContextInit* init, PPCContext* ctx)
    {
        if (!init)
            return status::kInvalidParameter;
        if (Tracing())
        {
            LOG_INFO("XMA: context %08X: input %08X x%u, %08X x%u, read offset %u, output %08X x%u, %u subframes, channels %u, rate %u, loop %u-%u x%u "
                     "(called from %08X)",
                context, uint32_t(init->input0), uint32_t(init->input0PacketCount), uint32_t(init->input1), uint32_t(init->input1PacketCount),
                uint32_t(init->inputReadOffset), uint32_t(init->output), uint32_t(init->outputBlockCount), uint32_t(init->subframeDecodeCount),
                uint32_t(init->channelCount), uint32_t(init->sampleRate), uint32_t(init->loop.loopStart), uint32_t(init->loop.loopEnd),
                init->loop.loopCount, uint32_t(ctx->lr));
            guest::PrintBacktrace(ctx->r1.u32);
        }
        return WithContext(context, [&](Context& c) {
            RestartStream(context, init->inputReadOffset);
            memset(c.d, 0, sizeof(c.d));
            c.d[5] = PhysicalOf(init->input0);
            Context::Set(c.d[0], 0, 12, init->input0PacketCount);
            c.d[6] = PhysicalOf(init->input1);
            Context::Set(c.d[1], 0, 12, init->input1PacketCount);
            c.SetInputReadOffset(init->inputReadOffset);
            c.d[7] = PhysicalOf(init->output);
            Context::Set(c.d[0], 22, 5, init->outputBlockCount);
            c.d[8] = PhysicalOf(init->workBuffer);
            Context::Set(c.d[1], 20, 4, init->subframeDecodeCount);
            Context::Set(c.d[1], 29, 1, init->channelCount >= 1 ? 1 : 0);
            Context::Set(c.d[1], 27, 2, init->sampleRate);
            ApplyLoopData(c, init->loop);
            return 0u;
        });
    }

    uint32_t XMAEnableContext(uint32_t context)
    {
        Decoder& decoder = GetDecoder();
        std::lock_guard lock(decoder.lock);
        if (decoder.Valid(context))
            decoder.enabled[decoder.Index(context)] = true;
        decoder.wake.notify_one();
        return 0;
    }

    uint32_t XMADisableContext(uint32_t context, uint32_t /*wait*/)
    {
        // The worker holds the lock while decoding, so once it is taken the context is idle.
        Decoder& decoder = GetDecoder();
        std::lock_guard lock(decoder.lock);
        if (decoder.Valid(context))
            decoder.enabled[decoder.Index(context)] = false;
        return 0;
    }

    uint32_t XMABlockWhileInUse(uint32_t context)
    {
        Decoder& decoder = GetDecoder();
        for (;;)
        {
            {
                std::lock_guard lock(decoder.lock);
                if (!decoder.Valid(context))
                    return 0;
                const Context c = Load(context);
                if (!decoder.enabled[decoder.Index(context)] || !(c.Input0Valid() || c.Input1Valid()) || !c.OutputValid())
                    return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    uint32_t XMASetLoopData(uint32_t context, const LoopData* loop)
    {
        if (!loop)
            return 0;
        if (Tracing())
            LOG_INFO("XMA: context %08X: loop %u-%u x%u, subframes %u/%u", context, uint32_t(loop->loopStart), uint32_t(loop->loopEnd), loop->loopCount,
                loop->subframeEnd, loop->subframeSkip);
        return WithContext(context, [&](Context& c) {
            ApplyLoopData(c, *loop);
            return 0u;
        });
    }

    uint32_t XMASetInputBuffer0(uint32_t context, uint32_t buffer, uint32_t packets)
    {
        if (Tracing())
            LOG_INFO("XMA: context %08X: input 0 %08X x%u", context, buffer, packets);
        return WithContext(context, [&](Context& c) {
            c.d[5] = PhysicalOf(buffer);
            Context::Set(c.d[0], 0, 12, packets);
            return 0u;
        });
    }

    uint32_t XMASetInputBuffer1(uint32_t context, uint32_t buffer, uint32_t packets)
    {
        if (Tracing())
            LOG_INFO("XMA: context %08X: input 1 %08X x%u", context, buffer, packets);
        return WithContext(context, [&](Context& c) {
            c.d[6] = PhysicalOf(buffer);
            Context::Set(c.d[1], 0, 12, packets);
            return 0u;
        });
    }

    // DUMP_XMA=1: an input buffer as it becomes valid (its data is ready then), or as a kick finds it valid.
    void DumpInput(const Context& c, uint32_t physical, uint32_t packets)
    {
        static const bool dumping = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DUMP_XMA");
            return setting && std::atoi(setting) != 0;
        }();
        static std::vector<uint64_t> dumped;
        const uint64_t id = (uint64_t(physical) << 32) | packets;
        if (!dumping || !physical || !packets || dumped.size() >= 64 || std::find(dumped.begin(), dumped.end(), id) != dumped.end())
            return;
        dumped.push_back(id);
        std::error_code error;
        std::filesystem::create_directories(config::DataDirectory() + "/dumps", error);
        char name[96];
        snprintf(name, sizeof(name), "/dumps/xma_%08X_%u_%uch_%u.bin", physical, packets, c.IsStereo() ? 2 : 1, c.SampleRate());
        if (FILE* file = fopen((config::DataDirectory() + name).c_str(), "wb"))
        {
            fwrite(guest::ToHost(guest::PhysicalToGuest(physical)), kPacketBytes, packets, file);
            fclose(file);
        }
    }

    // An input buffer becomes valid: one decoding has read to its end but not retired yet (see Stream::consumed)
    // has new data (on hardware, it would have been invalid already).
    void InputBufferValid(uint32_t context, uint32_t buffer, bool wasValid)
    {
        bool consumed = false;
#if HAVE_FFMPEG
        Decoder& decoder = GetDecoder();
        if (decoder.Valid(context))
        {
            consumed = decoder.streams[decoder.Index(context)].consumed[buffer];
            decoder.streams[decoder.Index(context)].consumed[buffer] = false;
        }
#endif
        if (Tracing() && wasValid)
            LOG_INFO("XMA: context %08X: input %u set valid while valid%s", context, buffer, consumed ? " (read to its end)" : "");
    }

    uint32_t XMASetInputBuffer0Valid(uint32_t context)
    {
        return WithContext(context, [&](Context& c) {
            DumpInput(c, c.Input0Pointer(), c.Input0PacketCount());
            InputBufferValid(context, 0, c.Input0Valid());
            c.SetInput0Valid(true);
            return 0u;
        });
    }

    uint32_t XMASetInputBuffer1Valid(uint32_t context)
    {
        return WithContext(context, [&](Context& c) {
            DumpInput(c, c.Input1Pointer(), c.Input1PacketCount());
            InputBufferValid(context, 1, c.Input1Valid());
            c.SetInput1Valid(true);
            return 0u;
        });
    }

    uint32_t XMAIsInputBuffer0Valid(uint32_t context)
    {
        return WithContext(context, [](Context& c) { return uint32_t(c.Input0Valid()); });
    }

    uint32_t XMAIsInputBuffer1Valid(uint32_t context)
    {
        return WithContext(context, [](Context& c) { return uint32_t(c.Input1Valid()); });
    }

    uint32_t XMASetInputBufferReadOffset(uint32_t context, uint32_t offset)
    {
        if (Tracing())
            LOG_INFO("XMA: context %08X: read offset %u", context, offset);
        return WithContext(context, [&](Context& c) {
            RestartStream(context, offset);
            c.SetInputReadOffset(offset);
            return 0u;
        });
    }

    uint32_t XMASetOutputBufferValid(uint32_t context)
    {
        return WithContext(context, [](Context& c) {
            c.SetOutputValid(true);
            return 0u;
        });
    }

    uint32_t XMAIsOutputBufferValid(uint32_t context)
    {
        return WithContext(context, [](Context& c) { return uint32_t(c.OutputValid()); });
    }

    uint32_t XMAGetOutputBufferWriteOffset(uint32_t context)
    {
        return WithContext(context, [](Context& c) { return c.OutputWriteOffset(); });
    }

    uint32_t XMAGetOutputBufferReadOffset(uint32_t context)
    {
        return WithContext(context, [](Context& c) { return c.OutputReadOffset(); });
    }

    uint32_t XMASetOutputBufferReadOffset(uint32_t context, uint32_t offset)
    {
        return WithContext(context, [&](Context& c) {
            c.SetOutputReadOffset(offset);
            return 0u;
        });
    }

    // ---------------------------------------------------------------------------------------------
    // The decoder's registers. Later XDKs' XAudio (DOAX2's) drives the contexts through them instead of the XMA*
    // exports, keeping a copy of each context that it copies in before kicking it and back once the decoder is done
    // with it. The registers are a little-endian page at 0x7FEA0000, register n at byte 4n (as in Xenia's
    // xma_register_table.inc):
    //   ContextArrayAddress (0x600): the context array's physical address, which XAudio reads to turn a context's
    //     address into its index.
    //   CurrentContextIndex (0x606): the context the decoder is working on, with bit 9 set while it is; XAudio waits
    //     until that isn't one of a voice's contexts before copying them back.
    //   Kick (0x650-0x659), Lock (0x690-0x699) and Clear (0x6A0-0x6A9): one bit per context. A kick decodes until the
    //     context's output ring is full or its input is used up; a clear resets its buffers.
    // A kick is decoded at once, on the kicking thread, so the decoder is never busy when XAudio looks and
    // CurrentContextIndex stays 0. XAudio sets the contexts up itself (no XMAInitializeContext or read offset export
    // tells the decoder), so a kick that finds the input moved from where decoding left it restarts the stream there.
    constexpr uint32_t kContextArrayRegister = 0x600;
    constexpr uint32_t kKickRegisters = 0x650, kLockRegisters = 0x690, kClearRegisters = 0x6A0, kRegistersPerGroup = 10;

    std::bitset<kContextCount> g_kickedSinceClear; // for TRACE_XMA

    bool DecodesSilently()
    {
#if HAVE_FFMPEG
        static const bool silent = [] {
            const char* setting = std::getenv(GAME_PREFIX "_XMA_SILENT");
            return setting && std::atoi(setting) != 0;
        }();
        return silent;
#else
        return true;
#endif
    }

    void KickContext(Decoder& decoder, uint32_t index)
    {
        if (!decoder.allocated[index])
            return;
        const uint32_t address = decoder.array + index * kContextSize;
        const Context c = Load(address);
        if (Tracing() && !g_kickedSinceClear[index])
        {
            LOG_INFO("XMA: context %u kicked: input %08X x%u%s, %08X x%u%s, read offset %u, output %08X x%u%s, %u channels, %u Hz", index,
                c.Input0Pointer(), c.Input0PacketCount(), c.Input0Valid() ? " (valid)" : "", c.Input1Pointer(), c.Input1PacketCount(),
                c.Input1Valid() ? " (valid)" : "", c.InputReadOffset(), c.OutputPointer(), c.OutputBlockCount(), c.OutputValid() ? " (valid)" : "",
                c.IsStereo() ? 2 : 1, c.SampleRate());
        }
        g_kickedSinceClear[index] = true;
        if (c.Input0Valid())
            DumpInput(c, c.Input0Pointer(), c.Input0PacketCount());
        if (c.Input1Valid())
            DumpInput(c, c.Input1Pointer(), c.Input1PacketCount());
        if (DecodesSilently())
        {
            WorkSilent(decoder, index);
            return;
        }
#if HAVE_FFMPEG
        if (!decoder.positionKnown[index] || c.InputReadOffset() != decoder.readOffset[index] || c.CurrentBuffer() != decoder.currentBuffer[index])
        {
            if (Tracing() && decoder.positionKnown[index])
                LOG_INFO("XMA: context %u restarts at buffer %u, bit %u", index, c.CurrentBuffer(), c.InputReadOffset());
            decoder.streams[index].Restart(c.InputReadOffset());
        }
        Work(decoder, index);
        const Context after = Load(address);
        decoder.positionKnown[index] = true;
        decoder.readOffset[index] = after.InputReadOffset();
        decoder.currentBuffer[index] = after.CurrentBuffer();
#endif
    }

    void ClearContext(Decoder& decoder, uint32_t index)
    {
        const uint32_t address = decoder.array + index * kContextSize;
        Context context = Load(address);
        context.SetInput0Valid(false);
        context.SetInput1Valid(false);
        context.SetOutputValid(false);
        context.SetOutputReadOffset(0);
        context.SetOutputWriteOffset(0);
        Store(address, context);
        decoder.framesSincePacket[index] = 0;
#if HAVE_FFMPEG
        decoder.streams[index].Restart(32);
#endif
        decoder.positionKnown[index] = false;
        g_kickedSinceClear[index] = false;
        if (Tracing())
            LOG_INFO("XMA: context %u cleared", index);
    }
}

// A store XenonRecomp marks as MMIO into the register page; `value` is the register's (see ppc_overrides.h).
void XmaRegisterWrite(uint32_t address, uint32_t value)
{
    const uint32_t reg = (address - audio::kXmaRegisterPage) / 4;
    const auto apply = [reg, value](uint32_t first, auto&& action) {
        if (reg < first || reg >= first + kRegistersPerGroup)
            return false;
        Decoder& decoder = GetDecoder();
        std::lock_guard lock(decoder.lock);
        for (uint32_t bit = 0; bit < 32; bit++)
        {
            const uint32_t index = (reg - first) * 32 + bit;
            if ((value >> bit) & 1 && decoder.array && index < kContextCount)
                action(decoder, index);
        }
        return true;
    };
    if (apply(kKickRegisters, KickContext) || apply(kClearRegisters, ClearContext) ||
        apply(kLockRegisters, [](Decoder& decoder, uint32_t index) { decoder.enabled[index] = false; }))
        return;
    LOG_INFO("XMA: register 0x%03X = 0x%08X", reg, value);
}

namespace audio
{
    void InitializeXmaRegisters()
    {
        Decoder& decoder = GetDecoder();
        std::lock_guard lock(decoder.lock);
        EnsureStarted(decoder);
        // In host (little-endian) byte order, as XAudio reads it with lwbrx.
        *reinterpret_cast<uint32_t*>(guest::ToHost(kXmaRegisterPage + kContextArrayRegister * 4)) = guest::PhysicalAddress(decoder.array);
    }
}

namespace audio
{
    XmaTestDecode DecodeXmaForTest(uint32_t physical, uint32_t packets, uint32_t channels, uint32_t sampleRate, bool direct)
    {
        XmaTestDecode result;
#if HAVE_FFMPEG
        auto stream = std::make_unique<Stream>();
        if (!stream->Open(channels, sampleRate, true))
            return result;
        result.ok = true;
        if (direct)
        {
            for (uint32_t i = 0; i < packets; i++)
            {
                memcpy(stream->assembled, guest::ToHost(guest::PhysicalToGuest(physical + i * kPacketBytes)), kPacketBytes);
                result.ok = stream->SendPacket(0) && result.ok;
            }
        }
        else
        {
            Context context{};
            context.d[5] = physical;
            Context::Set(context.d[0], 0, 12, packets);
            context.SetInput0Valid(true);
            uint32_t length = 0;
            while (ReadFrame(context, *stream, length) == ReadResult::kFrame)
                result.ok = stream->AppendFrame(length) && result.ok;
            result.ok = stream->Flush() && result.ok;
            result.frames = stream->frames;
        }
        result.ok = stream->Drain() && result.ok;
        for (size_t i = stream->pendingOffset; i + 1 < stream->pending.size(); i += 2)
            result.samples.push_back(int16_t((stream->pending[i] << 8) | stream->pending[i + 1]));
        stream->Close();
#else
        (void)physical;
        (void)packets;
        (void)channels;
        (void)sampleRate;
        (void)direct;
#endif
        return result;
    }
}

GUEST_EXPORT(XMACreateContext, XMACreateContext)
GUEST_EXPORT(XMAReleaseContext, XMAReleaseContext)
GUEST_EXPORT(XMAInitializeContext, XMAInitializeContext)
GUEST_EXPORT(XMAEnableContext, XMAEnableContext)
GUEST_EXPORT(XMADisableContext, XMADisableContext)
GUEST_EXPORT(XMABlockWhileInUse, XMABlockWhileInUse)
GUEST_EXPORT(XMASetLoopData, XMASetLoopData)
GUEST_EXPORT(XMASetInputBuffer0, XMASetInputBuffer0)
GUEST_EXPORT(XMASetInputBuffer1, XMASetInputBuffer1)
GUEST_EXPORT(XMASetInputBuffer0Valid, XMASetInputBuffer0Valid)
GUEST_EXPORT(XMASetInputBuffer1Valid, XMASetInputBuffer1Valid)
GUEST_EXPORT(XMAIsInputBuffer0Valid, XMAIsInputBuffer0Valid)
GUEST_EXPORT(XMAIsInputBuffer1Valid, XMAIsInputBuffer1Valid)
GUEST_EXPORT(XMASetInputBufferReadOffset, XMASetInputBufferReadOffset)
GUEST_EXPORT(XMASetOutputBufferValid, XMASetOutputBufferValid)
GUEST_EXPORT(XMAIsOutputBufferValid, XMAIsOutputBufferValid)
GUEST_EXPORT(XMAGetOutputBufferWriteOffset, XMAGetOutputBufferWriteOffset)
GUEST_EXPORT(XMAGetOutputBufferReadOffset, XMAGetOutputBufferReadOffset)
GUEST_EXPORT(XMASetOutputBufferReadOffset, XMASetOutputBufferReadOffset)
