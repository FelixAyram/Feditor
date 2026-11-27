#include "gfx/dx12/hello_o3d_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/texture/internal.hpp"
#include "omsi/job_system.hpp"
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
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

namespace omsi::gfx::dx12 {

void HelloO3dRenderer::clear_meshes() {
    if (impl_) scene_dirty(*impl_, "clear_meshes");
    if (!impl_) return;
    wait_gpu_idle_for_upload("clear_meshes");
    impl_->mesh_chunks.clear();
    impl_->highlighted_chunks.clear();
    // Free GPU textures + reclaim SRV slots so stream reload does not keep growing/whitening.
    impl_->texture_resources.clear();
    impl_->texture_cache.clear();
    impl_->texture_slot_cache.clear();
    impl_->srv_next = 1;
    impl_->srv_exhausted_logged = false;
}

void HelloO3dRenderer::set_chunk_world_matrix(std::size_t chunk_index, const float world_transposed[16]) {
    if (!impl_ || !world_transposed || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& w = impl_->mesh_chunks[chunk_index].local_world_transposed;
    if (std::memcmp(&w, world_transposed, sizeof(XMFLOAT4X4)) == 0) return;
    std::memcpy(&w, world_transposed, sizeof(XMFLOAT4X4));
    scene_dirty(*impl_, "world_matrix");
}

void HelloO3dRenderer::translate_chunk(std::size_t chunk_index, float dx, float dy, float dz) {
    if (impl_) scene_dirty(*impl_, "translate_chunk");
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    const DirectX::XMMATRIX wt = DirectX::XMLoadFloat4x4(&chunk.local_world_transposed);
    const DirectX::XMMATRIX w = DirectX::XMMatrixTranspose(wt);
    const DirectX::XMMATRIX shifted = w * DirectX::XMMatrixTranslation(dx, dy, dz);
    DirectX::XMStoreFloat4x4(&chunk.local_world_transposed, DirectX::XMMatrixTranspose(shifted));
}

void HelloO3dRenderer::set_chunk_split_blend(std::size_t chunk_index, bool on) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    impl_->mesh_chunks[chunk_index].split_blend = on;
    scene_dirty(*impl_, "split_blend");
}

void HelloO3dRenderer::set_chunk_double_sided(std::size_t chunk_index, bool on) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& c = impl_->mesh_chunks[chunk_index];
    for (auto* list : {&c.opaque_batches, &c.alpha_batches, &c.alpha_nozwrite_batches})
        for (auto& b : *list) b.double_sided = on;
    scene_dirty(*impl_, "double_sided");
}

bool HelloO3dRenderer::update_chunk_skin(std::size_t chunk_index, const float* pos_nrm6, std::uint32_t count) {
    if (!impl_ || !pos_nrm6 || chunk_index >= impl_->mesh_chunks.size()) return false;
    auto& d = *impl_;
    auto& c = d.mesh_chunks[chunk_index];
    if (!c.skin_base || c.skin_base->size() != count || count == 0 || !d.device) return false;
    if (!c.skin) c.skin = std::make_shared<GpuMeshChunk::SkinRing>();
    auto& sk = *c.skin;
    const auto& base = *c.skin_base;
    if (sk.mode < 0) {
        // which frame openOMSI's vertices are in against ours (it swaps the file's y and z): the one closer to the
        // vertices as uploaded, taken while the bellows are near their modelled pose
        double e0 = 0.0, e1 = 0.0;
        for (std::uint32_t v = 0; v < count; ++v) {
            const float* q = pos_nrm6 + static_cast<std::size_t>(v) * 6u;
            const auto& b = base[v].position;
            e0 += std::fabs(q[0] - b[0]) + std::fabs(q[1] - b[1]) + std::fabs(q[2] - b[2]);
            e1 += std::fabs(q[0] - b[0]) + std::fabs(q[2] - b[1]) + std::fabs(q[1] - b[2]);
        }
        sk.mode = e1 < e0 ? 1 : 0;
    }
    const std::size_t nf = static_cast<std::size_t>(count) * 6u;
    if (sk.last.size() == nf && std::memcmp(sk.last.data(), pos_nrm6, nf * sizeof(float)) == 0) return true; // same pose
    sk.last.assign(pos_nrm6, pos_nrm6 + nf);
    const UINT bytes = count * static_cast<UINT>(sizeof(omsi::o3d::Vertex));
    const std::uint32_t slot = sk.next++ % 4u; // (4 > frames in flight: never written while the GPU reads it)
    if (!sk.buf[slot]) {
        const CD3DX12_HEAP_PROPERTIES up(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        if (FAILED(d.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr, IID_PPV_ARGS(sk.buf[slot].GetAddressOf()))))
            return false;
        void* mp = nullptr;
        if (FAILED(sk.buf[slot]->Map(0, nullptr, &mp))) return false;
        sk.map[slot] = static_cast<std::uint8_t*>(mp);
    }
    auto* dst = reinterpret_cast<omsi::o3d::Vertex*>(sk.map[slot]);
    for (std::uint32_t v = 0; v < count; ++v) {
        const float* q = pos_nrm6 + static_cast<std::size_t>(v) * 6u;
        omsi::o3d::Vertex o = base[v];
        if (sk.mode == 1) {
            o.position[0] = q[0], o.position[1] = q[2], o.position[2] = q[1];
            o.normal[0] = q[3], o.normal[1] = q[5], o.normal[2] = q[4];
        } else {
            o.position[0] = q[0], o.position[1] = q[1], o.position[2] = q[2];
            o.normal[0] = q[3], o.normal[1] = q[4], o.normal[2] = q[5];
        }
        dst[v] = o;
    }
    c.vertex_buffer = sk.buf[slot];
    c.vertex_buffer_view.BufferLocation = sk.buf[slot]->GetGPUVirtualAddress();
    c.vertex_buffer_view.StrideInBytes = sizeof(omsi::o3d::Vertex);
    c.vertex_buffer_view.SizeInBytes = bytes;
    scene_dirty(d, "skin");
    return true;
}

void HelloO3dRenderer::set_chunk_vehicle_self(std::size_t chunk_index, bool on) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    if (impl_->mesh_chunks[chunk_index].vehicle_self == on) return;
    impl_->mesh_chunks[chunk_index].vehicle_self = on;
    scene_dirty(*impl_, "vehicle_self");
}

void HelloO3dRenderer::set_chunk_wind_sway(std::size_t chunk_index, bool on) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    if (impl_->mesh_chunks[chunk_index].wind_sway == on) return;
    impl_->mesh_chunks[chunk_index].wind_sway = on;
    scene_dirty(*impl_, "wind_sway");
}

void HelloO3dRenderer::set_scene_night_wind(float night_blend, float wind_x_ms, float wind_y_ms) {
    if (impl_) { const float a_[3] = {night_blend, wind_x_ms, wind_y_ms}; static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(a_, 3), "night_wind"); }
    if (!impl_) return;
    impl_->night_blend = night_blend;
    impl_->wind_x = wind_x_ms;
    impl_->wind_y = wind_y_ms;
}

void HelloO3dRenderer::set_chunk_editor_helper(std::size_t chunk_index, std::uint8_t kinds) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    if (chunk.editor_helper == kinds) return;
    const bool want = chunk.meta.visible || (chunk.editor_helper && chunk.want_visible);
    chunk.editor_helper = kinds;
    set_chunk_visible(chunk_index, want);
}

std::uint8_t HelloO3dRenderer::chunk_editor_helper(std::size_t chunk_index) const {
    return impl_ && chunk_index < impl_->mesh_chunks.size() ? impl_->mesh_chunks[chunk_index].editor_helper : 0;
}

void HelloO3dRenderer::set_editor_helper_mask(std::uint8_t shown_kinds) {
    if (!impl_ || impl_->editor_helper_mask == shown_kinds) return;
    impl_->editor_helper_mask = shown_kinds;
    for (std::size_t i = 0; i < impl_->mesh_chunks.size(); ++i)
        if (impl_->mesh_chunks[i].editor_helper) set_chunk_visible(i, impl_->mesh_chunks[i].want_visible);
    scene_dirty(*impl_, "editor_helpers");
}

void HelloO3dRenderer::set_chunk_visible(std::size_t chunk_index, bool visible) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    chunk.want_visible = visible;
    if (chunk.editor_helper & ~impl_->editor_helper_mask) visible = false;
    if (chunk.meta.visible == visible) return;
    // Netted per frame (populate): editor passes hide and show the same chunk within one frame.
    if (impl_->vis_touched.insert(chunk_index).second) impl_->vis_before.emplace_back(chunk_index, !visible);
    chunk.meta.visible = visible;
}

