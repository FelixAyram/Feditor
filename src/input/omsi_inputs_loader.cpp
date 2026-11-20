#include "omsi/inputs.hpp"

#include "omsi/logger.hpp"

#include <fstream>
#include <sstream>

namespace omsi::inputs {
namespace {

std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

ViewAction view_action_from_command(const std::string& command) {
    if (command == "view_set_driver") return ViewAction::set_driver;
    if (command == "view_set_passenger") return ViewAction::set_passenger;
    if (command == "view_set_outside") return ViewAction::set_outside;
    if (command == "view_set_map") return ViewAction::set_map;
    if (command == "view_set_schedule") return ViewAction::set_schedule;
    if (command == "view_reset_direction") return ViewAction::reset_direction;
    if (command == "view_reset_all_directions") return ViewAction::reset_all_directions;
    if (command == "view_interiorcam_minus") return ViewAction::interiorcam_minus;
    if (command == "view_interiorcam_plus") return ViewAction::interiorcam_plus;
    if (command == "toggel_mouse_ctrl") return ViewAction::toggle_mouse_ctrl;
    return ViewAction::none;
}

std::optional<std::vector<KeyboardBinding>> load_keyboard_cfg(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> lines;
    {
        std::istringstream ss(bytes);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(trim(line));
    }

    std::vector<KeyboardBinding> out;
    std::string section = "game";
    std::vector<std::string> entry;

    auto flush_entry = [&]() {
        if (entry.size() < 3) {
            entry.clear();
            return;
        }
        KeyboardBinding b{};
        b.command = entry[0];
        try {
            b.dik_code = std::stoi(entry[1]);
            b.modifiers = std::stoi(entry[2]);
        } catch (...) {
            entry.clear();
            return;
        }
        b.section = section;
        out.push_back(std::move(b));
        entry.clear();
    };

    for (const auto& raw : lines) {
        if (raw.empty()) continue;
        if (raw == "[game]") {
            flush_entry();
            section = "game";
            continue;
        }
        if (raw == "[vehicles]") {
            flush_entry();
            section = "vehicles";
            continue;
        }
        if (raw == "[entry]") {
            flush_entry();
            continue;
        }
        entry.push_back(raw);
    }
    flush_entry();

    omsi::log::info("inputs: loaded " + std::to_string(out.size()) + " bindings from " + path.string());
    return out;
}

} // namespace omsi::inputs
