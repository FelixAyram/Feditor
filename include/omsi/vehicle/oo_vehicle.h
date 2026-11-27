/* C ABI of ffi/omsi_vehicle_ffi: openOMSI's own vehicle simulation (scripts, rigid body,
 * gearbox, sound, auto-start) driving the editor's bus. openOMSI is used unmodified from
 * third_party/openOMSI (https://github.com/openOMSI-org/openOMSI), GNU AGPL-3.0; see NOTICE.md.
 * World frame: x east, y north, z up (editor metres); headings in degrees clockwise from north. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OoBus OoBus;

typedef void (*OoLogFn)(int32_t level /*0 info, 1 warn, 2 error*/, const char* msg);
typedef int32_t (*OoGroundFn)(void* user, double x, double y, double* out_z);

typedef struct OoBox {
    double cx, cy;         /* centre */
    double half_x, half_y; /* across / along the heading */
    double heading;        /* radians, clockwise from north */
    double z0, z1;         /* height range */
    int64_t id;            /* map object id */
    float pole_mass_t;     /* [crashmode_pole] mass (t), 0 = a solid obstacle */
    float pole_load_kn;    /* [crashmode_pole] break load (kN) */
} OoBox;

typedef struct OoStatus {
    float speed_kmh;
    float engine_rpm;
    int32_t engine_running;
    int32_t power_on;
    int32_t autostart_running;
    int32_t voices;
    int32_t audio_ok;
    float throttle;
    float brake;
    float steering;
} OoStatus;

int32_t oo_init(const char* omsi_root, OoLogFn log_fn);
OoBus* oo_spawn(const char* bus_path, double x, double y, double z, double heading_deg);
void oo_despawn(OoBus* bus);
void oo_set_ground(OoBus* bus, OoGroundFn fn, void* user);
void oo_set_obstacles(OoBus* bus, const OoBox* boxes, size_t n);
void oo_key(OoBus* bus, const char* action, int32_t pressed);
void oo_autostart(OoBus* bus);
void oo_tick(OoBus* bus, float dt, float listener_x, float listener_y, float listener_z, float fwd_x, float fwd_y,
             float fwd_z);
int32_t oo_body_matrix(OoBus* bus, float* out16); /* DirectXMath row-vector matrix, row-major */
int32_t oo_pose(OoBus* bus, double* out6);        /* x y z heading pitch bank */
/* puts the bus at x y z / heading, standing (carried like an object in the editor) */
int32_t oo_teleport(OoBus* bus, double x, double y, double z, double heading_deg);
int32_t oo_var_slot(OoBus* bus, const char* name);
float oo_var_at(OoBus* bus, int32_t slot);
float oo_var(OoBus* bus, const char* name, float fallback);
int32_t oo_set_var(OoBus* bus, const char* name, float value);
int32_t oo_status(OoBus* bus, OoStatus* out);
/* meshes as openOMSI poses them (LOD 0, model.cfg order) */
int32_t oo_mesh_count(OoBus* bus); /* the front section's meshes, then each coupled section's */
int32_t oo_section_count(OoBus* bus);  /* sections coupled behind the bus (articulated, trailers) */
int32_t oo_section_info(OoBus* bus, int32_t s, char* bus_path, size_t cap, int32_t* first_mesh, int32_t* mesh_count);

/* AI road traffic (openOMSI TrafficSim): the editor hands over lanes (its own paths, world frame) */
typedef struct OoTraffic OoTraffic;
typedef struct OoLaneDesc {
    int32_t kind; /* 0 street 1 sidewalk 2 rail 3 air */
    float width;
    float speed_limit_kmh; /* 0 = the default */
    float density;
    int32_t no_cars, rule_bus, rule_trucks, no_overtaking;
    int32_t turn;   /* 0 none, 1 left, 2 right */
    float priority; /* < 0 = the default */
    int32_t source; /* 1 spline, 2 object */
    int32_t tile_x, tile_y;
    int64_t id;
    int32_t path;
    int32_t reversed;
    int64_t light_object; /* crossing whose light program holds the lane's light, -1 none */
    int32_t light_index;
    int32_t n_points;
    /* [rule] trafficdensity per AI group: n_groups (group, value) float pairs from pair group_first of `groups` */
    int32_t n_groups;
    int32_t group_first;
    float offset;             /* spline lane: lateral offset from the spline's middle (.sli path lateral) */
    int32_t crossing_problem; /* object path [crossingproblem] */
} OoLaneDesc;
OoTraffic* oo_traffic_create(const char* map_dir, int32_t target, float spawn_radius, int32_t left_hand);
void oo_traffic_destroy(OoTraffic* t);
void oo_traffic_set_target(OoTraffic* t, int32_t target, float spawn_radius);
void oo_traffic_set_ground(OoTraffic* t, OoGroundFn fn, void* user);
int32_t oo_traffic_light_program(OoTraffic* t, int64_t object_id, const char* sco_path);
int32_t oo_traffic_add_lanes(OoTraffic* t, const OoLaneDesc* lanes, size_t n, const float* pts, const float* groups);
void oo_traffic_tick(OoTraffic* t, float dt, double cx, double cy, double cz, double fx, double fy, const double* player7,
                     float day_hour);
