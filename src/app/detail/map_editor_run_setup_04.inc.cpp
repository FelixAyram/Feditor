        }
        for (const auto want : group_sel_spl_ids) {
            for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                if (edit_splines[i].id == want) {
                    group_sel_spl.push_back(static_cast<int>(i));
                    break;
                }
            }
        }
        group_grab_active = false;
        group_rotate_active = false;
        shift_s_move_clear();
        refresh_editor_lists();
        if (!group_sel_obj.empty() || !group_sel_spl.empty())
            panel.set_selection_text("Grupo: " + std::to_string(group_sel_obj.size()) + " obj, " +
                                     std::to_string(group_sel_spl.size()) + " spl");
        else
            panel.set_selection_text("(none)");
        editor_log::event("SNAP_APPLY_DONE", "live obj=" + std::to_string(edit_objects.size()) + " spl=" +
                                                 std::to_string(edit_splines.size()) + " tr_obj=" +
                                                 std::to_string(transient_objects.size()) + " tr_spl=" +
                                                 std::to_string(transient_splines.size()));
        // Undo/redo must NOT force a full disk save of every loaded tile.
        // Mark only tiles touched by objects/splines that actually changed so
        // stream-out / Ctrl+S / autosave can persist selectively.
        {
            auto mark_xy = [&](float ex, float ey) {
                if (auto* t = find_tile_mut_xy(ex, ey)) mark_tile_dirty(t->grid_x, t->grid_y);
            };
            std::unordered_set<std::int32_t> after_s, after_o;
            after_s.reserve(edit_splines.size());
            after_o.reserve(edit_objects.size());
            for (const auto& s : edit_splines) {
                after_s.insert(s.id);
                const auto it = before_s.find(s.id);
                if (it == before_s.end() || !spline_geom_equal(it->second, s)) {
                    mark_xy(s.position.x, s.position.y);
                    if (it != before_s.end()) mark_xy(it->second.position.x, it->second.position.y);
                }
            }
            for (const auto& o : edit_objects) {
                after_o.insert(o.id);
                const auto it = before_o.find(o.id);
                if (it == before_o.end() || !object_geom_equal(it->second, o)) {
                    mark_xy(o.position.x, o.position.y);
                    if (it != before_o.end()) mark_xy(it->second.position.x, it->second.position.y);
                }
            }
            for (const auto& kv : before_s) {
                if (!after_s.count(kv.first)) mark_xy(kv.second.position.x, kv.second.position.y);
            }
            for (const auto& kv : before_o) {
                if (!after_o.count(kv.first)) mark_xy(kv.second.position.x, kv.second.position.y);
            }
        }
        grad_slider_undo_id = 0;
        // Index-based path-rail state dies with the erased/rebuilt rails; stale indices were a crash source.
        path_rails_detect_now = true; // undo/redo moved things: re-check which owners really changed
        selected_path_rail = -1;
        traffic_anchor_rail = -1;
        paint_drag_last_rail = -1;
        traffic_chain_gen.fetch_add(1, std::memory_order_acq_rel);
        traffic_chain_rails.clear();
        spline_attach_obj_to_entry.clear();
        for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
            const auto iid = edit_spline_attachments[ei].att.instance_id;
            if (iid != 0) spline_attach_obj_to_entry[iid] = ei;
        }
    };
    // SEH net for undo/redo: an access violation while swapping a snapshot is logged and survived
    // (the editor keeps running) instead of killing the viewer. Returns false when it trapped one.
    // set after every undo / redo: the co-op module re-applies the other users' latest changes,
    // because a snapshot restore would otherwise undo their work too.
    bool coop_snapshot_applied = false;
    auto apply_editor_snapshot_safe = [&](EditorSnapshot snap) -> bool {
        coop_snapshot_applied = true;
        struct Ctx {
            std::function<void()> fn;
        } ctx;
        ctx.fn = [&]() { apply_editor_snapshot(std::move(snap)); };
        const int seh = seh_invoke(+[](void* p) { static_cast<Ctx*>(p)->fn(); }, &ctx);
        if (seh != 0) {
            editor_log::event("UNDO_SEH", "code=0x" + std::to_string(static_cast<unsigned>(seh)));
            log::error("[undo] SEH trapped while applying snapshot code=" + std::to_string(static_cast<unsigned>(seh)));
            panel.set_status("Undo/Redo fallo (recuperado) - guarda y revisa el log");
            selected_spline = selected_object = -1;
            return false;
        }
        return true;
    };
    // undo_stack / redo_stack / CameraSnapshot y variables de cámara se definen
    // en modules/mod_undo.inc.cpp (incluido vía map_editor_modules.inc.cpp más abajo).
    auto pump_editor_busy = [&]() {
        glfwPollEvents();
        panel.pump();
        int pw = 1, ph = 1;
        glfwGetFramebufferSize(window, &pw, &ph);
        if (pw < 1 || ph < 1) return;
        const float aspect = static_cast<float>(pw) / static_cast<float>(ph);
        float wvp_t[16]{};
        camera.build_world_view_proj(aspect, wvp_t);
        renderer.set_world_view_proj(wvp_t);
        update_renderer_tile_cull();
        if (imgui_overlay.active()) {
            imgui_overlay.begin_frame();
            imgui_overlay.end_frame_and_build();
        }
        renderer.render();
    };
    // Async spline environment adaptation state. Worker threads only consume immutable
    // snapshots; edit_splines and renderer mutations are committed on the editor thread.
    struct SplineEnvPartPlan {
        float start_along = 0.f;
        float end_along = 0.f;
        float start_z = 0.f;
        float end_z = 0.f;
        float start_grad = 0.f;
        float end_grad = 0.f;
        float start_cant = 0.f;
        float end_cant = 0.f;
        float delta_height = 0.f;
    };
    struct SplineEnvSplinePlan {
        std::int32_t source_id = 0;
        float original_start_z = 0.f;
        std::int32_t old_prev = 0;
        std::int32_t old_next = 0;
        std::filesystem::path sli_path;
        std::vector<SplineEnvPartPlan> parts;
        float worst_gap = 0.f;
        bool all_endpoints_within_tolerance = false;
    };
    struct SplineEnvJobResult {
        std::vector<SplineEnvSplinePlan> splines;
        int requested_quality = 1;
    };
    struct SplineEnvJobState {
        std::mutex mutex;
        std::thread worker;
        bool running = false;
        bool finished = false;
        std::shared_ptr<SplineEnvJobResult> result;
    };
    SplineEnvJobState spline_env_job;

