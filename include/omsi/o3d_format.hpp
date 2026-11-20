#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "f4f23ma/math.hpp"

namespace omsi::o3d {

// Binary layout mirrors OMSI TX2O3D_Mesh / mc_o3dfiles (Omsi.exe).
// No conversion on load — GPU vertex buffer uses identical stride.

inline constexpr std::uint16_t kMagic = 0x1984; // file bytes: 0x84 0x19
inline constexpr std::uint8_t kChunkVertices = 0x17;
inline constexpr std::uint8_t kChunkTriangles = 0x49;
inline constexpr std::uint8_t kChunkMaterials = 0x26;
inline constexpr std::uint8_t kChunkBones = 0x54;
inline constexpr std::uint8_t kChunkTransform = 0x79;
inline constexpr std::uint32_t kNoEncryption = 0xFFFFFFFFu;

#pragma pack(push, 1)

struct FileHeaderV1 {
    std::uint16_t magic;
    std::uint8_t version;
};

struct FileHeaderExtended {
    std::uint8_t options; // bit0 long tri indices, bit1 alt encryption seed
    std::uint32_t encryption_key;
};

// 32 bytes on disk — 8 consecutive floats (pos, normal, uv).
struct Vertex {
    f4f23ma::f32 position[3];
    f4f23ma::f32 normal[3];
    f4f23ma::f32 uv[2];
};
static_assert(sizeof(Vertex) == 32);

struct TriangleU16 {
    std::uint16_t i0, i1, i2, material;
};
static_assert(sizeof(TriangleU16) == 8);

struct TriangleU32 {
    std::uint32_t i0, i1, i2;
    std::uint16_t material;
};
static_assert(sizeof(TriangleU32) == 14);

// 44 bytes fixed + Pascal string (length byte + chars, no NUL).
struct MaterialFixed {
    f4f23ma::f32 diffuse[4];
    f4f23ma::f32 specular[3];
    f4f23ma::f32 emission[3];
    f4f23ma::f32 power;
};
static_assert(sizeof(MaterialFixed) == 44);

struct TransformBlock {
    f4f23ma::f32 rows[16];
};
static_assert(sizeof(TransformBlock) == 64);

#pragma pack(pop)

struct BoneWeight {
    std::uint32_t vertex_index;
    f4f23ma::f32 weight;
};

struct Bone {
    std::string name;
    std::vector<BoneWeight> weights;
};

struct Material {
    MaterialFixed fixed{};
    std::string texture; // cp1252 in OMSI
};

struct NativeSection {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct NativeMesh {
    std::uint8_t version = 0;
    bool long_triangle_indices = false;
    bool alt_encryption_seed = false;
    bool encrypted = false;
    std::uint32_t encryption_key = kNoEncryption;

    NativeSection vertices;
    NativeSection triangles;
    NativeSection materials_raw;
    NativeSection transform;
    NativeSection bones_raw;

    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_count = 0;
    std::uint16_t material_count = 0;

    std::span<const Vertex> vertex_span() const {
        return {reinterpret_cast<const Vertex*>(vertices.data), vertex_count};
    }

    std::span<const TriangleU16> triangle_u16_span() const {
        return {reinterpret_cast<const TriangleU16*>(triangles.data), triangle_count};
    }

    std::span<const TriangleU32> triangle_u32_span() const {
        return {reinterpret_cast<const TriangleU32*>(triangles.data), triangle_count};
    }

    std::span<const TransformBlock> transform_span() const {
        if (!transform.data) return {};
        return {reinterpret_cast<const TransformBlock*>(transform.data), 1};
    }

    f4f23ma::ma4 object_matrix() const {
        const auto blocks = transform_span();
        if (blocks.empty()) return f4f23ma::ma4::identity();
        return f4f23ma::ma4::from_o3d_rows(blocks[0].rows);
    }

