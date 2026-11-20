#include "omsi/vehicle/bus_body_physics.hpp"

#include <algorithm>
#include <cmath>

namespace omsi::vehicle {
namespace {

constexpr float kOmsiFrame = 1.f / 30.f; // OMSI's damping is defined per (30 fps) frame
constexpr float kSubstep = 1.f / 240.f;

float sgn(float v) { return v > 0.f ? 1.f : (v < 0.f ? -1.f : 0.f); }

} // namespace

void BusBodyPhysics::init(const omsi::bus::BusDocument& bus) {
    mass_ = (std::max)(bus.mass_t < 100.f ? bus.mass_t * 1000.f : bus.mass_t, 1000.f);
    // [momentofintertia] (t m^2): OMSI hands the three values to its body as pitch, yaw, roll.
    const auto moi = [](float v, float fallback) { return v > 0.5f ? (v < 5000.f ? v * 1000.f : v) : fallback; };
    i_pitch_ = (std::max)(moi(bus.moment_x, 300000.f), 1000.f);
    i_yaw_ = (std::max)(moi(bus.moment_y, 80000.f), 1000.f);
    i_roll_ = (std::max)(moi(bus.moment_z, 300000.f), 1000.f);
    cog_h_ = bus.schwerpunkt_m > 0.f ? bus.schwerpunkt_m : 1.2f;
    bb_[0] = (std::max)(bus.bbox_half_x * 2.f, 1.f);
    bb_[1] = (std::max)(bus.bbox_half_y * 2.f, 2.f);
    bb_[2] = (std::max)(bus.bbox_half_z * 2.f, 1.f);
    bb_[3] = 0.f;
    bb_[4] = bus.bbox_offset_y;
    bb_[5] = bb_[2] * 0.5f + 0.25f; // floor clears the road: a kerb is the tyres' business
    spawn_checked_ = false;
    spawned_inside_.clear();
    rot_pnt_long_ = bus.rot_pnt_long_m;

    wheels_.clear();
    std::vector<omsi::bus::BusAxle> axles = bus.axles;
    if (axles.empty()) {
        omsi::bus::BusAxle f{}, r{};
        f.long_m = bus.bbox_half_y * 0.6f;
        r.long_m = -bus.bbox_half_y * 0.5f;
        r.antrieb = true;
        axles = {f, r};
    }
    bool any_driven = false;
    float front = -1e9f;
    for (const auto& a : axles) {
        any_driven = any_driven || a.antrieb;
        front = (std::max)(front, a.long_m);
    }
    float sum_k_kn = 0.f;
    for (std::size_t ai = 0; ai < axles.size() && ai < 4; ++ai) {
        const auto& a = axles[ai];
        const float half = 0.25f * ((std::max)(a.maxwidth, 0.6f) + (std::max)(a.minwidth, 0.4f));
        for (int side = 0; side < 2; ++side) {
            Wheel w;
            w.axle = static_cast<int>(ai);
            w.side = side;
            w.px = side == 0 ? -half : half;
            w.py = a.long_m;
            w.k = (a.feder_kNm > 1.f ? a.feder_kNm : 150.f) * 1000.f;
            w.c = (a.daempfer > 0.1f ? a.daempfer : 12.f) * 1000.f;
            w.fmax = (a.maxforce_kN > 1.f ? a.maxforce_kN : 200.f) * 1000.f;
            w.driven = any_driven ? a.antrieb : (a.long_m < front - 0.01f);
            sum_k_kn += w.k / 1000.f;
            wheels_.push_back(w);
        }
    }
    body_freq_ = std::sqrt((std::max)(sum_k_kn / (mass_ / 1000.f), 0.f));
}

void BusBodyPhysics::sample_ground(const GroundFn& ground) {
    const float h = heading_deg_ / kRad2Deg;
    const float sh = std::sin(h), ch = std::cos(h);
    for (auto& w : wheels_) {
        const float wx = x_ + w.px * ch + w.py * sh;
        const float wy = y_ - w.px * sh + w.py * ch;
        w.ground_z = ground(wx, wy, strut_z(w) + 2.f);
    }
}

void BusBodyPhysics::place(float x, float y, float heading_deg, const GroundFn& ground) {
    x_ = x;
    y_ = y;
    heading_deg_ = heading_deg;
    pitch_ = roll_ = w_pitch_ = w_roll_ = 0.f;
    u_ = v_ = r_ = vz_ = 0.f;
    a_long_ = a_lat_ = 0.f;
    z_ = ground(x, y, 1e4f);
    sample_ground(ground);
    float gz = 0.f, sum_k = 0.f;
    for (const auto& w : wheels_) {
        gz += w.ground_z;
        sum_k += w.k;
    }
    if (!wheels_.empty()) gz /= static_cast<float>(wheels_.size());
    z_ = gz - mass_ * kG / (std::max)(sum_k, 1.f); // static sag
    // Let the springs settle (2 s, no input) so the bus starts at rest on uneven ground.
    Inputs rest{};
    rest.brake_force_n = mass_ * kG;
    for (int i = 0; i < 480; ++i) substep(kSubstep, rest);
    u_ = v_ = r_ = 0.f;
    x_ = x;
    y_ = y;
    heading_deg_ = heading_deg;
}

Obb BusBodyPhysics::body_box() const {
    return Obb::from_box(bb_, Vec3d{x_, y_, z_}, heading_deg_);
}

std::vector<BusBodyPhysics::Impact> BusBodyPhysics::collide(const std::vector<Obb>& obstacles, float dt) {
    constexpr float kRestitution = 0.2f, kScrapeFriction = 0.4f, kCrashSpeed = 0.4f;
    std::vector<Impact> impacts;
    if (!spawn_checked_) {
        const Obb me = body_box();
        for (const auto& o : obstacles)
            if (o.id >= 0 && me.contact(o)) spawned_inside_.push_back(o.id);
        spawn_checked_ = true;
    } else if (!spawned_inside_.empty()) {
        // once out of one, it counts again
        const Obb me = body_box();
        std::erase_if(spawned_inside_, [&](std::int64_t id) {
            for (const auto& o : obstacles)
                if (o.id == id && me.contact(o)) return false;
            return true;
        });
    }
    const auto inside = [&](std::int64_t id) {
        return id >= 0 && std::find(spawned_inside_.begin(), spawned_inside_.end(), id) != spawned_inside_.end();
    };
    const float hr0 = heading_deg_ / kRad2Deg;
    const Vec2d fwd{std::sin(hr0), std::cos(hr0)}, right{std::cos(hr0), -std::sin(hr0)};
    // world velocity of the body and its yaw rate (CCW +)
    Vec2d vel = fwd * u_ + right * v_;
    double w = r_;
    const double inv_m = 1.0 / mass_, inv_i = 1.0 / i_yaw_;
    std::vector<std::size_t> answered;
    for (int it = 0; it < 4; ++it) {
        const Obb me = body_box();
        int best = -1;
        Contact bc{};
        for (std::size_t i = 0; i < obstacles.size(); ++i) {
            const auto& o = obstacles[i];
            if (inside(o.id) || std::find(answered.begin(), answered.end(), i) != answered.end()) continue;
            const auto c = me.contact(o);
            if (!c || !std::isfinite(c->depth) || !std::isfinite(c->point.x) || !std::isfinite(c->point.y)) continue;
            // deeper than this frame's motion could have gone: we were not the one running into it
            const double rel = vel.length() + std::fabs(w) * 6.0;
            if (c->depth > 0.5 + rel * dt * 2.0) continue;
            if (best < 0 || c->depth > bc.depth) {
                best = static_cast<int>(i);
                bc = *c;
            }
        }
        if (best < 0) break;
        answered.push_back(static_cast<std::size_t>(best));
        const Vec2d n = bc.normal;
        const Vec2d r{bc.point.x - x_, bc.point.y - y_};
        const auto cross = [](Vec2d a, Vec2d b) { return a.x * b.y - a.y * b.x; };
        const auto point_vel = [&]() { return vel + Vec2d{-w * r.y, w * r.x}; };
        const auto k_at = [&](Vec2d dir) {
            const double rn = cross(r, dir);
            return inv_m + rn * rn * inv_i;
        };
        // out of the obstacle along the shortest way, a millimetre clear
        x_ += static_cast<float>(n.x * (bc.depth + 0.001));
        y_ += static_cast<float>(n.y * (bc.depth + 0.001));
        const Vec2d vp = point_vel();
        const double vn = vp.dot(n);
        if (vn >= 0.0) continue;
        const double before = 0.5 * mass_ * vel.dot(vel) + 0.5 * i_yaw_ * w * w;
        // a gentle push is dead, a real blow gives a little back
        const double e = -vn > 1.0 ? kRestitution : 0.0;
        const double kn = k_at(n);
        const double j = -(1.0 + e) * vn / kn;
        Vec2d p = n * j;
        const Vec2d vt = vp - n * vn;
        const double vt_len = vt.length();
        if (vt_len > 1e-3) {
            const Vec2d t = vt * (1.0 / vt_len);
            const double jt = (std::min)(vt_len / k_at(t), kScrapeFriction * j);
            p = p - t * jt;
        }
        vel = vel + p * inv_m;
        w += cross(r, p) * inv_i;
        const double after = 0.5 * mass_ * vel.dot(vel) + 0.5 * i_yaw_ * w * w;
        impacts.push_back(Impact{static_cast<float>(-vn), static_cast<float>((std::max)(before - after, 0.0)),
                                 obstacles[static_cast<std::size_t>(best)].id});
    }
    if (!impacts.empty()) {
        u_ = static_cast<float>(vel.dot(fwd));
        v_ = static_cast<float>(vel.dot(right));
        r_ = static_cast<float>(w);
        if (!std::isfinite(u_) || !std::isfinite(v_) || !std::isfinite(r_)) u_ = v_ = r_ = 0.f;
    }
    (void)kCrashSpeed;
    return impacts;
}

void BusBodyPhysics::step(float dt, const Inputs& in, const GroundFn& ground) {
    if (dt <= 0.f || wheels_.empty()) return;
    dt = (std::min)(dt, 0.1f);
    sample_ground(ground);
    const int n = std::clamp(static_cast<int>(std::ceil(dt / kSubstep)), 1, 32);
    const float h = dt / static_cast<float>(n);
    for (int i = 0; i < n; ++i) substep(h, in);
}

void BusBodyPhysics::substep(float h, const Inputs& in) {
    // --- struts: OMSI force = k * penetration - c * (body speed), capped at maxforce, no pull ---
    float fz = 0.f, t_pitch = 0.f, t_roll = 0.f;
    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        auto& w = wheels_[i];
        const float pen = w.ground_z - strut_z(w);
        const float rate = -(vz_ + w.py * w_pitch_ + w.px * w_roll_);
        float f = 0.f;
        if (pen > 0.f) {
            const float sf = std::clamp(i < 8 ? in.spring_factor[i] : 1.f, 0.2f, 2.f);
            f = std::clamp(w.k * sf * pen + w.c * rate, 0.f, w.fmax);
        }
        w.compression = (std::max)(pen, 0.f);
        w.compression_rate = rate;
        w.load = f;
        fz += f;
        t_pitch += f * w.py;
        t_roll += f * w.px;
    }

