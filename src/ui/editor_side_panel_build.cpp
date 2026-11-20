#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui/editor_side_panel.hpp"
#include "ui/editor_side_panel_ids.hpp"

#include "omsi/logger.hpp"
#include "omsi/map_geo_tools.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace omsi::ui {
using namespace editor_side_panel_ids;
void EditorSidePanel::set_edit_float(HWND edit, float v) const {
    if (!edit) return;
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.4g", static_cast<double>(v));
    SetWindowTextW(edit, buf);
}

float EditorSidePanel::get_edit_float(HWND edit, float fallback) const {
    if (!edit) return fallback;
    wchar_t buf[64]{};
    GetWindowTextW(edit, buf, 64);
    try {
        return std::stof(std::wstring(buf));
    } catch (...) {
        return fallback;
    }
}

namespace {
constexpr float kGradMin = -100.f;
constexpr float kGradMax = 100.f;
constexpr int kGradTrackOffset = 1000;
constexpr int kGradTrackMin = 0;
constexpr int kGradTrackMax = 2000;
constexpr float kTrackDragSens = 1.f;    // thumb 1:1 with mouse pixels
constexpr float kTrackAttenYScale = 500.f; // slower value change far from map Y=0
constexpr float kTrackSmoothLambda = 12.f; // ~0.06 s half-life at 60 Hz
constexpr float kTrackSmoothSnap = 0.001f;

struct ScalarTrackSpec {
    float min_v = 0.f;
    float max_v = 1.f;
    float step = 0.1f;
    int pos_min = 0;
    int pos_max = 1000;
    int decimals = 2;
};

// pos = (value - min) / step; pos_max = (max - min) / step
constexpr ScalarTrackSpec kRotSpec{-360.f, 360.f, 1.f, 0, 720, 1};
constexpr ScalarTrackSpec kLenSpec{0.f, 100.f, 0.1f, 0, 1000, 2};
constexpr ScalarTrackSpec kRadSpec{-300.f, 300.f, 0.5f, 0, 1200, 2};
constexpr ScalarTrackSpec kSkewSpec{-90.f, 90.f, 0.1f, 0, 1800, 2};
constexpr ScalarTrackSpec kCantSpec{-100.f, 100.f, 0.1f, 0, 2000, 1};

int scalar_to_track_pos(const ScalarTrackSpec& spec, float v) {
    const int pos = static_cast<int>(std::lround((v - spec.min_v) / spec.step));
    return (std::clamp)(pos, spec.pos_min, spec.pos_max);
}

float track_pos_to_scalar(const ScalarTrackSpec& spec, int pos) {
    pos = (std::clamp)(pos, spec.pos_min, spec.pos_max);
    return spec.min_v + static_cast<float>(pos) * spec.step;
}

float wrap_rot_deg(float deg) {
    if (!std::isfinite(deg)) return 0.f;
    deg = std::fmod(deg, 360.f);
    if (deg > 360.f) deg -= 360.f;
    if (deg <= -360.f) deg += 360.f;
    return deg;
}

void write_scalar_edit(HWND edit, int decimals, float v) {
    if (!edit) return;
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.*f", decimals, static_cast<double>(v));
    SetWindowTextW(edit, buf);
}

float clamp_grad(float v) {
    v = std::round(v * 10.f) / 10.f;
    return (std::clamp)(v, kGradMin, kGradMax);
}

int grad_to_track_pos(float v) {
    return static_cast<int>(std::lround(clamp_grad(v) * 10.f)) + kGradTrackOffset;
}

float track_pos_to_grad(int pos) {
    return clamp_grad(static_cast<float>(pos - kGradTrackOffset) / 10.f);
}

void set_grad_edit(HWND edit, float v) {
    if (!edit) return;
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.1f", static_cast<double>(clamp_grad(v)));
    SetWindowTextW(edit, buf);
}

void init_scalar_track(HWND track, const ScalarTrackSpec& spec) {
    if (!track) return;
    SendMessageW(track, TBM_SETRANGE, TRUE, MAKELPARAM(spec.pos_min, spec.pos_max));
    SendMessageW(track, TBM_SETTICFREQ, (std::max)(1, (spec.pos_max - spec.pos_min) / 8), 0);
    SendMessageW(track, TBM_SETPOS, TRUE, scalar_to_track_pos(spec, 0.f));
}

HWND mk_scalar_track(HWND page, int id, int x, int y, int w, const ScalarTrackSpec& spec) {
    HWND track = CreateWindowW(TRACKBAR_CLASSW, L"",
                               WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_NOTIFYBEFOREMOVE, x, y, w, 22, page,
                               reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), GetModuleHandleW(nullptr),
                               nullptr);
    init_scalar_track(track, spec);
    return track;
}
} // namespace

void EditorSidePanel::set_grad_controls(HWND track, HWND edit, float pct) {
    const float v = clamp_grad(pct);
    if (track) SendMessageW(track, TBM_SETPOS, TRUE, grad_to_track_pos(v));
    if (edit) set_grad_edit(edit, v);
    if (track == s_g0_) grad_smooth_g0_ = v;
    if (track == s_g1_) grad_smooth_g1_ = v;
}

float EditorSidePanel::get_grad_value(HWND track, HWND edit, float fallback) const {
    if (edit) return clamp_grad(get_edit_float(edit, fallback));
    if (track) return track_pos_to_grad(static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0)));
    return fallback;
}

void EditorSidePanel::sync_grad_track_to_edit(HWND track, HWND edit) const {
    if (!track || !edit) return;
    set_grad_edit(edit, track_pos_to_grad(static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0))));
}

void EditorSidePanel::sync_grad_edit_to_track(HWND track, HWND edit) const {
    if (!edit) return;
    const float v = clamp_grad(get_edit_float(edit, 0.f));
    suppress_grad_notify_ = true;
    set_grad_edit(edit, v);
    if (track) SendMessageW(track, TBM_SETPOS, TRUE, grad_to_track_pos(v));
    suppress_grad_notify_ = false;
}

void EditorSidePanel::arm_spline_grad_undo() {
    if (!grad_edit_armed_) return;
    spline_grad_undo_pending_ = true;
    grad_edit_armed_ = false;
}

void EditorSidePanel::arm_object_edit_undo() {
    if (!object_edit_armed_) return;
    object_edit_undo_pending_ = true;
    object_edit_armed_ = false;
}

void EditorSidePanel::clear_spline_grad_undo_pending() {
    spline_grad_undo_pending_ = false;
    grad_edit_armed_ = true;
}

void EditorSidePanel::nudge_grad(HWND track, HWND edit, float delta) {
    arm_spline_grad_undo();
    const float v = get_grad_value(track, edit, 0.f) + delta;
    suppress_grad_notify_ = true;
    set_grad_controls(track, edit, v);
    suppress_grad_notify_ = false;
    read_spline_edits(pending_.spline_edit);
    pending_.spline_grad_preview = true;
    pending_.spline_grad_preview_end = true;
}

void EditorSidePanel::nudge_z(float delta) {
    arm_spline_grad_undo();
    set_edit_float(s_z_, get_edit_float(s_z_, 0.f) + delta);
    read_spline_edits(pending_.spline_edit);
    pending_.spline_grad_preview = true;
    pending_.spline_grad_preview_end = true;
}

void EditorSidePanel::handle_spline_nudge_btn(int btn_id) {
    auto nudge = [this](HWND track, HWND edit, const ScalarTrackSpec& spec, float delta, bool wrap_rot) {
        arm_spline_grad_undo();
        float v = get_edit_float(edit, 0.f) + delta;
        if (wrap_rot) v = wrap_rot_deg(v);
        suppress_grad_notify_ = true;
        set_scalar_field(track, edit, spec.min_v, spec.max_v, spec.step, spec.decimals, v);
        suppress_grad_notify_ = false;
        read_spline_edits(pending_.spline_edit);
        pending_.spline_grad_preview = true;
        pending_.spline_grad_preview_end = true;
    };
    switch (btn_id) {
    case IDC_S_ROT_UP: nudge(s_rot_trk_, s_rot_, kRotSpec, kRotSpec.step, true); break;
    case IDC_S_ROT_DN: nudge(s_rot_trk_, s_rot_, kRotSpec, -kRotSpec.step, true); break;
    case IDC_S_SKEW0_UP: nudge(s_skew0_trk_, s_skew0_, kSkewSpec, kSkewSpec.step, false); break;
    case IDC_S_SKEW0_DN: nudge(s_skew0_trk_, s_skew0_, kSkewSpec, -kSkewSpec.step, false); break;
    case IDC_S_SKEW1_UP: nudge(s_skew1_trk_, s_skew1_, kSkewSpec, kSkewSpec.step, false); break;
    case IDC_S_SKEW1_DN: nudge(s_skew1_trk_, s_skew1_, kSkewSpec, -kSkewSpec.step, false); break;
    case IDC_S_LEN_UP: nudge(s_len_trk_, s_len_, kLenSpec, kLenSpec.step, false); break;
    case IDC_S_LEN_DN: nudge(s_len_trk_, s_len_, kLenSpec, -kLenSpec.step, false); break;
    case IDC_S_RAD_UP: nudge(s_rad_trk_, s_radius_, kRadSpec, kRadSpec.step, false); break;
    case IDC_S_RAD_DN: nudge(s_rad_trk_, s_radius_, kRadSpec, -kRadSpec.step, false); break;
    case IDC_S_CANT0_UP: nudge(s_cant0_trk_, s_cant0_, kCantSpec, kCantSpec.step, false); break;
    case IDC_S_CANT0_DN: nudge(s_cant0_trk_, s_cant0_, kCantSpec, -kCantSpec.step, false); break;
    case IDC_S_CANT1_UP: nudge(s_cant1_trk_, s_cant1_, kCantSpec, kCantSpec.step, false); break;
    case IDC_S_CANT1_DN: nudge(s_cant1_trk_, s_cant1_, kCantSpec, -kCantSpec.step, false); break;
    default: break;
    }
}