void HelloO3dRenderer::set_chunk_terrain(std::size_t chunk_index, bool on) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    bool changed = chunk.is_terrain != on;
    chunk.is_terrain = on; // survives mesh replacements (see replace_textured_mesh)
    if (on) {
        if (chunk.meta.mesh_file != "__oo_terrain__") changed = true;
        chunk.meta.mesh_file = "__oo_terrain__"; chunk.content_mesh = -1;
    } else if (chunk.meta.mesh_file == "__oo_terrain__") {
        chunk.meta.mesh_file.clear();
    }
    if (changed) scene_dirty(*impl_, "set_chunk_terrain");
}

void HelloO3dRenderer::set_chunk_lod_capable(std::size_t chunk_index, bool capable) {
    if (impl_) scene_dirty(*impl_, "set_chunk_lod_capable");
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    chunk.lod_capable = capable;
    if (!capable) {
        // Drop the cluster LODs requested so far: the chunk is drawn at full detail from now on.
        for (auto& lod : chunk.lod_ib) {
            if (lod.buffer) impl_->lod_graveyard.emplace_back(impl_->lod_frame_counter + 8u, std::move(lod.buffer));
            lod = GpuMeshChunk::LodIndexBuffer{};
        }
        chunk.lod_level = 0;
        chunk.lod_job_submitted = false;
        return;
    }
    if (!chunk.lod_listed) {
        chunk.lod_listed = true;
        impl_->lod_chunks.push_back(chunk_index);
    }
}

void HelloO3dRenderer::set_chunk_size_cull_scale(std::size_t chunk_index, float scale) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    impl_->mesh_chunks[chunk_index].size_cull_scale = scale > 0.f ? scale : 1.f;
}

void HelloO3dRenderer::set_chunk_no_size_cull(std::size_t chunk_index, bool exempt) {
    if (impl_) scene_dirty(*impl_, "set_chunk_no_size_cull");
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    impl_->mesh_chunks[chunk_index].no_size_cull = exempt;
}

void HelloO3dRenderer::release_chunk_gpu(std::size_t chunk_index) {
    if (impl_) scene_dirty(*impl_, "release_chunk_gpu");
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    // (a clone's own text slots go back to the pool for the next AI bus)
    for (auto* list : {&chunk.opaque_batches, &chunk.alpha_batches, &chunk.alpha_nozwrite_batches, &chunk.shadow_batches})
        for (auto& b : *list)
            if (b.is_text_texture && b.text_slot_pooled && b.srv_slot != ~0u) {
                impl_->text_slot_free.push_back(b.srv_slot);
                b.text_slot_pooled = false;
            }
    chunk.meta.visible = false;
    chunk.vertex_buffer.Reset();
    chunk.index_buffer.Reset();
    chunk.vb_alloc.reset(); // (its arena range is reused once the frames in flight are done)
    chunk.vertex_buffer_view = {};
    chunk.index_buffer_view = {};
    chunk.shadow_batches.clear();
    chunk.opaque_batches.clear();
    chunk.alpha_batches.clear();
    chunk.alpha_nozwrite_batches.clear();
    clear_chunk_detail(chunk_index);
    // Textures stay in texture_cache (shared); VB/IB was the bulk of RAM growth.
}

void HelloO3dRenderer::set_chunk_presurface(std::size_t chunk_index, bool enabled) {
    if (impl_) scene_dirty(*impl_, "set_chunk_presurface");
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    chunk.is_presurface = enabled;
    auto mark = [enabled](MaterialDrawBatch& b) {
        b.is_presurface = enabled;
        // Unit126 TMap.Render pass 0: normal Z. Do not set no_zcheck — that drew the
        // hole.tga quad on top of the stairs (pink 4x4 DXT3) and skipped the depth punch.
        if (enabled) {
            b.no_zcheck = false;
            b.no_zwrite = false;
        }
    };
    for (auto& b : chunk.opaque_batches) mark(b);
    for (auto& b : chunk.alpha_batches) mark(b);
    for (auto& b : chunk.alpha_nozwrite_batches) mark(b);
}

void HelloO3dRenderer::set_chunk_render_phase(std::size_t chunk_index, WorldRenderPhase phase, bool surface_bias,
                                              bool decal) {
    if (impl_) scene_dirty(*impl_, "set_chunk_render_phase");
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    chunk.render_phase = phase;
    chunk.surface_bias = surface_bias;
    chunk.decal = decal;
    chunk.is_presurface = (phase == WorldRenderPhase::PreSurface);
    auto mark = [&](MaterialDrawBatch& b) {
        b.is_presurface = chunk.is_presurface;
        b.is_on_surface = decal || phase == WorldRenderPhase::OnSurface;
        if (chunk.is_presurface) {
            b.no_zcheck = false;
            b.no_zwrite = false;
        }
    };
    for (auto& b : chunk.opaque_batches) mark(b);
    for (auto& b : chunk.alpha_batches) mark(b);
    for (auto& b : chunk.alpha_nozwrite_batches) mark(b);
}

void HelloO3dRenderer::set_draw_shadow_decals(bool enabled) {
    if (impl_) scene_dirty(*impl_, "set_draw_shadow_decals");
    if (!impl_) return;
    impl_->draw_shadow_decals = enabled;
}

void HelloO3dRenderer::set_chunk_highlight(std::size_t chunk_index, float amount, float r, float g,
                                                 float b) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    const float prev = chunk.highlight;
    const float next = (std::max)(0.f, (std::min)(amount, 1.f));
    if (prev == next && (next <= 0.001f || (chunk.highlight_r == r && chunk.highlight_g == g && chunk.highlight_b == b)))
        return;
    chunk.highlight = next; // the scene cache compares the final highlight set per frame
    chunk.highlight_r = r;
    chunk.highlight_g = g;
    chunk.highlight_b = b;
    auto& stack = impl_->highlighted_chunks;
    if (chunk.highlight > 0.001f) {
        if (prev <= 0.001f) stack.push_back(chunk_index);
    } else if (prev > 0.001f) {
        stack.erase(std::remove(stack.begin(), stack.end(), chunk_index), stack.end());
    }
}

void HelloO3dRenderer::set_chunk_opacity(std::size_t chunk_index, float opacity) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    const float clamped = (std::max)(0.f, (std::min)(opacity, 1.f));
    if (chunk.opacity == clamped) return;
    chunk.opacity = clamped;
    scene_dirty(*impl_, "opacity");
}

void HelloO3dRenderer::clear_chunk_highlights() {
    if (!impl_) return;
    auto& stack = impl_->highlighted_chunks;
    for (const std::size_t ci : stack) {
        if (ci >= impl_->mesh_chunks.size()) continue;
        auto& chunk = impl_->mesh_chunks[ci];
        chunk.highlight = 0.f;
        chunk.highlight_r = 1.f;
        chunk.highlight_g = 0.95f;
        chunk.highlight_b = 0.55f;
    }
    stack.clear();
    // Opacity is now owned by the editor's scene-place/spline pass. Resetting every GPU
    // chunk here turned into an O(total_chunks) scan every frame on large maps.
}

void HelloO3dRenderer::set_chunk_invert_color(std::size_t chunk_index, bool invert) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    if (impl_->mesh_chunks[chunk_index].invert_color == invert) return;
    impl_->mesh_chunks[chunk_index].invert_color = invert;
    scene_dirty(*impl_, "invert");
}

void HelloO3dRenderer::set_chunk_wireframe(std::size_t chunk_index, bool wireframe) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    if (chunk.wireframe == wireframe) return;
    chunk.wireframe = wireframe;
    scene_dirty(*impl_, "wireframe");
}

void HelloO3dRenderer::set_chunk_path_rail(std::size_t chunk_index, bool rail) {
    if (impl_) scene_dirty(*impl_, "set_chunk_path_rail");
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    impl_->mesh_chunks[chunk_index].path_rail = rail;
}

void HelloO3dRenderer::set_chunk_fixed_lit(std::size_t chunk_index, bool fixed_lit) {
    if (impl_) scene_dirty(*impl_, "set_chunk_fixed_lit");
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& chunk = impl_->mesh_chunks[chunk_index];
    chunk.fixed_lit = fixed_lit;
    if (!fixed_lit) return;
    auto mark = [&](const std::vector<MaterialDrawBatch>& batches) {
        for (const auto& b : batches) {
            if (b.srv_slot != ~0u) impl_->texture_slot_ignore_season.insert(b.srv_slot);
        }
    };
    mark(chunk.opaque_batches);
    mark(chunk.alpha_batches);
    mark(chunk.alpha_nozwrite_batches);
    mark(chunk.shadow_batches);
}

void HelloO3dRenderer::mark_texture_slot_ignore_season(std::uint32_t slot) {
    if (!impl_) return;
    impl_->texture_slot_ignore_season.insert(slot);
}

