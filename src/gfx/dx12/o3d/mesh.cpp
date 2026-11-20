#include <mutex>
#include <chrono>
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
#include "omsi/o3d_prepared_cache.hpp"
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
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

namespace omsi::gfx::dx12 {

namespace {
std::mutex g_o3d_upload_mutex; // serialize copy/upload allocator (single list)


// Terrain tiles carry 50-100 MB of vertices/indices. Their upload ran entirely serial on the main thread (bbox scan,
// two pick-array copies, staging memcpy): 30-150 ms per tile in the logs. These helpers split the pure-CPU parts
// across the job pool; small meshes keep the original serial path.
void parallel_memcpy(void* dst, const void* src, std::size_t size) {
    constexpr std::size_t kParallelMin = 8u << 20;
    constexpr std::size_t kBlock = 4u << 20;
    if (size < kParallelMin) {
        std::memcpy(dst, src, size);
        return;
    }
    const std::size_t blocks = (size + kBlock - 1) / kBlock;
    omsi::JobSystem::global().parallel_for(
        0, blocks,
        [&](std::size_t b) {
            const std::size_t off = b * kBlock;
            const std::size_t len = (std::min)(kBlock, size - off);
            std::memcpy(static_cast<char*>(dst) + off, static_cast<const char*>(src) + off, len);
        },
        1);
}

// bbox + the CPU-side picking copies (positions/indices) of a freshly uploaded mesh.
void prepare_mesh_cpu(const omsi::o3d::Vertex* vertices, std::uint32_t vertex_count, const std::uint32_t* indices,
                      std::uint32_t index_count, float bmin[3], float bmax[3],
                      std::vector<DirectX::XMFLOAT3>& pick_positions, std::vector<std::uint32_t>& pick_indices) {
    bmin[0] = bmax[0] = vertices[0].position[0];
    bmin[1] = bmax[1] = vertices[0].position[1];
    bmin[2] = bmax[2] = vertices[0].position[2];
    pick_positions.resize(vertex_count);
    pick_indices.resize(index_count);
    constexpr std::uint32_t kParallelMinVerts = 100000;
    constexpr std::uint32_t kBlock = 65536;
    if (vertex_count < kParallelMinVerts) {
        for (std::uint32_t i = 0; i < vertex_count; ++i) {
            for (int a = 0; a < 3; ++a) {
                bmin[a] = (std::min)(bmin[a], vertices[i].position[a]);
                bmax[a] = (std::max)(bmax[a], vertices[i].position[a]);
            }
            pick_positions[i] = {vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]};
        }
        std::memcpy(pick_indices.data(), indices, static_cast<std::size_t>(index_count) * sizeof(std::uint32_t));
        return;
    }
    const std::size_t blocks = (vertex_count + kBlock - 1) / kBlock;
    std::vector<std::array<float, 6>> part(blocks);
    DirectX::XMFLOAT3* const pp = pick_positions.data();
    omsi::JobSystem::global().parallel_for(
        0, blocks,
        [&, pp](std::size_t b) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(b) * kBlock;
            const std::uint32_t i1 = (std::min)(i0 + kBlock, vertex_count);
            float lo[3] = {vertices[i0].position[0], vertices[i0].position[1], vertices[i0].position[2]};
            float hi[3] = {lo[0], lo[1], lo[2]};
            for (std::uint32_t i = i0; i < i1; ++i) {
                for (int a = 0; a < 3; ++a) {
                    lo[a] = (std::min)(lo[a], vertices[i].position[a]);
                    hi[a] = (std::max)(hi[a], vertices[i].position[a]);
                }
                pp[i] = {vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]};
            }
            part[b] = {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]};
        },
        1);
    for (const auto& p : part) {
        for (int a = 0; a < 3; ++a) {
            bmin[a] = (std::min)(bmin[a], p[static_cast<std::size_t>(a)]);
            bmax[a] = (std::max)(bmax[a], p[static_cast<std::size_t>(3 + a)]);
        }
    }
    parallel_memcpy(pick_indices.data(), indices, static_cast<std::size_t>(index_count) * sizeof(std::uint32_t));
}

// P7: static mesh data lives on DEFAULT heap; one-time UPLOAD staging copy.
void create_default_buffer(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                           const void* data, UINT64 size, D3D12_RESOURCE_STATES final_state,
                           Microsoft::WRL::ComPtr<ID3D12Resource>& out_default,
                           Microsoft::WRL::ComPtr<ID3D12Resource>& out_staging) {
    const CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    throw_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(out_default.GetAddressOf())),
             "default buffer");
    throw_hr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(out_staging.GetAddressOf())),
             "staging buffer");
    void* mapped = nullptr;
    throw_hr(out_staging->Map(0, nullptr, &mapped), "staging map");
    parallel_memcpy(mapped, data, static_cast<size_t>(size));
    out_staging->Unmap(0, nullptr);
    cmd->CopyBufferRegion(out_default.Get(), 0, out_staging.Get(), 0, size);
    D3D12_RESOURCE_BARRIER barrier = dx12_transition_barrier(out_default.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             final_state);
    cmd->ResourceBarrier(1, &barrier);
}

// VB + IB in ONE default buffer (IB at a 256-byte aligned offset), staged through the slot's
// persistently mapped ring. Was 4 CreateCommittedResource per mesh (VB, IB, 2 staging) = 4-8 ms
// per spline / SCO on a 1 GHz CPU even for a few hundred bytes. Now 1 (2 if the ring is full).
// A range of `total` bytes (256-aligned) in one of the pool's 64 MB arenas; null when it does not fit one.
std::shared_ptr<VbArenaAlloc> vb_pool_alloc(ID3D12Device* device, const std::shared_ptr<VbArenaPool>& pool,
                                            UINT64 total) {
    constexpr UINT64 kArena = 64ull << 20;
    const UINT64 size = (total + 255ull) & ~255ull;
    if (!pool || size > kArena / 4) return nullptr;
    std::lock_guard<std::mutex> lk(pool->mu);
    pool->reclaim_locked(3); // (HelloO3dRenderer::kFrameCount, private here)
    for (std::uint32_t ai = 0;; ++ai) {
        if (ai == pool->arenas.size()) {
            VbArenaPool::Arena a;
            const CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
            const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(kArena);
            if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(a.buf.GetAddressOf()))))
                return nullptr;
            a.cap = kArena;
            a.free_ranges[0] = kArena;
            a.max_free = kArena;
            pool->arenas.push_back(std::move(a));
        }
        auto& a = pool->arenas[ai];
        if (a.max_free < size) continue;
        UINT64 largest = 0;
        for (auto it = a.free_ranges.begin(); it != a.free_ranges.end(); ++it) {
            if (it->second < size) {
                largest = (std::max)(largest, it->second);
                continue;
            }
            const UINT64 off = it->first, rest = it->second - size;
            a.free_ranges.erase(it);
            if (rest > 0) a.free_ranges[off + size] = rest;
            // (max_free stays an upper bound: only a range shrank; a scan that finds nothing makes it exact)
            pool->bytes_used += size;
            auto out = std::make_shared<VbArenaAlloc>();
            out->pool = pool;
            out->arena = ai;
            out->off = off;
            out->size = size;
            return out;
        }
        a.max_free = largest; // (looked at every range: exact)
    }
}

template <class Slot>
void upload_vb_ib(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, Slot& slot, const void* vdata,
                  UINT vsize, const void* idata, UINT isize, GpuMeshChunk& chunk,
                  const std::shared_ptr<VbArenaPool>& pool = nullptr) {
    const UINT64 ib_off = (static_cast<UINT64>(vsize) + 255ull) & ~255ull;
    const UINT64 total = ib_off + isize;
    const CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    Microsoft::WRL::ComPtr<ID3D12Resource> buf;
    const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(total);
    auto sub = vb_pool_alloc(device, pool, total);
    const UINT64 dst_off = sub ? sub->off : 0;
    if (sub) {
        std::lock_guard<std::mutex> lk(pool->mu);
        buf = pool->arenas[sub->arena].buf;
    } else {
        throw_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(buf.GetAddressOf())),
                 "vb/ib buffer");
    }
    constexpr UINT64 kRingCap = 2ull << 20;
    if (!slot.ring && total <= kRingCap) {
        const CD3DX12_RESOURCE_DESC rdesc = CD3DX12_RESOURCE_DESC::Buffer(kRingCap);
        if (SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(slot.ring.GetAddressOf())))) {
            void* mapped = nullptr;
            if (SUCCEEDED(slot.ring->Map(0, nullptr, &mapped))) {
                slot.ring_ptr = static_cast<std::uint8_t*>(mapped);
                slot.ring_cap = kRingCap;
                slot.ring_off = 0;
            } else {
                slot.ring.Reset();
            }
        }
    }
    ID3D12Resource* src = nullptr;
    UINT64 src_off = 0;
    std::uint8_t* dst = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    const UINT64 ring_at = (slot.ring_off + 255ull) & ~255ull;
    if (slot.ring && slot.ring_ptr && ring_at + total <= slot.ring_cap) {
        src = slot.ring.Get();
        src_off = ring_at;
        dst = slot.ring_ptr + ring_at;
        slot.ring_off = ring_at + total;
    } else {
        throw_hr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(staging.GetAddressOf())),
                 "vb/ib staging");
        void* mapped = nullptr;
        throw_hr(staging->Map(0, nullptr, &mapped), "vb/ib staging map");
        dst = static_cast<std::uint8_t*>(mapped);
        src = staging.Get();
    }
    std::memcpy(dst, vdata, vsize);
    std::memcpy(dst + ib_off, idata, isize);
    if (staging) staging->Unmap(0, nullptr);
    cmd->CopyBufferRegion(buf.Get(), dst_off, src, src_off, total);
    if (!sub) {
        D3D12_RESOURCE_BARRIER barrier =
            dx12_transition_barrier(buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        cmd->ResourceBarrier(1, &barrier);
        slot.keep_alive.push_back(buf);
    }
    // (an arena: a buffer, promoted to COPY_DEST by the copy on the copy queue and back to COMMON when that
    //  list is done; the graphics queue reads it from COMMON by implicit promotion, as before)
    if (staging) slot.keep_alive.push_back(std::move(staging));
    chunk.vertex_buffer = buf;
    chunk.index_buffer = buf;
    chunk.vb_alloc = std::move(sub);
    const D3D12_GPU_VIRTUAL_ADDRESS va = buf->GetGPUVirtualAddress() + dst_off;
    chunk.vertex_buffer_view.BufferLocation = va;
    chunk.vertex_buffer_view.StrideInBytes = sizeof(omsi::o3d::Vertex);
    chunk.vertex_buffer_view.SizeInBytes = vsize;
    chunk.index_buffer_view.BufferLocation = va + ib_off;
    chunk.index_buffer_view.SizeInBytes = isize;
    chunk.index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
}
} // namespace


