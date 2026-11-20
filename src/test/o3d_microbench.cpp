// Isolated O3D load/render bench — no map editor, no Ultralight.
// Compares the editor CPU path vs a zero-copy mmap path on the heaviest .o3d.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "o3d/o3d_loader.hpp"
#include "omsi/o3d_format.hpp"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

double ms_since(const clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

struct TexInfo {
    std::string name;
    std::string path;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::uint64_t bytes = 0;
    bool found = false;
};

std::optional<std::pair<std::uint32_t, std::uint32_t>> peek_tex_wh(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    unsigned char hdr[32]{};
    f.read(reinterpret_cast<char*>(hdr), 32);
    const auto n = static_cast<std::size_t>(f.gcount());
    if (n >= 20 && std::memcmp(hdr, "DDS ", 4) == 0) {
        std::uint32_t h = 0, w = 0;
        std::memcpy(&h, hdr + 12, 4);
        std::memcpy(&w, hdr + 16, 4);
        if (w && h) return std::pair{w, h};
    }
    if (n >= 24 && hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G') {
        const std::uint32_t w = (std::uint32_t(hdr[16]) << 24) | (std::uint32_t(hdr[17]) << 16) |
                                (std::uint32_t(hdr[18]) << 8) | hdr[19];
        const std::uint32_t h = (std::uint32_t(hdr[20]) << 24) | (std::uint32_t(hdr[21]) << 16) |
                                (std::uint32_t(hdr[22]) << 8) | hdr[23];
        if (w && h) return std::pair{w, h};
    }
    if (n >= 26 && hdr[0] == 'B' && hdr[1] == 'M') {
        std::int32_t w = 0, h = 0;
        std::memcpy(&w, hdr + 18, 4);
        std::memcpy(&h, hdr + 22, 4);
        if (w > 0 && h != 0) return std::pair{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(std::abs(h))};
    }
    if (n >= 18) {
        const std::uint16_t w = static_cast<std::uint16_t>(hdr[12] | (hdr[13] << 8));
        const std::uint16_t h = static_cast<std::uint16_t>(hdr[14] | (hdr[15] << 8));
        if (w >= 2 && h >= 2 && w <= 16384 && h <= 16384) return std::pair{w, h};
    }
    return std::nullopt;
}

void expand_indices(const omsi::o3d::NativeMesh& mesh, bool flip, std::vector<std::uint32_t>& out) {
    out.clear();
    out.reserve(static_cast<std::size_t>(mesh.triangle_count) * 3u);
    if (mesh.long_triangle_indices) {
        const auto tris = mesh.triangle_u32_span();
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            out.push_back(tris[t].i0);
            out.push_back(flip ? tris[t].i2 : tris[t].i1);
            out.push_back(flip ? tris[t].i1 : tris[t].i2);
        }
    } else {
        const auto tris = mesh.triangle_u16_span();
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            out.push_back(tris[t].i0);
            out.push_back(flip ? tris[t].i2 : tris[t].i1);
            out.push_back(flip ? tris[t].i1 : tris[t].i2);
        }
    }
}

// A — current editor: always copy+decrypt, then per-vert matrix, bbox, pick clone.
double bench_editor_now(const omsi::o3d::NativeMesh& mesh, int iters) {
    const bool flip = omsi::o3d::o3d_draw_matrix_flips_winding(mesh);
    double sum = 0.0;
    for (int i = 0; i < iters; ++i) {
        const auto t0 = clock::now();
        const auto decrypted = omsi::o3d::decrypt_o3d_vertices(mesh);
        std::vector<omsi::o3d::Vertex> gpu;
        gpu.reserve(decrypted.size());
        for (const auto& v : decrypted) gpu.push_back(omsi::o3d::prepare_o3d_vertex_for_gpu(v, mesh));
        float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};
        if (!gpu.empty()) {
            bmin[0] = bmax[0] = gpu[0].position[0];
            bmin[1] = bmax[1] = gpu[0].position[1];
            bmin[2] = bmax[2] = gpu[0].position[2];
            for (const auto& gv : gpu) {
                for (int c = 0; c < 3; ++c) {
                    bmin[c] = (std::min)(bmin[c], gv.position[c]);
                    bmax[c] = (std::max)(bmax[c], gv.position[c]);
                }
            }
        }
        std::vector<omsi::o3d::Vertex> pick = gpu;
        std::vector<std::uint32_t> idx;
        expand_indices(mesh, flip, idx);
        (void)bmin;
        (void)bmax;
        (void)pick;
        sum += ms_since(t0);
    }
    return sum / iters;
}

