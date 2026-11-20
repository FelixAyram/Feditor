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

bool Dx12Renderer::create_swapchain(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = config_.width;
    scd.Height = config_.height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kFrameCount;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> sc1;
    throw_hr(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &scd, nullptr, nullptr, sc1.GetAddressOf()),
             "CreateSwapChainForHwnd");
    throw_hr(sc1.As(&swapchain_), "SwapChain3");
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    frame_index_ = swapchain_->GetCurrentBackBufferIndex();
    return true;
}

bool Dx12Renderer::create_render_targets() {
    frames_.resize(kFrameCount);
    const auto rtv_base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t i = 0; i < kFrameCount; ++i) {
        auto& f = frames_[i];
        throw_hr(swapchain_->GetBuffer(i, IID_PPV_ARGS(f.render_target.GetAddressOf())), "GetBuffer");
        f.rtv_cpu = offset_cpu_handle(rtv_base, rtv_stride_, i);
        device_->CreateRenderTargetView(f.render_target.Get(), nullptr, f.rtv_cpu);
        throw_hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(f.allocator.GetAddressOf())),
                 "CreateCommandAllocator");
    }
    return true;
}
void Dx12Renderer::resize(std::uint32_t width, std::uint32_t height) {
    if (!swapchain_ || (width == 0 || height == 0)) return;
    if (width == config_.width && height == config_.height) return;
    wait_gpu();
    for (auto& f : frames_) f.render_target.Reset();
    depth_buffer_.Reset();
    throw_hr(swapchain_->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
    frame_index_ = swapchain_->GetCurrentBackBufferIndex();
    create_render_targets();
    config_.width = width;
    config_.height = height;
    create_depth_buffer();
}

} // namespace omsi::gfx::dx12
