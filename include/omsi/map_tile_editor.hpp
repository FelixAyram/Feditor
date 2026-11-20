#pragma once

#include <cstdint>
#include <filesystem>

#include "omsi/map_tile_layout.hpp"
#include "omsi/map_types.hpp"

namespace omsi::map {

// TMap+0x144 centerkachel — grid cell under view center / RMB target.
struct CenterKachel {
    std::int32_t gx = 0;
    std::int32_t gy = 0;
    bool valid = false;
};

// TMap_GetKachelIndexWithCoord @ 0x00783AC8 (_Unit126.c)
[[nodiscard]] std::int32_t get_kachel_index_with_coord(const GlobalMap& global, std::int32_t grid_x,
                                                       std::int32_t grid_y);

// World position → grid cell (delegates grid_cell_at_world_xy).
[[nodiscard]] CenterKachel centerkachel_from_world(const GlobalMap& global, const MapTileLayout& layout,
                                                 float world_x, float world_y);

// TMap_CreateNewTile @ 0x00790F54 + TMap.save (Button5Click @ 0x0083301C).
[[nodiscard]] bool create_new_tile(GlobalMap& global, std::int32_t grid_x, std::int32_t grid_y,
                                   const std::filesystem::path& omsi_root, float flat_height = 0.f);

} // namespace omsi::map
