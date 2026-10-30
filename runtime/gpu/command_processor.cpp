#include "gpu/command_processor.h"

#include "gpu/registers.h"
#include "gpu/renderer.h"
#include "gpu/state_audit.h"
#include "guest_memory.h"
#include "kernel/threads.h"
#include "log.h"
#include "util/profiler.h"
#include "util/wake.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <immintrin.h>
#include <intrin.h>
#include <windows.h>

namespace gpu
{
    namespace
    {
        // Register indices (dword offsets in the register window).
        constexpr uint32_t kRegScratchUmsk = 0x01DC;
        constexpr uint32_t kRegScratchAddr = 0x01DD;
        constexpr uint32_t kRegCpRbRptr = 0x01C4;
        constexpr uint32_t kRegCpRbWptr = 0x01C5;
        constexpr uint32_t kRegScratch0 = 0x0578;
        constexpr uint32_t kRegRbbmStatus = 0x05D0;
        constexpr uint32_t kRegCoherStatusHost = 0x0A31;
        constexpr uint32_t kRegVgtEventInitiator = 0x21F9;
        constexpr uint32_t kRegisterCount = kRegisterWindowSize / 4;

        // PM4 type 3 opcodes.
        enum Opcode : uint32_t
        {
            kNop = 0x10,
            kRegRmw = 0x21,
            kDrawIndx = 0x22,
            kVizQuery = 0x23,
            kWaitForIdle = 0x26,
            kImLoad = 0x27,
            kImLoadImmediate = 0x2B,
            kSetConstant = 0x2D,
            kLoadAluConstant = 0x2F,
            kDrawIndx2 = 0x36,
            kIndirectBufferPfd = 0x37,
            kInvalidateState = 0x3B,
            kWaitRegMem = 0x3C,
            kMemWrite = 0x3D,
            kIndirectBuffer = 0x3F,
            kCondWrite = 0x45,
            kEventWrite = 0x46,
            kMeInit = 0x48,
            kSetBinMaskLo = 0x60,
            kSetBinMaskHi = 0x61,
            kSetBinSelectLo = 0x62,
            kSetBinSelectHi = 0x63,
            kSetBinMask = 0x50,
            kSetBinSelect = 0x51,
            kMemWriteCounter = 0x4F,
            kInterrupt = 0x54,
            kSetConstant2 = 0x55,
            kSetShaderConstants = 0x56,
            kEventWriteShd = 0x58,
            kEventWriteCfl = 0x59,
            kEventWriteExt = 0x5A,
            kEventWriteZpd = 0x5B,
            kContextUpdate = 0x5E,
        };

        struct State
        {
            std::mutex lock;
            uint32_t ringPhysical = 0;
            uint32_t ringSizeDwords = 0;
            uint32_t readPointer = 0; // dwords
            uint32_t writeBackPhysical = 0;
            uint32_t interruptCallback = 0;
            uint32_t interruptUserData = 0;
            std::bitset<128> reportedOpcodes;
            std::vector<uint32_t> ucode; // scratch for shader loads
            uint32_t frameCounter = 0;
            // Predicated tiling: a type 3 packet with header bit 0 set only executes when
            // (bin mask & bin select) is nonzero, which D3D uses to run per-tile packets.
            uint64_t binMask = ~0ull;
            uint64_t binSelect = ~0ull;
            uint64_t predicatedSkips = 0;
            bool started = false;
            std::unique_ptr<kernel::GuestCallQueue> interrupts;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        Statistics g_statistics;
        std::unique_ptr<Renderer> g_renderer;

        // Recent type 3 packets: header, the first three data dwords and the buffer they came from.
        struct PacketRecord
        {
            uint32_t header, data[3], buffer;
        };
        constexpr uint32_t kPacketHistory = 8192;
        PacketRecord g_packetHistory[kPacketHistory];
        std::atomic<uint64_t> g_packetCount{ 0 };
        std::atomic<uint32_t> g_currentBuffer{ 0 }; // physical address of the ring or IB being read
        std::atomic<uint32_t> g_waiting{ 0 };       // nonzero while blocked in WAIT_REG_MEM (poll address)
        std::atomic<uint64_t> g_pacingBefore{ 0 };  // while in a frame's pacing wait before its swap: that swap's number
        std::atomic<const char*> g_phase{ "idle" };  // what the command processor thread is doing

        // Threads that wait for each other sleep instead of spinning: guest threads in D3D's GPU wait loops wait for
        // this thread's progress (see WaitForProgress), and this thread waits in WAIT_REG_MEM for interrupts to be
        // handled (D3D's vblank handler releases its frame pacing).
        util::WakeCounter g_progress;
        util::WakeCounter g_interruptsHandled;

        // Discarded frames (BeginDiscard): ranges of frames by swap, [first, last], a frame being the packets up to and
        // including its swap; `last` is kOpen until EndDiscard. Written by the guest's main thread, read by the command
        // processor, which checks the version before each packet that renders and keeps its own copy.
        constexpr uint64_t kOpen = ~0ull;
        std::mutex g_discardLock;
        std::vector<std::pair<uint64_t, uint64_t>> g_discards;
        std::atomic<uint64_t> g_discardVersion{ 0 };
        std::atomic<uint64_t> g_swapsRecorded{ 0 };
        std::atomic<uint64_t> g_discardedFrames{ 0 }, g_discardedDraws{ 0 }, g_discardedWaits{ 0 };
        // This thread's CPU time (TSC cycles) by frame, discarded or drawn (from the swap before to the frame's), and the
        // TSC's rate (from the first swap's TSC and QueryPerformanceCounter).
        std::atomic<uint64_t> g_discardedCycles{ 0 }, g_drawnFrames{ 0 }, g_drawnCycles{ 0 };
        std::atomic<uint64_t> g_firstSwapTsc{ 0 };
        std::atomic<int64_t> g_firstSwapQpc{ 0 };

