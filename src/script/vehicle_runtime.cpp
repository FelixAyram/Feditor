#include "omsi/utf8_win32.hpp"
#include "omsi/script/vehicle_runtime.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include "omsi/vehicle_cabin.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <regex>

namespace omsi::script {
namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kG = 9.81f;

void set_if_exists(VarRegistry& vars, std::vector<float>& floats, const std::string& name, float value) {
    if (const auto idx = vars.float_index(name)) floats.at(static_cast<std::size_t>(*idx)) = value;
}

float sum_axle_brakeforce(const VarRegistry& vars, const std::vector<float>& floats) {
    float sum = 0.f;
    if (const auto bf = vars.float_index("Brakeforce")) {
        sum = floats.at(static_cast<std::size_t>(*bf));
        if (sum > 1.f) return sum;
    }
    for (int axle = 0; axle < 4; ++axle) {
        for (const char* side : {"L", "R"}) {
            const std::string name = "Axle_Brakeforce_" + std::to_string(axle) + "_" + side;
            if (const auto idx = vars.float_index(name)) sum += floats.at(static_cast<std::size_t>(*idx));
        }
    }
    return sum;
}

} // namespace

VehicleRuntime::~VehicleRuntime() { stop_async_sim(); }

void VehicleRuntime::apply_bus_physics(const bus::BusDocument& bus) {
    mass_t_ = bus.mass_t > 0.1f ? bus.mass_t : 10.f;
    rollwiderstand_N_ = bus.rollwiderstand_N > 0.f ? bus.rollwiderstand_N : 900.f;
    schwerpunkt_m_ = bus.schwerpunkt_m > 0.f ? bus.schwerpunkt_m : 1.2f;
    inv_min_turnradius_ = bus.inv_min_turnradius > 0.01f ? bus.inv_min_turnradius : 0.13f;
    inertia_roll_ = bus.inertia_x > 1.f ? bus.inertia_x : 500.f;
    inertia_pitch_ = bus.inertia_y > 1.f ? bus.inertia_y : 200.f;
    rot_pnt_long_m_ = bus.rot_pnt_long_m;
    axles_ = bus.axles;
    wheel_radius_m_ = 0.47f;
    for (const auto& a : axles_) {
        if (a.raddurchmesser_m > 0.1f) {
            wheel_radius_m_ = a.raddurchmesser_m * 0.5f;
            if (a.antrieb) break; // OMSI: first driven axle sets drive diameter
        }
    }
    omsi::log::info("vehicle physics: mass=" + std::to_string(mass_t_) + "t roll=" +
                    std::to_string(rollwiderstand_N_) + "N r_wheel=" + std::to_string(wheel_radius_m_) +
                    "m invRmax=" + std::to_string(inv_min_turnradius_) + " axles=" +
                    std::to_string(axles_.size()));
}

bool VehicleRuntime::load(const bus::BusDocument& bus, const std::filesystem::path& omsi_root) {
    reset();
    apply_bus_physics(bus);

    if (!vars_.load_engine_lists(omsi_root))
        omsi::log::warn("OMSI vehicle: engine varlists missing, continuing with bus-only lists");
    vars_.merge_bus_lists(bus.bus_dir, bus.varnamelist_files, bus.stringvarnamelist_files);

    public_floats_.assign(vars_.float_var_count(), 0.f);
    public_strings_.assign(vars_.string_var_count(), std::string{});

    constfiles_ = load_constfiles(bus.bus_dir, bus.constfile_paths);
    for (const auto& doc : constfiles_) {
        for (const auto& [name, value] : doc.constants) merged_consts_[name] = value;
        for (const auto& [name, curve] : doc.curves) merged_curves_[name] = curve;
    }

    if (!osc_.load(bus.bus_dir, bus.script_files, vars_)) {
        omsi::log::warn("OMSI vehicle: no OSC scripts loaded");
    }

    osc_.bind_vars(&public_floats_, &vars_);
    osc_.bind_strings(&public_strings_);
    osc_.bind_consts(&merged_consts_);
    osc_.bind_curves(&merged_curves_);
    osc_.bind_sound(&sound_);
    osc_.bind_system_vars(weather_temperature_c_);
    osc_.set_ai_mode(ai_mode_);

    if (!bus.sound_cfg_rel.empty()) {
        sound_.load(bus.bus_dir, bus.sound_cfg_rel);
        sound_.bind_vars(&public_floats_, &vars_);
    }

    // Unit128: paths / cabin load with the .bus (AI walk + seats).
    if (!bus.paths_cfg_rel.empty()) {
        const auto p = bus.bus_dir / bus.paths_cfg_rel;
        paths_ = cabin::load_vehicle_paths(p);
        if (!paths_) omsi::log::warn("vehicle paths missing: " + omsi::path_text(p));
    }
    if (!bus.passengercabin_cfg_rel.empty()) {
        const auto p = bus.bus_dir / bus.passengercabin_cfg_rel;
        cabin_ = cabin::load_passenger_cabin(p);
        if (!cabin_) omsi::log::warn("passenger cabin missing: " + omsi::path_text(p));
    }

    init_engine_defaults();
    osc_.run_init();
    // Matrix compares GetTime vs *ActTimer; timer=0 ⇒ multi-hour delta ⇒ Line_change spam.
    {
        constexpr float kDayStart = 8.f * 3600.f; // matches push_system_state
        set_if_exists(vars_, public_floats_, "Matrix_NrActTimer", kDayStart);
        set_if_exists(vars_, public_floats_, "Matrix_TerminusActTimer", kDayStart);
        if (const auto ibis = vars_.float_index("IBIS_Linie_Complex")) {
            const float line = public_floats_.at(static_cast<std::size_t>(*ibis));
            set_if_exists(vars_, public_floats_, "Matrix_Nr_Last", line);
            set_if_exists(vars_, public_floats_, "Matrix_NrAct", line);
        }
    }
    // engine_init uses RPN that can yield negative tank on our VM until fully verified; clamp for drivability.
    if (public_float("engine_tank_content") < 10.f) set_public_float("engine_tank_content", 200.f);
    if (public_float("engine_temperature") < 1.f) set_public_float("engine_temperature", weather_temperature_c_);
    update_suspension(1.f / 60.f);
    update_steering(0.f);

    omsi::log::info("OMSI vehicle runtime: floats=" + std::to_string(public_floats_.size()) + " consts=" +
                    std::to_string(merged_consts_.size()) + " triggers=" + std::to_string(osc_.stats().trigger_count) +
                    " sounds=" + std::to_string(sound_.definition_count()));
    return true;
}