namespace {
const ScalarTrackSpec* spec_for_id(int control_id) {
    switch (control_id) {
    case IDC_S_ROT: return &kRotSpec;
    case IDC_S_LEN: return &kLenSpec;
    case IDC_S_RAD: return &kRadSpec;
    case IDC_S_SKEW0: return &kSkewSpec;
    case IDC_S_SKEW1: return &kSkewSpec;
    case IDC_S_CANT0: return &kCantSpec;
    case IDC_S_CANT1: return &kCantSpec;
    default: return nullptr;
    }
}
} // namespace

void EditorSidePanel::set_scalar_field(HWND track, HWND edit, float min_v, float max_v, float step,
                                       int decimals, float v) {
    ScalarTrackSpec spec{min_v, max_v, step, 0, static_cast<int>(std::lround((max_v - min_v) / step)), decimals};
    if (track) SendMessageW(track, TBM_SETPOS, TRUE, scalar_to_track_pos(spec, v));
    write_scalar_edit(edit, decimals, v);
}

float EditorSidePanel::get_scalar_field(HWND track, HWND edit, float min_v, float max_v, float step,
                                        float fallback) const {
    if (edit) {
        wchar_t buf[64]{};
        GetWindowTextW(edit, buf, 64);
        try {
            return std::stof(std::wstring(buf));
        } catch (...) {
        }
    }
    if (track) {
        ScalarTrackSpec spec{min_v, max_v, step, 0, static_cast<int>(std::lround((max_v - min_v) / step)), 2};
        return track_pos_to_scalar(spec, static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0)));
    }
    return fallback;
}

void EditorSidePanel::set_spline_scalar_tracks(const EditorSplineProps& props) {
    track_smooth_active_ = false;
    track_drag_hwnd_ = nullptr;
    suppress_grad_notify_ = true;
    set_scalar_field(s_rot_trk_, s_rot_, kRotSpec.min_v, kRotSpec.max_v, kRotSpec.step, kRotSpec.decimals,
                     wrap_rot_deg(props.rot));
    set_scalar_field(s_skew0_trk_, s_skew0_, kSkewSpec.min_v, kSkewSpec.max_v, kSkewSpec.step, kSkewSpec.decimals,
                     props.skew_start);
    set_scalar_field(s_skew1_trk_, s_skew1_, kSkewSpec.min_v, kSkewSpec.max_v, kSkewSpec.step, kSkewSpec.decimals,
                     props.skew_end);
    set_scalar_field(s_len_trk_, s_len_, kLenSpec.min_v, kLenSpec.max_v, kLenSpec.step, kLenSpec.decimals,
                     props.length);
    set_scalar_field(s_rad_trk_, s_radius_, kRadSpec.min_v, kRadSpec.max_v, kRadSpec.step, kRadSpec.decimals,
                     props.radius);
    set_scalar_field(s_cant0_trk_, s_cant0_, kCantSpec.min_v, kCantSpec.max_v, kCantSpec.step, kCantSpec.decimals,
                     props.cant_start);
    set_scalar_field(s_cant1_trk_, s_cant1_, kCantSpec.min_v, kCantSpec.max_v, kCantSpec.step, kCantSpec.decimals,
                     props.cant_end);
    grad_smooth_g0_ = props.start_grad;
    grad_smooth_g1_ = props.end_grad;
    set_grad_controls(s_g0_, s_g0_edit_, props.start_grad);
    set_grad_controls(s_g1_, s_g1_edit_, props.end_grad);
    suppress_grad_notify_ = false;
}

bool EditorSidePanel::is_spline_smooth_track(HWND track) const {
    return track == s_g0_ || track == s_g1_ || track == s_rot_trk_ || track == s_len_trk_ ||
           track == s_rad_trk_ || track == s_skew0_trk_ || track == s_skew1_trk_ || track == s_cant0_trk_ ||
           track == s_cant1_trk_;
}

void EditorSidePanel::display_spline_track_value(HWND track, float v) {
    HWND edit = nullptr;
    const ScalarTrackSpec* spec = nullptr;
    const bool is_grad = (track == s_g0_ || track == s_g1_);
    if (track == s_g0_) edit = s_g0_edit_;
    else if (track == s_g1_) edit = s_g1_edit_;
    else if (track == s_rot_trk_) {
        edit = s_rot_;
        spec = &kRotSpec;
    } else if (track == s_len_trk_) {
        edit = s_len_;
        spec = &kLenSpec;
    } else if (track == s_rad_trk_) {
        edit = s_radius_;
        spec = &kRadSpec;
    } else if (track == s_skew0_trk_) {
        edit = s_skew0_;
        spec = &kSkewSpec;
    } else if (track == s_skew1_trk_) {
        edit = s_skew1_;
        spec = &kSkewSpec;
    } else if (track == s_cant0_trk_) {
        edit = s_cant0_;
        spec = &kCantSpec;
    } else if (track == s_cant1_trk_) {
        edit = s_cant1_;
        spec = &kCantSpec;
    } else
        return;

    suppress_grad_notify_ = true;
    if (is_grad) {
        set_grad_controls(track, edit, v);
    } else if (spec) {
        float out = v;
        if (track == s_rot_trk_) out = wrap_rot_deg(v);
        write_scalar_edit(edit, spec->decimals, out);
        SendMessageW(track, TBM_SETPOS, TRUE, scalar_to_track_pos(*spec, out));
    }
    suppress_grad_notify_ = false;
}

bool EditorSidePanel::handle_spline_track_thumb_changing(HWND track, int raw_pos, int reason) {
    if (!is_spline_smooth_track(track)) return false;

    HWND edit = nullptr;
    const ScalarTrackSpec* spec = nullptr;
    const bool is_grad = (track == s_g0_ || track == s_g1_);
    if (track == s_g0_) edit = s_g0_edit_;
    else if (track == s_g1_) edit = s_g1_edit_;
    else if (track == s_rot_trk_) {
        edit = s_rot_;
        spec = &kRotSpec;
    } else if (track == s_len_trk_) {
        edit = s_len_;
        spec = &kLenSpec;
    } else if (track == s_rad_trk_) {
        edit = s_radius_;
        spec = &kRadSpec;
    } else if (track == s_skew0_trk_) {
        edit = s_skew0_;
        spec = &kSkewSpec;
    } else if (track == s_skew1_trk_) {
        edit = s_skew1_;
        spec = &kSkewSpec;
    } else if (track == s_cant0_trk_) {
        edit = s_cant0_;
        spec = &kCantSpec;
    } else if (track == s_cant1_trk_) {
        edit = s_cant1_;
        spec = &kCantSpec;
    }

    const bool thumb_drag =
        reason == TB_THUMBTRACK || reason == TB_TOP || reason == TB_THUMBPOSITION || reason == TB_BOTTOM;

    if (thumb_drag) {
        if (track_drag_hwnd_ != track || reason == TB_TOP) {
            arm_spline_grad_undo();
            track_drag_hwnd_ = track;
            track_drag_anchor_pos_ = raw_pos;
            POINT cpt{};
            GetCursorPos(&cpt);
            track_drag_anchor_mouse_x_ = cpt.x;
            if (is_grad) {
                track_drag_anchor_val_ = get_grad_value(track, edit, 0.f);
            } else if (spec) {
                float v = get_edit_float(edit, track_pos_to_scalar(*spec, raw_pos));
                if (track == s_rot_trk_) v = wrap_rot_deg(v);
                track_drag_anchor_val_ = v;
            }
            track_smooth_value_ = track_smooth_target_ = track_drag_anchor_val_;
            track_smooth_active_ = true;
            track_smooth_last_tick_ = std::chrono::steady_clock::now();
        }

        POINT cpt{};
        GetCursorPos(&cpt);
        const int pixel_delta = cpt.x - track_drag_anchor_mouse_x_;
        const int thumb_pos =
            is_grad ? raw_pos
                    : (spec ? (std::clamp)(track_drag_anchor_pos_ + pixel_delta, spec->pos_min, spec->pos_max)
                            : raw_pos);
        const float y_atten =
            1.f / (1.f + std::fabs(spline_track_atten_y_) / kTrackAttenYScale);
        if (is_grad) {
            track_smooth_target_ =
                clamp_grad(track_drag_anchor_val_ + static_cast<float>(pixel_delta) * 0.01f * kTrackDragSens *
                           y_atten);
            SendMessageW(track, TBM_SETPOS, TRUE, thumb_pos);
        } else if (spec) {
            float t = track_drag_anchor_val_ + static_cast<float>(pixel_delta) * spec->step * kTrackDragSens * y_atten;
            if (track == s_rot_trk_) t = wrap_rot_deg(t);
            track_smooth_target_ = t;
            SendMessageW(track, TBM_SETPOS, TRUE, thumb_pos);
        }

        display_spline_track_value(track, track_smooth_value_);
        return true;
    }

    arm_spline_grad_undo();
    float v = 0.f;
    if (is_grad) {
        v = track_pos_to_grad(raw_pos);
    } else if (spec) {
        v = track_pos_to_scalar(*spec, raw_pos);
        if (track == s_rot_trk_) v = wrap_rot_deg(v);
    }
    track_smooth_value_ = track_smooth_target_ = v;
    track_smooth_active_ = false;
    track_drag_hwnd_ = nullptr;
    display_spline_track_value(track, v);
    read_spline_edits(pending_.spline_edit);
    pending_.spline_grad_preview = true;
    return true;
}

