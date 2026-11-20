#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "editor_side_panel.hpp"

#include "omsi/map_types.hpp"

namespace omsi::ui {

// Modal property editors — SS_NOTIFY action bars + Enter/Esc (no nested Apply BUTTON pages).
[[nodiscard]] bool run_path_rule_dialog(HWND owner, EditorPathRuleProps& io);
[[nodiscard]] bool run_extra_lines_dialog(HWND owner, EditorObjectProps& io);
// Returns selected entrypoint index, or -1 if cancelled.
[[nodiscard]] int run_entrypoint_picker_dialog(HWND owner, const map::GlobalMap& global,
                                               const std::vector<map::EntryPoint>& entries,
                                               int current_sel = 0);
// Tile radius 1..20 (Chebyshev); visual radius 50..2000 m (0 = unlimited).
// Optional: union_ball / grad_circle screen size as percent of min(viewport) (e.g. 2.0 = 2%).
// Returns false if cancelled.
[[nodiscard]] bool run_editor_options_dialog(HWND owner, int& io_tile_radius, int& io_visual_radius_m,
                                             float* io_union_ball_pct = nullptr,
                                             float* io_grad_circle_pct = nullptr);

} // namespace omsi::ui
