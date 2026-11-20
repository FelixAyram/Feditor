#include "omsi/utf8_win32.hpp"
#include "omsi/bus_catalog.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cctype>

namespace omsi::bus_catalog {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

bool looks_like_section(const std::string& line) {
    return line.size() >= 3 && line.front() == '[' && line.back() == ']';
}

std::string ascii_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ieq_ascii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

} // namespace

FriendlyName parse_friendlyname(const std::filesystem::path& bus_path) {
    FriendlyName out{};
    const auto lines = cfg::read_cfg_lines(bus_path);
    // Skip template blocks — use the last exact [friendlyname] header (real data block).
    std::size_t fn_start = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (line == "[friendlyname]" || ieq_ascii(line, "[friendlyname]")) fn_start = i;
    }
    if (fn_start >= lines.size()) return out;

    std::vector<std::string> collected;
    for (std::size_t i = fn_start + 1; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (looks_like_section(line)) break;
        if (line.empty()) {
            if (!collected.empty()) break;
            continue;
        }
        collected.push_back(line);
        if (collected.size() >= 3) break;
    }
    if (collected.size() >= 1) out.category = collected[0];
    if (collected.size() >= 2) out.model = collected[1];
    if (collected.size() >= 3) out.variant = collected[2];
    return out;
}

std::vector<BusEntry> scan_vehicles(const std::filesystem::path& omsi_root) {
    std::vector<BusEntry> out;
    const auto vehicles = omsi_root / "Vehicles";
    std::error_code ec;
    if (!std::filesystem::is_directory(vehicles, ec)) {
        omsi::log::warn("bus catalog: Vehicles not found under " + omsi::path_text(omsi_root));
        return out;
    }

    for (const auto& folder_ent : std::filesystem::directory_iterator(vehicles, ec)) {
        if (!folder_ent.is_directory(ec)) continue;
        const auto folder = folder_ent.path();
        const std::string folder_name = omsi::path_text(folder.filename());
        // Only Vehicles\{folder}\*.bus — ignore nested .bus (addons copies, Generated, etc.).
        for (const auto& file_ent : std::filesystem::directory_iterator(folder, ec)) {
            if (!file_ent.is_regular_file(ec)) continue;
            auto ext = omsi::path_text(file_ent.path().extension());
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".bus") continue;

            BusEntry e{};
            e.bus_path = file_ent.path();
            e.folder_name = folder_name;
            e.file_name = omsi::path_text(file_ent.path().filename());
            e.friendly = parse_friendlyname(e.bus_path);
            if (e.friendly.model.empty()) e.friendly.model = e.friendly.category;
            if (e.friendly.model.empty()) e.friendly.model = e.file_name;
            if (e.friendly.category.empty()) e.friendly.category = "(Sin categoría)";

            // Label: variant — file [folder]; group headers use category/model in the tree.
            std::string label;
            if (!e.friendly.variant.empty()) label = e.friendly.variant;
            else label = e.file_name;
            label += "  [" + e.folder_name + "]";
            if (e.file_name != e.friendly.model) label += "  (" + e.file_name + ")";
            e.list_label = std::move(label);
            out.push_back(std::move(e));
        }
    }

    std::sort(out.begin(), out.end(), [](const BusEntry& a, const BusEntry& b) {
        const int c = ascii_lower(a.friendly.category).compare(ascii_lower(b.friendly.category));
        if (c != 0) return c < 0;
        const int m = ascii_lower(a.friendly.model).compare(ascii_lower(b.friendly.model));
        if (m != 0) return m < 0;
        const int v = ascii_lower(a.friendly.variant).compare(ascii_lower(b.friendly.variant));
        if (v != 0) return v < 0;
        const int f = ascii_lower(a.folder_name).compare(ascii_lower(b.folder_name));
        if (f != 0) return f < 0;
        return ascii_lower(a.file_name) < ascii_lower(b.file_name);
    });

    omsi::log::info("bus catalog: " + std::to_string(out.size()) + " .bus under Vehicles/*/");
    return out;
}

} // namespace omsi::bus_catalog
