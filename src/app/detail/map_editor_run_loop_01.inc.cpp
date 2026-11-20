    // (the watchdog now starts at the top of the load: setup_01)
    main_watchdog.mark("loop");
    while (!glfwWindowShouldClose(window)) {

        if (pending_visual_texture_reload && renderer.chunk_count() > 64u) {
            auto _watch = main_watchdog.phase("visual_texture_reload");
            pending_visual_texture_reload = false;
            gfx::dx12::set_texture_max_edge(0);
            (void)renderer.reload_file_textures();
            panel.set_status("Texturas: resolución nativa + mipmaps (calidad visual)");
            log::info("visual_quality: reload_file_textures chunks=" + std::to_string(renderer.chunk_count()));
        }
        ++g_editor_frame_id;
        main_watchdog.frame_started(g_editor_frame_id);
        auto _watch_frame = main_watchdog.phase("frame.start_and_init");
        frame_profiler.begin_frame();
        const auto now = std::chrono::steady_clock::now();
        {
            static auto s_wall_prev = now;
            g_editor_frame_wall_ms =
                std::chrono::duration<double, std::milli>(now - s_wall_prev).count();
            s_wall_prev = now;
        }
        {
            auto _watch = main_watchdog.phase("undo_remesh.pump");
            auto _perf = frame_profiler.scope(MapEditorFrameProfiler::UndoJobs);
            pump_undo_remesh_queue(6); // amortize leftover undo GPU work (~12ms/frame)
            pump_catalog_reload_queue(stream_busy() ? 3 : 10);
            (void)poll_spline_environment_job();
            pump_catalog_scan_job(); // async "Escanear carpeta" apply (never blocks hud_build)
        }
        // Pegar cadena: al cambiar la selección, re-encadenar next_id async (remesh propaga).
        {
            static std::int32_t last_chain_sel_id = -1;
            if (panel.imgui_state().spline_stick_chain && selected_spline >= 0 &&
                static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                const std::int32_t sid = edit_splines[static_cast<std::size_t>(selected_spline)].id;
                if (sid != last_chain_sel_id) {
                    last_chain_sel_id = sid;
                    // remesh_spline_one: avoid full follow cascade on mere selection
                    remesh_spline_one(edit_splines[static_cast<std::size_t>(selected_spline)], false, false);
                    log::info("[spline_chain] select id=" + std::to_string(sid) +
                              " stick_chain=1 → remesh_one (no force-follow)");
                }
            } else if (selected_spline < 0) {
                last_chain_sel_id = -1;
            }
        }

        static auto script_last = now;
        const float script_dt = std::chrono::duration<float>(now - script_last).count();
        script_last = now;

        sco_anim_dt = (std::min)(script_dt, 0.05f);
        // 1. Run at max 20 Hz (every 3 frames at 60 FPS) — scripts don't need 60 Hz
        {
            static int x5_was = -1;
            const int x5 = panel.imgui_state().traffic_light_x5 ? 1 : 0;
            if (x5 != x5_was) {
                log::info("traffic lights x5 -> " + std::to_string(x5) + " (frame " + std::to_string(editor_frame_serial) +
                          ", addr " + std::to_string(reinterpret_cast<std::uintptr_t>(&panel.imgui_state().traffic_light_x5)) + ")");
                x5_was = x5;
            }
        }
        static int s_tick_frame = 0;
        static float s_script_acc = 0.f; // real time since the scripts last ran
        // Cap only real stalls (loading): at 1 GHz three frames easily pass 0.25 s and the
        // old cap made the traffic-light clock run at half speed.
        s_script_acc = (std::min)(s_script_acc + script_dt, 1.0f);
        if (++s_tick_frame < 3) {
            // Still need to advance traffic light controllers
            scenery_scripts.set_traffic_time_scale(panel.imgui_state().traffic_light_x5 ? 5.f : 1.f);
            // CPU focus = camera target; radius linked to UI visual (editor_cpu_radius_m).
            cpu_focus_x = camera.target_x;
            cpu_focus_y = camera.target_y;
        } else {
            s_tick_frame = 0;
            scenery_scripts.set_traffic_time_scale(panel.imgui_state().traffic_light_x5 ? 5.f : 1.f);
            // CPU focus = camera target; radius linked to UI visual (editor_cpu_radius_m).
            cpu_focus_x = camera.target_x;
            cpu_focus_y = camera.target_y;
            // tick_near: same metres as options dialog; 0 = no distance filter (full map scripts).
            {
                auto _watch = main_watchdog.phase("scripts.tick_near");
                auto _perf = frame_profiler.scope(MapEditorFrameProfiler::ScriptTick);
                if (sa_scripts_dirty && !sa_spawn_active) {
                    scenery_scripts.sync(edit_objects, cfg.omsi_root);
                    sa_scripts_dirty = false;
                    sa_scripts_last_sync = std::chrono::steady_clock::now();
                }
                // Cap script radius to prevent runaway tick cost
                float script_radius_m = (std::min)(editor_cpu_radius_m, 300.f);
                // Scripts and traffic-light clocks run on real time every 3rd frame, whether or not the camera
                // moves (ticking only on camera moves froze every light and every time-driven script); the
                // radius keeps the cost bounded.
                if (stream_busy()) script_radius_m = (std::min)(script_radius_m, 150.f);
                scenery_scripts.tick_near(s_script_acc, camera.target_x, camera.target_y, script_radius_m, edit_objects);
                s_script_acc = 0.f;
            }
        }
        // [attachObj]: hang on what streaming / a camera move brought in (the radius pump that used to do
        // this is no longer called, so after the first load attachments only appeared at boot)
        {
            static float s_att_cx = -1.e9f, s_att_cy = -1.e9f;
            const float adx = camera.target_x - s_att_cx, ady = camera.target_y - s_att_cy;
            const bool att_moved = adx * adx + ady * ady > 40.f * 40.f;
            const bool att_retry = attach_wait_parent && glfwGetTime() - attach_wait_t > 1.0;
            if (!stream_busy() && (attach_pending || att_retry || att_moved)) {
                s_att_cx = camera.target_x;
                s_att_cy = camera.target_y;
                attach_wait_t = glfwGetTime();
                attach_wait_parent = false;
                const float att_r = (std::max)(editor_visual_radius_m, 350.f);
                spawn_edit_attachments([&](float x, float y) {
                    const float ex = x - camera.target_x, ey = y - camera.target_y;
                    return ex * ex + ey * ey <= att_r * att_r;
                });
            }
        }
        sco_anim_dt = (std::min)(script_dt, 0.05f);
        // path_rails_visual_radius: run at max 10 Hz (every 6 frames)
        static int s_pr_frame = 0;
        if (++s_pr_frame < 6) {
        } else {
            s_pr_frame = 0;
            {
                auto _watch = main_watchdog.phase("path_rails_visual_radius");
                auto _perf = frame_profiler.scope(MapEditorFrameProfiler::PathRails);
                ensure_path_rails_visual_radius();
            }
        }
        {
            auto _watch = main_watchdog.phase("hidden_objects_ghost");
            // Only FORCE hide. Never force-show every frame — that undoes distance cull
            // and revives (0,0) ghost chunks with identity matrices.
            const auto& hid_o = panel.imgui_state().hidden_object_ids;
            const auto& hid_s = panel.imgui_state().hidden_spline_ids;
            const float op = panel.imgui_state().mesh_opacity;
            const bool ghost = op < 0.995f;
            const float amt = 1.f - op;
            // Perf: with nothing hidden and no ghost opacity this was ~20k hash lookups every frame.
            if (!hid_o.empty() || ghost) {
                for (auto& sp : scene_places) {
                    if (!sp.alive) continue;
                    if (hid_o.count(sp.object_id)) renderer.set_chunk_visible(sp.chunk, false);
                    else if (ghost) renderer.set_chunk_highlight(sp.chunk, amt, 0.75f, 0.75f, 0.78f);
                }
            }
            for (auto& g : spline_gpu) {
                if (!g.alive) continue;
                const bool hide = hid_s.count(g.id) != 0;
                for (const auto c : g.chunks) {
                    if (hide) renderer.set_chunk_visible(c, false);
                    else if (ghost) renderer.set_chunk_highlight(c, amt, 0.75f, 0.75f, 0.78f);
                }
            }
        }
        last = now;
        {
            auto _watch = main_watchdog.phase("ui_pump_poll");
            auto _perf = frame_profiler.scope(MapEditorFrameProfiler::UiPumpPoll);
            // Same as glfwPollEvents' own loop (it dispatches every window of this thread), but
            // timed per message so a slow window procedure or sent message shows in the log.
            // glfwPollEvents afterwards still does GLFW's bookkeeping (stuck shift keys, cursor).
            {
                using clk = std::chrono::steady_clock;
                const auto pump_t0 = clk::now();
                MSG msg{};
                int n_msgs = 0;
                double worst_ms = 0.0;
                UINT worst_msg = 0;
                wchar_t worst_cls[64] = L"";
                // Keep looking at the queue every frame: skipping PeekMessage when it was empty made
                // DXGI/DWM pace the window much harder (Present 80-100 ms instead of a few ms). The
                // empty-queue time-slice loss is handled by the loader threads' lower priority.
                for (;;) {
                    const auto peek_t0 = clk::now();
                    const BOOL got = PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
                    const double peek_ms = std::chrono::duration<double, std::milli>(clk::now() - peek_t0).count();
                    if (peek_ms > worst_ms) {
                        worst_ms = peek_ms;
                        worst_msg = 0xFFFF; // time inside PeekMessage: sent messages / input sync
                        wcscpy_s(worst_cls, L"(peek)");
                    }
                    if (!got) break;
                    if (msg.message == WM_QUIT) {
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                        continue;
                    }
                    ++n_msgs;
                    const auto d_t0 = clk::now();
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                    const double d_ms = std::chrono::duration<double, std::milli>(clk::now() - d_t0).count();
                    if (d_ms > worst_ms) {
                        worst_ms = d_ms;
                        worst_msg = msg.message;
                        if (!msg.hwnd || !GetClassNameW(msg.hwnd, worst_cls, 64)) wcscpy_s(worst_cls, L"(thread)");
                    }
                    if (n_msgs >= 256) break; // never starve the frame on a message flood
                }
                const auto glfw_t0 = clk::now();
                glfwPollEvents(); // GLFW's bookkeeping (stuck shift keys, cursor re-centring)
                const double glfw_ms = std::chrono::duration<double, std::milli>(clk::now() - glfw_t0).count();
                const double pump_ms = std::chrono::duration<double, std::milli>(clk::now() - pump_t0).count();
                static auto s_pump_log_t = clk::time_point{};
                if (pump_ms > 8.0 && clk::now() - s_pump_log_t > std::chrono::milliseconds(500)) {
                    s_pump_log_t = clk::now();
                    char cls[64]{};
                    WideCharToMultiByte(CP_UTF8, 0, worst_cls, -1, cls, 63, nullptr, nullptr);
                    log::info("UI_PUMP_SLOW ms=" + std::to_string(pump_ms) + " msgs=" + std::to_string(n_msgs) +
                              " worst=0x" + [](UINT m) { char b[16]; std::snprintf(b, 16, "%04X", m); return std::string(b); }(worst_msg) +
                              " worst_ms=" + std::to_string(worst_ms) + " class=" + cls +
                              " glfw_ms=" + std::to_string(glfw_ms));
                }
            }
        }
        {
            auto _watch = main_watchdog.phase("ui_pump_panel");
            auto _perf = frame_profiler.scope(MapEditorFrameProfiler::UiPumpPanel);
            panel.pump();
            ui::pump_map_text_editor();
            adjust_hud.pump();
        }
        // Keep the full event/edit logic labelled. Previously all uninstrumented operations appeared
        // only as phase=frame, hiding which subsystem owned long stalls.
        std::optional<omsi::MainThreadWatchdog::Scope> editor_logic_watch;
        editor_logic_watch.emplace(main_watchdog.phase("editor_logic"));
        // Reassign this phase at the boundaries of the large event/input regions, so future
        // long waits are attributed to the responsible subsystem rather than a 2,000-line loop.
        std::optional<omsi::MainThreadWatchdog::Scope> editor_subphase_watch;
        frame_profiler.begin_segment();
        {
            static bool enter_was = false;
            if (panel.keyboard_captured()) {
                const bool enter_now = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
                if (enter_now && !enter_was) (void)panel.try_apply_on_enter();
                enter_was = enter_now;
            } else {
                enter_was = false;
            }
        }
        if (panel.imgui_primary()) {
            if (adjust_hud_imgui.restore_requested) {
                if (adjust_hud_latched_mode == ui::EditorAdjustHud::Mode::Height)
                    apply_exact_height(adjust_hud_anchor_z);
                else if (adjust_hud_latched_mode == ui::EditorAdjustHud::Mode::Rotation)
                    apply_exact_yaw(adjust_hud_anchor_yaw);
                adjust_hud_imgui.restore_requested = false;
                adjust_hud_imgui.commit_pending = false;
            } else if (adjust_hud_imgui.commit_pending) {
                if (adjust_hud_latched_mode == ui::EditorAdjustHud::Mode::Height)
                    apply_exact_height(adjust_hud_imgui.commit_value);
                else if (adjust_hud_latched_mode == ui::EditorAdjustHud::Mode::Rotation)
                    apply_exact_yaw(adjust_hud_imgui.commit_value);
                adjust_hud_imgui.commit_pending = false;
            }
        } else if (auto exact = adjust_hud.poll_committed()) {
            if (adjust_hud_latched_mode == ui::EditorAdjustHud::Mode::Height) apply_exact_height(*exact);
            else if (adjust_hud_latched_mode == ui::EditorAdjustHud::Mode::Rotation) apply_exact_yaw(*exact);
        }

        if (terrain_align_gpu_dirty && !terrain_reload_active) {
            auto _watch = main_watchdog.phase("terrain_align_gpu_reload");

            // All callers converge on one incremental queue. The former non-reload branch
            // restamped AND rebuilt every terrain tile in one main-thread frame; on large maps
            // this was the 1-3 second editor_logic freeze seen in main_hang.log.
            if (!terrain_align_gpu_reload_only) {
                terrain_align_gpu_reload_tiles.clear();
                // Only rebuild terrain meshes that are currently resident in the GPU scene.
                // The previous loop queued every terrain field in the map; upload_terrain_gpu_stamped
                // then appended nonresident tiles too, causing a huge startup burst and continuous
                // shadow-map invalidation. Unloaded fields remain in RAM and are restamped on tile load.
                terrain_align_gpu_reload_tiles.reserve(terrain_chunks.size());
                // Build a field index once. The previous any_of(terrain_fields) for every
                // resident chunk made queue creation O(chunks * fields) before the 4 ms budget
                // even started; large maps could therefore freeze despite the incremental loop.
                auto tile_key = [](std::int32_t gx, std::int32_t gy) -> std::uint64_t {
                    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)) << 32) |
                           static_cast<std::uint32_t>(gy);
                };
                std::unordered_set<std::uint64_t> field_tiles;
                field_tiles.reserve(terrain_fields.size() * 2 + 1);
                for (const auto& tf : terrain_fields) field_tiles.insert(tile_key(tf.grid_x, tf.grid_y));
                std::unordered_set<std::uint64_t> queued_tiles;
                queued_tiles.reserve(terrain_chunks.size() * 2 + 1);
                for (const auto& tc : terrain_chunks) {
                    if (!tc.alive) continue;
                    const auto key = tile_key(tc.tile_gx, tc.tile_gy);
                    if (!field_tiles.count(key) || !queued_tiles.insert(key).second) continue;
                    terrain_align_gpu_reload_tiles.emplace_back(tc.tile_gx, tc.tile_gy);
                }
                terrain_align_gpu_reload_i = 0;
                terrain_align_gpu_reload_only = true;
                terrain_align_gpu_needs_restamp = true;
            }

            // Hand the whole batch to the background pipeline. Dirty requests raised while it runs
            // (spline drags, SA spawns) are coalesced into the NEXT batch instead of being dropped.
            const bool batch_restamp = terrain_align_gpu_needs_restamp;
            const auto batch_tiles = std::move(terrain_align_gpu_reload_tiles);
            terrain_align_gpu_reload_tiles.clear();
            terrain_align_gpu_dirty = false;
            terrain_align_gpu_reload_only = false;
            terrain_align_gpu_needs_restamp = false;
            terrain_align_gpu_reload_i = 0;
            terrain_reload_start(batch_tiles, batch_restamp);
        }
        if (terrain_reload_active) {
            auto _watch = main_watchdog.phase("terrain_align_gpu_reload");
            terrain_reload_pump();
        }

        if (dtm_job && dtm_job->finished.load()) {
            auto _watch = main_watchdog.phase("dtm_result_apply");
            if (dtm_job->thread.joinable()) dtm_job->thread.join();
            auto done = std::move(dtm_job);
            dtm_job.reset();
            if (done->result.ok && map::write_terrain_heights(done->map_path, done->heights)) {
                reload_terrain_gpu_tile(done->grid_x, done->grid_y);
                panel.set_status(done->result.message);
                log::info("editor DTM applied " + done->map_path.filename().string());
            } else if (done->result.message == "no key") {
                panel.set_status("Google DTM: ingresa API key");
            } else {
                panel.set_status("DTM: " + done->result.message);
            }
        }

        if (aerial_job && aerial_job->finished.load()) {
            auto _watch = main_watchdog.phase("aerial_result_apply");
            if (aerial_job->thread.joinable()) aerial_job->thread.join();
            auto done = std::move(aerial_job);
            aerial_job.reset();
            if (done->result.ok) {
                if (done->set_visible) {
                    geo_settings.tile_aerial_visible = true;
                    map::save_editor_geo_settings(cfg.global.root, geo_settings);
                }
                invalidate_aerial_thumb(done->grid_x, done->grid_y);
                reload_terrain_gpu_tile(done->grid_x, done->grid_y);
                panel.set_status(done->result.message + " → " + done->out_jpg.filename().string());
                log::info("editor aerial " + done->out_jpg.string());
            } else if (!done->result.message.empty() && done->result.message != "world coords required") {
                const std::wstring msg = L"Satellite download failed:\n" +
                                         std::wstring(done->result.message.begin(), done->result.message.end());
                MessageBoxW(glfwGetWin32Window(window), msg.c_str(), L"Warning", MB_OK | MB_ICONWARNING);
                panel.set_status("Aerial: " + done->result.message);
            }
        }

        editor_subphase_watch.emplace(main_watchdog.phase("editor_event_dispatch"));
        main_watchdog.mark("M.dispatch_start");
        auto ev = panel.take_events();
        if (view_only) {
            // Strip mutation UI; keep navigation, read-only catalog tools, folder/notepad.
            const bool keep_load_map = ev.request_load_map;
            const bool keep_open_tile = ev.request_open_tile_map;
            const bool keep_open_instance_map = ev.open_instance_map;
            const bool keep_ep = ev.open_entrypoint_picker;
            const bool keep_opts = ev.open_options_dialog;
            const bool keep_apply_opts = ev.apply_editor_options;
            const int keep_opts_r = ev.options_tile_radius;
            const int keep_opts_vis = ev.options_visual_radius_m;
            const float keep_opts_union = ev.options_union_pct;
            const float keep_opts_grad = ev.options_grad_pct;
            const bool keep_spawn_vc = ev.spawn_omsi_bus;
            const auto keep_vc_dff = ev.omsi_bus_path;
            const bool keep_bus_despawn = ev.omsi_bus_despawn;
            const bool keep_bus_reset = ev.omsi_bus_reset;
            const bool keep_shortcuts = ev.show_shortcuts_dialog;
            const bool keep_catalog_folder = ev.open_catalog_folder;
            const bool keep_edit_catalog = ev.edit_catalog_file;
            const bool keep_load_catalog = ev.load_catalog_file;
            const bool keep_scan_catalog = ev.load_catalog_directory;
            const bool keep_spawn_random_scan = ev.spawn_random_scan_folder;
            const bool keep_markers = ev.toggled_markers;
            const bool keep_show_ends = ev.show_spline_ends;
            const bool keep_show_path = ev.show_path_ends;
            const bool keep_rails = ev.show_path_rails;
            const bool keep_tool_changed = ev.tool_changed;
            const auto keep_tool = ev.tool;
            const int keep_teleport = ev.teleport_entry;
            const int keep_sel_obj = ev.select_object_index;
            const std::int32_t keep_sel_obj_id = ev.select_object_id;
            const int keep_sel_spl = ev.select_spline_index;
            const std::int32_t keep_sel_spl_id = ev.select_spline_id;
            const int keep_cat_obj = ev.select_catalog_object;
            const int keep_cat_spl = ev.select_catalog_spline;
            const bool any_mutation =
                ev.open_spline_creator || ev.reload_catalog_file || ev.create_tile || ev.delete_tile ||
                ev.attach_to_object || ev.attach_to_spline || ev.parent_to_object || ev.spline_split ||
                ev.spline_complete || ev.spline_align_start || ev.spline_align_end || ev.sli_bake_pick ||
                ev.adapt_spline_gradient_terrain || ev.adapt_spline_clearance ||
                ev.sli_bake_run || ev.sli_bake_replace || ev.tile_bake_run || ev.tile_bake_replace || ev.tile_bake_fbx || ev.create_lightmap || ev.delete_tile_aerial ||
                ev.apply_tile_dtm || ev.import_tile_dem || ev.download_tile_aerial || ev.convert_world_coords ||
                ev.ground_create_layer || ev.ground_delete_layer || ev.ground_edit_layer || ev.ground_apply ||
                ev.tt_create_track || ev.tt_delete_track_entry || ev.tt_create_trip_t1 ||
                ev.tt_create_trip_t2 || ev.tt_delete_station || ev.chrono_plus || ev.chrono_minus ||
                ev.chrono_remove || ev.object_edit.valid || ev.apply_path_rule ||
                ev.apply_object || ev.apply_spline || ev.apply_spline_mirror_only;
            if (any_mutation) {
                panel.set_status("Modo solo visualización — acción de edición bloqueada");
                editor_log::event("VIEW_ONLY_BLOCK", "panel UI mutation");
            }
            // Settings are not map edits: keep them in view-only too. They used to be wiped here, so in
            // "Solo visualizar" VSync / Vista / texture-limit changes were never applied nor saved.
            const bool keep_vsync_set = ev.options_vsync_set;
            const bool keep_vsync = ev.options_vsync;
            const bool keep_save_ui = ev.save_ui_settings;
            const bool keep_save_geo = ev.save_geo_settings;
            const bool keep_reload_tex = ev.reload_file_textures;
            const bool keep_tex256 = ev.tex_max_256;
            const bool keep_autofill = ev.autofill_spline_ends;
            ev = {};
            ev.options_vsync_set = keep_vsync_set;
            ev.options_vsync = keep_vsync;
            ev.save_ui_settings = keep_save_ui;
            ev.save_geo_settings = keep_save_geo;
            ev.reload_file_textures = keep_reload_tex;
            ev.tex_max_256 = keep_tex256;
            ev.autofill_spline_ends = keep_autofill;
            ev.request_load_map = keep_load_map;
            ev.request_open_tile_map = keep_open_tile;
            ev.open_instance_map = keep_open_instance_map;
            ev.open_entrypoint_picker = keep_ep;
            ev.open_options_dialog = keep_opts;
            ev.apply_editor_options = keep_apply_opts;
            if (keep_apply_opts) {
                ev.options_tile_radius = keep_opts_r;
                ev.options_visual_radius_m = keep_opts_vis;
                ev.options_union_pct = keep_opts_union;
                ev.options_grad_pct = keep_opts_grad;
            }
            ev.spawn_omsi_bus = keep_spawn_vc;
            ev.omsi_bus_path = keep_vc_dff;
            ev.omsi_bus_despawn = keep_bus_despawn;
            ev.omsi_bus_reset = keep_bus_reset;
            ev.show_shortcuts_dialog = keep_shortcuts;
            ev.open_catalog_folder = keep_catalog_folder;
            ev.edit_catalog_file = keep_edit_catalog;
            ev.load_catalog_file = keep_load_catalog;
            ev.load_catalog_directory = keep_scan_catalog;
            ev.spawn_random_scan_folder = keep_spawn_random_scan;
            ev.toggled_markers = keep_markers;
            ev.show_spline_ends = keep_show_ends;
            ev.show_path_ends = keep_show_path;
            ev.show_path_rails = keep_rails;
            ev.tool_changed = keep_tool_changed;
            ev.tool = keep_tool;
            ev.teleport_entry = keep_teleport;
            ev.select_object_index = keep_sel_obj;
            ev.select_object_id = keep_sel_obj_id;
            ev.select_spline_index = keep_sel_spl;
            ev.select_spline_id = keep_sel_spl_id;
            ev.select_catalog_object = keep_cat_obj;
            ev.select_catalog_spline = keep_cat_spl;
        }
        if (cfg.test_spline_creator && !view_only) {
            static bool spline_creator_test_armed = true;
            if (spline_creator_test_armed && frames >= 3) {
                ev.open_spline_creator = true;
                spline_creator_test_armed = false;
            }
        }
        // Forward pointer to Ultralight BEFORE over_ui so capture flags are fresh this frame.
        // Use GLFW window coords (same space as ImGui panel width), not framebuffer pixels.
        double ul_vx = 0.0, ul_vy = 0.0;
        if (ultralight_ui) {
            glfwGetCursorPos(window, &ul_vx, &ul_vy);
            ultralight_overlay.on_mouse_move(ul_vx, ul_vy);
            static bool ul_lmb = false;
            const bool lmb_now = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb_now != ul_lmb) {
                ultralight_overlay.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT,
                                                   lmb_now ? GLFW_PRESS : GLFW_RELEASE, ul_vx, ul_vy);
                ul_lmb = lmb_now;
            }
        }
        const bool over_ui = panel.pointer_over_panel() || imgui_overlay.wants_capture_mouse() ||
                             (ultralight_ui && ultralight_overlay.pointer_over_panel(ul_vx, ul_vy));