#include "detail/map_editor_modules.inc.cpp"
    auto capture_camera_snapshot = [&]() -> CameraSnapshot {
        return {camera.target_x, camera.target_y, camera.target_z, camera.radius};
    };
    auto apply_camera_snapshot = [&](const CameraSnapshot& s) {
        camera_apply_silent = true;
        camera.target_x = s.target_x;
        camera.target_y = s.target_y;
        camera.target_z = s.target_z;
        camera.radius = s.radius;
        camera_track_last = s;
        camera_apply_silent = false;
    };
    auto editor_modal_owner = [&]() -> HWND {
        // ImGui-primary hides panel.hwnd(); modals need the visible GLFW frame as owner.
        if (panel.imgui_primary()) return glfwGetWin32Window(window);
        if (panel.hwnd()) return panel.hwnd();
        return glfwGetWin32Window(window);
    };
    auto camera_changed = [](const CameraSnapshot& a, const CameraSnapshot& b) {
        auto ne = [](float x, float y) { return std::abs(x - y) > 1.e-4f; };
        return ne(a.target_x, b.target_x) || ne(a.target_y, b.target_y) || ne(a.target_z, b.target_z) ||
               ne(a.radius, b.radius);
    };
    auto track_camera_after_update = [&]() {
        if (camera_apply_silent) return;
        const CameraSnapshot now = capture_camera_snapshot();
        if (!camera_track_init) {
            camera_track_last = now;
            camera_track_init = true;
            return;
        }
        if (!camera_changed(camera_track_last, now)) return;
        // Sample undo at most ~5 Hz. Pushing every orbit/zoom frame + erase(begin) on a
        // vector was a small hitch while holding RMB/MMB (and filled undo with noise).
        const auto t = std::chrono::steady_clock::now();
        constexpr auto kMinGap = std::chrono::milliseconds(200);
        if (!camera_undo_last_push_valid || (t - camera_undo_last_push) >= kMinGap) {
            camera_undo_stack.push_back(camera_track_last);
            if (camera_undo_stack.size() > 64) camera_undo_stack.pop_front();
            camera_redo_stack.clear();
            camera_undo_last_push = t;
            camera_undo_last_push_valid = true;
            camera_track_last = now;
        }
    };
    auto undo_camera = [&]() -> bool {
        if (camera_undo_stack.empty()) return false;
        camera_redo_stack.push_back(capture_camera_snapshot());
        const CameraSnapshot prev = camera_undo_stack.back();
        camera_undo_stack.pop_back();
        apply_camera_snapshot(prev);
        panel.set_status("Camara undo (Ctrl+Alt+Z)");
        return true;
    };
    auto redo_camera = [&]() -> bool {
        if (camera_redo_stack.empty()) return false;
        camera_undo_stack.push_back(capture_camera_snapshot());
        const CameraSnapshot next = camera_redo_stack.back();
        camera_redo_stack.pop_back();
        apply_camera_snapshot(next);
        panel.set_status("Camara redo (Ctrl+Shift+Alt+Z)");
        return true;
    };
    auto shift_editor_snapshot_xy = [&](EditorSnapshot& snap, float dx, float dy) {
        auto shift_o = [&](map::MapObjectInst& o) {
            o.position.x += dx;
            o.position.y += dy;
        };
        auto shift_s = [&](map::SplineInst& s) {
            s.position.x += dx;
            s.position.y += dy;
        };
        for (auto& o : snap.objects) shift_o(o);
        for (auto& s : snap.splines) shift_s(s);
        for (auto& o : snap.transient_objects) shift_o(o);
        for (auto& s : snap.transient_splines) shift_s(s);
    };
    auto remap_indices_from_ids = [&](std::vector<int>& indices, const std::vector<std::int32_t>& ids,
                                      const auto& list, auto id_of) {
        indices.clear();
        indices.reserve(ids.size());
        for (const auto want : ids) {
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (id_of(list[i]) == want) {
                    indices.push_back(static_cast<int>(i));
                    break;
                }
            }
        }
    };
    auto remap_marquee_and_group_after_stream = [&]() {
        // Never remap by stale list indices â€” edit_objects was rebuilt; use remembered ids only.
        if (marquee.phase == MarqueePhase::Placing) {
            remap_indices_from_ids(marquee.track_obj, marquee.track_obj_ids, edit_objects,
                                   [](const map::MapObjectInst& o) { return o.id; });
            remap_indices_from_ids(marquee.track_spl, marquee.track_spl_ids, edit_splines,
                                   [](const map::SplineInst& s) { return s.id; });
        }
        remap_indices_from_ids(group_sel_obj, group_sel_obj_ids, edit_objects,
                               [](const map::MapObjectInst& o) { return o.id; });
        remap_indices_from_ids(group_sel_spl, group_sel_spl_ids, edit_splines,
                               [](const map::SplineInst& s) { return s.id; });
        if (group_grab_active || group_rotate_active) {
            // Remap grab/rotate indices from ids â€” keep mode alive across tile stream.
            std::vector<int> new_kind, new_index;
            std::vector<std::int32_t> new_ids;
            std::vector<float> new_off_x, new_off_y, new_off_z, new_start_z, new_yaw;
            new_kind.reserve(group_grab_ids.size());
            for (std::size_t i = 0; i < group_grab_ids.size(); ++i) {
                const std::int32_t id = group_grab_ids[i];
                const int kind = (i < group_grab_kind.size()) ? group_grab_kind[i] : 0;
                int idx = -1;
                if (kind == 0) {
                    for (std::size_t j = 0; j < edit_objects.size(); ++j) {
                        if (edit_objects[j].id == id) {
                            idx = static_cast<int>(j);
                            break;
                        }
                    }
                } else {
                    for (std::size_t j = 0; j < edit_splines.size(); ++j) {
                        if (edit_splines[j].id == id) {
                            idx = static_cast<int>(j);
                            break;
                        }
                    }
                }
                if (idx < 0) continue;
                new_kind.push_back(kind);
                new_index.push_back(idx);
                new_ids.push_back(id);
                if (i < group_off_x.size()) new_off_x.push_back(group_off_x[i]);
                if (i < group_off_y.size()) new_off_y.push_back(group_off_y[i]);
                if (i < group_off_z.size()) new_off_z.push_back(group_off_z[i]);
                if (i < group_grab_start_z.size()) new_start_z.push_back(group_grab_start_z[i]);
                if (i < group_start_yaw.size()) new_yaw.push_back(group_start_yaw[i]);
            }
            group_grab_kind = std::move(new_kind);
            group_grab_index = std::move(new_index);
            group_grab_ids = std::move(new_ids);
            group_off_x = std::move(new_off_x);
            group_off_y = std::move(new_off_y);
            group_off_z = std::move(new_off_z);
            group_grab_start_z = std::move(new_start_z);
            group_start_yaw = std::move(new_yaw);
            if (group_grab_index.empty()) {
                group_grab_active = false;
                group_rotate_active = false;
                if (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight ||
                    edit_mode == EditMode::Rotate)
                    edit_mode = EditMode::None;
            }
        }
    };
    auto spawn_editor_object_gpu = [&](map::MapObjectInst& o) {
        if (bng_spawn_hook && bng_spawn_hook(o)) return;
        if (const auto* att = find_attach_for_child(o.id)) {
            sync_attach_child_gpu(*att, o);
            return;
        }
        // Drop stale GPU for this id before respawn (stream recreate).
        for (auto& sp : scene_places) {
            if (sp.object_id != o.id) continue;
            sp.alive = false;
            renderer.set_chunk_visible(sp.chunk, false);
        }
        const auto sco = sco::load_sco(o.sco_path);
        const bool abs_z = sco && sco::skip_terrain_add(*sco);
        const float th = terrain_add(abs_z, o.position.x, o.position.y);
        bool spawned = false;
        if (sco && sco->tree.valid) {
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
            } else if (const auto tex_path = gfx::dx12::resolve_texture_path(o.sco_path, tex_name, cfg.omsi_root)) {
                tex_resolved = *tex_path;
                tree_tex_cache.emplace(tex_key, tex_resolved);
            }
            if (!tex_resolved.empty()) {
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
                if (ok) {
                    ScenePlace sp{};
                    sp.object_id = o.id;
                    sp.chunk = chunk;
                    sp.tile_gx = tile_gx;
                    sp.tile_gy = tile_gy;
                    sp.tx = o.position.x;
                    sp.ty = o.position.y;
                    sp.tz = o.position.z + th;
                    sp.abs_z = abs_z;
                    sp.alive = true;
                    sp.pinned = true;
                    sp.is_tree = true;
                    sp.half_xy = width * 0.5f;
                    sp.height = height;
                    const XMMATRIX world_m =
                        DirectX::XMMatrixScaling(width, width, height) * sco_world_matrix(o, nullptr, th, 0.f, 0.f);
                    renderer.set_chunk_world_matrix(sp.chunk, store_world_transposed(world_m).data());
                    renderer.set_chunk_visible(sp.chunk, true);
                    scene_places.push_back(std::move(sp));
                    spawned = true;
                }
            }
        }
        if (!spawned && sco && !sco->meshes.empty()) {
            const XMMATRIX world_m = sco_world_matrix(o, nullptr, th, 0.f, 0.f);
            if (append_sco_meshes_at(*sco, o.sco_path, world_m, o.id, 0, tile_gx, tile_gy, "[object]", abs_z)) {
                for (auto& sp : scene_places) {
                    if (sp.object_id == o.id && sp.alive) sp.pinned = true;
                }
                spawned = true;
            }
        }
        if (!spawned) {
            place_marker(o.position.x, o.position.y, o.position.z + th, 1.2f, false, o.id, true, true, 0, tile_gx,
                         tile_gy);
            ScenePlace sp{};
            sp.object_id = o.id;
            sp.chunk = end_markers.back().chunk;
            sp.tile_gx = tile_gx;
            sp.tile_gy = tile_gy;
            sp.tx = o.position.x;
            sp.ty = o.position.y;
            sp.tz = o.position.z + th;
            sp.alive = true;
            sp.pinned = true;
            scene_places.push_back(std::move(sp));
        }
    };