bool HelloO3dRenderer::chunk_has_uv_vars(std::size_t chunk_index) const {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return false;
    return impl_->mesh_chunks[chunk_index].has_uv_vars;
}

void HelloO3dRenderer::apply_chunk_uv_vars(std::size_t chunk_index,
                                           const std::function<float(const std::string&)>& get) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size() || !get) return;
    bool changed = false;
    auto set_if = [&](float& dst, float v) {
        if (dst != v) {
            dst = v;
            changed = true;
        }
    };
    auto apply = [&](std::vector<MaterialDrawBatch>& batches) {
        for (auto& b : batches) {
            if (!b.texcoord_trans_x_var.empty()) set_if(b.uv_off_x, get(b.texcoord_trans_x_var));
            if (!b.texcoord_trans_y_var.empty()) set_if(b.uv_off_y, get(b.texcoord_trans_y_var));
            if (!b.alpha_scale_var.empty()) set_if(b.alpha_scale, std::clamp(get(b.alpha_scale_var), 0.f, 1.f));
            if (!b.lightmap_var.empty()) set_if(b.lightmap_scale, std::clamp(get(b.lightmap_var), 0.f, 1.f));
        }
    };
    auto& chunk = impl_->mesh_chunks[chunk_index];
    apply(chunk.opaque_batches);
    apply(chunk.alpha_batches);
    apply(chunk.alpha_nozwrite_batches);
    apply(chunk.shadow_batches);
    if (changed) scene_dirty(*impl_, "uv_vars");
}

bool HelloO3dRenderer::replace_chunk_albedo_rgba(std::size_t chunk_index, const std::uint8_t* rgba,
                                                 std::uint32_t width, std::uint32_t height,
                                                 int text_texture_index) {
    if (impl_) scene_dirty(*impl_, "replace_chunk_albedo_rgba");
    if (!impl_ || !rgba || width == 0 || height == 0) return false;
    if (chunk_index >= impl_->mesh_chunks.size()) return false;
    auto& d = *impl_;
    if (!d.device || !d.upload_command_list || !d.upload_allocator || !d.queue) return false;

    auto& chunk = d.mesh_chunks[chunk_index];
    std::vector<MaterialDrawBatch*> targets;
    auto collect = [&](std::vector<MaterialDrawBatch>& batches) {
        for (auto& b : batches) {
            if (!b.is_text_texture) continue;
            if (text_texture_index >= 0 && b.text_texture_index != text_texture_index) continue;
            targets.push_back(&b);
        }
    };
    collect(chunk.opaque_batches);
    collect(chunk.alpha_batches);
    collect(chunk.alpha_nozwrite_batches);
    collect(chunk.shadow_batches);
    if (targets.empty()) return false;

    // Same async path as append_textured_mesh: never wait_gpu_idle (that hitch'd stream + sco_visibility).
    wait_pending_texture_uploads();
    if (!try_reset_upload_allocator()) return false;
    throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "texttex cmd reset");

    const D3D12_CPU_DESCRIPTOR_HANDLE white_srv_cpu =
        offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);

    bool any = false;
    for (auto* batch : targets) {
        std::uint32_t slot = batch->srv_slot;
        if (!batch->text_slot_own) {
            // a clone's display: a slot pair of its own (pooled), never the prototype's shared one
            if (!d.text_slot_free.empty()) {
                slot = d.text_slot_free.back();
                d.text_slot_free.pop_back();
            } else {
                if (d.srv_next + 2u > kSrvDescriptorCount) continue;
                slot = d.srv_next;
                d.srv_next += 2;
                d.device->CopyDescriptorsSimple(
                    1,
                    offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                      kCbvDescriptorCount + slot + 1),
                    white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            batch->srv_slot = slot;
            batch->text_slot_own = true;
            batch->text_slot_pooled = true;
        }
        if (slot == ~0u) {
            if (d.srv_next + 2u > kSrvDescriptorCount) continue;
            slot = d.srv_next;
            d.srv_next += 2;
            d.device->CopyDescriptorsSimple(
                1,
                offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                  kCbvDescriptorCount + slot + 1),
                white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            batch->srv_slot = slot;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
        const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
        try {
            const auto tex = texture_detail::upload_rgba_texture(d.device.Get(), d.upload_command_list.Get(), dst_cpu,
                                                                 dst_gpu, rgba, width, height, width * 4u, 1u);
            if (!tex.valid) continue;
            d.texture_by_slot[slot] = tex.resource; // drops previous default-heap tex for this slot
            if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
            d.texture_slot_files.erase(slot);
            batch->srv_gpu = dst_gpu;
            batch->has_texture = true;
            any = true;
        } catch (...) {
            continue;
        }
    }

    if (!any) {
        try {
            d.upload_command_list->Close();
        } catch (...) {
        }
        return false;
    }
    throw_hr(d.upload_command_list->Close(), "texttex cmd close");
    ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 upload_fence = d.fence_value++;
    throw_hr(d.queue->Signal(d.fence.Get(), upload_fence), "texttex Signal");
    d.pending_tex_upload_fence = upload_fence; // cleared (+ scratch) on next wait_pending / frame
    return true;
}

std::string HelloO3dRenderer::chunk_freetex_original(std::size_t chunk_index,
                                                     std::string_view texture_key) const {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return {};
    const auto& chunk = impl_->mesh_chunks[chunk_index];
    auto scan = [&](const std::vector<MaterialDrawBatch>& batches) -> std::string {
        for (const auto& b : batches) {
            if (!b.is_freetex) continue;
            if (!texture_key.empty() && b.freetex_key != texture_key) continue;
            if (!b.original_albedo.empty()) return b.original_albedo;
        }
        return {};
    };
    if (auto s = scan(chunk.opaque_batches); !s.empty()) return s;
    if (auto s = scan(chunk.alpha_batches); !s.empty()) return s;
    if (auto s = scan(chunk.alpha_nozwrite_batches); !s.empty()) return s;
    return scan(chunk.shadow_batches);
}

namespace {
// a [matl_change] texture decoded and its mips built on a worker (a 2-4k TGA took a second of the main thread)
struct VariantJob {
    std::atomic<bool> done{false};
    texture_detail::RgbaMipChain chain;
    // a real DDS (by its bytes): read on the worker into the raw-file cache, the main thread only uploads it
    std::filesystem::path dds_path;
};
} // namespace

bool HelloO3dRenderer::chunk_freetex_variant_ready(std::size_t chunk_index, std::string_view texture_key,
                                                   const std::filesystem::path& path, int target) const {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size() || path.empty()) return true;
    const auto& d = *impl_;
    const auto& chunk = d.mesh_chunks[chunk_index];
    const std::string pkey = (target != 0 ? "t" + std::to_string(target) + "|" : std::string{}) + path.string();
    bool ready = true;
    auto each = [&](const std::vector<MaterialDrawBatch>& batches) {
        for (const auto& b : batches)
            if (b.is_freetex && b.freetex_key == texture_key && b.srv_slot != ~0u &&
                !d.freetex_variants.count(std::to_string(b.srv_slot) + "|" + pkey))
                ready = false;
    };
    each(chunk.opaque_batches);
    each(chunk.alpha_batches);
    each(chunk.alpha_nozwrite_batches);
    each(chunk.shadow_batches);
    return ready;
}

