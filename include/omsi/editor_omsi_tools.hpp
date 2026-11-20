#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "omsi/map_types.hpp"

namespace omsi::map {

// OMSI tile lightmap: sibling file "<tile.map>.LM.bmp" — 256×256 BGR24.
// Matches Unit126 TMap_CreateLightMap / wiki [maplight]: ambient slope + maplight splats
// (full brightness until radius, then falls with square of distance).

struct LightmapPoint {
    float x = 0.f, y = 0.f, z = 0.f; // tile-local metres
    float r = 1.f, g = 1.f, b = 1.f;  // 0..1
    float radius = 10.f;
};

[[nodiscard]] bool write_tile_lightmap_bmp(const std::filesystem::path& tile_map_path,
                                           const std::vector<float>& heights_61x61,
                                           const std::vector<LightmapPoint>& lights = {});

// Soft slope shade + optional maplights → 256² lightmap (BGR).
[[nodiscard]] std::vector<std::uint8_t> bake_lightmap_bgr_from_heights(
    const std::vector<float>& heights_61x61, float size_meters = 300.f,
    const std::vector<LightmapPoint>& lights = {});

// Collect [maplight] from every .sco on the tile (and light_enh as weaker fill).
[[nodiscard]] std::vector<LightmapPoint> collect_tile_maplights(const std::filesystem::path& tile_map_path,
                                                                const std::filesystem::path& omsi_root);

// Bake + write LM.bmp using heights + maplights from the .map.
[[nodiscard]] bool refresh_tile_lightmap(const std::filesystem::path& tile_map_path,
                                         const std::filesystem::path& omsi_root);

struct WholeMapJobOptions {
    bool create_lightmaps = false;
    bool validate_attach = false;
    bool list_objects = false;
    bool find_zero_splines = false;
    bool aerial_missing = false;
    bool aerial_all = false;
};

struct WholeMapJobResult {
    bool ok = false;
    std::string message;
    int lightmaps = 0;
    int zero_splines = 0;
    int attach_issues = 0;
    int aerials = 0;
};

// Batch ops (Do with whole map). Aerial download callback optional.
using AerialDownloadFn = bool (*)(const GlobalMap& global, std::int32_t grid_x, std::int32_t grid_y,
                                  bool only_if_missing, std::string& err);

[[nodiscard]] WholeMapJobResult run_whole_map_jobs(const GlobalMap& global, const WholeMapJobOptions& opt,
                                                   AerialDownloadFn aerial_fn = nullptr);

// List TTData tracks/trips and station-link file presence for editor browsers.
struct TimetableCatalog {
    std::vector<std::filesystem::path> tracks;   // *.ttr
    std::vector<std::filesystem::path> trips;    // *.ttp
    std::vector<std::filesystem::path> busstops; // Busstops.cfg etc.
    std::filesystem::path stn_links;
    std::filesystem::path signal_routes;
};

[[nodiscard]] TimetableCatalog scan_timetable_catalog(const std::filesystem::path& map_root);

// Create empty OMSI TTData track/trip skeletons (headers match Omsi 2.3).
[[nodiscard]] bool create_empty_track_file(const std::filesystem::path& map_root, const std::string& name_stem);
[[nodiscard]] bool create_empty_trip_file(const std::filesystem::path& map_root, const std::string& name_stem,
                                          int type /*1 or 2*/, const std::filesystem::path& from_track = {});
[[nodiscard]] bool copy_tt_file(const std::filesystem::path& src, const std::filesystem::path& dst_stem_path);

} // namespace omsi::map
