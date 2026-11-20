#include "debug_mesh.hpp"

#include "f4f23ma/math.hpp"

namespace omsi::gfx {
namespace {

using omsi::o3d::Vertex;

void add_face(std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices, f4f23ma::f4 n,
              f4f23ma::f4 a, f4f23ma::f4 b, f4f23ma::f4 c, f4f23ma::f4 d) {
    const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
    n.w = 0.f;
    verts.push_back(Vertex{{a.x, a.y, a.z}, {n.x, n.y, n.z}, {0.f, 0.f}});
    verts.push_back(Vertex{{b.x, b.y, b.z}, {n.x, n.y, n.z}, {1.f, 0.f}});
    verts.push_back(Vertex{{c.x, c.y, c.z}, {n.x, n.y, n.z}, {1.f, 1.f}});
    verts.push_back(Vertex{{d.x, d.y, d.z}, {n.x, n.y, n.z}, {0.f, 1.f}});
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

} // namespace

BufferMesh make_unit_cube() {
    BufferMesh mesh{};
    const f4f23ma::f32 s = 1.f;
    add_face(mesh.vertices, mesh.indices, {0.f, 0.f, 1.f, 0.f},
             {-s, -s, s, 0.f}, {s, -s, s, 0.f}, {s, s, s, 0.f}, {-s, s, s, 0.f});
    add_face(mesh.vertices, mesh.indices, {0.f, 0.f, -1.f, 0.f},
             {s, -s, -s, 0.f}, {-s, -s, -s, 0.f}, {-s, s, -s, 0.f}, {s, s, -s, 0.f});
    add_face(mesh.vertices, mesh.indices, {1.f, 0.f, 0.f, 0.f},
             {s, -s, -s, 0.f}, {s, -s, s, 0.f}, {s, s, s, 0.f}, {s, s, -s, 0.f});
    add_face(mesh.vertices, mesh.indices, {-1.f, 0.f, 0.f, 0.f},
             {-s, -s, s, 0.f}, {-s, -s, -s, 0.f}, {-s, s, -s, 0.f}, {-s, s, s, 0.f});
    add_face(mesh.vertices, mesh.indices, {0.f, 1.f, 0.f, 0.f},
             {-s, s, s, 0.f}, {s, s, s, 0.f}, {s, s, -s, 0.f}, {-s, s, -s, 0.f});
    add_face(mesh.vertices, mesh.indices, {0.f, -1.f, 0.f, 0.f},
             {-s, -s, -s, 0.f}, {s, -s, -s, 0.f}, {s, -s, s, 0.f}, {-s, -s, s, 0.f});
    return mesh;
}

} // namespace omsi::gfx
