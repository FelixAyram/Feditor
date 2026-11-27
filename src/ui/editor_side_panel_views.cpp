#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "omsi/path_utf8.hpp"
#include "ui/editor_side_panel.hpp"
#include "ui/editor_side_panel_ids.hpp"
#include "ui/map_editor_imgui_panel.hpp"
#include "ui/i18n.hpp"

#include "omsi/logger.hpp"
#include "omsi/map_geo_tools.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace omsi::ui {
using namespace editor_side_panel_ids;
void EditorSidePanel::paint_tab_strip(HDC hdc) {
    RECT rc{};
    GetClientRect(tab_strip_ ? tab_strip_ : hwnd_, &rc);
    FillRect(hdc, &rc, bg_brush_);
    const int h = (std::max)(1, static_cast<int>(rc.bottom - rc.top));
    const int tab_h = h / kTabCount;
    const int active = static_cast<int>(tool_);

    // OMSI vertical tabs: LOGFONT escapement 900 rotates the whole label.
    SetBkMode(hdc, TRANSPARENT);
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font_vert_));
    SetTextAlign(hdc, TA_BASELINE | TA_LEFT);
    auto draw_vert = [&](int x, int y_bottom, const wchar_t* text, COLORREF col) {
        SetTextColor(hdc, col);
        SIZE sz{};
        GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
        const int y = y_bottom - sz.cx;
        TextOutW(hdc, x, y, text, static_cast<int>(wcslen(text)));
    };
    SetTextColor(hdc, RGB(90, 90, 90));
    for (const auto& g : kGroups) {
        const int y1 = (g.last + 1) * tab_h;
        draw_vert(4, y1 - 8, g.name, RGB(90, 90, 90));
    }

    for (int i = 0; i < kTabCount; ++i) {
        RECT tr{16, i * tab_h + 1, kTabStripW - 2, (i + 1) * tab_h - 1};
        if (i == active) {
            HBRUSH sel = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(hdc, &tr, sel);
            DeleteObject(sel);
            DrawEdge(hdc, &tr, EDGE_SUNKEN, BF_RECT);
            draw_vert(tr.left + 6, tr.bottom - 6, kTabNames[i], RGB(0, 0, 0));
        } else {
            DrawEdge(hdc, &tr, EDGE_RAISED, BF_RECT);
            draw_vert(tr.left + 6, tr.bottom - 6, kTabNames[i], RGB(30, 30, 30));
        }
    }
    SetTextAlign(hdc, TA_LEFT | TA_TOP);
    SelectObject(hdc, old);
}

void EditorSidePanel::layout_children() {
    if (!hwnd_ || !owner_) return;
    RECT rc{};
    GetClientRect(owner_, &rc);
    const int ow = (std::max)(1, static_cast<int>(rc.right - rc.left));
    const int oh = (std::max)(1, static_cast<int>(rc.bottom - rc.top));
    if (!layout_force_ && ow == layout_ow_ && oh == layout_oh_) return;
    layout_force_ = false;
    layout_ow_ = ow;
    layout_oh_ = oh;
    z_order_last_ = {}; // re-assert the z-order right after a re-layout
    const int view_w = (std::max)(1, ow - kPanelWidth);
    if (toolbar_) MoveWindow(toolbar_, 0, 0, view_w, kToolbarH, TRUE);
    if (HWND menu_btn = GetDlgItem(toolbar_, IDC_BTN_TOOLBAR_MENU))
        MoveWindow(menu_btn, view_w - 86, 3, 80, 22, TRUE);
    MoveWindow(hwnd_, ow - kPanelWidth, 0, kPanelWidth, oh, TRUE);
    if (tab_strip_) MoveWindow(tab_strip_, 0, 0, kTabStripW, oh, TRUE);
    const int cw = kPanelWidth - kContentX - 4;
    const int ch = oh - 40;
    auto move_page = [&](HWND p) {
        if (p) MoveWindow(p, kContentX, 0, cw, ch, TRUE);
    };
    move_page(page_tile_);
    move_page(page_object_);
    move_page(page_spline_);
    move_page(page_terrain_);
    move_page(page_traffic_);
    move_page(page_tracks_);
    move_page(page_station_);
    move_page(page_splexp_);
    move_page(page_signal_);
    if (status_) MoveWindow(status_, kContentX, oh - 36, cw, 32, TRUE);
}

void EditorSidePanel::refill_catalog() {
    HWND list = (tool_ == EditorTool::Spline) ? s_catalog_ : o_catalog_;
    if (!list) return;
    const bool splines = (tool_ == EditorTool::Spline);
    const auto& items = splines ? catalog_splines_ : catalog_objects_;
    const std::wstring cat_filter = edit_text(splines ? s_cat_search_ : o_cat_search_);
    auto& filtered = splines ? filtered_catalog_spline_indices_ : filtered_catalog_object_indices_;
    filtered.clear();
    // (the Win32 list is hidden while ImGui is the panel: only the filtered indices are kept; it is filled
    //  when the Win32 panel is shown again - filling it with every entry stalled the editor on tool changes)
    const bool fill = !imgui_primary_;
    if (fill) SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!item_matches_search(cat_filter, items[i])) continue;
        filtered.push_back(static_cast<int>(i));
        if (!fill) continue;
        std::wstring label = std::to_wstring(filtered.size() - 1) + L": " + to_wide(items[i].label);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    const int sel = splines ? catalog_sli_sel_ : catalog_obj_sel_;
    if (fill && sel >= 0) {
        for (std::size_t li = 0; li < filtered.size(); ++li) {
            if (filtered[li] == sel) {
                SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(li), 0);
                SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(li), 0);
                break;
            }
        }
    }
}

std::wstring EditorSidePanel::edit_text(HWND edit) {
    if (!edit) return {};
    const int len = GetWindowTextLengthW(edit);
    if (len <= 0) return {};
    std::wstring buf(static_cast<std::size_t>(len + 1), L'\0');
    GetWindowTextW(edit, buf.data(), len + 1);
    buf.resize(static_cast<std::size_t>(len));
    return buf;
}

static void lower_inplace(std::wstring& s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
}

