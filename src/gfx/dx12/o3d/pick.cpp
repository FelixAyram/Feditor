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

namespace {

bool ray_hits_aabb(float ox, float oy, float oz, float dx, float dy, float dz, const float bmin[3],
                   const float bmax[3], float& out_t) {
    float tmin = 0.f;
    float tmax = 1e9f;
    const float origin[3] = {ox, oy, oz};
    const float dir[3] = {dx, dy, dz};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dir[axis]) < 1e-8f) {
            if (origin[axis] < bmin[axis] || origin[axis] > bmax[axis]) return false;
            continue;
        }
        const float inv = 1.f / dir[axis];
        float t0 = (bmin[axis] - origin[axis]) * inv;
        float t1 = (bmax[axis] - origin[axis]) * inv;
        if (t0 > t1) std::swap(t0, t1);
        tmin = (std::max)(tmin, t0);
        tmax = (std::min)(tmax, t1);
        if (tmax < tmin) return false;
    }
    out_t = tmin;
    return tmax >= 0.f;
}

// [PERF] Inverse world matrix cache for picking: it depends only on local_world_transposed, yet
// XMMatrixInverse was recomputed on every hover pick for every chunk that passed the AABB test.
// Validated by memcmp of the source matrix, so a recycled chunk address is detected.
// (The world-space AABB itself is already cached in GpuMeshChunk::aabb_cache_*.)
struct ChunkInvCache {
    DirectX::XMFLOAT4X4 src{};
    DirectX::XMFLOAT4X4 inv{};
    bool valid = false;
    bool singular = true;
};

ChunkInvCache& chunk_inv_cache(const GpuMeshChunk& chunk) {
    static std::unordered_map<const GpuMeshChunk*, ChunkInvCache> s_cache;
    if (s_cache.size() > 65536) s_cache.clear();
    auto it = s_cache.find(&chunk);
    if (it == s_cache.end()) it = s_cache.emplace(&chunk, ChunkInvCache{}).first;
    ChunkInvCache& e = it->second;
    if (e.valid && std::memcmp(&e.src, &chunk.local_world_transposed, sizeof(e.src)) == 0) return e;
    e.src = chunk.local_world_transposed;
    const DirectX::XMMATRIX world =
        DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&chunk.local_world_transposed));
    DirectX::XMVECTOR det{};
    const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(&det, world);
    e.singular = std::fabs(DirectX::XMVectorGetX(det)) < 1e-12f;
    DirectX::XMStoreFloat4x4(&e.inv, inv);
    e.valid = true;
    return e;
}

bool ray_hits_chunk_world_aabb(float ox, float oy, float oz, float dx, float dy, float dz,
                               const GpuMeshChunk& chunk, float& out_t) {
    float bmin[3];
    float bmax[3];
    if (chunk.aabb_cache_valid &&
        std::memcmp(&chunk.aabb_cache_matrix, &chunk.local_world_transposed,
                    sizeof(DirectX::XMFLOAT4X4)) == 0) {
        std::memcpy(bmin, chunk.aabb_cache_min, sizeof(bmin));
        std::memcpy(bmax, chunk.aabb_cache_max, sizeof(bmax));
    } else {
        const DirectX::XMMATRIX world =
            DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&chunk.local_world_transposed));
        const float x0 = chunk.meta.bbox_min[0], y0 = chunk.meta.bbox_min[1], z0 = chunk.meta.bbox_min[2];
        const float x1 = chunk.meta.bbox_max[0], y1 = chunk.meta.bbox_max[1], z1 = chunk.meta.bbox_max[2];
        bmin[0] = 1e9f, bmin[1] = 1e9f, bmin[2] = 1e9f;
        bmax[0] = -1e9f, bmax[1] = -1e9f, bmax[2] = -1e9f;
        const float xs[2] = {x0, x1}, ys[2] = {y0, y1}, zs[2] = {z0, z1};
        for (float x : xs)
            for (float y : ys)
                for (float z : zs) {
                    DirectX::XMFLOAT3 p{};
                    DirectX::XMStoreFloat3(&p,
                                           DirectX::XMVector3Transform(DirectX::XMVectorSet(x, y, z, 1.f), world));
                    bmin[0] = (std::min)(bmin[0], p.x);
                    bmin[1] = (std::min)(bmin[1], p.y);
                    bmin[2] = (std::min)(bmin[2], p.z);
                    bmax[0] = (std::max)(bmax[0], p.x);
                    bmax[1] = (std::max)(bmax[1], p.y);
                    bmax[2] = (std::max)(bmax[2], p.z);
                }
        chunk.aabb_cache_matrix = chunk.local_world_transposed;
        std::memcpy(chunk.aabb_cache_min, bmin, sizeof(bmin));
        std::memcpy(chunk.aabb_cache_max, bmax, sizeof(bmax));
        chunk.aabb_cache_valid = true;
    }
    for (int a = 0; a < 3; ++a) {
        if (bmax[a] - bmin[a] < 0.05f) {
            const float m = 0.5f * (bmin[a] + bmax[a]);
            bmin[a] = m - 0.05f;
            bmax[a] = m + 0.05f;
        }
    }
    return ray_hits_aabb(ox, oy, oz, dx, dy, dz, bmin, bmax, out_t);
}

