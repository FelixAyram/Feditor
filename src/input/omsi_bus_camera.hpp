#pragma once

#include "omsi/bus_types.hpp"

struct GLFWwindow;

namespace omsi::input {

// Fixed OMSI bus interior camera (add_camera_driver / add_camera_pax).
struct OmsiBusCamera {
    bus::BusCamera def{};
    int index = -1;

    void set_definition(const bus::BusCamera& cam, int cam_index);
    void reset_look();
    void update_mouse(GLFWwindow* window);
    void build_world_view_proj(float aspect, float out_transposed_wvp[16]) const;
    // Apply chassis attitude (OMSI absposition Federung) so interior cams ride with the body.
    void set_body_pose(float heave_m, float pitch_rad, float roll_rad, float ride_z_m);
    void cycle(int delta, int count);

private:
    float yaw_offset_deg_ = 0.f;
    float pitch_offset_deg_ = 0.f;
    float zoom_factor_ = 1.f;
    float body_heave_m_ = 0.f;
    float body_pitch_rad_ = 0.f;
    float body_roll_rad_ = 0.f;
    float ride_z_m_ = 0.f;
    double last_x_ = 0.0;
    double last_y_ = 0.0;
    bool has_last_ = false;
};

enum class BusViewMode { exterior, driver, passenger };

struct BusCameraController {
    BusViewMode mode = BusViewMode::driver;
    int driver_index = 0;
    int passenger_index = 0;
    int saved_driver_index = 0;

    void apply_bus_defaults(int std_driver_index);
    void on_key_f1();
    void on_key_f2();
    void on_key_f3();
    void cycle_driver(int delta, int count);
    void cycle_passenger(int delta, int count);
};

} // namespace omsi::input
