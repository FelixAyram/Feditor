#pragma once

#include "omsi/map_types.hpp"

#include <filesystem>
#include <functional>
#include <vector>

namespace omsi::world_height {

// OMSI TMapKachel_GetHeightAboveAll: DTM + spline [heightprofile] + SCO [surface]/collision.
// Highest surface at (ex,ey) with z <= z_hint (downward ray; ignore roofs/overpasses above).
using TerrainSample = std::function<float(float ex, float ey)>;

[[nodiscard]] float get_height_above_all(float ex, float ey, float z_hint, const TerrainSample& terrain,
                                         const std::vector<map::SplineInst>& splines,
                                         const std::vector<map::MapObjectInst>& objects,
                                         const std::filesystem::path& omsi_root);

} // namespace omsi::world_height