    const f4f23ma::f32* object_rows() const {
        static constexpr f4f23ma::f32 kIdentity[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                                                       0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
        const auto blocks = transform_span();
        return blocks.empty() ? kIdentity : blocks[0].rows;
    }

    // Parses Pascal-string materials from the raw section (only metadata walk).
    std::vector<Material> parse_materials() const;
    std::vector<Bone> parse_bones() const;
};

// OMSI world: +X to the right of +Y, +Y to the left of +X, +Z up (height).
// File vertices are OMSI file space; Blender-O3D-IO exports (0x79) need Y<->Z for Z-up display.

// OMSI / Blender-O3D-IO: 0x79 stores row-major rows (object / pivot matrix).
inline f4f23ma::f4 o3d_row_transform_point(const f4f23ma::f32 rows[16], const f4f23ma::f4& p) {
    const f4f23ma::f32 x = p.x;
    const f4f23ma::f32 y = p.y;
    const f4f23ma::f32 z = p.z;
    const f4f23ma::f32 w = p.w;
    return {
        rows[0] * x + rows[4] * y + rows[8] * z + rows[12] * w,
        rows[1] * x + rows[5] * y + rows[9] * z + rows[13] * w,
        rows[2] * x + rows[6] * y + rows[10] * z + rows[14] * w,
        rows[3] * x + rows[7] * y + rows[11] * z + rows[15] * w,
    };
}

inline f4f23ma::f4 o3d_row_transform_direction3(const f4f23ma::f32 rows[16], const f4f23ma::f4& d) {
    const f4f23ma::f32 x = d.x;
    const f4f23ma::f32 y = d.y;
    const f4f23ma::f32 z = d.z;
    return {
        rows[0] * x + rows[4] * y + rows[8] * z,
        rows[1] * x + rows[5] * y + rows[9] * z,
        rows[2] * x + rows[6] * y + rows[10] * z,
        0.f,
    };
}

// Raw |column| lengths from O3D 0x79 (Unity ApplyO3DTransformStatic localScale). Diagnostics only —
// scenery verts are already in meters; applying this (often ~5 from Blender object scale) makes
// atlas buildings giant vs OMSI Editor.
inline void o3d_unity_root_column_scale(const f4f23ma::f32 rows[16], f4f23ma::f32& sx, f4f23ma::f32& sy,
                                        f4f23ma::f32& sz) {
    const auto col_len = [](f4f23ma::f32 x, f4f23ma::f32 y, f4f23ma::f32 z) {
        return std::sqrt(x * x + y * y + z * z);
    };
    sx = col_len(rows[0], rows[4], rows[8]);
    sy = col_len(rows[1], rows[5], rows[9]);
    sz = col_len(rows[2], rows[6], rows[10]);
}

// Map/scenery root: translation (pivot) only, scale = 1. Matches Blender-O3D-IO (0x79 ≈ pivot)
// and OMSI Editor size for atlas packs (Vaalserq*, Felix*, …).
inline f4f23ma::ma4 o3d_unity_root_transform(const f4f23ma::f32 rows[16]) {
    f4f23ma::ma4 out = f4f23ma::ma4::identity();
    out.m[3][0] = rows[3];
    out.m[3][1] = rows[7];
    out.m[3][2] = rows[11];
    return out;
}

inline f4f23ma::f32 o3d_row_matrix_det3(const f4f23ma::f32 rows[16]) {
    const f4f23ma::f32 m00 = rows[0];
    const f4f23ma::f32 m01 = rows[1];
    const f4f23ma::f32 m02 = rows[2];
    const f4f23ma::f32 m10 = rows[4];
    const f4f23ma::f32 m11 = rows[5];
    const f4f23ma::f32 m12 = rows[6];
    const f4f23ma::f32 m20 = rows[8];
    const f4f23ma::f32 m21 = rows[9];
    const f4f23ma::f32 m22 = rows[10];
    return m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) +
           m02 * (m10 * m21 - m11 * m20);
}

inline bool o3d_row_matrix_flips_winding(const f4f23ma::f32 rows[16]) {
    return o3d_row_matrix_det3(rows) < 0.f;
}

inline Vertex flip_vertex_normal(const Vertex& src) {
    Vertex out = src;
    out.normal[0] = -src.normal[0];
    out.normal[1] = -src.normal[1];
    out.normal[2] = -src.normal[2];
    return out;
}

inline const f4f23ma::f32* default_mesh_basis_rows() {
    static constexpr f4f23ma::f32 kRows[16] = {
        1.f, 0.f, 0.f, 0.f, //
        0.f, 0.f, 1.f, 0.f, //
        0.f, -1.f, 0.f, 0.f, //
        0.f, 0.f, 0.f, 1.f, //
    };
    return kRows;
}

// 0x79 reflection bases (det < 0, typical Y<->Z swap) store outward normals for the opposite winding.
inline bool mesh_normals_need_flip(const NativeMesh& mesh) {
    const auto blocks = mesh.transform_span();
    const f4f23ma::f32* rows = blocks.empty() ? default_mesh_basis_rows() : blocks[0].rows;
    return o3d_row_matrix_flips_winding(rows);
}

// Y-up (Blender export) -> OMSI Z-up: (x,y,z) -> (x,z,y). Height = Z. No axis flips/negations.
inline f4f23ma::ma4 o3d_y_up_to_z_up_matrix() {
    f4f23ma::ma4 r{};
    r.m[0][0] = 1.f;
    r.m[2][1] = 1.f; // y' = z
    r.m[1][2] = 1.f; // z' = y
    r.m[3][3] = 1.f;
    return r;
}

// Legacy meshes without 0x79: Blender Y-up exports need Y<->Z; OMSI scenery is already Z-up.
inline bool mesh_has_transform_chunk(const NativeMesh& mesh) {
    return !mesh.transform_span().empty();
}

inline void mesh_local_position_extents(const NativeMesh& mesh, f4f23ma::f32& dx, f4f23ma::f32& dy,
                                        f4f23ma::f32& dz) {
    const auto verts = mesh.vertex_span();
    if (verts.empty()) {
        dx = dy = dz = 0.f;
        return;
    }
    f4f23ma::f32 minx = verts[0].position[0], maxx = minx;
    f4f23ma::f32 miny = verts[0].position[1], maxy = miny;
    f4f23ma::f32 minz = verts[0].position[2], maxz = minz;
    for (const auto& v : verts) {
        minx = (std::min)(minx, v.position[0]);
        maxx = (std::max)(maxx, v.position[0]);
        miny = (std::min)(miny, v.position[1]);
        maxy = (std::max)(maxy, v.position[1]);
        minz = (std::min)(minz, v.position[2]);
        maxz = (std::max)(maxz, v.position[2]);
    }
    dx = maxx - minx;
    dy = maxy - miny;
    dz = maxz - minz;
}

// Blender exports without 0x79 are taller on Y; OMSI scenery (trees, tiles) is taller on Z.
inline bool mesh_vertices_authored_y_up(const NativeMesh& mesh) {
    f4f23ma::f32 dx = 0.f, dy = 0.f, dz = 0.f;
    mesh_local_position_extents(mesh, dx, dy, dz);
    constexpr f4f23ma::f32 kEps = 1e-4f;
    if (dy <= kEps && dz <= kEps) return false;
    if (dz <= kEps) return true;
    if (dy <= kEps) return false;
    return dy > dz * 1.15f;
}

// Blender-O3D-IO (io_o3d_import.py): axis_conversion_matrix swaps Y<->Z on import.
// Net world position = axis_conversion @ file_vertex (0x79 only affects object pivot in Blender).
// https://github.com/space928/Blender-O3D-IO-Public/blob/main/o3d_io/io_o3d_import.py
inline f4f23ma::ma4 o3d_blender_io_axis_conversion() { return o3d_y_up_to_z_up_matrix(); }

// Local draw matrix.
// Only Blender-O3D-IO 0x79 bases need Y<->Z (io_o3d_import axis_conversion).
// Do NOT use bbox Y-up heuristics here: thin doors on Y-up vehicle packs (OA101) match
// Carroceria (Y=up, Z=along) already; converting them alone breaks hinges vs origin_trans.
inline f4f23ma::ma4 o3d_mesh_draw_matrix(const NativeMesh& mesh) {
    if (mesh_has_transform_chunk(mesh)) return o3d_blender_io_axis_conversion();
    return f4f23ma::ma4::identity();
}

// Y↔Z swap (0x79 / Blender) has det3 < 0 → reverses triangle winding on the GPU.
inline bool o3d_ma4_flips_winding(const f4f23ma::ma4& m) {
    const f4f23ma::f32 a = m.m[0][0], b = m.m[1][0], c = m.m[2][0];
    const f4f23ma::f32 d = m.m[0][1], e = m.m[1][1], f = m.m[2][1];
    const f4f23ma::f32 g = m.m[0][2], h = m.m[1][2], i = m.m[2][2];
    return (a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)) < 0.f;
}

inline bool o3d_draw_matrix_flips_winding(const NativeMesh& mesh) {
    return o3d_ma4_flips_winding(o3d_mesh_draw_matrix(mesh));
}

inline Vertex transform_vertex_with_ma4(const Vertex& src, const f4f23ma::ma4& matrix) {
    const f4f23ma::f4 p = matrix.transform_point(
        {src.position[0], src.position[1], src.position[2], 1.f});
    f4f23ma::f4 n = matrix.transform_direction3({src.normal[0], src.normal[1], src.normal[2], 0.f});
    n = n.normalized3();

    Vertex out = src;
    out.position[0] = p.x;
    out.position[1] = p.y;
    out.position[2] = p.z;
    out.normal[0] = n.x;
    out.normal[1] = n.y;
    out.normal[2] = n.z;
    return out;
}

inline Vertex transform_vertex_to_omsi_world(const Vertex& src, const NativeMesh& mesh) {
    return transform_vertex_with_ma4(src, o3d_mesh_draw_matrix(mesh));
}

// OMSI vertex obfuscation (Unity O3DLoader.DecryptVertex / omsi_o3d_crypto.py). Involutive.
inline constexpr std::uint32_t kEncryptionNoProduct = 0x0000FFFFu;

inline std::uint32_t o3d_init_encryption_seed(const std::uint32_t key, const bool alt_seed,
                                              const std::uint8_t version) {
    if (key == kNoEncryption) return 0;
    if (key == kEncryptionNoProduct) return 0;
    std::uint32_t salt = (key + version - 4u) & 0xFFFFu;
    if (alt_seed) salt = (salt + 381u) % 65000u;
    else salt = salt % 65000u;
    return salt;
}

inline std::vector<Vertex> decrypt_o3d_vertices(const NativeMesh& mesh) {
    const auto src = mesh.vertex_span();
    std::vector<Vertex> out(src.begin(), src.end());
    if (!mesh.encrypted || mesh.encryption_key == kNoEncryption ||
        mesh.encryption_key == kEncryptionNoProduct)
        return out;

    const int n_verts = static_cast<int>(mesh.vertex_count % 65000u);
    std::uint32_t prev_seed = o3d_init_encryption_seed(mesh.encryption_key, mesh.alt_encryption_seed, mesh.version);
    int prev_vpos_seed = 0;
    for (auto& v : out) {
        std::uint32_t product_id_salt = mesh.encryption_key == 0 ? (mesh.alt_encryption_seed ? 304u : 0u) : prev_seed;
        product_id_salt = static_cast<std::uint32_t>(
            ((prev_vpos_seed * n_verts) + (n_verts * static_cast<int>(product_id_salt))) % 8000);

        float ox = v.position[0], oy = v.position[1], oz = v.position[2];
        float nx = v.normal[0], ny = v.normal[1], nz = v.normal[2];
        float ux = v.uv[0], uy = v.uv[1];

        const float frac_x = ox - std::trunc(ox);
        const float frac_y = oy - std::trunc(oy);
        const float frac_z = oz - std::trunc(oz);
        const int salt = static_cast<int>(std::abs(frac_x * frac_y * frac_z) * 600.f) & 0xFF;

        if (product_id_salt >= 1000u) {
            if (product_id_salt >= 3000u) {
                if (product_id_salt > 7000u) std::swap(oy, oz);
            } else {
                std::swap(ox, oz);
            }
        } else {
            std::swap(ox, oy);
        }

        if ((product_id_salt & 3u) == 0u) nx = -nx;
        if ((product_id_salt % 6u) == 0u) ny = -ny;
        if ((product_id_salt % 7u) == 0u) nz = -nz;

        if (product_id_salt >= 600u) {
            if (product_id_salt > 4500u) std::swap(nx, ny);
        } else {
            std::swap(ny, nz);
        }

        if ((product_id_salt % 5u) == 0u)
            ux -= static_cast<float>((product_id_salt % 100u) * (product_id_salt % 100u)) / 10000.f;
        if ((product_id_salt % 3u) == 0u)
            uy -= static_cast<float>((product_id_salt % 50u) * (product_id_salt % 50u)) / 2500.f;

        v.position[0] = ox;
        v.position[1] = oy;
        v.position[2] = oz;
        v.normal[0] = nx;
        v.normal[1] = ny;
        v.normal[2] = nz;
        v.uv[0] = ux;
        v.uv[1] = uy;
        prev_seed = product_id_salt;
        prev_vpos_seed = salt;
    }
    return out;
}

inline Vertex prepare_o3d_vertex_for_gpu(const Vertex& src, const NativeMesh& mesh) {
    return transform_vertex_to_omsi_world(src, mesh);
}

inline bool o3d_needs_vertex_decrypt(const NativeMesh& mesh) {
    return mesh.encrypted && mesh.encryption_key != kNoEncryption &&
           mesh.encryption_key != kEncryptionNoProduct;
}

// One buffer: decrypt only if obfuscated, Y↔Z only if 0x79. Same GPU verts as the old copy+loop.
inline void prepare_o3d_mesh_vertices_for_gpu(const NativeMesh& mesh, std::vector<Vertex>& out) {
    if (o3d_needs_vertex_decrypt(mesh))
        out = decrypt_o3d_vertices(mesh);
    else {
        const auto src = mesh.vertex_span();
        out.assign(src.begin(), src.end());
    }
    if (!mesh_has_transform_chunk(mesh)) return;
    for (auto& v : out) v = prepare_o3d_vertex_for_gpu(v, mesh);
}

// OMSI: X lateral, Y forward, Z up. Bus viewer keeps the same basis for meshes + cameras
// (HelloO3dRenderer / axis gizmo). Do NOT negate Y — that put cab cams behind the bus.
inline f4f23ma::f4 bus_omsi_to_display(f4f23ma::f32 x, f4f23ma::f32 y, f4f23ma::f32 z) {
    return {x, y, z, 1.f};
}

inline f4f23ma::f4 bus_omsi_dir_to_display(f4f23ma::f32 x, f4f23ma::f32 y, f4f23ma::f32 z) {
    return {x, y, z, 0.f};
}

inline f4f23ma::ma4 omsi_to_render_y_up() {
    f4f23ma::ma4 r{};
    r.m[0][0] = 1.f;
    r.m[1][2] = 1.f;
    r.m[2][1] = 1.f;
    r.m[3][3] = 1.f;
    return r;
}

inline f4f23ma::ma4 omsi_to_dx_y_up() { return omsi_to_render_y_up(); }

inline f4f23ma::ma4 rotation_y_deg(f4f23ma::f32 yaw_deg) {
    const f4f23ma::f32 r = yaw_deg * (f4f23ma::f32(3.14159265) / 180.f);
    const f4f23ma::f32 c = std::cos(r);
    const f4f23ma::f32 s = std::sin(r);
    f4f23ma::ma4 m = f4f23ma::ma4::identity();
    m.m[0][0] = c;
    m.m[0][2] = s;
    m.m[2][0] = -s;
    m.m[2][2] = c;
    return m;
}

inline f4f23ma::ma4 omsi_map_object_transform(f4f23ma::f32 x, f4f23ma::f32 y, f4f23ma::f32 z,
                                              f4f23ma::f32 yaw_deg, f4f23ma::f32 tile_x,
                                              f4f23ma::f32 tile_y) {
    f4f23ma::ma4 t = f4f23ma::ma4::identity();
    t.m[3][0] = x + tile_x;
    t.m[3][1] = z;
    t.m[3][2] = y + tile_y;
    return rotation_y_deg(yaw_deg) * t;
}

inline f4f23ma::ma4 gpu_model_matrix(const f4f23ma::ma4& object = f4f23ma::ma4::identity()) {
    return omsi_to_render_y_up() * object;
}

inline f4f23ma::f4 omsi_pos_to_render_y_up(f4f23ma::f32 x, f4f23ma::f32 y, f4f23ma::f32 z) {
    return omsi_to_render_y_up().transform_point({x, y, z, 1.f});
}

inline f4f23ma::f4 omsi_pos_to_dx_y_up(f4f23ma::f32 x, f4f23ma::f32 y, f4f23ma::f32 z) {
    return omsi_pos_to_render_y_up(x, y, z);
}

inline f4f23ma::f4 omsi_dir_to_render_y_up(f4f23ma::f32 x, f4f23ma::f32 y, f4f23ma::f32 z) {
    return omsi_to_render_y_up().transform_direction3({x, y, z, 0.f});
}

inline Vertex transform_vertex_to_render_y_up(const Vertex& src, const f4f23ma::ma4& world_xform) {
    const f4f23ma::f4 p = world_xform.transform_point(
        {src.position[0], src.position[1], src.position[2], 1.f});
    f4f23ma::f4 n = world_xform.transform_direction3(
        {src.normal[0], src.normal[1], src.normal[2], 0.f});
    n = n.normalized3();

    Vertex out = src;
    out.position[0] = p.x;
    out.position[1] = p.y;
    out.position[2] = p.z;
    out.normal[0] = n.x;
    out.normal[1] = n.y;
    out.normal[2] = n.z;
    return out;
}

inline Vertex transform_vertex_to_dx_y_up(const Vertex& src, const f4f23ma::ma4& world_xform) {
    return transform_vertex_to_render_y_up(src, world_xform);
}

inline f4f23ma::f32 world_matrix_det3(const f4f23ma::ma4& world) {
    const auto& m = world.m;
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[1][0] * (m[0][1] * m[2][2] - m[0][2] * m[2][1]) +
           m[2][0] * (m[0][1] * m[1][2] - m[0][2] * m[1][1]);
}

// Row-major float[16] for Luna/DX shaders (mul(vec, mat)).
inline void ma4_to_shader_rows(const f4f23ma::ma4& matrix, float out_row_major[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out_row_major[r * 4 + c] = matrix.m[c][r];
}

} // namespace omsi::o3d
