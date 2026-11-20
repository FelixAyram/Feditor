#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app/hello_o3d_app.hpp"
#include "f4f23ma/math.hpp"
#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "input/luna_camera.hpp"
#include "input/omsi_bus_camera.hpp"
#include "omsi/bus.hpp"
#include "omsi/cti.hpp"
#include "omsi/inputs.hpp"
#include "omsi/logger.hpp"
#include "omsi/model/mesh_animator.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/script/vehicle_runtime.hpp"
#include "omsi/script/vehicle_state_logger.hpp"
#include "omsi/vehicle_input_bindings.hpp"
#include "omsi/visible_runtime.hpp"
#include "o3d/o3d_loader.hpp"
#include "omsi/o3d_format.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::filesystem::path exe_directory() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

std::vector<std::filesystem::path> fallback_paths_for(const std::filesystem::path& requested,
                                                      const std::filesystem::path& omsi_root) {
    const auto name = requested.filename();
    if (name.empty()) return {};
    std::vector<std::filesystem::path> paths = {
        exe_directory() / name,
        exe_directory() / "assets" / name,
    };
    if (!omsi_root.empty()) {
        paths.push_back(omsi_root / "Sceneryobjects/Felix/model" / name);
        paths.push_back(omsi_root / "Sceneryobjects/felix/model" / name);
    }
    return paths;
}

std::filesystem::path normalize_cli_path(std::filesystem::path path) {
    std::string s = path.string();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (!s.empty() && s.front() == '@') s.erase(s.begin());
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
    return std::filesystem::path(s);
}

std::filesystem::path default_debug_o3d(const std::filesystem::path& omsi_root) {
    const std::vector<std::filesystem::path> candidates = {
        omsi_root / "Sceneryobjects/Felix/model/cubo orientacion ejes.o3d",
        omsi_root / "Sceneryobjects/Felix/model/9 de julio y obelisco.o3d",
        omsi_root / "Sceneryobjects/Felix/model/3.99m NAZCA LOCAL 15.o3d",
        omsi_root / "Sceneryobjects/juañ/model/247_atlas.o3d",
        omsi_root / "Vehicles/MAN_NL202/model/MAN_NL202.o3d",
        omsi_root / "Vehicles/MAN_LC_G/model/MAN_LC_G.o3d",
    };
    std::error_code ec;
    for (const auto& p : candidates)
        if (std::filesystem::exists(p, ec)) return p;
    return {};
}

std::optional<std::filesystem::path> find_o3d_by_name(const std::filesystem::path& omsi_root,
                                                        const std::string& filename) {
    if (omsi_root.empty() || filename.empty()) return std::nullopt;

#ifdef _WIN32
    const std::wstring wide_name(filename.begin(), filename.end());
    const std::vector<std::filesystem::path> roots = {omsi_root / "Sceneryobjects", omsi_root / "Vehicles"};
    std::deque<std::filesystem::path> pending;

    for (const auto& root : roots) {
        DWORD attrs = GetFileAttributesW(root.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) pending.push_back(root);
    }

    while (!pending.empty()) {
        const auto dir = pending.front();
        pending.pop_front();

        const std::wstring spec = dir.wstring() + L"\\*";
        WIN32_FIND_DATAW fd{};
        const HANDLE handle = FindFirstFileW(spec.c_str(), &fd);
        if (handle == INVALID_HANDLE_VALUE) continue;

        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::filesystem::path child = dir / fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                pending.push_back(child);
                continue;
            }
            if (_wcsicmp(fd.cFileName, wide_name.c_str()) == 0) {
                FindClose(handle);
                return child;
            }
        } while (FindNextFileW(handle, &fd));

        FindClose(handle);
    }
    return std::nullopt;
#else
    const std::vector<std::filesystem::path> roots = {omsi_root / "Sceneryobjects", omsi_root / "Vehicles"};
    std::deque<std::filesystem::path> pending;

    for (const auto& root : roots) {
        std::error_code ec;
        if (std::filesystem::exists(root, ec) && !ec) pending.push_back(root);
    }

    constexpr int kMaxDepth = 12;
    while (!pending.empty()) {
        const auto dir = pending.front();
        const int depth = static_cast<int>(std::distance(dir.begin(), dir.end())) -
                          static_cast<int>(std::distance(omsi_root.begin(), omsi_root.end()));
        pending.pop_front();

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                ec.clear();
                break;
            }

            std::error_code type_ec;
            if (entry.is_directory(type_ec) && !type_ec) {
                if (depth < kMaxDepth) pending.push_back(entry.path());
                continue;
            }
            if (entry.is_regular_file(type_ec) && !type_ec) {
                if (entry.path().filename().string() == filename) return entry.path();
            }
        }
    }
    return std::nullopt;