bool HelloO3dRenderer::upload_mesh_gpu(const MappedO3D& source, const ModelCfgMeshEntry* cfg_mesh,
                                       const std::vector<omsi::o3d::Vertex>* vertex_override) {
    auto& d = *impl_;
    const auto& mesh = source.mesh();

    GpuMeshChunk chunk{};
    std::vector<std::optional<std::filesystem::path>> batch_texture_paths;
    struct PendingBatch {
        MaterialDrawBatch batch;
        std::optional<std::filesystem::path> texture_path;
        std::optional<std::filesystem::path> envmap_path;
        std::optional<std::filesystem::path> nightmap_path; // matl_nightmap -> slot+2 (t4)
        // [matl_transmap] of a see-through material: ITS alpha is the transparency (the main texture's alpha is
        // only the reflection mask) - the albedo goes up with this alpha in place of its own
        std::optional<std::filesystem::path> transmap_path;
        std::uint16_t material_id = 0;
        std::string embedded_texture;
        std::string cfg_tex;
        std::string resolved_path;
        bool log_material = false;
        std::uint32_t srv_slot = UINT32_MAX;
    };
    std::vector<PendingBatch> pending_batches;

    // Y↔Z draw matrix (0x79) has det < 0 → swap winding so Cull BACK shows exteriors.
    const bool flip_winding = omsi::o3d::o3d_draw_matrix_flips_winding(mesh);
    auto push_tri = [&](std::vector<std::uint32_t>& bucket, std::uint32_t i0, std::uint32_t i1,
                        std::uint32_t i2) {
        bucket.push_back(i0);
        if (flip_winding) {
            bucket.push_back(i2);
            bucket.push_back(i1);
        } else {
            bucket.push_back(i1);
            bucket.push_back(i2);
        }
    };

    std::map<std::uint16_t, std::vector<std::uint32_t>> indices_by_material;
    if (mesh.long_triangle_indices) {
        const auto tris = mesh.triangle_u32_span();
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            auto& bucket = indices_by_material[tris[t].material];
            push_tri(bucket, tris[t].i0, tris[t].i1, tris[t].i2);
        }
    } else {
        const auto tris = mesh.triangle_u16_span();
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            auto& bucket = indices_by_material[tris[t].material];
            push_tri(bucket, tris[t].i0, tris[t].i1, tris[t].i2);
        }
    }

    std::vector<std::uint32_t> flat_indices;
    flat_indices.reserve(static_cast<std::size_t>(mesh.triangle_count) * 3u);
    const auto materials = mesh.parse_materials();
    const auto model_dir = source.path().parent_path();
    const auto& base_file = source.path(); // Blender find_image_path base (.o3d)

    auto tex_key = [](std::string_view s) {
        std::string out(s);
        std::replace(out.begin(), out.end(), '\\', '/');
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    // [matl] name + occurrence index among slots with that filename (not the global O3D slot).
    // routearrow_L: slot0="", slot1=Arrow.tga, slot2=text.bmp → [matl] text.bmp 0 binds slot 2.
    auto tex_occurrence = [&](std::uint16_t slot, std::string_view embedded) {
        const auto want = tex_key(embedded);
        int occ = 0;
        for (std::size_t i = 0; i < materials.size() && i < static_cast<std::size_t>(slot); ++i) {
            if (tex_key(materials[i].texture) == want) ++occ;
        }
        return occ;
    };
    auto cfg_mat_for_slot = [&](std::uint16_t slot, std::string_view embedded) -> const model_cfg::ModelCfgMaterial* {
        if (!cfg_mesh) return nullptr;
        const auto want = tex_key(embedded);
        const int occ = tex_occurrence(slot, embedded);
        const model_cfg::ModelCfgMaterial* placeholder = nullptr;
        const model_cfg::ModelCfgMaterial* tex_hit = nullptr;
        for (const auto& mat : cfg_mesh->materials) {
            if (mat.is_matl_change) continue;
            const auto cfgk = tex_key(mat.diffuse_texture);
            const bool ph = cfgk.empty() || mat.diffuse_texture.find("xxxxxxxxxx") != std::string::npos;
            if (ph) {
                if (mat.mat_id == static_cast<int>(slot) || mat.mat_id == occ) placeholder = &mat;
                continue;
            }
            if (cfgk != want) continue;
            const int idx = mat.mat_id < 0 ? 0 : mat.mat_id;
            if (idx == occ) {
                tex_hit = &mat;
                break;
            }
        }
        if (tex_hit) return tex_hit;
        // Fallback: same texture NAME ignoring folder/extension (.bmp vs .tga vs .png, "Texture\\x.bmp" vs "x.tga").
        // Without this a [matl_alpha] 1/2 block whose texture is spelled differently than in the .o3d never binds,
        // so the material (and its alpha mode) is silently dropped.
        if (!want.empty()) {
            auto stem_of = [&](const std::string& k) {
                std::string t = k;
                const auto sl = t.find_last_of('/');
                if (sl != std::string::npos) t.erase(0, sl + 1);
                const auto dt = t.find_last_of('.');
                if (dt != std::string::npos) t.erase(dt);
                return t;
            };
            const std::string want_stem = stem_of(want);
            const model_cfg::ModelCfgMaterial* stem_any = nullptr;
            for (const auto& mat : cfg_mesh->materials) {
                if (mat.is_matl_change) continue;
                const auto cfgk = tex_key(mat.diffuse_texture);
                if (cfgk.empty() || mat.diffuse_texture.find("xxxxxxxxxx") != std::string::npos) continue;
                if (stem_of(cfgk) != want_stem) continue;
                const int idx = mat.mat_id < 0 ? 0 : mat.mat_id;
                if (idx == occ) return &mat;
                if (!stem_any) stem_any = &mat;
            }
            // (only when the mesh uses that texture once: [matl] tex N is the N-th use of the texture, and
            // giving every use the block of the first turned a car's body into its own glass - wreck03)
            int stem_uses = 0;
            for (const auto& m : materials)
                if (stem_of(tex_key(m.texture)) == want_stem) ++stem_uses;
            if (stem_any && !placeholder && stem_uses <= 1) return stem_any;
        }
        if (placeholder) return placeholder;
        // One [matl] per mesh (WallAvenue glas vs tex). Never steal useTextTexture from slot 0
        // onto every other O3D material (Naechste Abfahrt / Autobahnschild — unique_tex hitch).
        const model_cfg::ModelCfgMaterial* only = nullptr;
        int nbase = 0;
        for (const auto& mat : cfg_mesh->materials) {
            if (mat.is_matl_change) continue;
            ++nbase;
            only = &mat;
        }
        // (only a [matl] that names no slot of the mesh may stand for the others: one that names its own
        //  slot - 3_stk_1's fence zaun1.dds with [matl_alpha] 1 - put its alpha test on every wall of the
        //  building, and only the windows were left)
        // (and only for a mesh of one slot, never a [useTextTexture] block: a car's number plate [matl] - its
        //  texture in no slot of the body .o3d - made the body and the wheels text planes, drawn blended
        //  without depth: the AI cars were wheels and a shadow, the buses had holes. Omsi.exe binds a [matl]
        //  only to the slot of its texture.)
        if (nbase == 1 && only && materials.size() <= 1 && !only->use_text_texture) {
            const auto ok = tex_key(only->diffuse_texture);
            auto stem_of2 = [](std::string t) {
                const auto sl = t.find_last_of('/');
                if (sl != std::string::npos) t.erase(0, sl + 1);
                const auto dt = t.find_last_of('.');
                if (dt != std::string::npos) t.erase(dt);
                return t;
            };
            for (const auto& m : materials)
                if (stem_of2(tex_key(m.texture)) == stem_of2(ok)) return nullptr;
            return only;
        }
        return nullptr;
    };

    for (const auto& [material_id, tri_indices] : indices_by_material) {
        if (tri_indices.empty()) continue;
        PendingBatch pending{};
        pending.batch.index_start = static_cast<std::uint32_t>(flat_indices.size());
        flat_indices.insert(flat_indices.end(), tri_indices.begin(), tri_indices.end());
        pending.batch.index_count = static_cast<std::uint32_t>(flat_indices.size()) - pending.batch.index_start;

        std::string_view embedded_texture;
        if (material_id < materials.size()) embedded_texture = materials[material_id].texture;

        const omsi::model_cfg::ModelCfgMaterial* cfg_mat = cfg_mat_for_slot(material_id, embedded_texture);

        std::string_view texture_name = embedded_texture;
        if (cfg_mat) {
            // OMSI often uses placeholder [matl] names (xxxxxxxxxx.png) only to attach
            // matl_alpha / noZwrite; the real diffuse lives in the O3D material string.
            const auto& cfg_tex = cfg_mat->diffuse_texture;
            const bool cfg_is_placeholder =
                cfg_tex.empty() || cfg_tex.find("xxxxxxxxxx") != std::string::npos;
            if (!cfg_is_placeholder &&
                (tex_key(cfg_tex) == tex_key(embedded_texture) || embedded_texture.empty())) {
                if (auto cfg_path = resolve_texture_path(base_file, cfg_tex))
                    texture_name = cfg_tex;
                // else keep embedded_texture (cfg path missing — do not blank the atlas)
            }
            pending.batch.alpha_mode = cfg_mat->alpha_mode;
            pending.batch.no_zwrite = cfg_mat->no_zwrite;
            pending.batch.no_zcheck = cfg_mat->no_zcheck;
            pending.batch.is_shadow = cfg_mat->is_shadow;
            for (const auto& om : cfg_mesh->materials) {
                const bool same_tex = tex_key(om.diffuse_texture) == tex_key(cfg_mat->diffuse_texture);
                const bool same_id = om.mat_id == cfg_mat->mat_id;
                // (a repeated [matl] of the SAME slot only: matched by the index alone, one material's
                //  [matl_noZwrite] / [matl_alpha] 2 - a car's glass, "tex 0" - reached every other "tex 0"
                //  of the mesh and the body was drawn as glass)
                if (!(same_tex && same_id)) continue;
                if (static_cast<int>(om.alpha_mode) > static_cast<int>(pending.batch.alpha_mode))
                    pending.batch.alpha_mode = om.alpha_mode;
                pending.batch.no_zwrite = pending.batch.no_zwrite || om.no_zwrite;
                pending.batch.no_zcheck = pending.batch.no_zcheck || om.no_zcheck;
            }
            apply_omsi_material_state(pending.batch.alpha_mode, pending.batch.no_zwrite,
                                        pending.batch.no_zcheck);
            if (cfg_mesh && cfg_mesh->on_surface) {
                pending.batch.is_on_surface = true;
                pending.batch.no_zwrite = true;
                if (pending.batch.alpha_mode == MatlAlphaMode::opaque)
                    pending.batch.alpha_mode = MatlAlphaMode::alpha_blend;
            }
            if (cfg_mesh && cfg_mesh->presurface) {
                pending.batch.is_presurface = true;
                // Unit126 TMap.Render pass 0: normal Z. Hole must write Z so terrain (pass 1) misses the opening.
                pending.batch.no_zcheck = false;
                pending.batch.no_zwrite = false;
            }
            if (cfg_mat->envmap_texture && cfg_mat->envmap_strength > 0.f) {
                pending.batch.has_envmap = true;
                pending.batch.envmap_strength = cfg_mat->envmap_strength;
                pending.batch.env_alpha_mask = cfg_mat->transmap_texture.has_value() ||
                                               cfg_mat->envmap_mask_texture.has_value();
            }
            if (cfg_mat->texcoord_trans_x_var) pending.batch.texcoord_trans_x_var = *cfg_mat->texcoord_trans_x_var;
            if (cfg_mat->texcoord_trans_y_var) pending.batch.texcoord_trans_y_var = *cfg_mat->texcoord_trans_y_var;
            if (cfg_mat->alpha_scale_var) pending.batch.alpha_scale_var = *cfg_mat->alpha_scale_var;
            if (cfg_mat->use_text_texture) {
                pending.batch.is_text_texture = true;
                pending.batch.text_texture_index = *cfg_mat->use_text_texture;
                if (pending.batch.alpha_mode == MatlAlphaMode::opaque)
                    pending.batch.alpha_mode = MatlAlphaMode::alpha_test;
                static int tt_bind_log = 0;
                if (tt_bind_log < 8) {
                    omsi::log::info("useTextTexture mesh=" + source.path().filename().string() +
                                    " slot=" + std::to_string(material_id) + " occ=" +
                                    std::to_string(tex_occurrence(material_id, embedded_texture)) +
                                    " o3d=\"" + std::string(embedded_texture) + "\" cfg=\"" +
                                    cfg_mat->diffuse_texture + "\" idx=" + std::to_string(cfg_mat->mat_id) +
                                    " tt=" + std::to_string(*cfg_mat->use_text_texture) +
                                    " alpha=" + std::to_string(static_cast<int>(pending.batch.alpha_mode)));
                    ++tt_bind_log;
                }
            }
            if (cfg_mat->freetex_label) {
                pending.batch.is_freetex = true;
                pending.batch.freetex_key = cfg_mat->texture_key;
            }
        }
        // [matl_change] without a base [matl] (Florito R17): still unique SRV per slot.
        if (cfg_mesh) {
            // A [matl_change] names its slot as [matl] does: the texture and which of the slots with that
            // texture (0 = first). Matched by the number alone, HVV_HST_Unterstand's two changes
            // (werbung1.bmp 0, HH_Haltestelle.png 0) both took slot 0 - the poster - and fought over it,
            // the poster showing the stop's whole texture atlas.
            const int occ = tex_occurrence(material_id, embedded_texture);
            const auto emb_key = tex_key(embedded_texture);
            for (const auto& m : cfg_mesh->materials) {
                if (!m.is_matl_change || m.change_var.empty()) continue;
                const auto mkey = tex_key(m.diffuse_texture);
                bool named_in_o3d = false;
                for (const auto& om : materials)
                    if (tex_key(om.texture) == mkey) named_in_o3d = true;
                const bool hit = named_in_o3d ? (mkey == emb_key && m.mat_id == occ)
                                              : (m.mat_id == static_cast<int>(material_id));
                if (!hit) continue;
                pending.batch.is_freetex = true;
                pending.batch.freetex_key = "mc:" + mkey + "#" + std::to_string(m.mat_id);
                pending.batch.original_albedo = std::string(embedded_texture);
                break;
            }
        }
        // [CTCTexture] slot: the object's script picks the scheme (texture) at run time
        if (cfg_mesh && !cfg_mesh->ctc_defaults.empty()) {
            const auto emb_key = tex_key(embedded_texture);
            for (const auto& def : cfg_mesh->ctc_defaults) {
                if (tex_key(def) != emb_key) continue;
                pending.batch.is_freetex = true;
                pending.batch.freetex_key = "ctc:" + emb_key;
                pending.batch.original_albedo = std::string(embedded_texture);
                break;
            }
        }
        if (cfg_mesh && cfg_mesh->is_shadow_mesh) pending.batch.is_shadow = true;
        if (cfg_mesh && cfg_mesh->on_surface) {
            pending.batch.is_on_surface = true;
            pending.batch.no_zwrite = true;
            if (pending.batch.alpha_mode == MatlAlphaMode::opaque)
                pending.batch.alpha_mode = MatlAlphaMode::alpha_blend;
        }
        if (cfg_mesh && cfg_mesh->presurface) {
            pending.batch.is_presurface = true;
            pending.batch.no_zcheck = false;
            pending.batch.no_zwrite = false;
        }

        if (cfg_mesh) {
            pending.log_material = true;
            pending.material_id = material_id;
            pending.embedded_texture = std::string(embedded_texture);
            pending.cfg_tex = cfg_mat ? cfg_mat->diffuse_texture : "(o3d only)";
            if (!texture_name.empty()) {
                if (auto p = resolve_texture_path(base_file, texture_name))
                    pending.resolved_path = p->string();
                else
                    pending.resolved_path = "(none)";
            } else {
                pending.resolved_path = "(none)";
            }
        }

        if (!texture_name.empty())
            pending.texture_path = resolve_texture_path(base_file, texture_name);
        else if (material_id < materials.size() && !materials[material_id].texture.empty())
            pending.texture_path = resolve_texture_path(base_file, materials[material_id].texture);

        if (pending.batch.has_envmap && cfg_mat && cfg_mat->envmap_texture)
            pending.envmap_path = resolve_texture_path(base_file, *cfg_mat->envmap_texture);
        if (cfg_mat && cfg_mat->nightmap_texture)
            pending.nightmap_path = resolve_texture_path(base_file, *cfg_mat->nightmap_texture);
        // [matl_transmap]: Omsi.exe's material setter (0x7fd9b0) switches gMatlTransMapOn for it; per the OMSI SDK
        // the transmap's alpha is the transparency and the texture's own alpha stays the reflection mask.
        // Bound as a texture of its own (slot+3 = t5), so the base alpha still masks the envmap per texel
        // and a [matl_change] / CTC paint swap of t0 keeps the transparency.
        if (cfg_mat && cfg_mat->transmap_texture && !cfg_mat->transmap_texture->empty() && pending.texture_path &&
            !pending.batch.is_text_texture)
            pending.transmap_path = resolve_texture_path(base_file, *cfg_mat->transmap_texture);
        // [matl_lightmap] (also the [matl_item] of a "NightlightA" [matl_change]: the lit posters and
        // windows of a stop at night): Omsi.exe lays it on the vertex light with ADDSMOOTH before the
        // texture (light + map x (1 - light)) - the texture glows where the map is bright, in the dark;
        // by day it hardly shows. Bound in the night slot, flagged as a light map.
        const omsi::model_cfg::ModelCfgMaterial* lm_mat = (cfg_mat && cfg_mat->lightmap_texture) ? cfg_mat : nullptr;
        if (!lm_mat && cfg_mesh) {
            // (a [matl_change] slot: cfg_mat_for_slot leaves changes out; matched by texture + occurrence)
            const int occ_lm = tex_occurrence(material_id, embedded_texture);
            const auto emb_lm = tex_key(embedded_texture);
            for (const auto& m : cfg_mesh->materials) {
                if (!m.is_matl_change || !m.lightmap_texture) continue;
                if (tex_key(m.diffuse_texture) != emb_lm || (m.mat_id < 0 ? 0 : m.mat_id) != occ_lm) continue;
                lm_mat = &m;
                break;
            }
        }
        if (!pending.nightmap_path && lm_mat) {
            pending.nightmap_path = resolve_texture_path(base_file, *lm_mat->lightmap_texture);
            if (pending.nightmap_path) pending.batch.lightmap_mode = true;
            if (pending.nightmap_path && lm_mat->lightmap_var) {
                pending.batch.lightmap_var = *lm_mat->lightmap_var;
                pending.batch.lightmap_scale = 0.f;
            }
        }
        // OMSI's own night textures (openOMSI night_texture_name): the same file in a `night` folder beside
        // the texture - black but for lit windows / signs - added at night. Every stock building has them.
        if (!pending.nightmap_path && pending.texture_path && !pending.texture_path->empty()) {
            const auto& tp = *pending.texture_path;
            const auto night_dir = tp.parent_path() / L"night";
            auto night = night_dir / tp.filename();
            if (!texture_detail::path_exists(night)) night = night_dir / (tp.stem().wstring() + L".dds");
            if (!texture_detail::path_exists(night)) {
                // the .sco may name the .bmp while the folder holds the .dds (or the other way round)
                night = night_dir / (tp.stem().wstring() + L".bmp");
            }
            if (texture_detail::path_exists(night)) pending.nightmap_path = night;
        }
        if (material_id < materials.size()) {
            const auto& em = materials[material_id].fixed.emission;
            auto q = [](float v) {
                if (!(v > 0.f)) return 0u;
                return static_cast<unsigned>((std::min)(v, 1.f) * 255.f + 0.5f);
            };
            const unsigned packed = q(em[0]) | (q(em[1]) << 8) | (q(em[2]) << 16);
            if (packed != 0u) pending.batch.emissive_packed = static_cast<float>(packed);
        }

        pending_batches.push_back(std::move(pending));
    }

    if (pending_batches.empty() && mesh.triangle_count > 0) {
        PendingBatch pending{};
        pending.batch.index_start = 0;
        if (mesh.long_triangle_indices) {
            const auto tris = mesh.triangle_u32_span();
            for (std::uint32_t t = 0; t < mesh.triangle_count; ++t)
                push_tri(flat_indices, tris[t].i0, tris[t].i1, tris[t].i2);
        } else {
            const auto tris = mesh.triangle_u16_span();
            for (std::uint32_t t = 0; t < mesh.triangle_count; ++t)
                push_tri(flat_indices, tris[t].i0, tris[t].i1, tris[t].i2);
        }
        pending.batch.index_count = static_cast<std::uint32_t>(flat_indices.size());
        pending_batches.push_back(std::move(pending));
    }

    if (flat_indices.empty()) return false;

    // Decrypt only if obfuscated; Y↔Z only if 0x79. Bbox + pick in the same pass.
    std::vector<omsi::o3d::Vertex> gpu_verts;
    if (vertex_override && !vertex_override->empty()) {
        gpu_verts = *vertex_override;
    } else if (const auto* cached = omsi::o3d::prepared_vertex_cache().try_get(source.path())) {
        gpu_verts = *cached;
    } else {
        omsi::o3d::prepare_o3d_mesh_vertices_for_gpu(mesh, gpu_verts);
        omsi::o3d::prepared_vertex_cache().store(source.path(), gpu_verts);
        if (omsi::o3d::o3d_needs_vertex_decrypt(mesh)) {
            static std::mutex log_mu;
            static std::unordered_set<std::string> logged;
            const auto fn = source.path().filename().string();
            bool first = false;
            {
                std::lock_guard<std::mutex> lk(log_mu);
                first = logged.insert(fn).second;
            }
            if (first)
                omsi::log::info("o3d decrypted \"" + fn + "\" verts=" +
                                std::to_string(gpu_verts.size()) +
                                " key=" + std::to_string(mesh.encryption_key));
        }
    }

    chunk.meta.visible = true;
    if (cfg_mesh) {
        chunk.meta.mesh_file = cfg_mesh->mesh_file; chunk.content_mesh = -1;
        chunk.meta.visible_var = cfg_mesh->visible_var;
        chunk.meta.visible_required = cfg_mesh->visible_default;
        chunk.meta.mouseevent_var = cfg_mesh->mouseevent_var;
        for (const int li : cfg_mesh->illumination_interior)
            if (li >= 0) chunk.interior = true; // (-1 -1 -1 -1: the body - no lamp lights it)
    } else {
        chunk.meta.mesh_file = source.path().filename().string(); chunk.content_mesh = -1;
    }
    chunk.pick_positions.reserve(gpu_verts.size());
    if (!gpu_verts.empty()) {
        chunk.meta.bbox_min[0] = chunk.meta.bbox_max[0] = gpu_verts[0].position[0];
        chunk.meta.bbox_min[1] = chunk.meta.bbox_max[1] = gpu_verts[0].position[1];
        chunk.meta.bbox_min[2] = chunk.meta.bbox_max[2] = gpu_verts[0].position[2];
        for (const auto& gv : gpu_verts) {
            chunk.meta.bbox_min[0] = (std::min)(chunk.meta.bbox_min[0], gv.position[0]);
            chunk.meta.bbox_min[1] = (std::min)(chunk.meta.bbox_min[1], gv.position[1]);
            chunk.meta.bbox_min[2] = (std::min)(chunk.meta.bbox_min[2], gv.position[2]);
            chunk.meta.bbox_max[0] = (std::max)(chunk.meta.bbox_max[0], gv.position[0]);
            chunk.meta.bbox_max[1] = (std::max)(chunk.meta.bbox_max[1], gv.position[1]);
            chunk.meta.bbox_max[2] = (std::max)(chunk.meta.bbox_max[2], gv.position[2]);
            chunk.pick_positions.push_back({gv.position[0], gv.position[1], gv.position[2]});
        }
    }
    chunk.pick_indices = flat_indices;
    if (cfg_mesh && cfg_mesh->smooth_skin && !gpu_verts.empty())
        chunk.skin_base = std::make_shared<std::vector<omsi::o3d::Vertex>>(gpu_verts);

    const UINT vertex_buffer_size = static_cast<UINT>(gpu_verts.size() * sizeof(omsi::o3d::Vertex));
    const UINT index_buffer_size = static_cast<UINT>(flat_indices.size() * sizeof(std::uint32_t));

    // COPY-queue batch (same as append_textured_mesh): graphics Wait GPU-side in flush_proc_uploads.
    // Never wait_gpu_idle here — that froze the UI 5–13 s after SA spawn.
    std::lock_guard<std::mutex> upload_lock(g_o3d_upload_mutex);
    if (!begin_proc_upload()) return false;
    {
        auto& slot = d.proc_slots[d.proc_slot_i];
        upload_vb_ib(d.device.Get(), d.proc_command_list.Get(), slot, gpu_verts.data(), vertex_buffer_size, flat_indices.data(),
                     index_buffer_size, chunk, d.vb_pool);
        ++d.proc_pending;
    }

    // VB/IB views were set by upload_vb_ib (IB lives at an offset in the same buffer).

    if (d.mesh_chunks.empty()) {
        omsi::log::info("gpu v0 pos=(" + std::to_string(gpu_verts[0].position[0]) + "," +
                        std::to_string(gpu_verts[0].position[1]) + "," +
                        std::to_string(gpu_verts[0].position[2]) + ") uv=(" +
                        std::to_string(gpu_verts[0].uv[0]) + "," + std::to_string(gpu_verts[0].uv[1]) +
                        ") flip_winding=" + std::to_string(flip_winding ? 1 : 0));
    }

    // VB/IB on DEFAULT heap (P7). Textures still use upload_command_list.
    bool needs_tex_upload = false;
    for (const auto& pending : pending_batches) {
        if (!pending.texture_path || pending.batch.is_text_texture) continue;
        const std::string key =
            pending.texture_path->string() +
            (pending.envmap_path ? ("#env#" + pending.envmap_path->string()) : "#env#white") +
            (pending.nightmap_path ? ("#night#" + pending.nightmap_path->string()) : std::string{}) +
            (pending.transmap_path ? ("#trans#" + pending.transmap_path->string()) : std::string{});
        if (d.texture_cache.find(key) == d.texture_cache.end()) {
            needs_tex_upload = true;
            break;
        }
    }

    if (needs_tex_upload) {
        if (!try_reset_upload_allocator()) {
            needs_tex_upload = false;
        } else {
            throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "upload cmd reset");
        }
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE white_srv_cpu =
        offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);

    auto load_into_slot = [&](std::uint32_t slot, const std::filesystem::path& path) -> bool {
        const std::string key = path.string();
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
        const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu =
            offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);

        if (const auto it = d.texture_slot_cache.find(key); it != d.texture_slot_cache.end()) {
            const D3D12_CPU_DESCRIPTOR_HANDLE src_cpu = offset_cpu_handle(
                d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + it->second);
            d.device->CopyDescriptorsSimple(1, dst_cpu, src_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            d.texture_cache[key] = dst_gpu;
            return true;
        }

        if (!needs_tex_upload) return false;
        try {
            const auto tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, path);
            if (!tex.valid) return false;
            d.texture_by_slot[slot] = tex.resource;
            if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
            d.texture_slot_files[slot] = path;
            d.texture_slot_cache[key] = slot;
            d.texture_cache[key] = dst_gpu;
            return true;
        } catch (...) {
            return false;
        }
    };

    auto assign_batch_textures = [&](PendingBatch& pending) {
        auto& batch = pending.batch;
        if (!pending.texture_path && !batch.is_text_texture) return;

        const std::string key =
            pending.texture_path
                ? (pending.texture_path->string() +
                   (pending.envmap_path ? ("#env#" + pending.envmap_path->string()) : "#env#white") +
                   (pending.nightmap_path ? ("#night#" + pending.nightmap_path->string()) : std::string{}) +
                   (pending.transmap_path ? ("#trans#" + pending.transmap_path->string()) : std::string{}))
                : std::string{};

        // Reuse existing descriptor pair — do not burn new SRV slots per instance.
        // [useTextTexture] must be unique: live OFT raster overwrites this SRV.
        const bool unique_srv = batch.is_text_texture || batch.is_freetex;
        if (!unique_srv && !key.empty()) {
            if (const auto it = d.texture_cache.find(key); it != d.texture_cache.end()) {
                batch.srv_gpu = it->second;
                batch.has_texture = true;
                batch.has_transmap = pending.transmap_path &&
                                     d.texture_slot_cache.count(pending.transmap_path->string()) > 0;
                // Same texture pair => same night slot; only valid if that night map actually loaded.
                batch.has_nightmap = pending.nightmap_path &&
                                     d.texture_slot_cache.count(pending.nightmap_path->string()) > 0;
                return;
            }
        }

        const std::uint32_t nslots = pending.transmap_path ? 4u : (pending.nightmap_path ? 3u : 2u);
        if (d.srv_next + nslots > kSrvDescriptorCount) {
            if (!d.srv_exhausted_logged) {
                omsi::log::warn("SRV heap full (" + std::to_string(d.srv_next) + "/" +
                                std::to_string(kSrvDescriptorCount) + ") — textures go white");
                d.srv_exhausted_logged = true;
            }
            return;
        }

        const std::uint32_t base_slot = d.srv_next;
        d.srv_next += nslots;

        if (pending.texture_path && !batch.is_text_texture) {
            if (!load_into_slot(base_slot, *pending.texture_path)) {
                // (unreadable: white, as Omsi.exe draws a material whose texture failed - left unset, the
                //  batch's empty descriptor showed whatever lay at the start of the heap: grass, leaves)
                d.device->CopyDescriptorsSimple(
                    1,
                    offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                      kCbvDescriptorCount + base_slot),
                    white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                if (!needs_tex_upload) {
                    // (every texture upload list was in flight: not unreadable, just not now - it went white
                    //  for good; it is loaded into this slot by pump_late_textures)
                    d.late_textures.push_back({base_slot, *pending.texture_path});
                } else {
                    omsi::log::warn("texture unreadable: " + pending.texture_path->string());
                }
            }
        } else {
            d.device->CopyDescriptorsSimple(
                1,
                offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                  kCbvDescriptorCount + base_slot),
                white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        if (pending.envmap_path)
            load_into_slot(base_slot + 1, *pending.envmap_path);
        else
            d.device->CopyDescriptorsSimple(
                1,
                offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                  kCbvDescriptorCount + base_slot + 1),
                white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Night map at slot+2 (t4): only real when the load succeeds; the shader gates it by flag bit 4.
        batch.has_nightmap = false;
        if (pending.nightmap_path) batch.has_nightmap = load_into_slot(base_slot + 2, *pending.nightmap_path);
        // the table is contiguous: a transmap batch without a night map still needs t4 filled
        if (pending.transmap_path && !batch.has_nightmap)
            d.device->CopyDescriptorsSimple(
                1,
                offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                  kCbvDescriptorCount + base_slot + 2),
                white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        batch.has_transmap = false;
        if (pending.transmap_path) {
            batch.has_transmap = load_into_slot(base_slot + 3, *pending.transmap_path);
            if (!batch.has_transmap) omsi::log::warn("transmap unreadable: " + pending.transmap_path->string());
        }
        batch.srv_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, base_slot);
        batch.has_texture = true;
        batch.srv_slot = base_slot;
        pending.srv_slot = base_slot;
        if (!unique_srv && !key.empty()) d.texture_cache[key] = batch.srv_gpu;
    };

    for (auto& pending : pending_batches) {
        assign_batch_textures(pending);
        auto& batch = pending.batch;
        // [matl_alpha] 1: Omsi.exe 0x800044 sets ALPHATESTENABLE, ALPHAREF 0x80, ALPHAFUNC GREATER - a texel
        // shows when its alpha > 128/255 (the shader keeps a >= ref)
        batch.alpha_ref = 128.5f / 255.f;

        if (pending.log_material && cfg_mesh) {
            // Once per mesh_file+slot — trees were flooding the log (and CPU) per instance.
            static std::unordered_set<std::string> s_logged_mat;
            const std::string mk = cfg_mesh->mesh_file + "#" + std::to_string(pending.material_id) + "#" +
                                   pending.resolved_path;
            if (s_logged_mat.size() < 48 && s_logged_mat.insert(mk).second) {
                omsi::log::info("mat " + cfg_mesh->mesh_file + " slot=" + std::to_string(pending.material_id) +
                                " o3d_tex=\"" + pending.embedded_texture + "\" cfg_tex=\"" + pending.cfg_tex +
                                "\" -> \"" + pending.resolved_path + "\"" +
                                (batch.has_texture ? " srv=" + std::to_string(pending.srv_slot) + " bound=1"
                                                   : " bound=0") +
                                (batch.is_shadow ? " [shadow]" : "") +
                                " alpha=" + std::to_string(static_cast<int>(batch.alpha_mode)) +
                                " noz=" + std::to_string(batch.no_zwrite ? 1 : 0));
            }
        }

        if (batch.is_shadow) {
            if (batch.alpha_mode == MatlAlphaMode::opaque) batch.alpha_mode = MatlAlphaMode::alpha_blend;
            batch.no_zwrite = true;
            batch.no_zcheck = false;
            chunk.shadow_batches.push_back(batch);
        } else if (batch.is_text_texture) {
            // a text plane lies in the plane of its sign or arrow; Omsi.exe draws the model slot by slot
            // with LESSEQUAL, so the later slot - the text - is seen. Drawn last, blended, pulled a little
            // towards the eye (openOMSI draw_order_level), instead of losing the tie to the board.
            batch.alpha_mode = MatlAlphaMode::alpha_blend;
            batch.no_zwrite = true;
            chunk.alpha_nozwrite_batches.push_back(batch);
        } else if (batch.alpha_mode == MatlAlphaMode::alpha_blend) {
            if (batch.no_zwrite)
                chunk.alpha_nozwrite_batches.push_back(batch);
            else
                chunk.alpha_batches.push_back(batch);
        } else if (batch.alpha_mode == MatlAlphaMode::alpha_test) {
            chunk.alpha_batches.push_back(batch);
        } else
            chunk.opaque_batches.push_back(batch);
    }

    if (needs_tex_upload) {
        throw_hr(d.upload_command_list->Close(), "upload cmd close");
        ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        const UINT64 upload_fence = d.fence_value++;
        throw_hr(d.queue->Signal(d.fence.Get(), upload_fence), "upload Signal");
        d.pending_tex_upload_fence = upload_fence;
    }

    // Presurface draw pass is driven by sco render_phase / batch flags, not whole-chunk upload.
    auto batch_has_uv = [](const std::vector<MaterialDrawBatch>& batches) {
        for (const auto& b : batches) {
            if (!b.texcoord_trans_x_var.empty() || !b.texcoord_trans_y_var.empty() || !b.alpha_scale_var.empty() ||
                !b.lightmap_var.empty())
                return true;
        }
        return false;
    };
    chunk.has_uv_vars = batch_has_uv(chunk.opaque_batches) || batch_has_uv(chunk.alpha_batches) ||
                        batch_has_uv(chunk.alpha_nozwrite_batches) || batch_has_uv(chunk.shadow_batches);
    d.mesh_chunks.push_back(std::move(chunk));
    DirectX::XMStoreFloat4x4(&d.mesh_chunks.back().local_world_transposed, DirectX::XMMatrixIdentity());
    return true;
}

