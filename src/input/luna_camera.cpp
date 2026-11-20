#include "luna_camera.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace omsi::input {
namespace {

float g_wheel_delta = 0.f;
float g_luna_far = 10000.f;
bool g_scroll_hooked = false;
ScrollFilterFn g_scroll_filter;
GLFWscrollfun g_prev_scroll = nullptr;

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    if (g_scroll_filter && g_scroll_filter(xoffset, yoffset)) return;
    // Chain to previous callback (Dear ImGui installs one at Init).
    // Official ImGui+GLFW pattern: never replace without forwarding.
    if (g_prev_scroll) g_prev_scroll(window, xoffset, yoffset);
    g_wheel_delta += static_cast<float>(yoffset);
}

} // namespace

void set_scroll_filter(ScrollFilterFn filter) { g_scroll_filter = std::move(filter); }

void set_luna_far_plane(float far_m) { g_luna_far = (std::max)(far_m, 10000.f); }
float luna_far_plane() { return g_luna_far; }

DirectX::XMMATRIX luna_proj_matrix(float aspect, bool orthographic, float radius, bool mirror_x) {
    // Reverse-Z (DirectXTK): swap near/far so near→1 far→0; pairs with GREATER_EQUAL + clear 0.
    // Fixed 1 m near plane clipped everything within 1 m of the eye at low camera angles (meshes looked cut /
    // see-through). Reverse-Z keeps depth precision, so the near plane shrinks with the orbit radius.
    const float kNear = (std::clamp)(radius * 0.02f, 0.1f, 1.f);
    const float kFar = luna_far_plane();
    DirectX::XMMATRIX proj{};
    if (orthographic) {
        const float half_h = (std::max)(radius * 0.45f, 5.f);
        const float half_w = half_h * aspect;
        proj = DirectX::XMMatrixOrthographicLH(2.f * half_w, 2.f * half_h, kFar, kNear);
    } else {
        proj = DirectX::XMMatrixPerspectiveFovLH(0.25f * DirectX::XM_PI, aspect, kFar, kNear);
    }
    if (mirror_x) proj = proj * DirectX::XMMatrixScaling(-1.f, 1.f, 1.f);
    return proj;
}

DirectX::XMMATRIX luna_view_matrix(float target_x, float target_y, float target_z, float radius, float theta,
                                   float phi, bool z_up) {
    float ex = 0.f;
    float ey = 0.f;
    float ez = 0.f;
    if (z_up) {
        ex = target_x + radius * std::sinf(phi) * std::cosf(theta);
        ey = target_y + radius * std::sinf(phi) * std::sinf(theta);
        ez = target_z + radius * std::cosf(phi);
    } else {
        ex = target_x + radius * std::sinf(phi) * std::cosf(theta);
        ey = target_y + radius * std::cosf(phi);
        ez = target_z + radius * std::sinf(phi) * std::sinf(theta);
    }

    const DirectX::XMVECTOR pos = DirectX::XMVectorSet(ex, ey, ez, 1.f);
    const DirectX::XMVECTOR target = DirectX::XMVectorSet(target_x, target_y, target_z, 1.f);
    const DirectX::XMVECTOR up =
        z_up ? DirectX::XMVectorSet(0.f, 0.f, 1.f, 0.f) : DirectX::XMVectorSet(0.f, 1.f, 0.f, 0.f);
    return DirectX::XMMatrixLookAtLH(pos, target, up);
}

DirectX::XMMATRIX load_world_row_major(const float world_row_major_4x4[16]) {
    DirectX::XMFLOAT4X4 wf{};
    std::memcpy(&wf, world_row_major_4x4, sizeof(wf));
    return DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&wf));
}

void store_transposed(const DirectX::XMMATRIX& m, float out_transposed[16]) {
    DirectX::XMFLOAT4X4 stored{};
    DirectX::XMStoreFloat4x4(&stored, DirectX::XMMatrixTranspose(m));
    std::memcpy(out_transposed, &stored, sizeof(stored));
}

f4f23ma::ma4 xm_to_ma4(const DirectX::XMMATRIX& m) {
    DirectX::XMFLOAT4X4 f{};
    DirectX::XMStoreFloat4x4(&f, m);
    f4f23ma::ma4 out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.m[c][r] = f.m[r][c];
    return out;
}

namespace {
bool g_cursor_center = false;
}
void set_cursor_center_override(bool on) { g_cursor_center = on; }
bool cursor_center_override() { return g_cursor_center; }

