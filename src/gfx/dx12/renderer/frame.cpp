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

void Dx12Renderer::begin_frame(const f4f23ma::ma4& view, const f4f23ma::ma4& proj) {
    view_ = view;
    proj_ = proj;
    last_frame_draws_ = 0;
    draw_cb_count_ = 0;
    auto& f = frames_[frame_index_];
    throw_hr(f.allocator->Reset(), "allocator reset");
    throw_hr(f.cmd_list->Reset(f.allocator.Get(), pso_.Get()), "cmd reset");

    D3D12_RESOURCE_BARRIER to_barrier =
        dx12_transition_barrier(f.render_target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    f.cmd_list->ResourceBarrier(1, &to_barrier);

    const float clear[4] = {0.22f, 0.26f, 0.32f, 1.f};
    f.cmd_list->ClearRenderTargetView(f.rtv_cpu, clear, 0, nullptr);
    f.cmd_list->ClearDepthStencilView(dsv_cpu_, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
    f.cmd_list->OMSetRenderTargets(1, &f.rtv_cpu, FALSE, &dsv_cpu_);
    f.cmd_list->SetGraphicsRootSignature(root_signature_.Get());
    f.cmd_list->SetPipelineState(pso_.Get());
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    f.cmd_list->SetDescriptorHeaps(1, heaps);
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.f;
    viewport.TopLeftY = 0.f;
    viewport.Width = static_cast<float>(config_.width);
    viewport.Height = static_cast<float>(config_.height);
    viewport.MinDepth = 0.f;
    viewport.MaxDepth = 1.f;
    D3D12_RECT scissor{0, 0, static_cast<LONG>(config_.width), static_cast<LONG>(config_.height)};
    f.cmd_list->RSSetViewports(1, &viewport);
    f.cmd_list->RSSetScissorRects(1, &scissor);
    f.cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
void Dx12Renderer::end_frame() {
    auto& f = frames_[frame_index_];

    std::filesystem::path shot_path;
    if (!pending_screenshot_path_.empty()) {
        shot_path = pending_screenshot_path_;
        pending_screenshot_path_.clear();
        // Copy while still RENDER_TARGET on this frame's list (before Present).
        enqueue_screenshot_copy(f.cmd_list.Get(), f.render_target.Get());
    }

    D3D12_RESOURCE_BARRIER to_present = dx12_transition_barrier(
        f.render_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    f.cmd_list->ResourceBarrier(1, &to_present);
    throw_hr(f.cmd_list->Close(), "cmd close");
    ID3D12CommandList* lists[] = {f.cmd_list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    swapchain_->Present(config_.vsync ? 1 : 0, 0);
    move_to_next_frame();
    if (!shot_path.empty()) {
        wait_gpu();
        finalize_screenshot_bmp(shot_path);
    }
}
void Dx12Renderer::wait_for_gpu() { wait_gpu(); }

void Dx12Renderer::wait_gpu() {
    const UINT64 v = ++fence_counter_;
    queue_->Signal(fence_.Get(), v);
    if (fence_->GetCompletedValue() < v) {
        fence_->SetEventOnCompletion(v, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

void Dx12Renderer::move_to_next_frame() {
    const UINT64 v = ++fence_counter_;
    queue_->Signal(fence_.Get(), v);
    frames_[frame_index_].fence_value = v;
    frame_index_ = swapchain_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < frames_[frame_index_].fence_value) {
        fence_->SetEventOnCompletion(frames_[frame_index_].fence_value, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

} // namespace omsi::gfx::dx12
