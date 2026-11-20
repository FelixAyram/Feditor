#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "omsi/vfs_overlay.hpp"
#include "omsi/utf8_win32.hpp"
#include <cstdio>
#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "app/crash_report.hpp"

#include <cmath>
#include <algorithm>
#include <chrono>

#include <filesystem>

#include <iostream>

#include <sstream>

#include <filesystem>
#include <memory>
#include <optional>
#include <thread>

#include <vector>



#include "app/hello_cube_app.hpp"
#include "app/hello_o3d_app.hpp"
#include "app/map_editor_app.hpp"
#include "app/beamng_main.hpp"
#include "beamng/bng_session.hpp"
#include "app/spline_creator_app.hpp"
#include "gfx/common/rhi.hpp"
#include "gfx/debug_mesh.hpp"
#include "omsi/test/vehicle_test_runner.hpp"

#include "input/orbit_camera.hpp"

#include "omsi/o3d_format.hpp"
#include "omsi/logger.hpp"
#include "omsi/editor_map_workspace.hpp"

#include "omsi/map_types.hpp"

#include "omsi/options.hpp"
#include "o3d/o3d_loader.hpp"
#include "omsi/world.hpp"

#include "omsi/world_load_job.hpp"

#include "ui/launcher.hpp"
#include "ui/bus_picker.hpp"

#include "ui/loading_screen.hpp"



namespace {



std::filesystem::path find_omsi_root() {

    const std::vector<std::filesystem::path> candidates = {

        R"(C:\Program Files (x86)\Steam\steamapps\common\OMSI 2)",

    };

    for (const auto& p : candidates)

        if (std::filesystem::exists(p / "Omsi.exe")) return p;

    return {};

}



omsi::world::LoadOptions make_load_opts(const omsi::options::OmsiOptions& opts) {

    omsi::world::LoadOptions load{};

    load.omsi_root = opts.omsi_root;

    const auto tr = static_cast<std::int32_t>(std::lround(opts.tile_dist_max));
    load.tile_radius = tr < 1 ? 1 : tr;

    load.max_object_dist = opts.max_object_dist;

    load.load_splines = false;

    load.load_terrain = false;

    return load;
}

struct RenderMesh {
    std::shared_ptr<omsi::o3d::MappedO3D> o3d;
    f4f23ma::ma4 transform = f4f23ma::ma4::identity();
};

std::wstring normalize_cli_wpath(std::wstring w) {
    while (!w.empty() && (w.front() == L' ' || w.front() == L'\t')) w.erase(w.begin());
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\t')) w.pop_back();
    if (!w.empty() && w.front() == L'@') w.erase(w.begin());
    if (w.size() >= 2 && ((w.front() == L'"' && w.back() == L'"') || (w.front() == L'\'' && w.back() == L'\'')))
        w = w.substr(1, w.size() - 2);
    return w;
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
        omsi_root / "Vehicles/MAN_NL202/model/MAN_NL202.o3d",
        omsi_root / "Vehicles/MAN_LC_G/model/MAN_LC_G.o3d",
        omsi_root / "Vehicles/C2_Family/model/C2_Family.o3d",
    };
    for (const auto& p : candidates)
        if (std::filesystem::exists(p)) return p;

    std::filesystem::path best;
    std::uintmax_t best_size = 0;
    const std::vector<std::filesystem::path> roots = {
        omsi_root / "Vehicles",
        omsi_root / "Sceneryobjects",
    };
    for (const auto& root : roots) {
        if (!std::filesystem::exists(root)) continue;
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (ec || !entry.is_regular_file()) continue;
            if (entry.path().extension() != ".o3d") continue;
            if (entry.path().parent_path().filename() != "model") continue;
            const auto size = entry.file_size(ec);
            if (ec) continue;
            // Prefer a large but still reasonable vehicle mesh (avoid multi‑MB scenery blobs).
            if (size < 200'000 || size > 8'000'000) continue;
            if (size <= best_size) continue;
            best_size = size;
            best = entry.path();
        }
    }
    return best;
}

std::optional<std::filesystem::path> find_o3d_by_name(const std::filesystem::path& omsi_root,
                                                        const std::string& filename) {
    const std::vector<std::filesystem::path> roots = {omsi_root / "Sceneryobjects", omsi_root / "Vehicles"};
    for (const auto& root : roots) {
        if (!std::filesystem::exists(root)) continue;
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (ec || !entry.is_regular_file()) continue;
            if (entry.path().filename().string() == filename) return entry.path();
        }
    }
    return std::nullopt;
}

struct MeshBounds {
    f4f23ma::f4 min{f4f23ma::f32(1e9f), f4f23ma::f32(1e9f), f4f23ma::f32(1e9f), 1.f};
    f4f23ma::f4 max{f4f23ma::f32(-1e9f), f4f23ma::f32(-1e9f), f4f23ma::f32(-1e9f), 1.f};
    bool valid = false;
};