bool HelloO3dRenderer::load_o3d(const MappedO3D& source, const ModelCfgMeshEntry* cfg_mesh) {
    if (impl_) scene_dirty(*impl_, "load_o3d");
    if (!impl_ || !source.valid()) return false;
    try {
        impl_->mesh_chunks.clear();
        impl_->texture_resources.clear();
        impl_->texture_cache.clear();
        impl_->texture_slot_cache.clear();
        impl_->srv_next = 1;
        return upload_mesh_gpu(source, cfg_mesh);
    } catch (const std::exception& ex) {
        omsi::log::error(std::string("load_o3d failed: ") + ex.what());
        return false;
    }
}

bool HelloO3dRenderer::begin_proc_upload() {
    if (!impl_) return false;
    auto& d = *impl_;
    if (!d.device || !d.copy_queue || !d.copy_fence || !d.copy_fence_event) return false;
    if (d.proc_open) return true;
    std::uint32_t chosen = Impl::kProcUploadSlots;
    for (std::uint32_t n = 0; n < Impl::kProcUploadSlots; ++n) {
        const std::uint32_t i = (d.proc_slot_i + n) % Impl::kProcUploadSlots;
        auto& cand = d.proc_slots[i];
        if (cand.fence_value != 0 && d.copy_fence->GetCompletedValue() < cand.fence_value) continue;
        chosen = i;
        break;
    }
    if (chosen == Impl::kProcUploadSlots) return false;
    d.proc_slot_i = chosen;
    auto& slot = d.proc_slots[d.proc_slot_i];
    if (!slot.allocator) {
        throw_hr(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                  IID_PPV_ARGS(slot.allocator.GetAddressOf())),
                 "proc batch allocator");
    }
    slot.keep_alive.clear();
    slot.ring_off = 0; // fence passed: the GPU finished reading this slot's ring
    throw_hr(slot.allocator->Reset(), "proc batch alloc reset");
    if (!d.proc_command_list) {
        throw_hr(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, slot.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(d.proc_command_list.GetAddressOf())),
                 "proc batch list");
    } else {
        throw_hr(d.proc_command_list->Reset(slot.allocator.Get(), nullptr), "proc batch list reset");
    }
    d.proc_open = true;
    d.proc_pending = 0;
    return true;
}

