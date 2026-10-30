#include "gpu/d3d12/d3d12_renderer.h"

#include "config.h"
#include "gpu/d3d12/device.h"
#include "gpu/d3d12/overlay.h"
#include "gpu/d3d12/pipelines.h"
#include "gpu/d3d12/presenter.h"
#include "gpu/d3d12/render_targets.h"
#include "gpu/d3d12/resources.h"
#include "gpu/d3d12/scaled_resolves.h"
#include "gpu/d3d12/shader_compiler.h"
#include "gpu/d3d12/shared_memory.h"
#include "gpu/d3d12/texture_cache.h"
#include "gpu/command_processor.h"
#include "gpu/memory_watch.h"
#include "gpu/state_audit.h"
#include "gpu/registers.h"
#include "gpu/shader/hlsl.h"
#include "gpu/shader/shader.h"
#include "gpu/xenos.h"
#include "guest_memory.h"
#include "log.h"
#include "settings.h"
#include "ui/window.h"
#include "util/memory_search.h"
#include "util/png.h"
#include "util/profiler.h"
#include "util/write_watch.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <immintrin.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace gpu::d3d12
{
    namespace
    {
        // RequestFrameDump: dump the next presented frame.
        std::atomic<bool> g_frameDumpRequested{ false };

        uint32_t SwapIndex(uint32_t value, uint32_t endian, bool index32)
        {
            switch (endian)
            {
            case 1: return index32 ? (((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF)) : uint32_t(__builtin_bswap16(uint16_t(value)));
            case 2: return index32 ? __builtin_bswap32(value) : value;
            case 3: return index32 ? (value << 16) | (value >> 16) : value;
            default: return value;
            }
        }

        // Copies registers in host byte order (the register file is guest memory, big-endian).
        void CopyRegisters(uint32_t* destination, uint32_t first, uint32_t count)
        {
            const uint8_t* source = guest::ToHost(kRegisterWindow + first * 4);
            const __m128i swap = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
            uint32_t i = 0;
            for (; i + 4 <= count; i += 4)
            {
                const __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + i * 4));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), _mm_shuffle_epi8(value, swap));
            }
            for (; i < count; i++)
                destination[i] = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(source + i * 4));
        }

        // The name suffix of a pixel shader variant: "_coverage" for alpha to coverage, then "_c" and the centroid
        // interpolators' mask in 4 hex digits.
        std::string VariantSuffix(bool alphaCoverage, uint32_t centroidInterpolators)
        {
            char suffix[24];
            snprintf(suffix, sizeof(suffix), "%s%s", alphaCoverage ? "_coverage" : "", centroidInterpolators ? "_c" : "");
            std::string text = suffix;
            if (centroidInterpolators)
            {
                snprintf(suffix, sizeof(suffix), "%04X", centroidInterpolators & 0xFFFF);
                text += suffix;
            }
            return text;
        }

        uint32_t BlendControlRegister(uint32_t index)
        {
            static const uint32_t kRegisters[4] = { reg::RB_BLENDCONTROL0, reg::RB_BLENDCONTROL1, reg::RB_BLENDCONTROL2, reg::RB_BLENDCONTROL3 };
            return kRegisters[index & 3];
        }

        // The source blend factors the pixel shader applies for the min and max blend ops (see
        // shader::SystemConstants::minMaxFactors). It can't apply ones that take the destination or the blend
        // constant, or a destination factor other than one: those are reported once.
        uint32_t MinMaxFactors(uint32_t blendControl)
        {
            uint32_t factors = 0;
            for (const uint32_t shift : { 0u, 16u })
            {
                const uint32_t source = (blendControl >> shift) & 0x1F, op = (blendControl >> (shift + 5)) & 7, dest = (blendControl >> (shift + 8)) & 0x1F;
                if (op != 2 && op != 3)
                    continue;
                factors |= (source | 0x20) << (shift / 2);
                const bool sourceOnly = source <= 1 || (source >= 4 && source <= 7);
                if (!sourceOnly || dest != 1)
                {
                    static std::atomic<bool> reported{ false };
                    if (!reported.exchange(true))
                        LOG_WARN("GPU: min/max blend with factors %u and %u (blend control %08X) isn't emulated exactly", source, dest, blendControl);
                }
            }
            return factors;
        }

        uint32_t ColorInfoRegister(uint32_t index)
        {
            static const uint32_t kRegisters[4] = { reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO, reg::RB_COLOR3_INFO };
            return kRegisters[index & 3];
        }

        class D3D12Renderer final : public gpu::Renderer
        {
        public:
            bool Initialize(ui::Window& window);

            void LoadShader(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount) override;
            void Draw(const DrawCommand& draw) override;
            void Swap(const SwapCommand& swap) override;
            void PrecompileShaders(const uint8_t* image, size_t size) override;

        private:
            // Records the draw; returns what happened to it ("drawn", "resolve" or why it was skipped).
            const char* DrawInternal(const DrawCommand& draw);
            struct Shader
            {
                shader::ParsedShader parsed;
                std::shared_ptr<ShaderCompiler::Job> job; // null if translation failed; shared by shaders that translate the same
                // Pixel shaders: variants by translation options (alpha to coverage, centroid interpolators; see
                // shader::TranslationOptions), compiled on first use. Keyed by VariantKey.
                std::unordered_map<uint32_t, std::shared_ptr<ShaderCompiler::Job>> variants;
                bool Ready() const { return !job || job->done; }
                ID3DBlob* Bytecode() const { return job && job->done ? job->bytecode.Get() : nullptr; }
                // What the translation couldn't express (shader::HlslResult::unsupported), reported when first drawn.
                std::vector<std::string> unsupported;
                bool unsupportedReported = false;
            };

            void BeginFrame();
            // Applies changed graphics settings (settings::Graphics), between frames.
            void ApplyGraphics();
            void Submit();
            void UpdateTitle();
            D3D12_GPU_VIRTUAL_ADDRESS UploadConstants(const void* data, uint32_t size);
            // A constant buffer of `count` registers from `first`, of which only the first `copied` are filled
            // (shaders read no further).
            D3D12_GPU_VIRTUAL_ADDRESS UploadRegisters(uint32_t first, uint32_t count, uint32_t copied = UINT32_MAX);
            D3D12_GPU_VIRTUAL_ADDRESS BindTextures(const Shader& shader);
            void ReportOnce(uint32_t id, const char* format, uint32_t value);

            ui::Window* m_window = nullptr;
            Device m_device;
            Presenter m_presenter;
            Overlay m_overlay; // the runtime's notices over the frame (ui/notices.h)
            ComPtr<ID3D12CommandAllocator> m_allocators[kFramesInFlight];
            uint64_t m_frameFences[kFramesInFlight] = {};
            ComPtr<ID3D12GraphicsCommandList> m_list;
            // Guest memory uploads, executed before m_list (see shared_memory.h).
            ComPtr<ID3D12CommandAllocator> m_uploadAllocators[kFramesInFlight];
            ComPtr<ID3D12GraphicsCommandList> m_uploadList;
            uint32_t m_frame = 0;
            uint64_t m_frameNumber = 0;
            UploadAllocator m_upload;
            // PRECOMPILE_SHADERS=0 skips precompilation. Once it has started, shaders it didn't cover are
            // reported when they're compiled, and kept in m_seenShaders (microcode, host-endian dwords) for the
            // next run's: D3D's patched copies of the vertex shaders aren't in the executable.
            bool m_precompile = true;
            std::atomic<bool> m_precompiling{ false };
            std::string m_seenShaders;
            // Pipelines draws needed, kept in <data>/cache/pipelines.txt (shader names and the rest of the key) and
            // prepared in the background at startup, so that later runs needn't wait for the driver to compile them.
            std::string m_pipelineList;
            void SavePipelines();
            void PreparePipelines(const std::unordered_map<std::string, std::shared_ptr<ShaderCompiler::Job>>& shaders,
                const std::unordered_map<std::string, shader::ParsedShader>& pixelShaders);
            DescriptorHeap m_views;
            DescriptorHeap m_samplers;
            SharedMemory m_sharedMemory;
            ScaledResolves m_scaledResolves; // resolution scaling (used by both caches)
            TextureCache m_textures;
            RenderTargetCache m_renderTargets;
            ShaderCompiler m_compiler; // (outlives m_pipelines, whose workers use its bytecode)
            PipelineCache m_pipelines;
            // PROFILE: GPU time per frame from timestamps at the start of its upload list and the end of its list.
            ComPtr<ID3D12QueryHeap> m_timestamps;
            ComPtr<ID3D12Resource> m_timestampReadback;
            uint64_t m_timestampFrequency = 0;
            bool m_timestampPending[kFramesInFlight] = {};
            // DEBUG_NAN: the shaders' counts (see hlsl.h), copied back every 300 frames, and the counts last logged.
            ComPtr<ID3D12Resource> m_debugCounts;
            ComPtr<ID3D12Resource> m_debugCountsReadback;
            bool m_debugCountsPending[kFramesInFlight] = {};
            std::vector<uint32_t> m_debugCountsLogged;
            void LogDebugCounts();
            bool m_syncShaders = false;
            bool m_alphaCoverage = false; // settings::Graphics::alphaToCoverage
            uint32_t m_graphicsVersion = 0;
            uint64_t m_skippedDraws = 0;

            // DUMP_FRAMES=first[,count[,interval]][;first...]: writes `count` presented frames, `interval` apart,
            // to <data>/dumps/frame_<presented frame>.png, for each range. A range whose first is written F<first>
            // counts the frames of a fight instead (netplay::FightFrame, from its first frame past its loading, which two
            // runs count alike), into fight_<fight frame>.png: two runs' dumps of a fight then show the same frames.
            struct DumpRange
            {
                uint64_t first = 0, count = 1, interval = 1;
                bool fight = false;
            };
            std::vector<DumpRange> m_dumpRanges;
            bool DumpThisFrame(bool fight, uint64_t frame) const
            {
                for (const DumpRange& range : m_dumpRanges)
                {
                    if (range.fight == fight && frame >= range.first && (frame - range.first) % range.interval == 0 &&
                        (frame - range.first) / range.interval < range.count)
                        return true;
                }
                return false;
            }
            bool m_dumpShaders = false;

            std::chrono::steady_clock::time_point m_titleTime;
            uint64_t m_titleFrame = 0;
            uint64_t m_frameDraws = 0;

            // Shaders by stage and microcode hash; the active ones are the last loaded of each stage.
            std::unordered_map<uint64_t, std::unique_ptr<Shader>> m_shaders;
            Shader* m_vertexShader = nullptr;
            Shader* m_pixelShader = nullptr;

            std::bitset<256> m_reported;
            // DRAW_LOG=first[,count]: logs every draw (and why it was skipped) for those frames.
            uint64_t m_drawLogFirst = UINT64_MAX, m_drawLogCount = 0;
            uint64_t m_transferLogFirst = UINT64_MAX, m_transferLogCount = 0; // LOG_TRANSFERS=first,count
            // DRAW_LOG_CONSTANTS=first,count[;first,count...]: also logs those float constants (0-255
            // vertex, 256-511 pixel).
            std::vector<std::pair<uint32_t, uint32_t>> m_drawLogConstants;
            std::unordered_set<std::string> m_dumpedBuffers; // DRAW_LOG_BUFFERS: files written
            uint32_t m_drawIndex = 0;
            bool DrawLogging() const { return m_frameNumber >= m_drawLogFirst && m_frameNumber < m_drawLogFirst + m_drawLogCount; }
            // CAPTURE=frame[+count]:draw,draw,...: PNGs of the bound color target after those draws.
            // A draw of the form pN means every draw drawn to a render target with pitch N, vADDRESS (hex)
            // every draw whose first vertex buffer is at that physical address, and sHASH (hex, a prefix of the
            // 16 digits) every draw with that vertex shader (draw numbers vary between runs).
            uint64_t m_captureFrame = UINT64_MAX, m_captureFrameCount = 1;
            std::vector<uint32_t> m_captureDraws;
            uint32_t m_capturePitch = 0;
            std::vector<uint32_t> m_captureVertexBuffers;
            std::vector<std::string> m_captureVertexShaders;
            // Whether the active vertex shader's hash starts with one of these (upper case hex) prefixes.
            bool VertexShaderMatches(const std::vector<std::string>& prefixes) const
            {
                if (!m_vertexShader)
                    return false;
                char hash[17];
                snprintf(hash, sizeof(hash), "%016llX", (unsigned long long)m_vertexShader->parsed.hash);
                return std::any_of(prefixes.begin(), prefixes.end(),
                    [&](const std::string& prefix) { return strncmp(hash, prefix.c_str(), prefix.size()) == 0; });
            }
            bool CaptureVertexShader() const { return VertexShaderMatches(m_captureVertexShaders); }
            // DRAW_LOG_FILTER=vADDRESS,sHASH,...: logs only the draws whose first vertex buffer is at one of those physical
            // addresses (hex) or whose vertex shader's hash starts with one of those prefixes.
            std::vector<uint32_t> m_drawLogVertexBuffers;
            std::vector<std::string> m_drawLogVertexShaders;
            bool DrawLogFilter() const
            {
                if (m_drawLogVertexBuffers.empty() && m_drawLogVertexShaders.empty())
                    return true;
                return std::find(m_drawLogVertexBuffers.begin(), m_drawLogVertexBuffers.end(), FirstVertexBuffer()) != m_drawLogVertexBuffers.end() ||
                    VertexShaderMatches(m_drawLogVertexShaders);
            }
            // Physical address of the active vertex shader's first vertex buffer (0 if none).
            uint32_t FirstVertexBuffer() const
            {
                if (!m_vertexShader || m_vertexShader->parsed.vertexFetchConstants.empty())
                    return 0;
                return Register(reg::SHADER_CONSTANT_FETCH_00_0 + m_vertexShader->parsed.vertexFetchConstants[0] * 2) & 0xFFFFFFFC;
            }
            std::vector<std::pair<uint32_t, RenderTargetCache::Capture>> m_captures;
            void WriteCaptures();
            void LogDraw(const DrawCommand& draw, const char* outcome);
            double m_pipelineMs = 0.0;
            // A frame waits up to this long in all for new pipelines; draws are skipped after that until they're ready.
            // PIPELINE_WAIT_MS sets it (default 8).
            double m_pipelineWaitMs = 8.0;
            double m_framePipelineMs = 0.0; // waited this frame
            bool m_halfPixelOffset = true;
            bool m_debugNoDepth = false; // DEBUG_NO_DEPTH=1: no depth test or depth clipping (diagnostics)
            bool m_debugReupload = false; // DEBUG_REUPLOAD=1: re-uploads guest memory every frame (diagnostics)
            // CHECK_LATE_WRITES=1: hashes each (small) vertex buffer and loaded texture when processed and
            // again when the frame is presented, reporting data the CPU changed in between and uploads that
            // differed from guest memory (with VERIFY_UPLOADS) (diagnostics).
            struct LateWriteCheck
            {
                uint32_t draw, physical, size, hash;
                uint32_t uploadedHash; // of the GPU's copy (when verifying uploads), else hash
                bool texture;
            };
            void RecordLateWriteCheck(uint32_t physical, uint32_t size, bool texture);
            bool m_checkLateWrites = false;
            // FRAME_SUMMARY=first,count: one line per frame with draw outcomes and predicated packets,
            // cheap enough not to change the command processor's timing much (diagnostics).
            uint64_t m_summaryFirst = UINT64_MAX, m_summaryCount = 0;
            std::unordered_map<std::string, uint32_t> m_summaryOutcomes;
            uint64_t m_summarySkipsAtFrameStart = 0;
            std::vector<LateWriteCheck> m_lateWriteChecks;
            static uint32_t HashGuest(uint32_t physical, uint32_t size);
            void CheckLateWrites();
        };

        void D3D12Renderer::ReportOnce(uint32_t id, const char* format, uint32_t value)
        {
            if (m_reported[id & 255])
                return;
            m_reported[id & 255] = true;
            char message[256];
            snprintf(message, sizeof(message), format, value);
            LOG_WARN("%s", message);
        }

        bool D3D12Renderer::Initialize(ui::Window& window)
        {
            m_window = &window;
            const char* debug = std::getenv(GAME_PREFIX "_D3D12_DEBUG");
            if (!m_device.Initialize(debug && std::atoi(debug) != 0))
                return false;
            if (!m_presenter.Initialize(m_device, window))
                return false;

            ID3D12Device* device = m_device.Get();
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
            LOG_INFO("D3D12: resource binding tier %d", int(options.ResourceBindingTier));
            if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
            {
                LOG_ERROR("D3D12: bindless textures need resource binding tier 2");
                return false;
            }

            for (uint32_t i = 0; i < kFramesInFlight; i++)
            {
                if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocators[i]))) ||
                    FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_uploadAllocators[i]))))
                    return false;
            }
            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_list))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_uploadAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_uploadList))))
                return false;
            // Lists are created recording; BeginFrame resets them.
            m_list->Close();
            m_uploadList->Close();
            m_upload.Initialize(device, 16ull << 20);
            if (!m_views.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, 1024) ||
                !m_samplers.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1792, 64))
                return false;
            if (!m_overlay.Initialize(m_device, m_views))
                return false;
            m_presenter.SetOverlay(&m_overlay);
            // Graphics settings: the resolution scale (renders at that many times the guest's resolution in each direction),
            // an MSAA override for surfaces DOA4 renders with 2x MSAA, alpha to coverage and anisotropic filtering.
            m_graphicsVersion = settings::GraphicsVersion();
            const settings::Graphics graphics = settings::GetGraphics();
            m_scaledResolves.Initialize(device, graphics.resolutionScale);
            m_renderTargets.scale = graphics.resolutionScale;
            m_renderTargets.msaaSamples = graphics.msaaSamples;
            m_alphaCoverage = graphics.alphaToCoverage;
            m_textures.anisotropy = std::max(graphics.anisotropy, 1u);
            if (!m_sharedMemory.Initialize(device) || !m_textures.Initialize(device, m_views, m_samplers, m_sharedMemory, m_scaledResolves) ||
                !m_renderTargets.Initialize(device, m_views, m_sharedMemory, m_scaledResolves) || !m_pipelines.Initialize(device))
                return false;

            if (const char* dump = std::getenv(GAME_PREFIX "_DUMP_FRAMES"))
            {
                for (const char* p = dump; *p;)
                {
                    char* end = nullptr;
                    DumpRange range;
                    range.fight = *p == 'F';
                    range.first = strtoull(p + (range.fight ? 1 : 0), &end, 10);
                    if (end && *end == ',')
                        range.count = strtoull(end + 1, &end, 10);
                    if (end && *end == ',')
                        range.interval = std::max<uint64_t>(strtoull(end + 1, &end, 10), 1);
                    m_dumpRanges.push_back(range);
                    if (!end || *end != ';')
                        break;
                    p = end + 1;
                }
                std::error_code error;
                std::filesystem::create_directories(config::DataDirectory() + "/dumps", error);
            }
            if (const char* dumpShaders = std::getenv(GAME_PREFIX "_DUMP_SHADERS"); dumpShaders && std::atoi(dumpShaders) != 0)
            {
                m_dumpShaders = true;
                std::error_code error;
                std::filesystem::create_directories(config::DataDirectory() + "/dumps/shaders", error);
            }
            if (const char* halfPixel = std::getenv(GAME_PREFIX "_HALF_PIXEL"))
                m_halfPixelOffset = std::atoi(halfPixel) != 0;
            if (const char* transferLog = std::getenv(GAME_PREFIX "_LOG_TRANSFERS"))
            {
                char* end = nullptr;
                m_transferLogFirst = strtoull(transferLog, &end, 10);
                m_transferLogCount = (end && *end == ',') ? strtoull(end + 1, nullptr, 10) : 1;
            }
            if (const char* drawLog = std::getenv(GAME_PREFIX "_DRAW_LOG"))
            {
                char* end = nullptr;
                m_drawLogFirst = strtoull(drawLog, &end, 10);
                m_drawLogCount = (end && *end == ',') ? strtoull(end + 1, nullptr, 10) : 1;
            }
            if (const char* filter = std::getenv(GAME_PREFIX "_DRAW_LOG_FILTER"))
            {
                for (const char* p = filter; *p;)
                {
                    char* end = const_cast<char*>(p + 1);
                    if (*p == 'v')
                        m_drawLogVertexBuffers.push_back(uint32_t(strtoul(p + 1, &end, 16)));
                    else if (*p == 's')
                    {
                        while (isxdigit(uint8_t(*end)))
                            end++;
                        m_drawLogVertexShaders.emplace_back(p + 1, size_t(end - (p + 1)));
                        for (char& c : m_drawLogVertexShaders.back())
                            c = char(toupper(uint8_t(c)));
                    }
                    while (*end && *end != ',')
                        end++;
                    p = *end ? end + 1 : end;
                }
            }
            if (const char* constants = std::getenv(GAME_PREFIX "_DRAW_LOG_CONSTANTS"))
            {
                for (const char* p = constants; *p;)
                {
                    char* end = nullptr;
                    const uint32_t first = uint32_t(strtoul(p, &end, 10)) & 511;
                    uint32_t count = 1;
                    if (*end == ',')
                        count = uint32_t(strtoul(end + 1, &end, 10));
                    m_drawLogConstants.emplace_back(first, std::min(count, 512 - first));
                    p = *end ? end + 1 : end;
                }
            }
            if (const char* capture = std::getenv(GAME_PREFIX "_CAPTURE"))
            {
                char* end = nullptr;
                m_captureFrame = strtoull(capture, &end, 10);
                if (end && *end == '+')
                    m_captureFrameCount = strtoull(end + 1, &end, 10);
                while (end && (*end == ':' || *end == ','))
                {
                    if (end[1] == 'p')
                        m_capturePitch = uint32_t(strtoul(end + 2, &end, 10));
                    else if (end[1] == 'v')
                        m_captureVertexBuffers.push_back(uint32_t(strtoul(end + 2, &end, 16)));
                    else if (end[1] == 's')
                    {
                        const char* start = end + 2;
                        end = const_cast<char*>(start);
                        while (isxdigit(uint8_t(*end)))
                            end++;
                        m_captureVertexShaders.emplace_back(start, size_t(end - start));
                        for (char& c : m_captureVertexShaders.back())
                            c = char(toupper(uint8_t(c)));
                    }
                    else
                        m_captureDraws.push_back(uint32_t(strtoul(end + 1, &end, 10)));
                }
            }
            if (const char* noDepth = std::getenv(GAME_PREFIX "_DEBUG_NO_DEPTH"))
                m_debugNoDepth = std::atoi(noDepth) != 0;
            if (const char* reupload = std::getenv(GAME_PREFIX "_DEBUG_REUPLOAD"))
                m_debugReupload = std::atoi(reupload) != 0;
            if (const char* summary = std::getenv(GAME_PREFIX "_FRAME_SUMMARY"))
            {
                char* end = nullptr;
                m_summaryFirst = strtoull(summary, &end, 10);
                m_summaryCount = (end && *end == ',') ? strtoull(end + 1, nullptr, 10) : 1;
            }
            // Uploads and texture loads are recorded outside render passes (see render_targets.h).
            m_sharedMemory.beforeWork = [this] { m_renderTargets.EndPass(m_list.Get()); };
            m_textures.beforeLoad = [this] { m_renderTargets.EndPass(m_list.Get()); };
            if (const char* lateWrites = std::getenv(GAME_PREFIX "_CHECK_LATE_WRITES"))
                m_checkLateWrites = std::atoi(lateWrites) != 0;
            if (m_checkLateWrites)
            {
                m_textures.afterLoadRequest = [this](uint32_t base, uint32_t size) {
                    if (size <= 0x400000)
                        RecordLateWriteCheck(base, size, true);
                };
            }
            if (const char* syncShaders = std::getenv(GAME_PREFIX "_SYNC_SHADERS"))
                m_syncShaders = std::atoi(syncShaders) != 0;
            if (const char* precompile = std::getenv(GAME_PREFIX "_PRECOMPILE_SHADERS"))
                m_precompile = std::atoi(precompile) != 0;
            if (profile::Enabled())
            {
                D3D12_QUERY_HEAP_DESC queries{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kFramesInFlight, 0 };
                if (FAILED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&m_timestamps))) ||
                    FAILED(m_device.Queue()->GetTimestampFrequency(&m_timestampFrequency)) ||
                    !(m_timestampReadback = CreateBuffer(device, 16 * kFramesInFlight, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST)))
                    m_timestamps.Reset();
            }
            if (shader::DebugNan())
            {
                m_debugCounts = CreateBuffer(device, shader::kDebugCountBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                m_debugCountsReadback =
                    CreateBuffer(device, uint64_t(shader::kDebugCountBytes) * kFramesInFlight, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                if (!m_debugCounts || !m_debugCountsReadback)
                    return false;
                m_debugCountsLogged.assign(shader::kDebugCountBytes / 4, 0);
                LOG_INFO("DEBUG_NAN: counting, per shader, invocations with a product of 0 and infinity or NaN, with non-finite outputs, and "
                         "with non-finite inputs%s",
                    shader::XenosProducts() ? " (Xenos products)" : " (IEEE products)");
            }
            m_compiler.Start(config::DataDirectory() + "/cache/shaders");
            m_seenShaders = config::DataDirectory() + "/cache/shaders/seen";
            m_pipelineList = config::DataDirectory() + "/cache/pipelines.txt";
            if (const char* wait = std::getenv(GAME_PREFIX "_PIPELINE_WAIT_MS"))
                m_pipelineWaitMs = std::max(0.0, std::atof(wait));
            if (m_syncShaders)
                m_pipelineWaitMs = 1e9;

            m_titleTime = std::chrono::steady_clock::now();
            BeginFrame();
            return true;
        }

        void D3D12Renderer::LoadShader(ShaderStage stage, const uint32_t* guestUcode, uint32_t dwordCount)
        {
            // D3D reloads its shaders for nearly every draw (about 2,000 loads and 4 MB of microcode a frame in
            // fights), so the lookup hashes the microcode as loaded, and only new shaders are byte-swapped.
            const uint64_t key = XXH3_64bits(guestUcode, size_t(dwordCount) * 4) ^ uint64_t(stage);
            auto& entry = m_shaders[key];
            if (!entry)
            {
                std::vector<uint32_t> ucode(guestUcode, guestUcode + dwordCount);
                for (uint32_t& dword : ucode)
                    dword = __builtin_bswap32(dword);
                entry = std::make_unique<Shader>();
                entry->parsed = shader::Parse(stage, ucode.data(), dwordCount);
                const shader::ParsedShader& parsed = entry->parsed;
                const char* stageName = stage == ShaderStage::kVertex ? "vs" : "ps";
                char name[64];
                snprintf(name, sizeof(name), "%s_%016llX", stageName, (unsigned long long)parsed.hash);
                // Microcode that's mostly zeros draws nothing: it was read from memory D3D never wrote.
                if (const auto zeros = uint32_t(std::count(ucode.begin(), ucode.end(), 0u)); dwordCount >= 16 && zeros * 4 > dwordCount * 3)
                    LOG_WARN("GPU: shader %s is mostly zeros (%u of %u dwords)", name, zeros, dwordCount);
                std::string hlsl;
                if (!parsed.valid)
                {
                    LOG_WARN("GPU: can't parse shader %s: %s", name, parsed.error.c_str());
                }
                else
                {
                    shader::HlslResult translation = shader::TranslateToHlsl(parsed);
                    hlsl = std::move(translation.source);
                    entry->unsupported = std::move(translation.unsupported);
                    entry->job = m_compiler.Compile(hlsl, stage == ShaderStage::kVertex ? "vs_5_1" : "ps_5_1", name);
                    if (m_precompiling && !entry->job->precompiled && entry->job->name == name)
                    {
                        LOG_INFO("GPU: shader %s wasn't precompiled", name);
                        const std::string path = m_seenShaders + "/" + name + ".bin";
                        std::error_code error;
                        if (!std::filesystem::exists(path, error))
                        {
                            std::filesystem::create_directories(m_seenShaders, error);
                            if (FILE* file = fopen(path.c_str(), "wb"))
                            {
                                fwrite(ucode.data(), 4, dwordCount, file);
                                fclose(file);
                            }
                        }
                    }
                    if (m_syncShaders)
                        m_compiler.Wait(*entry->job);
                    if (parsed.memoryExport)
                        LOG_WARN("GPU: shader %s uses memory export (not supported)", name);
                }
                if (m_dumpShaders)
                {
                    const std::string base = config::DataDirectory() + "/dumps/shaders/" + name;
                    if (FILE* file = fopen((base + ".txt").c_str(), "wb"))
                    {
                        const std::string text = shader::Disassemble(parsed);
                        fwrite(text.data(), 1, text.size(), file);
                        fclose(file);
                    }
                    if (FILE* file = fopen((base + ".hlsl").c_str(), "wb"))
                    {
                        fwrite(hlsl.data(), 1, hlsl.size(), file);
                        fclose(file);
                    }
                    if (FILE* file = fopen((base + ".bin").c_str(), "wb"))
                    {
                        fwrite(ucode.data(), 4, dwordCount, file); // the microcode (host-endian dwords)
                        fclose(file);
                    }
                }
            }
            (stage == ShaderStage::kVertex ? m_vertexShader : m_pixelShader) = entry.get();
        }

        // Translates and compiles shaders in the background, so draws rarely wait for FXC: first the ones earlier runs
        // compiled on demand (m_seenShaders), then the executable's (see shader::FindShaderBinaries).
        void D3D12Renderer::PrecompileShaders(const uint8_t* image, size_t size)
        {
            if (!m_precompile)
                return;
            m_precompiling = true;
            std::thread([this, image, size] {
                profile::NameThread("Shader precompiler");
                const auto start = std::chrono::steady_clock::now();
                const uint64_t cacheHits = m_compiler.cacheHits, compiled = m_compiler.compiled;
                std::vector<std::shared_ptr<ShaderCompiler::Job>> jobs;
                std::vector<std::string> names;
                std::unordered_map<std::string, shader::ParsedShader> pixelShaders;
                const auto precompile = [&](ShaderStage stage, const std::vector<uint32_t>& ucode) {
                    shader::ParsedShader parsed = shader::Parse(stage, ucode.data(), uint32_t(ucode.size()));
                    if (!parsed.valid)
                        return;
                    char name[64];
                    snprintf(name, sizeof(name), "%s_%016llX", stage == ShaderStage::kVertex ? "vs" : "ps", (unsigned long long)parsed.hash);
                    jobs.push_back(m_compiler.Compile(shader::TranslateToHlsl(parsed).source, stage == ShaderStage::kVertex ? "vs_5_1" : "ps_5_1", name, true));
                    names.push_back(name);
                    if (stage == ShaderStage::kPixel)
                        pixelShaders.emplace(name, std::move(parsed));
                };
                std::error_code error;
                for (const auto& file : std::filesystem::directory_iterator(m_seenShaders, error))
                {
                    const std::string stem = file.path().stem().string();
                    const uintmax_t bytes = file.file_size(error);
                    if (file.path().extension() != ".bin" || (stem.rfind("vs_", 0) != 0 && stem.rfind("ps_", 0) != 0) || error || !bytes || bytes % 4)
                        continue;
                    std::vector<uint32_t> ucode(size_t(bytes / 4));
                    if (FILE* stream = fopen(file.path().string().c_str(), "rb"))
                    {
                        const bool read = fread(ucode.data(), 4, ucode.size(), stream) == ucode.size();
                        fclose(stream);
                        if (read)
                            precompile(stem[0] == 'v' ? ShaderStage::kVertex : ShaderStage::kPixel, ucode);
                    }
                }
                const size_t seen = jobs.size();
                for (const shader::ShaderBinary& binary : shader::FindShaderBinaries(image, size))
                    precompile(binary.stage, binary.ucode);
                for (const auto& job : jobs)
                    m_compiler.Wait(*job);
                LOG_INFO("GPU: precompiled %zu shaders (%zu seen in earlier runs) in %.1f s (%llu compiled, %llu from the cache)", jobs.size(), seen,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    (unsigned long long)(m_compiler.compiled - compiled), (unsigned long long)(m_compiler.cacheHits - cacheHits));
                // Shaders by name: their own, and the name of the job compiling them (shared by shaders that translate the same).
                std::unordered_map<std::string, std::shared_ptr<ShaderCompiler::Job>> shaders;
                for (size_t i = 0; i < jobs.size(); i++)
                {
                    shaders.emplace(names[i], jobs[i]);
                    shaders.emplace(jobs[i]->name, jobs[i]);
                }
                PreparePipelines(shaders, pixelShaders);
            }).detach();
        }

        // pipelines.txt: "doa4 pipelines 1", then per pipeline its vertex and pixel shaders' names ("-" for none) and the rest
        // of its key in hex (from geometryShader on). Change the version with the key.
        static_assert(sizeof(PipelineKey) - offsetof(PipelineKey, geometryShader) == 56, "change pipelines.txt's version");

        void D3D12Renderer::PreparePipelines(const std::unordered_map<std::string, std::shared_ptr<ShaderCompiler::Job>>& shaders,
            const std::unordered_map<std::string, shader::ParsedShader>& pixelShaders)
        {
            FILE* file = fopen(m_pipelineList.c_str(), "rb");
            if (!file)
                return;
            const auto start = std::chrono::steady_clock::now();
            char line[512];
            const bool current = fgets(line, sizeof(line), file) && strncmp(line, "doa4 pipelines 1", 16) == 0;
            std::unordered_set<std::string> lines;
            while (current && fgets(line, sizeof(line), file))
                lines.insert(line);
            fclose(file);
            if (!current)
            {
                LOG_INFO("GPU: %s is from another version, starting over", m_pipelineList.c_str());
                remove(m_pipelineList.c_str());
                return;
            }

            // A shader by name: a precompiled one, or a variant of one (see VariantSuffix), compiled now.
            const auto shader = [&](const std::string& name) -> std::shared_ptr<ShaderCompiler::Job> {
                if (const auto it = shaders.find(name); it != shaders.end())
                    return it->second;
                shader::TranslationOptions options;
                std::string baseName = name;
                if (baseName.size() > 6 && baseName.compare(baseName.size() - 6, 2, "_c") == 0)
                {
                    options.centroidInterpolators = uint32_t(strtoul(baseName.c_str() + baseName.size() - 4, nullptr, 16));
                    baseName.resize(baseName.size() - 6);
                }
                if (baseName.size() > 9 && baseName.compare(baseName.size() - 9, 9, "_coverage") == 0)
                {
                    options.alphaCoverage = true;
                    baseName.resize(baseName.size() - 9);
                }
                const auto base = pixelShaders.find(baseName);
                if (base == pixelShaders.end() || (!options.alphaCoverage && !options.centroidInterpolators) ||
                    name != baseName + VariantSuffix(options.alphaCoverage, options.centroidInterpolators))
                    return nullptr;
                auto job = m_compiler.Compile(shader::TranslateToHlsl(base->second, options).source, "ps_5_1", name, true);
                m_compiler.Wait(*job);
                return job;
            };
            std::unordered_map<std::string, std::shared_ptr<ShaderCompiler::Job>> variants;
            size_t prepared = 0;
            for (const std::string& text : lines)
            {
                char vertexName[64], pixelName[64], state[2 * (sizeof(PipelineKey) - offsetof(PipelineKey, geometryShader)) + 1];
                if (sscanf(text.c_str(), "%63s %63s %112s", vertexName, pixelName, state) != 3 || strlen(state) + 1 != sizeof(state))
                    continue;
                const auto vertex = shader(vertexName);
                std::shared_ptr<ShaderCompiler::Job> pixel;
                if (strcmp(pixelName, "-") != 0)
                {
                    auto& variant = variants[pixelName];
                    if (!variant)
                        variant = shader(pixelName);
                    pixel = variant;
                    if (!pixel)
                        continue;
                }
                if (!vertex || !vertex->bytecode || (pixel && !pixel->bytecode))
                    continue;
                PipelineKey key{};
                key.vertexShader = vertex.get();
                key.pixelShader = pixel.get();
                uint8_t* bytes = reinterpret_cast<uint8_t*>(&key) + offsetof(PipelineKey, geometryShader);
                for (size_t i = 0; i + 1 < sizeof(state); i += 2)
                    bytes[i / 2] = uint8_t(strtoul(std::string(state + i, 2).c_str(), nullptr, 16));
                m_pipelines.Prepare(key, vertex->bytecode.Get(), pixel ? pixel->bytecode.Get() : nullptr);
                prepared++;
            }
            LOG_INFO("GPU: preparing %zu pipelines that earlier runs used (of %zu)", prepared, lines.size());
            while (m_pipelines.preparing.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            LOG_INFO("GPU: prepared them in %.1f s", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }

        void D3D12Renderer::SavePipelines()
        {
            const std::vector<PipelineKey> keys = m_pipelines.TakeNewKeys();
            if (keys.empty())
                return;
            std::error_code error;
            const bool exists = std::filesystem::exists(m_pipelineList, error);
            FILE* file = fopen(m_pipelineList.c_str(), "ab");
            if (!file)
                return;
            if (!exists)
                fputs("doa4 pipelines 1\n", file);
            for (const PipelineKey& key : keys)
            {
                const auto* vertex = static_cast<const ShaderCompiler::Job*>(key.vertexShader);
                const auto* pixel = static_cast<const ShaderCompiler::Job*>(key.pixelShader);
                fprintf(file, "%s %s ", vertex->name.c_str(), pixel ? pixel->name.c_str() : "-");
                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&key) + offsetof(PipelineKey, geometryShader);
                for (size_t i = 0; i < sizeof(PipelineKey) - offsetof(PipelineKey, geometryShader); i++)
                    fprintf(file, "%02X", bytes[i]);
                fputs("\n", file);
            }
            fclose(file);
        }

        D3D12_GPU_VIRTUAL_ADDRESS D3D12Renderer::UploadConstants(const void* data, uint32_t size)
        {
            const auto allocation = m_upload.Allocate((size + 255) & ~255u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            memcpy(allocation.cpu, data, size);
            return allocation.gpu;
        }

        D3D12_GPU_VIRTUAL_ADDRESS D3D12Renderer::UploadRegisters(uint32_t first, uint32_t count, uint32_t copied)
        {
            const auto allocation = m_upload.Allocate((count * 4 + 255) & ~255u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            CopyRegisters(reinterpret_cast<uint32_t*>(allocation.cpu), first, std::min(copied, count));
            return allocation.gpu;
        }

        D3D12_GPU_VIRTUAL_ADDRESS D3D12Renderer::BindTextures(const Shader& shader)
        {
            shader::BindingConstants bindings{};
            const shader::ParsedShader& parsed = shader.parsed;
            for (uint32_t i = 0; i < parsed.textures.size() && i < 32; i++)
            {
                const shader::TextureBinding& binding = parsed.textures[i];
                xenos::TextureFetch fetch;
                for (uint32_t d = 0; d < 6; d++)
                    fetch.d[d] = Register(reg::SHADER_CONSTANT_FETCH_00_0 + binding.fetchConstant * 6 + d);
                audit::TextureFetch(fetch);
                bindings.textures[i] = m_textures.GetSrv(m_list.Get(), m_upload, fetch, binding.dimension);
                // DEBUG_NULL_TEXTURE=address[,odd]: binds a null (black) view instead of that texture,
                // optionally only in odd frames, so consecutive dumps show where it's used (diagnostics).
                static const uint32_t nullTexture = [] {
                    const char* setting = std::getenv(GAME_PREFIX "_DEBUG_NULL_TEXTURE");
                    return setting ? uint32_t(strtoul(setting, nullptr, 16)) : 0u;
                }();
                static const bool nullOddFrames = [] {
                    const char* setting = std::getenv(GAME_PREFIX "_DEBUG_NULL_TEXTURE");
                    return setting && strstr(setting, "odd");
                }();
                if (nullTexture && fetch.BaseAddress() == nullTexture && (!nullOddFrames || (m_frameNumber & 1)))
                    bindings.textures[i] = m_textures.NullSrv(binding.dimension);
            }
            for (uint32_t i = 0; i < parsed.samplers.size() && i < 32; i++)
            {
                const shader::SamplerBinding& binding = parsed.samplers[i];
                xenos::TextureFetch fetch;
                for (uint32_t d = 0; d < 6; d++)
                    fetch.d[d] = Register(reg::SHADER_CONSTANT_FETCH_00_0 + binding.fetchConstant * 6 + d);
                bindings.samplers[i] = m_textures.GetSampler(fetch, binding);
            }
            return UploadConstants(&bindings, sizeof(bindings));
        }

        void D3D12Renderer::Draw(const DrawCommand& draw)
        {
            m_renderTargets.logTransfers = DrawLogging() || (m_frameNumber >= m_transferLogFirst && m_frameNumber < m_transferLogFirst + m_transferLogCount);
            m_renderTargets.frameNumber = m_frameNumber;
            const char* outcome = DrawInternal(draw);
            if (DrawLogging() && DrawLogFilter())
                LogDraw(draw, outcome);
            if (m_frameNumber >= m_summaryFirst && m_frameNumber < m_summaryFirst + m_summaryCount)
            {
                char key[64];
                snprintf(key, sizeof(key), "%s/p%u", outcome, Register(reg::RB_SURFACE_INFO) & 0x3FFF);
                m_summaryOutcomes[key]++;
            }
            const bool captureDraw = std::find(m_captureDraws.begin(), m_captureDraws.end(), m_drawIndex) != m_captureDraws.end() ||
                (m_capturePitch && strcmp(outcome, "drawn") == 0 && (Register(reg::RB_SURFACE_INFO) & 0x3FFF) == m_capturePitch) ||
                (!m_captureVertexBuffers.empty() && strcmp(outcome, "drawn") == 0 &&
                    std::find(m_captureVertexBuffers.begin(), m_captureVertexBuffers.end(), FirstVertexBuffer()) != m_captureVertexBuffers.end()) ||
                (!m_captureVertexShaders.empty() && strcmp(outcome, "drawn") == 0 && CaptureVertexShader());
            if (m_frameNumber >= m_captureFrame && m_frameNumber < m_captureFrame + m_captureFrameCount && captureDraw)
            {
                RenderTargetCache::Capture capture;
                if (m_renderTargets.CaptureBoundColor(m_list.Get(), 720, capture))
                    m_captures.emplace_back(m_drawIndex, std::move(capture));
                else
                    LOG_WARN("GPU: nothing to capture after draw %u", m_drawIndex);
            }
            m_drawIndex++;
        }

        void D3D12Renderer::WriteCaptures()
        {
            for (auto& [draw, capture] : m_captures)
            {
                void* data = nullptr;
                if (FAILED(capture.readback->Map(0, nullptr, &data)))
                    continue;
                char name[96];
                snprintf(name, sizeof(name), "/dumps/capture_%05llu_%04u_rt%03X_p%u.png", (unsigned long long)m_frameNumber, draw, capture.base,
                    capture.pitch);
                const std::string path = config::DataDirectory() + name;
                util::WritePng(path.c_str(), static_cast<const uint8_t*>(data), capture.width, capture.height, capture.footprint.Footprint.RowPitch);
                capture.readback->Unmap(0, nullptr);
                LOG_INFO("Captured draw %u (render target %03X, pitch %u, %ux MSAA) to %s", draw, capture.base, capture.pitch, capture.samples,
                    path.c_str());
            }
            m_captures.clear();
        }

        const char* D3D12Renderer::DrawInternal(const DrawCommand& draw)
        {
            ID3D12GraphicsCommandList* list = m_list.Get();
            const uint32_t edramMode = Register(reg::RB_MODECONTROL) & 7;
            if (edramMode == kEdramCopy)
            {
                profile::ScopedZone zone(profile::Zone::kDrawResolve);
                audit::Resolve();
                m_renderTargets.Resolve(list, m_upload);
                return "resolve";
            }
            if (edramMode != kEdramColorDepth && edramMode != kEdramDepth)
            {
                audit::Finding("draw in EDRAM mode " + std::to_string(edramMode) + " (skipped)");
                return "skipped: edram mode";
            }
            Shader* vs = m_vertexShader;
            Shader* ps = edramMode == kEdramColorDepth ? m_pixelShader : nullptr;
            if (!vs || !vs->job || (ps && !ps->job))
                return "skipped: no shader";
            // Shaders still compiling: skip the draw rather than stall the command processor.
            if (!vs->Ready() || (ps && !ps->Ready()))
            {
                m_skippedDraws++;
                return "skipped: compiling";
            }
            if (!vs->Bytecode() || (ps && !ps->Bytecode()))
                return "skipped: shader failed";
            // What the translator couldn't express, once per shader when it's drawn (shaders that are only precompiled have
            // some: D3D's vertex shader templates, whose fetches get their formats when bound).
            for (Shader* shader : { vs, ps })
            {
                if (!shader || shader->unsupported.empty() || shader->unsupportedReported)
                    continue;
                shader->unsupportedReported = true;
                std::string what;
                for (const std::string& item : shader->unsupported)
                    what += (what.empty() ? "" : ", ") + item;
                LOG_WARN("GPU: %s_%016llX drawn with what the translation couldn't express: %s", shader == vs ? "vs" : "ps",
                    (unsigned long long)shader->parsed.hash, what.c_str());
            }
            uint32_t count = draw.initiator.NumIndices();
            if (!count)
                return "skipped: no indices";

            // Primitive type: host topology, expansion and conversion.
            const uint32_t primitive = draw.initiator.PrimitiveType();
            D3D12_PRIMITIVE_TOPOLOGY topology;
            D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType;
            GeometryShader geometryShader = GeometryShader::kNone;
            enum class Conversion
            {
                kNone,
                kFan,
                kQuads,
            } conversion = Conversion::kNone;
            switch (primitive)
            {
            case kPointList:
                topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
                geometryShader = GeometryShader::kPointList;
                break;
            case kLineList:
                topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                break;
            case kLineStrip:
                topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                break;
            case kTriangleList:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                break;
            case kTriangleStrip:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                break;
            case kTriangleFan:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                conversion = Conversion::kFan;
                break;
            case kRectangleList:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                geometryShader = GeometryShader::kRectangleList;
                break;
            case kQuadList:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                conversion = Conversion::kQuads;
                break;
            default:
                ReportOnce(primitive, "GPU: primitive type %u not supported", primitive);
                audit::Finding("primitive type " + std::to_string(primitive) + " (skipped)");
                return "skipped: primitive type";
            }

            // Render targets: color targets the pixel shader writes with a nonzero write mask.
            const uint32_t rbColorMask = Register(reg::RB_COLOR_MASK);
            uint32_t colorTargets = 0;
            uint16_t writeMasks = 0;
            if (ps)
            {
                for (uint32_t i = 0; i < 4; i++)
                {
                    const uint32_t mask = (rbColorMask >> (i * 4)) & 0xF;
                    if ((ps->parsed.colorTargetsWritten & (1u << i)) && mask)
                    {
                        colorTargets |= 1u << i;
                        writeMasks |= uint16_t(mask << (i * 4));
                    }
                }
            }
            const uint32_t depthControl = Register(reg::RB_DEPTHCONTROL);
            const bool depthStencil = (depthControl & 3) != 0;
            if (!colorTargets && !depthStencil)
                return "skipped: no writes";
            RenderTargetCache::Bindings targets;
            if (!m_renderTargets.Lookup(colorTargets, depthStencil, targets))
                return "skipped: no targets";
            audit::Draw(primitive, colorTargets, targets.samples, targets.depth != nullptr);

            // Pixel shader variants for multisampled targets, compiled on first use (the plain shader draws until they're
            // ready): alpha to coverage for alpha-tested draws, and centroid sampling of the interpolators the game asks for:
            // SQ_INTERPOLATOR_CNTL has a bit for each interpolator in use (param_shade, smooth shading) and one for each
            // sampled at the pixel's center (sampling_pattern), which applies when SQ_CONTEXT_MISC's sc_sample_cntl is 2 (0:
            // all centroid, 1: all center). DOA4 marks the last one or two of its interpolators centroid.
            ShaderCompiler::Job* pixelJob = ps ? ps->job.get() : nullptr;
            const uint32_t colorControl = Register(reg::RB_COLORCONTROL);
            bool alphaCoverage = false;
            if (ps && targets.samples > 1)
            {
                const bool coverage = m_alphaCoverage && (colorControl & 8) && (colorControl & 7) != 7;
                const uint32_t interpolators = Register(reg::SQ_INTERPOLATOR_CNTL);
                const uint32_t sampleControl = (Register(reg::SQ_CONTEXT_MISC) >> 2) & 3;
                const uint32_t loaded = ps->parsed.tempCount >= 16 ? 0xFFFFu : (1u << std::max(ps->parsed.tempCount, 1u)) - 1;
                const uint32_t centroid = (sampleControl == 0 ? interpolators : sampleControl == 2 ? interpolators & ~(interpolators >> 16) : 0) & loaded;
                if (coverage || centroid)
                {
                    const uint32_t variantKey = (coverage ? 1u : 0u) | (centroid << 1);
                    auto& variant = ps->variants[variantKey];
                    if (!variant)
                    {
                        shader::TranslationOptions options;
                        options.alphaCoverage = coverage;
                        options.centroidInterpolators = centroid;
                        variant = m_compiler.Compile(shader::TranslateToHlsl(ps->parsed, options).source, "ps_5_1",
                            ps->job->name + VariantSuffix(coverage, centroid));
                        if (m_syncShaders)
                            m_compiler.Wait(*variant);
                    }
                    if (variant->done && variant->bytecode)
                    {
                        pixelJob = variant.get();
                        alphaCoverage = coverage;
                    }
                }
            }

            // Pipeline.
            PipelineKey key{};
            key.vertexShader = vs->job.get();
            key.pixelShader = pixelJob;
            key.geometryShader = geometryShader;
            key.topologyType = uint8_t(topologyType);
            key.samples = uint8_t(targets.samples);
            for (uint32_t i = 0; i < 4; i++)
            {
                if (!targets.color[i])
                    continue;
                key.renderTargetCount = uint8_t(i + 1);
                key.renderTargetFormats[i] = uint16_t(targets.color[i]->viewFormat);
                key.blend[i] = PipelineCache::PackBlend(Register(BlendControlRegister(i)));
            }
            key.writeMasks = writeMasks;
            if (targets.depth)
            {
                key.depthFormat = uint16_t(targets.depth->viewFormat);
                key.depthControl = m_debugNoDepth ? (depthControl & ~0x76u) | 0x70u : depthControl;
                const uint32_t refMask = Register(reg::RB_STENCILREFMASK);
                key.stencilMasks = ((refMask >> 8) & 0xFF) | (((refMask >> 16) & 0xFF) << 8);
            }
            const uint32_t modeControl = Register(reg::PA_SU_SC_MODE_CNTL);
            const bool polygonal = topologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            bool cullFront = false, cullBack = false;
            if (polygonal && primitive != kRectangleList)
            {
                cullFront = modeControl & 1;
                cullBack = (modeControl >> 1) & 1;
                if (cullFront && cullBack)
                    return "skipped: culled";
                key.cullMode = uint8_t(cullFront ? D3D12_CULL_MODE_FRONT : cullBack ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE);
                key.frontCounterClockwise = ((modeControl >> 2) & 1) == 0;
            }
            else
            {
                key.cullMode = uint8_t(D3D12_CULL_MODE_NONE);
            }
            // Polygon offset, as Xenia applies it: the front faces' unless they're culled or it's zero, else the back faces'
            // (a pipeline has one); points and lines take the front's with poly_offset_para_enable. The scale is in 1/16
            // subpixel units. The offset is in normalized depth, twice what's applied to 24-bit depth (as on later AMD GPUs),
            // and Direct3D 12 takes a whole number of the depth buffer's steps, 2^-24 for 24-bit depth (for 20e4 float depth,
            // Xenia found 2^19 per unit right). It's rounded away from zero: truncated, DOA4's 1e-5 vanished, and the layers
            // of moss and lichen on its mountains, drawn over the rock without an offset, z-fought with it.
            float polyScale = 0.0f, polyOffset = 0.0f;
            if (polygonal ? ((modeControl >> 11) & 1) && !cullFront : (modeControl >> 13) & 1)
            {
                polyScale = RegisterFloat(reg::PA_SU_POLY_OFFSET_FRONT_SCALE);
                polyOffset = RegisterFloat(reg::PA_SU_POLY_OFFSET_FRONT_OFFSET);
            }
            if (polygonal && ((modeControl >> 12) & 1) && !cullBack && polyScale == 0.0f && polyOffset == 0.0f)
            {
                polyScale = RegisterFloat(reg::PA_SU_POLY_OFFSET_BACK_SCALE);
                polyOffset = RegisterFloat(reg::PA_SU_POLY_OFFSET_BACK_OFFSET);
            }
            const float offsetSteps = polyOffset * float(1u << (((Register(reg::RB_DEPTH_INFO) >> 16) & 1) ? 19 : 23));
            key.depthBias = int32_t(std::ceil(std::abs(offsetSteps))) * (offsetSteps < 0.0f ? -1 : 1);
            key.slopeScaledDepthBias = polyScale / 16.0f;
            // PA_CL_CLIP_CNTL.clip_disable turns off clipping, including against the near and far planes.
            if (m_debugNoDepth || (Register(reg::PA_CL_CLIP_CNTL) & (1u << 16)))
                key.flags |= kPipelineFlagNoDepthClip;
            const bool indexed = draw.initiator.SourceSelect() == 0;
            const bool index32 = draw.initiator.Index32();
            if (indexed && conversion == Conversion::kNone && ((modeControl >> 21) & 1) &&
                (primitive == kTriangleStrip || primitive == kLineStrip))
            {
                const uint32_t reset = Register(reg::VGT_MULTI_PRIM_IB_RESET_INDX);
                if (reset == (index32 ? 0xFFFFFFFFu : 0xFFFFu))
                    key.stripCut = index32 ? 2 : 1;
            }
            // New pipelines are created on worker threads: wait for them within the frame's budget, then skip their draws
            // until they're ready.
            profile::ScopedZone pipelineZone(profile::Zone::kDrawPipeline);
            bool pipelineFailed = false;
            double waitedMs = 0.0;
            ID3D12PipelineState* pipeline = m_pipelines.Get(key, vs->Bytecode(), pixelJob ? pixelJob->bytecode.Get() : nullptr,
                std::max(0.0, m_pipelineWaitMs - m_framePipelineMs), pipelineFailed, waitedMs);
            pipelineZone.Stop();
            m_pipelineMs += waitedMs;
            m_framePipelineMs += waitedMs;
            if (!pipeline)
            {
                if (pipelineFailed)
                    return "skipped: pipeline failed";
                m_skippedDraws++;
                return "skipped: pipeline compiling";
            }

            // Guest memory the draw reads: vertex buffers, then the index buffer.
            profile::ScopedZone uploadZone(profile::Zone::kDrawUploads);
            for (uint32_t constant : vs->parsed.vertexFetchConstants)
            {
                const uint32_t d0 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + constant * 2);
                const uint32_t d1 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + constant * 2 + 1);
                if ((d0 & 3) == 3)
                {
                    const uint32_t size = ((d1 >> 2) & 0xFFFFFF) * 4;
                    m_sharedMemory.RequestRange(m_upload, d0 & 0xFFFFFFFC, size);
                    if (m_checkLateWrites && size && size <= 0x10000)
                        RecordLateWriteCheck(d0 & 0xFFFFFFFC, size, false);
                }
            }

            // Index buffer: the guest's, or converted fan/quad indices.
            D3D12_INDEX_BUFFER_VIEW indexBuffer{};
            bool drawIndexed = false;
            bool swapIndices = false;
            if (conversion != Conversion::kNone)
            {
                std::vector<uint32_t> source(count);
                if (indexed)
                {
                    const uint32_t address = guest::PhysicalToGuest(draw.indexBase);
                    count = std::min(count, draw.indexSize);
                    source.resize(count);
                    for (uint32_t i = 0; i < count; i++)
                    {
                        const uint32_t raw = index32 ? *reinterpret_cast<const uint32_t*>(guest::ToHost(address + i * 4))
                                                     : *reinterpret_cast<const uint16_t*>(guest::ToHost(address + i * 2));
                        source[i] = SwapIndex(raw, draw.indexEndian, index32);
                    }
                }
                else
                {
                    for (uint32_t i = 0; i < count; i++)
                        source[i] = i;
                }
                std::vector<uint32_t> converted;
                if (conversion == Conversion::kFan)
                {
                    for (uint32_t i = 1; i + 1 < count; i++)
                        converted.insert(converted.end(), { source[0], source[i], source[i + 1] });
                }
                else
                {
                    for (uint32_t i = 0; i + 3 < count; i += 4)
                        converted.insert(converted.end(), { source[i], source[i + 1], source[i + 2], source[i], source[i + 2], source[i + 3] });
                }
                if (converted.empty())
                    return "skipped: empty conversion";
                const uint32_t size = uint32_t(converted.size() * 4);
                const auto allocation = m_upload.Allocate(size, 4);
                memcpy(allocation.cpu, converted.data(), size);
                indexBuffer = { allocation.gpu, size, DXGI_FORMAT_R32_UINT };
                count = uint32_t(converted.size());
                drawIndexed = true;
            }
            else if (indexed)
            {
                const uint32_t size = std::min(count, draw.indexSize) * (index32 ? 4 : 2);
                m_sharedMemory.RequestRange(m_upload, draw.indexBase, size);
                indexBuffer = { m_sharedMemory.GpuAddress() + draw.indexBase, size, index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
                count = std::min(count, draw.indexSize);
                drawIndexed = true;
                swapIndices = true;
            }
            uploadZone.Stop();

            // Textures (may record untiling work, so before the graphics state is set).
            profile::ScopedZone textureZone(profile::Zone::kDrawTextures);
            const D3D12_GPU_VIRTUAL_ADDRESS vertexBindings = BindTextures(*vs);
            const D3D12_GPU_VIRTUAL_ADDRESS pixelBindings = ps ? BindTextures(*ps) : vertexBindings;
            textureZone.Stop();
            m_sharedMemory.Transition(list, SharedMemory::kReadState);

            // System constants: viewport transform, index handling, point size, alpha test.
            shader::SystemConstants system{};
            const uint32_t vte = Register(reg::PA_CL_VTE_CNTL);
            float scale[3] = { 1.0f, 1.0f, 1.0f }, offset[3] = { 0.0f, 0.0f, 0.0f };
            for (uint32_t axis = 0; axis < 3; axis++)
            {
                if (vte & (1u << (axis * 2)))
                    scale[axis] = RegisterFloat(reg::PA_CL_VPORT_XSCALE + axis * 2);
                if (vte & (2u << (axis * 2)))
                    offset[axis] = RegisterFloat(reg::PA_CL_VPORT_XOFFSET + axis * 2);
            }
            int32_t windowX = 0, windowY = 0;
            if ((modeControl >> 16) & 1)
            {
                const uint32_t windowOffset = Register(reg::PA_SC_WINDOW_OFFSET);
                windowX = int32_t(windowOffset << 17) >> 17;
                windowY = int32_t(windowOffset << 1) >> 17;
            }
            offset[0] += float(windowX);
            offset[1] += float(windowY);
            // D3D9-style pixel centers (PA_SU_VTX_CNTL.pix_center = 0) are half a pixel off the host's.
            if (m_halfPixelOffset && !(Register(reg::PA_SU_VTX_CNTL) & 1))
            {
                offset[0] += 0.5f;
                offset[1] += 0.5f;
            }
            const float width = float(targets.width), height = float(targets.height);
            system.ndcScale[0] = 2.0f * scale[0] / width;
            system.ndcScale[1] = -2.0f * scale[1] / height;
            system.ndcScale[2] = scale[2];
            system.ndcOffset[0] = 2.0f * offset[0] / width - 1.0f;
            system.ndcOffset[1] = 1.0f - 2.0f * offset[1] / height;
            system.ndcOffset[2] = offset[2];
            if (vte & (1u << 8))
                system.flags |= shader::kSysFlagXYDividedByW;
            if (vte & (1u << 9))
                system.flags |= shader::kSysFlagZDividedByW;
            if (vte & (1u << 10))
                system.flags |= shader::kSysFlagWNotReciprocal;
            if (swapIndices)
            {
                system.flags |= shader::kSysFlagIndexed;
                system.vertexIndexEndian = draw.indexEndian;
            }
            system.vertexBaseIndex = int32_t(Register(reg::VGT_INDX_OFFSET));
            system.vertexIndexMin = Register(reg::VGT_MIN_VTX_INDX);
            system.vertexIndexMax = Register(reg::VGT_MAX_VTX_INDX);
            if (system.vertexIndexMax < system.vertexIndexMin || (system.vertexIndexMax == 0 && count > 1))
                system.vertexIndexMax = UINT32_MAX;
            const uint32_t pointSize = Register(reg::PA_SU_POINT_SIZE), pointMinMax = Register(reg::PA_SU_POINT_MINMAX);
            system.pointSize = float(pointSize >> 16) / 8.0f;
            system.pointSizeMinMax[0] = float(pointMinMax & 0xFFFF) / 8.0f;
            system.pointSizeMinMax[1] = float(pointMinMax >> 16) / 8.0f;
            system.pointScreenToNdc[0] = 2.0f / width;
            system.pointScreenToNdc[1] = 2.0f / height;
            system.alphaFunc = (colorControl & 8) ? (colorControl & 7) : 7;
            system.alphaCoverageSamples = alphaCoverage ? targets.samples : 0;
            system.alphaRef = RegisterFloat(reg::RB_ALPHA_REF);
            const uint32_t programControl = Register(reg::SQ_PROGRAM_CNTL);
            system.paramGenRegister = ((programControl >> 18) & 1) ? (Register(reg::SQ_CONTEXT_MISC) >> 8) & 0xFF : 0xFFFFFFFF;
            system.resolutionScale = float(m_renderTargets.scale);
            for (uint32_t i = 0; i < 4; i++)
            {
                const uint32_t info = Register(ColorInfoRegister(i));
                const xenos::ColorFormat format = xenos::ColorFormat((info >> 16) & 0xF);
                const int32_t bias = int32_t(info << 6) >> 26;
                const bool fixed16 = format == xenos::ColorFormat::k_16_16 || format == xenos::ColorFormat::k_16_16_16_16;
                system.colorExpBias[i] = std::ldexp(fixed16 ? 1.0f / 32.0f : 1.0f, bias);
                system.minMaxFactors[i] = MinMaxFactors(Register(BlendControlRegister(i)));
            }

            // Scissor: the window scissor (offset by the window offset unless disabled), clipped to the targets.
            const uint32_t scissorTL = Register(reg::PA_SC_WINDOW_SCISSOR_TL), scissorBR = Register(reg::PA_SC_WINDOW_SCISSOR_BR);
            int32_t left = scissorTL & 0x7FFF, top = (scissorTL >> 16) & 0x7FFF, right = scissorBR & 0x7FFF, bottom = (scissorBR >> 16) & 0x7FFF;
            if (!(scissorTL & 0x80000000))
            {
                left += windowX;
                right += windowX;
                top += windowY;
                bottom += windowY;
            }
            const uint32_t screenTL = Register(reg::PA_SC_SCREEN_SCISSOR_TL), screenBR = Register(reg::PA_SC_SCREEN_SCISSOR_BR);
            left = std::max({ left, int32_t(screenTL & 0x7FFF), 0 });
            top = std::max({ top, int32_t((screenTL >> 16) & 0x7FFF), 0 });
            right = std::min({ right, int32_t(screenBR & 0x7FFF), int32_t(targets.width) });
            bottom = std::min({ bottom, int32_t((screenBR >> 16) & 0x7FFF), int32_t(targets.height) });
            if (right <= left || bottom <= top)
                return "skipped: empty scissor";

            // Constants.
            profile::ScopedZone constantZone(profile::Zone::kDrawConstants);
            const D3D12_GPU_VIRTUAL_ADDRESS systemConstants = UploadConstants(&system, sizeof(system));
            // Float constants: the ones the shader reads (all 256 with relative addressing, or with
            // DEBUG_ALL_CONSTANTS=1, diagnostics).
            static const bool allConstants = [] {
                const char* setting = std::getenv(GAME_PREFIX "_DEBUG_ALL_CONSTANTS");
                return setting && std::atoi(setting) != 0;
            }();
            const auto floatConstantDwords = [](const Shader* shader) {
                return !shader ? 0u : (allConstants || shader->parsed.relativeConstants) ? 1024u : shader->parsed.floatConstantCount * 4;
            };
            const D3D12_GPU_VIRTUAL_ADDRESS vertexConstants = UploadRegisters(reg::SHADER_CONSTANT_000_X, 1024, floatConstantDwords(vs));
            const D3D12_GPU_VIRTUAL_ADDRESS pixelConstants = UploadRegisters(reg::SHADER_CONSTANT_000_X + 1024, 1024, floatConstantDwords(ps));
            // Bool and loop constants, then the fetch constants.
            const auto boolLoopFetch = m_upload.Allocate(1024, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            CopyRegisters(reinterpret_cast<uint32_t*>(boolLoopFetch.cpu), reg::SHADER_CONSTANT_BOOL_000_031, 40);
            CopyRegisters(reinterpret_cast<uint32_t*>(boolLoopFetch.cpu) + 40, reg::SHADER_CONSTANT_FETCH_00_0, 192);
            constantZone.Stop();

            // Record: in a render pass with the targets (after the uploads and texture loads above).
            profile::ScopedZone recordZone(profile::Zone::kDrawRecord);
            // The area the draw can touch, for EDRAM ownership: the scissor, within the viewport when it applies. Clears
            // and blits draw in screen space with a scissor often all of EDRAM: those of a few vertices, with the
            // position in their first vertex fetch as D3D's have, are within their vertices' bounding box.
            int32_t areaLeft = left, areaTop = top, areaRight = right, areaBottom = bottom;
            const uint32_t firstFormat = vs->parsed.firstVertexFormat;
            if ((vte & 0xF) == 0xF)
            {
                areaLeft = std::max(areaLeft, int32_t(std::floor(offset[0] - std::abs(scale[0]))));
                areaRight = std::min(areaRight, int32_t(std::ceil(offset[0] + std::abs(scale[0]))));
                areaTop = std::max(areaTop, int32_t(std::floor(offset[1] - std::abs(scale[1]))));
                areaBottom = std::min(areaBottom, int32_t(std::ceil(offset[1] + std::abs(scale[1]))));
            }
            else if ((vte & 0xF) == 0 && vs->parsed.firstVertexConstant != UINT32_MAX && draw.initiator.SourceSelect() == 2 &&
                     draw.initiator.NumIndices() <= 6 &&
                     (firstFormat == ucode::kVertex_32_32_FLOAT || firstFormat == ucode::kVertex_32_32_32_FLOAT ||
                         firstFormat == ucode::kVertex_32_32_32_32_FLOAT))
            {
                const uint32_t constant = vs->parsed.firstVertexConstant;
                const uint32_t d0 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + constant * 2), d1 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + constant * 2 + 1);
                float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
                for (uint32_t i = 0; i < draw.initiator.NumIndices(); i++)
                {
                    const uint32_t address = (d0 & ~3u) + uint32_t(int32_t(i * vs->parsed.firstVertexStride) + vs->parsed.firstVertexOffset) * 4;
                    float xy[2];
                    for (uint32_t c = 0; c < 2; c++)
                    {
                        uint32_t raw;
                        memcpy(&raw, guest::ToHost(guest::PhysicalToGuest(address + c * 4)), 4);
                        switch (d1 & 3)
                        {
                        case 1: raw = ((raw & 0x00FF00FF) << 8) | ((raw >> 8) & 0x00FF00FF); break;
                        case 2: raw = __builtin_bswap32(raw); break;
                        case 3: raw = (raw << 16) | (raw >> 16); break;
                        default: break;
                        }
                        memcpy(&xy[c], &raw, 4);
                    }
                    minX = std::min(minX, xy[0]);
                    maxX = std::max(maxX, xy[0]);
                    minY = std::min(minY, xy[1]);
                    maxY = std::max(maxY, xy[1]);
                }
                if (minX >= -16384.0f && maxX <= 16384.0f && minY >= -16384.0f && maxY <= 16384.0f)
                {
                    areaLeft = std::max(areaLeft, int32_t(std::floor(minX)) + windowX);
                    areaRight = std::min(areaRight, int32_t(std::ceil(maxX)) + windowX);
                    areaTop = std::max(areaTop, int32_t(std::floor(minY)) + windowY);
                    areaBottom = std::min(areaBottom, int32_t(std::ceil(maxY)) + windowY);
                }
            }
            m_renderTargets.Bind(list, m_upload, targets, uint32_t(areaLeft), uint32_t(areaTop), uint32_t(std::max(areaRight, areaLeft)),
                uint32_t(std::max(areaBottom, areaTop)));
            list->SetGraphicsRootSignature(m_pipelines.RootSignature());
            list->SetPipelineState(pipeline);
            list->SetGraphicsRootConstantBufferView(kRootSystemConstants, systemConstants);
            list->SetGraphicsRootConstantBufferView(kRootVertexFloatConstants, vertexConstants);
            list->SetGraphicsRootConstantBufferView(kRootPixelFloatConstants, pixelConstants);
            list->SetGraphicsRootConstantBufferView(kRootBoolLoopFetchConstants, boolLoopFetch.gpu);
            list->SetGraphicsRootConstantBufferView(kRootVertexBindings, vertexBindings);
            list->SetGraphicsRootConstantBufferView(kRootPixelBindings, pixelBindings);
            list->SetGraphicsRootShaderResourceView(kRootSharedMemory, m_sharedMemory.GpuAddress());
            list->SetGraphicsRootDescriptorTable(kRootTextures2D, m_views.Gpu(0));
            list->SetGraphicsRootDescriptorTable(kRootTextures3D, m_views.Gpu(0));
            list->SetGraphicsRootDescriptorTable(kRootTexturesCube, m_views.Gpu(0));
            list->SetGraphicsRootDescriptorTable(kRootSamplers, m_samplers.Gpu(0));
            if (m_debugCounts)
                list->SetGraphicsRootUnorderedAccessView(m_pipelines.DebugCountsParameter(), m_debugCounts->GetGPUVirtualAddress());

            // The viewport transform maps to the guest's pixels (above); the host target is `scale` times larger.
            const int32_t hostScale = int32_t(m_renderTargets.scale);
            const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, width * float(hostScale), height * float(hostScale), 0.0f, 1.0f };
            const D3D12_RECT scissor{ left * hostScale, top * hostScale, right * hostScale, bottom * hostScale };
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            if (targets.depth)
                list->OMSetStencilRef(Register(reg::RB_STENCILREFMASK) & 0xFF);
            // Direct3D 12's blend factor applies the constant's color to color components and its alpha to alpha. Xenos's
            // constant alpha factors (14, 15) take its alpha for all four, so when color blends with them (and not with the
            // constant's color, 12 and 13), the constant goes in with its alpha as its color. DOA4's stage select fades its
            // stage previews in so, with a white constant whose alpha rises: their color didn't fade.
            float blendFactor[4] = { RegisterFloat(reg::RB_BLEND_RED), RegisterFloat(reg::RB_BLEND_GREEN), RegisterFloat(reg::RB_BLEND_BLUE),
                RegisterFloat(reg::RB_BLEND_ALPHA) };
            bool constantColor = false, constantAlpha = false;
            for (uint32_t i = 0; i < 4; i++)
            {
                if (!(colorTargets & (1u << i)))
                    continue;
                const uint32_t blend = Register(BlendControlRegister(i));
                for (const uint32_t factor : { blend & 0x1F, (blend >> 8) & 0x1F })
                {
                    constantColor |= factor == 12 || factor == 13;
                    constantAlpha |= factor == 14 || factor == 15;
                }
            }
            if (constantAlpha && !constantColor)
                blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3];
            list->OMSetBlendFactor(blendFactor);
            list->IASetPrimitiveTopology(topology);
            if (drawIndexed)
            {
                list->IASetIndexBuffer(&indexBuffer);
                list->DrawIndexedInstanced(count, 1, 0, 0, 0);
            }
            else
            {
                list->DrawInstanced(count, 1, 0, 0);
            }
            m_frameDraws++;
            profile::Count(profile::Counter::kDrawsRecorded);
            return "drawn";
        }

        void D3D12Renderer::LogDraw(const DrawCommand& draw, const char* outcome)
        {
            std::string line;
            const auto append = [&line](const char* format, auto... arguments) {
                char buffer[512];
                snprintf(buffer, sizeof(buffer), format, arguments...);
                line += buffer;
            };
            const uint32_t surface = Register(reg::RB_SURFACE_INFO), color = Register(reg::RB_COLOR_INFO), depth = Register(reg::RB_DEPTH_INFO);
            const uint32_t mode = Register(reg::RB_MODECONTROL) & 7;
            append("[draw] f%llu #%u %s: prim %u x%u src %u%s mode %u vs %016llX ps %016llX", (unsigned long long)m_frameNumber, m_drawIndex, outcome,
                draw.initiator.PrimitiveType(), draw.initiator.NumIndices(), draw.initiator.SourceSelect(), draw.initiator.Index32() ? " (32)" : "", mode,
                (unsigned long long)(m_vertexShader ? m_vertexShader->parsed.hash : 0), (unsigned long long)(m_pixelShader ? m_pixelShader->parsed.hash : 0));
            append(" | rt pitch %u msaa %u color %03X/%u depth %03X/%u ctl %08X mask %04X blend %08X cc %08X ref %g stencil %06X/%06X", surface & 0x3FFF,
                (surface >> 16) & 3, color & 0xFFF, (color >> 16) & 0xF, depth & 0xFFF, (depth >> 16) & 1, Register(reg::RB_DEPTHCONTROL),
                Register(reg::RB_COLOR_MASK), Register(reg::RB_BLENDCONTROL0), Register(reg::RB_COLORCONTROL), RegisterFloat(reg::RB_ALPHA_REF),
                Register(reg::RB_STENCILREFMASK) & 0xFFFFFF, Register(reg::RB_STENCILREFMASK_BF) & 0xFFFFFF);
            append(" | clip %08X woff %08X sc %08X-%08X su %08X vte %08X vp %g %g %g %g", Register(reg::PA_CL_CLIP_CNTL), Register(reg::PA_SC_WINDOW_OFFSET),
                Register(reg::PA_SC_WINDOW_SCISSOR_TL), Register(reg::PA_SC_WINDOW_SCISSOR_BR), Register(reg::PA_SU_SC_MODE_CNTL), Register(reg::PA_CL_VTE_CNTL),
                RegisterFloat(reg::PA_CL_VPORT_XSCALE), RegisterFloat(reg::PA_CL_VPORT_XOFFSET), RegisterFloat(reg::PA_CL_VPORT_YSCALE),
                RegisterFloat(reg::PA_CL_VPORT_YOFFSET));
            if (Register(reg::PA_SU_SC_MODE_CNTL) & (3u << 11))
                append(" | poly offset front %g scale %g back %g scale %g", RegisterFloat(reg::PA_SU_POLY_OFFSET_FRONT_OFFSET),
                    RegisterFloat(reg::PA_SU_POLY_OFFSET_FRONT_SCALE), RegisterFloat(reg::PA_SU_POLY_OFFSET_BACK_OFFSET),
                    RegisterFloat(reg::PA_SU_POLY_OFFSET_BACK_SCALE));
            if (mode == kEdramCopy)
            {
                append(" | copy %08X dest %08X pitch %08X info %08X", Register(reg::RB_COPY_CONTROL), Register(reg::RB_COPY_DEST_BASE),
                    Register(reg::RB_COPY_DEST_PITCH), Register(reg::RB_COPY_DEST_INFO));
            }
            else if (m_vertexShader)
            {
                // Index buffer and base index, then each vertex buffer: a hash of its contents and its first dwords.
                if (draw.initiator.SourceSelect() == 0)
                    append(" | ib %08X x%u e%u: %08X", draw.indexBase, draw.indexSize, draw.indexEndian, guest::LoadU32(guest::PhysicalToGuest(draw.indexBase)));
                append(" | base %d min %u max %u | const vs %08X ps %08X", int32_t(Register(reg::VGT_INDX_OFFSET)), Register(reg::VGT_MIN_VTX_INDX),
                    Register(reg::VGT_MAX_VTX_INDX), Register(reg::SQ_VS_CONST), Register(reg::SQ_PS_CONST));
                append(" | bools %08X %08X %08X %08X %08X %08X %08X %08X", Register(reg::SHADER_CONSTANT_BOOL_000_031),
                    Register(reg::SHADER_CONSTANT_BOOL_000_031 + 1), Register(reg::SHADER_CONSTANT_BOOL_000_031 + 2),
                    Register(reg::SHADER_CONSTANT_BOOL_000_031 + 3), Register(reg::SHADER_CONSTANT_BOOL_000_031 + 4),
                    Register(reg::SHADER_CONSTANT_BOOL_000_031 + 5), Register(reg::SHADER_CONSTANT_BOOL_000_031 + 6),
                    Register(reg::SHADER_CONSTANT_BOOL_000_031 + 7));
                for (const auto& [first, count] : m_drawLogConstants)
                {
                    for (uint32_t i = first; i < first + count; i++)
                    {
                        const uint32_t c = reg::SHADER_CONSTANT_000_X + i * 4;
                        append(" | %sc%u %g %g %g %g", i >= 256 ? "p" : "", i & 255, RegisterFloat(c), RegisterFloat(c + 1), RegisterFloat(c + 2),
                            RegisterFloat(c + 3));
                    }
                }
                for (uint32_t constant : m_vertexShader->parsed.vertexFetchConstants)
                {
                    const uint32_t d0 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + constant * 2);
                    const uint32_t d1 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + constant * 2 + 1);
                    const uint32_t size = ((d1 >> 2) & 0xFFFFFF) * 4;
                    append(" | vf%u %08X size %u e%u", constant, d0 & ~3u, size, d1 & 3);
                    if ((d0 & 3) != 3)
                        continue;
                    const uint32_t vertex = guest::PhysicalToGuest(d0 & ~3u);
                    uint32_t hash = 2166136261u;
                    for (uint32_t i = 0; i < std::min(size, 1u << 20); i++)
                        hash = (hash ^ *guest::ToHost(vertex + i)) * 16777619u;
                    append(" hash %08X (at %08X):", hash, vertex);
                    // (Rectangle lists, as clears draw: the three vertices, up to 24 dwords.)
                    const uint32_t shown = draw.initiator.PrimitiveType() == 8 ? std::min(size / 4, 24u) : 6u;
                    for (uint32_t i = 0; i < shown; i++)
                    {
                        const uint32_t value = guest::LoadU32(vertex + i * 4);
                        float asFloat;
                        memcpy(&asFloat, &value, 4);
                        append(" %08X(%g)", value, asFloat);
                    }
                }
                // DRAW_LOG_BUFFERS=1 also writes the index and vertex buffers, as the GPU reads them (guest
                // bytes), to dumps/{ib,vb}_f<frame>_<physical>_<bytes>.bin, once per buffer and frame;
                // DRAW_LOG_BUFFERS=<min>-<max> (hex physical addresses) only the vertex buffers in that range,
                // with their draws' index buffers.
                static const auto dumpRange = []() -> std::pair<uint32_t, uint32_t> {
                    const char* setting = std::getenv(GAME_PREFIX "_DRAW_LOG_BUFFERS");
                    if (!setting || !*setting || strcmp(setting, "0") == 0)
                        return { 1, 0 };
                    char* end = nullptr;
                    const uint32_t low = uint32_t(strtoul(setting, &end, 16));
                    if (*end != '-')
                        return { 0, UINT32_MAX };
                    return { low, uint32_t(strtoul(end + 1, nullptr, 16)) };
                }();
                bool dumpBuffers = false;
                for (uint32_t constant : m_vertexShader->parsed.vertexFetchConstants)
                {
                    const uint32_t address = Register(reg::SHADER_CONSTANT_FETCH_00_0 + constant * 2) & ~3u;
                    dumpBuffers |= address >= dumpRange.first && address <= dumpRange.second;
                }
                if (dumpBuffers)
                {
                    const auto dumpOnce = [this](const char* kind, uint32_t physical, uint32_t size) {
                        char name[96];
                        snprintf(name, sizeof(name), "/dumps/%s_f%05llu_%08X_%u.bin", kind, (unsigned long long)m_frameNumber, physical, size);
                        if (!size || !m_dumpedBuffers.insert(name).second)
                            return;
                        if (FILE* file = fopen((config::DataDirectory() + name).c_str(), "wb"))
                        {
                            fwrite(guest::ToHost(guest::PhysicalToGuest(physical)), 1, size, file);
                            fclose(file);
                        }
                    };
                    if (draw.initiator.SourceSelect() == 0)
                        dumpOnce("ib", draw.indexBase, draw.indexSize * (draw.initiator.Index32() ? 4 : 2));
                    for (uint32_t constant : m_vertexShader->parsed.vertexFetchConstants)
                    {
                        const uint32_t d0 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + constant * 2);
                        const uint32_t d1 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + constant * 2 + 1);
                        if ((d0 & 3) == 3)
                            dumpOnce("vb", d0 & ~3u, ((d1 >> 2) & 0xFFFFFF) * 4);
                    }
                }
            }
            for (const Shader* shader : { m_vertexShader, m_pixelShader })
            {
                if (!shader || mode == kEdramCopy)
                    continue;
                for (const shader::TextureBinding& binding : shader->parsed.textures)
                {
                    xenos::TextureFetch fetch;
                    for (uint32_t d = 0; d < 6; d++)
                        fetch.d[d] = Register(reg::SHADER_CONSTANT_FETCH_00_0 + binding.fetchConstant * 6 + d);
                    append(" | %s tf%u dim%u fmt %u %ux%u @%08X%s", shader == m_vertexShader ? "vs" : "ps", binding.fetchConstant, binding.dimension,
                        uint32_t(fetch.Format()), fetch.Width(), fetch.Height(), fetch.BaseAddress(), fetch.Tiled() ? " tiled" : "");
                    if (fetch.Dimension() != 1 || fetch.MipAddress() || fetch.MipMaxLevel() || fetch.PackedMips())
                        append(" (fdim %u, mips @%08X levels %u-%u%s)", fetch.Dimension(), fetch.MipAddress(), fetch.MipMinLevel(), fetch.MipMaxLevel(),
                            fetch.PackedMips() ? ", packed" : "");
                    append(" [%08X %08X %08X %08X %08X %08X]", fetch.d[0], fetch.d[1], fetch.d[2], fetch.d[3], fetch.d[4], fetch.d[5]);
                    // DRAW_LOG_TEXTURES=1 also writes each texture's base level as guest memory holds it (tiled or not, guest
                    // endian) to dumps/tex_f<frame>_<physical>_fmt<format>_<width>x<height>_p<pitch>[_tiled].bin, once per
                    // texture and frame.
                    static const bool dumpTextures = [] {
                        const char* setting = std::getenv(GAME_PREFIX "_DRAW_LOG_TEXTURES");
                        return setting && std::atoi(setting) != 0;
                    }();
                    const xenos::FormatInfo& format = xenos::GetFormatInfo(fetch.Format());
                    if (dumpTextures && format.bytesPerBlock && fetch.Dimension() == 1)
                    {
                        const xenos::TextureLayout layout = xenos::GetTextureLayout(format, fetch.BaseAddress(), 0, fetch.Width(), fetch.Height(),
                            fetch.Pitch(), fetch.Tiled(), false, 1, 1);
                        char name[128];
                        snprintf(name, sizeof(name), "/dumps/tex_f%05llu_%08X_fmt%u_%ux%u_p%u%s.bin", (unsigned long long)m_frameNumber,
                            fetch.BaseAddress(), uint32_t(fetch.Format()), fetch.Width(), fetch.Height(), fetch.Pitch(), fetch.Tiled() ? "_tiled" : "");
                        if (m_dumpedBuffers.insert(name).second)
                        {
                            if (FILE* file = fopen((config::DataDirectory() + name).c_str(), "wb"))
                            {
                                fwrite(guest::ToHost(guest::PhysicalToGuest(layout.baseAddress)), 1, layout.baseBytes, file);
                                fclose(file);
                            }
                        }
                    }
                }
                // The instructions' filter overrides (3, or 7 for anisotropy: the fetch constant's).
                for (const shader::SamplerBinding& sampler : shader->parsed.samplers)
                    append(" | %s sampler tf%u filters %u/%u/%u aniso %u", shader == m_vertexShader ? "vs" : "ps", sampler.fetchConstant, sampler.magFilter,
                        sampler.minFilter, sampler.mipFilter, sampler.anisoFilter);
            }
            fprintf(stderr, "%s\n", line.c_str());
        }

        uint32_t D3D12Renderer::HashGuest(uint32_t physical, uint32_t size)
        {
            const uint8_t* data = guest::ToHost(guest::PhysicalToGuest(physical));
            uint32_t hash = 2166136261u;
            for (uint32_t i = 0; i < size; i++)
                hash = (hash ^ data[i]) * 16777619u;
            return hash;
        }

        void D3D12Renderer::RecordLateWriteCheck(uint32_t physical, uint32_t size, bool texture)
        {
            const uint32_t hash = HashGuest(physical, size);
            uint32_t uploadedHash = hash;
            if (const uint8_t* uploaded = watch::Uploaded(physical))
            {
                uploadedHash = 2166136261u;
                for (uint32_t i = 0; i < size; i++)
                    uploadedHash = (uploadedHash ^ uploaded[i]) * 16777619u;
            }
            m_lateWriteChecks.push_back({ m_drawIndex, physical, size, hash, uploadedHash, texture });
        }

        void D3D12Renderer::CheckLateWrites()
        {
            // Changed after the draw (the CPU wrote late), or the GPU's copy differed from guest memory
            // already when the draw was recorded (written during the upload).
            uint32_t changed = 0, stale = 0;
            std::string examples;
            for (const LateWriteCheck& check : m_lateWriteChecks)
            {
                const uint32_t final = HashGuest(check.physical, check.size);
                const bool late = final != check.hash, uploadedStale = check.uploadedHash != check.hash;
                if (!late && !uploadedStale)
                    continue;
                changed += late;
                stale += uploadedStale;
                if (changed + stale <= 6)
                {
                    char text[80];
                    snprintf(text, sizeof(text), " #%u%s@%08X+%X%s%s", check.draw, check.texture ? " texture" : "", check.physical, check.size,
                        late ? " late" : "", uploadedStale ? " stale" : "");
                    examples += text;
                }
            }
            if (changed || stale)
                LOG_WARN("GPU: frame %llu: of %zu buffers and textures, %u changed after their draws, %u uploaded copies differed:%s",
                    (unsigned long long)m_frameNumber, m_lateWriteChecks.size(), changed, stale, examples.c_str());
            m_lateWriteChecks.clear();
        }

        void D3D12Renderer::ApplyGraphics()
        {
            m_graphicsVersion = settings::GraphicsVersion();
            const settings::Graphics graphics = settings::GetGraphics();
            m_window->SetFullscreen(graphics.fullscreen); // (the presenter follows the window's size)
            m_alphaCoverage = graphics.alphaToCoverage;
            m_textures.anisotropy = std::max(graphics.anisotropy, 1u);
            if (graphics.resolutionScale == m_renderTargets.scale && graphics.msaaSamples == m_renderTargets.msaaSamples)
                return;
            // New render targets (and scaled resolve copies, for another scale): the next frame redraws them.
            const auto start = std::chrono::steady_clock::now();
            m_device.WaitIdle();
            if (graphics.resolutionScale != m_renderTargets.scale)
            {
                m_scaledResolves.Reset(graphics.resolutionScale);
                m_textures.DropScaled();
            }
            m_renderTargets.scale = graphics.resolutionScale;
            m_renderTargets.msaaSamples = graphics.msaaSamples;
            m_renderTargets.Reset();
            LOG_INFO("GPU: resolution scale %u, MSAA %u (%.0f ms)", graphics.resolutionScale, graphics.msaaSamples,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }

        void D3D12Renderer::BeginFrame()
        {
            {
                profile::ScopedZone zone(profile::Zone::kSwapFenceWait);
                m_device.WaitFor(m_frameFences[m_frame]);
            }
            if (settings::GraphicsVersion() != m_graphicsVersion)
                ApplyGraphics();
            if (m_timestamps && m_timestampPending[m_frame])
            {
                const D3D12_RANGE range{ m_frame * 16, m_frame * 16 + 16 };
                void* data = nullptr;
                if (SUCCEEDED(m_timestampReadback->Map(0, &range, &data)))
                {
                    const uint64_t* ticks = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(data) + range.Begin);
                    if (ticks[1] > ticks[0])
                        profile::RecordGpuTime(double(ticks[1] - ticks[0]) * 1000.0 / double(m_timestampFrequency));
                    const D3D12_RANGE written{ 0, 0 };
                    m_timestampReadback->Unmap(0, &written);
                }
                m_timestampPending[m_frame] = false;
            }
            if (m_debugCountsPending[m_frame])
            {
                LogDebugCounts();
                m_debugCountsPending[m_frame] = false;
            }
            m_allocators[m_frame]->Reset();
            m_list->Reset(m_allocators[m_frame].Get(), nullptr);
            m_uploadAllocators[m_frame]->Reset();
            m_uploadList->Reset(m_uploadAllocators[m_frame].Get(), nullptr);
            if (m_timestamps)
                m_uploadList->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_frame * 2);
            m_upload.BeginFrame(m_frame);
            m_views.BeginFrame(m_frame);
            m_samplers.BeginFrame(m_frame);
            ID3D12DescriptorHeap* heaps[] = { m_views.Get(), m_samplers.Get() };
            m_list->SetDescriptorHeaps(2, heaps);
            m_sharedMemory.BeginFrame(m_uploadList.Get());
            m_textures.BeginCommandList();
            m_scaledResolves.BeginCommandList();
            m_drawIndex = 0;
            if (m_debugReupload)
                watch::InvalidatePhysical(0, watch::kPhysicalSize);
        }

        void D3D12Renderer::Submit()
        {
            // Uploads first, in their own submission: separate ExecuteCommandLists calls don't overlap.
            profile::ScopedZone zone(profile::Zone::kSwapSubmit);
            if (m_timestamps)
            {
                m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_frame * 2 + 1);
                m_list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_frame * 2, 2, m_timestampReadback.Get(), m_frame * 16);
                m_timestampPending[m_frame] = true;
            }
            if (m_debugCounts && m_frameNumber % 300 == 0 && !m_debugCountsPending[m_frame])
            {
                D3D12_RESOURCE_BARRIER barrier =
                    TransitionBarrier(m_debugCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_list->ResourceBarrier(1, &barrier);
                m_list->CopyBufferRegion(m_debugCountsReadback.Get(), uint64_t(m_frame) * shader::kDebugCountBytes, m_debugCounts.Get(), 0,
                    shader::kDebugCountBytes);
                barrier = TransitionBarrier(m_debugCounts.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                m_list->ResourceBarrier(1, &barrier);
                m_debugCountsPending[m_frame] = true;
            }
            m_uploadList->Close();
            m_list->Close();
            ID3D12CommandList* uploads[] = { m_uploadList.Get() };
            m_device.Queue()->ExecuteCommandLists(1, uploads);
            ID3D12CommandList* lists[] = { m_list.Get() };
            m_device.Queue()->ExecuteCommandLists(1, lists);
            m_frameFences[m_frame] = m_device.Signal();
        }

        void D3D12Renderer::Swap(const SwapCommand& swap)
        {
            // The front buffer is a texture in guest memory (written by a resolve).
            xenos::TextureFetch fetch;
            memcpy(fetch.d, swap.fetch, sizeof(fetch.d));
            Presenter::Source source;
            source.srv = m_views.Gpu(m_textures.GetSrv(m_list.Get(), m_upload, fetch, 1, &source.scale));
            source.width = fetch.Width();
            source.height = fetch.Height();
            // The display's gamma ramp, for the present pass (DEBUG_GAMMA_RAMP=0 shows the front buffer without it).
            static const bool gammaRamp = [] {
                const char* setting = std::getenv(GAME_PREFIX "_DEBUG_GAMMA_RAMP");
                return !setting || std::atoi(setting) != 0;
            }();
            const UploadAllocator::Allocation ramp = m_upload.Allocate(256 * 4, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            for (uint32_t i = 0; i < 256; i++)
            {
                const uint32_t identity = (i << 2) | (i >> 6);
                const uint32_t entry = gammaRamp && swap.gammaRamp ? swap.gammaRamp[i] : identity | (identity << 10) | (identity << 20);
                memcpy(ramp.cpu + i * 4, &entry, 4);
            }
            source.gammaRamp = ramp.gpu;
            m_renderTargets.EndPass(m_list.Get());
            // The notice to show over the frame, drawn for the largest the frame is shown at: in the window, or dumped at
            // the front buffer's own size.
            m_overlay.Prepare(m_list.Get(), m_upload, m_frameNumber,
                std::max(m_presenter.WindowImageHeight(source), source.height * source.scale));
            m_presenter.Record(m_list.Get(), &source);
            if (m_checkLateWrites)
                CheckLateWrites();
            if (m_frameNumber >= m_summaryFirst && m_frameNumber < m_summaryFirst + m_summaryCount)
            {
                const uint64_t skips = GetStatistics().predicatedSkips.load();
                std::string line;
                for (const auto& [key, count] : m_summaryOutcomes)
                    line += " " + key + "=" + std::to_string(count);
                LOG_INFO("GPU: frame %llu: %llu predicated packets skipped;%s", (unsigned long long)m_frameNumber,
                    (unsigned long long)(skips - m_summarySkipsAtFrameStart), line.c_str());
                m_summaryOutcomes.clear();
            }
            m_summarySkipsAtFrameStart = GetStatistics().predicatedSkips.load();

            const bool fightDump = swap.fightFrame >= 0 && DumpThisFrame(true, uint64_t(swap.fightFrame));
            const bool frameDump = DumpThisFrame(false, m_frameNumber) || !m_captures.empty() ||
                g_frameDumpRequested.exchange(false, std::memory_order_relaxed);
            if (frameDump || fightDump)
                m_presenter.RecordDump(m_list.Get(), &source);
            Submit();
            {
                profile::ScopedZone zone(profile::Zone::kSwapPresent);
                m_presenter.Present();
            }
            if (frameDump || fightDump)
            {
                m_device.WaitFor(m_frameFences[m_frame]);
                char name[64];
                if (frameDump)
                {
                    snprintf(name, sizeof(name), "/dumps/frame_%05llu.png", (unsigned long long)m_frameNumber);
                    m_presenter.FinishDump(config::DataDirectory() + name);
                }
                if (fightDump)
                {
                    snprintf(name, sizeof(name), "/dumps/fight_%05lld.png", (long long)swap.fightFrame);
                    m_presenter.FinishDump(config::DataDirectory() + name);
                }
                WriteCaptures();
            }

            debug::FloatSearchOnFrame(m_frameNumber);
            debug::WatchChangesOnFrame(m_frameNumber);
            debug::WriteWatchOnFrame(m_frameNumber);
            audit::Frame(m_frameNumber);
            // Frames that waited for new pipelines (the driver compiling their shaders): hitches.
            if (m_framePipelineMs >= 2.0)
                LOG_INFO("GPU: frame %llu: waited %.0f ms for pipelines", (unsigned long long)m_frameNumber, m_framePipelineMs);
            m_framePipelineMs = 0.0;
            m_frameNumber++;
            UpdateTitle();
            m_frame = (m_frame + 1) % kFramesInFlight;
            BeginFrame();
        }

        // DEBUG_NAN: logs the shaders whose counts changed since they were last logged, from the copy this frame slot made.
        void D3D12Renderer::LogDebugCounts()
        {
            const D3D12_RANGE range{ size_t(m_frame) * shader::kDebugCountBytes, size_t(m_frame + 1) * shader::kDebugCountBytes };
            void* data = nullptr;
            if (FAILED(m_debugCountsReadback->Map(0, &range, &data)))
                return;
            const uint32_t* counts = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data) + range.Begin);
            for (uint32_t slot = 0; slot < shader::kDebugCountSlots; slot++)
            {
                const uint32_t* entry = counts + slot * shader::kDebugSlotDwords;
                uint32_t* logged = m_debugCountsLogged.data() + slot * shader::kDebugSlotDwords;
                if (entry[3] == logged[3] && entry[4] == logged[4] && entry[7] == logged[7] && entry[8] == logged[8] && entry[9] == logged[9])
                    continue;
                const uint32_t flags = entry[2];
                // The interpolator components seen non-finite, as "interpolators 3.w 5.xyzw".
                std::string interpolators = "interpolators";
                for (uint32_t n = 0; n < 16; n++)
                {
                    const uint32_t bits = (entry[8 + (n >> 3)] >> ((n & 7) * 4)) & 0xF;
                    if (!bits)
                        continue;
                    interpolators += " " + std::to_string(n) + ".";
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        if (bits & (1u << c))
                            interpolators += "xyzw"[c];
                    }
                }
                auto kinds = [flags](std::initializer_list<std::pair<uint32_t, std::string>> names) {
                    std::string text;
                    for (const auto& [bit, name] : names)
                    {
                        if (flags & bit)
                            text += std::string(text.empty() ? " (" : ", ") + name;
                    }
                    return text.empty() ? text : text + ")";
                };
                LOG_INFO("DEBUG_NAN: by frame %llu, %s_%08X%08X: %u invocations with a product of 0 and infinity or NaN (ALU instructions %u-%u), "
                         "%u with non-finite outputs%s, %u with non-finite inputs%s",
                    (unsigned long long)m_frameNumber, (flags & 1) ? "ps" : "vs", entry[1], entry[0], entry[3], 0xFFFFu - entry[5], entry[6], entry[4],
                    kinds({ { shader::kDebugNonFinitePosition, "position" }, { shader::kDebugNonFiniteExports, interpolators },
                        { shader::kDebugNonFiniteColor, "color or depth" } })
                        .c_str(),
                    entry[7],
                    kinds({ { shader::kDebugNonFiniteVertexData, "vertex data" }, { shader::kDebugNonFiniteInterpolators, interpolators },
                        { shader::kDebugNonFiniteTexels, "texels" } })
                        .c_str());
                memcpy(logged, entry, shader::kDebugSlotDwords * 4);
            }
            const D3D12_RANGE written{ 0, 0 };
            m_debugCountsReadback->Unmap(0, &written);
        }

        void D3D12Renderer::UpdateTitle()
        {
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - m_titleTime).count();
            if (seconds < 1.0)
                return;
            char title[160];
            const double frames = double(m_frameNumber - m_titleFrame);
            snprintf(title, sizeof(title), GAME_TITLE " - %.1f fps, %.0f draws/frame, %llu uploaded MB, %llu texture loads, %llu resolves",
                frames / seconds, frames ? double(m_frameDraws) / frames : 0.0, (unsigned long long)(m_sharedMemory.uploadedBytes >> 20),
                (unsigned long long)m_textures.loads, (unsigned long long)m_renderTargets.resolves);
            if (m_pipelineMs > 100.0 || m_skippedDraws)
                LOG_INFO("GPU: last %.1f s: %.0f ms waiting for pipelines, %llu draws skipped while shaders and pipelines compile", seconds,
                    m_pipelineMs, (unsigned long long)m_skippedDraws);
            SavePipelines();
            m_pipelineMs = 0.0;
            m_skippedDraws = 0;
            m_window->SetTitle(title);
            m_titleTime = now;
            m_titleFrame = m_frameNumber;
            m_frameDraws = 0;
        }
    }

    std::unique_ptr<gpu::Renderer> CreateRenderer(ui::Window& window)
    {
        auto renderer = std::make_unique<D3D12Renderer>();
        if (!renderer->Initialize(window))
            return nullptr;
        return renderer;
    }
}

namespace gpu
{
    void RequestFrameDump()
    {
        static std::once_flag directory;
        std::call_once(directory, [] {
            std::error_code error;
            std::filesystem::create_directories(config::DataDirectory() + "/dumps", error);
        });
        d3d12::g_frameDumpRequested.store(true, std::memory_order_relaxed);
    }
}
