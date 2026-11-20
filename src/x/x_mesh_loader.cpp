#include "omsi/x_mesh.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace omsi::xfile {
namespace {

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Skip whitespace and // line comments.
void skip_ws(const std::string& s, std::size_t& i) {
    while (i < s.size()) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(s[i]))) break;
        ++i;
    }
}

bool consume(const std::string& s, std::size_t& i, char c) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != c) return false;
    ++i;
    return true;
}

bool parse_number(const std::string& s, std::size_t& i, float& out) {
    skip_ws(s, i);
    if (i >= s.size()) return false;
    const char* begin = s.c_str() + i;
    char* end = nullptr;
    out = std::strtof(begin, &end);
    if (end == begin) return false;
    i = static_cast<std::size_t>(end - s.c_str());
    return true;
}

bool parse_dword(const std::string& s, std::size_t& i, std::uint32_t& out) {
    float f = 0.f;
    if (!parse_number(s, i, f)) return false;
    out = static_cast<std::uint32_t>(f);
    return true;
}

bool expect_semi(const std::string& s, std::size_t& i) {
    return consume(s, i, ';');
}

// .x text uses `z;,` between verts and `z;;` before the next DWORD — skip all separators.
void skip_separators(const std::string& s, std::size_t& i) {
    for (;;) {
        skip_ws(s, i);
        if (i < s.size() && (s[i] == ',' || s[i] == ';')) {
            ++i;
            continue;
        }
        break;
    }
}

using Mat4 = std::array<std::array<float, 4>, 4>;

constexpr Mat4 kMat4Identity = {{{{1.f, 0.f, 0.f, 0.f}},
                                 {{0.f, 1.f, 0.f, 0.f}},
                                 {{0.f, 0.f, 1.f, 0.f}},
                                 {{0.f, 0.f, 0.f, 1.f}}}};

bool parse_matrix4(const std::string& s, std::size_t brace, Mat4& out) {
    std::size_t i = brace + 1;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = 0.f;
            if (!parse_number(s, i, v)) return false;
            out[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = v;
            skip_separators(s, i);
        }
    }
    return true;
}

bool is_omsi_root_axis_swap(const Mat4& m) {
    constexpr float e = 1e-4f;
    return std::fabs(m[0][0] - 1.f) < e && std::fabs(m[0][1]) < e && std::fabs(m[0][2]) < e &&
           std::fabs(m[1][0]) < e && std::fabs(m[1][1]) < e && std::fabs(m[1][2] - 1.f) < e &&
           std::fabs(m[2][0]) < e && std::fabs(m[2][1] - 1.f) < e && std::fabs(m[2][2]) < e &&
           std::fabs(m[3][0]) < e && std::fabs(m[3][1]) < e && std::fabs(m[3][2]) < e;
}

bool mat4_is_identity(const Mat4& m) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float want = (r == c) ? 1.f : 0.f;
            if (std::fabs(m[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] - want) > 1e-4f) return false;
        }
    }
    return true;
}

Mat4 local_frame_matrix(const std::string& s, std::size_t mesh_brace) {
    // Parent Frame only — do not stack sibling FrameTransformMatrix (Plane + Plane001).
    std::size_t pos = mesh_brace;
    while (pos > 0) {
        const auto p = s.rfind("FrameTransformMatrix", pos);
        if (p == std::string::npos) break;
        const auto brace = s.find('{', p);
        if (brace == std::string::npos || brace >= mesh_brace) {
            if (p == 0) break;
            pos = p - 1;
            continue;
        }
        Mat4 mat = kMat4Identity;
        if (!parse_matrix4(s, brace, mat)) {
            if (p == 0) break;
            pos = p - 1;
            continue;
        }
        if (is_omsi_root_axis_swap(mat)) return kMat4Identity;
        return mat;
    }
    return kMat4Identity;
}

