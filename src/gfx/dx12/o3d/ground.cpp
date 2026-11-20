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

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <objbase.h>
#include <wrl/client.h>

#include <fstream>
#include <cstring>
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

void HelloO3dRenderer::clear_ground() {
    if (impl_) scene_dirty(*impl_, "clear_ground");
    if (!impl_) return;
    impl_->has_ground_plane = false;
    impl_->ground_chunk = {};
}

bool HelloO3dRenderer::upload_ground_plane(float center_x, float center_y, float z, float half_extent_x,
                                           float half_extent_y) {
    if (impl_) scene_dirty(*impl_, "upload_ground_plane");
    if (!impl_) return false;
    auto& d = *impl_;
    if (!d.device) return false;

    auto make_vertex = [](float x, float y, float vz) {
        omsi::o3d::Vertex v{};
        v.position[0] = x;
        v.position[1] = y;
        v.position[2] = vz;
        v.normal[0] = 0.f;
        v.normal[1] = 0.f;
        v.normal[2] = 1.f;
        v.uv[0] = 0.f;
        v.uv[1] = 0.f;
        return v;
    };

    const float hx = half_extent_x;
    const float hy = half_extent_y;
    const std::vector<omsi::o3d::Vertex> gpu_verts = {
        make_vertex(center_x - hx, center_y - hy, z), make_vertex(center_x + hx, center_y - hy, z),
        make_vertex(center_x + hx, center_y + hy, z), make_vertex(center_x - hx, center_y + hy, z),
    };
    const std::vector<std::uint32_t> flat_indices = {0, 1, 2, 0, 2, 3};

    GpuMeshChunk& chunk = d.ground_chunk;
    chunk = GpuMeshChunk{};
    chunk.meta.visible = true;
    chunk.meta.mesh_file = "__ground_plane__"; chunk.content_mesh = -1;
    chunk.meta.bbox_min[0] = center_x - hx;
    chunk.meta.bbox_min[1] = center_y - hy;
    chunk.meta.bbox_min[2] = z - 0.05f;
    chunk.meta.bbox_max[0] = center_x + hx;
    chunk.meta.bbox_max[1] = center_y + hy;
    chunk.meta.bbox_max[2] = z + 0.05f;
    XMStoreFloat4x4(&chunk.local_world_transposed, DirectX::XMMatrixIdentity());

    const UINT vertex_buffer_size = static_cast<UINT>(gpu_verts.size() * sizeof(omsi::o3d::Vertex));
    const UINT index_buffer_size = static_cast<UINT>(flat_indices.size() * sizeof(std::uint32_t));
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC vb_desc = CD3DX12_RESOURCE_DESC::Buffer(vertex_buffer_size);
    const CD3DX12_RESOURCE_DESC ib_desc = CD3DX12_RESOURCE_DESC::Buffer(index_buffer_size);

    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(chunk.vertex_buffer.GetAddressOf())),
             "ground vb");
    void* mapped = nullptr;
    throw_hr(chunk.vertex_buffer->Map(0, nullptr, &mapped), "ground vb map");
    std::memcpy(mapped, gpu_verts.data(), vertex_buffer_size);
    chunk.vertex_buffer->Unmap(0, nullptr);
    chunk.vertex_buffer_view.BufferLocation = chunk.vertex_buffer->GetGPUVirtualAddress();
    chunk.vertex_buffer_view.StrideInBytes = sizeof(omsi::o3d::Vertex);
    chunk.vertex_buffer_view.SizeInBytes = vertex_buffer_size;

    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &ib_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(chunk.index_buffer.GetAddressOf())),
             "ground ib");
    throw_hr(chunk.index_buffer->Map(0, nullptr, &mapped), "ground ib map");
    std::memcpy(mapped, flat_indices.data(), index_buffer_size);
    chunk.index_buffer->Unmap(0, nullptr);
    chunk.index_buffer_view.BufferLocation = chunk.index_buffer->GetGPUVirtualAddress();
    chunk.index_buffer_view.SizeInBytes = index_buffer_size;
    chunk.index_buffer_view.Format = DXGI_FORMAT_R32_UINT;

    MaterialDrawBatch batch{};
    batch.index_start = 0;
    batch.index_count = static_cast<std::uint32_t>(flat_indices.size());
    batch.has_texture = false;
    chunk.opaque_batches.push_back(batch);

    d.has_ground_plane = true;
    omsi::log::info("ground plane at z=" + std::to_string(z) + " size=" + std::to_string(hx * 2.f) + "x" +
                    std::to_string(hy * 2.f));
    return true;
}

