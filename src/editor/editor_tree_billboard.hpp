#pragma once

#include "gfx/debug_mesh.hpp"
#include "omsi/o3d_format.hpp"

#include <cstdint>
#include <vector>

namespace omsi::editor {

// MergeMapTrees _billboard_cross_mesh, remapped to OMSI map Z-up (Y-up mesh → swap Y/Z).
[[nodiscard]] gfx::BufferMesh make_tree_billboard_mesh(float height, float width);

inline float tree_billboard_width(float height, float aspect_or_ratio, float sco_width, float sco_ratio) {
    if (sco_width > 0.01f) return sco_width;
    const float r = (aspect_or_ratio > 0.01f) ? aspect_or_ratio : ((sco_ratio > 0.01f) ? sco_ratio : 0.85f);
    return height * r;
}

// OMSI TKachelForest.GenerateForest / TreePostCalc: bake many billboards into one mesh
// (RotationY × Scale × Translation applied to unit cross geometry).
struct ForestTreeInstance {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float width = 1.f;
    float height = 1.f;
    float yaw_deg = 0.f; // OMSI object rotation X (heading)
};

[[nodiscard]] gfx::BufferMesh build_forest_mesh(const gfx::BufferMesh& unit_billboard,
                                                const std::vector<ForestTreeInstance>& trees);

} // namespace omsi::editor
