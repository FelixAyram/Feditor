// MOD: group — aislado (Shift+S settle async, clear grab). Motor compartido.
    // Async group place: main thread applies items progressively (visible settle);
    // disk flush runs on a worker with std::execution::par (saturate cores).
    // GPU/DX12 stays on the render thread â€” classic producerâ†’main-queue pattern.
    struct GroupSettleItem {
        int kind = 0; // 0=object 1=spline
        std::int32_t id = 0;
    };
    std::vector<GroupSettleItem> group_settle_items;
    std::size_t group_settle_i = 0;
    bool group_settle_remesh = false;
    std::unordered_set<std::int64_t> group_settle_dirty;
    std::chrono::steady_clock::time_point group_settle_t0{};
    std::thread group_settle_persist_thread;
    std::atomic<bool> group_settle_persist_done{true};
    std::size_t group_settle_tile_n = 0;
    std::size_t group_settle_obj_n = 0;
    std::size_t group_settle_spl_n = 0;

    auto clear_group_grab_interaction = [&]() {
        group_grab_active = false;
        group_rotate_active = false;
        group_rot_have_ref = false;
        group_grab_have_mouse_ref = false;
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
        shift_s_move_clear();
    };

    auto multisel_debug = []() -> bool {
        static int cached = -1;
        if (cached < 0) {
            char* e = nullptr;
            size_t n = 0;
            if (_dupenv_s(&e, &n, "OMSI_MULTI_DEBUG") != 0 || !e)
                cached = 0;
            else {
                cached = (e[0] == '1' || e[0] == 'y' || e[0] == 'Y') ? 1 : 0;
                free(e);
            }
        }
        return cached == 1;
    };

    auto log_multisel = [&](const std::string& msg) {
        log::info("multisel " + msg);
        editor_log::event("MULTI_SEL", msg);
    };

    auto multisel_log_state = [&](const char* tag) {
        if (!multisel_debug()) return;
        log_multisel(std::string(tag) + " rot=" + std::to_string(group_rotate_active) + " grab=" +
                     std::to_string(group_grab_active) + " shift_s=" + std::to_string(shift_s_move.active) +
                     " grab_idx=" + std::to_string(group_grab_index.size()) + " obj_sel=" +
                     std::to_string(group_sel_obj.size()) + " spl_sel=" + std::to_string(group_sel_spl.size()) +
                     " edit_mode=" + std::to_string(static_cast<int>(edit_mode)) + " settle_phase=" +
                     std::to_string(static_cast<int>(group_settle_phase)));
    };

    auto build_group_settle_items = [&]() -> std::vector<GroupSettleItem> {
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
        return items;
    };

    auto snap_group_grab_to_gpu = [&]() {
        multisel_log_state("snap_gpu_begin");
        for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
            if (group_grab_kind[i] == 0) {
                if (static_cast<std::size_t>(group_grab_index[i]) >= edit_objects.size()) continue;
                auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                sync_object_gpu(o);
                upsert_transient_object(o);
                for (auto& sp : scene_places) {
                    if (sp.object_id == o.id) sp.pinned = false;
                }
            } else {
                if (static_cast<std::size_t>(group_grab_index[i]) >= edit_splines.size()) continue;
                auto& s = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
                remesh_spline(s, false);
                upsert_transient_spline(s);
                if (auto* g = find_spline_gpu(s.id)) g->pinned = false;
            }
        }
        multisel_log_state("snap_gpu_done");
    };

    auto begin_group_settle = [&](std::vector<GroupSettleItem> items, bool need_remesh) {
        if (group_settle_persist_thread.joinable()) {
            if (!group_settle_persist_done.load()) {
                panel.set_status("EsperÃ¡: guardando grupo anterior...");
                return false;
            }
            group_settle_persist_thread.join();
        }
        group_settle_obj_n = 0;
        group_settle_spl_n = 0;
        for (const auto& it : items) {
            if (it.kind == 0) ++group_settle_obj_n;
            else ++group_settle_spl_n;
        }
        group_settle_items = std::move(items);
        group_settle_i = 0;
        group_settle_remesh = need_remesh;
        group_settle_dirty.clear();
        group_settle_tile_n = 0;
        group_settle_t0 = std::chrono::steady_clock::now();
        group_settle_persist_done.store(true);
        group_settle_phase = GroupSettlePhase::Applying;
        clear_group_grab_interaction();
        panel.set_status("Asentando grupo 0/" + std::to_string(group_settle_items.size()) +
                         " (async, UI libre)...");
        editor_log::event("SHIFT_S_GROUP_SETTLE_BEGIN",
                          "count=" + std::to_string(group_settle_items.size()) +
                              (need_remesh ? " remesh" : " matrix_only") + " obj=" +
                              std::to_string(group_settle_obj_n) + " spl=" +
                              std::to_string(group_settle_spl_n) + " | " + session_state());
        return true;
    };

    auto confirm_group_rotate_place = [&](const char* reason) -> bool {
        log_multisel(std::string("rotate_lmb ") + reason);
        multisel_log_state("rotate_lmb");
        if (!group_rotate_active || group_grab_index.empty()) {
            log::info(std::string("editor group_rotate LMB skip (") + reason + ") rot=" +
                      std::to_string(group_rotate_active) + " idx=" + std::to_string(group_grab_index.size()));
            return false;
        }
        snap_group_grab_to_gpu();
        path_rails_detect_now = true; // rotated owners get fresh rails (signatures changed)
        auto items = build_group_settle_items();
        const std::size_t n = items.size();
        if (!begin_group_settle(std::move(items), true)) {
            panel.set_status("Grupo: espera a que termine el guardado anterior");
            log::info("editor group_rotate LMB busy n=" + std::to_string(n));
            return false;
        }
        panel.set_status("Rotacion grupo confirmada (LMB) " + std::to_string(n));
        log::info("editor group_rotate LMB confirm n=" + std::to_string(n));
        editor_log::event("LMB_PLACE", "confirm group rotate | " + session_state());
        edit_mode = EditMode::None;
        grabbing = false;
        rotating = false;
        new_grab_lock_xy = false;
        r_tap = 0;
        g_tap = 0;
        return true;
    };

    auto pump_group_settle = [&]() {
        if (group_settle_phase == GroupSettlePhase::Idle) return;

        if (group_settle_phase == GroupSettlePhase::Applying) {
            // Cap main-thread work so frames keep rendering; user sees items lock in one-by-one.
            auto& gov = omsi::LoadGovernor::global();
            auto grp_fb = gov.budget();
            auto mark_tile = [&](float ex, float ey) {
                if (auto* t = find_tile_mut_xy(ex, ey))
                    group_settle_dirty.insert(tile_key(t->grid_x, t->grid_y));
            };
            while (group_settle_i < group_settle_items.size()) {
                omsi::FrameBudget::WorkCost c{32ull * 1024ull, 256, 384, 1, 0, 0.2};
                if (gov.admit(grp_fb, omsi::LoadGovernor::Kind::Generic, c) == omsi::FrameBudget::Admit::Defer)
                    break;
                const auto item = group_settle_items[group_settle_i++];
                const auto t0 = std::chrono::steady_clock::now();
                if (item.kind == 0) {
                    map::MapObjectInst* po = nullptr;
                    for (auto& o : edit_objects) {
                        if (o.id == item.id) {
                            po = &o;
                            break;
                        }
                    }
                    if (!po) continue;
                    // Always absolute GPU sync for Shift+S settle (no leftover translates).
                    sync_object_gpu(*po);
                    upsert_transient_object(*po);
                    mark_tile(po->position.x, po->position.y);
                    for (auto& sp : scene_places) {
                        if (sp.object_id == po->id) sp.pinned = false;
                    }
                } else {
                    map::SplineInst* ps = nullptr;
                    for (auto& s : edit_splines) {
                        if (s.id == item.id) {
                            ps = &s;
                            break;
                        }
                    }
                    if (!ps) continue;
                    // Always remesh on settle so next Shift+S G starts with identity offset.
                    remesh_spline(*ps);
                    upsert_transient_spline(*ps);
                    mark_tile(ps->position.x, ps->position.y);
                    if (auto* g = find_spline_gpu(ps->id)) g->pinned = false;
                }
                const double dt =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                grp_fb.charge(dt, c);
                gov.learn(omsi::LoadGovernor::Kind::Generic, c.bytes, dt);
                if (!grp_fb.time_ok()) break;
            }
            panel.set_status("Asentando grupo " + std::to_string(group_settle_i) + "/" +
                             std::to_string(group_settle_items.size()) +
                             (group_settle_remesh ? " (remesh)" : " (matriz)"));
            if (group_settle_i < group_settle_items.size()) return;

            // Snapshot dirty tiles on main, flush in parallel on a worker (MSVC PSTL / thread pool).
            std::vector<map::MapTileData> snaps;
            snaps.reserve(group_settle_dirty.size());
            for (auto& tile : world.tiles_mut()) {
                if (!group_settle_dirty.count(tile_key(tile.grid_x, tile.grid_y))) continue;
                sync_tile_lists_only(tile);
                snaps.push_back(tile);
            }
            group_settle_tile_n = snaps.size();
            group_settle_persist_done.store(false);
            group_settle_phase = GroupSettlePhase::Persisting;
            const auto omsi_root = cfg.omsi_root;
            group_settle_persist_thread =
                std::thread([snaps = std::move(snaps), omsi_root, ws = cfg.workspace,
                             done = &group_settle_persist_done]() mutable {
                    try {
                        std::for_each(std::execution::par, snaps.begin(), snaps.end(),
                                      [&](map::MapTileData& t) {
                                          try {
                                          std::filesystem::path write_path = t.path;
                                          std::filesystem::path read_fb = t.path;
                                          if (ws && !ws->actual_dir.empty()) {
                                              write_path = map::workspace_overlay_write_path(*ws, t.path);
                                              if (write_path.empty()) {
                                                  log::warn("async persist refused " + t.path.string());
                                                  return;
                                              }
                                              read_fb = map::workspace_tile_map_for_read(&*ws, t.path);
                                          }
                                          if (!map::write_tile_map_file(write_path, t, omsi_root, read_fb)) {
                                              log::warn("async persist tile failed " + write_path.string());
                                          }
                                          } catch (const std::exception& ex) {
                                              log::warn(std::string("group settle tile: ") + ex.what());
                                          } catch (...) {
                                              log::warn("group settle tile: unknown");
                                          }
                                      });
                    } catch (const std::exception& ex) {
                        log::warn(std::string("group settle persist: ") + ex.what());
                    } catch (...) {
                        log::warn("group settle persist: unknown");
                    }
                    done->store(true);
                });
            panel.set_status("Guardando " + std::to_string(group_settle_tile_n) +
                             " tiles en paralelo (CPU)...");
            return;
        }

        if (group_settle_phase == GroupSettlePhase::Persisting) {
            if (!group_settle_persist_done.load()) {
                panel.set_status("Guardando " + std::to_string(group_settle_tile_n) +
                                 " tiles (bg, par)...");
                return;
            }
            if (group_settle_persist_thread.joinable()) group_settle_persist_thread.join();
            const auto place_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - group_settle_t0)
                                      .count();
            const std::size_t placed_n = group_settle_items.size();
            panel.set_status("Grupo asentado " + std::to_string(placed_n) + " en " +
                             std::to_string(place_ms) +
                             "ms async (sigue seleccionado; click vacio/Esc=soltar)");
            log::info("editor group settle count=" + std::to_string(placed_n) +
                      " ms=" + std::to_string(place_ms) + " tiles=" +
                      std::to_string(group_settle_tile_n) +
                      (group_settle_remesh ? " remesh" : " matrix_only"));
            editor_log::event("SHIFT_S_GROUP_PLACE",
                              "count=" + std::to_string(placed_n) + " ms=" +
                                  std::to_string(place_ms) + " tiles=" +
                                  std::to_string(group_settle_tile_n) +
                                  (group_settle_remesh ? " remesh" : " matrix_only") +
                                  " async=1 obj=" + std::to_string(group_settle_obj_n) + " spl=" +
                                  std::to_string(group_settle_spl_n) + " | " + session_state());
            group_settle_items.clear();
            group_settle_dirty.clear();
            group_settle_phase = GroupSettlePhase::Idle;
        }
    };