        void NoteFrameCycles(bool discarded)
        {
            static uint64_t last = 0;
            ULONG64 now = 0;
            QueryThreadCycleTime(GetCurrentThread(), &now);
            if (!g_firstSwapTsc.load(std::memory_order_relaxed))
            {
                LARGE_INTEGER qpc;
                QueryPerformanceCounter(&qpc);
                g_firstSwapQpc.store(qpc.QuadPart, std::memory_order_relaxed);
                g_firstSwapTsc.store(__rdtsc(), std::memory_order_relaxed);
            }
            if (last)
            {
                if (discarded)
                    g_discardedCycles.fetch_add(now - last, std::memory_order_relaxed);
                else
                {
                    g_drawnCycles.fetch_add(now - last, std::memory_order_relaxed);
                    g_drawnFrames.fetch_add(1, std::memory_order_relaxed);
                }
            }
            last = now;
        }

        // Command processor thread: whether the frame it's executing (its next swap's) is discarded.
        bool Discarding()
        {
            static uint64_t version = 0;
            static std::vector<std::pair<uint64_t, uint64_t>> ranges;
            if (const uint64_t now = g_discardVersion.load(std::memory_order_acquire); now != version)
            {
                std::lock_guard lock(g_discardLock);
                version = g_discardVersion.load(std::memory_order_relaxed);
                ranges = g_discards;
            }
            if (ranges.empty())
                return false;
            const uint64_t frame = g_statistics.swaps.load(std::memory_order_relaxed) + 1;
            for (const auto& [first, last] : ranges)
            {
                if (frame >= first && frame <= last)
                    return true;
            }
            return false;
        }

        // GPU_TRACE=first,count logs the command stream (render state, draws, shader loads,
        // swaps) for `count` frames starting at frame `first`. GPU_TRACE_BRIEF=1 leaves out
        // register writes, and stops after GPU_TRACE_LIMIT packets (default 20000).
        struct Trace
        {
            uint32_t first = 0;
            uint32_t count = 0;
            bool brief = false;
            uint64_t limit = 20000;

            Trace()
            {
                if (const char* setting = std::getenv(GAME_PREFIX "_GPU_TRACE"))
                {
                    char* end = nullptr;
                    first = uint32_t(strtoul(setting, &end, 10));
                    count = (end && *end == ',') ? uint32_t(strtoul(end + 1, nullptr, 10)) : 1;
                }
                if (const char* setting = std::getenv(GAME_PREFIX "_GPU_TRACE_BRIEF"))
                    brief = std::atoi(setting) != 0;
                if (const char* setting = std::getenv(GAME_PREFIX "_GPU_TRACE_LIMIT"))
                    limit = strtoull(setting, nullptr, 10);
            }
        };

        const Trace& TraceSettings()
        {
            static const Trace trace;
            return trace;
        }

        bool Tracing()
        {
            const Trace& trace = TraceSettings();
            if (!trace.count)
                return false;
            const uint32_t frame = uint32_t(g_statistics.swaps.load());
            if (frame < trace.first || frame >= trace.first + trace.count)
                return false;
            static std::atomic<uint64_t> traced{ 0 };
            return traced++ < trace.limit;
        }

        bool TracingRegisters()
        {
            return !TraceSettings().brief && Tracing();
        }

        const char* PrimitiveName(uint32_t type)
        {
            switch (type)
            {
            case kPointList: return "point list";
            case kLineList: return "line list";
            case kLineStrip: return "line strip";
            case kTriangleList: return "triangle list";
            case kTriangleFan: return "triangle fan";
            case kTriangleStrip: return "triangle strip";
            case kRectangleList: return "rectangle list";
            case kLineLoop: return "line loop";
            case kQuadList: return "quad list";
            case kQuadStrip: return "quad strip";
            default: return "other";
            }
        }

        uint32_t LoadPhysical(uint32_t physical)
        {
            return guest::LoadU32(guest::PhysicalToGuest(physical));
        }

        // GPU memory writes carry an endian mode in the address's low two bits.
        uint32_t SwapForEndian(uint32_t value, uint32_t endian)
        {
            switch (endian)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF); // 8-in-16
            case 2: return __builtin_bswap32(value);                                   // 8-in-32
            case 3: return (value << 16) | (value >> 16);                              // 16-in-32
            default: return value;
            }
        }

        void WriteMemory(uint32_t addressAndEndian, uint32_t value)
        {
            const uint32_t address = guest::PhysicalToGuest(addressAndEndian & ~3u);
            // Guest memory is big-endian; the endian mode is applied to the value as the GPU sees it.
            guest::BeforeHostWrite(address, 4);
            *reinterpret_cast<uint32_t*>(guest::ToHost(address)) = SwapForEndian(value, addressAndEndian & 3);
            g_progress.Notify(); // fences
        }

        uint32_t ReadMemory(uint32_t addressAndEndian)
        {
            const uint32_t address = guest::PhysicalToGuest(addressAndEndian & ~3u);
            return SwapForEndian(*reinterpret_cast<const uint32_t*>(guest::ToHost(address)), addressAndEndian & 3);
        }

        // The register file is the guest register window itself: the CPU also writes registers there
        // directly, so waits on registers must see those writes.
        void PublishRegister(uint32_t index, uint32_t value)
        {
            guest::StoreU32(kRegisterWindow + index * 4, value);
        }

        uint32_t ReadRegister(uint32_t index)
        {
            return index < kRegisterCount ? guest::LoadU32(kRegisterWindow + index * 4) : 0;
        }

