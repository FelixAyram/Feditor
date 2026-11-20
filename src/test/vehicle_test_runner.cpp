#include "omsi/test/vehicle_test_runner.hpp"

#include "omsi/bus.hpp"
#include "omsi/logger.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/script/vehicle_runtime.hpp"
#include "omsi/script/vehicle_state_logger.hpp"
#include "omsi/vehicle/bus_body_physics.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <chrono>
#include <memory>
#include <thread>
#include <sstream>

namespace omsi::test {
namespace {

constexpr float kTick = VehicleTestRunner::kTickSec;

// "body" script command: the editor's rigid body on flat ground (z = 0), coupled like mod_omsi_bus.
std::unique_ptr<vehicle::BusBodyPhysics> g_body;
std::vector<vehicle::Obb> g_walls; // "wall" command: fixed obstacles for the body

void step_body(script::VehicleRuntime& rt, float dt) {
    if (!g_body) return;
    const auto& cf = rt.chassis_forces();
    vehicle::BusBodyPhysics::Inputs in;
    in.drive_force_n = cf.drive_n;
    in.brake_force_n = cf.brake_n;
    in.rolling_force_n = cf.rolling_n;
    in.inv_turn_radius = rt.inv_lenkradius();
    g_body->step(dt, in, [](float, float, float) { return 0.f; });
    for (const auto& im : g_body->collide(g_walls, dt))
        omsi::log::info("vehicle-test collision obj=" + std::to_string(im.obstacle_id) + " speed=" +
                        std::to_string(im.speed * 3.6f) + "kmh energy=" + std::to_string(im.energy));
    float comp[8]{};
    const auto& ws = g_body->wheels();
    for (std::size_t i = 0; i < ws.size() && i < 8; ++i) comp[i] = ws[i].compression;
    rt.set_external_motion(g_body->speed_ms() * 3.6f, g_body->accel_long(), comp,
                           static_cast<int>((std::min)(ws.size(), std::size_t{8})));
}

void tick_n(script::VehicleRuntime& rt, script::VehicleStateLogger& logger, const bus::BusDocument& bus,
            int frames, float throttle, float brake, const std::vector<std::string>& triggers = {}) {
    for (const auto& t : triggers) rt.apply_trigger(t);
    for (int i = 0; i < frames; ++i) {
        rt.set_driver_input(throttle, brake);
        rt.tick(kTick);
        step_body(rt, kTick);
        logger.maybe_log(bus, rt, rt.sim_time());
    }
}

std::string trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    auto out = std::string(s.substr(a, b - a + 1));
    if (!out.empty() && static_cast<unsigned char>(out[0]) == 0xEFu && out.size() >= 3 &&
        static_cast<unsigned char>(out[1]) == 0xBBu && static_cast<unsigned char>(out[2]) == 0xBFu)
        out.erase(0, 3);
    return out;
}

bool parse_script_line(const std::string& line, script::VehicleRuntime& rt, script::VehicleStateLogger& logger,
                       const bus::BusDocument& bus, float& throttle, float& brake, std::string& error) {
    if (line.empty() || line.front() == '#') return true;

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd == "trigger") {
        std::string name;
        iss >> name;
        if (name.empty()) {
            error = "trigger missing name";
            return false;
        }
        rt.apply_trigger(name);
        return true;
    }
    if (cmd == "wait") {
        float sec = 0.f;
        iss >> sec;
        const int frames = std::max(1, static_cast<int>(sec / kTick));
        tick_n(rt, logger, bus, frames, throttle, brake);
        return true;
    }
    if (cmd == "sleep") { // sleep <sec>: like wait, but in real time (audio device keeps running)
        float sec = 0.f;
        iss >> sec;
        const int frames = std::max(1, static_cast<int>(sec / kTick));
        for (int i = 0; i < frames; ++i) {
            tick_n(rt, logger, bus, 1, throttle, brake);
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(kTick * 1e6f)));
        }
        return true;
    }
    if (cmd == "throttle") {
        iss >> throttle;
        return true;
    }
    if (cmd == "brake") {
        iss >> brake;
        return true;
    }
    if (cmd == "ai_mode") {
        std::string onoff;
        iss >> onoff;
        const bool ai = (onoff == "1" || onoff == "on" || onoff == "true");
        rt.set_ai_mode(ai);
        return true;
    }
    if (cmd == "set") {
        std::string var;
        float value = 0.f;
        iss >> var >> value;
        if (var.empty()) {
            error = "set missing var";
            return false;
        }
        rt.set_public_float(var, value);
        return true;
    }
    if (cmd == "print") { // print <var> [var...]: values into the log (diagnostics, no assertion)
        std::string var, out = "vehicle-test print t=" + std::to_string(rt.sim_time());
        while (iss >> var) out += " " + var + "=" + std::to_string(rt.public_float(var));
        omsi::log::info(out);
        return true;
    }
    if (cmd == "body") { // body: drive the rigid body (flat ground) instead of the runtime's 1-D chassis
        g_body = std::make_unique<vehicle::BusBodyPhysics>();
        g_body->init(bus);
        g_body->place(0.f, 0.f, 0.f, [](float, float, float) { return 0.f; });
        rt.set_external_chassis(true);
        return true;
    }
    if (cmd == "wall") { // wall <x> <y> <w> <l> <h> <heading_deg>: a fixed box on the ground
        float v[6]{0, 0, 1, 1, 3, 0};
        for (auto& x : v) iss >> x;
        const float bb[6] = {v[2], v[3], v[4], 0.f, 0.f, v[4] * 0.5f};
        auto o = vehicle::Obb::from_box(bb, vehicle::Vec3d{v[0], v[1], 0.0}, v[5]);
        o.id = static_cast<std::int64_t>(1000 + g_walls.size());
        g_walls.push_back(o);
        return true;
    }
    if (cmd == "steer") { // steer left|right|none
        std::string d;
        iss >> d;
        rt.set_steering_keys(d == "left", d == "right", d != "left" && d != "right");
        return true;
    }
    if (cmd == "bodyprint") {
        if (!g_body) return true;
        const auto& b = *g_body;
        std::string out = "vehicle-test body t=" + std::to_string(rt.sim_time()) + " x=" + std::to_string(b.x()) +
                          " y=" + std::to_string(b.y()) + " z=" + std::to_string(b.z()) +
                          " hdg=" + std::to_string(b.heading_deg()) + " pitch=" + std::to_string(b.pitch_deg()) +
                          " roll=" + std::to_string(b.roll_deg()) + " u=" + std::to_string(b.speed_ms() * 3.6f) +
                          "kmh v=" + std::to_string(b.lateral_ms()) + " r=" + std::to_string(b.yaw_rate()) +
                          " invR=" + std::to_string(rt.inv_lenkradius()) + " drive=" +
                          std::to_string(rt.chassis_forces().drive_n) + " brake=" + std::to_string(rt.chassis_forces().brake_n);
        for (const auto& w : b.wheels()) out += " c" + std::to_string(w.axle) + (w.side ? "R=" : "L=") + std::to_string(w.compression).substr(0, 5);
        omsi::log::info(out);
        return true;
    }
    if (cmd == "log") {
        logger.log_now(bus, rt, rt.sim_time());
        return true;
    }
    if (cmd == "assert") {
        std::string var;
        float rhs = 0.f;
        std::string op;
        iss >> var >> rhs >> op;
        const float lhs = rt.public_float(var);
        bool ok = false;
        if (op == ">" || op.empty()) ok = lhs > rhs;
        else if (op == ">=") ok = lhs >= rhs;
        else if (op == "<") ok = lhs < rhs;
        else if (op == "<=") ok = lhs <= rhs;
        else if (op == "=" || op == "==") ok = std::fabs(lhs - rhs) < 1e-3f;
        if (!ok) {
            error = "assert failed: " + var + "=" + std::to_string(lhs) + " op " + op + " " + std::to_string(rhs);
            return false;
        }
        return true;
    }
    error = "unknown script command: " + cmd;
    return false;
}

} // namespace