int32_t oo_traffic_lane_count(OoTraffic* t);
int32_t oo_traffic_car_count(OoTraffic* t);
int32_t oo_traffic_car(OoTraffic* t, int32_t i, uint64_t* id, int32_t* mesh_count);
size_t oo_traffic_car_part_file(OoTraffic* t, int32_t i, int32_t k, char* buf, size_t cap);
size_t oo_traffic_car_mesh_file(OoTraffic* t, int32_t i, int32_t k, char* buf, size_t cap);
int32_t oo_traffic_car_mesh_world(OoTraffic* t, int32_t i, int32_t k, float* out16); /* 0 none, 1 shown, 2 hidden */
/* [CTC] paint of mesh k's part of car i: "<default texture, lower case>	<file>
" lines (openOMSI scheme_substitutions) */
size_t oo_traffic_car_paint(OoTraffic* t, int32_t i, int32_t k, char* buf, size_t cap);
size_t oo_traffic_stats(OoTraffic* t, char* buf, size_t cap);
void oo_traffic_set_bus(OoTraffic* t, OoBus* bus); /* player bus + trailers as obstacles; null clears */
float oo_traffic_car_var(OoTraffic* t, int32_t i, const char* name, float fallback);
/* takes the car of id `id` off the road (editor delete); 1 when it was on it */
int32_t oo_traffic_remove_car(OoTraffic* t, uint64_t id);
/* lit [light_enh]/[light_enh_2] lamps, 10 floats each: x y z, dir x y z (0: omni), r g b, size*level */
size_t oo_traffic_lamps(OoTraffic* t, double cx, double cy, double radius, float* out, size_t cap);
size_t oo_bus_lamps(OoBus* bus, float* out, size_t cap);
/* [smoothskin] train mesh i as its bones hold it now: 6 floats a vertex (pos, normal), mesh frame, file order */
size_t oo_bus_skinned(OoBus* bus, int32_t i, float* out, size_t cap);
/* lit [light_enh]/[light_enh_2] coronas of the bus (glows only), 10 floats each as oo_traffic_lamps */
size_t oo_bus_coronas(OoBus* bus, float* out, size_t cap);
/* lit [interiorlight]s of the bus and its sections, 8 floats each: x y z, range, r g b (0..1), level */
size_t oo_bus_interior_lights(OoBus* bus, float* out, size_t cap);
/* every AI car / towed section: x y half_length half_width (4 floats each) */
size_t oo_traffic_bodies(OoTraffic* t, float* out, size_t cap);
/* the phase the editor shows for light `light` of crossing `object_id`: the traffic obeys it */
int32_t oo_traffic_light_set(OoTraffic* t, int64_t object_id, int32_t light, int32_t state);
/* a timetable bus on its trip: entries = (owner id, path index) pairs of the trip's track, stops = (x, y, object id,
   departure s of day) quadruples; bus_path "" = one of the map's AI buses. Returns the car id, 0 when none placed. */
uint64_t oo_traffic_service_bus(OoTraffic* t, const char* bus_path, const int64_t* entries, size_t n_entries,
                                const double* stops, size_t n_stops, const char* line, const char* terminus);
/* the same for a trip without a track (stations only): starts at the lane nearest (sx, sy), shortest way between stations */
size_t oo_traffic_car_debug(OoTraffic* t, uint64_t id, char* buf, size_t cap);
uint64_t oo_traffic_service_bus_stations(OoTraffic* t, const char* bus_path, double sx, double sy, const double* stops,
                                         size_t n_stops, const char* line, const char* terminus);
/* what car `id` does now (one call per car every half second for the editor's car window) */
typedef struct OoCarState {
    int32_t action; /* 0 driving 1 accelerating 2 braking 3 queue 4 red light 5 giving way 6 at a stop 7 pulling out /
                       parking 8 end of its way 9 waiting for room on its exit */
    float speed_kmh;
    float stopped_s;
    float why_dist; /* m to what holds it, -1 none */
    uint64_t waits_on;
    char why[16];
    char phase[40]; /* timetable bus: BusService phase */
} OoCarState;
int32_t oo_traffic_car_state(OoTraffic* t, uint64_t id, OoCarState* out);
int32_t oo_traffic_car_body(OoTraffic* t, int32_t i, float* out16); /* body -> world, as oo_body_matrix */
/* the camera's view: cars out of it run their scripts / animations less often (call before each tick) */
void oo_traffic_set_view(OoTraffic* t, double dx, double dy, double dz, float fov_deg, float aspect);
/* one line on why car `id` drives where it does: lane, exits, plan, route, holds (the editor logs it on a click) */
size_t oo_traffic_car_explain(OoTraffic* t, uint64_t id, char* buf, size_t cap);
size_t oo_mesh_file(OoBus* bus, int32_t i, char* buf, size_t cap); /* UTF-8 .o3d path */
/* [CTC] paint of the player's train mesh i (its first repaint, as OMSI's dialog picks): "<default>	<file>
" lines */
size_t oo_bus_paint(OoBus* bus, int32_t i, char* buf, size_t cap);
/* the text a [texttexture] field (script string var or built-in string number) shows now, UTF-8 */
size_t oo_bus_texttex_string(OoBus* bus, const char* field, char* buf, size_t cap);
int32_t oo_mesh_world(OoBus* bus, int32_t i, float* out16);       /* DirectXMath row-vector, row-major */
int32_t oo_mesh_visible(OoBus* bus, int32_t i);
/* analog controls over the keys: throttle/brake 0..1, steering -1 left .. +1 right; active 0 = keys */
void oo_set_analog(OoBus* bus, int32_t active, float throttle, float brake, float steering);
float oo_inv_min_turn_radius(OoBus* bus);
/* record every output device (loopback) for `seconds`: levels in the log, WAVs in wav_dir */
int32_t oo_audio_probe(float seconds, const char* wav_dir);
/* log openOMSI's report of the sound set (entries heard / off and why) */
void oo_sound_report(OoBus* bus);
void oo_debug_wheels(OoBus* bus);

#ifdef __cplusplus
}
#endif