MeshBounds compute_mesh_bounds(const omsi::o3d::MappedO3D& o3d, const f4f23ma::ma4& transform) {
    MeshBounds b{};
    for (const auto& v : o3d.mesh().vertex_span()) {
        const f4f23ma::f4 p = transform.transform_point({v.position[0], v.position[1], v.position[2], 1.f});
        b.min.x = (std::min)(b.min.x, p.x);
        b.min.y = (std::min)(b.min.y, p.y);
        b.min.z = (std::min)(b.min.z, p.z);
        b.max.x = (std::max)(b.max.x, p.x);
        b.max.y = (std::max)(b.max.y, p.y);
        b.max.z = (std::max)(b.max.z, p.z);
        b.valid = true;
    }
    return b;
}

void frame_orbit_on_bounds(omsi::input::OrbitCamera& camera, const MeshBounds& b) {
    if (!b.valid) return;
    camera.frame_bounds(b.min, b.max);
}

#ifdef _WIN32
struct WinCommandLine {
    int argc = 0;
    LPWSTR* argv = nullptr;

    WinCommandLine() { argv = CommandLineToArgvW(GetCommandLineW(), &argc); }
    ~WinCommandLine() { if (argv) LocalFree(argv); }

    WinCommandLine(const WinCommandLine&) = delete;
    WinCommandLine& operator=(const WinCommandLine&) = delete;
};

bool warg_is(const wchar_t* w, const wchar_t* lit) { return wcscmp(w, lit) == 0; }

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}
#else
std::filesystem::path path_from_utf8(const std::string& utf8) { return std::filesystem::path(utf8); }
#endif

} // namespace



