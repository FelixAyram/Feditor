#pragma once
// Terrain tile LOD by vertex clustering.
//
// Why: after AlignTerrain / polycut a 300 m tile can reach 40-76 MB (>1 M vertices of centimetre-sized
// triangles). Drawing every neighbouring tile at that density is what pushed gpu_scene to 20-65 ms.
//
// What: for a big tile mesh we build up to kLevels *index-only* LODs that reference the SAME vertex buffer:
//   - vertices are snapped into square cells (size = tile_width/60 * kCellMult[level]),
//   - every cluster keeps ONE original vertex (the one closest to the cluster centroid), so the LOD surface
//     is made of real points of the full-quality surface (no height drift, heights used by objects /
//     pick / heightfield are never touched),
//   - vertices on the tile border are never merged, so neighbouring tiles stay watertight whatever LOD
//     each of them uses,
//   - triangles that collapse (two corners in the same cluster) are dropped.
// Pure CPU, no D3D types: it runs on a worker thread (see draw.cpp: update_terrain_lods).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

namespace omsi::gfx::dx12::terrain_lod {

constexpr int kLevels = 4;
// Cluster cell = (tile width / 60 grid steps) * kCellMult[level]. Level 1 (ring 1) already removes every
// sub-grid sliver of the polycut; levels 2..4 halve the resolution again each ring.
constexpr float kCellMult[kLevels] = {1.f, 2.f, 4.f, 8.f};
// A plain 61x61 tile is ~21.6 k indices: never worth a LOD. Only heavy (cut) tiles qualify.
constexpr std::uint32_t kMinIndicesForLod = 90000u;
// Hardening against "big city / whole-map" meshes: a terrain tile is at most ~1 km on a side, roughly square
// and densely tessellated. Clustering a sparse city shell collapses its silhouettes / UV islands.
constexpr float kMaxTileExtent = 1200.f;    // metres per side; bigger = not a tile
constexpr float kMaxTileAspect = 2.5f;      // tiles are square, roads / city shells are long and thin
constexpr float kMinTriPerM2 = 0.15f;       // tiles are a regular grid; composite meshes are sparse

[[nodiscard]] inline bool is_lod_candidate(std::uint32_t index_count, const float bmin[3], const float bmax[3]) {
    if (index_count < kMinIndicesForLod) return false;
    const float wx = bmax[0] - bmin[0];
    const float wy = bmax[1] - bmin[1];
    const float wz = bmax[2] - bmin[2];
    if (wx < 100.f || wy < 100.f) return false;      // tile sized
    if (wx > kMaxTileExtent || wy > kMaxTileExtent) return false; // map / city sized, never a tile
    if ((std::max)(wx, wy) > kMaxTileAspect * (std::min)(wx, wy)) return false; // long thin strip, not a tile
    const float area = wx * wy;
    if (area <= 0.f) return false;
    if (static_cast<float>(index_count / 3u) < kMinTriPerM2 * area) return false; // sparse: not a survey grid
    if (wz > 0.5f * (std::min)(wx, wy)) return false; // not a building/object
    return true;
}

// Builds one LOD index list. Returns false when nothing useful was produced.
inline bool build_cluster_lod(const std::vector<DirectX::XMFLOAT3>& pos, const std::vector<std::uint32_t>& idx,
                              float minx, float miny, float maxx, float maxy, float cell,
                              std::vector<std::uint32_t>& out) {
    out.clear();
    const std::size_t vcount = pos.size();
    if (vcount == 0 || idx.size() < 3 || cell < 1.e-3f) return false;
    const float inv = 1.f / cell;
    constexpr float kEdgeEps = 0.02f;

    std::vector<std::uint32_t> cluster_of(vcount);
    std::unordered_map<std::uint64_t, std::uint32_t> table;
    table.reserve(vcount / 6 + 64);
    std::vector<float> sum_x, sum_y;
    std::vector<std::uint32_t> cnt;
    std::vector<std::uint8_t> is_border;
    sum_x.reserve(vcount / 6 + 64);
    sum_y.reserve(vcount / 6 + 64);
    cnt.reserve(vcount / 6 + 64);
    is_border.reserve(vcount / 6 + 64);

    for (std::size_t v = 0; v < vcount; ++v) {
        const float x = pos[v].x;
        const float y = pos[v].y;
        const bool border = (x - minx < kEdgeEps) || (maxx - x < kEdgeEps) || (y - miny < kEdgeEps) ||
                            (maxy - y < kEdgeEps);
        std::uint64_t key;
        if (border) {
            // exact position (1 mm) -> duplicated per-cell border vertices still merge, nothing else does
            const std::uint64_t qx = static_cast<std::uint64_t>(std::llround((x - minx) * 1000.f)) & 0x3FFFFFFFull;
            const std::uint64_t qy = static_cast<std::uint64_t>(std::llround((y - miny) * 1000.f)) & 0x3FFFFFFFull;
            key = (1ull << 63) | (qx << 31) | qy;
        } else {
            const std::int64_t cx = static_cast<std::int64_t>(std::floor((x - minx) * inv));
            const std::int64_t cy = static_cast<std::int64_t>(std::floor((y - miny) * inv));
            key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
                  static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy));
        }
        auto it = table.find(key);
        std::uint32_t c;
        if (it == table.end()) {
            c = static_cast<std::uint32_t>(sum_x.size());
            table.emplace(key, c);
            sum_x.push_back(0.f);
            sum_y.push_back(0.f);
            cnt.push_back(0u);
            is_border.push_back(border ? 1 : 0);
        } else {
            c = it->second;
        }
        cluster_of[v] = c;
        sum_x[c] += x;
        sum_y[c] += y;
        ++cnt[c];
    }

    // Representative = original vertex nearest to the cluster centroid.
    const std::size_t ncl = sum_x.size();
    std::vector<std::uint32_t> rep(ncl, 0xFFFFFFFFu);
    std::vector<float> best(ncl, 3.4e38f);
    for (std::size_t v = 0; v < vcount; ++v) {
        const std::uint32_t c = cluster_of[v];
        const float mx = sum_x[c] / static_cast<float>(cnt[c]);
        const float my = sum_y[c] / static_cast<float>(cnt[c]);
        const float dx = pos[v].x - mx;
        const float dy = pos[v].y - my;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best[c]) {
            best[c] = d2;
            rep[c] = static_cast<std::uint32_t>(v);
        }
    }

    out.reserve(idx.size() / 4 + 64);
    const std::size_t tri_n = idx.size() / 3;
    for (std::size_t t = 0; t < tri_n; ++t) {
        const std::uint32_t i0 = idx[t * 3 + 0], i1 = idx[t * 3 + 1], i2 = idx[t * 3 + 2];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
        const std::uint32_t c0 = cluster_of[i0], c1 = cluster_of[i1], c2 = cluster_of[i2];
        if (c0 == c1 || c1 == c2 || c0 == c2) continue; // collapsed
        out.push_back(rep[c0]);
        out.push_back(rep[c1]);
        out.push_back(rep[c2]);
    }
    return out.size() >= 3;
}

// All levels in one job (one snapshot copy of the mesh, then kLevels cheap passes).
inline void build_all_levels(const std::vector<DirectX::XMFLOAT3>& pos, const std::vector<std::uint32_t>& idx,
                             float minx, float miny, float maxx, float maxy,
                             std::vector<std::uint32_t> out[kLevels]) {
    const float width = (std::max)(maxx - minx, maxy - miny);
    const float step = width / 60.f;
    for (int L = 0; L < kLevels; ++L) {
        if (!build_cluster_lod(pos, idx, minx, miny, maxx, maxy, step * kCellMult[L], out[L])) {
            out[L].clear();
            continue;
        }
        // A level that barely reduces the mesh is not worth switching to.
        if (out[L].size() * 10u > idx.size() * 9u) out[L].clear();
    }
}

} // namespace omsi::gfx::dx12::terrain_lod