void VehicleRuntime::reset() {
    stop_async_sim();
    vars_ = VarRegistry{};
    public_floats_.clear();
    public_strings_.clear();
    merged_consts_.clear();
    merged_curves_.clear();
    constfiles_.clear();
    axles_.clear();
    paths_.reset();
    cabin_.reset();
    sim_time_ = 0.f;
    velocity_ = 0.f;
    throttle_ = 0.f;
    brake_ = 0.f;
    mass_t_ = 10.f;
    rollwiderstand_N_ = 900.f;
    schwerpunkt_m_ = 1.2f;
    wheel_radius_m_ = 0.47f;
    inv_min_turnradius_ = 0.13f;
    inv_lenkradius_ = 0.f;
    steer_left_ = steer_right_ = steer_neutral_ = false;
    accel_ms2_ = 0.f;
    body_heave_m_ = body_pitch_rad_ = body_roll_rad_ = 0.f;
    body_heave_v_ = body_pitch_v_ = body_roll_v_ = 0.f;
    for (float& x : susp_x_) x = 0.f;
    for (float& v : susp_v_) v = 0.f;
}

void VehicleRuntime::init_engine_defaults() {
    set_if_exists(vars_, public_floats_, "Velocity", 0.f);
    set_if_exists(vars_, public_floats_, "Velocity_Ground", 0.f);
    set_if_exists(vars_, public_floats_, "M_Wheel", 0.f);
    set_if_exists(vars_, public_floats_, "n_Wheel", 0.f);
    set_if_exists(vars_, public_floats_, "Brakeforce", 0.f);
    // Cold start: battery OFF. E (cp_batterietrennschalter_toggle) turns it ON.
    // If we init ON, the first E toggles OFF and looks like a double-press bug.
    set_if_exists(vars_, public_floats_, "elec_busbar_main", 0.f);
    set_if_exists(vars_, public_floats_, "elec_busbar_avail", 0.f);
    set_if_exists(vars_, public_floats_, "elec_busbar_main_sw", 0.f);
    set_if_exists(vars_, public_floats_, "elec_V_battery", 1.f);
    set_if_exists(vars_, public_floats_, "elec_V_generator", 0.f);
    set_if_exists(vars_, public_floats_, "elec_failure_general", 0.f);
    set_if_exists(vars_, public_floats_, "engine_starter_antirepeat", 0.f);
    set_if_exists(vars_, public_floats_, "engine_on", 0.f);
    set_if_exists(vars_, public_floats_, "engine_injection_on", 0.f);
    set_if_exists(vars_, public_floats_, "engine_idle_control", 0.f);
    set_if_exists(vars_, public_floats_, "engine_last_n", 0.f);
    set_if_exists(vars_, public_floats_, "engine_n", 0.f);
    // Avoid first-frame (T.L.ev_engineshutdown): script fires when !shutoff_finished && n<400.
    set_if_exists(vars_, public_floats_, "engine_shutoff_finished", 1.f);
    set_if_exists(vars_, public_floats_, "cp_taster_anlasser", 0.f);
    set_if_exists(vars_, public_floats_, "cp_Temperatur", weather_temperature_c_);
    set_if_exists(vars_, public_floats_, "engine_temperature", weather_temperature_c_);
    set_if_exists(vars_, public_floats_, "engine_temperature_envir", weather_temperature_c_);
    set_if_exists(vars_, public_floats_, "cp_Wind_Velocity", 0.f);
    // Neutral gear required by engine.osc kw_m_enginestart (antrieb_getr_gangwahl == 1).
    set_if_exists(vars_, public_floats_, "antrieb_getr_gangwahl", 1.f);
    set_if_exists(vars_, public_floats_, "antrieb_getr_aktugang", 0.f);
    set_if_exists(vars_, public_floats_, "BlokovaniDveri", 0.f);
    set_if_exists(vars_, public_floats_, "nouzdvere_1_sw", 0.f);
    set_if_exists(vars_, public_floats_, "nouzdvere_2_sw", 0.f);
    set_if_exists(vars_, public_floats_, "bremse_p_Tank04", 800000.f);
    set_if_exists(vars_, public_floats_, "bremse_p_tank01", 900000.f);
    set_if_exists(vars_, public_floats_, "bremse_p_tank02", 900000.f);
    set_if_exists(vars_, public_floats_, "bremse_p_tank03", 900000.f);
    set_if_exists(vars_, public_floats_, "bremse_p_tank04", 900000.f);
    set_if_exists(vars_, public_floats_, "bremse_p_Tank01", 900000.f);
    set_if_exists(vars_, public_floats_, "bremse_p_Tank02", 900000.f);
    set_if_exists(vars_, public_floats_, "bremse_p_Tank03", 900000.f);
    set_if_exists(vars_, public_floats_, "bremse_p_leitung01", 900000.f);
    set_if_exists(vars_, public_floats_, "bremse_p_leitung02", 900000.f);
    // Already "filled" so first frame does not fire (T.L.ev_kompressor_ein).
    set_if_exists(vars_, public_floats_, "bremse_kompressor", 1.f);
    set_if_exists(vars_, public_floats_, "engine_tank_content", 200.f);
    set_if_exists(vars_, public_floats_, "engine_failure_general", 0.f);
    // Parking brake ON at spawn (OMSI-like; release with '.').
    set_if_exists(vars_, public_floats_, "bremse_feststell", 1.f);
    set_if_exists(vars_, public_floats_, "bremse_feststell_sw", 1.f);
    // bremse.osc cold bags: rear often softer (heck sits lower) until compressor fills.
    for (int axle = 0; axle < 4; ++axle) {
        for (const char* side : {"L", "R"}) {
            const std::string ax = std::to_string(axle);
            const bool rear = axle >= 1;
            set_if_exists(vars_, public_floats_, "bremse_p_balg_" + ax + "_" + side, rear ? 280000.f : 350000.f);
            set_if_exists(vars_, public_floats_, "Axle_Springfactor_" + ax + "_" + side, rear ? 0.5f : 0.65f);
        }
    }
}

