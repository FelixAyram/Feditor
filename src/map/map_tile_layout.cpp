#include "omsi/map_tile_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <limits>

#include "omsi/logger.hpp"

namespace omsi::map {
namespace {

constexpr f4f23ma::f32 kWgs84RadiusM = 6378137.f;
constexpr f4f23ma::f32 kPi = 3.14159265f;
// OpenOMSI / Marcel forum: N=65536, lat=atan(sinh(π·2y/N)), width=2πR·cos(lat)/N.
constexpr f4f23ma::f32 kRealTileCount = 65536.f;
constexpr f4f23ma::f32 kEquatorTileM = kWgs84RadiusM * kPi * 2.f / kRealTileCount; // ≈611.496

f4f23ma::f32 latitude_from_grid_y(const std::int32_t tile_y) {
    const f4f23ma::f32 lat_rad =
        std::atan(std::sinh(kPi * (2.f * static_cast<f4f23ma::f32>(tile_y) / kRealTileCount)));
    return lat_rad * 180.f / kPi;
}

f4f23ma::f32 tile_size_from_grid_y(const std::int32_t tile_y) {
    const f4f23ma::f32 lat_rad = latitude_from_grid_y(tile_y) * (kPi / 180.f);
    return kEquatorTileM * std::cos(lat_rad);
}

f4f23ma::f32 row_height_at_grid_y(const MapTileLayout& layout, const std::int32_t grid_y) {
    for (const auto& metric : layout.catalog) {
        if (metric.grid_y == grid_y) return metric.size_meters;
    }
    return tile_size_from_grid_y(grid_y);
}

f4f23ma::f32 col_width_at_grid(const MapTileLayout& layout, const std::int32_t grid_x,
                               const std::int32_t grid_y) {
    if (const auto* metric = layout.find_grid(grid_x, grid_y)) return metric->size_meters;
    return tile_size_from_grid_y(grid_y);
}

f4f23ma::f32 tile_origin_y_bidir(const MapTileLayout& layout, const std::int32_t grid_y) {
    // Prefix-sum of per-row heights (cos(lat)·equator). gy·equator leaves a gap at mid-latitudes
    // because height < equator pitch (e.g. ~238 m between gy 11283 and 11284 @ ~52°).
    const std::int32_t ay = layout.origin_anchor_set ? layout.origin_anchor_gy : layout.min_grid_y;
    if (grid_y == ay) return 0.f;
    double acc = 0.0;
    if (grid_y > ay) {
        for (std::int32_t gy = ay; gy < grid_y; ++gy)
            acc += static_cast<double>(row_height_at_grid_y(layout, gy));
        return static_cast<f4f23ma::f32>(acc);
    }
    for (std::int32_t gy = grid_y; gy < ay; ++gy)
        acc += static_cast<double>(row_height_at_grid_y(layout, gy));
    return static_cast<f4f23ma::f32>(-acc);
}

f4f23ma::f32 tile_origin_x_bidir(const MapTileLayout& layout, const std::int32_t grid_x,
                                 const std::int32_t grid_y) {
    // Direct: x·width(lat) on this row (uniform column pitch; no prefix sum).
    const std::int32_t ax = layout.origin_anchor_set ? layout.origin_anchor_gx : layout.min_grid_x;
    const f4f23ma::f32 w = col_width_at_grid(layout, grid_x, grid_y);
    return static_cast<f4f23ma::f32>(grid_x - ax) * w;
}

bool wc_rect_lookup(const MapTileLayout& layout, const std::int32_t grid_x, const std::int32_t grid_y,
                    GridTileRect& out) {
    if (layout.wc_nx <= 0 || layout.wc_ny <= 0) return false;
    const std::int32_t ix = grid_x - layout.wc_gx_min;
    const std::int32_t iy = grid_y - layout.wc_gy_min;
    if (ix < 0 || iy < 0 || ix >= layout.wc_nx || iy >= layout.wc_ny) return false;
    const std::int32_t anchor_ix = layout.origin_anchor_gx - layout.wc_gx_min;
    const std::int32_t anchor_iy = layout.origin_anchor_gy - layout.wc_gy_min;
    if (anchor_ix < 0 || anchor_iy < 0 || anchor_ix >= layout.wc_nx || anchor_iy >= layout.wc_ny) return false;
    const std::size_t idx =
        static_cast<std::size_t>(iy) * static_cast<std::size_t>(layout.wc_nx) + static_cast<std::size_t>(ix);
    const std::size_t anchor_idx =
        static_cast<std::size_t>(anchor_iy) * static_cast<std::size_t>(layout.wc_nx) +
        static_cast<std::size_t>(anchor_ix);
    out.grid_x = grid_x;
    out.grid_y = grid_y;
    // X pitch is per-row (cos(lat)); never subtract anchor column origin from another row's width.
    out.width_m = layout.wc_col_width[idx];
    out.height_m = layout.wc_row_height[static_cast<std::size_t>(iy)];
    out.origin_x =
        static_cast<f4f23ma::f32>(grid_x - layout.origin_anchor_gx) * out.width_m;
    out.origin_y = layout.wc_row_origin_y[static_cast<std::size_t>(iy)] -
                   layout.wc_row_origin_y[static_cast<std::size_t>(anchor_iy)];
    out.resolved = true;
    return true;
}

bool wc_cell_from_world(const MapTileLayout& layout, const f4f23ma::f32 world_x, const f4f23ma::f32 world_y,
                        GridTileRect& out) {
    if (layout.wc_nx <= 0 || layout.wc_ny <= 0) return false;
    const std::int32_t ax = layout.origin_anchor_gx - layout.wc_gx_min;
    const std::int32_t ay = layout.origin_anchor_gy - layout.wc_gy_min;
    if (ax < 0 || ay < 0 || ax >= layout.wc_nx || ay >= layout.wc_ny) return false;
    const auto& rows = layout.wc_row_origin_y;
    if (rows.empty()) return false;
    const f4f23ma::f32 y_abs = world_y + rows[static_cast<std::size_t>(ay)];
    auto yit = std::upper_bound(rows.begin(), rows.end(), y_abs);
    if (yit == rows.begin()) return false;
    const std::int32_t iy = static_cast<std::int32_t>(std::distance(rows.begin(), yit) - 1);
    if (iy < 0 || iy >= layout.wc_ny) return false;
    if (y_abs >= rows[static_cast<std::size_t>(iy)] + layout.wc_row_height[static_cast<std::size_t>(iy)])
        return false;
    const std::size_t aidx = static_cast<std::size_t>(ay) * static_cast<std::size_t>(layout.wc_nx) +
                             static_cast<std::size_t>(ax);
    const f4f23ma::f32 x_abs = world_x + layout.wc_col_origin_x[aidx];
    std::int32_t lo = 0;
    std::int32_t hi = layout.wc_nx;
    while (lo < hi) {
        const std::int32_t mid = lo + (hi - lo) / 2;
        const std::size_t idx =
            static_cast<std::size_t>(iy) * static_cast<std::size_t>(layout.wc_nx) + static_cast<std::size_t>(mid);
        if (layout.wc_col_origin_x[idx] <= x_abs) lo = mid + 1;
        else hi = mid;
    }
    const std::int32_t ix = lo - 1;
    if (ix < 0 || ix >= layout.wc_nx) return false;
    const std::int32_t gx = layout.wc_gx_min + ix;
    const std::int32_t gy = layout.wc_gy_min + iy;
    if (!wc_rect_lookup(layout, gx, gy, out)) return false;
    return world_x >= out.origin_x && world_x < out.origin_x + out.width_m && world_y >= out.origin_y &&
           world_y < out.origin_y + out.height_m;
}

ActiveTileAtWorld active_tile_from_rect(const MapTileLayout& layout, const GridTileRect& rect) {
    ActiveTileAtWorld out{};
    out.grid_x = rect.grid_x;
    out.grid_y = rect.grid_y;
    out.size_meters = rect.width_m;
    out.origin_x = rect.origin_x;
    out.origin_y = rect.origin_y;
    out.latitude_deg = latitude_from_grid_y(rect.grid_y);
    out.in_map = layout.find_grid(rect.grid_x, rect.grid_y) != nullptr;
    out.resolved = true;
    return out;
}

bool world_xy_in_grid_rect(const GridTileRect& rect, const f4f23ma::f32 world_x, const f4f23ma::f32 world_y) {
    return world_x >= rect.origin_x && world_x < rect.origin_x + rect.width_m && world_y >= rect.origin_y &&
           world_y < rect.origin_y + rect.height_m;
}

std::int32_t max_grid_y_in_layout(const MapTileLayout& layout) {
    std::int32_t max_y = layout.min_grid_y;
    for (const auto& metric : layout.catalog) max_y = std::max(max_y, metric.grid_y);
    return max_y;
}

std::int32_t max_grid_x_in_layout(const MapTileLayout& layout) {
    std::int32_t max_x = layout.min_grid_x;
    for (const auto& metric : layout.catalog) max_x = std::max(max_x, metric.grid_x);
    return max_x;
}

TileCatalogEntry make_tile_metric(const MapTileRef& ref, const bool map_world_coordinates) {
    TileCatalogEntry entry{};
    entry.grid_x = ref.grid_x;
    entry.grid_y = ref.grid_y;
    entry.tile_path = ref.tile_path;

    const auto stem = ref.tile_path.stem().string();
    entry.is_global = is_global_coordinate_tile_name(stem);
    entry.world_coordinates = map_world_coordinates && !entry.is_global;

    if (entry.is_global) {
        f4f23ma::f32 lat = 0.f;
        if (parse_latitude_from_global_tile_name(stem, lat)) {
            const f4f23ma::f32 lat_rad = lat * (kPi / 180.f);
            entry.size_meters = kEquatorTileM * std::cos(lat_rad);
        } else {
            entry.size_meters = kStandardTileMeters;
        }
    } else if (entry.world_coordinates) {
        entry.size_meters = tile_size_from_grid_y(ref.grid_y);
    } else {
        entry.size_meters = kStandardTileMeters;
    }

    return entry;
}

std::pair<f4f23ma::f32, f4f23ma::f32> origin_from_metric(const TileCatalogEntry& metric,
                                                           const MapTileLayout& layout) {
    // Catalog origins are always filled in build_map_tile_layout (classic anchor-relative,
    // including 0,0 and negative west/south — do not gate on >0.001).
    if (metric.is_global || metric.world_coordinates || layout.origin_anchor_set ||
        metric.origin_x != 0.f || metric.origin_y != 0.f) {
        return {metric.origin_x, metric.origin_y};
    }
    return {
        static_cast<f4f23ma::f32>(metric.grid_x - layout.min_grid_x) * kStandardTileMeters,
        static_cast<f4f23ma::f32>(metric.grid_y - layout.min_grid_y) * kStandardTileMeters,
    };
}

} // namespace

bool is_global_coordinate_tile_name(const std::string& stem) {
    if (stem.rfind("tile_", 0) == 0) return false;
    if (stem.size() < 5) return false;
    return std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool parse_latitude_from_global_tile_name(const std::string& stem, f4f23ma::f32& latitude_deg) {
    if (!is_global_coordinate_tile_name(stem)) return false;
    try {
        const long code = std::stol(stem);
        if (code >= 100'000L) {
            latitude_deg = static_cast<f4f23ma::f32>(code) / 10'000.f;
            return true;
        }
        if (code >= 10'000L) {
            latitude_deg = static_cast<f4f23ma::f32>(code) / 1'000.f;
            return true;
        }
    } catch (...) {
    }
    return false;
}

const TileCatalogEntry* MapTileLayout::find_grid(const std::int32_t grid_x,
                                                const std::int32_t grid_y) const {
    const auto it = by_grid.find(tile_grid_key(grid_x, grid_y));
    if (it == by_grid.end()) return nullptr;
    return &it->second;
}

MapTileLayout build_map_tile_layout(const GlobalMap& global, const std::int32_t origin_anchor_gx,
                                    const std::int32_t origin_anchor_gy) {
    MapTileLayout layout{};
    layout.map_world_coordinates = global.world_coordinates;

    std::vector<TileCatalogEntry> metrics;
    metrics.reserve(global.tiles.size());
    for (const auto& ref : global.tiles) {
        if (!std::filesystem::exists(ref.tile_path)) continue;
        metrics.push_back(make_tile_metric(ref, global.world_coordinates));
    }
    if (metrics.empty()) return layout;

    layout.min_grid_x = std::numeric_limits<std::int32_t>::max();
    layout.min_grid_y = std::numeric_limits<std::int32_t>::max();
    for (const auto& m : metrics) {
        layout.min_grid_x = std::min(layout.min_grid_x, m.grid_x);
        layout.min_grid_y = std::min(layout.min_grid_y, m.grid_y);
    }

    layout.origin_anchor_gx = origin_anchor_gx;
    layout.origin_anchor_gy = origin_anchor_gy;
    layout.origin_anchor_set = true;

    for (auto& m : metrics) {
        if (m.size_meters < 0.01f) m.size_meters = kStandardTileMeters;
    }

    layout.catalog = std::move(metrics);
    for (const auto& m : layout.catalog) layout.by_grid[tile_grid_key(m.grid_x, m.grid_y)] = m;

    if (layout.map_world_coordinates) {
        constexpr std::int32_t kWcMargin = 8;
        const std::int32_t gx0 =
            (std::min)(layout.min_grid_x, layout.origin_anchor_gx) - kWcMargin;
        const std::int32_t gx1 =
            (std::max)(max_grid_x_in_layout(layout), layout.origin_anchor_gx) + kWcMargin;
        const std::int32_t gy0 =
            (std::min)(layout.min_grid_y, layout.origin_anchor_gy) - kWcMargin;
        const std::int32_t gy1 =
            (std::max)(max_grid_y_in_layout(layout), layout.origin_anchor_gy) + kWcMargin;
        layout.wc_gx_min = gx0;
        layout.wc_gy_min = gy0;
        layout.wc_nx = gx1 - gx0 + 1;
        layout.wc_ny = gy1 - gy0 + 1;
        if (layout.wc_nx > 0 && layout.wc_ny > 0) {
            layout.wc_row_origin_y.resize(static_cast<std::size_t>(layout.wc_ny));
            layout.wc_row_height.resize(static_cast<std::size_t>(layout.wc_ny));
            // Absolute Y = prefix sum of row heights (must match height=equator·cos(lat)).
            // wc_rect_lookup subtracts the anchor row origin.
            double y_acc = 0.0;
            for (std::int32_t iy = 0; iy < layout.wc_ny; ++iy) {
                const std::int32_t gy = gy0 + iy;
                layout.wc_row_origin_y[static_cast<std::size_t>(iy)] = static_cast<f4f23ma::f32>(y_acc);
                const f4f23ma::f32 h = row_height_at_grid_y(layout, gy);
                layout.wc_row_height[static_cast<std::size_t>(iy)] = h;
                y_acc += static_cast<double>(h);
            }
            const std::size_t cell_count =
                static_cast<std::size_t>(layout.wc_nx) * static_cast<std::size_t>(layout.wc_ny);
            layout.wc_col_origin_x.resize(cell_count);
            layout.wc_col_width.resize(cell_count);
            for (std::int32_t iy = 0; iy < layout.wc_ny; ++iy) {
                const std::int32_t gy = gy0 + iy;
                for (std::int32_t ix = 0; ix < layout.wc_nx; ++ix) {
                    const std::int32_t gx = gx0 + ix;
                    const std::size_t idx =
                        static_cast<std::size_t>(iy) * static_cast<std::size_t>(layout.wc_nx) +
                        static_cast<std::size_t>(ix);
                    const f4f23ma::f32 w = col_width_at_grid(layout, gx, gy);
                    layout.wc_col_width[idx] = w;
                    // Direct column pitch: gx·width(row) — no cumulative float sum.
                    layout.wc_col_origin_x[idx] = static_cast<f4f23ma::f32>(gx) * w;
                }
            }
        }
        for (auto& m : layout.catalog) {
            GridTileRect rect{};
            if (wc_rect_lookup(layout, m.grid_x, m.grid_y, rect)) {
                m.origin_x = rect.origin_x;
                m.origin_y = rect.origin_y;
                layout.by_grid[tile_grid_key(m.grid_x, m.grid_y)] = m;
            }
        }
    } else {
        // OMSI classic: 300×300; origin relative to session anchor (not moving min_grid).
        const std::int32_t ax = layout.origin_anchor_set ? layout.origin_anchor_gx : layout.min_grid_x;
        const std::int32_t ay = layout.origin_anchor_set ? layout.origin_anchor_gy : layout.min_grid_y;
        for (auto& m : layout.catalog) {
            m.size_meters = kStandardTileMeters;
            m.origin_x = static_cast<f4f23ma::f32>(m.grid_x - ax) * kStandardTileMeters;
            m.origin_y = static_cast<f4f23ma::f32>(m.grid_y - ay) * kStandardTileMeters;
            layout.by_grid[tile_grid_key(m.grid_x, m.grid_y)] = m;
        }
    }
    return layout;
}

MapTileLayout build_map_tile_layout(const GlobalMap& global) {
    MapTileLayout probe{};
    probe.map_world_coordinates = global.world_coordinates;
    std::int32_t min_x = std::numeric_limits<std::int32_t>::max();
    std::int32_t min_y = std::numeric_limits<std::int32_t>::max();
    bool any = false;
    for (const auto& ref : global.tiles) {
        if (!std::filesystem::exists(ref.tile_path)) continue;
        any = true;
        min_x = std::min(min_x, ref.grid_x);
        min_y = std::min(min_y, ref.grid_y);
    }
    if (!any) return {};
    return build_map_tile_layout(global, min_x, min_y);
}

std::pair<f4f23ma::f32, f4f23ma::f32> tile_origin_xy(const MapTileLayout& layout, const std::int32_t grid_x,
                                                     const std::int32_t grid_y) {
    if (const auto* metric = layout.find_grid(grid_x, grid_y)) return origin_from_metric(*metric, layout);
    if (layout.map_world_coordinates) {
        GridTileRect rect{};
        if (wc_rect_lookup(layout, grid_x, grid_y, rect)) return {rect.origin_x, rect.origin_y};
        return {tile_origin_x_bidir(layout, grid_x, grid_y), tile_origin_y_bidir(layout, grid_y)};
    }
    const std::int32_t ax = layout.origin_anchor_set ? layout.origin_anchor_gx : layout.min_grid_x;
    const std::int32_t ay = layout.origin_anchor_set ? layout.origin_anchor_gy : layout.min_grid_y;
    return {
        static_cast<f4f23ma::f32>(grid_x - ax) * kStandardTileMeters,
        static_cast<f4f23ma::f32>(grid_y - ay) * kStandardTileMeters,
    };
}

bool uses_absolute_tile_file_coords(const GlobalMap& global) {
    return !global.world_coordinates && !std::filesystem::exists(global.root / "tile_0_0.map");
}

bool uses_classic_grid_tile_origins(const GlobalMap& global) {
    return !global.world_coordinates && !uses_absolute_tile_file_coords(global);
}

const TileCatalogEntry* find_tile_at_world_position(const MapTileLayout& layout, const f4f23ma::f32 world_x,
                                                    const f4f23ma::f32 world_y) {
    for (const auto& metric : layout.catalog) {
        const auto origin = origin_from_metric(metric, layout);
        if (world_x >= origin.first && world_x < origin.first + metric.size_meters && world_y >= origin.second &&
            world_y < origin.second + metric.size_meters)
            return &metric;
    }
    return nullptr;
}

f4f23ma::f32 latitude_deg_from_grid_y(const std::int32_t grid_y) {
    return latitude_from_grid_y(grid_y);
}

f4f23ma::f32 tile_size_meters_from_grid_y(const std::int32_t grid_y) {
    return tile_size_from_grid_y(grid_y);
}

f4f23ma::f32 tile_size_meters_at_grid(const GlobalMap& global, const MapTileLayout& layout,
                                      const std::int32_t grid_x, const std::int32_t grid_y) {
    if (const auto* metric = layout.find_grid(grid_x, grid_y)) return metric->size_meters;
    if (global.world_coordinates) return tile_size_from_grid_y(grid_y);
    return kStandardTileMeters;
}

GridTileRect grid_tile_rect_at(const GlobalMap& global, const MapTileLayout& layout, const std::int32_t grid_x,
                               const std::int32_t grid_y) {
    GridTileRect out{};
    out.grid_x = grid_x;
    out.grid_y = grid_y;
    // WC: prefer prefix table for every cell (catalog + empty) so wireframe/terrain edges align.
    if (global.world_coordinates || layout.map_world_coordinates) {
        if (wc_rect_lookup(layout, grid_x, grid_y, out)) return out;
    }
    if (const auto* metric = layout.find_grid(grid_x, grid_y)) {
        const auto origin = origin_from_metric(*metric, layout);
        out.origin_x = origin.first;
        out.origin_y = origin.second;
        out.width_m = metric->size_meters;
        out.height_m = metric->size_meters;
        out.resolved = true;
        return out;
    }
    if (global.world_coordinates || layout.map_world_coordinates) {
        if (layout.wc_nx > 0) {
            const char* dbg_env = std::getenv("OMSI_TILE_DEBUG");
            if (dbg_env && (dbg_env[0] == '1' || dbg_env[0] == 'y' || dbg_env[0] == 'Y')) {
                log::warn("TILE_DEBUG grid_tile_rect_at WC prefix MISS gx=" + std::to_string(grid_x) +
                          " gy=" + std::to_string(grid_y) + " -> slow path");
            }
        }
        out.origin_x = tile_origin_x_bidir(layout, grid_x, grid_y);
        out.origin_y = tile_origin_y_bidir(layout, grid_y);
        out.width_m = col_width_at_grid(layout, grid_x, grid_y);
        out.height_m = row_height_at_grid_y(layout, grid_y);
        out.resolved = true;
        return out;
    }
    const auto origin = tile_origin_xy(layout, grid_x, grid_y);
    out.origin_x = origin.first;
    out.origin_y = origin.second;
    out.width_m = kStandardTileMeters;
    out.height_m = kStandardTileMeters;
    out.resolved = true;
    return out;
}

ActiveTileAtWorld grid_cell_at_world_xy(const GlobalMap& global, const MapTileLayout& layout,
                                        const f4f23ma::f32 world_x, const f4f23ma::f32 world_y) {
    ActiveTileAtWorld out{};

    if (const auto* metric = find_tile_at_world_position(layout, world_x, world_y)) {
        const auto origin = origin_from_metric(*metric, layout);
        out.grid_x = metric->grid_x;
        out.grid_y = metric->grid_y;
        out.size_meters = metric->size_meters;
        out.origin_x = origin.first;
        out.origin_y = origin.second;
        out.latitude_deg = latitude_from_grid_y(metric->grid_y);
        out.in_map = true;
        out.resolved = true;
        return out;
    }

    if ((global.world_coordinates || layout.map_world_coordinates) && !layout.catalog.empty() &&
        layout.wc_nx > 0 && layout.wc_ny > 0) {
        GridTileRect rect{};
        if (wc_cell_from_world(layout, world_x, world_y, rect)) return active_tile_from_rect(layout, rect);
        const char* dbg_env = std::getenv("OMSI_TILE_DEBUG");
        if (dbg_env && (dbg_env[0] == '1' || dbg_env[0] == 'y' || dbg_env[0] == 'Y')) {
            log::warn("TILE_DEBUG grid_cell_at_world_xy WC prefix scan miss wx=" + std::to_string(world_x) +
                      " wy=" + std::to_string(world_y));
        }
    } else if (!layout.catalog.empty()) {
        // OMSI classic: size = 300 (float 0x43960000). World origin of min_grid is (0,0).
        // posVec of kachel (gx,gy) relative to center = ((gx-cx)*300, (gy-cy)*300).
        // Absolute world origin of (gx,gy) = ((gx-min_x)*300, (gy-min_y)*300).
        const f4f23ma::f32 tile_m = kStandardTileMeters;
        const std::int32_t ax = layout.origin_anchor_set ? layout.origin_anchor_gx : layout.min_grid_x;
        const std::int32_t ay = layout.origin_anchor_set ? layout.origin_anchor_gy : layout.min_grid_y;
        const std::int32_t gx = ax + static_cast<std::int32_t>(std::floor(world_x / tile_m));
        const std::int32_t gy = ay + static_cast<std::int32_t>(std::floor(world_y / tile_m));
        out.grid_x = gx;
        out.grid_y = gy;
        out.size_meters = tile_m;
        out.origin_x = static_cast<f4f23ma::f32>(gx - ax) * tile_m;
        out.origin_y = static_cast<f4f23ma::f32>(gy - ay) * tile_m;
        out.latitude_deg = latitude_from_grid_y(gy);
        out.in_map = layout.find_grid(gx, gy) != nullptr;
        out.resolved = true;
        return out;
    }

    return out;
}

ActiveTileAtWorld active_tile_at_world(const GlobalMap& global, const MapTileLayout& layout,
                                       const f4f23ma::f32 world_x, const f4f23ma::f32 world_y) {
    return grid_cell_at_world_xy(global, layout, world_x, world_y);
}

f4f23ma::f4 resolve_entry_world_position(const GlobalMap& global, const MapTileLayout& layout,
                                         const EntryPoint& ep) {
    // position_local is Z-up tile-local (x, ground_y, height) after omsi_map_loader remap.
    // category indexes [map] in global.cfg order (classic + [worldcoordinates]) — same as tt-viewer.
    if (ep.category >= 0 && static_cast<std::size_t>(ep.category) < global.tiles.size()) {
        const auto& ref = global.tiles[static_cast<std::size_t>(ep.category)];
        const auto origin = tile_origin_xy(layout, ref.grid_x, ref.grid_y);
        return {
            origin.first + ep.position_local.x,
            origin.second + ep.position_local.y,
            ep.position_local.z,
            1.f,
        };
    }
    return ep.position_local;
}

void resolve_entry_world_positions(GlobalMap& global) {
    const auto layout = build_map_tile_layout(global);
    for (auto& ep : global.entrypoints) ep.position = resolve_entry_world_position(global, layout, ep);
}

} // namespace omsi::map
