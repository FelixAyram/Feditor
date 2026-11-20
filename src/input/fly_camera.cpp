#include "fly_camera.hpp"

#include <algorithm>
#include <cmath>

namespace omsi::input {

FlyCamera::FlyCamera(GLFWwindow* window) : window_(window) {}

namespace {

f4f23ma::f4 quat_rotate(const f4f23ma::f4& q, const f4f23ma::f4& v) {
    const f4f23ma::f4 qv{q.x, q.y, q.z, 0.f};
    const f4f23ma::f4 t = qv.cross3(v) * 2.f;
    return v + t * q.w + qv.cross3(t);
}

void aim_from_direction(CameraState& state, const f4f23ma::f4& dir) {
    state.yaw = std::atan2(dir.y, dir.x);
    state.pitch = std::asin(std::clamp(dir.z, -1.f, 1.f));
    state.target = state.eye + dir;
}

} // namespace

void FlyCamera::reset_mouse() { first_mouse_ = true; }

void FlyCamera::set_fixed_view(const f4f23ma::f4& eye, const f4f23ma::f4& target) {
    state_.eye = eye;
    state_.eye.w = 1.f;
    state_.target = target;
    state_.target.w = 1.f;
    const f4f23ma::f4 look = (state_.target - state_.eye).normalized3();
    state_.yaw = std::atan2(look.y, look.x);
    state_.pitch = std::asin(std::clamp(look.z, -1.f, 1.f));
    reset_mouse();
}

void FlyCamera::set_spawn(const f4f23ma::f4& position, const f4f23ma::f4& rotation_quat) {
    f4f23ma::f4 dir = quat_rotate(rotation_quat, f4f23ma::f4{1.f, 0.f, 0.f, 0.f});
    dir.z = 0.f;
    dir.w = 0.f;
    dir = dir.normalized3();
    if (dir.dot3(dir) <= 1e-8f) dir = f4f23ma::f4{1.f, 0.f, 0.f, 0.f};

    state_.eye = position;
    state_.eye.z += 2.5f;
    state_.eye.x -= dir.x * 6.f;
    state_.eye.y -= dir.y * 6.f;

    state_.target = position + dir * 60.f;
    state_.target.z = state_.eye.z;

    const f4f23ma::f4 look = (state_.target - state_.eye).normalized3();
    state_.yaw = std::atan2(look.y, look.x);
    state_.pitch = std::asin(std::clamp(look.z, -1.f, 1.f));
    reset_mouse();
}

void FlyCamera::update(float dt) {
    f4f23ma::f4 forward = (state_.target - state_.eye);
    forward.w = 0.f;
    const auto fwd = forward.normalized3();
    const auto right = fwd.cross3(state_.up).normalized3();

    const float speed = state_.move_speed * dt;
    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) state_.eye = state_.eye + fwd * speed;
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) state_.eye = state_.eye - fwd * speed;
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) state_.eye = state_.eye - right * speed;
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) state_.eye = state_.eye + right * speed;
    if (glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS) state_.eye.z += speed;
    if (glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) state_.eye.z -= speed;

    if (glfwGetInputMode(window_, GLFW_CURSOR) != GLFW_CURSOR_DISABLED) return;

    double x, y;
    glfwGetCursorPos(window_, &x, &y);
    if (first_mouse_) {
        last_x_ = x;
        last_y_ = y;
        first_mouse_ = false;
        return;
    }

    const float dx = static_cast<float>(x - last_x_);
    const float dy = static_cast<float>(y - last_y_);
    last_x_ = x;
    last_y_ = y;

    state_.yaw += dx * state_.look_sensitivity;
    state_.pitch -= dy * state_.look_sensitivity;
    state_.pitch = std::clamp(state_.pitch, -1.45f, 1.45f);

    const float cp = std::cos(state_.pitch);
    const float sp = std::sin(state_.pitch);
    const float cy = std::cos(state_.yaw);
    const float sy = std::sin(state_.yaw);

    f4f23ma::f4 dir{cy * cp, sy * cp, sp, 0.f};
    dir = dir.normalized3();
    state_.target = state_.eye + dir * 100.f;
}

f4f23ma::ma4 FlyCamera::view_matrix() const {
    return f4f23ma::look_at(state_.eye, state_.target, state_.up);
}

f4f23ma::ma4 FlyCamera::proj_matrix(float aspect) const {
    return f4f23ma::perspective_dx(1.04719755f, aspect, 0.5f, 15000.f);
}

} // namespace omsi::input