float VehicleRuntime::public_float(const std::string& name, float default_value) const {
    if (const auto idx = vars_.float_index(name)) return public_floats_.at(static_cast<std::size_t>(*idx));
    return default_value;
}

void VehicleRuntime::set_public_float(const std::string& name, float value) {
    set_if_exists(vars_, public_floats_, name, value);
}

void VehicleRuntime::set_external_motion(float v_kmh, float accel_ms2, const float* compression, int n_wheels) {
    velocity_ = v_kmh;
    accel_ms2_ = accel_ms2;
    set_public_float("Velocity", velocity_);
    set_public_float("Velocity_Ground", velocity_);
    if (!compression) return;
    for (int i = 0; i < n_wheels && i < 8; ++i) {
        const std::string name = "Axle_Suspension_" + std::to_string(i / 2) + "_" + (i % 2 == 0 ? "L" : "R");
        if (vars_.float_index(name)) set_public_float(name, -compression[i]);
    }
}

void VehicleRuntime::debug_set_velocity_kmh(float kmh) {
    velocity_ = kmh;
    set_public_float("Velocity", kmh);
    set_public_float("Velocity_Ground", kmh);
    const float r = (std::max)(wheel_radius_m_, 0.2f);
    const float rpm = (kmh / 3.6f) / r * 60.f / kTwoPi;
    set_public_float("n_Wheel", rpm);
}

void VehicleRuntime::apply_trigger(const std::string& trigger_name) { osc_.fire_trigger(trigger_name); }

void VehicleRuntime::apply_cti(const omsi::cti::CtiDocument& cti) {
    for (const auto& [name, value] : cti.setvars) set_public_float(name, value);
    omsi::log::info("applied cti setvars: " + omsi::path_text(cti.path.filename()) + " count=" +
                    std::to_string(cti.setvars.size()) + " items=" + std::to_string(cti.items.size()));
}

