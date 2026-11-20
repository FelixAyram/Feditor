#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "f4f23ma/math.hpp"
#include "omsi/map_types.hpp"
#include "omsi/sco_types.hpp"

namespace omsi::map {

// Concatenated spline list index entry (Unity GlobalSplineIndexToTile).
struct GlobalSplineIndexEntry {
    std::int32_t tile_gx = 0;
    std::int32_t tile_gy = 0;
    std::int32_t local_index = 0;
};

struct TileSplineRef {
    std::int32_t tile_gx = 0;
    std::int32_t tile_gy = 0;
    std::int32_t local_index = 0;
};

[[nodiscard]] std::vector<GlobalSplineIndexEntry> build_global_spline_index(
    const GlobalMap& global, const std::filesystem::path& omsi_root);

[[nodiscard]] std::vector<GlobalSplineIndexEntry> build_global_spline_index_from_tiles(
    const std::vector<MapTileData>& tiles);

// Unity SplineAttachmentManager.TryResolveAttachmentSpline: local index first, then global.
[[nodiscard]] std::optional<TileSplineRef> resolve_spline_attachment_index(
    std::int32_t spline_index, std::int32_t attachment_tile_gx, std::int32_t attachment_tile_gy,
    std::int32_t attachment_tile_spline_count, const std::vector<GlobalSplineIndexEntry>& global_index);

[[nodiscard]] const SplineInst* find_spline_in_tiles(const std::vector<MapTileData>& tiles,
                                                      const TileSplineRef& ref);

// Unity EnumerateAlongDistances — start = offsetAlong - localLength (length when field absent).
[[nodiscard]] f4f23ma::f32 spline_local_length(const SplineInst& sp);

[[nodiscard]] std::vector<float> enumerate_spline_attachment_along(const MapSplineAttachmentInst& att,
                                                                     const SplineInst& spline);

struct SplineAttachAlongHit {
    std::int32_t spline_id = 0;
    float along = 0.f;
};

// Walks prev_id if startAlong < 0. Repeat always uses repeat_range; next_id lengths are summed
// so the range can span linked splines (and is clipped to that chain).
[[nodiscard]] std::vector<SplineAttachAlongHit> enumerate_spline_attachment_along_chain(
    const MapSplineAttachmentInst& att, const SplineInst& start,
    const std::vector<SplineInst>& splines);

struct SplineAttachmentPose {
    f4f23ma::f32 x = 0.f;
    f4f23ma::f32 y = 0.f;
    f4f23ma::f32 z = 0.f;
    f4f23ma::f32 yaw_deg = 0.f;
    f4f23ma::f32 pitch_deg = 0.f;
    f4f23ma::f32 bank_deg = 0.f;
};

[[nodiscard]] SplineAttachmentPose compute_spline_attachment_pose(
    const SplineInst& spline, const MapSplineAttachmentInst& att, float along_m,
    std::optional<f4f23ma::f32> chain_end_z = std::nullopt, bool tangential_follow = false);

// Editor panel: x=lateral, y=along (0..length), z=height offset (OMSI offset_x/y/along).
struct SplineAttachPanelCoords {
    f4f23ma::f32 x = 0.f;
    f4f23ma::f32 y = 0.f;
    f4f23ma::f32 z = 0.f;
    f4f23ma::f32 rot = 0.f;
};

[[nodiscard]] SplineAttachPanelCoords spline_attach_to_panel(const SplineInst& spline,
                                                             const MapSplineAttachmentInst& att);
[[nodiscard]] bool spline_attach_from_panel(const SplineInst& spline, f4f23ma::f32 px, f4f23ma::f32 py,
                                            f4f23ma::f32 pz, MapSplineAttachmentInst& att);

struct SplinePointProjection {
    f4f23ma::f32 along_m = 0.f;
    f4f23ma::f32 lateral_m = 0.f;
    f4f23ma::f32 dist_sq = 0.f;
};

[[nodiscard]] SplinePointProjection project_point_onto_spline(
    const SplineInst& spline, f4f23ma::f32 wx, f4f23ma::f32 wy,
    std::optional<f4f23ma::f32> chain_end_z = std::nullopt);

// [attachObj] with object parent only (Unity ObjectAttachmentManager / TrySpawnAttachedObject).
struct ObjectAttachmentPose {
    f4f23ma::f32 x = 0.f;
    f4f23ma::f32 y = 0.f;
    f4f23ma::f32 z = 0.f;
    f4f23ma::f32 yaw_deg = 0.f;
    f4f23ma::f32 pitch_deg = 0.f;
    f4f23ma::f32 bank_deg = 0.f;
};

// Local euler of the child at the attach point (parent heading is applied separately).
// yaw = attach_rot_z + extra heading (.map extra_rot_x + extra_rot_y)
// pitch = attach_rot_x + extra_rot_y2
// bank = attach_rot_y + extra_rot_z
[[nodiscard]] ObjectAttachmentPose attach_obj_local_euler(const MapAttachObjectInst& attach,
                                                          const sco::AttachmentPoint& pt);

[[nodiscard]] std::optional<ObjectAttachmentPose> compute_attach_obj_pose(const MapAttachObjectInst& attach,
                                                                          const MapObjectInst& parent);

[[nodiscard]] f4f23ma::ma4 attach_obj_tile_transform(const MapAttachObjectInst& attach,
                                                     const MapObjectInst& parent);

// Map extra-string line: "var=value" → value; otherwise whole line.
[[nodiscard]] std::string extract_object_text_value(const std::string& cleaned);

// Parse one [splineAttachement] block at tag_line (Unity OMSICfgReader line indices).
[[nodiscard]] std::optional<MapSplineAttachmentInst> parse_spline_attachment_at_tag_line(
    const std::vector<std::string>& lines, std::size_t tag_line,
    const std::filesystem::path& omsi_root = {});

// Parse [splineAttachement] / [splineAttachement_repeater] from raw .map lines (Unity OMSICfgReader).
[[nodiscard]] std::vector<MapSplineAttachmentInst> parse_map_spline_attachment_entries(
    const std::filesystem::path& tile_map);

} // namespace omsi::map
