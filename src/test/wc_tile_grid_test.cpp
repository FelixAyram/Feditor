// WC grid: adjacent tile rects must share edges (no stair-step along X/Y).

#include "omsi/map_tile_layout.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << '\n';
    return 1;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "omsi_wc_tile_grid_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    auto write_stub = [&](const char* name) {
        std::ofstream f(root / name);
        f << "[version]\n1\n";
    };
    write_stub("tile_100_11283.map");
    write_stub("tile_101_11283.map");
    write_stub("tile_100_11284.map");

    omsi::map::GlobalMap g{};
    g.world_coordinates = true;
    g.root = root;
    g.tiles.push_back({100, 11283, root / "tile_100_11283.map"});
    g.tiles.push_back({101, 11283, root / "tile_101_11283.map"});
    g.tiles.push_back({100, 11284, root / "tile_100_11284.map"});

    const auto layout = omsi::map::build_map_tile_layout(g, 100, 11283);
    const auto r00 = omsi::map::grid_tile_rect_at(g, layout, 100, 11283);
    const auto r10 = omsi::map::grid_tile_rect_at(g, layout, 101, 11283);
    const auto r01 = omsi::map::grid_tile_rect_at(g, layout, 100, 11284);
    if (!r00.resolved || !r10.resolved || !r01.resolved) return fail("rect not resolved");

    const float dx = std::abs(r10.origin_x - (r00.origin_x + r00.width_m));
    if (dx > 0.05f)
        return fail("X seam gap " + std::to_string(dx) + " m between gx 100 and 101");

    const float dy = std::abs(r01.origin_y - (r00.origin_y + r00.height_m));
    if (dy > 0.05f)
        return fail("Y seam gap " + std::to_string(dy) + " m between gy 11283 and 11284");

    std::cout << "OK wc_tile_grid_test\n";
    return 0;
}