float mat3_det(const Mat4& m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

void transform_point(const Mat4& m, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
    oy = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
    oz = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
}

void transform_normal3(const Mat4& m, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = x * m[0][0] + y * m[1][0] + z * m[2][0];
    oy = x * m[0][1] + y * m[1][1] + z * m[2][1];
    oz = x * m[0][2] + y * m[1][2] + z * m[2][2];
}

void apply_frame_to_mesh(XMesh& mesh, const Mat4& frame_m) {
    if (mat4_is_identity(frame_m)) return;
    const bool flip = mat3_det(frame_m) < 0.f;
    for (auto& v : mesh.vertices) {
        float px = 0.f, py = 0.f, pz = 0.f;
        transform_point(frame_m, v.position[0], v.position[1], v.position[2], px, py, pz);
        v.position[0] = px;
        v.position[1] = py;
        v.position[2] = pz;
        float nx = 0.f, ny = 0.f, nz = 0.f;
        transform_normal3(frame_m, v.normal[0], v.normal[1], v.normal[2], nx, ny, nz);
        if (flip) {
            nx = -nx;
            ny = -ny;
            nz = -nz;
        }
        v.normal[0] = nx;
        v.normal[1] = ny;
        v.normal[2] = nz;
    }
    if (flip) {
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
            std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
    }
}

bool texture_key_ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

void flip_mesh_winding(XMesh& mesh) {
    for (auto& v : mesh.vertices) {
        v.normal[0] = -v.normal[0];
        v.normal[1] = -v.normal[1];
        v.normal[2] = -v.normal[2];
    }
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
}

float pos_dist2(const o3d::Vertex& a, const o3d::Vertex& b) {
    const float dx = a.position[0] - b.position[0];
    const float dy = a.position[1] - b.position[1];
    const float dz = a.position[2] - b.position[2];
    return dx * dx + dy * dy + dz * dz;
}

float normal_dot(const o3d::Vertex& a, const o3d::Vertex& b) {
    return a.normal[0] * b.normal[0] + a.normal[1] * b.normal[1] + a.normal[2] * b.normal[2];
}

void align_mirrored_sibling_parts(std::vector<XMesh>& parts) {
    for (std::size_t i = 1; i < parts.size(); ++i) {
        const auto& prev = parts[i - 1];
        auto& cur = parts[i];
        if (!texture_key_ieq(prev.texture_filename, cur.texture_filename)) continue;
        if (prev.vertices.empty() || cur.vertices.empty()) continue;
        if (pos_dist2(prev.vertices[0], cur.vertices[0]) > 0.05f * 0.05f) continue;
        if (normal_dot(prev.vertices[0], cur.vertices[0]) >= 0.f) continue;
        flip_mesh_winding(cur);
    }
}

// Find `Mesh {` that is not MeshNormals / MeshTextureCoords / MeshMaterialList / MeshFace…
std::size_t find_mesh_block(const std::string& s, std::size_t pos) {
    while (pos < s.size()) {
        const auto p = s.find("Mesh", pos);
        if (p == std::string::npos) return std::string::npos;
        if (p > 0 && std::isalnum(static_cast<unsigned char>(s[p - 1]))) {
            pos = p + 4;
            continue;
        }
        std::size_t j = p + 4;
        skip_ws(s, j);
        if (j < s.size() && (std::isalpha(static_cast<unsigned char>(s[j])) || s[j] == '_')) {
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            skip_ws(s, j);
        }
        if (j < s.size() && s[j] == '{') {
            const auto kw = s.substr(p, j - p);
            if (kw.find("MeshNormal") != std::string::npos || kw.find("MeshTexture") != std::string::npos ||
                kw.find("MeshMaterial") != std::string::npos || kw.find("MeshVertex") != std::string::npos ||
                kw.find("MeshFace") != std::string::npos) {
                pos = j + 1;
                continue;
            }
            return j;
        }
        pos = p + 4;
    }
    return std::string::npos;
}

std::size_t find_named_block(const std::string& s, const char* name, std::size_t from, std::size_t until) {
    const std::string key(name);
    std::size_t pos = from;
    while (pos < until && pos < s.size()) {
        const auto p = s.find(key, pos);
        if (p == std::string::npos || p >= until) return std::string::npos;
        if (p > 0 && std::isalnum(static_cast<unsigned char>(s[p - 1]))) {
            pos = p + key.size();
            continue;
        }
        std::size_t j = p + key.size();
        skip_ws(s, j);
        if (j < s.size() && (std::isalpha(static_cast<unsigned char>(s[j])) || s[j] == '_')) {
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            skip_ws(s, j);
        }
        if (j < until && j < s.size() && s[j] == '{') return j;
        pos = p + key.size();
    }
    return std::string::npos;
}

// Index of the '}' closing the block opened at `open` (skips quoted strings).
std::size_t find_block_end(const std::string& s, std::size_t open) {
    int depth = 0;
    bool in_str = false;
    for (std::size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') in_str = !in_str;
        if (in_str) continue;
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i;
    }
    return std::string::npos;
}

std::string texture_in_block(const std::string& s, std::size_t open, std::size_t close) {
    const auto tf = s.find("TextureFilename", open);
    if (tf == std::string::npos || tf >= close) return {};
    const auto q0 = s.find('"', tf);
    const auto q1 = (q0 == std::string::npos) ? std::string::npos : s.find('"', q0 + 1);
    if (q0 == std::string::npos || q1 == std::string::npos || q1 >= close) return {};
    return s.substr(q0 + 1, q1 - q0 - 1);
}

// Top-level `Material Name { … }` referenced from MeshMaterialList as `{ Name }`.
std::string texture_of_named_material(const std::string& s, const std::string& name) {
    std::size_t pos = 0;
    while (pos < s.size()) {
        const auto p = s.find("Material", pos);
        if (p == std::string::npos) return {};
        std::size_t j = p + 8;
        if (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) {
            skip_ws(s, j);
            std::size_t k = j;
            while (k < s.size() && (std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '_' || s[k] == '-' ||
                                    s[k] == '.'))
                ++k;
            if (s.compare(j, k - j, name) == 0) {
                skip_ws(s, k);
                if (k < s.size() && s[k] == '{') {
                    const auto close = find_block_end(s, k);
                    if (close != std::string::npos) return texture_in_block(s, k, close);
                }
            }
        }
        pos = p + 8;
    }
    return {};
}

struct XMaterialList {
    std::vector<std::uint32_t> face_material; // per .x face
    std::vector<std::string> textures;        // per material ("" = untextured)
    std::vector<std::array<float, 4>> diffuse; // Material faceColor RGBA
};

bool diffuse_in_block(const std::string& s, std::size_t open, std::size_t close, float out[4]) {
    std::size_t i = open + 1;
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
    if (!parse_number(s, i, r) || !consume(s, i, ';')) return false;
    if (!parse_number(s, i, g) || !consume(s, i, ';')) return false;
    if (!parse_number(s, i, b) || !consume(s, i, ';')) return false;
    if (!parse_number(s, i, a)) return false;
    if (i >= close) return false;
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
    return true;
}

bool diffuse_of_named_material(const std::string& s, const std::string& name, float out[4]) {
    std::size_t pos = 0;
    while (pos < s.size()) {
        const auto p = s.find("Material", pos);
        if (p == std::string::npos) return false;
        std::size_t j = p + 8;
        if (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) {
            skip_ws(s, j);
            std::size_t k = j;
            while (k < s.size() && (std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '_' || s[k] == '-' ||
                                    s[k] == '.'))
                ++k;
            if (s.compare(j, k - j, name) == 0) {
                skip_ws(s, k);
                if (k < s.size() && s[k] == '{') {
                    const auto close = find_block_end(s, k);
                    if (close != std::string::npos) return diffuse_in_block(s, k, close, out);
                }
            }
        }
        pos = p + 8;
    }
    return false;
}

// D3DXLoadMeshFromX semantics (OMSI draws one DrawSubset per material): face → material index.
std::optional<XMaterialList> parse_material_list(const std::string& s, std::size_t mesh_brace, std::size_t mesh_end) {
    const auto mb = find_named_block(s, "MeshMaterialList", mesh_brace, mesh_end);
    if (mb == std::string::npos) return std::nullopt;
    const auto mclose = find_block_end(s, mb);
    if (mclose == std::string::npos) return std::nullopt;
    std::size_t i = mb + 1;
    std::uint32_t n_mats = 0, n_face_idx = 0;
    if (!parse_dword(s, i, n_mats) || !expect_semi(s, i)) return std::nullopt;
    if (!parse_dword(s, i, n_face_idx) || !expect_semi(s, i)) return std::nullopt;
    XMaterialList out{};
    out.face_material.resize(n_face_idx, 0);
    for (std::uint32_t f = 0; f < n_face_idx; ++f) {
        if (!parse_dword(s, i, out.face_material[f])) return std::nullopt;
        skip_separators(s, i);
    }
    out.textures.resize(n_mats);
    out.diffuse.assign(n_mats, {1.f, 1.f, 1.f, 1.f});
    for (std::uint32_t m = 0; m < n_mats && i < mclose; ++m) {
        skip_separators(s, i);
        if (i >= mclose) break;
        if (s[i] == '{') {
            const auto close = s.find('}', i);
            if (close == std::string::npos || close > mclose) break;
            std::string name = s.substr(i + 1, close - i - 1);
            const auto a = name.find_first_not_of(" \t\r\n");
            const auto b = name.find_last_not_of(" \t\r\n");
            name = a == std::string::npos ? std::string{} : name.substr(a, b - a + 1);
            out.textures[m] = texture_of_named_material(s, name);
            float d[4] = {1.f, 1.f, 1.f, 1.f};
            if (diffuse_of_named_material(s, name, d))
                out.diffuse[m] = {d[0], d[1], d[2], d[3]};
            i = close + 1;
            continue;
        }
        const auto mat = find_named_block(s, "Material", i, mclose);
        if (mat == std::string::npos) break;
        const auto close = find_block_end(s, mat);
        if (close == std::string::npos || close > mclose) break;
        out.textures[m] = texture_in_block(s, mat, close);
        float d[4] = {1.f, 1.f, 1.f, 1.f};
        if (diffuse_in_block(s, mat, close, d)) out.diffuse[m] = {d[0], d[1], d[2], d[3]};
        i = close + 1;
    }
    return out;
}

std::optional<XMesh> parse_mesh_at(const std::string& s, std::size_t mesh_brace, std::size_t mesh_end,
                                   std::vector<std::uint32_t>* tri_material = nullptr) {
    std::size_t i = mesh_brace + 1;
    std::uint32_t n_verts = 0;
    if (!parse_dword(s, i, n_verts) || !expect_semi(s, i) || n_verts == 0) return std::nullopt;

    XMesh out{};
    out.vertices.resize(n_verts);
    for (std::uint32_t v = 0; v < n_verts; ++v) {
        float x = 0.f, y = 0.f, z = 0.f;
        if (!parse_number(s, i, x) || !consume(s, i, ';')) return std::nullopt;
        if (!parse_number(s, i, y) || !consume(s, i, ';')) return std::nullopt;
        if (!parse_number(s, i, z)) return std::nullopt;
        skip_separators(s, i);
        if (i > mesh_end) return std::nullopt;
        out.vertices[v].position[0] = x;
        out.vertices[v].position[1] = y;
        out.vertices[v].position[2] = z;
        out.vertices[v].normal[2] = 1.f;
    }

    std::uint32_t n_faces = 0;
    if (!parse_dword(s, i, n_faces) || !expect_semi(s, i)) return std::nullopt;
    out.indices.reserve(n_faces * 6);
    for (std::uint32_t f = 0; f < n_faces; ++f) {
        std::uint32_t n_idx = 0;
        if (!parse_dword(s, i, n_idx) || !consume(s, i, ';') || n_idx < 3) return std::nullopt;
        std::vector<std::uint32_t> face(n_idx);
        for (std::uint32_t k = 0; k < n_idx; ++k) {
            if (!parse_dword(s, i, face[k])) return std::nullopt;
            skip_ws(s, i);
            if (k + 1 < n_idx) {
                if (!consume(s, i, ',')) return std::nullopt;
            } else {
                skip_separators(s, i);
            }
        }
        skip_separators(s, i);
        for (std::uint32_t k = 1; k + 1 < n_idx; ++k) {
            out.indices.push_back(face[0]);
            out.indices.push_back(face[k + 1]);
            out.indices.push_back(face[k]);
            if (tri_material) tri_material->push_back(f);
        }
    }

    if (const auto nb = find_named_block(s, "MeshNormals", mesh_brace, mesh_end); nb != std::string::npos) {
        std::size_t ni = nb + 1;
        std::uint32_t nn = 0;
        if (parse_dword(s, ni, nn) && expect_semi(s, ni) && nn == n_verts) {
            for (std::uint32_t v = 0; v < nn; ++v) {
                float x = 0.f, y = 0.f, z = 0.f;
                if (!parse_number(s, ni, x) || !consume(s, ni, ';')) break;
                if (!parse_number(s, ni, y) || !consume(s, ni, ';')) break;
                if (!parse_number(s, ni, z)) break;
                skip_separators(s, ni);
                out.vertices[v].normal[0] = x;
                out.vertices[v].normal[1] = y;
                out.vertices[v].normal[2] = z;
            }
        }
    }

    if (const auto tb = find_named_block(s, "MeshTextureCoords", mesh_brace, mesh_end); tb != std::string::npos) {
        std::size_t ti = tb + 1;
        std::uint32_t nt = 0;
        if (parse_dword(s, ti, nt) && expect_semi(s, ti) && nt == n_verts) {
            for (std::uint32_t v = 0; v < nt; ++v) {
                float u = 0.f, vv = 0.f;
                if (!parse_number(s, ti, u) || !consume(s, ti, ';')) break;
                if (!parse_number(s, ti, vv)) break;
                skip_separators(s, ti);
                out.vertices[v].uv[0] = u;
                out.vertices[v].uv[1] = vv;
            }
        }
    }

    const auto tf = s.find("TextureFilename", mesh_brace);
    if (tf != std::string::npos && tf < mesh_end) {
        const auto q0 = s.find('"', tf);
        const auto q1 = (q0 == std::string::npos) ? std::string::npos : s.find('"', q0 + 1);
        if (q0 != std::string::npos && q1 != std::string::npos && q1 < mesh_end)
            out.texture_filename = s.substr(q0 + 1, q1 - q0 - 1);
    }

    if (out.indices.empty()) return std::nullopt;
    apply_frame_to_mesh(out, local_frame_matrix(s, mesh_brace));
    return out;
}

std::optional<XFile> load_x_file_uncached(const std::filesystem::path& path) {
    const std::string s = read_all(path);
    if (s.size() < 8) return std::nullopt;
    if (s.find("txt") == std::string::npos && s.find("TXT") == std::string::npos) return std::nullopt;

    XFile file{};
    std::size_t pos = 0;
    for (;;) {
        const std::size_t brace = find_mesh_block(s, pos);
        if (brace == std::string::npos) break;
        const std::size_t next = find_mesh_block(s, brace + 1);
        const std::size_t mesh_end = next == std::string::npos ? s.size() : next;
        std::vector<std::uint32_t> tri_face;
        auto part = parse_mesh_at(s, brace, mesh_end, &tri_face);
        pos = brace + 1;
        if (!part) continue;
        const auto mats = parse_material_list(s, brace, mesh_end);
        if (!mats || mats->textures.size() <= 1 || tri_face.size() * 3 != part->indices.size()) {
            file.parts.push_back(std::move(*part));
            continue;
        }
        // One part per material (DrawSubset order); vertices compacted so bbox/pick stay tight.
        const std::size_t n_mats = mats->textures.size();
        std::vector<XMesh> subs(n_mats);
        std::vector<std::vector<std::uint32_t>> remap(n_mats);
        for (std::size_t t = 0; t < tri_face.size(); ++t) {
            const std::uint32_t f = tri_face[t];
            std::uint32_t m = f < mats->face_material.size() ? mats->face_material[f] : 0;
            if (m >= n_mats) m = 0;
            auto& sub = subs[m];
            auto& rm = remap[m];
            if (rm.empty()) rm.assign(part->vertices.size(), ~0u);
            const std::size_t nv = part->vertices.size();
            if (part->indices[t * 3] >= nv || part->indices[t * 3 + 1] >= nv || part->indices[t * 3 + 2] >= nv)
                continue;
            for (int c = 0; c < 3; ++c) {
                const std::uint32_t vi = part->indices[t * 3 + static_cast<std::size_t>(c)];
                if (rm[vi] == ~0u) {
                    rm[vi] = static_cast<std::uint32_t>(sub.vertices.size());
                    sub.vertices.push_back(part->vertices[vi]);
                }
                sub.indices.push_back(rm[vi]);
            }
        }
        for (std::size_t m = 0; m < n_mats; ++m) {
            if (subs[m].indices.empty()) continue;
            subs[m].texture_filename = mats->textures[m];
            if (m < mats->diffuse.size()) {
                subs[m].diffuse_rgba[0] = mats->diffuse[m][0];
                subs[m].diffuse_rgba[1] = mats->diffuse[m][1];
                subs[m].diffuse_rgba[2] = mats->diffuse[m][2];
                subs[m].diffuse_rgba[3] = mats->diffuse[m][3];
                subs[m].has_diffuse = true;
            }
            file.parts.push_back(std::move(subs[m]));
        }
    }
    if (file.parts.empty()) return std::nullopt;
    align_mirrored_sibling_parts(file.parts);
    return file;
}

} // namespace

