#include "gfx/dx12/dx12_renderer.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/o3d_format.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

namespace omsi::gfx::dx12 {

bool Dx12Renderer::create_device() {
    UINT flags = 0;
#if defined(_DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    throw_hr(CreateDXGIFactory2(flags, IID_PPV_ARGS(factory_.GetAddressOf())), "CreateDXGIFactory2");

    for (UINT i = 0; factory_->EnumAdapters1(i, adapter_.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter_->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device_.GetAddressOf()))))
            break;
    }
    if (!device_) throw_hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device_.GetAddressOf())),
                           "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    throw_hr(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.GetAddressOf())), "CreateCommandQueue");

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = kRtvCount;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    throw_hr(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap_.GetAddressOf())), "RTV heap");
    rtv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_desc.NumDescriptors = 1;
    throw_hr(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(dsv_heap_.GetAddressOf())), "DSV heap");
    dsv_cpu_ = dsv_heap_->GetCPUDescriptorHandleForHeapStart();

    CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC draw_cb_desc =
        CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(kDrawCbStride) * kMaxDrawsPerFrame);
    throw_hr(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &draw_cb_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(draw_cb_upload_.GetAddressOf())),
             "draw CB ring");
    draw_cb_upload_->Map(0, nullptr, reinterpret_cast<void**>(&draw_cb_cpu_));

    throw_hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf())), "CreateFence");
    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return true;
}

} // namespace omsi::gfx::dx12