void cursor_to_framebuffer(GLFWwindow* window, float& out_x, float& out_y, float& out_fb_w,
                           float& out_fb_h) {
    int win_w = 1, win_h = 1, fb_w = 1, fb_h = 1;
    glfwGetWindowSize(window, &win_w, &win_h);
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    win_w = (std::max)(win_w, 1);
    win_h = (std::max)(win_h, 1);
    fb_w = (std::max)(fb_w, 1);
    fb_h = (std::max)(fb_h, 1);
    double cx = 0.0, cy = 0.0;
    glfwGetCursorPos(window, &cx, &cy);
    // GLFW cursor = screen coords; Unproject / DX12 viewport = framebuffer pixels.
    out_x = static_cast<float>(cx) * (static_cast<float>(fb_w) / static_cast<float>(win_w));
    out_y = static_cast<float>(cy) * (static_cast<float>(fb_h) / static_cast<float>(win_h));
    if (g_cursor_center) {
        out_x = 0.5f * static_cast<float>(fb_w);
        out_y = 0.5f * static_cast<float>(fb_h);
    }
    out_fb_w = static_cast<float>(fb_w);
    out_fb_h = static_cast<float>(fb_h);
}

void LunaCamera::stop_orbit_inertia() {
    orbit_vel_theta_ = 0.f;
    orbit_vel_phi_ = 0.f;
    pan_lerp_active_ = false;
}

void LunaCamera::reset_mouse() {
    has_last_ = false;
    rmb_was_down_ = false;
    mmb_was_down_ = false;
    rmb_drag_px_ = 0.f;
    ground_teleport_pending_ = false;
    fly_has_last_ = false;
}

// ── BeamNG free-flight camera ────────────────────────────────────────────────────────────────────
namespace {
// Fixed target distance: every view/proj/pick path keeps deriving the eye from target+radius+theta+phi.
constexpr float kFlyRadius = 10.f;
// ~84.2 deg, same margin as the editor orbit clamp (editor_nav uses 0.1 .. PI-0.1).
constexpr float kFlyPitchLimit = 1.5708f - 0.1f;
constexpr float kFlyMouseRadPerPx = 0.25f * 0.01745329251994329577f; // same feel as the editor orbit

void fly_forward(float yaw, float pitch, float out[3]) {
    const float cp = std::cosf(pitch);
    out[0] = cp * std::cosf(yaw);
    out[1] = cp * std::sinf(yaw);
    out[2] = std::sinf(pitch);
}
} // namespace

void LunaCamera::add_fly_wheel(float delta) { fly_wheel_pending_ += delta; }

void LunaCamera::sync_fly_from_orbit() {
    // BeamNG is Z-up; theta/phi are rebuilt from yaw/pitch with the same convention every frame.
    z_up = true;
    eye_position(fly_eye_x_, fly_eye_y_, fly_eye_z_);
    const float dx = target_x - fly_eye_x_;
    const float dy = target_y - fly_eye_y_;
    const float dz = target_z - fly_eye_z_;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len > 1.e-4f) {
        fly_yaw_ = std::atan2(dy, dx);
        fly_pitch_ = (std::clamp)(std::asinf((std::clamp)(dz / len, -1.f, 1.f)), -kFlyPitchLimit, kFlyPitchLimit);
    }
    fly_pitch_ = (std::clamp)(fly_pitch_, -kFlyPitchLimit, kFlyPitchLimit);
    float fwd[3]{};
    fly_forward(fly_yaw_, fly_pitch_, fwd);
    // Keep the eye where it is and put the target 10 m ahead: toggling F11 does not move the view.
    radius = kFlyRadius;
    target_x = fly_eye_x_ + fwd[0] * radius;
    target_y = fly_eye_y_ + fwd[1] * radius;
    target_z = fly_eye_z_ + fwd[2] * radius;
    theta = fly_yaw_ + DirectX::XM_PI;
    phi = 0.5f * DirectX::XM_PI + fly_pitch_;
    fly_target_x_ = target_x;
    fly_target_y_ = target_y;
    fly_target_z_ = target_z;
    fly_target_valid_ = true;
    fly_init_ = true;
    fly_has_last_ = false;
    fly_wheel_pending_ = 0.f;
    fly_last_time_ = 0.0;
    reset_mouse();
}

