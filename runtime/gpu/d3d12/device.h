#pragma once

// D3D12 device, direct queue and fence, shared by the presenter and the renderer.
#include <cstdint>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace gpu::d3d12
{
    using Microsoft::WRL::ComPtr;

    class Device
    {
    public:
        // `debug` enables the D3D12 debug layer (D3D12_DEBUG=1).
        bool Initialize(bool debug);

        ID3D12Device* Get() const { return m_device.Get(); }
        IDXGIFactory4* Factory() const { return m_factory.Get(); }
        ID3D12CommandQueue* Queue() const { return m_queue.Get(); }
        // Vendor ID of the adapter (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel; Qualcomm reports 'QCOM', 0x4D4F4351).
        uint32_t VendorId() const { return m_vendorId; }

        // Signals the queue and returns the fence value to wait for.
        uint64_t Signal();
        void WaitFor(uint64_t value);
        bool IsComplete(uint64_t value) const { return m_fence->GetCompletedValue() >= value; }
        void WaitIdle() { WaitFor(Signal()); }

    private:
        ComPtr<IDXGIFactory4> m_factory;
        ComPtr<ID3D12Device> m_device;
        ComPtr<ID3D12CommandQueue> m_queue;
        ComPtr<ID3D12Fence> m_fence;
        uint64_t m_fenceValue = 0;
        HANDLE m_fenceEvent = nullptr;
        uint32_t m_vendorId = 0;
    };

    // Compiles HLSL with d3dcompiler (shader model 5.1). Logs errors and returns null on failure.
    ComPtr<ID3DBlob> CompileShader(const char* source, const char* entryPoint, const char* target, const char* name);
}