void EditorSidePanel::tick_spline_track_smooth(float dt_seconds) {
    if (!track_smooth_active_ || !track_drag_hwnd_) return;
    const float prev = track_smooth_value_;
    const float k = 1.f - std::exp(-kTrackSmoothLambda * dt_seconds);
    track_smooth_value_ += (track_smooth_target_ - track_smooth_value_) * k;
    if (std::fabs(track_smooth_value_ - track_smooth_target_) < kTrackSmoothSnap)
        track_smooth_value_ = track_smooth_target_;
    if (std::fabs(track_smooth_value_ - prev) <= kTrackSmoothSnap) return;
    display_spline_track_value(track_drag_hwnd_, track_smooth_value_);
    read_spline_edits(pending_.spline_edit);
    pending_.spline_grad_preview = true;
}

void EditorSidePanel::finish_spline_track_drag() {
    if (!track_drag_hwnd_) return;
    track_smooth_value_ = track_smooth_target_;
    display_spline_track_value(track_drag_hwnd_, track_smooth_value_);
    track_smooth_active_ = false;
    track_drag_hwnd_ = nullptr;
    read_spline_edits(pending_.spline_edit);
    pending_.spline_grad_preview_end = true;
}

void EditorSidePanel::handle_spline_track_hscroll(HWND track, WPARAM wp) {
    if (!is_spline_smooth_track(track)) return;
    const int code = LOWORD(wp);
    if (code == TB_ENDTRACK) finish_spline_track_drag();
}

void EditorSidePanel::sync_spline_scalar_edit(int control_id) {
    const ScalarTrackSpec* spec = spec_for_id(control_id);
    if (!spec) return;
    HWND edit = nullptr;
    HWND track = nullptr;
    switch (control_id) {
    case IDC_S_ROT: edit = s_rot_; track = s_rot_trk_; break;
    case IDC_S_LEN: edit = s_len_; track = s_len_trk_; break;
    case IDC_S_RAD: edit = s_radius_; track = s_rad_trk_; break;
    case IDC_S_SKEW0: edit = s_skew0_; track = s_skew0_trk_; break;
    case IDC_S_SKEW1: edit = s_skew1_; track = s_skew1_trk_; break;
    case IDC_S_CANT0: edit = s_cant0_; track = s_cant0_trk_; break;
    case IDC_S_CANT1: edit = s_cant1_; track = s_cant1_trk_; break;
    default: return;
    }
    if (!edit || !track) return;
    suppress_grad_notify_ = true;
    float v = get_edit_float(edit, 0.f);
    if (control_id == IDC_S_ROT) v = wrap_rot_deg(v);
    if (control_id == IDC_S_ROT) write_scalar_edit(edit, spec->decimals, v);
    SendMessageW(track, TBM_SETPOS, TRUE, scalar_to_track_pos(*spec, v));
    suppress_grad_notify_ = false;
}

