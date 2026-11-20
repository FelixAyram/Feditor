#include "omsi/vfs_overlay.hpp"
#include "omsi/fs_cow.hpp"
#include "editor_ground.hpp"

#include "omsi/logger.hpp"
#include "omsi/o3d_format.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#pragma comment(lib, "windowscodecs.lib")

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <span>
#include <vector>

namespace omsi::editor {
namespace {

std::filesystem::path resolve_ground_texture(const std::filesystem::path& omsi_root,
                                             const std::filesystem::path& map_root,
                                             const std::filesystem::path& rel) {
    if (rel.empty()) return {};
    if (rel.is_absolute() && omsi::vfs::exists(rel)) return rel;

    const auto try_path = [](const std::filesystem::path& p) -> std::filesystem::path {
        if (omsi::vfs::exists(p)) return p;
        // Prefer sibling .dds (same as mesh textures).
        const auto dds = p.parent_path() / (p.stem().wstring() + L".dds");
        if (omsi::vfs::exists(dds)) return dds;
        return {};
    };

    if (auto p = try_path(omsi_root / rel); !p.empty()) return p;
    if (auto p = try_path(map_root / rel); !p.empty()) return p;

    // Case variants: texture/ vs Texture/
    auto s = rel.generic_string();
    if (s.size() >= 8 && (s.compare(0, 8, "texture/") == 0 || s.compare(0, 8, "texture\\") == 0)) {
        s[0] = 'T';
        if (auto p = try_path(omsi_root / s); !p.empty()) return p;
    } else if (s.size() >= 8 && (s.compare(0, 8, "Texture/") == 0 || s.compare(0, 8, "Texture\\") == 0)) {
        s[0] = 't';
        if (auto p = try_path(omsi_root / s); !p.empty()) return p;
    }

    const auto fallback = omsi_root / rel;
    log::warn("groundtex missing: " + rel.string() + " tried " + fallback.string());
    return fallback;
}

int clamp_idx(int v, int dim) {
    if (v < 0) return 0;
    if (v > dim - 1) return dim - 1;
    return v;
}

} // namespace

std::filesystem::path ground_paint_bmp_path(const std::filesystem::path& map_path); // defined below

std::optional<std::vector<float>> load_terrain_heights(const std::filesystem::path& map_path) {
    const auto path = std::filesystem::path(map_path.string() + ".terrain");
    if (const auto ram = omsi::vfs::get(path)) {
        const std::size_t cnt = static_cast<std::size_t>(kTerrainDim) * static_cast<std::size_t>(kTerrainDim);
        if (ram->size() < 4 + cnt * sizeof(float)) return std::nullopt;
        std::vector<float> h(cnt);
        std::memcpy(h.data(), ram->data() + 4, cnt * sizeof(float));
        return h;
    }
    if (!omsi::vfs::exists(path)) return std::nullopt;

    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    std::uint32_t header = 0;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in) return std::nullopt;

    const int n = kTerrainDim * kTerrainDim;
    std::vector<float> heights(static_cast<std::size_t>(n));
    in.read(reinterpret_cast<char*>(heights.data()), static_cast<std::streamsize>(heights.size() * sizeof(float)));
    if (!in) return std::nullopt;
    (void)header;
    return heights;
}

float sample_terrain_height(const std::vector<float>& heights, float size_meters, float x, float y) {
    if (heights.size() < static_cast<std::size_t>(kTerrainDim * kTerrainDim)) return 0.f;
    const float size = size_meters > 0.01f ? size_meters : map::kStandardTileMeters;
    const float gx = x / size * static_cast<float>(kTerrainDim - 1);
    const float gy = y / size * static_cast<float>(kTerrainDim - 1);
    const int x0 = clamp_idx(static_cast<int>(std::floor(gx)), kTerrainDim);
    const int x1 = clamp_idx(static_cast<int>(std::ceil(gx)), kTerrainDim);
    const int y0 = clamp_idx(static_cast<int>(std::floor(gy)), kTerrainDim);
    const int y1 = clamp_idx(static_cast<int>(std::ceil(gy)), kTerrainDim);
    const float fx = gx - static_cast<float>(x0);
    const float fy = gy - static_cast<float>(y0);
    const auto at = [&](int xi, int yi) {
        return heights[static_cast<std::size_t>(yi) * static_cast<std::size_t>(kTerrainDim) +
                       static_cast<std::size_t>(xi)];
    };
    const float ll = at(x0, y0);
    const float lh = at(x1, y0);
    const float hl = at(x0, y1);
    const float hh = at(x1, y1);
    const float a = ll * (1.f - fx) + lh * fx;
    const float b = hl * (1.f - fx) + hh * fx;
    return a * (1.f - fy) + b * fy;
}