#include "modules/mod_marquee.inc.cpp"
    auto sync_spawn_height_from_ui = [&]() {
        map::EditorUiSettings ui{};
        panel.collect_ui_settings(ui);
        spawn_align_pitch_bank = ui.spawn_align_pitch_bank;
        if (ui.spawn_stick_terrain)
            spawn_height_mode = SpawnHeightMode::Terrain;
        else if (ui.spawn_on_meshes)
            spawn_height_mode = SpawnHeightMode::OnMeshes;
        else
            spawn_height_mode = SpawnHeightMode::Default;
    };
    auto exclude_chunks_for_place = [&](std::int32_t object_id, std::int32_t spline_id,
                                        std::vector<std::size_t>& out) {
        out.clear();
        for (const auto& sp : scene_places) {
            if (!sp.alive) continue;
            if (object_id != 0 && sp.object_id == object_id) out.push_back(sp.chunk);
        }
        if (spline_id != 0) {
            if (auto* g = find_spline_gpu(spline_id)) {
                for (const auto c : g->chunks) out.push_back(c);
            }
        }
        for (const auto& mk : end_markers) {
            if (mk.alive) out.push_back(mk.chunk);
        }
        for (const auto& rail : path_rails) {
            if (rail.alive) out.push_back(rail.chunk);
        }
    };
    float mesh_pick_cache_x = 0.f, mesh_pick_cache_y = 0.f, mesh_pick_cache_z = 0.f;
    double mesh_pick_cache_t = -1.0;
    std::int32_t mesh_pick_cache_oid = 0, mesh_pick_cache_sid = 0;
    auto sample_place_world_z = [&](float x, float y, std::int32_t exclude_oid,
                                    std::int32_t exclude_sid) -> float {
        const float terr = sample_terrain_editor_xy(x, y);
        if (spawn_height_mode != SpawnHeightMode::OnMeshes) return terr;
        const double now = glfwGetTime();
        const float dx = x - mesh_pick_cache_x;
        const float dy = y - mesh_pick_cache_y;
        if (mesh_pick_cache_t >= 0.0 && (now - mesh_pick_cache_t) < 0.05 &&
            exclude_oid == mesh_pick_cache_oid && exclude_sid == mesh_pick_cache_sid &&
            dx * dx + dy * dy < 0.25f) {
            return mesh_pick_cache_z;
        }
        std::vector<std::size_t> excl;
        exclude_chunks_for_place(exclude_oid, exclude_sid, excl);
        constexpr float kTop = 8000.f;
        float z = terr;
        if (const auto hit =
                renderer.pick_surface_hit_z(x, y, kTop, 0.f, 0.f, -1.f, excl.empty() ? nullptr : excl.data(),
                                            excl.size())) {
            z = (std::max)(terr, *hit);
        }
        mesh_pick_cache_x = x;
        mesh_pick_cache_y = y;
        mesh_pick_cache_z = z;
        mesh_pick_cache_t = now;
        mesh_pick_cache_oid = exclude_oid;
        mesh_pick_cache_sid = exclude_sid;
        return z;
    };
    auto apply_sco_follow_xy = [&](map::MapObjectInst& o, float x, float y) {
        const float keep_z = o.position.z;
        o.position.x = x;
        o.position.y = y;
        bool abs_z = false;
        if (const sco::SceneryObject* sco = cached_sco(o.sco_path)) abs_z = sco::skip_terrain_add(*sco);
        if (spawn_height_mode == SpawnHeightMode::Default) {
            o.position.z = keep_z;
        } else if (!abs_z) {
            o.position.z = 0.f;
        } else {
            o.position.z = sample_terrain_editor_xy(x, y);
        }
        if (apply_object_surface_follow_fn) apply_object_surface_follow_fn(o, false);
        sync_object_gpu(o);
    };
    auto apply_spline_follow_xy = [&](map::SplineInst& s, float x, float y) {
        s.position.x = x;
        s.position.y = y;
        if (spawn_height_mode == SpawnHeightMode::Default) {
            // Keep draft Z (camera plane / previous) — no per-frame heightfield stick.
            return;
        }
        // Place so the LOWEST profile vertex sits 0.01m above whatever is below
        // (terrain, .sco mesh, or another .sli) — sample under that vertex XY, not only origin.
        if (const auto def = spline::load_sli(s.sli_path)) {
            float min_wz = s.position.z;
            float min_wx = x, min_wy = y;
            bool any = false;
            for (const auto& profile : def->profiles) {
                for (const auto& pp : profile) {
                    const auto samp = spline::evaluate_spline(s, 0.f, pp.x, pp.z, std::nullopt);
                    if (!any || samp.z < min_wz) {
                        any = true;
                        min_wz = samp.z;
                        min_wx = samp.x;
                        min_wy = samp.y;
                    }
                }
            }
            // Also sample a mid-span and end profile point for long/curved segments.
            if (s.length > 1.f) {
                for (float along : {s.length * 0.5f, s.length}) {
                    for (const auto& profile : def->profiles) {
                        for (const auto& pp : profile) {
                            const auto samp = spline::evaluate_spline(s, along, pp.x, pp.z, std::nullopt);
                            if (!any || samp.z < min_wz) {
                                any = true;
                                min_wz = samp.z;
                                min_wx = samp.x;
                                min_wy = samp.y;
                            }
                        }
                    }
                }
            }
            if (!any) min_wz = s.position.z;
            const float surf_at_bottom = sample_place_world_z(min_wx, min_wy, 0, s.id);
            const float surf_at_origin = sample_place_world_z(x, y, 0, s.id);
            const float surf_z = (std::max)(surf_at_bottom, surf_at_origin);
            // Lift so lowest world Z becomes surf_z + 0.01
            s.position.z += (surf_z + 0.01f) - min_wz;
        } else {
            s.position.z = sample_place_world_z(x, y, 0, s.id) + 0.01f;
        }
    };
    apply_object_surface_follow_fn = [&](map::MapObjectInst& o, bool lock_z) {
        const bool is_sa = spline_attach_spawn_ids.count(o.id) != 0;
        const bool want_align = spawn_align_pitch_bank;
        const bool want_lift = spawn_height_mode != SpawnHeightMode::Default;
        if (is_sa && lock_z) {
            if (!want_align && !want_lift) return;
        } else if (lock_z) {
            if (!want_align) return;
        } else if (spawn_height_mode == SpawnHeightMode::Default && !want_align) {
            return;
        }
        const sco::SceneryObject* sco = cached_sco(o.sco_path);
        if (!sco) return;
        bool abs_z = sco::skip_terrain_add(*sco);
        if (is_sa) abs_z = true;
        const float th = abs_z ? 0.f : sample_terrain_editor_xy(o.position.x, o.position.y);
        SurfaceFollowOptions opt{};
        if (spawn_height_mode == SpawnHeightMode::OnMeshes)
            opt.sample_world_z = [&](float sx, float sy) { return sample_place_world_z(sx, sy, o.id, 0); };
        else
            opt.sample_world_z = [&](float sx, float sy) { return sample_terrain_editor_xy(sx, sy); };
        opt.align_pitch_bank = want_align;
        opt.lift_body = want_lift;
        opt.lock_z = lock_z && want_align && !want_lift;
        apply_surface_follow(o, *sco, th, abs_z, opt);
    };
    map::MapObjectInst new_sco_draft{};
