#include "gfx/dx12/hello_o3d_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/logger.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/visible_runtime.hpp"
#include "omsi/o3d_format.hpp"
#include "o3d/o3d_loader.hpp"
#include "ui/imgui_overlay.hpp"

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
#include <chrono>
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

bool HelloO3dRenderer::create_device_and_swapchain(HWND__* hwnd) {
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

    // COPY queue: geometry/texture uploads without blocking the 3D queue.
    D3D12_COMMAND_QUEUE_DESC copy_qd{};
    copy_qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    throw_hr(d.device->CreateCommandQueue(&copy_qd, IID_PPV_ARGS(d.copy_queue.GetAddressOf())), "CreateCopyQueue");
    throw_hr(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                               IID_PPV_ARGS(d.copy_allocator.GetAddressOf())),
             "CreateCopyAllocator");
    throw_hr(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, d.copy_allocator.Get(), nullptr,
                                         IID_PPV_ARGS(d.copy_command_list.GetAddressOf())),
             "CreateCopyCommandList");
    throw_hr(d.copy_command_list->Close(), "copy list initial close");
    throw_hr(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(d.copy_fence.GetAddressOf())),
             "CreateCopyFence");
    d.copy_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!d.copy_fence_event) throw std::runtime_error("CreateEvent copy fence failed");

    // COMPUTE queue: ready for GPU-driven work later.
    D3D12_COMMAND_QUEUE_DESC comp_qd{};
    comp_qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    throw_hr(d.device->CreateCommandQueue(&comp_qd, IID_PPV_ARGS(d.compute_queue.GetAddressOf())),
             "CreateComputeQueue");
    throw_hr(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                               IID_PPV_ARGS(d.compute_allocator.GetAddressOf())),
             "CreateComputeAllocator");
    throw_hr(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, d.compute_allocator.Get(), nullptr,
                                         IID_PPV_ARGS(d.compute_command_list.GetAddressOf())),
             "CreateComputeCommandList");
    throw_hr(d.compute_command_list->Close(), "compute list initial close");
    throw_hr(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(d.compute_fence.GetAddressOf())),
             "CreateComputeFence");

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width_;
    scd.Height = height_;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kFrameCount;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    {
        BOOL tearing = FALSE;
        if (SUCCEEDED(d.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                     sizeof(tearing))) &&
            tearing)
            d.allow_tearing = true;
    }
    if (d.allow_tearing) scd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    ComPtr<IDXGISwapChain1> swapchain1;
    throw_hr(d.factory->CreateSwapChainForHwnd(d.queue.Get(), hwnd, &scd, nullptr, nullptr,
                                                 swapchain1.GetAddressOf()),
             "CreateSwapChainForHwnd");
    throw_hr(swapchain1.As(&d.swapchain), "SwapChain3");
    d.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    frame_index_ = d.swapchain->GetCurrentBackBufferIndex();
    // Diagnosis for "capped at 60 on a 144 Hz monitor": if tearing is OFF, Present(0, flags=0)
    // gets paced by the OS/DWM. Needed to know whether to fix tearing or reduce GPU cost.
    omsi::log::info(std::string("DX12_SWAPCHAIN buffers=") + std::to_string(scd.BufferCount) +
                    " tearing=" + (d.allow_tearing ? "on" : "OFF") +
                    " present_sync=0 (unlimited; OS may pace if tearing=OFF)");

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    // backbuffers + scene_color + MSAA + SMAA edges + SMAA blend
    rtv_heap_desc.NumDescriptors = kFrameCount * 2u + 3u;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    throw_hr(d.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(d.rtv_heap.GetAddressOf())), "RTV heap");
    rtv_descriptor_size_ = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC shader_heap_desc{};
    shader_heap_desc.NumDescriptors = kHeapDescriptorCount;
    shader_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shader_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    throw_hr(d.device->CreateDescriptorHeap(&shader_heap_desc, IID_PPV_ARGS(d.srv_heap.GetAddressOf())), "SRV heap");
    d.cbv_heap = d.srv_heap;
    d.srv_stride = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d.srv_heap_gpu = offset_gpu_handle(d.srv_heap->GetGPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);

    const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_white =
        offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = 1;
    tex_desc.Height = 1;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    throw_hr(d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(d.white_texture.GetAddressOf())),
             "white tex");

    const std::uint32_t pixel = 0xFFFFFFFFu;
    CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(256);
    ComPtr<ID3D12Resource> white_upload;
    throw_hr(d.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(white_upload.GetAddressOf())),
             "white upload");
    void* mapped = nullptr;
    white_upload->Map(0, nullptr, &mapped);
    std::memset(mapped, 0, 256);
    std::memcpy(mapped, &pixel, sizeof(pixel));
    white_upload->Unmap(0, nullptr);

    for (std::uint32_t i = 0; i < kFrameCount; ++i) {
        throw_hr(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(d.frames[i].allocator.GetAddressOf())),
                 "CreateCommandAllocator");
        throw_hr(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.frames[i].allocator.Get(), nullptr,
                                             IID_PPV_ARGS(d.frames[i].command_list.GetAddressOf())),
                 "CreateCommandList");
        throw_hr(d.frames[i].command_list->Close(), "initial cmd close");
        d.frames[i].fence_value = 0;
    }

    // Per-frame GPU timestamp query/readback. The query heap is shared, but each
    // frame slot owns its 16-byte readback buffer. We only map it after the
    // corresponding frame fence completes, so this adds no GPU/CPU sync to the
    // steady-state render path.
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = HelloO3dRenderer::kFrameCount * HelloO3dRenderer::kGpuStampCount;
        throw_hr(d.device->CreateQueryHeap(&qd, IID_PPV_ARGS(d.timestamp_query_heap.GetAddressOf())),
                 "Create timestamp query heap");
        if (FAILED(d.queue->GetTimestampFrequency(&d.timestamp_frequency)) || d.timestamp_frequency == 0)
            throw std::runtime_error("GetTimestampFrequency failed");
        {
            UINT64 gpu_ts = 0, cpu_ts = 0;
            if (SUCCEEDED(d.queue->GetClockCalibration(&gpu_ts, &cpu_ts))) {
                omsi::log::info("GPU_CLOCK freq=" + std::to_string(d.timestamp_frequency) +
                                " calib_gpu=" + std::to_string(gpu_ts) + " calib_qpc=" + std::to_string(cpu_ts));
            }
        }

        CD3DX12_HEAP_PROPERTIES readback_heap(D3D12_HEAP_TYPE_READBACK);
        const auto readback_desc =
            CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64) * HelloO3dRenderer::kGpuStampCount);
        for (std::uint32_t i = 0; i < kFrameCount; ++i) {
            throw_hr(d.device->CreateCommittedResource(
                         &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                         IID_PPV_ARGS(d.frames[i].timestamp_readback.GetAddressOf())),
                     "Create timestamp readback");
        }
    }

    // Dedicated upload list (texture/mesh offline) — never shares a frame's in-flight allocator.
    throw_hr(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(d.upload_allocator.GetAddressOf())),
             "CreateUploadAllocator");
    throw_hr(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.upload_allocator.Get(), nullptr,
                                         IID_PPV_ARGS(d.upload_command_list.GetAddressOf())),
             "CreateUploadCommandList");
    throw_hr(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(d.fence.GetAddressOf())), "CreateFence");
    d.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!d.fence_event) throw std::runtime_error("CreateEvent failed");

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = d.white_texture.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = white_upload.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = 0;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width = 1;
    src_loc.PlacedFootprint.Footprint.Height = 1;
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = 256;
    d.upload_command_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    D3D12_RESOURCE_BARRIER white_barrier = dx12_transition_barrier(
        d.white_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.upload_command_list->ResourceBarrier(1, &white_barrier);
    throw_hr(d.upload_command_list->Close(), "white cmd close");

    ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 white_fence = d.fence_value++;
    throw_hr(d.queue->Signal(d.fence.Get(), white_fence), "white Signal");
    if (d.fence->GetCompletedValue() < white_fence) {
        throw_hr(d.fence->SetEventOnCompletion(white_fence, d.fence_event), "white SetEventOnCompletion");
        WaitForSingleObject(d.fence_event, INFINITE);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    d.device->CreateShaderResourceView(d.white_texture.Get(), &srv_desc, srv_cpu_white);
    d.srv_next = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t i = 0; i < kFrameCount; ++i) {
        throw_hr(d.swapchain->GetBuffer(i, IID_PPV_ARGS(d.render_targets[i].GetAddressOf())), "GetBuffer");
        d.device->CreateRenderTargetView(d.render_targets[i].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += static_cast<SIZE_T>(rtv_descriptor_size_);
    }

    pick_msaa_quality();
    create_msaa_color_buffer(width_, height_);
    create_depth_buffer(width_, height_);
    create_scene_color_buffers(width_, height_);

    throw_hr(d.upload_allocator->Reset(), "upload allocator reset after white");
    d.tex_slots[0].allocator = d.upload_allocator;
    d.tex_slots[0].list = d.upload_command_list;
    d.tex_slot_cur = 0;

    d.viewport = {0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_), 0.f, 1.f};
    scene_viewport_rect(width_, height_, d.viewport, d.scissor);
    return true;
}
void HelloO3dRenderer::wait_pending_texture_uploads() {
    if (!impl_ || !impl_->fence) return;
    auto& d = *impl_;
    const UINT64 done = d.fence->GetCompletedValue();
    for (std::uint32_t i = 0; i < Impl::kTexUploadSlots; ++i) {
        auto& s = d.tex_slots[i];
        if (i == d.tex_slot_cur || s.fence == 0 || done < s.fence) continue;
        s.fence = 0;
        s.scratch.clear();
    }
    if (d.pending_tex_upload_fence == 0 || done < d.pending_tex_upload_fence) return;
    d.pending_tex_upload_fence = 0;
    d.texture_upload_scratch.clear();
}

