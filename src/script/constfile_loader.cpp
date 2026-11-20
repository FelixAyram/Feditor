#include "omsi/script/constfile.hpp"
#include "omsi/path_utf8.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"

namespace omsi::script {

std::optional<ConstFileDocument> load_constfile(const std::filesystem::path& path) {
    const auto lines = cfg::read_cfg_lines(path);
    if (lines.empty()) {
        omsi::log::error("OMSI constfile: empty or missing '" + omsi::path_utf8(path) + "'");
        return std::nullopt;
    }

    ConstFileDocument doc{};
    doc.path = path;
    std::string current_curve;

    for (const auto& raw : lines) {
        std::string line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line.front() == '\'') continue;

        if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
            const auto tag = line.substr(1, line.size() - 2);
            if (tag == "const") {
                current_curve.clear();
            } else if (tag == "newcurve") {
                current_curve.clear();
            } else if (tag == "pnt") {
                // points follow on next lines - handled below via state
            }
            continue;
        }

        if (current_curve.empty() && doc.curves.empty() && line.find(' ') == std::string::npos) {
            // might be curve name after [newcurve] - cfg sections flatten; use line-based parse
        }
    }

    // Line-oriented parser (OMSI constfile is not always bracket-section based in files).
    current_curve.clear();
    enum class Mode { none, const_name, curve_name, pnt_x } mode = Mode::none;
    float pending_x = 0.f;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (line.empty() || line.front() == '\'') continue;
        if (line == "[const]") {
            mode = Mode::const_name;
            continue;
        }
        if (line == "[newcurve]") {
            mode = Mode::curve_name;
            continue;
        }
        if (line == "[pnt]") {
            mode = Mode::pnt_x;
            continue;
        }

        if (mode == Mode::const_name) {
            const std::string name = line;
            if (i + 1 >= lines.size()) {
                omsi::log::error("OMSI constfile: [const] '" + name + "' missing value in " + omsi::path_utf8(path.filename()));
                break;
            }
            try {
                doc.constants[name] = std::stof(lines[++i]);
            } catch (...) {
                omsi::log::error("OMSI constfile: invalid float for const '" + name + "'");
            }
            mode = Mode::none;
            continue;
        }

        if (mode == Mode::curve_name) {
            current_curve = line;
            doc.curves[current_curve] = ConstCurve{.name = current_curve};
            mode = Mode::none;
            continue;
        }

        if (mode == Mode::pnt_x) {
            try {
                pending_x = std::stof(line);
                mode = Mode::none;
                if (i + 1 < lines.size()) {
                    ConstCurvePoint pt{.x = pending_x, .y = std::stof(lines[++i])};
                    if (current_curve.empty()) {
                        omsi::log::warn("OMSI constfile: [pnt] without [newcurve] in " + omsi::path_utf8(path.filename()));
                    } else {
                        doc.curves[current_curve].points.push_back(pt);
                    }
                }
            } catch (...) {
                omsi::log::error("OMSI constfile: invalid [pnt] in " + omsi::path_utf8(path.filename()));
            }
            continue;
        }
    }

    omsi::log::info("OMSI constfile: " + omsi::path_utf8(path.filename()) + " consts=" + std::to_string(doc.constants.size()) +
                    " curves=" + std::to_string(doc.curves.size()));
    return doc;
}

std::vector<ConstFileDocument> load_constfiles(const std::filesystem::path& bus_dir,
                                                const std::vector<std::filesystem::path>& rel_paths) {
    std::vector<ConstFileDocument> out;
    out.reserve(rel_paths.size());
    for (const auto& rel : rel_paths) {
        if (auto doc = load_constfile(bus_dir / rel)) out.push_back(std::move(*doc));
    }
    return out;
}

} // namespace omsi::script
