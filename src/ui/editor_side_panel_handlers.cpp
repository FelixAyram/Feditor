#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui/editor_side_panel.hpp"
#include "ui/editor_side_panel_ids.hpp"

#include "omsi/logger.hpp"
#include "omsi/utf8_win32.hpp"
#include "omsi/map_geo_tools.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <random>
#include <string>
#include "omsi/tt_man.hpp"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace omsi::ui {
using namespace editor_side_panel_ids;
void EditorSidePanel::show_path_rule(const EditorPathRuleProps& props) {
    const int keep_brush = imgui_path_rule_.traffic_brush;
    const int keep_uvg = imgui_path_rule_.density_uvg;
    last_path_rule_ = props;
    imgui_path_rule_ = props;
    imgui_path_rule_.traffic_brush = keep_brush;
    if (keep_uvg >= 0) imgui_path_rule_.density_uvg = keep_uvg;
    set_tool(EditorTool::TrafficRules);
    if (t_speed_ && props.has_speed) set_edit_float(t_speed_, props.speed_kmh);
    if (t_hint_) SetWindowTextW(t_hint_, to_wide(props.label).c_str());
}

void EditorSidePanel::hide_path_rule() {
    last_path_rule_ = {};
    imgui_path_rule_ = {};
}

void EditorSidePanel::read_object_edits(EditorObjectProps& out) const {
    if (imgui_primary_) {
        out = imgui_object_;
        out.valid = imgui_object_.valid;
        out.varparent_id = imgui_state_.parent_id_field;
        out.attach_point = imgui_state_.attach_point_field;
        return;
    }
    out = last_object_;
    out.valid = true;
    out.x = get_edit_float(o_x_, out.x);
    out.y = get_edit_float(o_y_, out.y);
    out.z = get_edit_float(o_z_, out.z);
    out.rot = get_edit_float(o_rot_, out.rot);
    out.pitch = get_edit_float(o_pitch_, out.pitch);
    out.bank = get_edit_float(o_bank_, out.bank);
    out.varparent_id = static_cast<std::int32_t>(get_edit_float(o_parent_, static_cast<float>(out.varparent_id)));
    out.spline_repeat_step = get_edit_float(o_dist_, out.spline_repeat_step);
    out.spline_repeat_range = get_edit_float(o_range_, out.spline_repeat_range);
    out.spline_tangential =
        o_chk_tangential_ && SendMessageW(o_chk_tangential_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.spline_align_terrain =
        o_chk_align_terr_ && SendMessageW(o_chk_align_terr_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (out.text_slot_count > 0) {
        const int n = (std::min)(out.text_slot_count, kMaxSplineTextSlots);
        out.strings.assign(static_cast<std::size_t>(n), std::string{});
        for (int i = 0; i < n; ++i) {
            if (!o_slot_edits_[i]) continue;
            const std::wstring w = edit_text(o_slot_edits_[i]);
            out.strings[static_cast<std::size_t>(i)] = wide_to_utf8(w);
        }
    }
}

void EditorSidePanel::read_spline_edits(EditorSplineProps& out) const {
    if (imgui_primary_) {
        out = imgui_spline_;
        out.valid = imgui_spline_.valid;
        return;
    }
    out = last_spline_;
    out.valid = true;
    out.z = get_edit_float(s_z_, out.z);
    out.rot = get_edit_float(s_rot_, out.rot);
    out.skew_start = get_edit_float(s_skew0_, out.skew_start);
    out.skew_end = get_edit_float(s_skew1_, out.skew_end);
    out.length = get_edit_float(s_len_, out.length);
    out.radius = get_edit_float(s_radius_, out.radius);
    out.start_grad = get_grad_value(s_g0_, s_g0_edit_, out.start_grad);
    out.end_grad = get_grad_value(s_g1_, s_g1_edit_, out.end_grad);
    out.delta_h = get_edit_float(s_dh_, out.delta_h);
    out.cant_start = get_edit_float(s_cant0_, out.cant_start);
    out.cant_end = get_edit_float(s_cant1_, out.cant_end);
    out.use_delta_h = s_dh_check_ && SendMessageW(s_dh_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.mirror = s_mirror_ && SendMessageW(s_mirror_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    out.is_spline_h = out.use_delta_h;
}

void EditorSidePanel::read_traffic_rule_edits(EditorPathRuleProps& out) const {
    if (imgui_primary_) {
        out = imgui_path_rule_;
        out.valid = imgui_path_rule_.valid;
        return;
    }
    out = last_path_rule_;
    out.valid = last_path_rule_.valid;
    out.density_uvg = 0;
    if (t_uvg_list_) {
        const int sel = static_cast<int>(SendMessageW(t_uvg_list_, LB_GETCURSEL, 0, 0));
        if (sel >= 0) out.density_uvg = sel;
    }
    out.has_speed = out.has_priority = out.no_cars = out.buses = out.trucks = out.no_overtake = false;
    out.crossed_out = false;
    out.density = 1.f;
    int chosen = 6;
    for (std::size_t i = 0; i < t_radios_.size(); ++i) {
        if (t_radios_[i] && SendMessageW(t_radios_[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            chosen = static_cast<int>(i);
            break;
        }
    }
    out.traffic_brush = chosen;
    switch (static_cast<RuleRadio>(chosen)) {
    case RuleRadio::SpeedLimit:
        out.has_speed = true;
        out.speed_kmh = get_edit_float(t_speed_, 30.f);
        break;
    case RuleRadio::OvertakeProhib:
        out.no_overtake = true;
        break;
    case RuleRadio::NoUnsched:
        out.crossed_out = true;
        out.density = 0.f;
        break;
    case RuleRadio::DenExtremeLow:
        out.density = 0.1f;
        break;
    case RuleRadio::DenVLow:
        out.density = 0.25f;
        break;
    case RuleRadio::DenLow:
        out.density = 0.5f;
        break;
    case RuleRadio::DenNormal:
        out.density = 1.f;
        break;
    case RuleRadio::DenHigh:
        out.density = 1.5f;
        break;
    case RuleRadio::HigherPrio:
        out.has_priority = true;
        out.priority = 160.f;
        break;
    case RuleRadio::LowerPrio:
        out.has_priority = true;
        out.priority = 96.f;
        break;
    case RuleRadio::CarsProhib:
        out.no_cars = true;
        break;
    case RuleRadio::TrucksAllowed:
        out.trucks = true;
        break;
    case RuleRadio::BusesCabs:
        out.buses = true;
        break;
    default:
        break;
    }
}

void EditorSidePanel::pump() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - track_smooth_last_tick_).count();
    track_smooth_last_tick_ = now;
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    if (track_smooth_active_) tick_spline_track_smooth((std::min)(dt, 0.05f));
    const auto t1 = clk::now();
    layout_children();
    const auto t2 = clk::now();
    // keep the toolbar and the panel above the 3D view: twice a second is enough (every frame it
    // reshuffled the z-order and made Windows recompose the window each time)
    if (now - z_order_last_ > std::chrono::milliseconds(500)) {
        z_order_last_ = now;
        // BringWindowToTop on a child ACTIVATES its top-level parent: every 500 ms focus/activation
        // messages (also cross-process, to the app that had the focus) were processed inside the next
        // PeekMessage (25-35 ms) and the editor stole the focus. Reorder only when another sibling
        // really is above ours, and never activate.
        const HWND parent = hwnd_ ? GetParent(hwnd_) : (toolbar_ ? GetParent(toolbar_) : nullptr);
        bool foreign_above = false;
        int ours_seen = 0;
        const int ours_total = (hwnd_ && IsWindowVisible(hwnd_) ? 1 : 0) + (toolbar_ && IsWindowVisible(toolbar_) ? 1 : 0);
        for (HWND c = parent ? GetWindow(parent, GW_CHILD) : nullptr; c && ours_seen < ours_total;
             c = GetWindow(c, GW_HWNDNEXT)) {
            if (!IsWindowVisible(c)) continue;
            if (c == hwnd_ || c == toolbar_) {
                ++ours_seen;
                continue;
            }
            foreign_above = true;
            break;
        }
        if (foreign_above) {
            constexpr UINT kFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
            if (toolbar_) SetWindowPos(toolbar_, HWND_TOP, 0, 0, 0, 0, kFlags);
            if (hwnd_) SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, kFlags);
        }
    }
    const auto t3 = clk::now();
    MSG msg{};
    int n = 0;
    // empty queue: no PeekMessage (it may give the time slice away to the loader threads)
    while (HIWORD(GetQueueStatus(QS_ALLINPUT)) != 0 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (++n >= 256) break;
    }
    const auto t4 = clk::now();
    auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    static auto s_log_t = clk::time_point{};
    if (ms(t0, t4) > 8.0 && t4 - s_log_t > std::chrono::milliseconds(500)) {
        s_log_t = t4;
        omsi::log::info("UI_PANEL_SLOW ms=" + std::to_string(ms(t0, t4)) + " smooth=" + std::to_string(ms(t0, t1)) +
                        " layout=" + std::to_string(ms(t1, t2)) + " zorder=" + std::to_string(ms(t2, t3)) +
                        " msgs=" + std::to_string(n) + " msgs_ms=" + std::to_string(ms(t3, t4)));
    }
}

EditorSidePanelState EditorSidePanel::drain_catalog_pending() {
    EditorSidePanelState out{};
    out.tool = tool_;
    auto take = [&](bool& dst, bool& src) {
        if (src) {
            dst = true;
            src = false;
        }
    };
    take(out.load_catalog_file, pending_.load_catalog_file);
    take(out.load_catalog_directory, pending_.load_catalog_directory);
    take(out.spawn_random_scan_folder, pending_.spawn_random_scan_folder);
    take(out.open_catalog_folder, pending_.open_catalog_folder);
    take(out.edit_catalog_file, pending_.edit_catalog_file);
    take(out.reload_catalog_file, pending_.reload_catalog_file);
    return out;
}

EditorSidePanelState EditorSidePanel::take_events() {
    EditorSidePanelState out = pending_;
    // The .sco pick flag is a persistent mode flag; keep it across the per-frame reset.
    const bool keep_pick = pending_.sco_path_pick_pending;
    pending_ = {};
    pending_.tool = tool_;
    pending_.sco_path_pick_pending = keep_pick;
    out.tool = tool_;
    if (out.apply_spline && spline_grad_undo_pending_) {
        out.undo_before_spline = true;
        spline_grad_undo_pending_ = false;
        grad_edit_armed_ = true;
    }
    if (out.apply_object && object_edit_undo_pending_) {
        out.undo_before_object = true;
        object_edit_undo_pending_ = false;
        object_edit_armed_ = true;
    }
    if (imgui_primary_) {
        out.show_spline_ends = imgui_state_.show_spline_ends;
        out.show_path_ends = imgui_state_.show_path_ends;
        out.show_path_rails = imgui_state_.show_path_rails;
        out.autofill_spline_ends = imgui_state_.autofill_spline_ends;
        out.tex_max_256 = imgui_state_.tex_max_256;
    } else {
        out.show_spline_ends =
            !s_chk_ends_ || SendMessageW(s_chk_ends_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        out.show_path_ends =
            !s_chk_path_ends_ || SendMessageW(s_chk_path_ends_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        out.show_path_rails =
            (tool_ == EditorTool::TrafficRules) ||
            (s_chk_path_rails_ && SendMessageW(s_chk_path_rails_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        out.autofill_spline_ends =
            s_chk_autofill_ && SendMessageW(s_chk_autofill_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (catalog_object_sel() >= 0) out.select_catalog_object = catalog_object_sel();
    if (catalog_spline_sel() >= 0) out.select_catalog_spline = catalog_spline_sel();
    return out;
}

bool EditorSidePanel::pointer_over_panel() const {
    if (imgui_primary_) return false;
    POINT pt{};
    GetCursorPos(&pt);
    if (toolbar_ && IsWindowVisible(toolbar_)) {
        RECT rc{};
        GetWindowRect(toolbar_, &rc);
        if (PtInRect(&rc, pt) != 0) return true;
    }
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return false;
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    return PtInRect(&rc, pt) != 0;
}

bool EditorSidePanel::keyboard_captured() const {
    if (imgui_primary_) return false;
    const HWND f = GetFocus();
    if (!f || !hwnd_) return false;
    wchar_t cls[64]{};
    GetClassNameW(f, cls, 64);
    if (_wcsicmp(cls, L"Edit") != 0 && _wcsicmp(cls, L"ComboBox") != 0) return false;
    return IsChild(hwnd_, f) != FALSE;
}

int EditorSidePanel::catalog_object_sel() const { return catalog_obj_sel_; }
int EditorSidePanel::catalog_spline_sel() const { return catalog_sli_sel_; }

std::filesystem::path EditorSidePanel::catalog_selected_path() const {
    const bool spline_tab = tool_ == EditorTool::Spline;
    const int sel = spline_tab ? catalog_sli_sel_ : catalog_obj_sel_;
    if (sel < 0) return {};
    const auto& items = spline_tab ? catalog_splines_ : catalog_objects_;
    if (static_cast<std::size_t>(sel) >= items.size()) return {};
    const auto& path = items[static_cast<std::size_t>(sel)].path;
    const auto ext = path.extension().wstring();
    if (spline_tab) {
        if (_wcsicmp(ext.c_str(), L".sli") != 0) return {};
    } else if (_wcsicmp(ext.c_str(), L".sco") != 0) {
        return {};
    }
    return path;
}

std::filesystem::path EditorSidePanel::pick_spawn_random_sco_path() {
    auto& st = imgui_state_;
    std::vector<std::filesystem::path> pool;
    pool.reserve(64);
    const auto is_sco = [](const std::filesystem::path& p) {
        if (p.empty()) return false;
        const auto ext = p.extension().wstring();
        return _wcsicmp(ext.c_str(), L".sco") == 0;
    };
    if (st.spawn_random_mode == 1) {
        if (!st.fav_cats.empty()) {
            const int ci = (std::clamp)(st.spawn_random_fav_cat, 0, static_cast<int>(st.fav_cats.size()) - 1);
            for (const auto& it : st.fav_cats[static_cast<std::size_t>(ci)].items) {
                if (it.spline || !is_sco(it.path)) continue;
                pool.push_back(it.path);
            }
        }
    } else if (st.spawn_random_mode == 2) {
        for (const auto& p : st.spawn_random_folder_pool) {
            if (is_sco(p)) pool.push_back(p);
        }
    } else {
        for (const auto& it : catalog_objects_) {
            if (is_sco(it.path)) pool.push_back(it.path);
        }
    }
    if (pool.empty()) return {};
    std::mt19937 rng(++st.spawn_random_seed);
    std::uniform_int_distribution<std::size_t> dist(0, pool.size() - 1);
    return pool[dist(rng)];
}

LRESULT CALLBACK EditorSidePanel::toolbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<EditorSidePanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_COMMAND && self) {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (code == BN_CLICKED && id == IDC_BTN_TOOLBAR_MENU) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDC_BTN_LOAD_MAP, L"Load map (global.cfg)...");
            AppendMenuW(menu, MF_STRING, IDC_BTN_OPEN_TILE, L"Open tile .map...");
            AppendMenuW(menu, MF_STRING, IDC_BTN_SHORTCUTS, L"Shortcuts...");
            HWND btn = GetDlgItem(hwnd, IDC_BTN_TOOLBAR_MENU);
            RECT btn_rc{};
            if (btn) GetWindowRect(btn, &btn_rc);
            TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, btn_rc.left, btn_rc.bottom, 0, hwnd,
                           nullptr);
            DestroyMenu(menu);
            return 0;
        }
        if (id == IDC_BTN_LOAD_MAP) {
            self->pending_.request_load_map = true;
            return 0;
        }
        if (id == IDC_BTN_OPEN_TILE) {
            self->pending_.request_open_tile_map = true;
            return 0;
        }
        if (id == IDC_BTN_SHORTCUTS) {
            self->pending_.show_shortcuts_dialog = true;
            return 0;
        }
        if (id == IDC_BTN_ENTRYPOINTS) {
            self->pending_.open_entrypoint_picker = true;
            return 0;
        }
        if (id == IDC_BTN_OPTIONS) {
            self->pending_.open_options_dialog = true;
            return 0;
        }
        return SendMessageW(self->hwnd_, WM_COMMAND, wp, lp);
    }
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, GetSysColorBrush(COLOR_3DFACE));
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK EditorSidePanel::page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<EditorSidePanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_COMMAND && self) {
        return SendMessageW(self->hwnd_, msg, wp, lp);
    }
    if (msg == WM_HSCROLL && self) {
        // Trackbar is a child of the page — forward so resolution label updates.
        return SendMessageW(self->hwnd_, msg, wp, lp);
    }
    if (msg == WM_NOTIFY && self) {
        return SendMessageW(self->hwnd_, msg, wp, lp);
    }
    if (msg == WM_VSCROLL) {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        const int old = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP:
            si.nPos -= 24;
            break;
        case SB_LINEDOWN:
            si.nPos += 24;
            break;
        case SB_PAGEUP:
            si.nPos -= static_cast<int>(si.nPage);
            break;
        case SB_PAGEDOWN:
            si.nPos += static_cast<int>(si.nPage);
            break;
        case SB_THUMBTRACK:
            si.nPos = si.nTrackPos;
            break;
        default:
            break;
        }
        si.fMask = SIF_POS;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        GetScrollInfo(hwnd, SB_VERT, &si);
        const int dy = old - si.nPos;
        if (dy != 0)
            ScrollWindowEx(hwnd, 0, dy, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        const WPARAM sb = delta > 0 ? SB_LINEUP : SB_LINEDOWN;
        SendMessageW(hwnd, WM_VSCROLL, sb, 0);
        if (delta > WHEEL_DELTA || delta < -WHEEL_DELTA) SendMessageW(hwnd, WM_VSCROLL, sb, 0);
        return 0;
    }
    if (msg == WM_SIZE) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_PAGE;
        GetScrollInfo(hwnd, SB_VERT, &si);
        si.nPage = static_cast<UINT>((std::max)(1, static_cast<int>(rc.bottom)));
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        return 0;
    }
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, self && self->bg_brush_ ? self->bg_brush_
                                                                        : GetSysColorBrush(COLOR_3DFACE));
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EditorSidePanel::try_apply_on_enter() {
    const HWND f = GetFocus();
    if (!f) return false;
    if (tool_ == EditorTool::Object) {
        if (f == o_x_ || f == o_y_ || f == o_z_ || f == o_rot_ || f == o_pitch_ || f == o_bank_ ||
            f == o_parent_ || f == o_dist_ || f == o_range_) {
            pending_.apply_object = true;
            read_object_edits(pending_.object_edit);
            return true;
        }
        if (last_object_.text_slot_count > 0) {
            for (int i = 0; i < kMaxSplineTextSlots; ++i) {
                if (f == o_slot_edits_[i]) {
                    pending_.apply_object = true;
                    read_object_edits(pending_.object_edit);
                    return true;
                }
            }
        }
    }
    if (tool_ == EditorTool::Spline) {
        if (f == s_z_ || f == s_rot_ || f == s_skew0_ || f == s_skew1_ || f == s_len_ || f == s_radius_ ||
            f == s_g0_edit_ || f == s_g1_edit_ || f == s_dh_ || f == s_cant0_ || f == s_cant1_ ||
            f == s_max_radius_) {
            arm_spline_grad_undo();
            pending_.apply_spline = true;
            read_spline_edits(pending_.spline_edit);
            return true;
        }
    }
    if (tool_ == EditorTool::TrafficRules) {
        if (f == t_speed_) {
            read_traffic_rule_edits(pending_.path_rule_edit);
            pending_.path_rule_edit.valid = last_path_rule_.valid;
            pending_.apply_path_rule = true;
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK EditorSidePanel::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<EditorSidePanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        self->layout_children();
        return 0;
    case WM_KEYDOWN:
        if (self && wp == VK_RETURN && self->try_apply_on_enter()) return 0;
        break;
    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (di && di->CtlID == IDC_TAB_STRIP) {
            self->paint_tab_strip(di->hDC);
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (x < kTabStripW) {
            const int tab = self->hit_tab(x, y);
            if (tab >= 0) {
                self->set_tool(static_cast<EditorTool>(tab));
                if (tab == static_cast<int>(EditorTool::TrafficRules)) {
                    self->pending_.toggled_markers = true;
                    if (self->s_chk_path_rails_)
                        SendMessageW(self->s_chk_path_rails_, BM_SETCHECK, BST_CHECKED, 0);
                }
            }
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<LPNMHDR>(lp);
        if (nm && nm->code == TRBN_THUMBPOSCHANGING && self->is_spline_smooth_track(nm->hwndFrom)) {
            auto* ch = reinterpret_cast<NMTRBTHUMBPOSCHANGING*>(lp);
            if (self->handle_spline_track_thumb_changing(nm->hwndFrom, static_cast<int>(ch->dwPos), ch->nReason))
                return TRUE;
        }
        break;
    }
    case WM_HSCROLL: {
        if (self->t_aerial_res_ && reinterpret_cast<HWND>(lp) == self->t_aerial_res_) {
            const int pos = static_cast<int>(SendMessageW(self->t_aerial_res_, TBM_GETPOS, 0, 0));
            if (self->t_aerial_res_lbl_) {
                wchar_t buf[64]{};
                swprintf_s(buf, L"Resolution: %d px", map::aerial_resolution_label_px(pos));
                SetWindowTextW(self->t_aerial_res_lbl_, buf);
            }
            self->pending_.aerial_resolution_pos = pos;
            return 0;
        }
        if (!self->suppress_grad_notify_ && self->s_g0_ && reinterpret_cast<HWND>(lp) == self->s_g0_) {
            self->handle_spline_track_hscroll(self->s_g0_, wp);
            return 0;
        }
        if (!self->suppress_grad_notify_ && self->s_g1_ && reinterpret_cast<HWND>(lp) == self->s_g1_) {
            self->handle_spline_track_hscroll(self->s_g1_, wp);
            return 0;
        }
        if (!self->suppress_grad_notify_ && self->s_rot_trk_ &&
            reinterpret_cast<HWND>(lp) == self->s_rot_trk_) {
            self->handle_spline_track_hscroll(self->s_rot_trk_, wp);
            return 0;
        }
        if (!self->suppress_grad_notify_ && self->s_len_trk_ &&
            reinterpret_cast<HWND>(lp) == self->s_len_trk_) {
            self->handle_spline_track_hscroll(self->s_len_trk_, wp);
            return 0;
        }
        if (!self->suppress_grad_notify_ && self->s_rad_trk_ &&
            reinterpret_cast<HWND>(lp) == self->s_rad_trk_) {
            self->handle_spline_track_hscroll(self->s_rad_trk_, wp);
            return 0;
        }
        if (!self->suppress_grad_notify_ && self->s_skew0_trk_ &&
            reinterpret_cast<HWND>(lp) == self->s_skew0_trk_) {
            self->handle_spline_track_hscroll(self->s_skew0_trk_, wp);
            return 0;
        }
        if (!self->suppress_grad_notify_ && self->s_skew1_trk_ &&
            reinterpret_cast<HWND>(lp) == self->s_skew1_trk_) {
            self->handle_spline_track_hscroll(self->s_skew1_trk_, wp);
            return 0;
        }
        if (!self->suppress_grad_notify_ && self->s_cant0_trk_ &&
            reinterpret_cast<HWND>(lp) == self->s_cant0_trk_) {
            self->handle_spline_track_hscroll(self->s_cant0_trk_, wp);
            return 0;
        }
        if (!self->suppress_grad_notify_ && self->s_cant1_trk_ &&
            reinterpret_cast<HWND>(lp) == self->s_cant1_trk_) {
            self->handle_spline_track_hscroll(self->s_cant1_trk_, wp);
            return 0;
        }
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (code == LBN_SELCHANGE) {
            if (id == IDC_O_CATALOG) {
                const int sel = static_cast<int>(SendMessageW(self->o_catalog_, LB_GETCURSEL, 0, 0));
                self->catalog_obj_sel_ = self->filtered_catalog_object_index(sel);
                self->pending_.select_catalog_object = self->catalog_obj_sel_;
            } else if (id == IDC_S_CATALOG) {
                const int sel = static_cast<int>(SendMessageW(self->s_catalog_, LB_GETCURSEL, 0, 0));
                self->catalog_sli_sel_ = self->filtered_catalog_spline_index(sel);
                self->pending_.select_catalog_spline = self->catalog_sli_sel_;
            } else if (id == IDC_O_INST) {
                const int sel = static_cast<int>(SendMessageW(self->o_instances_, LB_GETCURSEL, 0, 0));
                self->pending_.select_object_index = self->filtered_object_index(sel);
                self->pending_.select_object_id = 0;
                for (const auto& it : self->object_list_) {
                    if (it.map_index == self->pending_.select_object_index) {
                        self->pending_.select_object_id = it.id;
                        break;
                    }
                }
            } else if (id == IDC_S_INST) {
                const int sel = static_cast<int>(SendMessageW(self->s_instances_, LB_GETCURSEL, 0, 0));
                self->pending_.select_spline_index = self->filtered_spline_index(sel);
                self->pending_.select_spline_id = 0;
                for (const auto& it : self->spline_list_) {
                    if (it.map_index == self->pending_.select_spline_index) {
                        self->pending_.select_spline_id = it.id;
                        break;
                    }
                }
            } else if (id == IDC_TE_LAYERS) {
                self->ground_layer_sel_ = static_cast<int>(SendMessageW(self->terr_layers_, LB_GETCURSEL, 0, 0));
                self->pending_.ground_layer_sel = self->ground_layer_sel_;
                self->pending_.save_ui_settings = true;
            } else if (id == IDC_T_CHRONO_LIST) {
                self->pending_.chrono_sel = static_cast<int>(SendMessageW(self->chrono_list_, LB_GETCURSEL, 0, 0));
            } else if (id == IDC_TT_TRACKS) {
                // Unit134 ListBox1Click
                const int sel = static_cast<int>(SendMessageW(self->tracks_list_, LB_GETCURSEL, 0, 0));
                self->pending_.tt_track_entry_list_index = sel;
                self->pending_.tt_track_entry_sel_changed = true;
                self->pending_.tt_request_camera_jump = true;
                auto& man = omsi::tt::global_tt_man();
                man.on_track_entry_list_click(sel);
            } else if (id == IDC_TT_TRIPS) {
                // Unit134 ListBox2Click
                const int sel = static_cast<int>(SendMessageW(self->trips_list_, LB_GETCURSEL, 0, 0));
                self->pending_.tt_station_list_index = sel;
                self->pending_.tt_station_sel_changed = true;
                self->pending_.tt_request_camera_jump = true;
                auto& man = omsi::tt::global_tt_man();
                man.on_trip_station_list_click(sel);
                // Refresh track-entry list selection (OMSI also selects linked entry)
                self->set_tt_from_man();
            }
        } else if (code == CBN_SELCHANGE) {
            if (id == IDC_TT_TRACK_COMBO) {
                const int sel = static_cast<int>(SendMessageW(self->tracks_combo_, CB_GETCURSEL, 0, 0));
                self->pending_.tt_track_combo_index = sel;
                self->pending_.tt_track_combo_changed = true;
                auto& man = omsi::tt::global_tt_man();
                man.selected_track = sel;
                man.selected_track_entry = -1;
                self->set_tt_from_man();
            } else if (id == IDC_TT_TRIP_COMBO) {
                const int sel = static_cast<int>(SendMessageW(self->trips_combo_, CB_GETCURSEL, 0, 0));
                self->pending_.tt_trip_combo_index = sel;
                self->pending_.tt_trip_combo_changed = true;
                auto& man = omsi::tt::global_tt_man();
                man.selected_trip = sel;
                man.selected_trip_station = -1;
                self->set_tt_from_man();
            }
        } else if (code == EN_CHANGE) {
            if (!self->suppress_grad_notify_ && id == IDC_S_G0_EDIT) {
                self->arm_spline_grad_undo();
                self->sync_grad_edit_to_track(self->s_g0_, self->s_g0_edit_);
                self->read_spline_edits(self->pending_.spline_edit);
                self->pending_.spline_grad_preview = true;
            } else if (!self->suppress_grad_notify_ && id == IDC_S_G1_EDIT) {
                self->arm_spline_grad_undo();
                self->sync_grad_edit_to_track(self->s_g1_, self->s_g1_edit_);
                self->read_spline_edits(self->pending_.spline_edit);
                self->pending_.spline_grad_preview = true;
            } else if (!self->suppress_grad_notify_ &&
                       (id == IDC_S_ROT || id == IDC_S_LEN || id == IDC_S_RAD || id == IDC_S_SKEW0 ||
                        id == IDC_S_SKEW1 || id == IDC_S_CANT0 || id == IDC_S_CANT1)) {
                self->arm_spline_grad_undo();
                self->sync_spline_scalar_edit(id);
                self->read_spline_edits(self->pending_.spline_edit);
                self->pending_.spline_grad_preview = true;
            } else if (id == IDC_O_TILE_SEARCH || id == IDC_O_INST_SEARCH || id == IDC_S_TILE_SEARCH ||
                id == IDC_S_INST_SEARCH || id == IDC_O_CAT_SEARCH || id == IDC_S_CAT_SEARCH) {
                if (id == IDC_O_CAT_SEARCH || id == IDC_S_CAT_SEARCH)
                    self->refill_catalog();
                else
                    self->refill_instances();
            }
        } else if (code == BN_CLICKED) {
            if (id == IDC_BTN_LOAD_MAP) {
                self->pending_.request_load_map = true;
            } else if (id == IDC_BTN_OPEN_TILE) {
                self->pending_.request_open_tile_map = true;
            } else if (id == IDC_BTN_SHORTCUTS) {
                self->pending_.show_shortcuts_dialog = true;
            } else if (id == IDC_O_APPLY) {
                self->pending_.apply_object = true;
                self->read_object_edits(self->pending_.object_edit);
            } else if (id == IDC_S_G0_UP) {
                self->nudge_grad(self->s_g0_, self->s_g0_edit_, 0.1f);
            } else if (id == IDC_S_G0_DN) {
                self->nudge_grad(self->s_g0_, self->s_g0_edit_, -0.1f);
            } else if (id == IDC_S_G1_UP) {
                self->nudge_grad(self->s_g1_, self->s_g1_edit_, 0.1f);
            } else if (id == IDC_S_G1_DN) {
                self->nudge_grad(self->s_g1_, self->s_g1_edit_, -0.1f);
            } else if (id == IDC_S_Z_UP) {
                self->nudge_z(0.1f);
            } else if (id == IDC_S_Z_DN) {
                self->nudge_z(-0.1f);
            } else if (id >= IDC_S_ROT_UP && id <= IDC_S_CANT1_DN) {
                self->handle_spline_nudge_btn(id);
            } else if (id == IDC_S_APPLY) {
                self->arm_spline_grad_undo();
                self->pending_.apply_spline = true;
                self->read_spline_edits(self->pending_.spline_edit);
            } else if (id == IDC_S_MIRROR) {
                self->arm_spline_grad_undo();
                self->pending_.apply_spline = true;
                self->pending_.apply_spline_mirror_only = true;
                self->read_spline_edits(self->pending_.spline_edit);
            } else if (id == IDC_S_DH_CHK) {
                self->arm_spline_grad_undo();
                self->pending_.apply_spline = true;
                self->read_spline_edits(self->pending_.spline_edit);
            } else if (id == IDC_O_OPENMAP || id == IDC_S_OPENMAP) {
                self->pending_.open_instance_map = true;
            } else if (id == IDC_O_LOAD || id == IDC_S_LOAD) {
                self->pending_.load_catalog_file = true;
            } else if (id == IDC_S_CREATE) {
                self->pending_.open_spline_creator = true;
            } else if (id == IDC_O_LOADDIR) {
                self->pending_.load_catalog_directory = true;
            } else if (id == IDC_O_FOLDER || id == IDC_S_FOLDER) {
                self->pending_.open_catalog_folder = true;
            } else if (id == IDC_O_EDITFILE || id == IDC_S_EDITFILE) {
                self->pending_.edit_catalog_file = true;
            } else if (id == IDC_O_RELOAD || id == IDC_S_RELOAD) {
                self->pending_.reload_catalog_file = true;
            } else if (id == IDC_T_CREATE_TILE) {
                self->pending_.create_tile = true;
            } else if (id == IDC_T_DELETE_TILE) {
                self->pending_.delete_tile = true;
            } else if (id == IDC_O_STRINGS) {
                self->read_object_edits(self->pending_.object_edit);
                self->pending_.object_edit.valid = true;
                self->pending_.open_extra_lines_dialog = true;
            } else if (id == IDC_T_APPLY) {
                self->read_traffic_rule_edits(self->pending_.path_rule_edit);
                self->pending_.path_rule_edit.valid = self->last_path_rule_.valid;
                self->pending_.apply_path_rule = true;
                self->pending_.open_path_rule_dialog = false;
            } else if (id == IDC_S_ENDS || id == IDC_S_PATH_ENDS || id == IDC_S_PATH_RAILS ||
                       id == IDC_S_AUTOFILL) {
                self->pending_.toggled_markers = true;
                self->pending_.save_ui_settings = true;
            } else if (id == IDC_T_CONVERT) {
                self->read_tile_page_edits(self->pending_.tile_geo_edit);
                self->read_tile_page_flags(self->pending_.tile_aerial_visible, self->pending_.aerial_resolution_pos);
                self->pending_.convert_world_coords = true;
            } else if (id == IDC_T_AERIAL_DNL) {
                self->read_tile_page_edits(self->pending_.tile_geo_edit);
                self->read_tile_page_flags(self->pending_.tile_aerial_visible, self->pending_.aerial_resolution_pos);
                self->pending_.download_tile_aerial = true;
            } else if (id == IDC_T_AERIAL_DEL) {
                self->pending_.delete_tile_aerial = true;
            } else if (id == IDC_T_AERIAL_VIS) {
                self->read_tile_page_flags(self->pending_.tile_aerial_visible, self->pending_.aerial_resolution_pos);
                self->read_tile_page_edits(self->pending_.tile_geo_edit);
                self->pending_.tile_geo_edit.tile_aerial_visible = self->pending_.tile_aerial_visible;
            } else if (id == IDC_T_DTM) {
                self->read_tile_page_edits(self->pending_.tile_geo_edit);
                self->pending_.apply_tile_dtm = true;
            } else if (id == IDC_O_ATTACH) {
                self->pending_.attach_to_object = true;
                self->pending_.attach_point = self->attach_point();
            } else if (id == IDC_O_ATTACH_SLI) {
                self->pending_.attach_to_spline = true;
            } else if (id == IDC_O_PARENT) {
                self->pending_.parent_to_object = true;
            } else if (id == IDC_S_SPLIT) {
                self->pending_.spline_split = true;
            } else if (id == IDC_S_COMPLETE) {
                self->pending_.spline_complete = true;
                self->pending_.spline_max_radius = self->spline_max_radius();
            } else if (id == IDC_S_ALIGN_START) {
                self->pending_.spline_align_start = true;
            } else if (id == IDC_S_ALIGN_END) {
                self->pending_.spline_align_end = true;
            } else if (id == IDC_T_SPAWN_TERR || id == IDC_O_SPAWN_TERR || id == IDC_S_SPAWN_TERR ||
                       id == IDC_T_SPAWN_MESH || id == IDC_O_SPAWN_MESH || id == IDC_S_SPAWN_MESH ||
                       id == IDC_T_SPAWN_ALIGN || id == IDC_O_SPAWN_ALIGN || id == IDC_S_SPAWN_ALIGN) {
                self->on_spawn_height_clicked(id);
            } else if (id == IDC_T_LIGHTMAP) {
                self->pending_.create_lightmap = true;
            } else if (id == IDC_T_WHOLE_MAP) {
                self->pending_.do_whole_map = true;
            } else if (id == IDC_T_AERIAL_MISS) {
                self->read_tile_page_edits(self->pending_.tile_geo_edit);
                self->read_tile_page_flags(self->pending_.tile_aerial_visible, self->pending_.aerial_resolution_pos);
                self->pending_.aerial_missing = true;
            } else if (id == IDC_T_AERIAL_ALL) {
                self->read_tile_page_edits(self->pending_.tile_geo_edit);
                self->read_tile_page_flags(self->pending_.tile_aerial_visible, self->pending_.aerial_resolution_pos);
                self->pending_.aerial_all = true;
            } else if (id == IDC_T_CHRONO_PLUS) {
                self->pending_.chrono_plus = true;
                self->pending_.chrono_sel = self->chrono_sel();
            } else if (id == IDC_T_CHRONO_MINUS) {
                self->pending_.chrono_minus = true;
                self->pending_.chrono_sel = self->chrono_sel();
            } else if (id == IDC_T_CHRONO_REM) {
                self->pending_.chrono_remove = true;
                self->pending_.chrono_sel = self->chrono_sel();
            } else if (id == IDC_T_BAKE_O3D) {
                const int sel = static_cast<int>(SendMessageW(self->t_bake_parts_, CB_GETCURSEL, 0, 0));
                self->pending_.tile_bake_parts = (sel >= 0 && sel < 5) ? (sel + 1) : 1;
                const int conf = MessageBoxW(
                    self->hwnd_,
                    L"Bake tile to O3D/SCO (grid parts)?\r\nRuns in background.",
                    L"Tile bake O3D", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (conf == IDYES) self->pending_.tile_bake_run = true;
            } else if (id == IDC_T_BAKE_REPLACE) {
                const int conf = MessageBoxW(
                    self->hwnd_,
                    L"Replace tile contents with last bake?\r\nUndoable.",
                    L"Tile bake replace", MB_YESNOCANCEL | MB_ICONWARNING);
                if (conf == IDYES) self->pending_.tile_bake_replace = true;
            } else if (id == IDC_T_BAKE_FBX) {
                const int sel = static_cast<int>(SendMessageW(self->t_bake_parts_, CB_GETCURSEL, 0, 0));
                self->pending_.tile_bake_parts = (sel >= 0 && sel < 5) ? (sel + 1) : 1;
                const int conf = MessageBoxW(
                    self->hwnd_,
                    L"Export tile bake to FBX + textures?\r\n(Independent of O3D bake)",
                    L"Tile bake FBX", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (conf == IDYES) self->pending_.tile_bake_fbx = true;
            } else if (id == IDC_TE_LAYER_NEW) {
                self->pending_.ground_create_layer = true;
            } else if (id == IDC_TE_LAYER_EDIT) {
                self->pending_.ground_edit_layer = true;
                self->pending_.ground_layer_sel = self->ground_layer_sel();
            } else if (id == IDC_TE_LAYER_DEL) {
                self->pending_.ground_delete_layer = true;
                self->pending_.ground_layer_sel = self->ground_layer_sel();
            } else if (id == IDC_TE_HIGHER || id == IDC_TE_LOWER || id == IDC_TE_SET || id == IDC_TE_PAINT ||
                       id == IDC_TE_RUBBER) {
                self->pending_.save_ui_settings = true;
            } else if (id == IDC_TT_REFRESH) {
                self->pending_.refresh_tt = true;
            } else if (id == IDC_TT_OPEN) {
                self->pending_.open_tt_file = true;
                self->pending_.tt_file_to_open = self->tt_selected_path();
            } else if (id == IDC_TT_MODE0 || id == IDC_TT_MODE1 || id == IDC_TT_MODE2) {
                self->pending_.tt_mode = (id == IDC_TT_MODE0) ? 0 : (id == IDC_TT_MODE1) ? 1 : 2;
            } else if (id == IDC_TT_TRACK_CREATE) {
                self->pending_.tt_create_track = true;
            } else if (id == IDC_TT_TRACK_DEL) {
                self->pending_.tt_delete_track_entry = true;
            } else if (id == IDC_TT_TRIP_CREATE1) {
                self->pending_.tt_create_trip_t1 = true;
            } else if (id == IDC_TT_TRIP_CREATE2) {
                self->pending_.tt_create_trip_t2 = true;
            } else if (id == IDC_TT_TRIP_DEL_STN) {
                self->pending_.tt_delete_station = true;
            } else if (id == IDC_TT_TRIP_PROFILES) {
                self->pending_.tt_edit_profiles = true;
            } else if (id == IDC_TT_TRIP_ASSIST) {
                self->pending_.tt_assistent = true;
            } else if (id == IDC_TT_TRIP_ADD) {
                self->pending_.tt_add_from_list = true;
            } else if (id == IDC_TT_TRIP_TYPE2) {
                self->pending_.tt_change_type2 = true;
            } else if (id == IDC_TT_COPY_TRACK) {
                self->pending_.tt_copy_track = true;
            } else if (id == IDC_TT_COPY_TRIP) {
                self->pending_.tt_copy_trip = true;
            } else if (id == IDC_TT_COPY_BOTH) {
                self->pending_.tt_copy_both = true;
            } else if (id == IDC_TT_EDIT_TT) {
                self->pending_.tt_edit_timetable = true;
            } else if (id == IDC_TT_FIND_ERR) {
                self->pending_.tt_find_errors = true;
            } else if (id == IDC_SL_FILTER) {
                self->pending_.sl_filter = true;
            } else if (id == IDC_SL_NEW) {
                self->pending_.sl_new = true;
            } else if (id == IDC_SL_SEARCH_ERR) {
                self->pending_.sl_search_err = true;
            } else if (id == IDC_SL_DELETE) {
                self->pending_.sl_delete = true;
            } else if (id == IDC_SL_MODE_START) {
                self->pending_.sl_mode = 0;
            } else if (id == IDC_SL_MODE_END) {
                self->pending_.sl_mode = 1;
            } else if (id == IDC_SL_MODE_PATH) {
                self->pending_.sl_mode = 2;
            } else if (id == IDC_TT_TRAIN_REV) {
                self->pending_.tt_train_reversed =
                    self->tt_train_rev_ && SendMessageW(self->tt_train_rev_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            }
        }
        return 0;
    }
    case WM_DESTROY:
        self->hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EditorSidePanel::terrain_paint_brush(TerrainBrushUi& out) const {
    if (tool_ != EditorTool::Terrain) return false;
    if (imgui_primary_) {
        out = imgui_state_.terrain_brush;
        return true;
    }
    out.brush_size_m = get_edit_float(terr_brush_size_, 20.f);
    out.speed = get_edit_float(terr_speed_, 3.f);
    out.set_height = get_edit_float(terr_set_h_, 0.f);
    out.ground_layer = ground_layer_sel();
    out.mode = TerrainBrushModeUi::Higher;
    if (terr_radio_lower_ && SendMessageW(terr_radio_lower_, BM_GETCHECK, 0, 0) == BST_CHECKED)
        out.mode = TerrainBrushModeUi::Lower;
    else if (terr_radio_set_ && SendMessageW(terr_radio_set_, BM_GETCHECK, 0, 0) == BST_CHECKED)
        out.mode = TerrainBrushModeUi::SetHeight;
    else if (terr_radio_paint_ && SendMessageW(terr_radio_paint_, BM_GETCHECK, 0, 0) == BST_CHECKED)
        out.mode = TerrainBrushModeUi::PaintLayer;
    else if (terr_radio_rubber_ && SendMessageW(terr_radio_rubber_, BM_GETCHECK, 0, 0) == BST_CHECKED)
        out.mode = TerrainBrushModeUi::RubberLayer;
    return true;
}

void EditorSidePanel::set_ground_layers(const std::vector<std::string>& labels, int sel) {
    imgui_state_.ground_layer_labels = labels;
    if (!labels.empty()) {
        if (sel < 0) sel = 0;
        if (sel >= static_cast<int>(labels.size())) sel = static_cast<int>(labels.size()) - 1;
        imgui_state_.terrain_brush.ground_layer = sel;
        ground_layer_sel_ = sel;
    } else {
        ground_layer_sel_ = -1;
    }
    if (!terr_layers_) return;
    SendMessageW(terr_layers_, LB_RESETCONTENT, 0, 0);
    for (const auto& s : labels) SendMessageW(terr_layers_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(to_wide(s).c_str()));
    if (!labels.empty()) {
        if (sel < 0) sel = 0;
        if (sel >= static_cast<int>(labels.size())) sel = static_cast<int>(labels.size()) - 1;
        SendMessageW(terr_layers_, LB_SETCURSEL, sel, 0);
        ground_layer_sel_ = sel;
    } else {
        ground_layer_sel_ = -1;
    }
}

void EditorSidePanel::set_chrono_list(const std::vector<std::string>& labels) {
    imgui_state_.chrono_labels = labels;
    if (!chrono_list_) return;
    SendMessageW(chrono_list_, LB_RESETCONTENT, 0, 0);
    for (const auto& s : labels) SendMessageW(chrono_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(to_wide(s).c_str()));
}

void EditorSidePanel::set_uvg_groups(const std::vector<std::string>& names) {
    imgui_state_.uvg_groups = names;
    if (imgui_state_.uvg_sel >= static_cast<int>(names.size())) imgui_state_.uvg_sel = 0;
    if (!t_uvg_list_) return;
    SendMessageW(t_uvg_list_, LB_RESETCONTENT, 0, 0);
    for (const auto& s : names)
        SendMessageW(t_uvg_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(to_wide(s).c_str()));
    if (!names.empty())
        SendMessageW(t_uvg_list_, LB_SETCURSEL, static_cast<WPARAM>(imgui_state_.uvg_sel), 0);
}


void EditorSidePanel::set_tt_from_man() {
    auto& man = omsi::tt::global_tt_man();
    auto to_wide = [](const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    auto fill_combo = [&](HWND cb, const std::vector<std::string>& items, int sel) {
        if (!cb) return;
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        for (const auto& s : items) {
            auto w = to_wide(s);
            SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
        }
        if (sel >= 0 && sel < static_cast<int>(items.size()))
            SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    };
    auto fill_list = [&](HWND lb, const std::vector<std::string>& items, int sel) {
        if (!lb) return;
        SendMessageW(lb, LB_RESETCONTENT, 0, 0);
        for (const auto& s : items) {
            auto w = to_wide(s);
            SendMessageW(lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
        }
        // sel is logical index (-1 = "< - >" row 0)
        const int row = sel + 1;
        if (row >= 0 && row < static_cast<int>(items.size()))
            SendMessageW(lb, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
        else if (!items.empty())
            SendMessageW(lb, LB_SETCURSEL, 0, 0);
    };

    fill_combo(tracks_combo_, man.track_combo_strings(), man.selected_track);
    fill_combo(trips_combo_, man.trip_combo_strings(), man.selected_trip);
    fill_list(tracks_list_, man.track_entry_list_strings(), man.selected_track_entry);
    fill_list(trips_list_, man.trip_station_list_strings(), man.selected_trip_station);

    if (tt_length_label_) {
        auto w = to_wide(man.track_length_label());
        SetWindowTextW(tt_length_label_, w.c_str());
    }
    // Terminus / Line / train reverse from selected trip
    if (man.selected_trip >= 0 && man.selected_trip < static_cast<int>(man.data.trips.size())) {
        const auto& tp = man.data.trips[static_cast<std::size_t>(man.selected_trip)];
        if (tt_terminus_) SetWindowTextW(tt_terminus_, to_wide(tp.terminus).c_str());
        if (tt_line_) SetWindowTextW(tt_line_, to_wide(tp.line).c_str());
        if (tt_train_rev_)
            SendMessageW(tt_train_rev_, BM_SETCHECK, tp.train_reversed ? BST_CHECKED : BST_UNCHECKED, 0);
        // ImGui buffers
        std::snprintf(imgui_state_.tt_terminus_buf, sizeof(imgui_state_.tt_terminus_buf), "%s", tp.terminus.c_str());
        std::snprintf(imgui_state_.tt_line_buf, sizeof(imgui_state_.tt_line_buf), "%s", tp.line.c_str());
        imgui_state_.tt_train_reversed = tp.train_reversed;
    }
    // ImGui track/trip file lists + entry rows
    imgui_state_.tt_tracks.clear();
    for (const auto& tr : man.data.tracks)
        imgui_state_.tt_tracks.push_back(tr.path.empty() ? (man.data.root / (tr.name + ".ttr")) : tr.path);
    imgui_state_.tt_trips.clear();
    for (const auto& tp : man.data.trips)
        imgui_state_.tt_trips.push_back(tp.path.empty() ? (man.data.root / (tp.name + ".ttp")) : tp.path);
    imgui_state_.tt_track_sel = man.selected_track;
    imgui_state_.tt_trip_sel = man.selected_trip;
    imgui_state_.tt_track_entries = man.track_entry_list_strings();
    imgui_state_.tt_trip_stations = man.trip_station_list_strings();
    imgui_state_.tt_track_entry_sel = man.selected_track_entry;
    imgui_state_.tt_trip_station_sel = man.selected_trip_station;
}

void EditorSidePanel::set_tt_lists(const std::vector<std::filesystem::path>& tracks,
                                   const std::vector<std::filesystem::path>& trips,
                                   const std::vector<std::filesystem::path>& station,
                                   const std::vector<std::filesystem::path>& signals) {
    tt_paths_.clear();
    auto fill = [&](HWND list, const std::vector<std::filesystem::path>& items, int base) {
        if (!list) return;
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& p : items) {
            tt_paths_.push_back(p);
            const std::wstring label = to_wide(p.filename().string());
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        (void)base;
    };
    fill(tracks_list_, tracks, 0);
    fill(trips_list_, trips, static_cast<int>(tracks.size()));
    fill(station_list_, station, static_cast<int>(tracks.size() + trips.size()));
    fill(signal_list_, signals,
         static_cast<int>(tracks.size() + trips.size() + station.size()));
    // ImGui panel state (map_editor_imgui_panel.cpp)
    imgui_state_.tt_tracks = tracks;
    imgui_state_.tt_trips = trips;
    imgui_state_.tt_stations = station;
    imgui_state_.tt_signals = signals;
    if (imgui_state_.tt_track_sel >= static_cast<int>(tracks.size())) imgui_state_.tt_track_sel = -1;
    if (imgui_state_.tt_trip_sel >= static_cast<int>(trips.size())) imgui_state_.tt_trip_sel = -1;
    if (imgui_state_.tt_station_sel >= static_cast<int>(station.size())) imgui_state_.tt_station_sel = -1;
    if (imgui_state_.tt_signal_sel >= static_cast<int>(signals.size())) imgui_state_.tt_signal_sel = -1;
}

int EditorSidePanel::ground_layer_sel() const {
    if (imgui_primary_) return imgui_state_.terrain_brush.ground_layer;
    if (terr_layers_) {
        const int sel = static_cast<int>(SendMessageW(terr_layers_, LB_GETCURSEL, 0, 0));
        if (sel >= 0) return sel;
    }
    return ground_layer_sel_;
}

int EditorSidePanel::chrono_sel() const {
    if (imgui_primary_) return imgui_state_.chrono_sel;
    if (!chrono_list_) return -1;
    return static_cast<int>(SendMessageW(chrono_list_, LB_GETCURSEL, 0, 0));
}

std::filesystem::path EditorSidePanel::tt_selected_path() const {
    if (imgui_primary_) {
        auto pick_vec = [](const std::vector<std::filesystem::path>& v, int sel) -> std::filesystem::path {
            if (sel < 0 || sel >= static_cast<int>(v.size())) return {};
            return v[static_cast<std::size_t>(sel)];
        };
        if (tool_ == EditorTool::TracksTrips) {
            if (auto p = pick_vec(imgui_state_.tt_tracks, imgui_state_.tt_track_sel); !p.empty()) return p;
            return pick_vec(imgui_state_.tt_trips, imgui_state_.tt_trip_sel);
        }
        if (tool_ == EditorTool::StationLinks) return pick_vec(imgui_state_.tt_stations, imgui_state_.tt_station_sel);
        if (tool_ == EditorTool::SignalRts) return pick_vec(imgui_state_.tt_signals, imgui_state_.tt_signal_sel);
        return {};
    }
    auto pick = [&](HWND list, int base) -> std::filesystem::path {
        if (!list) return {};
        const int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel < 0) return {};
        const int idx = base + sel;
        if (idx < 0 || static_cast<std::size_t>(idx) >= tt_paths_.size()) return {};
        return tt_paths_[static_cast<std::size_t>(idx)];
    };
    if (tool_ == EditorTool::TracksTrips) {
        if (auto p = pick(tracks_list_, 0); !p.empty()) return p;
        const int ntracks = tracks_list_ ? static_cast<int>(SendMessageW(tracks_list_, LB_GETCOUNT, 0, 0)) : 0;
        return pick(trips_list_, ntracks);
    }
    if (tool_ == EditorTool::StationLinks) return pick(station_list_, 0);
    if (tool_ == EditorTool::SignalRts) return pick(signal_list_, 0);
    return {};
}

float EditorSidePanel::spline_max_radius() const {
    if (imgui_primary_) return imgui_state_.spline_max_radius;
    return get_edit_float(s_max_radius_, 200.f);
}

int EditorSidePanel::attach_point() const {
    if (imgui_primary_) return imgui_state_.attach_point_field;
    return static_cast<int>(get_edit_float(o_att_pnt_, 0.f));
}

std::int32_t EditorSidePanel::parent_id_field() const {
    if (imgui_primary_) return imgui_state_.parent_id_field;
    return static_cast<std::int32_t>(get_edit_float(o_parent_, 0.f));
}

bool EditorSidePanel::traffic_paint_brush(EditorPathRuleProps& out) const {
    if (tool_ != EditorTool::TrafficRules) return false;
    read_traffic_rule_edits(out);
    return true;
}

} // namespace omsi::ui