// Ring of texture upload lists (like proc_slots for VB/IB on the COPY queue): take any slot whose
// last submit finished, instead of refusing while the single list was in flight. Mesh copies use
// their own allocators, so a busy COPY queue no longer blocks texture uploads.
bool HelloO3dRenderer::try_reset_upload_allocator() {
    if (!impl_ || !impl_->upload_allocator || !impl_->fence || !impl_->device) return false;
    auto& d = *impl_;
    const UINT64 done = d.fence->GetCompletedValue();
    {
        auto& cur = d.tex_slots[d.tex_slot_cur];
        if (!cur.allocator) {
            cur.allocator = d.upload_allocator;
            cur.list = d.upload_command_list;
        }
        if (d.pending_tex_upload_fence != 0) cur.fence = d.pending_tex_upload_fence;
        for (auto& r : d.texture_upload_scratch) cur.scratch.push_back(std::move(r));
        d.texture_upload_scratch.clear();
        d.pending_tex_upload_fence = 0;
    }
    for (std::uint32_t n = 0; n < Impl::kTexUploadSlots; ++n) {
        const std::uint32_t i = (d.tex_slot_cur + n) % Impl::kTexUploadSlots;
        auto& s = d.tex_slots[i];
        if (s.allocator && s.fence != 0 && done < s.fence) continue;
        try {
            if (!s.allocator) {
                throw_hr(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(s.allocator.GetAddressOf())),
                         "tex ring allocator");
                throw_hr(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr,
                                                     IID_PPV_ARGS(s.list.GetAddressOf())),
                         "tex ring list");
                throw_hr(s.list->Close(), "tex ring list close");
            }
        } catch (...) {
            s.allocator.Reset();
            s.list.Reset();
            continue;
        }
        if (FAILED(s.allocator->Reset())) continue;
        s.fence = 0;
        s.scratch.clear();
        d.tex_slot_cur = i;
        d.upload_allocator = s.allocator;
        d.upload_command_list = s.list;
        d.pending_upload_reset = false;
        return true;
    }
    // All slots in flight: restore the current slot's pending fence so waits still see it.
    d.pending_tex_upload_fence = d.tex_slots[d.tex_slot_cur].fence;
    d.pending_upload_reset = true;
    return false;
}

