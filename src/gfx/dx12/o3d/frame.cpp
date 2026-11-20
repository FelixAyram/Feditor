#include "gfx/dx12/hello_o3d_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/logger.hpp"
#include "omsi/visual_quality.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/visible_runtime.hpp"
#include "omsi/o3d_format.hpp"
#include "o3d/o3d_loader.hpp"

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <objbase.h>
#include <wrl/client.h>

#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

namespace omsi::gfx::dx12 {

void HelloO3dRenderer::pick_msaa_quality() {
    auto& d = *impl_;
    d.msaa_samples = 1;
    d.msaa_quality = 0;
    if (!d.device) return;
    unsigned want = omsi::visual_quality_msaa_samples();
    if (want <= 1) return;
    const UINT candidates[] = {want, 8u, 4u, 2u};
    for (UINT c : candidates) {
        if (c <= 1) continue;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
        q.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        q.SampleCount = c;
        q.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        if (FAILED(d.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))))
            continue;
        if (q.NumQualityLevels == 0) continue;
        d.msaa_samples = c;
        d.msaa_quality = 0; // standard pattern (MS docs: quality 0 is valid when NumQualityLevels > 0)
        omsi::log::info("MSAA enabled samples=" + std::to_string(d.msaa_samples) + " quality_levels=" +
                        std::to_string(q.NumQualityLevels));
        return;
    }
    omsi::log::warn("MSAA requested but unsupported — falling back to 1x");
}

void HelloO3dRenderer::create_msaa_color_buffer(std::uint32_t width, std::uint32_t height) {
    auto& d = *impl_;
    d.msaa_color.Reset();
    if (!d.device || width == 0 || height == 0 || d.msaa_samples <= 1) return;

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = d.msaa_samples;
    desc.SampleDesc.Quality = d.msaa_quality;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear_value.Color[0] = 0.69f;
    clear_value.Color[1] = 0.77f;
    clear_value.Color[2] = 0.87f;
    clear_value.Color[3] = 1.f;

    throw_hr(d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                                               IID_PPV_ARGS(d.msaa_color.GetAddressOf())),
             "MSAA color RT");

    // RTV slot after backbuffers + scene_color: index = 2 * kFrameCount
    d.msaa_rtv_cpu =
        offset_cpu_handle(d.rtv_heap->GetCPUDescriptorHandleForHeapStart(), rtv_descriptor_size_, kFrameCount * 2u);
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    d.device->CreateRenderTargetView(d.msaa_color.Get(), &rtv, d.msaa_rtv_cpu);
}

void HelloO3dRenderer::create_depth_buffer(std::uint32_t width, std::uint32_t height) {
    auto& d = *impl_;
    d.depth_buffer.Reset();
    if (!d.dsv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        throw_hr(d.device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(d.dsv_heap.GetAddressOf())), "DSV heap");
        d.dsv_cpu = d.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    }

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC depth_desc{};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = (std::max)(1u, d.msaa_samples);
    depth_desc.SampleDesc.Quality = d.msaa_samples > 1 ? d.msaa_quality : 0;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = DXGI_FORMAT_D32_FLOAT;
    clear_value.DepthStencil.Depth = 0.f; // reverse-Z far

    throw_hr(d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
                                               IID_PPV_ARGS(d.depth_buffer.GetAddressOf())),
             "depth buffer");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension =
        d.msaa_samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    d.device->CreateDepthStencilView(d.depth_buffer.Get(), &dsv_desc, d.dsv_cpu);
}

void HelloO3dRenderer::set_world_view_proj(const float world_view_proj_transposed[16]) {
    if (!impl_ || !impl_->cb_ring_cpu) return;
    std::memcpy(&impl_->view_proj_transposed_, world_view_proj_transposed, sizeof(XMFLOAT4X4));
    impl_->has_view_proj_ = true;
}

void HelloO3dRenderer::set_terrain_lod_enabled(bool on) {
    if (impl_) { static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, on ? 2ull : 1ull, "terrain_lod"); }
    if (impl_) impl_->terrain_lod_enabled = on;
}

void HelloO3dRenderer::set_min_screen_size_pct(float pct) {
    if (impl_) { static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(&pct, 1), "min_size"); }
    if (!impl_) return;
    impl_->min_screen_size_pct = pct > 0.f ? (pct < 30.f ? pct : 30.f) : 0.f;
}

void HelloO3dRenderer::set_camera_world_pos(float x, float y, float z) {
    if (!impl_) return;
    impl_->camera_world_pos[0] = x;
    impl_->camera_world_pos[1] = y;
    impl_->camera_world_pos[2] = z;
}

void HelloO3dRenderer::set_cull_max_distance(float metres) {
    if (impl_) { static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(&metres, 1), "cull_dist"); }
    if (!impl_) return;
    impl_->cull_max_distance = metres > 0.f ? metres : 0.f;
}

void HelloO3dRenderer::set_camera_view(float fwd_x, float fwd_y, float fwd_z, float aspect, float fov_y_rad) {
    if (!impl_) return;
    const float len = std::sqrt(fwd_x * fwd_x + fwd_y * fwd_y + fwd_z * fwd_z);
    if (len > 1e-6f) {
        impl_->camera_forward[0] = fwd_x / len;
        impl_->camera_forward[1] = fwd_y / len;
        impl_->camera_forward[2] = fwd_z / len;
    }
    if (aspect > 0.1f) impl_->camera_aspect = aspect;
    if (fov_y_rad > 0.05f) impl_->camera_fov_y = fov_y_rad;
}

