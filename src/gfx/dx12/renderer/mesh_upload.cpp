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

Dx12MeshGpu Dx12Renderer::upload_mesh(const o3d::MappedO3D& source) {
    const auto& mesh = source.mesh();
    Dx12MeshGpu gpu{};

    std::map<std::uint16_t, std::vector<std::uint32_t>> indices_by_material;
    if (mesh.long_triangle_indices) {
        const auto tris = mesh.triangle_u32_span();
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            auto& bucket = indices_by_material[tris[t].material];
            bucket.push_back(tris[t].i0);
            bucket.push_back(tris[t].i1);
            bucket.push_back(tris[t].i2);
        }
    } else {
        const auto tris = mesh.triangle_u16_span();
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            auto& bucket = indices_by_material[tris[t].material];
            bucket.push_back(tris[t].i0);
            bucket.push_back(tris[t].i1);
            bucket.push_back(tris[t].i2);
        }
    }

    std::vector<std::uint32_t> flat_indices;
    flat_indices.reserve(static_cast<std::size_t>(mesh.triangle_count) * 3u);
    const auto materials = mesh.parse_materials();
    const auto& base_file = source.path(); // Blender find_image_path base (.o3d)

    for (const auto& [material_id, tri_indices] : indices_by_material) {
        if (tri_indices.empty()) continue;
        MaterialDrawBatch batch{};
        batch.index_start = static_cast<std::uint32_t>(flat_indices.size());
        flat_indices.insert(flat_indices.end(), tri_indices.begin(), tri_indices.end());
        batch.index_count = static_cast<std::uint32_t>(flat_indices.size()) - batch.index_start;

        if (material_id < materials.size() && !materials[material_id].texture.empty()) {
            if (const auto tex_path = resolve_texture_path(base_file, materials[material_id].texture)) {
                batch.srv_gpu = load_texture_srv(*tex_path);
                batch.has_texture = batch.srv_gpu.ptr != 0;
                if (batch.has_texture) omsi::log::info("texture: " + tex_path->string());
            } else {
                omsi::log::warn("texture not found: " + materials[material_id].texture);
            }
        }
        gpu.batches.push_back(batch);
    }

    if (gpu.batches.empty() && mesh.triangle_count > 0) {
        MaterialDrawBatch batch{};
        batch.index_start = 0;
        gpu.batches.push_back(batch);
        if (mesh.long_triangle_indices) {
            const auto tris = mesh.triangle_u32_span();
            for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
                flat_indices.push_back(tris[t].i0);
                flat_indices.push_back(tris[t].i1);
                flat_indices.push_back(tris[t].i2);
            }
        } else {
            const auto tris = mesh.triangle_u16_span();
            for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
                flat_indices.push_back(tris[t].i0);
                flat_indices.push_back(tris[t].i1);
                flat_indices.push_back(tris[t].i2);
            }
        }
        gpu.batches.back().index_count = static_cast<std::uint32_t>(flat_indices.size());
    }

    const auto src_verts = mesh.vertex_span();

    const std::size_t vb_size = src_verts.size_bytes();
    const std::size_t ib_size = flat_indices.size() * sizeof(std::uint32_t);

    ComPtr<ID3D12Resource> upload;
    CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(vb_size + ib_size);
    throw_hr(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(upload.GetAddressOf())),
             "upload heap");

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    std::memcpy(mapped, src_verts.data(), vb_size);
    std::memcpy(static_cast<std::uint8_t*>(mapped) + vb_size, flat_indices.data(), ib_size);
    upload->Unmap(0, nullptr);

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC vb_desc = CD3DX12_RESOURCE_DESC::Buffer(vb_size);
    throw_hr(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(gpu.vertex_buffer.GetAddressOf())),
             "vb default");
    CD3DX12_RESOURCE_DESC ib_desc = CD3DX12_RESOURCE_DESC::Buffer(ib_size);
    throw_hr(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &ib_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(gpu.index_buffer.GetAddressOf())),
             "ib default");

    auto& f = frames_[frame_index_];
    f.cmd_list->CopyBufferRegion(gpu.vertex_buffer.Get(), 0, upload.Get(), 0, vb_size);
    f.cmd_list->CopyBufferRegion(gpu.index_buffer.Get(), 0, upload.Get(), vb_size, ib_size);

    D3D12_RESOURCE_BARRIER vb_barrier = dx12_transition_barrier(
        gpu.vertex_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    D3D12_RESOURCE_BARRIER ib_barrier = dx12_transition_barrier(
        gpu.index_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    D3D12_RESOURCE_BARRIER barriers[] = {vb_barrier, ib_barrier};
    f.cmd_list->ResourceBarrier(2, barriers);

    gpu.vbv.BufferLocation = gpu.vertex_buffer->GetGPUVirtualAddress();
    gpu.vbv.SizeInBytes = static_cast<UINT>(vb_size);
    gpu.vbv.StrideInBytes = sizeof(o3d::Vertex);

    gpu.ibv.BufferLocation = gpu.index_buffer->GetGPUVirtualAddress();
    gpu.ibv.SizeInBytes = static_cast<UINT>(ib_size);
    gpu.ibv.Format = DXGI_FORMAT_R32_UINT;
    gpu.index_count = static_cast<std::uint32_t>(flat_indices.size());
    return gpu;
}

Dx12MeshGpu Dx12Renderer::upload_buffer(const o3d::Vertex* vertices, std::uint32_t vertex_count,
                                        const std::uint32_t* indices, std::uint32_t index_count) {
    Dx12MeshGpu gpu{};
    MaterialDrawBatch batch{};
    batch.index_start = 0;
    batch.index_count = index_count;
    gpu.batches.push_back(batch);

    const std::size_t vb_size = static_cast<std::size_t>(vertex_count) * sizeof(o3d::Vertex);
    const std::size_t ib_size = static_cast<std::size_t>(index_count) * sizeof(std::uint32_t);

    ComPtr<ID3D12Resource> upload;
    CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(vb_size + ib_size);
    throw_hr(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(upload.GetAddressOf())),
             "upload heap");

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    std::memcpy(mapped, vertices, vb_size);
    std::memcpy(static_cast<std::uint8_t*>(mapped) + vb_size, indices, ib_size);
    upload->Unmap(0, nullptr);

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC vb_desc = CD3DX12_RESOURCE_DESC::Buffer(vb_size);
    throw_hr(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(gpu.vertex_buffer.GetAddressOf())),
             "vb default");
    CD3DX12_RESOURCE_DESC ib_desc = CD3DX12_RESOURCE_DESC::Buffer(ib_size);
    throw_hr(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &ib_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(gpu.index_buffer.GetAddressOf())),
             "ib default");

    auto& f = frames_[frame_index_];
    f.cmd_list->CopyBufferRegion(gpu.vertex_buffer.Get(), 0, upload.Get(), 0, vb_size);
    f.cmd_list->CopyBufferRegion(gpu.index_buffer.Get(), 0, upload.Get(), vb_size, ib_size);

    D3D12_RESOURCE_BARRIER vb_barrier = dx12_transition_barrier(
        gpu.vertex_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    D3D12_RESOURCE_BARRIER ib_barrier = dx12_transition_barrier(
        gpu.index_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    D3D12_RESOURCE_BARRIER barriers[] = {vb_barrier, ib_barrier};
    f.cmd_list->ResourceBarrier(2, barriers);

    gpu.vbv.BufferLocation = gpu.vertex_buffer->GetGPUVirtualAddress();
    gpu.vbv.SizeInBytes = static_cast<UINT>(vb_size);
    gpu.vbv.StrideInBytes = sizeof(o3d::Vertex);
    gpu.ibv.BufferLocation = gpu.index_buffer->GetGPUVirtualAddress();
    gpu.ibv.SizeInBytes = static_cast<UINT>(ib_size);
    gpu.ibv.Format = DXGI_FORMAT_R32_UINT;
    gpu.index_count = index_count;
    return gpu;
}

} // namespace omsi::gfx::dx12
