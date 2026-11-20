#include "omsi/crossing_height_deform.hpp"

#include "o3d/o3d_loader.hpp"
#include "omsi/logger.hpp"
#include "omsi/spline_types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace omsi::sco {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.f;
constexpr float kTriEps = -0.001f;

std::mutex g_base_cache_mu;
std::unordered_map<std::string, CrossingHeightBaseMesh> g_base_cache;

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string path_key(const std::filesystem::path& p) {
    auto s = p.lexically_normal().string();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool point_in_triangle_xy(float px, float py, float ax, float ay, float bx, float by, float cx, float cy,
                          float& w_a, float& w_b, float& w_c) {
    const float denom = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (std::abs(denom) < 1.e-8f) return false;
    w_a = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / denom;
    w_b = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / denom;
    w_c = 1.f - w_a - w_b;
    if (w_a < kTriEps || w_b < kTriEps || w_c < kTriEps) return false;
    return true;
}

void triangle_normal(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz,
                     float& nx, float& ny, float& nz) {
    const float e1x = bx - ax;
    const float e1y = by - ay;
    const float e1z = bz - az;
    const float e2x = cx - ax;
    const float e2y = cy - ay;
    const float e2z = cz - az;
    nx = e1y * e2z - e1z * e2y;
    ny = e1z * e2x - e1x * e2z;
    nz = e1x * e2y - e1y * e2x;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1.e-8f) {
        nx /= len;
        ny /= len;
        nz /= len;
    } else {
        nx = 0.f;
        ny = 0.f;
        nz = 1.f;
    }
    if (nz < 0.f) {
        nx = -nx;
        ny = -ny;
        nz = -nz;
    }
}

bool top_surface_at_xy(float x, float y, float z_ceiling, const CrossingHeightBaseMesh& base, CrossingHeightHit& out) {
    if (!base.has_triangles()) return false;
    float best_z = -std::numeric_limits<float>::infinity();
    bool found = false;
    CrossingHeightHit best{};
    for (std::size_t ti = 0; ti + 2 < base.triangle_indices.size(); ti += 3) {
        const auto& va = base.vertices[base.triangle_indices[ti]];
        const auto& vb = base.vertices[base.triangle_indices[ti + 1]];
        const auto& vc = base.vertices[base.triangle_indices[ti + 2]];
        float w_a = 0.f, w_b = 0.f, w_c = 0.f;
        if (!point_in_triangle_xy(x, y, va[0], va[1], vb[0], vb[1], vc[0], vc[1], w_a, w_b, w_c)) continue;
        const float z_hit = w_a * va[2] + w_b * vb[2] + w_c * vc[2];
        if (z_hit > z_ceiling + 1.e-4f) continue;
        if (z_hit > best_z) {
            best_z = z_hit;
            best.z = z_hit;
            triangle_normal(va[0], va[1], va[2], vb[0], vb[1], vb[2], vc[0], vc[1], vc[2], best.nx, best.ny,
                            best.nz);
            best.triangle_index = static_cast<std::uint32_t>(ti / 3);
            found = true;
        }
    }
    if (!found) return false;
    out = best;
    return true;
}

// OMSI GetHeightAbove: height_above = ray_offset - hit_z → delta = hit_z (additive on flat crossing z=0).
float crossing_height_delta(const float hit_z, const float ray_offset) {
    return ray_offset - (ray_offset - hit_z);
}

void adjust_path_grad_from_normal(float& grad_start, float& grad_end, float nx, float ny, float nz,
                                  float heading_deg, bool at_end) {
    if (std::abs(nz) < 1.e-5f) return;
    const float rad = heading_deg * kDeg;
    const float fx = std::sin(rad);
    const float fy = std::cos(rad);
    const float lx = fy;
    const float ly = -fx;
    const float lat = (nx * lx + ny * ly) / nz * 100.f;
    const float fwd = (nx * fx + ny * fy) / nz * 100.f;
    // OMSI _Unit128_sub_007BA930: start adjusts +0x18/+0x20, end adjusts +0x1c/+0x24 (grad_start/grad_end pair).
    if (at_end) {
        grad_end -= lat;
        grad_start += fwd;
    } else {
        grad_start -= lat;
        grad_end += fwd;
    }
}

