// Aerosol / scatter painter.
// Circle / square: hold LMB to stream objects. Lasso: draw an area, objects are placed on release.
// Either way the last batch stays selected as a group (move / rotate / delete) and editing the spray
// settings regenerates it live: positions are computed on a worker thread, objects are spawned in a
// per-frame budget so the UI never stalls.
{
    using SprayShape = ui::EditorSidePanelImGuiState::SprayShapeUi;
    struct SprayPos {
        float x = 0.f, y = 0.f, yaw = 0.f;
        std::filesystem::path path;
    };
    struct SprayBatch {
        bool valid = false;
        int shape = 0;
        std::vector<std::pair<float, float>> region; // lasso polygon, or the stamp centres of a stroke
        std::vector<std::int32_t> ids;               // objects of the batch (same order as the group selection)
        std::uint32_t seed = 1;
        float first_x = 0.f, first_y = 0.f;
        float sig[8]{};
    };
    struct SprayJobIn {
        std::vector<std::pair<float, float>> region;
        int shape = 0;
        float radius = 10.f, spacing = 2.f, yaw = 0.f;
        int definition = 28, max_count = 100;
        bool random_rot = false;
        std::uint32_t seed = 1;
        std::vector<std::pair<float, float>> existing;
        std::vector<std::pair<std::filesystem::path, float>> pool;
    };
    static SprayBatch batch;
    static std::future<std::vector<SprayPos>> gen_fut;
    static bool gen_busy = false;
    static std::vector<SprayPos> spawn_q;
    static std::size_t spawn_i = 0;
    static bool spawning = false;
    static std::vector<std::int32_t> spawn_ids;
    static std::vector<std::int32_t> stroke_ids;
    static std::vector<std::pair<float, float>> stroke_stamps;
    static std::uint32_t stroke_seed0 = 1;
    static float regen_wait = -1.f;

    auto spray_sig = [&](float* out) {
        auto& s = panel.imgui_state();
        float total_w = 0.f;
        int n = 0;
        for (const auto& c : s.spray_categories)
            if (c.enabled)
                for (const auto& it : c.items)
                    if (it.enabled && !it.path.empty()) { total_w += it.weight; ++n; }
        out[0] = s.spray_radius;
        out[1] = s.spray_spacing;
        out[2] = static_cast<float>(s.spray_definition);
        out[3] = static_cast<float>(s.spray_max_count);
        out[4] = s.spray_yaw_deg;
        out[5] = s.spray_random_rotation ? 1.f : 0.f;
        out[6] = static_cast<float>(n) + total_w * 0.001f;
        out[7] = static_cast<float>(static_cast<int>(s.spray_shape));
    };
    auto spray_pool = [&]() {
        std::vector<std::pair<std::filesystem::path, float>> pool;
        for (const auto& c : panel.imgui_state().spray_categories) {
            if (!c.enabled) continue;
            for (const auto& it : c.items)
                if (it.enabled && !it.path.empty() && it.path.extension() == ".sco")
                    pool.emplace_back(it.path, (std::max)(0.01f, it.weight));
        }
        return pool;
    };
    // Pure function (runs on a worker thread): candidate positions for one batch.
    auto spray_generate = [](SprayJobIn in) -> std::vector<SprayPos> {
        std::vector<SprayPos> out;
        if (in.pool.empty()) return out;
        std::mt19937 rng(in.seed);
        std::uniform_real_distribution<float> unit(0.f, 1.f);
        float total = 0.f;
        for (const auto& p : in.pool) total += p.second;
        const float cell = (std::max)(0.5f, in.spacing);
        const float min_d2 = in.spacing * in.spacing;
        std::unordered_map<long long, std::vector<std::pair<float, float>>> grid;
        auto key = [&](float x, float y) {
            return (static_cast<long long>(std::floor(x / cell)) << 32) ^
                   (static_cast<long long>(std::floor(y / cell)) & 0xffffffffLL);
        };
        auto add = [&](float x, float y) { grid[key(x, y)].emplace_back(x, y); };
        auto blocked = [&](float x, float y) {
            if (min_d2 <= 0.f) return false;
            const int cx = static_cast<int>(std::floor(x / cell)), cy = static_cast<int>(std::floor(y / cell));
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto it = grid.find((static_cast<long long>(cx + dx) << 32) ^
                                              (static_cast<long long>(cy + dy) & 0xffffffffLL));
                    if (it == grid.end()) continue;
                    for (const auto& q : it->second) {
                        const float ddx = q.first - x, ddy = q.second - y;
                        if (ddx * ddx + ddy * ddy < min_d2) return true;
                    }
                }
            return false;
        };
        for (const auto& e : in.existing) add(e.first, e.second);
        auto pick_item = [&]() -> const std::filesystem::path& {
            float v = unit(rng) * total;
            for (const auto& p : in.pool) {
                v -= p.second;
                if (v <= 0.f) return p.first;
            }
            return in.pool.back().first;
        };
        auto push = [&](float x, float y) {
            SprayPos sp;
            sp.x = x;
            sp.y = y;
            sp.yaw = omsi::editor::spray_place_yaw_deg(in.random_rot, in.yaw, unit(rng));
            sp.path = pick_item();
            out.push_back(std::move(sp));
            add(x, y);
        };
        if (in.shape == static_cast<int>(SprayShape::Lasso)) {
            const auto& poly = in.region;
            if (poly.size() < 3) return out;
            float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
            for (const auto& p : poly) {
                mnx = (std::min)(mnx, p.first); mxx = (std::max)(mxx, p.first);
                mny = (std::min)(mny, p.second); mxy = (std::max)(mxy, p.second);
            }
            auto inside = [&](float px, float py) {
                bool in_ = false;
                for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                    const auto& a = poly[i];
                    const auto& b = poly[j];
                    if (((a.second > py) != (b.second > py)) &&
                        px < (b.first - a.first) * (py - a.second) / ((b.second - a.second) + 1.e-6f) + a.first)
                        in_ = !in_;
                }
                return in_;
            };
            const int desired = (std::clamp)(in.max_count > 0 ? in.max_count : in.definition * 8, 1, 20000);
            for (int attempt = 0; static_cast<int>(out.size()) < desired && attempt < desired * 20; ++attempt) {
                const float x = mnx + unit(rng) * (mxx - mnx), y = mny + unit(rng) * (mxy - mny);
                if (!inside(x, y) || blocked(x, y)) continue;
                push(x, y);
            }
        } else {
            const int tries = (std::max)(4, in.definition);
            for (const auto& c : in.region) {
                if (in.max_count > 0 && static_cast<int>(out.size()) >= in.max_count) break;
                for (int a = 0; a < tries; ++a) {
                    float x, y;
                    if (in.shape == static_cast<int>(SprayShape::Circle)) {
                        const float ang = unit(rng) * 6.2831853f, r = std::sqrt(unit(rng)) * in.radius;
                        x = c.first + std::cos(ang) * r;
                        y = c.second + std::sin(ang) * r;
                    } else {
                        x = c.first + (unit(rng) * 2.f - 1.f) * in.radius;
                        y = c.second + (unit(rng) * 2.f - 1.f) * in.radius;
                    }
                    if (blocked(x, y)) continue;
                    push(x, y);
                    break;
                }
            }
        }
        return out;
    };
    auto spray_start_job = [&](const std::unordered_set<std::int32_t>& exclude_ids) {
        auto& s = panel.imgui_state();
        SprayJobIn in;
        in.region = batch.region;
        in.shape = batch.shape;
        in.radius = s.spray_radius;
        in.spacing = s.spray_spacing;
        in.yaw = s.spray_yaw_deg;
        in.definition = s.spray_definition;
        in.max_count = s.spray_max_count;
        in.random_rot = s.spray_random_rotation;
        in.seed = batch.seed;
        in.pool = spray_pool();
        in.existing.reserve(edit_objects.size());
        for (const auto& o : edit_objects)
            if (!exclude_ids.count(o.id)) in.existing.emplace_back(o.position.x, o.position.y);
        gen_fut = std::async(std::launch::async, spray_generate, std::move(in));
        gen_busy = true;
    };
    auto spray_remove_ids = [&](const std::vector<std::int32_t>& ids) {
        if (ids.empty()) return;
        std::unordered_set<std::int32_t> dead(ids.begin(), ids.end());
        for (const auto oid : ids) {
            mark_editor_object_deleted(oid);
            purge_path_rails_for_owner(oid, true);
        }
        for (auto& sp_ : scene_places) {
            if (!sp_.alive || !dead.count(sp_.object_id)) continue;
            sp_.alive = false;
            if (!sp_.is_tree) renderer.set_chunk_visible(sp_.chunk, false);
        }
        for (auto& mk : end_markers)
            if (mk.from_object && dead.count(mk.owner_id)) {
                mk.alive = false;
                renderer.set_chunk_visible(mk.chunk, false);
            }
        edit_objects.erase(std::remove_if(edit_objects.begin(), edit_objects.end(),
                                          [&](const map::MapObjectInst& o) { return dead.count(o.id) != 0; }),
                           edit_objects.end());
        transient_objects.erase(std::remove_if(transient_objects.begin(), transient_objects.end(),
                                               [&](const map::MapObjectInst& o) { return dead.count(o.id) != 0; }),
                                transient_objects.end());
        group_sel_obj.clear();
        group_sel_obj_ids.clear();
        selected_object = -1;
    };
    auto spray_select_batch = [&]() {
        std::unordered_set<std::int32_t> want(batch.ids.begin(), batch.ids.end());
        group_sel_obj.clear();
        group_sel_obj_ids.clear();
        group_sel_spl.clear();
        group_sel_spl_ids.clear();
        for (std::size_t i = 0; i < edit_objects.size(); ++i)
            if (want.count(edit_objects[i].id)) {
                group_sel_obj.push_back(static_cast<int>(i));
                group_sel_obj_ids.push_back(edit_objects[i].id);
            }
        batch.ids = group_sel_obj_ids;
        selected_object = group_sel_obj.empty() ? -1 : group_sel_obj.back();
        selected_spline = -1;
        if (!group_sel_obj.empty()) {
            const auto& f = edit_objects[static_cast<std::size_t>(group_sel_obj.front())];
            batch.first_x = f.position.x;
            batch.first_y = f.position.y;
        }
        spray_sig(batch.sig);
        refresh_editor_lists();
        for (auto& tile : world.tiles_mut()) mark_tile_dirty(tile.grid_x, tile.grid_y);
    };
    auto spray_spawn_pos = [&](const SprayPos& p) {
        map::MapObjectInst placed{};
        placed.id = next_object_id();
        placed.sco_path = p.path;
        placed.position.x = p.x;
        placed.position.y = p.y;
        const auto zh = sample_terrain_editor_xy_opt(p.x, p.y);
        placed.position.z = zh ? *zh : camera.target_z;
        placed.rotation_deg.x = p.yaw;
        fill_sco_map_extras(placed);
        edit_objects.push_back(placed);
        transient_objects.push_back(placed);
        spawn_editor_object_gpu(edit_objects.back());
        sync_object_gpu(edit_objects.back());
        spawn_ids.push_back(placed.id);
    };

    // ── live batch maintenance (runs even with the pointer over the UI) ─────────────────────────────
    if (!view_only) {
        const float dt = ImGui::GetIO().DeltaTime;
        if (gen_busy && gen_fut.valid() && gen_fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            spawn_q = gen_fut.get();
            gen_busy = false;
            spray_remove_ids(batch.ids); // previous version of the batch (empty on the first pass)
            batch.ids.clear();
            spawn_ids.clear();
            spawn_i = 0;
            spawning = true;
        }
        if (spawning) {
            const auto t0 = std::chrono::steady_clock::now();
            int n = 0;
            while (spawn_i < spawn_q.size() && n < 400) {
                spray_spawn_pos(spawn_q[spawn_i++]);
                ++n;
                if ((n & 7) == 0 &&
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 5.0)
                    break;
            }
            if (spawn_i >= spawn_q.size()) {
                spawning = false;
                batch.ids = spawn_ids;
                spray_select_batch();
                panel.set_status("Aerosol: " + std::to_string(batch.ids.size()) +
                                 " objetos (seleccionados - ajustá la zona/cantidad para regenerar, G mover, Supr borrar)");
                spawn_q.clear();
            }
        }
        if (batch.valid && !gen_busy && !spawning) {
            // The batch is only live while it is still exactly the selected group and was not moved.
            bool ok = !batch.ids.empty() && group_sel_obj_ids == batch.ids;
            if (ok && !group_sel_obj.empty()) {
                const auto fi = static_cast<std::size_t>(group_sel_obj.front());
                if (fi < edit_objects.size() &&
                    (std::fabs(edit_objects[fi].position.x - batch.first_x) > 0.01f ||
                     std::fabs(edit_objects[fi].position.y - batch.first_y) > 0.01f))
                    ok = false;
            }
            if (!ok) {
                batch.valid = false;
                regen_wait = -1.f;
            } else {
                float cur[8];
                spray_sig(cur);
                bool changed = false;
                for (int i = 0; i < 8; ++i) changed |= std::fabs(cur[i] - batch.sig[i]) > 1.e-4f;
                if (changed) {
                    if (regen_wait < 0.f) regen_wait = 0.18f; // debounce while dragging a slider
                    regen_wait -= dt;
                    if (regen_wait <= 0.f) {
                        regen_wait = -1.f;
                        const std::unordered_set<std::int32_t> mine(batch.ids.begin(), batch.ids.end());
                        spray_start_job(mine);
                        spray_sig(batch.sig);
                    }
                } else {
                    regen_wait = -1.f;
                }
            }
        }
    }

    spray_owns_lmb = panel.imgui_state().spray_enabled && !panel.imgui_state().spray_pick_mode && !view_only &&
                     edit_mode == EditMode::None;

    auto spray_spawn_one = [&]() -> bool {
        auto& sp = panel.imgui_state();
        const auto pool = spray_pool();
        if (pool.empty()) return false;
        float mx = 0, my = 0, fw = 1, fh = 1;
        input::cursor_to_framebuffer(window, mx, my, fw, fh);
        camera.ground_z = camera.target_z;
        auto hit = camera.hit_ground_xy(mx, my, fw, fh);
        if (!hit) return false;
        SprayJobIn in;
        in.region = {{hit->x, hit->y}};
        in.shape = static_cast<int>(sp.spray_shape);
        in.radius = sp.spray_radius;
        in.spacing = sp.spray_spacing;
        in.yaw = sp.spray_yaw_deg;
        in.definition = sp.spray_definition;
        in.max_count = 0;
        in.random_rot = sp.spray_random_rotation;
        in.seed = ++sp.spray_seed;
        in.pool = pool;
        if (sp.spray_spacing > 0.f) {
            for (const auto& o : edit_objects)
                if (std::fabs(o.position.x - hit->x) < sp.spray_radius + sp.spray_spacing &&
                    std::fabs(o.position.y - hit->y) < sp.spray_radius + sp.spray_spacing)
                    in.existing.emplace_back(o.position.x, o.position.y);
        }
        const auto res = spray_generate(std::move(in));
        if (res.empty()) return false;
        spawn_ids.clear();
        spray_spawn_pos(res.front());
        stroke_ids.push_back(spawn_ids.front());
        stroke_stamps.emplace_back(hit->x, hit->y);
        const auto& placed = edit_objects.back();
        fire_place_asmr(editor::PlaceFxKind::PlaceBounce, placed.position.x, placed.position.y, placed.position.z, 2.f,
                        placed.id, editor::SfxCue::StonePlace);
        sp.spray_stroke_points.push_back({placed.position.x, placed.position.y});
        ++sp.spray_placed_total;
        ++sp.spray_stroke_object_count;
        return true;
    };
    if (panel.imgui_state().spray_pick_mode && !over_ui && !view_only) {
        static bool pick_was = false;
        const bool pick_down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (pick_down && !pick_was) {
            float px, py, pfw, pfh;
            input::cursor_to_framebuffer(window, px, py, pfw, pfh);
            const float asp = (pfh > 0.f) ? pfw / pfh : 16.f / 9.f;
            float wvp[16]{};
            camera.build_world_view_proj(asp, wvp);
            const int pi = pick_object_index(px, py, pfw, pfh, wvp);
            if (pi >= 0 && static_cast<std::size_t>(pi) < edit_objects.size() &&
                !edit_objects[static_cast<std::size_t>(pi)].sco_path.empty()) {
                auto& sp = panel.imgui_state();
                auto& cat = sp.spray_categories.empty()
                                ? sp.spray_categories.emplace_back(
                                      ui::EditorSidePanelImGuiState::SprayCategory{"Pick", {}, true, false})
                                : sp.spray_categories[static_cast<std::size_t>(
                                      std::clamp(sp.spray_active_category, 0,
                                                 static_cast<int>(sp.spray_categories.size()) - 1))];
                const auto path = edit_objects[static_cast<std::size_t>(pi)].sco_path;
                bool exists = false;
                for (const auto& it : cat.items)
                    if (it.path == path) exists = true;
                if (!exists) cat.items.push_back({path, path.filename().string(), true, 1.f});
                ui::save_editor_docks(sp);
                // Pick stays on until the user toggles it off.
                panel.set_status("Aerosol: agregado " + path.filename().string() + " (pick activo)");
                log::info("SPRAY_PICK_3D " + path.string());
            } else
                panel.set_status("Aerosol: no se encontró un SCO bajo el cursor");
        }
        pick_was = pick_down;
    }
    // Shape overlay (circle / square draped on the terrain, lasso polyline) while aerosol is armed.
    if (panel.imgui_state().spray_enabled && !panel.imgui_state().spray_pick_mode && !over_ui && !view_only) {
        auto& sp = panel.imgui_state();
        float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
        input::cursor_to_framebuffer(window, mx, my, fw, fh);
        camera.ground_z = camera.target_z;
        if (auto hit = camera.hit_ground_xy(mx, my, fw, fh)) {
            if (sp.spray_shape == SprayShape::Circle)
                draw_ground_ring(hit->x, hit->y, sp.spray_radius, false, IM_COL32(80, 200, 255, 235),
                                 IM_COL32(80, 200, 255, 30));
            else if (sp.spray_shape == SprayShape::Square)
                draw_ground_ring(hit->x, hit->y, sp.spray_radius, true, IM_COL32(80, 200, 255, 235),
                                 IM_COL32(80, 200, 255, 30));
            else if (!sp.spray_painting)
                draw_ground_ring(hit->x, hit->y, 0.6f, false, IM_COL32(80, 200, 255, 235), IM_COL32(80, 200, 255, 90));
        }
        if (sp.spray_shape == SprayShape::Lasso && sp.spray_lasso_points.size() >= 2) {
            const float aspect = (fh > 0.f) ? fw / fh : 16.f / 9.f;
            float wvp[16]{};
            camera.build_world_view_proj(aspect, wvp);
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const float kx = disp.x / (std::max)(1.f, fw), ky = disp.y / (std::max)(1.f, fh);
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            std::vector<ImVec2> pts;
            pts.reserve(sp.spray_lasso_points.size());
            for (const auto& p : sp.spray_lasso_points) {
                float sx = 0.f, sy = 0.f;
                if (world_to_screen(p.x, p.y, camera.ground_z + 0.05f, 0.f, 0.f, fw, fh, wvp, sx, sy))
                    pts.emplace_back(sx * kx, sy * ky);
            }
            if (pts.size() >= 2) {
                if (pts.size() >= 3) fg->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), IM_COL32(80, 200, 255, 22));
                fg->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(0, 0, 0, 150), ImDrawFlags_None, 4.f);
                fg->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(80, 200, 255, 245), ImDrawFlags_None, 2.2f);
                fg->AddLine(pts.back(), pts.front(), IM_COL32(80, 200, 255, 140), 1.4f); // closing edge
            }
        }
    }
    if (spray_owns_lmb && !over_ui) {
        auto& sp = panel.imgui_state();
        const bool down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        static bool was = false;
        const bool is_lasso = sp.spray_shape == SprayShape::Lasso;
        if (down && !was && !gen_busy && !spawning) {
            sp.spray_painting = true;
            sp.spray_stroke_active = true;
            sp.spray_stroke_points.clear();
            sp.spray_lasso_points.clear();
            sp.spray_stroke_object_count = 0;
            sp.spray_accumulator = 0.f;
            stroke_ids.clear();
            stroke_stamps.clear();
            stroke_seed0 = sp.spray_seed;
            batch.valid = false;
            regen_wait = -1.f;
            push_undo("spray_stroke");
            float mx, my, fw, fh;
            input::cursor_to_framebuffer(window, mx, my, fw, fh);
            camera.ground_z = camera.target_z;
            if (is_lasso) {
                if (auto h = camera.hit_ground_xy(mx, my, fw, fh)) sp.spray_lasso_points.push_back({h->x, h->y});
            } else {
                spray_spawn_one();
            }
        }
        if (down && sp.spray_painting && is_lasso) {
            float mx, my, fw, fh;
            input::cursor_to_framebuffer(window, mx, my, fw, fh);
            camera.ground_z = camera.target_z;
            if (auto h = camera.hit_ground_xy(mx, my, fw, fh)) {
                if (sp.spray_lasso_points.empty() ||
                    std::hypot(h->x - sp.spray_lasso_points.back().x, h->y - sp.spray_lasso_points.back().y) > 0.15f)
                    sp.spray_lasso_points.push_back({h->x, h->y});
            }
        }
        if (down && sp.spray_painting && !is_lasso) {
            sp.spray_accumulator += (std::max)(0.f, ImGui::GetIO().DeltaTime * sp.spray_speed);
            const int n = static_cast<int>(sp.spray_accumulator);
            sp.spray_accumulator -= static_cast<float>(n);
            for (int i = 0; i < n; ++i) {
                if (sp.spray_max_count > 0 && static_cast<int>(sp.spray_stroke_object_count) >= sp.spray_max_count) {
                    sp.spray_painting = false;
                    break;
                }
                if (!spray_spawn_one() && sp.spray_stop_no_fit) break;
            }
        }
        if (!down && was && (sp.spray_painting || sp.spray_stroke_active)) {
            if (is_lasso) {
                if (sp.spray_lasso_points.size() >= 3) {
                    batch.valid = true;
                    batch.shape = static_cast<int>(SprayShape::Lasso);
                    batch.region.clear();
                    for (const auto& p : sp.spray_lasso_points) batch.region.emplace_back(p.x, p.y);
                    batch.seed = ++sp.spray_seed;
                    batch.ids.clear();
                    spray_sig(batch.sig);
                    spray_start_job({});
                    panel.set_status("Aerosol lazo: generando…");
                    log::info("SPRAY_LASSO_COMMIT points=" + std::to_string(sp.spray_lasso_points.size()));
                } else {
                    panel.set_status("Aerosol lazo: dibujá un área (mantené el click y arrastrá)");
                }
            } else if (!stroke_ids.empty()) {
                batch.valid = true;
                batch.shape = static_cast<int>(sp.spray_shape);
                batch.region = stroke_stamps;
                batch.ids = stroke_ids;
                batch.seed = stroke_seed0 + 1;
                spray_select_batch();
                panel.set_status("Aerosol: " + std::to_string(batch.ids.size()) +
                                 " objetos seleccionados (ajustá zona/cantidad para regenerar, G mover, Supr borrar)");
            }
            sp.spray_painting = false;
            sp.spray_stroke_active = false;
            sp.spray_lasso_points.clear();
        }
        was = down;
    }
}
