#pragma once

#include "omsi/bus_types.hpp"
#include "omsi/cti.hpp"
#include "omsi/script/constfile.hpp"
#include "omsi/script/osc_vm.hpp"
#include "omsi/script/var_registry.hpp"
#include "omsi/sound/sound_runtime.hpp"
#include "omsi/vehicle_cabin.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace omsi::script {

// OMSI TRoadVehicleInst: varlists, consts, OSC VM, sound pack, frame tick.
class VehicleRuntime {
public:
    VehicleRuntime() = default;
    ~VehicleRuntime();

    bool load(const bus::BusDocument& bus, const std::filesystem::path& omsi_root);
    // Apply .cti [setvar] into public floats (Unit87 Colorscheme).
    void apply_cti(const omsi::cti::CtiDocument& cti);
    void reset();
    void tick(float dt_sec);

    // Background sim: OSC + sound run off the render thread (viewer).
    void start_async_sim();
    void stop_async_sim();
    void post_tick(float dt_sec);
    void sync_sim();

    void apply_trigger(const std::string& trigger_name);
    void apply_triggers(const std::vector<std::string>& pressed, const std::vector<std::string>& released);
    // OMSI TComplMapObjInst.TriggerXML: 1=press, 2=drag, else=release (_off).
    void trigger_xml(const std::string& name, int value);
    void set_driver_input(float throttle, float brake);
    // Unit121 toggel_mouse_ctrl: cursor drives pedals + inv_lenkradius (bypasses KeyCommand ramp).
    void set_mouse_drive_mode(bool active);
    void set_mouse_drive_input(float throttle, float brake, float inv_lenkradius);
    // Unit129 KeyCommand continuous: KP4/5/6 → steering_left / steering_neutral / steering_right.
    void set_steering_keys(bool left, bool right, bool neutral);
    void set_ai_mode(bool ai);
    void set_mouse(float x, float y);

    [[nodiscard]] float sim_time() const { return sim_time_; }
    // Body attitude around Schwerpunkt (OMSI absposition / Federung). Flat-ground viewer.
    [[nodiscard]] float body_heave_m() const { return body_heave_m_; }
    [[nodiscard]] float body_pitch_rad() const { return body_pitch_rad_; }
    [[nodiscard]] float body_roll_rad() const { return body_roll_rad_; }
    [[nodiscard]] float inv_lenkradius() const { return inv_lenkradius_; }
    [[nodiscard]] float wheel_radius_m() const { return wheel_radius_m_; }
    // Ground clearance: O3D already places tire centers near radius; bag Federweg is Axle_Suspension.
    [[nodiscard]] float ride_height_m() const { return 0.f; }
    [[nodiscard]] const VarRegistry& vars() const { return vars_; }
    [[nodiscard]] VarRegistry& vars() { return vars_; }
    [[nodiscard]] const std::vector<float>& public_floats() const { return public_floats_; }
    [[nodiscard]] float public_float(const std::string& name, float default_value = 0.f) const;
    void set_public_float(const std::string& name, float value);

    [[nodiscard]] const OscVM& osc() const { return osc_; }
    [[nodiscard]] omsi::sound::SoundRuntime& sound() { return sound_; }
    [[nodiscard]] const omsi::sound::SoundRuntime& sound() const { return sound_; }
    [[nodiscard]] const std::unordered_map<std::string, float>& merged_consts() const { return merged_consts_; }
    [[nodiscard]] const std::optional<cabin::VehiclePathsDocument>& paths() const { return paths_; }
    [[nodiscard]] const std::optional<cabin::PassengerCabinDocument>& cabin() const { return cabin_; }

    // Test/debug: force chassis speed (km/h) matching Velocity script vars.
    void debug_set_velocity_kmh(float kmh);

    // External chassis (editor rigid body): the scripts still produce wheel torque and brakes, read
    // back here as forces; speed, acceleration and spring travel come from the body physics.
    struct ChassisForces {
        float drive_n = 0.f;   // tractive force (M_Wheel / wheel radius)
        float brake_n = 0.f;   // brake capacity (script brakes, pedal floor, parking brake)
        float rolling_n = 0.f; // [rollwiderstand]
    };
    void set_external_chassis(bool on) { external_chassis_ = on; }
    [[nodiscard]] const ChassisForces& chassis_forces() const { return chassis_forces_; }
    // compression: metres per wheel, index axle * 2 + side (L, R).
    void set_external_motion(float v_kmh, float accel_ms2, const float* compression, int n_wheels);

private:
    VarRegistry vars_;
    std::vector<float> public_floats_;
    std::vector<std::string> public_strings_;
    std::unordered_map<std::string, float> merged_consts_;
    std::unordered_map<std::string, ConstCurve> merged_curves_;
    std::vector<ConstFileDocument> constfiles_;
    OscVM osc_;
    omsi::sound::SoundRuntime sound_;
    std::optional<cabin::VehiclePathsDocument> paths_;
    std::optional<cabin::PassengerCabinDocument> cabin_;

    float sim_time_ = 0.f;
    float velocity_ = 0.f; // km/h (Velocity / Velocity_Ground)
    float throttle_ = 0.f;
    float brake_ = 0.f;
    float weather_temperature_c_ = 20.f;
    float mouse_x_ = 0.f;
    float mouse_y_ = 0.f;
    bool ai_mode_ = false;

    // Chassis params from .bus (OMSI TRoadVehicle). Used after OSC writes M_Wheel / Brakeforce.
    float mass_t_ = 10.f;
    float rollwiderstand_N_ = 900.f;
    float schwerpunkt_m_ = 1.2f;
    float wheel_radius_m_ = 0.47f;
    float inv_min_turnradius_ = 0.13f; // 1/m from [inv_min_turnradius]
    float inertia_pitch_ = 200.f;      // t·m² around Y (Längsachse) — [mass] line 3
    float inertia_roll_ = 500.f;       // t·m² around X
    float rot_pnt_long_m_ = 0.f;
    std::vector<bus::BusAxle> axles_;

    // Unit129: inv_lenkradius (+ = left). Axle_Steering published in rad.
    float inv_lenkradius_ = 0.f;
    bool steer_left_ = false;
    bool steer_right_ = false;
    bool steer_neutral_ = false;
    bool mouse_drive_mode_ = false;
    float mouse_inv_lenkradius_ = 0.f;

    // Per-corner spring travel (m, compressed +). Indices: axle*2 + (0=L,1=R).
    float susp_x_[8]{};
    float susp_v_[8]{};
    float body_heave_m_ = 0.f;  // world Z of body (negative = squat)
    float body_pitch_rad_ = 0.f; // + nose up
    float body_roll_rad_ = 0.f;  // + right down
    float body_heave_v_ = 0.f;
    float body_pitch_v_ = 0.f;
    float body_roll_v_ = 0.f;
    float accel_ms2_ = 0.f; // longitudinal a from last integrate
    bool external_chassis_ = false;
    ChassisForces chassis_forces_{};

    void init_engine_defaults();
    void apply_bus_physics(const bus::BusDocument& bus);
    void integrate_chassis(float dt_sec);
    void update_steering(float dt_sec);
    void update_suspension(float dt_sec);
    void push_system_state(float dt_sec);
    void tick_impl(float dt_sec);
    void sim_thread_main();

    std::thread sim_thread_;
    mutable std::mutex sim_mutex_;
    std::condition_variable sim_cv_;
    std::condition_variable sim_idle_cv_;
    std::deque<float> pending_dt_;
    std::atomic<bool> sim_stop_{false};
    std::atomic<bool> sim_busy_{false};
    bool async_sim_ = false;
};

} // namespace omsi::script
