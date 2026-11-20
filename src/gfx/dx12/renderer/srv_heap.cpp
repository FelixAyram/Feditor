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

bool Dx12Renderer::create_srv_heap() {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = kMaxSrvDescriptors;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    throw_hr(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(srv_heap_.GetAddressOf())), "SRV heap");
    srv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv_heap_gpu_ = srv_heap_->GetGPUDescriptorHandleForHeapStart();

    // Slot 0: 1x1 white fallback for untextured draws.
    const auto cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = 1;
    tex_desc.Height = 1;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> white_tex;
    throw_hr(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(white_tex.GetAddressOf())),
             "white tex");

    const std::uint32_t pixel = 0xFFFFFFFFu;
    CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(256);
    ComPtr<ID3D12Resource> upload;
    throw_hr(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(upload.GetAddressOf())),
             "white upload");
    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    std::memset(mapped, 0, 256);
    std::memcpy(mapped, &pixel, sizeof(pixel));
    upload->Unmap(0, nullptr);

    auto& f = frames_[0];
    throw_hr(f.allocator->Reset(), "white alloc reset");
    throw_hr(f.cmd_list->Reset(f.allocator.Get(), nullptr), "white cmd reset");

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = white_tex.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = upload.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = 0;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width = 1;
    src_loc.PlacedFootprint.Footprint.Height = 1;
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = 256;
    f.cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    D3D12_RESOURCE_BARRIER barrier = dx12_transition_barrier(white_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    f.cmd_list->ResourceBarrier(1, &barrier);
    throw_hr(f.cmd_list->Close(), "white cmd close");
    ID3D12CommandList* lists[] = {f.cmd_list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_gpu();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(white_tex.Get(), &srv_desc, cpu);
    default_white_texture_ = white_tex;

    srv_next_ = 1;
    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE Dx12Renderer::load_texture_srv(const std::filesystem::path& path) {
    const auto key = path.string();
    if (const auto it = texture_srv_cache_.find(key); it != texture_srv_cache_.end()) return it->second;
    if (srv_next_ >= kMaxSrvDescriptors) return {};

    const auto cpu = offset_cpu_handle(srv_heap_->GetCPUDescriptorHandleForHeapStart(), srv_stride_, srv_next_);
    const auto gpu = offset_gpu_handle(srv_heap_gpu_, srv_stride_, srv_next_);
    ++srv_next_;

    auto& f = frames_[frame_index_];
    const auto tex = load_texture_file(device_.Get(), f.cmd_list.Get(), cpu, gpu, path);
    if (!tex.valid) return {};

    texture_resources_.push_back(tex.resource);
    if (tex.upload_staging) texture_staging_resources_.push_back(tex.upload_staging);
    texture_srv_cache_[key] = gpu;
    return gpu;
}

} // namespace omsi::gfx::dx12