float sample_terrain_height(const LocalTerrainTile& tile, float x, float y) {
    return sample_terrain_height(tile.heights, tile.size_meters, x, y);
}

void apply_terrain_hole_mask_to_mesh(gfx::BufferMesh& mesh, const std::vector<std::uint8_t>& hole_mask, int dim) {
    if (dim < 2 || hole_mask.size() < static_cast<std::size_t>(dim * dim)) return;
    if (mesh.vertices.size() < static_cast<std::size_t>(dim * dim)) return;
    mesh.indices.clear();
    mesh.indices.reserve(static_cast<std::size_t>((dim - 1) * (dim - 1) * 6));
    auto masked = [&](int x, int y) -> bool {
        return hole_mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(dim) +
                         static_cast<std::size_t>(x)] != 0;
    };
    for (int y = 0; y < dim - 1; ++y) {
        for (int x = 0; x < dim - 1; ++x) {
            // Cut when the quad center is inside the hole (any 2+ corners masked).
            const int m = (masked(x, y) ? 1 : 0) + (masked(x + 1, y) ? 1 : 0) +
                          (masked(x + 1, y + 1) ? 1 : 0) + (masked(x, y + 1) ? 1 : 0);
            if (m >= 2) continue;
            const std::uint32_t i0 = static_cast<std::uint32_t>(y * dim + x);
            const std::uint32_t i1 = static_cast<std::uint32_t>(y * dim + x + 1);
            const std::uint32_t i2 = static_cast<std::uint32_t>((y + 1) * dim + x + 1);
            const std::uint32_t i3 = static_cast<std::uint32_t>((y + 1) * dim + x);
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i3);
        }
    }
}

void apply_heightfield_to_terrain_mesh(gfx::BufferMesh& mesh, const std::vector<float>& heights, float size_meters) {
    if (heights.size() < static_cast<std::size_t>(kTerrainDim * kTerrainDim)) return;
    if (mesh.vertices.size() < static_cast<std::size_t>(kTerrainDim * kTerrainDim)) return;
    const float size = size_meters > 0.01f ? size_meters : map::kStandardTileMeters;
    const float step = size / static_cast<float>(kTerrainDim - 1);
    const float* h = heights.data();
    for (int y = 0; y < kTerrainDim; ++y) {
        for (int x = 0; x < kTerrainDim; ++x) {
            auto& v = mesh.vertices[static_cast<std::size_t>(y * kTerrainDim + x)];
            v.position[2] = h[static_cast<std::size_t>(y) * static_cast<std::size_t>(kTerrainDim) +
                              static_cast<std::size_t>(x)];
        }
    }
    const auto height_at = [&](int xi, int yi) -> float {
        xi = clamp_idx(xi, kTerrainDim);
        yi = clamp_idx(yi, kTerrainDim);
        return h[static_cast<std::size_t>(yi) * static_cast<std::size_t>(kTerrainDim) +
                 static_cast<std::size_t>(xi)];
    };
    for (int y = 0; y < kTerrainDim; ++y) {
        for (int x = 0; x < kTerrainDim; ++x) {
            const float hL = height_at(x - 1, y);
            const float hR = height_at(x + 1, y);
            const float hD = height_at(x, y - 1);
            const float hU = height_at(x, y + 1);
            const float dzdx = (hR - hL) / (2.f * step);
            const float dzdy = (hU - hD) / (2.f * step);
            float nx = -dzdx;
            float ny = -dzdy;
            float nz = 1.f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f) {
                nx /= len;
                ny /= len;
                nz /= len;
            }
            auto& v = mesh.vertices[static_cast<std::size_t>(y * kTerrainDim + x)];
            v.normal[0] = nx;
            v.normal[1] = ny;
            v.normal[2] = nz;
        }
    }
}

std::filesystem::path ground_layer_mask_path(const std::filesystem::path& map_path, const int layer_index);
std::filesystem::path bake_ground_layer_paint_bmp(const std::filesystem::path& map_path,
                                                  const std::vector<map::GroundTex>& ground_textures,
                                                  const std::filesystem::path& omsi_root, float size_meters,
                                                  int out_px);
bool tile_has_omsi_ground_masks(const std::filesystem::path& map_path, int max_layer_exclusive);