#endif
}

std::filesystem::path resolve_o3d_path(std::filesystem::path requested, const std::filesystem::path& omsi_root) {
    requested = normalize_cli_path(std::move(requested));
    std::error_code ec;
    const bool explicit_path = !requested.empty();

    if (!requested.empty() && std::filesystem::exists(requested, ec)) return requested;

    if (!omsi_root.empty() && !requested.empty() && requested.is_relative()) {
        const auto rel = omsi_root / requested;
        if (std::filesystem::exists(rel, ec)) return rel;
    }

    if (!omsi_root.empty() && explicit_path && !requested.filename().empty()) {
        if (auto found = find_o3d_by_name(omsi_root, requested.filename().string())) {
            omsi::log::info("debug-o3d resolved by name: " + found->string());
            return *found;
        }
        for (const auto& alt : fallback_paths_for(requested, omsi_root)) {
            if (std::filesystem::exists(alt, ec)) return alt;
        }
    }

    if (!explicit_path && !omsi_root.empty()) {
        const auto preferred = default_debug_o3d(omsi_root);
        if (!preferred.empty() && std::filesystem::exists(preferred, ec)) return preferred;
    }

    if (!omsi_root.empty()) {
        const auto filename =
            explicit_path ? requested.filename().string() : std::string("cubo orientacion ejes.o3d");
        if (!filename.empty()) {
            if (auto found = find_o3d_by_name(omsi_root, filename)) {
                omsi::log::info("debug-o3d resolved by name: " + found->string());
                return *found;
            }
        }
    }

    return requested;
}

struct MeshBounds {
    float min_x = 1e9f;
    float min_y = 1e9f;
    float min_z = 1e9f;
    float max_x = -1e9f;
    float max_y = -1e9f;
    float max_z = -1e9f;
    bool valid = false;
};

MeshBounds compute_mesh_bounds(const omsi::o3d::MappedO3D& o3d) {
    MeshBounds b{};
    const auto& mesh = o3d.mesh();
    for (const auto& v : mesh.vertex_span()) {
        const auto tv = omsi::o3d::transform_vertex_to_omsi_world(v, mesh);
        b.min_x = (std::min)(b.min_x, tv.position[0]);
        b.min_y = (std::min)(b.min_y, tv.position[1]);
        b.min_z = (std::min)(b.min_z, tv.position[2]);
        b.max_x = (std::max)(b.max_x, tv.position[0]);
        b.max_y = (std::max)(b.max_y, tv.position[1]);
        b.max_z = (std::max)(b.max_z, tv.position[2]);
        b.valid = true;
    }
    return b;
}

