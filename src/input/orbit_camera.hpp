#pragma once

#include <GLFW/glfw3.h>

#include "f4f23ma/math.hpp"

namespace omsi::input {

struct OrbitCamera {
    f4f23ma::f4 target{0.f, 0.f, 0.f, 1.f};
    f4f23ma::f4 up{0.f, 0.f, 1.f, 0.f};
    float yaw = 0.785398f;
    float pitch = 0.35f;
    float distance = 4.f;
    float min_distance = 0.5f;
    float max_distance = 500.f;
    float orbit_sensitivity = 0.005f;
    float scroll_sensitivity = 0.12f;

    void set_target(const f4f23ma::f4& t);
    void frame_bounds(const f4f23ma::f4& min, const f4f23ma::f4& max);
    void update(GLFWwindow* window);
    void on_scroll(float delta);
    [[nodiscard]] f4f23ma::f4 eye_position() const;
    [[nodiscard]] f4f23ma::ma4 view_matrix() const;
    [[nodiscard]] f4f23ma::ma4 proj_matrix(float aspect) const;

    static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);

private:
    double last_x_ = 0.0;
    double last_y_ = 0.0;
    bool dragging_ = false;
    float scroll_delta_ = 0.f;
};

} // namespace omsi::input