std::optional<LocalTerrainTile> build_local_terrain_tile(const std::filesystem::path& map_path,
                                                         const map::GroundTex& groundtex,
                                                         const std::filesystem::path& omsi_root,
                                                         f4f23ma::f32 size_meters,
                                                         std::span<const map::GroundTex> ground_textures) {
    auto heights = load_terrain_heights(map_path);
    if (!heights) {
        log::warn("terrain missing: " + map_path.string() + ".terrain");
        return std::nullopt;
    }

    if (size_meters < 0.01f) size_meters = map::kStandardTileMeters;

    LocalTerrainTile out{};
    out.heights = std::move(*heights);
    out.size_meters = size_meters;
    out.step_meters = size_meters / static_cast<f4f23ma::f32>(kTerrainDim - 1);

    std::vector<map::GroundTex> tex_list;
    if (!ground_textures.empty()) {
        tex_list.assign(ground_textures.begin(), ground_textures.end());
    } else {
        tex_list.push_back(groundtex);
    }
    const int max_layer = static_cast<int>(tex_list.size());
    const auto paint_bmp = ground_paint_bmp_path(map_path);
    const bool has_masks = tile_has_omsi_ground_masks(map_path, max_layer);
    if (has_masks) {
        bool stale = !omsi::vfs::exists(paint_bmp);
        if (!stale) {
            std::error_code ec;
            const auto paint_t = std::filesystem::last_write_time(paint_bmp, ec);
            for (int li = 1; li < max_layer && !stale; ++li) {
                const auto mp = ground_layer_mask_path(map_path, li);
                if (!omsi::vfs::exists(mp)) continue;
                const auto mt = std::filesystem::last_write_time(mp, ec);
                if (!ec && mt > paint_t) stale = true;
            }
        }
        if (stale) {
            (void)bake_ground_layer_paint_bmp(map_path, tex_list, omsi_root, size_meters, 512);
        }
        if (omsi::vfs::exists(paint_bmp)) out.texture_path = paint_bmp;
    } else if (omsi::vfs::exists(paint_bmp)) {
        out.texture_path = paint_bmp;
    }
    if (out.texture_path.empty()) {
        out.texture_path = resolve_ground_texture(omsi_root, map_path.parent_path(), groundtex.diffuse);
    }
    const bool use_paint = !out.texture_path.empty() &&
                           out.texture_path.filename().string().find("ground_paint") != std::string::npos;
    const float tex_scale = groundtex.scale > 0.01f ? groundtex.scale : 60.f;
    const auto tex_name = out.texture_path.filename().string();
    // Painted composite / aerial cover the whole tile → UV 0..1; else repeat by scale.
    const bool aerial_tex = use_paint || tex_name.find(".aerial.") != std::string::npos;
    const float* h = out.heights.data();
    const float step = out.step_meters;
    const float uv_den = aerial_tex ? static_cast<float>(kTerrainDim - 1) : tex_scale;

    // Z-up same as .map [object]: world (x,y,height). File heights[y][x] → index y*dim+x.
    // (Blender WIP terrain verts swap XY; we keep objects/terrain consistent instead.)
    out.mesh.vertices.resize(static_cast<std::size_t>(kTerrainDim * kTerrainDim));
    for (int y = 0; y < kTerrainDim; ++y) {
        for (int x = 0; x < kTerrainDim; ++x) {
            const float height = h[static_cast<std::size_t>(y) * static_cast<std::size_t>(kTerrainDim) +
                                   static_cast<std::size_t>(x)];
            out.max_height = (std::max)(out.max_height, height);

            o3d::Vertex v{};
            v.position[0] = static_cast<float>(x) * step;
            v.position[1] = static_cast<float>(y) * step;
            v.position[2] = height;
            v.normal[0] = 0.f;
            v.normal[1] = 0.f;
            v.normal[2] = 1.f;
            if (aerial_tex) {
                // Aerial JPEG is north-up (V=0 = north). Tile local y=0 is south → flip V.
                // VIEW_MIRROR_X flips the view, not mesh UVs — keep U = west→east.
                v.uv[0] = static_cast<float>(x) / uv_den;
                v.uv[1] = 1.f - static_cast<float>(y) / uv_den;
            } else {
                v.uv[0] = v.position[0] / tex_scale;
                v.uv[1] = v.position[1] / tex_scale;
            }
            out.mesh.vertices[static_cast<std::size_t>(y * kTerrainDim + x)] = v;
        }
    }

    const auto height_at = [&](int xi, int yi) -> float {
        xi = clamp_idx(xi, kTerrainDim);
        yi = clamp_idx(yi, kTerrainDim);
        return h[static_cast<std::size_t>(yi) * static_cast<std::size_t>(kTerrainDim) + static_cast<std::size_t>(xi)];
    };
    for (int y = 0; y < kTerrainDim; ++y) {
        for (int x = 0; x < kTerrainDim; ++x) {
            const float hL = height_at(x - 1, y);
            const float hR = height_at(x + 1, y);
            const float hD = height_at(x, y - 1);
            const float hU = height_at(x, y + 1);
            const float dzdx = (hR - hL) / (2.f * step);
            const float dzdy = (hU - hD) / (2.f * step);
            float nx = -dzdx;
            float ny = -dzdy;
            float nz = 1.f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f) {
                nx /= len;
                ny /= len;
                nz /= len;
            }
            auto& v = out.mesh.vertices[static_cast<std::size_t>(y * kTerrainDim + x)];
            v.normal[0] = nx;
            v.normal[1] = ny;
            v.normal[2] = nz;
        }
    }

    out.mesh.indices.reserve(static_cast<std::size_t>((kTerrainDim - 1) * (kTerrainDim - 1) * 6));
    for (int y = 0; y < kTerrainDim - 1; ++y) {
        for (int x = 0; x < kTerrainDim - 1; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(y * kTerrainDim + x);
            const std::uint32_t i1 = static_cast<std::uint32_t>(y * kTerrainDim + x + 1);
            const std::uint32_t i2 = static_cast<std::uint32_t>((y + 1) * kTerrainDim + x + 1);
            const std::uint32_t i3 = static_cast<std::uint32_t>((y + 1) * kTerrainDim + x);
            out.mesh.indices.push_back(i0);
            out.mesh.indices.push_back(i1);
            out.mesh.indices.push_back(i2);
            out.mesh.indices.push_back(i0);
            out.mesh.indices.push_back(i2);
            out.mesh.indices.push_back(i3);
        }
    }

    static const bool terrain_verbose = [] {
        const char* e = std::getenv("OMSI_TERRAIN_VERBOSE");
        return e && e[0] && e[0] != '0';
    }();
    if (terrain_verbose) {
        log::info("local terrain verts=" + std::to_string(out.mesh.vertices.size()) +
                  " tris=" + std::to_string(out.mesh.indices.size() / 3) +
                  " size_m=" + std::to_string(out.size_meters) + " step=" + std::to_string(out.step_meters) +
                  " max_h=" + std::to_string(out.max_height) + " tex=" + out.texture_path.string());
    }
    return out;
}