void HelloO3dRenderer::wait_for_previous_frame() {
    if (!impl_ || !impl_->swapchain) return;

    impl_->perf_wait_frame_ms = 0.0;
    wait_pending_texture_uploads();

    // Swapchain back-buffer index = which FrameResource we record into next.
    frame_index_ = impl_->swapchain->GetCurrentBackBufferIndex();

    // Wait only for THIS frame slot's previous submit (kFrameCount frames ago).
    const UINT64 fence_to_wait = impl_->frames[frame_index_].fence_value;
    const auto wait_t0 = std::chrono::steady_clock::now();
    if (fence_to_wait != 0 && impl_->fence->GetCompletedValue() < fence_to_wait) {
        throw_hr(impl_->fence->SetEventOnCompletion(fence_to_wait, impl_->fence_event), "SetEventOnCompletion");
        WaitForSingleObject(impl_->fence_event, INFINITE);
    }
    impl_->perf_wait_frame_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_t0).count();
    // A long wait is evidence about queue pressure, not permission to reuse GPU resources early.
    // Rate-limit diagnostics so logging itself cannot become a frame hitch.
    static std::uint64_t slow_fence_samples = 0;
    ++slow_fence_samples;
    if (impl_->perf_wait_frame_ms >= 8.0 &&
        ((slow_fence_samples % 120u) == 0u || impl_->perf_wait_frame_ms >= 100.0)) {
        const UINT64 completed = impl_->fence->GetCompletedValue();
        omsi::log::info("DX12_FRAME_FENCE_WAIT ms=" + std::to_string(impl_->perf_wait_frame_ms) +
                        " slot=" + std::to_string(frame_index_) +
                        " target=" + std::to_string(fence_to_wait) +
                        " completed=" + std::to_string(completed) +
                        " frames=" + std::to_string(kFrameCount));
    }

    // The fence guarantees that the GPU has written the timestamp readback.
    // Read it now, before this frame slot is reused.
    if (fence_to_wait != 0 && impl_->frames[frame_index_].timestamp_readback && impl_->timestamp_frequency) {
        void* mapped = nullptr;
        const D3D12_RANGE read_range{0, sizeof(UINT64) * HelloO3dRenderer::kGpuStampCount};
        if (SUCCEEDED(impl_->frames[frame_index_].timestamp_readback->Map(0, &read_range, &mapped)) && mapped) {
            const auto* ts = static_cast<const UINT64*>(mapped);
            const double freq = static_cast<double>(impl_->timestamp_frequency);
            auto ms_between = [freq](UINT64 a, UINT64 b) -> double {
                return b >= a && freq > 0.0 ? (1000.0 * static_cast<double>(b - a) / freq) : 0.0;
            };
            const UINT64 t0 = ts[0];
            const UINT64 t1 = ts[1];
            const UINT64 t2 = ts[2];
            impl_->perf_last_gpu_scene_ms = ms_between(t0, t1);
            impl_->perf_last_gpu_ui_ms = ms_between(t1, t2);
            impl_->perf_last_gpu_ms = ms_between(t0, t2);
            impl_->frames[frame_index_].timestamp_readback->Unmap(0, nullptr);
        }
    }
}