bool EditorSidePanel::item_matches_search(const std::wstring& filter, const EditorListItem& it) {
    if (filter.empty()) return true;
    std::wstring f = filter;
    while (!f.empty() && (f.back() == L' ' || f.back() == L'\t')) f.pop_back();
    std::size_t start = 0;
    while (start < f.size() && (f[start] == L' ' || f[start] == L'\t')) ++start;
    if (start > 0) f = f.substr(start);
    if (f.empty()) return true;
    lower_inplace(f);
    std::wstring label = to_wide(it.label);
    lower_inplace(label);
    if (label.find(f) != std::wstring::npos) return true;
    const bool all_digits =
        !f.empty() && std::all_of(f.begin(), f.end(), [](wchar_t c) { return std::iswdigit(c) != 0; });
    if (all_digits) {
        const auto id_w = std::to_wstring(it.id);
        return id_w.size() >= f.size() && id_w.compare(0, f.size(), f) == 0;
    }
    return false;
}

bool EditorSidePanel::item_matches_tile(const std::wstring& filter, const EditorListItem& it) {
    if (filter.empty()) return true;
    std::wstring f = filter;
    lower_inplace(f);
    const auto gx = std::to_wstring(it.tile_gx);
    const auto gy = std::to_wstring(it.tile_gy);
    const std::wstring pair = gx + L"," + gy;
    if (pair.find(f) != std::wstring::npos) return true;
    if (gx.find(f) != std::wstring::npos || gy.find(f) != std::wstring::npos) return true;
    return false;
}

int EditorSidePanel::filtered_object_index(int list_sel) const {
    if (list_sel < 0 || static_cast<std::size_t>(list_sel) >= filtered_object_indices_.size()) return -1;
    return filtered_object_indices_[static_cast<std::size_t>(list_sel)];
}

int EditorSidePanel::filtered_spline_index(int list_sel) const {
    if (list_sel < 0 || static_cast<std::size_t>(list_sel) >= filtered_spline_indices_.size()) return -1;
    return filtered_spline_indices_[static_cast<std::size_t>(list_sel)];
}

int EditorSidePanel::filtered_catalog_object_index(int list_sel) const {
    if (list_sel < 0 || static_cast<std::size_t>(list_sel) >= filtered_catalog_object_indices_.size()) return -1;
    return filtered_catalog_object_indices_[static_cast<std::size_t>(list_sel)];
}

int EditorSidePanel::filtered_catalog_spline_index(int list_sel) const {
    if (list_sel < 0 || static_cast<std::size_t>(list_sel) >= filtered_catalog_spline_indices_.size()) return -1;
    return filtered_catalog_spline_indices_[static_cast<std::size_t>(list_sel)];
}

void EditorSidePanel::refill_instances() {
    HWND list = (tool_ == EditorTool::Spline) ? s_instances_ : o_instances_;
    if (!list) return;
    const bool splines = (tool_ == EditorTool::Spline);
    const auto& items = splines ? spline_list_ : object_list_;
    const std::wstring tile_filter = edit_text(splines ? s_tile_search_ : o_tile_search_);
    const std::wstring inst_filter = edit_text(splines ? s_inst_search_ : o_inst_search_);
    auto& filtered = splines ? filtered_spline_indices_ : filtered_object_indices_;
    filtered.clear();
    // (hidden while ImGui is the panel: 47k LB_ADDSTRING with 4 tiles streamed took 0.5-1 s per tool change)
    const bool fill = !imgui_primary_;
    if (fill) SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& it : items) {
        if (!item_matches_tile(tile_filter, it)) continue;
        if (!item_matches_search(inst_filter, it)) continue;
        if (it.map_index >= 0) filtered.push_back(it.map_index);
        if (!fill) continue;
        const auto w = to_wide(it.label);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
}

void EditorSidePanel::set_entrypoints(const std::vector<map::EntryPoint>& entries) { entries_data_ = entries; }
void EditorSidePanel::set_scene_items(const std::vector<EditorSceneItem>& items) { scene_data_ = items; }
void EditorSidePanel::set_object_list(const std::vector<EditorListItem>& items) {
    object_list_ = items;
    if (tool_ == EditorTool::Object) refill_instances();
}
void EditorSidePanel::set_spline_list(const std::vector<EditorListItem>& items) {
    spline_list_ = items;
    if (tool_ == EditorTool::Spline) refill_instances();
}
void EditorSidePanel::set_catalog_objects(const std::vector<EditorListItem>& items) {
    catalog_objects_ = items;
    if (tool_ == EditorTool::Object) refill_catalog();
}
void EditorSidePanel::set_catalog_splines(const std::vector<EditorListItem>& items) {
    catalog_splines_ = items;
    if (tool_ == EditorTool::Spline) refill_catalog();
}

void EditorSidePanel::set_tool(EditorTool tool) {
    if (tool != tool_) {
        pending_.tool_changed = true;
    }
    tool_ = tool;
    sync_page_visibility();
    refill_catalog();
    refill_instances();
    if (tab_strip_) InvalidateRect(tab_strip_, nullptr, TRUE);
    if (tool == EditorTool::TrafficRules) {
        pending_.toggled_markers = true;
        if (s_chk_path_rails_) SendMessageW(s_chk_path_rails_, BM_SETCHECK, BST_CHECKED, 0);
    }
}

void EditorSidePanel::queue_imgui_tool_tab(int tab_index) {
    imgui_state_.queued_tool_tab = tab_index;
}

void EditorSidePanel::clear_selection_props() {
    last_object_.valid = false;
    imgui_object_.valid = false;
    last_spline_.valid = false;
    imgui_spline_.valid = false;
}