std::vector<float> make_flat_terrain_heights(const float height) {
    return std::vector<float>(static_cast<std::size_t>(kTerrainDim * kTerrainDim), height);
}

bool paint_terrain_brush(std::vector<float>& heights, const float size_meters, const float local_x,
                         const float local_y, const float radius_m, const TerrainBrushMode mode,
                         const float amount, const float set_height) {
    if (heights.size() < static_cast<std::size_t>(kTerrainDim * kTerrainDim)) return false;
    const float size = size_meters > 0.01f ? size_meters : map::kStandardTileMeters;
    const float step = size / static_cast<float>(kTerrainDim - 1);
    const float brush_r = std::fmax(step * 0.5f, radius_m);
    const float brush_r2 = brush_r * brush_r;
    bool changed = false;
    for (int yi = 0; yi < kTerrainDim; ++yi) {
        for (int xi = 0; xi < kTerrainDim; ++xi) {
            const float px = static_cast<float>(xi) * step;
            const float py = static_cast<float>(yi) * step;
            const float dx = px - local_x;
            const float dy = py - local_y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > brush_r2) continue;
            const float t = 1.f - std::sqrt(d2) / brush_r; // 1 at center → 0 at edge
            const float w = t * t;
            float& h = heights[static_cast<std::size_t>(yi) * static_cast<std::size_t>(kTerrainDim) +
                               static_cast<std::size_t>(xi)];
            const float before = h;
            if (mode == TerrainBrushMode::Higher) h += amount * w;
            else if (mode == TerrainBrushMode::Lower) h -= amount * w;
            else {
                // SetHeight: strong blend so one stroke actually reaches the target
                const float k = std::fmin(1.f, w * 0.85f);
                h = h * (1.f - k) + set_height * k;
            }
            if (h != before) changed = true;
        }
    }
    return changed;
}

std::filesystem::path ground_layer_mask_path(const std::filesystem::path& map_path, const int layer_index) {
    // Unit126 TMapKachel.GetGroundTexPath: <mapdir>\texture\map\<tile.map>.<N>.dds
    return map_path.parent_path() / "texture" / "map" /
           (map_path.filename().string() + "." + std::to_string(layer_index) + ".dds");
}

