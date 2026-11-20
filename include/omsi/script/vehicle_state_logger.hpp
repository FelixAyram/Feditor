#pragma once

#include "omsi/bus_types.hpp"
#include "omsi/script/vehicle_runtime.hpp"

#include <string>

namespace omsi::script {

// Periodic OMSI-style vehicle state dump (engine, climate, doors, wheels, sound).
class VehicleStateLogger {
public:
    void set_interval(float seconds) { interval_sec_ = seconds; }
    void maybe_log(const bus::BusDocument& bus, const VehicleRuntime& rt, float sim_time_sec);
    void log_now(const bus::BusDocument& bus, const VehicleRuntime& rt, float sim_time_sec);

private:
    float interval_sec_ = 1.f;
    float last_log_time_ = -1e9f;

    static std::string fmt(float v);
};

} // namespace omsi::script
