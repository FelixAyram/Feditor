#include "editor_tree_billboard.hpp"

#include <cmath>

namespace omsi::editor {
namespace {

void push_vert(gfx::BufferMesh& mesh, float x, float y, float z, float nx, float ny, float nz, float u, float v) {
    o3d::Vertex vert{};
    vert.position[0] = x;
    vert.position[1] = y;
    vert.position[2] = z;
    vert.normal[0] = nx;
    vert.normal[1] = ny;
    vert.normal[2] = nz;
    vert.uv[0] = u;
    vert.uv[1] = v;
    mesh.vertices.push_back(vert);
}

void push_quad(gfx::BufferMesh& mesh, const float c[4][3], float nx, float ny, float nz) {
    const float uvs[4][2] = {{1.f, 1.f}, {0.f, 1.f}, {0.f, 0.f}, {1.f, 0.f}};
    const std::uint32_t n0 = static_cast<std::uint32_t>(mesh.vertices.size());
    for (int i = 0; i < 4; ++i) push_vert(mesh, c[i][0], c[i][1], c[i][2], nx, ny, nz, uvs[i][0], uvs[i][1]);
    mesh.indices.insert(mesh.indices.end(), {n0 + 0, n0 + 1, n0 + 2, n0 + 0, n0 + 2, n0 + 3});
    // Back face
    const std::uint32_t n1 = static_cast<std::uint32_t>(mesh.vertices.size());
    for (int i = 0; i < 4; ++i)
        push_vert(mesh, c[i][0], c[i][1], c[i][2], -nx, -ny, -nz, uvs[i][0], uvs[i][1]);
    mesh.indices.insert(mesh.indices.end(), {n1 + 0, n1 + 2, n1 + 1, n1 + 0, n1 + 3, n1 + 2});
}

} // namespace

gfx::BufferMesh make_tree_billboard_mesh(float height, float width) {
    if (height < 0.1f) height = 12.f;
    if (width < 0.1f) width = height * 0.85f;
    const float hw = width * 0.5f;

    // MergeMapTrees Y-up cross → OMSI Z-up: (x,y,z)_y → (x,z,y)
    const float a[4][3] = {
        {hw, 0.f, 0.f},
        {-hw, 0.f, 0.f},
        {-hw, 0.f, height},
        {hw, 0.f, height},
    };
    const float b[4][3] = {
        {0.f, hw, 0.f},
        {0.f, -hw, 0.f},
        {0.f, -hw, height},
        {0.f, hw, height},
    };

    gfx::BufferMesh mesh{};
    push_quad(mesh, a, 0.f, -1.f, 0.f);
    push_quad(mesh, b, 1.f, 0.f, 0.f);
    return mesh;
}

gfx::BufferMesh build_forest_mesh(const gfx::BufferMesh& unit_billboard,
                                  const std::vector<ForestTreeInstance>& trees) {
    gfx::BufferMesh out{};
    if (trees.empty() || unit_billboard.vertices.empty()) return out;
    out.vertices.reserve(unit_billboard.vertices.size() * trees.size());
    out.indices.reserve(unit_billboard.indices.size() * trees.size());
    constexpr float kPi = 3.14159265f;
    for (const auto& t : trees) {
        const float yaw = t.yaw_deg * (kPi / 180.f);
        const float cy = std::cos(yaw);
        const float sy = std::sin(yaw);
        const float sx = (t.width > 0.01f) ? t.width : 1.f;
        const float sy_s = sx; // billboard cross: X/Y scale = width
        const float sz = (t.height > 0.01f) ? t.height : 1.f;
        const std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());
        for (const auto& v : unit_billboard.vertices) {
            // TreePostCalc: Scale * RotY * Translation (column-vector / row-major D3DX style applied to pos)
            float px = v.position[0] * sx;
            float py = v.position[1] * sy_s;
            float pz = v.position[2] * sz;
            // RotY around Z-up
            const float rx = px * cy - py * sy;
            const float ry = px * sy + py * cy;
            const float rz = pz;
            o3d::Vertex w = v;
            w.position[0] = rx + t.x;
            w.position[1] = ry + t.y;
            w.position[2] = rz + t.z;
            // rotate normal (ignore scale)
            const float nx = v.normal[0];
            const float ny = v.normal[1];
            w.normal[0] = nx * cy - ny * sy;
            w.normal[1] = nx * sy + ny * cy;
            out.vertices.push_back(w);
        }
        for (const auto idx : unit_billboard.indices) out.indices.push_back(base + idx);
    }
    return out;
}

} // namespace omsi::editor