void EditorSidePanel::apply_ui_settings(const map::EditorUiSettings& ui) {
    if (ui.tool >= 0 && ui.tool <= static_cast<int>(EditorTool::SignalRts))
        set_tool(static_cast<EditorTool>(ui.tool));
    imgui_state_.show_spline_ends = ui.show_spline_ends;
    imgui_state_.show_path_ends = ui.show_path_ends;
    imgui_state_.show_path_rails = ui.show_path_rails;
    imgui_state_.autofill_spline_ends = ui.autofill_spline_ends;
    imgui_state_.tex_max_256 = ui.tex_max_256;
    imgui_state_.place_feedback = ui.place_feedback;
    imgui_state_.place_feedback_volume = ui.place_feedback_volume;
    imgui_state_.sfx_pack = ui.sfx_pack;
    imgui_state_.terrain_brush.brush_size_m = ui.terrain_brush_size_m;
    imgui_state_.terrain_brush.speed = ui.terrain_brush_speed;
    imgui_state_.terrain_brush.set_height = ui.terrain_set_height;
    imgui_state_.terrain_brush.ground_layer = ui.terrain_ground_layer;
    imgui_state_.terrain_brush.mode = static_cast<TerrainBrushModeUi>(
        (std::max)(0, (std::min)(ui.terrain_brush_mode, 5)));
    imgui_state_.spawn_stick_terrain = ui.spawn_stick_terrain;
    imgui_state_.spawn_on_meshes = ui.spawn_on_meshes;
    imgui_state_.spawn_align_pitch_bank = ui.spawn_align_pitch_bank;
    imgui_state_.spawn_random = ui.spawn_random;
    imgui_state_.spawn_random_mode = (std::clamp)(ui.spawn_random_mode, 0, 2);
    imgui_state_.spawn_random_fav_cat = (std::max)(0, ui.spawn_random_fav_cat);
    imgui_state_.spawn_random_folder = ui.spawn_random_folder;
    if (!imgui_state_.spawn_random_folder.empty())
        rebuild_spawn_random_folder_pool(imgui_state_);
    imgui_state_.spline_stick_chain = ui.spline_stick_chain;
    imgui_state_.field_steps = ui.field_steps;
    imgui_state_.sim_hour = ui.sim_hour;
    imgui_state_.sim_month = ui.sim_month;
    imgui_state_.sim_day = ui.sim_day;
    imgui_state_.sim_year = ui.sim_year;
    imgui_state_.time_running = ui.time_running;
    imgui_state_.time_speed = ui.time_speed;
    imgui_state_.timezone_h = ui.timezone_h;
    imgui_state_.weather_id = ui.weather_id;
    imgui_state_.show_sun_shadows = ui.show_sun_shadows;
    imgui_state_.shaft_plus = ui.shaft_plus;
    imgui_state_.fog_enabled = ui.fog_enabled;
    imgui_state_.parked_cars_on = ui.parked_cars_on;
    imgui_state_.show_onlyeditor_objects = ui.show_onlyeditor_objects;
    imgui_state_.shadow_distance_m = ui.shadow_distance_m;
    imgui_state_.shadow_map_size = ui.shadow_map_size;
    imgui_state_.path_style = ui.path_style == 1 ? 1 : 0;
    imgui_state_.hide_onlyeditor_splines = ui.hide_onlyeditor_splines;
    imgui_state_.hints_ignore = ui.hints_ignore;
    imgui_state_.spline_fill_chain = ui.spline_fill_chain;
    imgui_state_.show_sco_origin = ui.show_sco_origin;
    imgui_state_.arrows_3d = ui.arrows_3d;
    imgui_state_.hide_nodes = ui.hide_nodes;
    i18n::set_language(ui.language == 1 ? i18n::Lang::En : i18n::Lang::Es);
    if (s_chk_ends_)
        SendMessageW(s_chk_ends_, BM_SETCHECK, ui.show_spline_ends ? BST_CHECKED : BST_UNCHECKED, 0);
    if (s_chk_path_ends_)
        SendMessageW(s_chk_path_ends_, BM_SETCHECK, ui.show_path_ends ? BST_CHECKED : BST_UNCHECKED, 0);
    if (s_chk_path_rails_)
        SendMessageW(s_chk_path_rails_, BM_SETCHECK, ui.show_path_rails ? BST_CHECKED : BST_UNCHECKED, 0);
    if (s_chk_autofill_)
        SendMessageW(s_chk_autofill_, BM_SETCHECK, ui.autofill_spline_ends ? BST_CHECKED : BST_UNCHECKED, 0);
    set_edit_float(terr_brush_size_, ui.terrain_brush_size_m);
    set_edit_float(terr_speed_, ui.terrain_brush_speed);
    set_edit_float(terr_set_h_, ui.terrain_set_height);
    HWND terr_radios[] = {terr_radio_higher_, terr_radio_lower_, terr_radio_set_, terr_radio_paint_,
                          terr_radio_rubber_};
    const int tm = (std::max)(0, (std::min)(ui.terrain_brush_mode, 4));
    for (int i = 0; i < 5; ++i) {
        if (terr_radios[i])
            SendMessageW(terr_radios[i], BM_SETCHECK, (i == tm) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    ground_layer_sel_ = ui.terrain_ground_layer;
    if (terr_layers_) {
        const int n = static_cast<int>(SendMessageW(terr_layers_, LB_GETCOUNT, 0, 0));
        if (n > 0) {
            const int sel = (std::max)(0, (std::min)(ui.terrain_ground_layer, n - 1));
            SendMessageW(terr_layers_, LB_SETCURSEL, sel, 0);
            ground_layer_sel_ = sel;
        }
    }
    bool stick = ui.spawn_stick_terrain;
    bool mesh = ui.spawn_on_meshes;
    sync_spawn_height_checks(stick, mesh);
    const auto set_align = [](HWND h, bool on) {
        if (h) SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    set_align(t_chk_spawn_align_, ui.spawn_align_pitch_bank);
    set_align(o_chk_spawn_align_, ui.spawn_align_pitch_bank);
    set_align(s_chk_spawn_align_, ui.spawn_align_pitch_bank);
}

void EditorSidePanel::sync_spawn_height_checks(bool stick_terrain, bool spawn_on_meshes) const {
    if (stick_terrain && spawn_on_meshes) spawn_on_meshes = false;
    const auto set = [](HWND h, bool on) {
        if (h) SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    set(t_chk_spawn_terr_, stick_terrain);
    set(t_chk_spawn_mesh_, spawn_on_meshes);
    set(o_chk_spawn_terr_, stick_terrain);
    set(o_chk_spawn_mesh_, spawn_on_meshes);
    set(s_chk_spawn_terr_, stick_terrain);
    set(s_chk_spawn_mesh_, spawn_on_meshes);
}

void EditorSidePanel::on_spawn_height_clicked(int id) {
    HWND src = nullptr;
    if (id == IDC_T_SPAWN_TERR) src = t_chk_spawn_terr_;
    else if (id == IDC_O_SPAWN_TERR) src = o_chk_spawn_terr_;
    else if (id == IDC_S_SPAWN_TERR) src = s_chk_spawn_terr_;
    else if (id == IDC_T_SPAWN_MESH) src = t_chk_spawn_mesh_;
    else if (id == IDC_O_SPAWN_MESH) src = o_chk_spawn_mesh_;
    else if (id == IDC_S_SPAWN_MESH) src = s_chk_spawn_mesh_;
    else if (id == IDC_T_SPAWN_ALIGN) src = t_chk_spawn_align_;
    else if (id == IDC_O_SPAWN_ALIGN) src = o_chk_spawn_align_;
    else if (id == IDC_S_SPAWN_ALIGN) src = s_chk_spawn_align_;
    const bool is_terr =
        id == IDC_T_SPAWN_TERR || id == IDC_O_SPAWN_TERR || id == IDC_S_SPAWN_TERR;
    const bool is_mesh =
        id == IDC_T_SPAWN_MESH || id == IDC_O_SPAWN_MESH || id == IDC_S_SPAWN_MESH;
    if (is_terr || is_mesh) {
        const bool on = src && SendMessageW(src, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (is_terr && on)
            sync_spawn_height_checks(true, false);
        else if (is_mesh && on)
            sync_spawn_height_checks(false, true);
        else if (is_terr || is_mesh)
            sync_spawn_height_checks(false, false);
    } else if (id == IDC_T_SPAWN_ALIGN || id == IDC_O_SPAWN_ALIGN || id == IDC_S_SPAWN_ALIGN) {
        const bool on = src && SendMessageW(src, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const auto set = [](HWND h, bool v) {
            if (h) SendMessageW(h, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
        };
        set(t_chk_spawn_align_, on);
        set(o_chk_spawn_align_, on);
        set(s_chk_spawn_align_, on);
    }
    pending_.save_ui_settings = true;
}

void EditorSidePanel::collect_ui_settings(map::EditorUiSettings& out) const {
    out.tool = static_cast<int>(tool_);
    if (imgui_primary_) {
        out.show_spline_ends = imgui_state_.show_spline_ends;
        out.show_path_ends = imgui_state_.show_path_ends;
        out.show_path_rails = imgui_state_.show_path_rails;
        out.autofill_spline_ends = imgui_state_.autofill_spline_ends;
        out.tex_max_256 = imgui_state_.tex_max_256;
        out.place_feedback = imgui_state_.place_feedback;
        out.place_feedback_volume = imgui_state_.place_feedback_volume;
        out.sfx_pack = imgui_state_.sfx_pack;
        out.terrain_brush_size_m = imgui_state_.terrain_brush.brush_size_m;
        out.terrain_brush_speed = imgui_state_.terrain_brush.speed;
        out.terrain_set_height = imgui_state_.terrain_brush.set_height;
        out.terrain_ground_layer = imgui_state_.terrain_brush.ground_layer;
        out.terrain_brush_mode = static_cast<int>(imgui_state_.terrain_brush.mode);
        out.spawn_stick_terrain = imgui_state_.spawn_stick_terrain;
        out.spawn_on_meshes = imgui_state_.spawn_on_meshes;
        out.spawn_align_pitch_bank = imgui_state_.spawn_align_pitch_bank;
        out.spawn_random = imgui_state_.spawn_random;
        out.spawn_random_mode = imgui_state_.spawn_random_mode;
        out.spawn_random_fav_cat = imgui_state_.spawn_random_fav_cat;
        out.spawn_random_folder = imgui_state_.spawn_random_folder;
        out.spline_stick_chain = imgui_state_.spline_stick_chain;
        out.field_steps = imgui_state_.field_steps;
        out.sim_hour = imgui_state_.sim_hour;
        out.sim_month = imgui_state_.sim_month;
        out.sim_day = imgui_state_.sim_day;
        out.sim_year = imgui_state_.sim_year;
        out.time_running = imgui_state_.time_running;
        out.time_speed = imgui_state_.time_speed;
        out.timezone_h = imgui_state_.timezone_h;
        out.weather_id = imgui_state_.weather_id;
        out.show_sun_shadows = imgui_state_.show_sun_shadows;
        out.shaft_plus = imgui_state_.shaft_plus;
        out.fog_enabled = imgui_state_.fog_enabled;
        out.parked_cars_on = imgui_state_.parked_cars_on;
        out.show_onlyeditor_objects = imgui_state_.show_onlyeditor_objects;
        out.shadow_distance_m = imgui_state_.shadow_distance_m;
        out.shadow_map_size = imgui_state_.shadow_map_size;
        out.path_style = imgui_state_.path_style == 1 ? 1 : 0;
        out.hide_onlyeditor_splines = imgui_state_.hide_onlyeditor_splines;
        out.hints_ignore = imgui_state_.hints_ignore;
        out.spline_fill_chain = imgui_state_.spline_fill_chain;
        out.show_sco_origin = imgui_state_.show_sco_origin;
        out.arrows_3d = imgui_state_.arrows_3d;
        out.hide_nodes = imgui_state_.hide_nodes;
        out.language = i18n::language() == i18n::Lang::En ? 1 : 0;
        return;
    }
    out.show_spline_ends =
        !s_chk_ends_ || SendMessageW(s_chk_ends_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.show_path_ends =
        !s_chk_path_ends_ || SendMessageW(s_chk_path_ends_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.show_path_rails =
        s_chk_path_rails_ && SendMessageW(s_chk_path_rails_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.autofill_spline_ends =
        s_chk_autofill_ && SendMessageW(s_chk_autofill_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    TerrainBrushUi brush{};
    if (terrain_paint_brush(brush)) {
        out.terrain_brush_size_m = brush.brush_size_m;
        out.terrain_brush_speed = brush.speed;
        out.terrain_set_height = brush.set_height;
        out.terrain_ground_layer = brush.ground_layer;
        out.terrain_brush_mode = static_cast<int>(brush.mode);
    }
    out.spawn_stick_terrain =
        (t_chk_spawn_terr_ && SendMessageW(t_chk_spawn_terr_, BM_GETCHECK, 0, 0) == BST_CHECKED) ||
        (o_chk_spawn_terr_ && SendMessageW(o_chk_spawn_terr_, BM_GETCHECK, 0, 0) == BST_CHECKED) ||
        (s_chk_spawn_terr_ && SendMessageW(s_chk_spawn_terr_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.spawn_on_meshes =
        (t_chk_spawn_mesh_ && SendMessageW(t_chk_spawn_mesh_, BM_GETCHECK, 0, 0) == BST_CHECKED) ||
        (o_chk_spawn_mesh_ && SendMessageW(o_chk_spawn_mesh_, BM_GETCHECK, 0, 0) == BST_CHECKED) ||
        (s_chk_spawn_mesh_ && SendMessageW(s_chk_spawn_mesh_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (out.spawn_stick_terrain && out.spawn_on_meshes) out.spawn_on_meshes = false;
    out.spawn_align_pitch_bank =
        (t_chk_spawn_align_ && SendMessageW(t_chk_spawn_align_, BM_GETCHECK, 0, 0) == BST_CHECKED) ||
        (o_chk_spawn_align_ && SendMessageW(o_chk_spawn_align_, BM_GETCHECK, 0, 0) == BST_CHECKED) ||
        (s_chk_spawn_align_ && SendMessageW(s_chk_spawn_align_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.field_steps = imgui_state_.field_steps;
}

void EditorSidePanel::set_status(const std::string& status) {
    status_text_ = status;
    if (status_) SetWindowTextW(status_, to_wide(status).c_str());
}
void EditorSidePanel::set_selection_text(const std::string& text) {
    selection_text_ = text;
    if (!imgui_primary_) set_status(text);
}
void EditorSidePanel::set_autofill(bool enabled) {
    imgui_state_.autofill_spline_ends = enabled;
    if (s_chk_autofill_) SendMessageW(s_chk_autofill_, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

void EditorSidePanel::set_tile_page(const EditorTilePageInfo& info, const map::EditorGeoSettings& geo) {
    if (!info.valid) return;
    if (t_coord_lbl_) {
        const auto label = map::map_coord_kind_label(info.coord_kind);
        SetWindowTextW(t_coord_lbl_, (L"Coord: " + to_wide(label)).c_str());
    }
    if (t_tile_lbl_) {
        wchar_t buf[320]{};
        if (!info.wgs84_summary.empty()) {
            if (std::fabs(info.latitude_deg) > 1e-4f) {
                swprintf_s(buf, L"Tile (%d,%d)  %.1f m  lat %.4f  |  %hs", info.grid_x, info.grid_y,
                           static_cast<double>(info.tile_size_m), static_cast<double>(info.latitude_deg),
                           info.wgs84_summary.c_str());
            } else {
                swprintf_s(buf, L"Tile (%d,%d)  %.1f m  |  %hs", info.grid_x, info.grid_y,
                           static_cast<double>(info.tile_size_m), info.wgs84_summary.c_str());
            }
        } else if (std::fabs(info.latitude_deg) > 1e-4f) {
            swprintf_s(buf, L"Tile (%d,%d)  %.1f m  lat %.4f%s", info.grid_x, info.grid_y,
                       static_cast<double>(info.tile_size_m), static_cast<double>(info.latitude_deg),
                       info.tile_in_map ? L"" : L"  (inferido)");
        } else {
            swprintf_s(buf, L"Tile (%d,%d)  %.1f m%s", info.grid_x, info.grid_y,
                       static_cast<double>(info.tile_size_m),
                       info.tile_in_map ? L"" : L"  — SIN TILE (Crear tile)");
        }
        SetWindowTextW(t_tile_lbl_, buf);
    }
    if (t_tile_hint_) SetWindowTextW(t_tile_hint_, to_wide(info.hint).c_str());
    if (t_btn_create_tile_) EnableWindow(t_btn_create_tile_, info.tile_in_map ? FALSE : TRUE);
    if (t_btn_delete_tile_) EnableWindow(t_btn_delete_tile_, info.tile_in_map ? TRUE : FALSE);
    if (t_btn_convert_) EnableWindow(t_btn_convert_, info.can_convert ? TRUE : FALSE);
    const bool bounds_ok = info.geo_ready || info.coord_kind == map::MapCoordKind::WorldCoordinates;
    if (t_aerial_vis_) EnableWindow(t_aerial_vis_, bounds_ok ? TRUE : FALSE);
    if (t_aerial_res_) EnableWindow(t_aerial_res_, bounds_ok ? TRUE : FALSE);
    if (t_btn_aerial_dnl_) EnableWindow(t_btn_aerial_dnl_, bounds_ok ? TRUE : FALSE);
    if (t_btn_aerial_del_) EnableWindow(t_btn_aerial_del_, info.tile_in_map ? TRUE : FALSE);
    if (t_sat_provider_) EnableWindow(t_sat_provider_, TRUE);
    if (t_dtm_provider_) EnableWindow(t_dtm_provider_, TRUE);
    if (t_ref_lat_) EnableWindow(t_ref_lat_, TRUE);
    if (t_ref_lon_) EnableWindow(t_ref_lon_, TRUE);
    if (t_btn_dtm_) EnableWindow(t_btn_dtm_, bounds_ok ? TRUE : FALSE);

    const bool omsi_sat = geo.satellite_provider == map::SatelliteProvider::OmsiLinkAerial ||
                          geo.satellite_provider == map::SatelliteProvider::GcmodsBing ||
                          geo.satellite_provider == map::SatelliteProvider::GcmodsGoogle;
    const bool custom_url = geo.satellite_provider == map::SatelliteProvider::CustomAerialUrl;
    if (t_link_lbl_) ShowWindow(t_link_lbl_, (omsi_sat || custom_url) ? SW_SHOW : SW_HIDE);
    if (t_link_aerial_) ShowWindow(t_link_aerial_, (omsi_sat || custom_url) ? SW_SHOW : SW_HIDE);
    if (t_custom_preset_lbl_) ShowWindow(t_custom_preset_lbl_, custom_url ? SW_SHOW : SW_HIDE);
    if (t_custom_preset_) ShowWindow(t_custom_preset_, custom_url ? SW_SHOW : SW_HIDE);
    if (t_api_lbl_) ShowWindow(t_api_lbl_, SW_SHOW);
    if (t_api_key_) ShowWindow(t_api_key_, SW_SHOW);

    if (t_aerial_vis_ && GetFocus() != t_aerial_vis_) {
        SendMessageW(t_aerial_vis_, BM_SETCHECK, geo.tile_aerial_visible ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (t_aerial_res_ && GetFocus() != t_aerial_res_) {
        SendMessageW(t_aerial_res_, TBM_SETPOS, TRUE, geo.aerial_resolution_pos);
    }
    if (t_aerial_res_lbl_) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"Resolution: %d px", map::aerial_resolution_label_px(geo.aerial_resolution_pos));
        SetWindowTextW(t_aerial_res_lbl_, buf);
    }
    if (t_sat_provider_ && GetFocus() != t_sat_provider_) {
        const int sel = static_cast<int>(geo.satellite_provider);
        if (sel >= 0 && sel < 16) SendMessageW(t_sat_provider_, CB_SETCURSEL, sel, 0);
    }
    if (t_custom_preset_ && GetFocus() != t_custom_preset_) {
        const int sel = geo.custom_aerial_preset;
        if (sel >= 0 && sel <= 8) SendMessageW(t_custom_preset_, CB_SETCURSEL, sel, 0);
    }
    if (t_dtm_provider_ && GetFocus() != t_dtm_provider_) {
        const int sel = static_cast<int>(geo.elevation_provider);
        if (sel >= 0 && sel < 4) SendMessageW(t_dtm_provider_, CB_SETCURSEL, sel, 0);
    }

    if (t_link_aerial_ && GetFocus() != t_link_aerial_) {
        std::string url = geo.editor_link_aerial;
        if (geo.satellite_provider == map::SatelliteProvider::GcmodsBing)
            url = map::gcmods_bing_satellite_link();
        else if (geo.satellite_provider == map::SatelliteProvider::GcmodsGoogle)
            url = map::gcmods_google_satellite_link();
        else if (custom_url && !geo.custom_aerial_url.empty())
            url = geo.custom_aerial_url;
        SetWindowTextW(t_link_aerial_, to_wide(url).c_str());
    }
    if (t_api_key_ && GetFocus() != t_api_key_) {
        std::string key;
        if (geo.satellite_provider == map::SatelliteProvider::MapTiler) key = geo.maptiler_api_key;
        else if (geo.satellite_provider == map::SatelliteProvider::BingAerial ||
                 geo.satellite_provider == map::SatelliteProvider::GcmodsBing)
            key = geo.bing_api_key;
        else if (geo.satellite_provider == map::SatelliteProvider::GoogleMapTiles ||
                 geo.satellite_provider == map::SatelliteProvider::GoogleStaticMaps ||
                 geo.satellite_provider == map::SatelliteProvider::GcmodsGoogle ||
                 geo.elevation_provider == map::ElevationProvider::Google)
            key = geo.google_api_key;
        else if (!geo.maptiler_api_key.empty()) key = geo.maptiler_api_key;
        else if (!geo.bing_api_key.empty()) key = geo.bing_api_key;
        else key = geo.google_api_key;
        SetWindowTextW(t_api_key_, to_wide(key).c_str());
    }
    if (t_ref_lat_ && GetFocus() != t_ref_lat_) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"%.6f", geo.ref_lat);
        SetWindowTextW(t_ref_lat_, buf);
    }
    if (t_ref_lon_ && GetFocus() != t_ref_lon_) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"%.6f", geo.ref_lon);
        SetWindowTextW(t_ref_lon_, buf);
    }
}

void EditorSidePanel::read_tile_page_edits(map::EditorGeoSettings& out) const {
    if (imgui_primary_) {
        out = imgui_state_.tile_geo;
        return;
    }
    if (t_sat_provider_) {
        const int sel = static_cast<int>(SendMessageW(t_sat_provider_, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel <= 15) out.satellite_provider = static_cast<map::SatelliteProvider>(sel);
    }
    if (t_custom_preset_) {
        const int sel = static_cast<int>(SendMessageW(t_custom_preset_, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel <= 8) {
            out.custom_aerial_preset = sel;
            if (out.satellite_provider == map::SatelliteProvider::CustomAerialUrl && sel > 0) {
                const auto* preset_url =
                    map::custom_aerial_preset_url(static_cast<map::CustomAerialPreset>(sel));
                if (preset_url && preset_url[0] != '\0') out.custom_aerial_url = preset_url;
            }
        }
    }
    if (t_dtm_provider_) {
        const int sel = static_cast<int>(SendMessageW(t_dtm_provider_, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel <= 3) out.elevation_provider = static_cast<map::ElevationProvider>(sel);
    }
    if (out.satellite_provider == map::SatelliteProvider::GcmodsBing)
        out.editor_link_aerial = map::gcmods_bing_satellite_link();
    else if (out.satellite_provider == map::SatelliteProvider::GcmodsGoogle)
        out.editor_link_aerial = map::gcmods_google_satellite_link();
    if (t_link_aerial_) {
        wchar_t buf[1024]{};
        GetWindowTextW(t_link_aerial_, buf, 1024);
        const std::string url(buf, buf + wcslen(buf));
        if (out.satellite_provider == map::SatelliteProvider::CustomAerialUrl)
            out.custom_aerial_url = url;
        else if (out.satellite_provider == map::SatelliteProvider::OmsiLinkAerial)
            out.editor_link_aerial = url;
    }
    if (t_api_key_) {
        wchar_t buf[512]{};
        GetWindowTextW(t_api_key_, buf, 512);
        const std::string key(buf, buf + wcslen(buf));
        if (!key.empty()) {
            if (out.satellite_provider == map::SatelliteProvider::MapTiler) out.maptiler_api_key = key;
            else if (out.satellite_provider == map::SatelliteProvider::BingAerial ||
                     out.satellite_provider == map::SatelliteProvider::GcmodsBing)
                out.bing_api_key = key;
            else if (out.satellite_provider == map::SatelliteProvider::GoogleMapTiles ||
                     out.satellite_provider == map::SatelliteProvider::GoogleStaticMaps ||
                     out.satellite_provider == map::SatelliteProvider::GcmodsGoogle ||
                     out.elevation_provider == map::ElevationProvider::Google)
                out.google_api_key = key;
            else {
                // Allow pasting a key before switching provider.
                out.maptiler_api_key = key;
                out.bing_api_key = key;
                out.google_api_key = key;
            }
        }
    }
    out.ref_lat = get_edit_float(t_ref_lat_, static_cast<float>(out.ref_lat));
    out.ref_lon = get_edit_float(t_ref_lon_, static_cast<float>(out.ref_lon));
}

void EditorSidePanel::read_tile_page_flags(bool& tile_aerial_visible, int& aerial_resolution_pos) const {
    if (imgui_primary_) {
        tile_aerial_visible = imgui_state_.tile_aerial_visible;
        aerial_resolution_pos = imgui_state_.aerial_resolution_pos;
        return;
    }
    if (t_aerial_vis_) tile_aerial_visible = SendMessageW(t_aerial_vis_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (t_aerial_res_) {
        aerial_resolution_pos = static_cast<int>(SendMessageW(t_aerial_res_, TBM_GETPOS, 0, 0));
        if (aerial_resolution_pos < 0) aerial_resolution_pos = 0;
        if (aerial_resolution_pos > 5) aerial_resolution_pos = 5;
    }
}

void EditorSidePanel::remember_catalog_path(const std::filesystem::path& path) {
    if (path.empty()) return;
    const auto ext = path.extension().wstring();
    const bool want_spline = _wcsicmp(ext.c_str(), L".sli") == 0;
    const bool want_sco = _wcsicmp(ext.c_str(), L".sco") == 0;
    if (!want_spline && !want_sco) return;
    auto& items = want_spline ? catalog_splines_ : catalog_objects_;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].path != path &&
            _stricmp(items[i].path.filename().string().c_str(), path.filename().string().c_str()) != 0)
            continue;
        if (want_spline) catalog_sli_sel_ = static_cast<int>(i);
        else catalog_obj_sel_ = static_cast<int>(i);
        refill_catalog();
        return;
    }
    EditorListItem it;
    it.path = path;
    it.label = omsi::path_utf8(path.filename());
    items.push_back(std::move(it));
    if (want_spline) catalog_sli_sel_ = static_cast<int>(items.size()) - 1;
    else catalog_obj_sel_ = static_cast<int>(items.size()) - 1;
    refill_catalog();
}

bool EditorSidePanel::random_tile_stress_enabled() const {
    return o_chk_random_ && SendMessageW(o_chk_random_, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void EditorSidePanel::show_object(const EditorObjectProps& props) {
    if (!props.valid) return;
    EditorObjectProps p = props;
    if (p.text_slot_count <= 0) {
        if (!p.strings.empty()) p.text_slot_count = static_cast<int>(p.strings.size());
        else if (!p.string_labels.empty())
            p.text_slot_count = static_cast<int>(p.string_labels.size());
    }
    while (static_cast<int>(p.strings.size()) < p.text_slot_count) p.strings.emplace_back();
    last_object_ = p;
    imgui_object_ = p;
    imgui_state_.parent_id_field = p.varparent_id;
    imgui_state_.attach_point_field = p.attach_point;
    if (o_name_) SetWindowTextW(o_name_, to_wide(p.sco_name).c_str());
    set_edit_float(o_x_, p.x);
    set_edit_float(o_y_, p.y);
    set_edit_float(o_z_, p.z);
    set_edit_float(o_rot_, p.rot);
    set_edit_float(o_pitch_, p.pitch);
    set_edit_float(o_bank_, p.bank);
    set_edit_float(o_parent_, static_cast<float>(p.varparent_id));
    set_edit_float(o_att_pnt_, static_cast<float>(p.attach_point));
    set_edit_float(o_dist_, p.spline_repeat_step);
    set_edit_float(o_range_, p.spline_repeat_range);
    if (o_count_lbl_) {
        wchar_t buf[48]{};
        if (p.attach_kind == EditorObjectProps::AttachKind::SplineAttach)
            swprintf_s(buf, L"Count (~): %d", p.spline_attach_count);
        else
            swprintf_s(buf, L"Count (~): —");
        SetWindowTextW(o_count_lbl_, buf);
    }
    if (o_chk_align_terr_)
        SendMessageW(o_chk_align_terr_, BM_SETCHECK, p.spline_align_terrain ? BST_CHECKED : BST_UNCHECKED, 0);
    if (o_chk_tangential_)
        SendMessageW(o_chk_tangential_, BM_SETCHECK, p.spline_tangential ? BST_CHECKED : BST_UNCHECKED, 0);
    std::string sel = "Object id=" + std::to_string(p.id) + " " + p.sco_name;
    if (p.attach_kind == EditorObjectProps::AttachKind::AttachObj)
        sel += " [attachObj child=" + std::to_string(p.id) + " padre=" + std::to_string(p.parent_id) +
               " punto=" + std::to_string(p.attach_point) + "]";
    else if (p.attach_kind == EditorObjectProps::AttachKind::SplineAttach)
        sel += " [splineAttach idx=" + std::to_string(p.spline_index) + "]";
    else if (p.parent_id != 0)
        sel += " parent=" + std::to_string(p.parent_id);
    if (p.varparent_id != 0)
        sel += " [varparent=" + std::to_string(p.varparent_id) + "]";
    set_selection_text(sel);
    for (std::size_t i = 0; i < object_list_.size(); ++i) {
        if (object_list_[i].id != p.id) continue;
        if (o_instances_) {
            SendMessageW(o_instances_, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
            SendMessageW(o_instances_, LB_SETTOPINDEX, static_cast<WPARAM>(i), 0);
        }
        break;
    }
    update_object_text_slots(p);
}

void EditorSidePanel::update_object_live_values(const EditorObjectProps& props) {
    if (!props.valid) return;
    last_object_.x = props.x;
    last_object_.y = props.y;
    last_object_.z = props.z;
    last_object_.rot = props.rot;
    last_object_.pitch = props.pitch;
    last_object_.bank = props.bank;
    imgui_object_.x = props.x;
    imgui_object_.y = props.y;
    imgui_object_.z = props.z;
    imgui_object_.rot = props.rot;
    imgui_object_.pitch = props.pitch;
    imgui_object_.bank = props.bank;
    set_edit_float(o_x_, props.x);
    set_edit_float(o_y_, props.y);
    set_edit_float(o_z_, props.z);
    set_edit_float(o_rot_, props.rot);
    set_edit_float(o_pitch_, props.pitch);
    set_edit_float(o_bank_, props.bank);
}

bool EditorSidePanel::object_spline_tangential() const {
    if (imgui_primary_) return imgui_object_.spline_tangential;
    return o_chk_tangential_ && SendMessageW(o_chk_tangential_, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void EditorSidePanel::update_object_text_slots(const EditorObjectProps& props) {
    const bool show = props.valid && props.text_slot_count > 0;
    const int rows = show ? (std::min)(props.text_slot_count, kMaxSplineTextSlots) : 0;
    const int slots_h = rows > 0 ? (18 + rows * 22 + 8) : 0;
    if (o_btn_strings_) ShowWindow(o_btn_strings_, show ? SW_HIDE : SW_SHOW);
    if (o_slots_grp_) {
        if (show) {
            wchar_t cap[48]{};
            swprintf_s(cap, L"Extra lines (%d)", props.text_slot_count);
            SetWindowTextW(o_slots_grp_, cap);
            SetWindowPos(o_slots_grp_, nullptr, 8, o_slots_base_y_, 256, slots_h, SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(o_slots_grp_, SW_SHOW);
        } else {
            ShowWindow(o_slots_grp_, SW_HIDE);
        }
    }
    if (o_grp_ && o_obj_grp_y_ > 0) {
        const int gh = o_obj_grp_base_h_ + (show ? slots_h + 4 : 0);
        SetWindowPos(o_grp_, nullptr, 4, o_obj_grp_y_, 272, gh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (int i = 0; i < kMaxSplineTextSlots; ++i) {
        const bool row = show && i < props.text_slot_count;
        if (o_slot_lbl_[i]) ShowWindow(o_slot_lbl_[i], row ? SW_SHOW : SW_HIDE);
        if (o_slot_edits_[i]) {
            ShowWindow(o_slot_edits_[i], row ? SW_SHOW : SW_HIDE);
            if (row) {
                std::string val;
                if (static_cast<std::size_t>(i) < props.strings.size()) val = props.strings[static_cast<std::size_t>(i)];
                SetWindowTextW(o_slot_edits_[i], to_wide(val).c_str());
            }
        }
    }
    if (o_object_tail_y_ > 0) {
        const int extra = show ? slots_h + 4 : 0;
        const int tail_y = o_object_tail_y_ + extra;
        auto repos = [](HWND w, const int x, const int y, const int cw, const int ch) {
            if (w) SetWindowPos(w, nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
        };
        repos(o_btn_autorot_, 8, tail_y, 90, 22);
        repos(o_btn_parent_, 104, tail_y, 90, 22);
        repos(o_parent_, 200, tail_y, 50, 22);
        repos(o_btn_strings_, 8, tail_y + 28, 90, 24);
        repos(o_obj_search_lbl_, 8, tail_y + 58, 160, 16);
        repos(o_inst_search_, 8, tail_y + 74, 260, 20);
        repos(o_obj_list_lbl_, 8, tail_y + 98, 120, 16);
        repos(o_instances_, 8, tail_y + 114, 260, 70);
        if (page_object_) {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_RANGE;
            si.nMin = 0;
            si.nMax = tail_y + 184;
            SetScrollInfo(page_object_, SB_VERT, &si, TRUE);
        }
    }
}

void EditorSidePanel::update_spline_live_values(const EditorSplineProps& props) {
    if (!props.valid) return;
    last_spline_ = props;
    if (track_smooth_active_) return;
    suppress_grad_notify_ = true;
    set_edit_float(s_z_, props.z);
    set_spline_scalar_tracks(props);
    suppress_grad_notify_ = false;
}

void EditorSidePanel::show_spline_fields(const EditorSplineProps& props) {
    if (!props.valid) return;
    last_spline_ = props;
    imgui_spline_ = props;
    spline_grad_undo_pending_ = false;
    grad_edit_armed_ = true;
    suppress_grad_notify_ = true;
    set_edit_float(s_z_, props.z);
    set_spline_scalar_tracks(props);
    set_edit_float(s_dh_, props.delta_h);
    suppress_grad_notify_ = false;
    if (s_dh_check_) SendMessageW(s_dh_check_, BM_SETCHECK, props.use_delta_h ? BST_CHECKED : BST_UNCHECKED, 0);
    if (s_mirror_) SendMessageW(s_mirror_, BM_SETCHECK, props.mirror ? BST_CHECKED : BST_UNCHECKED, 0);
    float ang = 0.f;
    if (std::fabs(props.radius) > 1.e-4f) ang = (props.length / props.radius) * (180.f / 3.14159265f);
    if (s_angle_) {
        wchar_t buf[48]{};
        swprintf_s(buf, L"%.3g", static_cast<double>(ang));
        SetWindowTextW(s_angle_, buf);
    }
}

void EditorSidePanel::show_spline(const EditorSplineProps& props) {
    if (!props.valid) return;
    show_spline_fields(props);
    set_selection_text(std::string(props.is_spline_h ? "spline_h" : "spline") + " id=" + std::to_string(props.id) +
                       " " + props.sli_name);
    if (!s_instances_) return;
    for (std::size_t li = 0; li < filtered_spline_indices_.size(); ++li) {
        const int mi = filtered_spline_indices_[li];
        if (mi < 0 || static_cast<std::size_t>(mi) >= spline_list_.size()) continue;
        if (spline_list_[static_cast<std::size_t>(mi)].id != props.id) continue;
        SendMessageW(s_instances_, LB_SETCURSEL, static_cast<WPARAM>(li), 0);
        SendMessageW(s_instances_, LB_SETTOPINDEX, static_cast<WPARAM>(li), 0);
        break;
    }
}

void EditorSidePanel::set_spline_track_atten_y(float world_y) { spline_track_atten_y_ = world_y; }

} // namespace omsi::ui