void HelloO3dRenderer::flush_batched_mesh_uploads() { flush_proc_uploads(); }

int HelloO3dRenderer::pump_late_textures(int max_n) {
    if (!impl_ || max_n <= 0) return 0;
    auto& d = *impl_;
    if (d.late_textures.empty() || !d.device || !d.queue || !d.fence || !d.srv_heap) return 0;
    if (!try_reset_upload_allocator()) return 0;
    if (!d.upload_command_list || !d.upload_allocator) return 0;
    if (FAILED(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr))) return 0;
    int done = 0, uploaded = 0;
    std::size_t i = 0;
    for (; i < d.late_textures.size() && done < max_n; ++i) {
        const auto& lt = d.late_textures[i];
        ++done;
        const std::string key = lt.path.string();
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + lt.slot);
        const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, lt.slot);
        if (const auto it = d.texture_slot_cache.find(key); it != d.texture_slot_cache.end()) {
            const D3D12_CPU_DESCRIPTOR_HANDLE src_cpu = offset_cpu_handle(
                d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + it->second);
            d.device->CopyDescriptorsSimple(1, dst_cpu, src_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            continue;
        }
        try {
            const auto tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, lt.path);
            if (!tex.valid) {
                omsi::log::warn("texture unreadable: " + key);
                continue;
            }
            d.texture_by_slot[lt.slot] = tex.resource;
            if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
            d.texture_slot_files[lt.slot] = lt.path;
            d.texture_slot_cache[key] = lt.slot;
            ++uploaded;
        } catch (...) {
        }
    }
    d.late_textures.erase(d.late_textures.begin(), d.late_textures.begin() + static_cast<std::ptrdiff_t>(i));
    throw_hr(d.upload_command_list->Close(), "late tex close");
    ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 fv = d.fence_value++;
    throw_hr(d.queue->Signal(d.fence.Get(), fv), "late tex signal");
    d.pending_tex_upload_fence = fv;
    return uploaded;
}

