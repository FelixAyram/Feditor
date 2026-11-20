#pragma once

// Pure rules for the editor crash/UI/G+Z/v327 fixpack. Mini-tests link this header
// without bringing the map-editor TU.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace omsi::editor {

inline constexpr std::uint32_t kSelectedSplineNodeColor = 0xFFFFFFFFu; // IM_COL32(255,255,255,255)
inline constexpr std::uint32_t kSelectedGradCircleColor = 0xFFFFFFFFu;

inline bool panel_pump_peeks_thread_queue(bool imgui_primary) { return !imgui_primary; }
inline bool honor_glfw_close(bool boot_ready) { return boot_ready; }
// Map switch must not go through glfwSetWindowShouldClose (that closed the process).
inline bool map_switch_uses_glfw_close() { return false; }

inline bool spray_pick_enables_paint() { return false; }
inline bool spray_can_paint(bool spray_enabled, bool spray_pick_mode) {
    return spray_enabled && !spray_pick_mode;
}
inline float spray_place_yaw_deg(bool random_rotation, float yaw_deg, float unit01) {
    if (random_rotation) return (std::clamp)(unit01, 0.f, 1.f) * 360.f;
    return yaw_deg;
}

inline bool spray_lasso_fills_while_dragging() { return false; }
inline bool circle_select_stays_armed(bool sel_mode_circle) { return sel_mode_circle; }

inline bool spawn_marker_chunk_ok(bool place_marker_pushed, bool end_markers_empty) {
    return place_marker_pushed && !end_markers_empty;
}
inline bool mesh_bake_spawns_gpu_inside_imgui() { return false; }

template <class Exists>
std::string unique_numeric_suffix(const std::string& stem, Exists&& exists) {
    if (stem.empty()) return {};
    if (!exists(stem)) return stem;
    for (unsigned suffix = 1; suffix <= 9999; ++suffix) {
        char tail[16]{};
        std::snprintf(tail, sizeof(tail), "_%02u", suffix);
        std::string candidate = stem + tail;
        if (!exists(candidate)) return candidate;
    }
    return {};
}

inline bool cmake_has_ultralight(std::string_view cmake_text) {
    return cmake_text.find("Ultralight") != std::string_view::npos ||
           cmake_text.find("WebCore") != std::string_view::npos ||
           cmake_text.find("AppCore") != std::string_view::npos ||
           cmake_text.find("OMSI_ULTRALIGHT") != std::string_view::npos;
}

// --- Busy overlay: one message, hysteresis so stream/.sli/.sco/SA cannot flicker. ---
struct BusyLatch {
    bool overlay = false;
    int hold = 0;
    int detail_hold = 0;
    std::string title;
    std::string detail;
    float progress = -1.f;
};

struct BusySource {
    bool on = false;
    int priority = 0; // higher wins; never swap same-frame
    const char* title = "";
    std::string detail;
    float progress = -1.f;
};

inline BusySource pick_busy_source(const BusySource* src, int n) {
    BusySource best{};
    for (int i = 0; i < n; ++i) {
        if (!src[i].on) continue;
        if (!best.on || src[i].priority > best.priority) best = src[i];
    }
    return best;
}

inline constexpr const char* kBusyLoadTitle = "Cargando mundo…";
inline constexpr int kBusyDetailHoldFrames = 24; // ~400 ms at 60 fps; never swap .sli/.sco/att every frame
// Bottom ##BusyToast fights Estado; load progress lives in the status bar instead.
inline constexpr bool kShowBusyToast = false;

inline bool busy_title_is_load(std::string_view t) {
    if (t.empty()) return false;
    if (t == kBusyLoadTitle || t.find("Cargando") != std::string_view::npos) return true;
    if (t.find(".sli") != std::string_view::npos || t.find(".sco") != std::string_view::npos) return true;
    if (t.find("att") != std::string_view::npos || t.find("tree") != std::string_view::npos) return true;
    if (t.find("streaming") != std::string_view::npos || t.find("Colocando") != std::string_view::npos)
        return true;
    if (t.find("Preset") != std::string_view::npos || t.find("Clonando") != std::string_view::npos)
        return true;
    if (t.find("Tile bake") != std::string_view::npos || t.find("Adaptando") != std::string_view::npos)
        return true;
    if (t.find("Guardando") != std::string_view::npos) return true;
    return false;
}

