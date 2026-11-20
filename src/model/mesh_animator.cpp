#include "omsi/model/mesh_animator.hpp"

#include "omsi/logger.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/o3d_format.hpp"

#include "o3d/o3d_loader.hpp"

#include <DirectXMath.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace omsi::model_cfg {
namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
constexpr float kScaleDriftEps = 0.02f;
constexpr float kSmoothMoveEps = 1e-4f;
constexpr float kPosChangeEps = 1e-3f;
constexpr float kSclChangeEps = 5e-3f;
constexpr float kSmoothChangeEps = 0.25f;
constexpr float kXformMinInterval = 0.35f;
constexpr float kPivotGapWarnM = 0.75f;

f4f23ma::ma4 invert_rigid(const f4f23ma::ma4& m); // defined below

f4f23ma::ma4 translation(f4f23ma::f32 x, f4f23ma::f32 y, f4f23ma::f32 z) {
    auto m = f4f23ma::ma4::identity();
    m.m[3][0] = x;
    m.m[3][1] = y;
    m.m[3][2] = z;
    return m;
}

f4f23ma::ma4 rotation_axis(MeshAnimAxis axis, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    auto m = f4f23ma::ma4::identity();
    switch (axis) {
    case MeshAnimAxis::x:
        m.m[1][1] = c;
        m.m[1][2] = s;
        m.m[2][1] = -s;
        m.m[2][2] = c;
        break;
    case MeshAnimAxis::y:
        m.m[0][0] = c;
        m.m[0][2] = -s;
        m.m[2][0] = s;
        m.m[2][2] = c;
        break;
    case MeshAnimAxis::z:
        m.m[0][0] = c;
        m.m[0][1] = s;
        m.m[1][0] = -s;
        m.m[1][1] = c;
        break;
    }
    return m;
}

// Arbitrary-axis rotation (Rodrigues) — column-major, applies as R*v.
f4f23ma::ma4 rotation_around_unit_axis(const f4f23ma::f4& axis, float radians) {
    const float x = axis.x, y = axis.y, z = axis.z;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float t = 1.f - c;
    auto m = f4f23ma::ma4::identity();
    m.m[0][0] = t * x * x + c;
    m.m[1][0] = t * x * y - s * z;
    m.m[2][0] = t * x * z + s * y;
    m.m[0][1] = t * x * y + s * z;
    m.m[1][1] = t * y * y + c;
    m.m[2][1] = t * y * z - s * x;
    m.m[0][2] = t * x * z - s * y;
    m.m[1][2] = t * y * z + s * x;
    m.m[2][2] = t * z * z + c;
    return m;
}

f4f23ma::f4 map_wiki_point(float x, float y, float z, OriginAxisMap axis_map) {
    if (axis_map == OriginAxisMap::wiki_yz_swap) return {x, z, y, 1.f};
    return {x, y, z, 1.f};
}

f4f23ma::f4 map_wiki_dir(float x, float y, float z, OriginAxisMap axis_map) {
    if (axis_map == OriginAxisMap::wiki_yz_swap) return {x, z, y, 0.f};
    return {x, y, z, 0.f};
}

f4f23ma::f4 binding_local_axis(MeshAnimAxis axis) {
    switch (axis) {
    case MeshAnimAxis::x: return {1.f, 0.f, 0.f, 0.f};
    case MeshAnimAxis::y: return {0.f, 1.f, 0.f, 0.f};
    case MeshAnimAxis::z: return {0.f, 0.f, 1.f, 0.f};
    }
    return {1.f, 0.f, 0.f, 0.f};
}

// 0x79 last row xyz — OMSI object pivot in file space (not rows[3,7,11] used by scenery spawn).
f4f23ma::f4 extract_o3d_anim_pivot_file(const omsi::o3d::NativeMesh& mesh) {
    const auto blocks = mesh.transform_span();
    if (blocks.empty()) return {0.f, 0.f, 0.f, 1.f};
    const auto* rows = blocks[0].rows;
    return {rows[12], rows[13], rows[14], 1.f};
}

std::optional<f4f23ma::ma4> extract_o3d_object_rotation_file(const omsi::o3d::NativeMesh& mesh) {
    const auto blocks = mesh.transform_span();
    if (blocks.empty()) return std::nullopt;
    auto m = f4f23ma::ma4::from_o3d_rows(blocks[0].rows);
    m.m[3][0] = 0.f;
    m.m[3][1] = 0.f;
    m.m[3][2] = 0.f;
    return m;
}

f4f23ma::f4 mesh_bbox_center_gpu(const omsi::o3d::NativeMesh& mesh) {
    const auto verts = mesh.vertex_span();
    if (verts.empty()) return {0.f, 0.f, 0.f, 1.f};
    const auto draw = omsi::o3d::o3d_mesh_draw_matrix(mesh);
    float minx = 1e9f, miny = 1e9f, minz = 1e9f;
    float maxx = -1e9f, maxy = -1e9f, maxz = -1e9f;
    for (const auto& v : verts) {
        const auto p = draw.transform_point({v.position[0], v.position[1], v.position[2], 1.f});
        minx = (std::min)(minx, p.x);
        maxx = (std::max)(maxx, p.x);
        miny = (std::min)(miny, p.y);
        maxy = (std::max)(maxy, p.y);
        minz = (std::min)(minz, p.z);
        maxz = (std::max)(maxz, p.z);
    }
    return {(minx + maxx) * 0.5f, (miny + maxy) * 0.5f, (minz + maxz) * 0.5f, 1.f};
}

float nearest_vert_distance_gpu(const omsi::o3d::NativeMesh& mesh, const f4f23ma::f4& hinge_gpu) {
    const auto verts = mesh.vertex_span();
    if (verts.empty()) return 1e9f;
    const auto draw = omsi::o3d::o3d_mesh_draw_matrix(mesh);
    float best = 1e9f;
    for (const auto& v : verts) {
        const auto p = draw.transform_point({v.position[0], v.position[1], v.position[2], 1.f});
        const float dx = p.x - hinge_gpu.x;
        const float dy = p.y - hinge_gpu.y;
        const float dz = p.z - hinge_gpu.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) best = d2;
    }
    return std::sqrt(best);
}

