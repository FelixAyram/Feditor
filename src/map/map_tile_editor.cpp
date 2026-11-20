#include "omsi/vfs_overlay.hpp"
#include "omsi/fs_cow.hpp"
#include "omsi/map_tile_editor.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

#include "editor/editor_ground.hpp"
#include "omsi/cfg_document.hpp"
#include "omsi/editor_map_workspace.hpp"
#include "omsi/editor_omsi_tools.hpp"
#include "omsi/logger.hpp"
#include "omsi/map_geo_tools.hpp"

namespace omsi::map {
namespace {

bool map_file_is_utf16_le(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return in.gcount() == 2 && b[0] == 0xFF && b[1] == 0xFE;
}

bool global_cfg_prefers_utf16(const std::filesystem::path& map_dir) {
    if (map_dir.empty()) return true;
    const auto global_cfg = map_dir / "global.cfg";
    if (std::filesystem::exists(global_cfg) && map_file_is_utf16_le(global_cfg)) return true;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(map_dir, ec)) {
        if (ec || !e.is_regular_file()) continue;
        const auto fn = e.path().filename().string();
        if (fn.rfind("tile_", 0) != 0) continue;
        if (e.path().extension() != ".map") continue;
        if (map_file_is_utf16_le(e.path())) return true;
    }
    return false;
}

bool is_section_header(const std::string& line, std::string& out_tag) {
    std::string t = line;
    while (!t.empty() && (t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
    std::size_t i = 0;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    if (i >= t.size() || t[i] != '[') return false;
    const auto end = t.find(']', i);
    if (end == std::string::npos) return false;
    out_tag = t.substr(i, end - i + 1);
    return true;
}

std::vector<std::string> normalize_omsi_new_tile_map_lines(std::vector<std::string> lines) {
    auto has_section = [&](const char* tag) {
        std::string t;
        for (const auto& line : lines) {
            if (is_section_header(line, t) && t == tag) return true;
        }
        return false;
    };
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (!has_section("[variable_terrainlightmap]")) {
        if (!lines.empty()) lines.emplace_back();
        lines.emplace_back("[variable_terrainlightmap]");
        lines.emplace_back();
    }
    if (!has_section("[variable_terrain]")) {
        lines.emplace_back("[variable_terrain]");
        lines.emplace_back();
    }
    return lines;
}

bool write_text_lines_utf16_aware(const std::filesystem::path& path, const std::vector<std::string>& lines,
                                    bool utf16_le) {
    if (omsi::vfs::write_blocked(path)) return false;
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (utf16_le) {
        const unsigned char bom[2] = {0xFF, 0xFE};
        f.write(reinterpret_cast<const char*>(bom), 2);
        for (const auto& line : lines) {
            for (unsigned char c : line) {
                const wchar_t wc = static_cast<wchar_t>(c);
                f.write(reinterpret_cast<const char*>(&wc), sizeof(wchar_t));
            }
            const wchar_t nl = L'\n';
            f.write(reinterpret_cast<const char*>(&nl), sizeof(wchar_t));
        }
        return f.good();
    }
    for (const auto& line : lines) f << line << '\n';
    return f.good();
}

bool finalize_new_tile_map_file(const std::filesystem::path& tile_path, const std::filesystem::path& map_dir) {
    if (!std::filesystem::exists(tile_path)) return false;
    const bool utf16 = map_file_is_utf16_le(tile_path) || global_cfg_prefers_utf16(map_dir);
    auto lines = normalize_omsi_new_tile_map_lines(cfg::read_cfg_lines(tile_path));
    if (lines.empty()) return false;
    if (!write_text_lines_utf16_aware(tile_path, lines, utf16)) return false;
    if (utf16) log::info("tile .map UTF-16 LE: " + tile_path.filename().string());
    return true;
}

} // namespace

std::int32_t get_kachel_index_with_coord(const GlobalMap& global, const std::int32_t grid_x,
                                         const std::int32_t grid_y) {
    for (std::size_t i = 0; i < global.tiles.size(); ++i) {
        const auto& t = global.tiles[i];
        if (t.grid_x == grid_x && t.grid_y == grid_y) return static_cast<std::int32_t>(i);
    }
    return -1;
}

CenterKachel centerkachel_from_world(const GlobalMap& global, const MapTileLayout& layout, const float world_x,
                                     const float world_y) {
    CenterKachel ck{};
    const auto hit = grid_cell_at_world_xy(global, layout, world_x, world_y);
    if (!hit.resolved) return ck;
    ck.gx = hit.grid_x;
    ck.gy = hit.grid_y;
    ck.valid = true;
    return ck;
}

bool create_new_tile(GlobalMap& global, const std::int32_t grid_x, const std::int32_t grid_y,
                     const std::filesystem::path& omsi_root, const float flat_height) {
    // TMap_GetKachelIndexWithCoord == -1 (Button5Click @ 0x0083301C).
    if (get_kachel_index_with_coord(global, grid_x, grid_y) >= 0) {
        log::warn("create_new_tile: already exists (" + std::to_string(grid_x) + "," + std::to_string(grid_y) + ")");
        return false;
    }
    if (global.root.empty() || global.global_cfg_path.empty()) return false;
    if (omsi::vfs::write_blocked(global.global_cfg_path)) return false;

    const auto write_root = global.global_cfg_path.parent_path().empty() ? global.root
                                                                        : global.global_cfg_path.parent_path();
    const std::string fname = "tile_" + std::to_string(grid_x) + "_" + std::to_string(grid_y) + ".map";
    const auto tile_path = write_root / fname;
    std::error_code exists_ec;
    if (std::filesystem::exists(tile_path, exists_ec)) {
        log::warn("create_new_tile: file already on disk " + tile_path.string());
        return false;
    }
    const auto template_map = omsi_root / "template" / "map" / "newmap.map";
    const auto template_lm = omsi_root / "template" / "map" / "newmap.map.LM.bmp";
    std::error_code ec;
    std::filesystem::create_directories(write_root, ec);
    const bool cfg_utf16 =
        map_file_is_utf16_le(global.global_cfg_path) || global_cfg_prefers_utf16(write_root);
    const bool template_utf16 =
        std::filesystem::exists(template_map, ec) && map_file_is_utf16_le(template_map);
    const bool map_utf16 = template_utf16 || cfg_utf16;

    // TMap_CreateNewTile @ 0x00790F54: copy template\newmap.map + .LM.bmp, texture/
    if (!omsi_root.empty() && std::filesystem::exists(template_map, ec)) {
        omsi::fsutil::break_link(tile_path); // copy-on-write workspace: never write through a link
        std::filesystem::copy_file(template_map, tile_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            log::warn("create_new_tile: template copy failed");
            return false;
        }
        const auto lm_dest = write_root / (fname + ".LM.bmp");
        if (std::filesystem::exists(template_lm, ec)) {
            omsi::fsutil::break_link(lm_dest); // copy-on-write workspace: never write through a link
            std::filesystem::copy_file(template_lm, lm_dest, std::filesystem::copy_options::overwrite_existing, ec);
        }
        std::filesystem::create_directories(write_root / "texture", ec);
        if (!finalize_new_tile_map_file(tile_path, write_root)) {
            log::warn("create_new_tile: finalize .map failed");
            return false;
        }
    } else {
        const std::vector<std::string> minimal_lines = {"File created with OMSI-F4-Editor",
                                                        "",
                                                        "[version]",
                                                        "14",
                                                        "",
                                                        "[terrain]",
                                                        "",
                                                        "[variable_terrainlightmap]",
                                                        "",
                                                        "[variable_terrain]",
                                                        ""};
        if (!write_text_lines_utf16_aware(tile_path, minimal_lines, map_utf16)) return false;
    }

    // F4 extension: .terrain + lightmap for editor preview (OMSI defers terrain).
    const auto heights = editor::make_flat_terrain_heights(flat_height);
    if (!write_terrain_heights(tile_path, heights)) {
        log::warn("create_new_tile: terrain write failed");
        return false;
    }
    if (!write_tile_lightmap_bmp(tile_path, heights)) {
        log::warn("create_new_tile: lightmap write failed");
        return false;
    }

    // Append [map] block (TMap.save / KachelInfos).
    {
        auto lines = cfg::read_cfg_lines(global.global_cfg_path);
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back("[map]");
        lines.push_back(std::to_string(grid_x));
        lines.push_back(std::to_string(grid_y));
        lines.push_back(fname);
        if (!write_text_lines_utf16_aware(global.global_cfg_path, lines, cfg_utf16)) return false;
    }

    MapTileRef ref{};
    ref.grid_x = grid_x;
    ref.grid_y = grid_y;
    ref.tile_path = tile_path;
    global.tiles.push_back(std::move(ref));
    log::info("create_new_tile " + fname + " write=" + tile_path.string());
    return true;
}

} // namespace omsi::map