    // --- tyres: longitudinal and side forces (body frame) ---
    float n_total = 0.f, n_driven = 0.f;
    for (const auto& w : wheels_) {
        n_total += w.load;
        if (w.driven) n_driven += w.load;
    }
    // OMSI's curvature is + to the right (steering_left drives it negative); here + = left (CCW yaw).
    const float kappa = -in.inv_turn_radius;
    float fx = 0.f, fy = 0.f, t_yaw = 0.f;
    // per axle (wheels come in L/R pairs)
    for (std::size_t i = 0; i + 1 < wheels_.size(); i += 2) {
        const auto& wl = wheels_[i];
        const auto& wr = wheels_[i + 1];
        const float py = wl.py;
        const float n_axle = wl.load + wr.load;
        if (n_axle <= 1.f) continue;
        const float delta = std::atan(kappa * (py - rot_pnt_long_));
        const float sd = std::sin(delta), cd = std::cos(delta);
        const float v_lat = v_ - r_ * py;
        const float v_f = -v_lat * sd + u_ * cd;
        const float v_n = -v_lat * cd - u_ * sd;
        const float alpha = std::atan2(v_n, (std::max)(std::fabs(v_f), 0.5f));
        // cornering stiffness scales with the static share of the axle (load sensitivity is in mu)
        const float c_alpha = corner_k_ * mass_ * kG * (n_axle / (std::max)(n_total, 1.f));
        float f_n = -c_alpha * alpha;
        const float grip = mu_ * n_axle;
        f_n = std::clamp(f_n, -grip, grip);
        fx += -f_n * cd;
        fy += -f_n * sd;
        t_yaw += py * f_n * cd;
    }