        // Interrupts are delivered to each CPU in `cpuMask`, with the interrupt thread's current CPU set
        // accordingly: D3D's handler acknowledges by clearing its own CPU's bit, and the GPU may wait for all of them.
        // Command processor interrupts (source 1) complete before the CP continues, as D3D's handler reads
        // callback state that the following packets may overwrite.
        void DispatchInterrupt(uint32_t source, uint32_t cpuMask)
        {
            State& state = GetState();
            if (!state.interruptCallback || !state.interrupts)
                return;
            uint64_t ticket = 0;
            for (int cpu = 0; cpu < 6; cpu++)
            {
                if (cpuMask & (1u << cpu))
                {
                    ticket = state.interrupts->Post(state.interruptCallback, { source, state.interruptUserData }, cpu);
                    profile::Count(profile::Counter::kInterrupts);
                }
            }
            if (source == 1 && ticket)
            {
                g_phase = "waiting for an interrupt callback";
                profile::ScopedPhase phase(profile::Zone::kWaitInterrupt);
                state.interrupts->Wait(ticket);
                g_phase = "executing";
            }
        }

        // The display's gamma ramp (see SwapCommand::gammaRamp): identity until the game writes it.
        uint32_t g_gammaRamp[256];
        bool g_gammaRampChanged = false;

        bool InitializeGammaRamp()
        {
            for (uint32_t i = 0; i < 256; i++)
            {
                const uint32_t value = (i << 2) | (i >> 6); // 8 to 10 bits
                g_gammaRamp[i] = value | (value << 10) | (value << 20);
            }
            return true;
        }
        [[maybe_unused]] const bool g_gammaRampInitialized = InitializeGammaRamp();

        // DC_LUT_* writes. D3D sets the ramp in table mode, an index then a 30-bit color for each entry. The other ways to
        // write it (piecewise linear, sequential components, LUT B) are reported once.
        void WriteGammaRamp(uint32_t index, uint32_t value)
        {
            if (index == reg::DC_LUT_30_COLOR)
            {
                const uint32_t entry = ReadRegister(reg::DC_LUT_RW_INDEX) & 0xFF;
                const uint32_t mask = ReadRegister(reg::DC_LUT_WRITE_EN_MASK);
                uint32_t components = 0;
                for (uint32_t component = 0; component < 3; component++)
                {
                    if (mask & (1u << component))
                        components |= 0x3FFu << (component * 10);
                }
                const uint32_t updated = (g_gammaRamp[entry] & ~components) | (value & components);
                g_gammaRampChanged |= updated != g_gammaRamp[entry];
                g_gammaRamp[entry] = updated;
                PublishRegister(reg::DC_LUT_RW_INDEX, (entry + 1) & 0xFF);
                return;
            }
            const bool unsupported = index == reg::DC_LUT_SEQ_COLOR || index == reg::DC_LUT_PWL_DATA ||
                ((index == reg::DC_LUT_RW_SELECT || index == reg::DC_LUT_RW_MODE) && (value & 1));
            static bool reported[16];
            if (unsupported && !reported[index & 15])
            {
                reported[index & 15] = true;
                LOG_WARN("GPU: display gamma ramp register %04X = %08X not supported (only the 256-entry table of LUT A is)", index, value);
            }
        }

        void WriteRegister(uint32_t index, uint32_t value)
        {
            if (index >= kRegisterCount)
                return;

            if (index == kRegCoherStatusHost && (value & 0x80000000))
                value &= ~0x80000000u; // coherency requests complete immediately

            PublishRegister(index, value);
            audit::RegisterWritten(index, value);

            if (index >= reg::DC_LUT_RW_SELECT && index <= reg::DC_LUT_WRITE_EN_MASK)
                WriteGammaRamp(index, value);

            if ((index >= 0x2000 && index < 0x2400) || (index >= reg::SHADER_CONSTANT_FETCH_00_0 && index < 0x4930))
            {
                if (TracingRegisters())
                    fprintf(stderr, "[gpu]   reg %04X = %08X\n", index, value);
            }

            // Scratch registers are also written to memory when enabled in SCRATCH_UMSK.
            if (index >= kRegScratch0 && index < kRegScratch0 + 8)
            {
                const uint32_t scratch = index - kRegScratch0;
                if (ReadRegister(kRegScratchUmsk) & (1u << scratch))
                    WriteMemory(ReadRegister(kRegScratchAddr) + scratch * 4, __builtin_bswap32(value));
            }
        }

        // The ring buffer read pointer, as the guest sees it: the register and its write-back copy in memory.
        void PublishReadPointer(uint32_t position)
        {
            PublishRegister(kRegCpRbRptr, position);
            if (const uint32_t writeBack = GetState().writeBackPhysical)
                guest::StoreU32(guest::PhysicalToGuest(writeBack), position);
            g_progress.Notify();
        }

        // Reads dwords from a ring (wrapping) or a linear buffer.
        class Reader
        {
        public:
            // Ring and indirect buffers are physically contiguous, so the base is translated once.
            Reader(uint32_t physicalBase, uint32_t sizeDwords, uint32_t start, uint32_t end)
                : m_base(guest::PhysicalToGuest(physicalBase)), m_size(sizeDwords), m_position(start), m_end(end)
            {
            }

            bool AtEnd() const { return m_position == m_end; }
            uint32_t Remaining() const { return (m_end + m_size - m_position) % m_size; }
            uint32_t Position() const { return m_position; }

            uint32_t Read()
            {
                const uint32_t value = guest::LoadU32(m_base + m_position * 4);
                m_position = (m_position + 1) % m_size;
                return value;
            }

            uint32_t Peek(uint32_t offset) const
            {
                return guest::LoadU32(m_base + ((m_position + offset) % m_size) * 4);
            }

            void Skip(uint32_t count)
            {
                m_position = (m_position + count) % m_size;
            }

        private:
            uint32_t m_base, m_size, m_position, m_end;
        };

        bool CompareWait(uint32_t function, uint32_t value, uint32_t reference)
        {
            switch (function & 7)
            {
            case 0: return false;
            case 1: return value < reference;
            case 2: return value <= reference;
            case 3: return value == reference;
            case 4: return value != reference;
            case 5: return value >= reference;
            case 6: return value > reference;
            default: return true;
            }
        }

