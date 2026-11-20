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

void Dx12Renderer::set_debug_origin(const f4f23ma::f4& origin) { debug_origin_ = origin; }

void Dx12Renderer::draw_overlay() { draw_ndc_overlay(); }

void Dx12Renderer::bind_and_draw_batch(const Dx12MeshGpu& gpu, const f4f23ma::ma4& object,
                                         const MaterialDrawBatch& batch) {
    if (draw_cb_count_ >= kMaxDrawsPerFrame || batch.index_count == 0) return;

    DrawCB& slot = draw_cb_cpu_[draw_cb_count_];
    slot.view_proj = proj_ * view_;
    slot.object = object;

    auto& f = frames_[frame_index_];
    const auto cb_addr =
        draw_cb_upload_->GetGPUVirtualAddress() + static_cast<UINT64>(draw_cb_count_) * kDrawCbStride;
    f.cmd_list->SetGraphicsRootConstantBufferView(0, cb_addr);
    if (batch.has_texture) f.cmd_list->SetGraphicsRootDescriptorTable(1, batch.srv_gpu);
    else f.cmd_list->SetGraphicsRootDescriptorTable(1, srv_heap_gpu_);
    f.cmd_list->IASetVertexBuffers(0, 1, &gpu.vbv);
    f.cmd_list->IASetIndexBuffer(&gpu.ibv);
    f.cmd_list->DrawIndexedInstanced(batch.index_count, 1, batch.index_start, 0, 0);
    ++draw_cb_count_;
    ++last_frame_draws_;
}
void Dx12Renderer::prepare_mesh(const GpuMesh& mesh, const o3d::MappedO3D& source) {
    (void)mesh;
    const auto& key = source.path().string();
    if (mesh_gpu_cache_.find(key) != mesh_gpu_cache_.end()) return;
    mesh_gpu_cache_.emplace(key, upload_mesh(source));
}

void Dx12Renderer::draw_mesh(const GpuMesh& mesh, const o3d::MappedO3D& source) {
    const auto& key = source.path().string();
    auto it = mesh_gpu_cache_.find(key);
    if (it == mesh_gpu_cache_.end()) return;
    if (!it->second.batches.empty()) {
        for (const auto& batch : it->second.batches) bind_and_draw_batch(it->second, mesh.object_matrix, batch);
        return;
    }
    MaterialDrawBatch whole{};
    whole.index_start = 0;
    whole.index_count = it->second.index_count;
    bind_and_draw_batch(it->second, mesh.object_matrix, whole);
}

void Dx12Renderer::prepare_buffer_mesh(std::string_view key, const o3d::Vertex* vertices,
                                       std::uint32_t vertex_count, const std::uint32_t* indices,
                                       std::uint32_t index_count, const std::filesystem::path& texture_path) {
    const std::string cache_key(key);
    if (mesh_gpu_cache_.find(cache_key) != mesh_gpu_cache_.end()) return;
    auto gpu = upload_buffer(vertices, vertex_count, indices, index_count);
    if (!texture_path.empty() && !gpu.batches.empty()) {
        gpu.batches[0].srv_gpu = load_texture_srv(texture_path);
        gpu.batches[0].has_texture = gpu.batches[0].srv_gpu.ptr != 0;
        if (gpu.batches[0].has_texture) omsi::log::info("texture: " + texture_path.string());
    }
    mesh_gpu_cache_.emplace(cache_key, std::move(gpu));
}

void Dx12Renderer::draw_buffer_mesh(std::string_view key, const f4f23ma::ma4& object) {
    const std::string cache_key(key);
    auto it = mesh_gpu_cache_.find(cache_key);
    if (it == mesh_gpu_cache_.end()) return;
    if (!it->second.batches.empty()) {
        for (const auto& batch : it->second.batches) bind_and_draw_batch(it->second, object, batch);
        return;
    }
    MaterialDrawBatch whole{};
    whole.index_start = 0;
    whole.index_count = it->second.index_count;
    bind_and_draw_batch(it->second, object, whole);
}

} // namespace omsi::gfx::dx12
