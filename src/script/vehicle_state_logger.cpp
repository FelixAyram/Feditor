#include "omsi/script/vehicle_state_logger.hpp"

#include "omsi/logger.hpp"
#include "omsi/sound/sound_runtime.hpp"

#include <cmath>
#include <sstream>

namespace omsi::script {

std::string VehicleStateLogger::fmt(float v) {
    std::ostringstream os;
    os.precision(3);
    os << std::fixed << v;
    return os.str();
}

void VehicleStateLogger::maybe_log(const bus::BusDocument& bus, const VehicleRuntime& rt, float sim_time_sec) {
    if (sim_time_sec - last_log_time_ < interval_sec_) return;
    log_now(bus, rt, sim_time_sec);
    last_log_time_ = sim_time_sec;
}

void VehicleStateLogger::log_now(const bus::BusDocument& bus, const VehicleRuntime& rt, float sim_time_sec) {
    std::ostringstream os;
    os << "OMSI state t=" << fmt(sim_time_sec) << "s";
    os << " | V=" << fmt(rt.public_float("Velocity")) << " km/h";
    os << " | engine_on=" << fmt(rt.public_float("engine_on"));
    os << " inj=" << fmt(rt.public_float("engine_injection_on"));
    os << " starter=" << fmt(rt.public_float("cp_taster_anlasser"));
    os << " antirep=" << fmt(rt.public_float("engine_starter_antirepeat"));
    os << " | engine_n=" << fmt(rt.public_float("engine_n")) << " rpm";
    os << " idle_ctrl=" << fmt(rt.public_float("engine_idle_control"));
    os << " | fuel=" << fmt(rt.public_float("engine_tank_content")) << "L";
    os << " | gear=" << fmt(rt.public_float("antrieb_getr_gang"));
    os << " akt=" << fmt(rt.public_float("antrieb_getr_aktugang"));
    os << " gangwahl=" << fmt(rt.public_float("antrieb_getr_gangwahl"));
    os << " vorwahl=" << fmt(rt.public_float("antrieb_getr_gangvorwahl"));
    os << " | T=" << fmt(rt.public_float("cp_Temperatur")) << "C";
    os << " | wind=" << fmt(rt.public_float("cp_Wind_Velocity"));
    os << " | elec=" << fmt(rt.public_float("elec_busbar_main"));
    os << " sw=" << fmt(rt.public_float("elec_busbar_main_sw"));
    os << " batt=" << fmt(rt.public_float("elec_V_battery"));
    os << " | door0=" << fmt(rt.public_float("door_0")) << " tgt=" << fmt(rt.public_float("doorTarget_0"));
    os << " | door1=" << fmt(rt.public_float("door_1"));
    os << " | wheel0R=" << fmt(rt.public_float("Wheel_Rotation_0_R"));
    os << " steerL=" << fmt(rt.public_float("Axle_Steering_0_L"));
    os << " invR=" << fmt(rt.inv_lenkradius());
    os << " body_z=" << fmt(rt.body_heave_m());
    os << " pitch=" << fmt(rt.body_pitch_rad());
    os << " roll=" << fmt(rt.body_roll_rad());
    os << " suspF=" << fmt(rt.public_float("Axle_Suspension_0_R"));
    os << " suspR=" << fmt(rt.public_float("Axle_Suspension_1_R"));
    os << " sprF=" << fmt(rt.public_float("Axle_Springfactor_0_R"));
    os << " sprR=" << fmt(rt.public_float("Axle_Springfactor_1_R"));
    os << " | mass=" << fmt(bus.mass_t) << "t";
    os << " | bbox=" << fmt(bus.bbox_half_x) << "," << fmt(bus.bbox_half_y) << "," << fmt(bus.bbox_half_z);
    os << " | collision_E=" << fmt(rt.public_float("collision_energy"));
    os << " | sounds=" << rt.sound().definition_count();
    os << " | osc_macros=" << rt.osc().stats().macro_count;
    os << " | triggers=" << rt.osc().stats().trigger_count;
    omsi::log::info(os.str());
}

} // namespace omsi::script