int main(int argc, char** argv) {
#ifdef _WIN32
    // Before any HWND: PerMonitorV2 so 4K@200% does not bitmap-scale and desync clicks
    // (GLFW #845/#2530). Manifest also declares this; call is belt-and-suspenders.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    omsi::crash_report::install();
#endif

    omsi::log::init();

    omsi::log::info("omsi_f4_viewer start");

    omsi::log::info("log file: " + omsi::log::log_path().string());
    omsi::crash_report::note("main boot");

    std::filesystem::path omsi_root = find_omsi_root();
    bool omsi_from_cli = false;

    bool headless_dump = false;

    bool load_only = false;
    bool cli_view_only = false;

    int launch_map = -1;
    std::string cli_beamng_level, cli_beamng_spawn;
    float cli_beamng_radius = 0.f;

    int launch_entry = -1; // -1 = not set on CLI; used as dialog default when >= 0

    int max_frames = -1;
    int stream_stress_hops = 0;
    bool debug_cube = false;
    bool debug_o3d = false;
    bool debug_model_cfg = false;
    bool debug_bus = false;
    bool editor_verify = false;
    bool test_spline_creator = false;
    bool sa_entry_hop_test = false;
    std::int32_t sa_watch_id = 14355;
    bool spline_creator = false;
    std::filesystem::path spline_creator_sli;
    std::filesystem::path debug_o3d_path;
    std::filesystem::path model_cfg_path;
    std::filesystem::path bus_path;
    std::filesystem::path screenshot_path;
    std::filesystem::path map_path;
    float rot_deg_fixed = -1.f;
    int axis_mode = 0;
    float origin_rx = -43.9091f, origin_ry = 90.f, origin_rz = 0.f;
    float origin_tx = 0.f, origin_ty = 0.f, origin_tz = 0.f;
    bool has_origin_trans = false;
    std::string vehicle_test_scenario;
    std::filesystem::path vehicle_test_script;

#ifdef _WIN32
    WinCommandLine wcmd{};
    if (wcmd.argv) {
        for (int i = 1; i < wcmd.argc; ++i) {
            const wchar_t* arg = wcmd.argv[i];
            if (warg_is(arg, L"--omsi") && i + 1 < wcmd.argc) {
                omsi_root = std::filesystem::path(wcmd.argv[++i]);
                omsi_from_cli = true;
            } else if (warg_is(arg, L"--dump-maps")) headless_dump = true;
            else if (warg_is(arg, L"--load-only")) load_only = true;
            else if (warg_is(arg, L"--view-only")) cli_view_only = true;
            else if (warg_is(arg, L"--debug-cube")) debug_cube = true;
            else if (warg_is(arg, L"--debug-o3d")) {
                debug_o3d = true;
                if (i + 1 < wcmd.argc && wcmd.argv[i + 1][0] != L'-')
                    debug_o3d_path = normalize_cli_path(std::filesystem::path(normalize_cli_wpath(wcmd.argv[++i])));
            } else if (warg_is(arg, L"--model-cfg")) {
                debug_model_cfg = true;
                if (i + 1 < wcmd.argc && wcmd.argv[i + 1][0] != L'-')
                    model_cfg_path = normalize_cli_path(std::filesystem::path(normalize_cli_wpath(wcmd.argv[++i])));
            } else if (warg_is(arg, L"--bus")) {
                debug_bus = true;
                if (i + 1 < wcmd.argc && wcmd.argv[i + 1][0] != L'-')
                    bus_path = normalize_cli_path(std::filesystem::path(normalize_cli_wpath(wcmd.argv[++i])));
            } else if (warg_is(arg, L"--launch-map") && i + 1 < wcmd.argc) launch_map = _wtoi(wcmd.argv[++i]);
            else if (warg_is(arg, L"--beamng") && i + 1 < wcmd.argc) cli_beamng_level = wide_to_utf8(wcmd.argv[++i]);
            else if (warg_is(arg, L"--beamng-spawn") && i + 1 < wcmd.argc) cli_beamng_spawn = wide_to_utf8(wcmd.argv[++i]);
            else if (warg_is(arg, L"--beamng-radius") && i + 1 < wcmd.argc) cli_beamng_radius = static_cast<float>(_wtof(wcmd.argv[++i]));
            else if (warg_is(arg, L"--map") && i + 1 < wcmd.argc)
                map_path = normalize_cli_path(std::filesystem::path(normalize_cli_wpath(wcmd.argv[++i])));
            else if (warg_is(arg, L"--entry") && i + 1 < wcmd.argc) launch_entry = _wtoi(wcmd.argv[++i]);
            else if (warg_is(arg, L"--frames") && i + 1 < wcmd.argc) max_frames = _wtoi(wcmd.argv[++i]);
            else if (warg_is(arg, L"--stream-stress") && i + 1 < wcmd.argc)
                stream_stress_hops = _wtoi(wcmd.argv[++i]);
            else if (warg_is(arg, L"--editor-verify")) editor_verify = true;
            else if (warg_is(arg, L"--test-spline-creator")) test_spline_creator = true;
            else if (warg_is(arg, L"--sa-entry-hop-test")) sa_entry_hop_test = true;
            else if (warg_is(arg, L"--sa-watch-id") && i + 1 < wcmd.argc)
                sa_watch_id = _wtoi(wcmd.argv[++i]);
            else if (warg_is(arg, L"--spline-creator")) spline_creator = true;
            else if (warg_is(arg, L"--sli") && i + 1 < wcmd.argc)
                spline_creator_sli = normalize_cli_path(std::filesystem::path(normalize_cli_wpath(wcmd.argv[++i])));
            else if (warg_is(arg, L"--rot-deg") && i + 1 < wcmd.argc) rot_deg_fixed = static_cast<float>(_wtof(wcmd.argv[++i]));
            else if (warg_is(arg, L"--axis-mode") && i + 1 < wcmd.argc) axis_mode = _wtoi(wcmd.argv[++i]);
            else if (warg_is(arg, L"--origin-rot") && i + 3 < wcmd.argc) {
                origin_rx = static_cast<float>(_wtof(wcmd.argv[++i]));
                origin_ry = static_cast<float>(_wtof(wcmd.argv[++i]));
                origin_rz = static_cast<float>(_wtof(wcmd.argv[++i]));
            } else if (warg_is(arg, L"--origin-trans") && i + 3 < wcmd.argc) {
                origin_tx = static_cast<float>(_wtof(wcmd.argv[++i]));
                origin_ty = static_cast<float>(_wtof(wcmd.argv[++i]));
                origin_tz = static_cast<float>(_wtof(wcmd.argv[++i]));
                has_origin_trans = true;
            } else if (warg_is(arg, L"--screenshot") && i + 1 < wcmd.argc)
                screenshot_path = std::filesystem::path(wcmd.argv[++i]);
            else if (warg_is(arg, L"--log") && i + 1 < wcmd.argc) {
                omsi::log::shutdown();
                omsi::log::init(wide_to_utf8(wcmd.argv[++i]));
            } else if (warg_is(arg, L"--vehicle-test") && i + 1 < wcmd.argc)
                vehicle_test_scenario = wide_to_utf8(wcmd.argv[++i]);
            else if (warg_is(arg, L"--vehicle-test-script") && i + 1 < wcmd.argc)
                vehicle_test_script = std::filesystem::path(wcmd.argv[++i]);
            else if (warg_is(arg, L"--vehicle-test-bus") && i + 1 < wcmd.argc)
                bus_path = normalize_cli_path(std::filesystem::path(normalize_cli_wpath(wcmd.argv[++i])));
        }
    }
#else
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--omsi" && i + 1 < argc) {
            omsi_root = path_from_utf8(argv[++i]);
            omsi_from_cli = true;
        } else if (arg == "--dump-maps") headless_dump = true;
        else if (arg == "--load-only") load_only = true;
        else if (arg == "--view-only") cli_view_only = true;
        else if (arg == "--debug-cube") debug_cube = true;
        else if (arg == "--debug-o3d") {
            debug_o3d = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') debug_o3d_path = normalize_cli_path(path_from_utf8(argv[++i]));
        } else if (arg == "--model-cfg") {
            debug_model_cfg = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') model_cfg_path = normalize_cli_path(path_from_utf8(argv[++i]));
        } else if (arg == "--bus") {
            debug_bus = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') bus_path = normalize_cli_path(path_from_utf8(argv[++i]));
        } else if (arg == "--launch-map" && i + 1 < argc) launch_map = std::stoi(argv[++i]);
        else if (arg == "--map" && i + 1 < argc) map_path = normalize_cli_path(path_from_utf8(argv[++i]));
        else if (arg == "--entry" && i + 1 < argc) launch_entry = std::stoi(argv[++i]);
        else if (arg == "--frames" && i + 1 < argc) max_frames = std::stoi(argv[++i]);
        else if (arg == "--stream-stress" && i + 1 < argc) stream_stress_hops = std::stoi(argv[++i]);
        else if (arg == "--editor-verify") editor_verify = true;
        else if (arg == "--test-spline-creator") test_spline_creator = true;
        else if (arg == "--sa-entry-hop-test") sa_entry_hop_test = true;
        else if (arg == "--sa-watch-id" && i + 1 < argc) sa_watch_id = std::stoi(argv[++i]);
        else if (arg == "--spline-creator") spline_creator = true;
        else if (arg == "--sli" && i + 1 < argc) spline_creator_sli = normalize_cli_path(path_from_utf8(argv[++i]));
        else if (arg == "--rot-deg" && i + 1 < argc) rot_deg_fixed = std::stof(argv[++i]);
        else if (arg == "--axis-mode" && i + 1 < argc) axis_mode = std::stoi(argv[++i]);
        else if (arg == "--origin-rot" && i + 3 < argc) {
            origin_rx = std::stof(argv[++i]);
            origin_ry = std::stof(argv[++i]);
            origin_rz = std::stof(argv[++i]);
        } else if (arg == "--origin-trans" && i + 3 < argc) {
            origin_tx = std::stof(argv[++i]);
            origin_ty = std::stof(argv[++i]);
            origin_tz = std::stof(argv[++i]);
            has_origin_trans = true;
        } else if (arg == "--screenshot" && i + 1 < argc) screenshot_path = path_from_utf8(argv[++i]);
        else if (arg == "--log" && i + 1 < argc) {
            omsi::log::shutdown();
            omsi::log::init(argv[++i]);
        } else if (arg == "--vehicle-test" && i + 1 < argc) {
            vehicle_test_scenario = argv[++i];
        } else if (arg == "--vehicle-test-script" && i + 1 < argc) {
            vehicle_test_script = path_from_utf8(argv[++i]);
        } else if (arg == "--vehicle-test-bus" && i + 1 < argc) {
            bus_path = normalize_cli_path(path_from_utf8(argv[++i]));
        }
    }
