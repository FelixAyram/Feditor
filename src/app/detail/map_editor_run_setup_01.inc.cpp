    // Independent watchdog: writes main_hang.log without using the engine logger. Started here, at the
    // top of the load, so a stalled load is reported (and its stacks sampled) too - it used to start only
    // with the frame loop.
    omsi::MainThreadWatchdog main_watchdog("main_hang.log");
    g_main_watchdog = &main_watchdog;
    main_watchdog.set_stack_sampler(&omsi::crash_report::sample_thread_stack,
                                    omsi::crash_report::open_current_thread());
    // VIEW-ONLY: no workspace, no mutations (place/move/delete/save/undo/autofill/trim/creator…).
    // Selection, camera, rails, catalog, "Ver en .map" (read) remain enabled.
    const bool view_only = cfg.view_only;
    auto block_if_view_only = [&](const char* action) -> bool {
        if (!view_only) return false;
        const std::string msg = std::string("Modo solo visualización — ") + action + " bloqueado";
        // panel may not exist yet during early setup; status is set when available.
        log::info(std::string("VIEW_ONLY_BLOCK ") + action);
        return true;
    };

    std::size_t object_count = 0;
    std::size_t spline_count = 0;
    for (const auto& tile : world.tiles()) {
        object_count += tile.objects.size();
        spline_count += tile.splines.size();
    }
    log::info("editor map=\"" + cfg.global.friendly_name + "\" tiles=" + std::to_string(world.tiles().size()) +
              " objects=" + std::to_string(object_count) + " splines=" + std::to_string(spline_count) +
              " meshes=" + std::to_string(world.meshes().size()) + " (HelloO3d Z-up path)" +
              (view_only ? " [VIEW-ONLY]" : " [EDIT]"));

    // [terrainmapping] spline verges: the map's first ground texture (before any spline is meshed)
    if (!cfg.global.ground_textures.empty()) {
        const auto& g0 = cfg.global.ground_textures.front();
        std::filesystem::path gt;
        for (const auto& base : {cfg.omsi_root, cfg.global.root}) {
            if (g0.diffuse.empty()) break;
            const auto p = g0.diffuse.is_absolute() ? g0.diffuse : base / g0.diffuse;
            if (omsi::vfs::exists(p)) { gt = p; break; }
            const auto dds = p.parent_path() / (p.stem().wstring() + L".dds");
            if (omsi::vfs::exists(dds)) { gt = dds; break; }
        }
        spline::set_terrain_mapping_ground(gt, g0.scale);
        if (!gt.empty()) log::info("terrainmapping ground: " + omsi::path_utf8(gt) + " scale=" + std::to_string(g0.scale));
    } else {
        spline::set_terrain_mapping_ground({}, 60.f);
    }
    int stream_stress_left = cfg.stream_stress_hops;
    int random_hop_cd = 0;
    bool pending_visual_texture_reload = false;

    editor_log::init();
    editor_log::event("SESSION", "map=\"" + cfg.global.friendly_name + "\" log=" + editor_log::session_path().string() +
                                     (view_only ? " VIEW_ONLY" : ""));
    if (view_only) editor_log::event("VIEW_ONLY", "mutations blocked; no *_actual workspace");
    log::info("editor session log: " + editor_log::session_path().string());
    log::info("engine log file: " + log::log_path().string());
    {
        const auto cmd_log = editor_log::session_path().parent_path() / "editor_command_log.txt";
        omsi::editor::CommandManager::Get().InitializeLog(cmd_log.string());
        omsi::editor::CommandManager::Get().Note("SESSION", cfg.global.friendly_name);
        log::info("editor command log: " + cmd_log.string());
    }
    {
        omsi::HardwareProfile::global().calibrate(cfg.global.global_cfg_path.parent_path());
    }

#ifdef _WIN32
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    auto report_boot = [&](float frac, const std::string& status) {
        if (cfg.boot_progress) cfg.boot_progress(frac, status);
    };
    report_boot(0.64f, "Iniciando GLFW / DirectX…");
    if (!glfwInit()) return 5;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    const int editor_win_w = 1600;
    GLFWwindow* window = glfwCreateWindow(editor_win_w, 900,
                                          view_only ? "Feditor (solo visualización)" : "Feditor",
                                          nullptr, nullptr);
    if (!window) return 6;