        uint32_t PollValue(uint32_t waitInfo, uint32_t pollAddress)
        {
            if (waitInfo & 0x10)
                return ReadMemory(pollAddress);
            return ReadRegister(pollAddress);
        }

        void ExecuteBuffer(Reader& reader, int depth);

        void ExecuteType3(Reader& reader, uint32_t header, int depth)
        {
            State& state = GetState();
            const uint32_t opcode = (header >> 8) & 0x7F;
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            uint32_t consumed = 0;
            {
                PacketRecord& record = g_packetHistory[g_packetCount.fetch_add(1, std::memory_order_relaxed) % kPacketHistory];
                record.header = header;
                for (uint32_t i = 0; i < 3; i++)
                    record.data[i] = i < count ? reader.Peek(i) : 0;
                record.buffer = g_currentBuffer.load(std::memory_order_relaxed);
            }
            const auto read = [&] {
                consumed++;
                return reader.Read();
            };

            if ((header & 1) && !(state.binMask & state.binSelect))
            {
                state.predicatedSkips++;
                g_statistics.predicatedSkips++;
                if (Tracing())
                    fprintf(stderr, "[gpu] (skipped predicated op %02X)\n", opcode);
                reader.Skip(count);
                return;
            }

            switch (opcode)
            {
            case kMeInit:
            case kNop:
            case kWaitForIdle:
            case kInvalidateState:
            case kContextUpdate:
            case kVizQuery:
            case kMemWriteCounter:
            case kEventWriteZpd:
                break;

            case kSetBinMask:
            case kSetBinSelect:
            {
                const uint64_t high = read(), low = read();
                (opcode == kSetBinMask ? state.binMask : state.binSelect) = (high << 32) | low;
                if (Tracing())
                    fprintf(stderr, "[gpu] %s = %016llX\n", opcode == kSetBinMask ? "SET_BIN_MASK" : "SET_BIN_SELECT",
                        (unsigned long long)((high << 32) | low));
                break;
            }

            case kSetBinMaskLo:
            case kSetBinMaskHi:
            case kSetBinSelectLo:
            case kSetBinSelectHi:
            {
                const uint64_t value = read();
                uint64_t& target = (opcode == kSetBinMaskLo || opcode == kSetBinMaskHi) ? state.binMask : state.binSelect;
                if (opcode == kSetBinMaskHi || opcode == kSetBinSelectHi)
                    target = (target & 0xFFFFFFFFull) | (value << 32);
                else
                    target = (target & 0xFFFFFFFF00000000ull) | value;
                break;
            }

            case kImLoad:
            case kImLoadImmediate:
            {
                profile::ScopedPhase phase(profile::Zone::kShaderLoad);
                const uint32_t first = read(), sizeInfo = read();
                const uint32_t type = first & 3;
                const uint32_t dwords = sizeInfo & 0xFFFF;
                // The microcode in guest byte order: in guest memory (D3D loads the same shaders over and
                // over, so it isn't copied), or copied out of the packet.
                const uint32_t* ucode;
                uint32_t ucodeDwords;
                if (opcode == kImLoad)
                {
                    ucode = reinterpret_cast<const uint32_t*>(guest::ToHost(guest::PhysicalToGuest(first & ~3u)));
                    ucodeDwords = dwords;
                }
                else
                {
                    state.ucode.clear();
                    for (uint32_t i = 0; i < dwords && consumed < count; i++)
                        state.ucode.push_back(__builtin_bswap32(read()));
                    ucode = state.ucode.data();
                    ucodeDwords = uint32_t(state.ucode.size());
                }
                if (Tracing())
                {
                    uint32_t hash = 2166136261u;
                    for (uint32_t i = 0; i < ucodeDwords; i++)
                        hash = (hash ^ __builtin_bswap32(ucode[i])) * 16777619u;
                    fprintf(stderr, "[gpu] %s %s shader: %u dwords%s, hash %08X\n", opcode == kImLoad ? "IM_LOAD" : "IM_LOAD_IMMEDIATE",
                        type == 0 ? "vertex" : "pixel", dwords, opcode == kImLoad ? "" : " (inline)", hash);
                }
                profile::Count(profile::Counter::kShaderLoads);
                profile::Count(profile::Counter::kShaderLoadDwords, ucodeDwords);
                // Also in discarded frames: D3D loads a shader only when it changes, so the frames after them may draw
                // with one these loaded.
                if (g_renderer)
                    g_renderer->LoadShader(type == 0 ? ShaderStage::kVertex : ShaderStage::kPixel, ucode, ucodeDwords);
                break;
            }

            case kDrawIndx:
            case kDrawIndx2:
            {
                g_statistics.draws++;
                profile::Count(profile::Counter::kDrawPackets);
                DrawCommand draw;
                if (opcode == kDrawIndx)
                    read(); // visibility query
                draw.initiator = DrawInitiator{ read() };
                if (draw.initiator.SourceSelect() == 0 && opcode == kDrawIndx)
                {
                    draw.indexBase = read();
                    const uint32_t size = read();
                    draw.indexSize = size & 0xFFFFFF;
                    draw.indexEndian = size >> 30;
                }
                if (Tracing())
                {
                    char indices[96] = "";
                    if (draw.initiator.SourceSelect() == 0)
                        snprintf(indices, sizeof(indices), ", indices at %08X (%u x %s, endian %u)", draw.indexBase, draw.indexSize,
                            draw.initiator.Index32() ? "u32" : "u16", draw.indexEndian);
                    fprintf(stderr, "[gpu] DRAW %s, %u indices, source %u%s, edram mode %u\n", PrimitiveName(draw.initiator.PrimitiveType()),
                        draw.initiator.NumIndices(), draw.initiator.SourceSelect(), indices, ReadRegister(reg::RB_MODECONTROL) & 7);
                }
                // Immediate (inline) indices aren't supported; D3D doesn't use them for DOA4.
                if (Discarding())
                    g_discardedDraws.fetch_add(1, std::memory_order_relaxed);
                else if (g_renderer && draw.initiator.SourceSelect() != 1)
                {
                    g_phase = "renderer draw";
                    profile::ScopedPhase phase(profile::Zone::kDraw);
                    g_renderer->Draw(draw);
                    g_phase = "executing";
                }
                break;
            }

            case kInterrupt:
            {
                const uint32_t cpuMask = read();
                if (Tracing())
                    fprintf(stderr, "[gpu] INTERRUPT cpus %02X\n", cpuMask);
                DispatchInterrupt(1, cpuMask);
                break;
            }

            case kOpcodeXeSwap:
            {
                read(); // 'SWAP'
                SwapCommand swap;
                swap.frontBuffer = read();
                swap.width = read();
                swap.height = read();
                for (uint32_t& dword : swap.fetch)
                    dword = read();
                if (count >= 14 && read() == kSwapFrameTag)
                {
                    swap.gameFrame = read();
                    swap.fightFrame = int64_t(read()) - 1;
                }
                swap.gammaRamp = g_gammaRamp;
                if (g_gammaRampChanged)
                {
                    g_gammaRampChanged = false;
                    static uint32_t logged = 0;
                    if (logged++ < 32)
                        LOG_INFO("GPU: display gamma ramp set (of 1023: 16 -> %u, 64 -> %u, 128 -> %u, 192 -> %u, 255 -> %u)%s",
                            (g_gammaRamp[16] >> 10) & 0x3FF, (g_gammaRamp[64] >> 10) & 0x3FF, (g_gammaRamp[128] >> 10) & 0x3FF,
                            (g_gammaRamp[192] >> 10) & 0x3FF, (g_gammaRamp[255] >> 10) & 0x3FF, logged == 32 ? "; later changes aren't logged" : "");
                }
                if (Tracing())
                    fprintf(stderr, "[gpu] SWAP front buffer %08X (%u x %u), fetch %08X %08X %08X %08X %08X %08X ======== frame %llu\n", swap.frontBuffer,
                        swap.width, swap.height, swap.fetch[0], swap.fetch[1], swap.fetch[2], swap.fetch[3], swap.fetch[4], swap.fetch[5],
                        (unsigned long long)g_statistics.swaps.load());
                const bool discarded = Discarding();
                if (discarded)
                    g_discardedFrames.fetch_add(1, std::memory_order_relaxed);
                else if (g_renderer)
                {
                    g_phase = "renderer swap";
                    profile::ScopedPhase phase(profile::Zone::kSwap);
                    g_renderer->Swap(swap);
                    g_phase = "executing";
                }
                NoteFrameCycles(discarded);
                g_statistics.swaps++;
                state.frameCounter++;
                profile::EndFrame();
                break;
            }

            case kIndirectBuffer:
            case kIndirectBufferPfd:
            {
                const uint32_t address = read();
                const uint32_t length = read() & 0xFFFFF;
                if (Tracing())
                    fprintf(stderr, "[gpu] INDIRECT_BUFFER %08X, %u dwords (depth %d)\n", address, length, depth);
                if (depth < 4 && length)
                {
                    const uint32_t outer = g_currentBuffer.exchange(address, std::memory_order_relaxed);
                    Reader sub(address, length + 1, 0, length);
                    ExecuteBuffer(sub, depth + 1);
                    g_currentBuffer.store(outer, std::memory_order_relaxed);
                }
                break;
            }

            case kWaitRegMem:
            {
                const uint32_t waitInfo = read(), pollAddress = read(), reference = read(), mask = read();
                read(); // wait interval
                if (Tracing())
                    fprintf(stderr, "[gpu] WAIT_REG_MEM %s %08X & %08X func %u ref %08X (now %08X)\n", (waitInfo & 0x10) ? "mem" : "reg", pollAddress,
                        mask, waitInfo & 7, reference, PollValue(waitInfo, pollAddress) & mask);
                // In a discarded frame, the frame pacing's wait before its swap (for D3D's flag in memory to be cleared,
                // which its vblank handler does once the presentation interval has passed) is released at once: D3D's
                // swap callback has set the flag, and a vblank would clear it.
                const bool beforeSwap = reader.Remaining() > 0 && (reader.Peek(0) >> 30) == 3 && ((reader.Peek(0) >> 8) & 0x7F) == kOpcodeXeSwap;
                if ((waitInfo & 0x10) && (waitInfo & 7) == 3 && beforeSwap && Discarding() &&
                    !CompareWait(waitInfo, PollValue(waitInfo, pollAddress) & mask, reference))
                {
                    WriteMemory(pollAddress, (ReadMemory(pollAddress) & ~mask) | (reference & mask));
                    g_discardedWaits.fetch_add(1, std::memory_order_relaxed);
                }
                const auto start = std::chrono::steady_clock::now();
                bool reported = false;
                profile::ScopedPhase phase(profile::Zone::kWaitRegMem);
                g_waiting.store(pollAddress | 1, std::memory_order_relaxed);
                // A frame's pacing wait before its swap: the frame's draws are all executed (WaitForRecordedDraws).
                const bool pacing = (waitInfo & 0x10) && beforeSwap;
                if (pacing)
                {
                    g_pacingBefore.store(g_statistics.swaps.load(std::memory_order_relaxed) + 1, std::memory_order_release);
                    g_progress.Notify();
                }
                // DOA4 waits here for its frame pacing, which D3D's vblank handler releases: after a short spin,
                // sleep until an interrupt has been handled (or for a millisecond, in case the CPU is what writes).
                uint64_t handled = g_interruptsHandled.Value();
                while (!CompareWait(waitInfo, PollValue(waitInfo, pollAddress) & mask, reference))
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - start < std::chrono::microseconds(50))
                    {
                        for (int i = 0; i < 32; i++)
                            _mm_pause();
                    }
                    else
                    {
                        handled = g_interruptsHandled.Wait(handled, 1);
                    }
                    if (!reported && now - start > std::chrono::seconds(5))
                    {
                        LOG_WARN("GPU: WAIT_REG_MEM on %s 0x%08X (mask 0x%08X, ref 0x%08X, func %u) still waiting",
                            (waitInfo & 0x10) ? "memory" : "register", pollAddress, mask, reference, waitInfo & 7);
                        reported = true;
                    }
                }
                if (pacing)
                    g_pacingBefore.store(0, std::memory_order_release);
                g_waiting.store(0, std::memory_order_relaxed);
                break;
            }

