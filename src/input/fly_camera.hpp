#pragma once

#include <GLFW/glfw3.h>

#include "f4f23ma/math.hpp"

namespace omsi::input {

struct CameraState {
    f4f23ma::f4 eye{0.f, 0.f, 10.f, 1.f};
    f4f23ma::f4 target{0.f, 0.f, 0.f, 1.f};
    f4f23ma::f4 up{0.f, 0.f, 1.f, 0.f};
    float yaw = 0.f;
    float pitch = 0.f;
    float move_speed = 8.f;
    float look_sensitivity = 0.0025f;
};

class FlyCamera {
public:
  explicit FlyCamera(GLFWwindow* window);

    void update(float dt);
    [[nodiscard]] f4f23ma::ma4 view_matrix() const;
    [[nodiscard]] f4f23ma::ma4 proj_matrix(float aspect) const;
    void set_spawn(const f4f23ma::f4& position, const f4f23ma::f4& rotation_quat = {0.f, 0.f, 0.f, 1.f});
    void set_fixed_view(const f4f23ma::f4& eye, const f4f23ma::f4& target);
    void reset_mouse();
    [[nodiscard]] const CameraState& state() const { return state_; }

private:
    GLFWwindow* window_ = nullptr;
    CameraState state_{};
    double last_x_ = 0.0;
    double last_y_ = 0.0;
    bool first_mouse_ = true;
};

} // namespace omsi::input
