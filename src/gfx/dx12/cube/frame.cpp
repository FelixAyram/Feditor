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

void HelloCubeRenderer::set_mvp(const float mvp_column_major[16]) {
    if (!impl_ || !impl_->cb_cpu) return;
    std::memcpy(&impl_->cb_cpu->world_view_proj, mvp_column_major, sizeof(XMFLOAT4X4));
}

void HelloCubeRenderer::resize(std::uint32_t width, std::uint32_t height) {
    if (!impl_ || !impl_->swapchain || width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    wait_for_previous_frame();
    for (std::uint32_t i = 0; i < kFrameCount; ++i) impl_->render_targets[i].Reset();
    impl_->depth_buffer.Reset();

    throw_hr(impl_->swapchain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
    frame_index_ = impl_->swapchain->GetCurrentBackBufferIndex();
    width_ = width;
    height_ = height;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t i = 0; i < kFrameCount; ++i) {
        throw_hr(impl_->swapchain->GetBuffer(i, IID_PPV_ARGS(impl_->render_targets[i].GetAddressOf())), "GetBuffer");
        impl_->device->CreateRenderTargetView(impl_->render_targets[i].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += static_cast<SIZE_T>(rtv_descriptor_size_);
    }

    create_depth_buffer(width_, height_);

    impl_->viewport.Width = static_cast<float>(width_);
    impl_->viewport.Height = static_cast<float>(height_);
    impl_->scissor.right = static_cast<LONG>(width_);
    impl_->scissor.bottom = static_cast<LONG>(height_);
}

void HelloCubeRenderer::render() {
    if (!impl_) return;
    populate_command_list();
    ID3D12CommandList* lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    throw_hr(impl_->swapchain->Present(1, 0), "Present");
    wait_for_previous_frame();
}

void HelloCubeRenderer::populate_command_list() {
    auto& d = *impl_;
    throw_hr(d.command_allocator->Reset(), "allocator reset");
    throw_hr(d.command_list->Reset(d.command_allocator.Get(), d.pipeline_state.Get()), "cmd reset");

    d.command_list->SetGraphicsRootSignature(d.root_signature.Get());
    ID3D12DescriptorHeap* heaps[] = {d.cbv_heap.Get()};
    d.command_list->SetDescriptorHeaps(1, heaps);
    d.command_list->SetGraphicsRootDescriptorTable(0, d.cbv_heap->GetGPUDescriptorHandleForHeapStart());
    d.command_list->RSSetViewports(1, &d.viewport);
    d.command_list->RSSetScissorRects(1, &d.scissor);

    D3D12_RESOURCE_BARRIER barrier = dx12_transition_barrier(
        d.render_targets[frame_index_].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    d.command_list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv_handle.ptr += static_cast<SIZE_T>(frame_index_) * rtv_descriptor_size_;
    d.command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, &d.dsv_cpu);

    const float clear_color[] = {0.69f, 0.77f, 0.87f, 1.f};
    d.command_list->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);
    d.command_list->ClearDepthStencilView(d.dsv_cpu, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
    d.command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.command_list->IASetVertexBuffers(0, 1, &d.vertex_buffer_view);
    d.command_list->IASetIndexBuffer(&d.index_buffer_view);
    d.command_list->DrawIndexedInstanced(d.index_count, 1, 0, 0, 0);

    barrier = dx12_transition_barrier(d.render_targets[frame_index_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_PRESENT);
    d.command_list->ResourceBarrier(1, &barrier);
    throw_hr(d.command_list->Close(), "cmd close");
}

void HelloCubeRenderer::wait_for_previous_frame() {
    if (!impl_) return;
    const UINT64 fence = impl_->fence_value;
    throw_hr(impl_->queue->Signal(impl_->fence.Get(), fence), "Signal");
    ++impl_->fence_value;
    if (impl_->fence->GetCompletedValue() < fence) {
        throw_hr(impl_->fence->SetEventOnCompletion(fence, impl_->fence_event), "SetEventOnCompletion");
        WaitForSingleObject(impl_->fence_event, INFINITE);
    }
    frame_index_ = impl_->swapchain->GetCurrentBackBufferIndex();
}

} // namespace omsi::gfx::dx12