void log_mesh_uv_faces_textures(const omsi::o3d::MappedO3D& o3d, const std::vector<omsi::o3d::Material>& materials) {
    const auto& mesh = o3d.mesh();
    const auto verts = mesh.vertex_span();

    omsi::log::info("=== o3d mesh dump (Unity 1:1) ===");
    omsi::log::info("vertices=" + std::to_string(mesh.vertex_count) + " faces(triangles)=" +
                    std::to_string(mesh.triangle_count) + " materials=" + std::to_string(materials.size()));

    omsi::log::info("-- texturas (" + std::to_string(materials.size()) + ") --");
    for (std::size_t m = 0; m < materials.size(); ++m) {
        const auto& mat = materials[m];
        const std::string tex = mat.texture.empty() ? "(sin textura)" : mat.texture;
        omsi::log::info("  mat[" + std::to_string(m) + "] texture=\"" + tex + "\"");
    }

    constexpr std::uint32_t kMaxVertexLog = 256;
    const std::uint32_t vert_log_count = (std::min)(mesh.vertex_count, kMaxVertexLog);
    omsi::log::info("-- vertices (" + std::to_string(vert_log_count) +
                    (mesh.vertex_count > kMaxVertexLog ? " de " + std::to_string(mesh.vertex_count) : "") + ") --");
    for (std::uint32_t i = 0; i < vert_log_count; ++i) {
        const auto& v = verts[i];
        std::ostringstream line;
        line << std::fixed << std::setprecision(4);
        line << "  v[" << i << "] pos=(" << v.position[0] << "," << v.position[1] << "," << v.position[2] << ")"
             << " uv=(" << v.uv[0] << "," << v.uv[1] << ")";
        omsi::log::info(line.str());
    }
    if (mesh.vertex_count > kMaxVertexLog) {
        omsi::log::info("  ... (" + std::to_string(mesh.vertex_count - kMaxVertexLog) + " vertices omitidos)");
    }

    {
        std::vector<std::string> corners;
        corners.reserve(mesh.vertex_count);
        for (const auto& v : verts) {
            std::ostringstream c;
            c << std::fixed << std::setprecision(3);
            c << "(" << v.position[0] << "," << v.position[1] << "," << v.position[2] << ")";
            corners.push_back(c.str());
        }
        std::sort(corners.begin(), corners.end());
        corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
        omsi::log::info("-- esquinas locales unicas: " + std::to_string(corners.size()) + " --");
        for (const auto& c : corners) omsi::log::info("  " + c);
    }

    omsi::log::info("-- caras / triangulos (" + std::to_string(mesh.triangle_count) + ") --");
    if (mesh.long_triangle_indices) {
        const auto tris = mesh.triangle_u32_span();
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            omsi::log::info("  tri[" + std::to_string(t) + "] idx=(" + std::to_string(tris[t].i0) + "," +
                            std::to_string(tris[t].i1) + "," + std::to_string(tris[t].i2) +
                            ") mat=" + std::to_string(tris[t].material));
        }
    } else {
        const auto tris = mesh.triangle_u16_span();
        for (std::uint32_t t = 0; t < mesh.triangle_count; ++t) {
            omsi::log::info("  tri[" + std::to_string(t) + "] idx=(" + std::to_string(tris[t].i0) + "," +
                            std::to_string(tris[t].i1) + "," + std::to_string(tris[t].i2) +
                            ") mat=" + std::to_string(tris[t].material));
        }
    }
    omsi::log::info("=== fin mesh dump ===");
}

} // namespace

namespace omsi::app {

int run_hello_o3d_app(const std::filesystem::path& o3d_path, const std::filesystem::path& omsi_root,
                      int max_frames, const std::filesystem::path& screenshot_path, float rot_deg_fixed,
                      int axis_mode, float origin_rx, float origin_ry, float origin_rz, float origin_tx,
                      float origin_ty, float origin_tz, bool has_origin_trans) {
    const auto resolved = resolve_o3d_path(o3d_path, omsi_root);
    if (resolved.empty() || !std::filesystem::exists(resolved)) {
        log::error("debug o3d not found — usa --debug-o3d \"ruta\\modelo.o3d\"");
        return 2;
    }

    std::optional<o3d::MappedO3D> loaded = o3d::load_mapped(resolved);
    if (!loaded) {
        for (const auto& alt : fallback_paths_for(resolved, omsi_root)) {
            std::error_code ec;
            if (!std::filesystem::exists(alt, ec)) continue;
            if (auto alt_loaded = o3d::load_mapped(alt)) {
                log::warn("debug-o3d: using fallback mesh " + alt.string() + " (invalid file: " + resolved.string() +
                          ")");
                loaded = std::move(alt_loaded);
                break;
            }
        }
    }
    if (!loaded) {
        log::error("failed to load o3d: " + resolved.string());
        return 2;
    }

    const auto& mesh = loaded->mesh();
    const auto materials = mesh.parse_materials();
    const auto bounds = compute_mesh_bounds(*loaded);

    log::info("hello o3d app (Frank Luna pipeline)");
    log::info("o3d: " + resolved.string());
    log::info("verts=" + std::to_string(mesh.vertex_count) + " tris=" + std::to_string(mesh.triangle_count) +
              " materials=" + std::to_string(materials.size()));
    log_mesh_uv_faces_textures(*loaded, materials);
    if (bounds.valid) {
        const float ex = bounds.max_x - bounds.min_x;
        const float ey = bounds.max_y - bounds.min_y;
        const float ez = bounds.max_z - bounds.min_z;
        log::info("axis extent dx=" + std::to_string(ex) + " dy=" + std::to_string(ey) + " dz=" + std::to_string(ez) +
                  " (altura del mesh = eje Z, igual que Blender/OMSI)");
    }

    if (!glfwInit()) {
        log::error("glfwInit failed");
        return 5;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "OMSI Hello O3D", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        log::error("glfwCreateWindow failed");
        return 5;
    }
    glfwShowWindow(window);

    int fbw = 0;
    int fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);

