#include "gfx/dx12/hello_cube_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/cube/internal.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace omsi::gfx::dx12 {

bool HelloCubeRenderer::create_device_and_swapchain(HWND__* hwnd) {
    auto& d = *impl_;
    throw_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(d.factory.GetAddressOf())), "CreateDXGIFactory2");

    for (UINT i = 0; d.factory->EnumAdapters1(i, d.adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        d.adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(d.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d.device.GetAddressOf()))))
            break;
    }
    if (!d.device)
        throw_hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d.device.GetAddressOf())),
                 "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    throw_hr(d.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(d.queue.GetAddressOf())), "CreateCommandQueue");

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width_;
    scd.Height = height_;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kFrameCount;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapchain1;
    throw_hr(d.factory->CreateSwapChainForHwnd(d.queue.Get(), hwnd, &scd, nullptr, nullptr,
                                                 swapchain1.GetAddressOf()),
             "CreateSwapChainForHwnd");
    throw_hr(swapchain1.As(&d.swapchain), "SwapChain3");
    d.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    frame_index_ = d.swapchain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = kFrameCount;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    throw_hr(d.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(d.rtv_heap.GetAddressOf())), "RTV heap");
    rtv_descriptor_size_ = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_desc{};
    cbv_heap_desc.NumDescriptors = 1;
    cbv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    throw_hr(d.device->CreateDescriptorHeap(&cbv_heap_desc, IID_PPV_ARGS(d.cbv_heap.GetAddressOf())), "CBV heap");

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t i = 0; i < kFrameCount; ++i) {
        throw_hr(d.swapchain->GetBuffer(i, IID_PPV_ARGS(d.render_targets[i].GetAddressOf())), "GetBuffer");
        d.device->CreateRenderTargetView(d.render_targets[i].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += static_cast<SIZE_T>(rtv_descriptor_size_);
    }

    create_depth_buffer(width_, height_);

    throw_hr(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(d.command_allocator.GetAddressOf())),
             "CreateCommandAllocator");
    throw_hr(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.command_allocator.Get(), nullptr,
                                         IID_PPV_ARGS(d.command_list.GetAddressOf())),
             "CreateCommandList");
    throw_hr(d.command_list->Close(), "Close initial cmd list");

    throw_hr(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(d.fence.GetAddressOf())), "CreateFence");
    d.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!d.fence_event) throw std::runtime_error("CreateEvent failed");

    d.viewport = {0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_), 0.f, 1.f};
    d.scissor = {0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    return true;
}

} // namespace omsi::gfx::dx12