int VehicleTestRunner::run(const std::filesystem::path& bus_path, const std::filesystem::path& omsi_root,
                            const std::string& scenario) {
    omsi::log::info("vehicle-test: scenario='" + scenario + "' bus=" + bus_path.string());

    if (scenario == "all") {
        const auto results = run_all(bus_path, omsi_root);
        int failed = 0;
        for (const auto& r : results) {
            if (r.passed)
                omsi::log::info("vehicle-test PASS: " + r.name + " - " + r.message);
            else {
                omsi::log::error("vehicle-test FAIL: " + r.name + " - " + r.message);
                ++failed;
            }
        }
        omsi::log::info("vehicle-test: " + std::to_string(results.size() - failed) + "/" +
                        std::to_string(results.size()) + " passed");
        return failed > 0 ? 1 : 0;
    }

    VehicleTestResult r{};
    if (scenario == "load")
        r = test_load(bus_path, omsi_root);
    else if (scenario == "startup")
        r = test_startup(bus_path, omsi_root);
    else if (scenario == "accel" || scenario == "accelerate")
        r = test_accelerate(bus_path, omsi_root);
    else if (scenario == "doors")
        r = test_doors(bus_path, omsi_root);
    else if (scenario == "physics")
        r = test_physics(bus_path, omsi_root);
    else {
        omsi::log::error("vehicle-test: unknown scenario '" + scenario +
                         "'. Use: load, startup, accel, doors, physics, all");
        return 2;
    }

    if (r.passed)
        omsi::log::info("vehicle-test PASS: " + r.name + " - " + r.message);
    else
        omsi::log::error("vehicle-test FAIL: " + r.name + " - " + r.message);
    return r.passed ? 0 : 1;
}