bool tile_has_omsi_ground_masks(const std::filesystem::path& map_path, const int max_layer_exclusive) {
    const int hi = (std::max)(2, max_layer_exclusive);
    for (int li = 1; li < hi; ++li) {
        if (omsi::vfs::exists(ground_layer_mask_path(map_path, li))) return true;
    }
    // Also scan a few extra indices (maps often skip layer numbers).
    for (int li = hi; li <= hi + 8; ++li) {
        if (omsi::vfs::exists(ground_layer_mask_path(map_path, li))) return true;
    }
    return false;
}

std::filesystem::path ground_paint_bmp_path(const std::filesystem::path& map_path) {
    return std::filesystem::path(map_path.string() + ".ground_paint.bmp");
}

namespace {

bool load_file_bgr(const std::filesystem::path& path, std::vector<std::uint8_t>& bgr, int& w, int& h) {
    bgr.clear();
    w = h = 0;
    if (path.empty() || !omsi::vfs::exists(path)) return false;
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    IWICBitmapDecoder* dec = nullptr;
    bool ok = false;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnLoad, &dec))) {
        IWICBitmapFrameDecode* frame = nullptr;
        if (SUCCEEDED(dec->GetFrame(0, &frame))) {
            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(factory->CreateFormatConverter(&conv))) {
                if (SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr,
                                               0.f, WICBitmapPaletteTypeCustom))) {
                    UINT fw = 0, fh = 0;
                    conv->GetSize(&fw, &fh);
                    bgr.resize(static_cast<std::size_t>(fw) * static_cast<std::size_t>(fh) * 3);
                    if (SUCCEEDED(conv->CopyPixels(nullptr, fw * 3, static_cast<UINT>(bgr.size()), bgr.data()))) {
                        w = static_cast<int>(fw);
                        h = static_cast<int>(fh);
                        ok = true;
                    }
                }
                conv->Release();
            }
            frame->Release();
        }
        dec->Release();
    }
    factory->Release();
    return ok;
}

// OMSI ground masks: uncompressed 8-bit ALPHA DDS (DDPF_ALPHA, Berlin = 256×256).
bool load_mask_a8(const std::filesystem::path& path, std::vector<std::uint8_t>& a8, int& w, int& h) {
    a8.clear();
    w = h = 0;
    if (path.empty() || !omsi::vfs::exists(path)) return false;
    std::istringstream ram_in;
    std::ifstream file_in;
    std::istream* in_p = &file_in;
    if (const auto ram = omsi::vfs::get(path)) {
        ram_in.str(*ram);
        in_p = &ram_in;
    } else {
        file_in.open(path, std::ios::binary);
    }
    std::istream& in = *in_p;
    if (!in) return false;
    char magic[4]{};
    in.read(magic, 4);
    if (std::memcmp(magic, "DDS ", 4) != 0) return false;
    std::uint32_t hdr[31]{};
    in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (!in) return false;
    // DDS_HEADER layout after magic: dwSize, dwFlags, dwHeight, dwWidth, ...
    h = static_cast<int>(hdr[2]);
    w = static_cast<int>(hdr[3]);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    const std::uint32_t pf_flags = hdr[19]; // ddspf.dwFlags
    const std::uint32_t rgb_bitcount = hdr[21];
    a8.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    if ((pf_flags & 0x2u) != 0 && rgb_bitcount == 8) {
        // DDPF_ALPHA — raw A8
        in.read(reinterpret_cast<char*>(a8.data()), static_cast<std::streamsize>(a8.size()));
        return static_cast<bool>(in);
    }
    if ((pf_flags & 0x40u) != 0 && rgb_bitcount == 32) {
        // A8R8G8B8 — take alpha
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        in.read(reinterpret_cast<char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
        if (!in) return false;
        for (std::size_t i = 0; i < a8.size(); ++i) a8[i] = bgra[i * 4 + 3];
        return true;
    }
    if ((pf_flags & 0x20000u) != 0 && rgb_bitcount == 8) {
        // DDPF_LUMINANCE
        in.read(reinterpret_cast<char*>(a8.data()), static_cast<std::streamsize>(a8.size()));
        return static_cast<bool>(in);
    }
    return false;
}

bool write_mask_a8_dds(const std::filesystem::path& path, const int w, const int h,
                       const std::vector<std::uint8_t>& a8) {
    if (w <= 0 || h <= 0 || static_cast<int>(a8.size()) < w * h) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write("DDS ", 4);
    std::uint32_t hdr[31]{};
    hdr[0] = 124;                                  // dwSize
    hdr[1] = 0x1007;                               // CAPS|HEIGHT|WIDTH|PIXELFORMAT
    hdr[2] = static_cast<std::uint32_t>(h);
    hdr[3] = static_cast<std::uint32_t>(w);
    hdr[4] = static_cast<std::uint32_t>(w); // pitch
    hdr[18] = 32;                                  // ddspf.dwSize
    hdr[19] = 0x2;                                 // DDPF_ALPHA
    hdr[21] = 8;                                   // bitcount
    hdr[25] = 0xFF;                                // dwABitMask
    hdr[26] = 0x1000;                              // DDSCAPS_TEXTURE
    out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(a8.data()), static_cast<std::streamsize>(w * h));
    return static_cast<bool>(out);
}

