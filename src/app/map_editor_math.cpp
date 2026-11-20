#include "app/map_editor_math.hpp"

#include "omsi/map_attachment.hpp"
#include "omsi/logger.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/spline_types.hpp"
#include "omsi/x_mesh.hpp"
#include "o3d/o3d_loader.hpp"

#include <DirectXMath.h>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <string>

namespace omsi::app::map_editor_math {
using namespace DirectX;

namespace {

f4f23ma::f4 quat_rotate_v(const f4f23ma::f4& q, const f4f23ma::f4& v) {
    const f4f23ma::f4 qv{q.x, q.y, q.z, 0.f};
    const f4f23ma::f4 t = qv.cross3(v) * 2.f;
    return v + t * q.w + qv.cross3(t);
}

} // namespace

std::array<float, 16> store_world_transposed(const XMMATRIX& world) {
    XMFLOAT4X4 stored{};
    XMStoreFloat4x4(&stored, XMMatrixTranspose(world));
    std::array<float, 16> out{};
    std::memcpy(out.data(), &stored, sizeof(stored));
    return out;
}

XMMATRIX ma4_to_xm(const f4f23ma::ma4& m) {
    XMFLOAT4X4 f{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) f.m[r][c] = m.m[c][r];
    return DirectX::XMLoadFloat4x4(&f);
}

XMMATRIX omsi_object_rotation_matrix(const float yaw_deg, const float pitch_deg, const float bank_deg) {
    constexpr float kDeg = 3.14159265f / 180.f;
    // Column vectors: M*v applies Rz first, then Rx on local X, then Ry on local Y.
    // Bank sign inverted vs .map literal — OMSI bank 0.42 renders as Ry(-0.42).
    return XMMatrixRotationY(-bank_deg * kDeg) * XMMatrixRotationX(-pitch_deg * kDeg) *
           XMMatrixRotationZ(-yaw_deg * kDeg);
}

XMMATRIX sco_object_base_matrix(const map::MapObjectInst& obj, float terrain_h, float tile_ox, float tile_oy) {
    const XMMATRIX R =
        omsi_object_rotation_matrix(obj.rotation_deg.x, obj.rotation_deg.y, obj.rotation_deg.z);
    return R * XMMatrixTranslation(obj.position.x + tile_ox, obj.position.y + tile_oy, obj.position.z + terrain_h);
}

XMMATRIX sco_world_matrix(const map::MapObjectInst& obj, const o3d::MappedO3D* mesh, float terrain_h,
                          float tile_ox, float tile_oy) {
    const XMMATRIX base = sco_object_base_matrix(obj, terrain_h, tile_ox, tile_oy);
    if (!mesh) return base;
    const auto root = o3d::o3d_unity_root_transform(mesh->mesh().object_rows());
    return ma4_to_xm(root) * base;
}

bool sco_path_debug_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("OMSI_PATH_DEBUG");
        if (!e || !e[0]) return false;
        return e[0] == '1' || e[0] == 'y' || e[0] == 'Y' || e[0] == 't' || e[0] == 'T';
    }();
    return on;
}

