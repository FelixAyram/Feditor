#pragma once

#include <GLFW/glfw3.h>

#include <functional>
#include <optional>

#include "f4f23ma/math.hpp"

namespace omsi::input {

// Map glfw cursor (screen coords) → framebuffer pixels.
// Needed on Windows HiDPI (e.g. 4K @ 200%): see GLFW issues #2530 / #845.
void cursor_to_framebuffer(GLFWwindow* window, float& out_x, float& out_y, float& out_fb_w,
                           float& out_fb_h);
// Walk-edit mode (F7): the editor's cursor is the centre of the view (the crosshair) - picking, hover and
// placement aim where the walker looks.
void set_cursor_center_override(bool on);
[[nodiscard]] bool cursor_center_override();

// If filter returns true, wheel is consumed (e.g. UI panel) and not applied to camera zoom.
using ScrollFilterFn = std::function<bool(double xoffset, double yoffset)>;
void set_scroll_filter(ScrollFilterFn filter);

// Far plane of luna_proj_matrix (reverse-Z, metres). Default 10 km. BeamNG raises it to
// max(far, LevelInfo.visibleDistance * 1.2) so huge distant TStatic meshes are not clipped.
void set_luna_far_plane(float far_m);
[[nodiscard]] float luna_far_plane();

// Frank Luna d3d12book BoxApp camera, with optional OMSI Z-up orbit target.
struct LunaCamera {
    float theta = 1.5f * 3.14159265f;
    float phi = 0.785398f;
    float radius = 5.f;
    float min_radius = 1.f;
    float max_radius = 2000.f;
    float target_x = 0.f;
    float target_y = 0.f;
    float target_z = 0.f;
    bool z_up = true;
    // tt-viewer VIEW_MIRROR_X: top-down matches OMSI Editor.
    bool mirror_x = false;
    float ground_z = 0.f;
    // Snap the orbit target to the sampled ground (mesh/object surface). Off = keep the plane target
    // (useful in BeamNG where the heightfield lies far below the real floor). Toggled with F8 in BeamNG mode.
    bool ground_follow = true;
    // Optional heightfield sample (editor XY). nullopt = keep previous z (hf job empty).
    std::function<std::optional<float>(float x, float y)> sample_ground_xy;
    // Blender-style numpad 5: toggle orthographic / perspective projection.
    bool orthographic = false;

    // Map editor navigation (strict):
    //  RMB click  → teleport target (keep zoom + view angles)
    //  RMB drag Y → zoom
    //  MMB drag   → orbit
    //  LMB        → unused here (selection handled by app)
    bool editor_nav = false;
    // UV canvas / other HUD: skip wheel zoom and MMB orbit this frame.
    bool suppress_pointer = false;
    // Text field focused (app sets it): WASDQE must type, not fly.
    bool suppress_keys = false;
    // Drive / follow-cam: keep target on the vehicle; wheel only changes radius (no ground dolly).
    bool orbit_target_locked = false;

    // True once after an RMB ground teleport (editor_nav). Cleared by consume_ground_teleport().
    // out_* = editor-space target written on click (not live camera.target — wheel/orbit must not retrigger stream).
    [[nodiscard]] bool consume_ground_teleport(float* out_editor_x = nullptr, float* out_editor_y = nullptr,
                                               float* out_editor_z = nullptr);

    // ── BeamNG free-flight camera (F11, only enabled by the app when cfg.beamng) ──────────────────
    // Keeps an eye position + yaw/pitch, and stores target = eye + forward * kFlyRadius every frame,
    // so eye_position(), the view/proj builders and the picking rays keep working unchanged.
    // editor_nav (OMSI) is untouched: this branch replaces it only while fly_mode is true.
    // OMSI default is false; BeamNG setup turns it ON right after placing the spawn camera (F11 toggles).
    bool fly_mode = false;
    float fly_speed = 30.f;         // base m/s: Shift x4, Ctrl /4, RMB+wheel x1.25 per tick (0.5..2000)
    bool fly_speed_changed = false; // set by update() when the wheel changed fly_speed (app shows it)
    bool has_fly_pivot = false;     // Alt+LMB orbit pivot (selection); false = 20 m ahead of the eye
    float fly_pivot_x = 0.f;
    float fly_pivot_y = 0.f;
    float fly_pivot_z = 0.f;
    // Seed eye/yaw/pitch from the current orbit pose (call when F11 turns fly mode on).
    void sync_fly_from_orbit();
    // The app drains the wheel for the brush tools; while flying it forwards the ticks here instead.
    void add_fly_wheel(float delta);