void VehicleRuntime::trigger_xml(const std::string& name, int value) {
    if (name.empty()) return;
    if (value == 1)
        apply_trigger(name);
    else if (value == 2)
        apply_trigger(name + "_drag");
    else
        apply_trigger(name + "_off");
}

void VehicleRuntime::apply_triggers(const std::vector<std::string>& pressed,
                                     const std::vector<std::string>& released) {
    const float engine_on_before = public_float("engine_on", 0.f);

    for (const auto& t : pressed) {
        if (t == "kw_m_enginestart") {
            const float elec = public_float("elec_busbar_main", 0.f);
            const float sw = public_float("cp_schalter_batterie", 0.f);
            const float gang = public_float("antrieb_getr_gangwahl", 0.f);
            const float on = public_float("engine_on", 0.f);
            const float antirep = public_float("engine_starter_antirepeat", 0.f);
            const float anlasser = public_float("cp_taster_anlasser", 0.f);
            // engine_1620.osc gate: !antirepeat && !engine_on && gangwahl==1 && elec_busbar_main
            std::string fail;
            if (antirep > 0.5f) fail += "antirepeat ";
            if (on > 0.5f) fail += "already_on ";
            if (std::fabs(gang - 1.f) > 0.1f) fail += "gangwahl!=1 ";
            if (elec < 0.5f) fail += "no_elec ";
            if (fail.empty()) {
                omsi::log::info("engine start GATE OK: elec=" + std::to_string(elec) + " sw=" + std::to_string(sw) +
                                " gangwahl=" + std::to_string(gang) + " antirep=" + std::to_string(antirep) +
                                " anlasser=" + std::to_string(anlasser));
            } else {
                omsi::log::warn("engine start GATE FAIL [" + fail + "]: elec=" + std::to_string(elec) +
                                " sw=" + std::to_string(sw) + " gangwahl=" + std::to_string(gang) +
                                " engine_on=" + std::to_string(on) + " antirep=" + std::to_string(antirep) +
                                " anlasser=" + std::to_string(anlasser) +
                                " — tip: E enables battery, N sets Neutral, hold M to start");
            }
        }
        osc_.fire_trigger(t);
        if (t == "kw_m_enginestart") {
            const float on_after = public_float("engine_on", 0.f);
            const float antirep = public_float("engine_starter_antirepeat", 0.f);
            const float anlasser = public_float("cp_taster_anlasser", 0.f);
            const float inj = public_float("injection", 0.f);
            omsi::log::info("engine start AFTER: engine_on=" + std::to_string(on_after) +
                            " inj=" + std::to_string(inj) + " antirep=" + std::to_string(antirep) +
                            " anlasser=" + std::to_string(anlasser) +
                            " rpm=" + std::to_string(public_float("engine_n", 0.f)));
        }
    }
    for (const auto& t : released) osc_.fire_trigger(t);

    const float engine_on_after = public_float("engine_on", 0.f);
    if (engine_on_before < 0.5f && engine_on_after > 0.5f) {
        omsi::log::info("*** ENGINE STARTED *** engine_on 0->1 rpm=" +
                        std::to_string(public_float("engine_n", 0.f)) +
                        " idle_ctrl=" + std::to_string(public_float("idle_control", 0.f)));
    } else if (engine_on_before > 0.5f && engine_on_after < 0.5f) {
        omsi::log::info("*** ENGINE STOPPED *** engine_on 1->0");
    }
}

void VehicleRuntime::set_driver_input(float throttle, float brake) {
    throttle_ = std::clamp(throttle, 0.f, 1.f);
    brake_ = std::clamp(brake, 0.f, 1.f);
    // OMSI physics publishes these every tick for scripts (engine/antrieb).
    set_public_float("Throttle", throttle_);
    set_public_float("Brake", brake_);
}

void VehicleRuntime::set_mouse_drive_mode(bool active) { mouse_drive_mode_ = active; }

void VehicleRuntime::set_mouse_drive_input(float throttle, float brake, float inv_lenkradius) {
    set_driver_input(throttle, brake);
    mouse_inv_lenkradius_ = inv_lenkradius;
}

void VehicleRuntime::set_steering_keys(bool left, bool right, bool neutral) {
    steer_left_ = left;
    steer_right_ = right;
    steer_neutral_ = neutral;
}

void VehicleRuntime::set_ai_mode(bool ai) {
    ai_mode_ = ai;
    osc_.set_ai_mode(ai);
}

void VehicleRuntime::set_mouse(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;
}

void VehicleRuntime::push_system_state(float dt_sec) {
    OscSystemState st{};
    st.timegap = dt_sec;
    // OMSI GetTime: seconds since midnight; use wall-clock-ish from sim day start.
    constexpr float kDayStart = 8.f * 3600.f; // 08:00
    st.get_time = kDayStart + sim_time_;
    st.weather_temperature = weather_temperature_c_;
    st.weather_abs_hum = 8.f;
    st.day = 10.f;
    st.month = 7.f;
    st.year = 2026.f;
    st.day_of_year = 191.f;
    st.mouse_x = mouse_x_;
    st.mouse_y = mouse_y_;
    st.coll_energy = 0.f;
    st.coll_pos_y = 0.f;
    st.coll_pos_z = 0.f;
    osc_.set_system_state(st);
}

