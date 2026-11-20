#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>
#include <optional>
#include <string>

#include "f4f23ma/math.hpp"

namespace omsi::options {

// Mirrors OMSI options.cfg sections used at map load (see decompiled TOptions.load).
struct OmsiOptions {
    std::filesystem::path omsi_root;
    std::filesystem::path last_map_cfg;
    std::size_t last_entrypoint_index = 0;
    std::string language;

    f4f23ma::f32 tile_dist_max = 1.f;      // [performance_tiledistmax]
    f4f23ma::f32 max_object_dist = 265.f;  // [performance_maxObjDist]
    f4f23ma::f32 dyn_tile_red = 20.f;      // [performance_dyn_tile_red]
    bool vsync = true;
    // options.cfg [texmax256] — OMSI TTextureMan.LoadTexture (gvar texmax256, not user bus).
    bool tex_max_256 = false;
};

[[nodiscard]] OmsiOptions load_options(const std::filesystem::path& omsi_root);
[[nodiscard]] std::optional<std::filesystem::path> resolve_last_map(const OmsiOptions& opts);

// Persist last map + entrypoint for the next editor launch ({omsi_root}/.omsi_f4_engine.cfg).
void save_editor_session(const std::filesystem::path& omsi_root, const std::filesystem::path& global_cfg,
                         std::size_t entrypoint_index);

// Engine prefs next to the viewer exe (omsi_f4_engine_prefs.cfg) — survives without a known OMSI root.
[[nodiscard]] std::filesystem::path engine_prefs_path();
// Global editor UI prefs next to the exe (omsi_f4_editor.cfg) — [ui]/[vista]/[menu_steps] for any map.
[[nodiscard]] std::filesystem::path editor_ui_prefs_path();
[[nodiscard]] bool is_valid_omsi_root(const std::filesystem::path& omsi_root);
[[nodiscard]] std::optional<std::filesystem::path> load_saved_omsi_root();
void save_omsi_root(const std::filesystem::path& omsi_root);
// OMSI 2 folders used before (most recent first, valid ones only); save_omsi_root() adds to it.
[[nodiscard]] std::vector<std::filesystem::path> load_recent_omsi_roots();

} // namespace omsi::options