int VehicleTestRunner::run_script_file(const std::filesystem::path& bus_path,
                                        const std::filesystem::path& omsi_root,
                                        const std::filesystem::path& script_path) {
    const auto bus_doc = bus::load_bus(bus_path);
    if (!bus_doc) {
        omsi::log::error("vehicle-test-script: bus load failed");
        return 2;
    }

    script::VehicleRuntime rt{};
    if (!rt.load(*bus_doc, omsi_root)) {
        omsi::log::error("vehicle-test-script: runtime load failed");
        return 2;
    }

    script::VehicleStateLogger logger{};
    logger.set_interval(0.5f);

    std::ifstream in(script_path);
    if (!in) {
        omsi::log::error("vehicle-test-script: cannot open " + script_path.string());
        return 2;
    }

    float throttle = 0.f;
    float brake = 0.f;
    std::string line;
    while (std::getline(in, line)) {
        const auto t = trim(line);
        std::string err;
        if (!parse_script_line(t, rt, logger, *bus_doc, throttle, brake, err)) {
            omsi::log::error("vehicle-test-script: " + err + " at: " + t);
            return 1;
        }
    }

    logger.log_now(*bus_doc, rt, rt.sim_time());
    omsi::log::info("vehicle-test-script: completed ok sim_t=" + std::to_string(rt.sim_time()));
    return 0;
}

std::vector<VehicleTestResult> VehicleTestRunner::run_all(const std::filesystem::path& bus_path,
                                                            const std::filesystem::path& omsi_root) {
    return {test_load(bus_path, omsi_root),   test_physics(bus_path, omsi_root), test_startup(bus_path, omsi_root),
            test_accelerate(bus_path, omsi_root), test_doors(bus_path, omsi_root)};
}

VehicleTestResult VehicleTestRunner::test_load(const std::filesystem::path& bus_path,
                                                const std::filesystem::path& omsi_root) {
    VehicleTestResult r{.name = "load"};
    const auto bus_doc = bus::load_bus(bus_path);
    if (!bus_doc) {
        r.message = "bus parse failed";
        return r;
    }

    const auto cfg_path = bus::resolve_model_cfg_path(*bus_doc);
    const auto cfg = model_cfg::load_model_cfg(cfg_path);
    if (!cfg) {
        r.message = "model.cfg parse failed: " + cfg_path.string();
        return r;
    }

    script::VehicleRuntime rt{};
    if (!rt.load(*bus_doc, omsi_root)) {
        r.message = "vehicle runtime load failed";
        return r;
    }

    const auto* lod = model_cfg::primary_lod(*cfg);
    const std::size_t meshes = lod ? lod->meshes.size() : 0;
    r.passed = true;
    r.message = "bus ok model.cfg meshes=" + std::to_string(meshes) + " vars=" +
                std::to_string(rt.vars().float_var_count()) + " osc_macros=" +
                std::to_string(rt.osc().stats().macro_count) + " sounds=" +
                std::to_string(rt.sound().definition_count());
    return r;
}