bool write_bmp24(const std::filesystem::path& path, const int w, const int h, const std::vector<std::uint8_t>& bgr) {
    if (w <= 0 || h <= 0 || static_cast<int>(bgr.size()) < w * h * 3) return false;
    const std::size_t row_bytes = static_cast<std::size_t>((w * 3 + 3) & ~3);
    std::vector<std::uint8_t> padded(row_bytes * static_cast<std::size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        const int src_y = h - 1 - y;
        std::memcpy(padded.data() + static_cast<std::size_t>(y) * row_bytes,
                    bgr.data() + static_cast<std::size_t>(src_y) * static_cast<std::size_t>(w) * 3,
                    static_cast<std::size_t>(w) * 3);
    }
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::uint32_t file_size = static_cast<std::uint32_t>(54 + padded.size());
    const std::uint32_t data_offset = 54;
    const std::uint32_t dib = 40;
    const std::uint16_t planes = 1;
    const std::uint16_t bpp = 24;
    const std::uint32_t zero = 0;
    out.put('B');
    out.put('M');
    out.write(reinterpret_cast<const char*>(&file_size), 4);
    out.write(reinterpret_cast<const char*>(&zero), 4);
    out.write(reinterpret_cast<const char*>(&data_offset), 4);
    out.write(reinterpret_cast<const char*>(&dib), 4);
    out.write(reinterpret_cast<const char*>(&w), 4);
    out.write(reinterpret_cast<const char*>(&h), 4);
    out.write(reinterpret_cast<const char*>(&planes), 2);
    out.write(reinterpret_cast<const char*>(&bpp), 2);
    out.write(reinterpret_cast<const char*>(&zero), 4);
    const std::uint32_t img_sz = static_cast<std::uint32_t>(padded.size());
    out.write(reinterpret_cast<const char*>(&img_sz), 4);
    const std::int32_t ppm = 2835;
    out.write(reinterpret_cast<const char*>(&ppm), 4);
    out.write(reinterpret_cast<const char*>(&ppm), 4);
    out.write(reinterpret_cast<const char*>(&zero), 4);
    out.write(reinterpret_cast<const char*>(&zero), 4);
    out.write(reinterpret_cast<const char*>(padded.data()), static_cast<std::streamsize>(padded.size()));
    return static_cast<bool>(out);
}

} // namespace

bool paint_omsi_ground_layer_mask(const std::filesystem::path& map_path, const int layer_index,
                                  const float size_meters, const float local_x, const float local_y,
                                  const float radius_m, const bool rubber, const int mask_px,
                                  const float hardness, const float strength) {
    if (map_path.empty() || layer_index < 1 || mask_px < 8) return false;
    const auto path = ground_layer_mask_path(map_path, layer_index);
    std::vector<std::uint8_t> a8;
    int w = 0, h = 0;
    if (!load_mask_a8(path, a8, w, h) || w != mask_px || h != mask_px) {
        w = h = mask_px;
        a8.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    }
    const float size = size_meters > 0.01f ? size_meters : map::kStandardTileMeters;
    const float brush_r = std::fmax(size / static_cast<float>(w) * 0.5f, radius_m);
    const float brush_r2 = brush_r * brush_r;
    const std::uint8_t target = rubber ? static_cast<std::uint8_t>(0) : static_cast<std::uint8_t>(255);
    // ground_paint mesh uses UV v = 1 - y/size (north-up). Mask row 0 = north = high local_y.
    const float paint_y = size - local_y;
    const float hard = std::clamp(hardness, 0.f, 1.f);
    const float str = std::clamp(strength, 0.f, 1.f);
    // hardness: 0 = soft falloff (power 2), 1 = hard edge (almost step)
    const float falloff_pow = 1.f + (1.f - hard) * 2.f; // hard=1 -> pow1, hard=0 -> pow3
    bool changed = false;
    for (int yi = 0; yi < h; ++yi) {
        for (int xi = 0; xi < w; ++xi) {
            const float wx = (static_cast<float>(xi) + 0.5f) / static_cast<float>(w) * size;
            const float wy = (static_cast<float>(yi) + 0.5f) / static_cast<float>(h) * size;
            const float dx = wx - local_x;
            const float dy = wy - paint_y;
            if (dx * dx + dy * dy > brush_r2) continue;
            float t = 1.f - std::sqrt(dx * dx + dy * dy) / brush_r;
            if (t < 0.02f) continue;
            if (hard > 0.85f) {
                // hard edge: keep full strength until near rim
                const float rim = 1.f - hard * 0.85f;
                t = t > rim ? 1.f : (t / rim);
            }
            t = std::pow(std::clamp(t, 0.f, 1.f), falloff_pow) * str;
            if (t < 0.01f) continue;
            auto& cell = a8[static_cast<std::size_t>(yi) * static_cast<std::size_t>(w) + static_cast<std::size_t>(xi)];
            std::uint8_t nv = cell;
            if (rubber) {
                nv = static_cast<std::uint8_t>(std::lround(static_cast<float>(cell) * (1.f - t)));
            } else {
                nv = static_cast<std::uint8_t>(
                    std::lround(static_cast<float>(cell) + (static_cast<float>(target) - static_cast<float>(cell)) * t));
            }
            if (nv != cell) {
                cell = nv;
                changed = true;
            }
        }
    }
    if (!changed) return false;
    return write_mask_a8_dds(path, w, h, a8);
}

