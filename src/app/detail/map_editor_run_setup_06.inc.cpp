
    auto marker_pose = [&](const EndMarker& mk, float& ox, float& oy, float& oz, float& heading_deg) -> bool {
        if (!mk.alive || mk.from_object || mk.is_path) return false;
        for (const auto& s : edit_splines) {
            if (s.id != mk.owner_id) continue;
            const auto samp = spline::evaluate_spline(s, mk.is_start ? 0.f : s.length);
            ox = samp.x;
            oy = samp.y;
            oz = samp.z;
            heading_deg = samp.heading_rad * (180.f / 3.14159265f);
            return true;
        }
        return false;
    };

    auto pick_spline_end_marker = [&](float mx, float my, float fw, float fh, const float* wvp_t) -> int {
        if (panel.imgui_state().hide_nodes && !autofill_spline_ends) return -1; // Vista > Ocultar nodos
        int best = -1;
        float best_d2 = 22.f * 22.f;
        for (int i = 0; i < static_cast<int>(end_markers.size()); ++i) {
            const auto& mk = end_markers[static_cast<std::size_t>(i)];
            if (mk.is_path) continue;
            float wx = 0.f, wy = 0.f, wz = 0.f, wh = 0.f;
            if (!marker_pose(mk, wx, wy, wz, wh)) continue;
            wz += kArrowFloatZ;
            if (selected_spline >= 0 &&
                static_cast<std::size_t>(selected_spline) < edit_splines.size() &&
                mk.owner_id == edit_splines[static_cast<std::size_t>(selected_spline)].id)
                continue;
            DirectX::XMFLOAT4X4 wvp_t_m{};
            std::memcpy(&wvp_t_m, wvp_t, sizeof(wvp_t_m));
            const DirectX::XMMATRIX wvp = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&wvp_t_m));
            DirectX::XMFLOAT4 clip{};
            DirectX::XMStoreFloat4(&clip,
                                   DirectX::XMVector4Transform(DirectX::XMVectorSet(wx, wy, wz, 1.f), wvp));
            if (clip.w <= 1.e-5f) continue;
            const float sx = (clip.x / clip.w * 0.5f + 0.5f) * fw;
            const float sy = (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * fh;
            const float ddx = sx - mx;
            const float ddy = sy - my;
            const float d2 = ddx * ddx + ddy * ddy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = i;
            }
        }
        return best;
    };

    auto find_tile_path = [&](std::int32_t gx, std::int32_t gy) -> std::filesystem::path {
        auto try_resolve = [&](const std::filesystem::path& p) -> std::filesystem::path {
            if (p.empty()) return {};
            if (cfg.workspace) {
                const auto ws = map::workspace_tile_map_for_read(&*cfg.workspace, p);
                if (!ws.empty() && std::filesystem::exists(ws)) return ws;
            }
            if (std::filesystem::exists(p)) return p;
            return {};
        };
        for (const auto& t : world.tiles()) {
            if (t.grid_x == gx && t.grid_y == gy) {
                if (const auto r = try_resolve(t.path); !r.empty()) return r;
            }
        }
        for (const auto& ref : cfg.global.tiles) {
            if (ref.grid_x == gx && ref.grid_y == gy) {
                if (const auto r = try_resolve(ref.tile_path); !r.empty()) return r;
                return ref.tile_path;
            }
        }
        const auto logical = cfg.global.root / ("tile_" + std::to_string(gx) + "_" + std::to_string(gy) + ".map");
        if (const auto r = try_resolve(logical); !r.empty()) return r;
        return {};
    };

#include "detail/map_editor_run_mod_tile.inc.cpp"

    // ImGui panel (default): step buttons, drag-scrub inputs, saved prefs.
    // OMSI_ULTRALIGHT_PANEL=1 â†’ experimental HTML UI.
    // SLI bake controls live in Herramientas (no floating top-left Bake window).
    auto draw_sli_bake_hud = [&]() {};
    // Vista > "Punto en el origen del .sco": 2D ImGui marker at the selected object's world origin,
    // re-projected every frame so it follows camera moves and drags.
    auto draw_sco_origin_marker = [&]() {
        if (!panel.imgui_state().show_sco_origin) return;
        if (selected_object < 0 || static_cast<std::size_t>(selected_object) >= edit_objects.size()) return;
        const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
        float wx = o.position.x, wy = o.position.y, wz = 0.f;
        bool have = false;
        for (const auto& sp : scene_places) {
            if (sp.alive && sp.object_id == o.id) { // same transform the renderer uses (abs/terrain Z solved)
                wx = sp.tx;
                wy = sp.ty;
                wz = sp.tz;
                have = true;
                break;
            }
        }
        if (!have) wz = o.position.z + sample_terrain_editor_xy(wx, wy);
        float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
        input::cursor_to_framebuffer(window, mx, my, fw, fh);
        const float asp = (fh > 0.f) ? fw / fh : 16.f / 9.f;
        float wvp[16]{};
        camera.build_world_view_proj(asp, wvp);
        float sx = 0.f, sy = 0.f;
        if (!world_to_screen(wx, wy, wz, 0.f, 0.f, fw, fh, wvp, sx, sy)) return; // behind the camera
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const ImVec2 p(sx * disp.x / (std::max)(1.f, fw), sy * disp.y / (std::max)(1.f, fh));
        if (p.x < -50.f || p.y < -50.f || p.x > disp.x + 50.f || p.y > disp.y + 50.f) return;
        // Background list: drawn over the 3D view but under the panels/menus.
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const ImU32 dark = IM_COL32(0, 0, 0, 190);
        const ImU32 col = IM_COL32(255, 64, 196, 255);
        dl->AddLine(ImVec2(p.x - 11.f, p.y), ImVec2(p.x - 4.f, p.y), dark, 3.f);
        dl->AddLine(ImVec2(p.x + 4.f, p.y), ImVec2(p.x + 11.f, p.y), dark, 3.f);
        dl->AddLine(ImVec2(p.x, p.y - 11.f), ImVec2(p.x, p.y - 4.f), dark, 3.f);
        dl->AddLine(ImVec2(p.x, p.y + 4.f), ImVec2(p.x, p.y + 11.f), dark, 3.f);
        dl->AddLine(ImVec2(p.x - 11.f, p.y), ImVec2(p.x - 4.f, p.y), col, 1.5f);
        dl->AddLine(ImVec2(p.x + 4.f, p.y), ImVec2(p.x + 11.f, p.y), col, 1.5f);
        dl->AddLine(ImVec2(p.x, p.y - 11.f), ImVec2(p.x, p.y - 4.f), col, 1.5f);
        dl->AddLine(ImVec2(p.x, p.y + 4.f), ImVec2(p.x, p.y + 11.f), col, 1.5f);
        dl->AddCircleFilled(p, 4.5f, dark);
        dl->AddCircleFilled(p, 3.f, col);
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "origen  %.2f %.2f %.2f", static_cast<double>(wx), static_cast<double>(wy),
                      static_cast<double>(wz));
        dl->AddText(ImVec2(p.x + 9.f, p.y + 7.f), dark, lbl);
        dl->AddText(ImVec2(p.x + 8.f, p.y + 6.f), col, lbl);
    };