inline bool status_is_load_spam(std::string_view s) {
    return busy_title_is_load(s) || s.find("Cargando tiles") != std::string_view::npos ||
           s.find("worst=") != std::string_view::npos;
}

inline void latch_busy(BusyLatch& latch, const BusySource& src, int hold_frames = 18) {
    if (src.on) {
        latch.overlay = true;
        latch.hold = hold_frames;
        const char* raw = src.title ? src.title : "";
        const char* title = busy_title_is_load(raw) ? kBusyLoadTitle : raw;
        if (latch.title != title) latch.title = title;
        if (latch.detail != src.detail) {
            if (latch.detail.empty() || latch.detail_hold <= 0) {
                latch.detail = src.detail;
                latch.detail_hold = kBusyDetailHoldFrames;
            } else {
                --latch.detail_hold;
            }
        } else {
            latch.detail_hold = kBusyDetailHoldFrames;
        }
        latch.progress = src.progress;
        return;
    }
    if (latch.hold > 0) {
        --latch.hold;
        latch.overlay = true;
        return;
    }
    latch.overlay = false;
    latch.title.clear();
    latch.detail.clear();
    latch.progress = -1.f;
    latch.detail_hold = 0;
}

// --- Map text search: free text vs instance-id only (never prev/next). ---
inline bool map_line_is_section(std::string_view t) {
    return !t.empty() && t.front() == '[';
}

inline bool map_ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

inline std::string_view map_trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// 1-based line of the instance-id token (never prev/next / spline_index).
// [spline]/[spline_h]/[object]/[attachObj]/[splineAttachement]: flags, path, id → token 3.
// [splineAttachement_repeater]: 0, 0, spline_index, path, id → token 5.
inline int map_find_id_line(const std::vector<std::string>& lines, std::int32_t id) {
    const std::string want = std::to_string(id);
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto t = map_trim(lines[i]);
        int id_tok = 0;
        if (map_ieq(t, "[spline]") || map_ieq(t, "[spline_h]") || map_ieq(t, "[object]") ||
            map_ieq(t, "[attachobj]") || map_ieq(t, "[splineattachement]") ||
            map_ieq(t, "[splineattachment]")) {
            id_tok = 3;
        } else if (map_ieq(t, "[splineattachement_repeater]") || map_ieq(t, "[splineattachment_repeater]")) {
            id_tok = 5;
        }
        if (id_tok <= 0) continue;
        int tok = 0;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const auto tj = map_trim(lines[j]);
            if (tj.empty()) continue;
            if (map_line_is_section(tj)) break;
            ++tok;
            if (tok == id_tok && tj == want) return static_cast<int>(j + 1);
        }
    }
    return 0;
}

inline int map_find_text_line(const std::vector<std::string>& lines, std::string_view needle, int from_line_1) {
    if (needle.empty()) return 0;
    const int start = from_line_1 > 0 ? from_line_1 : 1;
    auto contains = [&](std::string_view hay) {
        if (needle.size() > hay.size()) return false;
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            bool ok = true;
            for (size_t k = 0; k < needle.size(); ++k) {
                char a = hay[i + k], b = needle[k];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                if (a != b) {
                    ok = false;
                    break;
                }
            }
            if (ok) return true;
        }
        return false;
    };
    for (int i = start; i <= static_cast<int>(lines.size()); ++i) {
        if (contains(lines[static_cast<size_t>(i - 1)])) return i;
    }
    for (int i = 1; i < start; ++i) {
        if (contains(lines[static_cast<size_t>(i - 1)])) return i;
    }
    return 0;
}