VehicleTestResult VehicleTestRunner::test_physics(const std::filesystem::path& bus_path,
                                                   const std::filesystem::path& omsi_root) {
    VehicleTestResult r{.name = "physics"};
    const auto bus_doc = bus::load_bus(bus_path);
    if (!bus_doc) {
        r.message = "bus load failed";
        return r;
    }

    script::VehicleRuntime rt{};
    rt.load(*bus_doc, omsi_root);
    script::VehicleStateLogger logger{};

    // 1) Throttle in N + parking must not move (toy throttle→vel used to).
    // Also: engine on, park off, throttle in N/aktugang=0 must still not invent Drive.
    tick_n(rt, logger, *bus_doc, 60, 1.f, 0.f);
    const float v_park = rt.public_float("Velocity");
    const float fest = rt.public_float("bremse_feststell");
    const float gang = rt.public_float("antrieb_getr_gangwahl");
    rt.set_public_float("bremse_feststell", 0.f);
    rt.set_public_float("bremse_feststell_sw", 0.f);
    rt.set_public_float("engine_on", 1.f);
    rt.set_public_float("antrieb_getr_gangwahl", 1.f);
    rt.set_public_float("antrieb_getr_aktugang", 0.f);
    for (int i = 0; i < 90; ++i) {
        rt.set_public_float("antrieb_getr_gangwahl", 1.f);
        rt.set_public_float("antrieb_getr_aktugang", 0.f);
        rt.set_driver_input(1.f, 0.f);
        rt.tick(kTick);
    }
    const float v_N = rt.public_float("Velocity");
    const float mw_N = rt.public_float("M_Wheel");
    rt.set_driver_input(0.f, 0.f);
    rt.debug_set_velocity_kmh(0.f);

    // 2) Seed forward speed then brake — Velocity/n_Wheel must stay >= 0 (no reverse wheels).
    rt.set_public_float("bremse_feststell", 0.f);
    rt.set_public_float("bremse_feststell_sw", 0.f);
    rt.set_public_float("Brakeforce", 0.f);
    for (int axle = 0; axle < 4; ++axle) {
        for (const char* side : {"L", "R"}) {
            rt.set_public_float("Axle_Brakeforce_" + std::to_string(axle) + "_" + side, 0.f);
        }
    }
    rt.debug_set_velocity_kmh(36.f); // 10 m/s
    const float v_go = rt.public_float("Velocity");
    tick_n(rt, logger, *bus_doc, 120, 0.f, 1.f);
    const float v_stop = rt.public_float("Velocity");
    const float rpm_stop = rt.public_float("n_Wheel");

    // 3) Unit129 steering KeyCommand → Axle_Steering + body settles under gravity.
    rt.set_steering_keys(true, false, false);
    for (int i = 0; i < 90; ++i) rt.tick(kTick);
    const float steer_L = rt.public_float("Axle_Steering_0_L");
    const float inv_R = rt.inv_lenkradius();
    rt.set_steering_keys(false, false, true);
    for (int i = 0; i < 90; ++i) rt.tick(kTick);
    const float steer_center = std::fabs(rt.public_float("Axle_Steering_0_L"));
    // Zero-throttle coast vs drive: force drive vars each tick (OSC may overwrite gangwahl).
    rt.set_public_float("bremse_feststell", 0.f);
    rt.set_public_float("bremse_feststell_sw", 0.f);
    rt.set_public_float("Brakeforce", 0.f);
    for (int axle = 0; axle < 4; ++axle) {
        for (const char* side : {"L", "R"}) {
            rt.set_public_float("Axle_Brakeforce_" + std::to_string(axle) + "_" + side, 0.f);
        }
    }
    for (int i = 0; i < 90; ++i) {
        rt.set_public_float("engine_on", 1.f);
        // antrieb copies gangvorwahl → gangwahl after antrieb_gangwahlzeit; keep both in D.
        rt.set_public_float("antrieb_getr_gangvorwahl", 2.f);
        rt.set_public_float("antrieb_getr_gangwahl", 2.f);
        rt.set_public_float("antrieb_getr_aktugang", 1.f);
        rt.set_public_float("bremse_feststell", 0.f);
        rt.set_driver_input(1.f, 0.f);
        rt.tick(kTick);
    }
    const float v_gas = rt.public_float("Velocity");
    for (int i = 0; i < 120; ++i) {
        rt.set_public_float("engine_on", 1.f);
        rt.set_public_float("antrieb_getr_gangvorwahl", 2.f);
        rt.set_public_float("antrieb_getr_gangwahl", 2.f);
        rt.set_public_float("antrieb_getr_aktugang", 1.f);
        rt.set_public_float("bremse_feststell", 0.f);
        rt.set_driver_input(0.f, 0.f);
        rt.tick(kTick);
    }
    const float v_coast = rt.public_float("Velocity");
    const float mw_coast = rt.public_float("M_Wheel");

    const bool gates_ok = std::fabs(v_park) < 0.25f && fest > 0.5f && std::fabs(gang - 1.f) < 0.2f;
    const bool neutral_ok = std::fabs(v_N) < 0.5f && std::fabs(mw_N) < 0.05f;
    const bool drove = v_go > 1.f;
    const bool no_reverse = v_stop >= -0.05f && rpm_stop >= -1.f && v_stop < v_go;
    const bool steering_ok = inv_R < -0.05f && steer_L < -0.1f && steer_center < 0.05f;
    const bool coasts_ok = v_gas > 1.f && v_coast <= v_gas + 0.25f && std::fabs(mw_coast) < 1.f;
    r.passed = bus_doc->mass_t > 0.f && bus_doc->rollwiderstand_N > 0.f && !bus_doc->axles.empty() && gates_ok &&
               neutral_ok && drove && no_reverse && steering_ok && coasts_ok;
    r.message = "mass=" + std::to_string(bus_doc->mass_t) + "t roll=" + std::to_string(bus_doc->rollwiderstand_N) +
                "N axles=" + std::to_string(bus_doc->axles.size()) + " v_park=" + std::to_string(v_park) +
                " fest=" + std::to_string(fest) + " v_N=" + std::to_string(v_N) + " MwN=" + std::to_string(mw_N) +
                " v_go=" + std::to_string(v_go) + " v_brake=" + std::to_string(v_stop) + " n_Wheel=" +
                std::to_string(rpm_stop) + " steerL=" + std::to_string(steer_L) + " invR=" +
                std::to_string(inv_R) + " steer0=" + std::to_string(steer_center) + " v_gas=" +
                std::to_string(v_gas) + " v_coast=" + std::to_string(v_coast) + " Mw0=" + std::to_string(mw_coast);
    return r;
}