float path_end_heading_deg(const ScoPath& path, const float length) {
    if (std::abs(path.radius) < 1.e-6f) return path.heading_deg;
    return path.heading_deg + (length / path.radius) * (180.f / kPi);
}

} // namespace

map::SplineInst sco_path_to_spline_inst(const ScoPath& path) {
    map::SplineInst sp{};
    sp.position.x = path.x;
    sp.position.y = path.y;
    sp.position.z = path.z;
    sp.heading_deg = path.heading_deg;
    sp.radius = path.radius;
    sp.length = std::max(std::abs(path.length), 0.05f);
    // a .sco [path]'s gradients are fractions (0.04 = 4 %: openOMSI object_path_gradients test, 20 m at
    // 0.04 rises 0.8 m); the spline's are percent - taken as they stand the paths came out flat
    sp.start_grad = path.grad_start * 100.f;
    sp.end_grad = path.grad_end * 100.f;
    if (path.height_deformed) {
        sp.use_delta_height = true;
        sp.is_spline_h = true;
        sp.delta_height = path.height_delta;
    }
    return sp;
}

std::filesystem::path resolve_sco_o3d(const std::filesystem::path& sco_dir, std::string_view rel) {
    if (rel.empty()) return {};
    std::filesystem::path p(rel);
    if (p.is_absolute() && std::filesystem::exists(p)) return p;
    const auto direct = sco_dir / p;
    if (std::filesystem::exists(direct)) return direct;
    const auto in_model = sco_dir / "model" / p.filename();
    if (std::filesystem::exists(in_model)) return in_model;
    return direct;
}

bool mesh_stem_matches(std::string_view mesh_stem, std::string_view ref_stem) {
    if (ref_stem.empty()) return false;
    return ieq(mesh_stem, ref_stem);
}

bool is_crossing_height_base_mesh(const SceneryObject& sco, const std::filesystem::path& mesh_path) {
    if (sco.crossing_height_deformation_mesh.empty()) return false;
    const auto base_stem = std::filesystem::path(sco.crossing_height_deformation_mesh).stem().string();
    return mesh_stem_matches(mesh_path.stem().string(), base_stem) ||
           ieq(mesh_path.filename().string(), sco.crossing_height_deformation_mesh);
}

bool should_deform_crossing_mesh(const SceneryObject& sco, const std::filesystem::path& mesh_path) {
    if (sco.crossing_height_deformation_mesh.empty()) return false;
    return !is_crossing_height_base_mesh(sco, mesh_path);
}

std::optional<CrossingHeightBaseMesh> load_crossing_height_base(const std::filesystem::path& o3d_path) {
    if (o3d_path.empty()) return std::nullopt;
    const auto key = path_key(o3d_path);
    {
        std::lock_guard lock(g_base_cache_mu);
        if (const auto it = g_base_cache.find(key); it != g_base_cache.end()) return it->second;
    }

    auto mapped = o3d::load_mapped_shared(o3d_path);
    if (!mapped || !mapped->valid()) return std::nullopt;

    CrossingHeightBaseMesh out{};
    const auto& mesh = mapped->mesh();
    const auto decrypted = o3d::decrypt_o3d_vertices(mesh);
    out.vertices.reserve(decrypted.size());
    for (const auto& raw : decrypted) {
        const auto v = o3d::prepare_o3d_vertex_for_gpu(raw, mesh);
        out.vertices.push_back({v.position[0], v.position[1], v.position[2]});
    }

    if (mesh.long_triangle_indices) {
        const auto tris = mesh.triangle_u32_span();
        out.triangle_indices.reserve(static_cast<std::size_t>(mesh.triangle_count) * 3);
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            out.triangle_indices.push_back(tris[t].i0);
            out.triangle_indices.push_back(tris[t].i1);
            out.triangle_indices.push_back(tris[t].i2);
        }
    } else {
        const auto tris = mesh.triangle_u16_span();
        out.triangle_indices.reserve(static_cast<std::size_t>(mesh.triangle_count) * 3);
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            out.triangle_indices.push_back(tris[t].i0);
            out.triangle_indices.push_back(tris[t].i1);
            out.triangle_indices.push_back(tris[t].i2);
        }
    }

    {
        std::lock_guard lock(g_base_cache_mu);
        g_base_cache.emplace(key, out);
    }
    return out;
}

