#pragma once

#include "omsi/map_types.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace omsi::launcher {

struct LaunchSelection {
    std::filesystem::path global_cfg;
    std::filesystem::path omsi_root;
    std::string map_friendly_name;
    std::size_t entrypoint_index = 0;
    bool confirmed = false;
    // true = load map read-only (no robocopy *_actual workspace, no mutations).
    bool view_only = false;
    // User pressed "BeamNG.drive" at the top: open the BeamNG launcher instead (confirmed stays false).
    bool switch_to_beamng = false;
    // "Gráficos" combo: 0 auto, 1 d3d12, 2 vulkan, 3 warp (gfx::backend::Kind order). Saved to gfx_backend.txt.
    int gfx_backend = 0;
};

// BeamNG.drive launcher (install / user folder / level / spawn / load radius).
struct BeamngLaunchSelection {
    std::filesystem::path install;
    std::filesystem::path user_folder;
    std::string level;          // folder name
    std::string spawn;          // SpawnSphere name ("" = level default)
    float load_radius = 1000.f;
    bool view_only = false;
    bool confirmed = false;
    bool switch_to_omsi = false;
};
[[nodiscard]] std::optional<BeamngLaunchSelection> show_beamng_launcher(HWND owner = nullptr);

// Folder picker for OMSI 2 root. Cancel → nullopt.
[[nodiscard]] std::optional<std::filesystem::path> browse_omsi_root(HWND owner = nullptr);

// Resolve OMSI root: saved prefs → hint → forced browse until valid (or empty if user aborts app path).
[[nodiscard]] std::filesystem::path ensure_omsi_root(const std::filesystem::path& hint = {},
                                                    HWND owner = nullptr);

// Map + entrypoint picker. Always shows a scan progress dialog first.
// owner: optional parent (editor HWND) for modal behaviour.
[[nodiscard]] std::optional<LaunchSelection> show_launcher(
    const std::filesystem::path& omsi_root, const std::filesystem::path& default_map_cfg = {},
    std::size_t default_entrypoint_index = 0, HWND owner = nullptr);

// Ask which entrypoint to spawn at (map already chosen). Cancel → nullopt.
[[nodiscard]] std::optional<std::size_t> pick_entrypoint(const map::GlobalMap& map,
                                                         std::size_t default_index = 0);

// Logs all maps + entrypoints to omsi_f4_engine.log and returns count.
int dump_maps_catalog(const std::filesystem::path& omsi_root);

} // namespace omsi::launcher