    // longitudinal: traction limited by the driven wheels' grip, brakes by all of them
    const float drive = std::clamp(in.drive_force_n, -mu_ * n_driven, mu_ * n_driven);
    const float resist = (std::min)(in.brake_force_n, mu_ * n_total) + (std::max)(in.rolling_force_n, 0.f);
    float f_long = fy;
    bool hold = false;
    if (std::fabs(u_) < 0.05f) {
        if (resist >= std::fabs(drive)) hold = true;
        else f_long += drive - sgn(drive) * resist;
    } else {
        f_long += drive - sgn(u_) * resist;
    }

    // --- integrate the plane (body frame, rotating) ---
    const float u0 = u_;
    a_long_ = hold ? 0.f : f_long / mass_;
    a_lat_ = fx / mass_;
    if (hold) {
        u_ = 0.f;
        v_ *= 0.5f;
        r_ *= 0.5f;
    } else {
        const float du = (f_long / mass_ - r_ * v_) * h;
        const float dv = (fx / mass_ + r_ * u_) * h;
        u_ += du;
        v_ += dv;
        r_ += t_yaw / i_yaw_ * h;
        // resisting forces stop the bus, they never drive it backwards
        if (u0 * u_ < 0.f && std::fabs(drive) < resist) u_ = 0.f;
    }
    // walking pace and below: the kinematic turn (no slip), so the bus neither creeps nor jitters
    const float s = std::clamp((std::fabs(u_) - 0.8f) / 2.f, 0.f, 1.f);
    const float r_kin = u_ * kappa;
    const float v_kin = r_kin * rot_pnt_long_;
    r_ = s * r_ + (1.f - s) * r_kin;
    v_ = s * v_ + (1.f - s) * v_kin;