// Möller–Trumbore; t is distance along unnormalized ray dir (dx,dy,dz).
bool ray_hits_triangle(float ox, float oy, float oz, float dx, float dy, float dz, const DirectX::XMFLOAT3& a,
                       const DirectX::XMFLOAT3& b, const DirectX::XMFLOAT3& c, float& out_t) {
    const float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
    const float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
    const float px = dy * e2z - dz * e2y;
    const float py = dz * e2x - dx * e2z;
    const float pz = dx * e2y - dy * e2x;
    const float det = e1x * px + e1y * py + e1z * pz;
    if (std::fabs(det) < 1.e-8f) return false;
    const float inv = 1.f / det;
    const float tx = ox - a.x, ty = oy - a.y, tz = oz - a.z;
    const float u = (tx * px + ty * py + tz * pz) * inv;
    if (u < 0.f || u > 1.f) return false;
    const float qx = ty * e1z - tz * e1y;
    const float qy = tz * e1x - tx * e1z;
    const float qz = tx * e1y - ty * e1x;
    const float v = (dx * qx + dy * qy + dz * qz) * inv;
    if (v < 0.f || u + v > 1.f) return false;
    const float tt = (e2x * qx + e2y * qy + e2z * qz) * inv;
    if (tt < 1.e-5f) return false;
    out_t = tt;
    return true;
}

// [PERF] Heavy meshes (100k..millions of triangles) were ray-tested triangle by triangle on every hover
// pick, and the editor repeats the pick up to 25x (spiral) when the cursor misses. Group the triangles in
// fixed blocks with a local-space AABB (built once per mesh, validated by buffer identity/size) and only
// test the triangles of blocks the ray actually crosses.
constexpr std::size_t kPickBlockTris = 128;
constexpr std::size_t kPickBlockMinTris = 4096;

struct PickBlocks {
    const void* idx_data = nullptr;
    const void* pos_data = nullptr;
    std::size_t idx_count = 0;
    std::size_t pos_count = 0;
    std::vector<float> bb; // 6 floats per block: min xyz, max xyz
};

const PickBlocks& pick_blocks_for(const GpuMeshChunk& src) {
    static std::unordered_map<const GpuMeshChunk*, PickBlocks> s_blocks;
    if (s_blocks.size() > 4096) s_blocks.clear();
    PickBlocks& pb = s_blocks[&src];
    if (pb.idx_data == src.pick_indices.data() && pb.pos_data == src.pick_positions.data() &&
        pb.idx_count == src.pick_indices.size() && pb.pos_count == src.pick_positions.size() && !pb.bb.empty())
        return pb;
    pb.idx_data = src.pick_indices.data();
    pb.pos_data = src.pick_positions.data();
    pb.idx_count = src.pick_indices.size();
    pb.pos_count = src.pick_positions.size();
    const std::size_t tris = pb.idx_count / 3;
    const std::size_t nblocks = (tris + kPickBlockTris - 1) / kPickBlockTris;
    pb.bb.assign(nblocks * 6, 0.f);
    for (std::size_t b = 0; b < nblocks; ++b) {
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        const std::size_t t0 = b * kPickBlockTris;
        const std::size_t t1 = (std::min)(t0 + kPickBlockTris, tris);
        for (std::size_t t = t0; t < t1; ++t) {
            for (int k = 0; k < 3; ++k) {
                const std::size_t vi = src.pick_indices[t * 3 + static_cast<std::size_t>(k)];
                if (vi >= pb.pos_count) continue;
                const auto& p = src.pick_positions[vi];
                mn[0] = (std::min)(mn[0], p.x); mn[1] = (std::min)(mn[1], p.y); mn[2] = (std::min)(mn[2], p.z);
                mx[0] = (std::max)(mx[0], p.x); mx[1] = (std::max)(mx[1], p.y); mx[2] = (std::max)(mx[2], p.z);
            }
        }
        for (int a = 0; a < 3; ++a) {
            pb.bb[b * 6 + static_cast<std::size_t>(a)] = mn[a] - 1.e-3f;
            pb.bb[b * 6 + 3 + static_cast<std::size_t>(a)] = mx[a] + 1.e-3f;
        }
    }
    return pb;
}

