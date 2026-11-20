#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "omsi/sco_types.hpp"

#include "app/map_editor/editor_types.hpp"

namespace omsi::app::map_editor {

// Single source of truth for the .sco path editor.
//
// The 3D interaction (G/R/RR/N/click/Ctrl-snap/Esc) and the ImGui "Paths" tab both read and write
// this struct. The run loop owns one instance and hands its address to the side panel; the panel
// never keeps a mirror copy. Mutations mark `dirty` and/or `rebuild_requested`; the run loop rebuilds
// the path rails at the start of the next frame. Actions that need undo (new/duplicate/delete/paste)
// are requested through the `req_*` flags and performed by the run loop so it can push an undo
// snapshot *before* mutating.
struct PathEditorState {
    bool active = false;
    bool dirty = false;
    std::int32_t object_id = 0;
    std::filesystem::path sco_path;

    // Path data (mirrors Omsi.exe [path] blocks).
    std::vector<sco::ScoPath> paths;
    // Multi-selection (page-style Ctrl+click). `selected` holds indices; `sel` is the primary/active
    // one (== last clicked), used by the 3D drag and the parameter editor.
    std::vector<int> selected;
    int sel = -1;

    // Traffic lights ([traffic_lights_group]).
    sco::ScoTrafficLightsGroup tl;
    bool has_tl = false;
    int tl_sel = 0;

    // 3D interaction.
    bool moving = false;
    bool rotating = false;
    bool following = false; // RR: engine spline-follow (fit_spline_to_xy)
    bool placing = false;
    float grab_lx = 0.f, grab_ly = 0.f, grab_lz = 0.f, grab_hdg = 0.f, grab_len = 0.f, grab_radius = 0.f;
    float grab_wx = 0.f, grab_wy = 0.f, grab_wz = 0.f;

    // Clipboard (Ctrl+C / Ctrl+V, multi).
    std::vector<sco::ScoPath> clip;

    // AA (engine double-A autofill) and its first endpoint pick.
    bool aa_mode = false;
    PathEndpointPick aa_a{};

    // Snapshot for "discard changes".
    std::vector<sco::ScoPath> snap_paths;
    sco::ScoTrafficLightsGroup snap_tl;
    bool snap_has_tl = false;

    // Set by any mutation (panel or 3D) that needs the rails re-uploaded.
    bool rebuild_requested = false;

    // ImGui tab requests, consumed by the run loop (undo snapshot pushed there).
    bool req_new = false;
    bool req_duplicate = false;
    bool req_delete = false;
    bool req_copy = false;
    bool req_paste = false;
    bool req_save = false;
    bool req_exit = false;
    bool req_snap_to_spline = false;
    bool req_undo = false;
    bool req_redo = false;

    // Legacy slots kept so the editor snapshot / marker sync keep compiling.
    bool traffic_open = false;
    int temp_first = -1;
    int temp_count = 0;

    // Display options (Texturas tab).
    bool show_arrows = true;
    bool show_width = true;
    float path_opacity = 0.55f;

    [[nodiscard]] bool is_selected(int i) const {
        if (i < 0) return false;
        if (i == sel) return true;
        return std::find(selected.begin(), selected.end(), i) != selected.end();
    }
    [[nodiscard]] bool has_index(int i) const {
        return i >= 0 && static_cast<std::size_t>(i) < paths.size();
    }
    void select_only(int i) {
        selected.clear();
        if (i >= 0) selected.push_back(i);
        sel = i;
    }
    void toggle_select(int i) {
        if (i < 0) return;
        const auto it = std::find(selected.begin(), selected.end(), i);
        if (it != selected.end())
            selected.erase(it);
        else
            selected.push_back(i);
        sel = selected.empty() ? -1 : selected.back();
    }
    [[nodiscard]] bool primary_valid() const { return has_index(sel); }
};

} // namespace omsi::app::map_editor