#include "modules/mod_map_pack.inc.cpp"
#include "modules/mod_coop.inc.cpp"
    // Labels of the missing-asset cubes (within 300 m of the camera target).
    auto draw_missing_asset_labels = [&]() {
        if (missing_assets.empty()) return;
        float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
        input::cursor_to_framebuffer(window, mx, my, fw, fh);
        const float asp = (fh > 0.f) ? fw / fh : 16.f / 9.f;
        float wvp[16]{};
        camera.build_world_view_proj(asp, wvp);
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        int drawn = 0;
        for (const auto& [k, m] : missing_assets) {
            const float dx = m.x - camera.target_x, dy = m.y - camera.target_y;
            if (dx * dx + dy * dy > 300.f * 300.f) continue;
            float sx = 0.f, sy = 0.f;
            if (!world_to_screen(m.x, m.y, m.z + (m.spline ? 2.f : 2.6f), 0.f, 0.f, fw, fh, wvp, sx, sy)) continue;
            const ImVec2 p(sx * disp.x / (std::max)(1.f, fw), sy * disp.y / (std::max)(1.f, fh));
            if (p.x < -100.f || p.y < -40.f || p.x > disp.x + 100.f || p.y > disp.y + 40.f) continue;
            const std::string txt = std::string(m.spline ? "falta spline " : "falta objeto ") + m.name;
            const ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
            const ImVec2 a(p.x - ts.x * 0.5f - 4.f, p.y - ts.y - 4.f);
            dl->AddRectFilled(a, ImVec2(a.x + ts.x + 8.f, a.y + ts.y + 4.f), IM_COL32(40, 0, 30, 200), 4.f);
            dl->AddText(ImVec2(a.x + 4.f, a.y + 2.f), IM_COL32(255, 120, 230, 255), txt.c_str());
            if (++drawn > 200) break;
        }
    };
    // "Editar mesh .o3d...": pick the object in the map, choose one of its .o3d meshes, edit it in place.
    struct O3dEditPick {
        bool active = false;
        bool accept = false;
        bool file = false;
        int choice = 0;
        std::int32_t obj_id = -1;
        std::vector<std::filesystem::path> meshes;
    };
    O3dEditPick o3d_pick;
    auto o3d_pick_meshes_of = [&](const map::MapObjectInst& o) {
        std::vector<std::filesystem::path> out;
        const auto sc = sco::load_sco(o.sco_path);
        if (!sc) return out;
        for (const auto& m : sc->meshes) {
            if (!m.is_o3d) continue;
            std::error_code ec;
            if (!std::filesystem::exists(m.mesh_file, ec)) continue;
            if (std::find(out.begin(), out.end(), m.mesh_file) == out.end()) out.push_back(m.mesh_file);
        }
        return out;
    };
    auto draw_o3d_pick_ui = [&]() {
        if (!o3d_pick.active) return;
        const map::MapObjectInst* po =
            (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                ? &edit_objects[static_cast<std::size_t>(selected_object)]
                : nullptr;
        if (po && po->id != o3d_pick.obj_id) {
            o3d_pick.obj_id = po->id;
            o3d_pick.meshes = o3d_pick_meshes_of(*po);
            o3d_pick.choice = 0;
        } else if (!po) {
            o3d_pick.obj_id = -1;
            o3d_pick.meshes.clear();
        }
        ui::float_slot_begin("o3d_pick", 25); // docked with the other small windows
        ImGui::SetNextWindowSize(ImVec2(460.f, 0.f), ImGuiCond_Appearing);
        bool open = true;
        const bool o3d_pick_drawn =
            ImGui::Begin("Editar mesh .o3d##o3d_pick", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
        ui::float_slot_end();
        if (o3d_pick_drawn) {
            ImGui::TextColored(ImVec4(1.f, 0.82f, 0.25f, 1.f), "Pick: hace click en un objeto del mapa");
            if (!po) {
                ImGui::TextDisabled("(ningun objeto seleccionado)");
            } else {
                ImGui::Text("Objeto: %s", omsi::wide_to_utf8(po->sco_path.filename().native()).c_str());
                if (o3d_pick.meshes.empty()) ImGui::TextDisabled("Este objeto no tiene meshes .o3d");
                for (int i = 0; i < static_cast<int>(o3d_pick.meshes.size()); ++i) {
                    const std::string lab = omsi::wide_to_utf8(o3d_pick.meshes[static_cast<std::size_t>(i)].filename().native()) +
                                            "##o3dm" + std::to_string(i);
                    ImGui::RadioButton(lab.c_str(), &o3d_pick.choice, i);
                }
            }
            ImGui::Separator();
            const bool can = po && !o3d_pick.meshes.empty();
            if (!can) ImGui::BeginDisabled();
            if (ImGui::Button("Editar", ImVec2(120.f, 0.f))) o3d_pick.accept = true;
            if (!can) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Desde archivo...", ImVec2(150.f, 0.f))) o3d_pick.file = true;
            ImGui::SameLine();
            if (ImGui::Button("Cancelar", ImVec2(110.f, 0.f))) open = false;
        }
        ImGui::End();
        if (!open) {
            o3d_pick.active = false;
            o3d_pick.accept = false;
            o3d_pick.file = false;
        }
    };
    auto draw_extra_ui = [&]() {
        draw_o3d_pick_ui();
        draw_missing_asset_labels();
        draw_map_pack_dialog();
        draw_coop_ui();
    };
    if (imgui_overlay.active()) {
        // Panel thumbnails -> ImGui SRV slots (LRU, never evicts one drawn in the last 3 frames: GPU may
        // still read it). Uploads per frame are capped by time, not count.
        struct ThumbSrv {
            std::string key;
            std::uint64_t cpu = 0, gpu = 0;
            int used = -1000;
            int fails = 0;
            bool ok = false;
        };
        static std::vector<ThumbSrv> s_thumb_srv;
        static std::unordered_map<std::string, std::size_t> s_thumb_idx;
        static int s_thumb_frame = -1;
        static double s_thumb_ms = 0.0;
        // A map reload restarts the editor in this process with a NEW ImGui descriptor heap: the slots of
        // the previous run point into the destroyed one (CopyDescriptorsSimple wrote there -> AV crash).
        s_thumb_srv.clear();
        s_thumb_idx.clear();
        s_thumb_frame = -1;
        s_thumb_ms = 0.0;
        ui::set_catalog_thumb_srv_evictor([&](const std::filesystem::path& p) {
            const std::string key = p.string();
            const auto it = s_thumb_idx.find(key);
            if (it == s_thumb_idx.end()) return;
            auto& t = s_thumb_srv[it->second];
            t.key.clear();
            t.ok = false;
            t.fails = 0;
            s_thumb_idx.erase(it);
        });
        ui::set_thumb_texture_provider([&](const std::filesystem::path& p) -> std::uint64_t {
            const int fr = ImGui::GetFrameCount();
            if (fr != s_thumb_frame) {
                s_thumb_frame = fr;
                s_thumb_ms = 0.0;
            }
            const std::string key = p.string();
            std::size_t si = static_cast<std::size_t>(-1);
            if (const auto it = s_thumb_idx.find(key); it != s_thumb_idx.end()) {
                auto& t = s_thumb_srv[it->second];
                t.used = fr;
                if (t.ok) return t.gpu;
                if (t.fails >= 120) return 0;
                if (t.fails > 2 && (fr % (8 + (t.fails / 8))) != 0) return 0;
                si = it->second;
            }
            const double thumb_budget_ms =
                stream_busy() ? 2.0 : (omsi::load_turbo_enabled() ? 5.0 : 3.5);
            if (s_thumb_ms > thumb_budget_ms) return 0;
            if (si == static_cast<std::size_t>(-1)) {
                if (s_thumb_srv.size() < 128) {
                    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
                    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
                    if (imgui_overlay.alloc_srv(&cpu, &gpu)) {
                        s_thumb_srv.push_back({});
                        s_thumb_srv.back().cpu = static_cast<std::uint64_t>(cpu.ptr);
                        s_thumb_srv.back().gpu = static_cast<std::uint64_t>(gpu.ptr);
                        si = s_thumb_srv.size() - 1;
                    }
                }
                if (si == static_cast<std::size_t>(-1)) {
                    int oldest = fr - 3;
                    for (std::size_t i = 0; i < s_thumb_srv.size(); ++i) {
                        if (s_thumb_srv[i].used < oldest) {
                            oldest = s_thumb_srv[i].used;
                            si = i;
                        }
                    }
                    if (si == static_cast<std::size_t>(-1)) return 0;
                    s_thumb_idx.erase(s_thumb_srv[si].key);
                }
                auto& t = s_thumb_srv[si];
                t.key = key;
                t.ok = false;
                t.fails = 0;
                s_thumb_idx[key] = si;
            }
            auto& t = s_thumb_srv[si];
            t.used = fr;
            const auto t0 = std::chrono::steady_clock::now();
            t.ok = renderer.copy_file_texture_srv(p, t.cpu);
            s_thumb_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!t.ok) ++t.fails;
            return t.ok ? t.gpu : 0;
        });
    }
    // Opciones del editor > Sonido (was the floating "Sonido 3D" window): applied at once, saved on release
    ui::set_options_sound_section([&]() {
        bool changed = false, commit = false;
        if (ImGui::Checkbox("Activar sonido##ws_on", &world_sound_ui.enabled)) changed = commit = true;
        ImGui::SetNextItemWidth(240.f);
        if (ImGui::SliderFloat("Volumen##ws_vol", &world_sound_ui.volume_pct, 0.f, 100.f, "%.0f %%")) changed = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
        ImGui::TextDisabled("Objetos con sonido: %d   En rango: %d   Voces: %d",
                            static_cast<int>(world_sound_scene.candidate_count()),
                            static_cast<int>(world_sound_scene.live_objects()),
                            static_cast<int>(world_sound_scene.live_voices()));
        if (changed) {
            world_sound.set_enabled(world_sound_ui.enabled);
            world_sound.set_master_volume(world_sound_ui.volume_pct / 100.f);
        }
        if (commit) {
            editor_ui_settings.world_sound_enabled = world_sound_ui.enabled;
            editor_ui_settings.world_sound_volume = world_sound_ui.volume_pct;
            map::save_editor_ui_settings(cfg.global.root, editor_ui_settings);
        }
    });
    if (imgui_overlay.active() && !ultralight_ui) {
        mesh_tab_hosted = true;
        ui::set_coop_session_tab([&]() { draw_coop_session_tab(); });
        ui::set_mesh_edit_tab([&]() { return edit_mode == EditMode::Mesh; },
                              [&]() {
                                  if (mesh_edit_body_fn) mesh_edit_body_fn();
                              });
        imgui_overlay.set_build_fn([&]() {
            // (HUD_SLOW: which part of the panel build held a frame over 30 ms - hud_build spikes of 120-280 ms
            //  while driving)
            using HudClock = std::chrono::steady_clock;
            const auto hud_t0 = HudClock::now();
            auto hud_ms = [](HudClock::time_point a) { return std::chrono::duration<double, std::milli>(HudClock::now() - a).count(); };
            ui::MapEditorImGuiArgs ui_args{};
            ui_args.panel = &panel;
            ui_args.tile_info = imgui_tile_info;
            ui_args.map_name =
                cfg.global.name.empty() ? cfg.global.friendly_name.c_str() : cfg.global.name.c_str();
            ui_args.map_friendly_name = cfg.global.friendly_name.c_str();
            ui_args.omsi_root = cfg.omsi_root;
            ui_args.catalog_on_file_picked = on_catalog_file_picked;
            ui_args.catalog_on_dir_picked = on_catalog_dir_picked;
            ui_args.tile_count = world.tiles().size();
            ui_args.object_count = edit_objects.size();
            ui_args.spline_count = edit_splines.size();
            ui_args.beamng_mode = cfg.beamng != nullptr;
            ui_args.bng_scene_tab = bng_ui_scene_tab;
            ui_args.bng_quality_tab = bng_ui_quality_tab;
            ui_args.bng_roads_tab = bng_ui_roads_tab;
            ui_args.bng_terrain_tab = bng_ui_terrain_tab;
            ui_args.bng_vegetation_tab = bng_ui_vegetation_tab;
            if (bng_ui_status) ui_args.bng_status = bng_ui_status();
            ui_args.selected_object_index = selected_object;
            ui_args.selected_spline_index = selected_spline;
            ui_args.adjust_hud = &adjust_hud_imgui;
            ui_args.shortcuts = &editor_shortcuts;
            ui_args.spline_group_count = group_sel_spl.size();
            ui_args.object_group_count = group_sel_obj.size();
            fill_water_args(ui_args);
            {
                // Multi-selection summary for the data panels (centre, shared file, mean rotation, list rows).
                std::vector<int> go = group_sel_obj, gs = group_sel_spl;
                if (go.empty() && selected_object >= 0) go.push_back(selected_object);
                if (gs.empty() && selected_spline >= 0) gs.push_back(selected_spline);
                auto is_att = [&](const map::MapObjectInst& o) {
                    return spline_attach_spawn_ids.count(o.id) != 0 || find_attach_for_child(o.id) != nullptr;
                };
                double sx = 0.0, sy = 0.0, sz = 0.0, rp = 0.0, rr = 0.0, rb = 0.0, rs = 0.0;
                std::size_t cnt = 0, nobj = 0, nspl = 0;
                for (int i : go)
                    if (i >= 0 && static_cast<std::size_t>(i) < edit_objects.size()) {
                        const auto& o = edit_objects[static_cast<std::size_t>(i)];
                        sx += o.position.x; sy += o.position.y; sz += o.position.z;
                        rr += o.rotation_deg.x; rp += o.rotation_deg.y; rb += o.rotation_deg.z;
                        ++cnt; ++nobj;
                        if (is_att(o)) ui_args.group_has_attach = true;
                    }
                for (int i : gs)
                    if (i >= 0 && static_cast<std::size_t>(i) < edit_splines.size()) {
                        const auto& sp = edit_splines[static_cast<std::size_t>(i)];
                        sx += sp.position.x; sy += sp.position.y; sz += sp.position.z;
                        rs += sp.heading_deg;
                        ++cnt; ++nspl;
                    }
                if (cnt > 0) {
                    ui_args.group_center[0] = static_cast<float>(sx / static_cast<double>(cnt));
                    ui_args.group_center[1] = static_cast<float>(sy / static_cast<double>(cnt));
                    ui_args.group_center[2] = static_cast<float>(sz / static_cast<double>(cnt));
                    if (nobj > 0) {
                        ui_args.group_mean_rot[0] = static_cast<float>(rp / static_cast<double>(nobj));
                        ui_args.group_mean_rot[1] = static_cast<float>(rr / static_cast<double>(nobj));
                        ui_args.group_mean_rot[2] = static_cast<float>(rb / static_cast<double>(nobj));
                    } else if (nspl > 0) {
                        ui_args.group_mean_rot[1] = static_cast<float>(rs / static_cast<double>(nspl));
                    }
                }
                const bool spl_tool = panel.current_tool() == ui::EditorTool::Spline;
                std::filesystem::path same;
                if (spl_tool) {
                    const int pi = selected_spline >= 0 ? selected_spline : (gs.empty() ? -1 : gs.front());
                    if (pi >= 0 && static_cast<std::size_t>(pi) < edit_splines.size())
                        same = edit_splines[static_cast<std::size_t>(pi)].sli_path;
                } else {
                    const int pi = selected_object >= 0 ? selected_object : (go.empty() ? -1 : go.front());
                    if (pi >= 0 && static_cast<std::size_t>(pi) < edit_objects.size())
                        same = edit_objects[static_cast<std::size_t>(pi)].sco_path;
                }
                if (!same.empty()) {
                    ui_args.group_same_name = same.filename().string();
                    static std::filesystem::path s_cnt_path;
                    static std::size_t s_cnt_n = 0, s_cnt_att = 0, s_cnt_size = static_cast<std::size_t>(-1);
                    static bool s_cnt_spl = false;
                    const std::size_t cur_size = spl_tool ? edit_splines.size() : edit_objects.size();
                    if (s_cnt_path != same || s_cnt_size != cur_size || s_cnt_spl != spl_tool) {
                        s_cnt_n = 0;
                        s_cnt_att = 0;
                        if (spl_tool) {
                            for (const auto& sp : edit_splines) if (sp.sli_path == same) ++s_cnt_n;
                        } else {
                            for (const auto& o : edit_objects)
                                if (o.sco_path == same) {
                                    ++s_cnt_n;
                                    if (is_att(o)) ++s_cnt_att;
                                }
                        }
                        s_cnt_path = same; s_cnt_size = cur_size; s_cnt_spl = spl_tool;
                    }
                    ui_args.group_same_total = s_cnt_n;
                    ui_args.group_same_attach = s_cnt_att;
                }
                // Rows of the "edit as list" window: rebuilt every frame while it is open (edits are kept by the UI).
                if (panel.imgui_state().group_list_open && go.size() + gs.size() <= 4000) {
                    for (int i : go)
                        if (i >= 0 && static_cast<std::size_t>(i) < edit_objects.size()) {
                            const auto& o = edit_objects[static_cast<std::size_t>(i)];
                            ui::EditorSidePanelState::GroupRow r;
                            r.spline = false; r.id = o.id; r.name = o.sco_path.filename().string(); r.attach = is_att(o);
                            if (r.attach && spline_attach_spawn_ids.count(o.id)) {
                                if (const auto sit = spline_attach_obj_to_entry.find(o.id);
                                    sit != spline_attach_obj_to_entry.end() && sit->second < edit_spline_attachments.size()) {
                                    const auto& entry = edit_spline_attachments[sit->second];
                                    if (const map::SplineInst* sp = resolve_edit_spline_for_attach(entry)) {
                                        const auto pc = map::spline_attach_to_panel(*sp, entry.att);
                                        r.v[0] = pc.x; r.v[1] = pc.y; r.v[2] = pc.z;
                                    }
                                    r.v[3] = entry.att.rotation_x; r.v[4] = entry.att.rotation_y; r.v[5] = entry.att.rotation_z;
                                    r.v[6] = entry.att.align_terrain ? 1.f : 0.f;
                                    r.v[7] = entry.att.tangential ? 1.f : 0.f;
                                }
                            } else {
                                r.v[0] = o.position.x; r.v[1] = o.position.y; r.v[2] = o.position.z;
                                r.v[3] = o.rotation_deg.y; r.v[4] = o.rotation_deg.x; r.v[5] = o.rotation_deg.z;
                                r.v[6] = o.align_terrain ? 1.f : 0.f;
                            }
                            ui_args.group_rows_live.push_back(std::move(r));
                        }
                    for (int i : gs)
                        if (i >= 0 && static_cast<std::size_t>(i) < edit_splines.size()) {
                            const auto& sp = edit_splines[static_cast<std::size_t>(i)];
                            ui::EditorSidePanelState::GroupRow r;
                            r.spline = true; r.id = sp.id; r.name = sp.sli_path.filename().string();
                            r.v[0] = sp.position.x; r.v[1] = sp.position.y; r.v[2] = sp.position.z; r.v[3] = sp.heading_deg;
                            r.v[4] = sp.length; r.v[5] = sp.radius; r.v[6] = sp.start_grad; r.v[7] = sp.end_grad;
                            r.v[8] = sp.delta_height; r.v[9] = sp.skew_start; r.v[10] = sp.skew_end;
                            r.v[11] = sp.cant_start; r.v[12] = sp.cant_end; r.v[13] = sp.mirror ? 1.f : 0.f;
                            r.v[14] = static_cast<float>(sp.terrain_align);
                            ui_args.group_rows_live.push_back(std::move(r));
                        }
                }
            }
            {
                auto add_mode = [&](const char* s) {
                    if (!ui_args.edit_mode_label.empty()) ui_args.edit_mode_label += " + ";
                    ui_args.edit_mode_label += s;
                };
                switch (edit_mode) {
                case EditMode::Grab: add_mode("G — mover"); break;
                case EditMode::GrabHeight: add_mode("G+Z — altura"); break;
                case EditMode::GrabLength: add_mode("Largo (arrastre)"); break;
                case EditMode::Radius: add_mode("Radio (arrastre)"); break;
                case EditMode::Rotate:
                    add_mode(group_rotate_active ? "R grupo" : "R — rotar");
                    break;
                case EditMode::SplineFollow: add_mode("RR — follow spline"); break;
                case EditMode::NewGrab: add_mode("N spline (10 m / grab)"); break;
                case EditMode::NewFollow: add_mode("N spline follow"); break;
                case EditMode::NewScoFollow: add_mode("N objeto"); break;
                case EditMode::TrimSpline: add_mode("Ctrl+R recortar"); break;
                case EditMode::AttachPickParent: add_mode("Attach objeto (pick padre)"); break;
                case EditMode::AttachPickSpline: add_mode("Attach spline"); break;
                case EditMode::CompleteSpline: add_mode("A — complete"); break;
                case EditMode::PickSplineGrad: add_mode("Gradiente (pick)"); break;
                case EditMode::EditSplineGrad: add_mode("Gradiente (editar)"); break;
                case EditMode::Mesh: add_mode("Mesh verts/edges"); break;
                case EditMode::None: break;
                }
                if (marquee.phase == MarqueePhase::Select)
                    add_mode(marquee.intent == MarqueeIntent::SelectOnly ? "Recuadro (solo sel.)"
                                                                        : "Recuadro (clon)");
                else if (marquee.phase == MarqueePhase::Placing)
                    add_mode("marquee placing");
                if (autofill_path_mode) add_mode("Path autofill");
                if (sli_bake_pick) add_mode("Seleccionar SLI (bake)");
                if (sli_bake_ready) add_mode("Bake SLI listo");
                if (group_grab_active && edit_mode == EditMode::Grab) add_mode("G grupo");
                if (shift_s_move.active) add_mode("G recuadro/Shift+S");
                if (picking_varparent) add_mode("Pick varparent");
                if (panel.imgui_state().spline_adapt_gradient_live) add_mode("Gradiente live");
                if (panel.imgui_state().spline_multi_pick || panel.imgui_state().object_multi_pick)
                    add_mode("Multi spline/objeto");
                if (r_tap == 2 && edit_mode != EditMode::SplineFollow) add_mode("r_tap=2");
                if (ui_args.edit_mode_label.empty()) add_mode("Idle (ninguna herramienta 3D)");
            }
            {
                std::int32_t sid = 0;
                if (selected_spline >= 0 &&
                    static_cast<std::size_t>(selected_spline) < edit_splines.size())
                    sid = edit_splines[static_cast<std::size_t>(selected_spline)].id;
                else if (selected_object >= 0 &&
                         static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                    if (const auto ei = spline_attach_entry_for_object(
                            edit_objects[static_cast<std::size_t>(selected_object)].id))
                        sid = edit_spline_attachments[*ei].parent_spline_id;
                }
                ui_args.spline_attach_parent_id = sid;
                if (sid > 0) {
                    for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
                        const auto& e = edit_spline_attachments[ei];
                        if (e.parent_spline_id != sid) continue;
                        const std::int32_t oid = e.att.instance_id > 0 ? e.att.instance_id : 0;
                        ui::SplineAttachHudItem it{};
                        it.object_id = oid;
                        std::string name = e.att.sco_path.filename().string();
                        if (oid > 0) {
                            if (const auto* o = find_edit_obj_by_id(oid))
                                name = o->sco_path.filename().string();
                        }
                        it.label = (oid > 0 ? ("id=" + std::to_string(oid) + "  ") : "") + name;
                        ui_args.spline_attach_hud.push_back(std::move(it));
                    }
                }
            }
            omsi_bus_catalog_pump();
            ui_args.bus_catalog = &omsi_bus_catalog;
            ui_args.last_omsi_bus = editor_ui_settings.last_omsi_bus;
            ui_args.sli_bake_pick_active = sli_bake_pick;
            ui_args.sli_bake_ready = sli_bake_ready;
            ui_args.sli_bake_label = sli_bake_sco.empty() ? std::string{} : omsi::path_utf8(sli_bake_sco.filename());
            ui_args.options_tile_radius = editor_tile_radius;
            ui_args.options_visual_radius_m = editor_visual_radius_m;
            ui_args.options_union_pct = editor_ui_settings.union_ball_screen_pct;
            ui_args.options_grad_pct = editor_ui_settings.grad_circle_screen_pct;
            ui_args.editor_ui_settings = &editor_ui_settings;
            ui_args.view_only = view_only;
            const double hud_args_ms = hud_ms(hud_t0);
            auto t1 = HudClock::now();
            ui::build_map_editor_imgui_ui(ui_args);
            const double hud_panel_ms = hud_ms(t1);
            t1 = HudClock::now();
            draw_sli_bake_hud();
            const double hud_bake_ms = hud_ms(t1);
            t1 = HudClock::now();
            draw_bottom_left_huds();
            const double hud_bl_ms = hud_ms(t1);
            t1 = HudClock::now();
            draw_sco_origin_marker();
            draw_extra_ui();
            const double hud_extra_ms = hud_ms(t1);
            const double hud_total = hud_ms(hud_t0);
            if (hud_total > 30.0)
                log::warn("HUD_SLOW total=" + std::to_string(hud_total) + " args=" + std::to_string(hud_args_ms) +
                          " panel=" + std::to_string(hud_panel_ms) + " bake=" + std::to_string(hud_bake_ms) +
                          " bottom_left=" + std::to_string(hud_bl_ms) + " extra=" + std::to_string(hud_extra_ms));
        });
    } else if (ultralight_ui && imgui_overlay.active()) {
        imgui_overlay.set_build_fn([&]() {
            ui::MapEditorUltralightArgs ui_args{};
            ui_args.panel = &panel;
            ui_args.overlay = &ultralight_overlay;
            ui_args.tile_info = imgui_tile_info;
            ui_args.map_name = cfg.global.friendly_name.c_str();
            ui_args.tile_count = world.tiles().size();
            ui_args.object_count = edit_objects.size();
            ui_args.spline_count = edit_splines.size();
            omsi_bus_catalog_pump();
            ui_args.bus_catalog = &omsi_bus_catalog;
            ui_args.last_omsi_bus = editor_ui_settings.last_omsi_bus;
            if (selected_object >= 0 &&
                static_cast<std::size_t>(selected_object) < edit_objects.size())
                ui_args.selected_object_id = edit_objects[static_cast<std::size_t>(selected_object)].id;
            else if (panel.imgui_object_edit().valid)
                ui_args.selected_object_id = panel.imgui_object_edit().id;
            if (selected_spline >= 0 &&
                static_cast<std::size_t>(selected_spline) < edit_splines.size())
                ui_args.selected_spline_id = edit_splines[static_cast<std::size_t>(selected_spline)].id;
            ui::draw_map_editor_ultralight_panel(ui_args);
            draw_sli_bake_hud();
            draw_bottom_left_huds();
            draw_sco_origin_marker();
            draw_extra_ui();
            ui::MapEditorImGuiArgs picker_args{};
            picker_args.panel = &panel;
            picker_args.omsi_root = cfg.omsi_root;
            picker_args.catalog_on_file_picked = on_catalog_file_picked;
            picker_args.catalog_on_dir_picked = on_catalog_dir_picked;
            omsi_bus_catalog_pump();
            picker_args.bus_catalog = &omsi_bus_catalog;
            picker_args.last_omsi_bus = editor_ui_settings.last_omsi_bus;
            ui::catalog_browser_draw(picker_args);
        });
        log::info("map editor UI: Ultralight hot-reload (assets/ui/map_editor â€” F5)");
    }