            case kRegRmw:
            {
                const uint32_t info = read(), andMask = read(), orMask = read();
                uint32_t value = ReadRegister(info & 0x1FFF);
                value &= (info >> 31) ? ReadRegister(andMask & 0x1FFF) : andMask;
                value |= ((info >> 30) & 1) ? ReadRegister(orMask & 0x1FFF) : orMask;
                WriteRegister(info & 0x1FFF, value);
                break;
            }

            case kCondWrite:
            {
                const uint32_t waitInfo = read(), pollAddress = read(), reference = read(), mask = read();
                const uint32_t writeAddress = read(), writeData = read();
                if (CompareWait(waitInfo, PollValue(waitInfo, pollAddress) & mask, reference))
                {
                    if (waitInfo & 0x100)
                        WriteMemory(writeAddress, writeData);
                    else
                        WriteRegister(writeAddress, writeData);
                }
                break;
            }

            case kEventWrite:
                WriteRegister(kRegVgtEventInitiator, read() & 0x3F);
                break;

            case kEventWriteShd:
            {
                const uint32_t initiator = read(), address = read(), value = read();
                WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
                if (Tracing())
                    fprintf(stderr, "[gpu] EVENT_WRITE_SHD event %u to %08X = %08X\n", initiator & 0x3F, address,
                        (initiator >> 31) ? state.frameCounter : value);
                WriteMemory(address, (initiator >> 31) ? state.frameCounter : value);
                break;
            }