void VehicleRuntime::update_steering(float dt_sec) {
    // Unit129 KeyCommand: continuous keys ramp inv_lenkradius (±inv_min_turnradius from .bus).
    if (mouse_drive_mode_) {
        inv_lenkradius_ = std::clamp(mouse_inv_lenkradius_, -inv_min_turnradius_, inv_min_turnradius_);
    } else {
        // Release holds last angle; KP5 (steering_neutral) centers — matches keyboard.cfg continuous bits.
        float target = inv_lenkradius_;
        if (steer_neutral_)
            target = 0.f;
        else if (steer_left_ && !steer_right_)
            target = -inv_min_turnradius_; // KP4 left → negative inv (OMSI axle left turn visual)
        else if (steer_right_ && !steer_left_)
            target = inv_min_turnradius_;

        // Dyn. Lenkgeschwindigkeit-ish: ~full lock in ~1.2 s.
        const float rate = inv_min_turnradius_ / 1.2f;
        const float step = rate * dt_sec;
        if (inv_lenkradius_ < target)
            inv_lenkradius_ = (std::min)(inv_lenkradius_ + step, target);
        else if (inv_lenkradius_ > target)
            inv_lenkradius_ = (std::max)(inv_lenkradius_ - step, target);
    }

    // Bicycle + simple Ackermann. Wheelbase from first/last axle longs (OMSI Y forward).
    float y_front = 3.4f;
    float y_rear = -2.f;
    float track_half = 1.1f;
    if (axles_.size() >= 2) {
        y_front = axles_.front().long_m;
        y_rear = axles_.back().long_m;
        track_half = 0.5f * 0.5f * (axles_.front().maxwidth + axles_.front().minwidth);
    } else if (!axles_.empty()) {
        y_front = axles_.front().long_m;
        track_half = 0.25f * (axles_.front().maxwidth + axles_.front().minwidth);
    }
    const float wb = (std::max)(std::fabs(y_front - y_rear), 1.f);
    const float inv = inv_lenkradius_;
    // δ = atan(L / R) = atan(L * inv_R); positive inv → left (OMSI Axle_Steering sign).
    auto steer_at = [&](float y_axle, float lat_sign) {
        if (std::fabs(inv) < 1e-6f) return 0.f;
        // Distance from rear axle along wheelbase for this axle.
        const float L_axle = std::fabs(y_axle - y_rear);
        const float denom = 1.f - lat_sign * track_half * inv;
        if (std::fabs(denom) < 1e-4f) return std::atan(L_axle * inv);
        return std::atan((L_axle * inv) / denom);
    };

    const int n_axles = axles_.empty() ? 2 : static_cast<int>(axles_.size());
    for (int axle = 0; axle < 4; ++axle) {
        const float y = (axle < static_cast<int>(axles_.size())) ? axles_[static_cast<std::size_t>(axle)].long_m
                                                                 : (axle == 0 ? y_front : y_rear);
        // Only axles near the front steer (ahead of midpoint of wheelbase).
        const float mid = 0.5f * (y_front + y_rear);
        const bool steers = axle < n_axles && y >= mid - 0.5f;
        const float ang_L = steers ? steer_at(y, +1.f) : 0.f;
        const float ang_R = steers ? steer_at(y, -1.f) : 0.f;
        set_public_float("Axle_Steering_" + std::to_string(axle) + "_L", ang_L);
        set_public_float("Axle_Steering_" + std::to_string(axle) + "_R", ang_R);
        (void)wb;
    }
}

