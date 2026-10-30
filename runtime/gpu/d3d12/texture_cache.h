#pragma once

// Host textures for guest texture fetch constants: 2D (1D as 2D) and cube textures with their mip
// levels. Textures are untiled on the GPU from the shared memory buffer (so render-to-texture results
// from resolves are seen without CPU readback) and are reloaded when the guest memory they cover
// changes. With resolution scaling, textures resolves wrote load from their scaled copies instead, at the
// render targets' scale. Fetches that read a texture's components as signed view it through its format's SNORM
// version. SRVs and samplers live in persistent slots of the shader-visible heaps, which shaders index bindlessly.
#include "gpu/d3d12/resources.h"
#include "gpu/d3d12/scaled_resolves.h"
#include "gpu/d3d12/shared_memory.h"
#include "gpu/shader/shader.h"
#include "gpu/xenos.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gpu::d3d12
{
    class TextureCache
    {
    public:
        bool Initialize(ID3D12Device* device, DescriptorHeap& views, DescriptorHeap& samplers, SharedMemory& sharedMemory,
            ScaledResolves& scaledResolves);

        // Persistent SRV index for the texture `fetch` describes, as a view of the dimension the
        // shader declared (ucode dimension). Unsupported textures get a null view. `scale`, if given, receives the
        // texture's host texels per guest texel (above 1 for one loaded from scaled resolve copies).
        uint32_t GetSrv(ID3D12GraphicsCommandList* list, UploadAllocator& upload, const xenos::TextureFetch& fetch, uint32_t shaderDimension,
            uint32_t* scale = nullptr);
        // Persistent sampler index for a sampler binding.
        uint32_t GetSampler(const xenos::TextureFetch& fetch, const shader::SamplerBinding& binding);

        uint32_t NullSrv(uint32_t shaderDimension) const;
        // Drops the textures loaded from scaled resolve copies, e.g. for another resolution scale (with the GPU idle).
        void DropScaled();

        // Anisotropic filtering for linearly minified textures (settings::Graphics; 1: as the guest asks).
        uint32_t anisotropy = 1;
        // Buffers decay to COMMON when a command list finishes executing: call before recording the next.
        void BeginCommandList() { m_stagingState = D3D12_RESOURCE_STATE_COMMON; }
        uint32_t DefaultSampler() const { return m_defaultSampler; }

        uint64_t loads = 0; // statistics

        // Called before a texture load records its work, which must be outside render passes.
        std::function<void()> beforeLoad;
        // Debugging: called after a texture load requested its guest memory range.
        std::function<void(uint32_t base, uint32_t size)> afterLoadRequest;

    private:
        struct Key
        {
            uint32_t base, format, endian, tiled, pitch, width, height, dimension, packed, mipAddress, levels;
            uint32_t scale; // 1, or the resolution scale for a texture loaded from scaled resolve copies
            bool operator==(const Key&) const = default;
        };
        struct KeyHash
        {
            size_t operator()(const Key& key) const;
        };
        struct Texture
        {
            Key key{};
            ComPtr<ID3D12Resource> resource;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
            xenos::TextureLayout layout; // where the levels are in guest memory
            uint64_t version = UINT64_MAX;
            std::unordered_map<uint32_t, uint32_t> srvs; // by swizzle, and bit 12 for the SNORM view
        };

        Texture* CreateTexture(const Key& key);
        void Load(ID3D12GraphicsCommandList* list, UploadAllocator& upload, Texture& texture);
        // Sum of the guest memory versions of the texture's levels.
        static uint64_t Version(const Texture& texture);

        ID3D12Device* m_device = nullptr;
        DescriptorHeap* m_views = nullptr;
        DescriptorHeap* m_samplers = nullptr;
        SharedMemory* m_sharedMemory = nullptr;
        ScaledResolves* m_scaledResolves = nullptr;

        std::unordered_map<Key, std::unique_ptr<Texture>, KeyHash> m_textures;
        std::unordered_map<uint64_t, uint32_t> m_samplerSlots; // by D3D12_SAMPLER_DESC hash
        uint32_t m_nullSrvs[4] = {};
        uint32_t m_defaultSampler = 0;

        ComPtr<ID3D12RootSignature> m_untileRootSignature;
        ComPtr<ID3D12PipelineState> m_untilePipeline;
        ComPtr<ID3D12RootSignature> m_scaledUntileRootSignature;
        ComPtr<ID3D12PipelineState> m_scaledUntilePipeline;
        ComPtr<ID3D12Resource> m_staging; // untiled data before the copies into the texture
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> m_footprints;
        uint64_t m_stagingSize = 0;
        D3D12_RESOURCE_STATES m_stagingState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        // Outgrown staging buffers, kept alive because recorded (or executing) work may still use them.
        std::vector<ComPtr<ID3D12Resource>> m_retiredStaging;
    };
}