            case kEventWriteExt:
            {
                const uint32_t initiator = read(), address = read();
                WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
                // Screen extents (min x, max x, min y, max y, min z, max z) as 16-bit values.
                // (Predicated tiling uses them to skip draws outside a tile; covering everything is safe.)
                const uint16_t extents[6] = { 0, 2560 >> 3, 0, 2560 >> 3, 0, 1 };
                const uint32_t target = guest::PhysicalToGuest(address & ~3u);
                guest::BeforeHostWrite(target, 12);
                for (int i = 0; i < 6; i++)
                    *reinterpret_cast<uint16_t*>(guest::ToHost(target + i * 2)) = __builtin_bswap16(extents[i]);
                break;
            }

            case kMemWrite:
            {
                const uint32_t address = read();
                if (Tracing())
                    fprintf(stderr, "[gpu] MEM_WRITE %08X, %u dwords\n", address, count - 1);
                for (uint32_t i = 1; i < count; i++)
                    WriteMemory(address + (i - 1) * 4, read());
                break;
            }

            case kSetConstant:
            {
                const uint32_t offsetType = read();
                const uint32_t index = offsetType & 0x7FF;
                static constexpr uint32_t kBases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                const uint32_t base = type < 5 ? kBases[type] : 0x2000;
                if (Tracing())
                    fprintf(stderr, "[gpu] SET_CONSTANT %u dwords to register %04X\n", count - 1, base + index);
                for (uint32_t i = 1; i < count; i++)
                    WriteRegister(base + index + i - 1, read());
                break;
            }

            case kSetConstant2:
            case kSetShaderConstants:
            {
                const uint32_t index = read() & 0xFFFF;
                for (uint32_t i = 1; i < count; i++)
                    WriteRegister(index + i - 1, read());
                break;
            }

            case kLoadAluConstant:
            {
                const uint32_t address = read() & 0x3FFFFFFF, offsetType = read(), size = read();
                const uint32_t index = offsetType & 0x7FF;
                static constexpr uint32_t kBases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                const uint32_t base = type < 5 ? kBases[type] : 0x2000;
                if (Tracing())
                    fprintf(stderr, "[gpu] LOAD_ALU_CONSTANT %08X: %u dwords to register %04X\n", address, size, base + index);
                for (uint32_t i = 0; i < size; i++)
                    WriteRegister(base + index + i, LoadPhysical(address + i * 4));
                break;
            }

