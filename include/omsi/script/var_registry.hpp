#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace omsi::script {

// OMSI _Unit130_sub_007F64E4: merge varlist files -> flat index = global var id.
class VarRegistry {
public:
    bool load_engine_lists(const std::filesystem::path& omsi_root);
    bool load_scenery_lists(const std::filesystem::path& omsi_root);
    bool merge_bus_lists(const std::filesystem::path& bus_dir,
                         const std::vector<std::filesystem::path>& varnamelist_files,
                         const std::vector<std::filesystem::path>& stringvarnamelist_files);

    // Append a float if missing (OMSI scenery vars like TrafficLightPhase).
    bool ensure_float(const std::string& name);

    [[nodiscard]] std::size_t float_var_count() const { return float_names_.size(); }
    [[nodiscard]] std::size_t string_var_count() const { return string_names_.size(); }

    [[nodiscard]] std::optional<int> float_index(const std::string& name) const;
    [[nodiscard]] std::optional<int> string_index(const std::string& name) const;
    [[nodiscard]] const std::string& float_name(int index) const;
    [[nodiscard]] const std::string& string_name(int index) const;

    [[nodiscard]] const std::vector<std::string>& float_names() const { return float_names_; }
    [[nodiscard]] const std::vector<std::string>& string_names() const { return string_names_; }

private:
    std::vector<std::string> float_names_;
    std::vector<std::string> string_names_;
    std::unordered_map<std::string, int> float_index_ci_;
    std::unordered_map<std::string, int> string_index_ci_;

    static std::string upper_key(std::string_view name);
    bool append_varlist_file(const std::filesystem::path& path, bool is_string);
};

} // namespace omsi::script