inline std::vector<std::string> map_split_lines(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else
            cur.push_back(c);
    }
    if (!cur.empty()) {
        if (cur.back() == '\r') cur.pop_back();
        out.push_back(cur);
    }
    return out;
}

// --- G/Z height: deltaH-centric. Never rotate. ---
// grab_target: 0 body, 1 center, 2 union, 3 end, 4 start.
inline bool gz_center_is_height_only(int spline_grab_target) { return spline_grab_target == 1; }
inline bool gz_z_cancels_rr(int spline_grab_target) {
    return spline_grab_target == 3 || spline_grab_target == 4 || spline_grab_target == 2;
}
inline bool rr_second_r_keeps_first_rotations() { return true; }
inline bool node_click_deselects_body() { return true; }
inline bool apply_root_x_axis_swap_matrix() { return true; } // OpenOMSI applies Frame Y/Z swap

struct GzHeightPose {
    float z0 = 0.f;
    float length = 1.f;
    float g0 = 0.f;
    float g1 = 0.f;
    bool use_delta = false;
    float delta = 0.f;
};

inline float clamp_grad(float g) { return (std::clamp)(g, -100.f, 100.f); }

inline float spline_quad_end_delta(float length, float g0, float g1) {
    const float L = (std::max)(length, 1.e-4f);
    return (g0 + g1) * L * 0.005f; // (g0+g1)*L/200
}

inline float spline_end_world_z(const GzHeightPose& p) {
    if (p.use_delta) return p.z0 + p.delta;
    return p.z0 + spline_quad_end_delta(p.length, p.g0, p.g1);
}

// Cubic mid: z0 + delta/2 - (g1-g0)*L/800
inline float spline_cubic_mid_z(float z0, float length, float g0, float g1, float delta) {
    const float L = (std::max)(length, 1.e-4f);
    return z0 + 0.5f * delta - (g1 - g0) * L / 800.f;
}

struct GzDeltaH {
    float self_z0 = 0.f;
    float self_g0 = 0.f;
    float self_g1 = 0.f;
    float self_delta = 0.f;
    bool self_use_delta = false;
    bool write_self_z0 = false;
    bool write_self_grads = false;
    bool write_self_delta = false;
    float next_g0 = 0.f;
    float next_g1 = 0.f;
    float next_delta = 0.f;
    bool next_use_delta = false;
    bool write_next = false;
    float prev_g0 = 0.f;
    float prev_g1 = 0.f;
    float prev_delta = 0.f;
    bool prev_use_delta = false;
    bool write_prev = false;
    bool restore_xy_rot = false; // Z after G cancels RR
    bool forbid_rotate = false;
};

inline GzHeightPose freeze_end_into_delta(const GzHeightPose& p) {
    GzHeightPose o = p;
    const float zend = spline_end_world_z(p);
    o.use_delta = true;
    o.delta = zend - p.z0;
    return o;
}