#endif

    if (!vehicle_test_scenario.empty() || !vehicle_test_script.empty()) {
        if (bus_path.empty()) {
            omsi::log::error("--vehicle-test requires --vehicle-test-bus <path.bus>");
            omsi::log::shutdown();
            return 2;
        }
        const int rc = vehicle_test_script.empty()
                           ? omsi::test::VehicleTestRunner::run(bus_path, omsi_root, vehicle_test_scenario)
                           : omsi::test::VehicleTestRunner::run_script_file(bus_path, omsi_root, vehicle_test_script);
        omsi::log::shutdown();
        return rc;
    }

    if (spline_creator) {
        omsi::app::SplineCreatorCli cli{};
        cli.omsi_root = omsi_root.empty() ? omsi::app::default_omsi_root() : omsi_root;
        cli.open_sli = spline_creator_sli;
        const int rc = omsi::app::run_spline_creator_app(cli);
        return rc;
    }

    if (debug_cube) {
        const int rc = omsi::app::run_hello_cube_app(max_frames);
        omsi::log::shutdown();
        return rc;
    }

    if (debug_model_cfg) {
        if (model_cfg_path.empty() && !omsi_root.empty())
            model_cfg_path = omsi_root / "Vehicles/GTA_admiral/model/model.cfg";
        const int rc = omsi::app::run_hello_model_cfg_app(model_cfg_path, omsi_root, max_frames, screenshot_path);
        omsi::log::shutdown();
        return rc;
    }

    if (debug_bus) {
        if (omsi_root.empty()) omsi_root = find_omsi_root();
        if (bus_path.empty()) {
            if (omsi_root.empty() || !std::filesystem::is_directory(omsi_root / "Vehicles")) {
                omsi::log::error("--bus sin ruta: hace falta --omsi con carpeta Vehicles");
                omsi::log::shutdown();
                return 2;
            }
            const auto picked = omsi::ui::show_bus_picker(omsi_root);
            if (!picked) {
                omsi::log::info("bus picker cancelled");
                omsi::log::shutdown();
                return 0;
            }
            bus_path = *picked;
        }
        const int rc = omsi::app::run_hello_bus_app(bus_path, omsi_root, max_frames, screenshot_path);
        omsi::log::shutdown();
        return rc;
    }

    if (debug_o3d) {
        const int rc = omsi::app::run_hello_o3d_app(debug_o3d_path, omsi_root, max_frames, screenshot_path, rot_deg_fixed,
                                                   axis_mode, origin_rx, origin_ry, origin_rz, origin_tx, origin_ty,
                                                   origin_tz, has_origin_trans);
        omsi::log::shutdown();
        return rc;
    }

    // Game choice (top of the launcher). This loop alternates the OMSI launcher and the BeamNG one as many
    // times as the user switches games; nothing is persisted until a launcher confirms, so switching never
    // rewrites the "game" preference nor kills the process.
    const bool cli_map_choice = !map_path.empty() || launch_map >= 0 || headless_dump || load_only;
    bool want_beamng = !debug_cube && (!cli_beamng_level.empty() || (!cli_map_choice && bng::load_prefs().game == "beamng"));
    std::optional<omsi::launcher::LaunchSelection> selection;
    omsi::options::OmsiOptions game_opts{};
    bool omsi_ready = false;
    // Map chosen with "Cargar mapa" from inside the BeamNG editor (rc 100): reopen it in OMSI mode.
    std::filesystem::path bng_switch_map, bng_switch_omsi;
    std::size_t bng_switch_entry = 0;
    bool bng_switch_view_only = false, have_bng_switch = false;

    // The "game" preference (which launcher opens next time) is written only on confirmation.
    auto save_game_pref = [](const char* game) {
        auto pr = bng::load_prefs();
        if (pr.game == game) return;
        pr.game = game;
        bng::save_prefs(pr);
        omsi::log::info(std::string("prefs: game=") + game);
    };
    // BeamNG does not need OMSI, so the OMSI root is resolved lazily, only when that side is entered.
    auto prepare_omsi = [&]() -> bool {
        if (omsi_ready) return true;
        if (!debug_cube) {
            if (omsi_from_cli && omsi::options::is_valid_omsi_root(omsi_root)) {
                omsi::options::save_omsi_root(omsi_root);
            } else {
                omsi_root = omsi::launcher::ensure_omsi_root(omsi_root);
            }
            if (!omsi::options::is_valid_omsi_root(omsi_root)) {
                omsi::log::error("OMSI 2 not found / not selected");
                std::cerr << "OMSI 2 no encontrado. Elegí la carpeta o usa --omsi \"…\\OMSI 2\"\n";
                return false;
            }
        }
        game_opts = debug_cube ? omsi::options::OmsiOptions{} : omsi::options::load_options(omsi_root);
        if (!debug_cube) omsi::log::info("omsi_root: " + omsi_root.string());
        omsi_ready = true;
        return true;
    };

    for (int hop = 0; hop < 64 && !selection; ++hop) {
        if (want_beamng) {
            omsi::app::BeamngFlowArgs bargs;
            bargs.omsi_root = omsi_root;
            bargs.max_frames = max_frames;
            bargs.screenshot_path = screenshot_path;
            bargs.cli_level = cli_beamng_level;
            bargs.cli_spawn = cli_beamng_spawn;
            bargs.cli_radius = cli_beamng_radius;
            cli_beamng_level.clear(); // CLI overrides apply to the first pass only
            cli_beamng_spawn.clear();
            cli_beamng_radius = 0.f;
            omsi::app::BeamngFlowResult bres;
            const int brc = omsi::app::run_beamng_flow(bargs, bres);
            if (bres.editor_ran) {
                if (bres.rc == 100 && !bres.switch_map.empty()) {
                    // "Cargar mapa" inside BeamNG: keep going in OMSI mode with the chosen map.
                    save_game_pref("omsi");
                    bng_switch_map = bres.switch_map;
                    bng_switch_omsi = bres.switch_omsi;
                    bng_switch_entry = bres.switch_entry;
                    bng_switch_view_only = bres.switch_view_only;
                    have_bng_switch = true;
                    want_beamng = false;
                    continue;
                }
                omsi::log::info("beamng flow done rc=" + std::to_string(brc));
                omsi::log::shutdown();
                return brc;
            }
            if (bres.switch_to_omsi) {
                want_beamng = false;
                continue;
            }
            if (bres.rc != 0) { // failed before the editor opened (level / shim / VFS)
                omsi::log::shutdown();
                return bres.rc;
            }
            omsi::log::info("launcher cancelled");
            omsi::log::shutdown();
            return 0;
        }

        if (!prepare_omsi()) {
            omsi::log::shutdown();
            return 1;
        }

        if (headless_dump) {
            const int n = omsi::launcher::dump_maps_catalog(omsi_root);
            omsi::log::info("--dump-maps done, count=" + std::to_string(n));
            omsi::log::shutdown();
            return n > 0 ? 0 : 2;
        }

        if (have_bng_switch) {
            omsi::launcher::LaunchSelection sel{};
            sel.omsi_root = bng_switch_omsi.empty() ? omsi_root : bng_switch_omsi;
            sel.global_cfg = bng_switch_map;
            sel.entrypoint_index = bng_switch_entry;
            sel.view_only = bng_switch_view_only;
            sel.confirmed = true;
            have_bng_switch = false;
            selection = sel;
            omsi::log::info("beamng -> OMSI map: " + bng_switch_map.string());
            break;
        }

        if (!map_path.empty()) {
            std::filesystem::path cfg = map_path;
            if (std::filesystem::is_directory(cfg)) cfg /= "global.cfg";
            else if (cfg.extension().empty() || cfg.filename() != "global.cfg") {
                const auto candidate = omsi_root / "maps" / map_path / "global.cfg";
                if (std::filesystem::exists(candidate)) cfg = candidate;
            }
            if (!std::filesystem::exists(cfg)) {
                omsi::log::error("map not found: " + map_path.string());
                omsi::log::shutdown();
                return 2;
            }
            omsi::launcher::LaunchSelection sel{};
            sel.omsi_root = omsi_root;
            sel.global_cfg = cfg;
            sel.entrypoint_index = launch_entry < 0 ? 0u : static_cast<std::size_t>(launch_entry);
            sel.confirmed = true;
            sel.view_only = cli_view_only;
            selection = sel;
            omsi::log::info("editor map path: " + cfg.string() + (cli_view_only ? " [VIEW-ONLY]" : ""));
            break;
        }
        if (launch_map >= 0) {
            const auto cfgs = omsi::map::discover_maps(omsi_root / "maps");
            if (static_cast<std::size_t>(launch_map) >= cfgs.size()) {
                omsi::log::error("--launch-map index out of range");
                omsi::log::shutdown();
                return 2;
            }
            omsi::launcher::LaunchSelection sel{};
            sel.omsi_root = omsi_root;
            sel.global_cfg = cfgs[static_cast<std::size_t>(launch_map)];
            sel.entrypoint_index = launch_entry < 0 ? 0u : static_cast<std::size_t>(launch_entry);
            sel.confirmed = true;
            sel.view_only = cli_view_only;
            selection = sel;
            break;
        }

        const auto last_map = omsi::options::resolve_last_map(game_opts);
        auto picked = omsi::launcher::show_launcher(omsi_root, last_map.value_or(std::filesystem::path{}),
                                                    game_opts.last_entrypoint_index);
        if (picked && picked->switch_to_beamng) {
            // Only the preference changes here: it is saved once the BeamNG launcher confirms.
            omsi::log::info("launcher -> BeamNG.drive");
            want_beamng = true;
            continue;
        }
        if (picked && picked->confirmed) save_game_pref("omsi");
        selection = picked;
        break;
    }

    if (!selection || !selection->confirmed) {
        omsi::log::info("launcher cancelled");
        omsi::log::shutdown();
        return 0;
    }

    omsi::ui::LoadingScreen boot_loading{};
    auto boot_progress = [&](float fraction, const std::string& status) {
        if (fraction >= 1.f) {
            boot_loading.update(1.f, status.empty() ? "Listo" : status);
            boot_loading.close();
            return;
        }
        boot_loading.open(L"Feditor — Cargando editor");
        boot_loading.update(fraction, status);
    };
    boot_progress(0.02f, "Iniciando…");

    if (!selection->omsi_root.empty() && omsi::options::is_valid_omsi_root(selection->omsi_root)) {
        omsi_root = selection->omsi_root;
        omsi::options::save_omsi_root(omsi_root);
        game_opts = omsi::options::load_options(omsi_root);
    }
    std::filesystem::path map_cfg = selection->global_cfg;
    std::optional<omsi::map::EditorMapWorkspace> current_ws;
    bool view_only = selection->view_only || cli_view_only;
    if (view_only) {
#ifdef _WIN32
        _putenv_s("OMSI_LOAD_TURBO", "1");
#else
        setenv("OMSI_LOAD_TURBO", "1", 1);
#endif
        boot_progress(0.05f, "Modo solo visualización — sin copiar workspace…");
        omsi::log::info("VIEW-ONLY: skipping ensure_editor_map_workspace (no robocopy *_actual); "
                        "mutations will be blocked. map=" +
                        map_cfg.string());
    } else {
        boot_progress(0.05f, "Preparando workspace del mapa…");
        if (auto ws = omsi::map::ensure_editor_map_workspace(map_cfg, boot_progress)) {
            current_ws = *ws;
            map_cfg = ws->actual_global_cfg;
            omsi::log::info("editor using map_actual=" + ws->actual_dir.string() +
                            " (original never written: " + ws->original_dir.string() + ")");
        }
    }
    std::size_t entry_index = selection->entrypoint_index;
    std::filesystem::path switch_map;
    std::filesystem::path switch_omsi;
    std::size_t switch_entry = 0;
    omsi::app::TileStreamRequest tile_stream{};
    omsi::map::EntryPoint stream_focus{};
    bool use_stream_focus = false;
    bool restore_orbit = false;
    float restore_radius = 50.f;
    float restore_theta = 0.f;
    float restore_phi = 0.f;
    // Entry is chosen in the main launcher UI or via --entry; never show a second modal on boot.
    bool prompt_entrypoint = false;

    for (;;) {
        boot_progress(0.55f, "Leyendo global.cfg…");
        const auto global = omsi::map::load_global_map(map_cfg);
        if (!global) {
            boot_loading.close();
            omsi::log::error("reload global.cfg failed: " + map_cfg.string());
            omsi::log::shutdown();
            return 2;
        }
        auto global_mut = *global;
        if (current_ws) {
            // Full *_actual copy: all tile paths + root under the edit folder.
            global_mut.root = current_ws->actual_dir;
            global_mut.global_cfg_path = current_ws->actual_global_cfg;
            for (auto& t : global_mut.tiles) {
                std::error_code ec;
                auto rel = std::filesystem::relative(t.tile_path, global_mut.root, ec);
                if (ec || rel.empty() || rel.begin()->string() == "..") rel = t.tile_path.filename();
                t.tile_path = (current_ws->actual_dir / rel).lexically_normal();
            }
        }
        if (global_mut.entrypoints.empty()) {
            omsi::log::warn("map has no entrypoints — spawning at (0,0,0)");
            prompt_entrypoint = false;
        }
        if (!global_mut.entrypoints.empty() && entry_index >= global_mut.entrypoints.size()) entry_index = 0;

        if (prompt_entrypoint && !use_stream_focus && !global_mut.entrypoints.empty()) {
            const auto picked = omsi::launcher::pick_entrypoint(global_mut, entry_index);
            if (!picked) {
                omsi::log::info("entrypoint pick cancelled");
                omsi::log::shutdown();
                return 0;
            }
            entry_index = *picked;
        }
        // Map reload: keep entry_index / switch_entry; entry picker only from editor UI (open_entrypoint_picker).

        // Remember live map + entry (not *_actual / *_original).
        {
            std::filesystem::path session_map = map_cfg;
            if (current_ws && !current_ws->actual_dir.empty()) {
                const auto actual_name = current_ws->actual_dir.filename().string();
                if (actual_name.size() > 7) {
                    const std::string suf = actual_name.substr(actual_name.size() - 7);
                    if (_stricmp(suf.c_str(), "_actual") == 0) {
                        const std::string base = actual_name.substr(0, actual_name.size() - 7);
                        session_map = current_ws->actual_dir.parent_path() / base / "global.cfg";
                    }
                }
            }
            omsi::options::save_editor_session(omsi_root, session_map, entry_index);
        }

        omsi::map::EntryPoint origin_spawn{};
        const bool spawn_at_origin = global_mut.entrypoints.empty();
        if (spawn_at_origin) origin_spawn = omsi::map::default_spawn_entrypoint();

        omsi::map::EntryPoint ep =
            use_stream_focus ? stream_focus
                             : spawn_at_origin ? origin_spawn
                                               : global_mut.entrypoints[entry_index];
        use_stream_focus = false;

        if (load_only) {
            boot_loading.close();
            auto load_opts = omsi::app::make_editor_load_opts(game_opts, omsi_root);
            load_opts.workspace = current_ws ? &*current_ws : nullptr;
            omsi::world::World world{};
            if (!world.load_map(global_mut, ep, load_opts)) {
                omsi::log::error("map load failed (--load-only)");
                omsi::log::shutdown();
                return 4;
            }
            omsi::log::info("--load-only done tiles=" + std::to_string(world.tiles().size()) +
                            " meshes=" + std::to_string(world.meshes().size()));
            omsi::log::shutdown();
            return 0;
        }

        // Map tiles load inside the editor so the side panel appears first.
        omsi::world::World world{};

        omsi::app::MapEditorConfig editor_cfg{};
        editor_cfg.omsi_root = omsi_root;
        editor_cfg.global = std::move(global_mut);
        editor_cfg.entry_index = entry_index;
        editor_cfg.load_entry = ep;
        editor_cfg.game_opts = game_opts;
        editor_cfg.max_frames = max_frames;
        editor_cfg.stream_stress_hops = stream_stress_hops;
        editor_cfg.sa_entry_hop_test = sa_entry_hop_test;
        editor_cfg.sa_watch_id = sa_watch_id;
        editor_cfg.screenshot_path = screenshot_path;
        editor_cfg.verify_mode = editor_verify;
        editor_cfg.test_spline_creator = test_spline_creator;
        editor_cfg.switch_map_out = &switch_map;
        editor_cfg.switch_omsi_out = &switch_omsi;
        editor_cfg.switch_entry_out = &switch_entry;
        editor_cfg.switch_view_only_out = &view_only; // may flip on "Cargar mapa" launcher
        editor_cfg.tile_stream = &tile_stream;
        editor_cfg.workspace = current_ws;
        editor_cfg.boot_progress = boot_progress;
        editor_cfg.view_only = view_only;
        bool ctrl_s_saved = false;
        bool had_map_edits = false;
        editor_cfg.ctrl_s_saved_out = &ctrl_s_saved;
        editor_cfg.had_map_edits_out = &had_map_edits;
        tile_stream = {};
        switch_map.clear();
        switch_omsi.clear();
        switch_entry = 0;
        if (restore_orbit) {
            editor_cfg.restore_orbit = true;
            editor_cfg.restore_radius = restore_radius;
            editor_cfg.restore_theta = restore_theta;
            editor_cfg.restore_phi = restore_phi;
            restore_orbit = false;
        }
        if (editor_verify) {
            editor_cfg.max_frames = -1;
            if (editor_cfg.screenshot_path.empty()) {
                editor_cfg.screenshot_path =
                    std::filesystem::path("_shots/editor_entry0.bmp");
            }
            editor_cfg.screenshot_entry1_path = editor_cfg.screenshot_path.parent_path() / "editor_entry1.bmp";
            std::error_code ec;
            std::filesystem::create_directories(editor_cfg.screenshot_path.parent_path(), ec);
        }

        boot_progress(0.62f, "Abriendo ventana del editor…");
        const int rc = omsi::app::run_map_editor_app(std::move(editor_cfg), std::move(world));
        boot_loading.close();
        if (rc == 101 && tile_stream.active) {
            stream_focus = tile_stream.focus;
            use_stream_focus = true;
            prompt_entrypoint = false;
            restore_orbit = true;
            restore_radius = tile_stream.keep_radius;
            restore_theta = tile_stream.keep_theta;
            restore_phi = tile_stream.keep_phi;
            boot_progress(0.2f, "Recargando tiles (stream)…");
            omsi::log::info("streaming tiles around (" + std::to_string(stream_focus.position.x) + "," +
                            std::to_string(stream_focus.position.y) + ")");
            continue;
        }
        // Keep/discard *_actual only when really quitting (not tile-stream 101, not load-other-map 100).
        if (rc != 100 && rc != 101 && current_ws && !view_only && !editor_verify && !test_spline_creator &&
            !sa_entry_hop_test && (ctrl_s_saved || had_map_edits)) {
            const int ans =
                MessageBoxW(nullptr,
                            L"¿Conservar la carpeta *_actual?\n\n"
                            L"Ctrl+S ya escribió los .map en *_actual (y en el original o en «… cambios» si lo elegiste).\n\n"
                            L"Sí = dejar *_actual y poner el nombre «… - edit» en global.cfg\n"
                            L"No = borrar *_actual (se pierden los .map de esa carpeta; no borra el original ni «… cambios»)",
                            L"Feditor — Salir del editor",
                            MB_YESNO | MB_ICONQUESTION | MB_TOPMOST |
                                (ctrl_s_saved ? MB_DEFBUTTON1 : MB_DEFBUTTON2));
            if (ans == IDYES) omsi::map::commit_editor_workspace(*current_ws);
            else omsi::map::discard_editor_workspace(*current_ws);
        }
        if (rc == 100 && !switch_map.empty()) {
            if (!switch_omsi.empty() && omsi::options::is_valid_omsi_root(switch_omsi)) {
                omsi_root = switch_omsi;
                omsi::options::save_omsi_root(omsi_root);
                game_opts = omsi::options::load_options(omsi_root);
                omsi::log::info("editor switch omsi_root=" + omsi_root.string());
            }
            map_cfg = switch_map;
            entry_index = switch_entry;
            prompt_entrypoint = false; // launcher already chose entrypoint
            current_ws.reset();
            if (view_only) {
                boot_progress(0.05f, "Modo solo visualización — sin copiar workspace…");
                omsi::log::info("VIEW-ONLY switch: no robocopy, map=" + map_cfg.string());
            } else if (omsi::vfs::under_root(map_cfg)) {
                // co-op guest: the host's version lives in RAM, nothing is copied or linked on disk
                boot_progress(0.05f, "Version del anfitrion en RAM…");
                omsi::log::info("coop RAM switch: no workspace, map=" + omsi::path_text(map_cfg));
            } else {
                boot_progress(0.05f, "Preparando workspace del mapa…");
                if (auto ws = omsi::map::ensure_editor_map_workspace(map_cfg, boot_progress)) {
                    current_ws = *ws;
                    map_cfg = ws->actual_global_cfg;
                    if (!std::filesystem::exists(map_cfg)) {
                        const auto live = omsi::map::workspace_live_map_dir(*ws) / "global.cfg";
                        omsi::log::warn("switch missing actual global.cfg, falling back to " + live.string());
                        if (std::filesystem::exists(live)) map_cfg = live;
                    }
                    omsi::log::info("editor switch → map_actual=" + ws->actual_dir.string());
                }
            }
            omsi::log::info("reloading map: " + map_cfg.string() + " entry=" + std::to_string(entry_index) +
                            (view_only ? " [VIEW-ONLY]" : " [EDIT]"));
            continue;
        }
        omsi::log::shutdown();
#if defined(_WIN32)
        {
            // Exit guard: the editor has saved and closed; if the process is still here 4 s later (a static
            // destructor or a worker joining forever - the window was gone and the process stayed for
            // minutes), write where the main thread is to exit_hang.log and end the process.
            static void* s_main_th = omsi::crash_report::open_current_thread();
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::seconds(4));
                if (std::FILE* f = std::fopen("exit_hang.log", "a")) {
                    for (int k = 0; k < 3; ++k) {
                        char st[1600];
                        omsi::crash_report::sample_thread_stack(s_main_th, st, sizeof(st));
                        std::fprintf(f, "EXIT_HANG sample %d: %s\n", k, st);
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                    std::fclose(f);
                }
                TerminateProcess(GetCurrentProcess(), 0);
            }).detach();
        }
#endif
        return rc;
    }
}