// Pivot candidates must use the same path as GPU verts: wiki→file map, then o3d_mesh_draw_matrix.
OriginAxisMap fit_origin_axis_map(const omsi::o3d::NativeMesh& mesh, float ox, float oy, float oz) {
    const auto draw = omsi::o3d::o3d_mesh_draw_matrix(mesh);
    const auto cand = [&](OriginAxisMap map) {
        const auto file_p = map_wiki_point(ox, oy, oz, map);
        return draw.transform_point(file_p);
    };
    const float d_raw = nearest_vert_distance_gpu(mesh, cand(OriginAxisMap::identity));
    const float d_yz = nearest_vert_distance_gpu(mesh, cand(OriginAxisMap::wiki_yz_swap));
    if (d_yz + 0.05f < d_raw) return OriginAxisMap::wiki_yz_swap;
    return OriginAxisMap::identity;
}

f4f23ma::f4 wiki_origin_to_gpu(float ox, float oy, float oz, OriginAxisMap axis_map,
                               const f4f23ma::ma4& draw) {
    return draw.transform_point(map_wiki_point(ox, oy, oz, axis_map));
}

f4f23ma::ma4 to_gpu_anim_matrix(const f4f23ma::ma4& file_anim, const f4f23ma::ma4& draw) {
    // V_gpu = Draw * V_file  =>  M_gpu = Draw * M_file * inv(Draw)
    return draw * file_anim * invert_rigid(draw);
}

f4f23ma::f4 hinge_axis_file_to_gpu(const f4f23ma::f4& axis_file, const f4f23ma::ma4& draw,
                                   bool origin_from_mesh) {
    f4f23ma::f4 axis = draw.transform_direction3(axis_file).normalized3();
    // origin_from_mesh (0x79 local X, after origin_rot_*): opposite of +anim_rot in GPU.
    if (origin_from_mesh) {
        axis.x = -axis.x;
        axis.y = -axis.y;
        axis.z = -axis.z;
    }
    return axis;
}

float smooth_anim_value(float target, float last, float dt, const MeshAnimBinding& binding) {
    if (!binding.maxspeed && !binding.delay) return target;
    float delta = target - last;
    if (binding.delay && *binding.delay > 0.f) {
        float rate = *binding.delay * dt;
        if (rate > 1.f) rate = 1.f;
        if (rate < 0.f) rate = 0.f;
        delta *= rate;
    }
    if (binding.maxspeed && *binding.maxspeed > 0.f) {
        const float max_step = *binding.maxspeed * dt;
        if (delta > max_step) delta = max_step;
        if (delta < -max_step) delta = -max_step;
    }
    return last + delta;
}

bool matrix_near_identity(const f4f23ma::ma4& m, float eps = 1e-4f) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            const float expect = (c == r) ? 1.f : 0.f;
            if (std::fabs(m.m[c][r] - expect) > eps) return false;
        }
    }
    return true;
}

std::string fmt_f(float v) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << v;
    return os.str();
}

float column_scale(const f4f23ma::ma4& m, int c) {
    const float x = m.m[c][0];
    const float y = m.m[c][1];
    const float z = m.m[c][2];
    return std::sqrt(x * x + y * y + z * z);
}

float det3_upper(const f4f23ma::ma4& m) {
    const float a = m.m[0][0], b = m.m[1][0], c = m.m[2][0];
    const float d = m.m[0][1], e = m.m[1][1], f = m.m[2][1];
    const float g = m.m[0][2], h = m.m[1][2], i = m.m[2][2];
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

float vec3_len(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

float hinge_gap(const f4f23ma::f4& hinge, const f4f23ma::f4& center) {
    return vec3_len(hinge.x - center.x, hinge.y - center.y, hinge.z - center.z);
}

struct MeshDriveInfo {
    float best_smooth = 0.f;
    float best_var = 0.f;
    const MeshAnimBinding* best_b = nullptr;
};

MeshDriveInfo best_drive(const ModelCfgMeshEntry& mesh, const std::vector<std::vector<float>>& layer_vals,
                         const std::function<float(const std::string&)>& get_var) {
    MeshDriveInfo out;
    for (std::size_t li = 0; li < mesh.animations.size() && li < layer_vals.size(); ++li) {
        for (std::size_t bi = 0; bi < mesh.animations[li].bindings.size() && bi < layer_vals[li].size(); ++bi) {
            const float s = layer_vals[li][bi];
            if (std::fabs(s) >= std::fabs(out.best_smooth)) {
                out.best_smooth = s;
                out.best_b = &mesh.animations[li].bindings[bi];
                out.best_var = get_var(out.best_b->var_name);
            }
        }
    }
    return out;
}

std::string wall_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::mutex g_xform_mutex;
std::ofstream g_xform_file;
std::filesystem::path g_xform_path;
bool g_xform_opened = false;

std::mutex g_door_vert_mutex;
std::ofstream g_door_vert_file;
std::filesystem::path g_door_vert_path;
bool g_door_vert_opened = false;

std::filesystem::path default_xform_log_path() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto p = std::filesystem::path(buf);
    p.replace_filename(L"omsi_f4_anim_xform.log");
    return p;
}

std::filesystem::path default_door_vert_log_path() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto p = std::filesystem::path(buf);
    p.replace_filename(L"omsi_f4_door_verts.log");
    return p;
}

void ensure_xform_file() {
    if (g_xform_opened) return;
    g_xform_path = default_xform_log_path();
    g_xform_file.open(g_xform_path, std::ios::out | std::ios::app);
    g_xform_opened = true;
    if (g_xform_file.is_open()) {
        g_xform_file << "\n=== anim xform session " << wall_timestamp()
                     << " | V'=P+R(V-P) only | ===\n";
        g_xform_file.flush();
    }
}

