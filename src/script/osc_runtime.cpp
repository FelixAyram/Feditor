#include "omsi/utf8_win32.hpp"
#include "omsi/script/osc_runtime.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>

namespace omsi::script {
namespace {

std::string trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

bool is_comment(std::string_view line) {
    const auto t = trim(line);
    return t.empty() || t.front() == '\'';
}

struct MacroBlock {
    std::string name;
    std::vector<std::string> lines;
};

std::vector<MacroBlock> parse_macros(const std::vector<std::string>& lines) {
    std::vector<MacroBlock> macros;
    MacroBlock* current = nullptr;
    for (const auto& raw : lines) {
        if (is_comment(raw)) continue;
        const auto line = trim(raw);
        if (line == "{end}") {
            current = nullptr;
            continue;
        }
        if (line.size() > 2 && line.front() == '{' && line.back() == '}') {
            macros.push_back({line.substr(1, line.size() - 2), {}});
            current = &macros.back();
            continue;
        }
        if (current) current->lines.push_back(line);
    }
    return macros;
}

void scan_references(const std::string& text, const std::regex& re, const char* prefix,
                     const std::function<void(const std::string&)>& on_name) {
    std::sregex_iterator it(text.begin(), text.end(), re);
    const std::sregex_iterator end;
    for (; it != end; ++it) {
        const std::string name = (*it)[1].str();
        if (!name.empty()) on_name(name);
    }
}

} // namespace

OscScriptBundle load_osc_scripts(const std::filesystem::path& bus_dir,
                                  const std::vector<std::filesystem::path>& script_files, const VarRegistry& vars) {
    OscScriptBundle bundle{};
    const std::regex float_var_re(R"(\(L\.L\.([^)]+)\))");
    const std::regex store_var_re(R"(\(S\.L\.([^)]+)\))");
    const std::regex string_var_re(R"(\(S\$\.([^)]+)\))");
    const std::regex const_re(R"(\(C\.L\.([^)]+)\))");
    const std::regex macro_call_re(R"(\(M\.L\.([^)]+)\))");

    for (const auto& rel : script_files) {
        const auto path = bus_dir / rel;
        OscCompileStats stats{};
        stats.path = path;

        const auto lines = cfg::read_cfg_lines(path);
        if (lines.empty()) {
            stats.errors.push_back("OMSI Script: Datei nicht gefunden oder leer: " + omsi::path_text(path));
            omsi::log::error("OMSI Script: could not read '" + omsi::path_text(path) + "'");
            ++bundle.total_errors;
            bundle.scripts.push_back(std::move(stats));
            continue;
        }

        const auto macros = parse_macros(lines);
        stats.macro_count = static_cast<int>(macros.size());
        for (const auto& macro : macros) {
            if (macro.name == "frame") ++stats.frame_macro_count;
            if (macro.name == "init") ++stats.init_macro_count;
        }

        std::string full_text;
        for (const auto& line : lines) {
            if (!is_comment(line)) full_text += line + '\n';
        }

        auto check_float = [&](const std::string& name) {
            if (!vars.float_index(name)) {
                const std::string msg = "OMSI Script: Unbekannte Variable '" + name + "' in " + omsi::path_text(path.filename());
                stats.warnings.push_back(msg);
                omsi::log::warn(msg);
                ++stats.unknown_tokens;
            }
            ++stats.token_count;
        };

        scan_references(full_text, float_var_re, "L.L.", check_float);
        scan_references(full_text, store_var_re, "S.L.", check_float);
        scan_references(full_text, string_var_re, "S$.", [&](const std::string& name) {
            if (!vars.string_index(name)) {
                const std::string msg =
                    "OMSI Script: Unbekannte String-Variable '" + name + "' in " + omsi::path_text(path.filename());
                stats.warnings.push_back(msg);
                omsi::log::warn(msg);
                ++stats.unknown_tokens;
            }
            ++stats.token_count;
        });

        scan_references(full_text, macro_call_re, "M.L.", [&](const std::string& name) {
            const std::string macro_name = "macro:" + name;
            const bool found = std::any_of(macros.begin(), macros.end(),
                                           [&](const MacroBlock& m) { return m.name == macro_name || m.name == name; });
            if (!found) {
                const std::string msg = "OMSI Script: Unbekanntes Makro '(M.L." + name + ")' in " + omsi::path_text(path.filename());
                stats.warnings.push_back(msg);
                omsi::log::warn(msg);
            }
            ++stats.token_count;
        });

        scan_references(full_text, const_re, "C.L.", [&](const std::string&) { ++stats.token_count; });

        omsi::log::info("OMSI Script: " + omsi::path_text(path.filename()) + " macros=" + std::to_string(stats.macro_count) +
                        " init=" + std::to_string(stats.init_macro_count) + " frame=" +
                        std::to_string(stats.frame_macro_count) + " refs=" + std::to_string(stats.token_count) +
                        " unknown=" + std::to_string(stats.unknown_tokens));
        bundle.total_warnings += static_cast<int>(stats.warnings.size());
        bundle.scripts.push_back(std::move(stats));
    }

    omsi::log::info("OMSI Script: loaded " + std::to_string(bundle.scripts.size()) + " files warnings=" +
                    std::to_string(bundle.total_warnings));
    return bundle;
}

} // namespace omsi::script