#include "modules/mod_terrain_precache.inc.cpp"

    // Streaming chooses which tiles exist; frustum cull is GPU-side.
    // OMSI screensize LOD: hide non-active [LOD] mesh bands (no remesh / no hitch).
    float visual_gpu_last_cx = 1.e30f;
    float visual_gpu_last_cy = 1.e30f;
    std::unordered_map<std::int32_t, int> sco_lod_hysteresis; // object_id → last lod_index
    std::unordered_set<std::int32_t> visual_meter_in; // hysteresis ×1.35 leave radius
    static float s_cull_cam_x = 1.e30f;
    static float s_cull_cam_y = 1.e30f;
    static float s_cull_cam_z = 1.e30f;
    static std::size_t s_cull_places_n = 0;
    static std::vector<std::uint8_t> s_cull_show_cache;
    static std::unordered_map<std::int32_t, std::pair<std::int32_t, std::int32_t>> obj_tile_by_id;
    static std::uint64_t obj_tile_by_id_gen = 0;
    // [PERF] Persistent spatial grid for distance_cull — avoids rebuilding the hashmap every
    // frame the camera moves (was 1-7 ms/frame on large maps). Only rebuilt when scene_places
    // changes size (objects streamed in/out), not on camera movement.
    static std::unordered_map<std::int64_t, std::vector<std::size_t>> s_cull_spatial_grid;
    static std::size_t s_cull_spatial_grid_gen = 0;
    // [PERF] Persistent LOD bands/bounds cache — rebuilt only when scene_places mutates.
    struct LodBand {
        int index = 0;
        float threshold = 0.f;
    };
    struct ObjectCullBounds {
        float radius = 2.f;
        float min_x = 1.e30f, max_x = -1.e30f;
        float min_y = 1.e30f, max_y = -1.e30f;
        float z = 0.f;
        bool any = false;
    };
    static std::unordered_map<std::int32_t, std::vector<LodBand>> s_bands_by_obj;
    static std::unordered_map<std::int32_t, ObjectCullBounds> s_bounds_by_obj;
    static std::unordered_set<std::int32_t> s_multi_lod_ids;
    static std::unordered_set<std::int32_t> s_surface_or_ground_ids;
    static std::size_t s_lod_cache_gen = 0;
    // [PERF] distance cull state, dense and indexed incrementally (scene_places is append-only): a streaming
    // map adds places almost every frame, and rebuilding the grid, the LOD bands and three hash maps over every
    // place each time (Thuringer Wald, 4 tiles, 2 km radius: 63k places) took 30-60 ms a frame.
    struct DcObj {
        std::vector<LodBand> bands;
        ObjectCullBounds bounds;
        float detail_div = 0.5f; // OMSI Detailfactor: 0.1 for [surface] / ground layer
        int prior = -1;          // LOD hysteresis
        int pick = 0;
        bool sorted = true;
        bool surf_known = false;
    };
    constexpr std::uint32_t kDcNoSlot = 0xffffffffu;
    static std::unordered_map<std::int32_t, std::uint32_t> s_dc_obj_slot;
    static std::vector<DcObj> s_dc_objs;
    static std::vector<std::uint32_t> s_dc_multi;      // slots with more than one LOD band
    static std::vector<std::uint32_t> s_dc_place_slot; // per place
    static std::vector<float> s_dc_place_xy;           // per place, tx/ty
    static std::vector<std::uint8_t> s_dc_meter_in, s_dc_meter_new;
    static std::vector<std::uint8_t> s_dc_applied; // per place: last visibility set here (2 = unknown)
    static std::size_t s_dc_indexed_n = 0;
    static bool s_dc_surf_pending = false;
    static double s_dc_surf_t = -1.0;
    static std::uint32_t s_dc_full_tick = 0;
    auto refresh_obj_tile_by_id = [&]() {
        if (obj_tile_by_id_gen == editor_obj_tile_epoch && !obj_tile_by_id.empty()) return;
        obj_tile_by_id.clear();
        obj_tile_by_id.reserve(edit_objects.size() * 2 + 64);
        for (const auto& tile : world.tiles()) {
            for (const auto& o : tile.objects) obj_tile_by_id[o.id] = {tile.grid_x, tile.grid_y};
        }
        obj_tile_by_id_gen = editor_obj_tile_epoch;
    };
    auto invalidate_visual_cull_cache = [&]() {
        s_cull_cam_x = 1.e30f;
        s_cull_cam_y = 1.e30f;
        s_cull_cam_z = 1.e30f;
        s_cull_places_n = 0;
        s_cull_show_cache.clear();
        // [PERF] Also invalidate persistent spatial grid and LOD caches.
        s_cull_spatial_grid.clear();
        s_cull_spatial_grid_gen = 0;
        s_bands_by_obj.clear();
        s_bounds_by_obj.clear();
        s_multi_lod_ids.clear();
        s_surface_or_ground_ids.clear();
        s_lod_cache_gen = 0;
        s_dc_obj_slot.clear();
        s_dc_objs.clear();
        s_dc_multi.clear();
        s_dc_place_slot.clear();
        s_dc_place_xy.clear();
        s_dc_meter_in.clear();
        s_dc_meter_new.clear();
        s_dc_applied.clear();
        s_dc_indexed_n = 0;
        s_dc_surf_pending = false;
        editor_visual_cull_cache_dirty = false;
    };
    auto apply_editor_visual_distance_cull = [&]() {
        if (editor_visual_cull_cache_dirty) invalidate_visual_cull_cache();
        refresh_obj_tile_by_id();
        float cam_x = 0.f, cam_y = 0.f, cam_z = 20.f;
        camera.eye_position(cam_x, cam_y, cam_z);
        // Meter cull: eye position (Kreuz/surface stay visible when orbiting around look-at).
        const float meter_x = cam_x;
        const float meter_y = cam_y;
        constexpr float kFovY = 0.25f * 3.14159265f;
        // When the user leaves the visual radius at 0 (unlimited), still cap the RENDERED
        // distance: otherwise every frame transforms + records tens of thousands of chunks
        // (render_cpu ~15 ms) and it is impossible to reach high refresh. This only affects
        // chunk visibility/draw — editing and picking still use the full in-RAM document.
        constexpr float kRenderFallbackRadiusM = 600.f;
        const float vis_r = editor_visual_radius_m > 0.f ? editor_visual_radius_m : kRenderFallbackRadiusM;
        const float vis_r2 = vis_r * vis_r;
        // Match unload Chebyshev margin (×1.35) so Z/visibility don't thrash at the load edge.
        const float vis_out = vis_r > 0.f ? vis_r * 1.35f : 0.f;
        const float vis_out2 = vis_out * vis_out;
        // Perf: camera static (<0.75 m) and same place count -> the in-radius set cannot change, so skip the
        // per-frame grid rebuild over every scene place (was 1-7 ms/frame) and just re-assert cached visibility.
        // [PERF] If the previous frame already ended with the same camera and place count, the cached
        // flags are already live on the GPU: return immediately (no scene_places walk, no
        // set_chunk_visible). Re-asserted every kCullReassertFrames as a safety net for chunk
        // revival / stream churn, which can touch visibility outside this function.
        // Eye movement that triggers a full re-evaluation over every scene place (~2.5 ms at 1 GHz with
        // 40k places). Was 0.75 m: every small orbit step recomputed; 3 m changes nothing at a radius
        // of hundreds of metres (LOD bands are tens of metres apart).
        constexpr float kCullMoveM = 3.f;
        constexpr std::uint32_t kCullReassertFrames = 15;
        static std::uint32_t s_cull_still_frames = 0;
        {
            const std::size_t n_fast = scene_places.size();
            if (std::abs(cam_x - s_cull_cam_x) < kCullMoveM && std::abs(cam_y - s_cull_cam_y) < kCullMoveM &&
                std::abs(cam_z - s_cull_cam_z) < kCullMoveM && n_fast == s_cull_places_n &&
                s_cull_show_cache.size() == n_fast) {
                if (++s_cull_still_frames < kCullReassertFrames) return;
                s_cull_still_frames = 0;
                for (std::size_t i = 0; i < n_fast; ++i) {
                    const auto& sp = scene_places[i];
                    if (!sp.alive || sp.object_id == 0) continue;
                    renderer.set_chunk_visible(sp.chunk, s_cull_show_cache[i] != 0 &&
                                                             (!sp.script_visible_known || sp.script_visible));
                }
                return;
            }
            s_cull_still_frames = 0;
        }
        constexpr float kPlaceCell = 128.f;
        const std::size_t n_places = scene_places.size();
        // new places: their object's LOD bands and bounds, and the spatial grid
        if (s_dc_indexed_n > n_places) invalidate_visual_cull_cache(); // (append-only; never expected)
        for (std::size_t i = s_dc_indexed_n; i < n_places; ++i) {
            const auto& sp = scene_places[i];
            std::uint32_t slot = kDcNoSlot;
            if (sp.object_id != 0) {
                const auto [it, fresh] =
                    s_dc_obj_slot.try_emplace(sp.object_id, static_cast<std::uint32_t>(s_dc_objs.size()));
                if (fresh) s_dc_objs.emplace_back();
                slot = it->second;
                auto& r = s_dc_objs[slot];
                bool found = false;
                for (const auto& b : r.bands)
                    if (b.index == sp.lod_index) {
                        found = true;
                        break;
                    }
                if (!found) {
                    r.bands.push_back(LodBand{sp.lod_index, sp.lod_screensize});
                    r.sorted = false;
                    if (r.bands.size() == 2) {
                        s_dc_multi.push_back(slot);
                        s_dc_surf_pending = true;
                    }
                }
                auto& bounds = r.bounds;
                bounds.radius = (std::max)(bounds.radius, sp.half_xy);
                bounds.radius = (std::max)(bounds.radius, sp.height * 0.5f);
                bounds.min_x = (std::min)(bounds.min_x, sp.tx - sp.half_xy);
                bounds.max_x = (std::max)(bounds.max_x, sp.tx + sp.half_xy);
                bounds.min_y = (std::min)(bounds.min_y, sp.ty - sp.half_xy);
                bounds.max_y = (std::max)(bounds.max_y, sp.ty + sp.half_xy);
                if (!bounds.any) {
                    bounds.z = sp.tz;
                    bounds.any = true;
                }
                const int ix = static_cast<int>(std::floor(sp.tx / kPlaceCell));
                const int iy = static_cast<int>(std::floor(sp.ty / kPlaceCell));
                s_cull_spatial_grid[(static_cast<std::int64_t>(ix) << 32) | static_cast<std::uint32_t>(iy)].push_back(i);
            }
            s_dc_place_slot.push_back(slot);
            s_dc_place_xy.push_back(sp.tx);
            s_dc_place_xy.push_back(sp.ty);
        }
        s_dc_indexed_n = n_places;
        s_dc_meter_in.resize(s_dc_objs.size(), 0);
        // [surface] / ground-layer objects with several LODs keep a finer one farther away (twice a second at
        // most: one pass over the objects)
        if (s_dc_surf_pending && glfwGetTime() - s_dc_surf_t > 0.5) {
            s_dc_surf_t = glfwGetTime();
            s_dc_surf_pending = false;
            for (const auto& o : edit_objects) {
                const auto it = s_dc_obj_slot.find(o.id);
                if (it == s_dc_obj_slot.end()) continue;
                auto& r = s_dc_objs[it->second];
                if (r.bands.size() <= 1 || r.surf_known) continue;
                r.surf_known = true;
                if (const sco::SceneryObject* sco_obj = cached_sco(o.sco_path))
                    r.detail_div =
                        (sco_obj->surface || sco::sco_is_ground_layer(sco_obj->render_priority)) ? 0.1f : 0.5f;
            }
        }
        // Meter cull per object: in when ANY of its places is in radius (surface crossings span tiles);
        // it leaves at x1.35 the radius
        if (vis_r > 0.f) {
            s_dc_meter_new.assign(s_dc_objs.size(), 0);
            const int cx = static_cast<int>(std::floor(meter_x / kPlaceCell));
            const int cy = static_cast<int>(std::floor(meter_y / kPlaceCell));
            const int cr = static_cast<int>(std::ceil(vis_out / kPlaceCell)) + 1;
            for (int dy = -cr; dy <= cr; ++dy) {
                for (int dx = -cr; dx <= cr; ++dx) {
                    const std::int64_t key =
                        (static_cast<std::int64_t>(cx + dx) << 32) | static_cast<std::uint32_t>(cy + dy);
                    const auto git = s_cull_spatial_grid.find(key);
                    if (git == s_cull_spatial_grid.end()) continue;
                    for (const std::size_t i : git->second) {
                        const std::uint32_t slot = s_dc_place_slot[i];
                        if (s_dc_meter_new[slot]) continue;
                        const float ddx = s_dc_place_xy[2 * i] - meter_x;
                        const float ddy = s_dc_place_xy[2 * i + 1] - meter_y;
                        if (ddx * ddx + ddy * ddy <= (s_dc_meter_in[slot] ? vis_out2 : vis_r2)) s_dc_meter_new[slot] = 1;
                    }
                }
            }
            s_dc_meter_in.swap(s_dc_meter_new);
        }
        // LOD band per multi-LOD object
        for (const std::uint32_t slot : s_dc_multi) {
            auto& r = s_dc_objs[slot];
            auto& bands = r.bands;
            if (!r.sorted) {
                std::sort(bands.begin(), bands.end(),
                          [](const LodBand& a, const LodBand& b) { return a.threshold > b.threshold; });
                r.sorted = true;
            }
            float radius = 2.f, ox = 0.f, oy = 0.f, oz = 0.f;
            if (r.bounds.any) {
                const auto& bounds = r.bounds;
                radius = (std::max)(bounds.radius,
                    0.5f * (std::max)(bounds.max_x - bounds.min_x, bounds.max_y - bounds.min_y));
                ox = 0.5f * (bounds.min_x + bounds.max_x);
                oy = 0.5f * (bounds.min_y + bounds.max_y);
                oz = bounds.z;
            }
            const float dx = ox - cam_x, dy = oy - cam_y, dz = oz - cam_z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            float ss = model_cfg::compute_screensize(radius, dist, kFovY);
            ss /= (std::max)(r.detail_div, 1.e-3f);
            // Never hard-hide all bands (Kreuz surface/path + multi-LOD): keep coarsest.
            const int prior = r.prior;
            int pick = bands.back().index;
            for (const auto& b : bands) {
                if (ss + 1.e-6f >= b.threshold) {
                    pick = b.index;
                    break;
                }
            }
            constexpr float kEps = 0.02f;
            if (prior >= 0 && prior != pick) {
                float th_pick = 0.f, th_prior = 0.f;
                for (const auto& b : bands) {
                    if (b.index == pick) th_pick = b.threshold;
                    if (b.index == prior) th_prior = b.threshold;
                }
                if (prior < pick) {
                    if (ss > th_prior - kEps) pick = prior;
                } else {
                    if (ss < th_pick + kEps) pick = prior;
                }
            }
            r.prior = pick;
            r.pick = pick;
        }
        // per place; the chunk's visibility is set only where it changed (all of them every 15th time, for
        // visibility changed elsewhere)
        s_cull_show_cache.resize(n_places);
        s_dc_applied.resize(n_places, 2);
        auto eval_one = [&](std::size_t i) {
            const std::uint32_t slot = s_dc_place_slot[i];
            if (slot == kDcNoSlot) {
                s_cull_show_cache[i] = 0;
                return;
            }
            const auto& r = s_dc_objs[slot];
            const bool meter_ok = vis_r <= 0.f || s_dc_meter_in[slot] != 0;
            const bool lod_ok = r.bands.size() <= 1 || scene_places[i].lod_index == r.pick;
            s_cull_show_cache[i] = (meter_ok && lod_ok) ? 1 : 0; // script state applied below, never cached
        };
        if (n_places > 4096) {
            omsi::JobSystem::global().parallel_for(0, n_places, eval_one, 1024, omsi::JobSystem::Priority::High);
        } else {
            for (std::size_t i = 0; i < n_places; ++i) eval_one(i);
        }
        const bool full_apply = ++s_dc_full_tick >= 15;
        if (full_apply) s_dc_full_tick = 0;
        for (std::size_t i = 0; i < n_places; ++i) {
            if (s_dc_place_slot[i] == kDcNoSlot) continue;
            const auto& sp = scene_places[i];
            if (!sp.alive) continue;
            const std::uint8_t v =
                (s_cull_show_cache[i] != 0 && (!sp.script_visible_known || sp.script_visible)) ? 1 : 0;
            if (!full_apply && s_dc_applied[i] == v) continue;
            s_dc_applied[i] = v;
            renderer.set_chunk_visible(sp.chunk, v != 0);
        }
        s_cull_cam_x = cam_x;
        s_cull_cam_y = cam_y;
        s_cull_cam_z = cam_z;
        s_cull_places_n = n_places;
    };

    struct VisualSpawnTask {
        std::int32_t object_id = 0;
        std::int32_t tile_gx = 0;
        std::int32_t tile_gy = 0;
    };
    std::deque<VisualSpawnTask> visual_spawn_queue;
    std::unordered_set<std::int32_t> visual_spawn_queued_ids;

    auto ensure_visual_radius_gpu = [&]() {
        // The tile streamer already owns spawn/upload work while active. Running the fallback
        // queue alongside it duplicates lookups and synchronous mesh creation on the main thread.
        // Defer this supplemental safety-net pass until streaming goes idle; nothing is discarded.
        if (stream_busy() || sa_spawn_active) return;
        refresh_obj_tile_by_id();
        const float spl_r = editor_visual_radius_m > 0.f ? editor_visual_radius_m
                                                        : omsi::HardwareProfile::global().load_radius_m();
        if (spl_r <= 0.f) return;
        const float dx = camera.target_x - visual_gpu_last_cx;
        const float dy = camera.target_y - visual_gpu_last_cy;
        const bool moved_enough = (dx * dx + dy * dy) >= 64.f;
        if (moved_enough) {
            visual_gpu_last_cx = camera.target_x;
            visual_gpu_last_cy = camera.target_y;

            // Rebuild the desired set at a spatial checkpoint, but don't upload all of it in one
            // frame. Previously this loop called stream_spawn_object_gpu for every missing object,
            // including mesh/texture work, on the main thread; a radius change could consume 600ms+.
            visual_spawn_queue.clear();
            visual_spawn_queued_ids.clear();
            if (editor_visual_radius_m > 0.f) {
                for (const auto& o : edit_objects) {
                    if (editor_deleted_object_ids.count(o.id)) continue;
                    if (spline_attach_spawn_ids.count(o.id)) continue;
                    if (!visual_in_radius(o.position.x, o.position.y)) continue;
                    if (object_has_alive_gpu(o.id)) continue;
                    if (!visual_spawn_queued_ids.insert(o.id).second) continue;
                    std::int32_t tgx = tile_gx, tgy = tile_gy;
                    if (const auto it = obj_tile_by_id.find(o.id); it != obj_tile_by_id.end()) {
                        tgx = it->second.first;
                        tgy = it->second.second;
                    }
                    if (!stream_tile_wanted(tgx, tgy)) {
                        visual_spawn_queued_ids.erase(o.id);
                        continue;
                    }
                    visual_spawn_queue.push_back({o.id, tgx, tgy});
                }
                log::info("visual radius spawn queue=" + std::to_string(visual_spawn_queue.size()) +
                          " live_places=" + std::to_string(scene_places.size()));
            }
        }

        // Consume queue incrementally. This is a scheduling slice, not a permanent object cap:
        // every eligible object remains queued until it is present, deleted, or leaves the radius.
        // Single-object work can still exceed the time slice, therefore slow individual spawns are logged.
        const auto spawn_begin = std::chrono::steady_clock::now();
        constexpr double kVisualSpawnBudgetMs = 2.5;
        constexpr std::size_t kVisualSpawnMaxPerFrame = 6;
        constexpr std::size_t kVisualSpawnMaxInspectedPerFrame = 24;
        std::size_t spawned_this_frame = 0;
        std::size_t inspected_this_frame = 0;
        while (!visual_spawn_queue.empty() &&
               spawned_this_frame < kVisualSpawnMaxPerFrame &&
               inspected_this_frame < kVisualSpawnMaxInspectedPerFrame) {
            if (spawned_this_frame > 0 &&
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - spawn_begin).count() >=
                    kVisualSpawnBudgetMs)
                break;

            const VisualSpawnTask task = visual_spawn_queue.front();
            visual_spawn_queue.pop_front();
            visual_spawn_queued_ids.erase(task.object_id);
            ++inspected_this_frame;

            if (editor_deleted_object_ids.count(task.object_id) ||
                spline_attach_spawn_ids.count(task.object_id) || object_has_alive_gpu(task.object_id))
                continue;
            auto oit = std::find_if(edit_objects.begin(), edit_objects.end(), [&](const map::MapObjectInst& o) {
                return o.id == task.object_id;
            });
            if (oit == edit_objects.end()) continue;
            if (!visual_in_radius(oit->position.x, oit->position.y)) {
                // Keep it pending: the camera may return inside the radius before the next checkpoint.
                if (visual_spawn_queued_ids.insert(task.object_id).second)
                    visual_spawn_queue.push_back(task);
                continue;
            }

            const auto one_spawn_t0 = std::chrono::steady_clock::now();
            stream_spawn_object_gpu(*oit, task.tile_gx, task.tile_gy);
            const double one_spawn_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - one_spawn_t0).count();
            ++spawned_this_frame;
            if (one_spawn_ms >= 8.0) {
                log::warn("VISUAL_SPAWN_SLOW id=" + std::to_string(task.object_id) +
                          " ms=" + std::to_string(one_spawn_ms) +
                          " sco=" + oit->sco_path.filename().string());
            }
        }

        // Splines missing inside the radius: keep going every frame until none is left (the 6 / 6 ms
        // slice used to resume only on the next 8 m camera move, so the rest never appeared).
        static bool s_spl_pending = true;
        if (moved_enough || s_spl_pending) {
            const auto remesh_t0 = std::chrono::steady_clock::now();
            int remesh_n = 0;
            s_spl_pending = false;
            for (auto& s : edit_splines) {
                if (remesh_n >= 6 ||
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - remesh_t0).count() > 6.0) {
                    s_spl_pending = true;
                    break;
                }
                if (editor_deleted_spline_ids.count(s.id)) continue;
                if (!spline::spline_in_radius_xy(s, camera.target_x, camera.target_y, spl_r)) continue;
                if (find_spline_gpu(s.id)) continue;
                remesh_spline(s);
                ++remesh_n;
            }
        }
        if (moved_enough) {
            const auto attach_t0 = std::chrono::steady_clock::now();
            // [PERF] VISUAL_ATTACH_SPAWN_SLOW (424ms): spawn_edit_attachments calls
            // append_sco_meshes_at (heavy GPU upload) synchronously for each attachment.
            // Wrap with a wall-clock guard: pass the budget deadline so the function can
            // exit early and resume next frame via the camera-move trigger (moved_enough).
            spawn_edit_attachments(visual_in_radius);
            const double attach_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - attach_t0).count();
            if (attach_ms >= 8.0) {
                log::warn("VISUAL_ATTACH_SPAWN_SLOW ms=" + std::to_string(attach_ms) +
                          " attachments=" + std::to_string(edit_attachments.size()));
            }
        } else if (attach_pending || (attach_wait_parent && glfwGetTime() - attach_wait_t > 1.0)) {
            // Finish the [attachObj] pass the budget cut short (or new spline attachment parents), and
            // once a second the children whose parent was not in the scene yet.
            attach_wait_t = glfwGetTime();
            attach_wait_parent = false;
            spawn_edit_attachments(visual_in_radius);
        }
    };

    arm_terrain_precache();
#include "modules/mod_beamng_setup.inc.cpp"
    boot_trace.mark("setup_02_06_editor_bind");
    boot_trace.finish();