void HelloO3dRenderer::flush_proc_uploads() {
    if (!impl_) return;
    auto& d = *impl_;
    if (!d.proc_open) return;
    d.proc_open = false;
    auto& slot = d.proc_slots[d.proc_slot_i];
    throw_hr(d.proc_command_list->Close(), "proc batch close");
    ID3D12CommandList* lists[] = {d.proc_command_list.Get()};
    d.copy_queue->ExecuteCommandLists(1, lists);
    const UINT64 fv = ++d.copy_fence_value;
    throw_hr(d.copy_queue->Signal(d.copy_fence.Get(), fv), "proc batch signal");
    slot.fence_value = fv;
    throw_hr(d.queue->Wait(d.copy_fence.Get(), fv), "proc batch gfx wait");
    d.proc_slot_i = (d.proc_slot_i + 1) % Impl::kProcUploadSlots;
    d.proc_pending = 0;
}

bool HelloO3dRenderer::append_o3d(const MappedO3D& source, const ModelCfgMeshEntry* cfg_mesh,
                                   const std::vector<omsi::o3d::Vertex>* vertex_override) {
    if (impl_) scene_dirty(*impl_, "append_o3d");
    if (!impl_ || !source.valid()) return false;
    try {
        return upload_mesh_gpu(source, cfg_mesh, vertex_override);
    } catch (const std::exception& ex) {
        omsi::log::error(std::string("append_o3d failed: ") + ex.what());
        return false;
    } catch (...) {
        omsi::log::error("append_o3d failed: unknown exception");
        return false;
    }
}

void HelloO3dRenderer::ensure_texture_files_resident(const std::vector<std::filesystem::path>& paths) {
    if (impl_) scene_dirty(*impl_, "ensure_texture_files_resident");
    if (!impl_ || paths.empty()) return;
    auto& d = *impl_;
    if (!d.device || !d.queue) return;

    const D3D12_CPU_DESCRIPTOR_HANDLE white_srv_cpu = offset_cpu_handle(
        d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);
    const D3D12_GPU_DESCRIPTOR_HANDLE white_srv_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, 0);

    std::lock_guard<std::mutex> upload_lock(g_o3d_upload_mutex);
    bool upload_open = false;
    auto close_upload = [&]() {
        if (!upload_open) return;
        try {
            throw_hr(d.upload_command_list->Close(), "warm tex close");
            ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
            d.queue->ExecuteCommandLists(1, lists);
            const UINT64 upload_fence = d.fence_value++;
            throw_hr(d.queue->Signal(d.fence.Get(), upload_fence), "warm tex Signal");
            d.pending_tex_upload_fence = upload_fence;
        } catch (...) {
        }
        upload_open = false;
    };

    for (const auto& texture_path : paths) {
        if (texture_path.empty()) continue;
        const std::string key = texture_path.string() + "#env#white";
        if (d.texture_cache.find(key) != d.texture_cache.end()) continue;
        if (d.srv_next + 2u > kSrvDescriptorCount) continue;
        if (!try_reset_upload_allocator()) continue;
        if (!upload_open) {
            throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "warm tex reset");
            upload_open = true;
        }
        const std::uint32_t slot = d.srv_next;
        d.srv_next += 2;
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
        const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
        try {
            const auto tex =
                load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, texture_path);
            if (!tex.valid) continue;
            d.texture_by_slot[slot] = tex.resource;
            if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
            d.texture_slot_files[slot] = texture_path;
            d.device->CopyDescriptorsSimple(
                1,
                offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                  kCbvDescriptorCount + slot + 1),
                white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            d.texture_slot_cache[texture_path.string()] = slot;
            d.texture_cache[key] = dst_gpu;
        } catch (...) {
        }
    }
    close_upload();
}

namespace {

void push_proc_draw_batch(GpuMeshChunk& chunk, const MaterialDrawBatch& batch_in) {
    MaterialDrawBatch batch = batch_in;
    if (batch.is_text_texture) { // (.x text plane: drawn last over its board, as for .o3d)
        batch.alpha_mode = MatlAlphaMode::alpha_blend;
        batch.no_zwrite = true;
        chunk.alpha_nozwrite_batches.push_back(batch);
    } else if (batch.alpha_mode == MatlAlphaMode::alpha_blend || batch.alpha_mode == MatlAlphaMode::alpha_test)
        chunk.alpha_batches.push_back(batch);
    else
        chunk.opaque_batches.push_back(batch);
    if (!batch.texcoord_trans_x_var.empty() || !batch.texcoord_trans_y_var.empty() || !batch.alpha_scale_var.empty())
        chunk.has_uv_vars = true;
}

void set_proc_batch_alpha(MaterialDrawBatch& batch, std::uint8_t alpha_mode, int text_texture_index) {
    batch.has_texture = false;
    if (alpha_mode == 2) {
        batch.alpha_mode = MatlAlphaMode::alpha_blend;
        batch.no_zwrite = false;
    } else if (alpha_mode == 1) {
        batch.alpha_mode = MatlAlphaMode::alpha_test;
        batch.no_zwrite = false;
    } else {
        batch.alpha_mode = MatlAlphaMode::opaque;
        batch.no_zwrite = false;
    }
    if (text_texture_index >= 0) {
        batch.is_text_texture = true;
        batch.text_texture_index = text_texture_index;
        if (batch.alpha_mode == MatlAlphaMode::opaque) batch.alpha_mode = MatlAlphaMode::alpha_test;
    }
}

} // namespace