// Wait until GPU finished ALL in-flight FrameResources + pending tex uploads.
// Required before Reset(upload_allocator) from offline texture/mesh paths.
void HelloO3dRenderer::wait_gpu_idle_for_upload(const char* reason) {
    if (!impl_ || !impl_->fence) return;
    using SteadyClock = std::chrono::steady_clock;
    const auto total_t0 = SteadyClock::now();
    double flush_ms = 0.0;
    double direct_wait_ms = 0.0;
    double copy_wait_ms = 0.0;

    const auto flush_t0 = SteadyClock::now();
    flush_proc_uploads();
    flush_ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - flush_t0).count();

    auto& d = *impl_;
    if (d.pending_tex_upload_fence != 0 && d.fence->GetCompletedValue() < d.pending_tex_upload_fence) {
        const auto t0 = SteadyClock::now();
        throw_hr(d.fence->SetEventOnCompletion(d.pending_tex_upload_fence, d.fence_event), "tex idle fence");
        WaitForSingleObject(d.fence_event, INFINITE);
        direct_wait_ms += std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
    }
    d.pending_tex_upload_fence = 0;
    d.texture_upload_scratch.clear();
    UINT64 max_fence = 0;
    for (std::uint32_t i = 0; i < kFrameCount; ++i) {
        if (impl_->frames[i].fence_value > max_fence) max_fence = impl_->frames[i].fence_value;
    }
    for (auto& s : d.tex_slots) {
        if (s.fence > max_fence) max_fence = s.fence;
    }
    if (max_fence != 0 && impl_->fence->GetCompletedValue() < max_fence) {
        const auto t0 = SteadyClock::now();
        throw_hr(impl_->fence->SetEventOnCompletion(max_fence, impl_->fence_event), "upload idle fence");
        WaitForSingleObject(impl_->fence_event, INFINITE);
        direct_wait_ms += std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
    }
    // Also drain COPY queue (mesh/texture uploads). This wait is intentional for resource
    // destruction/reuse; keep it safe, but make its cost visible in the perf log.
    if (impl_->copy_fence && impl_->copy_fence_value != 0) {
        if (impl_->copy_fence->GetCompletedValue() < impl_->copy_fence_value) {
            const auto t0 = SteadyClock::now();
            throw_hr(impl_->copy_fence->SetEventOnCompletion(impl_->copy_fence_value, impl_->copy_fence_event),
                     "copy idle fence");
            WaitForSingleObject(impl_->copy_fence_event, INFINITE);
            copy_wait_ms += std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
        }
    }
    for (auto& s : d.tex_slots) {
        s.fence = 0;
        s.scratch.clear();
    }

    const double total_ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - total_t0).count();
    static std::uint64_t slow_idle_samples = 0;
    ++slow_idle_samples;
    if (total_ms >= 8.0 && ((slow_idle_samples % 120u) == 0u || total_ms >= 100.0)) {
        omsi::log::info("DX12_UPLOAD_IDLE_WAIT reason=" + std::string(reason ? reason : "unspecified") +
                        " ms=" + std::to_string(total_ms) +
                        " flush_ms=" + std::to_string(flush_ms) +
                        " direct_wait_ms=" + std::to_string(direct_wait_ms) +
                        " copy_wait_ms=" + std::to_string(copy_wait_ms) +
                        " max_fence=" + std::to_string(max_fence) +
                        " copy_fence=" + std::to_string(impl_->copy_fence_value));
    }
}