namespace {
std::mutex g_x_mu;
std::unordered_map<std::string, XFile> g_x_cache;

std::string x_key(const std::filesystem::path& p) {
    auto s = p.lexically_normal().string();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

void invalidate_x_mesh_cache(const std::filesystem::path& path) {
    std::lock_guard lock(g_x_mu);
    if (path.empty()) {
        g_x_cache.clear();
        return;
    }
    g_x_cache.erase(x_key(path));
}

std::size_t x_mesh_cache_size() {
    std::lock_guard lock(g_x_mu);
    return g_x_cache.size();
}

std::optional<XFile> load_x_file(const std::filesystem::path& path) {
    const auto key = x_key(path);
    {
        std::lock_guard lock(g_x_mu);
        if (const auto it = g_x_cache.find(key); it != g_x_cache.end()) return it->second;
    }
    auto loaded = load_x_file_uncached(path);
    if (!loaded) return std::nullopt;
    std::lock_guard lock(g_x_mu);
    g_x_cache.emplace(key, *loaded);
    return loaded;
}

std::optional<XMesh> load_x_mesh(const std::filesystem::path& path) {
    auto file = load_x_file(path);
    if (!file || file->parts.empty()) return std::nullopt;
    return file->parts.front();
}

} // namespace omsi::xfile