#include "modules/mod_streaming.inc.cpp"
    // Boot scenery GPU: drain via UploadScenery (time-sliced) instead of MAINLOAD sync spawn.
    // (the texture warm-up thread keeps running: UploadScenery is time-sliced and loads what is not warm yet;
    //  waiting for it here held the load 5 s on a cold disk)
    if (!boot_gpu_queue.empty() && stream_phase == StreamPhase::Idle) {
        stream_new_objects.clear();
        stream_new_objects.reserve(boot_gpu_queue.size());
        for (const auto& b : boot_gpu_queue)
            stream_new_objects.push_back({b.id, b.tile_gx, b.tile_gy});
        const auto n = stream_new_objects.size();
        boot_gpu_queue.clear();
        stream_i = 0;
        stream_tree_i = 0;
        stream_phase = StreamPhase::UploadScenery;
        stream_status = "GPU boot scenery...";
        log::info("boot UploadScenery kick n=" + std::to_string(n));
    }
#include "modules/mod_omsi_bus.inc.cpp"
#include "modules/mod_ai_traffic.inc.cpp"
#include "modules/mod_parked_cars.inc.cpp"
    auto teleport_camera_to_entrypoint = [&](const map::EntryPoint& ep, const char* via = nullptr) {
        const float ex = ep.position.x - tile_ox;
        const float ey = ep.position.y - tile_oy;
        const float ez = ep.position.z;
        camera_apply_silent = true;
        camera.target_x = ex;
        camera.target_y = ey;
        camera.target_z = ez;
        camera.ground_z = ez;
        camera.radius = 35.f;
        camera.reset_mouse();
        camera_track_last = {ex, ey, ez, camera.radius};
        camera_track_init = true;
        camera_apply_silent = false;
        if (via) {
            log::info(std::string("editor teleport entry via=") + via + " id=" + std::to_string(ep.id) +
                      " name=\"" + ep.name + "\" editor=(" + std::to_string(ex) + "," + std::to_string(ey) + "," +
                      std::to_string(ez) + ") world=(" + std::to_string(ep.position.x) + "," +
                      std::to_string(ep.position.y) + ")");
        }
        const std::string label = ep.name.empty() ? std::to_string(ep.id) : ep.name;
        const bool streaming = request_tile_stream_for_entrypoint(ep);
        panel.set_status("Entrypoint: " + label + (streaming ? " — cargando tiles…" : ""));
    };

    // Catálogo: browser ImGui (docs/MAP_EDITOR_CATALOG_PICKER.md). Ultralight aún manda load_catalog_* por eventos.
    // Scan runs off-main: logfile showed hud_build ~9s stall on sync scan+apply (walked=1690).
    struct CatalogScanJob {
        std::mutex mu;
        std::atomic<bool> busy{false};
        bool ready = false;
        bool for_spline = false;
        map::CatalogScanResult result{};
        std::filesystem::path dir;
    } catalog_scan_job;
    auto catalog_apply_picked = [&](const bool for_spline, const std::vector<std::filesystem::path>& picked,
                                    const bool scan_truncated) {
        const char* ext = for_spline ? ".sli" : ".sco";
        auto normalize_catalog = [&](std::filesystem::path p) {
            std::error_code ec;
            auto abs = std::filesystem::weakly_canonical(p, ec);
            if (ec) abs = std::filesystem::absolute(p, ec);
            if (!ec) p = std::move(abs);
            if (!cfg.omsi_root.empty()) {
                const auto rel = std::filesystem::relative(p.lexically_normal(), cfg.omsi_root.lexically_normal(), ec);
                if (!ec && !rel.empty() && rel.native().find(L"..") == std::wstring::npos) return cfg.omsi_root / rel;
            }
            return p;
        };
        auto& extra = for_spline ? extra_catalog_sli : extra_catalog_sco;
        std::unordered_set<std::string> seen;
        seen.reserve(extra.size() * 2 + picked.size());
        for (const auto& e : extra) seen.insert(e.lexically_normal().string());
        int added = 0;
        for (const auto& p : picked) {
            const auto e = p.extension().string();
            if (for_spline) {
                if (_stricmp(e.c_str(), ".sli") != 0) continue;
            } else if (_stricmp(e.c_str(), ".sco") != 0) {
                continue;
            }
            const auto n = normalize_catalog(p);
            if (!seen.insert(n.lexically_normal().string()).second) continue;
            extra.push_back(n);
            ++added;
        }
        if (added > 0) {
            refresh_editor_lists();
            if (for_spline) {
                sticky_sli_path = extra.back();
                panel.remember_catalog_path(sticky_sli_path);
            } else {
                sticky_sco_path = extra.back();
                panel.remember_catalog_path(sticky_sco_path);
            }
            panel.set_status(std::string("Catalog: +") + std::to_string(added) + " " + ext +
                             (scan_truncated ? " (truncado)" : ""));
            log::info("editor catalog load added=" + std::to_string(added) + " ext=" + ext);
        } else if (!picked.empty()) {
            panel.set_status("Catalog: ya en la lista");
        }
    };

    auto on_catalog_file_picked = [&](std::filesystem::path p, const bool for_spline) {
        catalog_apply_picked(for_spline, {std::move(p)}, false);
    };
    auto on_catalog_dir_picked = [&](std::filesystem::path dir, const bool for_spline) {
        if (catalog_scan_job.busy.load()) {
            panel.set_status("Escaneo en curso…");
            return;
        }
        const char* ext = for_spline ? ".sli" : ".sco";
        catalog_scan_job.busy.store(true);
        {
            std::lock_guard<std::mutex> lock(catalog_scan_job.mu);
            catalog_scan_job.ready = false;
            catalog_scan_job.for_spline = for_spline;
            catalog_scan_job.dir = dir;
            catalog_scan_job.result = {};
        }
        panel.set_status("Escaneando " + dir.filename().string() + " (fondo)…");
        log::info(std::string("editor catalog scan start async ext=") + ext + " dir=" + dir.string());
        std::thread([&, dir, for_spline, ext = std::string(ext)]() {
            auto scan = map::scan_catalog_files(dir, ext.c_str());
            for (auto& p : scan.paths) {
                std::error_code ec;
                auto n = std::filesystem::weakly_canonical(p, ec);
                if (!ec) p = std::move(n);
            }
            const std::size_t found = scan.paths.size();
            const std::size_t walked = scan.entries_walked;
            const double walk_ms = scan.walk_ms;
            {
                std::lock_guard<std::mutex> lock(catalog_scan_job.mu);
                catalog_scan_job.result = std::move(scan);
                catalog_scan_job.for_spline = for_spline;
                catalog_scan_job.ready = true;
            }
            catalog_scan_job.busy.store(false);
            log::info("editor catalog scan worker done found=" + std::to_string(found) +
                      " walked=" + std::to_string(walked) + " ms=" + std::to_string(walk_ms) +
                      " ext=" + ext);
        }).detach();
    };
    auto pump_catalog_scan_job = [&]() {
        map::CatalogScanResult scan;
        bool for_spline = false;
        {
            std::lock_guard<std::mutex> lock(catalog_scan_job.mu);
            if (!catalog_scan_job.ready) return;
            catalog_scan_job.ready = false;
            scan = std::move(catalog_scan_job.result);
            for_spline = catalog_scan_job.for_spline;
        }
        const char* ext = for_spline ? ".sli" : ".sco";
        catalog_apply_picked(for_spline, scan.paths, scan.truncated);
        log::info("editor catalog scan apply found=" + std::to_string(scan.paths.size()) +
                  " walked=" + std::to_string(scan.entries_walked) +
                  " walk_ms=" + std::to_string(scan.walk_ms) + " ext=" + ext);
    };

    auto open_catalog_browser_from_cev = [&](const ui::EditorSidePanelState& cev) {
        if (!cev.load_catalog_file && !cev.load_catalog_directory && !cev.spawn_random_scan_folder) return;
        bool for_spline = panel.current_tool() == ui::EditorTool::Spline;
        bool scan_dir = cev.load_catalog_directory;
        if (cev.spawn_random_scan_folder) {
            panel.imgui_state().catalog_browser_to_spawn_random = true;
            for_spline = false;
            scan_dir = true;
        }
        std::filesystem::path initial = cfg.global.root;
        if (!cfg.omsi_root.empty())
            initial = cfg.omsi_root / (for_spline ? "Splines" : "Sceneryobjects");
        std::error_code ec;
        if (initial.empty() || !std::filesystem::is_directory(initial, ec)) initial = cfg.omsi_root;
        ui::catalog_browser_open(panel.imgui_state(), initial, for_spline, scan_dir);
        log::info(std::string("editor catalog browser open scan=") + (scan_dir ? "1" : "0") +
                  " ext=" + (for_spline ? ".sli" : ".sco") +
                  (cev.spawn_random_scan_folder ? " spawn_random=1" : ""));
    };

    auto resolve_catalog_abs = [&](const std::filesystem::path& p) -> std::filesystem::path {
        if (p.empty()) return {};
        std::error_code ec;
        auto try_file = [&](std::filesystem::path candidate) -> std::filesystem::path {
            if (candidate.empty()) return {};
            candidate = std::filesystem::absolute(candidate, ec);
            if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
            return {};
        };
        if (auto resolved = try_file(p); !resolved.empty()) return resolved;
        if (!cfg.omsi_root.empty()) {
            if (auto resolved = try_file(cfg.omsi_root / p); !resolved.empty()) return resolved;
        }
        log::warn("resolve_catalog_abs: not found " + p.string());
        return {};
    };
    auto catalog_path_on_tab = [&](const bool spline_tab) -> std::filesystem::path {
        const int sel = spline_tab ? panel.catalog_spline_sel() : panel.catalog_object_sel();
        if (sel < 0) return {};
        const auto& items = spline_tab ? panel.catalog_splines() : panel.catalog_objects();
        if (static_cast<std::size_t>(sel) >= items.size()) return {};
        return items[static_cast<std::size_t>(sel)].path;
    };
    auto accept_catalog_ext = [&](std::filesystem::path p, const bool spline_tab) -> std::filesystem::path {
        p = resolve_catalog_abs(p);
        if (p.empty()) return {};
        const auto ext = p.extension().string();
        if (spline_tab) {
            if (_stricmp(ext.c_str(), ".sli") != 0) return {};
        } else if (_stricmp(ext.c_str(), ".sco") != 0) {
            return {};
        }
        return p;
    };
    auto pick_catalog_path = [&](const ui::EditorSidePanelState& /*cev*/) -> std::filesystem::path {
        const bool spline_tab = panel.current_tool() == ui::EditorTool::Spline;
        if (auto path = accept_catalog_ext(catalog_path_on_tab(spline_tab), spline_tab); !path.empty())
            return path;
        if (spline_tab) {
            if (auto path = accept_catalog_ext(sticky_sli_path, true); !path.empty()) return path;
            if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                if (auto path =
                        accept_catalog_ext(edit_splines[static_cast<std::size_t>(selected_spline)].sli_path, true);
                    !path.empty())
                    return path;
            std::int32_t want_id = 0;
            if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                want_id = edit_splines[static_cast<std::size_t>(selected_spline)].id;
            else if (panel.imgui_spline_edit().valid)
                want_id = panel.imgui_spline_edit().id;
            for (const auto& it : panel.spline_instances()) {
                if (want_id == 0) break;
                if (it.id != want_id) continue;
                if (auto path = accept_catalog_ext(it.path, true); !path.empty()) return path;
                break;
            }
        } else {
            if (auto path = accept_catalog_ext(sticky_sco_path, false); !path.empty()) return path;
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                if (auto path =
                        accept_catalog_ext(edit_objects[static_cast<std::size_t>(selected_object)].sco_path, false);
                    !path.empty())
                    return path;
            std::int32_t want_id = 0;
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                want_id = edit_objects[static_cast<std::size_t>(selected_object)].id;
            else if (panel.imgui_object_edit().valid)
                want_id = panel.imgui_object_edit().id;
            for (const auto& it : panel.object_instances()) {
                if (want_id == 0) break;
                if (it.id != want_id) continue;
                if (auto path = accept_catalog_ext(it.path, false); !path.empty()) return path;
                break;
            }
        }
        return {};
    };

    auto handle_catalog_panel_events = [&](const ui::EditorSidePanelState& cev) {
        if (cev.load_catalog_file || cev.load_catalog_directory || cev.spawn_random_scan_folder) {
            open_catalog_browser_from_cev(cev);
            return;
        }
        if (cev.open_catalog_folder || cev.edit_catalog_file) {
            const auto path = pick_catalog_path(cev);
            const HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
            if (!path.empty()) {
                std::string err;
                if (cev.edit_catalog_file) {
                    if (win32::shell_open_notepad(path, owner, &err))
                        panel.set_status("Notepad: " + path.filename().string());
                    else
                        panel.set_status("Error Notepad: " + err);
                } else if (win32::shell_open_folder_select_file(path, owner, &err))
                    panel.set_status("Carpeta: " + path.parent_path().filename().string());
                else
                    panel.set_status("Error carpeta: " + err);
            } else {
                panel.set_status("Selecciona un .sco/.sli en Tipos o una instancia en el mapa");
                log::warn("editor catalog folder/edit: no path (Tipos o instancia)");
            }
        }
        if (cev.reload_catalog_file) {
            auto path = pick_catalog_path(cev);
            if (path.empty()) {
                panel.set_status("Recargar: selecciona un .sco/.sli en Tipos");
                log::warn("editor reload: select a type in Tipos first");
            } else {
                const auto ext = path.extension().string();
                auto same_file = [&](const std::filesystem::path& a, const std::filesystem::path& b) {
                    std::error_code ec;
                    if (std::filesystem::equivalent(a, b, ec)) return true;
                    return _stricmp(a.filename().string().c_str(), b.filename().string().c_str()) == 0 &&
                           a.parent_path().filename() == b.parent_path().filename();
                };
                int n = 0;
                if (ext == ".sli" || ext == ".SLI") {
                    spline::invalidate_sli_cache(path);
                    ui::invalidate_catalog_thumb_cache(path);
                    catalog_reload_q.object_ids.clear();
                    catalog_reload_q.spline_ids.clear();
                    catalog_reload_q.pending_sa_respawn = false;
                    for (auto& s : edit_splines) {
                        if (!same_file(s.sli_path, path)) continue;
                        catalog_reload_q.spline_ids.push_back(s.id);
                        ++n;
                    }
                    if (show_path_rails) {
                        for (auto& s : edit_splines) {
                            if (!same_file(s.sli_path, path)) continue;
                            mark_path_rails_dirty_spline_id(s.id);
                        }
                    }
                    catalog_reload_q.pending_end_sync = n > 0;
                    catalog_reload_q.status_label = "Recargado .sli x" + std::to_string(n);
                    log::info("editor reload .sli queued=" + std::to_string(n) + " " + path.filename().string());
                    if (n == 0) panel.set_status("Recargado .sli x0");
                } else if (ext == ".sco" || ext == ".SCO") {
                    sco::invalidate_sco_cache(path);
                    ui::invalidate_catalog_thumb_cache(path);
                    catalog_reload_q.object_ids.clear();
                    catalog_reload_q.spline_ids.clear();
                    for (auto& o : edit_objects) {
                        if (!same_file(o.sco_path, path)) continue;
                        catalog_reload_q.object_ids.push_back(o.id);
                        ++n;
                    }
                    catalog_reload_q.pending_sa_respawn = false;
                    catalog_reload_q.sco_for_sa.clear();
                    for (const auto& e : edit_spline_attachments) {
                        if (same_file(e.att.sco_path, path)) {
                            catalog_reload_q.pending_sa_respawn = true;
                            catalog_reload_q.sco_for_sa = path;
                            break;
                        }
                    }
                    if (show_path_rails) {
                        for (auto& o : edit_objects) {
                            if (!same_file(o.sco_path, path)) continue;
                            mark_path_rails_dirty(o.id, true);
                        }
                    }
                    catalog_reload_q.pending_end_sync = n > 0;
                    catalog_reload_q.status_label = "Recargado .sco x" + std::to_string(n);
                    log::info("editor reload .sco queued=" + std::to_string(n) + " " + path.filename().string());
                    if (n == 0) panel.set_status("Recargado .sco x0");
                }
            }
        }
    };
#include "modules/mod_mesh_sco.inc.cpp"