void LunaCamera::update_fly(GLFWwindow* window, float dx_px, float dy_px, bool lmb, bool rmb, bool mmb) {
    if (!fly_init_) sync_fly_from_orbit();

    const double now = glfwGetTime();
    float dt = (fly_last_time_ > 0.0) ? static_cast<float>(now - fly_last_time_) : (1.f / 60.f);
    fly_last_time_ = now;
    dt = (std::clamp)(dt, 0.001f, 0.1f);

    float fwd[3]{};
    fly_forward(fly_yaw_, fly_pitch_, fwd);

    // External focus (Ir al spawn / Centrar en objetos / focus_selection) rewrote the target: keep that
    // target and re-anchor the eye 10 m back along the current view direction (no orbit snap).
    if (fly_target_valid_) {
        const float ex = target_x - fly_target_x_;
        const float ey = target_y - fly_target_y_;
        const float ez = target_z - fly_target_z_;
        if (ex * ex + ey * ey + ez * ez > 1.e-4f) {
            fly_eye_x_ = target_x - fwd[0] * kFlyRadius;
            fly_eye_y_ = target_y - fwd[1] * kFlyRadius;
            fly_eye_z_ = target_z - fwd[2] * kFlyRadius;
        }
    }

    const bool alt = glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                     glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
    const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                       glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                      glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;

    // Wheel: with RMB it sets the base speed (x1.25 per tick, 0.5..2000 m/s); without RMB it dollies
    // the eye along the view direction proportional to that speed.
    if (fly_wheel_pending_ != 0.f) {
        if (!suppress_pointer) {
            const float wheel = fly_wheel_pending_;
            if (rmb) {
                fly_speed = (std::clamp)(fly_speed * std::pow(1.25f, wheel), 0.5f, 2000.f);
                fly_speed_changed = true;
            } else {
                const float step = fly_speed * 0.5f * wheel;
                fly_eye_x_ += fwd[0] * step;
                fly_eye_y_ += fwd[1] * step;
                fly_eye_z_ += fwd[2] * step;
            }
        }
        fly_wheel_pending_ = 0.f;
    }

    if (!suppress_pointer) {
        // RMB held: mouse look (yaw/pitch), same sensitivity/sign as the OMSI editor orbit.
        if (rmb) {
            const float dtheta = kFlyMouseRadPerPx * dx_px;
            fly_yaw_ += mirror_x ? -dtheta : dtheta;
            fly_pitch_ = (std::clamp)(fly_pitch_ - kFlyMouseRadPerPx * dy_px, -kFlyPitchLimit, kFlyPitchLimit);
            fly_forward(fly_yaw_, fly_pitch_, fwd);
        }
        const float rl_h = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
        // MMB drag: lateral pan along the camera right/up axes (view follows the mouse).
        if (mmb) {
            if (rl_h > 1.e-5f) {
                const float rx = -fwd[1] / rl_h, ry = fwd[0] / rl_h;
                const float ux = -fwd[2] * fwd[0] / rl_h, uy = -fwd[2] * fwd[1] / rl_h, uz = rl_h;
                const float k = (std::max)(0.1f, fly_speed * 0.005f);
                fly_eye_x_ += -rx * dx_px * k + ux * dy_px * k;
                fly_eye_y_ += -ry * dx_px * k + uy * dy_px * k;
                fly_eye_z_ += uz * dy_px * k;
            }
        }
        // Alt+LMB drag: orbit around the pivot (selection, else the point 20 m ahead).
        if (alt && lmb) {
            float px = fly_pivot_x, py = fly_pivot_y, pz = fly_pivot_z;
            if (!has_fly_pivot) {
                px = fly_eye_x_ + fwd[0] * 20.f;
                py = fly_eye_y_ + fwd[1] * 20.f;
                pz = fly_eye_z_ + fwd[2] * 20.f;
            }
            const float vx = fly_eye_x_ - px, vy = fly_eye_y_ - py, vz = fly_eye_z_ - pz;
            const float dist = (std::max)(std::sqrt(vx * vx + vy * vy + vz * vz), 0.5f);
            const float dtheta = kFlyMouseRadPerPx * dx_px;
            fly_yaw_ += mirror_x ? -dtheta : dtheta;
            fly_pitch_ = (std::clamp)(fly_pitch_ - kFlyMouseRadPerPx * dy_px, -kFlyPitchLimit, kFlyPitchLimit);
            fly_forward(fly_yaw_, fly_pitch_, fwd);
            fly_eye_x_ = px - fwd[0] * dist;
            fly_eye_y_ = py - fwd[1] * dist;
            fly_eye_z_ = pz - fwd[2] * dist;
        }
    }

    // WASD + Q/E. Shift x4, Ctrl /4; diagonal is normalized so the base speed is kept.
    if (!suppress_keys) {
        auto down = [&](int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
        const float rl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
        float mx = 0.f, my = 0.f, mz = 0.f;
        if (down(GLFW_KEY_W)) {
            mx += fwd[0];
            my += fwd[1];
            mz += fwd[2];
        }
        if (down(GLFW_KEY_S)) {
            mx -= fwd[0];
            my -= fwd[1];
            mz -= fwd[2];
        }
        // Right vector: with the same yaw convention used by the mouse look (and by FlyCamera/EditorCamera,
        // right = forward x up) it is (fy, -fx). The old code used its exact negation, so A/D strafed the
        // wrong way in the free-flight (BeamNG) camera.
        if (rl > 1.e-5f) {
            const float rx = fwd[1] / rl, ry = -fwd[0] / rl;
            if (down(GLFW_KEY_D)) {
                mx += rx;
                my += ry;
            }
            if (down(GLFW_KEY_A)) {
                mx -= rx;
                my -= ry;
            }
        }
        // Q/E is world vertical (BeamNG is Z-up): E climbs, Q descends — independent of yaw/pitch signs.
        if (down(GLFW_KEY_E)) mz += 1.f;
        if (down(GLFW_KEY_Q)) mz -= 1.f;
        const float ml = std::sqrt(mx * mx + my * my + mz * mz);
        if (ml > 1.e-5f) {
            const float k = fly_speed * (shift ? 4.f : 1.f) * (ctrl ? 0.25f : 1.f) * dt / ml;
            fly_eye_x_ += mx * k;
            fly_eye_y_ += my * k;
            fly_eye_z_ += mz * k;
        }
    }

    // Publish the pose in the orbit representation: no height clamp, no ground teleport in this mode.
    fly_forward(fly_yaw_, fly_pitch_, fwd);
    radius = kFlyRadius;
    target_x = fly_eye_x_ + fwd[0] * radius;
    target_y = fly_eye_y_ + fwd[1] * radius;
    target_z = fly_eye_z_ + fwd[2] * radius;
    theta = fly_yaw_ + DirectX::XM_PI;
    phi = 0.5f * DirectX::XM_PI + fly_pitch_;
    fly_target_x_ = target_x;
    fly_target_y_ = target_y;
    fly_target_z_ = target_z;
    fly_target_valid_ = true;
    fly_has_last_ = true;
}

bool LunaCamera::consume_ground_teleport(float* out_editor_x, float* out_editor_y, float* out_editor_z) {
    if (!ground_teleport_pending_) return false;
    if (out_editor_x) *out_editor_x = teleport_x_;
    if (out_editor_y) *out_editor_y = teleport_y_;
    if (out_editor_z) *out_editor_z = teleport_z_;
    ground_teleport_pending_ = false;
    return true;
}

void LunaCamera::frame_bounds(float min_x, float min_y, float min_z, float max_x, float max_y, float max_z) {
    target_x = (min_x + max_x) * 0.5f;
    target_y = (min_y + max_y) * 0.5f;
    target_z = (min_z + max_z) * 0.5f;
    const float dx = max_x - min_x;
    const float dy = max_y - min_y;
    const float dz = max_z - min_z;
    const float extent = (std::max)((std::max)(dx, dy), (std::max)(dz, 0.5f));
    radius = (std::max)(extent * 2.8f, 3.f);
    theta = 0.75f * DirectX::XM_PI;
    phi = z_up ? 0.55f : 1.05f;
    reset_mouse();
}

void LunaCamera::eye_position(float& out_x, float& out_y, float& out_z) const {
    if (z_up) {
        out_x = target_x + radius * std::sinf(phi) * std::cosf(theta);
        out_y = target_y + radius * std::sinf(phi) * std::sinf(theta);
        out_z = target_z + radius * std::cosf(phi);
    } else {
        out_x = target_x + radius * std::sinf(phi) * std::cosf(theta);
        out_y = target_y + radius * std::cosf(phi);
        out_z = target_z + radius * std::sinf(phi) * std::sinf(theta);
    }
}

std::optional<LunaCamera::PickRay> LunaCamera::make_pick_ray(float mouse_x, float mouse_y, float width,
                                                              float height) const {
    if (width < 1.f || height < 1.f) return std::nullopt;
    const float aspect = width / height;
    const DirectX::XMMATRIX view = luna_view_matrix(target_x, target_y, target_z, radius, theta, phi, z_up);
    const DirectX::XMMATRIX proj = luna_proj_matrix(aspect, orthographic, radius, mirror_x);

    const DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();
    const DirectX::XMVECTOR near_p = DirectX::XMVector3Unproject(
        DirectX::XMVectorSet(mouse_x, mouse_y, 0.f, 0.f), 0.f, 0.f, width, height, 0.f, 1.f, proj, view, world);
    const DirectX::XMVECTOR far_p = DirectX::XMVector3Unproject(
        DirectX::XMVectorSet(mouse_x, mouse_y, 1.f, 0.f), 0.f, 0.f, width, height, 0.f, 1.f, proj, view, world);

    DirectX::XMFLOAT3 n{}, f{};
    DirectX::XMStoreFloat3(&n, near_p);
    DirectX::XMStoreFloat3(&f, far_p);

    PickRay ray{};
    ray.origin = {n.x, n.y, n.z, 1.f};
    ray.dir = f4f23ma::f4{f.x - n.x, f.y - n.y, f.z - n.z, 0.f}.normalized3();
    return ray;
}

std::optional<f4f23ma::f4> LunaCamera::hit_ground_xy(float mouse_x, float mouse_y, float width,
                                                     float height) const {
    const auto ray = make_pick_ray(mouse_x, mouse_y, width, height);
    if (!ray) return std::nullopt;
    if (std::fabs(ray->dir.z) < 1e-6f) return std::nullopt;
    float plane_z = ground_z;
    f4f23ma::f4 hit{};
    for (int iter = 0; iter < 3; ++iter) {
        const float t = (plane_z - ray->origin.z) / ray->dir.z;
        if (t < 0.f) return std::nullopt;
        hit = ray->origin + ray->dir * t;
        if (!ground_follow || !sample_ground_xy) break;
        const auto sampled = sample_ground_xy(hit.x, hit.y);
        if (!sampled) break;
        plane_z = *sampled;
        hit.z = plane_z;
    }
    return hit;
}

void LunaCamera::ensure_scroll_callback(GLFWwindow* window) {
    if (!window) return;
    // Reclaim ownership but keep chaining to whoever was installed (usually ImGui).
    const GLFWscrollfun prev = glfwSetScrollCallback(window, scroll_callback);
    if (prev && prev != scroll_callback) g_prev_scroll = prev;
    g_scroll_hooked = true;
}

float LunaCamera::take_scroll_wheel_delta() {
    const float d = g_wheel_delta;
    g_wheel_delta = 0.f;
    return d;
}

void LunaCamera::update(GLFWwindow* window) {
    ensure_scroll_callback(window);

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    float fb_x = 0.f, fb_y = 0.f, fb_w = 1.f, fb_h = 1.f;
    cursor_to_framebuffer(window, fb_x, fb_y, fb_w, fb_h);

    const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool rmb = !suppress_pointer && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool mmb = !suppress_pointer && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

    // BeamNG free flight (cfg.beamng only): completely replaces the OMSI editor nav below.
    if (fly_mode && z_up) {
        const float fdx = fly_has_last_ ? static_cast<float>(x - fly_last_x_) : 0.f;
        const float fdy = fly_has_last_ ? static_cast<float>(y - fly_last_y_) : 0.f;
        g_wheel_delta = 0.f; // the app forwards fly ticks through add_fly_wheel()
        update_fly(window, fdx, fdy, lmb, rmb, mmb);
        fly_last_x_ = x;
        fly_last_y_ = y;
        has_last_ = false; // leaving fly mode restarts the orbit drag tracking cleanly
        return;
    }

    if (suppress_pointer && g_wheel_delta != 0.f) g_wheel_delta = 0.f;
    // Map editor: wheel is for brush/aerosol size (consumed by the app). Zoom = RMB drag.
    if (editor_nav && g_wheel_delta != 0.f) g_wheel_delta = 0.f;
    if (g_wheel_delta != 0.f) {
        const float old_r = radius;
        const float new_r = std::clamp(old_r * std::pow(0.9f, g_wheel_delta), min_radius, max_radius);
        // Follow-cam: zoom in place. Dolly-to-ground would fight the locked target next tick.
        if (!orbit_target_locked) {
            if (const auto hit = hit_ground_xy(fb_x, fb_y, fb_w, fb_h)) {
                const float t = (old_r > 1e-4f) ? (new_r / old_r) : 1.f;
                target_x = hit->x + (target_x - hit->x) * t;
                target_y = hit->y + (target_y - hit->y) * t;
                if (sample_ground_xy && ground_follow) {
                    if (const auto z = sample_ground_xy(target_x, target_y)) {
                        target_z = *z;
                        ground_z = *z;
                    }
                } else {
                    target_z = hit->z + (target_z - hit->z) * t;
                }
            }
        }
        radius = new_r;
        g_wheel_delta = 0.f;
    }

    if (!has_last_) {
        last_x_ = x;
        last_y_ = y;
        has_last_ = true;
        rmb_was_down_ = rmb;
        mmb_was_down_ = mmb;
        return;
    }

    const float dx_px = static_cast<float>(x - last_x_);
    const float dy_px = static_cast<float>(y - last_y_);

    if (editor_nav) {
        // Middle-button drag = orbit (keep zoom).
        if (mmb) {
            const float dtheta = DirectX::XMConvertToRadians(0.25f * dx_px);
            const float dphi = DirectX::XMConvertToRadians(0.25f * dy_px);
            const float signed_dtheta = mirror_x ? -dtheta : dtheta;
            theta += signed_dtheta;
            phi -= dphi;
            phi = std::clamp(phi, 0.1f, DirectX::XM_PI - 0.1f);
            // Blend toward current frame delta for soft release inertia.
            orbit_vel_theta_ = orbit_vel_theta_ * 0.55f + signed_dtheta * 0.45f;
            orbit_vel_phi_ = orbit_vel_phi_ * 0.55f - dphi * 0.45f;
        } else if (!suppress_pointer &&
                   (mmb_was_down_ || std::fabs(orbit_vel_theta_) > 1.e-5f ||
                    std::fabs(orbit_vel_phi_) > 1.e-5f)) {
            theta += orbit_vel_theta_;
            phi += orbit_vel_phi_;
            phi = std::clamp(phi, 0.1f, DirectX::XM_PI - 0.1f);
            orbit_vel_theta_ *= 0.88f;
            orbit_vel_phi_ *= 0.88f;
            if (std::fabs(orbit_vel_theta_) < 1.e-5f) orbit_vel_theta_ = 0.f;
            if (std::fabs(orbit_vel_phi_) < 1.e-5f) orbit_vel_phi_ = 0.f;
        } else if (suppress_pointer) {
            orbit_vel_theta_ = 0.f;
            orbit_vel_phi_ = 0.f;
            pan_lerp_active_ = false;
        }

        // Right button: drag Y = zoom; short click = teleport (keep zoom + angles).
        if (rmb) {
            if (!rmb_was_down_) {
                rmb_drag_px_ = 0.f;
                rmb_down_fb_x_ = fb_x;
                rmb_down_fb_y_ = fb_y;
                rmb_fb_w_ = fb_w;
                rmb_fb_h_ = fb_h;
            } else {
                rmb_drag_px_ += std::fabs(dx_px) + std::fabs(dy_px);
                // Vertical drag zooms (up = in, down = out).
                radius *= std::pow(1.015f, dy_px);
                radius = std::clamp(radius, min_radius, max_radius);
            }
        } else if (rmb_was_down_) {
            if (rmb_drag_px_ < 12.f) {
                if (auto hit = hit_ground_xy(rmb_down_fb_x_, rmb_down_fb_y_, rmb_fb_w_, rmb_fb_h_)) {
                    float tz = hit->z;
                    if (sample_ground_xy && ground_follow) {
                        if (const auto z = sample_ground_xy(hit->x, hit->y)) tz = *z;
                    }
                    pan_from_x_ = target_x;
                    pan_from_y_ = target_y;
                    pan_from_z_ = target_z;
                    pan_to_x_ = hit->x;
                    pan_to_y_ = hit->y;
                    pan_to_z_ = tz;
                    pan_lerp_t_ = 0.f;
                    pan_lerp_active_ = true;
                    teleport_x_ = pan_to_x_;
                    teleport_y_ = pan_to_y_;
                    teleport_z_ = pan_to_z_;
                    ground_teleport_pending_ = true;
                    ground_z = tz;
                }
            }
        }
        if (pan_lerp_active_) {
            // ~0.20 s ease-out (1 - (1-t)^2), sibling of MMB orbit inertia.
            constexpr float kPanDur = 0.20f;
            pan_lerp_t_ += 1.f / 60.f; // update() is per-frame; ~60 Hz nominal
            float u = pan_lerp_t_ / kPanDur;
            if (u >= 1.f) {
                u = 1.f;
                pan_lerp_active_ = false;
            }
            const float e = 1.f - (1.f - u) * (1.f - u);
            target_x = pan_from_x_ + (pan_to_x_ - pan_from_x_) * e;
            target_y = pan_from_y_ + (pan_to_y_ - pan_from_y_) * e;
            target_z = pan_from_z_ + (pan_to_z_ - pan_from_z_) * e;
        }
        (void)lmb; // selection handled by map editor
    } else {
        // Hello viewers / vc_car orbit: LMB or MMB orbit, RMB zoom.
        if (lmb || mmb) {
            const float dtheta = DirectX::XMConvertToRadians(0.25f * dx_px);
            const float dphi = DirectX::XMConvertToRadians(0.25f * dy_px);
            theta += dtheta;
            phi -= dphi;
            phi = std::clamp(phi, 0.1f, DirectX::XM_PI - 0.1f);
        } else if (rmb) {
            radius += 0.01f * dx_px - 0.01f * dy_px;
            radius = std::clamp(radius, min_radius, max_radius);
        }
    }

    rmb_was_down_ = rmb;
    mmb_was_down_ = mmb;
    last_x_ = x;
    last_y_ = y;
}

void LunaCamera::build_world_view_proj(float aspect, const float world_row_major_4x4[16],
                                       float out_transposed_wvp[16]) const {
    const DirectX::XMMATRIX view = luna_view_matrix(target_x, target_y, target_z, radius, theta, phi, z_up);
    DirectX::XMMATRIX proj = luna_proj_matrix(aspect, orthographic, radius, mirror_x);
    DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();
    if (world_row_major_4x4) world = load_world_row_major(world_row_major_4x4);
    store_transposed(world * view * proj, out_transposed_wvp);
}

void LunaCamera::build_world_view(float aspect, const float world_row_major_4x4[16],
                                  float out_transposed_wv[16]) const {
    const DirectX::XMMATRIX view = luna_view_matrix(target_x, target_y, target_z, radius, theta, phi, z_up);
    DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();
    if (world_row_major_4x4) world = load_world_row_major(world_row_major_4x4);
    (void)aspect;
    store_transposed(world * view, out_transposed_wv);
}

void LunaCamera::build_world_view_proj(float aspect, float out_transposed_wvp[16]) const {
    build_world_view_proj(aspect, nullptr, out_transposed_wvp);
}

void LunaCamera::build_view_proj_ma4(float aspect, f4f23ma::ma4& out) const {
    const DirectX::XMMATRIX view = luna_view_matrix(target_x, target_y, target_z, radius, theta, phi, z_up);
    DirectX::XMMATRIX proj = luna_proj_matrix(aspect, orthographic, radius, mirror_x);
    out = xm_to_ma4(proj * view);
}

void LunaCamera::set_top_down_view() {
    phi = 0.1f;
    reset_mouse();
}

void LunaCamera::toggle_orthographic() {
    orthographic = !orthographic;
}

void LunaCamera::build_view_rotation_transposed(float out_transposed_view_rot[16]) const {
    DirectX::XMMATRIX view = luna_view_matrix(target_x, target_y, target_z, radius, theta, phi, z_up);
    view.r[3] = DirectX::XMVectorSet(0.f, 0.f, 0.f, 1.f);
    store_transposed(view, out_transposed_view_rot);
}

void LunaCamera::build_axis_gizmo_wvp(float out_transposed_wvp[16]) const {
    DirectX::XMMATRIX view = luna_view_matrix(target_x, target_y, target_z, radius, theta, phi, z_up);
    view.r[3] = DirectX::XMVectorSet(0.f, 0.f, 0.f, 1.f);
    DirectX::XMMATRIX ortho = DirectX::XMMatrixOrthographicLH(3.4f, 3.4f, 0.01f, 10.f);
    if (mirror_x) ortho = ortho * DirectX::XMMatrixScaling(-1.f, 1.f, 1.f);
    store_transposed(view * ortho, out_transposed_wvp);
}

} // namespace omsi::input