void EditorSidePanel::build_tile_page(HWND page) {
    int y = 8;
    t_coord_lbl_ = mk_static(page, L"Coord: Classic", 8, y, 260, 16);
    y += 18;
    t_tile_lbl_ = mk_static(page, L"Tile grid (0,0) size 300 m", 8, y, 260, 16);
    y += 18;
    t_btn_create_tile_ = mk_btn(page, IDC_T_CREATE_TILE, L"Crear tile (camara)", 8, y, 160, 26);
    t_btn_delete_tile_ = mk_btn(page, IDC_T_DELETE_TILE, L"Borrar tile", 172, y, 96, 26);
    y += 30;
    t_tile_hint_ = mk_static(page, L"Classic: convert to [worldcoordinates] for aerial download", 8, y, 260, 28);
    y += 30;
    t_btn_convert_ = mk_btn(page, IDC_T_CONVERT, L"Convert to world coordinates", 8, y, 260, 24);
    y += 28;
    t_aerial_vis_ = mk_check(page, IDC_T_AERIAL_VIS, L"Use tile aerials", 8, y, 100, 20);
    t_btn_aerial_dnl_ = mk_btn(page, IDC_T_AERIAL_DNL, L"Dnl tile", 112, y - 2, 72, 24);
    t_btn_aerial_del_ = mk_btn(page, IDC_T_AERIAL_DEL, L"Del aerial", 188, y - 2, 80, 24);
    y += 24;
    t_aerial_res_lbl_ = mk_static(page, L"Resolution: 512 px", 8, y, 200, 16);
    y += 16;
    t_aerial_res_ = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 8, y, 200, 26, page,
                                  reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_T_AERIAL_RES)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(t_aerial_res_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 5));
    SendMessageW(t_aerial_res_, TBM_SETPOS, TRUE, 1);
    y += 30;
    t_sat_lbl_ = mk_static(page, L"Satellite source:", 8, y, 160, 16);
    y += 16;
    t_sat_provider_ = mk_combo(page, IDC_T_SATPROV, 8, y, 260, 180);
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Esri World Imagery (free)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"EOX Sentinel-2 (free)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OMSI Editor_LinkAerial"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MapTiler (free key)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Google Map Tiles (paid)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OpenStreetMap (street, free)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bing Maps aerial (Azure key)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Google Static Maps (API key)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Geoportal PL orthophoto"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Geoportal PL orthophoto hi-res"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom URL (presets)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GCBA CABA orthophoto 2017 (free)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"IGN Argentina VANT (free)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GCMods Bing (OMSI Editor_LinkAerial)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GCMods Google (OMSI, 256px)"));
    SendMessageW(t_sat_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Google satellite mt* (test, no key)"));
    SendMessageW(t_sat_provider_, CB_SETCURSEL, 0, 0);
    y += 28;
    t_custom_preset_lbl_ = mk_static(page, L"URL preset:", 8, y, 120, 16);
    y += 16;
    t_custom_preset_ = mk_combo(page, IDC_T_CUSTPRE, 8, y, 260, 180);
    for (int i = 0; i <= 8; ++i)
        SendMessageW(t_custom_preset_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(to_wide(map::custom_aerial_preset_label(static_cast<map::CustomAerialPreset>(i))).c_str()));
    SendMessageW(t_custom_preset_, CB_SETCURSEL, 0, 0);
    y += 28;
    t_link_lbl_ = mk_static(page, L"Editor_LinkAerial (~x/~y/~z):", 8, y, 240, 16);
    y += 16;
    t_link_aerial_ = mk_edit(page, IDC_T_LINK_AERIAL, 8, y, 260, 22);
    y += 26;
    t_api_lbl_ = mk_static(page, L"API key (GCMods Bing/Google / MapTiler):", 8, y, 260, 16);
    y += 16;
    t_api_key_ = mk_edit(page, IDC_T_APIKEY, 8, y, 260, 22);
    y += 26;
    t_dtm_lbl_ = mk_static(page, L"Elevation source:", 8, y, 160, 16);
    y += 16;
    t_dtm_provider_ = mk_combo(page, IDC_T_DTMPROV, 8, y, 260, 100);
    SendMessageW(t_dtm_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OpenTopoData ASTER 30m (free)"));
    SendMessageW(t_dtm_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Google Elevation (paid)"));
    SendMessageW(t_dtm_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"NASA SRTM 30m (free)"));
    SendMessageW(t_dtm_provider_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Geoportal PL NMT 1m (Poland)"));
    SendMessageW(t_dtm_provider_, CB_SETCURSEL, 0, 0);
    y += 28;
    t_ref_lbl_ = mk_static(page, L"Ref lat / lon (classic maps):", 8, y, 240, 16);
    y += 16;
    mk_static(page, L"Lat:", 8, y, 28, 16);
    t_ref_lat_ = mk_edit(page, IDC_T_REFLAT, 36, y - 2, 100, 22);
    mk_static(page, L"Lon:", 144, y, 28, 16);
    t_ref_lon_ = mk_edit(page, IDC_T_REFLON, 172, y - 2, 96, 22);
    y += 28;
    t_btn_dtm_ = mk_btn(page, IDC_T_DTM, L"Apply real terrain height (DTM)", 8, y, 260, 26);
    y += 30;
    mk_static(page, L"N spawn height (solo una, o ninguna = default):", 8, y, 260, 16);
    y += 18;
    t_chk_spawn_terr_ = mk_check(page, IDC_T_SPAWN_TERR, L"Pegar a terreno (.terrain)", 8, y, 260, 20);
    y += 20;
    t_chk_spawn_mesh_ = mk_check(page, IDC_T_SPAWN_MESH, L"Encima de mallas (.o3d cuerpo)", 8, y, 260, 20);
    y += 20;
    t_chk_spawn_align_ = mk_check(page, IDC_T_SPAWN_ALIGN, L"Alinear pitch y bank", 8, y, 260, 20);
    y += 24;
    t_btn_aerial_miss_ = mk_btn(page, IDC_T_AERIAL_MISS, L"Aerial bulk (missing)", 8, y, 128, 24);
    t_btn_aerial_all_ = mk_btn(page, IDC_T_AERIAL_ALL, L"Aerial bulk (all)", 140, y, 128, 24);
    y += 28;
    t_btn_lightmap_ = mk_btn(page, IDC_T_LIGHTMAP, L"Create Lightmap", 8, y, 120, 24);
    t_btn_whole_map_ = mk_btn(page, IDC_T_WHOLE_MAP, L"Do with whole map...", 132, y, 136, 24);
    y += 32;
    mk_static(page, L"Chrono scenarios (Chrono + / - on selection):", 8, y, 260, 16);
    y += 18;
    chrono_list_ = mk_list(page, IDC_T_CHRONO_LIST, 8, y, 260, 70);
    y += 74;
    t_btn_chrono_plus_ = mk_btn(page, IDC_T_CHRONO_PLUS, L"Chrono +", 8, y, 80, 24);
    t_btn_chrono_minus_ = mk_btn(page, IDC_T_CHRONO_MINUS, L"Chrono -", 92, y, 80, 24);
    t_btn_chrono_rem_ = mk_btn(page, IDC_T_CHRONO_REM, L"Remove", 176, y, 80, 24);
    y += 36;
    // --- Tile bake (parts 1..5, vertex limit 65535) ---
    t_bake_lbl_ = mk_static(page, L"Tile bake (grid parts, max 65535 verts/o3d):", 8, y, 260, 16);
    y += 18;
    mk_static(page, L"Parts:", 8, y, 40, 16);
    t_bake_parts_ = mk_combo(page, IDC_T_BAKE_PARTS, 52, y - 2, 60, 120);
    for (int n = 1; n <= 5; ++n) {
        wchar_t buf[8];
        swprintf(buf, 8, L"%d", n);
        SendMessageW(t_bake_parts_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    SendMessageW(t_bake_parts_, CB_SETCURSEL, 0, 0);
    y += 26;
    t_btn_bake_o3d_ = mk_btn(page, IDC_T_BAKE_O3D, L"Bake O3D", 8, y, 80, 26);
    t_btn_bake_replace_ = mk_btn(page, IDC_T_BAKE_REPLACE, L"Replace", 92, y, 80, 26);
    EnableWindow(t_btn_bake_replace_, FALSE);
    t_btn_bake_fbx_ = mk_btn(page, IDC_T_BAKE_FBX, L"Bake FBX", 176, y, 80, 26);
    y += 30;
    t_bake_status_ = mk_static(page, L"Bake: idle", 8, y, 260, 32);
    y += 36;
    // Enable vertical scroll so API / DTM stay reachable on short windows.
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = y;
    si.nPage = 1;
    si.nPos = 0;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

void EditorSidePanel::build_stub_page(HWND page, const wchar_t* title) {
    mk_static(page, title, 8, 8, 250, 20);
    mk_static(page, L"(OMSI tab — pending full port)", 8, 32, 250, 20);
}

void EditorSidePanel::build_tracks_page(HWND page) {
    int y = 8;
    mk_static(page, L"Mode", 8, y, 60, 16);
    y += 18;
    tt_mode0_ = CreateWindowW(L"BUTTON", L"Add Trackentry", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 8,
                              y, 140, 18, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TT_MODE0)),
                              GetModuleHandleW(nullptr), nullptr);
    tt_mode1_ = CreateWindowW(L"BUTTON", L"Add Station", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 150, y, 110, 18,
                              page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TT_MODE1)),
                              GetModuleHandleW(nullptr), nullptr);
    y += 20;
    tt_mode2_ = CreateWindowW(L"BUTTON", L"Show Timetable of Station", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 8, y,
                              200, 18, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TT_MODE2)),
                              GetModuleHandleW(nullptr), nullptr);
    SendMessageW(tt_mode0_, BM_SETCHECK, BST_CHECKED, 0);
    mk_btn(page, IDC_TT_FIND_ERR, L"Find Errors", 210, y - 20, 60, 40);
    y += 28;
    mk_static(page, L"Tracks", 8, y, 50, 16);
    mk_btn(page, IDC_TT_TRACK_CREATE, L"Create...", 60, y - 2, 60, 22);
    mk_btn(page, IDC_TT_REFRESH, L"Refresh", 126, y - 2, 55, 22);
    mk_btn(page, IDC_TT_OPEN, L"Open", 186, y - 2, 40, 22);
    y += 22;
    tt_length_label_ = mk_static(page, L"Length = 0.000 km", 8, y, 260, 16);
    y += 20;
    tracks_combo_ = mk_combo(page, IDC_TT_TRACK_COMBO, 8, y, 260, 160);
    y += 26;
    tracks_list_ = mk_list(page, IDC_TT_TRACKS, 8, y, 260, 90);
    y += 94;
    mk_btn(page, IDC_TT_TRACK_DEL, L"Delete act. Track Entry", 8, y, 160, 22);
    y += 30;
    mk_static(page, L"Trips", 8, y, 60, 16);
    y += 18;
    mk_btn(page, IDC_TT_TRIP_CREATE1, L"Create New (type 1) with act. Track...", 8, y, 260, 22);
    y += 24;
    mk_btn(page, IDC_TT_TRIP_CREATE2, L"Create New (type 2) ...", 8, y, 160, 22);
    y += 26;
    trips_combo_ = mk_combo(page, IDC_TT_TRIP_COMBO, 8, y, 260, 160);
    y += 26;
    mk_static(page, L"Terminus:", 8, y, 60, 16);
    tt_terminus_ = mk_edit(page, IDC_TT_TERMINUS, 70, y - 2, 120, 20);
    mk_btn(page, IDC_TT_TRIP_TYPE2, L"→T2", 200, y - 2, 40, 22);
    y += 24;
    mk_static(page, L"Line:", 8, y, 40, 16);
    tt_line_ = mk_edit(page, IDC_TT_LINE, 70, y - 2, 120, 20);
    mk_btn(page, IDC_TT_TRIP_ASSIST, L"Assist...", 200, y - 2, 68, 22);
    y += 26;
    trips_list_ = mk_list(page, IDC_TT_TRIPS, 8, y, 220, 80);
    mk_btn(page, IDC_TT_TRIP_ADD, L"Add\nfrom\nList", 232, y, 40, 80);
    y += 86;
    mk_btn(page, IDC_TT_TRIP_DEL_STN, L"Delete ac. Station", 8, y, 120, 22);
    mk_btn(page, IDC_TT_TRIP_PROFILES, L"Edit Profiles...", 136, y, 120, 22);
    y += 26;
    tt_train_rev_ = mk_check(page, IDC_TT_TRAIN_REV, L"Train reversed by default", 8, y, 200, 18);
    y += 26;
    mk_btn(page, IDC_TT_COPY_TRACK, L"Copy Track", 8, y, 80, 22);
    mk_btn(page, IDC_TT_COPY_TRIP, L"Copy Trip", 96, y, 80, 22);
    mk_btn(page, IDC_TT_COPY_BOTH, L"Copy T && T", 184, y, 88, 22);
    y += 28;
    mk_btn(page, IDC_TT_EDIT_TT, L"Edit Timetable...", 8, y, 260, 24);
    y += 36;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = y;
    si.nPage = 1;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