void HelloO3dRenderer::bind_proc_texture(GpuMeshChunk& chunk, MaterialDrawBatch& batch,
                                           const std::filesystem::path& texture_path, std::uint8_t alpha_mode,
                                           int text_texture_index, const float* solid_rgba) {
    if (!impl_) return;
    auto& d = *impl_;
    set_proc_batch_alpha(batch, alpha_mode, text_texture_index);

    const D3D12_CPU_DESCRIPTOR_HANDLE white_srv_cpu = offset_cpu_handle(
        d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);
    const D3D12_GPU_DESCRIPTOR_HANDLE white_srv_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, 0);

    if (batch.is_text_texture && d.srv_next + 2u <= kSrvDescriptorCount) {
        const std::uint32_t slot = d.srv_next;
        d.srv_next += 2;
        d.device->CopyDescriptorsSimple(
            1,
            offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                              kCbvDescriptorCount + slot),
            white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        d.device->CopyDescriptorsSimple(
            1,
            offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                              kCbvDescriptorCount + slot + 1),
            white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        batch.srv_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
        batch.has_texture = true;
        batch.srv_slot = slot;
    } else if (!texture_path.empty()) {
        const std::string key = texture_path.string() + "#env#white";
        if (const auto it = d.texture_cache.find(key); it != d.texture_cache.end()) {
            batch.srv_gpu = it->second;
            batch.has_texture = true;
        } else if (d.srv_next + 2u <= kSrvDescriptorCount) {
            const std::uint32_t slot = d.srv_next;
            d.srv_next += 2;
            const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
                d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
            const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
            try {
                if (!try_reset_upload_allocator()) {
                    // every texture upload list in flight: white in its own slot for now, the texture goes into
                    // that slot a few frames later (pump_late_textures) - it stayed white until "Recargar"
                    d.device->CopyDescriptorsSimple(1, dst_cpu, white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    d.device->CopyDescriptorsSimple(
                        1,
                        offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                          kCbvDescriptorCount + slot + 1),
                        white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    d.late_textures.push_back({slot, texture_path});
                    batch.srv_gpu = dst_gpu;
                    batch.has_texture = true;
                    batch.srv_slot = slot;
                    d.texture_cache[key] = dst_gpu; // (the other strips with this texture share the slot)
                } else {
                    throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "proc tex cmd reset");
                    const auto tex =
                        load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, texture_path);
                    if (tex.valid) {
                        d.texture_by_slot[slot] = tex.resource;
                        if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
                        d.texture_slot_files[slot] = texture_path;
                        d.device->CopyDescriptorsSimple(
                            1,
                            offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                              kCbvDescriptorCount + slot + 1),
                            white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                        throw_hr(d.upload_command_list->Close(), "proc tex close");
                        ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
                        d.queue->ExecuteCommandLists(1, lists);
                        const UINT64 upload_fence = d.fence_value++;
                        throw_hr(d.queue->Signal(d.fence.Get(), upload_fence), "proc tex Signal");
                        d.pending_tex_upload_fence = upload_fence;
                        batch.srv_gpu = dst_gpu;
                        batch.has_texture = true;
                        batch.srv_slot = slot;
                        d.texture_slot_cache[texture_path.string()] = slot;
                        d.texture_cache[key] = dst_gpu;
                    } else {
                        throw_hr(d.upload_command_list->Close(), "proc tex close empty");
                        d.device->CopyDescriptorsSimple(1, dst_cpu, white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                        batch.srv_gpu = white_srv_gpu;
                        batch.has_texture = true;
                    }
                }
            } catch (...) {
                try {
                    d.upload_command_list->Close();
                } catch (...) {
                }
                batch.srv_gpu = white_srv_gpu;
                batch.has_texture = true;
            }
        } else if (!d.srv_exhausted_logged) {
            omsi::log::warn("SRV heap full (" + std::to_string(d.srv_next) + "/" +
                            std::to_string(kSrvDescriptorCount) + ") — textures go white");
            d.srv_exhausted_logged = true;
            batch.srv_gpu = white_srv_gpu;
            batch.has_texture = true;
        } else {
            batch.srv_gpu = white_srv_gpu;
            batch.has_texture = true;
        }
    } else {
        std::uint8_t px[4] = {255, 255, 255, 255};
        bool colored = false;
        if (solid_rgba) {
            auto to_u8 = [](float v) -> std::uint8_t {
                return static_cast<std::uint8_t>((std::max)(0.f, (std::min)(255.f, v * 255.f + 0.5f)));
            };
            px[0] = to_u8(solid_rgba[0]);
            px[1] = to_u8(solid_rgba[1]);
            px[2] = to_u8(solid_rgba[2]);
            px[3] = to_u8(solid_rgba[3]);
            colored = px[0] < 250 || px[1] < 250 || px[2] < 250 || px[3] < 250;
        }
        if (colored && d.srv_next + 2u <= kSrvDescriptorCount) {
            char solid_key[64];
            std::snprintf(solid_key, sizeof(solid_key), "__solid_%02x%02x%02x%02x", px[0], px[1], px[2], px[3]);
            const std::string key = std::string(solid_key) + "#env#white";
            if (const auto it = d.texture_cache.find(key); it != d.texture_cache.end()) {
                batch.srv_gpu = it->second;
                batch.has_texture = true;
                chunk.meta.mesh_file = solid_key; chunk.content_mesh = -1;
            } else {
                const std::uint32_t slot = d.srv_next;
                d.srv_next += 2;
                const D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = offset_cpu_handle(
                    d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount + slot);
                const D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
                try {
                    if (!try_reset_upload_allocator()) {
                        batch.srv_gpu = white_srv_gpu;
                        batch.has_texture = true;
                    } else {
                        throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "solid tex cmd");
                        const auto tex = texture_detail::upload_rgba_texture(
                            d.device.Get(), d.upload_command_list.Get(), dst_cpu, dst_gpu, px, 1, 1, 4, 1);
                        if (tex.valid) {
                            d.texture_by_slot[slot] = tex.resource;
                            if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
                            d.device->CopyDescriptorsSimple(
                                1,
                                offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                                  kCbvDescriptorCount + slot + 1),
                                white_srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                            throw_hr(d.upload_command_list->Close(), "solid tex close");
                            ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
                            d.queue->ExecuteCommandLists(1, lists);
                            const UINT64 upload_fence = d.fence_value++;
                            throw_hr(d.queue->Signal(d.fence.Get(), upload_fence), "solid tex Signal");
                            d.pending_tex_upload_fence = upload_fence;
                            batch.srv_gpu = dst_gpu;
                            batch.has_texture = true;
                            batch.srv_slot = slot;
                            d.texture_cache[key] = dst_gpu;
                            chunk.meta.mesh_file = solid_key; chunk.content_mesh = -1;
                        } else {
                            throw_hr(d.upload_command_list->Close(), "solid tex empty");
                            batch.srv_gpu = white_srv_gpu;
                            batch.has_texture = true;
                        }
                    }
                } catch (...) {
                    try {
                        d.upload_command_list->Close();
                    } catch (...) {
                    }
                    batch.srv_gpu = white_srv_gpu;
                    batch.has_texture = true;
                }
            }
        } else {
            batch.srv_gpu = white_srv_gpu;
            batch.has_texture = true;
            chunk.meta.mesh_file = "__solid_white__"; chunk.content_mesh = -1;
        }
    }
}

bool HelloO3dRenderer::append_textured_mesh_merged(const omsi::o3d::Vertex* vertices, std::uint32_t vertex_count,
                                                     const std::uint32_t* indices, std::uint32_t index_count,
                                                     const std::vector<ProcMeshSubrange>& subranges) {
    if (impl_) scene_dirty(*impl_, "append_textured_mesh_merged");
    if (!impl_ || !vertices || !indices || vertex_count == 0 || index_count == 0 || subranges.empty()) return false;
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

    GpuMeshChunk chunk{};
    chunk.meta.visible = true;
    chunk.meta.mesh_file = "__spline_merged__"; chunk.content_mesh = -1;
    for (int a = 0; a < 3; ++a) {
        chunk.meta.bbox_min[a] = bmin[a];
        chunk.meta.bbox_max[a] = bmax[a];
    }
    XMStoreFloat4x4(&chunk.local_world_transposed, DirectX::XMMatrixIdentity());
    chunk.pick_positions.resize(vertex_count);
    for (std::uint32_t i = 0; i < vertex_count; ++i)
        chunk.pick_positions[i] = {vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]};
    chunk.pick_indices.assign(indices, indices + index_count);

    const UINT vertex_buffer_size = static_cast<UINT>(vertex_count * sizeof(omsi::o3d::Vertex));
    const UINT index_buffer_size = static_cast<UINT>(index_count * sizeof(std::uint32_t));

    using clk_ = std::chrono::steady_clock;
    const auto tm0 = clk_::now();
    std::lock_guard<std::mutex> upload_lock(g_o3d_upload_mutex);
    const auto tm_lock = clk_::now();
    if (!begin_proc_upload()) return false;
    {
        auto& slot = d.proc_slots[d.proc_slot_i];
        upload_vb_ib(d.device.Get(), d.proc_command_list.Get(), slot, vertices, vertex_buffer_size, indices,
                     index_buffer_size, chunk, d.vb_pool);
        ++d.proc_pending;
        if (d.proc_pending >= 192u) flush_proc_uploads();
    }
    const auto tm_buf = clk_::now();

    // VB/IB views were set by upload_vb_ib (IB lives at an offset in the same buffer).

    chunk.has_uv_vars = false;
    for (const auto& sub : subranges) {
        if (sub.index_count == 0) continue;
        if (sub.index_start + sub.index_count > index_count) continue;
        MaterialDrawBatch batch{};
        batch.index_start = sub.index_start;
        batch.index_count = sub.index_count;
        // The solid colour is passed even when there is a colorMap: it is the fallback the caller needs when the
        // texture file cannot be loaded (see below).
        bind_proc_texture(chunk, batch, sub.texture_path, sub.alpha_mode, sub.text_texture_index,
                          sub.has_solid ? sub.solid_rgba : nullptr);
        // A colorMap that failed to load leaves the white 1x1 placeholder in the SRV (has_texture stays true), and
        // the batch is built once, so it was never requested again: white props as soon as the camera got close.
        // Fall back to the sub's solid colour instead (failures are not cached, so a later upload retries).
        if (!sub.texture_path.empty() && sub.has_solid) {
            const std::string albedo_key = sub.texture_path.string() + "#env#white";
            if (d.texture_cache.find(albedo_key) == d.texture_cache.end()) {
                const std::string keep_mesh_file = chunk.meta.mesh_file; // the solid path renames it
                bind_proc_texture(chunk, batch, std::filesystem::path(), sub.alpha_mode, sub.text_texture_index,
                                  sub.solid_rgba);
                chunk.meta.mesh_file = keep_mesh_file; chunk.content_mesh = -1;
            }
        }
        // BeamNG material state travels even when the material has no extra maps.
        batch.alpha_ref = (std::max)(1.f, (std::min)(255.f, static_cast<float>(sub.alpha_ref))) / 255.f;
        batch.double_sided = sub.double_sided;
        batch.pbr_detail_distance = sub.detail_distance;
        // Material table (t0..t5: colour, 1x1 params, normal, detail, detail normal, opacityMap). Bound when
        // there is any extra map / opacityMask or when the subrange carries baseColorFactor / opacityFactor
        // (mask_tint differs from white). Plain OMSI splines keep the legacy two-descriptor path.
        const bool has_params =
            sub.mask_tint[0] < 0.999f || sub.mask_tint[1] < 0.999f || sub.mask_tint[2] < 0.999f ||
            sub.mask_tint[3] < 0.999f || sub.base_color[0] < 0.999f || sub.base_color[1] < 0.999f ||
            sub.base_color[2] < 0.999f || sub.opacity_factor < 0.999f;
        if (!sub.normal_path.empty() || !sub.detail_path.empty() || !sub.detail_normal_path.empty() ||
            !sub.mask_path.empty() || !sub.mask.empty() || has_params || sub.double_sided)
            bind_pbr_textures(batch, sub);
        push_proc_draw_batch(chunk, batch);
    }
    if (chunk.opaque_batches.empty() && chunk.alpha_batches.empty()) return false;
    const auto tm_tex = clk_::now();
    d.mesh_chunks.push_back(std::move(chunk));
    const auto tm_end = clk_::now();
    {
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        if (ms(tm0, tm_end) > 4.0)
            omsi::log::info("MERGED_SLOW lock=" + std::to_string(ms(tm0, tm_lock)) + " buf=" +
                            std::to_string(ms(tm_lock, tm_buf)) + " tex=" + std::to_string(ms(tm_buf, tm_tex)) +
                            " push=" + std::to_string(ms(tm_tex, tm_end)) + " vb=" + std::to_string(vertex_buffer_size) +
                            " chunks=" + std::to_string(d.mesh_chunks.size()));
    }
    return true;
}