void write_xform_line(std::string_view line) {
    ensure_xform_file();
    std::lock_guard lock(g_xform_mutex);
    if (!g_xform_file.is_open()) return;
    g_xform_file << "[" << wall_timestamp() << "] " << line << '\n';
    g_xform_file.flush();
}

void ensure_door_vert_file() {
    if (g_door_vert_opened) return;
    g_door_vert_path = default_door_vert_log_path();
    g_door_vert_file.open(g_door_vert_path, std::ios::out | std::ios::app);
    g_door_vert_opened = true;
    if (g_door_vert_file.is_open()) {
        g_door_vert_file << "\n=== door verts session " << wall_timestamp()
                         << " | cols: i,x0,y0,z0,x1,y1,z1 (rest -> transformed) ===\n";
        g_door_vert_file.flush();
    }
}

f4f23ma::ma4 swap_yz_matrix() {
    // Similarity S=S^-1 that maps wiki (x,y,z)->file (x,z,y). det=-1 (axis swap).
    f4f23ma::ma4 r = f4f23ma::ma4::identity();
    r.m[1][1] = 0.f;
    r.m[2][2] = 0.f;
    r.m[2][1] = 1.f;
    r.m[1][2] = 1.f;
    return r;
}

// Inverse of rigid T*R (column-major).
f4f23ma::ma4 invert_rigid(const f4f23ma::ma4& m) {
    f4f23ma::ma4 inv = f4f23ma::ma4::identity();
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            inv.m[c][r] = m.m[r][c];
    const float tx = m.m[3][0];
    const float ty = m.m[3][1];
    const float tz = m.m[3][2];
    inv.m[3][0] = -(m.m[0][0] * tx + m.m[0][1] * ty + m.m[0][2] * tz);
    inv.m[3][1] = -(m.m[1][0] * tx + m.m[1][1] * ty + m.m[1][2] * tz);
    inv.m[3][2] = -(m.m[2][0] * tx + m.m[2][1] * ty + m.m[2][2] * tz);
    return inv;
}

// Pivot in model.cfg wiki space (before optional S·M·S into mesh/GPU).
f4f23ma::f4 resolve_layer_pivot_wiki(const MeshAnimLayer& layer, const f4f23ma::f4& mesh_file_pivot,
                                     OriginAxisMap axis_map) {
    f4f23ma::f4 pivot{0.f, 0.f, 0.f, 1.f};
    if (layer.origin_from_mesh) {
        // Mesh pivot is GPU/file; convert to wiki if file uses yz_swap.
        if (axis_map == OriginAxisMap::wiki_yz_swap)
            pivot = {mesh_file_pivot.x, mesh_file_pivot.z, mesh_file_pivot.y, 1.f};
        else
            pivot = mesh_file_pivot;
    }
    if (layer.has_origin_trans) {
        const f4f23ma::f4 t{layer.origin_trans[0], layer.origin_trans[1], layer.origin_trans[2], 1.f};
        if (layer.origin_from_mesh) {
            pivot.x += t.x;
            pivot.y += t.y;
            pivot.z += t.z;
        } else {
            pivot = t;
        }
    }
    return pivot;
}

} // namespace

f4f23ma::ma4 rotate_around_pivot(const f4f23ma::f4& pivot, const f4f23ma::f4& axis_unit, float angle_radians) {
    // V' = P + R*(V-P)  <=>  M = T(P) * R * T(-P)
    return translation(pivot.x, pivot.y, pivot.z) * rotation_around_unit_axis(axis_unit, angle_radians) *
           translation(-pivot.x, -pivot.y, -pivot.z);
}

// Origin in FILE space (Y-up after wiki→file map), matching Unit87 tag semantics.
// cfg origin_rot_y → D3DX RotZ, origin_rot_z → D3DX RotY (wiki Y-fwd/Z-up ↔ D3DX Z-fwd/Y-up).
// Tags compose as Origin = Origin * Rot (Old*Rot): so rot_x can tip the shaft after Y/Z remaps
// aim local X. Left-mul Rot*Old leaves local X = Remap*e_x and gauges tumble “forward”.
f4f23ma::ma4 build_layer_origin_file(const MeshAnimLayer& layer, const f4f23ma::f4& mesh_file_pivot,
                                     OriginAxisMap axis_map, const f4f23ma::ma4* mesh_object_file) {
    const f4f23ma::f4 pivot_w = resolve_layer_pivot_wiki(layer, mesh_file_pivot, axis_map);
    const f4f23ma::f4 pivot_f = map_wiki_point(pivot_w.x, pivot_w.y, pivot_w.z, axis_map);
    f4f23ma::ma4 origin;
    // origin_from_mesh: same as bus origin_trans — hinge in FILE space, then Draw to GPU.
    // 0x79 3x3 orients anim_rot (bank); last-row xyz is the hinge, not a spawn offset.
    if (layer.origin_from_mesh && mesh_object_file) {
        origin = f4f23ma::ma4::identity();
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                origin.m[c][r] = mesh_object_file->m[c][r];
        origin.m[3][0] = pivot_f.x;
        origin.m[3][1] = pivot_f.y;
        origin.m[3][2] = pivot_f.z;
    } else {
        origin = translation(pivot_f.x, pivot_f.y, pivot_f.z);
    }
    if (layer.origin_rot_deg[0] != 0.f)
        origin = origin * rotation_axis(MeshAnimAxis::x, layer.origin_rot_deg[0] * kDegToRad);
    if (layer.origin_rot_deg[1] != 0.f)
        origin = origin * rotation_axis(MeshAnimAxis::z, layer.origin_rot_deg[1] * kDegToRad);
    if (layer.origin_rot_deg[2] != 0.f)
        origin = origin * rotation_axis(MeshAnimAxis::y, layer.origin_rot_deg[2] * kDegToRad);
    return origin;
}

