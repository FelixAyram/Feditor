#include "omsi/o3d_writer.hpp"

#include <algorithm>
#include <fstream>

namespace omsi::o3d {
namespace {

void write_pascal(std::ostream& out, const std::string& s) {
    const auto n = static_cast<std::uint8_t>((std::min)(s.size(), static_cast<std::size_t>(255)));
    out.put(static_cast<char>(n));
    out.write(s.data(), n);
}

} // namespace

bool write_o3d_file(const std::filesystem::path& path, const WritableMesh& mesh) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    const bool long_idx = mesh.vertices.size() > 65535u ||
                          std::any_of(mesh.triangles.begin(), mesh.triangles.end(), [](const TriangleU32& t) {
                              return t.i0 > 65535u || t.i1 > 65535u || t.i2 > 65535u;
                          });
    const std::uint8_t version = 4;
    const std::uint8_t options = long_idx ? 1u : 0u;
    const std::uint16_t magic = kMagic;
    out.write(reinterpret_cast<const char*>(&magic), 2);
    out.put(static_cast<char>(version));
    out.put(static_cast<char>(options));
    const std::uint32_t key = kNoEncryption;
    out.write(reinterpret_cast<const char*>(&key), 4);

    const std::uint32_t nv = static_cast<std::uint32_t>(mesh.vertices.size());
    out.put(static_cast<char>(kChunkVertices));
    out.write(reinterpret_cast<const char*>(&nv), 4);
    out.write(reinterpret_cast<const char*>(mesh.vertices.data()),
              static_cast<std::streamsize>(nv * sizeof(Vertex)));

    const std::uint32_t nt = static_cast<std::uint32_t>(mesh.triangles.size());
    out.put(static_cast<char>(kChunkTriangles));
    out.write(reinterpret_cast<const char*>(&nt), 4);
    if (long_idx) {
        out.write(reinterpret_cast<const char*>(mesh.triangles.data()),
                  static_cast<std::streamsize>(nt * sizeof(TriangleU32)));
    } else {
        for (const auto& t : mesh.triangles) {
            TriangleU16 s{static_cast<std::uint16_t>(t.i0), static_cast<std::uint16_t>(t.i1),
                          static_cast<std::uint16_t>(t.i2), t.material};
            out.write(reinterpret_cast<const char*>(&s), sizeof(s));
        }
    }

    auto mats = mesh.materials;
    if (mats.empty()) {
        Material m{};
        m.fixed.diffuse[0] = m.fixed.diffuse[1] = m.fixed.diffuse[2] = m.fixed.diffuse[3] = 1.f;
        m.fixed.power = 10.f;
        mats.push_back(std::move(m));
    }
    const std::uint16_t nm = static_cast<std::uint16_t>(mats.size());
    out.put(static_cast<char>(kChunkMaterials));
    out.write(reinterpret_cast<const char*>(&nm), 2);
    for (const auto& m : mats) {
        out.write(reinterpret_cast<const char*>(&m.fixed), sizeof(MaterialFixed));
        write_pascal(out, m.texture);
    }
    return static_cast<bool>(out);
}

} // namespace omsi::o3d
