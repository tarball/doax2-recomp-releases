#include "gpu/d3d12/device.h"

#include "log.h"

#include <cstring>
#include <string>

#include <d3dcompiler.h>

namespace gpu::d3d12
{
    bool Device::Initialize(bool debug)
    {
        UINT factoryFlags = 0;
        if (debug)
        {
            ComPtr<ID3D12Debug> debugInterface;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface))))
            {
                debugInterface->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                LOG_INFO("D3D12: debug layer enabled");
            }
        }

        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory))))
        {
            LOG_ERROR("D3D12: CreateDXGIFactory2 failed");
            return false;
        }

        // The first hardware adapter that supports D3D12 (feature level 11.0+), preferring high performance.
        ComPtr<IDXGIFactory6> factory6;
        m_factory.As(&factory6);
        for (UINT index = 0;; index++)
        {
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT result = factory6 ? factory6->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
                                      : m_factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 description;
            adapter->GetDesc1(&description);
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
            {
                char name[128];
                WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name), nullptr, nullptr);
                LOG_INFO("D3D12: using %s (vendor %04X)", name, description.VendorId);
                m_vendorId = description.VendorId;
                break;
            }
        }
        if (!m_device)
        {
            LOG_ERROR("D3D12: no hardware adapter supports D3D12");
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(m_device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_queue))) ||
            FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
        {
            LOG_ERROR("D3D12: failed to create the queue or fence");
            return false;
        }
        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    uint64_t Device::Signal()
    {
        m_queue->Signal(m_fence.Get(), ++m_fenceValue);
        return m_fenceValue;
    }

    void Device::WaitFor(uint64_t value)
    {
        if (m_fence->GetCompletedValue() >= value)
            return;
        m_fence->SetEventOnCompletion(value, m_fenceEvent);
        if (WaitForSingleObject(m_fenceEvent, 5000) == WAIT_TIMEOUT)
        {
            LOG_ERROR("D3D12: GPU work hasn't finished after 5 seconds (fence %llu, completed %llu, device removed reason 0x%08lX)",
                (unsigned long long)value, (unsigned long long)m_fence->GetCompletedValue(), m_device->GetDeviceRemovedReason());
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    ComPtr<ID3DBlob> CompileShader(const char* source, const char* entryPoint, const char* target, const char* name)
    {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT result = D3DCompile(source, strlen(source), name, nullptr, nullptr, entryPoint, target,
            D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &code, &errors);
        if (FAILED(result))
        {
            LOG_ERROR("Shader %s failed to compile: %s", name, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
            return nullptr;
        }
        return code;
    }
}
