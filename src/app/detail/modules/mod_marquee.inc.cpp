// MOD: marquee — aislado (select/clone box). Usa el motor (spawn/remesh/GPU) compartido.
    auto recreate_marquee_from_blueprint = [&]() {
        if (marquee.phase != MarqueePhase::Placing) return;
        // Rebuild GPU from remembered ids/poses only â€” ignore other tile objects.
        marquee.track_obj.clear();
        marquee.track_spl.clear();
        for (std::size_t bi = 0; bi < marquee.track_obj_ids.size(); ++bi) {
            const std::int32_t id = marquee.track_obj_ids[bi];
            int idx = -1;
            for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                if (edit_objects[i].id == id) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
            if (idx < 0 && bi < marquee.base_obj.size()) {
                auto c = marquee.base_obj[bi];
                c.id = id;
                edit_objects.push_back(c);
                upsert_transient_object(c);
                idx = static_cast<int>(edit_objects.size() - 1);
            }
            if (idx < 0) continue;
            if (bi < marquee.base_obj.size()) {
                const auto& b = marquee.base_obj[bi];
                auto& o = edit_objects[static_cast<std::size_t>(idx)];
                o.position = b.position;
                o.rotation_deg = b.rotation_deg;
                o.sco_path = b.sco_path;
                o.obj_type = b.obj_type;
                o.tree_height = b.tree_height;
                o.tree_aspect = b.tree_aspect;
                o.tree_texture = b.tree_texture;
            }
            spawn_editor_object_gpu(edit_objects[static_cast<std::size_t>(idx)]);
            marquee.track_obj.push_back(idx);
        }
        for (std::size_t bi = 0; bi < marquee.track_spl_ids.size(); ++bi) {
            const std::int32_t id = marquee.track_spl_ids[bi];
            int idx = -1;
            for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                if (edit_splines[i].id == id) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
            if (idx < 0 && bi < marquee.base_spl.size()) {
                auto c = marquee.base_spl[bi];
                c.id = id;
                remesh_spline(c);
                edit_splines.push_back(c);
                upsert_transient_spline(c);
                idx = static_cast<int>(edit_splines.size() - 1);
            }
            if (idx < 0) continue;
            auto& s = edit_splines[static_cast<std::size_t>(idx)];
            if (bi < marquee.base_spl.size()) {
                const auto& b = marquee.base_spl[bi];
                s.position = b.position;
                s.heading_deg = b.heading_deg;
                s.length = b.length;
                s.radius = b.radius;
                s.sli_path = b.sli_path;
            }
            remesh_spline(s);
            if (auto* g = find_spline_gpu(s.id)) {
                g->pinned = true;
                g->tile_gx = tile_gx;
                g->tile_gy = tile_gy;
            }
            marquee.track_spl.push_back(idx);
        }
        // Keep base = absolute pose; PrepShift already shifted base+anchor together.
        marquee.base_obj.clear();
        marquee.base_spl.clear();
        for (int i : marquee.track_obj) marquee.base_obj.push_back(edit_objects[static_cast<std::size_t>(i)]);
        for (int i : marquee.track_spl) marquee.base_spl.push_back(edit_splines[static_cast<std::size_t>(i)]);
        panel.set_status("Marquee clone: recreado tras cambio de tile (LMB=place)");
        log::info("editor marquee recreated after stream obj=" + std::to_string(marquee.track_obj.size()) +
                  " spl=" + std::to_string(marquee.track_spl.size()));
    };
    // Circle brush: apply under one screen-space center without finishing the mode.
    // Selection circle (screen centre, radius sel_circle_r px): what it touches. An object counts when the
    // circle overlaps the screen rectangle of its bounding box, a spline when it reaches the road (centre
    // line + half width). Only what is drawn (alive, not hidden) counts. Screen rectangles are cached
    // while the camera does not move, so painting stays cheap on big maps.
    struct CircleScreenCache {
        float wvp[16]{};
        std::size_t n_places = 0, n_splines = 0;
        double t = -1.0;
        std::vector<std::array<float, 4>> place_rect; // x0, y0, x1, y1 (x0 > x1: not on screen)
        std::vector<std::array<float, 4>> spl_rect;
    };
    auto circle_cache = std::make_shared<CircleScreenCache>();
    auto circle_hits = [&, circle_cache](float cx, float cy, float fw, float fh, const float* wvp_t,
                                         std::vector<int>& sel_o, std::vector<int>& sel_s,
                                         std::vector<std::size_t>* place_hits) {
        sel_o.clear();
        sel_s.clear();
        if (place_hits) place_hits->clear();
        const float cr = sel_circle_r;
        auto& cc = *circle_cache;
        const double now_t = glfwGetTime();
        if (std::memcmp(cc.wvp, wvp_t, sizeof(cc.wvp)) != 0 || cc.n_places != scene_places.size() ||
            cc.n_splines != edit_splines.size() || now_t - cc.t > 0.5) {
            std::memcpy(cc.wvp, wvp_t, sizeof(cc.wvp));
            cc.n_places = scene_places.size();
            cc.n_splines = edit_splines.size();
            cc.t = now_t;
            cc.place_rect.assign(scene_places.size(), {1.f, 0.f, 0.f, 0.f});
            for (std::size_t pi = 0; pi < scene_places.size(); ++pi) {
                const auto& sp = scene_places[pi];
                if (!sp.alive) continue;
                const float h = (std::max)(0.3f, sp.half_xy), hz = (std::max)(0.3f, sp.height);
                float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                int vis = 0;
                for (int c = 0; c < 8; ++c) {
                    const float wx = sp.tx + ((c & 1) ? h : -h), wy = sp.ty + ((c & 2) ? h : -h);
                    const float wz = sp.tz + ((c & 4) ? hz : 0.f);
                    float sx = 0.f, sy = 0.f;
                    if (!world_to_screen(wx, wy, wz, 0.f, 0.f, fw, fh, wvp_t, sx, sy)) continue;
                    ++vis;
                    x0 = (std::min)(x0, sx);
                    y0 = (std::min)(y0, sy);
                    x1 = (std::max)(x1, sx);
                    y1 = (std::max)(y1, sy);
                }
                if (vis >= 4) cc.place_rect[pi] = {x0, y0, x1, y1};
            }
            cc.spl_rect.assign(edit_splines.size(), {1.f, 0.f, 0.f, 0.f});
            for (std::size_t si = 0; si < edit_splines.size(); ++si) {
                const auto& sl = edit_splines[si];
                float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                int vis = 0;
                for (int k = 0; k <= 6; ++k) {
                    const auto smp = spline::evaluate_spline(sl, sl.length * static_cast<float>(k) / 6.f);
                    float sx = 0.f, sy = 0.f;
                    if (!world_to_screen(smp.x, smp.y, smp.z, 0.f, 0.f, fw, fh, wvp_t, sx, sy)) continue;
                    ++vis;
                    x0 = (std::min)(x0, sx);
                    y0 = (std::min)(y0, sy);
                    x1 = (std::max)(x1, sx);
                    y1 = (std::max)(y1, sy);
                }
                if (vis) cc.spl_rect[si] = {x0 - 80.f, y0 - 80.f, x1 + 80.f, y1 + 80.f}; // curve bulge + width
            }
        }
        auto circle_rect = [&](const std::array<float, 4>& r) {
            if (r[0] > r[2]) return false;
            const float qx = std::clamp(cx, r[0], r[2]), qy = std::clamp(cy, r[1], r[3]);
            return (qx - cx) * (qx - cx) + (qy - cy) * (qy - cy) <= cr * cr;
        };
        const auto tool = panel.current_tool();
        const bool want_o = tool != ui::EditorTool::Spline, want_s = tool != ui::EditorTool::Object;
        const auto& hid_o = panel.imgui_state().hidden_object_ids;
        const auto& hid_s = panel.imgui_state().hidden_spline_ids;
        if (want_o) {
            std::unordered_map<std::int32_t, int> idx;
            std::unordered_set<int> seen;
            for (std::size_t pi = 0; pi < scene_places.size() && pi < cc.place_rect.size(); ++pi) {
                const auto& sp = scene_places[pi];
                if (!sp.alive || hid_o.count(sp.object_id) || !circle_rect(cc.place_rect[pi])) continue;
                if (idx.empty()) {
                    idx.reserve(edit_objects.size());
                    for (std::size_t k = 0; k < edit_objects.size(); ++k) idx.emplace(edit_objects[k].id, static_cast<int>(k));
                }
                const auto it = idx.find(sp.object_id);
                if (it == idx.end()) continue;
                if (place_hits) place_hits->push_back(pi);
                if (seen.insert(it->second).second) sel_o.push_back(it->second);
            }
            // objects without a mesh (missing assets): the cube where they stand
            for (const auto& [k, m] : missing_assets) {
                if (m.spline) continue;
                float sx = 0.f, sy = 0.f;
                if (!world_to_screen(m.x, m.y, m.z + 1.f, 0.f, 0.f, fw, fh, wvp_t, sx, sy)) continue;
                if ((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy) > (cr + 12.f) * (cr + 12.f)) continue;
                for (std::size_t oi = 0; oi < edit_objects.size(); ++oi)
                    if (edit_objects[oi].id == m.id) {
                        if (seen.insert(static_cast<int>(oi)).second) sel_o.push_back(static_cast<int>(oi));
                        break;
                    }
            }
        }
        if (want_s) {
            for (std::size_t si = 0; si < edit_splines.size() && si < cc.spl_rect.size(); ++si) {
                const auto& sl = edit_splines[si];
                if (hid_s.count(sl.id) || !circle_rect(cc.spl_rect[si])) continue;
                const auto* g = find_spline_gpu(sl.id);
                if (!g || !g->alive) continue;
                // half road width in pixels at the middle of the spline
                float w_px = 6.f;
                {
                    const auto mid = spline::evaluate_spline(sl, sl.length * 0.5f);
                    float ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
                    if (world_to_screen(mid.x, mid.y, mid.z, 0.f, 0.f, fw, fh, wvp_t, ax, ay) &&
                        world_to_screen(mid.x + 2.5f, mid.y, mid.z, 0.f, 0.f, fw, fh, wvp_t, bx, by))
                        w_px = (std::clamp)(std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)), 3.f, 120.f);
                }
                const float rr = cr + w_px;
                const float step = (std::clamp)(sl.length / 48.f, 0.5f, 3.f);
                bool hit = false;
                float px = 0.f, py = 0.f;
                bool have_prev = false;
                for (float along = 0.f; along <= sl.length + 1.e-3f && !hit; along += step) {
                    const auto smp = spline::evaluate_spline(sl, (std::min)(along, sl.length));
                    float sx = 0.f, sy = 0.f;
                    if (!world_to_screen(smp.x, smp.y, smp.z, 0.f, 0.f, fw, fh, wvp_t, sx, sy)) {
                        have_prev = false;
                        continue;
                    }
                    // distance from the circle centre to the screen segment prev -> this sample
                    float d2 = (sx - cx) * (sx - cx) + (sy - cy) * (sy - cy);
                    if (have_prev) {
                        const float ex = sx - px, ey = sy - py, l2 = ex * ex + ey * ey;
                        if (l2 > 1e-6f) {
                            const float t = std::clamp(((cx - px) * ex + (cy - py) * ey) / l2, 0.f, 1.f);
                            const float qx = px + ex * t - cx, qy = py + ey * t - cy;
                            d2 = (std::min)(d2, qx * qx + qy * qy);
                        }
                    }
                    if (d2 <= rr * rr) hit = true;
                    px = sx;
                    py = sy;
                    have_prev = true;
                }
                if (hit) sel_s.push_back(static_cast<int>(si));
            }
        }
    };
    auto apply_circle_brush_at = [&](float cx, float cy, float fw, float fh, const float* wvp_t,
                                     const bool remove) {
        std::vector<int> sel_o, sel_s;
        circle_hits(cx, cy, fw, fh, wvp_t, sel_o, sel_s, nullptr);
        if (remove) {
            for (int i : sel_o) {
                if (i < 0 || static_cast<std::size_t>(i) >= edit_objects.size()) continue;
                const auto id = edit_objects[static_cast<std::size_t>(i)].id;
                group_sel_obj.erase(std::remove(group_sel_obj.begin(), group_sel_obj.end(), i), group_sel_obj.end());
                group_sel_obj_ids.erase(std::remove(group_sel_obj_ids.begin(), group_sel_obj_ids.end(), id),
                                        group_sel_obj_ids.end());
            }
            for (int i : sel_s) {
                if (i < 0 || static_cast<std::size_t>(i) >= edit_splines.size()) continue;
                const auto id = edit_splines[static_cast<std::size_t>(i)].id;
                group_sel_spl.erase(std::remove(group_sel_spl.begin(), group_sel_spl.end(), i), group_sel_spl.end());
                group_sel_spl_ids.erase(std::remove(group_sel_spl_ids.begin(), group_sel_spl_ids.end(), id),
                                        group_sel_spl_ids.end());
            }
        } else {
            auto add_o = [&](int i) {
                if (std::find(group_sel_obj.begin(), group_sel_obj.end(), i) != group_sel_obj.end()) return;
                group_sel_obj.push_back(i);
                if (i >= 0 && static_cast<std::size_t>(i) < edit_objects.size())
                    group_sel_obj_ids.push_back(edit_objects[static_cast<std::size_t>(i)].id);
            };
            auto add_s = [&](int i) {
                if (std::find(group_sel_spl.begin(), group_sel_spl.end(), i) != group_sel_spl.end()) return;
                group_sel_spl.push_back(i);
                if (i >= 0 && static_cast<std::size_t>(i) < edit_splines.size())
                    group_sel_spl_ids.push_back(edit_splines[static_cast<std::size_t>(i)].id);
            };
            for (int i : sel_o) add_o(i);
            for (int i : sel_s) add_s(i);
        }
        if (!group_sel_obj.empty()) {
            selected_object = group_sel_obj.back();
            selected_spline = -1;
        } else if (!group_sel_spl.empty()) {
            selected_spline = group_sel_spl.back();
            selected_object = -1;
        }
    };
    auto finish_marquee_select = [&](float fw, float fh, const float* wvp_t) {
        const float minx = (std::min)(marquee.sx0, marquee.sx1);
        const float maxx = (std::max)(marquee.sx0, marquee.sx1);
        const float miny = (std::min)(marquee.sy0, marquee.sy1);
        const float maxy = (std::max)(marquee.sy0, marquee.sy1);
        const float cx = marquee.sx0;
        const float cy = marquee.sy0;
        // Circle is a fixed-radius brush. During a drag every sampled cursor point
        // contributes a circle, so the final selection follows the complete sweep.
        const float cr = sel_circle_r;
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
        auto in_shape = [&](float sx, float sy) {
            if (sel_shape == SelShapeMode::Circle) {
                if (sel_lasso_pts.empty()) {
                    const float dx = sx - cx, dy = sy - cy;
                    return dx * dx + dy * dy <= cr * cr;
                }
                for (const auto& p : sel_lasso_pts) {
                    const float dx = sx - p.x, dy = sy - p.y;
                    if (dx * dx + dy * dy <= cr * cr) return true;
                }
                return false;
            }
            if (sel_shape == SelShapeMode::Lasso) return point_in_lasso(sx, sy);
            return sx >= minx && sx <= maxx && sy >= miny && sy <= maxy;
        };
        std::vector<int> sel_o, sel_s;
        std::unordered_set<int> sel_o_seen;
        for (const auto& sp : scene_places) {
            if (!sp.alive) continue;
            float sx = 0.f, sy = 0.f;
            if (!world_to_screen(sp.tx, sp.ty, sp.tz, 0.f, 0.f, fw, fh, wvp_t, sx, sy)) continue;
            if (!in_shape(sx, sy)) continue;
            for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                if (edit_objects[i].id != sp.object_id) continue;
                const int idx = static_cast<int>(i);
                if (sel_o_seen.insert(idx).second) sel_o.push_back(idx);
                break;
            }
        }
        for (std::size_t i = 0; i < edit_objects.size(); ++i) {
            if (sel_o_seen.count(static_cast<int>(i)) != 0) continue;
            const auto& o = edit_objects[i];
            float sx = 0.f, sy = 0.f;
            const float oz = o.position.z + sample_terrain_editor_xy(o.position.x, o.position.y);
            if (!world_to_screen(o.position.x, o.position.y, oz, 0.f, 0.f, fw, fh, wvp_t, sx, sy)) continue;
            if (in_shape(sx, sy)) sel_o.push_back(static_cast<int>(i));
        }
        for (std::size_t i = 0; i < edit_splines.size(); ++i) {
            const auto& s = edit_splines[i];
            bool hit = false;
            const float step = (std::max)(1.f, s.length / 24.f);
            for (float along = 0.f; along <= s.length + 1.e-3f; along += step) {
                const auto samp = spline::evaluate_spline(s, along);
                float sx = 0.f, sy = 0.f;
                if (!world_to_screen(samp.x, samp.y, samp.z, 0.f, 0.f, fw, fh, wvp_t, sx, sy)) continue;
                if (in_shape(sx, sy)) {
                    hit = true;
                    break;
                }
            }
            if (hit) sel_s.push_back(static_cast<int>(i));
        }
        sel_lasso_pts.clear();
        sel_lasso_drawing = false;
        const bool shift_sel = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl_sel = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool additive =
            ctrl_sel || panel.imgui_state().spline_multi_pick || panel.imgui_state().object_multi_pick ||
            (sel_shape == SelShapeMode::Circle && !shift_sel);
        {
            const auto tool = panel.current_tool();
            if (tool == ui::EditorTool::Spline) {
                sel_o.clear();
            } else if (tool == ui::EditorTool::Object) {
                sel_s.clear();
            }
        }
        clear_marquee_box();
        marquee.dragging = false;
        // Keep Select tool armed until the user cancels — Recuadro/Lazo/Circulo stay connected.
        const bool keep_armed = (marquee.intent == MarqueeIntent::SelectOnly);
        if (sel_o.empty() && sel_s.empty() && !shift_sel && !additive) {
            marquee.phase = keep_armed ? MarqueePhase::Select : MarqueePhase::None;
            marquee.intent = MarqueeIntent::SelectOnly;
            panel.set_status("Selección: nada en el área");
            editor_log::event("SHIFT_S_EMPTY",
                              "rect=" +
                                  std::to_string(static_cast<int>(maxx - minx)) + "x" +
                                  std::to_string(static_cast<int>(maxy - miny)) + " | " +
                                  session_state());
            return;
        }
        if (marquee.intent == MarqueeIntent::SelectOnly) {
            marquee.phase = MarqueePhase::Select;
            marquee.intent = MarqueeIntent::SelectOnly;
            if (shift_sel) {
                for (int i : sel_o) {
                    if (i < 0 || static_cast<std::size_t>(i) >= edit_objects.size()) continue;
                    const auto id = edit_objects[static_cast<std::size_t>(i)].id;
                    group_sel_obj.erase(std::remove(group_sel_obj.begin(), group_sel_obj.end(), i),
                                        group_sel_obj.end());
                    group_sel_obj_ids.erase(std::remove(group_sel_obj_ids.begin(), group_sel_obj_ids.end(), id),
                                            group_sel_obj_ids.end());
                    for (auto& r : sel_id_rows)
                        if (r.kind == 0 && r.id == id) r.checked = false;
                }
                for (int i : sel_s) {
                    if (i < 0 || static_cast<std::size_t>(i) >= edit_splines.size()) continue;
                    const auto id = edit_splines[static_cast<std::size_t>(i)].id;
                    group_sel_spl.erase(std::remove(group_sel_spl.begin(), group_sel_spl.end(), i),
                                        group_sel_spl.end());
                    group_sel_spl_ids.erase(std::remove(group_sel_spl_ids.begin(), group_sel_spl_ids.end(), id),
                                            group_sel_spl_ids.end());
                    for (auto& r : sel_id_rows)
                        if (r.kind == 1 && r.id == id) r.checked = false;
                }
            } else if (additive) {
                auto add_o = [&](int i) {
                    if (std::find(group_sel_obj.begin(), group_sel_obj.end(), i) != group_sel_obj.end()) return;
                    group_sel_obj.push_back(i);
                    if (i >= 0 && static_cast<std::size_t>(i) < edit_objects.size())
                        group_sel_obj_ids.push_back(edit_objects[static_cast<std::size_t>(i)].id);
                };
                auto add_s = [&](int i) {
                    if (std::find(group_sel_spl.begin(), group_sel_spl.end(), i) != group_sel_spl.end()) return;
                    group_sel_spl.push_back(i);
                    if (i >= 0 && static_cast<std::size_t>(i) < edit_splines.size())
                        group_sel_spl_ids.push_back(edit_splines[static_cast<std::size_t>(i)].id);
                };
                for (int i : sel_o) add_o(i);
                for (int i : sel_s) add_s(i);
            } else {
            group_sel_obj = std::move(sel_o);
            group_sel_spl = std::move(sel_s);
            group_sel_obj_ids.clear();
            group_sel_spl_ids.clear();
            for (int i : group_sel_obj) {
                if (i >= 0 && static_cast<std::size_t>(i) < edit_objects.size())
                    group_sel_obj_ids.push_back(edit_objects[static_cast<std::size_t>(i)].id);
            }
            for (int i : group_sel_spl) {
                if (i >= 0 && static_cast<std::size_t>(i) < edit_splines.size())
                    group_sel_spl_ids.push_back(edit_splines[static_cast<std::size_t>(i)].id);
            }
            }
            selected_object = -1;
            selected_spline = -1;
            if (!group_sel_obj.empty()) {
                selected_object = group_sel_obj.front();
                show_object_at(static_cast<std::size_t>(selected_object));
            }
            if (group_sel_obj.empty() && !group_sel_spl.empty()) {
                selected_spline = group_sel_spl.front();
                show_spline_at(static_cast<std::size_t>(selected_spline));
            } else if (!group_sel_spl.empty()) {
                selected_spline = group_sel_spl.front();
            }
            panel.set_status("Grupo: " + std::to_string(group_sel_obj.size()) + " obj, " +
                             std::to_string(group_sel_spl.size()) + " spl (G=XY Z=altura R=rotar Ctrl=90)");
            sel_filter_open = true;
            sel_filter_mode = 0;
            if (!shift_sel && !additive) {
                sel_path_rows.clear();
                sel_id_rows.clear();
            }
            sel_filter_minimized = false;
            log::info("editor group select Shift+S obj=" + std::to_string(group_sel_obj.size()) + " spl=" +
                      std::to_string(group_sel_spl.size()));
            maybe_multi_spline_hint();
            editor_log::event("SHIFT_S_SELECT",
                              "rect=" +
                                  std::to_string(static_cast<int>(maxx - minx)) + "x" +
                                  std::to_string(static_cast<int>(maxy - miny)) + " obj=" +
                                  std::to_string(group_sel_obj.size()) + " spl=" +
                                  std::to_string(group_sel_spl.size()) + " | " + session_state());
            return;
        }
        push_undo("marquee_clone");
        {
            auto& bst = panel.imgui_state();
            bst.busy_overlay = true;
            bst.busy_spinner = true;
            bst.busy_title = omsi::editor::kBusyLoadTitle;
            bst.busy_detail = "Clonando Shift+D";
            bst.busy_progress = 0.f;
            pump_editor_busy();
        }
        marquee.track_spl.clear();
        marquee.track_obj_ids.clear();
        marquee.track_spl_ids.clear();
        marquee.base_obj.clear();
        marquee.base_spl.clear();
        float pivot_x = 0.f, pivot_y = 0.f;
        int pivot_n = 0;
        for (int i : sel_o) {
            const auto& o = edit_objects[static_cast<std::size_t>(i)];
            pivot_x += o.position.x;
            pivot_y += o.position.y;
            ++pivot_n;
        }
        for (int i : sel_s) {
            const auto& s = edit_splines[static_cast<std::size_t>(i)];
            pivot_x += s.position.x;
            pivot_y += s.position.y;
            ++pivot_n;
        }
        if (pivot_n > 0) {
            pivot_x /= static_cast<float>(pivot_n);
            pivot_y /= static_cast<float>(pivot_n);
        }
        marquee.anchor_x = pivot_x;
        marquee.anchor_y = pivot_y;
        const int clone_total = static_cast<int>(sel_o.size() + sel_s.size());
        int clone_done = 0;
        auto clone_busy_tick = [&](const char* kind) {
            ++clone_done;
            auto& bst = panel.imgui_state();
            bst.busy_overlay = true;
            bst.busy_spinner = true;
            bst.busy_title = omsi::editor::kBusyLoadTitle;
            bst.busy_detail = std::string("Clonando ") + kind + " " + std::to_string(clone_done) + "/" +
                              std::to_string((std::max)(1, clone_total));
            bst.busy_progress = clone_total > 0 ? static_cast<float>(clone_done) / static_cast<float>(clone_total)
                                                : 1.f;
            if ((clone_done % 3) == 0 || clone_done == clone_total) pump_editor_busy();
        };
        for (int i : sel_o) {
            auto c = edit_objects[static_cast<std::size_t>(i)];
            c.id = next_object_id();
            edit_objects.push_back(c);
            spawn_editor_object_gpu(edit_objects.back());
            upsert_transient_object(edit_objects.back());
            marquee.base_obj.push_back(c);
            marquee.track_obj.push_back(static_cast<int>(edit_objects.size() - 1));
            marquee.track_obj_ids.push_back(c.id);
            clone_busy_tick("objeto");
        }
        for (int i : sel_s) {
            auto c = edit_splines[static_cast<std::size_t>(i)];
            c.id = next_spline_id();
            c.prev_id = 0;
            c.next_id = 0;
            remesh_spline(c);
            if (auto* g = find_spline_gpu(c.id)) g->pinned = true;
            edit_splines.push_back(c);
            upsert_transient_spline(c);
            marquee.base_spl.push_back(c);
            marquee.track_spl.push_back(static_cast<int>(edit_splines.size() - 1));
            marquee.track_spl_ids.push_back(c.id);
            clone_busy_tick("spline");
        }
        refresh_editor_lists();
        scenery_scripts.sync(edit_objects, cfg.omsi_root);
        {
            auto& bst = panel.imgui_state();
            bst.busy_overlay = false;
            bst.busy_spinner = false;
            bst.busy_progress = -1.f;
            bst.busy_title.clear();
            bst.busy_detail.clear();
        }
        marquee.phase = MarqueePhase::Placing;
        panel.set_status("Marquee clone: move mouse, LMB=place, Esc=cancel");
    };
    auto update_marquee_placing = [&](float tx, float ty) {
        if (marquee.phase != MarqueePhase::Placing) return;
        const float dx = tx - marquee.anchor_x;
        const float dy = ty - marquee.anchor_y;
        for (std::size_t i = 0; i < marquee.track_obj.size() && i < marquee.base_obj.size(); ++i) {
            auto& o = edit_objects[static_cast<std::size_t>(marquee.track_obj[i])];
            o.position.x = marquee.base_obj[i].position.x + dx;
            o.position.y = marquee.base_obj[i].position.y + dy;
            o.position.z = marquee.base_obj[i].position.z;
            sync_object_gpu(o);
            upsert_transient_object(o);
        }
        for (std::size_t i = 0; i < marquee.track_spl.size() && i < marquee.base_spl.size(); ++i) {
            auto& s = edit_splines[static_cast<std::size_t>(marquee.track_spl[i])];
            s.position.x = marquee.base_spl[i].position.x + dx;
            s.position.y = marquee.base_spl[i].position.y + dy;
            s.position.z = marquee.base_spl[i].position.z;
            // Mesh was remeshed at base pose â€” translate by (dx,dy) only.
            set_spline_chunk_offset(s.id, dx, dy, 0.f);
            upsert_transient_spline(s);
        }
    };
    auto cancel_marquee = [&]() {
        if (marquee.phase == MarqueePhase::Placing && !undo_stack.empty()) {
            editor_log::event("MARQUEE_CANCEL", "restore snap | " + session_state());
            apply_editor_snapshot(undo_stack.back());
            pop_undo("marquee_cancel_restore");
        }
        clear_marquee_box();
        marquee = {};
        panel.set_status("Marquee cancelled");
    };