void VehicleRuntime::update_suspension(float dt_sec) {
    // OMSI Federweg ~ bremse_level_setpoint (−0.105). Soft Axle_Springfactor (deflated bags)
    // increases compression so the rear sits lower; compressor fills → rise toward setpoint.
    // Do NOT use raw F/k ≈ 0.3 m — that clamped at 0.35 and lifted rear tires into the arches.
    constexpr float kNeutral = 0.105f;
    const float mass_kg = (std::max)(mass_t_ * 1000.f, 1000.f);
    const int n_axles = axles_.empty() ? 2 : static_cast<int>((std::min)(axles_.size(), std::size_t{4}));
    if (n_axles < 1) return;

    float y_front = axles_.empty() ? 3.4f : axles_.front().long_m;
    float y_rear = axles_.empty() ? -2.f : axles_[static_cast<std::size_t>(n_axles - 1)].long_m;
    float track = 2.2f;
    if (!axles_.empty()) track = 0.5f * (axles_.front().maxwidth + axles_.front().minwidth);
    const float wb = (std::max)(std::fabs(y_front - y_rear), 1.f);

    const float cog_y = (std::fabs(rot_pnt_long_m_) > 0.05f) ? rot_pnt_long_m_ : 0.5f * (y_front + y_rear);
    const float weight = mass_kg * kG;
    float w_front = weight * std::fabs(cog_y - y_rear) / wb;
    float w_rear = weight - w_front;
    const float transfer = mass_kg * accel_ms2_ * schwerpunkt_m_ / wb;
    w_front -= transfer;
    w_rear += transfer;
    const float v_ms = velocity_ / 3.6f;
    const float a_cent = v_ms * v_ms * std::fabs(inv_lenkradius_);
    const float a_lat_right = (inv_lenkradius_ < 0.f) ? a_cent : (inv_lenkradius_ > 0.f ? -a_cent : 0.f);
    const float lat_transfer = mass_kg * a_lat_right * schwerpunkt_m_ / (std::max)(track, 1.f);
    w_front = (std::max)(w_front, 0.f);
    w_rear = (std::max)(w_rear, 0.f);

    float sum_x = 0.f, front_sum = 0.f, rear_sum = 0.f, left_sum = 0.f, right_sum = 0.f;
    int front_n = 0, rear_n = 0;

    for (int axle = 0; axle < n_axles; ++axle) {
        float y = (axle == 0) ? y_front : y_rear;
        if (axle < static_cast<int>(axles_.size())) y = axles_[static_cast<std::size_t>(axle)].long_m;
        const bool is_front = y >= 0.5f * (y_front + y_rear);
        const float axle_w = is_front ? w_front : w_rear;
        const float w_static_axle =
            (is_front ? (weight * std::fabs(cog_y - y_rear) / wb) : (weight - weight * std::fabs(cog_y - y_rear) / wb));

        for (int side = 0; side < 2; ++side) {
            const int idx = axle * 2 + side;
            const char* side_ch = side == 0 ? "L" : "R";
            float corner_w = axle_w * 0.5f;
            if (side == 0)
                corner_w -= 0.5f * lat_transfer / static_cast<float>(n_axles);
            else
                corner_w += 0.5f * lat_transfer / static_cast<float>(n_axles);
            corner_w = (std::max)(corner_w, 0.f);

            float factor = public_float("Axle_Springfactor_" + std::to_string(axle) + "_" + side_ch, 0.65f);
            if (factor < 0.05f) factor = 0.65f;
            factor = std::clamp(factor, 0.5f, 1.5f);

            // Deflated bag (factor→0.5) → travel up to ~0.21; filled (1.0) → 0.105.
            float travel = kNeutral / factor;
            // OA101 cold air: hinterachse often sits lower until bags fill (user/OMSI squat).
            if (!is_front && factor < 0.85f) travel += 0.04f * (0.85f - factor);
            const float w_static_c = w_static_axle * 0.5f;
            // Dynamic load (brake/accel/turn) as centimetres around bag rest.
            travel += std::clamp((corner_w - w_static_c) / (mass_kg * kG) * 0.35f, -0.04f, 0.04f);
            travel = std::clamp(travel, 0.06f, 0.22f);
            susp_x_[idx] = travel;
            susp_v_[idx] = 0.f;
            set_public_float("Axle_Suspension_" + std::to_string(axle) + "_" + side_ch, -travel);

            sum_x += travel;
            if (is_front) {
                front_sum += travel;
                ++front_n;
            } else {
                rear_sum += travel;
                ++rear_n;
            }
            if (side == 0)
                left_sum += travel;
            else
                right_sum += travel;
            (void)idx;
            (void)y;
        }
    }

    const float corners = static_cast<float>((std::max)(n_axles * 2, 1));
    const float avg = sum_x / corners;
    const float f_avg = front_n > 0 ? front_sum / static_cast<float>(front_n) : avg;
    const float r_avg = rear_n > 0 ? rear_sum / static_cast<float>(rear_n) : avg;
    // Soft bags: heck sits lower. Viewer RotationX: negative pitch = nose down / heck up was
    // inverted vs mesh Y-forward, so soft-rear uses negative pitch here.
    body_heave_m_ = std::clamp(-(avg - kNeutral) * 0.35f, -0.06f, 0.02f);
    body_pitch_rad_ = std::clamp(-0.028f - (r_avg - f_avg) / wb * 2.5f, -0.08f, 0.02f);
    body_roll_rad_ = std::clamp((right_sum - left_sum) / (corners * 0.5f) / (std::max)(track, 1.f), -0.1f, 0.1f);
    // Blend in lateral lean from cornering.
    body_roll_rad_ = std::clamp(body_roll_rad_ + std::copysign(a_cent * 0.012f, -inv_lenkradius_), -0.12f, 0.12f);
    (void)inertia_pitch_;
    (void)inertia_roll_;
    (void)dt_sec;
}