bool HelloO3dRenderer::is_gpu_idle() const {
    if (!impl_ || !impl_->fence) return true;
    return impl_->fence->GetCompletedValue() + 1 >= impl_->fence_value;
}

void HelloO3dRenderer::shutdown() {
    if (!impl_) return;
    imgui_overlay_ = nullptr;
    if (impl_->fence && impl_->queue) wait_for_previous_frame();
    if (impl_->constant_buffer && impl_->cb_ring_cpu) {
        impl_->constant_buffer->Unmap(0, nullptr);
        impl_->cb_ring_cpu = nullptr;
    }
    if (impl_->axis_constant_buffer && impl_->axis_cb_cpu) {
        impl_->axis_constant_buffer->Unmap(0, nullptr);
        impl_->axis_cb_cpu = nullptr;
    }
    if (impl_->instance_buffer && impl_->instance_buffer_cpu) {
        impl_->instance_buffer->Unmap(0, nullptr);
        impl_->instance_buffer_cpu = nullptr;
    }
    if (impl_->fence_event) CloseHandle(impl_->fence_event);
    delete impl_;
    impl_ = nullptr;
}

void HelloO3dRenderer::set_imgui_overlay(omsi::ui::ImGuiOverlay* overlay) { imgui_overlay_ = overlay; }
void HelloO3dRenderer::set_ultralight_overlay(omsi::ui::UltralightOverlay* overlay) {
    ultralight_overlay_ = overlay;
}

