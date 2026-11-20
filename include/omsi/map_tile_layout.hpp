#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

#include "f4f23ma/math.hpp"
#include "omsi/map_types.hpp"

namespace omsi::map {

constexpr f4f23ma::f32 kStandardTileMeters = 300.f;
// OpenOMSI: 2πR/65536 ≈ 611.496 m (legacy rounded 611.5 kept for external callers).
constexpr f4f23ma::f32 kWorldEquatorTileMeters = 611.5f;

[[nodiscard]] bool is_global_coordinate_tile_name(const std::string& stem);
[[nodiscard]] bool parse_latitude_from_global_tile_name(const std::string& stem, f4f23ma::f32& latitude_deg);

struct TileCatalogEntry {
    std::int32_t grid_x = 0;
    std::int32_t grid_y = 0;
    f4f23ma::f32 origin_x = 0.f;
    f4f23ma::f32 origin_y = 0.f;
    f4f23ma::f32 size_meters = kStandardTileMeters;
    bool is_global = false;
    bool world_coordinates = false;
    std::filesystem::path tile_path;
};

struct MapTileLayout {
    bool map_world_coordinates = false;
    std::int32_t min_grid_x = 0;
    std::int32_t min_grid_y = 0;
    // World (0,0) stays on this cell across create/delete — not min_grid (which moves west/south).
    std::int32_t origin_anchor_gx = 0;
    std::int32_t origin_anchor_gy = 0;
    bool origin_anchor_set = false;
    std::vector<TileCatalogEntry> catalog;
    std::unordered_map<std::int64_t, TileCatalogEntry> by_grid;
    // WC prefix tables for O(1) grid_tile_rect_at (built in build_map_tile_layout).
    std::int32_t wc_gx_min = 0;
    std::int32_t wc_gy_min = 0;
    std::int32_t wc_nx = 0;
    std::int32_t wc_ny = 0;
    std::vector<f4f23ma::f32> wc_row_origin_y;
    std::vector<f4f23ma::f32> wc_row_height;
    std::vector<f4f23ma::f32> wc_col_origin_x;
    std::vector<f4f23ma::f32> wc_col_width;

    [[nodiscard]] const TileCatalogEntry* find_grid(std::int32_t grid_x, std::int32_t grid_y) const;
};

// Mirrors docs/js/map_processor.js getTileOriginFromMetric + applyWorldLayout.
// Optional keep_anchor: reuse previous origin_anchor so adding west/south tiles does not shift the map.
[[nodiscard]] MapTileLayout build_map_tile_layout(const GlobalMap& global);
[[nodiscard]] MapTileLayout build_map_tile_layout(const GlobalMap& global, std::int32_t origin_anchor_gx,
                                                  std::int32_t origin_anchor_gy);
[[nodiscard]] f4f23ma::f32 latitude_deg_from_grid_y(std::int32_t grid_y);
[[nodiscard]] f4f23ma::f32 tile_size_meters_from_grid_y(std::int32_t grid_y);
[[nodiscard]] f4f23ma::f32 tile_size_meters_at_grid(const GlobalMap& global, const MapTileLayout& layout,
                                                    std::int32_t grid_x, std::int32_t grid_y);

struct ActiveTileAtWorld {
    std::int32_t grid_x = 0;
    std::int32_t grid_y = 0;
    f4f23ma::f32 size_meters = kStandardTileMeters;
    f4f23ma::f32 origin_x = 0.f;
    f4f23ma::f32 origin_y = 0.f;
    f4f23ma::f32 latitude_deg = 0.f;
    bool in_map = false;
    bool resolved = false;
};

// World-space SW corner + size for a grid cell (catalog tile or inferred WC/classic).
struct GridTileRect {
    std::int32_t grid_x = 0;
    std::int32_t grid_y = 0;
    f4f23ma::f32 origin_x = 0.f;
    f4f23ma::f32 origin_y = 0.f;
    f4f23ma::f32 width_m = kStandardTileMeters;
    f4f23ma::f32 height_m = kStandardTileMeters;
    bool resolved = false;
};

[[nodiscard]] GridTileRect grid_tile_rect_at(const GlobalMap& global, const MapTileLayout& layout,
                                             std::int32_t grid_x, std::int32_t grid_y);

// tt-viewer: resolve grid + 611.5·cos(lat) size from editor world position (loaded tile or inferred).
[[nodiscard]] ActiveTileAtWorld grid_cell_at_world_xy(const GlobalMap& global, const MapTileLayout& layout,
                                                      f4f23ma::f32 world_x, f4f23ma::f32 world_y);
[[nodiscard]] ActiveTileAtWorld active_tile_at_world(const GlobalMap& global, const MapTileLayout& layout,
                                                     f4f23ma::f32 world_x, f4f23ma::f32 world_y);
[[nodiscard]] std::pair<f4f23ma::f32, f4f23ma::f32> tile_origin_xy(const MapTileLayout& layout,
                                                                      std::int32_t grid_x, std::int32_t grid_y);
// Entry local + [map] category origin (tt-viewer getTileOriginFromMetric). Classic and worldcoords.
[[nodiscard]] f4f23ma::f4 resolve_entry_world_position(const GlobalMap& global, const MapTileLayout& layout,
                                                       const EntryPoint& ep);
void resolve_entry_world_positions(GlobalMap& global);

[[nodiscard]] bool uses_absolute_tile_file_coords(const GlobalMap& global);
// Classic tile_0_0 maps: layout origins match grid*300 when min grid is (0,0).
[[nodiscard]] bool uses_classic_grid_tile_origins(const GlobalMap& global);
[[nodiscard]] const TileCatalogEntry* find_tile_at_world_position(const MapTileLayout& layout, f4f23ma::f32 world_x,
                                                                   f4f23ma::f32 world_y);

inline std::int64_t tile_grid_key(std::int32_t x, std::int32_t y) {
    return (static_cast<std::int64_t>(x) << 32) | static_cast<std::uint32_t>(y);
}

} // namespace omsi::map