bool HelloO3dRenderer::set_chunk_freetex_variant(std::size_t chunk_index, std::string_view texture_key,
                                                 const std::filesystem::path& path, int target) {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return false;
    auto& d = *impl_;
    if (!d.device || !d.srv_heap) return false;
    auto& chunk = d.mesh_chunks[chunk_index];
    const std::string pkey = path.empty() ? std::string{} : path.string();      // the texture file
    const std::string vkey = (target != 0 ? "t" + std::to_string(target) + "|" : std::string{}) + pkey; // the table
    bool changed = false, recording = false, failed = false;
    auto cpu = [&](std::uint32_t slot) {
        return offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                 kCbvDescriptorCount + slot);
    };
    auto table_for = [&](const MaterialDrawBatch& b) -> std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> {
        const std::string vk = std::to_string(b.srv_slot) + "|" + vkey;
        if (const auto it = d.freetex_variants.find(vk); it != d.freetex_variants.end()) return it->second;
        if (failed) return std::nullopt;
        const bool own_night = b.variant_night ? b.base_has_nightmap : b.has_nightmap;
        const std::uint32_t n_own = b.has_transmap ? 4u : (own_night ? 3u : 2u); // (the transmap at +3 stays the slot's own)
        const std::uint32_t n = target != 0 ? (std::max)(n_own, 3u) : n_own;
        // the texture itself, once: a .dds goes up as it is (no per-texel work); anything else is decoded and
        // its mips built on a worker first - until then the slot keeps its own texture (null: try again)
        auto ts = d.variant_tex_slot.find(pkey);
        if (ts == d.variant_tex_slot.end()) {
            // Every file is read (and, if not a DDS, decoded with its mips) on a worker - a CTC paint named
            // ".dds" went up through load_texture_file on the main thread, and one that was a BMP by its
            // bytes was WIC-decoded there: freezes of up to 2.7 s with 120 AI cars.
            std::shared_ptr<VariantJob> job;
            {
                auto& jp = d.variant_jobs[pkey];
                if (!jp) {
                    auto nj = std::make_shared<VariantJob>();
                    jp = nj;
                    const std::filesystem::path p0 = path;
                    omsi::JobSystem::global().submit(
                        [nj, p0]() {
                            try {
                                // as load_texture_file: <stem>.dds first, then the file; the format by its bytes
                                std::filesystem::path p = p0;
                                if (!texture_detail::iequals_ext(p0, L".dds")) {
                                    const auto dds = p0.parent_path() / (p0.stem().wstring() + L".dds");
                                    if (texture_detail::path_exists(dds)) p = dds;
                                }
                                if (texture_detail::sniff_texture_format(p) == texture_detail::TexFormat::dds) {
                                    std::vector<std::uint8_t> bytes;
                                    if (texture_detail::read_file_bytes(p, bytes)) {
                                        std::vector<std::uint8_t> rgba;
                                        std::uint32_t uw = 0, uh = 0;
                                        if (texture_detail::dds_uncompressed_to_rgba(bytes.data(), bytes.size(), rgba, uw, uh)) {
                                            // (uncompressed: converted here, with its mips, not on the main thread)
                                            nj->chain = texture_detail::build_rgba_mip_chain(rgba.data(), uw, uh, uw * 4u, 0u);
                                        } else {
                                            texture_detail::raw_file_cache_put(p, std::move(bytes));
                                            nj->dds_path = p;
                                        }
                                    }
                                } else if (const auto img = load_image_cpu_rgba(p)) {
                                    nj->chain = texture_detail::build_rgba_mip_chain(img->rgba.data(), img->width,
                                                                                     img->height, img->width * 4u, 0u);
                                }
                            } catch (...) {
                            }
                            nj->done.store(true, std::memory_order_release);
                        },
                        omsi::JobSystem::Priority::Low);
                    return std::nullopt;
                }
                job = std::static_pointer_cast<VariantJob>(jp);
                if (!job->done.load(std::memory_order_acquire)) return std::nullopt;
            }
            // at most one new texture up per frame (the rest wait their turn: the slot shows its own until then)
            if (d.variant_upload_frame == d.variant_frame) return std::nullopt;
            if (d.srv_next + 1u > kSrvDescriptorCount) return std::nullopt;
            if (!recording) {
                if (!d.upload_command_list || !d.upload_allocator || !d.queue || !try_reset_upload_allocator()) {
                    failed = true;
                    return std::nullopt;
                }
                throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "variant cmd reset");
                recording = true;
            }
            const std::uint32_t tslot = d.srv_next;
            const D3D12_GPU_DESCRIPTOR_HANDLE tgpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, tslot);
            std::uint32_t got = ~0u;
            try {
                Dx12Texture tex{};
                d.variant_upload_frame = d.variant_frame;
                if (job->chain.levels > 0)
                    tex = texture_detail::upload_rgba_mip_chain(d.device.Get(), d.upload_command_list.Get(),
                                                                cpu(tslot), tgpu, job->chain);
                else if (!job->dds_path.empty()) // (its bytes are in the raw-file cache: an upload, no disk)
                    tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), cpu(tslot), tgpu,
                                            job->dds_path);
                if (tex.valid) {
                    d.srv_next += 1;
                    d.texture_by_slot[tslot] = tex.resource;
                    if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
                    got = tslot;
                }
            } catch (...) {
            }
            d.variant_jobs.erase(pkey);
            ts = d.variant_tex_slot.emplace(pkey, got).first;
        }
        if (ts->second == ~0u) {
            d.freetex_variants[vk] = b.base_srv_gpu; // (no file to read: keep showing its own texture)
            return b.base_srv_gpu;
        }
        if (d.srv_next + n > kSrvDescriptorCount) return std::nullopt;
        const std::uint32_t slot = d.srv_next;
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
        d.srv_next += n;
        if (target == 0) {
            d.device->CopyDescriptorsSimple(1, cpu(slot), cpu(ts->second), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            // the rest of the table (env map, night / light map) stays the slot's own
            for (std::uint32_t k = 1; k < n; ++k)
                d.device->CopyDescriptorsSimple(1, cpu(slot + k), cpu(b.srv_slot + k), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        } else {
            // the slot's own table with the item's night / light map in the night slot (+2 = t4)
            for (std::uint32_t k = 0; k < n; ++k)
                if (k != 2u && k < n_own)
                    d.device->CopyDescriptorsSimple(1, cpu(slot + k), cpu(b.srv_slot + k), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            d.device->CopyDescriptorsSimple(1, cpu(slot + 2u), cpu(ts->second), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        d.freetex_variants[vk] = gpu;
        return gpu;
    };
    auto each = [&](std::vector<MaterialDrawBatch>& batches) {
        for (auto& b : batches) {
            if (!b.is_freetex || b.freetex_key != texture_key || b.srv_slot == ~0u) continue;
            if (!b.variant_on) b.base_srv_gpu = b.srv_gpu;
            auto restore_night = [&]() {
                if (!b.variant_night) return;
                b.has_nightmap = b.base_has_nightmap;
                b.lightmap_mode = b.base_lightmap_mode;
                b.lightmap_scale = b.base_lightmap_scale;
                b.night_always = false;
                b.variant_night = false;
            };
            if (pkey.empty()) {
                if (b.variant_on) {
                    b.srv_gpu = b.base_srv_gpu;
                    b.variant_on = false;
                    restore_night();
                    changed = true;
                }
                continue;
            }
            const auto t = table_for(b);
            if (!t) continue;
            if (b.srv_gpu.ptr != t->ptr) changed = true;
            b.srv_gpu = *t;
            b.variant_on = true;
            if (target != 0 && t->ptr != b.base_srv_gpu.ptr) {
                if (!b.variant_night) {
                    b.base_has_nightmap = b.has_nightmap;
                    b.base_lightmap_mode = b.lightmap_mode;
                    b.base_lightmap_scale = b.lightmap_scale;
                    b.variant_night = true;
                }
                b.has_nightmap = true;
                b.night_always = target == 2;
                b.lightmap_mode = target == 3;
                if (target == 3) b.lightmap_scale = 1.f;
            } else if (target == 0) {
                restore_night();
            }
        }
    };
    each(chunk.opaque_batches);
    each(chunk.alpha_batches);
    each(chunk.alpha_nozwrite_batches);
    each(chunk.shadow_batches);
    if (recording) {
        throw_hr(d.upload_command_list->Close(), "variant cmd close");
        ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        const UINT64 upload_fence = d.fence_value++;
        throw_hr(d.queue->Signal(d.fence.Get(), upload_fence), "variant Signal");
        d.pending_tex_upload_fence = upload_fence;
    }
    if (changed) scene_dirty(d, "freetex_variant");
    return changed;
}

bool HelloO3dRenderer::replace_chunk_albedo_file(std::size_t chunk_index, const std::filesystem::path& path,
                                                 std::string_view texture_key) {
    if (!impl_ || path.empty() || chunk_index >= impl_->mesh_chunks.size()) return false;
    // A named slot ([matl_freetex] / [matl_change] / CTC key) gets a table of its own for THIS chunk: written
    // into the slot in place, the texture changed every clone sharing it - the parked cars cloned from a
    // repainted scenery car took its colour while their [LOD] mesh (another prototype) kept the default, so
    // the car changed colour at the LOD switch. Read and decoded on a worker (set_chunk_freetex_variant);
    // true once this chunk shows it.
    if (!texture_key.empty()) {
        set_chunk_freetex_variant(chunk_index, texture_key, path);
        return chunk_freetex_variant_ready(chunk_index, texture_key, path);
    }
    if (impl_) scene_dirty(*impl_, "replace_chunk_albedo_file");
    auto& d = *impl_;
    if (!d.device || !d.upload_command_list || !d.upload_allocator || !d.queue) return false;

    auto& chunk = d.mesh_chunks[chunk_index];
    std::vector<MaterialDrawBatch*> targets;
    auto collect = [&](std::vector<MaterialDrawBatch>& batches) {
        for (auto& b : batches) {
            if (!b.is_freetex) continue;
            if (!texture_key.empty() && b.freetex_key != texture_key) continue;
            targets.push_back(&b);
        }
    };
    collect(chunk.opaque_batches);
    collect(chunk.alpha_batches);
    collect(chunk.alpha_nozwrite_batches);
    collect(chunk.shadow_batches);
    if (targets.empty()) return false;

    if (!try_reset_upload_allocator()) return false;
    throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "freetex cmd reset");

    const D3D12_CPU_DESCRIPTOR_HANDLE white_srv_cpu =
        offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);

    bool any = false;
    for (auto* batch : targets) {
        std::uint32_t slot = batch->srv_slot;
        if (slot == ~0u) {
            if (d.srv_next + 2u > kSrvDescriptorCount) continue;
            slot = d.srv_next;
            d.srv_next += 2;
            d.device->CopyDescriptorsSimple(
                1,
                offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                  kCbvDescriptorCount + slot + 1),
                white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            batch->srv_slot = slot;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
        const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
        try {
            const auto tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, path);
            if (!tex.valid) continue;
            d.texture_by_slot[slot] = tex.resource;
            if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
            d.texture_slot_files[slot] = path;
            batch->srv_gpu = dst_gpu;
            batch->has_texture = true;
            any = true;
        } catch (...) {
            continue;
        }
    }

    if (!any) {
        try {
            d.upload_command_list->Close();
        } catch (...) {
        }
        return false;
    }
    throw_hr(d.upload_command_list->Close(), "freetex cmd close");
    ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 upload_fence = d.fence_value++;
    throw_hr(d.queue->Signal(d.fence.Get(), upload_fence), "freetex Signal");
    d.pending_tex_upload_fence = upload_fence;
    return true;
}

void HelloO3dRenderer::set_screen_rect_overlay(float fb_x0, float fb_y0, float fb_x1, float fb_y1, float fb_w,
                                              float fb_h, bool enabled) {
    if (impl_) { const float a_[7] = {fb_x0, fb_y0, fb_x1, fb_y1, fb_w, fb_h, enabled ? 1.f : 0.f}; static std::uint64_t s_h_ = 0; scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(a_, 7), "rect_overlay"); }
    if (!impl_) return;
    auto& d = *impl_;
    d.screen_rect_enabled = enabled;
    if (!enabled) {
        if (d.screen_rect_chunk != static_cast<std::size_t>(-1) && d.screen_rect_chunk < d.mesh_chunks.size())
            d.mesh_chunks[d.screen_rect_chunk].meta.visible = false;
        return;
    }
    if (fb_w < 1.f || fb_h < 1.f) return;
    const float minx = (std::min)(fb_x0, fb_x1);
    const float maxx = (std::max)(fb_x0, fb_x1);
    const float miny = (std::min)(fb_y0, fb_y1);
    const float maxy = (std::max)(fb_y0, fb_y1);
    if (maxx - minx < 2.f && maxy - miny < 2.f) {
        if (d.screen_rect_chunk != static_cast<std::size_t>(-1) && d.screen_rect_chunk < d.mesh_chunks.size())
            d.mesh_chunks[d.screen_rect_chunk].meta.visible = false;
        return;
    }
    auto to_ndc = [&](float sx, float sy, float& nx, float& ny) {
        nx = (sx / fb_w) * 2.f - 1.f;
        ny = 1.f - (sy / fb_h) * 2.f;
    };
    float x0, y0, x1, y1;
    to_ndc(minx, miny, x0, y0);
    to_ndc(maxx, maxy, x1, y1);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const float t = (2.f / fb_h) * 2.5f; // ~2.5 px border in NDC
    const float tw = (2.f / fb_w) * 2.5f;
    std::vector<omsi::o3d::Vertex> verts;
    std::vector<std::uint32_t> inds;
    auto push_quad = [&](float ax, float ay, float bx, float by) {
        const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
        const float pts[4][2] = {{ax, ay}, {bx, ay}, {bx, by}, {ax, by}};
        for (const auto& p : pts) {
            omsi::o3d::Vertex v{};
            v.position[0] = p[0];
            v.position[1] = p[1];
            v.position[2] = 0.f;
            v.normal[0] = 0.f;
            v.normal[1] = 0.f;
            v.normal[2] = 1.f;
            verts.push_back(v);
        }
        inds.insert(inds.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    // Border strips (outer frame).
    push_quad(x0, y1 - t, x1, y1);         // top
    push_quad(x0, y0, x1, y0 + t);         // bottom
    push_quad(x0, y0 + t, x0 + tw, y1 - t); // left
    push_quad(x1 - tw, y0 + t, x1, y1 - t); // right

    if (d.screen_rect_chunk == static_cast<std::size_t>(-1) || d.screen_rect_chunk >= d.mesh_chunks.size()) {
        if (!append_textured_mesh(verts.data(), static_cast<std::uint32_t>(verts.size()), inds.data(),
                                  static_cast<std::uint32_t>(inds.size()), {}, 0u))
            return;
        d.screen_rect_chunk = d.mesh_chunks.size() - 1;
    } else {
        replace_textured_mesh(d.screen_rect_chunk, verts.data(), static_cast<std::uint32_t>(verts.size()),
                              inds.data(), static_cast<std::uint32_t>(inds.size()), {}, 0u, false);
    }
    auto& chunk = d.mesh_chunks[d.screen_rect_chunk];
    chunk.screen_space = true;
    chunk.meta.visible = true;
    chunk.highlight = 1.f;
    chunk.highlight_r = 0.05f;
    chunk.highlight_g = 0.95f;
    chunk.highlight_b = 1.f;
    DirectX::XMStoreFloat4x4(&chunk.local_world_transposed, DirectX::XMMatrixIdentity());
}

namespace {

std::string tex_path_key(const std::filesystem::path& p) {
    try {
        const auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    } catch (const std::filesystem::filesystem_error& e) {
        omsi::log::error(std::string("tex path u8: ") + e.what());
        return {};
    }
}

bool steal_chunk_srv(const GpuMeshChunk& chunk, MaterialDrawBatch& batch) {
    auto from = [&](const std::vector<MaterialDrawBatch>& v) {
        for (const auto& b : v) {
            if (b.has_texture && b.srv_gpu.ptr != 0) {
                batch.has_texture = true;
                batch.srv_gpu = b.srv_gpu;
                batch.srv_slot = b.srv_slot;
                batch.has_nightmap = b.has_nightmap;
                return true;
            }
        }
        return false;
    };
    return from(chunk.opaque_batches) || from(chunk.alpha_batches) || from(chunk.alpha_nozwrite_batches);
}

} // namespace

void HelloO3dRenderer::flush_deferred_upload_reset() {
    if (!impl_ || !impl_->pending_upload_reset || !impl_->upload_allocator) return;
    (void)try_reset_upload_allocator();
}

bool HelloO3dRenderer::copy_file_texture_srv(const std::filesystem::path& path, std::uint64_t dst_cpu_ptr) {
    if (impl_) scene_dirty(*impl_, "copy_file_texture_srv");
    if (!impl_ || !impl_->device || path.empty() || dst_cpu_ptr == 0) return false;
    auto& d = *impl_;
    const std::string key = tex_path_key(path);
    if (key.empty()) return false;
    std::uint32_t slot = ~0u;
    if (const auto it = d.texture_slot_cache.find(key); it != d.texture_slot_cache.end())
        slot = it->second;
    else {
        try {
            if (const auto it2 = d.texture_slot_cache.find(path.string()); it2 != d.texture_slot_cache.end())
                slot = it2->second;
        } catch (const std::filesystem::filesystem_error&) {
        }
    }
    if (slot == ~0u) {
        if (d.srv_next + 2u > kSrvDescriptorCount) return false;
        if (!try_reset_upload_allocator()) return false;
        const HRESULT rst = d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr);
        if (FAILED(rst)) {
            omsi::log::error("copy_file_texture_srv cmd Reset hr=" +
                             std::to_string(static_cast<unsigned>(rst)));
            return false;
        }
        slot = d.srv_next;
        d.srv_next += 2;
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
        const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
        const auto tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, path);
        if (!tex.valid) {
            const HRESULT cl = d.upload_command_list->Close();
            if (FAILED(cl))
                omsi::log::error("copy_file_texture_srv close empty hr=" +
                                 std::to_string(static_cast<unsigned>(cl)));
            d.srv_next -= 2;
            return false;
        }
        d.texture_by_slot[slot] = tex.resource;
        if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
        d.texture_slot_files[slot] = path;
        const HRESULT cl = d.upload_command_list->Close();
        if (FAILED(cl)) {
            omsi::log::error("copy_file_texture_srv close hr=" + std::to_string(static_cast<unsigned>(cl)));
            return false;
        }
        ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        const UINT64 upload_fence = d.fence_value++;
        const HRESULT sig = d.queue->Signal(d.fence.Get(), upload_fence);
        if (FAILED(sig)) {
            omsi::log::error("copy_file_texture_srv Signal hr=" + std::to_string(static_cast<unsigned>(sig)));
            return false;
        }
        d.pending_tex_upload_fence = upload_fence;
        d.texture_slot_cache[key] = slot;
        d.texture_cache[key + "#env#white"] = dst_gpu;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dst{};
    dst.ptr = static_cast<SIZE_T>(dst_cpu_ptr);
    const D3D12_CPU_DESCRIPTOR_HANDLE src = offset_cpu_handle(
        d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
    d.device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool HelloO3dRenderer::replace_textured_mesh(std::size_t chunk_index, const omsi::o3d::Vertex* vertices,
                                            std::uint32_t vertex_count, const std::uint32_t* indices,
                                            std::uint32_t index_count, const std::filesystem::path& texture_path,
                                            std::uint8_t alpha_mode, bool wait_gpu, bool wireframe,
                                            bool force_tex) {
    if (impl_) scene_dirty(*impl_, "replace_textured_mesh");
    if (!impl_ || !vertices || !indices || vertex_count == 0 || index_count == 0) return false;
    if (chunk_index >= impl_->mesh_chunks.size()) return false;
    auto& d = *impl_;
    if (!d.device) return false;

    (void)wait_gpu;
    auto& chunk = d.mesh_chunks[chunk_index];
    const float keep_hl = chunk.highlight;
    const float keep_hr = chunk.highlight_r;
    const float keep_hg = chunk.highlight_g;
    const float keep_hb = chunk.highlight_b;
    const float keep_op = chunk.opacity;
    const XMFLOAT4X4 keep_world = chunk.local_world_transposed;
    const bool keep_visible = chunk.meta.visible;
    const bool keep_is_terrain = chunk.is_terrain;

    float bmin[3] = {vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
    float bmax[3] = {bmin[0], bmin[1], bmin[2]};
    for (std::uint32_t i = 1; i < vertex_count; ++i) {
        for (int a = 0; a < 3; ++a) {
            bmin[a] = (std::min)(bmin[a], vertices[i].position[a]);
            bmax[a] = (std::max)(bmax[a], vertices[i].position[a]);
        }
    }
    for (int a = 0; a < 3; ++a) {
        chunk.meta.bbox_min[a] = bmin[a];
        chunk.meta.bbox_max[a] = bmax[a];
    }
    chunk.meta.mesh_file = texture_path.empty() ? "__proc_mesh__" : texture_path.filename().string(); chunk.content_mesh = -1;
    // A terrain tile re-uploaded (streaming, brush stroke, AlignTerrain reload) keeps its terrain marking:
    // it must stay in the terrain pass / terrain bias, never fall back into the surface pass.
    if (keep_is_terrain) chunk.meta.mesh_file = "__oo_terrain__"; chunk.content_mesh = -1;
    chunk.meta.visible = keep_visible;
    chunk.highlight = keep_hl;
    chunk.highlight_r = keep_hr;
    chunk.highlight_g = keep_hg;
    chunk.highlight_b = keep_hb;
    chunk.opacity = keep_op;
    chunk.local_world_transposed = keep_world;
    chunk.pick_positions.resize(vertex_count);
    for (std::uint32_t i = 0; i < vertex_count; ++i)
        chunk.pick_positions[i] = {vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]};
    chunk.pick_indices.assign(indices, indices + index_count);

    // Geometry replaced: invalidate terrain LODs (old index buffers outlive the frames still in flight).
    for (auto& lod : chunk.lod_ib) {
        if (lod.buffer) d.lod_graveyard.emplace_back(d.lod_frame_counter + 8u, std::move(lod.buffer));
        lod = GpuMeshChunk::LodIndexBuffer{};
    }
    ++chunk.lod_gen;
    chunk.lod_job_submitted = false;
    chunk.lod_level = 0;
    chunk.lod_capable = terrain_lod::is_lod_candidate(index_count, bmin, bmax);
    if (chunk.lod_capable && !chunk.lod_listed) {
        chunk.lod_listed = true;
        d.lod_chunks.push_back(chunk_index);
    }

    const UINT vertex_buffer_size = static_cast<UINT>(vertex_count * sizeof(omsi::o3d::Vertex));
    const UINT index_buffer_size = static_cast<UINT>(index_count * sizeof(std::uint32_t));
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC vb_desc = CD3DX12_RESOURCE_DESC::Buffer(vertex_buffer_size);
    const CD3DX12_RESOURCE_DESC ib_desc = CD3DX12_RESOURCE_DESC::Buffer(index_buffer_size);

    chunk.vertex_buffer.Reset();
    chunk.index_buffer.Reset();
    chunk.vb_alloc.reset();
    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(chunk.vertex_buffer.GetAddressOf())),
             "replace vb");
    void* mapped = nullptr;
    throw_hr(chunk.vertex_buffer->Map(0, nullptr, &mapped), "replace vb map");
    std::memcpy(mapped, vertices, vertex_buffer_size);
    chunk.vertex_buffer->Unmap(0, nullptr);
    chunk.vertex_buffer_view.BufferLocation = chunk.vertex_buffer->GetGPUVirtualAddress();
    chunk.vertex_buffer_view.StrideInBytes = sizeof(omsi::o3d::Vertex);
    chunk.vertex_buffer_view.SizeInBytes = vertex_buffer_size;

    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &ib_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(chunk.index_buffer.GetAddressOf())),
             "replace ib");
    throw_hr(chunk.index_buffer->Map(0, nullptr, &mapped), "replace ib map");
    std::memcpy(mapped, indices, index_buffer_size);
    chunk.index_buffer->Unmap(0, nullptr);
    chunk.index_buffer_view.BufferLocation = chunk.index_buffer->GetGPUVirtualAddress();
    chunk.index_buffer_view.SizeInBytes = index_buffer_size;
    chunk.index_buffer_view.Format = DXGI_FORMAT_R32_UINT;

    MaterialDrawBatch batch{};
    batch.index_start = 0;
    batch.index_count = index_count;
    if (alpha_mode == 2) {
        // Splines/roads often use matl_alpha=2; without Z write the upper strip ghosts through.
        batch.alpha_mode = MatlAlphaMode::alpha_blend;
        batch.no_zwrite = false;
    } else if (alpha_mode == 1) {
        batch.alpha_mode = MatlAlphaMode::alpha_test;
        batch.no_zwrite = false;
    } else {
        batch.alpha_mode = MatlAlphaMode::opaque;
        batch.no_zwrite = false;
    }
    std::uintmax_t tex_sz = 0;
    std::filesystem::file_time_type tex_mt{};
    bool tex_id_ok = false;
    if (!texture_path.empty()) {
        std::error_code tec;
        tex_sz = std::filesystem::file_size(texture_path, tec);
        if (!tec) {
            tex_mt = std::filesystem::last_write_time(texture_path, tec);
            tex_id_ok = !tec;
        }
    }
    const bool same_tex = !force_tex && tex_id_ok && texture_path == chunk.bound_tex &&
                          tex_sz == chunk.bound_tex_size && tex_mt == chunk.bound_tex_mtime;
    if (same_tex && steal_chunk_srv(chunk, batch)) {
        chunk.bound_tex = texture_path;
        chunk.bound_tex_size = tex_sz;
        chunk.bound_tex_mtime = tex_mt;
    } else if (!texture_path.empty()) {
        const std::string key = tex_path_key(texture_path);
        const std::string env_key = key.empty() ? std::string{} : key + "#env#white";
        bool bound = false;
        if (!force_tex && !env_key.empty()) {
            if (auto it = d.texture_cache.find(env_key); it != d.texture_cache.end()) {
                batch.has_texture = true;
                batch.srv_gpu = it->second;
                bound = true;
            }
        }
        if (!bound && !force_tex) {
            try {
                if (auto it2 = d.texture_cache.find(texture_path.string()); it2 != d.texture_cache.end()) {
                    batch.has_texture = true;
                    batch.srv_gpu = it2->second;
                    bound = true;
                }
            } catch (...) { // path::string() throws std::system_error on non-ANSI names, not filesystem_error
            }
        }
        if (!bound && d.srv_next + 2u <= kSrvDescriptorCount) {
            const bool alloc_ok = try_reset_upload_allocator();
            if (!alloc_ok) {
                if (!steal_chunk_srv(chunk, batch))
                    omsi::log::warn("replace_textured_mesh: allocator busy, keeping prior SRV");
            } else {
                const HRESULT rst = d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr);
                if (FAILED(rst)) {
                    omsi::log::error("replace tex cmd Reset hr=" + std::to_string(static_cast<unsigned>(rst)));
                    if (!steal_chunk_srv(chunk, batch)) return false;
                } else {
                    const std::uint32_t slot = d.srv_next;
                    d.srv_next += 2;
                    const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
                        d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
                    const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu =
                        offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
                    const auto tex =
                        load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, texture_path);
                    if (tex.valid) {
                        d.texture_by_slot[slot] = tex.resource;
                        if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
                        d.texture_slot_files[slot] = texture_path;
                        const HRESULT cl = d.upload_command_list->Close();
                        if (FAILED(cl)) {
                            omsi::log::error("replace tex close hr=" + std::to_string(static_cast<unsigned>(cl)));
                            d.srv_next -= 2;
                            if (!steal_chunk_srv(chunk, batch)) return false;
                        } else {
                            ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
                            d.queue->ExecuteCommandLists(1, lists);
                            const UINT64 upload_fence = d.fence_value++;
                            const HRESULT sig = d.queue->Signal(d.fence.Get(), upload_fence);
                            if (FAILED(sig)) {
                                omsi::log::error("replace tex Signal hr=" +
                                                 std::to_string(static_cast<unsigned>(sig)));
                                if (!steal_chunk_srv(chunk, batch)) return false;
                            } else {
                                d.pending_tex_upload_fence = upload_fence;
                                batch.srv_gpu = dst_gpu;
                                batch.has_texture = true;
                                batch.srv_slot = slot;
                                if (!key.empty()) {
                                    d.texture_slot_cache[key] = slot;
                                    d.texture_cache[env_key] = dst_gpu;
                                }
                                bound = true;
                            }
                        }
                    } else {
                        const HRESULT cl = d.upload_command_list->Close();
                        if (FAILED(cl))
                            omsi::log::error("replace tex close empty hr=" +
                                             std::to_string(static_cast<unsigned>(cl)));
                        d.srv_next -= 2;
                        omsi::log::error("replace_textured_mesh: load failed");
                        if (!steal_chunk_srv(chunk, batch)) return false;
                    }
                }
            }
        }
        if (batch.has_texture && tex_id_ok) {
            chunk.bound_tex = texture_path;
            chunk.bound_tex_size = tex_sz;
            chunk.bound_tex_mtime = tex_mt;
        }
    }

    chunk.opaque_batches.clear();
    chunk.alpha_batches.clear();
    chunk.alpha_nozwrite_batches.clear();
    chunk.shadow_batches.clear();
    if (batch.alpha_mode == MatlAlphaMode::alpha_blend || batch.alpha_mode == MatlAlphaMode::alpha_test)
        chunk.alpha_batches.push_back(batch);
    else
        chunk.opaque_batches.push_back(batch);
    chunk.wireframe = wireframe;
    chunk.invert_color = false;
    return true;
}

