        }
        if (ev.open_extra_lines_dialog && ev.object_edit.valid) {
            auto edit = ev.object_edit;
            HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
            if (ui::run_extra_lines_dialog(owner, edit)) {
                for (auto& o : edit_objects) {
                    if (o.id != edit.id) continue;
                    o.strings = edit.strings;
                    o.string_line_count = edit.text_slot_count > 0
                                              ? edit.text_slot_count
                                              : static_cast<std::int32_t>(edit.strings.size());
                    if (o.string_line_count < static_cast<std::int32_t>(o.strings.size()))
                        o.string_line_count = static_cast<std::int32_t>(o.strings.size());
                    fill_sco_map_extras(o);
                    if (const auto sa_it = spline_attach_obj_to_entry.find(o.id);
                        sa_it != spline_attach_obj_to_entry.end() &&
                        sa_it->second < edit_spline_attachments.size()) {
                        auto& entry = edit_spline_attachments[sa_it->second];
                        entry.att.object_text_lines = edit.strings;
                        entry.att.object_text_line_count =
                            edit.text_slot_count > 0 ? edit.text_slot_count
                                                     : static_cast<std::int32_t>(edit.strings.size());
                    }
                    for (auto& a : edit_attachments) {
                        if (a.child_id != edit.id) continue;
                        a.object_text_lines = edit.strings;
                        a.object_text_line_count = edit.text_slot_count > 0
                                                       ? edit.text_slot_count
                                                       : static_cast<std::int32_t>(edit.strings.size());
                        if (a.options_flag == 0 && a.object_text_line_count > 0) a.options_flag = 1;
                        break;
                    }
                    persist_editor_xy(o.position.x, o.position.y);
                    scenery_scripts.refresh_traffic_indices(edit_objects);
                    // Name / labels of entrypoint SCO update global.cfg string.
                    {
                        std::int32_t gx = tile_gx, gy = tile_gy;
                        for (const auto& sp : scene_places) {
                            if (sp.alive && sp.object_id == o.id) { gx = sp.tile_gx; gy = sp.tile_gy; break; }
                        }
                        sync_object_entrypoint_to_global(o, gx, gy);
                    }
                    break;
                }
                panel.show_object(edit);
                panel.set_status("Lineas extra guardadas");
            }
        }
        // H2: "Cargar mapa" is an OMSI-only path (it writes cfg.switch_*_out, null in BeamNG mode).
        if (ev.request_load_map && !cfg.beamng) {
            HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
            int ans = IDNO;
            if (!cfg.view_only && !dirty_tile_keys.empty()) {
                ans = MessageBoxW(
                    owner,
                    L"¿Guardar este mapa antes de cargar otro?\n\n"
                    L"Sí = te pregunta dónde (original o carpeta cambios) y después el launcher\n"
                    L"No = launcher sin guardar\n"
                    L"Cancelar = seguir en este mapa",
                    L"Load map", MB_YESNOCANCEL | MB_ICONQUESTION);
            }
            bool go_launcher = (ans != IDCANCEL);
            if (ans == IDYES && !cfg.view_only) {
                if (!begin_save_job())
                    go_launcher = false;
                else
                    finish_save_job();
            }
            if (go_launcher) {
                std::filesystem::path default_cfg = cfg.global.global_cfg_path;
                if (cfg.workspace && !cfg.workspace->actual_dir.empty()) {
                    const auto live = map::workspace_live_map_dir(*cfg.workspace);
                    if (!live.empty()) default_cfg = live / "global.cfg";
                }
                if (auto sel = omsi::launcher::show_launcher(cfg.omsi_root, default_cfg, cfg.entry_index, owner)) {
                    if (cfg.switch_map_out) *cfg.switch_map_out = sel->global_cfg;
                    if (cfg.switch_omsi_out) *cfg.switch_omsi_out = sel->omsi_root;
                    if (cfg.switch_entry_out) *cfg.switch_entry_out = sel->entrypoint_index;
                    if (cfg.switch_view_only_out) *cfg.switch_view_only_out = sel->view_only;
                    log::info("editor load map (launcher) -> " + sel->global_cfg.string() +
                              " omsi=" + sel->omsi_root.string() +
                              " entry=" + std::to_string(sel->entrypoint_index) +
                              (sel->view_only ? " [VIEW-ONLY]" : " [EDIT]"));
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }
        }
        if (ev.request_open_tile_map) {
            HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
            std::filesystem::path map_file = imgui_tile_info.map_path;
            if (map_file.empty() || !std::filesystem::exists(map_file)) {
                OPENFILENAMEW ofn{};
                wchar_t file[MAX_PATH]{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = owner;
                ofn.lpstrFilter = L"Tile map (*.map)\0*.map\0All\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrTitle = L"Abrir tile .map";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) map_file = file;
            }
            if (!map_file.empty() && std::filesystem::exists(map_file)) {
                std::string err;
                if (win32::shell_open_notepad(map_file, owner, &err))
                    panel.set_status("Notepad: " + map_file.filename().string());
                else
                    panel.set_status("Error Notepad: " + err);
            } else {
                panel.set_status("Abrir .map: no hay tile actual");
            }
        }
        if (ev.open_entrypoint_picker) {
            const int sel =
                ui::run_entrypoint_picker_dialog(editor_modal_owner(), cfg.global, cfg.global.entrypoints, 0);
            if (sel >= 0 && static_cast<std::size_t>(sel) < cfg.global.entrypoints.size())
                teleport_camera_to_entrypoint(cfg.global.entrypoints[static_cast<std::size_t>(sel)], "picker");
        }
        if (ev.open_options_dialog) {
            // Win32 modal removed — open ImGui options (ultralight/legacy menu path).
            panel.imgui_state().options_editor_open = true;
        }
        if (ev.options_vsync_set && !ev.apply_editor_options) {
            // VSync checkbox toggled on its own: apply to the swapchain and persist immediately.
            editor_ui_settings.vsync = ev.options_vsync;
            renderer.set_vsync(ev.options_vsync);
            map::save_editor_ui_settings(cfg.global.root, editor_ui_settings);
            panel.set_status(ev.options_vsync ? "VSync ON" : "VSync OFF");
        }
        if (ev.apply_editor_options) {
            int radius = ev.options_tile_radius;
            if (radius < 1) radius = 1;
            if (radius > 20) radius = 20;
            editor_tile_radius = radius;
            editor_ui_settings.editor_tile_radius = radius;
            editor_visual_radius_m = static_cast<float>(ev.options_visual_radius_m);
            if (editor_visual_radius_m > 0.f && editor_visual_radius_m < 50.f) editor_visual_radius_m = 50.f;
            if (editor_visual_radius_m > 2000.f) editor_visual_radius_m = 2000.f;
            editor_ui_settings.editor_visual_radius_m = editor_visual_radius_m;
            editor_cpu_radius_m = editor_visual_radius_m > 0.f ? editor_visual_radius_m : 0.f;
            editor_ui_settings.union_ball_screen_pct = ev.options_union_pct;
            editor_ui_settings.grad_circle_screen_pct = ev.options_grad_pct;
            if (ev.options_vsync_set) editor_ui_settings.vsync = ev.options_vsync;
            map::save_editor_ui_settings(cfg.global.root, editor_ui_settings);
            stream_queued_force = true;
            if (const auto focus =
                    stream_focus_from_rmb(camera.target_x, camera.target_y, camera.target_z)) {
                if (stream_busy()) {
                    stream_queued_focus = *focus;
                } else {
                    begin_stream_diff(*focus);
                    stream_queued_force = false;
                }
            } else {
                stream_queued_force = false;
            }
            if (ev.options_vsync_set) {
                renderer.set_vsync(ev.options_vsync);
            }
            panel.set_status("Tiles R=" + std::to_string(radius) + " visual/cpu=" +
                             (editor_visual_radius_m <= 0.f
                                  ? "off"
                                  : std::to_string(static_cast<int>(editor_visual_radius_m)) + "m") +
                             " union=" +
                             std::to_string(static_cast<int>(ev.options_union_pct * 1000.f) / 10.f) + "%" +
                             (ev.options_vsync_set ? (ev.options_vsync ? " | VSync ON" : " | VSync OFF") : ""));
        }
        if (ev.spawn_omsi_bus)
            request_spawn_omsi_bus(ev.omsi_bus_path.empty() ? editor_ui_settings.last_omsi_bus : ev.omsi_bus_path);
        if (ev.omsi_bus_despawn) despawn_omsi_bus();
        if (ev.omsi_bus_reset && omsi_bus) request_spawn_omsi_bus(omsi_bus->bus_file);
        if (ev.show_shortcuts_dialog) {
            panel.imgui_state().shortcuts_editor_open = true;
        }
        if (ev.apply_path_rule && ev.path_rule_edit.valid) {
            apply_path_rule_view(ev.path_rule_edit);
            // Persist owner tile
            if (ev.path_rule_edit.from_object) {
                for (const auto& o : edit_objects) {
                    if (o.id == ev.path_rule_edit.owner_id) {
                        persist_editor_xy(o.position.x, o.position.y);
                        break;
                    }
                }
            } else {
                for (const auto& s : edit_splines) {
                    if (s.id == ev.path_rule_edit.owner_id) {
                        persist_editor_xy(s.position.x, s.position.y);
                        break;
                    }
                }
            }
            if (show_path_rails)
                retint_path_rails_for_owner(ev.path_rule_edit.owner_id, ev.path_rule_edit.from_object,
                                            ev.path_rule_edit.path_index);
            // Reselect same rail if still present
            for (std::size_t i = 0; i < path_rails.size(); ++i) {
                const auto& r = path_rails[i];
                if (r.alive && r.owner_id == ev.path_rule_edit.owner_id &&
                    r.from_object == ev.path_rule_edit.from_object &&
                    r.path_index == ev.path_rule_edit.path_index) {
                    show_path_rail_at(static_cast<int>(i));
                    break;
                }
            }
            panel.set_status("Reglas de path aplicadas");
        }

        main_watchdog.mark("M.dispatch.L02_209");
        {
            static bool n_was = false;
            const bool n_now = editor_keys_live && !panel.keyboard_captured() &&
                               glfwGetKey(window, GLFW_KEY_N) == GLFW_PRESS;
            if (n_now && !n_was && sco_path_edit.active && edit_mode == EditMode::None)
                sco_start_new_path();
            n_was = n_now;
        }

        {
            const bool g_now = editor_shortcuts.held(ui::EditorAction::Grab);
            if (g_now && !g_was && view_only) {
                panel.set_status("Modo solo visualización — G (mover) bloqueado");
                editor_log::event("VIEW_ONLY_BLOCK", "G grab");
            } else if (g_now && !g_was && edit_mode == EditMode::Mesh && !ImGui::GetIO().WantTextInput) {
                if (mesh_edit.uv_editor_open && (mesh_edit.uv_win_focused || mesh_edit.uv_canvas_hov))
                    mesh_uv_g_begin(0);
                else if (mesh_xform.active && mesh_xform.mode == 0) {
                    mesh_xform.uv_lock = true;
                    mesh_xform.skip_weld = true;
                    panel.set_status("GG: mover geo, UV fijo (LMB confirma, Esc cancela)");
                } else if (!mesh_xform.active && !mesh_edit.uv_g)
                    mesh_xform_begin(0);
            } else if (g_now && !g_was && sco_path_edit.active && sco_path_edit.sel >= 0 &&
                       selected_spline < 0 && edit_mode == EditMode::None) {
                push_undo("sco_path_g");
                auto& p = sco_path_edit.paths[static_cast<std::size_t>(sco_path_edit.sel)];
                sco_path_edit.moving = true;
                sco_path_edit.rotating = false;
                sco_path_edit.following = false;
                sco_path_edit.grab_lx = p.x;
                sco_path_edit.grab_ly = p.y;
                sco_path_edit.grab_lz = p.z;
                sco_path_edit.grab_hdg = p.heading_deg;
                sco_path_edit.grab_len = p.length;
                sco_path_edit.grab_radius = p.radius;
                edit_mode = EditMode::Grab;
                grabbing = true;
                panel.set_status("G path .sco");
                sco_path_arm_live();
            } else if (g_now && !g_was) {
                if ((edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight) &&
                    grab_suppress_g_cancel) {
                    panel.set_status("Attach place: usá LMB para asentar o Esc para cancelar (G ignorado)");
                } else if (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight) {
                    // Second G: cancel first G (restore start pose). Drop matching undo snap.
                    pop_undo("grab_g_cancel");
                    clear_fill_temps();
                    editor_log::event("KEY_G", "cancel | " + session_state());
                    if (shift_s_move.active) {
                        shift_s_move_restore_starts(true);
                        shift_s_move_clear();
                    } else if (group_grab_active) {
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
                    } else if (selected_object >= 0 &&
                        static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                        auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                        if (spline_attach_spawn_ids.count(o.id)) {
                            if (const auto sa_it = spline_attach_obj_to_entry.find(o.id);
                                sa_it != spline_attach_obj_to_entry.end() &&
                                sa_it->second < edit_spline_attachments.size()) {
                                edit_spline_attachments[sa_it->second].att.offset_y = grab_start_sa_offset_y;
                                apply_spline_attach_from_world_xy(sa_it->second, grab_start_x, grab_start_y,
                                                                   true);
                            }
                        } else {
                            o.position.x = grab_start_x;
                            o.position.y = grab_start_y;
                            o.position.z = grab_start_z;
                            sync_object_gpu(o);
                            show_object_at(static_cast<std::size_t>(selected_object));
                        }
                    } else if (selected_spline >= 0 &&
                               static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                        auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                        s.position.x = grab_start_x;
                        s.position.y = grab_start_y;
                        s.position.z = grab_start_z;
                        set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
                        remesh_spline(s);
                        show_spline_at(static_cast<std::size_t>(selected_spline));
                    }
                    edit_mode = EditMode::None;
                    grabbing = false;
                    g_tap = 0;
                    grab_suppress_g_cancel = false;
                    dismiss_adjust_hud();
                    log::info("editor grab cancelled (GG)");
                } else if (edit_mode == EditMode::None && selected_spline < 0 && selected_object < 0 &&
                           group_sel_obj.empty() && group_sel_spl.empty() &&
                           chain_sel_handle_kind == 0 && // nodo seleccionado → G mueve el nodo, no gradiente
                           ev.tool == ui::EditorTool::Spline) {
                    edit_mode = EditMode::PickSplineGrad;
                    grabbing = false;
                    rotating = false;
                    show_spline_ends = true;
                    panel.set_autofill(false);
                    panel.set_status("G gradiente: click flecha inicio/fin de una .sli");
                    log::info("editor pick spline end for gradient");
                } else if (edit_mode == EditMode::PickSplineGrad && chain_sel_handle_kind == 1) {
                    // Estaba en pick gradiente pero hay nodo: G mueve el nodo
                    edit_mode = EditMode::None;
                    // fall through no — re-dispatch en el bloque de abajo con chain_sel
                    sync_spawn_height_from_ui();
                    if (chain_sel_handle_spline != 0 && selected_spline < 0) {
                        for (int i = 0; i < static_cast<int>(edit_splines.size()); ++i) {
                            if (edit_splines[static_cast<std::size_t>(i)].id ==
                                chain_sel_handle_spline) {
                                selected_spline = i;
                                break;
                            }
                        }
                    }
                    push_undo("grab_g_start");
                    if (selected_spline >= 0 &&
                        static_cast<std::size_t>(selected_spline) < edit_splines.size())
                        maybe_unlink_on_free_move(edit_splines[static_cast<std::size_t>(selected_spline)].id);
                    edit_mode = EditMode::Grab;
                    path_align_path_index = 0;
                    grabbing = true;
                    rotating = false;
                    g_tap = 1;
                    // Reuse same target setup as normal node G (below continues after this block ends)
                    // → jump into selected_spline path by not returning early
                    if (selected_spline >= 0 &&
                        static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                        const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                        grab_start_x = s.position.x;
                        grab_start_y = s.position.y;
                        grab_start_z = s.position.z;
                        grab_start_yaw = s.heading_deg;
                        grab_start_len = s.length;
                        grab_start_radius = s.radius;
                        grab_mesh_ox = s.position.x;
                        grab_mesh_oy = s.position.y;
                        grab_mesh_oz = s.position.z;
                        spline_grab_target = 0;
                        chain_fix_next_valid = false;
                        clear_fill_temps();
                        if (chain_sel_handle_partner != 0) {
                            if (chain_sel_handle_start_side) {
                                for (int i = 0; i < static_cast<int>(edit_splines.size()); ++i) {
                                    if (edit_splines[static_cast<std::size_t>(i)].id ==
                                        chain_sel_handle_partner) {
                                        selected_spline = i;
                                        break;
                                    }
                                }
                            }
                            spline_grab_target = 2;
                            panel.set_status("G nodo union: RR en final + biarc en siguiente (fin fijo)");
                            log::info("[nodo] G union (from PickSplineGrad)");
                        } else if (chain_sel_handle_start_side) {
                            spline_grab_target = 4;
                            panel.set_status("G nodo inicio: start sigue mouse, fin fijo");
                            log::info("[nodo] G inicio libre (from PickSplineGrad)");
                        } else {
                            spline_grab_target = 3;
                            panel.set_status("G nodo final: RR (start fijo, fin sigue mouse)");
                            log::info("[nodo] G final libre (from PickSplineGrad)");
                        }
                        // Multi collect + fixed ends (same as normal path)
                        {
                            map::SplineInst* sp = nullptr;
                            if (selected_spline >= 0 &&
                                static_cast<std::size_t>(selected_spline) < edit_splines.size())
                                sp = &edit_splines[static_cast<std::size_t>(selected_spline)];
                            if (sp) {
                                chain_fix_prev_id = sp->prev_id;
                                grab_start_x = sp->position.x;
                                grab_start_y = sp->position.y;
                                grab_start_z = sp->position.z;
                                grab_start_yaw = sp->heading_deg;
                                grab_start_len = sp->length;
                                grab_start_radius = sp->radius;
                                const auto pe = spline::evaluate_spline(*sp, sp->length);
                                chain_fix_next_ex = pe.x;
                                chain_fix_next_ey = pe.y;
                                chain_fix_next_ez = pe.z;
                                chain_fix_next_eh = pe.heading_rad * (180.f / 3.14159265f);
                                if (spline_grab_target == 4) chain_fix_next_valid = true;
                                if (sp->next_id != 0) {
                                    if (const auto* nxt = find_spline_by_id(sp->next_id)) {
                                        const auto ne = spline::evaluate_spline(*nxt, nxt->length);
                                        chain_fix_next_ex = ne.x;
                                        chain_fix_next_ey = ne.y;
                                        chain_fix_next_ez = ne.z;
                                        chain_fix_next_eh =
                                            ne.heading_rad * (180.f / 3.14159265f);
                                        chain_fix_next_id = nxt->id;
                                        chain_fix_next_valid = true;
                                    }
                                } else if (spline_grab_target == 2 &&
                                           chain_sel_handle_partner != 0) {
                                    const std::int32_t pid =
                                        (chain_sel_handle_spline == sp->id)
                                            ? chain_sel_handle_partner
                                            : chain_sel_handle_spline;
                                    if (const auto* nxt = find_spline_by_id(pid)) {
                                        const auto ne =
                                            spline::evaluate_spline(*nxt, nxt->length);
                                        chain_fix_next_ex = ne.x;
                                        chain_fix_next_ey = ne.y;
                                        chain_fix_next_ez = ne.z;
                                        chain_fix_next_eh =
                                            ne.heading_rad * (180.f / 3.14159265f);
                                        chain_fix_next_id = nxt->id;
                                        chain_fix_next_valid = true;
                                    }
                                }
                                if (spline_grab_target == 2) {
                                    node_starter_ids.clear();
                                    node_starter_fex.clear();
                                    node_starter_fey.clear();
                                    node_starter_feh.clear();
                                    node_ender_ids.clear();
                                    const auto jend =
                                        spline::evaluate_spline(*sp, sp->length);
                                    constexpr float kProx = 0.15f; // ~15 cm: mismo nodo visual
                                    auto add_st = [&](std::int32_t id) {
                                        if (id == 0 || id == sp->id) return;
                                        for (std::int32_t x : node_starter_ids)
                                            if (x == id) return;
                                        if (auto* b = find_spline_by_id(id)) {
                                            node_starter_ids.push_back(id);
                                            const auto be =
                                                spline::evaluate_spline(*b, b->length);
                                            node_starter_fex.push_back(be.x);
                                            node_starter_fey.push_back(be.y);
                                            node_starter_feh.push_back(
                                                be.heading_rad * (180.f / 3.14159265f));
                                        }
                                    };
                                    if (sp->next_id != 0) add_st(sp->next_id);
                                    if (chain_fix_next_id != 0) add_st(chain_fix_next_id);
                                    if (chain_sel_handle_partner != 0)
                                        add_st(chain_sel_handle_partner);
                                    for (const auto& o : edit_splines) {
                                        if (o.id == sp->id) continue;
                                        const float dx = o.position.x - jend.x;
                                        const float dy = o.position.y - jend.y;
                                        if (dx * dx + dy * dy <= kProx * kProx)
                                            add_st(o.id);
                                        const auto oe =
                                            spline::evaluate_spline(o, o.length);
                                        const float ex = oe.x - jend.x;
                                        const float ey = oe.y - jend.y;
                                        if (ex * ex + ey * ey <= kProx * kProx) {
                                            bool have = false;
                                            for (std::int32_t x : node_ender_ids)
                                                if (x == o.id) {
                                                    have = true;
                                                    break;
                                                }
                                            if (!have) node_ender_ids.push_back(o.id);
                                        }
                                    }
                                    log::info("[nodo] G multi starters=" +
                                              std::to_string(node_starter_ids.size()) +
                                              " enders=" +
                                              std::to_string(node_ender_ids.size()));
                                }
                            }
                        }
                    }
                    log::info("editor grab XY from node (was PickSplineGrad)");
                } else if (edit_mode == EditMode::None &&
                           (!group_sel_obj.empty() || !group_sel_spl.empty() || selected_object >= 0 ||
                            selected_spline >= 0 || selected_entrypoint >= 0 ||
                            chain_sel_handle_kind == 1 || chain_sel_handle_kind == 2)) {
                    sync_spawn_height_from_ui();
                    const bool shift_s_group = !group_sel_obj.empty() || !group_sel_spl.empty();
                    // Nodo: SIEMPRE resolver primary desde el nodo (no desde body seleccionado
                    // ni catálogo). Evita reemplazar .sli de la spline del joint.
                    if ((chain_sel_handle_kind == 1 || chain_sel_handle_kind == 2) &&
                        chain_sel_handle_spline != 0) {
                        // Primary = quien TERMINA en el joint si hay partner; si no, el del handle.
                        std::int32_t primary_id = chain_sel_handle_spline;
                        if (chain_sel_handle_kind == 1 && chain_sel_handle_partner != 0) {
                            // end_id = quien termina, start_id = quien inicia
                            primary_id = chain_sel_handle_start_side
                                             ? chain_sel_handle_partner
                                             : chain_sel_handle_spline;
                        }
                        selected_spline = -1;
                        for (int i = 0; i < static_cast<int>(edit_splines.size()); ++i) {
                            if (edit_splines[static_cast<std::size_t>(i)].id == primary_id) {
                                selected_spline = i;
                                break;
                            }
                        }
                        // No tocar sli_path — se usa el de cada instancia
                    }
                    push_undo("grab_g_start");
                    if (selected_spline >= 0 &&
                        static_cast<std::size_t>(selected_spline) < edit_splines.size())
                        maybe_unlink_on_free_move(edit_splines[static_cast<std::size_t>(selected_spline)].id);
                    edit_mode = EditMode::Grab;
                    path_align_path_index = 0;
                    grabbing = true;
                    rotating = false;
                    g_tap = 1;
                    if (shift_s_group) {
                        // Dedicated Shift+S path — never touches group_grab_* (rotate/marquee).
                        group_grab_active = false;
                        if (!shift_s_move_begin()) {
                            pop_undo("shift_s_move_empty");
                            edit_mode = EditMode::None;
                            grabbing = false;
                            g_tap = 0;
                            panel.set_status("Shift+S: nada que mover (ids invalidos)");
                        } else {
                            panel.set_status("Shift+S mover " +
                                             std::to_string(shift_s_move.items.size()) +
                                             " (delta comun; LMB=asentar Esc/GG=cancel)");
                            log::info("editor shift_s move begin count=" +
                                      std::to_string(shift_s_move.items.size()));
                            editor_log::event("KEY_G",
                                              "shift_s_move_start count=" +
                                                  std::to_string(shift_s_move.items.size()) +
                                                  " obj=" + std::to_string(group_sel_obj_ids.size()) +
                                                  " spl=" + std::to_string(group_sel_spl_ids.size()) +
                                                  " | " + session_state());
                        }
                    } else if (selected_object >= 0 &&
                        static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                        const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                        grab_start_x = o.position.x;
                        grab_start_y = o.position.y;
                        grab_start_z = o.position.z;
                        grab_start_yaw = o.rotation_deg.x;
                        grab_start_sa_offset_y = 0.f;
                        if (const auto sa_it = spline_attach_obj_to_entry.find(o.id);
                            sa_it != spline_attach_obj_to_entry.end() &&
                            sa_it->second < edit_spline_attachments.size()) {
                            grab_start_sa_offset_y =
                                edit_spline_attachments[sa_it->second].att.offset_y;
                        }
                        editor_log::event("KEY_G", "grab_start | " + session_state());
                    } else if (selected_spline >= 0 &&
                               static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                        const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                        grab_start_x = s.position.x;
                        grab_start_y = s.position.y;
                        grab_start_z = s.position.z;
                        grab_start_yaw = s.heading_deg;
                        grab_start_len = s.length;
                        grab_start_radius = s.radius;
                        grab_mesh_ox = s.position.x;
                        grab_mesh_oy = s.position.y;
                        grab_mesh_oz = s.position.z;
                        // Target: body / center / union / nodo libre según handle.
                        // 0=body 1=centro 2=union(joint) 3=nodo final libre 4=nodo inicio libre
                        spline_grab_target = 0;
                        chain_fix_next_valid = false;
                        clear_fill_temps();
                        if (chain_sel_handle_kind == 2 && chain_sel_handle_spline == s.id) {
                            spline_grab_target = 1; // centro → panza / gradientes
                            panel.set_status("G centro: panza/gradientes (extremos fijos si aislada)");
                        } else if (chain_sel_handle_kind == 1 &&
                                   (chain_sel_handle_spline == s.id ||
                                    chain_sel_handle_partner == s.id)) {
                            if (chain_sel_handle_partner != 0) {
                                // Ejemplo A: joint — RR en quien termina + biarc en quien inicia
                                // Primary = quien tiene el FINAL en el joint.
                                if (chain_sel_handle_start_side) {
                                    // Click en inicio de primary → partner es el que termina
                                    // Re-asignar selected_spline al que termina (end side)
                                    for (int i = 0; i < static_cast<int>(edit_splines.size()); ++i) {
                                        if (edit_splines[static_cast<std::size_t>(i)].id ==
                                            chain_sel_handle_partner) {
                                            selected_spline = i;
                                            break;
                                        }
                                    }
                                }
                                spline_grab_target = 2;
                                panel.set_status("G nodo union: RR en final + biarc en siguiente (fin fijo)");
                                log::info("[nodo] G union primary/partner");
                            } else if (chain_sel_handle_start_side) {
                                // Ejemplo C: inicio libre
                                spline_grab_target = 4;
                                panel.set_status("G nodo inicio: start sigue mouse, fin fijo (RR invertido)");
                                log::info("[nodo] G inicio libre id=" + std::to_string(s.id));
                            } else {
                                // Ejemplo B: final libre
                                spline_grab_target = 3;
                                panel.set_status("G nodo final: RR (start fijo, fin sigue mouse)");
                                log::info("[nodo] G final libre id=" + std::to_string(s.id));
                            }
                        }
                        // Capture node grab snapshot (for the Z delta_h mode) + the grabbed id.
                        node_height_mode = false;
                        node_grab_snap_valid = false;
                        grab_spline_id = 0;
                        node_grab_snap_has_prev = node_grab_snap_has_next = false;
                        if (selected_spline >= 0 &&
                            static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                            auto& sp0 = edit_splines[static_cast<std::size_t>(selected_spline)];
                            grab_spline_id = sp0.id;
                            node_grab_snap_primary = sp0;
                            if (sp0.prev_id != 0)
                                if (auto* p = find_spline_by_id(sp0.prev_id)) {
                                    node_grab_snap_prev = *p;
                                    node_grab_snap_has_prev = true;
                                }
                            if (sp0.next_id != 0)
                                if (auto* n = find_spline_by_id(sp0.next_id)) {
                                    node_grab_snap_next = *n;
                                    node_grab_snap_has_next = true;
                                }
                            node_grab_snap_valid = true;
                        }
                        chain_fix_next_id = 0;
                        chain_fix_prev_id = 0;
                        // Re-leer s por si reasignamos selected_spline
                        {
                            map::SplineInst* sp = nullptr;
                            if (selected_spline >= 0 &&
                                static_cast<std::size_t>(selected_spline) < edit_splines.size())
                                sp = &edit_splines[static_cast<std::size_t>(selected_spline)];
                            if (sp) {
                                chain_fix_prev_id = sp->prev_id;
                                grab_start_x = sp->position.x;
                                grab_start_y = sp->position.y;
                                grab_start_z = sp->position.z;
                                grab_start_yaw = sp->heading_deg;
                                grab_start_len = sp->length;
                                grab_start_radius = sp->radius;
                                // Fin fijo del primary (para target 4)
                                {
                                    const auto pe = spline::evaluate_spline(*sp, sp->length);
                                    chain_fix_next_ex = pe.x;
                                    chain_fix_next_ey = pe.y;
                                    chain_fix_next_ez = pe.z;
                                    chain_fix_next_eh = pe.heading_rad * (180.f / 3.14159265f);
                                    if (spline_grab_target == 4) chain_fix_next_valid = true;
                                }
                                if (sp->next_id != 0) {
                                    if (const auto* nxt = find_spline_by_id(sp->next_id)) {
                                        const auto ne = spline::evaluate_spline(*nxt, nxt->length);
                                        chain_fix_next_ex = ne.x;
                                        chain_fix_next_ey = ne.y;
                                        chain_fix_next_ez = ne.z;
                                        chain_fix_next_eh = ne.heading_rad * (180.f / 3.14159265f);
                                        chain_fix_next_id = nxt->id;
                                        chain_fix_next_valid = true;
                                    }
                                } else if (spline_grab_target == 2 && chain_sel_handle_partner != 0) {
                                    const std::int32_t pid =
                                        (chain_sel_handle_spline == sp->id) ? chain_sel_handle_partner
                                                                           : chain_sel_handle_spline;
                                    if (const auto* nxt = find_spline_by_id(pid)) {
                                        // Partner puede ser el que inicia: fijar SU fin
                                        const auto ne = spline::evaluate_spline(*nxt, nxt->length);
                                        chain_fix_next_ex = ne.x;
                                        chain_fix_next_ey = ne.y;
                                        chain_fix_next_ez = ne.z;
                                        chain_fix_next_eh = ne.heading_rad * (180.f / 3.14159265f);
                                        chain_fix_next_id = nxt->id;
                                        chain_fix_next_valid = true;
                                    }
                                }
                                // D/E: recolectar starters (inician en joint) y enders (terminan en joint)
                                if (spline_grab_target == 2) {
                                    node_starter_ids.clear();
                                    node_starter_fex.clear();
                                    node_starter_fey.clear();
                                    node_starter_feh.clear();
                                    node_ender_ids.clear();
                                    node_ender_sx.clear();
                                    node_ender_sy.clear();
                                    node_ender_sz.clear();
                                    node_ender_sh.clear();
                                    // Joint de referencia: preferir el START del starter (B)
                                    // si existe partner, porque ahí terminan TODAS las A.
                                    const auto jend_sp = spline::evaluate_spline(*sp, sp->length);
                                    float jref_x = jend_sp.x, jref_y = jend_sp.y, jref_z = jend_sp.z;
                                    float jref_h = jend_sp.heading_rad * (180.f / 3.14159265f);
                                    // Partner / next como starter: su start ES el joint
                                    auto use_starter_as_joint = [&](std::int32_t sid) {
                                        if (sid == 0 || sid == sp->id) return;
                                        if (auto* b = find_spline_by_id(sid)) {
                                            jref_x = b->position.x;
                                            jref_y = b->position.y;
                                            jref_z = b->position.z;
                                            jref_h = b->heading_deg;
                                        }
                                    };
                                    if (chain_sel_handle_partner != 0 && chain_sel_handle_start_side)
                                        use_starter_as_joint(chain_sel_handle_spline); // click en B start
                                    else if (chain_sel_handle_partner != 0)
                                        use_starter_as_joint(chain_sel_handle_partner); // click en A end
                                    else if (sp->next_id != 0)
                                        use_starter_as_joint(sp->next_id);
                                    node_joint0_x = jref_x;
                                    node_joint0_y = jref_y;
                                    node_joint0_z = jref_z;
                                    node_joint0_h = jref_h;
                                    node_joint0_valid = true;
                                    // 3.5 m: varias A pueden quedar un poco separadas tras RR previos
                                    constexpr float kProx = 0.15f; // ~15 cm: mismo nodo visual
                                    auto add_st = [&](std::int32_t id) {
                                        if (id == 0 || id == sp->id) return;
                                        for (std::int32_t x : node_starter_ids)
                                            if (x == id) return;
                                        if (auto* b = find_spline_by_id(id)) {
                                            node_starter_ids.push_back(id);
                                            const auto be = spline::evaluate_spline(*b, b->length);
                                            node_starter_fex.push_back(be.x);
                                            node_starter_fey.push_back(be.y);
                                            node_starter_feh.push_back(
                                                be.heading_rad * (180.f / 3.14159265f));
                                        }
                                    };
                                    auto add_en = [&](std::int32_t id) {
                                        if (id == 0) return;
                                        for (std::int32_t x : node_ender_ids)
                                            if (x == id) return;
                                        if (auto* e = find_spline_by_id(id)) {
                                            node_ender_ids.push_back(id);
                                            if (id == sp->id) {
                                                node_ender_sx.push_back(grab_start_x);
                                                node_ender_sy.push_back(grab_start_y);
                                                node_ender_sz.push_back(grab_start_z);
                                                node_ender_sh.push_back(grab_start_yaw);
                                            } else {
                                                node_ender_sx.push_back(e->position.x);
                                                node_ender_sy.push_back(e->position.y);
                                                node_ender_sz.push_back(e->position.z);
                                                node_ender_sh.push_back(e->heading_deg);
                                            }
                                        }
                                    };
                                    add_en(sp->id);
                                    // Partner solo si es STARTER real (inicio en joint), no otro ender
                                    auto maybe_add_st = [&](std::int32_t id) {
                                        if (id == 0 || id == sp->id) return;
                                        auto* b = find_spline_by_id(id);
                                        if (!b) return;
                                        const float ds = std::hypot(b->position.x - jref_x,
                                                                    b->position.y - jref_y);
                                        const auto be = spline::evaluate_spline(*b, b->length);
                                        const float de = std::hypot(be.x - jref_x, be.y - jref_y);
                                        // Solo starter si el START está más cerca del joint que el FIN
                                        if (ds <= kProx && ds <= de + 0.05f)
                                            add_st(id);
                                    };
                                    if (sp->next_id != 0) maybe_add_st(sp->next_id);
                                    if (chain_fix_next_id != 0) maybe_add_st(chain_fix_next_id);
                                    if (chain_sel_handle_partner != 0)
                                        maybe_add_st(chain_sel_handle_partner);
                                    if (chain_sel_handle_start_side && chain_sel_handle_spline != 0)
                                        maybe_add_st(chain_sel_handle_spline);
                                    for (const auto& o : edit_splines) {
                                        if (o.id == sp->id) continue;
                                        if (!std::isfinite(o.position.x) || !std::isfinite(o.length) ||
                                            o.length < 0.f || o.length > 1.e5f)
                                            continue;
                                        const float ds =
                                            std::hypot(o.position.x - jref_x, o.position.y - jref_y);
                                        const auto oe = spline::evaluate_spline(o, o.length);
                                        if (!std::isfinite(oe.x) || !std::isfinite(oe.y)) continue;
                                        const float de = std::hypot(oe.x - jref_x, oe.y - jref_y);
                                        // Clasificación exclusiva: ender gana si el FIN está más cerca
                                        if (de <= kProx && de <= ds + 0.05f)
                                            add_en(o.id);
                                        else if (ds <= kProx && ds < de)
                                            add_st(o.id);
                                    }
                                    // Quitar de starters cualquier id que también sea ender
                                    for (std::size_t si = 0; si < node_starter_ids.size();) {
                                        bool is_en = false;
                                        for (std::int32_t eid : node_ender_ids)
                                            if (eid == node_starter_ids[si]) {
                                                is_en = true;
                                                break;
                                            }
                                        if (is_en) {
                                            node_starter_ids.erase(node_starter_ids.begin() +
                                                                   static_cast<std::ptrdiff_t>(si));
                                            node_starter_fex.erase(node_starter_fex.begin() +
                                                                   static_cast<std::ptrdiff_t>(si));
                                            node_starter_fey.erase(node_starter_fey.begin() +
                                                                   static_cast<std::ptrdiff_t>(si));
                                            node_starter_feh.erase(node_starter_feh.begin() +
                                                                   static_cast<std::ptrdiff_t>(si));
                                        } else
                                            ++si;
                                    }
                                    log::info("[nodo] joint_ref=(" + std::to_string(jref_x) + "," +
                                              std::to_string(jref_y) + ") prox=" +
                                              std::to_string(kProx));
                                    node_sli_lock_ids.clear();
                                    node_sli_lock_paths.clear();
                                    auto lock_sli = [&](std::int32_t id) {
                                        if (id == 0) return;
                                        for (std::int32_t x : node_sli_lock_ids)
                                            if (x == id) return;
                                        if (auto* p = find_spline_by_id(id)) {
                                            node_sli_lock_ids.push_back(id);
                                            node_sli_lock_paths.push_back(p->sli_path);
                                        }
                                    };
                                    lock_sli(sp->id);
                                    for (std::int32_t id : node_starter_ids) lock_sli(id);
                                    for (std::int32_t id : node_ender_ids) lock_sli(id);
                                    log::info("[nodo] G multi starters=" +
                                              std::to_string(node_starter_ids.size()) +
                                              " enders=" + std::to_string(node_ender_ids.size()) +
                                              " primary_id=" + std::to_string(sp->id) +
                                              " primary_sli=" + sp->sli_path.filename().string());
                                    for (std::size_t si = 0; si < node_starter_ids.size(); ++si) {
                                        std::string sn = "?";
                                        if (auto* b = find_spline_by_id(node_starter_ids[si]))
                                            sn = b->sli_path.filename().string();
                                        log::info("[nodo]   starter[" + std::to_string(si) +
                                                  "] id=" + std::to_string(node_starter_ids[si]) +
                                                  " sli=" + sn + " fe=(" +
                                                  std::to_string(node_starter_fex[si]) + "," +
                                                  std::to_string(node_starter_fey[si]) + ") feh=" +
                                                  std::to_string(node_starter_feh[si]));
                                    }
                                    for (std::size_t ei = 0; ei < node_ender_ids.size(); ++ei) {
                                        std::string sn = "?";
                                        if (auto* e = find_spline_by_id(node_ender_ids[ei]))
                                            sn = e->sli_path.filename().string();
                                        log::info("[nodo]   ender[" + std::to_string(ei) +
                                                  "] id=" + std::to_string(node_ender_ids[ei]) +
                                                  " sli=" + sn +
                                                  (node_ender_ids[ei] == sp->id ? " (primary)" : ""));
                                    }
                                    if (node_starter_ids.size() > 1)
                                        log::info("[nodo] DETECT multi-start n=" +
                                                  std::to_string(node_starter_ids.size()));
                                    if (node_ender_ids.size() > 1)
                                        log::info("[nodo] DETECT multi-end n=" +
                                                  std::to_string(node_ender_ids.size()));
                                }
                            }
                        }
                    } else if (selected_entrypoint >= 0 &&
                               static_cast<std::size_t>(selected_entrypoint) < entry_chunks.size()) {
                        const auto& e = entry_chunks[static_cast<std::size_t>(selected_entrypoint)];
                        grab_start_x = e.world_x;
                        grab_start_y = e.world_y;
                        grab_start_z = e.world_z;
                    }
                    if (edit_mode == EditMode::Grab)
                        log::info("editor grab XY (G again=cancel, Z=height, LMB=place Esc=cancel)");
                }
            }
            g_was = g_now;
            if (editor_shortcuts.pressed(ui::EditorAction::Hide) && !view_only) {
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
                for (auto& sp : scene_places) {
                    if (!sp.alive) continue;
                    renderer.set_chunk_visible(sp.chunk, hs.hidden_object_ids.count(sp.object_id) == 0);
                }
                for (auto& g : spline_gpu) {
                    if (!g.alive) continue;
                    const bool show = hs.hidden_spline_ids.count(g.id) == 0;
                    for (const auto c : g.chunks) renderer.set_chunk_visible(c, show);
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

                panel.set_status("Ocultar (H)");
            }
        }
        main_watchdog.mark("M.dispatch.L02_946");
        {
            static bool f9_was = false;
            const bool f9_now = hotkey_down(GLFW_KEY_F9, VK_F9);
            if (f9_now && !f9_was && edit_mode == EditMode::Grab &&
                (selected_spline >= 0 || selected_object >= 0)) {
                const int pc = path_align_path_count();
                if (pc > 0) {
                    path_align_path_index = (path_align_path_index + 1) % pc;
                    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && !over_ui) {
                        float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                        input::cursor_to_framebuffer(window, mx, my, fw, fh);
                        const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                        float wvp_pick[16]{};
                        camera.build_world_view_proj(aspect, wvp_pick);
                        const auto pp = pick_path_endpoint_near_mouse(mx, my, fw, fh, wvp_pick);
                        if (pp.valid) apply_path_align_grab(pp);
                    }
                    panel.set_status("Path align " + std::to_string(path_align_path_index) + "/" +
                                     std::to_string((std::max)(1, pc) - 1));
                }
            }
            f9_was = f9_now;
        }
        {
            static bool z_was = false;
            const bool z_now = hotkey_down(GLFW_KEY_Z, 'Z');
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (z_now && !z_was && !ctrl &&
                (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight) &&
                node_grab_snap_valid && grab_spline_id != 0 && spline_grab_target != 0) {
                // Z on a node grab: cancel the G geometry change and switch to delta_h-only mode.
                auto* sp = find_spline_by_id(grab_spline_id);
                if (sp) {
                    *sp = node_grab_snap_primary;
                    if (node_grab_snap_has_prev)
                        if (auto* p = find_spline_by_id(node_grab_snap_prev.id)) *p = node_grab_snap_prev;
                    if (node_grab_snap_has_next)
                        if (auto* n = find_spline_by_id(node_grab_snap_next.id)) *n = node_grab_snap_next;
                    set_spline_chunk_offset(sp->id, 0.f, 0.f, 0.f);
                    remesh_spline(*sp, true);
                    if (node_grab_snap_has_prev)
                        if (auto* p = find_spline_by_id(node_grab_snap_prev.id)) remesh_spline(*p, true);
                    if (node_grab_snap_has_next)
                        if (auto* n = find_spline_by_id(node_grab_snap_next.id)) remesh_spline(*n, true);
                }
                node_height_mode = true;
                edit_mode = EditMode::GrabHeight;
                grabbing = true;
                {
                    float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                    input::cursor_to_framebuffer(window, mx, my, fw, fh);
                    grab_height_my0 = my;
                }
                panel.set_status("Z nodo: delta_h (mouse) — LMB coloca, Esc cancela");
                log::info("editor node delta_h mode id=" + std::to_string(grab_spline_id));
            } else if (z_now && !z_was && !ctrl &&
                (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight)) {
                if (shift_s_move.active) {
                    // Keep current XY delta; only switch to height editing.
                    shift_s_move_apply(shift_s_move.cur_dx, shift_s_move.cur_dy, 0.f, true, false);
                } else if (group_grab_active) {
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
                            remesh_spline(s, true);
                        }
                    }
                } else if (selected_object >= 0 &&
                    static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                    auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                    o.position.x = grab_start_x;
                    o.position.y = grab_start_y;
                    o.position.z = grab_start_z;
                    sync_object_gpu(o);
                    show_object_at(static_cast<std::size_t>(selected_object));
                } else if (selected_spline >= 0 &&
                           static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    s.position.x = grab_start_x;
                    s.position.y = grab_start_y;
                    s.position.z = grab_start_z;
                    set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
                    remesh_spline(s, true);
                    show_spline_at(static_cast<std::size_t>(selected_spline));
                }
                edit_mode = EditMode::GrabHeight;
                grabbing = true;
                if (shift_s_move.active && !shift_s_move.items.empty()) {
                    const auto& it = shift_s_move.items.front();
                    grab_start_x = it.x0;
                    grab_start_y = it.y0;
                    grab_start_z = it.z0;
                } else if (group_grab_active && !group_grab_start_z.empty()) {
                    grab_start_z = group_grab_start_z.front();
                }
                float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                input::cursor_to_framebuffer(window, mx, my, fw, fh);
                grab_height_my0 = my;
                log::info("editor grab HEIGHT only (Z) — restored G pose, LMB=place Esc/GG=cancel");
            }
            // Ctrl+Alt+Z / Ctrl+Shift+Alt+Z camera undo/redo; Ctrl+Z / Ctrl+Shift+Z editor undo/redo
            const bool z_async = editor_keys_live && (GetAsyncKeyState('Z') & 0x8000) != 0;
            static bool z_undo_was = false;
            const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            if (z_async && !z_undo_was && ctrl && alt) {
                if (!typing_ui) {
                const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                if (shift) {
                    if (!redo_camera()) {
                        editor_log::event("CAM_REDO_EMPTY", session_state());
                        panel.set_status("Camara redo: vacio");
                    } else {
                        editor_log::event("CAM_REDO_OK", session_state());
                        log::info("editor camera redo");
                    }
                } else if (!undo_camera()) {
                    editor_log::event("CAM_UNDO_EMPTY", session_state());
                    panel.set_status("Camara undo: vacio");
                } else {
                    editor_log::event("CAM_UNDO_OK", session_state());
                    log::info("editor camera undo");
                }
                }
            } else if (z_async && !z_undo_was && ctrl && !alt) {
                if (edit_mode == EditMode::Mesh) {
                    if (mesh_xform.active) {
                        mesh_xform_cancel();
                    } else {
                    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (shift) mesh_redo_face();
                    else mesh_undo_face();
                    }
                } else if (view_only) {
                    panel.set_status("Modo solo visualización — Ctrl+Z bloqueado");
                    editor_log::event("VIEW_ONLY_BLOCK", "Ctrl+Z undo");
                } else if (panel.current_tool() == ui::EditorTool::TracksTrips ||
                           panel.current_tool() == ui::EditorTool::StationLinks) {
                    // Tracks & Trips / Station Links: undo the last edit of the TTData (memory only, saved with Save).
                    auto& tm = omsi::tt::global_tt_man();
                    tm.track_changes();
                    const bool ok = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ? tm.redo() : tm.undo();
                    panel.set_status(ok ? "TT: deshacer / rehacer" : "TT: nada que deshacer / rehacer");
                    if (ok) panel.set_tt_from_man();
                } else if (edit_mode != EditMode::None || marquee.phase != MarqueePhase::None ||
                    group_settle_busy() || async_remesh_backlog() > 24) {
                    // backlog > 24: many rapid undo/redo steps queued more remeshes than the worker finished;
                    // applying another snapshot on top is what corrupted the GPU upload (crash in memcpy).
                    editor_log::event("UNDO_BLOCKED",
                                      "Ctrl+Z while busy | " + session_state() +
                                          " shift=" +
                                          std::to_string((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0));
                    panel.set_status(group_settle_busy()
                                         ? "Undo bloqueado (asentando grupo async)"
                                         : (async_remesh_backlog() > 24
                                                ? "Undo en pausa: terminando remallas en segundo plano…"
                                                : "Undo bloqueado (modo activo o marquee)"));
                } else {
                    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    auto flash_undo_toast = [&](const char* title, const std::string& action) {
                        undo_toast_title = title;
                        undo_toast_detail = action.empty() ? "(sin nombre)" : action;
                        undo_toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                        panel.set_status(undo_toast_title + ": " + undo_toast_detail);
                    };
                    if (shift) {
                        if (!redo_stack.empty()) {
                            const std::size_t redo_before = redo_stack.size();
                            const std::string redo_snap = snap_brief(redo_stack.back());
                            const std::string redo_action = redo_stack.back().action;
                            undo_stack.push_back(capture_editor_snapshot());
                            undo_stack.back().action = redo_action.empty() ? "rehacer" : redo_action;
                            undo_stack.back().focus_spline_ids = redo_stack.back().focus_spline_ids;
                            undo_stack.back().focus_object_ids = redo_stack.back().focus_object_ids;
                            if (kMaxUndo > 0 && undo_stack.size() > kMaxUndo)
                                undo_stack.erase(undo_stack.begin());
                            editor_log::event("UNDO_PUSH",
                                              "redo_branch_current | " + session_state() + " snap=" +
                                                  snap_brief(undo_stack.back()));
                            auto next = std::move(redo_stack.back());
                            redo_stack.pop_back();
                            // Rellenar cadena: soltar tracking de temps antes del snapshot
                            // (GPU de ids removidos lo limpia apply_editor_snapshot).
                            fill_temp_ids.clear();
                            fill_temp_pool.clear();
                            // Tras undo/redo: cancelar grab de nodo y caches de multi-end
                            // para no tocar ids/indices que el snapshot ya no tiene.
                            if (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight) {
                                log::info("[nodo] undo/redo: cancel grab target=" +
                                          std::to_string(spline_grab_target));
                                edit_mode = EditMode::None;
                            }
                            spline_grab_target = 0;
                            node_starter_ids.clear();
                            node_starter_fex.clear();
                            node_starter_fey.clear();
                            node_starter_feh.clear();
                            node_ender_ids.clear();
                            node_ender_sx.clear();
                            node_ender_sy.clear();
                            node_ender_sz.clear();
                            node_ender_sh.clear();
                            node_sli_lock_ids.clear();
                            node_sli_lock_paths.clear();
                            node_joint0_valid = false;
                            chain_fix_next_valid = false;
                            apply_editor_snapshot_safe(std::move(next));
                            editor_sfx.play(editor::SfxCue::UndoWhoosh, 0.f, 1.04f, 0.55f);
                            flash_undo_toast("Rehacer", redo_action);
                            editor_log::event("REDO_OK", "redo_before=" + std::to_string(redo_before) + "->" +
                                                              std::to_string(redo_stack.size()) + " applied=" +
                                                              redo_snap + " | " + session_state());
                            log::info("editor redo");
                        } else {
                            editor_log::event("REDO_EMPTY", session_state());
                            panel.set_status("Redo: vacio");
                        }
                    } else if (!undo_stack.empty()) {
                        const std::size_t undo_before = undo_stack.size();
                        const std::string undo_snap = snap_brief(undo_stack.back());
                        const std::string undo_action = undo_stack.back().action;
                        redo_stack.push_back(capture_editor_snapshot());
                        redo_stack.back().action = undo_action.empty() ? "deshacer" : undo_action;
                        // Same entities the undo will reverse — redo must remesh only those too.
                        redo_stack.back().focus_spline_ids = undo_stack.back().focus_spline_ids;
                        redo_stack.back().focus_object_ids = undo_stack.back().focus_object_ids;
                        editor_log::event("REDO_PUSH",
                                          "from undo branch snap=" + snap_brief(redo_stack.back()) + " | " +
                                              session_state());
                        auto prev = std::move(undo_stack.back());
                        undo_stack.pop_back();
fill_temp_ids.clear();
                        fill_temp_pool.clear();
                        if (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight) {
                            log::info("[nodo] undo/redo: cancel grab target=" +
                                      std::to_string(spline_grab_target));
                            edit_mode = EditMode::None;
                        }
                        spline_grab_target = 0;
                        node_starter_ids.clear();
                        node_starter_fex.clear();
                        node_starter_fey.clear();
                        node_starter_feh.clear();
                        node_ender_ids.clear();
                        node_ender_sx.clear();
                        node_ender_sy.clear();
                        node_ender_sz.clear();
                        node_ender_sh.clear();
                        node_sli_lock_ids.clear();
                        node_sli_lock_paths.clear();
                        node_joint0_valid = false;
                        chain_fix_next_valid = false;
                        apply_editor_snapshot_safe(std::move(prev));
                        // Clear transient node/handle state that may reference ids removed by undo.
                        chain_sel_handle_kind = 0;
                        chain_sel_handle_spline = 0;
                        chain_sel_handle_partner = 0;
                        node_click_suppress_body_pick = false;
                        node_height_mode = false;
                        grab_spline_id = 0;
                        node_grab_snap_valid = false;
                        if (selected_spline < 0 || static_cast<std::size_t>(selected_spline) >= edit_splines.size())
                            selected_spline = -1;
                        if (selected_object < 0 || static_cast<std::size_t>(selected_object) >= edit_objects.size())
                            selected_object = -1;
                        if (sco_path_edit.sel >= 0 &&
                            static_cast<std::size_t>(sco_path_edit.sel) < sco_path_edit.paths.size())
                            sco_path_edit.select_only(sco_path_edit.sel);
                        else
                            sco_path_edit.select_only(-1);
                        editor_sfx.play(editor::SfxCue::UndoWhoosh, 0.f, 1.f, 0.55f);
                        flash_undo_toast("Deshacer", undo_action);
                        editor_log::event("UNDO_OK", "undo_before=" + std::to_string(undo_before) + "->" +
                                                           std::to_string(undo_stack.size()) + " applied=" +
                                                           undo_snap + " | " + session_state());
                        log::info("editor undo");
                    } else {
                        editor_log::event("UNDO_EMPTY", session_state());
                        panel.set_status("Undo: vacio");
                    }
                }
            } else if (z_async && !z_undo_was && !ctrl) {
                editor_log::event("KEY_Z", "Z without Ctrl (ignored for undo)");
            }
            z_undo_was = z_async && ctrl;
            z_was = z_now;
            const bool y_async = editor_keys_live && (GetAsyncKeyState('Y') & 0x8000) != 0;
            static bool y_redo_was = false;
            if (y_async && !y_redo_was && ctrl && !alt) {
                if (edit_mode == EditMode::Mesh) {
                    mesh_redo_face();
                } else if (view_only) {
                    panel.set_status("Modo solo visualización — Ctrl+Y bloqueado");
                } else if (panel.current_tool() == ui::EditorTool::TracksTrips ||
                           panel.current_tool() == ui::EditorTool::StationLinks) {
                    // Tracks & Trips / Station Links: undo the last edit of the TTData (memory only, saved with Save).
                    auto& tm = omsi::tt::global_tt_man();
                    tm.track_changes();
                    const bool ok = tm.redo();
                    panel.set_status(ok ? "TT: rehacer" : "TT: nada que rehacer");
                    if (ok) panel.set_tt_from_man();
                } else if (edit_mode != EditMode::None || marquee.phase != MarqueePhase::None ||
                           group_settle_busy() || async_remesh_backlog() > 24) {
                    panel.set_status(group_settle_busy() ? "Redo bloqueado (asentando grupo async)"
                                                         : (async_remesh_backlog() > 24
                                                                ? "Redo en pausa: terminando remallas en segundo plano..."
                                                                : "Redo bloqueado (modo activo o marquee)"));
                } else if (!redo_stack.empty()) {
                    const std::size_t redo_before = redo_stack.size();
                    const std::string redo_snap = snap_brief(redo_stack.back());
                    const std::string redo_action = redo_stack.back().action;
                    undo_stack.push_back(capture_editor_snapshot());
                    undo_stack.back().action = redo_action.empty() ? "rehacer" : redo_action;
                    undo_stack.back().focus_spline_ids = redo_stack.back().focus_spline_ids;
                    undo_stack.back().focus_object_ids = redo_stack.back().focus_object_ids;
                    if (kMaxUndo > 0 && undo_stack.size() > kMaxUndo) undo_stack.erase(undo_stack.begin());
                    auto next = std::move(redo_stack.back());
                    redo_stack.pop_back();
                    fill_temp_ids.clear();
                    fill_temp_pool.clear();
                    if (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight) {
                        log::info("[nodo] undo/redo: cancel grab target=" +
                                  std::to_string(spline_grab_target));
                        edit_mode = EditMode::None;
                    }
                    spline_grab_target = 0;
                    node_starter_ids.clear();
                    node_starter_fex.clear();
                    node_starter_fey.clear();
                    node_starter_feh.clear();
                    node_ender_ids.clear();
                    node_ender_sx.clear();
                    node_ender_sy.clear();
                    node_ender_sz.clear();
                    node_ender_sh.clear();
                    node_sli_lock_ids.clear();
                    node_sli_lock_paths.clear();
                    node_joint0_valid = false;
                    chain_fix_next_valid = false;
                    apply_editor_snapshot_safe(std::move(next));
                    chain_sel_handle_kind = 0;
                    chain_sel_handle_spline = 0;
                    chain_sel_handle_partner = 0;
                    node_click_suppress_body_pick = false;
                    node_height_mode = false;
                    grab_spline_id = 0;
                    node_grab_snap_valid = false;
                    if (selected_spline < 0 || static_cast<std::size_t>(selected_spline) >= edit_splines.size())
                        selected_spline = -1;
                    if (selected_object < 0 || static_cast<std::size_t>(selected_object) >= edit_objects.size())
                        selected_object = -1;
                    if (sco_path_edit.sel >= 0 &&
                        static_cast<std::size_t>(sco_path_edit.sel) < sco_path_edit.paths.size())
                        sco_path_edit.select_only(sco_path_edit.sel);
                    else
                        sco_path_edit.select_only(-1);
                    editor_sfx.play(editor::SfxCue::UndoWhoosh, 0.f, 1.04f, 0.55f);
                    undo_toast_title = "Rehacer";
                    undo_toast_detail = redo_action.empty() ? "(sin nombre)" : redo_action;
                    undo_toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    panel.set_status(undo_toast_title + ": " + undo_toast_detail);
                    editor_log::event("REDO_OK", "ctrl+y redo_before=" + std::to_string(redo_before) + "->" +
                                                       std::to_string(redo_stack.size()) + " applied=" + redo_snap +
                                                       " | " + session_state());
                    log::info("editor redo (Ctrl+Y)");
                } else {
                    panel.set_status("Redo: vacio");
                }
            }
            y_redo_was = y_async && ctrl;
        }
        main_watchdog.mark("M.dispatch.L02_1326");
        {
            static bool s_was = false;
            const bool s_now = hotkey_down(GLFW_KEY_S, 'S');
            const bool ctrl_s = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (s_now && !s_was && ctrl_s &&
                (edit_mode == EditMode::None || edit_mode == EditMode::Mesh)) {
                if (view_only) {
                    panel.set_status("Modo solo visualización — Ctrl+S bloqueado");
                    editor_log::event("VIEW_ONLY_BLOCK", "Ctrl+S");
                } else if (save_job_active) {
                    panel.set_status("Guardando... (ya en curso)");
                } else if (stream_busy()) {
                    panel.set_status("Ctrl+S bloqueado (streaming)");
                } else {
                    begin_save_job();
                }
            } else if (s_now && !s_was && edit_mode == EditMode::Mesh && !ctrl_s &&
                       !ImGui::GetIO().WantTextInput) {
                if (mesh_edit.uv_editor_open && (mesh_edit.uv_win_focused || mesh_edit.uv_canvas_hov))
                    mesh_uv_g_begin(2);
                else if (!mesh_edit.uv_g)
                    mesh_xform_begin(2);
            }
            s_was = s_now;
        }
        {
            // R/RR must ignore ImGui HUD focus (typing_ui): second R was eaten by the adjust window.
            const bool r_trim = editor_shortcuts.pressed(ui::EditorAction::TrimSpline);
            const auto rot_ch = editor_shortcuts.chords[static_cast<int>(ui::EditorAction::Rotate)];
            const auto fol_ch = editor_shortcuts.chords[static_cast<int>(ui::EditorAction::SplineFollow)];
            const bool follow_same_as_rotate =
                rot_ch.vk == fol_ch.vk && rot_ch.ctrl == fol_ch.ctrl && rot_ch.shift == fol_ch.shift &&
                rot_ch.alt == fol_ch.alt;
            const bool r_follow = !follow_same_as_rotate && editor_shortcuts.pressed(ui::EditorAction::SplineFollow);
            const bool r_rot = editor_shortcuts.pressed(ui::EditorAction::Rotate);
            const bool r_now = r_trim || r_rot || r_follow;
            auto enter_spline_follow = [&]() {
                const bool have_sel = selected_spline >= 0 &&
                                      static_cast<std::size_t>(selected_spline) < edit_splines.size();
                const bool have_group = !group_sel_spl.empty();
                if (edit_mode != EditMode::Rotate || (!have_sel && !have_group))
                    return false;
                auto restore_prev_skew = [&](std::int32_t prev_id, float prev_skew_e) {
                    if (prev_id == 0) return;
                    for (auto& p : edit_splines) {
                        if (p.id != prev_id) continue;
                        p.skew_end = prev_skew_e;
                        remesh_spline_one(p, true, false);
                        break;
                    }
                };
                if (group_rotate_active && !group_grab_index.empty()) {
                    for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                        const int gi = group_grab_index[i];
                        if (gi < 0 || i >= group_grab_kind.size() || i >= group_off_x.size() ||
                            i >= group_off_y.size())
                            continue;
                        if (group_grab_kind[i] == 0) {
                            if (static_cast<std::size_t>(gi) >= edit_objects.size()) continue;
                            auto& o = edit_objects[static_cast<std::size_t>(gi)];
                            o.position.x = group_pivot_x + group_off_x[i];
                            o.position.y = group_pivot_y + group_off_y[i];
                            if (i < group_grab_start_z.size()) o.position.z = group_grab_start_z[i];
                            if (i < group_start_yaw.size()) o.rotation_deg.x = group_start_yaw[i];
                            sync_object_gpu(o);
                        } else {
                            if (static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                            auto& sp = edit_splines[static_cast<std::size_t>(gi)];
                            sp.position.x = group_pivot_x + group_off_x[i];
                            sp.position.y = group_pivot_y + group_off_y[i];
                            if (i < group_grab_start_z.size()) sp.position.z = group_grab_start_z[i];
                            if (i < group_start_yaw.size()) sp.heading_deg = group_start_yaw[i];
                            if (i < group_start_skew_s.size()) sp.skew_start = group_start_skew_s[i];
                            if (i < group_start_skew_e.size()) sp.skew_end = group_start_skew_e[i];
                            if (i < group_start_prev_id.size() && i < group_start_prev_skew_e.size())
                                restore_prev_skew(group_start_prev_id[i], group_start_prev_skew_e[i]);
                            remesh_spline(sp, true);
                        }
                    }
                    group_rot_have_ref = false;
                    if (!have_sel && have_group) selected_spline = group_sel_spl.front();
                } else if (have_sel) {
                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    s.heading_deg = grab_start_yaw;
                    s.length = grab_start_len;
                    s.radius = grab_start_radius;
                    s.skew_start = grab_start_skew_s;
                    s.skew_end = grab_start_skew_e;
                    restore_prev_skew(grab_start_prev_id, grab_start_prev_skew_e);
                    remesh_spline(s, true);
                } else if (have_group) {
                    selected_spline = group_sel_spl.front();
                }
                group_rotate_active = false;
                edit_mode = EditMode::SplineFollow;
                rotating = false;
                grabbing = true;
                r_tap = 2;
                log::info("editor spline SKELETON (lines only) — move len/radius, LMB=place");
                panel.set_status(have_group && group_sel_spl.size() > 1
                                     ? "RR grupo: esqueleto (radio/largo en todas)"
                                     : "RR: esqueleto lineas/puntos (ajustá radio/largo)");
                return true;
            };
            if (edit_mode == EditMode::Mesh && r_trim && !r_was && !ImGui::GetIO().WantTextInput) {
                mesh_knife_cancel();
                mesh_edit.loopcut_on = !mesh_edit.loopcut_on;
                panel.set_status(mesh_edit.loopcut_on ? "Loop cut Ctrl+R: rueda/cortes, LMB aplica, Esc cancela"
                                                      : "Loop cut OFF");
            } else if (edit_mode == EditMode::Mesh && r_now && !r_was && !r_trim && !ImGui::GetIO().WantTextInput) {
                if (mesh_edit.uv_editor_open && (mesh_edit.uv_win_focused || mesh_edit.uv_canvas_hov))
                    mesh_uv_g_begin(1);
                else if (!mesh_edit.uv_g)
                    mesh_xform_begin(1);
            } else if (!view_only && r_now && sco_path_edit.active && sco_path_edit.sel >= 0 &&
                selected_spline < 0 && edit_mode != EditMode::Mesh) {
                auto& p = sco_path_edit.paths[static_cast<std::size_t>(sco_path_edit.sel)];
                if (edit_mode == EditMode::Rotate || sco_path_edit.rotating) {
                    p.heading_deg = sco_path_edit.grab_hdg;
                    sco_path_edit.rotating = false;
                    sco_path_edit.following = true;
                    sco_path_edit.moving = false;
                    sco_path_edit.grab_len = p.length;
                    sco_path_edit.grab_radius = p.radius;
                    edit_mode = EditMode::SplineFollow;
                    grabbing = true;
                    rotating = false;
                    r_tap = 2;
                    rebuild_one_object_path_rail(sco_path_edit.object_id, sco_path_edit.sel, true);
                    panel.set_status("RR path .sco radio/largo (wire)");
                } else if (edit_mode == EditMode::None) {
                    push_undo("sco_path_r");
                    sco_path_edit.rotating = true;
                    sco_path_edit.moving = false;
                    sco_path_edit.following = false;
                    sco_path_edit.grab_lx = p.x;
                    sco_path_edit.grab_ly = p.y;
                    sco_path_edit.grab_hdg = p.heading_deg;
                    edit_mode = EditMode::Rotate;
                    rotating = true;
                    r_tap = 1;
                    sco_path_arm_live();
                    panel.set_status("R path .sco");
                }
            } else if (view_only && r_now) {
                panel.set_status("Modo solo visualización — R/Ctrl+R bloqueado");
                editor_log::event("VIEW_ONLY_BLOCK", "R rotate/trim");
            } else if (r_trim && edit_mode == EditMode::None && selected_spline >= 0 &&
                static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                edit_mode = EditMode::TrimSpline;
                grabbing = true;
                const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                // Snapshot pose so any accidental fall-through restore cannot jump the spline.
                grab_start_x = s.position.x;
                grab_start_y = s.position.y;
                grab_start_z = s.position.z;
                grab_start_yaw = s.heading_deg;
                grab_start_len = s.length;
                grab_start_radius = s.radius;
                trim_along = grab_start_len * 0.5f;
                trim_cuts.clear();
                update_trim_preview(s, trim_along);
                panel.set_status("Ctrl+R: linea naranja = corte (LMB=split, no borra; Esc=cancela)");
                log::info("editor TRIM spline id=" + std::to_string(s.id));
            } else if ((r_follow || r_rot) && enter_spline_follow()) {
            } else if (r_rot) {
                if (edit_mode == EditMode::None && (!group_sel_obj.empty() || !group_sel_spl.empty())) {
                    push_undo("group_rotate_start");
                    group_rot_have_ref = false;
                    group_grab_active = false;
                    group_grab_kind.clear();
                    group_grab_index.clear();
                    group_grab_ids.clear();
                    group_off_x.clear();
                    group_off_y.clear();
                    group_off_z.clear();
                    group_grab_start_z.clear();
                    group_start_yaw.clear();
                    group_start_skew_s.clear();
                    group_start_skew_e.clear();
                    group_start_prev_id.clear();
                    group_start_prev_skew_e.clear();
                    group_pivot_x = 0.f;
                    group_pivot_y = 0.f;
                    int pivot_n = 0;
                    for (int i : group_sel_obj) {
                        const auto& o = edit_objects[static_cast<std::size_t>(i)];
                        group_pivot_x += o.position.x;
                        group_pivot_y += o.position.y;
                        ++pivot_n;
                    }
                    for (int i : group_sel_spl) {
                        const auto& s = edit_splines[static_cast<std::size_t>(i)];
                        group_pivot_x += s.position.x;
                        group_pivot_y += s.position.y;
                        ++pivot_n;
                    }
                    if (pivot_n > 0) {
                        group_pivot_x /= static_cast<float>(pivot_n);
                        group_pivot_y /= static_cast<float>(pivot_n);
                    }
                    for (int i : group_sel_obj) {
                        const auto& o = edit_objects[static_cast<std::size_t>(i)];
                        group_grab_kind.push_back(0);
                        group_grab_index.push_back(i);
                        group_grab_ids.push_back(o.id);
                        group_off_x.push_back(o.position.x - group_pivot_x);
                        group_off_y.push_back(o.position.y - group_pivot_y);
                        group_grab_start_z.push_back(o.position.z);
                        group_start_yaw.push_back(o.rotation_deg.x);
                        group_start_skew_s.push_back(0.f);
                        group_start_skew_e.push_back(0.f);
                        group_start_prev_id.push_back(0);
                        group_start_prev_skew_e.push_back(0.f);
                        for (auto& sp : scene_places) {
                            if (sp.object_id == o.id) sp.pinned = true;
                        }
                    }
                    for (int i : group_sel_spl) {
                        const auto& s = edit_splines[static_cast<std::size_t>(i)];
                        group_grab_kind.push_back(1);
                        group_grab_index.push_back(i);
                        group_grab_ids.push_back(s.id);
                        group_off_x.push_back(s.position.x - group_pivot_x);
                        group_off_y.push_back(s.position.y - group_pivot_y);
                        group_grab_start_z.push_back(s.position.z);
                        group_start_yaw.push_back(s.heading_deg);
                        group_start_skew_s.push_back(s.skew_start);
                        group_start_skew_e.push_back(s.skew_end);
                        group_start_prev_id.push_back(s.prev_id);
                        float prev_se = 0.f;
                        if (s.prev_id != 0) {
                            for (const auto& p : edit_splines) {
                                if (p.id == s.prev_id) {
                                    prev_se = p.skew_end;
                                    break;
                                }
                            }
                        }
                        group_start_prev_skew_e.push_back(prev_se);
                        if (auto* g = find_spline_gpu(s.id)) g->pinned = true;
                    }
                    if (!group_grab_index.empty()) {
                        group_rotate_active = true;
                        edit_mode = EditMode::Rotate;
                        rotating = true;
                        grabbing = false;
                        r_tap = 1;
                        if (!group_start_yaw.empty()) grab_start_yaw = group_start_yaw.front();
                        capture_hud_group_pose();
                    }
                    panel.set_status("Grupo R: rotar (Ctrl=90), LMB=confirmar Esc=cancelar");
                    multisel_log_state("rotate_start");
                    log::info("editor group rotate count=" + std::to_string(group_grab_index.size()));
                } else if (edit_mode == EditMode::None && selected_spline >= 0 &&
                    static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    push_undo("rotate_start");
                    maybe_unlink_on_free_move(s.id);
                    edit_mode = EditMode::Rotate;
                    rotating = true;
                    grabbing = false;
                    r_tap = 1;
                    grab_start_x = s.position.x;
                    grab_start_y = s.position.y;
                    grab_start_z = s.position.z;
                    grab_start_yaw = s.heading_deg;
                    grab_start_len = s.length;
                    grab_start_radius = s.radius;
                    grab_start_skew_s = s.skew_start;
                    grab_start_skew_e = s.skew_end;
                    grab_start_prev_id = s.prev_id;
                    grab_start_prev_skew_e = 0.f;
                    if (s.prev_id != 0) {
                        for (const auto& p : edit_splines) {
                            if (p.id == s.prev_id) {
                                grab_start_prev_skew_e = p.skew_end;
                                break;
                            }
                        }
                    }
                    log::info("editor spline ROTATE (Ctrl=90deg snap from current yaw)");
                    panel.set_status("R: rotar (Ctrl=pasos 90 desde yaw actual)");
                } else if (edit_mode == EditMode::None && selected_object >= 0) {
                    push_undo("rotate_start");
                    edit_mode = EditMode::Rotate;
                    rotating = true;
                    grabbing = false;
                    const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                    grab_start_x = o.position.x;
                    grab_start_y = o.position.y;
                    grab_start_z = o.position.z;
                    grab_start_yaw = o.rotation_deg.x;
                    log::info("editor rotate yaw (LMB=place Esc=cancel)");
                }
            }
            r_was = r_now;
        }
        {
            static bool d_was = false;
            const bool d_now = hotkey_down(GLFW_KEY_D, 'D');
            const bool shift_d = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (view_only && d_now && !d_was && shift_d) {
                panel.set_status("Modo solo visualización — Shift+D (clonar) bloqueado");
                editor_log::event("VIEW_ONLY_BLOCK", "Shift+D clone");
            } else if (d_now && !d_was && edit_mode == EditMode::Mesh) {
                const bool uv_hot = mesh_edit.uv_editor_open &&
                                    (mesh_edit.uv_win_focused || mesh_edit.uv_canvas_hov);
                if (!uv_hot) {
                    const bool alt_d = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                    if (alt_d) mesh_duplicate(true);
                    else if (shift_d) mesh_duplicate(false);
                }
            } else if (d_now && !d_was && sco_path_edit.active && !shift_d && edit_mode == EditMode::None) {
                sco_clone_sel_path();
            } else if (d_now && !d_was && shift_d && edit_mode == EditMode::None && marquee.phase == MarqueePhase::None &&
                !over_ui) {
                marquee.phase = MarqueePhase::Select;
                marquee.intent = MarqueeIntent::Clone;
                marquee.dragging = false;
                panel.set_status("Shift+D: arrastra rectangulo, suelta LMB=clonar");
                log::info("editor marquee select (Shift+D)");
            }
            d_was = d_now;
        }
        {
            static bool e_was = false, i_was = false, l_was = false, x_was = false, yk_was = false, z_was = false,
                        n_mesh_was = false, vp_was = false, fp_was = false, k_was = false;
            const bool e_now = hotkey_down(GLFW_KEY_E, 'E');
            const bool i_now = hotkey_down(GLFW_KEY_I, 'I');
            const bool l_now = hotkey_down(GLFW_KEY_L, 'L');
            const bool x_now = hotkey_down(GLFW_KEY_X, 'X');
            const bool yk_now = hotkey_down(GLFW_KEY_Y, 'Y');
            const bool z_axis = hotkey_down(GLFW_KEY_Z, 'Z');
            const bool n_now_mesh = hotkey_down(GLFW_KEY_N, 'N');
            const bool v_pick = hotkey_down(GLFW_KEY_V, 'V');
            const bool f_pick = hotkey_down(GLFW_KEY_F, 'F');
            const bool k_now = hotkey_down(GLFW_KEY_K, 'K');
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (edit_mode == EditMode::Mesh && !ImGui::GetIO().WantTextInput) {
                const bool uv_hot = mesh_edit.uv_editor_open &&
                                    (mesh_edit.uv_win_focused || mesh_edit.uv_canvas_hov);
                if (v_pick && !vp_was && !ctrl) {
                    mesh_edit.pick = MeshEdit::Pick::Verts;
                    mesh_hover_clear();
                    panel.set_status(uv_hot ? "UV snap/sel: verts" : "Pick verts");
                }
                if (f_pick && !fp_was && !ctrl) {
                    mesh_edit.pick = MeshEdit::Pick::Faces;
                    mesh_hover_clear();
                    panel.set_status(uv_hot ? "UV snap/sel: faces" : "Pick faces");
                }
                if (e_now && !e_was) {
                    if (uv_hot) {
                        mesh_edit.pick = MeshEdit::Pick::Edges;
                        mesh_hover_clear();
                        panel.set_status("UV snap/sel: edges");
                    } else
                        mesh_extrude_region();
                }
                if (i_now && !i_was) {
                    if (ctrl) {
                        mesh_sel_cap();
                        auto inv = [&](std::unordered_set<int>& s, int n) {
                            std::unordered_set<int> ns;
                            for (int i = 0; i < n; ++i)
                                if (!s.count(i)) ns.insert(i);
                            s.swap(ns);
                        };
                        if (mesh_edit.pick == MeshEdit::Pick::Verts)
                            inv(mesh_edit.sel_verts, static_cast<int>(mesh_edit.verts.size()));
                        else if (mesh_edit.pick == MeshEdit::Pick::Edges)
                            inv(mesh_edit.sel_edges, static_cast<int>(mesh_edit.edges.size()));
                        else
                            inv(mesh_edit.sel_faces, static_cast<int>(mesh_edit.faces.size()));
                    } else if (!mesh_xform.active) {
                        mesh_inset();
                    }
                }
                if (l_now && !l_was && !uv_hot) mesh_select_linked();
                {
                    // Blender: 1 / 2 / 3 = vertex / edge / face select mode (top-row digits, not numpad).
                    static bool d1_was = false, d2_was = false, d3_was = false, zs_was = false;
                    const bool d1 = hotkey_down(GLFW_KEY_1, '1'), d2 = hotkey_down(GLFW_KEY_2, '2'),
                               d3 = hotkey_down(GLFW_KEY_3, '3');
                    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                    auto set_pick = [&](MeshEdit::Pick pk, const char* name) {
                        mesh_edit.pick = pk;
                        mesh_hover_clear();
                        panel.set_status(name);
                    };
                    if (!ctrl && !alt && !mesh_xform.active) {
                        if (d1 && !d1_was) set_pick(MeshEdit::Pick::Verts, "1: vertices");
                        if (d2 && !d2_was) set_pick(MeshEdit::Pick::Edges, "2: edges");
                        if (d3 && !d3_was) set_pick(MeshEdit::Pick::Faces, "3: caras");
                    }
                    d1_was = d1;
                    d2_was = d2;
                    d3_was = d3;
                    // Z = next viewport shading (Render > Solid > Wireframe), Shift+Z = wireframe on/off.
                    // Not while Z picks an axis (G/R/S, knife, UV move).
                    const bool zs = z_axis && !ctrl && !alt && !mesh_xform.active && !mesh_edit.knife_on &&
                                    !mesh_edit.uv_g && !uv_hot;
                    if (zs && !zs_was) {
                        static int s_prev_shading = 2;
                        if (shift) {
                            if (mesh_edit.shading == 1) mesh_edit.shading = s_prev_shading == 1 ? 2 : s_prev_shading;
                            else {
                                s_prev_shading = mesh_edit.shading;
                                mesh_edit.shading = 1;
                            }
                        } else {
                            mesh_edit.shading = mesh_edit.shading == 2 ? 0 : mesh_edit.shading == 0 ? 1 : 2;
                        }
                        mesh_refresh_preview();
                        panel.set_status(mesh_edit.shading == 0   ? "Vista: Solid"
                                         : mesh_edit.shading == 1 ? "Vista: Wireframe"
                                                                  : "Vista: Render (texturas)");
                    }
                    zs_was = zs;
                }
                if (n_now_mesh && !n_mesh_was && shift && !ctrl && !uv_hot) mesh_recalc_n();
                if (k_now && !k_was && !ctrl && !uv_hot && !mesh_xform.active) {
                    mesh_loopcut_cancel();
                    mesh_edit.knife_on = !mesh_edit.knife_on;
                    if (!mesh_edit.knife_on) mesh_knife_cancel();
                    else {
                        mesh_edit.knife_axis = -1;
                        mesh_edit.knife_pts.clear();
                    }
                    panel.set_status(mesh_edit.knife_on ? "Knife K: click puntos, X/Y/Z eje, Enter corta"
                                                        : "Knife OFF");
                }
                if (yk_now && !yk_was && !ctrl && !mesh_xform.active && !uv_hot && !mesh_edit.uv_g &&
                    !mesh_edit.loopcut_on && !mesh_edit.knife_on)
                    mesh_split_sel();
                if (mesh_edit.knife_on) {
                    const bool x_edge = x_now && !x_was;
                    const bool y_edge = yk_now && !yk_was && !ctrl;
                    const bool z_edge = z_axis && !z_was && !ctrl;
                    if (x_edge) {
                        mesh_edit.knife_axis = mesh_edit.knife_axis == 0 ? -1 : 0;
                        panel.set_status(mesh_edit.knife_axis == 0 ? "Knife  X" : "Knife");
                    } else if (y_edge) {
                        mesh_edit.knife_axis = mesh_edit.knife_axis == 1 ? -1 : 1;
                        panel.set_status(mesh_edit.knife_axis == 1 ? "Knife  Y" : "Knife");
                    } else if (z_edge) {
                        mesh_edit.knife_axis = mesh_edit.knife_axis == 2 ? -1 : 2;
                        panel.set_status(mesh_edit.knife_axis == 2 ? "Knife  Z" : "Knife");
                    }
                    const bool enter_now =
                        glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS ||
                        glfwGetKey(window, GLFW_KEY_KP_ENTER) == GLFW_PRESS;
                    static bool knife_enter_was = false;
                    if (enter_now && !knife_enter_was) mesh_knife_confirm();
                    knife_enter_was = enter_now;
                } else if (mesh_edit.uv_g && mesh_edit.uv_xf_mode == 0) {
                    const bool x_edge = x_now && !x_was;
                    const bool y_edge = yk_now && !yk_was && !ctrl;
                    if (x_edge) {
                        mesh_edit.uv_axis = mesh_edit.uv_axis == 0 ? -1 : 0;
                        panel.set_status(mesh_edit.uv_axis == 0 ? "UV G  X" : "UV G");
                    } else if (y_edge) {
                        mesh_edit.uv_axis = mesh_edit.uv_axis == 1 ? -1 : 1;
                        panel.set_status(mesh_edit.uv_axis == 1 ? "UV G  Y" : "UV G");
                    }
                } else if (mesh_edit.uv_g && mesh_edit.uv_xf_mode == 2) {
                    const bool x_edge = x_now && !x_was;
                    const bool y_edge = yk_now && !yk_was && !ctrl;
                    if (x_edge) mesh_uv_scale_toggle_axis(0);
                    else if (y_edge) mesh_uv_scale_toggle_axis(1);
                } else if (mesh_xform.active && mesh_xform.mode == 2) {
                    const bool x_edge = x_now && !x_was;
                    const bool y_edge = yk_now && !yk_was && !ctrl;
                    const bool z_edge = z_axis && !z_was && !ctrl;
                    if (x_edge) mesh_scale_toggle_axis(0);
                    else if (y_edge) mesh_scale_toggle_axis(1);
                    else if (z_edge) mesh_scale_toggle_axis(2);
                } else if (mesh_xform.active) {
                    const bool x_edge = x_now && !x_was;
                    const bool y_edge = yk_now && !yk_was && !ctrl;
                    const bool z_edge = z_axis && !z_was && !ctrl;
                    if (x_edge) {
                        mesh_xform.axis = mesh_xform.axis == 0 ? -1 : 0;
                        mesh_xform_status_axis();
                    } else if (y_edge) {
                        mesh_xform.axis = mesh_xform.axis == 1 ? -1 : 1;
                        mesh_xform_status_axis();
                    } else if (z_edge) {
                        mesh_xform.axis = mesh_xform.axis == 2 ? -1 : 2;
                        mesh_xform_status_axis();
                    }
                }
                const bool del_now = editor_keys_live && (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
                static bool delm_was = false;
                if (del_now && !delm_was) mesh_delete_sel();
                delm_was = del_now;
                static bool kp_add_was = false, kp_sub_was = false;
                const bool kp_add = editor_keys_live &&
                                    ((GetAsyncKeyState(VK_ADD) & 0x8000) != 0 ||
                                     (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0);
                const bool kp_sub = editor_keys_live &&
                                    ((GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0 ||
                                     (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0);
                if (kp_add && !kp_add_was && ctrl) mesh_select_more();
                if (kp_sub && !kp_sub_was && ctrl) mesh_select_less();
                kp_add_was = kp_add;
                kp_sub_was = kp_sub;
            }
            e_was = e_now;
            i_was = i_now;
            l_was = l_now;
            x_was = x_now;
            yk_was = yk_now;
            z_was = z_axis;
            n_mesh_was = n_now_mesh;
            vp_was = v_pick;
            fp_was = f_pick;
            k_was = k_now;
        }
        main_watchdog.mark("M.dispatch.L02_1806");
        {
            static bool shift_s_marquee_was = false;
            const bool s_key = hotkey_down(GLFW_KEY_S, 'S');
            const bool shift_s = s_key && ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
            const bool ctrl_s = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (s_key && !shift_s_marquee_was && shift_s && !ctrl_s && edit_mode == EditMode::None &&
                marquee.phase == MarqueePhase::None && !over_ui) {
                marquee.phase = MarqueePhase::Select;
                marquee.intent = MarqueeIntent::SelectOnly;
                marquee.dragging = false;
                panel.set_status("Shift+S: arrastra rectangulo para seleccionar grupo (G/Z/R)");
                log::info("editor group marquee (Shift+S)");
                editor_log::event("SHIFT_S_ARM", session_state());
            }
            shift_s_marquee_was = s_key && shift_s && !ctrl_s;
        }
        {
            const bool n_now = hotkey_down(GLFW_KEY_N, 'N');
            if (n_now && !n_was && view_only) {
                panel.set_status("Modo solo visualización — N (crear/continuar) bloqueado");
                editor_log::event("VIEW_ONLY_BLOCK", "N place");
            } else if (n_now && !n_was && sco_path_edit.active && edit_mode == EditMode::None) {
                sco_start_new_path();
            } else if (n_now && !n_was && edit_mode == EditMode::None) {
                const bool spline_tool = ev.tool == ui::EditorTool::Spline;
                const bool object_tool = ev.tool == ui::EditorTool::Object;
                const bool shift = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ||
                                   (glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
                if (!shift && group_sel_spl.size() > 1 &&
                    (spline_tool || selected_spline >= 0 || !object_tool)) {
                    push_undo("n_continue_group");
                    std::vector<int> srcs = group_sel_spl;
                    std::vector<int> news;
                    std::vector<std::int32_t> news_ids;
                    for (int src_i : srcs) {
                        if (src_i < 0 || static_cast<std::size_t>(src_i) >= edit_splines.size()) continue;
                        const auto& src = edit_splines[static_cast<std::size_t>(src_i)];
                        const auto end = spline::evaluate_spline(src, src.length);
                        map::SplineInst draft = src;
                        draft.id = next_spline_id();
                        draft.next_id = 0;
                        draft.prev_id = (src.next_id == 0) ? src.id : 0;
                        draft.position.x = end.x;
                        draft.position.y = end.y;
                        draft.position.z = end.z;
                        draft.heading_deg =
                            (end.heading_rad - 2.f * std::atan(src.skew_end)) * (180.f / 3.14159265f);
                        while (draft.heading_deg > 180.f) draft.heading_deg -= 360.f;
                        while (draft.heading_deg <= -180.f) draft.heading_deg += 360.f;
                        draft.skew_start = -src.skew_end;
                        draft.radius = 0.f;
                        draft.length = src.length;
                        if (draft.prev_id != 0) {
                            for (auto& ps : edit_splines) {
                                if (ps.id != draft.prev_id) continue;
                                for (auto& os : edit_splines) {
                                    if (os.id != ps.id && os.next_id == draft.id) os.next_id = 0;
                                }
                                ps.next_id = draft.id;
                                break;
                            }
                        }
                        edit_splines.push_back(draft);
                        transient_splines.push_back(draft);
                        remesh_spline(edit_splines.back());
                        const auto& s = edit_splines.back();
                        const auto a = spline::evaluate_spline(s, 0.f);
                        const auto b = spline::evaluate_spline(s, s.length);
                        place_marker(a.x, a.y, a.z + kArrowFloatZ, kArrowScale, false, s.id, true, false, 0,
                                     tile_gx, tile_gy, a.heading_rad);
                        place_marker(b.x, b.y, b.z + kArrowFloatZ, kArrowScale, false, s.id, false, false, 0,
                                     tile_gx, tile_gy, b.heading_rad);
                        persist_editor_xy(s.position.x, s.position.y);
                        if (show_path_rails) ensure_path_rails_for_owner(s.id, false);
                        news.push_back(static_cast<int>(edit_splines.size() - 1));
                        news_ids.push_back(s.id);
                    }
                    group_sel_spl = std::move(news);
                    group_sel_spl_ids = std::move(news_ids);
                    if (!group_sel_spl.empty()) selected_spline = group_sel_spl.front();
                    selected_object = -1;
                    refresh_editor_lists();
                    edit_mode = EditMode::None;
                    grabbing = false;
                    rotating = false;
                    panel.set_status("N grupo: " + std::to_string(group_sel_spl.size()) + " splines nuevas");
                } else {
                // Free create when: no selection, or Shift+N. Else continue from selected end.
                const bool free_create = shift || selected_spline < 0;

                if (spline_tool || (!object_tool && selected_spline >= 0) ||
                    (!object_tool && panel.catalog_spline_sel() >= 0)) {
                    bool have = false;
                    if (!free_create && selected_spline >= 0 &&
                        static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                        const auto& src = edit_splines[static_cast<std::size_t>(selected_spline)];
                        const auto end = spline::evaluate_spline(src, src.length);
                        new_spline_draft = src;
                        new_spline_draft.id = next_spline_id();
                        new_spline_draft.next_id = 0;
                        if (src.next_id == 0) {
                            new_spline_draft.prev_id = src.id;
                        } else {
                            new_spline_draft.prev_id = 0;
                        }
                        new_spline_draft.position.x = end.x;
                        new_spline_draft.position.y = end.y;
                        new_spline_draft.position.z = end.z;
                        new_spline_draft.heading_deg = end.heading_rad * (180.f / 3.14159265f);
                        new_spline_draft.skew_start = -src.skew_end;
                        new_spline_draft.radius = 0.f;
                        have = true;
                        log::info("editor N continue from id=" + std::to_string(src.id) +
                                  (src.next_id == 0 ? " (will link)" : " (free, parent already has next)"));
                    } else {
                        std::filesystem::path path = panel.catalog_selected_path();
                        if (path.empty()) path = sticky_sli_path;
                        if (path.empty()) {
                            for (const auto& s : edit_splines) {
                                path = s.sli_path;
                                break;
                            }
                        }
                        if (path.empty()) {
                            for (const auto& s : transient_splines) {
                                path = s.sli_path;
                                break;
                            }
                        }
                        if (!path.empty()) {
                            sticky_sli_path = path;
                            panel.remember_catalog_path(path);
                            // Start under mouse on ground (fallback: camera look target).
                            float hx = camera.target_x, hy = camera.target_y, hz = camera.target_z;
                            {
                                float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                                input::cursor_to_framebuffer(window, mx, my, fw, fh);
                                camera.ground_z = camera.target_z;
                                if (const auto hit = camera.hit_ground_xy(mx, my, fw, fh)) {
                                    hx = hit->x;
                                    hy = hit->y;
                                }
                            }
                            hz = sample_terrain_editor_xy(hx, hy);
                            new_spline_draft = {};
                            new_spline_draft.sli_path = path;
                            new_spline_draft.id = next_spline_id();
                            new_spline_draft.position.x = hx;
                            new_spline_draft.position.y = hy;
                            new_spline_draft.position.z = hz;
                            // Tip points along camera look on XY (OMSI: dir=(sin h, cos h)).
                            {
                                float ex = 0.f, ey = 0.f, ez = 0.f;
                                camera.eye_position(ex, ey, ez);
                                const float vx = camera.target_x - ex;
                                const float vy = camera.target_y - ey;
                                if (vx * vx + vy * vy > 1.e-8f)
                                    new_spline_draft.heading_deg =
                                        std::atan2(vx, vy) * (180.f / 3.14159265f);
                                else
                                    new_spline_draft.heading_deg =
                                        std::atan2(-std::cos(camera.theta), -std::sin(camera.theta)) *
                                        (180.f / 3.14159265f);
                            }
                            have = true;
                            selected_spline = -1;
                            selected_object = -1;
                            log::info("editor N free spline " + path.filename().string());
                        }
                    }
                    if (have) {
                        if (free_create) {
                            sync_spawn_height_from_ui();
                            apply_spline_follow_xy(new_spline_draft, new_spline_draft.position.x,
                                                   new_spline_draft.position.y);
                        }
                    }
                    if (have) {
                        if (selected_spline >= 0 &&
                            static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                            const auto& sel = edit_splines[static_cast<std::size_t>(selected_spline)];
                            new_spline_draft.length = sel.length;
                            if (free_create) new_spline_draft.radius = sel.radius;
                            else new_spline_draft.radius = 0.f;
                        } else {
                            new_spline_draft.length = 10.f;
                            new_spline_draft.radius = 0.f;
                        }
                        if (!free_create) {
                            // Continue from selected: place immediately (no LMB wait).
                            push_undo("n_continue_spline");
                            // Wire parent.next_id → new (only when we set prev_id above).
                            if (new_spline_draft.prev_id != 0) {
                                for (auto& ps : edit_splines) {
                                    if (ps.id == new_spline_draft.prev_id) {
                                        // Steal any other next_id pointing at this child.
                                        for (auto& os : edit_splines) {
                                            if (os.id != ps.id && os.next_id == new_spline_draft.id)
                                                os.next_id = 0;
                                        }
                                        ps.next_id = new_spline_draft.id;
                                        break;
                                    }
                                }
                            }
                            edit_splines.push_back(new_spline_draft);
                            transient_splines.push_back(new_spline_draft);
                            selected_spline = static_cast<int>(edit_splines.size() - 1);
                            selected_object = -1;
                            remesh_spline(edit_splines.back());
                            const auto& s = edit_splines.back();
                            const auto a = spline::evaluate_spline(s, 0.f);
                            const auto b = spline::evaluate_spline(s, s.length);
                            place_marker(a.x, a.y, a.z + kArrowFloatZ, kArrowScale, false, s.id, true, false, 0, tile_gx,
                                         tile_gy, a.heading_rad);
                            place_marker(b.x, b.y, b.z + kArrowFloatZ, kArrowScale, false, s.id, false, false, 0, tile_gx,
                                         tile_gy, b.heading_rad);
                            if (const auto def = spline::load_sli(s.sli_path)) {
                                for (int pi = 0; pi < static_cast<int>(def->paths.size()); ++pi) {
                                    const auto& pp = def->paths[static_cast<std::size_t>(pi)];
                                    const auto p0 = spline::evaluate_spline(s, 0.f, pp.lateral, pp.height);
                                    const auto p1 =
                                        spline::evaluate_spline(s, s.length, pp.lateral, pp.height);
                                    place_marker(p0.x, p0.y, p0.z + kMarkFloatZ, 0.16f, true, s.id, true, false, pi, tile_gx,
                                                 tile_gy);
                                    place_marker(p1.x, p1.y, p1.z + kMarkFloatZ, 0.16f, true, s.id, false, false, pi,
                                                 tile_gx, tile_gy);
                                }
                            }
                            persist_editor_xy(s.position.x, s.position.y);
                            refresh_editor_lists();
                            if (show_path_rails) ensure_path_rails_for_owner(s.id, false);
                            show_spline_at(static_cast<std::size_t>(selected_spline));
                            edit_mode = EditMode::None;
                            grabbing = false;
                            rotating = false;
                            log::info("editor N auto-placed continue spline id=" + std::to_string(s.id) +
                                      " prev=" + std::to_string(s.prev_id));
                        } else {
                            new_grab_lock_xy = false;
                            edit_mode = EditMode::NewGrab;
                            grabbing = true;
                            rotating = false;
                            remesh_spline(new_spline_draft, true);
                            grab_mesh_ox = new_spline_draft.position.x;
                            grab_mesh_oy = new_spline_draft.position.y;
                            grab_mesh_oz = new_spline_draft.position.z;
                            ui::EditorSplineProps p{};
                            p.valid = true;
                            p.id = new_spline_draft.id;
                            p.sli_name = new_spline_draft.sli_path.filename().string();
                            p.z = new_spline_draft.position.z;
                            p.rot = new_spline_draft.heading_deg;
                            p.length = new_spline_draft.length;
                            p.radius = new_spline_draft.radius;
                            p.skew_start = new_spline_draft.skew_start;
                            p.skew_end = new_spline_draft.skew_end;
                            panel.show_spline_fields(p);
                            log::info("editor N: 10m grab move — LMB place / Esc cancel");
                            panel.set_status(
                                std::string("N spline: sigue mouse (Ctrl=snap path/spline fin/inicio)") +
                                (spawn_height_mode == SpawnHeightMode::OnMeshes ? " [mallas]"
                                 : spawn_height_mode == SpawnHeightMode::Terrain ? " [terreno]" : "") +
                                ", LMB=place");
                        }
                    } else {
                        log::warn("editor N: pick a type in Catálogo first");
                    }
                } else if (object_tool || selected_object >= 0 || panel.catalog_object_sel() >= 0 ||
                           !sticky_sco_path.empty()) {
                    const bool sel_ok = selected_object >= 0 &&
                                        static_cast<std::size_t>(selected_object) < edit_objects.size();
                    const std::int32_t sel_oid = sel_ok ? edit_objects[static_cast<std::size_t>(selected_object)].id : 0;
                    const auto sa_src = (sel_oid > 0) ? spline_attach_obj_to_entry.find(sel_oid)
                                                      : spline_attach_obj_to_entry.end();
                    if (sel_ok && sa_src != spline_attach_obj_to_entry.end() &&
                        sa_src->second < edit_spline_attachments.size()) {
                        push_undo("n_clone_spline_attach");
                        const auto& src_o = edit_objects[static_cast<std::size_t>(selected_object)];
                        const auto src_entry = edit_spline_attachments[sa_src->second];
                        map::MapObjectInst clone = src_o;
                        clone.id = next_object_id();
                        auto new_entry = src_entry;
                        new_entry.att.instance_id = clone.id;
                        new_entry.att.repeat_step = 0.f;
                        new_entry.att.repeat_range = 0.f;
                        new_entry.att.is_repeater = false;
                        float parent_len = 10.f;
                        for (const auto& s : edit_splines) {
                            if (s.id != src_entry.parent_spline_id) continue;
                            parent_len = (std::max)(0.5f, s.length);
                            break;
                        }
                        new_entry.att.offset_along =
                            (std::min)(parent_len, src_entry.att.offset_along + 1.f);
                        edit_objects.push_back(clone);
                        transient_objects.push_back(clone);
                        edit_spline_attachments.push_back(std::move(new_entry));
                        const std::size_t ei = edit_spline_attachments.size() - 1;
                        const std::int32_t nid = edit_objects.back().id;
                        spline_attach_obj_to_entry[nid] = ei;
                        spline_attach_spawn_ids.insert(nid);
                        sa_spawned_entries.insert(ei);
                        selected_object = static_cast<int>(edit_objects.size() - 1);
                        selected_spline = -1;
                        spawn_editor_object_gpu(edit_objects.back());
                        update_spline_attachment_entry_poses(ei);
                        sync_spline_attach_gpu(edit_objects.back());
                        scenery_scripts.sync(edit_objects, cfg.omsi_root);
                        new_sco_draft = edit_objects.back();
                        edit_mode = EditMode::NewScoFollow;
                        grabbing = true;
                        grab_suppress_g_cancel = true;
                        show_object_at(static_cast<std::size_t>(selected_object));
                        persist_editor_xy(edit_objects.back().position.x, edit_objects.back().position.y);
                        refresh_editor_lists();
                        panel.set_status("N clone SA id=" + std::to_string(nid) +
                                         " misma spline — mové along, LMB=fijar Esc=cancel");
                        log::info("editor N clone splineAttachement id=" + std::to_string(nid) +
                                  " from=" + std::to_string(sel_oid) + " splineId=" +
                                  std::to_string(src_entry.parent_spline_id));
                    } else {
                    std::filesystem::path path;
                    bool random_miss = false;
                    if (panel.imgui_state().spawn_random && object_tool) {
                        ui::load_editor_docks(panel.imgui_state());
                        path = panel.pick_spawn_random_sco_path();
                        if (path.empty()) {
                            random_miss = true;
                            panel.set_status(
                                "Generar al azar: sin .sco (catálogo / favoritos / carpeta)");
                            log::warn("editor N spawn_random: empty pool");
                        }
                    }
                    if (!random_miss) {
                        if (path.empty()) path = panel.catalog_selected_path();
                        if (path.empty()) path = sticky_sco_path;
                        if (path.empty()) {
                            for (const auto& o : edit_objects) {
                                path = o.sco_path;
                                break;
                            }
                        }
                    }
                    if (!path.empty()) {
                        sync_spawn_height_from_ui();
                        sticky_sco_path = path;
                        panel.remember_catalog_path(path);
                        f4f23ma::f4 inherit_rot{};
                        std::int32_t inherit_vp = 0;
                        float inherit_z = 0.f;
                        std::vector<std::string> inherit_str;
                        const bool inherit_rot_from_sel =
                            selected_object >= 0 &&
                            static_cast<std::size_t>(selected_object) < edit_objects.size();
                        if (inherit_rot_from_sel) {
                            const auto& src = edit_objects[static_cast<std::size_t>(selected_object)];
                            inherit_rot = src.rotation_deg;
                            inherit_vp = src.varparent_id;
                            inherit_z = src.position.z;
                            inherit_str = src.strings;
                        }
                        float hx = camera.target_x, hy = camera.target_y;
                        {
                            float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                            input::cursor_to_framebuffer(window, mx, my, fw, fh);
                            camera.ground_z = camera.target_z;
                            if (const auto hit = camera.hit_ground_xy(mx, my, fw, fh)) {
                                hx = hit->x;
                                hy = hit->y;
                            }
                        }
                        new_sco_draft = {};
                        new_sco_draft.id = next_object_id();
                        new_sco_draft.sco_path = path;
                        new_sco_draft.position.x = hx;
                        new_sco_draft.position.y = hy;
                        new_sco_draft.position.z = inherit_z;
                        new_sco_draft.varparent_id = inherit_vp;
                        new_sco_draft.strings = inherit_str;
                        if (inherit_rot_from_sel) new_sco_draft.rotation_deg = inherit_rot;
                        fill_sco_map_extras(new_sco_draft);
                        push_undo("n_sco_preview");
                        edit_objects.push_back(new_sco_draft);
                        transient_objects.push_back(new_sco_draft);
                        selected_object = static_cast<int>(edit_objects.size() - 1);
                        selected_spline = -1;
                        spawn_editor_object_gpu(edit_objects.back());
                        sync_object_entrypoint_to_global(edit_objects.back(), tile_gx, tile_gy);
                        scenery_scripts.sync(edit_objects, cfg.omsi_root);
                        apply_sco_follow_xy(edit_objects.back(), hx, hy);
                        if (inherit_rot_from_sel) {
                            auto& placed = edit_objects.back();
                            placed.rotation_deg.x = inherit_rot.x;
                            placed.position.z = inherit_z;
                            placed.varparent_id = inherit_vp;
                            if (!spawn_align_pitch_bank) {
                                placed.rotation_deg.y = inherit_rot.y;
                                placed.rotation_deg.z = inherit_rot.z;
                            }
                            if (inherit_vp != 0) apply_varparent(placed, inherit_vp);
                            sync_object_gpu(placed);
                        }
                        new_sco_draft = edit_objects.back();
                        edit_mode = EditMode::NewScoFollow;
                        grabbing = true;
                        panel.set_selection_text(
                            "Nuevo: " + path.filename().string() +
                            (spawn_height_mode == SpawnHeightMode::OnMeshes ? " [mallas]"
                                 : spawn_height_mode == SpawnHeightMode::Terrain ? " [terreno]" : "") +
                            " (LMB=fijar)");
                        log::info("editor N sco preview id=" + std::to_string(new_sco_draft.id));
                    } else {
                        log::warn("editor N: pick a .sco in Catálogo first");
                    }
                    }
                }
                } // end else (non group-N)
            }
            n_was = n_now;
        }
        main_watchdog.mark("M.dispatch.L02_2227");
        {
            static bool tab_was = false;
            const bool tab_now = editor_keys_live && glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS;
            if (tab_now && !tab_was && edit_mode == EditMode::None && !panel.keyboard_captured()) {
                const auto cur = ev.tool;
                const auto next = (cur == ui::EditorTool::Object) ? ui::EditorTool::Spline : ui::EditorTool::Object;
                panel.set_tool(next);
                panel.queue_imgui_tool_tab(static_cast<int>(next));
                selected_object = -1;
                selected_spline = -1;
                selected_entrypoint = -1;
                panel.set_selection_text("(none)");
                panel.set_status(next == ui::EditorTool::Object ? "Tool: Objects (Tab)" : "Tool: Splines (Tab)");
                log::info(std::string("editor Tab -> ") + (next == ui::EditorTool::Object ? "Object" : "Spline"));
            }
            tab_was = tab_now;
        }
        {
            const bool c_now = hotkey_down(GLFW_KEY_C, 'C');
            const bool del_now = editor_keys_live && (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
            static bool del_was = false;
            const bool ctrl_c = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (c_now && !c_was && edit_mode == EditMode::Mesh && !ImGui::GetIO().WantTextInput && !ctrl_c) {
                const bool uv_hot = mesh_edit.uv_editor_open &&
                                    (mesh_edit.uv_win_focused || mesh_edit.uv_canvas_hov);
                if (uv_hot) {
                    mesh_edit.circle_sel = !mesh_edit.circle_sel;
                    mesh_edit.circle_stroke_cap = false;
                    panel.set_status(mesh_edit.circle_sel ? "UV circulo" : "UV circulo OFF");
                } else {
                    mesh_edit.circle_sel = !mesh_edit.circle_sel;
                    mesh_edit.circle_stroke_cap = false;
                    panel.set_status(mesh_edit.circle_sel ? "Circulo: LMB pinta V/E/F, rueda radio, C sale"
                                                          : "Circulo OFF");
                }
            } else if (((c_now && !c_was) || (del_now && !del_was)) && edit_mode == EditMode::None &&
                marquee.phase == MarqueePhase::None) {
                if (view_only) {
                    panel.set_status("Modo solo visualización — C/Delete bloqueado");
                    editor_log::event("VIEW_ONLY_BLOCK", "C/Delete");
                } else if (sco_path_edit.active && ctrl_c && c_now && !c_was) {
                    sco_path_edit.clip.clear();
                    if (sco_path_edit.sel >= 0 &&
                        static_cast<std::size_t>(sco_path_edit.sel) < sco_path_edit.paths.size())
                        sco_path_edit.clip.push_back(sco_path_edit.paths[static_cast<std::size_t>(sco_path_edit.sel)]);
                    else
                        sco_path_edit.clip = sco_path_edit.paths;
                    panel.set_status("Copiados " + std::to_string(sco_path_edit.clip.size()) + " paths");
                } else if (sco_path_edit.active) {
                    sco_delete_sel_path();
                } else if (ctrl_c && c_now && !c_was) {
                    clipboard = {};
                    float px = 0.f, py = 0.f;
                    int n = 0;
                    auto add_obj = [&](const map::MapObjectInst& o) {
                        clipboard.objects.push_back(o);
                        px += o.position.x;
                        py += o.position.y;
                        ++n;
                    };
                    auto add_spl = [&](const map::SplineInst& s) {
                        clipboard.splines.push_back(s);
                        px += s.position.x;
                        py += s.position.y;
                        ++n;
                    };
                    if (!group_sel_obj.empty() || !group_sel_spl.empty()) {
                        for (int i : group_sel_obj) {
                            if (i >= 0 && static_cast<std::size_t>(i) < edit_objects.size())
                                add_obj(edit_objects[static_cast<std::size_t>(i)]);
                        }
                        for (int i : group_sel_spl) {
                            if (i >= 0 && static_cast<std::size_t>(i) < edit_splines.size())
                                add_spl(edit_splines[static_cast<std::size_t>(i)]);
                        }
                    } else if (selected_object >= 0 &&
                               static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                        add_obj(edit_objects[static_cast<std::size_t>(selected_object)]);
                    } else if (selected_spline >= 0 &&
                               static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                        add_spl(edit_splines[static_cast<std::size_t>(selected_spline)]);
                    }
                    if (n > 0) {
                        clipboard.pivot_x = px / static_cast<float>(n);
                        clipboard.pivot_y = py / static_cast<float>(n);
                        clipboard.valid = true;
                        panel.set_status("Copiado: " + std::to_string(clipboard.objects.size()) + " sco, " +
                                         std::to_string(clipboard.splines.size()) + " sli (Ctrl+V)");
                        log::info("editor copy obj=" + std::to_string(clipboard.objects.size()) +
                                  " spl=" + std::to_string(clipboard.splines.size()));
                    } else {
                        panel.set_status("Ctrl+C: nada seleccionado");
                    }
                } else if (!group_sel_obj.empty() || !group_sel_spl.empty()) {
                    omsi::editor::CommandManager::Get().ExecuteCommand(omsi::editor::make_lambda_command(
                        "delete_group",
                        [&] {
                    push_undo("delete_group");
                    for (const auto oid : group_sel_obj_ids) {
                        for (const auto& o : edit_objects) {
                            if (o.id != oid) continue;
                            fire_place_asmr(editor::PlaceFxKind::DeleteShrink, o.position.x, o.position.y,
                                            o.position.z, object_place_radius(o.id), oid,
                                            editor::SfxCue::PebbleSoft);
                            break;
                        }
                    }
                    const auto del_o = group_sel_obj_ids;
                    const auto del_s = group_sel_spl_ids;
                    for (const auto oid : del_o) {
                        if (map::remove_global_entrypoint_by_object_id(cfg.global, oid)) {
                            entrypoints_dirty = true;
                            panel.set_entrypoints(cfg.global.entrypoints);
                        }
                        mark_editor_object_deleted(oid);
                        purge_path_rails_for_owner(oid, true);
                        for (auto& sp : scene_places) {
                            if (sp.object_id != oid) continue;
                            sp.alive = false;
                            // [tree] forest: do not hide shared chunk — rebuild_dirty_forests drops geom
                            if (!sp.is_tree) renderer.set_chunk_visible(sp.chunk, false);
                        }
                        for (auto& mk : end_markers) {
                            if (mk.from_object && mk.owner_id == oid) {
                                mk.alive = false;
                                renderer.set_chunk_visible(mk.chunk, false);
                            }
                        }
                        edit_objects.erase(std::remove_if(edit_objects.begin(), edit_objects.end(),
                                                          [&](const map::MapObjectInst& o) { return o.id == oid; }),
                                           edit_objects.end());
                        transient_objects.erase(
                            std::remove_if(transient_objects.begin(), transient_objects.end(),
                                           [&](const map::MapObjectInst& o) { return o.id == oid; }),
                            transient_objects.end());
                    }
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
                        // Clear chain links pointing at deleted ids (before erase).
                        for (auto& s : edit_splines) {
                            if (s.next_id == sid) s.next_id = 0;
                            if (s.prev_id == sid) s.prev_id = 0;
                        }
                        for (auto& s : transient_splines) {
                            if (s.next_id == sid) s.next_id = 0;
                            if (s.prev_id == sid) s.prev_id = 0;
                        }
                        edit_splines.erase(std::remove_if(edit_splines.begin(), edit_splines.end(),
                                                          [&](const map::SplineInst& s) { return s.id == sid; }),
                                           edit_splines.end());
                        transient_splines.erase(
                            std::remove_if(transient_splines.begin(), transient_splines.end(),
                                           [&](const map::SplineInst& s) { return s.id == sid; }),
                            transient_splines.end());
                    }
                    group_sel_obj.clear();
                    group_sel_spl.clear();
                    group_sel_obj_ids.clear();
                    group_sel_spl_ids.clear();
                    selected_object = -1;
                    selected_spline = -1;
                    refresh_editor_lists();
                    // Mark dirty only — never sync-write every loaded tile on delete (was ~1.4s freeze).
                    for (auto& tile : world.tiles_mut()) mark_tile_dirty(tile.grid_x, tile.grid_y);
                    panel.set_status("Grupo borrado");
                    log::info("editor delete group");
                        },
                        "grp_obj=" + std::to_string(group_sel_obj_ids.size()) +
                            " grp_spl=" + std::to_string(group_sel_spl_ids.size())));
                } else {
                    if (selected_object >= 0 || selected_spline >= 0) {
                        omsi::editor::CommandManager::Get().ExecuteCommand(omsi::editor::make_lambda_command(
                            "delete_single",
                            [&] {
                        push_undo("delete_single");
                        if (selected_object >= 0 &&
                            static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                            const auto& del_o = edit_objects[static_cast<std::size_t>(selected_object)];
                            fire_place_asmr(editor::PlaceFxKind::DeleteShrink, del_o.position.x, del_o.position.y,
                                            del_o.position.z, object_place_radius(del_o.id), del_o.id,
                                            editor::SfxCue::PebbleSoft);
                            const auto oid = del_o.id;
                            const bool is_sa = spline_attach_obj_to_entry.count(oid) != 0;
                            // Drop matching [entrypoints] row (OMSI keeps global.cfg in sync on delete).
                            if (map::remove_global_entrypoint_by_object_id(cfg.global, oid)) {
                                entrypoints_dirty = true;
                                panel.set_entrypoints(cfg.global.entrypoints);
                            }
                            mark_editor_object_deleted(oid);
                            purge_path_rails_for_owner(oid, true);
                            for (auto& sp : scene_places) {
                                if (sp.object_id == oid) {
                                    sp.alive = false;
                                    renderer.set_chunk_visible(sp.chunk, false);
                                }
                            }
                            for (auto& mk : end_markers) {
                                if (mk.from_object && mk.owner_id == oid) {
                                    mk.alive = false;
                                    renderer.set_chunk_visible(mk.chunk, false);
                                }
                            }
                            if (!is_sa)
                                edit_objects.erase(edit_objects.begin() + selected_object);
                            transient_objects.erase(
                                std::remove_if(transient_objects.begin(), transient_objects.end(),
                                               [&](const map::MapObjectInst& o) { return o.id == oid; }),
                                transient_objects.end());
                            selected_object = -1;
                            // Dirty tiles only (stream/Ctrl+S persist). Avoid full-map write freeze.
                            for (auto& tile : world.tiles_mut()) mark_tile_dirty(tile.grid_x, tile.grid_y);
                            panel.set_selection_text("(deleted object)");
                            log::info("editor delete object id=" + std::to_string(oid));
                        } else if (selected_spline >= 0 &&
                                   static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                            const auto sid = edit_splines[static_cast<std::size_t>(selected_spline)].id;
                            mark_editor_spline_deleted(sid);
                            for (const auto& s : edit_splines)
                                if (s.next_id == sid) mark_path_rails_dirty(s.id, false);
                            purge_path_rails_for_owner(sid, false);
                            if (auto* g = find_spline_gpu(sid)) {
                                g->alive = false;
                                for (const std::size_t c : g->chunks) renderer.set_chunk_visible(c, false);
                            }
                            for (auto& mk : end_markers) {
                                if (!mk.from_object && mk.owner_id == sid) {
                                    mk.alive = false;
                                    renderer.set_chunk_visible(mk.chunk, false);
                                }
                            }
                            // Clear prev_id / next_id links that pointed at the deleted spline.
                            for (auto& s : edit_splines) {
                                if (s.next_id == sid) s.next_id = 0;
                                if (s.prev_id == sid) s.prev_id = 0;
                            }
                            for (auto& s : transient_splines) {
                                if (s.next_id == sid) s.next_id = 0;
                                if (s.prev_id == sid) s.prev_id = 0;
                            }
                            edit_splines.erase(edit_splines.begin() + selected_spline);
                            transient_splines.erase(
                                std::remove_if(transient_splines.begin(), transient_splines.end(),
                                               [&](const map::SplineInst& s) { return s.id == sid; }),
                                transient_splines.end());
                            selected_spline = -1;
                            for (auto& tile : world.tiles_mut()) mark_tile_dirty(tile.grid_x, tile.grid_y);
                            panel.set_selection_text("(deleted spline)");
                            log::info("editor delete spline id=" + std::to_string(sid) + " (chain links cleared)");
                        }
                            },
                            "sel_obj=" + std::to_string(selected_object) +
                                " sel_spl=" + std::to_string(selected_spline)));
                    }
                }
            }
            c_was = c_now;
            del_was = del_now;