// BeamNG material: 6 contiguous SRVs (albedo, 1x1 material params copied from the bound batch, then normal /
// detail / detail normal / opacityMap). Each extra file loads once (texture_slot_cache) and is copied into
// every table that needs it.
void HelloO3dRenderer::bind_pbr_textures(MaterialDrawBatch& batch, const ProcMeshSubrange& sub) {
    if (!impl_ || !batch.has_texture) return;
    auto& d = *impl_;
    // t1 is ALWAYS the 1x1 material-params texture in mode 5: rgb = baseColorFactor, a = opacityFactor.
    // The opacityMap (tyre marks / dirt decals) is bound to t0 when there is no colorMap (legacy single
    // texture path, flag bit 8) and to t7 when a colorMap exists, where it multiplies the colour alpha.
    const std::filesystem::path& mask_file = sub.mask_path.empty() ? sub.mask : sub.mask_path;
    const bool has_opacity_map = !mask_file.empty();
    const bool mask_in_albedo = has_opacity_map && sub.texture_path.empty();
    const bool opacity_at_t7 = has_opacity_map && !sub.texture_path.empty();
    const bool tint_rgb = !sub.texture_path.empty(); // colorMap: baseColorFactor multiplies the colour
    auto tint_u8 = [](float v) {
        return static_cast<std::uint8_t>((std::max)(0.f, (std::min)(255.f, v * 255.f + 0.5f)));
    };
    float mt_rgba[4]{sub.mask_tint[0], sub.mask_tint[1], sub.mask_tint[2], sub.mask_tint[3]};
    if (sub.base_color[0] < 0.999f || sub.base_color[1] < 0.999f || sub.base_color[2] < 0.999f) {
        mt_rgba[0] = sub.base_color[0];
        mt_rgba[1] = sub.base_color[1];
        mt_rgba[2] = sub.base_color[2];
    }
    if (sub.opacity_factor < 0.999f) mt_rgba[3] = sub.opacity_factor;
    std::uint8_t tint_px[4]{255, 255, 255, 255};
    for (int c = 0; c < 4; ++c) tint_px[c] = tint_u8(mt_rgba[c]);
    // baseColorFactor ~0: with a colorMap keep the texture colour (white tint) so decals are not black;
    // without one fall back to 0.12 grey (skid marks).
    if (tint_px[0] < 3 && tint_px[1] < 3 && tint_px[2] < 3)
        tint_px[0] = tint_px[1] = tint_px[2] = sub.texture_path.empty() ? 31 : 255;
    char tint_suffix[48] = {};
    std::snprintf(tint_suffix, sizeof(tint_suffix), "|%02x%02x%02x%02x", static_cast<unsigned>(tint_px[0]),
                  static_cast<unsigned>(tint_px[1]), static_cast<unsigned>(tint_px[2]),
                  static_cast<unsigned>(tint_px[3]));
    const std::string key = "__pbr|" + (mask_in_albedo ? mask_file.string() : sub.texture_path.string()) + "|" +
                            sub.normal_path.string() + "|" + sub.detail_path.string() + "|" +
                            sub.detail_normal_path.string() + "|" + std::to_string(sub.detail_scale) + "x" +
                            std::to_string(sub.detail_scale_y) +
                            (opacity_at_t7 ? "|om:" + mask_file.string() : std::string()) + tint_suffix;
    float flags = 0.f;
    if (!sub.normal_path.empty()) flags += 1.f;
    if (!sub.detail_path.empty()) flags += 2.f;
    if (!sub.detail_normal_path.empty()) flags += 4.f;
    if (mask_in_albedo) flags += 8.f;
    auto finish = [&](D3D12_GPU_DESCRIPTOR_HANDLE h) {
        batch.srv_gpu = h;
        batch.pbr = true;
        const float sy = sub.detail_scale_y > 0.f ? sub.detail_scale_y : (sub.detail_scale > 0.f ? sub.detail_scale : 1.f);
        batch.pbr_flags = flags + 16.f * std::round((std::min)(sy, 4000.f) * 64.f);
        batch.pbr_detail_scale = sub.detail_scale > 0.f ? sub.detail_scale : 1.f;
        batch.pbr_opacity_map = opacity_at_t7;
        batch.pbr_tint_rgb = tint_rgb;
        batch.alpha_ref = (std::max)(1.f, (std::min)(255.f, static_cast<float>(sub.alpha_ref))) / 255.f;
        batch.double_sided = sub.double_sided;
        batch.pbr_detail_distance = sub.detail_distance;
    };
    if (const auto it = d.texture_cache.find(key); it != d.texture_cache.end()) {
        finish(it->second);
        return;
    }
    if (d.srv_next + 6u + 3u > kSrvDescriptorCount) return;
    const auto heap_cpu = d.srv_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE white_cpu = offset_cpu_handle(heap_cpu, d.srv_stride, kCbvDescriptorCount);
    const std::uint32_t slot = d.srv_next;
    d.srv_next += 6;
    auto cpu_at = [&](std::uint32_t s) { return offset_cpu_handle(heap_cpu, d.srv_stride, kCbvDescriptorCount + s); };
    // albedo + env from the batch's current table
    const auto src_index = static_cast<std::uint32_t>((batch.srv_gpu.ptr - d.srv_heap_gpu.ptr) / d.srv_stride);
    d.device->CopyDescriptorsSimple(2, cpu_at(slot), cpu_at(src_index), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // t7 = opacityMap slot: white until an opacityMap is loaded into it.
    d.device->CopyDescriptorsSimple(1, cpu_at(slot + 5u), white_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const std::filesystem::path* extra[3] = {&sub.normal_path, &sub.detail_path, &sub.detail_normal_path};
    bool list_open = false;
    for (int i = 0; i < 3; ++i) {
        const auto dst = cpu_at(slot + 2u + static_cast<std::uint32_t>(i));
        const auto& p = *extra[i];
        if (p.empty()) {
            d.device->CopyDescriptorsSimple(1, dst, white_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            continue;
        }
        if (const auto it = d.texture_slot_cache.find(p.string()); it != d.texture_slot_cache.end()) {
            d.device->CopyDescriptorsSimple(1, dst, cpu_at(it->second), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            continue;
        }
        bool ok = false;
        try {
            if (!list_open) {
                if (!try_reset_upload_allocator()) throw std::runtime_error("upload allocator busy");
                throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "pbr tex cmd reset");
                list_open = true;
            }
            const std::uint32_t s = slot + 2u + static_cast<std::uint32_t>(i);
            const auto tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst,
                                               offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, s), p);
            if (tex.valid) {
                d.texture_by_slot[s] = tex.resource;
                if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
                d.texture_slot_files[s] = p;
                d.texture_slot_cache[p.string()] = s;
                ok = true;
            }
        } catch (...) {
        }
        if (!ok) {
            d.device->CopyDescriptorsSimple(1, dst, white_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (i == 0) flags = static_cast<float>(static_cast<int>(flags) & ~1);
            if (i == 1) flags = static_cast<float>(static_cast<int>(flags) & ~2);
            if (i == 2) flags = static_cast<float>(static_cast<int>(flags) & ~4);
        }
    }
    if (mask_in_albedo) {
        // No colorMap: t0 = opacity mask (replaces the white albedo seeded by bind_proc_texture).
        bool mask_ok = false;
        try {
            if (!list_open) {
                if (!try_reset_upload_allocator()) throw std::runtime_error("upload allocator busy");
                throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "mask tex cmd reset");
                list_open = true;
            }
            const auto tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), cpu_at(slot),
                                               offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot), mask_file);
            if (tex.valid) {
                d.texture_by_slot[slot] = tex.resource;
                if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
                d.texture_slot_files[slot] = mask_file;
                d.texture_slot_cache[mask_file.string()] = slot;
                mask_ok = true;
            }
        } catch (...) {
        }
        if (!mask_ok) {
            d.device->CopyDescriptorsSimple(1, cpu_at(slot), white_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            flags = static_cast<float>(static_cast<int>(flags) & ~8);
        }
    } else if (opacity_at_t7) {
        // colorMap + opacityMap: keep t0 = colour and bind the mask to t7 (mask in alpha/luminance).
        bool om_ok = false;
        try {
            if (!list_open) {
                if (!try_reset_upload_allocator()) throw std::runtime_error("upload allocator busy");
                throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "opacity tex cmd reset");
                list_open = true;
            }
            const auto dst = cpu_at(slot + 5u);
            const auto tex = load_texture_file(d.device.Get(), d.upload_command_list.Get(), dst,
                                               offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot + 5u),
                                               mask_file);
            if (tex.valid) {
                d.texture_by_slot[slot + 5u] = tex.resource;
                if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
                d.texture_slot_files[slot + 5u] = mask_file;
                d.texture_slot_cache[mask_file.string()] = slot + 5u;
                om_ok = true;
            }
        } catch (...) {
        }
        if (!om_ok)
            d.device->CopyDescriptorsSimple(1, cpu_at(slot + 5u), white_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    // t1 = 1x1 material params (rgb = baseColorFactor, a = opacityFactor) — always in mode 5.
    bool tint_ok = false;
    try {
        if (!list_open) {
            if (!try_reset_upload_allocator()) throw std::runtime_error("upload allocator busy");
            throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "mat params cmd reset");
            list_open = true;
        }
        const auto tex = texture_detail::upload_rgba_texture(
            d.device.Get(), d.upload_command_list.Get(), cpu_at(slot + 1u),
            offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot + 1u), tint_px, 1, 1, 4, 1);
        if (tex.valid) {
            d.texture_by_slot[slot + 1u] = tex.resource;
            if (tex.upload_staging) d.texture_upload_scratch.push_back(tex.upload_staging);
            tint_ok = true;
        }
    } catch (...) {
    }
    if (!tint_ok)
        d.device->CopyDescriptorsSimple(1, cpu_at(slot + 1u), white_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (list_open) {
        try {
            throw_hr(d.upload_command_list->Close(), "pbr tex close");
            ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
            d.queue->ExecuteCommandLists(1, lists);
            const UINT64 upload_fence = d.fence_value++;
            throw_hr(d.queue->Signal(d.fence.Get(), upload_fence), "pbr tex Signal");
            d.pending_tex_upload_fence = upload_fence;
        } catch (...) {
        }
    }
    const auto gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, slot);
    d.texture_cache[key] = gpu;
    finish(gpu);
}