bool paint_omsi_ground_layer_mask_polygon(const std::filesystem::path& map_path, const int layer_index,
                                          const float size_meters,
                                          const std::vector<std::pair<float, float>>& pts_local, const bool rubber,
                                          const int mask_px) {
    if (map_path.empty() || layer_index < 1 || mask_px < 8 || pts_local.size() < 3) return false;
    const auto path = ground_layer_mask_path(map_path, layer_index);
    std::vector<std::uint8_t> a8;
    int w = 0, h = 0;
    if (!load_mask_a8(path, a8, w, h) || w != mask_px || h != mask_px) {
        w = h = mask_px;
        a8.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    }
    const float size = size_meters > 0.01f ? size_meters : map::kStandardTileMeters;
    const std::uint8_t target = rubber ? static_cast<std::uint8_t>(0) : static_cast<std::uint8_t>(255);
    // Polygon in mask space (mask y = size - local_y, same convention as the brush).
    std::vector<std::pair<float, float>> poly;
    poly.reserve(pts_local.size());
    for (const auto& p : pts_local) poly.emplace_back(p.first, size - p.second);
    auto inside = [&](float px, float py) {
        bool in = false;
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const float xi = poly[i].first, yi = poly[i].second;
            const float xj = poly[j].first, yj = poly[j].second;
            if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) in = !in;
        }
        return in;
    };
    bool changed = false;
    for (int yi = 0; yi < h; ++yi) {
        for (int xi = 0; xi < w; ++xi) {
            const float wx = (static_cast<float>(xi) + 0.5f) / static_cast<float>(w) * size;
            const float wy = (static_cast<float>(yi) + 0.5f) / static_cast<float>(h) * size;
            if (!inside(wx, wy)) continue;
            auto& cell = a8[static_cast<std::size_t>(yi) * static_cast<std::size_t>(w) + static_cast<std::size_t>(xi)];
            if (cell != target) {
                cell = target;
                changed = true;
            }
        }
    }
    if (!changed) return false;
    return write_mask_a8_dds(path, w, h, a8);
}