// Slab test; true when the ray (origin o, direction d, t in [0, t_max]) crosses the box.
inline bool ray_hits_block_aabb(const float o[3], const float inv_d[3], const float* bb, float t_max) {
    float tmin = 0.f, tmax = t_max;
    for (int a = 0; a < 3; ++a) {
        const float t0 = (bb[a] - o[a]) * inv_d[a];
        const float t1 = (bb[3 + a] - o[a]) * inv_d[a];
        const float lo = (std::min)(t0, t1), hi = (std::max)(t0, t1);
        tmin = (std::max)(tmin, lo);
        tmax = (std::min)(tmax, hi);
        if (tmax < tmin) return false;
    }
    return true;
}

bool ray_hits_chunk_triangles(float ox, float oy, float oz, float dx, float dy, float dz,
                              const GpuMeshChunk& chunk, const std::vector<GpuMeshChunk>& chunks, float& out_t) {
    const GpuMeshChunk* src = &chunk;
    if (chunk.pick_positions.empty() && chunk.pick_src < chunks.size())
        src = &chunks[chunk.pick_src];
    if (src->pick_positions.empty() || src->pick_indices.size() < 3)
        return ray_hits_chunk_world_aabb(ox, oy, oz, dx, dy, dz, chunk, out_t);

    float aabb_t = 0.f;
    if (!ray_hits_chunk_world_aabb(ox, oy, oz, dx, dy, dz, chunk, aabb_t)) return false;

    // Ray into chunk-local space once (affine: t is preserved), instead of transforming every
    // triangle vertex to world — 3 matrix products per triangle on every hover frame.
    // [PERF] The inverse is cached with the chunk (recomputed only when its world matrix changes):
    // picking re-enters the same chunks on every hover frame.
    const ChunkInvCache& cc = chunk_inv_cache(chunk);
    if (cc.singular) return false;
    const DirectX::XMMATRIX inv = DirectX::XMLoadFloat4x4(&cc.inv);
    DirectX::XMFLOAT3 lo{}, ld{};
    DirectX::XMStoreFloat3(&lo, DirectX::XMVector3Transform(DirectX::XMVectorSet(ox, oy, oz, 1.f), inv));
    DirectX::XMStoreFloat3(&ld, DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(dx, dy, dz, 0.f), inv));
    bool hit = false;
    float best = 1e9f;
    if (src->pick_indices.size() >= kPickBlockMinTris * 3) {
        const PickBlocks& pb = pick_blocks_for(*src);
        const float lo_a[3] = {lo.x, lo.y, lo.z};
        auto safe_inv = [](float v) {
            const float a = std::fabs(v) < 1.e-12f ? (v < 0.f ? -1.e-12f : 1.e-12f) : v;
            return 1.f / a;
        };
        const float inv_d[3] = {safe_inv(ld.x), safe_inv(ld.y), safe_inv(ld.z)};
        const std::size_t tris = src->pick_indices.size() / 3;
        const std::size_t nblocks = pb.bb.size() / 6;
        for (std::size_t b = 0; b < nblocks; ++b) {
            if (!ray_hits_block_aabb(lo_a, inv_d, &pb.bb[b * 6], best)) continue;
            const std::size_t t0 = b * kPickBlockTris;
            const std::size_t t1 = (std::min)(t0 + kPickBlockTris, tris);
            for (std::size_t t = t0; t < t1; ++t) {
                const auto& ia = src->pick_positions[src->pick_indices[t * 3]];
                const auto& ib = src->pick_positions[src->pick_indices[t * 3 + 1]];
                const auto& ic = src->pick_positions[src->pick_indices[t * 3 + 2]];
                const DirectX::XMFLOAT3 wa{ia.x, ia.y, ia.z}, wb{ib.x, ib.y, ib.z}, wc{ic.x, ic.y, ic.z};
                float tt = 0.f;
                if (!ray_hits_triangle(lo.x, lo.y, lo.z, ld.x, ld.y, ld.z, wa, wb, wc, tt)) continue;
                if (tt < best) {
                    best = tt;
                    hit = true;
                }
            }
        }
        if (!hit) return false;
        out_t = best;
        return true;
    }
    for (std::size_t i = 0; i + 2 < src->pick_indices.size(); i += 3) {
        const auto& ia = src->pick_positions[src->pick_indices[i]];
        const auto& ib = src->pick_positions[src->pick_indices[i + 1]];
        const auto& ic = src->pick_positions[src->pick_indices[i + 2]];
        const DirectX::XMFLOAT3 wa{ia.x, ia.y, ia.z}, wb{ib.x, ib.y, ib.z}, wc{ic.x, ic.y, ic.z};
        float tt = 0.f;
        if (!ray_hits_triangle(lo.x, lo.y, lo.z, ld.x, ld.y, ld.z, wa, wb, wc, tt)) continue;
        if (tt < best) {
            best = tt;
            hit = true;
        }
    }
    if (!hit) return false;
    out_t = best;
    return true;
}

} // namespace