void EditorSidePanel::build_station_page(HWND page) {
    int y = 8;
    mk_static(page, L"Station Links (TTData/StnLinks.cfg)", 8, y, 260, 18);
    y += 22;
    mk_btn(page, IDC_SL_FILTER, L"Filter Start Station", 8, y, 160, 24);
    mk_btn(page, IDC_TT_REFRESH, L"Refresh", 176, y, 80, 24);
    y += 28;
    mk_static(page, L"Links already available:", 8, y, 200, 16);
    y += 18;
    sl_links_ = mk_list(page, IDC_SL_LINKS, 8, y, 260, 100);
    station_list_ = sl_links_; // compatibility with tt_selected_path station branch
    y += 106;
    mk_btn(page, IDC_SL_NEW, L"New", 8, y, 60, 24);
    mk_btn(page, IDC_SL_SEARCH_ERR, L"Search Next Erroneous", 76, y, 160, 24);
    y += 30;
    mk_static(page, L"Path Segments:", 8, y, 160, 16);
    y += 18;
    sl_segments_ = mk_list(page, IDC_SL_SEGMENTS, 8, y, 260, 90);
    y += 96;
    sl_mode_start_ = CreateWindowW(L"BUTTON", L"Set Start Station", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                                   8, y, 140, 18, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SL_MODE_START)),
                                   GetModuleHandleW(nullptr), nullptr);
    sl_mode_end_ = CreateWindowW(L"BUTTON", L"Set End Station", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                 150, y, 120, 18, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SL_MODE_END)),
                                 GetModuleHandleW(nullptr), nullptr);
    y += 20;
    sl_mode_path_ = CreateWindowW(L"BUTTON", L"Add Path Segments", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                  8, y, 150, 18, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SL_MODE_PATH)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(sl_mode_start_, BM_SETCHECK, BST_CHECKED, 0);
    mk_btn(page, IDC_SL_DELETE, L"Delete", 176, y - 2, 80, 22);
    y += 28;
    mk_static(page, L"Log / validation:", 8, y, 160, 16);
    y += 18;
    sl_log_ = mk_list(page, IDC_SL_LOG, 8, y, 260, 80);
    y += 90;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = y;
    si.nPage = 1;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

void EditorSidePanel::build_signal_page(HWND page) {
    int y = 8;
    mk_static(page, L"Signal Routes (signalroutes.cfg)", 8, y, 260, 18);
    y += 22;
    mk_btn(page, IDC_TT_REFRESH, L"Refresh", 8, y, 80, 24);
    mk_btn(page, IDC_TT_OPEN, L"Open selected", 96, y, 110, 24);
    y += 30;
    signal_list_ = mk_list(page, IDC_TT_SIGNAL, 8, y, 260, 200);
}

void EditorSidePanel::build_splexp_page(HWND page) {
    int y = 8;
    mk_static(page, L"Spline-Exp. — perfil + instancia", 8, y, 260, 18);
    y += 24;
    mk_btn(page, IDC_S_CREATE, L"Crear .sli...", 8, y, 100, 24);
    mk_btn(page, IDC_S_SPLIT, L"Split!", 116, y, 80, 24);
    mk_btn(page, IDC_S_COMPLETE, L"Complete to...", 204, y, 64, 24);
    y += 30;
    mk_btn(page, IDC_S_ALIGN_START, L"Align Start to Obj.", 8, y, 130, 24);
    mk_btn(page, IDC_S_ALIGN_END, L"Align End to Obj.", 144, y, 120, 24);
    y += 30;
    mk_static(page, L"Crear abre editor 2D+3D. Ctrl+R = trim length.", 8, y, 260, 36);
}

void EditorSidePanel::build_terrain_page(HWND page) {
    int y = 8;
    mk_static(page, L"Terrain (OMSI brush)", 8, y, 250, 18);
    y += 22;
    mk_static(page, L"Brush size (m):", 8, y, 100, 16);
    terr_brush_size_ = mk_edit(page, IDC_TE_BRUSH, 110, y - 2, 60, 20);
    set_edit_float(terr_brush_size_, 20.f);
    y += 24;
    mk_static(page, L"Speed (m/s):", 8, y, 100, 16);
    terr_speed_ = mk_edit(page, IDC_TE_SPEED, 110, y - 2, 60, 20);
    set_edit_float(terr_speed_, 3.f);
    y += 24;
    mk_static(page, L"Set height (m):", 8, y, 100, 16);
    terr_set_h_ = mk_edit(page, IDC_TE_SETH, 110, y - 2, 60, 20);
    set_edit_float(terr_set_h_, 0.f);
    y += 28;
    terr_radio_higher_ = CreateWindowW(L"BUTTON", L"Higher", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                                       8, y, 70, 20, page,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TE_HIGHER)),
                                       GetModuleHandleW(nullptr), nullptr);
    terr_radio_lower_ = CreateWindowW(L"BUTTON", L"Lower", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 80, y, 60, 20,
                                      page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TE_LOWER)),
                                      GetModuleHandleW(nullptr), nullptr);
    terr_radio_set_ = CreateWindowW(L"BUTTON", L"Set Height", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 145, y, 90,
                                    20, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TE_SET)),
                                    GetModuleHandleW(nullptr), nullptr);
    y += 22;
    terr_radio_paint_ = CreateWindowW(L"BUTTON", L"Paint on Layer", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 8, y,
                                      120, 20, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TE_PAINT)),
                                      GetModuleHandleW(nullptr), nullptr);
    terr_radio_rubber_ = CreateWindowW(L"BUTTON", L"Rubber on Layer", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 140,
                                       y, 120, 20, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TE_RUBBER)),
                                       GetModuleHandleW(nullptr), nullptr);
    SendMessageW(terr_radio_higher_, BM_SETCHECK, BST_CHECKED, 0);
    y += 28;
    mk_static(page, L"Ground Layers:", 8, y, 160, 16);
    y += 18;
    terr_layers_ = mk_list(page, IDC_TE_LAYERS, 8, y, 260, 90);
    y += 94;
    terr_btn_layer_new_ = mk_btn(page, IDC_TE_LAYER_NEW, L"Create New Layer...", 8, y, 120, 24);
    terr_btn_layer_edit_ = mk_btn(page, IDC_TE_LAYER_EDIT, L"Edit Act. Layer...", 132, y, 120, 24);
    y += 28;
    terr_btn_layer_del_ = mk_btn(page, IDC_TE_LAYER_DEL, L"Delete Layer", 8, y, 100, 24);
    y += 30;
    mk_static(page, L"LMB drag on ground to paint. Shift = opposite (heights).", 8, y, 260, 36);
    y += 40;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = y;
    si.nPage = 1;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