std::optional<std::size_t> HelloO3dRenderer::clone_chunk(std::size_t src_chunk_index) {
    if (impl_) scene_dirty(*impl_, "clone_chunk");
    if (!impl_ || src_chunk_index >= impl_->mesh_chunks.size()) return std::nullopt;
    // Released source (stream unload) has no VB/IB/batches: a clone would be pickable but never drawn.
    if (!chunk_has_gpu(src_chunk_index)) return std::nullopt;
    GpuMeshChunk clone = impl_->mesh_chunks[src_chunk_index]; // ComPtr share VB/IB
    clone.highlight = 0.f;
    clone.opacity = 1.f;
    clone.meta.visible = true;
    // (the helper mark is the instance's, not the mesh's: a carpark box that was the first of its mesh hid every
    //  ordinary object cloned from it while Show carpark was off)
    clone.editor_helper = 0;
    clone.want_visible = true;
    // Pick mesh is identical in local space; sharing via empty + AABB fallback (pick.cpp)
    // avoids duplicating megabytes of pick_positions per instance (RAM climb on stream).
    clone.pick_positions.clear();
    clone.pick_indices.clear();
    if (clone.pick_src == static_cast<std::size_t>(-1)) clone.pick_src = src_chunk_index;
    // (its displays: drawn into slots of its own on the first replace_chunk_albedo_rgba - every AI bus of a type
    //  showed the last one's destination otherwise)
    for (auto* list : {&clone.opaque_batches, &clone.alpha_batches, &clone.alpha_nozwrite_batches, &clone.shadow_batches})
        for (auto& b : *list)
            if (b.is_text_texture) {
                b.text_slot_own = false;
                b.text_slot_pooled = false;
            }
    XMStoreFloat4x4(&clone.local_world_transposed, DirectX::XMMatrixIdentity());
    impl_->mesh_chunks.push_back(std::move(clone));
    return impl_->mesh_chunks.size() - 1;
}

