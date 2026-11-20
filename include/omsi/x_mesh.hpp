#pragma once

#include "omsi/o3d_format.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace omsi::xfile {

// One `Mesh { … }` in a DirectX `.x` text file (xof 03xx txt).
// TextureFilename is taken from this mesh's MeshMaterialList only.
struct XMesh {
    std::vector<o3d::Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::string texture_filename;
    // MeshMaterialList Material diffuse RGBA (OMSI/D3DX uses this when TextureFilename is absent).
    float diffuse_rgba[4] = {1.f, 1.f, 1.f, 1.f};
    bool has_diffuse = false;
};

// All top-level Mesh blocks in one .x (StreetSign: casco + placa; wartehaus: 3 parts).
struct XFile {
    std::vector<XMesh> parts;
};

[[nodiscard]] std::optional<XFile> load_x_file(const std::filesystem::path& path);
// First mesh only (entrypoint helpers). Prefer load_x_file for SCO spawn.
[[nodiscard]] std::optional<XMesh> load_x_mesh(const std::filesystem::path& path);
void invalidate_x_mesh_cache(const std::filesystem::path& path = {});
[[nodiscard]] std::size_t x_mesh_cache_size();

} // namespace omsi::xfile
