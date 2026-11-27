        }
        sync_adjust_hud();
        tick_spline_gradient_terrain_live();
        {
            static bool v_was = false;
            const bool v_now = hotkey_down(GLFW_KEY_V, 'V');
            const bool ctrl_v = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (v_now && !v_was && ctrl_v && view_only) {
                panel.set_status("Modo solo visualización — Ctrl+V (pegar) bloqueado");
                editor_log::event("VIEW_ONLY_BLOCK", "Ctrl+V paste");
            } else if (v_now && !v_was && ctrl_v && sco_path_edit.active && !sco_path_edit.clip.empty()) {
                push_undo("sco_path_paste");
                for (auto p : sco_path_edit.clip) sco_path_edit.paths.push_back(std::move(p));
                sco_path_edit.sel = static_cast<int>(sco_path_edit.paths.size()) - 1;
                auto& np = sco_path_edit.paths.back();
                sco_path_edit.placing = true;
                sco_path_edit.moving = true;
                sco_path_edit.rotating = sco_path_edit.following = false;
                sco_path_edit.grab_lx = np.x;
                sco_path_edit.grab_ly = np.y;
                sco_path_edit.grab_lz = np.z;
                sco_path_edit.grab_hdg = np.heading_deg;
                sco_path_edit.grab_len = np.length;
                sco_path_edit.grab_radius = np.radius;
                sco_path_edit.dirty = true;
                edit_mode = EditMode::Grab;
                grabbing = true;
                rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
                sco_path_arm_live();
                panel.set_status("Pegar paths — mouse coloca (z local .sco), LMB asienta");
            } else if (v_now && !v_was && ctrl_v && edit_mode == EditMode::None &&
                marquee.phase == MarqueePhase::None && clipboard.valid &&
                (!clipboard.objects.empty() || !clipboard.splines.empty())) {
                push_undo("paste_clipboard");
                float paste_x = clipboard.pivot_x;
                float paste_y = clipboard.pivot_y;
                {
                    float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                    input::cursor_to_framebuffer(window, mx, my, fw, fh);
                    camera.ground_z = camera.target_z;
                    if (const auto hit = camera.hit_ground_xy(mx, my, fw, fh)) {
                        paste_x = hit->x;
                        paste_y = hit->y;
                    }
                }
                marquee = {};
                marquee.phase = MarqueePhase::Placing;
                marquee.intent = MarqueeIntent::Clone;
                marquee.anchor_x = paste_x;
                marquee.anchor_y = paste_y;
                for (const auto& src : clipboard.objects) {
                    auto c = src;
                    c.id = next_object_id();
                    c.position.x = paste_x + (src.position.x - clipboard.pivot_x);
                    c.position.y = paste_y + (src.position.y - clipboard.pivot_y);
                    edit_objects.push_back(c);
                    spawn_editor_object_gpu(edit_objects.back());
                    upsert_transient_object(edit_objects.back());
                    marquee.base_obj.push_back(edit_objects.back());
                    marquee.track_obj.push_back(static_cast<int>(edit_objects.size() - 1));
                    marquee.track_obj_ids.push_back(c.id);
                }
                // Al pegar: siempre borrar prev_id / next_id (no heredar links del original).
                for (const auto& src : clipboard.splines) {
                    auto c = src;
                    c.id = next_spline_id();
                    c.prev_id = 0;
                    c.next_id = 0;
                    c.position.x = paste_x + (src.position.x - clipboard.pivot_x);
                    c.position.y = paste_y + (src.position.y - clipboard.pivot_y);
                    remesh_spline(c);
                    if (auto* g = find_spline_gpu(c.id)) g->pinned = true;
                    edit_splines.push_back(c);
                    upsert_transient_spline(c);
                    marquee.base_spl.push_back(edit_splines.back());
                    marquee.track_spl.push_back(static_cast<int>(edit_splines.size() - 1));
                    marquee.track_spl_ids.push_back(c.id);
                }
                refresh_editor_lists();
                scenery_scripts.sync(edit_objects, cfg.omsi_root);
                update_marquee_placing(paste_x, paste_y);
                panel.set_status("Pegado: mové mouse, LMB=place, Esc=cancel");
                log::info("editor paste placing at mouse");
            }
            v_was = v_now;
        }
        main_watchdog.mark("M.dispatch.L03_87");
        {
            static bool a_was = false;
            static double last_a_time = 0.0;
            const bool a_now = hotkey_down(GLFW_KEY_A, 'A');
            if (a_now && !a_was && view_only) {
                panel.set_status("Modo solo visualización — A (autofill) bloqueado");
                editor_log::event("VIEW_ONLY_BLOCK", "A autofill");
            } else if (a_now && !a_was && edit_mode == EditMode::Mesh) {
                if (!ImGui::GetIO().WantTextInput) mesh_toggle_select_all();
            } else if (a_now && !a_was && sco_path_edit.active && edit_mode == EditMode::None) {
                const double now = glfwGetTime();
                if (now - last_a_time < 0.45) {
                    sco_path_edit.aa_mode = !sco_path_edit.aa_mode;
                    autofill_path_mode = sco_path_edit.aa_mode;
                    autofill_path_stage = 0;
                    autofill_path_a = {};
                    autofill_path_b = {};
                    sco_path_edit.aa_a = {};
                    if (sco_path_edit.aa_mode) {
                        show_path_ends = true;
                        show_path_rails = true;
                        if (path_rails.empty()) ensure_path_rails_build_started();
                        else {
                            for (auto& r : path_rails)
                                if (r.alive) renderer.set_chunk_visible(r.chunk, true);
                        }
                        panel.set_status("AA paths: click extremo A luego B");
                    } else {
                        panel.set_status("AA paths OFF");
                    }
                }
                last_a_time = now;
            } else if (a_now && !a_was && edit_mode == EditMode::None) {
                const double now = glfwGetTime();
                if (now - last_a_time < 0.45) {
                    autofill_path_mode = !autofill_path_mode;
                    autofill_path_stage = 0;
                    autofill_path_a = {};
                    autofill_path_b = {};
                    if (autofill_path_mode) {
                        autofill_spline_ends = false;
                        panel.set_autofill(false);
                        show_path_ends = true;
                        show_path_rails = true;
                        if (path_rails.empty()) ensure_path_rails_build_started();
                        else {
                            for (auto& r : path_rails)
                                if (r.alive) renderer.set_chunk_visible(r.chunk, true);
                        }
                        panel.set_status("Path autofill: hover/click start path endpoint");
                        log::info("editor path autofill ON");
                    } else {
                        panel.set_status("Path autofill OFF");
                        log::info("editor path autofill OFF");
                    }
                } else {
                    autofill_path_mode = false;
                    autofill_path_stage = 0;
                    if (selected_spline < 0) {
                        panel.set_status("Autofill: select a spline first");
                        log::warn("editor autofill: no spline selected");
                    } else {
                        autofill_spline_ends = !autofill_spline_ends;
                        if (autofill_spline_ends) show_spline_ends = true;
                        panel.set_autofill(autofill_spline_ends);
                        panel.set_status(autofill_spline_ends
                                             ? "Autofill ON: click another start/end (hand cursor)"
                                             : "Autofill OFF");
                        log::info(std::string("editor autofill ") + (autofill_spline_ends ? "ON" : "OFF"));
                        editor_log::event("KEY_A",
                                          std::string("autofill_spline=") + (autofill_spline_ends ? "ON" : "OFF") +
                                              " | " + session_state());
                    }
                }
                last_a_time = now;
            }
            a_was = a_now;
        }

        if (ev.tool_changed) {
            selected_entrypoint = -1;
            if (ev.tool == ui::EditorTool::Tile) tile_frame_empty_pool_dirty = true;
            // Keep object/spline selection when switching tabs (attach / cross-select).
            // Only clear props if nothing remains selected.
            if (selected_object < 0 && selected_spline < 0) {
                panel.clear_selection_props();
                panel.set_selection_text("(none)");
            }
        }
        if (ev.teleport_entry >= 0 &&
            static_cast<std::size_t>(ev.teleport_entry) < cfg.global.entrypoints.size())
            teleport_camera_to_entrypoint(
                cfg.global.entrypoints[static_cast<std::size_t>(ev.teleport_entry)], "event");
        if (ev.select_catalog_object >= 0) {
            const int sel = panel.catalog_object_sel();
            if (sel >= 0 && static_cast<std::size_t>(sel) < panel.catalog_objects().size())
                sticky_sco_path = panel.catalog_objects()[static_cast<std::size_t>(sel)].path;
        }
        if (ev.select_catalog_spline >= 0) {
            const int sel = panel.catalog_spline_sel();
            if (sel >= 0 && static_cast<std::size_t>(sel) < panel.catalog_splines().size()) {
                sticky_sli_path = panel.catalog_splines()[static_cast<std::size_t>(sel)].path;
                const auto& new_path = sticky_sli_path;
                std::vector<int> targets;
                if (!group_sel_spl.empty()) {
                    std::filesystem::path match;
                    if (selected_spline >= 0 &&
                        static_cast<std::size_t>(selected_spline) < edit_splines.size())
                        match = edit_splines[static_cast<std::size_t>(selected_spline)].sli_path;
                    else if (group_sel_spl.front() >= 0 &&
                             static_cast<std::size_t>(group_sel_spl.front()) < edit_splines.size())
                        match = edit_splines[static_cast<std::size_t>(group_sel_spl.front())].sli_path;
                    for (int gi : group_sel_spl) {
                        if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                        if (edit_splines[static_cast<std::size_t>(gi)].sli_path == match)
                            targets.push_back(gi);
                    }
                } else if (selected_spline >= 0 &&
                           static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                    targets.push_back(selected_spline);
                }
                int n_sli = 0;
                for (int ti : targets) {
                    auto& s = edit_splines[static_cast<std::size_t>(ti)];
                    if (s.sli_path == new_path) continue;
                    if (n_sli == 0) push_undo("replace_sli");
                    s.sli_path = new_path;
                    // Full remesh; apply_spline_gpu_meshes clears old strips on sli change.
                    remesh_spline(s, false);
                    upsert_transient_spline(s);
                    persist_editor_xy(s.position.x, s.position.y);
                    // Keep in-memory tile.splines in sync so stream unload/reload cannot
                    // revive the previous .sli mesh from stale tile data.
                    if (map::MapTileData* tile = find_tile_mut_xy(s.position.x, s.position.y)) {
                        sync_tile_lists_only(*tile);
                        mark_tile_dirty(tile->grid_x, tile->grid_y);
                    }
                    ++n_sli;
                }
                if (n_sli > 0) {
                    if (selected_spline >= 0 &&
                        static_cast<std::size_t>(selected_spline) < edit_splines.size())
                        show_spline_at(static_cast<std::size_t>(selected_spline));
                    panel.set_status("SLI " + new_path.filename().string() + " x" + std::to_string(n_sli));
                }
            }
        }
        if (ev.select_object_id > 0 || ev.select_object_index >= 0) {
            const int prev_obj = selected_object;
            selected_object = -1;
            if (ev.select_object_id > 0) {
                for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                    if (edit_objects[i].id != ev.select_object_id) continue;
                    selected_object = static_cast<int>(i);
                    break;
                }
            } else if (static_cast<std::size_t>(ev.select_object_index) < edit_objects.size()) {
                selected_object = ev.select_object_index;
            }
            if (selected_object >= 0 &&
                static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                selected_spline = -1;
                show_object_at(static_cast<std::size_t>(selected_object));
                const auto oid = edit_objects[static_cast<std::size_t>(selected_object)].id;
                const bool add_grp = panel.imgui_state().object_multi_pick ||
                                     ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                                     ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) ||
                                     ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
                if (add_grp) {
                    if (prev_obj >= 0 && prev_obj != selected_object &&
                        static_cast<std::size_t>(prev_obj) < edit_objects.size() &&
                        std::find(group_sel_obj.begin(), group_sel_obj.end(), prev_obj) == group_sel_obj.end()) {
                        group_sel_obj.push_back(prev_obj);
                        group_sel_obj_ids.push_back(edit_objects[static_cast<std::size_t>(prev_obj)].id);
                    }
                    const auto it = std::find(group_sel_obj.begin(), group_sel_obj.end(), selected_object);
                    if (it != group_sel_obj.end()) {
                        group_sel_obj.erase(it);
                        group_sel_obj_ids.erase(std::remove(group_sel_obj_ids.begin(), group_sel_obj_ids.end(), oid),
                                                group_sel_obj_ids.end());
                    } else {
                        group_sel_obj.push_back(selected_object);
                        group_sel_obj_ids.push_back(oid);
                    }
                    panel.set_status("Grupo: " + std::to_string(group_sel_obj.size()) + " obj, " +
                                     std::to_string(group_sel_spl.size()) + " spl");
                } else {
                    const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                    camera.target_x = o.position.x;
                    camera.target_y = o.position.y;
                    camera.target_z = o.position.z + sample_terrain_editor_xy(o.position.x, o.position.y);
                    camera.radius = 35.f;
                }
            } else if (ev.select_object_id > 0) {
                for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
                    if (edit_spline_attachments[ei].att.instance_id != ev.select_object_id) continue;
                    select_spline_attachment_entry(ei);
                    const auto& entry = edit_spline_attachments[ei];
                    if (const map::SplineInst* sp = resolve_edit_spline_for_attach(entry)) {
                        const auto pose = map::compute_spline_attachment_pose(
                            *sp, entry.att,
                            entry.att.offset_along - map::spline_local_length(*sp),
                            spline::resolve_spline_mesh_chain_end_z(*sp, spline_chain_z_for_attach()));
                        camera.target_x = pose.x;
                        camera.target_y = pose.y;
                        camera.target_z = pose.z;
                        camera.radius = 35.f;
                    }
                    break;
                }
            }
        }
        if (ev.select_spline_id > 0 || ev.select_spline_index >= 0) {
            const int prev_spl = selected_spline;
            selected_spline = -1;
            if (ev.select_spline_id > 0) {
                for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                    if (edit_splines[i].id != ev.select_spline_id) continue;
                    selected_spline = static_cast<int>(i);
                    break;
                }
            } else if (static_cast<std::size_t>(ev.select_spline_index) < edit_splines.size()) {
                selected_spline = ev.select_spline_index;
            }
            if (selected_spline >= 0 &&
                static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                selected_object = -1;
                show_spline_at(static_cast<std::size_t>(selected_spline));
                const auto sid = edit_splines[static_cast<std::size_t>(selected_spline)].id;
                const bool add_grp = panel.imgui_state().spline_multi_pick ||
                                     ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                                     ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) ||
                                     ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
                if (add_grp) {
                    if (prev_spl >= 0 && prev_spl != selected_spline &&
                        static_cast<std::size_t>(prev_spl) < edit_splines.size() &&
                        std::find(group_sel_spl.begin(), group_sel_spl.end(), prev_spl) == group_sel_spl.end()) {
                        group_sel_spl.push_back(prev_spl);
                        group_sel_spl_ids.push_back(edit_splines[static_cast<std::size_t>(prev_spl)].id);
                    }
                    const auto it = std::find(group_sel_spl.begin(), group_sel_spl.end(), selected_spline);
                    if (it != group_sel_spl.end()) {
                        group_sel_spl.erase(it);
                        group_sel_spl_ids.erase(std::remove(group_sel_spl_ids.begin(), group_sel_spl_ids.end(), sid),
                                                group_sel_spl_ids.end());
                    } else {
                        group_sel_spl.push_back(selected_spline);
                        group_sel_spl_ids.push_back(sid);
                    }
                    panel.set_status("Grupo: " + std::to_string(group_sel_obj.size()) + " obj, " +
                                     std::to_string(group_sel_spl.size()) + " spl");
                } else {
                    const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    const auto mid = spline::evaluate_spline(s, s.length * 0.5f);
                    camera.target_x = mid.x;
                    camera.target_y = mid.y;
                    camera.target_z = mid.z;
                    camera.radius = 40.f;
                }
            }
        }
        if (ev.apply_object && ev.object_edit.valid) {
            // Same as spline: one push per arm_undo (start of drag / button click)
            if (ev.undo_before_object)
                push_undo("object_panel_edit");
            bool respawn_sa = false;
            bool matched_obj = false;
            for (auto& o : edit_objects) {
                if (o.id != ev.object_edit.id) continue;
                matched_obj = true;
                if (const auto sa_it = spline_attach_obj_to_entry.find(o.id);
                    sa_it != spline_attach_obj_to_entry.end() &&
                    sa_it->second < edit_spline_attachments.size()) {
                    auto& entry = edit_spline_attachments[sa_it->second];
                    const float old_step = entry.att.repeat_step;
                    const float old_range = entry.att.repeat_range;
                    entry.att.object_text_lines = ev.object_edit.strings;
                    entry.att.object_text_line_count =
                        ev.object_edit.text_slot_count > 0
                            ? ev.object_edit.text_slot_count
                            : static_cast<std::int32_t>(ev.object_edit.strings.size());
                    entry.att.repeat_step = ev.object_edit.spline_repeat_step;
                    entry.att.repeat_range = ev.object_edit.spline_repeat_range;
                    if (std::abs(old_step - entry.att.repeat_step) > 1.e-4f ||
                        std::abs(old_range - entry.att.repeat_range) > 1.e-4f)
                        respawn_sa = true;
                    else if (const map::SplineInst* sp = resolve_edit_spline_for_attach(entry)) {
                        if (!map::spline_attach_from_panel(*sp, ev.object_edit.x, ev.object_edit.y,
                                                           ev.object_edit.z, entry.att))
                            panel.set_status("Along fuera de rango (0.." +
                                             std::to_string(sp->length) + " m)");
                        else {
                            entry.att.align_terrain = ev.object_edit.spline_align_terrain;
                            entry.att.tangential = ev.object_edit.spline_tangential;
                            if (entry.att.tangential) {
                                // Still store 0 rots when tangential (OMSI convention + flag=1)
                                entry.att.rotation_y = 0.f;
                                entry.att.rotation_x = 0.f;
                                entry.att.rotation_z = 0.f;
                            } else {
                                entry.att.rotation_y = ev.object_edit.rot;
                                entry.att.rotation_x = ev.object_edit.pitch;
                                entry.att.rotation_z = ev.object_edit.bank;
                            }
                            update_spline_attachment_entry_poses(sa_it->second,
                                                                   ev.object_edit.spline_tangential);
                            if (auto* tile = find_tile_mut_xy(o.position.x, o.position.y)) {
                                auto path = tile->path;
                                if (cfg.workspace) {
                                    const auto wr = map::workspace_tile_map_for_read(&*cfg.workspace, path);
                                    if (!wr.empty()) path = wr;
                                }
                                map::update_sa_tangential_in_map(path, entry.att.instance_id, entry.att.tangential);
                                map::update_sco_align_terrain_in_map(path, "[splineAttachement]",
                                                                     entry.att.instance_id, entry.att.align_terrain);
                                mark_tile_dirty(tile->grid_x, tile->grid_y);
                                terrain_align_gpu_dirty = true;
                            }
                        }
                    }
                } else {
                    {
                        const auto [wx, wy] = editor_to_world_xy(o.position.x, o.position.y);
                        const auto [home_gx, home_gy] = object_home_tile(o.id, wx, wy);
                        const auto [new_wx, new_wy] =
                            world_from_tile_local(ev.object_edit.x, ev.object_edit.y, home_gx, home_gy);
                        const auto [ex, ey] = world_to_editor_xy(new_wx, new_wy);
                        o.position.x = ex;
                        o.position.y = ey;
                    }
                    o.position.z = ev.object_edit.z;
                    o.rotation_deg.x = ev.object_edit.rot;
                    o.rotation_deg.y = ev.object_edit.pitch;
                    o.rotation_deg.z = ev.object_edit.bank;
                    o.align_terrain = ev.object_edit.spline_align_terrain;
                    if (o.align_terrain)
                        o.position.z = sample_terrain_editor_xy(o.position.x, o.position.y);
                    if (auto* tile = find_tile_mut_xy(o.position.x, o.position.y)) {
                        auto path = tile->path;
                        if (cfg.workspace) {
                            const auto wr = map::workspace_tile_map_for_read(&*cfg.workspace, path);
                            if (!wr.empty()) path = wr;
                        }
                        map::update_object_align_terrain_in_map(path, o.id, o.align_terrain);
                        mark_tile_dirty(tile->grid_x, tile->grid_y);
                        terrain_align_gpu_dirty = true;
                    }
                    sync_object_gpu(o);
                }
                o.strings = ev.object_edit.strings;
                o.string_line_count = ev.object_edit.text_slot_count > 0
                                          ? ev.object_edit.text_slot_count
                                          : static_cast<std::int32_t>(ev.object_edit.strings.size());
                if (o.string_line_count < static_cast<std::int32_t>(o.strings.size()))
                    o.string_line_count = static_cast<std::int32_t>(o.strings.size());
                fill_sco_map_extras(o);
                for (auto& a : edit_attachments) {
                    if (a.child_id != o.id) continue;
                    a.object_text_lines = ev.object_edit.strings;
                    a.object_text_line_count = o.string_line_count;
                    if (a.options_flag == 0 && a.object_text_line_count > 0) a.options_flag = 1;
                    break;
                }
                if (o.varparent_id != ev.object_edit.varparent_id)
                    apply_varparent(o, ev.object_edit.varparent_id);
                else
                    scenery_scripts.refresh_traffic_indices(edit_objects);
                for (auto& to : transient_objects) {
                    if (to.id == o.id) {
                        to = o;
                        break;
                    }
                }
                persist_editor_xy(o.position.x, o.position.y);
                {
                    std::int32_t gx = tile_gx, gy = tile_gy;
                    for (const auto& sp : scene_places) {
                        if (sp.alive && sp.object_id == o.id) { gx = sp.tile_gx; gy = sp.tile_gy; break; }
                    }
                    sync_object_entrypoint_to_global(o, gx, gy);
                }
                log::info("editor apply object id=" + std::to_string(o.id) +
                          " strings=" + std::to_string(o.strings.size()));
                break;
            }
            if (!matched_obj &&
                ev.object_edit.attach_kind == ui::EditorObjectProps::AttachKind::SplineAttach) {
                for (auto& entry : edit_spline_attachments) {
                    if (entry.att.instance_id != ev.object_edit.id) continue;
                    const float old_step = entry.att.repeat_step;
                    const float old_range = entry.att.repeat_range;
                    entry.att.object_text_lines = ev.object_edit.strings;
                    entry.att.object_text_line_count =
                        ev.object_edit.text_slot_count > 0
                            ? ev.object_edit.text_slot_count
                            : static_cast<std::int32_t>(ev.object_edit.strings.size());
                    entry.att.repeat_step = ev.object_edit.spline_repeat_step;
                    entry.att.repeat_range = ev.object_edit.spline_repeat_range;
                    if (std::abs(old_step - entry.att.repeat_step) > 1.e-4f ||
                        std::abs(old_range - entry.att.repeat_range) > 1.e-4f)
                        respawn_sa = true;
                    else if (const map::SplineInst* sp = resolve_edit_spline_for_attach(entry)) {
                        if (!map::spline_attach_from_panel(*sp, ev.object_edit.x, ev.object_edit.y,
                                                           ev.object_edit.z, entry.att))
                            panel.set_status("Along fuera de rango (0.." +
                                             std::to_string(sp->length) + " m)");
                        else {
                            entry.att.align_terrain = ev.object_edit.spline_align_terrain;
                            entry.att.tangential = ev.object_edit.spline_tangential;
                            if (entry.att.tangential) {
                                // Still store 0 rots when tangential (OMSI convention + flag=1)
                                entry.att.rotation_y = 0.f;
                                entry.att.rotation_x = 0.f;
                                entry.att.rotation_z = 0.f;
                            } else {
                                entry.att.rotation_y = ev.object_edit.rot;
                                entry.att.rotation_x = ev.object_edit.pitch;
                                entry.att.rotation_z = ev.object_edit.bank;
                            }
                            for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
                                if (&entry == &edit_spline_attachments[ei]) {
                                    update_spline_attachment_entry_poses(ei,
                                                                       ev.object_edit.spline_tangential);
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            if (respawn_sa) {
                for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
                    if (edit_spline_attachments[ei].att.instance_id != ev.object_edit.id) continue;
                    request_sa_entry_respawn(ei);
                    break;
                }
                refresh_editor_lists();
                const int want_id = ev.object_edit.id;
                selected_object = -1;
                for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                    if (edit_objects[i].id != want_id) continue;
                    selected_object = static_cast<int>(i);
                    show_object_at(i);
                    break;
                }
                if (selected_object < 0) {
                    for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
                        if (edit_spline_attachments[ei].att.instance_id != want_id) continue;
                        select_spline_attachment_entry(ei);
                        break;
                    }
                }
            }
        }
        handle_catalog_panel_events(ev);
        if (ev.open_spline_creator) {
            ui::SplineCreatorResult created{};
            std::optional<spline::SplineDefinition> initial;
            std::filesystem::path sli_path = panel.catalog_selected_path();
            if (sli_path.empty() && selected_spline >= 0 &&
                static_cast<std::size_t>(selected_spline) < edit_splines.size())
                sli_path = edit_splines[static_cast<std::size_t>(selected_spline)].sli_path;
            if (!sli_path.empty()) initial = spline::load_sli(sli_path);
            const HWND owner = glfwGetWin32Window(window);
            try {
            if (cfg.test_spline_creator) {
                const bool pass = ui::run_spline_creator_self_test(cfg.omsi_root, owner);
                cfg.spline_creator_test_ok = pass;
                panel.set_status(pass ? "Spline creator test PASS" : "Spline creator test FAIL");
                log::info(std::string("editor spline creator self-test ") + (pass ? "PASS" : "FAIL"));
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            } else {
                SplineCreatorCli cli{};
                cli.omsi_root = cfg.omsi_root;
                cli.open_sli = sli_path;
                const auto launched = launch_python_spline_creator(cli, owner);
                std::filesystem::path saved = launched.saved_path;
                bool ok = launched.started && !saved.empty();
        if (!launched.started) {
            log::warn("spline creator: Python no arrancó (el diálogo C++ ya no se usa)");
            panel.set_status("Creador Python no arrancó — instalá python en PATH");
            ok = false;
        }
                if (ok && !saved.empty()) {
                spline::invalidate_sli_cache(saved);
                auto normalize_catalog = [&](std::filesystem::path p) {
                    std::error_code ec;
                    p = std::filesystem::absolute(p, ec);
                    if (!cfg.omsi_root.empty()) {
                        const auto rel = std::filesystem::relative(p.lexically_normal(),
                                                                 cfg.omsi_root.lexically_normal(), ec);
                        if (!ec && !rel.empty() && rel.native().find(L"..") == std::wstring::npos)
                            return cfg.omsi_root / rel;
                    }
                    return p;
                };
                const auto npath = normalize_catalog(saved);
                bool dup = false;
                for (const auto& e : extra_catalog_sli) {
                    std::error_code ec;
                    if (e == npath || std::filesystem::equivalent(e, npath, ec)) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) extra_catalog_sli.push_back(npath);
                refresh_editor_lists();
                sticky_sli_path = npath;
                panel.remember_catalog_path(sticky_sli_path);
                panel.set_status("Spline creada: " + npath.filename().string());
                log::info("editor spline creator saved " + npath.string());
                }
            }
            } catch (...) {
                panel.set_status("Spline creator: error");
                log::error("editor spline creator exception");
            }
            glfwPollEvents();
        }
        {
            // "Colocar" for splines kept in wireframe by an edit: button, Enter in the viewport, or as
            // soon as the selection / tool moves on.
            static std::int32_t s_place_sel_id = -1;
            static int s_place_tool = -1;
            static bool s_enter_was = false;
            const std::int32_t sel_id =
                (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                    ? edit_splines[static_cast<std::size_t>(selected_spline)].id
                    : -1;
            const int tool_now = static_cast<int>(ev.tool);
            const bool enter = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS ||
                               glfwGetKey(window, GLFW_KEY_KP_ENTER) == GLFW_PRESS;
            const bool enter_hit = enter && !s_enter_was && !ImGui::GetIO().WantTextInput;
            s_enter_was = enter;
            const bool moved_on = (sel_id != s_place_sel_id && s_place_sel_id != -1) ||
                                  (s_place_tool != -1 && tool_now != s_place_tool);
            const bool dragging = ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureMouse;
            // the click that places / confirms in the 3D view (LMB released over the viewport) is "Colocar"
            static bool s_lmb_was = false;
            const bool lmb_now = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            const bool placed_click = s_lmb_was && !lmb_now && !ImGui::GetIO().WantCaptureMouse;
            s_lmb_was = lmb_now;
            if (!spline_pending_place.empty() &&
                (ev.spline_place_commit || enter_hit || placed_click || (moved_on && !dragging)))
                commit_spline_place();
            s_place_sel_id = sel_id;
            s_place_tool = tool_now;
            panel.imgui_state().spline_pending_place = static_cast<int>(spline_pending_place.size());
        }
        if (ev.open_instance_map) {
            std::int32_t id = 0;
            float px = 0.f, py = 0.f;
            bool is_spline = false;
            bool ok = false;
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                id = o.id;
                px = o.position.x;
                py = o.position.y;
                is_spline = false;
                ok = true;
            } else if (selected_spline >= 0 &&
                       static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                id = s.id;
                px = s.position.x;
                py = s.position.y;
                is_spline = true;
                ok = true;
            } else if (!group_sel_obj.empty()) {
                const int i = group_sel_obj.front();
                if (i >= 0 && static_cast<std::size_t>(i) < edit_objects.size()) {
                    const auto& o = edit_objects[static_cast<std::size_t>(i)];
                    id = o.id;
                    px = o.position.x;
                    py = o.position.y;
                    ok = true;
                }
            } else if (!group_sel_spl.empty()) {
                const int i = group_sel_spl.front();
                if (i >= 0 && static_cast<std::size_t>(i) < edit_splines.size()) {
                    const auto& s = edit_splines[static_cast<std::size_t>(i)];
                    id = s.id;
                    px = s.position.x;
                    py = s.position.y;
                    is_spline = true;
                    ok = true;
                }
            }
            // The block lives in the tile that lists the id: an [attachObj] child is written in its
            // parent's tile (its own XY can fall in a neighbour), a [splineAttachement] in its
            // spline's. Search by id first; XY only as a fallback.
            auto tile_owning_id = [&]() -> map::MapTileData* {
                if (id == 0) return nullptr;
                for (auto& t : world.tiles_mut()) {
                    if (is_spline) {
                        for (const auto& s : t.splines)
                            if (s.id == id) return &t;
                        continue;
                    }
                    for (const auto& a : t.attachments)
                        if (a.child_id == id) return &t;
                    for (const auto& sa : t.spline_attachments)
                        if (sa.instance_id == id) return &t;
                    for (const auto& o : t.objects)
                        if (o.id == id) return &t;
                }
                return nullptr;
            };
            map::MapTileData* owner_tile = ok ? tile_owning_id() : nullptr;
            if (!owner_tile && ok) owner_tile = find_tile_mut_xy(px, py);
            if (!ok) {
                panel.set_status("Open .map: selecciona un .sco o .sli primero");
            } else if (auto* tile = owner_tile) {
                if (!tile->path.empty()) {
                    std::filesystem::path open_path = tile->path;
                    std::filesystem::path read_fb = tile->path;
                    if (!view_only && cfg.workspace && !cfg.workspace->actual_dir.empty()) {
                        sync_tile_lists_only(*tile);
                        const auto overlay = map::workspace_overlay_write_path(*cfg.workspace, tile->path);
                        if (!overlay.empty()) {
                            std::error_code ec;
                            std::filesystem::create_directories(overlay.parent_path(), ec);
                            open_path = overlay;
                        }
                        read_fb = map::workspace_tile_map_for_read(
                            cfg.workspace ? &*cfg.workspace : nullptr, tile->path);
                        if (!map::write_tile_map_file(open_path, *tile, cfg.omsi_root, read_fb)) {
                            log::warn("Open .map: write snapshot failed path=" + open_path.string());
                            if (!std::filesystem::exists(open_path) && std::filesystem::exists(tile->path))
                                open_path = tile->path;
                        }
                    } else if (view_only) {
                        log::info("Open .map VIEW-ONLY read-only " + open_path.string());
                    } else {
                        log::warn("Open .map: skip write without workspace (would hit live)");
                    }
                    const int line = map::find_map_block_line(open_path, id, is_spline);
                    HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
                    const std::filesystem::path* write_root =
                        cfg.workspace && !cfg.workspace->actual_dir.empty() ? &cfg.workspace->actual_dir : nullptr;
                    ui::open_map_text_editor(owner, open_path, line > 0 ? line : 1, write_root);
                    if (line > 0) {
                        panel.set_status("Open .map L" + std::to_string(line) + " id=" + std::to_string(id) +
                                         " (memoria)");
                    } else {
                        panel.set_status("Open .map: abierto (memoria), id " + std::to_string(id) +
                                         " no hallado en bloque");
                    }
                    log::info("editor open map (from memory) " + open_path.string() + " id=" +
                              std::to_string(id) + " line=" + std::to_string(line));
                } else {
                    panel.set_status("Open .map: ruta de tile invalida");
                }
            } else {
                panel.set_status("Open .map: no hay tile para la seleccion");
            }
        }
        if (ev.spline_grad_preview && ev.spline_edit.valid && selected_spline >= 0) {
            static auto last_grad_preview = std::chrono::steady_clock::now();
            const auto nowgp = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowgp - last_grad_preview).count() >= 16) {
                last_grad_preview = nowgp;
                auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                if (s.id == ev.spline_edit.id) {
                    ensure_grad_slider_undo(s.id);
                    s.position.x = ev.spline_edit.x;
                    s.position.y = ev.spline_edit.y;
                    s.position.z = ev.spline_edit.z;
                    s.heading_deg = std::fmod(ev.spline_edit.rot, 360.f);
                    if (s.heading_deg <= -360.f) s.heading_deg += 360.f;
                    s.skew_start = ev.spline_edit.skew_start;
                    s.skew_end = ev.spline_edit.skew_end;
                    s.length = (std::max)(0.01f, ev.spline_edit.length);
                    s.radius = ev.spline_edit.radius;
                    s.start_grad = (std::clamp)(ev.spline_edit.start_grad, -100.f, 100.f);
                    s.end_grad = (std::clamp)(ev.spline_edit.end_grad, -100.f, 100.f);
                    s.use_delta_height = ev.spline_edit.use_delta_h;
                    s.is_spline_h = ev.spline_edit.use_delta_h;
                    s.delta_height = ev.spline_edit.use_delta_h ? ev.spline_edit.delta_h : 0.f;
                    s.cant_start = ev.spline_edit.cant_start;
                    s.cant_end = ev.spline_edit.cant_end;
                    s.mirror = ev.spline_edit.mirror;
                    {
                        int al = ev.spline_edit.terrain_align;
                        if (al < 0) al = 0;
                        if (al > 4) al = 4;
                        const auto prev = s.terrain_align;
                        s.terrain_align = static_cast<std::uint8_t>(al);
                        if (prev != s.terrain_align) {
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
                    }
                    remesh_spline(s, true); // preview-only (light)
                    update_spline_panel_live(static_cast<std::size_t>(selected_spline));
                }
            }
        }
        if (ev.spline_grad_preview_end && ev.spline_edit.valid && selected_spline >= 0) {
            auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
            if (s.id == ev.spline_edit.id) {
                ensure_grad_slider_undo(s.id);
                s.position.x = ev.spline_edit.x;
                s.position.y = ev.spline_edit.y;
                s.position.z = ev.spline_edit.z;
                s.heading_deg = std::fmod(ev.spline_edit.rot, 360.f);
                if (s.heading_deg <= -360.f) s.heading_deg += 360.f;
                s.skew_start = ev.spline_edit.skew_start;
                s.skew_end = ev.spline_edit.skew_end;
                s.length = (std::max)(0.01f, ev.spline_edit.length);
                s.radius = ev.spline_edit.radius;
                s.start_grad = (std::clamp)(ev.spline_edit.start_grad, -100.f, 100.f);
                s.end_grad = (std::clamp)(ev.spline_edit.end_grad, -100.f, 100.f);
                s.use_delta_height = ev.spline_edit.use_delta_h;
                s.is_spline_h = ev.spline_edit.use_delta_h;
                s.delta_height = ev.spline_edit.use_delta_h ? ev.spline_edit.delta_h : 0.f;
                s.cant_start = ev.spline_edit.cant_start;
                s.cant_end = ev.spline_edit.cant_end;
                s.mirror = ev.spline_edit.mirror;
                    {
                        int al = ev.spline_edit.terrain_align;
                        if (al < 0) al = 0;
                        if (al > 4) al = 4;
                        const auto prev = s.terrain_align;
                        s.terrain_align = static_cast<std::uint8_t>(al);
                        if (prev != s.terrain_align) {
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
                    }
                remesh_spline(s, false); // full textured commit
                upsert_transient_spline(s);
                persist_editor_xy(s.position.x, s.position.y);
                update_spline_panel_live(static_cast<std::size_t>(selected_spline));
            }
        }
        if (ev.apply_spline_mirror_only) {
            std::vector<int> targets = group_sel_spl;
            if (selected_spline >= 0 &&
                static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                if (std::find(targets.begin(), targets.end(), selected_spline) == targets.end())
                    targets.push_back(selected_spline);
            }
            if (targets.size() < 2) {
                panel.set_status("Mirror grupal: seleccioná 2+ splines (Shift/Ctrl o multi-select)");
            } else {
                push_undo("spline_mirror_group");
                int changed = 0;
                for (int gi : targets) {
                    if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                    auto& s = edit_splines[static_cast<std::size_t>(gi)];
                    s.mirror = !s.mirror;
                    remesh_spline(s);
                    upsert_transient_spline(s);
                    persist_editor_xy(s.position.x, s.position.y);
                    ++changed;
                }
                if (selected_spline >= 0 &&
                    static_cast<std::size_t>(selected_spline) < edit_splines.size())
                    update_spline_panel_live(static_cast<std::size_t>(selected_spline));
                panel.set_status("Mirror grupal invertido en " + std::to_string(changed) + " splines");
            }
        } else if (ev.apply_spline && ev.spline_edit.valid) {
            // Coalesce: only push_undo when arm_undo is set (first frame of a drag /
            // arrow click). Subsequent frames of the same drag fire without arm.
            if (ev.undo_before_spline)
                push_undo("spline_panel_edit");
            {
                for (auto& s : edit_splines) {
                    if (s.id != ev.spline_edit.id) continue;
                    s.position.x = ev.spline_edit.x;
                    s.position.y = ev.spline_edit.y;
                    s.position.z = ev.spline_edit.z;
                    s.heading_deg = std::fmod(ev.spline_edit.rot, 360.f);
                    if (s.heading_deg <= -360.f) s.heading_deg += 360.f;
                    s.skew_start = ev.spline_edit.skew_start;
                    s.skew_end = ev.spline_edit.skew_end;
                    s.length = (std::max)(0.f, ev.spline_edit.length);
                    s.radius = ev.spline_edit.radius;
                    s.start_grad = (std::clamp)(ev.spline_edit.start_grad, -100.f, 100.f);
                    s.end_grad = (std::clamp)(ev.spline_edit.end_grad, -100.f, 100.f);
                    s.use_delta_height = ev.spline_edit.use_delta_h;
                    s.is_spline_h = ev.spline_edit.use_delta_h;
                    if (ev.spline_edit.use_delta_h) {
                        s.delta_height = ev.spline_edit.delta_h;
                    } else {
                        s.delta_height = 0.f;
                    }
                    s.cant_start = ev.spline_edit.cant_start;
                    s.cant_end = ev.spline_edit.cant_end;
                    s.mirror = ev.spline_edit.mirror;
                    {
                        int al = ev.spline_edit.terrain_align;
                        if (al < 0) al = 0;
                        if (al > 4) al = 4;
                        const auto prev = s.terrain_align;
                        s.terrain_align = static_cast<std::uint8_t>(al);
                        if (prev != s.terrain_align) {
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
                    }
                    remesh_spline(s);
                    upsert_transient_spline(s);
                    persist_editor_xy(s.position.x, s.position.y);
                    break;
                }
            }
        }

        if (editor_keys_live && edit_mode != EditMode::Mesh && glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS &&
            !cfg.global.entrypoints.empty())
            teleport_camera_to_entrypoint(cfg.global.entrypoints[0]);
        if (editor_keys_live && edit_mode != EditMode::Mesh && glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS &&
            cfg.global.entrypoints.size() > 1)
            teleport_camera_to_entrypoint(cfg.global.entrypoints[1]);

        main_watchdog.mark("M.after_dispatch");
        editor_subphase_watch.emplace(main_watchdog.phase("camera_input_and_tile_follow"));
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        if (w > 0 && h > 0 && (w != fbw || h != fbh)) {
            renderer.resize(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
            fbw = w;
            fbh = h;
        }

        const bool over_ui_cam = over_ui;
        input::LunaCamera::ensure_scroll_callback(window);
        camera.suppress_pointer = mesh_edit.uv_canvas_hov;
        camera.suppress_keys = panel.keyboard_captured() || bus_drive_active;
        // BeamNG free flight (F11): Alt+LMB orbits around the selection mid point.
        camera.has_fly_pivot = false;
        if (camera.fly_mode) {
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                const auto& fo = edit_objects[static_cast<std::size_t>(selected_object)];
                camera.fly_pivot_x = fo.position.x;
                camera.fly_pivot_y = fo.position.y;
                camera.fly_pivot_z = fo.position.z;
                camera.has_fly_pivot = true;
            } else if (selected_spline >= 0 &&
                       static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                const auto& fs = edit_splines[static_cast<std::size_t>(selected_spline)];
                const auto fmid = spline::evaluate_spline(fs, fs.length * 0.5f);
                camera.fly_pivot_x = fmid.x;
                camera.fly_pivot_y = fmid.y;
                camera.fly_pivot_z = fmid.z;
                camera.has_fly_pivot = true;
            }
        }
        // Editor-wide: no wheel zoom (RMB drag zooms). Wheel only sizes circle brush / aerosol / mesh.
        {
            float wheel = input::LunaCamera::take_scroll_wheel_delta();
            if (camera.fly_mode) {
                // Free flight owns the wheel (speed with RMB, dolly without); brush size is untouched.
                if (!over_ui_cam && !mesh_edit.uv_canvas_hov && std::fabs(wheel) > 0.01f)
                    camera.add_fly_wheel(wheel);
            } else if (!over_ui_cam && !mesh_edit.uv_canvas_hov && std::fabs(wheel) > 0.01f) {
                auto& spw = panel.imgui_state();
                if (edit_mode == EditMode::Mesh && (mesh_edit.circle_sel || mesh_edit.loopcut_on)) {
                    mesh_edit.circle_r *= wheel > 0.f ? 1.12f : (1.f / 1.12f);
                    if (mesh_edit.circle_r < 4.f) mesh_edit.circle_r = 4.f;
                    if (mesh_edit.circle_r > 400.f) mesh_edit.circle_r = 400.f;
                } else if (spw.spray_enabled && !spw.spray_pick_mode) {
                    spw.spray_radius *= wheel > 0.f ? 1.12f : (1.f / 1.12f);
                    if (spw.spray_radius < 0.25f) spw.spray_radius = 0.25f;
                    if (spw.spray_radius > 100.f) spw.spray_radius = 100.f;
                } else if (marquee.phase == MarqueePhase::Select && sel_shape == SelShapeMode::Circle) {
                    sel_circle_r *= wheel > 0.f ? 1.12f : (1.f / 1.12f);
                    if (sel_circle_r < 8.f) sel_circle_r = 8.f;
                    if (sel_circle_r > 720.f) sel_circle_r = 720.f;
                }
            }
        }
        // Test hook: OMSI_TEST_SPAWN_BUS=<.bus path> spawns that bus once the map is up (frame 120) and
        // blows the horn for 3 s, so sound/engine can be checked from the log without the UI.
        {
            static int test_frame = 0;
            static const char* test_bus = std::getenv("OMSI_TEST_SPAWN_BUS");
            if (test_bus && *test_bus) {
                ++test_frame;
                // Performance drive (OMSI_PERF_DRIVE=<seconds>): from the moment the bus drives itself,
                // every frame time; each 10 s the window's average fps, 1 % / 0.1 % lows (average of the
                // worst frames), worst frame and frames over 16.7 / 33 ms with where the bus is; at the end
                // the whole run's, then the editor closes.
                static const char* perf_env = std::getenv("OMSI_PERF_DRIVE");
                if (perf_env && *perf_env && omsi_bus && autopilot.on) {
                    static const double perf_dur = (std::max)(30.0, std::atof(perf_env));
                    static double perf_t0 = -1.0, perf_last = 0.0, perf_win_t = 0.0;
                    static std::vector<float> perf_all, perf_win;
                    const double now_p = glfwGetTime();
                    auto perf_stats = [](std::vector<float> v, const char* tag, double t, float x, float y, float kmh) {
                        if (v.empty()) return;
                        double sum = 0.0;
                        int o16 = 0, o33 = 0;
                        for (float f : v) {
                            sum += f;
                            o16 += f > 16.7f;
                            o33 += f > 33.4f;
                        }
                        std::sort(v.begin(), v.end(), std::greater<float>());
                        auto low = [&](double frac) {
                            const std::size_t n = (std::max)(std::size_t{1}, static_cast<std::size_t>(v.size() * frac));
                            double s2 = 0.0;
                            for (std::size_t i = 0; i < n; ++i) s2 += v[i];
                            return 1000.0 / (s2 / static_cast<double>(n));
                        };
                        char line[320];
                        std::snprintf(line, sizeof(line),
                                      "PERFDRIVE %s t=%.0fs frames=%zu avg_fps=%.1f low1=%.1f low01=%.1f worst_ms=%.1f "
                                      "over16=%d over33=%d pos=(%.0f,%.0f) v=%.0fkmh",
                                      tag, t, v.size(), 1000.0 * static_cast<double>(v.size()) / sum, low(0.01), low(0.001),
                                      static_cast<double>(v.front()), o16, o33, static_cast<double>(x), static_cast<double>(y),
                                      static_cast<double>(kmh));
                        log::info(line);
                    };
                    static bool perf_vsync_was = false;
                    if (perf_t0 < 0.0) {
                        // uncapped while measuring (the saved vsync choice comes back before the editor closes)
                        perf_vsync_was = renderer.vsync();
                        renderer.set_vsync(false);
                        perf_t0 = perf_last = perf_win_t = now_p;
                        log::info("PERFDRIVE start duration=" + std::to_string(static_cast<int>(perf_dur)) + "s");
                    } else {
                        const float ms = static_cast<float>((now_p - perf_last) * 1000.0);
                        perf_last = now_p;
                        perf_all.push_back(ms);
                        perf_win.push_back(ms);
                        OoStatus pst{};
                        if (omsi_bus->oo) oo_status(omsi_bus->oo, &pst);
                        if (now_p - perf_win_t >= 10.0) {
                            perf_stats(perf_win, "win", now_p - perf_t0, omsi_bus->x, omsi_bus->y, pst.speed_kmh);
                            perf_win.clear();
                            perf_win_t = now_p;
                        }
                        if (now_p - perf_t0 >= perf_dur) {
                            perf_stats(perf_all, "TOTAL", now_p - perf_t0, omsi_bus->x, omsi_bus->y, pst.speed_kmh);
                            renderer.set_vsync(perf_vsync_was);
                            glfwSetWindowShouldClose(window, GLFW_TRUE);
                        }
                    }
                }
                static const bool test_autopilot = std::getenv("OMSI_TEST_AUTOPILOT") != nullptr;
                if (test_frame == 118 && test_autopilot) {
                    // the longest spline near the camera: the bus starts on it and follows the chain
                    int best = -1;
                    float best_len = 0.f;
                    for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                        const auto& sp = edit_splines[i];
                        if (std::hypot(sp.position.x - camera.target_x, sp.position.y - camera.target_y) > 400.f) continue;
                        // a road: a vehicle lane ([path] typ 0) running forward
                        const auto def = spline::load_sli_shared(sp.sli_path);
                        if (!def) continue;
                        bool road = false;
                        for (const auto& pth : def->paths)
                            if (pth.typ == 0 && pth.direction == 0) {
                                road = true;
                                autopilot.lateral_pending = pth.lateral;
                            }
                        if (!road) continue;
                        if (sp.length > best_len) {
                            best_len = sp.length;
                            best = static_cast<int>(i);
                        }
                    }
                    if (best >= 0) {
                        selected_spline = best;
                        autopilot.on = true;
                        autopilot.spline = best;
                        autopilot.forward = true;
                        autopilot.lateral = autopilot.lateral_pending;
                        log::info("autopilot: lane at lateral " + std::to_string(autopilot.lateral) + " m");
                        log::info("autopilot: start on spline id " + std::to_string(edit_splines[best].id) + " length " +
                                  std::to_string(best_len) + " m (of " + std::to_string(edit_splines.size()) + ")");
                    } else {
                        log::warn("autopilot: no spline near the camera");
                    }
                }
                static const char* test_track = std::getenv("OMSI_TEST_TRACK");
                if (test_frame == 118 && test_track && *test_track) {
                    auto& tm = omsi::tt::global_tt_man();
                    tm.ensure_loaded_now(cfg.global.root);
                    const auto& tracks = tm.data.tracks;
                    int pick = -1;
                    for (std::size_t i = 0; i < tracks.size(); ++i)
                        if (tracks[i].name == test_track) pick = static_cast<int>(i);
                    std::size_t start_entry = 0;
                    if (pick < 0) {
                        // the track running closest to the camera, from the entry there
                        std::vector<std::array<float, 3>> pts;
                        float best = 1e9f;
                        // bus routes only: tracks some trip runs on (not AI aircraft / tram tracks)
                        std::unordered_set<std::string> used;
                        for (const auto& trp : tm.data.trips) {
                            std::string n = trp.track_name;
                            for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            used.insert(n);
                        }
                        for (std::size_t i = 0; i < tracks.size(); ++i)
                            for (std::size_t k = 0; k + 3 < tracks[i].entries.size(); ++k) {
                                std::string tn = tracks[i].name;
                                for (auto& c : tn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                if (!used.empty() && !used.count(tn)) break;
                                if (!track_entry_points(tracks[i].entries[k], nullptr, pts) || pts.empty()) continue;
                                const float d = std::hypot(pts[0][0] - camera.target_x, pts[0][1] - camera.target_y);
                                if (d < best) {
                                    best = d;
                                    pick = static_cast<int>(i);
                                    start_entry = k;
                                }
                            }
                    }
                    log::info("track test: " + std::to_string(tracks.size()) + " tracks, picked " +
                              (pick >= 0 ? tracks[static_cast<std::size_t>(pick)].name : std::string("none")));
                    if (pick >= 0) {
                        autopilot = BusAutopilot{};
                        autopilot.on = true;
                        autopilot.track = pick;
                        autopilot.next_entry = start_entry;
                        extend_route();
                        if (autopilot.route.size() >= 2) {
                            const auto& a0 = autopilot.route[0];
                            const auto& a1 = autopilot.route[(std::min)(autopilot.route.size() - 1, std::size_t{4})];
                            bus_spawn_at = std::array<float, 3>{a0[0], a0[1],
                                                                std::atan2(a1[0] - a0[0], a1[1] - a0[1]) * 57.2957795f};
                        }
                        log::info("track test: route " + std::to_string(autopilot.route.size()) + " points from " +
                                  std::to_string(autopilot.next_entry) + "/" +
                                  std::to_string(tracks[static_cast<std::size_t>(pick)].entries.size()) + " entries");
                    }
                }
                static const char* test_trip = std::getenv("OMSI_TEST_TRIP");
                static int trip_phase = 0; // 0 pick, 1 streaming the first stop, 2 spawned, 3 driving
                static int trip_track = -1; // the trip's own track (.ttr): the route it drives
                static float trip_wait = 0.f;
                if (test_trip && *test_trip) {
                    if (trip_phase == 0 && test_frame >= 100) {
                        auto& tm = omsi::tt::global_tt_man();
                        tm.ensure_loaded_now(cfg.global.root);
                        // a trip by name, else the longest: its track's length (the saved path lengths; the
                        // entry count where none were saved), more stops on a tie - the hardest drive of the map
                        std::size_t pick = 0;
                        double most = -1.0;
                        bool named = false;
                        for (std::size_t i = 0; i < tm.data.trips.size() && !named; ++i) {
                            const auto& tr = tm.data.trips[i];
                            if (tr.name == test_trip) {
                                pick = i;
                                named = true;
                                break;
                            }
                            double len = 0.0;
                            int ti = tr.track_index;
                            if (ti == omsi::tt::Trip::kUnresolved) ti = tm.find_track_by_name(tr.track_name);
                            if (ti >= 0 && ti < static_cast<int>(tm.data.tracks.size())) {
                                const auto& trk = tm.data.tracks[static_cast<std::size_t>(ti)];
                                for (const auto& e : trk.entries) len += e.length > 0.f ? e.length : 25.0;
                            }
                            const double score = len + static_cast<double>(tr.stations.size()) * 1e-3;
                            if (score > most) {
                                most = score;
                                pick = i;
                            }
                        }
                        if (!named && pick < tm.data.trips.size())
                            log::info("trip test: longest trip '" + tm.data.trips[pick].name + "', track ~" +
                                      std::to_string(static_cast<int>(most)) + " m");
                        if (pick < tm.data.trips.size()) {
                            const auto& tr = tm.data.trips[pick];
                            trip_track = tr.track_index;
                            if (trip_track == omsi::tt::Trip::kUnresolved) trip_track = tm.find_track_by_name(tr.track_name);
                            if (trip_track >= static_cast<int>(tm.data.tracks.size())) trip_track = -1;
                        }
                        if (trip_load(pick)) {
                            std::array<float, 3> p0{};
                            bool exact = false;
                            if (trip_stop_position(ai_trip.stops[0], p0, exact)) {
                                camera.target_x = p0[0];
                                camera.target_y = p0[1];
                                (void)maybe_stream_follow(p0[0], p0[1], 40.f);
                            }
                            log::info("trip test: '" + ai_trip.name + "' " + std::to_string(ai_trip.stops.size()) +
                                      " stops, first '" + ai_trip.stops[0].name + "' tile " + std::to_string(ai_trip.stops[0].tile) +
                                      (exact ? " (loaded)" : " (streaming)"));
                            trip_phase = 1;
                        } else {
                            log::warn("trip test: no trip");
                            trip_phase = 99;
                        }
                    }
                    if (trip_phase == 1) {
                        trip_wait += script_dt;
                        std::array<float, 3> p0{};
                        bool exact = false;
                        (void)trip_stop_position(ai_trip.stops[0], p0, exact);
                        if (!exact) (void)maybe_stream_follow(p0[0], p0[1], 40.f);
                        if (((exact && !stream_busy()) || trip_wait > 90.f) && trip_track >= 0) {
                            // the trip's track: the bus drives the timetable's own path list end to end (a
                            // search over the loaded lanes found no way to stops still streaming in)
                            autopilot = BusAutopilot{};
                            autopilot.on = true;
                            autopilot.track = trip_track;
                            autopilot.next_entry = 0;
                            extend_route();
                            if (autopilot.route.size() >= 2) {
                                const auto& a0 = autopilot.route[0];
                                const auto& a1 = autopilot.route[(std::min)(autopilot.route.size() - 1, std::size_t{4})];
                                bus_spawn_at = std::array<float, 3>{a0[0], a0[1],
                                                                    std::atan2(a1[0] - a0[0], a1[1] - a0[1]) * 57.2957795f};
                                log::info("trip test: driving its track '" +
                                          omsi::tt::global_tt_man().data.tracks[static_cast<std::size_t>(trip_track)].name +
                                          "', " + std::to_string(autopilot.route.size()) + " route points so far");
                                request_spawn_omsi_bus(test_bus);
                                trip_phase = 4;
                            } else {
                                autopilot = BusAutopilot{};
                                trip_track = -1; // (nothing of it resolved: the lane search below)
                            }
                        }
                        if (trip_phase == 1 && ((exact && !stream_busy()) || trip_wait > 90.f)) {
                            ai_build(p0[0], p0[1]);
                            std::size_t pt = 0;
                            float bestd = 1e9f;
                            AiLane lane{};
                            for (std::size_t i = 0; i < ai_net.paths.size(); ++i)
                                for (std::size_t k = 0; k + 2 < ai_net.paths[i].pts.size(); ++k) {
                                    const auto& q = ai_net.paths[i].pts[k];
                                    const float d = std::hypot(q[0] - p0[0], q[1] - p0[1]);
                                    if (d < bestd && ai_net.paths[i].dir != 2) {
                                        bestd = d;
                                        lane = AiLane{static_cast<int>(i), ai_net.paths[i].dir == 1};
                                        pt = k;
                                    }
                                }
                            if (lane.path >= 0) {
                                const auto& pts = ai_net.paths[static_cast<std::size_t>(lane.path)].pts;
                                const auto& a = pts[pt];
                                const auto& c = pts[lane.rev ? (pt > 0 ? pt - 1 : 0) : pt + 1];
                                bus_spawn_at = std::array<float, 3>{a[0], a[1], std::atan2(c[0] - a[0], c[1] - a[1]) * 57.2957795f};
                                log::info("trip test: spawning at the first stop's lane, " + std::to_string(bestd) +
                                          " m from it, after " + std::to_string(trip_wait) + " s");
                            }
                            request_spawn_omsi_bus(test_bus);
                            ai_trip.next = 1; // the bus starts at the first stop
                            trip_phase = 2;
                        }
                    }
                    if (trip_phase == 2 && omsi_bus && omsi_bus->oo && omsi_bus->age > 12.f) {
                        autopilot = BusAutopilot{};
                        autopilot.on = true;
                        autopilot.ai = true;
                        autopilot.t = 100.f;
                        bus_ai_paths_visible = true;
                        (void)trip_route(*omsi_bus);
                        trip_phase = 3;
                    }
                }
                // OMSI_TEST_BUS_TRIGGERS="kw_scheinwerfer_toggle,..." - pressed once on the test bus after 10 s,
                // released half a second later (lights on for the night shots)
                {
                    static const char* trig_env = std::getenv("OMSI_TEST_BUS_TRIGGERS");
                    static int trig_stage = 0;
                    if (trig_env && *trig_env && omsi_bus && omsi_bus->oo && trig_stage < 2 &&
                        omsi_bus->age > (trig_stage == 0 ? 10.f : 10.5f)) {
                        std::string all(trig_env);
                        std::size_t p0 = 0;
                        while (p0 <= all.size()) {
                            auto e = all.find(',', p0);
                            if (e == std::string::npos) e = all.size();
                            const std::string tk = all.substr(p0, e - p0);
                            if (!tk.empty()) {
                                oo_key(omsi_bus->oo, tk.c_str(), trig_stage == 0 ? 1 : 0);
                                if (trig_stage == 0) log::info("test hook: bus trigger " + tk);
                            }
                            p0 = e + 1;
                        }
                        ++trig_stage;
                    }
                }
                static const bool test_ai = std::getenv("OMSI_TEST_AI") != nullptr;
                if (test_ai && omsi_bus && omsi_bus->oo && !autopilot.on && omsi_bus->age > 12.f) {
                    if (ai_start(*omsi_bus)) {
                        autopilot = BusAutopilot{};
                        autopilot.on = true;
                        autopilot.ai = true;
                        autopilot.t = 100.f;
                        bus_ai_paths_visible = true;
                        log::info("test hook: AI driving on");
                    }
                }
                if (test_frame == 120 && !omsi_bus && !(test_trip && *test_trip)) {
                    request_spawn_omsi_bus(test_bus);
                    if (const char* probe = std::getenv("OMSI_TEST_AUDIO_PROBE"); probe && *probe && omsi_bus && omsi_bus->oo)
                        oo_audio_probe(25.f, probe);
                }

                // after the spawn: horn at 2 s, then (once the auto-start had time) brake, D, throttle
                static auto spawn_t = std::chrono::steady_clock::time_point{};
                static int step = 0;
                if (omsi_bus && step == 0) {
                    spawn_t = std::chrono::steady_clock::now();
                    step = 1;
                }
                if (omsi_bus && step > 0 && !test_autopilot && !(test_track && *test_track) && !test_ai && !(test_trip && *test_trip)) {
                    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - spawn_t).count();
                    const struct { float at; const char* action; bool down; } seq[] = {
                        {2.f, "horn", true},     {3.f, "horn", false},          {22.f, "brake", true},
                        {22.5f, "automatic_D", true}, {22.7f, "automatic_D", false}, {23.5f, "brake", false},
                        {23.6f, "throttle", true}, {38.f, "throttle", false}};
                    if (step <= static_cast<int>(std::size(seq)) && t >= seq[step - 1].at) {
                        log::info(std::string("test hook: ") + seq[step - 1].action + (seq[step - 1].down ? " down" : " up"));
                        bus_key(*omsi_bus, seq[step - 1].action, seq[step - 1].down);
                        ++step;
                    }
                }
            }
        }
        // Panel lists: deferred while a bus exists, rebuilt once when it is gone and streaming is idle.
        editor_lists_defer = omsi_bus != nullptr;
        if (!editor_lists_defer && editor_lists_dirty && stream_phase == StreamPhase::Idle) refresh_editor_lists();
        if (!omsi_bus) {
            {
                auto _pb = frame_profiler.scope(MapEditorFrameProfiler::BusTick);
                tick_omsi_bus(script_dt); // (a bus being read on a worker is put up here once it is ready)
            }
            auto _w_ai = omsi::MainThreadWatchdog::active_scope("ai_traffic");
            {
                auto _pa = frame_profiler.scope(MapEditorFrameProfiler::AiTraffic);
                ai_traffic_frame(script_dt); // AI traffic also without a bus of your own
            }
            auto _pp = frame_profiler.scope(MapEditorFrameProfiler::ParkedCars);
            parked_frame();
        } else {
            static double bus_ms_sum = 0.0, bus_ms_max = 0.0, bus_win = 0.0;
            static int bus_n = 0;
            const auto bt0 = std::chrono::steady_clock::now();
            {
                auto _pb = frame_profiler.scope(MapEditorFrameProfiler::BusTick);
                tick_omsi_bus(script_dt);
            }
            {
                auto _w_ai = omsi::MainThreadWatchdog::active_scope("ai_traffic");
                {
                    auto _pa = frame_profiler.scope(MapEditorFrameProfiler::AiTraffic);
                    ai_traffic_frame(script_dt);
                }
                auto _pp = frame_profiler.scope(MapEditorFrameProfiler::ParkedCars);
                parked_frame();
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bt0).count();
            bus_ms_sum += ms;
            bus_ms_max = (std::max)(bus_ms_max, ms);
            ++bus_n;
            bus_win += script_dt;
            if (bus_win > 5.0) {
                log::info("PERF bus tick avg=" + std::to_string(bus_ms_sum / bus_n) + " ms max=" + std::to_string(bus_ms_max) +
                          " ms frames=" + std::to_string(bus_n) + " fps=" + std::to_string(bus_n / bus_win));
                bus_ms_sum = bus_ms_max = bus_win = 0.0;
                bus_n = 0;
            }
        }
        camera.update(window);
        // Co-op room (network, remote edits, avatars) + pedestrian mode camera (after the orbit update).
        coop_frame_tick();
        {
            // World sound listener = editor camera (OMSI space, Z up). See omsi/sound/world_sound_scene.hpp.
            float ex = 0.f, ey = 0.f, ez = 0.f;
            camera.eye_position(ex, ey, ez);
            omsi::sound::WorldSound::instance().set_listener(
                {ex, ey, ez}, {camera.target_x - ex, camera.target_y - ey, camera.target_z - ez});
            world_sound_scene.tick(edit_objects, {ex, ey, ez}, ImGui::GetIO().DeltaTime);
        }
        camera.suppress_pointer = false;
        camera.suppress_keys = false;
        if (camera.fly_speed_changed) {
            camera.fly_speed_changed = false;
            char fly_speed_buf[96];
            std::snprintf(fly_speed_buf, sizeof(fly_speed_buf),
                          "BeamNG vuelo (F11): velocidad base %.2f m/s (rueda+RMB x1.25)", camera.fly_speed);
            panel.set_status(fly_speed_buf);
        }
        if (!bus_drive_active) {
            const bool cam_orbit = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
                                   glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
            if (!cam_orbit)
                (void)maybe_stream_follow(camera.target_x, camera.target_y, camera.target_z);
        } else if (omsi_bus) {
            // driving: the map streams around the bus (the session origin never moves, so the bus
            // keeps its coordinates)
            (void)maybe_stream_follow(omsi_bus->x, omsi_bus->y, omsi_bus->z);
        }
        if (panel.current_tool() == ui::EditorTool::Tile) {
            if (!centerkachel.valid ||
                map::get_kachel_index_with_coord(cfg.global, centerkachel.gx, centerkachel.gy) < 0)
                sync_centerkachel_from_camera();
        }
        if (!bus_drive_active && !camera.fly_mode) track_camera_after_update();
        ++editor_frame_serial;
        if (stream_cooldown_frames > 0) --stream_cooldown_frames;
        const bool rmb_cam = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        const bool mmb_cam = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
        float rmb_ex = 0.f, rmb_ey = 0.f, rmb_ez = 0.f;
        if (!camera.fly_mode && !over_ui_cam && !rmb_cam && !mmb_cam && !bus_drive_active &&
            camera.consume_ground_teleport(&rmb_ex, &rmb_ey, &rmb_ez)) {
            // BeamNG: the camera already resolved z from the surface (ground_follow) or left it on the plane;
            // the OMSI heightfield does not exist here (the shim tile is flat and hidden), so never fall back to it.
            if (cfg.beamng) {
                if (camera.ground_follow && camera.sample_ground_xy) {
                    if (const auto z = camera.sample_ground_xy(rmb_ex, rmb_ey)) {
                        camera.target_z = *z;
                        camera.ground_z = *z;
                        rmb_ez = *z;
                    }
                }
            } else if (const auto z = sample_terrain_editor_xy_opt(rmb_ex, rmb_ey)) {
                camera.target_z = *z;
                camera.ground_z = *z;
                rmb_ez = *z;
            }
            if (panel.current_tool() == ui::EditorTool::Tile) {
                const auto [wx, wy] = editor_to_world_xy(rmb_ex, rmb_ey);
                on_tile_rmb(wx, wy);
            } else {
                (void)request_tile_stream_on_rmb(rmb_ex, rmb_ey, rmb_ez);
            }
        }
        if (pending_create_tile) {
            pending_create_tile = false;
            if (!on_create_tile_here())
                log::warn("create_tile: on_create_tile_here failed");
        }
        editor_subphase_watch.emplace(main_watchdog.phase("input_edit_and_stream_schedule"));
        auto& load_gov = omsi::LoadGovernor::global();
        {
            static bool s_first_area_done = false;
            static bool s_seen_stream = false;
            static const auto s_boot = std::chrono::steady_clock::now();
            static auto s_last_input = std::chrono::steady_clock::now();
            const auto now_in = std::chrono::steady_clock::now();
            bool interacting = rmb_cam || mmb_cam || edit_mode != EditMode::None;
            if (ImGui::GetCurrentContext()) {
                const ImGuiIO& gio = ImGui::GetIO();
                interacting = interacting || gio.MouseDown[0] || gio.MouseDown[1] || gio.MouseDown[2] ||
                              gio.MouseWheel != 0.f ||
                              std::fabs(gio.MouseDelta.x) + std::fabs(gio.MouseDelta.y) > 0.5f ||
                              ImGui::IsAnyItemActive();
            }
            if (interacting) s_last_input = now_in;
            if (stream_busy()) s_seen_stream = true;
            if (!s_first_area_done && !stream_busy() &&
                (s_seen_stream || now_in - s_boot > std::chrono::seconds(8)))
                s_first_area_done = true;
            omsi::LoadGovernor::Mode gmode = omsi::LoadGovernor::Mode::Interactive;
            if (!s_first_area_done || stream_busy())
                gmode = omsi::LoadGovernor::Mode::Loading; // fill leftover frame while streaming
            else if (now_in - s_last_input > std::chrono::milliseconds(350))
                gmode = omsi::LoadGovernor::Mode::Idle;
            load_gov.begin_frame(g_editor_frame_wall_ms, gmode);
        }
        // Non-load frame cost: loading must not shrink its own budget.
        g_stream_last_render_ms = load_gov.base_ms();
        const auto load_t0 = std::chrono::steady_clock::now();
        if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: path_rails");
        // path_rails_refresh: run at max 15 Hz (every 4 frames) + budget limit
        static int s_prr_frame = 0;
        if (++s_prr_frame < 4) {
        } else {
            s_prr_frame = 0;
            if (!rmb_cam && !mmb_cam) {
                auto _watch = main_watchdog.phase("path_rails_refresh");
                auto _perf = frame_profiler.scope(MapEditorFrameProfiler::StreamApply);
                const double prr_budget = 2.0; // ms budget
                pump_path_rails_refresh();
            }
        }
        {
            auto _watch = main_watchdog.phase("save_job_pump");
            pump_save_job();
        }
        {
            auto _watch = main_watchdog.phase("stream_apply");
            auto _perf = frame_profiler.scope(MapEditorFrameProfiler::StreamApply);
            if (!rmb_cam && !mmb_cam)
                pump_stream_apply();
            if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: stream_apply done");
            {
                auto _w_remesh = main_watchdog.phase("stream.async_remesh");
                // (also while driving and streaming: gated on both, the roads of a map loaded with the bus already
                //  driving never went up at all - the strips' upload is byte/ms budgeted by the LoadGovernor)
                if (!rmb_cam && !mmb_cam) pump_async_remesh();
            }
            if (panel.imgui_state().hide_onlyeditor_splines != last_hide_onlyeditor_splines) {
                auto _w_vis = main_watchdog.phase("stream.onlyeditor_spline_vis");
                last_hide_onlyeditor_splines = panel.imgui_state().hide_onlyeditor_splines;
                apply_onlyeditor_spline_visibility();
            }
        }
        {
            auto _watch = main_watchdog.phase("terrain_precache");
            auto _perf = frame_profiler.scope(MapEditorFrameProfiler::TerrainPrecache);
            // [PERF] Skip terrain precache during camera orbit/pan (CAM_HITCH mitigation)
            if (!rmb_cam && !mmb_cam)
                pump_terrain_precache();
            pump_terrain_detail();
        }
        if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: terrain_precache");
        // SA after stream Finish — markers/sync must not compete with attachment GPU spawn.
        {
            auto _watch = main_watchdog.phase("spline_attachment_spawn");
            auto _perf = frame_profiler.scope(MapEditorFrameProfiler::SceneryAttachSpawn);
            const double sa_ms =
                stream_busy()
                    ? (std::min)(2.0, omsi::FrameBudget::for_stream(false, g_stream_last_render_ms, 1000.0 / 30.0, 16.0)
                                     .cpu_ms_hard *
                                     0.5)
                    : (std::min)(4.0, omsi::FrameBudget::for_stream(false, g_stream_last_render_ms, 1000.0 / 60.0, 4.0)
                                     .cpu_ms_hard *
                                     0.5);
            if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: sa_spawn");
            if (stream_phase != StreamPhase::Finish)
                pump_sa_spawn(sa_ms);
            else if (stream_finish_pass >= 7)
                pump_sa_spawn(sa_ms);
        }
        if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: post_sa");
        load_gov.note_load_ms(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_t0).count());
        {
            static auto s_gov_log = std::chrono::steady_clock::now();
            const auto now_gov = std::chrono::steady_clock::now();
            if (stream_busy() && now_gov - s_gov_log > std::chrono::seconds(2)) {
                s_gov_log = now_gov;
                log::info("LOAD_GOV " + load_gov.summary());
            }
        }
        if ((panel.random_tile_stress_enabled() || stream_stress_left > 0) && !stream_busy() &&
            edit_mode == EditMode::None && !cfg.global.tiles.empty()) {
            if (--random_hop_cd <= 0) {
                random_hop_cd = panel.random_tile_stress_enabled() ? 120 : 60;
                const auto& ref =
                    cfg.global.tiles[static_cast<std::size_t>(std::rand()) % cfg.global.tiles.size()];
                const auto layout = world.tile_layout();
                const auto origin = map::tile_origin_xy(layout, ref.grid_x, ref.grid_y);
                const float size_m =
                    map::tile_size_meters_at_grid(cfg.global, layout, ref.grid_x, ref.grid_y);
                const float wx = origin.first + size_m * 0.5f;
                const float wy = origin.second + size_m * 0.5f;
                camera.target_x = wx - tile_ox;
                camera.target_y = wy - tile_oy;
                if (const auto z = sample_terrain_editor_xy_opt(camera.target_x, camera.target_y)) {
                    camera.target_z = *z;
                    camera.ground_z = *z;
                }
                log_watch_scos("streamStressHop");
                log::info("streamStress hop tile=(" + std::to_string(ref.grid_x) + "," +
                          std::to_string(ref.grid_y) + ") center=(" + std::to_string(tile_gx) + "," +
                          std::to_string(tile_gy) + ") camEditor=(" + std::to_string(camera.target_x) + "," +
                          std::to_string(camera.target_y) + ") tile_ox=" + std::to_string(tile_ox));
                for (const auto& o : edit_objects) {
                    if (o.id != 90 && o.id != 91 && o.id != 92) continue;
                    const float wx = o.position.x + tile_ox;
                    const float wy = o.position.y + tile_oy;
                    std::int32_t tgx = ref.grid_x, tgy = ref.grid_y;
                    for (const auto& sp : scene_places)
                        if (sp.alive && sp.object_id == o.id) {
                            tgx = sp.tile_gx;
                            tgy = sp.tile_gy;
                            break;
                        }
                    const auto tile_o = map::tile_origin_xy(world.tile_layout(), tgx, tgy);
                    log::info("streamStress obj=" + std::to_string(o.id) + " editor=(" +
                              std::to_string(o.position.x) + "," + std::to_string(o.position.y) + ")" +
                              " world=(" + std::to_string(wx) + "," + std::to_string(wy) + ")" +
                              " local@(" + std::to_string(tgx) + "," + std::to_string(tgy) + ")=(" +
                              std::to_string(wx - tile_o.first) + "," + std::to_string(wy - tile_o.second) +
                              ")");
                }
                if (stream_stress_left > 0) --stream_stress_left;
            }
        }
        pump_group_settle();
        if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: post_group_settle");

        if (marquee.phase != MarqueePhase::None && !over_ui) {
            float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
            input::cursor_to_framebuffer(window, mx, my, fw, fh);
            const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
            float wvp_m[16]{};
            camera.build_world_view_proj(aspect, wvp_m);
            if (marquee.phase == MarqueePhase::Select && marquee.dragging) {
                marquee.sx1 = mx;
                marquee.sy1 = my;
                // Circle: paint frame-a-frame (add/remove) without leaving the mode.
                // Lasso: accumulate polyline; select on release. Recuadro: grow box, select on release.
                if (sel_shape == SelShapeMode::Lasso || sel_shape == SelShapeMode::Circle) {
                    if (sel_lasso_pts.empty() ||
                        (mx - sel_lasso_pts.back().x) * (mx - sel_lasso_pts.back().x) +
                                (my - sel_lasso_pts.back().y) * (my - sel_lasso_pts.back().y) >
                            9.f)
                        sel_lasso_pts.push_back({mx, my});
                }
                if (sel_shape == SelShapeMode::Circle) {
                    const bool remove = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    // Cap work per frame so huge scenes don't stall the main thread.
                    std::size_t budget = 8;
                    while (sel_circle_brush_upto < sel_lasso_pts.size() && budget > 0) {
                        const auto& p = sel_lasso_pts[sel_circle_brush_upto++];
                        apply_circle_brush_at(p.x, p.y, fw, fh, wvp_m, remove);
                        --budget;
                    }
                    // Always paint under the live cursor this frame.
                    apply_circle_brush_at(mx, my, fw, fh, wvp_m, remove);
                }
                update_marquee_box(fw, fh, wvp_m);
            } else if (marquee.phase == MarqueePhase::Placing && !stream_busy()) {
                camera.ground_z = camera.target_z;
                if (const auto hit = camera.hit_ground_xy(mx, my, fw, fh)) update_marquee_placing(hit->x, hit->y);
            }
        }

        // Grab: XY only (keep Z offset + rotation). Rotate: yaw from mouse around object.
        if (edit_mode != EditMode::None && !over_ui) {
            float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
            input::cursor_to_framebuffer(window, mx, my, fw, fh);
            camera.ground_z = camera.target_z;
            if (edit_mode == EditMode::GrabHeight) {
                const float my_v = (coop_walk_active && coop_walk_edit) ? -walk.pitch * 400.f : my;
                const float dz = (grab_height_my0 - my_v) * 0.025f;
                if (node_height_mode && grab_spline_id != 0) {
                    auto* sp = find_spline_by_id(grab_spline_id);
                    if (sp) {
                        // Absolute from snapshot (restore then apply) so the drag is not cumulative.
                        *sp = node_grab_snap_primary;
                        if (node_grab_snap_has_prev)
                            if (auto* p = find_spline_by_id(node_grab_snap_prev.id)) *p = node_grab_snap_prev;
                        if (node_grab_snap_has_next)
                            if (auto* n = find_spline_by_id(node_grab_snap_next.id)) *n = node_grab_snap_next;
                        int kind = 0;
                        if (spline_grab_target == 4) kind = 1;      // start (no prev)
                        else if (spline_grab_target == 1) kind = 3; // middle -> grads
                        else if (spline_grab_target == 3) kind = sp->next_id ? 0 : 2;
                        apply_node_delta_h(*sp, kind, dz);
                        remesh_spline(*sp, true);
                        if (node_grab_snap_has_prev)
                            if (auto* p = find_spline_by_id(node_grab_snap_prev.id)) remesh_spline(*p, true);
                        if (node_grab_snap_has_next)
                            if (auto* n = find_spline_by_id(node_grab_snap_next.id)) remesh_spline(*n, true);
                    }
                } else if (shift_s_move.active) {
                    static double last_ss_h = 0.0;
                    const double now_h = glfwGetTime();
                    const bool big = shift_s_move.items.size() > 64;
                    if (!(big && (now_h - last_ss_h) < 0.033)) {
                        last_ss_h = now_h;
                        shift_s_move_apply(shift_s_move.cur_dx, shift_s_move.cur_dy, dz, true, true);
                    }
                } else if (group_grab_active) {
                    for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                        if (group_grab_kind[i] == 0) {
                            auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                            o.position.x = grab_start_x + group_off_x[i];
                            o.position.y = grab_start_y + group_off_y[i];
                            o.position.z = group_grab_start_z[i] + dz;
                            sync_object_gpu(o);
                        } else {
                            auto& s = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                            s.position.x = grab_start_x + group_off_x[i];
                            s.position.y = grab_start_y + group_off_y[i];
                            s.position.z = group_grab_start_z[i] + dz;
                            remesh_spline(s, true);
                        }
                    }
                } else if (group_rotate_active) {
                    for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                        if (group_grab_kind[i] == 0) {
                            auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                            o.position.z = group_grab_start_z[i] + dz;
                            sync_object_gpu(o);
                        } else {
                            auto& s = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                            s.position.z = group_grab_start_z[i] + dz;
                            remesh_spline(s, true);
                        }
                    }
                } else if (selected_object >= 0 &&
                    static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                    auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                    if (spline_attach_spawn_ids.count(o.id)) {
                        if (const auto sa_it = spline_attach_obj_to_entry.find(o.id);
                            sa_it != spline_attach_obj_to_entry.end()) {
                            auto& entry = edit_spline_attachments[sa_it->second];
                            entry.att.offset_y = grab_start_sa_offset_y + dz;
                            update_spline_attachment_entry_poses(sa_it->second);
                        }
                    } else {
                        o.position.x = grab_start_x;
                        o.position.y = grab_start_y;
                        o.position.z = grab_start_z + dz;
                        sync_object_gpu(o);
                        show_object_at(static_cast<std::size_t>(selected_object));
                    }
                } else if (selected_spline >= 0 &&
                           static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    s.position.x = grab_start_x;
                    s.position.y = grab_start_y;
                    s.position.z = grab_start_z + dz;
                    remesh_spline(s, true);
                    update_spline_panel_live(static_cast<std::size_t>(selected_spline));
                }
                adjust_hud.set_current(current_ref_z());
                if (panel.imgui_primary() && adjust_hud_imgui.active && !adjust_hud_imgui.user_editing)
                    adjust_hud_imgui.current = current_ref_z();
                } else if (edit_mode == EditMode::EditSplineGrad && selected_spline >= 0 &&
                           static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    const float new_g =
                        (std::clamp)(grad_edit_base + (grab_height_my0 - my) * 0.04f, -100.f, 100.f);
                    if (grad_edit_is_start) s.start_grad = new_g;
                    else s.end_grad = new_g;
                    remesh_spline(s, true);
                    update_spline_panel_live(static_cast<std::size_t>(selected_spline));
                } else if (const auto hit = [&]() {
                               // walk-edit: the first-person camera's ground plane is at eye height; the
                               // crosshair's aim point (walk tick) is where the object goes
                               auto h = camera.hit_ground_xy(mx, my, fw, fh);
                               if (coop_walk_active && coop_walk_edit) {
                                   if (!h) h.emplace();
                                   h->x = walk_aim[0];
                                   h->y = walk_aim[1];
                                   h->z = walk_aim[2];
                               }
                               return h;
                           }()) {
                if ((sco_path_edit.moving || sco_path_edit.following || sco_path_edit.placing) &&
                    (edit_mode == EditMode::Grab || edit_mode == EditMode::SplineFollow) &&
                    sco_path_edit.sel >= 0) {
                    apply_sco_path_hit(hit->x, hit->y, hit->z);
                } else if (edit_mode == EditMode::Grab) {
                    const bool ctrl_grab = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    static bool path_align_undo_armed = false;
                    bool path_align_this_frame = false;
                    if (ctrl_grab && (selected_spline >= 0 || selected_object >= 0)) {
                        const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                        float wvp_align[16]{};
                        camera.build_world_view_proj(aspect, wvp_align);
                        const auto pp = pick_path_endpoint_near_mouse(mx, my, fw, fh, wvp_align);
                        if (pp.valid) {
                            static PathEndpointPick last_align_pick{};
                            static auto last_align_remesh = std::chrono::steady_clock::time_point{};
                            const auto now_align = std::chrono::steady_clock::now();
                            const bool pick_changed =
                                !last_align_pick.valid || pp.owner_id != last_align_pick.owner_id ||
                                pp.from_object != last_align_pick.from_object ||
                                pp.path_index != last_align_pick.path_index || pp.is_start != last_align_pick.is_start ||
                                std::fabs(pp.x - last_align_pick.x) > 1.e-4f ||
                                std::fabs(pp.y - last_align_pick.y) > 1.e-4f;
                            const bool due = last_align_remesh.time_since_epoch().count() == 0 ||
                                             std::chrono::duration_cast<std::chrono::milliseconds>(now_align -
                                                                                                   last_align_remesh)
                                                     .count() >= 80;
                            if (!path_align_undo_armed) {
                                push_undo("path_align_grab");
                                path_align_undo_armed = true;
                            }
                            if (pick_changed || due) {
                                apply_path_align_grab(pp);
                                last_align_pick = pp;
                                last_align_remesh = now_align;
                            }
                            path_align_this_frame = true;
                        } else {
                            path_align_undo_armed = false;
                        }
                    } else {
                        path_align_undo_armed = false;
                    }
                    if (!path_align_this_frame) {
                    if (shift_s_move.active) {
                        static double last_ss_xy = 0.0;
                        const double now_g = glfwGetTime();
                        const bool big = shift_s_move.items.size() > 64;
                        if (!shift_s_move.have_ref) {
                            shift_s_move.ref_mx = hit->x;
                            shift_s_move.ref_my = hit->y;
                            shift_s_move.have_ref = true;
                        }
                        if (!(big && (now_g - last_ss_xy) < 0.033)) {
                            last_ss_xy = now_g;
                            const float dx = hit->x - shift_s_move.ref_mx;
                            const float dy = hit->y - shift_s_move.ref_my;
                            // One shared delta for every id → distances preserved exactly.
                            shift_s_move_apply(dx, dy, 0.f, true, false);
                        }
                    } else if (group_grab_active) {
                        static double last_group_grab_upd = 0.0;
                        const double now_g = glfwGetTime();
                        const bool big = group_grab_index.size() > 64;
                        if (!group_grab_have_mouse_ref) {
                            group_grab_ref_mx = hit->x;
                            group_grab_ref_my = hit->y;
                            group_grab_have_mouse_ref = true;
                            xform_soa_grab_last_dx = 0.f;
                            xform_soa_grab_last_dy = 0.f;
                            xform_soa.rebuild_from(edit_objects);
                        }
                        if (!(big && (now_g - last_group_grab_upd) < 0.033)) {
                        last_group_grab_upd = now_g;
                        const float dx = hit->x - group_grab_ref_mx;
                        const float dy = hit->y - group_grab_ref_my;
                        static std::vector<std::int32_t> soa_ids;
                        soa_ids.clear();
                        if (soa_ids.capacity() < group_grab_ids.size()) soa_ids.reserve(group_grab_ids.size());
                        for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                            if (group_grab_kind[i] == 0 && i < group_grab_ids.size())
                                soa_ids.push_back(group_grab_ids[i]);
                        }
                        xform_soa.apply_delta(soa_ids, dx - xform_soa_grab_last_dx, dy - xform_soa_grab_last_dy);
                        xform_soa_grab_last_dx = dx;
                        xform_soa_grab_last_dy = dy;
                        for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                            const float nx = group_grab_mesh_ox[i] + dx;
                            const float ny = group_grab_mesh_oy[i] + dy;
                            if (group_grab_kind[i] == 0) {
                                auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                                if (const auto it = xform_soa.id_to_i.find(o.id); it != xform_soa.id_to_i.end()) {
                                    o.position.x = xform_soa.x[it->second];
                                    o.position.y = xform_soa.y[it->second];
                                } else {
                                    o.position.x = nx;
                                    o.position.y = ny;
                                }
                                for (auto& sp : scene_places) {
                                    if (sp.object_id == o.id && sp.alive) {
                                        const float tdx2 = o.position.x - sp.tx;
                                        const float tdy2 = o.position.y - sp.ty;
                                        sp.tx = o.position.x;
                                        sp.ty = o.position.y;
                                        renderer.translate_chunk(sp.chunk, tdx2, tdy2, 0.f);
                                    }
                                }
                            } else {
                                auto& s = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                                s.position.x = nx;
                                s.position.y = ny;
                                // Mesh baked at start; one shared world translation (dx,dy).
                                set_spline_chunk_offset(s.id, dx, dy, 0.f);
                                set_path_rails_offset(s.id, false, dx, dy, 0.f);
                            }
                        }
                        } // group grab throttle
                    } else if (selected_entrypoint >= 0 &&
                               static_cast<std::size_t>(selected_entrypoint) < entry_chunks.size()) {
                        auto& ec = entry_chunks[static_cast<std::size_t>(selected_entrypoint)];
                        if (ec.ep_index >= 0 &&
                            static_cast<std::size_t>(ec.ep_index) < cfg.global.entrypoints.size()) {
                            auto& ep = cfg.global.entrypoints[static_cast<std::size_t>(ec.ep_index)];
                            ep.position.x = hit->x;
                            ep.position.y = hit->y;
                            // Keep local pose coherent (worldcoords maps use local + category).
                            ep.position_local.x = hit->x;
                            ep.position_local.y = hit->y;
                            refresh_entry_matrix(ec);
                        }
                    } else if (selected_object >= 0 &&
                        static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                        auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                        float gx = hit->x, gy = hit->y;
                        if (obj_grab_axis == 0 || obj_grab_axis == 1) {
                            // X / Y lock: the point on the axis line through the G start nearest the cursor
                            float ax = obj_grab_axis == 0 ? 1.f : 0.f, ay = obj_grab_axis == 1 ? 1.f : 0.f;
                            if (obj_grab_axis_local) {
                                // object axes in world (omsi_object_rotation_matrix: RotationZ(-heading))
                                const float a = grab_start_yaw * (3.14159265f / 180.f);
                                if (obj_grab_axis == 0) ax = std::cos(a), ay = -std::sin(a);
                                else ax = std::sin(a), ay = std::cos(a);
                            }
                            const float t = (gx - grab_start_x) * ax + (gy - grab_start_y) * ay;
                            gx = grab_start_x + ax * t;
                            gy = grab_start_y + ay * t;
                        }
                        if (spline_attach_spawn_ids.count(o.id))
                            apply_spline_attach_follow_xy(o, gx, gy);
                        else
                            apply_sco_follow_xy(o, gx, gy);
                    } else if (selected_spline >= 0 &&
                               static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                        // CRÍTICO: no mantener referencia viva a edit_splines — fill_to_fixed_end /
                        // ensure_temp hacen push_back y invalidan `s` → AV 0xC0000005 en
                        // set_spline_chunk_offset(s.id, ...) al final de este bloque.
                        const std::int32_t grab_sid = edit_splines[static_cast<std::size_t>(selected_spline)].id;
                        auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                        const float max_r = panel.spline_max_radius();
                        const bool stick = panel.imgui_state().spline_stick_chain;

                        if (spline_grab_target == 1) {
                            // Centro: cadena → solo gradientes; aislada → rot/L/R extremos fijos.
                            const bool chained = (s.prev_id != 0 && s.next_id != 0);
                            if (chained || edit_mode == EditMode::GrabHeight)
                                fit_isolated_center_drag(s, hit->x, hit->y, hit->z, 3, max_r);
                            else
                                fit_isolated_center_drag(s, hit->x, hit->y, hit->z, 0, max_r);
                            remesh_spline_one(s, true, false);
                        } else if (spline_grab_target == 3) {
                            // Ejemplo B: nodo final libre — RR (start fijo, fin → mouse)
                            float L = 0.f, R = 0.f;
                            spline::fit_spline_to_xy(s.position.x, s.position.y, s.heading_deg,
                                                     hit->x, hit->y, L, R);
                            if (L < 0.25f) L = 0.25f;
                            if (max_r > 0.f && std::fabs(R) > max_r) {
                                const float sign = (R >= 0.f) ? 1.f : -1.f;
                                R = sign * max_r;
                                spline::fit_spline_to_xy(s.position.x, s.position.y, s.heading_deg,
                                                         hit->x, hit->y, L, R);
                                if (L < 0.25f) L = 0.25f;
                            }
                            s.length = L;
                            s.radius = R;
                            remesh_spline_one(s, true, false);
                        } else if (spline_grab_target == 4) {
                            // Start libre → mouse; fin fijo (XYZ + heading) vía biarc.
                            // SOLO por grab_sid: fill_to_fixed_end puede push_back y matar `s`.
                            const float fex = chain_fix_next_ex;
                            const float fey = chain_fix_next_ey;
                            const float feh = chain_fix_next_eh;
                            std::filesystem::path keep_sli;
                            if (auto* sp0 = find_spline_by_id(grab_sid))
                                keep_sli = sp0->sli_path;
                            float best_h0 = grab_start_yaw;
                            float best_score = 1.e30f;
                            for (int k = 0; k < 36; ++k) {
                                const float h0 = grab_start_yaw + static_cast<float>(k - 18) * 10.f;
                                float L = 0.f, R = 0.f;
                                if (!spline::fit_spline_to_end_heading(hit->x, hit->y, h0, fex, fey, feh, L,
                                                                        R) ||
                                    L < 0.25f)
                                    continue;
                                map::SplineInst tmp{};
                                tmp.position.x = hit->x;
                                tmp.position.y = hit->y;
                                tmp.position.z = grab_start_z;
                                tmp.heading_deg = h0;
                                tmp.length = L;
                                tmp.radius = R;
                                const auto e = spline::evaluate_spline(tmp, tmp.length);
                                const float gap = std::hypot(e.x - fex, e.y - fey);
                                float eh = e.heading_rad * (180.f / 3.14159265f) - feh;
                                while (eh > 180.f) eh -= 360.f;
                                while (eh < -180.f) eh += 360.f;
                                const float score = gap * 25.f + std::fabs(eh);
                                if (score < best_score) {
                                    best_score = score;
                                    best_h0 = h0;
                                    if (score < 0.5f) break;
                                }
                            }
                            park_fill_temps();
                            if (auto* sp = find_spline_by_id(grab_sid)) {
                                sp->position.x = hit->x;
                                sp->position.y = hit->y;
                                sp->position.z = grab_start_z;
                                sp->heading_deg = best_h0;
                                sp->sli_path = keep_sli;
                            }
                            // head=0 → reshape grab_sid desde mouse hasta fin fijo
                            fill_to_fixed_end(0, grab_sid, fex, fey, feh, max_r, 2);
                            // Tras fill: NUNCA tocar `s` (puede estar invalidado)
                            if (auto* sp = find_spline_by_id(grab_sid)) {
                                sp->sli_path = keep_sli;
                                remesh_spline_one(*sp, true, false);
                            } else {
                                log::warn("[nodo] target4: grab_sid=" + std::to_string(grab_sid) +
                                          " missing after fill");
                            }
                            for (std::int32_t tid : fill_temp_ids) {
                                if (auto* ts = find_spline_by_id(tid)) {
                                    if (ts->sli_path.empty()) ts->sli_path = keep_sli;
                                    remesh_spline_one(*ts, true, false);
                                }
                            }
                            log::info("[nodo] target4 done sid=" + std::to_string(grab_sid) +
                                      " temps=" + std::to_string(fill_temp_ids.size()) +
                                      " edit_n=" + std::to_string(edit_splines.size()));
                        } else if (spline_grab_target == 2) {
                            // Joint libre en mouse.
                            // - 1 ender (A): RR de A + biarc de cada starter B (fin fijo).
                            // - N enders (A1..An): biarc de TODAS las A (start fijo → joint)
                            //   + biarc de cada B (joint → fin fijo).
                            try {
                            for (std::size_t li = 0; li < node_sli_lock_ids.size(); ++li) {
                                if (auto* p = find_spline_by_id(node_sli_lock_ids[li])) {
                                    if (p->sli_path != node_sli_lock_paths[li]) {
                                        log::warn("[nodo] sli override blocked id=" +
                                                  std::to_string(p->id) + " kept=" +
                                                  node_sli_lock_paths[li].filename().string());
                                        p->sli_path = node_sli_lock_paths[li];
                                    }
                                }
                            }
                            // NUNCA guardar puntero a edit_splines: ensure_temp hace push_back
                            // y invalida referencias → AV 0xC0000005.
                            const std::int32_t primary_id = grab_sid; // no usar s.id tras posible realloc
                            log::info("[nodo] G target2 begin primary=" + std::to_string(primary_id) +
                                      " multi_end_cand=" + std::to_string(node_ender_ids.size()) +
                                      " starters=" + std::to_string(node_starter_ids.size()) +
                                      " edit_n=" + std::to_string(edit_splines.size()) +
                                      " temps=" + std::to_string(fill_temp_ids.size()) +
                                      " pool=" + std::to_string(fill_temp_pool.size()));
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
                                    if (si < node_starter_fex.size())
                                        node_starter_fex.erase(node_starter_fex.begin() +
                                                               static_cast<std::ptrdiff_t>(si));
                                    if (si < node_starter_fey.size())
                                        node_starter_fey.erase(node_starter_fey.begin() +
                                                               static_cast<std::ptrdiff_t>(si));
                                    if (si < node_starter_feh.size())
                                        node_starter_feh.erase(node_starter_feh.begin() +
                                                               static_cast<std::ptrdiff_t>(si));
                                } else
                                    ++si;
                            }
                            const bool multi_end = node_ender_ids.size() > 1;
                            float jx = hit->x, jy = hit->y, jz = hit->z;
                            float jh = node_joint0_valid ? node_joint0_h : s.heading_deg;
                            park_fill_temps();

                            if (!multi_end) {
                                if (auto* a = find_spline_by_id(primary_id)) {
                                    float L = 0.f, R = 0.f;
                                    spline::fit_spline_to_xy(a->position.x, a->position.y, a->heading_deg,
                                                             hit->x, hit->y, L, R);
                                    if (L < 0.25f) L = 0.25f;
                                    if (max_r > 0.f && std::fabs(R) > max_r) {
                                        const float sign = (R >= 0.f) ? 1.f : -1.f;
                                        R = sign * max_r;
                                        spline::fit_spline_to_xy(a->position.x, a->position.y,
                                                                 a->heading_deg, hit->x, hit->y, L, R);
                                        if (L < 0.25f) L = 0.25f;
                                    }
                                    a->length = L;
                                    a->radius = R;
                                    // no remesh aquí — se hace al final por id (evita doble trabajo)
                                    const auto joint = spline::evaluate_spline(*a, a->length);
                                    jx = joint.x;
                                    jy = joint.y;
                                    jz = joint.z;
                                    jh = joint.heading_rad * (180.f / 3.14159265f);
                                    log::info("[nodo] single-end RR primary=" + std::to_string(primary_id) +
                                              " L=" + std::to_string(L) + " R=" + std::to_string(R) +
                                              " joint=(" + std::to_string(jx) + "," + std::to_string(jy) +
                                              ") jh=" + std::to_string(jh));
                                }
                            } else {
                                // Multi-end: biarc de CADA A (start fijo → joint), no RR simple.
                                // RR de un solo segmento no puede cumplir pos+heading cuando dh es
                                // grande → las otras A solo se "estiraban" y el biarc de B fallaba.
                                if (!node_starter_ids.empty() && node_joint0_valid)
                                    jh = node_joint0_h;
                                jx = hit->x;
                                jy = hit->y;
                                jz = hit->z;
                                for (std::size_t ei = 0; ei < node_ender_ids.size(); ++ei) {
                                    const std::int32_t eid = node_ender_ids[ei];
                                    auto* en = find_spline_by_id(eid);
                                    if (!en) continue;
                                    const std::filesystem::path keep = en->sli_path;
                                    // Restaurar start fijo del ender (capturado al iniciar G)
                                    if (ei < node_ender_sx.size()) {
                                        en->position.x = node_ender_sx[ei];
                                        en->position.y = node_ender_sy[ei];
                                        en->position.z = node_ender_sz[ei];
                                        en->heading_deg = node_ender_sh[ei];
                                    }
                                    // head_id=0 → origen = start actual del tail; reshape tail
                                    // (+temps) hasta el joint (jx,jy,jh). Solo ids → sin AV.
                                    fill_to_fixed_end(0, eid, jx, jy, jh, max_r, 2);
                                    if (auto* e2 = find_spline_by_id(eid)) {
                                        e2->sli_path = keep;
                                        // No remesh aquí: se hace al final con todos los temps
                                    }
                                    // Restaurar sli de temps recién creados para este ender
                                    for (std::int32_t tid : fill_temp_ids) {
                                        if (auto* ts = find_spline_by_id(tid)) {
                                            if (ts->sli_path.empty()) ts->sli_path = keep;
                                        }
                                    }
                                }
                                // Tras biarcs de As, joint efectivo = evaluate(primary).end
                                // (mejor continuidad de heading que el mouse crudo).
                                if (auto* a = find_spline_by_id(primary_id)) {
                                    const auto je = spline::evaluate_spline(*a, a->length);
                                    jx = je.x;
                                    jy = je.y;
                                    jz = je.z;
                                    jh = je.heading_rad * (180.f / 3.14159265f);
                                }
                            }

                            // Starters: desde joint (pos+heading de primary) → fin fijo de cada B.
                            // head = primary para linkear next/prev del primary; fill solo por ids.
                            for (std::size_t si = 0; si < node_starter_ids.size(); ++si) {
                                if (si >= node_starter_fex.size()) continue;
                                const std::int32_t bid = node_starter_ids[si];
                                auto* b0 = find_spline_by_id(bid);
                                if (!b0) continue;
                                const std::filesystem::path keep = b0->sli_path;
                                // Colocar start de B en el joint actual (después de biarcs A)
                                b0->position.x = jx;
                                b0->position.y = jy;
                                b0->position.z = jz;
                                b0->heading_deg = jh;
                                if (auto* a = find_spline_by_id(primary_id)) {
                                    const auto ae = spline::evaluate_spline(*a, a->length);
                                    log::info("[nodo] pose A id=" + std::to_string(a->id) +
                                              " start=(" + std::to_string(a->position.x) + "," +
                                              std::to_string(a->position.y) + "," +
                                              std::to_string(a->position.z) + ") h=" +
                                              std::to_string(a->heading_deg) + " L=" +
                                              std::to_string(a->length) + " R=" +
                                              std::to_string(a->radius) + " end=(" +
                                              std::to_string(ae.x) + "," + std::to_string(ae.y) +
                                              ") eh=" +
                                              std::to_string(ae.heading_rad * (180.f / 3.14159265f)));
                                }
                                log::info("[nodo] pose B id=" + std::to_string(bid) + " start=(" +
                                          std::to_string(jx) + "," + std::to_string(jy) + "," +
                                          std::to_string(jz) + ") h=" + std::to_string(jh) +
                                          " fe=(" + std::to_string(node_starter_fex[si]) + "," +
                                          std::to_string(node_starter_fey[si]) + ") feh=" +
                                          std::to_string(node_starter_feh[si]));
                                fill_to_fixed_end(primary_id, bid, node_starter_fex[si],
                                                  node_starter_fey[si], node_starter_feh[si], max_r, 2);
                                if (auto* b = find_spline_by_id(bid)) {
                                    b->sli_path = keep;
                                }
                            }
                            // Remesh enders + starters + temps (todo por id, sin refs vivas)
                            {
                                std::unordered_set<std::int32_t> remesh_ids;
                                for (std::int32_t eid : node_ender_ids) remesh_ids.insert(eid);
                                for (std::int32_t bid : node_starter_ids) remesh_ids.insert(bid);
                                for (std::int32_t tid : fill_temp_ids) remesh_ids.insert(tid);
                                remesh_ids.insert(primary_id);
                                log::info("[nodo] G target2 remesh n=" + std::to_string(remesh_ids.size()) +
                                          " temps=" + std::to_string(fill_temp_ids.size()) +
                                          " pool=" + std::to_string(fill_temp_pool.size()) +
                                          " edit_splines=" + std::to_string(edit_splines.size()) +
                                          " primary=" + std::to_string(primary_id));
                                for (std::int32_t id : remesh_ids) {
                                    auto* rs = find_spline_by_id(id);
                                    if (!rs) {
                                        log::warn("[nodo] remesh skip missing id=" + std::to_string(id));
                                        continue;
                                    }
                                    try {
                                        remesh_spline_one(*rs, true, false);
                                    } catch (const std::exception& ex) {
                                        log::error("[nodo] remesh id=" + std::to_string(id) +
                                                   " exception: " + ex.what());
                                    } catch (...) {
                                        log::error("[nodo] remesh id=" + std::to_string(id) +
                                                   " unknown exception");
                                    }
                                }
                                log::info("[nodo] G target2 remesh done");
                            }
                            } catch (const std::exception& ex) {
                                log::error(std::string("[nodo] exception en G target2: ") + ex.what());
                            } catch (...) {
                                log::error("[nodo] exception desconocida en G target2");
                            }
                        } else if (stick && (s.prev_id != 0 || s.next_id != 0)) {
                            const bool fill = panel.imgui_state().spline_fill_chain;
                            // SIMPLE G+fill: move mid rigidly, AA prev→mid, AA mid→fixed end.
                            // No rigid clamp, no multi-walk — those fought each other and limited motion.
                            const std::int32_t sid = s.id;
                            const std::int32_t prev_keep = (chain_fix_prev_id != 0) ? chain_fix_prev_id : s.prev_id;
                            const std::int32_t next_keep = (chain_fix_next_id != 0) ? chain_fix_next_id : s.next_id;

                            // 1) Place mid at cursor (keep L/R/heading from G-start)
                            if (auto* sp = find_spline_by_id(sid)) {
                                sp->length = grab_start_len;
                                sp->radius = grab_start_radius;
                                sp->heading_deg = grab_start_yaw;
                                sp->position.x = hit->x;
                                sp->position.y = hit->y;
                            }

                            if (fill) {
                                // Park (unlink) but keep temps for reuse — avoids mesh flicker
                                park_fill_temps(); // active → pool; fill_temp_ids cleared
                                // 2) AA: end of prev → start of mid
                                if (prev_keep != 0) {
                                    if (auto* prev = find_spline_by_id(prev_keep)) {
                                        if (auto* sp = find_spline_by_id(sid)) {
                                            // Break old link; fill will relink
                                            prev->next_id = 0;
                                            sp->prev_id = 0;
                                            fill_bridge_between(*prev, *sp, *prev, max_r, 2);
                                            // Exact join: mid.start = evaluate(prev-chain tail).end
                                            if (auto* mid = find_spline_by_id(sid)) {
                                                map::SplineInst* tail = nullptr;
                                                if (mid->prev_id != 0) tail = find_spline_by_id(mid->prev_id);
                                                if (!tail) tail = find_spline_by_id(prev_keep);
                                                if (tail) {
                                                    const auto e = spline::evaluate_spline(*tail, tail->length);
                                                    mid->position.x = e.x;
                                                    mid->position.y = e.y;
                                                    mid->position.z = e.z;
                                                    // heading of mid stays (rigid); position exact
                                                }
                                            }
                                        }
                                    }
                                }
                                // 3) AA: end of mid → fixed end of old next (or next.start)
                                if (next_keep != 0) {
                                    if (auto* sp = find_spline_by_id(sid)) {
                                        if (auto* nxt = find_spline_by_id(next_keep)) {
                                            if (chain_fix_next_valid) {
                                                // Bridge mid → a temp tail that ends at fixed pose
                                                // Reuse fill_to_fixed_end (simple biarc path inside)
                                                fill_to_fixed_end(sid, next_keep, chain_fix_next_ex,
                                                                  chain_fix_next_ey, chain_fix_next_eh,
                                                                  max_r, 2);
                                            } else {
                                                // No fixed end: just attach next start to mid end
                                                const auto e = spline::evaluate_spline(*sp, sp->length);
                                                nxt->position.x = e.x;
                                                nxt->position.y = e.y;
                                                nxt->position.z = e.z;
                                                nxt->heading_deg = e.heading_rad * (180.f / 3.14159265f);
                                                sp->next_id = nxt->id;
                                                nxt->prev_id = sp->id;
                                            }
                                        }
                                    }
                                }
                                // Drop pool leftovers not taken by AA this frame
                                purge_unused_fill_temps();
                            } else {
                                // stick without fill (solo pegar cadena):
                                // - Ajustar el joint prev→mid (fit L/R del prev).
                                // - Propagar TODA la cadena siguiente: cada next se coloca
                                //   en el final del anterior (pos + heading del joint).
                                //   No se fija a posiciones absolutas previas ni se insertan
                                //   temps/biarcs; solo se reencadenan next_id / prev_id y se
                                //   arrastra la geometría relativa al final del spline editado.
                                if (auto* sp = find_spline_by_id(sid)) {
                                    if (sp->prev_id != 0) {
                                        if (auto* prev = find_spline_by_id(sp->prev_id)) {
                                            float L = 0.f, R = 0.f;
                                            if (spline::fit_spline_to_end_heading(
                                                    prev->position.x, prev->position.y, prev->heading_deg,
                                                    sp->position.x, sp->position.y, sp->heading_deg, L, R) &&
                                                L >= 0.25f &&
                                                (max_r <= 0.f || std::fabs(R) <= max_r * 1.05f)) {
                                                prev->length = L;
                                                prev->radius = R;
                                            } else {
                                                spline::fit_spline_to_xy(
                                                    prev->position.x, prev->position.y, prev->heading_deg,
                                                    sp->position.x, sp->position.y, L, R);
                                                if (L < 0.25f) L = 0.25f;
                                                prev->length = L;
                                                prev->radius = R;
                                            }
                                            const auto e = spline::evaluate_spline(*prev, prev->length);
                                            sp->position.x = e.x;
                                            sp->position.y = e.y;
                                            sp->position.z = e.z;
                                        }
                                    }
                                    // Propagar toda la cadena forward desde mid
                                    {
                                        std::unordered_set<std::int32_t> seen_fwd;
                                        map::SplineInst* cur = sp;
                                        for (int hop = 0; hop < 256 && cur; ++hop) {
                                            if (!seen_fwd.insert(cur->id).second) break;
                                            if (cur->next_id == 0) break;
                                            map::SplineInst* nxt = find_spline_by_id(cur->next_id);
                                            if (!nxt) break;
                                            // Colocar inicio de next en el final de cur
                                            const auto e = spline::evaluate_spline(*cur, cur->length);
                                            nxt->position.x = e.x;
                                            nxt->position.y = e.y;
                                            nxt->position.z = e.z;
                                            nxt->heading_deg = e.heading_rad * (180.f / 3.14159265f);
                                            // Asegurar links (por si se habían roto)
                                            cur->next_id = nxt->id;
                                            nxt->prev_id = cur->id;
                                            cur = nxt;
                                        }
                                    }
                                }
                            }
                            // Debug dump full chains once every ~15 frames
                            {
                                static int dump_cd = 0;
                                if ((++dump_cd % 15) == 0) {
                                    dump_fill_chain("prev-mid", prev_keep != 0 ? prev_keep : sid, sid);
                                    if (next_keep != 0)
                                        dump_fill_chain("mid-next", sid, next_keep);
                                    log::info("[chain_dbg] fill_temp_ids count=" +
                                              std::to_string(fill_temp_ids.size()));
                                    for (std::int32_t tid : fill_temp_ids) {
                                        if (auto* ts = find_spline_by_id(tid)) {
                                            const auto e = spline::evaluate_spline(*ts, ts->length);
                                            log::info("[chain_dbg] TEMP id=" + std::to_string(tid) +
                                                      " start=(" + std::to_string(ts->position.x) + "," +
                                                      std::to_string(ts->position.y) + ") h=" +
                                                      std::to_string(ts->heading_deg) +
                                                      " L=" + std::to_string(ts->length) +
                                                      " R=" + std::to_string(ts->radius) +
                                                      " end=(" + std::to_string(e.x) + "," +
                                                      std::to_string(e.y) + ")");
                                        } else {
                                            log::info("[chain_dbg] TEMP id=" + std::to_string(tid) + " MISSING");
                                        }
                                    }
                                }
                            }
                            // Preview remesh: toda la cadena tocada (prev real + temps + mid + next…).
                            // Sin fill: también toda la cadena forward (puede ser larga).
                            // Antes solo se remesheaba sp->prev_id (a menudo un TEMP), y el prev
                            // original (L/R ya cambiados) seguía con malla vieja → wireframe mal
                            // hasta el click/PLACE.
                            {
                                std::unordered_set<std::int32_t> remesh_ids;
                                if (prev_keep != 0) remesh_ids.insert(prev_keep);
                                if (next_keep != 0) remesh_ids.insert(next_keep);
                                remesh_ids.insert(sid);
                                for (std::int32_t tid : fill_temp_ids) remesh_ids.insert(tid);
                                // Walk chain from prev_keep (or sid) forward.
                                // Con fill: hasta next_keep / 16 hops (temps cortos).
                                // Sin fill: toda la cadena siguiente (hasta 256).
                                const int max_hops = fill ? 16 : 256;
                                std::int32_t walk = (prev_keep != 0) ? prev_keep : sid;
                                for (int hop = 0; hop < max_hops && walk != 0; ++hop) {
                                    remesh_ids.insert(walk);
                                    auto* w = find_spline_by_id(walk);
                                    if (!w) break;
                                    if (fill && walk == next_keep) break;
                                    walk = w->next_id;
                                }
                                for (std::int32_t id : remesh_ids) {
                                    if (auto* rs = find_spline_by_id(id)) {
                                        set_spline_chunk_offset(rs->id, 0.f, 0.f, 0.f);
                                        remesh_spline_one(*rs, true, false);
                                    }
                                }
                            }
                        } else {
                            apply_spline_follow_xy(s, hit->x, hit->y);
                            set_spline_chunk_offset(s.id, s.position.x - grab_mesh_ox,
                                                    s.position.y - grab_mesh_oy);
                            set_path_rails_offset(s.id, false, s.position.x - grab_mesh_ox,
                                                 s.position.y - grab_mesh_oy);
                            remesh_spline(s, true);
                        }
                        // Re-buscar por id: s puede estar invalidado tras fill temps
                        if (auto* sg = find_spline_by_id(grab_sid))
                            set_spline_chunk_offset(sg->id, 0.f, 0.f, 0.f);
                        else
                            log::warn("[nodo] grab end: spline id=" + std::to_string(grab_sid) +
                                      " desapareció (¿realloc/erase?)");
                    }
                    } // normal grab XY
                } else if (edit_mode == EditMode::NewGrab && !new_grab_lock_xy) {
                    float tx = hit->x;
                    float ty = hit->y;
                    bool snapped = false;
                    const bool ctrl_spawn = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (ctrl_spawn) {
                        const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                        float wvp_spawn[16]{};
                        camera.build_world_view_proj(aspect, wvp_spawn);
                        const auto pp = pick_path_endpoint_near_mouse(mx, my, fw, fh, wvp_spawn);
                        if (pp.valid) {
                            int spawn_path_idx = 0;
                            if (const auto def = spline::load_sli(new_spline_draft.sli_path))
                                spawn_path_idx =
                                    (std::min)(path_align_path_index,
                                               static_cast<int>(def->paths.size()) - 1);
                            if (snap_spline_path_to_pick(new_spline_draft, spawn_path_idx, pp)) {
                                snapped = true;
                            }
                        } else {
                            const int mk = pick_spline_end_marker(mx, my, fw, fh, wvp_spawn);
                            if (mk >= 0) {
                                float tz = 0.f, th = 0.f;
                                const auto& marker = end_markers[static_cast<std::size_t>(mk)];
                                if (marker_pose(marker, tx, ty, tz, th)) {
                                    new_spline_draft.position.z = tz;
                                    float h = th + (marker.is_start ? 180.f : 0.f);
                                    while (h > 180.f) h -= 360.f;
                                    while (h < -180.f) h += 360.f;
                                    new_spline_draft.heading_deg = h;
                                    snapped = true;
                                }
                            }
                        }
                    }
                    if (snapped) {
                        new_spline_draft.position.x = tx;
                        new_spline_draft.position.y = ty;
                        remesh_spline(new_spline_draft, true);
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
                    } else {
                        apply_spline_follow_xy(new_spline_draft, tx, ty);
                    }
                    set_spline_chunk_offset(new_spline_draft.id, new_spline_draft.position.x - grab_mesh_ox,
                                           new_spline_draft.position.y - grab_mesh_oy,
                                           new_spline_draft.position.z - grab_mesh_oz);
                } else if ((edit_mode == EditMode::GrabLength || edit_mode == EditMode::TrimSpline) &&
                           selected_spline >= 0 &&
                           static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    // Robust along: closest point on the curve to the mouse hit.
                    // Analytic atan2 fails / sticks with |R| >> length (near-straight arcs).
                    const float L = (std::max)(s.length, 0.5f);
                    float along = 0.f;
                    {
                        const float dx0 = hit->x - s.position.x;
                        const float dy0 = hit->y - s.position.y;
                        const float rot = s.heading_deg * (3.14159265f / 180.f);
                        // Fast straight projection as seed
                        along = dx0 * std::sin(rot) + dy0 * std::cos(rot);
                        if (std::abs(s.radius) > 1.e-3f && std::abs(s.radius) < 5000.f) {
                            const float lx = dx0 * std::cos(rot) - dy0 * std::sin(rot);
                            const float ly = dx0 * std::sin(rot) + dy0 * std::cos(rot);
                            const float R = s.radius;
                            const float den = R - lx;
                            if (std::abs(den) > 1.e-4f) {
                                const float ang = std::atan2(ly, den);
                                along = std::abs(R) * std::abs(ang);
                            }
                        }
                        // Always refine by dense closest-point (handles huge R and any curvature)
                        const int nprobe = (std::max)(16, static_cast<int>(std::ceil(L / 0.5f)));
                        float best_d2 = 1.e30f;
                        float best_a = along;
                        for (int i = 0; i <= nprobe; ++i) {
                            const float a = L * static_cast<float>(i) / static_cast<float>(nprobe);
                            const auto p = spline::evaluate_spline(s, a);
                            const float ddx = p.x - hit->x;
                            const float ddy = p.y - hit->y;
                            const float d2 = ddx * ddx + ddy * ddy;
                            if (d2 < best_d2) {
                                best_d2 = d2;
                                best_a = a;
                            }
                        }
                        // Local refine ±1 step
                        const float step = L / static_cast<float>(nprobe);
                        for (float a = best_a - step; a <= best_a + step + 1.e-4f; a += step * 0.25f) {
                            if (a < 0.f || a > L) continue;
                            const auto p = spline::evaluate_spline(s, a);
                            const float ddx = p.x - hit->x;
                            const float ddy = p.y - hit->y;
                            const float d2 = ddx * ddx + ddy * ddy;
                            if (d2 < best_d2) {
                                best_d2 = d2;
                                best_a = a;
                            }
                        }
                        along = best_a;
                    }
                    along = (std::max)(0.5f, along);
                    if (edit_mode == EditMode::TrimSpline) {
                        along = (std::min)(along, grab_start_len);
                        update_trim_preview(s, along);
                    } else {
                        s.length = along;
                        remesh_spline(s, true);
                    }
                } else if (edit_mode == EditMode::Radius && selected_spline >= 0 &&
                           static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    const float rot = s.heading_deg * (3.14159265f / 180.f);
                    const float dx = hit->x - s.position.x;
                    const float dy = hit->y - s.position.y;
                    const float local_x = dx * std::cos(rot) - dy * std::sin(rot);
                    const float local_y = dx * std::sin(rot) + dy * std::cos(rot);
                    // Circle through start tangent + mouse; length fixed.
                    if (std::abs(local_x) > 0.05f) {
                        s.radius = (local_x * local_x + local_y * local_y) / (2.f * local_x);
                        if (std::abs(s.radius) < 0.05f) s.radius = 0.f;
                    } else {
                        s.radius = 0.f;
                    }
                    s.length = grab_start_len;
                    remesh_spline(s, true);
                } else if (edit_mode == EditMode::Rotate) {
                    if (sco_path_edit.rotating && sco_path_edit.sel >= 0) {
                        apply_sco_path_hit(hit->x, hit->y, hit->z);
                    } else {
                    const bool snap90 = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    auto snap_yaw = [&](float free_deg) {
                        if (!snap90) return free_deg;
                        // 90 deg steps relative to rotation at R-press (not tile axes).
                        float rel = free_deg - grab_start_yaw;
                        while (rel > 180.f) rel -= 360.f;
                        while (rel < -180.f) rel += 360.f;
                        const float step = std::round(rel / 90.f) * 90.f;
                        return grab_start_yaw + step;
                    };
                    auto snap_delta = [&](float free_delta) {
                        if (!snap90) return free_delta;
                        float rel = free_delta;
                        while (rel > 180.f) rel -= 360.f;
                        while (rel < -180.f) rel += 360.f;
                        return std::round(rel / 90.f) * 90.f;
                    };
                    if (group_rotate_active && !group_grab_index.empty()) {
                        const float dx = hit->x - group_pivot_x;
                        const float dy = hit->y - group_pivot_y;
                        if (dx * dx + dy * dy > 1.e-4f) {
                            // OMSI heading: 0 = +Y, atan2(x,y). Rotate offsets around group pivot.
                            const float ang = std::atan2(dx, dy) * (180.f / 3.14159265f);
                            if (!group_rot_have_ref) {
                                group_rot_ref_deg = ang;
                                group_rot_have_ref = true;
                            } else {
                                float delta = snap_delta(ang - group_rot_ref_deg);
                                const float rad = delta * (3.14159265f / 180.f);
                                const float c = std::cos(rad);
                                const float s = std::sin(rad);
                                for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                                    // Orbit around selection center (not each object's origin).
                                    const float ox = group_off_x[i];
                                    const float oy = group_off_y[i];
                                    // OMSI yaw uses atan2(x,y): rotate offsets with same measure.
                                    const float rx = ox * c + oy * s;
                                    const float ry = -ox * s + oy * c;
                                    if (group_grab_kind[i] == 0) {
                                        auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                                        o.position.x = group_pivot_x + rx;
                                        o.position.y = group_pivot_y + ry;
                                        if (i < group_start_yaw.size())
                                            o.rotation_deg.x = group_start_yaw[i] + delta;
                                        sync_object_gpu(o);
                                    } else {
                                        auto& sp = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                                        sp.position.x = group_pivot_x + rx;
                                        sp.position.y = group_pivot_y + ry;
                                        if (i < group_start_yaw.size())
                                            sp.heading_deg = group_start_yaw[i] + delta;
                                        set_spline_chunk_offset(sp.id, 0.f, 0.f, 0.f);
                                    }
                                }
                                std::unordered_set<std::int32_t> moved;
                                for (std::size_t i = 0; i < group_grab_index.size(); ++i)
                                    if (group_grab_kind[i] != 0)
                                        moved.insert(
                                            edit_splines[static_cast<std::size_t>(group_grab_index[i])].id);
                                after_rotate_spline_ids(moved, true, snap90);
                            }
                        }
                    } else if (!group_rotate_active && selected_object >= 0 &&
                        static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                        auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                        const float dx = hit->x - o.position.x;
                        const float dy = hit->y - o.position.y;
                        if (dx * dx + dy * dy > 1.e-4f) {
                            o.rotation_deg.x = snap_yaw(std::atan2(dx, dy) * (180.f / 3.14159265f));
                            sync_object_gpu(o);
                        }
                    } else if (selected_spline >= 0 &&
                               static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                        auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                        const float dx = hit->x - s.position.x;
                        const float dy = hit->y - s.position.y;
                        if (dx * dx + dy * dy > 1.e-4f) {
                            s.heading_deg = snap_yaw(std::atan2(dx, dy) * (180.f / 3.14159265f));
                            after_rotate_spline_ids({s.id}, true, snap90);
                        }
                    }
                    adjust_hud.set_current(current_ref_yaw());
                    if (panel.imgui_primary() && adjust_hud_imgui.active && !adjust_hud_imgui.user_editing)
                        adjust_hud_imgui.current = current_ref_yaw();
                    }
                } else if (edit_mode == EditMode::SplineFollow && sco_path_edit.following &&
                           sco_path_edit.sel >= 0) {
                    apply_sco_path_hit(hit->x, hit->y, hit->z);
                } else if (edit_mode == EditMode::SplineFollow && selected_spline >= 0 &&
                           static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                    float tx = hit->x;
                    float ty = hit->y;
                    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (ctrl) {
                        const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                        float wvp_snap[16]{};
                        camera.build_world_view_proj(aspect, wvp_snap);
                        const auto pp = pick_path_endpoint_near_mouse(mx, my, fw, fh, wvp_snap);
                        if (pp.valid) {
                            float cx = 0.f, cy = 0.f, cz = 0.f, ch = 0.f;
                            if (centerline_at_path_pick(pp, cx, cy, cz, ch)) {
                                tx = cx;
                                ty = cy;
                            }
                        } else {
                            const int mk = pick_spline_end_marker(mx, my, fw, fh, wvp_snap);
                            if (mk >= 0) {
                                float th = 0.f, tz = 0.f;
                                marker_pose(end_markers[static_cast<std::size_t>(mk)], tx, ty, tz, th);
                            }
                        }
                    }
                    apply_spline_follow_to_point(s, tx, ty);
                    remesh_spline(s, true);
                    if (group_sel_spl.size() > 1) {
                        bool same_rot = true;
                        const float href = s.heading_deg;
                        float cx = 0.f, cy = 0.f;
                        int cn = 0;
                        for (int gi : group_sel_spl) {
                            if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                            const auto& o = edit_splines[static_cast<std::size_t>(gi)];
                            float dh = o.heading_deg - href;
                            while (dh > 180.f) dh -= 360.f;
                            while (dh < -180.f) dh += 360.f;
                            if (std::fabs(dh) > 3.f) same_rot = false;
                            cx += o.position.x;
                            cy += o.position.y;
                            ++cn;
                        }
                        if (cn > 0) {
                            cx /= static_cast<float>(cn);
                            cy /= static_cast<float>(cn);
                        }
                        const float hrad = href * (3.14159265f / 180.f);
                        const float rx = std::cos(hrad);
                        const float ry = -std::sin(hrad);
                        if (same_rot && cn > 1) {
                            float L0 = 0.f, R0 = 0.f;
                            spline::fit_spline_to_xy(cx, cy, href, tx, ty, L0, R0);
                            for (int gi : group_sel_spl) {
                                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                                auto& o = edit_splines[static_cast<std::size_t>(gi)];
                                const float lat = (o.position.x - cx) * rx + (o.position.y - cy) * ry;
                                float Ri = R0;
                                float Li = L0;
                                if (std::fabs(R0) > 0.25f) {
                                    Ri = R0 - lat;
                                    if (R0 > 0.f && Ri < 0.35f) Ri = 0.35f;
                                    if (R0 < 0.f && Ri > -0.35f) Ri = -0.35f;
                                    Li = L0 * (std::fabs(Ri) / std::fabs(R0));
                                    if (Li < 0.05f) Li = 0.05f;
                                }
                                o.length = Li;
                                o.radius = Ri;
                                remesh_spline(o, true);
                            }
                        } else {
                            for (int gi : group_sel_spl) {
                                if (gi == selected_spline) continue;
                                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                                auto& o = edit_splines[static_cast<std::size_t>(gi)];
                                o.length = s.length;
                                o.radius = s.radius;
                                remesh_spline(o, true);
                            }
                        }
                    }
                    update_spline_panel_live(static_cast<std::size_t>(selected_spline));
                } else if (edit_mode == EditMode::NewFollow) {
                    float new_len = new_spline_draft.length;
                    float new_r = new_spline_draft.radius;
                    spline::fit_spline_to_xy(new_spline_draft.position.x, new_spline_draft.position.y,
                                             new_spline_draft.heading_deg, hit->x, hit->y, new_len, new_r);
                    new_spline_draft.length = new_len;
                    new_spline_draft.radius = new_r;
                    remesh_spline(new_spline_draft, true);
                    ui::EditorSplineProps p{};
                    p.valid = true;
                    p.id = new_spline_draft.id;
                    p.sli_name = new_spline_draft.sli_path.filename().string();
                    p.z = new_spline_draft.position.z;
                    p.rot = new_spline_draft.heading_deg;
                    p.skew_start = new_spline_draft.skew_start;
                    p.skew_end = new_spline_draft.skew_end;
                    p.length = new_spline_draft.length;
                    p.radius = new_spline_draft.radius;
                    p.start_grad = new_spline_draft.start_grad;
                    p.end_grad = new_spline_draft.end_grad;
                    p.mirror = new_spline_draft.mirror;
                    p.is_spline_h = new_spline_draft.is_spline_h;
                    panel.show_spline_fields(p);
                } else if (edit_mode == EditMode::NewScoFollow) {
                    new_sco_draft.position.x = hit->x;
                    new_sco_draft.position.y = hit->y;
                    if (selected_object >= 0 &&
                        static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                        auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                        if (spline_attach_obj_to_entry.count(o.id))
                            apply_spline_attach_follow_xy(o, hit->x, hit->y);
                        else
                            apply_sco_follow_xy(o, hit->x, hit->y);
                        new_sco_draft = o;
                    }
                    ui::EditorObjectProps p{};
                    p.valid = true;
                    p.id = new_sco_draft.id;
                    p.sco_name = new_sco_draft.sco_path.filename().string();
                    p.x = new_sco_draft.position.x;
                    p.y = new_sco_draft.position.y;
                    p.z = new_sco_draft.position.z;
                    p.rot = new_sco_draft.rotation_deg.x;
                    p.varparent_id = new_sco_draft.varparent_id;
                    panel.update_object_live_values(p);
                }
            }
        }

        // LMB short-click: place (grab/rotate) or pick (MMB orbits; coords → framebuffer for HiDPI).
        {
            const bool lmb = !over_ui && !bus_drive_active && !spray_owns_lmb && !bng_ter_owns_lmb &&
                             !(camera.fly_mode && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0) &&
                             glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
            input::cursor_to_framebuffer(window, mx, my, fw, fh);
            if (lmb && !lmb_was) {
                lmb_down_x = mx;
                lmb_down_y = my;
                paint_drag_last_rail = -1;
                {
                    const bool ctrl_log = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    editor_log::event("CLICK_DOWN",
                                      "mx=" + std::to_string(static_cast<int>(mx)) + " my=" +
                                          std::to_string(static_cast<int>(my)) + " tool=" +
                                          std::to_string(static_cast<int>(ev.tool)) + " ctrl=" +
                                          (ctrl_log ? "1" : "0") + " over_ui=" + (over_ui ? "1" : "0") + " | " +
                                          session_state());
                }
                if (marquee.phase == MarqueePhase::Select) {
                    marquee.dragging = true;
                    marquee.sx0 = mx;
                    marquee.sy0 = my;
                    marquee.sx1 = mx;
                    marquee.sy1 = my;
                    if (sel_shape == SelShapeMode::Lasso || sel_shape == SelShapeMode::Circle) {
                        sel_lasso_pts.clear();
                        sel_lasso_pts.push_back({mx, my});
                        sel_lasso_drawing = (sel_shape == SelShapeMode::Lasso);
                        sel_circle_brush_upto = 0;
                    }
                    update_marquee_box(fw, fh, nullptr);
                    editor_log::event(
                        "SHIFT_S_DRAG_START",
                        "mx=" + std::to_string(static_cast<int>(mx)) + " my=" +
                            std::to_string(static_cast<int>(my)) + " intent=" +
                            (marquee.intent == MarqueeIntent::SelectOnly ? "select" : "clone") +
                            " | " + session_state());
                }
            }
            // CPU-based path rail picker: works even when 3D rails are invisible (modern mode),
            // by hit-testing the same centerlines the overlay draws.
            auto pick_path_rail_cpu = [&](float mx, float my, float fw, float fh) -> int {
                const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                float wvp[16]{};
                camera.build_world_view_proj(aspect, wvp);
                float best = 16.f * 16.f;
                int best_i = -1;
                for (std::size_t ri = 0; ri < path_rails.size(); ++ri) {
                    const auto& rail = path_rails[ri];
                    if (!rail.alive) continue;
                    if (sco_path_edit.active && !sco_path_edit.aa_mode) {
                        if (!rail.from_object || rail.owner_id != sco_path_edit.object_id) continue;
                    }
                    // Build centerline screen points.
                    if (rail.from_object) {
                        const map::MapObjectInst* obj = nullptr;
                        for (const auto& o : edit_objects)
                            if (o.id == rail.owner_id) { obj = &o; break; }
                        if (!obj) continue;
                        const auto* sco = sco::load_sco(obj->sco_path);
                        const std::vector<sco::ScoPath>* paths = nullptr;
                        if (sco_path_edit.active && sco_path_edit.object_id == obj->id) paths = &sco_path_edit.paths;
                        else if (sco) paths = &sco->paths;
                        if (!paths || rail.path_index < 0 ||
                            static_cast<std::size_t>(rail.path_index) >= paths->size())
                            continue;
                        const auto& p = (*paths)[static_cast<std::size_t>(rail.path_index)];
                        const float th = sco ? (sco::skip_terrain_add(*sco) ? 0.f : sample_terrain_editor_xy(obj->position.x, obj->position.y)) : 0.f;
                        const XMMATRIX world_m = sco ? sco_world_matrix(*obj, nullptr, th, 0.f, 0.f) : XMMatrixIdentity();
                        auto path_len = [](const sco::ScoPath& pp) {
                            float plen = std::abs(pp.length);
                            if (std::abs(pp.radius) > 0.001f && plen > 0.001f && plen <= 360.f && plen / std::abs(pp.radius) > 3.2f)
                                plen = std::abs(pp.radius) * plen * (3.14159265f / 180.f);
                            return (std::max)(0.05f, plen);
                        };
                        const float len = path_len(p);
                        const int steps = 16;
                        float prev_sx = 0.f, prev_sy = 0.f;
                        bool have = false;
                        for (int k = 0; k <= steps; ++k) {
                            map::SplineInst mini = sco::sco_path_to_spline_inst(p);
                            mini.length = len;
                            const auto lp = spline::evaluate_spline(mini, (std::clamp)(len * static_cast<float>(k) / static_cast<float>(steps), 0.f, len), 0.f, 0.f);
                            DirectX::XMFLOAT3 w{};
                            DirectX::XMStoreFloat3(&w, xform_point(world_m, lp.x, lp.y, lp.z));
                            float sx = 0.f, sy = 0.f;
                            if (!world_to_screen(w.x, w.y, w.z + 0.06f, 0.f, 0.f, fw, fh, wvp, sx, sy)) { have = false; continue; }
                            if (have) {
                                // seg distance
                                const float vx = sx - prev_sx, vy = sy - prev_sy;
                                const float wx = mx - prev_sx, wy = my - prev_sy;
                                const float L = vx * vx + vy * vy;
                                float t = L > 1.e-6f ? (wx * vx + wy * vy) / L : 0.f;
                                t = (std::clamp)(t, 0.f, 1.f);
                                const float dx = wx - vx * t, dy = wy - vy * t;
                                const float d2 = dx * dx + dy * dy;
                                if (d2 < best) { best = d2; best_i = static_cast<int>(ri); }
                            }
                            prev_sx = sx; prev_sy = sy; have = true;
                        }
                    } else {
                        const map::SplineInst* sp = nullptr;
                        for (const auto& s : edit_splines)
                            if (s.id == rail.owner_id) { sp = &s; break; }
                        if (!sp) continue;
                        const auto def = spline::load_sli(sp->sli_path);
                        if (!def || rail.path_index < 0 ||
                            static_cast<std::size_t>(rail.path_index) >= def->paths.size())
                            continue;
                        const auto& pp = def->paths[static_cast<std::size_t>(rail.path_index)];
                        const float half = (std::max)(0.15f, static_cast<float>(pp.width) * 0.5f);
                        const int stt = 16;
                        float prev_sx = 0.f, prev_sy = 0.f;
                        bool have = false;
                        for (int k = 0; k <= stt; ++k) {
                            const float along = sp->length * static_cast<float>(k) / static_cast<float>(stt);
                            const auto c = spline::evaluate_spline(*sp, along, pp.lateral, pp.height);
                            float sx = 0.f, sy = 0.f;
                            if (!world_to_screen(c.x, c.y, c.z + 0.06f, 0.f, 0.f, fw, fh, wvp, sx, sy)) { have = false; continue; }
                            if (have) {
                                const float vx = sx - prev_sx, vy = sy - prev_sy;
                                const float wx = mx - prev_sx, wy = my - prev_sy;
                                const float L = vx * vx + vy * vy;
                                float t = L > 1.e-6f ? (wx * vx + wy * vy) / L : 0.f;
                                t = (std::clamp)(t, 0.f, 1.f);
                                const float dx = wx - vx * t, dy = wy - vy * t;
                                const float d2 = dx * dx + dy * dy;
                                if (d2 < best) { best = d2; best_i = static_cast<int>(ri); }
                            }
                            prev_sx = sx; prev_sy = sy; have = true;
                        }
                    }
                }
                if (best_i >= 0 && best <= 16.f * 16.f) return best_i;
                return -1;
            };

            // Click-edge + drag paint (OMSI): apply rule on click and swipe across paths.
            static bool lmb_was_paint = false;
            const bool lmb_edge = lmb && !lmb_was_paint;
            if (lmb && ev.tool == ui::EditorTool::TrafficRules && show_path_rails &&
                edit_mode == EditMode::None) {
                const int rail = pick_path_rail_cpu(mx, my, fw, fh);
                if (rail >= 0 && (lmb_edge || rail != paint_drag_last_rail)) {
                    // One undo step per click / swipe (Ctrl+Z in Traffic Rules reverts the last paint).
                    if (lmb_edge && !(GetAsyncKeyState(VK_MENU) & 0x8000)) push_undo("traffic_paint");
                    if (paint_path_rail_fast(rail)) paint_drag_last_rail = rail;
                    else panel.set_status("Paint failed: rail=" + std::to_string(rail));
                } else if (rail < 0) {
                    panel.set_status("No path under cursor");
                }
            }
            if (!lmb) { paint_drag_last_rail = -1; lmb_was_paint = false; }
            else lmb_was_paint = true;
            // Terrain vertex edit: click/drag a vertex handle to raise/lower it precisely.
            if (ev.tool == ui::EditorTool::Terrain && edit_mode == EditMode::None && !view_only &&
                panel.imgui_state().terrain_vertex_edit && !panel.imgui_state().terrain_lasso &&
                panel.imgui_state().terrain_active_sub == 0) {
                static bool vwas = false;
                terrain_vertex.hover_field = -1;
                if (const auto vhit = camera.hit_ground_xy(mx, my, fw, fh)) {
                    for (std::size_t fi = 0; fi < terrain_fields.size(); ++fi) {
                        auto& tf = terrain_fields[fi];
                        const float lx = vhit->x - tf.rel_x, ly = vhit->y - tf.rel_y;
                        if (lx < 0.f || ly < 0.f || lx > tf.size_m || ly > tf.size_m) continue;
                        const float step = tf.size_m / 60.f;
                        const int ix = (std::clamp)((int)std::lround(lx / step), 0, 60);
                        const int iy = (std::clamp)((int)std::lround(ly / step), 0, 60);
                        terrain_vertex.hover_field = (int)fi;
                        terrain_vertex.hover_ix = ix;
                        terrain_vertex.hover_iy = iy;
                        if (lmb && !vwas) {
                            auto& heights = !tf.heights_base.empty() ? tf.heights_base : tf.heights;
                            if (heights.size() == static_cast<std::size_t>(61 * 61)) {
                                terrain_vertex.field = (int)fi;
                                terrain_vertex.ix = ix;
                                terrain_vertex.iy = iy;
                                terrain_vertex.start_h = heights[static_cast<std::size_t>(iy) * 61 + ix];
                                terrain_vertex.start_my = my;
                                terrain_vertex.dragging = true;
                            }
                        }
                        break;
                    }
                }
                if (lmb && terrain_vertex.dragging && terrain_vertex.field >= 0 &&
                    terrain_vertex.field < static_cast<int>(terrain_fields.size())) {
                    auto& sf = terrain_fields[static_cast<std::size_t>(terrain_vertex.field)];
                    auto& heights = !sf.heights_base.empty() ? sf.heights_base : sf.heights;
                    if (heights.size() == static_cast<std::size_t>(61 * 61)) {
                        const float dz = (terrain_vertex.start_my - my) * 0.02f;
                        heights[static_cast<std::size_t>(terrain_vertex.iy) * 61 + terrain_vertex.ix] =
                            terrain_vertex.start_h + dz;
                        restamp_terrain_field(sf);
                        refresh_terrain_gpu_from_memory(sf);
                    }
                } else if (!lmb && vwas && terrain_vertex.dragging) {
                    terrain_vertex.dragging = false;
                    if (terrain_vertex.field >= 0 &&
                        terrain_vertex.field < static_cast<int>(terrain_fields.size())) {
                        auto& sf = terrain_fields[static_cast<std::size_t>(terrain_vertex.field)];
                        terrain_dirty_tiles.insert(tile_key(sf.grid_x, sf.grid_y));
                        mark_tile_dirty(sf.grid_x, sf.grid_y);
                    }
                }
                // Draw EVERY vertex of the current tile (wireframe + nodes) so heights can be adjusted comfortably.
                {
                    int vf = terrain_vertex.dragging ? terrain_vertex.field : terrain_vertex.hover_field;
                    if (vf < 0 || vf >= static_cast<int>(terrain_fields.size())) {
                        vf = -1;
                        for (std::size_t fi = 0; fi < terrain_fields.size(); ++fi) {
                            const auto& tf = terrain_fields[fi];
                            const float lx = camera.target_x - tf.rel_x, ly = camera.target_y - tf.rel_y;
                            if (lx >= 0.f && ly >= 0.f && lx <= tf.size_m && ly <= tf.size_m) { vf = static_cast<int>(fi); break; }
                        }
                    }
                    if (vf >= 0) {
                        const auto& tf = terrain_fields[static_cast<std::size_t>(vf)];
                        const auto& hs = tf.heights.size() == static_cast<std::size_t>(61 * 61) ? tf.heights : tf.heights_base;
                        if (hs.size() == static_cast<std::size_t>(61 * 61)) {
                            const float asp = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                            float wvp_v[16]{};
                            camera.build_world_view_proj(asp, wvp_v);
                            const ImVec2 disp = ImGui::GetIO().DisplaySize;
                            const float kx = disp.x / (std::max)(1.f, fw), ky = disp.y / (std::max)(1.f, fh);
                            ImDrawList* dl = ImGui::GetBackgroundDrawList();
                            const float step = tf.size_m / 60.f;
                            static std::vector<ImVec2> scr;
                            static std::vector<char> ok;
                            scr.assign(61 * 61, ImVec2(0.f, 0.f));
                            ok.assign(61 * 61, 0);
                            for (int iy = 0; iy < 61; ++iy)
                                for (int ix = 0; ix < 61; ++ix) {
                                    float sx = 0.f, sy = 0.f;
                                    const std::size_t i = static_cast<std::size_t>(iy) * 61 + ix;
                                    if (world_to_screen(tf.rel_x + ix * step, tf.rel_y + iy * step, hs[i] + 0.03f, 0.f, 0.f,
                                                        fw, fh, wvp_v, sx, sy)) {
                                        scr[i] = ImVec2(sx * kx, sy * ky);
                                        ok[i] = (sx > -50.f && sx < fw + 50.f && sy > -50.f && sy < fh + 50.f) ? 1 : 0;
                                    }
                                }
                            const ImU32 line_col = IM_COL32(120, 220, 255, 90);
                            for (int iy = 0; iy < 61; ++iy)
                                for (int ix = 0; ix < 61; ++ix) {
                                    const std::size_t i = static_cast<std::size_t>(iy) * 61 + ix;
                                    if (!ok[i]) continue;
                                    if (ix + 1 < 61 && ok[i + 1]) dl->AddLine(scr[i], scr[i + 1], line_col, 1.f);
                                    if (iy + 1 < 61 && ok[i + 61]) dl->AddLine(scr[i], scr[i + 61], line_col, 1.f);
                                }
                            for (std::size_t i = 0; i < scr.size(); ++i)
                                if (ok[i]) dl->AddCircleFilled(scr[i], 2.6f, IM_COL32(255, 255, 255, 210), 6);
                            auto mark = [&](int ix, int iy, ImU32 col, float r) {
                                if (ix < 0 || iy < 0 || ix > 60 || iy > 60) return;
                                const std::size_t i = static_cast<std::size_t>(iy) * 61 + ix;
                                if (ok[i]) dl->AddCircleFilled(scr[i], r, col, 12);
                            };
                            if (terrain_vertex.hover_field == vf) mark(terrain_vertex.hover_ix, terrain_vertex.hover_iy, IM_COL32(255, 235, 60, 255), 6.f);
                            if (terrain_vertex.dragging && terrain_vertex.field == vf) mark(terrain_vertex.ix, terrain_vertex.iy, IM_COL32(255, 70, 50, 255), 7.f);
                        }
                    }
                }
                vwas = lmb;
            }
            // Terrain lasso paint/erase of the selected ground layer (freehand polygon).
            if (ev.tool == ui::EditorTool::Terrain && edit_mode == EditMode::None && !view_only &&
                panel.imgui_state().terrain_lasso && panel.imgui_state().terrain_active_sub == 2) {
                static bool lasso_was = false;
                const auto hit = camera.hit_ground_xy(mx, my, fw, fh);
                if (lmb && !lasso_was) {
                    terrain_lasso_pts.clear();
                    terrain_lasso_drawing = true;
                    if (hit) terrain_lasso_pts.emplace_back(hit->x, hit->y);
                } else if (lmb && terrain_lasso_drawing) {
                    if (hit && (terrain_lasso_pts.empty() ||
                                std::hypot(hit->x - terrain_lasso_pts.back().first,
                                           hit->y - terrain_lasso_pts.back().second) > 1.0f))
                        terrain_lasso_pts.emplace_back(hit->x, hit->y);
                } else if (!lmb && lasso_was && terrain_lasso_drawing) {
                    terrain_lasso_drawing = false;
                    if (terrain_lasso_pts.size() >= 3) {
                        ui::TerrainBrushUi brush{};
                        panel.terrain_paint_brush(brush);
                        const int layer = brush.ground_layer;
                        if (layer < 1) {
                            panel.set_status("Lazo: elegi capa >=1 (capa 0 = pasto base)");
                        } else {
                            const bool rubber = panel.imgui_state().terrain_lasso_action == 1;
                            int tiles_done = 0;
                            for (auto& tf : terrain_fields) {
                                std::vector<std::pair<float, float>> local;
                                local.reserve(terrain_lasso_pts.size());
                                bool any = false;
                                for (const auto& pt : terrain_lasso_pts) {
                                    const float lx = pt.first - tf.rel_x;
                                    const float ly = pt.second - tf.rel_y;
                                    local.emplace_back(lx, ly);
                                    if (lx >= 0.f && ly >= 0.f && lx <= tf.size_m && ly <= tf.size_m) any = true;
                                }
                                if (!any) continue;
                                auto map_path = find_tile_path(tf.grid_x, tf.grid_y);
                                if (cfg.workspace)
                                    map_path = map::workspace_tile_map_for_read(&*cfg.workspace, map_path);
                                if (map_path.empty()) continue;
                                if (!editor::paint_omsi_ground_layer_mask_polygon(map_path, layer, tf.size_m, local,
                                                                                  rubber))
                                    continue;
                                const auto paint = editor::bake_ground_layer_paint_bmp(
                                    map_path, cfg.global.ground_textures, cfg.omsi_root, tf.size_m, 512);
                                if (!paint.empty()) renderer.invalidate_texture_path(paint);
                                reload_terrain_gpu_tile(tf.grid_x, tf.grid_y);
                                ++tiles_done;
                            }
                            panel.set_status(std::string(rubber ? "Lazo: capa borrada" : "Lazo: capa pintada") +
                                             (tiles_done ? "" : " (sin tile)"));
                        }
                    }
                    terrain_lasso_pts.clear();
                }
                lasso_was = lmb;
            }
            // Brush cursor: ring draped on the terrain with the brush radius.
            if (!over_ui && ev.tool == ui::EditorTool::Terrain && edit_mode == EditMode::None && !view_only &&
                !panel.imgui_state().terrain_lasso && !panel.imgui_state().terrain_vertex_edit &&
                panel.imgui_state().terrain_active_sub == ((panel.imgui_state().terrain_brush.mode == ui::TerrainBrushModeUi::PaintLayer || panel.imgui_state().terrain_brush.mode == ui::TerrainBrushModeUi::RubberLayer) ? 2 : 0)) {
                ui::TerrainBrushUi ring_brush{};
                if (panel.terrain_paint_brush(ring_brush)) {
                    camera.ground_z = camera.target_z;
                    if (const auto rh = camera.hit_ground_xy(mx, my, fw, fh))
                        draw_ground_ring(rh->x, rh->y, ring_brush.brush_size_m, false, IM_COL32(255, 235, 120, 230),
                                         IM_COL32(255, 235, 120, 24));
                }
            }
            // Terrain brush (heights + OMSI ground-layer paint/rubber -> texture/map/*.map.N.dds).
            // Only the brush of the sub-section that is open acts: heights in Terreno, layer paint in Capas.
            if (lmb && ev.tool == ui::EditorTool::Terrain && edit_mode == EditMode::None && !view_only &&
                !panel.imgui_state().terrain_lasso && !panel.imgui_state().terrain_vertex_edit &&
                panel.imgui_state().terrain_active_sub == ((panel.imgui_state().terrain_brush.mode == ui::TerrainBrushModeUi::PaintLayer || panel.imgui_state().terrain_brush.mode == ui::TerrainBrushModeUi::RubberLayer) ? 2 : 0)) {
                ui::TerrainBrushUi brush{};
                if (panel.terrain_paint_brush(brush)) {
                    camera.ground_z = camera.target_z;
                    if (const auto hit = camera.hit_ground_xy(mx, my, fw, fh)) {
                        TerrainHeightField* field = nullptr;
                        for (auto& tf : terrain_fields) {
                            const float lx = hit->x - tf.rel_x;
                            const float ly = hit->y - tf.rel_y;
                            if (lx >= 0.f && ly >= 0.f && lx <= tf.size_m && ly <= tf.size_m) {
                                field = &tf;
                                break;
                            }
                        }
                        if (field && (!field->heights.empty() || !field->heights_base.empty())) {
                            const float lx = hit->x - field->rel_x;
                            const float ly = hit->y - field->rel_y;
                            auto map_path = find_tile_path(field->grid_x, field->grid_y);
                            if (cfg.workspace)
                                map_path = map::workspace_tile_map_for_read(&*cfg.workspace, map_path);
                            if (brush.mode == ui::TerrainBrushModeUi::PaintLayer ||
                                brush.mode == ui::TerrainBrushModeUi::RubberLayer) {
                                if (!map_path.empty()) {
                                    const int layer = brush.ground_layer;
                                    if (layer < 1) {
                                        panel.set_status(
                                            "Pintar capa: elegi capa >=1 (capa 0 = pasto base OMSI)");
                                    } else if (editor::paint_omsi_ground_layer_mask(
                                                   map_path, layer, field->size_m, lx, ly, brush.brush_size_m,
                                                   brush.mode == ui::TerrainBrushModeUi::RubberLayer)) {
                                        const auto paint = editor::bake_ground_layer_paint_bmp(
                                            map_path, cfg.global.ground_textures, cfg.omsi_root, field->size_m,
                                            512);
                                        if (!paint.empty()) renderer.invalidate_texture_path(paint);
                                        reload_terrain_gpu_tile(field->grid_x, field->grid_y);
                                        panel.set_status(
                                            std::string(brush.mode == ui::TerrainBrushModeUi::RubberLayer
                                                            ? "Rubber L"
                                                            : "Paint L") +
                                            std::to_string(layer) + " -> " +
                                            editor::ground_layer_mask_path(map_path, layer).filename().string());
                                    }
                                }
                            } else if (brush.mode == ui::TerrainBrushModeUi::FitMesh) {
                                // Raise/lower terrain toward lowest mesh (SCO/SLI) above each sample.
                                auto& heights =
                                    !field->heights_base.empty() ? field->heights_base : field->heights;
                                if (heights.size() < static_cast<std::size_t>(61 * 61)) {
                                    heights = editor::make_flat_terrain_heights(0.f);
                                }
                                const float size = field->size_m > 0.01f ? field->size_m : 300.f;
                                const float step = size / 60.f;
                                const float brush_r = std::fmax(step * 0.5f, brush.brush_size_m);
                                const float brush_r2 = brush_r * brush_r;
                                bool changed = false;
                                for (int yi = 0; yi < 61; ++yi) {
                                    for (int xi = 0; xi < 61; ++xi) {
                                        const float px = static_cast<float>(xi) * step;
                                        const float py = static_cast<float>(yi) * step;
                                        const float dx = px - lx;
                                        const float dy = py - ly;
                                        if (dx * dx + dy * dy > brush_r2) continue;
                                        const float wx = field->rel_x + px;
                                        const float wy = field->rel_y + py;
                                        float target = heights[static_cast<std::size_t>(yi) * 61 +
                                                               static_cast<std::size_t>(xi)];
                                        // Sample object bases near this point
                                        for (const auto& o : edit_objects) {
                                            const float odx = o.position.x - wx;
                                            const float ody = o.position.y - wy;
                                            if (odx * odx + ody * ody > 4.f) continue;
                                            target = (std::max)(target, o.position.z - 0.01f);
                                        }
                                        for (const auto& s : edit_splines) {
                                            const float sdx = s.position.x - wx;
                                            const float sdy = s.position.y - wy;
                                            // coarse reject then sample along length
                                            if (sdx * sdx + sdy * sdy >
                                                (s.length + brush_r + 5.f) * (s.length + brush_r + 5.f))
                                                continue;
                                            const float seglen = (std::max)(2.f, s.length / 8.f);
                                            for (float along = 0.f; along <= s.length + 0.01f; along += seglen) {
                                                const auto samp = spline::evaluate_spline(s, along);
                                                const float qx = samp.x - wx;
                                                const float qy = samp.y - wy;
                                                if (qx * qx + qy * qy > 2.25f) continue;
                                                target = (std::max)(target, samp.z - 0.01f);
                                            }
                                        }
                                        float& h = heights[static_cast<std::size_t>(yi) * 61 +
                                                          static_cast<std::size_t>(xi)];
                                        const float t = 1.f - std::sqrt(dx * dx + dy * dy) / brush_r;
                                        const float w = t * t;
                                        const float nh = h * (1.f - w) + target * w;
                                        if (std::fabs(nh - h) > 1.e-4f) {
                                            h = nh;
                                            changed = true;
                                        }
                                    }
                                }
                                if (changed) {
                                    field->heights_base = heights;
                                    restamp_terrain_field(*field);
                                    {
    terrain_dirty_tiles.insert(tile_key(field->grid_x, field->grid_y));
    mark_tile_dirty(field->grid_x, field->grid_y);
    refresh_terrain_gpu_from_memory(*field);
}
                                }
                            } else {
                                editor::TerrainBrushMode mode = editor::TerrainBrushMode::Higher;
                                if (brush.mode == ui::TerrainBrushModeUi::Lower)
                                    mode = editor::TerrainBrushMode::Lower;
                                else if (brush.mode == ui::TerrainBrushModeUi::SetHeight)
                                    mode = editor::TerrainBrushMode::SetHeight;
                                if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                                    glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) {
                                    if (mode == editor::TerrainBrushMode::Higher)
                                        mode = editor::TerrainBrushMode::Lower;
                                    else if (mode == editor::TerrainBrushMode::Lower)
                                        mode = editor::TerrainBrushMode::Higher;
                                }
                                auto& heights =
                                    !field->heights_base.empty() ? field->heights_base : field->heights;
                                if (heights.size() < static_cast<std::size_t>(61 * 61)) {
                                    heights = editor::make_flat_terrain_heights(0.f);
                                }
                                // brush.speed is a real rate (m/s at the brush centre), scaled by the frame time so
                                // it does not depend on FPS. Monotonic: double the speed = double the change per second.
                                const float dt_s = (std::clamp)(ImGui::GetIO().DeltaTime, 0.002f, 0.1f);
                                const float amount = (std::max)(0.01f, brush.speed) * dt_s;
                                if (editor::paint_terrain_brush(heights, field->size_m, lx, ly,
                                                                brush.brush_size_m, mode, amount,
                                                                brush.set_height)) {
                                    field->heights_base = heights;
                                    restamp_terrain_field(*field);
                                    {
    terrain_dirty_tiles.insert(tile_key(field->grid_x, field->grid_y));
    mark_tile_dirty(field->grid_x, field->grid_y);
    refresh_terrain_gpu_from_memory(*field);
}
                                }
                            }
                        }
                    }
                }
        if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: loop_03_end");