HelloO3dRenderer::ResourceDiag HelloO3dRenderer::resource_diag() const {
    ResourceDiag out{};
    if (!impl_) return out;
    out.chunks = impl_->mesh_chunks.size();
    out.srv_next = impl_->srv_next;
    out.texture_cache = impl_->texture_cache.size();
    out.tex_slots = impl_->texture_by_slot.size();
    out.upload_scratch = impl_->texture_upload_scratch.size();
    for (const auto& c : impl_->mesh_chunks) {
        if (c.vertex_buffer && c.index_buffer) ++out.chunks_with_gpu;
        out.pick_bytes += c.pick_positions.size() * sizeof(DirectX::XMFLOAT3);
        out.pick_bytes += c.pick_indices.size() * sizeof(std::uint32_t);
    }
    return out;
}

HelloO3dRenderer::GpuTiming HelloO3dRenderer::last_gpu_timing() const {
    GpuTiming t{};
    if (!impl_) return t;
    t.gpu_scene_ms = impl_->perf_last_gpu_scene_ms;
    t.gpu_ui_ms = impl_->perf_last_gpu_ui_ms;
    t.gpu_total_ms = impl_->perf_last_gpu_ms;
    t.cpu_wait_fence_ms = impl_->perf_wait_frame_ms;
    t.cpu_wait_tex_ms = impl_->perf_wait_texture_ms;
    t.cpu_populate_ms = impl_->perf_populate_ms;
    t.cpu_cull_ms = impl_->perf_cull_ms;
    t.cpu_record_ms = impl_->perf_record_ms;
    t.cpu_submit_ms = impl_->perf_submit_ms;
    t.cpu_present_ms = impl_->perf_present_ms;
    t.draws = impl_->perf_draws;
    t.visible_chunks = impl_->perf_chunks_visible;
    t.culled_chunks = impl_->perf_chunks_culled;
    t.bound = "n/a";
    return t;
}