// Center node: freeze current end height into deltaH, retarget g0/g1 so mid follows mouse_z.
// next.g0 = self.g1; prev.g1 = self.g0; neighbor deltaH kept or created from old gradient end.
inline GzDeltaH gz_center_delta_h(const GzHeightPose& self, const GzHeightPose* prev, const GzHeightPose* next,
                                  float mouse_mid_z) {
    GzDeltaH d{};
    d.forbid_rotate = true;
    d.restore_xy_rot = true;
    const auto frozen = freeze_end_into_delta(self);
    d.self_z0 = frozen.z0;
    d.self_use_delta = true;
    d.self_delta = frozen.delta;
    d.write_self_delta = true;
    const float L = (std::max)(frozen.length, 1.e-4f);
    const float avg = 0.5f * (frozen.g0 + frozen.g1);
    const float target_mid = mouse_mid_z;
    const float rest_mid = frozen.z0 + 0.5f * frozen.delta;
    // z_mid = z0 + delta/2 - (g1-g0)*L/800  =>  g1-g0 = -800*(z_mid - rest_mid)/L
    const float diff = -800.f * (target_mid - rest_mid) / L;
    d.self_g0 = clamp_grad(avg - 0.5f * diff);
    d.self_g1 = clamp_grad(avg + 0.5f * diff);
    d.write_self_grads = true;

    if (next) {
        d.write_next = true;
        d.next_g0 = d.self_g1;
        d.next_g1 = next->g1;
        if (next->use_delta) {
            d.next_use_delta = true;
            d.next_delta = next->delta;
        } else {
            d.next_use_delta = true;
            d.next_delta = spline_quad_end_delta(next->length, next->g0, next->g1);
        }
    }
    if (prev) {
        d.write_prev = true;
        d.prev_g0 = prev->g0;
        d.prev_g1 = d.self_g0;
        if (prev->use_delta) {
            d.prev_use_delta = true;
            d.prev_delta = prev->delta;
        } else {
            d.prev_use_delta = true;
            d.prev_delta = spline_quad_end_delta(prev->length, prev->g0, prev->g1);
        }
    }
    return d;
}

// End node + Z: raise this end via deltaH, opposite on next. Grads unchanged.
inline GzDeltaH gz_end_delta_h(const GzHeightPose& self, const GzHeightPose* next, float raise, bool has_z) {
    GzDeltaH d{};
    d.restore_xy_rot = has_z;
    d.forbid_rotate = has_z;
    const auto frozen = freeze_end_into_delta(self);
    d.self_z0 = frozen.z0;
    d.self_g0 = frozen.g0;
    d.self_g1 = frozen.g1;
    d.self_use_delta = true;
    d.self_delta = frozen.delta + raise;
    d.write_self_delta = true;
    if (next) {
        d.write_next = true;
        d.next_g0 = next->g0;
        d.next_g1 = next->g1;
        d.next_use_delta = true;
        const float nd0 = next->use_delta ? next->delta : spline_quad_end_delta(next->length, next->g0, next->g1);
        d.next_delta = nd0 - raise;
    }
    return d;
}

// Start node without prev + Z: raise z0, counteract with deltaH so previous end stays.
inline GzDeltaH gz_start_delta_h(const GzHeightPose& self, float raise, bool has_z) {
    GzDeltaH d{};
    d.restore_xy_rot = has_z;
    d.forbid_rotate = has_z;
    const auto frozen = freeze_end_into_delta(self);
    d.self_z0 = frozen.z0 + raise;
    d.write_self_z0 = true;
    d.self_g0 = frozen.g0;
    d.self_g1 = frozen.g1;
    d.self_use_delta = true;
    d.self_delta = frozen.delta - raise;
    d.write_self_delta = true;
    return d;
}

// Split heading at along t: geometric heading minus 2*atan(skew_at).
inline float split_heading_deg_with_skew(float heading_rad, float skew_start, float skew_end, float t01) {
    const float t = (std::clamp)(t01, 0.f, 1.f);
    const float skew = skew_start + t * (skew_end - skew_start);
    constexpr float kPi = 3.14159265358979323846f;
    float deg = (heading_rad - 2.f * std::atan(skew)) * (180.f / kPi);
    while (deg > 180.f) deg -= 360.f;
    while (deg <= -180.f) deg += 360.f;
    return deg;
}

inline float split_skew_at(float skew_start, float skew_end, float t01) {
    const float t = (std::clamp)(t01, 0.f, 1.f);
    return skew_start + t * (skew_end - skew_start);
}

// Second R must keep live yaw (not group_start_yaw).
inline float rr_keep_yaw(float live_yaw, float start_yaw, bool keep_first_r) {
    return keep_first_r ? live_yaw : start_yaw;
}

// Path balls within merge_m share one handle; click asks start vs end if both sides exist.
inline bool path_balls_should_merge(float dx, float dy, float merge_m) {
    return dx * dx + dy * dy <= merge_m * merge_m;
}

} // namespace omsi::editor
