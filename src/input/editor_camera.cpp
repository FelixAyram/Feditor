#include "editor_camera.hpp"

#include "omsi/logger.hpp"
#include "omsi/map_tile_layout.hpp"
#include "omsi/o3d_format.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace omsi::input {
namespace {

f4f23ma::f4 quat_rotate(const f4f23ma::f4& q, const f4f23ma::f4& v) {
    const f4f23ma::f4 qv{q.x, q.y, q.z, 0.f};
    const f4f23ma::f4 t = qv.cross3(v) * 2.f;
    return v + t * q.w + qv.cross3(t);
}

DirectX::XMMATRIX ma4_to_xm(const f4f23ma::ma4& m) {
    DirectX::XMFLOAT4X4 f{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) f.m[r][c] = m.m[c][r];
    return DirectX::XMLoadFloat4x4(&f);
}

// Object/world space matches omsi_map_object_transform: Y = up.
std::optional<f4f23ma::f4> intersect_y_plane(const EditorRay& ray, float ground_y) {
    if (std::fabs(ray.direction.y) < 1e-6f) return std::nullopt;
    const float t = (ground_y - ray.origin.y) / ray.direction.y;
    if (t < 0.f) return std::nullopt;
    return ray.origin + ray.direction * t;
}

} // namespace

EditorCamera::EditorCamera(GLFWwindow* window) : window_(window) { rebuild_target(); }

void EditorCamera::rebuild_target() {
    // Yaw around +Y, pitch up/down: look direction in XZ with Y height.
    const float cp = std::cos(pitch_);
    const float sp = std::sin(pitch_);
    const float cy = std::cos(yaw_);
    const float sy = std::sin(yaw_);
    f4f23ma::f4 dir{cy * cp, sp, sy * cp, 0.f};
    dir = dir.normalized3();
    target_ = eye_ + dir * 100.f;
}

void EditorCamera::set_look(float yaw, float pitch) {
    yaw_ = yaw;
    pitch_ = std::clamp(pitch, -1.45f, 1.45f);
    rebuild_target();
}

void EditorCamera::teleport_to_entry(const map::EntryPoint& ep) {
    // Entry world is OMSI (x,y_horiz,z_height) → render (x, height, y_horiz).
    const f4f23ma::f4 pos = o3d::omsi_pos_to_render_y_up(ep.position.x, ep.position.y, ep.position.z);
    f4f23ma::f4 dir = quat_rotate(ep.rotation_quat, f4f23ma::f4{1.f, 0.f, 0.f, 0.f});
    dir = o3d::omsi_dir_to_render_y_up(dir.x, dir.y, dir.z);
    dir.y = 0.f;
    dir.w = 0.f;
    dir = dir.normalized3();
    if (dir.dot3(dir) <= 1e-8f) dir = f4f23ma::f4{1.f, 0.f, 0.f, 0.f};

    eye_ = pos;
    eye_.y += 8.f;
    eye_.x -= dir.x * 10.f;
    eye_.z -= dir.z * 10.f;
    set_look(std::atan2(dir.z, dir.x), -0.25f);
    log::info("editor cam teleport entry id=" + std::to_string(ep.id) + " name=\"" + ep.name + "\" eye=(" +
              std::to_string(eye_.x) + "," + std::to_string(eye_.y) + "," + std::to_string(eye_.z) + ")");
}

void EditorCamera::apply_mapcam(const map::GlobalMap& global) {
    if (!global.mapcam.valid) return;
    const auto layout = map::build_map_tile_layout(global);
    const auto origin = map::tile_origin_xy(layout, global.mapcam.grid_x, global.mapcam.grid_y);
    const float wx = origin.first + global.mapcam.position.x;
    const float wy = origin.second + global.mapcam.position.y;
    const float wz = global.mapcam.position.z;
    const f4f23ma::f4 focus = o3d::omsi_pos_to_render_y_up(wx, wy, wz);
    const float dist = global.mapcam.distance > 1.f ? global.mapcam.distance : 50.f;
    const float yaw = global.mapcam.yaw_deg * (3.14159265f / 180.f);
    const float pitch = global.mapcam.pitch_deg * (3.14159265f / 180.f);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    f4f23ma::f4 back{-std::cos(yaw) * cp, -sp, -std::sin(yaw) * cp, 0.f};
    eye_ = focus + back * dist;
    eye_.y = (std::max)(eye_.y, focus.y + 5.f);
    set_look(yaw, pitch);
    log::info("editor cam mapcam focus=(" + std::to_string(focus.x) + "," + std::to_string(focus.y) + "," +
              std::to_string(focus.z) + ") eye=(" + std::to_string(eye_.x) + "," + std::to_string(eye_.y) + "," +
              std::to_string(eye_.z) + ")");
}

void EditorCamera::focus_world_xy(float world_x, float world_y, float world_z, float height) {
    look_at_point({world_x, world_y, world_z, 1.f}, 28.f, height);
}

void EditorCamera::look_at_point(const f4f23ma::f4& target, float back, float up) {
    eye_ = {target.x - back * 0.7f, target.y + up, target.z - back * 0.7f, 1.f};
    f4f23ma::f4 dir = (target - eye_);
    dir.w = 0.f;
    dir = dir.normalized3();
    set_look(std::atan2(dir.z, dir.x), std::asin(std::clamp(dir.y, -1.f, 1.f)));
}