std::optional<CrossingHeightHit> raycast_crossing_height(const float x, const float y, const float z,
                                                         const float ray_offset,
                                                         const CrossingHeightBaseMesh& base) {
    CrossingHeightHit hit{};
    if (!top_surface_at_xy(x, y, z + ray_offset, base, hit)) return std::nullopt;
    return hit;
}

std::optional<std::vector<o3d::Vertex>> deform_o3d_for_crossing(const o3d::NativeMesh& mesh,
                                                                const CrossingHeightBaseMesh& base,
                                                                CrossingMeshDeformStats* stats_out) {
    const auto decrypted = o3d::decrypt_o3d_vertices(mesh);
    if (decrypted.empty()) return std::nullopt;
    std::vector<o3d::Vertex> out;
    out.reserve(decrypted.size());
    for (const auto& raw : decrypted) {
        auto v = o3d::prepare_o3d_vertex_for_gpu(raw, mesh);
        const float orig_z = v.position[2];
        if (const auto hit = raycast_crossing_height(v.position[0], v.position[1], v.position[2],
                                                     kCrossingHeightRayVertex, base)) {
            // OMSI LoadMeshFromPreMesh: y += (300 - height_above) = hit_y — preserve local offset (0.1→1.1).
            v.position[2] += crossing_height_delta(hit->z, kCrossingHeightRayVertex);
            if (stats_out) {
                ++stats_out->hit_verts;
                stats_out->max_abs_dz = std::max(stats_out->max_abs_dz, std::abs(v.position[2] - orig_z));
            }
        }
        out.push_back(v);
    }
    if (stats_out) stats_out->total_verts = out.size();
    return out;
}

CrossingPathDeformStats deform_sco_paths(SceneryObject& sco, const CrossingHeightBaseMesh& base) {
    CrossingPathDeformStats stats{};
    stats.total_paths = sco.paths.size();
    for (auto& path : sco.paths) {
        const float orig_z = path.z;
        const float len = std::max(std::abs(path.length), 0.05f);

        map::SplineInst flat{};
        flat.position.x = path.x;
        flat.position.y = path.y;
        flat.position.z = orig_z;
        flat.heading_deg = path.heading_deg;
        flat.radius = path.radius;
        flat.length = len;
        flat.start_grad = path.grad_start;
        flat.end_grad = path.grad_end;
        const auto end_flat = spline::evaluate_spline(flat, len);

        bool start_hit = false;
        if (const auto hit0 = raycast_crossing_height(path.x, path.y, orig_z, kCrossingHeightRayVertex, base)) {
            path.z = orig_z + crossing_height_delta(hit0->z, kCrossingHeightRayVertex);
            adjust_path_grad_from_normal(path.grad_start, path.grad_end, hit0->nx, hit0->ny, hit0->nz,
                                         path.heading_deg, false);
            start_hit = true;
            ++stats.start_hits;
        }

        const float end_heading = path_end_heading_deg(path, len);
        if (const auto hit1 = raycast_crossing_height(end_flat.x, end_flat.y, end_flat.z,
                                                      kCrossingHeightRayPathEnd, base)) {
            adjust_path_grad_from_normal(path.grad_start, path.grad_end, hit1->nx, hit1->ny, hit1->nz, end_heading,
                                         true);
            if (start_hit) {
                const float deformed_end_z = end_flat.z + crossing_height_delta(hit1->z, kCrossingHeightRayPathEnd);
                path.height_delta = deformed_end_z - path.z;
                path.height_deformed = true;
            }
        } else if (start_hit) {
            path.height_delta = end_flat.z - path.z;
            path.height_deformed = true;
        }
        if (path.height_deformed) ++stats.height_deformed;
    }
    return stats;
}

void apply_crossing_height_to_sco(SceneryObject& sco) {
    if (sco.crossing_height_deformation_mesh.empty()) return;
    const auto base_path = resolve_sco_o3d(sco.sco_path.parent_path(), sco.crossing_height_deformation_mesh);
    const auto base = load_crossing_height_base(base_path);
    if (!base) return;
    if (sco.paths.empty()) return;
    const auto ps = deform_sco_paths(sco, *base);
    log::info("crossingH paths sco=" + sco.sco_path.filename().string() + " total=" +
              std::to_string(ps.total_paths) + " start_hits=" + std::to_string(ps.start_hits) +
              " deformed=" + std::to_string(ps.height_deformed));
}

} // namespace omsi::sco