f4f23ma::ma4 build_anim_only(const MeshAnimLayer& layer, const std::vector<float>& binding_values) {
    auto anim = f4f23ma::ma4::identity();
    for (std::size_t bi = 0; bi < layer.bindings.size(); ++bi) {
        const auto& binding = layer.bindings[bi];
        const float value = bi < binding_values.size() ? binding_values[bi] : 0.f;
        if (binding.type == MeshAnimBindingType::rotation) {
            anim = anim * rotation_axis(binding.axis, value * kDegToRad);
        } else {
            if (binding.axis == MeshAnimAxis::x)
                anim = anim * translation(value, 0.f, 0.f);
            else if (binding.axis == MeshAnimAxis::y)
                anim = anim * translation(0.f, value, 0.f);
            else
                anim = anim * translation(0.f, 0.f, value);
        }
    }
    return anim;
}

f4f23ma::ma4 compose_layer_matrix(const f4f23ma::ma4& origin, const f4f23ma::ma4& origin_inv,
                                  const f4f23ma::ma4& anim, OriginAxisMap /*axis_map*/) {
    // Origin already in file space (Unit87 remap applied). OMSI: M = origin * anim * originI.
    return origin * anim * origin_inv;
}

f4f23ma::ma4 build_anim_layer_matrix(const MeshAnimLayer& layer, const f4f23ma::f4& mesh_file_pivot,
                                     const std::vector<float>& binding_values, OriginAxisMap axis_map,
                                     const f4f23ma::ma4* mesh_object_file) {
    const auto origin = build_layer_origin_file(layer, mesh_file_pivot, axis_map, mesh_object_file);
    return compose_layer_matrix(origin, invert_rigid(origin), build_anim_only(layer, binding_values), axis_map);
}