#ifdef _WIN32
    // Black window from the very first paint (Windows paints the class brush = white until DX12 presents).
    if (HWND hwnd_boot = glfwGetWin32Window(window)) {
        SetClassLongPtrW(hwnd_boot, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
        if (HDC dc = GetDC(hwnd_boot)) {
            RECT rc{};
            GetClientRect(hwnd_boot, &rc);
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            ReleaseDC(hwnd_boot, dc);
        }
    }
#endif
    boot_trace.mark("glfw_window");

    // Prefs + async tile parse start BEFORE dx12 so disk/CPU overlap GPU device init.
    map::EditorUiSettings editor_ui_settings = map::load_editor_ui_settings(cfg.global.root);
    // Chrono scenarios of this map (Vista > Cronos): the preview date saved for THIS map lays the scenarios in
    // force on it over every tile read (0 / another map = the plain map). Saving is blocked while it does.
    const std::vector<map::ChronoScenario> chrono_scenarios = map::list_chrono_scenarios(cfg.global.root);
    {
        const bool this_map = editor_ui_settings.chrono_map == omsi::path_utf8(cfg.global.root);
        const std::int32_t cdate = this_map ? editor_ui_settings.chrono_date : 0;
        std::vector<std::filesystem::path> dirs;
        if (cdate != 0)
            for (const auto& sc : chrono_scenarios)
                if (sc.active_on(cdate)) dirs.push_back(sc.dir);
        map::set_active_chrono_dirs(dirs);
        if (!this_map) editor_ui_settings.chrono_date = 0;
        log::info("chrono: scenarios=" + std::to_string(chrono_scenarios.size()) + " preview_date=" +
                  std::to_string(cdate) + " active=" + std::to_string(dirs.size()));
    }
    int editor_tile_radius_early = editor_ui_settings.editor_tile_radius;
    if (editor_tile_radius_early < 1) editor_tile_radius_early = 1;
    if (editor_tile_radius_early > 20) editor_tile_radius_early = 20;
    world::AsyncWorldLoader boot_job{};
    bool boot_job_started = false;
    const bool sync_boot_tiles = []() {
        const char* e = std::getenv("OMSI_SYNC_BOOT");
        return e && e[0] && e[0] != '0';
    }();
    if (world.tiles().empty() && !sync_boot_tiles) {
        const map::EntryPoint& boot_ep = cfg.load_entry;
        // Boot only the near rings: with radius 20 the boot parsed/uploaded ~336 tiles (~59 s) before the editor
        // opened. The rest of the radius is streamed right after the first frame (stream_diff_needed).
        auto boot_load_opts =
            make_editor_load_opts(cfg.game_opts, cfg.omsi_root, (std::min)(editor_tile_radius_early, 3));
        boot_load_opts.workspace = cfg.workspace ? &*cfg.workspace : nullptr;
        boot_job.start(cfg.global, boot_ep, boot_load_opts);
        boot_job_started = true;
        log::info("async tile load started (overlaps dx12_init)");
    }

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    gfx::dx12::HelloO3dRenderer renderer;
    report_boot(0.68f, "Inicializando renderer DX12…");
    if (!renderer.initialize(glfwGetWin32Window(window), static_cast<std::uint32_t>((std::max)(fbw, 1)),
                             static_cast<std::uint32_t>((std::max)(fbh, 1)), true)) {
        log::error("HelloO3dRenderer init failed");
        return 7;
    }
    boot_trace.mark("dx12_init");
    renderer.set_vsync(editor_ui_settings.vsync); // saved preference (needs the renderer impl, so after initialize)
    renderer.set_draw_shadow_decals(false); // scenery: skip shadow-decal pass (fewer draws → FPS)

    ui::ImGuiOverlay imgui_overlay{};
    report_boot(0.72f, "Cargando interfaz…");
    if (!imgui_overlay.initialize(window, renderer))
        log::warn("Dear ImGui overlay init failed — continuing without ImGui");

    ui::UltralightOverlay ultralight_overlay{};
    // ImGui panel: step ▲/▼, drag-scrub; UI prefs globales en omsi_f4_editor.cfg (junto al exe).
    // OMSI_ULTRALIGHT_PANEL=1 → UI HTML experimental (assets/ui/map_editor).
    const char* ul_opt = std::getenv("OMSI_ULTRALIGHT_PANEL");
    const bool want_ultralight = ul_opt && ul_opt[0] && ul_opt[0] != '0';
    const bool ultralight_ui =
        want_ultralight && imgui_overlay.active() &&
        ultralight_overlay.initialize(window, renderer, &imgui_overlay, 400);
    if (want_ultralight && !ultralight_ui && imgui_overlay.active())
        log::warn("Ultralight UI unavailable — using ImGui panel");
    else if (!ultralight_ui && imgui_overlay.active())
        log::info("map editor UI: ImGui panel (prefs globales omsi_f4_editor.cfg)");
    if (ultralight_ui) renderer.set_ultralight_overlay(&ultralight_overlay);
    boot_trace.mark("imgui_ui");

    ui::EditorTilePageInfo imgui_tile_info{};
    editor::PlaceFxSystem place_fx{};
    editor::EditorSfx editor_sfx{};
    {
        // beside the .exe (a release folder), else the source tree it was built from
        std::filesystem::path sfx_dir;
        {
            wchar_t exe_buf[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
            sfx_dir = std::filesystem::path(exe_buf).parent_path() / "assets" / "editor_sfx";
        }
#ifdef OMSI_F4_SOURCE_DIR
        if (std::error_code ec; !std::filesystem::exists(sfx_dir, ec))
            sfx_dir = std::filesystem::path(OMSI_F4_SOURCE_DIR) / "assets" / "editor_sfx";
#endif
        if (!editor_sfx.init(sfx_dir)) log::warn("EditorSfx init failed — silent place feedback");
        editor_sfx.set_pack(editor_ui_settings.sfx_pack.empty() ? "blocky" : editor_ui_settings.sfx_pack);
        editor_sfx.set_enabled(editor_ui_settings.place_feedback);
        editor_sfx.set_volume(editor_ui_settings.place_feedback_volume / 100.f);
        place_fx.enabled = editor_ui_settings.place_feedback;
    }
    // World (3D) sound for placed scenery (.sco [sound] -> cfg). Self-contained block:
    // WorldSound worker starts here; WorldSoundScene is ticked from loop_03, window drawn in loop_04.
    omsi::sound::WorldSound& world_sound = omsi::sound::WorldSound::instance();
    omsi::sound::WorldSoundScene world_sound_scene{};
    omsi::sound::WorldSoundUi world_sound_ui{editor_ui_settings.world_sound_enabled,
                                             editor_ui_settings.world_sound_volume};
    if (!world_sound.init()) log::warn("WorldSound init failed - 3D sound disabled");
    // this .exe's Windows audio session: the old waveOut backend left its channel volumes at 0
    (void)omsi::sound::restore_app_session_volume();
    world_sound.set_enabled(world_sound_ui.enabled);
    world_sound.set_master_volume(world_sound_ui.volume_pct / 100.f);
    ui::EditorShortcutTable editor_shortcuts;
    editor_shortcuts.load();
    int editor_tile_radius = editor_ui_settings.editor_tile_radius;
    if (editor_tile_radius < 1) editor_tile_radius = 1;
    if (editor_tile_radius > 20) editor_tile_radius = 20;
    // Visual/CPU radius from prefs (0 = unlimited). Tile streaming uses Chebyshev tile_radius;
    // SCO mesh visibility stays frustum-driven — this only gates splines/path rails/scripts.
    float editor_visual_radius_m = editor_ui_settings.editor_visual_radius_m;
    if (editor_visual_radius_m > 0.f && editor_visual_radius_m < 50.f) editor_visual_radius_m = 50.f;
    if (editor_visual_radius_m > 2000.f) editor_visual_radius_m = 2000.f;
    float editor_cpu_radius_m = editor_visual_radius_m > 0.f ? editor_visual_radius_m : 0.f;
    float cpu_focus_x = 0.f, cpu_focus_y = 0.f; // updated each frame from camera.target

    ui::EditorSidePanel panel{};
    panel.open(glfwGetWin32Window(window));
    panel.apply_ui_settings(editor_ui_settings);
    if (ultralight_ui) {
        ultralight_overlay.set_event_callback([&](const std::string& json) {
            if (json.find("reload_ui") != std::string::npos) {
                ultralight_overlay.reload();
                return;
            }
            ui::handle_ultralight_panel_event(panel, json);
        });
        input::LunaCamera::ensure_scroll_callback(window);
        input::set_scroll_filter([&](double /*xoff*/, double yoff) -> bool {
            // Window (client) coords — must match Ultralight/ImGui panel_w_ (not framebuffer).
            double mx = 0.0, my = 0.0;
            glfwGetCursorPos(window, &mx, &my);
            if (!ultralight_overlay.pointer_over_panel(mx, my)) return false;
            ultralight_overlay.on_mouse_move(mx, my);
            ultralight_overlay.on_scroll(0.0, yoff);
            return true;
        });
    }
    if (imgui_overlay.active() || ultralight_ui) panel.set_imgui_primary(true);
    // set_imgui_primary can sync marker flags from Win32 defaults — re-apply cfg [ui].
    if (!editor_ui_settings.tex_max_256_from_file)
        editor_ui_settings.tex_max_256 = cfg.game_opts.tex_max_256;
    panel.apply_ui_settings(editor_ui_settings);
    if (omsi::visual_quality_native_textures() && editor_ui_settings.tex_max_256) {
        editor_ui_settings.tex_max_256 = false;
        editor_ui_settings.tex_max_256_from_file = true;
        panel.apply_ui_settings(editor_ui_settings);
        pending_visual_texture_reload = true;
        log::info("visual_quality: texmax256=0 (nativo+mipmaps)");
    }
    if (omsi::visual_quality_enabled()) {
        log::info("visual_quality: MSAA=" + std::to_string(omsi::visual_quality_msaa_samples()) +
                  " AF=16 mip_bias=" + std::to_string(omsi::visual_quality_mip_lod_bias()) +
                  " FXAA=" + (omsi::visual_quality_fxaa_enabled() ? "1" : "0"));
    }
    gfx::dx12::set_texture_max_edge(editor_ui_settings.tex_max_256 ? 256u : 0u);
    log::info(std::string("texmax256=") + (editor_ui_settings.tex_max_256 ? "1" : "0") +
              (editor_ui_settings.tex_max_256_from_file ? " (editor Vista)" : " (options.cfg)"));
    panel.set_entrypoints(cfg.global.entrypoints);
    {
        std::vector<std::string> uvg;
        uvg.reserve(cfg.global.unsched_vehgroups.size());
        for (const auto& g : cfg.global.unsched_vehgroups) uvg.push_back(g.name);
        panel.set_uvg_groups(uvg);
    }
    const auto map_load_t0 = std::chrono::steady_clock::now();
    panel.set_status("Cargando mapa...");

    // Toast-only UI until full panel build_fn is wired in setup_06.
    if (imgui_overlay.active() && !ultralight_ui) {
        imgui_overlay.set_build_fn([&]() {
            ui::MapEditorImGuiArgs ui_args{};
            ui_args.panel = &panel;
            ui_args.map_name = cfg.global.friendly_name.c_str();
            ui::build_map_editor_imgui_ui(ui_args);
        });
    }
    auto pump_boot_ui = [&]() {
        panel.imgui_state().boot_screen = true; // full black loading screen until the main loop runs
        glfwPollEvents();
        panel.pump();
        int pw = 1, ph = 1;
        glfwGetFramebufferSize(window, &pw, &ph);
        if (pw < 1 || ph < 1) return;
        const float aspect = static_cast<float>(pw) / static_cast<float>(ph);
        input::LunaCamera boot_cam{};
        boot_cam.editor_nav = true;
        boot_cam.z_up = true;
        boot_cam.mirror_x = true;
        boot_cam.radius = 80.f;
        boot_cam.phi = 0.55f;
        float wvp_t[16]{};
        boot_cam.build_world_view_proj(aspect, wvp_t);
        renderer.set_world_view_proj(wvp_t);
        if (imgui_overlay.active()) {
            imgui_overlay.begin_frame();
            imgui_overlay.end_frame_and_build();
        }
        renderer.render();
    };

    if (world.tiles().empty()) {
        const map::EntryPoint& boot_ep = cfg.load_entry;
        const auto boot_load_opts = [&]() {
            auto o = make_editor_load_opts(cfg.game_opts, cfg.omsi_root, (std::min)(editor_tile_radius, 3));
            o.workspace = cfg.workspace ? &*cfg.workspace : nullptr;
            return o;
        }();
        if (sync_boot_tiles) {
            log::info("MAINLOAD tiles on editor thread (OMSI_SYNC_BOOT=1, no worker)");
            report_boot(0.75f, "Cargando tiles (hilo principal)…");
            panel.set_status("Cargando tiles (hilo principal)...");
            pump_boot_ui();
            if (!world.load_map(cfg.global, boot_ep, boot_load_opts)) {
                log::error("map load failed (sync boot)");
                return 4;
            }
            boot_trace.mark("tiles_parse_place_MAIN");
        } else {
        if (!boot_job_started) {
            boot_job.start(cfg.global, boot_ep, boot_load_opts);
            boot_job_started = true;
            log::info("async tile load started (after UI; dx12 already done)");
        }
        report_boot(0.75f, "Cargando tiles del mapa…");
        while (!boot_job.finished()) {
            const auto progress = boot_job.poll();
            const std::string detail = progress.status.empty() ? "Cargando tiles..." : progress.status;
            panel.set_status(detail);
            auto& bst = panel.imgui_state();
            bst.busy_overlay = true;
            bst.busy_spinner = true;
            bst.busy_title = "Cargando mapa";
            bst.busy_detail = detail;
            bst.busy_progress = progress.fraction;
            // Map 0..1 tile load into remaining boot bar (0.75..0.96).
            report_boot(0.75f + 0.21f * progress.fraction, detail);
            pump_boot_ui();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        const auto final_p = boot_job.poll();
        world = boot_job.take_world();
        if (final_p.failed && world.tiles().empty()) {
            log::error("map load failed: " + final_p.status);
            return 4;
        }
        boot_trace.mark("tiles_parse_WAIT_bg");
        }
        {
            auto& bst = panel.imgui_state();
            bst.busy_overlay = true;
            bst.busy_spinner = true;
            bst.busy_title = "Cargando mapa";
            bst.busy_detail = "Cargando geometria...";
            bst.busy_progress = 0.95f;
        }
        panel.set_status("Cargando geometria...");
        report_boot(0.97f, "Cargando geometría…");
        pump_boot_ui();
    }
    // Hand off to ImGui busy toast — close Win32 loading dialog so it does not cover the editor.
    report_boot(1.f, "Listo");
    cfg.boot_progress = {};

    object_count = 0;
    spline_count = 0;
    for (const auto& tile : world.tiles()) {
        object_count += tile.objects.size();
        spline_count += tile.splines.size();
    }
    log::info("editor map=\"" + cfg.global.friendly_name + "\" tiles=" + std::to_string(world.tiles().size()) +
              " objects=" + std::to_string(object_count) + " splines=" + std::to_string(spline_count) +
              " meshes=" + std::to_string(world.meshes().size()) + " (HelloO3d Z-up path)");

    std::filesystem::path tile_path;
    std::int32_t tile_gx = 0;
    std::int32_t tile_gy = 0;
    // Center tile = tile under the load spawn (selected entrypoint / stream focus).
    auto pick_tile = [&](std::int32_t gx, std::int32_t gy) -> const map::MapTileData* {
        for (const auto& t : world.tiles())
            if (t.grid_x == gx && t.grid_y == gy) return &t;
        return nullptr;
    };
    const map::MapTileData* spawn_tile = nullptr;
    if (const auto origin = world::find_origin_tile(cfg.global, world.spawn())) {
        tile_gx = origin->grid_x;
        tile_gy = origin->grid_y;
        spawn_tile = pick_tile(tile_gx, tile_gy);
        log::info("editor spawn from entry/focus tile grid=(" + std::to_string(tile_gx) + "," +
                  std::to_string(tile_gy) + ") world=(" + std::to_string(world.spawn().position.x) + "," +
                  std::to_string(world.spawn().position.y) + ")");
    }
    if (!spawn_tile) {
        for (const auto& t : world.tiles()) {
            if (!t.objects.empty() || !t.splines.empty()) {
                spawn_tile = &t;
                break;
            }
        }
    }
    if (!spawn_tile && !world.tiles().empty()) spawn_tile = &world.tiles().front();
    if (!spawn_tile) {
        log::error("no loaded tile for editor");
        return 3;
    }
    tile_path = spawn_tile->path;
    tile_gx = spawn_tile->grid_x;
    tile_gy = spawn_tile->grid_y;

    // Same formula as omsi-tt-viewer / map_tile_layout: 611.5·cos(lat) for world tiles.
    float tile_size_m = map::kStandardTileMeters;
    if (const auto* metric = world.tile_layout().find_grid(tile_gx, tile_gy)) {
        tile_size_m = metric->size_meters;
    } else {
        const auto layout = map::build_map_tile_layout(cfg.global);
        if (const auto* m = layout.find_grid(tile_gx, tile_gy)) tile_size_m = m->size_meters;
    }
    log::info("editor tile=" + tile_path.filename().string() + " grid=(" + std::to_string(tile_gx) + "," +
              std::to_string(tile_gy) + ") size_m=" + std::to_string(tile_size_m) +
              " (tt-viewer: 611.5*cos(lat); classic=300)");

    map::GroundTex groundtex{};
    if (!cfg.global.ground_textures.empty()) groundtex = cfg.global.ground_textures.front();
    else {
        groundtex.diffuse = "texture\\gras.bmp";
        groundtex.scale = 60.f;
    }

    const auto terrain = editor::build_local_terrain_tile(tile_path, groundtex, cfg.omsi_root, tile_size_m,
                                                          cfg.global.ground_textures);
    if (!terrain) {
        log::error("terrain build failed: " + tile_path.string());
        return 4;
    }
    log::info("editor terrain verts=" + std::to_string(terrain->mesh.vertices.size()) +
              " tris=" + std::to_string(terrain->mesh.indices.size() / 3) +
              " size_m=" + std::to_string(terrain->size_meters) + " step=" + std::to_string(terrain->step_meters) +
              " tex=" + terrain->texture_path.filename().string());
    boot_trace.mark("terrain_mesh_cpu");

    // Editor tile at (0,0); world origin for the tile is still logged for .map↔world checks.
    float tile_ox = 0.f, tile_oy = 0.f;
    {
        const auto origin = map::tile_origin_xy(world.tile_layout(), tile_gx, tile_gy);
        tile_ox = origin.first;
        tile_oy = origin.second;
        log::info("SPAWN tile grid=(" + std::to_string(tile_gx) + "," + std::to_string(tile_gy) +
                  ") editor_origin=(0,0) world_origin=(" + std::to_string(tile_ox) + "," +
                  std::to_string(tile_oy) + ") size_m=" + std::to_string(tile_size_m) +
                  " SESSION_ORIGIN_FIXED=1");
    }

    struct ScenePlace {
        std::int32_t object_id = 0;
        std::size_t chunk = 0;
        std::int32_t tile_gx = 0;
        std::int32_t tile_gy = 0;
        std::string label;
        float tx = 0.f, ty = 0.f, tz = 0.f;
        float half_xy = 2.f;
        float height = 3.f;
        bool alive = true;
        // [absheight] or [path]: map Z is absolute. Else Z is offset above terrain at XY.
        bool abs_z = false;
        bool help_arrow = false; // .sco [helparrow] — editor cyan cue
        std::int32_t parent_id = 0; // attachObj child → parent instance
        bool pinned = false; // survive tile unload (marquee clone / transient until placed)
        bool is_tree = false; // unit billboard mesh; scale via half_xy/height
        int sco_mesh_index = -1; // index in .sco [mesh] list ([visible] links)
        int lod_index = 0;         // OMSI [LOD] band (0 = highest detail)
        float lod_screensize = 1.f; // threshold: show when screensize >= this
        int text_texture_index = -1; // [useTextTexture] N on this .x part
        bool is_x_chunk = false; // .x submesh: text only if text_texture_index>=0. .o3d: apply all.
        bool script_visible_known = false;
        bool script_visible = true;
        // [useTextTexture] N -> the text last put on this place's chunk. Per place, not per chunk index:
        // a respawned object reusing a freed chunk index with the same text was skipped and kept the
        // blank placeholder, and an object with several text textures on one mesh (bus stop displays)
        // overwrote one entry and re-uploaded every texture every frame.
        std::vector<std::pair<int, std::string>> text_applied;
        std::string ctc_applied; // [CTC]: the scheme files last put on this place

        DirectX::XMFLOAT4X4 base_world{};
        bool has_base_world = false;
    };
    std::vector<ScenePlace> scene_places;
    map_script::MapSceneryScriptHost scenery_scripts;
    struct ScoAnimRuntime {
        model_cfg::ModelCfgDocument cfg;
        model_cfg::MeshAnimator animator;
        double last_update_s = -1.0; // wall clock of last animator.update (round-robin passes skip frames)
    };
    std::unordered_map<std::int32_t, ScoAnimRuntime> sco_anims;
    std::unordered_map<std::size_t, std::string> texttex_last;
    std::unordered_map<std::size_t, std::string> freetex_last;
    std::unordered_map<std::string, std::string> matl_change_last;
    // Shared CPU raster of [texttexture] (font|wx|h|text) — same street name reused across instances.
    std::unordered_map<std::string, sco::TextTextureImage> texttex_raster_cache;
    constexpr std::size_t kTextTexRasterCacheMax = 384;
    std::unordered_map<std::size_t, std::size_t> texttex_chunk_place; // per drain: chunk -> scene place
    // Worker → main: baked RGBA for [texttexture]; main only uploads GPU.
    struct TextTexBakeReady {
        std::size_t chunk = 0;
        std::int32_t obj_id = 0; // the place's object when submitted (a freed chunk index may be reused)
        int ti = 0;
        std::string txt;
        std::string rkey;
        sco::TextTextureImage img;
    };
    // Shared with worker jobs by shared_ptr: a job still running at exit keeps this state alive instead of
    // touching this function's destroyed locals (AV after "editor exit ok").
    struct TextTexBakeShared {
        std::mutex mu;
        std::vector<TextTexBakeReady> ready;
        std::unordered_set<std::string> inflight;
    };
    const auto texttex_shared = std::make_shared<TextTexBakeShared>();
    std::mutex& texttex_async_mu = texttex_shared->mu;
    std::vector<TextTexBakeReady>& texttex_bake_ready = texttex_shared->ready;
    std::unordered_set<std::string>& texttex_bake_inflight = texttex_shared->inflight;
    // Async path resolve for freetex/matl_change (filesystem::exists storms off the main thread).
    struct PathResolveShared {
        std::mutex mu;
        std::unordered_map<std::string, std::optional<std::filesystem::path>> cache;
        std::unordered_set<std::string> inflight;
    };
    const auto path_resolve_shared = std::make_shared<PathResolveShared>();
    std::mutex& path_resolve_mu = path_resolve_shared->mu;
    auto& path_resolve_cache = path_resolve_shared->cache;
    auto& path_resolve_inflight = path_resolve_shared->inflight;
    // Boot GPU spawn deferred into stream UploadScenery (default on; OMSI_SYNC_BOOT_GPU=1 = old sync).
    struct BootGpuItem {
        std::int32_t id = 0;
        std::int32_t tile_gx = 0;
        std::int32_t tile_gy = 0;
    };
    std::vector<BootGpuItem> boot_gpu_queue;
    const bool defer_boot_gpu = [] {
        const char* e = std::getenv("OMSI_SYNC_BOOT_GPU");
        return !(e && e[0] && e[0] != '0');
    }();
    struct TerrainChunk {
        std::size_t chunk = 0;
        std::int32_t tile_gx = 0;
        std::int32_t tile_gy = 0;
        bool alive = true;
        // Cut (polycut) detail mesh, built on demand for tiles near the camera (see pump_terrain_detail).
        // 0 = not built, 1 = building, 2 = uploaded, 3 = tile needs no detail (no cuts) / failed.
        std::uint8_t detail_state = 0;
        std::uint32_t detail_gen = 0; // bumped when the tile's mesh/heights change: drops stale builds
    };
    std::vector<TerrainChunk> terrain_chunks;
    // Every terrain tile upload registers here. The chunk MUST be flagged as terrain: the renderer then
    // draws it in the terrain pass (before surfaces) with pipeline_state_terrain_bias, whose vs_terrain
    // pushes the ground away from the eye. Without the flag the tile fell into the surface pass with the
    // plain PSO (no push, no bias), so the stamped terrain - AlignTerrain flushes it to the road/object
    // height - covered splines, crossings and [surface] SCOs as soon as the camera got close.
    auto register_terrain_chunk = [&](std::size_t chunk, std::int32_t gx, std::int32_t gy) {
        renderer.set_chunk_terrain(chunk, true);
        terrain_chunks.push_back(TerrainChunk{chunk, gx, gy, true});
    };
    // Uncut 61x61 grid mesh (stamped heights + UVs) of each uploaded tile: the input of the detail (polycut) job.
    std::unordered_map<std::int64_t, std::shared_ptr<const gfx::BufferMesh>> terrain_detail_base;
    struct TileFrameSlot {
        std::size_t chunk = static_cast<std::size_t>(-1);
        std::int32_t tile_gx = -1;
        std::int32_t tile_gy = -1;
    };
    TileFrameSlot tile_frame_cam{};
    TileFrameSlot tile_frame_hover{};
    std::vector<TileFrameSlot> tile_frame_empty_pool;
    bool tile_frame_empty_pool_dirty = true;
    std::int32_t tile_frame_empty_pool_center_gx = -1;
    std::int32_t tile_frame_empty_pool_center_gy = -1;
    map::CenterKachel centerkachel{};
    centerkachel.gx = tile_gx;
    centerkachel.gy = tile_gy;
    centerkachel.valid = true;
    bool pending_create_tile = false;
    std::int32_t pending_create_dx = 0;
    std::int32_t pending_create_dy = 0;
    struct TileTabDebug {
        bool enabled = false;
        std::uint64_t tile_tab_frames = 0;
        std::uint64_t resolve_cam_calls = 0;
        std::uint64_t zone_rebuilds = 0;
        std::uint64_t wireframe_rebuilds = 0;
        std::uint64_t wireframe_upsert_fail = 0;
        std::uint64_t frame_mesh_append = 0;
        std::uint64_t frame_mesh_replace = 0;
        std::uint64_t frame_mesh_reuse = 0;
        std::uint64_t hover_miss = 0;
        std::uint64_t wc_prefix_miss = 0;
        std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
        std::chrono::nanoseconds tile_tab_ns{};
    } tile_dbg;
#ifdef _WIN32
    if (const char* tile_dbg_env = std::getenv("OMSI_TILE_DEBUG")) {
        tile_dbg.enabled = tile_dbg_env[0] == '1' || tile_dbg_env[0] == 'y' || tile_dbg_env[0] == 'Y';
    }
#endif
    if (tile_dbg.enabled) {
        log::info("TILE_DEBUG enabled (OMSI_TILE_DEBUG=1) — resumen cada ~1s en pestaña Tile; log en " +
                  log::log_path().string());
        panel.imgui_set_tool(ui::EditorTool::Tile);
        tile_frame_empty_pool_dirty = true;
    }
    if (sco_path_debug_enabled()) {
        log::info("PATH_DEBUG enabled (OMSI_PATH_DEBUG=1) — pathdbg en " + log::log_path().string());
    }
    std::uint64_t editor_frame_serial = 0;
    map::ActiveTileAtWorld cached_stream_tile_frame{};
    std::uint64_t cached_stream_tile_frame_serial = 0;
    // Per-tile heightfields for object placement (must match drawn terrain meshes).
    struct TerrainHeightField {
        std::int32_t grid_x = 0;
        std::int32_t grid_y = 0;
        float rel_x = 0.f;
        float rel_y = 0.f;
        float size_m = 300.f;
        std::vector<float> heights_base;
        std::vector<float> heights;
        std::vector<std::uint8_t> hole_mask;
        std::vector<editor::TerrainHoleRing> polycut_rings;
        // Token of the async restamp/reload job allowed to write this field (0 = none). Any synchronous
        // restamp resets it so a late background result can never overwrite newer data.
        std::uint64_t restamp_token = 0;
    };
    std::vector<TerrainHeightField> terrain_fields;
    std::vector<map::MapObjectInst> edit_objects;
    // BeamNG backend hooks (mod_beamng_setup): objects with sco_path "vfs:..." spawn / sync through these.
    std::function<bool(map::MapObjectInst&)> bng_spawn_hook;
    std::function<bool(map::MapObjectInst&)> bng_sync_hook;
    std::function<bool()> bng_save_hook; // Ctrl+S in BeamNG mode: scene + terrain + forest into beamng_workspace
    std::function<std::optional<float>(float, float)> bng_terrain_sample;
    // BeamNG UI (drawn inside the ImGui panel): scene tree tab, quality tab, status line
    std::function<void()> bng_ui_scene_tab;
    std::function<void()> bng_ui_object_actions; // World Editor Edit/Camera/Object menu actions (top of Escena)
    std::function<void()> bng_ui_quality_tab;
    std::function<void()> bng_ui_roads_tab;
    std::function<void()> bng_ui_terrain_tab;
    std::function<void()> bng_ui_vegetation_tab; // Vegetacion: arboles del forest + pasto del GroundCover
    std::function<std::string()> bng_ui_status;
    float bng_shadow_distance = 400.f; // ScatterSky.shadowDistance (capped for the editor)
    float bng_visible_distance = 6000.f; // LevelInfo.visibleDistance: cap of the render-distance cull
    editor::EditorTransformSoA xform_soa;
    bool o3d_defer_if_unready = false;
    bool sa_pause_lists = false;
    std::vector<map::SplineInst> edit_splines;
    std::vector<map::MapAttachObjectInst> edit_attachments;
    struct EditSplineAttachmentEntry {
        map::MapSplineAttachmentInst att;
        std::int32_t tile_gx = 0;
        std::int32_t tile_gy = 0;
        std::int32_t tile_spline_count = 0;
        std::int32_t parent_spline_id = 0; // cached at load — stable across stream/entrypoint hops
    };
    std::vector<EditSplineAttachmentEntry> edit_spline_attachments;
    std::unordered_set<std::int32_t> spline_attach_spawn_ids;
    std::unordered_map<std::int32_t, std::size_t> spline_attach_obj_to_entry;
    // Session tombstones: stream reload reads .map from disk — block re-merge of deleted ids.
    std::unordered_set<std::int32_t> editor_deleted_object_ids;
    std::unordered_set<std::int32_t> editor_deleted_spline_ids;
    std::unordered_map<std::int64_t, std::vector<std::size_t>> tile_local_spline_edit_idx;
    std::vector<map::GlobalSplineIndexEntry> global_spline_index =
        map::build_global_spline_index_from_tiles(world.tiles());
    auto tile_key = [](std::int32_t gx, std::int32_t gy) -> std::int64_t {
        return (static_cast<std::int64_t>(gx) << 32) | static_cast<std::uint32_t>(gy);
    };
    auto tile_rel = [&](std::int32_t gx, std::int32_t gy) -> std::pair<float, float> {
        const auto rect = map::grid_tile_rect_at(cfg.global, world.tile_layout(), gx, gy);
        if (rect.resolved) return {rect.origin_x - tile_ox, rect.origin_y - tile_oy};
        const auto o = map::tile_origin_xy(world.tile_layout(), gx, gy);
        return {o.first - tile_ox, o.second - tile_oy};
    };
    auto sample_terrain_containing_xy = [&](float ex, float ey) -> std::optional<float> {
        if (bng_terrain_sample) return bng_terrain_sample(ex, ey);
        for (const auto& tf : terrain_fields) {
            if (tf.heights.empty()) continue;
            const float lx = ex - tf.rel_x;
            const float ly = ey - tf.rel_y;
            if (lx < -0.5f || ly < -0.5f || lx > tf.size_m + 0.5f || ly > tf.size_m + 0.5f) continue;
            return editor::sample_terrain_height(tf.heights, tf.size_m, lx, ly);
        }
        return std::nullopt;
    };
    auto sample_terrain_editor_xy = [&](float ex, float ey) -> float {
        if (auto h = sample_terrain_containing_xy(ex, ey)) return *h;
        // Nearest tile (edge / between tiles) — relative objects only.
        if (!terrain_fields.empty()) {
            const TerrainHeightField* best = nullptr;
            float best_d2 = 1.e30f;
            for (const auto& tf : terrain_fields) {
                if (tf.heights.empty()) continue;
                const float cx = tf.rel_x + tf.size_m * 0.5f;
                const float cy = tf.rel_y + tf.size_m * 0.5f;
                const float dx = ex - cx;
                const float dy = ey - cy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best = &tf;
                }
            }
            if (best) {
                return editor::sample_terrain_height(best->heights, best->size_m, ex - best->rel_x,
                                                    ey - best->rel_y);
            }
        }
        if (terrain) return editor::sample_terrain_height(*terrain, ex, ey);
        return 0.f;
    };
    auto sample_terrain_editor_xy_opt = [&](float ex, float ey) -> std::optional<float> {
        if (auto h = sample_terrain_containing_xy(ex, ey)) return h;
        if (!terrain_fields.empty()) return sample_terrain_editor_xy(ex, ey);
        if (terrain) return editor::sample_terrain_height(*terrain, ex, ey);
        return std::nullopt;
    };
    // applied=false → caller must freeze Z (no containing HF for abs/path ground lift).
    auto terrain_add = [&](bool abs_z, float x, float y, float map_z = 0.f, bool ground_layer = false,
                           bool* applied = nullptr) -> float {
        if (applied) *applied = true;
        if (!abs_z) return sample_terrain_editor_xy(x, y);
        // Path/surface SCOs: authored Z. Ground-layer lift only with containing HF (never 0/nearest).
        // OMSI / openOMSI keep the .map Z of [surface] objects: no lift over the terrain (it moved
        // e.g. Kreuz_MC crossings up by centimetres where the map does not adjust the terrain).
        // The surface PSOs pull them towards the eye in the VS instead (SurfacePull), so they
        // still win over the flush terrain.
        if (ground_layer) {
            (void)map_z;
            return 0.f;
        }
        return 0.f;
    };
    // Ground-layer lift (+3 cm over the terrain) for [surface] / ground-layer SCOs — except AlignTerrain
    // objects with an [absheight] [terrainhole] outline: they cut the terrain under themselves (see
    // editor::TerrainRestampContext), so, as in OMSI, the .map Z is final. Lifting them against the
    // aligned terrain (sidewalk level around a crossing) put e.g. Kreuz_* crossings ~10-13 cm too high.
    auto sco_ground_layer = [&](const map::MapObjectInst& o, const sco::SceneryObject& s) -> bool {
        if (!(sco::sco_is_ground_layer(s.render_priority) || s.surface)) return false;
        return !(o.align_terrain && editor::sco_has_terrain_hole_outline(o.sco_path));
    };
    for (const auto& tile : world.tiles()) {
        const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
        std::unordered_set<std::int32_t> tile_sa_inst_ids;
        for (const auto& sa : tile.spline_attachments)
            if (sa.instance_id > 0) tile_sa_inst_ids.insert(sa.instance_id);
        for (const auto& o : tile.objects) {
            if (tile_sa_inst_ids.count(o.id)) continue;
            auto c = o;
            c.position.x += rx;
            c.position.y += ry;
            edit_objects.push_back(std::move(c));
        }
        for (const auto& a : tile.attachments) {
            auto c = a;
            edit_attachments.push_back(std::move(c));
        }
        for (const auto& sa : tile.spline_attachments) {
            EditSplineAttachmentEntry e{};
            e.att = sa;
            e.tile_gx = tile.grid_x;
            e.tile_gy = tile.grid_y;
            e.tile_spline_count = static_cast<std::int32_t>(tile.splines.size());
            edit_spline_attachments.push_back(std::move(e));
        }
        for (const auto& s : tile.splines) {
            auto c = s;
            c.position.x += rx;
            c.position.y += ry;
            edit_splines.push_back(std::move(c));
            tile_local_spline_edit_idx[tile_key(tile.grid_x, tile.grid_y)].push_back(edit_splines.size() - 1);
        }
    }
    // DIAG: rules surviving into edit_* after multi-tile load
    {
        int obj_r = 0, spl_r = 0, tot = 0;
        for (const auto& o : edit_objects) {
            if (!o.path_rules.empty()) {
                ++obj_r;
                tot += static_cast<int>(o.path_rules.size());
                for (const auto& r : o.path_rules)
                    std::fprintf(stderr, "[edit-load] obj#%d rule kw=%s pi=%d val=%.3f\n",
                                 (int)o.id, r.keyword.c_str(), (int)r.path_index, (double)r.value);
            }
        }
        for (const auto& s : edit_splines) {
            if (!s.path_rules.empty()) {
                ++spl_r;
                tot += static_cast<int>(s.path_rules.size());
                for (const auto& r : s.path_rules)
                    std::fprintf(stderr, "[edit-load] spl#%d rule kw=%s pi=%d val=%.3f\n",
                                 (int)s.id, r.keyword.c_str(), (int)r.path_index, (double)r.value);
            }
        }
        std::fprintf(stderr, "[edit-load] summary objs_with_rules=%d spl_with_rules=%d total_rules=%d\n",
                     obj_r, spl_r, tot);
    }
    spline::apply_spline_chain_headings(edit_splines);
    for (const auto& a : edit_attachments) {
        if (a.object_text_line_count <= 0 && a.object_text_lines.empty()) continue;
        for (auto& o : edit_objects) {
            if (o.id != a.child_id) continue;
            o.strings = a.object_text_lines;
            o.string_line_count = a.object_text_line_count > 0
                                        ? a.object_text_line_count
                                        : static_cast<std::int32_t>(a.object_text_lines.size());
            break;
        }
    }
    map::compute_spline_length_accum(edit_splines);
    // [spline_terrain_align] under [attachObj] is stored on the attachment — copy onto the child
    // SCO so AlignTerrain stamping / UI see the same flag as a plain [object].
    for (const auto& a : edit_attachments) {
        if (!a.align_terrain) continue;
        for (auto& o : edit_objects) {
            if (o.id != a.child_id) continue;
            o.align_terrain = true;
            break;
        }
    }
    log::info("editor multi-tile objects=" + std::to_string(edit_objects.size()) +
              " attachments=" + std::to_string(edit_attachments.size()) + " spline_attachments=" +
              std::to_string(edit_spline_attachments.size()) + " splines=" +
              std::to_string(edit_splines.size()) + " tiles=" + std::to_string(world.tiles().size()));
    boot_trace.mark("flatten_tile_instances");
    panel.imgui_state().map_names_seeded = false;

    {
        std::vector<ui::EditorListItem> obj_list;
        for (std::size_t i = 0; i < edit_objects.size(); ++i) {
            const auto& o = edit_objects[i];
            ui::EditorListItem it{};
            it.id = o.id;
            it.label = std::to_string(o.id) + ": " + omsi::path_utf8(o.sco_path.filename());
            it.path = o.sco_path;
            it.map_index = static_cast<int>(i);
            obj_list.push_back(std::move(it));
        }
        panel.set_object_list(obj_list);
        std::vector<ui::EditorListItem> sli_list;
        for (std::size_t i = 0; i < edit_splines.size(); ++i) {
            const auto& s = edit_splines[i];
            ui::EditorListItem it{};
            it.id = s.id;
            it.label = std::to_string(s.id) + ": " + omsi::path_utf8(s.sli_path.filename());
            it.path = s.sli_path;
            it.map_index = static_cast<int>(i);
            sli_list.push_back(std::move(it));
        }
        panel.set_spline_list(sli_list);
        std::vector<ui::EditorListItem> cat_o;
        std::vector<ui::EditorListItem> cat_s;
        auto add_unique = [](std::vector<ui::EditorListItem>& dst, const std::filesystem::path& path) {
            for (const auto& e : dst)
                if (e.path == path) return;
            ui::EditorListItem it{};
            it.id = static_cast<std::int32_t>(dst.size());
            it.label = omsi::path_utf8(path.filename());
            it.path = path;
            dst.push_back(std::move(it));
        };
        for (const auto& o : edit_objects) add_unique(cat_o, o.sco_path);
        for (const auto& s : edit_splines) add_unique(cat_s, s.sli_path);
        panel.set_catalog_objects(cat_o);
        panel.set_catalog_splines(cat_s);
        panel.set_status("Cargando objetos y splines...");
    }

    // Load every tile's .terrain BEFORE placing SCOs/SLIs so height = terrain(xy) + map_z.
    terrain_fields.clear();
    for (const auto& tile : world.tiles()) {
        float size_m = map::kStandardTileMeters;
        if (const auto* m = world.tile_layout().find_grid(tile.grid_x, tile.grid_y)) size_m = m->size_meters;
        auto heights = editor::load_terrain_heights(tile.path);
        if (!heights) {
            log::warn("terrain heights missing for place: " + tile.path.filename().string());
            continue;
        }
        const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
        TerrainHeightField field{};
        field.grid_x = tile.grid_x;
        field.grid_y = tile.grid_y;
        field.rel_x = rx;
        field.rel_y = ry;
        field.size_m = size_m;
        field.heights_base = std::move(*heights);
        field.heights = field.heights_base;
        terrain_fields.push_back(std::move(field));
    }
    log::info("editor terrain heightfields=" + std::to_string(terrain_fields.size()));
    boot_trace.mark("terrain_heightfields");

    bool terrain_align_gpu_dirty = false;
    bool terrain_align_gpu_reload_only = false;
    // True only for editor-side spline/alignment edits. The streaming pass already stamps
    // all TerrainHeightFields incrementally before scheduling GPU uploads.
    bool terrain_align_gpu_needs_restamp = false;
    std::size_t terrain_align_gpu_reload_i = 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> terrain_align_gpu_reload_tiles;
    // Bumped on stream merge/unload — obj_tile_by_id must not key off tiles().size() alone.
    std::uint64_t editor_obj_tile_epoch = 1;
    bool editor_visual_cull_cache_dirty = false;
    // ---- Terrain restamp pipeline (see editor/terrain_restamp.hpp) -------------------------------
    // The heavy work (stamping, polycut rings, hole masks, SA poses, .sco hole probes, mesh assembly)
    // runs on JobSystem workers. The main thread only snapshots the lists and applies results.
    struct TerrainMeshTemplate {
        gfx::BufferMesh mesh;
        std::filesystem::path texture_path;
        std::string tex_key;
    };
    struct TerrainMeshPayload {
        gfx::BufferMesh mesh;
        std::filesystem::path texture_path;
        bool ok = false;
    };
    struct TerrainTileUser {
        std::filesystem::path map_path;
        map::GroundTex tex;
        std::filesystem::path omsi_root;
        std::vector<map::GroundTex> ground_textures;
        float size_m = 300.f;
        std::string tex_key;
    };
    auto terrain_mesh_templates = std::make_shared<editor::KeyedLruCache<TerrainMeshTemplate>>(192);
    editor::TerrainRestampQueue terrain_restamp_queue; // RAM-only restamps (stream pass 2)
    editor::TerrainRestampQueue terrain_reload_queue;  // restamp + mesh build for GPU reloads
    std::uint64_t terrain_token_seq = 0;
    bool stream_restamp_submitted = false;
    bool terrain_reload_active = false;
    bool terrain_reload_restamp = false;
    std::size_t terrain_reload_total = 0;
    std::vector<editor::TerrainRestampDone> terrain_reload_ready;

    // Immutable snapshot of everything a restamp reads from the editor lists. Cheap (the lists are
    // copied once; only align_terrain objects / SAs are kept) and callable from the main thread only.
    auto make_restamp_context = [&]() -> std::shared_ptr<editor::TerrainRestampContext> {
        std::vector<editor::TerrainRestampObjectIn> objs;
        for (const auto& o : edit_objects) {
            if (!o.align_terrain) continue;
            objs.push_back({o.position.x, o.position.y, o.position.z, o.rotation_deg.z, o.sco_path, o.rotation_deg.x});
        }
        bool any_att = false;
        for (const auto& a : edit_attachments)
            if (a.align_terrain) {
                any_att = true;
                break;
            }
        if (any_att) {
            std::unordered_map<std::int32_t, std::size_t> object_index_by_id;
            object_index_by_id.reserve(edit_objects.size() * 2 + 1);
            for (std::size_t oi = 0; oi < edit_objects.size(); ++oi)
                object_index_by_id.emplace(edit_objects[oi].id, oi);
            // [attachObj] + [spline_terrain_align]: child may not have been flagged yet / not in radius.
            for (const auto& a : edit_attachments) {
                if (!a.align_terrain) continue;
                const auto it = object_index_by_id.find(a.child_id);
                if (it == object_index_by_id.end()) continue;
                const auto& o = edit_objects[it->second];
                objs.push_back({o.position.x, o.position.y, o.position.z, o.rotation_deg.z, o.sco_path, o.rotation_deg.x});
            }
        }
        std::vector<editor::TerrainRestampSaIn> sas;
        bool any_sa = false;
        for (const auto& e : edit_spline_attachments)
            if (e.att.align_terrain) {
                any_sa = true;
                break;
            }
        if (any_sa) {
            std::unordered_map<std::int32_t, std::size_t> sli_by_id;
            sli_by_id.reserve(edit_splines.size() * 2 + 1);
            for (std::size_t si = 0; si < edit_splines.size(); ++si) sli_by_id[edit_splines[si].id] = si;
            for (const auto& e : edit_spline_attachments) {
                if (!e.att.align_terrain) continue;
                std::int64_t parent = -1;
                if (e.parent_spline_id > 0) {
                    if (const auto it = sli_by_id.find(e.parent_spline_id); it != sli_by_id.end())
                        parent = static_cast<std::int64_t>(it->second);
                }
                if (parent < 0) {
                    const auto idx_it = tile_local_spline_edit_idx.find(tile_key(e.tile_gx, e.tile_gy));
                    if (idx_it != tile_local_spline_edit_idx.end() && e.att.spline_index >= 0 &&
                        static_cast<std::size_t>(e.att.spline_index) < idx_it->second.size()) {
                        const std::size_t ei = idx_it->second[static_cast<std::size_t>(e.att.spline_index)];
                        if (ei < edit_splines.size()) parent = static_cast<std::int64_t>(ei);
                    }
                }
                if (parent < 0) continue;
                editor::TerrainRestampSaIn in;
                in.att = e.att;
                in.parent_index = static_cast<std::int32_t>(parent);
                sas.push_back(std::move(in));
            }
        }
        return editor::TerrainRestampContext::build(edit_splines, std::move(objs), std::move(sas));
    };
    auto apply_restamp_result = [&](TerrainHeightField& tf, editor::TerrainRestampResult&& r) {
        tf.heights = std::move(r.heights);
        // Old behaviour: when the heightfield is malformed the previous rings/mask stay untouched.
        if (r.has_cut_data) {
            tf.polycut_rings = std::move(r.polycut_rings);
            tf.hole_mask = std::move(r.hole_mask);
        }
    };
    // Synchronous single-tile restamp against a given snapshot (interactive edits, tile merge).
    auto restamp_terrain_field_ctx = [&](const std::shared_ptr<editor::TerrainRestampContext>& ctx,
                                         TerrainHeightField& tf) {
        if (tf.heights_base.empty()) return;
        tf.restamp_token = 0; // invalidate any in-flight async result for this field
        apply_restamp_result(tf, ctx->stamp_tile(tf.heights_base, tf.rel_x, tf.rel_y, tf.size_m));
    };
    auto restamp_terrain_field = [&](TerrainHeightField& tf) {
        if (tf.heights_base.empty()) return;
        restamp_terrain_field_ctx(make_restamp_context(), tf);
    };
    auto apply_stamped_heights_to_mesh = [&](gfx::BufferMesh& mesh, std::int32_t gx, std::int32_t gy,
                                             float size_m) {
        for (const auto& tf : terrain_fields) {
            if (tf.grid_x != gx || tf.grid_y != gy) continue;
            // Uncut grid only: the cut (polycut) mesh is built later, on a worker, for tiles near the camera.
            editor::apply_heightfield_to_terrain_mesh(mesh, tf.heights, size_m);
            return;
        }
    };
    {
        const auto restamp_ctx = make_restamp_context();
        for (auto& tf : terrain_fields) restamp_terrain_field_ctx(restamp_ctx, tf);
    }
    {
        std::size_t n_sli = 0, n_sco = 0, n_sa = 0;
        for (const auto& s : edit_splines)
            if (s.terrain_align != 0) ++n_sli;
        for (const auto& o : edit_objects)
            if (o.align_terrain) ++n_sco;
        for (const auto& e : edit_spline_attachments)
            if (e.att.align_terrain) ++n_sa;
        if (n_sli || n_sco || n_sa)
            log::info("terrain AlignTerrain stamp at load sli=" + std::to_string(n_sli) +
                      " sco=" + std::to_string(n_sco) + " sa=" + std::to_string(n_sa) +
                      " heightfields=" + std::to_string(terrain_fields.size()));
    }

    map::EditorGeoSettings geo_settings = map::load_editor_geo_settings(cfg.global.root);
    if (panel.imgui_primary()) {
        panel.imgui_state().tile_geo = geo_settings;
        panel.imgui_state().tile_aerial_visible = geo_settings.tile_aerial_visible;
        panel.imgui_state().aerial_resolution_pos = geo_settings.aerial_resolution_pos;
    }

    // Upload terrain FIRST so the tile is visible while SCOs/trees spawn (OMSI editor feel).
    renderer.clear_ground();
    terrain_chunks.clear();
    for (const auto& tile : world.tiles()) {
        float size_m = map::kStandardTileMeters;
        if (const auto* m = world.tile_layout().find_grid(tile.grid_x, tile.grid_y)) size_m = m->size_meters;
        map::GroundTex tile_tex = groundtex;
        if (geo_settings.tile_aerial_visible) {
            if (const auto aerial = map::find_tile_aerial_jpg(cfg.omsi_root, cfg.global.root, tile.path))
                tile_tex.diffuse = *aerial;
        }
        const auto terr = editor::build_local_terrain_tile(tile.path, tile_tex, cfg.omsi_root, size_m,
                                                           cfg.global.ground_textures);
        if (!terr) continue;
        auto mesh = terr->mesh;
        apply_stamped_heights_to_mesh(mesh, tile.grid_x, tile.grid_y, size_m);
        terrain_detail_base[tile_key(tile.grid_x, tile.grid_y)] = std::make_shared<const gfx::BufferMesh>(mesh);
        const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
        if (!renderer.append_textured_mesh(mesh.vertices.data(),
                                           static_cast<std::uint32_t>(mesh.vertices.size()),
                                           mesh.indices.data(),
                                           static_cast<std::uint32_t>(mesh.indices.size()),
                                           terr->texture_path, 0u)) {
            log::warn("terrain append fail " + tile.path.filename().string());
            continue;
        }
        const std::size_t chunk = renderer.chunk_count() - 1;
        const auto wt = store_world_transposed(XMMatrixTranslation(rx, ry, 0.f));
        renderer.set_chunk_world_matrix(chunk, wt.data());
        register_terrain_chunk(chunk, tile.grid_x, tile.grid_y);
        log::info("terrain place " + tile.path.filename().string() + " rel=(" + std::to_string(rx) + "," +
                  std::to_string(ry) + ") tex=" + terr->texture_path.filename().string());
    }

    input::LunaCamera spawn_preview_cam{};
    spawn_preview_cam.editor_nav = true;
    spawn_preview_cam.z_up = true;
    spawn_preview_cam.mirror_x = true;
    {
        const auto& sp = world.spawn();
        spawn_preview_cam.target_x = sp.position.x - tile_ox;
        spawn_preview_cam.target_y = sp.position.y - tile_oy;
        spawn_preview_cam.target_z = sp.position.z;
        spawn_preview_cam.radius = 80.f;
        spawn_preview_cam.phi = 0.55f;
    }
    auto visual_spawn_in_radius = [&](const float x, const float y) -> bool {
        if (editor_visual_radius_m <= 0.f) return true;
        const float dx = x - spawn_preview_cam.target_x;
        const float dy = y - spawn_preview_cam.target_y;
        return dx * dx + dy * dy <= editor_visual_radius_m * editor_visual_radius_m;
    };
    auto pump_spawn_preview = [&]() {
        glfwPollEvents();
        panel.pump();
        int pw = 1, ph = 1;
        glfwGetFramebufferSize(window, &pw, &ph);
        if (pw < 1 || ph < 1) return;
        const float aspect = static_cast<float>(pw) / static_cast<float>(ph);
        float wvp_t[16]{};
        spawn_preview_cam.build_world_view_proj(aspect, wvp_t);
        renderer.set_world_view_proj(wvp_t);
        renderer.set_tile_cull_cell(0.f, 0.f, tile_size_m, tile_size_m);
        if (imgui_overlay.active()) {
            imgui_overlay.begin_frame();
            imgui_overlay.end_frame_and_build();
        }
        renderer.render();
    };
    pump_spawn_preview(); // tiles visible before object spawn
    {
        auto& bst = panel.imgui_state();
        bst.busy_overlay = true;
        bst.busy_spinner = true;
        bst.busy_title = "Cargando objetos";
        bst.busy_detail = "Colocando SCOs / trees...";
        bst.busy_progress = 0.f;
    }
    std::jthread spawn_tex_thread; // (joins itself when the editor closes; the load no longer waits for it)
    std::size_t spawn_total = 0;
    for (const auto& pm : world.meshes()) {
        if (!pm.o3d || !pm.o3d->valid()) continue;
        const auto stem = pm.source_path.stem().string();
        if (stem == "empty_lod" || stem == "empty") continue;
        const map::MapObjectInst* obj0 = find_object(world, pm.object_id);
        if (obj0) {
            const auto [rx, ry] = tile_rel(pm.tile_grid_x, pm.tile_grid_y);
            if (!visual_spawn_in_radius(obj0->position.x + rx, obj0->position.y + ry)) continue;
        }
        ++spawn_total;
    }
    {
        struct MeshSnap {
            std::filesystem::path source;
            std::vector<std::string> tex_names;
        };
        struct ObjSnap {
            std::filesystem::path sco_path;
            std::string tree_texture;
        };
        std::vector<MeshSnap> meshes;
        meshes.reserve(world.meshes().size());
        for (const auto& pm : world.meshes()) {
            if (!pm.o3d || !pm.o3d->valid()) continue;
            const map::MapObjectInst* obj0 = find_object(world, pm.object_id);
            MeshSnap m{};
            m.source = pm.source_path;
            for (const auto& mat : pm.o3d->mesh().parse_materials()) {
                if (!mat.texture.empty()) m.tex_names.push_back(mat.texture);
            }
            meshes.push_back(std::move(m));
        }
        std::vector<ObjSnap> objs;
        objs.reserve(object_count);
        for (const auto& tile : world.tiles()) {
            const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
            for (const auto& obj : tile.objects) {
                const auto* sco = sco::load_sco(obj.sco_path);
                if (sco && sco->tree.valid) ++spawn_total;
                objs.push_back(ObjSnap{obj.sco_path, obj.tree_texture});
            }
        }
        log::info("spawn tex prefetch kick meshes=" + std::to_string(meshes.size()) +
                  " objs=" + std::to_string(objs.size()) + " cores=" +
                  std::to_string((std::max)(1u, std::thread::hardware_concurrency())));
        {
            std::unordered_set<std::string> seen_o3d;
            std::size_t n_o3d = 0;
            for (const auto& m : meshes) {
                if (m.source.empty()) continue;
                if (!seen_o3d.insert(m.source.lexically_normal().string()).second) continue;
                const auto p = m.source;
                omsi::JobSystem::global().submit(
                    [p]() { o3d::prepared_vertex_cache().ensure_path(p); }, omsi::JobSystem::Priority::High);
                ++n_o3d;
            }
            log::info("boot o3d prefetch submit n=" + std::to_string(n_o3d));
        }
        if (!meshes.empty() || !objs.empty()) {
            const auto omsi_root = cfg.omsi_root;
            spawn_tex_thread = std::jthread([meshes = std::move(meshes), objs = std::move(objs), omsi_root]() {
              // Any exception escaping a std::thread is std::terminate: the editor closed at boot because a texture or
              // .sco name with non-ANSI characters made path::string() throw here. Keys are wide strings now, and the
              // whole prefetch is guarded (it is only a warm-up; failure just means textures load later).
              try {
                std::vector<std::filesystem::path> texs;
                std::unordered_set<std::wstring> seen;
                auto add_tex = [&](const std::optional<std::filesystem::path>& p) {
                    if (!p || p->empty()) return;
                    if (seen.insert(p->native()).second) texs.push_back(*p);
                };
                for (const auto& m : meshes) {
                    for (const auto& n : m.tex_names)
                        add_tex(gfx::dx12::resolve_texture_path(m.source, n, omsi_root));
                }
                std::vector<std::filesystem::path> scos;
                std::unordered_set<std::wstring> seen_sco;
                for (const auto& o : objs) {
                    if (o.sco_path.empty()) continue;
                    if (seen_sco.insert(o.sco_path.lexically_normal().native()).second) scos.push_back(o.sco_path);
                }
                const char* hdd_env = std::getenv("OMSI_HDD_LOAD");
                if (!hdd_env || !hdd_env[0]) hdd_env = std::getenv("OMSI_LOAD_HDD");
                const bool hdd_mode = hdd_env && hdd_env[0] && hdd_env[0] != '0';
                sco::preload_scos(scos, hdd_mode);
                for (const auto& o : objs) {
                    const auto* sco = sco::load_sco(o.sco_path);
                    if (!sco || !sco->tree.valid) continue;
                    const std::string tn = !o.tree_texture.empty() ? o.tree_texture : sco->tree.texture;
                    add_tex(gfx::dx12::resolve_texture_path(o.sco_path, tn, omsi_root));
                }
                if (!texs.empty()) gfx::dx12::prefetch_textures_cpu(texs);
              } catch (const std::exception& ex) {
                log::warn(std::string("boot texture prefetch aborted: ") + ex.what());
              } catch (...) {
                log::warn("boot texture prefetch aborted (unknown exception)");
              }
            });
        }
    }
    std::size_t spawn_done = 0;
    auto last_spawn_preview = std::chrono::steady_clock::now();
    auto note_spawn_item = [&]() {
        ++spawn_done;
        const auto now_pv = std::chrono::steady_clock::now();
        if (now_pv - last_spawn_preview < std::chrono::milliseconds(20)) return;
        last_spawn_preview = now_pv;
        auto& bst = panel.imgui_state();
        bst.busy_progress = spawn_total > 0
                                ? (std::min)(1.f, static_cast<float>(spawn_done) / static_cast<float>(spawn_total))
                                : -1.f;
        bst.busy_detail = "Colocando SCOs / trees... " + std::to_string(spawn_done) + "/" +
                          std::to_string(spawn_total);
        pump_spawn_preview();
    };

    int spawn_ok = 0, spawn_bad = 0;
    std::size_t sa_stale_cpu_n = 0;
    std::size_t sa_empty_along_n = 0;
    // First GPU upload per .o3d path; later instances share VB/IB via clone_chunk.
    auto cached_sco = [&](const std::filesystem::path& path) -> const sco::SceneryObject* {
        return sco::load_sco(path);
    };
    // openOMSI paint_at_foot: a plain object that is only paint at its foot (a zebra crossing, markings:
    // the whole mesh between -5 and +25 cm and at most 5 cm thick) lies on the road like a [surface] object
    // and goes over the splines it overlaps (else the road, pulled towards the eye, covered it).
    auto paint_at_foot_z = [](float zmin, float zmax) { return zmin >= -0.05f && zmax <= 0.25f && zmax - zmin <= 0.05f; };
    // over ALL the object's .o3d meshes (openOMSI: the type's meshes), cached per .sco
    std::unordered_map<std::wstring, bool> paint_at_foot_cache;
    auto sco_paint_at_foot = [&](const sco::SceneryObject& sc) -> bool {
        if (sc.surface) return false;
        std::wstring key = sc.sco_path.native();
        for (auto& ch : key) ch = static_cast<wchar_t>(towlower(ch));
        if (const auto it = paint_at_foot_cache.find(key); it != paint_at_foot_cache.end()) return it->second;
        float zmn = 1e30f, zmx = -1e30f;
        bool any = false, has_x = false;
        for (const auto& mr : sc.meshes) {
            if (mr.mesh_file.empty()) continue;
            if (!mr.is_o3d) {
                has_x = true;
                continue;
            }
            const auto m = o3d::load_mapped_shared(mr.mesh_file);
            if (!m || !m->valid()) continue;
            float bmn[3]{}, bmx[3]{};
            mesh_local_bbox(*m, bmn, bmx);
            zmn = (std::min)(zmn, bmn[2]);
            zmx = (std::max)(zmx, bmx[2]);
            any = true;
        }
        const bool paint = any && !has_x && paint_at_foot_z(zmn, zmx);
        paint_at_foot_cache[key] = paint;
        return paint;
    };
    auto apply_sco_draw_phase = [&](std::size_t chunk, sco::ScoRenderPriority prio,
                                    bool force_surface_bias = false) {
        const auto phase =
            static_cast<gfx::dx12::WorldRenderPhase>(sco::world_render_phase_from_priority(prio));
        renderer.set_chunk_render_phase(
            chunk, phase, force_surface_bias || prio == sco::ScoRenderPriority::Surface,
            prio == sco::ScoRenderPriority::OnSurface);
        if (prio == sco::ScoRenderPriority::Presurface) renderer.set_chunk_presurface(chunk, true);
    };
    std::unordered_map<std::string, std::size_t> o3d_gpu_proto;
    std::unordered_map<std::string, std::size_t> x_gpu_proto;
    // Chunks other instances clone from: stream unload must hide them, never release VB/IB.
    std::unordered_set<std::size_t> gpu_proto_chunks;
    auto remember_gpu_proto = [&](std::unordered_map<std::string, std::size_t>& protos, const std::string& key,
                                  std::size_t chunk) {
        if (const auto it = protos.find(key); it != protos.end() && it->second != chunk) {
            log::warn("gpu proto reupload (source chunk had no GPU) key=" + key + " old=" +
                      std::to_string(it->second) + " new=" + std::to_string(chunk));
            gpu_proto_chunks.erase(it->second);
        }
        protos.insert_or_assign(key, chunk);
        gpu_proto_chunks.insert(chunk);
    };
    auto o3d_path_key = [](const std::filesystem::path& p) {
        auto s = p.lexically_normal().string();
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto o3d_gpu_key = [&](const std::filesystem::path& p, const model_cfg::ModelCfgMeshEntry* matl) {
        std::string key = o3d_path_key(p);
        if (!matl) return key;
        if (matl->presurface) key += "|pre";
        if (matl->on_surface) key += "|ons";
        std::uint32_t h = 2166136261u;
        for (const auto& m : matl->materials) {
            h ^= static_cast<std::uint32_t>(m.alpha_mode);
            h *= 16777619u;
            h ^= m.no_zwrite ? 1u : 0u;
            h *= 16777619u;
            h ^= m.no_zcheck ? 2u : 0u;
            h *= 16777619u;
        }
        key += "|a" + std::to_string(h);
        return key;
    };
    auto resolve_x_tex = [&](const std::filesystem::path& mesh_file, const std::filesystem::path& sco_path,
                             const std::string& tex_name) -> std::filesystem::path {
        if (tex_name.empty()) return {};
        if (auto p = gfx::dx12::resolve_texture_path(mesh_file, tex_name, cfg.omsi_root)) return *p;
        if (auto p2 = gfx::dx12::resolve_texture_path(sco_path, tex_name, cfg.omsi_root)) return *p2;
        return {};
    };
    // Tile [object] entrypoint_*.sco must render their own .x like any other SCO.
    // global.cfg [entrypoints] still use entry_chunks (helper mesh). Returning false here
    // avoids skipping map-object meshes (was: invisible/transparent entrypoint markers).
    auto is_global_entrypoint_marker_sco = [](const std::filesystem::path& /*p*/) {
        return false;
    };
    auto spawn_x_parts = [&](const sco::SceneryObject& sco, const std::filesystem::path& sco_path,
                             const sco::SceneryMeshRef& mesh_ref, int mesh_idx, const XMMATRIX& world_m,
                             std::int32_t inst_id, std::int32_t parent_id, std::int32_t tgx, std::int32_t tgy,
                             std::string_view tag, bool abs_z, float tx, float ty, float tz,
                             bool help_arrow = false) -> bool {
        if (is_global_entrypoint_marker_sco(sco_path)) return false;
        auto mesh_path = mesh_ref.mesh_file;
        auto xf = xfile::load_x_file(mesh_path);
        if ((!xf || xf->parts.empty()) && mesh_path.has_parent_path()) {
            // Retry under sco_dir/model/ and sco_dir/ (common OMSI layout).
            const auto base = sco_path.parent_path();
            for (const auto& cand : {base / "model" / mesh_path.filename(), base / mesh_path.filename(),
                                     cfg.omsi_root / "Sceneryobjects" / "Generic" / "model" / mesh_path.filename()}) {
                if (!std::filesystem::exists(cand)) continue;
                xf = xfile::load_x_file(cand);
                if (xf && !xf->parts.empty()) { mesh_path = cand; break; }
            }
        }
        if (!xf || xf->parts.empty()) {
            log::warn("SPAWN x-fail id=" + std::to_string(inst_id) + " mesh=" + mesh_ref.mesh_file.string());
            return false;
        }
        bool any_part = false;
        // paint at the foot is a property of the whole object (openOMSI paint_at_foot over all its meshes),
        // never of one part: a sign's flat text plate taken alone was turned into ground paint, drawn
        // before the board, and the board covered the text
        bool x_paint = false;
        if (!sco.surface && !help_arrow) {
            float zmn = 1e30f, zmx = -1e30f;
            for (const auto& prt : xf->parts)
                for (const auto& v : prt.vertices) {
                    zmn = (std::min)(zmn, v.position[2]);
                    zmx = (std::max)(zmx, v.position[2]);
                }
            x_paint = zmn <= zmx && paint_at_foot_z(zmn, zmx);
        }
        for (std::size_t pi = 0; pi < xf->parts.size(); ++pi) {
            const auto& part = xf->parts[pi];
            if (part.vertices.empty() || part.indices.empty()) continue;
            const auto bind = bind_x_part(sco, mesh_idx, xf->parts, pi);
            const auto tex = resolve_x_tex(mesh_ref.mesh_file, sco_path, part.texture_filename);
            if (!part.texture_filename.empty() && tex.empty()) {
                static int x_tex_miss = 0;
                if (x_tex_miss < 24) {
                    log::warn("x tex miss mesh=" + mesh_ref.mesh_file.filename().string() + " part=" +
                              std::to_string(pi) + " name=\"" + part.texture_filename + "\" sco=" +
                              sco_path.filename().string());
                    ++x_tex_miss;
                }
            }
            const int tt_idx = bind.text_texture_index;
            const std::uint8_t amode = bind.alpha_mode;
            const float* solid =
                (tex.empty() && part.has_diffuse) ? part.diffuse_rgba : nullptr;
            const std::string xkey = o3d_path_key(mesh_ref.mesh_file) + "|" + std::to_string(pi) + "|" +
                                     o3d_path_key(tex) + "|" + std::to_string(static_cast<int>(amode)) +
                                     (solid ? "|d" : "") +
                                     (sco.render_priority == sco::ScoRenderPriority::Presurface ? "|pre" : "");
            std::optional<std::size_t> chunk_opt;
            if (tt_idx < 0) {
                if (const auto it = x_gpu_proto.find(xkey); it != x_gpu_proto.end())
                    chunk_opt = renderer.clone_chunk(it->second);
            }
            if (!chunk_opt) {
                if (!renderer.append_textured_mesh(part.vertices.data(),
                                                   static_cast<std::uint32_t>(part.vertices.size()),
                                                   part.indices.data(),
                                                   static_cast<std::uint32_t>(part.indices.size()), tex, amode,
                                                   tt_idx, solid))
                    continue;
                const std::size_t chunk = renderer.chunk_count() - 1;
                if (tt_idx < 0) remember_gpu_proto(x_gpu_proto, xkey, chunk);
                chunk_opt = chunk;
            }
            const std::size_t chunk = *chunk_opt;
            renderer.set_chunk_world_matrix(chunk, store_world_transposed(world_m).data());
            renderer.set_chunk_visible(chunk, true);
            apply_sco_draw_phase(chunk, x_paint ? sco::ScoRenderPriority::Surface : sco.render_priority,
                                 sco.surface || x_paint);
            if (sco.special == sco::ScoSpecial::EntryPoint || sco.special == sco::ScoSpecial::Carpark)
                renderer.set_chunk_editor_helper(chunk, true);
            ScenePlace sp{};
            sp.object_id = inst_id;
            sp.chunk = chunk;
            sp.parent_id = parent_id;
            sp.tile_gx = tgx;
            sp.tile_gy = tgy;
            sp.label = "id=" + std::to_string(inst_id) + " [x] " + sco_path.filename().string();
            sp.tx = tx;
            sp.ty = ty;
            sp.tz = tz;
            sp.abs_z = abs_z;
            sp.alive = true;
            sp.help_arrow = help_arrow;
            sp.half_xy = 8.f;
            sp.height = 4.f;
            sp.sco_mesh_index = mesh_idx;
            sp.lod_index = mesh_ref.lod_index;
            sp.lod_screensize = mesh_ref.lod_screensize;
            sp.text_texture_index = tt_idx;
            sp.is_x_chunk = true;
            DirectX::XMStoreFloat4x4(&sp.base_world, world_m);
            sp.has_base_world = true;
            if (tag == "[splineAttach]") sp.pinned = true;
            scene_places.push_back(std::move(sp));
            any_part = true;
        }
        return any_part;
    };
    auto append_or_clone_o3d = [&](const o3d::MappedO3D& mapped, const std::filesystem::path& src_path,
                                   const model_cfg::ModelCfgMeshEntry* matl,
                                   const std::filesystem::path& sco_path = {}) -> std::optional<std::size_t> {
        std::string key = o3d_gpu_key(src_path, matl);
        const sco::SceneryObject* sco = sco_path.empty() ? nullptr : cached_sco(sco_path);
        const std::vector<o3d::Vertex>* vert_override = nullptr;
        std::vector<o3d::Vertex> deformed;
        sco::CrossingMeshDeformStats deform_stats{};
        if (sco && sco::should_deform_crossing_mesh(*sco, src_path)) {
            const auto base_path =
                sco::resolve_sco_o3d(sco_path.parent_path(), sco->crossing_height_deformation_mesh);
            if (const auto base = sco::load_crossing_height_base(base_path)) {
                if (auto dv = sco::deform_o3d_for_crossing(mapped.mesh(), *base, &deform_stats)) {
                    deformed = std::move(*dv);
                    vert_override = &deformed;
                } else {
                    log::warn("crossingH deform failed mesh=" + src_path.filename().string() + " sco=" +
                              sco_path.filename().string());
                }
            } else {
                log::warn("crossingH base missing mesh=" + src_path.filename().string() + " sco=" +
                          sco_path.filename().string() + " base=" + sco->crossing_height_deformation_mesh);
            }
        }
        // Crossing height is per upload — never clone/share proto (flat cache poisons streamed tiles).
        if (vert_override) {
            if (!renderer.append_o3d(mapped, matl, vert_override)) return std::nullopt;
            const std::size_t chunk = renderer.chunk_count() - 1;
            char max_dz_buf[32] = {};
            std::snprintf(max_dz_buf, sizeof(max_dz_buf), "%.4f", deform_stats.max_abs_dz);
            log::info("crossingH upload mesh=" + src_path.filename().string() + " sco=" +
                      sco_path.filename().string() + " hits=" + std::to_string(deform_stats.hit_verts) + "/" +
                      std::to_string(deform_stats.total_verts) + " max_dz=" + max_dz_buf);
            return chunk;
        }
        // Only skip GPU proto/clone when the mesh is truly unique per instance
        // (text textures / freetex / on_surface). Alpha-test trees (matl_alpha=1)
        // and glass (alpha blend) SHARE the same VB/IB — must clone or 3000 trees
        // = 3000 full uploads + 3000 draws and tank FPS.
        const bool unique_tex =
            matl && (matl->on_surface ||
                     std::any_of(matl->materials.begin(), matl->materials.end(),
                                 [](const model_cfg::ModelCfgMaterial& m) {
                                     return m.use_text_texture.has_value() || m.freetex_label.has_value() ||
                                            m.is_matl_change;
                                 }));
        if (!vert_override) {
            if (const auto* cached = o3d::prepared_vertex_cache().try_get(src_path))
                vert_override = cached;
            else if (o3d_defer_if_unready && (unique_tex || o3d_gpu_proto.find(key) == o3d_gpu_proto.end()))
                return std::nullopt;
            else {
                o3d::prepared_vertex_cache().ensure(mapped);
                vert_override = o3d::prepared_vertex_cache().try_get(src_path);
            }
        }
        if (unique_tex) {
            if (!renderer.append_o3d(mapped, matl, vert_override)) return std::nullopt;
            return renderer.chunk_count() - 1;
        }
        if (const auto it = o3d_gpu_proto.find(key); it != o3d_gpu_proto.end()) {
            if (auto c = renderer.clone_chunk(it->second)) return *c;
        }
        if (!renderer.append_o3d(mapped, matl, vert_override)) return std::nullopt;
        const std::size_t chunk = renderer.chunk_count() - 1;
        remember_gpu_proto(o3d_gpu_proto, key, chunk);
        return chunk;
    };
    // Upload all [mesh] entries from a .sco (.o3d + .x) at world matrix; always mark GPU visible.
    auto append_sco_meshes_at = [&](const sco::SceneryObject& sco, const std::filesystem::path& sco_path,
                                    const XMMATRIX& world_m, std::int32_t inst_id, std::int32_t parent_id,
                                    std::int32_t tgx, std::int32_t tgy, std::string_view tag,
                                    bool abs_z) -> bool {
        if (is_global_entrypoint_marker_sco(sco_path)) return false;
        bool any = false;
        XMFLOAT4X4 wf{};
        DirectX::XMStoreFloat4x4(&wf, world_m);
        for (std::size_t mesh_idx = 0; mesh_idx < sco.meshes.size(); ++mesh_idx) {
            const auto& mesh_ref = sco.meshes[mesh_idx];
            if (mesh_ref.is_o3d) {
                const auto stem = mesh_ref.mesh_file.stem().string();
                if (stem == "empty_lod" || stem == "empty") continue;
                auto mapped = o3d::load_mapped_shared(mesh_ref.mesh_file);
                if (!mapped || !mapped->valid()) continue;
                auto matl_store = sco_matl_cfg(sco, mesh_ref.mesh_file);
                const model_cfg::ModelCfgMeshEntry* matl = &matl_store;
                const auto root = o3d::o3d_unity_root_transform(mapped->mesh().object_rows());
                const XMMATRIX mesh_world = ma4_to_xm(root) * world_m;
                const auto chunk_opt = append_or_clone_o3d(*mapped, mesh_ref.mesh_file, matl, sco_path);
                if (!chunk_opt) continue;
                const std::size_t chunk = *chunk_opt;
                renderer.set_chunk_world_matrix(chunk, store_world_transposed(mesh_world).data());
                renderer.set_chunk_visible(chunk, true);
                {
                    const bool paint = sco_paint_at_foot(sco);
                    apply_sco_draw_phase(chunk, paint ? sco::ScoRenderPriority::Surface : sco.render_priority,
                                         sco.surface || paint);
                    if (sco.special == sco::ScoSpecial::EntryPoint || sco.special == sco::ScoSpecial::Carpark)
                        renderer.set_chunk_editor_helper(chunk, true);
                }
                ScenePlace sp{};
                sp.object_id = inst_id;
                sp.chunk = chunk;
                sp.parent_id = parent_id;
                sp.tile_gx = tgx;
                sp.tile_gy = tgy;
                sp.label = "id=" + std::to_string(inst_id) + " " + std::string(tag) + " " +
                           sco_path.filename().string();
                sp.tx = wf._41;
                sp.ty = wf._42;
                sp.tz = wf._43;
                {
                    // bounds from the mesh, measured from the object's origin (the click test's sphere
                    // must reach meshes far off it; 2 m missed anything bigger than a bin)
                    float bmn[3]{}, bmx[3]{};
                    mesh_local_bbox(*mapped, bmn, bmx);
                    sp.half_xy = (std::max)(1.f, std::sqrt((std::max)(bmn[0] * bmn[0], bmx[0] * bmx[0]) +
                                                           (std::max)(bmn[1] * bmn[1], bmx[1] * bmx[1])));
                    sp.height = (std::max)(1.f, 2.f * (std::max)(std::fabs(bmn[2]), std::fabs(bmx[2])));
                }
                sp.abs_z = abs_z;
                sp.alive = true;
                sp.pinned = (tag == "[splineAttach]");
                sp.sco_mesh_index = static_cast<int>(mesh_idx);
                sp.lod_index = mesh_ref.lod_index;
                sp.lod_screensize = mesh_ref.lod_screensize;
                DirectX::XMStoreFloat4x4(&sp.base_world, mesh_world);
                sp.has_base_world = true;
                scene_places.push_back(std::move(sp));
                if (tag == "[splineAttach]" && inst_id == 14355) {
                    XMFLOAT4X4 mf{};
                    DirectX::XMStoreFloat4x4(&mf, mesh_world);
                    log::info("splineAttachGPU spawn id=" + std::to_string(inst_id) + " sco=" +
                              sco_path.filename().string() + " mesh=" + mesh_ref.mesh_file.filename().string() +
                              " kind=o3d chunk=" + std::to_string(chunk) + " tile=(" + std::to_string(tgx) + "," +
                              std::to_string(tgy) + ") pose=(" + std::to_string(wf._41) + "," +
                              std::to_string(wf._42) + "," + std::to_string(wf._43) + ") gpuPivot=(" +
                              std::to_string(mf._41) + "," + std::to_string(mf._42) + "," +
                              std::to_string(mf._43) + ") abs_z=" + (abs_z ? "1" : "0"));
                }
                any = true;
                } else if (!sco.tree.valid) {
                if (spawn_x_parts(sco, sco_path, mesh_ref, static_cast<int>(mesh_idx), world_m, inst_id, parent_id,
                                  tgx, tgy, tag, abs_z, wf._41, wf._42, wf._43)) {
                    if (tag == "[splineAttach]" && inst_id == 14355) {
                        XMFLOAT4X4 mf{};
                        DirectX::XMStoreFloat4x4(&mf, world_m);
                        log::info("splineAttachGPU spawn id=" + std::to_string(inst_id) + " sco=" +
                                  sco_path.filename().string() + " mesh=" + mesh_ref.mesh_file.filename().string() +
                                  " kind=x tile=(" + std::to_string(tgx) + "," + std::to_string(tgy) + ") pose=(" +
                                  std::to_string(wf._41) + "," + std::to_string(wf._42) + "," +
                                  std::to_string(wf._43) + ") gpuPivot=(" + std::to_string(mf._41) + "," +
                                  std::to_string(mf._42) + "," + std::to_string(mf._43) + ") abs_z=" +
                                  (abs_z ? "1" : "0"));
                    }
                    any = true;
                }
            }
        }
        return any;
    };

    if (defer_boot_gpu) {
        log::info("boot GPU spawn deferred → stream UploadScenery (OMSI_SYNC_BOOT_GPU=1 for sync)");
    }

    for (const auto& pm : world.meshes()) {
        if (defer_boot_gpu) break;
        if (!pm.o3d || !pm.o3d->valid()) continue;
        const auto stem = pm.source_path.stem().string();
        if (stem == "empty_lod" || stem == "empty") continue;
        const map::MapObjectInst* obj_skip = find_object(world, pm.object_id);
        if (!obj_skip) continue; // [attachObj] / spline-row mesh: hung on its parent by the editor itself
        const model_cfg::ModelCfgMeshEntry* matl = nullptr;
        model_cfg::ModelCfgMeshEntry matl_store{};
        if (const map::MapObjectInst* obj0 = find_object(world, pm.object_id)) {
            if (const auto sco = sco::load_sco(obj0->sco_path)) {
                matl_store = sco_matl_cfg(*sco, pm.source_path);
                matl = &matl_store;
            }
        }
        const auto chunk_opt = append_or_clone_o3d(*pm.o3d, pm.source_path, matl, pm.sco_path);
        if (!chunk_opt) {
            log::warn("append_o3d failed: " + pm.source_path.filename().string());
            continue;
        }
        note_spawn_item();
        const std::size_t chunk = *chunk_opt;
        const map::MapObjectInst* obj = find_object(world, pm.object_id);
        if (obj) {
            if (const auto sco = sco::load_sco(obj->sco_path)) {
                const bool paint = sco_paint_at_foot(*sco);
                apply_sco_draw_phase(chunk, paint ? sco::ScoRenderPriority::Surface : sco->render_priority,
                                     sco->surface || paint);
                if (sco->special == sco::ScoSpecial::EntryPoint || sco->special == sco::ScoSpecial::Carpark)
                    renderer.set_chunk_editor_helper(chunk, true);
            }
            else if (matl && matl->presurface)
                renderer.set_chunk_presurface(chunk, true);
        } else if (matl && matl->presurface) {
            renderer.set_chunk_presurface(chunk, true);
        }
        XMMATRIX world_m = XMMatrixIdentity();
        ScenePlace sp{};
        sp.object_id = pm.object_id;
        sp.chunk = chunk;
        sp.tile_gx = pm.tile_grid_x;
        sp.tile_gy = pm.tile_grid_y;
        sp.label = "id=" + std::to_string(pm.object_id) + " " + pm.sco_path.filename().string();
            if (obj) {
            const auto [rx, ry] = tile_rel(pm.tile_grid_x, pm.tile_grid_y);
            bool abs_z = false;
            bool ground_layer = false;
            if (const auto sco = sco::load_sco(obj->sco_path)) {
                abs_z = sco::skip_terrain_add(*sco);
                ground_layer = sco_ground_layer(*obj, *sco);
                sp.help_arrow = sco->special == sco::ScoSpecial::HelpArrow;
                for (std::size_t mi = 0; mi < sco->meshes.size(); ++mi) {
                    const auto& mr = sco->meshes[mi];
                    if (mr.mesh_file == pm.source_path || mr.mesh_file.filename() == pm.source_path.filename()) {
                        sp.sco_mesh_index = static_cast<int>(mi);
                        sp.lod_index = mr.lod_index;
                        sp.lod_screensize = mr.lod_screensize;
                        break;
                    }
                }
            }
            const float th = terrain_add(abs_z, obj->position.x + rx, obj->position.y + ry,
                                         obj->position.z, ground_layer);
            world_m = sco_world_matrix(*obj, pm.o3d.get(), th, rx, ry);
            sp.tx = obj->position.x + rx;
            sp.ty = obj->position.y + ry;
            sp.tz = obj->position.z + th;
            sp.abs_z = abs_z;
            float bmin[3]{}, bmax[3]{};
            mesh_local_bbox(*pm.o3d, bmin, bmax);
            // radius from the object's origin (meshes are often far off it: half the extent missed them)
            sp.half_xy = (std::max)(1.f, std::sqrt((std::max)(bmin[0] * bmin[0], bmax[0] * bmax[0]) +
                                                   (std::max)(bmin[1] * bmin[1], bmax[1] * bmax[1])));
            sp.height = (std::max)(1.f, bmax[2] - bmin[2]);
            const auto dbg =
                make_spawn_debug(*obj, "o3d", pm.source_path, world_m, th, tile_ox, tile_oy, pm.o3d.get());
            if (dbg.ok && spawn_ok < 8) log_spawn_debug(dbg);
            if (dbg.ok) ++spawn_ok;
            else ++spawn_bad;
        }
        const auto wt = store_world_transposed(world_m);
        renderer.set_chunk_world_matrix(chunk, wt.data());
        // FIX 2026-09-24: always force visible after matrix (stream path already did this;
        // initial spawn relied on prototype state and clones could stay hidden).
        renderer.set_chunk_visible(chunk, true);
        if (obj) {
            DirectX::XMStoreFloat4x4(&sp.base_world, world_m); // mesh animator base (clock hands ...)
            sp.has_base_world = true;
        }
        scene_places.push_back(std::move(sp));
    }

    // .x meshes from .sco (entrypoint_bus.x, etc.). Skip when [tree] billboard is used (treehelper.x).
    for (const auto& tile : world.tiles()) {
        if (defer_boot_gpu) break;
        const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
        for (const auto& obj : tile.objects) {
            const auto sco = sco::load_sco(obj.sco_path);
            if (!sco || sco->tree.valid) continue;
            for (std::size_t mesh_idx = 0; mesh_idx < sco->meshes.size(); ++mesh_idx) {
                const auto& mesh_ref = sco->meshes[mesh_idx];
                if (mesh_ref.is_o3d) continue;
                const bool abs_z = sco::skip_terrain_add(*sco);
                const bool ground_layer = sco_ground_layer(obj, *sco);
                const float th = terrain_add(abs_z, obj.position.x + rx, obj.position.y + ry,
                                             obj.position.z, ground_layer);
                const float yaw = obj.rotation_deg.x;
                const float pitch = obj.rotation_deg.y;
                const float bank = obj.rotation_deg.z;
                const XMMATRIX world_m = omsi::app::map_editor_math::omsi_object_rotation_matrix(yaw, pitch, bank) *
                                         XMMatrixTranslation(obj.position.x + rx, obj.position.y + ry,
                                                           obj.position.z + th);
                spawn_x_parts(*sco, obj.sco_path, mesh_ref, static_cast<int>(mesh_idx), world_m, obj.id, 0,
                              tile.grid_x, tile.grid_y, "[x]", abs_z, obj.position.x + rx, obj.position.y + ry,
                              obj.position.z + th, sco->special == sco::ScoSpecial::HelpArrow);
            }
        }
    }

    int tree_count = 0;
    std::unordered_map<std::string, std::size_t> tree_gpu_proto;
    std::unordered_map<std::string, std::filesystem::path> tree_tex_cache;
    const auto unit_tree = editor::make_tree_billboard_mesh(1.f, 1.f);
    for (const auto& tile : world.tiles()) {
        if (defer_boot_gpu) break;
        const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
        for (const auto& obj : tile.objects) {
            const auto sco = sco::load_sco(obj.sco_path);
            if (!sco || !sco->tree.valid) continue;

            float height = sco->tree.height;
            float aspect = sco->tree.ratio;
            if (obj.obj_type == 4) {
                if (obj.tree_height > 0.01f) height = obj.tree_height;
                if (obj.tree_aspect > 0.01f) aspect = obj.tree_aspect;
            }
            const float width =
                editor::tree_billboard_width(height, aspect, sco->tree.width, sco->tree.ratio);

            const std::string tex_name = !obj.tree_texture.empty() ? obj.tree_texture : sco->tree.texture;
            const std::string tex_key = obj.sco_path.string() + "|" + tex_name;
            std::filesystem::path tex_resolved;
            if (const auto it = tree_tex_cache.find(tex_key); it != tree_tex_cache.end()) {
                tex_resolved = it->second;
            } else {
                const auto tex_path = gfx::dx12::resolve_texture_path(obj.sco_path, tex_name, cfg.omsi_root);
                if (!tex_path) {
                    log::warn("SPAWN tree tex miss id=" + std::to_string(obj.id) + " name=" + tex_name);
                    continue;
                }
                tex_resolved = *tex_path;
                tree_tex_cache.emplace(tex_key, tex_resolved);
            }

            // OMSI TKachelForest: one mesh per texture, instance scale in matrix (TreePostCalc).
            std::size_t chunk = 0;
            const std::string proto_key = tex_resolved.lexically_normal().string();
            bool have_chunk = false;
            if (const auto it = tree_gpu_proto.find(proto_key); it != tree_gpu_proto.end()) {
                if (auto c = renderer.clone_chunk(it->second)) {
                    chunk = *c;
                    have_chunk = true;
                }
            }
            if (!have_chunk) {
                if (!renderer.append_textured_mesh(unit_tree.vertices.data(),
                                                   static_cast<std::uint32_t>(unit_tree.vertices.size()),
                                                   unit_tree.indices.data(),
                                                   static_cast<std::uint32_t>(unit_tree.indices.size()),
                                                   tex_resolved, 1u))
                    continue;
                chunk = renderer.chunk_count() - 1;
                remember_gpu_proto(tree_gpu_proto, proto_key, chunk);
                renderer.set_chunk_wind_sway(chunk, true); // tree prototype: wind sway (clones inherit)
            }
            const bool abs_z = sco::skip_terrain_add(*sco);
            const float th = terrain_add(abs_z, obj.position.x + rx, obj.position.y + ry);
            const XMMATRIX S = DirectX::XMMatrixScaling(width, width, height);
            const XMMATRIX world_m = S * sco_world_matrix(obj, nullptr, th, rx, ry);
            const auto wt = store_world_transposed(world_m);
            renderer.set_chunk_world_matrix(chunk, wt.data());
            renderer.set_chunk_visible(chunk, true); // FIX 2026-09-24 invisible trees

            const auto dbg = make_spawn_debug(obj, "tree", obj.sco_path, world_m, th, tile_ox, tile_oy, nullptr,
                                              width * 0.5f, width * 0.5f, height);
            if (dbg.ok && tree_count < 3) log_spawn_debug(dbg);
            if (dbg.ok) ++spawn_ok;
            else ++spawn_bad;

            ScenePlace sp{};
            sp.object_id = obj.id;
            sp.chunk = chunk;
            sp.tile_gx = tile.grid_x;
            sp.tile_gy = tile.grid_y;
            sp.label = "id=" + std::to_string(obj.id) + " [tree] " + obj.sco_path.filename().string();
            sp.tx = obj.position.x + rx;
            sp.ty = obj.position.y + ry;
            sp.tz = obj.position.z + th;
            sp.abs_z = abs_z;
            sp.half_xy = width * 0.5f;
            sp.height = height;
            sp.is_tree = true;
            scene_places.push_back(std::move(sp));
            ++tree_count;
            note_spawn_item();
        }
    }
    // Defer path: keep prefetch running into stream UploadScenery (join at kick). Sync path: wait here.
    if (!defer_boot_gpu && spawn_tex_thread.joinable()) spawn_tex_thread.join();

    // scene_places is append-only for the life of the editor: dead chunks are marked alive=false
    // rather than erased. Index every appended place once so checking an object's GPU residency is
    // O(number of meshes for that object), not O(all scene chunks). Streaming can ask this thousands
    // of times while applying a tile; the old linear scan made that path quadratic on large maps.
    std::unordered_map<std::int32_t, std::vector<std::size_t>> scene_place_indices_by_object;
    std::size_t scene_places_indexed_n = 0;
    auto object_has_alive_gpu = [&](const std::int32_t oid) {
        if (scene_places.size() < scene_places_indexed_n) {
            // Defensive fallback if a future code path starts replacing the scene vector.
            scene_place_indices_by_object.clear();
            scene_places_indexed_n = 0;
        }
        for (std::size_t i = scene_places_indexed_n; i < scene_places.size(); ++i) {
            const auto id = scene_places[i].object_id;
            if (id != 0) scene_place_indices_by_object[id].push_back(i);
        }
        scene_places_indexed_n = scene_places.size();
        const auto it = scene_place_indices_by_object.find(oid);
        if (it == scene_place_indices_by_object.end()) return false;
        for (const std::size_t i : it->second) {
            if (i < scene_places.size() && scene_places[i].alive) return true;
        }
        return false;
    };
    // Shared by boot, stream, and instance-list select. world_loader skips [tree] / .x-only SCOs,
    // so the GPU path here is the only one that actually places those instances.
    // ---- Missing assets: a translucent magenta cube + a label ("falta objeto X.sco") where a .sco /
    // .sli / .bus the map uses is not installed, instead of an empty spot. Also used by co-op sessions,
    // which never transfer objects: the guest sees what it lacks.
    struct MissingAsset {
        bool spline = false;
        std::int32_t id = 0;
        float x = 0.f, y = 0.f, z = 0.f;
        std::string name;
        std::size_t chunk = static_cast<std::size_t>(-1);
    };
    std::unordered_map<std::int64_t, MissingAsset> missing_assets;
    std::vector<o3d::Vertex> miss_cube_v;
    std::vector<std::uint32_t> miss_cube_i;
    {
        // unit cube, 6 faces x 4 corners (own normals), both windings
        const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (int f = 0; f < 6; ++f) {
            const float nx = n[f][0], ny = n[f][1], nz = n[f][2];
            // two axes spanning the face
            const float ux = (nz != 0.f) ? 1.f : 0.f, uy = (nx != 0.f) ? 1.f : 0.f, uz = (ny != 0.f) ? 1.f : 0.f;
            const float vx = ny * uz - nz * uy, vy = nz * ux - nx * uz, vz = nx * uy - ny * ux;
            const std::uint32_t b = static_cast<std::uint32_t>(miss_cube_v.size());
            const float cs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto& c : cs) {
                o3d::Vertex v{};
                v.position[0] = 0.5f * (nx + c[0] * ux + c[1] * vx);
                v.position[1] = 0.5f * (ny + c[0] * uy + c[1] * vy);
                v.position[2] = 0.5f * (nz + c[0] * uz + c[1] * vz);
                v.normal[0] = nx;
                v.normal[1] = ny;
                v.normal[2] = nz;
                miss_cube_v.push_back(v);
            }
            miss_cube_i.insert(miss_cube_i.end(), {b, b + 1, b + 2, b, b + 2, b + 3, b + 2, b + 1, b, b + 3, b + 2, b});
        }
    }
    auto missing_key = [](bool spline, std::int32_t id) {
        return (static_cast<std::int64_t>(spline ? 1 : 0) << 40) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(id));
    };
    auto mark_missing_asset = [&](bool spline, std::int32_t id, float x, float y, float z, const std::string& name) {
        auto& m = missing_assets[missing_key(spline, id)];
        m.spline = spline;
        m.id = id;
        m.x = x;
        m.y = y;
        m.z = z;
        m.name = name;
        if (m.chunk == static_cast<std::size_t>(-1)) {
            if (!renderer.append_textured_mesh(miss_cube_v.data(), static_cast<std::uint32_t>(miss_cube_v.size()),
                                               miss_cube_i.data(), static_cast<std::uint32_t>(miss_cube_i.size()),
                                               {}, 0u))
                return;
            m.chunk = renderer.chunk_count() - 1;
        }
        const float sz = spline ? 1.5f : 2.f;
        const auto wt = store_world_transposed(DirectX::XMMatrixScaling(sz, sz, sz) *
                                               DirectX::XMMatrixTranslation(x, y, z + sz * 0.5f));
        renderer.set_chunk_world_matrix(m.chunk, wt.data());
        renderer.set_chunk_highlight(m.chunk, 1.f, 1.f, 0.15f, 0.85f);
        renderer.set_chunk_opacity(m.chunk, 0.55f);
        renderer.set_chunk_visible(m.chunk, true);
    };
    auto clear_missing_asset = [&](bool spline, std::int32_t id) {
        const auto it = missing_assets.find(missing_key(spline, id));
        if (it == missing_assets.end()) return;
        if (it->second.chunk != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(it->second.chunk, false);
        missing_assets.erase(it);
    };
    auto spawn_one_object_gpu = [&](map::MapObjectInst& o, const std::int32_t tgx, const std::int32_t tgy) {
        if (editor_deleted_object_ids.count(o.id)) return;
        if (is_global_entrypoint_marker_sco(o.sco_path)) return;
        if (object_has_alive_gpu(o.id)) return;
        const sco::SceneryObject* sco = cached_sco(o.sco_path);
        if (!sco) {
            log::warn("spawn GPU miss id=" + std::to_string(o.id) + " sco missing " +
                      o.sco_path.filename().string());
            if (!o.sco_path.empty())
                mark_missing_asset(false, o.id, o.position.x, o.position.y,
                                   o.position.z + sample_terrain_editor_xy(o.position.x, o.position.y),
                                   o.sco_path.filename().string());
            return;
        }
        // [splineAttachement] objects hold the spline's world Z already (trees on a spline came here
        // and got the terrain added a second time: 2x the height, floating hundreds of metres up)
        const bool abs_z = sco::skip_terrain_add(*sco) || spline_attach_spawn_ids.count(o.id) != 0;
        const bool ground_layer = sco_ground_layer(o, *sco);
        const float th = terrain_add(abs_z, o.position.x, o.position.y, o.position.z, ground_layer);
        if (sco->tree.valid) {
            float height = sco->tree.height;
            float aspect = sco->tree.ratio;
            if (o.obj_type == 4) {
                if (o.tree_height > 0.01f) height = o.tree_height;
                if (o.tree_aspect > 0.01f) aspect = o.tree_aspect;
            }
            const float width =
                editor::tree_billboard_width(height, aspect, sco->tree.width, sco->tree.ratio);
            const std::string tex_name = !o.tree_texture.empty() ? o.tree_texture : sco->tree.texture;
            const std::string tex_key = o.sco_path.string() + "|" + tex_name;
            std::filesystem::path tex_resolved;
            if (const auto it = tree_tex_cache.find(tex_key); it != tree_tex_cache.end()) {
                tex_resolved = it->second;
            } else if (const auto tex_path =
                           gfx::dx12::resolve_texture_path(o.sco_path, tex_name, cfg.omsi_root)) {
                tex_resolved = *tex_path;
                tree_tex_cache.emplace(tex_key, tex_resolved);
            }
            if (tex_resolved.empty()) {
                log::warn("spawn GPU tree no tex id=" + std::to_string(o.id) + " " + tex_name);
                return;
            }
            std::size_t chunk = 0;
            const std::string proto_key = tex_resolved.lexically_normal().string();
            bool ok = false;
            if (const auto it = tree_gpu_proto.find(proto_key); it != tree_gpu_proto.end()) {
                if (auto c = renderer.clone_chunk(it->second)) {
                    chunk = *c;
                    ok = true;
                }
            }
            if (!ok && renderer.append_textured_mesh(
                           unit_tree.vertices.data(), static_cast<std::uint32_t>(unit_tree.vertices.size()),
                           unit_tree.indices.data(), static_cast<std::uint32_t>(unit_tree.indices.size()),
                           tex_resolved, 1u)) {
                chunk = renderer.chunk_count() - 1;
                remember_gpu_proto(tree_gpu_proto, proto_key, chunk);
                renderer.set_chunk_wind_sway(chunk, true); // tree prototype: wind sway (clones inherit)
                ok = true;
            }
            if (!ok) return;
            ScenePlace sp{};
            sp.object_id = o.id;
            sp.chunk = chunk;
            sp.tile_gx = tgx;
            sp.tile_gy = tgy;
            sp.tx = o.position.x;
            sp.ty = o.position.y;
            sp.tz = o.position.z + th;
            sp.abs_z = abs_z;
            sp.alive = true;
            sp.is_tree = true;
            sp.half_xy = width * 0.5f;
            sp.height = height;
            const XMMATRIX world_m =
                DirectX::XMMatrixScaling(width, width, height) * sco_world_matrix(o, nullptr, th, 0.f, 0.f);
            renderer.set_chunk_world_matrix(sp.chunk, store_world_transposed(world_m).data());
            renderer.set_chunk_visible(sp.chunk, true);
            scene_places.push_back(std::move(sp));
            ++tree_count;
            return;
        }
        if (!sco->meshes.empty()) {
            const XMMATRIX world_m = sco_world_matrix(o, nullptr, th, 0.f, 0.f);
            append_sco_meshes_at(*sco, o.sco_path, world_m, o.id, 0, tgx, tgy, "[spawn]", abs_z);
        }
    };
    auto tile_grid_for_object_id = [&](const std::int32_t oid) -> std::pair<std::int32_t, std::int32_t> {
        for (const auto& tile : world.tiles()) {
            for (const auto& obj : tile.objects) {
                if (obj.id == oid) return {tile.grid_x, tile.grid_y};
            }
        }
        return {tile_gx, tile_gy};
    };

    auto is_attach_child_id = [&](std::int32_t id) -> bool {
        for (const auto& a : edit_attachments)
            if (a.child_id == id) return true;
        return false;
    };
    // Terrain is applied once to the root [object]. Nested attachObj parents already
    // store world Z, so do not add terrain a second time for attachment children.
    // Kept in setup_01 because setup_02's live attachment-sync path also uses it.
    // The parent as it stands in the scene (its place's Z holds the terrain it was put on). Recomputed
    // from the terrain instead, a parent in a tile whose terrain was not loaded yet got 0 and its
    // attachments were hung metres under the ground (strsgnpN's sign, Hamburg).
    auto parent_place_tz = [&](std::int32_t id) -> std::optional<float> {
        if (!object_has_alive_gpu(id)) return std::nullopt;
        const auto it = scene_place_indices_by_object.find(id);
        if (it == scene_place_indices_by_object.end()) return std::nullopt;
        for (const std::size_t i : it->second)
            if (i < scene_places.size() && scene_places[i].alive) return scene_places[i].tz;
        return std::nullopt;
    };
    auto attach_parent_terrain_h = [&](const map::MapObjectInst& parent) -> float {
        if (const auto tz = parent_place_tz(parent.id)) return *tz - parent.position.z;
        if (is_attach_child_id(parent.id)) return 0.f;
        if (spline_attach_spawn_ids.count(parent.id)) return 0.f; // spline row objects hold world Z
        const auto parent_sco = sco::load_sco(parent.sco_path);
        const bool parent_abs = parent_sco && sco::skip_terrain_add(*parent_sco);
        return terrain_add(parent_abs, parent.position.x, parent.position.y);
    };
    // [attachObj]: Unity ObjectAttachmentManager — object parent only, multi-pass until parent exists.
    int attach_ok = 0, attach_skip = 0;
    std::unordered_set<std::int32_t> attach_spawned;
    std::unordered_set<std::int32_t> attach_never; // refused / nothing to draw: not tried again
    std::unordered_map<std::int32_t, float> attach_parent_tz_used; // child -> its parent's place Z when hung
    // Set when a pass stopped on its time budget, or when new parents appeared (spline
    // attachment instances): the per-frame visual pump keeps calling until it is clear. Before,
    // the rest only resumed on a camera move, so at 1 GHz many [attachObj] never appeared.
    bool attach_pending = false;
    bool attach_wait_parent = false; // children waiting for their parent's meshes: retried once a second
    double attach_wait_t = 0.0;
    // openOMSI stages a tile's [attachObj]s from the tile itself every time it is placed: the loaded
    // tiles are the source. The editor's own list lost streamed ones on the way (strsgnpN's sign was in
    // the tile and never in the list); whatever a loaded tile has and the list does not is added here.
    std::size_t attach_sync_sig = 0;
    auto sync_attachments_from_tiles = [&]() {
        std::size_t sig = world.tiles().size() * 1000003u;
        for (const auto& t : world.tiles()) sig = sig * 31u + t.attachments.size() + static_cast<std::size_t>(t.grid_x * 7 + t.grid_y);
        sig ^= edit_attachments.size() * 2654435761u;
        if (sig == attach_sync_sig) return;
        std::unordered_set<std::int32_t> have;
        have.reserve(edit_attachments.size() * 2 + 1);
        for (const auto& a : edit_attachments) have.insert(a.child_id);
        std::size_t added = 0;
        for (const auto& t : world.tiles())
            for (const auto& a : t.attachments) {
                if (editor_deleted_object_ids.count(a.child_id)) continue;
                if (have.insert(a.child_id).second) {
                    edit_attachments.push_back(a);
                    ++added;
                }
            }
        if (added) log::info("attachObj sync from tiles +" + std::to_string(added));
        attach_sync_sig = world.tiles().size() * 1000003u;
        for (const auto& t : world.tiles())
            attach_sync_sig = attach_sync_sig * 31u + t.attachments.size() + static_cast<std::size_t>(t.grid_x * 7 + t.grid_y);
        attach_sync_sig ^= edit_attachments.size() * 2654435761u;
    };
    auto spawn_edit_attachments = [&](const auto& in_radius_fn) {
        attach_pending = false;
        sync_attachments_from_tiles();
        // Build O(1) indices for this pass. edit_objects and scene_places may grow during spawning,
        // but are not erased by this function; update the object map whenever we append a child.
        std::unordered_map<std::int32_t, std::size_t> edit_obj_index;
        edit_obj_index.reserve(edit_objects.size() * 2 + edit_attachments.size() + 8);
        for (std::size_t i = 0; i < edit_objects.size(); ++i)
            edit_obj_index[edit_objects[i].id] = i;
        auto find_edit_obj = [&](std::int32_t id) -> map::MapObjectInst* {
            const auto it = edit_obj_index.find(id);
            return it != edit_obj_index.end() && it->second < edit_objects.size()
                       ? &edit_objects[it->second]
                       : nullptr;
        };
        std::unordered_map<std::int32_t, std::pair<std::int32_t, std::int32_t>> parent_tiles;
        parent_tiles.reserve(scene_places.size() / 2 + 8);
        for (const auto& sp : scene_places) {
            if (sp.alive && sp.object_id != 0)
                parent_tiles.try_emplace(sp.object_id, sp.tile_gx, sp.tile_gy);
        }
        std::unordered_set<std::int32_t> attach_child_ids;
        attach_child_ids.reserve(edit_attachments.size() * 2 + 8);
        for (const auto& attachment : edit_attachments) attach_child_ids.insert(attachment.child_id);

        // [PERF] Time-budget guard: single heavy attachment can take 50-424ms.
        // Stop after the budget expires so the frame can be presented;
        // attach_spawned persists across calls so progress is never lost.
        const auto spawn_t0 = std::chrono::steady_clock::now();
        constexpr double kAttachBudgetMs = 8.0; // max ms per frame for attachment spawning
        auto attach_over_budget = [&]() -> bool {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - spawn_t0).count() >= kAttachBudgetMs;
        };

        // Resolve dependencies over as many passes as necessary, but stop as soon as a pass makes
        // no progress. The previous loop always executed edit_attachments.size() full passes even
        // when every child had already been spawned, producing O(N^2) work on every camera move.
        const std::size_t attach_passes = edit_attachments.size();
        for (std::size_t pass = 0; pass < attach_passes; ++pass) {
            bool made_progress = false;
            for (const auto& a : edit_attachments) {
                if (attach_spawned.count(a.child_id)) {
                    // its meshes went with a tile that streamed out (the parent came back with the tile,
                    // the child stayed "spawned" and never appeared again): hang it on once more
                    if (attach_never.count(a.child_id)) continue;
                    if (object_has_alive_gpu(a.child_id)) {
                        // the parent was put down again elsewhere (its terrain came in): follow it
                        const auto used = attach_parent_tz_used.find(a.child_id);
                        const auto now_tz = parent_place_tz(a.parent_id);
                        if (used == attach_parent_tz_used.end() || !now_tz || std::fabs(*now_tz - used->second) < 0.05f)
                            continue;
                        if (const auto pit = scene_place_indices_by_object.find(a.child_id);
                            pit != scene_place_indices_by_object.end()) {
                            for (const std::size_t i : pit->second) {
                                if (i >= scene_places.size() || !scene_places[i].alive) continue;
                                scene_places[i].alive = false;
                                renderer.set_chunk_visible(scene_places[i].chunk, false);
                            }
                        }
                    }
                    attach_spawned.erase(a.child_id);
                }
                // an object on a later object of a spline row (instance != 0): Omsi.exe refuses it
                // ("nicht mehr unterstuetzt") and openOMSI drops it
                if (a.extra_rot_y != 0.f) {
                    ++attach_skip;
                    attach_never.insert(a.child_id);
                    attach_spawned.insert(a.child_id);
                    made_progress = true;
                    continue;
                }
                map::MapObjectInst* parent = find_edit_obj(a.parent_id);
                if (!parent) continue;
                if (!in_radius_fn(parent->position.x, parent->position.y)) continue;

                const auto child_sco = sco::load_sco(a.sco_path);
                if (!child_sco || child_sco->meshes.empty()) {
                    ++attach_skip;
                    attach_never.insert(a.child_id);
                    attach_spawned.insert(a.child_id);
                    made_progress = true;
                    continue;
                }
                // Use this pass' child-id index instead of scanning the entire attachment list
                // for every parent. Large nested attachment sets otherwise trend toward O(N^2).
                float pth = 0.f;
                const auto parent_tz = parent_place_tz(parent->id);
                if (parent_tz) {
                    pth = *parent_tz - parent->position.z;
                    attach_parent_tz_used[a.child_id] = *parent_tz;
                } else {
                    const auto parent_sco = sco::load_sco(parent->sco_path);
                    if (parent_sco && !parent_sco->meshes.empty()) {
                        attach_wait_parent = true; // hang it on once its parent stands in the scene
                        continue;
                    }
                    if (!attach_child_ids.count(parent->id) && !spline_attach_spawn_ids.count(parent->id)) {
                        const bool parent_abs = parent_sco && sco::skip_terrain_add(*parent_sco);
                        pth = terrain_add(parent_abs, parent->position.x, parent->position.y);
                    }
                }
                const auto child_world_opt = attach_child_world_matrix(a, parent, nullptr, pth, std::nullopt);
                if (!child_world_opt) {
                    ++attach_skip;
                    attach_spawned.insert(a.child_id);
                    made_progress = true;
                    continue;
                }
                const XMMATRIX child_world = *child_world_opt;
                std::int32_t agx = tile_gx, agy = tile_gy;
                if (const auto it = parent_tiles.find(a.parent_id); it != parent_tiles.end()) {
                    agx = it->second.first;
                    agy = it->second.second;
                }
                const bool spawned_any = append_sco_meshes_at(*child_sco, a.sco_path, child_world, a.child_id,
                                                              a.parent_id, agx, agy, "[attach]", true);
                {
                    static int s_att_log = 0;
                    if (s_att_log < 40) {
                        ++s_att_log;
                        XMFLOAT4X4 cf{};
                        DirectX::XMStoreFloat4x4(&cf, child_world);
                        log::info("attach hang child=" + std::to_string(a.child_id) + " parent=" + std::to_string(a.parent_id) +
                                  " world=(" + std::to_string(cf._41) + "," + std::to_string(cf._42) + "," +
                                  std::to_string(cf._43) + ") parent_pos=(" + std::to_string(parent->position.x) + "," +
                                  std::to_string(parent->position.y) + ") pth=" + std::to_string(pth) + " ok=" +
                                  std::to_string(spawned_any ? 1 : 0));
                    }
                }
                if (spawned_any) {
                    if (!find_edit_obj(a.child_id)) {
                        map::MapObjectInst child{};
                        child.id = a.child_id;
                        child.sco_path = a.sco_path;
                        child.position.x = scene_places.back().tx;
                        child.position.y = scene_places.back().ty;
                        child.position.z = scene_places.back().tz;
                        // the angles of the world matrix itself, so a child of this child turns with it
                        omsi_euler_from_matrix(child_world, child.rotation_deg.x, child.rotation_deg.y,
                                               child.rotation_deg.z);
                        child.varparent_id = a.varparent_id;
                        child.strings = a.object_text_lines;
                        child.string_line_count = a.object_text_line_count > 0
                                                      ? a.object_text_line_count
                                                      : static_cast<std::int32_t>(a.object_text_lines.size());
                        edit_objects.push_back(std::move(child));
                        edit_obj_index[a.child_id] = edit_objects.size() - 1;
                        parent_tiles[a.child_id] = {agx, agy};
                    } else if (auto* existing = find_edit_obj(a.child_id)) {
                        if (existing->varparent_id == 0) existing->varparent_id = a.varparent_id;
                        if (a.object_text_line_count > 0 || !a.object_text_lines.empty()) {
                            existing->strings = a.object_text_lines;
                            existing->string_line_count = a.object_text_line_count > 0
                                                              ? a.object_text_line_count
                                                              : static_cast<std::int32_t>(a.object_text_lines.size());
                        }
                    }
                    parent_tiles[a.child_id] = {agx, agy};
                    ++attach_ok;
                } else {
                    ++attach_skip;
                }
                attach_spawned.insert(a.child_id);
                made_progress = true;
                // [PERF] Hard time-budget: stop after this attachment if we've exceeded the
                // per-frame budget. attach_spawned is persistent so the next call resumes.
                if (attach_over_budget()) {
                    attach_pending = true;
                    return;
                }
            }
            if (!made_progress) break;
        }
    };
    spawn_edit_attachments(visual_spawn_in_radius);
    log::info("editor attachments ok=" + std::to_string(attach_ok) +
              " skip=" + std::to_string(attach_skip));
    log::info("editor CPU radius " +
              (editor_cpu_radius_m <= 0.f ? std::string("off (linked to visual)")
                                         : std::to_string(static_cast<int>(editor_cpu_radius_m)) + " m") +
              " — linked to UI visual radius; GPU+pick stay for streaming");
    if (editor_visual_radius_m > 0.f) {
        log::info("editor visual radius " + std::to_string(static_cast<int>(editor_visual_radius_m)) +
                  "m — initial GPU spawn limited to focus");
    }

    // [splineAttachement]: along = offsetAlong - localLength (Unity / OMSI TMapKachel §060).
    // OMSI: SA → parent TFileSpline reference; file index only at save (FindFileSplIndex).
    auto resolve_edit_spline_for_attach = [&](const EditSplineAttachmentEntry& e) -> const map::SplineInst* {
        if (e.parent_spline_id > 0) {
            for (const auto& s : edit_splines) {
                if (s.id == e.parent_spline_id) return &s;
            }
        }
        const auto ref_opt = map::resolve_spline_attachment_index(
            e.att.spline_index, e.tile_gx, e.tile_gy, e.tile_spline_count, global_spline_index);
        if (ref_opt) {
            const auto it = tile_local_spline_edit_idx.find(tile_key(ref_opt->tile_gx, ref_opt->tile_gy));
            if (it != tile_local_spline_edit_idx.end() && ref_opt->local_index >= 0 &&
                static_cast<std::size_t>(ref_opt->local_index) < it->second.size()) {
                const std::size_t idx = it->second[static_cast<std::size_t>(ref_opt->local_index)];
                if (idx != static_cast<std::size_t>(-1) && idx < edit_splines.size())
                    return &edit_splines[idx];
            }
        }
        return nullptr;
    };
    auto fill_sa_parent_spline_id = [&](EditSplineAttachmentEntry& e) {
        if (e.parent_spline_id > 0) {
            if (editor_deleted_spline_ids.count(e.parent_spline_id)) e.parent_spline_id = 0;
            else {
                for (const auto& s : edit_splines)
                    if (s.id == e.parent_spline_id) return;
            }
        }
        if (const map::SplineInst* sp = resolve_edit_spline_for_attach(e))
            e.parent_spline_id = sp->id;
    };
#include "modules/mod_spline_attach_spawn.inc.cpp"

    if (!defer_boot_gpu) {
        int missing_gpu = 0;
        for (auto& o : edit_objects) {
            if (is_attach_child_id(o.id)) continue;
            if (object_has_alive_gpu(o.id)) continue;
            const auto [tgx, tgy] = tile_grid_for_object_id(o.id);
            spawn_one_object_gpu(o, tgx, tgy);
            if (object_has_alive_gpu(o.id)) ++missing_gpu;
        }
        if (missing_gpu > 0)
            log::warn("boot spawned missing GPU objects n=" + std::to_string(missing_gpu) +
                      " (list had instances without scene_places)");
    } else {
        boot_gpu_queue.clear();
        boot_gpu_queue.reserve(edit_objects.size());
        for (const auto& o : edit_objects) {
            if (is_attach_child_id(o.id)) continue;
            const auto [tgx, tgy] = tile_grid_for_object_id(o.id);
            boot_gpu_queue.push_back({o.id, tgx, tgy});
        }
        log::info("boot_gpu_queue n=" + std::to_string(boot_gpu_queue.size()) +
                  " (UploadScenery after streaming module init)");
    }
    xform_soa.rebuild_from(edit_objects);

    log::info("editor placed scos=" + std::to_string(scene_places.size() - static_cast<std::size_t>(tree_count)) +
              " trees=" + std::to_string(tree_count) + " chunks=" + std::to_string(renderer.chunk_count()) +
              " SPAWN_CHECK ok=" + std::to_string(spawn_ok) + " bad=" + std::to_string(spawn_bad) +
              (defer_boot_gpu ? " deferred=1" : " deferred=0") +
              " (pose-delta>2.5m; no per-id warn)");
    boot_trace.mark("gpu_spawn_scos_trees");
    std::unordered_map<std::int32_t, const map::MapObjectInst*> edit_obj_by_id;
    std::size_t edit_obj_index_size = 0;
    const map::MapObjectInst* edit_obj_index_data = nullptr; // storage the cached pointers refer to
    std::uint64_t edit_obj_index_signature = 0;
    auto compute_edit_obj_signature = [&]() -> std::uint64_t {
        std::uint64_t h = 1469598103934665603ull;
        for (const auto& o : edit_objects) {
            h ^= static_cast<std::uint32_t>(o.id);
            h *= 1099511628211ull;
        }
        return h;
    };
    auto ensure_edit_obj_index = [&]() {
        // Size-only fast path: full FNV over all ids every frame was O(n) for free.
        // The map holds pointers INTO edit_objects: a same-size swap (undo/redo moves in a whole new vector)
        // left them dangling, so the storage address is part of the validity check too.
        if (edit_objects.size() == edit_obj_index_size && edit_objects.data() == edit_obj_index_data) return;
        edit_obj_by_id.clear();
        edit_obj_by_id.reserve(edit_objects.size() * 2 + 1);
        for (const auto& o : edit_objects) edit_obj_by_id[o.id] = &o;
        edit_obj_index_size = edit_objects.size();
        edit_obj_index_data = edit_objects.data();
        edit_obj_index_signature = compute_edit_obj_signature();
    };

    // Compatibility helper used by later setup sections.
    // Keep the lookup O(1) by reusing the cached object index rather than
    // reintroducing the old linear scan through edit_objects.
    auto find_edit_obj_by_id = [&](std::int32_t id) -> const map::MapObjectInst* {
        ensure_edit_obj_index();
        const auto it = edit_obj_by_id.find(id);
        return it != edit_obj_by_id.end() ? it->second : nullptr;
    };

    struct ScoScriptInfo {
        const sco::SceneryObject* source = nullptr;
        bool has_visible_links = false;
        bool has_text_textures = false;
        bool has_freetex = false;
        bool has_matl_change = false;
        bool has_mesh_anims = false;
        std::vector<std::size_t> text_material_indices;
        std::vector<std::size_t> freetex_material_indices;
        std::vector<std::size_t> matl_change_material_indices;
    };
    std::unordered_map<std::string, ScoScriptInfo> sco_script_info;
    struct ScoDynamicSceneRef {
        std::size_t scene_index = 0;
        std::uint8_t flags = 0;
        // Resolved once per rebuild: load_sco/o3d_path_key normalize paths (+mutex) — per frame × refs was ~20 ms.
        const sco::SceneryObject* sco = nullptr;
        const ScoScriptInfo* info = nullptr;
    };
    constexpr std::uint8_t kScoDynVisible = 1u << 0;
    constexpr std::uint8_t kScoDynTextTex = 1u << 1;
    constexpr std::uint8_t kScoDynFreeTex = 1u << 2;
    constexpr std::uint8_t kScoDynMeshAnim = 1u << 3;
    constexpr std::uint8_t kScoDynUv = 1u << 4;
    constexpr std::uint8_t kScoDynMatlChange = 1u << 5;
    constexpr std::uint8_t kScoDynCtc = 1u << 6;
    std::vector<ScoDynamicSceneRef> sco_dynamic_scene_refs;
    std::size_t sco_dynamic_scene_size = static_cast<std::size_t>(-1);
    std::uint64_t sco_dynamic_obj_signature = 0;

    auto is_nightlight_var = [](const std::string& v) {
        std::string t;
        for (char c : v)
            if (c != ' ' && c != '\t') t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return t == "nightlighta";
    };
    auto script_info_for = [&](const std::filesystem::path& path, const sco::SceneryObject* source) -> ScoScriptInfo& {
        const auto key = o3d_path_key(path);
        auto& info = sco_script_info[key];
        if (info.source != source) {
            info = {};
            info.source = source;
            if (source) {
                info.has_visible_links = !source->visible_links.empty();
                info.has_text_textures = !source->text_textures.empty();
                info.has_mesh_anims = source->has_mesh_anims();
                for (std::size_t i = 0; i < source->materials.size(); ++i) {
                    const auto& mat = source->materials[i];
                    if (mat.use_text_texture) info.text_material_indices.push_back(i);
                    if (mat.freetex_label) info.freetex_material_indices.push_back(i);
                    // ("NightlightA" changes only switch to the night item - its light map, which the
                    //  renderer lays on by the night itself; nothing to swap here)
                    if (mat.is_matl_change && !mat.change_var.empty() && !is_nightlight_var(mat.change_var))
                        info.matl_change_material_indices.push_back(i);
                }
                info.has_freetex = !info.freetex_material_indices.empty();
                info.has_matl_change = !info.matl_change_material_indices.empty();
            }
        }
        return info;
    };

    auto rebuild_sco_dynamic_scene_refs = [&]() {
        ensure_edit_obj_index();
        sco_dynamic_scene_refs.clear();
        sco_dynamic_scene_refs.reserve(scene_places.size() / 8 + 1);
        std::unordered_map<std::filesystem::path::string_type, std::pair<const sco::SceneryObject*, ScoScriptInfo*>>
            by_raw_path;
        by_raw_path.reserve(1024);
        for (std::size_t i = 0; i < scene_places.size(); ++i) {
            const auto& sp = scene_places[i];
            if (!sp.alive) continue;
            const auto obj_it = edit_obj_by_id.find(sp.object_id);
            if (obj_it == edit_obj_by_id.end()) continue;
            const auto* obj = obj_it->second;
            auto cache_it = by_raw_path.find(obj->sco_path.native());
            if (cache_it == by_raw_path.end()) {
                const auto* s = cached_sco(obj->sco_path);
                ScoScriptInfo* inf = s ? &script_info_for(obj->sco_path, s) : nullptr;
                cache_it = by_raw_path.emplace(obj->sco_path.native(), std::make_pair(s, inf)).first;
            }
            const auto* sco = cache_it->second.first;
            if (!sco || !cache_it->second.second) continue;
            const auto& info = *cache_it->second.second;
            std::uint8_t flags = 0;
            if (info.has_visible_links) flags |= kScoDynVisible;
            if (info.has_text_textures) flags |= kScoDynTextTex;
            if (info.has_freetex) flags |= kScoDynFreeTex;
            if (info.has_matl_change) flags |= kScoDynMatlChange;
            if (!sco->ctc.empty() && !sco->ctc_textures.empty()) flags |= kScoDynCtc;
            if (info.has_mesh_anims) flags |= kScoDynMeshAnim;
            if (renderer.chunk_has_uv_vars(sp.chunk)) flags |= kScoDynUv;
            if (flags) sco_dynamic_scene_refs.push_back({i, flags, sco, &info});
        }
        sco_dynamic_scene_size = scene_places.size();
        sco_dynamic_obj_signature = edit_obj_index_signature;
    };

    scenery_scripts.sync(edit_objects, cfg.omsi_root);
    ensure_edit_obj_index();
    float sco_anim_dt = 0.016f;
    auto cpu_in_radius = [&](const float x, const float y) -> bool {
        if (editor_cpu_radius_m <= 0.f) return true;
        const float dx = x - cpu_focus_x;
        const float dy = y - cpu_focus_y;
        return dx * dx + dy * dy <= editor_cpu_radius_m * editor_cpu_radius_m;
    };
    std::unordered_set<std::int32_t> anim_ticked;
    anim_ticked.reserve(scene_places.size() / 8 + 1);

    auto apply_sco_script_visibility = [&]() {
        const auto vis_t0 = std::chrono::steady_clock::now();
        const bool loading_mode =
            omsi::LoadGovernor::global().mode() == omsi::LoadGovernor::Mode::Loading;
        // Hard wall-clock: one sync replace/exists must never freeze the main for seconds.
        // Perf: was 4 ms/frame, always exhausted, always restarting at ref #0 (later refs starved).
        // Now 1.5 ms/frame with a persistent round-robin cursor so every ref is served within a few frames.
        const double hard_ms = loading_mode ? 12.0 : 1.5;
        auto vis_elapsed_ms = [&]() {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - vis_t0)
                .count();
        };
        auto vis_expired = [&]() { return vis_elapsed_ms() >= hard_ms; };
        double ms_rebuild = 0, ms_visible = 0, ms_texttex = 0, ms_freetex = 0, ms_matl = 0, ms_anim = 0;
        int n_bake_submit = 0, n_bake_apply = 0, n_path_submit = 0;

        ensure_edit_obj_index();
        {
            const auto t0 = std::chrono::steady_clock::now();
            if (sco_dynamic_scene_size != scene_places.size() ||
                sco_dynamic_obj_signature != edit_obj_index_signature)
                rebuild_sco_dynamic_scene_refs();
            ms_rebuild = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        anim_ticked.clear();

        auto tt_fb = omsi::FrameBudget::for_stream(false, 10.0, 1000.0 / 30.0, hard_ms);
        tt_fb.begin_phase(hard_ms);
        bool tt_stop = false;
        auto tt_admit = [&](const omsi::FrameBudget::WorkCost& c) -> bool {
            if (tt_stop || vis_expired()) {
                tt_stop = true;
                return false;
            }
            const auto a = tt_fb.try_admit(c);
            if (a == omsi::FrameBudget::Admit::Defer) {
                tt_stop = true;
                return false;
            }
            if (a == omsi::FrameBudget::Admit::DeferHeavy) return false;
            return true;
        };

        // 0=pending (worker), 1=missing, 2=found (out filled).
        auto lookup_or_submit_path = [&](const std::filesystem::path& sco_path, const std::string& want,
                                         std::filesystem::path& out) -> int {
            std::string rel = want;
            for (char& c : rel)
                if (c == '/') c = '\\';
            while (!rel.empty() && (rel.front() == '\\' || rel.front() == '/')) rel.erase(rel.begin());
            const std::string key = sco_path.lexically_normal().string() + "|" + rel;
            {
                std::lock_guard<std::mutex> lk(path_resolve_mu);
                if (const auto it = path_resolve_cache.find(key); it != path_resolve_cache.end()) {
                    if (!it->second) return 1;
                    out = *it->second;
                    return 2;
                }
                if (!path_resolve_inflight.insert(key).second) return 0;
            }
            ++n_path_submit;
            const auto root = cfg.global.root;
            const auto omsi = cfg.omsi_root;
            omsi::JobSystem::global().submit(
                [key, sco_path, rel, root, omsi, prs = path_resolve_shared]() {
                    auto& path_resolve_mu = prs->mu;
                    auto& path_resolve_cache = prs->cache;
                    auto& path_resolve_inflight = prs->inflight;
                    std::optional<std::filesystem::path> resolved;
                    auto try_dir = [&](const std::filesystem::path& dir) {
                        if (dir.empty() || resolved) return;
                        std::error_code ec;
                        for (const char* folder : {"texture", "Texture", ""}) {
                            const auto base = folder[0] ? (dir / folder) : dir;
                            const auto p = (base / rel).lexically_normal();
                            if (std::filesystem::exists(p, ec)) {
                                resolved = p;
                                return;
                            }
                            const auto p2 = (base / std::filesystem::path(rel).filename()).lexically_normal();
                            if (std::filesystem::exists(p2, ec)) {
                                resolved = p2;
                                return;
                            }
                        }
                    };
                    try_dir(sco_path.parent_path());
                    try_dir(root);
                    try_dir(omsi);
                    if (!resolved) resolved = gfx::dx12::resolve_texture_path(sco_path, rel, omsi);
                    std::lock_guard<std::mutex> lk(path_resolve_mu);
                    path_resolve_cache[key] = resolved;
                    path_resolve_inflight.erase(key);
                },
                omsi::JobSystem::Priority::Normal);
            return 0;
        };

        // Drain worker bakes → GPU upload (main thread only).
        {
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<TextTexBakeReady> ready;
            {
                std::lock_guard<std::mutex> lk(texttex_async_mu);
                ready.swap(texttex_bake_ready);
            }
            for (auto& item : ready) {
                if (vis_expired() || tt_stop) {
                    std::lock_guard<std::mutex> lk(texttex_async_mu);
                    texttex_bake_ready.push_back(std::move(item));
                    continue;
                }
                // (one entry out when full, not the whole cache: cleared every few bakes on a map with
                //  hundreds of signs, a text was gone again before its place came round, baked again,
                //  gone again - signs stayed blank and the bake jobs never stopped)
                if (texttex_raster_cache.size() >= kTextTexRasterCacheMax &&
                    texttex_raster_cache.find(item.rkey) == texttex_raster_cache.end())
                    texttex_raster_cache.erase(texttex_raster_cache.begin());
                auto& cached = texttex_raster_cache[item.rkey];
                if (cached.rgba.empty()) cached = std::move(item.img);
                ++n_bake_apply;
                if (texttex_chunk_place.empty()) {
                    for (const auto& dref : sco_dynamic_scene_refs)
                        if (dref.scene_index < scene_places.size())
                            texttex_chunk_place[scene_places[dref.scene_index].chunk] = dref.scene_index;
                }
                static const bool trace_ready = std::getenv("OMSI_TRACE_TEXTTEX") != nullptr;
                const auto pit = texttex_chunk_place.find(item.chunk);
                if (pit == texttex_chunk_place.end() || pit->second >= scene_places.size()) {
                    if (trace_ready) log::info("texttex ready: no place for chunk " + std::to_string(item.chunk) + " obj " + std::to_string(item.obj_id));
                    continue;
                }
                auto& tsp = scene_places[pit->second];
                if (!tsp.alive || tsp.chunk != item.chunk || tsp.object_id != item.obj_id) {
                    if (trace_ready) log::info("texttex ready: place mismatch obj " + std::to_string(item.obj_id) + " vs " + std::to_string(tsp.object_id));
                    continue;
                }
                std::string* applied = nullptr;
                for (auto& ta : tsp.text_applied)
                    if (ta.first == item.ti) applied = &ta.second;
                if (applied && *applied == item.txt) continue;
                omsi::FrameBudget::WorkCost tc;
                tc.bytes = static_cast<std::uint64_t>(cached.width) * static_cast<std::uint64_t>(cached.height) * 4ull;
                tc.est_ms = 0.2 + static_cast<double>(tc.bytes) / (8.0 * 1024.0 * 1024.0);
                if (!tt_admit(tc)) continue; // cached: the per-place pass puts it on later
                const auto item_t0 = std::chrono::steady_clock::now();
                if (renderer.replace_chunk_albedo_rgba(item.chunk, cached.rgba.data(), cached.width, cached.height,
                                                       item.ti)) {
                    tt_fb.charge(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - item_t0)
                                     .count(),
                                 tc);
                    if (applied) *applied = item.txt;
                    else tsp.text_applied.emplace_back(item.ti, item.txt);
                    if (trace_ready) log::info("texttex ready: applied obj " + std::to_string(item.obj_id) + " \"" + item.txt + "\"");
                } else if (trace_ready) {
                    log::info("texttex ready: replace failed obj " + std::to_string(item.obj_id) + " ti " + std::to_string(item.ti));
                }
            }
            texttex_chunk_place.clear();
            ms_texttex += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }

        const std::size_t n_dyn_refs = sco_dynamic_scene_refs.size();
        static std::size_t s_vis_cursor = 0;
        if (s_vis_cursor >= n_dyn_refs) s_vis_cursor = 0;
        std::size_t vis_done = 0;
        for (; vis_done < n_dyn_refs; ++vis_done) {
            const auto& dyn = sco_dynamic_scene_refs[(s_vis_cursor + vis_done) % n_dyn_refs];
            if (vis_done >= 8 && vis_expired()) { // always progress >= 8 refs/frame (no starvation)
                tt_stop = true;
                break;
            }
            if (dyn.scene_index >= scene_places.size()) continue;
            auto& sp = scene_places[dyn.scene_index];
            if (!sp.alive) continue;
            // Outside CPU radius: leave GPU mesh as-is (visible/pickable via distance cull + streaming).
            if (!cpu_in_radius(sp.tx, sp.ty)) continue;
            const auto obj_it = edit_obj_by_id.find(sp.object_id);
            if (obj_it == edit_obj_by_id.end()) continue;
            const map::MapObjectInst* obj = obj_it->second;
            const auto* sco = dyn.sco;
            if (!sco || !dyn.info) continue;
            const auto& info = *dyn.info;
            const std::uint8_t flags = dyn.flags;

            if (flags & kScoDynVisible) {
                const auto t0 = std::chrono::steady_clock::now();
                const bool visible = scenery_scripts.mesh_visible(*obj, sp.sco_mesh_index, *sco);
                if (!sp.script_visible_known || sp.script_visible != visible) {
                    static const bool trace_vis = std::getenv("OMSI_TRACE_SCRIPT_VIS") != nullptr;
                    if (trace_vis && sp.script_visible_known)
                        log::info("script visible: obj " + std::to_string(obj->id) + " " +
                                  omsi::path_text(obj->sco_path.filename()) + " mesh " + std::to_string(sp.sco_mesh_index) +
                                  (visible ? " ON" : " off"));
                    renderer.set_chunk_visible(sp.chunk, visible);
                    sp.script_visible = visible;
                    sp.script_visible_known = true;
                }
                ms_visible += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            if (flags & kScoDynUv) {
                renderer.apply_chunk_uv_vars(sp.chunk, [&](const std::string& name) {
                    return scenery_scripts.float_var(obj->id, name);
                });
            }
            if ((flags & kScoDynTextTex) && !tt_stop) {
                auto resolve_tt = [&](const sco::ScoTextTexture& tt) -> std::string {
                    std::string name = tt.string_var;
                    if (name.empty() && tt.source_index >= 0 &&
                        static_cast<std::size_t>(tt.source_index) < sco->string_var_names.size())
                        name = sco->string_var_names[static_cast<std::size_t>(tt.source_index)];
                    if (!name.empty()) {
                        auto s = scenery_scripts.string_var(obj->id, name);
                        if (!s.empty()) return s;
                    }
                    if (tt.source_index >= 0 && static_cast<std::size_t>(tt.source_index) < obj->strings.size())
                        return obj->strings[static_cast<std::size_t>(tt.source_index)];
                    return {};
                };
                for (const auto mat_index : info.text_material_indices) {
                    if (tt_stop) break;
                    if (mat_index >= sco->materials.size()) continue;
                    const auto& mat = sco->materials[mat_index];
                    if (sp.is_x_chunk) {
                        if (sp.text_texture_index < 0) continue;
                        if (*mat.use_text_texture != sp.text_texture_index) continue;
                    }
                    if (mat.mesh_index >= 0 && sp.sco_mesh_index >= 0 &&
                        mat.mesh_index != sp.sco_mesh_index)
                        continue;
                    const int ti = *mat.use_text_texture;
                    if (ti < 0 || static_cast<std::size_t>(ti) >= sco->text_textures.size()) continue;
                    const auto& tt = sco->text_textures[static_cast<std::size_t>(ti)];
                    const std::string txt = resolve_tt(tt);
                    std::string* applied = nullptr;
                    for (auto& ta : sp.text_applied)
                        if (ta.first == ti) applied = &ta.second;
                    if (applied && *applied == txt) continue;
                    auto mark_applied = [&]() {
                        if (applied) *applied = txt;
                        else sp.text_applied.emplace_back(ti, txt);
                    };
                    const std::string rkey = tt.font_name + "|" + std::to_string(tt.width) + "x" +
                                            std::to_string(tt.height) + "|" + std::to_string(tt.align) + "|" +
                                            std::to_string(tt.color_mode) + "|" + txt;
                    const sco::TextTextureImage* img = nullptr;
                    if (const auto cit = texttex_raster_cache.find(rkey); cit != texttex_raster_cache.end()) {
                        img = &cit->second;
                    } else {
                        // Bake on worker — never rasterize_text_texture / .oft load on main.
                        bool submit = false;
                        {
                            std::lock_guard<std::mutex> lk(texttex_async_mu);
                            if (texttex_bake_inflight.insert(rkey).second) submit = true;
                        }
                        if (submit) {
                            ++n_bake_submit;
                            static const bool trace_sub = std::getenv("OMSI_TRACE_TEXTTEX") != nullptr;
                            if (trace_sub)
                                log::info("texttex submit obj " + std::to_string(obj->id) + " font=" + tt.font_name +
                                          " \"" + txt + "\"");
                            const auto chunk = sp.chunk;
                            const auto obj_id = obj->id;
                            const auto omsi_root = cfg.omsi_root;
                            omsi::JobSystem::global().submit(
                                [tt, txt, rkey, chunk, obj_id, ti, omsi_root, tts = texttex_shared]() {
                                    auto& texttex_async_mu = tts->mu;
                                    auto& texttex_bake_ready = tts->ready;
                                    auto& texttex_bake_inflight = tts->inflight;
                                    TextTexBakeReady ready{};
                                    ready.chunk = chunk;
                                    ready.obj_id = obj_id;
                                    ready.ti = ti;
                                    ready.txt = txt;
                                    ready.rkey = rkey;
                                    // Never let a throwing bake leave rkey in `inflight` forever.
                                    try {
                                        if (auto baked = sco::rasterize_text_texture(tt, txt, omsi_root))
                                            ready.img = std::move(*baked);
                                    } catch (const std::exception& ex) {
                                        log::warn(std::string("texttex bake failed: ") + ex.what());
                                    } catch (...) {
                                        log::warn("texttex bake failed: unknown exception");
                                    }
                                    std::lock_guard<std::mutex> lk(texttex_async_mu);
                                    texttex_bake_inflight.erase(rkey);
                                    if (!ready.img.rgba.empty()) texttex_bake_ready.push_back(std::move(ready));
                                },
                                omsi::JobSystem::Priority::High);
                        }
                        continue;
                    }
                    if (!img) {
                        mark_applied();
                        continue;
                    }
                    omsi::FrameBudget::WorkCost tc;
                    tc.bytes = static_cast<std::uint64_t>(img->width) * static_cast<std::uint64_t>(img->height) * 4ull;
                    tc.est_ms = 0.2 + static_cast<double>(tc.bytes) / (8.0 * 1024.0 * 1024.0);
                    if (!tt_admit(tc)) continue;
                    const auto item_t0 = std::chrono::steady_clock::now();
                    if (renderer.replace_chunk_albedo_rgba(sp.chunk, img->rgba.data(), img->width, img->height,
                                                           ti)) {
                        const double dt = std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - item_t0)
                                              .count();
                        tt_fb.charge(dt, tc);
                        static int tt_logged = 0;
                        if (tt_logged < 8) {
                            log::info("texttexture id=" + std::to_string(obj->id) + " sco=" +
                                      obj->sco_path.filename().string() + " font=" + tt.font_name +
                                      " text=\"" + txt + "\" " + std::to_string(img->width) + "x" +
                                      std::to_string(img->height));
                            ++tt_logged;
                        }
                    } else {
                        static int tt_fail = 0;
                        if (tt_fail < 8) {
                            log::warn("texttexture replace failed id=" + std::to_string(obj->id) +
                                      " chunk=" + std::to_string(sp.chunk) + " tt=" + std::to_string(ti) +
                                      " (no useTextTexture batch?)");
                            ++tt_fail;
                        }
                    }
                    mark_applied();
                }
            }
            if ((flags & kScoDynFreeTex) && !tt_stop) {
                const auto t0 = std::chrono::steady_clock::now();
                for (const auto mat_index : info.freetex_material_indices) {
                if (tt_stop || vis_expired()) break;
                if (mat_index >= sco->materials.size()) continue;
                const auto& mat = sco->materials[mat_index];
                if (!mat.freetex_label) continue;
                if (mat.mesh_index >= 0 && sp.sco_mesh_index >= 0 && mat.mesh_index != sp.sco_mesh_index)
                    continue;
                std::string tex;
                const auto& label = *mat.freetex_label;
                if (const auto s = scenery_scripts.string_var(obj->id, label); !s.empty()) tex = s;
                if (tex.empty()) {
                    for (std::size_t si = 0; si < sco->string_var_names.size() && si < obj->strings.size(); ++si) {
                        if (sco->string_var_names[si] == label) {
                            tex = obj->strings[si];
                            break;
                        }
                    }
                }
                if (tex.empty() && !obj->strings.empty()) tex = obj->strings.front();
                const std::string cache_key = mat.texture_key + "|" + tex;
                const auto last_it = freetex_last.find(sp.chunk);
                if (last_it != freetex_last.end() && last_it->second == cache_key) continue;
                if (tex.empty()) continue;
                std::filesystem::path resolved_path;
                const int st = lookup_or_submit_path(obj->sco_path, tex, resolved_path);
                if (st == 0) continue;
                if (st == 1) {
                    static int ft_miss = 0;
                    if (ft_miss < 12) {
                        log::warn("matl_freetex missing id=" + std::to_string(obj->id) + " file=\"" + tex +
                                  "\" sco=" + obj->sco_path.filename().string());
                        ++ft_miss;
                    }
                    continue;
                }
                omsi::FrameBudget::WorkCost fc{256ull * 1024ull, 0, 0, 1, 0, 0.35};
                if (!tt_admit(fc)) continue;
                const auto ft0 = std::chrono::steady_clock::now();
                const bool ft_shown = renderer.replace_chunk_albedo_file(sp.chunk, resolved_path, mat.texture_key);
                tt_fb.charge(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ft0).count(),
                             fc);
                if (ft_shown) freetex_last[sp.chunk] = cache_key; // (decoded on a worker: asked again until shown)
                }
                ms_freetex += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            if ((flags & kScoDynMatlChange) && !tt_stop) {
                const auto t0 = std::chrono::steady_clock::now();
                for (const auto mat_index : info.matl_change_material_indices) {
                    if (vis_expired()) {
                        tt_stop = true;
                        break;
                    }
                    if (mat_index >= sco->materials.size()) continue;
                    const auto& mat = sco->materials[mat_index];
                    if (!mat.is_matl_change || mat.change_var.empty()) continue;
                    if (mat.mesh_index >= 0 && sp.sco_mesh_index >= 0 && mat.mesh_index != sp.sco_mesh_index)
                        continue;
                    std::string tkey = mat.diffuse_texture;
                    for (char& c : tkey) {
                        if (c == '\\') c = '/';
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    const std::string slot_key = "mc:" + tkey + "#" + std::to_string(mat.mat_id);
                    const bool on = scenery_scripts.float_var(obj->id, mat.change_var) > 0.5f;
                    std::string want_rel = on ? mat.diffuse_texture : renderer.chunk_freetex_original(sp.chunk, slot_key);
                    if (want_rel.empty()) continue;
                    const std::string cache_key = slot_key + "|" + want_rel;
                    const std::string last_key = std::to_string(sp.chunk) + "|" + slot_key;
                    const auto last_it = matl_change_last.find(last_key);
                    if (last_it != matl_change_last.end() && last_it->second == cache_key) continue;
                    std::filesystem::path resolved_path;
                    if (lookup_or_submit_path(obj->sco_path, want_rel, resolved_path) != 2) continue;
                    omsi::FrameBudget::WorkCost mc{256ull * 1024ull, 0, 0, 1, 0, 0.35};
                    if (!tt_admit(mc)) {
                        tt_stop = true;
                        break;
                    }
                    const auto t1 = std::chrono::steady_clock::now();
                    if (renderer.replace_chunk_albedo_file(sp.chunk, resolved_path, slot_key)) {
                        tt_fb.charge(
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count(),
                            mc);
                        matl_change_last[last_key] = cache_key;
                    }
                }
                ms_matl += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            if ((flags & kScoDynCtc) && !tt_stop) {
                // openOMSI DynamicTextureGroup: the variable's value is the scheme's index (-1 / out of
                // range: the model's own textures)
                auto ieq_ci = [](const std::string& a, const std::string& b) {
                    if (a.size() != b.size()) return false;
                    for (std::size_t q = 0; q < a.size(); ++q)
                        if (std::tolower(static_cast<unsigned char>(a[q])) != std::tolower(static_cast<unsigned char>(b[q])))
                            return false;
                    return true;
                };
                std::string want;
                std::vector<std::pair<std::string, std::filesystem::path>> todo; // slot key, file
                for (const auto& c : sco->ctc) {
                    const float v = scenery_scripts.float_var(obj->id, c.var);
                    const int idx = static_cast<int>(std::floor(v + 0.001f));
                    if (idx < 0 || idx >= static_cast<int>(c.schemes.size())) continue;
                    const auto& sc = c.schemes[static_cast<std::size_t>(idx)];
                    for (const auto& [name, file] : sc.textures) {
                        for (const auto& [cname, def] : sco->ctc_textures) {
                            if (!ieq_ci(cname, name)) continue;
                            std::string k = def;
                            for (char& ch : k) {
                                if (ch == '\\') ch = '/';
                                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                            }
                            todo.emplace_back("ctc:" + k, sc.dir / std::filesystem::path(file));
                            want += k + "=" + file + ";";
                        }
                    }
                }
                // the scheme's textures are read and decoded on a worker first (a cold file read on this
                // thread was a 50-250 ms hitch per bus stop); applied once they are all warm
                static std::mutex s_ctc_mu;
                static std::unordered_set<std::wstring> s_ctc_ready, s_ctc_inflight;
                auto ctc_warm = [&](const std::filesystem::path& f) -> bool {
                    const std::wstring k = f.lexically_normal().native();
                    {
                        std::lock_guard<std::mutex> lk(s_ctc_mu);
                        if (s_ctc_ready.count(k)) return true;
                        if (!s_ctc_inflight.insert(k).second) return false;
                    }
                    omsi::JobSystem::global().submit(
                        [f, k]() {
                            try {
                                std::error_code fec;
                                if (std::filesystem::exists(f, fec)) gfx::dx12::prefetch_textures_cpu({f});
                            } catch (...) {
                            }
                            std::lock_guard<std::mutex> lk(s_ctc_mu);
                            s_ctc_inflight.erase(k);
                            s_ctc_ready.insert(k);
                        },
                        omsi::JobSystem::Priority::Low);
                    return false;
                };
                bool ctc_all_warm = true;
                if (!want.empty() && want != sp.ctc_applied)
                    for (const auto& [key, file] : todo) ctc_all_warm = ctc_warm(file) && ctc_all_warm;
                if (!want.empty() && want != sp.ctc_applied && ctc_all_warm) {
                    omsi::FrameBudget::WorkCost cc{512ull * 1024ull, 0, 0, 1, 0, 0.5};
                    if (tt_admit(cc)) {
                        bool all_shown = true;
                        for (const auto& [key, file] : todo) {
                            std::error_code fec;
                            if (std::filesystem::exists(file, fec))
                                all_shown = renderer.replace_chunk_albedo_file(sp.chunk, file, key) && all_shown;
                        }
                        if (all_shown) sp.ctc_applied = want; // (decoded on a worker: asked again until shown)
                    }
                }
            }
            if (!(flags & kScoDynMeshAnim) || sp.sco_mesh_index < 0) continue;
            const auto anim_t0 = std::chrono::steady_clock::now();
            auto it = sco_anims.find(obj->id);
            if (it == sco_anims.end()) {
                ScoAnimRuntime rt{};
                rt.cfg.cfg_path = sco->sco_path;
                model_cfg::ModelCfgLod lod{};
                for (const auto& m : sco->meshes) {
                    model_cfg::ModelCfgMeshEntry e{};
                    e.mesh_file = m.mesh_file.string();
                    e.animations = m.animations;
                    lod.meshes.push_back(std::move(e));
                }
                rt.cfg.lods.push_back(std::move(lod));
                rt.animator.init(rt.cfg);
                it = sco_anims.emplace(obj->id, std::move(rt)).first;
            }
            if (anim_ticked.insert(obj->id).second) {
                // Real elapsed time since this object's last update (round-robin may skip frames).
                const double now_s = std::chrono::duration<double>(
                                         std::chrono::steady_clock::now().time_since_epoch()).count();
                float anim_dt = sco_anim_dt;
                if (it->second.last_update_s >= 0.0)
                    anim_dt = static_cast<float>((std::min)((std::max)(now_s - it->second.last_update_s, 0.0), 0.25));
                it->second.last_update_s = now_s;
                it->second.animator.update(it->second.cfg, anim_dt, [&](const std::string& name) {
                    return scenery_scripts.float_var(obj->id, name);
                });
            }
            if (!sp.has_base_world) continue;
            const auto& dx_t = it->second.animator.matrices_dx_transposed();
            const auto mi = static_cast<std::size_t>(sp.sco_mesh_index);
            if (mi >= dx_t.size()) continue;
            // Same path as the bus viewer: DX row-vector T(-P)*R*T(P), then object base.
            // ma4_to_xm(matrix_for_mesh) put translation in the wrong slot → sheared faces.
            DirectX::XMFLOAT4X4 stored{};
            std::memcpy(&stored, dx_t[mi].data(), sizeof(stored));
            const XMMATRIX mesh = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&stored));
            const XMMATRIX base = DirectX::XMLoadFloat4x4(&sp.base_world);
            renderer.set_chunk_world_matrix(sp.chunk, store_world_transposed(mesh * base).data());
            ms_anim += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - anim_t0).count();
        }
        if (n_dyn_refs > 0) s_vis_cursor = (s_vis_cursor + vis_done) % n_dyn_refs;
        const double total_ms = vis_elapsed_ms();
        if (total_ms > 2.0 || n_bake_submit > 0 || n_bake_apply > 0 || n_path_submit > 0) {
            log::info("sco_visibility ms=" + std::to_string(total_ms) + " hard=" + std::to_string(hard_ms) +
                      " rebuild=" + std::to_string(ms_rebuild) + " visible=" + std::to_string(ms_visible) +
                      " texttex=" + std::to_string(ms_texttex) + " freetex=" + std::to_string(ms_freetex) +
                      " matl=" + std::to_string(ms_matl) + " anim=" + std::to_string(ms_anim) +
                      " bake_sub=" + std::to_string(n_bake_submit) + " bake_apply=" + std::to_string(n_bake_apply) +
                      " path_sub=" + std::to_string(n_path_submit) + (tt_stop ? " STOP" : ""));
        }
    };
    {
        auto& bst = panel.imgui_state();
        bst.busy_overlay = false;
        bst.busy_spinner = false;
        bst.busy_progress = -1.f;
        bst.busy_title.clear();
        bst.busy_detail.clear();
    }
    boot_trace.mark("sco_scripts_texttex");

    // Splines: tessellate .sli profiles (Blender-O3D-IO / tt-viewer fields) into textured strips.
    struct SplineGpu {
        std::int32_t id = 0;
        std::int32_t tile_gx = 0;
        std::int32_t tile_gy = 0;
        std::vector<std::size_t> chunks;
        bool alive = true;
        bool pinned = false; // marquee clone until placed
        // Last .sli filename baked into chunks — if it changes, force full rebuild
        // (replace_sli / undo) so old mesh strips cannot linger until tile reload.
        std::wstring sli_filename{};
    };
    std::vector<SplineGpu> spline_gpu;
    std::unordered_map<std::int32_t, f4f23ma::f32> spline_start_z;
    for (const auto& tile : world.tiles()) {
        for (const auto& sp_local : tile.splines) spline_start_z[sp_local.id] = sp_local.position.z;
    }
    // Tessellate+GPU on worker+sliced upload (see async remesh). Boot used to freeze the UI
    // for seconds (sliProfileWinding + SPAWN spline × thousands on the main thread).
    log::info(std::string("editor splines deferred n=") + std::to_string(edit_splines.size()) +
              (view_only ? " [VIEW-ONLY]" : " [EDIT]"));

    for (auto& e : edit_spline_attachments) fill_sa_parent_spline_id(e);
    request_sa_full_respawn();

    int selected_path_rail = -1;
    std::vector<EndMarker> end_markers;
    std::unordered_map<std::int32_t, std::vector<std::size_t>> markers_by_owner;
    std::vector<PathRail> path_rails;
    std::vector<map::MapObjectInst> transient_objects; // survive tile stream
    std::vector<map::SplineInst> transient_splines;
    std::vector<o3d::Vertex> arrow_verts;
    std::vector<std::uint32_t> arrow_inds;
    std::vector<o3d::Vertex> path_mark_verts;
    std::vector<std::uint32_t> path_mark_inds;
    make_arrow_mesh(arrow_verts, arrow_inds);
    make_uv_sphere(path_mark_verts, path_mark_inds, 1.f, 12);
    // Vista > Flechas 3D: one translucent solid arrow per spline end (replaces the flat spline arrow and
    // the path spheres). Chunks made on first use, keyed by (spline id, end).
    std::vector<o3d::Vertex> arrow3d_verts;
    std::vector<std::uint32_t> arrow3d_inds;
    make_arrow_mesh_3d(arrow3d_verts, arrow3d_inds);
    std::unordered_map<std::int64_t, std::size_t> arrow3d_chunks;
    auto arrow3d_key = [](std::int32_t owner, bool is_start) {
        return (static_cast<std::int64_t>(owner) << 1) | (is_start ? 1 : 0);
    };
    auto arrow3d_chunk = [&](std::int32_t owner, bool is_start) -> std::size_t {
        const auto key = arrow3d_key(owner, is_start);
        if (const auto it = arrow3d_chunks.find(key); it != arrow3d_chunks.end()) return it->second;
        if (!renderer.append_textured_mesh(arrow3d_verts.data(), static_cast<std::uint32_t>(arrow3d_verts.size()),
                                           arrow3d_inds.data(), static_cast<std::uint32_t>(arrow3d_inds.size()), {},
                                           0u))
            return static_cast<std::size_t>(-1);
        const std::size_t chunk = renderer.chunk_count() - 1;
        renderer.set_chunk_opacity(chunk, 0.55f);
        renderer.set_chunk_visible(chunk, false);
        arrow3d_chunks.emplace(key, chunk);
        return chunk;
    };
    constexpr float kMarkFloatZ = 0.38f;
    constexpr float kArrowFloatZ = 1.f;
    constexpr float kArrowScale = 0.9f; // mesh height 1 m * scale
    // Visual tip offset: keep connection math on centerline; draw diamond outside the road body.
    constexpr float kEndMarkOutM = 0.85f;
    auto push_end_mark_out = [](float sx, float sy, float sz, float heading_rad, bool is_start, float& ox,
                                float& oy, float& oz) {
        const float hx = std::sin(heading_rad);
        const float hy = std::cos(heading_rad);
        const float s = is_start ? -kEndMarkOutM : kEndMarkOutM;
        ox = sx + s * hx;
        oy = sy + s * hy;
        oz = sz + kMarkFloatZ;
    };
    auto arrow_world_matrix = [](float x, float y, float z, float heading_rad, float scale) {
        const float hx = std::sin(heading_rad);
        const float hy = std::cos(heading_rad);
        // Row 1 (index 1) = forward for row-vector v*(0,1,0); stored transposed for HLSL mul(M,v).
        const DirectX::XMMATRIX basis =
            DirectX::XMMatrixSet(-hy, hx, 0.f, 0.f, hx, hy, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f);
        return DirectX::XMMatrixMultiply(DirectX::XMMatrixScaling(scale, scale, scale),
                                         DirectX::XMMatrixMultiply(basis, DirectX::XMMatrixTranslation(x, y, z)));
    };
    auto place_marker = [&](float x, float y, float z, float scale, bool is_path, std::int32_t owner,
                            bool is_start, bool from_object = false, int path_index = 0, std::int32_t gx = 0,
                            std::int32_t gy = 0, float heading_rad = 0.f) {
        const bool use_arrow = !is_path && !from_object;
        const auto& mv = use_arrow ? arrow_verts : path_mark_verts;
        const auto& mi = use_arrow ? arrow_inds : path_mark_inds;
        if (!renderer.append_textured_mesh(mv.data(), static_cast<std::uint32_t>(mv.size()), mi.data(),
                                           static_cast<std::uint32_t>(mi.size()), {}, 0u))
            return;
        const std::size_t chunk = renderer.chunk_count() - 1;
        const XMMATRIX world =
            use_arrow ? arrow_world_matrix(x, y, z, heading_rad, scale)
                      : DirectX::XMMatrixMultiply(DirectX::XMMatrixScaling(scale, scale, scale),
                                                  DirectX::XMMatrixTranslation(x, y, z));
        const auto wt = store_world_transposed(world);
        renderer.set_chunk_world_matrix(chunk, wt.data());
        if (use_arrow) {
            renderer.set_chunk_invert_color(chunk, true);
            renderer.set_chunk_highlight(chunk, 0.f, 1.f, 1.f, 1.f);
        } else
            renderer.set_chunk_highlight(chunk, 0.55f, 0.75f, 0.95f, 0.85f);
        renderer.set_chunk_visible(chunk, false);
        EndMarker m{};
        m.chunk = chunk;
        m.is_path = is_path;
        m.owner_id = owner;
        m.is_start = is_start;
        m.from_object = from_object;
        m.path_index = path_index;
        m.scale = scale;
        m.tile_gx = gx;
        m.tile_gy = gy;
        end_markers.push_back(m);
        markers_by_owner[owner].push_back(end_markers.size() - 1);
    };
    if (!view_only) {
    for (const auto& s : edit_splines) {
        if (!spline::spline_in_radius_xy(s, spawn_preview_cam.target_x, spawn_preview_cam.target_y,
                                         editor_visual_radius_m))
            continue;
        std::int32_t sgx = tile_gx, sgy = tile_gy;
        for (const auto& g : spline_gpu) {
            if (g.id == s.id) {
                sgx = g.tile_gx;
                sgy = g.tile_gy;
                break;
            }
        }
        const auto a = spline::evaluate_spline(s, 0.f);
        const auto b = spline::evaluate_spline(s, s.length);
        place_marker(a.x, a.y, a.z + kArrowFloatZ, kArrowScale, false, s.id, true, false, 0, sgx, sgy, a.heading_rad);
        place_marker(b.x, b.y, b.z + kArrowFloatZ, kArrowScale, false, s.id, false, false, 0, sgx, sgy, b.heading_rad);
        if (const auto def = spline::load_sli_shared(s.sli_path)) {
            for (int pi = 0; pi < static_cast<int>(def->paths.size()); ++pi) {
                const auto& pp = def->paths[static_cast<std::size_t>(pi)];
                const auto p0 = spline::evaluate_spline(s, 0.f, pp.lateral, pp.height);
                const auto p1 = spline::evaluate_spline(s, s.length, pp.lateral, pp.height);
                place_marker(p0.x, p0.y, p0.z + kMarkFloatZ, 0.16f, true, s.id, true, false, pi, sgx, sgy);
                place_marker(p1.x, p1.y, p1.z + kMarkFloatZ, 0.16f, true, s.id, false, false, pi, sgx, sgy);
            }
        }
    }
    for (const auto& o : edit_objects) {
        if (!visual_spawn_in_radius(o.position.x, o.position.y)) continue;
        const auto sco = sco::load_sco(o.sco_path);
        if (!sco || sco->paths.empty()) continue;
        std::int32_t ogx = tile_gx, ogy = tile_gy;
        for (const auto& sp : scene_places) {
            if (sp.alive && sp.object_id == o.id) {
                ogx = sp.tile_gx;
                ogy = sp.tile_gy;
                break;
            }
        }
        const bool abs_z = sco::skip_terrain_add(*sco);
        const bool ground_layer = sco_ground_layer(o, *sco);
        const float th = terrain_add(abs_z, o.position.x, o.position.y, o.position.z, ground_layer);
        const XMMATRIX world_m = sco_world_matrix(o, nullptr, th, 0.f, 0.f);
        for (int pi = 0; pi < static_cast<int>(sco->paths.size()); ++pi) {
            const auto& path = sco->paths[static_cast<std::size_t>(pi)];
            map::SplineInst mini = sco::sco_path_to_spline_inst(path);
            mini.length = std::abs(path.length);
            if (mini.length < 0.05f) mini.length = 0.05f;
            const auto local0 = spline::evaluate_spline(mini, 0.f);
            const auto local1 = spline::evaluate_spline(mini, mini.length);
            DirectX::XMFLOAT3 w0{}, w1{};
            DirectX::XMStoreFloat3(&w0, xform_point(world_m, local0.x, local0.y, local0.z));
            DirectX::XMStoreFloat3(&w1, xform_point(world_m, local1.x, local1.y, local1.z));
            log_sco_path_placement(o, *sco, path, pi, true, th, w0.x, w0.y, w0.z, "markers");
            log_sco_path_placement(o, *sco, path, pi, false, th, w1.x, w1.y, w1.z, "markers");
            place_marker(w0.x, w0.y, w0.z, 0.35f, true, o.id, true, true, pi, ogx, ogy);
            place_marker(w1.x, w1.y, w1.z, 0.35f, true, o.id, false, true, pi, ogx, ogy);
        }
    }
    }
    log::info("editor end markers=" + std::to_string(end_markers.size()) +
              (view_only ? " [VIEW-ONLY skipped]" : ""));

    map::EditorGeoSettings& geo_settings_ref = geo_settings; // already loaded before terrain
    (void)geo_settings_ref;
    // editor_ui_settings already loaded above

    // global.cfg entrypoints: fixed world positions (editor-local = world - tile origin).
    struct EntryChunk {
        std::size_t chunk = 0;
        int ep_index = -1;
        float world_x = 0.f, world_y = 0.f, world_z = 0.f;
        bool visible = true;
    };
    int selected_entrypoint = -1;
    std::vector<EntryChunk> entry_chunks;
    auto refresh_entry_matrix = [&](EntryChunk& e) {
        if (e.ep_index < 0 || static_cast<std::size_t>(e.ep_index) >= cfg.global.entrypoints.size()) return;
        const auto& ep = cfg.global.entrypoints[static_cast<std::size_t>(e.ep_index)];
        e.world_x = ep.position.x;
        e.world_y = ep.position.y;
        e.world_z = ep.position.z;
        const auto wt = store_world_transposed(entrypoint_world_matrix(ep, tile_ox, tile_oy));
        renderer.set_chunk_world_matrix(e.chunk, wt.data());
    };
    auto refresh_entry_visibility = [&]() {
        for (auto& e : entry_chunks) {
            const auto tile = world::find_tile_at_world(cfg.global, e.world_x, e.world_y);
            const bool show = tile && world.has_tile(tile->grid_x, tile->grid_y);
            e.visible = show;
            renderer.set_chunk_editor_helper(e.chunk, true); // (Vista > Show carpark hides them too)
            renderer.set_chunk_visible(e.chunk, show);
        }
    };
    auto shift_entry_chunks = [&](float dx, float dy) {
        if (std::fabs(dx) < 0.01f && std::fabs(dy) < 0.01f) return;
        for (auto& e : entry_chunks) renderer.translate_chunk(e.chunk, dx, dy, 0.f);
    };
    {
        const auto entry_sco = cfg.omsi_root / "Sceneryobjects" / "Generic" / "entrypoint_bus.sco";
        const auto entry_x = cfg.omsi_root / "Sceneryobjects" / "Generic" / "model" / "entrypoint_bus.x";
        auto xm = xfile::load_x_mesh(entry_x);
        std::filesystem::path entry_tex{};
        if (xm && !xm->texture_filename.empty()) {
            if (auto p = gfx::dx12::resolve_texture_path(entry_x, xm->texture_filename, cfg.omsi_root))
                entry_tex = *p;
            else if (auto p2 = gfx::dx12::resolve_texture_path(entry_sco, xm->texture_filename, cfg.omsi_root))
                entry_tex = *p2;
        }
        if (!xm) log::warn("entrypoint_bus.x missing: " + entry_x.string());
        // Do NOT draw entrypoint .x from global.cfg — the tile [object] .sco owns the mesh.
        // Drawing here caused duplicate models that blocked picking the real map object.
        entry_chunks.clear();
        if (!cfg.global.entrypoints.empty()) {
            log::info("global.cfg entrypoints loaded n=" +
                      std::to_string(cfg.global.entrypoints.size()) +
                      " (no helper mesh; map objects render .x)");
        }
    }

    {
        const double map_load_sec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - map_load_t0).count();
        char buf[160]{};
        std::snprintf(buf, sizeof(buf),
                      "Mapa cargado completo en %.2f s (tiles=%zu objetos=%d splines=%d)", map_load_sec,
                      world.tiles().size(), object_count, spline_count);
        log::info(buf);
        panel.set_status(buf);
        boot_trace.mark("setup_01_gpu_lists");
    }
    log::info("editor side panel open");