    gfx::dx12::HelloO3dRenderer renderer{};
    if (!renderer.initialize(glfwGetWin32Window(window), static_cast<std::uint32_t>(fbw),
                             static_cast<std::uint32_t>(fbh))) {
        glfwDestroyWindow(window);
        glfwTerminate();
        log::error("hello o3d renderer init failed");
        return 7;
    }

    if (!renderer.load_o3d(*loaded)) {
        renderer.shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        log::error("hello o3d mesh upload failed");
        return 7;
    }

    // GPU verts match VB (draw matrix). Pivot from these verts, not raw file bbox.
    std::vector<DirectX::XMFLOAT3> rest_gpu;
    float hinge_x = 0.f, hinge_y = 0.f, hinge_z = 0.f;
    float mesh_cx = 0.f, mesh_cy = 0.f, mesh_cz = 0.f;
    {
        const auto draw = o3d::o3d_mesh_draw_matrix(mesh);
        const auto verts = mesh.vertex_span();
        rest_gpu.reserve(verts.size());
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, minz = 1e9f, maxz = -1e9f;
        for (const auto& v : verts) {
            const auto p = draw.transform_point({v.position[0], v.position[1], v.position[2], 1.f});
            rest_gpu.push_back({p.x, p.y, p.z});
            minx = (std::min)(minx, p.x);
            maxx = (std::max)(maxx, p.x);
            miny = (std::min)(miny, p.y);
            maxy = (std::max)(maxy, p.y);
            minz = (std::min)(minz, p.z);
            maxz = (std::max)(maxz, p.z);
        }
        mesh_cx = 0.5f * (minx + maxx);
        mesh_cy = 0.5f * (miny + maxy);
        mesh_cz = 0.5f * (minz + maxz);
        hinge_x = (std::fabs(maxx) >= std::fabs(minx)) ? maxx : minx;
        hinge_y = mesh_cy;
        hinge_z = mesh_cz;
        log::info("gpu bbox=(" + std::to_string(minx) + ".." + std::to_string(maxx) + "," +
                  std::to_string(miny) + ".." + std::to_string(maxy) + "," + std::to_string(minz) + ".." +
                  std::to_string(maxz) + ") hinge=(" + std::to_string(hinge_x) + "," + std::to_string(hinge_y) +
                  "," + std::to_string(hinge_z) + ") rot_deg=" + std::to_string(rot_deg_fixed));
    }

    auto pack_world_t = [](DirectX::XMMATRIX world, std::array<float, 16>& out) {
        DirectX::XMFLOAT4X4 stored{};
        DirectX::XMStoreFloat4x4(&stored, DirectX::XMMatrixTranspose(world));
        std::memcpy(out.data(), &stored, sizeof(stored));
    };

