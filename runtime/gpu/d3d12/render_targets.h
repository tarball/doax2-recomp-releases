#pragma once

// EDRAM render targets as host textures, and resolves (EDRAM -> memory copies).
//
// Each distinct EDRAM surface (base tile, pitch, MSAA, format) gets its own host render target sized
// to the EDRAM space after its base. Surfaces overlap in EDRAM: the cache keeps which target holds each tile's current
// contents (the last to draw there), and a target that draws over or resolves tiles another holds first copies them in,
// sample by sample through the EDRAM layout, when both have the same format. That's how D3D's clears work on the 360:
// DOAX2 clears a 1280x720 surface as 640 pixels wide with 2x MSAA (the same EDRAM, half the pixels), then draws it
// unmultisampled. Between formats, contents aren't carried over (reinterpreting them is rare).
//
// Draws are recorded in explicit render passes, which the cache begins when targets are bound and
// ends before any other work (copies, dispatches, barriers and clears aren't allowed inside a pass),
// so tile-based GPUs know exactly where targets must be loaded and stored.
#include "gpu/d3d12/resources.h"
#include "gpu/d3d12/scaled_resolves.h"
#include "gpu/d3d12/shared_memory.h"
#include "gpu/xenos.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gpu::d3d12
{
    class RenderTargetCache
    {
    public:
        bool Initialize(ID3D12Device* device, DescriptorHeap& views, SharedMemory& sharedMemory, ScaledResolves& scaledResolves);

        struct Target
        {
            uint32_t base = 0, pitch = 0, samples = 1, format = 0;
            uint32_t hostSamples = 1; // samples, or more with an MSAA override
            bool depth = false;
            uint32_t width = 0, height = 0;
            ComPtr<ID3D12Resource> resource;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_CPU_DESCRIPTOR_HANDLE view{}; // RTV or DSV
            uint32_t srv = 0;                   // persistent SRV (color, or depth)
            uint32_t stencilSrv = 0;            // depth targets: persistent SRV of the stencil
            DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // RTV/DSV format
            // EDRAM tiles (columns c0-c1, rows r0-r1 of the surface) known to be this target's while the cache's owner
            // epoch is `checkedEpoch` (see Acquire).
            uint64_t checkedEpoch = 0;
            uint32_t checked[4] = {};
        };

        // Render targets for a draw from the current registers. `colorMask` selects the color targets
        // (0-3); `depth` whether to use the depth buffer. Returns false if there are none.
        struct Bindings
        {
            Target* color[4] = {};
            Target* depth = nullptr;
            uint32_t samples = 1; // host samples
            uint32_t width = 0, height = 0; // of the targets (the smallest)
        };
        bool Lookup(uint32_t colorMask, bool depth, Bindings& out);
        // Makes the targets current for a draw within the rectangle (guest pixels): inside a render pass with them
        // (ending a pass with others), holding the current contents of the EDRAM there.
        void Bind(ID3D12GraphicsCommandList* list, UploadAllocator& upload, const Bindings& targets, uint32_t left, uint32_t top, uint32_t right,
            uint32_t bottom);
        // Ends the current render pass, if any. Call before recording anything but draws.
        void EndPass(ID3D12GraphicsCommandList* list);

        // Executes a resolve draw (RB_MODECONTROL copy mode) from the current registers.
        void Resolve(ID3D12GraphicsCommandList* list, UploadAllocator& upload);

        // Debugging: copies the color target bound at slot 0 (8-bit RGBA formats) into a readback
        // buffer, resolving multisampling. Returns false if there's nothing to capture.
        struct Capture
        {
            ComPtr<ID3D12Resource> readback;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            uint32_t width = 0, height = 0;
            uint32_t base = 0, pitch = 0, samples = 1;
        };
        bool CaptureBoundColor(ID3D12GraphicsCommandList* list, uint32_t maxHeight, Capture& out);

        static DXGI_FORMAT HostColorFormat(xenos::ColorFormat format);

        uint64_t resolves = 0, transfers = 0; // statistics
        // Diagnostics: log each EDRAM transfer (the renderer sets it for DRAW_LOG and LOG_TRANSFERS frames), with the frame.
        bool logTransfers = false;
        uint64_t frameNumber = 0;

        // Draw resolution scale (settings::Graphics): host render targets are this many times the guest's in each
        // direction, and resolves also write scaled copies (ScaledResolves). Targets keep the guest's size in
        // width/height. Set before the first target is created, or Reset after changing it.
        uint32_t scale = 1;
        // MSAA override (settings::Graphics): surfaces the guest multisamples get this many samples on the host (0: the
        // guest's).
        uint32_t msaaSamples = 0;
        // Drops every target, e.g. for another scale or MSAA override (with the GPU idle, outside a render pass). Their
        // contents are lost, as across a frame in DOA4.
        void Reset();

    private:
        Target* GetTarget(uint32_t base, uint32_t pitch, uint32_t samples, uint32_t format, bool depth);
        void Transition(ID3D12GraphicsCommandList* list, Target& target, D3D12_RESOURCE_STATES state);
        // Records render passes on `list` from now on.
        void UseList(ID3D12GraphicsCommandList* list);
        // Makes `target` hold the current contents of the EDRAM tiles under the rectangle (guest pixels), copying in
        // those other targets hold, and the owner of them. Ends the render pass if it copies.
        void Acquire(ID3D12GraphicsCommandList* list, UploadAllocator& upload, Target& target, uint32_t left, uint32_t top, uint32_t right,
            uint32_t bottom);
        // Copies the EDRAM tiles of `mask` (a bit per tile) from `source` into `dest`, outside a render pass.
        void Transfer(ID3D12GraphicsCommandList* list, UploadAllocator& upload, Target& source, Target& dest, const std::array<uint32_t, 64>& mask);
        ID3D12PipelineState* TransferPipeline(const Target& dest, bool sourceMultisampled, int stencilBit = -1);

        ID3D12Device* m_device = nullptr;
        DescriptorHeap* m_views = nullptr;
        SharedMemory* m_sharedMemory = nullptr;
        ScaledResolves* m_scaledResolves = nullptr;
        CpuDescriptorPool m_rtvs, m_dsvs;
        std::unordered_map<uint64_t, std::unique_ptr<Target>> m_targets;

        Bindings m_bound;      // the targets of the current render pass
        bool m_passActive = false;
        ID3D12GraphicsCommandList* m_passList = nullptr;
        ComPtr<ID3D12GraphicsCommandList4> m_passList4;
        D3D12_CPU_DESCRIPTOR_HANDLE m_nullView{}; // for holes between color targets
        std::vector<ComPtr<ID3D12Resource>> m_captureCopies;

        ComPtr<ID3D12RootSignature> m_resolveRootSignature;
        ComPtr<ID3D12PipelineState> m_resolvePipelines[2]; // single-sampled, multisampled source

        // EDRAM ownership: the target holding each tile's current contents (null: none), and a counter bumped whenever
        // a tile changes hands.
        std::array<Target*, 2048> m_owners{};
        uint64_t m_ownerEpoch = 1;
        ComPtr<ID3D12RootSignature> m_transferRootSignature;
        ComPtr<ID3DBlob> m_transferVertexShader;
        ComPtr<ID3DBlob> m_transferPixelShaders[16]; // by variant: multisampled source 1, depth 2, stencil 4, stencil bit 8
        std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> m_transferPipelines;
        // How transfers into depth targets copy the stencil: through SV_StencilRef where pixel shaders can write it, else
        // (NVIDIA's GPUs) by zeroing it and setting it a bit at a time. DEBUG_STENCIL_TRANSFER (diagnostics): 0, not at
        // all (as before the bits); 1, a bit at a time even with SV_StencilRef.
        enum class StencilTransfer { kShader, kBits, kNone };
        StencilTransfer m_stencilTransfer = StencilTransfer::kBits;
        std::vector<uint64_t> m_reportedTransfers; // pairs of targets logged once
    };
}