void EditorSidePanel::build_objects_page(HWND page) {
    int y = 6;
    o_tile_lbl_ = mk_static(page, L"Filter tile (gx, gy or #):", 8, y, 200, 16);
    o_tile_search_ = mk_edit(page, IDC_O_TILE_SEARCH, 8, y + 16, 260, 20);
    y += 42;
    mk_static(page, L"Search types (name):", 8, y, 160, 16);
    y += 16;
    o_cat_search_ = mk_edit(page, IDC_O_CAT_SEARCH, 8, y, 260, 20);
    y += 24;
    o_types_lbl_ = mk_static(page, L"Actual Object Types:", 8, y, 200, 16);
    y += 18;
    o_catalog_ = mk_list(page, IDC_O_CATALOG, 8, y, 260, 120);
    y += 124;
    o_btn_load_ = mk_btn(page, IDC_O_LOAD, L"Load...", 8, y, 70, 24);
    o_chk_random_ = mk_check(page, 0, L"Random", 90, y + 2, 70, 20);
    o_btn_loaddir_ = mk_btn(page, IDC_O_LOADDIR, L"Load directory!", 168, y, 100, 24);
    y += 28;
    mk_btn(page, IDC_O_EDITFILE, L"Edit .sco", 8, y, 72, 22);
    mk_btn(page, IDC_O_FOLDER, L"Open folder", 84, y, 78, 22);
    mk_btn(page, IDC_O_RELOAD, L"Reload", 166, y, 52, 22);
    mk_btn(page, IDC_O_OPENMAP, L"Open .map", 222, y, 46, 22);
    y += 28;
    o_chk_spawn_terr_ = mk_check(page, IDC_O_SPAWN_TERR, L"Pegar a terreno (.terrain)", 8, y, 260, 20);
    y += 22;
    o_chk_spawn_mesh_ = mk_check(page, IDC_O_SPAWN_MESH, L"Encima de mallas (.o3d cuerpo)", 8, y, 260, 20);
    y += 22;
    o_chk_spawn_align_ = mk_check(page, IDC_O_SPAWN_ALIGN, L"Alinear pitch y bank", 8, y, 260, 20);
    y += 26;
    o_obj_grp_y_ = y;
    o_obj_grp_base_h_ = 210;
    o_grp_ = CreateWindowW(L"BUTTON", L"Object Properties", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 4, y, 272,
                           o_obj_grp_base_h_, page, nullptr, GetModuleHandleW(nullptr), nullptr);
    const int gy = y + 18;
    mk_static(page, L"x:", 12, gy, 20, 16);
    o_x_ = mk_edit(page, IDC_O_X, 32, gy - 2, 70, 20);
    mk_static(page, L"m", 104, gy, 16, 16);
    o_name_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                              130, gy - 2, 140, 20, page, nullptr, GetModuleHandleW(nullptr), nullptr);
    mk_static(page, L"y:", 12, gy + 24, 20, 16);
    o_y_ = mk_edit(page, IDC_O_Y, 32, gy + 22, 70, 20);
    mk_static(page, L"m", 104, gy + 24, 16, 16);
    mk_static(page, L"Pitch:", 130, gy + 24, 40, 16);
    o_pitch_ = mk_edit(page, IDC_O_PITCH, 172, gy + 22, 50, 20);
    mk_static(page, L"°", 224, gy + 24, 12, 16);
    mk_static(page, L"z:", 12, gy + 48, 20, 16);
    o_z_ = mk_edit(page, IDC_O_Z, 32, gy + 46, 70, 20);
    mk_static(page, L"m", 104, gy + 48, 16, 16);
    mk_static(page, L"Bank:", 130, gy + 48, 40, 16);
    o_bank_ = mk_edit(page, IDC_O_BANK, 172, gy + 46, 50, 20);
    mk_static(page, L"°", 224, gy + 48, 12, 16);
    mk_static(page, L"rot:", 12, gy + 72, 28, 16);
    o_rot_ = mk_edit(page, IDC_O_ROT, 40, gy + 70, 62, 20);
    mk_static(page, L"°", 104, gy + 72, 12, 16);
    o_btn_attach_obj_ = mk_btn(page, IDC_O_ATTACH, L"Attach to Object...", 12, gy + 96, 120, 22);
    mk_static(page, L"AttPntNr.:", 140, gy + 98, 60, 16);
    o_att_pnt_ = mk_edit(page, IDC_O_ATT_PNT, 200, gy + 96, 40, 20);
    set_edit_float(o_att_pnt_, 0.f);
    o_btn_attach_sli_ = mk_btn(page, IDC_O_ATTACH_SLI, L"~ to Spline ...", 12, gy + 122, 110, 22);
    o_count_lbl_ = mk_static(page, L"Count (~): —", 130, gy + 124, 120, 16);
    mk_static(page, L"Distance:", 12, gy + 148, 55, 16);
    o_dist_ = mk_edit(page, 0, 68, gy + 146, 50, 20);
    mk_static(page, L"m", 120, gy + 148, 14, 16);
    mk_static(page, L"Range:", 140, gy + 148, 40, 16);
    o_range_ = mk_edit(page, 0, 182, gy + 146, 50, 20);
    mk_static(page, L"m", 234, gy + 148, 14, 16);
    o_chk_tangential_ = mk_check(page, IDC_O_TANGENTIAL, L"Tangential to spline", 12, gy + 170, 140, 18);
    o_chk_align_terr_ = mk_check(page, IDC_O_ALIGN_TERR, L"Align Terrain", 160, gy + 170, 100, 18);
    SendMessageW(o_chk_align_terr_, BM_SETCHECK, BST_CHECKED, 0);
    o_slots_base_y_ = gy + 192;
    o_slots_grp_ = CreateWindowW(L"BUTTON", L"Extra lines", WS_CHILD | BS_GROUPBOX, 8, o_slots_base_y_, 256, 24,
                                 page, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        const int slot_y0 = o_slots_base_y_ + 18;
        for (int i = 0; i < kMaxSplineTextSlots; ++i) {
            wchar_t lbl[16]{};
            swprintf_s(lbl, L"Slot %d:", i + 1);
            o_slot_lbl_[i] = mk_static(page, lbl, 12, slot_y0 + i * 22, 52, 16);
            o_slot_edits_[i] = mk_edit(page, IDC_O_SLOT0 + i, 68, slot_y0 + i * 22 - 2, 196, 20);
            ShowWindow(o_slot_lbl_[i], SW_HIDE);
            ShowWindow(o_slot_edits_[i], SW_HIDE);
        }
        ShowWindow(o_slots_grp_, SW_HIDE);
    }
    y += o_obj_grp_base_h_ + 6;
    o_object_tail_y_ = y;
    o_btn_autorot_ = mk_btn(page, 0, L"Auto Rot. All", 8, y, 90, 22);
    o_btn_parent_ = mk_btn(page, IDC_O_PARENT, L"Parent to...:", 104, y, 90, 22);
    o_parent_ = mk_edit(page, IDC_O_PARENT_ID, 200, y, 50, 22);
    y += 28;
    o_btn_strings_ = mk_btn(page, IDC_O_STRINGS, L"Labels...", 8, y, 90, 24);
    o_btn_apply_ = nullptr;
    y += 30;
    o_obj_search_lbl_ = mk_static(page, L"Search objects (id/name):", 8, y, 160, 16);
    y += 16;
    o_inst_search_ = mk_edit(page, IDC_O_INST_SEARCH, 8, y, 260, 20);
    SetWindowTextW(o_inst_search_, L"");
    y += 24;
    o_obj_list_lbl_ = mk_static(page, L"Objects on map:", 8, y, 120, 16);
    y += 16;
    o_instances_ = mk_list(page, IDC_O_INST, 8, y, 260, 70);
    y += 76;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = y;
    si.nPage = 1;
    si.nPos = 0;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

