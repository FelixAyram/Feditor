#pragma once

#include <GLFW/glfw3.h>

#include <optional>

#include "f4f23ma/math.hpp"
#include "omsi/map_types.hpp"

namespace omsi::input {

struct EditorRay {
    f4f23ma::f4 origin{0.f, 0.f, 0.f, 1.f};
    f4f23ma::f4 direction{0.f, 0.f, 1.f, 0.f};
};

class EditorCamera {
public:
    explicit EditorCamera(GLFWwindow* window);

    void update(float dt, float ground_z, bool pointer_over_ui);
    [[nodiscard]] f4f23ma::ma4 view_matrix() const;
    [[nodiscard]] f4f23ma::ma4 proj_matrix(float aspect) const;
    [[nodiscard]] EditorRay screen_ray(float mouse_x, float mouse_y, float width, float height) const;
    [[nodiscard]] std::optional<f4f23ma::f4> hit_ground(float mouse_x, float mouse_y, float width, float height,
                                                        float ground_z) const;

    void teleport_to_entry(const map::EntryPoint& ep);
    void apply_mapcam(const map::GlobalMap& global);
    void focus_world_xy(float world_x, float world_y, float world_z, float height = 12.f);
    void look_at_point(const f4f23ma::f4& target, float back = 30.f, float up = 18.f);

    [[nodiscard]] const f4f23ma::f4& eye() const { return eye_; }
    [[nodiscard]] float yaw() const { return yaw_; }
    [[nodiscard]] float pitch() const { return pitch_; }
    [[nodiscard]] bool consumed_lmb_click() const { return consumed_lmb_click_; }
    [[nodiscard]] float last_pan_delta() const { return last_pan_delta_; }

private:
    void rebuild_target();
    void set_look(float yaw, float pitch);

    GLFWwindow* window_ = nullptr;
    f4f23ma::f4 eye_{0.f, 12.f, 0.f, 1.f};
    f4f23ma::f4 target_{1.f, 12.f, 0.f, 1.f};
    f4f23ma::f4 up_{0.f, 1.f, 0.f, 0.f}; // match omsi_map_object_transform (Y up)
    float yaw_ = 0.f;
    float pitch_ = -0.35f;
    float move_speed_ = 40.f;
    float look_sensitivity_ = 0.0035f;

    bool look_dragging_ = false;
    bool pan_dragging_ = false;
    bool lmb_was_down_ = false;
    bool rmb_was_down_ = false;
    bool consumed_lmb_click_ = false;
    double last_x_ = 0.0;
    double last_y_ = 0.0;
    f4f23ma::f4 pan_anchor_{};
    float last_pan_delta_ = 0.f;
};

} // namespace omsi::input
