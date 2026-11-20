#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "omsi/o3d_format.hpp"

namespace omsi::o3d {

struct WritableMesh {
    std::vector<Vertex> vertices;
    std::vector<TriangleU32> triangles; // material in .material
    std::vector<Material> materials;
};

// Unencrypted v4. No 0x79 (verts already OMSI Z-up). Returns false on empty mesh.
[[nodiscard]] bool write_o3d_file(const std::filesystem::path& path, const WritableMesh& mesh);

} // namespace omsi::o3d
