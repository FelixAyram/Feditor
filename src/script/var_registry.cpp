#include "omsi/script/var_registry.hpp"
#include "omsi/path_utf8.hpp"

#include "omsi/logger.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <unordered_set>

namespace omsi::script {
namespace {

std::string trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

bool is_comment_line(std::string_view line) {
    const auto t = trim(line);
    return t.empty() || t.front() == '\'';
}

} // namespace

std::string VarRegistry::upper_key(std::string_view name) {
    std::string out(name);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool VarRegistry::append_varlist_file(const std::filesystem::path& path, bool is_string) {
    std::ifstream in(path);
    if (!in) {
        omsi::log::error("OMSI varlist: could not open '" + omsi::path_utf8(path) + "'");
        return false;
    }

    std::string line;
    int added = 0;
    while (std::getline(in, line)) {
        const auto name = trim(line);
        if (name.empty() || is_comment_line(name)) continue;

        auto& names = is_string ? string_names_ : float_names_;
        auto& index = is_string ? string_index_ci_ : float_index_ci_;
        const auto key = upper_key(name);
        if (index.contains(key)) {
            // Many SCO varlists repeat names (and merge onto scenery base). One warn per name/file.
            static thread_local std::unordered_set<std::string> warned;
            const std::string wk = omsi::path_utf8(path.filename()) + "|" + key;
            if (warned.insert(wk).second) {
                omsi::log::warn("OMSI varlist: duplicate variable '" + name + "' in " +
                                omsi::path_utf8(path.filename()) + " (ignored, first index kept)");
            }
            continue;
        }
        index[key] = static_cast<int>(names.size());
        names.push_back(name);
        ++added;
    }

    if (added > 0)
        omsi::log::info("OMSI varlist: +" + std::to_string(added) + " from " + omsi::path_utf8(path.filename()));
    return true;
}

bool VarRegistry::load_engine_lists(const std::filesystem::path& omsi_root) {
    if (omsi_root.empty()) return false;
    const auto float_path = omsi_root / "program" / "varlist_roadvehicle.txt";
    const auto string_path = omsi_root / "program" / "stringvarlist_roadvehicle.txt";
    bool ok = true;
    if (!append_varlist_file(float_path, false)) ok = false;
    if (!append_varlist_file(string_path, true)) ok = false;
    omsi::log::info("OMSI varlist: engine floats=" + std::to_string(float_names_.size()) + " strings=" +
                    std::to_string(string_names_.size()));
    return ok;
}

bool VarRegistry::load_scenery_lists(const std::filesystem::path& omsi_root) {
    if (omsi_root.empty()) return false;
    const auto float_path = omsi_root / "program" / "varlist_scenobj.txt";
    const auto string_path = omsi_root / "program" / "stringvarlist_scenobj.txt";
    bool ok = true;
    if (!append_varlist_file(float_path, false)) ok = false;
    if (std::filesystem::exists(string_path) && !append_varlist_file(string_path, true)) ok = false;
    omsi::log::info("OMSI varlist: scenery floats=" + std::to_string(float_names_.size()) + " strings=" +
                    std::to_string(string_names_.size()));
    return ok;
}

bool VarRegistry::merge_bus_lists(const std::filesystem::path& bus_dir,
                                   const std::vector<std::filesystem::path>& varnamelist_files,
                                   const std::vector<std::filesystem::path>& stringvarnamelist_files) {
    const auto floats_before = float_names_.size();
    const auto strings_before = string_names_.size();
    bool ok = true;
    for (const auto& rel : varnamelist_files) {
        const auto path = bus_dir / rel;
        if (!append_varlist_file(path, false)) ok = false;
    }
    for (const auto& rel : stringvarnamelist_files) {
        const auto path = bus_dir / rel;
        if (!append_varlist_file(path, true)) ok = false;
    }
    if (float_names_.size() != floats_before || string_names_.size() != strings_before) {
        omsi::log::info("OMSI varlist: merged total floats=" + std::to_string(float_names_.size()) +
                        " strings=" + std::to_string(string_names_.size()));
    }
    return ok;
}

bool VarRegistry::ensure_float(const std::string& name) {
    if (name.empty() || float_index(name)) return false;
    const int idx = static_cast<int>(float_names_.size());
    float_names_.push_back(name);
    float_index_ci_[upper_key(name)] = idx;
    return true;
}

std::optional<int> VarRegistry::float_index(const std::string& name) const {
    const auto it = float_index_ci_.find(upper_key(name));
    if (it == float_index_ci_.end()) return std::nullopt;
    return it->second;
}

std::optional<int> VarRegistry::string_index(const std::string& name) const {
    const auto it = string_index_ci_.find(upper_key(name));
    if (it == string_index_ci_.end()) return std::nullopt;
    return it->second;
}

const std::string& VarRegistry::float_name(int index) const { return float_names_.at(static_cast<std::size_t>(index)); }

const std::string& VarRegistry::string_name(int index) const {
    return string_names_.at(static_cast<std::size_t>(index));
}

} // namespace omsi::script
