            bool* sel_filter_user_nada_ptr = nullptr;
            // Multi-select "Selección" panel — stable order, working Todo/Nada/Cancelar.
            {
                const std::size_t multi_n = group_sel_obj_ids.size() + group_sel_spl_ids.size();
                static std::size_t s_last_multi_sig = 0;
                std::size_t sig = multi_n;
                for (auto id : group_sel_obj_ids) sig = sig * 131ull + static_cast<std::size_t>(id);
                for (auto id : group_sel_spl_ids) sig = sig * 131ull + static_cast<std::size_t>(id);
                static bool s_user_nada = false; // "Nada" pressed: keep the window so "Todo" can restore the snapshot
                if (sig != s_last_multi_sig) {
                    s_last_multi_sig = sig;
                    if (multi_n > 1) { sel_filter_dismissed = false; s_user_nada = false; }
                }
                if (multi_n == 0 && !s_user_nada) {
                    // Deselected in the 3D view: the list must go away with the selection.
                    sel_filter_open = false;
                    sel_snap_obj_ids.clear();
                    sel_snap_spl_ids.clear();
                    sel_id_rows.clear();
                    sel_path_rows.clear();
                }
                sel_filter_user_nada_ptr = &s_user_nada;
                if (multi_n > 1 && !sel_filter_dismissed) {
                    if (!sel_filter_open) {
                        sel_filter_open = true;
                        sel_snap_obj_ids = group_sel_obj_ids;
                        sel_snap_spl_ids = group_sel_spl_ids;
                        sel_id_rows.clear();
                        sel_path_rows.clear();
                        auto push_id = [&](std::int32_t id, int k, const std::string& lab) {
                            sel_id_rows.push_back({id, k, true, lab});
                        };
                        for (const auto id : sel_snap_obj_ids) {
                            const map::MapObjectInst* o = nullptr;
                            for (const auto& cand : edit_objects)
                                if (cand.id == id) {
                                    o = &cand;
                                    break;
                                }
                            char lab[256]{};
                            std::snprintf(lab, sizeof(lab), "SCO %d  %s", static_cast<int>(id),
                                          o ? o->sco_path.filename().string().c_str() : "?");
                            push_id(id, 0, lab);
                        }
                        for (const auto id : sel_snap_spl_ids) {
                            const map::SplineInst* s = nullptr;
                            for (const auto& cand : edit_splines)
                                if (cand.id == id) {
                                    s = &cand;
                                    break;
                                }
                            char lab[256]{};
                            std::snprintf(lab, sizeof(lab), "SLI %d  %s", static_cast<int>(id),
                                          s ? s->sli_path.filename().string().c_str() : "?");
                            push_id(id, 1, lab);
                        }
                        std::unordered_map<std::string, int> sco_counts, sli_counts;
                        for (const auto id : sel_snap_obj_ids)
                            for (const auto& o : edit_objects)
                                if (o.id == id) {
                                    ++sco_counts[o.sco_path.lexically_normal().string()];
                                    break;
                                }
                        for (const auto id : sel_snap_spl_ids)
                            for (const auto& s : edit_splines)
                                if (s.id == id) {
                                    ++sli_counts[s.sli_path.lexically_normal().string()];
                                    break;
                                }
                        for (const auto& kv : sco_counts)
                            sel_path_rows.push_back({kv.first, 0, kv.second, true});
                        for (const auto& kv : sli_counts)
                            sel_path_rows.push_back({kv.first, 1, kv.second, true});
                    }
                }
            }
            if (sel_filter_open && (!sel_snap_obj_ids.empty() || !sel_snap_spl_ids.empty() ||
                                    !sel_id_rows.empty() || !sel_path_rows.empty())) {
                {
                    const ImGuiViewport* vp = ImGui::GetMainViewport();
                    const ImVec2 pos(vp->WorkPos.x + vp->WorkSize.x - 380.f, vp->WorkPos.y + 280.f);
                    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                }
                ImGui::SetNextWindowSizeConstraints(ImVec2(360.f, 0.f), ImVec2(360.f, 4000.f));
                ImGui::SetNextWindowBgAlpha(0.92f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
                ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
                bool win_open = sel_filter_open;
                if (ImGui::Begin("Selección", &win_open, wflags)) {
                    char sel_hdr[96]{};
                    std::snprintf(sel_hdr, sizeof(sel_hdr), "%d obj · %d spl##sel_hdr",
                                  static_cast<int>(group_sel_obj_ids.size()), static_cast<int>(group_sel_spl_ids.size()));
                    const bool sel_content_open = ImGui::CollapsingHeader(sel_hdr, ImGuiTreeNodeFlags_DefaultOpen);
                    sel_filter_minimized = !sel_content_open;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Click para minimizar / desplegar la lista de seleccionados");
                    if (sel_content_open) {
                        if (ImGui::RadioButton("Por id", sel_filter_mode == 0)) sel_filter_mode = 0;
                        ImGui::SameLine();
                        if (ImGui::RadioButton("Por path", sel_filter_mode == 1)) sel_filter_mode = 1;
                        ImGui::Separator();
                        auto remove_obj_id = [&](std::int32_t id) {
                            group_sel_obj_ids.erase(
                                std::remove(group_sel_obj_ids.begin(), group_sel_obj_ids.end(), id),
                                group_sel_obj_ids.end());
                        };
                        auto remove_spl_id = [&](std::int32_t id) {
                            group_sel_spl_ids.erase(
                                std::remove(group_sel_spl_ids.begin(), group_sel_spl_ids.end(), id),
                                group_sel_spl_ids.end());
                        };
                        auto add_obj_id = [&](std::int32_t id) {
                            if (std::find(group_sel_obj_ids.begin(), group_sel_obj_ids.end(), id) ==
                                group_sel_obj_ids.end())
                                group_sel_obj_ids.push_back(id);
                        };
                        auto add_spl_id = [&](std::int32_t id) {
                            if (std::find(group_sel_spl_ids.begin(), group_sel_spl_ids.end(), id) ==
                                group_sel_spl_ids.end())
                                group_sel_spl_ids.push_back(id);
                        };
                        {
                            const std::size_t nrows = sel_filter_mode == 0 ? sel_id_rows.size() : sel_path_rows.size();
                            const float rows_h = (std::min)(260.f, static_cast<float>(nrows) * ImGui::GetFrameHeightWithSpacing() + 8.f);
                            ImGui::BeginChild("##sf_rows", ImVec2(0.f, rows_h), ImGuiChildFlags_Borders);
                        }
                        if (sel_filter_mode == 0) {
                            // Sync checked flags from live group without reordering rows.
                            for (auto& row : sel_id_rows) {
                                if (row.kind == 0)
                                    row.checked = std::find(group_sel_obj_ids.begin(), group_sel_obj_ids.end(),
                                                            row.id) != group_sel_obj_ids.end();
                                else
                                    row.checked = std::find(group_sel_spl_ids.begin(), group_sel_spl_ids.end(),
                                                            row.id) != group_sel_spl_ids.end();
                            }
                            for (auto& row : sel_id_rows) {
                                char lab[280]{};
                                std::snprintf(lab, sizeof(lab), "%s##sf_id_%d_%d", row.label.c_str(),
                                              static_cast<int>(row.id), row.kind);
                                if (ImGui::Checkbox(lab, &row.checked)) {
                                    if (row.kind == 0) {
                                        if (!row.checked) remove_obj_id(row.id);
                                        else add_obj_id(row.id);
                                    } else {
                                        if (!row.checked) remove_spl_id(row.id);
                                        else add_spl_id(row.id);
                                    }
                                    rebuild_group_sel_indices();
                                }
                            }
                        } else {
                            for (auto& row : sel_path_rows) {
                                // Keep stable ImGui id from path hash, never vector index.
                                const std::size_t ph =
                                    std::hash<std::string>{}(row.path) ^
                                    (static_cast<std::size_t>(row.kind) * 0x9e3779b97f4a7c15ull);
                                const std::filesystem::path fp(row.path);
                                char lab[320]{};
                                std::snprintf(lab, sizeof(lab), "%s  %s  (%d)##sf_p_%llx",
                                              row.kind == 0 ? "SCO" : "SLI", fp.filename().string().c_str(),
                                              row.count, static_cast<unsigned long long>(ph));
                                if (ImGui::Checkbox(lab, &row.checked)) {
                                    if (!row.checked) {
                                        if (row.kind == 0) {
                                            for (const auto id : sel_snap_obj_ids)
                                                for (const auto& o : edit_objects)
                                                    if (o.id == id &&
                                                        o.sco_path.lexically_normal().string() == row.path)
                                                        remove_obj_id(o.id);
                                        } else {
                                            for (const auto id : sel_snap_spl_ids)
                                                for (const auto& s : edit_splines)
                                                    if (s.id == id &&
                                                        s.sli_path.lexically_normal().string() == row.path)
                                                        remove_spl_id(s.id);
                                        }
                                    } else {
                                        if (row.kind == 0) {
                                            for (const auto id : sel_snap_obj_ids)
                                                for (const auto& o : edit_objects)
                                                    if (o.id == id &&
                                                        o.sco_path.lexically_normal().string() == row.path)
                                                        add_obj_id(o.id);
                                        } else {
                                            for (const auto id : sel_snap_spl_ids)
                                                for (const auto& s : edit_splines)
                                                    if (s.id == id &&
                                                        s.sli_path.lexically_normal().string() == row.path)
                                                        add_spl_id(s.id);
                                        }
                                    }
                                    rebuild_group_sel_indices();
                                }
                            }
                        }
                        ImGui::EndChild();
                        if (ImGui::Button("Todo")) {
                            if (sel_filter_user_nada_ptr) *sel_filter_user_nada_ptr = false;
                            group_sel_obj_ids = sel_snap_obj_ids;
                            group_sel_spl_ids = sel_snap_spl_ids;
                            for (auto& r : sel_id_rows) r.checked = true;
                            for (auto& r : sel_path_rows) r.checked = true;
                            rebuild_group_sel_indices();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Nada")) {
                            if (sel_filter_user_nada_ptr) *sel_filter_user_nada_ptr = true;
                            group_sel_obj_ids.clear();
                            group_sel_spl_ids.clear();
                            for (auto& r : sel_id_rows) r.checked = false;
                            for (auto& r : sel_path_rows) r.checked = false;
                            rebuild_group_sel_indices();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancelar")) {
                            sel_filter_open = false;
                            sel_filter_dismissed = true;
                        }
                    }
                }
                if (!win_open) {
                    sel_filter_open = false;
                    sel_filter_dismissed = true;
                }
                ImGui::End();
                ImGui::PopStyleVar();
            } else if (group_sel_obj_ids.empty() && group_sel_spl_ids.empty() && sel_snap_obj_ids.empty()) {
                sel_filter_open = false;
            }
            sel_filter_user_nada_ptr = nullptr;
            {
                // Marquee / circle / lasso overlays — draw in ImGui space (HiDPI-safe).
                if (marquee.phase == MarqueePhase::Select) {
                    ImDrawList* fg = ImGui::GetForegroundDrawList();
                    const ImGuiIO& io = ImGui::GetIO();
                    const float dpi_x =
                        io.DisplayFramebufferScale.x > 0.01f ? io.DisplayFramebufferScale.x : 1.f;
                    const float dpi_y =
                        io.DisplayFramebufferScale.y > 0.01f ? io.DisplayFramebufferScale.y : 1.f;
                    auto to_ui = [&](float fx, float fy) {
                        return ImVec2(fx / dpi_x, fy / dpi_y);
                    };
                    float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                    input::cursor_to_framebuffer(window, mx, my, fw, fh);
                    if (sel_shape == SelShapeMode::Circle) {
                        // fixed-size brush under the cursor (the wheel changes the size, the drag never does)
                        const float cx = mx;
                        const float cy = my;
                        const float cr = sel_circle_r;
                        const ImVec2 c = to_ui(cx, cy);
                        const float cr_ui = cr / dpi_x;
                        if (marquee.dragging) {
                            const bool rem = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                             (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                            fg->AddCircleFilled(c, cr_ui, rem ? IM_COL32(240, 90, 80, 40) : IM_COL32(166, 227, 161, 40), 64);
                        }
                        fg->AddCircle(c, cr_ui, IM_COL32(166, 227, 161, 220), 64, 1.6f);
                        fg->AddCircle(c, cr_ui + 1.2f, IM_COL32(0, 0, 0, 140), 64, 1.2f);
                    } else if (sel_shape == SelShapeMode::Lasso && sel_lasso_pts.size() >= 2) {
                        for (std::size_t i = 1; i < sel_lasso_pts.size(); ++i)
                            fg->AddLine(to_ui(sel_lasso_pts[i - 1].x, sel_lasso_pts[i - 1].y),
                                        to_ui(sel_lasso_pts[i].x, sel_lasso_pts[i].y),
                                        IM_COL32(166, 227, 161, 220), 2.f);
                        if (sel_lasso_pts.size() >= 3)
                            fg->AddLine(to_ui(sel_lasso_pts.back().x, sel_lasso_pts.back().y),
                                        to_ui(sel_lasso_pts.front().x, sel_lasso_pts.front().y),
                                        IM_COL32(166, 227, 161, 120), 1.2f);
                    } else if (marquee.dragging) {
                        const float x0 = (std::min)(marquee.sx0, marquee.sx1);
                        const float x1 = (std::max)(marquee.sx0, marquee.sx1);
                        const float y0 = (std::min)(marquee.sy0, marquee.sy1);
                        const float y1 = (std::max)(marquee.sy0, marquee.sy1);
                        fg->AddRect(to_ui(x0, y0), to_ui(x1, y1), IM_COL32(166, 227, 161, 220), 0.f, 0, 2.f);
                        fg->AddRectFilled(to_ui(x0, y0), to_ui(x1, y1), IM_COL32(166, 227, 161, 28));
                    }
                }
            }
