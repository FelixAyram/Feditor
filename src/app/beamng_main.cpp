// BeamNG.drive entry: launcher -> mount + load level -> editor (same engine and tools as OMSI).
// The editor core needs at least one OMSI tile, so a tiny hidden shim map (one empty tile at the origin) lives in
// <exe>/beamng_workspace/<level>/; BeamNG world coordinates are used as-is (editor origin = world origin).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "app/beamng_main.hpp"

#include "beamng/bng_session.hpp"
#include "app/map_editor_app.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/logger.hpp"
#include "omsi/map_tile_editor.hpp"
#include "omsi/map_types.hpp"
#include "ui/launcher.hpp"
#include "ui/loading_screen.hpp"

#include <fstream>
#include <future>
#include <thread>

#include <windows.h>

namespace omsi::app {

namespace {

// Leaving BeamNG must not leave globals pointing at the dead session: a stale "vfs:" provider made the next
// OMSI map load textures from a destroyed VFS, and the leftover mip-skip broke texture quality.
void release_beamng_gfx_globals() {
    gfx::dx12::set_virtual_file_provider(nullptr, nullptr);
    bng::set_active_vfs(nullptr);
    gfx::dx12::set_texture_mip_skip(0);
}

std::filesystem::path exe_dir() {
    wchar_t mod[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::filesystem::current_path();
    return std::filesystem::path(mod).parent_path();
}

// Bump when the shim layout changes so stale workspaces are rebuilt.
constexpr int kShimVersion = 2;

std::optional<std::filesystem::path> ensure_shim_map(const std::filesystem::path& ws, const std::string& title) {
    std::error_code ec;
    std::filesystem::create_directories(ws, ec);
    const auto cfg_path = ws / "global.cfg";
    const auto stamp = ws / "beamng_shim.txt";
    {
        std::ifstream in(stamp);
        int v = 0;
        if (in >> v && v == kShimVersion && std::filesystem::exists(cfg_path, ec) && std::filesystem::exists(ws / "tile_0_0.map", ec))
            return cfg_path;
    }
    std::filesystem::remove(ws / "tile_0_0.map", ec);
    {
        std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
        out << "File created by OMSI F4 Engine (BeamNG.drive shim)\r\n\r\n[friendlyname]\r\n" << title
            << "\r\n\r\n[name]\r\n" << title << "\r\n\r\n";
    }
    map::GlobalMap g;
    g.root = ws;
    g.global_cfg_path = cfg_path;
    if (!map::create_new_tile(g, 0, 0, {}, 0.f)) {
        log::error("beamng: no se pudo crear el tile shim en " + ws.string());
        return std::nullopt;
    }
    std::ofstream(stamp) << kShimVersion;
    return cfg_path;
}
} // namespace

int run_beamng_flow(const BeamngFlowArgs& args, BeamngFlowResult& out) {
    out = BeamngFlowResult{};
    std::optional<launcher::BeamngLaunchSelection> sel;
    if (!args.cli_level.empty()) {
        const auto pr = bng::load_prefs();
        launcher::BeamngLaunchSelection c;
        c.install = !pr.install.empty() ? pr.install : bng::find_beamng_install();
        c.user_folder = !pr.user_folder.empty() ? pr.user_folder : bng::find_beamng_user_folder();
        c.level = args.cli_level;
        c.spawn = args.cli_spawn;
        c.load_radius = args.cli_radius > 0.f ? args.cli_radius : pr.load_radius;
        c.confirmed = true;
        sel = c;
    } else {
        sel = launcher::show_beamng_launcher();
    }
    if (!sel) return 0;
    if (sel->switch_to_omsi) {
        out.switch_to_omsi = true;
        return 0;
    }
    if (!sel->confirmed) return 0;

    // Whatever happens inside the editor, the BeamNG globals are reset on the way out (RAII so an exception
    // or an early return cannot leave a dangling "vfs:" provider behind).
    struct GlobalsGuard {
        ~GlobalsGuard() { release_beamng_gfx_globals(); }
    } globals_guard;

    ui::LoadingScreen loading{};
    loading.open(L"Feditor - Cargando nivel de BeamNG.drive");
    loading.update(0.05f, "Montando archivos del juego...");

    auto session = std::make_shared<bng::Session>();
    session->install = sel->install;
    session->user_folder = sel->user_folder;
    session->spawn_name = sel->spawn;
    session->load_radius = sel->load_radius;
    session->view_only = sel->view_only;
    // The launcher already mounted the VFS; remount only if it didn't.
    if (auto v = bng::active_vfs_shared(); v && v->file_count() > 0) {
        session->vfs = std::move(v);
    } else {
        auto vfs = std::make_shared<bng::Vfs>();
        vfs->mount_beamng(sel->install, sel->user_folder);
        bng::set_active_vfs(vfs);
        session->vfs = vfs;
    }
    const auto levels = bng::discover_levels(*session->vfs);
    for (const auto& l : levels)
        if (l.name == sel->level) session->info = l;
    if (session->info.name.empty()) {
        loading.close();
        MessageBoxW(nullptr, L"No se encontro el nivel elegido.", L"BeamNG.drive", MB_OK | MB_ICONERROR);
        out.rc = 2;
        return 2;
    }

    // Load the level on a worker; the loading screen keeps pumping.
    bng::LoadProgress prog;
    auto fut = std::async(std::launch::async, [&]() { return bng::load_level(*session->vfs, session->info, &prog); });
    while (fut.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready) {
        loading.update(0.1f + 0.75f * prog.fraction, "Leyendo nivel: " + prog.stage + "...");
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) continue; // stale quit from a closed dialog: keep loading
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    auto lvl = fut.get();
    if (!lvl) {
        loading.close();
        MessageBoxW(nullptr, L"No se pudo leer el nivel.", L"BeamNG.drive", MB_OK | MB_ICONERROR);
        out.rc = 2;
        return 2;
    }
    for (const auto& w : lvl->warnings) log::warn("beamng level: " + w);
    session->level = std::make_shared<bng::Level>(std::move(*lvl));
    bng::find_spawn(*session->level, session->spawn_name, session->spawn_pos, &session->spawn_heading);
    log::info("beamng: level=" + session->info.name + " objects=" + std::to_string(session->level->objects.size()) +
              " forest=" + std::to_string(session->level->forest.size()) + " spawn=(" + std::to_string(session->spawn_pos[0]) + "," +
              std::to_string(session->spawn_pos[1]) + "," + std::to_string(session->spawn_pos[2]) + ")");

    session->workspace = exe_dir() / "beamng_workspace" / session->info.name;
    const auto shim_cfg = ensure_shim_map(session->workspace, session->info.name);
    if (!shim_cfg) {
        loading.close();
        out.rc = 2;
        return 2;
    }
    auto global = map::load_global_map(*shim_cfg);
    if (!global || global->tiles.empty()) {
        loading.close();
        log::error("beamng: shim global.cfg invalido");
        out.rc = 2;
        return 2;
    }
    loading.update(0.9f, "Abriendo editor...");

    MapEditorConfig cfg{};
    cfg.omsi_root = args.omsi_root;
    cfg.global = std::move(*global);
    cfg.entry_index = 0;
    map::EntryPoint ep{};
    ep.name = session->spawn_name.empty() ? "spawn" : session->spawn_name;
    ep.position.x = session->spawn_pos[0];
    ep.position.y = session->spawn_pos[1];
    ep.position.z = session->spawn_pos[2];
    ep.position_local = ep.position;
    ep.rotation_quat.w = 1.f;
    cfg.load_entry = ep;
    cfg.max_frames = args.max_frames;
    cfg.screenshot_path = args.screenshot_path;
    cfg.view_only = session->view_only;
    cfg.beamng = session;
    // "Cargar mapa" from inside the BeamNG editor: the editor returns 100 with the OMSI map the user picked;
    // main reopens it in OMSI mode instead of quitting.
    std::filesystem::path switch_map, switch_omsi;
    std::size_t switch_entry = 0;
    bool switch_view_only = false;
    cfg.switch_map_out = &switch_map;
    cfg.switch_omsi_out = &switch_omsi;
    cfg.switch_entry_out = &switch_entry;
    cfg.switch_view_only_out = &switch_view_only;
    cfg.boot_progress = [&](float f, const std::string& s) {
        if (f >= 1.f) {
            loading.close();
            return;
        }
        loading.update(0.9f + 0.1f * f, s);
    };
    const int rc = run_map_editor_app(std::move(cfg), world::World{});
    loading.close();
    out.editor_ran = true;
    out.rc = rc;
    if (rc == 100 && !switch_map.empty()) {
        out.switch_map = switch_map;
        out.switch_omsi = switch_omsi;
        out.switch_entry = switch_entry;
        out.switch_view_only = switch_view_only;
        log::info("beamng: editor pide mapa OMSI -> " + switch_map.string());
    }
    return rc;
}

} // namespace omsi::app
