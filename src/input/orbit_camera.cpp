#include "orbit_camera.hpp"

#include <algorithm>
#include <cmath>

namespace omsi::input {

void OrbitCamera::scroll_callback(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    if (auto* cam = static_cast<OrbitCamera*>(glfwGetWindowUserPointer(window)))
        cam->on_scroll(static_cast<float>(yoffset));
}

void OrbitCamera::on_scroll(float delta) { scroll_delta_ += delta; }

void OrbitCamera::set_target(const f4f23ma::f4& t) {
    target = t;
    target.w = 1.f;
}

void OrbitCamera::frame_bounds(const f4f23ma::f4& min, const f4f23ma::f4& max) {
    target = f4f23ma::f4{
        (min.x + max.x) * 0.5f,
        (min.y + max.y) * 0.5f,
        (min.z + max.z) * 0.5f,
        1.f,
    };
    const f4f23ma::f32 dx = max.x - min.x;
    const f4f23ma::f32 dy = max.y - min.y;
    const f4f23ma::f32 dz = max.z - min.z;
    const f4f23ma::f32 radius = (std::max)((std::max)(dx, dy), (std::max)(dz, 1.f)) * 0.55f;
    distance = (std::max)(radius * 2.8f, 2.f);
}

f4f23ma::f4 OrbitCamera::eye_position() const {
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    // OMSI Z-up: yaw around Z, pitch elevates from the XY plane.
    return f4f23ma::f4{
        target.x + distance * cp * cy,
        target.y + distance * cp * sy,
        target.z + distance * sp,
        1.f,
    };
}

void OrbitCamera::update(GLFWwindow* window) {
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);

    const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (lmb) {
        if (!dragging_) {
            dragging_ = true;
            last_x_ = x;
            last_y_ = y;
        } else {
            const float dx = static_cast<float>(x - last_x_);
            const float dy = static_cast<float>(y - last_y_);
            yaw += dx * orbit_sensitivity;
            pitch -= dy * orbit_sensitivity;
            pitch = std::clamp(pitch, -1.45f, 1.45f);
            last_x_ = x;
            last_y_ = y;
        }
    } else {
        dragging_ = false;
    }

    if (scroll_delta_ != 0.f) {
        const float factor = 1.f - scroll_delta_ * scroll_sensitivity;
        distance *= (std::max)(factor, 0.05f);
        distance = std::clamp(distance, min_distance, max_distance);
        scroll_delta_ = 0.f;
    }
}

f4f23ma::ma4 OrbitCamera::view_matrix() const {
    return f4f23ma::look_at(eye_position(), target, up);
}

f4f23ma::ma4 OrbitCamera::proj_matrix(float aspect) const {
    return f4f23ma::perspective_dx(1.04719755f, aspect, 0.1f, 15000.f);
}

} // namespace omsi::input
