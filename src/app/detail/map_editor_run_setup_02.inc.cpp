    log::info("editor side panel open");
    // Declared early so path-rails / remesh pumps (this file + setup_03) can adapt to last frame ms.
    double g_stream_last_render_ms = 10.0;
    // Bumped once per editor frame: per-frame memo key for picks called from several places.
    std::uint64_t g_editor_frame_id = 0;
    double g_editor_frame_wall_ms = 16.0;

    std::function<void(std::int32_t)> path_rails_spline_commit_hook;
    input::LunaCamera camera{};
    camera.z_up = true;
    camera.mirror_x = true; // same as omsi-tt-viewer VIEW_MIRROR_X (OMSI Editor top-down)
    camera.editor_nav = true;
    camera.ground_z = 0.f;
    camera.sample_ground_xy = [&](float x, float y) -> std::optional<float> {
        return sample_terrain_editor_xy_opt(x, y);
    };
    auto update_renderer_tile_cull = [&]() {
        if (cfg.global.world_coordinates) {
            const float wx = tile_ox + camera.target_x;
            const float wy = tile_oy + camera.target_y;
            const auto& layout = world.tile_layout();
            const auto cell = map::grid_cell_at_world_xy(cfg.global, layout, wx, wy);
            if (cell.resolved) {
                const auto rect =
                    map::grid_tile_rect_at(cfg.global, layout, cell.grid_x, cell.grid_y);
                renderer.set_tile_cull_cell(rect.origin_x - tile_ox, rect.origin_y - tile_oy, rect.width_m,
                                            rect.height_m);
                return;
            }
        }
        renderer.set_tile_cull_cell(0.f, 0.f, tile_size_m, tile_size_m);
    };
    const float extent = terrain->size_meters;
    camera.frame_omsi_bounds(0.f, 0.f, 0.f, extent, extent, (std::max)(1.f, terrain->max_height));
    camera.radius = (std::max)(camera.radius, extent * 0.75f);
    // Camera on spawn focus (entry/stream) in editor-local coords (center tile at 0,0).
    {
        const auto& sp = world.spawn();
        camera.target_x = sp.position.x - tile_ox;
        camera.target_y = sp.position.y - tile_oy;
        camera.target_z = sp.position.z;
        camera.radius = 40.f;
        if (cfg.restore_orbit) {
            camera.radius = cfg.restore_radius;
            camera.theta = cfg.restore_theta;
            camera.phi = cfg.restore_phi;
        }
    }
    auto visual_in_radius = [&](const float x, const float y) -> bool {
        if (editor_visual_radius_m <= 0.f) return true;
        const float dx = x - camera.target_x;
        const float dy = y - camera.target_y;
        return dx * dx + dy * dy <= editor_visual_radius_m * editor_visual_radius_m;
    };
    auto visual_in_radius_spline = [&](const map::SplineInst& sp) -> bool {
        if (editor_visual_radius_m <= 0.f) return true;
        return spline::spline_in_radius_xy(sp, camera.target_x, camera.target_y, editor_visual_radius_m);
    };
    {
        float cx = 0.f, cy = 0.f, fw = 0.f, fh = 0.f;
        input::cursor_to_framebuffer(window, cx, cy, fw, fh);
        float sx = 1.f, sy = 1.f;
        glfwGetWindowContentScale(window, &sx, &sy);
        log::info("editor DPI: content_scale=(" + std::to_string(sx) + "," + std::to_string(sy) +
                  ") fb=" + std::to_string(static_cast<int>(fw)) + "x" + std::to_string(static_cast<int>(fh)));
    }
    log::info("editor cam target=(" + std::to_string(camera.target_x) + "," + std::to_string(camera.target_y) +
              "," + std::to_string(camera.target_z) + ") radius=" + std::to_string(camera.radius) +
              " tile=" + std::to_string(extent) + "m (RMB=goto / RMB-drag=zoom / LMB=pick / MMB=orbit)");

    int selected_object = -1;
    int selected_spline = -1;
    // Last picked/catalog type — kept after deselect so N can spawn the same .sco/.sli.
    std::filesystem::path sticky_sco_path;
    std::filesystem::path sticky_sli_path;

    // global.cfg [entrypoints] edits stay in memory until Ctrl+S / finish_save_job.
    bool entrypoints_dirty = false;
    // Declared early: flush_entrypoints_to_disk_if_dirty lambda references it (set later on Ctrl+S).
    bool save_overwrite_original = false;

    auto sync_object_entrypoint_to_global = [&](const map::MapObjectInst& o, std::int32_t gx, std::int32_t gy) {
        const auto* sco = sco::load_sco(o.sco_path);
        if (!sco || sco->special != sco::ScoSpecial::EntryPoint) return;
        map::EntryPoint ep{};
        ep.type = 0;
        ep.id = o.id;
        ep.link_id = 0;
        ep.position_local = o.position;
        ep.position = o.position;
        {
            const float yaw = o.rotation_deg.x * 0.01745329251f * 0.5f;
            ep.rotation_quat.x = 0.f;
            ep.rotation_quat.y = std::sin(yaw);
            ep.rotation_quat.z = 0.f;
            ep.rotation_quat.w = std::cos(yaw);
        }
        ep.category = map::entrypoint_category_for_tile(cfg.global, gx, gy);
        if (!o.strings.empty() && !o.strings[0].empty())
            ep.name = o.strings[0];
        else
            ep.name = o.sco_path.stem().string();
        map::upsert_global_entrypoint(cfg.global, ep);
        entrypoints_dirty = true;
        panel.set_entrypoints(cfg.global.entrypoints);
        log::info("entrypoint sync (mem) id=" + std::to_string(ep.id) + " cat=" + std::to_string(ep.category) +
                  " name=\"" + ep.name + "\"");
    };

    auto flush_entrypoints_to_disk_if_dirty = [&]() {
        if (!entrypoints_dirty) return;
        if (cfg.global.global_cfg_path.empty()) {
            log::warn("entrypoint save skipped — no global_cfg_path");
            return;
        }
        bool ok = map::save_global_entrypoints(cfg.global.global_cfg_path, cfg.global.entrypoints);
        // When user chose "replace original", also write live maps/<Name>/global.cfg (not only *_actual).
        if (ok && save_overwrite_original) {
            std::filesystem::path live_cfg;
            if (cfg.workspace && !cfg.workspace->actual_dir.empty()) {
                const auto live_dir = map::workspace_live_map_dir(*cfg.workspace);
                if (!live_dir.empty()) live_cfg = live_dir / "global.cfg";
            }
            if (live_cfg.empty()) {
                const auto live_dir = map::live_map_dir_from_folder(cfg.global.root);
                if (!live_dir.empty()) live_cfg = live_dir / "global.cfg";
            }
            if (!live_cfg.empty() && live_cfg != cfg.global.global_cfg_path) {
                map::backup_file_timestamped_bak(live_cfg);
                if (!map::save_global_entrypoints(live_cfg, cfg.global.entrypoints)) {
                    log::warn("global.cfg live entrypoints save failed: " + live_cfg.string());
                    ok = false;
                } else {
                    log::info("global.cfg entrypoints saved to live " + live_cfg.string());
                }
            }
        }
        if (ok) {
            entrypoints_dirty = false;
            log::info("global.cfg entrypoints saved count=" +
                      std::to_string(cfg.global.entrypoints.size()));
        } else {
            log::warn("global.cfg entrypoints save failed");
        }
    };



    auto fill_sco_map_extras = [&](map::MapObjectInst& o) {
        const auto* sco = sco::load_sco(o.sco_path);
        if (!sco) return;
        const int n = sco->extra_text_slots();
        if (n <= 0) return;
        while (static_cast<int>(o.strings.size()) < n) o.strings.emplace_back();
        if (o.string_line_count < n) o.string_line_count = n;
    };

    auto spline_attach_entry_for_object = [&](const std::int32_t oid) -> std::optional<std::size_t> {
        const auto it = spline_attach_obj_to_entry.find(oid);
        if (it == spline_attach_obj_to_entry.end()) return std::nullopt;
        return it->second;
    };
    auto spline_attach_group_object_ids = [&](const std::size_t entry_i) {
        std::vector<std::int32_t> ids;
        for (const auto& kv : spline_attach_obj_to_entry)
            if (kv.second == entry_i) ids.push_back(kv.first);
        if (entry_i < edit_spline_attachments.size()) {
            const std::int32_t primary = edit_spline_attachments[entry_i].att.instance_id;
            if (primary > 0 &&
                std::find(ids.begin(), ids.end(), primary) == ids.end())
                ids.push_back(primary);
        }
        return ids;
    };
    auto highlight_spline_attach_group = [&](const std::size_t entry_i, const float amt, const float r,
                                             const float g, const float b) {
        for (const auto oid : spline_attach_group_object_ids(entry_i)) {
            for (const auto& sp : scene_places) {
                if (sp.alive && sp.object_id == oid)
                    renderer.set_chunk_highlight(sp.chunk, amt, r, g, b);
            }
        }
    };

    // OMSI panel: tile-local XY = world − tile_origin (invariant across editor recenter).
    auto editor_to_world_xy = [&](const float ex, const float ey) {
        return std::pair{ex + tile_ox, ey + tile_oy};
    };
    auto world_to_editor_xy = [&](const float wx, const float wy) {
        return std::pair{wx - tile_ox, wy - tile_oy};
    };
    auto tile_local_from_world = [&](const float wx, const float wy, const std::int32_t gx,
                                     const std::int32_t gy) {
        const auto o = map::tile_origin_xy(world.tile_layout(), gx, gy);
        return std::pair{wx - o.first, wy - o.second};
    };
    auto world_from_tile_local = [&](const float lx, const float ly, const std::int32_t gx,
                                     const std::int32_t gy) {
        const auto o = map::tile_origin_xy(world.tile_layout(), gx, gy);
        return std::pair{lx + o.first, ly + o.second};
    };
    auto tile_at_world_xy = [&](const float wx, const float wy) {
        if (const auto ref = world::find_existing_tile_at_world(cfg.global, wx, wy))
            return std::pair{ref->grid_x, ref->grid_y};
        for (const auto& tile : world.tiles()) {
            float size_m = map::kStandardTileMeters;
            if (const auto* m = world.tile_layout().find_grid(tile.grid_x, tile.grid_y)) size_m = m->size_meters;
            const auto o = map::tile_origin_xy(world.tile_layout(), tile.grid_x, tile.grid_y);
            if (wx >= o.first - 1.f && wy >= o.second - 1.f && wx <= o.first + size_m + 1.f &&
                wy <= o.second + size_m + 1.f)
                return std::pair{tile.grid_x, tile.grid_y};
        }
        return std::pair{tile_gx, tile_gy};
    };
    auto object_home_tile = [&](const std::int32_t oid, const float wx, const float wy) {
        if (const auto it = spline_attach_obj_to_entry.find(oid); it != spline_attach_obj_to_entry.end()) {
            if (it->second < edit_spline_attachments.size())
                return std::pair{edit_spline_attachments[it->second].tile_gx,
                                 edit_spline_attachments[it->second].tile_gy};
        }
        return tile_at_world_xy(wx, wy);
    };
    auto log_watch_scos = [&](std::string_view tag) {
        for (const auto& o : edit_objects) {
            const std::string fname = o.sco_path.filename().string();
            if (o.id != 9 && fname.find("NAZCA") == std::string::npos) continue;
            const auto [wx, wy] = editor_to_world_xy(o.position.x, o.position.y);
            const auto [hgx, hgy] = object_home_tile(o.id, wx, wy);
            const auto [lx, ly] = tile_local_from_world(wx, wy, hgx, hgy);
            log::info("watchSco tag=" + std::string(tag) + " id=" + std::to_string(o.id) + " sco=" + fname +
                      " editor=(" + std::to_string(o.position.x) + "," + std::to_string(o.position.y) + ")" +
                      " world=(" + std::to_string(wx) + "," + std::to_string(wy) + ") local=(" +
                      std::to_string(lx) + "," + std::to_string(ly) + ") home=(" + std::to_string(hgx) + "," +
                      std::to_string(hgy) + ") session_ox=" + std::to_string(tile_ox));
        }
    };

    auto show_object_at = [&](std::size_t idx) {
        if (idx >= edit_objects.size()) return;
        const auto& o = edit_objects[idx];
        sticky_sco_path = o.sco_path;
        panel.remember_catalog_path(o.sco_path);
        const auto [wx, wy] = editor_to_world_xy(o.position.x, o.position.y);
        const auto [home_gx, home_gy] = object_home_tile(o.id, wx, wy);
        const auto [local_x, local_y] = tile_local_from_world(wx, wy, home_gx, home_gy);
        ui::EditorObjectProps p{};
        p.valid = true;
        p.id = o.id;
        p.varparent_id = o.varparent_id;
        p.sco_name = o.sco_path.filename().string();
        p.x = local_x;
        p.y = local_y;
        p.z = o.position.z; // map offset above terrain (OMSI map.cfg Z)
        p.rot = o.rotation_deg.x;
        p.pitch = o.rotation_deg.y;
        p.bank = o.rotation_deg.z;
        p.spline_align_terrain = o.align_terrain;
        p.strings = o.strings;
        p.text_slot_count = o.string_line_count;
        if (p.text_slot_count <= 0 && !o.strings.empty())
            p.text_slot_count = static_cast<int>(o.strings.size());
        while (static_cast<int>(p.strings.size()) < p.text_slot_count) p.strings.emplace_back();
        for (const auto& a : edit_attachments) {
            if (a.child_id == o.id) {
                p.parent_id = a.parent_id;
                p.attach_kind = ui::EditorObjectProps::AttachKind::AttachObj;
                p.attach_point = a.attach_point_index;
                if (p.varparent_id == 0) p.varparent_id = a.varparent_id;
                p.text_slot_count = a.object_text_line_count;
                if (p.text_slot_count <= 0)
                    p.text_slot_count = static_cast<int>(a.object_text_lines.size());
                p.strings = a.object_text_lines;
                while (static_cast<int>(p.strings.size()) < p.text_slot_count) p.strings.emplace_back();
                break;
            }
        }
        if (const auto sa_it = spline_attach_obj_to_entry.find(o.id); sa_it != spline_attach_obj_to_entry.end()) {
            if (sa_it->second < edit_spline_attachments.size()) {
                const auto& entry = edit_spline_attachments[sa_it->second];
                p.id = entry.att.instance_id;
                p.attach_kind = ui::EditorObjectProps::AttachKind::SplineAttach;
                p.spline_repeat_step = entry.att.repeat_step;
                p.spline_repeat_range = entry.att.repeat_range;
                p.spline_index = entry.att.spline_index;
                p.parent_spline_id = entry.parent_spline_id;
                p.text_slot_count = entry.att.object_text_line_count;
                if (p.text_slot_count <= 0)
                    p.text_slot_count = static_cast<int>(entry.att.object_text_lines.size());
                if (const auto sco_sa = sco::load_sco(o.sco_path))
                    if (p.text_slot_count < sco_sa->extra_text_slots())
                        p.text_slot_count = sco_sa->extra_text_slots();
                p.strings = entry.att.object_text_lines;
                while (static_cast<int>(p.strings.size()) < p.text_slot_count) p.strings.emplace_back();
                if (const map::SplineInst* sp = resolve_edit_spline_for_attach(entry)) {
                    const auto panel_coords = map::spline_attach_to_panel(*sp, entry.att);
                    p.x = panel_coords.x;
                    p.y = panel_coords.y;
                    p.z = panel_coords.z;
                    p.rot = panel_coords.rot;
                    p.pitch = entry.att.rotation_x;
                    p.bank = entry.att.rotation_z;
                    p.spline_tangential = entry.att.tangential;
                    p.spline_align_terrain = entry.att.align_terrain;
            const auto along = map::enumerate_spline_attachment_along_chain(entry.att, *sp, edit_splines);
                    p.spline_attach_count = static_cast<int>(along.size());
                    p.parent_spline_id = sp->id;
                    log::info("splineAttach panel id=" + std::to_string(o.id) + " count=" +
                              std::to_string(p.spline_attach_count) + " offsetAlong=" +
                              std::to_string(entry.att.offset_along) + " localLength=" +
                              std::to_string(map::spline_local_length(*sp)) + " startAlong=" +
                              std::to_string(entry.att.offset_along - map::spline_local_length(*sp)) +
                              " step=" + std::to_string(entry.att.repeat_step) + " range=" +
                              std::to_string(entry.att.repeat_range));
                } else if (entry.att.repeat_step > 0.001f && entry.att.repeat_range > 0.001f)
                    p.spline_attach_count =
                        static_cast<int>(entry.att.repeat_range / entry.att.repeat_step + 0.999f);
                else
                    p.spline_attach_count = 1;
                if (p.varparent_id == 0) p.varparent_id = entry.att.varparent_id;
            }
        }
        std::string tags;
        if (const auto sco = sco::load_sco(o.sco_path)) {
            p.string_labels = sco->string_var_names;
            if (p.attach_kind != ui::EditorObjectProps::AttachKind::SplineAttach) {
                if (p.text_slot_count <= 0 && !p.strings.empty())
                    p.text_slot_count = static_cast<int>(p.strings.size());
                else if (p.text_slot_count <= 0)
                    p.text_slot_count = sco->extra_text_slots();
                while (static_cast<int>(p.strings.size()) < p.text_slot_count) p.strings.emplace_back();
            }
            if (sco->abs_height) tags += " [absHeight]";
            if (sco->special == sco::ScoSpecial::HelpArrow) tags += " [helparrow]";
            if (sco->special == sco::ScoSpecial::TrafficLight) tags += " [trafficlight]";
            if (sco->traffic_lights_group) tags += " [traffic_lights_group]";
            if (!sco->script_files.empty()) tags += " [script]";
            if (!sco->lights.empty()) tags += " [light_enh]";
            if (sco->special == sco::ScoSpecial::TrafficLight) {
                p.string_labels = {"Nr. Traffic Light"};
                if (p.text_slot_count <= 0) p.text_slot_count = 1;
                while (static_cast<int>(p.strings.size()) < p.text_slot_count) p.strings.emplace_back();
                const std::int32_t vp = o.varparent_id != 0 ? o.varparent_id : p.varparent_id;
                if (vp != 0) {
                    for (const auto& po : edit_objects) {
                        if (po.id != vp) continue;
                        if (const auto psco = sco::load_sco(po.sco_path); psco && psco->traffic_lights_group) {
                            for (const auto& tl : psco->traffic_lights_group->lights)
                                p.traffic_light_names.push_back(tl.name);
                        }
                        break;
                    }
                }
            }
        }
        // Selecting must not restomp GPU: previous path rebuilt the world matrix without
        // o3d-root / tree scale and re-added terrain, so clicks made meshes jump or vanish.
        {
            bool any_gpu = false;
            int n_places = 0;
            for (auto& sp : scene_places) {
                if (sp.object_id != o.id) continue;
                ++n_places;
                const bool has_gpu = sp.alive && renderer.chunk_has_gpu(sp.chunk);
                log::info("select gpu id=" + std::to_string(o.id) + " sco=" + o.sco_path.filename().string() +
                          " place alive=" + (sp.alive ? "1" : "0") + " pinned=" + (sp.pinned ? "1" : "0") +
                          " tile=(" + std::to_string(sp.tile_gx) + "," + std::to_string(sp.tile_gy) +
                          ") proto=" + (gpu_proto_chunks.count(sp.chunk) ? "1" : "0") + " " +
                          renderer.chunk_debug_line(sp.chunk));
                if (sp.alive && !has_gpu) {
                    log::warn("select: ScenePlace alive WITHOUT GPU buffers id=" + std::to_string(o.id) +
                              " chunk=" + std::to_string(sp.chunk) + " (released/clone of released proto)");
                    sp.alive = false;
                    renderer.set_chunk_visible(sp.chunk, false);
                }
                if (has_gpu) any_gpu = true;
            }
            if (!any_gpu) {
                log::warn("select: no GPU ScenePlace for id=" + std::to_string(o.id) + " sco=" +
                          o.sco_path.filename().string() + " places=" + std::to_string(n_places) +
                          " sa=" + (spline_attach_spawn_ids.count(o.id) ? "1" : "0") + " — trying force spawn");
                auto& mut = edit_objects[idx];
                const auto [tgx, tgy] = tile_grid_for_object_id(mut.id);
                spawn_one_object_gpu(mut, tgx, tgy);
                if (object_has_alive_gpu(mut.id))
                    log::info("select: force-spawned GPU meshes for id=" + std::to_string(mut.id));
            }
        }
        panel.show_object(p);
        bool abs_z = false;
        if (const auto sco = sco::load_sco(o.sco_path)) abs_z = sco::skip_terrain_add(*sco);
        for (const auto& a : edit_attachments)
            if (a.child_id == o.id) {
                abs_z = true;
                tags += " [attachObj]";
                break;
            }
        if (spline_attach_spawn_ids.count(o.id)) tags += " [splineAttach]";
        if (o.varparent_id != 0) {
            tags += " [varparent=" + std::to_string(o.varparent_id) + "]";
            const map::MapObjectInst* vp = nullptr;
            for (const auto& po : edit_objects) {
                if (po.id == o.varparent_id) {
                    vp = &po;
                    break;
                }
            }
            if (vp)
                tags += " parent_sco=" + vp->sco_path.filename().string();
            else
                tags += " parent_missing";
        }
        const float terr = abs_z ? 0.f : sample_terrain_editor_xy(o.position.x, o.position.y);
        const float world_z = o.position.z + terr;
        char buf[320]{};
        std::snprintf(buf, sizeof(buf),
                      "id=%d %s  tile=(%d,%d)  local=(%.3g,%.3g)  world=(%.3g,%.3g)  editor=(%.3g,%.3g)  "
                      "z_map=%.3g  terr=%.3g  world_z=%.3g%s",
                      o.id, o.sco_path.filename().string().c_str(), home_gx, home_gy,
                      static_cast<double>(local_x), static_cast<double>(local_y),
                      static_cast<double>(wx), static_cast<double>(wy),
                      static_cast<double>(o.position.x), static_cast<double>(o.position.y),
                      static_cast<double>(o.position.z), static_cast<double>(terr),
                      static_cast<double>(world_z), tags.c_str());
        panel.set_selection_text(buf);
        if (o.id == 9 || o.sco_path.filename().string().find("NAZCA") != std::string::npos)
            log_watch_scos("panelShow");
    };
    auto apply_varparent = [&](map::MapObjectInst& child, std::int32_t parent_code) {
        child.varparent_id = parent_code;
        for (auto& a : edit_attachments) {
            if (a.child_id == child.id) a.varparent_id = parent_code;
        }
        if (const auto sa_it = spline_attach_obj_to_entry.find(child.id);
            sa_it != spline_attach_obj_to_entry.end() && sa_it->second < edit_spline_attachments.size())
            edit_spline_attachments[sa_it->second].att.varparent_id = parent_code;
        scenery_scripts.sync(edit_objects, cfg.omsi_root);
    };
    auto show_spline_attachment_entry = [&](std::size_t entry_i) {
        if (entry_i >= edit_spline_attachments.size()) return;
        const auto& entry = edit_spline_attachments[entry_i];
        std::size_t pick_i = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < edit_objects.size(); ++i) {
            const auto ei = spline_attach_entry_for_object(edit_objects[i].id);
            if (!ei || *ei != entry_i) continue;
            if (edit_objects[i].id == entry.att.instance_id) {
                pick_i = i;
                break;
            }
            if (pick_i == static_cast<std::size_t>(-1)) pick_i = i;
        }
        if (pick_i != static_cast<std::size_t>(-1)) {
            selected_object = static_cast<int>(pick_i);
            selected_spline = -1;
            show_object_at(pick_i);
            return;
        }
        selected_object = -1;
        selected_spline = -1;
        sticky_sco_path = entry.att.sco_path;
        panel.remember_catalog_path(entry.att.sco_path);
        ui::EditorObjectProps p{};
        p.valid = true;
        p.id = entry.att.instance_id;
        p.varparent_id = entry.att.varparent_id;
        p.sco_name = entry.att.sco_path.filename().string();
        p.attach_kind = ui::EditorObjectProps::AttachKind::SplineAttach;
        p.spline_repeat_step = entry.att.repeat_step;
        p.spline_repeat_range = entry.att.repeat_range;
        p.spline_index = entry.att.spline_index;
        p.parent_spline_id = entry.parent_spline_id;
        p.text_slot_count = entry.att.object_text_line_count;
        if (p.text_slot_count <= 0)
            p.text_slot_count = static_cast<int>(entry.att.object_text_lines.size());
        if (const auto sco = sco::load_sco(entry.att.sco_path)) {
            if (p.text_slot_count < sco->extra_text_slots()) p.text_slot_count = sco->extra_text_slots();
            p.string_labels = sco->string_var_names;
        }
        p.strings = entry.att.object_text_lines;
        while (static_cast<int>(p.strings.size()) < p.text_slot_count) p.strings.emplace_back();
        if (const map::SplineInst* sp = resolve_edit_spline_for_attach(entry)) {
            const auto panel_coords = map::spline_attach_to_panel(*sp, entry.att);
            p.x = panel_coords.x;
            p.y = panel_coords.y;
            p.z = panel_coords.z;
            p.rot = panel_coords.rot;
            p.pitch = entry.att.rotation_x;
            p.bank = entry.att.rotation_z;
            p.spline_tangential = std::abs(entry.att.rotation_y) < 0.01f &&
                                  std::abs(entry.att.rotation_x) < 0.01f &&
                                  std::abs(entry.att.rotation_z) < 0.01f;
            const auto along = map::enumerate_spline_attachment_along_chain(entry.att, *sp, edit_splines);
            p.spline_attach_count = static_cast<int>(along.size());
            p.parent_spline_id = sp->id;
        }
        if (const auto sco = sco::load_sco(entry.att.sco_path)) p.string_labels = sco->string_var_names;
        panel.show_object(p);
        panel.set_selection_text("splineAttach id=" + std::to_string(entry.att.instance_id) + " " +
                                 entry.att.sco_path.filename().string());
    };
    auto select_spline_attachment_entry = show_spline_attachment_entry;
    auto show_spline_at = [&](std::size_t idx) {
        if (idx >= edit_splines.size()) return;
        const auto& s = edit_splines[idx];
        sticky_sli_path = s.sli_path;
        panel.remember_catalog_path(s.sli_path);
        ui::EditorSplineProps p{};
        p.valid = true;
        p.id = s.id;
        p.sli_name = s.sli_path.filename().string();
        p.is_spline_h = s.is_spline_h;
        p.x = s.position.x;
        p.y = s.position.y;
        p.z = s.position.z;
        p.rot = s.heading_deg;
        p.skew_start = s.skew_start;
        p.skew_end = s.skew_end;
        p.length = s.length;
        p.radius = s.radius;
        p.start_grad = s.start_grad;
        p.end_grad = s.end_grad;
        p.use_delta_h = s.use_delta_height || s.is_spline_h;
        p.delta_h = s.delta_height;
        p.cant_start = s.cant_start;
        p.cant_end = s.cant_end;
        p.mirror = s.mirror;
        p.terrain_align = static_cast<int>(s.terrain_align);
        const auto end_s = spline::evaluate_spline(s, s.length);
        panel.show_spline(p);
        panel.set_selection_text("spl id=" + std::to_string(s.id) + " z=" +
                                 std::to_string(s.position.z) + "→" + std::to_string(end_s.z) + " grad=" +
                                 std::to_string(s.start_grad) + "/" + std::to_string(s.end_grad));
        panel.set_spline_track_atten_y(s.position.y);
    };
    auto update_spline_panel_live = [&](std::size_t idx) {
        if (idx >= edit_splines.size()) return;
        const auto& s = edit_splines[idx];
        ui::EditorSplineProps p{};
        p.valid = true;
        p.id = s.id;
        p.sli_name = s.sli_path.filename().string();
        p.is_spline_h = s.is_spline_h;
        p.x = s.position.x;
        p.y = s.position.y;
        p.z = s.position.z;
        p.rot = s.heading_deg;
        p.skew_start = s.skew_start;
        p.skew_end = s.skew_end;
        p.length = s.length;
        p.radius = s.radius;
        p.start_grad = s.start_grad;
        p.end_grad = s.end_grad;
        p.use_delta_h = s.use_delta_height || s.is_spline_h;
        p.delta_h = s.delta_height;
        p.cant_start = s.cant_start;
        p.cant_end = s.cant_end;
        p.mirror = s.mirror;
        p.terrain_align = static_cast<int>(s.terrain_align);
        panel.update_spline_live_values(p);
    };
    // Frame camera on selection — only Numpad `.` (KP_DECIMAL), not on pick.
    auto focus_selection = [&]() {
        if (selected_object >= 0 &&
            static_cast<std::size_t>(selected_object) < edit_objects.size()) {
            const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
            camera.target_x = o.position.x;
            camera.target_y = o.position.y;
            bool abs_z = false;
            if (const auto sco = sco::load_sco(o.sco_path)) abs_z = sco::skip_terrain_add(*sco);
            for (const auto& a : edit_attachments)
                if (a.child_id == o.id) {
                    abs_z = true;
                    break;
                }
            camera.target_z = o.position.z + terrain_add(abs_z, o.position.x, o.position.y);
            camera.radius = (std::min)(camera.radius, 40.f);
            return;
        }
        if (selected_spline >= 0 &&
            static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
            const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
            const auto mid = spline::evaluate_spline(s, s.length * 0.5f);
            camera.target_x = mid.x;
            camera.target_y = mid.y;
            camera.target_z = mid.z;
            camera.radius = (std::min)(camera.radius, 50.f);
        }
    };
    auto find_attach_for_child = [&](std::int32_t id) -> const map::MapAttachObjectInst* {
        for (const auto& a : edit_attachments)
            if (a.child_id == id) return &a;
        return nullptr;
    };
    auto find_edit_obj_mut = [&](std::int32_t id) -> map::MapObjectInst* {
        for (auto& o : edit_objects)
            if (o.id == id) return &o;
        return nullptr;
    };
    auto find_edit_spl_mut = [&](std::int32_t id) -> map::SplineInst* {
        for (auto& s : edit_splines)
            if (s.id == id) return &s;
        return nullptr;
    };
    // cached_sco → sco::load_sco (process-wide cache, no per-call copy).
    // Rebuild attach child GPU pose from parent (world Z already includes terrain).
    auto sync_attach_child_gpu = [&](const map::MapAttachObjectInst& a, map::MapObjectInst& child) {
        map::MapObjectInst* parent = find_edit_obj_mut(a.parent_id);
        if (!parent) return;
        const auto child_sco = sco::load_sco(a.sco_path);
        const float pth = attach_parent_terrain_h(*parent);
        const auto child_world_opt = attach_child_world_matrix(a, parent, nullptr, pth, std::nullopt);
        if (!child_world_opt) return;
        const XMMATRIX child_world = *child_world_opt;
        XMFLOAT4X4 f;
        DirectX::XMStoreFloat4x4(&f, child_world);
        child.position.x = f._41;
        child.position.y = f._42;
        child.position.z = f._43;
        omsi_euler_from_matrix(child_world, child.rotation_deg.x, child.rotation_deg.y, child.rotation_deg.z);
        for (auto& sp : scene_places) {
            if (sp.object_id != child.id) continue;
            sp.alive = true;
            sp.abs_z = true;
            sp.parent_id = a.parent_id;
            sp.tx = child.position.x;
            sp.ty = child.position.y;
            sp.tz = child.position.z;
            XMMATRIX mesh_world = child_world;
            if (child_sco && !child_sco->meshes.empty() && child_sco->meshes.front().is_o3d) {
                if (auto mapped = o3d::load_mapped_shared(child_sco->meshes.front().mesh_file)) {
                    const auto root = o3d::o3d_unity_root_transform(mapped->mesh().object_rows());
                    mesh_world = ma4_to_xm(root) * child_world;
                }
            }
            const auto wt = store_world_transposed(mesh_world);
            renderer.set_chunk_world_matrix(sp.chunk, wt.data());
            DirectX::XMStoreFloat4x4(&sp.base_world, mesh_world);
            sp.has_base_world = true;
            // [AMPEL] no set_chunk_visible(true) — script mesh_visible owns Red/Yellow/Green
        }
    };
    // [splineAttachement] spawns: root*R*T via append_sco_meshes_at — must not fall through to sco_world_matrix-only sync.
    // Match by sco_mesh_index: .x meshes create one ScenePlace per part (same mesh_index), so a
    // mesh-loop with ++sp_i left unmatched places and Grab looked dead (see unmatched scene_places warn).
    auto sync_spline_attach_gpu = [&](map::MapObjectInst& o) {
        const sco::SceneryObject* sco = cached_sco(o.sco_path);
        if (!sco) return;
        const XMMATRIX base = sco_world_matrix(o, nullptr, 0.f, 0.f, 0.f);
        auto mesh_world_for = [&](int mesh_idx) -> XMMATRIX {
            if (!sco || mesh_idx < 0 || static_cast<std::size_t>(mesh_idx) >= sco->meshes.size()) return base;
            const auto& mesh_ref = sco->meshes[static_cast<std::size_t>(mesh_idx)];
            if (!mesh_ref.is_o3d) return base;
            const auto stem = mesh_ref.mesh_file.stem().string();
            if (stem == "empty_lod" || stem == "empty") return base;
            if (auto mapped = o3d::load_mapped_shared(mesh_ref.mesh_file)) {
                const auto root = o3d::o3d_unity_root_transform(mapped->mesh().object_rows());
                return ma4_to_xm(root) * base;
            }
            return base;
        };
        int updated = 0;
        auto apply_place = [&](ScenePlace& sp) {
            const XMMATRIX mesh_world = mesh_world_for(sp.sco_mesh_index);
            renderer.set_chunk_world_matrix(sp.chunk, store_world_transposed(mesh_world).data());
            // [AMPEL] no set_chunk_visible(true) — script mesh_visible owns Red/Yellow/Green
            sp.alive = true;
            sp.abs_z = true;
            sp.tx = o.position.x;
            sp.ty = o.position.y;
            sp.tz = o.position.z;
            DirectX::XMStoreFloat4x4(&sp.base_world, mesh_world);
            sp.has_base_world = true;
            ++updated;
        };
        for (auto& sp : scene_places) {
            if (sp.object_id != o.id || !sp.alive) continue;
            apply_place(sp);
        }
        if (updated == 0) {
            for (std::size_t i = scene_places.size(); i-- > 0;) {
                if (scene_places[i].object_id != o.id) continue;
                apply_place(scene_places[i]);
            }
        }
        if (updated == 0) {
            log::warn("splineAttachGPU sync id=" + std::to_string(o.id) + " sco=" +
                      o.sco_path.filename().string() + " no scene_places");
        }
    };
    auto spline_chain_z_for_attach = [&]() {
        std::unordered_map<std::int32_t, f4f23ma::f32> z;
        for (const auto& s : edit_splines) z[s.id] = s.position.z;
        return z;
    };
    auto compute_spline_index_for_attach = [&](const std::size_t edit_idx, const std::int32_t attach_tile_gx,
                                               const std::int32_t attach_tile_gy) -> std::int32_t {
        if (edit_idx >= edit_splines.size()) return -1;
        const std::int32_t sid = edit_splines[edit_idx].id;
        auto unpack_tk = [](std::int64_t k) -> std::pair<std::int32_t, std::int32_t> {
            return {static_cast<std::int32_t>(k >> 32), static_cast<std::int32_t>(static_cast<std::uint32_t>(k))};
        };
        auto local_on_tile = [&](std::int32_t gx, std::int32_t gy) -> std::int32_t {
            const auto it = tile_local_spline_edit_idx.find(tile_key(gx, gy));
            if (it == tile_local_spline_edit_idx.end()) return -1;
            for (std::size_t li = 0; li < it->second.size(); ++li) {
                if (it->second[li] == edit_idx) return static_cast<std::int32_t>(li);
            }
            for (std::size_t li = 0; li < it->second.size(); ++li) {
                const std::size_t ei = it->second[li];
                if (ei < edit_splines.size() && edit_splines[ei].id == sid)
                    return static_cast<std::int32_t>(li);
            }
            return -1;
        };
        std::int32_t spl_gx = attach_tile_gx;
        std::int32_t spl_gy = attach_tile_gy;
        std::int32_t local_idx = local_on_tile(attach_tile_gx, attach_tile_gy);
        if (local_idx < 0) {
            for (const auto& g : spline_gpu) {
                if (!g.alive || g.id != sid) continue;
                local_idx = local_on_tile(g.tile_gx, g.tile_gy);
                if (local_idx >= 0) {
                    spl_gx = g.tile_gx;
                    spl_gy = g.tile_gy;
                    break;
                }
            }
        }
        if (local_idx < 0) {
            for (const auto& kv : tile_local_spline_edit_idx) {
                const auto [gx, gy] = unpack_tk(kv.first);
                local_idx = local_on_tile(gx, gy);
                if (local_idx >= 0) {
                    spl_gx = gx;
                    spl_gy = gy;
                    break;
                }
            }
        }
        if (local_idx < 0) {
            log::warn("attach spline: no local index sid=" + std::to_string(sid) + " edit_idx=" +
                      std::to_string(edit_idx) + " obj_tile=(" + std::to_string(attach_tile_gx) + "," +
                      std::to_string(attach_tile_gy) + ") idx_tiles=" +
                      std::to_string(tile_local_spline_edit_idx.size()));
            return -1;
        }
        if (spl_gx == attach_tile_gx && spl_gy == attach_tile_gy) return local_idx;
        for (std::size_t gi = 0; gi < global_spline_index.size(); ++gi) {
            const auto& e = global_spline_index[gi];
            if (e.tile_gx == spl_gx && e.tile_gy == spl_gy && e.local_index == local_idx)
                return static_cast<std::int32_t>(gi);
        }
        log::info("attach spline: using tile-local index sid=" + std::to_string(sid) + " local=" +
                  std::to_string(local_idx) + " spl_tile=(" + std::to_string(spl_gx) + "," +
                  std::to_string(spl_gy) + ") obj_tile=(" + std::to_string(attach_tile_gx) + "," +
                  std::to_string(attach_tile_gy) + ")");
        return local_idx;
    };
    auto update_spline_attachment_entry_poses = [&](const std::size_t entry_i,
                                                    std::optional<bool> tangential_follow = std::nullopt) {
        if (entry_i >= edit_spline_attachments.size()) return;
        auto& entry = edit_spline_attachments[entry_i];
        const map::SplineInst* parent = resolve_edit_spline_for_attach(entry);
        if (!parent) return;
        const auto along_list = map::enumerate_spline_attachment_along_chain(entry.att, *parent, edit_splines);
        if (along_list.empty()) return;
        std::vector<std::pair<std::int32_t, std::size_t>> objs;
        for (std::size_t oi = 0; oi < edit_objects.size(); ++oi) {
            const auto& o = edit_objects[oi];
            const auto it = spline_attach_obj_to_entry.find(o.id);
            if (it == spline_attach_obj_to_entry.end() || it->second != entry_i) continue;
            objs.push_back({o.id, oi});
        }
        std::sort(objs.begin(), objs.end(), [&](const auto& a, const auto& b) {
            if (a.first == entry.att.instance_id) return true;
            if (b.first == entry.att.instance_id) return false;
            return a.first < b.first;
        });
        const std::size_t n = (std::min)(objs.size(), along_list.size());
        // OMSI default: yaw = heading + rotation_y. Tangential only when panel explicitly requests it.
        const bool tangential = tangential_follow.value_or(entry.att.tangential);
        bool wrote_att_rot = false;
        for (std::size_t i = 0; i < n; ++i) {
            auto& o = edit_objects[objs[i].second];
            const auto& hit = along_list[i];
            const map::SplineInst* pose_sp = parent;
            for (const auto& s : edit_splines) {
                if (s.id == hit.spline_id) {
                    pose_sp = &s;
                    break;
                }
            }
            const auto pose_chain_z = spline::resolve_spline_mesh_chain_end_z(*pose_sp, spline_chain_z_for_attach());
            const auto pose = map::compute_spline_attachment_pose(*pose_sp, entry.att, hit.along, pose_chain_z,
                                                                tangential);
            o.position.x = pose.x;
            o.position.y = pose.y;
            // AlignTerrain ON: Z from heightfield (OMSI plants the SCO on terrain).
            // AlignTerrain OFF: pure spline surface Z (offset_y included in evaluate).
            if (entry.att.align_terrain)
                o.position.z = sample_terrain_editor_xy(pose.x, pose.y) + entry.att.offset_y;
            else
                o.position.z = pose.z;
            o.rotation_deg.x = pose.yaw_deg;
            o.rotation_deg.y = pose.pitch_deg;
            o.rotation_deg.z = pose.bank_deg;
            if (spawn_align_pitch_bank && !tangential && o.id == entry.att.instance_id) {
                entry.att.rotation_x = o.rotation_deg.y;
                entry.att.rotation_z = o.rotation_deg.z;
                wrote_att_rot = true;
            }
            sync_spline_attach_gpu(o);
        }
        if (wrote_att_rot && spawn_align_pitch_bank && !tangential) {
            for (std::size_t i = 0; i < n; ++i) {
                auto& o = edit_objects[objs[i].second];
                if (o.id == entry.att.instance_id) continue;
                o.rotation_deg.y = entry.att.rotation_x;
                o.rotation_deg.z = entry.att.rotation_z;
                sync_spline_attach_gpu(o);
            }
        }
    };
    auto refresh_spline_attachments_for_spline = [&](const std::int32_t spline_id) {
        for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
            const map::SplineInst* sp = resolve_edit_spline_for_attach(edit_spline_attachments[ei]);
            if (sp && sp->id == spline_id) update_spline_attachment_entry_poses(ei);
        }
    };
    auto apply_spline_attach_from_world_xy = [&](const std::size_t entry_i, const float wx, const float wy,
                                                 const bool update_panel) {
        if (entry_i >= edit_spline_attachments.size()) return;
        auto& entry = edit_spline_attachments[entry_i];
        const map::SplineInst* parent = resolve_edit_spline_for_attach(entry);
        if (!parent) return;
        const auto chain_z = spline::resolve_spline_mesh_chain_end_z(*parent, spline_chain_z_for_attach());
        const auto proj = map::project_point_onto_spline(*parent, wx, wy, chain_z);
        const float len = (std::max)(parent->length, 0.f);
        entry.att.offset_x = proj.lateral_m;
        entry.att.offset_along = map::spline_local_length(*parent) + std::clamp(proj.along_m, 0.f, len);
        update_spline_attachment_entry_poses(entry_i);
        if (!update_panel) return;
        const auto panel_coords = map::spline_attach_to_panel(*parent, entry.att);
        ui::EditorObjectProps p{};
        p.valid = true;
        p.attach_kind = ui::EditorObjectProps::AttachKind::SplineAttach;
        p.id = entry.att.instance_id;
        p.x = panel_coords.x;
        p.y = panel_coords.y;
        p.z = panel_coords.z;
        p.rot = panel_coords.rot;
        p.spline_tangential = entry.att.tangential;
        p.spline_align_terrain = entry.att.align_terrain;
        panel.update_object_live_values(p);
    };
    auto apply_spline_attach_follow_xy = [&](map::MapObjectInst& o, const float wx, const float wy) {
        const auto it = spline_attach_obj_to_entry.find(o.id);
        if (it == spline_attach_obj_to_entry.end()) return;
        apply_spline_attach_from_world_xy(it->second, wx, wy, true);
    };
    auto bind_selected_object_to_spline = [&](const int spline_edit_idx) -> bool {
        if (spline_edit_idx < 0 || static_cast<std::size_t>(spline_edit_idx) >= edit_splines.size()) return false;
        if (selected_object < 0 || static_cast<std::size_t>(selected_object) >= edit_objects.size()) return false;
        auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
        const std::int32_t oid = o.id;
        if (oid <= 0) {
            log::warn("attach spline: object has no instance id");
            return false;
        }
        const auto [wx, wy] = editor_to_world_xy(o.position.x, o.position.y);
        const auto [home_gx, home_gy] = object_home_tile(oid, wx, wy);
        const auto tk = tile_key(home_gx, home_gy);
        std::int32_t tile_spline_count = 0;
        if (const auto it = tile_local_spline_edit_idx.find(tk); it != tile_local_spline_edit_idx.end())
            tile_spline_count = static_cast<std::int32_t>(it->second.size());
        const std::int32_t spline_index =
            compute_spline_index_for_attach(static_cast<std::size_t>(spline_edit_idx), home_gx, home_gy);
        if (spline_index < 0) {
            log::warn("attach spline: spline_index unresolved edit_idx=" + std::to_string(spline_edit_idx) +
                      " tile=(" + std::to_string(home_gx) + "," + std::to_string(home_gy) + ")");
            return false;
        }
        const std::int32_t parent_sid = edit_splines[static_cast<std::size_t>(spline_edit_idx)].id;
        const float ex = o.position.x;
        const float ey = o.position.y;

        edit_attachments.erase(std::remove_if(edit_attachments.begin(), edit_attachments.end(),
                                              [&](const map::MapAttachObjectInst& a) { return a.child_id == oid; }),
                               edit_attachments.end());

        std::size_t entry_i = static_cast<std::size_t>(-1);
        if (const auto sa_it = spline_attach_obj_to_entry.find(oid); sa_it != spline_attach_obj_to_entry.end()) {
            entry_i = sa_it->second;
        } else {
            for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
                if (edit_spline_attachments[ei].att.instance_id != oid) continue;
                entry_i = ei;
                break;
            }
        }
        if (entry_i == static_cast<std::size_t>(-1)) {
            edit_spline_attachments.push_back({});
            entry_i = edit_spline_attachments.size() - 1;
        }

        auto& entry = edit_spline_attachments[entry_i];
        entry.att.sco_path = o.sco_path;
        entry.att.instance_id = oid;
        entry.att.spline_index = spline_index;
        entry.att.object_text_lines = o.strings;
        entry.att.object_text_line_count = o.string_line_count;
        entry.att.varparent_id = o.varparent_id;
        // Keep existing repeater fields if reusing a map entry; clear for fresh free→SA.
        if (!spline_attach_spawn_ids.count(oid)) {
            entry.att.repeat_step = 0.f;
            entry.att.repeat_range = 0.f;
            entry.att.rotation_y = 0.f;
            entry.att.rotation_x = 0.f;
            entry.att.rotation_z = 0.f;
        }
        entry.tile_gx = home_gx;
        entry.tile_gy = home_gy;
        entry.tile_spline_count = tile_spline_count;
        entry.parent_spline_id = parent_sid;

        // Convert / rebind in place — keep existing GPU places (no kill/respawn).
        spline_attach_obj_to_entry[oid] = entry_i;
        spline_attach_spawn_ids.insert(oid);
        sa_spawned_entries.insert(entry_i);
        sa_spawn_queued.erase(entry_i);
        sa_spawn_queue.erase(std::remove(sa_spawn_queue.begin(), sa_spawn_queue.end(), entry_i),
                             sa_spawn_queue.end());
        sa_cpu_by_entry.erase(entry_i);
        sa_spawn_retry_count.erase(entry_i);

        apply_spline_attach_from_world_xy(entry_i, ex, ey, false);
        for (auto& sp : scene_places) {
            if (sp.object_id != oid) continue;
            sp.alive = true;
            sp.abs_z = true;
            sp.parent_id = parent_sid;
            // [AMPEL] no set_chunk_visible(true) — script mesh_visible owns Red/Yellow/Green
        }
        sync_spline_attach_gpu(o);
        show_object_at(static_cast<std::size_t>(selected_object));
        log::info("attach spline in-place id=" + std::to_string(oid) + " splineId=" +
                  std::to_string(parent_sid) + " ei=" + std::to_string(entry_i) + " along=" +
                  std::to_string(entry.att.offset_along));
        return true;
    };
    auto purge_spline_attachment_entry = [&](const std::size_t entry_i) {
        if (entry_i >= edit_spline_attachments.size()) return;
        const std::int32_t inst = edit_spline_attachments[entry_i].att.instance_id;
        if (inst > 0) editor_deleted_object_ids.insert(inst);
        std::vector<std::int32_t> oids;
        for (const auto& kv : spline_attach_obj_to_entry) {
            if (kv.second != entry_i) continue;
            oids.push_back(kv.first);
        }
        for (const std::int32_t id : oids) {
            editor_deleted_object_ids.insert(id);
            spline_attach_spawn_ids.erase(id);
            spline_attach_obj_to_entry.erase(id);
            for (auto& sp : scene_places) {
                if (sp.object_id != id) continue;
                sp.alive = false;
                renderer.set_chunk_visible(sp.chunk, false);
            }
            edit_objects.erase(std::remove_if(edit_objects.begin(), edit_objects.end(),
                                              [&](const map::MapObjectInst& o) { return o.id == id; }),
                               edit_objects.end());
        }
        edit_spline_attachments.erase(edit_spline_attachments.begin() +
                                      static_cast<std::ptrdiff_t>(entry_i));
        reindex_spline_attach_obj_to_entry_after_erase(entry_i);
    };
    auto mark_editor_object_deleted = [&](const std::int32_t oid) {
        editor_deleted_object_ids.insert(oid);
        if (const auto sa_it = spline_attach_obj_to_entry.find(oid); sa_it != spline_attach_obj_to_entry.end())
            purge_spline_attachment_entry(sa_it->second);
    };
    auto mark_editor_spline_deleted = [&](const std::int32_t sid) {
        editor_deleted_spline_ids.insert(sid);
        for (auto& s : edit_splines) {
            if (s.next_id == sid) s.next_id = 0;
            if (s.prev_id == sid) s.prev_id = 0;
        }
        for (std::size_t ei = edit_spline_attachments.size(); ei-- > 0;) {
            auto& e = edit_spline_attachments[ei];
            if (e.parent_spline_id == sid) {
                editor_deleted_object_ids.insert(e.att.instance_id);
                purge_spline_attachment_entry(ei);
            }
        }
    };
    auto sync_object_gpu = [&](map::MapObjectInst& o) {
        if (bng_sync_hook && bng_sync_hook(o)) return;
        // attachObj children store world Z — never add terrain (that was doubling height).
        if (const auto* att = find_attach_for_child(o.id)) {
            sync_attach_child_gpu(*att, o);
            return;
        }
        if (spline_attach_spawn_ids.count(o.id)) {
            sync_spline_attach_gpu(o);
            return;
        }
        bool abs_z = false;
        bool ground_layer = false;
        bool help_arrow = false;
        float tree_w = 0.f, tree_h = 0.f;
        bool is_tree = false;
        const sco::SceneryObject* sco = cached_sco(o.sco_path);
        if (sco) {
            abs_z = sco::skip_terrain_add(*sco);
            ground_layer = sco_ground_layer(o, *sco);
            help_arrow = sco->special == sco::ScoSpecial::HelpArrow;
            if (sco->tree.valid) {
                is_tree = true;
                tree_h = sco->tree.height;
                float aspect = sco->tree.ratio;
                if (o.obj_type == 4) {
                    if (o.tree_height > 0.01f) tree_h = o.tree_height;
                    if (o.tree_aspect > 0.01f) aspect = o.tree_aspect;
                }
                tree_w = editor::tree_billboard_width(tree_h, aspect, sco->tree.width, sco->tree.ratio);
            }
        }
        // ScenePlace from attach spawn also marks abs_z (safety if edit_attachments stale).
        for (const auto& sp : scene_places) {
            if (sp.alive && sp.object_id == o.id && (sp.parent_id != 0 || sp.abs_z)) {
                abs_z = true;
                break;
            }
        }
        bool z_applied = true;
        float th = terrain_add(abs_z, o.position.x, o.position.y, o.position.z, ground_layer, &z_applied);
        if (!z_applied) {
            // No containing heightfield — freeze prior GPU Z (avoid sink to 0/nearest).
            for (const auto& sp : scene_places) {
                if (!sp.alive || sp.object_id != o.id) continue;
                th = sp.tz - o.position.z;
                break;
            }
        }
        const XMMATRIX base = sco_object_base_matrix(o, th, 0.f, 0.f);
        if (!is_tree && sco && !sco->meshes.empty()) {
            std::vector<std::size_t> sp_indices;
            sp_indices.reserve(sco->meshes.size());
            for (std::size_t i = 0; i < scene_places.size(); ++i) {
                if (scene_places[i].object_id == o.id && scene_places[i].alive) sp_indices.push_back(i);
            }
            if (sp_indices.empty()) {
                // Undo restore / killed GPU: take newest dead places only (not all ghosts).
                for (std::size_t i = scene_places.size(); i-- > 0;) {
                    if (scene_places[i].object_id != o.id) continue;
                    sp_indices.push_back(i);
                }
                std::reverse(sp_indices.begin(), sp_indices.end());
            }
            std::size_t sp_i = 0;
            for (const auto& mesh_ref : sco->meshes) {
                if (sp_i >= sp_indices.size()) break;
                auto& sp = scene_places[sp_indices[sp_i]];
                sp.alive = true;
                sp.abs_z = abs_z;
                sp.help_arrow = help_arrow;
                sp.tx = o.position.x;
                sp.ty = o.position.y;
                sp.tz = o.position.z + th;
                if (mesh_ref.is_o3d) {
                    const auto stem = mesh_ref.mesh_file.stem().string();
                    if (stem == "empty_lod" || stem == "empty") continue;
                    XMMATRIX mesh_world = base;
                    if (auto mapped = o3d::load_mapped_shared(mesh_ref.mesh_file)) {
                        const auto root = o3d::o3d_unity_root_transform(mapped->mesh().object_rows());
                        mesh_world = ma4_to_xm(root) * base;
                    }
                    renderer.set_chunk_world_matrix(sp.chunk, store_world_transposed(mesh_world).data());
                    DirectX::XMStoreFloat4x4(&sp.base_world, mesh_world);
                    sp.has_base_world = true;
                    // [AMPEL] no set_chunk_visible(true) — script mesh_visible owns Red/Yellow/Green
                    ++sp_i;
                } else if (!sco->tree.valid) {
                    renderer.set_chunk_world_matrix(sp.chunk, store_world_transposed(base).data());
                    // [AMPEL] no set_chunk_visible(true) — script mesh_visible owns Red/Yellow/Green
                    ++sp_i;
                }
            }
            for (; sp_i < sp_indices.size(); ++sp_i) {
                auto& sp = scene_places[sp_indices[sp_i]];
                sp.alive = true;
                sp.abs_z = abs_z;
                sp.help_arrow = help_arrow;
                sp.tx = o.position.x;
                sp.ty = o.position.y;
                sp.tz = o.position.z + th;
                renderer.set_chunk_world_matrix(sp.chunk, store_world_transposed(base).data());
                // [AMPEL] no set_chunk_visible(true) — script mesh_visible owns Red/Yellow/Green
            }
        } else {
            bool any_alive = false;
            std::size_t newest_dead = static_cast<std::size_t>(-1);
            for (std::size_t i = 0; i < scene_places.size(); ++i) {
                if (scene_places[i].object_id != o.id) continue;
                if (scene_places[i].alive) any_alive = true;
                else newest_dead = i;
            }
            for (std::size_t i = 0; i < scene_places.size(); ++i) {
                auto& sp = scene_places[i];
                if (sp.object_id != o.id) continue;
                // Prefer live GPU; if none, revive only the newest dead place (avoid 0,0,0 ghosts).
                if (any_alive) {
                    if (!sp.alive) continue;
                } else if (i != newest_dead) {
                    continue;
                }
                sp.alive = true;
                sp.abs_z = abs_z;
                sp.help_arrow = help_arrow;
                sp.tx = o.position.x;
                sp.ty = o.position.y;
                sp.tz = o.position.z + th;
                if (is_tree || sp.is_tree) {
                    sp.is_tree = true;
                    if (tree_w > 0.1f) {
                        sp.half_xy = tree_w * 0.5f;
                        sp.height = tree_h;
                    }
                    const float w = (std::max)(0.1f, sp.half_xy * 2.f);
                    const float h = (std::max)(0.1f, sp.height);
                    const XMMATRIX world_m =
                        DirectX::XMMatrixScaling(w, w, h) * sco_world_matrix(o, nullptr, th, 0.f, 0.f);
                    renderer.set_chunk_world_matrix(sp.chunk, store_world_transposed(world_m).data());
                } else {
                    renderer.set_chunk_world_matrix(sp.chunk, store_world_transposed(base).data());
                }
                // [AMPEL] no set_chunk_visible(true) — script mesh_visible owns Red/Yellow/Green
            }
        }
        // Keep attached children glued to parent after parent moves.
        for (const auto& a : edit_attachments) {
            if (a.parent_id != o.id) continue;
            if (auto* ch = find_edit_obj_mut(a.child_id)) sync_attach_child_gpu(a, *ch);
        }
    };

    auto highlight_chunks = [&](const std::vector<std::size_t>& chunks, float amount, float r, float g,
                                   float b) {
        for (const std::size_t c : chunks) renderer.set_chunk_highlight(c, amount, r, g, b);
    };
    // Hover = light yellow, selected = light red.
    constexpr float kHoverAmt = 0.65f, kSelAmt = 0.55f;
    constexpr float kHoverR = 1.f, kHoverG = 0.92f, kHoverB = 0.12f; // yellow hover
    constexpr float kSelR = 1.f, kSelG = 0.08f, kSelB = 0.08f;       // red select (live)
    // O(1) lookup. spline_gpu only ever grows (push_back), so an id->index map plus a "how much of the vector is
    // indexed" watermark stays valid; every hit is re-verified and a failed check falls back to a full rebuild.
    // The old linear scan ran per hover frame and per spline at upload time (O(N^2) with thousands of splines).
    std::unordered_map<std::int32_t, std::size_t> spline_gpu_index;
    std::size_t spline_gpu_indexed = 0;
    auto find_spline_gpu = [&](std::int32_t id) -> SplineGpu* {
        if (spline_gpu_indexed > spline_gpu.size()) {
            spline_gpu_index.clear();
            spline_gpu_indexed = 0;
        }
        auto index_tail = [&]() {
            for (; spline_gpu_indexed < spline_gpu.size(); ++spline_gpu_indexed)
                spline_gpu_index.try_emplace(spline_gpu[spline_gpu_indexed].id, spline_gpu_indexed);
        };
        if (const auto it = spline_gpu_index.find(id); it != spline_gpu_index.end()) {
            if (it->second < spline_gpu.size() && spline_gpu[it->second].id == id) return &spline_gpu[it->second];
            spline_gpu_index.clear(); // stale entry (an id changed): rebuild from scratch
            spline_gpu_indexed = 0;
        }
        index_tail();
        if (const auto it = spline_gpu_index.find(id); it != spline_gpu_index.end()) return &spline_gpu[it->second];
        return nullptr;
    };
    auto highlight_spline_next_chain = [&](std::int32_t sid, float amt, float r, float g, float b) {
        std::unordered_set<std::int32_t> seen;
        for (int hop = 0; sid != 0 && hop < 256; ++hop) {
            if (!seen.insert(sid).second) break;
            if (auto* gpu = find_spline_gpu(sid)) highlight_chunks(gpu->chunks, amt, r, g, b);
            const map::SplineInst* sp = nullptr;
            for (const auto& s : edit_splines) {
                if (s.id == sid) {
                    sp = &s;
                    break;
                }
            }
            if (!sp) break;
            sid = sp->next_id;
        }
    };
    auto set_spline_chunk_offset = [&](std::int32_t id, float dx, float dy, float dz = 0.f) {
        if (auto* g = find_spline_gpu(id)) {
            const auto wt = store_world_transposed(XMMatrixTranslation(dx, dy, dz));
            for (const std::size_t c : g->chunks) renderer.set_chunk_world_matrix(c, wt.data());
        }
    };
    std::unordered_map<std::string, bool> sli_only_editor_cache;
    auto sli_is_only_editor = [&](const std::filesystem::path& sli) -> bool {
        const std::string key = sli.string();
        if (const auto it = sli_only_editor_cache.find(key); it != sli_only_editor_cache.end()) return it->second;
        const auto def = spline::load_sli_shared(sli);
        const bool v = def && def->only_editor;
        sli_only_editor_cache.emplace(key, v);
        return v;
    };
    auto spline_mesh_hidden_onlyeditor = [&](const map::SplineInst& s) {
        return panel.imgui_state().hide_onlyeditor_splines && sli_is_only_editor(s.sli_path);
    };
    bool last_hide_onlyeditor_splines = false;
    auto apply_onlyeditor_spline_visibility = [&]() {
        const auto& st = panel.imgui_state();
        const bool hide = st.hide_onlyeditor_splines;
        const std::size_t max_c = renderer.chunk_count();
        int n = 0;
        for (const auto& s : edit_splines) {
            if (!sli_is_only_editor(s.sli_path)) continue;
            SplineGpu* g = find_spline_gpu(s.id);
            if (!g || !g->alive) continue;
            const bool vis = !hide && !st.hidden_spline_ids.count(s.id);
            for (const auto c : g->chunks)
                if (c < max_c) renderer.set_chunk_visible(c, vis);
            ++n;
        }
        log::info(std::string("onlyeditor splines mesh ") + (hide ? "hidden" : "shown") + " n=" +
                  std::to_string(n));
    };
    std::unordered_map<std::string, std::filesystem::path> spline_tex_resolve_cache;
    auto resolve_spline_tex = [&](const std::filesystem::path& sli, const std::filesystem::path& tex) {
        std::string key = sli.string();
        key += '|';
        key += tex.string();
        if (const auto it = spline_tex_resolve_cache.find(key); it != spline_tex_resolve_cache.end())
            return it->second;
        std::filesystem::path out{};
        if (const auto r = gfx::dx12::resolve_texture_path(sli, tex.string(), cfg.omsi_root)) out = *r;
        spline_tex_resolve_cache.emplace(std::move(key), out);
        return out;
    };
    // A strip is only safe to upload when its indices stay inside its vertex array. A stale / half-written
    // strip (worker result for a spline that was undone meanwhile) used to be memcpy'd straight into the GPU
    // heap and crashed inside VCRUNTIME140 (stages apply_spline_gpu_meshes / spline_merge_begin).
    auto spline_meshes_sane = [](const std::vector<spline::SplineMesh>& meshes) -> bool {
        if (meshes.empty() || meshes.size() > 4096) return false;
        for (const auto& m : meshes) {
            const std::size_t nv = m.vertices.size(), ni = m.indices.size();
            if (nv == 0 || ni == 0) continue;
            if (nv > (1u << 26) || ni > (1u << 27) || (ni % 3) != 0) return false;
            for (const auto idx : m.indices)
                if (static_cast<std::size_t>(idx) >= nv) return false;
        }
        return true;
    };
    // Per-spline lookups used at upload time. They were linear scans over edit_splines / every tile index entry
    // for EACH uploaded spline (O(N^2); several ms/spline with thousands of splines on the main thread).
    // Caches are keyed by a cheap fingerprint of the source containers; hits are re-verified and a stale
    // hit or a miss on a changed fingerprint rebuilds once.
    struct SplineLookupCache {
        const void* sp_data = nullptr;
        std::size_t sp_size = 0;
        std::size_t idx_buckets = 0;
        std::size_t idx_total = 0;
        bool valid = false;
        std::unordered_map<std::int32_t, std::size_t> edit_by_id;
        std::unordered_map<std::int32_t, std::pair<std::int32_t, std::int32_t>> home_by_id;
    } spline_lookup;
    auto spline_lookup_rebuild = [&]() {
        spline_lookup.edit_by_id.clear();
        spline_lookup.home_by_id.clear();
        spline_lookup.edit_by_id.reserve(edit_splines.size() * 2 + 1);
        for (std::size_t i = 0; i < edit_splines.size(); ++i)
            spline_lookup.edit_by_id.try_emplace(edit_splines[i].id, i);
        std::size_t total = 0;
        for (const auto& kv : tile_local_spline_edit_idx) {
            total += kv.second.size();
            for (const std::size_t ei : kv.second) {
                if (ei >= edit_splines.size()) continue;
                spline_lookup.home_by_id.try_emplace(
                    edit_splines[ei].id,
                    std::pair<std::int32_t, std::int32_t>{static_cast<std::int32_t>(kv.first >> 32),
                                                         static_cast<std::int32_t>(static_cast<std::uint32_t>(kv.first))});
            }
        }
        spline_lookup.sp_data = edit_splines.data();
        spline_lookup.sp_size = edit_splines.size();
        spline_lookup.idx_buckets = tile_local_spline_edit_idx.size();
        spline_lookup.idx_total = total;
        spline_lookup.valid = true;
    };
    auto spline_lookup_fresh = [&]() {
        if (!spline_lookup.valid || spline_lookup.sp_data != edit_splines.data() ||
            spline_lookup.sp_size != edit_splines.size() ||
            spline_lookup.idx_buckets != tile_local_spline_edit_idx.size())
            spline_lookup_rebuild();
    };
    auto find_edit_spline_by_id = [&](std::int32_t id) -> const map::SplineInst* {
        spline_lookup_fresh();
        if (const auto it = spline_lookup.edit_by_id.find(id); it != spline_lookup.edit_by_id.end()) {
            if (it->second < edit_splines.size() && edit_splines[it->second].id == id)
                return &edit_splines[it->second];
            spline_lookup_rebuild(); // stale hit
            if (const auto it2 = spline_lookup.edit_by_id.find(id); it2 != spline_lookup.edit_by_id.end())
                return &edit_splines[it2->second];
        }
        return nullptr;
    };
    auto apply_spline_gpu_meshes = [&](map::SplineInst& s, const std::vector<spline::SplineMesh>& meshes,
                                       bool preview_only, bool stream_upload) -> bool {
        if (meshes.empty()) return false;
        if (!spline_meshes_sane(meshes)) {
            log::warn("apply_spline_gpu_meshes: rejected inconsistent strips id=" + std::to_string(s.id));
            return false;
        }
        const std::string sli_path_copy = s.sli_path.string(); // keep a local copy; `s` may live in a vector
        (void)sli_path_copy;
        const auto remesh_t0 = std::chrono::steady_clock::now();
        std::optional<float> chain_z;
        if (s.next_id != 0) {
            if (const auto* n = find_edit_spline_by_id(s.next_id))
                chain_z = spline::spline_mesh_chain_end_z(s, n->position.z);
        }
        SplineGpu* gpu = find_spline_gpu(s.id);
        auto spline_home_from_index = [&]() -> std::pair<std::int32_t, std::int32_t> {
            spline_lookup_fresh();
            if (const auto it = spline_lookup.home_by_id.find(s.id); it != spline_lookup.home_by_id.end())
                return it->second;
            return {tile_gx, tile_gy};
        };
        const auto [home_gx, home_gy] = spline_home_from_index();
        const auto t_home = std::chrono::steady_clock::now();
        if (gpu && !gpu->alive) {
            // Streaming already released these chunks. Do not touch the old chunk ids:
            // they have no valid VB/IB anymore and can belong to a recycled spline id.
            gpu->chunks.clear();
            gpu->alive = true;
            gpu->tile_gx = home_gx;
            gpu->tile_gy = home_gy;
        }
        if (!gpu) {
            SplineGpu created{};
            created.id = s.id;
            created.tile_gx = home_gx;
            created.tile_gy = home_gy;
            created.alive = true;
            spline_gpu.push_back(std::move(created));
            gpu = &spline_gpu.back();
        }
        gpu->alive = true;
        gpu->tile_gx = home_gx;
        gpu->tile_gy = home_gy;
        {
            const std::wstring sli_fn = s.sli_path.filename().wstring();
            if (!gpu->sli_filename.empty() && gpu->sli_filename != sli_fn) {
                // Path changed (replace_sli / undo): hide every old strip then drop IDs so
                // replace_textured_mesh cannot leave a mismatched geometry mix visible.
                for (const auto c : gpu->chunks) renderer.set_chunk_visible(c, false);
                gpu->chunks.clear();
            }
            gpu->sli_filename = sli_fn;
        }
        // Stream and editor remesh: never block the UI on GPU fence — textures finish async.
        const bool wait_gpu = false;
        // FIX 2026-09-24 v4: NO purgar chunks a medias (causaba meshes/objetos faltantes).
        // Si algún índice está fuera de rango → ocultar los que se pueda, clear, rebuild full append.
        {
            const std::size_t max_c = renderer.chunk_count();
            bool any_bad = false;
            for (const auto c : gpu->chunks) {
                if (c >= max_c) { any_bad = true; break; }
            }
            if (any_bad) {
                log::warn("remesh full rebuild id=" + std::to_string(s.id) +
                          " (stale chunk ids, max=" + std::to_string(max_c) + ")");
                for (const auto c : gpu->chunks) {
                    if (c < max_c) {
                        renderer.set_chunk_visible(c, false);
                        renderer.set_chunk_wireframe(c, false);
                    }
                }
                gpu->chunks.clear();
            }
        }
        if (stream_upload) {
            for (const auto c : gpu->chunks) {
                if (c < renderer.chunk_count()) {
                    renderer.set_chunk_visible(c, false);
                    renderer.set_chunk_wireframe(c, false);
                }
            }
            gpu->chunks.clear();
            std::vector<omsi::o3d::Vertex> merged_v;
            std::vector<std::uint32_t> merged_i;
            std::vector<omsi::gfx::dx12::ProcMeshSubrange> merged_sub;
            std::size_t vert_reserve = 0;
            std::size_t idx_reserve = 0;
            for (const auto& mesh : meshes) {
                vert_reserve += mesh.vertices.size();
                idx_reserve += mesh.indices.size();
            }
            merged_v.reserve(vert_reserve);
            merged_i.reserve(idx_reserve);
            merged_sub.reserve(meshes.size());
            for (const auto& mesh : meshes) {
                if (mesh.vertices.empty() || mesh.indices.empty()) continue;
                const std::uint32_t v_base = static_cast<std::uint32_t>(merged_v.size());
                const std::uint32_t i_start = static_cast<std::uint32_t>(merged_i.size());
                merged_v.insert(merged_v.end(), mesh.vertices.begin(), mesh.vertices.end());
                for (const auto idx : mesh.indices) merged_i.push_back(idx + v_base);
                omsi::gfx::dx12::ProcMeshSubrange sub{};
                sub.index_start = i_start;
                sub.index_count = static_cast<std::uint32_t>(mesh.indices.size());
                sub.texture_path = resolve_spline_tex(s.sli_path, mesh.texture_path);
                sub.alpha_mode = mesh.alpha_mode;
                merged_sub.push_back(std::move(sub));
            }
            const auto t_merge = std::chrono::steady_clock::now();
            bool uploaded = false;
            if (!merged_v.empty() && !merged_i.empty() && !merged_sub.empty()) {
                uploaded = renderer.append_textured_mesh_merged(
                    merged_v.data(), static_cast<std::uint32_t>(merged_v.size()), merged_i.data(),
                    static_cast<std::uint32_t>(merged_i.size()), merged_sub);
                if (uploaded) {
                    const std::size_t chunk = renderer.chunk_count() - 1;
                    renderer.set_chunk_wireframe(chunk, false);
                    renderer.set_chunk_highlight(chunk, 0.f);
                    const auto wt = store_world_transposed(XMMatrixIdentity());
                    renderer.set_chunk_world_matrix(chunk, wt.data());
                    // Merged strips are tagged "__spline_merged__" and routed to the spline PSO.
                    gpu->chunks.push_back(chunk);
                }
            }
            if (!uploaded) {
                for (std::size_t i = 0; i < meshes.size(); ++i) {
                    const auto& mesh = meshes[i];
                    if (mesh.vertices.empty() || mesh.indices.empty()) continue;
                    const std::filesystem::path tex_path = resolve_spline_tex(s.sli_path, mesh.texture_path);
                    if (!renderer.append_textured_mesh(mesh.vertices.data(),
                                                       static_cast<std::uint32_t>(mesh.vertices.size()),
                                                       mesh.indices.data(),
                                                       static_cast<std::uint32_t>(mesh.indices.size()), tex_path,
                                                       mesh.alpha_mode))
                        continue;
                    const std::size_t chunk = renderer.chunk_count() - 1;
                    renderer.set_chunk_wireframe(chunk, false);
                    renderer.set_chunk_highlight(chunk, 0.f);
                    const auto wt = store_world_transposed(XMMatrixIdentity());
                    renderer.set_chunk_world_matrix(chunk, wt.data());
                    if (mesh.alpha_mode == 0)
                        renderer.set_chunk_render_phase(chunk, omsi::gfx::dx12::WorldRenderPhase::Spline, false, false);
                    gpu->chunks.push_back(chunk);
                }
            }
            const auto t_append = std::chrono::steady_clock::now();
            if (!preview_only && s.terrain_align != 0) terrain_align_gpu_dirty = true;
            if (gpu) {
                const bool hide_mesh = spline_mesh_hidden_onlyeditor(s);
                for (const auto c : gpu->chunks) {
                    renderer.set_chunk_highlight(c, 0.f);
                    if (hide_mesh) renderer.set_chunk_visible(c, false);
                }
            }
            const double remesh_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - remesh_t0).count();
            if (remesh_ms > 5.0) {
                log::info("SPLINE_SLOW id=" + std::to_string(s.id) + " ms=" + std::to_string(remesh_ms) +
                          " strips=" + std::to_string(meshes.size()) + " stream=1 merged=" +
                          (uploaded ? "1" : "0") + " chunks=" + std::to_string(gpu->chunks.size()) +
                          " home=" + std::to_string(std::chrono::duration<double, std::milli>(t_home - remesh_t0).count()) +
                          " merge=" + std::to_string(std::chrono::duration<double, std::milli>(t_merge - t_home).count()) +
                          " append=" + std::to_string(std::chrono::duration<double, std::milli>(t_append - t_merge).count()));
            }
            return true;
        }
        const std::size_t n = (std::min)(gpu->chunks.size(), meshes.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& mesh = meshes[i];
            if (mesh.vertices.empty() || mesh.indices.empty()) {
                log::warn("remesh empty mesh strip id=" + std::to_string(s.id) + " strip=" + std::to_string(i));
                continue;
            }
            // The preview keeps the real texture (the wireframe pass ignores it): dropping it lost the
            // chunk's SRV, and the solid remesh on "place" then stayed white whenever the texture upload
            // queue was busy, until the next remesh (a deselect click). Same texture = SRV reused, free.
            const std::filesystem::path tex_path = resolve_spline_tex(s.sli_path, mesh.texture_path);
            const std::uint8_t alpha = preview_only ? 0 : mesh.alpha_mode;
            const std::size_t ci = gpu->chunks[i];
            if (!renderer.replace_textured_mesh(ci, mesh.vertices.data(),
                                               static_cast<std::uint32_t>(mesh.vertices.size()),
                                               mesh.indices.data(),
                                               static_cast<std::uint32_t>(mesh.indices.size()), tex_path, alpha,
                                               wait_gpu, preview_only)) {
                log::warn("remesh replace failed id=" + std::to_string(s.id) + " strip=" + std::to_string(i));
                // Never leave a failed strip as sticky preview wireframe.
                renderer.set_chunk_visible(ci, false);
                renderer.set_chunk_wireframe(ci, false);
            } else {
                renderer.set_chunk_wireframe(ci, preview_only);
                renderer.set_chunk_highlight(ci, 0.f);
                renderer.set_chunk_visible(ci, true);
                const auto wt = store_world_transposed(XMMatrixIdentity());
                renderer.set_chunk_world_matrix(ci, wt.data());
                if (!preview_only && alpha == 0)
                    renderer.set_chunk_render_phase(ci, omsi::gfx::dx12::WorldRenderPhase::Spline, false, false);
            }
        }
        // Residuos de preview: strips de más → ocultar Y quitar wireframe
        for (std::size_t i = n; i < gpu->chunks.size(); ++i) {
            const std::size_t ci = gpu->chunks[i];
            if (ci >= renderer.chunk_count()) continue;
            renderer.set_chunk_visible(ci, false);
            renderer.set_chunk_wireframe(ci, false);
        }
        // Tras PLACE (no preview): forzar wireframe off en todos los strips activos
        if (!preview_only && gpu) {
            for (std::size_t i = 0; i < n && i < gpu->chunks.size(); ++i) {
                const std::size_t ci = gpu->chunks[i];
                if (ci < renderer.chunk_count())
                    renderer.set_chunk_wireframe(ci, false);
            }
        }
        for (std::size_t i = n; i < meshes.size(); ++i) {
            const auto& mesh = meshes[i];
            const std::filesystem::path tex_path = resolve_spline_tex(s.sli_path, mesh.texture_path);
            const std::uint8_t alpha = preview_only ? 0 : mesh.alpha_mode;
            if (!renderer.append_textured_mesh(mesh.vertices.data(),
                                               static_cast<std::uint32_t>(mesh.vertices.size()),
                                               mesh.indices.data(),
                                               static_cast<std::uint32_t>(mesh.indices.size()), tex_path, alpha)) {
                continue;
            }
            const std::size_t chunk = renderer.chunk_count() - 1;
            renderer.set_chunk_wireframe(chunk, preview_only);
            renderer.set_chunk_highlight(chunk, 0.f);
            const auto wt = store_world_transposed(XMMatrixIdentity());
            renderer.set_chunk_world_matrix(chunk, wt.data());
            if (mesh.alpha_mode == 0)
                renderer.set_chunk_render_phase(chunk, omsi::gfx::dx12::WorldRenderPhase::Spline, false, false);
            gpu->chunks.push_back(chunk);
        }
        if (preview_only) return true;
        if (!stream_upload) {
            const auto remesh_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - remesh_t0)
                    .count();
            log::info("editor remesh spline id=" + std::to_string(s.id) + " strips=" +
                      std::to_string(meshes.size()) + " len=" + std::to_string(s.length) + " r=" +
                      std::to_string(s.radius) + " ms=" + std::to_string(remesh_ms));
        }
        if (!preview_only && s.terrain_align != 0) terrain_align_gpu_dirty = true;
        if (gpu) {
            const bool hide_mesh = spline_mesh_hidden_onlyeditor(s);
            for (const auto c : gpu->chunks) {
                renderer.set_chunk_highlight(c, 0.f);
                if (hide_mesh) renderer.set_chunk_visible(c, false);
            }
        }
        if (stream_upload) return true;
        // Refresh white/blue end markers for this spline id.
        // FIX 2026-09-23b: no tocar markers con chunk inválido (evita AV READ).
        for (auto& mk : end_markers) {
            if (mk.from_object || mk.owner_id != s.id) continue;
            if (mk.chunk >= renderer.chunk_count()) {
                mk.alive = false;
                continue;
            }
            mk.alive = true;
            float x = 0.f, y = 0.f, z = 0.f;
            float scl = mk.is_path ? 0.16f : kArrowScale;
            if (!mk.is_path) {
                const auto samp = spline::evaluate_spline(s, mk.is_start ? 0.f : s.length, 0.f, 0.f, chain_z);
                x = samp.x;
                y = samp.y;
                z = samp.z + kArrowFloatZ;
                const auto wt = store_world_transposed(
                    arrow_world_matrix(x, y, z, samp.heading_rad, scl));
                renderer.set_chunk_world_matrix(mk.chunk, wt.data());
                renderer.set_chunk_highlight(mk.chunk, 0.95f, 1.f, 1.f, 1.f);
            } else if (const auto def = spline::load_sli_shared(s.sli_path)) {
                if (mk.path_index < 0 || static_cast<std::size_t>(mk.path_index) >= def->paths.size()) continue;
                const auto& pp = def->paths[static_cast<std::size_t>(mk.path_index)];
                const auto samp =
                    spline::evaluate_spline(s, mk.is_start ? 0.f : s.length, pp.lateral, pp.height, chain_z);
                // Path ends: push further outward along lateral (never toward center).
                const float hx = std::sin(samp.heading_rad);
                const float hy = std::cos(samp.heading_rad);
                const float nx = hy, ny = -hx;
                const float lat_sign = (pp.lateral >= 0.f) ? 1.f : -1.f;
                x = samp.x + nx * lat_sign * 0.35f;
                y = samp.y + ny * lat_sign * 0.35f;
                z = samp.z + 0.28f;
            } else
                continue;
            if (mk.is_path) {
                const auto wt = store_world_transposed(DirectX::XMMatrixScaling(scl, scl, scl) *
                                                      XMMatrixTranslation(x, y, z));
                renderer.set_chunk_world_matrix(mk.chunk, wt.data());
                renderer.set_chunk_highlight(mk.chunk, 0.55f, 0.75f, 0.95f, 0.85f);
            }
            // Visibility driven by the per-frame hover loop.
            renderer.set_chunk_visible(mk.chunk, false);
        }
        if (!preview_only && path_rails_spline_commit_hook) path_rails_spline_commit_hook(s.id);
        refresh_spline_attachments_for_spline(s.id);
        return true;
    };
    int spline_chain_follow_guard = 0;

    // -------------------------------------------------------------------------
    // Async interactive remesh (CPU on worker, GPU upload time-sliced on main).
    // Pattern mirrors stream_kick_spline_cpu: parallel build_spline_meshes, then
    // main-thread apply_spline_gpu_meshes with a per-frame budget to avoid hitches.
    // preview_only and stream_upload stay fully synchronous (latency / existing path).
    // -------------------------------------------------------------------------
    struct AsyncRemeshJob {
        std::int32_t id = 0;
        std::uint64_t gen = 0;
        map::SplineInst inst{};
        std::optional<float> chain_z;
    };
    struct AsyncRemeshResult {
        std::int32_t id = 0;
        std::uint64_t gen = 0;
        std::vector<spline::SplineMesh> meshes;
        bool ok = false;
    };
    std::mutex async_remesh_mu;
    std::condition_variable async_remesh_cv;
    std::deque<AsyncRemeshJob> async_remesh_pending;
    std::deque<AsyncRemeshResult> async_remesh_ready;
    std::unordered_map<std::int32_t, std::uint64_t> async_remesh_gen; // latest gen per id
    std::atomic<bool> async_remesh_stop{false};
    std::atomic<bool> async_remesh_busy{false};
    // where the view is (set by the main thread each frame): the worker meshes the nearest splines first
    std::atomic<float> async_remesh_focus_x{0.f}, async_remesh_focus_y{0.f};
    std::thread async_remesh_worker;
    bool async_remesh_worker_started = false;

    auto async_remesh_ensure_worker = [&]() {
        if (async_remesh_worker_started) return;
        async_remesh_worker_started = true;
        async_remesh_stop = false;
        async_remesh_worker = std::thread([&]() {
            for (;;) {
                std::vector<AsyncRemeshJob> batch;
                {
                    std::unique_lock<std::mutex> lk(async_remesh_mu);
                    async_remesh_cv.wait(lk, [&] {
                        return async_remesh_stop.load() || !async_remesh_pending.empty();
                    });
                    if (async_remesh_stop.load() && async_remesh_pending.empty()) break;
                    // Drain all pending into one parallel batch (latest gen wins later).
                    while (!async_remesh_pending.empty()) {
                        batch.push_back(std::move(async_remesh_pending.front()));
                        async_remesh_pending.pop_front();
                    }
                }
                if (batch.empty()) continue;
                async_remesh_busy = true;
                const auto omsi_root = cfg.omsi_root;
                // Serial CPU on the dedicated remesh thread (JobSystem::parallel_for here nested with
                // main-thread pool and PSTL elsewhere -> sporadic 0xE06D7363 on first frames).
                // The nearest splines first (sorted again every 256 as the view moves), and the results handed
                // over every few splines: the boot batch is every spline of the loaded tiles (14k on Thuringer
                // Wald at 4 tiles) and went over only when all of it was done - the roads showed after minutes.
                // Their textures are read by the pool meanwhile (prefetch_textures_cpu skips what is in RAM).
                std::unordered_set<std::string> tex_sent;
                std::vector<std::filesystem::path> tex_queue;
                auto flush_tex = [&]() {
                    if (tex_queue.empty()) return;
                    omsi::JobSystem::global().submit(
                        [texs = std::move(tex_queue)]() {
                            try {
                                gfx::dx12::prefetch_textures_cpu(texs);
                            } catch (...) {
                            }
                        },
                        omsi::JobSystem::Priority::Normal);
                    tex_queue.clear();
                };
                std::vector<AsyncRemeshResult> out;
                auto flush_out = [&]() {
                    if (out.empty()) return;
                    std::lock_guard<std::mutex> lk(async_remesh_mu);
                    for (auto& r : out) async_remesh_ready.push_back(std::move(r));
                    out.clear();
                };
                auto t_flush = std::chrono::steady_clock::now();
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    if (i % 256 == 0 && batch.size() - i > 1) {
                        const float fx = async_remesh_focus_x.load(std::memory_order_relaxed);
                        const float fy = async_remesh_focus_y.load(std::memory_order_relaxed);
                        auto d2 = [&](const AsyncRemeshJob& j) {
                            const float dx = j.inst.position.x - fx, dy = j.inst.position.y - fy;
                            return dx * dx + dy * dy;
                        };
                        std::sort(batch.begin() + static_cast<std::ptrdiff_t>(i), batch.end(),
                                  [&](const AsyncRemeshJob& x, const AsyncRemeshJob& y) { return d2(x) < d2(y); });
                    }
                    if (async_remesh_stop.load()) break;
                    auto& job = batch[i];
                    out.emplace_back();
                    auto& r = out.back();
                    r.id = job.id;
                    r.gen = job.gen;
                    try {
                        const auto def = spline::load_sli_shared(job.inst.sli_path);
                        if (def) {
                            r.meshes = spline::build_spline_meshes(job.inst, *def, omsi_root, 0.f, job.chain_z);
                            r.ok = !r.meshes.empty();
                            for (const auto& m : r.meshes) {
                                if (m.texture_path.empty()) continue;
                                const auto t = gfx::dx12::resolve_texture_path(
                                    job.inst.sli_path, omsi::path_text(m.texture_path), omsi_root);
                                if (t && tex_sent.insert(omsi::path_text(*t)).second) tex_queue.push_back(*t);
                            }
                        } else {
                            r.ok = false;
                        }
                    } catch (const std::exception& e) {
                        r.ok = false;
                        log::error(std::string("async remesh worker id=") + std::to_string(job.id) + ": " +
                                   e.what());
                    } catch (...) {
                        r.ok = false;
                        log::error("async remesh worker id=" + std::to_string(job.id) + ": unknown exception");
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (out.size() >= 32 || now - t_flush > std::chrono::milliseconds(30)) {
                        flush_tex();
                        flush_out();
                        t_flush = now;
                    }
                }
                flush_tex();
                flush_out();
                async_remesh_busy = false;
            }
        });
    };

    // Undo / redo restores whole splines: results computed from the pre-undo shape must never be applied.
    auto async_remesh_invalidate_all = [&]() {
        std::lock_guard<std::mutex> lk(async_remesh_mu);
        async_remesh_pending.clear();
        async_remesh_ready.clear();
        for (auto& kv : async_remesh_gen) ++kv.second; // any in-flight job now has an older gen
    };
    auto async_remesh_backlog = [&]() -> std::size_t {
        std::lock_guard<std::mutex> lk(async_remesh_mu);
        return async_remesh_pending.size() + async_remesh_ready.size();
    };
    auto async_remesh_enqueue = [&](map::SplineInst& s) {
        async_remesh_ensure_worker();
        std::optional<float> chain_z;
        if (s.next_id != 0) {
            for (const auto& n : edit_splines)
                if (n.id == s.next_id) {
                    chain_z = spline::spline_mesh_chain_end_z(s, n.position.z);
                    break;
                }
        }
        AsyncRemeshJob job;
        job.id = s.id;
        job.inst = s; // value copy — worker never touches edit_splines
        job.chain_z = chain_z;
        {
            std::lock_guard<std::mutex> lk(async_remesh_mu);
            job.gen = ++async_remesh_gen[s.id];
            // Drop older pending jobs for same id (latest wins).
            for (auto it = async_remesh_pending.begin(); it != async_remesh_pending.end();) {
                if (it->id == s.id) it = async_remesh_pending.erase(it);
                else ++it;
            }
            async_remesh_pending.push_back(std::move(job));
        }
        async_remesh_cv.notify_one();
    };

    // Boot: tessellate every loaded spline off the main thread; GPU upload is sliced per frame.
    {
        int n = 0;
        for (auto& s : edit_splines) {
            async_remesh_enqueue(s);
            ++n;
        }
        log::info("async remesh boot enqueue n=" + std::to_string(n) +
                  (view_only ? " [VIEW-ONLY]" : " [EDIT]"));
    }

    auto async_remesh_join = [&]() {
        if (!async_remesh_worker_started) return;
        {
            std::lock_guard<std::mutex> lk(async_remesh_mu);
            async_remesh_stop = true;
        }
        async_remesh_cv.notify_all();
        if (async_remesh_worker.joinable()) async_remesh_worker.join();
        async_remesh_worker_started = false;
        async_remesh_pending.clear();
        async_remesh_ready.clear();
        async_remesh_gen.clear();
        async_remesh_busy = false;
        async_remesh_stop = false;
    };

    // CPU already ran on the worker. GPU apply is byte+ms budgeted (no fixed spline count).
    auto pump_async_remesh = [&]() {
        {
            float fx = 0.f, fy = 0.f, fz = 0.f;
            camera.eye_position(fx, fy, fz);
            async_remesh_focus_x.store(fx, std::memory_order_relaxed);
            async_remesh_focus_y.store(fy, std::memory_order_relaxed);
        }
        auto& gov = omsi::LoadGovernor::global();
        auto fb = gov.budget();
        int done = 0;
        std::uint64_t bytes = 0;
        for (;;) {
            AsyncRemeshResult r;
            {
                std::lock_guard<std::mutex> lk(async_remesh_mu);
                if (async_remesh_ready.empty()) break;
                r = std::move(async_remesh_ready.front());
                async_remesh_ready.pop_front();
            }
            {
                std::lock_guard<std::mutex> lk(async_remesh_mu);
                const auto it = async_remesh_gen.find(r.id);
                if (it != async_remesh_gen.end() && r.gen != it->second) continue;
            }
            if (!r.ok) {
                log::warn("async remesh failed id=" + std::to_string(r.id));
                continue;
            }
            map::SplineInst* sp = nullptr;
            for (auto& s : edit_splines) {
                if (s.id == r.id) {
                    sp = &s;
                    break;
                }
            }
            if (!sp) continue;
            if (!spline_meshes_sane(r.meshes)) {
                log::warn("async remesh: dropped inconsistent result id=" + std::to_string(r.id));
                continue;
            }
            std::uint64_t add = 0;
            std::uint64_t nv = 0, ni = 0;
            for (const auto& m : r.meshes) {
                add += m.vertices.size() * sizeof(m.vertices[0]);
                add += m.indices.size() * sizeof(m.indices[0]);
                nv += m.vertices.size();
                ni += m.indices.size();
            }
            auto c = omsi::FrameBudget::gpu_mesh(nv, ni);
            c.bytes = add;
            if (gov.admit(fb, omsi::LoadGovernor::Kind::Spline, c) == omsi::FrameBudget::Admit::Defer) {
                std::lock_guard<std::mutex> lk(async_remesh_mu);
                async_remesh_ready.push_front(std::move(r));
                break;
            }
            const auto t0 = std::chrono::steady_clock::now();
            apply_spline_gpu_meshes(*sp, r.meshes, false, true);
            // Unconditional WF off after async commit (preview remesh may have left strips wire).
            if (auto* gpu = find_spline_gpu(sp->id)) {
                for (auto c : gpu->chunks)
                    if (c < renderer.chunk_count()) renderer.set_chunk_wireframe(c, false);
            }
            const double dt =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            fb.charge(dt, c);
            gov.learn(omsi::LoadGovernor::Kind::Spline, c.bytes, dt);
            bytes += add;
            ++done;
            if (fb.expired() || !fb.time_ok()) break;
        }
        if (done > 0) {
            static auto s_remesh_log = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - s_remesh_log > std::chrono::seconds(1)) {
                s_remesh_log = now;
                log::info("async remesh GPU +" + std::to_string(done) + " bytes=" + std::to_string(bytes));
            }
        }
    };

    // Splines an edit showed as a wireframe preview keep that preview until "Colocar" (button / Enter /
    // another selection): the full remesh at the end of each drag turned them into white untextured
    // strips while their textures were still loading.
    std::unordered_set<std::int32_t> spline_preview_ids;
    std::unordered_set<std::int32_t> spline_pending_place;
    bool spline_place_committing = false;
    auto remesh_spline_one = [&](map::SplineInst& s, bool preview_only, bool stream_upload) -> bool {
        // Interactive full remesh: CPU off main thread. Preview + stream stay sync.
        if (!preview_only && !stream_upload) {
            if (!spline_place_committing && spline_preview_ids.count(s.id)) {
                spline_pending_place.insert(s.id);
                return true; // the wireframe preview stays until placed
            }
            spline_preview_ids.erase(s.id);
            spline_pending_place.erase(s.id);
            async_remesh_enqueue(s);
            return true;
        }
        if (preview_only && !stream_upload) spline_preview_ids.insert(s.id);
        if (stream_upload) {
            spline_preview_ids.erase(s.id);
            spline_pending_place.erase(s.id);
        }
        const auto def = spline::load_sli_shared(s.sli_path);
        if (!def) {
            log::warn("remesh spline missing sli id=" + std::to_string(s.id));
            if (!s.sli_path.empty()) {
                const auto mid = spline::evaluate_spline(s, s.length * 0.5f);
                mark_missing_asset(true, s.id, mid.x, mid.y, mid.z, s.sli_path.filename().string());
            }
            return false;
        }
        std::optional<float> chain_z;
        if (s.next_id != 0) {
            for (const auto& n : edit_splines)
                if (n.id == s.next_id) {
                    chain_z = spline::spline_mesh_chain_end_z(s, n.position.z);
                    break;
                }
        }
        const auto meshes = spline::build_spline_meshes(s, *def, cfg.omsi_root, 0.f, chain_z);
        if (meshes.empty()) {
            log::warn("remesh spline empty id=" + std::to_string(s.id));
            return false;
        }
        return apply_spline_gpu_meshes(s, meshes, preview_only, stream_upload);
    };
    auto remesh_spline = [&](map::SplineInst& s, bool preview_only = false, bool stream_upload = false) -> bool {
        const bool ok = remesh_spline_one(s, preview_only, stream_upload);
        // Pegar cadena: follow ONLY already-linked next_id successors (same .sli).
        // Never proximity-scan for new links while dragging — that was "detecting"
        // unrelated nearby splines in real time. Visual update runs in preview_only
        // so the chain moves with the primary while G/R/etc is held.
        if (!ok) return ok;
        if (spline_chain_follow_guard != 0) return ok;
        if (!panel.imgui_state().spline_stick_chain) return ok;
        ++spline_chain_follow_guard;
        std::unordered_set<std::int32_t> seen;
        std::int32_t sid = s.id;
        int hops = 0;
        for (int hop = 0; hop < 256; ++hop) {
            map::SplineInst* cur = nullptr;
            for (auto& x : edit_splines) {
                if (x.id == sid) {
                    cur = &x;
                    break;
                }
            }
            if (!cur || !seen.insert(cur->id).second) break;
            if (cur->next_id == 0 || seen.count(cur->next_id)) break;
            map::SplineInst* nxt = nullptr;
            for (auto& x : edit_splines) {
                if (x.id == cur->next_id) {
                    nxt = &x;
                    break;
                }
            }
            if (!nxt) {
                log::info("[spline_chain] break at id=" + std::to_string(cur->id) +
                          " next_id=" + std::to_string(cur->next_id) + " (missing)");
                break; // broken link or different .sli — stop, do NOT search nearby
            }
            const auto e = spline::evaluate_spline(*cur, cur->length);
            // Pegar/unir cadena (stick): siempre pega el start de next al end de cur.
            // - SIN fill_chain: NO fijar el final de next. Solo mueve start+heading y
            //   conserva L/R propios → la cadena sigue al spline editado (G/R/RR/etc).
            // - CON fill_chain: re-fit para preservar el END pose viejo (biarc-style).
            const bool do_fill = panel.imgui_state().spline_fill_chain;
            float old_ex = 0.f, old_ey = 0.f, old_ez = 0.f, old_eh = 0.f;
            if (do_fill) {
                const auto old_end = spline::evaluate_spline(*nxt, nxt->length);
                old_ex = old_end.x;
                old_ey = old_end.y;
                old_ez = old_end.z;
                old_eh = old_end.heading_rad * (180.f / 3.14159265f);
            }
            nxt->position.x = e.x;
            nxt->position.y = e.y;
            nxt->position.z = e.z;
            spline::apply_child_joint_from_parent(*nxt, *cur);
            if (do_fill) {
                float len = nxt->length, rad = nxt->radius;
                if (spline::fit_spline_to_end_heading(nxt->position.x, nxt->position.y, nxt->heading_deg,
                                                      old_ex, old_ey, old_eh, len, rad) &&
                    len >= 0.25f && std::isfinite(len) && std::isfinite(rad)) {
                    nxt->length = len;
                    nxt->radius = rad;
                } else {
                    spline::fit_spline_to_xy(nxt->position.x, nxt->position.y, nxt->heading_deg,
                                             old_ex, old_ey, len, rad);
                    if (len < 0.25f) len = 0.25f;
                    nxt->length = len;
                    nxt->radius = rad;
                }
                (void)old_ez;
            }
            // Keep existing prev/next; do not rewire or invent links.
            // L/R sin fill se dejan intactos → cadena se arrastra libre.
            remesh_spline_one(*nxt, preview_only, stream_upload);
            ++hops;
            sid = nxt->id;
        }
        if (hops > 0 && !preview_only)
            log::info("[spline_chain] followed " + std::to_string(hops) +
                      " hop(s) from id=" + std::to_string(s.id));
        --spline_chain_follow_guard;
        return ok;
    };
    auto commit_spline_place = [&]() {
        if (spline_pending_place.empty() && spline_preview_ids.empty()) return;
        std::unordered_set<std::int32_t> ids = spline_pending_place;
        ids.insert(spline_preview_ids.begin(), spline_preview_ids.end());
        spline_place_committing = true;
        for (auto& sp : edit_splines)
            if (ids.count(sp.id)) remesh_spline(sp, false);
        spline_place_committing = false;
        spline_pending_place.clear();
        spline_preview_ids.clear();
        log::info("spline place commit n=" + std::to_string(ids.size()));
    };
    auto sync_prev_skew_from_child = [&](map::SplineInst& child) {
        if (child.prev_id == 0) return;
        for (auto& p : edit_splines) {
            if (p.id != child.prev_id) continue;
            spline::apply_parent_skew_from_child_heading(p, child);
            break;
        }
    };
    auto clear_spline_wireframe = [&](const map::SplineInst& sp) {
        if (spline_pending_place.count(sp.id) || spline_preview_ids.count(sp.id)) return; // until "Colocar"
        if (auto* gpu = find_spline_gpu(sp.id)) {
            for (const auto c : gpu->chunks) {
                if (c < renderer.chunk_count()) renderer.set_chunk_wireframe(c, false);
            }
        }
    };
    // OMSI TFileSpline.RefreshNachfolger: rewrite next pose from parent end + joint.
    // Side-effect remeshes never stay in wireframe (only actively moved ids may).
    auto refresh_nachfolger = [&](map::SplineInst& parent, bool preview_only = false) {
        std::unordered_set<std::int32_t> seen;
        map::SplineInst* cur = &parent;
        for (int hop = 0; hop < 256; ++hop) {
            if (!seen.insert(cur->id).second) break;
            if (cur->next_id == 0) break;
            map::SplineInst* nxt = nullptr;
            for (auto& x : edit_splines) {
                if (x.id == cur->next_id) {
                    nxt = &x;
                    break;
                }
            }
            if (!nxt) break;
            const auto e = spline::evaluate_spline(*cur, cur->length);
            nxt->position.x = e.x;
            nxt->position.y = e.y;
            nxt->position.z = e.z;
            spline::apply_child_joint_from_parent(*nxt, *cur);
            remesh_spline_one(*nxt, preview_only, false);
            clear_spline_wireframe(*nxt);
            cur = nxt;
        }
    };
    // After rotating `moved` ids: incoming joint from unselected prev (Ctrl → skew_end=0).
    auto after_rotate_spline_ids = [&](const std::unordered_set<std::int32_t>& moved, bool preview_only,
                                       bool ctrl_straight) {
        if (moved.empty()) return;
        std::unordered_set<std::int32_t> done_refresh;
        for (auto& s : edit_splines) {
            if (!moved.count(s.id)) continue;
            if (s.prev_id != 0 && !moved.count(s.prev_id)) {
                for (auto& p : edit_splines) {
                    if (p.id != s.prev_id) continue;
                    if (ctrl_straight) {
                        p.skew_end = 0.f;
                        s.skew_start = 0.f;
                    } else {
                        spline::apply_parent_skew_from_child_heading(p, s);
                    }
                    // Parent is a side-effect: remesh then always clear WF.
                    remesh_spline_one(p, preview_only, false);
                    clear_spline_wireframe(p);
                    if (done_refresh.insert(p.id).second) refresh_nachfolger(p, preview_only);
                    break;
                }
            }
            remesh_spline_one(s, preview_only, false);
            clear_spline_wireframe(s);
            if (done_refresh.insert(s.id).second) refresh_nachfolger(s, preview_only);
        }
        // Commit / any leftover: force WF off on whole moved set + neighbors.
        for (auto& s : edit_splines) {
            if (!moved.count(s.id) && !moved.count(s.prev_id) && !moved.count(s.next_id)) continue;
            clear_spline_wireframe(s);
        }
    };
    auto unlink_spline_chain_ids = [&](std::int32_t sid) {
        if (sid == 0) return;
        for (auto& s : edit_splines) {
            if (s.id == sid) {
                s.prev_id = 0;
                s.next_id = 0;
            } else {
                if (s.next_id == sid) s.next_id = 0;
                if (s.prev_id == sid) s.prev_id = 0;
            }
        }
    };
    auto maybe_unlink_on_free_move = [&](std::int32_t sid) {
        if (panel.imgui_state().spline_stick_chain) return;
        unlink_spline_chain_ids(sid);
    };
    // In a co-op room every editor allocates new ids in its own range (host: < 100M, guest k: 100M + k*10M),
    // so two people placing objects at the same time never create the same id. Outside a room: max + 1.
    auto next_spline_id = [&]() -> std::int32_t {
        std::int32_t lo = 1, hi = 0x7fffffff;
        omsi::coop::Session::get().id_range(lo, hi);
        std::int32_t m = lo - 1;
        for (const auto& s : edit_splines)
            if (s.id >= lo && s.id <= hi) m = (std::max)(m, s.id);
        for (const auto& s : transient_splines)
            if (s.id >= lo && s.id <= hi) m = (std::max)(m, s.id);
        return m + 1;
    };
    auto next_object_id = [&]() -> std::int32_t {
        std::int32_t lo = 1, hi = 0x7fffffff;
        omsi::coop::Session::get().id_range(lo, hi);
        std::int32_t m = lo - 1;
        for (const auto& o : edit_objects)
            if (o.id >= lo && o.id <= hi) m = (std::max)(m, o.id);
        for (const auto& o : transient_objects)
            if (o.id >= lo && o.id <= hi) m = (std::max)(m, o.id);
        for (const auto& e : edit_spline_attachments)
            if (e.att.instance_id >= lo && e.att.instance_id <= hi) m = (std::max)(m, e.att.instance_id);
        return m + 1;
    };

    auto tile_size_at = [&](std::int32_t gx, std::int32_t gy) -> float {
        float size_m = map::kStandardTileMeters;
        if (const auto* m = world.tile_layout().find_grid(gx, gy)) size_m = m->size_meters;
        return size_m;
    };
    auto find_tile_mut_xy = [&](float ex, float ey) -> map::MapTileData* {
        map::MapTileData* best = nullptr;
        float best_d2 = 1.e30f;
        for (auto& tile : world.tiles_mut()) {
            const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
            const float size_m = tile_size_at(tile.grid_x, tile.grid_y);
            const float lx = ex - rx;
            const float ly = ey - ry;
            if (lx >= -0.5f && ly >= -0.5f && lx <= size_m + 0.5f && ly <= size_m + 0.5f) return &tile;
            const float cx = rx + size_m * 0.5f;
            const float cy = ry + size_m * 0.5f;
            const float dx = ex - cx;
            const float dy = ey - cy;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = &tile;
            }
        }
        return best;
    };
    auto sync_tile_lists_only = [&](map::MapTileData& tile) {
        const map::MapTileData before_sync = tile;
        const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
        const float size_m = tile_size_at(tile.grid_x, tile.grid_y);
        tile.objects.clear();
        tile.splines.clear();
        std::unordered_set<std::int32_t> attach_child_ids;
        for (const auto& a : edit_attachments) attach_child_ids.insert(a.child_id);
        for (const auto& o : edit_objects) {
            if (spline_attach_spawn_ids.count(o.id)) continue;
            if (attach_child_ids.count(o.id)) continue;
            const float lx = o.position.x - rx;
            const float ly = o.position.y - ry;
            if (lx < -1.f || ly < -1.f || lx > size_m + 1.f || ly > size_m + 1.f) continue;
            auto c = o;
            c.position.x = lx;
            c.position.y = ly;
            tile.objects.push_back(std::move(c));
        }
        for (const auto& s : edit_splines) {
            const float lx = s.position.x - rx;
            const float ly = s.position.y - ry;
            if (lx < -1.f || ly < -1.f || lx > size_m + 1.f || ly > size_m + 1.f) continue;
            auto c = s;
            c.position.x = lx;
            c.position.y = ly;
            tile.splines.push_back(std::move(c));
        }
        tile.attachments.clear();
        for (const auto& a : edit_attachments) {
            bool on_tile = false;
            std::int32_t walk = a.parent_id;
            for (int guard = 0; guard < 64 && !on_tile; ++guard) {
                for (const auto& o : tile.objects) {
                    if (o.id == walk) {
                        on_tile = true;
                        break;
                    }
                }
                if (on_tile) break;
                bool found = false;
                for (const auto& b : edit_attachments) {
                    if (b.child_id != walk) continue;
                    walk = b.parent_id;
                    found = true;
                    break;
                }
                if (!found) break;
            }
            if (on_tile) tile.attachments.push_back(a);
        }
        tile.spline_attachments.clear();
        for (const auto& entry : edit_spline_attachments) {
            if (entry.tile_gx != tile.grid_x || entry.tile_gy != tile.grid_y) continue;
            if (editor_deleted_object_ids.count(entry.att.instance_id)) continue;
            if (entry.parent_spline_id > 0 && editor_deleted_spline_ids.count(entry.parent_spline_id))
                continue;
            auto att = entry.att;
            if (entry.parent_spline_id > 0) {
                int found = -1;
                for (std::size_t si = 0; si < tile.splines.size(); ++si) {
                    if (tile.splines[si].id == entry.parent_spline_id) {
                        found = static_cast<int>(si);
                        break;
                    }
                }
                if (found < 0) continue;
                att.spline_index = found;
            }
            tile.spline_attachments.push_back(std::move(att));
        }
        map::remap_objects_from_file_order(tile, before_sync);
    };
    auto sync_and_write_tile = [&](map::MapTileData& tile) {
        if (view_only) {
            log::info("skip tile write (VIEW-ONLY) " + tile.path.filename().string());
            return;
        }
        sync_tile_lists_only(tile);
        if (!cfg.workspace || cfg.workspace->actual_dir.empty()) {
            log::warn("refused tile write: no workspace (would hit live map) " + tile.path.string());
            return;
        }
        const auto write_path = map::workspace_overlay_write_path(*cfg.workspace, tile.path);
        if (write_path.empty()) {
            log::warn("refused tile write outside workspace: " + tile.path.string());
            return;
        }
        const auto read_fb =
            map::workspace_tile_map_for_read(cfg.workspace ? &*cfg.workspace : nullptr, tile.path);
        if (!map::write_tile_map_file(write_path, tile, cfg.omsi_root, read_fb)) {
            log::warn("persist tile failed " + write_path.string());
        }
    };
    auto tile_dirty_key = [](std::int32_t gx, std::int32_t gy) -> std::int64_t {
        return (static_cast<std::int64_t>(gx) << 32) | static_cast<std::uint32_t>(gy);
    };
    std::unordered_set<std::int64_t> dirty_tile_keys;
    auto mark_tile_dirty = [&](std::int32_t gx, std::int32_t gy) {
        // (diagnostic: the first marks of a session with their call stack as module RVAs - a session
        //  nobody edited asked on exit whether to keep *_actual and wrote tiles)
        static int s_dirty_logged = 0;
        if (s_dirty_logged < 6 && dirty_tile_keys.count(tile_dirty_key(gx, gy)) == 0) {
            ++s_dirty_logged;
            void* frames[8]{};
            const USHORT n = RtlCaptureStackBackTrace(0, 8, frames, nullptr);
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::string st;
            char b[24];
            for (USHORT k = 0; k < n; ++k) {
                std::snprintf(b, sizeof(b), " 0x%llx",
                              static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[k]) - base));
                st += b;
            }
            log::info("tile dirty (" + std::to_string(gx) + "," + std::to_string(gy) + ") rva:" + st);
        }
        dirty_tile_keys.insert(tile_dirty_key(gx, gy));
        if (cfg.had_map_edits_out) *cfg.had_map_edits_out = true;
    };
    auto persist_editor_xy = [&](float ex, float ey) {
        // Mark dirty only — stream unload / Ctrl+S write the tile. Avoids blocking I/O on every edit.
        if (auto* t = find_tile_mut_xy(ex, ey)) {
            mark_tile_dirty(t->grid_x, t->grid_y);
        }
    };
    // Mark BOTH old and new tile when something moves across a boundary (avoids
    // "edit disappears when returning to previous tile").
    auto persist_editor_xy_move = [&](float old_ex, float old_ey, float new_ex, float new_ey) {
        persist_editor_xy(old_ex, old_ey);
        persist_editor_xy(new_ex, new_ey);
    };
    auto upsert_transient_spline = [&](const map::SplineInst& s) {
        for (auto& ts : transient_splines) {
            if (ts.id == s.id) {
                ts = s;
                return;
            }
        }
        transient_splines.push_back(s);
    };
    auto upsert_transient_object = [&](const map::MapObjectInst& o) {
        for (auto& to : transient_objects) {
            if (to.id == o.id) {
                to = o;
                return;
            }
        }
        transient_objects.push_back(o);
    };
    // Keep edits alive across tile stream: transient + dirty key so unload writes .map.
    auto touch_edited_spline = [&](map::SplineInst& s) {
        upsert_transient_spline(s);
        persist_editor_xy(s.position.x, s.position.y);
    };
    auto touch_edited_object = [&](map::MapObjectInst& o) {
        upsert_transient_object(o);
        persist_editor_xy(o.position.x, o.position.y);
    };
    struct AerialThumb {
        std::int32_t gx = 0, gy = 0;
        int w = 0, h = 0;
        std::vector<std::uint8_t> bgr;
    };
    std::unordered_map<std::int64_t, AerialThumb> aerial_thumbs;
    auto aerial_key = [](std::int32_t gx, std::int32_t gy) -> std::int64_t {
        return (static_cast<std::int64_t>(gx) << 32) | static_cast<std::uint32_t>(gy);
    };
    auto load_aerial_thumb = [&](std::int32_t gx, std::int32_t gy, const std::filesystem::path& jpg) -> const AerialThumb* {
        const std::int64_t key = aerial_key(gx, gy);
        if (const auto it = aerial_thumbs.find(key); it != aerial_thumbs.end()) return &it->second;
        std::ifstream in(jpg, std::ios::binary);
        if (!in) return nullptr;
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty()) return nullptr;
        IWICImagingFactory* factory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return nullptr;
        AerialThumb thumb{};
        thumb.gx = gx;
        thumb.gy = gy;
        IWICStream* stream = nullptr;
        if (SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes.data())),
                       static_cast<DWORD>(bytes.size())))) {
            IWICBitmapDecoder* dec = nullptr;
            if (SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) {
                IWICBitmapFrameDecode* frame = nullptr;
                if (SUCCEEDED(dec->GetFrame(0, &frame))) {
                    IWICFormatConverter* conv = nullptr;
                    if (SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
                        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr,
                                                 0.f, WICBitmapPaletteTypeCustom))) {
                        UINT fw = 0, fh = 0;
                        conv->GetSize(&fw, &fh);
                        thumb.w = static_cast<int>(fw);
                        thumb.h = static_cast<int>(fh);
                        thumb.bgr.resize(static_cast<std::size_t>(fw) * static_cast<std::size_t>(fh) * 3);
                        if (SUCCEEDED(conv->CopyPixels(nullptr, fw * 3, static_cast<UINT>(thumb.bgr.size()),
                                                        thumb.bgr.data()))) {
                            aerial_thumbs.emplace(key, std::move(thumb));
                        }
                        conv->Release();
                    }
                    frame->Release();
                }
                dec->Release();
            }
        }
        if (stream) stream->Release();
        factory->Release();
        const auto it = aerial_thumbs.find(key);
        return it != aerial_thumbs.end() ? &it->second : nullptr;
    };
    auto invalidate_aerial_thumb = [&](std::int32_t gx, std::int32_t gy) {
        aerial_thumbs.erase(aerial_key(gx, gy));
    };
    auto sample_ground_luminance = [&](float ex, float ey) -> float {
        float base = 0.42f;
        if (geo_settings.tile_aerial_visible) {
            if (map::MapTileData* tile = find_tile_mut_xy(ex, ey)) {
                if (const auto jpg = map::find_tile_aerial_jpg(cfg.omsi_root, cfg.global.root, tile->path)) {
                    if (const AerialThumb* thumb = load_aerial_thumb(tile->grid_x, tile->grid_y, *jpg)) {
                        const auto [rx, ry] = tile_rel(tile->grid_x, tile->grid_y);
                        const float size_m = tile_size_at(tile->grid_x, tile->grid_y);
                        const float lx = (std::max)(0.f, (std::min)(size_m, ex - rx));
                        const float ly = (std::max)(0.f, (std::min)(size_m, ey - ry));
                        const int px =
                            (std::max)(0, (std::min)(thumb->w - 1, static_cast<int>(lx / size_m * thumb->w)));
                        const int py =
                            (std::max)(0, (std::min)(thumb->h - 1, static_cast<int>((1.f - ly / size_m) * thumb->h)));
                        const std::size_t i =
                            (static_cast<std::size_t>(py) * static_cast<std::size_t>(thumb->w) +
                             static_cast<std::size_t>(px)) *
                            3;
                        if (i + 2 < thumb->bgr.size()) {
                            const float b = thumb->bgr[i] / 255.f;
                            const float g = thumb->bgr[i + 1] / 255.f;
                            const float r = thumb->bgr[i + 2] / 255.f;
                            base = 0.114f * b + 0.587f * g + 0.299f * r;
                        }
                    }
                }
            }
        }
        float eye_x = 0.f, eye_y = 0.f, eye_z = 0.f;
        camera.eye_position(eye_x, eye_y, eye_z);
        const float dx = ex - eye_x;
        const float dy = ey - eye_y;
        const float dz = -eye_z;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float view_factor = (len > 1.e-4f) ? (std::max)(0.25f, dz / len) : 1.f;
        return base * view_factor;
    };
    auto persist_all_loaded_tiles = [&]() {
        for (auto& tile : world.tiles_mut()) sync_and_write_tile(tile);
    };
    // Ctrl+S / Load map: *_actual always; optional overwrite live .map (.bak) or "cambios/" dump.
    bool save_job_active = false;
    // save_overwrite_original already declared near entrypoints_dirty (used by flush lambda).
    std::size_t save_job_i = 0;
    std::size_t save_job_n = 0;
    std::vector<map::MapTileData*> save_job_queue;
    std::filesystem::path save_cambios_dir;
    std::filesystem::path save_live_dir; // original map folder (set when Ctrl+S starts)
    auto mirror_tt_to_destination = [&]() {
        auto& m = omsi::tt::global_tt_man();
        if (m.data.root.empty()) return;
        if (save_overwrite_original && !save_live_dir.empty())
            (void)omsi::tt::mirror_ttdata(m.data.root, save_live_dir / m.data.root.filename());
        else if (!save_cambios_dir.empty())
            (void)omsi::tt::mirror_ttdata(m.data.root, save_cambios_dir / m.data.root.filename());
    };
    auto write_one_save_tile = [&](map::MapTileData& tile) -> bool {
        sync_tile_lists_only(tile);
        std::filesystem::path actual_path = tile.path;
        std::filesystem::path read_fb = tile.path;
        if (cfg.workspace && !cfg.workspace->actual_dir.empty()) {
            const auto wp = map::workspace_overlay_write_path(*cfg.workspace, tile.path);
            if (!wp.empty()) actual_path = wp;
            read_fb = map::workspace_tile_map_for_read(cfg.workspace ? &*cfg.workspace : nullptr, tile.path);
        }
        bool ok = map::write_tile_map_file(actual_path, tile, cfg.omsi_root, read_fb);
        if (ok && save_overwrite_original) {
            std::filesystem::path live_path;
            if (cfg.workspace && !cfg.workspace->actual_dir.empty())
                live_path = map::workspace_live_tile_path(*cfg.workspace, tile.path);
            else
                live_path = map::live_map_dir_from_folder(cfg.global.root) / tile.path.filename();
            if (!live_path.empty()) {
                std::error_code mk;
                std::filesystem::create_directories(live_path.parent_path(), mk);
                if (!map::backup_file_timestamped_bak(live_path)) {
                    log::warn("save bak failed " + live_path.string());
                    ok = false;
                } else if (!map::write_tile_map_file(live_path, tile, cfg.omsi_root, read_fb)) {
                    log::warn("save original failed " + live_path.string());
                    ok = false;
                }
            }
        } else if (ok && !save_overwrite_original && !save_cambios_dir.empty()) {
            const auto out_path = save_cambios_dir / tile.path.filename();
            if (!map::write_tile_map_file(out_path, tile, cfg.omsi_root, read_fb)) {
                log::warn("save cambios failed " + out_path.string());
                ok = false;
            }
        }
        if (ok) dirty_tile_keys.erase(tile_dirty_key(tile.grid_x, tile.grid_y));
        return ok;
    };
    // Terrain edits live in memory (GPU is refreshed from the height field); the .terrain files are written on Save.
    std::unordered_set<std::int64_t> terrain_dirty_tiles;
    std::function<void()> flush_terrain_for_save;
    auto begin_save_job = [&]() -> bool {
        if (view_only) {
            panel.set_status("Modo solo visualización — guardar bloqueado");
            editor_log::event("VIEW_ONLY_BLOCK", "Ctrl+S / save");
            return false;
        }
        if (save_job_active) return false;
        if (!map::active_chrono_dirs().empty()) {
            // the tiles hold the chrono scenarios laid over the map: saving would write them into the base map
            panel.set_status("Vista de crono activa: volvé a «Mapa base (sin cronos)» en Vista para guardar");
            return false;
        }
        if (omsi::coop::Session::get().role() == omsi::coop::Role::Guest) {
            // In a co-op room only the host writes the map: a guest's edits already live in the host's
            // editor (and stay there after the guest leaves, until the host saves or discards them).
            panel.set_status("Sala cooperativa: solo el anfitrion guarda; tus cambios ya estan en su mapa");
            return false;
        }
        if (cfg.beamng) return bng_save_hook ? bng_save_hook() : false; // no OMSI tiles in BeamNG mode
        if (flush_terrain_for_save) flush_terrain_for_save();
        save_job_queue.clear();
        for (auto& tile : world.tiles_mut()) {
            if (dirty_tile_keys.count(tile_dirty_key(tile.grid_x, tile.grid_y)))
                save_job_queue.push_back(&tile);
        }
        if (save_job_queue.empty()) {
            panel.set_status("Nada que guardar");
            return false;
        }
#ifdef _WIN32
        const HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
        const int ans = MessageBoxW(
            owner,
            L"¿Dónde guardar los .map?\n\n"
            L"Sí = mapa ORIGINAL (maps\\Nombre\\). Antes copia cada .map a .bak+fecha.\n"
            L"No = carpeta «Nombre cambios» (tenés que copiar a mano al mapa de OMSI).\n"
            L"Cancelar = no guardar nada.\n\n"
            L"Siempre se escribe también en *_actual.",
            L"Feditor — Guardar mapa", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
        if (ans == IDCANCEL) {
            panel.set_status("Guardar cancelado");
            save_job_queue.clear();
            return false;
        }
        save_overwrite_original = (ans == IDYES);
#else
        save_overwrite_original = false;
#endif
        const auto live_dir = cfg.workspace && !cfg.workspace->actual_dir.empty()
                                  ? map::workspace_live_map_dir(*cfg.workspace)
                                  : map::live_map_dir_from_folder(cfg.global.root);
        save_live_dir = live_dir;
        const auto base_name = live_dir.empty() ? cfg.global.root.filename().string() : live_dir.filename().string();
        if (base_name.empty()) {
            panel.set_status("No se pudo determinar nombre del mapa");
            save_job_queue.clear();
            return false;
        }
        save_cambios_dir.clear();
        if (!save_overwrite_original) {
            if (!cfg.omsi_root.empty())
                save_cambios_dir = cfg.omsi_root / "maps" / (base_name + " cambios");
            else
                save_cambios_dir = live_dir.parent_path() / (base_name + " cambios");
            std::error_code ec;
            std::filesystem::create_directories(save_cambios_dir, ec);
            if (ec) {
                panel.set_status("Error creando carpeta de cambios");
                log::warn("save cambios mkdir failed: " + save_cambios_dir.string());
                save_job_queue.clear();
                return false;
            }
        }
        save_job_active = true;
        save_job_i = 0;
        save_job_n = save_job_queue.size();
        const std::string dest_label = save_overwrite_original ? std::string("original + *_actual")
                                                               : save_cambios_dir.filename().string() + " + *_actual";
        panel.set_status("Guardando " + std::to_string(save_job_n) + " .map (" + dest_label + ")...");
        editor_log::event("KEY_CTRL_S", std::string(save_overwrite_original ? "save_overwrite_start" : "save_cambios_start") +
                                            " n=" + std::to_string(save_job_n));
        return true;
    };
    auto pump_save_job = [&]() {
        if (!save_job_active) return;
        save_job_n = save_job_queue.size();
        if (save_job_i < save_job_queue.size()) {
            write_one_save_tile(*save_job_queue[save_job_i]);
            ++save_job_i;
        }
        if (save_job_i >= save_job_queue.size()) {
            save_job_active = false;
            if (cfg.ctrl_s_saved_out) *cfg.ctrl_s_saved_out = true;
            // TTimeTableMan_Save equivalent: flush TTData only with map save
            if (omsi::tt::global_tt_man().dirty || omsi::tt::global_tt_man().loaded) {
                if (omsi::tt::global_tt_man().data.root.empty())
                    omsi::tt::global_tt_man().data.root = cfg.global.root / "TTData";
                omsi::tt::global_tt_man().save();
                mirror_tt_to_destination();
            }
            flush_entrypoints_to_disk_if_dirty();

            std::string msg = "Guardado *_actual (" + std::to_string(save_job_n) + " .map)";
            if (save_overwrite_original) msg += " y originales (.bak previo)";
            else if (!save_cambios_dir.empty()) msg += " y " + save_cambios_dir.string();
            if (omsi::tt::global_tt_man().loaded) msg += " + TTData";
            panel.set_status(msg);
            log::info("editor Ctrl+S " + msg);
            editor_log::event("KEY_CTRL_S", "save_done n=" + std::to_string(save_job_n));
        }
    };
    auto finish_save_job = [&]() {
        if (!save_job_active) return;
        while (save_job_i < save_job_queue.size()) {
            write_one_save_tile(*save_job_queue[save_job_i]);
            ++save_job_i;
        }
        save_job_active = false;
        if (cfg.ctrl_s_saved_out) *cfg.ctrl_s_saved_out = true;
        if (omsi::tt::global_tt_man().dirty || omsi::tt::global_tt_man().loaded) {
            if (omsi::tt::global_tt_man().data.root.empty())
                omsi::tt::global_tt_man().data.root = cfg.global.root / "TTData";
            omsi::tt::global_tt_man().save();
            mirror_tt_to_destination();
        }
        std::string msg = "Guardado *_actual (" + std::to_string(save_job_n) + " .map)";
        if (save_overwrite_original) msg += " y originales (.bak previo)";
        else if (!save_cambios_dir.empty()) msg += " y " + save_cambios_dir.string();
        if (omsi::tt::global_tt_man().loaded) msg += " + TTData";
        panel.set_status(msg);
        log::info("editor save flush " + msg);
    };
    constexpr float kPathRailZLift = 0.f; // strict map_z + path.z (was +0.12 m)
    auto make_rail_strip = [&](const map::SplineInst& s, float width, float lat0, float h0) {
        std::vector<o3d::Vertex> verts;
        std::vector<std::uint32_t> inds;
        std::optional<float> chain_z;
        if (s.next_id != 0) {
            for (const auto& n : edit_splines)
                if (n.id == s.next_id) {
                    chain_z = spline::spline_mesh_chain_end_z(s, n.position.z);
                    break;
                }
        }
        const int segs = (std::max)(2, spline::omis_teile(s, chain_z));
        // Allow thin modern strips (width 0.06 → half 0.03); classic helper stays ≥0.2 m half.
        const float half = (width < 0.25f) ? (std::max)(0.02f, width * 0.5f) : (std::max)(0.2f, width * 0.5f);
        const float zlift = kPathRailZLift; // same lift as classic helper (no modern offset)
        constexpr float kPathTiling = 0.1f; // helper/path.sli [length] 10
        for (int i = 0; i <= segs; ++i) {
            const float along = s.length * (static_cast<float>(i) / static_cast<float>(segs));
            const auto L = spline::evaluate_spline(s, along, lat0 - half, h0, chain_z);
            const auto R = spline::evaluate_spline(s, along, lat0 + half, h0, chain_z);
            o3d::Vertex a{}, b{};
            a.position[0] = L.x; a.position[1] = L.y; a.position[2] = L.z + zlift;
            b.position[0] = R.x; b.position[1] = R.y; b.position[2] = R.z + zlift;
            a.normal[2] = b.normal[2] = 1.f;
            a.uv[0] = 0.f;
            b.uv[0] = 1.f;
            a.uv[1] = b.uv[1] = along * kPathTiling;
            verts.push_back(a);
            verts.push_back(b);
            if (i > 0) {
                const std::uint32_t i0 = static_cast<std::uint32_t>((i - 1) * 2);
                const std::uint32_t i1 = i0 + 1;
                const std::uint32_t i2 = static_cast<std::uint32_t>(i * 2);
                const std::uint32_t i3 = i2 + 1;
                inds.insert(inds.end(), {i0, i2, i1, i1, i2, i3});
            }
        }
        return std::make_pair(std::move(verts), std::move(inds));
    };
    map_editor::PathEditorState sco_path_edit;
    panel.set_path_editor_state(&sco_path_edit);
#include "modules/mod_path_rails.inc.cpp"
path_rails_spline_commit_hook = [&](std::int32_t id) { mark_path_rails_dirty_spline_id(id); };