void MeshAnimator::init(const ModelCfgDocument& cfg) {
    const auto* lod = primary_lod(cfg);
    if (!lod) return;

    const std::size_t n = lod->meshes.size();
    matrices_.assign(n, f4f23ma::ma4::identity());
    matrices_dx_t_.assign(n, {});
    for (auto& slot : matrices_dx_t_) {
        DirectX::XMFLOAT4X4 id{};
        DirectX::XMStoreFloat4x4(&id, DirectX::XMMatrixIdentity());
        std::memcpy(slot.data(), &id, sizeof(id));
    }
    parent_index_.assign(n, -1);
    mesh_file_pivot_.assign(n, std::nullopt);
    mesh_file_origin_.assign(n, std::nullopt);
    origin_axis_map_.assign(n, OriginAxisMap::identity);
    mesh_draw_matrix_.assign(n, f4f23ma::ma4::identity());
    layer_pivot_gpu_.assign(n, {});
    layer_axis_gpu_.assign(n, {});
    mesh_bbox_center_gpu_.assign(n, std::nullopt);
    mesh_hinge_gpu_.assign(n, std::nullopt);
    last_values_.assign(n, {});
    door_rest_verts_.assign(n, {});
    door_vert_prev_smooth_.assign(n, 0.f);
    door_vert_rest_dumped_.assign(n, false);
    door_vert_announced_ = false;
    xform_prev_mats_.assign(n, f4f23ma::ma4::identity());
    xform_prev_smooth_.assign(n, 0.f);
    xform_last_emit_t_.assign(n, -1e9f);
    xform_announced_ = false;

    std::unordered_map<std::string, int> ident_to_index;
    for (std::size_t i = 0; i < n; ++i) {
        if (lod->meshes[i].mesh_ident) ident_to_index[*lod->meshes[i].mesh_ident] = static_cast<int>(i);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!lod->meshes[i].animparent) continue;
        const auto it = ident_to_index.find(*lod->meshes[i].animparent);
        if (it != ident_to_index.end()) parent_index_[i] = it->second;
        else
            omsi::log::warn("mesh animator: animparent \"" + *lod->meshes[i].animparent +
                            "\" not found for mesh " + lod->meshes[i].mesh_file);
    }

    const auto model_dir = cfg.cfg_path.parent_path();
    std::size_t anim_mesh = 0;
    std::size_t from_mesh = 0;
    std::size_t with_parent = 0;
    std::size_t binding_count = 0;
    std::size_t yz_swap_count = 0;
    std::size_t o3d_ok = 0;
    std::size_t o3d_fail = 0;
    int hinge_fit_logged = 0;

    for (std::size_t i = 0; i < n; ++i) {
        const auto& mesh = lod->meshes[i];
        if (!mesh.animations.empty()) ++anim_mesh;
        if (parent_index_[i] >= 0) ++with_parent;

        last_values_[i].resize(mesh.animations.size());
        bool need_file_pivot = false;
        bool need_o3d = !mesh.animations.empty() && !mesh.mesh_file.empty();
        const MeshAnimLayer* first_origin_layer = nullptr;
        for (std::size_t li = 0; li < mesh.animations.size(); ++li) {
            const auto& layer = mesh.animations[li];
            last_values_[i][li].assign(layer.bindings.size(), 0.f);
            binding_count += layer.bindings.size();
            if (layer.origin_from_mesh) need_file_pivot = true;
            if (!first_origin_layer && layer.has_origin_trans) first_origin_layer = &layer;
        }
        if (!need_o3d) continue;

        const auto o3d_path = model_dir / mesh.mesh_file;
        auto mapped = omsi::o3d::load_mapped(o3d_path);
        if (!mapped || !mapped->valid()) {
            ++o3d_fail;
            omsi::log::warn("mesh animator: O3D load failed: " + o3d_path.string());
            continue;
        }
        ++o3d_ok;
        const auto& native = mapped->mesh();
        const auto draw = omsi::o3d::o3d_mesh_draw_matrix(native);
        mesh_draw_matrix_[i] = draw;
        mesh_bbox_center_gpu_[i] = mesh_bbox_center_gpu(native);

        OriginAxisMap axis = OriginAxisMap::identity;
        float hinge_ox = 0.f;
        float hinge_oy = 0.f;
        float hinge_oz = 0.f;
        if (first_origin_layer) {
            hinge_ox = first_origin_layer->origin_trans[0];
            hinge_oy = first_origin_layer->origin_trans[1];
            hinge_oz = first_origin_layer->origin_trans[2];
        } else if (need_file_pivot) {
            const auto fp = extract_o3d_anim_pivot_file(native);
            hinge_ox = fp.x;
            hinge_oy = fp.y;
            hinge_oz = fp.z;
        }
        if (first_origin_layer || need_file_pivot) {
            axis = fit_origin_axis_map(native, hinge_ox, hinge_oy, hinge_oz);
            if (hinge_fit_logged < 6) {
                const auto d_raw =
                    nearest_vert_distance_gpu(native, wiki_origin_to_gpu(hinge_ox, hinge_oy, hinge_oz,
                                                                         OriginAxisMap::identity, draw));
                const auto d_yz =
                    nearest_vert_distance_gpu(native, wiki_origin_to_gpu(hinge_ox, hinge_oy, hinge_oz,
                                                                         OriginAxisMap::wiki_yz_swap, draw));
                omsi::log::info("mesh animator hinge_fit[" + std::to_string(hinge_fit_logged) + "] " + mesh.mesh_file +
                                " d_raw=" + fmt_f(d_raw) + " d_yz=" + fmt_f(d_yz) +
                                " map=" + (axis == OriginAxisMap::wiki_yz_swap ? "yz_swap" : "identity") +
                                " draw79=" + (omsi::o3d::mesh_has_transform_chunk(native) ? "1" : "0"));
                ++hinge_fit_logged;
            }
        }
        origin_axis_map_[i] = axis;
        if (axis == OriginAxisMap::wiki_yz_swap) ++yz_swap_count;

        if (need_file_pivot) {
            mesh_file_pivot_[i] = extract_o3d_anim_pivot_file(native);
            mesh_file_origin_[i] = extract_o3d_object_rotation_file(native);
            ++from_mesh;
        }
        if (first_origin_layer) {
            mesh_hinge_gpu_[i] =
                wiki_origin_to_gpu(first_origin_layer->origin_trans[0], first_origin_layer->origin_trans[1],
                                   first_origin_layer->origin_trans[2], axis, draw);
        } else if (need_file_pivot && mesh_file_pivot_[i]) {
            mesh_hinge_gpu_[i] = draw.transform_point(*mesh_file_pivot_[i]);
        }

        // Cache rest verts for door meshes (Puerta*) — GPU/draw space, same as VB.
        {
            std::string lower = mesh.mesh_file;
            for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lower.find("puerta") != std::string::npos) {
                const auto verts = native.vertex_span();
                door_rest_verts_[i].reserve(verts.size());
                for (const auto& v : verts) {
                    door_rest_verts_[i].push_back(
                        draw.transform_point({v.position[0], v.position[1], v.position[2], 1.f}));
                }
            }
        }
    }

    // Bake hinge P + axis from Unit87 Origin_file, then Draw. Runtime uses rigid T(-P) R T(P).
    int rot_logged = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& mesh = lod->meshes[i];
        const f4f23ma::f4 file_pivot =
            mesh_file_pivot_[i] ? *mesh_file_pivot_[i] : f4f23ma::f4{0.f, 0.f, 0.f, 1.f};
        const f4f23ma::ma4* mesh_object_file =
            (i < mesh_file_origin_.size() && mesh_file_origin_[i]) ? &*mesh_file_origin_[i] : nullptr;
        const OriginAxisMap axis_map =
            i < origin_axis_map_.size() ? origin_axis_map_[i] : OriginAxisMap::identity;
        const f4f23ma::ma4 draw =
            i < mesh_draw_matrix_.size() ? mesh_draw_matrix_[i] : f4f23ma::ma4::identity();
        layer_pivot_gpu_[i].resize(mesh.animations.size());
        layer_axis_gpu_[i].resize(mesh.animations.size());
        for (std::size_t li = 0; li < mesh.animations.size(); ++li) {
            const auto origin_f =
                build_layer_origin_file(mesh.animations[li], file_pivot, axis_map, mesh_object_file);
            f4f23ma::f4 pivot_f{origin_f.m[3][0], origin_f.m[3][1], origin_f.m[3][2], 1.f};
            f4f23ma::f4 axis_f = origin_f.transform_direction3({1.f, 0.f, 0.f, 0.f}).normalized3();
            layer_pivot_gpu_[i][li] = draw.transform_point(pivot_f);
            layer_axis_gpu_[i][li] =
                hinge_axis_file_to_gpu(axis_f, draw, mesh.animations[li].origin_from_mesh);
            if (rot_logged < 8) {
                const auto& Pg = layer_pivot_gpu_[i][li];
                const auto& Ag = layer_axis_gpu_[i][li];
                omsi::log::info("mesh animator rot[" + std::to_string(i) + "] " + mesh.mesh_file +
                                " origin_from_mesh=" + (mesh.animations[li].origin_from_mesh ? "1" : "0") +
                                " P_file=(" + fmt_f(pivot_f.x) + "," + fmt_f(pivot_f.y) + "," + fmt_f(pivot_f.z) +
                                ") P_gpu=(" + fmt_f(Pg.x) + "," + fmt_f(Pg.y) + "," + fmt_f(Pg.z) + ") A_gpu=(" +
                                fmt_f(Ag.x) + "," + fmt_f(Ag.y) + "," + fmt_f(Ag.z) + ")");
                ++rot_logged;
            }
        }
    }

    std::size_t door_tracked = 0;
    for (const auto& v : door_rest_verts_)
        if (!v.empty()) ++door_tracked;
    omsi::log::info("mesh animator: V'=P+R(V-P) Unit87-file meshes=" + std::to_string(n) +
                    " with_anim=" + std::to_string(anim_mesh) + " bindings=" + std::to_string(binding_count) +
                    " origin_from_mesh=" + std::to_string(from_mesh) +
                    " animparent=" + std::to_string(with_parent) + " o3d_ok=" + std::to_string(o3d_ok) +
                    " o3d_fail=" + std::to_string(o3d_fail) + " wiki_yz_swap=" + std::to_string(yz_swap_count) +
                    " door_vert_tracked=" + std::to_string(door_tracked));
}

