#pragma once

#include "omsi/bus_types.hpp"
#include "omsi/vehicle/collision.hpp"

#include <functional>
#include <vector>

namespace omsi::vehicle {

// Road vehicle body for the editor's drivable bus, built from the .bus file alone (OMSI's own
// approach: OMSI integrates its own chassis, the scripts only hand it wheel torque and brakes).
//
// * Body: heave, pitch and roll on one spring/damper strut per wheel ([newachse] achse_feder /
//   achse_daempfer / achse_maxforce; force = k * penetration - c * speed, capped at maxforce),
//   spring unloaded when the tyre stands on the model's ground plane, so the body sags by its own
//   load. Pitch and roll get OMSI's per-frame sin(x)/x damping, made frame-rate independent.
// * Plane: longitudinal speed from drive / brake / rolling forces, lateral speed and yaw from tyre
//   side forces (slip angle x cornering stiffness, saturating at mu x the wheel's current load),
//   every axle steered towards the turning centre on [rot_pnt_long]. Below walking pace the motion
//   blends to the kinematic (no-slip) turn so a parked bus never creeps sideways.
// Frame: x right, y forward, z up; heading in degrees clockwise (forward = (sin h, cos h)).
class BusBodyPhysics {
public:
    // Ground height under (x, y) near z_hint (world, editor metres).
    using GroundFn = std::function<float(float x, float y, float z_hint)>;

    struct Inputs {
        float drive_force_n = 0.f;   // tractive force at the contact patches (+ = forward)
        float brake_force_n = 0.f;   // brake capacity (always opposes motion)
        float rolling_force_n = 0.f; // rolling resistance magnitude
        float inv_turn_radius = 0.f; // OMSI steering curvature (1/m, OMSI sign: + = right)
        float spring_factor[8]{1, 1, 1, 1, 1, 1, 1, 1}; // Axle_Springfactor per wheel (axle*2+side)
    };

    struct Wheel {
        float px = 0.f, py = 0.f; // strut position in the body frame
        int axle = 0, side = 0;   // side 0 = L, 1 = R
        float k = 150000.f, c = 20000.f, fmax = 250000.f;
        bool driven = false;
        float ground_z = 0.f;
        float compression = 0.f; // metres the spring is pressed (>= 0)
        float compression_rate = 0.f;
        float load = 0.f; // N
    };

    void init(const omsi::bus::BusDocument& bus);
    // Put the body on the ground at rest (springs settled to their static sag).
    void place(float x, float y, float heading_deg, const GroundFn& ground);
    void step(float dt, const Inputs& in, const GroundFn& ground);

    // Collisions (after openOMSI RigidBody::collide, AGPL-3.0): the body's [boundingbox] against the
    // obstacles - pushed out the shortest way, an impulse at the contact (restitution 0.2 on a real
    // blow, scrape friction 0.4). Obstacles the bus was spawned inside are ignored until it leaves them.
    struct Impact {
        float speed = 0.f;  // closing speed (m/s)
        float energy = 0.f; // J taken out of the body
        std::int64_t obstacle_id = -1;
    };
    std::vector<Impact> collide(const std::vector<Obb>& obstacles, float dt);
    [[nodiscard]] Obb body_box() const;

    [[nodiscard]] float x() const { return x_; }
    [[nodiscard]] float y() const { return y_; }
    [[nodiscard]] float z() const { return z_; }
    [[nodiscard]] float heading_deg() const { return heading_deg_; }
    // Model rotation as the editor's omsi_object_rotation_matrix expects: it applies RotationX(-pitch),
    // so a positive pitch lowers the nose; RotationY(-bank) lowers the right side.
    [[nodiscard]] float pitch_deg() const { return -pitch_ * kRad2Deg; }
    [[nodiscard]] float roll_deg() const { return -roll_ * kRad2Deg; }
    [[nodiscard]] float speed_ms() const { return u_; }          // forward speed
    [[nodiscard]] float lateral_ms() const { return v_; }        // sideways speed (+ = right)
    [[nodiscard]] float yaw_rate() const { return r_; }          // rad/s, + = left
    [[nodiscard]] float accel_long() const { return a_long_; }   // m/s^2
    [[nodiscard]] float accel_lat() const { return a_lat_; }     // m/s^2 (+ = right)
    [[nodiscard]] const std::vector<Wheel>& wheels() const { return wheels_; }
    void set_speed(float u_ms) { u_ = u_ms; }

private:
    static constexpr float kRad2Deg = 57.29577951f;
    static constexpr float kG = 9.81f;

    void sample_ground(const GroundFn& ground);
    void substep(float h, const Inputs& in);
    [[nodiscard]] float strut_z(const Wheel& w) const { return z_ + w.py * pitch_ + w.px * roll_; }

    std::vector<Wheel> wheels_;
    float mass_ = 10000.f;
    float i_pitch_ = 300000.f, i_roll_ = 300000.f, i_yaw_ = 80000.f;
    float cog_h_ = 1.2f;
    float rot_pnt_long_ = 0.f;
    float body_freq_ = 7.f; // sqrt(sum springs kN/m / mass t), OMSI's damping argument
    float mu_ = 0.85f;
    float corner_k_ = 7.f; // cornering stiffness per rad, in multiples of the axle's static load

    // state
    float x_ = 0.f, y_ = 0.f, z_ = 0.f, heading_deg_ = 0.f;
    float vz_ = 0.f;
    float pitch_ = 0.f, roll_ = 0.f; // rad: nose up +, right side up +
    float w_pitch_ = 0.f, w_roll_ = 0.f;
    float u_ = 0.f, v_ = 0.f, r_ = 0.f;
    float bb_[6]{2.5f, 12.f, 3.f, 0.f, 0.f, 1.5f}; // [boundingbox] w l h cx cy cz (cz over the body origin)
    bool spawn_checked_ = false;
    std::vector<std::int64_t> spawned_inside_;
    float a_long_ = 0.f, a_lat_ = 0.f;
};

} // namespace omsi::vehicle
