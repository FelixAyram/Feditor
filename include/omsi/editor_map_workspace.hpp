#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "omsi/map_types.hpp"

namespace omsi::map {

struct EditorMapWorkspace {
    std::filesystem::path original_dir; // frozen snapshot (*_original) — never write
    std::filesystem::path actual_dir;   // full edit copy (*_actual) — all editor reads/writes
    std::filesystem::path actual_global_cfg;
};

// Progress while copying *_original / *_actual (fraction 0..1).
using WorkspaceProgressFn = std::function<void(float fraction, const std::string& status_utf8)>;

// Redirects maps/Foo → maps/Foo_actual via robocopy. Foo_original is a frozen snapshot.
[[nodiscard]] std::optional<EditorMapWorkspace> ensure_editor_map_workspace(
    const std::filesystem::path& global_cfg, WorkspaceProgressFn progress = {});

// Prefer overlay/actual global.cfg if present, else original snapshot.
[[nodiscard]] std::filesystem::path workspace_global_cfg_for_read(const EditorMapWorkspace& ws);

// Ensure actual/global.cfg exists (copy from original if needed) before mutating name/EP/[map].
[[nodiscard]] bool workspace_prepare_global_cfg_for_write(EditorMapWorkspace& ws);

// Map a path under the map to the *_actual write path (creates parent dirs as needed).
// Returns empty if path is not under original/actual (refuses alien paths).
[[nodiscard]] std::filesystem::path workspace_overlay_write_path(const EditorMapWorkspace& ws,
                                                                const std::filesystem::path& map_file);

// Live OMSI map folder (maps/Foo), stripping *_actual / *_original from map_dir.
[[nodiscard]] std::filesystem::path live_map_dir_from_folder(const std::filesystem::path& map_dir);
[[nodiscard]] std::filesystem::path workspace_live_map_dir(const EditorMapWorkspace& ws);
// Corresponding .map under maps/Foo for a tile path in actual/original/live.
[[nodiscard]] std::filesystem::path workspace_live_tile_path(const EditorMapWorkspace& ws,
                                                            const std::filesystem::path& tile_path);
// Copy existing file to "<name>.YYYYMMDD-HHMMSS.bak" beside it. True if missing or copy ok.
[[nodiscard]] bool backup_file_timestamped_bak(const std::filesystem::path& src);

// Ensure tile .map (and parent dirs) exist under *_actual for writes; COW from original if needed.
// Always returns a path under actual_dir when workspace is active.
[[nodiscard]] std::filesystem::path workspace_ensure_tile_write_path(const EditorMapWorkspace& ws,
                                                                    const std::filesystem::path& logical_tile_path);

// .map to parse: under *_actual if that file exists, else logical path.
[[nodiscard]] std::filesystem::path workspace_tile_map_for_read(const EditorMapWorkspace* ws,
                                                               const std::filesystem::path& logical_tile_path);

// Keep _actual and set [name]/[friendlyname] to "<base> - edit".
[[nodiscard]] bool commit_editor_workspace(EditorMapWorkspace& ws);

// Delete the edit copy (_actual). Next open re-copies from original/source.
[[nodiscard]] bool discard_editor_workspace(const EditorMapWorkspace& ws);

// Restore objects_from_file_order after entity vectors were rebuilt (match Object/Attach/SA by id).
void remap_objects_from_file_order(MapTileData& tile, const MapTileData& before_sync);

// Rewrite [name] / [friendlyname] in global.cfg (preserves UTF-16 LE when present).
[[nodiscard]] bool set_global_map_names(const std::filesystem::path& global_cfg, const std::string& name,
                                        const std::string& friendly);

// Re-encode global.cfg as UTF-16 LE when sibling tiles use OMSI wide-char format (fixes version parse / old-map dialog).
[[nodiscard]] bool ensure_global_cfg_utf16(const std::filesystem::path& map_dir);

// Rewrite [object]/[spline]/[spline_h] of a tile .map from in-memory tile data.
// Preserves other sections from read_fallback (or write path if present).
[[nodiscard]] bool write_tile_map_file(const std::filesystem::path& tile_map_path,
                                       const MapTileData& tile,
                                       const std::filesystem::path& omsi_root,
                                       const std::filesystem::path& read_fallback = {});

// Find 1-based line of [object]/[attachObj]/[splineAttachement]/[spline(_h)] header for instance id.
[[nodiscard]] int find_map_block_line(const std::filesystem::path& map_path, std::int32_t id, bool want_spline);

// Recursive *.sco / *.sli scan under folder (case-insensitive extension).
// 0 = no cap (preferred). Legacy callers may pass a positive limit.
inline constexpr std::size_t k_catalog_scan_max_files = 0;

struct CatalogScanResult {
    std::vector<std::filesystem::path> paths;
    bool truncated = false;
    std::size_t entries_walked = 0;
    double walk_ms = 0.0;
};

[[nodiscard]] CatalogScanResult scan_catalog_files(const std::filesystem::path& root, const char* ext,
                                                   std::size_t max_files = k_catalog_scan_max_files);

// Remove tile from global.cfg [map] and delete .map/.terrain (keeps backup via workspace).
[[nodiscard]] bool delete_map_tile(GlobalMap& global, std::int32_t grid_x, std::int32_t grid_y);

} // namespace omsi::map