void EditorSidePanel::build_splines_page(HWND page) {
    int y = 6;
    s_tile_lbl_ = mk_static(page, L"Filter tile (gx, gy or #):", 8, y, 200, 16);
    s_tile_search_ = mk_edit(page, IDC_S_TILE_SEARCH, 8, y + 16, 260, 20);
    y += 42;
    mk_static(page, L"Search types (name):", 8, y, 160, 16);
    y += 16;
    s_cat_search_ = mk_edit(page, IDC_S_CAT_SEARCH, 8, y, 260, 20);
    y += 24;
    s_types_lbl_ = mk_static(page, L"Actual Spline Types:", 8, y, 200, 16);
    y += 18;
    s_catalog_ = mk_list(page, IDC_S_CATALOG, 8, y, 260, 100);
    y += 104;
    s_btn_load_ = mk_btn(page, IDC_S_LOAD, L"Load...", 8, y, 64, 24);
    s_btn_create_ = mk_btn(page, IDC_S_CREATE, L"Crear...", 76, y, 64, 24);
    s_btn_parent_ = mk_btn(page, 0, L"Set New Parent...", 148, y, 120, 24);
    y += 28;
    mk_btn(page, IDC_S_EDITFILE, L"Edit .sli", 8, y, 72, 22);
    mk_btn(page, IDC_S_FOLDER, L"Open folder", 84, y, 78, 22);
    mk_btn(page, IDC_S_RELOAD, L"Reload", 166, y, 52, 22);
    mk_btn(page, IDC_S_OPENMAP, L"Open .map", 222, y, 46, 22);
    y += 28;
    s_chk_spawn_terr_ = mk_check(page, IDC_S_SPAWN_TERR, L"Pegar a terreno (.terrain)", 8, y, 260, 20);
    y += 22;
    s_chk_spawn_mesh_ = mk_check(page, IDC_S_SPAWN_MESH, L"Encima de mallas (.o3d cuerpo)", 8, y, 260, 20);
    y += 22;
    s_chk_spawn_align_ = mk_check(page, IDC_S_SPAWN_ALIGN, L"Alinear pitch y bank", 8, y, 260, 20);
    y += 26;
    s_btn_split_ = mk_btn(page, IDC_S_SPLIT, L"Split!", 90, y, 80, 24);
    y += 30;
    mk_static(page, L"z:", 8, y, 20, 16);
    s_z_ = mk_edit(page, IDC_S_Z, 28, y - 2, 134, 20);
    mk_btn(page, IDC_S_Z_UP, L"\x25B2", 168, y - 1, 18, 11);
    mk_btn(page, IDC_S_Z_DN, L"\x25BC", 168, y + 10, 18, 11);
    mk_static(page, L"m", 192, y, 14, 16);
    y += 24;
    mk_static(page, L"rot:", 8, y, 28, 16);
    s_rot_trk_ = mk_scalar_track(page, IDC_S_ROT_TRK, 36, y + 2, 128, kRotSpec);
    s_rot_ = mk_edit(page, IDC_S_ROT, 168, y - 2, 46, 20);
    mk_btn(page, IDC_S_ROT_UP, L"\x25B2", 216, y - 1, 18, 11);
    mk_btn(page, IDC_S_ROT_DN, L"\x25BC", 216, y + 10, 18, 11);
    mk_static(page, L"°", 236, y, 12, 16);
    y += 26;
    mk_static(page, L"Start Skew:", 8, y, 70, 16);
    s_skew0_trk_ = mk_scalar_track(page, IDC_S_SKEW0_TRK, 80, y + 2, 84, kSkewSpec);
    s_skew0_ = mk_edit(page, IDC_S_SKEW0, 168, y - 2, 46, 20);
    mk_btn(page, IDC_S_SKEW0_UP, L"\x25B2", 216, y - 1, 18, 11);
    mk_btn(page, IDC_S_SKEW0_DN, L"\x25BC", 216, y + 10, 18, 11);
    y += 26;
    mk_static(page, L"End Skew:", 8, y, 70, 16);
    s_skew1_trk_ = mk_scalar_track(page, IDC_S_SKEW1_TRK, 80, y + 2, 84, kSkewSpec);
    s_skew1_ = mk_edit(page, IDC_S_SKEW1, 168, y - 2, 46, 20);
    mk_btn(page, IDC_S_SKEW1_UP, L"\x25B2", 216, y - 1, 18, 11);
    mk_btn(page, IDC_S_SKEW1_DN, L"\x25BC", 216, y + 10, 18, 11);
    y += 26;
    mk_static(page, L"Length:", 8, y, 50, 16);
    s_len_trk_ = mk_scalar_track(page, IDC_S_LEN_TRK, 58, y + 2, 106, kLenSpec);
    s_len_ = mk_edit(page, IDC_S_LEN, 168, y - 2, 46, 20);
    mk_btn(page, IDC_S_LEN_UP, L"\x25B2", 216, y - 1, 18, 11);
    mk_btn(page, IDC_S_LEN_DN, L"\x25BC", 216, y + 10, 18, 11);
    mk_static(page, L"m", 236, y, 14, 16);
    y += 26;
    mk_static(page, L"Angel:", 8, y, 40, 16); // OMSI spelling
    s_angle_ = mk_static(page, L"0", 50, y, 50, 16);
    mk_static(page, L"°", 100, y, 12, 16);
    s_btn_align_start_ = mk_btn(page, IDC_S_ALIGN_START, L"Align Start to Obj.", 118, y - 2, 132, 22);
    y += 26;
    s_btn_align_end_ = mk_btn(page, IDC_S_ALIGN_END, L"Align End to Obj.", 118, y - 2, 132, 22);
    y += 26;
    mk_static(page, L"Radius:", 8, y, 50, 16);
    s_rad_trk_ = mk_scalar_track(page, IDC_S_RAD_TRK, 58, y + 2, 106, kRadSpec);
    s_radius_ = mk_edit(page, IDC_S_RAD, 168, y - 2, 46, 20);
    mk_btn(page, IDC_S_RAD_UP, L"\x25B2", 216, y - 1, 18, 11);
    mk_btn(page, IDC_S_RAD_DN, L"\x25BC", 216, y + 10, 18, 11);
    mk_static(page, L"m", 236, y, 14, 16);
    y += 28;
    mk_static(page, L"Gradient start:", 8, y, 90, 16);
    s_g0_ = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_NOTIFYBEFOREMOVE, 8, y + 14,
                          158, 22, page, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_S_G0)),
                          GetModuleHandleW(nullptr), nullptr);
    SendMessageW(s_g0_, TBM_SETRANGE, TRUE, MAKELPARAM(kGradTrackMin, kGradTrackMax));
    SendMessageW(s_g0_, TBM_SETPOS, TRUE, kGradTrackOffset);
    s_g0_edit_ = mk_edit(page, IDC_S_G0_EDIT, 168, y + 12, 46, 20);
    s_g0_up_ = mk_btn(page, IDC_S_G0_UP, L"\x25B2", 216, y + 11, 18, 11);
    s_g0_dn_ = mk_btn(page, IDC_S_G0_DN, L"\x25BC", 216, y + 22, 18, 11);
    mk_static(page, L"%", 236, y + 14, 14, 16);
    y += 36;
    mk_static(page, L"Gradient end:", 8, y, 90, 16);
    s_g1_ = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_NOTIFYBEFOREMOVE, 8, y + 14,
                          158, 22, page, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_S_G1)),
                          GetModuleHandleW(nullptr), nullptr);
    SendMessageW(s_g1_, TBM_SETRANGE, TRUE, MAKELPARAM(kGradTrackMin, kGradTrackMax));
    SendMessageW(s_g1_, TBM_SETPOS, TRUE, kGradTrackOffset);
    s_g1_edit_ = mk_edit(page, IDC_S_G1_EDIT, 168, y + 12, 46, 20);
    s_g1_up_ = mk_btn(page, IDC_S_G1_UP, L"\x25B2", 216, y + 11, 18, 11);
    s_g1_dn_ = mk_btn(page, IDC_S_G1_DN, L"\x25BC", 216, y + 22, 18, 11);
    mk_static(page, L"%", 236, y + 14, 14, 16);
    y += 36;
    s_dh_check_ = mk_check(page, IDC_S_DH_CHK, L"delta h =", 8, y, 80, 18);
    s_dh_ = mk_edit(page, IDC_S_DH, 90, y - 2, 50, 20);
    mk_static(page, L"m", 142, y, 14, 16);
    y += 22;
    mk_static(page, L"Cant start:", 8, y, 70, 16);
    s_cant0_trk_ = mk_scalar_track(page, IDC_S_CANT0_TRK, 80, y + 2, 84, kCantSpec);
    s_cant0_ = mk_edit(page, IDC_S_CANT0, 168, y - 2, 46, 20);
    mk_btn(page, IDC_S_CANT0_UP, L"\x25B2", 216, y - 1, 18, 11);
    mk_btn(page, IDC_S_CANT0_DN, L"\x25BC", 216, y + 10, 18, 11);
    mk_static(page, L"%", 236, y, 14, 16);
    y += 26;
    mk_static(page, L"Cant end:", 8, y, 70, 16);
    s_cant1_trk_ = mk_scalar_track(page, IDC_S_CANT1_TRK, 80, y + 2, 84, kCantSpec);
    s_cant1_ = mk_edit(page, IDC_S_CANT1, 168, y - 2, 46, 20);
    mk_btn(page, IDC_S_CANT1_UP, L"\x25B2", 216, y - 1, 18, 11);
    mk_btn(page, IDC_S_CANT1_DN, L"\x25BC", 216, y + 10, 18, 11);
    mk_static(page, L"%", 236, y, 14, 16);
    y += 26;
    s_mirror_ = mk_check(page, IDC_S_MIRROR, L"Mirror", 8, y, 70, 18);
    mk_static(page, L"Align Terrain:", 90, y, 80, 16);
    s_align_terr_ = mk_combo(page, 0, 172, y - 2, 90, 120);
    SendMessageW(s_align_terr_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
    SendMessageW(s_align_terr_, CB_SETCURSEL, 0, 0);
    y += 26;
    s_btn_complete_ = mk_btn(page, IDC_S_COMPLETE, L"Complete to...", 8, y, 100, 24);
    mk_static(page, L"max. Radius:", 120, y, 80, 16);
    s_max_radius_ = mk_edit(page, IDC_S_MAX_RAD, 200, y, 50, 22);
    set_edit_float(s_max_radius_, 200.f);
    y += 30;
    s_btn_apply_ = nullptr;
    s_chk_ends_ = mk_check(page, IDC_S_ENDS, L"Ends", 90, y + 2, 50, 18);
    s_chk_path_ends_ = mk_check(page, IDC_S_PATH_ENDS, L"PathEnds", 145, y + 2, 70, 18);
    s_chk_path_rails_ = mk_check(page, IDC_S_PATH_RAILS, L"Rails", 220, y + 2, 50, 18);
    SendMessageW(s_chk_ends_, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(s_chk_path_ends_, BM_SETCHECK, BST_CHECKED, 0);
    y += 24;
    s_chk_autofill_ = mk_check(page, IDC_S_AUTOFILL, L"Autofill to clicked end", 8, y, 200, 18);
    y += 22;
    mk_static(page, L"Search splines (id/name):", 8, y, 160, 16);
    y += 16;
    s_inst_search_ = mk_edit(page, IDC_S_INST_SEARCH, 8, y, 260, 20);
    SetWindowTextW(s_inst_search_, L"");
    y += 24;
    mk_static(page, L"Splines on map:", 8, y, 120, 16);
    y += 16;
    s_instances_ = mk_list(page, IDC_S_INST, 8, y, 260, 60);
    y += 66;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = y;
    si.nPage = 1;
    si.nPos = 0;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

void EditorSidePanel::build_traffic_page(HWND page) {
    int y = 8;
    t_uvg_lbl_ = mk_static(page, L"Set Traffic Density for unsched. vehicle group:", 8, y, 270, 32);
    y += 34;
    t_uvg_list_ = mk_list(page, IDC_T_UVG, 8, y, 260, 70);
    y += 78;
    t_grp_ = CreateWindowW(L"BUTTON", L"Set Rules", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 4, y, 272, 320, page,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
    int ry = y + 18;
    t_radios_.clear();
    t_radios_.resize(static_cast<std::size_t>(RuleRadio::Count));
    for (int i = 0; i < static_cast<int>(RuleRadio::Count); ++i) {
        const int id = IDC_T_RADIO0 + i;
        const int w = (i == 0) ? 100 : 250;
        t_radios_[static_cast<std::size_t>(i)] =
            mk_radio(page, id, kRuleRadioLabels[i], 12, ry, w, 18, i == 0);
        if (i == 0) {
            t_speed_ = mk_edit(page, IDC_T_SPEED, 120, ry - 2, 40, 20);
            set_edit_float(t_speed_, 30.f);
        }
        ry += 20;
    }
    SendMessageW(t_radios_[0], BM_SETCHECK, BST_CHECKED, 0);
    y += 326;
    t_btn_apply_ = nullptr;
    y += 30;
    t_hint_ = mk_static(page,
                        L"Click a path in the viewport to paint the selected rule (OMSI).", 8, y, 270, 36);
}
bool EditorSidePanel::open(HWND owner) {
    close();
    if (!owner) return false;
    owner_ = owner;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);

    bg_brush_ = CreateSolidBrush(RGB(240, 240, 240));
    font_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Sans Serif");
    // Vertical text for tabs (900 = 90°)
    font_vert_ = CreateFontW(-11, 0, 900, 900, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Sans Serif");

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OmsiF4EditorSide";
    wc.hbrBackground = bg_brush_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    WNDCLASSW pc{};
    pc.lpfnWndProc = page_proc;
    pc.hInstance = GetModuleHandleW(nullptr);
    pc.lpszClassName = L"OmsiF4EditorSidePage";
    pc.hbrBackground = bg_brush_;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&pc);

    WNDCLASSW tc{};
    tc.lpfnWndProc = toolbar_proc;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.lpszClassName = L"OmsiF4EditorToolbar";
    tc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
    tc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&tc);

    RECT owner_rc{};
    GetClientRect(owner_, &owner_rc);
    const int ph = (std::max)(1, static_cast<int>(owner_rc.bottom - owner_rc.top));
    const int px = (std::max)(0, static_cast<int>(owner_rc.right - owner_rc.left) - kPanelWidth);

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_BORDER, px, 0,
                            kPanelWidth, ph, owner_, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    tab_strip_ = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, 0, 0, kTabStripW,
                               ph, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB_STRIP)),
                               GetModuleHandleW(nullptr), nullptr);
    // Clicks land on the strip child — forward mouse to parent hit-test.
    SetWindowLongPtrW(tab_strip_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowSubclass(
        tab_strip_,
        [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) -> LRESULT {
            auto* self = reinterpret_cast<EditorSidePanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self && msg == WM_LBUTTONDOWN) {
                const int y = GET_Y_LPARAM(lp);
                const int tab = self->hit_tab(1, y);
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
            return DefSubclassProc(hwnd, msg, wp, lp);
        },
        1, 0);

    auto mk_page = [&](HWND& out) {
        out = CreateWindowExW(0, L"OmsiF4EditorSidePage", L"", WS_CHILD | WS_CLIPCHILDREN | WS_VSCROLL, kContentX, 0,
                              kPanelWidth - kContentX, ph - 40, hwnd_, nullptr, GetModuleHandleW(nullptr), this);
        SetWindowLongPtrW(out, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    };
    mk_page(page_tile_);
    mk_page(page_object_);
    mk_page(page_spline_);
    mk_page(page_terrain_);
    mk_page(page_traffic_);
    mk_page(page_tracks_);
    mk_page(page_station_);
    mk_page(page_splexp_);
    mk_page(page_signal_);

    build_tile_page(page_tile_);
    build_objects_page(page_object_);
    build_splines_page(page_spline_);
    build_terrain_page(page_terrain_);
    build_traffic_page(page_traffic_);
    build_tracks_page(page_tracks_);
    build_station_page(page_station_);
    build_splexp_page(page_splexp_);
    build_signal_page(page_signal_);

    status_ = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE | SS_LEFT, kContentX, ph - 36,
                            kPanelWidth - kContentX - 8, 32, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), GetModuleHandleW(nullptr),
                            nullptr);

    const int view_w = (std::max)(1, static_cast<int>(owner_rc.right - owner_rc.left) - kPanelWidth);
    toolbar_ = CreateWindowExW(WS_EX_CONTROLPARENT, tc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 0, 0, view_w, kToolbarH, owner_, nullptr, GetModuleHandleW(nullptr), this);
    if (toolbar_) {
        SetWindowLongPtrW(toolbar_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SendMessageW(toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        btn_load_map_ = CreateWindowW(L"BUTTON", L"Load map", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 4, 3, 82, 22,
                                      toolbar_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_LOAD_MAP)),
                                      GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"Open .map", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 90, 3, 82, 22, toolbar_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_OPEN_TILE)), GetModuleHandleW(nullptr),
                      nullptr);
        btn_shortcuts_ = CreateWindowW(L"BUTTON", L"Shortcuts", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 176, 3, 82, 22,
                                       toolbar_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SHORTCUTS)),
                                       GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"Entrypoints...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 266, 3, 110, 22, toolbar_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_ENTRYPOINTS)), GetModuleHandleW(nullptr),
                      nullptr);
        CreateWindowW(L"BUTTON", L"Options...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 382, 3, 82, 22, toolbar_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_OPTIONS)), GetModuleHandleW(nullptr),
                      nullptr);
        CreateWindowW(L"BUTTON", L"Editor \u25BE", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, view_w - 86, 3, 80, 22,
                      toolbar_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_TOOLBAR_MENU)),
                      GetModuleHandleW(nullptr), nullptr);
    }

    // Propagate font to children
    EnumChildWindows(
        hwnd_,
        [](HWND c, LPARAM lp) -> BOOL {
            SendMessageW(c, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font_));

    tool_ = EditorTool::Object;
    layout_force_ = true;
    layout_children();
    sync_page_visibility();
    if (toolbar_) BringWindowToTop(toolbar_);
    BringWindowToTop(hwnd_);
    log::info("editor side panel: OMSI-style vertical tabs");
    return true;
}