VehicleTestResult VehicleTestRunner::test_startup(const std::filesystem::path& bus_path,
                                                   const std::filesystem::path& omsi_root) {
    VehicleTestResult r{.name = "startup"};
    const auto bus_doc = bus::load_bus(bus_path);
    if (!bus_doc) {
        r.message = "bus load failed";
        return r;
    }

    script::VehicleRuntime rt{};
    rt.load(*bus_doc, omsi_root);
    script::VehicleStateLogger logger{};

    if (!rt.osc().has_trigger("kw_m_enginestart")) {
        r.message = "trigger kw_m_enginestart not in scripts (skipped)";
        r.passed = true;
        return r;
    }

    // Let Elec_Frame publish busbar from switch (peek-store chain) before starter.
    tick_n(rt, logger, *bus_doc, 5, 0.f, 0.f);
    if (rt.public_float("elec_busbar_main") < 0.5f &&
        rt.osc().has_trigger("cp_batterietrennschalter_toggle")) {
        tick_n(rt, logger, *bus_doc, 1, 0.f, 0.f, {"cp_batterietrennschalter_toggle"});
        tick_n(rt, logger, *bus_doc, 5, 0.f, 0.f);
    }
    // Neutral is required by engine.osc; ensure gangwahl==1.
    if (std::fabs(rt.public_float("antrieb_getr_gangwahl") - 1.f) > 0.1f &&
        rt.osc().has_trigger("automatic_N"))
        tick_n(rt, logger, *bus_doc, 1, 0.f, 0.f, {"automatic_N"});

    rt.apply_trigger("kw_m_enginestart");
    const float engine_on_after_trigger = rt.public_float("engine_on");
    // Hold starter across frames (cp_taster_anlasser) while cranking.
    tick_n(rt, logger, *bus_doc, 120, 0.f, 0.f, {"kw_m_enginestart"});
    logger.log_now(*bus_doc, rt, rt.sim_time());

    const float engine_on = rt.public_float("engine_on");
    const float engine_n = rt.public_float("engine_n");
    const float injection = rt.public_float("engine_injection_on");
    const float elec = rt.public_float("elec_busbar_main");
    r.passed = engine_on > 0.5f && engine_n > 200.f && injection > 0.5f;
    r.message = "engine_on=" + std::to_string(engine_on) + " engine_n=" + std::to_string(engine_n) +
                " injection=" + std::to_string(injection) + " elec=" + std::to_string(elec) +
                " after_trigger=" + std::to_string(engine_on_after_trigger);
    return r;
}

