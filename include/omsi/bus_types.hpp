#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace omsi::bus {

struct BusCamera {
    float pos_x = 0.f;
    float pos_y = 0.f;
    float pos_z = 0.f;
    float rotation_centre_offset = 0.f;
    float fov_deg = 45.f;
    float yaw_deg = 0.f;
    float pitch_deg = 0.f;
    bool view_schedule = false;
};

// One [newachse] block from .bus (OMSI TRoadVehicle axle).
struct BusAxle {
    float long_m = 0.f;
    float maxwidth = 2.f;
    float minwidth = 1.5f;
    float raddurchmesser_m = 0.94f;
    float feder_kNm = 150.f;   // kN/m per side
    float maxforce_kN = 250.f;
    float daempfer = 20.f;     // kNs/m
    bool antrieb = false;
};

// Parsed .bus document (TComplMapObj_LoadFromFile order for post-processing).
struct BusDocument {
    std::filesystem::path bus_path;
    std::filesystem::path bus_dir;

    std::filesystem::path model_cfg_rel;
    std::filesystem::path sound_cfg_rel;
    std::filesystem::path sound_ai_cfg_rel;
    std::filesystem::path paths_cfg_rel;
    std::filesystem::path passengercabin_cfg_rel;

    std::vector<std::filesystem::path> varnamelist_files;
    std::vector<std::filesystem::path> stringvarnamelist_files;
    std::vector<std::filesystem::path> script_files;
    std::vector<std::filesystem::path> constfile_paths;

    std::vector<BusCamera> driver_cameras;
    std::vector<BusCamera> passenger_cameras;

    int std_driver_camera_index = 0;
    float outside_center_x = 0.f;
    float outside_center_y = 0.f;
    float outside_center_z = 1.2f;

    float kmcounter_init_km = 0.f;
    float kmcounter_init_m = 0.f;

    float mass_t = 10.f;
    float inertia_x = 500.f;
    float inertia_y = 200.f;
    float inertia_z = 500.f;
    float moment_x = 300.f;
    float moment_y = 80.f;
    float moment_z = 300.f;
    float bbox_half_x = 1.2f;
    float bbox_half_y = 4.4f;
    float bbox_half_z = 1.5f;
    float bbox_offset_y = 0.f;

    float schwerpunkt_m = 1.2f;
    float rollwiderstand_N = 900.f;
    float rot_pnt_long_m = 0.f;
    float inv_min_turnradius = 0.13f;
    std::vector<BusAxle> axles;
};

[[nodiscard]] std::optional<std::size_t> schedule_camera_index(const BusDocument& bus);

} // namespace omsi::bus