bool HelloO3dRenderer::chunk_has_gpu(std::size_t chunk_index) const {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return false;
    const auto& c = impl_->mesh_chunks[chunk_index];
    if (!c.vertex_buffer || !c.index_buffer) return false;
    return !c.opaque_batches.empty() || !c.alpha_batches.empty() || !c.alpha_nozwrite_batches.empty() ||
           !c.shadow_batches.empty();
}

std::string HelloO3dRenderer::chunk_debug_line(std::size_t chunk_index) const {
    if (!impl_) return "no-renderer";
    if (chunk_index >= impl_->mesh_chunks.size())
        return "chunk=" + std::to_string(chunk_index) + " OUT_OF_RANGE max=" +
               std::to_string(impl_->mesh_chunks.size());
    const auto& c = impl_->mesh_chunks[chunk_index];
    std::uint32_t idx = 0, tex = 0;
    auto count = [&](const std::vector<MaterialDrawBatch>& v) {
        for (const auto& b : v) {
            idx += b.index_count;
            if (b.has_texture) ++tex;
        }
    };
    count(c.opaque_batches);
    count(c.alpha_batches);
    count(c.alpha_nozwrite_batches);
    char buf[512]{};
    std::snprintf(buf, sizeof(buf),
                  "chunk=%zu mesh=%s vis=%d vb=%d ib=%d batches(op=%zu a=%zu anz=%zu sh=%zu) idx=%u tex=%u "
                  "opacity=%.2f pre=%d wire=%d world=(%.2f,%.2f,%.2f) bbox=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)",
                  chunk_index, c.meta.mesh_file.c_str(), c.meta.visible ? 1 : 0, c.vertex_buffer ? 1 : 0,
                  c.index_buffer ? 1 : 0, c.opaque_batches.size(), c.alpha_batches.size(),
                  c.alpha_nozwrite_batches.size(), c.shadow_batches.size(), idx, tex,
                  static_cast<double>(c.opacity), c.is_presurface ? 1 : 0, c.wireframe ? 1 : 0,
                  static_cast<double>(c.local_world_transposed._14),
                  static_cast<double>(c.local_world_transposed._24),
                  static_cast<double>(c.local_world_transposed._34), static_cast<double>(c.meta.bbox_min[0]),
                  static_cast<double>(c.meta.bbox_min[1]), static_cast<double>(c.meta.bbox_min[2]),
                  static_cast<double>(c.meta.bbox_max[0]), static_cast<double>(c.meta.bbox_max[1]),
                  static_cast<double>(c.meta.bbox_max[2]));
    std::string out = buf;
    auto txt = [&](const char* lname, const std::vector<MaterialDrawBatch>& v) {
        for (const auto& b : v) {
            if (!b.is_text_texture && !b.is_freetex) continue;
            char t[200]{};
            std::snprintf(t, sizeof(t), " [%s text=%d tt=%d srv=%u hastex=%d alpha=%d idx=%u start=%u key=%s]", lname,
                          b.is_text_texture ? 1 : 0, b.text_texture_index, b.srv_slot, b.has_texture ? 1 : 0,
                          static_cast<int>(b.alpha_mode), b.index_count, b.index_start, b.freetex_key.c_str());
            out += t;
        }
    };
    txt("op", c.opaque_batches);
    txt("a", c.alpha_batches);
    txt("anz", c.alpha_nozwrite_batches);
    return out;
}