#include "modules/mod_spray.inc.cpp"
#include "modules/mod_beamng_frame.inc.cpp"
        // Only block editor hotkeys when a text field is actively being typed into.
        // WantCaptureKeyboard alone stays true after ImGui spins and was locking G/R/Z/undo
        // until the user left-clicked the 3D view (losing multi-select).
        const bool typing_ui = panel.keyboard_captured() ||
                                 imgui_overlay.wants_text_input() ||
                                 (ultralight_ui && ultralight_overlay.wants_capture_keyboard());
        // F5 = hot-reload HTML/JS from disk (no C++ rebuild).
        {
            static bool f5_was = false;
            const bool f5 = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
            if (ultralight_ui && f5 && !f5_was) ultralight_overlay.reload();
            f5_was = f5;
        }
        const HWND glfw_hwnd = glfwGetWin32Window(window);
        const bool editor_keys_live = [&]() {
            if (bus_drive_active) return false;
            // pedestrian mode owns WASD / space; walk-edit (F7) keeps the rest of the editor's keys
            if (coop_walk_active && !(coop_walk_edit && !coop_walk_cursor)) return false;
            if (typing_ui && edit_mode == EditMode::None) return false;
            const HWND fg = GetForegroundWindow();
            if (!fg) return false;
            if (fg != glfw_hwnd) {
                if (!(adjust_hud.visible() &&
                      (fg == adjust_hud.hwnd() || IsChild(adjust_hud.hwnd(), fg))))
                    return false;
            }
            return true;
        }();
        editor_shortcuts.input_live = editor_keys_live;
        auto hotkey_down = [&](int /*glfw_key*/, int vk) -> bool {
            if (!editor_keys_live) return false;
            // (walking: W A S D, E, space and shift are the walker's)
            if (coop_walk_active && (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' || vk == 'E' || vk == VK_SPACE ||
                                     vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT))
                return false;
            return (GetAsyncKeyState(vk) & 0x8000) != 0;
        };
        // Clicking the 3D view (not the panel) restores Win32 focus to the GLFW root.
        if (!over_ui && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS &&
            edit_mode != EditMode::GrabHeight && edit_mode != EditMode::Rotate && !adjust_hud_latched) {
            bool steal_focus = true;
            if (adjust_hud.visible()) {
                POINT pt{};
                GetCursorPos(&pt);
                const HWND under = WindowFromPoint(pt);
                if (under == adjust_hud.hwnd() || IsChild(adjust_hud.hwnd(), under)) steal_focus = false;
            }
            if (steal_focus) SetFocus(glfw_hwnd);
        }

        main_watchdog.mark("M.dispatch.L01_454");
        // Tile page info (WGS84 bounds, hints, panel page): 2.6 ms that ran every frame. It only has to
        // follow the camera: every 250 ms while the Tile tab is open (its edits are read here), else 1 s.
        static auto tile_info_last = std::chrono::steady_clock::time_point{};
        const auto tile_info_now = std::chrono::steady_clock::now();
        const auto tile_info_every = panel.current_tool() == ui::EditorTool::Tile ? std::chrono::milliseconds(250)
                                                                                  : std::chrono::milliseconds(1000);
        if (ev.save_geo_settings || tile_info_now - tile_info_last >= tile_info_every) {
            tile_info_last = tile_info_now;
            ui::EditorTilePageInfo tpi{};
            tpi.valid = true;
            tpi.coord_kind = map::detect_map_coord_kind(cfg.global);
            tpi.can_convert = tpi.coord_kind == map::MapCoordKind::Classic && !cfg.global.world_coordinates;
            const auto& layout = world.tile_layout();
            if (panel.current_tool() == ui::EditorTool::Tile) {
                if (!centerkachel.valid ||
                    map::get_kachel_index_with_coord(cfg.global, centerkachel.gx, centerkachel.gy) < 0)
                    sync_centerkachel_from_camera();
            }
            const auto active = panel.current_tool() == ui::EditorTool::Tile
                                    ? centerkachel_as_active()
                                    : resolve_streaming_tile_at_camera();
            tpi.grid_x = active.grid_x;
            tpi.grid_y = active.grid_y;
            tpi.tile_size_m = active.size_meters;
            tpi.latitude_deg = active.latitude_deg;
            tpi.tile_in_map = active.in_map;
            if (tpi.tile_in_map) {
                tpi.can_create_n =
                    map::get_kachel_index_with_coord(cfg.global, tpi.grid_x, tpi.grid_y + 1) < 0;
                tpi.can_create_s =
                    map::get_kachel_index_with_coord(cfg.global, tpi.grid_x, tpi.grid_y - 1) < 0;
                tpi.can_create_e =
                    map::get_kachel_index_with_coord(cfg.global, tpi.grid_x + 1, tpi.grid_y) < 0;
                tpi.can_create_w =
                    map::get_kachel_index_with_coord(cfg.global, tpi.grid_x - 1, tpi.grid_y) < 0;
            }
            for (const auto& tile : world.tiles()) {
                if (tile.grid_x == tpi.grid_x && tile.grid_y == tpi.grid_y) {
                    tpi.map_path = tile.path;
                    break;
                }
            }
            if (const auto bounds = map::tile_wgs84_bounds(cfg.global, layout, active.grid_x, active.grid_y,
                                                           geo_settings.ref_lat, geo_settings.ref_lon)) {
                tpi.wgs84_summary = map::format_tile_wgs84_summary(*bounds);
            }
            if (cfg.global.world_coordinates) {
                tpi.hint = "Web Mercator: tile = 611.5·cos(lat) m desde posicion camara (tt-viewer). GCBA/IGN TMS.";
                tpi.geo_ready = true;
            } else if (tpi.coord_kind != map::MapCoordKind::Classic) {
                tpi.hint = "Global tile names — satelite/DTM via bounds WGS84.";
                tpi.geo_ready = true;
            } else if (!tpi.tile_in_map) {
                tpi.hint = "No hay tile aqui: pestana Tile → Crear tile (camara). Classic ref lat/lon o [worldcoordinates].";
                tpi.geo_ready =
                    std::fabs(geo_settings.ref_lat) > 1e-4 || std::fabs(geo_settings.ref_lon) > 1e-4;
            } else {
                tpi.hint = "Classic: ref lat/lon SW de tile_0_0 (ej. BA: -34.60, -58.38) o [worldcoordinates].";
                tpi.geo_ready =
                    std::fabs(geo_settings.ref_lat) > 1e-4 || std::fabs(geo_settings.ref_lon) > 1e-4;
            }
            // Keep geo_settings in sync with UI every frame — otherwise set_tile_page
            // resets provider/resolution to stale values and ignores the user's choices.
            panel.read_tile_page_edits(geo_settings);
            panel.read_tile_page_flags(geo_settings.tile_aerial_visible, geo_settings.aerial_resolution_pos);
            panel.set_tile_page(tpi, geo_settings);
            imgui_tile_info = tpi;
        }
        main_watchdog.mark("M.after_tile_page_info");
        // Tile tab edited ref lat/lon, API keys or aerial flags: write them now (per-map + global copy).
        if (ev.save_geo_settings) {
            map::save_editor_geo_settings(cfg.global.root, geo_settings);
            panel.set_status("Origen geo / claves guardados");
        }
        {
            static bool aerial_vis_saved = geo_settings.tile_aerial_visible;
            if (geo_settings.tile_aerial_visible != aerial_vis_saved) {
                aerial_vis_saved = geo_settings.tile_aerial_visible;
                map::save_editor_geo_settings(cfg.global.root, geo_settings);
                // Rebuild tiles on the background pipeline. The old synchronous loop decoded 8k
                // aerial JPEGs and uploaded them on the frame thread, freezing the editor.
                std::vector<std::pair<std::int32_t, std::int32_t>> aerial_tiles;
                aerial_tiles.reserve(world.tiles().size());
                for (const auto& tile : world.tiles())
                    aerial_tiles.emplace_back(tile.grid_x, tile.grid_y);
                terrain_reload_start(aerial_tiles, false);
                panel.set_status(geo_settings.tile_aerial_visible ? "Tile aerials ON (guardado)"
                                                                  : "Tile aerials OFF (guardado)");
            }
        }

        main_watchdog.mark("M.before_convert");
        if (ev.convert_world_coords) {
            auto _watch = main_watchdog.phase("convert_world_coords");
            geo_settings = ev.tile_geo_edit;
            geo_settings.tile_aerial_visible = ev.tile_aerial_visible;
            geo_settings.aerial_resolution_pos = ev.aerial_resolution_pos;
            map::save_editor_geo_settings(cfg.global.root, geo_settings);
            if (cfg.global.world_coordinates) {
                MessageBoxW(glfw_hwnd, L"Map has already world coordinates!", L"Informationen",
                            MB_OK | MB_ICONINFORMATION);
            } else {
                bool ok = true;
                if (cfg.workspace) {
                    ok = map::workspace_prepare_global_cfg_for_write(*cfg.workspace);
                    if (ok) cfg.global.global_cfg_path = cfg.workspace->actual_global_cfg;
                }
                if (ok && map::enable_world_coordinates(cfg.global.global_cfg_path)) {
                cfg.global.world_coordinates = true;
                panel.set_status("World coordinates enabled — reload editor for tile metric");
                log::info("editor converted map to [worldcoordinates]");
                } else {
                panel.set_status("Convert failed (global.cfg)");
                }
            }
        }
        if (ev.download_tile_aerial || ev.apply_tile_dtm) {
            panel.read_tile_page_edits(geo_settings);
            panel.read_tile_page_flags(geo_settings.tile_aerial_visible, geo_settings.aerial_resolution_pos);
            map::save_editor_geo_settings(cfg.global.root, geo_settings);
        }
        main_watchdog.mark("M.before_tile_edit");
        // H2: no tile editing in BeamNG mode (tiles are an OMSI concept).
        if (ev.create_tile && !cfg.beamng) {
            pending_create_tile = true;
            pending_create_dx = ev.create_tile_dx;
            pending_create_dy = ev.create_tile_dy;
        }
        if (ev.delete_tile && !cfg.beamng) {
            sync_centerkachel_from_camera();
            const std::int32_t gx = centerkachel.gx;
            const std::int32_t gy = centerkachel.gy;
            if (map::delete_map_tile(cfg.global, gx, gy)) {
                for (auto& tc : terrain_chunks) {
                    if (tc.tile_gx == gx && tc.tile_gy == gy) {
                        tc.alive = false;
                        renderer.set_chunk_visible(tc.chunk, false);
                    }
                }
                auto& tiles = world.tiles_mut();
                tiles.erase(std::remove_if(tiles.begin(), tiles.end(),
                                           [&](const map::MapTileData& t) {
                                               return t.grid_x == gx && t.grid_y == gy;
                                           }),
                            tiles.end());
                world.refresh_tile_layout(cfg.global);
                tile_frame_empty_pool_dirty = true;
                panel.set_status("Tile borrado (" + std::to_string(gx) + "," + std::to_string(gy) + ")");
            } else {
                panel.set_status("Delete Tile: no encontrado");
            }
        }

        main_watchdog.mark("M.lambdas");
        auto refresh_ground_layer_ui = [&]() {
            std::vector<std::string> labels;
            for (std::size_t i = 0; i < cfg.global.ground_textures.size(); ++i)
                labels.push_back(std::to_string(i) + ": " + cfg.global.ground_textures[i].diffuse.filename().string());
            const int sel = panel.ground_layer_sel() >= 0 ? panel.ground_layer_sel() : editor_ui_settings.terrain_ground_layer;
            panel.set_ground_layers(labels, sel);
        };
        auto persist_ui_settings = [&]() {
            panel.collect_ui_settings(editor_ui_settings);
            // Keep dirty-check fields stable even if collect omits them.
            editor_ui_settings.show_spline_ends = show_spline_ends;
            editor_ui_settings.show_path_ends = show_path_ends;
            editor_ui_settings.show_path_rails = show_path_rails;
            editor_ui_settings.autofill_spline_ends = autofill_spline_ends;
            editor_ui_settings.spline_stick_chain = panel.imgui_state().spline_stick_chain;
            if (!map::save_editor_ui_settings(cfg.global.root, editor_ui_settings)) {
                log::warn("editor prefs save failed path=" + options::editor_ui_prefs_path().string());
            }
        };
        auto refresh_chrono_ui = [&]() {
            std::vector<std::string> chronos;
            const auto chrono_dir = cfg.global.root / "Chrono";
            if (std::filesystem::exists(chrono_dir)) {
                for (const auto& e : std::filesystem::directory_iterator(chrono_dir)) {
                    if (e.is_directory()) chronos.push_back(e.path().filename().string());
                }
            }
            std::sort(chronos.begin(), chronos.end());
            panel.set_chrono_list(chronos);
        };
        auto refresh_tt_ui = [&]() {
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            // Unit134 lists: combo stems + entry/station rows ("0: id", "0: id/idx - name")
            panel.set_tt_from_man();
            // Keep legacy file-path lists for Open / Signal tabs
            const auto cat = map::scan_timetable_catalog(cfg.global.root);
            std::vector<std::filesystem::path> station;
            if (!cat.stn_links.empty()) station.push_back(cat.stn_links);
            for (const auto& b : cat.busstops) station.push_back(b);
            std::vector<std::filesystem::path> sigs;
            if (!cat.signal_routes.empty()) sigs.push_back(cat.signal_routes);
            // Do not overwrite entry lists — set_tt_lists fills tracks_list_ with filenames.
            // Only update signal list via a dedicated path if needed; entry lists stay from set_tt_from_man.
            if (panel.current_tool() == ui::EditorTool::SignalRts)
                panel.set_tt_lists(cat.tracks, cat.trips, station, sigs);
        };

        // Camera jump to selected track entry / station (Unit134 ListBox1Click / ListBox2Click)
        auto tt_jump_camera_to_selection = [&]() {
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) return;
            auto set_cam = [&](float x, float y, float z) {
                camera.target_x = x;
                camera.target_y = y;
                camera.target_z = z;
                camera.radius = 40.f;
            };
            // ListBox2Click: SetMapCamToStation(object_id)
            if (man.selected_trip >= 0 && man.selected_trip < (int)man.data.trips.size() &&
                man.selected_trip_station >= 0) {
                const auto& tp = man.data.trips[(size_t)man.selected_trip];
                if (man.selected_trip_station < (int)tp.stations.size()) {
                    const auto& st = tp.stations[(size_t)man.selected_trip_station];
                    for (const auto& o : edit_objects) {
                        if (o.id == st.object_id) {
                            set_cam(o.position.x, o.position.y, o.position.z);
                            return;
                        }
                    }
                }
            }
            // ListBox1Click: PathSegment → AbsPosCalc → mapcamTargetPos
            if (man.selected_track >= 0 && man.selected_track < (int)man.data.tracks.size() &&
                man.selected_track_entry >= 0) {
                const auto& tr = man.data.tracks[(size_t)man.selected_track];
                if (man.selected_track_entry < (int)tr.entries.size()) {
                    const auto& e = tr.entries[(size_t)man.selected_track_entry];
                    // Prefer path_rails match (owner_id + path_index) like TTrafficPathManager_PathSegment
                    for (const auto& rail : path_rails) {
                        if (!rail.alive) continue;
                        if (rail.owner_id != e.id) continue;
                        if (e.path_index >= 0 && rail.path_index != e.path_index) continue;
                        // Owner world pos
                        if (!rail.from_object) {
                            for (const auto& s : edit_splines) {
                                if (s.id != e.id) continue;
                                const auto mid = spline::evaluate_spline(s, s.length * 0.5f);
                                set_cam(mid.x, mid.y, mid.z);
                                return;
                            }
                        } else {
                            for (const auto& o : edit_objects) {
                                if (o.id != e.id) continue;
                                set_cam(o.position.x, o.position.y, o.position.z);
                                return;
                            }
                        }
                    }
                    // Fallback: match only owner id on splines/objects
                    for (const auto& s : edit_splines) {
                        if (s.id == e.id) {
                            const auto mid = spline::evaluate_spline(s, s.length * 0.5f);
                            set_cam(mid.x, mid.y, mid.z);
                            return;
                        }
                    }
                    for (const auto& o : edit_objects) {
                        if (o.id == e.id) {
                            set_cam(o.position.x, o.position.y, o.position.z);
                            return;
                        }
                    }
                }
            }
        };
        main_watchdog.mark("M.ttdata_track_changes");
        {
            // Load TTData for the CURRENT map; re-load when the map folder changes (File > Load map).
            static bool ui_lists_once = false;
            if (!ui_lists_once) {
                refresh_ground_layer_ui();
                refresh_chrono_ui();
                ui_lists_once = true;
            }
            // TTData undo tracking hashes the whole timetable: 4 times a second is enough (edits made
            // within 250 ms become one undo step); every frame it was ~0.2 ms of idle CPU.
            static int s_tt_tick = 0;
            if (++s_tt_tick >= 15) {
                s_tt_tick = 0;
                // (read on a worker thread: the lists are refreshed once it has arrived)
                static bool s_tt_was_loaded = false;
                omsi::tt::global_tt_man().ensure_loaded(cfg.global.root);
                const bool tt_now = omsi::tt::global_tt_man().loaded;
                if (tt_now && !s_tt_was_loaded) refresh_tt_ui();
                s_tt_was_loaded = tt_now;
                if (tt_now) omsi::tt::global_tt_man().track_changes();
            }
        }
        main_watchdog.mark("M.L01_726");
        // Rail change detection must not run (and rebuild rails) while a G / R / height drag is live.
        {
            const bool pr_paused_now = (edit_mode != EditMode::None) || group_grab_active || group_rotate_active;
            // G / R / height just ended: compare signatures right now so only what moved gets new rails.
            if (path_rails_detect_paused && !pr_paused_now) path_rails_detect_now = true;
            path_rails_detect_paused = pr_paused_now;
        }
        if (ev.refresh_tt) {
            // Force disk reload (drop dirty skip)
            {
                auto& man = omsi::tt::global_tt_man();
                man.dirty = false;
                man.loaded = false;
                man.load(cfg.global.root);
            }
            refresh_tt_ui();
            panel.set_status("TTData reloaded from disk: tracks=" +
                             std::to_string(omsi::tt::global_tt_man().data.tracks.size()) +
                             " trips=" + std::to_string(omsi::tt::global_tt_man().data.trips.size()));
        }
        if (ev.open_tt_file && !ev.tt_file_to_open.empty()) {
            ShellExecuteW(glfw_hwnd, L"open", ev.tt_file_to_open.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            panel.set_status("Opened " + ev.tt_file_to_open.filename().string());
        }
        auto prompt_name = [&](const wchar_t* title) -> std::string {
            wchar_t buf[128] = L"New";
            // Simple input via InputBox substitute: MessageBox + fixed name with timestamp.
            SYSTEMTIME st{};
            GetLocalTime(&st);
            swprintf_s(buf, L"%s_%04d%02d%02d_%02d%02d", title, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
            std::string out;
            for (wchar_t* p = buf; *p; ++p) out.push_back(static_cast<char>(*p));
            return out;
        };
        
        // The ImGui Tracks tab owns the selection (combos/lists); the old Win32 file-list sync is gone.
        auto sync_tt_man_selection = [&]() {
            omsi::tt::global_tt_man().ensure_loaded(cfg.global.root);
        };

        if (ev.tt_create_track) {
            sync_tt_man_selection();
            const auto name = prompt_name(L"Track");
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            const int idx = man.add_track(name);
            if (idx >= 0) {
                refresh_tt_ui();
                panel.set_status("Track en memoria: " + name + " (Save mapa para escribir .ttr)");
            } else {
                panel.set_status("Create track failed (nombre vacío o duplicado)");
            }
        }
        if (ev.tt_create_trip_t1 || ev.tt_create_trip_t2) {
            sync_tt_man_selection();
            const auto name = prompt_name(ev.tt_create_trip_t1 ? L"TripT1" : L"TripT2");
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            int idx = -1;
            if (ev.tt_create_trip_t1) {
                std::string track_name;
                if (man.selected_track >= 0 && man.selected_track < (int)man.data.tracks.size())
                    track_name = man.data.tracks[(size_t)man.selected_track].name;
                else if (!man.data.tracks.empty()) track_name = man.data.tracks[0].name;
                idx = man.add_trip_type1(name, track_name);
            } else {
                idx = man.add_trip_type2(name);
            }
            if (idx >= 0) {
                refresh_tt_ui();
                panel.set_status(std::string("Trip en memoria: ") + name + " (Save mapa → .ttp)");
            } else panel.set_status("Create trip failed");
        }
        if (ev.tt_copy_track || ev.tt_copy_trip || ev.tt_copy_both) {
            const auto src = panel.tt_selected_path();
            if (src.empty()) {
                panel.set_status("Copy: select a .ttr/.ttp first");
            } else {
                const auto stem = src.stem().string() + "_copy" + src.extension().string();
                const auto dst = src.parent_path() / stem;
                if (map::copy_tt_file(src, dst)) {
                    refresh_tt_ui();
                    panel.set_status("Copied → " + stem);
                } else {
                    panel.set_status("Copy failed");
                }
            }
        }
                if (ev.tt_assistent) {
            panel.set_status("Assistent: wizard OMSI no portado — usa Create type1/2 + Add Station/Trackentry");
        }
        if (ev.tt_edit_profiles) {
            const auto sel = panel.tt_selected_path();
            if (sel.empty() || sel.extension() != ".ttp") {
                panel.set_status("Edit Profiles: selecciona un .ttp");
            } else {
                omsi::tt::Trip trip;
                if (omsi::tt::load_trip_ttp(sel, trip)) {
                    if (trip.profiles.empty()) {
                        omsi::tt::TripProfile pr;
                        pr.name = "standard";
                        pr.minutes = 1.f;
                        trip.profiles.push_back(pr);
                    }
                    // Ensure one man_dep per station index for type2 (0..n-1) as editable skeleton
                    auto& pr = trip.profiles[0];
                    if (pr.man_dep.empty() && !trip.stations.empty()) {
                        for (int i = 0; i < static_cast<int>(trip.stations.size()); ++i)
                            pr.man_dep.emplace_back(i, static_cast<float>(i));
                    }
                    if (omsi::tt::save_trip_ttp(trip)) {
                        ShellExecuteW(glfw_hwnd, L"open", sel.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        panel.set_status("Profiles listos en " + trip.name + " — editá [profile]/[profile_man_dep_time] y guardá");
                    }
                }
            }
        }
        if (ev.tt_edit_timetable) {
            const auto tdir = cfg.global.root / "TTData";
            std::error_code ec;
            std::filesystem::create_directories(tdir, ec);
            // Open or create a default line file for quick edit
            auto line_path = tdir / "line_edit.ttl";
            if (!std::filesystem::exists(line_path)) {
                omsi::tt::Line ln;
                ln.path = line_path;
                ln.name = "line_edit";
                ln.user_allowed = true;
                ln.priority = 0;
                omsi::tt::Tour tour;
                tour.number = "1";
                tour.depot_name = "Depot";
                tour.day_mask = omsi::tt::kTourAllDays;
                ln.tours.push_back(tour);
                omsi::tt::save_line_ttl(ln);
            }
            ShellExecuteW(glfw_hwnd, L"open", line_path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            panel.set_status("Edit Timetable: " + line_path.filename().string() + " ([newtour]/[addtrip])");
        }
        if (ev.tt_add_from_list) {
            const auto sel = panel.tt_selected_path();
            if (sel.empty() || sel.extension() != ".ttp") {
                panel.set_status("Add from List: selecciona un .ttp");
            } else {
                omsi::tt::TimeTableData ttd{};
                omsi::tt::load_ttdata(cfg.global.root / "TTData", ttd);
                if (ttd.busstops.empty()) {
                    panel.set_status("Busstops.cfg vacio o ausente");
                } else {
                    omsi::tt::Trip trip;
                    if (omsi::tt::load_trip_ttp(sel, trip)) {
                        // Append first busstop not already in trip (simple list pick)
                        for (const auto& b : ttd.busstops) {
                            bool used = false;
                            for (const auto& s : trip.stations)
                                if (s.object_id == b.object_id) { used = true; break; }
                            if (used) continue;
                            omsi::tt::TripStation st{};
                            st.typ2 = trip.is_type2() || trip.track_name.empty();
                            st.object_id = b.object_id;
                            if (!st.typ2) {
                                st.name = b.name;
                                st.tile_index = b.tile_index;
                            }
                            trip.stations.push_back(st);
                            omsi::tt::save_trip_ttp(trip);
                            panel.set_status("Add from List: +" + b.name + " id=" + std::to_string(b.object_id));
                            break;
                        }
                    }
                }
            }
        }
        if (!ev.tt_terminus.empty() || !ev.tt_line.empty() || ev.tt_train_reversed) {
            const auto sel = panel.tt_selected_path();
            if (!sel.empty() && sel.extension() == ".ttp") {
                omsi::tt::Trip trip;
                if (omsi::tt::load_trip_ttp(sel, trip)) {
                    if (!ev.tt_terminus.empty()) trip.terminus = ev.tt_terminus;
                    if (!ev.tt_line.empty()) trip.line = ev.tt_line;
                    // train reverse: pending bool is "checked state" when toggled
                    trip.train_reversed = ev.tt_train_reversed;
                    omsi::tt::save_trip_ttp(trip);
                    panel.set_status("Trip actualizado: " + trip.name);
                }
            }
        }

        // Persist TT mode for map-click hooks (Add Trackentry / Add Station / Show TT).
        if (ev.tt_request_camera_jump || ev.tt_track_entry_sel_changed || ev.tt_station_sel_changed) {
            tt_jump_camera_to_selection();
            panel.set_tt_from_man(); // keep list selection in sync
        }
        if (ev.tt_track_combo_changed || ev.tt_trip_combo_changed) {
            // Selection already applied in handlers; refresh length + lists
            panel.set_tt_from_man();
        }

        if (ev.tt_mode >= 0 && ev.tt_mode <= 2) {
            extern int g_omsi_tt_mode;
            g_omsi_tt_mode = ev.tt_mode;
            panel.set_status(std::string("TT mode: ") +
                             (g_omsi_tt_mode == 0 ? "Add Trackentry" :
                              g_omsi_tt_mode == 1 ? "Add Station" : "Show Timetable"));
        }
        if (ev.tt_find_errors) {
            sync_tt_man_selection();
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            man.validate_all();
            // Also surface StnLink errors via SearchNext pattern
            const int bad_link = man.search_next_erroneous_stnlink(man.selected_stnlink);
            if (bad_link >= 0) {
                man.selected_stnlink = bad_link;
                man.data.errors.push_back("StnLink #" + std::to_string(bad_link) + " erroneous (from/to/segments)");
            }
            if (man.data.errors.empty()) {
                panel.set_status("Find Errors: OK — tracks=" + std::to_string(man.data.tracks.size()) +
                                 " trips=" + std::to_string(man.data.trips.size()) +
                                 " busstops=" + std::to_string(man.data.busstops.size()) +
                                 " stnlinks=" + std::to_string(man.data.stn_links.size()));
            } else {
                panel.set_status("Find Errors: " + std::to_string(man.data.errors.size()) +
                                 " — " + man.data.errors.front());
                for (const auto& e : man.data.errors) omsi::log::warn(std::string("TT: ") + e);
                // Jump camera to first broken track entry if possible
                for (int ti = 0; ti < (int)man.data.tracks.size(); ++ti) {
                    if (man.data.tracks[(size_t)ti].entries.empty()) {
                        man.selected_track = ti;
                        man.selected_track_entry = -1;
                        break;
                    }
                }
                panel.set_tt_from_man();
            }
        }
        main_watchdog.mark("M.L01_962");
        if (ev.tt_change_type2) {
            sync_tt_man_selection();
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            if (man.selected_trip < 0) panel.set_status("→T2: sin trip seleccionado");
            else if (man.change_trip_to_type2(man.selected_trip)) {
                refresh_tt_ui();
                panel.set_status("Trip → type2 en memoria (Save mapa)");
            } else panel.set_status("→T2 failed");
        }
        if (ev.tt_delete_station) {
            sync_tt_man_selection();
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            if (man.selected_trip < 0) panel.set_status("Delete station: sin trip");
            else {
                int si = man.selected_trip_station;
                if (si < 0) {
                    const auto& tp = man.data.trips[(size_t)man.selected_trip];
                    si = (int)tp.stations.size() - 1;
                }
                if (man.del_trip_busstop(man.selected_trip, si)) {
                    refresh_tt_ui();
                    panel.set_status("Estacion borrada (memoria; Save mapa)");
                } else panel.set_status("Trip sin estaciones");
            }
        }
        if (ev.tt_delete_track_entry) {
            sync_tt_man_selection();
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            if (man.selected_track < 0) panel.set_status("Delete track entry: sin track");
            else {
                int ei = man.selected_track_entry;
                if (ei < 0) {
                    const auto& tr = man.data.tracks[(size_t)man.selected_track];
                    ei = (int)tr.entries.size() - 1;
                }
                if (man.del_track_entry(man.selected_track, ei)) {
                    refresh_tt_ui();
                    panel.set_status("Track entry borrado (memoria; Save mapa)");
                } else panel.set_status("Track sin entries");
            }
        }

        // ── Station Links runtime (in-memory, flush to StnLinks.cfg) ───────────
        static omsi::tt::TimeTableData s_ttd{};
        static bool s_ttd_loaded = false;
        static int s_sl_sel = -1;
        static int s_sl_err_cursor = -1;
        extern int g_omsi_sl_mode; // defined in map_editor_app.cpp
        static std::int32_t s_sl_filter_from = 0;
        auto ensure_ttd = [&]() {
            if (!s_ttd_loaded) {
                omsi::tt::load_ttdata(cfg.global.root / "TTData", s_ttd);
                s_ttd_loaded = true;
            }
        };
        auto save_stnlinks = [&]() {
            ensure_ttd();
            const auto path = cfg.global.root / "TTData" / "StnLinks.cfg";
            s_ttd.root = cfg.global.root / "TTData";
            if (omsi::tt::save_stnlinks_cfg(path, s_ttd.stn_links))
                panel.set_status("StnLinks.cfg guardado (" + std::to_string(s_ttd.stn_links.size()) + " links)");
            else
                panel.set_status("Error guardando StnLinks.cfg");
        };
        auto refresh_sl_lists = [&]() {
            ensure_ttd();
            // Populate station_list / sl_links via set_tt_lists station vector of synthetic labels
            // Stored as paths with filename = "from->to#idx" for selection parsing
            std::vector<std::filesystem::path> link_paths;
            for (std::size_t i = 0; i < s_ttd.stn_links.size(); ++i) {
                const auto& L = s_ttd.stn_links[i];
                if (s_sl_filter_from != 0 && L.from_id != s_sl_filter_from) continue;
                link_paths.push_back(std::filesystem::path(
                    "sl_" + std::to_string(i) + "_" + std::to_string(L.from_id) + "-" + std::to_string(L.to_id)));
            }
            const auto cat = map::scan_timetable_catalog(cfg.global.root);
            std::vector<std::filesystem::path> bus;
            for (const auto& b : cat.busstops) bus.push_back(b);
            std::vector<std::filesystem::path> sigs;
            if (!cat.signal_routes.empty()) sigs.push_back(cat.signal_routes);
            panel.set_tt_lists(cat.tracks, cat.trips, link_paths.empty() ? bus : link_paths, sigs);
        };
        if (ev.refresh_tt) {
            s_ttd_loaded = false;
            ensure_ttd();
            refresh_sl_lists();
        }
        if (ev.sl_mode >= 0) {
            extern int g_omsi_sl_mode;
            g_omsi_sl_mode = ev.sl_mode;
            panel.set_status(std::string("SL mode: ") +
                             (g_omsi_sl_mode == 0 ? "Set Start" : g_omsi_sl_mode == 1 ? "Set End" : "Add Path Segments"));
        }
        if (ev.sl_new) {
            sync_tt_man_selection();
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            const int i = man.add_stnlink();
            panel.set_status("StnLink #" + std::to_string(i) + " en memoria (Save mapa)");
            refresh_tt_ui();
        }
        if (ev.sl_delete) {
            sync_tt_man_selection();
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            if (man.del_stnlink(man.selected_stnlink))
                panel.set_status("StnLink eliminado (memoria)");
            else panel.set_status("SL Delete: sin selección");
            refresh_tt_ui();
        }
        if (ev.sl_filter) {
            sync_tt_man_selection();
            auto& man = omsi::tt::global_tt_man();
            if (selected_object >= 0 && (size_t)selected_object < edit_objects.size()) {
                man.stnlink_filter_id = edit_objects[(size_t)selected_object].id;
                panel.set_status("Filter Start id=" + std::to_string(man.stnlink_filter_id));
            } else {
                man.stnlink_filter_id = 0;
                panel.set_status("Filter Start: limpio");
            }
            refresh_tt_ui();
        }
        if (ev.sl_search_err) {
            sync_tt_man_selection();
            auto& man = omsi::tt::global_tt_man();
            if (!man.loaded) man.load(cfg.global.root);
            const int f = man.search_next_erroneous_stnlink(man.selected_stnlink);
            if (f < 0) panel.set_status("Search Next Erroneous: ninguno");
            else {
                man.selected_stnlink = f;
                const auto& L = man.data.stn_links[(size_t)f];
                panel.set_status("Erroneous #" + std::to_string(f) + " from=" + std::to_string(L.from_id) +
                                 " to=" + std::to_string(L.to_id) + " segs=" + std::to_string(L.entries.size()));
            }
        }
        // Show Timetable of Station (mode 2): the window opens from the click handler (loop_04) and
        // from here when a busstop object is selected in that mode.
        if (g_omsi_tt_mode == 2 && selected_object >= 0 &&
            static_cast<std::size_t>(selected_object) < edit_objects.size()) {
            static std::int32_t s_last_tt_show_id = 0;
            const auto oid = edit_objects[static_cast<std::size_t>(selected_object)].id;
            if (oid != s_last_tt_show_id) {
                s_last_tt_show_id = oid;
                auto& man = omsi::tt::global_tt_man();
                man.ensure_loaded(cfg.global.root);
                omsi::tt::ui::global_ui().show_station_timetable(man, oid);
            }
        }

        if (ev.attach_to_object) {
            picking_varparent = false;
            varparent_pick_child_id = 0;
            if (selected_object < 0 && panel.imgui_object_edit().valid && panel.imgui_object_edit().id > 0) {
                const std::int32_t want = panel.imgui_object_edit().id;
                for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                    if (edit_objects[i].id != want) continue;
                    selected_object = static_cast<int>(i);
                    break;
                }
            }
            if (selected_object < 0) {
                panel.set_status("Attach obj: seleccioná primero el objeto hijo");
            } else {
                edit_mode = EditMode::AttachPickParent;
                panel.set_status("Attach obj: click el objeto PADRE en el 3D (Esc cancela) · AttPnt=" +
                                 std::to_string(panel.attach_point()));
            }
        }
        if (ev.attach_to_spline) {
            if (selected_object < 0 && panel.imgui_object_edit().valid && panel.imgui_object_edit().id > 0) {
                const std::int32_t want = panel.imgui_object_edit().id;
                for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                    if (edit_objects[i].id != want) continue;
                    selected_object = static_cast<int>(i);
                    break;
                }
            }
            if (selected_object < 0) {
                panel.set_status("Attach spline: seleccioná primero el objeto a atar");
            } else {
                edit_mode = EditMode::AttachPickSpline;
                panel.set_status("Attach spline: click la SPLINE en el 3D (Esc cancela)");
            }
        }
        if (ev.parent_to_object) {
            if (selected_object < 0 && panel.imgui_object_edit().valid && panel.imgui_object_edit().id > 0) {
                const std::int32_t want = panel.imgui_object_edit().id;
                for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                    if (edit_objects[i].id != want) continue;
                    selected_object = static_cast<int>(i);
                    break;
                }
            }
            if (selected_object < 0) {
                panel.set_status("Parent to: select child object first");
            } else {
                const auto& child = edit_objects[static_cast<std::size_t>(selected_object)];
                picking_varparent = true;
                varparent_pick_child_id = child.id;
                edit_mode = EditMode::AttachPickParent;
                panel.set_status("Parent to: click varparent object (script vars)");
            }
        }
        std::vector<int> split_targets;
        if (ev.group_split && group_sel_spl.size() > 1) {
            push_undo("group_split");
            split_targets = group_sel_spl;
        } else {
            split_targets.push_back(-1);
        }
        for (const int split_idx : split_targets) {
        if (split_idx >= 0) selected_spline = split_idx;
        if ((ev.spline_split || ev.group_split || pending_trim_split) && selected_spline >= 0 &&
            static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
            const bool from_trim = pending_trim_split;
            pending_trim_split = false;
            if (split_idx < 0) push_undo(from_trim ? "spline_split_ctrl_r" : "spline_split");
            auto& a = edit_splines[static_cast<std::size_t>(selected_spline)];
            int ncuts = ev.spline_split_cuts > 0 ? ev.spline_split_cuts : panel.imgui_state().spline_cut_count;
            if (ncuts < 1) ncuts = 1;
            const float L = a.length;
            std::vector<float> cuts;
            if (from_trim && !trim_cuts.empty()) {
                cuts = trim_cuts;
            } else if (ncuts <= 1) {
                cuts.push_back(from_trim ? trim_along : L * 0.5f);
            } else {
                for (int i = 1; i <= ncuts; ++i)
                    cuts.push_back(L * static_cast<float>(i) / static_cast<float>(ncuts + 1));
            }
            trim_cuts.clear();
            map::SplineInst src = a;
            const std::int32_t old_next = a.next_id;
            std::vector<float> u;
            for (float c : cuts) {
                if (c > 0.45f && c < L - 0.45f && (u.empty() || c - u.back() > 0.4f)) u.push_back(c);
            }
            if (u.empty()) {
                panel.set_status("Split: spline too short");
            } else {
                a.length = u[0];
                a.next_id = 0;
                remesh_spline(a);
                upsert_transient_spline(a);
                std::int32_t chain_prev = a.id;
                for (std::size_t i = 0; i < u.size(); ++i) {
                    const float start = u[i];
                    const float end = (i + 1 < u.size()) ? u[i + 1] : src.length;
                    const auto pose = spline::evaluate_spline(src, start);
                    map::SplineInst b = src;
                    b.id = next_spline_id();
                    b.prev_id = chain_prev;
                    b.next_id = 0;
                    b.position.x = pose.x;
                    b.position.y = pose.y;
                    b.position.z = pose.z;
                    b.heading_deg = pose.heading_rad * (180.f / 3.14159265f);
                    b.length = (std::max)(0.5f, end - start);
                    for (auto& s : edit_splines)
                        if (s.id == chain_prev) s.next_id = b.id;
                    edit_splines.push_back(b);
                    remesh_spline(edit_splines.back());
                    upsert_transient_spline(edit_splines.back());
                    chain_prev = b.id;
                }
                if (old_next != 0) {
                    for (auto& s : edit_splines)
                        if (s.id == chain_prev) s.next_id = old_next;
                    for (auto& s : edit_splines)
                        if (s.id == old_next) s.prev_id = chain_prev;
                }
                persist_editor_xy(a.position.x, a.position.y);
                refresh_editor_lists();
                panel.set_status("Split x" + std::to_string(static_cast<int>(u.size())) + " cortes");
            }
        }
        } // group split passes
        pump_water();
        if (ev.water_create || ev.water_set || ev.water_delete) handle_water_events(ev);
        // ── Group tools: select-all-same, relative / absolute adjust, list apply, flags, replace-all ─────────
        auto grp_is_sa = [&](const map::MapObjectInst& o) { return spline_attach_spawn_ids.count(o.id) != 0; };
        auto grp_is_att = [&](const map::MapObjectInst& o) { return grp_is_sa(o) || find_attach_for_child(o.id) != nullptr; };
        if (ev.group_select_same) {
            std::filesystem::path match_o, match_s;
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                match_o = edit_objects[static_cast<std::size_t>(selected_object)].sco_path;
            else if (!group_sel_obj.empty() && group_sel_obj.front() >= 0 &&
                     static_cast<std::size_t>(group_sel_obj.front()) < edit_objects.size())
                match_o = edit_objects[static_cast<std::size_t>(group_sel_obj.front())].sco_path;
            if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                match_s = edit_splines[static_cast<std::size_t>(selected_spline)].sli_path;
            else if (!group_sel_spl.empty() && group_sel_spl.front() >= 0 &&
                     static_cast<std::size_t>(group_sel_spl.front()) < edit_splines.size())
                match_s = edit_splines[static_cast<std::size_t>(group_sel_spl.front())].sli_path;
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size() &&
                std::find(group_sel_obj.begin(), group_sel_obj.end(), selected_object) == group_sel_obj.end()) {
                group_sel_obj.push_back(selected_object);
                group_sel_obj_ids.push_back(edit_objects[static_cast<std::size_t>(selected_object)].id);
            }
            if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size() &&
                std::find(group_sel_spl.begin(), group_sel_spl.end(), selected_spline) == group_sel_spl.end()) {
                group_sel_spl.push_back(selected_spline);
                group_sel_spl_ids.push_back(edit_splines[static_cast<std::size_t>(selected_spline)].id);
            }
            std::unordered_set<std::int32_t> have_o(group_sel_obj_ids.begin(), group_sel_obj_ids.end());
            std::unordered_set<std::int32_t> have_s(group_sel_spl_ids.begin(), group_sel_spl_ids.end());
            std::size_t added = 0, skipped_att = 0;
            if (!match_o.empty() && panel.current_tool() != ui::EditorTool::Spline) {
                for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                    if (edit_objects[i].sco_path != match_o || have_o.count(edit_objects[i].id)) continue;
                    if (ev.group_select_attach == 1 && grp_is_att(edit_objects[i])) { ++skipped_att; continue; }
                    group_sel_obj.push_back(static_cast<int>(i));
                    group_sel_obj_ids.push_back(edit_objects[i].id);
                    ++added;
                }
            }
            if (!match_s.empty() && panel.current_tool() == ui::EditorTool::Spline) {
                for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                    if (edit_splines[i].sli_path != match_s || have_s.count(edit_splines[i].id)) continue;
                    group_sel_spl.push_back(static_cast<int>(i));
                    group_sel_spl_ids.push_back(edit_splines[i].id);
                    ++added;
                }
            }
            panel.set_status("Seleccionados todos los iguales: +" + std::to_string(added) +
                             (skipped_att ? " (" + std::to_string(skipped_att) + " attachments omitidos)" : "") +
                             " - grupo " + std::to_string(group_sel_obj.size()) + " obj / " +
                             std::to_string(group_sel_spl.size()) + " spl");
        }
        main_watchdog.mark("M.L01_1294");
        if (ev.group_apply_delta || ev.group_set_z || ev.group_list_apply || ev.group_sco_flags ||
            ev.group_spline_align) {
            std::vector<int> oi = group_sel_obj, si = group_sel_spl;
            if (oi.empty() && selected_object >= 0) oi.push_back(selected_object);
            if (si.empty() && selected_spline >= 0) si.push_back(selected_spline);
            std::unordered_map<std::int32_t, const ui::EditorSidePanelState::GroupRow*> row_by_key;
            if (ev.group_list_apply)
                for (const auto& r : ev.group_rows) row_by_key[r.spline ? -r.id - 1 : r.id] = &r;
            if (!oi.empty() || !si.empty()) {
                // One undo step per gesture (a drag sends many deltas) / per button.
                if (ev.group_new_gesture || ev.group_set_z || ev.group_list_apply || ev.group_sco_flags ||
                    ev.group_spline_align)
                    push_undo("group_edit");
                int touched = 0, skipped_attach_obj = 0;
                // Pivot = centre of every item that moves. Rotation turns the group around it (heading grows
                // clockwise, like splines) and "set Z" shifts the group so its centre lands on Z.
                double pcx = 0.0, pcy = 0.0, pcz = 0.0;
                std::size_t pn = 0;
                for (int i : oi)
                    if (i >= 0 && static_cast<std::size_t>(i) < edit_objects.size() &&
                        !grp_is_att(edit_objects[static_cast<std::size_t>(i)])) {
                        const auto& o = edit_objects[static_cast<std::size_t>(i)];
                        pcx += o.position.x; pcy += o.position.y; pcz += o.position.z; ++pn;
                    }
                for (int i : si)
                    if (i >= 0 && static_cast<std::size_t>(i) < edit_splines.size()) {
                        const auto& sp = edit_splines[static_cast<std::size_t>(i)];
                        pcx += sp.position.x; pcy += sp.position.y; pcz += sp.position.z; ++pn;
                    }
                if (pn > 0) { pcx /= static_cast<double>(pn); pcy /= static_cast<double>(pn); pcz /= static_cast<double>(pn); }
                const bool about = ev.group_about_center && pn > 1;
                const float drot_deg = ev.group_delta[3];
                const float ang = drot_deg * 0.01745329252f;
                const float ca = std::cos(ang), sa = std::sin(ang);
                const float dz_center = ev.group_z - static_cast<float>(pcz);
                auto rotate_about = [&](float& x, float& y) {
                    const float dx = x - static_cast<float>(pcx), dy = y - static_cast<float>(pcy);
                    x = static_cast<float>(pcx) + dx * ca + dy * sa;
                    y = static_cast<float>(pcy) - dx * sa + dy * ca;
                };
                std::vector<GroupSettleItem> settle;
                for (int i : oi) {
                    if (i < 0 || static_cast<std::size_t>(i) >= edit_objects.size()) continue;
                    auto& o = edit_objects[static_cast<std::size_t>(i)];
                    if (grp_is_sa(o)) {
                        // Spline attachment: x = lateral, y = along, z = height on its spline (panel coordinates).
                        const auto sit = spline_attach_obj_to_entry.find(o.id);
                        if (sit == spline_attach_obj_to_entry.end() || sit->second >= edit_spline_attachments.size()) continue;
                        auto& entry = edit_spline_attachments[sit->second];
                        const map::SplineInst* sp = resolve_edit_spline_for_attach(entry);
                        if (ev.group_sco_flags) {
                            if (ev.group_sco_align >= 0) entry.att.align_terrain = ev.group_sco_align != 0;
                            if (ev.group_sco_tangential >= 0) entry.att.tangential = ev.group_sco_tangential != 0;
                        } else if (sp) {
                            auto pc = map::spline_attach_to_panel(*sp, entry.att);
                            if (ev.group_list_apply) {
                                const auto it = row_by_key.find(o.id);
                                if (it == row_by_key.end()) continue;
                                const auto& v = it->second->v;
                                pc.x = v[0]; pc.y = v[1]; pc.z = v[2];
                                entry.att.rotation_x = v[3]; entry.att.rotation_y = v[4]; entry.att.rotation_z = v[5];
                                entry.att.align_terrain = v[6] > 0.5f;
                                entry.att.tangential = v[7] > 0.5f;
                            } else if (ev.group_set_z) {
                                pc.z = ev.group_z;
                            } else if (ev.group_apply_delta) {
                                pc.x += ev.group_delta[0]; pc.y += ev.group_delta[1]; pc.z += ev.group_delta[2];
                                // They stay on their spline: rotation turns each one on its own origin.
                                entry.att.rotation_y += ev.group_delta[3];
                                entry.att.rotation_x += ev.group_delta_pb[0];
                                entry.att.rotation_z += ev.group_delta_pb[1];
                            }
                            map::spline_attach_from_panel(*sp, pc.x, pc.y, pc.z, entry.att);
                        } else {
                            continue;
                        }
                        update_spline_attachment_entry_poses(sit->second, entry.att.tangential);
                        if (auto* tile = find_tile_mut_xy(o.position.x, o.position.y)) {
                            auto path = tile->path;
                            if (cfg.workspace) {
                                const auto wr = map::workspace_tile_map_for_read(&*cfg.workspace, path);
                                if (!wr.empty()) path = wr;
                            }
                            map::update_sa_tangential_in_map(path, entry.att.instance_id, entry.att.tangential);
                            map::update_sco_align_terrain_in_map(path, "[splineAttachement]", entry.att.instance_id,
                                                                 entry.att.align_terrain);
                            mark_tile_dirty(tile->grid_x, tile->grid_y);
                            terrain_align_gpu_dirty = true;
                        }
                        settle.push_back({0, o.id});
                        ++touched;
                        continue;
                    }
                    if (find_attach_for_child(o.id)) { ++skipped_attach_obj; continue; } // pose comes from its parent object
                    if (ev.group_sco_flags) continue; // flags only exist for spline attachments
                    if (ev.group_list_apply) {
                        const auto it = row_by_key.find(o.id);
                        if (it == row_by_key.end()) continue;
                        const auto& v = it->second->v;
                        o.position.x = v[0]; o.position.y = v[1]; o.position.z = v[2];
                        o.rotation_deg.y = v[3]; o.rotation_deg.x = v[4]; o.rotation_deg.z = v[5];
                        o.align_terrain = v[6] > 0.5f;
                        if (o.align_terrain) o.position.z = sample_terrain_editor_xy(o.position.x, o.position.y);
                    } else if (ev.group_set_z) {
                        if (about) o.position.z += dz_center;
                        else o.position.z = ev.group_z;
                    } else if (ev.group_apply_delta) {
                        if (about && drot_deg != 0.f) rotate_about(o.position.x, o.position.y);
                        o.position.x += ev.group_delta[0];
                        o.position.y += ev.group_delta[1];
                        o.position.z += ev.group_delta[2];
                        o.rotation_deg.x += ev.group_delta[3];
                        o.rotation_deg.y += ev.group_delta_pb[0];
                        o.rotation_deg.z += ev.group_delta_pb[1];
                    }
                    sync_object_gpu(o);
                    upsert_transient_object(o);
                    persist_editor_xy(o.position.x, o.position.y);
                    settle.push_back({0, o.id});
                    ++touched;
                }
                for (int i : si) {
                    if (i < 0 || static_cast<std::size_t>(i) >= edit_splines.size()) continue;
                    auto& s = edit_splines[static_cast<std::size_t>(i)];
                    if (ev.group_sco_flags) continue;
                    if (ev.group_spline_align) {
                        const int al = (std::clamp)(ev.group_spline_align_value, 0, 4);
                        if (s.terrain_align != static_cast<std::uint8_t>(al)) {
                            s.terrain_align = static_cast<std::uint8_t>(al);
                            terrain_align_gpu_dirty = true;
                            if (auto* tile = find_tile_mut_xy(s.position.x, s.position.y)) {
                                auto path = tile->path;
                                if (cfg.workspace) {
                                    const auto wr = map::workspace_tile_map_for_read(&*cfg.workspace, path);
                                    if (!wr.empty()) path = wr;
                                }
                                map::update_spline_terrain_align_in_map(path, s.id, s.terrain_align);
                                mark_tile_dirty(tile->grid_x, tile->grid_y);
                            }
                        }
                    } else if (ev.group_list_apply) {
                        const auto it = row_by_key.find(-s.id - 1);
                        if (it == row_by_key.end()) continue;
                        const auto& v = it->second->v;
                        s.position.x = v[0]; s.position.y = v[1]; s.position.z = v[2]; s.heading_deg = v[3];
                        s.length = (std::max)(0.05f, v[4]); s.radius = v[5];
                        s.start_grad = v[6]; s.end_grad = v[7]; s.delta_height = v[8];
                        s.use_delta_height = std::fabs(v[8]) > 1.e-6f;
                        s.skew_start = v[9]; s.skew_end = v[10]; s.cant_start = v[11]; s.cant_end = v[12];
                        s.mirror = v[13] > 0.5f;
                        const int al = (std::clamp)(static_cast<int>(std::lround(v[14])), 0, 4);
                        if (s.terrain_align != static_cast<std::uint8_t>(al)) {
                            s.terrain_align = static_cast<std::uint8_t>(al);
                            terrain_align_gpu_dirty = true;
                            if (auto* tile = find_tile_mut_xy(s.position.x, s.position.y)) {
                                auto path = tile->path;
                                if (cfg.workspace) {
                                    const auto wr = map::workspace_tile_map_for_read(&*cfg.workspace, path);
                                    if (!wr.empty()) path = wr;
                                }
                                map::update_spline_terrain_align_in_map(path, s.id, s.terrain_align);
                                mark_tile_dirty(tile->grid_x, tile->grid_y);
                            }
                        }
                    } else if (ev.group_set_z) {
                        if (about) s.position.z += dz_center;
                        else s.position.z = ev.group_z;
                    } else if (ev.group_apply_delta) {
                        if (about && drot_deg != 0.f) rotate_about(s.position.x, s.position.y);
                        s.position.x += ev.group_delta[0];
                        s.position.y += ev.group_delta[1];
                        s.position.z += ev.group_delta[2];
                        s.heading_deg += ev.group_delta[3];
                    }
                    upsert_transient_spline(s);
                    persist_editor_xy(s.position.x, s.position.y);
                    settle.push_back({1, s.id});
                    ++touched;
                }
                xform_soa.rebuild_from(edit_objects);
                sync_all_end_markers();
                // Paths follow: owners whose position / heading / length changed get fresh rails right now.
                path_rails_detect_now = true;
                // Solid remesh of every spline runs in the group-settle pump (budgeted per frame, async UI) instead of
                // one blocking remesh per spline here.
                if (!settle.empty()) {
                    if (!begin_group_settle(std::move(settle), true)) {
                        for (int i : si)
                            if (i >= 0 && static_cast<std::size_t>(i) < edit_splines.size())
                                async_remesh_enqueue(edit_splines[static_cast<std::size_t>(i)]);
                    }
                }
                panel.set_status("Edicion de grupo aplicada a " + std::to_string(touched) + " elementos" +
                                 (skipped_attach_obj ? " (" + std::to_string(skipped_attach_obj) +
                                                           " attachObj omitidos: su pose viene del padre)"
                                                     : "") +
                                 " - en memoria, se guarda con Save");
            }
        }
        if (ev.group_replace_apply && !ev.group_replace_path.empty()) {
            const auto& np = ev.group_replace_path;
            int n = 0;
            if (ev.group_replace_spline) {
                std::filesystem::path match;
                if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                    match = edit_splines[static_cast<std::size_t>(selected_spline)].sli_path;
                else if (!group_sel_spl.empty() && group_sel_spl.front() >= 0 &&
                         static_cast<std::size_t>(group_sel_spl.front()) < edit_splines.size())
                    match = edit_splines[static_cast<std::size_t>(group_sel_spl.front())].sli_path;
                if (!match.empty() && match != np) {
                    push_undo("replace_all_sli");
                    for (auto& s : edit_splines) {
                        if (s.sli_path != match) continue;
                        s.sli_path = np;
                        if (n < 40) remesh_spline(s, false);
                        else async_remesh_enqueue(s);
                        upsert_transient_spline(s);
                        persist_editor_xy(s.position.x, s.position.y);
                        if (map::MapTileData* tile = find_tile_mut_xy(s.position.x, s.position.y)) {
                            sync_tile_lists_only(*tile);
                            mark_tile_dirty(tile->grid_x, tile->grid_y);
                        }
                        ++n;
                    }
                }
            } else {
                std::filesystem::path match;
                if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                    match = edit_objects[static_cast<std::size_t>(selected_object)].sco_path;
                else if (!group_sel_obj.empty() && group_sel_obj.front() >= 0 &&
                         static_cast<std::size_t>(group_sel_obj.front()) < edit_objects.size())
                    match = edit_objects[static_cast<std::size_t>(group_sel_obj.front())].sco_path;
                if (!match.empty() && match != np) {
                    push_undo("replace_all_sco");
                    for (auto& o : edit_objects) {
                        if (o.sco_path != match) continue;
                        o.sco_path = np;
                        spawn_editor_object_gpu(o);
                        fill_sco_map_extras(o);
                        upsert_transient_object(o);
                        persist_editor_xy(o.position.x, o.position.y);
                        if (map::MapTileData* tile = find_tile_mut_xy(o.position.x, o.position.y)) {
                            sync_tile_lists_only(*tile);
                            mark_tile_dirty(tile->grid_x, tile->grid_y);
                        }
                        ++n;
                    }
                    edit_obj_index_size = static_cast<std::size_t>(-1);
                    xform_soa.rebuild_from(edit_objects);
                }
            }
            path_rails_detect_now = true;
            panel.set_status("Reemplazados " + std::to_string(n) + " por " + np.filename().string() +
                             " (en memoria; se guarda con Save, Ctrl+Z deshace)");
        }
        main_watchdog.mark("M.L01_1549");
        if (ev.toggle_editor_hide) {
            auto& hs = panel.imgui_state();
            auto tog = [](std::unordered_set<std::int32_t>& set, std::int32_t id) {
                if (id <= 0) return;
                if (set.count(id)) set.erase(id);
                else set.insert(id);
            };
            std::unordered_set<std::int32_t> oids, sids;
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                oids.insert(edit_objects[static_cast<std::size_t>(selected_object)].id);
            if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                sids.insert(edit_splines[static_cast<std::size_t>(selected_spline)].id);
            for (int gi : group_sel_obj)
                if (gi >= 0 && static_cast<std::size_t>(gi) < edit_objects.size())
                    oids.insert(edit_objects[static_cast<std::size_t>(gi)].id);
            for (int gi : group_sel_spl)
                if (gi >= 0 && static_cast<std::size_t>(gi) < edit_splines.size())
                    sids.insert(edit_splines[static_cast<std::size_t>(gi)].id);
            for (auto id : oids) tog(hs.hidden_object_ids, id);
            for (auto id : sids) tog(hs.hidden_spline_ids, id);
            // Apply immediately this frame (don't wait for the per-frame pass).
            for (auto& sp : scene_places) {
                if (!sp.alive) continue;
                if (hs.hidden_object_ids.count(sp.object_id))
                    renderer.set_chunk_visible(sp.chunk, false);
            }
            for (auto& g : spline_gpu) {
                if (!g.alive) continue;
                const bool hide = hs.hidden_spline_ids.count(g.id) != 0;
                for (const auto c : g.chunks)
                    if (hide) renderer.set_chunk_visible(c, false);
            }

            // Deselect hidden items (only list can re-select them).
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size() &&
                hs.hidden_object_ids.count(edit_objects[static_cast<std::size_t>(selected_object)].id)) {
                selected_object = -1;
            }
            if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size() &&
                hs.hidden_spline_ids.count(edit_splines[static_cast<std::size_t>(selected_spline)].id)) {
                selected_spline = -1;
            }
            group_sel_obj.erase(std::remove_if(group_sel_obj.begin(), group_sel_obj.end(),
                [&](int gi) {
                    return gi >= 0 && static_cast<std::size_t>(gi) < edit_objects.size() &&
                           hs.hidden_object_ids.count(edit_objects[static_cast<std::size_t>(gi)].id) != 0;
                }), group_sel_obj.end());
            group_sel_spl.erase(std::remove_if(group_sel_spl.begin(), group_sel_spl.end(),
                [&](int gi) {
                    return gi >= 0 && static_cast<std::size_t>(gi) < edit_splines.size() &&
                           hs.hidden_spline_ids.count(edit_splines[static_cast<std::size_t>(gi)].id) != 0;
                }), group_sel_spl.end());
            group_sel_obj_ids.clear();
            for (int gi : group_sel_obj)
                if (gi >= 0 && static_cast<std::size_t>(gi) < edit_objects.size())
                    group_sel_obj_ids.push_back(edit_objects[static_cast<std::size_t>(gi)].id);
            group_sel_spl_ids.clear();
            for (int gi : group_sel_spl)
                if (gi >= 0 && static_cast<std::size_t>(gi) < edit_splines.size())
                    group_sel_spl_ids.push_back(edit_splines[static_cast<std::size_t>(gi)].id);

            panel.set_status("Ocultar (H) toggled");
        }
        if (ev.unhide_all_editor) {
            auto& hs = panel.imgui_state();
            const std::size_t n = hs.hidden_object_ids.size() + hs.hidden_spline_ids.size();
            hs.hidden_object_ids.clear();
            hs.hidden_spline_ids.clear();
            // Force visible immediately (same frame) for everything still alive.
            for (auto& sp : scene_places) {
                if (sp.alive) renderer.set_chunk_visible(sp.chunk, true);
            }
            for (auto& g : spline_gpu) {
                if (!g.alive) continue;
                for (const auto c : g.chunks) renderer.set_chunk_visible(c, true);
            }
            panel.set_status("Desocultar todo (" + std::to_string(n) + " items)");
        }
        if (ev.spline_complete) {
            if (selected_spline < 0) {
                panel.set_status("Complete: select source spline first");
            } else {
                edit_mode = EditMode::CompleteSpline;
                panel.set_status("Complete to...: click target end (max R=" +
                                 std::to_string(panel.spline_max_radius()) + ")");
            }
        }
        if (ev.sli_bake_pick) {
            sli_bake_pick = true;
            sli_bake_ready = false;
            panel.imgui_set_tool(ui::EditorTool::Spline);
            // Seed bake group with current selection so the already-selected spline(s) count.
            if (selected_spline >= 0 &&
                static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                const auto sid = edit_splines[static_cast<std::size_t>(selected_spline)].id;
                if (std::find(group_sel_spl.begin(), group_sel_spl.end(), selected_spline) ==
                    group_sel_spl.end()) {
                    group_sel_spl.push_back(selected_spline);
                    group_sel_spl_ids.push_back(sid);
                }
            }
            panel.set_status("Bake: " + std::to_string(group_sel_spl.size()) +
                             " SLI en grupo — click 3D para sumar/quitar. Esc cancela.");
        }
        if (ev.enter_mesh_edit) {
            if (sco_path_edit.active && !try_exit_sco_path_edit()) {
            } else {
                edit_mode = EditMode::Mesh;
                show_path_rails = false;
                show_path_ends = false;
                for (const auto& r : path_rails)
                    if (r.alive) renderer.set_chunk_visible(r.chunk, false);
                for (const auto& mk : end_markers)
                    if (mk.alive) renderer.set_chunk_visible(mk.chunk, false);
                mesh_edit.enter_snap = mesh_capture_snap();
                mesh_edit.session_bake_dir.clear();
                mesh_edit.o3d_edit_path.clear();
                mesh_edit.dirty = false;
                panel.set_status("Bake mesh: Vertices/Edges/Faces — click 3D");
            }
        }
        // object picked for "Editar mesh": shown again once the mesh editor is left
        if (mesh_edit.o3d_obj_id >= 0 && edit_mode != EditMode::Mesh) {
            auto& hs_o3d = panel.imgui_state();
            if (!mesh_edit.o3d_obj_was_hidden) {
                hs_o3d.hidden_object_ids.erase(mesh_edit.o3d_obj_id);
                for (auto& sp : scene_places)
                    if (sp.alive && sp.object_id == mesh_edit.o3d_obj_id) renderer.set_chunk_visible(sp.chunk, true);
            }
            mesh_edit.o3d_obj_id = -1;
        }
        if (ev.edit_o3d_file && !(sco_path_edit.active && !try_exit_sco_path_edit()) &&
            (edit_mode != EditMode::Mesh || try_exit_mesh())) {
            o3d_pick.active = true;
            o3d_pick.accept = false;
            o3d_pick.file = false;
            o3d_pick.obj_id = -1; // refresh the mesh list for the current selection
            panel.set_status("Editar mesh: clickea un objeto del mapa y elegi 'Editar' (o 'Desde archivo...')");
        }
        auto enter_o3d_edit = [&](const std::filesystem::path& file, const map::MapObjectInst* obj) {
            if (edit_mode == EditMode::Mesh && !try_exit_mesh()) return;
            edit_mode = EditMode::Mesh;
            show_path_rails = false;
            show_path_ends = false;
            for (const auto& r : path_rails)
                if (r.alive) renderer.set_chunk_visible(r.chunk, false);
            for (const auto& mk : end_markers)
                if (mk.alive) renderer.set_chunk_visible(mk.chunk, false);
            mesh_edit.session_bake_dir.clear();
            XMMATRIX base = XMMatrixIdentity();
            const XMMATRIX* pbase = nullptr;
            if (obj) {
                float th = 0.f;
                if (!find_attach_for_child(obj->id)) {
                    const auto sc = sco::load_sco(obj->sco_path);
                    th = terrain_add(sc ? sco::skip_terrain_add(*sc) : false, obj->position.x, obj->position.y);
                }
                base = sco_world_matrix(*obj, nullptr, th, 0.f, 0.f);
                pbase = &base;
            }
            if (!mesh_load_o3d(file, pbase)) {
                edit_mode = EditMode::None;
                return;
            }
            mesh_edit.enter_snap = mesh_capture_snap();
            if (obj) {
                auto& hs_o3d = panel.imgui_state();
                mesh_edit.o3d_obj_id = obj->id;
                mesh_edit.o3d_obj_was_hidden = hs_o3d.hidden_object_ids.count(obj->id) != 0;
                hs_o3d.hidden_object_ids.insert(obj->id);
                for (auto& sp : scene_places)
                    if (sp.alive && sp.object_id == obj->id) renderer.set_chunk_visible(sp.chunk, false);
            }
        };
        if (o3d_pick.file) {
            o3d_pick.file = false;
            OPENFILENAMEW ofn{};
            wchar_t file[MAX_PATH]{};
            std::wstring init_dir = (cfg.omsi_root / "Sceneryobjects").wstring();
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = glfw_hwnd;
            ofn.lpstrFilter = L"OMSI mesh (*.o3d)\0*.o3d\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrInitialDir = init_dir.c_str();
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                o3d_pick.active = false;
                enter_o3d_edit(std::filesystem::path(file), nullptr);
            }
        }
        if (o3d_pick.accept) {
            o3d_pick.accept = false;
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size() &&
                o3d_pick.choice >= 0 && static_cast<std::size_t>(o3d_pick.choice) < o3d_pick.meshes.size()) {
                const auto mesh_file = o3d_pick.meshes[static_cast<std::size_t>(o3d_pick.choice)];
                const map::MapObjectInst obj_copy = edit_objects[static_cast<std::size_t>(selected_object)];
                o3d_pick.active = false;
                enter_o3d_edit(mesh_file, &obj_copy);
            }
        }
        auto selected_sco_with_paths = [&]() -> const sco::SceneryObject* {
            const bool has_sel = selected_object >= 0 &&
                                 static_cast<std::size_t>(selected_object) < edit_objects.size();
            if (!has_sel) return nullptr;
            const sco::SceneryObject* s =
                sco::load_sco(edit_objects[static_cast<std::size_t>(selected_object)].sco_path);
            if (!s || (!s->has_path && !s->traffic_lights_group.has_value())) return nullptr;
            return s;
        };
        if (ev.enter_sco_path_edit) {
            if (selected_sco_with_paths()) {
                panel.clear_sco_path_pick();
                enter_sco_path_from_selection(false);
            } else {
                panel.begin_sco_path_pick();
                panel.set_status("Pick: clickea un .o3d con [path] en el viewport y luego 'Aceptar'");
            }
        }
        if (ev.sco_path_pick_accept) {
            if (selected_sco_with_paths()) {
                panel.clear_sco_path_pick();
                enter_sco_path_from_selection(false);
            } else {
                panel.set_status("Pick: el objeto elegido no tiene [path]. Clickéa otro .o3d.");
            }
        }
        if (ev.enter_sco_traffic) enter_sco_path_from_selection(true);
        if (ev.toggle_spline_multi_pick) {
            const bool on = !panel.imgui_state().spline_multi_pick;
            panel.imgui_state().spline_multi_pick = on;
            panel.imgui_state().object_multi_pick = false;
            if (on) {
                marquee.phase = MarqueePhase::None;
                marquee.dragging = false;
                sel_lasso_pts.clear();
                clear_marquee_box();
                panel.imgui_state().sel_mode_circle = false;
                panel.imgui_state().sel_mode_lasso = false;
                panel.imgui_state().sel_ui_mode = 4;
                if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size() &&
                    std::find(group_sel_spl.begin(), group_sel_spl.end(), selected_spline) == group_sel_spl.end()) {
                    group_sel_spl.push_back(selected_spline);
                    group_sel_spl_ids.push_back(edit_splines[static_cast<std::size_t>(selected_spline)].id);
                }
            } else {
                panel.imgui_state().sel_ui_mode = 0;
            }
            panel.set_status(on ? "Multi splines: cada click SUMA (Pick simple para salir)"
                                : "Multi-select splines: off");
        }
        if (ev.toggle_object_multi_pick) {
            const bool on = !panel.imgui_state().object_multi_pick;
            panel.imgui_state().object_multi_pick = on;
            panel.imgui_state().spline_multi_pick = false;
            if (on) {
                marquee.phase = MarqueePhase::None;
                marquee.dragging = false;
                sel_lasso_pts.clear();
                clear_marquee_box();
                panel.imgui_state().sel_mode_circle = false;
                panel.imgui_state().sel_mode_lasso = false;
                panel.imgui_state().sel_ui_mode = 4;
                if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size() &&
                    std::find(group_sel_obj.begin(), group_sel_obj.end(), selected_object) ==
                        group_sel_obj.end()) {
                    group_sel_obj.push_back(selected_object);
                    group_sel_obj_ids.push_back(edit_objects[static_cast<std::size_t>(selected_object)].id);
                }
            } else {
                panel.imgui_state().sel_ui_mode = 0;
            }
            panel.set_status(on ? "Multi objetos: cada click SUMA (Pick simple para salir)"
                                : "Multi-select objetos: off");
        }
        main_watchdog.mark("M.L01_1769");
        if (ev.cancel_group_marquee) {
            marquee.phase = MarqueePhase::None;
            marquee.intent = MarqueeIntent::SelectOnly;
            marquee.dragging = false;
            sel_lasso_pts.clear();
            sel_lasso_drawing = false;
            sel_circle_brush_upto = 0;
            clear_marquee_box();
            panel.imgui_state().sel_mode_circle = false;
            panel.imgui_state().sel_mode_lasso = false;
            panel.imgui_state().spline_multi_pick = false;
            panel.imgui_state().object_multi_pick = false;
            panel.imgui_state().sel_ui_mode = 0;
            panel.set_status("Pick simple");
        }
        if (ev.start_group_marquee && edit_mode == EditMode::None) {
            marquee.phase = MarqueePhase::Select;
            marquee.intent = MarqueeIntent::SelectOnly;
            marquee.dragging = false;
            sel_lasso_pts.clear();
            sel_lasso_drawing = false;
            sel_circle_brush_upto = 0;
            panel.imgui_state().spline_multi_pick = false;
            panel.imgui_state().object_multi_pick = false;
            if (ev.sel_use_lasso) {
                sel_shape = SelShapeMode::Lasso;
                panel.imgui_state().sel_mode_circle = false;
                panel.imgui_state().sel_mode_lasso = true;
                panel.imgui_state().sel_ui_mode = 3;
                panel.set_status("Lazo: dibujá con LMB y soltá (sigue activo hasta Pick simple)");
            } else if (ev.sel_use_circle) {
                sel_shape = SelShapeMode::Circle;
                panel.imgui_state().sel_mode_circle = true;
                panel.imgui_state().sel_mode_lasso = false;
                panel.imgui_state().sel_ui_mode = 2;
                panel.set_status("Círculo: mantené LMB y mové para seleccionar lo que toca; rueda=tamaño, Ctrl quita, Esc=Pick simple");
            } else {
                sel_shape = SelShapeMode::Rect;
                panel.imgui_state().sel_mode_circle = false;
                panel.imgui_state().sel_mode_lasso = false;
                panel.imgui_state().sel_ui_mode = 1;
                panel.set_status("Recuadro: arrastrá LMB (sigue activo hasta Pick simple)");
            }
            log::info("editor group marquee (UI button)");
            editor_log::event("GROUP_MARQUEE_ARM", session_state());
        }
        if (ev.save_selection_preset) {
            auto& st = panel.imgui_state();
            std::string pname = st.preset_name_buf;
            if (pname.empty()) pname = "preset";
            ui::EditorSidePanelImGuiState::EditorPreset pr;
            pr.name = pname;
            float px = 0.f, py = 0.f;
            int n = 0;
            for (int gi : group_sel_obj) {
                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_objects.size()) continue;
                px += edit_objects[static_cast<std::size_t>(gi)].position.x;
                py += edit_objects[static_cast<std::size_t>(gi)].position.y;
                ++n;
            }
            for (int gi : group_sel_spl) {
                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                px += edit_splines[static_cast<std::size_t>(gi)].position.x;
                py += edit_splines[static_cast<std::size_t>(gi)].position.y;
                ++n;
            }
            if (n > 0) {
                px /= static_cast<float>(n);
                py /= static_cast<float>(n);
            }
            for (int gi : group_sel_obj) {
                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_objects.size()) continue;
                const auto& o = edit_objects[static_cast<std::size_t>(gi)];
                ui::EditorSidePanelImGuiState::EditorPresetItem it{};
                it.spline = false;
                it.path = o.sco_path;
                it.dx = o.position.x - px;
                it.dy = o.position.y - py;
                it.dz = o.position.z;
                it.rot_x = o.rotation_deg.x;
                it.rot_y = o.rotation_deg.y;
                it.rot_z = o.rotation_deg.z;
                it.varparent_id = o.varparent_id;
                it.string_line_count = o.string_line_count;
                it.strings = o.strings;
                pr.items.push_back(std::move(it));
            }
            for (int gi : group_sel_spl) {
                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                const auto& s = edit_splines[static_cast<std::size_t>(gi)];
                ui::EditorSidePanelImGuiState::EditorPresetItem it{};
                it.spline = true;
                it.path = s.sli_path;
                it.dx = s.position.x - px;
                it.dy = s.position.y - py;
                it.dz = s.position.z;
                it.heading = s.heading_deg;
                it.length = s.length;
                it.radius = s.radius;
                it.start_grad = s.start_grad;
                it.end_grad = s.end_grad;
                it.use_delta_h = s.use_delta_height;
                it.delta_h = s.delta_height;
                it.skew_start = s.skew_start;
                it.skew_end = s.skew_end;
                it.cant_start = s.cant_start;
                it.cant_end = s.cant_end;
                it.mirror = s.mirror;
                pr.items.push_back(std::move(it));
            }
            if (pr.items.empty()) {
                panel.set_status("Save seleccion: grupo vacio");
            } else {
                st.presets.push_back(std::move(pr));
                ui::save_editor_docks(st);
                panel.set_status("Preset guardado: " + pname);
            }
        }
        if (ev.spawn_preset_index >= 0) {
            auto& st = panel.imgui_state();
            if (static_cast<std::size_t>(ev.spawn_preset_index) < st.presets.size() &&
                edit_mode == EditMode::None && marquee.phase == MarqueePhase::None) {
                const auto& pr = st.presets[static_cast<std::size_t>(ev.spawn_preset_index)];
                push_undo("preset_place");
                const float ax = camera.target_x;
                const float ay = camera.target_y;
                marquee = {};
                marquee.anchor_x = ax;
                marquee.anchor_y = ay;
                auto& bst = st;
                bst.busy_overlay = true;
                bst.busy_spinner = true;
                bst.busy_title = "Preset";
                bst.busy_detail = pr.name;
                bst.busy_progress = 0.f;
                pump_editor_busy();
                int done = 0;
                const int total = static_cast<int>(pr.items.size());
                for (const auto& it : pr.items) {
                    ++done;
                    bst.busy_detail = pr.name + " " + std::to_string(done) + "/" + std::to_string((std::max)(1, total));
                    bst.busy_progress = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 1.f;
                    if (it.spline) {
                        map::SplineInst s{};
                        s.sli_path = it.path;
                        s.id = next_spline_id();
                        s.position.x = ax + it.dx;
                        s.position.y = ay + it.dy;
                        s.position.z = it.dz;
                        s.heading_deg = it.heading;
                        s.length = it.length;
                        s.radius = it.radius;
                        s.start_grad = it.start_grad;
                        s.end_grad = it.end_grad;
                        s.use_delta_height = it.use_delta_h;
                        s.is_spline_h = it.use_delta_h;
                        s.delta_height = it.use_delta_h ? it.delta_h : 0.f;
                        s.skew_start = it.skew_start;
                        s.skew_end = it.skew_end;
                        s.cant_start = it.cant_start;
                        s.cant_end = it.cant_end;
                        s.mirror = it.mirror;
                        remesh_spline(s);
                        if (auto* g = find_spline_gpu(s.id)) g->pinned = true;
                        edit_splines.push_back(s);
                        upsert_transient_spline(s);
                        marquee.base_spl.push_back(s);
                        marquee.track_spl.push_back(static_cast<int>(edit_splines.size() - 1));
                        marquee.track_spl_ids.push_back(s.id);
                    } else {
                        map::MapObjectInst o{};
                        o.sco_path = it.path;
                        o.id = next_object_id();
                        o.position.x = ax + it.dx;
                        o.position.y = ay + it.dy;
                        o.position.z = it.dz;
                        o.rotation_deg.x = it.rot_x;
                        o.rotation_deg.y = it.rot_y;
                        o.rotation_deg.z = it.rot_z;
                        o.varparent_id = it.varparent_id;
                        o.string_line_count = it.string_line_count;
                        o.strings = it.strings;
                        fill_sco_map_extras(o);
                        edit_objects.push_back(o);
                        spawn_editor_object_gpu(edit_objects.back());
                        upsert_transient_object(edit_objects.back());
                        marquee.base_obj.push_back(o);
                        marquee.track_obj.push_back(static_cast<int>(edit_objects.size() - 1));
                        marquee.track_obj_ids.push_back(o.id);
                    }
                    if ((done % 2) == 0 || done == total) pump_editor_busy();
                }
                refresh_editor_lists();
                scenery_scripts.sync(edit_objects, cfg.omsi_root);
                bst.busy_overlay = false;
                bst.busy_spinner = false;
                bst.busy_progress = -1.f;
                bst.busy_title.clear();
                bst.busy_detail.clear();
                marquee.phase = MarqueePhase::Placing;
                panel.set_status("Preset '" + pr.name + "': mové el mouse, LMB=colocar, Esc=cancelar");
            }
        }
        if (ev.adapt_spline_gradient_terrain) {
            const int n = apply_spline_gradient_terrain_targets(true);
            if (n == 0) {
                panel.set_status("Adaptar gradiente: selecciona una spline");
            } else {
                panel.set_status("Gradiente adaptado al terreno (" + std::to_string(n) + " spl)");
            }
        }
        if (ev.adapt_spline_clearance) {
            const int quality = (std::clamp)(ev.spline_clearance_quality, 1, 10);
            bool env_running = false;
            {
                std::scoped_lock lock(spline_env_job.mutex);
                env_running = spline_env_job.running;
            }
            if (!env_running) {
                if (start_spline_environment_job(quality)) {
                    push_undo("adapt_spline_clearance_split");
                    panel.imgui_state().busy_overlay = true;
                    panel.imgui_state().busy_spinner = true;
                    panel.imgui_state().busy_progress = -1.f;
                    panel.imgui_state().busy_title = "Adaptando spline";
                    panel.imgui_state().busy_detail =
                        "Buscando cortes, gradientes, cant y alturas (calidad " + std::to_string(quality) + ")…";
                    panel.set_status("Calculando adaptación del entorno en segundo plano…");
                } else {
                    panel.set_status("Adaptar entorno: selecciona una spline válida");
                }
            }
        }
        main_watchdog.mark("M.L01_2002");
        if (ev.sli_bake_run) {
            std::vector<map::SplineInst> pack;
            for (int gi : group_sel_spl) {
                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                pack.push_back(edit_splines[static_cast<std::size_t>(gi)]);
            }
            if (pack.empty()) {
                panel.set_status("Bake: no hay SLI en el grupo");
            } else {
                const auto bake_root = (cfg.omsi_root.empty() ? cfg.global.root : cfg.omsi_root) /
                                       "Sceneryobjects" / "f4_bake";
                const auto out_dir = editor::unique_sli_bake_dir(
                    bake_root, editor::sli_bake_folder_name(pack.front().id));
                const auto baked =
                    editor::bake_splines_to_sco(pack, cfg.omsi_root, out_dir, out_dir.filename().string());
                if (!baked.error.empty()) {
                    panel.set_status("Bake error: " + baked.error);
                    log::warn("sli bake " + baked.error);
                } else {
                    sco::invalidate_sco_cache(baked.sco_path);
                    sli_bake_sco = baked.sco_path;
                    sli_bake_cx = baked.cx;
                    sli_bake_cy = baked.cy;
                    sli_bake_cz = baked.cz;
                    sli_bake_ready = true;
                    panel.remember_catalog_path(baked.sco_path);
                    panel.set_status("Bake OK " + baked.sco_path.string() + " (queda en OMSI, no en el mapa)");
                    log::info("sli bake sco=" + baked.sco_path.string() + " o3d=" + baked.o3d_path.string());
                }
            }
        }
        if (ev.sli_bake_replace && sli_bake_ready && !sli_bake_sco.empty()) {
            push_undo("sli_bake_replace");
            const auto del_s = group_sel_spl_ids;
            for (const auto sid : del_s) {
                mark_editor_spline_deleted(sid);
                for (const auto& s : edit_splines)
                    if (s.next_id == sid) mark_path_rails_dirty(s.id, false);
                purge_path_rails_for_owner(sid, false);
                if (auto* g = find_spline_gpu(sid)) {
                    g->alive = false;
                    for (const auto c : g->chunks) renderer.set_chunk_visible(c, false);
                }
                for (auto& mk : end_markers) {
                    if (!mk.from_object && mk.owner_id == sid) {
                        mk.alive = false;
                        renderer.set_chunk_visible(mk.chunk, false);
                    }
                }
                edit_splines.erase(std::remove_if(edit_splines.begin(), edit_splines.end(),
                                                  [&](const map::SplineInst& s) { return s.id == sid; }),
                                   edit_splines.end());
                transient_splines.erase(std::remove_if(transient_splines.begin(), transient_splines.end(),
                                                       [&](const map::SplineInst& s) { return s.id == sid; }),
                                        transient_splines.end());
            }
            group_sel_spl.clear();
            group_sel_spl_ids.clear();
            selected_spline = -1;
            map::MapObjectInst o{};
            o.id = next_object_id();
            o.sco_path = sli_bake_sco;
            o.position.x = sli_bake_cx;
            o.position.y = sli_bake_cy;
            o.position.z = sli_bake_cz;
            fill_sco_map_extras(o);
            edit_objects.push_back(o);
            spawn_editor_object_gpu(edit_objects.back());
            selected_object = static_cast<int>(edit_objects.size() - 1);
            mark_path_rails_dirty(o.id, true);
            scenery_scripts.sync(edit_objects, cfg.omsi_root);
            refresh_editor_lists();
            persist_all_loaded_tiles();
            sli_bake_pick = false;
            sli_bake_ready = false;
            panel.set_status("Reemplazado por " + sli_bake_sco.filename().string() + " id=" +
                             std::to_string(o.id));
            log::info("sli bake replace id=" + std::to_string(o.id));
        }
        // --- Tile bake (parts 1..5) ---
        {
            static std::shared_ptr<editor::TileBakeProgress> tb_prog;
            static std::future<editor::TileBakeResult> tb_fut;
            static bool tb_busy = false;
            static editor::TileBakeResult tb_last{};
            static bool tb_ready = false;

            // Resolve bake folder: Sceneryobjects/bake_tiles/<map_name>/
            auto resolve_map_bake_name = [&]() -> std::string {
                std::string n;
                if (cfg.workspace) {
                    // prefer actual map folder name
                    n = cfg.workspace->actual_dir.filename().string();
                    // strip _actual suffix if present
                    const std::string suf = "_actual";
                    if (n.size() > suf.size() && n.substr(n.size() - suf.size()) == suf)
                        n = n.substr(0, n.size() - suf.size());
                }
                if (n.empty() && !cfg.global.root.empty())
                    n = cfg.global.root.filename().string();
                if (n.empty()) n = "map";
                // sanitize
                for (char& c : n) {
                    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
                        c == '?' || c == '*')
                        c = '_';
                }
                return n;
            };
            auto resolve_tile_stem = [&]() -> std::string {
                return "tile_" + std::to_string(centerkachel.gx) + "_" + std::to_string(centerkachel.gy);
            };
            auto bake_map_dir = [&]() -> std::filesystem::path {
                return cfg.omsi_root / "Sceneryobjects" / "bake_tiles" / resolve_map_bake_name();
            };
            auto refresh_bake_flags = [&]() {
                auto _watch = main_watchdog.phase("tile_bake.refresh_flags");
                std::error_code ec;
                const auto dir = bake_map_dir();
                const auto stem = resolve_tile_stem();
                // Directory enumeration can touch slow/network disks. The UI only
                // needs eventual consistency, not a full rescan every frame.
                static std::filesystem::path cached_dir;
                static std::string cached_stem;
                static auto last_scan = std::chrono::steady_clock::time_point{};
                static bool cached_has_o3d = false;
                static bool cached_has_fbx = false;
                const auto scan_now = std::chrono::steady_clock::now();
                if (dir == cached_dir && stem == cached_stem && last_scan.time_since_epoch().count() != 0 &&
                    scan_now - last_scan < std::chrono::milliseconds(500)) {
                    panel.imgui_state().tile_bake_folder = dir;
                    panel.imgui_state().tile_bake_has_o3d = cached_has_o3d;
                    panel.imgui_state().tile_bake_has_fbx = cached_has_fbx;
                    panel.imgui_state().tile_bake_ready = cached_has_o3d && !tb_busy;
                    tb_ready = cached_has_o3d && !tb_last.parts.empty();
                    return;
                }
                cached_dir = dir;
                cached_stem = stem;
                last_scan = scan_now;
                panel.imgui_state().tile_bake_folder = dir;
                bool has_o3d = false;
                // any model/stem*.o3d or stem*.sco
                if (std::filesystem::exists(dir / "model", ec)) {
                    for (const auto& e : std::filesystem::directory_iterator(dir / "model", ec)) {
                        if (!e.is_regular_file()) continue;
                        const auto name = e.path().filename().string();
                        if (name.rfind(stem, 0) == 0 && e.path().extension() == ".o3d") {
                            has_o3d = true;
                            break;
                        }
                    }
                }
                if (!has_o3d) {
                    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                        if (!e.is_regular_file()) continue;
                        const auto name = e.path().filename().string();
                        if (name.rfind(stem, 0) == 0 && e.path().extension() == ".sco") {
                            has_o3d = true;
                            break;
                        }
                    }
                }
                bool has_fbx = false;
                const auto fbx_tile = dir / "fbx" / stem;
                if (std::filesystem::exists(fbx_tile, ec)) {
                    for (const auto& e : std::filesystem::directory_iterator(fbx_tile, ec)) {
                        if (e.is_regular_file() && e.path().extension() == ".fbx") {
                            has_fbx = true;
                            break;
                        }
                    }
                }
                cached_has_o3d = has_o3d;
                cached_has_fbx = has_fbx;
                panel.imgui_state().tile_bake_has_o3d = has_o3d;
                panel.imgui_state().tile_bake_has_fbx = has_fbx;
                panel.imgui_state().tile_bake_ready = has_o3d && !tb_busy;
                tb_ready = has_o3d && !tb_last.parts.empty();
                if (has_o3d && tb_last.parts.empty()) {
                    // Reconstruct parts list from disk so Replace works after restart
                    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                        if (!e.is_regular_file()) continue;
                        if (e.path().extension() != ".sco") continue;
                        const auto name = e.path().stem().string();
                        if (name.rfind(stem, 0) != 0) continue;
                        editor::TileBakePartResult pr;
                        pr.sco_path = e.path();
                        pr.o3d_path = dir / "model" / (name + ".o3d");
                        // default center = tile center
                        const auto& layout = world.tile_layout();
                        const auto rect = map::grid_tile_rect_at(cfg.global, layout, centerkachel.gx, centerkachel.gy);
                        // Same convention as bake: SCO sits at focus-tile editor origin
                        {
                            const auto ed0 = tile_rel(centerkachel.gx, centerkachel.gy);
                            pr.cx = ed0.first;
                            pr.cy = ed0.second;
                            pr.cz = 0.f;
                        }
                        tb_last.parts.push_back(pr);
                        tb_last.ok = true;
                        tb_last.out_dir = dir;
                    }
                    tb_ready = !tb_last.parts.empty();
                    panel.imgui_state().tile_bake_ready = tb_ready;
                }
            };
            refresh_bake_flags();

            if (tb_busy && tb_prog && tb_prog->finished.load()) {
                tb_busy = false;
                try {
                    tb_last = tb_fut.get();
                    tb_ready = tb_last.ok;
                    panel.imgui_state().tile_bake_busy = false;
                    panel.imgui_state().busy_overlay = false;
                    panel.imgui_state().busy_spinner = false;
                    panel.imgui_state().busy_progress = -1.f;
                    if (tb_last.ok) {
                        panel.set_status("Tile bake OK parts=" + std::to_string(tb_last.parts.size()) +
                                         " → " + tb_last.out_dir.string());
                        log::info("tile bake finished ok parts=" + std::to_string(tb_last.parts.size()));
                    } else {
                        panel.set_status("Tile bake error: " + tb_last.error);
                        log::warn("tile bake fail: " + tb_last.error);
                    }
                } catch (const std::exception& ex) {
                    tb_ready = false;
                    panel.imgui_state().tile_bake_busy = false;
                    panel.imgui_state().busy_overlay = false;
                    panel.set_status(std::string("Tile bake exception: ") + ex.what());
                }
                refresh_bake_flags();
            } else if (tb_busy && tb_prog) {
                const int pct = tb_prog->percent.load();
                panel.set_status("Tile bake " + std::to_string(pct) + "% (" +
                                std::to_string(tb_prog->parts_done.load()) + "/" +
                                std::to_string(tb_prog->parts_total.load()) + ")");
                panel.imgui_state().tile_bake_busy = true;
                panel.imgui_state().busy_overlay = true;
                panel.imgui_state().busy_spinner = true;
                panel.imgui_state().busy_progress = pct / 100.f;
                panel.imgui_state().busy_title = "Tile bake";
                panel.imgui_state().busy_detail = std::to_string(pct) + "%";
            }

            if (ev.tile_bake_run) {
                if (tb_busy) {
                    panel.set_status("Tile bake ya en curso");
                } else if (panel.imgui_state().tile_bake_has_o3d) {
                    panel.set_status("Ya existe bake O3D — usá Clean bake .o3d primero");
                } else {
                    // Always re-sync focus tile from camera so bake targets the tile you see
                    sync_centerkachel_from_camera();
                    const int parts = (std::max)(1, (std::min)(5, ev.tile_bake_parts));
                    const auto& layout = world.tile_layout();
                    const auto rect =
                        map::grid_tile_rect_at(cfg.global, layout, centerkachel.gx, centerkachel.gy);
                    editor::TileBakeRequest req;
                    req.tile_x = centerkachel.gx;
                    req.tile_y = centerkachel.gy;
                    req.parts = parts;
                    // Editor-space origin of THIS kachel (session origin may be another tile)
                    const auto ed0 = tile_rel(centerkachel.gx, centerkachel.gy);
                    req.tile_origin_x = ed0.first;
                    req.tile_origin_y = ed0.second;
                    req.tile_size = rect.width_m > 1.f ? rect.width_m : 300.f;
                    req.omsi_root = cfg.omsi_root;
                    req.map_dir = cfg.workspace ? cfg.workspace->actual_dir : cfg.global.root;
                    req.out_parent = bake_map_dir();
                    req.stem = resolve_tile_stem();
                    // Focus-tile AABB in editor space (not always 0..size when session origin ≠ focus)
                    const float x0 = ed0.first, y0 = ed0.second;
                    const float x1 = x0 + req.tile_size, y1 = y0 + req.tile_size;
                    auto in_focus_tile = [&](float x, float y) {
                        return x >= x0 && x < x1 && y >= y0 && y < y1;
                    };
                    // Spline counts if start OR end is inside the focus tile (long roads)
                    auto spline_in_focus = [&](const map::SplineInst& s) {
                        if (in_focus_tile(s.position.x, s.position.y)) return true;
                        const float rad = s.heading_deg * 3.14159265f / 180.f;
                        const float ex = s.position.x + std::cos(rad) * s.length;
                        const float ey = s.position.y + std::sin(rad) * s.length;
                        if (in_focus_tile(ex, ey)) return true;
                        // mid sample
                        const float mx = s.position.x + std::cos(rad) * s.length * 0.5f;
                        const float my = s.position.y + std::sin(rad) * s.length * 0.5f;
                        return in_focus_tile(mx, my);
                    };
                    for (const auto& o : edit_objects) {
                        if (in_focus_tile(o.position.x, o.position.y))
                            req.objects.push_back(o);
                    }
                    for (const auto& s : edit_splines) {
                        if (spline_in_focus(s))
                            req.splines.push_back(s);
                    }
                    for (const auto& sa : edit_spline_attachments) {
                        req.spline_attaches.push_back(sa.att);
                    }
                    for (const auto& a : edit_attachments) {
                        req.attaches.push_back(a);
                    }
                    // Terrain heightfield for non-flat tiles (MergeMapTrees: z + terrain unless absheight)
                    {
                        std::filesystem::path map_path;
                        for (const auto& t : world.tiles()) {
                            if (t.grid_x == centerkachel.gx && t.grid_y == centerkachel.gy) {
                                map_path = t.path; // MapTileData::path
                                break;
                            }
                        }
                        if (!map_path.empty()) {
                            if (auto h = editor::load_terrain_heights(map_path)) {
                                req.terrain_heights = std::move(*h);
                                log::info("tile bake terrain heights n=" +
                                          std::to_string(req.terrain_heights.size()));
                            }
                        }
                    }
                    log::info("tile bake collect focus=(" + std::to_string(req.tile_x) + "," +
                              std::to_string(req.tile_y) + ") stem=" + req.stem +
                              " origin=(" + std::to_string(req.tile_origin_x) + "," +
                              std::to_string(req.tile_origin_y) + ") objs=" +
                              std::to_string(req.objects.size()) +
                              " splines=" + std::to_string(req.splines.size()) +
                              " size=" + std::to_string(req.tile_size) +
                              " terrain=" + std::to_string(req.terrain_heights.size()) +
                              " attachObj=" + std::to_string(req.attaches.size()));
                    tb_prog = std::make_shared<editor::TileBakeProgress>();
                    tb_busy = true;
                    panel.imgui_state().tile_bake_busy = true;
                    panel.imgui_state().tile_bake_ready = false;
                    panel.imgui_state().busy_overlay = true;
                    panel.imgui_state().busy_spinner = true;
                    panel.imgui_state().busy_progress = 0.f;
                    panel.imgui_state().busy_title = "Tile bake O3D";
                    panel.imgui_state().busy_detail = "Partes=" + std::to_string(parts);
                    panel.set_status("Tile bake partes=" + std::to_string(parts) + " (background)…");
                    log::info("tile bake started parts=" + std::to_string(parts) +
                              " dir=" + req.out_parent.string());
                    tb_fut = std::async(std::launch::async, [req, prog = tb_prog]() {
                        return editor::bake_tile(req, prog.get());
                    });
                }
            }
            if (ev.tile_bake_fbx) {
                // Placeholder: mark fbx folder; full FBX writer can follow
                std::error_code ec;
                const auto fbx_dir = bake_map_dir() / "fbx" / resolve_tile_stem();
                std::filesystem::create_directories(fbx_dir, ec);
                // If we have o3d parts, copy a stub note; real FBX export TBD
                const auto note = fbx_dir / "README_fbx.txt";
                {
                    std::ofstream ofs(note);
                    if (ofs) {
                        ofs << "FBX export folder for tile " << resolve_tile_stem() << "\n";
                        ofs << "Place exported .fbx here (or enable full FBX writer).\n";
                        for (const auto& p : tb_last.parts) {
                            if (!p.o3d_path.empty()) ofs << "source o3d: " << p.o3d_path.string() << "\n";
                        }
                    }
                }
                // Create empty .fbx marker so has_fbx becomes true only if real file —
                // User asked gray when bake fbx exists. Write a minimal placeholder only if o3d ready.
                if (panel.imgui_state().tile_bake_has_o3d || !tb_last.parts.empty()) {
                    const auto fbx_path = fbx_dir / (resolve_tile_stem() + ".fbx");
                    std::ofstream ofs(fbx_path, std::ios::binary);
                    if (ofs) {
                        // minimal ASCII marker (not a valid FBX — will be replaced by real exporter)
                        ofs << "; OMSI F4 tile bake FBX placeholder — re-export with full writer\n";
                    }
                    panel.set_status("FBX folder ready: " + fbx_dir.string() + " (placeholder)");
                    log::info("tile bake fbx folder=" + fbx_dir.string());
                } else {
                    panel.set_status("Bake O3D primero (o export FBX directo TBD)");
                }
                refresh_bake_flags();
            }
            if (ev.tile_bake_replace) {
                refresh_bake_flags();
                if (!panel.imgui_state().tile_bake_has_o3d || tb_last.parts.empty()) {
                    panel.set_status("Tile bake replace: no hay bake O3D listo");
                } else {
                    push_undo("tile_bake_replace");
                    sync_centerkachel_from_camera();
                    const auto& layout = world.tile_layout();
                    const auto rect =
                        map::grid_tile_rect_at(cfg.global, layout, centerkachel.gx, centerkachel.gy);
                    const auto ed0 = tile_rel(centerkachel.gx, centerkachel.gy);
                    const float x0 = ed0.first, y0 = ed0.second;
                    const float ts = rect.width_m > 1.f ? rect.width_m : 300.f;
                    const float x1 = x0 + ts, y1 = y0 + ts;
                    auto in_tile_xy = [&](float x, float y) {
                        return x >= x0 && x < x1 && y >= y0 && y < y1;
                    };
                    log::info("tile bake replace focus=(" + std::to_string(centerkachel.gx) + "," +
                              std::to_string(centerkachel.gy) + ") editorAABB=(" +
                              std::to_string(x0) + "," + std::to_string(y0) + ")-(" +
                              std::to_string(x1) + "," + std::to_string(y1) + ") stem=" +
                              resolve_tile_stem());
                    auto is_bake_product = [](const std::filesystem::path& p) {
                        const auto s = p.string();
                        return s.find("bake_tiles") != std::string::npos ||
                               editor::is_tile_bake_product_name(p.filename().string());
                    };
                    auto lower_name = [](const std::filesystem::path& p) {
                        std::string s = p.filename().string();
                        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        return s;
                    };
                    // Keep entrypoint / carpark / busstop / trafficlight / texttexture(+script) as free objects
                    auto is_keep_helper_sco = [&](const map::MapObjectInst& o) -> bool {
                        if (is_bake_product(o.sco_path)) return true;
                        const auto low = lower_name(o.sco_path);
                        if (low.find("entrypoint") != std::string::npos) return true;
                        if (low.find("carpark") != std::string::npos) return true;
                        if (low.find("busstop") != std::string::npos || low.find("bus_stop") != std::string::npos)
                            return true;
                        if (const auto* sco = sco::load_sco(o.sco_path)) {
                            using S = sco::ScoSpecial;
                            if (sco->special == S::TrafficLight || sco->special == S::HelpArrow) return true;
                            if (!sco->text_textures.empty()) return true;
                            if (!sco->script_files.empty() && !sco->text_textures.empty()) return true;
                        }
                        return false;
                    };

                    // Collect keep object ids in this tile
                    std::unordered_set<std::int32_t> keep_obj_ids;
                    for (const auto& o : edit_objects) {
                        if (!in_tile_xy(o.position.x, o.position.y)) continue;
                        if (is_keep_helper_sco(o) && !is_bake_product(o.sco_path))
                            keep_obj_ids.insert(o.id);
                    }

                    // Splines used by keep objects via [splineAttachement]
                    std::unordered_set<std::int32_t> keep_spline_ids;
                    for (const auto& entry : edit_spline_attachments) {
                        if (keep_obj_ids.count(entry.att.instance_id)) {
                            if (entry.parent_spline_id > 0) keep_spline_ids.insert(entry.parent_spline_id);
                        }
                        // also if attachment sco itself is entrypoint/carpark
                        const auto low = lower_name(entry.att.sco_path);
                        if (low.find("entrypoint") != std::string::npos ||
                            low.find("carpark") != std::string::npos) {
                            if (entry.parent_spline_id > 0) keep_spline_ids.insert(entry.parent_spline_id);
                            keep_obj_ids.insert(entry.att.instance_id);
                        }
                    }

                    // Path-only / invisible / onlyeditor SLIs must stay on the map (no mesh in o3d).
                    // e.g. invis_street.sli id=99, invis_sidewalk.sli id=8787
                    // Keep only Helper_Blue-style helpers on the map after bake.
                    // [onlyeditor] / invis_* paths are written into the tile SCO; do NOT keep those SLIs.
                    auto is_path_only_sli = [&](const map::SplineInst& s) -> bool {
                        const auto low = lower_name(s.sli_path);
                        if (low.find("helper_blue") != std::string::npos) return true;
                        // true helper markers only (not invis path carriers)
                        if (low.find("helper_") != std::string::npos && low.find("invis") == std::string::npos)
                            return true;
                        return false;
                    };
                    for (const auto& s : edit_splines) {
                        const float rad = s.heading_deg * 3.14159265f / 180.f;
                        const float ex = s.position.x + std::cos(rad) * s.length;
                        const float ey = s.position.y + std::sin(rad) * s.length;
                        const bool in_focus = in_tile_xy(s.position.x, s.position.y) ||
                                             in_tile_xy(ex, ey) ||
                                             in_tile_xy(0.5f * (s.position.x + ex), 0.5f * (s.position.y + ey));
                        if (in_focus && is_path_only_sli(s))
                            keep_spline_ids.insert(s.id);
                    }
                    log::info("tile bake keep path-only/invis splines=" +
                              std::to_string(keep_spline_ids.size()));

                    // Helper Blue path
                    std::filesystem::path helper_blue;
                    {
                        const std::filesystem::path candidates[] = {
                            cfg.omsi_root / "Sceneryobjects" / "UKDT" / "Westcountry3" / "_Helpers" /
                                "Helper_Blue.sli",
                            cfg.omsi_root / "Sceneryobjects" / "Generic" / "Helper_Blue.sli",
                            cfg.omsi_root / "Splines" / "Helper_Blue.sli",
                            cfg.omsi_root / "Splines" / "helpers" / "Helper_Blue.sli",
                        };
                        std::error_code hec;
                        for (const auto& c : candidates) {
                            if (std::filesystem::exists(c, hec)) {
                                helper_blue = c;
                                break;
                            }
                        }
                        // Fall back to any already-loaded Helper_Blue path from edit_splines
                        if (helper_blue.empty()) {
                            for (const auto& s : edit_splines) {
                                const auto low = lower_name(s.sli_path);
                                if (low.find("helper_blue") != std::string::npos) {
                                    helper_blue = s.sli_path;
                                    break;
                                }
                            }
                        }
                    }
                    const bool helper_ok = !helper_blue.empty();
                    if (!helper_ok)
                        log::warn("Helper_Blue.sli no encontrado (keep-splines no convertirán a helper)");
                    else
                        log::info("Helper_Blue path=" + helper_blue.string());

                    // Convert keep splines → Helper_Blue (same pose/len/heading), drop other tile splines
                    std::vector<map::SplineInst> kept_splines;
                    for (auto& s : edit_splines) {
                        if (!in_tile_xy(s.position.x, s.position.y)) {
                            kept_splines.push_back(s);
                            continue;
                        }
                        if (keep_spline_ids.count(s.id)) {
                            // Path-only / invis keep their own .sli; only convert generic
                            // "host" keeps (e.g. entrypoint attachment parents) to Helper_Blue.
                            if (helper_ok && !is_path_only_sli(s)) {
                                s.sli_path = helper_blue;
                                remesh_spline(s);
                                upsert_transient_spline(s);
                                log::info("tile bake: spline id=" + std::to_string(s.id) +
                                          " → Helper_Blue.sli");
                            } else {
                                log::info("tile bake: keep spline id=" + std::to_string(s.id) + " " +
                                          s.sli_path.filename().string());
                            }
                            kept_splines.push_back(s);
                        }
                        // else drop (geometry was baked)
                    }
                    // Hide GPU for dropped splines (baked into O3D) so visuals update without reload
                    {
                        std::unordered_set<std::int32_t> keep_ids;
                        for (const auto& s : kept_splines) keep_ids.insert(s.id);
                        int hidden = 0;
                        for (const auto& s : edit_splines) {
                            if (keep_ids.count(s.id)) continue;
                            if (auto* g = find_spline_gpu(s.id)) {
                                g->alive = false;
                                for (const auto c : g->chunks) renderer.set_chunk_visible(c, false);
                                ++hidden;
                            }
                        }
                        log::info("tile bake replace hide gpu splines=" + std::to_string(hidden));
                    }
                    edit_splines = std::move(kept_splines);

                    // Rebuild local spline_index map: id → index among remaining (for this tile + global list)
                    // OMSI spline_index is local to tile .map; approximate with order of edit_splines that
                    // share the same tile focus for attachments we keep.
                    std::unordered_map<std::int32_t, std::int32_t> spline_id_to_index;
                    for (std::int32_t i = 0; i < static_cast<std::int32_t>(edit_splines.size()); ++i)
                        spline_id_to_index[edit_splines[static_cast<std::size_t>(i)].id] = i;

                    // Attachments: keep only entrypoint/carpark; update spline_index
                    {
                        auto kept_sa = edit_spline_attachments;
                        kept_sa.clear();
                        kept_sa.reserve(edit_spline_attachments.size());
                        for (auto& entry : edit_spline_attachments) {
                            const bool keep_inst = keep_obj_ids.count(entry.att.instance_id) != 0;
                            const auto low = lower_name(entry.att.sco_path);
                            const bool keep_by_name = low.find("entrypoint") != std::string::npos ||
                                                     low.find("carpark") != std::string::npos ||
                                                     low.find("busstop") != std::string::npos ||
                                                     low.find("bus_stop") != std::string::npos;
                            if (!keep_inst && !keep_by_name) continue;
                            if (entry.parent_spline_id > 0) {
                                auto it = spline_id_to_index.find(entry.parent_spline_id);
                                if (it != spline_id_to_index.end())
                                    entry.att.spline_index = it->second;
                            }
                            kept_sa.push_back(std::move(entry));
                        }
                        edit_spline_attachments = std::move(kept_sa);
                        // re-sync id maps if present
                        spline_attach_obj_to_entry.clear();
                        for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
                            const auto id = edit_spline_attachments[ei].att.instance_id;
                            if (id > 0) spline_attach_obj_to_entry[id] = ei;
                        }
                    }

                    // [attachObj]: keep only entrypoint/carpark/busstop/trafficlight/texttexture children;
                    // rest were baked into tile mesh — hide GPU + drop from list
                    {
                        std::vector<map::MapAttachObjectInst> kept_att;
                        kept_att.reserve(edit_attachments.size());
                        std::unordered_set<std::int32_t> doomed_children;
                        for (const auto& a : edit_attachments) {
                            const auto low = lower_name(a.sco_path);
                            bool keep = low.find("entrypoint") != std::string::npos ||
                                        low.find("carpark") != std::string::npos ||
                                        low.find("busstop") != std::string::npos ||
                                        low.find("bus_stop") != std::string::npos;
                            if (!keep) {
                                if (const auto* sco = sco::load_sco(a.sco_path)) {
                                    using S = sco::ScoSpecial;
                                    if (sco->special == S::TrafficLight || sco->special == S::HelpArrow)
                                        keep = true;
                                    if (!sco->text_textures.empty()) keep = true;
                                }
                            }
                            if (keep) {
                                kept_att.push_back(a);
                            } else {
                                doomed_children.insert(a.child_id);
                            }
                        }
                        for (auto& sp : scene_places) {
                            if (!sp.alive || !doomed_children.count(sp.object_id)) continue;
                            sp.alive = false;
                            renderer.set_chunk_visible(sp.chunk, false);
                        }
                        log::info("tile bake replace attachObj keep=" + std::to_string(kept_att.size()) +
                                  " drop=" + std::to_string(doomed_children.size()));
                        edit_attachments = std::move(kept_att);
                    }

                    // Objects: keep entrypoint/carpark + bake products; remove rest in tile
                    // Hide GPU chunks first so visual matches .map without full reload.
                    {
                        std::unordered_set<std::int32_t> doomed_obj;
                        for (const auto& o : edit_objects) {
                            if (!in_tile_xy(o.position.x, o.position.y)) continue;
                            if (is_bake_product(o.sco_path)) continue;
                            if (keep_obj_ids.count(o.id)) continue;
                            doomed_obj.insert(o.id);
                        }
                        for (auto& sp : scene_places) {
                            if (!sp.alive || !doomed_obj.count(sp.object_id)) continue;
                            sp.alive = false;
                            renderer.set_chunk_visible(sp.chunk, false);
                        }
                        log::info("tile bake replace hide gpu objs=" + std::to_string(doomed_obj.size()));
                    }
                    edit_objects.erase(
                        std::remove_if(edit_objects.begin(), edit_objects.end(),
                                       [&](const map::MapObjectInst& o) {
                                           if (!in_tile_xy(o.position.x, o.position.y)) return false;
                                           if (is_bake_product(o.sco_path)) return false;
                                           if (keep_obj_ids.count(o.id)) return false;
                                           return true;
                                       }),
                        edit_objects.end());

                    // Place bake SCO parts
                    for (const auto& p : tb_last.parts) {
                        if (p.sco_path.empty()) continue;
                        bool already = false;
                        for (const auto& o : edit_objects) {
                            if (o.sco_path == p.sco_path) {
                                already = true;
                                break;
                            }
                        }
                        if (already) continue;
                        map::MapObjectInst o{};
                        o.id = next_object_id();
                        o.sco_path = p.sco_path;
                        o.position.x = p.cx;
                        o.position.y = p.cy;
                        o.position.z = p.cz;
                        log::info("tile bake place SCO id=" + std::to_string(o.id) + " at=(" +
                                  std::to_string(o.position.x) + "," + std::to_string(o.position.y) +
                                  "," + std::to_string(o.position.z) + ") " +
                                  p.sco_path.filename().string() + " focus=(" +
                                  std::to_string(centerkachel.gx) + "," + std::to_string(centerkachel.gy) +
                                  ")");
                        fill_sco_map_extras(o);
                        edit_objects.push_back(o);
                        spawn_editor_object_gpu(edit_objects.back());
                        mark_path_rails_dirty(o.id, true);
                    }
                    scenery_scripts.sync(edit_objects, cfg.omsi_root);
                    refresh_editor_lists();
                    persist_all_loaded_tiles();
                    panel.set_status("Replace: bake +" + std::to_string(tb_last.parts.size()) +
                                     " SCO; helpers EP/carpark; Helper_Blue x" +
                                     std::to_string(keep_spline_ids.size()));
                    log::info("tile bake replace parts=" + std::to_string(tb_last.parts.size()) +
                              " keep_splines=" + std::to_string(keep_spline_ids.size()) +
                              " keep_objs=" + std::to_string(keep_obj_ids.size()));
                }
            }
            if (ev.tile_bake_clean_o3d) {
                std::error_code ec;
                const auto dir = bake_map_dir();
                const auto stem = resolve_tile_stem();
                int n = 0;
                // remove matching sco at root
                for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                    if (!e.is_regular_file()) continue;
                    const auto name = e.path().stem().string();
                    if (name.rfind(stem, 0) == 0 && e.path().extension() == ".sco") {
                        std::filesystem::remove(e.path(), ec);
                        ++n;
                    }
                }
                if (std::filesystem::exists(dir / "model", ec)) {
                    for (const auto& e : std::filesystem::directory_iterator(dir / "model", ec)) {
                        if (!e.is_regular_file()) continue;
                        const auto name = e.path().stem().string();
                        if (name.rfind(stem, 0) == 0) {
                            std::filesystem::remove(e.path(), ec);
                            ++n;
                        }
                    }
                }
                tb_last = {};
                tb_ready = false;
                panel.set_status("Clean O3D: borrados " + std::to_string(n) + " archivos de " + stem);
                log::info("tile bake clean o3d stem=" + stem + " n=" + std::to_string(n));
                refresh_bake_flags();
            }
            if (ev.tile_bake_clean_fbx) {
                std::error_code ec;
                const auto fbx_tile = bake_map_dir() / "fbx" / resolve_tile_stem();
                const auto removed = std::filesystem::remove_all(fbx_tile, ec);
                panel.set_status("Clean FBX: " + fbx_tile.string() + " (" + std::to_string(removed) + ")");
                log::info("tile bake clean fbx " + fbx_tile.string());
                refresh_bake_flags();
            }
            if (ev.tile_bake_open_folder) {
                const auto dir = bake_map_dir();
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                // ShellExecute open folder
                ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                panel.set_status("Open: " + dir.string());
            }
        }

        if (ev.create_lightmap) {
            sync_centerkachel_from_camera();
            const auto active = centerkachel_as_active();
            const auto map_path = find_tile_path(active.grid_x, active.grid_y);
            auto heights = editor::load_terrain_heights(map_path);
            if (!heights) heights = editor::make_flat_terrain_heights(0.f);
            if (!map_path.empty() && map::refresh_tile_lightmap(map_path, cfg.omsi_root))
                panel.set_status("Lightmap: " + map_path.filename().string() + ".LM.bmp");
            else
                panel.set_status("Lightmap failed");
        }
        main_watchdog.mark("M.L01_2750");
        if (ev.do_whole_map) {
            const int ans =
                MessageBoxW(glfw_hwnd,
                            L"Do with whole map?\n\nYes = lightmaps + validate attach + zero-length splines\n"
                            L"No = cancel\n\n(Use Aerial bulk buttons for satellite batch)",
                            L"Do with whole map", MB_YESNO | MB_ICONQUESTION);
            if (ans == IDYES) {
                map::WholeMapJobOptions opt{};
                opt.create_lightmaps = true;
                opt.validate_attach = true;
                opt.find_zero_splines = true;
                map::WholeMapJobResult r;
                {
                    auto _watch = main_watchdog.phase("whole_map.jobs");
                    r = map::run_whole_map_jobs(cfg.global, opt, nullptr);
                }
                panel.set_status("Whole map: " + r.message);
            }
        }
        if (ev.aerial_missing || ev.aerial_all) {
            auto _watch = main_watchdog.phase("aerial.bulk_download");
            geo_settings = ev.tile_geo_edit;
            geo_settings.tile_aerial_visible = ev.tile_aerial_visible;
            geo_settings.aerial_resolution_pos = ev.aerial_resolution_pos;
            map::save_editor_geo_settings(cfg.global.root, geo_settings);
            int n = 0;
            int skip = 0;
            const auto& layout = world.tile_layout();
            const int out_px = map::aerial_resolution_label_px(geo_settings.aerial_resolution_pos);
            std::string api_key = geo_settings.bing_api_key;
            if (!geo_settings.google_api_key.empty()) api_key = geo_settings.google_api_key;
            if (!geo_settings.maptiler_api_key.empty() &&
                geo_settings.satellite_provider == map::SatelliteProvider::MapTiler)
                api_key = geo_settings.maptiler_api_key;
            const std::string custom_url = geo_settings.custom_aerial_url;
            for (const auto& ref : cfg.global.tiles) {
                const auto out_jpg =
                    map::tile_aerial_jpg_path(cfg.omsi_root, cfg.global.root, ref.tile_path);
                if (ev.aerial_missing && std::filesystem::exists(out_jpg)) {
                    ++skip;
                    continue;
                }
                const auto bounds_opt = map::tile_wgs84_bounds(cfg.global, layout, ref.grid_x, ref.grid_y,
                                                               geo_settings.ref_lat, geo_settings.ref_lon);
                if (!bounds_opt) continue;
                const float size_m = tile_size_at(ref.grid_x, ref.grid_y);
                auto r = map::download_tile_aerial_satellite(geo_settings.satellite_provider, *bounds_opt, size_m,
                                                            out_px, out_jpg, api_key, custom_url);
                if (r.ok) ++n;
            }
            panel.set_status(std::string(ev.aerial_all ? "Aerial all: " : "Aerial missing: ") + std::to_string(n) +
                             " ok, skipped " + std::to_string(skip));
        }
        if (ev.chrono_plus || ev.chrono_minus || ev.chrono_remove) {
            const int csel = ev.chrono_sel >= 0 ? ev.chrono_sel : panel.chrono_sel();
            std::string name;
            {
                const auto chrono_dir = cfg.global.root / "Chrono";
                std::vector<std::string> chronos;
                if (std::filesystem::exists(chrono_dir)) {
                    for (const auto& e : std::filesystem::directory_iterator(chrono_dir))
                        if (e.is_directory()) chronos.push_back(e.path().filename().string());
                }
                std::sort(chronos.begin(), chronos.end());
                if (csel >= 0 && csel < static_cast<int>(chronos.size())) name = chronos[static_cast<std::size_t>(csel)];
            }
            auto tag = [&](const std::string& n) { return std::string("chrono:") + n; };
            auto apply_chrono = [&](std::vector<std::string>& strings) {
                const std::string t = tag(name);
                if (ev.chrono_remove || ev.chrono_minus) {
                    strings.erase(std::remove(strings.begin(), strings.end(), t), strings.end());
                }
                if (ev.chrono_plus && !name.empty()) {
                    if (std::find(strings.begin(), strings.end(), t) == strings.end()) strings.push_back(t);
                }
            };
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                apply_chrono(edit_objects[static_cast<std::size_t>(selected_object)].strings);
                persist_editor_xy(edit_objects[static_cast<std::size_t>(selected_object)].position.x,
                                  edit_objects[static_cast<std::size_t>(selected_object)].position.y);
                panel.set_status("Chrono updated on object");
            } else if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                // Splines: store as a path-rule-like keyword in a synthetic string via label on first rule — use flags note.
                panel.set_status("Chrono: select an object (spline chrono tags not in .map yet)");
            } else {
                panel.set_status("Chrono: select object/spline first");
            }
        }
        if (ev.ground_create_layer) {
            OPENFILENAMEW ofn{};
            wchar_t file[MAX_PATH]{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = glfw_hwnd;
            ofn.lpstrFilter = L"Textures (*.bmp;*.dds;*.png)\0*.bmp;*.dds;*.png\0All\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                std::filesystem::path abs = file;
                std::filesystem::path rel = abs;
                if (!cfg.omsi_root.empty()) {
                    const auto root = cfg.omsi_root.lexically_normal();
                    const auto p = abs.lexically_normal();
                    auto rel_opt = std::filesystem::relative(p, root);
                    if (!rel_opt.empty() && rel_opt.native().find(L"..") == std::wstring::npos) rel = rel_opt;
                }
                if (cfg.workspace) {
                    if (!map::workspace_prepare_global_cfg_for_write(*cfg.workspace)) {
                        panel.set_status("Ground layer: no se pudo crear overlay global.cfg");
                    } else {
                        cfg.global.global_cfg_path = cfg.workspace->actual_global_cfg;
                    }
                }
                if (std::filesystem::exists(cfg.global.global_cfg_path) &&
                    map::append_global_groundtex(cfg.global.global_cfg_path, rel, 60.f)) {
                    if (auto g = map::load_global_map(cfg.global.global_cfg_path))
                        cfg.global.ground_textures = g->ground_textures;
                    refresh_ground_layer_ui();
                    panel.set_status("Ground layer added: " + rel.string());
                }
            }
        }
        if (ev.ground_apply) {
            if (cfg.workspace) {
                if (map::workspace_prepare_global_cfg_for_write(*cfg.workspace))
                    cfg.global.global_cfg_path = cfg.workspace->actual_global_cfg;
            }
            const int flag = (ev.ground_apply_index >= 0 &&
                              ev.ground_apply_index < static_cast<int>(cfg.global.ground_textures.size()))
                                 ? cfg.global.ground_textures[static_cast<std::size_t>(ev.ground_apply_index)].flag
                                 : 0;
            if (map::write_global_groundtex(cfg.global.global_cfg_path, ev.ground_apply_index, ev.ground_apply_main,
                                            ev.ground_apply_detail, flag, ev.ground_apply_main_rep,
                                            ev.ground_apply_det_rep)) {
                if (auto g = map::load_global_map(cfg.global.global_cfg_path))
                    cfg.global.ground_textures = g->ground_textures;
                refresh_ground_layer_ui();
                if (!cfg.global.ground_textures.empty()) groundtex = cfg.global.ground_textures.front();
                panel.set_status(ev.ground_apply_index >= 0 ? "Capa de suelo actualizada" : "Capa de suelo creada");
            } else {
                panel.set_status("Capa de suelo: no se pudo escribir global.cfg");
            }
        }
        if (ev.ground_delete_layer) {
            const int sel = ev.ground_layer_sel >= 0 ? ev.ground_layer_sel : panel.ground_layer_sel();
            if (cfg.workspace) {
                if (map::workspace_prepare_global_cfg_for_write(*cfg.workspace))
                    cfg.global.global_cfg_path = cfg.workspace->actual_global_cfg;
            }
            if (map::delete_global_groundtex_at(cfg.global.global_cfg_path, sel)) {
                if (auto g = map::load_global_map(cfg.global.global_cfg_path))
                    cfg.global.ground_textures = g->ground_textures;
                refresh_ground_layer_ui();
                panel.set_status("Ground layer deleted");
            }
        }
        if (ev.ground_edit_layer) {
            const int sel = ev.ground_layer_sel >= 0 ? ev.ground_layer_sel : panel.ground_layer_sel();
            OPENFILENAMEW ofn{};
            wchar_t file[MAX_PATH]{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = glfw_hwnd;
            ofn.lpstrFilter = L"Textures (*.bmp;*.dds;*.png)\0*.bmp;*.dds;*.png\0All\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn) && sel == 0) {
                std::filesystem::path abs = file;
                std::filesystem::path rel = abs;
                if (!cfg.omsi_root.empty()) {
                    auto rel_opt = std::filesystem::relative(abs.lexically_normal(), cfg.omsi_root.lexically_normal());
                    if (!rel_opt.empty() && rel_opt.native().find(L"..") == std::wstring::npos) rel = rel_opt;
                }
                if (cfg.workspace) {
                    if (map::workspace_prepare_global_cfg_for_write(*cfg.workspace))
                        cfg.global.global_cfg_path = cfg.workspace->actual_global_cfg;
                }
                if (map::update_global_groundtex(cfg.global.global_cfg_path, rel, 60.f)) {
                    if (auto g = map::load_global_map(cfg.global.global_cfg_path))
                        cfg.global.ground_textures = g->ground_textures;
                    refresh_ground_layer_ui();
                    if (!cfg.global.ground_textures.empty()) groundtex = cfg.global.ground_textures.front();
                    panel.set_status("Edited groundtex[0]=" + rel.string());
                }
            } else if (sel != 0) {
                panel.set_status("Edit Act. Layer: currently rewrites [groundtex] #0 only — delete+create for others");
            }
        }

        if (ev.delete_tile_aerial) {
            sync_centerkachel_from_camera();
            const auto active = centerkachel_as_active();
            const auto map_path = find_tile_path(active.grid_x, active.grid_y);
            if (map_path.empty()) {
                panel.set_status("Aerial: tile actual no cargado");
            } else {
                const auto existing =
                    map::find_tile_aerial_jpg(cfg.omsi_root, cfg.global.root, map_path);
                const auto bmp_path = cfg.global.root / "texture" /
                                      ("aerial_" + std::to_string(active.grid_x) + "_" +
                                       std::to_string(active.grid_y) + ".bmp");
                const bool had_bmp = std::filesystem::exists(bmp_path);
                if (!existing && !had_bmp) {
                    panel.set_status("Aerial: no hay imagen en este tile");
                } else {
                    map::remove_tile_aerial_files(cfg.omsi_root, cfg.global.root, map_path);
                    if (had_bmp) {
                        std::error_code ec;
                        std::filesystem::remove(bmp_path, ec);
                    }
                    invalidate_aerial_thumb(active.grid_x, active.grid_y);
                    reload_terrain_gpu_tile(active.grid_x, active.grid_y);
                    panel.set_status("Aerial borrado tile (" + std::to_string(active.grid_x) + "," +
                                     std::to_string(active.grid_y) + ")");
                    log::info("editor aerial deleted tile (" + std::to_string(active.grid_x) + "," +
                              std::to_string(active.grid_y) + ")" +
                              (existing ? " jpg=" + existing->string() : "") +
                              (had_bmp ? " bmp=" + bmp_path.string() : ""));
                }
            }
        }
        main_watchdog.mark("M.L01_2971");
        if (ev.download_tile_aerial) {
            if (aerial_job && !aerial_job->finished.load()) {
                panel.set_status("Aerial: descargando en segundo plano...");
            } else {
                try {
                    if (aerial_job && aerial_job->thread.joinable()) aerial_job->thread.join();
                    aerial_job.reset();

                    sync_centerkachel_from_camera();
                    const auto active = centerkachel_as_active();
                    const auto& layout = world.tile_layout();
                    const auto bounds_opt = map::tile_wgs84_bounds(cfg.global, layout, active.grid_x, active.grid_y,
                                                                   geo_settings.ref_lat, geo_settings.ref_lon);
                    const auto map_path = find_tile_path(active.grid_x, active.grid_y);
                    if (map_path.empty()) {
                        panel.set_status("Aerial: tile actual no cargado");
                    } else if (!bounds_opt) {
                        panel.set_status("Aerial: define ref lat/lon o activa [worldcoordinates]");
                    } else {
                        panel.read_tile_page_edits(geo_settings);
                        panel.read_tile_page_flags(geo_settings.tile_aerial_visible,
                                                   geo_settings.aerial_resolution_pos);
                        const float size_m = active.size_meters;
                        const auto bounds = *bounds_opt;
                        auto out_jpg = map::tile_aerial_jpg_path(cfg.omsi_root, cfg.global.root, map_path);
                        const int out_px = map::effective_aerial_download_px(
                            geo_settings.satellite_provider, geo_settings.aerial_resolution_pos,
                            geo_settings.editor_link_aerial);
                        const auto provider = geo_settings.satellite_provider;
                        map::remove_tile_aerial_files(cfg.omsi_root, cfg.global.root, map_path);
                        const auto bmp_path = cfg.global.root / "texture" /
                                              ("aerial_" + std::to_string(active.grid_x) + "_" +
                                               std::to_string(active.grid_y) + ".bmp");
                        if (std::filesystem::exists(bmp_path)) {
                            std::error_code ec;
                            std::filesystem::remove(bmp_path, ec);
                        }
                        invalidate_aerial_thumb(active.grid_x, active.grid_y);
                        const int label_px =
                            map::aerial_resolution_label_px(geo_settings.aerial_resolution_pos);
                        if (out_px < label_px) {
                            panel.set_status(
                                std::string("Aerial: ") + map::satellite_provider_label(provider) +
                                " limitado a " + std::to_string(out_px) + "px (slider " +
                                std::to_string(label_px) + "px)");
                        }
                        const auto world_coords = cfg.global.world_coordinates;
                        const auto link_aerial = geo_settings.editor_link_aerial;
                        const auto bing_key = geo_settings.bing_api_key;
                        const auto google_key = geo_settings.google_api_key;
                        const auto maptiler_key = geo_settings.maptiler_api_key;
                        const auto custom_url = geo_settings.custom_aerial_url;
                        const auto res_pos = geo_settings.aerial_resolution_pos;
                        const auto map_root = cfg.global.root;
                        const auto gx = active.grid_x;
                        const auto gy = active.grid_y;

                        auto job = std::make_unique<EditorAerialJob>();
                        job->grid_x = gx;
                        job->grid_y = gy;
                        job->out_jpg = out_jpg;
                        EditorAerialJob* raw = job.get();
                        raw->thread = std::thread([raw, bounds, size_m, out_jpg, out_px, provider, world_coords,
                                                   link_aerial, bing_key, google_key, maptiler_key, custom_url,
                                                   res_pos, map_root, gx, gy]() {
                            try {
                                const bool is_omsi_link =
                                    provider == map::SatelliteProvider::OmsiLinkAerial ||
                                    provider == map::SatelliteProvider::GcmodsBing ||
                                    provider == map::SatelliteProvider::GcmodsGoogle;
                                if (is_omsi_link) {
                                    if (!world_coords) {
                                        raw->result.message = "world coords required";
                                    } else {
                                        std::string link = link_aerial;
                                        std::string key = bing_key;
                                        if (provider == map::SatelliteProvider::GcmodsBing) {
                                            link = map::gcmods_bing_satellite_link();
                                            key = bing_key;
                                        } else if (provider == map::SatelliteProvider::GcmodsGoogle) {
                                            link = map::gcmods_google_satellite_link();
                                            key = google_key;
                                        } else if (link.find("service=google") != std::string::npos) {
                                            key = google_key.empty() ? bing_key : google_key;
                                        }
                                        if (link.empty()) link = map::gcmods_bing_satellite_link();
                                        raw->result =
                                            map::download_tile_aerial_omsi(link, gx, gy, res_pos, out_jpg, key);
                                    }
                                } else if (provider == map::SatelliteProvider::GoogleStaticMaps) {
                                    if (google_key.empty())
                                        raw->result.message = "Google Static: ingresa API key";
                                    else
                                        raw->result =
                                            map::download_tile_aerial_google_static(google_key, bounds, out_px, out_jpg);
                                } else if (provider == map::SatelliteProvider::GoogleMapTiles) {
                                    if (google_key.empty()) {
                                        raw->result.message = "Google: ingresa API key";
                                    } else {
                                        const auto out_bmp =
                                            map_root / "texture" /
                                            ("aerial_" + std::to_string(gx) + "_" + std::to_string(gy) + ".bmp");
                                        raw->result =
                                            map::download_tile_aerial_bmp(google_key, bounds, size_m, out_bmp);
                                        if (raw->result.ok) raw->result.output_path = out_jpg;
                                    }
                                } else {
                                    std::string key;
                                    if (provider == map::SatelliteProvider::MapTiler) key = maptiler_key;
                                    else if (provider == map::SatelliteProvider::BingAerial) key = bing_key;
                                    raw->result = map::download_tile_aerial_satellite(
                                        provider, bounds, size_m, out_px, out_jpg, key, custom_url);
                                }
                            } catch (const std::exception& ex) {
                                raw->result.message = std::string("excepcion: ") + ex.what();
                            } catch (...) {
                                raw->result.message = "excepcion desconocida";
                            }
                            raw->finished.store(true);
                        });
                        aerial_job = std::move(job);
                        panel.set_status("Aerial: descargando " + std::to_string(out_px) +
                                         "px en segundo plano...");
                        log::info("editor aerial started " + out_jpg.filename().string() + " px=" +
                                  std::to_string(out_px) + " res_pos=" +
                                  std::to_string(geo_settings.aerial_resolution_pos) + " prov=" +
                                  std::to_string(static_cast<int>(provider)));
                    }
                } catch (const std::exception& ex) {
                    log::error(std::string("aerial download exception: ") + ex.what());
                    panel.set_status(std::string("Aerial error: ") + ex.what());
                } catch (...) {
                    log::error("aerial download unknown exception");
                    panel.set_status("Aerial: error interno (ver log)");
                }
            }
        }
        if (ev.apply_tile_dtm) {
            if (dtm_job && !dtm_job->finished.load()) {
                panel.set_status("DTM: descargando en segundo plano (~40 s)...");
            } else {
                if (dtm_job && dtm_job->thread.joinable()) dtm_job->thread.join();
                dtm_job.reset();

                sync_centerkachel_from_camera();
            const auto active = centerkachel_as_active();
                const auto& layout = world.tile_layout();
                const auto bounds = map::tile_wgs84_bounds(cfg.global, layout, active.grid_x, active.grid_y,
                                                           geo_settings.ref_lat, geo_settings.ref_lon);
                const auto map_path = find_tile_path(active.grid_x, active.grid_y);
                if (!bounds || map_path.empty()) {
                    panel.set_status("DTM: tile/bounds invalid — ref lat/lon o world coords");
                } else {
                    const auto bounds_copy = *bounds;
                    const auto provider = geo_settings.elevation_provider;
                    const auto google_key = geo_settings.google_api_key;
                    auto job = std::make_unique<EditorDtmJob>();
                    job->grid_x = active.grid_x;
                    job->grid_y = active.grid_y;
                    job->map_path = map_path;
                    EditorDtmJob* raw = job.get();
                    raw->thread = std::thread([raw, bounds_copy, provider, google_key]() {
                        try {
                            raw->result = map::download_tile_dtm_with_fallback(
                                provider, google_key, bounds_copy, editor::kTerrainDim, raw->heights);
                        } catch (const std::exception& ex) {
                            raw->result.message = std::string("excepcion: ") + ex.what();
                        } catch (...) {
                            raw->result.message = "excepcion desconocida";
                        }
                        raw->finished.store(true);
                    });
                    dtm_job = std::move(job);
                    panel.set_status("DTM: descargando en segundo plano (~40 s)...");
                    log::info("editor DTM started " + map_path.filename().string());
                }
            }
        }
        if (ev.import_tile_dem && !ev.import_tile_dem_path.empty()) {
            if (dtm_job && !dtm_job->finished.load()) {
                panel.set_status("DTM: ya hay una descarga en curso");
            } else {
                if (dtm_job && dtm_job->thread.joinable()) dtm_job->thread.join();
                dtm_job.reset();
                sync_centerkachel_from_camera();
                const auto active = centerkachel_as_active();
                const auto map_path = find_tile_path(active.grid_x, active.grid_y);
                if (map_path.empty()) {
                    panel.set_status("DEM import: tile invalido");
                } else {
                    const auto dem_path = ev.import_tile_dem_path;
                    auto job = std::make_unique<EditorDtmJob>();
                    job->grid_x = active.grid_x;
                    job->grid_y = active.grid_y;
                    job->map_path = map_path;
                    EditorDtmJob* raw = job.get();
                    raw->thread = std::thread([raw, dem_path]() {
                        try {
                            raw->result = map::import_heightmap_file(dem_path, editor::kTerrainDim, raw->heights);
                        } catch (const std::exception& ex) {
                            raw->result.message = std::string("excepcion: ") + ex.what();
                        } catch (...) {
                            raw->result.message = "excepcion desconocida";
                        }
                        raw->finished.store(true);
                    });
                    dtm_job = std::move(job);
                    panel.set_status("DEM: importando " + dem_path.filename().string() + "...");
                }
            }
        }

        auto cancel_edit = [&]() {
            if (edit_mode == EditMode::Mesh) {
                if (mesh_edit.uv_g) {
                    mesh_uv_g_end(true);
                    return;
                }
                edit_mode = EditMode::None;
                return;
            }
            if (edit_mode == EditMode::None && sco_path_edit.active &&
                !sco_path_edit.moving && !sco_path_edit.rotating && !sco_path_edit.following) {
                exit_sco_path_edit();
                return;
            }
            if (edit_mode == EditMode::None) return;
            if (sco_path_edit.moving || sco_path_edit.rotating || sco_path_edit.following) {
                if (sco_path_edit.sel >= 0 &&
                    static_cast<std::size_t>(sco_path_edit.sel) < sco_path_edit.paths.size()) {
                    auto& p = sco_path_edit.paths[static_cast<std::size_t>(sco_path_edit.sel)];
                    p.x = sco_path_edit.grab_lx;
                    p.y = sco_path_edit.grab_ly;
                    p.z = sco_path_edit.grab_lz;
                    p.heading_deg = sco_path_edit.grab_hdg;
                    p.length = sco_path_edit.grab_len;
                    p.radius = sco_path_edit.grab_radius;
                    rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
                }
                sco_path_edit.moving = sco_path_edit.rotating = sco_path_edit.following = false;
                edit_mode = EditMode::None;
                grabbing = false;
                rotating = false;
                r_tap = 0;
                g_tap = 0;
                return;
            }
            // TrimSpline only draws an overlay; never mutate live spline.
            // Falling through restored stale grab_start_* and jumped the spline on Esc/Cancel.
            if (edit_mode == EditMode::TrimSpline) {
                clear_trim_preview();
                trim_cuts.clear();
                edit_mode = EditMode::None;
                grabbing = false;
                rotating = false;
                r_tap = 0;
                g_tap = 0;
                dismiss_adjust_hud();
                log::info("editor edit cancelled");
                return;
            }
            if (edit_mode == EditMode::PickSplineGrad || edit_mode == EditMode::EditSplineGrad) {
                if (edit_mode == EditMode::EditSplineGrad && selected_spline >= 0 &&
                    static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    if (grad_edit_is_start) s.start_grad = grad_edit_base;
                    else s.end_grad = grad_edit_base;
                    remesh_spline(s, true);
                }
                edit_mode = EditMode::None;
                grabbing = false;
                rotating = false;
                r_tap = 0;
                g_tap = 0;
                log::info("editor grad edit cancelled");
                return;
            }
            if (group_rotate_active && edit_mode == EditMode::Rotate) {
                pop_undo("rotate_esc_cancel");
                for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                    const float ang = 0.f; // restore from stored offs at grab_start (unrotated)
                    (void)ang;
                    if (group_grab_kind[i] == 0) {
                        auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                        o.position.x = group_pivot_x + group_off_x[i];
                        o.position.y = group_pivot_y + group_off_y[i];
                        if (i < group_start_yaw.size()) o.rotation_deg.x = group_start_yaw[i];
                        sync_object_gpu(o);
                    } else {
                        auto& s = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                        s.position.x = group_pivot_x + group_off_x[i];
                        s.position.y = group_pivot_y + group_off_y[i];
                        if (i < group_start_yaw.size()) s.heading_deg = group_start_yaw[i];
                        remesh_spline(s);
                    }
                }
                group_rotate_active = false;
                group_rot_have_ref = false;
                group_grab_active = false;
                group_grab_kind.clear();
                group_grab_index.clear();
                group_grab_ids.clear();
                group_off_x.clear();
                group_off_y.clear();
                group_off_z.clear();
                group_grab_start_z.clear();
                group_grab_mesh_ox.clear();
                group_grab_mesh_oy.clear();
                group_start_yaw.clear();
                edit_mode = EditMode::None;
                grabbing = false;
                rotating = false;
                r_tap = 0;
                g_tap = 0;
                log::info("editor group rotate cancelled");
                return;
            }
            // Drop rotate/RR snap so Esc + later Ctrl+Z don't replay a stale pose.
            if ((edit_mode == EditMode::Rotate || edit_mode == EditMode::SplineFollow) &&
                !group_rotate_active) {
                pop_undo("rotate_esc_cancel");
            }
            if (edit_mode == EditMode::NewFollow || edit_mode == EditMode::NewGrab) {
                if (auto* g = find_spline_gpu(new_spline_draft.id)) {
                    g->alive = false;
                    for (const auto c : g->chunks) renderer.set_chunk_visible(c, false);
                }
            } else if (edit_mode == EditMode::NewScoFollow && selected_object >= 0 &&
                       static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                // Drop preview SCO + matching undo snap from N.
                pop_undo("n_sco_esc_cancel");
                const int kill_id = edit_objects[static_cast<std::size_t>(selected_object)].id;
                for (auto& sp : scene_places) {
                    if (sp.object_id != kill_id || !sp.alive) continue;
                    sp.alive = false;
                    renderer.set_chunk_visible(sp.chunk, false);
                }
                edit_objects.erase(edit_objects.begin() + selected_object);
                for (auto it = transient_objects.begin(); it != transient_objects.end(); ++it) {
                    if (it->id == kill_id) {
                        transient_objects.erase(it);
                        break;
                    }
                }
                selected_object = -1;
                edit_mode = EditMode::None;
                grabbing = false;
                rotating = false;
                r_tap = 0;
                g_tap = 0;
                log::info("editor N sco cancelled");
                return;
            } else if (shift_s_move.active &&
                       (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight)) {
                pop_undo("shift_s_esc_cancel");
                shift_s_move_restore_starts(true);
                shift_s_move_clear();
            } else if (group_grab_active &&
                       (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight)) {
                pop_undo("group_grab_esc_cancel");
                for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                    if (group_grab_kind[i] == 0) {
                        auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                        o.position.x = grab_start_x + group_off_x[i];
                        o.position.y = grab_start_y + group_off_y[i];
                        o.position.z = group_grab_start_z[i];
                        sync_object_gpu(o);
                    } else {
                        auto& s = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                        s.position.x = grab_start_x + group_off_x[i];
                        s.position.y = grab_start_y + group_off_y[i];
                        s.position.z = group_grab_start_z[i];
                        set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
                        remesh_spline(s);
                    }
                }
                group_grab_active = false;
                group_grab_kind.clear();
                group_grab_index.clear();
                group_grab_ids.clear();
                group_off_x.clear();
                group_off_y.clear();
                group_off_z.clear();
                group_grab_start_z.clear();
                group_grab_mesh_ox.clear();
                group_grab_mesh_oy.clear();
            } else if (selected_object >= 0 &&
                       static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                if (spline_attach_spawn_ids.count(o.id)) {
                    if (const auto sa_it = spline_attach_obj_to_entry.find(o.id);
                        sa_it != spline_attach_obj_to_entry.end() &&
                        sa_it->second < edit_spline_attachments.size()) {
                        edit_spline_attachments[sa_it->second].att.offset_y = grab_start_sa_offset_y;
                        apply_spline_attach_from_world_xy(sa_it->second, grab_start_x, grab_start_y, true);
                    }
                } else {
                    o.position.x = grab_start_x;
                    o.position.y = grab_start_y;
                    o.position.z = grab_start_z;
                    o.rotation_deg.x = grab_start_yaw;
                    sync_object_gpu(o);
                    show_object_at(static_cast<std::size_t>(selected_object));
                }
            } else if (selected_spline >= 0 &&
                       static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                s.position.x = grab_start_x;
                s.position.y = grab_start_y;
                s.position.z = grab_start_z;
                s.heading_deg = grab_start_yaw;
                s.length = grab_start_len;
                s.radius = grab_start_radius;
                set_spline_chunk_offset(s.id, 0.f, 0.f);
                remesh_spline(s);
                show_spline_at(static_cast<std::size_t>(selected_spline));
            }
            node_height_mode = false;
            grab_spline_id = 0;
            node_grab_snap_valid = false;
            edit_mode = EditMode::None;
            grabbing = false;
            rotating = false;
            new_grab_lock_xy = false;
            grab_suppress_g_cancel = false;
            r_tap = 0;
            g_tap = 0;
            dismiss_adjust_hud();
            log::info("editor edit cancelled");
        };

        if (bus_drive_active && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            if (!esc_was) exit_bus_drive();
            esc_was = true;
        } else if (editor_keys_live && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            if (!esc_was) {
                if (mesh_xform.active) {
                    mesh_xform_cancel();
                } else if (mesh_edit.loopcut_on) {
                    mesh_loopcut_cancel();
                    panel.set_status("Loop cut cancel");
                } else if (mesh_edit.knife_on) {
                    mesh_knife_cancel();
                    panel.set_status("Knife cancel");
                } else if (mesh_edit.uv_g) {
                    mesh_uv_g_end(true);
                    panel.set_status("UV cancel");
                } else if (edit_mode == EditMode::Mesh && (mesh_edit.tex_pick_kind != 0 || mesh_edit.tex_pick_list_open)) {
                    mesh_edit.tex_pick_kind = 0;
                    mesh_edit.tex_pick_list_open = false;
                    mesh_edit.tex_pick_sel_id = 0;
                    panel.set_status("Pick textura cancelado");
                } else if (sco_path_edit.active &&
                           (sco_path_edit.moving || sco_path_edit.rotating || sco_path_edit.following)) {
                    cancel_edit();
                } else if (sco_path_edit.active) {
                    try_exit_sco_path_edit();
                } else if (edit_mode == EditMode::Mesh) {
                    try_exit_mesh();
                } else if (marquee.phase == MarqueePhase::Select) {
                    // Recuadro / Lazo / Circulo: Esc goes back to Pick simple (the selection stays)
                    marquee.phase = MarqueePhase::None;
                    marquee.intent = MarqueeIntent::SelectOnly;
                    marquee.dragging = false;
                    sel_lasso_pts.clear();
                    sel_lasso_drawing = false;
                    sel_circle_brush_upto = 0;
                    clear_marquee_box();
                    panel.imgui_state().sel_mode_circle = false;
                    panel.imgui_state().sel_mode_lasso = false;
                    panel.imgui_state().spline_multi_pick = false;
                    panel.imgui_state().object_multi_pick = false;
                    panel.imgui_state().sel_ui_mode = 0;
                    panel.set_status("Pick simple");
                } else if (marquee.phase != MarqueePhase::None)
                    cancel_marquee();
                else if (edit_mode != EditMode::None)
                    cancel_edit();
                else if (autofill_spline_ends || autofill_path_mode) {
                    autofill_spline_ends = false;
                    autofill_path_mode = false;
                    autofill_path_stage = 0;
                    autofill_path_a = {};
                    autofill_path_b = {};
                    panel.set_autofill(false);
                    panel.set_status("Autofill cancelado (Esc)");
                    log::info("editor autofill cancelled (Esc)");
                    editor_log::event("KEY_ESC", "autofill_cancel | " + session_state());
                } else if (!group_sel_obj.empty() || !group_sel_spl.empty()) {
                    group_sel_obj.clear();
                    group_sel_spl.clear();
                    group_sel_obj_ids.clear();
                    group_sel_spl_ids.clear();
                    sli_bake_pick = false;
                    sli_bake_ready = false;
                    panel.imgui_state().spline_multi_pick = false;
                    panel.set_selection_text("(none)");
                    panel.set_status("Grupo deseleccionado");
                    dismiss_adjust_hud();
                }
            }
            esc_was = true;
        } else {
            esc_was = false;
        }

        main_watchdog.mark("M.dispatch.L01_3481");
        {
            const bool kp_dot = editor_keys_live && glfwGetKey(window, GLFW_KEY_KP_DECIMAL) == GLFW_PRESS;
            // BeamNG free flight (F11): F also frames the selection, like the BeamNG editor.
            static bool f_key_was = false;
            const bool f_key = camera.fly_mode && edit_mode == EditMode::None && editor_keys_live &&
                               glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
            if ((kp_dot && !kp_dot_was) || (f_key && !f_key_was)) {
                if (edit_mode == EditMode::None) focus_selection();
            }
            kp_dot_was = kp_dot;
            f_key_was = f_key;
        }
        {
            // Blender numpad views (editor and mesh .o3d mode): 7 top / Ctrl+7 bottom, 1 front / Ctrl+1 back,
            // 3 right / Ctrl+3 left, 9 opposite side, 5 ortho/persp, 2/8 orbit down/up, 4/6 orbit left/right.
            static bool kp_was[10] = {};
            const bool nav_ok = (edit_mode == EditMode::None || edit_mode == EditMode::Mesh) && !over_ui &&
                                !bus_drive_active && !ImGui::GetIO().WantTextInput;
            const bool kctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                               glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
            constexpr float kPi = 3.14159265f;
            constexpr float kStep = 15.f * kPi / 180.f;
            auto side_view = [&](float theta, const char* name) {
                camera.theta = theta;
                camera.phi = 0.5f * kPi; // horizontal
                camera.reset_mouse();
                panel.set_status(name);
            };
            for (int k = 1; k <= 9; ++k) {
                const bool down = glfwGetKey(window, GLFW_KEY_KP_0 + k) == GLFW_PRESS;
                const bool hit = down && !kp_was[k] && nav_ok;
                kp_was[k] = down;
                if (!hit) continue;
                switch (k) {
                case 7:
                    if (kctrl) {
                        camera.phi = kPi - 0.1f;
                        camera.reset_mouse();
                        panel.set_status("Ctrl+Numpad 7: vista inferior");
                    } else {
                        camera.set_top_down_view();
                        panel.set_status("Numpad 7: vista superior (top-down)");
                    }
                    break;
                // eye offset = (sin phi cos theta, sin phi sin theta, cos phi): front looks along +Y
                case 1: side_view(kctrl ? 0.5f * kPi : -0.5f * kPi, kctrl ? "Ctrl+Numpad 1: atras" : "Numpad 1: frente"); break;
                case 3: side_view(kctrl ? kPi : 0.f, kctrl ? "Ctrl+Numpad 3: izquierda" : "Numpad 3: derecha"); break;
                case 9:
                    camera.theta += kPi;
                    if (camera.phi < 0.2f || camera.phi > kPi - 0.2f) camera.phi = kPi - camera.phi;
                    camera.reset_mouse();
                    panel.set_status("Numpad 9: lado opuesto");
                    break;
                case 5:
                    camera.toggle_orthographic();
                    panel.set_status(camera.orthographic ? "Numpad 5: proyeccion ortografica"
                                                         : "Numpad 5: proyeccion en perspectiva");
                    break;
                case 2: camera.phi = (std::min)(camera.phi + kStep, kPi - 0.1f); break;
                case 8: camera.phi = (std::max)(camera.phi - kStep, 0.1f); break;
                case 4: camera.theta -= kStep; break;
                case 6: camera.theta += kStep; break;
                default: break;
                }
            }
        }

        show_spline_ends = ev.show_spline_ends;
        show_path_ends = ev.show_path_ends;
        if (ev.toggled_markers) autofill_spline_ends = ev.autofill_spline_ends;
        if (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight || edit_mode == EditMode::NewScoFollow ||
            edit_mode == EditMode::NewGrab)
            sync_spawn_height_from_ui();
        // Persist marker/vista toggles only when they actually change.
        // BUGFIX: vista_dirty could stay true every frame → disk write ~60×/s.
        const bool stick = panel.imgui_state().spline_stick_chain;
        const bool vista_dirty =
            ev.show_spline_ends != editor_ui_settings.show_spline_ends ||
            ev.show_path_ends != editor_ui_settings.show_path_ends ||
            ev.show_path_rails != editor_ui_settings.show_path_rails ||
            ev.autofill_spline_ends != editor_ui_settings.autofill_spline_ends ||
            ev.tex_max_256 != editor_ui_settings.tex_max_256 ||
            stick != editor_ui_settings.spline_stick_chain;
        // vista_dirty is false again on the next frame (fields copied below), so this is not a per-frame
        // write. view_only used to skip it, which is why Vista toggles never persisted in "Solo visualizar".
        if (ev.save_ui_settings || vista_dirty) {
            editor_ui_settings.show_spline_ends = ev.show_spline_ends;
            editor_ui_settings.show_path_ends = ev.show_path_ends;
            editor_ui_settings.show_path_rails = ev.show_path_rails;
            editor_ui_settings.autofill_spline_ends = ev.autofill_spline_ends;
            editor_ui_settings.tex_max_256 = ev.tex_max_256;
            editor_ui_settings.spline_stick_chain = stick;
            show_spline_ends = ev.show_spline_ends;
            show_path_ends = ev.show_path_ends;
            show_path_rails = ev.show_path_rails;
            autofill_spline_ends = ev.autofill_spline_ends;
            persist_ui_settings();
        }
        if (ev.apply_map_names) {
            bool ok = true;
            if (cfg.workspace) {
                ok = map::workspace_prepare_global_cfg_for_write(*cfg.workspace);
                if (ok) cfg.global.global_cfg_path = cfg.workspace->actual_global_cfg;
            }
            const std::string friendly =
                ev.map_friendly_value.empty() ? ev.map_name_value : ev.map_friendly_value;
            if (ok && map::set_global_map_names(cfg.global.global_cfg_path, ev.map_name_value, friendly)) {
                if (const auto g = map::load_global_map(cfg.global.global_cfg_path)) {
                    cfg.global.name = g->name;
                    cfg.global.friendly_name = g->friendly_name;
                }
                panel.imgui_state().map_names_seeded = false;
                panel.set_status("Nombre guardado en global.cfg (*_actual)");
            } else {
                panel.set_status("Error al guardar [name]/[friendlyname] en global.cfg");
            }
        }
        if (ev.reload_file_textures) {
            gfx::dx12::set_texture_max_edge(ev.tex_max_256 ? 256u : 0u);
            editor_ui_settings.tex_max_256 = ev.tex_max_256;
            editor_ui_settings.tex_max_256_from_file = true;
            (void)renderer.reload_file_textures();
            panel.set_status(ev.tex_max_256 ? "Texturas: límite 256px (OMSI texmax256)"
                                            : "Texturas: resolución nativa + mipmaps");
        }
        if (autofill_spline_ends) show_spline_ends = true;
        if (edit_mode == EditMode::PickSplineGrad || edit_mode == EditMode::EditSplineGrad)
            show_spline_ends = true;
        const bool rails_was = show_path_rails;
        path_rails_forced_by_tool = ev.tool == ui::EditorTool::TrafficRules ||
                                    ev.tool == ui::EditorTool::TracksTrips ||
                                    ev.tool == ui::EditorTool::StationLinks;
        show_path_rails = ev.show_path_rails || path_rails_forced_by_tool || bus_ai_paths_visible;
        if (show_path_rails) {
            // Only rebuild when turning ON or user toggles markers — NOT every frame while
            // uploads are still empty (!any_alive). That was re-kicking CPU forever.
            // Rails that already exist (hidden, not destroyed) are kept: only owners that have none yet are
            // queued, so switching tabs / toggling the checkbox no longer recomputes every path of the map.
            const bool busy = path_rails_rebuild ||
                              (path_rails_cpu_started && !path_rails_cpu_done.load(std::memory_order_acquire));
            if ((!rails_was || ev.toggled_markers) && !busy) {
                if (path_rails.empty()) refresh_path_rails();
                else sync_path_rails_after_stream();
            }
        } else if (rails_was) {
            for (auto& r : path_rails) {
                if (r.alive) renderer.set_chunk_visible(r.chunk, false);
            }
        }
        if (pending_path_dialog) {
            auto edit = *pending_path_dialog;
            pending_path_dialog.reset();
            HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
            if (ui::run_path_rule_dialog(owner, edit)) {
                apply_path_rule_view(edit);
                if (edit.from_object) {
                    for (const auto& o : edit_objects)
                        if (o.id == edit.owner_id) {
                            persist_editor_xy(o.position.x, o.position.y);
                            break;
                        }
                } else {
                    for (const auto& s : edit_splines)
                        if (s.id == edit.owner_id) {
                            persist_editor_xy(s.position.x, s.position.y);
                            break;
                        }
                }
                if (show_path_rails)
                    retint_path_rails_for_owner(edit.owner_id, edit.from_object, edit.path_index);
                panel.show_path_rule(edit);
                panel.set_status("Reglas de path guardadas");
            }
        }
        if (ev.open_path_rule_dialog && ev.path_rule_edit.valid) {
            auto edit = ev.path_rule_edit;
            HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
            if (ui::run_path_rule_dialog(owner, edit)) {
                apply_path_rule_view(edit);
                if (edit.from_object) {
                    for (const auto& o : edit_objects)
                        if (o.id == edit.owner_id) {
                            persist_editor_xy(o.position.x, o.position.y);
                            break;
                        }
                } else {
                    for (const auto& s : edit_splines)
                        if (s.id == edit.owner_id) {
                            persist_editor_xy(s.position.x, s.position.y);
                            break;
                        }
                }
                if (show_path_rails)
                    retint_path_rails_for_owner(edit.owner_id, edit.from_object, edit.path_index);
                panel.show_path_rule(edit);
                panel.set_status("Reglas de path guardadas");
            }
