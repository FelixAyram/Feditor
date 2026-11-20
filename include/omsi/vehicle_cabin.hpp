#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace omsi::cabin {

// Trampelpfade.cfg — TPathManager_LoadFromFile (Unit128).
struct PathPoint {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct PathLink {
    int a = 0;
    int b = 0;
};

struct VehiclePathsDocument {
    std::filesystem::path path;
    std::vector<std::string> step_sounds;
    std::vector<PathPoint> points;
    std::vector<PathLink> links;
};

// Sitzplaetze.cfg — TPassengerCabin_LoadFromFile (Unit86).
struct CabinSeat {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float height = 0.f;
    float yaw = 0.f;
    bool is_driver = false;
};

struct PassengerCabinDocument {
    std::filesystem::path path;
    std::vector<int> entries; // pathpnt indices
    std::vector<int> exits;
    std::vector<CabinSeat> seats;
    std::optional<CabinSeat> driver;
};

[[nodiscard]] std::optional<VehiclePathsDocument> load_vehicle_paths(const std::filesystem::path& path);
[[nodiscard]] std::optional<PassengerCabinDocument> load_passenger_cabin(const std::filesystem::path& path);

} // namespace omsi::cabin
