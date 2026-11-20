#pragma once

#include "omsi/map_types.hpp"
#include "omsi/o3d_format.hpp"
#include "omsi/sco_types.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace omsi::sco {

// O3D height plane for [crossing_heightdeformation] — local Z-up (same space as GPU verts).
struct CrossingHeightBaseMesh {
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::uint32_t> triangle_indices; // flat i0,i1,i2,...
    bool has_triangles() const { return triangle_indices.size() >= 3; }
};

struct CrossingHeightHit {
    float z = 0.f;
    float nx = 0.f;
    float ny = 0.f;
    float nz = 1.f;
    std::uint32_t triangle_index = 0;
};

struct CrossingMeshDeformStats {
    std::size_t total_verts = 0;
    std::size_t hit_verts = 0;
    float max_abs_dz = 0.f;
};

struct CrossingPathDeformStats {
    std::size_t total_paths = 0;
    std::size_t start_hits = 0;
    std::size_t height_deformed = 0;
};

inline constexpr float kCrossingHeightRayVertex = 300.f;
inline constexpr float kCrossingHeightRayPathEnd = 1000.f;

[[nodiscard]] std::filesystem::path resolve_sco_o3d(const std::filesystem::path& sco_dir,
                                                    std::string_view rel);
[[nodiscard]] bool mesh_stem_matches(std::string_view mesh_stem, std::string_view ref_stem);
[[nodiscard]] bool is_crossing_height_base_mesh(const SceneryObject& sco,
                                               const std::filesystem::path& mesh_path);
[[nodiscard]] bool should_deform_crossing_mesh(const SceneryObject& sco,
                                               const std::filesystem::path& mesh_path);
[[nodiscard]] std::optional<CrossingHeightBaseMesh> load_crossing_height_base(
    const std::filesystem::path& o3d_path);
[[nodiscard]] std::optional<CrossingHeightHit> raycast_crossing_height(float x, float y, float z,
                                                                       float ray_offset,
                                                                       const CrossingHeightBaseMesh& base);
[[nodiscard]] std::optional<std::vector<o3d::Vertex>> deform_o3d_for_crossing(
    const o3d::NativeMesh& mesh, const CrossingHeightBaseMesh& base,
    CrossingMeshDeformStats* stats_out = nullptr);
[[nodiscard]] map::SplineInst sco_path_to_spline_inst(const ScoPath& path);
[[nodiscard]] CrossingPathDeformStats deform_sco_paths(SceneryObject& sco,
                                                       const CrossingHeightBaseMesh& base);
void apply_crossing_height_to_sco(SceneryObject& sco);

} // namespace omsi::sco