void log_sco_path_placement(const map::MapObjectInst& obj, const sco::SceneryObject& sco, const sco::ScoPath& path,
                            const int path_index, const bool endpoint_start, const float terrain_h,
                            const float world_x, const float world_y, const float world_z, const char* site,
                            const float rail_lift_m) {
    if (!sco_path_debug_enabled()) return;

    static std::mutex logged_mu;
    static std::unordered_set<std::uint64_t> logged;
    const auto zq = static_cast<std::uint32_t>(std::lround(obj.position.z * 1000.f));
    const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(obj.id)) << 40) |
                              (static_cast<std::uint64_t>(static_cast<std::uint32_t>(path_index)) << 24) |
                              (static_cast<std::uint64_t>(zq) << 4) | (endpoint_start ? 2ull : 0ull) |
                              (std::hash<std::string>{}(site ? site : "") & 3ull);
    {
        std::lock_guard<std::mutex> lock(logged_mu);
        if (!logged.insert(key).second) return;
    }

    const XMMATRIX base_m = sco_world_matrix(obj, nullptr, terrain_h, 0.f, 0.f);
    DirectX::XMFLOAT3 origin{};
    XMStoreFloat3(&origin, xform_point(base_m, 0.f, 0.f, 0.f));

    float mesh_origin_z = origin.z;
    std::shared_ptr<const o3d::MappedO3D> o3d_holder;
    for (const auto& ref : sco.meshes) {
        if (!ref.is_o3d) continue;
        o3d_holder = o3d::load_mapped_shared(ref.mesh_file);
        if (!o3d_holder || !o3d_holder->valid()) continue;
        const XMMATRIX mesh_m = sco_world_matrix(obj, o3d_holder.get(), terrain_h, 0.f, 0.f);
        DirectX::XMFLOAT3 mesh_origin{};
        XMStoreFloat3(&mesh_origin, xform_point(mesh_m, 0.f, 0.f, 0.f));
        mesh_origin_z = mesh_origin.z;
        break;
    }

    const float naive_z = obj.position.z + terrain_h + path.z;
    const float mesh_delta_z = mesh_origin_z - origin.z;
    const float world_draw_z = world_z + rail_lift_m;

    std::ostringstream oss;
    oss << "pathdbg site=" << (site ? site : "?") << " obj=" << obj.id << " sco=" << sco.sco_path.filename().string()
        << " pi=" << path_index << " end=" << (endpoint_start ? "start" : "end") << " map_z=" << obj.position.z
        << " th=" << terrain_h << " skip_terr=" << (sco::skip_terrain_add(sco) ? 1 : 0) << " path_xyz=("
        << path.x << "," << path.y << "," << path.z << ") naive_z=" << naive_z << " (map+th+path.z sin rot)"
        << " origin_z=" << origin.z << " mesh_origin_z=" << mesh_origin_z << " mesh_delta_z=" << mesh_delta_z
        << " world_xyz=(" << world_x << "," << world_y << "," << world_z << ")";
    if (rail_lift_m > 1.e-5f) oss << " rail_lift=" << rail_lift_m << " draw_z=" << world_draw_z;
    if (path.height_deformed) oss << " h_deform=1 delta_h=" << path.height_delta;
    log::info(oss.str());
}

XMMATRIX entrypoint_world_matrix(const map::EntryPoint& ep, float tile_ox, float tile_oy) {
    constexpr float kDeg = 3.14159265f / 180.f;
    // OMSI quaternion is (x, y, z, w) and the heading is yaw around +Y-up:
    // heading = 2*atan2(q.y, q.w) (matches openOMSI omsi-map global.rs). Object rotation uses
    // RotationZ(-yaw) (see omsi_object_rotation_matrix), so entrypoints use the same convention.
    const float yaw = 2.f * std::atan2(ep.rotation_quat.y, ep.rotation_quat.w) * 180.f / 3.14159265f;
    const XMMATRIX R = XMMatrixRotationZ(-yaw * kDeg);
    const XMMATRIX T =
        XMMatrixTranslation(ep.position.x - tile_ox, ep.position.y - tile_oy, ep.position.z);
    return R * T;
}

void omsi_euler_from_matrix(const XMMATRIX& m, float& yaw_deg, float& pitch_deg, float& bank_deg) {
    // M = RotY(-bank) * RotX(-pitch) * RotZ(-yaw) (row vectors): M[1] = (-cP sY', cP cY', sP'),
    // M[0][2] = -sB' cP', M[2][2] = cB' cP' with the primed angles negated
    XMFLOAT4X4 f;
    DirectX::XMStoreFloat4x4(&f, m);
    constexpr float kRad = 180.f / 3.14159265f;
    const float sp = (std::clamp)(f._23, -1.f, 1.f);
    const float b = std::asin(sp);
    const float c = std::atan2(-f._21, f._22);
    const float a = std::atan2(-f._13, f._33);
    yaw_deg = -c * kRad;
    pitch_deg = -b * kRad;
    bank_deg = -a * kRad;
}

std::optional<XMMATRIX> attach_child_world_matrix(const map::MapAttachObjectInst& a,
                                                  const map::MapObjectInst* parent_obj,
                                                  const map::SplineInst* parent_spline, float parent_terrain_h,
                                                  std::optional<f4f23ma::f32> chain_end_z) {
    (void)parent_spline;
    (void)chain_end_z;
    if (!parent_obj) return std::nullopt;
    // The parent's [new_attachment] point; one the parent does not have (its object changed after the
    // map was made) is the parent's own origin, as in Omsi.exe / openOMSI - never the last point.
    XMMATRIX att = XMMatrixIdentity();
    if (const auto parent_sco = sco::load_sco(parent_obj->sco_path)) {
        const int idx = a.attach_point_index;
        if (idx >= 0 && idx < static_cast<int>(parent_sco->attachment_points.size())) {
            XMFLOAT4X4 f;
            std::memcpy(&f, parent_sco->attachment_points[static_cast<std::size_t>(idx)].m, sizeof(f));
            att = DirectX::XMLoadFloat4x4(&f);
        }
    }
    const XMMATRIX parent_m = sco_world_matrix(*parent_obj, nullptr, parent_terrain_h, 0.f, 0.f);
    // openOMSI Pose::attached (Omsi.exe 0x79d4c4..0x79d689): the point's place turned and moved with
    // the parent; the turn is the D3DX product point x own x parent - the parent's rotation, then the
    // point's, then the object's own (heading / pitch / bank of the record), as rotations of the
    // world axes. For turns about the vertical alone this is the plain hierarchy.
    XMFLOAT4X4 af;
    DirectX::XMStoreFloat4x4(&af, att);
    const XMVECTOR origin = DirectX::XMVector3TransformCoord(XMVectorSet(af._41, af._42, af._43, 1.f), parent_m);
    XMMATRIX att_r = att;
    att_r.r[3] = XMVectorSet(0.f, 0.f, 0.f, 1.f);
    XMMATRIX parent_r = parent_m;
    parent_r.r[3] = XMVectorSet(0.f, 0.f, 0.f, 1.f);
    const XMMATRIX own = omsi_object_rotation_matrix(a.extra_rot_x, a.extra_rot_y2, a.extra_rot_z);
    XMMATRIX world = parent_r * att_r * own;
    world.r[3] = XMVectorSetW(origin, 1.f);
    return world;
}

