// Stand-ins for the openOMSI vehicle DLL when it is not built (no cargo): oo_init reports failure, so
// the editor bus uses the C++ vehicle runtime and never calls the rest.
#include "omsi/vehicle/oo_vehicle.h"

extern "C" {
int32_t oo_init(const char*, OoLogFn) { return 0; }
OoBus* oo_spawn(const char*, double, double, double, double) { return nullptr; }
void oo_despawn(OoBus*) {}
void oo_set_ground(OoBus*, OoGroundFn, void*) {}
void oo_set_obstacles(OoBus*, const OoBox*, size_t) {}
void oo_key(OoBus*, const char*, int32_t) {}
void oo_autostart(OoBus*) {}
void oo_tick(OoBus*, float, float, float, float, float, float, float) {}
int32_t oo_body_matrix(OoBus*, float*) { return 0; }
int32_t oo_pose(OoBus*, double*) { return 0; }
int32_t oo_var_slot(OoBus*, const char*) { return -1; }
float oo_var_at(OoBus*, int32_t) { return 0.f; }
float oo_var(OoBus*, const char*, float fallback) { return fallback; }
int32_t oo_set_var(OoBus*, const char*, float) { return 0; }
int32_t oo_status(OoBus*, OoStatus*) { return 0; }
int32_t oo_mesh_count(OoBus*) { return 0; }
int32_t oo_section_count(OoBus*) { return 0; }
int32_t oo_section_info(OoBus*, int32_t, char*, size_t, int32_t*, int32_t*) { return 0; }
OoTraffic* oo_traffic_create(const char*, int32_t, float, int32_t) { return nullptr; }
void oo_traffic_destroy(OoTraffic*) {}
void oo_traffic_set_target(OoTraffic*, int32_t, float) {}
void oo_traffic_set_ground(OoTraffic*, OoGroundFn, void*) {}
int32_t oo_traffic_light_program(OoTraffic*, int64_t, const char*) { return -1; }
int32_t oo_traffic_add_lanes(OoTraffic*, const OoLaneDesc*, size_t, const float*, const float*) { return 0; }
void oo_traffic_tick(OoTraffic*, float, double, double, double, double, double, const double*, float) {}
int32_t oo_traffic_lane_count(OoTraffic*) { return 0; }
int32_t oo_traffic_car_count(OoTraffic*) { return 0; }
int32_t oo_traffic_car(OoTraffic*, int32_t, uint64_t*, int32_t*) { return 0; }
size_t oo_traffic_car_part_file(OoTraffic*, int32_t, int32_t, char*, size_t) { return 0; }
size_t oo_traffic_car_mesh_file(OoTraffic*, int32_t, int32_t, char*, size_t) { return 0; }
int32_t oo_traffic_car_mesh_world(OoTraffic*, int32_t, int32_t, float*) { return 0; }
size_t oo_traffic_car_paint(OoTraffic*, int32_t, int32_t, char*, size_t) { return 0; }
size_t oo_traffic_stats(OoTraffic*, char*, size_t) { return 0; }
void oo_traffic_set_bus(OoTraffic*, OoBus*) {}
float oo_traffic_car_var(OoTraffic*, int32_t, const char*, float f) { return f; }
int32_t oo_traffic_remove_car(OoTraffic*, uint64_t) { return 0; }
int32_t oo_teleport(OoBus*, double, double, double, double) { return 0; }
size_t oo_traffic_lamps(OoTraffic*, double, double, double, float*, size_t) { return 0; }
size_t oo_bus_lamps(OoBus*, float*, size_t) { return 0; }
size_t oo_traffic_bodies(OoTraffic*, float*, size_t) { return 0; }
int32_t oo_traffic_light_set(OoTraffic*, int64_t, int32_t, int32_t) { return 0; }
size_t oo_mesh_file(OoBus*, int32_t, char*, size_t) { return 0; }
size_t oo_bus_paint(OoBus*, int32_t, char*, size_t) { return 0; }
size_t oo_bus_skinned(OoBus*, int32_t, float*, size_t) { return 0; }
size_t oo_bus_coronas(OoBus*, float*, size_t) { return 0; }
size_t oo_bus_interior_lights(OoBus*, float*, size_t) { return 0; }
size_t oo_bus_texttex_string(OoBus*, const char*, char*, size_t) { return 0; }
int32_t oo_mesh_world(OoBus*, int32_t, float*) { return 0; }
int32_t oo_mesh_visible(OoBus*, int32_t) { return 1; }
void oo_set_analog(OoBus*, int32_t, float, float, float) {}
float oo_inv_min_turn_radius(OoBus*) { return 0.1f; }
int32_t oo_audio_probe(float, const char*) { return 0; }
void oo_sound_report(OoBus*) {}
void oo_debug_wheels(OoBus*) {}
}