    auto log_pivot_radii = [&](DirectX::XMMATRIX world, float angle_deg, float px, float py, float pz) {
        float max_r0 = 0.f, max_r1 = 0.f, max_drift = 0.f;
        for (const auto& r0 : rest_gpu) {
            const float d0 = std::sqrt((r0.x - px) * (r0.x - px) + (r0.y - py) * (r0.y - py) + (r0.z - pz) * (r0.z - pz));
            DirectX::XMVECTOR p1 = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(r0.x, r0.y, r0.z, 1.f), world);
            DirectX::XMFLOAT3 r1{};
            DirectX::XMStoreFloat3(&r1, p1);
            const float d1 = std::sqrt((r1.x - px) * (r1.x - px) + (r1.y - py) * (r1.y - py) + (r1.z - pz) * (r1.z - pz));
            max_r0 = (std::max)(max_r0, d0);
            max_r1 = (std::max)(max_r1, d1);
            max_drift = (std::max)(max_drift, std::fabs(d1 - d0));
        }
        log::info("rot_test angle=" + std::to_string(angle_deg) + " pivot=(" + std::to_string(px) + "," +
                  std::to_string(py) + "," + std::to_string(pz) + ") max_r0=" + std::to_string(max_r0) +
                  " max_r1=" + std::to_string(max_r1) + " max_pivot_drift=" + std::to_string(max_drift) +
                  (max_drift < 1e-3f ? " OK_rigid" : " BAD_stretch"));
    };

    input::LunaCamera camera{};
    if (bounds.valid) {
        camera.frame_omsi_bounds(bounds.min_x, bounds.min_y, bounds.min_z, bounds.max_x, bounds.max_y,
                                 bounds.max_z);
    } else {
        camera.z_up = true;
    }
    // Needles are ~cm; default orbit radius>=3m makes them a speck. Close-up on fitted pivot.
    if (has_origin_trans && !rest_gpu.empty()) {
        const auto draw0 = o3d::o3d_mesh_draw_matrix(mesh);
        const auto d_id = [&](float x, float y, float z) {
            const auto p = draw0.transform_point({x, y, z, 1.f});
            float best = 1e9f;
            for (const auto& r : rest_gpu) {
                const float dx = r.x - p.x, dy = r.y - p.y, dz = r.z - p.z;
                best = (std::min)(best, dx * dx + dy * dy + dz * dz);
            }
            return std::sqrt(best);
        };
        const float d_raw = d_id(origin_tx, origin_ty, origin_tz);
        const float d_yz = d_id(origin_tx, origin_tz, origin_ty);
        const bool use_yz = d_yz + 0.05f < d_raw;
        const float px = origin_tx;
        const float py = use_yz ? origin_tz : origin_ty;
        const float pz = use_yz ? origin_ty : origin_tz;
        const auto Pg = draw0.transform_point({px, py, pz, 1.f});
        camera.target_x = Pg.x;
        camera.target_y = Pg.y;
        camera.target_z = Pg.z;
        camera.min_radius = 0.02f;
        camera.radius = 0.22f;
        log::info("rot_cam map=" + std::string(use_yz ? "yz_swap" : "identity") + " d_raw=" +
                  std::to_string(d_raw) + " d_yz=" + std::to_string(d_yz) + " radius=0.22");
    }

    auto last = std::chrono::steady_clock::now();
    int rendered_frames = 0;
    float fps_accum = 0.f;
    int frame_count = 0;
    int last_fbw = fbw;
    int last_fbh = fbh;
    bool screenshot_done = false;

    while (!glfwWindowShouldClose(window)) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        fps_accum += dt;
        ++frame_count;

        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, GLFW_TRUE);

        camera.update(window);

        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0 && (fbw != last_fbw || fbh != last_fbh)) {
            renderer.resize(static_cast<std::uint32_t>(fbw), static_cast<std::uint32_t>(fbh));
            last_fbw = fbw;
            last_fbh = fbh;
        }

        const float aspect = fbw > 0 && fbh > 0 ? static_cast<float>(fbw) / static_cast<float>(fbh) : 16.f / 9.f;
        float wvp_t[16]{};
        camera.build_world_view_proj(aspect, wvp_t);
        renderer.set_world_view_proj(wvp_t);

        // Isolation: rigid DXMath T(-P)*R*T(P) with Unit87-derived axis (modes swap remaps).
        if (rot_deg_fixed >= 0.f) {
            const float kDeg = 3.14159265f / 180.f;
            const auto draw = o3d::o3d_mesh_draw_matrix(mesh);
            auto rot_ax = [](int axis, float rad) {
                const float c = std::cos(rad), s = std::sin(rad);
                auto m = f4f23ma::ma4::identity();
                if (axis == 0) {
                    m.m[1][1] = c;
                    m.m[1][2] = s;
                    m.m[2][1] = -s;
                    m.m[2][2] = c;
                } else if (axis == 1) {
                    m.m[0][0] = c;
                    m.m[0][2] = -s;
                    m.m[2][0] = s;
                    m.m[2][2] = c;
                } else {
                    m.m[0][0] = c;
                    m.m[0][1] = s;
                    m.m[1][0] = -s;
                    m.m[1][1] = c;
                }
                return m;
            };
            auto T = [](float x, float y, float z) {
                auto m = f4f23ma::ma4::identity();
                m.m[3][0] = x;
                m.m[3][1] = y;
                m.m[3][2] = z;
                return m;
            };
            // cfg origin_trans is wiki; pick identity vs (x,z,y) like MeshAnimator::fit_origin_axis_map
            // (NOT tied to 0x79 — OA101 needles have no transform chunk but still need yz_swap).
            float fx = hinge_x, fy = hinge_y, fz = hinge_z;
            if (has_origin_trans) {
#include "app/detail/hello_o3d_app_part2.inc.cpp"