    heading_deg_ -= r_ * h * kRad2Deg;
    if (heading_deg_ >= 360.f) heading_deg_ -= 360.f;
    if (heading_deg_ < 0.f) heading_deg_ += 360.f;
    const float hr = heading_deg_ / kRad2Deg;
    const float sh = std::sin(hr), ch = std::cos(hr);
    x_ += (u_ * sh + v_ * ch) * h;
    y_ += (u_ * ch - v_ * sh) * h;

    // --- integrate the body: heave, pitch (squat/dive), roll (lean out of the turn) ---
    vz_ += (fz / mass_ - kG) * h;
    z_ += vz_ * h;
    w_pitch_ += (t_pitch + mass_ * a_long_ * cog_h_) / i_pitch_ * h;
    w_roll_ += (t_roll + mass_ * a_lat_ * cog_h_) / i_roll_ * h;
    // OMSI's per-frame sin(x)/x damping of pitch and roll, x = 1.5 * body_freq * frame
    const float x0 = 1.5f * body_freq_ * kOmsiFrame;
    if (x0 > 1e-6f) {
        const float k = std::pow(std::clamp(std::sin(x0) / x0, 0.1f, 1.f), h / kOmsiFrame);
        w_pitch_ *= k;
        w_roll_ *= k;
    }
    pitch_ = std::clamp(pitch_ + w_pitch_ * h, -0.35f, 0.35f);
    roll_ = std::clamp(roll_ + w_roll_ * h, -0.35f, 0.35f);
}

} // namespace omsi::vehicle
