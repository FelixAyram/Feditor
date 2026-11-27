            }
            if (!lmb) paint_drag_last_rail = -1;
            if (!lmb && lmb_was) {
                const float ddx = mx - static_cast<float>(lmb_down_x);
                const float ddy = my - static_cast<float>(lmb_down_y);
                editor_log::event("CLICK_UP",
                                  "dx=" + std::to_string(static_cast<int>(ddx)) + " dy=" +
                                      std::to_string(static_cast<int>(ddy)) + " | " + session_state());
                // Attach / parent pick: accept click even with small drag; keep mode until hit or Esc.
                const bool attach_pick = edit_mode == EditMode::AttachPickParent ||
                                         edit_mode == EditMode::AttachPickSpline;
                if (attach_pick) {
                    const float aspect_sc = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                    float wvp_sc[16]{};
                    camera.build_world_view_proj(aspect_sc, wvp_sc);
                    // Prefer release position (more natural after aiming).
                    const float px = mx;
                    const float py = my;
                    if (edit_mode == EditMode::AttachPickParent) {
                        const int best = pick_object_index(px, py, fw, fh, wvp_sc);
                        int child_i = selected_object;
                        if (picking_varparent && varparent_pick_child_id != 0) {
                            child_i = -1;
                            for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                                if (edit_objects[i].id == varparent_pick_child_id) {
                                    child_i = static_cast<int>(i);
                                    break;
                                }
                            }
                        }
                        if (best >= 0 && child_i >= 0 && best != child_i &&
                            static_cast<std::size_t>(child_i) < edit_objects.size()) {
                            auto& child = edit_objects[static_cast<std::size_t>(child_i)];
                            const auto& parent = edit_objects[static_cast<std::size_t>(best)];
                            if (picking_varparent) {
                                apply_varparent(child, parent.id);
                                persist_editor_xy(child.position.x, child.position.y);
                                selected_object = child_i;
                                show_object_at(static_cast<std::size_t>(child_i));
                                panel.set_status("varparent id=" + std::to_string(child.id) + " -> " +
                                                 std::to_string(parent.id));
                            } else {
                                edit_attachments.erase(
                                    std::remove_if(edit_attachments.begin(), edit_attachments.end(),
                                                   [&](const map::MapAttachObjectInst& a) {
                                                       return a.child_id == child.id;
                                                   }),
                                    edit_attachments.end());
                                map::MapAttachObjectInst a{};
                                a.sco_path = child.sco_path;
                                a.child_id = child.id;
                                a.parent_id = parent.id;
                                a.attach_point_index = panel.attach_point();
                                edit_attachments.push_back(a);
                                sync_object_gpu(child);
                                persist_editor_xy(child.position.x, child.position.y);
                                show_object_at(static_cast<std::size_t>(selected_object));
                                panel.set_status("Attached " + std::to_string(child.id) + " → " +
                                                 std::to_string(parent.id) + " [attachObj]");
                            }
                            picking_varparent = false;
                            varparent_pick_child_id = 0;
                            edit_mode = EditMode::None;
                        } else {
                            panel.set_status(picking_varparent
                                                 ? "Parent to: click otro objeto (Esc cancela)"
                                                 : "Attach obj: click el padre (Esc cancela)");
                        }
                    } else { // AttachPickSpline
                        const int spl = pick_spline_index(px, py, fw, fh, wvp_sc);
                        if (spl < 0) {
                            panel.set_status("Attach spline: click una spline visible (Esc cancela)");
                        } else {
                            push_undo("attach_to_spline");
                            if (!bind_selected_object_to_spline(spl)) {
                                pop_undo("attach_bind_failed");
                                panel.set_status(
                                    "Attach spline: no se pudo bindear (¿mismo tile / índice?) — reintentá o Esc");
                            } else {
                                refresh_editor_lists();
                                const auto& sref = edit_splines[static_cast<std::size_t>(spl)];
                                camera.ground_z = camera.target_z;
                                if (selected_object >= 0 &&
                                    static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                                    auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                                    if (const auto hit = camera.hit_ground_xy(px, py, fw, fh))
                                        apply_spline_attach_follow_xy(o, hit->x, hit->y);
                                    edit_mode = EditMode::Grab;
                                    grabbing = true;
                                    rotating = false;
                                    g_tap = 0;
                                    r_tap = 0;
                                    grab_suppress_g_cancel = true;
                                    attach_slide_armed = true;
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
                                    panel.set_status(
                                        "Attach spline id=" + std::to_string(sref.id) +
                                        " — mové sobre la .sli (along 0…" +
                                        std::to_string(static_cast<int>(sref.length + 0.5f)) +
                                        " m) · LMB=asentar Esc=cancel (G no cancela)");
                                } else {
                                    edit_mode = EditMode::None;
                                    grab_suppress_g_cancel = false;
                                    panel.set_status("Attached to spline id=" + std::to_string(sref.id) +
                                                     " [splineAttachement]");
                                }
                            }
                        }
                    }
                } else if (marquee.phase == MarqueePhase::Select && marquee.dragging) {
                    editor_log::event(
                        "SHIFT_S_DRAG_END",
                        "dx=" + std::to_string(static_cast<int>(ddx)) + " dy=" +
                            std::to_string(static_cast<int>(ddy)) + " | " + session_state());
                    if (stream_busy()) {
                        clear_marquee_box();
                        marquee.dragging = false;
                        marquee.phase = MarqueePhase::None;
                        panel.set_status("Marquee: cancelado (cambio de tile)");
                    } else if (sel_shape == SelShapeMode::Circle) {
                        // Circle already painted frame-a-frame; keep mode armed until Pick simple.
                        clear_marquee_box();
                        marquee.dragging = false;
                        sel_lasso_pts.clear();
                        sel_circle_brush_upto = 0;
                        marquee.phase = MarqueePhase::Select;
                        marquee.intent = MarqueeIntent::SelectOnly;
                        panel.set_status("Círculo: grupo " + std::to_string(group_sel_obj.size()) + " obj / " +
                                         std::to_string(group_sel_spl.size()) +
                                         " spl (seguí pintando o Pick simple)");
                    } else {
                        const float aspect_m = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                        float wvp_m[16]{};
                        camera.build_world_view_proj(aspect_m, wvp_m);
                        finish_marquee_select(fw, fh, wvp_m);
                    }
                } else if (marquee.phase == MarqueePhase::Placing && ddx * ddx + ddy * ddy < 36.f) {
                    const int place_total =
                        static_cast<int>(marquee.track_obj.size() + marquee.track_spl.size());
                    int place_done = 0;
                    auto place_tick = [&](const char* kind) {
                        ++place_done;
                        auto& bst = panel.imgui_state();
                        bst.busy_overlay = true;
                        bst.busy_spinner = true;
                        bst.busy_title = "Colocando";
                        bst.busy_detail = std::string(kind) + " " + std::to_string(place_done) + "/" +
                                          std::to_string((std::max)(1, place_total));
                        bst.busy_progress =
                            place_total > 0 ? static_cast<float>(place_done) / static_cast<float>(place_total)
                                            : 1.f;
                        if ((place_done % 2) == 0 || place_done == place_total) pump_editor_busy();
                    };
                    {
                        auto& bst = panel.imgui_state();
                        bst.busy_overlay = true;
                        bst.busy_spinner = true;
                        bst.busy_title = "Colocando";
                        bst.busy_detail = "escribiendo tiles";
                        bst.busy_progress = 0.f;
                        pump_editor_busy();
                    }
                    for (int idx : marquee.track_obj) {
                        if (idx >= 0 && static_cast<std::size_t>(idx) < edit_objects.size()) {
                            auto& o = edit_objects[static_cast<std::size_t>(idx)];
                            upsert_transient_object(o);
                            persist_editor_xy(o.position.x, o.position.y);
                            place_tick("objeto");
                        }
                    }
                    for (int idx : marquee.track_spl) {
                        if (idx >= 0 && static_cast<std::size_t>(idx) < edit_splines.size()) {
                            auto& s = edit_splines[static_cast<std::size_t>(idx)];
                            upsert_transient_spline(s);
                            if (auto* g = find_spline_gpu(s.id)) g->pinned = false;
                            persist_editor_xy(s.position.x, s.position.y);
                            place_tick("spline");
                        }
                    }
                    for (int idx : marquee.track_obj) {
                        if (idx >= 0 && static_cast<std::size_t>(idx) < edit_objects.size()) {
                            const auto oid = edit_objects[static_cast<std::size_t>(idx)].id;
                            for (auto& sp : scene_places) {
                                if (sp.object_id == oid) sp.pinned = false;
                            }
                        }
                    }
                    {
                        auto& bst = panel.imgui_state();
                        bst.busy_overlay = false;
                        bst.busy_spinner = false;
                        bst.busy_progress = -1.f;
                        bst.busy_title.clear();
                        bst.busy_detail.clear();
                    }
                    // Keep selection on the pasted/cloned items — only clear on empty-space click
                    // or when picking a different instance in the active tool section.
                    group_sel_obj = marquee.track_obj;
                    group_sel_spl = marquee.track_spl;
                    group_sel_obj_ids.clear();
                    group_sel_spl_ids.clear();
                    selected_object = -1;
                    selected_spline = -1;
                    for (int idx : group_sel_obj) {
                        if (idx >= 0 && static_cast<std::size_t>(idx) < edit_objects.size())
                            group_sel_obj_ids.push_back(edit_objects[static_cast<std::size_t>(idx)].id);
                    }
                    for (int idx : group_sel_spl) {
                        if (idx >= 0 && static_cast<std::size_t>(idx) < edit_splines.size())
                            group_sel_spl_ids.push_back(edit_splines[static_cast<std::size_t>(idx)].id);
                    }
                    if (group_sel_obj.size() == 1)
                        selected_object = group_sel_obj.front();
                    if (group_sel_spl.size() == 1)
                        selected_spline = group_sel_spl.front();
                    if (selected_object >= 0)
                        show_object_at(static_cast<std::size_t>(selected_object));
                    if (selected_spline >= 0)
                        show_spline_at(static_cast<std::size_t>(selected_spline));
                    marquee = {};
                    panel.set_status("Colocado — selección conservada");
                    log::info("editor marquee/paste placed (selection kept)");
                } else if (marquee.phase == MarqueePhase::None && ddx * ddx + ddy * ddy < 36.f) {
                    const float aspect_sc =
                        (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                    float wvp_sc[16]{};
                    camera.build_world_view_proj(aspect_sc, wvp_sc);
                    if (edit_mode == EditMode::PickSplineGrad) {
                        const int mk = pick_spline_end_marker(static_cast<float>(lmb_down_x),
                                                              static_cast<float>(lmb_down_y), fw, fh, wvp_sc);
                        if (mk >= 0) {
                            const auto& marker = end_markers[static_cast<std::size_t>(mk)];
                            int sidx = -1;
                            for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                                if (edit_splines[i].id == marker.owner_id) {
                                    sidx = static_cast<int>(i);
                                    break;
                                }
                            }
                            if (sidx >= 0) {
                                push_undo("spline_grad_edit_start");
                                selected_object = -1;
                                show_spline_at(static_cast<std::size_t>(selected_spline));
                                grad_edit_is_start = marker.is_start;
                                auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                                grad_edit_base = grad_edit_is_start ? s.start_grad : s.end_grad;
                                grab_height_my0 = static_cast<float>(lmb_down_y);
                                edit_mode = EditMode::EditSplineGrad;
                                panel.set_status(std::string("Gradiente ") +
                                                 (grad_edit_is_start ? "inicio" : "fin") +
                                                 ": mové mouse (Y), LMB=confirma");
                                log::info("editor edit gradient " +
                                          std::string(grad_edit_is_start ? "start" : "end") +
                                          " id=" + std::to_string(s.id));
                            } else {
                                panel.set_status("G gradiente: flecha sin spline");
                                edit_mode = EditMode::None;
                            }
                        } else {
                            panel.set_status("G gradiente: click una flecha de extremo");
                        }
                    } else if (edit_mode == EditMode::CompleteSpline && selected_spline >= 0 &&
                               static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                        const int mk = pick_spline_end_marker(static_cast<float>(lmb_down_x),
                                                              static_cast<float>(lmb_down_y), fw, fh, wvp_sc);
                        if (mk >= 0) {
                            float tx = 0.f, ty = 0.f, tz = 0.f, th = 0.f;
                            if (marker_pose(end_markers[static_cast<std::size_t>(mk)], tx, ty, tz, th)) {
                                auto& src = edit_splines[static_cast<std::size_t>(selected_spline)];
                                const auto end = spline::evaluate_spline(src, src.length);
                                const float dx = tx - end.x;
                                const float dy = ty - end.y;
                                const float dist = std::sqrt(dx * dx + dy * dy);
                                const float max_r = panel.spline_max_radius();
                                if (dist < 0.5f || dist > max_r) {
                                    panel.set_status("Complete: distance " + std::to_string(dist) +
                                                     " outside max radius");
                                } else {
                                    map::SplineInst ns = src;
                                    push_undo("complete_spline");
                                    ns.id = next_spline_id();
                                    ns.prev_id = src.id;
                                    ns.next_id = 0;
                                    ns.position.x = end.x;
                                    ns.position.y = end.y;
                                    ns.position.z = end.z;
                                    const float end_h_deg = end.heading_rad * (180.f / 3.14159265f);
                                    ns.heading_deg = end_h_deg;
                                    float out_len = 0.f, out_radius = 0.f;
                                    if (!spline::fit_spline_to_end_heading(end.x, end.y, end_h_deg, tx, ty, th,
                                                                           out_len, out_radius) ||
                                        out_len < 0.25f) {
                                        spline::fit_spline_to_xy(end.x, end.y, end_h_deg, tx, ty, out_len, out_radius);
                                    }
                                    ns.length = out_len;
                                    ns.radius = out_radius;
                                    src.next_id = ns.id;
                                    edit_splines.push_back(ns);
                                    remesh_spline(src);
                                    remesh_spline(edit_splines.back());
                                    upsert_transient_spline(src);
                                    upsert_transient_spline(edit_splines.back());
                                    persist_editor_xy(src.position.x, src.position.y);
                                    refresh_editor_lists();
                                    panel.set_status("Complete → spline id=" + std::to_string(ns.id));
                                }
                            }
                        }
                        edit_mode = EditMode::None;
                    } else if (edit_mode == EditMode::Mesh) {
                        const float aspect_m = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                        float wvp_m[16]{};
                        camera.build_world_view_proj(aspect_m, wvp_m);
                        const bool ctrl_m = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        const bool shift_m = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (mesh_edit.uv_g) {
                            if (!mesh_edit.uv_canvas_hov && !mesh_edit.uv_win_focused &&
                                !imgui_overlay.wants_capture_mouse())
                                mesh_uv_g_end(false);
                        } else if (!imgui_overlay.wants_capture_mouse()) {
                            if (mesh_xform.active)
                                mesh_xform_confirm();
                            else if (mesh_edit.tex_pick_kind != 0)
                                mesh_tex_pick_click(static_cast<float>(lmb_down_x), static_cast<float>(lmb_down_y), fw,
                                                    fh, wvp_m);
                            else if (mesh_edit.loopcut_on)
                                mesh_loopcut_apply();
                            else if (mesh_edit.knife_on)
                                mesh_knife_click(static_cast<float>(lmb_down_x), static_cast<float>(lmb_down_y), fw, fh);
                            else if (!mesh_edit.circle_sel)
                                handle_mesh_click(static_cast<float>(lmb_down_x), static_cast<float>(lmb_down_y), fw,
                                                  fh, wvp_m, shift_m, ctrl_m);
                        }
                    } else if (sco_path_edit.active &&
                               (sco_path_edit.moving || sco_path_edit.rotating || sco_path_edit.following ||
                                sco_path_edit.placing) &&
                               (edit_mode == EditMode::Grab || edit_mode == EditMode::Rotate ||
                                edit_mode == EditMode::SplineFollow)) {
                        sco_path_commit_live();
                        sco_path_edit.moving = sco_path_edit.rotating = sco_path_edit.following =
                            sco_path_edit.placing = false;
                        editor_log::event("LMB_PLACE", "confirm sco path live | " + session_state());
                        edit_mode = EditMode::None;
                        grabbing = false;
                        rotating = false;
                        new_grab_lock_xy = false;
                        grab_suppress_g_cancel = false;
                        r_tap = 0;
                        g_tap = 0;
                    } else if (edit_mode != EditMode::None) {
                        if (edit_mode == EditMode::NewFollow || edit_mode == EditMode::NewGrab) {
                            // Bake grab translate into baked mesh origin before final remesh.
                            if (edit_mode == EditMode::NewGrab) {
                                set_spline_chunk_offset(new_spline_draft.id, 0.f, 0.f);
                            }
                            push_undo("n_place_new_spline");
                            edit_splines.push_back(new_spline_draft);
                            transient_splines.push_back(new_spline_draft);
                            selected_spline = static_cast<int>(edit_splines.size() - 1);
                            selected_object = -1;
                            // Single selection on N place unless Shift keeps multi-select.
                            if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) {
                                group_sel_obj.clear();
                                group_sel_obj_ids.clear();
                                group_sel_spl.clear();
                                group_sel_spl_ids.clear();
                                group_sel_spl.push_back(selected_spline);
                                group_sel_spl_ids.push_back(edit_splines.back().id);
                            } else {
                                group_sel_spl.push_back(selected_spline);
                                group_sel_spl_ids.push_back(edit_splines.back().id);
                            }
                            remesh_spline(edit_splines.back());
                            clear_spline_wireframe(edit_splines.back());
                            const auto& s = edit_splines.back();
                            {
                                const auto mid = spline::evaluate_spline(s, s.length * 0.5f);
                                fire_place_asmr(editor::PlaceFxKind::SplineStripe, mid.x, mid.y, mid.z,
                                                (std::max)(1.f, s.length * 0.12f), 0, editor::SfxCue::WoodClick);
                            }
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
                            SetFocus(glfwGetWin32Window(window));
                            log::info("editor placed NEW spline id=" + std::to_string(s.id));
                        } else if (edit_mode == EditMode::TrimSpline && selected_spline >= 0 &&
                                   static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                            pending_trim_split = true;
                            clear_trim_preview();
                            grabbing = false;
                            edit_mode = EditMode::None;
                            panel.set_status("Split (Ctrl+R)…");
                        } else if (edit_mode == EditMode::NewScoFollow) {
                            // Already spawned on N — commit pose to map_actual.
                            if (selected_object >= 0 &&
                                static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                                auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                                o.position = new_sco_draft.position;
                                o.rotation_deg = new_sco_draft.rotation_deg;
                                for (auto& t : transient_objects) {
                                    if (t.id == o.id) {
                                        t = o;
                                        break;
                                    }
                                }
                                sync_object_gpu(o);
                                persist_editor_xy(o.position.x, o.position.y);
                                refresh_editor_lists();
                                show_object_at(static_cast<std::size_t>(selected_object));
                                log::info("editor placed NEW sco id=" + std::to_string(o.id));
                                fire_place_asmr(editor::PlaceFxKind::PlaceBounce, o.position.x, o.position.y,
                                                o.position.z, object_place_radius(o.id), o.id,
                                                editor::SfxCue::StonePlace);
                            }
                        } else if (edit_mode == EditMode::EditSplineGrad && selected_spline >= 0 &&
                                   static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                            auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                            remesh_spline(s);
                            upsert_transient_spline(s);
                            persist_editor_xy(s.position.x, s.position.y);
                            show_spline_at(static_cast<std::size_t>(selected_spline));
                            panel.set_status(std::string("Gradiente ") +
                                             (grad_edit_is_start ? "inicio=" : "fin=") +
                                             std::to_string(grad_edit_is_start ? s.start_grad : s.end_grad) +
                                             "%");
                            log::info("editor gradient placed id=" + std::to_string(s.id));
                        } else if (edit_mode == EditMode::Grab && shift_s_move.active) {
                            const bool need_remesh = true;
                            std::vector<GroupSettleItem> items;
                            items.reserve(shift_s_move.items.size());
                            for (const auto& it : shift_s_move.items) {
                                GroupSettleItem g{};
                                g.kind = it.kind;
                                g.id = it.id;
                                items.push_back(g);
                            }
                            editor_log::event("SHIFT_S_MOVE_PLACE",
                                              "dx=" + std::to_string(shift_s_move.cur_dx) +
                                                  " dy=" + std::to_string(shift_s_move.cur_dy) +
                                                  " count=" + std::to_string(items.size()) +
                                                  " | " + session_state());
                            shift_s_move_clear();
                            const std::size_t placed_n = items.size();
                            if (!begin_group_settle(std::move(items), need_remesh)) {
                                panel.set_status("Grupo: espera a que termine el guardado anterior");
                            } else {
                                panel.set_status("Grupo en cola async " + std::to_string(placed_n) +
                                                 " (se asienta en vivo, UI libre)");
                            }
                            editor_log::event("LMB_PLACE", "confirm shift_s XY | " + session_state());
                            edit_mode = EditMode::None;
                            grabbing = false;
                            rotating = false;
                            new_grab_lock_xy = false;
                            r_tap = 0;
                            g_tap = 0;
                        } else if (edit_mode == EditMode::Grab && group_grab_active) {
                            const bool need_remesh = true;
                            std::vector<GroupSettleItem> items;
                            items.reserve(group_grab_index.size());
                            for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                                GroupSettleItem it{};
                                it.kind = group_grab_kind[i];
                                it.id = (i < group_grab_ids.size())
                                            ? group_grab_ids[i]
                                            : (group_grab_kind[i] == 0
                                                   ? edit_objects[static_cast<std::size_t>(group_grab_index[i])].id
                                                   : edit_splines[static_cast<std::size_t>(group_grab_index[i])].id);
                                items.push_back(it);
                            }
                            const std::size_t placed_n = items.size();
                            if (!begin_group_settle(std::move(items), need_remesh)) {
                                panel.set_status("Grupo: espera a que termine el guardado anterior");
                            } else {
                                panel.set_status("Grupo en cola async " + std::to_string(placed_n) +
                                                 " (se asienta en vivo, UI libre)");
                            }
                            editor_log::event("LMB_PLACE", "confirm group grab | " + session_state());
                            xform_soa.write_back(edit_objects);
                            edit_mode = EditMode::None;
                            grabbing = false;
                            rotating = false;
                            new_grab_lock_xy = false;
                            r_tap = 0;
                            g_tap = 0;
                        } else if (edit_mode == EditMode::Rotate) {
                            const bool snap90 = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                            auto snap_yaw = [&](float free_deg) {
                                if (!snap90) return free_deg;
                                float rel = free_deg - grab_start_yaw;
                                while (rel > 180.f) rel -= 360.f;
                                while (rel < -180.f) rel += 360.f;
                                return grab_start_yaw + std::round(rel / 90.f) * 90.f;
                            };
                            auto apply_yaw_at = [&](float wx, float wy) {
                                if (selected_spline >= 0 &&
                                    static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                                    const float dx = wx - s.position.x;
                                    const float dy = wy - s.position.y;
                                    if (dx * dx + dy * dy > 1.e-4f) {
                                        s.heading_deg =
                                            snap_yaw(std::atan2(dx, dy) * (180.f / 3.14159265f));
                                        remesh_spline(s);
                                        persist_editor_xy(s.position.x, s.position.y);
                                        show_spline_at(static_cast<std::size_t>(selected_spline));
                                    }
                                } else if (selected_object >= 0 &&
                                           static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                                    auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                                    const float dx = wx - o.position.x;
                                    const float dy = wy - o.position.y;
                                    if (dx * dx + dy * dy > 1.e-4f) {
                                        o.rotation_deg.x =
                                            snap_yaw(std::atan2(dx, dy) * (180.f / 3.14159265f));
                                        sync_object_gpu(o);
                                        persist_editor_xy(o.position.x, o.position.y);
                                        show_object_at(static_cast<std::size_t>(selected_object));
                                    }
                                }
                            };
                            bool deselect_group = false;
                            if (!group_rotate_active && (!group_sel_obj.empty() || !group_sel_spl.empty())) {
                                if (ddx * ddx + ddy * ddy < 36.f) {
                                    const float aspect_d =
                                        (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                                    float wvp_d[16]{};
                                    camera.build_world_view_proj(aspect_d, wvp_d);
                                    const int spl_d = pick_spline_index(static_cast<float>(lmb_down_x),
                                                                        static_cast<float>(lmb_down_y), fw, fh, wvp_d);
                                    const int obj_d = pick_object_index(static_cast<float>(lmb_down_x),
                                                                        static_cast<float>(lmb_down_y), fw, fh, wvp_d);
                                    if (spl_d < 0 && obj_d < 0) deselect_group = true;
                                }
                            }
                            if (deselect_group) {
                                group_sel_obj.clear();
                                group_sel_spl.clear();
                                group_sel_obj_ids.clear();
                                group_sel_spl_ids.clear();
                                selected_spline = -1;
                                selected_object = -1;
                                panel.set_selection_text("(none)");
                                panel.set_status("Grupo deseleccionado");
                                editor_log::event("SHIFT_S_CLEAR", session_state());
                                shift_s_move_clear();
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
                                new_grab_lock_xy = false;
                                r_tap = 0;
                                g_tap = 0;
                            } else if (group_rotate_active && !group_grab_index.empty()) {
                                (void)confirm_group_rotate_place("lmb_rotate");
                            } else {
                                if (!group_rotate_active) {
                                    camera.ground_z = camera.target_z;
                                    if (const auto hit = camera.hit_ground_xy(static_cast<float>(lmb_down_x),
                                                                              static_cast<float>(lmb_down_y), fw, fh))
                                        apply_yaw_at(hit->x, hit->y);
                                }
                                edit_mode = EditMode::None;
                                grabbing = false;
                                rotating = false;
                                group_rotate_active = false;
                                group_rot_have_ref = false;
                                new_grab_lock_xy = false;
                                r_tap = 0;
                                g_tap = 0;
                                panel.set_status(snap90 ? "Rotacion 90 confirmada (LMB)"
                                                          : "Rotacion confirmada (LMB)");
                                if (selected_object >= 0 &&
                                    static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                                    const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                                    if (show_path_rails) rebuild_path_rails_for_owner(o.id, true);
                                    fire_place_asmr(editor::PlaceFxKind::RotateChips, o.position.x, o.position.y,
                                                    o.position.z, object_place_radius(o.id), o.id,
                                                    editor::SfxCue::GravelScrape);
                                } else if (selected_spline >= 0 &&
                                           static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                                    const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                                    if (show_path_rails) rebuild_path_rails_for_owner(s.id, false);
                                    fire_place_asmr(editor::PlaceFxKind::RotateChips, s.position.x, s.position.y,
                                                    s.position.z, (std::max)(1.f, s.length * 0.08f), 0,
                                                    editor::SfxCue::GravelScrape);
                                }
                            }
                        } else if (edit_mode == EditMode::GrabHeight) {
                            bool deselect_group = false;
                            if (!shift_s_move.active && !group_grab_active && !group_rotate_active &&
                                (!group_sel_obj.empty() || !group_sel_spl.empty())) {
                                if (ddx * ddx + ddy * ddy < 36.f) {
                                    const float aspect_d =
                                        (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                                    float wvp_d[16]{};
                                    camera.build_world_view_proj(aspect_d, wvp_d);
                                    const int spl_d = pick_spline_index(static_cast<float>(lmb_down_x),
                                                                        static_cast<float>(lmb_down_y), fw, fh, wvp_d);
                                    const int obj_d = pick_object_index(static_cast<float>(lmb_down_x),
                                                                        static_cast<float>(lmb_down_y), fw, fh, wvp_d);
                                    if (spl_d < 0 && obj_d < 0) deselect_group = true;
                                }
                            }
                            if (deselect_group) {
                                group_sel_obj.clear();
                                group_sel_spl.clear();
                                group_sel_obj_ids.clear();
                                group_sel_spl_ids.clear();
                                selected_spline = -1;
                                selected_object = -1;
                                panel.set_selection_text("(none)");
                                panel.set_status("Grupo deseleccionado");
                                editor_log::event("SHIFT_S_CLEAR", session_state());
                                shift_s_move_clear();
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
                                new_grab_lock_xy = false;
                                r_tap = 0;
                                g_tap = 0;
                            } else {
                                const bool need_remesh = true;
                                if (shift_s_move.active) {
                                    std::vector<GroupSettleItem> items;
                                    items.reserve(shift_s_move.items.size());
                                    for (const auto& it : shift_s_move.items) {
                                        GroupSettleItem g{};
                                        g.kind = it.kind;
                                        g.id = it.id;
                                        items.push_back(g);
                                    }
                                    shift_s_move_clear();
                                    if (!begin_group_settle(std::move(items), need_remesh))
                                        panel.set_status("Grupo: espera a que termine el guardado anterior");
                                    else
                                        panel.set_status("Altura confirmada (Shift+S grupo)");
                                } else if (group_grab_active) {
                                    std::vector<GroupSettleItem> items;
                                    items.reserve(group_grab_index.size());
                                    for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                                        GroupSettleItem it{};
                                        it.kind = group_grab_kind[i];
                                        it.id = (i < group_grab_ids.size())
                                                    ? group_grab_ids[i]
                                                    : (group_grab_kind[i] == 0
                                                           ? edit_objects[static_cast<std::size_t>(
                                                                 group_grab_index[i])]
                                                                 .id
                                                           : edit_splines[static_cast<std::size_t>(
                                                                 group_grab_index[i])]
                                                                 .id);
                                        items.push_back(it);
                                    }
                                    if (!begin_group_settle(std::move(items), need_remesh))
                                        panel.set_status("Grupo: espera a que termine el guardado anterior");
                                    else
                                        panel.set_status("Altura confirmada (grupo)");
                                } else if (selected_object >= 0 &&
                                           static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                                    sync_object_gpu(edit_objects[static_cast<std::size_t>(selected_object)]);
                                    persist_editor_xy(
                                        edit_objects[static_cast<std::size_t>(selected_object)].position.x,
                                        edit_objects[static_cast<std::size_t>(selected_object)].position.y);
                                    show_object_at(static_cast<std::size_t>(selected_object));
                                    panel.set_status("Altura confirmada (LMB)");
                                } else if (selected_spline >= 0 &&
                                           static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                                    auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                                    remesh_spline(s);
                                    set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
                                    rebuild_path_rails_for_owner(s.id, false);
                                    sync_all_end_markers();
                                    grab_mesh_ox = s.position.x;
                                    grab_mesh_oy = s.position.y;
                                    grab_mesh_oz = s.position.z;
                                    persist_editor_xy(s.position.x, s.position.y);
                                    show_spline_at(static_cast<std::size_t>(selected_spline));
                                    panel.set_status("Altura confirmada (LMB)");
                                }
                                editor_log::event("LMB_PLACE", "confirm grab height | " + session_state());
                                edit_mode = EditMode::None;
                                grabbing = false;
                                rotating = false;
                                new_grab_lock_xy = false;
                                r_tap = 0;
                                g_tap = 0;
                            }
                        } else if (shift_s_move.active || group_grab_active || group_rotate_active) {
                            // Fallback settle (should not reach for GrabHeight/Rotate).
                            const bool from_shift_s = shift_s_move.active;
                            const bool need_remesh = true;
                            std::vector<GroupSettleItem> items;
                            if (from_shift_s) {
                                items.reserve(shift_s_move.items.size());
                                for (const auto& it : shift_s_move.items) {
                                    GroupSettleItem g{};
                                    g.kind = it.kind;
                                    g.id = it.id;
                                    items.push_back(g);
                                }
                                shift_s_move_clear();
                            } else {
                                items.reserve(group_grab_index.size());
                                for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                                    GroupSettleItem it{};
                                    it.kind = group_grab_kind[i];
                                    it.id = (i < group_grab_ids.size())
                                                ? group_grab_ids[i]
                                                : (group_grab_kind[i] == 0
                                                       ? edit_objects[static_cast<std::size_t>(
                                                             group_grab_index[i])]
                                                             .id
                                                       : edit_splines[static_cast<std::size_t>(
                                                             group_grab_index[i])]
                                                             .id);
                                    items.push_back(it);
                                }
                            }
                            const std::size_t placed_n = items.size();
                            if (!begin_group_settle(std::move(items), need_remesh)) {
                                panel.set_status("Grupo: espera a que termine el guardado anterior");
                            } else {
                                panel.set_status("Grupo en cola async " + std::to_string(placed_n) +
                                                 " (se asienta en vivo, UI libre)");
                            }
                        } else if (selected_object >= 0 &&
                                   static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                            sync_object_gpu(edit_objects[static_cast<std::size_t>(selected_object)]);
                            persist_editor_xy(edit_objects[static_cast<std::size_t>(selected_object)].position.x,
                                              edit_objects[static_cast<std::size_t>(selected_object)].position.y);
                            show_object_at(static_cast<std::size_t>(selected_object));
                            log::info("editor place object id=" +
                                      std::to_string(edit_objects[static_cast<std::size_t>(selected_object)].id));
                        } else if (selected_spline < 0 && chain_sel_handle_spline != 0 &&
                                   (spline_grab_target != 0 || !fill_temp_ids.empty() ||
                                    !node_starter_ids.empty())) {
                            // Fallback PLACE: se perdió selected_spline (click en bola) pero hay
                            // estado de nodo/fill — resolver primary y remesh sólido.
                            for (int i = 0; i < static_cast<int>(edit_splines.size()); ++i) {
                                if (edit_splines[static_cast<std::size_t>(i)].id ==
                                    chain_sel_handle_spline) {
                                    selected_spline = i;
                                    break;
                                }
                            }
                            if (selected_spline < 0 && !node_starter_ids.empty()) {
                                for (int i = 0; i < static_cast<int>(edit_splines.size()); ++i) {
                                    if (edit_splines[static_cast<std::size_t>(i)].id ==
                                        node_starter_ids[0]) {
                                        selected_spline = i;
                                        break;
                                    }
                                }
                            }
                            log::info("[chain_dbg] PLACE fallback resolve selected_spline=" +
                                      std::to_string(selected_spline) + " chain_sel=" +
                                      std::to_string(chain_sel_handle_spline));
                        }
                        if (selected_spline >= 0 &&
                            static_cast<std::size_t>(selected_spline) < edit_splines.size() &&
                            (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight ||
                             edit_mode == EditMode::SplineFollow ||
                             grabbing || !fill_temp_ids.empty() || !node_starter_ids.empty())) {
                            auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                            log::info("[chain_dbg] PLACE begin id=" + std::to_string(s.id) +
                                      " prev=" + std::to_string(s.prev_id) +
                                      " next=" + std::to_string(s.next_id) +
                                      " temps=" + std::to_string(fill_temp_ids.size()) +
                                      " starters=" + std::to_string(node_starter_ids.size()) +
                                      " enders=" + std::to_string(node_ender_ids.size()));
                            dump_fill_chain("PLACE-before", s.prev_id != 0 ? s.prev_id : s.id, s.id);
                            if (s.next_id != 0)
                                dump_fill_chain("PLACE-after", s.id, s.next_id);
                            // Remesh sólido de todo lo tocado (cadena + nodo multi + temps + RR grupo)
                            {
                                std::unordered_set<std::int32_t> place_ids;
                                place_ids.insert(s.id);
                                for (int gsi : group_sel_spl) {
                                    if (gsi >= 0 && static_cast<std::size_t>(gsi) < edit_splines.size())
                                        place_ids.insert(edit_splines[static_cast<std::size_t>(gsi)].id);
                                }
                                for (std::int32_t tid : fill_temp_ids) place_ids.insert(tid);
                                for (std::int32_t id : node_starter_ids) place_ids.insert(id);
                                for (std::int32_t id : node_ender_ids) place_ids.insert(id);
                                std::int32_t root = s.id;
                                for (int i = 0; i < 32; ++i) {
                                    auto* sp = find_spline_by_id(root);
                                    if (!sp || sp->prev_id == 0) break;
                                    root = sp->prev_id;
                                }
                                for (int hop = 0; hop < 64 && root != 0; ++hop) {
                                    place_ids.insert(root);
                                    auto* sp = find_spline_by_id(root);
                                    if (!sp) break;
                                    root = sp->next_id;
                                }
                                for (std::int32_t sid : node_starter_ids) {
                                    std::int32_t id = sid;
                                    for (int hop = 0; hop < 32 && id != 0; ++hop) {
                                        place_ids.insert(id);
                                        auto* sp = find_spline_by_id(id);
                                        if (!sp) break;
                                        id = sp->next_id;
                                    }
                                }
                                for (std::int32_t id : place_ids) {
                                    auto* sp = find_spline_by_id(id);
                                    if (!sp) continue;
                                    set_spline_chunk_offset(sp->id, 0.f, 0.f, 0.f);
                                    remesh_spline_one(*sp, false, false);
                                    clear_spline_wireframe(*sp);
                                    rebuild_path_rails_for_owner(sp->id, false);
                                    if (auto* gpu = find_spline_gpu(sp->id)) {
                                        for (auto c : gpu->chunks) {
                                            renderer.set_chunk_wireframe(c, false);
                                            renderer.set_chunk_visible(c, true);
                                        }
                                    } else {
                                        // GPU entry missing after N/RR: sync remesh once to create strips.
                                        remesh_spline_one(*sp, false, true);
                                        clear_spline_wireframe(*sp);
                                    }
                                    const auto e = spline::evaluate_spline(*sp, sp->length);
                                    log::info("[chain_dbg] PLACE remesh id=" + std::to_string(sp->id) +
                                              " start=(" + std::to_string(sp->position.x) + "," +
                                              std::to_string(sp->position.y) + ") h=" +
                                              std::to_string(sp->heading_deg) +
                                              " L=" + std::to_string(sp->length) +
                                              " R=" + std::to_string(sp->radius) +
                                              " end=(" + std::to_string(e.x) + "," +
                                              std::to_string(e.y) + ") next=" +
                                              std::to_string(sp->next_id));
                                }
                            }
                            set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
                            sync_all_end_markers();
                            grab_mesh_ox = s.position.x;
                            grab_mesh_oy = s.position.y;
                            grab_mesh_oz = s.position.z;
                            spline_grab_target = 0;
                            node_height_mode = false;
                            grab_spline_id = 0;
                            node_grab_snap_valid = false;
                            // Bridges del fill/nodo quedan permanentes (ya no son "temps")
                            fill_temp_ids.clear();
                            if (!fill_temp_pool.empty()) {
                                for (std::int32_t tid : fill_temp_pool) {
                                    if (auto* gpu = find_spline_gpu(tid)) {
                                        for (auto c : gpu->chunks) {
                                            renderer.set_chunk_visible(c, false);
                                            renderer.set_chunk_wireframe(c, false);
                                        }
                                    }
                                }
                                purge_unused_fill_temps();
                            }
                            fill_temp_pool.clear();
                            node_starter_ids.clear();
                            node_starter_fex.clear();
                            node_starter_fey.clear();
                            node_starter_feh.clear();
                            node_ender_ids.clear();
                            chain_fix_next_id = 0;
                            chain_fix_prev_id = 0;
                            chain_fix_next_valid = false;
                            persist_editor_xy(s.position.x, s.position.y);
                            show_spline_at(static_cast<std::size_t>(selected_spline));
                            log::info("editor place spline id=" + std::to_string(s.id));
                        }
                        if (edit_mode != EditMode::GrabHeight && edit_mode != EditMode::Rotate) {
                            // attach_slide_armed only belongs to the Attach->Grab slide. When it stayed set (it was never
                            // cleared on N / Esc), every placement click was swallowed here: nothing got placed, the
                            // mode never ended and the next click pushed the same draft again (duplicates).
                            if (attach_slide_armed && edit_mode != EditMode::Grab) attach_slide_armed = false;
                            if (attach_slide_armed) {
                                attach_slide_armed = false; // first click only arms the slide; the next one places
                                panel.set_status(
                                    "Attach spline: deslizá sobre la .sli · LMB=asentar Esc=cancel");
                            } else {
                            editor_log::event("LMB_PLACE", "confirm edit_mode was active | " + session_state());
                            if (selected_object >= 0 &&
                                static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                                const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                                fire_place_asmr(editor::PlaceFxKind::SoftPuff, o.position.x, o.position.y,
                                                o.position.z, object_place_radius(o.id), 0,
                                                editor::SfxCue::WoodThud);
                            } else if (selected_spline >= 0 &&
                                       static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                                const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
                                fire_place_asmr(editor::PlaceFxKind::SplineStripe, s.position.x, s.position.y,
                                                s.position.z, (std::max)(1.f, s.length * 0.08f), 0,
                                                editor::SfxCue::GravelScrape);
                            }
                            sco_path_edit.moving = sco_path_edit.rotating = sco_path_edit.following =
                                sco_path_edit.placing = false;
                            edit_mode = EditMode::None;
                            grabbing = false;
                            rotating = false;
                            new_grab_lock_xy = false;
                            grab_suppress_g_cancel = false;
                            attach_slide_armed = false;
                            r_tap = 0;
                            g_tap = 0;
                            }
                        }
                    } else {
                        const float aspect =
                            (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                        float wvp_pick[16]{};
                        camera.build_world_view_proj(aspect, wvp_pick);
                        const bool ctrl_held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        const bool shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (edit_mode == EditMode::Mesh) {
                            if (mesh_edit.uv_g) {
                                if (!mesh_edit.uv_canvas_hov && !mesh_edit.uv_win_focused &&
                                    !imgui_overlay.wants_capture_mouse())
                                    mesh_uv_g_end(false);
                            } else if (!imgui_overlay.wants_capture_mouse()) {
                                if (mesh_xform.active)
                                    mesh_xform_confirm();
                                else if (mesh_edit.tex_pick_kind != 0)
                                    mesh_tex_pick_click(static_cast<float>(lmb_down_x), static_cast<float>(lmb_down_y),
                                                        fw, fh, wvp_pick);
                                else if (mesh_edit.loopcut_on)
                                    mesh_loopcut_apply();
                                else if (mesh_edit.knife_on)
                                    mesh_knife_click(static_cast<float>(lmb_down_x), static_cast<float>(lmb_down_y), fw,
                                                     fh);
                                else if (!mesh_edit.circle_sel)
                                    handle_mesh_click(static_cast<float>(lmb_down_x), static_cast<float>(lmb_down_y), fw,
                                                      fh, wvp_pick, shift_held, ctrl_held);
                            }
                        } else {
                        bool sco_path_click = false;
                        if (sco_path_edit.active && edit_mode == EditMode::None) {
                            // Native .sco path editor: the clickable visual is the rail.
                            const int rail = pick_path_rail(static_cast<float>(lmb_down_x),
                                                            static_cast<float>(lmb_down_y), fw, fh, wvp_pick,
                                                            true);
                            if (rail >= 0) {
                                const auto& pr = path_rails[static_cast<std::size_t>(rail)];
                                if (pr.from_object && pr.owner_id == sco_path_edit.object_id) {
                                    const int path_idx = pr.path_index;
                                    selected_spline = -1;
                                    selected_object = -1;
                                    selected_path_rail = rail;
                                    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
                                        sco_path_edit.toggle_select(path_idx);
                                    else
                                        sco_path_edit.select_only(path_idx);
                                    panel.hide_path_rule();
                                    panel.set_status("Path .sco " + std::to_string(path_idx) + " (id " +
                                                     std::to_string(path_idx) + ")");
                                    sco_path_click = true;
                                }
                            }
                        }
                        if (!sco_path_click) {
                            // Tracks & Trips (Add Station / Show Timetable) and Station Links (start / end):
                            // only [busstop] objects are valid targets, picked in the 3D view.
                            extern int g_omsi_tt_mode;
                            extern int g_omsi_sl_mode;
                            const bool stop_pick =
                                edit_mode == EditMode::None &&
                                ((ev.tool == ui::EditorTool::TracksTrips && g_omsi_tt_mode >= 1) ||
                                 (ev.tool == ui::EditorTool::StationLinks && g_omsi_sl_mode <= 1));
                            if (stop_pick) {
                                const int bi2 = pick_object_index(static_cast<float>(lmb_down_x),
                                                                  static_cast<float>(lmb_down_y), fw, fh, wvp_pick);
                                if (bi2 >= 0 && static_cast<std::size_t>(bi2) < edit_objects.size()) {
                                    const auto& bo = edit_objects[static_cast<std::size_t>(bi2)];
                                    const auto bm = sco::load_sco(bo.sco_path);
                                    if (bm && bm->special == sco::ScoSpecial::Busstop) {
                                        auto& man = omsi::tt::global_tt_man();
                                        man.ensure_loaded(cfg.global.root);
                                        selected_object = bi2;
                                        selected_spline = -1;
                                        const auto oid = bo.id;
                                        if (ev.tool == ui::EditorTool::TracksTrips && g_omsi_tt_mode == 1) {
                                            std::string bname;
                                            std::int32_t tile_i = 0;
                                            const int bidx = man.find_busstop_by_object(oid);
                                            if (bidx >= 0) {
                                                bname = man.data.busstops[static_cast<std::size_t>(bidx)].name;
                                                tile_i = man.data.busstops[static_cast<std::size_t>(bidx)].tile_index;
                                            }
                                            if (man.selected_trip < 0) {
                                                panel.set_status("TT Add Station: crea/selecciona un trip");
                                            } else if (man.add_trip_busstop(man.selected_trip, oid, bname, tile_i,
                                                                            man.selected_track_entry,
                                                                            man.selected_trip_station + 1)) {
                                                panel.set_status("TT +station id=" + std::to_string(oid) +
                                                                 (bname.empty() ? "" : " " + bname));
                                            } else {
                                                panel.set_status("TT Add Station: elegí antes la track entry de esta parada");
                                            }
                                        } else if (ev.tool == ui::EditorTool::TracksTrips && g_omsi_tt_mode == 2) {
                                            omsi::tt::ui::global_ui().show_station_timetable(man, oid);
                                            panel.set_status("Timetable of station " + std::to_string(oid));
                                        } else if (ev.tool == ui::EditorTool::StationLinks) {
                                            if (man.selected_stnlink < 0) man.add_stnlink();
                                            man.set_stnlink_busstop(man.selected_stnlink, oid, g_omsi_sl_mode == 0);
                                            panel.set_status(std::string("SL Set ") + (g_omsi_sl_mode == 0 ? "Start" : "End") +
                                                             " id=" + std::to_string(oid));
                                        }
                                        sco_path_click = true;
                                    }
                                }
                            }
                        }
                        if (!sco_path_click) {
                            extern int g_omsi_tt_mode;
                            extern int g_omsi_sl_mode;
                            if (edit_mode == EditMode::None &&
                                (g_omsi_tt_mode == 0 ||
                                 (g_omsi_sl_mode == 2 && panel.current_tool() == ui::EditorTool::StationLinks))) {
                                const int rail_tt = pick_path_rail(static_cast<float>(lmb_down_x),
                                                                   static_cast<float>(lmb_down_y), fw, fh, wvp_pick,
                                                                   true);
                                if (rail_tt >= 0) {
                                    const auto& pr = path_rails[static_cast<std::size_t>(rail_tt)];
                                    auto resolve_tile = [&]() -> std::int32_t {
                                        int ti = 0;
                                        for (const auto& ref : cfg.global.tiles) {
                                            if (ref.grid_x == pr.tile_gx && ref.grid_y == pr.tile_gy) return ti;
                                            ++ti;
                                        }
                                        return 0;
                                    };
                                    // Builds the PathPickInfo of one rail (same data OMSI's RefreshTrackEntry caches).
                                    auto build_tt_pick = [&](const PathRail& rr) {
                                        omsi::tt::PathPickInfo pick{};
                                        pick.owner_id = rr.owner_id;
                                        pick.path_index = rr.path_index;
                                        int ti2 = 0, tile_found = 0;
                                        for (const auto& ref : cfg.global.tiles) {
                                            if (ref.grid_x == rr.tile_gx && ref.grid_y == rr.tile_gy) { tile_found = ti2; break; }
                                            ++ti2;
                                        }
                                        pick.tile_index = tile_found;
                                        pick.internal_path_no = rr.path_index;
                                        pick.length = 0.f;
                                        if (!rr.from_object) {
                                            for (const auto& sp2 : edit_splines)
                                                if (sp2.id == rr.owner_id) { pick.length = sp2.length > 0.f ? sp2.length : 0.f; break; }
                                        } else {
                                            // Real .sco path length (arcs given in degrees are converted with the radius).
                                            for (const auto& o : edit_objects) {
                                                if (o.id != rr.owner_id) continue;
                                                if (const auto sco_m = sco::load_sco(o.sco_path)) {
                                                    if (rr.path_index >= 0 &&
                                                        static_cast<std::size_t>(rr.path_index) < sco_m->paths.size()) {
                                                        const auto& pth = sco_m->paths[static_cast<std::size_t>(rr.path_index)];
                                                        float plen = std::abs(pth.length);
                                                        if (std::abs(pth.radius) > 0.001f && plen > 0.001f && plen <= 360.f &&
                                                            plen / std::abs(pth.radius) > 3.2f)
                                                            plen = std::abs(pth.radius) * plen * (3.14159265f / 180.f);
                                                        pick.length = plen;
                                                    }
                                                }
                                                break;
                                            }
                                        }
                                        return pick;
                                    };
                                    if (g_omsi_tt_mode == 0 && panel.current_tool() == ui::EditorTool::TracksTrips) {
                                        auto& man = omsi::tt::global_tt_man();
                                        man.ensure_loaded(cfg.global.root);
                                        // The selection comes from the ImGui combos only (no legacy file list).
                                        if (man.selected_track < 0 && !man.data.tracks.empty()) man.selected_track = 0;
                                        // Ctrl: complete the route from the last picked rail to this one (same
                                        // endpoint-chain search as the Traffic tab), in path order.
                                        std::vector<int> to_add;
                                        const bool ctrl_now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                                        if (ctrl_now && traffic_anchor_rail >= 0 && !traffic_chain_rails.empty()) {
                                            for (const int cr : traffic_chain_rails)
                                                if (cr != traffic_anchor_rail && cr >= 0 &&
                                                    static_cast<std::size_t>(cr) < path_rails.size())
                                                    to_add.push_back(cr);
                                        }
                                        if (std::find(to_add.begin(), to_add.end(), rail_tt) == to_add.end())
                                            to_add.push_back(rail_tt);
                                        int added = 0;
                                        for (const int ri : to_add) {
                                            const auto& rr = path_rails[static_cast<std::size_t>(ri)];
                                            if (man.add_track_entry(man.selected_track, man.selected_track_entry,
                                                                    build_tt_pick(rr)))
                                                ++added;
                                        }
                                        if (added > 0) {
                                            traffic_anchor_rail = rail_tt;
                                            traffic_chain_rails.clear();
                                            panel.set_status("TT +" + std::to_string(added) + " track entr" +
                                                             (added == 1 ? "y" : "ies") + " (Ctrl = completar ruta)");
                                            sco_path_click = true;
                                        } else {
                                            panel.set_status("TT Add Trackentry: crea/selecciona un track");
                                        }
                                    }
                                    if (g_omsi_sl_mode == 2 && panel.current_tool() == ui::EditorTool::StationLinks) {
                                        auto& man = omsi::tt::global_tt_man();
                                        if (!man.loaded) man.load(cfg.global.root);
                                        if (man.selected_stnlink < 0) man.add_stnlink();
                                        omsi::tt::PathPickInfo pick{};
                                        pick.owner_id = pr.owner_id;
                                        pick.path_index = pr.path_index;
                                        pick.tile_index = resolve_tile();
                                        pick.length = 0.f;
                                        if (man.add_stnlink_entry(man.selected_stnlink, man.selected_stnlink_entry, pick)) {
                                            panel.set_status("SL +segment id=" + std::to_string(pick.owner_id) +
                                                             " (memoria)");
                                            sco_path_click = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (!sco_path_click) {
                        const bool imgui_ctrl = ImGui::GetIO().KeyCtrl;
                        const bool imgui_shift = ImGui::GetIO().KeyShift;
                        const bool add_mod = ctrl_held || shift_held || imgui_ctrl || imgui_shift;
                        const bool add_to_group =
                            add_mod || sli_bake_pick ||
                            (ev.tool == ui::EditorTool::Spline && panel.imgui_state().spline_multi_pick) ||
                            (ev.tool == ui::EditorTool::Object && panel.imgui_state().object_multi_pick);
                        // Seed group from the current primary pick so Ctrl/multi doesn't drop the first.
                        auto seed_primary_into_group = [&]() {
                            if (selected_object >= 0 &&
                                static_cast<std::size_t>(selected_object) < edit_objects.size() &&
                                std::find(group_sel_obj.begin(), group_sel_obj.end(), selected_object) ==
                                    group_sel_obj.end()) {
                                group_sel_obj.push_back(selected_object);
                                group_sel_obj_ids.push_back(
                                    edit_objects[static_cast<std::size_t>(selected_object)].id);
                            }
                            if (selected_spline >= 0 &&
                                static_cast<std::size_t>(selected_spline) < edit_splines.size() &&
                                std::find(group_sel_spl.begin(), group_sel_spl.end(), selected_spline) ==
                                    group_sel_spl.end()) {
                                group_sel_spl.push_back(selected_spline);
                                group_sel_spl_ids.push_back(
                                    edit_splines[static_cast<std::size_t>(selected_spline)].id);
                            }
                        };
                        bool autofill_click_consumed = false;
                        if (autofill_path_mode &&
                            (ev.tool == ui::EditorTool::Spline || sco_path_edit.aa_mode)) {
                            float wvp_af[16]{};
                            camera.build_world_view_proj(aspect, wvp_af);
                            auto pick = pick_path_endpoint_near_mouse(static_cast<float>(lmb_down_x),
                                                                            static_cast<float>(lmb_down_y), fw, fh,
                                                                            wvp_af);
                            // CPU fallback: nearest live path endpoint of the active .sco (the "bolita"),
                            // so AA grabs the endpoint under the cursor instead of a neighbouring path.
                            if (!pick.valid && sco_path_edit.active) {
                                map::MapObjectInst* aobj = nullptr;
                                for (auto& o : edit_objects)
                                    if (o.id == sco_path_edit.object_id) {
                                        aobj = &o;
                                        break;
                                    }
                                if (aobj) {
                                    const float ath = sco_obj_th(*aobj);
                                    const XMMATRIX awm = sco_world_matrix(*aobj, nullptr, ath, 0.f, 0.f);
                                    float bd = 44.f * 44.f;
                                    for (int pi = 0; pi < static_cast<int>(sco_path_edit.paths.size()); ++pi) {
                                        const auto& sp = sco_path_edit.paths[static_cast<std::size_t>(pi)];
                                        map::SplineInst mini = sco::sco_path_to_spline_inst(sp);
                                        mini.length = (std::max)(0.05f, std::abs(sp.length));
                                        for (int e = 0; e < 2; ++e) {
                                            const auto lp = spline::evaluate_spline(mini, e ? mini.length : 0.f);
                                            DirectX::XMFLOAT3 w{};
                                            DirectX::XMStoreFloat3(&w, xform_point(awm, lp.x, lp.y, lp.z));
                                            const float d = screen_dist2(w.x, w.y, w.z,
                                                                         static_cast<float>(lmb_down_x),
                                                                         static_cast<float>(lmb_down_y), fw, fh, wvp_af);
                                            if (d < bd) {
                                                bd = d;
                                                pick.valid = true;
                                                pick.owner_id = sco_path_edit.object_id;
                                                pick.from_object = true;
                                                pick.path_index = pi;
                                                pick.is_start = (e == 0);
                                                pick.x = w.x;
                                                pick.y = w.y;
                                                pick.z = w.z;
                                                pick.heading_deg = lp.heading_rad * (180.f / 3.14159265f) +
                                                                   aobj->rotation_deg.x;
                                            }
                                        }
                                    }
                                }
                            }
                            if (pick.valid) {
                                if (autofill_path_stage == 0) {
                                    autofill_path_a = pick;
                                    autofill_path_stage = 1;
                                    if (!pick.from_object) {
                                        for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                                            if (edit_splines[i].id != pick.owner_id) continue;
                                            selected_spline = static_cast<int>(i);
                                            selected_object = -1;
                                            sticky_sli_path = edit_splines[i].sli_path;
                                            panel.remember_catalog_path(sticky_sli_path);
                                            show_spline_at(i);
                                            break;
                                        }
                                    }
                                    panel.set_status("Path autofill: click destination path endpoint (src id=" +
                                                     std::to_string(pick.owner_id) + ")");
                                } else {
                                    // Second pick: ask which side of the target to join (start/end).
                                    autofill_path_b = pick;
                                    aa_join_pending = true;
                                }
                                SetFocus(glfw_hwnd);
                                autofill_click_consumed = true;
                            } else if (sco_path_edit.active) {
                                panel.set_status("AA: click un extremo (CPU/GPU)");
                                autofill_click_consumed = true;
                            } else {
                                // Click vacío / fuera de path → cancelar AA y seguir a selección normal.
                                autofill_path_mode = false;
                                autofill_path_stage = 0;
                                autofill_path_a = {};
                                autofill_path_b = {};
                                panel.set_status("Path autofill cancelado (click vacío)");
                                log::info("editor path autofill cancelled (empty click)");
                            }
                        }
                        if (!autofill_click_consumed && autofill_spline_ends && selected_spline >= 0 &&
                            ev.tool == ui::EditorTool::Spline) {
                            const int mk = pick_spline_end_marker(static_cast<float>(lmb_down_x),
                                                                  static_cast<float>(lmb_down_y), fw, fh, wvp_pick);
                            if (mk >= 0) {
                                float tx = 0.f, ty = 0.f, tz = 0.f, th = 0.f;
                                if (marker_pose(end_markers[static_cast<std::size_t>(mk)], tx, ty, tz, th)) {
                                    autofill_from_selection_to(tx, ty, tz, th,
                                                               end_markers[static_cast<std::size_t>(mk)].is_start);
                                    SetFocus(glfw_hwnd);
                                    autofill_click_consumed = true;
                                }
                            } else {
                                // Click vacío u otra zona → cancelar A y permitir seleccionar otra spline.
                                autofill_spline_ends = false;
                                panel.set_autofill(false);
                                panel.set_status("Autofill cancelado (click vacío)");
                                log::info("editor spline autofill cancelled (empty click)");
                            }
                        }
                        if (autofill_click_consumed) {
                            // ya manejado
                        } else if (sco_path_edit.active && ev.tool != ui::EditorTool::Spline) {
                            // LMB vacío: no deseleccionar ni salir de paths .sco
                        } else if (ev.tool == ui::EditorTool::TrafficRules) {
                            // Only path rails are interactive in Traffic Rules (OMSI).
                            const int rail = pick_path_rail(static_cast<float>(lmb_down_x),
                                                            static_cast<float>(lmb_down_y), fw, fh, wvp_pick,
                                                            show_path_rails);
                            if (rail >= 0) {
                                selected_spline = -1;
                                selected_object = -1;
                                if (paint_path_rail_fast(rail)) {
                                    paint_drag_last_rail = rail;
                                    panel.set_status("Painted path " + std::to_string(
                                        path_rails[static_cast<std::size_t>(rail)].path_index));
                                }
                            }
                        } else if (ev.tool == ui::EditorTool::Spline) {
                            // Si el press fue sobre una bola/nodo, no re-seleccionar el body.
                            if (node_click_suppress_body_pick) {
                                node_click_suppress_body_pick = false;
                                selected_path_rail = -1;
                                panel.hide_path_rule();
                                // Mantener selected_spline=-1 y chain_sel del nodo
                            } else {
                            const int best = pick_spline_index(static_cast<float>(lmb_down_x),
                                                               static_cast<float>(lmb_down_y), fw, fh, wvp_pick);
                            selected_path_rail = -1;
                            panel.hide_path_rule();
                            if (add_to_group && best >= 0) {
                                seed_primary_into_group();
                                const auto sid = edit_splines[static_cast<std::size_t>(best)].id;
                                const bool multi_only = panel.imgui_state().spline_multi_pick;
                                const auto it = std::find(group_sel_spl.begin(), group_sel_spl.end(), best);
                                if (it != group_sel_spl.end()) {
                                    // Multi mode only grows; Ctrl/Shift without multi still toggles off.
                                    if (!multi_only) {
                                        group_sel_spl.erase(it);
                                        group_sel_spl_ids.erase(
                                            std::remove(group_sel_spl_ids.begin(), group_sel_spl_ids.end(), sid),
                                            group_sel_spl_ids.end());
                                    }
                                } else {
                                    group_sel_spl.push_back(best);
                                    group_sel_spl_ids.push_back(sid);
                                }
                                selected_spline = best;
                                selected_object = -1;
                                sticky_sli_path = edit_splines[static_cast<std::size_t>(best)].sli_path;
                                panel.remember_catalog_path(sticky_sli_path);
                                show_spline_at(static_cast<std::size_t>(best));
                                panel.set_status("Grupo: " + std::to_string(group_sel_obj.size()) + " obj, " +
                                                 std::to_string(group_sel_spl.size()) +
                                                 (multi_only ? " spl (multi+)" : " spl (Ctrl/Shift+click)"));
                            } else if (!add_to_group && (!group_sel_obj.empty() || !group_sel_spl.empty())) {
                                if (best < 0 && !sli_bake_pick) {
                                    group_sel_obj.clear();
                                    group_sel_spl.clear();
                                    group_sel_obj_ids.clear();
                                    group_sel_spl_ids.clear();
                                    selected_spline = -1;
                                    selected_object = -1;
                                    panel.set_selection_text("(none)");
                                    panel.set_status("Grupo deseleccionado");
                                    editor_log::event("SHIFT_S_CLEAR", session_state());
                                } else {
                                    panel.set_status("Grupo activo (" +
                                                     std::to_string(group_sel_obj.size()) + "+" +
                                                     std::to_string(group_sel_spl.size()) +
                                                     "): click vacio/Esc=soltar, G=mover");
                                }
                            } else if (add_to_group && best < 0) {
                                // Multi-select / Ctrl+Shift+click: empty click keeps the group.
                            } else {
                                if (best >= 0) {
                                    group_sel_obj.clear();
                                    group_sel_spl.clear();
                                    group_sel_obj_ids.clear();
                                    group_sel_spl_ids.clear();
                                    selected_spline = best;
                                    selected_object = -1;
                                    show_spline_at(static_cast<std::size_t>(best));
                                } else {
                                    selected_spline = -1;
                                    selected_object = -1;
                                    if (!sticky_sli_path.empty()) {
                                        panel.remember_catalog_path(sticky_sli_path);
                                        panel.set_selection_text("Plantilla: " +
                                                                 sticky_sli_path.filename().string() +
                                                                 " (N=nuevo)");
                                    } else {
                                        panel.set_selection_text("(none)");
                                    }
                                }
                            }
                            } // end else (!node_click_suppress_body_pick)
                        } else if (ev.tool == ui::EditorTool::Object) {
                            // Respect the active tool / selection mode: Alt+LMB (orbit), RMB drags and any live
                            // gizmo (edit_mode) are not selection clicks. A real click also needs a still cursor.
                            const bool alt_sel = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
                                                 (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
                            const bool rmb_sel = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                            const bool sel_click = !alt_sel && !rmb_sel && edit_mode == EditMode::None &&
                                                   ddx * ddx + ddy * ddy < 36.f;
                            if (sel_click && ddx * ddx + ddy * ddy < 36.f) {
                                int best_ep = -1;
                                {
                                    static std::vector<std::size_t> ep_cand;
                                    ep_cand.clear();
                                    ep_cand.reserve(entry_chunks.size());
                                    for (const auto& e : entry_chunks) {
                                        if (e.visible) ep_cand.push_back(e.chunk);
                                    }
                                    float ep_t = 1e30f;
                                    if (!ep_cand.empty()) {
                                        if (const auto hitc = renderer.pick_chunk(
                                                static_cast<float>(lmb_down_x), static_cast<float>(lmb_down_y),
                                                static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh),
                                                wvp_pick, false, ep_cand.data(), ep_cand.size(), &ep_t)) {
                                            for (std::size_t i = 0; i < entry_chunks.size(); ++i) {
                                                if (entry_chunks[i].chunk == *hitc) {
                                                    best_ep = static_cast<int>(i);
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                                if (best_ep < 0) {
                                    if (const auto hit = camera.hit_ground_xy(static_cast<float>(lmb_down_x),
                                                                              static_cast<float>(lmb_down_y), fw, fh)) {
                                        float best_d2 = 36.f;
                                        for (std::size_t i = 0; i < entry_chunks.size(); ++i) {
                                            const auto& e = entry_chunks[i];
                                            // hit_ground_xy is editor space; entry_chunks store world space.
                                            const float dx = (hit->x + tile_ox) - e.world_x;
                                            const float dy = (hit->y + tile_oy) - e.world_y;
                                            const float d2 = dx * dx + dy * dy;
                                            if (d2 < best_d2) {
                                                best_d2 = d2;
                                                best_ep = static_cast<int>(i);
                                            }
                                        }
                                    }
                                }
                                if (best_ep >= 0) {
                                        selected_entrypoint = best_ep;
                                        selected_object = -1;
                                        selected_spline = -1;
                                        const auto& ep =
                                            cfg.global.entrypoints[static_cast<std::size_t>(
                                                entry_chunks[static_cast<std::size_t>(best_ep)].ep_index)];
                                        panel.set_selection_text("entrypoint id=" + std::to_string(ep.id) + " " +
                                                                 ep.name);
                                        panel.set_status("Entrypoint seleccionado — G=mover");
                                        SetFocus(glfw_hwnd);
                                        continue;
                                }
                            }
                            const int best = (!sel_click || sco_path_edit.active || edit_mode == EditMode::Mesh)
                                                 ? -1
                                                 : pick_object_index(static_cast<float>(lmb_down_x),
                                                                     static_cast<float>(lmb_down_y), fw, fh, wvp_pick);
                            // A forest instance / GroundCover (grass) may be the thing under the cursor: those are
                            // not edit_objects, so the click handler of the BeamNG module already selected them and
                            // this branch must not clear / overwrite that selection.
                            const bool other_sel = bng_veg_owns_lmb ||
                                                   (best < 0 && (bng_forest_pick_won || bng_gc_pick_won || ai_pick_won));
                            // an AI car: its details / delete window (it is no edit_object)
                            if (sel_click) {
                                if (best < 0 && ai_pick_won) {
                                    ai_selected_id = ai_pick_id;
                                    panel.set_status("Auto IA seleccionado: Supr = borrar, ventana Auto IA = detalles");
                                } else {
                                    ai_selected_id = 0;
                                }
                            }
                            selected_path_rail = -1;
                            panel.hide_path_rule();
                            if (sel_click && !other_sel) {
                            if (add_to_group && best >= 0) {
                                seed_primary_into_group();
                                const auto oid = edit_objects[static_cast<std::size_t>(best)].id;
                                const bool multi_only = panel.imgui_state().object_multi_pick;
                                const auto it = std::find(group_sel_obj.begin(), group_sel_obj.end(), best);
                                if (it != group_sel_obj.end()) {
                                    if (!multi_only) {
                                        group_sel_obj.erase(it);
                                        group_sel_obj_ids.erase(
                                            std::remove(group_sel_obj_ids.begin(), group_sel_obj_ids.end(), oid),
                                            group_sel_obj_ids.end());
                                    }
                                } else {
                                    group_sel_obj.push_back(best);
                                    group_sel_obj_ids.push_back(oid);
                                }
                                selected_object = best;
                                selected_spline = -1;
                                sticky_sco_path = edit_objects[static_cast<std::size_t>(best)].sco_path;
                                panel.remember_catalog_path(sticky_sco_path);
                                if (const auto ei =
                                        spline_attach_entry_for_object(edit_objects[static_cast<std::size_t>(best)].id))
                                    select_spline_attachment_entry(*ei);
                                else
                                    show_object_at(static_cast<std::size_t>(best));
                                panel.set_status("Grupo: " + std::to_string(group_sel_obj.size()) + " obj, " +
                                                 std::to_string(group_sel_spl.size()) +
                                                 (multi_only ? " spl (multi+)" : " spl (Ctrl+click)"));
                            } else if (add_to_group && best < 0) {
                                // Multi-select: empty click keeps the group.
                            } else if (!group_sel_obj.empty() || !group_sel_spl.empty()) {
                                if (best < 0) {
                                    group_sel_obj.clear();
                                    group_sel_spl.clear();
                                    group_sel_obj_ids.clear();
                                    group_sel_spl_ids.clear();
                                    selected_object = -1;
                                    selected_spline = -1;
                                    panel.set_selection_text("(none)");
                                    panel.set_status("Grupo deseleccionado");
                                    editor_log::event("SHIFT_S_CLEAR", session_state());
                                } else {
                                    panel.set_status("Grupo activo (" +
                                                     std::to_string(group_sel_obj.size()) + "+" +
                                                     std::to_string(group_sel_spl.size()) +
                                                     "): click vacio/Esc=soltar, G=mover");
                                }
                            } else {
                                if (best >= 0) {
                                    // Properties: only on a double click (same object within 400 ms) or when the
                                    // selection really changed. Re-clicking the selected object used to reopen /
                                    // re-stomp the properties box on every click.
                                    static double s_sel_click_t = 0.0;
                                    static int s_sel_click_i = -1;
                                    const double click_now = glfwGetTime();
                                    const bool same_sel = selected_object == best;
                                    const bool dbl_click = same_sel && s_sel_click_i == best &&
                                                           (click_now - s_sel_click_t) < 0.40;
                                    s_sel_click_t = click_now;
                                    s_sel_click_i = best;
                                    selected_object = best;
                                    selected_spline = -1;
                                    if (!same_sel || dbl_click) {
                                        if (const auto ei = spline_attach_entry_for_object(
                                                edit_objects[static_cast<std::size_t>(best)].id))
                                            select_spline_attachment_entry(*ei);
                                        else
                                            show_object_at(static_cast<std::size_t>(best));
                                    }
                                    // TTData / Station Links (memory — Save mapa writes disk)
                                    {
                                        extern int g_omsi_tt_mode;
                                        extern int g_omsi_sl_mode;
                                        auto& man = omsi::tt::global_tt_man();
                                        if (!man.loaded) man.load(cfg.global.root);
                                        const auto oid = edit_objects[static_cast<std::size_t>(best)].id;
                                        if (g_omsi_tt_mode == 1) {
                                            if (man.selected_trip < 0 && !man.data.trips.empty())
                                                man.selected_trip = 0;
                                            std::string bname;
                                            std::int32_t tile_i = 0;
                                            const int bi = man.find_busstop_by_object(oid);
                                            if (bi >= 0) {
                                                bname = man.data.busstops[static_cast<std::size_t>(bi)].name;
                                                tile_i = man.data.busstops[static_cast<std::size_t>(bi)].tile_index;
                                            }
                                            if (man.add_trip_busstop(man.selected_trip, oid, bname, tile_i,
                                                                     man.selected_track_entry)) {
                                                panel.set_tt_from_man();
                                                panel.set_status("TT +station id=" + std::to_string(oid) + " (memoria)");
                                            }
                                            else
                                                panel.set_status("TT Add Station: crea/selecciona un trip");
                                        }
                                        if (panel.current_tool() == ui::EditorTool::StationLinks &&
                                            (g_omsi_sl_mode == 0 || g_omsi_sl_mode == 1)) {
                                            if (man.selected_stnlink < 0) man.add_stnlink();
                                            man.set_stnlink_busstop(man.selected_stnlink, oid, g_omsi_sl_mode == 0);
                                            panel.set_status(std::string("SL Set ") +
                                                             (g_omsi_sl_mode == 0 ? "Start" : "End") +
                                                             " id=" + std::to_string(oid) + " (memoria)");
                                        }
                                        if (g_omsi_tt_mode == 2) {
                                            omsi::tt::ui::global_ui().show_station_timetable(man, oid);
                                            panel.set_status("Timetable of station " + std::to_string(oid));
                                        }
                                    }
                                } else {
                                    selected_object = -1;
                                    selected_spline = -1;
                                    if (!sticky_sco_path.empty()) {
                                        panel.remember_catalog_path(sticky_sco_path);
                                        panel.set_selection_text("Plantilla: " +
                                                                 sticky_sco_path.filename().string() +
                                                                 " (N=nuevo)");
                                    } else {
                                        panel.set_selection_text("(none)");
                                    }
                                }
                            }
                            } // end sel_click && !other_sel
                        } else if (!autofill_path_mode && (!group_sel_obj.empty() || !group_sel_spl.empty())) {
                            // Terrain / Traffic / etc.: empty click clears Shift+S group.
                            group_sel_obj.clear();
                            group_sel_spl.clear();
                            group_sel_obj_ids.clear();
                            group_sel_spl_ids.clear();
                            selected_object = -1;
                            selected_spline = -1;
                            panel.set_selection_text("(none)");
                            panel.set_status("Grupo deseleccionado");
                            editor_log::event("SHIFT_S_CLEAR", session_state());
                        }
                        }
                        }
                    }
                }
            }
            lmb_was = lmb;
        }

        editor_subphase_watch.reset();
        // Separate the large per-frame picking/highlight path from generic editor_logic
        // so a freeze report can name this block if a hit-test or scene walk stalls.
        std::optional<omsi::MainThreadWatchdog::Scope> interaction_pick_watch;
        interaction_pick_watch.emplace(main_watchdog.phase("interaction_pick_highlight"));
        if (ev.switch_map && !ev.switch_map_path.empty() && cfg.switch_map_out) {
            *cfg.switch_map_out = ev.switch_map_path;
            log::info("editor switch map -> " + ev.switch_map_path.string());
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Mesh tint highlight (selection/hover/marquee). Disabled: neon green/cyan flash when orbiting
        // was set_chunk_highlight with high G (e.g. marquee 0.25,0.95,1 and helparrow 0.2,0.95,1).
        // Pick/selection logic still runs; only the GPU tint is off.
        {
            auto _watch = main_watchdog.phase("sync_sco_path_edit");
            sync_sco_path_edit();
        }
        {
            auto _watch = main_watchdog.phase("interaction_chunk_highlights");
            renderer.clear_chunk_highlights();
            coop_reapply_tints(); // avatar colours, missing-asset cubes (cleared above every frame)
            constexpr bool kMeshTintHighlight = true; // selection/hover tint restored
            if (kMeshTintHighlight) {
                for (const std::size_t c : marquee.box_chunks)
                    renderer.set_chunk_highlight(c, 0.40f, 1.f, 0.85f, 0.35f); // marquee soft amber (was neon cyan)
                // [helparrow] — Unit128 special=8; cyan cue in editor (Objects tab only).
                if (ev.tool == ui::EditorTool::Object) {
                    const bool cam_drag_hl =
                        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
                        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
                    if (!cam_drag_hl) {
                        // [PERF] Cache help_arrow chunk indices — avoids scanning all scene_places
                        // every frame (was ~1 ms on large maps). Rebuild only when scene size changes.
                        static std::vector<std::size_t> s_help_arrow_chunks;
                        static std::size_t s_help_arrow_gen = 0;
                        if (s_help_arrow_gen != scene_places.size()) {
                            s_help_arrow_chunks.clear();
                            for (std::size_t hi = 0; hi < scene_places.size(); ++hi) {
                                if (scene_places[hi].alive && scene_places[hi].help_arrow)
                                    s_help_arrow_chunks.push_back(hi);
                            }
                            s_help_arrow_gen = scene_places.size();
                        }
                        for (const auto idx : s_help_arrow_chunks) {
                            if (idx < scene_places.size() && scene_places[idx].alive && scene_places[idx].help_arrow)
                                renderer.set_chunk_highlight(scene_places[idx].chunk, 0.30f, 0.35f, 0.85f, 1.f);
                        }
                    }
                }
            }
            const bool object_tab_highlight =
                kMeshTintHighlight && !sco_path_edit.active &&
                (ev.tool == ui::EditorTool::Object || edit_mode == EditMode::AttachPickParent);
            const bool spline_tab_highlight =
                kMeshTintHighlight &&
                (ev.tool == ui::EditorTool::Spline || sco_path_edit.active ||
                 edit_mode == EditMode::AttachPickSpline || edit_mode == EditMode::TrimSpline);
            int hover_object = -1;
            int hover_spline = -1;
            int endpeg_spline = -1;
            int hover_path_rail = -1;
            autofill_hover_marker = -1;
            autofill_hover_path_marker = -1;
            path_align_hover_marker = -1;
            if (!over_ui) {
                float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                input::cursor_to_framebuffer(window, mx, my, fw, fh);
                const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                float wvp_pick[16]{};
                camera.build_world_view_proj(aspect, wvp_pick);
                // [PERF] Object hover pick throttle. pick_object_index runs a full ray/triangle pass
                // over the candidate chunks, yet it was repeated every frame even when neither the
                // cursor nor the pick context changed. Re-pick only when the mouse moved more than
                // 2 px, the tool/edit-mode/hidden-set changed, or every 4th frame; otherwise reuse
                // the previous index (same result, at most 3 frames stale).
                static float s_hov_mx = -1.e9f, s_hov_my = -1.e9f;
                static int s_hov_key = -1;
                static std::size_t s_hov_hid_n = static_cast<std::size_t>(-1);
                static int s_hov_result = -1;
                static std::uint32_t s_hov_age = 0;
                auto pick_object_index_throttled = [&](float pmx, float pmy, float pfw, float pfh,
                                                       const float* pwvp) -> int {
                    const int key = static_cast<int>(ev.tool) * 256 + static_cast<int>(edit_mode);
                    const std::size_t hid_n = panel.imgui_state().hidden_object_ids.size();
                    const bool moved = std::abs(pmx - s_hov_mx) > 2.f || std::abs(pmy - s_hov_my) > 2.f;
                    const bool ctx_changed = key != s_hov_key || hid_n != s_hov_hid_n;
                    if (!moved && !ctx_changed && ++s_hov_age < 4) return s_hov_result;
                    s_hov_age = 0;
                    s_hov_mx = pmx;
                    s_hov_my = pmy;
                    s_hov_key = key;
                    s_hov_hid_n = hid_n;
                    s_hov_result = pick_object_index(pmx, pmy, pfw, pfh, pwvp);
                    return s_hov_result;
                };
                const bool ctrl_held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool cam_drag = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
                                      glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
                // Native path drag tick (G / R / RR): follow the mouse on the ground plane.
                if (sco_path_edit.active &&
                    (sco_path_edit.moving || sco_path_edit.rotating || sco_path_edit.following) && !cam_drag) {
                    if (const auto hit = camera.hit_ground_xy(mx, my, fw, fh)) {
                        // Editor space (same as the loop_03 drag tick); apply_sco_path_hit expects it.
                        apply_sco_path_hit(hit->x, hit->y, hit->z);
                    }
                }
                if (autofill_spline_ends && selected_spline >= 0 && ev.tool == ui::EditorTool::Spline) {
                    autofill_hover_marker = pick_spline_end_marker(mx, my, fw, fh, wvp_pick);
                    if (autofill_hover_marker >= 0 && hand_cursor)
                        glfwSetCursor(window, hand_cursor);
                    else
                        glfwSetCursor(window, nullptr);
                } else if (edit_mode == EditMode::Grab && ctrl_held &&
                           (selected_spline >= 0 || selected_object >= 0)) {
                    path_align_hover_marker =
                        pick_path_marker_index_near_mouse(mx, my, fw, fh, wvp_pick);
                    if (path_align_hover_marker >= 0 && hand_cursor)
                        glfwSetCursor(window, hand_cursor);
                    else
                        glfwSetCursor(window, nullptr);
                } else {
                    glfwSetCursor(window, nullptr);
                    if (edit_mode == EditMode::Mesh) {
                        hover_object = -1;
                        hover_spline = -1;
                        hover_path_rail = -1;
                    } else if (ev.tool == ui::EditorTool::TracksTrips || ev.tool == ui::EditorTool::StationLinks) {
                        extern int g_omsi_tt_mode;
                        extern int g_omsi_sl_mode;
                        const bool tt_p = (ev.tool == ui::EditorTool::TracksTrips && g_omsi_tt_mode == 0) ||
                                          (ev.tool == ui::EditorTool::StationLinks && g_omsi_sl_mode == 2);
                        const bool tt_s = (ev.tool == ui::EditorTool::TracksTrips && g_omsi_tt_mode >= 1) ||
                                          (ev.tool == ui::EditorTool::StationLinks && g_omsi_sl_mode <= 1);
                        if (tt_p && !cam_drag) {
                            hover_path_rail = pick_path_rail(mx, my, fw, fh, wvp_pick, true);
                            if (hover_path_rail >= 0 && hand_cursor) glfwSetCursor(window, hand_cursor);
                        } else if (tt_s && !cam_drag) {
                            const int hb = pick_object_index_throttled(mx, my, fw, fh, wvp_pick);
                            if (hb >= 0 && static_cast<std::size_t>(hb) < edit_objects.size()) {
                                const auto sm = sco::load_sco(edit_objects[static_cast<std::size_t>(hb)].sco_path);
                                if (sm && sm->special == sco::ScoSpecial::Busstop) {
                                    hover_object = hb;
                                    if (hand_cursor) glfwSetCursor(window, hand_cursor);
                                }
                            }
                        }
                    } else if (ev.tool == ui::EditorTool::TrafficRules) {
                        hover_path_rail = pick_path_rail(mx, my, fw, fh, wvp_pick, show_path_rails);
                        if (hover_path_rail >= 0 && hand_cursor) glfwSetCursor(window, hand_cursor);
                    } else if (sco_path_edit.active) {
                        // Native path editor: hover the rails directly.
                        hover_path_rail = pick_path_rail(mx, my, fw, fh, wvp_pick, true);
                        if (hover_path_rail >= 0 && hand_cursor) glfwSetCursor(window, hand_cursor);
                    } else if (edit_mode == EditMode::AttachPickSpline ||
                               (ev.tool == ui::EditorTool::Spline && edit_mode != EditMode::Mesh)) {
                        // Attach-to-spline must hover real [spline]/[spline_h], not SCO attachments.
                        if (!cam_drag)
                            hover_spline = pick_spline_index(mx, my, fw, fh, wvp_pick);
                        if (edit_mode == EditMode::AttachPickSpline && hover_spline >= 0 && hand_cursor)
                            glfwSetCursor(window, hand_cursor);
} else if (edit_mode == EditMode::AttachPickParent ||
                                (ev.tool == ui::EditorTool::Object && edit_mode != EditMode::Mesh &&
                                 !sco_path_edit.active)) {
                        if (!cam_drag)
                            hover_object = pick_object_index_throttled(mx, my, fw, fh, wvp_pick);
                        // The throttled pick can return a cached index up to 3 frames old. While a stream
                        // diff runs (PrepLists) edit_objects is moved into the bg job and is EMPTY, so a stale
                        // index read edit_objects[idx] from a null buffer -> the recurring AV at PrepLists begin.
                        if (hover_object >= 0 && static_cast<std::size_t>(hover_object) >= edit_objects.size())
                            hover_object = -1;
                        if (edit_mode == EditMode::AttachPickParent && hover_object >= 0 && hand_cursor)
                            glfwSetCursor(window, hand_cursor);
                    }
                    // Path rails: hover/pick ONLY in Traffic Rules (or native SCO path editor).
                    // Tile / Object / Spline tools must not highlight or click paths.
                }
                if (!cam_drag && ev.tool == ui::EditorTool::Spline && !sco_path_edit.active &&
                    edit_mode != EditMode::Mesh)
                    endpeg_spline = pick_spline_index(mx, my, fw, fh, wvp_pick);
                if (autofill_path_mode) {
                    autofill_hover_path_marker =
                        pick_path_marker_index_near_mouse(mx, my, fw, fh, wvp_pick);
                    const auto pp = pick_path_endpoint_near_mouse(mx, my, fw, fh, wvp_pick);
                    if ((autofill_hover_path_marker >= 0 || pp.valid) && hand_cursor)
                        glfwSetCursor(window, hand_cursor);
                }
            } else {
                glfwSetCursor(window, nullptr);
            }

            const bool spline_tab = (ev.tool == ui::EditorTool::Spline);
            const bool path_aa = sco_path_edit.aa_mode || (autofill_path_mode && sco_path_edit.active);
            const bool markers_active = (spline_tab || path_aa) && edit_mode != EditMode::Mesh;
            const bool hide_nodes = panel.imgui_state().hide_nodes;
            const bool arrows_3d = panel.imgui_state().arrows_3d && !hide_nodes;
            // (spline id, end) -> highlight kind: 1 shown, 2 hovered, 3 selected
            std::unordered_map<std::int64_t, int> want_arrow3d;
            if (!markers_active) {
                for (const auto& mk : end_markers) {
                    if (mk.alive) renderer.set_chunk_visible(mk.chunk, false);
                }
            }
            std::int32_t hover_spline_id = -1;
            std::int32_t selected_spline_id = -1;
            if (hover_spline >= 0 && static_cast<std::size_t>(hover_spline) < edit_splines.size())
                hover_spline_id = edit_splines[static_cast<std::size_t>(hover_spline)].id;
            if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                selected_spline_id = edit_splines[static_cast<std::size_t>(selected_spline)].id;

            // Only the hovered/selected spline needs end markers. Previously grad mode created
            // markers for EVERY spline (visible all over the map, and O(all splines) per frame).
            if (spline_tab || path_aa) {
                if (spline_tab) {
                    if (hover_spline >= 0 && static_cast<std::size_t>(hover_spline) < edit_splines.size())
                        ensure_spline_end_markers(edit_splines[static_cast<std::size_t>(hover_spline)]);
                    if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                        ensure_spline_end_markers(edit_splines[static_cast<std::size_t>(selected_spline)]);
                }
                sync_all_end_markers();
            }
            if (markers_active) {
            for (const auto& mk : end_markers) {
                if (!mk.alive) continue;
                bool show = false;
                if (!spline_tab && !path_aa) {
                    show = false;
                } else if (mk.is_path) {
                    show = show_path_ends || autofill_path_mode || path_aa ||
                           (hover_spline_id >= 0 && mk.owner_id == hover_spline_id) ||
                           (selected_spline_id >= 0 && mk.owner_id == selected_spline_id) ||
                           (sco_path_edit.active && mk.from_object && mk.owner_id == sco_path_edit.object_id);
                } else if (mk.from_object) {
                    show = path_aa && mk.owner_id == sco_path_edit.object_id;
                } else {
                    // Spline end nodes: only in the Spline tab, and only for the spline under the
                    // cursor or the selected one — never for every spline / prev / next ids.
                    const bool hovered = hover_spline_id >= 0 && mk.owner_id == hover_spline_id;
                    const bool selected = selected_spline_id >= 0 && mk.owner_id == selected_spline_id;
                    show = spline_tab && (hovered || selected);
                }
                // Vista > Ocultar nodos (the autofill AA ball of the selected spline stays)
                if (hide_nodes && !(autofill_spline_ends && !mk.from_object && selected_spline_id >= 0 &&
                                    mk.owner_id == selected_spline_id))
                    show = false;
                if (arrows_3d && show && !mk.from_object) {
                    // path sphere + spline arrow of this end -> one 3D arrow
                    const bool sel = selected_spline_id >= 0 && mk.owner_id == selected_spline_id;
                    const bool hov = hover_spline_id >= 0 && mk.owner_id == hover_spline_id;
                    int& kind = want_arrow3d[arrow3d_key(mk.owner_id, mk.is_start)];
                    kind = (std::max)(kind, sel ? 3 : hov ? 2 : 1);
                    show = false;
                }
                renderer.set_chunk_visible(mk.chunk, show);
                if (!show) continue;
                if (mk.is_path) {
                    const bool sel = selected_spline_id >= 0 && mk.owner_id == selected_spline_id;
                    const bool hov = hover_spline_id >= 0 && mk.owner_id == hover_spline_id;
                    if (sel)
                        renderer.set_chunk_highlight(mk.chunk, 1.f, 0.2f, 0.45f, 1.f);
                    else if (hov)
                        renderer.set_chunk_highlight(mk.chunk, 0.85f, 0.45f, 0.95f, 0.95f);
                    else
                        renderer.set_chunk_highlight(mk.chunk, 0.55f, 0.75f, 0.95f, 0.85f);
                } else {
                    float ax = 0.f, ay = 0.f, az = 0.f, heading = 0.f;
                    if (!mk.from_object) {
                        for (const auto& s : edit_splines) {
                            if (s.id != mk.owner_id) continue;
                            const auto samp =
                                spline::evaluate_spline(s, mk.is_start ? 0.f : s.length);
                            ax = samp.x;
                            ay = samp.y;
                            az = samp.z + kArrowFloatZ;
                            heading = samp.heading_rad;
                            break;
                        }
                        // Direction arrow: fixed ~1% of screen (scales with camera.radius).
                        // world_size ≈ pct * 1.15 * orbit_radius  (FOV~60° approximation)
                        float arrow_scl = camera.radius * 0.0115f;
                        if (arrow_scl < 0.12f) arrow_scl = 0.12f;
                        if (arrow_scl > 30.f) arrow_scl = 30.f;
                        const auto wt = store_world_transposed(
                            arrow_world_matrix(ax, ay, az, heading, arrow_scl));
                        renderer.set_chunk_world_matrix(mk.chunk, wt.data());
                    }
                    const float lum = sample_ground_luminance(ax, ay);
                    const bool light_bg = lum > 0.52f;
                    renderer.set_chunk_highlight(mk.chunk, 0.95f, light_bg ? 0.08f : 1.f, light_bg ? 0.08f : 1.f,
                                                 light_bg ? 0.08f : 1.f);
                }
            }
            }
            // Vista > Flechas 3D: place / hide the translucent arrows.
            {
                std::unordered_set<std::size_t> arrow3d_on;
                for (const auto& [key, kind] : want_arrow3d) {
                    const std::int32_t owner = static_cast<std::int32_t>(key >> 1);
                    const bool is_start = (key & 1) != 0;
                    const map::SplineInst* sp = nullptr;
                    for (const auto& s : edit_splines)
                        if (s.id == owner) {
                            sp = &s;
                            break;
                        }
                    if (!sp) continue;
                    const std::size_t ch = arrow3d_chunk(owner, is_start);
                    if (ch == static_cast<std::size_t>(-1)) continue;
                    const auto samp = spline::evaluate_spline(*sp, is_start ? 0.f : sp->length);
                    // ~2 % of the view, between 1.5 m and 40 m long; pulled back half its length at the
                    // start / out at the end so it straddles the node without hiding it
                    const float len = std::clamp(camera.radius * 0.03f, 1.5f, 40.f);
                    const float hx = std::sin(samp.heading_rad), hy = std::cos(samp.heading_rad);
                    const float off = is_start ? -0.15f * len : -0.85f * len;
                    const auto wt = store_world_transposed(arrow_world_matrix(
                        samp.x + hx * off, samp.y + hy * off, samp.z + 0.6f, samp.heading_rad, len));
                    renderer.set_chunk_world_matrix(ch, wt.data());
                    if (kind == 3) renderer.set_chunk_highlight(ch, 1.f, 1.f, 0.25f, 0.6f);
                    else if (kind == 2) renderer.set_chunk_highlight(ch, 1.f, 0.85f, 0.5f, 1.f);
                    else renderer.set_chunk_highlight(ch, 1.f, 0.3f, 0.85f, 1.f);
                    renderer.set_chunk_opacity(ch, kind >= 2 ? 0.7f : 0.5f);
                    renderer.set_chunk_visible(ch, true);
                    arrow3d_on.insert(ch);
                }
                for (const auto& [key, ch] : arrow3d_chunks)
                    if (!arrow3d_on.count(ch)) renderer.set_chunk_visible(ch, false);
            }
            // Path helpers: classic keeps surface_bias (original). Modern depth-tests (no always-on-top).
            // Hover / rule tint / pick only in Traffic Rules (or native SCO path editor).
            const bool in_traffic = (ev.tool == ui::EditorTool::TrafficRules);
            const bool tt_hover_tool = (ev.tool == ui::EditorTool::TracksTrips ||
                                        ev.tool == ui::EditorTool::StationLinks);
            if (!in_traffic && !sco_path_edit.active && !tt_hover_tool)
                hover_path_rail = -1; // never yellow-hover paths outside Traffic / Tracks / Stn links
            // OMSI: path helpers also visible while Tracks&Trips / StationLinks edit modes
            // (Unit121 modes 0x14/0x17/0x18 and stnlink range) so route membership is visible.
            extern int g_omsi_tt_mode;
            extern int g_omsi_sl_mode;
            // g_omsi_tt_mode is 0 by default, so it must NOT be tested on its own: that made the TT
            // colours (red/yellow members) show on every tab. Only the two TT tools count.
            const bool tt_path_vis =
                (panel.current_tool() == ui::EditorTool::StationLinks) ||
                (panel.current_tool() == ui::EditorTool::TracksTrips);
            const bool tt_paths_mode =
                (panel.current_tool() == ui::EditorTool::TracksTrips && g_omsi_tt_mode == 0) ||
                (panel.current_tool() == ui::EditorTool::StationLinks && g_omsi_sl_mode == 2);
            const bool rails_vis =
                (in_traffic || autofill_path_mode || sco_path_edit.active || show_path_rails ||
                 tt_path_vis) &&
                edit_mode != EditMode::Mesh;
            static bool rails_were_vis = false;
            if (!rails_vis && rails_were_vis) {
                for (const auto& rail : path_rails) {
                    if (rail.alive) {
                        renderer.set_chunk_visible(rail.chunk, false);
                        renderer.set_chunk_highlight(rail.chunk, 0.f, 1.f, 1.f, 1.f);
                        renderer.set_chunk_wireframe(rail.chunk, false);
                    }
                }
            }
            rails_were_vis = rails_vis;
            if (rails_vis) {
            int sco_active_rail = selected_path_rail;
            if (sco_path_edit.active) {
                sco_active_rail = -1;
                const int ap = sco_path_edit.sel;
                if (ap >= 0) {
                    for (std::size_t ri = 0; ri < path_rails.size(); ++ri) {
                        const auto& rail = path_rails[ri];
                        if (rail.alive && rail.from_object && rail.owner_id == sco_path_edit.object_id &&
                            rail.path_index == ap) {
                            sco_active_rail = static_cast<int>(ri);
                            break;
                        }
                    }
                }
            }
            // Classic (path_style==0): textured helper + surface_bias. Rule colors MULTIPLY/lerp
            // the texture (amount ~0.55) so the helper_path bitmap stays visible.
            // Modern (path_style==1): same 3D rail mesh (GPU depth = free occlusion), solid
            // kKind / rule colors (amount 1). No 2D world overlay.
            const bool modern_paths = panel.imgui_state().path_style == 1;
            // Rebuild path-rail meshes when switching classic ↔ modern (width differs).
            {
                static int last_path_style = -1;
                const int cur_style = panel.imgui_state().path_style;
                if (last_path_style >= 0 && last_path_style != cur_style) {
                    path_rails_rebuild = true;
                    // Clear live rails so they are re-uploaded with the new strip width.
                    for (auto& rail : path_rails) {
                        if (!rail.alive) continue;
                        renderer.set_chunk_visible(rail.chunk, false);
                        path_rails_free_chunks.push_back(rail.chunk);
                        rail.alive = false;
                    }
                    path_rails.clear();
                    path_rails_kick_cpu();
                }
                last_path_style = cur_style;
            }
            // Modern base colors (distinct from rule tints: blues / black / turquoise / green / yellow).
            static const float kKindRGB[4][3] = {
                {0.72f, 0.28f, 0.95f}, // cars  → purple/magenta
                {0.55f, 0.95f, 0.35f}, // peds  → lime
                {0.95f, 0.35f, 0.70f}, // rail  → pink
                {0.20f, 0.95f, 0.75f}, // air   → mint
            };
            // Classic: multiply wash (shader col *= lerp(1,tint,amt)). amt < 0.99 keeps texture.
            constexpr float kClassicRuleAmt = 0.90f;
            constexpr float kClassicBlackAmt = 0.95f;
            constexpr float kClassicHoverAmt = 0.85f;
            poll_traffic_chain();
            {
                static int last_chain_hover = -1;
                static bool last_chain_ctrl = false;
                // Tracks & Trips: the anchor of the Ctrl-chain is always the selected (last clicked) track entry,
                // i.e. "path 1" of the route to complete, even when it was picked from the list or in another frame.
                if (tt_paths_mode && panel.current_tool() == ui::EditorTool::TracksTrips) {
                    auto& aman = omsi::tt::global_tt_man();
                    if (aman.loaded && aman.selected_track >= 0 &&
                        aman.selected_track < static_cast<int>(aman.data.tracks.size())) {
                        const auto& atr = aman.data.tracks[static_cast<std::size_t>(aman.selected_track)];
                        int ei = aman.selected_track_entry;
                        if (ei < 0 || ei >= static_cast<int>(atr.entries.size())) ei = static_cast<int>(atr.entries.size()) - 1;
                        int found = -1;
                        if (ei >= 0) {
                            const auto& ae = atr.entries[static_cast<std::size_t>(ei)];
                            for (std::size_t ri = 0; ri < path_rails.size(); ++ri) {
                                const auto& r = path_rails[ri];
                                if (r.alive && r.owner_id == ae.id && r.path_index == ae.path_index) { found = static_cast<int>(ri); break; }
                            }
                        }
                        traffic_anchor_rail = found;
                    } else {
                        traffic_anchor_rail = -1;
                    }
                }
                const bool ctrl_chain = (in_traffic || tt_paths_mode) && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
                if (ctrl_chain && traffic_anchor_rail >= 0 && hover_path_rail >= 0) {
                    if (hover_path_rail != last_chain_hover || !last_chain_ctrl)
                        kick_traffic_chain_async(traffic_anchor_rail, hover_path_rail);
                } else if (!ctrl_chain && last_chain_ctrl) {
                    traffic_chain_rails.clear();
                }
                last_chain_hover = hover_path_rail;
                last_chain_ctrl = ctrl_chain;
            }
            // TT / StnLink membership colors (Unit121 helper selected-byte +0x2dc):
            //   0 = clear (SelectAllHelper 0)
            //   1 = HovPath hover candidate
            //   2 = member of selected track / stnlink (all entries)
            //   3 = the selected trackentry / stnlinkentry
            // Applied only while TT modes or StationLinks tool are active — not in pure traffic.
            auto& tt_man = omsi::tt::global_tt_man();
            const bool tt_color_active = tt_path_vis && tt_man.loaded;
            // (The old Win32 file-list sync lived here; the ImGui combos now own the selection.)
            // Build membership set for O(1) lookup: (owner_id << 16) ^ path_index
            std::unordered_set<std::uint64_t> tt_track_keys;
            std::uint64_t tt_sel_entry_key = 0;
            bool tt_have_sel_entry = false;
            std::unordered_set<std::uint64_t> tt_stn_keys;
            std::uint64_t tt_sel_stn_key = 0;
            bool tt_have_sel_stn = false;
            auto make_pk = [](std::int32_t oid, std::int32_t pi) -> std::uint64_t {
                return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(oid)) << 32) |
                       static_cast<std::uint32_t>(pi);
            };
            if (tt_color_active) {
                if (tt_man.selected_track >= 0 &&
                    tt_man.selected_track < static_cast<int>(tt_man.data.tracks.size())) {
                    const auto& tr = tt_man.data.tracks[static_cast<std::size_t>(tt_man.selected_track)];
                    for (int ei = 0; ei < static_cast<int>(tr.entries.size()); ++ei) {
                        const auto& e = tr.entries[static_cast<std::size_t>(ei)];
                        const auto k = make_pk(e.id, e.path_index);
                        tt_track_keys.insert(k);
                        if (ei == tt_man.selected_track_entry) {
                            tt_sel_entry_key = k;
                            tt_have_sel_entry = true;
                        }
                    }
                }
                if (panel.current_tool() == ui::EditorTool::StationLinks &&
                    tt_man.selected_stnlink >= 0 &&
                    tt_man.selected_stnlink < static_cast<int>(tt_man.data.stn_links.size())) {
                    const auto& L = tt_man.data.stn_links[static_cast<std::size_t>(tt_man.selected_stnlink)];
                    for (int ei = 0; ei < static_cast<int>(L.entries.size()); ++ei) {
                        const auto& e = L.entries[static_cast<std::size_t>(ei)];
                        const auto k = make_pk(e.id, e.path_index);
                        tt_stn_keys.insert(k);
                        if (ei == tt_man.selected_stnlink_entry) {
                            tt_sel_stn_key = k;
                            tt_have_sel_stn = true;
                        }
                    }
                }
            }

            for (std::size_t ri = 0; ri < path_rails.size(); ++ri) {
                const auto& rail = path_rails[ri];
                if (!rail.alive) continue;
                const bool rail_sel = sco_path_edit.active && rail.from_object &&
                                      rail.owner_id == sco_path_edit.object_id &&
                                      sco_path_edit.is_selected(rail.path_index);
                const bool rail_hidden = sco_path_edit.active &&
                                         panel.imgui_state().sco_path_hide_visual &&
                                         rail.from_object && rail.owner_id == sco_path_edit.object_id &&
                                         !rail_sel;
                const bool rail_show = !rail_hidden &&
                                       path_rail_in_visual_radius(rail.owner_id, rail.from_object);
                renderer.set_chunk_visible(rail.chunk, rail_show);
                if (!rail_show) {
                    renderer.set_chunk_highlight(rail.chunk, 0.f, 1.f, 1.f, 1.f);
                    renderer.set_chunk_wireframe(rail.chunk, false);
                    continue;
                }
                renderer.set_chunk_wireframe(rail.chunk, false);
                renderer.set_chunk_render_phase(rail.chunk, omsi::gfx::dx12::WorldRenderPhase::Normal,
                                               /*surface_bias=*/true, /*decal=*/false);
                const bool traffic_paint = in_traffic || sco_path_edit.active;
                const auto tint = rail_tint_for(rail.from_object, rail.owner_id, rail.path_index, rail.path_typ);
                const bool hovered = traffic_paint && (hover_path_rail == static_cast<int>(ri));
                const bool in_chain = traffic_paint && rail_in_traffic_chain(static_cast<int>(ri));
                const int kk = (rail.path_typ >= 0 && rail.path_typ < 4) ? rail.path_typ : 0;
                float br = 1.f, bg = 1.f, bb = 0.45f;
                const bool have_brush = traffic_paint && brush_preview_rgb(br, bg, bb);
                const bool hover_or_chain = (hovered || in_chain) && have_brush;

                // TT membership (takes priority over neutral base, below traffic brush/hover)
                int tt_state = 0; // 0 none, 2 member, 3 selected entry
                if (tt_color_active) {
                    const auto pk = make_pk(rail.owner_id, rail.path_index);
                    if (tt_have_sel_entry && pk == tt_sel_entry_key) tt_state = 3;
                    else if (tt_track_keys.count(pk)) tt_state = 2;
                    if (tt_have_sel_stn && pk == tt_sel_stn_key) tt_state = 3;
                    else if (tt_state == 0 && tt_stn_keys.count(pk)) tt_state = 2;
                }

                const bool tt_hov = tt_color_active && tt_paths_mode &&
                                    (hover_path_rail == static_cast<int>(ri) ||
                                     rail_in_traffic_chain(static_cast<int>(ri)));
                if (tt_hov) {
                    // Hover candidate (and the Ctrl-chain preview): yellow
                    renderer.set_chunk_highlight(rail.chunk, modern_paths ? 1.f : 0.92f, 1.0f, 0.9f, 0.0f);
                } else if (tt_state > 0 && !hover_or_chain && !(traffic_paint && tint[3] >= 0.01f)) {
                    // Exact RGB from Unit84 TSplineSegment_RenderSpline (selected:byte & 0xF):
                    //   1 hover:  R=0 G=0.5 B=1
                    //   2 member: R=1 G=0   B=0   (track/stnlink membership)
                    //   3 sel:    R=1 G=0.9 B=0   (0x3f666666 ≈ 0.9)
                    // Channel order confirmed: local_c=R, local_10=G, local_14=B
                    float tr, tg, tb, amt;
                    if (tt_state == 3) {
                        tr = 1.0f; tg = 0.30f; tb = 0.05f; // state 3 selected entry (bright red-orange)
                        amt = modern_paths ? 1.f : 0.92f;
                    } else {
                        // state 2 — same red for track members AND stnlink members
                        // (OMSI uses selected=2 for both tracks and stnlink path segments)
                        tr = 0.85f; tg = 0.02f; tb = 0.02f; // members: red
                        amt = modern_paths ? 1.f : 0.88f;
                    }
                    renderer.set_chunk_highlight(rail.chunk, amt, tr, tg, tb);
                } else if (modern_paths) {
                    if (hover_or_chain)
                        renderer.set_chunk_highlight(rail.chunk, 1.f, br, bg, bb);
                    else if (traffic_paint && tint[3] >= 0.01f)
                        renderer.set_chunk_highlight(rail.chunk, 1.f, tint[0], tint[1], tint[2]);
                    else
                        renderer.set_chunk_highlight(rail.chunk, 1.f, kKindRGB[kk][0], kKindRGB[kk][1],
                                                    kKindRGB[kk][2]);
                } else {
                    if (!traffic_paint && tt_state == 0) {
                        renderer.set_chunk_highlight(rail.chunk, 0.f, 1.f, 1.f, 1.f);
                    } else if (hover_or_chain) {
                        renderer.set_chunk_highlight(rail.chunk, kClassicHoverAmt, br, bg, bb);
                    } else if (tint[3] >= 0.01f) {
                        const float lum = 0.2126f * tint[0] + 0.7152f * tint[1] + 0.0722f * tint[2];
                        const float amt = (lum < 0.12f) ? kClassicBlackAmt : kClassicRuleAmt;
                        renderer.set_chunk_highlight(rail.chunk, amt, tint[0], tint[1], tint[2]);
                    } else {
                        renderer.set_chunk_highlight(rail.chunk, 0.f, 1.f, 1.f, 1.f);
                    }
                }
                (void)rail_sel;
                (void)sco_active_rail;
            }
            // Modern endpoint NODES as 3D spheres (same depth/surface_bias as rails — not 2D overlay).
            if (modern_paths && rails_vis) {
                static std::vector<std::size_t> modern_node_chunks;
                static bool node_pool_init = false;
                if (!node_pool_init && !path_mark_verts.empty()) {
                    constexpr int kPool = 256;
                    modern_node_chunks.reserve(static_cast<std::size_t>(kPool));
                    for (int i = 0; i < kPool; ++i) {
                        if (!renderer.append_textured_mesh(
                                path_mark_verts.data(), static_cast<std::uint32_t>(path_mark_verts.size()),
                                path_mark_inds.data(), static_cast<std::uint32_t>(path_mark_inds.size()), {},
                                0u))
                            break;
                        const std::size_t ch = renderer.chunk_count() - 1;
                        renderer.set_chunk_visible(ch, false);
                        renderer.set_chunk_fixed_lit(ch, true);
                        renderer.set_chunk_render_phase(ch, omsi::gfx::dx12::WorldRenderPhase::OnSurface,
                                                       /*surface_bias=*/true, /*decal=*/true);
                        modern_node_chunks.push_back(ch);
                    }
                    node_pool_init = true;
                }
                for (std::size_t ch : modern_node_chunks)
                    renderer.set_chunk_visible(ch, false);
                std::size_t ni = 0;
                auto place_node = [&](float x, float y, float z, float r, float g, float b, bool start) {
                    if (ni >= modern_node_chunks.size()) return;
                    const std::size_t ch = modern_node_chunks[ni++];
                    const float sc = start ? 0.11f : 0.09f;
                    const XMMATRIX world =
                        DirectX::XMMatrixMultiply(DirectX::XMMatrixScaling(sc, sc, sc),
                                                 DirectX::XMMatrixTranslation(x, y, z + 0.04f));
                    const auto wt = store_world_transposed(world);
                    renderer.set_chunk_world_matrix(ch, wt.data());
                    renderer.set_chunk_highlight(ch, 1.f, r, g, b);
                    renderer.set_chunk_visible(ch, true);
                };
                for (std::size_t ri = 0; ri < path_rails.size(); ++ri) {
                    const auto& rail = path_rails[ri];
                    if (!rail.alive) continue;
                    if (!path_rail_in_visual_radius(rail.owner_id, rail.from_object)) continue;
                    float sx, sy, sz, ex, ey, ez;
                    if (!path_rail_endpoints(static_cast<int>(ri), sx, sy, sz, ex, ey, ez)) continue;
                    const auto tint =
                        rail_tint_for(rail.from_object, rail.owner_id, rail.path_index, rail.path_typ);
                    const int kk = (rail.path_typ >= 0 && rail.path_typ < 4) ? rail.path_typ : 0;
                    float cr = kKindRGB[kk][0], cg = kKindRGB[kk][1], cb = kKindRGB[kk][2];
                    if (in_traffic && tint[3] >= 0.01f) {
                        cr = tint[0];
                        cg = tint[1];
                        cb = tint[2];
                    }
                    float br, bg, bb;
                    if (in_traffic && (hover_path_rail == static_cast<int>(ri) ||
                                      rail_in_traffic_chain(static_cast<int>(ri))) &&
                        brush_preview_rgb(br, bg, bb)) {
                        cr = br;
                        cg = bg;
                        cb = bb;
                    }
                    place_node(sx, sy, sz, cr, cg, cb, true);
                    place_node(ex, ey, ez, cr, cg, cb, false);
                }
            }
            }
            // Traffic rules inspector (bottom-right): hover or Alt-selected path.
            if (in_traffic) {
                const int panel_rail =
                    (selected_path_rail >= 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0 &&
                     hover_path_rail < 0)
                        ? selected_path_rail
                        : (hover_path_rail >= 0 ? hover_path_rail
                                               : (selected_path_rail >= 0 ? selected_path_rail : -1));
                // Prefer live hover; keep sticky selection when not hovering.
                const int show_rail = (hover_path_rail >= 0) ? hover_path_rail : selected_path_rail;
                if (show_rail >= 0 && static_cast<std::size_t>(show_rail) < path_rails.size()) {
                    const auto& pr = path_rails[static_cast<std::size_t>(show_rail)];
                    if (pr.alive) {
                        auto* rules = path_rules_for(pr.from_object, pr.owner_id);
                        ImGui::SetNextWindowPos(
                            ImVec2(ImGui::GetIO().DisplaySize.x - 12.f, ImGui::GetIO().DisplaySize.y - 12.f),
                            ImGuiCond_Always, ImVec2(1.f, 1.f));
                        ImGui::SetNextWindowSize(ImVec2(340.f, 0.f), ImGuiCond_Always);
                        ImGui::SetNextWindowBgAlpha(0.92f);
                        if (ImGui::Begin("##path_rules_hover", nullptr,
                                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                             ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav)) {
                            ImGui::Text("path id: %d, %s id: %d", pr.path_index,
                                        pr.from_object ? ".sco" : ".sli", pr.owner_id);
                            ImGui::Separator();
                            if (!rules) {
                                ImGui::TextDisabled("(sin rules)");
                            } else {
                                // Collect indices for this path_index only
                                std::vector<std::size_t> idxs;
                                for (std::size_t i = 0; i < rules->size(); ++i)
                                    if ((*rules)[i].path_index == pr.path_index) idxs.push_back(i);
                                int remove_i = -1;
                                for (std::size_t n = 0; n < idxs.size(); ++n) {
                                    const std::size_t ri = idxs[n];
                                    auto& r = (*rules)[ri];
                                    ImGui::PushID(static_cast<int>(ri));
                                    // Drag handle
                                    ImGui::Selectable("::", false, ImGuiSelectableFlags_None,
                                                      ImVec2(18.f, 0.f));
                                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                        ImGui::SetDragDropPayload("PATH_RULE_ORD", &ri, sizeof(ri));
                                        ImGui::Text("%s", r.keyword.c_str());
                                        ImGui::EndDragDropSource();
                                    }
                                    if (ImGui::BeginDragDropTarget()) {
                                        if (const ImGuiPayload* pl =
                                                ImGui::AcceptDragDropPayload("PATH_RULE_ORD")) {
                                            const std::size_t from =
                                                *static_cast<const std::size_t*>(pl->Data);
                                            if (from != ri && from < rules->size()) {
                                                auto tmp = (*rules)[from];
                                                rules->erase(rules->begin() +
                                                             static_cast<std::ptrdiff_t>(from));
                                                const std::size_t to =
                                                    (from < ri) ? (ri - 1) : ri;
                                                rules->insert(rules->begin() +
                                                                  static_cast<std::ptrdiff_t>(to),
                                                              std::move(tmp));
                                                if (pr.from_object) {
                                                    for (const auto& o : edit_objects)
                                                        if (o.id == pr.owner_id) {
                                                            persist_editor_xy(o.position.x, o.position.y);
                                                            break;
                                                        }
                                                } else {
                                                    for (const auto& s : edit_splines)
                                                        if (s.id == pr.owner_id) {
                                                            persist_editor_xy(s.position.x, s.position.y);
                                                            break;
                                                        }
                                                }
                                            }
                                        }
                                        ImGui::EndDragDropTarget();
                                    }
                                    ImGui::SameLine();
                                    // UVG group index + name (unsched_vehgroups.txt order = OMSI TPathRule density arrays)
                                    {
                                        const auto& uvg = panel.imgui_state().uvg_groups;
                                        char gbuf[96];
                                        if (rule_ieq(r.keyword, "trafficdensity") &&
                                            r.extra >= 0 && static_cast<std::size_t>(r.extra) < uvg.size())
                                            std::snprintf(gbuf, sizeof(gbuf), "(%d) %s", r.extra,
                                                          uvg[static_cast<std::size_t>(r.extra)].c_str());
                                        else
                                            std::snprintf(gbuf, sizeof(gbuf), "(%d)", r.extra);
                                        ImGui::TextUnformatted(gbuf);
                                    }
                                    ImGui::SameLine();
                                    // Label (Spanish + value) — matches OMSI SetRule keywords
                                    const char* label = r.keyword.c_str();
                                    char buf[128];
                                    if (rule_ieq(r.keyword, "trafficdensity")) {
                                        if (r.kill || r.value <= 1.e-4f)
                                            std::snprintf(buf, sizeof(buf), "no_programado");
                                        else
                                            std::snprintf(buf, sizeof(buf), "densidad %.3g", r.value);
                                        label = buf;
                                    } else if (rule_ieq(r.keyword, "overtaking_prohib"))
                                        label = "no_adelantar";
                                    else if (rule_ieq(r.keyword, "no_cars"))
                                        label = "no_autos";
                                    else if (rule_ieq(r.keyword, "speedlimit")) {
                                        std::snprintf(buf, sizeof(buf), "velocidad %.0f", r.value);
                                        label = buf;
                                    } else if (rule_ieq(r.keyword, "priority")) {
                                        std::snprintf(buf, sizeof(buf), "prioridad %.0f", r.value);
                                        label = buf;
                                    } else if (rule_ieq(r.keyword, "buses"))
                                        label = "buses";
                                    else if (rule_ieq(r.keyword, "trucks"))
                                        label = "trucks";
                                    ImGui::TextUnformatted(label);
                                    // Edit speed (pencil) — only for speedlimit
                                    if (rule_ieq(r.keyword, "speedlimit") && !r.kill) {
                                        ImGui::SameLine();
                                        if (ImGui::SmallButton("e")) {
                                            ImGui::OpenPopup("edit_speed");
                                        }
                                        if (ImGui::BeginPopup("edit_speed")) {
                                            ImGui::SetNextItemWidth(100.f);
                                            float sp = r.value;
                                            if (ImGui::InputFloat("km/h", &sp, 5.f, 10.f, "%.0f")) {
                                                if (sp < 0.f) sp = 0.f;
                                                if (sp > 200.f) sp = 200.f;
                                                r.value = sp;
                                                if (pr.from_object) {
                                                    for (const auto& o : edit_objects)
                                                        if (o.id == pr.owner_id) {
                                                            persist_editor_xy(o.position.x, o.position.y);
                                                            break;
                                                        }
                                                } else {
                                                    for (const auto& s : edit_splines)
                                                        if (s.id == pr.owner_id) {
                                                            persist_editor_xy(s.position.x, s.position.y);
                                                            break;
                                                        }
                                                }
                                            }
                                            ImGui::EndPopup();
                                        }
                                    }
                                    // Delete
                                    ImGui::SameLine();
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 0.9f));
                                    if (ImGui::SmallButton("x")) remove_i = static_cast<int>(ri);
                                    ImGui::PopStyleColor();
                                    ImGui::PopID();
                                }
                                if (remove_i >= 0 && static_cast<std::size_t>(remove_i) < rules->size()) {
                                    rules->erase(rules->begin() + remove_i);
                                    if (pr.from_object) {
                                        for (const auto& o : edit_objects)
                                            if (o.id == pr.owner_id) {
                                                persist_editor_xy(o.position.x, o.position.y);
                                                break;
                                            }
                                    } else {
                                        for (const auto& s : edit_splines)
                                            if (s.id == pr.owner_id) {
                                                persist_editor_xy(s.position.x, s.position.y);
                                                break;
                                            }
                                    }
                                    for (auto& rail : path_rails) {
                                        if (!rail.alive) continue;
                                        if (rail.owner_id == pr.owner_id &&
                                            rail.from_object == pr.from_object &&
                                            rail.path_index == pr.path_index)
                                            tint_path_rail_chunk(rail);
                                    }
                                }
                            }
                            ImGui::TextDisabled("Alt+click = seleccionar | Shift = borrar brush");
                        }
                        ImGui::End();
                    }
                }
            }
            for (const auto& e : entry_chunks)
                if (e.visible) renderer.set_chunk_highlight(e.chunk, kSelAmt, kSelR, kSelG, kSelB);
            if (autofill_hover_marker >= 0) {
                const auto& mk = end_markers[static_cast<std::size_t>(autofill_hover_marker)];
                renderer.set_chunk_highlight(mk.chunk, 1.f, 0.25f, 1.f, 0.35f); // magenta finger target
            }
            if (autofill_hover_path_marker >= 0) {
                const auto& mk = end_markers[static_cast<std::size_t>(autofill_hover_path_marker)];
                renderer.set_chunk_highlight(mk.chunk, 1.f, 0.35f, 1.f, 0.55f); // cyan path target
            }
            if (path_align_hover_marker >= 0 && edit_mode == EditMode::Grab) {
                const auto& mk = end_markers[static_cast<std::size_t>(path_align_hover_marker)];
                renderer.set_chunk_highlight(mk.chunk, 0.2f, 1.f, 0.45f, 0.4f);
            }
            if (ev.tool == ui::EditorTool::Tile) {
                const auto tile_t0 = std::chrono::steady_clock::now();
                rebuild_neighbor_wireframes();
                const auto cam_tile = centerkachel_as_active();
                std::int32_t hover_gx = -1;
                std::int32_t hover_gy = -1;
                if (!over_ui) {
                    float tmx = 0.f, tmy = 0.f, tfw = 1.f, tfh = 1.f;
                    input::cursor_to_framebuffer(window, tmx, tmy, tfw, tfh);
                    if (const auto hit = camera.hit_ground_xy(tmx, tmy, tfw, tfh)) {
                        const auto [wx, wy] = editor_to_world_xy(hit->x, hit->y);
                        const auto hover_ck =
                            map::centerkachel_from_world(cfg.global, world.tile_layout(), wx, wy);
                        if (hover_ck.valid) {
                            hover_gx = hover_ck.gx;
                            hover_gy = hover_ck.gy;
                        }
                    }
                }
                constexpr float kEmptyFrameAmt = 0.18f;
                for (const auto& slot : tile_frame_empty_pool) {
                    if (slot.chunk == static_cast<std::size_t>(-1)) continue;
                    const bool is_cam =
                        cam_tile.resolved && slot.tile_gx == cam_tile.grid_x && slot.tile_gy == cam_tile.grid_y;
                    const bool is_hover = slot.tile_gx == hover_gx && slot.tile_gy == hover_gy;
                    if (is_cam || is_hover) continue;
                    renderer.set_chunk_wireframe(slot.chunk, true);
                    renderer.set_chunk_visible(slot.chunk, true);
                    renderer.set_chunk_highlight(slot.chunk, kEmptyFrameAmt, 0.42f, 0.48f, 0.55f);
                }
                const bool hover_is_cam =
                    hover_gx == cam_tile.grid_x && hover_gy == cam_tile.grid_y;
                if (cam_tile.resolved) {
                    if (hover_is_cam)
                        set_tile_cell_highlight(cam_tile.grid_x, cam_tile.grid_y, 0.45f, 1.f, 0.95f, 0.55f);
                    else
                        set_tile_cell_highlight(cam_tile.grid_x, cam_tile.grid_y, 0.30f, 0.15f, 0.85f, 0.45f);
                }
                if (hover_gx >= 0 && hover_gy >= 0 && !hover_is_cam)
                    set_tile_cell_highlight(hover_gx, hover_gy, kHoverAmt, kHoverR, kHoverG, kHoverB);
                if (tile_dbg.enabled) {
                    ++tile_dbg.tile_tab_frames;
                    tile_dbg.tile_tab_ns += std::chrono::steady_clock::now() - tile_t0;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - tile_dbg.last_report >= std::chrono::seconds(1)) {
                        const double avg_ms =
                            tile_dbg.tile_tab_frames > 0
                                ? std::chrono::duration<double, std::milli>(tile_dbg.tile_tab_ns).count() /
                                      static_cast<double>(tile_dbg.tile_tab_frames)
                                : 0.0;
                        log::info("TILE_DEBUG summary frames=" + std::to_string(tile_dbg.tile_tab_frames) +
                                  " avg_tile_tab_ms=" + std::to_string(avg_ms) + " wf_rebuilds=" +
                                  std::to_string(tile_dbg.wireframe_rebuilds) + " wf_fail=" +
                                  std::to_string(tile_dbg.wireframe_upsert_fail) + " wf_pool=" +
                                  std::to_string(tile_frame_empty_pool.size()) + " center=(" +
                                  std::to_string(centerkachel.gx) + "," + std::to_string(centerkachel.gy) + ")" +
                                  " valid=" + std::to_string(centerkachel.valid));
                        tile_dbg.tile_tab_frames = 0;
                        tile_dbg.tile_tab_ns = {};
                        tile_dbg.last_report = now;
                    }
                }
            } else {
                hide_tile_frame_slots();
            }
            if (edit_mode != EditMode::AttachPickSpline && hover_object >= 0 && object_tab_highlight) {
                if (const auto ei =
                        spline_attach_entry_for_object(edit_objects[static_cast<std::size_t>(hover_object)].id)) {
                    highlight_spline_attach_group(*ei, kHoverAmt, kHoverR, kHoverG, kHoverB);
                    highlight_spline_next_chain(edit_spline_attachments[*ei].parent_spline_id, 0.5f, 0.2f, 0.95f,
                                                1.f);
                    if (parked_hot_hook)
                        for (const auto goid : spline_attach_group_object_ids(*ei))
                            parked_hot_hook(goid, kHoverAmt, kHoverR, kHoverG, kHoverB);
                } else {
                    const auto oid = edit_objects[static_cast<std::size_t>(hover_object)].id;
                    for (const auto& sp : scene_places) {
                        if (sp.alive && sp.object_id == oid)
                            renderer.set_chunk_highlight(sp.chunk, kHoverAmt, kHoverR, kHoverG, kHoverB);
                    }
                    if (parked_hot_hook) parked_hot_hook(oid, kHoverAmt, kHoverR, kHoverG, kHoverB);
                }
            }
            if (ev.tool == ui::EditorTool::TracksTrips || ev.tool == ui::EditorTool::StationLinks) {
                // Stations: stops of the selected trip / links in red, hovered busstop in yellow,
                // clicked busstop in bright red.
                auto& ttm = omsi::tt::global_tt_man();
                auto paint_obj = [&](std::int32_t oid, float amt, float r, float g, float b) {
                    for (const auto& sp : scene_places)
                        if (sp.alive && sp.object_id == oid) renderer.set_chunk_highlight(sp.chunk, amt, r, g, b);
                };
                if (ev.tool == ui::EditorTool::TracksTrips && ttm.loaded && ttm.selected_trip >= 0 &&
                    ttm.selected_trip < static_cast<int>(ttm.data.trips.size())) {
                    const auto& tp = ttm.data.trips[static_cast<std::size_t>(ttm.selected_trip)];
                    for (const auto& st : tp.stations) paint_obj(st.object_id, 0.85f, 0.85f, 0.02f, 0.02f);
                }
                if (ev.tool == ui::EditorTool::StationLinks && ttm.loaded && ttm.selected_stnlink >= 0 &&
                    ttm.selected_stnlink < static_cast<int>(ttm.data.stn_links.size())) {
                    const auto& L = ttm.data.stn_links[static_cast<std::size_t>(ttm.selected_stnlink)];
                    if (L.from_id) paint_obj(L.from_id, 0.85f, 0.85f, 0.02f, 0.02f);
                    if (L.to_id) paint_obj(L.to_id, 0.85f, 0.85f, 0.02f, 0.02f);
                }
                if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                    const auto sm = sco::load_sco(edit_objects[static_cast<std::size_t>(selected_object)].sco_path);
                    if (sm && sm->special == sco::ScoSpecial::Busstop)
                        paint_obj(edit_objects[static_cast<std::size_t>(selected_object)].id, 0.95f, 1.f, 0.05f, 0.05f);
                }
                if (hover_object >= 0 && static_cast<std::size_t>(hover_object) < edit_objects.size())
                    paint_obj(edit_objects[static_cast<std::size_t>(hover_object)].id, 0.92f, 1.f, 0.9f, 0.f);
            }
            if ((edit_mode == EditMode::AttachPickSpline || ev.tool == ui::EditorTool::Spline ||
                 sco_path_edit.active) &&
                hover_spline >= 0 && static_cast<std::size_t>(hover_spline) < edit_splines.size()) {
                if (auto* gpu = find_spline_gpu(edit_splines[static_cast<std::size_t>(hover_spline)].id))
                    highlight_chunks(gpu->chunks, kHoverAmt, kHoverR, kHoverG, kHoverB);
            }
            if (object_tab_highlight && selected_object >= 0 &&
                static_cast<std::size_t>(selected_object) < edit_objects.size() &&
                !sco_path_edit.active && edit_mode != EditMode::Mesh) {
                if (const auto ei = spline_attach_entry_for_object(
                        edit_objects[static_cast<std::size_t>(selected_object)].id)) {
                    highlight_spline_attach_group(*ei, kSelAmt, kSelR, kSelG, kSelB);
                    highlight_spline_next_chain(edit_spline_attachments[*ei].parent_spline_id, 0.55f, 0.15f, 0.9f,
                                                1.f);
                    if (parked_hot_hook)
                        for (const auto goid : spline_attach_group_object_ids(*ei))
                            parked_hot_hook(goid, kSelAmt, kSelR, kSelG, kSelB);
                } else {
                    const auto oid = edit_objects[static_cast<std::size_t>(selected_object)].id;
                    for (const auto& sp : scene_places) {
                        if (sp.alive && sp.object_id == oid)
                            renderer.set_chunk_highlight(sp.chunk, kSelAmt, kSelR, kSelG, kSelB);
                    }
                    if (parked_hot_hook) parked_hot_hook(oid, kSelAmt, kSelR, kSelG, kSelB);
                }
            }
            if (spline_tab_highlight && selected_spline >= 0 &&
                static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
                const std::int32_t sid = edit_splines[static_cast<std::size_t>(selected_spline)].id;
                if (auto* gpu = find_spline_gpu(sid))
                    highlight_chunks(gpu->chunks, kSelAmt, kSelR, kSelG, kSelB);
                for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
                    if (edit_spline_attachments[ei].parent_spline_id != sid) continue;
                    highlight_spline_attach_group(ei, 0.4f, 1.f, 0.55f, 0.15f);
                }
            }
            if (object_tab_highlight && !sco_path_edit.active && edit_mode != EditMode::Mesh) {
                for (int gi : group_sel_obj) {
                    if (gi < 0 || static_cast<std::size_t>(gi) >= edit_objects.size()) continue;
                    const auto oid = edit_objects[static_cast<std::size_t>(gi)].id;
                    for (const auto& sp : scene_places) {
                        if (sp.alive && sp.object_id == oid)
                            renderer.set_chunk_highlight(sp.chunk, kSelAmt, kSelR, kSelG, kSelB);
                    }
                    if (parked_hot_hook) parked_hot_hook(oid, kSelAmt, kSelR, kSelG, kSelB);
                }
            }
            if (spline_tab_highlight) {
                for (int gi : group_sel_spl) {
                    if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                    if (auto* g = find_spline_gpu(edit_splines[static_cast<std::size_t>(gi)].id))
                        highlight_chunks(g->chunks, kSelAmt, kSelR, kSelG, kSelB);
                }
            }
            // Live red tint while dragging Recuadro/Lazo (circle already mutates group_sel).
            if (kMeshTintHighlight && marquee.phase == MarqueePhase::Select && marquee.dragging &&
                sel_shape != SelShapeMode::Circle && !over_ui) {
                float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                input::cursor_to_framebuffer(window, mx, my, fw, fh);
                const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                float wvp_live[16]{};
                camera.build_world_view_proj(aspect, wvp_live);
                const float minx = (std::min)(marquee.sx0, marquee.sx1);
                const float maxx = (std::max)(marquee.sx0, marquee.sx1);
                const float miny = (std::min)(marquee.sy0, marquee.sy1);
                const float maxy = (std::max)(marquee.sy0, marquee.sy1);
                auto point_in_lasso = [&](float sx, float sy) -> bool {
                    if (sel_lasso_pts.size() < 3) return false;
                    bool inside = false;
                    for (std::size_t i = 0, j = sel_lasso_pts.size() - 1; i < sel_lasso_pts.size(); j = i++) {
                        const float yi = sel_lasso_pts[i].y, yj = sel_lasso_pts[j].y;
                        const float xi = sel_lasso_pts[i].x, xj = sel_lasso_pts[j].x;
                        if (((yi > sy) != (yj > sy)) &&
                            (sx < (xj - xi) * (sy - yi) / ((yj - yi) + 1.e-8f) + xi))
                            inside = !inside;
                    }
                    return inside;
                };
                auto in_preview = [&](float sx, float sy) {
                    if (sel_shape == SelShapeMode::Lasso) return point_in_lasso(sx, sy);
                    return sx >= minx && sx <= maxx && sy >= miny && sy <= maxy;
                };
                for (const auto& sp : scene_places) {
                    if (!sp.alive) continue;
                    float sx = 0.f, sy = 0.f;
                    if (!world_to_screen(sp.tx, sp.ty, sp.tz, 0.f, 0.f, fw, fh, wvp_live, sx, sy)) continue;
                    if (!in_preview(sx, sy)) continue;
                    renderer.set_chunk_highlight(sp.chunk, kSelAmt, kSelR, kSelG, kSelB);
                }
                for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                    const auto& s = edit_splines[i];
                    const auto samp = spline::evaluate_spline(s, s.length * 0.5f);
                    float sx = 0.f, sy = 0.f;
                    if (!world_to_screen(samp.x, samp.y, samp.z, 0.f, 0.f, fw, fh, wvp_live, sx, sy)) continue;
                    if (!in_preview(sx, sy)) continue;
                    if (auto* g = find_spline_gpu(s.id)) highlight_chunks(g->chunks, kSelAmt, kSelR, kSelG, kSelB);
                }
            }
            // Circle: hover preview of exactly what a click / stroke takes (red when Ctrl/Shift removes)
            if (marquee.phase == MarqueePhase::Select && sel_shape == SelShapeMode::Circle && !over_ui &&
                edit_mode == EditMode::None) {
                float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                input::cursor_to_framebuffer(window, mx, my, fw, fh);
                const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                float wvp_c[16]{};
                camera.build_world_view_proj(aspect, wvp_c);
                static std::vector<int> s_ch_o, s_ch_s;
                static std::vector<std::size_t> s_ch_p;
                circle_hits(mx, my, fw, fh, wvp_c, s_ch_o, s_ch_s, &s_ch_p);
                const bool rem = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                const float hr = rem ? 1.f : 0.55f, hg = rem ? 0.3f : 0.95f, hb = rem ? 0.25f : 1.f;
                for (const std::size_t pi : s_ch_p)
                    if (pi < scene_places.size()) renderer.set_chunk_highlight(scene_places[pi].chunk, 0.5f, hr, hg, hb);
                for (const int si : s_ch_s)
                    if (si >= 0 && static_cast<std::size_t>(si) < edit_splines.size())
                        if (auto* g = find_spline_gpu(edit_splines[static_cast<std::size_t>(si)].id))
                            highlight_chunks(g->chunks, 0.5f, hr, hg, hb);
            }
            {
                const auto& hid_o = panel.imgui_state().hidden_object_ids;
                const auto& hid_s = panel.imgui_state().hidden_spline_ids;
                const float op = panel.imgui_state().mesh_opacity;
                const bool ghost = op < 0.995f;
                // Always restore visible+opacity when not intentionally hidden. Previously only
                // wrote when ghost/hide was true, so a lowered opacity or a stream hide could leave
                // splines transparent/invisible until a click remeshed them.
                {
                    auto _watch = main_watchdog.phase("sco_visibility");
                    auto _perf = frame_profiler.scope(MapEditorFrameProfiler::ScoVisibility);
                    if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: pre_sco_visibility");
                    // Apply script-driven visibility exactly once, after UI/input events, then let
                    // explicit editor hiding and distance culling override it below.
                    // Traffic-light / script visibility only needs ~30 Hz, not every frame; it
                    // iterates every alive place (2+ ms on large maps). Run every 4th frame.
                    static int s_sco_vis_tick = 0;
                    const bool sco_vis_tick = ((++s_sco_vis_tick & 3) == 1);
                    try {
                        if (!stream_busy() && g_editor_frame_id > 3 && sco_vis_tick)
                            apply_sco_script_visibility();
                    } catch (const std::exception& ex) {
                        log::error(std::string("apply_sco_script_visibility: ") + ex.what());
                    } catch (...) {
                        log::error("apply_sco_script_visibility: unknown exception");
                    }
                    if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: post_sco_visibility");
                }
                // Opacity/hide pass: only when ghost slider, hide-sets, or scene size changed.
                // Touching every alive chunk every frame was ~ms of pure iteration on big maps.
                {
                    static float s_last_op = -1.f;
                    static std::size_t s_last_hid_o = static_cast<std::size_t>(-1);
                    static std::size_t s_last_hid_s = static_cast<std::size_t>(-1);
                    static std::size_t s_last_scene_n = static_cast<std::size_t>(-1);
                    static std::size_t s_last_spline_n = static_cast<std::size_t>(-1);
                    const bool opacity_dirty =
                        ghost || op != s_last_op || hid_o.size() != s_last_hid_o || hid_s.size() != s_last_hid_s ||
                        scene_places.size() != s_last_scene_n || spline_gpu.size() != s_last_spline_n;
                    if (opacity_dirty) {
                        for (auto& sp : scene_places) {
                            if (!sp.alive) continue;
                            if (hid_o.count(sp.object_id)) {
                                renderer.set_chunk_visible(sp.chunk, false);
                            } else {
                                // Do NOT force visible=true: Ampel/mesh [visible] (Red/Yellow/Green) is
                                // applied by apply_sco_script_visibility — forcing true shows all lights on.
                                renderer.set_chunk_opacity(sp.chunk, ghost ? op : 1.f);
                            }
                        }
                        for (auto& g : spline_gpu) {
                            if (!g.alive) continue;
                            const bool hide = hid_s.count(g.id) != 0;
                            for (const auto c : g.chunks) {
                                if (hide) {
                                    renderer.set_chunk_visible(c, false);
                                } else {
                                    // Do not force visible=true — screensize/distance cull owns visibility.
                                    renderer.set_chunk_opacity(c, ghost ? op : 1.f);
                                    renderer.set_chunk_wireframe(c, false);
                                }
                            }
                        }
                        s_last_op = op;
                        s_last_hid_o = hid_o.size();
                        s_last_hid_s = hid_s.size();
                        s_last_scene_n = scene_places.size();
                        s_last_spline_n = spline_gpu.size();
                    }
                }
                // Re-apply OMSI-style cull after any hide/opacity pass.
                {
                    auto _watch = main_watchdog.phase("distance_cull");
                    auto _perf = frame_profiler.scope(MapEditorFrameProfiler::DistanceCull);
                    apply_editor_visual_distance_cull();
                }
            }
            // .sco path editor: view toggles must win over the distance cull above, otherwise the
            // hidden model/path reappears every frame.
            if (sco_path_edit.active) {
                const auto& st = panel.imgui_state();
                for (auto& sp : scene_places) {
                    if (sp.alive && sp.object_id == sco_path_edit.object_id)
                        renderer.set_chunk_visible(sp.chunk, !st.sco_path_hide_model);
                }
                if (sco_path_edit.temp_first >= 0) {
                    // Keep the path-width spline visible and clickable even when the width/rail
                    // reference is hidden ("Ocultar ancho" only hides the rail).
                    for (int ti = 0; ti < sco_path_edit.temp_count; ++ti) {
                        const int si = sco_path_edit.temp_first + ti;
                        if (si < 0 || si >= static_cast<int>(edit_splines.size())) continue;
                        if (auto* g = find_spline_gpu(edit_splines[static_cast<std::size_t>(si)].id))
                            for (const auto c : g->chunks)
                                renderer.set_chunk_visible(c, true);
                    }
                }
            }
            if (edit_mode == EditMode::TrimSpline &&
                trim_preview_chunk != static_cast<std::size_t>(-1)) {
                renderer.set_chunk_visible(trim_preview_chunk, true);
                renderer.set_chunk_highlight(trim_preview_chunk, 0.9f, 1.f, 0.45f, 0.12f);
            }
        }
        interaction_pick_watch.reset();

        // Status line + window title at 10 Hz (ostringstream, ground ray and SetWindowText every frame
        // were idle CPU for text nobody reads at 60 Hz); the title is only set when it changed.
        static int s_status_tick = 0;
        if (++s_status_tick >= 6) {
            s_status_tick = 0;
            auto _watch = main_watchdog.phase("frame_status_hud");
            std::ostringstream st;
            float mmx = 0.f, mmy = 0.f, mfw = 1.f, mfh = 1.f;
            input::cursor_to_framebuffer(window, mmx, mmy, mfw, mfh);
            camera.ground_z = camera.target_z;
            float hx = 0.f, hy = 0.f, hz = 0.f;
            bool have_hit = false;
            if (const auto hit = camera.hit_ground_xy(mmx, mmy, mfw, mfh)) {
                hx = hit->x;
                hy = hit->y;
                hz = sample_terrain_editor_xy(hit->x, hit->y);
                have_hit = true;
            }
            st << "tool=" << static_cast<int>(ev.tool) << " target=(" << camera.target_x << "," << camera.target_y
               << "," << camera.target_z << ") r=" << camera.radius << " obj=" << selected_object
               << " sli=" << selected_spline;
            if (have_hit)
                st << " mouse=(" << hx << "," << hy << "," << hz << ") world=(" << (hx + tile_ox) << ","
                   << (hy + tile_oy) << "," << hz << ")";
            if (edit_mode == EditMode::Grab) {
                st << " [GRAB]";
                if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                    (selected_spline >= 0 || selected_object >= 0)) {
                    const int pc = path_align_path_count();
                    st << " align path " << path_align_path_index;
                    if (pc > 1) st << "/" << (pc - 1) << " F9=cycle";
                }
            }
            else if (edit_mode == EditMode::GrabHeight) st << " [HEIGHT]";
            else if (edit_mode == EditMode::GrabLength) st << " [LEN]";
            else if (edit_mode == EditMode::Radius) st << " [RAD]";
            else if (edit_mode == EditMode::Rotate) st << (group_rotate_active ? " [ROT GRUPO]" : " [ROT]");
            else if (edit_mode == EditMode::AttachPickParent) st << " [ATTACH OBJ]";
            else if (edit_mode == EditMode::AttachPickSpline) st << " [ATTACH SPL]";
            else if (edit_mode == EditMode::PickSplineGrad) st << " [PICK GRAD]";
            else if (edit_mode == EditMode::EditSplineGrad) st << " [GRAD]";
            else if (edit_mode == EditMode::SplineFollow) st << " [FOLLOW R]";
            else if (edit_mode == EditMode::NewGrab) st << " [NEW grab 10m]";
            else if (edit_mode == EditMode::NewFollow)
                st << " [NEW sli len=" << new_spline_draft.length << " r=" << new_spline_draft.radius
                   << " skew=" << new_spline_draft.skew_start << ".." << new_spline_draft.skew_end << "]";
            else if (edit_mode == EditMode::NewScoFollow) st << " [NEW sco]";
            else if (edit_mode == EditMode::TrimSpline) st << " [TRIM " << trim_along << "m]";
            if (save_job_active)
                st << " | Guardando " << save_job_i << "/" << save_job_n;
            else if (stream_busy()) st << " | " << stream_status;
            else if (sa_spawn_active)
                st << " | SA spawn pend=" << sa_spawn_queue.size();
            const std::string status = st.str();
            static std::string last_status;
            if (status != last_status) {
                panel.set_status(status);
                last_status = status;
            }
            // FPS (1s window) + editor version in title bar
            static float fps_accum = 0.f;
            static int fps_frames = 0;
            static int fps_display = 0;
            static auto fps_t0 = std::chrono::steady_clock::now();
            const auto fps_now = std::chrono::steady_clock::now();
            fps_accum += std::chrono::duration<float>(fps_now - fps_t0).count();
            fps_t0 = fps_now;
            fps_frames += 6; // this block runs every 6th frame
            if (fps_accum >= 0.5f) {
                fps_display = static_cast<int>(static_cast<float>(fps_frames) / fps_accum + 0.5f);
                fps_accum = 0.f;
                fps_frames = 0;
            }
            const auto gpu = renderer.last_gpu_timing();
            std::string title = std::string(omsi::kEditorProductName) + " " + omsi::kEditorVersion;
            title += std::string(" [") + gfx::backend::label(gfx::backend::active()) + "]";
            {
                char fps_bit[96]{};
                std::snprintf(fps_bit, sizeof(fps_bit), " | %d FPS 1%%=%.0f GPU=%.1fms %s", fps_display,
                              frame_profiler.fps.fps_1pct_low(), gpu.gpu_total_ms, frame_profiler.fps.bound());
                title += fps_bit;
            }
            if (stream_busy())
                title += " | streaming...";
            else if (save_job_active)
                title += " | guardando...";
            title += " | " + cfg.global.friendly_name;
            static std::string s_last_title;
            if (title != s_last_title) {
                s_last_title = title;
                glfwSetWindowTitle(window, title.c_str());
            }
        }

        // Busy toast state for ImGui (bottom-right spinner + %).
        {
            auto& bst = panel.imgui_state();
            bst.boot_screen = false; // main loop reached: the editor is usable, back to the small toast
            if (save_job_active) {
                bst.busy_overlay = true;
                bst.busy_spinner = true;
                bst.busy_title = "Guardando mapa";
                bst.busy_detail = "Tile " + std::to_string(save_job_i) + "/" + std::to_string(save_job_n);
                bst.busy_progress = (save_job_n > 0)
                                        ? static_cast<float>(save_job_i) / static_cast<float>(save_job_n)
                                        : 0.f;
            } else if (stream_busy()) {
                bst.busy_overlay = true;
                bst.busy_spinner = true;
                bst.busy_title = "Cargando tiles";
                bst.busy_detail = stream_status;
                bst.busy_progress = stream_progress_frac;
            } else if (sa_spawn_active && !sa_spawn_queue.empty()) {
                bst.busy_overlay = true;
                bst.busy_spinner = true;
                bst.busy_title = "Cargando attachments";
                {
                    const std::size_t ei = sa_spawn_queue.front();
                    std::string detail = "Pendientes: " + std::to_string(sa_spawn_queue.size());
                    if (ei < edit_spline_attachments.size()) {
                        const auto& e = edit_spline_attachments[ei];
                        std::string map_name =
                            "tile_" + std::to_string(e.tile_gx) + "_" + std::to_string(e.tile_gy) + ".map";
                        for (const auto& t : world.tiles()) {
                            if (t.grid_x == e.tile_gx && t.grid_y == e.tile_gy) {
                                map_name = t.path.filename().string();
                                break;
                            }
                        }
                        detail += " | id=" + std::to_string(e.att.instance_id) + " " + map_name;
                    }
                    bst.busy_detail = std::move(detail);
                }
                    bst.busy_progress = (sa_spawn_wave > 0)
                                        ? 1.f - static_cast<float>(sa_spawn_queue.size()) /
                                                    static_cast<float>(sa_spawn_wave)
                                        : -1.f;
            } else if (now < undo_toast_until && !undo_toast_title.empty()) {
                bst.busy_overlay = true;
                bst.busy_spinner = false;
                bst.busy_title = undo_toast_title;
                bst.busy_detail = undo_toast_detail;
                bst.busy_progress = -1.f;
            } else {
                bst.busy_overlay = false;
                bst.busy_spinner = false;
                bst.busy_progress = -1.f;
                bst.busy_title.clear();
                bst.busy_detail.clear();
            }
        }

        const float aspect = (fbw > 0 && fbh > 0) ? static_cast<float>(fbw) / static_cast<float>(fbh) : 16.f / 9.f;
        float wvp_t[16]{};
        camera.build_world_view_proj(aspect, wvp_t);
        renderer.set_world_view_proj(wvp_t);
        update_renderer_tile_cull();

        const float elapsed = std::chrono::duration<float>(now - loop_start).count();
        // Wait ~10s so SCO/spline textures finish uploading before capture.
        constexpr float kScreenshotAtSec = 10.f;
        if (cfg.verify_mode) {
            if (!shot0 && elapsed >= kScreenshotAtSec) {
                const auto path = cfg.screenshot_path.empty() ? std::filesystem::path("editor_entry0.bmp")
                                                             : cfg.screenshot_path;
                renderer.set_pending_screenshot(path);
                shot0 = true;
                log::info("editor capture after " + std::to_string(elapsed) + "s");
            }
            if (elapsed >= kScreenshotAtSec + 1.5f) glfwSetWindowShouldClose(window, GLFW_TRUE);
        } else if (!cfg.screenshot_path.empty() && !shot0 && elapsed >= kScreenshotAtSec) {
            renderer.set_pending_screenshot(cfg.screenshot_path);
            shot0 = true;
        }
        // Test tour (OMSI_SHOT_TOUR): "label:gx:gy:lx:ly:id:hour:radius:theta:phi;..." - streams the tile of each
        // object, frames it, sets the hour, and writes shots/<label>.bmp; closes the editor at the end.
        {
            struct ShotStop {
                std::string label;
                int gx = 0, gy = 0;
                float lx = 0.f, ly = 0.f;
                std::int32_t id = 0;
                float hour = 13.f, radius = 20.f, theta = -1.f, phi = -1.f, dz = 0.f;
            };
            static std::vector<ShotStop> s_tour;
            static bool s_tour_init = false;
            static std::size_t s_ti = 0;
            static int s_stage = 0;
            static float s_t = 0.f;
            if (!s_tour_init) {
                s_tour_init = true;
                if (const char* env = std::getenv("OMSI_SHOT_TOUR")) {
                    std::string all(env);
                    std::size_t p = 0;
                    while (p < all.size()) {
                        auto e = all.find(';', p);
                        if (e == std::string::npos) e = all.size();
                        const std::string item = all.substr(p, e - p);
                        p = e + 1;
                        std::vector<std::string> f;
                        std::size_t q = 0;
                        while (q <= item.size()) {
                            auto c = item.find(':', q);
                            if (c == std::string::npos) c = item.size();
                            f.push_back(item.substr(q, c - q));
                            q = c + 1;
                        }
                        if (f.size() < 8) continue;
                        ShotStop st{};
                        try {
                            st.label = f[0];
                            st.gx = std::stoi(f[1]);
                            st.gy = std::stoi(f[2]);
                            st.lx = std::stof(f[3]);
                            st.ly = std::stof(f[4]);
                            st.id = std::stoi(f[5]);
                            st.hour = std::stof(f[6]);
                            st.radius = std::stof(f[7]);
                            if (f.size() > 8) st.theta = std::stof(f[8]);
                            if (f.size() > 9) st.phi = std::stof(f[9]);
                            if (f.size() > 10) st.dz = std::stof(f[10]);
                        } catch (...) {
                            continue;
                        }
                        s_tour.push_back(st);
                    }
                    log::info("shot tour stops=" + std::to_string(s_tour.size()));
                }
            }
            if (!s_tour.empty() && elapsed > 8.f) {
                ui::set_hide_all_ui(true);
                static const bool s_tour_rails = std::getenv("OMSI_SHOT_RAILS") != nullptr;
                if (!s_tour_rails)
                    for (auto& r : path_rails)
                        if (r.alive) renderer.set_chunk_visible(r.chunk, false);
                if (s_ti >= s_tour.size()) {
                    if (s_stage != 99) {
                        s_stage = 99;
                        s_t = elapsed;
                    } else if (elapsed - s_t > 2.f) {
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                } else {
                    auto& st = s_tour[s_ti];
                    const auto [wx, wy] = world_from_tile_local(st.lx, st.ly, st.gx, st.gy);
                    if (s_stage == 0) {
                        panel.imgui_state().sim_hour = st.hour;
                        map::EntryPoint ep{};
                        ep.position.x = wx;
                        ep.position.y = wy;
                        ep.position.z = camera.target_z;
                        ep.position_local = ep.position;
                        ep.rotation_quat.w = 1.f;
                        ep.name = st.label;
                        // (id -1: the test bus, OMSI_TEST_SPAWN_BUS, wherever it stands)
                        // (id -2: the test bus from its driver's seat camera)
                        if ((st.id != -1 && st.id != -2) || !omsi_bus) teleport_camera_to_entrypoint(ep, "shot_tour");
                        s_stage = 1;
                        s_t = elapsed;
                    } else if (s_stage == 1) {
                        // wait for the object (its tile streams in), then frame it
                        float oz = -1.e9f;
                        const bool in_cab = st.id == -2;
                        const bool on_bus = st.id == -1 || in_cab;
                        if (on_bus) {
                            if (omsi_bus && omsi_bus->age > 8.f) oz = omsi_bus->z + 1.6f + st.dz;
                        } else
                            for (const auto& sp2 : scene_places)
                                if (sp2.alive && sp2.object_id == st.id) {
                                    oz = sp2.tz + sp2.height * 0.4f + st.dz;
                                    break;
                                }
                        if (oz > -1.e8f || elapsed - s_t > (on_bus ? 90.f : 45.f)) {
                            // (the test bus is driven: its seat camera put every shot in the cab whatever the
                            //  orbit asked - out of the driver's view for the tour's own camera)
                            if (in_cab && omsi_bus) {
                                // the driver's seat (the bus's standard driver camera), as the bus starts
                                bus_drive_active = true;
                                camera.editor_nav = false;
                                bus_cam_mode = 1;
                                bus_cam_driver_idx = (std::max)(0, omsi_bus->doc.std_driver_camera_index);
                                bus_cam_need_snap = true;
                                apply_bus_camera(*omsi_bus);
                            } else {
                                if (on_bus && bus_drive_active) exit_bus_drive();
                                camera.target_x = on_bus && omsi_bus ? omsi_bus->x : wx - tile_ox;
                                camera.target_y = on_bus && omsi_bus ? omsi_bus->y : wy - tile_oy;
                                if (oz > -1.e8f) camera.target_z = oz;
                                camera.ground_z = camera.target_z;
                                camera.radius = st.radius;
                                if (st.theta >= 0.f) camera.theta = st.theta;
                                if (st.phi >= 0.f) camera.phi = st.phi;
                                camera.reset_mouse();
                            }
                            log::info("shot tour " + st.label + (oz > -1.e8f ? " found" : " NOT FOUND") + " id=" +
                                      std::to_string(st.id));
                            {
                                // diagnostics for the test shots: script vars, attachments of this object
                                std::string dv;
                                for (const char* vn : {"clock_hand_hour", "clock_hand_minute", "Werbung"})
                                    dv += std::string(" ") + vn + "=" + std::to_string(scenery_scripts.float_var(st.id, vn));
                                int kids = 0, kids_alive = 0;
                                for (const auto& a : edit_attachments) {
                                    if (a.parent_id != st.id) continue;
                                    ++kids;
                                    for (const auto& sp3 : scene_places)
                                        if (sp3.alive && sp3.object_id == a.child_id) {
                                            ++kids_alive;
                                            log::info("shot tour child " + std::to_string(a.child_id) + " at (" +
                                                      std::to_string(sp3.tx) + "," + std::to_string(sp3.ty) + "," +
                                                      std::to_string(sp3.tz) + ") parent z=" + std::to_string(oz));
                                            break;
                                        }
                                }
                                log::info("shot tour diag" + dv + " attach=" + std::to_string(kids) + " alive=" +
                                          std::to_string(kids_alive) + " edit_attachments=" +
                                          std::to_string(edit_attachments.size()) + " tiles=" +
                                          std::to_string(world.tiles().size()));
                                for (const auto& t : world.tiles())
                                    if (t.grid_x == st.gx && t.grid_y == st.gy)
                                        log::info("shot tour tile (" + std::to_string(t.grid_x) + "," + std::to_string(t.grid_y) +
                                                  ") objects=" + std::to_string(t.objects.size()) + " attachments=" +
                                                  std::to_string(t.attachments.size()));
                                for (const auto& t : world.tiles())
                                    for (const auto& a : t.attachments)
                                        if (a.parent_id == st.id || a.child_id == st.id + 1)
                                            log::info("shot tour tile-att child=" + std::to_string(a.child_id) + " parent=" +
                                                      std::to_string(a.parent_id) + " ap=" + std::to_string(a.attach_point_index) +
                                                      " inst=" + std::to_string(a.extra_rot_y) + " sco=" + a.sco_path.filename().string());
                                for (const auto& a : edit_attachments)
                                    if (a.child_id == st.id + 1)
                                        log::info("shot tour edit-att child=" + std::to_string(a.child_id) + " parent=" +
                                                  std::to_string(a.parent_id));
                            }
                            s_stage = 2;
                            s_t = elapsed;
                        }
                    } else if (s_stage == 2 && elapsed - s_t > 9.f) {
                        for (const auto& sp4 : scene_places)
                            if (sp4.alive && sp4.object_id == st.id)
                                log::info("shot tour chunk " + renderer.chunk_debug_line(sp4.chunk) + " applied=" +
                                          std::to_string(sp4.text_applied.size()));
                        log::info("shot tour capture " + st.label + " script=" +
                                  std::to_string(scenery_scripts.script_state(st.id)) + " hour=" +
                                  std::to_string(scenery_scripts.float_var(st.id, "clock_hand_hour")) + " werbung=" +
                                  std::to_string(scenery_scripts.float_var(st.id, "Werbung")));
                        std::error_code mec;
                        std::filesystem::create_directories("shots", mec);
                        renderer.set_pending_screenshot(std::filesystem::path("shots") / (st.label + ".bmp"));
                        s_stage = 3;
                        s_t = elapsed;
                    } else if (s_stage == 3 && elapsed - s_t > 1.f) {
                        ++s_ti;
                        s_stage = 0;
                    }
                }
            }
        }

        editor_logic_watch.reset();
        frame_profiler.end_segment(MapEditorFrameProfiler::EditorLogic);
        if (imgui_overlay.active()) {
            auto _watch_imgui = main_watchdog.phase("imgui_build");
            frame_profiler.begin_segment();
            imgui_overlay.begin_frame();
            // ---- Spline handles: union balls + gradient circles (screen-space) ----
            {
                auto _w_sh = main_watchdog.phase("imgui.spline_handles");
                auto _perf_sh = frame_profiler.scope(MapEditorFrameProfiler::SplineHandles);
                // Project in framebuffer space (same as pickers), then map to ImGui display coords.
                float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                input::cursor_to_framebuffer(window, mx, my, fw, fh);
                const ImGuiIO& io = ImGui::GetIO();
                const float disp_w = (io.DisplaySize.x > 1.f) ? io.DisplaySize.x : fw;
                const float disp_h = (io.DisplaySize.y > 1.f) ? io.DisplaySize.y : fh;
                const float sx_to_imgui = (fw > 1.f) ? (disp_w / fw) : 1.f;
                const float sy_to_imgui = (fh > 1.f) ? (disp_h / fh) : 1.f;

                const float aspect_h = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
                float wvp_h[16]{};
                camera.build_world_view_proj(aspect_h, wvp_h);

                int hit = -1;
                std::vector<SplineHandleHit> handles;
                // Spline handles only exist in the Spline tool. In Objects/Tiles/etc. the
                // collection is skipped entirely (no nodes drawn, and less CPU). The .sco path
                // editor can also hide them for a cleaner rail-only view.
                const bool hide_sco_nodes = (sco_path_edit.active && panel.imgui_state().sco_path_hide_nodes) ||
                                            panel.imgui_state().hide_nodes; // Vista > Ocultar nodos
                const bool spline_tool =
                    (ev.tool == ui::EditorTool::Spline || sco_path_edit.active) && !hide_sco_nodes;
                // __try/SEH no usable aqui (C2712 con objetos C++). Collect endurecido + logs.
                if (spline_tool) try {
                    const int hover_idx = pick_spline_index(mx, my, fw, fh, wvp_h);
                    collect_spline_handles(handles, fw, fh, wvp_h, hover_idx,
                                           editor_ui_settings.union_ball_screen_pct,
                                           editor_ui_settings.grad_circle_screen_pct);
                    hit = pick_spline_handle(mx, my, handles);
                } catch (const std::exception& ex) {
                    std::fprintf(stderr, "[nodo] exception collect/pick: %s\n", ex.what());
                    std::fflush(stderr);
                    log::error(std::string("[nodo] exception collect/pick: ") + ex.what());
                    handles.clear();
                    hit = -1;
                } catch (...) {
                    std::fputs("[nodo] exception desconocida collect/pick\n", stderr);
                    std::fflush(stderr);
                    log::error("[nodo] exception desconocida collect/pick");
                    handles.clear();
                    hit = -1;
                }

                const bool handle_click_ok =
                    (edit_mode == EditMode::None || edit_mode == EditMode::PickSplineGrad) &&
                    !grabbing && !rotating;
                if (handle_click_ok && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    !ImGui::GetIO().WantCaptureMouse) {
                    std::fprintf(stderr, "[nodo] click hit=%d handles=%zu mode=%d\n", hit,
                                 handles.size(), static_cast<int>(edit_mode));
                    std::fflush(stderr);
                    log::info("[nodo] click hit=" + std::to_string(hit) +
                              " handles=" + std::to_string(handles.size()));
                }
                try {
                    if (handle_click_ok && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hit >= 0 &&
                        hit < static_cast<int>(handles.size()) && !ImGui::GetIO().WantCaptureMouse) {
                        const auto& hh = handles[static_cast<std::size_t>(hit)];
                        chain_sel_handle_spline = hh.spline_id;
                        chain_sel_handle_partner = hh.partner_id;
                        chain_sel_handle_kind =
                            (hh.kind == SplineHandleHit::Kind::UnionBall) ? 1 : 2;
                        chain_sel_handle_start_side = hh.is_start_side;
                        if (hh.kind == SplineHandleHit::Kind::UnionBall) {
                            selected_spline = -1;
                            group_sel_spl.clear();
                            group_sel_spl_ids.clear();
                            group_sel_obj.clear();
                            group_sel_obj_ids.clear();
                            if (edit_mode == EditMode::PickSplineGrad) {
                                edit_mode = EditMode::None;
                                panel.set_status("Nodo seleccionado — G=mover");
                            }
                            node_click_suppress_body_pick = true;
                            if (hh.partner_id != 0) {
                                const std::int32_t end_id =
                                    hh.is_start_side ? hh.partner_id : hh.spline_id;
                                const std::int32_t start_id =
                                    hh.is_start_side ? hh.spline_id : hh.partner_id;
                                log::info("[nodo] une final de spline id=" + std::to_string(end_id) +
                                          " y el inicio de spline id=" + std::to_string(start_id));
                            } else if (hh.is_start_side) {
                                log::info("[nodo] nodo de inicio de spline id=" +
                                          std::to_string(hh.spline_id));
                            } else {
                                log::info("[nodo] nodo final de spline id=" +
                                          std::to_string(hh.spline_id) + " sin next id");
                            }
                        } else {
                            // Middle node (GradCircle): select the node only, never re-pick the body.
                            node_click_suppress_body_pick = true;
                            panel.set_status("Nodo medio seleccionado - G=panza/gradientes");
                        }
                    } else if (handle_click_ok && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                               hit < 0 && !ImGui::GetIO().WantCaptureMouse) {
                        chain_sel_handle_kind = 0;
                        chain_sel_handle_spline = 0;
                        chain_sel_handle_partner = 0;
                    }
                } catch (const std::exception& ex) {
                    std::fprintf(stderr, "[nodo] exception select: %s\n", ex.what());
                    std::fflush(stderr);
                    log::error(std::string("[nodo] exception select: ") + ex.what());
                    chain_sel_handle_kind = 0;
                    chain_sel_handle_spline = 0;
                    chain_sel_handle_partner = 0;
                } catch (...) {
                    std::fputs("[nodo] exception desconocida select\n", stderr);
                    std::fflush(stderr);
                    log::error("[nodo] exception desconocida select");
                    chain_sel_handle_kind = 0;
                    chain_sel_handle_spline = 0;
                    chain_sel_handle_partner = 0;
                }
                // NO borrar chain_sel cuando selected_spline < 0: el nodo vive solo.

                auto to_imgui = [&](float px, float py) -> ImVec2 {
                    return ImVec2(px * sx_to_imgui, py * sy_to_imgui);
                };

                if (ImDrawList* dl = ImGui::GetBackgroundDrawList()) {
                    for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
                        const auto& h = handles[static_cast<std::size_t>(i)];
                        const bool is_hit = (i == hit);
                        // Nodo: solo la bola azul (sel) / naranja (hover). Nunca resaltar el body.
                        const bool is_sel =
                            chain_sel_handle_kind != 0 &&
                            ((chain_sel_handle_kind == 1 && h.kind == SplineHandleHit::Kind::UnionBall &&
                              ((h.spline_id == chain_sel_handle_spline &&
                                h.is_start_side == chain_sel_handle_start_side) ||
                               (h.partner_id == chain_sel_handle_spline &&
                                h.spline_id == chain_sel_handle_partner) ||
                               (h.spline_id == chain_sel_handle_partner &&
                                h.partner_id == chain_sel_handle_spline))) ||
                             (chain_sel_handle_kind == 2 && h.kind == SplineHandleHit::Kind::GradCircle &&
                              h.spline_id == chain_sel_handle_spline));
                        ImU32 col = IM_COL32(255, 255, 255, 180);
                        if (h.kind == SplineHandleHit::Kind::GradCircle) {
                            // Nodo medio: idle naranja, hover amarillo, seleccionado rojo.
                            col = is_sel ? IM_COL32(255, 60, 60, 255)
                                         : (is_hit ? IM_COL32(255, 240, 90, 240)
                                                   : IM_COL32(255, 160, 40, 200));
                        } else {
                            // Extremos: idle blanco, hover amarillo, seleccionado rojo.
                            col = is_sel ? IM_COL32(255, 60, 60, 255)
                                         : (is_hit ? IM_COL32(255, 240, 90, 240)
                                                   : IM_COL32(255, 255, 255, 180));
                        }
                        const ImVec2 c = to_imgui(h.screen_x, h.screen_y);
                        const float r = h.radius_px * 0.5f * (sx_to_imgui + sy_to_imgui);
                        dl->AddCircleFilled(c, r, col, 24);
                        dl->AddCircle(c, r, IM_COL32(0, 0, 0, 160), 24, 1.5f);
                    }
                }
                // Sin highlight de arista/body: el nodo es solo la bola.
            }
            {
                auto _w_panels = main_watchdog.phase("imgui.place_and_filter_ui");
                place_fx.enabled = panel.imgui_state().place_feedback;
                {
                    const std::string& pack = panel.imgui_state().sfx_pack;
                    if (pack != editor_sfx.pack()) editor_sfx.set_pack(pack.empty() ? "blocky" : pack);
                    editor_sfx.set_enabled(panel.imgui_state().place_feedback);
                    editor_sfx.set_volume(panel.imgui_state().place_feedback_volume / 100.f);
                }
                {
                    ai_bus_list_window();
                    // World sound window (self-contained block, see omsi/sound/world_sound_window.hpp).
                    const int ws_flags = omsi::sound::draw_world_sound_window(
                        world_sound_ui, world_sound_scene.candidate_count(), world_sound_scene.live_objects(),
                        world_sound_scene.live_voices());
                    if (ws_flags & omsi::sound::kWorldSoundChanged) {
                        world_sound.set_enabled(world_sound_ui.enabled);
                        world_sound.set_master_volume(world_sound_ui.volume_pct / 100.f);
                    }
                    if (ws_flags & omsi::sound::kWorldSoundCommit) {
                        editor_ui_settings.world_sound_enabled = world_sound_ui.enabled;
                        editor_ui_settings.world_sound_volume = world_sound_ui.volume_pct;
                        map::save_editor_ui_settings(cfg.global.root, editor_ui_settings);
                    }
                }
                place_fx.tick(ImGui::GetIO().DeltaTime);
                {
                    float pmx = 0.f, pmy = 0.f, pfw = 1.f, pfh = 1.f;
                    input::cursor_to_framebuffer(window, pmx, pmy, pfw, pfh);
                    const float aspect = (pfw > 0.f && pfh > 0.f) ? pfw / pfh : 16.f / 9.f;
                    float wvp[16]{};
                    camera.build_world_view_proj(aspect, wvp);
                    place_fx.draw_imgui([&](float wx, float wy, float wz, float& sx, float& sy) {
                        return world_to_screen(wx, wy, wz, 0.f, 0.f, pfw, pfh, wvp, sx, sy);
                    });
                    // Only while a place bounce runs (and one frame after, to restore scale 1): the walk
                    // over every scene place asking bounce_scale() cost ~0.8 ms/frame at 1 GHz for nothing.
                    static std::unordered_set<std::int32_t> s_bounced;
                    bool bouncing = false;
                    for (const auto& e : place_fx.events()) {
                        if (e.kind != omsi::editor::PlaceFxKind::PlaceBounce || e.object_id == 0) continue;
                        bouncing = true;
                        s_bounced.insert(e.object_id);
                    }
                    const bool restore_pass = !bouncing && !s_bounced.empty();
                    static std::remove_reference_t<decltype(scene_places)> s_no_places;
                    for (auto& sp : s_bounced.empty() ? s_no_places : scene_places) {
                        if (!sp.alive || sp.object_id == 0 || !sp.has_base_world) continue;
                        if (!s_bounced.count(sp.object_id)) continue;
                        const float s = restore_pass ? 1.f : place_fx.bounce_scale(sp.object_id);
                        using namespace DirectX;
                        const XMMATRIX base = XMLoadFloat4x4(&sp.base_world);
                        const XMMATRIX bounce_world = XMMatrixScaling(s, s, s) * base;
                        XMFLOAT4X4 wt{};
                        XMStoreFloat4x4(&wt, XMMatrixTranspose(bounce_world));
                        renderer.set_chunk_world_matrix(sp.chunk, reinterpret_cast<const float*>(&wt));
                    }
                    if (restore_pass) s_bounced.clear();
                }
#include "modules/mod_sel_filter_ui.inc.cpp"
                // The AI car picked with a click: what it is, where, how fast; delete it (it is taken off the
                // road) or look at it. Its meshes are highlighted while selected.
                {
                    static std::uint64_t s_hl_id = 0;
                    static std::vector<std::size_t> s_hl_chunks;
                    auto clear_hl = [&]() {
                        for (const auto ch : s_hl_chunks)
                            if (ch < renderer.chunk_count()) renderer.set_chunk_highlight(ch, 0.f, 1.f, 1.f, 1.f);
                        s_hl_chunks.clear();
                        s_hl_id = 0;
                    };
                    int ai_i = -1;
                    if (ai_selected_id != 0 && ai_traffic) {
                        const int n = oo_traffic_car_count(ai_traffic);
                        for (int i = 0; i < n && ai_i < 0; ++i) {
                            std::uint64_t id = 0;
                            int mc = 0;
                            if (oo_traffic_car(ai_traffic, i, &id, &mc) && id == ai_selected_id) ai_i = i;
                        }
                    }
                    const auto car_it = ai_traffic_cars.find(ai_selected_id);
                    if (ai_i < 0 || car_it == ai_traffic_cars.end()) {
                        if (ai_selected_id != 0) ai_selected_id = 0; // (gone: off the road / out of range)
                        if (s_hl_id != 0) clear_hl();
                    } else {
                        if (s_hl_id != ai_selected_id) {
                            clear_hl();
                            s_hl_id = ai_selected_id;
                            for (const auto ch : car_it->second.chunks)
                                if (ch != kNoChunk) s_hl_chunks.push_back(ch);
                        }
                        for (const auto ch : s_hl_chunks) renderer.set_chunk_highlight(ch, 0.35f, 1.f, 0.85f, 0.2f);
                        char part[1024]{};
                        oo_traffic_car_part_file(ai_traffic, ai_i, 0, part, sizeof(part));
                        float m16[16]{};
                        oo_traffic_car_mesh_world(ai_traffic, ai_i, 0, m16);
                        const float kmh = std::fabs(oo_traffic_car_var(ai_traffic, ai_i, "Velocity", 0.f));
                        float cex = 0.f, cey = 0.f, cez = 0.f;
                        camera.eye_position(cex, cey, cez);
                        const float dist = std::sqrt((m16[12] - cex) * (m16[12] - cex) + (m16[13] - cey) * (m16[13] - cey));
                        ui::float_slot_begin("ai_car", 40); // docked with the other small windows
                        ImGui::SetNextWindowBgAlpha(0.92f);
                        bool open = true;
                        const bool ai_win_drawn = ImGui::Begin("Auto IA###ai_car_win", &open,
                                                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
                        ui::float_slot_end();
                        if (ai_win_drawn) {
                            const std::string pf = omsi::path_text(std::filesystem::u8path(part).filename());
                            ImGui::Text("Tipo: %s", pf.c_str());
                            ImGui::Text("Id: %llu   Mallas: %zu", static_cast<unsigned long long>(ai_selected_id),
                                        car_it->second.chunks.size());
                            ImGui::Text("Velocidad: %.0f km/h", kmh);
                            ImGui::Text("Posicion: %.1f, %.1f, %.1f  (a %.0f m)", m16[12], m16[13], m16[14], dist);
                            ImGui::TextDisabled("%s", part);
                            // what it does now (openOMSI AiState / BusService), asked every half second
                            {
                                static OoCarState s_st{};
                                static std::uint64_t s_st_id = 0;
                                static double s_st_t = -1.0;
                                static bool s_st_ok = false;
                                const double tnow = glfwGetTime();
                                if (s_st_id != ai_selected_id || tnow - s_st_t > 0.5) {
                                    // its decisions in the log: lane, exits, plan, route, holds - once on the click
                                    // and again whenever they change (one car, twice a second)
                                    static std::string s_expl_last;
                                    char expl[2048];
                                    if (oo_traffic_car_explain(ai_traffic, ai_selected_id, expl, sizeof(expl))) {
                                        std::string e(expl);
                                        // (the speed / standing time change all the time: compared without them)
                                        const auto cut = e.find(" | v ");
                                        const std::string key = std::to_string(ai_selected_id) + (cut == std::string::npos ? e : e.substr(0, cut));
                                        if (key != s_expl_last) {
                                            s_expl_last = key;
                                            log::info("ai car " + std::to_string(ai_selected_id) + ": " + e);
                                        }
                                    }
                                    s_st_id = ai_selected_id;
                                    s_st_t = tnow;
                                    s_st_ok = oo_traffic_car_state(ai_traffic, ai_selected_id, &s_st) != 0;
                                }
                                if (s_st_ok) {
                                    static const char* kAct[] = {"circulando", "acelerando", "frenando", "en cola",
                                                                 "semaforo en rojo", "cediendo el paso", "en la parada",
                                                                 "saliendo / estacionando", "fin del camino",
                                                                 "esperando sitio a la salida"};
                                    const int a = s_st.action >= 0 && s_st.action < 10 ? s_st.action : 0;
                                    const ImVec4 col = a == 2 || a == 4 ? ImVec4(1.f, 0.45f, 0.35f, 1.f)
                                                       : a == 1           ? ImVec4(0.45f, 1.f, 0.5f, 1.f)
                                                       : a >= 3           ? ImVec4(1.f, 0.85f, 0.3f, 1.f)
                                                                          : ImVec4(0.85f, 0.85f, 0.85f, 1.f);
                                    ImGui::TextColored(col, "Accion: %s", kAct[a]);
                                    if (s_st.stopped_s > 0.5f) {
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("(parado %.0f s)", s_st.stopped_s);
                                    }
                                    if (s_st.why[0])
                                        ImGui::TextDisabled("Lo frena: %s a %.0f m%s", s_st.why, s_st.why_dist,
                                                            s_st.waits_on ? "  (espera a otro vehiculo)" : "");
                                    if (s_st.phase[0]) ImGui::Text("Servicio: %s", s_st.phase);
                                }
                            }
                            // LOD -> model -> textures of each part of its type (only the first LOD is on the GPU)
                            if (ImGui::TreeNode("Modelo (LOD / mallas / texturas)")) {
                                std::unordered_set<std::string> parts_shown;
                                for (int k = 0; k < static_cast<int>(car_it->second.chunks.size()); ++k) {
                                    char pf2[1024]{};
                                    if (!oo_traffic_car_part_file(ai_traffic, ai_i, k, pf2, sizeof(pf2))) continue;
                                    const std::string key = ai_traffic_lower(pf2);
                                    if (!parts_shown.insert(key).second) continue;
                                    const auto pit = ai_traffic_protos.find(key);
                                    if (pit == ai_traffic_protos.end()) continue;
                                    const auto& pr = pit->second;
                                    if (!ImGui::TreeNode(pr.name.c_str(), "%s (%zu mallas en GPU)", pr.name.c_str(), pr.all.size()))
                                        continue;
                                    if (pr.model && pr.model->has_value()) {
                                        const auto& md = **pr.model;
                                        ImGui::TextDisabled("%s", omsi::path_text(md.cfg_path.filename()).c_str());
                                        const auto* prim = omsi::model_cfg::primary_lod(md);
                                        for (std::size_t li = 0; li < md.lods.size(); ++li) {
                                            const auto& lod = md.lods[li];
                                            const bool loaded = &lod == prim;
                                            ImGui::PushID(static_cast<int>(li));
                                            if (ImGui::TreeNode("lod", "LOD %zu  (screensize %.2f)  %zu mallas  %s", li,
                                                                lod.distance, lod.meshes.size(), loaded ? "[CARGADO]" : "")) {
                                                for (std::size_t mi = 0; mi < lod.meshes.size(); ++mi) {
                                                    const auto& me = lod.meshes[mi];
                                                    if (me.mesh_file.empty()) continue;
                                                    ImGui::PushID(static_cast<int>(mi));
                                                    if (ImGui::TreeNode("m", "%s (%zu texturas)", me.mesh_file.c_str(), me.materials.size())) {
                                                        for (const auto& mt : me.materials)
                                                            ImGui::BulletText("%s%s", mt.diffuse_texture.c_str(),
                                                                              mt.is_matl_change ? "  (matl_change)" : "");
                                                        ImGui::TreePop();
                                                    }
                                                    ImGui::PopID();
                                                }
                                                ImGui::TreePop();
                                            }
                                            ImGui::PopID();
                                        }
                                    } else {
                                        ImGui::TextDisabled(pr.state == -1 ? "no se pudo leer" : "leyendo...");
                                    }
                                    ImGui::TreePop();
                                }
                                ImGui::TreePop();
                            }
                            if (ImGui::Button("Borrar auto")) {
                                oo_traffic_remove_car(ai_traffic, ai_selected_id);
                                panel.set_status("Auto IA borrado");
                                open = false;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(ai_follow_id == ai_selected_id ? "Dejar de seguir" : "Seguir")) {
                                if (ai_follow_id == ai_selected_id) ai_follow_stop();
                                else ai_follow_start(ai_selected_id);
                            }
                            if (ai_follow_id == ai_selected_id) ai_follow_cam_buttons();
                        }
                        ImGui::End();
                        if (!open) {
                            ai_selected_id = 0;
                            clear_hl();
                        } else if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                            oo_traffic_remove_car(ai_traffic, ai_selected_id);
                            panel.set_status("Auto IA borrado");
                            ai_selected_id = 0;
                            clear_hl();
                        }
                    }
                }
                // Feditor update: looked up once at start-up (GitHub releases); a newer release asks to update
                {
                    static bool s_upd_started = false;
                    if (!s_upd_started) {
                        s_upd_started = true;
                        omsi::updater::start_check();
                    }
                    using omsi::updater::State;
                    // (FEDITOR_UPDATE_AUTO=1: accept by itself - for testing the update path)
                    static const bool s_upd_auto = [] {
                        const char* e = std::getenv("FEDITOR_UPDATE_AUTO");
                        return e && *e == '1';
                    }();
                    if (s_upd_auto && omsi::updater::status().state == State::Available)
                        omsi::updater::start_download_and_install();
                    const auto us = omsi::updater::status();
                    const bool show = !omsi::updater::dismissed() &&
                                      (us.state == State::Available || us.state == State::Downloading ||
                                       us.state == State::ReadyToRestart ||
                                       (us.state == State::Failed && !us.latest.empty()));
                    if (show) {
                        const ImGuiViewport* vp = ImGui::GetMainViewport();
                        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.35f),
                                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                        ImGui::SetNextWindowBgAlpha(0.96f);
                        if (ImGui::Begin("Actualizacion de Feditor###feditor_update", nullptr,
                                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::Text("Hay una version nueva de Feditor: %s  (tenes %s)", us.latest.c_str(),
                                        omsi::kEditorVersion);
                            if (!us.notes.empty()) {
                                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460.f);
                                ImGui::TextDisabled("%s", us.notes.c_str());
                                ImGui::PopTextWrapPos();
                            }
                            ImGui::Separator();
                            if (us.state == State::Available) {
                                ImGui::TextUnformatted("Se descarga, el editor se cierra y vuelve a abrirse actualizado.");
                                if (ImGui::Button("Actualizar y reiniciar")) omsi::updater::start_download_and_install();
                                ImGui::SameLine();
                                if (ImGui::Button("Ahora no")) omsi::updater::dismiss();
                            } else if (us.state == State::Downloading) {
                                ImGui::TextUnformatted("Descargando...");
                                ImGui::ProgressBar(us.progress, ImVec2(360.f, 0.f));
                            } else if (us.state == State::ReadyToRestart) {
                                static bool s_launched = false;
                                if (!s_launched) {
                                    s_launched = true;
                                    if (omsi::updater::launch_installer()) {
                                        panel.set_status("Actualizando Feditor: se reabre en unos segundos");
                                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                                    }
                                }
                                ImGui::TextUnformatted("Instalando: el editor se cierra y vuelve a abrirse.");
                            } else {
                                ImGui::Text("No se pudo actualizar: %s", us.error.c_str());
                                if (ImGui::Button("Cerrar")) omsi::updater::dismiss();
                            }
                        }
                        ImGui::End();
                    }
                }
                // walk mode: the controls as text at the bottom, and the crosshair of walk-edit
                if (coop_walk_active) {
                    ImDrawList* fg = ImGui::GetForegroundDrawList();
                    const ImGuiViewport* vp = ImGui::GetMainViewport();
                    const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
                    if (coop_walk_edit && !coop_walk_cursor) {
                        const ImU32 col = walk_hold_id != 0 ? IM_COL32(255, 210, 60, 230) : IM_COL32(255, 255, 255, 210);
                        fg->AddLine(ImVec2(c.x - 9.f, c.y), ImVec2(c.x - 3.f, c.y), col, 2.f);
                        fg->AddLine(ImVec2(c.x + 3.f, c.y), ImVec2(c.x + 9.f, c.y), col, 2.f);
                        fg->AddLine(ImVec2(c.x, c.y - 9.f), ImVec2(c.x, c.y - 3.f), col, 2.f);
                        fg->AddLine(ImVec2(c.x, c.y + 3.f), ImVec2(c.x, c.y + 9.f), col, 2.f);
                    }
                    const char* hint =
                        !coop_walk_edit
                            ? "PEATON  |  WASD caminar  Shift correr  Espacio saltar  mouse mirar  E cursor  F7 salir  Esc salir"
                        : coop_walk_cursor
                            ? "PEATON EDITOR (cursor)  |  usa la interfaz con el mouse  WASD caminar  E volver a mirar  F7 solo peaton"
                        : walk_hold_bus
                            ? "LLEVANDO EL BUS  |  apunta donde va  rueda girar  click / click derecho soltar"
                        : walk_hold_id != 0
                            ? "LLEVANDO OBJETO  |  apunta donde va  click / click derecho soltar  Supr borrar  R rotar  Ctrl+Z deshacer"
                            : "PEATON EDITOR  |  click seleccionar  G mover  X/Y/Z eje  F6 local/global  R rotar  Supr borrar  "
                              "Ctrl+Z/Y deshacer/rehacer  E cursor  F7 solo peaton / subir al bus";
                    const ImVec2 ts = ImGui::CalcTextSize(hint);
                    const ImVec2 tp(c.x - ts.x * 0.5f, vp->Pos.y + vp->Size.y - 120.f);
                    fg->AddRectFilled(ImVec2(tp.x - 10.f, tp.y - 5.f), ImVec2(tp.x + ts.x + 10.f, tp.y + ts.y + 5.f),
                                      IM_COL32(0, 0, 0, 150), 6.f);
                    fg->AddText(tp, IM_COL32(255, 255, 255, 235), hint);
                }
                // The selected object's .sco: meshes, LOD bands, vertices / triangles, materials, size and flags
                // (read once per .sco; the window comes back with every new selection after being closed).
                // Each LOD is a list item (vertices / triangles / meshes) with its materials under it; hovering a
                // material shows its texture (decoded on a worker the first time). Docked in the float layout.
                {
                    struct ScoMat {
                        std::string name;           // texture as the mesh names it
                        std::filesystem::path file; // resolved file ('' = not found)
                    };
                    struct ScoModel { // one [mesh] entry: its file, size and textures
                        std::string file;
                        bool is_x = false;
                        std::uint64_t verts = 0, tris = 0;
                        std::vector<ScoMat> mats;
                    };
                    struct ScoLod {
                        int meshes = 0;
                        float screensize = 0.f;
                        std::uint64_t verts = 0, tris = 0;
                        std::vector<ScoModel> models;
                    };
                    struct ScoStats {
                        int meshes = 0, o3d = 0, x = 0;
                        std::uint64_t verts = 0, tris = 0;
                        std::map<int, ScoLod> lods; // lod index -> band
                        std::vector<ScoMat> textures; // every material texture of the object, once
                        std::string flags, name;
                        float bw = 0.f, bl = 0.f, bh = 0.f;
                    };
                    static std::unordered_map<std::wstring, ScoStats> s_sco_stats;
                    static std::int32_t s_info_obj = -1;
                    static bool s_info_open = true;
                    const bool one_sel = selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size();
                    if (one_sel) {
                        const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
                        if (o.id != s_info_obj) {
                            s_info_obj = o.id;
                            s_info_open = true;
                        }
                        auto it = s_sco_stats.find(o.sco_path.native());
                        if (it == s_sco_stats.end()) {
                            ScoStats st;
                            st.name = omsi::path_text(o.sco_path.filename());
                            if (const sco::SceneryObject* so = cached_sco(o.sco_path)) {
                                std::set<std::string> all_tex;
                                auto add_mat = [&](ScoModel& md, const std::string& tex, const std::filesystem::path& file) {
                                    if (tex.empty()) return;
                                    bool dup = false;
                                    for (const auto& m : md.mats) dup |= m.name == tex;
                                    if (!dup) md.mats.push_back({tex, file});
                                    if (all_tex.insert(tex).second) st.textures.push_back({tex, file});
                                };
                                for (const auto& m : so->meshes) {
                                    ++st.meshes;
                                    auto& lb0 = st.lods[m.lod_index];
                                    ++lb0.meshes;
                                    lb0.screensize = m.lod_screensize;
                                    lb0.models.push_back({});
                                    auto& lb = lb0.models.back(); // (named lb: the counters below are per model)
                                    lb.file = omsi::path_text(m.mesh_file.filename());
                                    lb.is_x = !m.is_o3d;
                                    if (!m.is_o3d) {
                                        ++st.x;
                                        auto xf = xfile::load_x_file(m.mesh_file);
                                        if (!xf || xf->parts.empty()) {
                                            const auto alt = o.sco_path.parent_path() / "model" / m.mesh_file.filename();
                                            xf = xfile::load_x_file(alt);
                                        }
                                        if (xf) {
                                            for (const auto& part : xf->parts) {
                                                lb.verts += part.vertices.size();
                                                lb.tris += part.indices.size() / 3;
                                                add_mat(lb, part.texture_filename,
                                                        resolve_x_tex(m.mesh_file, o.sco_path, part.texture_filename));
                                            }
                                        }
                                        continue;
                                    }
                                    ++st.o3d;
                                    if (const auto mp = o3d::load_mapped_shared(m.mesh_file); mp && mp->valid()) {
                                        lb.verts += mp->mesh().vertex_count;
                                        lb.tris += mp->mesh().triangle_count;
                                        for (const auto& mat : mp->mesh().parse_materials()) {
                                            std::filesystem::path file;
                                            if (!mat.texture.empty())
                                                if (const auto r = gfx::dx12::resolve_texture_path_day(m.mesh_file, mat.texture,
                                                                                                       cfg.omsi_root))
                                                    file = *r;
                                            add_mat(lb, mat.texture, file);
                                        }
                                    }
                                }
                                for (const auto& t : so->material_textures)
                                    if (!t.empty() && all_tex.insert(t).second) {
                                        std::filesystem::path file;
                                        if (const auto r = gfx::dx12::resolve_texture_path_day(o.sco_path, t, cfg.omsi_root))
                                            file = *r;
                                        st.textures.push_back({t, file});
                                    }
                                for (auto& [li, lb] : st.lods) {
                                    for (const auto& md : lb.models) {
                                        lb.verts += md.verts;
                                        lb.tris += md.tris;
                                    }
                                    st.verts += lb.verts;
                                    st.tris += lb.tris;
                                }
                                if (so->has_boundingbox) {
                                    st.bw = so->bbox_w;
                                    st.bl = so->bbox_l;
                                    st.bh = so->bbox_h;
                                }
                                if (so->fixed) st.flags += "[fixed] ";
                                if (so->surface) st.flags += "[surface] ";
                                if (so->no_collision) st.flags += "[nocollision] ";
                                if (so->only_editor_mesh) st.flags += "[onlyeditor] ";
                                if (so->tree.valid) st.flags += "[tree] ";
                                if (so->special == sco::ScoSpecial::Carpark) st.flags += "[carpark_p] ";
                            }
                            if (s_sco_stats.size() > 512) s_sco_stats.clear();
                            it = s_sco_stats.emplace(o.sco_path.native(), std::move(st)).first;
                        }
                        if (s_info_open) {
                            const auto& st = it->second;
                            // a material row: its texture's preview while hovered
                            auto mat_row = [&](const ScoMat& m, const char* id_suffix) {
                                ImGui::PushID(id_suffix);
                                ImGui::PushID(m.name.c_str());
                                if (m.file.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.4f, 1.f));
                                ImGui::BulletText("%s", m.name.c_str());
                                if (m.file.empty()) ImGui::PopStyleColor();
                                if (ImGui::IsItemHovered()) {
                                    ImGui::BeginTooltip();
                                    if (m.file.empty()) {
                                        ImGui::TextUnformatted("Textura no encontrada");
                                    } else {
                                        const auto pv = ui::texture_preview(m.file);
                                        if (pv.id != 0 && pv.w > 0 && pv.h > 0) {
                                            ImGui::Image(static_cast<ImTextureID>(pv.id),
                                                         ImVec2(static_cast<float>(pv.w), static_cast<float>(pv.h)));
                                        } else if (pv.failed) {
                                            ImGui::TextUnformatted("No se pudo leer la textura");
                                        } else {
                                            ImGui::TextDisabled("Cargando miniatura...");
                                        }
                                        ImGui::TextDisabled("%s", omsi::path_text(m.file.filename()).c_str());
                                    }
                                    ImGui::EndTooltip();
                                }
                                ImGui::PopID();
                                ImGui::PopID();
                            };
                            ui::float_slot_begin("sco_info", 20);
                            ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 0.f), ImVec2(420.f, 560.f));
                            ImGui::SetNextWindowBgAlpha(0.92f);
                            const bool info_drawn = ImGui::Begin("Info .sco###sco_info_win", &s_info_open,
                                                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
                            ui::float_slot_end();
                            if (info_drawn) {
                                ImGui::TextUnformatted(st.name.c_str());
                                ImGui::Text("Id %d   Mallas: %d (%d .o3d, %d .x)", o.id, st.meshes, st.o3d, st.x);
                                ImGui::Text("Vertices: %llu   Triangulos: %llu", static_cast<unsigned long long>(st.verts),
                                            static_cast<unsigned long long>(st.tris));
                                // the band the distance cull draws right now (setup_06 keeps it per object; one
                                // lookup, nothing recomputed)
                                int lod_now = st.lods.empty() ? 0 : st.lods.begin()->first;
                                if (const auto ds = s_dc_obj_slot.find(o.id); ds != s_dc_obj_slot.end() &&
                                                                              ds->second < s_dc_objs.size() &&
                                                                              s_dc_objs[ds->second].bands.size() > 1)
                                    lod_now = s_dc_objs[ds->second].pick;
                                ImGui::SeparatorText("LODs");
                                for (const auto& [li, lb] : st.lods) {
                                    char hdr[180]{};
                                    std::snprintf(hdr, sizeof(hdr), "LOD %d  -  %llu vertices, %llu tris, %d malla(s)%s###lod%d", li,
                                                  static_cast<unsigned long long>(lb.verts),
                                                  static_cast<unsigned long long>(lb.tris), lb.meshes,
                                                  li == lod_now && st.lods.size() > 1 ? "   << visible" : "", li);
                                    if (li == lod_now && st.lods.size() > 1)
                                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.5f, 1.f));
                                    const bool lod_open =
                                        ImGui::TreeNodeEx(hdr, li == st.lods.begin()->first ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                                    if (li == lod_now && st.lods.size() > 1) ImGui::PopStyleColor();
                                    if (ImGui::IsItemHovered() && lb.screensize > 0.f)
                                        ImGui::SetTooltip("screensize %.3f", lb.screensize);
                                    if (lod_open) {
                                        // LOD -> model (.o3d / .x) -> its textures
                                        for (std::size_t mi = 0; mi < lb.models.size(); ++mi) {
                                            const auto& md = lb.models[mi];
                                            char mhdr[200]{};
                                            std::snprintf(mhdr, sizeof(mhdr), "%s  -  %llu v, %llu t###mdl%d_%zu", md.file.c_str(),
                                                          static_cast<unsigned long long>(md.verts),
                                                          static_cast<unsigned long long>(md.tris), li, mi);
                                            if (ImGui::TreeNodeEx(mhdr, lb.models.size() == 1 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                                                if (md.mats.empty()) ImGui::TextDisabled("(sin texturas)");
                                                for (const auto& m : md.mats) mat_row(m, mhdr);
                                                ImGui::TreePop();
                                            }
                                        }
                                        ImGui::TreePop();
                                    }
                                }
                                char mh[64]{};
                                std::snprintf(mh, sizeof(mh), "Materiales (%zu)###sco_mats", st.textures.size());
                                if (!st.textures.empty() && ImGui::CollapsingHeader(mh)) {
                                    for (const auto& m : st.textures) mat_row(m, "all");
                                }
                                if (st.bw > 0.f || st.bl > 0.f || st.bh > 0.f)
                                    ImGui::Text("Bounding box: %.2f x %.2f x %.2f m", st.bw, st.bl, st.bh);
                                if (!st.flags.empty()) ImGui::TextWrapped("%s", st.flags.c_str());
                                ImGui::Text("Pos: %.2f, %.2f, %.2f  Rot: %.1f", o.position.x, o.position.y, o.position.z,
                                            o.rotation_deg.x);
                            }
                            ImGui::End();
                        }
                    } else {
                        s_info_obj = -1;
                    }
                }
            }
            {
                auto _w_hud = main_watchdog.phase("imgui.end_frame_and_build");
                auto _perf_hud = frame_profiler.scope(MapEditorFrameProfiler::HudBuild);
                imgui_overlay.end_frame_and_build();
            }
            {
                auto _w_cat = main_watchdog.phase("imgui.catalog_events");
                // Ultralight/ImGui clicks arrive after take_events(); drain at frame end.
                const auto catalog_ev = panel.drain_catalog_pending();
                open_catalog_browser_from_cev(catalog_ev);
                handle_catalog_panel_events(catalog_ev);
            }
            frame_profiler.end_segment(MapEditorFrameProfiler::ImGuiBuild);
        }

        {
            // Path editor panel requests. The ImGui tab mutates the shared PathEditorState directly;
            // actions that need an undo snapshot are requested here so we can push it *before* the
            // mutation. Then any pending rail rebuild is flushed.
            auto _w_sco = main_watchdog.phase("path_editor_actions");
            if (sco_path_edit.active) {
                if (sco_path_edit.req_new) sco_start_new_path();
                if (sco_path_edit.req_duplicate) sco_clone_sel_path();
                if (sco_path_edit.req_delete) {
                    std::vector<int> del = sco_path_edit.selected;
                    if (del.empty() && sco_path_edit.has_index(sco_path_edit.sel))
                        del.push_back(sco_path_edit.sel);
                    if (!del.empty()) {
                        push_undo("sco_path_del");
                        std::sort(del.begin(), del.end(), std::greater<int>());
                        for (int i : del)
                            if (sco_path_edit.has_index(i))
                                sco_path_edit.paths.erase(sco_path_edit.paths.begin() + i);
                        sco_path_edit.selected.clear();
                        sco_path_edit.sel = -1;
                        sco_path_edit.dirty = true;
                        sco_path_edit.rebuild_requested = true;
                    }
                }
                if (sco_path_edit.req_copy) {
                    sco_path_edit.clip.clear();
                    if (!sco_path_edit.selected.empty()) {
                        for (int i : sco_path_edit.selected)
                            if (sco_path_edit.has_index(i)) sco_path_edit.clip.push_back(sco_path_edit.paths[i]);
                    } else {
                        sco_path_edit.clip = sco_path_edit.paths;
                    }
                    panel.set_status("Copiados " + std::to_string(sco_path_edit.clip.size()) + " paths");
                }
                if (sco_path_edit.req_paste && !sco_path_edit.clip.empty()) {
                    push_undo("sco_path_paste");
                    for (auto p : sco_path_edit.clip) sco_path_edit.paths.push_back(std::move(p));
                    sco_path_edit.select_only(static_cast<int>(sco_path_edit.paths.size()) - 1);
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
                    sco_path_edit.rebuild_requested = true;
                    sco_path_arm_live();
                }
                if (sco_path_edit.req_save) sco_path_save();
                if (sco_path_edit.req_exit) try_exit_sco_path_edit();
                if (sco_path_edit.req_undo && !undo_stack.empty()) {
                    redo_stack.push_back(capture_editor_snapshot());
                    auto s = undo_stack.back();
                    undo_stack.pop_back();
                    apply_editor_snapshot_safe(std::move(s));
                }
                if (sco_path_edit.req_redo && !redo_stack.empty()) {
                    undo_stack.push_back(capture_editor_snapshot());
                    auto s = redo_stack.back();
                    redo_stack.pop_back();
                    apply_editor_snapshot_safe(std::move(s));
                }
                sco_path_edit.req_new = sco_path_edit.req_duplicate = sco_path_edit.req_delete = false;
                sco_path_edit.req_copy = sco_path_edit.req_paste = false;
                sco_path_edit.req_save = sco_path_edit.req_exit = sco_path_edit.req_snap_to_spline = false;
                sco_path_edit.req_undo = sco_path_edit.req_redo = false;
                if (sco_path_edit.rebuild_requested) {
                    sco_path_edit.rebuild_requested = false;
                    rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
                }
            }
        }

        {
            auto _watch_render = main_watchdog.phase("render_cpu_dx12");
            auto _perf = frame_profiler.scope(MapEditorFrameProfiler::Render);
            if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: pre_render");
            try {
                // The clock runs (Vista > El tiempo corre), x time_speed; past midnight the date moves on.
                {
                    auto& stw = panel.imgui_state();
                    static double s_clock_last = glfwGetTime();
                    const double clock_now = glfwGetTime();
                    double clock_dt = clock_now - s_clock_last;
                    s_clock_last = clock_now;
                    if (!(clock_dt > 0.0) || clock_dt > 0.5) clock_dt = 0.0;
                    if (stw.time_running && clock_dt > 0.0) {
                        stw.sim_hour += static_cast<float>(clock_dt * static_cast<double>(stw.time_speed) / 3600.0);
                        while (stw.sim_hour >= 24.f) {
                            stw.sim_hour -= 24.f;
                            static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                            const bool leap = (stw.sim_year % 4 == 0 && stw.sim_year % 100 != 0) || stw.sim_year % 400 == 0;
                            const int dim = kDays[(std::clamp)(stw.sim_month, 1, 12) - 1] + (stw.sim_month == 2 && leap ? 1 : 0);
                            if (++stw.sim_day > dim) {
                                stw.sim_day = 1;
                                if (++stw.sim_month > 12) {
                                    stw.sim_month = 1;
                                    ++stw.sim_year;
                                }
                            }
                        }
                    }
                    // chrono scenarios for the Vista stepper (by start date), once
                    static bool s_chrono_ui = false;
                    if (!s_chrono_ui) {
                        s_chrono_ui = true;
                        std::vector<std::pair<int, std::string>> ev;
                        for (const auto& sc : chrono_scenarios)
                            if (sc.start_date != 0 || sc.end_date != 0)
                                ev.push_back({sc.start_date != 0 ? sc.start_date : sc.end_date, sc.name});
                        std::stable_sort(ev.begin(), ev.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                        stw.chrono_event_dates.clear();
                        stw.chrono_event_names.clear();
                        for (auto& e : ev) {
                            stw.chrono_event_dates.push_back(e.first);
                            stw.chrono_event_names.push_back(std::move(e.second));
                        }
                        stw.chrono_date = editor_ui_settings.chrono_date;
                    }
                    if (stw.chrono_pick >= 0) {
                        const int pick = stw.chrono_pick;
                        stw.chrono_pick = -1;
                        const int new_date = (pick == 0 || pick > static_cast<int>(stw.chrono_event_dates.size()))
                                                 ? 0
                                                 : stw.chrono_event_dates[static_cast<std::size_t>(pick - 1)];
                        if (new_date != editor_ui_settings.chrono_date) {
                            if (!dirty_tile_keys.empty()) {
                                panel.set_status("Cronos: guardá o descartá los cambios del mapa antes de cambiar de crono");
                            } else {
                                if (new_date != 0) {
                                    stw.sim_year = new_date / 10000;
                                    stw.sim_month = (std::clamp)((new_date / 100) % 100, 1, 12);
                                    stw.sim_day = (std::clamp)(new_date % 100, 1, 31);
                                }
                                stw.chrono_date = new_date;
                                persist_ui_settings();
                                editor_ui_settings.chrono_date = new_date;
                                editor_ui_settings.chrono_map = omsi::path_utf8(cfg.global.root);
                                if (!map::save_editor_ui_settings(cfg.global.root, editor_ui_settings))
                                    log::warn("chrono: settings save failed");
                                // reload the same map (same entry point) with the scenarios of the new date
                                std::filesystem::path reload_cfg = cfg.global.global_cfg_path;
                                if (cfg.workspace && !cfg.workspace->actual_dir.empty()) {
                                    // (as Archivo > Cargar mapa: the map's own folder, not the *_actual copy)
                                    const auto live = map::workspace_live_map_dir(*cfg.workspace);
                                    if (!live.empty()) reload_cfg = live / "global.cfg";
                                }
                                if (cfg.switch_map_out) *cfg.switch_map_out = reload_cfg;
                                if (cfg.switch_omsi_out) *cfg.switch_omsi_out = cfg.omsi_root;
                                if (cfg.switch_entry_out) *cfg.switch_entry_out = cfg.entry_index;
                                if (cfg.switch_view_only_out) *cfg.switch_view_only_out = cfg.view_only;
                                log::info("chrono: preview date " + std::to_string(new_date) + " -> reload map");
                                glfwSetWindowShouldClose(window, GLFW_TRUE);
                            }
                        }
                    }
                }
                // Vista sim time → sky / season / sun (no openOMSI lighting port).
                {
                    const auto& st = panel.imgui_state();
                    const int doy = omsi::sky::day_of_year(st.sim_month, st.sim_day);
                    // Where the map lies on the Earth: a [worldcoordinates] map's tiles are Web-Mercator z16
                    // tiles (map_geo_tools), so its first tile gives latitude / longitude; any other map keeps
                    // evaluate's default, Berlin (OMSI's home - its stock maps carry no position).
                    static std::filesystem::path s_geo_map;
                    static float s_lat = 0.f, s_lon = 0.f;
                    if (s_geo_map != cfg.global.global_cfg_path) {
                        s_geo_map = cfg.global.global_cfg_path;
                        s_lat = s_lon = 0.f;
                        if (cfg.global.world_coordinates && !cfg.global.tiles.empty()) {
                            const auto& t0 = cfg.global.tiles.front();
                            // (map_geo_tools latlon_from_world_grid: z16 tile x = grid_x + 32768, y = 32767 - grid_y)
                            const double n16 = 65536.0;
                            const double tx = static_cast<double>(t0.grid_x) + 32768.0 + 0.5;
                            const double ty = 32767.0 - static_cast<double>(t0.grid_y) + 0.5;
                            s_lon = static_cast<float>(tx / n16 * 360.0 - 180.0);
                            s_lat = static_cast<float>(std::atan(std::sinh(3.14159265358979 * (1.0 - 2.0 * ty / n16))) * 180.0 /
                                                       3.14159265358979);
                            if (!(std::fabs(s_lat) <= 85.f) || !(std::fabs(s_lon) <= 180.f)) s_lat = s_lon = 0.f;
                        }
                        log::info("sky: sun / moon for lat " + std::to_string(s_lat == 0.f && s_lon == 0.f ? 52.505f : s_lat) +
                                  " lon " + std::to_string(s_lat == 0.f && s_lon == 0.f ? 13.2782f : s_lon));
                    }
                    const auto sky =
                        omsi::sky::evaluate(doy, st.sim_hour, s_lat, s_lon, st.timezone_h, st.weather_id);
                    omsi::gfx::set_texture_season(sky.season_dir, sky.prefer_night, sky.night_blend);
                    static std::uint64_t s_season_gen = 0;
                    const std::uint64_t gen = [&]() {
                        std::lock_guard<std::mutex> lk(omsi::gfx::texture_season_mu());
                        return omsi::gfx::texture_season_state().generation;
                    }();
                    if (gen != s_season_gen) {
                        s_season_gen = gen;
                        // Spread season/night rebinds across frames — never sync-reload all slots.
                        renderer.begin_season_texture_rebind();
                    }
                    (void)renderer.pump_season_texture_rebinds(3);
                    (void)renderer.pump_late_textures(4);
                    // Night map fade (OMSI night blend) + vegetation wind. Wind direction/strength live in the
                    // shared omsi::wind state; the weather id picks the default strength (m/s).
                    {
                        static int s_wind_wid = -1;
                        static const float kWindMs[4] = {1.5f, 3.5f, 6.0f, 4.5f}; // clear, cloudy, rain, snow
                        const int wid = (st.weather_id >= 0 && st.weather_id < 4) ? st.weather_id : 0;
                        if (wid != s_wind_wid) {
                            s_wind_wid = wid;
                            omsi::wind::set(0.8f, 0.6f, kWindMs[wid]);
                        }
                        const auto w = omsi::wind::snapshot();
                        renderer.set_scene_night_wind(sky.night_blend, w.dir_x * w.strength_ms, w.dir_y * w.strength_ms);
                    }
                    {
                        // Scenery lights at night: the [maplight]s of the placed objects (openOMSI lights the
                        // street with them; [light_enh] are coronas), the 64 nearest to the camera.
                        using SceneLightT = std::remove_reference_t<decltype(renderer)>::SceneLight;
                        static std::vector<SceneLightT> s_obj_lights;
                        static double s_obj_lights_t = -1.0;
                        const double now_l = glfwGetTime();
                        if (sky.night_blend < 0.03f) {
                            s_obj_lights.clear();
                        // (glfw time starts again at 0 when another map is loaded in this process: the last
                        //  refresh time of the old map lay in the future and the lights never refreshed)
                        } else if (now_l - s_obj_lights_t > 0.3 || now_l < s_obj_lights_t) {
                            s_obj_lights_t = now_l;
                            s_obj_lights.clear();
                            float cex = 0.f, cey = 0.f, cez = 0.f;
                            camera.eye_position(cex, cey, cez);
                            // every light of the view radius (300 / 2000 m...), the nearest 512 (the renderer
                            // bins them in a world grid: a pixel only loops the lights that reach it)
                            const float kLightReach =
                                editor_visual_radius_m > 0.f ? (std::max)(editor_visual_radius_m, 150.f) : 2000.f;
                            // per object, its lights in world space are kept until it moves (building them -
                            // the .sco lookup, the world matrix, every [maplight] - for each object in a
                            // 2 km radius three times a second was a spike of its own)
                            struct XLightC {
                                float x, y, z, range, r, g, b;
                                int le; // >= 0: [light_enh] index whose variable switches it
                            };
                            struct XObjL {
                                bool valid = false;
                                float tx = 0.f, ty = 0.f, tz = 0.f;
                                std::vector<XLightC> v;
                            };
                            static std::unordered_map<std::int32_t, XObjL> s_xl_cache;
                            if (s_xl_cache.size() > 200000) s_xl_cache.clear();
                            ensure_edit_obj_index();
                            std::unordered_set<std::int32_t> done;
                            struct Cand {
                                float d2;
                                SceneLightT l;
                            };
                            std::vector<Cand> cands;
                            for (const auto& sp : scene_places) {
                                if (!sp.alive || sp.is_tree) continue;
                                const float dx = sp.tx - cex, dy = sp.ty - cey;
                                if (dx * dx + dy * dy > kLightReach * kLightReach) continue;
                                if (!done.insert(sp.object_id).second) continue;
                                XObjL& ce = s_xl_cache[sp.object_id];
                                if (!ce.valid || ce.tx != sp.tx || ce.ty != sp.ty || ce.tz != sp.tz) {
                                    ce.valid = true;
                                    ce.tx = sp.tx;
                                    ce.ty = sp.ty;
                                    ce.tz = sp.tz;
                                    ce.v.clear();
                                    const auto it = edit_obj_by_id.find(sp.object_id);
                                    if (it == edit_obj_by_id.end() || !it->second) {
                                        ce.valid = false; // (not indexed yet: try again next time)
                                        continue;
                                    }
                                    const auto& o = *it->second;
                                    const sco::SceneryObject* sc = cached_sco(o.sco_path);
                                    if (!sc) {
                                        ce.valid = false;
                                        continue;
                                    }
                                    if (sc->maplights.empty() && sc->lights.empty()) continue;
                                    const XMMATRIX wm = sco_world_matrix(o, nullptr, sp.tz - o.position.z, 0.f, 0.f);
                                    for (std::size_t k = 0; k < sc->maplights.size(); ++k) {
                                        const auto& ml = sc->maplights[k];
                                        bool dup = false;
                                        for (std::size_t q = 0; q < k && !dup; ++q) {
                                            const auto& o2 = sc->maplights[q];
                                            dup = o2.position.x == ml.position.x && o2.position.y == ml.position.y &&
                                                  o2.position.z == ml.position.z && o2.radius == ml.radius;
                                        }
                                        if (dup) continue;
                                        DirectX::XMFLOAT3 w{};
                                        DirectX::XMStoreFloat3(&w, xform_point(wm, ml.position.x, ml.position.y, ml.position.z));
                                        // (openOMSI Enhanced picture: a [maplight] of up to 25 m - street lamps 8-20,
                                        //  a canopy 10-20, a floodlight mast 20 - fills its declared core at full
                                        //  colour; only a building's window glow, 60 / 120 m, is held to 15 m at a
                                        //  fifth. x2: the night picture is lit by these pools)
                                        const bool window_glow = ml.radius > 25.f;
                                        const float glow = (window_glow ? 0.2f * 0.75f : 0.75f) * 2.f;
                                        ce.v.push_back({w.x, w.y, w.z, window_glow ? 15.f : (std::max)(ml.radius, 0.5f),
                                                        ml.color.x * glow, ml.color.y * glow, ml.color.z * glow, -1});
                                    }
                                    // [light_enh] coronas of an object without [maplight]s (traffic lights, signs):
                                    // a small glow in their colour, on while their variable is
                                    if (sc->maplights.empty()) {
                                        for (std::size_t k = 0; k < sc->lights.size(); ++k) {
                                            const auto& le = sc->lights[k];
                                            DirectX::XMFLOAT3 w{};
                                            DirectX::XMStoreFloat3(&w, xform_point(wm, le.position.x, le.position.y,
                                                                                   le.position.z));
                                            ce.v.push_back({w.x, w.y, w.z, (std::clamp)(le.size * 3.f, 0.4f, 4.f),
                                                            le.color.x * 1.5f, le.color.y * 1.5f, le.color.z * 1.5f,
                                                            le.variable.empty() ? -2 : static_cast<int>(k)});
                                        }
                                    }
                                }
                                if (ce.v.empty()) continue;
                                const sco::SceneryObject* sc_vars = nullptr;
                                for (const auto& c : ce.v) {
                                    float on = 1.f;
                                    if (c.le >= 0) {
                                        if (!sc_vars) {
                                            const auto it = edit_obj_by_id.find(sp.object_id);
                                            if (it != edit_obj_by_id.end() && it->second)
                                                sc_vars = cached_sco(it->second->sco_path);
                                            if (!sc_vars) break;
                                        }
                                        if (static_cast<std::size_t>(c.le) >= sc_vars->lights.size()) continue;
                                        on = scenery_scripts.float_var(sp.object_id, sc_vars->lights[static_cast<std::size_t>(c.le)].variable);
                                        if (!(on > 0.05f)) continue;
                                        on = (std::min)(on, 1.f);
                                    }
                                    SceneLightT l{};
                                    l.x = c.x;
                                    l.y = c.y;
                                    l.z = c.z;
                                    l.range = c.range;
                                    l.r = c.r * on;
                                    l.g = c.g * on;
                                    l.b = c.b * on;
                                    l.intensity = sky.night_blend;
                                    const float ex = c.x - cex, ey = c.y - cey, ez = c.z - cez;
                                    cands.push_back({ex * ex + ey * ey + ez * ez, l});
                                }
                            }
                            constexpr std::size_t kMaxLights = 512;
                            if (cands.size() > kMaxLights) {
                                std::nth_element(cands.begin(), cands.begin() + kMaxLights, cands.end(),
                                                 [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
                                cands.resize(kMaxLights);
                            }
                            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
                            for (const auto& c : cands) s_obj_lights.push_back(c.l);
                        } else {
                            for (auto& l : s_obj_lights) l.intensity = sky.night_blend;
                        }
                        // the vehicles' lit lamps (AI traffic, the bus): openOMSI evaluates them from each car's
                        // script (headlights, brake, blinkers). A glow on the lamp and the body round it, and for
                        // a headlight the pool it throws on the road ahead. Every frame: they move.
                        static std::vector<SceneLightT> s_all_lights;
                        static std::vector<float> s_lamp_buf;
                        s_all_lights.clear();
                        // the bus's saloon lamps ([interiorlight], by day as well): they light only its meshes that
                        // name them in [illumination_interior] - interior-only lights, never the street or the body
                        if (omsi_bus && omsi_bus->oo) {
                            static std::vector<float> s_in_buf(32 * 8);
                            const std::size_t nin = oo_bus_interior_lights(omsi_bus->oo, s_in_buf.data(), 32);
                            for (std::size_t i = 0; i < nin; ++i) {
                                const float* q = &s_in_buf[i * 8];
                                SceneLightT l{};
                                l.x = q[0];
                                l.y = q[1];
                                l.z = q[2];
                                // (OMSI's D3D point light 1 / (d² / range²): full within the range, the range the core)
                                l.range = q[3];
                                l.r = q[4];
                                l.g = q[5];
                                l.b = q[6];
                                l.intensity = q[7] * (0.35f + 0.65f * sky.night_blend);
                                l.interior = 1.f;
                                s_all_lights.push_back(l);
                            }
                        }
                        // coronas ([light_enh] / [light_enh_2] glows, by day as well, dimmer): the bus's and the AI
                        // cars' lit lamps, drawn as glows at the lamps (OMSI's light textures - they light nothing)
                        // (the AI cars' lit lamps, asked once a frame: the coronas and the night lamps share them)
                        static std::vector<float> s_ai_lamps;
                        std::size_t n_ai_lamps = 0;
                        {
                            constexpr std::size_t kAiLampCap = 240;
                            s_ai_lamps.resize(kAiLampCap * 10);
                            float cex = 0.f, cey = 0.f, cez = 0.f;
                            camera.eye_position(cex, cey, cez);
                            if (ai_traffic) n_ai_lamps = oo_traffic_lamps(ai_traffic, cex, cey, 260.0, s_ai_lamps.data(), kAiLampCap);
                        }
                        {
                            static std::vector<float> s_corona_buf;
                            constexpr std::size_t kCoronaCap = 96;
                            s_corona_buf.resize(kCoronaCap * 10);
                            std::size_t nco = 0;
                            if (omsi_bus && omsi_bus->oo) nco += oo_bus_coronas(omsi_bus->oo, s_corona_buf.data(), 64);
                            const std::size_t take = (std::min)(n_ai_lamps, kCoronaCap - nco);
                            if (take > 0) std::memcpy(s_corona_buf.data() + nco * 10, s_ai_lamps.data(), take * 10 * sizeof(float));
                            nco += take;
                            renderer.set_vehicle_coronas(s_corona_buf.data(), static_cast<int>(nco),
                                                         0.35f + 0.65f * sky.night_blend);
                        }
                        if (sky.night_blend > 0.05f) {
                            constexpr std::size_t kLampCap = 240;
                            s_lamp_buf.resize(kLampCap * 10);
                            std::size_t nlamps = 0;
                            // (the bus: its headlights only - the [spotlight] its Spot_Select picks; see oo_bus_lamps)
                            if (omsi_bus && omsi_bus->oo) nlamps += oo_bus_lamps(omsi_bus->oo, s_lamp_buf.data(), 48);
                            const std::size_t n_bus_lamps = nlamps;
                            {
                                const std::size_t take = (std::min)(n_ai_lamps, kLampCap - nlamps);
                                if (take > 0)
                                    std::memcpy(s_lamp_buf.data() + nlamps * 10, s_ai_lamps.data(), take * 10 * sizeof(float));
                                nlamps += take;
                            }
                            const float lamp_k = (std::max)(sky.night_blend, 0.3f);
                            int pools = 0;
                            for (std::size_t i = 0; i < nlamps; ++i) {
                                const float* q = &s_lamp_buf[i * 10];
                                if (i < n_bus_lamps) {
                                    // the player's bus: only the pool its headlight throws on the road, well ahead of
                                    // the bumper and low - no light at the lamp (in the body it lit the dash and cab),
                                    // and the pool lights neither the bus nor its saloon (interior = -1)
                                    const float hl = std::hypot(q[3], q[4]);
                                    if (hl < 0.3f) continue;
                                    SceneLightT p{};
                                    p.x = q[0] + q[3] / hl * 11.f;
                                    p.y = q[1] + q[4] / hl * 11.f;
                                    p.z = q[2] - 0.4f;
                                    p.range = 5.5f;
                                    p.r = q[6] * 0.6f;
                                    p.g = q[7] * 0.6f;
                                    p.b = q[8] * 0.6f;
                                    p.intensity = sky.night_blend;
                                    p.interior = -1.f;
                                    s_all_lights.push_back(p);
                                    continue;
                                }
                                SceneLightT l{};
                                l.x = q[0];
                                l.y = q[1];
                                l.z = q[2];
                                // (a lamp's glow: about its own size round it - 3 m spread a red pool round
                                //  every tail light)
                                l.range = (std::clamp)(q[9] * 1.6f, 0.35f, 1.3f);
                                l.r = q[6] * 1.3f;
                                l.g = q[7] * 1.3f;
                                l.b = q[8] * 1.3f;
                                l.intensity = lamp_k;
                                s_all_lights.push_back(l);
                                const float hl = std::hypot(q[3], q[4]);
                                const bool head = hl > 0.5f && q[5] > -0.6f && q[6] > 0.6f && q[7] > 0.6f && q[8] > 0.45f;
                                if (head && pools < 64 && sky.night_blend > 0.05f) {
                                    SceneLightT p = l;
                                    p.x = q[0] + q[3] / hl * 7.f;
                                    p.y = q[1] + q[4] / hl * 7.f;
                                    p.z = q[2] + 0.4f;
                                    p.range = 9.f;
                                    p.r = q[6] * 0.55f;
                                    p.g = q[7] * 0.55f;
                                    p.b = q[8] * 0.55f;
                                    p.intensity = sky.night_blend;
                                    s_all_lights.push_back(p);
                                    ++pools;
                                }
                            }
                        }
                        for (const auto& l : s_obj_lights) {
                            if (s_all_lights.size() >= 512) break;
                            s_all_lights.push_back(l);
                        }
                        renderer.set_scene_lights(s_all_lights.empty() ? nullptr : s_all_lights.data(),
                                                  static_cast<int>(s_all_lights.size()), sky.ambient_scale);
                    }
                    // Intensity tracks solar elevation so hour/sun angle changes shading.
                    const float elev = (std::max)(0.f, sky.elevation_deg);
                    const float sun_str =
                        (std::min)(0.95f, elev / 18.f) * (std::max)(0.05f, 1.f - sky.night_blend * 0.92f);
                    float cam_x = 0.f, cam_y = 0.f, cam_z = 20.f;
                    camera.eye_position(cam_x, cam_y, cam_z);
                    renderer.set_camera_world_pos(cam_x, cam_y, cam_z);
                    // Render-only distance cull: bounded when the user leaves radius at 0 so the
                    // recorder does not draw/record far scenery+splines (big CPU/GPU saving).
                    // While editing a .sco we lift the distance cull so its rails never disappear.
                    // BeamNG culls only the object AABB against the frustum and clips at the far plane
                    // (LevelInfo.visibleDistance); it has no per-object distance cull. So in BeamNG mode the
                    // render cap IS the far plane, not the streaming radius: load_radius keeps limiting what
                    // is streamed/loaded, never what is drawn. draw.cpp still exempts huge meshes (>300 m)
                    // from both the distance and the size cull, and set_luna_far_plane() is kept >= this.
                    renderer.set_cull_max_distance(sco_path_edit.active ? 1.0e5f
                                                   : cfg.beamng ? bng_visible_distance
                                                   : (editor_visual_radius_m > 0.f ? editor_visual_radius_m : 450.f));
                    renderer.set_min_screen_size_pct(editor_ui_settings.min_screen_size_pct);
                    {
                        const float fx = camera.target_x - cam_x;
                        const float fy = camera.target_y - cam_y;
                        const float fz = camera.target_z - cam_z;
                        renderer.set_camera_view(fx, fy, fz, aspect, 0.25f * 3.14159265f);
                    }
                    // The moon: where the sun stood (age / 29.53 days) x 24 h earlier - new moon with the sun,
                    // full moon opposite it - lit by its phase. At night it is THE directional light: it
                    // lights and casts shadows (much fainter than the sun, cool), when it is up.
                    float moon_x = 0.f, moon_y = 0.f, moon_z = -1.f, moon_illum = 0.f;
                    {
                        // days since the new moon of 2000-01-06 18:14 UTC (synodic month 29.530589 d)
                        auto days_from_civil = [](int y, int m, int d) {
                            y -= m <= 2;
                            const int era = (y >= 0 ? y : y - 399) / 400;
                            const int yoe = y - era * 400;
                            const int doy2 = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
                            const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy2;
                            return era * 146097 + doe - 719468;
                        };
                        const double days = static_cast<double>(days_from_civil(st.sim_year, st.sim_month, st.sim_day) -
                                                                days_from_civil(2000, 1, 6)) +
                                            (static_cast<double>(st.sim_hour) - static_cast<double>(st.timezone_h)) / 24.0 -
                                            18.23 / 24.0;
                        double age = std::fmod(days, 29.530589);
                        if (age < 0.0) age += 29.530589;
                        const double frac = age / 29.530589;
                        moon_illum = static_cast<float>(0.5 * (1.0 - std::cos(frac * 6.283185307)));
                        float mh = st.sim_hour - static_cast<float>(frac * 24.0);
                        while (mh < 0.f) mh += 24.f;
                        const int doy_m = omsi::sky::day_of_year(st.sim_month, st.sim_day);
                        const auto msky = omsi::sky::evaluate(doy_m, mh, s_lat, s_lon, st.timezone_h, st.weather_id);
                        moon_x = msky.sun_x;
                        moon_y = msky.sun_y;
                        moon_z = msky.sun_z;
                    }
                    const float moon_up = (std::clamp)((moon_z - 0.02f) / 0.2f, 0.f, 1.f);
                    const float moon_str = 0.24f * (0.3f + 0.7f * moon_illum) * moon_up *
                                           (std::max)(0.f, 1.f - 0.85f * omsi::sky::weather_preset(st.weather_id).cover) *
                                           sky.night_blend;
                    // (test override OMSI_SHADOW_SIZE=<px>, 0 = no sun shadows: what the cascades cost the GPU)
                    static const int s_shadow_env = [] {
                        const char* e = std::getenv("OMSI_SHADOW_SIZE");
                        return e && *e ? std::atoi(e) : -1;
                    }();
                    const bool shadows_on = s_shadow_env == 0 ? false : st.show_sun_shadows;
                    const std::uint32_t shadows_size =
                        s_shadow_env > 0 ? static_cast<std::uint32_t>(s_shadow_env) : st.shadow_map_size;
                    if (moon_str > sun_str) {
                        renderer.set_sun_lighting(moon_x, moon_y, moon_z, moon_str * 0.62f, moon_str * 0.72f,
                                                  moon_str * 0.95f, shadows_on, shadows_size);
                    } else {
                        // the sun's colour by its height: the light crosses more air low over the horizon and
                        // comes in orange at sunrise / sunset, white at noon (it stayed one white all day)
                        const float lo = 1.f - (std::clamp)(sky.elevation_deg / 25.f, 0.f, 1.f);
                        const float k = lo * lo;
                        renderer.set_sun_lighting(sky.sun_x, sky.sun_y, sky.sun_z, sun_str, sun_str * (0.96f - 0.36f * k),
                                                  sun_str * (0.88f - 0.58f * k), shadows_on, shadows_size);
                    }
                    {
                        // Vista > Graficos > Distancia de sombras (0 = the visual radius)
                        const float csm_far = cfg.beamng ? bng_shadow_distance
                                              : st.shadow_distance_m > 0.f
                                                  ? st.shadow_distance_m
                                                  : (editor_visual_radius_m > 0.f ? editor_visual_radius_m : 300.f);
                        renderer.set_csm_far_distance(csm_far);
                        renderer.set_fog_scale(st.fog_enabled ? 1.f : 0.f);
                    }
                    renderer.set_clear_color(sky.sky_r, sky.sky_g, sky.sky_b);
                    // OpenOMSI-style gradient dome: zenith from sky RGB, warmer/brighter horizon + sun disk.
                    const float dusk = sky.sky_w_dusk;
                    const float night = sky.sky_w_night;
                    const float zen_r = sky.sky_r * (1.f - 0.35f * night);
                    const float zen_g = sky.sky_g * (1.f - 0.35f * night);
                    const float zen_b = sky.sky_b * (1.f - 0.25f * night);
                    const float hor_r = (std::min)(1.f, sky.sky_r * 1.15f + 0.35f * dusk + 0.08f * night);
                    const float hor_g = (std::min)(1.f, sky.sky_g * 1.05f + 0.18f * dusk + 0.06f * night);
                    const float hor_b = (std::min)(1.f, sky.sky_b * 0.95f + 0.05f * dusk + 0.12f * night);
                    const float sun_i =
                        (std::max)(0.05f, sun_str * (1.f - night * 0.85f) + dusk * 0.55f);
                    renderer.set_sky_visuals(zen_r, zen_g, zen_b, hor_r, hor_g, hor_b, sky.sun_x, sky.sun_y,
                                             sky.sun_z, 1.f, 0.92f + 0.08f * dusk, 0.65f + 0.2f * dusk, sun_i,
                                             true);
                    {
                        // OMSI environment (himmel day/dusk/night, clouds, stars, sun/moon, weather preset).
                        const auto& wp = omsi::sky::weather_preset(st.weather_id);
                        omsi::gfx::dx12::HelloO3dRenderer::SkyEnvironment env{};
                        env.asset_root = cfg.omsi_root;
                        env.sun_dir[0] = sky.sun_x;
                        env.sun_dir[1] = sky.sun_y;
                        env.sun_dir[2] = sky.sun_z;
                        // Sun colour warms toward orange as it nears the horizon (sunrise / sunset).
                        const float warm = 1.f - (std::max)(0.f, (std::min)(1.f, sky.elevation_deg / 20.f));
                        env.sun_color[0] = 1.f;
                        env.sun_color[1] = 0.96f - 0.51f * warm;
                        env.sun_color[2] = 0.90f - 0.72f * warm;
                        env.sun_intensity = sky.elevation_deg > -1.f ? sun_i : 0.f;
                        // the moon disk where its light comes from (see the moon above)
                        const float ml = (std::max)(1e-4f, std::sqrt(moon_x * moon_x + moon_y * moon_y + moon_z * moon_z));
                        env.moon_dir[0] = moon_x / ml;
                        env.moon_dir[1] = moon_y / ml;
                        env.moon_dir[2] = moon_z / ml;
                        env.moon_intensity = sky.night_blend * 0.8f * (0.35f + 0.65f * moon_illum);
                        env.day_w = sky.sky_w_day;
                        env.dusk_w = sky.sky_w_dusk;
                        env.night_w = sky.sky_w_night;
                        env.sun_alt_deg = sky.elevation_deg;
                        env.star_brightness = sky.night_blend;
                        env.lst_rad = sky.lst_rad;
                        env.lat_rad = sky.lat_rad;
                        env.cloud_cover = wp.cover;
                        env.cloud_type = static_cast<int>(wp.cloud_type);
                        env.cloud_base_m = wp.cloud_base_m;
                        env.cloud_wind_to_rad = (wp.wind_from_deg + 180.f) * 3.14159265f / 180.f;
                        env.cloud_wind_speed = wp.wind_mps;
                        const float lit = (std::max)(0.f, (std::min)(1.f, (sky.elevation_deg + 6.f) / 16.f));
                        env.cloud_brightness = (0.2f + 0.8f * lit) * (wp.precip != 0 ? 0.75f : 1.f);
                        env.fog_rgb[0] = hor_r;
                        env.fog_rgb[1] = hor_g;
                        env.fog_rgb[2] = hor_b;
                        const float fog_k = panel.imgui_state().fog_enabled ? 1.f : 0.f; // (Vista > Niebla)
                        env.fog_density = fog_k * 3.f / (std::max)(1.f, wp.fog_range_m);
                        env.haze = fog_k * (std::min)(0.6f, 150.f / (std::max)(1.f, wp.fog_range_m));
                        env.precip_type = wp.precip;
                        env.precip_intensity = wp.precip_intensity;
                        // Light shafts sample the CSM, so they follow the sun-shadow toggle.
                        env.god_ray_strength = st.show_sun_shadows ? sun_str : 0.f;
                        // OMSI_SHAFT_PLUS=1 forces Plus for testing without changing the saved cfg.
                        const char* shaft_env = std::getenv("OMSI_SHAFT_PLUS");
                        env.shaft_plus = st.shaft_plus || (shaft_env && shaft_env[0] == '1');
                        const char* shaft_dbg = std::getenv("OMSI_SHAFT_DEBUG");
                        env.shaft_debug = shaft_dbg && shaft_dbg[0] == '1';
                        {
                            // the terrain under the EYE (the camera's ground_z is the look target's)
                            float gx_e = 0.f, gy_e = 0.f, gz_e = 0.f;
                            camera.eye_position(gx_e, gy_e, gz_e);
                            const auto eye_gz = sample_terrain_editor_xy_opt(gx_e, gy_e);
                            env.ground_z = eye_gz ? *eye_gz : gz_e - 2.f;
                        }
                        renderer.set_sky_environment(env);
                    }
                }
                renderer.render();
            } catch (const std::exception& ex) {
                log::error(std::string("renderer.render: ") + ex.what());
            } catch (...) {
                log::error("renderer.render: unknown exception");
            }
            if (g_editor_frame_id <= 2) omsi::crash_report::note("frame load: post_render");
        }
        {
            auto _watch_post_render = main_watchdog.phase("post_render");
            auto _perf = frame_profiler.scope(MapEditorFrameProfiler::PostRender);
            // Marquee rect is drawn as screen-space GPU overlay (GDI cannot paint over DX12 flip).
            ++frames;
            if (cfg.sa_entry_hop_test) {
                auto hop_to_entry = [&](const std::size_t ep_i, const std::string_view tag) {
                    if (ep_i >= cfg.global.entrypoints.size()) return;
                    teleport_camera_to_entrypoint(cfg.global.entrypoints[ep_i], tag.data());
                    log::info(std::string("saHopTest teleport ") + std::string(tag) + " ep=" + std::to_string(ep_i));
                    audit_sa_watch(cfg.sa_watch_id, tag);
                };
                if (frames == 600) audit_sa_watch(cfg.sa_watch_id, "boot");
                else if (frames == 900 && cfg.global.entrypoints.size() > 1) hop_to_entry(1, "to_ep1");
                else if (frames == 2400) hop_to_entry(0, "back_ep0");
                else if (frames == 3600) audit_sa_watch(cfg.sa_watch_id, "final");
                if (frames >= 4200) glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            if (cfg.max_frames > 0 && frames >= cfg.max_frames) glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        {
            auto _watch_end = main_watchdog.phase("frame.end_profiling_and_diag");
            frame_profiler.end_frame(edit_objects.size(), scene_places.size(), spline_gpu.size(), renderer.chunk_count(),
                                     world.tiles().size(), renderer.last_gpu_timing());

            // Diagnostics: CAM_HITCH (tirón al orbitar/zoom) + MEM60 (RAM que crece sin techo).
            {
                const bool cam_held = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) ||
                                      (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS);
                static double s_frame_ms = 0.0;
                static auto s_prev = std::chrono::steady_clock::now();
                const auto now_diag = std::chrono::steady_clock::now();
                s_frame_ms = std::chrono::duration<double, std::milli>(now_diag - s_prev).count();
                s_prev = now_diag;
                if (cam_held && s_frame_ms > 22.0) {
                    static int s_hitch_n = 0;
                    if (s_hitch_n < 40 || (s_hitch_n % 30) == 0) {
                        log::warn("CAM_HITCH ms=" + std::to_string(s_frame_ms) +
                                  " rmb=" + std::to_string(glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) +
                                  " mmb=" + std::to_string(glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS) +
                                  " stream=" + std::to_string(static_cast<int>(stream_phase)) +
                                  " cam_undo=" + std::to_string(camera_undo_stack.size()) +
                                  " chunks=" + std::to_string(renderer.chunk_count()) +
                                  " places=" + std::to_string(scene_places.size()));
                    }
                    ++s_hitch_n;
                }
                if ((frames % 60) == 1) {
                    std::size_t alive_sc = 0, dead_sc = 0;
                    for (const auto& sp : scene_places) {
                        if (sp.alive) ++alive_sc;
                        else ++dead_sc;
                    }
                    const auto rd = renderer.resource_diag();
                    std::uint64_t working = 0, private_bytes = 0;
#if defined(_WIN32)
                    PROCESS_MEMORY_COUNTERS_EX pmc{};
                    if (GetProcessMemoryInfo(GetCurrentProcess(),
                                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                        working = static_cast<std::uint64_t>(pmc.WorkingSetSize);
                        private_bytes = static_cast<std::uint64_t>(pmc.PrivateUsage);
                    }
#endif
                    static std::uint64_t s_prev_priv = 0;
                    const std::int64_t dpriv = static_cast<std::int64_t>(private_bytes) -
                                              static_cast<std::int64_t>(s_prev_priv);
                    s_prev_priv = private_bytes;
                    log::info("MEM60 ws_mb=" + std::to_string(working / (1024ull * 1024ull)) +
                              " priv_mb=" + std::to_string(private_bytes / (1024ull * 1024ull)) +
                              " dpriv_mb=" + std::to_string(dpriv / (1024ll * 1024ll)) +
                              " chunks=" + std::to_string(rd.chunks) +
                              " gpu=" + std::to_string(rd.chunks_with_gpu) +
                              " srv=" + std::to_string(rd.srv_next) +
                              " tex_cache=" + std::to_string(rd.texture_cache) +
                              " tex_slots=" + std::to_string(rd.tex_slots) +
                              " scratch=" + std::to_string(rd.upload_scratch) +
                              " pick_mb=" + std::to_string(rd.pick_bytes / (1024ull * 1024ull)) +
                              " sco_cache=" + std::to_string(sco::sco_cache_size()) +
                              " x_cache=" + std::to_string(xfile::x_mesh_cache_size()) +
                              " scene_alive=" + std::to_string(alive_sc) +
                              " scene_dead=" + std::to_string(dead_sc) +
                              " edit_obj=" + std::to_string(edit_objects.size()) +
                              " stream=" + std::to_string(static_cast<int>(stream_phase)) +
                              " cam_undo=" + std::to_string(camera_undo_stack.size()));
                    log::info(frame_profiler.fps.summary_line());
                }
            }
        }
    }

    const bool switch_map = cfg.switch_map_out && !cfg.switch_map_out->empty();
    {
        map::save_editor_geo_settings(cfg.global.root, geo_settings);
        // BUGFIX (settings "always reset"): this used to save a default-constructed EditorUiSettings that
        // collect_ui_settings() only partly fills, so every exit / map switch wrote tile radius=1, visual
        // radius=300, min screen 6 %, VSync on, union/grad defaults... over the user's choices. Collect into
        // the live settings instead so the Options-dialog fields (not in the panel state) survive.
        panel.collect_ui_settings(editor_ui_settings);
        editor_ui_settings.editor_tile_radius = editor_tile_radius;
        editor_ui_settings.editor_visual_radius_m = editor_visual_radius_m;
        editor_ui_settings.vsync = renderer.vsync();
        map::save_editor_ui_settings(cfg.global.root, editor_ui_settings);
    }
    persist_all_loaded_tiles();
    {
        std::scoped_lock lock(spline_env_job.mutex);
        spline_env_job.running = false;
    }
    if (spline_env_job.worker.joinable()) spline_env_job.worker.join();
    async_remesh_join();
    stream_join_cpu_workers();
    if (dtm_job && dtm_job->thread.joinable()) dtm_job->thread.join();
    dtm_job.reset();
    if (aerial_job && aerial_job->thread.joinable()) aerial_job->thread.join();
    aerial_job.reset();
    // Text-texture bake jobs capture texttex_async_mu / texttex_bake_ready / texttex_bake_inflight BY REFERENCE: they
    // must finish before this function's locals are destroyed, or a worker touches freed memory (AV after
    // "editor exit ok"). Bounded wait: a bake is a few ms.
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(texttex_async_mu);
                if (texttex_bake_inflight.empty()) break;
            }
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) {
                log::warn("editor exit: text-texture bakes still running after 10 s");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (hand_cursor) glfwDestroyCursor(hand_cursor);
    editor_log::shutdown();
    imgui_overlay.shutdown();
    ultralight_overlay.shutdown();
    panel.close();
    renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    log::info("editor exit ok frames=" + std::to_string(frames) + (switch_map ? " switch_map" : ""));
    if (cfg.test_spline_creator || !cfg.spline_creator_test_ok) return cfg.spline_creator_test_ok ? 0 : 102;
    return switch_map ? 100 : 0;