EditorRay EditorCamera::screen_ray(float mouse_x, float mouse_y, float width, float height) const {
    EditorRay ray{};
    ray.origin = eye_;
    if (width <= 1.f || height <= 1.f) {
        ray.direction = (target_ - eye_).normalized3();
        return ray;
    }
    const float ndc_x = (2.f * mouse_x / width) - 1.f;
    const float ndc_y = 1.f - (2.f * mouse_y / height);
    const float aspect = width / height;
    const auto view = view_matrix();
    const auto proj = proj_matrix(aspect);
    const DirectX::XMMATRIX vp = ma4_to_xm(proj) * ma4_to_xm(view);
    DirectX::XMVECTOR det{};
    const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(&det, vp);
    DirectX::XMVECTOR near_p = DirectX::XMVectorSet(ndc_x, ndc_y, 0.f, 1.f);
    DirectX::XMVECTOR far_p = DirectX::XMVectorSet(ndc_x, ndc_y, 1.f, 1.f);
    near_p = DirectX::XMVector4Transform(near_p, inv);
    far_p = DirectX::XMVector4Transform(far_p, inv);
    DirectX::XMFLOAT4 n{}, f{};
    DirectX::XMStoreFloat4(&n, near_p);
    DirectX::XMStoreFloat4(&f, far_p);
    if (std::fabs(n.w) > 1e-8f) {
        n.x /= n.w;
        n.y /= n.w;
        n.z /= n.w;
    }
    if (std::fabs(f.w) > 1e-8f) {
        f.x /= f.w;
        f.y /= f.w;
        f.z /= f.w;
    }
    ray.origin = {n.x, n.y, n.z, 1.f};
    f4f23ma::f4 dir{f.x - n.x, f.y - n.y, f.z - n.z, 0.f};
    ray.direction = dir.normalized3();
    return ray;
}

std::optional<f4f23ma::f4> EditorCamera::hit_ground(float mouse_x, float mouse_y, float width, float height,
                                                    float ground_z) const {
    return intersect_y_plane(screen_ray(mouse_x, mouse_y, width, height), ground_z);
}

void EditorCamera::update(float dt, float ground_z, bool pointer_over_ui) {
    consumed_lmb_click_ = false;
    last_pan_delta_ = 0.f;

    const float speed = move_speed_ * dt * (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 4.f : 1.f);
    f4f23ma::f4 forward = (target_ - eye_);
    forward.y = 0.f;
    forward.w = 0.f;
    forward = forward.normalized3();
    if (forward.dot3(forward) < 1e-8f) forward = {1.f, 0.f, 0.f, 0.f};
    const f4f23ma::f4 right = forward.cross3(up_).normalized3();

    if (!pointer_over_ui) {
        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) eye_ = eye_ + forward * speed;
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) eye_ = eye_ - forward * speed;
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) eye_ = eye_ - right * speed;
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) eye_ = eye_ + right * speed;
        if (glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS) eye_.y += speed;
        if (glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) eye_.y -= speed;
    }

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window_, &mx, &my);
    const bool lmb = !pointer_over_ui && glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool mmb = !pointer_over_ui && glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    const bool rmb = !pointer_over_ui && glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    const float width = static_cast<float>((std::max)(fbw, 1));
    const float height = static_cast<float>((std::max)(fbh, 1));

    if ((mmb || (lmb && glfwGetKey(window_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS)) && !pan_dragging_) {
        if (!look_dragging_) {
            look_dragging_ = true;
            last_x_ = mx;
            last_y_ = my;
        }
    } else if (look_dragging_ && !mmb) {
        look_dragging_ = false;
    }

    if (look_dragging_) {
        const float dx = static_cast<float>(mx - last_x_);
        const float dy = static_cast<float>(my - last_y_);
        last_x_ = mx;
        last_y_ = my;
        set_look(yaw_ + dx * look_sensitivity_, pitch_ - dy * look_sensitivity_);
    }

    if (rmb && !rmb_was_down_) {
        if (auto hit = hit_ground(static_cast<float>(mx), static_cast<float>(my), width, height, ground_z)) {
            pan_dragging_ = true;
            pan_anchor_ = *hit;
            last_x_ = mx;
            last_y_ = my;
        }
    } else if (!rmb) {
        if (pan_dragging_) {
            log::info("editor pan end eye=(" + std::to_string(eye_.x) + "," + std::to_string(eye_.y) + "," +
                      std::to_string(eye_.z) + ") last_delta=" + std::to_string(last_pan_delta_));
        }
        pan_dragging_ = false;
    }

    if (pan_dragging_) {
        if (auto hit = hit_ground(static_cast<float>(mx), static_cast<float>(my), width, height, ground_z)) {
            f4f23ma::f4 delta = pan_anchor_ - *hit;
            delta.y = 0.f;
            last_pan_delta_ = std::sqrt(delta.dot3(delta));
            eye_.x += delta.x;
            eye_.z += delta.z;
            if (last_pan_delta_ > 0.01f)
                log::info("editor pan_delta=" + std::to_string(last_pan_delta_) + " eye=(" +
                          std::to_string(eye_.x) + "," + std::to_string(eye_.y) + "," + std::to_string(eye_.z) + ")");
        }
    }

    if (!lmb && lmb_was_down_ && !look_dragging_ && !pan_dragging_ && !pointer_over_ui)
        consumed_lmb_click_ = true;

    lmb_was_down_ = lmb;
    rmb_was_down_ = rmb;
    rebuild_target();
}

f4f23ma::ma4 EditorCamera::view_matrix() const { return f4f23ma::look_at(eye_, target_, up_); }

f4f23ma::ma4 EditorCamera::proj_matrix(float aspect) const {
    // Outdoor tiles: a tiny near + huge far maps everything to z≈1 and fails the depth test.
    return f4f23ma::perspective_dx(1.04719755f, aspect, 2.f, 4000.f);
}

} // namespace omsi::input