bool HelloO3dRenderer::append_textured_mesh(const omsi::o3d::Vertex* vertices, std::uint32_t vertex_count,
                                            const std::uint32_t* indices, std::uint32_t index_count,
                                            const std::filesystem::path& texture_path, std::uint8_t alpha_mode,
                                            int text_texture_index, const float* solid_rgba) {
    if (impl_) scene_dirty(*impl_, "append_textured_mesh");
    if (!impl_ || !vertices || !indices || vertex_count == 0 || index_count == 0) return false;
    auto& d = *impl_;
    if (!d.device) return false;

    GpuMeshChunk chunk{};
    float bmin[3], bmax[3];
    prepare_mesh_cpu(vertices, vertex_count, indices, index_count, bmin, bmax, chunk.pick_positions,
                     chunk.pick_indices);
    chunk.meta.visible = true;
    chunk.meta.mesh_file = texture_path.empty() ? "__proc_mesh__" : texture_path.filename().string(); chunk.content_mesh = -1;
    for (int a = 0; a < 3; ++a) {
        chunk.meta.bbox_min[a] = bmin[a];
        chunk.meta.bbox_max[a] = bmax[a];
    }
    XMStoreFloat4x4(&chunk.local_world_transposed, DirectX::XMMatrixIdentity());
    chunk.lod_capable = terrain_lod::is_lod_candidate(index_count, bmin, bmax);

    const UINT vertex_buffer_size = static_cast<UINT>(vertex_count * sizeof(omsi::o3d::Vertex));
    const UINT index_buffer_size = static_cast<UINT>(index_count * sizeof(std::uint32_t));

    std::lock_guard<std::mutex> upload_lock(g_o3d_upload_mutex);
    if (!begin_proc_upload()) return false;
    {
        // Buffers stay in COMMON: implicit promotion to VB/IB on the graphics queue; the
        // queue->Wait in flush_proc_uploads orders the draw after the copy (no CPU stall).
        auto& slot = d.proc_slots[d.proc_slot_i];
        upload_vb_ib(d.device.Get(), d.proc_command_list.Get(), slot, vertices, vertex_buffer_size, indices,
                     index_buffer_size, chunk, d.vb_pool);
        ++d.proc_pending;
        if (d.proc_pending >= 192u) flush_proc_uploads();
    }

    // VB/IB views were set by upload_vb_ib (IB lives at an offset in the same buffer).

    MaterialDrawBatch batch{};
    batch.index_start = 0;
    batch.index_count = index_count;
    bind_proc_texture(chunk, batch, texture_path, alpha_mode, text_texture_index, solid_rgba);
    push_proc_draw_batch(chunk, batch);
    const bool lod_cap = chunk.lod_capable;
    d.mesh_chunks.push_back(std::move(chunk));
    if (lod_cap) {
        d.mesh_chunks.back().lod_listed = true;
        d.lod_chunks.push_back(d.mesh_chunks.size() - 1);
    }
    return true;
}

bool HelloO3dRenderer::set_chunk_detail_mesh(std::size_t chunk_index, const omsi::o3d::Vertex* vertices,
                                             std::uint32_t vertex_count, const std::uint32_t* indices,
                                             std::uint32_t index_count, int ring_max) {
    if (impl_) scene_dirty(*impl_, "set_chunk_detail_mesh");
    if (!impl_ || !vertices || !indices || vertex_count == 0 || index_count == 0) return false;
    auto& d = *impl_;
    if (!d.device || chunk_index >= d.mesh_chunks.size()) return false;
    const UINT vertex_buffer_size = static_cast<UINT>(vertex_count * sizeof(omsi::o3d::Vertex));
    const UINT index_buffer_size = static_cast<UINT>(index_count * sizeof(std::uint32_t));

    Microsoft::WRL::ComPtr<ID3D12Resource> vb, ib, vb_staging, ib_staging;
    {
        std::lock_guard<std::mutex> upload_lock(g_o3d_upload_mutex);
        if (!begin_proc_upload()) return false;
        auto& slot = d.proc_slots[d.proc_slot_i];
        create_default_buffer(d.device.Get(), d.proc_command_list.Get(), vertices, vertex_buffer_size,
                              D3D12_RESOURCE_STATE_COMMON, vb, vb_staging);
        create_default_buffer(d.device.Get(), d.proc_command_list.Get(), indices, index_buffer_size,
                              D3D12_RESOURCE_STATE_COMMON, ib, ib_staging);
        slot.keep_alive.push_back(vb);
        slot.keep_alive.push_back(ib);
        slot.keep_alive.push_back(std::move(vb_staging));
        slot.keep_alive.push_back(std::move(ib_staging));
        ++d.proc_pending;
        if (d.proc_pending >= 192u) flush_proc_uploads();
    }
    auto& chunk = d.mesh_chunks[chunk_index];
    // Reuse the base chunk's material (first batch) for the whole detail index range.
    const MaterialDrawBatch* src = !chunk.opaque_batches.empty()   ? &chunk.opaque_batches.front()
                                   : !chunk.alpha_batches.empty() ? &chunk.alpha_batches.front()
                                                                  : nullptr;
    if (!src) return false;
    MaterialDrawBatch batch = *src;
    batch.index_start = 0;
    batch.index_count = index_count;
    // The previous detail (if any) may still be referenced by frames in flight.
    if (chunk.detail_vb) d.lod_graveyard.emplace_back(d.lod_frame_counter + 8u, std::move(chunk.detail_vb));
    if (chunk.detail_ib) d.lod_graveyard.emplace_back(d.lod_frame_counter + 8u, std::move(chunk.detail_ib));
    chunk.detail_vb = std::move(vb);
    chunk.detail_ib = std::move(ib);
    chunk.detail_vbv.BufferLocation = chunk.detail_vb->GetGPUVirtualAddress();
    chunk.detail_vbv.StrideInBytes = sizeof(omsi::o3d::Vertex);
    chunk.detail_vbv.SizeInBytes = vertex_buffer_size;
    chunk.detail_ibv.BufferLocation = chunk.detail_ib->GetGPUVirtualAddress();
    chunk.detail_ibv.SizeInBytes = index_buffer_size;
    chunk.detail_ibv.Format = DXGI_FORMAT_R32_UINT;
    chunk.detail_batches.assign(1, batch);
    chunk.detail_ready = true;
    chunk.detail_ring_max = ring_max;
    chunk.detail_active = false; // decided per frame from the camera ring
    if (std::find(d.detail_chunks.begin(), d.detail_chunks.end(), chunk_index) == d.detail_chunks.end())
        d.detail_chunks.push_back(chunk_index);
    return true;
}

void HelloO3dRenderer::clear_chunk_detail(std::size_t chunk_index) {
    if (impl_) scene_dirty(*impl_, "clear_chunk_detail");
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return;
    auto& d = *impl_;
    auto& chunk = d.mesh_chunks[chunk_index];
    if (chunk.detail_vb) d.lod_graveyard.emplace_back(d.lod_frame_counter + 8u, std::move(chunk.detail_vb));
    if (chunk.detail_ib) d.lod_graveyard.emplace_back(d.lod_frame_counter + 8u, std::move(chunk.detail_ib));
    chunk.detail_vb.Reset();
    chunk.detail_ib.Reset();
    chunk.detail_vbv = {};
    chunk.detail_ibv = {};
    chunk.detail_batches.clear();
    chunk.detail_ready = false;
    chunk.detail_active = false;
    chunk.detail_ring_max = -1;
    d.detail_chunks.erase(std::remove(d.detail_chunks.begin(), d.detail_chunks.end(), chunk_index),
                          d.detail_chunks.end());
}

bool HelloO3dRenderer::chunk_has_detail(std::size_t chunk_index) const {
    return impl_ && chunk_index < impl_->mesh_chunks.size() && impl_->mesh_chunks[chunk_index].detail_ready;
}

int HelloO3dRenderer::chunk_camera_ring(std::size_t chunk_index) const {
    if (!impl_ || chunk_index >= impl_->mesh_chunks.size()) return -1;
    const auto& d = *impl_;
    const auto& ch = d.mesh_chunks[chunk_index];
    const float tw = d.tile_cull_w > 1.f ? d.tile_cull_w : 300.f;
    const float th = d.tile_cull_h > 1.f ? d.tile_cull_h : tw;
    const float mx = 0.5f * (ch.meta.bbox_min[0] + ch.meta.bbox_max[0]);
    const float my = 0.5f * (ch.meta.bbox_min[1] + ch.meta.bbox_max[1]);
    const DirectX::XMMATRIX world = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&ch.local_world_transposed));
    DirectX::XMFLOAT3 c{};
    DirectX::XMStoreFloat3(&c, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(mx, my, 0.f, 1.f), world));
    const int tx = static_cast<int>(std::floor((c.x - d.tile_cull_ox) / tw));
    const int ty = static_cast<int>(std::floor((c.y - d.tile_cull_oy) / th));
    const int cx = static_cast<int>(std::floor((d.camera_world_pos[0] - d.tile_cull_ox) / tw));
    const int cy = static_cast<int>(std::floor((d.camera_world_pos[1] - d.tile_cull_oy) / th));
    return (std::max)(std::abs(tx - cx), std::abs(ty - cy));
}

bool HelloO3dRenderer::load_model_cfg(const ModelCfgDocument& cfg) {
    if (impl_) scene_dirty(*impl_, "load_model_cfg");
    if (!impl_) return false;
    try {
        impl_->mesh_chunks.clear();
        impl_->texture_resources.clear();
        impl_->texture_cache.clear();
        impl_->texture_slot_cache.clear();
        impl_->srv_next = 1;

        const auto* lod = omsi::model_cfg::primary_lod(cfg);
        if (!lod) return false;
        const auto model_dir = cfg.cfg_path.parent_path();
        std::size_t loaded = 0;
        std::size_t mesh_index = 0;

        for (const auto& mesh_entry : lod->meshes) {
            if (mesh_entry.mesh_file.empty()) {
                ++mesh_index;
                continue;
            }
            const auto o3d_path = model_dir / mesh_entry.mesh_file;
            std::error_code ec;
            if (!std::filesystem::exists(o3d_path, ec)) {
                omsi::log::warn("model.cfg mesh missing: " + o3d_path.string());
                ++mesh_index;
                continue;
            }
            auto mapped = omsi::o3d::load_mapped(o3d_path);
            if (!mapped) {
                omsi::log::warn("model.cfg mesh load failed: " + o3d_path.string());
                ++mesh_index;
                continue;
            }
            if (!upload_mesh_gpu(*mapped, &mesh_entry)) {
                omsi::log::warn("model.cfg mesh upload failed: " + o3d_path.string());
                ++mesh_index;
                continue;
            }
            if (!impl_->mesh_chunks.empty()) impl_->mesh_chunks.back().cfg_mesh_index = mesh_index;
            ++loaded;
            ++mesh_index;
        }

        omsi::log::info("model.cfg loaded meshes=" + std::to_string(loaded) + "/" +
                        std::to_string(lod->meshes.size()));
        return loaded > 0;
    } catch (const std::exception& ex) {
        omsi::log::error(std::string("load_model_cfg failed: ") + ex.what());
        return false;
    }
}

} // namespace omsi::gfx::dx12