void MeshAnimator::update(const ModelCfgDocument& cfg, float dt,
                          const std::function<float(const std::string&)>& get_var) {
    const auto* lod = primary_lod(cfg);
    if (!lod) return;
    if (matrices_.size() != lod->meshes.size()) init(cfg);

    const float safe_dt = dt > 0.f ? dt : 0.f;
    std::vector<DirectX::XMMATRIX> local_dx(lod->meshes.size(), DirectX::XMMatrixIdentity());
    std::vector<f4f23ma::ma4> local_col(lod->meshes.size(), f4f23ma::ma4::identity());

    for (std::size_t i = 0; i < lod->meshes.size(); ++i) {
        const auto& mesh = lod->meshes[i];
        if (last_values_[i].size() != mesh.animations.size()) {
            last_values_[i].resize(mesh.animations.size());
            for (std::size_t li = 0; li < mesh.animations.size(); ++li)
                last_values_[i][li].assign(mesh.animations[li].bindings.size(), 0.f);
        }
        if (layer_pivot_gpu_[i].size() != mesh.animations.size()) {
            init(cfg);
            return;
        }

        DirectX::XMMATRIX world_dx = DirectX::XMMatrixIdentity();
        f4f23ma::ma4 world_col = f4f23ma::ma4::identity();
        for (std::size_t li = 0; li < mesh.animations.size(); ++li) {
            const auto& layer = mesh.animations[li];
            auto& vals = last_values_[i][li];
            if (vals.size() != layer.bindings.size()) vals.assign(layer.bindings.size(), 0.f);
            for (std::size_t bi = 0; bi < layer.bindings.size(); ++bi) {
                const auto& binding = layer.bindings[bi];
                const float var = get_var(binding.var_name);
                const float target = var * binding.scale + binding.offset;
                vals[bi] = smooth_anim_value(target, vals[bi], safe_dt, binding);
            }

            const f4f23ma::f4& P = layer_pivot_gpu_[i][li];
            const f4f23ma::f4& A = layer_axis_gpu_[i][li];
            const DirectX::XMVECTOR axis_v = DirectX::XMVector3Normalize(DirectX::XMVectorSet(A.x, A.y, A.z, 0.f));

            DirectX::XMMATRIX motion = DirectX::XMMatrixIdentity();
            f4f23ma::ma4 motion_col = f4f23ma::ma4::identity();
            for (std::size_t bi = 0; bi < layer.bindings.size(); ++bi) {
                const float value = vals[bi];
                if (layer.bindings[bi].type == MeshAnimBindingType::rotation) {
                    float rad = value * kDegToRad;
                    if (layer.origin_from_mesh) rad = -rad;
                    motion = motion * DirectX::XMMatrixRotationAxis(axis_v, rad);
                    motion_col = motion_col * rotation_around_unit_axis(A, rad);
                } else {
                    motion = motion * DirectX::XMMatrixTranslation(A.x * value, A.y * value, A.z * value);
                    motion_col = motion_col * translation(A.x * value, A.y * value, A.z * value);
                }
            }
            const DirectX::XMMATRIX layer_dx = DirectX::XMMatrixTranslation(-P.x, -P.y, -P.z) * motion *
                                              DirectX::XMMatrixTranslation(P.x, P.y, P.z);
            const f4f23ma::ma4 layer_col =
                translation(P.x, P.y, P.z) * motion_col * translation(-P.x, -P.y, -P.z);
            world_dx = world_dx * layer_dx;
            world_col = world_col * layer_col;
        }
        local_dx[i] = world_dx;
        local_col[i] = world_col;
    }

    for (std::size_t i = 0; i < lod->meshes.size(); ++i) {
        DirectX::XMMATRIX m_dx = local_dx[i];
        f4f23ma::ma4 m_col = local_col[i];
        int p = parent_index_[i];
        int guard = 0;
        while (p >= 0 && p < static_cast<int>(local_dx.size()) && guard++ < 64) {
            m_dx = local_dx[static_cast<std::size_t>(p)] * m_dx;
            m_col = local_col[static_cast<std::size_t>(p)] * m_col;
            p = parent_index_[static_cast<std::size_t>(p)];
        }
        matrices_[i] = m_col;
        DirectX::XMFLOAT4X4 stored{};
        DirectX::XMStoreFloat4x4(&stored, DirectX::XMMatrixTranspose(m_dx));
        std::memcpy(matrices_dx_t_[i].data(), &stored, sizeof(stored));
    }
}