DirectX::XMVECTOR xform_point(const XMMATRIX& m, float x, float y, float z) {
    return DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(x, y, z, 1.f), m);
}

void make_octahedron(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices, float r) {
    verts.clear();
    indices.clear();
    const float p[6][3] = {{r, 0, 0}, {-r, 0, 0}, {0, r, 0}, {0, -r, 0}, {0, 0, r}, {0, 0, -r}};
    for (int i = 0; i < 6; ++i) {
        o3d::Vertex v{};
        v.position[0] = p[i][0];
        v.position[1] = p[i][1];
        v.position[2] = p[i][2];
        v.normal[0] = p[i][0] / r;
        v.normal[1] = p[i][1] / r;
        v.normal[2] = p[i][2] / r;
        verts.push_back(v);
    }
    const std::uint32_t faces[][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                                     {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (const auto& f : faces) {
        indices.push_back(f[0]);
        indices.push_back(f[1]);
        indices.push_back(f[2]);
    }
}

void make_thin_cylinder(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices, float radius,
                        float height, int segments) {
    verts.clear();
    indices.clear();
    const float z0 = 0.f;
    const float z1 = height;
    for (int i = 0; i < segments; ++i) {
        const float a0 = (static_cast<float>(i) / static_cast<float>(segments)) * 6.2831853f;
        const float a1 = (static_cast<float>(i + 1) / static_cast<float>(segments)) * 6.2831853f;
        const float x0 = std::cos(a0) * radius;
        const float y0 = std::sin(a0) * radius;
        const float x1 = std::cos(a1) * radius;
        const float y1 = std::sin(a1) * radius;
        const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
        o3d::Vertex v[4]{};
        v[0].position[0] = x0;
        v[0].position[1] = y0;
        v[0].position[2] = z0;
        v[1].position[0] = x1;
        v[1].position[1] = y1;
        v[1].position[2] = z0;
        v[2].position[0] = x1;
        v[2].position[1] = y1;
        v[2].position[2] = z1;
        v[3].position[0] = x0;
        v[3].position[1] = y0;
        v[3].position[2] = z1;
        const float nx = (x0 + x1) * 0.5f;
        const float ny = (y0 + y1) * 0.5f;
        const float inv = 1.f / (std::max)(1.e-6f, std::sqrt(nx * nx + ny * ny));
        for (int k = 0; k < 4; ++k) {
            v[k].normal[0] = nx * inv;
            v[k].normal[1] = ny * inv;
            v[k].normal[2] = 0.f;
            verts.push_back(v[k]);
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

void make_uv_sphere(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices, float radius, int segments) {
    verts.clear();
    indices.clear();
    const int stacks = (std::max)(2, segments / 2);
    const int slices = (std::max)(4, segments);
    for (int i = 0; i <= stacks; ++i) {
        const float v = static_cast<float>(i) / static_cast<float>(stacks);
        const float phi = v * 3.14159265f;
        const float z = std::cos(phi) * radius;
        const float r = std::sin(phi) * radius;
        for (int j = 0; j <= slices; ++j) {
            const float u = static_cast<float>(j) / static_cast<float>(slices);
            const float theta = u * 6.2831853f;
            o3d::Vertex vert{};
            vert.position[0] = r * std::cos(theta);
            vert.position[1] = r * std::sin(theta);
            vert.position[2] = z;
            const float inv = 1.f / (std::max)(1.e-6f, radius);
            vert.normal[0] = vert.position[0] * inv;
            vert.normal[1] = vert.position[1] * inv;
            vert.normal[2] = vert.position[2] * inv;
            verts.push_back(vert);
        }
    }
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(i * (slices + 1) + j);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + static_cast<std::uint32_t>(slices + 1);
            const std::uint32_t i3 = i2 + 1;
            indices.insert(indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
}

void make_arrow_mesh(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices) {
    verts.clear();
    indices.clear();
    auto push_tri = [&](float x0, float y0, float x1, float y1, float x2, float y2) {
        const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
        const float pts[3][2] = {{x0, y0}, {x1, y1}, {x2, y2}};
        for (const auto& p : pts) {
            o3d::Vertex v{};
            v.position[0] = p[0];
            v.position[1] = p[1];
            v.position[2] = 0.f;
            v.normal[0] = 0.f;
            v.normal[1] = 0.f;
            v.normal[2] = 1.f;
            verts.push_back(v);
        }
        indices.insert(indices.end(), {base, base + 1, base + 2});
    };
    const float shaft_hw = 0.11f;
    const float head_hw = 0.27f;
    const float shaft_top = 0.52f;
    const float tip_y = 1.f;
    push_tri(-shaft_hw, 0.f, shaft_hw, 0.f, shaft_hw, shaft_top);
    push_tri(-shaft_hw, 0.f, shaft_hw, shaft_top, -shaft_hw, shaft_top);
    push_tri(-head_hw, shaft_top, head_hw, shaft_top, 0.f, tip_y);
    const std::uint32_t n = static_cast<std::uint32_t>(indices.size());
    for (std::uint32_t i = 0; i < n; i += 3)
        indices.insert(indices.end(), {indices[i + 2], indices[i + 1], indices[i]});
}

// Solid 3D arrow along +Y (base y=0, tip y=1): octagonal shaft + cone head, centred on the Y axis.
void make_arrow_mesh_3d(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices) {
    verts.clear();
    indices.clear();
    constexpr int kSeg = 12;
    constexpr float kShaftR = 0.07f, kHeadR = 0.2f, kShaftTop = 0.58f, kTip = 1.f;
    const float two_pi = 6.2831853f;
    auto push = [&](float x, float y, float z, float nx, float ny, float nz) {
        o3d::Vertex v{};
        v.position[0] = x;
        v.position[1] = y;
        v.position[2] = z;
        v.normal[0] = nx;
        v.normal[1] = ny;
        v.normal[2] = nz;
        verts.push_back(v);
        return static_cast<std::uint32_t>(verts.size() - 1);
    };
    // ring k at y with radius r (x/z plane), outward normals
    auto ring = [&](float y, float r, float ny) {
        const std::uint32_t first = static_cast<std::uint32_t>(verts.size());
        for (int k = 0; k < kSeg; ++k) {
            const float a = two_pi * static_cast<float>(k) / kSeg;
            const float c = std::cos(a), s = std::sin(a);
            push(c * r, y, s * r, c, ny, s);
        }
        return first;
    };
    auto quad_strip = [&](std::uint32_t a, std::uint32_t b) {
        for (int k = 0; k < kSeg; ++k) {
            const std::uint32_t k1 = static_cast<std::uint32_t>((k + 1) % kSeg);
            indices.insert(indices.end(), {a + k, b + k, b + k1, a + k, b + k1, a + k1});
        }
    };
    auto cap = [&](std::uint32_t r0, float y, float ny) { // fan around a centre vertex
        const std::uint32_t c = push(0.f, y, 0.f, 0.f, ny, 0.f);
        for (int k = 0; k < kSeg; ++k) {
            const std::uint32_t k1 = static_cast<std::uint32_t>((k + 1) % kSeg);
            indices.insert(indices.end(), {c, r0 + k1, r0 + k});
        }
    };
    const std::uint32_t s0 = ring(0.f, kShaftR, 0.f);
    const std::uint32_t s1 = ring(kShaftTop, kShaftR, 0.f);
    quad_strip(s0, s1);
    cap(s0, 0.f, -1.f);
    const std::uint32_t h0 = ring(kShaftTop, kHeadR, 0.45f);
    cap(h0, kShaftTop, -1.f);
    const std::uint32_t tip = push(0.f, kTip, 0.f, 0.f, 1.f, 0.f);
    for (int k = 0; k < kSeg; ++k) {
        const std::uint32_t k1 = static_cast<std::uint32_t>((k + 1) % kSeg);
        indices.insert(indices.end(), {h0 + k, tip, h0 + k1});
    }
    // both windings: the arrow is seen from every side whatever the PSO culls
    const std::size_t n = indices.size();
    for (std::size_t i = 0; i < n; i += 3) indices.insert(indices.end(), {indices[i + 2], indices[i + 1], indices[i]});
}

void mesh_local_bbox(const o3d::MappedO3D& mapped, float out_min[3], float out_max[3]) {
    const auto& mesh = mapped.mesh();
    const auto decrypted = o3d::decrypt_o3d_vertices(mesh);
    if (decrypted.empty()) {
        out_min[0] = out_min[1] = out_min[2] = out_max[0] = out_max[1] = out_max[2] = 0.f;
        return;
    }
    const auto v0 = o3d::prepare_o3d_vertex_for_gpu(decrypted[0], mesh);
    out_min[0] = out_max[0] = v0.position[0];
    out_min[1] = out_max[1] = v0.position[1];
    out_min[2] = out_max[2] = v0.position[2];
    for (const auto& raw : decrypted) {
        const auto v = o3d::prepare_o3d_vertex_for_gpu(raw, mesh);
        for (int a = 0; a < 3; ++a) {
            out_min[a] = (std::min)(out_min[a], v.position[a]);
            out_max[a] = (std::max)(out_max[a], v.position[a]);
        }
    }
}

SpawnDebug make_spawn_debug(const map::MapObjectInst& obj, const char* kind, const std::filesystem::path& path,
                            const XMMATRIX& world_m, float terr_h, float tile_ox, float tile_oy,
                            const o3d::MappedO3D* mesh, float local_hx, float local_hy, float local_hz) {
    SpawnDebug d{};
    d.id = obj.id;
    d.kind = kind;
    d.path = path.filename().string();
    d.map_x = obj.position.x;
    d.map_y = obj.position.y;
    d.map_z = obj.position.z;
    d.terr_h = terr_h;
    d.world_ox = tile_ox;
    d.world_oy = tile_oy;
    d.expect_x = tile_ox + obj.position.x;
    d.expect_y = tile_oy + obj.position.y;
    d.expect_z = obj.position.z + terr_h;

    DirectX::XMFLOAT3 p0{};
    DirectX::XMStoreFloat3(&p0, xform_point(world_m, 0.f, 0.f, 0.f));
    d.pivot_x = p0.x;
    d.pivot_y = p0.y;
    d.pivot_z = p0.z;

    float lmin[3]{0, 0, 0}, lmax[3]{0, 0, 0};
    if (mesh) mesh_local_bbox(*mesh, lmin, lmax);
    else {
        const float hx = local_hx > 0.f ? local_hx : 1.f;
        const float hy = local_hy > 0.f ? local_hy : hx;
        const float hz = local_hz > 0.f ? local_hz : 1.f;
        lmin[0] = -hx;
        lmin[1] = -hy;
        lmin[2] = 0.f;
        lmax[0] = hx;
        lmax[1] = hy;
        lmax[2] = hz;
    }
    float wmin[3]{1e9f, 1e9f, 1e9f}, wmax[3]{-1e9f, -1e9f, -1e9f};
    const float corners[8][3] = {
        {lmin[0], lmin[1], lmin[2]}, {lmax[0], lmin[1], lmin[2]}, {lmin[0], lmax[1], lmin[2]},
        {lmax[0], lmax[1], lmin[2]}, {lmin[0], lmin[1], lmax[2]}, {lmax[0], lmin[1], lmax[2]},
        {lmin[0], lmax[1], lmax[2]}, {lmax[0], lmax[1], lmax[2]},
    };
    for (const auto& c : corners) {
        DirectX::XMFLOAT3 w{};
        DirectX::XMStoreFloat3(&w, xform_point(world_m, c[0], c[1], c[2]));
        wmin[0] = (std::min)(wmin[0], w.x);
        wmin[1] = (std::min)(wmin[1], w.y);
        wmin[2] = (std::min)(wmin[2], w.z);
        wmax[0] = (std::max)(wmax[0], w.x);
        wmax[1] = (std::max)(wmax[1], w.y);
        wmax[2] = (std::max)(wmax[2], w.z);
    }
    for (int a = 0; a < 3; ++a) {
        d.bbox_min[a] = wmin[a];
        d.bbox_max[a] = wmax[a];
    }

    // FIX 2026-09-24: compare pivot (world) vs expect (tile_ox+local), not vs local-only obj.position.
    // Previous formula marked almost every multi-tile object as SPAWN BAD and hid real issues.
    const float map_delta =
        std::sqrt((d.pivot_x - d.expect_x) * (d.pivot_x - d.expect_x) +
                  (d.pivot_y - d.expect_y) * (d.pivot_y - d.expect_y) +
                  (d.pivot_z - d.expect_z) * (d.pivot_z - d.expect_z));
    d.delta = map_delta;
    d.ok = map_delta < 2.5f;
    d.instance_scale = 1.f;
    if (mesh) {
        o3d::o3d_unity_root_column_scale(mesh->mesh().object_rows(), d.scale_x, d.scale_y, d.scale_z);
    } else if (kind == std::string("tree")) {
        d.scale_x = local_hx > 0.f ? local_hx * 2.f : 1.f;
        d.scale_y = local_hy > 0.f ? local_hy * 2.f : d.scale_x;
        d.scale_z = local_hz > 0.f ? local_hz : 1.f;
        d.instance_scale = d.scale_z;
    }
    return d;
}

void log_spawn_debug(const SpawnDebug& d) {
    if (!d.ok) return;
    std::ostringstream oss;
    oss << "SPAWN OK id=" << d.id << " kind=" << d.kind << " file=" << d.path;
    omsi::log::info(oss.str());
}

namespace {

struct WorldPoint {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct LocalMeshPart {
    std::vector<std::array<float, 3>> local{};
    f4f23ma::ma4 root = f4f23ma::ma4::identity();
    bool has_root = false;
};

std::unordered_map<std::string, std::vector<LocalMeshPart>> g_sco_local_mesh_cache;

const std::vector<LocalMeshPart>& sco_local_mesh_parts(const sco::SceneryObject& sco) {
    std::string cache_key;
    for (const auto& ref : sco.meshes) {
        cache_key += ref.mesh_file.lexically_normal().string();
        cache_key += ref.is_o3d ? 'o' : 'x';
    }
    if (const auto it = g_sco_local_mesh_cache.find(cache_key); it != g_sco_local_mesh_cache.end())
        return it->second;

    std::vector<LocalMeshPart> parts;
    parts.reserve(sco.meshes.size());
    for (const auto& ref : sco.meshes) {
        if (ref.is_o3d) {
            const auto stem = ref.mesh_file.stem().string();
            if (stem == "empty_lod" || stem == "empty") continue;
            auto mapped = o3d::load_mapped_shared(ref.mesh_file);
            if (!mapped || !mapped->valid()) continue;
            LocalMeshPart part{};
            part.root = o3d::o3d_unity_root_transform(mapped->mesh().object_rows());
            part.has_root = true;
            const auto decrypted = o3d::decrypt_o3d_vertices(mapped->mesh());
            part.local.reserve(decrypted.size());
            for (const auto& raw : decrypted) {
                const auto v = o3d::prepare_o3d_vertex_for_gpu(raw, mapped->mesh());
                part.local.push_back({v.position[0], v.position[1], v.position[2]});
            }
            if (!part.local.empty()) parts.push_back(std::move(part));
        } else if (!sco.tree.valid) {
            const auto xf = xfile::load_x_file(ref.mesh_file);
            if (!xf) continue;
            LocalMeshPart part{};
            for (const auto& xm : xf->parts) {
                for (const auto& v : xm.vertices)
                    part.local.push_back({v.position[0], v.position[1], v.position[2]});
            }
            if (!part.local.empty()) parts.push_back(std::move(part));
        }
    }
    return g_sco_local_mesh_cache.emplace(std::move(cache_key), std::move(parts)).first->second;
}

int64_t sample_grid_key(float x, float y, float quant) {
    const float q = (std::max)(quant, 0.05f);
    const int64_t ix = static_cast<int64_t>(std::llround(x / q));
    const int64_t iy = static_cast<int64_t>(std::llround(y / q));
    return (ix << 32) ^ (iy & 0xffffffffLL);
}

void transform_sco_to_world(const map::MapObjectInst& obj, const sco::SceneryObject& sco, float terrain_h,
                            float tile_ox, float tile_oy, std::vector<WorldPoint>& out) {
    out.clear();
    const XMMATRIX base = sco_object_base_matrix(obj, terrain_h, tile_ox, tile_oy);
    const auto& parts = sco_local_mesh_parts(sco);
    for (const auto& part : parts) {
        const XMMATRIX wm = part.has_root ? ma4_to_xm(part.root) * base : base;
        for (const auto& lp : part.local) {
            DirectX::XMFLOAT3 w{};
            DirectX::XMStoreFloat3(&w, xform_point(wm, lp[0], lp[1], lp[2]));
            out.push_back({w.x, w.y, w.z});
        }
    }
    if (out.empty()) {
        DirectX::XMFLOAT3 p{};
        DirectX::XMStoreFloat3(&p, xform_point(base, 0.f, 0.f, 0.f));
        out.push_back({p.x, p.y, p.z});
    }
}

void bottom_shell_vertices(const std::vector<WorldPoint>& verts, std::vector<WorldPoint>& out) {
    out.clear();
    if (verts.empty()) return;
    float min_z = verts[0].z, max_z = verts[0].z;
    for (const auto& v : verts) {
        min_z = (std::min)(min_z, v.z);
        max_z = (std::max)(max_z, v.z);
    }
    const float band = (std::max)(0.04f, (max_z - min_z) * 0.015f);
    out.reserve(verts.size() / 8 + 8);
    for (const auto& v : verts) {
        if (v.z <= min_z + band) out.push_back(v);
    }
    if (out.empty()) out = verts;
}

float sample_cached_z(float x, float y, const SurfaceFollowOptions& opt,
                      std::unordered_map<int64_t, float>& cache) {
    const int64_t key = sample_grid_key(x, y, opt.sample_quant_m);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    const float z = opt.sample_world_z(x, y);
    cache.emplace(key, z);
    return z;
}

bool sco_merged_sco_aabb(const sco::SceneryObject& sco, float out_min[3], float out_max[3]) {
    bool any = false;
    for (const auto& part : sco_local_mesh_parts(sco)) {
        for (const auto& lp : part.local) {
            f4f23ma::f4 p{lp[0], lp[1], lp[2], 1.f};
            if (part.has_root) p = part.root.transform_point(p);
            if (!any) {
                out_min[0] = out_max[0] = p.x;
                out_min[1] = out_max[1] = p.y;
                out_min[2] = out_max[2] = p.z;
                any = true;
            } else {
                out_min[0] = (std::min)(out_min[0], p.x);
                out_max[0] = (std::max)(out_max[0], p.x);
                out_min[1] = (std::min)(out_min[1], p.y);
                out_max[1] = (std::max)(out_max[1], p.y);
                out_min[2] = (std::min)(out_min[2], p.z);
                out_max[2] = (std::max)(out_max[2], p.z);
            }
        }
    }
    return any;
}

// Four bottom AABB corners → sample surface at each XY → pitch/bank (XNA terrain-follow recipe).
void align_pitch_bank_corners(const map::MapObjectInst& obj, const sco::SceneryObject& sco, float terrain_h,
                              float tile_ox, float tile_oy, const SurfaceFollowOptions& opt,
                              map::MapObjectInst& out_obj, std::unordered_map<int64_t, float>& cache) {
    float mn[3]{}, mx[3]{};
    if (!sco_merged_sco_aabb(sco, mn, mx)) return;

    const float local4[4][3] = {
        {mn[0], mn[1], mn[2]}, {mx[0], mn[1], mn[2]}, {mn[0], mx[1], mn[2]}, {mx[0], mx[1], mn[2]},
    };

    constexpr float kDeg = 3.14159265f / 180.f;
    const float yaw = obj.rotation_deg.x * kDeg;
    const XMMATRIX base =
        XMMatrixRotationZ(-yaw) *
        XMMatrixTranslation(obj.position.x + tile_ox, obj.position.y + tile_oy, obj.position.z + terrain_h);

    struct Corner {
        float x = 0.f, y = 0.f, f = 0.f, s = 0.f, surf_z = 0.f;
    };
    const float fx = std::cos(yaw);
    const float fy = -std::sin(yaw);
    const float rx = -std::sin(yaw);
    const float ry = -std::cos(yaw);
    const float cx = obj.position.x + tile_ox;
    const float cy = obj.position.y + tile_oy;

    Corner corners[4];
    for (int i = 0; i < 4; ++i) {
        DirectX::XMFLOAT3 w{};
        DirectX::XMStoreFloat3(&w, xform_point(base, local4[i][0], local4[i][1], local4[i][2]));
        corners[i].x = w.x;
        corners[i].y = w.y;
        corners[i].f = (w.x - cx) * fx + (w.y - cy) * fy;
        corners[i].s = (w.x - cx) * rx + (w.y - cy) * ry;
        corners[i].surf_z = sample_cached_z(w.x, w.y, opt, cache);
    }

    int order[4] = {0, 1, 2, 3};
    std::sort(order, order + 4, [&](const int a, const int b) { return corners[a].f > corners[b].f; });
    int i_fl = order[1], i_fr = order[0], i_bl = order[3], i_br = order[2];
    if (corners[i_fl].s > corners[i_fr].s) std::swap(i_fl, i_fr);
    if (corners[i_bl].s > corners[i_br].s) std::swap(i_bl, i_br);

    const float zf = (corners[i_fl].surf_z + corners[i_fr].surf_z) * 0.5f;
    const float zb = (corners[i_bl].surf_z + corners[i_br].surf_z) * 0.5f;
    const float zl = (corners[i_fl].surf_z + corners[i_bl].surf_z) * 0.5f;
    const float zr = (corners[i_fr].surf_z + corners[i_br].surf_z) * 0.5f;

    const float fmx = (corners[i_fl].x + corners[i_fr].x) * 0.5f;
    const float fmy = (corners[i_fl].y + corners[i_fr].y) * 0.5f;
    const float bmx = (corners[i_bl].x + corners[i_br].x) * 0.5f;
    const float bmy = (corners[i_bl].y + corners[i_br].y) * 0.5f;
    const float lmx = (corners[i_fl].x + corners[i_bl].x) * 0.5f;
    const float lmy = (corners[i_fl].y + corners[i_bl].y) * 0.5f;
    const float rmx = (corners[i_fr].x + corners[i_br].x) * 0.5f;
    const float rmy = (corners[i_fr].y + corners[i_br].y) * 0.5f;

    const float df = std::hypot(fmx - bmx, fmy - bmy);
    const float ds = std::hypot(rmx - lmx, rmy - lmy);
    constexpr float kRad = 180.f / 3.14159265f;

    if (df > 0.05f) out_obj.rotation_deg.y = std::atan2(zb - zf, df) * kRad;
    if (ds > 0.05f) out_obj.rotation_deg.z = std::atan2(zr - zl, ds) * kRad;
}

float lift_delta_bottom_grid(const std::vector<WorldPoint>& bottom, const SurfaceFollowOptions& opt,
                             std::unordered_map<int64_t, float>& cache) {
    struct Cell {
        float x = 0.f, y = 0.f, min_z = 0.f;
        bool set = false;
    };
    std::unordered_map<int64_t, Cell> cells;
    cells.reserve(bottom.size() / 2 + 4);
    for (const auto& v : bottom) {
        const int64_t key = sample_grid_key(v.x, v.y, opt.sample_quant_m);
        auto& c = cells[key];
        if (!c.set || v.z < c.min_z) {
            c.x = v.x;
            c.y = v.y;
            c.min_z = v.z;
            c.set = true;
        }
    }
    float delta = 0.f;
    for (const auto& [_, c] : cells) {
        if (!c.set) continue;
        const float surf = sample_cached_z(c.x, c.y, opt, cache);
        delta = (std::max)(delta, surf + opt.clearance - c.min_z);
    }
    return delta;
}

} // namespace

ScoWorldBounds sco_object_world_bounds(const map::MapObjectInst& obj, const sco::SceneryObject& sco,
                                       float terrain_h, float tile_ox, float tile_oy) {
    ScoWorldBounds b{};
    std::vector<WorldPoint> verts;
    transform_sco_to_world(obj, sco, terrain_h, tile_ox, tile_oy, verts);
    if (verts.empty()) return b;
    b.valid = true;
    b.min_x = b.max_x = verts[0].x;
    b.min_y = b.max_y = verts[0].y;
    b.min_z = b.max_z = verts[0].z;
    for (const auto& v : verts) {
        b.min_x = (std::min)(b.min_x, v.x);
        b.max_x = (std::max)(b.max_x, v.x);
        b.min_y = (std::min)(b.min_y, v.y);
        b.max_y = (std::max)(b.max_y, v.y);
        b.min_z = (std::min)(b.min_z, v.z);
        b.max_z = (std::max)(b.max_z, v.z);
    }
    return b;
}

void apply_surface_follow(map::MapObjectInst& obj, const sco::SceneryObject& sco, float terrain_h, bool abs_z,
                          const SurfaceFollowOptions& opt, float tile_ox, float tile_oy) {
    if (!opt.sample_world_z) return;

    const float th = abs_z ? 0.f : terrain_h;
    const int passes = (std::max)(1, (std::min)(opt.align_passes, 2));
    std::vector<WorldPoint> verts, bottom;
    std::unordered_map<int64_t, float> cache;
    cache.reserve(32);

    for (int pass = 0; pass < passes; ++pass) {
        cache.clear();

        if (opt.align_pitch_bank)
            align_pitch_bank_corners(obj, sco, th, tile_ox, tile_oy, opt, obj, cache);

        if (opt.lift_body && !opt.lock_z) {
            transform_sco_to_world(obj, sco, th, tile_ox, tile_oy, verts);
            bottom_shell_vertices(verts, bottom);
            cache.clear();
            const float delta = lift_delta_bottom_grid(bottom, opt, cache);
            if (delta > 1.e-5f) obj.position.z += delta;
        }

        if (!opt.align_pitch_bank) break;
    }
}

} // namespace omsi::app::map_editor_math