void EditorSidePanel::close() {
    if (toolbar_) {
        DestroyWindow(toolbar_);
        toolbar_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (bg_brush_) {
        DeleteObject(bg_brush_);
        bg_brush_ = nullptr;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (font_vert_) {
        DeleteObject(font_vert_);
        font_vert_ = nullptr;
    }
    tab_strip_ = status_ = nullptr;
    btn_load_map_ = btn_shortcuts_ = nullptr;
    page_tile_ = page_object_ = page_spline_ = page_terrain_ = page_traffic_ = nullptr;
    page_tracks_ = page_station_ = page_splexp_ = page_signal_ = nullptr;
    t_radios_.clear();
    owner_ = nullptr;
}

void EditorSidePanel::sync_page_visibility() {
    auto reset_scroll = [](HWND page) {
        if (!page) return;
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        GetScrollInfo(page, SB_VERT, &si);
        if (si.nPos != 0)
            ScrollWindowEx(page, 0, si.nPos, nullptr, nullptr, nullptr, nullptr,
                           SW_SCROLLCHILDREN | SW_INVALIDATE);
        si.nPos = 0;
        SetScrollInfo(page, SB_VERT, &si, TRUE);
    };
    HWND pages[] = {page_tile_,     page_object_, page_spline_, page_terrain_, page_traffic_,
                    page_tracks_,   page_station_, page_splexp_, page_signal_};
    for (HWND p : pages) {
        if (!p) continue;
        ShowWindow(p, SW_HIDE);
        reset_scroll(p);
    }
    const int t = static_cast<int>(tool_);
    HWND active = nullptr;
    switch (t) {
    case 0: active = page_tile_; break;
    case 1: active = page_object_; break;
    case 2: active = page_spline_; break;
    case 3: active = page_terrain_; break;
    case 4: active = page_traffic_; break;
    case 5: active = page_tracks_; break;
    case 6: active = page_station_; break;
    case 7: active = page_splexp_; break;
    case 8: active = page_signal_; break;
    default: break;
    }
    if (active) {
        ShowWindow(active, SW_SHOW);
        SetWindowPos(active, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        reset_scroll(active);
        InvalidateRect(active, nullptr, TRUE);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    if (tab_strip_) InvalidateRect(tab_strip_, nullptr, TRUE);
}

int EditorSidePanel::hit_tab(int x, int y) const {
    if (x < 0 || x >= kTabStripW || !hwnd_) return -1;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int h = (std::max)(1, static_cast<int>(rc.bottom - rc.top) - 4);
    const int tab_h = h / kTabCount;
    if (tab_h <= 0) return -1;
    const int i = y / tab_h;
    if (i < 0 || i >= kTabCount) return -1;
    return i;
}

} // namespace omsi::ui
