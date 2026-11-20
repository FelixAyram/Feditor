#pragma once

#include "omsi/map_geo_tools.hpp"

#include <string>
#include <vector>

namespace omsi::map {

// Sample a Float32 tiled GeoTIFF/COG (Deflate + Predictor 2/3) over WGS84 bounds.
// `url_or_path`: https://… COG (HTTP Range) or local .tif path.
[[nodiscard]] GeoJobResult sample_geotiff_float32_grid(const std::string& url_or_path,
                                                       const TileWgs84Bounds& bounds, int dim,
                                                       std::vector<float>& out_heights,
                                                       const char* label = "GeoTIFF");

} // namespace omsi::map