// Textures replaced by a reload stay alive while copied SRV descriptors may still point at them.
static constexpr std::size_t kRetiredTextureCap = 768;

bool HelloO3dRenderer::reload_file_textures() {
    if (impl_) scene_dirty(*impl_, "reload_file_textures");
    if (!impl_) return false;
    auto& d = *impl_;
    if (!d.device || !d.upload_command_list || !d.upload_allocator || !d.queue) return false;
    std::vector<std::pair<std::uint32_t, std::filesystem::path>> jobs;
    jobs.reserve(d.texture_slot_files.size());
    for (const auto& [slot, path] : d.texture_slot_files) {
        if (!path.empty()) jobs.emplace_back(slot, path);
    }
    if (jobs.empty()) {
        omsi::log::info("reload_file_textures: no file-backed slots");
        return true;
    }
    wait_gpu_idle_for_upload("reload_file_textures_before");
    throw_hr(d.upload_allocator->Reset(), "reload tex alloc reset");
    throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "reload tex cmd reset");
    int ok = 0;
    for (const auto& [slot, path] : jobs) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
        const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
        try {
            texture_detail::cpu_texture_cache_invalidate(path);
            const auto tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, path);
            if (!tex.valid) continue;
            // PBR tables and texture_cache hits share descriptors via CopyDescriptorsSimple, so the old
            // default-heap resource may still be sampled: retire it instead of releasing it (releasing
            // produced the pink/black rectangles after a texture-quality change). Bounded, so repeated
            // reloads cannot grow VRAM without limit.
            if (const auto it = d.texture_by_slot.find(slot); it != d.texture_by_slot.end() && it->second) {
                if (d.retired_textures.size() >= kRetiredTextureCap) {
                    omsi::log::warn("reload_file_textures: retired_textures cap reached, dropping oldest");
                    d.retired_textures.erase(d.retired_textures.begin());
                }
                d.retired_textures.push_back(std::move(it->second));
            }
            d.texture_by_slot[slot] = tex.resource;
            if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
            ++ok;
        } catch (...) {
            continue;
        }
    }
    throw_hr(d.upload_command_list->Close(), "reload tex cmd close");
    ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    wait_gpu_idle_for_upload("reload_file_textures_after");
    omsi::log::info("reload_file_textures ok=" + std::to_string(ok) + "/" + std::to_string(jobs.size()) +
                    " max_edge=" + std::to_string(texture_max_edge()));
    return ok > 0;
}

void HelloO3dRenderer::begin_season_texture_rebind() {
    if (!impl_) return;
    auto& d = *impl_;
    d.season_rebind_queue.clear();
    d.season_rebind_cursor = 0;
    d.season_rebind_queue.reserve(d.texture_slot_files.size());
    std::vector<std::filesystem::path> files;
    files.reserve(d.texture_slot_files.size());
    for (const auto& [slot, path] : d.texture_slot_files) {
        if (path.empty()) continue;
        if (d.texture_slot_ignore_season.count(slot)) continue;
        d.season_rebind_queue.push_back(slot);
        files.push_back(path);
    }
    // The season files found and read on the pool: done here (the folder probes for every texture slot and
    // then the reads, all waited for) it held the editor 12 s when the season changed while driving.
    // The GPU upload stays time-sliced in pump_season_texture_rebinds (which reads from disk itself for a
    // slot whose file is not in RAM yet).
    auto ready = std::make_shared<std::atomic<bool>>(files.empty());
    d.season_prefetch_ready = ready;
    if (!files.empty())
        omsi::JobSystem::global().submit(
            [files = std::move(files), ready]() {
                try {
                    std::vector<std::filesystem::path> prefetch;
                    prefetch.reserve(files.size());
                    for (const auto& path : files)
                        if (auto next = resolve_season_rebind(path); next && *next != path)
                            prefetch.push_back(std::move(*next));
                    if (!prefetch.empty()) prefetch_textures_cpu(prefetch);
                } catch (...) {
                }
                ready->store(true, std::memory_order_release);
            },
            omsi::JobSystem::Priority::Low);
}

int HelloO3dRenderer::pump_season_texture_rebinds(int max_n) {
    if (!impl_ || max_n <= 0) return 0;
    auto& d = *impl_;
    if (d.season_rebind_cursor >= d.season_rebind_queue.size()) return 0;
    if (d.season_prefetch_ready && !d.season_prefetch_ready->load(std::memory_order_acquire)) return 0;
    if (!d.device || !d.queue || !d.fence) return 0;
    // Never WaitForGpu on the frame thread — borrow a free tex-upload ring slot or defer.
    if (!try_reset_upload_allocator()) return 0;
    if (!d.upload_command_list || !d.upload_allocator) return 0;

    struct Job {
        std::uint32_t slot = 0;
        std::filesystem::path path;
    };
    // Cap hard: season hitch was from sync idle + big batches; 1–2 uploads/frame stays interactive.
    const int cap = (std::min)(max_n, 2);
    std::vector<Job> jobs;
    jobs.reserve(static_cast<std::size_t>(cap));
    while (d.season_rebind_cursor < d.season_rebind_queue.size() &&
           static_cast<int>(jobs.size()) < cap) {
        const std::uint32_t slot = d.season_rebind_queue[d.season_rebind_cursor++];
        if (d.texture_slot_ignore_season.count(slot)) continue;
        auto it = d.texture_slot_files.find(slot);
        if (it == d.texture_slot_files.end() || it->second.empty()) continue;
        auto next = resolve_season_rebind(it->second);
        if (!next || *next == it->second) continue;
        // Missing seasonal file: keep current path (no black flash).
        if (!std::filesystem::exists(*next)) continue;
        jobs.push_back(Job{slot, std::move(*next)});
    }
    if (jobs.empty()) return 0;

    if (FAILED(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr))) return 0;
    int ok = 0;
    for (const auto& job : jobs) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + job.slot);
        const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu =
            offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, job.slot);
        try {
            const auto tex =
                load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, job.path);
            // Failed load: leave previous SRV + path untouched (avoids black meshes).
            if (!tex.valid) continue;
            // Copied descriptors may still sample the replaced seasonal texture: retire, do not release.
            if (const auto it = d.texture_by_slot.find(job.slot); it != d.texture_by_slot.end() && it->second) {
                if (d.retired_textures.size() >= kRetiredTextureCap)
                    d.retired_textures.erase(d.retired_textures.begin());
                d.retired_textures.push_back(std::move(it->second));
            }
            d.texture_by_slot[job.slot] = tex.resource;
            if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
            d.texture_slot_files[job.slot] = job.path;
            ++ok;
        } catch (...) {
            continue;
        }
    }
    if (ok == 0) {
        (void)d.upload_command_list->Close();
        return 0;
    }
    if (FAILED(d.upload_command_list->Close())) return 0;
    ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 upload_fence = d.fence_value++;
    if (FAILED(d.queue->Signal(d.fence.Get(), upload_fence))) return ok;
    d.pending_tex_upload_fence = upload_fence; // drained by wait_pending_texture_uploads / ring
    return ok;
}

void HelloO3dRenderer::invalidate_texture_path(const std::filesystem::path& path) {
    if (impl_) scene_dirty(*impl_, "invalidate_texture_path");
    if (!impl_ || path.empty()) return;
    auto& d = *impl_;
    texture_detail::cpu_texture_cache_invalidate(path);
    const std::string p = path.string();
    const std::string key = p + "#env#white";
    d.texture_cache.erase(key);
    d.texture_cache.erase(p);
    d.texture_slot_cache.erase(p);
}

std::size_t HelloO3dRenderer::chunk_count() const {
    return impl_ ? impl_->mesh_chunks.size() : 0;
}

} // namespace omsi::gfx::dx12