bool HelloO3dRenderer::upload_terrain_mesh(const omsi::o3d::Vertex* vertices, std::uint32_t vertex_count,
                                           const std::uint32_t* indices, std::uint32_t index_count,
                                           const std::filesystem::path& texture_path) {
    if (impl_) scene_dirty(*impl_, "upload_terrain_mesh");
    if (!impl_ || !vertices || !indices || vertex_count == 0 || index_count == 0) return false;
    auto& d = *impl_;
    if (!d.device) return false;

    float bmin[3] = {vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
    float bmax[3] = {bmin[0], bmin[1], bmin[2]};
    for (std::uint32_t i = 1; i < vertex_count; ++i) {
        for (int a = 0; a < 3; ++a) {
            bmin[a] = (std::min)(bmin[a], vertices[i].position[a]);
            bmax[a] = (std::max)(bmax[a], vertices[i].position[a]);
        }
    }

    GpuMeshChunk& chunk = d.ground_chunk;
    chunk = GpuMeshChunk{};
    chunk.meta.visible = true;
    chunk.meta.mesh_file = "__terrain_tile__"; chunk.content_mesh = -1;
    for (int a = 0; a < 3; ++a) {
        chunk.meta.bbox_min[a] = bmin[a];
        chunk.meta.bbox_max[a] = bmax[a];
    }
    XMStoreFloat4x4(&chunk.local_world_transposed, DirectX::XMMatrixIdentity());

    const UINT vertex_buffer_size = static_cast<UINT>(vertex_count * sizeof(omsi::o3d::Vertex));
    const UINT index_buffer_size = static_cast<UINT>(index_count * sizeof(std::uint32_t));
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC vb_desc = CD3DX12_RESOURCE_DESC::Buffer(vertex_buffer_size);
    const CD3DX12_RESOURCE_DESC ib_desc = CD3DX12_RESOURCE_DESC::Buffer(index_buffer_size);

    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(chunk.vertex_buffer.GetAddressOf())),
             "terrain vb");
    void* mapped = nullptr;
    throw_hr(chunk.vertex_buffer->Map(0, nullptr, &mapped), "terrain vb map");
    std::memcpy(mapped, vertices, vertex_buffer_size);
    chunk.vertex_buffer->Unmap(0, nullptr);
    chunk.vertex_buffer_view.BufferLocation = chunk.vertex_buffer->GetGPUVirtualAddress();
    chunk.vertex_buffer_view.StrideInBytes = sizeof(omsi::o3d::Vertex);
    chunk.vertex_buffer_view.SizeInBytes = vertex_buffer_size;

    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &ib_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(chunk.index_buffer.GetAddressOf())),
             "terrain ib");
    throw_hr(chunk.index_buffer->Map(0, nullptr, &mapped), "terrain ib map");
    std::memcpy(mapped, indices, index_buffer_size);
    chunk.index_buffer->Unmap(0, nullptr);
    chunk.index_buffer_view.BufferLocation = chunk.index_buffer->GetGPUVirtualAddress();
    chunk.index_buffer_view.SizeInBytes = index_buffer_size;
    chunk.index_buffer_view.Format = DXGI_FORMAT_R32_UINT;

    MaterialDrawBatch batch{};
    batch.index_start = 0;
    batch.index_count = index_count;
    batch.has_texture = false;

    // Texture optional: .dds then original; miss → white terrain, never abort upload.
    if (!texture_path.empty() && d.srv_next + 2u < kSrvDescriptorCount) {
        const std::uint32_t slot = d.srv_next;
        d.srv_next += 2;
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
        const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
        const D3D12_CPU_DESCRIPTOR_HANDLE white_srv_cpu =
            offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);
        try {
            if (!try_reset_upload_allocator()) {
                d.device->CopyDescriptorsSimple(1, dst_cpu, white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            } else {
            throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "terrain tex cmd reset");
            const auto tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, texture_path);
            if (tex.valid) {
                d.texture_by_slot[slot] = tex.resource;
                if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
                d.texture_slot_files[slot] = texture_path;
                d.device->CopyDescriptorsSimple(
                    1,
                    offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                      kCbvDescriptorCount + slot + 1),
                    white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                throw_hr(d.upload_command_list->Close(), "terrain tex close");
                ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
                d.queue->ExecuteCommandLists(1, lists);
                const UINT64 upload_fence = d.fence_value++;
                throw_hr(d.queue->Signal(d.fence.Get(), upload_fence), "terrain tex Signal");
                d.pending_tex_upload_fence = upload_fence;
                batch.srv_gpu = dst_gpu;
                batch.has_texture = true;
            } else {
                throw_hr(d.upload_command_list->Close(), "terrain tex close empty");
                d.device->CopyDescriptorsSimple(1, dst_cpu, white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                omsi::log::warn("terrain texture load failed: " + texture_path.string());
            }
            }
        } catch (const std::exception& ex) {
            try {
                d.upload_command_list->Close();
            } catch (...) {
            }
            omsi::log::warn(std::string("terrain texture exception: ") + ex.what() + " path=" +
                            texture_path.string());
        } catch (...) {
            try {
                d.upload_command_list->Close();
            } catch (...) {
            }
            omsi::log::warn("terrain texture exception path=" + texture_path.string());
        }
    } else if (!texture_path.empty()) {
        omsi::log::warn("terrain texture skipped (no SRV slots): " + texture_path.string());
    }

    chunk.opaque_batches.push_back(batch);
    d.has_ground_plane = true;
    omsi::log::info("terrain gpu verts=" + std::to_string(vertex_count) + " indices=" + std::to_string(index_count) +
                    " textured=" + std::to_string(batch.has_texture ? 1 : 0) +
                    " path=" + texture_path.string());
    return true;
}

} // namespace omsi::gfx::dx12
