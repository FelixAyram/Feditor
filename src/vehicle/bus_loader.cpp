#include "omsi/bus.hpp"
#include "omsi/path_utf8.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"

#include <charconv>
#include <stdexcept>

namespace omsi::bus {
namespace {

bool parse_float(std::string_view text, float& out) {
    text = text;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    if (text.empty()) return false;
    try {
        out = std::stof(std::string(text));
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<BusCamera> parse_camera_block(const std::vector<std::string>& lines) {
    if (lines.size() < 6) return std::nullopt;
    BusCamera cam{};
    if (!parse_float(lines[0], cam.pos_x)) return std::nullopt;
    if (!parse_float(lines[1], cam.pos_y)) return std::nullopt;
    if (!parse_float(lines[2], cam.pos_z)) return std::nullopt;
    if (!parse_float(lines[3], cam.rotation_centre_offset)) return std::nullopt;
    if (!parse_float(lines[4], cam.fov_deg)) return std::nullopt;
    if (!parse_float(lines[5], cam.yaw_deg)) return std::nullopt;
    if (lines.size() >= 7) parse_float(lines[6], cam.pitch_deg);
    return cam;
}

void parse_numbered_file_list(const cfg::CfgDocument& doc, const char* section_key,
                               std::vector<std::filesystem::path>& out_paths) {
    const auto it = doc.find(section_key);
    if (it == doc.end()) return;
    for (const auto& block : it->second) {
        if (block.empty()) continue;
        int count = 0;
        try {
            count = std::stoi(block[0]);
        } catch (...) {
            continue;
        }
        for (int i = 1; i <= count && static_cast<std::size_t>(i) < block.size(); ++i) {
            if (!block[static_cast<std::size_t>(i)].empty()) out_paths.emplace_back(block[static_cast<std::size_t>(i)]);
        }
    }
}

void parse_single_line_path(const cfg::CfgDocument& doc, const char* section_key, std::filesystem::path& out) {
    const auto it = doc.find(section_key);
    if (it == doc.end() || it->second.empty() || it->second[0].empty()) return;
    out = it->second[0][0];
}

} // namespace

std::optional<BusDocument> load_bus(const std::filesystem::path& path) {
    const auto doc = cfg::load_cfg_file(path);
    if (!doc) return std::nullopt;

    BusDocument bus{};
    bus.bus_path = path;
    bus.bus_dir = path.parent_path();

    parse_single_line_path(*doc, "[model]", bus.model_cfg_rel);
    parse_single_line_path(*doc, "[sound]", bus.sound_cfg_rel);
    parse_single_line_path(*doc, "[sound_ai]", bus.sound_ai_cfg_rel);
    parse_single_line_path(*doc, "[paths]", bus.paths_cfg_rel);
    parse_single_line_path(*doc, "[passengercabin]", bus.passengercabin_cfg_rel);

    parse_numbered_file_list(*doc, "[varnamelist]", bus.varnamelist_files);
    parse_numbered_file_list(*doc, "[stringvarnamelist]", bus.stringvarnamelist_files);
    parse_numbered_file_list(*doc, "[script]", bus.script_files);
    parse_numbered_file_list(*doc, "[constfile]", bus.constfile_paths);

    if (const auto it = doc->find("[add_camera_driver]"); it != doc->end()) {
        for (const auto& block : it->second) {
            if (auto cam = parse_camera_block(block)) bus.driver_cameras.push_back(*cam);
        }
    }

    if (const auto it = doc->find("[add_camera_pax]"); it != doc->end()) {
        for (const auto& block : it->second) {
            if (auto cam = parse_camera_block(block)) bus.passenger_cameras.push_back(*cam);
        }
    }

    if (const auto it = doc->find("[view_schedule]"); it != doc->end() && !bus.driver_cameras.empty())
        bus.driver_cameras.back().view_schedule = true;

    if (const auto it = doc->find("[set_camera_std]"); it != doc->end() && !it->second.empty() && !it->second[0].empty()) {
        try {
            bus.std_driver_camera_index = std::stoi(it->second[0][0]);
        } catch (...) {
        }
    }

    if (const auto it = doc->find("[set_camera_outside_center]"); it != doc->end() && !it->second.empty()) {
        const auto& lines = it->second[0];
        if (lines.size() >= 1) parse_float(lines[0], bus.outside_center_x);
        if (lines.size() >= 2) parse_float(lines[1], bus.outside_center_y);
        if (lines.size() >= 3) parse_float(lines[2], bus.outside_center_z);
    }

    if (const auto it = doc->find("[kmcounter_init]"); it != doc->end() && !it->second.empty()) {
        const auto& lines = it->second[0];
        if (lines.size() >= 1) parse_float(lines[0], bus.kmcounter_init_km);
        if (lines.size() >= 2) parse_float(lines[1], bus.kmcounter_init_m);
    }

    if (const auto it = doc->find("[mass]"); it != doc->end() && !it->second.empty() && !it->second[0].empty())
        parse_float(it->second[0][0], bus.mass_t);

    if (const auto it = doc->find("[momentofintertia]"); it != doc->end() && !it->second.empty()) {
        const auto& lines = it->second[0];
        if (lines.size() >= 1) parse_float(lines[0], bus.moment_x);
        if (lines.size() >= 2) parse_float(lines[1], bus.moment_y);
        if (lines.size() >= 3) parse_float(lines[2], bus.moment_z);
    }

    // OMSI [mass] block may also list inertia on following lines in same section instance.
    if (const auto it = doc->find("[mass]"); it != doc->end()) {
        for (const auto& block : it->second) {
            if (block.size() >= 4) {
                parse_float(block[1], bus.inertia_x);
                parse_float(block[2], bus.inertia_y);
                parse_float(block[3], bus.inertia_z);
            }
        }
    }

    if (const auto it = doc->find("[boundingbox]"); it != doc->end() && !it->second.empty()) {
        const auto& lines = it->second[0];
        if (lines.size() >= 1) parse_float(lines[0], bus.bbox_half_x);
        if (lines.size() >= 2) parse_float(lines[1], bus.bbox_half_y);
        if (lines.size() >= 3) parse_float(lines[2], bus.bbox_half_z);
        if (lines.size() >= 4) parse_float(lines[3], bus.bbox_offset_y);
    }

    if (const auto it = doc->find("[schwerpunkt]"); it != doc->end() && !it->second.empty() && !it->second[0].empty())
        parse_float(it->second[0][0], bus.schwerpunkt_m);
    if (const auto it = doc->find("[rollwiderstand]"); it != doc->end() && !it->second.empty() && !it->second[0].empty())
        parse_float(it->second[0][0], bus.rollwiderstand_N);
    if (const auto it = doc->find("[rot_pnt_long]"); it != doc->end() && !it->second.empty() && !it->second[0].empty())
        parse_float(it->second[0][0], bus.rot_pnt_long_m);
    if (const auto it = doc->find("[inv_min_turnradius]"); it != doc->end() && !it->second.empty() &&
                                                                 !it->second[0].empty())
        parse_float(it->second[0][0], bus.inv_min_turnradius);

    // [newachse] blocks: alternating key/value lines (achse_long / 3.39 / achse_feder / 150 …).
    if (const auto it = doc->find("[newachse]"); it != doc->end()) {
        for (const auto& block : it->second) {
            BusAxle axle{};
            for (std::size_t i = 0; i + 1 < block.size(); ++i) {
                const std::string& key = block[i];
                const std::string& val = block[i + 1];
                if (key == "achse_long") {
                    parse_float(val, axle.long_m);
                    ++i;
                } else if (key == "achse_maxwidth") {
                    parse_float(val, axle.maxwidth);
                    ++i;
                } else if (key == "achse_minwidth") {
                    parse_float(val, axle.minwidth);
                    ++i;
                } else if (key == "achse_raddurchmesser") {
                    parse_float(val, axle.raddurchmesser_m);
                    ++i;
                } else if (key == "achse_feder") {
                    parse_float(val, axle.feder_kNm);
                    ++i;
                } else if (key == "achse_maxforce") {
                    parse_float(val, axle.maxforce_kN);
                    ++i;
                } else if (key == "achse_daempfer") {
                    parse_float(val, axle.daempfer);
                    ++i;
                } else if (key == "achse_antrieb") {
                    float flag = 0.f;
                    if (parse_float(val, flag)) axle.antrieb = flag > 0.5f;
                    ++i;
                }
            }
            bus.axles.push_back(axle);
        }
    }

    omsi::log::info("bus: " + omsi::path_utf8(path) + " model=" + omsi::path_utf8(bus.model_cfg_rel) + " varlists=" +
                    std::to_string(bus.varnamelist_files.size()) + " stringlists=" +
                    std::to_string(bus.stringvarnamelist_files.size()) + " scripts=" +
                    std::to_string(bus.script_files.size()) + " constfiles=" +
                    std::to_string(bus.constfile_paths.size()) + " sound=" + omsi::path_utf8(bus.sound_cfg_rel) +
                    " mass=" + std::to_string(bus.mass_t) + "t roll=" + std::to_string(bus.rollwiderstand_N) +
                    "N axles=" + std::to_string(bus.axles.size()) + " bbox=(" + std::to_string(bus.bbox_half_x) + "," +
                    std::to_string(bus.bbox_half_y) + "," + std::to_string(bus.bbox_half_z) + ")" +
                    " driver_cams=" + std::to_string(bus.driver_cameras.size()) + " pax_cams=" +
                    std::to_string(bus.passenger_cameras.size()) + " std_cam=" +
                    std::to_string(bus.std_driver_camera_index));
    return bus;
}

std::filesystem::path resolve_model_cfg_path(const BusDocument& bus) {
    return bus.bus_path.parent_path() / bus.model_cfg_rel;
}

std::optional<std::size_t> schedule_camera_index(const BusDocument& bus) {
    for (std::size_t i = 0; i < bus.driver_cameras.size(); ++i) {
        if (bus.driver_cameras[i].view_schedule) return i;
    }
    return std::nullopt;
}

} // namespace omsi::bus
