#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "f4f23ma/math.hpp"
#include "omsi/o3d_format.hpp"
#include "o3d/o3d_loader.hpp"
#include "omsi/map_types.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/spline_types.hpp"

namespace omsi::app::map_editor_math {

using DirectX::XMMATRIX;

std::array<float, 16> store_world_transposed(const XMMATRIX& world);
XMMATRIX ma4_to_xm(const f4f23ma::ma4& m);
// OMSI [object] intrinsic Euler: rot(Z) → pitch(X') → bank(Y''). Not world-fixed Ry/Rx.
XMMATRIX omsi_object_rotation_matrix(float yaw_deg, float pitch_deg, float bank_deg);
// Inverse of omsi_object_rotation_matrix for a pure rotation (translation ignored).
void omsi_euler_from_matrix(const XMMATRIX& m, float& yaw_deg, float& pitch_deg, float& bank_deg);
XMMATRIX sco_object_base_matrix(const map::MapObjectInst& obj, float terrain_h, float tile_ox = 0.f,
                                float tile_oy = 0.f);
XMMATRIX sco_world_matrix(const map::MapObjectInst& obj, const o3d::MappedO3D* mesh, float terrain_h,
                          float tile_ox = 0.f, float tile_oy = 0.f);
DirectX::XMVECTOR xform_point(const XMMATRIX& m, float x, float y, float z);

// OMSI_PATH_DEBUG=1 — log map_z / terrain / path.z / world_z per [path] placement.
[[nodiscard]] bool sco_path_debug_enabled();
void log_sco_path_placement(const map::MapObjectInst& obj, const sco::SceneryObject& sco, const sco::ScoPath& path,
                              int path_index, bool endpoint_start, float terrain_h, float world_x, float world_y,
                              float world_z, const char* site, float rail_lift_m = 0.f);

// global.cfg entrypoint mesh (Z-up tile-local).
XMMATRIX entrypoint_world_matrix(const map::EntryPoint& ep, float tile_ox, float tile_oy);

// attachObj child pose: object parent only (Unity ObjectAttachmentManager).
std::optional<XMMATRIX> attach_child_world_matrix(const map::MapAttachObjectInst& a,
                                                  const map::MapObjectInst* parent_obj,
                                                  const map::SplineInst* parent_spline, float parent_terrain_h,
                                                  std::optional<f4f23ma::f32> chain_end_z = std::nullopt);

void make_octahedron(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices, float r);
void make_thin_cylinder(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices, float radius,
                        float height, int segments = 10);
void make_uv_sphere(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices, float radius,
                    int segments = 12);
void make_arrow_mesh(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices);
void make_arrow_mesh_3d(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& indices);
void mesh_local_bbox(const o3d::MappedO3D& mapped, float out_min[3], float out_max[3]);

struct ScoWorldBounds {
    float min_x = 0.f, max_x = 0.f;
    float min_y = 0.f, max_y = 0.f;
    float min_z = 0.f, max_z = 0.f;
    bool valid = false;
};

[[nodiscard]] ScoWorldBounds sco_object_world_bounds(const map::MapObjectInst& obj,
                                                     const sco::SceneryObject& sco, float terrain_h,
                                                     float tile_ox = 0.f, float tile_oy = 0.f);

struct SurfaceFollowOptions {
    std::function<float(float x, float y)> sample_world_z;
    bool align_pitch_bank = false;
    bool lift_body = true;
    bool lock_z = false;
    float clearance = 0.01f;
    float sample_quant_m = 0.1f;
    int align_passes = 2;
};

void apply_surface_follow(map::MapObjectInst& obj, const sco::SceneryObject& sco, float terrain_h, bool abs_z,
                          const SurfaceFollowOptions& opt, float tile_ox = 0.f, float tile_oy = 0.f);

struct SpawnDebug {
    std::int32_t id = 0;
    std::string kind;
    std::string path;
    float map_x = 0, map_y = 0, map_z = 0;
    float terr_h = 0;
    float world_ox = 0, world_oy = 0;
    float expect_x = 0, expect_y = 0, expect_z = 0;
    float pivot_x = 0, pivot_y = 0, pivot_z = 0;
    float bbox_min[3]{}, bbox_max[3]{};
    float delta = 0;
    float scale_x = 1.f, scale_y = 1.f, scale_z = 1.f;
    float instance_scale = 1.f;
    bool ok = false;
};

SpawnDebug make_spawn_debug(const map::MapObjectInst& obj, const char* kind, const std::filesystem::path& path,
                            const XMMATRIX& world_m, float terr_h, float tile_ox, float tile_oy,
                            const o3d::MappedO3D* mesh, float local_hx = 8.f, float local_hy = 8.f, float local_hz = 4.f);
void log_spawn_debug(const SpawnDebug& dbg);

} // namespace omsi::app::map_editor_math