// B — one buffer: decrypt only if encrypted; transform only if 0x79; bbox in same pass; no pick clone.
double bench_onepass(const omsi::o3d::NativeMesh& mesh, int iters) {
    const bool flip = omsi::o3d::o3d_draw_matrix_flips_winding(mesh);
    const bool need_xform = omsi::o3d::mesh_has_transform_chunk(mesh);
    double sum = 0.0;
    for (int i = 0; i < iters; ++i) {
        const auto t0 = clock::now();
        std::vector<omsi::o3d::Vertex> gpu;
        if (mesh.encrypted && mesh.encryption_key != omsi::o3d::kNoEncryption &&
            mesh.encryption_key != omsi::o3d::kEncryptionNoProduct) {
            gpu = omsi::o3d::decrypt_o3d_vertices(mesh);
        } else {
            const auto src = mesh.vertex_span();
            gpu.assign(src.begin(), src.end());
        }
        float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};
        if (!gpu.empty()) {
            bmin[0] = bmax[0] = gpu[0].position[0];
            bmin[1] = bmax[1] = gpu[0].position[1];
            bmin[2] = bmax[2] = gpu[0].position[2];
        }
        for (auto& v : gpu) {
            if (need_xform) v = omsi::o3d::prepare_o3d_vertex_for_gpu(v, mesh);
            for (int c = 0; c < 3; ++c) {
                bmin[c] = (std::min)(bmin[c], v.position[c]);
                bmax[c] = (std::max)(bmax[c], v.position[c]);
            }
        }
        std::vector<std::uint32_t> idx;
        expand_indices(mesh, flip, idx);
        (void)bmin;
        (void)bmax;
        sum += ms_since(t0);
    }
    return sum / iters;
}

// C — mmap span, no vertex copy when plain+no 0x79. Indices only.
double bench_mmap_indices(const omsi::o3d::NativeMesh& mesh, int iters) {
    const bool flip = omsi::o3d::o3d_draw_matrix_flips_winding(mesh);
    const bool can_alias = !mesh.encrypted && !omsi::o3d::mesh_has_transform_chunk(mesh);
    double sum = 0.0;
    for (int i = 0; i < iters; ++i) {
        const auto t0 = clock::now();
        const auto src = mesh.vertex_span();
        volatile std::size_t touch = src.size_bytes();
        if (!can_alias) {
            auto gpu = omsi::o3d::decrypt_o3d_vertices(mesh);
            if (omsi::o3d::mesh_has_transform_chunk(mesh)) {
                for (auto& v : gpu) v = omsi::o3d::prepare_o3d_vertex_for_gpu(v, mesh);
            }
            touch += gpu.size();
        }
        std::vector<std::uint32_t> idx;
        expand_indices(mesh, flip, idx);
        touch += idx.size();
        (void)touch;
        sum += ms_since(t0);
    }
    return sum / iters;
}

