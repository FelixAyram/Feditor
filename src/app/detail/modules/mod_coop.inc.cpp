// ---- Co-op editing: edit sync, avatars, pedestrian mode, session / friends / chat windows ----
// Included inside run_map_editor_app (setup_06). Network + profile live in omsi::coop::Session
// (src/net/coop_session.cpp), which survives map reloads.
    auto& coop = omsi::coop::Session::get();
    bool coop_walk_active = false;
    bool coop_walk_edit = false;   // F7 walk-edit: the crosshair picks / places as the mouse does in the editor
    bool coop_walk_cursor = false; // E: mouse cursor for the interface, the view stops turning

    // ===== tile-local <-> editor coordinates (each peer has its own editor origin) =====
    auto coop_to_tile = [&](float ex, float ey, std::int32_t& gx, std::int32_t& gy, float& lx, float& ly) {
        gx = gy = 0;
        if (auto* t = find_tile_mut_xy(ex, ey)) {
            gx = t->grid_x;
            gy = t->grid_y;
        }
        const auto [rx, ry] = tile_rel(gx, gy);
        lx = ex - rx;
        ly = ey - ry;
    };
    auto coop_from_tile = [&](std::int32_t gx, std::int32_t gy, float lx, float ly, float& ex, float& ey) {
        const auto [rx, ry] = tile_rel(gx, gy);
        ex = lx + rx;
        ey = ly + ry;
    };
    auto coop_rel = [&](const std::filesystem::path& p) -> std::string {
        std::error_code ec;
        auto r = std::filesystem::relative(p, cfg.omsi_root, ec);
        // UTF-8 on the wire (path_text gives ANSI when it fits: invalid UTF-8 for the receiver)
        std::string s = omsi::wide_to_utf8((ec || r.empty() ? p : r).native());
        for (char& c : s)
            if (c == '/') c = '\\';
        return s;
    };
    auto coop_abs = [&](const std::string& rel) -> std::filesystem::path {
        const std::filesystem::path p(omsi::utf8_to_wide(rel)); // never throws (u8path does on bad bytes)
        return p.is_absolute() ? p : cfg.omsi_root / p;
    };

    // ===== serialization (unit separator 0x1f between fields) =====
    constexpr char kUS = '\x1f';
    auto coop_ser_obj = [&](const map::MapObjectInst& o) -> std::string {
        std::int32_t gx = 0, gy = 0;
        float lx = 0.f, ly = 0.f;
        coop_to_tile(o.position.x, o.position.y, gx, gy, lx, ly);
        std::ostringstream s;
        s.precision(9);
        s << coop_rel(o.sco_path) << kUS << gx << kUS << gy << kUS << lx << kUS << ly << kUS << o.position.z << kUS
          << o.rotation_deg.x << kUS << o.rotation_deg.y << kUS << o.rotation_deg.z << kUS << o.flags << kUS
          << o.obj_type << kUS << o.tree_texture << kUS << o.tree_height << kUS << o.tree_aspect << kUS
          << o.varparent_id << kUS << (o.align_terrain ? 1 : 0) << kUS << o.strings.size();
        for (const auto& str : o.strings) s << kUS << str;
        return s.str();
    };
    auto coop_split = [&](const std::string& d) {
        std::vector<std::string> f;
        std::string cur;
        for (char c : d) {
            if (c == kUS) {
                f.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        f.push_back(cur);
        return f;
    };
    auto fnum = [](const std::string& s) { return std::strtof(s.c_str(), nullptr); };
    auto inum = [](const std::string& s) { return static_cast<std::int32_t>(std::strtol(s.c_str(), nullptr, 10)); };
    auto coop_deser_obj = [&](std::int32_t id, const std::string& d, map::MapObjectInst& o) -> bool {
        const auto f = coop_split(d);
        if (f.size() < 17) return false;
        o.id = id;
        o.sco_path = coop_abs(f[0]);
        float ex = 0.f, ey = 0.f;
        coop_from_tile(inum(f[1]), inum(f[2]), fnum(f[3]), fnum(f[4]), ex, ey);
        o.position.x = ex;
        o.position.y = ey;
        o.position.z = fnum(f[5]);
        o.rotation_deg.x = fnum(f[6]);
        o.rotation_deg.y = fnum(f[7]);
        o.rotation_deg.z = fnum(f[8]);
        o.flags = inum(f[9]);
        o.obj_type = inum(f[10]);
        o.tree_texture = f[11];
        o.tree_height = fnum(f[12]);
        o.tree_aspect = fnum(f[13]);
        o.varparent_id = inum(f[14]);
        o.align_terrain = inum(f[15]) != 0;
        const int n = inum(f[16]);
        o.strings.clear();
        for (int i = 0; i < n && 17 + static_cast<std::size_t>(i) < f.size(); ++i) o.strings.push_back(f[17 + static_cast<std::size_t>(i)]);
        o.string_line_count = static_cast<std::int32_t>(o.strings.size());
        return true;
    };
    auto coop_ser_spl = [&](const map::SplineInst& sp) -> std::string {
        std::int32_t gx = 0, gy = 0;
        float lx = 0.f, ly = 0.f;
        coop_to_tile(sp.position.x, sp.position.y, gx, gy, lx, ly);
        std::ostringstream s;
        s.precision(9);
        s << coop_rel(sp.sli_path) << kUS << gx << kUS << gy << kUS << lx << kUS << ly << kUS << sp.position.z << kUS
          << sp.heading_deg << kUS << sp.length << kUS << sp.radius << kUS << sp.start_grad << kUS << sp.end_grad << kUS
          << sp.delta_height << kUS << sp.cant_start << kUS << sp.cant_end << kUS << sp.skew_start << kUS
          << sp.skew_end << kUS << sp.flags << kUS << sp.prev_id << kUS << sp.next_id << kUS << (sp.mirror ? 1 : 0)
          << kUS << static_cast<int>(sp.terrain_align) << kUS << (sp.is_spline_h ? 1 : 0) << kUS
          << (sp.use_delta_height ? 1 : 0) << kUS << sp.local_length;
        return s.str();
    };
    auto coop_deser_spl = [&](std::int32_t id, const std::string& d, map::SplineInst& sp) -> bool {
        const auto f = coop_split(d);
        if (f.size() < 24) return false;
        sp.id = id;
        sp.sli_path = coop_abs(f[0]);
        float ex = 0.f, ey = 0.f;
        coop_from_tile(inum(f[1]), inum(f[2]), fnum(f[3]), fnum(f[4]), ex, ey);
        sp.position.x = ex;
        sp.position.y = ey;
        sp.position.z = fnum(f[5]);
        sp.heading_deg = fnum(f[6]);
        sp.length = (std::max)(0.01f, fnum(f[7]));
        sp.radius = fnum(f[8]);
        sp.start_grad = fnum(f[9]);
        sp.end_grad = fnum(f[10]);
        sp.delta_height = fnum(f[11]);
        sp.cant_start = fnum(f[12]);
        sp.cant_end = fnum(f[13]);
        sp.skew_start = fnum(f[14]);
        sp.skew_end = fnum(f[15]);
        sp.flags = inum(f[16]);
        sp.prev_id = inum(f[17]);
        sp.next_id = inum(f[18]);
        sp.mirror = inum(f[19]) != 0;
        sp.terrain_align = static_cast<std::uint8_t>(std::clamp(inum(f[20]), 0, 255));
        sp.is_spline_h = inum(f[21]) != 0;
        sp.use_delta_height = inum(f[22]) != 0;
        sp.local_length = fnum(f[23]);
        return true;
    };
    // Cheap change hashes straight from the fields (no string building, no tile lookup): the scan runs
    // over thousands of entries per frame. Serialization happens only for what changed.
    auto mixh = [](std::size_t h, std::size_t v) { return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)); };
    auto fh = [](float f) {
        std::uint32_t u;
        std::memcpy(&u, &f, 4);
        return static_cast<std::size_t>(u);
    };
    auto coop_obj_hash = [&](const map::MapObjectInst& o) {
        std::size_t h = std::hash<std::wstring>{}(o.sco_path.native());
        for (float f : {o.position.x, o.position.y, o.position.z, o.rotation_deg.x, o.rotation_deg.y, o.rotation_deg.z,
                        o.tree_height, o.tree_aspect})
            h = mixh(h, fh(f));
        h = mixh(h, static_cast<std::size_t>(o.flags) * 31u + static_cast<std::size_t>(o.obj_type));
        h = mixh(h, static_cast<std::size_t>(o.varparent_id) * 2u + (o.align_terrain ? 1u : 0u));
        for (const auto& str : o.strings) h = mixh(h, std::hash<std::string>{}(str));
        return h;
    };
    auto coop_spl_hash = [&](const map::SplineInst& sp) {
        std::size_t h = std::hash<std::wstring>{}(sp.sli_path.native());
        for (float f : {sp.position.x, sp.position.y, sp.position.z, sp.heading_deg, sp.length, sp.radius, sp.start_grad,
                        sp.end_grad, sp.delta_height, sp.cant_start, sp.cant_end, sp.skew_start, sp.skew_end,
                        sp.local_length})
            h = mixh(h, fh(f));
        h = mixh(h, static_cast<std::size_t>(sp.flags) * 7u + static_cast<std::size_t>(sp.terrain_align) +
                        (sp.mirror ? 1000u : 0u) + (sp.is_spline_h ? 2000u : 0u) + (sp.use_delta_height ? 4000u : 0u));
        h = mixh(h, static_cast<std::size_t>(static_cast<std::uint32_t>(sp.prev_id)) * 131u +
                        static_cast<std::size_t>(static_cast<std::uint32_t>(sp.next_id)));
        return h;
    };
    // Latest remote state per entity (0 obj / 1 spline, id) while its last author is someone else:
    // re-applied after a local undo / redo, so nobody can undo another user's change.
    std::map<std::pair<int, std::int32_t>, omsi::coop::MapOp> coop_remote_last;

    // ===== change detection (round robin, a few thousand per frame) =====
    std::unordered_map<std::int32_t, std::size_t> coop_obj_h, coop_spl_h;
    std::unordered_set<std::int32_t> coop_obj_del_done, coop_spl_del_done;
    std::int32_t coop_base_obj_max = 0, coop_base_spl_max = 0;
    bool coop_baselined = false;
    std::size_t coop_obj_cursor = 0, coop_spl_cursor = 0;
    omsi::coop::Role coop_prev_role = omsi::coop::Role::None;
    auto coop_is_mine_new = [&](std::int32_t id, bool spline) {
        std::int32_t lo = 1, hi = 0x7fffffff;
        coop.id_range(lo, hi);
        if (coop.role() == omsi::coop::Role::Guest) return id >= lo && id <= hi;
        return id > (spline ? coop_base_spl_max : coop_base_obj_max) && id < 100000000;
    };
    auto coop_scan_changes = [&]() {
        if (!coop_baselined) {
            coop_base_obj_max = coop_base_spl_max = 0;
            for (const auto& o : edit_objects) coop_base_obj_max = (std::max)(coop_base_obj_max, o.id);
            for (const auto& s : edit_splines) coop_base_spl_max = (std::max)(coop_base_spl_max, s.id);
            for (const auto& o : transient_objects) coop_base_obj_max = (std::max)(coop_base_obj_max, o.id);
            for (const auto& s : transient_splines) coop_base_spl_max = (std::max)(coop_base_spl_max, s.id);
            coop_obj_del_done.insert(editor_deleted_object_ids.begin(), editor_deleted_object_ids.end());
            coop_spl_del_done.insert(editor_deleted_spline_ids.begin(), editor_deleted_spline_ids.end());
            coop_baselined = true;
        }
        // the selection / group are checked every frame (that is what gets edited), the rest round robin
        const auto t0 = std::chrono::steady_clock::now();
        auto check_obj = [&](const map::MapObjectInst& o) {
            if (spline_attach_spawn_ids.count(o.id) || editor_deleted_object_ids.count(o.id)) return;
            const std::size_t h = coop_obj_hash(o);
            const auto it = coop_obj_h.find(o.id);
            if (it == coop_obj_h.end()) {
                coop_obj_h.emplace(o.id, h);
                if (!coop_is_mine_new(o.id, false)) return;
            } else if (it->second != h) {
                it->second = h;
            } else {
                return;
            }
            coop_remote_last.erase({0, o.id}); // now authored here
            coop.send_op(omsi::coop::OpKind::ObjUpsert, o.id, coop_ser_obj(o));
        };
        auto check_spl = [&](const map::SplineInst& sp) {
            if (editor_deleted_spline_ids.count(sp.id)) return;
            const std::size_t h = coop_spl_hash(sp);
            const auto it = coop_spl_h.find(sp.id);
            if (it == coop_spl_h.end()) {
                coop_spl_h.emplace(sp.id, h);
                if (!coop_is_mine_new(sp.id, true)) return;
            } else if (it->second != h) {
                it->second = h;
            } else {
                return;
            }
            coop_remote_last.erase({1, sp.id});
            coop.send_op(omsi::coop::OpKind::SplUpsert, sp.id, coop_ser_spl(sp));
        };
        if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
            check_obj(edit_objects[static_cast<std::size_t>(selected_object)]);
        for (int gi : group_sel_obj)
            if (gi >= 0 && static_cast<std::size_t>(gi) < edit_objects.size()) check_obj(edit_objects[static_cast<std::size_t>(gi)]);
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
            check_spl(edit_splines[static_cast<std::size_t>(selected_spline)]);
        for (int gi : group_sel_spl)
            if (gi >= 0 && static_cast<std::size_t>(gi) < edit_splines.size()) check_spl(edit_splines[static_cast<std::size_t>(gi)]);
        auto over = [&]() {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 0.7;
        };
        for (std::size_t n = 0; n < 1500 && n < edit_objects.size(); ++n) {
            if (coop_obj_cursor >= edit_objects.size()) coop_obj_cursor = 0;
            check_obj(edit_objects[coop_obj_cursor++]);
            if ((n & 127) == 127 && over()) break;
        }
        for (std::size_t n = 0; n < 1500 && n < edit_splines.size(); ++n) {
            if (coop_spl_cursor >= edit_splines.size()) coop_spl_cursor = 0;
            check_spl(edit_splines[coop_spl_cursor++]);
            if ((n & 127) == 127 && over()) break;
        }
        if (editor_deleted_object_ids.size() != coop_obj_del_done.size())
            for (const auto id : editor_deleted_object_ids)
                if (coop_obj_del_done.insert(id).second) {
                    coop_remote_last.erase({0, id});
                    coop.send_op(omsi::coop::OpKind::ObjDelete, id, {});
                }
        if (editor_deleted_spline_ids.size() != coop_spl_del_done.size())
            for (const auto id : editor_deleted_spline_ids)
                if (coop_spl_del_done.insert(id).second) {
                    coop_remote_last.erase({1, id});
                    coop.send_op(omsi::coop::OpKind::SplDelete, id, {});
                }
    };

    // ===== apply remote edits =====
    auto coop_drop_selection_if = [&](bool obj, std::size_t erased_index) {
        if (obj && selected_object >= 0 && static_cast<std::size_t>(selected_object) >= erased_index) selected_object = -1;
        if (!obj && selected_spline >= 0 && static_cast<std::size_t>(selected_spline) >= erased_index) selected_spline = -1;
    };
    auto coop_apply_op = [&](const omsi::coop::MapOp& op) {
        using K = omsi::coop::OpKind;
        {
            const int grp = (op.kind == K::ObjUpsert || op.kind == K::ObjDelete) ? 0 : 1;
            coop_remote_last[{grp, op.id}] = op;
        }
        if (op.kind == K::ObjUpsert) {
            map::MapObjectInst in{};
            if (!coop_deser_obj(op.id, op.data, in)) return;
            editor_deleted_object_ids.erase(op.id);
            clear_missing_asset(false, op.id);
            map::MapObjectInst* o = nullptr;
            for (auto& e : edit_objects)
                if (e.id == op.id) {
                    o = &e;
                    break;
                }
            if (o) {
                const bool new_sco = o->sco_path != in.sco_path;
                persist_editor_xy(o->position.x, o->position.y);
                const auto keep_rules = o->path_rules;
                *o = in;
                o->path_rules = keep_rules;
                if (new_sco) {
                    for (auto& sp : scene_places)
                        if (sp.object_id == op.id && sp.alive) {
                            sp.alive = false;
                            renderer.set_chunk_visible(sp.chunk, false);
                        }
                    spawn_editor_object_gpu(*o);
                } else {
                    sync_object_gpu(*o);
                }
            } else {
                edit_objects.push_back(in);
                transient_objects.push_back(in);
                spawn_editor_object_gpu(edit_objects.back());
            }
            persist_editor_xy(in.position.x, in.position.y);
            coop_obj_h[op.id] = coop_obj_hash(in);
        } else if (op.kind == K::ObjDelete) {
            coop_obj_del_done.insert(op.id);
            coop_obj_h.erase(op.id);
            clear_missing_asset(false, op.id);
            bool found = false;
            for (const auto& e : edit_objects)
                if (e.id == op.id) {
                    persist_editor_xy(e.position.x, e.position.y);
                    found = true;
                    break;
                }
            if (found) {
                // (mark_editor_object_deleted may erase spline-attachment objects: find the index afterwards)
                mark_editor_object_deleted(op.id);
                purge_path_rails_for_owner(op.id, true);
                for (auto& sp : scene_places) {
                    if (sp.object_id != op.id) continue;
                    sp.alive = false;
                    if (!sp.is_tree) renderer.set_chunk_visible(sp.chunk, false);
                }
                for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                    if (edit_objects[i].id != op.id) continue;
                    edit_objects.erase(edit_objects.begin() + static_cast<std::ptrdiff_t>(i));
                    coop_drop_selection_if(true, i);
                    break;
                }
                selected_object = (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                                      ? selected_object
                                      : -1;
                group_sel_obj.clear();
            }
            editor_deleted_object_ids.insert(op.id);
            transient_objects.erase(std::remove_if(transient_objects.begin(), transient_objects.end(),
                                                   [&](const map::MapObjectInst& o) { return o.id == op.id; }),
                                    transient_objects.end());
        } else if (op.kind == K::SplUpsert) {
            map::SplineInst in{};
            if (!coop_deser_spl(op.id, op.data, in)) return;
            editor_deleted_spline_ids.erase(op.id);
            clear_missing_asset(true, op.id);
            map::SplineInst* s = nullptr;
            for (auto& e : edit_splines)
                if (e.id == op.id) {
                    s = &e;
                    break;
                }
            if (s) {
                persist_editor_xy(s->position.x, s->position.y);
                const auto keep_rules = s->path_rules;
                *s = in;
                s->path_rules = keep_rules;
                remesh_spline(*s);
            } else {
                edit_splines.push_back(in);
                transient_splines.push_back(in);
                remesh_spline(edit_splines.back());
            }
            persist_editor_xy(in.position.x, in.position.y);
            coop_spl_h[op.id] = coop_spl_hash(in);
        } else if (op.kind == K::SplDelete) {
            coop_spl_del_done.insert(op.id);
            coop_spl_h.erase(op.id);
            clear_missing_asset(true, op.id);
            group_sel_spl.clear();
            for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                if (edit_splines[i].id != op.id) continue;
                persist_editor_xy(edit_splines[i].position.x, edit_splines[i].position.y);
                mark_editor_spline_deleted(op.id);
                purge_path_rails_for_owner(op.id, false);
                if (auto* g = find_spline_gpu(op.id)) {
                    g->alive = false;
                    for (const auto c : g->chunks) renderer.set_chunk_visible(c, false);
                }
                edit_splines.erase(edit_splines.begin() + static_cast<std::ptrdiff_t>(i));
                coop_drop_selection_if(false, i);
                break;
            }
            // spline attachments of the deleted spline may have removed objects
            if (selected_object >= 0 && static_cast<std::size_t>(selected_object) >= edit_objects.size()) selected_object = -1;
            group_sel_obj.clear();
            editor_deleted_spline_ids.insert(op.id);
            transient_splines.erase(std::remove_if(transient_splines.begin(), transient_splines.end(),
                                                   [&](const map::SplineInst& s2) { return s2.id == op.id; }),
                                    transient_splines.end());
        }
    };

    // ===== avatars =====
    // Simple humanoid (feet at the origin, +Y forward, Z up): body (user colour), head (skin), face (dark).
    struct AvMesh {
        std::vector<o3d::Vertex> v;
        std::vector<std::uint32_t> i;
    };
    auto av_box = [](AvMesh& m, float cx, float cy, float cz, float hx, float hy, float hz) {
        const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const float h[3] = {hx, hy, hz};
        for (int f = 0; f < 6; ++f) {
            const int ax = n[f][0] != 0.f ? 0 : n[f][1] != 0.f ? 1 : 2;
            const int u = (ax + 1) % 3, w = (ax + 2) % 3;
            const std::uint32_t b = static_cast<std::uint32_t>(m.v.size());
            const float cs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto& c : cs) {
                float p[3];
                p[ax] = n[f][ax] * h[ax];
                p[u] = c[0] * h[u];
                p[w] = c[1] * h[w];
                o3d::Vertex vx{};
                vx.position[0] = cx + p[0];
                vx.position[1] = cy + p[1];
                vx.position[2] = cz + p[2];
                vx.normal[0] = n[f][0];
                vx.normal[1] = n[f][1];
                vx.normal[2] = n[f][2];
                m.v.push_back(vx);
            }
            m.i.insert(m.i.end(), {b, b + 1, b + 2, b, b + 2, b + 3, b + 2, b + 1, b, b + 3, b + 2, b});
        }
    };
    auto av_sphere = [](AvMesh& m, float cx, float cy, float cz, float rx, float ry, float rz) {
        constexpr int kLat = 12, kLon = 18;
        const std::uint32_t b = static_cast<std::uint32_t>(m.v.size());
        for (int a = 0; a <= kLat; ++a) {
            const float th = 3.14159265f * static_cast<float>(a) / kLat;
            for (int o = 0; o <= kLon; ++o) {
                const float ph = 6.2831853f * static_cast<float>(o) / kLon;
                const float nx = std::sin(th) * std::cos(ph), ny = std::sin(th) * std::sin(ph), nz = std::cos(th);
                o3d::Vertex vx{};
                vx.position[0] = cx + nx * rx;
                vx.position[1] = cy + ny * ry;
                vx.position[2] = cz + nz * rz;
                vx.normal[0] = nx;
                vx.normal[1] = ny;
                vx.normal[2] = nz;
                m.v.push_back(vx);
            }
        }
        for (int a = 0; a < kLat; ++a)
            for (int o = 0; o < kLon; ++o) {
                const std::uint32_t p0 = b + static_cast<std::uint32_t>(a * (kLon + 1) + o);
                const std::uint32_t p1 = p0 + kLon + 1;
                m.i.insert(m.i.end(), {p0, p1, p0 + 1, p0 + 1, p1, p1 + 1, p0 + 1, p1, p0, p1 + 1, p1, p0 + 1});
            }
    };
    // Articulated figure: every part is its own chunk, built around its joint (pivot at the origin)
    // so walking / jumping only changes per-part matrices. Feet at z 0, +Y forward, X right.
    constexpr int kAvParts = 10;
    enum AvPart { AvTorso = 0, AvHead, AvThighL, AvThighR, AvShinL, AvShinR, AvArmL, AvArmR, AvTie, AvCase };
    constexpr float kAvHipZ = 0.93f, kAvHipX = 0.095f, kAvKnee = 0.44f;
    constexpr float kAvShoulderX = 0.235f, kAvShoulderZ = 0.50f, kAvNeckZ = 0.62f; // torso-local
    AvMesh av_parts[kAvParts];
    {
        // torso (origin: pelvis)
        av_sphere(av_parts[AvTorso], 0.f, 0.f, 0.05f, 0.17f, 0.12f, 0.13f);  // hips
        av_sphere(av_parts[AvTorso], 0.f, 0.f, 0.32f, 0.20f, 0.13f, 0.30f);  // chest
        av_sphere(av_parts[AvTorso], 0.f, 0.f, 0.60f, 0.06f, 0.06f, 0.07f);  // neck
        // head (origin: neck joint)
        av_sphere(av_parts[AvHead], 0.f, 0.f, 0.18f, 0.19f, 0.19f, 0.19f);
        for (int s = 0; s < 2; ++s) {
            // thigh (origin: hip)
            av_sphere(av_parts[AvThighL + s], 0.f, 0.f, 0.f, 0.08f, 0.08f, 0.08f);
            av_sphere(av_parts[AvThighL + s], 0.f, 0.f, -0.21f, 0.085f, 0.09f, 0.25f);
            // shin + foot (origin: knee)
            av_sphere(av_parts[AvShinL + s], 0.f, 0.f, 0.f, 0.07f, 0.07f, 0.07f);
            av_sphere(av_parts[AvShinL + s], 0.f, 0.f, -0.21f, 0.07f, 0.075f, 0.24f);
            av_sphere(av_parts[AvShinL + s], 0.f, 0.06f, -0.45f, 0.075f, 0.13f, 0.05f);
            // arm (origin: shoulder)
            av_sphere(av_parts[AvArmL + s], 0.f, 0.f, 0.f, 0.07f, 0.07f, 0.07f);
            av_sphere(av_parts[AvArmL + s], 0.f, 0.f, -0.15f, 0.058f, 0.062f, 0.18f);
            av_sphere(av_parts[AvArmL + s], 0.f, 0.f, -0.31f, 0.055f, 0.055f, 0.055f);
            av_sphere(av_parts[AvArmL + s], 0.f, 0.01f, -0.45f, 0.052f, 0.056f, 0.15f);
            av_sphere(av_parts[AvArmL + s], 0.f, 0.02f, -0.62f, 0.055f, 0.05f, 0.065f);
        }
        // tie: a smooth strip lying on the chest (torso-local), knot at the neck
        auto front = [](float z) {
            const float k = (z - 0.32f) / 0.30f;
            return 0.13f * std::sqrt((std::max)(0.f, 1.f - k * k)) + 0.005f;
        };
        AvMesh& t = av_parts[AvTie];
        constexpr int kSeg = 14;
        const std::uint32_t tb = static_cast<std::uint32_t>(t.v.size());
        for (int i = 0; i <= kSeg; ++i) {
            const float u = static_cast<float>(i) / kSeg;
            const float z = 0.52f - 0.40f * u;
            float w = 0.016f + 0.03f * u;
            if (u > 0.86f) w *= (std::max)(0.08f, 1.f - (u - 0.86f) * 6.5f); // pointed tip
            const float y = front(z);
            const float slope = (front(z + 0.01f) - front(z - 0.01f)) / 0.02f;
            const float nl = std::sqrt(1.f + slope * slope);
            for (int sd = -1; sd <= 1; sd += 2) {
                o3d::Vertex vx{};
                vx.position[0] = static_cast<float>(sd) * w;
                vx.position[1] = y;
                vx.position[2] = z;
                vx.normal[1] = 1.f / nl;
                vx.normal[2] = -slope / nl;
                t.v.push_back(vx);
            }
        }
        for (int i = 0; i < kSeg; ++i) {
            const std::uint32_t a0 = tb + static_cast<std::uint32_t>(i * 2), a1 = a0 + 1, b0 = a0 + 2, b1 = a0 + 3;
            t.i.insert(t.i.end(), {a0, b0, a1, a1, b0, b1, a1, b0, a0, b1, b0, a1});
        }
        av_box(t, 0.f, front(0.55f) + 0.006f, 0.55f, 0.024f, 0.012f, 0.022f); // knot
        // briefcase (origin: the right hand; it always hangs straight down)
        av_box(av_parts[AvCase], 0.f, 0.f, -0.055f, 0.012f, 0.05f, 0.02f);  // handle
        av_box(av_parts[AvCase], 0.f, 0.f, -0.20f, 0.042f, 0.17f, 0.13f);   // case
    }
    constexpr std::size_t kAvNoChunk = static_cast<std::size_t>(-1);

    struct Avatar {
        std::size_t ch[kAvParts];
        Avatar() {
            for (auto& c : ch) c = static_cast<std::size_t>(-1);
        }
        bool placed = false;
        float x = 0.f, y = 0.f, z = 0.f, vx = 0.f, vy = 0.f, vz = 0.f;
        float body_yaw = 0.f, body_yaw_v = 0.f, head_yaw = 0.f, head_pitch = 0.f;
        float lean_f = 0.f, lean_fv = 0.f, lean_s = 0.f, lean_sv = 0.f;
        // animation
        float phase = 0.f;  // walk cycle (radians)
        float amp = 0.f;    // 0 idle .. 1 walk .. 1.6 run
        float air = 0.f;    // 0 on the ground .. 1 in the air (smoothed)
        float land = 0.f;   // landing squash, decays
        float anim_t = 0.f; // idle breathing / sway clock
        bool was_air = false;
        std::string name;
        float color[3] = {1, 1, 1};
        bool walking = false;
        bool seen = false;
    };
    std::unordered_map<int, Avatar> coop_avatars;
    Avatar coop_self_av; // own body in pedestrian mode
    auto av_upload = [&](const AvMesh& m) -> std::size_t {
        if (!renderer.append_textured_mesh(m.v.data(), static_cast<std::uint32_t>(m.v.size()), m.i.data(),
                                           static_cast<std::uint32_t>(m.i.size()), {}, 0u))
            return kAvNoChunk;
        return renderer.chunk_count() - 1;
    };
    auto av_chunks_of = [&](std::vector<std::size_t>& out) {
        for (const auto& [id, a] : coop_avatars)
            for (auto c : a.ch)
                if (c != kAvNoChunk) out.push_back(c);
        for (auto c : coop_self_av.ch)
            if (c != kAvNoChunk) out.push_back(c);
    };
    auto wrap_pi = [](float a) {
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    };
    // target feet position + look; integrates a springy body ("rigidbody" feel) and the gait state
    auto av_step = [&](Avatar& a, float tx, float ty, float tz, float yaw, float pitch, bool walking, bool airborne,
                       float dt) {
        if (!a.placed || std::fabs(tx - a.x) + std::fabs(ty - a.y) + std::fabs(tz - a.z) > 40.f) {
            a.x = tx;
            a.y = ty;
            a.z = tz;
            a.vx = a.vy = a.vz = 0.f;
            a.body_yaw = a.head_yaw = yaw;
            a.placed = true;
        }
        const float pvx = a.vx, pvy = a.vy;
        constexpr float kK = 55.f, kC = 12.f;
        a.vx += ((tx - a.x) * kK - a.vx * kC) * dt;
        a.vy += ((ty - a.y) * kK - a.vy * kC) * dt;
        // vertical: stiffer while in the air so the jump arc is not flattened
        const float kz = airborne ? 140.f : kK, cz = airborne ? 22.f : kC;
        a.vz += ((tz - a.z) * kz - a.vz * cz) * dt;
        a.x += a.vx * dt;
        a.y += a.vy * dt;
        a.z += a.vz * dt;
        a.head_yaw = yaw;
        a.head_pitch = std::clamp(pitch, -0.55f, 0.45f);
        // the body turns only when the head goes past ~70 deg, or towards where it walks
        const float speed = std::sqrt(a.vx * a.vx + a.vy * a.vy);
        float desired = a.body_yaw;
        const float rel = wrap_pi(a.head_yaw - a.body_yaw);
        constexpr float kNeck = 1.2f;
        if (rel > kNeck) desired = a.head_yaw - kNeck;
        else if (rel < -kNeck) desired = a.head_yaw + kNeck;
        if (speed > 0.8f) desired = std::atan2(a.vx, a.vy);
        a.body_yaw_v += (wrap_pi(desired - a.body_yaw) * 40.f - a.body_yaw_v * 9.f) * dt;
        a.body_yaw = wrap_pi(a.body_yaw + a.body_yaw_v * dt);
        // lean with acceleration (body frame), underdamped: it wobbles a bit
        const float ax = (a.vx - pvx) / (std::max)(dt, 1e-3f), ay = (a.vy - pvy) / (std::max)(dt, 1e-3f);
        const float fx = std::sin(a.body_yaw), fy = std::cos(a.body_yaw);
        const float acc_f = ax * fx + ay * fy, acc_s = ax * fy - ay * fx;
        const float tf = std::clamp(acc_f * 0.03f + speed * 0.025f, -0.35f, 0.4f);
        const float ts = std::clamp(acc_s * 0.03f - a.body_yaw_v * 0.05f, -0.35f, 0.35f);
        a.lean_fv += ((tf - a.lean_f) * 70.f - a.lean_fv * 6.f) * dt;
        a.lean_sv += ((ts - a.lean_s) * 70.f - a.lean_sv * 6.f) * dt;
        a.lean_f += a.lean_fv * dt;
        a.lean_s += a.lean_sv * dt;
        // gait
        const float k8 = (std::min)(1.f, dt * 8.f);
        if (a.was_air && !airborne && a.air > 0.35f) a.land = 1.f;
        a.was_air = airborne;
        a.air += ((airborne ? 1.f : 0.f) - a.air) * (std::min)(1.f, dt * 12.f);
        a.land = (std::max)(0.f, a.land - dt * 3.5f);
        const float amp_target = airborne ? 0.f : std::clamp(speed / 1.45f, 0.f, 1.6f);
        a.amp += (amp_target - a.amp) * k8;
        if (!airborne) {
            const float stride = 1.25f + 0.4f * std::clamp(speed - 1.5f, 0.f, 4.f); // metres per full cycle
            a.phase += 6.2831853f * speed / stride * dt;
            if (a.phase > 62.831853f) a.phase -= 62.831853f;
        }
        a.anim_t += dt;
        a.walking = walking;
    };
    auto av_draw = [&](Avatar& a, bool show_head) {
        for (int p = 0; p < kAvParts; ++p)
            if (a.ch[p] == kAvNoChunk) a.ch[p] = av_upload(av_parts[p]);
        for (auto c : a.ch)
            if (c == kAvNoChunk) return;
        using namespace DirectX;
        const float h = a.body_yaw;
        const float walk = (std::min)(a.amp, 1.f), run = std::clamp((a.amp - 1.f) / 0.6f, 0.f, 1.f);
        const float s = std::sin(a.phase), c = std::cos(a.phase);
        const float air = a.air, land = a.land;
        const float idle = (1.f - walk) * (1.f - air);
        // legs: hip swing, knee bends while the leg swings forward
        const float leg_sw = (0.42f + 0.28f * run) * walk;
        const float knee_k = (0.85f + 0.65f * run) * walk;
        float thigh_l = s * leg_sw, thigh_r = -s * leg_sw;
        float knee_l = -((std::max)(0.f, c) * knee_k + 0.06f * walk);
        float knee_r = -((std::max)(0.f, -c) * knee_k + 0.06f * walk);
        // arms opposite to the legs (the briefcase arm swings less)
        float arm_l = -s * (0.45f + 0.45f * run) * walk + std::sin(a.anim_t * 1.3f) * 0.03f * idle;
        float arm_r = s * (0.25f + 0.25f * run) * walk + std::sin(a.anim_t * 1.3f + 1.f) * 0.02f * idle;
        float roll_l = 0.12f, roll_r = 0.12f;
        // in the air: tucked legs, arms out for balance
        thigh_l += (0.65f - thigh_l) * air;
        thigh_r += (0.25f - thigh_r) * air;
        knee_l += (-1.05f - knee_l) * air;
        knee_r += (-0.55f - knee_r) * air;
        arm_l += (0.55f - arm_l) * air;
        arm_r += (0.35f - arm_r) * air;
        roll_l += 0.55f * air;
        roll_r += 0.30f * air;
        // landing squash
        thigh_l += 0.55f * land;
        thigh_r += 0.55f * land;
        knee_l -= 1.0f * land;
        knee_r -= 1.0f * land;
        const float knee_drop = kAvKnee * (1.f - std::cos(0.5f * land)) * 2.f;
        const float pelvis_dz = -0.035f * walk * std::fabs(s) - 0.02f * run - knee_drop +
                                0.006f * std::sin(a.anim_t * 2.1f) * idle;
        const float lean = a.lean_f + 0.12f * run + 0.2f * land - 0.08f * air;
        const float twist = s * (0.09f + 0.06f * run) * walk;
        const XMMATRIX basis = XMMatrixSet(std::cos(h), -std::sin(h), 0.f, 0.f, std::sin(h), std::cos(h), 0.f, 0.f, 0.f,
                                           0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f);
        const XMMATRIX feet = basis * XMMatrixTranslation(a.x, a.y, a.z);
        const XMMATRIX hips = XMMatrixRotationY(a.lean_s * 0.5f) * XMMatrixTranslation(0.f, 0.f, kAvHipZ + pelvis_dz) * feet;
        const XMMATRIX torso = XMMatrixRotationX(-lean) * XMMatrixRotationY(a.lean_s * 0.5f) * XMMatrixRotationZ(twist) * hips;
        const XMMATRIX thigh_lm = XMMatrixRotationX(thigh_l) * XMMatrixTranslation(-kAvHipX, 0.f, 0.f) * hips;
        const XMMATRIX thigh_rm = XMMatrixRotationX(thigh_r) * XMMatrixTranslation(kAvHipX, 0.f, 0.f) * hips;
        const XMMATRIX shin_lm = XMMatrixRotationX(knee_l) * XMMatrixTranslation(0.f, 0.f, -kAvKnee) * thigh_lm;
        const XMMATRIX shin_rm = XMMatrixRotationX(knee_r) * XMMatrixTranslation(0.f, 0.f, -kAvKnee) * thigh_rm;
        const XMMATRIX arm_lm = XMMatrixRotationX(arm_l) * XMMatrixRotationY(roll_l) *
                                XMMatrixTranslation(-kAvShoulderX, 0.f, kAvShoulderZ) * torso;
        const XMMATRIX arm_rm = XMMatrixRotationX(arm_r) * XMMatrixRotationY(-roll_r) *
                                XMMatrixTranslation(kAvShoulderX, 0.f, kAvShoulderZ) * torso;
        const float rel = wrap_pi(a.head_yaw - a.body_yaw) - twist;
        const XMMATRIX head = XMMatrixRotationX(a.head_pitch) * XMMatrixRotationZ(-std::clamp(rel, -1.3f, 1.3f)) *
                              XMMatrixTranslation(0.f, 0.f, kAvNeckZ) * torso;
        // briefcase hangs from the right hand, swinging a little behind the arm
        XMFLOAT3 hand{};
        XMStoreFloat3(&hand, XMVector3TransformCoord(XMVectorSet(0.f, 0.02f, -0.62f, 1.f), arm_rm));
        const XMMATRIX case_m = XMMatrixRotationX(arm_r * 0.35f) * basis * XMMatrixTranslation(hand.x, hand.y, hand.z);
        const XMMATRIX mats[kAvParts] = {torso, head, thigh_lm, thigh_rm, shin_lm, shin_rm, arm_lm, arm_rm, torso, case_m};
        for (int p = 0; p < kAvParts; ++p) {
            const auto w = store_world_transposed(mats[p]);
            renderer.set_chunk_world_matrix(a.ch[p], w.data());
            const bool dark = p == AvTie || p == AvCase;
            if (dark) renderer.set_chunk_highlight(a.ch[p], 1.f, 0.07f, 0.07f, 0.08f);
            else renderer.set_chunk_highlight(a.ch[p], 1.f, a.color[0], a.color[1], a.color[2]);
            renderer.set_chunk_visible(a.ch[p], p == AvHead ? show_head : true);
        }
    };
    auto coop_reapply_tints = [&]() {
        auto tint = [&](const Avatar& a) {
            for (int p = 0; p < kAvParts; ++p) {
                if (a.ch[p] == kAvNoChunk) continue;
                if (p == AvTie || p == AvCase) renderer.set_chunk_highlight(a.ch[p], 1.f, 0.07f, 0.07f, 0.08f);
                else renderer.set_chunk_highlight(a.ch[p], 1.f, a.color[0], a.color[1], a.color[2]);
            }
        };
        for (const auto& [id, a] : coop_avatars)
            if (a.seen) tint(a);
        if (coop_walk_active) tint(coop_self_av);
        for (const auto& [k, m] : missing_assets)
            if (m.chunk != static_cast<std::size_t>(-1)) renderer.set_chunk_highlight(m.chunk, 1.f, 1.f, 0.15f, 0.85f);
    };
    auto av_hide = [&](Avatar& a) {
        for (auto c : a.ch)
            if (c != kAvNoChunk) renderer.set_chunk_visible(c, false);
    };
    // highest surface (terrain, splines, objects) at or below z_from, avatars excluded
    // Vegetation the pedestrian walks through (trees, [tree] billboards, bushes, hedges): their chunks near the
    // walker, left out of the walk's wall probes and ground ray (you bumped into every tree)
    std::vector<std::size_t> walk_veg_excl;
    std::unordered_map<std::int32_t, bool> walk_veg_by_obj;
    double walk_veg_t = -1.0;
    auto walk_refresh_vegetation = [&](float wx, float wy) {
        const double now = glfwGetTime();
        if (now - walk_veg_t < 0.4 && now >= walk_veg_t) return;
        walk_veg_t = now;
        walk_veg_excl.clear();
        ensure_edit_obj_index();
        auto is_veg_path = [](const std::filesystem::path& p) {
            std::wstring s = p.native();
            for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
            for (const wchar_t* k : {L"tree", L"baum", L"b\u00e4um", L"busch", L"bush", L"hecke", L"hedge", L"strauch",
                                     L"shrub", L"pflanz", L"plant", L"veget", L"arbol", L"\u00e1rbol", L"palm", L"gras",
                                     L"flower", L"blume", L"forest"})
                if (s.find(k) != std::wstring::npos) return true;
            return false;
        };
        for (const auto& sp : scene_places) {
            if (!sp.alive) continue;
            const float dx = sp.tx - wx, dy = sp.ty - wy;
            if (dx * dx + dy * dy > 40.f * 40.f) continue;
            bool veg = sp.is_tree;
            if (!veg) {
                const auto vit = walk_veg_by_obj.find(sp.object_id);
                if (vit != walk_veg_by_obj.end()) {
                    veg = vit->second;
                } else {
                    const auto it = edit_obj_by_id.find(sp.object_id);
                    veg = it != edit_obj_by_id.end() && it->second && is_veg_path(it->second->sco_path);
                    walk_veg_by_obj.emplace(sp.object_id, veg);
                }
            }
            if (veg) walk_veg_excl.push_back(sp.chunk);
        }
    };
    auto coop_ground = [&](float x, float y, float z_from) -> float {
        std::vector<std::size_t> excl;
        av_chunks_of(excl);
        excl.insert(excl.end(), walk_veg_excl.begin(), walk_veg_excl.end());
        for (const auto& [k, m] : missing_assets)
            if (m.chunk != static_cast<std::size_t>(-1)) excl.push_back(m.chunk);
        if (const auto hz = renderer.pick_surface_hit_z(x, y, z_from, 0.f, 0.f, -1.f, excl.empty() ? nullptr : excl.data(),
                                                        excl.size()))
            return *hz;
        return sample_terrain_editor_xy(x, y);
    };

    // ===== pedestrian mode =====
    struct Walk {
        float x = 0.f, y = 0.f, z = 0.f, vz = 0.f;
        float yaw = 0.f, pitch = 0.f;
        bool on_ground = false;
        double mx = 0.0, my = 0.0;
        bool have_mouse = false;
    } walk;
    auto coop_walk_enter = [&]() {
        if (coop_walk_active || bus_drive_active) return;
        float ex = 0.f, ey = 0.f, ez = 0.f;
        camera.eye_position(ex, ey, ez);
        walk.x = camera.target_x;
        walk.y = camera.target_y;
        walk.z = coop_ground(walk.x, walk.y, camera.target_z + 3.f);
        walk.vz = 0.f;
        walk.yaw = std::atan2(camera.target_x - ex, camera.target_y - ey);
        walk.pitch = 0.f;
        walk.have_mouse = false;
        coop_walk_active = true;
        coop_walk_cursor = false;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        panel.set_status("Modo peaton: WASD caminar, Shift correr, Espacio saltar, mouse mirar, Esc salir");
    };
    // walk-edit: the object carried along the crosshair (its id), until a click places it
    std::int32_t walk_hold_id = 0;
    bool walk_hold_bus = false; // walk-edit: the player's bus carried along the crosshair
    int walk_hold_pending = 0; // frames until the editor's click selection is read
    bool walk_skip_hold = false;
    bool walk_lmb_was = false, walk_rmb_was = false, walk_e_was = false;
    float walk_aim[3]{};
    auto coop_walk_exit = [&]() {
        if (!coop_walk_active) return;
        coop_walk_active = false;
        coop_walk_edit = false;
        coop_walk_cursor = false;
        walk_hold_id = 0;
        walk_hold_pending = 0;
        input::set_cursor_center_override(false);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        camera.editor_nav = true;
        camera.orbit_target_locked = false;
        camera.target_x = walk.x;
        camera.target_y = walk.y;
        camera.target_z = walk.z + 1.f;
        camera.radius = 12.f;
        camera.phi = 1.1f;
        camera.reset_mouse();
        av_hide(coop_self_av);
        panel.set_status("Modo peaton: salida");
    };
    auto coop_walk_tick = [&](float dt) {
        if (!coop_walk_active) return;
        if (bus_drive_active || glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            coop_walk_exit();
            return;
        }
        // E: the mouse cursor for the interface (the view stops turning), E again: back to looking around
        {
            const bool e = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS && !ImGui::GetIO().WantTextInput;
            if (e && !walk_e_was) {
                coop_walk_cursor = !coop_walk_cursor;
                glfwSetInputMode(window, GLFW_CURSOR, coop_walk_cursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                walk.have_mouse = false; // (no jump of the view from the cursor's travel)
            }
            walk_e_was = e;
        }
        input::set_cursor_center_override(coop_walk_edit && !coop_walk_cursor);
        double cx = 0.0, cy = 0.0;
        glfwGetCursorPos(window, &cx, &cy);
        if (walk.have_mouse && !coop_walk_cursor) {
            walk.yaw = wrap_pi(walk.yaw + static_cast<float>(cx - walk.mx) * 0.0024f);
            walk.pitch = std::clamp(walk.pitch - static_cast<float>(cy - walk.my) * 0.0024f, -1.45f, 1.45f);
        }
        walk.mx = cx;
        walk.my = cy;
        walk.have_mouse = true;
        walk_refresh_vegetation(walk.x, walk.y);
        auto key = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        const float fx = std::sin(walk.yaw), fy = std::cos(walk.yaw);
        const float rx = std::cos(walk.yaw), ry = -std::sin(walk.yaw);
        float mvx = 0.f, mvy = 0.f;
        if (key(GLFW_KEY_W) || key(GLFW_KEY_UP)) mvx += fx, mvy += fy;
        if (key(GLFW_KEY_S) || key(GLFW_KEY_DOWN)) mvx -= fx, mvy -= fy;
        if (key(GLFW_KEY_D) || key(GLFW_KEY_RIGHT)) mvx += rx, mvy += ry;
        if (key(GLFW_KEY_A) || key(GLFW_KEY_LEFT)) mvx -= rx, mvy -= ry;
        const float ml = std::sqrt(mvx * mvx + mvy * mvy);
        if (ml > 1e-3f) {
            const bool run = key(GLFW_KEY_LEFT_SHIFT) || key(GLFW_KEY_RIGHT_SHIFT);
            const float sp = (run ? 6.5f : 2.2f) * dt / ml;
            const float dx = mvx * sp, dy = mvy * sp;
            // walls: horizontal probes at knee and chest height
            std::vector<std::size_t> excl;
            av_chunks_of(excl);
            excl.insert(excl.end(), walk_veg_excl.begin(), walk_veg_excl.end()); // walk through trees
            const float dl = std::sqrt(dx * dx + dy * dy);
            bool blocked = false;
            for (float hgt : {0.55f, 1.3f}) {
                float t = 0.f;
                if (renderer.pick_surface_hit_z(walk.x, walk.y, walk.z + hgt, dx / dl, dy / dl, 0.f,
                                                excl.empty() ? nullptr : excl.data(), excl.size(), &t) &&
                    t < dl + 0.3f)
                    blocked = true;
            }
            if (!blocked) {
                walk.x += dx;
                walk.y += dy;
            }
        }
        // gravity, steps up to 45 cm, jump
        walk.vz -= 9.81f * dt;
        walk.z += walk.vz * dt;
        const float g = coop_ground(walk.x, walk.y, walk.z + 0.45f);
        walk.on_ground = false;
        if (walk.z <= g) {
            walk.z = g;
            walk.vz = 0.f;
            walk.on_ground = true;
        }
        if (walk.on_ground && key(GLFW_KEY_SPACE)) walk.vz = 4.6f;
        // first-person camera: eye in the head, looking along yaw / pitch
        const float cp = std::cos(walk.pitch);
        const float dxw = std::sin(walk.yaw) * cp, dyw = std::cos(walk.yaw) * cp, dzw = std::sin(walk.pitch);
        const float ex = walk.x + dxw * 0.12f, ey = walk.y + dyw * 0.12f, ez = walk.z + 1.62f;
        constexpr float kR = 0.05f;
        camera.editor_nav = false;
        camera.orbit_target_locked = true;
        camera.z_up = true;
        camera.radius = kR;
        camera.target_x = ex + dxw * kR;
        camera.target_y = ey + dyw * kR;
        camera.target_z = ez + dzw * kR;
        camera.theta = std::atan2(-dyw, -dxw);
        camera.phi = std::acos(std::clamp(-dzw, -1.f, 1.f));
        camera.ground_z = walk.z;
        // walk-edit: a click selects what the crosshair is on (the editor's own click, aimed at the centre)
        // and carries it along the crosshair; a click or the right button puts it down. Where the
        // crosshair meets the ground / an object is where it goes; looking up past the ground carries it
        // ahead, up to 25 m.
        if (coop_walk_edit && !coop_walk_cursor) {
            const bool over_ui = ImGui::GetIO().WantCaptureMouse;
            const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            const bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
            if (lmb && !walk_lmb_was && !over_ui && walk_hold_bus) {
                walk_hold_bus = false; // the bus put down where it is
                walk_skip_hold = true;
                panel.set_status("Bus colocado");
            } else if (lmb && !walk_lmb_was && !over_ui && walk_hold_id != 0) {
                walk_hold_id = 0; // put down where it is
                walk_skip_hold = true;
                panel.set_status("Objeto colocado");
            } else if (lmb && !walk_lmb_was && !over_ui && omsi_bus && omsi_bus->oo && !bus_drive_active) {
                // the bus under the crosshair (it is no edit object): carried like one
                static std::vector<std::size_t> s_bus_chunks;
                s_bus_chunks.clear();
                for (const auto ch : omsi_bus->chunks)
                    if (ch != kNoChunk) s_bus_chunks.push_back(ch);
                for (const auto ch : omsi_bus->sec_chunks)
                    if (ch != kNoChunk) s_bus_chunks.push_back(ch);
                float mxc = 0.f, myc = 0.f, fwc = 1.f, fhc = 1.f;
                input::cursor_to_framebuffer(window, mxc, myc, fwc, fhc);
                float wvp[16]{};
                camera.build_world_view_proj(fhc > 0.f ? fwc / fhc : 1.6f, wvp);
                float tb = 1e30f;
                if (!s_bus_chunks.empty() &&
                    renderer.pick_chunk(mxc, myc, static_cast<std::uint32_t>(fwc), static_cast<std::uint32_t>(fhc), wvp,
                                        false, s_bus_chunks.data(), s_bus_chunks.size(), &tb)) {
                    walk_hold_bus = true;
                    walk_skip_hold = true; // (not the scenery behind it)
                    panel.set_status("Llevando el bus: rueda = girar, click / click derecho = soltar");
                }
            }
            if (!lmb && walk_lmb_was && !over_ui) {
                // the click only selects (the editor's own click at the crosshair); G moves it, as in the
                // normal view (it used to start carrying whatever was clicked)
                walk_skip_hold = false;
            }
            if (rmb && !walk_rmb_was && (walk_hold_id != 0 || walk_hold_bus)) {
                walk_hold_id = 0;
                walk_hold_bus = false;
                panel.set_status("Soltado");
            }
            walk_lmb_was = lmb;
            walk_rmb_was = rmb;
            if (walk_hold_pending > 0 && --walk_hold_pending == 0 && selected_object >= 0 &&
                static_cast<std::size_t>(selected_object) < edit_objects.size()) {
                walk_hold_id = edit_objects[static_cast<std::size_t>(selected_object)].id;
                push_undo("walk_carry");
                panel.set_status("Llevando objeto: click o click derecho = soltar, Supr = borrar");
            }
            // the aim point
            std::vector<std::size_t> excl;
            av_chunks_of(excl);
            excl.insert(excl.end(), walk_veg_excl.begin(), walk_veg_excl.end());
            std::int32_t aim_skip_id = walk_hold_id;
            if (aim_skip_id == 0 && (edit_mode == EditMode::Grab || edit_mode == EditMode::GrabHeight) &&
                selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                aim_skip_id = edit_objects[static_cast<std::size_t>(selected_object)].id; // G: not onto itself
            if (aim_skip_id != 0)
                for (const auto& sp : scene_places)
                    if (sp.alive && sp.object_id == aim_skip_id) excl.push_back(sp.chunk);
            if (walk_hold_bus && omsi_bus) {
                for (const auto ch : omsi_bus->chunks)
                    if (ch != kNoChunk) excl.push_back(ch);
                for (const auto ch : omsi_bus->sec_chunks)
                    if (ch != kNoChunk) excl.push_back(ch);
            }
            const float cpa = std::cos(walk.pitch);
            const float adx = std::sin(walk.yaw) * cpa, ady = std::cos(walk.yaw) * cpa, adz = std::sin(walk.pitch);
            const float aex = walk.x, aey = walk.y, aez = walk.z + 1.62f;
            float t = 0.f;
            constexpr float kReach = 25.f;
            if (renderer.pick_surface_hit_z(aex, aey, aez, adx, ady, adz, excl.empty() ? nullptr : excl.data(),
                                            excl.size(), &t) &&
                t > 0.f && t < kReach) {
                walk_aim[0] = aex + adx * t;
                walk_aim[1] = aey + ady * t;
                walk_aim[2] = aez + adz * t;
            } else {
                walk_aim[0] = aex + std::sin(walk.yaw) * kReach * (std::max)(cpa, 0.2f);
                walk_aim[1] = aey + std::cos(walk.yaw) * kReach * (std::max)(cpa, 0.2f);
                walk_aim[2] = coop_ground(walk_aim[0], walk_aim[1], aez + 50.f);
            }
            if (walk_hold_bus && omsi_bus && omsi_bus->oo) {
                // the bus stands where the crosshair meets the ground; the mouse wheel turns it
                float hd = omsi_bus->heading_deg + ImGui::GetIO().MouseWheel * 5.f;
                const float gz = coop_ground(walk_aim[0], walk_aim[1], walk_aim[2] + 3.f);
                oo_teleport(omsi_bus->oo, walk_aim[0], walk_aim[1], gz, hd);
                omsi_bus->x = walk_aim[0];
                omsi_bus->y = walk_aim[1];
                omsi_bus->z = gz;
                omsi_bus->heading_deg = hd;
            } else if (!omsi_bus) {
                walk_hold_bus = false;
            }
            if (walk_hold_id != 0) {
                bool found = false;
                for (auto& o : edit_objects)
                    if (o.id == walk_hold_id) {
                        apply_sco_follow_xy(o, walk_aim[0], walk_aim[1]);
                        found = true;
                        break;
                    }
                if (!found) walk_hold_id = 0; // (deleted)
            }
        } else {
            walk_hold_id = 0;
            walk_hold_bus = false;
            walk_hold_pending = 0;
            walk_lmb_was = walk_rmb_was = false;
        }
        std::memcpy(coop_self_av.color, coop.profile().color, sizeof(coop_self_av.color));
        av_step(coop_self_av, walk.x, walk.y, walk.z, walk.yaw, walk.pitch, true, !walk.on_ground, dt);
        coop_self_av.x = walk.x; // no lag on the own body
        coop_self_av.y = walk.y;
        coop_self_av.z = walk.z;
        av_draw(coop_self_av, false);
    };

    // ===== per-frame =====
    double coop_last_pose_t = 0.0;
    double coop_asset_req_t = 0.0;
    int coop_asset_epoch_seen = 0;
    // guest: every .sco / .sli of the map (and every mesh of those .sco) is checked once; what this PC
    // does not have is asked to the host, and the objects / splines using it wait to be spawned again
    std::unordered_set<std::wstring> coop_asset_ok, coop_asset_asked;
    std::unordered_set<std::int32_t> coop_await_obj, coop_await_spl;
    std::size_t coop_scan_obj = 0, coop_scan_spl = 0;
    auto coop_last_t = std::chrono::steady_clock::now();
    {
        // tell the session which map this editor run has open (guest: READY once it is the room's map)
        std::filesystem::path live = cfg.workspace && !cfg.workspace->actual_dir.empty()
                                         ? map::workspace_live_map_dir(*cfg.workspace)
                                         : map::live_map_dir_from_folder(cfg.global.root);
        if (live.empty()) live = cfg.global.root;
        coop.notify_map_loaded(cfg.omsi_root, live);
    }
    auto coop_frame_tick = [&]() {
        const auto t_now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(t_now - coop_last_t).count(), 0.f, 0.1f);
        coop_last_t = t_now;
        coop.update();
        {
            // F7 or Vista > Modo peaton toggles first person
            static bool f7_was = false;
            const bool f7 = glfwGetKey(window, GLFW_KEY_F7) == GLFW_PRESS && !ImGui::GetIO().WantTextInput;
            auto& stp = panel.imgui_state();
            // the bus: F7 while driving gets off when it stands (< 5 km/h); F7 next to it gets back in
            auto bus_kmh = [&]() {
                OoStatus bst{};
                if (omsi_bus && omsi_bus->oo) oo_status(omsi_bus->oo, &bst);
                return std::fabs(bst.speed_kmh);
            };
            auto near_bus = [&]() {
                if (!omsi_bus) return false;
                const auto& b = *omsi_bus;
                const float h = b.heading_deg * 3.14159265f / 180.f;
                const float dx = walk.x - b.x, dy = walk.y - b.y;
                const float lx = dx * std::cos(h) - dy * std::sin(h);  // right
                const float ly = dx * std::sin(h) + dy * std::cos(h) - b.doc.bbox_offset_y; // ahead
                return std::fabs(lx) < b.doc.bbox_half_x + 2.5f && std::fabs(ly) < b.doc.bbox_half_y + 2.5f &&
                       std::fabs(walk.z - b.z) < 4.f;
            };
            const bool f7_edge = (f7 && !f7_was) || stp.walk_toggle_request;
            if (f7_edge && bus_drive_active && omsi_bus) {
                stp.walk_toggle_request = false;
                if (bus_kmh() >= 5.f) {
                    panel.set_status("Detene el bus (menos de 5 km/h) para bajarte con F7");
                } else {
                    // get off on the right (the doors' side), next to the middle of the bus
                    const auto& b = *omsi_bus;
                    const float h = b.heading_deg * 3.14159265f / 180.f;
                    const float side = b.doc.bbox_half_x + 0.9f;
                    exit_bus_drive();
                    coop_walk_enter();
                    if (coop_walk_active) {
                        walk.x = b.x + std::cos(h) * side + std::sin(h) * b.doc.bbox_offset_y;
                        walk.y = b.y - std::sin(h) * side + std::cos(h) * b.doc.bbox_offset_y;
                        walk.z = coop_ground(walk.x, walk.y, b.z + 3.f);
                        walk.yaw = h - 1.5707963f; // facing the bus
                        coop_walk_edit = true;
                        panel.set_status("Bajaste del bus: F7 cerca del bus para volver a subir");
                    }
                }
            } else if (f7_edge && coop_walk_active && !walk_hold_bus && near_bus()) {
                stp.walk_toggle_request = false;
                coop_walk_exit();
                bus_drive_active = true;
                camera.editor_nav = false;
                bus_cam_mode = 1;
                bus_cam_driver_idx = (std::max)(0, omsi_bus->doc.std_driver_camera_index);
                bus_cam_need_snap = true;
                apply_bus_camera(*omsi_bus);
                panel.set_status("Subiste al bus (F7 con el bus detenido para bajar)");
            } else if (f7_edge) {
                // F7: walk-edit -> walk only -> off -> walk-edit ...
                stp.walk_toggle_request = false;
                if (!coop_walk_active) {
                    coop_walk_enter();
                    coop_walk_edit = coop_walk_active;
                    if (coop_walk_edit) panel.set_status("Modo peaton editor (F7: solo peaton)");
                } else if (coop_walk_edit) {
                    coop_walk_edit = false;
                    walk_hold_id = 0;
                    input::set_cursor_center_override(false);
                    panel.set_status("Modo peaton (F7: salir)");
                } else {
                    coop_walk_exit();
                }
            }
            f7_was = f7;
            stp.walk_active_ui = coop_walk_active;
        }
        coop_walk_tick(dt);
        // a version choice may need another map folder
        std::filesystem::path reload;
        bool reload_force = false;
        if (coop.take_reload(reload, &reload_force) && cfg.switch_map_out) {
            std::filesystem::path live = cfg.workspace && !cfg.workspace->actual_dir.empty()
                                             ? map::workspace_live_map_dir(*cfg.workspace)
                                             : map::live_map_dir_from_folder(cfg.global.root);
            std::error_code ec;
            if (!reload_force && !live.empty() && std::filesystem::equivalent(live, reload.parent_path(), ec)) {
                coop.notify_map_loaded(cfg.omsi_root, live);
            } else {
                *cfg.switch_map_out = reload;
                log::info("coop: switching map -> " + omsi::path_text(reload));
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }
        const auto role = coop.role();
        if (role != coop_prev_role) {
            if (role == omsi::coop::Role::None) {
                for (auto& [id, a] : coop_avatars) av_hide(a);
                coop_avatars.clear();
            }
            coop_baselined = false;
            coop_obj_h.clear();
            coop_spl_h.clear();
            coop_prev_role = role;
        }
        if (role == omsi::coop::Role::None) return;
        // guest: objects / splines this PC lacks come from the host into RAM, then they are spawned again
        if (role == omsi::coop::Role::Guest) {
            {
                std::vector<std::filesystem::path> want;
                auto path_key = [](const std::filesystem::path& p) {
                    std::wstring k = p.lexically_normal().native();
                    for (auto& c : k) c = static_cast<wchar_t>(std::towlower(c));
                    return k;
                };
                // true when the file is here (disk or RAM); otherwise it is asked (once) and false
                auto have = [&](const std::filesystem::path& p, const std::filesystem::path& ask) -> bool {
                    if (p.empty()) return true;
                    const auto k = path_key(p);
                    if (coop_asset_ok.count(k)) return true;
                    if (omsi::vfs::exists(p)) {
                        coop_asset_ok.insert(k);
                        return true;
                    }
                    if (coop_asset_asked.insert(path_key(ask)).second) want.push_back(ask);
                    return false;
                };
                int budget = 300;
                while (budget-- > 0 && !edit_objects.empty()) {
                    if (coop_scan_obj >= edit_objects.size()) {
                        coop_scan_obj = 0;
                        break;
                    }
                    const auto& o = edit_objects[coop_scan_obj++];
                    if (o.sco_path.empty()) continue;
                    bool ok = have(o.sco_path, o.sco_path);
                    if (ok) {
                        // the .sco is here: its meshes too? (the host sends them with the .sco)
                        const auto ck = path_key(o.sco_path) + L"|m";
                        if (!coop_asset_ok.count(ck)) {
                            if (const auto* sc = cached_sco(o.sco_path)) {
                                bool all = true;
                                for (const auto& m : sc->meshes)
                                    if (!m.mesh_file.empty() && !omsi::vfs::exists(m.mesh_file)) all = false;
                                if (all) coop_asset_ok.insert(ck);
                                else if (coop_asset_asked.insert(ck).second) {
                                    want.push_back(o.sco_path);
                                    ok = false;
                                } else {
                                    ok = false;
                                }
                            }
                        }
                    }
                    if (!ok) coop_await_obj.insert(o.id);
                }
                budget = 300;
                while (budget-- > 0 && !edit_splines.empty()) {
                    if (coop_scan_spl >= edit_splines.size()) {
                        coop_scan_spl = 0;
                        break;
                    }
                    const auto& sl = edit_splines[coop_scan_spl++];
                    if (!sl.sli_path.empty() && !have(sl.sli_path, sl.sli_path)) coop_await_spl.insert(sl.id);
                }
                if (!want.empty()) {
                    log::info("coop assets: pidiendo " + std::to_string(want.size()) + " .sco/.sli al anfitrion");
                    coop.request_assets(want);
                }
            }
            const int ep = coop.assets_epoch();
            if (ep != coop_asset_epoch_seen) {
                coop_asset_epoch_seen = ep;
                o3d::prepared_vertex_cache().clear_failed();
                std::vector<std::pair<bool, std::int32_t>> retry;
                std::unordered_set<std::int64_t> queued;
                auto q = [&](bool spl, std::int32_t id) {
                    if (queued.insert((static_cast<std::int64_t>(spl ? 1 : 0) << 40) ^ static_cast<std::uint32_t>(id)).second)
                        retry.emplace_back(spl, id);
                };
                for (const auto& [k, m] : missing_assets) q(m.spline, m.id);
                for (const auto id : coop_await_obj) q(false, id);
                for (const auto id : coop_await_spl) q(true, id);
                coop_await_obj.clear();
                coop_await_spl.clear();
                coop_asset_ok.clear(); // re-check on the next scan pass (meshes may have arrived)
                int again = 0;
                for (const auto& [spl, id] : retry) {
                    if (spl) {
                        for (auto& sp_e : edit_splines) {
                            if (sp_e.id != id) continue;
                            spline::invalidate_sli_cache(sp_e.sli_path);
                            clear_missing_asset(true, id);
                            remesh_spline(sp_e, false);
                            ++again;
                            break;
                        }
                    } else {
                        for (auto& o : edit_objects) {
                            if (o.id != id) continue;
                            sco::invalidate_sco_cache(o.sco_path); // mesh paths are resolved when it loads
                            clear_missing_asset(false, id);
                            for (auto& sp : scene_places)
                                if (sp.object_id == id && sp.alive) {
                                    sp.alive = false;
                                    renderer.set_chunk_visible(sp.chunk, false);
                                }
                            spawn_editor_object_gpu(o);
                            ++again;
                            break;
                        }
                    }
                }
                if (again) log::info("coop assets: reintentando " + std::to_string(again) + " objetos/splines con archivos del anfitrion");
            }
        }
        const bool live_room = role == omsi::coop::Role::Host ||
                               coop.version_state() == omsi::coop::VersionState::Ready;
        // after a local undo / redo: the other users' latest changes come back (only own changes undo)
        if (coop_snapshot_applied) {
            coop_snapshot_applied = false;
            if (live_room && !view_only) {
                const auto keep = coop_remote_last;
                for (const auto& [k, op] : keep) {
                    try {
                        coop_apply_op(op);
                    } catch (...) {
                    }
                }
                // Own changes the undo / redo moved are found by the normal scan (hash differs). What it
                // cannot see: own creations the undo removed, and own deletions it brought back.
                std::unordered_set<std::int32_t> present_o, present_s;
                for (const auto& o : edit_objects) present_o.insert(o.id);
                for (const auto& sp : edit_splines) present_s.insert(sp.id);
                for (auto it = coop_obj_h.begin(); it != coop_obj_h.end();) {
                    const std::int32_t id = it->first;
                    if (!present_o.count(id) && coop_is_mine_new(id, false) && !coop_remote_last.count({0, id})) {
                        coop.send_op(omsi::coop::OpKind::ObjDelete, id, {});
                        coop_obj_del_done.insert(id);
                        it = coop_obj_h.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (auto it = coop_spl_h.begin(); it != coop_spl_h.end();) {
                    const std::int32_t id = it->first;
                    if (!present_s.count(id) && coop_is_mine_new(id, true) && !coop_remote_last.count({1, id})) {
                        coop.send_op(omsi::coop::OpKind::SplDelete, id, {});
                        coop_spl_del_done.insert(id);
                        it = coop_spl_h.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (const auto& o : edit_objects)
                    if (coop_obj_del_done.count(o.id) && !editor_deleted_object_ids.count(o.id)) {
                        coop_obj_del_done.erase(o.id);
                        coop_obj_h[o.id] = coop_obj_hash(o);
                        coop.send_op(omsi::coop::OpKind::ObjUpsert, o.id, coop_ser_obj(o));
                    }
                for (const auto& sp : edit_splines)
                    if (coop_spl_del_done.count(sp.id) && !editor_deleted_spline_ids.count(sp.id)) {
                        coop_spl_del_done.erase(sp.id);
                        coop_spl_h[sp.id] = coop_spl_hash(sp);
                        coop.send_op(omsi::coop::OpKind::SplUpsert, sp.id, coop_ser_spl(sp));
                    }
                // full pass right away so the moved own entities go out this frame
                coop_obj_cursor = coop_spl_cursor = 0;
            }
        }
        // remote edits first, so the scan does not echo them
        for (const auto& op : coop.take_ops()) {
            if (!live_room || view_only) continue;
            try {
                coop_apply_op(op);
            } catch (const std::exception& ex) {
                log::warn(std::string("coop: remote edit skipped id=") + std::to_string(op.id) + ": " + ex.what());
            } catch (...) {
                log::warn("coop: remote edit skipped id=" + std::to_string(op.id));
            }
        }
        if (live_room && !view_only) coop_scan_changes();
        // own pose (15 Hz)
        const double now_d = glfwGetTime();
        if (now_d - coop_last_pose_t > 1.0 / 15.0) {
            coop_last_pose_t = now_d;
            float ex = 0.f, ey = 0.f, ez = 0.f, fx = 0.f, fy = 0.f, fz = 0.f, yaw = 0.f, pitch = 0.f;
            if (coop_walk_active) {
                fx = walk.x;
                fy = walk.y;
                fz = walk.z;
                yaw = walk.yaw;
                pitch = walk.pitch;
            } else {
                camera.eye_position(ex, ey, ez);
                const float dx = camera.target_x - ex, dy = camera.target_y - ey, dz = camera.target_z - ez;
                yaw = std::atan2(dx, dy);
                pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy));
                // stand where this user is looking (the orbit centre), on top of whatever is there:
                // terrain, a spline or a .sco - never inside it, never floating at the eye
                fx = camera.target_x;
                fy = camera.target_y;
                // ray from well above: the TOP of what is there (a building, a bridge, a road), not the
                // terrain inside it
                fz = coop_ground(fx, fy, (std::max)(camera.target_z, ez) + 80.f);
            }
            omsi::coop::Pose p;
            coop_to_tile(fx, fy, p.gx, p.gy, p.lx, p.ly);
            p.lz = fz;
            p.yaw = yaw;
            p.pitch = pitch;
            p.walking = coop_walk_active;
            p.airborne = coop_walk_active && !walk.on_ground;
            coop.send_pose(p);
        }
        // remote avatars
        for (auto& [id, a] : coop_avatars) a.seen = false;
        const auto users = coop.users();
        for (const auto& p : coop.poses()) {
            auto& a = coop_avatars[p.user];
            a.seen = true;
            for (const auto& u : users)
                if (u.id == p.user) {
                    a.name = u.name;
                    std::memcpy(a.color, u.color, sizeof(a.color));
                }
            float x = 0.f, y = 0.f;
            coop_from_tile(p.gx, p.gy, p.lx, p.ly, x, y);
            av_step(a, x, y, p.lz, p.yaw, p.pitch, p.walking, p.airborne, dt);
            av_draw(a, true);
        }
        for (auto& [id, a] : coop_avatars)
            if (!a.seen) av_hide(a);
    };

    // ===== windows =====
    struct CoopUi {
        char name[64]{};
        float color[3]{};
        bool profile_loaded = false;
        int port = 47123;
        char join[64]{};
        char chat[512]{};
        char manual_addr[64]{};
        int addr_idx = 0;
        bool show_invite = false;
        int share_mode = -1;     // invite code: -1 not chosen yet, 0 local network only, 1 online (internet)
        bool intro_seen = false; // co-op intro shown this run
        int intro_step = 0;      // 1 what co-op is, 2 the network permission
        bool friends_open = false;
        std::vector<std::string> dm_open; // friend guids with an open chat tab
        bool chat_min = false;            // chat widget minimized to its (blinking) bar
        std::unordered_map<std::string, std::string> dm_buf;
        std::string err;
    };
    auto coop_ui = std::make_shared<CoopUi>();
    auto fmt_time = [](std::int64_t t) {
        std::time_t tt = static_cast<std::time_t>(t);
        std::tm tm{};
        localtime_s(&tm, &tt);
        char b[16];
        std::strftime(b, sizeof(b), "%H:%M", &tm);
        return std::string(b);
    };
    auto draw_chat_lines = [&](const std::string& channel, float height) {
        ImGui::BeginChild(("##chat_" + channel).c_str(), ImVec2(-FLT_MIN, height), ImGuiChildFlags_Borders);
        const auto lines = coop.chat(channel);
        const std::size_t first = lines.size() > 300 ? lines.size() - 300 : 0;
        for (std::size_t i = first; i < lines.size(); ++i) {
            const auto& l = lines[i];
            if (l.system) {
                ImGui::TextDisabled("%s  %s", fmt_time(l.t).c_str(), l.text.c_str());
            } else {
                ImGui::TextDisabled("%s", fmt_time(l.t).c_str());
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.f, 1.f), "%s:", l.from_name.c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", l.text.c_str());
            }
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.f) ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
        coop.mark_read(channel);
    };
    auto open_dm = [&](const std::string& guid) {
        auto& v = coop_ui->dm_open;
        if (std::find(v.begin(), v.end(), guid) == v.end()) v.push_back(guid);
        coop_ui->chat_min = false;
    };
    // Session UI: a tab of the side panel ("Sesion", between Tile and Objeto) instead of floating windows.
    auto draw_coop_session_tab = [&]() {
        auto& st = panel.imgui_state();
        auto& ui = *coop_ui;
        auto& prof = coop.profile();
        (void)st;
        (void)prof;
        if (!ui.profile_loaded) {
            std::snprintf(ui.name, sizeof(ui.name), "%s", prof.name.c_str());
            std::memcpy(ui.color, prof.color, sizeof(ui.color));
            ui.profile_loaded = true;
        }
        const auto role = coop.role();
        {
                // ---- profile ----
                if (ImGui::CollapsingHeader("Mi perfil", role == omsi::coop::Role::None ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                    ImGui::SetNextItemWidth(220.f);
                    ImGui::InputText("Nombre", ui.name, sizeof(ui.name));
                    ImGui::SameLine();
                    if (ImGui::ColorEdit3("Color del muneco", ui.color, ImGuiColorEditFlags_NoInputs)) {
                        std::memcpy(prof.color, ui.color, sizeof(prof.color)); // live: the others see it at once
                        coop.save_profile();
                    }
                    if (ImGui::Button("Guardar perfil")) {
                        prof.name = ui.name[0] ? ui.name : "Editor";
                        std::memcpy(prof.color, ui.color, sizeof(prof.color));
                        coop.save_profile();
                    }
                    ImGui::SameLine();
                    const auto docs = std::filesystem::path(cfg.omsi_root) / "F4_perfil_coop";
                    if (ImGui::Button("Exportar perfil")) {
                        if (coop.export_profile(docs)) panel.set_status("Perfil exportado a " + omsi::path_text(docs));
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Copia nombre, color, amigos y chats a %s\npara llevarlos a otra PC.",
                                          omsi::path_text(docs).c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Importar perfil")) {
                        if (coop.import_profile(docs)) {
                            ui.profile_loaded = false;
                            panel.set_status("Perfil importado");
                        } else {
                            panel.set_status("No hay perfil en " + omsi::path_text(docs));
                        }
                    }
                    ImGui::TextDisabled("Cuenta local. Inicio con Google: ver nota en el tooltip.");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Sincronizar amigos y chats con una cuenta de Google necesita registrar una\n"
                                          "app OAuth en Google Cloud (client id) y guardar los datos en tu Google Drive\n"
                                          "(carpeta privada de la app). Mientras tanto: Exportar / Importar perfil.");
                }
                ImGui::Separator();
                if (role == omsi::coop::Role::None) {
                    // ---- create / join ----
                    ImGui::TextUnformatted("Crear una sala con el mapa abierto");
                    ImGui::SetNextItemWidth(110.f);
                    ImGui::InputInt("Puerto", &ui.port);
                    ui.port = std::clamp(ui.port, 1024, 65535);
                    ImGui::SameLine();
                    // (without the network permission yet: the permission dialog first)
                    if (ImGui::Button("Crear sala") && !coop.network_consent()) ui.intro_step = 2;
                    else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && coop.network_consent()) {
                        std::filesystem::path live = cfg.workspace && !cfg.workspace->actual_dir.empty()
                                                         ? map::workspace_live_map_dir(*cfg.workspace)
                                                         : map::live_map_dir_from_folder(cfg.global.root);
                        if (live.empty()) live = cfg.global.root;
                        std::string err;
                        if (!coop.host(cfg.omsi_root, live, static_cast<std::uint16_t>(ui.port), err)) ui.err = err;
                        else {
                            ui.err.clear();
                            ui.show_invite = true;
                        }
                    }
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Unirse con un codigo");
                    ImGui::SetNextItemWidth(260.f);
                    ImGui::InputTextWithHint("##join", "F4-XXXX-XXXX-XXXX-XXXX", ui.join, sizeof(ui.join));
                    ImGui::SameLine();
                    if (ImGui::Button("Unirse")) {
                        if (!coop.network_consent()) {
                            ui.intro_step = 2;
                        } else {
                            std::string err;
                            if (!coop.join(ui.join, cfg.omsi_root, err)) ui.err = err;
                            else ui.err.clear();
                        }
                    }
                    const auto lan = coop.lan_peers();
                    bool any_room = false;
                    for (const auto& lp : lan) any_room |= !lp.code.empty();
                    if (any_room) {
                        ImGui::Spacing();
                        ImGui::TextUnformatted("Salas en tu red (LAN)");
                        for (const auto& lp : lan) {
                            if (lp.code.empty()) continue;
                            ImGui::PushID(lp.guid.c_str());
                            ImGui::BulletText("%s  (%s)", lp.name.c_str(), lp.ip.c_str());
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Unirse")) {
                                std::string err;
                                if (!coop.join(lp.code, cfg.omsi_root, err)) ui.err = err;
                            }
                            ImGui::PopID();
                        }
                    }
                    if (!ui.err.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", ui.err.c_str());
                    ImGui::Spacing();
                    ImGui::TextWrapped("En la misma red funciona directo. Por internet el editor abre solo el puerto "
                                       "en el router del anfitrion (UPnP) y pone su IP publica en el codigo. Si el router "
                                       "no tiene UPnP o el proveedor usa CGNAT: abrir el puerto a mano o usar Tailscale / "
                                       "ZeroTier / Radmin. Windows puede pedir permiso de firewall: aceptar. No hay "
                                       "servidor externo.");
                } else {
                    // ---- in a room ----
                    ImGui::Text("%s  |  mapa: %s", role == omsi::coop::Role::Host ? "Anfitrion" : "Invitado",
                                coop.map_name().c_str());
                    ImGui::TextDisabled("%s", coop.status().c_str());
                    if (role == omsi::coop::Role::Host) {
                        const std::string inet = coop.internet_status();
                        if (!inet.empty()) ImGui::TextWrapped("%s", inet.c_str());
                    }
                    if (coop.editing_in_ram())
                        ImGui::TextColored(ImVec4(0.45f, 0.9f, 1.f, 1.f),
                                           "Version del anfitrion en RAM (%.1f MB): tu mapa en disco no se toca.",
                                           static_cast<double>(omsi::vfs::ram_bytes()) / (1024.0 * 1024.0));
                    else if (coop.editing_host_copy())
                        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                                           "Editando la copia del anfitrion: los cambios se guardan en su mapa.");
                    {
                        const std::string ast = coop.assets_status();
                        if (!ast.empty()) ImGui::TextColored(ImVec4(0.45f, 0.9f, 1.f, 1.f), "%s", ast.c_str());
                    }
                    if (role == omsi::coop::Role::Host && ImGui::Button("Invitar")) ui.show_invite = !ui.show_invite;
                    if (role == omsi::coop::Role::Host) ImGui::SameLine();
                    char fl[48];
                    std::snprintf(fl, sizeof(fl), "Amigos (%d)", static_cast<int>(coop.friends().size()));
                    if (ImGui::Button(fl)) ui.friends_open = !ui.friends_open;
                    ImGui::SameLine();
                    if (ImGui::Button(coop_walk_active ? "Salir modo peaton" : "Modo peaton")) {
                        if (coop_walk_active) coop_walk_exit();
                        else coop_walk_enter();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Salir de la sala")) {
                        coop_walk_exit();
                        coop.leave();
                    }
                    if (ui.show_invite && role == omsi::coop::Role::Host) {
                        ImGui::Separator();
                        ImGui::TextUnformatted("Compartir codigo de acceso");
                        if (ImGui::RadioButton("Solo red local", ui.share_mode == 0)) ui.share_mode = 0;
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Amigos en tu misma red (casa, LAN, VPN tipo Radmin / ZeroTier / Tailscale).");
                        ImGui::SameLine();
                        if (ImGui::RadioButton("Online (internet)", ui.share_mode == 1)) ui.share_mode = 1;
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Amigos en otras casas: un tunel seguro de Cloudflare, sin abrir puertos.");
                        std::string code;
                        bool waiting = false;
                        const auto addrs = coop.local_addresses();
                        if (ui.share_mode == 0) {
                            // the first address of this PC on a private network (no tunnel, no public IP)
                            for (const auto& a2 : addrs)
                                if (a2.rfind("tunel:", 0) != 0 && a2 != coop.public_address()) {
                                    code = coop.invite_code(a2);
                                    break;
                                }
                            if (code.empty()) ImGui::TextDisabled("Sin red local: conectate a una red o elegi Online.");
                        } else if (ui.share_mode == 1) {
                            for (const auto& a2 : addrs)
                                if (a2.rfind("tunel:", 0) == 0) {
                                    code = coop.invite_code(a2);
                                    break;
                                }
                            waiting = code.empty();
                            if (waiting) {
                                // a spinner while cloudflared is downloaded / the tunnel connects
                                const float t = static_cast<float>(ImGui::GetTime());
                                const ImVec2 pos = ImGui::GetCursorScreenPos();
                                const ImVec2 ctr(pos.x + 9.f, pos.y + 9.f);
                                ImDrawList* dl = ImGui::GetWindowDrawList();
                                dl->PathArcTo(ctr, 7.f, t * 6.f, t * 6.f + 4.2f, 24);
                                dl->PathStroke(IM_COL32(120, 200, 255, 255), 0, 2.5f);
                                ImGui::Dummy(ImVec2(20.f, 18.f));
                                ImGui::SameLine();
                                const std::string inet = coop.internet_status();
                                ImGui::TextWrapped("Preparando la conexion por internet...%s%s", inet.empty() ? "" : "\n",
                                                   inet.c_str());
                            }
                        } else {
                            ImGui::TextDisabled("Elegi si tus amigos estan en tu red local o por internet.");
                        }
                        if (!code.empty()) ImGui::Text("Codigo: %s", code.c_str());
                        ImGui::BeginDisabled(code.empty()); // (no session / no code yet: nothing to copy)
                        if (ImGui::Button("Copiar codigo") && !code.empty()) {
                            ImGui::SetClipboardText(code.c_str());
                            panel.set_status("Codigo copiado: pasaselo a tus amigos");
                        }
                        ImGui::EndDisabled();
                        if (!code.empty())
                            ImGui::TextDisabled("Pasale el codigo a quien quieras invitar (puerto %u).", coop.port());
                    }
                    ImGui::Separator();
                    // ---- users ----
                    const auto users = coop.users();
                    const int me = coop.my_id();
                    if (ImGui::BeginTable("##coop_users", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                        for (const auto& u : users) {
                            ImGui::PushID(u.id);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::ColorButton("##c", ImVec4(u.color[0], u.color[1], u.color[2], 1.f),
                                               ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
                            ImGui::SameLine();
                            ImGui::Text("%s%s%s", u.name.c_str(), u.host ? "  (anfitrion)" : "", u.id == me ? "  (vos)" : "");
                            ImGui::TableNextColumn();
                            if (u.id != me) {
                                if (coop.is_friend(u.guid)) ImGui::TextDisabled("amigo");
                                else if (ImGui::SmallButton("Agregar amigo")) coop.add_friend(u.guid, u.name, u.color);
                            }
                            ImGui::TableNextColumn();
                            if (u.id != me) {
                                const int un = coop.unread(u.guid);
                                char cl[32];
                                std::snprintf(cl, sizeof(cl), un ? "Chat (%d)" : "Chat", un);
                                if (ImGui::SmallButton(cl)) open_dm(u.guid);
                            }
                            ImGui::TableNextColumn();
                            if (role == omsi::coop::Role::Host && u.id != me && ImGui::SmallButton("Expulsar")) coop.kick(u.id);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TextDisabled("El chat esta abajo a la izquierda (se minimiza y parpadea con mensajes nuevos).");
                }
                if (role == omsi::coop::Role::None) {
                    ImGui::Separator();
                    char fl[48];
                    std::snprintf(fl, sizeof(fl), "Amigos (%d)", static_cast<int>(coop.friends().size()));
                    if (ImGui::Button(fl)) ui.friends_open = !ui.friends_open;
                    ImGui::SameLine();
                    if (ImGui::Button(coop_walk_active ? "Salir modo peaton" : "Modo peaton")) {
                        if (coop_walk_active) coop_walk_exit();
                        else coop_walk_enter();
                    }
                }
        }
        // ---- friends (inside the tab) ----
        ImGui::SetNextItemOpen(ui.friends_open, ImGuiCond_Always);
        ui.friends_open = ImGui::CollapsingHeader("Amigos");
        if (ui.friends_open) {
            {
                const auto fr = coop.friends();
                if (fr.empty()) ImGui::TextDisabled("Todavia no agregaste amigos (boton en la lista de la sala).");
                for (const auto& f : fr) {
                    ImGui::PushID(f.guid.c_str());
                    const bool on = coop.friend_online(f.guid);
                    ImGui::TextColored(on ? ImVec4(0.3f, 0.9f, 0.35f, 1.f) : ImVec4(0.5f, 0.5f, 0.5f, 1.f), on ? "o" : "o");
                    ImGui::SameLine();
                    ImGui::Text("%s  %s", f.name.c_str(), on ? "(en linea)" : "(desconectado)");
                    ImGui::SameLine();
                    const int un = coop.unread(f.guid);
                    char cl[32];
                    std::snprintf(cl, sizeof(cl), un ? "Chat (%d)" : "Chat", un);
                    if (ImGui::SmallButton(cl)) open_dm(f.guid);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Borrar")) coop.remove_friend(f.guid);
                    ImGui::PopID();
                }
                ImGui::Separator();
                ImGui::TextDisabled("En linea = en tu sala o en tu misma red. Los mensajes a un amigo\n"
                                    "se entregan cuando los dos estan en la misma sala.");
            }
        }
    };
    auto draw_coop_ui = [&]() {
        auto& st = panel.imgui_state();
        auto& ui = *coop_ui;
        auto& prof = coop.profile();
        if (!ui.profile_loaded) {
            std::snprintf(ui.name, sizeof(ui.name), "%s", prof.name.c_str());
            std::memcpy(ui.color, prof.color, sizeof(ui.color));
            ui.profile_loaded = true;
        }
        const auto role = coop.role();
        // (walk mode's controls and crosshair: map_editor_run_loop_04, one hint for every walk mode)

        // Co-op mode: before Windows asks about its firewall, what co-op is and how to use it; then the
        // permission it needs, and only on "Aceptar" the network opens (and Windows asks). Once a run until
        // agreed; the editor waits on the dialog meanwhile.
        if (!coop.network_consent() && (ui.intro_step > 0 || !ui.intro_seen)) {
            if (ui.intro_step == 0) ui.intro_step = 1;
            ui.intro_seen = true;
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::GetForegroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                          IM_COL32(0, 0, 0, 140));
            ImGui::OpenPopup("##coop_intro");
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                                    ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(520.f, 0.f));
            if (ImGui::BeginPopupModal("##coop_intro", nullptr,
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
                if (ui.intro_step == 1) {
                    ImGui::TextUnformatted("Modo cooperativo");
                    ImGui::Separator();
                    ImGui::TextWrapped("Este editor cuenta con modo cooperativo: construi un mapa junto a tus amigos, "
                                       "cada uno en su PC, viendo en vivo lo que hacen los demas.");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Como usarlo:");
                    ImGui::BulletText("Panel lateral > pestana Sesion.");
                    ImGui::BulletText("Anfitrion: \"Crear sala\" y despues \"Invitar\".");
                    ImGui::BulletText("Elegi \"Solo red local\" (misma casa / LAN) u \"Online\" (por internet).");
                    ImGui::BulletText("Copia el codigo y pasaselo a tus amigos.");
                    ImGui::BulletText("Invitados: pegan el codigo en \"Unirse con un codigo\".");
                    ImGui::Spacing();
                    if (ImGui::Button("Continuar", ImVec2(140.f, 0.f))) ui.intro_step = 2;
                    ImGui::SameLine();
                    if (ImGui::Button("Ahora no", ImVec2(140.f, 0.f))) {
                        ui.intro_step = 0;
                        ImGui::CloseCurrentPopup();
                    }
                } else {
                    ImGui::TextUnformatted("Permiso de red");
                    ImGui::Separator();
                    ImGui::TextWrapped("Para usar el modo cooperativo el editor necesita conectarse a la red "
                                       "(tu red local y, si lo elegis, internet). Al aceptar, Windows te va a "
                                       "preguntar si permitis el acceso a la red a este programa: elegi Permitir.");
                    ImGui::Spacing();
                    if (ImGui::Button("Aceptar", ImVec2(140.f, 0.f))) {
                        coop.give_network_consent(); // (Windows' firewall question comes now)
                        ui.intro_step = 0;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Ahora no", ImVec2(140.f, 0.f))) {
                        ui.intro_step = 0;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
        }

        // name tags over the avatars
        if (role != omsi::coop::Role::None && !coop_avatars.empty()) {
            float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
            input::cursor_to_framebuffer(window, mx, my, fw, fh);
            float wvp[16]{};
            camera.build_world_view_proj(fh > 0.f ? fw / fh : 1.6f, wvp);
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            for (const auto& [id, a] : coop_avatars) {
                if (!a.seen || !a.placed) continue;
                float sx = 0.f, sy = 0.f;
                if (!world_to_screen(a.x, a.y, a.z + 2.f, 0.f, 0.f, fw, fh, wvp, sx, sy)) continue;
                const ImVec2 p(sx * disp.x / (std::max)(1.f, fw), sy * disp.y / (std::max)(1.f, fh));
                const ImVec2 ts = ImGui::CalcTextSize(a.name.c_str());
                const ImVec2 q(p.x - ts.x * 0.5f, p.y - ts.y);
                const ImU32 col = IM_COL32(static_cast<int>(a.color[0] * 255), static_cast<int>(a.color[1] * 255),
                                           static_cast<int>(a.color[2] * 255), 255);
                dl->AddRectFilled(ImVec2(q.x - 4, q.y - 2), ImVec2(q.x + ts.x + 4, q.y + ts.y + 2), IM_COL32(0, 0, 0, 160), 4.f);
                dl->AddText(q, col, a.name.c_str());
            }
        }

        // guest: map version choice / transfer
        const auto vs = coop.version_state();
        if (role == omsi::coop::Role::Guest &&
            (vs == omsi::coop::VersionState::Differs || vs == omsi::coop::VersionState::Transferring ||
             vs == omsi::coop::VersionState::Comparing)) {
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(520.f, 0.f), ImGuiCond_Appearing);
            if (ImGui::Begin("Version del mapa de la sala", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
                if (vs == omsi::coop::VersionState::Comparing) {
                    ImGui::TextUnformatted("Comparando tu mapa con el del anfitrion...");
                } else if (vs == omsi::coop::VersionState::Transferring) {
                    ImGui::TextUnformatted("Recibiendo la carpeta del mapa del anfitrion...");
                    ImGui::ProgressBar(coop.transfer_progress(), ImVec2(-FLT_MIN, 0.f));
                } else {
                    ImGui::TextWrapped("%s", coop.version_summary().c_str());
                    ImGui::Spacing();
                    ImGui::TextWrapped("Solo se recibe la carpeta maps\\%s. Objetos, splines y vehiculos nunca se "
                                       "envian: lo que te falte se ve como un cubo con su nombre.",
                                       coop.map_name().c_str());
                    ImGui::Separator();
                    if (ImGui::Button("Actualizar mi mapa", ImVec2(240.f, 0.f))) coop.choose_update_my_map();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("El anfitrion envia sus .map / .cfg / terreno nuevos y reemplazan los tuyos\n"
                                          "(antes se guarda una copia de seguridad de lo que cambia).");
                    ImGui::SameLine();
                    if (ImGui::Button("Editar version del anfitrion (solo RAM)", ImVec2(-FLT_MIN, 0.f)))
                        coop.choose_host_version_ram();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Recomendado: el anfitrion te envia solo lo que cambia y queda en memoria.\n"
                                          "No se escribe ni se descarga nada en disco, tu mapa queda intacto\n"
                                          "(bloqueado contra escritura mientras dure la sesion) y lo que edites\n"
                                          "se guarda solo en el mapa del anfitrion. Al salir se recarga tu version.");
                    if (ImGui::Button("Editar en la version del anfitrion", ImVec2(240.f, 0.f))) coop.choose_host_version();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Tu mapa queda como esta. Se abre una copia con la version del anfitrion\n"
                                          "(maps\\%s_coop) y lo que edites se guarda solo en el anfitrion.",
                                          coop.map_name().c_str());
                    if (ImGui::Button("Salir de la sala", ImVec2(-FLT_MIN, 0.f))) coop.leave();
                }
            }
            ImGui::End();
        }

        // ---- chat widget (bottom left, Roblox style): tabs Sala + direct chats, minimizes to a bar
        //      that blinks while there are unread messages ----
        const bool any_chat = role != omsi::coop::Role::None || !ui.dm_open.empty();
        if (any_chat) {
            std::vector<std::string> chans{""};
            for (const auto& g : ui.dm_open) chans.push_back(g);
            int unread_total = 0;
            for (const auto& c : chans) unread_total += coop.unread(c);
            auto chan_name = [&](const std::string& guid) {
                if (guid.empty()) return std::string("Sala");
                std::string name = guid.substr(0, 6);
                for (const auto& f : coop.friends())
                    if (f.guid == guid) name = f.name;
                for (const auto& u : coop.users())
                    if (u.guid == guid) name = u.name;
                return name;
            };
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const bool blink = std::fmod(ImGui::GetTime(), 1.0) < 0.5;
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.f, vp->WorkPos.y + vp->WorkSize.y - 40.f), ImGuiCond_FirstUseEver,
                                    ImVec2(0.f, 1.f));
            if (ui.chat_min) {
                ImGui::SetNextWindowBgAlpha(0.f);
                if (ImGui::Begin("##coop_chat_min", nullptr,
                                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
                    char lab[48];
                    std::snprintf(lab, sizeof(lab), unread_total ? "Chat (%d)" : "Chat", unread_total);
                    const bool flash = unread_total > 0 && blink;
                    if (flash) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.55f, 0.1f, 1.f));
                    if (ImGui::Button(lab, ImVec2(110.f, 26.f))) ui.chat_min = false;
                    if (flash) ImGui::PopStyleColor();
                    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); // always above the panels
                }
                ImGui::End();
            } else {
                ImGui::SetNextWindowSize(ImVec2(400.f, 300.f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowBgAlpha(0.88f);
                if (ImGui::Begin("Chat##coop_chat", nullptr, ImGuiWindowFlags_NoCollapse)) {
                    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); // always above the panels
                    if (ImGui::SmallButton("_ minimizar")) ui.chat_min = true;
                    if (ImGui::BeginTabBar("##chat_tabs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
                        for (std::size_t ci = 0; ci < chans.size(); ++ci) {
                            const std::string& ch = chans[ci];
                            const int un = coop.unread(ch);
                            std::string lab = chan_name(ch) + (un ? " (" + std::to_string(un) + ")" : std::string()) + "##t" + ch;
                            bool keep = true;
                            const bool flash = un > 0 && blink;
                            if (flash) ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.95f, 0.55f, 0.1f, 1.f));
                            const bool open_tab = ImGui::BeginTabItem(lab.c_str(), ch.empty() ? nullptr : &keep);
                            if (flash) ImGui::PopStyleColor();
                            if (open_tab) {
                                draw_chat_lines(ch, (std::max)(60.f, ImGui::GetContentRegionAvail().y - 30.f));
                                auto& buf = ui.dm_buf[ch];
                                char tmp[512];
                                std::snprintf(tmp, sizeof(tmp), "%s", buf.c_str());
                                ImGui::SetNextItemWidth(-64.f);
                                const bool enter = ImGui::InputText("##in", tmp, sizeof(tmp), ImGuiInputTextFlags_EnterReturnsTrue);
                                buf = tmp;
                                ImGui::SameLine();
                                if ((ImGui::Button("Enviar") || enter) && !buf.empty()) {
                                    coop.send_chat(ch, buf);
                                    buf.clear();
                                    ImGui::SetKeyboardFocusHere(-1);
                                }
                                ImGui::EndTabItem();
                            }
                            if (!keep) {
                                ui.dm_open.erase(std::remove(ui.dm_open.begin(), ui.dm_open.end(), ch), ui.dm_open.end());
                                break;
                            }
                        }
                        ImGui::EndTabBar();
                    }
                }
                ImGui::End();
            }
        }
    };