    void reset_mouse();
    void stop_orbit_inertia();
    void frame_bounds(float min_x, float min_y, float min_z, float max_x, float max_y, float max_z);
    void frame_omsi_bounds(float min_x, float min_y, float min_z, float max_x, float max_y, float max_z) {
        z_up = true;
        frame_bounds(min_x, min_y, min_z, max_x, max_y, max_z);
    }
    void update(GLFWwindow* window);
    // Consume pending GLFW wheel delta (and clear it). Use when UI should eat scroll instead of zoom.
    static float take_scroll_wheel_delta();
    static void ensure_scroll_callback(GLFWwindow* window);
    void build_world_view_proj(float aspect, float out_transposed_wvp[16]) const;
    void build_world_view_proj(float aspect, const float world_row_major_4x4[16],
                               float out_transposed_wvp[16]) const;
    void build_view_proj_ma4(float aspect, f4f23ma::ma4& out) const;
    void build_world_view(float aspect, const float world_row_major_4x4[16],
                          float out_transposed_wv[16]) const;
    void build_view_rotation_transposed(float out_transposed_view_rot[16]) const;
    void build_axis_gizmo_wvp(float out_transposed_wvp[16]) const;

    void eye_position(float& out_x, float& out_y, float& out_z) const;
    void set_top_down_view();
    void toggle_orthographic();
    // mouse_x/y and width/height must be framebuffer pixels (use cursor_to_framebuffer).
    [[nodiscard]] std::optional<f4f23ma::f4> hit_ground_xy(float mouse_x, float mouse_y, float width,
                                                           float height) const;
    // Ray in world space (Z-up) for 3D picking — same unproject as hit_ground_xy.
    struct PickRay {
        f4f23ma::f4 origin{};
        f4f23ma::f4 dir{}; // normalized
    };
    [[nodiscard]] std::optional<PickRay> make_pick_ray(float mouse_x, float mouse_y, float width,
                                                         float height) const;

private:
    double last_x_ = 0.0;
    double last_y_ = 0.0;
    bool has_last_ = false;
    bool rmb_was_down_ = false;
    bool mmb_was_down_ = false;
    // Light orbit inertia after MMB release (no shake).
    float orbit_vel_theta_ = 0.f;
    float orbit_vel_phi_ = 0.f;
    float rmb_drag_px_ = 0.f;
    float rmb_down_fb_x_ = 0.f;
    float rmb_down_fb_y_ = 0.f;
    float rmb_fb_w_ = 1.f;
    float rmb_fb_h_ = 1.f;
    bool ground_teleport_pending_ = false;
    float teleport_x_ = 0.f;
    float teleport_y_ = 0.f;
    float teleport_z_ = 0.f;
    // v165 O: RMB teleport ease-out (~0.2 s), same family as MMB orbit inertia.
    bool pan_lerp_active_ = false;
    float pan_lerp_t_ = 0.f;
    float pan_from_x_ = 0.f, pan_from_y_ = 0.f, pan_from_z_ = 0.f;
    float pan_to_x_ = 0.f, pan_to_y_ = 0.f, pan_to_z_ = 0.f;
    // Free-flight state (see fly_mode above).
    void update_fly(GLFWwindow* window, float dx_px, float dy_px, bool lmb, bool rmb, bool mmb);
    bool fly_init_ = false;
    bool fly_has_last_ = false;
    double fly_last_x_ = 0.0;
    double fly_last_y_ = 0.0;
    double fly_last_time_ = 0.0;
    float fly_eye_x_ = 0.f;
    float fly_eye_y_ = 0.f;
    float fly_eye_z_ = 0.f;
    float fly_yaw_ = 0.f;
    float fly_pitch_ = 0.f;
    float fly_wheel_pending_ = 0.f;
    // Last target written by the fly cam, to notice an external focus (spawn / selection / objects).
    bool fly_target_valid_ = false;
    float fly_target_x_ = 0.f;
    float fly_target_y_ = 0.f;
    float fly_target_z_ = 0.f;
};

} // namespace omsi::input
