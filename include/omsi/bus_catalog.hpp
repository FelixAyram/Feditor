#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace omsi::bus_catalog {

// OMSI [friendlyname]: up to 3 lines — category / model / variant (color or operator).
struct FriendlyName {
    std::string category; // line 1
    std::string model;    // line 2 (falls back to line 1 / folder)
    std::string variant;  // line 3
};

struct BusEntry {
    std::filesystem::path bus_path;
    std::string folder_name; // Vehicles\{folder}
    std::string file_name;
    FriendlyName friendly;
    std::string list_label; // preformatted for UI
};

// Only Vehicles/<folder>/*.bus (not nested under Model/, Generated/, etc.).
[[nodiscard]] std::vector<BusEntry> scan_vehicles(const std::filesystem::path& omsi_root);

[[nodiscard]] FriendlyName parse_friendlyname(const std::filesystem::path& bus_path);

} // namespace omsi::bus_catalog