void HelloO3dRenderer::set_vehicle_coronas(const float* lamps10, int count, float strength) {
    if (!impl_) return;
    auto& v = impl_->vehicle_coronas;
    v.clear();
    if (!lamps10 || count <= 0) return;
    const int n = (std::min)(count, static_cast<int>(kSwCoronaMax));
    v.reserve(static_cast<std::size_t>(n) * 12u);
    for (int i = 0; i < n; ++i) {
        const float* q = lamps10 + static_cast<std::size_t>(i) * 10u;
        const bool directional = (q[3] * q[3] + q[4] * q[4] + q[5] * q[5]) > 1e-4f;
        const float size = (std::max)(q[9], 0.03f);
        v.insert(v.end(), {q[0], q[1], q[2], size, q[3], q[4], q[5], directional ? 1.f : 0.f, q[6], q[7], q[8],
                           strength});
    }
}

void HelloO3dRenderer::set_scene_lights(const SceneLight* lights, int count, float ambient_scale) {
    if (impl_) { static std::uint64_t s_h_ = 0; std::uint64_t h_ = scene_hash_floats_q(&ambient_scale, 1); if (lights && count > 0) h_ = scene_hash_floats_q(reinterpret_cast<const float*>(lights), static_cast<std::size_t>(count) * (sizeof(SceneLight) / sizeof(float)), h_ ^ static_cast<std::uint64_t>(count)); scene_dirty_if(*impl_, s_h_, h_, "lights"); }
    if (!impl_) return;
    auto& d = *impl_;
    d.ambient_scale = ambient_scale > 0.01f ? ambient_scale : 1.f;
    d.scene_light_count = 0;
    // Every light the app hands over (all of the view radius, nearest first, up to 512) goes to the
    // ShadowCB with a 48 x 48 world grid over their reach: each pixel loops only the lights covering its
    // cell. (Was: the nearest 32, each looped by every pixel - lamps further off never lit the street.)
    constexpr int kMaxXLights = 512;
    constexpr int kGrid = 48;
    constexpr std::size_t kBlob = 65536 - 272;
    constexpr std::size_t kLightOff = 16;
    constexpr std::size_t kCellOff = kLightOff + static_cast<std::size_t>(kMaxXLights) * 32;
    constexpr std::size_t kIdxOff = kCellOff + static_cast<std::size_t>(kGrid * kGrid) * 4;
    constexpr std::size_t kIdxCap = (kBlob - kIdxOff) / 2;
    std::uint64_t h = 1469598103934665603ull ^ static_cast<std::uint64_t>(count < 0 ? 0 : count);
    if (lights && count > 0)
        h = scene_hash_floats_q(reinterpret_cast<const float*>(lights),
                                static_cast<std::size_t>(count) * (sizeof(SceneLight) / sizeof(float)), h);
    if (h == d.xlight_hash && !d.xlight_blob.empty()) return; // same lights: grid and CB stay
    d.xlight_hash = h;
    d.xlight_blob.assign(kBlob, 0);
    d.xlight_dirty = true;
    d.xlight_count = 0;
    if (!lights || count <= 0) return;
    const int nx = count > kMaxXLights ? kMaxXLights : count;
    std::uint8_t* blob = d.xlight_blob.data();
    float* lrec = reinterpret_cast<float*>(blob + kLightOff);
    float minx = 1.e30f, miny = 1.e30f, maxx = -1.e30f, maxy = -1.e30f;
    for (int i = 0; i < nx; ++i) {
        float* o = lrec + static_cast<std::size_t>(i) * 8;
        const float core = lights[i].range > 0.5f ? lights[i].range : 0.5f;
        const float inten = lights[i].intensity > 0.f ? lights[i].intensity : 1.f;
        const float cut = core * 6.f; // cut-off (openOMSI: six times the core)
        o[0] = lights[i].x;
        o[1] = lights[i].y;
        o[2] = lights[i].z;
        o[3] = lights[i].interior < -0.5f ? -core : core; // (negative: the own bus's headlight pool, see ps)
        o[4] = lights[i].r * inten;
        o[5] = lights[i].g * inten;
        o[6] = lights[i].b * inten;
        o[7] = lights[i].interior > 0.5f ? -cut : cut; // (negative: an interior-only lamp, see ps)
        minx = (std::min)(minx, lights[i].x - cut);
        miny = (std::min)(miny, lights[i].y - cut);
        maxx = (std::max)(maxx, lights[i].x + cut);
        maxy = (std::max)(maxy, lights[i].y + cut);
    }
    const float cell = (std::max)(8.f, (std::max)(maxx - minx, maxy - miny) / static_cast<float>(kGrid) + 0.01f);
    std::vector<std::uint32_t> cnt(static_cast<std::size_t>(kGrid * kGrid), 0u);
    std::vector<std::array<int, 4>> rect(static_cast<std::size_t>(nx));
    std::size_t total = 0;
    int used = 0;
    auto cell_of = [&](float v, float o) {
        return (std::clamp)(static_cast<int>(std::floor((v - o) / cell)), 0, kGrid - 1);
    };
    for (int i = 0; i < nx; ++i) {
        const float* o = lrec + static_cast<std::size_t>(i) * 8;
        auto& r = rect[static_cast<std::size_t>(i)];
        const float oc = std::fabs(o[7]);
        r = {cell_of(o[0] - oc, minx), cell_of(o[1] - oc, miny), cell_of(o[0] + oc, minx), cell_of(o[1] + oc, miny)};
        const std::size_t c = static_cast<std::size_t>(r[2] - r[0] + 1) * static_cast<std::size_t>(r[3] - r[1] + 1);
        if (total + c > kIdxCap) break; // index list full: the farther lights stay out
        total += c;
        for (int y = r[1]; y <= r[3]; ++y)
            for (int x = r[0]; x <= r[2]; ++x) ++cnt[static_cast<std::size_t>(y * kGrid + x)];
        used = i + 1;
    }
    std::vector<std::uint32_t> fill(cnt.size(), 0u);
    std::uint32_t* cells = reinterpret_cast<std::uint32_t*>(blob + kCellOff);
    std::uint32_t acc = 0;
    for (std::size_t c = 0; c < cnt.size(); ++c) {
        fill[c] = acc;
        cells[c] = (acc << 10) | (std::min)(cnt[c], 1023u);
        acc += cnt[c];
    }
    std::uint16_t* idx = reinterpret_cast<std::uint16_t*>(blob + kIdxOff);
    for (int i = 0; i < used; ++i) {
        const auto& r = rect[static_cast<std::size_t>(i)];
        for (int y = r[1]; y <= r[3]; ++y)
            for (int x = r[0]; x <= r[2]; ++x) idx[fill[static_cast<std::size_t>(y * kGrid + x)]++] = static_cast<std::uint16_t>(i);
    }
    float* info = reinterpret_cast<float*>(blob);
    info[0] = static_cast<float>(used);
    info[1] = minx;
    info[2] = miny;
    info[3] = cell;
    d.xlight_count = used;
    return;
    const int n = count > 4 ? 4 : count;
    for (int i = 0; i < n; ++i) {
        d.scene_lights[i][0] = lights[i].x;
        d.scene_lights[i][1] = lights[i].y;
        d.scene_lights[i][2] = lights[i].z;
        d.scene_lights[i][3] = lights[i].range > 0.5f ? lights[i].range : 8.f;
        d.scene_lights[i][4] = lights[i].r;
        d.scene_lights[i][5] = lights[i].g;
        d.scene_lights[i][6] = lights[i].b;
        d.scene_lights[i][7] = lights[i].intensity > 0.f ? lights[i].intensity : 1.f;
    }
    d.scene_light_count = n;
}

