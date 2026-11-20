// ============================================================================
// MOD: adjust HUD (altura / rotacion) — aislado. No depender de edit_mode solo.
// ============================================================================
    bool adjust_hud_latched = false;
    ui::EditorAdjustHud::Mode adjust_hud_latched_mode = ui::EditorAdjustHud::Mode::None;
    float adjust_hud_anchor_z = 0.f;
    float adjust_hud_anchor_yaw = 0.f;

    auto adjust_hud_owner = [&]() -> HWND {
        return glfwGetWin32Window(window);
    };
    auto adjust_hud_has_selection = [&]() -> bool {
        return shift_s_move.active || group_grab_active || group_rotate_active ||
               (!group_sel_obj.empty() || !group_sel_spl.empty()) ||
               (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size()) ||
               (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size());
    };
    auto current_ref_z = [&]() -> float {
        if (shift_s_move.active && !shift_s_move.items.empty()) {
            const auto& it = shift_s_move.items.front();
            if (it.kind == 0) {
                if (auto* o = shift_s_find_obj(it.id)) return o->position.z;
            } else if (auto* s = shift_s_find_spl(it.id)) {
                return s->position.z;
            }
            return it.z0 + shift_s_move.cur_dz;
        }
        if (group_grab_active && !group_grab_index.empty()) {
            if (group_grab_kind.front() == 0 &&
                static_cast<std::size_t>(group_grab_index.front()) < edit_objects.size())
                return edit_objects[static_cast<std::size_t>(group_grab_index.front())].position.z;
            if (group_grab_kind.front() == 1 &&
                static_cast<std::size_t>(group_grab_index.front()) < edit_splines.size())
                return edit_splines[static_cast<std::size_t>(group_grab_index.front())].position.z;
        }
        if (group_rotate_active && !group_grab_index.empty()) {
            if (group_grab_kind.front() == 0 &&
                static_cast<std::size_t>(group_grab_index.front()) < edit_objects.size())
                return edit_objects[static_cast<std::size_t>(group_grab_index.front())].position.z;
            if (group_grab_kind.front() == 1 &&
                static_cast<std::size_t>(group_grab_index.front()) < edit_splines.size())
                return edit_splines[static_cast<std::size_t>(group_grab_index.front())].position.z;
        }
        if (!group_sel_obj.empty()) {
            const int gi = group_sel_obj.front();
            if (gi >= 0 && static_cast<std::size_t>(gi) < edit_objects.size())
                return edit_objects[static_cast<std::size_t>(gi)].position.z;
        }
        if (!group_sel_spl.empty()) {
            const int gi = group_sel_spl.front();
            if (gi >= 0 && static_cast<std::size_t>(gi) < edit_splines.size())
                return edit_splines[static_cast<std::size_t>(gi)].position.z;
        }
        if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
            return edit_objects[static_cast<std::size_t>(selected_object)].position.z;
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
            return edit_splines[static_cast<std::size_t>(selected_spline)].position.z;
        return adjust_hud_anchor_z;
    };
    auto current_ref_yaw = [&]() -> float {
        if (group_rotate_active && !group_grab_index.empty()) {
            if (group_grab_kind.front() == 0 &&
                static_cast<std::size_t>(group_grab_index.front()) < edit_objects.size())
                return edit_objects[static_cast<std::size_t>(group_grab_index.front())].rotation_deg.x;
            if (group_grab_kind.front() == 1 &&
                static_cast<std::size_t>(group_grab_index.front()) < edit_splines.size())
                return edit_splines[static_cast<std::size_t>(group_grab_index.front())].heading_deg;
            if (!group_start_yaw.empty()) return group_start_yaw.front();
        }
        if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
            return edit_objects[static_cast<std::size_t>(selected_object)].rotation_deg.x;
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
            return edit_splines[static_cast<std::size_t>(selected_spline)].heading_deg;
        return adjust_hud_anchor_yaw;
    };
    auto dismiss_adjust_hud = [&]() {
        adjust_hud_latched = false;
        adjust_hud_latched_mode = ui::EditorAdjustHud::Mode::None;
        hud_group_pose.valid = false;
        adjust_hud.hide();
    };
    auto apply_exact_height = [&](float abs_z) {
        const float delta = abs_z - adjust_hud_anchor_z;
        auto remesh_spl_commit = [&](map::SplineInst& s) {
            // Full textured remesh so height edits never leave vertex/wireframe mode.
            remesh_spline(s, false);
            upsert_transient_spline(s);
            persist_editor_xy(s.position.x, s.position.y);
        };
        if (shift_s_move.active) {
            // Height-only: cancel any XY offset from mouse during G before Z.
            shift_s_move_apply(0.f, 0.f, delta, true, true);
            return;
        }
        if (group_grab_active) {
            for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                if (group_grab_kind[i] == 0) {
                    auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                    // Restore XY to grab start (cancel mouse XY drift), apply shared Z delta.
                    o.position.x = grab_start_x + group_off_x[i];
                    o.position.y = grab_start_y + group_off_y[i];
                    o.position.z = group_grab_start_z[i] + delta;
                    sync_object_gpu(o);
                } else {
                    auto& s = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                    s.position.x = grab_start_x + group_off_x[i];
                    s.position.y = grab_start_y + group_off_y[i];
                    s.position.z = group_grab_start_z[i] + delta;
                    set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
                    remesh_spl_commit(s);
                }
            }
            return;
        }
        if (group_rotate_active) {
            for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                if (group_grab_kind[i] == 0) {
                    auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                    o.position.z = group_grab_start_z[i] + delta;
                    sync_object_gpu(o);
                } else {
                    auto& s = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                    s.position.z = group_grab_start_z[i] + delta;
                    remesh_spl_commit(s);
                }
            }
            return;
        }
        // Multi-select group without active grab: keep relative Z offsets.
        if (!group_sel_obj.empty() || !group_sel_spl.empty()) {
            for (int gi : group_sel_obj) {
                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_objects.size()) continue;
                auto& o = edit_objects[static_cast<std::size_t>(gi)];
                // Anchor is adjust_hud_anchor_z = first selected Z at latch time.
                // Each item moves by the same delta so relative heights stay.
                const float base = o.position.z; // will be wrong second step —
                (void)base;
            }
            // Re-read bases from a stable store: use delta from anchor so
            // second step still works if we track per-item start Z at latch.
            // Prefer group_sel start Z if we latched them; else absolute delta on each.
            for (int gi : group_sel_obj) {
                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_objects.size()) continue;
                auto& o = edit_objects[static_cast<std::size_t>(gi)];
                o.position.z += delta;
                // After apply, re-anchor so next step is relative to new value.
                sync_object_gpu(o);
            }
            for (int gi : group_sel_spl) {
                if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
                auto& s = edit_splines[static_cast<std::size_t>(gi)];
                s.position.z += delta;
                remesh_spl_commit(s);
            }
            adjust_hud_anchor_z = abs_z;
            return;
        }
        if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size()) {
            auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
            o.position.z = abs_z;
            sync_object_gpu(o);
            show_object_at(static_cast<std::size_t>(selected_object));
            adjust_hud_anchor_z = abs_z;
        } else if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
            auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
            s.position.z = abs_z;
            remesh_spl_commit(s);
            show_spline_at(static_cast<std::size_t>(selected_spline));
            adjust_hud_anchor_z = abs_z;
        }
    };
    auto sync_adjust_hud = [&]() {
        const int want_mode =
            edit_mode == EditMode::GrabHeight ? 1 : (edit_mode == EditMode::Rotate ? 2 : 0);

        if (want_mode == 1) {
            adjust_hud_latched = true;
            adjust_hud_latched_mode = ui::EditorAdjustHud::Mode::Height;
            if (adjust_hud_imgui.seed_mode != 1) {
                adjust_hud_imgui.seed_mode = 1;
                adjust_hud_imgui.user_editing = false;
                adjust_hud_anchor_z = grab_start_z;
                if (group_grab_active && !group_grab_start_z.empty())
                    adjust_hud_anchor_z = group_grab_start_z.front();
                else if (group_rotate_active && !group_grab_start_z.empty())
                    adjust_hud_anchor_z = group_grab_start_z.front();
                else if (!group_sel_obj.empty() && group_sel_obj.front() >= 0 &&
                         static_cast<std::size_t>(group_sel_obj.front()) < edit_objects.size())
                    adjust_hud_anchor_z = edit_objects[static_cast<std::size_t>(group_sel_obj.front())].position.z;
                else if (!group_sel_spl.empty() && group_sel_spl.front() >= 0 &&
                         static_cast<std::size_t>(group_sel_spl.front()) < edit_splines.size())
                    adjust_hud_anchor_z = edit_splines[static_cast<std::size_t>(group_sel_spl.front())].position.z;
                else if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                    adjust_hud_anchor_z = edit_objects[static_cast<std::size_t>(selected_object)].position.z;
                else if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                    adjust_hud_anchor_z = edit_splines[static_cast<std::size_t>(selected_spline)].position.z;
                adjust_hud_imgui.previous = adjust_hud_anchor_z;
                adjust_hud_imgui.current = current_ref_z();
            }
        } else if (want_mode == 2) {
            adjust_hud_latched = true;
            adjust_hud_latched_mode = ui::EditorAdjustHud::Mode::Rotation;
            if (adjust_hud_imgui.seed_mode != 2) {
                adjust_hud_imgui.seed_mode = 2;
                adjust_hud_imgui.user_editing = false;
                adjust_hud_anchor_yaw = grab_start_yaw;
                if (group_rotate_active && !group_start_yaw.empty())
                    adjust_hud_anchor_yaw = group_start_yaw.front();
                adjust_hud_imgui.previous = adjust_hud_anchor_yaw;
                adjust_hud_imgui.current = current_ref_yaw();
            }
        }
        // Tras confirmar R/G el HUD queda latched hasta deseleccionar (no limpiar en want_mode==0).

        if (!adjust_hud_latched) {
            adjust_hud.hide();
            adjust_hud_imgui = {};
            return;
        }
        if (!adjust_hud_has_selection()) {
            dismiss_adjust_hud();
            adjust_hud_imgui = {};
            return;
        }

        const int hud_mode =
            adjust_hud_latched_mode == ui::EditorAdjustHud::Mode::Height ? 1 : 2;

        if (panel.imgui_primary()) {
            adjust_hud.hide();
            adjust_hud_imgui.active = true;
            adjust_hud_imgui.mode = hud_mode;
            if (!adjust_hud_imgui.user_editing) {
                adjust_hud_imgui.previous =
                    hud_mode == 1 ? adjust_hud_anchor_z : adjust_hud_anchor_yaw;
                adjust_hud_imgui.current = hud_mode == 1 ? current_ref_z() : current_ref_yaw();
            }
            return;
        }

        const HWND owner = adjust_hud_owner();
        if (adjust_hud_latched_mode == ui::EditorAdjustHud::Mode::Height) {
            if (!adjust_hud.visible())
                adjust_hud.show(owner, ui::EditorAdjustHud::Mode::Height, adjust_hud_anchor_z, current_ref_z());
            else {
                adjust_hud.set_previous(adjust_hud_anchor_z);
                adjust_hud.set_current(current_ref_z());
                adjust_hud.keep_on_top();
            }
        } else if (adjust_hud_latched_mode == ui::EditorAdjustHud::Mode::Rotation) {
            if (!adjust_hud.visible())
                adjust_hud.show(owner, ui::EditorAdjustHud::Mode::Rotation, adjust_hud_anchor_yaw, current_ref_yaw());
            else {
                adjust_hud.set_previous(adjust_hud_anchor_yaw);
                adjust_hud.set_current(current_ref_yaw());
                adjust_hud.keep_on_top();
            }
        }
    };
