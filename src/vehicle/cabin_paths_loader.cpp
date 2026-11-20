#include "omsi/utf8_win32.hpp"
#include "omsi/vehicle_cabin.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"

#include <cctype>

namespace omsi::cabin {
namespace {

bool parse_float(const std::string& text, float& out) {
    try {
        out = std::stof(text);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_int(const std::string& text, int& out) {
    try {
        out = std::stoi(text);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

std::optional<VehiclePathsDocument> load_vehicle_paths(const std::filesystem::path& path) {
    const auto doc = cfg::load_cfg_file(path);
    if (!doc) return std::nullopt;

    VehiclePathsDocument out{};
    out.path = path;

    if (const auto it = doc->find("[stepsoundpack]"); it != doc->end() && !it->second.empty()) {
        const auto& block = it->second[0];
        int count = 0;
        if (!block.empty()) parse_int(block[0], count);
        for (int i = 1; i <= count && static_cast<std::size_t>(i) < block.size(); ++i)
            out.step_sounds.push_back(block[static_cast<std::size_t>(i)]);
    }

    if (const auto it = doc->find("[pathpnt]"); it != doc->end()) {
        for (const auto& block : it->second) {
            PathPoint p{};
            if (block.size() >= 1) parse_float(block[0], p.x);
            if (block.size() >= 2) parse_float(block[1], p.y);
            if (block.size() >= 3) parse_float(block[2], p.z);
            out.points.push_back(p);
        }
    }

    if (const auto it = doc->find("[pathlink]"); it != doc->end()) {
        for (const auto& block : it->second) {
            PathLink link{};
            if (block.size() >= 1) parse_int(block[0], link.a);
            if (block.size() >= 2) parse_int(block[1], link.b);
            out.links.push_back(link);
        }
    }

    omsi::log::info("vehicle paths: " + omsi::path_text(path) + " points=" + std::to_string(out.points.size()) +
                    " links=" + std::to_string(out.links.size()) + " stepsounds=" +
                    std::to_string(out.step_sounds.size()));
    return out;
}

std::optional<PassengerCabinDocument> load_passenger_cabin(const std::filesystem::path& path) {
    const auto doc = cfg::load_cfg_file(path);
    if (!doc) return std::nullopt;

    PassengerCabinDocument out{};
    out.path = path;

    if (const auto it = doc->find("[entry]"); it != doc->end()) {
        for (const auto& block : it->second) {
            int idx = 0;
            if (!block.empty() && parse_int(block[0], idx)) out.entries.push_back(idx);
        }
    }
    if (const auto it = doc->find("[exit]"); it != doc->end()) {
        for (const auto& block : it->second) {
            int idx = 0;
            if (!block.empty() && parse_int(block[0], idx)) out.exits.push_back(idx);
        }
    }

    auto read_seat = [](const std::vector<std::string>& block, bool driver) -> CabinSeat {
        CabinSeat s{};
        s.is_driver = driver;
        if (block.size() >= 1) parse_float(block[0], s.x);
        if (block.size() >= 2) parse_float(block[1], s.y);
        if (block.size() >= 3) parse_float(block[2], s.z);
        if (block.size() >= 4) parse_float(block[3], s.height);
        if (block.size() >= 5) parse_float(block[4], s.yaw);
        return s;
    };

    if (const auto it = doc->find("[drivpos]"); it != doc->end() && !it->second.empty()) {
        out.driver = read_seat(it->second[0], true);
        out.seats.push_back(*out.driver);
    }
    if (const auto it = doc->find("[passpos]"); it != doc->end()) {
        for (const auto& block : it->second) out.seats.push_back(read_seat(block, false));
    }

    omsi::log::info("passenger cabin: " + omsi::path_text(path) + " seats=" + std::to_string(out.seats.size()) +
                    " entries=" + std::to_string(out.entries.size()) + " exits=" +
                    std::to_string(out.exits.size()) +
                    (out.driver ? " driver=yes" : " driver=no"));
    return out;
}

} // namespace omsi::cabin