void HelloO3dRenderer::set_sun_lighting(float dir_x, float dir_y, float dir_z, float color_r, float color_g,
                                        float color_b, bool enable_shadows, std::uint32_t shadow_map_size) {
    if (impl_) { const float a_[6] = {dir_x, dir_y, dir_z, color_r, color_g, color_b}; static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(a_, 6) ^ (enable_shadows ? 1ull : 0ull) ^ (static_cast<std::uint64_t>(shadow_map_size) << 8), "sun"); }
    if (!impl_) return;
    auto& d = *impl_;
    float len = std::sqrt(dir_x * dir_x + dir_y * dir_y + dir_z * dir_z);
    if (len < 1e-4f) {
        dir_x = 0.4f;
        dir_y = 0.22f;
        dir_z = 0.88f;
        len = 1.f;
    }
    d.sun_dir[0] = dir_x / len;
    d.sun_dir[1] = dir_y / len;
    d.sun_dir[2] = dir_z / len;
    {
        const float dot = d.sun_dir[0] * d.shadow_sun_dir[0] + d.sun_dir[1] * d.shadow_sun_dir[1] +
                          d.sun_dir[2] * d.shadow_sun_dir[2];
        // (at most every 0.3 s: at x3600 the sun moves 15 degrees a second - all cascades 15 times a second)
        static auto s_last_step = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (dot < 0.99984770f && now - s_last_step > std::chrono::milliseconds(300)) { // (cos 1 degree)
            s_last_step = now;
            for (int k = 0; k < 3; ++k) d.shadow_sun_dir[k] = d.sun_dir[k];
            d.shadow_dir_dirty = true;
        }
    }
    d.sun_color[0] = color_r;
    d.sun_color[1] = color_g;
    d.sun_color[2] = color_b;
    d.sun_shadow_enable = enable_shadows ? 1.f : 0.f;
    if (shadow_map_size >= 512 && shadow_map_size <= 4096) d.sun_shadow_map_size = shadow_map_size;
}