void MeshAnimator::xform_log(const ModelCfgDocument& cfg, float sim_time_sec,
                             const std::function<float(const std::string&)>& get_var) {
    const auto* lod = primary_lod(cfg);
    if (!lod || matrices_.empty()) return;
    if (xform_prev_mats_.size() != matrices_.size()) {
        xform_prev_mats_.assign(matrices_.size(), f4f23ma::ma4::identity());
        xform_prev_smooth_.assign(matrices_.size(), 0.f);
        xform_last_emit_t_.assign(matrices_.size(), -1e9f);
    }
    if (!xform_announced_) {
        ensure_xform_file();
        omsi::log::info("anim xform log: " + g_xform_path.string());
        xform_announced_ = true;
    }

    int emitted = 0;
    constexpr int kMaxEmitPerCall = 24;
    for (std::size_t i = 0; i < lod->meshes.size() && i < matrices_.size(); ++i) {
        const auto& mesh = lod->meshes[i];
        if (mesh.animations.empty()) continue;
        const auto& m = matrices_[i];
        const MeshDriveInfo drive =
            i < last_values_.size() ? best_drive(mesh, last_values_[i], get_var) : MeshDriveInfo{};

        const float sx = column_scale(m, 0);
        const float sy = column_scale(m, 1);
        const float sz = column_scale(m, 2);
        const float det = det3_upper(m);
        const bool scale_bad = std::fabs(sx - 1.f) > kScaleDriftEps || std::fabs(sy - 1.f) > kScaleDriftEps ||
                               std::fabs(sz - 1.f) > kScaleDriftEps || std::fabs(std::fabs(det) - 1.f) > 0.05f;
        const float pos_len = vec3_len(m.m[3][0], m.m[3][1], m.m[3][2]);
        float gap = -1.f;
        bool pivot_gap_bad = false;
        if (i < mesh_hinge_gpu_.size() && mesh_hinge_gpu_[i] && i < mesh_bbox_center_gpu_.size() &&
            mesh_bbox_center_gpu_[i]) {
            gap = hinge_gap(*mesh_hinge_gpu_[i], *mesh_bbox_center_gpu_[i]);
            pivot_gap_bad = gap > kPivotGapWarnM;
        }

        const bool moving = std::fabs(drive.best_smooth) > kSmoothMoveEps || !matrix_near_identity(m);
        if (!moving && !scale_bad) {
            xform_prev_mats_[i] = m;
            xform_prev_smooth_[i] = drive.best_smooth;
            continue;
        }

        const auto& prev = xform_prev_mats_[i];
        const float dpos = std::fabs(m.m[3][0] - prev.m[3][0]) + std::fabs(m.m[3][1] - prev.m[3][1]) +
                           std::fabs(m.m[3][2] - prev.m[3][2]);
        const float dsmooth = std::fabs(drive.best_smooth - xform_prev_smooth_[i]);
        const bool first = xform_last_emit_t_[i] < 0.f;
        const bool due = first || scale_bad || pivot_gap_bad ||
                         ((dpos > kPosChangeEps || dsmooth > kSmoothChangeEps) &&
                          (sim_time_sec - xform_last_emit_t_[i]) >= kXformMinInterval);
        if (!due || emitted >= kMaxEmitPerCall) continue;

        std::ostringstream os;
        os << "t=" << fmt_f(sim_time_sec) << "s mesh[" << i << "]=" << mesh.mesh_file << " pos=("
           << fmt_f(m.m[3][0]) << "," << fmt_f(m.m[3][1]) << "," << fmt_f(m.m[3][2]) << ")"
           << " |pos|=" << fmt_f(pos_len) << " scl=(" << fmt_f(sx) << "," << fmt_f(sy) << "," << fmt_f(sz) << ")";
        if (i < mesh_hinge_gpu_.size() && mesh_hinge_gpu_[i]) {
            const auto& h = *mesh_hinge_gpu_[i];
            os << " hinge=(" << fmt_f(h.x) << "," << fmt_f(h.y) << "," << fmt_f(h.z) << ")";
        }
        if (gap >= 0.f) os << " gap=" << fmt_f(gap);
        if (i < origin_axis_map_.size() && origin_axis_map_[i] == OriginAxisMap::wiki_yz_swap)
            os << " yz_swap=1";
        if (drive.best_b) {
            os << " " << (drive.best_b->type == MeshAnimBindingType::rotation ? "rot" : "trans") << " \""
               << drive.best_b->var_name << "\" var=" << fmt_f(drive.best_var)
               << " smooth=" << fmt_f(drive.best_smooth);
        }
        if (scale_bad) os << " WARN_SCALE_DRIFT";
        if (pivot_gap_bad) os << " WARN_PIVOT_GAP";
        write_xform_line(os.str());
        xform_prev_mats_[i] = m;
        xform_prev_smooth_[i] = drive.best_smooth;
        xform_last_emit_t_[i] = sim_time_sec;
        ++emitted;
    }
}

void MeshAnimator::debug_log(const ModelCfgDocument& cfg,
                             const std::function<float(const std::string&)>& get_var,
                             const std::function<bool(const std::string&)>& var_exists) const {
    const auto* lod = primary_lod(cfg);
    if (!lod) return;

    std::size_t bindings = 0, missing = 0, nonzero_var = 0, nonzero_smooth = 0, moved_mats = 0;
    std::unordered_set<std::string> missing_names;
    std::unordered_set<std::string> active_names;
    for (std::size_t i = 0; i < lod->meshes.size(); ++i) {
        const auto& mesh = lod->meshes[i];
        for (std::size_t li = 0; li < mesh.animations.size(); ++li) {
            for (std::size_t bi = 0; bi < mesh.animations[li].bindings.size(); ++bi) {
                ++bindings;
                const auto& b = mesh.animations[li].bindings[bi];
                if (!var_exists(b.var_name)) {
                    ++missing;
                    if (missing_names.size() < 16) missing_names.insert(b.var_name);
                }
                const float var = get_var(b.var_name);
                if (std::fabs(var) > 1e-5f) {
                    ++nonzero_var;
                    if (active_names.size() < 16) active_names.insert(b.var_name + "=" + fmt_f(var));
                }
                if (i < last_values_.size() && li < last_values_[i].size() && bi < last_values_[i][li].size() &&
                    std::fabs(last_values_[i][li][bi]) > 1e-4f)
                    ++nonzero_smooth;
            }
        }
        if (i < matrices_.size() && !matrix_near_identity(matrices_[i])) ++moved_mats;
    }
    omsi::log::info("mesh animator debug: bindings=" + std::to_string(bindings) +
                    " missing_vars=" + std::to_string(missing) + " nonzero_vars=" + std::to_string(nonzero_var) +
                    " nonzero_smooth=" + std::to_string(nonzero_smooth) + " moved_mats=" +
                    std::to_string(moved_mats) + "/" + std::to_string(matrices_.size()));

    int shown = 0;
    for (std::size_t i = 0; i < lod->meshes.size() && shown < 8; ++i) {
        if (i >= matrices_.size() || matrix_near_identity(matrices_[i])) continue;
        const MeshDriveInfo drive =
            i < last_values_.size() ? best_drive(lod->meshes[i], last_values_[i], get_var) : MeshDriveInfo{};
        if (std::fabs(drive.best_smooth) < 1e-4f) continue;
        const auto& m = matrices_[i];
        std::ostringstream os;
        os << "mesh animator moved[" << shown << "] " << lod->meshes[i].mesh_file << " T=(" << fmt_f(m.m[3][0])
           << "," << fmt_f(m.m[3][1]) << "," << fmt_f(m.m[3][2]) << ")"
           << " scl=(" << fmt_f(column_scale(m, 0)) << "," << fmt_f(column_scale(m, 1)) << ","
           << fmt_f(column_scale(m, 2)) << ")";
        if (drive.best_b)
            os << " rot \"" << drive.best_b->var_name << "\" smooth=" << fmt_f(drive.best_smooth);
        omsi::log::info(os.str());
        ++shown;
    }
}

