#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include "o3d/o3d_loader.hpp"
#include "omsi/map_types.hpp"
#include "omsi/map_attachment.hpp"
#include "omsi/map_tile_layout.hpp"
#include "omsi/sco_types.hpp"

namespace omsi::map {
struct EditorMapWorkspace;
}

namespace omsi::world {

struct PlacedMesh {
    std::shared_ptr<o3d::MappedO3D> o3d;
    f4f23ma::ma4 transform = f4f23ma::ma4::identity();
    std::filesystem::path source_path;
    std::filesystem::path sco_path;
    std::int32_t object_id = 0;
    std::int32_t tile_grid_x = 0;
    std::int32_t tile_grid_y = 0;
};

struct LoadOptions {
    std::filesystem::path omsi_root;
    std::int32_t tile_radius = 1;
    f4f23ma::f32 max_object_dist = 265.f;
    // 0 = Chebyshev tile_radius (editor default). >0 = AABB metres window (game/HW path).
    f4f23ma::f32 load_radius_m = 0.f;
    f4f23ma::f32 unload_radius_m = 0.f;
    // Extra AABB metres so a tile whose spline *ends* near the camera is still parsed
    // (origin can sit on the far side of the kachel). 0 = start-tile AABB only.
    f4f23ma::f32 spline_reach_m = 250.f;
    bool load_splines = false;
    bool load_terrain = false;
    // Editor workspace (*_actual): prefer .map under actual when present.
    const map::EditorMapWorkspace* workspace = nullptr;
};

// Mirrors TMap.loadGlobalFile → tile load → [object]/[spline] dispatch.
class World {
public:
    [[nodiscard]] bool load_map(const map::GlobalMap& global, const map::EntryPoint& entry,
                              const LoadOptions& opts);

    // Incremental load (used by AsyncWorldLoader).
    void begin_load(const map::GlobalMap& global, const map::EntryPoint& entry, const LoadOptions& opts);
    void load_tile_incremental(const map::MapTileRef& ref, const LoadOptions& opts);
    // Parse already done (async tile stream). Places objects / attachments for this tile.
    void load_parsed_tile(map::MapTileData tile, const LoadOptions& opts, bool warm_cpu = true);
    void finish_load();
    [[nodiscard]] const std::vector<PlacedMesh>& meshes() const { return meshes_; }
    [[nodiscard]] const map::EntryPoint& spawn() const { return spawn_; }
    [[nodiscard]] const std::vector<map::MapTileData>& tiles() const { return tiles_; }
    [[nodiscard]] std::vector<map::MapTileData>& tiles_mut() { return tiles_; }
    [[nodiscard]] const map::MapTileLayout& tile_layout() const { return tile_layout_; }
    void refresh_tile_layout(const map::GlobalMap& global);

    // Streaming: keep already-loaded tiles, drop out-of-range, merge newly loaded.
    [[nodiscard]] bool has_tile(std::int32_t grid_x, std::int32_t grid_y) const;
    void set_spawn(const map::EntryPoint& entry) { spawn_ = entry; }
    void unload_tiles(const std::vector<std::pair<std::int32_t, std::int32_t>>& grids);
    void merge_from(World&& other);

    void register_script_asset(const std::filesystem::path& path);

private:
    void load_tile(const map::MapTileRef& ref, const LoadOptions& opts);
    void place_object(const map::MapObjectInst& obj, const LoadOptions& opts, f4f23ma::f32 tile_offset_x,
                      f4f23ma::f32 tile_offset_y, std::int32_t tile_grid_x, std::int32_t tile_grid_y);
    void place_object_transform(const map::MapObjectInst& obj, const LoadOptions& opts, f4f23ma::ma4 world_xform,
                                f4f23ma::f32 tile_offset_x, f4f23ma::f32 tile_offset_y,
                                std::int32_t tile_grid_x, std::int32_t tile_grid_y);
    void place_tile_attachments(const map::MapTileData& tile, const LoadOptions& opts, f4f23ma::f32 tile_ox,
                                f4f23ma::f32 tile_oy);

    map::GlobalMap global_{};
    std::vector<map::GlobalSplineIndexEntry> global_spline_index_{};
    map::EntryPoint spawn_{};
    f4f23ma::f32 max_object_dist_ = 265.f;
    bool absolute_tile_coords_ = false;
    bool classic_grid_tile_origins_ = false;
    map::MapTileLayout tile_layout_{};
    std::vector<map::MapTileData> tiles_;
    std::vector<PlacedMesh> meshes_;
    std::unordered_map<std::filesystem::path, std::shared_ptr<o3d::MappedO3D>> o3d_cache_;
    std::vector<std::filesystem::path> script_assets_;
};

f4f23ma::ma4 euler_zyx_deg_to_matrix(const f4f23ma::f4& deg);
f4f23ma::ma4 tile_offset_matrix(std::int32_t grid_x, std::int32_t grid_y);
f4f23ma::ma4 tile_offset_matrix(f4f23ma::f32 origin_x, f4f23ma::f32 origin_y);
} // namespace omsi::world
