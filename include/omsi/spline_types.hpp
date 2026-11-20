#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "f4f23ma/math.hpp"
#include "omsi/map_types.hpp"
#include "omsi/o3d_format.hpp"

namespace omsi::spline {

struct HeightProfile {
    f4f23ma::f32 left = 0.f;
    f4f23ma::f32 right = 0.f;
    f4f23ma::f32 height = 0.f;
    f4f23ma::f32 skew = 0.f;
};

// [profilepnt]: x, z(height), uv_x, y_tiling — Blender-O3D-IO load_spline.
struct ProfilePoint {
    f4f23ma::f32 x = 0.f;
    f4f23ma::f32 z = 0.f;
    f4f23ma::f32 uv_x = 0.f;
    f4f23ma::f32 y_tiling = 1.f;
    std::int32_t material_id = 0;
};

struct SliPath {
    std::int32_t typ = 0;
    f4f23ma::f32 lateral = 0.f;
    f4f23ma::f32 height = 0.f;
    f4f23ma::f32 width = 3.f; // 4th [path] field when 5 lines present (lane width)
    std::int32_t direction = 2;
    bool secondary = false; // true when declared under [path_2]
};

// Unit84 [patchwork_chain]: seglen, transitions, factors, inverse (0/1).
// Applies to the last material declared before the tag (materialindex).
struct PatchworkChain {
    bool valid = false;
    std::int32_t material_index = -1;
    f4f23ma::f32 seg_length = 10.f;
    std::string transitions; // e.g. ABCDABCD...
    std::string factors;     // digits, len = transitions-1
    std::string inverse;     // '0'/'1', len = transitions-1
};

// SplineCreator.exe extras — optional sidecar .surf / .cfg on export.
struct TextureExtra {
    bool damage_surf = false;
    bool puddles = false;
    bool moisture = false;
};

// [terrainholeprofilepnt] — TSplineObject.holeprofiles (x=lateral, y=height, z=cap along-inset).
struct HoleProfilePoint {
    f4f23ma::f32 x = 0.f;
    f4f23ma::f32 y = 0.f;
    f4f23ma::f32 z = 0.f;
};

struct SplineDefinition {
    std::filesystem::path sli_path;
    std::vector<HeightProfile> height_profiles;
    std::vector<std::vector<HoleProfilePoint>> hole_profiles;
    std::vector<std::vector<ProfilePoint>> profiles;
    // Parallel to profiles: optional "### Name ###" section headers in Graphical Lanes.
    std::vector<std::string> profile_comments;
    std::vector<std::string> textures;
    // Parallel to textures: [matl_alpha] after each [texture] (default opaque).
    std::vector<std::uint8_t> texture_alpha; // 0/1/2 = MatlAlphaMode
    std::vector<TextureExtra> texture_extra;
    std::vector<SliPath> paths;
    PatchworkChain patchwork{};
    f4f23ma::f32 path_length = 10.f; // [length] — repeat interval for path strips (helper/path.sli)
    bool only_editor = false;
};

struct SplineSample {
    f4f23ma::f32 x = 0.f;
    f4f23ma::f32 y = 0.f;
    f4f23ma::f32 z = 0.f;
    f4f23ma::f32 heading_rad = 0.f;
    f4f23ma::f32 pitch_rad = 0.f;
    f4f23ma::f32 cant_rad = 0.f;
};

struct SplineMesh {
    std::vector<o3d::Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::filesystem::path texture_path;
    std::uint8_t alpha_mode = 0; // MatlAlphaMode: 0 opaque, 1 test, 2 blend
};

[[nodiscard]] std::optional<SplineDefinition> load_sli(const std::filesystem::path& sli_path);
// Texture slots whose texture's .cfg has [terrainmapping] (grass verges: the file itself is a black
// placeholder) are drawn with the map's first [groundtex], UVs from the world position / scale (OMSI).
void set_terrain_mapping_ground(const std::filesystem::path& ground_texture, float scale_m);
// Cached parse; shares storage across callers (no copy on hit).
[[nodiscard]] std::shared_ptr<const SplineDefinition> load_sli_shared(const std::filesystem::path& sli_path);
// Cache lookup only — never reads disk. nullopt if not loaded yet.
[[nodiscard]] std::optional<std::size_t> peek_sli_path_count(const std::filesystem::path& sli_path);

// Lowest profilepnt Z relative to spline centerline (typically <= 0).
[[nodiscard]] inline f4f23ma::f32 sli_profile_min_z(const SplineDefinition& def) {
    f4f23ma::f32 min_z = 0.f;
    for (const auto& profile : def.profiles)
        for (const auto& pp : profile) min_z = (std::min)(min_z, pp.z);
    return min_z;
}
[[nodiscard]] bool write_sli(const SplineDefinition& def, const std::filesystem::path& sli_path);
[[nodiscard]] std::string format_sli(const SplineDefinition& def);
[[nodiscard]] bool export_sli_with_textures(
    const SplineDefinition& def, const std::filesystem::path& sli_path, const std::filesystem::path& omsi_root,
    const std::unordered_map<std::string, std::filesystem::path>& tex_sources = {});
[[nodiscard]] std::vector<std::string> validate_sli(const SplineDefinition& def);
// Drop cached parse (after Reload .sli). Empty path clears all.
void invalidate_sli_cache(const std::filesystem::path& sli_path = {});

// Port of Blender-O3D-IO / Unity OmsiScoPathGeometry — Z-up tile-local.
[[nodiscard]] SplineSample evaluate_spline(
    const map::SplineInst& sp,     f4f23ma::f32 along_m, f4f23ma::f32 lateral_m = 0.f,
    f4f23ma::f32 height_off = 0.f, std::optional<f4f23ma::f32> chain_end_z = std::nullopt,
    bool mirror_lateral = true,
    f4f23ma::f32 half_cant_width = 1.e9f);

// World Z of the lowest profile point at spline start (uses full evaluate_spline geometry).
[[nodiscard]] inline f4f23ma::f32 spline_mesh_bottom_z(const map::SplineInst& sp, const SplineDefinition& def,
                                                         std::optional<f4f23ma::f32> chain_end_z = std::nullopt) {
    f4f23ma::f32 min_z = sp.position.z;
    for (const auto& profile : def.profiles) {
        for (const auto& pp : profile) {
            const SplineSample samp = evaluate_spline(sp, 0.f, pp.x, pp.z, chain_end_z);
            min_z = (std::min)(min_z, samp.z);
        }
    }
    return min_z;
}

// Start Z of the next segment in a spline chain (for cubic height like OMSI RefreshNachfolger).
[[nodiscard]] inline std::optional<f4f23ma::f32> spline_chain_end_z(
    const map::SplineInst& sp, const std::unordered_map<std::int32_t, f4f23ma::f32>& id_to_start_z) {
    if (sp.next_id == 0) return std::nullopt;
    const auto it = id_to_start_z.find(sp.next_id);
    if (it == id_to_start_z.end()) return std::nullopt;
    return it->second;
}

// Mesh tessellation: never pin chain_end_z — gradient % / [spline_h] delta_height drive Z.
// (Chain RefreshNachfolger updates .map node Z; pinning next.position.z hid slopes when delta≈0.)
[[nodiscard]] inline std::optional<f4f23ma::f32> spline_mesh_chain_end_z(const map::SplineInst& sp,
                                                                          f4f23ma::f32 next_start_z) {
    (void)sp;
    (void)next_start_z;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<f4f23ma::f32> resolve_spline_mesh_chain_end_z(
    const map::SplineInst& sp, const std::unordered_map<std::int32_t, f4f23ma::f32>& id_to_start_z) {
    const auto raw = spline_chain_end_z(sp, id_to_start_z);
    if (!raw) return std::nullopt;
    return spline_mesh_chain_end_z(sp, *raw);
}

// OMSI TSplineSegment.Generate teile (Omsi.exe @ 0x5b1ec4 + _Unit84 TCoordSystem steigungA/C).
[[nodiscard]] int omis_teile(f4f23ma::f32 length, f4f23ma::f32 radius, int max_teilung = 200);
[[nodiscard]] int omis_teile(const map::SplineInst& sp, std::optional<f4f23ma::f32> chain_end_z = std::nullopt,
                             int max_teilung = 200);

// OMSI TCoordSystem.CircleToNorm — arc/straight from start pose to target XY.
void fit_spline_to_xy(f4f23ma::f32 ox, f4f23ma::f32 oy, f4f23ma::f32 heading_deg, f4f23ma::f32 tx,
                      f4f23ma::f32 ty, f4f23ma::f32& out_len, f4f23ma::f32& out_radius);
// Arc with fixed start pose and exact end heading (Complete to / autofill last leg).
[[nodiscard]] bool fit_spline_to_end_heading(f4f23ma::f32 x0, f4f23ma::f32 y0, f4f23ma::f32 h0_deg,
                                             f4f23ma::f32 x1, f4f23ma::f32 y1, f4f23ma::f32 h1_deg,
                                             f4f23ma::f32& out_len, f4f23ma::f32& out_radius);

// tess_dist<=0 → use omis_teile (OMSI parity). Positive → legacy meters-per-segment.
// sample_terrain is unused (OMSI does not drape the spline; terrain_align cuts/stamps terrain).
using SplineTerrainSampler = std::function<float(float x, float y)>;
[[nodiscard]] std::vector<SplineMesh> build_spline_meshes(
    const map::SplineInst& sp, const SplineDefinition& def, const std::filesystem::path& omsi_root,
    f4f23ma::f32 tess_dist = 0.f, std::optional<f4f23ma::f32> chain_end_z = std::nullopt,
    SplineTerrainSampler sample_terrain = {});

// Map editor/world XY onto spline (along, lateral). False if not on the segment.
[[nodiscard]] bool world_xy_to_spline_local(const map::SplineInst& sp, f4f23ma::f32 wx, f4f23ma::f32 wy,
                                            f4f23ma::f32& along, f4f23ma::f32& lateral);

// Min XY distance² to start, end, or the closest point on the centerline. A spline that
// *finishes* next to the camera is near even if its [spline] origin is far.
[[nodiscard]] inline f4f23ma::f32 spline_focus_dist2_xy(const map::SplineInst& sp, f4f23ma::f32 wx,
                                                        f4f23ma::f32 wy) {
    const f4f23ma::f32 dsx = sp.position.x - wx;
    const f4f23ma::f32 dsy = sp.position.y - wy;
    f4f23ma::f32 best = dsx * dsx + dsy * dsy;
    if (sp.length > 0.5f) {
        const SplineSample e = evaluate_spline(sp, sp.length);
        const f4f23ma::f32 dex = e.x - wx;
        const f4f23ma::f32 dey = e.y - wy;
        const f4f23ma::f32 de = dex * dex + dey * dey;
        if (de < best) best = de;
    }
    f4f23ma::f32 along = 0.f, lat = 0.f;
    if (world_xy_to_spline_local(sp, wx, wy, along, lat)) {
        const f4f23ma::f32 dlat = lat * lat;
        if (dlat < best) best = dlat;
    }
    return best;
}

[[nodiscard]] inline bool spline_in_radius_xy(const map::SplineInst& sp, f4f23ma::f32 wx, f4f23ma::f32 wy,
                                              f4f23ma::f32 radius_m) {
    if (radius_m <= 0.f) return true;
    return spline_focus_dist2_xy(sp, wx, wy) <= radius_m * radius_m;
}

// Parent→child joint (OMSI editor / Feditor). Headings in degrees; tan/atan in radians.
//   B.hdg = A.end_hdg - 2*atan(A.skew_end)
inline void apply_child_joint_from_parent(map::SplineInst& child, const map::SplineInst& parent) {
    constexpr f4f23ma::f32 kPi = 3.14159265358979323846f;
    const auto e = evaluate_spline(parent, parent.length);
    child.position.x = e.x;
    child.position.y = e.y;
    child.position.z = e.z;
    child.heading_deg = (e.heading_rad - 2.f * std::atan(parent.skew_end)) * (180.f / kPi);
    while (child.heading_deg > 180.f) child.heading_deg -= 360.f;
    while (child.heading_deg <= -180.f) child.heading_deg += 360.f;
    child.skew_start = -parent.skew_end;
}

inline void apply_parent_skew_from_child_heading(map::SplineInst& parent, map::SplineInst& child) {
    constexpr f4f23ma::f32 kPi = 3.14159265358979323846f;
    const auto e = evaluate_spline(parent, parent.length);
    const f4f23ma::f32 parent_end_deg = e.heading_rad * (180.f / kPi);
    f4f23ma::f32 d = std::fmod(parent_end_deg - child.heading_deg, 360.f);
    if (d > 180.f) d -= 360.f;
    else if (d <= -180.f) d += 360.f;
    d = (std::clamp)(d, -179.f, 179.f);
    parent.skew_end = std::tan((d * 0.5f) * (kPi / 180.f));
    child.heading_deg = parent_end_deg - d;
    while (child.heading_deg > 180.f) child.heading_deg -= 360.f;
    while (child.heading_deg <= -180.f) child.heading_deg += 360.f;
    child.skew_start = -parent.skew_end;
}

inline void apply_spline_chain_headings(std::vector<map::SplineInst>& splines) {
    std::unordered_map<std::int32_t, std::size_t> idx;
    idx.reserve(splines.size() * 2);
    for (std::size_t i = 0; i < splines.size(); ++i) idx[splines[i].id] = i;
    for (std::size_t i = 0; i < splines.size(); ++i) {
        if (splines[i].prev_id != 0) continue;
        std::unordered_set<std::int32_t> seen;
        std::size_t cur_i = i;
        for (int hop = 0; hop < 256; ++hop) {
            auto& cur = splines[cur_i];
            if (!seen.insert(cur.id).second) break;
            if (cur.next_id == 0) break;
            const auto it = idx.find(cur.next_id);
            if (it == idx.end()) break;
            auto& nxt = splines[it->second];
            apply_child_joint_from_parent(nxt, cur);
            cur_i = it->second;
        }
    }
}

// OMSI TSplineObject load @ 0x5adc52 / openOMSI terrain_hole_profiles: authored
// [terrainholeprofile]s, else auto-troughs from graphical [profile] chains (3 cm inset,
// 10 cm below lowest, ends ±0.5 m). Empty only when there is nothing to cut.
[[nodiscard]] std::vector<std::vector<HoleProfilePoint>>
effective_terrain_hole_profiles(const SplineDefinition& def);

// ---------------------------------------------------------------------------------------------
// Terrain-align plan: everything about the spline set that does NOT depend on the terrain tile,
// computed ONCE per restamp batch instead of once per (tile x spline).
//
// Before this existed every restamp of every tile did, for every aligned spline (~2300 on the
// large map): load_sli_shared() (mutex + LRU + path string), effective_terrain_hole_profiles()
// (deep copy), sorted_hole_profile() (copy + sort), a spline_id->z hash map, and only THEN the
// distance rejection. With ~315 tiles that is ~700k heavy iterations on the main thread.
// The plan is immutable after construction, so worker threads can share it freely.
// ---------------------------------------------------------------------------------------------
struct TerrainAlignRing {
    std::vector<HoleProfilePoint> sorted; // finite points sorted by x
    f4f23ma::f32 xmin = 0.f;
    f4f23ma::f32 xmax = 0.f;
    bool valid = false; // hole_profile_limits() passed
};

struct TerrainAlignCompiled {
    std::shared_ptr<const SplineDefinition> def;
    std::vector<std::vector<HoleProfilePoint>> raw_rings; // effective_terrain_hole_profiles(*def)
    std::vector<TerrainAlignRing> rings;                  // same order as raw_rings
    f4f23ma::f32 lateral_extent = 0.f;                    // max |x| over finite raw points
    f4f23ma::f32 end_extent = 0.f;                        // max |z| over finite raw points
};

struct TerrainAlignEntry {
    const map::SplineInst* sp = nullptr; // points into TerrainAlignPlan::splines
    std::shared_ptr<const TerrainAlignCompiled> comp;
    std::optional<f4f23ma::f32> chain_end_z;
    // Conservative world-space AABB (superset of every per-ring reach test).
    f4f23ma::f32 min_x = 0.f, max_x = 0.f, min_y = 0.f, max_y = 0.f;
};

struct TerrainAlignPlan {
    std::vector<map::SplineInst> splines; // snapshot: safe to use from worker threads
    std::vector<TerrainAlignEntry> entries; // only aligned splines with a usable hole profile
};

// Snapshot + precompile. Missing / unreadable .sli files are looked up once per plan.
[[nodiscard]] std::shared_ptr<const TerrainAlignPlan>
build_terrain_align_plan(const std::vector<map::SplineInst>& splines);

// Cheap broad phase. Only returns false for a DEFINITE miss (NaN keeps the old behaviour of
// "not rejected"), so callers that still run the exact per-ring test see identical results.
[[nodiscard]] inline bool terrain_align_entry_may_touch(const TerrainAlignEntry& e, f4f23ma::f32 tile_rel_x,
                                                        f4f23ma::f32 tile_rel_y, f4f23ma::f32 size) {
    if (e.max_x < tile_rel_x || e.min_x > tile_rel_x + size) return false;
    if (e.max_y < tile_rel_y || e.min_y > tile_rel_y + size) return false;
    return true;
}

// OMSI TSplineSegment.alignTerrain: stamp hole profile into a 61×61 heightfield (in-memory).
// Spline XY must be in the same space as tile_rel + local. Does not write .terrain.
void stamp_terrain_align(std::vector<float>& heights, f4f23ma::f32 size_m, f4f23ma::f32 tile_rel_x,
                         f4f23ma::f32 tile_rel_y, const std::vector<map::SplineInst>& splines,
                         int dim = 61);
// Same result, but reuses a prebuilt plan (no per-tile .sli lookups / profile copies).
void stamp_terrain_align(std::vector<float>& heights, f4f23ma::f32 size_m, f4f23ma::f32 tile_rel_x,
                         f4f23ma::f32 tile_rel_y, const TerrainAlignPlan& plan, int dim = 61);

// Mask cells under [spline_terrain_align/_2] strips (and optionally SCO align circles) so the
// terrain mesh can drop those quads — openOMSI-style hole cut (pasto no tapa la carretera).
// mask size = dim*dim; non-zero = cut. Modes 1–4 shrink along-range at ends like OMSI rims.
void mask_terrain_align_holes(std::vector<std::uint8_t>& mask, f4f23ma::f32 size_m, f4f23ma::f32 tile_rel_x,
                              f4f23ma::f32 tile_rel_y, const std::vector<map::SplineInst>& splines,
                              int dim = 61);
void mask_terrain_align_holes(std::vector<std::uint8_t>& mask, f4f23ma::f32 size_m, f4f23ma::f32 tile_rel_x,
                              f4f23ma::f32 tile_rel_y, const TerrainAlignPlan& plan, int dim = 61);
void mask_object_terrain_align_holes(std::vector<std::uint8_t>& mask, f4f23ma::f32 size_m,
                                     f4f23ma::f32 tile_rel_x, f4f23ma::f32 tile_rel_y, int dim,
                                     f4f23ma::f32 wx, f4f23ma::f32 wy, f4f23ma::f32 heading_deg,
                                     f4f23ma::f32 half_x, f4f23ma::f32 half_y);

// Lowest world Z across tessellated .sli mesh (same geometry as GPU).
[[nodiscard]] f4f23ma::f32 spline_mesh_world_min_z(const map::SplineInst& sp, const SplineDefinition& def,
                                                   const std::filesystem::path& omsi_root,
                                                   std::optional<f4f23ma::f32> chain_end_z = std::nullopt);

// Path overlay on a parent spline: keep parent arc length/radius; offset lateral per sample
// (OMSI editor — do NOT rebuild a parallel R'/L' spline; see evaluate_spline lateral).
[[nodiscard]] std::optional<SplineMesh> build_path_rail_mesh(
    const map::SplineInst& parent, f4f23ma::f32 path_lateral, f4f23ma::f32 path_height,
    const SplineDefinition& path_def, f4f23ma::f32 z_bias = 0.12f,
    std::optional<f4f23ma::f32> chain_end_z = std::nullopt);

} // namespace omsi::spline