            default:
                if (!state.reportedOpcodes[opcode])
                {
                    state.reportedOpcodes[opcode] = true;
                    LOG_WARN("GPU: unhandled PM4 opcode 0x%02X (%u dwords)", opcode, count);
                }
                break;
            }

            // Skip any data the handler didn't consume.
            if (consumed < count)
                reader.Skip(count - consumed);
        }

        void ExecuteBuffer(Reader& reader, int depth)
        {
            while (!reader.AtEnd())
            {
                const uint32_t header = reader.Read();
                g_statistics.packets++;
                switch (header >> 30)
                {
                case 0:
                {
                    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    const uint32_t base = header & 0x7FFF;
                    const bool sameRegister = header & 0x8000;
                    for (uint32_t i = 0; i < count; i++)
                        WriteRegister(sameRegister ? base : base + i, reader.Read());
                    break;
                }
                case 1:
                    WriteRegister(header & 0x7FF, reader.Read());
                    WriteRegister((header >> 11) & 0x7FF, reader.Read());
                    break;
                case 2:
                    break; // NOP
                case 3:
                    ExecuteType3(reader, header, depth);
                    break;
                }
                // The ring's read pointer advances as its packets complete (indirect buffers included), as the
                // hardware reports progress, not only at the end of a batch: D3D waits for it to pass positions
                // before reusing command buffer space.
                if (depth == 0)
                    PublishReadPointer(reader.Position());
            }
        }

        // Display controller registers the CPU reads directly, with the fixed values Xenia reports.
        // D3D's vblank interrupt handler ignores vblanks unless 0x6544 bit 0 (vblank pending) is set.
        void PublishDisplayRegisters()
        {
            const std::pair<uint32_t, uint32_t> registers[] = {
                { 0x3C00, 0x08100748 },
                { 0x3C04, 0x0000200E },
                { 0x6530, 0x000002D0 }, // scanline
                { 0x6544, 0x00000001 }, // vblank pending
                { 0x6584, 0x050002D0 }, // display size: 1280x720
            };
            for (const auto& [byteOffset, value] : registers)
                guest::StoreU32(kRegisterWindow + byteOffset, value);
        }

        void CommandProcessorThread()
        {
            profile::NameThread("GPU command processor");
            State& state = GetState();
            PublishRegister(kRegRbbmStatus, 0); // idle
            PublishDisplayRegisters();
            std::chrono::steady_clock::time_point idleSince;
            bool idle = false;
            for (;;)
            {
                uint32_t ringPhysical, ringSize, readPointer, writePointer;
                {
                    std::lock_guard lock(state.lock);
                    ringPhysical = state.ringPhysical;
                    ringSize = state.ringSizeDwords;
                    readPointer = state.readPointer;
                }
                writePointer = guest::LoadU32(kRegisterWindow + kRegCpRbWptr * 4);

                if (!ringPhysical || ringSize == 0 || writePointer == readPointer || writePointer >= ringSize)
                {
                    profile::SetPhase(profile::Zone::kIdle);
                    // The guest usually adds commands soon (it often waits for this thread), and a sleep takes
                    // a millisecond or more on Windows: poll for a while first.
                    const auto now = std::chrono::steady_clock::now();
                    if (!idle)
                    {
                        idle = true;
                        idleSince = now;
                    }
                    if (now - idleSince < std::chrono::microseconds(500))
                    {
                        for (int i = 0; i < 32; i++)
                            _mm_pause();
                    }
                    else
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                    continue;
                }
                idle = false;
                profile::SetPhase(profile::Zone::kExecute);

                Reader reader(ringPhysical, ringSize, readPointer, writePointer);
                g_currentBuffer.store(ringPhysical, std::memory_order_relaxed);
                g_phase = "executing";
                ExecuteBuffer(reader, 0);
                g_phase = "idle";

                {
                    std::lock_guard lock(state.lock);
                    state.readPointer = writePointer;
                }
                PublishReadPointer(writePointer);
            }
        }

        // Netplay's time sync (DelayVblanks): the delay still to come and the most a vblank takes of it (microseconds), and
        // the delay given in all.
        std::atomic<uint64_t> g_vblankDelay{ 0 }, g_vblankDelayGiven{ 0 };
        std::atomic<uint32_t> g_vblankDelayStep{ 0 };

        // VBLANK_HZ=n raises the vblank rate from 60 Hz (diagnostics: the game runs as fast as vblanks
        // allow, so a high rate measures how fast it could go).
        void VblankThread()
        {
            using namespace std::chrono;
            profile::NameThread("GPU vblank");
            const char* rate = std::getenv(GAME_PREFIX "_VBLANK_HZ");
            const int64_t hertz = rate ? std::max<int64_t>(std::atoll(rate), 1) : 60;
            const nanoseconds interval(hertz == 60 ? 16667000 : 1000000000 / hertz);
            auto next = steady_clock::now();
            for (;;)
            {
                next += interval;
                // The time sync's delay, a little a vblank (only this thread takes from it; DelayVblanks adds).
                if (const uint64_t pending = g_vblankDelay.load(std::memory_order_relaxed))
                {
                    const uint64_t take = std::min<uint64_t>(pending, std::max<uint32_t>(g_vblankDelayStep.load(std::memory_order_relaxed), 1));
                    g_vblankDelay.fetch_sub(take, std::memory_order_relaxed);
                    g_vblankDelayGiven.fetch_add(take, std::memory_order_relaxed);
                    next += microseconds(take);
                }
                std::this_thread::sleep_until(next);
                g_statistics.vblanks++;
                DispatchInterrupt(0, 1u << 2); // vblanks go to CPU 2, as in Xenia
            }
        }
    }

    Statistics& GetStatistics()
    {
        return g_statistics;
    }

    void DelayVblanks(uint64_t microseconds, uint32_t perVblank)
    {
        g_vblankDelayStep.store(perVblank, std::memory_order_relaxed);
        g_vblankDelay.fetch_add(microseconds, std::memory_order_relaxed);
    }

    uint64_t PendingVblankDelay()
    {
        return g_vblankDelay.load(std::memory_order_relaxed);
    }

    uint64_t VblankDelayGiven()
    {
        return g_vblankDelayGiven.load(std::memory_order_relaxed);
    }

    void BeginDiscard()
    {
        std::lock_guard lock(g_discardLock);
        // Ranges the command processor has passed are dropped.
        const uint64_t executed = g_statistics.swaps.load(std::memory_order_relaxed);
        std::erase_if(g_discards, [&](const std::pair<uint64_t, uint64_t>& range) { return range.second != kOpen && range.second <= executed; });
        g_discards.emplace_back(g_swapsRecorded.load(std::memory_order_relaxed) + 1, kOpen);
        g_discardVersion.fetch_add(1, std::memory_order_release);
    }

    void EndDiscard()
    {
        std::lock_guard lock(g_discardLock);
        if (g_discards.empty() || g_discards.back().second != kOpen)
            return;
        g_discards.back().second = g_swapsRecorded.load(std::memory_order_relaxed);
        if (g_discards.back().second < g_discards.back().first)
            g_discards.pop_back(); // no frame was recorded meanwhile
        g_discardVersion.fetch_add(1, std::memory_order_release);
    }

    void NoteSwapRecorded()
    {
        g_swapsRecorded.fetch_add(1, std::memory_order_relaxed);
    }

    bool DiscardingCurrentFrame()
    {
        // (A frame's place in the ranges is settled once it's recorded, before the command processor gets to it.)
        std::lock_guard lock(g_discardLock);
        const uint64_t frame = g_statistics.swaps.load(std::memory_order_relaxed) + 1;
        for (const auto& [first, last] : g_discards)
        {
            if (frame >= first && frame <= last)
                return true;
        }
        return false;
    }

    uint64_t SwapsPending()
    {
        const uint64_t recorded = g_swapsRecorded.load(std::memory_order_relaxed), executed = g_statistics.swaps.load(std::memory_order_relaxed);
        return recorded > executed ? recorded - executed : 0;
    }

    DiscardStatistics GetDiscardStatistics()
    {
        DiscardStatistics statistics{ g_discardedFrames.load(), g_discardedDraws.load(), g_discardedWaits.load() };
        // The TSC's cycles a microsecond, since the first swap.
        LARGE_INTEGER qpc, frequency;
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&frequency);
        const double elapsedUs = double(qpc.QuadPart - g_firstSwapQpc.load()) * 1e6 / double(frequency.QuadPart);
        const double cyclesPerUs = elapsedUs > 0 && g_firstSwapTsc.load() ? double(__rdtsc() - g_firstSwapTsc.load()) / elapsedUs : 0;
        if (cyclesPerUs > 0)
        {
            statistics.discardedMicroseconds = double(g_discardedCycles.load()) / cyclesPerUs;
            statistics.drawnMicroseconds = double(g_drawnCycles.load()) / cyclesPerUs;
        }
        statistics.drawnFrames = g_drawnFrames.load();
        return statistics;
    }

    static thread_local uint64_t t_waitedMicroseconds = 0; // WaitForProgress's blocking, the calling thread's

    void WaitForProgress()
    {
        // Progress since this thread's last call counts: its wait loop has checked its condition since then.
        thread_local uint64_t seen = UINT64_MAX;
        uint64_t now = g_progress.Value();
        if (now == seen)
        {
            const auto start = std::chrono::steady_clock::now();
            now = g_progress.Wait(now, 1);
            t_waitedMicroseconds +=
                uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        }
        seen = now;
    }

    uint64_t ThreadWaitedMicroseconds()
    {
        return t_waitedMicroseconds;
    }

    uint64_t WaitForRecordedDraws(uint64_t limitMicroseconds)
    {
        const auto start = std::chrono::steady_clock::now();
        const uint64_t recorded = g_swapsRecorded.load(std::memory_order_relaxed);
        uint64_t seen = g_progress.Value();
        for (;;)
        {
            // The last recorded frame's swap executed, or its pacing wait before the swap under way: its draws are done.
            const uint64_t executed = g_statistics.swaps.load(std::memory_order_acquire);
            const uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
            if (executed >= recorded || (executed + 1 == recorded && g_pacingBefore.load(std::memory_order_acquire) == recorded) ||
                elapsed >= limitMicroseconds)
                return elapsed;
            seen = g_progress.Wait(seen, 1);
        }
    }

    void DumpRecentPackets(uint32_t count)
    {
        State& state = GetState();
        const uint64_t total = g_packetCount.load();
        count = uint32_t(std::min<uint64_t>({ count, total, kPacketHistory }));
        fprintf(stderr, "[gpu] ring: read %u, write %u (of %u dwords); %llu type 3 packets; %s (%s)\n", state.readPointer,
            guest::LoadU32(kRegisterWindow + kRegCpRbWptr * 4), state.ringSizeDwords, (unsigned long long)total,
            g_waiting.load() ? "waiting in WAIT_REG_MEM" : "not waiting", g_phase.load());
        for (uint64_t i = total - count; i < total; i++)
        {
            const PacketRecord& record = g_packetHistory[i % kPacketHistory];
            fprintf(stderr, "[gpu]   #%llu op %02X len %u: %08X %08X %08X (buffer %08X)\n", (unsigned long long)i, (record.header >> 8) & 0x7F,
                ((record.header >> 16) & 0x3FFF) + 1, record.data[0], record.data[1], record.data[2], record.buffer);
        }
        fflush(stderr);
    }

    void SetRenderer(std::unique_ptr<Renderer> renderer)
    {
        g_renderer = std::move(renderer);
    }

    uint32_t Register(uint32_t index)
    {
        return ReadRegister(index);
    }

    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        state.ringPhysical = physicalAddress;
        state.ringSizeDwords = (1u << (sizeLog2 + 3)) / 4;
        state.readPointer = 0;
        LOG_INFO("GPU: ring buffer at physical 0x%08X, %u bytes", physicalAddress, state.ringSizeDwords * 4);
    }

    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockLog2)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        state.writeBackPhysical = physicalAddress;
        LOG_INFO("GPU: read pointer write-back to physical 0x%08X (block 2^%u)", physicalAddress, blockLog2);
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        state.interruptCallback = callback;
        state.interruptUserData = userData;
        LOG_INFO("GPU: interrupt callback 0x%08X (data 0x%08X)", callback, userData);
    }

    void Start()
    {
        State& state = GetState();
        {
            std::lock_guard lock(state.lock);
            if (state.started)
                return;
            state.started = true;
            state.interrupts = std::make_unique<kernel::GuestCallQueue>("GPU interrupts", [] {
                g_interruptsHandled.Notify();
                g_progress.Notify();
            });
        }
        std::thread(CommandProcessorThread).detach();
        std::thread(VblankThread).detach();
    }
}