void VehicleRuntime::integrate_chassis(float dt_sec) {
    // OMSI.exe chassis: scripts set M_Wheel (kNm) + Brakeforce/Axle_Brakeforce (N);
    // native integrates mass / roll resistance. Brake only removes energy (wiki Fahrzeugleistung).
    const float mass_kg = (std::max)(mass_t_ * 1000.f, 1000.f);
    const float r = (std::max)(wheel_radius_m_, 0.2f);
    float M_nm = public_float("rad_M_ab", public_float("M_Wheel", 0.f));
    const float gang = public_float("antrieb_getr_gangwahl", 1.f);
    const float aktugang = public_float("antrieb_getr_aktugang", 0.f);
    // antrieb.osc: open drivetrain / not in gear → no wheel torque.
    const bool in_drive = std::fabs(gang - 1.f) > 0.1f && std::fabs(aktugang) > 0.5f;
    const bool in_reverse = std::fabs(gang) < 0.1f && std::fabs(aktugang) > 0.5f;
    // Only buses whose scripts have these gearbox vars (Allison / MAN family) gate the torque here;
    // OMSI itself never does, every other bus leaves M_Wheel at 0 when it is out of gear.
    const bool has_gearbox_vars =
        vars_.float_index("antrieb_getr_gangwahl").has_value() && vars_.float_index("antrieb_getr_aktugang").has_value();
    if (has_gearbox_vars && !in_drive && !in_reverse) M_nm = 0.f;
    // Publish canonical name for scripts that read M_Wheel.
    if (std::fabs(public_float("M_Wheel", 0.f) - M_nm) > 1e-4f) set_public_float("M_Wheel", M_nm);
    M_nm = std::clamp(M_nm, -200000.f, 200000.f); // OMSI M_Wheel is N m (all driven wheels)

    const float F_drive = M_nm / r; // N  (tau / r)
    float F_brake = sum_axle_brakeforce(vars_, public_floats_);
    const bool park_on = public_float("bremse_feststell", 0.f) > 0.5f;
    if (park_on) {
        F_brake = (std::max)(F_brake, mass_kg * kG * 0.6f);
    } else if (brake_ > 0.05f) {
        // Service pedal: scale script brake force to pedal position if scripts leave it tiny.
        const float pedal_cap = mass_kg * kG * 0.6f * brake_;
        if (F_brake < pedal_cap * 0.2f) F_brake = pedal_cap;
    }

    float v_ms = velocity_ / 3.6f;
    if (external_chassis_) {
        chassis_forces_ = ChassisForces{F_drive, (std::max)(F_brake, 0.f), rollwiderstand_N_};
    } else {
    constexpr float kStopEps = 0.05f; // m/s
    float F_roll = 0.f;
    if (std::fabs(v_ms) > kStopEps)
        F_roll = rollwiderstand_N_ * (v_ms > 0.f ? 1.f : -1.f);

    float F_net = 0.f;
    if (std::fabs(v_ms) <= kStopEps) {
        // Standstill: brake cancels opposing forces up to capacity; never injects energy.
        const float F_attempt = F_drive;
        if (F_brake >= std::fabs(F_attempt)) {
            F_net = 0.f;
            v_ms = 0.f;
        } else {
            F_net = F_attempt - std::copysign(F_brake, F_attempt == 0.f ? 1.f : F_attempt);
        }
    } else {
        F_net = F_drive - std::copysign(F_brake, v_ms) - F_roll;
    }

    const float v_prev = v_ms;
    v_ms += (F_net / mass_kg) * dt_sec;
    accel_ms2_ = (v_ms - v_prev) / (std::max)(dt_sec, 1e-4f);
    // Crossing zero while braking → stick (no reverse from brake).
    if (F_brake > 1.f && v_prev * v_ms < 0.f) {
        v_ms = 0.f;
        accel_ms2_ = 0.f;
    }
    if (std::fabs(v_ms) < kStopEps && F_brake > 1.f) {
        v_ms = 0.f;
        accel_ms2_ = 0.f;
    }

    velocity_ = v_ms * 3.6f;
    velocity_ = std::clamp(velocity_, -80.f, 100.f);
    } // !external_chassis_

    set_public_float("Velocity", velocity_);
    set_public_float("Velocity_Ground", velocity_);

    // ω = v/r ; n_Wheel and Wheel_RotationSpeed are RPM (OMSIWiki).
    const float omega = v_ms / r;
    const float rpm = omega * 60.f / kTwoPi;
    set_public_float("n_Wheel", rpm);

    for (int axle = 0; axle < 4; ++axle) {
        for (const char* side : {"L", "R"}) {
            set_public_float("Wheel_RotationSpeed_" + std::to_string(axle) + "_" + side, rpm);
            const std::string rot_name = "Wheel_Rotation_" + std::to_string(axle) + "_" + side;
            if (const auto idx = vars_.float_index(rot_name)) {
                float& rot = public_floats_.at(static_cast<std::size_t>(*idx));
                rot += omega * dt_sec;
                if (rot > kTwoPi || rot < -kTwoPi) rot = std::fmod(rot, kTwoPi);
            }
        }
    }
    update_steering(dt_sec);
    if (!external_chassis_) update_suspension(dt_sec);
}

