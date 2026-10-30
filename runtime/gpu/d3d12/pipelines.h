#pragma once

// The root signature shared by all translated shaders, and pipeline state objects for draws.
#include "gpu/d3d12/device.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gpu::d3d12
{
    // Root parameter indices (see hlsl.h for the shader side).
    enum RootParameter : uint32_t
    {
        kRootSystemConstants = 0,   // b0
        kRootVertexFloatConstants,  // b1, vertex
        kRootPixelFloatConstants,   // b1, pixel
        kRootBoolLoopFetchConstants, // b2
        kRootVertexBindings,        // b3, vertex
        kRootPixelBindings,         // b3, pixel
        kRootSharedMemory,          // t0 space1
        kRootTextures2D,            // t0 space2 (table)
        kRootTextures3D,            // t0 space3 (table)
        kRootTexturesCube,          // t0 space4 (table)
        kRootSamplers,              // s0 (table)
        kRootParameterCount,
    };

    inline constexpr uint32_t kPipelineFlagNoDepthClip = 1u << 0;

    enum class GeometryShader : uint8_t
    {
        kNone,
        kRectangleList,
        kPointList,
    };

    // Everything that selects a pipeline; plain data, compared bytewise.
    struct PipelineKey
    {
        const void* vertexShader = nullptr; // compile jobs (stable addresses; shaders that translate the same share one)
        const void* pixelShader = nullptr;
        GeometryShader geometryShader = GeometryShader::kNone;
        uint8_t topologyType = 0; // D3D12_PRIMITIVE_TOPOLOGY_TYPE
        uint8_t stripCut = 0;     // 0 none, 1 0xFFFF, 2 0xFFFFFFFF
        uint8_t samples = 1;
        uint8_t renderTargetCount = 0;
        uint8_t cullMode = 1;     // D3D12_CULL_MODE
        uint8_t frontCounterClockwise = 0;
        uint8_t fillWireframe = 0;
        uint16_t renderTargetFormats[4] = {};
        uint16_t depthFormat = 0;
        uint16_t writeMasks = 0;  // 4 bits per render target
        uint32_t blend[4] = {};   // per target: enable, factors and ops (packed); 0 = disabled
        uint32_t depthControl = 0; // RB_DEPTHCONTROL bits used (0 when no depth buffer)
        uint32_t stencilMasks = 0; // read mask, write mask (front), back read/write
        int32_t depthBias = 0;
        float slopeScaledDepthBias = 0.0f;
        uint32_t flags = 0;    // kPipelineFlag*; also keeps the struct free of implicit padding
    };
    static_assert(sizeof(PipelineKey) == 72);

    // Pipelines are created on worker threads: the driver compiles their shaders then, which takes up to a few hundred
    // milliseconds each when its own cache doesn't have them (a first run, or after a driver update), and would stall
    // the frame. Shader blobs passed in must outlive the cache (compile jobs do).
    class PipelineCache
    {
    public:
        bool Initialize(ID3D12Device* device);
        ~PipelineCache();
        ID3D12RootSignature* RootSignature() const { return m_rootSignature.Get(); }
        // DEBUG_NAN: the root parameter of the shaders' counts buffer (UINT32_MAX when off).
        uint32_t DebugCountsParameter() const { return m_debugCountsParameter; }

        // The pipeline for a draw, starting its creation if it's new: waits up to `waitMs` for it (adding the time to
        // `waitedMs`), and returns null if it isn't ready by then, or if creation failed (`failed`).
        ID3D12PipelineState* Get(const PipelineKey& key, ID3DBlob* vertexShader, ID3DBlob* pixelShader, double waitMs, bool& failed,
            double& waitedMs);
        // Creates a pipeline in the background, after any that draws wait for: e.g. one an earlier run used. Thread-safe.
        void Prepare(const PipelineKey& key, ID3DBlob* vertexShader, ID3DBlob* pixelShader);
        // The pipelines created for draws since the last call (not the prepared ones), to prepare in later runs.
        std::vector<PipelineKey> TakeNewKeys();

        // Packs Xenos RB_BLENDCONTROL into PipelineKey::blend (0 if blending is effectively off).
        static uint32_t PackBlend(uint32_t blendControl);

        std::atomic<uint64_t> created{ 0 };   // statistics
        std::atomic<uint32_t> preparing{ 0 }; // prepared pipelines not created yet

    private:
        struct Entry
        {
            PipelineKey key;
            ID3DBlob* vertexShader = nullptr;
            ID3DBlob* pixelShader = nullptr;
            bool prepared = false;        // asked for by Prepare
            std::atomic<uint8_t> state{}; // kPending, kReady or kFailed
            ComPtr<ID3D12PipelineState> pipeline;
        };
        static constexpr uint8_t kPending = 0, kReady = 1, kFailed = 2;

        ComPtr<ID3D12PipelineState> Create(const Entry& entry) const;
        void Worker();

        struct KeyHash
        {
            size_t operator()(const PipelineKey& key) const;
        };
        struct KeyEqual
        {
            bool operator()(const PipelineKey& a, const PipelineKey& b) const;
        };

        ID3D12Device* m_device = nullptr;
        ComPtr<ID3D12RootSignature> m_rootSignature;
        uint32_t m_debugCountsParameter = UINT32_MAX;
        ComPtr<ID3DBlob> m_rectangleGeometryShader, m_pointGeometryShader;

        std::mutex m_lock;
        std::condition_variable m_queued, m_finished;
        std::unordered_map<PipelineKey, std::unique_ptr<Entry>, KeyHash, KeyEqual> m_pipelines; // entries are never removed
        std::deque<Entry*> m_urgent, m_background; // for draws, and prepared
        std::vector<PipelineKey> m_newKeys;
        std::vector<std::thread> m_workers;
        bool m_stop = false;
    };
}