VehicleTestResult VehicleTestRunner::test_accelerate(const std::filesystem::path& bus_path,
                                                      const std::filesystem::path& omsi_root) {
    VehicleTestResult r{.name = "accelerate"};
    const auto bus_doc = bus::load_bus(bus_path);
    if (!bus_doc) {
        r.message = "bus load failed";
        return r;
    }

    script::VehicleRuntime rt{};
    rt.load(*bus_doc, omsi_root);
    script::VehicleStateLogger logger{};

    tick_n(rt, logger, *bus_doc, 5, 0.f, 0.f);
    if (rt.public_float("elec_busbar_main") < 0.5f &&
        rt.osc().has_trigger("cp_batterietrennschalter_toggle"))
        tick_n(rt, logger, *bus_doc, 1, 0.f, 0.f, {"cp_batterietrennschalter_toggle"});
    if (rt.osc().has_trigger("kw_m_enginestart"))
        tick_n(rt, logger, *bus_doc, 90, 0.f, 0.f, {"kw_m_enginestart"});
    if (rt.osc().has_trigger("automatic_D")) tick_n(rt, logger, *bus_doc, 1, 0.f, 0.f, {"automatic_D"});
    // Allison waits antrieb_gangwahlzeit before vorwahl→gangwahl.
    tick_n(rt, logger, *bus_doc, 90, 0.f, 0.f);
    if (rt.osc().has_trigger("parking_brake_toggle") && rt.public_float("bremse_feststell") > 0.5f)
        tick_n(rt, logger, *bus_doc, 1, 0.f, 0.f, {"parking_brake_toggle"});
    else {
        rt.set_public_float("bremse_feststell", 0.f);
        rt.set_public_float("bremse_feststell_sw", 0.f);
    }
    tick_n(rt, logger, *bus_doc, 240, 1.f, 0.f);
    logger.log_now(*bus_doc, rt, rt.sim_time());

    const float v = rt.public_float("Velocity");
    const float rpm = rt.public_float("engine_n");
    const float gear = rt.public_float("antrieb_getr_gang");
    const float fest = rt.public_float("bremse_feststell");
    const float mw = rt.public_float("M_Wheel");
    const float gangw = rt.public_float("antrieb_getr_gangwahl");
    r.passed = v > 1.5f && fest < 0.5f;
    r.message = "Velocity=" + std::to_string(v) + " km/h engine_n=" + std::to_string(rpm) +
                " gear=" + std::to_string(gear) + " gangwahl=" + std::to_string(gangw) +
                " M_Wheel=" + std::to_string(mw) + " feststell=" + std::to_string(fest);
    return r;
}

VehicleTestResult VehicleTestRunner::test_doors(const std::filesystem::path& bus_path,
                                                 const std::filesystem::path& omsi_root) {
    VehicleTestResult r{.name = "doors"};
    const auto bus_doc = bus::load_bus(bus_path);
    if (!bus_doc) {
        r.message = "bus load failed";
        return r;
    }

    script::VehicleRuntime rt{};
    rt.load(*bus_doc, omsi_root);
    script::VehicleStateLogger logger{};

    const float door_before = rt.public_float("door_0");
    if (!rt.osc().has_trigger("bus_doorfront0")) {
        r.message = "trigger bus_doorfront0 missing";
        return r;
    }

    // Doors require elec_busbar_avail (Elec_Frame after battery/switch).
    tick_n(rt, logger, *bus_doc, 5, 0.f, 0.f);
    if (rt.public_float("elec_busbar_avail") < 0.5f &&
        rt.osc().has_trigger("cp_batterietrennschalter_toggle")) {
        tick_n(rt, logger, *bus_doc, 1, 0.f, 0.f, {"cp_batterietrennschalter_toggle"});
        tick_n(rt, logger, *bus_doc, 5, 0.f, 0.f);
    }
    tick_n(rt, logger, *bus_doc, 1, 0.f, 0.f, {"bus_doorfront0"});
    tick_n(rt, logger, *bus_doc, 180, 0.f, 0.f);
    logger.log_now(*bus_doc, rt, rt.sim_time());

    const float door_after = rt.public_float("door_0");
    const float target = rt.public_float("doorTarget_0");
    const float avail = rt.public_float("elec_busbar_avail");
    r.passed = door_after > door_before + 0.01f || target > 0.5f;
    r.message = "door_0 " + std::to_string(door_before) + " -> " + std::to_string(door_after) +
                " target=" + std::to_string(target) + " avail=" + std::to_string(avail);
    return r;
}

} // namespace omsi::test