void HelloO3dRenderer::set_clear_color(float r, float g, float b) {
    if (impl_) { const float a_[3] = {r, g, b}; static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(a_, 3), "clear"); }
    if (!impl_) return;
    impl_->clear_rgb[0] = r;
    impl_->clear_rgb[1] = g;
    impl_->clear_rgb[2] = b;
}

void HelloO3dRenderer::set_sky_visuals(float zenith_r, float zenith_g, float zenith_b, float horizon_r,
                                       float horizon_g, float horizon_b, float sun_dir_x, float sun_dir_y,
                                       float sun_dir_z, float sun_r, float sun_g, float sun_b,
                                       float sun_intensity, bool enabled) {
    if (impl_) { const float a_[14] = {zenith_r, zenith_g, zenith_b, horizon_r, horizon_g, horizon_b, sun_dir_x, sun_dir_y, sun_dir_z, sun_r, sun_g, sun_b, sun_intensity, enabled ? 1.f : 0.f}; static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(a_, 14), "sky_visuals"); }
    if (!impl_) return;
    auto& d = *impl_;
    d.sky_enabled = enabled;
    d.sky_zenith[0] = zenith_r;
    d.sky_zenith[1] = zenith_g;
    d.sky_zenith[2] = zenith_b;
    d.sky_horizon[0] = horizon_r;
    d.sky_horizon[1] = horizon_g;
    d.sky_horizon[2] = horizon_b;
    float len = std::sqrt(sun_dir_x * sun_dir_x + sun_dir_y * sun_dir_y + sun_dir_z * sun_dir_z);
    if (len < 1e-4f) {
        sun_dir_x = 0.4f;
        sun_dir_y = 0.22f;
        sun_dir_z = 0.88f;
        len = 1.f;
    }
    d.sky_sun_dir[0] = sun_dir_x / len;
    d.sky_sun_dir[1] = sun_dir_y / len;
    d.sky_sun_dir[2] = sun_dir_z / len;
    d.sky_sun_color[0] = sun_r;
    d.sky_sun_color[1] = sun_g;
    d.sky_sun_color[2] = sun_b;
    d.sky_sun_intensity = sun_intensity;
}

ID3D12Device* HelloO3dRenderer::dx12_device() const {
    return impl_ ? impl_->device.Get() : nullptr;
}

ID3D12CommandQueue* HelloO3dRenderer::dx12_command_queue() const {
    return impl_ ? impl_->queue.Get() : nullptr;
}

} // namespace omsi::gfx::dx12