void VehicleRuntime::tick_impl(float dt_sec) {
    if (dt_sec <= 0.f) return;
    // OMSI Calculate feeds scripts a bounded Timegap; uncapped frame dt breaks
    // antrieb integration and Matrix GetTime windows (Waittime/Changetime).
    constexpr float kMaxTimegap = 0.05f;
    dt_sec = std::min(dt_sec, kMaxTimegap);
    sim_time_ += dt_sec;

    // Starter motor: crank toward idle RPM while anlasser is held (engine_1620 idle ~600).
    if (public_floats_.size() > 0) {
        const auto anlasser_idx = vars_.float_index("cp_taster_anlasser");
        const auto engine_on_idx = vars_.float_index("engine_on");
        const auto engine_n_idx = vars_.float_index("engine_n");
        if (anlasser_idx && engine_on_idx && engine_n_idx) {
            const float anlasser = public_floats_.at(static_cast<std::size_t>(*anlasser_idx));
            const float engine_on = public_floats_.at(static_cast<std::size_t>(*engine_on_idx));
            if (anlasser > 0.5f && engine_on > 0.5f) {
                float& n = public_floats_.at(static_cast<std::size_t>(*engine_n_idx));
                if (n < 600.f) n += 500.f * dt_sec;
            }
        }
    }

    // Publish pedals + previous Velocity/n_Wheel, then OSC writes M_Wheel / brakes.
    set_public_float("Throttle", throttle_);
    set_public_float("Brake", brake_);
    set_public_float("Velocity", velocity_);
    set_public_float("Velocity_Ground", velocity_);
    osc_.set_ai_mode(ai_mode_);
    push_system_state(dt_sec);
    osc_.run_frame(dt_sec);
    // Allison may leave gangwahl in D while aktugang stays 0; antrieb then zeros M_Wheel.
    // Seed 1st gear only when already in Drive — never force D from throttle (N must stay open).
    {
        const float gw = public_float("antrieb_getr_gangwahl", 1.f);
        const float ak = public_float("antrieb_getr_aktugang", 0.f);
        if (public_float("engine_on", 0.f) > 0.5f && gw > 1.5f && std::fabs(ak) < 0.1f &&
            std::fabs(velocity_) < 5.f) {
            set_public_float("antrieb_getr_aktugang", 1.f);
        }
    }
    integrate_chassis(dt_sec);
    sound_.update(dt_sec);
}

void VehicleRuntime::tick(float dt_sec) {
    std::lock_guard lock(sim_mutex_);
    tick_impl(dt_sec);
}

void VehicleRuntime::start_async_sim() {
    stop_async_sim();
    sim_stop_ = false;
    async_sim_ = true;
    sim_thread_ = std::thread([this] { sim_thread_main(); });
}

void VehicleRuntime::stop_async_sim() {
    if (!async_sim_ && !sim_thread_.joinable()) return;
    sim_stop_ = true;
    sim_cv_.notify_all();
    if (sim_thread_.joinable()) sim_thread_.join();
    async_sim_ = false;
    sim_stop_ = false;
    {
        std::lock_guard lock(sim_mutex_);
        pending_dt_.clear();
    }
    sim_busy_ = false;
}

void VehicleRuntime::post_tick(float dt_sec) {
    if (!async_sim_ || dt_sec <= 0.f) return;
    {
        std::lock_guard lock(sim_mutex_);
        pending_dt_.push_back(dt_sec);
        // Avoid unbounded lag if sim falls behind render.
        constexpr std::size_t kMaxQueuedTicks = 4;
        while (pending_dt_.size() > kMaxQueuedTicks) pending_dt_.pop_front();
    }
    sim_cv_.notify_one();
}

void VehicleRuntime::sync_sim() {
    if (!async_sim_) return;
    std::unique_lock lock(sim_mutex_);
    sim_idle_cv_.wait(lock, [this] { return pending_dt_.empty() && !sim_busy_.load(); });
}

void VehicleRuntime::sim_thread_main() {
    while (!sim_stop_) {
        float dt = 0.f;
        {
            std::unique_lock lock(sim_mutex_);
            sim_cv_.wait(lock, [this] { return sim_stop_ || !pending_dt_.empty(); });
            if (sim_stop_) break;
            dt = pending_dt_.front();
            pending_dt_.pop_front();
            sim_busy_ = true;
            tick_impl(dt);
            sim_busy_ = false;
        }
        sim_idle_cv_.notify_all();
    }
}

} // namespace omsi::script