std::filesystem::path bake_ground_layer_paint_bmp(const std::filesystem::path& map_path,
                                                  const std::vector<map::GroundTex>& ground_textures,
                                                  const std::filesystem::path& omsi_root, const float size_meters,
                                                  const int out_px) {
    if (map_path.empty() || ground_textures.empty() || out_px < 64) return {};
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const float size = size_meters > 0.01f ? size_meters : map::kStandardTileMeters;

    struct LayerImg {
        std::vector<std::uint8_t> bgr;
        int w = 0, h = 0;
        float scale = 60.f;
        bool ok = false;
    };
    std::vector<LayerImg> imgs(ground_textures.size());
    for (std::size_t i = 0; i < ground_textures.size(); ++i) {
        const auto path =
            resolve_ground_texture(omsi_root, map_path.parent_path(), ground_textures[i].diffuse);
        imgs[i].scale = ground_textures[i].scale > 0.01f ? ground_textures[i].scale : 60.f;
        imgs[i].ok = load_file_bgr(path, imgs[i].bgr, imgs[i].w, imgs[i].h);
    }
    if (!imgs[0].ok) {
        imgs[0].w = imgs[0].h = 4;
        imgs[0].bgr.assign(48, 90);
        imgs[0].ok = true;
    }

    struct MaskImg {
        std::vector<std::uint8_t> a8;
        int w = 0, h = 0;
        bool ok = false;
    };
    std::vector<MaskImg> masks(ground_textures.size());
    for (std::size_t i = 1; i < ground_textures.size(); ++i) {
        masks[i].ok = load_mask_a8(ground_layer_mask_path(map_path, static_cast<int>(i)), masks[i].a8, masks[i].w,
                                   masks[i].h);
    }
    // Layers beyond groundtex count (skipped indices) — still composite if mask exists.
    for (int li = static_cast<int>(ground_textures.size()); li < static_cast<int>(ground_textures.size()) + 8; ++li) {
        MaskImg m{};
        if (!load_mask_a8(ground_layer_mask_path(map_path, li), m.a8, m.w, m.h)) continue;
        masks.push_back(std::move(m));
        imgs.push_back(imgs.back()); // reuse last diffuse if unknown
    }

    auto sample_layer = [&](int li, float wx, float wy, std::uint8_t& b, std::uint8_t& g, std::uint8_t& r) {
        if (li < 0 || li >= static_cast<int>(imgs.size()) || !imgs[static_cast<std::size_t>(li)].ok) li = 0;
        const auto& im = imgs[static_cast<std::size_t>(li)];
        float u = std::fmod(wx / im.scale, 1.f);
        float v = std::fmod(wy / im.scale, 1.f);
        if (u < 0.f) u += 1.f;
        if (v < 0.f) v += 1.f;
        int sx = static_cast<int>(u * static_cast<float>(im.w));
        int sy = static_cast<int>(v * static_cast<float>(im.h));
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx >= im.w) sx = im.w - 1;
        if (sy >= im.h) sy = im.h - 1;
        const std::size_t i =
            (static_cast<std::size_t>(sy) * static_cast<std::size_t>(im.w) + static_cast<std::size_t>(sx)) * 3;
        b = im.bgr[i];
        g = im.bgr[i + 1];
        r = im.bgr[i + 2];
    };
    auto sample_mask = [&](int li, float u, float v) -> float {
        if (li < 0 || li >= static_cast<int>(masks.size()) || !masks[static_cast<std::size_t>(li)].ok) return 0.f;
        const auto& m = masks[static_cast<std::size_t>(li)];
        int sx = static_cast<int>(u * static_cast<float>(m.w));
        int sy = static_cast<int>(v * static_cast<float>(m.h));
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx >= m.w) sx = m.w - 1;
        if (sy >= m.h) sy = m.h - 1;
        return static_cast<float>(
                   m.a8[static_cast<std::size_t>(sy) * static_cast<std::size_t>(m.w) + static_cast<std::size_t>(sx)]) /
               255.f;
    };

    std::vector<std::uint8_t> out(static_cast<std::size_t>(out_px) * static_cast<std::size_t>(out_px) * 3);
    for (int y = 0; y < out_px; ++y) {
        for (int x = 0; x < out_px; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(out_px);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(out_px);
            const float wx = u * size;
            const float wy = v * size;
            std::uint8_t b = 0, g = 0, r = 0;
            sample_layer(0, wx, wy, b, g, r);
            for (int li = 1; li < static_cast<int>(masks.size()); ++li) {
                const float a = sample_mask(li, u, v);
                if (a < 0.02f) continue;
                std::uint8_t lb = 0, lg = 0, lr = 0;
                sample_layer(li < static_cast<int>(imgs.size()) ? li : 0, wx, wy, lb, lg, lr);
                b = static_cast<std::uint8_t>(std::lround(static_cast<float>(b) * (1.f - a) + static_cast<float>(lb) * a));
                g = static_cast<std::uint8_t>(std::lround(static_cast<float>(g) * (1.f - a) + static_cast<float>(lg) * a));
                r = static_cast<std::uint8_t>(std::lround(static_cast<float>(r) * (1.f - a) + static_cast<float>(lr) * a));
            }
            const std::size_t o =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(out_px) + static_cast<std::size_t>(x)) * 3;
            out[o] = b;
            out[o + 1] = g;
            out[o + 2] = r;
        }
    }

    const auto out_path = ground_paint_bmp_path(map_path);
    if (!write_bmp24(out_path, out_px, out_px, out)) return {};
    log::info("baked ground paint " + out_path.filename().string() + " from OMSI masks");
    return out_path;
}

} // namespace omsi::editor
