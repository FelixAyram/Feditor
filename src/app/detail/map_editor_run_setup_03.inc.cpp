    auto pump_path_rails_refresh = [&]() {
        // Finish GPU of the current CPU batch before kicking another (kick clears slots).
        if (path_rails_rebuild && path_rails_cpu_started &&
            path_rails_cpu_done.load(std::memory_order_acquire)) {
            auto& gov = omsi::LoadGovernor::global();
            auto rails_fb = gov.budget();
            int uploads = 0;
            std::uint64_t bytes = 0;
            // One set for the whole batch (path_rail_exists / the id-based radius test scanned every rail/owner
            // per slot: O(n^2) on big maps).
            auto slot_key = [](std::int32_t id, int pi, bool fo) -> std::uint64_t {
                return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(id)) << 32) |
                       (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pi)) << 1) | (fo ? 1ull : 0ull);
            };
            std::unordered_set<std::uint64_t> existing_rails;
            if (path_rails_cpu_upload_i < path_rails_cpu_slots.size()) {
                existing_rails.reserve(path_rails.size() * 2 + path_rails_cpu_slots.size());
                for (const auto& r : path_rails)
                    if (r.alive) existing_rails.insert(slot_key(r.owner_id, r.path_index, r.from_object));
            }
            while (path_rails_cpu_upload_i < path_rails_cpu_slots.size()) {
                const auto& slot = path_rails_cpu_slots[path_rails_cpu_upload_i];
                if (!slot.ok || !path_rail_in_radius_at(slot.owner, slot.from_object, slot.ox, slot.oy) ||
                    existing_rails.count(slot_key(slot.owner, slot.path_index, slot.from_object))) {
                    ++path_rails_cpu_upload_i;
                    continue;
                }
                auto c = omsi::FrameBudget::gpu_mesh(slot.verts.size(), slot.inds.size());
                if (gov.admit(rails_fb, omsi::LoadGovernor::Kind::Rail, c) == omsi::FrameBudget::Admit::Defer) break;
                const auto t0 = std::chrono::steady_clock::now();
                upload_path_rail_mesh(slot.verts, slot.inds, slot.owner, slot.path_index, slot.from_object,
                                      slot.tile_gx, slot.tile_gy, slot.path_typ, slot.path_dir);
                existing_rails.insert(slot_key(slot.owner, slot.path_index, slot.from_object));
                {
                    // The rails now on screen were built from the state captured at kick time.
                    const std::uint64_t ok_key = prs_key(slot.owner, slot.from_object);
                    if (const auto sit = path_rails_cpu_sigs.find(ok_key); sit != path_rails_cpu_sigs.end())
                        path_rail_sigs[ok_key] = sit->second;
                }
                const double dt =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                rails_fb.charge(dt, c);
                gov.learn(omsi::LoadGovernor::Kind::Rail, c.bytes, dt);
                bytes += c.bytes;
                ++path_rails_cpu_upload_i;
                ++uploads;
                if (rails_fb.expired() || !rails_fb.time_ok()) break;
            }
            if (uploads > 0)
                log::info("path rails GPU +" + std::to_string(uploads) + " bytes=" + std::to_string(bytes) +
                          " " + rails_fb.summary());
            if (path_rails_cpu_upload_i >= path_rails_cpu_slots.size()) {
                path_rails_phase = 0;
                path_rails_rebuild = false;
                log::info("editor path rails=" + std::to_string(path_rails.size()) +
                          " cpu=" + std::to_string(path_rails_cpu_slots.size()));
                path_rails_cpu_slots.clear();
                path_rails_cpu_upload_i = 0;
                path_rails_join_cpu();
            }
        }
        pump_path_rails_dirty();
        if (!path_rails_rebuild) return;
        if (!path_rails_cpu_started && path_rails_cpu_slots.empty()) {
            path_rails_kick_cpu();
        }
    };

    auto sync_all_end_markers = [&](std::size_t begin = 0, std::optional<std::size_t> end_exclusive = std::nullopt) {
        const std::size_t end_i = end_exclusive ? *end_exclusive : end_markers.size();
        std::unordered_map<std::int32_t, const map::SplineInst*> spl_by_id;
        std::unordered_map<std::int32_t, const map::MapObjectInst*> obj_by_id;
        spl_by_id.reserve(edit_splines.size() * 2);
        obj_by_id.reserve(edit_objects.size() * 2);
        for (const auto& s : edit_splines) spl_by_id[s.id] = &s;
        for (const auto& o : edit_objects) obj_by_id[o.id] = &o;
        for (std::size_t mi = begin; mi < end_i && mi < end_markers.size(); ++mi) {
            auto& mk = end_markers[mi];
            if (!mk.alive) continue;
            float x = 0.f, y = 0.f, z = 0.f;
            bool ok = false;
            if (!mk.from_object) {
                const auto sit = spl_by_id.find(mk.owner_id);
                const map::SplineInst* sp = sit != spl_by_id.end() ? sit->second : nullptr;
                if (!sp) {
                    mk.alive = false;
                    renderer.set_chunk_visible(mk.chunk, false);
                    continue;
                }
                if (!mk.is_path) {
                    const auto samp = spline::evaluate_spline(*sp, mk.is_start ? 0.f : sp->length);
                    x = samp.x;
                    y = samp.y;
                    z = samp.z + kArrowFloatZ;
                    const auto wt = store_world_transposed(
                        arrow_world_matrix(x, y, z, samp.heading_rad, mk.scale));
                    renderer.set_chunk_world_matrix(mk.chunk, wt.data());
                    continue;
                }
                if (const auto def = spline::load_sli(sp->sli_path)) {
                    if (mk.path_index >= 0 && static_cast<std::size_t>(mk.path_index) < def->paths.size()) {
                        const auto& pp = def->paths[static_cast<std::size_t>(mk.path_index)];
                        const auto samp = spline::evaluate_spline(*sp, mk.is_start ? 0.f : sp->length,
                                                                  pp.lateral, pp.height);
                        x = samp.x;
                        y = samp.y;
                        z = samp.z + kMarkFloatZ;
                        ok = true;
                    }
                }
            } else {
                const auto oit = obj_by_id.find(mk.owner_id);
                const map::MapObjectInst* ob = oit != obj_by_id.end() ? oit->second : nullptr;
                if (!ob) {
                    mk.alive = false;
                    renderer.set_chunk_visible(mk.chunk, false);
                    continue;
                }
                const auto sco = sco::load_sco(ob->sco_path);
                const std::vector<sco::ScoPath>* live = nullptr;
                if (sco_path_edit.active && sco_path_edit.object_id == ob->id)
                    live = &sco_path_edit.paths;
                else if (sco)
                    live = &sco->paths;
                if (!live || mk.path_index < 0 || static_cast<std::size_t>(mk.path_index) >= live->size())
                    continue;
                const auto& path = (*live)[static_cast<std::size_t>(mk.path_index)];
                map::SplineInst mini{};
                mini.position.x = path.x;
                mini.position.y = path.y;
                mini.position.z = path.z;
                mini.heading_deg = path.heading_deg;
                mini.radius = path.radius;
                mini.length = (std::max)(0.05f, std::abs(path.length));
                const bool abs_z = sco && sco::skip_terrain_add(*sco);
                const float th = terrain_add(abs_z, ob->position.x, ob->position.y);
                const XMMATRIX world_m = sco_world_matrix(*ob, nullptr, th, 0.f, 0.f);
                const auto local = spline::evaluate_spline(mini, mk.is_start ? 0.f : mini.length);
                DirectX::XMFLOAT3 w{};
                DirectX::XMStoreFloat3(&w, xform_point(world_m, local.x, local.y, local.z));
                if (sco)
                    log_sco_path_placement(*ob, *sco, path, mk.path_index, mk.is_start, th, w.x, w.y, w.z,
                                           "marker_sync");
                x = w.x;
                y = w.y;
                z = w.z;
                ok = true;
            }
            if (!ok) continue;
            const auto wt = store_world_transposed(DirectX::XMMatrixScaling(mk.scale, mk.scale, mk.scale) *
                                                  XMMatrixTranslation(x, y, z));
            renderer.set_chunk_world_matrix(mk.chunk, wt.data());
        }
    };
    // Recreate/revive red end pegs after tile stream kills GPU owners.
    auto ensure_spline_end_markers = [&](const map::SplineInst& s) {
        auto find_or_place = [&](bool is_start, bool is_path, int path_index, float x, float y, float z,
                                 float scale, std::int32_t sgx, std::int32_t sgy, float heading_rad = 0.f) {
            const auto oit = markers_by_owner.find(s.id);
            if (oit != markers_by_owner.end()) {
            for (const std::size_t mi : oit->second) {
                if (mi >= end_markers.size()) continue;
                auto& mk = end_markers[mi];
                if (mk.from_object || mk.owner_id != s.id || mk.is_start != is_start ||
                    mk.is_path != is_path || mk.path_index != path_index)
                    continue;
                mk.alive = true;
                mk.scale = scale;
                mk.tile_gx = sgx;
                mk.tile_gy = sgy;
                const DirectX::XMMATRIX world =
                    (!is_path) ? arrow_world_matrix(x, y, z, heading_rad, scale)
                               : DirectX::XMMatrixMultiply(DirectX::XMMatrixScaling(scale, scale, scale),
                                                           DirectX::XMMatrixTranslation(x, y, z));
                const auto wt = store_world_transposed(world);
                renderer.set_chunk_world_matrix(mk.chunk, wt.data());
                return;
            }
            }
            place_marker(x, y, z, scale, is_path, s.id, is_start, false, path_index, sgx, sgy, heading_rad);
        };
        std::int32_t sgx = tile_gx, sgy = tile_gy;
        if (const auto* g = find_spline_gpu(s.id)) {
            sgx = g->tile_gx;
            sgy = g->tile_gy;
        }
        const auto a = spline::evaluate_spline(s, 0.f);
        const auto b = spline::evaluate_spline(s, s.length);
        find_or_place(true, false, 0, a.x, a.y, a.z + kArrowFloatZ, 0.35f, sgx, sgy, a.heading_rad);
        find_or_place(false, false, 0, b.x, b.y, b.z + kArrowFloatZ, 0.35f, sgx, sgy, b.heading_rad);
        if (const auto def = spline::load_sli(s.sli_path)) {
            for (int pi = 0; pi < static_cast<int>(def->paths.size()); ++pi) {
                const auto& pp = def->paths[static_cast<std::size_t>(pi)];
                const auto p0 = spline::evaluate_spline(s, 0.f, pp.lateral, pp.height);
                const auto p1 = spline::evaluate_spline(s, s.length, pp.lateral, pp.height);
                find_or_place(true, true, pi, p0.x, p0.y, p0.z + kMarkFloatZ, 0.16f, sgx, sgy);
                find_or_place(false, true, pi, p1.x, p1.y, p1.z + kMarkFloatZ, 0.16f, sgx, sgy);
            }
        }
    };
    std::vector<std::filesystem::path> extra_catalog_sco;
    std::vector<std::filesystem::path> extra_catalog_sli;
    // While a vehicle is driven the panel lists are not looked at: the stream finish only marks them
    // dirty (rebuilding them took 30-1570 ms, median 750 ms, on every streamed tile while driving).
    bool editor_lists_defer = false;
    bool editor_lists_dirty = false;
    auto refresh_editor_lists = [&]() {
        editor_lists_dirty = false;
        // Per-path cache (native string key, no lexically_normal / UTF-8 conversion per item):
        // filename text + whether the file is missing. Thousands of instances share few paths.
        struct PathInfo {
            std::string file;
            bool missing = false;
        };
        static std::unordered_map<std::wstring, PathInfo> path_info_cache;
        auto path_info = [&](const std::filesystem::path& p) -> const PathInfo& {
            const auto& wide = p.native();
            if (const auto it = path_info_cache.find(wide); it != path_info_cache.end()) return it->second;
            PathInfo pi{};
            pi.file = p.filename().string();
            if (p.empty()) {
                pi.missing = true;
            } else if (wide.size() >= 4 && wide[0] == L'v' && wide[1] == L'f' && wide[2] == L's' && wide[3] == L':') {
                // BeamNG objects carry "vfs:<shape.dae>" instead of a file on disk: not "missing".
                pi.missing = false;
            } else {
                std::error_code ec;
                bool miss = !std::filesystem::exists(p, ec);
                if (miss && !cfg.omsi_root.empty()) miss = !std::filesystem::exists(cfg.omsi_root / p, ec);
                pi.missing = miss;
            }
            return path_info_cache.emplace(wide, std::move(pi)).first->second;
        };
        auto path_missing = [&](const std::filesystem::path& p) -> bool { return path_info(p).missing; };
        std::unordered_map<std::int32_t, std::pair<std::int32_t, std::int32_t>> place_tile;
        place_tile.reserve(scene_places.size() * 2);
        for (const auto& sp : scene_places) {
            if (sp.alive) place_tile[sp.object_id] = {sp.tile_gx, sp.tile_gy};
        }
        std::unordered_map<std::int32_t, std::pair<std::int32_t, std::int32_t>> gpu_tile;
        gpu_tile.reserve(spline_gpu.size() * 2);
        for (const auto& g : spline_gpu) {
            if (g.alive) gpu_tile[g.id] = {g.tile_gx, g.tile_gy};
        }
        std::vector<ui::EditorListItem> obj_list;
        std::unordered_set<std::int32_t> listed_obj;
        obj_list.reserve(edit_objects.size() + edit_spline_attachments.size());
        listed_obj.reserve(edit_objects.size() * 2);
        for (std::size_t i = 0; i < edit_objects.size(); ++i) {
            const auto& o = edit_objects[i];
            if (spline_attach_spawn_ids.count(o.id)) {
                const auto ei = spline_attach_obj_to_entry.find(o.id);
                if (ei != spline_attach_obj_to_entry.end() && ei->second < edit_spline_attachments.size()) {
                    if (o.id != edit_spline_attachments[ei->second].att.instance_id) continue;
                }
            }
            ui::EditorListItem it{};
            it.id = o.id;
            // BeamNG TSStatic / Prefab instances: tag them so the dock list (and its filter, which also
            // searches the path) makes sense for "vfs:" shapes instead of showing them as plain files.
            const auto& onw = o.sco_path.native();
            const bool is_bng_obj =
                onw.size() >= 4 && onw[0] == L'v' && onw[1] == L'f' && onw[2] == L's' && onw[3] == L':';
            const auto& opi = path_info(o.sco_path);
            it.label = std::to_string(o.id) + (is_bng_obj ? ": [bng] " : ": ") + opi.file;
            it.path = o.sco_path;
            it.map_index = static_cast<int>(i);
            it.missing = opi.missing;
            if (it.missing) it.label = std::to_string(o.id) + ": missing " + opi.file;
            if (const auto pit = place_tile.find(o.id); pit != place_tile.end()) {
                it.tile_gx = pit->second.first;
                it.tile_gy = pit->second.second;
            }
            listed_obj.insert(o.id);
            obj_list.push_back(std::move(it));
        }
        for (const auto& entry : edit_spline_attachments) {
            if (entry.att.instance_id <= 0) continue;
            if (listed_obj.count(entry.att.instance_id)) continue;
            ui::EditorListItem it{};
            it.id = entry.att.instance_id;
            it.label = std::to_string(entry.att.instance_id) + ": " + omsi::path_utf8(entry.att.sco_path.filename()) +
                       " [splineAttach]";
            it.path = entry.att.sco_path;
            it.map_index = -1;
            it.missing = path_missing(entry.att.sco_path);
            if (it.missing)
                it.label = std::to_string(entry.att.instance_id) + ": missing " +
                           omsi::path_utf8(entry.att.sco_path.filename()) + " [splineAttach]";
            it.tile_gx = entry.tile_gx;
            it.tile_gy = entry.tile_gy;
            listed_obj.insert(entry.att.instance_id);
            obj_list.push_back(std::move(it));
        }
        panel.set_object_list(obj_list);
        std::vector<ui::EditorListItem> sli_list;
        sli_list.reserve(edit_splines.size());
        for (std::size_t i = 0; i < edit_splines.size(); ++i) {
            const auto& s = edit_splines[i];
            ui::EditorListItem it{};
            it.id = s.id;
            const auto& spi = path_info(s.sli_path);
            it.label = std::to_string(s.id) + ": " + spi.file;
            it.path = s.sli_path;
            it.map_index = static_cast<int>(i);
            it.preview_length = s.length;
            it.preview_radius = s.radius;
            it.missing = spi.missing;
            if (it.missing) it.label = std::to_string(s.id) + ": missing " + spi.file;
            if (const auto git = gpu_tile.find(s.id); git != gpu_tile.end()) {
                it.tile_gx = git->second.first;
                it.tile_gy = git->second.second;
            }
            sli_list.push_back(std::move(it));
        }
        panel.set_spline_list(sli_list);
        // Unique catalogs.
        std::vector<ui::EditorListItem> cat_o;
        std::vector<ui::EditorListItem> cat_s;
        std::unordered_set<std::wstring> cat_o_seen;
        std::unordered_set<std::wstring> cat_s_seen;
        auto add_unique = [&](std::vector<ui::EditorListItem>& dst, std::unordered_set<std::wstring>& seen,
                             const std::filesystem::path& path) {
            if (!seen.insert(path.native()).second) return;
            const auto& pi = path_info(path);
            ui::EditorListItem it{};
            it.id = static_cast<std::int32_t>(dst.size());
            it.label = pi.file;
            it.path = path;
            it.missing = pi.missing;
            if (it.missing) it.label = "missing " + pi.file;
            dst.push_back(std::move(it));
        };
        for (const auto& o : edit_objects) add_unique(cat_o, cat_o_seen, o.sco_path);
        for (const auto& s : edit_splines) add_unique(cat_s, cat_s_seen, s.sli_path);
        for (const auto& p : extra_catalog_sco) add_unique(cat_o, cat_o_seen, p);
        for (const auto& p : extra_catalog_sli) add_unique(cat_s, cat_s_seen, p);
        panel.set_catalog_objects(cat_o);
        panel.set_catalog_splines(cat_s);
    };
    auto pick_path_rail = [&](float mx, float my, float fw, float fh, const float* /*wvp_t*/,
                               bool rails_on) -> int {
        if (!rails_on) return -1;
            const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
            float wvp[16]{};
            camera.build_world_view_proj(aspect, wvp);
            auto proj = [&](float wx, float wy, float wz, float& sx, float& sy) -> bool {
                DirectX::XMFLOAT4X4 m{};
                std::memcpy(&m, wvp, sizeof(m));
                const DirectX::XMMATRIX vp = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&m));
                DirectX::XMFLOAT4 clip{};
                DirectX::XMStoreFloat4(&clip, DirectX::XMVector4Transform(DirectX::XMVectorSet(wx, wy, wz, 1.f), vp));
                if (clip.w <= 1.e-5f) return false;
                sx = (clip.x / clip.w * 0.5f + 0.5f) * fw;
                sy = (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * fh;
                return true;
            };
            float best = 16.f * 16.f;
        int best_i = -1;
        for (std::size_t ri = 0; ri < path_rails.size(); ++ri) {
            const auto& rail = path_rails[ri];
            if (!rail.alive) continue;
            if (sco_path_edit.active && !sco_path_edit.aa_mode) {
                if (!rail.from_object || rail.owner_id != sco_path_edit.object_id) continue;
            }
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
                    if (!proj(w.x, w.y, w.z + 0.06f, sx, sy)) { have = false; continue; }
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
                const int stt = 16;
                float prev_sx = 0.f, prev_sy = 0.f;
                bool have = false;
                for (int k = 0; k <= stt; ++k) {
                    const float along = sp->length * static_cast<float>(k) / static_cast<float>(stt);
                    const auto c = spline::evaluate_spline(*sp, along, pp.lateral, pp.height);
                    float sx = 0.f, sy = 0.f;
                    if (!proj(c.x, c.y, c.z + 0.06f, sx, sy)) { have = false; continue; }
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
    auto path_endpoint_pose = [&](const PathRail& rail, bool is_start, PathEndpointPick& out) -> bool {
        out = {};
        if (rail.from_object) {
            const map::MapObjectInst* ob = nullptr;
            for (const auto& o : edit_objects)
                if (o.id == rail.owner_id) {
                    ob = &o;
                    break;
                }
            if (!ob) return false;
            const auto sco = sco::load_sco(ob->sco_path);
            const std::vector<sco::ScoPath>* paths = nullptr;
            if (sco_path_edit.active && sco_path_edit.object_id == ob->id)
                paths = &sco_path_edit.paths;
            else if (sco)
                paths = &sco->paths;
            if (!paths || rail.path_index < 0 ||
                static_cast<std::size_t>(rail.path_index) >= paths->size())
                return false;
            const auto& path = (*paths)[static_cast<std::size_t>(rail.path_index)];
            map::SplineInst mini{};
            mini.position.x = path.x;
            mini.position.y = path.y;
            mini.position.z = path.z;
            mini.heading_deg = path.heading_deg;
            mini.radius = path.radius;
            mini.length = (std::max)(0.05f, std::abs(path.length));
            const bool abs_z = sco && sco::skip_terrain_add(*sco);
            const float th = terrain_add(abs_z, ob->position.x, ob->position.y);
            const XMMATRIX world_m = sco_world_matrix(*ob, nullptr, th, 0.f, 0.f);
            const auto local = spline::evaluate_spline(mini, is_start ? 0.f : mini.length);
            DirectX::XMFLOAT3 w{};
            DirectX::XMStoreFloat3(&w, xform_point(world_m, local.x, local.y, local.z));
            if (sco)
                log_sco_path_placement(*ob, *sco, path, rail.path_index, is_start, th, w.x, w.y, w.z, "path_endpoint");
            out.valid = true;
            out.owner_id = rail.owner_id;
            out.from_object = true;
            out.path_index = rail.path_index;
            out.is_start = is_start;
            out.x = w.x;
            out.y = w.y;
            out.z = w.z;
            out.heading_deg = local.heading_rad * (180.f / 3.14159265f) + ob->rotation_deg.x;
            return true;
        }
        for (const auto& s : edit_splines) {
            if (s.id != rail.owner_id) continue;
            const auto def = spline::load_sli(s.sli_path);
            if (!def || rail.path_index < 0 ||
                static_cast<std::size_t>(rail.path_index) >= def->paths.size())
                return false;
            const auto& pp = def->paths[static_cast<std::size_t>(rail.path_index)];
            const auto samp =
                spline::evaluate_spline(s, is_start ? 0.f : s.length, pp.lateral, pp.height);
            out.valid = true;
            out.owner_id = rail.owner_id;
            out.from_object = false;
            out.path_index = rail.path_index;
            out.is_start = is_start;
            out.x = samp.x;
            out.y = samp.y;
            out.z = samp.z;
            out.heading_deg = samp.heading_rad * (180.f / 3.14159265f);
            return true;
        }
        return false;
    };
    auto screen_dist2 = [&](float wx, float wy, float wz, float mx, float my, float fw, float fh,
                            const float* wvp_t) -> float {
        DirectX::XMFLOAT4X4 wvp_t_m{};
        std::memcpy(&wvp_t_m, wvp_t, sizeof(wvp_t_m));
        const DirectX::XMMATRIX wvp = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&wvp_t_m));
        DirectX::XMFLOAT4 clip{};
        DirectX::XMStoreFloat4(&clip,
                               DirectX::XMVector4Transform(DirectX::XMVectorSet(wx, wy, wz, 1.f), wvp));
        if (clip.w <= 1.e-5f) return 1.e12f;
        const float sx = (clip.x / clip.w * 0.5f + 0.5f) * fw;
        const float sy = (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * fh;
        const float dx = sx - mx;
        const float dy = sy - my;
        return dx * dx + dy * dy;
    };
    auto pick_path_endpoint_on_rail = [&](int rail_i, float mx, float my, float fw, float fh,
                                          const float* wvp_t) -> PathEndpointPick {
        PathEndpointPick out{};
        if (rail_i < 0 || static_cast<std::size_t>(rail_i) >= path_rails.size()) return out;
        const auto& rail = path_rails[static_cast<std::size_t>(rail_i)];
        PathEndpointPick a{}, b{};
        if (!path_endpoint_pose(rail, true, a) || !path_endpoint_pose(rail, false, b)) return out;
        const float da = screen_dist2(a.x, a.y, a.z + kMarkFloatZ, mx, my, fw, fh, wvp_t);
        const float db = screen_dist2(b.x, b.y, b.z + kMarkFloatZ, mx, my, fw, fh, wvp_t);
        return da <= db ? a : b;
    };
    auto pick_path_endpoint_near_mouse = [&](float mx, float my, float fw, float fh,
                                             const float* wvp_t) -> PathEndpointPick {
        if (panel.imgui_state().hide_nodes) return PathEndpointPick{}; // Vista > Ocultar nodos
        if (path_rails.empty()) ensure_path_rails_build_started();
        constexpr float kPathMarkPickPx = 56.f;
        constexpr float kPathEndPickPx = 38.f;
        auto pick_from_marker = [&](const EndMarker& mk, PathEndpointPick& out) -> bool {
            if (!mk.alive || !mk.is_path) return false;
            for (const auto& rail : path_rails) {
                if (!rail.alive || rail.owner_id != mk.owner_id || rail.from_object != mk.from_object ||
                    rail.path_index != mk.path_index)
                    continue;
                return path_endpoint_pose(rail, mk.is_start, out);
            }
            return false;
        };
        {
            std::vector<std::size_t> cand;
            cand.reserve(end_markers.size());
            for (const auto& mk : end_markers) {
                if (mk.alive && mk.is_path) cand.push_back(mk.chunk);
            }
            if (!cand.empty()) {
                const auto hit =
                    renderer.pick_chunk(mx, my, static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh),
                                        wvp_t, false, cand.data(), cand.size());
                if (hit) {
                    for (const auto& mk : end_markers) {
                        if (!mk.alive || !mk.is_path || mk.chunk != *hit) continue;
                        PathEndpointPick out{};
                        if (pick_from_marker(mk, out)) return out;
                    }
                }
            }
        }
        PathEndpointPick best{};
        float best_d2 = kPathMarkPickPx * kPathMarkPickPx;
        for (const auto& mk : end_markers) {
            if (!mk.alive || !mk.is_path) continue;
            PathEndpointPick p{};
            if (!pick_from_marker(mk, p)) continue;
            const float d2 = screen_dist2(p.x, p.y, p.z + kMarkFloatZ, mx, my, fw, fh, wvp_t);
            if (d2 < best_d2) {
                best_d2 = d2;
                best = p;
            }
        }
        if (best.valid) return best;
        best_d2 = kPathEndPickPx * kPathEndPickPx;
        for (std::size_t ri = 0; ri < path_rails.size(); ++ri) {
            const auto& rail = path_rails[ri];
            if (!rail.alive) continue;
            PathEndpointPick a{}, b{};
            if (!path_endpoint_pose(rail, true, a) || !path_endpoint_pose(rail, false, b)) continue;
            for (const PathEndpointPick* p : {&a, &b}) {
                const float d2 = screen_dist2(p->x, p->y, p->z + kMarkFloatZ, mx, my, fw, fh, wvp_t);
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best = *p;
                }
            }
        }
        auto consider_cpu = [&](const PathEndpointPick& p, float& best_d2_io, PathEndpointPick& best_io,
                                float prefer_mul) {
            if (!p.valid) return;
            const float d2 = screen_dist2(p.x, p.y, p.z + kMarkFloatZ, mx, my, fw, fh, wvp_t) * prefer_mul;
            if (d2 < best_d2_io) {
                best_d2_io = d2;
                best_io = p;
            }
        };
        if (!best.valid) best_d2 = kPathEndPickPx * kPathEndPickPx;
        else
            best_d2 = kPathMarkPickPx * kPathMarkPickPx;
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
            const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
            for (int end = 0; end < 2; ++end) {
                const auto samp = spline::evaluate_spline(s, end ? s.length : 0.f);
                PathEndpointPick p{};
                p.valid = true;
                p.owner_id = s.id;
                p.from_object = false;
                p.path_index = 0;
                p.is_start = end == 0;
                p.x = samp.x;
                p.y = samp.y;
                p.z = samp.z;
                p.heading_deg = samp.heading_rad * (180.f / 3.14159265f);
                consider_cpu(p, best_d2, best, 0.55f);
            }
        }
        for (const auto& s : edit_splines) {
            if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size() &&
                edit_splines[static_cast<std::size_t>(selected_spline)].id == s.id)
                continue;
            const auto def = spline::load_sli(s.sli_path);
            const int npath = def ? static_cast<int>(def->paths.size()) : 1;
            for (int pi = 0; pi < npath; ++pi) {
                float lat = 0.f, ht = 0.f;
                if (def && pi < static_cast<int>(def->paths.size())) {
                    lat = def->paths[static_cast<std::size_t>(pi)].lateral;
                    ht = def->paths[static_cast<std::size_t>(pi)].height;
                }
                for (int end = 0; end < 2; ++end) {
                    const auto samp = spline::evaluate_spline(s, end ? s.length : 0.f, lat, ht);
                    PathEndpointPick p{};
                    p.valid = true;
                    p.owner_id = s.id;
                    p.from_object = false;
                    p.path_index = pi;
                    p.is_start = end == 0;
                    p.x = samp.x;
                    p.y = samp.y;
                    p.z = samp.z;
                    p.heading_deg = samp.heading_rad * (180.f / 3.14159265f);
                    consider_cpu(p, best_d2, best, 1.f);
                }
            }
        }
        // Every .sco object with paths (the active one uses its live edit paths, others the cached
        // disk paths) so AA can join against any .sco, not only the one being edited.
        for (auto& obj : edit_objects) {
            const bool is_active = sco_path_edit.active && obj.id == sco_path_edit.object_id;
            const auto* sco = sco::load_sco(obj.sco_path);
            const std::vector<sco::ScoPath>* paths = nullptr;
            if (is_active)
                paths = &sco_path_edit.paths;
            else if (sco && !sco->paths.empty())
                paths = &sco->paths;
            if (!paths || paths->empty()) continue;
            if (!is_active && !visual_in_radius(obj.position.x, obj.position.y)) continue;
            const bool abs_z = sco && sco::skip_terrain_add(*sco);
            const float th = terrain_add(abs_z, obj.position.x, obj.position.y);
            const XMMATRIX world_m = sco_world_matrix(obj, nullptr, th, 0.f, 0.f);
            for (int pi = 0; pi < static_cast<int>(paths->size()); ++pi) {
                const auto& path = (*paths)[static_cast<std::size_t>(pi)];
                map::SplineInst mini = sco::sco_path_to_spline_inst(path);
                mini.length = (std::max)(0.05f, std::abs(path.length));
                for (int end = 0; end < 2; ++end) {
                    const auto local = spline::evaluate_spline(mini, end ? mini.length : 0.f);
                    DirectX::XMFLOAT3 w{};
                    DirectX::XMStoreFloat3(&w, xform_point(world_m, local.x, local.y, local.z));
                    PathEndpointPick p{};
                    p.valid = true;
                    p.owner_id = obj.id;
                    p.from_object = true;
                    p.path_index = pi;
                    p.is_start = end == 0;
                    p.x = w.x;
                    p.y = w.y;
                    p.z = w.z;
                    p.heading_deg = local.heading_rad * (180.f / 3.14159265f) + obj.rotation_deg.x;
                    consider_cpu(p, best_d2, best, 1.f);
                }
            }
        }
        return best;
    };
    auto pick_path_marker_index_near_mouse = [&](float mx, float my, float fw, float fh,
                                                 const float* wvp_t) -> int {
        if (panel.imgui_state().hide_nodes) return -1; // Vista > Ocultar nodos
        if (path_rails.empty()) ensure_path_rails_build_started();
        constexpr float kPathMarkPickPx = 56.f;
        int best_i = -1;
        float best_d2 = kPathMarkPickPx * kPathMarkPickPx;
        for (int i = 0; i < static_cast<int>(end_markers.size()); ++i) {
            const auto& mk = end_markers[static_cast<std::size_t>(i)];
            if (!mk.alive || !mk.is_path) continue;
            for (const auto& rail : path_rails) {
                if (!rail.alive || rail.owner_id != mk.owner_id || rail.from_object != mk.from_object ||
                    rail.path_index != mk.path_index)
                    continue;
                PathEndpointPick p{};
                if (!path_endpoint_pose(rail, mk.is_start, p)) continue;
                const float d2 = screen_dist2(p.x, p.y, p.z + kMarkFloatZ, mx, my, fw, fh, wvp_t);
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_i = i;
                }
                break;
            }
        }
        return best_i;
    };
    auto apply_spline_follow_to_point = [&](map::SplineInst& s, float tx, float ty) {
        float len = s.length;
        float rad = s.radius;
        spline::fit_spline_to_xy(s.position.x, s.position.y, s.heading_deg, tx, ty, len, rad);
        s.length = len;
        s.radius = rad;
    };
    // Called 3× per frame on the Spline tab (hover, end-peg, handles) — one real pick per frame.
    struct PickMemo {
        std::uint64_t frame = ~0ull;
        float mx = 0.f, my = 0.f;
        float wvp[16]{};
        int result = -1;
        bool hit(std::uint64_t fid, float x, float y, const float* w) const {
            return frame == fid && mx == x && my == y && std::memcmp(wvp, w, sizeof(wvp)) == 0;
        }
        int store(std::uint64_t fid, float x, float y, const float* w, int r) {
            frame = fid;
            mx = x;
            my = y;
            std::memcpy(wvp, w, sizeof(wvp));
            result = r;
            return r;
        }
    };
    PickMemo spline_pick_memo, object_pick_memo;
    auto pick_spline_index_raw = [&](float mx, float my, float fw, float fh, const float* wvp_t) -> int {
        const auto& hid_s = panel.imgui_state().hidden_spline_ids;
        std::vector<std::size_t> cand;
        for (const auto& g : spline_gpu) {
            if (!g.alive) continue;
            if (hid_s.count(g.id)) continue; // hidden: not clickable in 3D
            for (const std::size_t c : g.chunks) cand.push_back(c);
        }
        if (!cand.empty()) {
            const auto hit =
                renderer.pick_chunk(mx, my, static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh), wvp_t,
                                    false, cand.data(), cand.size());
            if (hit) {
                for (const auto& g : spline_gpu) {
                    if (!g.alive) continue;
                    for (const std::size_t c : g.chunks) {
                        if (c != *hit) continue;
                        for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                            if (edit_splines[i].id == g.id) return static_cast<int>(i);
                        }
                    }
                }
            }
        }
        // Fallback when mesh is culled / not yet on GPU (viewer, visual radius).
        // O(splines × segments) — reuse while mouse, camera and spline count are unchanged.
        static float s_fb_mx = -1.e9f, s_fb_my = -1.e9f;
        static float s_fb_wvp[16]{};
        static std::size_t s_fb_n = static_cast<std::size_t>(-1);
        static int s_fb_best = -1;
        static int s_fb_age = 0;
        if (s_fb_mx == mx && s_fb_my == my && s_fb_n == edit_splines.size() && ++s_fb_age < 30 &&
            std::memcmp(s_fb_wvp, wvp_t, sizeof(s_fb_wvp)) == 0)
            return s_fb_best;
        // When the path's visual mesh is hidden, the rail is the only clickable cue: widen the
        // centreline pick so clicking anywhere on the rail selects the temp spline.
        const float pick_px =
            (panel.is_in_sco_path_edit_mode() && panel.imgui_state().sco_path_hide_visual) ? 34.f : 16.f;
        float best_d2 = pick_px * pick_px;
        int best = -1;
        for (std::size_t i = 0; i < edit_splines.size(); ++i) {
            const auto& s = edit_splines[i];
            if (hid_s.count(s.id)) continue;
            if (editor_visual_radius_m > 0.f && !visual_in_radius_spline(s)) continue;
            std::optional<float> chain_z;
            if (const auto cz_it = spline_start_z.find(s.id); cz_it != spline_start_z.end())
                chain_z = cz_it->second;
            const int segs = (std::max)(6, spline::omis_teile(s, chain_z));
            for (int si = 0; si <= segs; ++si) {
                const float along = s.length * (static_cast<float>(si) / static_cast<float>(segs));
                const auto p = spline::evaluate_spline(s, along, 0.f, 0.f, chain_z);
                const float d2 = screen_dist2(p.x, p.y, p.z, mx, my, fw, fh, wvp_t);
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best = static_cast<int>(i);
                }
            }
        }
        s_fb_mx = mx;
        s_fb_my = my;
        std::memcpy(s_fb_wvp, wvp_t, sizeof(s_fb_wvp));
        s_fb_n = edit_splines.size();
        s_fb_best = best;
        s_fb_age = 0;
        return best;
    };
    auto pick_spline_index = [&](float mx, float my, float fw, float fh, const float* wvp_t) -> int {
        if (spline_pick_memo.hit(g_editor_frame_id, mx, my, wvp_t)) return spline_pick_memo.result;
        return spline_pick_memo.store(g_editor_frame_id, mx, my, wvp_t, pick_spline_index_raw(mx, my, fw, fh, wvp_t));
    };
    // -- Tree click silhouette ------------------------------------------------------------------------------
    // A tree is two crossed textured quads; clicking the transparent part of the quad next to the crown used to
    // select it. Per texture, a coarse silhouette of its opaque texels (kW x kH cells, a cell is solid when any
    // texel in it has alpha >= 128, the renderer's cutout) is built once on a worker and cached; the ray hits a
    // tree only where the quad's uv falls in a solid cell. Until it is ready the whole quad counts.
    static constexpr int kTreeSilW = 32, kTreeSilH = 48;
    struct TreeSilhouette {
        bool full = true; // no alpha to go by (texture unreadable / fully opaque): the whole quad
        std::array<std::uint8_t, kTreeSilW * kTreeSilH> cell{};
    };
    struct TreeSilCache {
        std::mutex mu;
        std::unordered_map<std::string, std::shared_ptr<const TreeSilhouette>> ready;
        std::unordered_set<std::string> pending;
    };
    static TreeSilCache s_tree_sil;
    auto tree_silhouette = [](const std::string& tex) -> std::shared_ptr<const TreeSilhouette> {
        if (tex.empty()) return {};
        std::lock_guard<std::mutex> lk(s_tree_sil.mu);
        if (const auto it = s_tree_sil.ready.find(tex); it != s_tree_sil.ready.end()) return it->second;
        if (!s_tree_sil.pending.insert(tex).second) return {};
        omsi::JobSystem::global().submit(
            [tex]() {
                auto sil = std::make_shared<TreeSilhouette>();
                if (const auto img = gfx::dx12::load_image_cpu_rgba(std::filesystem::path(tex));
                    img && img->width > 0 && img->height > 0 &&
                    img->rgba.size() >= static_cast<std::size_t>(img->width) * img->height * 4u) {
                    const std::uint32_t w = img->width, h = img->height;
                    for (std::uint32_t y = 0; y < h; ++y) {
                        const int cy =
                            (std::min)(kTreeSilH - 1, static_cast<int>(y * kTreeSilH / h));
                        const std::uint8_t* row = img->rgba.data() + static_cast<std::size_t>(y) * w * 4u;
                        for (std::uint32_t x = 0; x < w; ++x) {
                            if (row[x * 4u + 3u] < 128) continue;
                            const int cx =
                                (std::min)(kTreeSilW - 1, static_cast<int>(x * kTreeSilW / w));
                            sil->cell[static_cast<std::size_t>(cy * kTreeSilW + cx)] = 1;
                        }
                    }
                    std::size_t solid = 0;
                    for (const auto c : sil->cell) solid += c;
                    // nothing transparent: the plain quad; nothing opaque (alpha unused): never unclickable
                    sil->full = solid == sil->cell.size() || solid == 0;
                }
                std::lock_guard<std::mutex> lk2(s_tree_sil.mu);
                s_tree_sil.ready[tex] = std::move(sil);
                s_tree_sil.pending.erase(tex);
            },
            omsi::JobSystem::Priority::Low);
        return {};
    };
    // OMSI editor pick for [tree]: forest GPU mesh is batched, but selection must be
    // per-tree (sourceObjectIndex). Use CPU billboard/center test, not shared chunk id.
    // out_t: ray parameter on the same near→far ray as renderer.pick_chunk (so the caller can depth-compare).
    auto pick_tree_object_index = [&](float mx, float my, float fw, float fh, const float* wvp_t,
                                      float* out_t) -> int {
        const auto& hid_o = panel.imgui_state().hidden_object_ids;
        DirectX::XMFLOAT4X4 wvp_t_m{};
        std::memcpy(&wvp_t_m, wvp_t, sizeof(wvp_t_m));
        const DirectX::XMMATRIX wvp = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&wvp_t_m));
        float ro[3]{}, rd[3]{};
        bool have_ray = false;
        if (fw > 0.f && fh > 0.f) {
            const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(nullptr, wvp);
            const float nx = 2.f * mx / fw - 1.f;
            const float ny = 1.f - 2.f * my / fh;
            // Reverse-Z NDC (near = 1, far = 0), same ray and t units as renderer.pick_chunk. This used
            // z 0 → 1, i.e. a FAR→near ray: the smallest t was the tree farthest from the camera and
            // its t (measured from the far plane) beat any nearer mesh hit in the depth competition.
            DirectX::XMFLOAT4 n4{}, f4{};
            DirectX::XMStoreFloat4(&n4, DirectX::XMVector4Transform(DirectX::XMVectorSet(nx, ny, 1.f, 1.f), inv));
            DirectX::XMStoreFloat4(&f4, DirectX::XMVector4Transform(DirectX::XMVectorSet(nx, ny, 0.f, 1.f), inv));
            if (std::fabs(n4.w) > 1e-8f && std::fabs(f4.w) > 1e-8f) {
                ro[0] = n4.x / n4.w; ro[1] = n4.y / n4.w; ro[2] = n4.z / n4.w;
                rd[0] = f4.x / f4.w - ro[0]; rd[1] = f4.y / f4.w - ro[1]; rd[2] = f4.z / f4.w - ro[2];
                have_ray = true;
            }
        }
        auto ray_tri = [&](float ax, float ay, float az, float bx, float by, float bz, float cx, float cy,
                           float cz) -> float {
            const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
            const float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
            const float px = rd[1] * e2z - rd[2] * e2y;
            const float py = rd[2] * e2x - rd[0] * e2z;
            const float pz = rd[0] * e2y - rd[1] * e2x;
            const float det = e1x * px + e1y * py + e1z * pz;
            if (std::fabs(det) < 1e-12f) return 1e30f;
            const float inv = 1.f / det;
            const float tx = ro[0] - ax, ty = ro[1] - ay, tz = ro[2] - az;
            const float u = (tx * px + ty * py + tz * pz) * inv;
            if (u < 0.f || u > 1.f) return 1e30f;
            const float qx = ty * e1z - tz * e1y;
            const float qy = tz * e1x - tx * e1z;
            const float qz = tx * e1y - ty * e1x;
            const float v = (rd[0] * qx + rd[1] * qy + rd[2] * qz) * inv;
            if (v < 0.f || u + v > 1.f) return 1e30f;
            const float t = (e2x * qx + e2y * qy + e2z * qz) * inv;
            return t >= 0.f ? t : 1e30f;
        };
        float ex = 0.f, ey = 0.f, ez = 0.f;
        camera.eye_position(ex, ey, ez);
        (void)ez;
        std::int32_t best_oid = -1;
        float best_t = 1.e30f;
        const float rd_len2 = rd[0] * rd[0] + rd[1] * rd[1] + rd[2] * rd[2];
        for (const auto& sp : scene_places) {
            if (!sp.alive || !sp.is_tree) continue;
            if (hid_o.count(sp.object_id)) continue;
            // The drawn mesh: two crossed unit quads (editor_tree_billboard.cpp) under the chunk's world matrix.
            // Ray into that unit space; the hit's uv must land on the texture's silhouette.
            float wt[16]{};
            if (have_ray && rd_len2 > 1e-12f && renderer.chunk_world_transposed(sp.chunk, wt)) {
                // cheap reject: ray vs the tree's bounding sphere
                const float th_ = (std::max)(sp.height, 1.f);
                const float rad = std::sqrt(sp.half_xy * sp.half_xy * 2.f + 0.25f * th_ * th_) + 0.5f;
                const float px = sp.tx - ro[0], py = sp.ty - ro[1], pz = sp.tz + 0.5f * th_ - ro[2];
                const float tc = (px * rd[0] + py * rd[1] + pz * rd[2]) / rd_len2;
                const float qx = px - rd[0] * tc, qy = py - rd[1] * tc, qz = pz - rd[2] * tc;
                if (qx * qx + qy * qy + qz * qz > rad * rad) continue;
                DirectX::XMFLOAT4X4 wm{};
                std::memcpy(&wm, wt, sizeof(wm));
                DirectX::XMVECTOR det{};
                const DirectX::XMMATRIX inv =
                    DirectX::XMMatrixInverse(&det, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&wm)));
                if (std::fabs(DirectX::XMVectorGetX(det)) < 1e-12f) continue;
                DirectX::XMFLOAT3 lo{}, ld{};
                DirectX::XMStoreFloat3(&lo,
                                       DirectX::XMVector3Transform(DirectX::XMVectorSet(ro[0], ro[1], ro[2], 1.f), inv));
                DirectX::XMStoreFloat3(
                    &ld, DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(rd[0], rd[1], rd[2], 0.f), inv));
                const auto sil = tree_silhouette(sp.tree_tex);
                auto solid_at = [&](float u, float v) {
                    if (!sil || sil->full) return true;
                    const int cx = std::clamp(static_cast<int>(u * kTreeSilW), 0, kTreeSilW - 1);
                    const int cy = std::clamp(static_cast<int>(v * kTreeSilH), 0, kTreeSilH - 1);
                    return sil->cell[static_cast<std::size_t>(cy * kTreeSilW + cx)] != 0;
                };
                float t_hit = 1e30f;
                // quad A: plane y = 0, u = x + 0.5; quad B: plane x = 0, u = y + 0.5; both v = 1 - z
                if (std::fabs(ld.y) > 1e-9f) {
                    const float t = -lo.y / ld.y;
                    const float x = lo.x + t * ld.x, z = lo.z + t * ld.z;
                    if (t >= 0.f && x >= -0.5f && x <= 0.5f && z >= 0.f && z <= 1.f && solid_at(x + 0.5f, 1.f - z))
                        t_hit = (std::min)(t_hit, t);
                }
                if (std::fabs(ld.x) > 1e-9f) {
                    const float t = -lo.x / ld.x;
                    const float y = lo.y + t * ld.y, z = lo.z + t * ld.z;
                    if (t >= 0.f && y >= -0.5f && y <= 0.5f && z >= 0.f && z <= 1.f && solid_at(y + 0.5f, 1.f - z))
                        t_hit = (std::min)(t_hit, t);
                }
                if (t_hit < best_t) {
                    best_t = t_hit;
                    best_oid = sp.object_id;
                }
                continue;
            }
            // v69: pick the projected footprint of THIS tree, not a shared forest/chunk.
            // The old center-radius test only sampled one vertical line and could miss the
            // billboard when the camera moved to another part of the tile.
            const float half = (std::max)(sp.half_xy, 0.5f);
            const float h = (std::max)(sp.height, 1.f);
            const float z0 = sp.tz;
            const float z1 = sp.tz + h;
            if (!have_ray) continue;
            float rx = sp.tx - ex;
            float ry = sp.ty - ey;
            const float rlen = std::sqrt(rx * rx + ry * ry);
            float nx = 1.f, ny = 0.f;
            if (rlen >= 1e-4f) {
                nx = -ry / rlen;
                ny = rx / rlen;
            }
            const float p0x = sp.tx - nx * half, p0y = sp.ty - ny * half;
            const float p1x = sp.tx + nx * half, p1y = sp.ty + ny * half;
            const float t0 = ray_tri(p0x, p0y, z0, p1x, p1y, z0, p1x, p1y, z1);
            const float t1 = ray_tri(p0x, p0y, z0, p1x, p1y, z1, p0x, p0y, z1);
            const float t = (std::min)(t0, t1);
            if (t < best_t && t < 1e29f) {
                best_t = t;
                best_oid = sp.object_id;
            }
        }
        if (best_oid < 0) return -1;
        if (out_t) *out_t = have_ray ? best_t : 1e30f;
        for (std::size_t i = 0; i < edit_objects.size(); ++i)
            if (edit_objects[i].id == best_oid) return static_cast<int>(i);
        return -1;
    };
    // [PERF] Pick candidates are cached across frames: walking ~30k scene places (string-bearing
    // structs + hidden-id hash probes) on every hover frame was the per-frame cost. The list is
    // rebuilt only when scene_places / the hidden set / the tile epoch change. Each entry keeps a
    // cheap world-space sphere so the ray can reject most chunks before renderer.pick_chunk.
    struct PickCand {
        std::size_t chunk = 0;
        std::size_t place = 0;
        float cx = 0.f, cy = 0.f, cz = 0.f, r = 0.f;
    };
    static std::vector<PickCand> s_pick_cands;
    static std::vector<std::size_t> s_pick_cand_vis;
    static std::size_t s_pick_cand_gen = static_cast<std::size_t>(-1);
    static std::size_t s_pick_cand_hid_n = static_cast<std::size_t>(-1);
    static std::uint64_t s_pick_cand_epoch = 0;
    static std::uint64_t s_pick_cand_ver = 0;
    static std::vector<std::size_t> s_pick_cand_all;
    static std::uint64_t s_pick_cand_all_ver = static_cast<std::uint64_t>(-1);
    // Cached screen ray (one inverse VP per pick instead of one per chunk).
    static float s_pick_ray_vp[16]{};
    static float s_pick_ray_mx = -1.e9f, s_pick_ray_my = -1.e9f;
    static float s_pick_ray_fw = 0.f, s_pick_ray_fh = 0.f;
    static float s_pick_ray_o[3]{}, s_pick_ray_d[3]{};
    static float s_pick_ray_len = 0.f; // |far - near|: metres per unit of renderer.pick_chunk's t
    static bool s_pick_ray_ok = false;
    auto update_pick_ray = [](float mx, float my, float fw, float fh, const float* wvp_t) -> bool {
        if (s_pick_ray_mx == mx && s_pick_ray_my == my && s_pick_ray_fw == fw && s_pick_ray_fh == fh &&
            std::memcmp(s_pick_ray_vp, wvp_t, sizeof(s_pick_ray_vp)) == 0)
            return s_pick_ray_ok;
        s_pick_ray_mx = mx;
        s_pick_ray_my = my;
        s_pick_ray_fw = fw;
        s_pick_ray_fh = fh;
        std::memcpy(s_pick_ray_vp, wvp_t, sizeof(s_pick_ray_vp));
        s_pick_ray_ok = false;
        if (fw <= 0.f || fh <= 0.f) return false;
        DirectX::XMFLOAT4X4 vp{};
        std::memcpy(&vp, wvp_t, sizeof(vp));
        const DirectX::XMMATRIX inv_vp = DirectX::XMMatrixInverse(
            nullptr, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&vp)));
        const float ndc_x = 2.f * mx / fw - 1.f;
        const float ndc_y = 1.f - 2.f * my / fh;
        // Reverse-Z NDC (near = 1, far = 0), same convention as renderer.pick_chunk.
        DirectX::XMFLOAT4 n4{}, f4{};
        DirectX::XMStoreFloat4(&n4, DirectX::XMVector4Transform(
                                        DirectX::XMVectorSet(ndc_x, ndc_y, 1.f, 1.f), inv_vp));
        DirectX::XMStoreFloat4(&f4, DirectX::XMVector4Transform(
                                        DirectX::XMVectorSet(ndc_x, ndc_y, 0.f, 1.f), inv_vp));
        if (std::fabs(n4.w) < 1e-8f || std::fabs(f4.w) < 1e-8f) return false;
        const float ox = n4.x / n4.w, oy = n4.y / n4.w, oz = n4.z / n4.w;
        float dx = f4.x / f4.w - ox, dy = f4.y / f4.w - oy, dz = f4.z / f4.w - oz;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(len > 1e-6f)) return false;
        s_pick_ray_len = len;
        dx /= len;
        dy /= len;
        dz /= len;
        s_pick_ray_o[0] = ox;
        s_pick_ray_o[1] = oy;
        s_pick_ray_o[2] = oz;
        s_pick_ray_d[0] = dx;
        s_pick_ray_d[1] = dy;
        s_pick_ray_d[2] = dz;
        s_pick_ray_ok = true;
        return true;
    };
    // ── BeamNG forest pick (Root D) ─────────────────────────────────────────────────────────────────────────
    // Assigned by mod_beamng_setup.inc.cpp when the BeamNG backend is active: returns the ray parameter of the
    // closest forest instance under the cursor (1e30 = none) and its index in Level::forest. Forest items are
    // not edit_objects (they live in the level's forest list), so the click handler applies the selection.
    std::function<float(float, float, float, float, const float*, std::uint32_t*)> bng_pick_forest_hook;
    std::function<void(long long)> bng_select_forest_hook; // fid, or -1 to clear
    bool bng_forest_pick_won = false;      // last pick: the nearest hit was a forest instance
    std::uint32_t bng_forest_pick_fid = 0; // and this is the instance
    // ── BeamNG GroundCover pick (aggregated instance: one ground-cover type in one 32 m cell) ───────────────
    // The grass is generated as crossed quads and drawn as one chunk per (cover type, cell); the chunk
    // triangles are hit-tested like any other mesh and the click is mapped back to that aggregated instance.
    std::function<float(float, float, float, float, const float*, int*, std::uint64_t*)> bng_pick_groundcover_hook;
    std::function<void(int, std::uint64_t)> bng_select_groundcover_hook; // cover index + cell key, -1 clears
    bool bng_gc_pick_won = false;
    int bng_gc_pick_gi = -1;
    std::uint64_t bng_gc_pick_key = 0;
    // ── Vehicles of the traffic and parked-car modules (not scene places) ────────────────────────────────────
    // A parked car's hit selects its [carpark_p] object (the hook returns that edit_objects index); an AI car's
    // hit is remembered for the click handler (its details / delete window, mod_ai_traffic). Both compete by
    // ray depth with the scenery, as the forest above.
    std::function<int(float, float, float, float, const float*, float*)> parked_pick_hook;
    // A hovered / selected parking space: tint its parked car, or show its helper box for this frame while
    // "Car park real" hides the boxes (the selection colour went to a hidden mesh: nothing turned red).
    std::function<void(std::int32_t, float, float, float, float)> parked_hot_hook;
    // G on one object: X / Y lock the move to that axis (Z is the existing height mode), F6 switches the axes
    // between the object's own (heading) and the world's. Same in walk-edit (F7) and in the normal view.
    int obj_grab_axis = -1;          // -1 free, 0 X, 1 Y
    bool obj_grab_axis_local = true; // F6
    std::function<bool(float, float, float, float, const float*, float*, std::uint64_t*)> ai_pick_hook;
    bool ai_pick_won = false;
    std::uint64_t ai_pick_id = 0;
    auto pick_object_index_raw = [&](float mx, float my, float fw, float fh, const float* wvp_t) -> int {
        const auto& hid_o = panel.imgui_state().hidden_object_ids;
        // Trees (CPU per-tree box) and meshes (GPU triangles) compete by ray depth: a tree box
        // behind a house used to win unconditionally, so the house never hovered/selected.
        float tree_t = 1e30f;
        const int ti = pick_tree_object_index(mx, my, fw, fh, wvp_t, &tree_t);
        // BeamNG forest instances compete with the same ray depth (they are drawn as chunk clones).
        float forest_t = 1e30f;
        std::uint32_t forest_fid = 0;
        bng_forest_pick_won = false;
        if (bng_pick_forest_hook) forest_t = bng_pick_forest_hook(mx, my, fw, fh, wvp_t, &forest_fid);

        // [PERF] Rebuild the candidate list only when the scene really changed.
        if (s_pick_cand_gen != scene_places.size() || s_pick_cand_hid_n != hid_o.size() ||
            s_pick_cand_epoch != editor_obj_tile_epoch) {
            s_pick_cands.clear();
            s_pick_cands.reserve(scene_places.size());
            for (std::size_t pi = 0; pi < scene_places.size(); ++pi) {
                const auto& sp = scene_places[pi];
                if (!sp.alive || sp.is_tree) continue;
                if (hid_o.count(sp.object_id)) continue;
                PickCand pc{};
                pc.chunk = sp.chunk;
                pc.place = pi;
                const float half_h = 0.5f * sp.height;
                pc.cx = sp.tx;
                pc.cy = sp.ty;
                pc.cz = sp.tz + half_h;
                // Generous bound (bbox + margin): a conservative reject must never change hits.
                pc.r = (sp.half_xy + half_h) * 1.15f + 3.f; // contains the mesh wherever it lies off the origin
                s_pick_cands.push_back(pc);
            }
            s_pick_cand_gen = scene_places.size();
            s_pick_cand_hid_n = hid_o.size();
            s_pick_cand_epoch = editor_obj_tile_epoch;
            ++s_pick_cand_ver;
        }
        // Objects being dragged update tx/ty/tz without changing the place count: refresh the
        // cached spheres with a cheap float-only pass every few picks so the reject stays valid.
        if (s_pick_cands.size() > 64) {
            static std::uint32_t s_pick_cand_refresh = 0;
            if (++s_pick_cand_refresh >= 8) {
                s_pick_cand_refresh = 0;
                for (auto& pc : s_pick_cands) {
                    if (pc.place >= scene_places.size()) continue;
                    const auto& sp = scene_places[pc.place];
                    const float half_h = 0.5f * sp.height;
                    pc.cx = sp.tx;
                    pc.cy = sp.ty;
                    pc.cz = sp.tz + half_h;
                    pc.r = (sp.half_xy + half_h) * 1.15f + 3.f; // contains the mesh wherever it lies off the origin
                }
            }
        }
        const std::size_t* cand_ptr = nullptr;
        std::size_t cand_n = 0;
        // Quick ray-sphere reject first (tight loop over plain floats). Skipped on small maps,
        // where the full pass is already cheaper than the extra bookkeeping.
        if (s_pick_cands.size() > 64 && update_pick_ray(mx, my, fw, fh, wvp_t)) {
            const float ox = s_pick_ray_o[0], oy = s_pick_ray_o[1], oz = s_pick_ray_o[2];
            const float dx = s_pick_ray_d[0], dy = s_pick_ray_d[1], dz = s_pick_ray_d[2];
            s_pick_cand_vis.clear();
            for (const auto& pc : s_pick_cands) {
                const float px = pc.cx - ox, py = pc.cy - oy, pz = pc.cz - oz;
                const float t = px * dx + py * dy + pz * dz;
                float d2 = 0.f;
                if (t <= 0.f) {
                    d2 = px * px + py * py + pz * pz;
                } else {
                    const float qx = px - dx * t, qy = py - dy * t, qz = pz - dz * t;
                    d2 = qx * qx + qy * qy + qz * qz;
                }
                if (d2 <= pc.r * pc.r) s_pick_cand_vis.push_back(pc.chunk);
            }
            cand_ptr = s_pick_cand_vis.data();
            cand_n = s_pick_cand_vis.size();
        } else {
            if (s_pick_cand_all_ver != s_pick_cand_ver) {
                s_pick_cand_all.clear();
                s_pick_cand_all.reserve(s_pick_cands.size());
                for (const auto& pc : s_pick_cands) s_pick_cand_all.push_back(pc.chunk);
                s_pick_cand_all_ver = s_pick_cand_ver;
            }
            cand_ptr = s_pick_cand_all.data();
            cand_n = s_pick_cand_all.size();
        }
        // ── Exact pass: the triangle under the cursor, nearest first ─────────────────────────────────────────
        float mesh_t = 1e30f;
        std::optional<std::size_t> hit;
        if (cand_n != 0)
            hit = renderer.pick_chunk(mx, my, static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh), wvp_t, false,
                                      cand_ptr, cand_n, &mesh_t);
        if (!hit) mesh_t = 1e30f;
        // GroundCover (grass): same depth competition as the forest instances.
        float gc_t = 1e30f;
        int gc_gi = -1;
        std::uint64_t gc_key = 0;
        bng_gc_pick_won = false;
        if (bng_pick_groundcover_hook) gc_t = bng_pick_groundcover_hook(mx, my, fw, fh, wvp_t, &gc_gi, &gc_key);

        // ── Occluders: terrain tiles and visible spline meshes ──────────────────────────────────────────────
        // They are not selectable with the object tool, but they hide what is behind them: a click on the
        // ground / a road must never select a house behind a hill or an object under the road surface.
        const auto& hid_s = panel.imgui_state().hidden_spline_ids;
        static std::vector<std::size_t> s_pick_occ;
        s_pick_occ.clear();
        bool terrain_meshes = false;
        for (const auto& tc : terrain_chunks) {
            if (!tc.alive) continue;
            s_pick_occ.push_back(tc.chunk);
            terrain_meshes = true;
        }
        for (const auto& g : spline_gpu) {
            if (!g.alive || hid_s.count(g.id)) continue;
            s_pick_occ.insert(s_pick_occ.end(), g.chunks.begin(), g.chunks.end());
        }
        const float seg_len = update_pick_ray(mx, my, fw, fh, wvp_t) ? s_pick_ray_len : 0.f;
        // 0.5 m of slack: objects sunk a little into the ground / road (kerbs, ground decals, building
        // plinths) stay clickable right at their base; the coarse 5 m terrain grid used for the test
        // also differs slightly from the cut terrain that is drawn near AlignTerrain splines.
        const float occ_margin_t = seg_len > 1e-3f ? 0.5f / seg_len : 0.f;
        // Nearest occluder t (pick_chunk units) along the ray through (sx, sy), searched up to t_limit.
        auto occluder_t = [&](float sx, float sy, float t_limit) -> float {
            float best = 1e30f;
            if (!s_pick_occ.empty()) {
                float t = 1e30f;
                if (renderer.pick_chunk(sx, sy, static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh), wvp_t,
                                        false, s_pick_occ.data(), s_pick_occ.size(), &t))
                    best = t;
            }
            // No terrain tile meshes (BeamNG terrain block / heightfield only): march the CPU heightfield.
            if (!terrain_meshes && seg_len > 1e-3f && update_pick_ray(sx, sy, fw, fh, wvp_t)) {
                const float ox = s_pick_ray_o[0], oy = s_pick_ray_o[1], oz = s_pick_ray_o[2];
                const float dx = s_pick_ray_d[0], dy = s_pick_ray_d[1], dz = s_pick_ray_d[2];
                const float L = s_pick_ray_len;
                const float max_m = (std::min)((std::min)(best, t_limit) * L, 6000.f);
                bool above_seen = false;
                float prev_m = 0.f;
                for (float tm = 0.f; tm <= max_m;) {
                    if (const auto h = sample_terrain_containing_xy(ox + dx * tm, oy + dy * tm)) {
                        if (oz + dz * tm >= *h) {
                            above_seen = true;
                        } else if (above_seen) {
                            float lo = prev_m, hi = tm;
                            for (int it = 0; it < 10; ++it) {
                                const float mid = 0.5f * (lo + hi);
                                const auto hm = sample_terrain_containing_xy(ox + dx * mid, oy + dy * mid);
                                if (hm && oz + dz * mid < *hm)
                                    hi = mid;
                                else
                                    lo = mid;
                            }
                            best = (std::min)(best, hi / L);
                            break;
                        }
                    }
                    prev_m = tm;
                    tm += std::clamp(tm * 0.01f, 0.25f, 4.f);
                }
            }
            return best;
        };
        const float occ_center = occluder_t(mx, my, 1e30f);
        auto occluded_center = [&](float t) { return t < 1e29f && occ_center + occ_margin_t < t; };
        int tree_i = ti;
        if (tree_i >= 0 && occluded_center(tree_t)) {
            tree_i = -1;
            tree_t = 1e30f;
        }
        if (tree_i < 0) tree_t = 1e30f;
        if (occluded_center(forest_t)) forest_t = 1e30f;
        if (occluded_center(gc_t)) gc_t = 1e30f;
        if (hit && occluded_center(mesh_t)) {
            hit.reset();
            mesh_t = 1e30f;
        }

        // ── Thin-object tolerance ───────────────────────────────────────────────────────────────────────────
        // Posts, cables, thin fences and far poles are sub-pixel targets an exact click misses. Only when the
        // exact ray hit NOTHING selectable: retry within 4 px and keep the nearest triangle, but only if it is
        // not behind the surface under the cursor (terrain / road, +2 m) nor behind an occluder on its own
        // ray. The old 6 px spiral + padded-sphere "last resort" (+3 m radius, tolerance growing with
        // distance) selected far objects the user was not aiming at.
        const bool any_exact = hit || tree_i >= 0 || forest_t < 1e29f || gc_t < 1e29f;
        if (!any_exact && cand_n != 0) {
            const float ring_lim = occ_center < 1e29f ? occ_center + (seg_len > 1e-3f ? 2.f / seg_len : 0.f) : 1e30f;
            static constexpr float kPickRing[] = {0.f, 2.f, -2.f, 4.f, -4.f};
            for (const float ox : kPickRing)
                for (const float oy : kPickRing) {
                    if (ox == 0.f && oy == 0.f) continue;
                    if (ox * ox + oy * oy > 20.f) continue; // round 4 px disc (no 5.7 px corners)
                    float t = 1e30f;
                    const auto h = renderer.pick_chunk(mx + ox, my + oy, static_cast<std::uint32_t>(fw),
                                                       static_cast<std::uint32_t>(fh), wvp_t, false, cand_ptr, cand_n,
                                                       &t);
                    if (!h || !(t < ring_lim) || t >= mesh_t) continue;
                    if (occluder_t(mx + ox, my + oy, t) + occ_margin_t < t) continue;
                    hit = h;
                    mesh_t = t;
                }
        }

        // Forest / grass win only when they are the nearest surface: depth comparison against trees and
        // meshes. The click handler then selects them via bng_select_forest_hook / bng_select_groundcover_hook
        // (they are not edit_objects).
        const bool forest_won = forest_t < 1e29f && forest_t < tree_t && forest_t < gc_t && forest_t < mesh_t;
        const bool gc_won = !forest_won && gc_t < 1e29f && gc_t < tree_t && gc_t < mesh_t;
        bng_forest_pick_won = forest_won;
        if (forest_won) {
            bng_forest_pick_fid = forest_fid;
            return -1; // not an edit_object
        }
        if (gc_won) {
            bng_gc_pick_won = true;
            bng_gc_pick_gi = gc_gi;
            bng_gc_pick_key = gc_key;
            return -1; // not an edit_object
        }
        {
            // parked / AI cars: the nearest of everything wins
            ai_pick_won = false;
            const float scenery_t = (std::min)(hit ? mesh_t : 1e30f, tree_i >= 0 ? tree_t : 1e30f);
            float park_t = 1e30f;
            int park_i = parked_pick_hook ? parked_pick_hook(mx, my, fw, fh, wvp_t, &park_t) : -1;
            if (park_i >= 0 && occluded_center(park_t)) park_i = -1;
            float ai_t = 1e30f;
            std::uint64_t ai_id = 0;
            const bool ai_hit = ai_pick_hook && ai_pick_hook(mx, my, fw, fh, wvp_t, &ai_t, &ai_id) && !occluded_center(ai_t);
            if (ai_hit && ai_t < scenery_t && (park_i < 0 || ai_t < park_t)) {
                ai_pick_won = true;
                ai_pick_id = ai_id;
                return -1; // not an edit_object
            }
            if (park_i >= 0 && park_t < scenery_t) return park_i;
        }
        if (!hit) return tree_i;
        if (tree_i >= 0 && tree_t < mesh_t) return tree_i;
        std::int32_t best_id = -1;
        for (const auto& sp : scene_places) {
            if (sp.alive && !sp.is_tree && sp.chunk == *hit) {
                best_id = sp.object_id;
                break;
            }
        }
        if (best_id < 0) return tree_i;
        for (std::size_t i = 0; i < edit_objects.size(); ++i) {
            if (edit_objects[i].id == best_id) return static_cast<int>(i);
        }
        return tree_i;
    };
    auto pick_object_index = [&](float mx, float my, float fw, float fh, const float* wvp_t) -> int {
        if (object_pick_memo.hit(g_editor_frame_id, mx, my, wvp_t)) return object_pick_memo.result;
        return object_pick_memo.store(g_editor_frame_id, mx, my, wvp_t, pick_object_index_raw(mx, my, fw, fh, wvp_t));
    };
    refresh_editor_lists();


    auto last = std::chrono::steady_clock::now();
    const auto loop_start = last;
    int frames = 0;
    bool shot0 = false;
    bool lmb_was = false;
    bool spray_owns_lmb = false; // aerosol armed: LMB belongs to the spray, not to picking
    double lmb_down_x = 0.0, lmb_down_y = 0.0;
    bool g_was = false;
    bool r_was = false;
    bool n_was = false;
    bool c_was = false;
    bool kp_dot_was = false;
    bool esc_was = false;
    enum class SpawnHeightMode { Default = 0, Terrain = 1, OnMeshes = 2 };
    EditMode edit_mode = EditMode::None;
    std::string undo_toast_title;
    std::string undo_toast_detail;
    std::chrono::steady_clock::time_point undo_toast_until{};
    bool picking_varparent = false;
    std::int32_t varparent_pick_child_id = 0;
    SpawnHeightMode spawn_height_mode = SpawnHeightMode::Default;
    int r_tap = 0;
    int g_tap = 0;
    bool grabbing = false; // legacy alias for Grab/GrabLength/etc active
    bool rotating = false;
    // After Attach→Grab: ignore G (would cancel); LMB/Esc only.
    bool grab_suppress_g_cancel = false;
    bool attach_slide_armed = false;
    bool new_grab_lock_xy = false; // N continue: fixed start at previous end
    float grab_start_x = 0.f, grab_start_y = 0.f, grab_start_z = 0.f;
    float grab_start_sa_offset_y = 0.f;
    float grab_start_yaw = 0.f;
    float grab_start_len = 0.f, grab_start_radius = 0.f;
    float grab_mesh_ox = 0.f, grab_mesh_oy = 0.f, grab_mesh_oz = 0.f; // spline mesh baked origin for preview offset
    float grab_height_my0 = 0.f; // screen Y when entering GrabHeight
    ui::EditorAdjustHud adjust_hud;
    ui::EditorAdjustHudImGui adjust_hud_imgui;
    bool group_rotate_active = false;
    bool group_rot_have_ref = false;
    float group_rot_ref_deg = 0.f;
    std::vector<float> group_start_yaw;
    // Skew snapshot at R start so RR can undo parent/child mitre from live rotate.
    std::vector<float> group_start_skew_s;
    std::vector<float> group_start_skew_e;
    std::vector<std::int32_t> group_start_prev_id;
    std::vector<float> group_start_prev_skew_e;
    float grab_start_skew_s = 0.f;
    float grab_start_skew_e = 0.f;
    std::int32_t grab_start_prev_id = 0;
    float grab_start_prev_skew_e = 0.f;
    float xform_soa_grab_last_dx = 0.f;
    float xform_soa_grab_last_dy = 0.f;
    bool grad_edit_is_start = true;
    float grad_edit_base = 0.f;
    struct EditorClipboard {
        bool valid = false;
        float pivot_x = 0.f;
        float pivot_y = 0.f;
        std::vector<map::MapObjectInst> objects;
        std::vector<map::SplineInst> splines;
    } clipboard;
    map::SplineInst new_spline_draft{};
    // Restored from map_root/.omsi_f4_editor.cfg [ui] (loaded in setup_01).
    bool show_spline_ends = editor_ui_settings.show_spline_ends;
    bool show_path_ends = editor_ui_settings.show_path_ends;
    bool show_path_rails = editor_ui_settings.show_path_rails;
    bool autofill_spline_ends = editor_ui_settings.autofill_spline_ends;
    bool autofill_path_mode = false;
    int autofill_path_stage = 0;
    PathEndpointPick autofill_path_a{};
    PathEndpointPick autofill_path_b{};
    bool aa_join_pending = false;
    int autofill_hover_marker = -1;
    int autofill_hover_path_marker = -1;
    struct MarqueeCloneState {
        MarqueePhase phase = MarqueePhase::None;
        MarqueeIntent intent = MarqueeIntent::Clone;
        bool dragging = false;
        float sx0 = 0.f, sy0 = 0.f, sx1 = 0.f, sy1 = 0.f;
        std::vector<std::size_t> box_chunks;
        std::vector<int> track_obj;
        std::vector<int> track_spl;
        std::vector<std::int32_t> track_obj_ids;
        std::vector<std::int32_t> track_spl_ids;
        std::vector<map::MapObjectInst> base_obj;
        std::vector<map::SplineInst> base_spl;
        float anchor_x = 0.f, anchor_y = 0.f;
    } marquee;
    std::vector<int> group_sel_obj;
    std::vector<int> group_sel_spl;
    std::vector<std::int32_t> group_sel_obj_ids;
    std::vector<std::int32_t> group_sel_spl_ids;
    bool sel_filter_open = false;
    bool sel_filter_dismissed = false;
    int sel_filter_mode = 0;
    bool sel_filter_minimized = false;
    float sel_filter_anim = 1.f;
    std::vector<std::int32_t> sel_snap_obj_ids;
    std::vector<std::int32_t> sel_snap_spl_ids;
    struct SelPathRow {
        std::string path;
        int kind = 0;
        int count = 0;
        bool checked = true;
    };
    std::vector<SelPathRow> sel_path_rows;
    struct SelIdRow {
        std::int32_t id = 0;
        int kind = 0;
        bool checked = true;
        std::string label;
    };
    std::vector<SelIdRow> sel_id_rows;
    enum class SelShapeMode : std::uint8_t { Rect = 0, Circle = 1, Lasso = 2 };
    SelShapeMode sel_shape = SelShapeMode::Rect;
    float sel_circle_r = 64.f;
    struct SelLassoPt { float x = 0.f, y = 0.f; };
    std::vector<SelLassoPt> sel_lasso_pts;
    bool sel_lasso_drawing = false;
    // Terrain layer lasso: freehand polygon in editor world XY (paint/erase the selected layer).
    std::vector<std::pair<float, float>> terrain_lasso_pts;
    bool terrain_lasso_drawing = false;
    // Terrain vertex edit (clickable per-vertex handles + up/down arrows).
    struct TerrainVertexEditState {
        int field = -1, ix = -1, iy = -1; // selected vertex
        int hover_field = -1, hover_ix = -1, hover_iy = -1;
        bool dragging = false;
        float start_h = 0.f, start_my = 0.f;
    };
    TerrainVertexEditState terrain_vertex;
    std::size_t sel_circle_brush_upto = 0; // frame-a-frame circle paint progress this stroke
    auto rebuild_group_sel_indices = [&]() {
        group_sel_obj.clear();
        group_sel_spl.clear();
        for (const auto want : group_sel_obj_ids) {
            for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                if (edit_objects[i].id == want) {
                    group_sel_obj.push_back(static_cast<int>(i));
                    break;
                }
            }
        }
        for (const auto want : group_sel_spl_ids) {
            for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                if (edit_splines[i].id == want) {
                    group_sel_spl.push_back(static_cast<int>(i));
                    break;
                }
            }
        }
        selected_object = -1;
        selected_spline = -1;
        if (group_sel_obj.size() == 1) selected_object = group_sel_obj.front();
        else if (group_sel_obj.empty() && group_sel_spl.size() == 1) selected_spline = group_sel_spl.front();
    };
    auto maybe_multi_spline_hint = [&]() {
        auto& st = panel.imgui_state();
        if (st.hints_ignore || group_sel_spl.size() < 2) return;
        if (st.hint_visible && st.hint_id == 1) return;
        st.hint_id = 1;
        st.hint_visible = true;
        st.hint_play_sound = true;
        st.hint_anim = 0.f;
        st.hint_text =
            "Varias splines: R, RR y N se aplican a todas a la vez, igual que a una sola.";
    };
    bool sli_bake_pick = false;
    bool sli_bake_ready = false;
    std::filesystem::path sli_bake_sco;
    float sli_bake_cx = 0.f, sli_bake_cy = 0.f, sli_bake_cz = 0.f;
    // --- Shift+S group XY/Z move: dedicated state (does NOT share group_grab_* / rotate) ---
    struct ShiftSMoveItem {
        std::uint8_t kind = 0; // 0=object 1=spline
        std::int32_t id = 0;
        float x0 = 0.f, y0 = 0.f, z0 = 0.f; // pose frozen at G-start
    };
    struct ShiftSMoveState {
        bool active = false;
        bool have_ref = false;
        float ref_mx = 0.f, ref_my = 0.f;
        float cur_dx = 0.f, cur_dy = 0.f; // last applied mouse delta (shared by all items)
        float cur_dz = 0.f;               // last applied height delta (shared by all items)
        std::vector<ShiftSMoveItem> items;
    } shift_s_move;

    // Dedicated Shift+S move helpers — intentionally duplicated vs single-G / rotate paths.
    auto shift_s_find_obj = [&](std::int32_t id) -> map::MapObjectInst* {
        for (auto& o : edit_objects)
            if (o.id == id) return &o;
        return nullptr;
    };
    auto shift_s_find_spl = [&](std::int32_t id) -> map::SplineInst* {
        for (auto& s : edit_splines)
            if (s.id == id) return &s;
        return nullptr;
    };
    auto shift_s_move_clear = [&]() {
        shift_s_move.active = false;
        shift_s_move.have_ref = false;
        shift_s_move.ref_mx = 0.f;
        shift_s_move.ref_my = 0.f;
        shift_s_move.cur_dx = 0.f;
        shift_s_move.cur_dy = 0.f;
        shift_s_move.cur_dz = 0.f;
        shift_s_move.items.clear();
    };
    auto shift_s_move_restore_starts = [&](bool do_gpu) {
        for (const auto& it : shift_s_move.items) {
            if (it.kind == 0) {
                if (auto* o = shift_s_find_obj(it.id)) {
                    o->position.x = it.x0;
                    o->position.y = it.y0;
                    o->position.z = it.z0;
                    if (do_gpu) sync_object_gpu(*o);
                }
            } else if (auto* s = shift_s_find_spl(it.id)) {
                s->position.x = it.x0;
                s->position.y = it.y0;
                s->position.z = it.z0;
                if (do_gpu) {
                    set_spline_chunk_offset(s->id, 0.f, 0.f, 0.f);
                    remesh_spline(*s);
                }
            }
        }
    };
    // Same (dx,dy,dz) for every item → relative spacing stays exact. Resolve by id every time.
    auto shift_s_move_apply = [&](float dx, float dy, float dz_add, bool do_gpu, bool height_remesh) {
        shift_s_move.cur_dx = dx;
        shift_s_move.cur_dy = dy;
        shift_s_move.cur_dz = dz_add;
        for (const auto& it : shift_s_move.items) {
            if (it.kind == 0) {
                auto* o = shift_s_find_obj(it.id);
                if (!o) continue;
                o->position.x = it.x0 + dx;
                o->position.y = it.y0 + dy;
                o->position.z = it.z0 + dz_add;
                if (do_gpu) sync_object_gpu(*o);
            } else {
                auto* s = shift_s_find_spl(it.id);
                if (!s) continue;
                s->position.x = it.x0 + dx;
                s->position.y = it.y0 + dy;
                s->position.z = it.z0 + dz_add;
                if (!do_gpu) continue;
                if (height_remesh) {
                    remesh_spline(*s, true);
                } else {
                    // Mesh was baked at (x0,y0) on begin → one shared translation keeps spacing.
                    set_spline_chunk_offset(s->id, dx, dy, 0.f);
                }
            }
        }
    };
    auto shift_s_move_begin = [&]() -> bool {
        shift_s_move_clear();
        shift_s_move.items.reserve(group_sel_obj_ids.size() + group_sel_spl_ids.size());
        for (const std::int32_t id : group_sel_obj_ids) {
            auto* o = shift_s_find_obj(id);
            if (!o) continue;
            ShiftSMoveItem it{};
            it.kind = 0;
            it.id = id;
            it.x0 = o->position.x;
            it.y0 = o->position.y;
            it.z0 = o->position.z;
            shift_s_move.items.push_back(it);
            for (auto& sp : scene_places) {
                if (sp.object_id == id) sp.pinned = true;
            }
        }
        for (const std::int32_t id : group_sel_spl_ids) {
            auto* s = shift_s_find_spl(id);
            if (!s) continue;
            // Mesh must already match logical pose with identity world matrix
            // (Shift+S settle always remeshes). Drag applies one shared (dx,dy) offset.
            ShiftSMoveItem it{};
            it.kind = 1;
            it.id = id;
            it.x0 = s->position.x;
            it.y0 = s->position.y;
            it.z0 = s->position.z;
            shift_s_move.items.push_back(it);
            if (auto* g = find_spline_gpu(id)) g->pinned = true;
        }
        shift_s_move.active = !shift_s_move.items.empty();
        if (shift_s_move.active) {
            editor_log::event("SHIFT_S_MOVE_BEGIN",
                              "count=" + std::to_string(shift_s_move.items.size()) + " obj_ids=" +
                                  std::to_string(group_sel_obj_ids.size()) + " spl_ids=" +
                                  std::to_string(group_sel_spl_ids.size()));
        }
        return shift_s_move.active;
    };

    bool group_grab_active = false;
    float group_pivot_x = 0.f, group_pivot_y = 0.f;
    std::vector<float> group_off_x, group_off_y, group_off_z;
    std::vector<int> group_grab_kind; // 0=object 1=spline
    std::vector<int> group_grab_index;
    std::vector<std::int32_t> group_grab_ids;
    std::vector<float> group_grab_start_z;
    std::vector<float> group_grab_mesh_ox, group_grab_mesh_oy; // start XY per group item (same index as group_grab_*)
    bool group_grab_have_mouse_ref = false;
    float group_grab_ref_mx = 0.f, group_grab_ref_my = 0.f;
    std::size_t trim_preview_chunk = static_cast<std::size_t>(-1);
    float trim_along = 0.f;
    std::vector<float> trim_cuts;
    bool pending_trim_split = false;
    auto clear_trim_preview = [&]() {
        if (trim_preview_chunk != static_cast<std::size_t>(-1)) {
            renderer.set_chunk_visible(trim_preview_chunk, false);
            trim_preview_chunk = static_cast<std::size_t>(-1);
        }
    };
    auto update_trim_preview = [&](const map::SplineInst& s, float along) {
        const float L = (std::max)(s.length, 0.5f);
        along = (std::max)(0.45f, (std::min)(along, L - 0.45f));
        const int ncuts = (std::max)(1, panel.imgui_state().spline_cut_count);
        // Always keep trim_cuts size in sync; seed evenly only when count changes
        // so the line never "sticks" to a stale position after changing Cuts.
        if (static_cast<int>(trim_cuts.size()) != ncuts) {
            trim_cuts.assign(static_cast<std::size_t>(ncuts), 0.f);
            for (int i = 0; i < ncuts; ++i)
                trim_cuts[static_cast<std::size_t>(i)] =
                    L * static_cast<float>(i + 1) / static_cast<float>(ncuts + 1);
        }
        int best = 0;
        float bd = 1.e9f;
        for (int i = 0; i < ncuts; ++i) {
            const float d = std::fabs(trim_cuts[static_cast<std::size_t>(i)] - along);
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
        // With a single cut, follow the mouse freely (no neighbour clamps that pin it).
        float lo = 0.45f;
        float hi = L - 0.45f;
        if (ncuts > 1) {
            lo = (best == 0) ? 0.45f : trim_cuts[static_cast<std::size_t>(best - 1)] + 0.35f;
            hi = (best == ncuts - 1) ? L - 0.45f : trim_cuts[static_cast<std::size_t>(best + 1)] - 0.35f;
            if (hi < lo) hi = lo;
        }
        trim_cuts[static_cast<std::size_t>(best)] = (std::max)(lo, (std::min)(along, hi));
        trim_along = trim_cuts[static_cast<std::size_t>(best)];
        float lat0 = -2.5f, lat1 = 2.5f, h_off = 0.12f;
        if (const auto def = spline::load_sli(s.sli_path)) {
            lat0 = 1.e6f;
            lat1 = -1.e6f;
            float zmax = 0.f;
            for (const auto& pr : def->profiles) {
                for (const auto& p : pr) {
                    lat0 = (std::min)(lat0, p.x);
                    lat1 = (std::max)(lat1, p.x);
                    zmax = (std::max)(zmax, p.z);
                }
            }
            if (lat1 <= lat0) {
                lat0 = -2.5f;
                lat1 = 2.5f;
            }
            h_off = zmax + 0.08f;
        }
        std::vector<o3d::Vertex> verts;
        std::vector<std::uint32_t> inds;
        const float ds = 0.04f;
        constexpr int kLat = 10;
        auto push = [&](const spline::SplineSample& p) {
            o3d::Vertex v{};
            v.position[0] = p.x;
            v.position[1] = p.y;
            v.position[2] = p.z;
            v.normal[2] = 1.f;
            verts.push_back(v);
        };
        for (float al : trim_cuts) {
            al = (std::max)(ds, (std::min)(al, L - ds));
            const auto base = static_cast<std::uint32_t>(verts.size());
            for (int k = 0; k <= kLat; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(kLat);
                const float lat = lat0 + (lat1 - lat0) * t;
                push(spline::evaluate_spline(s, al - ds, lat, h_off));
                push(spline::evaluate_spline(s, al + ds, lat, h_off));
            }
            for (int k = 0; k < kLat; ++k) {
                const auto i0 = base + static_cast<std::uint32_t>(k * 2);
                inds.insert(inds.end(), {i0, i0 + 2, i0 + 1, i0 + 1, i0 + 2, i0 + 3});
            }
        }
        if (trim_preview_chunk == static_cast<std::size_t>(-1)) {
            if (!renderer.append_textured_mesh(verts.data(), static_cast<std::uint32_t>(verts.size()),
                                              inds.data(), static_cast<std::uint32_t>(inds.size()), {}, 0u))
                return;
            trim_preview_chunk = renderer.chunk_count() - 1;
        } else {
            renderer.replace_textured_mesh(trim_preview_chunk, verts.data(),
                                          static_cast<std::uint32_t>(verts.size()), inds.data(),
                                          static_cast<std::uint32_t>(inds.size()), {}, 0u);
        }
        renderer.set_chunk_visible(trim_preview_chunk, true);
        renderer.set_chunk_highlight(trim_preview_chunk, 1.f, 1.f, 0.35f, 0.05f);
        const auto wt = store_world_transposed(XMMatrixIdentity());
        renderer.set_chunk_world_matrix(trim_preview_chunk, wt.data());
    };
    auto clear_marquee_box = [&]() {
        renderer.set_screen_rect_overlay(0.f, 0.f, 0.f, 0.f, 1.f, 1.f, false);
        for (const std::size_t c : marquee.box_chunks) renderer.set_chunk_visible(c, false);
        marquee.box_chunks.clear();
    };
    auto world_to_screen = [&](float wx, float wy, float wz, float mx_ref, float my_ref, float fw, float fh,
                                 const float* wvp_t, float& sx, float& sy) -> bool {
        (void)mx_ref;
        (void)my_ref;
        DirectX::XMFLOAT4X4 wvp_t_m{};
        std::memcpy(&wvp_t_m, wvp_t, sizeof(wvp_t_m));
        const DirectX::XMMATRIX wvp = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&wvp_t_m));
        DirectX::XMFLOAT4 clip{};
        DirectX::XMStoreFloat4(&clip,
                               DirectX::XMVector4Transform(DirectX::XMVectorSet(wx, wy, wz, 1.f), wvp));
        if (clip.w <= 1.e-5f) return false;
        sx = (clip.x / clip.w * 0.5f + 0.5f) * fw;
        sy = (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * fh;
        return true;
    };
    // Ring / square draped on the terrain (follows the relief, like the terrain brush cursor).
    auto draw_ground_ring = [&](float cx, float cy, float radius, bool square, ImU32 col, ImU32 fill) {
        float mx_ = 0.f, my_ = 0.f, fw_ = 1.f, fh_ = 1.f;
        input::cursor_to_framebuffer(window, mx_, my_, fw_, fh_);
        const float asp_ = (fh_ > 0.f) ? fw_ / fh_ : 16.f / 9.f;
        float wvp_[16]{};
        camera.build_world_view_proj(asp_, wvp_);
        const ImVec2 disp_ = ImGui::GetIO().DisplaySize;
        const float kx_ = disp_.x / (std::max)(1.f, fw_), ky_ = disp_.y / (std::max)(1.f, fh_);
        constexpr int kSeg = 72;
        ImVec2 pts[kSeg];
        for (int i = 0; i < kSeg; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSeg);
            float ox, oy;
            if (square) {
                const float u = t * 4.f;
                const int side = static_cast<int>(u);
                const float f = (u - static_cast<float>(side)) * 2.f - 1.f;
                switch (side & 3) {
                case 0: ox = f; oy = -1.f; break;
                case 1: ox = 1.f; oy = f; break;
                case 2: ox = -f; oy = 1.f; break;
                default: ox = -1.f; oy = -f; break;
                }
                ox *= radius; oy *= radius;
            } else {
                ox = std::cos(t * 6.2831853f) * radius;
                oy = std::sin(t * 6.2831853f) * radius;
            }
            const float wx = cx + ox, wy = cy + oy;
            float sx = 0.f, sy = 0.f;
            if (!world_to_screen(wx, wy, sample_terrain_editor_xy(wx, wy) + 0.12f, 0.f, 0.f, fw_, fh_, wvp_, sx, sy)) return;
            pts[i] = ImVec2(sx * kx_, sy * ky_);
        }
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        if (fill & 0xFF000000u) fg->AddConvexPolyFilled(pts, kSeg, fill);
        fg->AddPolyline(pts, kSeg, IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, 3.2f);
        fg->AddPolyline(pts, kSeg, col, ImDrawFlags_Closed, 1.8f);
    };
    auto update_marquee_box = [&](float fw, float fh, const float* /*wvp_t*/) {
        if (marquee.phase != MarqueePhase::Select || !marquee.dragging) {
            clear_marquee_box();
            return;
        }
        // Only Recuadro uses the GPU rect overlay. Lazo/Círculo draw via ImGui (no fake square).
        if (sel_shape != SelShapeMode::Rect) {
            clear_marquee_box();
            return;
        }
        // DX12 flip swapchain: GDI overlays are invisible — draw NDC rect in the GPU path.
        renderer.set_screen_rect_overlay(marquee.sx0, marquee.sy0, marquee.sx1, marquee.sy1, fw, fh, true);
    };
    auto draw_marquee_screen_overlay = [&](GLFWwindow* /*win*/) {};
    GLFWcursor* hand_cursor = glfwCreateStandardCursor(GLFW_HAND_CURSOR);

    auto object_place_radius = [&](std::int32_t oid) -> float {
        for (const auto& sp : scene_places) {
            if (sp.alive && sp.object_id == oid) return (std::max)(0.5f, sp.half_xy);
        }
        return 2.f;
    };

    auto fire_place_asmr = [&](editor::PlaceFxKind kind, float x, float y, float z, float radius,
                               std::int32_t object_id, editor::SfxCue cue) {
        place_fx.enabled = panel.imgui_state().place_feedback;
        editor_sfx.set_enabled(panel.imgui_state().place_feedback);
        editor_sfx.set_volume(panel.imgui_state().place_feedback_volume / 100.f);
        if (!panel.imgui_state().place_feedback) return;
        place_fx.spawn(kind, x, y, z, radius, object_id);
        float pan = 0.f;
        float ex = 0.f, ey = 0.f, ez = 0.f;
        camera.eye_position(ex, ey, ez);
        pan = std::clamp((x - ex) * 0.0025f, -0.4f, 0.4f);
        editor_sfx.play(cue, pan);
    };

    struct EditorSnapshot {
        std::vector<map::MapObjectInst> objects;
        std::vector<map::SplineInst> splines;
        std::vector<map::MapObjectInst> transient_objects;
        std::vector<map::SplineInst> transient_splines;
        std::vector<map::MapAttachObjectInst> attachments;
        std::vector<EditSplineAttachmentEntry> spline_attachments;
        std::unordered_set<std::int32_t> spline_attach_spawn_ids;
        std::vector<std::int32_t> group_obj_ids;
        std::vector<std::int32_t> group_spl_ids;
        std::unordered_set<std::int32_t> deleted_object_ids;
        std::unordered_set<std::int32_t> deleted_spline_ids;
        std::string action;
        // IDs the user was editing when this snapshot was pushed (selection/group).
        // On undo/redo we ONLY remesh these (+ true add/remove). Never the whole map.
        std::vector<std::int32_t> focus_spline_ids;
        std::vector<std::int32_t> focus_object_ids;
        bool sco_path_active = false;
        bool sco_traffic_open = false;
        std::int32_t sco_path_oid = 0;
        std::filesystem::path sco_path_file;
        std::vector<sco::ScoPath> sco_paths;
        sco::ScoTrafficLightsGroup sco_tl;
        bool sco_has_tl = false;
        int sco_path_sel = -1;
        int sco_tl_sel = 0;
    };
    std::int32_t grad_slider_undo_id = 0;
    auto capture_editor_snapshot = [&]() -> EditorSnapshot {
        EditorSnapshot snap{};
        snap.objects = edit_objects;
        snap.splines = edit_splines;
        snap.transient_objects = transient_objects;
        snap.transient_splines = transient_splines;
        snap.attachments = edit_attachments;
        snap.spline_attachments = edit_spline_attachments;
        snap.spline_attach_spawn_ids = spline_attach_spawn_ids;
        snap.group_obj_ids = group_sel_obj_ids;
        snap.group_spl_ids = group_sel_spl_ids;
        snap.deleted_object_ids = editor_deleted_object_ids;
        snap.deleted_spline_ids = editor_deleted_spline_ids;
        snap.sco_path_active = sco_path_edit.active;
        snap.sco_traffic_open = sco_path_edit.traffic_open;
        snap.sco_path_oid = sco_path_edit.object_id;
        snap.sco_path_file = sco_path_edit.sco_path;
        snap.sco_paths = sco_path_edit.paths;
        snap.sco_tl = sco_path_edit.tl;
        snap.sco_has_tl = sco_path_edit.has_tl;
        snap.sco_path_sel = sco_path_edit.sel;
        snap.sco_tl_sel = sco_path_edit.tl_sel;
        return snap;
    };
    auto spline_geom_equal = [](const map::SplineInst& a, const map::SplineInst& b) {
        // Mesh-relevant fields + chain links (prev/next affect chain_z / strip join).
        // Path compare by filename (absolute vs relative must not force remesh).
        auto approx_eq = [](float x, float y) { return std::fabs(x - y) < 1.e-2f; };
        auto heading_eq = [](float x, float y) {
            float d = std::fabs(x - y);
            while (d > 180.f) d = std::fabs(d - 360.f);
            return d < 1.e-2f;
        };
        if (a.id != b.id || a.mirror != b.mirror) return false;
        if (a.use_delta_height != b.use_delta_height || a.is_spline_h != b.is_spline_h) return false;
        if (a.sli_path.filename() != b.sli_path.filename()) return false;
        // Links: fill-chain temps change prev/next without always moving pos —
        // undo must remesh those "original-looking" neighbors too.
        if (a.prev_id != b.prev_id || a.next_id != b.next_id) return false;
        return approx_eq(a.position.x, b.position.x) && approx_eq(a.position.y, b.position.y) &&
               approx_eq(a.position.z, b.position.z) && heading_eq(a.heading_deg, b.heading_deg) &&
               approx_eq(a.length, b.length) && approx_eq(a.radius, b.radius) &&
               approx_eq(a.delta_height, b.delta_height) && approx_eq(a.start_grad, b.start_grad) &&
               approx_eq(a.end_grad, b.end_grad) && approx_eq(a.cant_start, b.cant_start) &&
               approx_eq(a.cant_end, b.cant_end) && approx_eq(a.skew_start, b.skew_start) &&
               approx_eq(a.skew_end, b.skew_end);
    };
    auto object_geom_equal = [](const map::MapObjectInst& a, const map::MapObjectInst& b) {
        auto approx_eq = [](float x, float y) { return std::fabs(x - y) < 1.e-3f; };
        return a.id == b.id && a.sco_path == b.sco_path && a.varparent_id == b.varparent_id &&
               approx_eq(a.position.x, b.position.x) && approx_eq(a.position.y, b.position.y) &&
               approx_eq(a.position.z, b.position.z) && approx_eq(a.rotation_deg.x, b.rotation_deg.x) &&
               approx_eq(a.rotation_deg.y, b.rotation_deg.y) && approx_eq(a.rotation_deg.z, b.rotation_deg.z);
    };
    std::vector<std::int32_t> undo_remesh_queue; // amortized post-undo GPU rebuilds
    struct CatalogReloadQueue {
        std::vector<std::int32_t> object_ids;
        std::vector<std::int32_t> spline_ids;
        std::filesystem::path sco_for_sa;
        bool pending_sa_respawn = false;
        bool pending_end_sync = false;
        std::string status_label;
    } catalog_reload_q;
    auto pump_catalog_reload_queue = [&](int budget) {
        if (catalog_reload_q.object_ids.empty() && catalog_reload_q.spline_ids.empty()) {
            if (catalog_reload_q.pending_end_sync) {
                catalog_reload_q.pending_end_sync = false;
                sync_all_end_markers();
                if (!catalog_reload_q.status_label.empty()) {
                    panel.set_status(catalog_reload_q.status_label);
                    catalog_reload_q.status_label.clear();
                }
            }
            return;
        }
        int done = 0;
        while (!catalog_reload_q.object_ids.empty() && done < budget) {
            const std::int32_t oid = catalog_reload_q.object_ids.back();
            catalog_reload_q.object_ids.pop_back();
            for (auto& o : edit_objects) {
                if (o.id != oid) continue;
                sync_object_gpu(o);
                ++done;
                break;
            }
        }
        while (!catalog_reload_q.spline_ids.empty() && done < budget) {
            const std::int32_t sid = catalog_reload_q.spline_ids.back();
            catalog_reload_q.spline_ids.pop_back();
            for (auto& s : edit_splines) {
                if (s.id != sid) continue;
                remesh_spline_one(s, false, false);
                ++done;
                break;
            }
        }
        if (catalog_reload_q.object_ids.empty() && catalog_reload_q.spline_ids.empty()) {
            if (catalog_reload_q.pending_sa_respawn && !catalog_reload_q.sco_for_sa.empty()) {
                request_sa_respawn_for_sco(catalog_reload_q.sco_for_sa);
                refresh_editor_lists();
                catalog_reload_q.pending_sa_respawn = false;
                catalog_reload_q.sco_for_sa.clear();
            }
            if (catalog_reload_q.pending_end_sync) {
                catalog_reload_q.pending_end_sync = false;
                sync_all_end_markers();
                if (!catalog_reload_q.status_label.empty()) {
                    panel.set_status(catalog_reload_q.status_label);
                    catalog_reload_q.status_label.clear();
                }
            }
        } else {
            const std::size_t left = catalog_reload_q.object_ids.size() + catalog_reload_q.spline_ids.size();
            panel.set_status("Recargando en cola: " + std::to_string(left));
        }
    };
    auto pump_undo_remesh_queue = [&](int budget) {
        int done = 0;
        while (!undo_remesh_queue.empty() && done < budget) {
            const std::int32_t id = undo_remesh_queue.front();
            undo_remesh_queue.erase(undo_remesh_queue.begin());
            map::SplineInst* sp = nullptr;
            for (auto& s : edit_splines) {
                if (s.id == id) {
                    sp = &s;
                    break;
                }
            }
            if (!sp) continue;
            auto* g = find_spline_gpu(id);
            const bool has_gpu = g && g->alive && !g->chunks.empty();
            const bool in_rad = visual_in_radius_spline(*sp);
            if (!has_gpu && !in_rad) continue;
            remesh_spline_one(*sp, false, false);
            ++done;
        }
    };
    auto apply_editor_snapshot = [&](EditorSnapshot snap) {
        const auto t0 = std::chrono::steady_clock::now();
        undo_remesh_queue.clear(); // drop stale amortized work from prior undo
        async_remesh_invalidate_all(); // results of the pre-undo shapes must not be applied
        // Nota: fill_temp_ids/pool se limpian en el caller (loop_02 Ctrl+Z) porque
        // esas vars se declaran más abajo (mod_spline_chain_handles). Aquí el
        // snapshot ya restaura prev/next correctos y oculta GPU de ids removidos.
        editor_log::event("SNAP_APPLY",
                          "obj=" + std::to_string(snap.objects.size()) + " spl=" + std::to_string(snap.splines.size()) +
                              " tr_obj=" + std::to_string(snap.transient_objects.size()) + " tr_spl=" +
                              std::to_string(snap.transient_splines.size()) + " grp_obj=" +
                              std::to_string(snap.group_obj_ids.size()) + " grp_spl=" +
                              std::to_string(snap.group_spl_ids.size()));
        std::unordered_map<std::int32_t, map::SplineInst> before_s;
        std::unordered_map<std::int32_t, map::MapObjectInst> before_o;
        before_s.reserve(edit_splines.size());
        before_o.reserve(edit_objects.size());
        for (const auto& s : edit_splines) before_s.emplace(s.id, s);
        for (const auto& o : edit_objects) before_o.emplace(o.id, o);

        edit_objects = std::move(snap.objects);
        xform_soa.rebuild_from(edit_objects);
        edit_obj_index_size = static_cast<std::size_t>(-1); // cached id -> object pointers died with the old vector
        edit_splines = std::move(snap.splines);
        transient_objects = std::move(snap.transient_objects);
        transient_splines = std::move(snap.transient_splines);
        edit_attachments = std::move(snap.attachments);
        edit_spline_attachments = std::move(snap.spline_attachments);
        spline_attach_spawn_ids = std::move(snap.spline_attach_spawn_ids);
        sco_path_edit.active = snap.sco_path_active;
        sco_path_edit.traffic_open = snap.sco_traffic_open;
        sco_path_edit.object_id = snap.sco_path_oid;
        sco_path_edit.sco_path = snap.sco_path_file;
        sco_path_edit.paths = std::move(snap.sco_paths);
        sco_path_edit.tl = std::move(snap.sco_tl);
        sco_path_edit.has_tl = snap.sco_has_tl;
        sco_path_edit.sel = snap.sco_path_sel;
        sco_path_edit.tl_sel = snap.sco_tl_sel;
        sco_path_edit.dirty = sco_path_edit.active;
        sco_path_edit.moving = sco_path_edit.rotating = sco_path_edit.following = sco_path_edit.placing = false;
        if (sco_path_edit.active)
            rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
        // Rebuild instance_id → entry index map (not stored; derived).
        spline_attach_obj_to_entry.clear();
        for (std::size_t ei = 0; ei < edit_spline_attachments.size(); ++ei) {
            const auto iid = edit_spline_attachments[ei].att.instance_id;
            if (iid != 0) spline_attach_obj_to_entry[iid] = ei;
        }

        std::unordered_set<std::int32_t> keep_s;
        std::unordered_set<std::int32_t> keep_o;
        keep_s.reserve(edit_splines.size());
        keep_o.reserve(edit_objects.size());
        for (const auto& s : edit_splines) keep_s.insert(s.id);
        for (const auto& o : edit_objects) keep_o.insert(o.id);

        for (auto& sp : scene_places) {
            if (!sp.alive) continue;
            if (!keep_o.count(sp.object_id)) {
                sp.alive = false;
                sp.pinned = false;
                renderer.set_chunk_visible(sp.chunk, false);
            } else {
                sp.pinned = false;
            }
        }
        for (auto& g : spline_gpu) {
            if (!keep_s.count(g.id)) {
                g.alive = false;
                g.pinned = false;
                for (const auto c : g.chunks) renderer.set_chunk_visible(c, false);
            } else {
                g.pinned = false;
            }
        }
        for (auto& mk : end_markers) {
            const bool ok = mk.from_object ? keep_o.count(mk.owner_id) != 0 : keep_s.count(mk.owner_id) != 0;
            if (!ok) {
                mk.alive = false;
                renderer.set_chunk_visible(mk.chunk, false);
            } else {
                mk.alive = true;
            }
        }

        // SURGICAL undo: restore full data above, but ONLY remesh focus IDs from the
        // snapshot (what the user was editing when push_undo ran) plus true add/remove.
        // Never walk all 2750 splines into remesh_spline_one — that was the 5–6s freeze.
        int n_remesh = 0, n_reuse = 0, n_hide = 0, n_defer = 0;
        std::unordered_set<std::int32_t> focus_s(snap.focus_spline_ids.begin(), snap.focus_spline_ids.end());
        std::unordered_set<std::int32_t> focus_o(snap.focus_object_ids.begin(), snap.focus_object_ids.end());
        // True membership changes always need GPU work regardless of focus.
        for (const auto& s : edit_splines) {
            if (!before_s.count(s.id)) focus_s.insert(s.id); // added by this apply
        }
        for (const auto& kv : before_s) {
            if (!keep_s.count(kv.first)) {
                focus_s.insert(kv.first); // removed
                ++n_hide;
            }
        }
        for (const auto& o : edit_objects) {
            if (!before_o.count(o.id)) focus_o.insert(o.id);
        }
        for (const auto& kv : before_o) {
            if (!keep_o.count(kv.first)) focus_o.insert(kv.first);
        }
        // Always expand focus with real geom diffs (prev/next adapted by stick/fill
        // may be missing from the original focus seed). Budget still caps remesh.
        {
            for (const auto& s : edit_splines) {
                const auto it = before_s.find(s.id);
                if (it == before_s.end() || !spline_geom_equal(it->second, s)) focus_s.insert(s.id);
            }
            for (const auto& o : edit_objects) {
                const auto it = before_o.find(o.id);
                if (it == before_o.end() || !object_geom_equal(it->second, o)) focus_o.insert(o.id);
            }
        }
        // Expand focus along restored chain links so neighbors that only lost a
        // temp (or regained original prev/next) still remesh and rejoin cleanly.
        {
            std::vector<std::int32_t> seeds(focus_s.begin(), focus_s.end());
            for (std::int32_t sid : seeds) {
                // walk forward
                std::int32_t cur = sid;
                for (int hop = 0; hop < 64 && cur != 0; ++hop) {
                    focus_s.insert(cur);
                    map::SplineInst* sp = nullptr;
                    for (auto& x : edit_splines) {
                        if (x.id == cur) {
                            sp = &x;
                            break;
                        }
                    }
                    if (!sp || sp->next_id == 0) break;
                    cur = sp->next_id;
                }
                // walk backward
                cur = sid;
                for (int hop = 0; hop < 64 && cur != 0; ++hop) {
                    focus_s.insert(cur);
                    map::SplineInst* sp = nullptr;
                    for (auto& x : edit_splines) {
                        if (x.id == cur) {
                            sp = &x;
                            break;
                        }
                    }
                    if (!sp || sp->prev_id == 0) break;
                    cur = sp->prev_id;
                }
            }
        }
        // Splines: reuse GPU for everything not in focus; remesh only focus that actually changed.
        for (auto& s : edit_splines) {
            auto* g = find_spline_gpu(s.id);
            const bool has_gpu = g && g->alive && !g->chunks.empty();
            const bool in_focus = focus_s.count(s.id) != 0;
            if (!in_focus) {
                // Leave GPU as-is. Forcing visible=true re-showed hidden items and
                // orphaned strips after replace_sli undo until tile stream reload.
                ++n_reuse;
                continue;
            }
            const auto it = before_s.find(s.id);
            const bool geom_same = (it != before_s.end()) && spline_geom_equal(it->second, s);
            if (geom_same && has_gpu) {
                for (const auto c : g->chunks) renderer.set_chunk_visible(c, true);
                ++n_reuse;
                continue;
            }
            if (geom_same && !has_gpu) {
                ++n_defer;
                continue;
            }
            // Geometry changed for a focused spline — remesh this one only.
            // Hard budget: even a broken focus fallback cannot freeze the UI.
            if (n_remesh >= 48) {
                undo_remesh_queue.push_back(s.id);
                ++n_defer;
                continue;
            }
            if (g) {
                const bool sli_chg = (it == before_s.end()) ||
                                    (it->second.sli_path.filename() != s.sli_path.filename());
                for (const auto c : g->chunks) renderer.set_chunk_visible(c, false);
                if (sli_chg) g->chunks.clear();
            }
            remesh_spline_one(s, false, false);
            ++n_remesh;
        }
        // Hide GPU for removed focused splines (already counted in n_hide)
        for (const auto& kv : before_s) {
            if (keep_s.count(kv.first)) continue;
            if (auto* g = find_spline_gpu(kv.first)) {
                g->alive = false;
                for (const auto c : g->chunks) renderer.set_chunk_visible(c, false);
            }
        }
        for (auto& o : edit_objects) {
            const auto it = before_o.find(o.id);
            const bool in_focus = focus_o.count(o.id) != 0;
            if (!in_focus) {
                // Unrelated object — leave GPU alone (avoids 0,0,0 ghost revives).
                continue;
            }
            if (it != before_o.end() && object_geom_equal(it->second, o)) {
                for (auto& sp : scene_places) {
                    if (sp.object_id == o.id && sp.alive)
                        renderer.set_chunk_visible(sp.chunk, true);
                }
                continue;
            }
            sync_object_gpu(o);
        }
        sync_all_end_markers();
        for (auto& o : edit_objects) upsert_transient_object(o);
        for (auto& s : edit_splines) upsert_transient_spline(s);
        // Path rails full rebuild is expensive; only when object set/geom likely changed
        if (show_path_rails) {
            bool obj_changed = before_o.size() != edit_objects.size();
            if (!obj_changed) {
                for (const auto& o : edit_objects) {
                    const auto it = before_o.find(o.id);
                    if (it == before_o.end() || !object_geom_equal(it->second, o)) {
                        obj_changed = true;
                        break;
                    }
                }
            }
            // Only the owners that were added / removed / really changed get new rails (the periodic
            // signature check in pump_path_rails_dirty picks up geometry edits on its own).
            for (const auto& o : edit_objects)
                if (!before_o.count(o.id)) mark_path_rails_dirty(o.id, true);
            for (const auto& kv : before_o)
                if (!keep_o.count(kv.first)) purge_path_rails_for_owner(kv.first, true);
            for (const auto& s2 : edit_splines)
                if (!before_s.count(s2.id)) mark_path_rails_dirty_spline_id(s2.id);
            for (const auto& kv : before_s)
                if (!keep_s.count(kv.first)) purge_path_rails_for_owner(kv.first, false);
            (void)obj_changed;
        }
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
            log::info("[undo] apply_editor_snapshot " + std::to_string(ms) + " ms spl=" +
                      std::to_string(edit_splines.size()) + " obj=" + std::to_string(edit_objects.size()) +
                      " remesh=" + std::to_string(n_remesh) + " reuse=" + std::to_string(n_reuse) +
                      " defer=" + std::to_string(n_defer) + " hide=" + std::to_string(n_hide) +
                      " focus_s=" + std::to_string(focus_s.size()) + " focus_o=" + std::to_string(focus_o.size()));
            editor_log::event("SNAP_APPLY_MS", std::to_string(ms) + " remesh=" + std::to_string(n_remesh) +
                                                  " reuse=" + std::to_string(n_reuse) +
                                                  " defer=" + std::to_string(n_defer) +
                                                  " focus_s=" + std::to_string(focus_s.size()));
        }
        selected_spline = -1;
        selected_object = -1;
        group_sel_obj_ids = std::move(snap.group_obj_ids);
        group_sel_spl_ids = std::move(snap.group_spl_ids);
        editor_deleted_object_ids = std::move(snap.deleted_object_ids);
        editor_deleted_spline_ids = std::move(snap.deleted_spline_ids);
        group_sel_obj.clear();
        group_sel_spl.clear();
        for (const auto want : group_sel_obj_ids) {
            for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                if (edit_objects[i].id == want) {
                    group_sel_obj.push_back(static_cast<int>(i));
                    break;
                }
            }
