#pragma once

#include <cstdint>
#include <vector>

#include "o3d/o3d_loader.hpp"

namespace omsi::gfx {

struct BufferMesh {
    std::vector<omsi::o3d::Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

[[nodiscard]] BufferMesh make_unit_cube();

} // namespace omsi::gfx