std::vector<std::pair<std::uint64_t, fs::path>> find_heaviest(const fs::path& omsi_root, const int top_n) {
    std::vector<std::pair<std::uint64_t, fs::path>> best;
    const fs::path roots[] = {omsi_root / "Vehicles", omsi_root / "Sceneryobjects"};
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() != ".o3d" && it->path().extension() != ".O3D") continue;
            const auto sz = it->file_size(ec);
            if (ec || sz == 0) continue;
            best.push_back({sz, it->path()});
        }
    }
    std::sort(best.begin(), best.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if (static_cast<int>(best.size()) > top_n) best.resize(static_cast<std::size_t>(top_n));
    return best;
}

int bench_file(const fs::path& o3d_path, const int iters, const bool do_gpu) {
    const auto t_mmap0 = clock::now();
    omsi::o3d::MappedO3D mapped{o3d_path};
    const double mmap_ms = ms_since(t_mmap0);
    if (!mapped.valid()) {
        std::fprintf(stderr, "invalid o3d: %s\n", o3d_path.string().c_str());
        return 2;
    }
    const auto& mesh = mapped.mesh();
    const auto mats = mesh.parse_materials();

    std::printf("file=%s\n", o3d_path.string().c_str());
    std::printf("bytes=%zu mmap_ms=%.3f encrypted=%d has_0x79=%d long_idx=%d\n", mapped.bytes().size(), mmap_ms,
                mesh.encrypted ? 1 : 0, omsi::o3d::mesh_has_transform_chunk(mesh) ? 1 : 0,
                mesh.long_triangle_indices ? 1 : 0);
    std::printf("verts=%u tris=%u materials=%u (submeshes)\n", mesh.vertex_count, mesh.triangle_count,
                mesh.material_count);

    std::vector<TexInfo> texs;
    std::uint64_t tex_bytes = 0;
    for (const auto& m : mats) {
        TexInfo ti{};
        ti.name = m.texture;
        if (m.texture.empty()) {
            texs.push_back(std::move(ti));
            continue;
        }
        if (const auto p = omsi::gfx::dx12::resolve_texture_path(o3d_path, m.texture)) {
            ti.found = true;
            ti.path = p->string();
            std::error_code ec;
            ti.bytes = fs::file_size(*p, ec);
            if (!ec) tex_bytes += ti.bytes;
            if (const auto wh = peek_tex_wh(*p)) {
                ti.w = wh->first;
                ti.h = wh->second;
            }
        }
        texs.push_back(std::move(ti));
    }
    std::printf("textures=%zu found=%zu disk_bytes=%llu\n", texs.size(),
                static_cast<std::size_t>(std::count_if(texs.begin(), texs.end(), [](const TexInfo& t) { return t.found; })),
                static_cast<unsigned long long>(tex_bytes));
    for (const auto& t : texs) {
        if (t.name.empty()) continue;
        std::printf("  tex \"%s\" %s %ux%u %llu B\n", t.name.c_str(), t.found ? t.path.c_str() : "(missing)", t.w, t.h,
                    static_cast<unsigned long long>(t.bytes));
    }

    {
        const auto decrypted = omsi::o3d::decrypt_o3d_vertices(mesh);
        std::vector<omsi::o3d::Vertex> old_gpu;
        old_gpu.reserve(decrypted.size());
        for (const auto& v : decrypted) old_gpu.push_back(omsi::o3d::prepare_o3d_vertex_for_gpu(v, mesh));
        std::vector<omsi::o3d::Vertex> new_gpu;
        omsi::o3d::prepare_o3d_mesh_vertices_for_gpu(mesh, new_gpu);
        bool ok = old_gpu.size() == new_gpu.size();
        const bool xform = omsi::o3d::mesh_has_transform_chunk(mesh);
        for (std::size_t i = 0; ok && i < old_gpu.size(); ++i) {
            if (xform) {
                ok = std::memcmp(&old_gpu[i], &new_gpu[i], sizeof(omsi::o3d::Vertex)) == 0;
            } else {
                ok = std::memcmp(old_gpu[i].position, new_gpu[i].position, sizeof(old_gpu[i].position)) == 0 &&
                     std::memcmp(old_gpu[i].uv, new_gpu[i].uv, sizeof(old_gpu[i].uv)) == 0;
            }
        }
        std::printf("vertex_parity=%s  verts=%zu  has_0x79=%d  decrypt=%d\n", ok ? "ok" : "FAIL", new_gpu.size(),
                    xform ? 1 : 0, omsi::o3d::o3d_needs_vertex_decrypt(mesh) ? 1 : 0);
        if (!ok) return 8;
    }

    const double a = bench_editor_now(mesh, iters);
    const double b = bench_onepass(mesh, iters);
    const double c = bench_mmap_indices(mesh, iters);
    std::printf("cpu iters=%d\n", iters);
    std::printf("  A editor_now (copy+decrypt+prepare+bbox+pick)  %.3f ms\n", a);
    std::printf("  B onepass    (decrypt/xform only if needed)    %.3f ms\n", b);
    std::printf("  C mmap_alias (no vert copy if plain)           %.3f ms\n", c);
    const double best = (std::min)(a, (std::min)(b, c));
    std::printf("winner=%s  vs_editor=%.1fx\n", best == c ? "C" : (best == b ? "B" : "A"), a / (best > 1e-6 ? best : 1e-6));

    if (!do_gpu) return 0;

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 5;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(320, 240, "o3d microbench", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        return 5;
    }
    omsi::gfx::dx12::HelloO3dRenderer renderer{};
    const auto t_init = clock::now();
    if (!renderer.initialize(glfwGetWin32Window(window), 320, 240)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        std::fprintf(stderr, "dx12 init failed\n");
        return 7;
    }
    const double init_ms = ms_since(t_init);
    const auto t_up = clock::now();
    if (!renderer.load_o3d(mapped)) {
        renderer.shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        std::fprintf(stderr, "load_o3d GPU failed\n");
        return 7;
    }
    const double upload_ms = ms_since(t_up);
    float wvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    renderer.set_world_view_proj(wvp);
    const auto t_draw = clock::now();
    renderer.render();
    const double draw_ms = ms_since(t_draw);
    std::printf("gpu dx12_init=%.3f ms  load_o3d(editor path)=%.3f ms  first_present=%.3f ms  chunks=%zu\n",
                init_ms, upload_ms, draw_ms, renderer.chunk_count());
    renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

void usage() {
    std::fprintf(stderr,
                 "omsi_o3d_microbench [--find [omsi_root]] [--top N] [--iters N] [--gpu] [file.o3d]\n"
                 "  --find   scan Vehicles+Sceneryobjects for heaviest .o3d\n"
                 "  --gpu    hidden 320x240 DX12 upload + 1 present (editor renderer)\n");
}

} // namespace

int main(int argc, char** argv) {
    fs::path omsi_root = "C:/Program Files (x86)/Steam/steamapps/common/OMSI 2";
    fs::path file;
    int iters = 20;
    int top_n = 8;
    bool do_find = false;
    bool do_gpu = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--find") {
            do_find = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') omsi_root = argv[++i];
        } else if (a == "--top" && i + 1 < argc) {
            top_n = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--iters" && i + 1 < argc) {
            iters = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--gpu") {
            do_gpu = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a[0] != '-') {
            file = a;
        }
    }

    if (do_find) {
        std::printf("scanning %s (Vehicles + Sceneryobjects) ...\n", omsi_root.string().c_str());
        const auto t0 = clock::now();
        const auto list = find_heaviest(omsi_root, top_n);
        std::printf("scan_ms=%.1f  hits=%zu\n", ms_since(t0), list.size());
        for (const auto& [sz, p] : list)
            std::printf("  %10llu  %s\n", static_cast<unsigned long long>(sz), p.string().c_str());
        if (file.empty() && !list.empty()) file = list.front().second;
    }
    if (file.empty()) {
        usage();
        return 1;
    }
    return bench_file(file, iters, do_gpu);
}
