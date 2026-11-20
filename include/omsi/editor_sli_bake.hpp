#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "omsi/map_types.hpp"

namespace omsi::editor {

struct SliBakeResult {
    std::filesystem::path sco_path;
    std::filesystem::path o3d_path;
    float cx = 0.f;
    float cy = 0.f;
    float cz = 0.f;
    int path_count = 0;
    int vertex_count = 0;
    std::string error;
};

// Unique folder under parent: `base`, then `base (1)`, `base (2)` (Windows Explorer).
[[nodiscard]] std::filesystem::path unique_sli_bake_dir(const std::filesystem::path& parent,
                                                        const std::string& base_name);

[[nodiscard]] std::string sli_bake_folder_name(std::int32_t first_spline_id);

// Merge selected spline instances into one .o3d + .sco (paths in object-local space, origin = AABB center).
[[nodiscard]] SliBakeResult bake_splines_to_sco(const std::vector<map::SplineInst>& splines,
                                                const std::filesystem::path& omsi_root,
                                                const std::filesystem::path& out_dir,
                                                const std::string& stem);

} // namespace omsi::editor