void HelloO3dRenderer::set_fog_scale(float scale) {
    if (impl_) impl_->fog_scale = scale < 0.f ? 0.f : scale;
}

void HelloO3dRenderer::set_csm_far_distance(float far_m) {
    if (impl_) { static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(&far_m, 1), "csm_far"); }
    if (!impl_) return;
    // Practical split (NVIDIA): lerp(uniform, logarithmic, λ=0.5), near=0.5, 3 cascades.
    const float n = 0.5f;
    float f = far_m;
    if (f < 40.f) f = 40.f;
    if (f > 2000.f) f = 2000.f;
    // (lambda 0.5 gave the first cascade ~100 m at a 600 m far: 12 cm texels, so poles, posts and the
    //  contact of one object on another cast nothing. Mostly logarithmic now, the first cascade held to
    //  18 m - ~1.5 cm texels where the eye is - and the second to 80 m)
    constexpr float kLambda = 0.9f;
    for (int i = 1; i <= 3; ++i) {
        const float p = static_cast<float>(i) / 3.f;
        const float log_s = n * std::pow(f / n, p);
        const float uni_s = n + (f - n) * p;
        impl_->csm_splits[i - 1] = uni_s * (1.f - kLambda) + log_s * kLambda;
    }
    impl_->csm_splits[0] = (std::clamp)(impl_->csm_splits[0], 8.f, 18.f);
    // (the middle cascade grows with a far shadow distance: held to 80 m, everything past it fell in one
    //  coarse cascade and the shadows looked as if they stopped close to the camera)
    impl_->csm_splits[1] = (std::clamp)(impl_->csm_splits[1], impl_->csm_splits[0] * 2.5f, (std::max)(80.f, 0.15f * f));
    impl_->csm_splits[2] = f; // last cascade ends exactly at far
}

void HelloO3dRenderer::set_tile_cull_cell(float origin_x, float origin_y, float width_m, float height_m) {
    if (impl_) { const float a_[4] = {origin_x, origin_y, width_m, height_m}; static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(a_, 4), "tile_cull_cell"); }
    if (!impl_) return;
    if (width_m < 1.f) width_m = 300.f;
    if (height_m < 1.f) height_m = 300.f;
    impl_->tile_cull_ox = origin_x;
    impl_->tile_cull_oy = origin_y;
    impl_->tile_cull_w = width_m;
    impl_->tile_cull_h = height_m;
}

void HelloO3dRenderer::set_chunk_world_matrices(const std::vector<std::array<float, 16>>& matrices_transposed) {
    if (impl_) scene_dirty(*impl_, "set_chunk_world_matrices");
    if (!impl_) return;
    for (auto& chunk : impl_->mesh_chunks) {
        if (chunk.cfg_mesh_index < matrices_transposed.size()) {
            std::memcpy(&chunk.local_world_transposed, matrices_transposed[chunk.cfg_mesh_index].data(),
                        sizeof(XMFLOAT4X4));
        } else {
            DirectX::XMStoreFloat4x4(&chunk.local_world_transposed, DirectX::XMMatrixIdentity());
        }
    }
}

void HelloO3dRenderer::set_axis_gizmo_matrices(const float view_proj_transposed[16]) {
    if (impl_) { static std::uint64_t s_h_ = 0; if (view_proj_transposed) scene_dirty_if(*impl_, s_h_, scene_hash_bytes(view_proj_transposed, 64), "axis"); }
    if (!impl_ || !impl_->axis_cb_cpu) return;
    std::memcpy(&impl_->axis_cb_cpu->view_proj, view_proj_transposed, sizeof(XMFLOAT4X4));
}

void HelloO3dRenderer::resize(std::uint32_t width, std::uint32_t height) {
    if (impl_) scene_dirty(*impl_, "resize");
    if (!impl_ || !impl_->swapchain || width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    wait_for_previous_frame();
    for (std::uint32_t i = 0; i < kFrameCount; ++i) {
        impl_->render_targets[i].Reset();
        impl_->scene_color[i].Reset();
    }
    impl_->msaa_color.Reset();
    impl_->depth_buffer.Reset();

    throw_hr(impl_->swapchain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN,
                                                 impl_->allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u), "ResizeBuffers");
    frame_index_ = impl_->swapchain->GetCurrentBackBufferIndex();
    width_ = width;
    height_ = height;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t i = 0; i < kFrameCount; ++i) {
        throw_hr(impl_->swapchain->GetBuffer(i, IID_PPV_ARGS(impl_->render_targets[i].GetAddressOf())), "GetBuffer");
        impl_->device->CreateRenderTargetView(impl_->render_targets[i].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += static_cast<SIZE_T>(rtv_descriptor_size_);
    }

    create_msaa_color_buffer(width_, height_);
    create_depth_buffer(width_, height_);
    create_scene_color_buffers(width_, height_);

    impl_->viewport.Width = static_cast<float>(width);
    impl_->viewport.Height = static_cast<float>(height);
    scene_viewport_rect(width_, height_, impl_->viewport, impl_->scissor);
}

void HelloO3dRenderer::set_pending_screenshot(const std::filesystem::path& path) {
    if (impl_) impl_->pending_screenshot_path = path;
}

void HelloO3dRenderer::set_vsync(bool enabled) {
    if (!impl_) return;
    impl_->present_sync_interval = enabled ? 1u : 0u;
    impl_->vsync_explicit = true;
}

bool HelloO3dRenderer::vsync() const {
    return impl_ && impl_->present_sync_interval != 0;
}

} // namespace omsi::gfx::dx12