std::optional<std::size_t> HelloO3dRenderer::pick_chunk(float mouse_x, float mouse_y, std::uint32_t fb_width,
                                                          std::uint32_t fb_height,
                                                          const float view_proj_transposed[16],
                                                          bool require_mouseevent,
                                                          const std::size_t* allowed_chunks,
                                                          std::size_t allowed_count, float* out_t) const {
    if (!impl_ || fb_width == 0 || fb_height == 0) return std::nullopt;

    DirectX::XMFLOAT4X4 vp{};
    std::memcpy(&vp, view_proj_transposed, sizeof(vp));
    const DirectX::XMMATRIX vp_mat = DirectX::XMLoadFloat4x4(&vp);
    const DirectX::XMMATRIX inv_vp = DirectX::XMMatrixInverse(nullptr, DirectX::XMMatrixTranspose(vp_mat));

    const float ndc_x = 2.f * mouse_x / static_cast<float>(fb_width) - 1.f;
    const float ndc_y = 1.f - 2.f * mouse_y / static_cast<float>(fb_height);
    // Reverse-Z NDC: near=1, far=0 (matches luna_proj_matrix).
    const DirectX::XMVECTOR near_h = DirectX::XMVector4Transform(DirectX::XMVectorSet(ndc_x, ndc_y, 1.f, 1.f), inv_vp);
    const DirectX::XMVECTOR far_h = DirectX::XMVector4Transform(DirectX::XMVectorSet(ndc_x, ndc_y, 0.f, 1.f), inv_vp);
    DirectX::XMFLOAT4 n4{}, f4{};
    DirectX::XMStoreFloat4(&n4, near_h);
    DirectX::XMStoreFloat4(&f4, far_h);
    if (std::fabs(n4.w) < 1e-8f || std::fabs(f4.w) < 1e-8f) return std::nullopt;
    const float ox = n4.x / n4.w;
    const float oy = n4.y / n4.w;
    const float oz = n4.z / n4.w;
    const float fx = f4.x / f4.w;
    const float fy = f4.y / f4.w;
    const float fz = f4.z / f4.w;
    const float dx = fx - ox;
    const float dy = fy - oy;
    const float dz = fz - oz;

    auto try_chunk = [&](std::size_t i, std::optional<std::size_t>& best, float& best_t, bool triangles_only) {
        if (i >= impl_->mesh_chunks.size()) return;
        const auto& chunk = impl_->mesh_chunks[i];
        if (!chunk.meta.visible) return;
        if (require_mouseevent && !chunk.meta.mouseevent_var) return;
        float t = 0.f;
        if (ray_hits_chunk_triangles(ox, oy, oz, dx, dy, dz, chunk, impl_->mesh_chunks, t)) {
            if (t < best_t) {
                best_t = t;
                best = i;
            }
        }
        (void)triangles_only;
    };

    std::optional<std::size_t> best;
    float best_t = 1e9f;
    if (allowed_chunks && allowed_count > 0) {
        for (std::size_t n = 0; n < allowed_count; ++n) try_chunk(allowed_chunks[n], best, best_t, true);
    } else {
        for (std::size_t i = 0; i < impl_->mesh_chunks.size(); ++i) try_chunk(i, best, best_t, true);
    }
    if (best && out_t) *out_t = best_t;
    return best;
}

std::optional<float> HelloO3dRenderer::pick_surface_hit_z(float ox, float oy, float oz, float dx, float dy,
                                                           float dz, const std::size_t* exclude_chunks,
                                                           std::size_t exclude_count, float* out_t) const {
    if (!impl_) return std::nullopt;
    auto excluded = [&](std::size_t i) {
        if (!exclude_chunks || exclude_count == 0) return false;
        for (std::size_t n = 0; n < exclude_count; ++n)
            if (exclude_chunks[n] == i) return true;
        return false;
    };
    bool any = false;
    float best_t = 1e9f;
    for (std::size_t i = 0; i < impl_->mesh_chunks.size(); ++i) {
        if (excluded(i)) continue;
        const auto& chunk = impl_->mesh_chunks[i];
        if (!chunk.meta.visible) continue;
        if (chunk.screen_space) continue;
        float t = 0.f;
        if (!ray_hits_chunk_triangles(ox, oy, oz, dx, dy, dz, chunk, impl_->mesh_chunks, t)) continue;
        if (t < best_t) {
            best_t = t;
            any = true;
        }
    }
    if (!any) return std::nullopt;
    if (out_t) *out_t = best_t;
    return oz + best_t * dz;
}

} // namespace omsi::gfx::dx12