void MeshAnimator::door_vert_log(const ModelCfgDocument& cfg, float sim_time_sec) {
    const auto* lod = primary_lod(cfg);
    if (!lod || matrices_.empty()) return;
    if (door_rest_verts_.size() != lod->meshes.size()) return;

    bool any_tracked = false;
    for (const auto& v : door_rest_verts_)
        if (!v.empty()) {
            any_tracked = true;
            break;
        }
    if (!any_tracked) return;

    if (!door_vert_announced_) {
        ensure_door_vert_file();
        omsi::log::info("door vert log: " + g_door_vert_path.string());
        door_vert_announced_ = true;
    }
    if (!g_door_vert_file.is_open()) return;

    std::lock_guard lock(g_door_vert_mutex);
    for (std::size_t i = 0; i < lod->meshes.size(); ++i) {
        if (door_rest_verts_[i].empty() || i >= matrices_.size()) continue;

        float best_smooth = 0.f;
        if (i < last_values_.size()) {
            for (const auto& layer_vals : last_values_[i])
                for (float s : layer_vals)
                    if (std::fabs(s) >= std::fabs(best_smooth)) best_smooth = s;
        }
        const float prev = i < door_vert_prev_smooth_.size() ? door_vert_prev_smooth_[i] : 0.f;
        // Log only while the drive value is changing (opening/closing), plus one rest snapshot.
        // Holding a fully-open pose would otherwise dump every frame forever.
        const bool changing = std::fabs(best_smooth - prev) > 1e-3f;
        const bool dump_rest = i < door_vert_rest_dumped_.size() && !door_vert_rest_dumped_[i];
        if (!changing && !dump_rest) {
            if (i < door_vert_prev_smooth_.size()) door_vert_prev_smooth_[i] = best_smooth;
            continue;
        }

        const auto& m = matrices_[i];
        const auto& rest = door_rest_verts_[i];
        f4f23ma::f4 hinge{0.f, 0.f, 0.f, 1.f};
        if (i < mesh_hinge_gpu_.size() && mesh_hinge_gpu_[i]) hinge = *mesh_hinge_gpu_[i];

        float max_rest_r = 0.f;
        float max_xf_r = 0.f;
        float max_drift = 0.f; // | |V'-H| - |V0-H| | — should stay ~0 for rigid pivot rot
        for (const auto& v0 : rest) {
            const float r0 = vec3_len(v0.x - hinge.x, v0.y - hinge.y, v0.z - hinge.z);
            if (r0 > max_rest_r) max_rest_r = r0;
            const auto v1 = m.transform_point(v0);
            const float r1 = vec3_len(v1.x - hinge.x, v1.y - hinge.y, v1.z - hinge.z);
            if (r1 > max_xf_r) max_xf_r = r1;
            const float drift = std::fabs(r1 - r0);
            if (drift > max_drift) max_drift = drift;
        }

        g_door_vert_file << "FRAME t=" << fmt_f(sim_time_sec) << " mesh[" << i << "]=" << lod->meshes[i].mesh_file
                         << " verts=" << rest.size() << " smooth=" << fmt_f(best_smooth) << " T=("
                         << fmt_f(m.m[3][0]) << "," << fmt_f(m.m[3][1]) << "," << fmt_f(m.m[3][2]) << ")"
                         << " hinge=(" << fmt_f(hinge.x) << "," << fmt_f(hinge.y) << "," << fmt_f(hinge.z) << ")"
                         << " max_r0=" << fmt_f(max_rest_r) << " max_r1=" << fmt_f(max_xf_r)
                         << " max_pivot_drift=" << fmt_f(max_drift);
        if (i < origin_axis_map_.size() && origin_axis_map_[i] == OriginAxisMap::wiki_yz_swap)
            g_door_vert_file << " yz_swap=1";
        if (dump_rest) g_door_vert_file << " REST_SNAPSHOT";
        g_door_vert_file << '\n';
        g_door_vert_file << "i,x0,y0,z0,x1,y1,z1,r0,r1\n";

        for (std::size_t vi = 0; vi < rest.size(); ++vi) {
            const auto& v0 = rest[vi];
            const auto v1 = m.transform_point(v0);
            const float r0 = vec3_len(v0.x - hinge.x, v0.y - hinge.y, v0.z - hinge.z);
            const float r1 = vec3_len(v1.x - hinge.x, v1.y - hinge.y, v1.z - hinge.z);
            g_door_vert_file << vi << ',' << fmt_f(v0.x) << ',' << fmt_f(v0.y) << ',' << fmt_f(v0.z) << ','
                             << fmt_f(v1.x) << ',' << fmt_f(v1.y) << ',' << fmt_f(v1.z) << ',' << fmt_f(r0) << ','
                             << fmt_f(r1) << '\n';
        }
        g_door_vert_file << "END_FRAME\n";
        g_door_vert_file.flush();

        if (i < door_vert_rest_dumped_.size()) door_vert_rest_dumped_[i] = true;
        if (i < door_vert_prev_smooth_.size()) door_vert_prev_smooth_[i] = best_smooth;
    }
}

f4f23ma::ma4 MeshAnimator::matrix_for_mesh(std::size_t mesh_index) const {
    if (mesh_index < matrices_.size()) return matrices_[mesh_index];
    return f4f23ma::ma4::identity();
}

} // namespace omsi::model_cfg
