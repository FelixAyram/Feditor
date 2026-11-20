#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "omsi/map_types.hpp"

namespace omsi::editor {

// Hard limit from units_c/mc_o3dfiles.c: DynArrayLength must be <= 0xFFFF.
inline constexpr std::uint32_t kO3dMaxVertices = 65535;

// N parts for tile grid: only 1..5.
inline constexpr int kTileBakePartsMin = 1;
inline constexpr int kTileBakePartsMax = 5;

struct TileBakePartResult {
    int part_index = 0; // 0-based
    std::filesystem::path sco_path;
    std::filesystem::path o3d_path;
    float cx = 0.f, cy = 0.f, cz = 0.f;
    int object_count = 0;
    int spline_count = 0;
    int vertex_count = 0;
    int sub_part = 0; // >0 if auto-split by vertex limit
    std::string error;
};

struct TileBakeProgress {
    std::atomic<int> percent{0};
    std::atomic<int> parts_done{0};
    std::atomic<int> parts_total{0};
    std::mutex log_mutex;
    std::vector<std::string> log_lines;
    std::atomic<bool> finished{false};
    std::atomic<bool> success{false};
    std::string status; // guarded by log_mutex when writing long strings
};

struct TileBakeRequest {
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    int parts = 1; // 1..5
    float tile_origin_x = 0.f; // world/editor origin of tile
    float tile_origin_y = 0.f;
    float tile_size = 1000.f;  // meters (typical OMSI tile)
    std::filesystem::path omsi_root;
    std::filesystem::path map_dir;
    std::filesystem::path out_parent; // Sceneryobjects/… parent
    std::string stem;                 // tile_bake_X_Y
    // Snapshot of instances that belong to this tile (caller filters).
    std::vector<map::MapObjectInst> objects;
    std::vector<map::SplineInst> splines;
    std::vector<map::MapAttachObjectInst> attaches;
    std::vector<map::MapSplineAttachmentInst> spline_attaches;
    // 61x61 heightfield for focus tile (empty = flat 0). Same layout as .terrain_0 / LocalTerrainTile.
    std::vector<float> terrain_heights;
};

struct TileBakeResult {
    bool ok = false;
    std::string error;
    std::vector<TileBakePartResult> parts;
    std::filesystem::path out_dir;
};

// Detect SCO / folder names already produced by a previous tile bake.
[[nodiscard]] bool is_tile_bake_product_name(const std::string& name);

// Grid layout for N in 1..5 (MergeMapTrees-style).
struct TileGridCell {
    int ix = 0, iy = 0;
    float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
};
[[nodiscard]] std::vector<TileGridCell> tile_bake_grid(int parts, float origin_x, float origin_y,
                                                      float tile_size);

// Synchronous bake (CPU parallel inside). Safe to call from a worker thread.
[[nodiscard]] TileBakeResult bake_tile(const TileBakeRequest& req, TileBakeProgress* progress = nullptr);

} // namespace omsi::editor
