#include "gpu/d3d12/pipelines.h"

#include "gpu/shader/hlsl.h"
#include "log.h"
#include "util/profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace gpu::d3d12
{
    namespace
    {
        D3D12_BLEND BlendFactor(uint32_t factor, bool alpha)
        {
            switch (factor)
            {
            case 0: return D3D12_BLEND_ZERO;
            case 1: return D3D12_BLEND_ONE;
            case 4: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
            case 5: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
            case 6: return D3D12_BLEND_SRC_ALPHA;
            case 7: return D3D12_BLEND_INV_SRC_ALPHA;
            case 8: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
            case 9: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
            case 10: return D3D12_BLEND_DEST_ALPHA;
            case 11: return D3D12_BLEND_INV_DEST_ALPHA;
            case 12:
            case 14: return D3D12_BLEND_BLEND_FACTOR;
            case 13:
            case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
            case 16: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
            default: return D3D12_BLEND_ONE;
            }
        }

        D3D12_BLEND_OP BlendOp(uint32_t op)
        {
            switch (op)
            {
            case 1: return D3D12_BLEND_OP_SUBTRACT;
            case 2: return D3D12_BLEND_OP_MIN;
            case 3: return D3D12_BLEND_OP_MAX;
            case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
            default: return D3D12_BLEND_OP_ADD;
            }
        }
    }

    size_t PipelineCache::KeyHash::operator()(const PipelineKey& key) const
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&key);
        for (size_t i = 0; i < sizeof(key); i++)
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        return size_t(hash);
    }

    bool PipelineCache::KeyEqual::operator()(const PipelineKey& a, const PipelineKey& b) const
    {
        return memcmp(&a, &b, sizeof(PipelineKey)) == 0;
    }

    uint32_t PipelineCache::PackBlend(uint32_t blendControl)
    {
        const uint32_t colorSource = blendControl & 0x1F, colorOp = (blendControl >> 5) & 7, colorDest = (blendControl >> 8) & 0x1F;
        const uint32_t alphaSource = (blendControl >> 16) & 0x1F, alphaOp = (blendControl >> 21) & 7, alphaDest = (blendControl >> 24) & 0x1F;
        // Blending is always on in Xenos; ONE * src + ZERO * dst is "off".
        if (colorSource == 1 && colorDest == 0 && colorOp == 0 && alphaSource == 1 && alphaDest == 0 && alphaOp == 0)
            return 0;
        return 0x80000000u | (blendControl & 0x1FFF1FFF);
    }

    bool PipelineCache::Initialize(ID3D12Device* device)
    {
        m_device = device;

        D3D12_ROOT_PARAMETER parameters[kRootParameterCount + 2]{};
        auto cbv = [&](RootParameter index, uint32_t reg, D3D12_SHADER_VISIBILITY visibility) {
            parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[index].Descriptor = { reg, 0 };
            parameters[index].ShaderVisibility = visibility;
        };
        cbv(kRootSystemConstants, 0, D3D12_SHADER_VISIBILITY_ALL);
        cbv(kRootVertexFloatConstants, 1, D3D12_SHADER_VISIBILITY_VERTEX);
        cbv(kRootPixelFloatConstants, 1, D3D12_SHADER_VISIBILITY_PIXEL);
        cbv(kRootBoolLoopFetchConstants, 2, D3D12_SHADER_VISIBILITY_ALL);
        cbv(kRootVertexBindings, 3, D3D12_SHADER_VISIBILITY_VERTEX);
        cbv(kRootPixelBindings, 3, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[kRootSharedMemory].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[kRootSharedMemory].Descriptor = { 0, 1 };
        parameters[kRootSharedMemory].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Unbounded tables over the start of the heaps (the persistent descriptors).
        D3D12_DESCRIPTOR_RANGE ranges[4] = {
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0 },
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 3, 0 },
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 4, 0 },
            { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 0, 0 },
        };
        for (uint32_t i = 0; i < 4; i++)
        {
            RootParameter index = RootParameter(kRootTextures2D + i);
            parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[index].DescriptorTable = { 1, &ranges[i] };
            parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        // DEBUG_PIPELINE_SALT=n adds an unused root constant in register space 100 + n: every pipeline is new to the
        // driver's shader cache, as on a first run (to measure pipeline creation).
        uint32_t parameterCount = kRootParameterCount;
        if (const char* salt = std::getenv(GAME_PREFIX "_DEBUG_PIPELINE_SALT"))
        {
            parameters[parameterCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[parameterCount].Constants = { 0, 100u + uint32_t(std::atoi(salt)), 1 };
            parameters[parameterCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameterCount++;
        }
        // DEBUG_NAN: the shaders' counts (u0, see hlsl.h).
        if (shader::DebugNan())
        {
            m_debugCountsParameter = parameterCount;
            parameters[parameterCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[parameterCount].Descriptor = { 0, 0 };
            parameters[parameterCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameterCount++;
        }
        D3D12_ROOT_SIGNATURE_DESC description{ parameterCount, parameters, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS };
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature))))
        {
            LOG_ERROR("D3D12: can't create the draw root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "");
            return false;
        }

        m_rectangleGeometryShader = CompileShader(shader::RectangleListGeometryShader(), "main", "gs_5_1", "rectangle list");
        m_pointGeometryShader = CompileShader(shader::PointListGeometryShader(), "main", "gs_5_1", "point list");
        if (!m_rectangleGeometryShader || !m_pointGeometryShader)
            return false;

        // Half the hardware threads, below the game's priority (the driver may compile on threads of its own too).
        const uint32_t workers = std::max(2u, std::thread::hardware_concurrency() / 2);
        for (uint32_t i = 0; i < workers; i++)
            m_workers.emplace_back([this, i] {
                profile::NameThread("Pipeline compiler %u", i);
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                Worker();
            });
        return true;
    }

    PipelineCache::~PipelineCache()
    {
        {
            std::lock_guard lock(m_lock);
            m_stop = true;
        }
        m_queued.notify_all();
        for (std::thread& worker : m_workers)
            worker.join();
    }

    ID3D12PipelineState* PipelineCache::Get(const PipelineKey& key, ID3DBlob* vertexShader, ID3DBlob* pixelShader, double waitMs, bool& failed,
        double& waitedMs)
    {
        std::unique_lock lock(m_lock);
        auto& slot = m_pipelines[key];
        if (!slot)
        {
            slot = std::make_unique<Entry>();
            slot->key = key;
            slot->vertexShader = vertexShader;
            slot->pixelShader = pixelShader;
            m_urgent.push_back(slot.get());
            m_queued.notify_one();
        }
        else if (slot->prepared && slot->state.load(std::memory_order_acquire) == kPending)
        {
            // A draw needs a prepared pipeline that's still queued: ahead of the others.
            if (const auto queued = std::find(m_background.begin(), m_background.end(), slot.get()); queued != m_background.end())
            {
                m_background.erase(queued);
                m_urgent.push_back(slot.get());
            }
        }
        Entry* entry = slot.get();
        if (entry->state.load(std::memory_order_acquire) == kPending && waitMs > 0.0)
        {
            // (Timed only here: reading the clock for every draw added up.)
            const auto start = std::chrono::steady_clock::now();
            m_finished.wait_for(lock, std::chrono::duration<double, std::milli>(waitMs),
                [&] { return entry->state.load(std::memory_order_acquire) != kPending; });
            waitedMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
        const uint8_t state = entry->state.load(std::memory_order_acquire);
        failed = state == kFailed;
        return state == kReady ? entry->pipeline.Get() : nullptr;
    }

    void PipelineCache::Prepare(const PipelineKey& key, ID3DBlob* vertexShader, ID3DBlob* pixelShader)
    {
        std::lock_guard lock(m_lock);
        auto& slot = m_pipelines[key];
        if (slot)
            return;
        slot = std::make_unique<Entry>();
        slot->key = key;
        slot->vertexShader = vertexShader;
        slot->pixelShader = pixelShader;
        slot->prepared = true;
        m_background.push_back(slot.get());
        preparing++;
        m_queued.notify_one();
    }

    std::vector<PipelineKey> PipelineCache::TakeNewKeys()
    {
        std::lock_guard lock(m_lock);
        return std::exchange(m_newKeys, {});
    }

    void PipelineCache::Worker()
    {
        for (;;)
        {
            Entry* entry = nullptr;
            {
                std::unique_lock lock(m_lock);
                m_queued.wait(lock, [&] { return m_stop || !m_urgent.empty() || !m_background.empty(); });
                if (m_stop)
                    return;
                std::deque<Entry*>& queue = !m_urgent.empty() ? m_urgent : m_background;
                entry = queue.front();
                queue.pop_front();
            }
            const auto start = std::chrono::steady_clock::now();
            ComPtr<ID3D12PipelineState> pipeline = Create(*entry);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (ms >= 500.0)
                LOG_INFO("GPU: creating a pipeline took %.0f ms", ms);
            {
                std::lock_guard lock(m_lock);
                if (pipeline)
                {
                    created++;
                    if (!entry->prepared)
                        m_newKeys.push_back(entry->key);
                }
                if (entry->prepared)
                    preparing--;
                entry->pipeline = std::move(pipeline);
                entry->state.store(entry->pipeline ? kReady : kFailed, std::memory_order_release);
            }
            m_finished.notify_all();
        }
    }

    ComPtr<ID3D12PipelineState> PipelineCache::Create(const Entry& entry) const
    {
        profile::Count(profile::Counter::kPipelinesCreated);
        const PipelineKey& key = entry.key;
        ID3DBlob* vertexShader = entry.vertexShader;
        ID3DBlob* pixelShader = entry.pixelShader;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = m_rootSignature.Get();
        description.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        if (pixelShader)
            description.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
        if (key.geometryShader == GeometryShader::kRectangleList)
            description.GS = { m_rectangleGeometryShader->GetBufferPointer(), m_rectangleGeometryShader->GetBufferSize() };
        else if (key.geometryShader == GeometryShader::kPointList)
            description.GS = { m_pointGeometryShader->GetBufferPointer(), m_pointGeometryShader->GetBufferSize() };

        description.BlendState.IndependentBlendEnable = TRUE;
        for (uint32_t i = 0; i < 4; i++)
        {
            D3D12_RENDER_TARGET_BLEND_DESC& target = description.BlendState.RenderTarget[i];
            const uint32_t blend = key.blend[i];
            target.BlendEnable = blend != 0;
            target.SrcBlend = BlendFactor(blend & 0x1F, false);
            target.BlendOp = BlendOp((blend >> 5) & 7);
            target.DestBlend = BlendFactor((blend >> 8) & 0x1F, false);
            target.SrcBlendAlpha = BlendFactor((blend >> 16) & 0x1F, true);
            target.BlendOpAlpha = BlendOp((blend >> 21) & 7);
            target.DestBlendAlpha = BlendFactor((blend >> 24) & 0x1F, true);
            target.LogicOp = D3D12_LOGIC_OP_NOOP;
            target.RenderTargetWriteMask = uint8_t((key.writeMasks >> (i * 4)) & 0xF);
        }
        description.SampleMask = UINT_MAX;

        D3D12_RASTERIZER_DESC& rasterizer = description.RasterizerState;
        rasterizer.FillMode = key.fillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
        rasterizer.CullMode = D3D12_CULL_MODE(key.cullMode);
        rasterizer.FrontCounterClockwise = key.frontCounterClockwise;
        rasterizer.DepthBias = key.depthBias;
        rasterizer.SlopeScaledDepthBias = key.slopeScaledDepthBias;
        rasterizer.DepthClipEnable = (key.flags & kPipelineFlagNoDepthClip) ? FALSE : TRUE;
        rasterizer.MultisampleEnable = key.samples > 1;

        D3D12_DEPTH_STENCIL_DESC& depth = description.DepthStencilState;
        const uint32_t control = key.depthControl;
        depth.DepthEnable = (control >> 1) & 1;
        depth.DepthWriteMask = ((control >> 2) & 1) && depth.DepthEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D12_COMPARISON_FUNC(((control >> 4) & 7) + 1);
        depth.StencilEnable = control & 1;
        depth.StencilReadMask = uint8_t(key.stencilMasks);
        depth.StencilWriteMask = uint8_t(key.stencilMasks >> 8);
        depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC(((control >> 8) & 7) + 1);
        depth.FrontFace.StencilFailOp = D3D12_STENCIL_OP(((control >> 11) & 7) + 1);
        depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP(((control >> 14) & 7) + 1);
        depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP(((control >> 17) & 7) + 1);
        if ((control >> 7) & 1) // separate back face state
        {
            depth.BackFace.StencilFunc = D3D12_COMPARISON_FUNC(((control >> 20) & 7) + 1);
            depth.BackFace.StencilFailOp = D3D12_STENCIL_OP(((control >> 23) & 7) + 1);
            depth.BackFace.StencilPassOp = D3D12_STENCIL_OP(((control >> 26) & 7) + 1);
            depth.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP(((control >> 29) & 7) + 1);
        }
        else
        {
            depth.BackFace = depth.FrontFace;
        }

        description.IBStripCutValue = key.stripCut == 1 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
            : key.stripCut == 2                         ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                        : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topologyType);
        description.NumRenderTargets = key.renderTargetCount;
        for (uint32_t i = 0; i < key.renderTargetCount; i++)
            description.RTVFormats[i] = DXGI_FORMAT(key.renderTargetFormats[i]);
        description.DSVFormat = DXGI_FORMAT(key.depthFormat);
        description.SampleDesc.Count = key.samples;

        ComPtr<ID3D12PipelineState> pipeline;
        if (FAILED(m_device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline))))
        {
            LOG_ERROR("D3D12: pipeline creation failed (vs %p, ps %p, topology %u, %u targets)", key.vertexShader, key.pixelShader, key.topologyType,
                key.renderTargetCount);
            return nullptr;
        }
        return pipeline;
    }
}
