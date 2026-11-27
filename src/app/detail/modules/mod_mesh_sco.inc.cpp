// Mesh edit + SCO path editor (v105). Included after world_to_screen.
    struct MeshVert {
        float x = 0.f, y = 0.f, z = 0.f;
        int src_kind = 0;
        std::int32_t src_id = 0;
        int next_id = -1;
        int prev_id = -1;
    };
    struct MeshEdge {
        int a = 0, b = 0;
    };
    struct MeshFace {
        int i0 = 0, i1 = 0, i2 = 0;
        int mat = 0;
        float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 0.f, u2 = 0.f, v2 = 1.f;
        int slot = -1; // material slot (MeshEdit::slots), -1 none
    };
    struct MeshSnap {
        std::vector<MeshVert> verts;
        std::vector<MeshEdge> edges;
        std::vector<MeshFace> faces;
        std::unordered_set<int> sel_verts;
        std::unordered_set<int> sel_edges;
        std::unordered_set<int> sel_faces;
        std::unordered_set<int> sel_uv;
    };
    struct MeshXform {
        bool active = false;
        int mode = 0;
        int axis = -1;
        int scale_mask = 7;
        float start_mx = 0.f, start_my = 0.f;
        float pivot_x = 0.f, pivot_y = 0.f, pivot_z = 0.f;
        float disp_x = 0.f, disp_y = 0.f, disp_z = 0.f;
        float disp_angle = 0.f, disp_scale = 1.f;
        int cube_from = -1;
        bool did_cap = false;
        bool skip_weld = false;
        bool uv_lock = false;
        // X/Y/Z constraints follow the selection (face normal = Z, dominant edge = X), not world.
        bool local = false;
        float basis[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
        bool sel_was = false;
        bool snap_hit = false;
        char snap_kind = 'V';
        float snap_tx = 0.f, snap_ty = 0.f, snap_tz = 0.f;
        float snap_ax = 0.f, snap_ay = 0.f, snap_az = 0.f;
        float snap_bx = 0.f, snap_by = 0.f, snap_bz = 0.f;
        float snap_c0x = 0.f, snap_c0y = 0.f, snap_c0z = 0.f;
        float snap_c1x = 0.f, snap_c1y = 0.f, snap_c1z = 0.f;
        float snap_c2x = 0.f, snap_c2y = 0.f, snap_c2z = 0.f;
        std::vector<MeshVert> original;
        std::vector<int> affected;
        std::vector<float> inx, iny, inz;
        float inset_thick = 0.f;
    };
    struct MeshEdit {
        enum class Pick { Verts, Edges, Faces };
        Pick pick = Pick::Verts;
        std::vector<MeshVert> verts;
        std::vector<MeshEdge> edges;
        std::vector<MeshFace> faces;
        std::unordered_set<int> sel_verts;
        std::unordered_set<int> sel_edges;
        std::unordered_set<int> sel_faces;
        std::unordered_set<int> sel_uv;
        int last_vert = -1;
        int last_edge = -1;
        int last_face = -1;
        int hover_v = -1;
        int hover_e = -1;
        int hover_f = -1;
        int snap_mode = 0;
        int tex_sel = 0;
        std::vector<std::filesystem::path> tex_list;
        // Material slots: every face created while a slot is active goes into it; giving the slot a
        // texture retextures all of its triangles at once.
        struct Slot {
            std::string name;
            int tex = -1; // tex_list index, -1 none yet
        };
        std::vector<Slot> slots;
        int active_slot = -1;
        std::deque<MeshSnap> sel_undo;
        std::deque<MeshSnap> sel_redo;
        std::deque<MeshSnap> geo_undo;
        std::deque<MeshSnap> geo_redo;
        bool last_event_sel = false;
        bool dirty = false;
        float weld_m = 0.02f;
        bool shortcuts_collapsed = false;
        std::filesystem::path session_bake_dir;
        MeshSnap enter_snap;
        std::size_t preview_chunk = static_cast<std::size_t>(-1);
        std::vector<std::size_t> preview_chunks;
        std::size_t sel_overlay_chunk = static_cast<std::size_t>(-1);
        // Soft grey outline of every face (depth-tested wireframe copy of the preview).
        std::size_t edge_overlay_chunk = static_cast<std::size_t>(-1);
        // Viewport shading of the edited mesh: 0 solid (grey, lit), 1 wireframe, 2 render (textured).
        int shading = 2;
        std::vector<MeshVert> src_verts;
        std::vector<MeshEdge> src_edges;
        std::vector<MeshFace> src_tris;
        int src_kind = 0;
        std::int32_t src_id = 0;
        float src_px = 0.f, src_py = 0.f, src_pz = 0.f, src_len = 0.f;
        int src_hover_v = -1;
        int src_hover_e = -1;
        int src_hover_f = -1;
        std::vector<MeshVert> env_verts;
        std::vector<MeshEdge> env_edges;
        std::vector<MeshFace> env_tris;
        std::vector<std::int32_t> env_obj;
        std::vector<std::int32_t> env_spl;
        float hover_mx = 0.f, hover_my = 0.f;
        bool tex_browser_open = false;
        std::filesystem::path tex_browser_cwd;
        char tex_browser_filter[128]{};
        char bake_name[64]{"f4_mesh"};
        bool force_tex = false;
        bool uv_editor_open = false;
        bool uv_win_focused = false;
        bool uv_canvas_hov = false;
        bool uv_srv_ok = false;
        std::uint64_t uv_srv_cpu = 0;
        std::uint64_t uv_srv_gpu = 0;
        int uv_mat = -1;
        std::filesystem::path uv_tex_path;
        float uv_zoom = 1.f;
        float uv_pan_x = 0.f;
        float uv_pan_y = 0.f;
        int uv_hover_f = -1;
        int uv_hover = -1;
        int last_uv = -1;
        bool uv_g = false;
        int uv_xf_mode = 0;
        int uv_axis = -1;
        int uv_scale_mask = 3;
        float uv_g_mx = 0.f;
        float uv_g_my = 0.f;
        float uv_pivot_u = 0.f;
        float uv_pivot_v = 0.f;
        std::vector<int> uv_xf_loops;
        std::vector<float> uv_xf_ou;
        std::vector<float> uv_xf_ov;
        bool uv_wheel_dirty = false;
        double uv_wheel_t = 0.0;
        bool uv_wheel_capped = false;
        bool circle_sel = false;
        float circle_r = 48.f;
        bool circle_stroke_cap = false;
        int cut_hv = 0;
        int cut_n = 1;
        float cut_frac = 0.5f;
        int lc_face = -1;
        int lc_s = -1;
        int lc_e = -1;
        int subdiv_n = 1;
        bool loopcut_on = false;
        bool knife_on = false;
        int knife_axis = -1;
        std::vector<MeshVert> knife_pts;
        struct TexThumb {
            std::filesystem::path path;
            std::uint64_t srv_cpu = 0;
            std::uint64_t srv_gpu = 0;
            bool ok = false;
            int used = 0;
        };
        std::vector<TexThumb> tex_thumbs;
        int tex_thumb_clock = 0;
        int tex_thumb_loads = 0;
        // Edit mesh .o3d: file being edited, where its local origin sits in the editor world, and its
        // original materials (by tex_list index) so Save keeps diffuse/specular/etc.
        std::filesystem::path o3d_edit_path;
        float o3d_ox = 0.f, o3d_oy = 0.f, o3d_oz = 0.f;
        // rotation of the map object the mesh was picked from (rows, p_world = p_local * R); identity
        // when opened from a file. Save maps back with R^T. The object stays hidden while editing.
        float o3d_rot[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
        std::int32_t o3d_obj_id = -1;
        bool o3d_obj_was_hidden = false;
        std::unordered_map<int, o3d::Material> o3d_mats;
        // Pick texture from .sco/.sli (window with checkboxes).
        int tex_pick_kind = 0; // 0 off, 1 .sco, 2 .sli
        bool tex_pick_choose = false;
        bool tex_pick_list_open = false;
        std::string tex_pick_src;
        std::vector<std::filesystem::path> tex_pick_list;
        std::vector<char> tex_pick_on;
        int tex_pick_hover = -1;          // edit_objects / edit_splines index under the mouse
        std::int32_t tex_pick_sel_id = 0; // picked owner (red)
        bool tex_pick_sel_obj = true;
    };
    MeshEdit mesh_edit;
    MeshXform mesh_xform;
    std::function<void()> mesh_edit_body_fn;
    bool mesh_tab_hosted = false;

    auto load_mesh_tex_list = [&]() {
        mesh_edit.tex_list.clear();
        const auto cfgp = cfg.global.root / "f4_mesh_textures.cfg";
        for (const auto& line : cfg::read_cfg_lines(cfgp)) {
            auto s = line;
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (s.empty() || s.front() == '#') continue;
            std::filesystem::path p(s);
            if (p.is_relative()) p = cfg.omsi_root / p;
            mesh_edit.tex_list.push_back(p);
        }
    };
    load_mesh_tex_list();

    auto mesh_tex_append_path = [&](std::filesystem::path abs) -> bool {
        std::error_code ec;
        if (!std::filesystem::exists(abs, ec)) {
            panel.set_status("Textura no existe");
            return false;
        }
        abs = abs.lexically_normal();
        for (int i = 0; i < static_cast<int>(mesh_edit.tex_list.size()); ++i) {
            if (mesh_edit.tex_list[static_cast<std::size_t>(i)].lexically_normal() == abs) {
                mesh_edit.tex_sel = i;
                return true;
            }
        }
        std::filesystem::path stored = abs;
        if (!cfg.omsi_root.empty()) {
            auto rel = std::filesystem::relative(abs, cfg.omsi_root.lexically_normal(), ec);
            if (!ec && !rel.empty() && rel.native().find(L"..") == std::wstring::npos) stored = rel;
        }
        const auto cfgp = cfg.global.root / "f4_mesh_textures.cfg";
        {
            std::ofstream out(cfgp, std::ios::app);
            if (out) out << stored.generic_string() << "\n";
        }
        mesh_edit.tex_list.push_back(abs);
        mesh_edit.tex_sel = static_cast<int>(mesh_edit.tex_list.size()) - 1;
        panel.set_status("Tex " + abs.filename().string());
        return true;
    };

    auto mesh_tex_ext_ok = [](const std::filesystem::path& p) {
        auto e = p.extension().string();
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".bmp" || e == ".png" || e == ".dds" || e == ".tga" || e == ".jpg" || e == ".jpeg";
    };

    auto mesh_tex_browse = [&]() {
        mesh_edit.tex_browser_open = true;
        mesh_edit.tex_browser_filter[0] = 0;
        std::error_code ec;
        auto d = cfg.omsi_root / "Sceneryobjects";
        if (!std::filesystem::is_directory(d, ec)) d = cfg.omsi_root;
        mesh_edit.tex_browser_cwd = d;
        ui::browser_index_ensure(d);
    };

    auto mesh_tex_path = [&]() -> std::filesystem::path {
        if (mesh_edit.tex_sel >= 0 &&
            static_cast<std::size_t>(mesh_edit.tex_sel) < mesh_edit.tex_list.size())
            return mesh_edit.tex_list[static_cast<std::size_t>(mesh_edit.tex_sel)];
        return {};
    };
    auto mesh_tex_path_at = [&](int mat) -> std::filesystem::path {
        if (mat >= 0 && static_cast<std::size_t>(mat) < mesh_edit.tex_list.size())
            return mesh_edit.tex_list[static_cast<std::size_t>(mat)];
        return {};
    };
    auto mesh_is_own_chunk = [&](std::size_t c) {
        if (c == mesh_edit.preview_chunk || c == mesh_edit.sel_overlay_chunk || c == mesh_edit.edge_overlay_chunk)
            return true;
        for (auto p : mesh_edit.preview_chunks)
            if (p == c) return true;
        return false;
    };

    auto mesh_edge_key = [](int a, int b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) |
               static_cast<std::uint32_t>(b);
    };

    auto mesh_ensure_edge = [&](int a, int b) -> int {
        if (a == b) return -1;
        const auto k = mesh_edge_key(a, b);
        for (int i = 0; i < static_cast<int>(mesh_edit.edges.size()); ++i) {
            const auto& e = mesh_edit.edges[static_cast<std::size_t>(i)];
            if (mesh_edge_key(e.a, e.b) == k) return i;
        }
        mesh_edit.edges.push_back(MeshEdge{a, b});
        return static_cast<int>(mesh_edit.edges.size()) - 1;
    };

    auto mesh_find_xyz = [&](float x, float y, float z) -> int {
        const float kDup = mesh_edit.weld_m * mesh_edit.weld_m;
        for (int i = 0; i < static_cast<int>(mesh_edit.verts.size()); ++i) {
            const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
            const float dx = v.x - x, dy = v.y - y, dz = v.z - z;
            if (dx * dx + dy * dy + dz * dz <= kDup) return i;
        }
        return -1;
    };
    auto mesh_intern_xyz = [&](float x, float y, float z, int src_kind = 0, std::int32_t src_id = 0) -> int {
        const float kDup = mesh_edit.weld_m * mesh_edit.weld_m;
        for (int i = 0; i < static_cast<int>(mesh_edit.verts.size()); ++i) {
            auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
            const float dx = v.x - x, dy = v.y - y, dz = v.z - z;
            if (dx * dx + dy * dy + dz * dz <= kDup) {
                if (src_kind != 0 && (v.src_kind != src_kind || v.src_id != src_id)) v.src_kind = 0;
                return i;
            }
        }
        MeshVert nv{};
        nv.x = x;
        nv.y = y;
        nv.z = z;
        nv.src_kind = src_kind;
        nv.src_id = src_id;
        mesh_edit.verts.push_back(nv);
        return static_cast<int>(mesh_edit.verts.size()) - 1;
    };

    auto mesh_capture_snap = [&]() -> MeshSnap {
        MeshSnap s;
        s.verts = mesh_edit.verts;
        s.edges = mesh_edit.edges;
        s.faces = mesh_edit.faces;
        s.sel_verts = mesh_edit.sel_verts;
        s.sel_edges = mesh_edit.sel_edges;
        s.sel_faces = mesh_edit.sel_faces;
        s.sel_uv = mesh_edit.sel_uv;
        return s;
    };
    auto mesh_apply_snap = [&](const MeshSnap& s) {
        mesh_edit.verts = s.verts;
        mesh_edit.edges = s.edges;
        mesh_edit.faces = s.faces;
        mesh_edit.sel_verts = s.sel_verts;
        mesh_edit.sel_edges = s.sel_edges;
        mesh_edit.sel_faces = s.sel_faces;
        mesh_edit.sel_uv = s.sel_uv;
    };
    auto mesh_geo_cap = [&]() {
        const std::size_t cap = mesh_edit.verts.size() > 25000 ? 16u : 32u;
        mesh_edit.geo_undo.push_back(mesh_capture_snap());
        if (mesh_edit.geo_undo.size() > cap) mesh_edit.geo_undo.pop_front();
        mesh_edit.geo_redo.clear();
        mesh_edit.last_event_sel = false;
        mesh_edit.dirty = true;
    };
    auto mesh_sel_cap = [&]() {
        mesh_edit.sel_undo.push_back(mesh_capture_snap());
        if (mesh_edit.sel_undo.size() > 32) mesh_edit.sel_undo.pop_front();
        mesh_edit.sel_redo.clear();
        mesh_edit.last_event_sel = true;
    };
    auto mesh_debug_invariants = [&]() {
#ifndef NDEBUG
        const int nv = static_cast<int>(mesh_edit.verts.size());
        for (const auto& e : mesh_edit.edges) {
            assert(e.a >= 0 && e.b >= 0 && e.a < nv && e.b < nv && e.a != e.b);
        }
        for (const auto& f : mesh_edit.faces) {
            assert(f.i0 >= 0 && f.i1 >= 0 && f.i2 >= 0 && f.i0 < nv && f.i1 < nv && f.i2 < nv);
            assert(f.i0 != f.i1 && f.i1 != f.i2 && f.i2 != f.i0);
        }
#endif
        (void)0;
    };
    auto mesh_cube_uv_face = [&](MeshFace& f) {
        if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) return;
        if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
            static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
            static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
            return;
        const auto& a = mesh_edit.verts[static_cast<std::size_t>(f.i0)];
        const auto& b = mesh_edit.verts[static_cast<std::size_t>(f.i1)];
        const auto& c = mesh_edit.verts[static_cast<std::size_t>(f.i2)];
        const float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
        const float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        const float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (nx * nx + ny * ny + nz * nz < 1e-12f) {
            f.u0 = f.v0 = f.u1 = f.v1 = f.u2 = f.v2 = 0.f;
            static bool logged_uv_degen = false;
            if (!logged_uv_degen) {
                logged_uv_degen = true;
                log::info("Bake: reason=uv_degen");
            }
            return;
        }
        const float ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
        auto proj = [&](const MeshVert& p, float& u, float& v) {
            if (ax >= ay && ax >= az) {
                u = p.y;
                v = p.z;
            } else if (ay >= az) {
                u = p.x;
                v = p.z;
            } else {
                u = p.x;
                v = p.y;
            }
        };
        proj(a, f.u0, f.v0);
        proj(b, f.u1, f.v1);
        proj(c, f.u2, f.v2);
    };
    auto mesh_orient_face_to_camera = [&](MeshFace& f) {
        if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) return;
        if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
            static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
            static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
            return;
        const MeshVert* pa = &mesh_edit.verts[static_cast<std::size_t>(f.i0)];
        const MeshVert* pb = &mesh_edit.verts[static_cast<std::size_t>(f.i1)];
        const MeshVert* pc = &mesh_edit.verts[static_cast<std::size_t>(f.i2)];
        const float cx = (pa->x + pb->x + pc->x) * (1.f / 3.f);
        const float cy = (pa->y + pb->y + pc->y) * (1.f / 3.f);
        const float cz = (pa->z + pb->z + pc->z) * (1.f / 3.f);
        float nx = (pb->y - pa->y) * (pc->z - pa->z) - (pb->z - pa->z) * (pc->y - pa->y);
        float ny = (pb->z - pa->z) * (pc->x - pa->x) - (pb->x - pa->x) * (pc->z - pa->z);
        float nz = (pb->x - pa->x) * (pc->y - pa->y) - (pb->y - pa->y) * (pc->x - pa->x);
        float ex = 0.f, ey = 0.f, ez = 0.f;
        camera.eye_position(ex, ey, ez);
        const float vx = ex - cx, vy = ey - cy, vz = ez - cz;
        if (nx * vx + ny * vy + nz * vz < 0.f) {
            std::swap(f.i1, f.i2);
            std::swap(f.u1, f.u2);
            std::swap(f.v1, f.v2);
            pb = &mesh_edit.verts[static_cast<std::size_t>(f.i1)];
            pc = &mesh_edit.verts[static_cast<std::size_t>(f.i2)];
        }
        float fx = camera.target_x - ex, fy = camera.target_y - ey, fz = camera.target_z - ez;
        const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (fl > 1e-8f) {
            fx /= fl;
            fy /= fl;
            fz /= fl;
        } else {
            fx = 1.f;
            fy = 0.f;
            fz = 0.f;
        }
        float rx = -fy, ry = fx, rz = 0.f;
        float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rl < 1e-8f) {
            rx = 1.f;
            ry = 0.f;
            rz = 0.f;
            rl = 1.f;
        }
        rx /= rl;
        ry /= rl;
        rz /= rl;
        const float ux = fy * rz - fz * ry;
        const float uy = fz * rx - fx * rz;
        const float uz = fx * ry - fy * rx;
        auto proj_cam = [&](const MeshVert& p, float& u, float& v) {
            const float dx = p.x - cx, dy = p.y - cy, dz = p.z - cz;
            u = dx * rx + dy * ry + dz * rz;
            v = dx * ux + dy * uy + dz * uz;
        };
        proj_cam(*pa, f.u0, f.v0);
        proj_cam(*pb, f.u1, f.v1);
        proj_cam(*pc, f.u2, f.v2);
        const float umin = (std::min)(f.u0, (std::min)(f.u1, f.u2));
        const float umax = (std::max)(f.u0, (std::max)(f.u1, f.u2));
        const float vmin = (std::min)(f.v0, (std::min)(f.v1, f.v2));
        const float vmax = (std::max)(f.v0, (std::max)(f.v1, f.v2));
        const float du = umax - umin;
        const float dv = vmax - vmin;
        const float sc = (std::max)(du, dv);
        if (sc > 1e-8f) {
            f.u0 = (f.u0 - umin) / sc;
            f.u1 = (f.u1 - umin) / sc;
            f.u2 = (f.u2 - umin) / sc;
            f.v0 = (f.v0 - vmin) / sc;
            f.v1 = (f.v1 - vmin) / sc;
            f.v2 = (f.v2 - vmin) / sc;
        }
    };
    auto mesh_new_face = [&](int i0, int i1, int i2, int mat) -> MeshFace {
        MeshFace f{};
        f.i0 = i0;
        f.i1 = i1;
        f.i2 = i2;
        f.mat = (mat >= 0 && static_cast<std::size_t>(mat) < mesh_edit.tex_list.size()) ? mat : -1;
        if (mesh_edit.active_slot >= 0 && static_cast<std::size_t>(mesh_edit.active_slot) < mesh_edit.slots.size()) {
            f.slot = mesh_edit.active_slot;
            const int st = mesh_edit.slots[static_cast<std::size_t>(mesh_edit.active_slot)].tex;
            if (st >= 0 && static_cast<std::size_t>(st) < mesh_edit.tex_list.size()) f.mat = st;
        }
        mesh_cube_uv_face(f);
        return f;
    };
    auto mesh_cross_sq = [](const MeshVert& a, const MeshVert& b, const MeshVert& c) {
        const float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
        const float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        const float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        return nx * nx + ny * ny + nz * nz;
    };
    auto mesh_ray_tri = [](const f4f23ma::f4& orig, const f4f23ma::f4& dir, const MeshVert& a, const MeshVert& b,
                           const MeshVert& c, float& t_out) -> bool {
        const f4f23ma::f4 e1{b.x - a.x, b.y - a.y, b.z - a.z};
        const f4f23ma::f4 e2{c.x - a.x, c.y - a.y, c.z - a.z};
        const f4f23ma::f4 pvec = dir.cross3(e2);
        const float det = e1.dot3(pvec);
        if (det > -1e-8f && det < 1e-8f) return false;
        const float inv = 1.f / det;
        const f4f23ma::f4 tvec{orig.x - a.x, orig.y - a.y, orig.z - a.z};
        const float u = tvec.dot3(pvec) * inv;
        if (u < 0.f || u > 1.f) return false;
        const f4f23ma::f4 qvec = tvec.cross3(e1);
        const float v = dir.dot3(qvec) * inv;
        if (v < 0.f || u + v > 1.f) return false;
        const float t = e2.dot3(qvec) * inv;
        if (t <= 1e-6f) return false;
        t_out = t;
        return true;
    };

    auto mesh_src_clear = [&]() {
        mesh_edit.src_verts.clear();
        mesh_edit.src_edges.clear();
        mesh_edit.src_tris.clear();
        mesh_edit.src_kind = 0;
        mesh_edit.src_id = 0;
        mesh_edit.src_hover_v = mesh_edit.src_hover_e = mesh_edit.src_hover_f = -1;
    };

    auto mesh_src_push_world = [&](const XMMATRIX& wm, float x, float y, float z) {
        DirectX::XMFLOAT3 w{};
        DirectX::XMStoreFloat3(&w, xform_point(wm, x, y, z));
        mesh_edit.src_verts.push_back(MeshVert{w.x, w.y, w.z});
    };

    auto mesh_src_fill_object = [&](std::int32_t oid) {
        const map::MapObjectInst* obj = nullptr;
        for (const auto& o : edit_objects)
            if (o.id == oid) {
                obj = &o;
                break;
            }
        if (!obj) return;
        if (mesh_edit.src_kind == 1 && mesh_edit.src_id == oid &&
            std::fabs(mesh_edit.src_px - obj->position.x) < 1e-4f &&
            std::fabs(mesh_edit.src_py - obj->position.y) < 1e-4f &&
            std::fabs(mesh_edit.src_pz - obj->position.z) < 1e-4f)
            return;
        mesh_src_clear();
        mesh_edit.src_kind = 1;
        mesh_edit.src_id = oid;
        mesh_edit.src_px = obj->position.x;
        mesh_edit.src_py = obj->position.y;
        mesh_edit.src_pz = obj->position.z;
        const auto* sco = sco::load_sco(obj->sco_path);
        if (!sco) return;
        bool abs_z = sco::skip_terrain_add(*sco);
        for (const auto& a : edit_attachments)
            if (a.child_id == obj->id) abs_z = true;
        const float th = abs_z ? 0.f : sample_terrain_editor_xy(obj->position.x, obj->position.y);
        const XMMATRIX base = sco_object_base_matrix(*obj, th, 0.f, 0.f);
        std::unordered_set<std::uint64_t> seen;
        constexpr int kMax = 16000;
        auto add_edge_i = [&](int a, int b) {
            if (a == b || a < 0 || b < 0) return;
            const auto k = mesh_edge_key(a, b);
            if (!seen.insert(k).second) return;
            mesh_edit.src_edges.push_back(MeshEdge{a, b});
        };
        auto add_tri_i = [&](int a, int b, int c) {
            if (a < 0 || b < 0 || c < 0 || a == b || b == c || a == c) return;
            if (static_cast<int>(mesh_edit.src_tris.size()) >= kMax) return;
            MeshFace t{};
            t.i0 = a;
            t.i1 = b;
            t.i2 = c;
            mesh_edit.src_tris.push_back(t);
        };
        for (const auto& ref : sco->meshes) {
            if (static_cast<int>(mesh_edit.src_verts.size()) >= kMax) break;
            const auto stem = ref.mesh_file.stem().string();
            if (stem == "empty_lod" || stem == "empty") continue;
            if (ref.is_o3d) {
                auto mapped = o3d::load_mapped_shared(ref.mesh_file);
                if (!mapped || !mapped->valid()) continue;
                std::vector<o3d::Vertex> gv;
                o3d::prepare_o3d_mesh_vertices_for_gpu(mapped->mesh(), gv);
                const XMMATRIX wm =
                    ma4_to_xm(o3d::o3d_unity_root_transform(mapped->mesh().object_rows())) * base;
                const int base_i = static_cast<int>(mesh_edit.src_verts.size());
                for (const auto& v : gv) {
                    if (static_cast<int>(mesh_edit.src_verts.size()) >= kMax) break;
                    mesh_src_push_world(wm, v.position[0], v.position[1], v.position[2]);
                }
                const int nv = static_cast<int>(mesh_edit.src_verts.size()) - base_i;
                auto emit = [&](int i0, int i1, int i2) {
                    if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= nv || i1 >= nv || i2 >= nv) return;
                    add_edge_i(base_i + i0, base_i + i1);
                    add_edge_i(base_i + i1, base_i + i2);
                    add_edge_i(base_i + i2, base_i + i0);
                    add_tri_i(base_i + i0, base_i + i1, base_i + i2);
                };
                if (mapped->mesh().long_triangle_indices) {
                    for (const auto& t : mapped->mesh().triangle_u32_span())
                        emit(static_cast<int>(t.i0), static_cast<int>(t.i1), static_cast<int>(t.i2));
                } else {
                    for (const auto& t : mapped->mesh().triangle_u16_span())
                        emit(static_cast<int>(t.i0), static_cast<int>(t.i1), static_cast<int>(t.i2));
                }
            } else if (!sco->tree.valid) {
                const auto xf = xfile::load_x_file(ref.mesh_file);
                if (!xf) continue;
                for (const auto& xm : xf->parts) {
                    const int base_i = static_cast<int>(mesh_edit.src_verts.size());
                    for (const auto& v : xm.vertices) {
                        if (static_cast<int>(mesh_edit.src_verts.size()) >= kMax) break;
                        mesh_src_push_world(base, v.position[0], v.position[1], v.position[2]);
                    }
                    const int nv = static_cast<int>(mesh_edit.src_verts.size()) - base_i;
                    for (std::size_t ii = 0; ii + 2 < xm.indices.size(); ii += 3) {
                        const int i0 = static_cast<int>(xm.indices[ii]);
                        const int i1 = static_cast<int>(xm.indices[ii + 1]);
                        const int i2 = static_cast<int>(xm.indices[ii + 2]);
                        if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= nv || i1 >= nv || i2 >= nv) continue;
                        add_edge_i(base_i + i0, base_i + i1);
                        add_edge_i(base_i + i1, base_i + i2);
                        add_edge_i(base_i + i2, base_i + i0);
                        add_tri_i(base_i + i0, base_i + i1, base_i + i2);
                    }
                }
            }
        }
    };

    auto mesh_src_fill_spline = [&](std::int32_t sid) {
        const map::SplineInst* focus = nullptr;
        for (const auto& s : edit_splines)
            if (s.id == sid) {
                focus = &s;
                break;
            }
        if (!focus) return;
        if (mesh_edit.src_kind == 2 && mesh_edit.src_id == sid &&
            std::fabs(mesh_edit.src_px - focus->position.x) < 1e-4f &&
            std::fabs(mesh_edit.src_py - focus->position.y) < 1e-4f &&
            std::fabs(mesh_edit.src_pz - focus->position.z) < 1e-4f &&
            std::fabs(mesh_edit.src_len - focus->length) < 1e-4f)
            return;
        auto find_spl = [&](std::int32_t id) -> const map::SplineInst* {
            for (const auto& s : edit_splines)
                if (s.id == id) return &s;
            return nullptr;
        };
        std::vector<std::int32_t> chain;
        {
            std::unordered_set<std::int32_t> seen;
            seen.insert(sid);
            std::vector<std::int32_t> head;
            std::int32_t cur = focus->prev_id;
            for (int hops = 0; hops < 256 && cur != 0; ++hops) {
                if (!seen.insert(cur).second) break;
                const auto* p = find_spl(cur);
                if (!p) break;
                head.push_back(cur);
                cur = p->prev_id;
            }
            std::reverse(head.begin(), head.end());
            chain = std::move(head);
            chain.push_back(sid);
            cur = focus->next_id;
            for (int hops = 0; hops < 256 && cur != 0; ++hops) {
                if (!seen.insert(cur).second) break;
                const auto* n = find_spl(cur);
                if (!n) break;
                chain.push_back(cur);
                cur = n->next_id;
            }
        }
        mesh_src_clear();
        mesh_edit.src_kind = 2;
        mesh_edit.src_id = sid;
        mesh_edit.src_px = focus->position.x;
        mesh_edit.src_py = focus->position.y;
        mesh_edit.src_pz = focus->position.z;
        mesh_edit.src_len = focus->length;
        std::unordered_set<std::uint64_t> seen_e;
        constexpr int kMax = 16000;
        auto add_edge_i = [&](int a, int b) {
            if (a == b || a < 0 || b < 0) return;
            const auto k = mesh_edge_key(a, b);
            if (!seen_e.insert(k).second) return;
            mesh_edit.src_edges.push_back(MeshEdge{a, b});
        };
        auto add_tri_i = [&](int a, int b, int c) {
            if (a < 0 || b < 0 || c < 0 || a == b || b == c || a == c) return;
            if (static_cast<int>(mesh_edit.src_tris.size()) >= kMax) return;
            MeshFace t{};
            t.i0 = a;
            t.i1 = b;
            t.i2 = c;
            mesh_edit.src_tris.push_back(t);
        };
        bool capped = false;
        for (std::int32_t cid : chain) {
            const auto* sp = find_spl(cid);
            if (!sp) continue;
            const auto def = spline::load_sli(sp->sli_path);
            if (!def) continue;
            std::optional<float> chain_z;
            if (const auto it = spline_start_z.find(sp->id); it != spline_start_z.end()) chain_z = it->second;
            const auto meshes = spline::build_spline_meshes(*sp, *def, cfg.omsi_root, 0.f, chain_z);
            for (const auto& m : meshes) {
                const int base_i = static_cast<int>(mesh_edit.src_verts.size());
                for (const auto& v : m.vertices) {
                    if (static_cast<int>(mesh_edit.src_verts.size()) >= kMax) {
                        capped = true;
                        break;
                    }
                    MeshVert mv{};
                    mv.x = v.position[0];
                    mv.y = v.position[1];
                    mv.z = v.position[2];
                    mv.src_kind = 2;
                    mv.src_id = cid;
                    mesh_edit.src_verts.push_back(mv);
                }
                const int nv = static_cast<int>(mesh_edit.src_verts.size()) - base_i;
                for (std::size_t ii = 0; ii + 2 < m.indices.size(); ii += 3) {
                    const int i0 = static_cast<int>(m.indices[ii]);
                    const int i1 = static_cast<int>(m.indices[ii + 1]);
                    const int i2 = static_cast<int>(m.indices[ii + 2]);
                    if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= nv || i1 >= nv || i2 >= nv) continue;
                    add_edge_i(base_i + i0, base_i + i1);
                    add_edge_i(base_i + i1, base_i + i2);
                    add_edge_i(base_i + i2, base_i + i0);
                    add_tri_i(base_i + i0, base_i + i1, base_i + i2);
                }
                if (static_cast<int>(mesh_edit.src_verts.size()) >= kMax ||
                    static_cast<int>(mesh_edit.src_tris.size()) >= kMax) {
                    capped = true;
                    break;
                }
            }
            if (capped) break;
        }
        if (capped) panel.set_status("spline chain: cap 16000");
    };

    // Mesh mode only shows / picks what is within this radius of the camera eye or its orbit target
    // (move the camera to reach farther snap targets).
    struct MeshCamFilter {
        float ex = 0.f, ey = 0.f, ez = 0.f, tx = 0.f, ty = 0.f, tz = 0.f, r = 50.f;
        bool operator()(float x, float y, float z) const {
            const float r2 = r * r;
            const float ax = x - ex, ay = y - ey, az = z - ez;
            if (ax * ax + ay * ay + az * az <= r2) return true;
            const float bx = x - tx, by = y - ty, bz = z - tz;
            return bx * bx + by * by + bz * bz <= r2;
        }
        bool spline(const map::SplineInst& s) const {
            return spline::spline_in_radius_xy(s, ex, ey, r) || spline::spline_in_radius_xy(s, tx, ty, r);
        }
    };
    auto mesh_cam_filter = [&]() {
        MeshCamFilter f{};
        camera.eye_position(f.ex, f.ey, f.ez);
        f.tx = camera.target_x;
        f.ty = camera.target_y;
        f.tz = camera.target_z;
        return f;
    };
    auto mesh_src_sync = [&](float mx, float my, float fw, float fh, const float* wvp) {
        // Hover only changes the source when the mouse moves; re-pick at most every 8 frames otherwise.
        static float s_mx = -1.e9f, s_my = -1.e9f;
        static std::uint64_t s_frame = 0;
        if (std::fabs(mx - s_mx) < 1.f && std::fabs(my - s_my) < 1.f && g_editor_frame_id - s_frame < 8) return;
        s_mx = mx;
        s_my = my;
        s_frame = g_editor_frame_id;
        static std::vector<std::size_t> cand;
        cand.clear();
        const auto nch = renderer.chunk_count();
        const auto& hid_o = panel.imgui_state().hidden_object_ids;
        const auto& hid_s = panel.imgui_state().hidden_spline_ids;
        // Was O(candidates × (rails + markers)) per frame — tens of millions of compares on a full map.
        std::unordered_set<std::size_t> helper;
        helper.reserve(path_rails.size() + end_markers.size());
        for (const auto& r : path_rails)
            if (r.alive) helper.insert(r.chunk);
        for (const auto& mk : end_markers)
            if (mk.alive) helper.insert(mk.chunk);
        auto skip_helper = [&](std::size_t c) {
            if (c >= nch || !renderer.chunk_has_gpu(c)) return true;
            if (mesh_is_own_chunk(c)) return true;
            return helper.count(c) != 0;
        };
        const auto near_cam = mesh_cam_filter();
        for (const auto& sp : scene_places) {
            if (!sp.alive || sp.is_tree) continue;
            if (hid_o.count(sp.object_id)) continue;
            if (!near_cam(sp.tx, sp.ty, sp.tz)) continue;
            if (skip_helper(sp.chunk)) continue;
            cand.push_back(sp.chunk);
        }
        std::unordered_map<std::int32_t, const map::SplineInst*> spl_by_id;
        spl_by_id.reserve(edit_splines.size() * 2);
        for (const auto& s : edit_splines) spl_by_id[s.id] = &s;
        for (const auto& g : spline_gpu) {
            if (!g.alive || hid_s.count(g.id)) continue;
            const auto it = spl_by_id.find(g.id);
            if (it == spl_by_id.end() || !near_cam.spline(*it->second)) continue;
            for (const std::size_t c : g.chunks) {
                if (skip_helper(c)) continue;
                cand.push_back(c);
            }
        }
        if (cand.empty()) return;
        const auto hit = renderer.pick_chunk(mx, my, static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh),
                                            wvp, false, cand.data(), cand.size());
        if (!hit) return;
        for (const auto& sp : scene_places) {
            if (sp.alive && !sp.is_tree && sp.chunk == *hit) {
                mesh_src_fill_object(sp.object_id);
                return;
            }
        }
        for (const auto& g : spline_gpu) {
            if (!g.alive) continue;
            for (const std::size_t c : g.chunks) {
                if (c != *hit) continue;
                mesh_src_fill_spline(g.id);
                return;
            }
        }
    };

    auto mesh_env_sync = [&](float mx, float my, float fw, float fh, const float* wvp, float wx, float wy,
                             float wz) {
        std::vector<std::int32_t> oids, sids;
        const float r2 = 50.f * 50.f;
        const float px2 = 200.f * 200.f;
        auto near_pt = [&](float x, float y, float z) {
            const float dx = x - wx, dy = y - wy, dz = z - wz;
            if (dx * dx + dy * dy + dz * dz <= r2) return true;
            float sx = 0.f, sy = 0.f;
            if (!world_to_screen(x, y, z, 0, 0, fw, fh, wvp, sx, sy)) return false;
            const float ddx = sx - mx, ddy = sy - my;
            return ddx * ddx + ddy * ddy <= px2;
        };
        for (const auto& sp : scene_places) {
            if (!sp.alive || sp.is_tree) continue;
            if (!near_pt(sp.tx, sp.ty, sp.tz)) continue;
            if (std::find(oids.begin(), oids.end(), sp.object_id) == oids.end()) oids.push_back(sp.object_id);
            if (oids.size() >= 8u) break;
        }
        for (const auto& s : edit_splines) {
            if (!near_pt(s.position.x, s.position.y, s.position.z)) continue;
            if (std::find(sids.begin(), sids.end(), s.id) == sids.end()) sids.push_back(s.id);
            if (sids.size() >= 4u) break;
        }
        if (oids == mesh_edit.env_obj && sids == mesh_edit.env_spl) return;
        mesh_edit.env_obj = oids;
        mesh_edit.env_spl = sids;
        const int bak_kind = mesh_edit.src_kind;
        const std::int32_t bak_id = mesh_edit.src_id;
        const float bak_px = mesh_edit.src_px, bak_py = mesh_edit.src_py, bak_pz = mesh_edit.src_pz,
                    bak_len = mesh_edit.src_len;
        auto bak_v = std::move(mesh_edit.src_verts);
        auto bak_e = std::move(mesh_edit.src_edges);
        auto bak_t = std::move(mesh_edit.src_tris);
        mesh_edit.env_verts.clear();
        mesh_edit.env_edges.clear();
        mesh_edit.env_tris.clear();
        auto absorb = [&]() {
            const int off = static_cast<int>(mesh_edit.env_verts.size());
            mesh_edit.env_verts.insert(mesh_edit.env_verts.end(), mesh_edit.src_verts.begin(),
                                       mesh_edit.src_verts.end());
            for (const auto& e : mesh_edit.src_edges)
                mesh_edit.env_edges.push_back(MeshEdge{e.a + off, e.b + off});
            for (const auto& f : mesh_edit.src_tris) {
                MeshFace t = f;
                t.i0 += off;
                t.i1 += off;
                t.i2 += off;
                mesh_edit.env_tris.push_back(t);
            }
        };
        mesh_edit.src_kind = 0;
        mesh_edit.src_id = 0;
        for (std::int32_t oid : oids) {
            mesh_src_fill_object(oid);
            absorb();
            if (static_cast<int>(mesh_edit.env_verts.size()) >= 20000) break;
        }
        for (std::int32_t sid : sids) {
            mesh_src_fill_spline(sid);
            absorb();
            if (static_cast<int>(mesh_edit.env_verts.size()) >= 20000) break;
        }
        mesh_edit.src_verts = std::move(bak_v);
        mesh_edit.src_edges = std::move(bak_e);
        mesh_edit.src_tris = std::move(bak_t);
        mesh_edit.src_kind = bak_kind;
        mesh_edit.src_id = bak_id;
        mesh_edit.src_px = bak_px;
        mesh_edit.src_py = bak_py;
        mesh_edit.src_pz = bak_pz;
        mesh_edit.src_len = bak_len;
    };

    auto mesh_uv_sel_bbox = [&](float& u1, float& v1, float& u2, float& v2) -> bool {
        u1 = v1 = 1.e30f;
        u2 = v2 = -1.e30f;
        bool any = false;
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            const float us[3] = {f.u0, f.u1, f.u2};
            const float vs[3] = {f.v0, f.v1, f.v2};
            for (int k = 0; k < 3; ++k) {
                u1 = (std::min)(u1, us[k]);
                u2 = (std::max)(u2, us[k]);
                v1 = (std::min)(v1, vs[k]);
                v2 = (std::max)(v2, vs[k]);
            }
            any = true;
        }
        return any;
    };
    auto mesh_uv_each_sel = [&](auto&& fn) {
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            fn(f.u0, f.v0);
            fn(f.u1, f.v1);
            fn(f.u2, f.v2);
        }
    };
    auto mesh_uv_nudge = [&](float du, float dv) {
        if (mesh_edit.sel_faces.empty()) {
            panel.set_status("UV: caras");
            return;
        }
        mesh_geo_cap();
        mesh_uv_each_sel([&](float& u, float& v) {
            u += du;
            v += dv;
        });
    };
    auto mesh_uv_center = [&]() {
        float u1, v1, u2, v2;
        if (!mesh_uv_sel_bbox(u1, v1, u2, v2)) {
            panel.set_status("UV: caras");
            return;
        }
        mesh_geo_cap();
        const float cx = (u2 - u1) * 0.5f + u1;
        const float cy = (v2 - v1) * 0.5f + v1;
        const float du = 0.5f - cx;
        const float dv = 0.5f - cy;
        mesh_uv_each_sel([&](float& u, float& v) {
            u += du;
            v += dv;
        });
    };
    auto mesh_uv_zoom = [&](float factor) {
        float u1, v1, u2, v2;
        if (!mesh_uv_sel_bbox(u1, v1, u2, v2)) {
            panel.set_status("UV: caras");
            return;
        }
        mesh_geo_cap();
        const float px = (u2 - u1) * 0.5f + u1;
        const float py = (v2 - v1) * 0.5f + v1;
        mesh_uv_each_sel([&](float& u, float& v) {
            u = px * (1.f - factor) + u * factor;
            v = py * (1.f - factor) + v * factor;
        });
    };
    auto mesh_uv_flip = [&](bool flip_u, bool flip_v) {
        float u1, v1, u2, v2;
        if (!mesh_uv_sel_bbox(u1, v1, u2, v2)) {
            panel.set_status("UV Flip: caras");
            return;
        }
        mesh_geo_cap();
        const float cx = (u1 + u2) * 0.5f;
        const float cy = (v1 + v2) * 0.5f;
        mesh_uv_each_sel([&](float& u, float& v) {
            if (flip_u) u = cx * 2.f - u;
            if (flip_v) v = cy * 2.f - v;
        });
    };

    auto mesh_refresh_preview = [&]() {
        auto hide_chunk = [&](std::size_t c) {
            if (c != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(c, false);
        };
        if (mesh_edit.faces.empty()) {
            hide_chunk(mesh_edit.preview_chunk);
            for (auto c : mesh_edit.preview_chunks) hide_chunk(c);
            hide_chunk(mesh_edit.sel_overlay_chunk);
            hide_chunk(mesh_edit.edge_overlay_chunk);
            return;
        }
        auto emit_tri = [&](std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& inds, const MeshFace& f) {
            if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) return;
            if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                return;
            const auto& a = mesh_edit.verts[static_cast<std::size_t>(f.i0)];
            const auto& b = mesh_edit.verts[static_cast<std::size_t>(f.i1)];
            const auto& c = mesh_edit.verts[static_cast<std::size_t>(f.i2)];
            const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
            o3d::Vertex va{}, vb{}, vc{};
            va.position[0] = a.x;
            va.position[1] = a.y;
            va.position[2] = a.z;
            vb.position[0] = b.x;
            vb.position[1] = b.y;
            vb.position[2] = b.z;
            vc.position[0] = c.x;
            vc.position[1] = c.y;
            vc.position[2] = c.z;
            va.uv[0] = f.u0;
            va.uv[1] = f.v0;
            vb.uv[0] = f.u1;
            vb.uv[1] = f.v1;
            vc.uv[0] = f.u2;
            vc.uv[1] = f.v2;
            const float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
            const float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
            const float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            va.normal[0] = vb.normal[0] = vc.normal[0] = nx;
            va.normal[1] = vb.normal[1] = vc.normal[1] = ny;
            va.normal[2] = vb.normal[2] = vc.normal[2] = nz;
            verts.push_back(va);
            verts.push_back(vb);
            verts.push_back(vc);
            inds.insert(inds.end(), {base, base + 1, base + 2});
        };
        auto upload_batch = [&](std::size_t& slot, const std::vector<o3d::Vertex>& verts,
                                const std::vector<std::uint32_t>& inds, const std::filesystem::path& tex) {
            if (verts.empty()) {
                hide_chunk(slot);
                return;
            }
            if (slot == static_cast<std::size_t>(-1)) {
                if (renderer.append_textured_mesh(verts.data(), static_cast<std::uint32_t>(verts.size()),
                                                  inds.data(), static_cast<std::uint32_t>(inds.size()), tex, 0u))
                    slot = renderer.chunk_count() - 1;
            } else {
                renderer.replace_textured_mesh(slot, verts.data(), static_cast<std::uint32_t>(verts.size()),
                                               inds.data(), static_cast<std::uint32_t>(inds.size()), tex, 0u, false,
                                               false, mesh_edit.force_tex);
            }
            if (slot != static_cast<std::size_t>(-1)) {
                renderer.set_chunk_visible(slot, true);
                const auto wt = store_world_transposed(XMMatrixIdentity());
                renderer.set_chunk_world_matrix(slot, wt.data());
                // shading: solid = neutral grey (lit by the scene), wireframe = lines, render = texture
                renderer.set_chunk_wireframe(slot, mesh_edit.shading == 1);
                if (mesh_edit.shading == 0) renderer.set_chunk_highlight(slot, 0.85f, 0.72f, 0.72f, 0.74f);
                else if (mesh_edit.shading == 1) renderer.set_chunk_highlight(slot, 1.f, 0.85f, 0.85f, 0.88f);
                else renderer.set_chunk_highlight(slot, 0.f, 0.f, 0.f, 0.f);
            }
        };
        const int ntex = static_cast<int>(mesh_edit.tex_list.size());
        const int nslot = ntex + 1;
        if (static_cast<int>(mesh_edit.preview_chunks.size()) < nslot)
            mesh_edit.preview_chunks.resize(static_cast<std::size_t>(nslot), static_cast<std::size_t>(-1));
        std::vector<std::vector<o3d::Vertex>> bv(static_cast<std::size_t>(nslot));
        std::vector<std::vector<std::uint32_t>> bi(static_cast<std::size_t>(nslot));
        for (const auto& f : mesh_edit.faces) {
            int m = f.mat;
            if (m < 0 || m >= ntex) m = ntex;
            emit_tri(bv[static_cast<std::size_t>(m)], bi[static_cast<std::size_t>(m)], f);
        }
        for (int m = 0; m < nslot; ++m)
            upload_batch(mesh_edit.preview_chunks[static_cast<std::size_t>(m)], bv[static_cast<std::size_t>(m)],
                         bi[static_cast<std::size_t>(m)], m < ntex ? mesh_tex_path_at(m) : std::filesystem::path{});
        for (int m = nslot; m < static_cast<int>(mesh_edit.preview_chunks.size()); ++m)
            hide_chunk(mesh_edit.preview_chunks[static_cast<std::size_t>(m)]);
        mesh_edit.preview_chunk =
            mesh_edit.preview_chunks.empty() ? static_cast<std::size_t>(-1) : mesh_edit.preview_chunks[0];
        hide_chunk(mesh_edit.sel_overlay_chunk);
        // Face outlines: all faces in one untextured chunk drawn as soft grey depth-tested lines, so the
        // current faces read clearly in solid / render shading (wireframe shading shows them already).
        if (mesh_edit.shading != 1) {
            std::vector<o3d::Vertex> ev;
            std::vector<std::uint32_t> ei;
            ev.reserve(mesh_edit.faces.size() * 3);
            ei.reserve(mesh_edit.faces.size() * 3);
            for (const auto& f : mesh_edit.faces) emit_tri(ev, ei, f);
            upload_batch(mesh_edit.edge_overlay_chunk, ev, ei, {});
            if (mesh_edit.edge_overlay_chunk != static_cast<std::size_t>(-1) && !ev.empty()) {
                renderer.set_chunk_wireframe(mesh_edit.edge_overlay_chunk, true);
                renderer.set_chunk_highlight(mesh_edit.edge_overlay_chunk, 1.f, 0.55f, 0.56f, 0.6f);
            }
        } else {
            hide_chunk(mesh_edit.edge_overlay_chunk);
        }
        mesh_edit.force_tex = false;
    };
    auto mesh_cube_uv_sel = [&]() {
        if (mesh_edit.sel_faces.empty()) {
            panel.set_status("UV Cube: seleccioná caras");
            return;
        }
        mesh_geo_cap();
        auto axis_of = [&](const MeshFace& f) -> int {
            if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) return 2;
            if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                return 2;
            const auto& a = mesh_edit.verts[static_cast<std::size_t>(f.i0)];
            const auto& b = mesh_edit.verts[static_cast<std::size_t>(f.i1)];
            const auto& c = mesh_edit.verts[static_cast<std::size_t>(f.i2)];
            const float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
            const float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
            const float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            const float ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
            if (ax >= ay && ax >= az) return 0;
            if (ay >= az) return 1;
            return 2;
        };
        auto proj_axis = [&](const MeshVert& p, int axis, float& u, float& v) {
            if (axis == 0) {
                u = p.y;
                v = p.z;
            } else if (axis == 1) {
                u = p.x;
                v = p.z;
            } else {
                u = p.x;
                v = p.y;
            }
        };
        std::vector<int> groups[3];
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            groups[axis_of(mesh_edit.faces[static_cast<std::size_t>(fi)])].push_back(fi);
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (groups[axis].empty()) continue;
            float umin = 1.e30f, vmin = 1.e30f, umax = -1.e30f, vmax = -1.e30f;
            for (int fi : groups[axis]) {
                auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                const auto& a = mesh_edit.verts[static_cast<std::size_t>(f.i0)];
                const auto& b = mesh_edit.verts[static_cast<std::size_t>(f.i1)];
                const auto& c = mesh_edit.verts[static_cast<std::size_t>(f.i2)];
                proj_axis(a, axis, f.u0, f.v0);
                proj_axis(b, axis, f.u1, f.v1);
                proj_axis(c, axis, f.u2, f.v2);
                umin = (std::min)(umin, (std::min)(f.u0, (std::min)(f.u1, f.u2)));
                umax = (std::max)(umax, (std::max)(f.u0, (std::max)(f.u1, f.u2)));
                vmin = (std::min)(vmin, (std::min)(f.v0, (std::min)(f.v1, f.v2)));
                vmax = (std::max)(vmax, (std::max)(f.v0, (std::max)(f.v1, f.v2)));
            }
            const float sc = (std::max)(umax - umin, vmax - vmin);
            if (sc <= 1e-8f) continue;
            for (int fi : groups[axis]) {
                auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                f.u0 = (f.u0 - umin) / sc;
                f.u1 = (f.u1 - umin) / sc;
                f.u2 = (f.u2 - umin) / sc;
                f.v0 = (f.v0 - vmin) / sc;
                f.v1 = (f.v1 - vmin) / sc;
                f.v2 = (f.v2 - vmin) / sc;
            }
        }
        mesh_refresh_preview();
        panel.set_status(omsi::ui::i18n::tr_lit("UV Cube"));
    };
    auto mesh_assign_sel_mat = [&](int mat) {
        mesh_edit.tex_sel = mat;
        if (mesh_edit.sel_faces.empty()) {
            panel.set_status("Tex: seleccioná caras (Faces)");
            return;
        }
        mesh_geo_cap();
        for (int fi : mesh_edit.sel_faces) {
            if (fi >= 0 && static_cast<std::size_t>(fi) < mesh_edit.faces.size())
                mesh_edit.faces[static_cast<std::size_t>(fi)].mat = mat;
        }
        mesh_refresh_preview();
    };

    auto mesh_spline_same_chain = [&](std::int32_t a, std::int32_t b) -> bool {
        if (a == b) return true;
        if (a == 0 || b == 0) return false;
        auto find_spl = [&](std::int32_t id) -> const map::SplineInst* {
            for (const auto& s : edit_splines)
                if (s.id == id) return &s;
            return nullptr;
        };
        const auto* start = find_spl(a);
        if (!start) return false;
        std::unordered_set<std::int32_t> seen;
        auto walk = [&](std::int32_t cur, bool prev) -> bool {
            for (int hops = 0; hops < 256 && cur != 0; ++hops) {
                if (cur == b) return true;
                if (!seen.insert(cur).second) break;
                const auto* p = find_spl(cur);
                if (!p) break;
                cur = prev ? p->prev_id : p->next_id;
            }
            return false;
        };
        seen.insert(a);
        return walk(start->prev_id, true) || walk(start->next_id, false);
    };
    auto mesh_verts_same_surface = [&](int va, int vb) -> bool {
        if (va < 0 || vb < 0) return false;
        if (static_cast<std::size_t>(va) >= mesh_edit.verts.size() ||
            static_cast<std::size_t>(vb) >= mesh_edit.verts.size())
            return false;
        const auto& a = mesh_edit.verts[static_cast<std::size_t>(va)];
        const auto& b = mesh_edit.verts[static_cast<std::size_t>(vb)];
        if (a.src_kind == 0 && b.src_kind == 0) return true;
        if (a.src_kind == b.src_kind && a.src_id == b.src_id) return true;
        if (a.src_kind == 2 && b.src_kind == 2) return mesh_spline_same_chain(a.src_id, b.src_id);
        return false;
    };
    auto mesh_add_weld_edges = [&](std::unordered_map<int, std::vector<int>>& adj) {
        const int n = static_cast<int>(mesh_edit.verts.size());
        const float k = mesh_edit.weld_m * mesh_edit.weld_m;
        const float cell = mesh_edit.weld_m > 1e-6f ? mesh_edit.weld_m : 0.02f;
        auto ckey = [&](int ix, int iy, int iz) -> std::uint64_t {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ix)) << 42) ^
                   (static_cast<std::uint64_t>(static_cast<std::uint32_t>(iy)) << 21) ^
                   static_cast<std::uint64_t>(static_cast<std::uint32_t>(iz));
        };
        std::unordered_map<std::uint64_t, std::vector<int>> grid;
        for (int i = 0; i < n; ++i) {
            const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
            const int ix = static_cast<int>(std::floor(v.x / cell));
            const int iy = static_cast<int>(std::floor(v.y / cell));
            const int iz = static_cast<int>(std::floor(v.z / cell));
            grid[ckey(ix, iy, iz)].push_back(i);
        }
        std::unordered_set<std::uint64_t> seen_p;
        auto pairk = [](int a, int b) -> std::uint64_t {
            const int lo = (std::min)(a, b), hi = (std::max)(a, b);
            return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint32_t>(hi);
        };
        for (int i = 0; i < n; ++i) {
            const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
            const int ix = static_cast<int>(std::floor(v.x / cell));
            const int iy = static_cast<int>(std::floor(v.y / cell));
            const int iz = static_cast<int>(std::floor(v.z / cell));
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz) {
                        auto git = grid.find(ckey(ix + dx, iy + dy, iz + dz));
                        if (git == grid.end()) continue;
                        for (int j : git->second) {
                            if (j <= i) continue;
                            if (!mesh_verts_same_surface(i, j)) continue;
                            const auto& w = mesh_edit.verts[static_cast<std::size_t>(j)];
                            const float ddx = v.x - w.x, ddy = v.y - w.y, ddz = v.z - w.z;
                            if (ddx * ddx + ddy * ddy + ddz * ddz > k) continue;
                            if (!seen_p.insert(pairk(i, j)).second) continue;
                            adj[i].push_back(j);
                            adj[j].push_back(i);
                        }
                    }
        }
    };
    auto mesh_bfs_nodes = [&](const std::unordered_map<int, std::vector<int>>& adj, int start, int goal,
                              std::vector<int>& path) -> bool {
        if (start == goal) {
            path = {start};
            return true;
        }
        std::unordered_map<int, int> parent;
        std::queue<int> q;
        q.push(start);
        parent[start] = -1;
        bool found = false;
        while (!q.empty()) {
            const int cur = q.front();
            q.pop();
            auto it = adj.find(cur);
            if (it == adj.end()) continue;
            for (int nb : it->second) {
                if (parent.count(nb)) continue;
                parent[nb] = cur;
                if (nb == goal) {
                    found = true;
                    break;
                }
                q.push(nb);
            }
            if (found) break;
        }
        if (!found) return false;
        path.clear();
        for (int cur = goal; cur != -1; cur = parent[cur]) path.push_back(cur);
        std::reverse(path.begin(), path.end());
        return true;
    };
    auto mesh_select_path_verts = [&](int start, int goal, bool /*add*/) {
        if (start < 0 || goal < 0) return;
        if (start == goal) {
            mesh_edit.sel_verts.insert(goal);
            mesh_edit.last_vert = goal;
            return;
        }
        std::unordered_map<int, std::vector<int>> adj;
        auto add_v = [&](int a, int b) {
            if (a == b || a < 0 || b < 0) return;
            adj[a].push_back(b);
            adj[b].push_back(a);
        };
        for (const auto& e : mesh_edit.edges) add_v(e.a, e.b);
        for (const auto& f : mesh_edit.faces) {
            add_v(f.i0, f.i1);
            add_v(f.i1, f.i2);
            add_v(f.i2, f.i0);
        }
        mesh_add_weld_edges(adj);
        std::vector<int> path;
        if (!mesh_bfs_nodes(adj, start, goal, path)) {
            mesh_edit.sel_verts.insert(goal);
            mesh_edit.last_vert = goal;
            panel.set_status("Ctrl+click: sin camino");
            return;
        }
        for (int i : path) mesh_edit.sel_verts.insert(i);
        mesh_edit.last_vert = goal;
    };
    auto mesh_select_path_edges = [&](int start, int goal, bool /*add*/) {
        if (start < 0 || goal < 0) return;
        const int n = static_cast<int>(mesh_edit.edges.size());
        if (start >= n || goal >= n) return;
        auto ev = [&](int ei) {
            return std::pair<int, int>{mesh_edit.edges[static_cast<std::size_t>(ei)].a,
                                       mesh_edit.edges[static_cast<std::size_t>(ei)].b};
        };
        auto close_v = [&](int a, int b) -> bool {
            if (a < 0 || b < 0) return false;
            if (static_cast<std::size_t>(a) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(b) >= mesh_edit.verts.size())
                return false;
            const auto& va = mesh_edit.verts[static_cast<std::size_t>(a)];
            const auto& vb = mesh_edit.verts[static_cast<std::size_t>(b)];
            const float dx = va.x - vb.x, dy = va.y - vb.y, dz = va.z - vb.z;
            return dx * dx + dy * dy + dz * dz <= mesh_edit.weld_m * mesh_edit.weld_m;
        };
        std::unordered_map<int, std::vector<int>> adj;
        for (int i = 0; i < n; ++i) {
            const auto [a, b] = ev(i);
            for (int j = i + 1; j < n; ++j) {
                const auto [c, d] = ev(j);
                const bool share = a == c || a == d || b == c || b == d;
                const bool coin = (close_v(a, c) && close_v(b, d) && mesh_verts_same_surface(a, c) &&
                                   mesh_verts_same_surface(b, d)) ||
                                  (close_v(a, d) && close_v(b, c) && mesh_verts_same_surface(a, d) &&
                                   mesh_verts_same_surface(b, c));
                if (!share && !coin) continue;
                adj[i].push_back(j);
                adj[j].push_back(i);
            }
        }
        std::vector<int> path;
        if (!mesh_bfs_nodes(adj, start, goal, path)) {
            mesh_edit.sel_edges.insert(goal);
            mesh_edit.last_edge = goal;
            panel.set_status("Ctrl+click: sin camino");
            return;
        }
        for (int i : path) mesh_edit.sel_edges.insert(i);
        mesh_edit.last_edge = goal;
    };
    auto mesh_select_path_faces = [&](int goal, bool /*subtract*/) {
        if (mesh_edit.last_face < 0 || goal < 0) return;
        const int n = static_cast<int>(mesh_edit.faces.size());
        if (mesh_edit.last_face >= n || goal >= n) return;
        auto ekey = [](int a, int b) -> std::uint64_t {
            const int lo = (std::min)(a, b), hi = (std::max)(a, b);
            return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint32_t>(hi);
        };
        std::unordered_map<std::uint64_t, std::vector<int>> ef;
        for (int i = 0; i < n; ++i) {
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
            ef[ekey(f.i0, f.i1)].push_back(i);
            ef[ekey(f.i1, f.i2)].push_back(i);
            ef[ekey(f.i2, f.i0)].push_back(i);
        }
        std::unordered_map<int, std::vector<int>> adj;
        for (auto& [k, fl] : ef) {
            for (std::size_t i = 0; i < fl.size(); ++i)
                for (std::size_t j = i + 1; j < fl.size(); ++j) {
                    adj[fl[i]].push_back(fl[j]);
                    adj[fl[j]].push_back(fl[i]);
                }
        }
        auto close_v = [&](int a, int b) -> bool {
            if (a < 0 || b < 0) return false;
            if (static_cast<std::size_t>(a) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(b) >= mesh_edit.verts.size())
                return false;
            const auto& va = mesh_edit.verts[static_cast<std::size_t>(a)];
            const auto& vb = mesh_edit.verts[static_cast<std::size_t>(b)];
            const float dx = va.x - vb.x, dy = va.y - vb.y, dz = va.z - vb.z;
            return dx * dx + dy * dy + dz * dz <= mesh_edit.weld_m * mesh_edit.weld_m &&
                   mesh_verts_same_surface(a, b);
        };
        for (int i = 0; i < n; ++i) {
            const auto& fa = mesh_edit.faces[static_cast<std::size_t>(i)];
            const int ia[3] = {fa.i0, fa.i1, fa.i2};
            for (int j = i + 1; j < n; ++j) {
                const auto& fb = mesh_edit.faces[static_cast<std::size_t>(j)];
                const int ib[3] = {fb.i0, fb.i1, fb.i2};
                int hits = 0;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        if (close_v(ia[a], ib[b])) ++hits;
                if (hits < 2) continue;
                adj[i].push_back(j);
                adj[j].push_back(i);
            }
        }
        std::vector<int> path;
        if (!mesh_bfs_nodes(adj, mesh_edit.last_face, goal, path)) {
            mesh_edit.sel_faces.insert(goal);
            mesh_edit.last_face = goal;
            panel.set_status("Ctrl+click: sin camino");
            return;
        }
        for (int i : path) mesh_edit.sel_faces.insert(i);
        mesh_edit.last_face = goal;
    };

        auto mesh_bake_face = [&]() {
        std::unordered_map<int, std::vector<int>> adj;
        auto add_adj = [&](int a, int b) {
            if (a == b || a < 0 || b < 0) return;
            if (static_cast<std::size_t>(a) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(b) >= mesh_edit.verts.size())
                return;
            auto& na = adj[a];
            if (std::find(na.begin(), na.end(), b) == na.end()) na.push_back(b);
            auto& nb = adj[b];
            if (std::find(nb.begin(), nb.end(), a) == nb.end()) nb.push_back(a);
        };
        int nedges_ok = 0;
        for (int ei : mesh_edit.sel_edges) {
            if (ei < 0 || static_cast<std::size_t>(ei) >= mesh_edit.edges.size()) continue;
            const auto& e = mesh_edit.edges[static_cast<std::size_t>(ei)];
            add_adj(e.a, e.b);
            ++nedges_ok;
        }
        const int nverts_g = static_cast<int>(adj.size());
        auto bake_log = [&](int chains, int created, const char* reason) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "Bake: edges=%d verts=%d chains=%d created=%d reason=%s",
                          nedges_ok, nedges_ok > 0 ? nverts_g : static_cast<int>(mesh_edit.sel_verts.size()),
                          chains, created, reason);
            panel.set_status(buf);
            log::info(buf);
        };
        auto same_verts = [](const MeshFace& f, int a, int b, int c) {
            int n = 0;
            if (f.i0 == a || f.i1 == a || f.i2 == a) ++n;
            if (f.i0 == b || f.i1 == b || f.i2 == b) ++n;
            if (f.i0 == c || f.i1 == c || f.i2 == c) ++n;
            return n == 3 && a != b && b != c && a != c;
        };
        auto face_exists = [&](int a, int b, int c) {
            for (const auto& f : mesh_edit.faces)
                if (same_verts(f, a, b, c)) return true;
            return false;
        };
        auto tri_cs = [&](int ia, int ib, int ic) {
            const auto& a = mesh_edit.verts[static_cast<std::size_t>(ia)];
            const auto& b = mesh_edit.verts[static_cast<std::size_t>(ib)];
            const auto& c = mesh_edit.verts[static_cast<std::size_t>(ic)];
            return mesh_cross_sq(a, b, c);
        };
        auto commit_tris = [&](std::vector<std::array<int, 3>>& cand, int chains, const char* ok_reason) {
            std::vector<std::array<int, 3>> keep;
            bool any_valid = false;
            for (const auto& t : cand) {
                if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
                if (static_cast<std::size_t>(t[0]) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(t[1]) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(t[2]) >= mesh_edit.verts.size())
                    continue;
                if (tri_cs(t[0], t[1], t[2]) < 1e-12f) continue;
                any_valid = true;
                if (face_exists(t[0], t[1], t[2])) continue;
                keep.push_back(t);
            }
            if (!any_valid) {
                bake_log(chains, 0, "area0");
                return;
            }
            if (keep.empty()) {
                bake_log(chains, 0, "dup");
                return;
            }
            mesh_geo_cap();
            const int face0 = static_cast<int>(mesh_edit.faces.size());
            for (const auto& t : keep) {
                mesh_edit.faces.push_back(mesh_new_face(t[0], t[1], t[2], mesh_edit.tex_sel));
                mesh_cube_uv_face(mesh_edit.faces.back());
                mesh_ensure_edge(t[0], t[1]);
                mesh_ensure_edge(t[1], t[2]);
                mesh_ensure_edge(t[2], t[0]);
            }
            mesh_edit.sel_faces.clear();
            mesh_edit.sel_verts.clear();
            mesh_edit.sel_edges.clear();
            for (int i = face0; i < static_cast<int>(mesh_edit.faces.size()); ++i)
                mesh_edit.sel_faces.insert(i);
            mesh_refresh_preview();
            bake_log(chains, static_cast<int>(keep.size()), ok_reason);
        };
        auto walk_cycle = [&]() -> std::vector<int> {
            if (adj.empty()) return {};
            for (const auto& kv : adj) {
                if (kv.second.size() != 2) return {};
            }
            const int start = adj.begin()->first;
            int prev = -1, cur = start;
            std::vector<int> loop;
            do {
                loop.push_back(cur);
                const auto& ns = adj[cur];
                if (ns.size() != 2) return {};
                const int nxt = (ns[0] == prev) ? ns[1] : ns[0];
                prev = cur;
                cur = nxt;
                if (loop.size() > adj.size() + 2) return {};
            } while (cur != start);
            if (loop.size() != adj.size()) return {};
            return loop;
        };

        for (const auto& kv : adj) {
            if (kv.second.size() > 2) {
                bake_log(0, 0, "deg3");
                return;
            }
        }

        bool all_deg2 = !adj.empty();
        for (const auto& kv : adj) {
            if (kv.second.size() != 2) {
                all_deg2 = false;
                break;
            }
        }
        if (all_deg2 && nedges_ok == nverts_g) {
            auto loop = walk_cycle();
            if (loop.size() >= 3) {
                std::vector<std::array<int, 3>> tris;
                for (std::size_t i = 1; i + 1 < loop.size(); ++i)
                    tris.push_back({loop[0], loop[i], loop[i + 1]});
                commit_tris(tris, 0, "cycle");
                return;
            }
            bake_log(0, 0, "multi_cycle");
            return;
        }

        if (nedges_ok == 0) {
            if (mesh_edit.sel_verts.size() == 3) {
                std::vector<int> vs(mesh_edit.sel_verts.begin(), mesh_edit.sel_verts.end());
                std::vector<std::array<int, 3>> tris;
                tris.push_back({vs[0], vs[1], vs[2]});
                commit_tris(tris, 0, "verts3");
                return;
            }
            bake_log(0, 0, mesh_edit.sel_verts.size() >= 4 ? "verts_ge4" : "no_edges");
            return;
        }

        std::unordered_set<int> used;
        auto next_deg1 = [&]() -> int {
            for (const auto& kv : adj) {
                if (!used.count(kv.first) && kv.second.size() == 1) return kv.first;
            }
            return -1;
        };
        std::vector<std::vector<int>> chains;
        while (true) {
            const int start = next_deg1();
            if (start < 0) break;
            std::vector<int> ch;
            int prev = -1, cur = start;
            while (cur >= 0 && !used.count(cur)) {
                ch.push_back(cur);
                used.insert(cur);
                const auto& ns = adj[cur];
                int nxt = -1;
                for (int n : ns) {
                    if (n != prev) {
                        nxt = n;
                        break;
                    }
                }
                prev = cur;
                cur = nxt;
            }
            if (!ch.empty()) chains.push_back(std::move(ch));
        }
        int leftover = 0;
        for (const auto& kv : adj)
            if (!used.count(kv.first)) ++leftover;
        const int nchains = static_cast<int>(chains.size()) + (leftover > 0 ? 1 : 0);
        if (nchains >= 3 || leftover > 0) {
            bake_log(nchains, 0, "chains3");
            return;
        }
        if (chains.size() == 1 && chains[0].size() == 2) {
            bake_log(1, 0, "open1");
            return;
        }
        if (chains.size() == 1 && chains[0].size() >= 3) {
            std::vector<std::array<int, 3>> tris;
            const auto& loop = chains[0];
            for (std::size_t i = 1; i + 1 < loop.size(); ++i)
                tris.push_back({loop[0], loop[i], loop[i + 1]});
            commit_tris(tris, 1, "chain1");
            return;
        }
        if (chains.size() == 2) {
            int v1 = chains[0].front(), v2 = chains[0].back();
            int v3 = chains[1].front(), v4 = chains[1].back();
            auto tri_n = [&](int ia, int ib, int ic, float n[3]) {
                const auto& a = mesh_edit.verts[static_cast<std::size_t>(ia)];
                const auto& b = mesh_edit.verts[static_cast<std::size_t>(ib)];
                const auto& c = mesh_edit.verts[static_cast<std::size_t>(ic)];
                n[0] = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
                n[1] = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
                n[2] = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len < 1e-12f) {
                    n[0] = n[1] = n[2] = 0.f;
                    return;
                }
                n[0] /= len;
                n[1] /= len;
                n[2] /= len;
            };
            float na[3], nb[3];
            tri_n(v1, v2, v4, na);
            tri_n(v1, v4, v3, nb);
            const float dot_24 = na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2];
            tri_n(v1, v2, v3, na);
            tri_n(v1, v3, v4, nb);
            const float dot_13 = na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2];
            if (dot_24 < dot_13) std::swap(v3, v4);
            add_adj(v1, v3);
            add_adj(v2, v4);
            auto loop = walk_cycle();
            if (loop.size() < 3) {
                bake_log(2, 0, "pair_walk");
                return;
            }
            std::vector<std::array<int, 3>> tris;
            for (std::size_t i = 1; i + 1 < loop.size(); ++i)
                tris.push_back({loop[0], loop[i], loop[i + 1]});
            commit_tris(tris, 2, "pair");
            return;
        }
        bake_log(nchains, 0, "no_fill");
    };

    auto mesh_undo_face = [&]() {
        if (mesh_edit.last_event_sel && !mesh_edit.sel_undo.empty()) {
            mesh_edit.sel_redo.push_back(mesh_capture_snap());
            mesh_apply_snap(mesh_edit.sel_undo.back());
            mesh_edit.sel_undo.pop_back();
            mesh_edit.last_event_sel = !mesh_edit.sel_undo.empty();
            panel.set_status("Undo sel");
            return;
        }
        if (mesh_edit.geo_undo.empty()) return;
        mesh_edit.geo_redo.push_back(mesh_capture_snap());
        mesh_apply_snap(mesh_edit.geo_undo.back());
        mesh_edit.geo_undo.pop_back();
        mesh_edit.last_event_sel = false;
        mesh_edit.dirty = mesh_edit.geo_undo.empty() ? false : true;
        mesh_refresh_preview();
        panel.set_status("Undo mesh");
    };

    auto mesh_redo_face = [&]() {
        if (!mesh_edit.sel_redo.empty() && mesh_edit.last_event_sel) {
            mesh_edit.sel_undo.push_back(mesh_capture_snap());
            mesh_apply_snap(mesh_edit.sel_redo.back());
            mesh_edit.sel_redo.pop_back();
            panel.set_status("Redo sel");
            return;
        }
        if (mesh_edit.geo_redo.empty()) return;
        mesh_edit.geo_undo.push_back(mesh_capture_snap());
        mesh_apply_snap(mesh_edit.geo_redo.back());
        mesh_edit.geo_redo.pop_back();
        mesh_edit.dirty = true;
        mesh_refresh_preview();
        panel.set_status("Redo mesh");
    };

    auto mesh_bake_sco = [&]() {
        if (mesh_edit.faces.empty() || view_only) return;
        std::string stem = mesh_edit.bake_name;
        while (!stem.empty() && (stem.back() == ' ' || stem.back() == '\t')) stem.pop_back();
        if (stem.empty()) stem = "f4_mesh";
        for (char& c : stem) {
            if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                c == '|')
                c = '_';
        }
        std::error_code ec;
        auto try_dir = [&](const std::filesystem::path& parent) -> std::filesystem::path {
            if (parent.empty()) return {};
            std::filesystem::create_directories(parent, ec);
            if (ec) return {};
            auto probe = parent / "_wtest";
            std::filesystem::create_directories(probe, ec);
            if (ec) return {};
            std::filesystem::remove(probe, ec);
            if (!mesh_edit.session_bake_dir.empty()) return mesh_edit.session_bake_dir;
            return editor::unique_sli_bake_dir(parent, stem);
        };
        std::filesystem::path dir;
        if (!mesh_edit.session_bake_dir.empty())
            dir = mesh_edit.session_bake_dir;
        else
            dir = try_dir(cfg.omsi_root / "Sceneryobjects");
        if (dir.empty() && cfg.workspace && !cfg.workspace->actual_dir.empty())
            dir = try_dir(cfg.workspace->actual_dir / "Sceneryobjects");
        if (dir.empty()) {
            const auto tmp = std::filesystem::temp_directory_path() / "omsi_f4_mesh_bake";
            dir = try_dir(tmp);
        }
        if (dir.empty()) {
            panel.set_status("Bake .sco: sin permiso de escritura");
            return;
        }
        mesh_edit.session_bake_dir = dir;
        std::filesystem::create_directories(dir / "model", ec);
        std::filesystem::create_directories(dir / "texture", ec);
        const auto o3d_path = dir / "model" / (stem + ".o3d");
        const auto sco_path = dir / (stem + ".sco");
        HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
        if (std::filesystem::exists(o3d_path, ec) || std::filesystem::exists(sco_path, ec)) {
            const int ans = MessageBoxW(owner, L"El .sco/.o3d ya existe. ¿Sobrescribir?", L"Bake .sco",
                                        MB_YESNO | MB_ICONWARNING);
            if (ans != IDYES) return;
        }
        o3d::WritableMesh mesh;
        const int nmat = (std::max)(1, static_cast<int>(mesh_edit.tex_list.size()));
        for (int mi = 0; mi < nmat; ++mi) {
            o3d::Material mat{};
            mat.fixed.diffuse[0] = mat.fixed.diffuse[1] = mat.fixed.diffuse[2] = mat.fixed.diffuse[3] = 1.f;
            mat.fixed.power = 10.f;
            const auto tex = mesh_tex_path_at(mi);
            std::string tex_ref = "white.bmp";
            if (!tex.empty()) {
                tex_ref = tex.filename().string();
                std::filesystem::copy_file(tex, dir / "texture" / tex.filename(),
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
            mat.texture = tex_ref;
            mesh.materials.push_back(mat);
        }
        float cx = 0.f, cy = 0.f, cz = 0.f;
        int nv = 0;
        for (const auto& v : mesh_edit.verts) {
            cx += v.x;
            cy += v.y;
            cz += v.z;
            ++nv;
        }
        if (nv > 0) {
            cx /= static_cast<float>(nv);
            cy /= static_cast<float>(nv);
            cz /= static_cast<float>(nv);
        }
        for (const auto& f : mesh_edit.faces) {
            if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
            if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                continue;
            const auto& a = mesh_edit.verts[static_cast<std::size_t>(f.i0)];
            const auto& b = mesh_edit.verts[static_cast<std::size_t>(f.i1)];
            const auto& c = mesh_edit.verts[static_cast<std::size_t>(f.i2)];
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            o3d::Vertex va{}, vb{}, vc{};
            va.position[0] = a.x - cx;
            va.position[1] = a.y - cy;
            va.position[2] = a.z - cz;
            vb.position[0] = b.x - cx;
            vb.position[1] = b.y - cy;
            vb.position[2] = b.z - cz;
            vc.position[0] = c.x - cx;
            vc.position[1] = c.y - cy;
            vc.position[2] = c.z - cz;
            va.uv[0] = f.u0;
            va.uv[1] = f.v0;
            vb.uv[0] = f.u1;
            vb.uv[1] = f.v1;
            vc.uv[0] = f.u2;
            vc.uv[1] = f.v2;
            float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
            float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
            float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nl > 1e-12f) {
                nx /= nl;
                ny /= nl;
                nz /= nl;
            } else {
                nx = 0.f;
                ny = 0.f;
                nz = 1.f;
            }
            va.normal[0] = vb.normal[0] = vc.normal[0] = nx;
            va.normal[1] = vb.normal[1] = vc.normal[1] = ny;
            va.normal[2] = vb.normal[2] = vc.normal[2] = nz;
            mesh.vertices.push_back(va);
            mesh.vertices.push_back(vb);
            mesh.vertices.push_back(vc);
            o3d::TriangleU32 t{};
            t.i0 = base;
            t.i1 = base + 1;
            t.i2 = base + 2;
            t.material = static_cast<std::uint16_t>((f.mat >= 0 && f.mat < nmat) ? f.mat : 0);
            mesh.triangles.push_back(t);
        }
        if (!o3d::write_o3d_file(o3d_path, mesh)) {
            panel.set_status("Bake .sco: no se pudo escribir o3d");
            return;
        }
        std::ofstream sco(sco_path);
        sco << "[friendlyname]\n" << stem << "\n\n[mesh]\nmodel\\" << stem << ".o3d\n\n[absheight]\n";
        sco.close();
        push_undo("mesh_bake_sco");
        map::MapObjectInst o{};
        o.id = next_object_id();
        o.sco_path = sco_path;
        o.position.x = cx;
        o.position.y = cy;
        o.position.z = cz;
        edit_objects.push_back(o);
        transient_objects.push_back(o);
        selected_object = static_cast<int>(edit_objects.size() - 1);
        spawn_editor_object_gpu(edit_objects.back());
        persist_editor_xy(cx, cy);
        refresh_editor_lists();
        mesh_edit.verts.clear();
        mesh_edit.edges.clear();
        mesh_edit.faces.clear();
        mesh_edit.sel_verts.clear();
        mesh_edit.sel_edges.clear();
        mesh_edit.sel_faces.clear();
        mesh_edit.geo_undo.clear();
        mesh_edit.geo_redo.clear();
        mesh_edit.sel_undo.clear();
        mesh_edit.sel_redo.clear();
        mesh_edit.dirty = false;
        mesh_edit.session_bake_dir.clear();
        auto hide_c = [&](std::size_t c) {
            if (c != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(c, false);
        };
        hide_c(mesh_edit.preview_chunk);
        for (auto c : mesh_edit.preview_chunks) hide_c(c);
        hide_c(mesh_edit.sel_overlay_chunk);
        hide_c(mesh_edit.edge_overlay_chunk);
        mesh_edit.env_verts.clear();
        mesh_edit.env_edges.clear();
        mesh_edit.env_tris.clear();
        mesh_edit.env_obj.clear();
        mesh_edit.env_spl.clear();
        panel.set_status("Bake .sco " + sco_path.filename().string());
    };

    // Edit mesh .o3d: load into the bake editor at the camera target, weld by position (1 mm grid),
    // keep per-corner UV and the original materials.
    auto mesh_load_o3d = [&](const std::filesystem::path& path, const XMMATRIX* obj_base = nullptr) -> bool {
        auto mapped = o3d::load_mapped(path);
        if (!mapped || !mapped->valid()) {
            panel.set_status("Edit .o3d: no se pudo leer " + path.filename().string());
            return false;
        }
        const auto& mesh = mapped->mesh();
        std::vector<o3d::Vertex> gv;
        o3d::prepare_o3d_mesh_vertices_for_gpu(mesh, gv);
        if (gv.empty()) {
            panel.set_status("Edit .o3d: sin vertices");
            return false;
        }
        XMMATRIX wm = ma4_to_xm(o3d::o3d_unity_root_transform(mesh.object_rows()));
        mesh_edit.verts.clear();
        mesh_edit.edges.clear();
        mesh_edit.faces.clear();
        mesh_edit.sel_verts.clear();
        mesh_edit.sel_edges.clear();
        mesh_edit.sel_faces.clear();
        mesh_edit.sel_uv.clear();
        mesh_edit.geo_undo.clear();
        mesh_edit.geo_redo.clear();
        mesh_edit.sel_undo.clear();
        mesh_edit.sel_redo.clear();
        mesh_edit.o3d_edit_path = path;
        mesh_edit.o3d_ox = camera.target_x;
        mesh_edit.o3d_oy = camera.target_y;
        mesh_edit.o3d_oz = camera.target_z;
        {
            const float id9[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
            std::memcpy(mesh_edit.o3d_rot, id9, sizeof(id9));
        }
        if (obj_base) {
            // picked from the map: the mesh sits exactly where the object draws it
            DirectX::XMFLOAT4X4 b{};
            DirectX::XMStoreFloat4x4(&b, *obj_base);
            mesh_edit.o3d_ox = b._41;
            mesh_edit.o3d_oy = b._42;
            mesh_edit.o3d_oz = b._43;
            const float r9[9] = {b._11, b._12, b._13, b._21, b._22, b._23, b._31, b._32, b._33};
            std::memcpy(mesh_edit.o3d_rot, r9, sizeof(r9));
            XMMATRIX rot = *obj_base;
            rot.r[3] = DirectX::XMVectorSet(0.f, 0.f, 0.f, 1.f);
            wm = wm * rot;
        }
        mesh_edit.o3d_mats.clear();
        const auto mats = mesh.parse_materials();
        std::vector<int> mat_to_tex(mats.size(), -1);
        for (std::size_t mi = 0; mi < mats.size(); ++mi) {
            std::filesystem::path tex;
            if (!mats[mi].texture.empty()) {
                if (const auto r = gfx::dx12::resolve_texture_path(path, mats[mi].texture, cfg.omsi_root)) tex = *r;
            }
            int ti = -1;
            for (int k = 0; k < static_cast<int>(mesh_edit.tex_list.size()) && !tex.empty(); ++k)
                if (mesh_edit.tex_list[static_cast<std::size_t>(k)].lexically_normal() == tex.lexically_normal())
                    ti = k;
            if (ti < 0) {
                mesh_edit.tex_list.push_back(tex);
                ti = static_cast<int>(mesh_edit.tex_list.size()) - 1;
            }
            mat_to_tex[mi] = ti;
            mesh_edit.o3d_mats[ti] = mats[mi];
        }
        // one slot per material of the file
        mesh_edit.slots.clear();
        mesh_edit.active_slot = -1;
        for (std::size_t mi = 0; mi < mats.size(); ++mi) {
            MeshEdit::Slot sl{};
            sl.name = mats[mi].texture.empty() ? ("Material " + std::to_string(mi)) : std::string(mats[mi].texture);
            sl.tex = mat_to_tex[mi];
            mesh_edit.slots.push_back(sl);
        }
        std::unordered_map<std::uint64_t, int> grid;
        grid.reserve(gv.size() * 2);
        std::vector<int> remap(gv.size(), -1);
        for (std::size_t i = 0; i < gv.size(); ++i) {
            DirectX::XMFLOAT3 w{};
            DirectX::XMStoreFloat3(&w, xform_point(wm, gv[i].position[0], gv[i].position[1], gv[i].position[2]));
            const auto qx = static_cast<std::int64_t>(std::llround(w.x * 1000.0));
            const auto qy = static_cast<std::int64_t>(std::llround(w.y * 1000.0));
            const auto qz = static_cast<std::int64_t>(std::llround(w.z * 1000.0));
            const std::uint64_t key = (static_cast<std::uint64_t>(qx) * 73856093ull) ^
                                      (static_cast<std::uint64_t>(qy) * 19349663ull) ^
                                      (static_cast<std::uint64_t>(qz) * 83492791ull);
            auto it = grid.find(key);
            if (it != grid.end()) {
                const auto& e = mesh_edit.verts[static_cast<std::size_t>(it->second)];
                if (std::fabs(e.x - (w.x + mesh_edit.o3d_ox)) < 1e-3f &&
                    std::fabs(e.y - (w.y + mesh_edit.o3d_oy)) < 1e-3f &&
                    std::fabs(e.z - (w.z + mesh_edit.o3d_oz)) < 1e-3f) {
                    remap[i] = it->second;
                    continue;
                }
            }
            MeshVert v{};
            v.x = w.x + mesh_edit.o3d_ox;
            v.y = w.y + mesh_edit.o3d_oy;
            v.z = w.z + mesh_edit.o3d_oz;
            remap[i] = static_cast<int>(mesh_edit.verts.size());
            grid[key] = remap[i];
            mesh_edit.verts.push_back(v);
        }
        std::unordered_set<std::uint64_t> seen_e;
        auto add_face = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint16_t m) {
            if (a >= gv.size() || b >= gv.size() || c >= gv.size()) return;
            MeshFace f{};
            f.i0 = remap[a];
            f.i1 = remap[b];
            f.i2 = remap[c];
            if (f.i0 == f.i1 || f.i1 == f.i2 || f.i0 == f.i2) return;
            f.mat = m < mat_to_tex.size() ? mat_to_tex[m] : -1;
            f.slot = m < mat_to_tex.size() ? static_cast<int>(m) : -1;
            f.u0 = gv[a].uv[0];
            f.v0 = gv[a].uv[1];
            f.u1 = gv[b].uv[0];
            f.v1 = gv[b].uv[1];
            f.u2 = gv[c].uv[0];
            f.v2 = gv[c].uv[1];
            mesh_edit.faces.push_back(f);
            const int es[3][2] = {{f.i0, f.i1}, {f.i1, f.i2}, {f.i2, f.i0}};
            for (const auto& ab : es)
                if (seen_e.insert(mesh_edge_key(ab[0], ab[1])).second) mesh_edit.edges.push_back(MeshEdge{ab[0], ab[1]});
        };
        if (mesh.long_triangle_indices) {
            for (const auto& t : mesh.triangle_u32_span()) add_face(t.i0, t.i1, t.i2, t.material);
        } else {
            for (const auto& t : mesh.triangle_u16_span()) add_face(t.i0, t.i1, t.i2, t.material);
        }
        std::snprintf(mesh_edit.bake_name, sizeof(mesh_edit.bake_name), "%s", path.stem().string().c_str());
        mesh_edit.dirty = false;
        mesh_edit.force_tex = true;
        mesh_refresh_preview();
        panel.set_status("Edit .o3d " + path.filename().string() + "  V:" + std::to_string(mesh_edit.verts.size()) +
                         " F:" + std::to_string(mesh_edit.faces.size()));
        return true;
    };

    // Save back to the same .o3d: backup <name>.o3d.<YYYYMMDD_HHMM>.bak first, smooth normals with a
    // 45° crease, original material params kept. The file may be memory-mapped by the scene
    // (FILE_SHARE_READ): then it is written as <name>.o3d.new and the user is told.
    auto mesh_save_o3d = [&]() {
        if (mesh_edit.o3d_edit_path.empty() || mesh_edit.faces.empty() || view_only) return;
        const auto path = mesh_edit.o3d_edit_path;
        o3d::WritableMesh out;
        std::unordered_map<int, std::uint16_t> out_mat;
        auto mat_for = [&](int ti) -> std::uint16_t {
            if (const auto it = out_mat.find(ti); it != out_mat.end()) return it->second;
            o3d::Material m{};
            if (const auto it = mesh_edit.o3d_mats.find(ti); it != mesh_edit.o3d_mats.end()) {
                m = it->second;
            } else {
                m.fixed.diffuse[0] = m.fixed.diffuse[1] = m.fixed.diffuse[2] = m.fixed.diffuse[3] = 1.f;
                m.fixed.power = 10.f;
            }
            const auto tex = mesh_tex_path_at(ti);
            if (!tex.empty()) m.texture = tex.filename().string();
            const auto idx = static_cast<std::uint16_t>(out.materials.size());
            out.materials.push_back(m);
            out_mat[ti] = idx;
            return idx;
        };
        const std::size_t nf = mesh_edit.faces.size();
        std::vector<DirectX::XMFLOAT3> fnrm(nf);
        std::vector<std::vector<int>> vfaces(mesh_edit.verts.size());
        for (std::size_t fi = 0; fi < nf; ++fi) {
            const auto& f = mesh_edit.faces[fi];
            if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0 || static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                continue;
            const auto &a = mesh_edit.verts[static_cast<std::size_t>(f.i0)],
                       &b = mesh_edit.verts[static_cast<std::size_t>(f.i1)],
                       &c = mesh_edit.verts[static_cast<std::size_t>(f.i2)];
            float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
            float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
            float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (l > 1e-12f) {
                nx /= l;
                ny /= l;
                nz /= l;
            } else {
                nz = 1.f;
            }
            fnrm[fi] = {nx, ny, nz};
            for (const int vi : {f.i0, f.i1, f.i2}) vfaces[static_cast<std::size_t>(vi)].push_back(static_cast<int>(fi));
        }
        constexpr float kCrease = 0.7071f;
        std::unordered_map<std::string, std::uint32_t> vdedup;
        vdedup.reserve(nf * 2);
        for (std::size_t fi = 0; fi < nf; ++fi) {
            const auto& f = mesh_edit.faces[fi];
            if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0 || static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                continue;
            std::uint32_t corner[3]{};
            const int ids[3] = {f.i0, f.i1, f.i2};
            for (int c = 0; c < 3; ++c) {
                const auto& p = mesh_edit.verts[static_cast<std::size_t>(ids[c])];
                o3d::Vertex v{};
                const float* R = mesh_edit.o3d_rot; // local = (world - origin) * R^T
                {
                    const float d0 = p.x - mesh_edit.o3d_ox, d1 = p.y - mesh_edit.o3d_oy, d2 = p.z - mesh_edit.o3d_oz;
                    for (int j = 0; j < 3; ++j) v.position[j] = d0 * R[j * 3 + 0] + d1 * R[j * 3 + 1] + d2 * R[j * 3 + 2];
                }
                float sx = 0.f, sy = 0.f, sz = 0.f;
                for (const int g : vfaces[static_cast<std::size_t>(ids[c])]) {
                    const auto& n = fnrm[static_cast<std::size_t>(g)];
                    if (n.x * fnrm[fi].x + n.y * fnrm[fi].y + n.z * fnrm[fi].z < kCrease) continue;
                    sx += n.x;
                    sy += n.y;
                    sz += n.z;
                }
                const float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
                v.normal[0] = sl > 1e-12f ? sx / sl : fnrm[fi].x;
                v.normal[1] = sl > 1e-12f ? sy / sl : fnrm[fi].y;
                v.normal[2] = sl > 1e-12f ? sz / sl : fnrm[fi].z;
                {
                    const float n0 = v.normal[0], n1 = v.normal[1], n2 = v.normal[2];
                    for (int j = 0; j < 3; ++j) v.normal[j] = n0 * R[j * 3 + 0] + n1 * R[j * 3 + 1] + n2 * R[j * 3 + 2];
                }
                v.uv[0] = c == 0 ? f.u0 : (c == 1 ? f.u1 : f.u2);
                v.uv[1] = c == 0 ? f.v0 : (c == 1 ? f.v1 : f.v2);
                const std::string vk(reinterpret_cast<const char*>(&v), sizeof(v));
                if (const auto it = vdedup.find(vk); it != vdedup.end()) {
                    corner[c] = it->second;
                } else {
                    corner[c] = static_cast<std::uint32_t>(out.vertices.size());
                    vdedup.emplace(vk, corner[c]);
                    out.vertices.push_back(v);
                }
            }
            o3d::TriangleU32 t{};
            t.i0 = corner[0];
            t.i1 = corner[1];
            t.i2 = corner[2];
            t.material = mat_for(f.mat);
            out.triangles.push_back(t);
        }
        std::error_code ec;
        auto tmp = path;
        tmp += ".new";
        if (!o3d::write_o3d_file(tmp, out)) {
            panel.set_status("Guardar .o3d: no se pudo escribir");
            return;
        }
        char stamp[32]{};
        {
            const std::time_t now = std::time(nullptr);
            std::tm lt{};
            localtime_s(&lt, &now);
            std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M", &lt);
        }
        auto bak = path;
        bak += std::string(".") + stamp + ".bak";
        if (std::filesystem::exists(path, ec))
            std::filesystem::copy_file(path, bak, std::filesystem::copy_options::overwrite_existing, ec);
        o3d::invalidate_mapped_cache(path);
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            mesh_edit.dirty = false;
            panel.set_status("Guardado " + path.filename().string() + "  (backup " + bak.filename().string() + ")");
        } else {
            panel.set_status("Guardar .o3d: archivo en uso por la escena — quedo como " + tmp.filename().string() +
                             " (backup " + bak.filename().string() + ")");
        }
    };

    [[maybe_unused]] auto mesh_point_in_tri2 = [](float px, float py, float ax, float ay, float bx, float by, float cx, float cy) {
        const float d1 = (px - ax) * (by - ay) - (py - ay) * (bx - ax);
        const float d2 = (px - bx) * (cy - by) - (py - by) * (cx - bx);
        const float d3 = (px - cx) * (ay - cy) - (py - cy) * (ax - cx);
        const bool neg = (d1 < 0.f) || (d2 < 0.f) || (d3 < 0.f);
        const bool pos = (d1 > 0.f) || (d2 > 0.f) || (d3 > 0.f);
        return !(neg && pos);
    };
    [[maybe_unused]] auto mesh_tri_area2 = [](float ax, float ay, float bx, float by, float cx, float cy) {
        return std::fabs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
    };

    auto mesh_pick_at = [&](float mx, float my, float fw, float fh, const float* wvp, int& best_v, int& best_e,
                            int& best_f, int& best_src_f, float& best_vd, float& best_ed, float& best_fd) {
        best_v = best_e = best_f = best_src_f = -1;
        best_vd = 28.f * 28.f;
        best_ed = 22.f * 22.f;
        best_fd = 1.e30f;
        // Occlusion: a vertex / edge hidden behind a face (of the mesh or of the traced source) is not
        // pickable, except in wireframe shading where everything shows. The nearest candidate on screen
        // used to win even when it lay behind the face under the cursor ("clicks the one below").
        const auto near_cam_occ = mesh_cam_filter();
        auto point_visible = [&](float px, float py, float pz, float sx, float sy, int skip_a, int skip_b) -> bool {
            if (mesh_edit.shading == 1) return true;
            const auto r = camera.make_pick_ray(sx, sy, fw, fh);
            if (!r) return true;
            const float tv = (px - r->origin.x) * r->dir.x + (py - r->origin.y) * r->dir.y + (pz - r->origin.z) * r->dir.z;
            if (tv <= 0.f) return true;
            const float tol = (std::max)(0.004f, tv * 0.004f);
            auto blocks = [&](const MeshVert& a, const MeshVert& b, const MeshVert& c) {
                float t = 0.f;
                return mesh_ray_tri(r->origin, r->dir, a, b, c, t) && t > 1e-4f && t < tv - tol;
            };
            for (const auto& f : mesh_edit.faces) {
                if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
                if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                    continue;
                if (skip_a >= 0 && (f.i0 == skip_a || f.i1 == skip_a || f.i2 == skip_a)) continue;
                if (skip_b >= 0 && (f.i0 == skip_b || f.i1 == skip_b || f.i2 == skip_b)) continue;
                if (blocks(mesh_edit.verts[static_cast<std::size_t>(f.i0)], mesh_edit.verts[static_cast<std::size_t>(f.i1)],
                           mesh_edit.verts[static_cast<std::size_t>(f.i2)]))
                    return false;
            }
            for (const auto& f : mesh_edit.src_tris) {
                if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
                if (static_cast<std::size_t>(f.i0) >= mesh_edit.src_verts.size() ||
                    static_cast<std::size_t>(f.i1) >= mesh_edit.src_verts.size() ||
                    static_cast<std::size_t>(f.i2) >= mesh_edit.src_verts.size())
                    continue;
                const auto& p0 = mesh_edit.src_verts[static_cast<std::size_t>(f.i0)];
                if (!near_cam_occ(p0.x, p0.y, p0.z)) continue;
                if (blocks(p0, mesh_edit.src_verts[static_cast<std::size_t>(f.i1)],
                           mesh_edit.src_verts[static_cast<std::size_t>(f.i2)]))
                    return false;
            }
            return true;
        };
        {
            struct Cand {
                float d;
                int i;
                float sx, sy;
                bool sel;
            };
            std::vector<Cand> cands;
            for (int i = 0; i < static_cast<int>(mesh_edit.verts.size()); ++i) {
                const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
                float sx = 0.f, sy = 0.f;
                if (!world_to_screen(v.x, v.y, v.z, 0.f, 0.f, fw, fh, wvp, sx, sy)) continue;
                const float d = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
                if (d >= best_vd) continue;
                cands.push_back({d, i, sx, sy, mesh_edit.sel_verts.count(i) != 0});
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                if (std::fabs(a.d - b.d) > 16.f) return a.d < b.d;
                if (a.sel != b.sel) return a.sel;
                return a.i > b.i;
            });
            int tested = 0;
            for (const auto& c : cands) {
                if (++tested > 24) break;
                const auto& v = mesh_edit.verts[static_cast<std::size_t>(c.i)];
                if (!point_visible(v.x, v.y, v.z, c.sx, c.sy, c.i, -1)) continue;
                best_v = c.i;
                best_vd = c.d;
                break;
            }
        }
        {
            struct Cand {
                float d;
                int i;
                float t, px, py;
            };
            std::vector<Cand> cands;
            for (int i = 0; i < static_cast<int>(mesh_edit.edges.size()); ++i) {
                const auto& e = mesh_edit.edges[static_cast<std::size_t>(i)];
                if (e.a < 0 || e.b < 0) continue;
                if (static_cast<std::size_t>(e.a) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(e.b) >= mesh_edit.verts.size())
                    continue;
                const auto& a = mesh_edit.verts[static_cast<std::size_t>(e.a)];
                const auto& b = mesh_edit.verts[static_cast<std::size_t>(e.b)];
                float sax = 0, say = 0, sbx = 0, sby = 0;
                if (!world_to_screen(a.x, a.y, a.z, 0, 0, fw, fh, wvp, sax, say)) continue;
                if (!world_to_screen(b.x, b.y, b.z, 0, 0, fw, fh, wvp, sbx, sby)) continue;
                const float den = (std::max)(1.f, (sbx - sax) * (sbx - sax) + (sby - say) * (sby - say));
                const float t = (std::max)(0.f, (std::min)(1.f, ((mx - sax) * (sbx - sax) + (my - say) * (sby - say)) / den));
                const float px = sax + t * (sbx - sax);
                const float py = say + t * (sby - say);
                const float d = (px - mx) * (px - mx) + (py - my) * (py - my);
                if (d < best_ed) cands.push_back({d, i, t, px, py});
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
            int tested = 0;
            for (const auto& c : cands) {
                if (++tested > 24) break;
                const auto& e = mesh_edit.edges[static_cast<std::size_t>(c.i)];
                const auto& a = mesh_edit.verts[static_cast<std::size_t>(e.a)];
                const auto& b = mesh_edit.verts[static_cast<std::size_t>(e.b)];
                // screen t is close enough to the world point under the cursor for an occlusion test
                const float wx = a.x + (b.x - a.x) * c.t, wy = a.y + (b.y - a.y) * c.t, wz = a.z + (b.z - a.z) * c.t;
                if (!point_visible(wx, wy, wz, c.px, c.py, e.a, e.b)) continue;
                best_e = c.i;
                best_ed = c.d;
                break;
            }
        }
        auto consider_tri = [&](const MeshVert& a, const MeshVert& b, const MeshVert& c, int kind, int idx,
                                const input::LunaCamera::PickRay& ray, float& best_t, int& best_kind, int& best_idx) {
            if (mesh_cross_sq(a, b, c) < 1e-12f) return;
            float t = 0.f;
            if (!mesh_ray_tri(ray.origin, ray.dir, a, b, c, t)) return;
            const bool closer = t < best_t - 1e-5f;
            const bool tie = std::fabs(t - best_t) < 1e-5f &&
                             (kind < best_kind || (kind == best_kind && idx < best_idx));
            if (!closer && !tie) return;
            best_t = t;
            best_kind = kind;
            best_idx = idx;
            if (kind == 0) {
                best_f = idx;
                best_src_f = -1;
            } else {
                best_src_f = idx;
                best_f = -1;
            }
            best_fd = t;
        };
        if (const auto ray = camera.make_pick_ray(mx, my, fw, fh)) {
            float best_t = 1.e30f;
            int best_kind = 2;
            int best_idx = 0x7fffffff;
            for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
                if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
                if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                    continue;
                consider_tri(mesh_edit.verts[static_cast<std::size_t>(f.i0)],
                             mesh_edit.verts[static_cast<std::size_t>(f.i1)],
                             mesh_edit.verts[static_cast<std::size_t>(f.i2)], 0, i, *ray, best_t, best_kind,
                             best_idx);
            }
            const auto near_cam = mesh_cam_filter();
            for (int i = 0; i < static_cast<int>(mesh_edit.src_tris.size()); ++i) {
                const auto& f = mesh_edit.src_tris[static_cast<std::size_t>(i)];
                if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
                if (static_cast<std::size_t>(f.i0) >= mesh_edit.src_verts.size() ||
                    static_cast<std::size_t>(f.i1) >= mesh_edit.src_verts.size() ||
                    static_cast<std::size_t>(f.i2) >= mesh_edit.src_verts.size())
                    continue;
                {
                    const auto& p = mesh_edit.src_verts[static_cast<std::size_t>(f.i0)];
                    if (!near_cam(p.x, p.y, p.z)) continue;
                }
                consider_tri(mesh_edit.src_verts[static_cast<std::size_t>(f.i0)],
                             mesh_edit.src_verts[static_cast<std::size_t>(f.i1)],
                             mesh_edit.src_verts[static_cast<std::size_t>(f.i2)], 1, i, *ray, best_t, best_kind,
                             best_idx);
            }
        }
    };

    auto mesh_pick_src_at = [&](float mx, float my, float fw, float fh, const float* wvp, int& best_v, int& best_e,
                               float& best_vd, float& best_ed) {
        best_v = best_e = -1;
        best_vd = 28.f * 28.f;
        best_ed = 22.f * 22.f;
        const auto near_cam = mesh_cam_filter();
        for (int i = 0; i < static_cast<int>(mesh_edit.src_verts.size()); ++i) {
            const auto& v = mesh_edit.src_verts[static_cast<std::size_t>(i)];
            if (!near_cam(v.x, v.y, v.z)) continue;
            float sx = 0.f, sy = 0.f;
            if (!world_to_screen(v.x, v.y, v.z, 0.f, 0.f, fw, fh, wvp, sx, sy)) continue;
            const float d = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
            if (d < best_vd) {
                best_vd = d;
                best_v = i;
            }
        }
        for (int i = 0; i < static_cast<int>(mesh_edit.src_edges.size()); ++i) {
            const auto& e = mesh_edit.src_edges[static_cast<std::size_t>(i)];
            if (e.a < 0 || e.b < 0) continue;
            if (static_cast<std::size_t>(e.a) >= mesh_edit.src_verts.size() ||
                static_cast<std::size_t>(e.b) >= mesh_edit.src_verts.size())
                continue;
            const auto& a = mesh_edit.src_verts[static_cast<std::size_t>(e.a)];
            const auto& b = mesh_edit.src_verts[static_cast<std::size_t>(e.b)];
            if (!near_cam(a.x, a.y, a.z) && !near_cam(b.x, b.y, b.z)) continue;
            float sax = 0, say = 0, sbx = 0, sby = 0;
            if (!world_to_screen(a.x, a.y, a.z, 0, 0, fw, fh, wvp, sax, say)) continue;
            if (!world_to_screen(b.x, b.y, b.z, 0, 0, fw, fh, wvp, sbx, sby)) continue;
            const float den = (std::max)(1.f, (sbx - sax) * (sbx - sax) + (sby - say) * (sby - say));
            const float t = (std::max)(0.f, (std::min)(1.f, ((mx - sax) * (sbx - sax) + (my - say) * (sby - say)) / den));
            const float px = sax + t * (sbx - sax);
            const float py = say + t * (sby - say);
            const float d = (px - mx) * (px - mx) + (py - my) * (py - my);
            if (d < best_ed) {
                best_ed = d;
                best_e = i;
            }
        }
    };

    auto mesh_face_same_verts = [](const MeshFace& f, int a, int b, int c) {
        int n = 0;
        if (f.i0 == a || f.i1 == a || f.i2 == a) ++n;
        if (f.i0 == b || f.i1 == b || f.i2 == b) ++n;
        if (f.i0 == c || f.i1 == c || f.i2 == c) ++n;
        return n == 3 && a != b && b != c && a != c;
    };
    auto mesh_intern_src_tri = [&](int si) -> int {
        if (si < 0 || static_cast<std::size_t>(si) >= mesh_edit.src_tris.size()) return -1;
        const auto& t = mesh_edit.src_tris[static_cast<std::size_t>(si)];
        if (t.i0 < 0 || t.i1 < 0 || t.i2 < 0) return -1;
        if (static_cast<std::size_t>(t.i0) >= mesh_edit.src_verts.size() ||
            static_cast<std::size_t>(t.i1) >= mesh_edit.src_verts.size() ||
            static_cast<std::size_t>(t.i2) >= mesh_edit.src_verts.size())
            return -1;
        const auto& a = mesh_edit.src_verts[static_cast<std::size_t>(t.i0)];
        const auto& b = mesh_edit.src_verts[static_cast<std::size_t>(t.i1)];
        const auto& c = mesh_edit.src_verts[static_cast<std::size_t>(t.i2)];
        const int f0 = mesh_find_xyz(a.x, a.y, a.z);
        const int f1 = mesh_find_xyz(b.x, b.y, b.z);
        const int f2 = mesh_find_xyz(c.x, c.y, c.z);
        if (f0 >= 0 && f1 >= 0 && f2 >= 0) {
            for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
                if (mesh_face_same_verts(mesh_edit.faces[static_cast<std::size_t>(i)], f0, f1, f2)) return i;
            }
        }
        mesh_geo_cap();
        const int i0 = mesh_intern_xyz(a.x, a.y, a.z, mesh_edit.src_kind, mesh_edit.src_id);
        const int i1 = mesh_intern_xyz(b.x, b.y, b.z, mesh_edit.src_kind, mesh_edit.src_id);
        const int i2 = mesh_intern_xyz(c.x, c.y, c.z, mesh_edit.src_kind, mesh_edit.src_id);
        if (i0 < 0 || i1 < 0 || i2 < 0 || i0 == i1 || i1 == i2 || i0 == i2) {
            if (!mesh_edit.geo_undo.empty()) mesh_edit.geo_undo.pop_back();
            return -1;
        }
        mesh_ensure_edge(i0, i1);
        mesh_ensure_edge(i1, i2);
        mesh_ensure_edge(i2, i0);
        for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
            if (mesh_face_same_verts(mesh_edit.faces[static_cast<std::size_t>(i)], i0, i1, i2)) return i;
        }
        mesh_edit.faces.push_back(mesh_new_face(i0, i1, i2, mesh_edit.tex_sel));
        mesh_edit.dirty = true;
        mesh_refresh_preview();
        return static_cast<int>(mesh_edit.faces.size()) - 1;
    };

    auto mesh_hover_clear = [&]() {
        mesh_edit.hover_v = mesh_edit.hover_e = mesh_edit.hover_f = -1;
        mesh_edit.src_hover_v = mesh_edit.src_hover_e = mesh_edit.src_hover_f = -1;
    };
    auto mesh_uv_get = [&](const MeshFace& f, int c, float& u, float& v) {
        if (c == 0) {
            u = f.u0;
            v = f.v0;
        } else if (c == 1) {
            u = f.u1;
            v = f.v1;
        } else {
            u = f.u2;
            v = f.v2;
        }
    };
    auto mesh_uv_set = [&](MeshFace& f, int c, float u, float v) {
        if (c == 0) {
            f.u0 = u;
            f.v0 = v;
        } else if (c == 1) {
            f.u1 = u;
            f.v1 = v;
        } else {
            f.u2 = u;
            f.v2 = v;
        }
    };
    auto mesh_face_vi = [&](const MeshFace& f, int c) -> int {
        return c == 0 ? f.i0 : (c == 1 ? f.i1 : f.i2);
    };
    auto mesh_uv_face_shown = [&](int fi) -> bool {
        if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) return false;
        if (!mesh_edit.sel_faces.empty()) return mesh_edit.sel_faces.count(fi) != 0;
        if (!mesh_edit.sel_uv.empty())
            return mesh_edit.sel_uv.count(fi * 3) || mesh_edit.sel_uv.count(fi * 3 + 1) ||
                   mesh_edit.sel_uv.count(fi * 3 + 2);
        return false;
    };
    auto mesh_find_edge_i = [&](int a, int b) -> int {
        if (a < 0 || b < 0 || a == b) return -1;
        const auto k = mesh_edge_key(a, b);
        for (int i = 0; i < static_cast<int>(mesh_edit.edges.size()); ++i) {
            const auto& e = mesh_edit.edges[static_cast<std::size_t>(i)];
            if (mesh_edge_key(e.a, e.b) == k) return i;
        }
        return -1;
    };
    auto mesh_uv_sync_from_3d = [&]() {
        mesh_edit.sel_uv.clear();
        if (mesh_edit.pick == MeshEdit::Pick::Faces) {
            for (int fi : mesh_edit.sel_faces) {
                if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
                mesh_edit.sel_uv.insert(fi * 3 + 0);
                mesh_edit.sel_uv.insert(fi * 3 + 1);
                mesh_edit.sel_uv.insert(fi * 3 + 2);
            }
        } else if (mesh_edit.pick == MeshEdit::Pick::Verts) {
            for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                for (int c = 0; c < 3; ++c)
                    if (mesh_edit.sel_verts.count(mesh_face_vi(f, c))) mesh_edit.sel_uv.insert(fi * 3 + c);
            }
        } else {
            for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                const int e01 = mesh_find_edge_i(f.i0, f.i1);
                const int e12 = mesh_find_edge_i(f.i1, f.i2);
                const int e20 = mesh_find_edge_i(f.i2, f.i0);
                if (e01 >= 0 && mesh_edit.sel_edges.count(e01)) {
                    mesh_edit.sel_uv.insert(fi * 3 + 0);
                    mesh_edit.sel_uv.insert(fi * 3 + 1);
                }
                if (e12 >= 0 && mesh_edit.sel_edges.count(e12)) {
                    mesh_edit.sel_uv.insert(fi * 3 + 1);
                    mesh_edit.sel_uv.insert(fi * 3 + 2);
                }
                if (e20 >= 0 && mesh_edit.sel_edges.count(e20)) {
                    mesh_edit.sel_uv.insert(fi * 3 + 2);
                    mesh_edit.sel_uv.insert(fi * 3 + 0);
                }
            }
        }
    };
    auto mesh_slot_valid = [&](int si) {
        return si >= 0 && static_cast<std::size_t>(si) < mesh_edit.slots.size();
    };
    auto mesh_slot_add = [&]() {
        MeshEdit::Slot sl{};
        sl.name = "Slot " + std::to_string(mesh_edit.slots.size() + 1);
        mesh_edit.slots.push_back(sl);
        mesh_edit.active_slot = static_cast<int>(mesh_edit.slots.size()) - 1;
        panel.set_status("Slot activo: " + sl.name + " - las caras que crees van a este slot");
    };
    auto mesh_slot_set_tex = [&](int si, int tex) {
        if (!mesh_slot_valid(si)) return;
        if (tex >= 0 && static_cast<std::size_t>(tex) >= mesh_edit.tex_list.size()) tex = -1;
        mesh_geo_cap();
        auto& sl = mesh_edit.slots[static_cast<std::size_t>(si)];
        sl.tex = tex;
        int n = 0;
        if (tex >= 0) {
            for (auto& f : mesh_edit.faces)
                if (f.slot == si) {
                    f.mat = tex;
                    ++n;
                }
            mesh_edit.tex_sel = tex;
        }
        mesh_edit.dirty = true;
        mesh_refresh_preview();
        panel.set_status(sl.name + ": textura aplicada a " + std::to_string(n) + " triangulos");
    };
    auto mesh_slot_assign_sel = [&](int si) {
        if (!mesh_slot_valid(si) || mesh_edit.sel_faces.empty()) return;
        mesh_geo_cap();
        const int tex = mesh_edit.slots[static_cast<std::size_t>(si)].tex;
        int n = 0;
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            f.slot = si;
            if (tex >= 0) f.mat = tex;
            ++n;
        }
        mesh_edit.dirty = true;
        mesh_refresh_preview();
        panel.set_status(std::to_string(n) + " caras -> " + mesh_edit.slots[static_cast<std::size_t>(si)].name);
    };
    auto mesh_slot_select = [&](int si, bool add) {
        if (!mesh_slot_valid(si)) return;
        mesh_sel_cap();
        mesh_edit.pick = MeshEdit::Pick::Faces;
        if (!add) mesh_edit.sel_faces.clear();
        for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi)
            if (mesh_edit.faces[static_cast<std::size_t>(fi)].slot == si) mesh_edit.sel_faces.insert(fi);
        mesh_uv_sync_from_3d();
    };
    auto mesh_slot_delete = [&](int si) {
        if (!mesh_slot_valid(si)) return;
        mesh_geo_cap();
        for (auto& f : mesh_edit.faces) {
            if (f.slot == si) f.slot = -1;
            else if (f.slot > si) --f.slot;
        }
        mesh_edit.slots.erase(mesh_edit.slots.begin() + si);
        if (mesh_edit.active_slot == si) mesh_edit.active_slot = -1;
        else if (mesh_edit.active_slot > si) --mesh_edit.active_slot;
    };
    auto mesh_uv_sync_to_3d = [&]() {
        if (mesh_edit.pick == MeshEdit::Pick::Faces) {
            mesh_edit.sel_faces.clear();
            for (int lp : mesh_edit.sel_uv) {
                const int fi = lp / 3;
                if (fi >= 0 && static_cast<std::size_t>(fi) < mesh_edit.faces.size())
                    mesh_edit.sel_faces.insert(fi);
            }
            mesh_edit.last_face = mesh_edit.sel_faces.empty() ? -1 : *mesh_edit.sel_faces.begin();
        } else if (mesh_edit.pick == MeshEdit::Pick::Verts) {
            mesh_edit.sel_verts.clear();
            for (int lp : mesh_edit.sel_uv) {
                const int fi = lp / 3, c = lp % 3;
                if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
                mesh_edit.sel_verts.insert(mesh_face_vi(mesh_edit.faces[static_cast<std::size_t>(fi)], c));
            }
            mesh_edit.last_vert = mesh_edit.sel_verts.empty() ? -1 : *mesh_edit.sel_verts.begin();
        } else {
            mesh_edit.sel_edges.clear();
            for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
                const bool c0 = mesh_edit.sel_uv.count(fi * 3 + 0) != 0;
                const bool c1 = mesh_edit.sel_uv.count(fi * 3 + 1) != 0;
                const bool c2 = mesh_edit.sel_uv.count(fi * 3 + 2) != 0;
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                auto take = [&](int a, int b) {
                    const int ei = mesh_find_edge_i(a, b);
                    if (ei >= 0) mesh_edit.sel_edges.insert(ei);
                };
                if (c0 && c1) take(f.i0, f.i1);
                if (c1 && c2) take(f.i1, f.i2);
                if (c2 && c0) take(f.i2, f.i0);
            }
            mesh_edit.last_edge = mesh_edit.sel_edges.empty() ? -1 : *mesh_edit.sel_edges.begin();
        }
    };
    auto mesh_deselect_all = [&]() {
        const bool any = !mesh_edit.sel_verts.empty() || !mesh_edit.sel_edges.empty() ||
                         !mesh_edit.sel_faces.empty() || !mesh_edit.sel_uv.empty();
        if (any) mesh_sel_cap();
        mesh_edit.sel_verts.clear();
        mesh_edit.sel_edges.clear();
        mesh_edit.sel_faces.clear();
        mesh_edit.sel_uv.clear();
        mesh_edit.last_vert = mesh_edit.last_edge = mesh_edit.last_face = -1;
        mesh_edit.last_uv = -1;
        mesh_edit.uv_hover = -1;
        mesh_edit.uv_hover_f = -1;
        mesh_hover_clear();
        mesh_refresh_preview();
    };
    auto mesh_uv_sel_clear = [&]() {
        if (!mesh_edit.sel_uv.empty()) mesh_sel_cap();
        mesh_edit.sel_uv.clear();
        mesh_edit.last_uv = -1;
    };
    auto mesh_toggle_select_all = [&]() {
        const bool any = !mesh_edit.sel_verts.empty() || !mesh_edit.sel_edges.empty() ||
                         !mesh_edit.sel_faces.empty() || !mesh_edit.sel_uv.empty();
        if (mesh_edit.uv_win_focused && mesh_edit.uv_editor_open) {
            if (!mesh_edit.sel_uv.empty()) {
                mesh_sel_cap();
                mesh_edit.sel_uv.clear();
                mesh_edit.last_uv = -1;
                panel.set_status("UV: desel");
                mesh_refresh_preview();
                return;
            }
            mesh_sel_cap();
            int mat = mesh_edit.tex_sel;
            for (int fi : mesh_edit.sel_faces) {
                if (fi >= 0 && static_cast<std::size_t>(fi) < mesh_edit.faces.size()) {
                    mat = mesh_edit.faces[static_cast<std::size_t>(fi)].mat;
                    break;
                }
            }
            for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                if (mat >= 0 && f.mat != mat) continue;
                if (!mesh_edit.sel_faces.empty() && !mesh_edit.sel_faces.count(fi)) continue;
                mesh_edit.sel_uv.insert(fi * 3 + 0);
                mesh_edit.sel_uv.insert(fi * 3 + 1);
                mesh_edit.sel_uv.insert(fi * 3 + 2);
            }
            panel.set_status("UV: sel caras/mat");
            mesh_refresh_preview();
            return;
        }
        if (any) {
            mesh_deselect_all();
            panel.set_status("Deseleccionado");
            return;
        }
        mesh_sel_cap();
        if (mesh_edit.pick == MeshEdit::Pick::Verts) {
            for (int i = 0; i < static_cast<int>(mesh_edit.verts.size()); ++i) mesh_edit.sel_verts.insert(i);
            mesh_edit.last_vert = mesh_edit.verts.empty() ? -1 : 0;
            mesh_uv_sync_from_3d();
        } else if (mesh_edit.pick == MeshEdit::Pick::Edges) {
            for (int i = 0; i < static_cast<int>(mesh_edit.edges.size()); ++i) mesh_edit.sel_edges.insert(i);
            mesh_edit.last_edge = mesh_edit.edges.empty() ? -1 : 0;
            mesh_uv_sync_from_3d();
        } else {
            for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) mesh_edit.sel_faces.insert(i);
            mesh_edit.last_face = mesh_edit.faces.empty() ? -1 : 0;
            mesh_uv_sync_from_3d();
        }
        mesh_refresh_preview();
        panel.set_status("Seleccionado todo");
    };
    auto mesh_face_ring_adj = [&]() -> std::unordered_map<int, std::vector<int>> {
        const int n = static_cast<int>(mesh_edit.faces.size());
        auto ek = [](int a, int b) -> std::uint64_t {
            const int lo = (std::min)(a, b), hi = (std::max)(a, b);
            return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint32_t>(hi);
        };
        std::unordered_map<std::uint64_t, std::vector<int>> ef;
        for (int i = 0; i < n; ++i) {
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
            ef[ek(f.i0, f.i1)].push_back(i);
            ef[ek(f.i1, f.i2)].push_back(i);
            ef[ek(f.i2, f.i0)].push_back(i);
        }
        std::unordered_map<int, std::vector<int>> adj;
        auto link = [&](int a, int b) {
            if (a == b) return;
            auto& na = adj[a];
            if (std::find(na.begin(), na.end(), b) == na.end()) na.push_back(b);
            auto& nb = adj[b];
            if (std::find(nb.begin(), nb.end(), a) == nb.end()) nb.push_back(a);
        };
        for (auto& [k, fl] : ef) {
            for (std::size_t i = 0; i < fl.size(); ++i)
                for (std::size_t j = i + 1; j < fl.size(); ++j) link(fl[i], fl[j]);
        }
        auto close_v = [&](int a, int b) -> bool {
            if (a < 0 || b < 0) return false;
            if (static_cast<std::size_t>(a) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(b) >= mesh_edit.verts.size())
                return false;
            const auto& va = mesh_edit.verts[static_cast<std::size_t>(a)];
            const auto& vb = mesh_edit.verts[static_cast<std::size_t>(b)];
            const float dx = va.x - vb.x, dy = va.y - vb.y, dz = va.z - vb.z;
            return dx * dx + dy * dy + dz * dz <= mesh_edit.weld_m * mesh_edit.weld_m &&
                   mesh_verts_same_surface(a, b);
        };
        for (int i = 0; i < n; ++i) {
            const auto& fa = mesh_edit.faces[static_cast<std::size_t>(i)];
            const int ia[3] = {fa.i0, fa.i1, fa.i2};
            for (int j = i + 1; j < n; ++j) {
                const auto& fb = mesh_edit.faces[static_cast<std::size_t>(j)];
                const int ib[3] = {fb.i0, fb.i1, fb.i2};
                int hits = 0;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        if (ia[a] == ib[b] || close_v(ia[a], ib[b])) ++hits;
                if (hits >= 2) link(i, j);
            }
        }
        return adj;
    };
    auto mesh_select_more = [&]() {
        mesh_sel_cap();
        if (mesh_edit.pick == MeshEdit::Pick::Verts) {
            if (mesh_edit.sel_verts.empty()) {
                panel.set_status("Ctrl++: sin sel");
                return;
            }
            std::unordered_set<int> extra;
            for (const auto& e : mesh_edit.edges) {
                const bool sa = mesh_edit.sel_verts.count(e.a) != 0;
                const bool sb = mesh_edit.sel_verts.count(e.b) != 0;
                if (sa && !sb) extra.insert(e.b);
                if (sb && !sa) extra.insert(e.a);
            }
            for (const auto& f : mesh_edit.faces) {
                const int idx[3] = {f.i0, f.i1, f.i2};
                bool any = false;
                for (int v : idx)
                    if (mesh_edit.sel_verts.count(v)) {
                        any = true;
                        break;
                    }
                if (!any) continue;
                for (int v : idx)
                    if (!mesh_edit.sel_verts.count(v)) extra.insert(v);
            }
            for (int i : extra) mesh_edit.sel_verts.insert(i);
        } else if (mesh_edit.pick == MeshEdit::Pick::Edges) {
            if (mesh_edit.sel_edges.empty()) {
                panel.set_status("Ctrl++: sin sel");
                return;
            }
            std::unordered_set<int> extra;
            for (const auto& f : mesh_edit.faces) {
                const int ee[3] = {mesh_find_edge_i(f.i0, f.i1), mesh_find_edge_i(f.i1, f.i2),
                                   mesh_find_edge_i(f.i2, f.i0)};
                bool any = false;
                for (int e : ee)
                    if (e >= 0 && mesh_edit.sel_edges.count(e)) {
                        any = true;
                        break;
                    }
                if (!any) continue;
                for (int e : ee)
                    if (e >= 0 && !mesh_edit.sel_edges.count(e)) extra.insert(e);
            }
            for (int i : extra) mesh_edit.sel_edges.insert(i);
        } else {
            if (mesh_edit.sel_faces.empty()) {
                panel.set_status("Ctrl++: sin sel");
                return;
            }
            auto adj = mesh_face_ring_adj();
            std::unordered_set<int> extra;
            for (int fi : mesh_edit.sel_faces) {
                auto it = adj.find(fi);
                if (it == adj.end()) continue;
                for (int o : it->second)
                    if (!mesh_edit.sel_faces.count(o)) extra.insert(o);
            }
            for (int i : extra) mesh_edit.sel_faces.insert(i);
        }
        mesh_uv_sync_from_3d();
        panel.set_status("Select more");
    };
    auto mesh_select_less = [&]() {
        mesh_sel_cap();
        if (mesh_edit.pick == MeshEdit::Pick::Verts) {
            if (mesh_edit.sel_verts.empty()) return;
            std::unordered_set<int> drop;
            for (int v : mesh_edit.sel_verts) {
                bool border = false;
                for (const auto& e : mesh_edit.edges) {
                    if (e.a == v && !mesh_edit.sel_verts.count(e.b)) {
                        border = true;
                        break;
                    }
                    if (e.b == v && !mesh_edit.sel_verts.count(e.a)) {
                        border = true;
                        break;
                    }
                }
                if (!border) {
                    for (const auto& f : mesh_edit.faces) {
                        const int idx[3] = {f.i0, f.i1, f.i2};
                        bool has = false;
                        for (int x : idx)
                            if (x == v) has = true;
                        if (!has) continue;
                        for (int x : idx)
                            if (!mesh_edit.sel_verts.count(x)) {
                                border = true;
                                break;
                            }
                        if (border) break;
                    }
                }
                if (border) drop.insert(v);
            }
            for (int i : drop) mesh_edit.sel_verts.erase(i);
        } else if (mesh_edit.pick == MeshEdit::Pick::Edges) {
            if (mesh_edit.sel_edges.empty()) return;
            std::unordered_set<int> drop;
            for (const auto& f : mesh_edit.faces) {
                const int ee[3] = {mesh_find_edge_i(f.i0, f.i1), mesh_find_edge_i(f.i1, f.i2),
                                   mesh_find_edge_i(f.i2, f.i0)};
                bool any_sel = false, any_un = false;
                for (int e : ee) {
                    if (e < 0) continue;
                    if (mesh_edit.sel_edges.count(e)) any_sel = true;
                    else any_un = true;
                }
                if (any_sel && any_un)
                    for (int e : ee)
                        if (e >= 0 && mesh_edit.sel_edges.count(e)) drop.insert(e);
            }
            for (int i : drop) mesh_edit.sel_edges.erase(i);
        } else {
            if (mesh_edit.sel_faces.empty()) return;
            auto adj = mesh_face_ring_adj();
            std::unordered_set<int> drop;
            for (int fi : mesh_edit.sel_faces) {
                auto it = adj.find(fi);
                if (it == adj.end()) continue;
                for (int o : it->second)
                    if (!mesh_edit.sel_faces.count(o)) {
                        drop.insert(fi);
                        break;
                    }
            }
            for (int i : drop) mesh_edit.sel_faces.erase(i);
        }
        mesh_uv_sync_from_3d();
        panel.set_status("Select less");
    };
    auto mesh_circle_paint = [&](float mx, float my, float fw, float fh, const float* wvp, bool desel) {
        if (!mesh_edit.circle_stroke_cap) {
            mesh_sel_cap();
            mesh_edit.circle_stroke_cap = true;
        }
        const float r2 = mesh_edit.circle_r * mesh_edit.circle_r;
        auto inside = [&](float sx, float sy) {
            const float dx = sx - mx, dy = sy - my;
            return dx * dx + dy * dy <= r2;
        };
        auto tog = [&](std::unordered_set<int>& s, int id) {
            if (desel) s.erase(id);
            else s.insert(id);
        };
        if (mesh_edit.pick == MeshEdit::Pick::Verts) {
            for (int i = 0; i < static_cast<int>(mesh_edit.verts.size()); ++i) {
                const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
                float sx = 0.f, sy = 0.f;
                if (!world_to_screen(v.x, v.y, v.z, 0.f, 0.f, fw, fh, wvp, sx, sy)) continue;
                if (inside(sx, sy)) tog(mesh_edit.sel_verts, i);
            }
        } else if (mesh_edit.pick == MeshEdit::Pick::Edges) {
            for (int i = 0; i < static_cast<int>(mesh_edit.edges.size()); ++i) {
                const auto& e = mesh_edit.edges[static_cast<std::size_t>(i)];
                if (e.a < 0 || e.b < 0) continue;
                if (static_cast<std::size_t>(e.a) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(e.b) >= mesh_edit.verts.size())
                    continue;
                const auto& a = mesh_edit.verts[static_cast<std::size_t>(e.a)];
                const auto& b = mesh_edit.verts[static_cast<std::size_t>(e.b)];
                float sax = 0, say = 0, sbx = 0, sby = 0;
                if (!world_to_screen(a.x, a.y, a.z, 0, 0, fw, fh, wvp, sax, say)) continue;
                if (!world_to_screen(b.x, b.y, b.z, 0, 0, fw, fh, wvp, sbx, sby)) continue;
                const float den = (std::max)(1.f, (sbx - sax) * (sbx - sax) + (sby - say) * (sby - say));
                const float t =
                    (std::max)(0.f, (std::min)(1.f, ((mx - sax) * (sbx - sax) + (my - say) * (sby - say)) / den));
                const float px = sax + t * (sbx - sax);
                const float py = say + t * (sby - say);
                if (inside(px, py) || inside(sax, say) || inside(sbx, sby)) tog(mesh_edit.sel_edges, i);
            }
        } else {
            for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
                if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
                if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                    continue;
                const auto& a = mesh_edit.verts[static_cast<std::size_t>(f.i0)];
                const auto& b = mesh_edit.verts[static_cast<std::size_t>(f.i1)];
                const auto& c = mesh_edit.verts[static_cast<std::size_t>(f.i2)];
                float sax = 0, say = 0, sbx = 0, sby = 0, scx = 0, scy = 0;
                if (!world_to_screen(a.x, a.y, a.z, 0, 0, fw, fh, wvp, sax, say)) continue;
                if (!world_to_screen(b.x, b.y, b.z, 0, 0, fw, fh, wvp, sbx, sby)) continue;
                if (!world_to_screen(c.x, c.y, c.z, 0, 0, fw, fh, wvp, scx, scy)) continue;
                const float cx = (sax + sbx + scx) * (1.f / 3.f);
                const float cy = (say + sby + scy) * (1.f / 3.f);
                if (inside(cx, cy) || inside(sax, say) || inside(sbx, sby) || inside(scx, scy))
                    tog(mesh_edit.sel_faces, i);
            }
        }
        mesh_uv_sync_from_3d();
    };
    auto mesh_circle_paint_uv = [&](float mx, float my, float p0x, float p0y, float sz, bool desel) {
        if (!mesh_edit.circle_stroke_cap) {
            mesh_sel_cap();
            mesh_edit.circle_stroke_cap = true;
        }
        const float r2 = mesh_edit.circle_r * mesh_edit.circle_r;
        auto inside = [&](float u, float v) {
            const float sx = p0x + u * sz;
            const float sy = p0y + v * sz;
            const float dx = sx - mx, dy = sy - my;
            return dx * dx + dy * dy <= r2;
        };
        auto tog = [&](std::unordered_set<int>& s, int id) {
            if (desel) s.erase(id);
            else s.insert(id);
        };
        if (mesh_edit.pick == MeshEdit::Pick::Faces) {
            for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
                if (!mesh_uv_face_shown(i)) continue;
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
                const float cu = (f.u0 + f.u1 + f.u2) * (1.f / 3.f);
                const float cv = (f.v0 + f.v1 + f.v2) * (1.f / 3.f);
                if (inside(cu, cv) || inside(f.u0, f.v0) || inside(f.u1, f.v1) || inside(f.u2, f.v2)) {
                    tog(mesh_edit.sel_uv, i * 3 + 0);
                    tog(mesh_edit.sel_uv, i * 3 + 1);
                    tog(mesh_edit.sel_uv, i * 3 + 2);
                }
            }
        } else if (mesh_edit.pick == MeshEdit::Pick::Verts) {
            for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
                if (!mesh_uv_face_shown(fi)) continue;
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                const float us[3] = {f.u0, f.u1, f.u2};
                const float vs[3] = {f.v0, f.v1, f.v2};
                for (int c = 0; c < 3; ++c)
                    if (inside(us[c], vs[c])) tog(mesh_edit.sel_uv, fi * 3 + c);
            }
        } else {
            for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
                if (!mesh_uv_face_shown(fi)) continue;
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                const float us[3] = {f.u0, f.u1, f.u2};
                const float vs[3] = {f.v0, f.v1, f.v2};
                for (int c = 0; c < 3; ++c) {
                    if (inside(us[c], vs[c]) || inside(us[(c + 1) % 3], vs[(c + 1) % 3])) {
                        tog(mesh_edit.sel_uv, fi * 3 + c);
                        tog(mesh_edit.sel_uv, fi * 3 + ((c + 1) % 3));
                    }
                }
            }
        }
    };
    auto mesh_click_id = [&](std::unordered_set<int>& s, int id, int& last, bool shift, bool cap) {
        if (cap) mesh_sel_cap();
        if (shift) {
            if (s.count(id)) {
                s.erase(id);
                if (last == id) last = -1;
            } else {
                s.insert(id);
                last = id;
            }
            return;
        }
        if (s.count(id)) {
            s.erase(id);
            last = -1;
            return;
        }
        s.clear();
        s.insert(id);
        last = id;
    };

    auto mesh_update_hover = [&](float mx, float my, float fw, float fh, const float* wvp) {
        mesh_edit.hover_v = mesh_edit.hover_e = mesh_edit.hover_f = -1;
        mesh_edit.src_hover_v = mesh_edit.src_hover_e = mesh_edit.src_hover_f = -1;
        mesh_edit.hover_mx = mx;
        mesh_edit.hover_my = my;
        if (edit_mode != EditMode::Mesh) return;
        mesh_src_sync(mx, my, fw, fh, wvp);
        int best_v = -1, best_e = -1, best_f = -1, best_src_f = -1;
        float best_vd = 0, best_ed = 0, best_fd = 0;
        mesh_pick_at(mx, my, fw, fh, wvp, best_v, best_e, best_f, best_src_f, best_vd, best_ed, best_fd);
        int src_v = -1, src_e = -1;
        float src_vd = 0, src_ed = 0;
        mesh_pick_src_at(mx, my, fw, fh, wvp, src_v, src_e, src_vd, src_ed);
        if (mesh_edit.pick == MeshEdit::Pick::Edges) {
            const bool src_edge = src_e >= 0 && (best_e < 0 || src_ed <= best_ed);
            if (src_edge) mesh_edit.src_hover_e = src_e;
            else if (best_e >= 0) mesh_edit.hover_e = best_e;
        } else if (mesh_edit.pick == MeshEdit::Pick::Faces) {
            if (best_f >= 0) mesh_edit.hover_f = best_f;
            else if (best_src_f >= 0) mesh_edit.src_hover_f = best_src_f;
        } else {
            const bool src_vert = src_v >= 0 && (best_v < 0 || src_vd <= best_vd);
            if (src_vert) mesh_edit.src_hover_v = src_v;
            else if (best_v >= 0) mesh_edit.hover_v = best_v;
        }
    };

    auto handle_mesh_click = [&](float mx, float my, float fw, float fh, const float* wvp, bool shift,
                                 bool ctrl) {
        mesh_src_sync(mx, my, fw, fh, wvp);
        int best_v = -1, best_e = -1, best_f = -1, best_src_f = -1;
        float best_vd = 0, best_ed = 0, best_fd = 0;
        mesh_pick_at(mx, my, fw, fh, wvp, best_v, best_e, best_f, best_src_f, best_vd, best_ed, best_fd);
        int src_v = -1, src_e = -1;
        float src_vd = 0, src_ed = 0;
        mesh_pick_src_at(mx, my, fw, fh, wvp, src_v, src_e, src_vd, src_ed);
        auto intern_src_vert = [&](int si) -> int {
            if (si < 0 || static_cast<std::size_t>(si) >= mesh_edit.src_verts.size()) return -1;
            const auto& p = mesh_edit.src_verts[static_cast<std::size_t>(si)];
            const int sk = p.src_kind != 0 ? p.src_kind : mesh_edit.src_kind;
            const std::int32_t sid = p.src_kind != 0 ? p.src_id : mesh_edit.src_id;
            return mesh_intern_xyz(p.x, p.y, p.z, sk, sid);
        };
        auto has_edge = [&](int a, int b) -> bool { return mesh_find_edge_i(a, b) >= 0; };
        // ---- Ctrl+click shortest path over the SOURCE geometry (the spline / .sco being traced) ----
        // The mesh path search only knows the edges already taken into the mesh, so a path through
        // source edges not picked yet failed ("sin camino"). Source vertices that coincide (strip
        // seams) are merged by position first.
        std::vector<int> src_canon;
        auto build_src_canon = [&]() {
            if (!src_canon.empty() || mesh_edit.src_verts.empty()) return;
            const float q = (std::max)(mesh_edit.weld_m, 1e-4f);
            std::unordered_map<std::uint64_t, int> first;
            src_canon.resize(mesh_edit.src_verts.size());
            for (std::size_t i = 0; i < mesh_edit.src_verts.size(); ++i) {
                const auto& v = mesh_edit.src_verts[i];
                const auto kx = static_cast<std::int64_t>(std::llround(v.x / q));
                const auto ky = static_cast<std::int64_t>(std::llround(v.y / q));
                const auto kz = static_cast<std::int64_t>(std::llround(v.z / q));
                const std::uint64_t key = (static_cast<std::uint64_t>(kx) * 73856093ull) ^
                                          (static_cast<std::uint64_t>(ky) * 19349663ull) ^
                                          (static_cast<std::uint64_t>(kz) * 83492791ull);
                const auto it = first.find(key);
                src_canon[i] = it != first.end() ? it->second : static_cast<int>(i);
                if (it == first.end()) first.emplace(key, static_cast<int>(i));
            }
        };
        auto src_at_xyz = [&](float x, float y, float z) -> int {
            const float k = (std::max)(mesh_edit.weld_m * mesh_edit.weld_m, 1e-8f);
            int best = -1;
            float bd = k;
            for (int i = 0; i < static_cast<int>(mesh_edit.src_verts.size()); ++i) {
                const auto& v = mesh_edit.src_verts[static_cast<std::size_t>(i)];
                const float d = (v.x - x) * (v.x - x) + (v.y - y) * (v.y - y) + (v.z - z) * (v.z - z);
                if (d <= bd) {
                    bd = d;
                    best = i;
                }
            }
            if (best < 0) return -1;
            build_src_canon();
            return src_canon[static_cast<std::size_t>(best)];
        };
        auto src_vert_path = [&](int from_c, int to_c, std::vector<int>& path) -> bool {
            build_src_canon();
            if (from_c < 0 || to_c < 0) return false;
            std::unordered_map<int, std::vector<int>> adj;
            auto link = [&](int a, int b) {
                if (a < 0 || b < 0 || static_cast<std::size_t>(a) >= src_canon.size() ||
                    static_cast<std::size_t>(b) >= src_canon.size())
                    return;
                const int ca = src_canon[static_cast<std::size_t>(a)], cb = src_canon[static_cast<std::size_t>(b)];
                if (ca == cb) return;
                adj[ca].push_back(cb);
                adj[cb].push_back(ca);
            };
            for (const auto& e : mesh_edit.src_edges) link(e.a, e.b);
            for (const auto& t : mesh_edit.src_tris) {
                link(t.i0, t.i1);
                link(t.i1, t.i2);
                link(t.i2, t.i0);
            }
            return mesh_bfs_nodes(adj, from_c, to_c, path);
        };
        auto intern_src_canon = [&](int c) -> int {
            if (c < 0 || static_cast<std::size_t>(c) >= mesh_edit.src_verts.size()) return -1;
            const auto& p = mesh_edit.src_verts[static_cast<std::size_t>(c)];
            const int sk = p.src_kind != 0 ? p.src_kind : mesh_edit.src_kind;
            const std::int32_t sid = p.src_kind != 0 ? p.src_id : mesh_edit.src_id;
            return mesh_intern_xyz(p.x, p.y, p.z, sk, sid);
        };
        // shortest of the paths between any end of A and any end of B (canonical source vertices)
        auto best_src_path = [&](const std::vector<int>& ends_a, const std::vector<int>& ends_b,
                                 std::vector<int>& out) -> bool {
            bool ok = false;
            for (int a : ends_a)
                for (int b : ends_b) {
                    std::vector<int> pth;
                    if (!src_vert_path(a, b, pth)) continue;
                    if (!ok || pth.size() < out.size()) {
                        out = std::move(pth);
                        ok = true;
                    }
                }
            return ok;
        };
        if (mesh_edit.pick == MeshEdit::Pick::Faces) {
            int fi = best_f;
            bool new_geo = false;
            if (fi < 0 && best_src_f >= 0) {
                const int nf0 = static_cast<int>(mesh_edit.faces.size());
                fi = mesh_intern_src_tri(best_src_f);
                new_geo = static_cast<int>(mesh_edit.faces.size()) > nf0;
            }
            if (fi < 0) {
                mesh_deselect_all();
                return;
            }
            if (ctrl) {
                if (!new_geo) mesh_sel_cap();
                int src = mesh_edit.last_face;
                if (src < 0 && !mesh_edit.sel_faces.empty()) src = *mesh_edit.sel_faces.begin();
                bool src_done = false;
                if (src >= 0 && best_src_f >= 0 && static_cast<std::size_t>(src) < mesh_edit.faces.size()) {
                    // walk the source triangles from the one under the last face to the clicked one
                    build_src_canon();
                    auto tri_c = [&](int ti, int k) {
                        const auto& t = mesh_edit.src_tris[static_cast<std::size_t>(ti)];
                        const int idx = k == 0 ? t.i0 : k == 1 ? t.i1 : t.i2;
                        return (idx >= 0 && static_cast<std::size_t>(idx) < src_canon.size())
                                   ? src_canon[static_cast<std::size_t>(idx)]
                                   : -1;
                    };
                    const auto& lf = mesh_edit.faces[static_cast<std::size_t>(src)];
                    const int lc[3] = {
                        src_at_xyz(mesh_edit.verts[static_cast<std::size_t>(lf.i0)].x, mesh_edit.verts[static_cast<std::size_t>(lf.i0)].y, mesh_edit.verts[static_cast<std::size_t>(lf.i0)].z),
                        src_at_xyz(mesh_edit.verts[static_cast<std::size_t>(lf.i1)].x, mesh_edit.verts[static_cast<std::size_t>(lf.i1)].y, mesh_edit.verts[static_cast<std::size_t>(lf.i1)].z),
                        src_at_xyz(mesh_edit.verts[static_cast<std::size_t>(lf.i2)].x, mesh_edit.verts[static_cast<std::size_t>(lf.i2)].y, mesh_edit.verts[static_cast<std::size_t>(lf.i2)].z)};
                    int start_t = -1;
                    const int nt = static_cast<int>(mesh_edit.src_tris.size());
                    for (int ti = 0; ti < nt && start_t < 0; ++ti) {
                        int hit = 0;
                        for (int k = 0; k < 3; ++k) {
                            const int c = tri_c(ti, k);
                            if (c >= 0 && (c == lc[0] || c == lc[1] || c == lc[2])) ++hit;
                        }
                        if (hit == 3) start_t = ti;
                    }
                    if (start_t >= 0) {
                        std::unordered_map<std::uint64_t, std::vector<int>> by_edge;
                        auto ek = [](int a, int b) {
                            const int lo = (std::min)(a, b), hi = (std::max)(a, b);
                            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(lo)) << 32) |
                                   static_cast<std::uint32_t>(hi);
                        };
                        for (int ti = 0; ti < nt; ++ti) {
                            const int a = tri_c(ti, 0), b = tri_c(ti, 1), c = tri_c(ti, 2);
                            if (a < 0 || b < 0 || c < 0) continue;
                            by_edge[ek(a, b)].push_back(ti);
                            by_edge[ek(b, c)].push_back(ti);
                            by_edge[ek(c, a)].push_back(ti);
                        }
                        std::unordered_map<int, std::vector<int>> tadj;
                        for (auto& [k, lst] : by_edge)
                            for (std::size_t i = 0; i < lst.size(); ++i)
                                for (std::size_t j = i + 1; j < lst.size(); ++j) {
                                    tadj[lst[i]].push_back(lst[j]);
                                    tadj[lst[j]].push_back(lst[i]);
                                }
                        std::vector<int> tpath;
                        if (mesh_bfs_nodes(tadj, start_t, best_src_f, tpath)) {
                            // (mesh_intern_src_tri takes its own undo snapshot for new triangles)
                            for (int ti : tpath) {
                                const int f2 = mesh_intern_src_tri(ti);
                                if (f2 >= 0) mesh_edit.sel_faces.insert(f2);
                            }
                            mesh_edit.sel_faces.insert(src);
                            mesh_edit.last_face = fi;
                            src_done = true;
                        }
                    }
                }
                if (src_done) {
                } else if (src >= 0) {
                    mesh_edit.last_face = src;
                    mesh_select_path_faces(fi, false);
                } else {
                    mesh_edit.sel_faces.insert(fi);
                    mesh_edit.last_face = fi;
                }
            } else {
                mesh_click_id(mesh_edit.sel_faces, fi, mesh_edit.last_face, shift, !new_geo);
            }
            mesh_uv_sync_from_3d();
            return;
        }
        if (mesh_edit.pick == MeshEdit::Pick::Edges) {
            const bool use_src_e = src_e >= 0 && (best_e < 0 || src_ed <= best_ed);
            int ei = -1;
            bool new_geo = false;
            if (use_src_e) {
                const auto& se = mesh_edit.src_edges[static_cast<std::size_t>(src_e)];
                const auto& pa = mesh_edit.src_verts[static_cast<std::size_t>(se.a)];
                const auto& pb = mesh_edit.src_verts[static_cast<std::size_t>(se.b)];
                const int ia = mesh_find_xyz(pa.x, pa.y, pa.z);
                const int ib = mesh_find_xyz(pb.x, pb.y, pb.z);
                if (ia < 0 || ib < 0 || !has_edge(ia, ib)) {
                    mesh_geo_cap();
                    new_geo = true;
                }
                ei = mesh_ensure_edge(intern_src_vert(se.a), intern_src_vert(se.b));
            } else if (best_e >= 0) {
                ei = best_e;
            }
            if (ei < 0) {
                mesh_deselect_all();
                return;
            }
            if (ctrl) {
                if (!new_geo) mesh_sel_cap();
                int src = mesh_edit.last_edge;
                if (src < 0 && !mesh_edit.sel_edges.empty()) src = *mesh_edit.sel_edges.begin();
                bool src_done = false;
                if (src >= 0 && use_src_e && static_cast<std::size_t>(src) < mesh_edit.edges.size()) {
                    const auto& le = mesh_edit.edges[static_cast<std::size_t>(src)];
                    const auto& va = mesh_edit.verts[static_cast<std::size_t>(le.a)];
                    const auto& vb = mesh_edit.verts[static_cast<std::size_t>(le.b)];
                    build_src_canon();
                    const auto& se = mesh_edit.src_edges[static_cast<std::size_t>(src_e)];
                    std::vector<int> ends_a{src_at_xyz(va.x, va.y, va.z), src_at_xyz(vb.x, vb.y, vb.z)};
                    std::vector<int> ends_b{src_canon[static_cast<std::size_t>(se.a)], src_canon[static_cast<std::size_t>(se.b)]};
                    std::vector<int> vp;
                    if (best_src_path(ends_a, ends_b, vp)) {
                        if (!new_geo) mesh_geo_cap();
                        for (std::size_t k = 0; k + 1 < vp.size(); ++k) {
                            const int e2 = mesh_ensure_edge(intern_src_canon(vp[k]), intern_src_canon(vp[k + 1]));
                            if (e2 >= 0) mesh_edit.sel_edges.insert(e2);
                        }
                        mesh_edit.sel_edges.insert(src);
                        mesh_edit.sel_edges.insert(ei);
                        mesh_edit.last_edge = ei;
                        src_done = true;
                    }
                }
                if (src_done) {
                } else if (src >= 0) {
                    mesh_edit.last_edge = src;
                    mesh_select_path_edges(src, ei, true);
                } else {
                    mesh_click_id(mesh_edit.sel_edges, ei, mesh_edit.last_edge, shift, false);
                }
            } else {
                mesh_click_id(mesh_edit.sel_edges, ei, mesh_edit.last_edge, shift, !new_geo);
            }
            mesh_uv_sync_from_3d();
            return;
        }
        int pick_v = -1;
        bool new_geo = false;
        if (src_v >= 0 && (best_v < 0 || src_vd <= best_vd)) {
            const auto& p = mesh_edit.src_verts[static_cast<std::size_t>(src_v)];
            if (mesh_find_xyz(p.x, p.y, p.z) < 0) {
                mesh_geo_cap();
                new_geo = true;
            }
            pick_v = intern_src_vert(src_v);
        } else if (best_v >= 0) {
            pick_v = best_v;
        }
        if (pick_v < 0) {
            mesh_deselect_all();
            return;
        }
        if (ctrl) {
            if (!new_geo) mesh_sel_cap();
            int src = mesh_edit.last_vert;
            if (src < 0 && !mesh_edit.sel_verts.empty()) src = *mesh_edit.sel_verts.begin();
            bool src_done = false;
            if (src >= 0 && src_v >= 0 && static_cast<std::size_t>(src) < mesh_edit.verts.size()) {
                const auto& lv = mesh_edit.verts[static_cast<std::size_t>(src)];
                build_src_canon();
                std::vector<int> vp;
                if (best_src_path({src_at_xyz(lv.x, lv.y, lv.z)}, {src_canon[static_cast<std::size_t>(src_v)]}, vp)) {
                    if (!new_geo) mesh_geo_cap();
                    int prev = -1;
                    for (int c : vp) {
                        const int vi = intern_src_canon(c);
                        if (vi < 0) continue;
                        mesh_edit.sel_verts.insert(vi);
                        if (prev >= 0 && prev != vi) mesh_ensure_edge(prev, vi);
                        prev = vi;
                    }
                    mesh_edit.last_vert = pick_v;
                    src_done = true;
                }
            }
            if (src_done) {
            } else if (src >= 0) {
                mesh_edit.last_vert = src;
                mesh_select_path_verts(src, pick_v, true);
            } else {
                mesh_click_id(mesh_edit.sel_verts, pick_v, mesh_edit.last_vert, shift, false);
            }
        } else {
            mesh_click_id(mesh_edit.sel_verts, pick_v, mesh_edit.last_vert, shift, !new_geo);
        }
        mesh_uv_sync_from_3d();
    };

    auto mesh_collect_affected = [&]() -> std::vector<int> {
        std::unordered_set<int> s = mesh_edit.sel_verts;
        for (int ei : mesh_edit.sel_edges) {
            if (ei < 0 || static_cast<std::size_t>(ei) >= mesh_edit.edges.size()) continue;
            s.insert(mesh_edit.edges[static_cast<std::size_t>(ei)].a);
            s.insert(mesh_edit.edges[static_cast<std::size_t>(ei)].b);
        }
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            s.insert(f.i0);
            s.insert(f.i1);
            s.insert(f.i2);
        }
        return {s.begin(), s.end()};
    };
    auto mesh_weld_all = [&]() {
        const float k = mesh_edit.weld_m * mesh_edit.weld_m;
        const int n = static_cast<int>(mesh_edit.verts.size());
        std::vector<int> map(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) map[static_cast<std::size_t>(i)] = i;
        for (int i = 0; i < n; ++i) {
            if (map[static_cast<std::size_t>(i)] != i) continue;
            const auto& a = mesh_edit.verts[static_cast<std::size_t>(i)];
            for (int j = i + 1; j < n; ++j) {
                if (map[static_cast<std::size_t>(j)] != j) continue;
                const auto& b = mesh_edit.verts[static_cast<std::size_t>(j)];
                const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                if (dx * dx + dy * dy + dz * dz > k) continue;
                map[static_cast<std::size_t>(j)] = i;
                if (a.src_kind != b.src_kind || a.src_id != b.src_id)
                    mesh_edit.verts[static_cast<std::size_t>(i)].src_kind = 0;
            }
        }
        auto remap = [&](int i) {
            int c = i;
            while (c != map[static_cast<std::size_t>(c)]) c = map[static_cast<std::size_t>(c)];
            return c;
        };
        for (auto& e : mesh_edit.edges) {
            e.a = remap(e.a);
            e.b = remap(e.b);
        }
        for (auto& f : mesh_edit.faces) {
            f.i0 = remap(f.i0);
            f.i1 = remap(f.i1);
            f.i2 = remap(f.i2);
        }
    };
    auto mesh_xform_axis_parallel = [&](int ax) -> bool {
        if (ax < 0 || ax > 2 || mesh_xform.affected.size() < 2) return false;
        float mn[3] = {1.e30f, 1.e30f, 1.e30f};
        float mx[3] = {-1.e30f, -1.e30f, -1.e30f};
        for (int i : mesh_xform.affected) {
            if (i < 0 || static_cast<std::size_t>(i) >= mesh_edit.verts.size()) continue;
            const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
            const float p[3] = {v.x, v.y, v.z};
            for (int k = 0; k < 3; ++k) {
                mn[k] = (std::min)(mn[k], p[k]);
                mx[k] = (std::max)(mx[k], p[k]);
            }
        }
        const float sp[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
        const int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
        return sp[ax] > 1e-4f && sp[a1] < 0.02f && sp[a2] < 0.02f;
    };
    auto mesh_xform_status_axis = [&]() {
        if (mesh_xform.mode == 2) {
            std::string st = "S";
            if (mesh_xform.scale_mask != 7) {
                st += "  ";
                if (mesh_xform.scale_mask & 1) st += 'X';
                if (mesh_xform.scale_mask & 2) st += 'Y';
                if (mesh_xform.scale_mask & 4) st += 'Z';
            }
            panel.set_status(st);
            return;
        }
        if (mesh_xform.axis < 0) {
            panel.set_status(omsi::ui::i18n::tr_lit("eje View"));
            return;
        }
        const char* n = mesh_xform.axis == 0 ? "X" : (mesh_xform.axis == 1 ? "Y" : "Z");
        const char* frame = mesh_xform.local ? " (cara)" : "";
        if (mesh_xform.mode == 1 && !mesh_xform.local && mesh_xform_axis_parallel(mesh_xform.axis))
            panel.set_status(std::string("eje ") + n + " (arista paralela, no se mueve)");
        else
            panel.set_status(std::string("eje ") + n + frame);
    };
    auto mesh_scale_toggle_axis = [&](int ax) {
        const int bit = 1 << ax;
        if (mesh_xform.scale_mask == 7) mesh_xform.scale_mask = bit;
        else {
            mesh_xform.scale_mask ^= bit;
            if (mesh_xform.scale_mask == 0) mesh_xform.scale_mask = 7;
        }
        mesh_xform_status_axis();
    };
    auto mesh_uv_scale_toggle_axis = [&](int ax) {
        const int bit = 1 << ax;
        if (mesh_edit.uv_scale_mask == 3) mesh_edit.uv_scale_mask = bit;
        else {
            mesh_edit.uv_scale_mask ^= bit;
            if (mesh_edit.uv_scale_mask == 0) mesh_edit.uv_scale_mask = 3;
        }
        std::string st = "UV S";
        if (mesh_edit.uv_scale_mask != 3) {
            st += "  ";
            if (mesh_edit.uv_scale_mask & 1) st += 'X';
            if (mesh_edit.uv_scale_mask & 2) st += 'Y';
        }
        panel.set_status(st);
    };
    // Local frame of the selection: Z = area-weighted normal of selected faces (or of the faces using the
    // selected edges), X = longest selected edge projected on that plane, Y = Z × X. Verts only → world.
    auto mesh_sel_basis = [&](float out[3][3]) -> bool {
        auto vp = [&](int i) -> const MeshVert& { return mesh_edit.verts[static_cast<std::size_t>(i)]; };
        auto valid_f = [&](const MeshFace& f) {
            const int n = static_cast<int>(mesh_edit.verts.size());
            return f.i0 >= 0 && f.i1 >= 0 && f.i2 >= 0 && f.i0 < n && f.i1 < n && f.i2 < n;
        };
        float nx = 0.f, ny = 0.f, nz = 0.f;
        float tx = 0.f, ty = 0.f, tz = 0.f, tbest = 0.f;
        auto take_edge = [&](int a, int b) {
            const auto &p = vp(a), &q = vp(b);
            const float ex = q.x - p.x, ey = q.y - p.y, ez = q.z - p.z;
            const float l2 = ex * ex + ey * ey + ez * ez;
            if (l2 > tbest) {
                tbest = l2;
                tx = ex;
                ty = ey;
                tz = ez;
            }
        };
        auto add_normal = [&](const MeshFace& f) {
            const auto &a = vp(f.i0), &b = vp(f.i1), &c = vp(f.i2);
            nx += (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
            ny += (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
            nz += (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        };
        if (!mesh_edit.sel_faces.empty()) {
            for (int fi : mesh_edit.sel_faces) {
                if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                if (!valid_f(f)) continue;
                add_normal(f);
                take_edge(f.i0, f.i1);
                take_edge(f.i1, f.i2);
                take_edge(f.i2, f.i0);
            }
        } else if (!mesh_edit.sel_edges.empty()) {
            std::unordered_set<std::uint64_t> keys;
            for (int ei : mesh_edit.sel_edges) {
                if (ei < 0 || static_cast<std::size_t>(ei) >= mesh_edit.edges.size()) continue;
                const auto& e = mesh_edit.edges[static_cast<std::size_t>(ei)];
                if (e.a < 0 || e.b < 0 || static_cast<std::size_t>(e.a) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(e.b) >= mesh_edit.verts.size())
                    continue;
                take_edge(e.a, e.b);
                keys.insert(mesh_edge_key(e.a, e.b));
            }
            for (const auto& f : mesh_edit.faces) {
                if (!valid_f(f)) continue;
                if (keys.count(mesh_edge_key(f.i0, f.i1)) || keys.count(mesh_edge_key(f.i1, f.i2)) ||
                    keys.count(mesh_edge_key(f.i2, f.i0)))
                    add_normal(f);
            }
        } else {
            return false;
        }
        if (tbest < 1e-12f) return false;
        float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-9f) {
            // Loose edge: keep world Z as "up" unless the edge itself is vertical.
            nx = 0.f;
            ny = 0.f;
            nz = 1.f;
            nl = 1.f;
        }
        nx /= nl;
        ny /= nl;
        nz /= nl;
        const float d = tx * nx + ty * ny + tz * nz;
        tx -= d * nx;
        ty -= d * ny;
        tz -= d * nz;
        float tl = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (tl < 1e-6f) {
            // Edge along the normal: any perpendicular.
            tx = std::fabs(nz) < 0.9f ? -ny : 1.f;
            ty = std::fabs(nz) < 0.9f ? nx : 0.f;
            tz = 0.f;
            tl = std::sqrt(tx * tx + ty * ty + tz * tz);
        }
        tx /= tl;
        ty /= tl;
        tz /= tl;
        out[0][0] = tx;
        out[0][1] = ty;
        out[0][2] = tz;
        out[1][0] = ny * tz - nz * ty;
        out[1][1] = nz * tx - nx * tz;
        out[1][2] = nx * ty - ny * tx;
        out[2][0] = nx;
        out[2][1] = ny;
        out[2][2] = nz;
        return true;
    };
    auto mesh_xform_begin = [&](int mode, bool cap = true) {
        mesh_xform.affected = mesh_collect_affected();
        {
            float b[3][3]{};
            mesh_xform.local = mesh_sel_basis(b);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) mesh_xform.basis[r][c] = mesh_xform.local ? b[r][c] : (r == c ? 1.f : 0.f);
        }
        if (mesh_xform.affected.empty()) {
            panel.set_status("G/R/S: sin selección");
            return;
        }
        mesh_xform.sel_was = mesh_edit.last_event_sel;
        mesh_xform.did_cap = cap;
        mesh_xform.skip_weld = false;
        mesh_xform.uv_lock = false;
        mesh_xform.cube_from = -1;
        if (cap) mesh_geo_cap();
        mesh_xform.active = true;
        mesh_xform.mode = mode;
        mesh_xform.axis = -1;
        mesh_xform.scale_mask = 7;
        mesh_xform.original.clear();
        for (int i : mesh_xform.affected)
            mesh_xform.original.push_back(mesh_edit.verts[static_cast<std::size_t>(i)]);
        float px = 0, py = 0, pz = 0;
        for (int i : mesh_xform.affected) {
            const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
            px += v.x;
            py += v.y;
            pz += v.z;
        }
        const float inv = 1.f / static_cast<float>(mesh_xform.affected.size());
        mesh_xform.pivot_x = px * inv;
        mesh_xform.pivot_y = py * inv;
        mesh_xform.pivot_z = pz * inv;
        float mx = 0, my = 0, fw = 1, fh = 1;
        input::cursor_to_framebuffer(window, mx, my, fw, fh);
        mesh_xform.start_mx = mx;
        mesh_xform.start_my = my;
        mesh_xform.disp_x = mesh_xform.disp_y = mesh_xform.disp_z = 0.f;
        mesh_xform.disp_angle = 0.f;
        mesh_xform.disp_scale = 1.f;
        if (mode == 1 || mode == 2) mesh_xform_status_axis();
    };
    auto mesh_xform_restore = [&]() {
        for (std::size_t i = 0; i < mesh_xform.affected.size() && i < mesh_xform.original.size(); ++i)
            mesh_edit.verts[static_cast<std::size_t>(mesh_xform.affected[i])] = mesh_xform.original[i];
    };
    auto mesh_xform_cancel = [&]() {
        if (!mesh_xform.active) return;
        mesh_xform_restore();
        mesh_xform.active = false;
        if (!mesh_edit.geo_undo.empty()) {
            mesh_apply_snap(mesh_edit.geo_undo.back());
            mesh_edit.geo_undo.pop_back();
        }
        mesh_edit.last_event_sel = mesh_xform.sel_was;
        mesh_xform.did_cap = false;
        mesh_xform.skip_weld = false;
        mesh_xform.uv_lock = false;
        mesh_xform.cube_from = -1;
        mesh_refresh_preview();
        panel.set_status("Transform cancelado");
    };
    auto mesh_xform_tick = [&](float mx, float my, float fw, float fh, const float* wvp) {
        if (!mesh_xform.active) return;
        mesh_xform_restore();
        const float dmx = mx - mesh_xform.start_mx;
        const float dmy = my - mesh_xform.start_my;
        float dx = 0, dy = 0, dz = 0;
        camera.ground_z = mesh_xform.pivot_z;
        float wx0 = mesh_xform.pivot_x, wy0 = mesh_xform.pivot_y;
        float wx1 = mesh_xform.pivot_x, wy1 = mesh_xform.pivot_y;
        if (const auto h0 = camera.hit_ground_xy(mesh_xform.start_mx, mesh_xform.start_my, fw, fh)) {
            wx0 = h0->x;
            wy0 = h0->y;
        }
        if (const auto h1 = camera.hit_ground_xy(mx, my, fw, fh)) {
            wx1 = h1->x;
            wy1 = h1->y;
        }
        dx = wx1 - wx0;
        dy = wy1 - wy0;
        dz = -dmy * 0.02f;
        const float mouse_delta = (std::fabs(dmy) > std::fabs(dmx)) ? -dmy : dmx;
        (void)mouse_delta;
        const bool xform_ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        // snap only while Ctrl is held during the move
        if (mesh_edit.snap_mode > 0 && xform_ctrl && mesh_xform.mode == 0) mesh_src_sync(mx, my, fw, fh, wvp);
        mesh_xform.snap_hit = false;
        if (mesh_xform.mode == 0) {
            if (mesh_xform.local && mesh_xform.axis >= 0) {
                // Blender-style: project the local axis on screen, move by the mouse component along it,
                // so G X is always straight along the face's X even when that is diagonal in world.
                const float* a = mesh_xform.basis[mesh_xform.axis];
                float ex = 0.f, ey = 0.f, ez = 0.f;
                camera.eye_position(ex, ey, ez);
                const float dist = std::sqrt((mesh_xform.pivot_x - ex) * (mesh_xform.pivot_x - ex) +
                                             (mesh_xform.pivot_y - ey) * (mesh_xform.pivot_y - ey) +
                                             (mesh_xform.pivot_z - ez) * (mesh_xform.pivot_z - ez));
                const float L = (std::max)(0.05f, dist * 0.1f);
                float s0x = 0.f, s0y = 0.f, s1x = 0.f, s1y = 0.f;
                float t = 0.f;
                if (world_to_screen(mesh_xform.pivot_x, mesh_xform.pivot_y, mesh_xform.pivot_z, 0, 0, fw, fh, wvp, s0x,
                                    s0y) &&
                    world_to_screen(mesh_xform.pivot_x + a[0] * L, mesh_xform.pivot_y + a[1] * L,
                                    mesh_xform.pivot_z + a[2] * L, 0, 0, fw, fh, wvp, s1x, s1y)) {
                    const float sx = s1x - s0x, sy = s1y - s0y;
                    const float l2 = sx * sx + sy * sy;
                    if (l2 > 1.f) t = (dmx * sx + dmy * sy) / l2 * L;
                    else t = -dmy * 0.02f; // axis points at the camera
                }
                dx = a[0] * t;
                dy = a[1] * t;
                dz = a[2] * t;
            } else if (mesh_xform.axis == 0) {
                dy = dz = 0;
            } else if (mesh_xform.axis == 1) {
                dx = dz = 0;
            } else if (mesh_xform.axis == 2) {
                dx = dy = 0;
                dz = -dmy * 0.02f;
            } else {
                dz = 0;
            }
            float sdx = 0.f, sdy = 0.f, sdz = 0.f;
            mesh_xform.snap_hit = false;
            if (mesh_edit.snap_mode > 0 && xform_ctrl && !mesh_xform.affected.empty()) {
                std::unordered_set<int> aff(mesh_xform.affected.begin(), mesh_xform.affected.end());
                int cursor_i = 0;
                float best_cd = 1.e30f;
                for (std::size_t i = 0; i < mesh_xform.affected.size(); ++i) {
                    const auto& o = mesh_xform.original[i];
                    const float cx = o.x + dx, cy = o.y + dy, cz = o.z + dz;
                    float sx = 0.f, sy = 0.f;
                    if (!world_to_screen(cx, cy, cz, 0, 0, fw, fh, wvp, sx, sy)) continue;
                    const float d = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
                    if (d < best_cd) {
                        best_cd = d;
                        cursor_i = static_cast<int>(i);
                    }
                }
                const auto& co = mesh_xform.original[static_cast<std::size_t>(cursor_i)];
                const float cur_x = co.x + dx, cur_y = co.y + dy, cur_z = co.z + dz;
                mesh_env_sync(mx, my, fw, fh, wvp, cur_x, cur_y, cur_z);
                const auto ray = camera.make_pick_ray(mx, my, fw, fh);
                const float snap_px2 = 24.f * 24.f;
                float best_t = 1.e30f;
                float best_d2 = snap_px2;
                float tx = 0.f, ty = 0.f, tz = 0.f;
                char kind = 'V';
                auto consider_pt = [&](float px, float py, float pz, float d2, char k) -> bool {
                    if (d2 > snap_px2) return false;
                    float t = 1.e30f;
                    if (ray)
                        t = (px - ray->origin.x) * ray->dir.x + (py - ray->origin.y) * ray->dir.y +
                            (pz - ray->origin.z) * ray->dir.z;
                    if (t <= 1e-4f) return false;
                    const bool closer = t < best_t - 1e-5f;
                    const bool tie = std::fabs(t - best_t) < 1e-5f && d2 < best_d2;
                    if (!closer && !tie) return false;
                    best_t = t;
                    best_d2 = d2;
                    tx = px;
                    ty = py;
                    tz = pz;
                    kind = k;
                    mesh_xform.snap_hit = true;
                    mesh_xform.snap_ax = mesh_xform.snap_bx = px;
                    mesh_xform.snap_ay = mesh_xform.snap_by = py;
                    mesh_xform.snap_az = mesh_xform.snap_bz = pz;
                    return true;
                };
                if (mesh_edit.snap_mode == 1) {
                    for (int i = 0; i < static_cast<int>(mesh_edit.verts.size()); ++i) {
                        if (aff.count(i)) continue;
                        const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
                        float sx = 0.f, sy = 0.f;
                        if (!world_to_screen(v.x, v.y, v.z, 0, 0, fw, fh, wvp, sx, sy)) continue;
                        const float d2 = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
                        consider_pt(v.x, v.y, v.z, d2, 'V');
                    }
                    for (int i = 0; i < static_cast<int>(mesh_edit.src_verts.size()); ++i) {
                        const auto& v = mesh_edit.src_verts[static_cast<std::size_t>(i)];
                        float sx = 0.f, sy = 0.f;
                        if (!world_to_screen(v.x, v.y, v.z, 0, 0, fw, fh, wvp, sx, sy)) continue;
                        const float d2 = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
                        consider_pt(v.x, v.y, v.z, d2, 'V');
                    }
                    for (int i = 0; i < static_cast<int>(mesh_edit.env_verts.size()); ++i) {
                        const auto& v = mesh_edit.env_verts[static_cast<std::size_t>(i)];
                        float sx = 0.f, sy = 0.f;
                        if (!world_to_screen(v.x, v.y, v.z, 0, 0, fw, fh, wvp, sx, sy)) continue;
                        const float d2 = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
                        consider_pt(v.x, v.y, v.z, d2, 'V');
                    }
                } else if (mesh_edit.snap_mode == 2) {
                    auto consider_edge = [&](const MeshVert& a, const MeshVert& b) {
                        float sax = 0, say = 0, sbx = 0, sby = 0;
                        if (!world_to_screen(a.x, a.y, a.z, 0, 0, fw, fh, wvp, sax, say)) return;
                        if (!world_to_screen(b.x, b.y, b.z, 0, 0, fw, fh, wvp, sbx, sby)) return;
                        const float den = (std::max)(1.f, (sbx - sax) * (sbx - sax) + (sby - say) * (sby - say));
                        const float t =
                            (std::max)(0.f, (std::min)(1.f, ((mx - sax) * (sbx - sax) + (my - say) * (sby - say)) / den));
                        const float px = sax + t * (sbx - sax);
                        const float py = say + t * (sby - say);
                        const float d2 = (px - mx) * (px - mx) + (py - my) * (py - my);
                        if (consider_pt(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z), d2, 'E')) {
                            mesh_xform.snap_ax = a.x;
                            mesh_xform.snap_ay = a.y;
                            mesh_xform.snap_az = a.z;
                            mesh_xform.snap_bx = b.x;
                            mesh_xform.snap_by = b.y;
                            mesh_xform.snap_bz = b.z;
                        }
                    };
                    for (const auto& e : mesh_edit.edges) {
                        if (e.a < 0 || e.b < 0) continue;
                        if (aff.count(e.a) || aff.count(e.b)) continue;
                        if (static_cast<std::size_t>(e.a) >= mesh_edit.verts.size() ||
                            static_cast<std::size_t>(e.b) >= mesh_edit.verts.size())
                            continue;
                        consider_edge(mesh_edit.verts[static_cast<std::size_t>(e.a)],
                                      mesh_edit.verts[static_cast<std::size_t>(e.b)]);
                    }
                    for (const auto& e : mesh_edit.src_edges) {
                        if (e.a < 0 || e.b < 0) continue;
                        if (static_cast<std::size_t>(e.a) >= mesh_edit.src_verts.size() ||
                            static_cast<std::size_t>(e.b) >= mesh_edit.src_verts.size())
                            continue;
                        consider_edge(mesh_edit.src_verts[static_cast<std::size_t>(e.a)],
                                      mesh_edit.src_verts[static_cast<std::size_t>(e.b)]);
                    }
                    for (const auto& e : mesh_edit.env_edges) {
                        if (e.a < 0 || e.b < 0) continue;
                        if (static_cast<std::size_t>(e.a) >= mesh_edit.env_verts.size() ||
                            static_cast<std::size_t>(e.b) >= mesh_edit.env_verts.size())
                            continue;
                        consider_edge(mesh_edit.env_verts[static_cast<std::size_t>(e.a)],
                                      mesh_edit.env_verts[static_cast<std::size_t>(e.b)]);
                    }
                } else if (mesh_edit.snap_mode == 3 && ray) {
                    auto consider_face = [&](const MeshVert& a, const MeshVert& b, const MeshVert& c) {
                        if (mesh_cross_sq(a, b, c) < 1e-12f) return;
                        float t = 0.f;
                        if (!mesh_ray_tri(ray->origin, ray->dir, a, b, c, t)) return;
                        if (t <= 1e-4f) return;
                        if (t >= best_t - 1e-5f) return;
                        best_t = t;
                        tx = ray->origin.x + ray->dir.x * t;
                        ty = ray->origin.y + ray->dir.y * t;
                        tz = ray->origin.z + ray->dir.z * t;
                        kind = 'F';
                        mesh_xform.snap_hit = true;
                        mesh_xform.snap_c0x = a.x;
                        mesh_xform.snap_c0y = a.y;
                        mesh_xform.snap_c0z = a.z;
                        mesh_xform.snap_c1x = b.x;
                        mesh_xform.snap_c1y = b.y;
                        mesh_xform.snap_c1z = b.z;
                        mesh_xform.snap_c2x = c.x;
                        mesh_xform.snap_c2y = c.y;
                        mesh_xform.snap_c2z = c.z;
                    };
                    for (const auto& f : mesh_edit.faces) {
                        if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
                        if (aff.count(f.i0) || aff.count(f.i1) || aff.count(f.i2)) continue;
                        if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                            static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                            static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                            continue;
                        consider_face(mesh_edit.verts[static_cast<std::size_t>(f.i0)],
                                      mesh_edit.verts[static_cast<std::size_t>(f.i1)],
                                      mesh_edit.verts[static_cast<std::size_t>(f.i2)]);
                    }
                    for (const auto& f : mesh_edit.src_tris) {
                        if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
                        if (static_cast<std::size_t>(f.i0) >= mesh_edit.src_verts.size() ||
                            static_cast<std::size_t>(f.i1) >= mesh_edit.src_verts.size() ||
                            static_cast<std::size_t>(f.i2) >= mesh_edit.src_verts.size())
                            continue;
                        consider_face(mesh_edit.src_verts[static_cast<std::size_t>(f.i0)],
                                      mesh_edit.src_verts[static_cast<std::size_t>(f.i1)],
                                      mesh_edit.src_verts[static_cast<std::size_t>(f.i2)]);
                    }
                    for (const auto& f : mesh_edit.env_tris) {
                        if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
                        if (static_cast<std::size_t>(f.i0) >= mesh_edit.env_verts.size() ||
                            static_cast<std::size_t>(f.i1) >= mesh_edit.env_verts.size() ||
                            static_cast<std::size_t>(f.i2) >= mesh_edit.env_verts.size())
                            continue;
                        consider_face(mesh_edit.env_verts[static_cast<std::size_t>(f.i0)],
                                      mesh_edit.env_verts[static_cast<std::size_t>(f.i1)],
                                      mesh_edit.env_verts[static_cast<std::size_t>(f.i2)]);
                    }
                    {
                        std::vector<std::size_t> excl;
                        auto push_excl = [&](std::size_t c) {
                            if (c != static_cast<std::size_t>(-1)) excl.push_back(c);
                        };
                        push_excl(mesh_edit.preview_chunk);
                        for (auto c : mesh_edit.preview_chunks) push_excl(c);
                        push_excl(mesh_edit.sel_overlay_chunk);
                        push_excl(mesh_edit.edge_overlay_chunk);
                        float st = 0.f;
                        if (renderer.pick_surface_hit_z(ray->origin.x, ray->origin.y, ray->origin.z, ray->dir.x,
                                                        ray->dir.y, ray->dir.z, excl.empty() ? nullptr : excl.data(),
                                                        excl.size(), &st) &&
                            st > 1e-4f && st < best_t - 1e-5f) {
                            best_t = st;
                            tx = ray->origin.x + ray->dir.x * st;
                            ty = ray->origin.y + ray->dir.y * st;
                            tz = ray->origin.z + ray->dir.z * st;
                            kind = 'F';
                            mesh_xform.snap_hit = true;
                            mesh_xform.snap_c0x = mesh_xform.snap_c1x = mesh_xform.snap_c2x = tx;
                            mesh_xform.snap_c0y = mesh_xform.snap_c1y = mesh_xform.snap_c2y = ty;
                            mesh_xform.snap_c0z = mesh_xform.snap_c1z = mesh_xform.snap_c2z = tz;
                        }
                    }
                }
                if (mesh_xform.snap_hit) {
                    sdx = tx - cur_x;
                    sdy = ty - cur_y;
                    sdz = tz - cur_z;
                    mesh_xform.snap_kind = kind;
                    mesh_xform.snap_tx = tx;
                    mesh_xform.snap_ty = ty;
                    mesh_xform.snap_tz = tz;
                    if (mesh_xform.local && mesh_xform.axis >= 0) {
                        const float* a = mesh_xform.basis[mesh_xform.axis];
                        const float k = sdx * a[0] + sdy * a[1] + sdz * a[2];
                        sdx = a[0] * k;
                        sdy = a[1] * k;
                        sdz = a[2] * k;
                    } else if (mesh_xform.axis == 0) {
                        sdy = sdz = 0;
                    } else if (mesh_xform.axis == 1) {
                        sdx = sdz = 0;
                    } else if (mesh_xform.axis == 2) {
                        sdx = sdy = 0;
                    }
                }
            }
            dx += sdx;
            dy += sdy;
            dz += sdz;
            mesh_xform.disp_x = dx;
            mesh_xform.disp_y = dy;
            mesh_xform.disp_z = dz;
            for (std::size_t i = 0; i < mesh_xform.affected.size(); ++i) {
                auto& v = mesh_edit.verts[static_cast<std::size_t>(mesh_xform.affected[i])];
                v.x = mesh_xform.original[i].x + dx;
                v.y = mesh_xform.original[i].y + dy;
                v.z = mesh_xform.original[i].z + dz;
            }
        } else if (mesh_xform.mode == 1) {
            float psx = 0.f, psy = 0.f;
            world_to_screen(mesh_xform.pivot_x, mesh_xform.pivot_y, mesh_xform.pivot_z, 0, 0, fw, fh, wvp, psx, psy);
            float a0 = std::atan2(mesh_xform.start_my - psy, mesh_xform.start_mx - psx);
            float a1 = std::atan2(my - psy, mx - psx);
            float ang = a1 - a0;
            const float pi = 3.14159265f;
            while (ang > pi) ang -= 2.f * pi;
            while (ang < -pi) ang += 2.f * pi;
            if (xform_ctrl) {
                const float q = pi * 0.5f;
                ang = std::round(ang / q) * q;
            }
            mesh_xform.disp_angle = ang * (180.f / pi);
            float kx = 0.f, ky = 0.f, kz = 1.f;
            if (mesh_xform.local && mesh_xform.axis >= 0) {
                kx = mesh_xform.basis[mesh_xform.axis][0];
                ky = mesh_xform.basis[mesh_xform.axis][1];
                kz = mesh_xform.basis[mesh_xform.axis][2];
            } else if (mesh_xform.axis == 0) {
                kx = 1.f;
                ky = kz = 0.f;
            } else if (mesh_xform.axis == 1) {
                ky = 1.f;
                kx = kz = 0.f;
            } else if (mesh_xform.axis == 2) {
                kz = 1.f;
                kx = ky = 0.f;
            } else {
                float ex = 0.f, ey = 0.f, ez = 0.f;
                camera.eye_position(ex, ey, ez);
                kx = mesh_xform.pivot_x - ex;
                ky = mesh_xform.pivot_y - ey;
                kz = mesh_xform.pivot_z - ez;
                const float kn = std::sqrt(kx * kx + ky * ky + kz * kz);
                if (kn > 1e-6f) {
                    kx /= kn;
                    ky /= kn;
                    kz /= kn;
                } else {
                    kx = ky = 0.f;
                    kz = 1.f;
                }
            }
            const float c = std::cos(ang), s = std::sin(ang), ic = 1.f - c;
            for (std::size_t i = 0; i < mesh_xform.affected.size(); ++i) {
                const auto& o = mesh_xform.original[i];
                const float rx = o.x - mesh_xform.pivot_x, ry = o.y - mesh_xform.pivot_y, rz = o.z - mesh_xform.pivot_z;
                const float cxv = ky * rz - kz * ry, cyv = kz * rx - kx * rz, czv = kx * ry - ky * rx;
                const float dot = kx * rx + ky * ry + kz * rz;
                auto& v = mesh_edit.verts[static_cast<std::size_t>(mesh_xform.affected[i])];
                v.x = mesh_xform.pivot_x + rx * c + cxv * s + kx * dot * ic;
                v.y = mesh_xform.pivot_y + ry * c + cyv * s + ky * dot * ic;
                v.z = mesh_xform.pivot_z + rz * c + czv * s + kz * dot * ic;
            }
        } else if (mesh_xform.mode == 2) {
            float psx = 0.f, psy = 0.f;
            world_to_screen(mesh_xform.pivot_x, mesh_xform.pivot_y, mesh_xform.pivot_z, 0, 0, fw, fh, wvp, psx, psy);
            float d0 = std::hypot(mesh_xform.start_mx - psx, mesh_xform.start_my - psy);
            if (d0 < 16.f) d0 = 16.f;
            const float d1 = std::hypot(mx - psx, my - psy);
            float sc = d1 / d0;
            if (xform_ctrl) sc = std::round(sc * 10.f) / 10.f;
            sc = (std::max)(0.01f, sc);
            mesh_xform.disp_scale = sc;
            const int sm = mesh_xform.scale_mask;
            for (std::size_t i = 0; i < mesh_xform.affected.size(); ++i) {
                const auto& o = mesh_xform.original[i];
                float rx = o.x - mesh_xform.pivot_x, ry = o.y - mesh_xform.pivot_y, rz = o.z - mesh_xform.pivot_z;
                if (mesh_xform.local && sm != 7) {
                    float nrx = 0.f, nry = 0.f, nrz = 0.f;
                    for (int k = 0; k < 3; ++k) {
                        const float* a = mesh_xform.basis[k];
                        float c = rx * a[0] + ry * a[1] + rz * a[2];
                        if (sm & (1 << k)) c *= sc;
                        nrx += a[0] * c;
                        nry += a[1] * c;
                        nrz += a[2] * c;
                    }
                    rx = nrx;
                    ry = nry;
                    rz = nrz;
                } else {
                    if (sm & 1) rx *= sc;
                    if (sm & 2) ry *= sc;
                    if (sm & 4) rz *= sc;
                }
                auto& v = mesh_edit.verts[static_cast<std::size_t>(mesh_xform.affected[i])];
                v.x = mesh_xform.pivot_x + rx;
                v.y = mesh_xform.pivot_y + ry;
                v.z = mesh_xform.pivot_z + rz;
            }
        } else if (mesh_xform.mode == 3) {
            float psx = 0.f, psy = 0.f;
            world_to_screen(mesh_xform.pivot_x, mesh_xform.pivot_y, mesh_xform.pivot_z, 0, 0, fw, fh, wvp, psx, psy);
            float tx = psx - mesh_xform.start_mx, ty = psy - mesh_xform.start_my;
            const float tlen = std::hypot(tx, ty);
            float signed_px = std::hypot(mx - mesh_xform.start_mx, my - mesh_xform.start_my);
            if (tlen > 1.f)
                signed_px = ((mx - mesh_xform.start_mx) * tx + (my - mesh_xform.start_my) * ty) / tlen;
            float ex = 0.f, ey = 0.f, ez = 0.f;
            camera.eye_position(ex, ey, ez);
            const float dist = std::sqrt((mesh_xform.pivot_x - ex) * (mesh_xform.pivot_x - ex) +
                                         (mesh_xform.pivot_y - ey) * (mesh_xform.pivot_y - ey) +
                                         (mesh_xform.pivot_z - ez) * (mesh_xform.pivot_z - ez));
            float thick = signed_px * (std::max)(0.0005f, dist * 0.0012f);
            if (thick < 0.f) thick = 0.f;
            if (xform_ctrl) thick = std::round(thick / 0.05f) * 0.05f;
            if (thick > 25.f) thick = 25.f;
            mesh_xform.inset_thick = thick;
            mesh_xform.disp_x = thick;
            const std::size_t n = (std::min)(mesh_xform.affected.size(), mesh_xform.inx.size());
            for (std::size_t i = 0; i < n; ++i) {
                auto& v = mesh_edit.verts[static_cast<std::size_t>(mesh_xform.affected[i])];
                const auto& o = mesh_xform.original[i];
                v.x = o.x + mesh_xform.inx[i] * thick;
                v.y = o.y + mesh_xform.iny[i] * thick;
                v.z = o.z + mesh_xform.inz[i] * thick;
            }
        }
        mesh_refresh_preview();
    };
    auto mesh_xform_confirm = [&]() {
        if (!mesh_xform.active) return;
        bool moved = false;
        for (std::size_t i = 0; i < mesh_xform.affected.size() && i < mesh_xform.original.size(); ++i) {
            const auto& v = mesh_edit.verts[static_cast<std::size_t>(mesh_xform.affected[i])];
            const auto& o = mesh_xform.original[i];
            const float dx = v.x - o.x, dy = v.y - o.y, dz = v.z - o.z;
            if (dx * dx + dy * dy + dz * dz > 1e-10f) {
                moved = true;
                break;
            }
        }
        if (mesh_xform.cube_from >= 0) {
            for (int fi = mesh_xform.cube_from; fi < static_cast<int>(mesh_edit.faces.size()); ++fi)
                mesh_orient_face_to_camera(mesh_edit.faces[static_cast<std::size_t>(fi)]);
        }
        const bool uv_locked = mesh_xform.uv_lock;
        if (moved && !mesh_xform.skip_weld && !uv_locked) mesh_weld_all();
        if (mesh_xform.did_cap && mesh_xform.cube_from < 0 && !moved && !mesh_edit.geo_undo.empty()) {
            mesh_edit.geo_undo.pop_back();
            mesh_edit.last_event_sel = mesh_xform.sel_was;
        }
        mesh_xform.active = false;
        mesh_xform.did_cap = false;
        mesh_xform.skip_weld = false;
        mesh_xform.uv_lock = false;
        mesh_xform.cube_from = -1;
        mesh_debug_invariants();
        mesh_refresh_preview();
        panel.set_status(moved ? (uv_locked ? "GG OK (UV fijo)" : "Transform OK") : "Transform (sin cambio)");
    };
    auto mesh_duplicate = [&](bool linked) {
        if (mesh_edit.sel_verts.empty() && mesh_edit.sel_edges.empty() && mesh_edit.sel_faces.empty()) {
            panel.set_status("Duplicate: sin selección");
            return;
        }
        mesh_geo_cap();
        std::unordered_set<int> need = mesh_edit.sel_verts;
        for (int ei : mesh_edit.sel_edges) {
            if (ei < 0 || static_cast<std::size_t>(ei) >= mesh_edit.edges.size()) continue;
            need.insert(mesh_edit.edges[static_cast<std::size_t>(ei)].a);
            need.insert(mesh_edit.edges[static_cast<std::size_t>(ei)].b);
        }
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            need.insert(f.i0);
            need.insert(f.i1);
            need.insert(f.i2);
        }
        std::unordered_map<int, int> vmap;
        if (linked) {
            for (int v : need) vmap[v] = v;
        } else {
            for (int v : need) {
                MeshVert nv = mesh_edit.verts[static_cast<std::size_t>(v)];
                vmap[v] = static_cast<int>(mesh_edit.verts.size());
                mesh_edit.verts.push_back(nv);
            }
        }
        std::vector<int> nedges, nfaces, nverts;
        if (!linked) {
            for (int v : need) nverts.push_back(vmap[v]);
        }
        for (int ei : mesh_edit.sel_edges) {
            MeshEdge ne = mesh_edit.edges[static_cast<std::size_t>(ei)];
            ne.a = vmap[ne.a];
            ne.b = vmap[ne.b];
            nedges.push_back(static_cast<int>(mesh_edit.edges.size()));
            mesh_edit.edges.push_back(ne);
        }
        for (int fi : mesh_edit.sel_faces) {
            MeshFace nf = mesh_edit.faces[static_cast<std::size_t>(fi)];
            nf.i0 = vmap[nf.i0];
            nf.i1 = vmap[nf.i1];
            nf.i2 = vmap[nf.i2];
            nfaces.push_back(static_cast<int>(mesh_edit.faces.size()));
            mesh_edit.faces.push_back(nf);
            mesh_ensure_edge(nf.i0, nf.i1);
            mesh_ensure_edge(nf.i1, nf.i2);
            mesh_ensure_edge(nf.i2, nf.i0);
            nedges.push_back(mesh_find_edge_i(nf.i0, nf.i1));
            nedges.push_back(mesh_find_edge_i(nf.i1, nf.i2));
            nedges.push_back(mesh_find_edge_i(nf.i2, nf.i0));
        }
        mesh_edit.sel_verts.clear();
        mesh_edit.sel_edges.clear();
        mesh_edit.sel_faces.clear();
        for (int i : nverts) mesh_edit.sel_verts.insert(i);
        for (int i : nedges)
            if (i >= 0) mesh_edit.sel_edges.insert(i);
        for (int i : nfaces) mesh_edit.sel_faces.insert(i);
        mesh_debug_invariants();
        mesh_refresh_preview();
        if (linked) {
            mesh_xform_begin(0, false);
            mesh_xform.skip_weld = true;
        } else
            panel.set_status("Alt+D caras solapadas");
    };
    auto mesh_copy_face_uv = [&](MeshFace& dst, int c, float u, float v) {
        mesh_uv_set(dst, c, u, v);
    };
    auto mesh_split_face_plane = [&](const MeshFace& f, float nx, float ny, float nz, float w,
                                     std::vector<MeshFace>& out,
                                     std::unordered_map<std::uint64_t, int>& edge_cuts) {
        const int id[3] = {f.i0, f.i1, f.i2};
        float uu[3], vv[3], d[3];
        int s[3];
        for (int i = 0; i < 3; ++i) {
            if (id[i] < 0 || static_cast<std::size_t>(id[i]) >= mesh_edit.verts.size()) {
                out.push_back(f);
                return;
            }
            mesh_uv_get(f, i, uu[i], vv[i]);
            const auto& p = mesh_edit.verts[static_cast<std::size_t>(id[i])];
            d[i] = nx * p.x + ny * p.y + nz * p.z - w;
            s[i] = d[i] > 1e-5f ? 1 : (d[i] < -1e-5f ? -1 : 0);
        }
        int npos = 0, nneg = 0;
        for (int i = 0; i < 3; ++i) {
            if (s[i] > 0) ++npos;
            if (s[i] < 0) ++nneg;
        }
        if (npos == 0 || nneg == 0) {
            out.push_back(f);
            return;
        }
        auto cut_ab = [&](int a, int b, float& u, float& v) -> int {
            const int ia = id[a], ib = id[b];
            const auto ek = mesh_edge_key(ia, ib);
            if (const auto it = edge_cuts.find(ek); it != edge_cuts.end()) {
                const float t = d[a] / (d[a] - d[b]);
                const float tt = (std::max)(0.f, (std::min)(1.f, t));
                u = uu[a] + tt * (uu[b] - uu[a]);
                v = vv[a] + tt * (vv[b] - vv[a]);
                return it->second;
            }
            const float t = d[a] / (d[a] - d[b]);
            const float tt = (std::max)(0.f, (std::min)(1.f, t));
            const auto& pa = mesh_edit.verts[static_cast<std::size_t>(ia)];
            const auto& pb = mesh_edit.verts[static_cast<std::size_t>(ib)];
            u = uu[a] + tt * (uu[b] - uu[a]);
            v = vv[a] + tt * (vv[b] - vv[a]);
            const int vi = mesh_intern_xyz(pa.x + tt * (pb.x - pa.x), pa.y + tt * (pb.y - pa.y),
                                           pa.z + tt * (pb.z - pa.z));
            edge_cuts[ek] = vi;
            return vi;
        };
        auto emit = [&](int i0, int i1, int i2, float u0, float v0, float u1, float v1, float u2, float v2) {
            if (i0 == i1 || i1 == i2 || i0 == i2) return;
            MeshFace nf = f;
            nf.i0 = i0;
            nf.i1 = i1;
            nf.i2 = i2;
            mesh_copy_face_uv(nf, 0, u0, v0);
            mesh_copy_face_uv(nf, 1, u1, v1);
            mesh_copy_face_uv(nf, 2, u2, v2);
            out.push_back(nf);
            mesh_ensure_edge(i0, i1);
            mesh_ensure_edge(i1, i2);
            mesh_ensure_edge(i2, i0);
        };
        int lone = -1;
        for (int i = 0; i < 3; ++i) {
            int cnt = 0;
            for (int j = 0; j < 3; ++j)
                if (s[j] == s[i] && s[i] != 0) ++cnt;
            if (s[i] != 0 && cnt == 1) {
                lone = i;
                break;
            }
        }
        if (lone >= 0) {
            const int b = (lone + 1) % 3, c = (lone + 2) % 3;
            float uab, vab, uac, vac;
            const int pab = cut_ab(lone, b, uab, vab);
            const int pac = cut_ab(lone, c, uac, vac);
            emit(id[lone], pab, pac, uu[lone], vv[lone], uab, vab, uac, vac);
            emit(pab, id[b], id[c], uab, vab, uu[b], vv[b], uu[c], vv[c]);
            emit(id[b], id[c], pac, uu[b], vv[b], uu[c], vv[c], uac, vac);
            return;
        }
        int zp = -1;
        for (int i = 0; i < 3; ++i)
            if (s[i] == 0) zp = i;
        if (zp >= 0) {
            const int b = (zp + 1) % 3, c = (zp + 2) % 3;
            if (s[b] * s[c] < 0) {
                float uh, vh;
                const int h = cut_ab(b, c, uh, vh);
                emit(id[zp], id[b], h, uu[zp], vv[zp], uu[b], vv[b], uh, vh);
                emit(id[zp], h, id[c], uu[zp], vv[zp], uh, vh, uu[c], vv[c]);
                return;
            }
        }
        out.push_back(f);
    };
    auto mesh_split_all_plane = [&](float nx, float ny, float nz, float w, const std::unordered_set<int>* only,
                                    std::unordered_map<std::uint64_t, int>* edge_cuts = nullptr) {
        std::unordered_map<std::uint64_t, int> local_cuts;
        std::unordered_map<std::uint64_t, int>& cuts = edge_cuts ? *edge_cuts : local_cuts;
        std::vector<MeshFace> nf;
        nf.reserve(mesh_edit.faces.size() + 8);
        for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
            if (only && !only->count(i)) {
                nf.push_back(mesh_edit.faces[static_cast<std::size_t>(i)]);
                continue;
            }
            mesh_split_face_plane(mesh_edit.faces[static_cast<std::size_t>(i)], nx, ny, nz, w, nf, cuts);
        }
        mesh_edit.faces = std::move(nf);
    };
    auto mesh_cut_targets = [&]() -> std::unordered_set<int> {
        std::unordered_set<int> t = mesh_edit.sel_faces;
        if (t.empty() && mesh_edit.hover_f >= 0) t.insert(mesh_edit.hover_f);
        if (t.empty()) {
            for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) t.insert(i);
        }
        return t;
    };
    auto mesh_cut_frame = [&](float& cx, float& cy, float& cz, float& hx, float& hy, float& hz, float& hs,
                              float& vx, float& vy, float& vz, float& vs, float& smin, float& smax) -> bool {
        const auto tgt = mesh_cut_targets();
        if (tgt.empty()) return false;
        float nx = 0.f, ny = 0.f, nz = 0.f;
        cx = cy = cz = 0.f;
        int nv = 0;
        std::unordered_set<int> used;
        for (int fi : tgt) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            const int id[3] = {f.i0, f.i1, f.i2};
            const MeshVert* pv[3]{};
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                if (id[k] < 0 || static_cast<std::size_t>(id[k]) >= mesh_edit.verts.size()) {
                    ok = false;
                    break;
                }
                pv[k] = &mesh_edit.verts[static_cast<std::size_t>(id[k])];
            }
            if (!ok) continue;
            const float ax = pv[1]->x - pv[0]->x, ay = pv[1]->y - pv[0]->y, az = pv[1]->z - pv[0]->z;
            const float bx = pv[2]->x - pv[0]->x, by = pv[2]->y - pv[0]->y, bz = pv[2]->z - pv[0]->z;
            nx += ay * bz - az * by;
            ny += az * bx - ax * bz;
            nz += ax * by - ay * bx;
            for (int k = 0; k < 3; ++k) {
                if (used.insert(id[k]).second) {
                    cx += pv[k]->x;
                    cy += pv[k]->y;
                    cz += pv[k]->z;
                    ++nv;
                }
            }
        }
        if (nv <= 0) return false;
        cx /= static_cast<float>(nv);
        cy /= static_cast<float>(nv);
        cz /= static_cast<float>(nv);
        float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nlen < 1e-8f) {
            nx = 0.f;
            ny = 0.f;
            nz = 1.f;
            nlen = 1.f;
        }
        nx /= nlen;
        ny /= nlen;
        nz /= nlen;
        hx = -ny;
        hy = nx;
        hz = 0.f;
        float hlen = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (hlen < 1e-4f) {
            hx = 1.f;
            hy = 0.f;
            hz = 0.f;
            hlen = 1.f;
        }
        hx /= hlen;
        hy /= hlen;
        hz /= hlen;
        vx = ny * hz - nz * hy;
        vy = nz * hx - nx * hz;
        vz = nx * hy - ny * hx;
        float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (vlen < 1e-8f) {
            vx = 0.f;
            vy = 0.f;
            vz = 1.f;
            vlen = 1.f;
        }
        vx /= vlen;
        vy /= vlen;
        vz /= vlen;
        float hmin = 1.e30f, hmax = -1.e30f, vmin = 1.e30f, vmax = -1.e30f;
        for (int vi : used) {
            const auto& p = mesh_edit.verts[static_cast<std::size_t>(vi)];
            const float hp = (p.x - cx) * hx + (p.y - cy) * hy + (p.z - cz) * hz;
            const float vp = (p.x - cx) * vx + (p.y - cy) * vy + (p.z - cz) * vz;
            hmin = (std::min)(hmin, hp);
            hmax = (std::max)(hmax, hp);
            vmin = (std::min)(vmin, vp);
            vmax = (std::max)(vmax, vp);
        }
        hs = hmax - hmin;
        vs = vmax - vmin;
        cx += hx * (hmin + hmax) * 0.5f;
        cy += hy * (hmin + hmax) * 0.5f;
        cz += hz * (hmin + hmax) * 0.5f;
        cx += vx * (vmin + vmax) * 0.5f;
        cy += vy * (vmin + vmax) * 0.5f;
        cz += vz * (vmin + vmax) * 0.5f;
        if (mesh_edit.cut_hv == 0) {
            smin = vmin;
            smax = vmax;
        } else {
            smin = hmin;
            smax = hmax;
        }
        return hs > 1e-5f || vs > 1e-5f;
    };
    auto mesh_loopcut_cancel = [&]() {
        mesh_edit.loopcut_on = false;
    };
    auto mesh_knife_cancel = [&]() {
        mesh_edit.knife_on = false;
        mesh_edit.knife_axis = -1;
        mesh_edit.knife_pts.clear();
    };
    auto mesh_hit_bake = [&](float mx, float my, float fw, float fh, float& hx, float& hy, float& hz) -> bool {
        const auto ray = camera.make_pick_ray(mx, my, fw, fh);
        if (!ray) return false;
        float best = 1.e30f;
        bool hit = false;
        for (const auto& f : mesh_edit.faces) {
            if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
            if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                continue;
            float t = 0.f;
            if (!mesh_ray_tri(ray->origin, ray->dir, mesh_edit.verts[static_cast<std::size_t>(f.i0)],
                              mesh_edit.verts[static_cast<std::size_t>(f.i1)],
                              mesh_edit.verts[static_cast<std::size_t>(f.i2)], t))
                continue;
            if (t < best) {
                best = t;
                hx = ray->origin.x + ray->dir.x * t;
                hy = ray->origin.y + ray->dir.y * t;
                hz = ray->origin.z + ray->dir.z * t;
                hit = true;
            }
        }
        return hit;
    };
    // Blender loop cut (editmesh_loopcut + bmo_subdivide_edgering): tri pairs become quads, the
    // edge ring is walked through opposite edges, both ring edges split at the same factors and
    // every new strip re-triangulated. Triangles end the ring with a fan to their apex.
    struct LcQuad {
        int v[4] = {-1, -1, -1, -1};
        float u[4] = {}, w[4] = {};
        float ub[4] = {}, wb[4] = {}; // corner UVs as seen by face fb (v0, v2, v3), for edges touching v3
        int fa = -1, fb = -1;
        int n = 4;
    };
    struct LcItem {
        int q = -1;
        int s = -1, e = -1, s2 = -1, e2 = -1;
        bool fwd = true;
    };
    auto mesh_lc_build_quads = [&](std::vector<LcQuad>& quads, std::vector<int>& face_q) {
        quads.clear();
        const int nf = static_cast<int>(mesh_edit.faces.size());
        face_q.assign(static_cast<std::size_t>(nf), -1);
        auto vp = [&](int i) -> const MeshVert& { return mesh_edit.verts[static_cast<std::size_t>(i)]; };
        auto d2 = [&](int a, int b) {
            const auto &p = vp(a), &q = vp(b);
            const float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
            return dx * dx + dy * dy + dz * dz;
        };
        auto face_ok = [&](const MeshFace& f) {
            const int n = static_cast<int>(mesh_edit.verts.size());
            return f.i0 >= 0 && f.i1 >= 0 && f.i2 >= 0 && f.i0 < n && f.i1 < n && f.i2 < n && f.i0 != f.i1 &&
                   f.i1 != f.i2 && f.i0 != f.i2;
        };
        auto normal = [&](const MeshFace& f, float& nx, float& ny, float& nz) {
            const auto &a = vp(f.i0), &b = vp(f.i1), &c = vp(f.i2);
            const float ax = b.x - a.x, ay = b.y - a.y, az = b.z - a.z;
            const float bx = c.x - a.x, by = c.y - a.y, bz = c.z - a.z;
            nx = ay * bz - az * by;
            ny = az * bx - ax * bz;
            nz = ax * by - ay * bx;
            const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (l > 1e-12f) {
                nx /= l;
                ny /= l;
                nz /= l;
            }
        };
        std::unordered_map<std::uint64_t, std::vector<int>> by_edge;
        for (int fi = 0; fi < nf; ++fi) {
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            if (!face_ok(f)) continue;
            by_edge[mesh_edge_key(f.i0, f.i1)].push_back(fi);
            by_edge[mesh_edge_key(f.i1, f.i2)].push_back(fi);
            by_edge[mesh_edge_key(f.i2, f.i0)].push_back(fi);
        }
        struct Cand {
            float score;
            int fa, ca, fb;
        };
        std::vector<Cand> cands;
        for (int fi = 0; fi < nf; ++fi) {
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            if (!face_ok(f)) continue;
            const int id[3] = {f.i0, f.i1, f.i2};
            float el[3];
            for (int c = 0; c < 3; ++c) el[c] = d2(id[c], id[(c + 1) % 3]);
            for (int c = 0; c < 3; ++c) {
                const auto it = by_edge.find(mesh_edge_key(id[c], id[(c + 1) % 3]));
                if (it == by_edge.end() || it->second.size() != 2) continue;
                const int fj = it->second[0] == fi ? it->second[1] : it->second[0];
                if (fj <= fi) continue;
                const auto& g = mesh_edit.faces[static_cast<std::size_t>(fj)];
                // Blender bmo_join_triangles defaults: 40° face angle, no UV/material compare,
                // winding-agnostic, quad must be convex.
                float n0x, n0y, n0z, n1x, n1y, n1z;
                normal(f, n0x, n0y, n0z);
                normal(g, n1x, n1y, n1z);
                const float ndot = std::fabs(n0x * n1x + n0y * n1y + n0z * n1z);
                if (ndot < 0.766f) continue;
                const int gid[3] = {g.i0, g.i1, g.i2};
                int q_opp = -1;
                for (int m = 0; m < 3; ++m)
                    if (gid[m] != id[c] && gid[m] != id[(c + 1) % 3]) q_opp = gid[m];
                if (q_opp < 0) continue;
                {
                    const auto &s0 = vp(id[c]), &s1 = vp(id[(c + 1) % 3]), &p = vp(id[(c + 2) % 3]),
                               &qv = vp(q_opp);
                    const float ex = s1.x - s0.x, ey = s1.y - s0.y, ez = s1.z - s0.z;
                    auto crs = [&](const MeshVert& t, float& cx, float& cy, float& cz) {
                        const float tx = t.x - s0.x, ty = t.y - s0.y, tz = t.z - s0.z;
                        cx = ey * tz - ez * ty;
                        cy = ez * tx - ex * tz;
                        cz = ex * ty - ey * tx;
                    };
                    float ax, ay, az, bx, by, bz;
                    crs(p, ax, ay, az);
                    crs(qv, bx, by, bz);
                    if (ax * bx + ay * by + az * bz >= 0.f) continue; // p, q same side: not convex
                    // Diagonal p-q must also separate s0 and s1.
                    const float fx = qv.x - p.x, fy = qv.y - p.y, fz = qv.z - p.z;
                    auto crs2 = [&](const MeshVert& t, float& cx, float& cy, float& cz) {
                        const float tx = t.x - p.x, ty = t.y - p.y, tz = t.z - p.z;
                        cx = fy * tz - fz * ty;
                        cy = fz * tx - fx * tz;
                        cz = fx * ty - fy * tx;
                    };
                    crs2(s0, ax, ay, az);
                    crs2(s1, bx, by, bz);
                    if (ax * bx + ay * by + az * bz >= 0.f) continue;
                }
                float gl_max = 0.f;
                for (int k = 0; k < 3; ++k) gl_max = (std::max)(gl_max, d2(gid[k], gid[(k + 1) % 3]));
                const float fl_max = (std::max)((std::max)(el[0], el[1]), el[2]);
                const float sh = el[c];
                const float sc = (std::min)(sh / (std::max)(fl_max, 1e-12f), sh / (std::max)(gl_max, 1e-12f));
                if (sc < 0.5f) continue;
                cands.push_back(Cand{sc + ndot + (g.mat == f.mat ? 0.5f : 0.f), fi, c, fj});
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
        for (const auto& cd : cands) {
            if (face_q[static_cast<std::size_t>(cd.fa)] >= 0 || face_q[static_cast<std::size_t>(cd.fb)] >= 0)
                continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(cd.fa)];
            const auto& g = mesh_edit.faces[static_cast<std::size_t>(cd.fb)];
            const int c0 = cd.ca, c1 = (cd.ca + 1) % 3, cp = (cd.ca + 2) % 3;
            const int s0 = mesh_face_vi(f, c0), s1 = mesh_face_vi(f, c1);
            int cq = -1;
            for (int m = 0; m < 3; ++m) {
                const int vi = mesh_face_vi(g, m);
                if (vi != s0 && vi != s1) cq = m;
            }
            if (cq < 0) continue;
            LcQuad q;
            q.fa = cd.fa;
            q.fb = cd.fb;
            q.v[0] = s1;
            mesh_uv_get(f, c1, q.u[0], q.w[0]);
            q.v[1] = mesh_face_vi(f, cp);
            mesh_uv_get(f, cp, q.u[1], q.w[1]);
            q.v[2] = s0;
            mesh_uv_get(f, c0, q.u[2], q.w[2]);
            q.v[3] = mesh_face_vi(g, cq);
            mesh_uv_get(g, cq, q.u[3], q.w[3]);
            q.ub[3] = q.u[3];
            q.wb[3] = q.w[3];
            for (int m = 0; m < 3; ++m) {
                const int vi = mesh_face_vi(g, m);
                if (vi == s1) mesh_uv_get(g, m, q.ub[0], q.wb[0]);
                if (vi == s0) mesh_uv_get(g, m, q.ub[2], q.wb[2]);
            }
            face_q[static_cast<std::size_t>(cd.fa)] = face_q[static_cast<std::size_t>(cd.fb)] =
                static_cast<int>(quads.size());
            quads.push_back(q);
        }
        for (int fi = 0; fi < nf; ++fi) {
            if (face_q[static_cast<std::size_t>(fi)] >= 0) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            if (!face_ok(f)) continue;
            LcQuad q;
            q.n = 3;
            q.fa = fi;
            for (int c = 0; c < 3; ++c) {
                q.v[c] = mesh_face_vi(f, c);
                mesh_uv_get(f, c, q.u[c], q.w[c]);
            }
            face_q[static_cast<std::size_t>(fi)] = static_cast<int>(quads.size());
            quads.push_back(q);
        }
    };
    auto mesh_lc_ring = [&](const std::vector<LcQuad>& quads, int seed_q, int s, int e) -> std::vector<LcItem> {
        std::vector<LcItem> out;
        if (seed_q < 0 || static_cast<std::size_t>(seed_q) >= quads.size()) return out;
        std::unordered_map<std::uint64_t, std::vector<int>> q_by_edge;
        for (int qi = 0; qi < static_cast<int>(quads.size()); ++qi) {
            const auto& q = quads[static_cast<std::size_t>(qi)];
            for (int i = 0; i < q.n; ++i) q_by_edge[mesh_edge_key(q.v[i], q.v[(i + 1) % q.n])].push_back(qi);
        }
        const auto& sel = mesh_edit.sel_faces;
        auto allowed = [&](int qi) {
            if (sel.empty()) return true;
            const auto& q = quads[static_cast<std::size_t>(qi)];
            return sel.count(q.fa) != 0 || (q.fb >= 0 && sel.count(q.fb) != 0);
        };
        auto make = [&](int qi, int a, int b, LcItem& it) -> bool {
            const auto& q = quads[static_cast<std::size_t>(qi)];
            int i = -1;
            for (int k = 0; k < q.n; ++k)
                if (q.v[k] == a) i = k;
            if (i < 0) return false;
            it.q = qi;
            it.s = a;
            it.e = b;
            if (q.v[(i + 1) % q.n] == b) it.fwd = true;
            else if (q.v[(i + q.n - 1) % q.n] == b) it.fwd = false;
            else return false;
            if (q.n == 3) {
                it.s2 = it.e2 = q.v[(i + (it.fwd ? 2 : 1)) % 3];
            } else if (it.fwd) {
                it.s2 = q.v[(i + 3) % 4];
                it.e2 = q.v[(i + 2) % 4];
            } else {
                it.s2 = q.v[(i + 1) % 4];
                it.e2 = q.v[(i + 2) % 4];
            }
            return true;
        };
        std::unordered_set<int> seen;
        LcItem first;
        if (!make(seed_q, s, e, first)) return out;
        out.push_back(first);
        seen.insert(seed_q);
        auto walk = [&](int from_q, int a, int b, bool front) {
            for (;;) {
                const auto it = q_by_edge.find(mesh_edge_key(a, b));
                if (it == q_by_edge.end()) return;
                int nq = -1;
                for (int qi : it->second)
                    if (qi != from_q) nq = qi;
                if (nq < 0 || seen.count(nq) || !allowed(nq)) return;
                LcItem li;
                if (!make(nq, a, b, li)) return;
                seen.insert(nq);
                if (front) out.push_back(li);
                else out.insert(out.begin(), li);
                if (quads[static_cast<std::size_t>(nq)].n == 3) return;
                from_q = nq;
                a = li.s2;
                b = li.e2;
            }
        };
        if (quads[static_cast<std::size_t>(seed_q)].n == 4) walk(seed_q, first.s2, first.e2, true);
        walk(seed_q, first.s, first.e, false);
        return out;
    };
    auto mesh_lc_ts = [&]() {
        const int n = (std::max)(1, (std::min)(mesh_edit.cut_n, 16));
        std::vector<float> ts;
        for (int k = 0; k < n; ++k) {
            float t = mesh_edit.cut_frac;
            if (n > 1) t = mesh_edit.cut_frac + (static_cast<float>(k) - (static_cast<float>(n) - 1.f) * 0.5f) /
                                                    static_cast<float>(n + 1);
            ts.push_back((std::clamp)(t, 0.02f, 0.98f));
        }
        std::sort(ts.begin(), ts.end());
        ts.erase(std::unique(ts.begin(), ts.end(), [](float a, float b) { return std::fabs(a - b) < 1e-4f; }),
                 ts.end());
        return ts;
    };
    auto mesh_lc_lerp = [&](int a, int b, float t, float& x, float& y, float& z) {
        const auto& p = mesh_edit.verts[static_cast<std::size_t>(a)];
        const auto& q = mesh_edit.verts[static_cast<std::size_t>(b)];
        x = p.x + t * (q.x - p.x);
        y = p.y + t * (q.y - p.y);
        z = p.z + t * (q.z - p.z);
    };
    auto mesh_loopcut_pick = [&](float mx, float my, float fw, float fh, const float* wvp) {
        int fi = -1;
        if (const auto ray = camera.make_pick_ray(mx, my, fw, fh)) {
            float best_t = 1.e30f;
            for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
                const int n = static_cast<int>(mesh_edit.verts.size());
                if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0 || f.i0 >= n || f.i1 >= n || f.i2 >= n) continue;
                float t = 0.f;
                if (!mesh_ray_tri(ray->origin, ray->dir, mesh_edit.verts[static_cast<std::size_t>(f.i0)],
                                  mesh_edit.verts[static_cast<std::size_t>(f.i1)],
                                  mesh_edit.verts[static_cast<std::size_t>(f.i2)], t))
                    continue;
                if (t < best_t) {
                    best_t = t;
                    fi = i;
                }
            }
        }
        if (fi < 0) return;
        std::vector<LcQuad> quads;
        std::vector<int> face_q;
        mesh_lc_build_quads(quads, face_q);
        const int qi = face_q[static_cast<std::size_t>(fi)];
        if (qi < 0) return;
        const auto& q = quads[static_cast<std::size_t>(qi)];
        float best = 1.e30f, best_t = 0.5f;
        int bs = -1, be = -1;
        for (int i = 0; i < q.n; ++i) {
            const int a = q.v[i], b = q.v[(i + 1) % q.n];
            const auto &pa = mesh_edit.verts[static_cast<std::size_t>(a)], &pb = mesh_edit.verts[static_cast<std::size_t>(b)];
            float ax, ay, bx, by;
            if (!world_to_screen(pa.x, pa.y, pa.z, 0, 0, fw, fh, wvp, ax, ay)) continue;
            if (!world_to_screen(pb.x, pb.y, pb.z, 0, 0, fw, fh, wvp, bx, by)) continue;
            const float dx = bx - ax, dy = by - ay;
            const float l2 = (std::max)(1.f, dx * dx + dy * dy);
            const float t = (std::clamp)(((mx - ax) * dx + (my - ay) * dy) / l2, 0.f, 1.f);
            const float px = ax + t * dx - mx, py = ay + t * dy - my;
            const float d = px * px + py * py;
            if (d < best) {
                best = d;
                best_t = t;
                bs = a;
                be = b;
            }
        }
        if (bs < 0) return;
        mesh_edit.lc_face = fi;
        mesh_edit.lc_s = bs;
        mesh_edit.lc_e = be;
        mesh_edit.cut_frac = (std::clamp)(best_t, 0.02f, 0.98f);
    };
    auto mesh_loopcut_apply = [&]() {
        std::vector<LcQuad> quads;
        std::vector<int> face_q;
        mesh_lc_build_quads(quads, face_q);
        const int fi = mesh_edit.lc_face;
        if (fi < 0 || static_cast<std::size_t>(fi) >= face_q.size() || face_q[static_cast<std::size_t>(fi)] < 0) {
            panel.set_status("Loop cut: poné el mouse sobre una cara");
            return;
        }
        const auto ring = mesh_lc_ring(quads, face_q[static_cast<std::size_t>(fi)], mesh_edit.lc_s, mesh_edit.lc_e);
        if (ring.empty()) {
            panel.set_status("Loop cut: sin anillo");
            return;
        }
        mesh_geo_cap();
        const auto ts = mesh_lc_ts();
        std::unordered_set<std::uint64_t> old_face_edges;
        for (const auto& f : mesh_edit.faces) {
            old_face_edges.insert(mesh_edge_key(f.i0, f.i1));
            old_face_edges.insert(mesh_edge_key(f.i1, f.i2));
            old_face_edges.insert(mesh_edge_key(f.i2, f.i0));
        }
        std::unordered_set<int> drop;
        std::vector<MeshFace> add;
        auto uv_of = [&](const LcQuad& q, int vi, float& u, float& w) {
            for (int k = 0; k < q.n; ++k)
                if (q.v[k] == vi) {
                    u = q.u[k];
                    w = q.w[k];
                    return;
                }
            u = w = 0.f;
        };
        struct P {
            int vi;
            float u, w;
        };
        auto uv_of_b = [&](const LcQuad& q, int vi, float& u, float& w) {
            for (int k = 0; k < 4; ++k)
                if (q.v[k] == vi) {
                    u = q.ub[k];
                    w = q.wb[k];
                    return;
                }
            u = w = 0.f;
        };
        auto edge_pts = [&](const LcQuad& q, int a, int b) {
            std::vector<P> pts;
            float ua, wa, ub, wb;
            if (q.n == 4 && (a == q.v[3] || b == q.v[3])) {
                uv_of_b(q, a, ua, wa);
                uv_of_b(q, b, ub, wb);
            } else {
                uv_of(q, a, ua, wa);
                uv_of(q, b, ub, wb);
            }
            pts.push_back(P{a, ua, wa});
            for (float t : ts) {
                float x, y, z;
                mesh_lc_lerp(a, b, t, x, y, z);
                pts.push_back(P{mesh_intern_xyz(x, y, z), ua + t * (ub - ua), wa + t * (wb - wa)});
            }
            pts.push_back(P{b, ub, wb});
            return pts;
        };
        auto tri = [&](const MeshFace& tpl, const P& a, const P& b, const P& c) {
            if (a.vi == b.vi || b.vi == c.vi || a.vi == c.vi) return;
            MeshFace g = tpl;
            g.i0 = a.vi;
            g.i1 = b.vi;
            g.i2 = c.vi;
            mesh_uv_set(g, 0, a.u, a.w);
            mesh_uv_set(g, 1, b.u, b.w);
            mesh_uv_set(g, 2, c.u, c.w);
            add.push_back(g);
        };
        auto len2 = [&](int a, int b) {
            const auto& p = mesh_edit.verts[static_cast<std::size_t>(a)];
            const auto& q = mesh_edit.verts[static_cast<std::size_t>(b)];
            const float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
            return dx * dx + dy * dy + dz * dz;
        };
        for (const auto& it : ring) {
            const auto& q = quads[static_cast<std::size_t>(it.q)];
            const MeshFace tpl = mesh_edit.faces[static_cast<std::size_t>(q.fa)];
            drop.insert(q.fa);
            if (q.fb >= 0) drop.insert(q.fb);
            const auto L = edge_pts(q, it.s, it.e);
            if (q.n == 3) {
                float ua, wa;
                uv_of(q, it.s2, ua, wa);
                const P apex{it.s2, ua, wa};
                for (std::size_t j = 0; j + 1 < L.size(); ++j) {
                    if (it.fwd) tri(tpl, L[j], L[j + 1], apex);
                    else tri(tpl, L[j + 1], L[j], apex);
                }
                continue;
            }
            const auto R = edge_pts(q, it.s2, it.e2);
            for (std::size_t j = 0; j + 1 < L.size(); ++j) {
                P c[4];
                if (it.fwd) {
                    c[0] = L[j];
                    c[1] = L[j + 1];
                    c[2] = R[j + 1];
                    c[3] = R[j];
                } else {
                    c[0] = L[j];
                    c[1] = R[j];
                    c[2] = R[j + 1];
                    c[3] = L[j + 1];
                }
                if (len2(c[0].vi, c[2].vi) <= len2(c[1].vi, c[3].vi)) {
                    tri(tpl, c[0], c[1], c[2]);
                    tri(tpl, c[0], c[2], c[3]);
                } else {
                    tri(tpl, c[0], c[1], c[3]);
                    tri(tpl, c[1], c[2], c[3]);
                }
            }
        }
        std::vector<MeshFace> nf;
        nf.reserve(mesh_edit.faces.size() + add.size());
        for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i)
            if (!drop.count(i)) nf.push_back(mesh_edit.faces[static_cast<std::size_t>(i)]);
        for (auto& g : add) nf.push_back(g);
        mesh_edit.faces = std::move(nf);
        std::vector<MeshEdge> ne;
        std::unordered_set<std::uint64_t> seen;
        for (const auto& e : mesh_edit.edges) {
            const auto k = mesh_edge_key(e.a, e.b);
            if (old_face_edges.count(k) || !seen.insert(k).second) continue;
            ne.push_back(e);
        }
        for (const auto& f : mesh_edit.faces) {
            const int es[3][2] = {{f.i0, f.i1}, {f.i1, f.i2}, {f.i2, f.i0}};
            for (const auto& ab : es)
                if (seen.insert(mesh_edge_key(ab[0], ab[1])).second) ne.push_back(MeshEdge{ab[0], ab[1]});
        }
        mesh_edit.edges = std::move(ne);
        mesh_edit.sel_edges.clear();
        mesh_edit.sel_faces.clear();
        mesh_edit.sel_uv.clear();
        mesh_edit.last_edge = mesh_edit.last_face = -1;
        mesh_edit.hover_e = mesh_edit.hover_f = -1;
        mesh_edit.loopcut_on = false;
        mesh_debug_invariants();
        mesh_refresh_preview();
        panel.set_status("Loop cut x" + std::to_string(ts.size()) + "  quads " + std::to_string(ring.size()));
    };
    auto mesh_knife_lock = [&](MeshVert& q) {
        if (mesh_edit.knife_axis < 0 || mesh_edit.knife_pts.empty()) return;
        const auto& p = mesh_edit.knife_pts.back();
        if (mesh_edit.knife_axis == 0) {
            q.y = p.y;
            q.z = p.z;
        } else if (mesh_edit.knife_axis == 1) {
            q.x = p.x;
            q.z = p.z;
        } else {
            q.x = p.x;
            q.y = p.y;
        }
    };
    auto mesh_knife_cut_seg = [&](const MeshVert& a, const MeshVert& b) {
        float dx = b.x - a.x, dy = b.y - a.y;
        float nx = -dy, ny = dx, nz = 0.f;
        float n2 = nx * nx + ny * ny;
        if (n2 < 1e-10f) {
            const float fx = camera.target_x - a.x, fy = camera.target_y - a.y;
            nx = -fy;
            ny = fx;
            n2 = nx * nx + ny * ny;
            if (n2 < 1e-10f) {
                nx = 1.f;
                ny = 0.f;
                n2 = 1.f;
            }
        }
        const float inv = 1.f / std::sqrt(n2);
        nx *= inv;
        ny *= inv;
        const float w = nx * a.x + ny * a.y + nz * a.z;
        mesh_split_all_plane(nx, ny, nz, w, nullptr);
    };
    auto mesh_knife_confirm = [&]() {
        if (mesh_edit.knife_pts.size() < 2) {
            panel.set_status("Knife: 2 clicks");
            return;
        }
        mesh_geo_cap();
        for (std::size_t i = 1; i < mesh_edit.knife_pts.size(); ++i)
            mesh_knife_cut_seg(mesh_edit.knife_pts[i - 1], mesh_edit.knife_pts[i]);
        mesh_knife_cancel();
        mesh_debug_invariants();
        mesh_refresh_preview();
        panel.set_status("Knife OK");
    };
    auto mesh_knife_click = [&](float mx, float my, float fw, float fh) {
        MeshVert q{};
        if (!mesh_hit_bake(mx, my, fw, fh, q.x, q.y, q.z)) {
            panel.set_status("Knife: sin hit");
            return;
        }
        mesh_knife_lock(q);
        mesh_edit.knife_pts.push_back(q);
        panel.set_status("Knife pts " + std::to_string(mesh_edit.knife_pts.size()) + "  Enter corta  Esc cancela");
    };
    auto mesh_subdivide_sel = [&]() {
        const int cuts = (std::max)(1, (std::min)(mesh_edit.subdiv_n, 8));
        if (mesh_edit.pick == MeshEdit::Pick::Edges) {
            if (mesh_edit.sel_edges.empty()) {
                panel.set_status("Subdividir: edges");
                return;
            }
            mesh_geo_cap();
            std::unordered_map<std::uint64_t, std::vector<int>> chain;
            for (int ei : mesh_edit.sel_edges) {
                if (ei < 0 || static_cast<std::size_t>(ei) >= mesh_edit.edges.size()) continue;
                const auto e = mesh_edit.edges[static_cast<std::size_t>(ei)];
                if (e.a == e.b) continue;
                std::vector<int> pts;
                pts.push_back(e.a);
                const auto& pa = mesh_edit.verts[static_cast<std::size_t>(e.a)];
                const auto& pb = mesh_edit.verts[static_cast<std::size_t>(e.b)];
                for (int k = 1; k <= cuts; ++k) {
                    const float t = static_cast<float>(k) / static_cast<float>(cuts + 1);
                    pts.push_back(mesh_intern_xyz(pa.x + t * (pb.x - pa.x), pa.y + t * (pb.y - pa.y),
                                                   pa.z + t * (pb.z - pa.z)));
                }
                pts.push_back(e.b);
                chain[mesh_edge_key(e.a, e.b)] = pts;
                for (std::size_t i = 1; i < pts.size(); ++i) mesh_ensure_edge(pts[i - 1], pts[i]);
            }
            std::vector<MeshFace> nf;
            for (const auto& f : mesh_edit.faces) {
                const int ed[3][2] = {{f.i0, f.i1}, {f.i1, f.i2}, {f.i2, f.i0}};
                int hit = -1;
                for (int e = 0; e < 3; ++e)
                    if (chain.count(mesh_edge_key(ed[e][0], ed[e][1]))) {
                        hit = e;
                        break;
                    }
                if (hit < 0) {
                    nf.push_back(f);
                    continue;
                }
                auto pts = chain[mesh_edge_key(ed[hit][0], ed[hit][1])];
                if (pts.front() != ed[hit][0]) std::reverse(pts.begin(), pts.end());
                const int opp = ed[(hit + 2) % 3][0] == ed[hit][0] || ed[(hit + 2) % 3][0] == ed[hit][1]
                                    ? ed[(hit + 2) % 3][1]
                                    : ed[(hit + 2) % 3][0];
                float uo, vo, ua, va, ub, vb;
                const int ca = hit, cb = (hit + 1) % 3, copp = (hit + 2) % 3;
                mesh_uv_get(f, copp, uo, vo);
                mesh_uv_get(f, ca, ua, va);
                mesh_uv_get(f, cb, ub, vb);
                for (std::size_t i = 1; i < pts.size(); ++i) {
                    MeshFace g = f;
                    const float t0 = static_cast<float>(i - 1) / static_cast<float>(pts.size() - 1);
                    const float t1 = static_cast<float>(i) / static_cast<float>(pts.size() - 1);
                    g.i0 = pts[i - 1];
                    g.i1 = pts[i];
                    g.i2 = opp;
                    mesh_uv_set(g, 0, ua + t0 * (ub - ua), va + t0 * (vb - va));
                    mesh_uv_set(g, 1, ua + t1 * (ub - ua), va + t1 * (vb - va));
                    mesh_uv_set(g, 2, uo, vo);
                    nf.push_back(g);
                    mesh_ensure_edge(g.i0, g.i1);
                    mesh_ensure_edge(g.i1, g.i2);
                    mesh_ensure_edge(g.i2, g.i0);
                }
            }
            mesh_edit.faces = std::move(nf);
            mesh_edit.sel_edges.clear();
            mesh_debug_invariants();
            mesh_refresh_preview();
            panel.set_status("Subdividir edges x" + std::to_string(cuts));
            return;
        }
        if (mesh_edit.sel_faces.empty()) {
            panel.set_status("Subdividir: faces");
            return;
        }
        mesh_geo_cap();
        auto mid_edge = [&](int a, int b, float ua, float va, float ub, float vb, float& u, float& v) -> int {
            const auto& pa = mesh_edit.verts[static_cast<std::size_t>(a)];
            const auto& pb = mesh_edit.verts[static_cast<std::size_t>(b)];
            u = 0.5f * (ua + ub);
            v = 0.5f * (va + vb);
            return mesh_intern_xyz(0.5f * (pa.x + pb.x), 0.5f * (pa.y + pb.y), 0.5f * (pa.z + pb.z));
        };
        for (int pass = 0; pass < cuts; ++pass) {
            std::vector<char> mark(mesh_edit.faces.size(), 0);
            for (int fi : mesh_edit.sel_faces)
                if (fi >= 0 && static_cast<std::size_t>(fi) < mark.size()) mark[static_cast<std::size_t>(fi)] = 1;
            std::vector<MeshFace> nf;
            std::unordered_set<int> nsel;
            for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
                if (!mark[static_cast<std::size_t>(i)]) {
                    nf.push_back(f);
                    continue;
                }
                float u0, v0, u1, v1, u2, v2, u01, v01, u12, v12, u20, v20;
                mesh_uv_get(f, 0, u0, v0);
                mesh_uv_get(f, 1, u1, v1);
                mesh_uv_get(f, 2, u2, v2);
                const int m01 = mid_edge(f.i0, f.i1, u0, v0, u1, v1, u01, v01);
                const int m12 = mid_edge(f.i1, f.i2, u1, v1, u2, v2, u12, v12);
                const int m20 = mid_edge(f.i2, f.i0, u2, v2, u0, v0, u20, v20);
                auto tri = [&](int a, int b, int c, float ua, float va, float ub, float vb, float uc, float vc) {
                    MeshFace g = f;
                    g.i0 = a;
                    g.i1 = b;
                    g.i2 = c;
                    mesh_uv_set(g, 0, ua, va);
                    mesh_uv_set(g, 1, ub, vb);
                    mesh_uv_set(g, 2, uc, vc);
                    nsel.insert(static_cast<int>(nf.size()));
                    nf.push_back(g);
                    mesh_ensure_edge(a, b);
                    mesh_ensure_edge(b, c);
                    mesh_ensure_edge(c, a);
                };
                tri(f.i0, m01, m20, u0, v0, u01, v01, u20, v20);
                tri(m01, f.i1, m12, u01, v01, u1, v1, u12, v12);
                tri(m20, m12, f.i2, u20, v20, u12, v12, u2, v2);
                tri(m01, m12, m20, u01, v01, u12, v12, u20, v20);
            }
            mesh_edit.faces = std::move(nf);
            mesh_edit.sel_faces = std::move(nsel);
        }
        mesh_debug_invariants();
        mesh_refresh_preview();
        panel.set_status("Subdividir faces x" + std::to_string(cuts));
    };
    auto mesh_compact_unused = [&]() {
        const int n = static_cast<int>(mesh_edit.verts.size());
        std::vector<char> use(static_cast<std::size_t>(n), 0);
        std::vector<MeshFace> nf;
        for (const auto& f : mesh_edit.faces) {
            if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
            if (f.i0 == f.i1 || f.i1 == f.i2 || f.i0 == f.i2) continue;
            if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                continue;
            nf.push_back(f);
            use[static_cast<std::size_t>(f.i0)] = 1;
            use[static_cast<std::size_t>(f.i1)] = 1;
            use[static_cast<std::size_t>(f.i2)] = 1;
        }
        mesh_edit.faces = std::move(nf);
        std::vector<int> map(static_cast<std::size_t>(n), -1);
        std::vector<MeshVert> nv;
        for (int i = 0; i < n; ++i) {
            if (!use[static_cast<std::size_t>(i)]) continue;
            map[static_cast<std::size_t>(i)] = static_cast<int>(nv.size());
            nv.push_back(mesh_edit.verts[static_cast<std::size_t>(i)]);
        }
        mesh_edit.verts = std::move(nv);
        auto rm = [&](int i) { return (i >= 0 && static_cast<std::size_t>(i) < map.size()) ? map[static_cast<std::size_t>(i)] : -1; };
        for (auto& f : mesh_edit.faces) {
            f.i0 = rm(f.i0);
            f.i1 = rm(f.i1);
            f.i2 = rm(f.i2);
        }
        std::vector<MeshEdge> ne;
        std::unordered_set<std::uint64_t> seen;
        for (const auto& e : mesh_edit.edges) {
            const int a = rm(e.a), b = rm(e.b);
            if (a < 0 || b < 0 || a == b) continue;
            const auto k = mesh_edge_key(a, b);
            if (!seen.insert(k).second) continue;
            ne.push_back(MeshEdge{a, b});
        }
        for (const auto& f : mesh_edit.faces) {
            const int es[3][2] = {{f.i0, f.i1}, {f.i1, f.i2}, {f.i2, f.i0}};
            for (auto& ab : es) {
                if (ab[0] < 0 || ab[1] < 0) continue;
                const auto k = mesh_edge_key(ab[0], ab[1]);
                if (!seen.insert(k).second) continue;
                ne.push_back(MeshEdge{ab[0], ab[1]});
            }
        }
        mesh_edit.edges = std::move(ne);
        mesh_edit.sel_verts.clear();
        mesh_edit.sel_edges.clear();
        mesh_edit.sel_faces.clear();
        mesh_edit.sel_uv.clear();
        mesh_edit.last_vert = mesh_edit.last_edge = mesh_edit.last_face = -1;
    };
    auto mesh_split_sel = [&]() {
        std::unordered_set<int> need = mesh_edit.sel_verts;
        for (int ei : mesh_edit.sel_edges) {
            if (ei < 0 || static_cast<std::size_t>(ei) >= mesh_edit.edges.size()) continue;
            need.insert(mesh_edit.edges[static_cast<std::size_t>(ei)].a);
            need.insert(mesh_edit.edges[static_cast<std::size_t>(ei)].b);
        }
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            need.insert(f.i0);
            need.insert(f.i1);
            need.insert(f.i2);
        }
        if (need.empty()) {
            panel.set_status("Y: seleccioná V/E/F");
            return;
        }
        mesh_geo_cap();
        std::unordered_set<int> unsel_v;
        for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
            if (mesh_edit.sel_faces.count(fi)) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            unsel_v.insert(f.i0);
            unsel_v.insert(f.i1);
            unsel_v.insert(f.i2);
        }
        for (int ei = 0; ei < static_cast<int>(mesh_edit.edges.size()); ++ei) {
            if (mesh_edit.sel_edges.count(ei)) continue;
            const auto& e = mesh_edit.edges[static_cast<std::size_t>(ei)];
            if (!need.count(e.a) || !need.count(e.b)) {
                unsel_v.insert(e.a);
                unsel_v.insert(e.b);
            }
        }
        std::unordered_map<int, int> vmap;
        auto split_v = [&](int v) -> int {
            if (v < 0 || static_cast<std::size_t>(v) >= mesh_edit.verts.size()) return v;
            const auto it = vmap.find(v);
            if (it != vmap.end()) return it->second;
            if (!need.count(v) || !unsel_v.count(v)) {
                vmap[v] = v;
                return v;
            }
            MeshVert nv = mesh_edit.verts[static_cast<std::size_t>(v)];
            const int ni = static_cast<int>(mesh_edit.verts.size());
            mesh_edit.verts.push_back(nv);
            vmap[v] = ni;
            return ni;
        };
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            f.i0 = split_v(f.i0);
            f.i1 = split_v(f.i1);
            f.i2 = split_v(f.i2);
            mesh_ensure_edge(f.i0, f.i1);
            mesh_ensure_edge(f.i1, f.i2);
            mesh_ensure_edge(f.i2, f.i0);
        }
        for (int ei : mesh_edit.sel_edges) {
            if (ei < 0 || static_cast<std::size_t>(ei) >= mesh_edit.edges.size()) continue;
            auto& e = mesh_edit.edges[static_cast<std::size_t>(ei)];
            e.a = split_v(e.a);
            e.b = split_v(e.b);
        }
        mesh_edit.sel_verts.clear();
        for (int v : need) {
            const auto it = vmap.find(v);
            mesh_edit.sel_verts.insert(it == vmap.end() ? v : it->second);
        }
        mesh_debug_invariants();
        mesh_refresh_preview();
        mesh_xform_begin(0, false);
        mesh_xform.skip_weld = true;
        panel.set_status("Y: separado  G  LMB confirma");
    };
    auto mesh_extrude_region = [&]() {
        if (!mesh_edit.sel_faces.empty()) {
            std::unordered_set<int> sv;
            for (int fi : mesh_edit.sel_faces) {
                if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                sv.insert(f.i0);
                sv.insert(f.i1);
                sv.insert(f.i2);
            }
            if (sv.empty()) {
                panel.set_status("Extrude: sin sel");
                return;
            }
            mesh_geo_cap();
            std::unordered_map<int, int> vmap;
            std::vector<int> nverts;
            for (int v : sv) {
                if (v < 0 || static_cast<std::size_t>(v) >= mesh_edit.verts.size()) continue;
                MeshVert nv = mesh_edit.verts[static_cast<std::size_t>(v)];
                vmap[v] = static_cast<int>(mesh_edit.verts.size());
                nverts.push_back(vmap[v]);
                mesh_edit.verts.push_back(nv);
            }
            std::vector<int> nfaces;
            std::unordered_map<std::uint64_t, int> ecount;
            auto ek = [](int a, int b) {
                const int lo = (std::min)(a, b), hi = (std::max)(a, b);
                return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint32_t>(hi);
            };
            for (int fi : mesh_edit.sel_faces) {
                if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
                auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                ecount[ek(f.i0, f.i1)]++;
                ecount[ek(f.i1, f.i2)]++;
                ecount[ek(f.i2, f.i0)]++;
                f.i0 = vmap[f.i0];
                f.i1 = vmap[f.i1];
                f.i2 = vmap[f.i2];
                nfaces.push_back(fi);
            }
            const int cube_from = static_cast<int>(mesh_edit.faces.size());
            for (auto& [k, c] : ecount) {
                if (c != 1) continue;
                const int a = static_cast<int>(k >> 32);
                const int b = static_cast<int>(k & 0xffffffffu);
                if (!vmap.count(a) || !vmap.count(b)) continue;
                const int na = vmap[a], nb = vmap[b];
                mesh_edit.faces.push_back(mesh_new_face(a, b, nb, mesh_edit.tex_sel));
                mesh_edit.faces.push_back(mesh_new_face(a, nb, na, mesh_edit.tex_sel));
                mesh_orient_face_to_camera(mesh_edit.faces[mesh_edit.faces.size() - 2]);
                mesh_orient_face_to_camera(mesh_edit.faces.back());
                mesh_ensure_edge(a, b);
                mesh_ensure_edge(b, nb);
                mesh_ensure_edge(nb, na);
                mesh_ensure_edge(na, a);
            }
            mesh_edit.sel_verts.clear();
            mesh_edit.sel_edges.clear();
            mesh_edit.sel_faces.clear();
            for (int v : nverts) mesh_edit.sel_verts.insert(v);
            for (int f : nfaces) mesh_edit.sel_faces.insert(f);
            mesh_debug_invariants();
            mesh_refresh_preview();
            mesh_xform_begin(0, false);
            mesh_xform.cube_from = cube_from;
            return;
        }
        if (mesh_edit.sel_edges.empty()) {
            panel.set_status("Extrude: sin sel");
            return;
        }
        mesh_geo_cap();
        std::unordered_map<int, int> vmap;
        auto new_vert = [&](int old) -> int {
            const auto it = vmap.find(old);
            if (it != vmap.end()) return it->second;
            if (old < 0 || static_cast<std::size_t>(old) >= mesh_edit.verts.size()) return -1;
            MeshVert nv = mesh_edit.verts[static_cast<std::size_t>(old)];
            const int ni = static_cast<int>(mesh_edit.verts.size());
            mesh_edit.verts.push_back(nv);
            vmap[old] = ni;
            return ni;
        };
        std::vector<int> nedges;
        const int cube_from = static_cast<int>(mesh_edit.faces.size());
        for (int ei : mesh_edit.sel_edges) {
            if (ei < 0 || static_cast<std::size_t>(ei) >= mesh_edit.edges.size()) continue;
            const auto e = mesh_edit.edges[static_cast<std::size_t>(ei)];
            const int na = new_vert(e.a);
            const int nb = new_vert(e.b);
            if (na < 0 || nb < 0) continue;
            mesh_edit.faces.push_back(mesh_new_face(e.a, e.b, nb, mesh_edit.tex_sel));
            mesh_edit.faces.push_back(mesh_new_face(e.a, nb, na, mesh_edit.tex_sel));
            mesh_orient_face_to_camera(mesh_edit.faces[mesh_edit.faces.size() - 2]);
            mesh_orient_face_to_camera(mesh_edit.faces.back());
            mesh_ensure_edge(e.a, e.b);
            mesh_ensure_edge(e.b, nb);
            mesh_ensure_edge(nb, na);
            mesh_ensure_edge(na, e.a);
            nedges.push_back(mesh_ensure_edge(na, nb));
        }
        mesh_edit.sel_verts.clear();
        mesh_edit.sel_edges.clear();
        mesh_edit.sel_faces.clear();
        for (int e : nedges)
            if (e >= 0) mesh_edit.sel_edges.insert(e);
        mesh_debug_invariants();
        mesh_refresh_preview();
        mesh_xform_begin(0, false);
        mesh_xform.cube_from = cube_from;
    };
    auto mesh_inset = [&]() {
        if (mesh_edit.sel_faces.empty()) {
            panel.set_status("Inset: caras");
            return;
        }
        std::unordered_map<std::uint64_t, int> edge_count;
        std::unordered_map<std::uint64_t, std::pair<int, int>> edge_ab;
        auto add_e = [&](int a, int b) {
            if (a == b || a < 0 || b < 0) return;
            const auto k = mesh_edge_key(a, b);
            edge_count[k] += 1;
            edge_ab[k] = {a, b};
        };
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            add_e(f.i0, f.i1);
            add_e(f.i1, f.i2);
            add_e(f.i2, f.i0);
        }
        std::vector<std::pair<int, int>> border;
        std::unordered_map<int, std::vector<int>> badj;
        auto add_bn = [&](int a, int b) {
            auto& na = badj[a];
            if (std::find(na.begin(), na.end(), b) == na.end()) na.push_back(b);
            auto& nb = badj[b];
            if (std::find(nb.begin(), nb.end(), a) == nb.end()) nb.push_back(a);
        };
        for (const auto& kv : edge_count) {
            if (kv.second != 1) continue;
            const auto ab = edge_ab[kv.first];
            border.push_back(ab);
            add_bn(ab.first, ab.second);
        }
        for (const auto& kv : badj) {
            if (static_cast<int>(kv.second.size()) > 2) {
                panel.set_status("Inset: grado de borde > 2");
                log::info("Inset: reason=borde_deg3");
                return;
            }
        }
        mesh_geo_cap();
        float cx = 0.f, cy = 0.f, cz = 0.f;
        float uc = 0.f, vc = 0.f;
        int ncorn = 0;
        std::unordered_set<int> sel_vs;
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            sel_vs.insert(f.i0);
            sel_vs.insert(f.i1);
            sel_vs.insert(f.i2);
            uc += f.u0 + f.u1 + f.u2;
            vc += f.v0 + f.v1 + f.v2;
            ncorn += 3;
        }
        if (sel_vs.empty()) {
            panel.set_status("Inset: caras");
            return;
        }
        for (int vi : sel_vs) {
            const auto& p = mesh_edit.verts[static_cast<std::size_t>(vi)];
            cx += p.x;
            cy += p.y;
            cz += p.z;
        }
        const float invv = 1.f / static_cast<float>(sel_vs.size());
        cx *= invv;
        cy *= invv;
        cz *= invv;
        if (ncorn > 0) {
            uc /= static_cast<float>(ncorn);
            vc /= static_cast<float>(ncorn);
        }
        auto nrm3 = [](float& x, float& y, float& z) {
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len < 1e-8f) {
                x = y = 0.f;
                z = 1.f;
                return 0.f;
            }
            x /= len;
            y /= len;
            z /= len;
            return len;
        };
        std::unordered_map<int, int> inner_of;
        mesh_xform.inx.clear();
        mesh_xform.iny.clear();
        mesh_xform.inz.clear();
        mesh_xform.affected.clear();
        mesh_xform.original.clear();
        for (const auto& kv : badj) {
            const int vi = kv.first;
            const auto& ns = kv.second;
            const auto& p = mesh_edit.verts[static_cast<std::size_t>(vi)];
            float ix = 0.f, iy = 0.f, iz = 0.f;
            float half_ang = 1.5707963f;
            if (ns.size() == 2) {
                const auto& n0 = mesh_edit.verts[static_cast<std::size_t>(ns[0])];
                const auto& n1 = mesh_edit.verts[static_cast<std::size_t>(ns[1])];
                float d0x = n0.x - p.x, d0y = n0.y - p.y, d0z = n0.z - p.z;
                float d1x = n1.x - p.x, d1y = n1.y - p.y, d1z = n1.z - p.z;
                nrm3(d0x, d0y, d0z);
                nrm3(d1x, d1y, d1z);
                ix = d0x + d1x;
                iy = d0y + d1y;
                iz = d0z + d1z;
                nrm3(ix, iy, iz);
                ix = -ix;
                iy = -iy;
                iz = -iz;
                const float cdot = (std::max)(-1.f, (std::min)(1.f, d0x * d1x + d0y * d1y + d0z * d1z));
                half_ang = 0.5f * std::acos(cdot);
            } else if (ns.size() == 1) {
                const auto& n0 = mesh_edit.verts[static_cast<std::size_t>(ns[0])];
                float ex = n0.x - p.x, ey = n0.y - p.y, ez = n0.z - p.z;
                nrm3(ex, ey, ez);
                float tx = cx - p.x, ty = cy - p.y, tz = cz - p.z;
                const float along = tx * ex + ty * ey + tz * ez;
                ix = tx - along * ex;
                iy = ty - along * ey;
                iz = tz - along * ez;
                nrm3(ix, iy, iz);
                half_ang = 1.5707963f;
            } else {
                ix = cx - p.x;
                iy = cy - p.y;
                iz = cz - p.z;
                nrm3(ix, iy, iz);
            }
            if (ix * (cx - p.x) + iy * (cy - p.y) + iz * (cz - p.z) < 0.f) {
                ix = -ix;
                iy = -iy;
                iz = -iz;
            }
            const float s = std::sin((std::max)(0.05f, half_ang));
            const float sc = 1.f / s;
            inner_of[vi] = static_cast<int>(mesh_edit.verts.size());
            mesh_edit.verts.push_back(p);
            mesh_xform.affected.push_back(inner_of[vi]);
            mesh_xform.original.push_back(p);
            mesh_xform.inx.push_back(ix * sc);
            mesh_xform.iny.push_back(iy * sc);
            mesh_xform.inz.push_back(iz * sc);
        }
        auto mapv = [&](int i) {
            const auto it = inner_of.find(i);
            return it != inner_of.end() ? it->second : i;
        };
        std::vector<MeshFace> walls;
        std::vector<MeshFace> inners;
        for (const auto& ab : border) {
            const int a = ab.first, b = ab.second;
            const int na = mapv(a), nb = mapv(b);
            walls.push_back(mesh_new_face(a, b, nb, mesh_edit.tex_sel));
            walls.push_back(mesh_new_face(a, nb, na, mesh_edit.tex_sel));
        }
        auto lerp_uv = [&](int vi, float u_old, float v_old, float& u, float& v) {
            const auto& p = mesh_edit.verts[static_cast<std::size_t>(vi)];
            const float dist = std::sqrt((cx - p.x) * (cx - p.x) + (cy - p.y) * (cy - p.y) + (cz - p.z) * (cz - p.z));
            const float den = 0.05f + dist;
            const float alpha = den < 1e-8f ? 0.f : 0.05f / den;
            u = u_old + (uc - u_old) * alpha;
            v = v_old + (vc - v_old) * alpha;
        };
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            MeshFace inner = mesh_new_face(mapv(f.i0), mapv(f.i1), mapv(f.i2), f.mat);
            inner.slot = f.slot;
            inner.mat = f.mat;
            lerp_uv(f.i0, f.u0, f.v0, inner.u0, inner.v0);
            lerp_uv(f.i1, f.u1, f.v1, inner.u1, inner.v1);
            lerp_uv(f.i2, f.u2, f.v2, inner.u2, inner.v2);
            inners.push_back(inner);
        }
        std::vector<char> drop(mesh_edit.faces.size(), 0);
        for (int fi : mesh_edit.sel_faces)
            if (fi >= 0 && static_cast<std::size_t>(fi) < drop.size()) drop[static_cast<std::size_t>(fi)] = 1;
        std::vector<MeshFace> nf;
        for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i)
            if (!drop[static_cast<std::size_t>(i)]) nf.push_back(mesh_edit.faces[static_cast<std::size_t>(i)]);
        const int wall0 = static_cast<int>(nf.size());
        nf.insert(nf.end(), walls.begin(), walls.end());
        const int inner0 = static_cast<int>(nf.size());
        nf.insert(nf.end(), inners.begin(), inners.end());
        mesh_edit.faces = std::move(nf);
        for (int i = wall0; i < inner0; ++i) {
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
            mesh_ensure_edge(f.i0, f.i1);
            mesh_ensure_edge(f.i1, f.i2);
            mesh_ensure_edge(f.i2, f.i0);
        }
        mesh_edit.sel_faces.clear();
        mesh_edit.sel_verts.clear();
        mesh_edit.sel_edges.clear();
        for (int i = 0; i < static_cast<int>(inners.size()); ++i)
            mesh_edit.sel_faces.insert(inner0 + i);
        mesh_xform.sel_was = mesh_edit.last_event_sel;
        mesh_xform.did_cap = true;
        mesh_xform.cube_from = wall0;
        mesh_xform.active = true;
        mesh_xform.mode = 3;
        mesh_xform.axis = -1;
        mesh_xform.pivot_x = cx;
        mesh_xform.pivot_y = cy;
        mesh_xform.pivot_z = cz;
        mesh_xform.inset_thick = 0.f;
        mesh_xform.disp_x = mesh_xform.disp_y = mesh_xform.disp_z = 0.f;
        float imx = 0, imy = 0, ifw = 1, ifh = 1;
        input::cursor_to_framebuffer(window, imx, imy, ifw, ifh);
        mesh_xform.start_mx = imx;
        mesh_xform.start_my = imy;
        mesh_debug_invariants();
        mesh_refresh_preview();
        panel.set_status("I inset: mouse grosor, LMB confirma");
    };
    auto mesh_select_linked = [&]() {
        mesh_sel_cap();
        if (mesh_edit.pick == MeshEdit::Pick::Edges) {
            int seed = mesh_edit.last_edge;
            if (seed < 0 && !mesh_edit.sel_edges.empty()) seed = *mesh_edit.sel_edges.begin();
            if (seed < 0 || static_cast<std::size_t>(seed) >= mesh_edit.edges.size()) {
                panel.set_status("L: sin semilla");
                return;
            }
            const int n = static_cast<int>(mesh_edit.edges.size());
            std::unordered_map<int, std::vector<int>> adj;
            for (int i = 0; i < n; ++i) {
                const auto& a = mesh_edit.edges[static_cast<std::size_t>(i)];
                for (int j = i + 1; j < n; ++j) {
                    const auto& b = mesh_edit.edges[static_cast<std::size_t>(j)];
                    if (a.a == b.a || a.a == b.b || a.b == b.a || a.b == b.b) {
                        adj[i].push_back(j);
                        adj[j].push_back(i);
                    }
                }
            }
            std::unordered_set<int> vis;
            std::queue<int> q;
            q.push(seed);
            vis.insert(seed);
            while (!q.empty()) {
                const int u = q.front();
                q.pop();
                auto it = adj.find(u);
                if (it == adj.end()) continue;
                for (int v : it->second) {
                    if (vis.insert(v).second) q.push(v);
                }
            }
            for (int e : vis) mesh_edit.sel_edges.insert(e);
            mesh_uv_sync_from_3d();
            return;
        }
        if (mesh_edit.pick == MeshEdit::Pick::Faces) {
            int seed = mesh_edit.last_face;
            if (seed < 0 && !mesh_edit.sel_faces.empty()) seed = *mesh_edit.sel_faces.begin();
            if (seed < 0 || static_cast<std::size_t>(seed) >= mesh_edit.faces.size()) {
                panel.set_status("L: sin semilla");
                return;
            }
            const int n = static_cast<int>(mesh_edit.faces.size());
            auto ekey = [](int a, int b) -> std::uint64_t {
                const int lo = (std::min)(a, b), hi = (std::max)(a, b);
                return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint32_t>(hi);
            };
            std::unordered_map<std::uint64_t, std::vector<int>> ef;
            for (int i = 0; i < n; ++i) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
                ef[ekey(f.i0, f.i1)].push_back(i);
                ef[ekey(f.i1, f.i2)].push_back(i);
                ef[ekey(f.i2, f.i0)].push_back(i);
            }
            std::unordered_map<int, std::vector<int>> adj;
            for (auto& [k, fl] : ef) {
                for (std::size_t i = 0; i < fl.size(); ++i)
                    for (std::size_t j = i + 1; j < fl.size(); ++j) {
                        adj[fl[i]].push_back(fl[j]);
                        adj[fl[j]].push_back(fl[i]);
                    }
            }
            std::unordered_set<int> vis;
            std::queue<int> q;
            q.push(seed);
            vis.insert(seed);
            while (!q.empty()) {
                const int u = q.front();
                q.pop();
                auto it = adj.find(u);
                if (it == adj.end()) continue;
                for (int v : it->second)
                    if (vis.insert(v).second) q.push(v);
            }
            for (int f : vis) mesh_edit.sel_faces.insert(f);
            mesh_uv_sync_from_3d();
            return;
        }
        int seed = -1;
        if (mesh_edit.last_vert >= 0) seed = mesh_edit.last_vert;
        else if (mesh_edit.hover_v >= 0) seed = mesh_edit.hover_v;
        else if (!mesh_edit.sel_verts.empty()) seed = *mesh_edit.sel_verts.begin();
        if (seed < 0) {
            panel.set_status("L: sin semilla");
            return;
        }
        std::unordered_set<int> vis;
        std::queue<int> q;
        q.push(seed);
        vis.insert(seed);
        while (!q.empty()) {
            const int u = q.front();
            q.pop();
            for (const auto& e : mesh_edit.edges) {
                int nb = -1;
                if (e.a == u) nb = e.b;
                else if (e.b == u) nb = e.a;
                if (nb < 0 || vis.count(nb)) continue;
                if (!mesh_verts_same_surface(seed, nb) && mesh_edit.verts[static_cast<std::size_t>(seed)].src_kind != 0)
                    continue;
                vis.insert(nb);
                q.push(nb);
            }
        }
        for (int v : vis) mesh_edit.sel_verts.insert(v);
        mesh_uv_sync_from_3d();
    };
    auto mesh_delete_sel = [&]() {
        const bool any = !mesh_edit.sel_verts.empty() || !mesh_edit.sel_edges.empty() ||
                         !mesh_edit.sel_faces.empty();
        if (!any) {
            panel.set_status("Borrar: sin selección");
            return;
        }
        mesh_geo_cap();
        std::unordered_set<int> kill_f = mesh_edit.sel_faces;
        for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
            if (mesh_edit.sel_verts.count(f.i0) || mesh_edit.sel_verts.count(f.i1) ||
                mesh_edit.sel_verts.count(f.i2))
                kill_f.insert(i);
            const int e0 = mesh_find_edge_i(f.i0, f.i1), e1 = mesh_find_edge_i(f.i1, f.i2),
                      e2 = mesh_find_edge_i(f.i2, f.i0);
            if ((e0 >= 0 && mesh_edit.sel_edges.count(e0)) || (e1 >= 0 && mesh_edit.sel_edges.count(e1)) ||
                (e2 >= 0 && mesh_edit.sel_edges.count(e2)))
                kill_f.insert(i);
        }
        std::vector<MeshFace> nf;
        for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i)
            if (!kill_f.count(i)) nf.push_back(mesh_edit.faces[static_cast<std::size_t>(i)]);
        mesh_edit.faces = std::move(nf);
        mesh_compact_unused();
        mesh_debug_invariants();
        mesh_refresh_preview();
        panel.set_status("Borrar selección");
    };
    auto mesh_delete_faces = [&]() { mesh_delete_sel(); };
    auto mesh_flip_faces = [&]() {
        if (mesh_edit.sel_faces.empty()) return;
        mesh_geo_cap();
        for (int fi : mesh_edit.sel_faces) {
            auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            std::swap(f.i1, f.i2);
            std::swap(f.u1, f.u2);
            std::swap(f.v1, f.v2);
        }
        mesh_refresh_preview();
        panel.set_status("Normal invertida");
    };
    auto mesh_recalc_n = [&]() {
        const int n = static_cast<int>(mesh_edit.faces.size());
        if (n <= 0) {
            panel.set_status("Recalc outside: sin caras");
            return;
        }
        mesh_geo_cap();
        std::vector<char> use(static_cast<std::size_t>(n), 0);
        if (mesh_edit.sel_faces.empty()) {
            std::fill(use.begin(), use.end(), 1);
        } else {
            for (int fi : mesh_edit.sel_faces)
                if (fi >= 0 && fi < n) use[static_cast<std::size_t>(fi)] = 1;
        }
        auto ek = [](int a, int b) -> std::uint64_t {
            const int lo = (std::min)(a, b), hi = (std::max)(a, b);
            return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint32_t>(hi);
        };
        struct EdgeUse {
            int fi = 0;
            int a = 0;
            int b = 0;
        };
        std::unordered_map<std::uint64_t, std::vector<EdgeUse>> emap;
        for (int i = 0; i < n; ++i) {
            if (!use[static_cast<std::size_t>(i)]) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
            emap[ek(f.i0, f.i1)].push_back({i, f.i0, f.i1});
            emap[ek(f.i1, f.i2)].push_back({i, f.i1, f.i2});
            emap[ek(f.i2, f.i0)].push_back({i, f.i2, f.i0});
        }
        auto flip_f = [&](int fi) {
            auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            std::swap(f.i1, f.i2);
            std::swap(f.u1, f.u2);
            std::swap(f.v1, f.v2);
        };
        const auto orig = mesh_edit.faces;
        std::vector<char> vis(static_cast<std::size_t>(n), 0);
        std::vector<char> flp(static_cast<std::size_t>(n), 0);
        for (int seed = 0; seed < n; ++seed) {
            if (!use[static_cast<std::size_t>(seed)] || vis[static_cast<std::size_t>(seed)]) continue;
            std::vector<int> island;
            std::queue<int> q;
            q.push(seed);
            vis[static_cast<std::size_t>(seed)] = 1;
            flp[static_cast<std::size_t>(seed)] = 0;
            while (!q.empty()) {
                const int fi = q.front();
                q.pop();
                island.push_back(fi);
                const auto& f = orig[static_cast<std::size_t>(fi)];
                const int es[3][2] = {{f.i0, f.i1}, {f.i1, f.i2}, {f.i2, f.i0}};
                for (int e = 0; e < 3; ++e) {
                    auto it = emap.find(ek(es[e][0], es[e][1]));
                    if (it == emap.end()) continue;
                    for (const auto& ou : it->second) {
                        if (ou.fi == fi || vis[static_cast<std::size_t>(ou.fi)]) continue;
                        vis[static_cast<std::size_t>(ou.fi)] = 1;
                        const bool same_dir = (ou.a == es[e][0] && ou.b == es[e][1]);
                        flp[static_cast<std::size_t>(ou.fi)] =
                            static_cast<char>(flp[static_cast<std::size_t>(fi)] ^ (same_dir ? 1 : 0));
                        q.push(ou.fi);
                    }
                }
            }
            for (int fi : island)
                if (flp[static_cast<std::size_t>(fi)]) flip_f(fi);
            float mx = 0.f, my = 0.f, mz = 0.f;
            int nv = 0;
            for (int fi : island) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                const int idx[3] = {f.i0, f.i1, f.i2};
                for (int k = 0; k < 3; ++k) {
                    if (idx[k] < 0 || static_cast<std::size_t>(idx[k]) >= mesh_edit.verts.size()) continue;
                    const auto& v = mesh_edit.verts[static_cast<std::size_t>(idx[k])];
                    mx += v.x;
                    my += v.y;
                    mz += v.z;
                    ++nv;
                }
            }
            if (nv > 0) {
                mx /= static_cast<float>(nv);
                my /= static_cast<float>(nv);
                mz /= static_cast<float>(nv);
            }
            float acc = 0.f;
            for (int fi : island) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
                if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                    static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                    continue;
                const auto& a = mesh_edit.verts[static_cast<std::size_t>(f.i0)];
                const auto& b = mesh_edit.verts[static_cast<std::size_t>(f.i1)];
                const auto& c = mesh_edit.verts[static_cast<std::size_t>(f.i2)];
                const float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
                const float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
                const float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                const float fcx = (a.x + b.x + c.x) * (1.f / 3.f);
                const float fcy = (a.y + b.y + c.y) * (1.f / 3.f);
                const float fcz = (a.z + b.z + c.z) * (1.f / 3.f);
                acc += nx * (fcx - mx) + ny * (fcy - my) + nz * (fcz - mz);
            }
            if (acc < 0.f) {
                for (int fi : island) flip_f(fi);
            }
        }
        mesh_refresh_preview();
        panel.set_status("Normales al exterior");
    };
    auto mesh_clear_buffer = [&]() {
        mesh_edit.verts.clear();
        mesh_edit.edges.clear();
        mesh_edit.faces.clear();
        mesh_edit.sel_verts.clear();
        mesh_edit.sel_edges.clear();
        mesh_edit.sel_faces.clear();
        mesh_edit.sel_uv.clear();
        mesh_edit.last_vert = mesh_edit.last_edge = mesh_edit.last_face = -1;
        mesh_edit.last_uv = -1;
        mesh_hover_clear();
        mesh_edit.uv_hover = -1;
        mesh_edit.uv_hover_f = -1;
        mesh_edit.geo_undo.clear();
        mesh_edit.geo_redo.clear();
        mesh_edit.sel_undo.clear();
        mesh_edit.sel_redo.clear();
        mesh_edit.dirty = false;
        mesh_edit.session_bake_dir.clear();
        auto hide_c = [&](std::size_t c) {
            if (c != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(c, false);
        };
        hide_c(mesh_edit.preview_chunk);
        for (auto c : mesh_edit.preview_chunks) hide_c(c);
        hide_c(mesh_edit.sel_overlay_chunk);
        hide_c(mesh_edit.edge_overlay_chunk);
        mesh_edit.env_verts.clear();
        mesh_edit.env_edges.clear();
        mesh_edit.env_tris.clear();
        mesh_edit.env_obj.clear();
        mesh_edit.env_spl.clear();
    };
    auto try_exit_mesh = [&]() -> bool {
        if (mesh_xform.active) {
            mesh_xform_cancel();
            return false;
        }
        mesh_edit.uv_g = false;
        mesh_edit.uv_editor_open = false;
        if (!mesh_edit.dirty) {
            edit_mode = EditMode::None;
            return true;
        }
        HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
        const int ans = MessageBoxW(
            owner,
            L"¿Salir de Mesh?\n\nSí = quedarse\nNo = descartar malla y salir\nCancelar = seguir",
            L"Bake mesh", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (ans == IDCANCEL || ans == IDYES) return false;
        mesh_clear_buffer();
        edit_mode = EditMode::None;
        return true;
    };
    [[maybe_unused]] auto mesh_cancel_creation = [&]() {
        HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
        const int ans = MessageBoxW(owner, L"¿Borrar toda la malla en edición?", L"Cancelar creación",
                                    MB_YESNO | MB_ICONQUESTION);
        if (ans != IDYES) return;
        mesh_clear_buffer();
        panel.set_status("Malla borrada");
    };

    auto draw_mesh_overlay = [&](float fw, float fh, const float* wvp) {
        if (edit_mode != EditMode::Mesh) return;
        const bool modal = mesh_edit.tex_browser_open || panel.imgui_state().catalog_browser_open;
        if (mesh_edit.sel_overlay_chunk != static_cast<std::size_t>(-1))
            renderer.set_chunk_visible(mesh_edit.sel_overlay_chunk, false);
        if (modal) return;
        auto* dl = ImGui::GetBackgroundDrawList();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float ox = vp ? vp->Pos.x : 0.f;
        const float oy = vp ? vp->Pos.y : 0.f;
        auto sp = [&](float sx, float sy) { return ImVec2(sx + ox, sy + oy); };
        const ImU32 col_idle = IM_COL32(70, 78, 88, 200);
        const ImU32 col_src = IM_COL32(140, 175, 220, 200);
        const ImU32 col_sel = IM_COL32(255, 122, 10, 255);
        const ImU32 col_hov = IM_COL32(255, 180, 80, 255);
        const ImU32 col_face_hov_ln = IM_COL32(255, 200, 70, 230);
        auto draw_face_edges = [&](const MeshVert& a, const MeshVert& b, const MeshVert& c, ImU32 line, float thick) {
            float sax, say, sbx, sby, scx, scy;
            if (!world_to_screen(a.x, a.y, a.z, 0, 0, fw, fh, wvp, sax, say)) return;
            if (!world_to_screen(b.x, b.y, b.z, 0, 0, fw, fh, wvp, sbx, sby)) return;
            if (!world_to_screen(c.x, c.y, c.z, 0, 0, fw, fh, wvp, scx, scy)) return;
            dl->AddLine(sp(sax, say), sp(sbx, sby), line, thick);
            dl->AddLine(sp(sbx, sby), sp(scx, scy), line, thick);
            dl->AddLine(sp(scx, scy), sp(sax, say), line, thick);
        };
        for (int fi : mesh_edit.sel_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            if (f.i0 < 0 || f.i1 < 0 || f.i2 < 0) continue;
            if (static_cast<std::size_t>(f.i0) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i1) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(f.i2) >= mesh_edit.verts.size())
                continue;
            draw_face_edges(mesh_edit.verts[static_cast<std::size_t>(f.i0)],
                            mesh_edit.verts[static_cast<std::size_t>(f.i1)],
                            mesh_edit.verts[static_cast<std::size_t>(f.i2)], col_sel, 4.f);
        }
        if (mesh_edit.hover_f >= 0 && static_cast<std::size_t>(mesh_edit.hover_f) < mesh_edit.faces.size() &&
            !mesh_edit.sel_faces.count(mesh_edit.hover_f)) {
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(mesh_edit.hover_f)];
            if (f.i0 >= 0 && f.i1 >= 0 && f.i2 >= 0 &&
                static_cast<std::size_t>(f.i0) < mesh_edit.verts.size() &&
                static_cast<std::size_t>(f.i1) < mesh_edit.verts.size() &&
                static_cast<std::size_t>(f.i2) < mesh_edit.verts.size())
                draw_face_edges(mesh_edit.verts[static_cast<std::size_t>(f.i0)],
                                mesh_edit.verts[static_cast<std::size_t>(f.i1)],
                                mesh_edit.verts[static_cast<std::size_t>(f.i2)], col_face_hov_ln, 2.6f);
        }
        if (mesh_edit.src_hover_f >= 0 &&
            static_cast<std::size_t>(mesh_edit.src_hover_f) < mesh_edit.src_tris.size()) {
            const auto& f = mesh_edit.src_tris[static_cast<std::size_t>(mesh_edit.src_hover_f)];
            if (f.i0 >= 0 && f.i1 >= 0 && f.i2 >= 0 &&
                static_cast<std::size_t>(f.i0) < mesh_edit.src_verts.size() &&
                static_cast<std::size_t>(f.i1) < mesh_edit.src_verts.size() &&
                static_cast<std::size_t>(f.i2) < mesh_edit.src_verts.size())
                draw_face_edges(mesh_edit.src_verts[static_cast<std::size_t>(f.i0)],
                                mesh_edit.src_verts[static_cast<std::size_t>(f.i1)],
                                mesh_edit.src_verts[static_cast<std::size_t>(f.i2)], col_face_hov_ln, 2.6f);
        }
        const float near2 = 80.f * 80.f;
        auto src_near = [&](float sx, float sy) {
            const float dx = sx - mesh_edit.hover_mx;
            const float dy = sy - mesh_edit.hover_my;
            return dx * dx + dy * dy <= near2;
        };
        const auto mouse_ray = camera.make_pick_ray(mesh_edit.hover_mx, mesh_edit.hover_my, fw, fh);
        // Whole-scene ray (every chunk) — only when the mouse or camera changed.
        static float s_ts_mx = -1.e9f, s_ts_my = -1.e9f, s_ts_t = 0.f;
        static float s_ts_wvp[16]{};
        if (mouse_ray && (s_ts_mx != mesh_edit.hover_mx || s_ts_my != mesh_edit.hover_my ||
                          std::memcmp(s_ts_wvp, wvp, sizeof(s_ts_wvp)) != 0)) {
            s_ts_mx = mesh_edit.hover_mx;
            s_ts_my = mesh_edit.hover_my;
            std::memcpy(s_ts_wvp, wvp, sizeof(s_ts_wvp));
            s_ts_t = 0.f;
            std::vector<std::size_t> excl;
            if (mesh_edit.sel_overlay_chunk != static_cast<std::size_t>(-1))
                excl.push_back(mesh_edit.sel_overlay_chunk);
            float st = 0.f;
            if (renderer.pick_surface_hit_z(mouse_ray->origin.x, mouse_ray->origin.y, mouse_ray->origin.z,
                                            mouse_ray->dir.x, mouse_ray->dir.y, mouse_ray->dir.z,
                                            excl.empty() ? nullptr : excl.data(), excl.size(), &st))
                s_ts_t = st;
        }
        const float t_scene = s_ts_t;
        const auto near_cam = mesh_cam_filter();
        auto elem_occluded = [&](float wx, float wy, float wz, float sx, float sy) -> bool {
            const float dx = sx - mesh_edit.hover_mx;
            const float dy = sy - mesh_edit.hover_my;
            if (dx * dx + dy * dy > 24.f * 24.f) return false;
            if (!mouse_ray || t_scene <= 0.f) return false;
            const float t_elem = (wx - mouse_ray->origin.x) * mouse_ray->dir.x +
                                 (wy - mouse_ray->origin.y) * mouse_ray->dir.y +
                                 (wz - mouse_ray->origin.z) * mouse_ray->dir.z;
            return t_elem > t_scene + 1e-5f;
        };
        for (int i = 0; i < static_cast<int>(mesh_edit.src_edges.size()); ++i) {
            const auto& e = mesh_edit.src_edges[static_cast<std::size_t>(i)];
            if (e.a < 0 || e.b < 0) continue;
            if (static_cast<std::size_t>(e.a) >= mesh_edit.src_verts.size() ||
                static_cast<std::size_t>(e.b) >= mesh_edit.src_verts.size())
                continue;
            const auto& a = mesh_edit.src_verts[static_cast<std::size_t>(e.a)];
            const auto& b = mesh_edit.src_verts[static_cast<std::size_t>(e.b)];
            const bool hov = mesh_edit.src_hover_e == i;
            if (!hov && !near_cam(a.x, a.y, a.z) && !near_cam(b.x, b.y, b.z)) continue;
            float sax, say, sbx, sby;
            if (!world_to_screen(a.x, a.y, a.z, 0, 0, fw, fh, wvp, sax, say)) continue;
            if (!world_to_screen(b.x, b.y, b.z, 0, 0, fw, fh, wvp, sbx, sby)) continue;
            const bool na = src_near(sax, say);
            const bool nb = src_near(sbx, sby);
            if (!hov && !na && !nb) continue;
            if (!hov) {
                const bool vis_a = na && !elem_occluded(a.x, a.y, a.z, sax, say);
                const bool vis_b = nb && !elem_occluded(b.x, b.y, b.z, sbx, sby);
                if (!vis_a && !vis_b) continue;
            }
            dl->AddLine(sp(sax, say), sp(sbx, sby), hov ? col_hov : col_src, hov ? 3.6f : 1.6f);
        }
        for (int i = 0; i < static_cast<int>(mesh_edit.src_verts.size()); ++i) {
            const auto& v = mesh_edit.src_verts[static_cast<std::size_t>(i)];
            const bool hov = mesh_edit.src_hover_v == i;
            if (!hov && !near_cam(v.x, v.y, v.z)) continue;
            float sx, sy;
            if (!world_to_screen(v.x, v.y, v.z, 0, 0, fw, fh, wvp, sx, sy)) continue;
            if (!hov && !src_near(sx, sy)) continue;
            if (!hov && elem_occluded(v.x, v.y, v.z, sx, sy)) continue;
            const float r = hov ? 7.f : 4.5f;
            dl->AddCircleFilled(sp(sx, sy), r, hov ? col_hov : col_src);
        }
        for (int i = 0; i < static_cast<int>(mesh_edit.edges.size()); ++i) {
            const auto& e = mesh_edit.edges[static_cast<std::size_t>(i)];
            if (e.a < 0 || e.b < 0) continue;
            if (static_cast<std::size_t>(e.a) >= mesh_edit.verts.size() ||
                static_cast<std::size_t>(e.b) >= mesh_edit.verts.size())
                continue;
            const auto& a = mesh_edit.verts[static_cast<std::size_t>(e.a)];
            const auto& b = mesh_edit.verts[static_cast<std::size_t>(e.b)];
            const bool sel = mesh_edit.sel_edges.count(i) != 0;
            const bool hov = mesh_edit.hover_e == i;
            if (!sel && !hov && !near_cam(a.x, a.y, a.z) && !near_cam(b.x, b.y, b.z)) continue;
            float sax, say, sbx, sby;
            if (!world_to_screen(a.x, a.y, a.z, 0, 0, fw, fh, wvp, sax, say)) continue;
            if (!world_to_screen(b.x, b.y, b.z, 0, 0, fw, fh, wvp, sbx, sby)) continue;
            if (!sel && !hov && !src_near(sax, say) && !src_near(sbx, sby)) continue;
            if (elem_occluded(a.x, a.y, a.z, sax, say) && elem_occluded(b.x, b.y, b.z, sbx, sby)) continue;
            const ImU32 col = sel ? col_sel : (hov ? col_hov : col_idle);
            const float thick = (sel || hov) ? 4.f : 2.2f;
            dl->AddLine(sp(sax, say), sp(sbx, sby), col, thick);
        }
        if (mesh_edit.sel_verts.size() == 2) {
            auto it = mesh_edit.sel_verts.begin();
            const int ia = *it++;
            const int ib = *it;
            if (ia >= 0 && ib >= 0 && static_cast<std::size_t>(ia) < mesh_edit.verts.size() &&
                static_cast<std::size_t>(ib) < mesh_edit.verts.size()) {
                const auto& a = mesh_edit.verts[static_cast<std::size_t>(ia)];
                const auto& b = mesh_edit.verts[static_cast<std::size_t>(ib)];
                float sax, say, sbx, sby;
                if (world_to_screen(a.x, a.y, a.z, 0, 0, fw, fh, wvp, sax, say) &&
                    world_to_screen(b.x, b.y, b.z, 0, 0, fw, fh, wvp, sbx, sby))
                    dl->AddLine(sp(sax, say), sp(sbx, sby), col_sel, 2.5f);
            }
        }
        for (int i = 0; i < static_cast<int>(mesh_edit.verts.size()); ++i) {
            const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
            const bool sel = mesh_edit.sel_verts.count(i) != 0;
            const bool hov = mesh_edit.hover_v == i;
            if (!sel && !hov && !near_cam(v.x, v.y, v.z)) continue;
            float sx, sy;
            if (!world_to_screen(v.x, v.y, v.z, 0, 0, fw, fh, wvp, sx, sy)) continue;
            if (!sel && !hov && !src_near(sx, sy)) continue;
            if (elem_occluded(v.x, v.y, v.z, sx, sy)) continue;
            const ImU32 col = sel ? col_sel : (hov ? col_hov : col_idle);
            const float r = (sel || hov) ? 8.f : 6.f;
            dl->AddCircleFilled(sp(sx, sy), r, col);
            dl->AddCircle(sp(sx, sy), r + 1.4f, IM_COL32(0, 0, 0, 200), 14, 1.4f);
        }
        if (mesh_xform.active && mesh_xform.snap_hit && mesh_edit.snap_mode > 0) {
            const ImU32 col_snap = IM_COL32(40, 220, 255, 255);
            auto w2s = [&](float x, float y, float z, float& sx, float& sy) -> bool {
                return world_to_screen(x, y, z, 0, 0, fw, fh, wvp, sx, sy);
            };
            if (mesh_xform.snap_kind == 'V') {
                float sx, sy;
                if (w2s(mesh_xform.snap_tx, mesh_xform.snap_ty, mesh_xform.snap_tz, sx, sy)) {
                    dl->AddCircleFilled(sp(sx, sy), 9.f, col_snap);
                    dl->AddCircle(sp(sx, sy), 10.4f, IM_COL32(0, 0, 0, 200), 14, 1.4f);
                }
            } else if (mesh_xform.snap_kind == 'E') {
                float sax, say, sbx, sby;
                if (w2s(mesh_xform.snap_ax, mesh_xform.snap_ay, mesh_xform.snap_az, sax, say) &&
                    w2s(mesh_xform.snap_bx, mesh_xform.snap_by, mesh_xform.snap_bz, sbx, sby))
                    dl->AddLine(sp(sax, say), sp(sbx, sby), col_snap, 4.f);
            } else if (mesh_xform.snap_kind == 'F') {
                float sax, say, sbx, sby, scx, scy;
                if (w2s(mesh_xform.snap_c0x, mesh_xform.snap_c0y, mesh_xform.snap_c0z, sax, say) &&
                    w2s(mesh_xform.snap_c1x, mesh_xform.snap_c1y, mesh_xform.snap_c1z, sbx, sby) &&
                    w2s(mesh_xform.snap_c2x, mesh_xform.snap_c2y, mesh_xform.snap_c2z, scx, scy)) {
                    dl->AddTriangleFilled(sp(sax, say), sp(sbx, sby), sp(scx, scy), IM_COL32(40, 220, 255, 80));
                    dl->AddTriangle(sp(sax, say), sp(sbx, sby), sp(scx, scy), col_snap, 4.f);
                }
            }
        }
        if (mesh_xform.active) {
            char buf[96];
            const char* md = mesh_xform.mode == 0   ? "G"
                             : mesh_xform.mode == 1 ? "R"
                             : mesh_xform.mode == 2 ? "S"
                                                    : "I";
            const char* ax = mesh_xform.axis == 0 ? "X" : (mesh_xform.axis == 1 ? "Y" : (mesh_xform.axis == 2 ? "Z" : "View"));
            if (mesh_xform.mode == 0 && mesh_xform.snap_hit)
                std::snprintf(buf, sizeof(buf), "Snap %c: (%.3f, %.3f, %.3f)", mesh_xform.snap_kind,
                              mesh_xform.snap_tx, mesh_xform.snap_ty, mesh_xform.snap_tz);
            else if (mesh_xform.mode == 0)
                std::snprintf(buf, sizeof(buf), "%s  %s  D: %+.2f m", mesh_xform.uv_lock ? "GG" : "G", ax,
                              mesh_xform.axis == 0   ? mesh_xform.disp_x
                              : mesh_xform.axis == 1 ? mesh_xform.disp_y
                              : mesh_xform.axis == 2 ? mesh_xform.disp_z
                                                     : std::sqrt(mesh_xform.disp_x * mesh_xform.disp_x +
                                                                 mesh_xform.disp_y * mesh_xform.disp_y));
            else if (mesh_xform.mode == 1)
                std::snprintf(buf, sizeof(buf), "%s  %s  R: %+.1f deg", md, ax, mesh_xform.disp_angle);
            else if (mesh_xform.mode == 3)
                std::snprintf(buf, sizeof(buf), "I  T: %.3f m  LMB confirma", mesh_xform.inset_thick);
            else
                std::snprintf(buf, sizeof(buf), "%s  %s  S: %.3f", md, ax, mesh_xform.disp_scale);
            float mx = 0, my = 0, cfw = fw, cfh = fh;
            input::cursor_to_framebuffer(window, mx, my, cfw, cfh);
            dl->AddText(sp(mx + 16.f, my + 16.f), IM_COL32(255, 230, 120, 255), buf);
        }
        if (mesh_edit.circle_sel) {
            float mx = 0, my = 0, cfw = fw, cfh = fh;
            input::cursor_to_framebuffer(window, mx, my, cfw, cfh);
            dl->AddCircle(sp(mx, my), mesh_edit.circle_r, IM_COL32(255, 255, 255, 210), 64, 1.6f);
            dl->AddCircle(sp(mx, my), mesh_edit.circle_r + 1.2f, IM_COL32(0, 0, 0, 140), 64, 1.2f);
        }
        if (mesh_edit.loopcut_on) {
            float mx = 0, my = 0, cfw = fw, cfh = fh;
            input::cursor_to_framebuffer(window, mx, my, cfw, cfh);
            std::vector<LcQuad> quads;
            std::vector<int> face_q;
            std::vector<LcItem> ring;
            const int lf = mesh_edit.lc_face;
            if (lf >= 0 && static_cast<std::size_t>(lf) < mesh_edit.faces.size()) {
                mesh_lc_build_quads(quads, face_q);
                ring = mesh_lc_ring(quads, face_q[static_cast<std::size_t>(lf)], mesh_edit.lc_s, mesh_edit.lc_e);
            }
            const auto ts = mesh_lc_ts();
            const ImU32 col = IM_COL32(255, 220, 60, 240);
            for (const auto& it : ring) {
                for (float t : ts) {
                    float ax, ay, az, bx, by, bz;
                    mesh_lc_lerp(it.s, it.e, t, ax, ay, az);
                    if (it.s2 == it.e2) {
                        const auto& p = mesh_edit.verts[static_cast<std::size_t>(it.s2)];
                        bx = p.x;
                        by = p.y;
                        bz = p.z;
                    } else {
                        mesh_lc_lerp(it.s2, it.e2, t, bx, by, bz);
                    }
                    float sax, say, sbx, sby;
                    if (world_to_screen(ax, ay, az, 0, 0, fw, fh, wvp, sax, say) &&
                        world_to_screen(bx, by, bz, 0, 0, fw, fh, wvp, sbx, sby))
                        dl->AddLine(sp(sax, say), sp(sbx, sby), col, 2.4f);
                }
            }
            char buf[96];
            if (ring.empty())
                std::snprintf(buf, sizeof(buf), "LoopCut: mouse sobre una arista");
            else
                std::snprintf(buf, sizeof(buf), "LoopCut x%d  quads %d  LMB aplica", static_cast<int>(ts.size()),
                              static_cast<int>(ring.size()));
            dl->AddText(sp(mx + 16.f, my + 16.f), IM_COL32(255, 220, 60, 255), buf);
        }
        if (mesh_edit.knife_on) {
            const ImU32 col = IM_COL32(255, 90, 70, 240);
            auto w2s = [&](float x, float y, float z, float& sx, float& sy) -> bool {
                return world_to_screen(x, y, z, 0, 0, fw, fh, wvp, sx, sy);
            };
            for (std::size_t i = 0; i < mesh_edit.knife_pts.size(); ++i) {
                const auto& p = mesh_edit.knife_pts[i];
                float sx, sy;
                if (w2s(p.x, p.y, p.z, sx, sy)) {
                    dl->AddCircleFilled(sp(sx, sy), 6.f, col);
                    if (i > 0) {
                        const auto& q = mesh_edit.knife_pts[i - 1];
                        float qx, qy;
                        if (w2s(q.x, q.y, q.z, qx, qy)) dl->AddLine(sp(qx, qy), sp(sx, sy), col, 2.4f);
                    }
                }
            }
            float mx = 0, my = 0, cfw = fw, cfh = fh;
            input::cursor_to_framebuffer(window, mx, my, cfw, cfh);
            MeshVert cur{};
            if (mesh_hit_bake(mx, my, fw, fh, cur.x, cur.y, cur.z)) {
                mesh_knife_lock(cur);
                float sx, sy;
                if (w2s(cur.x, cur.y, cur.z, sx, sy)) {
                    dl->AddCircle(sp(sx, sy), 7.f, col, 16, 1.6f);
                    if (!mesh_edit.knife_pts.empty()) {
                        const auto& q = mesh_edit.knife_pts.back();
                        float qx, qy;
                        if (w2s(q.x, q.y, q.z, qx, qy))
                            dl->AddLine(sp(qx, qy), sp(sx, sy), IM_COL32(255, 90, 70, 160), 1.6f);
                    }
                }
            }
            const char* ax = mesh_edit.knife_axis == 0   ? "X"
                             : mesh_edit.knife_axis == 1 ? "Y"
                             : mesh_edit.knife_axis == 2 ? "Z"
                                                         : "libre";
            char buf[80];
            std::snprintf(buf, sizeof(buf), "Knife %s  pts %d  Enter", ax,
                          static_cast<int>(mesh_edit.knife_pts.size()));
            dl->AddText(sp(mx + 16.f, my + 16.f), col, buf);
        }
    };

    auto sco_obj_th = [&](const map::MapObjectInst& o) -> float {
        const auto* sco = sco::load_sco(o.sco_path);
        bool abs_z = sco && sco::skip_terrain_add(*sco);
        for (const auto& a : edit_attachments)
            if (a.child_id == o.id) abs_z = true;
        return abs_z ? 0.f : sample_terrain_editor_xy(o.position.x, o.position.y);
    };

    auto sco_path_world_origin = [&](const map::MapObjectInst& o, const sco::ScoPath& p, float& wx, float& wy,
                                     float& wz) {
        const float th = sco_obj_th(o);
        const XMMATRIX world_m = sco_world_matrix(o, nullptr, th, 0.f, 0.f);
        DirectX::XMFLOAT3 w{};
        DirectX::XMStoreFloat3(&w, xform_point(world_m, p.x, p.y, p.z));
        wx = w.x;
        wy = w.y;
        wz = w.z;
    };
    auto sco_path_commit_live = [&]() {
        if (!sco_path_edit.active) return;
        set_owner_path_rails_wireframe(sco_path_edit.object_id, true, false);
        rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
    };
    auto sco_path_arm_live = [&]() {
        if (!sco_path_edit.active || sco_path_edit.sel < 0 ||
            static_cast<std::size_t>(sco_path_edit.sel) >= sco_path_edit.paths.size())
            return;
        map::MapObjectInst* obj = nullptr;
        for (auto& o : edit_objects)
            if (o.id == sco_path_edit.object_id) {
                obj = &o;
                break;
            }
        if (!obj) return;
        const auto& p = sco_path_edit.paths[static_cast<std::size_t>(sco_path_edit.sel)];
        sco_path_world_origin(*obj, p, sco_path_edit.grab_wx, sco_path_edit.grab_wy, sco_path_edit.grab_wz);
        set_one_path_rail_offset(obj->id, true, sco_path_edit.sel, 0.f, 0.f, 0.f, true);
    };

    auto load_sco_path_from_object = [&](const map::MapObjectInst& o) {
        const auto* sco = sco::load_sco(o.sco_path);
        sco_path_edit.object_id = o.id;
        sco_path_edit.sco_path = o.sco_path;
        sco_path_edit.paths = sco ? sco->paths : std::vector<sco::ScoPath>{};
        sco_path_edit.has_tl = sco && sco->traffic_lights_group.has_value();
        sco_path_edit.tl = sco_path_edit.has_tl ? *sco->traffic_lights_group : sco::ScoTrafficLightsGroup{};
        sco_path_edit.select_only(sco_path_edit.paths.empty() ? -1 : 0);
        sco_path_edit.tl_sel = 0;
        sco_path_edit.dirty = false;
        sco_path_edit.moving = sco_path_edit.rotating = sco_path_edit.following = sco_path_edit.placing = false;
        sco_path_edit.aa_mode = false;
        sco_path_edit.aa_a = {};
        sco_path_edit.snap_paths = sco_path_edit.paths;
        sco_path_edit.snap_tl = sco_path_edit.tl;
        sco_path_edit.snap_has_tl = sco_path_edit.has_tl;
    };

    auto exit_sco_path_edit = [&]() {
        sco_path_edit.active = false;
        sco_path_edit.traffic_open = false;
        sco_path_edit.moving = sco_path_edit.rotating = sco_path_edit.following = sco_path_edit.placing = false;
        sco_path_edit.aa_mode = false;
        if (edit_mode == EditMode::Grab || edit_mode == EditMode::Rotate || edit_mode == EditMode::SplineFollow)
            edit_mode = EditMode::None;
        grabbing = false;
        rotating = false;
        r_tap = 0;
        g_tap = 0;
    };

    // OMSI helper path textures: cars / pedestrians / rail (train) / air. The "_2" variant is for
    // bidirectional paths (direction == 2).
    auto sco_path_helper_texture = [](bool train, bool people, bool car, int direction) -> const char* {
        const bool two = (direction == 2);
        if (train) return two ? "helper_path_rail_2.bmp" : "helper_path_rail_1.bmp";
        if (people) return two ? "helper_path_hum_2.bmp" : "helper_path_hum_1.bmp";
        if (car) return two ? "helper_path_cars_2.bmp" : "helper_path_cars_1.bmp";
        return two ? "helper_path_air_2.bmp" : "helper_path_air_1.bmp";
    };
    // Build (and cache on disk) a real .sli with a centred flat profile of the path width and the
    // matching OMSI helper texture. The temp spline then matches the path size and behaves like a
    // first-class spline (hover tint, click select, snap).
    auto sco_path_sli_for = [&](const char* texture, float width) -> std::filesystem::path {
        if (!texture || !*texture) return {};
        float w = width > 0.05f ? width : 3.f;
        w = std::round(w * 10.f) / 10.f;
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec) / "omsi_editor_sco_paths";
        if (ec) return {};
        std::filesystem::create_directories(dir, ec);
        if (ec) return {};
        const std::string stem = std::filesystem::path(texture).stem().string();
        char name[192]{};
        std::snprintf(name, sizeof(name), "%s_v2_w%.1f.sli", stem.c_str(), static_cast<double>(w));
        const auto out = dir / name;
        if (std::filesystem::exists(out, ec)) return out;
        spline::SplineDefinition d{};
        spline::ProfilePoint a{};
        a.x = -w * 0.5f;
        a.z = 0.f;
        a.uv_x = 0.f;
        a.y_tiling = 1.f;
        a.material_id = 0;
        spline::ProfilePoint b{};
        b.x = w * 0.5f;
        b.z = 0.f;
        b.uv_x = 1.f;
        b.y_tiling = 1.f;
        b.material_id = 0;
        d.profiles.push_back({a, b});
        d.textures.push_back(texture);
        d.texture_alpha.push_back(0);
        spline::SliPath lp{};
        lp.typ = 0;
        lp.lateral = 0.f;
        lp.height = 0.f;
        lp.width = w;
        lp.direction = 2;
        d.paths.push_back(lp);
        d.path_length = 10.f;
        if (!spline::write_sli(d, out)) return {};
        spline::invalidate_sli_cache(out);
        return out;
    };

    auto enter_sco_path_from_selection = [&](bool traffic) {
        if (selected_object < 0 || static_cast<std::size_t>(selected_object) >= edit_objects.size()) {
            panel.set_status("Seleccioná un .sco con [path]");
            return;
        }
        const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
        const auto* sco = sco::load_sco(o.sco_path);
        if (!sco || (!sco->has_path && !sco->traffic_lights_group && !traffic)) {
            panel.set_status("Seleccioná un .sco con [path]");
            return;
        }
        if (edit_mode == EditMode::Mesh && !try_exit_mesh()) return;
        if (edit_mode == EditMode::Mesh) edit_mode = EditMode::None;
        selected_spline = -1;
        load_sco_path_from_object(o);
        sco_path_edit.active = true;
        // Editing happens on the temporary splines: drop the object selection so G/R act on the
        // selected path instead of rotating/moving the whole .sco.
        selected_object = -1;
        panel.imgui_set_tool(ui::EditorTool::Spline);
        panel.queue_imgui_tool_tab(static_cast<int>(ui::EditorTool::Spline));
        sco_path_edit.temp_first = -1;
        sco_path_edit.temp_count = 0;
        sco_path_edit.select_only(sco_path_edit.paths.empty() ? -1 : 0);
        if (traffic) sco_path_edit.traffic_open = true;
        show_path_rails = true;
        ensure_path_rails_for_owner(o.id, true);
        rebuild_path_rails_for_owner(o.id, true);
        panel.set_status(traffic ? "Semaforos .sco" : "Editar paths .sco");
    };

    auto sco_start_new_path = [&]() {
        if (!sco_path_edit.active) return;
        push_undo("sco_path_n");
        sco::ScoPath p{};
        p.length = 10.f;
        p.width = 4.f;
        if (sco_path_edit.sel >= 0 &&
            static_cast<std::size_t>(sco_path_edit.sel) < sco_path_edit.paths.size()) {
            const auto& src = sco_path_edit.paths[static_cast<std::size_t>(sco_path_edit.sel)];
            p = src;
            map::SplineInst mini = sco::sco_path_to_spline_inst(src);
            mini.length = (std::max)(0.05f, std::abs(src.length));
            const auto end = spline::evaluate_spline(mini, mini.length);
            p.x = end.x;
            p.y = end.y;
            p.z = end.z;
            p.heading_deg = end.heading_rad * (180.f / 3.14159265f);
        }
        sco_path_edit.paths.push_back(p);
        sco_path_edit.select_only(static_cast<int>(sco_path_edit.paths.size()) - 1);
        auto& np = sco_path_edit.paths.back();
        sco_path_edit.placing = true;
        sco_path_edit.moving = true;
        sco_path_edit.rotating = sco_path_edit.following = false;
        sco_path_edit.grab_lx = np.x;
        sco_path_edit.grab_ly = np.y;
        sco_path_edit.grab_lz = np.z;
        sco_path_edit.grab_hdg = np.heading_deg;
        sco_path_edit.grab_len = np.length;
        sco_path_edit.grab_radius = np.radius;
        sco_path_edit.dirty = true;
        edit_mode = EditMode::Grab;
        grabbing = true;
        rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
        sco_path_arm_live();
        panel.set_status("N path .sco — mouse coloca, LMB asienta, Ctrl snap");
    };

    auto sco_clone_sel_path = [&]() {
        if (!sco_path_edit.active || sco_path_edit.sel < 0 ||
            static_cast<std::size_t>(sco_path_edit.sel) >= sco_path_edit.paths.size())
            return;
        push_undo("sco_path_clone");
        auto p = sco_path_edit.paths[static_cast<std::size_t>(sco_path_edit.sel)];
        sco_path_edit.paths.push_back(p);
        sco_path_edit.select_only(static_cast<int>(sco_path_edit.paths.size()) - 1);
        auto& np = sco_path_edit.paths.back();
        sco_path_edit.placing = true;
        sco_path_edit.moving = true;
        sco_path_edit.rotating = sco_path_edit.following = false;
        sco_path_edit.grab_lx = np.x;
        sco_path_edit.grab_ly = np.y;
        sco_path_edit.grab_lz = np.z;
        sco_path_edit.grab_hdg = np.heading_deg;
        sco_path_edit.grab_len = np.length;
        sco_path_edit.grab_radius = np.radius;
        sco_path_edit.dirty = true;
        edit_mode = EditMode::Grab;
        grabbing = true;
        rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
        sco_path_arm_live();
        panel.set_status("Clon path .sco — mouse coloca, LMB asienta");
    };

    auto sco_delete_sel_path = [&]() {
        if (!sco_path_edit.active || sco_path_edit.sel < 0 ||
            static_cast<std::size_t>(sco_path_edit.sel) >= sco_path_edit.paths.size())
            return;
        push_undo("sco_path_del");
        sco_path_edit.paths.erase(sco_path_edit.paths.begin() + sco_path_edit.sel);
        sco_path_edit.selected.clear();
        sco_path_edit.sel = -1;
        sco_path_edit.dirty = true;
        rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
    };

    auto sync_sco_path_edit = [&]() {
        if (!sco_path_edit.active) return;
        // The path editor is bound to the .sco object it was entered from. Selection belongs to the
        // temporary splines while editing, so never key the session off selected_object.
        const map::MapObjectInst* o = nullptr;
        for (const auto& cand : edit_objects)
            if (cand.id == sco_path_edit.object_id) {
                o = &cand;
                break;
            }
        if (!o) {
            if (!sco_path_edit.dirty) exit_sco_path_edit();
            return;
        }
        const auto* sco = sco::load_sco(o->sco_path);
        if (!sco || !sco->has_path) {
            if (!sco_path_edit.dirty) exit_sco_path_edit();
        }
    };

    auto sco_path_save = [&]() -> bool {
        if (!sco_path_edit.active || sco_path_edit.sco_path.empty()) return false;
        std::error_code ec;
        {
            const auto now = std::chrono::system_clock::now();
            const auto tt = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
            localtime_s(&tm, &tt);
            char ts[32]{};
            std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
            const auto bak = std::filesystem::path(sco_path_edit.sco_path.string() + "." + ts + ".bak");
            std::filesystem::copy_file(sco_path_edit.sco_path, bak,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
        std::optional<sco::ScoTrafficLightsGroup> grp;
        if (sco_path_edit.has_tl || !sco_path_edit.tl.lights.empty()) grp = sco_path_edit.tl;
        if (!sco::rewrite_sco_paths_and_traffic(sco_path_edit.sco_path, sco_path_edit.paths, grp)) {
            panel.set_status("No se pudo guardar .sco");
            return false;
        }
        sco_path_edit.dirty = false;
        rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
        panel.set_status("Guardado " + sco_path_edit.sco_path.filename().string());
        return true;
    };

    auto try_exit_sco_path_edit = [&]() -> bool {
        if (!sco_path_edit.active) return true;
        if (!sco_path_edit.dirty) {
            exit_sco_path_edit();
            return true;
        }
        HWND owner = panel.hwnd() ? panel.hwnd() : glfwGetWin32Window(window);
        const int ans = MessageBoxW(
            owner,
            L"¿Guardar los paths del .sco?\n\nSí = escribir .sco\nNo = descartar cambios\nCancelar = seguir editando",
            L"Editar paths .sco", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (ans == IDCANCEL) return false;
        if (ans == IDYES) {
            if (!sco_path_save()) return false;
        } else {
            sco_path_edit.paths = sco_path_edit.snap_paths;
            sco_path_edit.tl = sco_path_edit.snap_tl;
            sco_path_edit.has_tl = sco_path_edit.snap_has_tl;
            sco_path_edit.dirty = false;
            rebuild_path_rails_for_owner(sco_path_edit.object_id, true);
        }
        exit_sco_path_edit();
        return true;
    };

    auto apply_sco_path_hit = [&](float wx, float wy, float wz) {
        if (!sco_path_edit.active || sco_path_edit.sel < 0 ||
            static_cast<std::size_t>(sco_path_edit.sel) >= sco_path_edit.paths.size())
            return;
        map::MapObjectInst* obj = nullptr;
        for (auto& o : edit_objects)
            if (o.id == sco_path_edit.object_id) {
                obj = &o;
                break;
            }
        if (!obj) return;

        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        // Ctrl-snap with hysteresis: once an endpoint is acquired, stick to it until Ctrl is released
        // so the picked heading cannot flip between the two endpoints of a nearby path.
        static bool s_snap_active = false;
        static PathEndpointPick s_snap{};
        bool snapped = false;
        PathEndpointPick snap{};
        if (ctrl) {
            if (s_snap_active && s_snap.valid) {
                snap = s_snap;
                snapped = true;
            } else {
                float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                input::cursor_to_framebuffer(window, mx, my, fw, fh);
                float wvp[16]{};
                camera.build_world_view_proj(fw / fh, wvp);
                snap = pick_path_endpoint_near_mouse(mx, my, fw, fh, wvp);
                if (snap.valid && !(snap.from_object && snap.owner_id == sco_path_edit.object_id &&
                                    snap.path_index == sco_path_edit.sel)) {
                    s_snap = snap;
                    s_snap_active = true;
                    snapped = true;
                }
            }
        } else {
            s_snap_active = false;
            s_snap = {};
        }

        const float th = sco_obj_th(*obj);
        const XMMATRIX world_m = sco_world_matrix(*obj, nullptr, th, 0.f, 0.f);
        const XMMATRIX inv = DirectX::XMMatrixInverse(nullptr, world_m);
        if (snapped) {
            wx = snap.x;
            wy = snap.y;
            wz = snap.z;
        }
        DirectX::XMFLOAT3 loc{};
        DirectX::XMStoreFloat3(&loc, xform_point(inv, wx, wy, wz));

        auto& p = sco_path_edit.paths[static_cast<std::size_t>(sco_path_edit.sel)];
        auto snap_heading = [&]() -> float {
            auto norm = [](float v) {
                while (v > 180.f) v -= 360.f;
                while (v < -180.f) v += 360.f;
                return v;
            };
            const float h = snap.heading_deg - obj->rotation_deg.x;
            const float ca = std::fabs(norm(h - p.heading_deg));
            const float cb = std::fabs(norm(h + 180.f - p.heading_deg));
            return ca <= cb ? h : (h + 180.f);
        };
        if (sco_path_edit.moving || sco_path_edit.placing) {
            p.x = loc.x;
            p.y = loc.y;
            p.z = snapped ? loc.z : sco_path_edit.grab_lz;
            if (snapped) p.heading_deg = snap_heading();
            rebuild_one_object_path_rail(obj->id, sco_path_edit.sel, true);
        } else if (sco_path_edit.rotating) {
            const float dx = loc.x - sco_path_edit.grab_lx;
            const float dy = loc.y - sco_path_edit.grab_ly;
            float ang = std::atan2(dx, dy) * (180.f / 3.14159265f);
            if (snapped) {
                ang = snap_heading();
            } else if (ctrl) {
                float rel = ang - sco_path_edit.grab_hdg;
                while (rel > 180.f) rel -= 360.f;
                while (rel < -180.f) rel += 360.f;
                ang = sco_path_edit.grab_hdg + std::round(rel / 90.f) * 90.f;
            }
            p.heading_deg = ang;
            rebuild_one_object_path_rail(obj->id, sco_path_edit.sel, true);
        } else if (sco_path_edit.following) {
            spline::fit_spline_to_xy(p.x, p.y, p.heading_deg, loc.x, loc.y, p.length, p.radius);
            rebuild_one_object_path_rail(obj->id, sco_path_edit.sel, true);
        }
        sco_path_edit.dirty = true;
    };

    auto sco_reindex_tl = [&](int removed) {
        for (auto& p : sco_path_edit.paths) {
            if (!p.has_use_traffic_light) continue;
            if (p.use_traffic_light == removed) {
                p.has_use_traffic_light = false;
                p.use_traffic_light = -1;
            } else if (p.use_traffic_light > removed) {
                --p.use_traffic_light;
            }
        }
    };

    auto mesh_uv_mat = [&]() -> int {
        for (int fi : mesh_edit.sel_faces) {
            if (fi >= 0 && static_cast<std::size_t>(fi) < mesh_edit.faces.size())
                return mesh_edit.faces[static_cast<std::size_t>(fi)].mat;
        }
        return mesh_edit.tex_sel;
    };
    auto mesh_uv_bind_tex = [&]() {
        const int mat = mesh_uv_mat();
        const auto path = mesh_tex_path_at(mat);
        if (path.empty()) {
            mesh_edit.uv_srv_ok = false;
            mesh_edit.uv_mat = mat;
            return;
        }
        if (mesh_edit.uv_srv_ok && mesh_edit.uv_mat == mat && mesh_edit.uv_tex_path == path) return;
        if (!mesh_edit.uv_srv_cpu) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
            if (!imgui_overlay.alloc_srv(&cpu, &gpu)) {
                mesh_edit.uv_srv_ok = false;
                return;
            }
            mesh_edit.uv_srv_cpu = static_cast<std::uint64_t>(cpu.ptr);
            mesh_edit.uv_srv_gpu = static_cast<std::uint64_t>(gpu.ptr);
        }
        mesh_edit.uv_srv_ok = renderer.copy_file_texture_srv(path, mesh_edit.uv_srv_cpu);
        mesh_edit.uv_mat = mat;
        mesh_edit.uv_tex_path = path;
    };
    auto mesh_tex_thumb = [&](const std::filesystem::path& path) -> std::uint64_t {
        if (path.empty()) return 0;
        std::error_code ec;
        const auto n = path.lexically_normal();
        for (auto& t : mesh_edit.tex_thumbs) {
            if (t.path == n) {
                t.used = mesh_edit.tex_thumb_clock;
                return t.ok ? t.srv_gpu : 0;
            }
        }
        if (mesh_edit.tex_thumb_loads >= 2) return 0;
        MeshEdit::TexThumb* slot = nullptr;
        if (mesh_edit.tex_thumbs.size() < 32) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
            if (!imgui_overlay.alloc_srv(&cpu, &gpu)) return 0;
            mesh_edit.tex_thumbs.push_back({});
            slot = &mesh_edit.tex_thumbs.back();
            slot->srv_cpu = static_cast<std::uint64_t>(cpu.ptr);
            slot->srv_gpu = static_cast<std::uint64_t>(gpu.ptr);
        } else {
            int best = 0;
            for (int i = 1; i < static_cast<int>(mesh_edit.tex_thumbs.size()); ++i)
                if (mesh_edit.tex_thumbs[static_cast<std::size_t>(i)].used <
                    mesh_edit.tex_thumbs[static_cast<std::size_t>(best)].used)
                    best = i;
            slot = &mesh_edit.tex_thumbs[static_cast<std::size_t>(best)];
        }
        ++mesh_edit.tex_thumb_loads;
        slot->path = n;
        slot->used = mesh_edit.tex_thumb_clock;
        slot->ok = std::filesystem::exists(n, ec) && renderer.copy_file_texture_srv(n, slot->srv_cpu);
        return slot->ok ? slot->srv_gpu : 0;
    };
    auto mesh_uv_g_begin = [&](int mode = 0) {
        std::vector<int> loops;
        std::unordered_set<int> seen;
        auto add_lp = [&](int lp) {
            if (seen.insert(lp).second) loops.push_back(lp);
        };
        const int mat = mesh_uv_mat();
        auto face_ok = [&](int fi) -> bool {
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) return false;
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
            if (mat >= 0 && f.mat != mat) return false;
            if (mesh_edit.sel_uv.empty() && !mesh_edit.sel_faces.empty() && !mesh_edit.sel_faces.count(fi))
                return false;
            return true;
        };
        if (!mesh_edit.sel_uv.empty()) {
            for (int lp : mesh_edit.sel_uv)
                if (face_ok(lp / 3)) add_lp(lp);
        }
        if (loops.empty()) {
            for (int fi : mesh_edit.sel_faces) {
                if (!face_ok(fi)) continue;
                add_lp(fi * 3 + 0);
                add_lp(fi * 3 + 1);
                add_lp(fi * 3 + 2);
            }
        }
        if (loops.empty()) {
            panel.set_status("UV G/R/S: seleccioná caras de esta textura");
            return;
        }
        mesh_geo_cap();
        mesh_edit.uv_g = true;
        mesh_edit.uv_xf_mode = mode;
        mesh_edit.uv_axis = -1;
        mesh_edit.uv_scale_mask = 3;
        mesh_edit.uv_g_mx = ImGui::GetIO().MousePos.x;
        mesh_edit.uv_g_my = ImGui::GetIO().MousePos.y;
        mesh_edit.uv_xf_loops = std::move(loops);
        mesh_edit.uv_xf_ou.clear();
        mesh_edit.uv_xf_ov.clear();
        float umin = 1.e30f, vmin = 1.e30f, umax = -1.e30f, vmax = -1.e30f;
        int n = 0;
        for (int lp : mesh_edit.uv_xf_loops) {
            const int fi = lp / 3, c = lp % 3;
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            float u = 0.f, v = 0.f;
            mesh_uv_get(mesh_edit.faces[static_cast<std::size_t>(fi)], c, u, v);
            mesh_edit.uv_xf_ou.push_back(u);
            mesh_edit.uv_xf_ov.push_back(v);
            umin = (std::min)(umin, u);
            umax = (std::max)(umax, u);
            vmin = (std::min)(vmin, v);
            vmax = (std::max)(vmax, v);
            ++n;
        }
        if (n <= 0) {
            mesh_edit.uv_g = false;
            panel.set_status("UV G/R/S: seleccioná V/E/F");
            return;
        }
        mesh_edit.uv_pivot_u = (umin + umax) * 0.5f;
        mesh_edit.uv_pivot_v = (vmin + vmax) * 0.5f;
        panel.set_status(mode == 1 ? "UV R (eje bbox)" : (mode == 2 ? "UV S" : "UV G"));
    };
    auto mesh_uv_g_apply = [&](float p0x, float p0y, float sz) {
        if (!mesh_edit.uv_g) return;
        if (sz < 1.f) return;
        const float mx = ImGui::GetIO().MousePos.x;
        const float my = ImGui::GetIO().MousePos.y;
        const float piv_sx = p0x + mesh_edit.uv_pivot_u * sz;
        const float piv_sy = p0y + mesh_edit.uv_pivot_v * sz;
        const float du_raw = (mx - mesh_edit.uv_g_mx) / sz;
        const float dv_raw = (my - mesh_edit.uv_g_my) / sz;
        float du = du_raw, dv = dv_raw;
        if (mesh_edit.uv_xf_mode == 0) {
            if (mesh_edit.uv_axis == 0) dv = 0.f;
            else if (mesh_edit.uv_axis == 1) du = 0.f;
        }
        float ang = 0.f;
        float sc = 1.f;
        if (mesh_edit.uv_xf_mode == 1) {
            const float a0 = std::atan2(mesh_edit.uv_g_my - piv_sy, mesh_edit.uv_g_mx - piv_sx);
            const float a1 = std::atan2(my - piv_sy, mx - piv_sx);
            ang = a1 - a0;
            const float pi = 3.14159265f;
            while (ang > pi) ang -= 2.f * pi;
            while (ang < -pi) ang += 2.f * pi;
        } else if (mesh_edit.uv_xf_mode == 2) {
            float d0 = std::hypot(mesh_edit.uv_g_mx - piv_sx, mesh_edit.uv_g_my - piv_sy);
            if (d0 < 16.f) d0 = 16.f;
            sc = std::hypot(mx - piv_sx, my - piv_sy) / d0;
            sc = (std::max)(0.01f, sc);
        }
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (mesh_edit.uv_xf_mode == 1 && ctrl) {
            const float q = 3.14159265f * 0.5f;
            ang = std::round(ang / q) * q;
        }
        const float ca = std::cos(ang), sa = std::sin(ang);
        std::vector<float> nu(mesh_edit.uv_xf_loops.size()), nv(mesh_edit.uv_xf_loops.size());
        for (std::size_t i = 0; i < mesh_edit.uv_xf_loops.size() && i < mesh_edit.uv_xf_ou.size(); ++i) {
            const float ou = mesh_edit.uv_xf_ou[i];
            const float ov = mesh_edit.uv_xf_ov[i];
            if (mesh_edit.uv_xf_mode == 0) {
                nu[i] = ou + du;
                nv[i] = ov + dv;
            } else if (mesh_edit.uv_xf_mode == 1) {
                const float rx = ou - mesh_edit.uv_pivot_u;
                const float ry = ov - mesh_edit.uv_pivot_v;
                nu[i] = mesh_edit.uv_pivot_u + rx * ca - ry * sa;
                nv[i] = mesh_edit.uv_pivot_v + rx * sa + ry * ca;
            } else {
                float su = sc, sv = sc;
                const int sm = mesh_edit.uv_scale_mask;
                if (!(sm & 1)) su = 1.f;
                if (!(sm & 2)) sv = 1.f;
                nu[i] = mesh_edit.uv_pivot_u + (ou - mesh_edit.uv_pivot_u) * su;
                nv[i] = mesh_edit.uv_pivot_v + (ov - mesh_edit.uv_pivot_v) * sv;
            }
        }
        float sdu = 0.f, sdv = 0.f;
        const float mu = (mx - p0x) / sz, mv = (my - p0y) / sz;
        if (ctrl && mesh_edit.uv_xf_mode != 1) {
            const float d0u = std::fabs(mu);
            const float d1u = std::fabs(mu - 1.f);
            const float d0v = std::fabs(mv);
            const float d1v = std::fabs(mv - 1.f);
            const float mne = (std::min)((std::min)(d0u, d1u), (std::min)(d0v, d1v));
            float best = 1.e30f;
            for (std::size_t i = 0; i < nu.size(); ++i) {
                if (mne == d0u) {
                    const float d = std::fabs(nu[i]);
                    if (d < best) {
                        best = d;
                        sdu = -nu[i];
                        sdv = 0.f;
                    }
                } else if (mne == d1u) {
                    const float d = std::fabs(nu[i] - 1.f);
                    if (d < best) {
                        best = d;
                        sdu = 1.f - nu[i];
                        sdv = 0.f;
                    }
                } else if (mne == d0v) {
                    const float d = std::fabs(nv[i]);
                    if (d < best) {
                        best = d;
                        sdu = 0.f;
                        sdv = -nv[i];
                    }
                } else {
                    const float d = std::fabs(nv[i] - 1.f);
                    if (d < best) {
                        best = d;
                        sdu = 0.f;
                        sdv = 1.f - nv[i];
                    }
                }
            }
        } else if (!ctrl) {
            std::unordered_set<int> moving(mesh_edit.uv_xf_loops.begin(), mesh_edit.uv_xf_loops.end());
            const int mat = mesh_uv_mat();
            auto try_d = [&](float tu, float tv, float& best) {
                for (std::size_t i = 0; i < nu.size(); ++i) {
                    const float dx = tu - nu[i], dy = tv - nv[i];
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < best) {
                        best = d2;
                        sdu = dx;
                        sdv = dy;
                    }
                }
            };
            float best = 0.035f * 0.035f;
            for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
                const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                if (mat >= 0 && f.mat != mat) continue;
                const float us[3] = {f.u0, f.u1, f.u2};
                const float vs[3] = {f.v0, f.v1, f.v2};
                if (mesh_edit.pick == MeshEdit::Pick::Verts) {
                    for (int c = 0; c < 3; ++c) {
                        if (moving.count(fi * 3 + c)) continue;
                        try_d(us[c], vs[c], best);
                    }
                } else if (mesh_edit.pick == MeshEdit::Pick::Edges) {
                    for (int c = 0; c < 3; ++c) {
                        const int a = c, b = (c + 1) % 3;
                        if (moving.count(fi * 3 + a) && moving.count(fi * 3 + b)) continue;
                        const float ax = us[a], ay = vs[a], bx = us[b], by = vs[b];
                        for (std::size_t i = 0; i < nu.size(); ++i) {
                            const float den = (std::max)(1.e-8f, (bx - ax) * (bx - ax) + (by - ay) * (by - ay));
                            float t = ((nu[i] - ax) * (bx - ax) + (nv[i] - ay) * (by - ay)) / den;
                            t = (std::max)(0.f, (std::min)(1.f, t));
                            const float tu = ax + t * (bx - ax), tv = ay + t * (by - ay);
                            const float dx = tu - nu[i], dy = tv - nv[i];
                            const float d2 = dx * dx + dy * dy;
                            if (d2 < best) {
                                best = d2;
                                sdu = dx;
                                sdv = dy;
                            }
                        }
                    }
                } else {
                    const float cu = (us[0] + us[1] + us[2]) * (1.f / 3.f);
                    const float cv = (vs[0] + vs[1] + vs[2]) * (1.f / 3.f);
                    if (!moving.count(fi * 3) || !mesh_edit.sel_faces.count(fi)) try_d(cu, cv, best);
                }
            }
        }
        if (mesh_edit.uv_xf_mode == 0) {
            if (mesh_edit.uv_axis == 0) sdv = 0.f;
            else if (mesh_edit.uv_axis == 1) sdu = 0.f;
        } else if (mesh_edit.uv_xf_mode == 2) {
            if (!(mesh_edit.uv_scale_mask & 1)) sdu = 0.f;
            if (!(mesh_edit.uv_scale_mask & 2)) sdv = 0.f;
        }
        for (std::size_t i = 0; i < mesh_edit.uv_xf_loops.size() && i < nu.size(); ++i) {
            const int lp = mesh_edit.uv_xf_loops[i];
            const int fi = lp / 3, c = lp % 3;
            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
            mesh_uv_set(mesh_edit.faces[static_cast<std::size_t>(fi)], c, nu[i] + sdu, nv[i] + sdv);
        }
    };
    auto mesh_uv_g_end = [&](bool cancel) {
        if (!mesh_edit.uv_g) return;
        if (cancel) {
            for (std::size_t i = 0; i < mesh_edit.uv_xf_loops.size() && i < mesh_edit.uv_xf_ou.size(); ++i) {
                const int lp = mesh_edit.uv_xf_loops[i];
                const int fi = lp / 3, c = lp % 3;
                if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
                mesh_uv_set(mesh_edit.faces[static_cast<std::size_t>(fi)], c, mesh_edit.uv_xf_ou[i],
                            mesh_edit.uv_xf_ov[i]);
            }
        }
        mesh_refresh_preview();
        mesh_edit.uv_g = false;
        mesh_edit.uv_axis = -1;
        mesh_edit.uv_scale_mask = 3;
        mesh_edit.uv_xf_loops.clear();
        mesh_edit.uv_xf_ou.clear();
        mesh_edit.uv_xf_ov.clear();
    };
    auto mesh_uv_pick_face = [&](float u, float v) -> int {
        int best = -1;
        float best_a = 1.e30f;
        auto area = [&](float ax, float ay, float bx, float by, float cx, float cy) {
            return std::fabs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
        };
        for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
            const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
            if (!mesh_uv_face_shown(i)) continue;
            if (!mesh_point_in_tri2(u, v, f.u0, f.v0, f.u1, f.v1, f.u2, f.v2)) continue;
            const float a = area(f.u0, f.v0, f.u1, f.v1, f.u2, f.v2);
            if (a < best_a) {
                best_a = a;
                best = i;
            }
        }
        return best;
    };

    // Pick texture from a map .sco / .sli (Textura / UV → "Pick textura"): same hover (yellow) /
    // select (red) picking as the Objects / Splines tabs, then a window lists its textures.
    auto mesh_tex_pick_hover = [&](float mx, float my, float fw, float fh, const float* wvp) {
        if (mesh_edit.tex_pick_kind == 1) mesh_edit.tex_pick_hover = pick_object_index(mx, my, fw, fh, wvp);
        else if (mesh_edit.tex_pick_kind == 2) mesh_edit.tex_pick_hover = pick_spline_index(mx, my, fw, fh, wvp);
        else mesh_edit.tex_pick_hover = -1;
    };
    auto mesh_tex_pick_collect = [&](bool obj, int idx) {
        std::vector<std::filesystem::path> out;
        std::unordered_set<std::string> seen;
        auto add = [&](const std::filesystem::path& from, const std::string& name) {
            if (name.empty()) return;
            const auto r = gfx::dx12::resolve_texture_path(from, name, cfg.omsi_root);
            if (!r) return;
            if (seen.insert(r->lexically_normal().string()).second) out.push_back(*r);
        };
        if (obj) {
            const auto& o = edit_objects[static_cast<std::size_t>(idx)];
            mesh_edit.tex_pick_src = o.sco_path.filename().string();
            mesh_edit.tex_pick_sel_id = o.id;
            if (const auto* sco = sco::load_sco(o.sco_path)) {
                if (sco->tree.valid) add(o.sco_path, sco->tree.texture);
                for (const auto& ref : sco->meshes) {
                    if (ref.is_o3d) {
                        if (const auto m = o3d::load_mapped_shared(ref.mesh_file); m && m->valid())
                            for (const auto& mat : m->mesh().parse_materials()) add(ref.mesh_file, mat.texture);
                    } else if (const auto xf = xfile::load_x_file(ref.mesh_file)) {
                        for (const auto& part : xf->parts) add(ref.mesh_file, part.texture_filename);
                    }
                }
            }
        } else {
            const auto& s = edit_splines[static_cast<std::size_t>(idx)];
            mesh_edit.tex_pick_src = s.sli_path.filename().string();
            mesh_edit.tex_pick_sel_id = s.id;
            if (const auto def = spline::load_sli(s.sli_path))
                for (const auto& t : def->textures) add(s.sli_path, t);
        }
        mesh_edit.tex_pick_sel_obj = obj;
        mesh_edit.tex_pick_list = std::move(out);
        mesh_edit.tex_pick_on.assign(mesh_edit.tex_pick_list.size(), 0);
        mesh_edit.tex_pick_list_open = true;
    };
    auto mesh_tex_pick_click = [&](float mx, float my, float fw, float fh, const float* wvp) {
        mesh_tex_pick_hover(mx, my, fw, fh, wvp);
        const int idx = mesh_edit.tex_pick_hover;
        const bool obj = mesh_edit.tex_pick_kind == 1;
        if (idx < 0 || (obj && static_cast<std::size_t>(idx) >= edit_objects.size()) ||
            (!obj && static_cast<std::size_t>(idx) >= edit_splines.size())) {
            panel.set_status(obj ? "Pick textura: click sobre un .sco" : "Pick textura: click sobre un .sli");
            return;
        }
        mesh_tex_pick_collect(obj, idx);
        mesh_edit.tex_pick_kind = 0;
        panel.set_status("Texturas de " + mesh_edit.tex_pick_src + ": " +
                         std::to_string(mesh_edit.tex_pick_list.size()));
    };
    auto mesh_tex_pick_highlight = [&]() {
        auto tint_owner = [&](bool obj, std::int32_t id, float r, float g, float b, float amt) {
            if (obj) {
                for (const auto& sp : scene_places)
                    if (sp.alive && sp.object_id == id) renderer.set_chunk_highlight(sp.chunk, amt, r, g, b);
            } else if (const auto* gs = find_spline_gpu(id)) {
                for (const auto c : gs->chunks) renderer.set_chunk_highlight(c, amt, r, g, b);
            }
        };
        if (mesh_edit.tex_pick_kind != 0 && mesh_edit.tex_pick_hover >= 0) {
            const bool obj = mesh_edit.tex_pick_kind == 1;
            const auto h = static_cast<std::size_t>(mesh_edit.tex_pick_hover);
            if (obj && h < edit_objects.size()) tint_owner(true, edit_objects[h].id, 1.f, 0.9f, 0.15f, 0.55f);
            if (!obj && h < edit_splines.size()) tint_owner(false, edit_splines[h].id, 1.f, 0.9f, 0.15f, 0.55f);
        }
        if (mesh_edit.tex_pick_list_open && mesh_edit.tex_pick_sel_id != 0)
            tint_owner(mesh_edit.tex_pick_sel_obj, mesh_edit.tex_pick_sel_id, 1.f, 0.15f, 0.1f, 0.6f);
    };
    auto draw_tex_pick_window = [&]() {
        if (!mesh_edit.tex_pick_list_open) return;
        ImGui::SetNextWindowSize(ImVec2(360.f, 420.f), ImGuiCond_FirstUseEver);
        bool open = true;
        const std::string title = "Texturas: " + mesh_edit.tex_pick_src + "###tex_pick_win";
        ui::float_slot_begin("tex_pick", 50); // docked with the other small windows
        const bool tex_pick_drawn = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse);
        ui::float_slot_end();
        if (tex_pick_drawn) {
            int n_on = 0;
            for (char c : mesh_edit.tex_pick_on) n_on += c ? 1 : 0;
            if (mesh_edit.tex_pick_list.empty()) ImGui::TextDisabled(omsi::ui::i18n::tr_lit("Sin texturas resolubles"));
            if (ImGui::SmallButton(omsi::ui::i18n::tr_lit("Todas")))
                std::fill(mesh_edit.tex_pick_on.begin(), mesh_edit.tex_pick_on.end(), 1);
            ImGui::SameLine();
            if (ImGui::SmallButton(omsi::ui::i18n::tr_lit("Ninguna")))
                std::fill(mesh_edit.tex_pick_on.begin(), mesh_edit.tex_pick_on.end(), 0);
            ImGui::SameLine();
            ImGui::TextDisabled("%d / %d", n_on, static_cast<int>(mesh_edit.tex_pick_list.size()));
            if (ImGui::BeginChild("##tex_pick_rows", ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing() * 1.4f), true)) {
                for (std::size_t i = 0; i < mesh_edit.tex_pick_list.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    bool on = mesh_edit.tex_pick_on[i] != 0;
                    const auto gpu = mesh_tex_thumb(mesh_edit.tex_pick_list[i]);
                    if (ImGui::Checkbox("##on", &on)) mesh_edit.tex_pick_on[i] = on ? 1 : 0;
                    ImGui::SameLine();
                    if (gpu) {
                        ImGui::Image(static_cast<ImTextureID>(gpu), ImVec2(40.f, 40.f));
                        ImGui::SameLine();
                    }
                    if (ImGui::Selectable(mesh_edit.tex_pick_list[i].filename().string().c_str(), on, 0,
                                          ImVec2(0.f, gpu ? 40.f : 0.f)))
                        mesh_edit.tex_pick_on[i] = on ? 0 : 1;
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            if (n_on == 0) ImGui::BeginDisabled();
            if (ImGui::Button(omsi::ui::i18n::tr_lit("Agregar a la lista"))) {
                for (std::size_t i = 0; i < mesh_edit.tex_pick_list.size(); ++i)
                    if (mesh_edit.tex_pick_on[i]) mesh_tex_append_path(mesh_edit.tex_pick_list[i]);
                panel.set_status("Texturas agregadas: " + std::to_string(n_on));
            }
            ImGui::SameLine();
            const bool can_assign = n_on == 1 && !mesh_edit.sel_faces.empty();
            if (!can_assign && n_on != 0) ImGui::BeginDisabled();
            if (ImGui::Button(omsi::ui::i18n::tr_lit("Asignar a caras sel"))) {
                for (std::size_t i = 0; i < mesh_edit.tex_pick_list.size(); ++i)
                    if (mesh_edit.tex_pick_on[i] && mesh_tex_append_path(mesh_edit.tex_pick_list[i]))
                        mesh_assign_sel_mat(mesh_edit.tex_sel);
            }
            if (!can_assign && n_on != 0) ImGui::EndDisabled();
            if (n_on == 0) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(omsi::ui::i18n::tr_lit("Cerrar"))) open = false;
        }
        ImGui::End();
        if (!open) {
            mesh_edit.tex_pick_list_open = false;
            mesh_edit.tex_pick_sel_id = 0;
        }
    };
    // SCO path editor overlay only. World modern nodes are 3D spheres (run_loop_04).
    auto draw_sco_path_overlay = [&]() {
        const bool modern = panel.imgui_state().path_style == 1;
        const bool draw_sco = sco_path_edit.active && !sco_path_edit.paths.empty();
        if (!draw_sco) return;
        (void)modern;
        const bool in_traffic = true;
        int pw = 1, ph = 1;
        glfwGetFramebufferSize(window, &pw, &ph);
        const float fw = static_cast<float>((std::max)(1, pw));
        const float fh = static_cast<float>((std::max)(1, ph));
        float wvp[16]{};
        camera.build_world_view_proj(fw / fh, wvp);

        map::MapObjectInst* obj = nullptr;
        if (draw_sco) {
            for (auto& o : edit_objects)
                if (o.id == sco_path_edit.object_id) {
                    obj = &o;
                    break;
                }
        }
        const float th = obj ? sco_obj_th(*obj) : 0.f;
        const XMMATRIX world_m = obj ? sco_world_matrix(*obj, nullptr, th, 0.f, 0.f) : XMMatrixIdentity();

        auto project = [&](float wx, float wy, float wz, float& sx, float& sy) -> bool {
            DirectX::XMFLOAT4X4 m{};
            std::memcpy(&m, wvp, sizeof(m));
            const DirectX::XMMATRIX vp = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&m));
            DirectX::XMFLOAT4 clip{};
            DirectX::XMStoreFloat4(&clip,
                                   DirectX::XMVector4Transform(DirectX::XMVectorSet(wx, wy, wz, 1.f), vp));
            if (clip.w <= 1.e-5f) return false;
            sx = (clip.x / clip.w * 0.5f + 0.5f) * fw;
            sy = (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * fh;
            return true;
        };
        auto path_len = [](const sco::ScoPath& p) {
            float plen = std::abs(p.length);
            if (std::abs(p.radius) > 0.001f && plen > 0.001f && plen <= 360.f && plen / std::abs(p.radius) > 3.2f)
                plen = std::abs(p.radius) * plen * (3.14159265f / 180.f);
            return (std::max)(0.05f, plen);
        };
        auto world_pt = [&](const sco::ScoPath& p, float along, float lat, float len, float& wx, float& wy,
                            float& wz) {
            map::SplineInst mini = sco::sco_path_to_spline_inst(p);
            mini.length = len;
            const auto lp = spline::evaluate_spline(mini, (std::clamp)(along, 0.f, len), lat, 0.f);
            DirectX::XMFLOAT3 w{};
            DirectX::XMStoreFloat3(&w, xform_point(world_m, lp.x, lp.y, lp.z));
            wx = w.x;
            wy = w.y;
            wz = w.z + kPathRailZLift + 0.06f;
        };
        auto seg_d2 = [](float px, float py, float ax, float ay, float bx, float by) {
            const float vx = bx - ax, vy = by - ay;
            const float wx = px - ax, wy = py - ay;
            const float L = vx * vx + vy * vy;
            float t = L > 1.e-6f ? (wx * vx + wy * vy) / L : 0.f;
            t = (std::clamp)(t, 0.f, 1.f);
            const float dx = wx - vx * t, dy = wy - vy * t;
            return dx * dx + dy * dy;
        };

        float mx = 0.f, my = 0.f, cfw = fw, cfh = fh;
        input::cursor_to_framebuffer(window, mx, my, cfw, cfh);
        const bool captured = imgui_overlay.wants_capture_mouse();

        const int steps = 16;
        const int kcol = steps + 1;
        const std::size_t n = sco_path_edit.paths.size();
        int hover = -1;
        float best = 16.f * 16.f;
        std::vector<float> psx(n * static_cast<std::size_t>(kcol));
        std::vector<float> psy(n * static_cast<std::size_t>(kcol));
        std::vector<unsigned char> pok(n * static_cast<std::size_t>(kcol));
        std::vector<float> lens(n);
        auto pidx = [&](int i, int k) {
            return static_cast<std::size_t>(i) * static_cast<std::size_t>(kcol) + static_cast<std::size_t>(k);
        };
        for (int i = 0; i < static_cast<int>(n); ++i) {
            const auto& p = sco_path_edit.paths[static_cast<std::size_t>(i)];
            const float len = path_len(p);
            lens[static_cast<std::size_t>(i)] = len;
            for (int k = 0; k < kcol; ++k) {
                float wx, wy, wz, sx = 0.f, sy = 0.f;
                world_pt(p, len * static_cast<float>(k) / static_cast<float>(steps), 0.f, len, wx, wy, wz);
                const bool ok = captured ? false : project(wx, wy, wz, sx, sy);
                psx[pidx(i, k)] = sx;
                psy[pidx(i, k)] = sy;
                pok[pidx(i, k)] = ok ? 1 : 0;
            }
            if (!captured) {
                for (int k = 0; k < steps; ++k) {
                    if (!pok[pidx(i, k)] || !pok[pidx(i, k + 1)]) continue;
                    const float d =
                        seg_d2(mx, my, psx[pidx(i, k)], psy[pidx(i, k)], psx[pidx(i, k + 1)], psy[pidx(i, k + 1)]);
                    if (d < best) {
                        best = d;
                        hover = i;
                    }
                }
            }
        }

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        // Same palette as classic path helpers / panel list:
        // typ0 cars=celeste, typ1 hum=blanco/gris, typ2 rail=naranja, typ3 air=cian.
        // Hover = amarillo (255,240,90). Selection = ámbar.
        static const ImU32 kKind[4] = {IM_COL32(96, 168, 255, 230), IM_COL32(190, 200, 210, 230),
                                       IM_COL32(232, 150, 66, 230), IM_COL32(120, 220, 255, 230)};
        // Apply traffic-rule tint the same way classic 3D does (rail_tint_for), but as solid
        // overlay color. When no rule (ha≈0) keep kKind by path type.
        // Rule colors + yellow hover only in Traffic (or active SCO path edit).
        // Outside Traffic: base kKind only (celeste/blanco/naranja/cian).
        auto modern_path_col = [&](bool from_object, std::int32_t owner_id, int pi, int typ,
                                   bool hov, bool sel) -> ImU32 {
            if (in_traffic && sel) return IM_COL32(255, 212, 40, 255);
            if (in_traffic && hov) return IM_COL32(255, 240, 90, 255);
            const int kk = (typ >= 0 && typ < 4) ? typ : 0;
            if (in_traffic) {
                const auto tint = rail_tint_for(from_object, owner_id, pi, typ);
                if (tint[3] >= 0.01f) {
                    const int r = (std::clamp)(static_cast<int>(tint[0] * 255.f + 0.5f), 0, 255);
                    const int g = (std::clamp)(static_cast<int>(tint[1] * 255.f + 0.5f), 0, 255);
                    const int b = (std::clamp)(static_cast<int>(tint[2] * 255.f + 0.5f), 0, 255);
                    return IM_COL32(r, g, b, 230);
                }
            }
            return kKind[kk];
        };
        // Cheap occlusion: camera ray vs nearby object AABBs only (not full mesh triangles).
        // Path-rail 3D meshes stay invisible in modern mode but keep existing for pick.
        float cam_x = 0.f, cam_y = 0.f, cam_z = 20.f;
        camera.eye_position(cam_x, cam_y, cam_z);
        struct OccBox {
            float minx, miny, minz, maxx, maxy, maxz;
        };
        std::vector<OccBox> occ_boxes;
        occ_boxes.reserve(64);
        {
            // Collect objects near the camera (visual radius). Approximate AABB from position.
            const float R = (editor_visual_radius_m > 0.f) ? editor_visual_radius_m : 300.f;
            const float R2 = R * R;
            for (const auto& o : edit_objects) {
                if (o.id == 0) continue;
                if (sco_path_edit.active && o.id == sco_path_edit.object_id) continue; // editor on top
                const float dx = o.position.x - cam_x, dy = o.position.y - cam_y;
                if (dx * dx + dy * dy > R2) continue;
                // Typical SCO / bus footprint; tall enough to hide ground paths behind them.
                constexpr float hx = 4.0f, hy = 4.0f, hz0 = -0.5f, hz1 = 4.5f;
                OccBox b{};
                b.minx = o.position.x - hx;
                b.maxx = o.position.x + hx;
                b.miny = o.position.y - hy;
                b.maxy = o.position.y + hy;
                b.minz = o.position.z + hz0;
                b.maxz = o.position.z + hz1;
                occ_boxes.push_back(b);
            }
        }
        auto ray_hits_aabb = [](float ox, float oy, float oz, float dx, float dy, float dz,
                                const OccBox& b, float& out_t) -> bool {
            float tmin = 0.f, tmax = 1.e9f;
            auto slab = [&](float o, float d, float mn, float mx) -> bool {
                if (std::fabs(d) < 1.e-8f) return o >= mn && o <= mx;
                float t0 = (mn - o) / d, t1 = (mx - o) / d;
                if (t0 > t1) std::swap(t0, t1);
                tmin = (std::max)(tmin, t0);
                tmax = (std::min)(tmax, t1);
                return tmin <= tmax;
            };
            if (!slab(ox, dx, b.minx, b.maxx)) return false;
            if (!slab(oy, dy, b.miny, b.maxy)) return false;
            if (!slab(oz, dz, b.minz, b.maxz)) return false;
            if (tmax < 0.f) return false;
            out_t = (tmin > 0.f) ? tmin : tmax;
            return out_t >= 0.f;
        };
        auto point_occluded = [&](float wx, float wy, float wz) -> bool {
            if (occ_boxes.empty()) return false;
            const float dx = wx - cam_x, dy = wy - cam_y, dz = wz - cam_z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < 1.e-3f) return false;
            const float inv = 1.f / dist;
            const float rdx = dx * inv, rdy = dy * inv, rdz = dz * inv;
            for (const auto& b : occ_boxes) {
                float t = 0.f;
                if (!ray_hits_aabb(cam_x, cam_y, cam_z, rdx, rdy, rdz, b, t)) continue;
                // Occluder clearly in front of the path point (skip grazing/ground).
                if (t + 0.8f < dist) return true;
            }
            return false;
        };
        const float opacity = (std::clamp)(sco_path_edit.path_opacity, 0.15f, 0.95f);
        for (int i = 0; i < static_cast<int>(n); ++i) {
            const auto& p = sco_path_edit.paths[static_cast<std::size_t>(i)];
            const bool sel = sco_path_edit.is_selected(i);
            const bool hov = (i == hover);
            const int kk = (p.type >= 0 && p.type < 4) ? p.type : 0;
            const ImU32 col = modern_path_col(true, sco_path_edit.object_id, i, p.type, hov, sel);
            const float thick = sel ? 5.f : (hov ? 4.f : 2.5f);
            const float len = lens[static_cast<std::size_t>(i)];
            const float half = (std::max)(p.width, 0.3f) * 0.5f;
            if (sco_path_edit.show_width) {
                for (int k = 0; k < steps; ++k) {
                    const float a0 = len * static_cast<float>(k) / static_cast<float>(steps);
                    const float a1 = len * static_cast<float>(k + 1) / static_cast<float>(steps);
                    float lx0, ly0, lz0, rx0, ry0, rz0, lx1, ly1, lz1, rx1, ry1, rz1;
                    world_pt(p, a0, -half, len, lx0, ly0, lz0);
                    world_pt(p, a0, +half, len, rx0, ry0, rz0);
                    world_pt(p, a1, -half, len, lx1, ly1, lz1);
                    world_pt(p, a1, +half, len, rx1, ry1, rz1);
                    float l0x = 0.f, l0y = 0.f, r0x = 0.f, r0y = 0.f, l1x = 0.f, l1y = 0.f, r1x = 0.f, r1y = 0.f;
                    if (!(project(lx0, ly0, lz0, l0x, l0y) && project(rx0, ry0, rz0, r0x, r0y) &&
                          project(lx1, ly1, lz1, l1x, l1y) && project(rx1, ry1, rz1, r1x, r1y)))
                        continue;
                    const ImU32 band = sel ? IM_COL32(255, 212, 40, static_cast<int>(60 * opacity))
                                           : (hov ? IM_COL32(255, 240, 90, static_cast<int>(55 * opacity))
                                                  : IM_COL32(120, 150, 200, static_cast<int>(38 * opacity)));
                    dl->AddQuadFilled(ImVec2(l0x, l0y), ImVec2(r0x, r0y), ImVec2(r1x, r1y), ImVec2(l1x, l1y), band);
                }
            }
            for (int k = 0; k < steps; ++k) {
                if (!pok[pidx(i, k)] || !pok[pidx(i, k + 1)]) continue;
                dl->AddLine(ImVec2(psx[pidx(i, k)], psy[pidx(i, k)]), ImVec2(psx[pidx(i, k + 1)], psy[pidx(i, k + 1)]),
                            col, thick);
            }
            if (sco_path_edit.show_arrows) {
            if (pok[pidx(i, 0)])
                dl->AddCircleFilled(ImVec2(psx[pidx(i, 0)], psy[pidx(i, 0)]), sel ? 6.f : 4.5f, col);
            if (pok[pidx(i, steps)])
                dl->AddCircle(ImVec2(psx[pidx(i, steps)], psy[pidx(i, steps)]), sel ? 6.f : 4.5f,
                              IM_COL32(255, 255, 255, 230), 0, 2.f);
        }
        }
        if (!captured && hover >= 0) dl->AddCircle(ImVec2(mx, my), 11.f, IM_COL32(255, 240, 90, 200), 0, 2.f);
        if (!captured && sco_path_edit.aa_mode && sco_path_edit.aa_a.valid) {
            float sx = 0.f, sy = 0.f;
            if (project(sco_path_edit.aa_a.x, sco_path_edit.aa_a.y, sco_path_edit.aa_a.z, sx, sy))
                dl->AddCircle(ImVec2(sx, sy), 9.f, IM_COL32(255, 80, 200, 255), 0, 3.f);
        }
    };

    auto draw_bottom_left_huds = [&]() {
        int pw = 1, ph = 1;
        glfwGetFramebufferSize(window, &pw, &ph);
        const float fw = static_cast<float>((std::max)(1, pw));
        const float fh = static_cast<float>((std::max)(1, ph));
        float wvp[16]{};
        camera.build_world_view_proj(fw / fh, wvp);
        if (edit_mode == EditMode::Mesh) {
            float mx = 0.f, my = 0.f, cfw = fw, cfh = fh;
            input::cursor_to_framebuffer(window, mx, my, cfw, cfh);
            const bool typing = ImGui::GetIO().WantTextInput;
            if (mesh_xform.active && !typing && !mesh_edit.uv_g)
                mesh_xform_tick(mx, my, fw, fh, wvp);
            else if (mesh_edit.tex_pick_kind != 0)
                mesh_tex_pick_hover(mx, my, fw, fh, wvp);
            else if (!imgui_overlay.wants_capture_mouse())
                mesh_update_hover(mx, my, fw, fh, wvp);
            mesh_tex_pick_highlight();
            if (mesh_edit.loopcut_on && !typing && !mesh_edit.circle_sel) {
                if (!imgui_overlay.wants_capture_mouse()) mesh_loopcut_pick(mx, my, fw, fh, wvp);
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.f) {
                    mesh_edit.cut_n += wheel > 0.f ? 1 : -1;
                    if (mesh_edit.cut_n < 1) mesh_edit.cut_n = 1;
                    if (mesh_edit.cut_n > 16) mesh_edit.cut_n = 16;
                }
            }
            if (mesh_edit.circle_sel && !typing) {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.f) {
                    mesh_edit.circle_r *= wheel > 0.f ? 1.12f : (1.f / 1.12f);
                    if (mesh_edit.circle_r < 8.f) mesh_edit.circle_r = 8.f;
                    if (mesh_edit.circle_r > 360.f) mesh_edit.circle_r = 360.f;
                }
                const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                const bool shift_c = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                if (lmb && !mesh_edit.uv_win_focused && !imgui_overlay.wants_capture_mouse())
                    mesh_circle_paint(mx, my, fw, fh, wvp, shift_c);
                else if (!lmb)
                    mesh_edit.circle_stroke_cap = false;
            }
        }
        draw_mesh_overlay(fw, fh, wvp);
        draw_sco_path_overlay();
        // Terrain overlays: draped on the terrain (sampled Z per point) and on the BACKGROUND
        // draw list so ImGui panels/menus cover them instead of floating on screen.
        if (panel.current_tool() == ui::EditorTool::Terrain && edit_mode == EditMode::None) {
            ui::TerrainBrushUi brush{};
            if (panel.terrain_paint_brush(brush) && brush.brush_size_m > 0.f) {
                float bmx = 0.f, bmy = 0.f, bfw = fw, bfh = fh;
                input::cursor_to_framebuffer(window, bmx, bmy, bfw, bfh);
                if (const auto hit = camera.hit_ground_xy(bmx, bmy, fw, fh)) {
                    if (ImDrawList* dl = ImGui::GetBackgroundDrawList()) {
                        constexpr int kSeg = 64;
                        ImVec2 rim[kSeg];
                        bool ok = true;
                        for (int i = 0; i < kSeg; ++i) {
                            const float a = (2.f * 3.14159265f * static_cast<float>(i)) / kSeg;
                            const float wx = hit->x + std::cos(a) * brush.brush_size_m;
                            const float wy = hit->y + std::sin(a) * brush.brush_size_m;
                            const float wz = sample_terrain_editor_xy(wx, wy) + 0.06f;
                            float sx = 0.f, sy = 0.f;
                            if (!world_xy_to_screen(wx, wy, wz, fw, fh, wvp, sx, sy)) {
                                ok = false;
                                break;
                            }
                            rim[i] = ImVec2(sx, sy);
                        }
                        if (ok) {
                            dl->AddConvexPolyFilled(rim, kSeg, IM_COL32(216, 166, 87, 40));
                            for (int i = 0; i < kSeg; ++i)
                                dl->AddLine(rim[i], rim[(i + 1) % kSeg], IM_COL32(230, 185, 110, 210), 1.8f);
                        }
                    }
                }
            }
            // Lasso preview (draped over the terrain).
            if (panel.imgui_state().terrain_lasso && terrain_lasso_pts.size() >= 2) {
                if (ImDrawList* dl = ImGui::GetBackgroundDrawList()) {
                    ImVec2 prev{};
                    bool have = false;
                    for (const auto& p : terrain_lasso_pts) {
                        const float wz = sample_terrain_editor_xy(p.first, p.second) + 0.06f;
                        float sx = 0.f, sy = 0.f;
                        if (!world_xy_to_screen(p.first, p.second, wz, fw, fh, wvp, sx, sy)) {
                            have = false;
                            continue;
                        }
                        if (have) dl->AddLine(prev, ImVec2(sx, sy), IM_COL32(120, 220, 255, 220), 2.f);
                        prev = ImVec2(sx, sy);
                        have = true;
                    }
                    if (have && terrain_lasso_pts.size() >= 3) {
                        const auto& p0 = terrain_lasso_pts.front();
                        const float wz = sample_terrain_editor_xy(p0.first, p0.second) + 0.06f;
                        float sx0 = 0.f, sy0 = 0.f;
                        if (world_xy_to_screen(p0.first, p0.second, wz, fw, fh, wvp, sx0, sy0))
                            dl->AddLine(prev, ImVec2(sx0, sy0), IM_COL32(120, 220, 255, 140), 1.5f);
                    }
                }
            }
        }
        // Terrain vertex edit: drape small handles on the heightfield + ▲/▼ arrows on hover.
        if (panel.current_tool() == ui::EditorTool::Terrain && edit_mode == EditMode::None &&
            panel.imgui_state().terrain_vertex_edit && !panel.imgui_state().terrain_lasso) {
            float vmx = 0.f, vmy = 0.f, vfw = fw, vfh = fh;
            input::cursor_to_framebuffer(window, vmx, vmy, vfw, vfh);
            if (const auto hit = camera.hit_ground_xy(vmx, vmy, fw, fh)) {
                if (ImDrawList* dl = ImGui::GetBackgroundDrawList()) {
                    for (std::size_t fi = 0; fi < terrain_fields.size(); ++fi) {
                        auto& tf = terrain_fields[fi];
                        const float step = tf.size_m / 60.f;
                        const auto& heights = !tf.heights_base.empty() ? tf.heights_base : tf.heights;
                        if (heights.size() < static_cast<std::size_t>(61 * 61)) continue;
                        for (int iy = 0; iy <= 60; ++iy) {
                            for (int ix = 0; ix <= 60; ++ix) {
                                const float wx = tf.rel_x + static_cast<float>(ix) * step;
                                const float wy = tf.rel_y + static_cast<float>(iy) * step;
                                if (std::hypot(wx - hit->x, wy - hit->y) > 25.f) continue;
                                const float wz = heights[static_cast<std::size_t>(iy) * 61 + ix] + 0.04f;
                                float sx = 0.f, sy = 0.f;
                                if (!world_xy_to_screen(wx, wy, wz, fw, fh, wvp, sx, sy)) continue;
                                const bool hov = (static_cast<int>(fi) == terrain_vertex.hover_field &&
                                                  ix == terrain_vertex.hover_ix &&
                                                  iy == terrain_vertex.hover_iy);
                                dl->AddCircleFilled(ImVec2(sx, sy), hov ? 4.f : 2.f,
                                                    hov ? IM_COL32(255, 240, 90, 255)
                                                        : IM_COL32(210, 230, 255, 150));
                                if (hov) {
                                    dl->AddTriangleFilled(ImVec2(sx - 4.f, sy - 10.f), ImVec2(sx + 4.f, sy - 10.f),
                                                          ImVec2(sx, sy - 18.f), IM_COL32(120, 220, 140, 230));
                                    dl->AddTriangleFilled(ImVec2(sx - 4.f, sy + 10.f), ImVec2(sx + 4.f, sy + 10.f),
                                                          ImVec2(sx, sy + 18.f), IM_COL32(220, 130, 120, 230));
                                }
                            }
                        }
                    }
                }
            }
        }
        if (aa_join_pending) {
            const ImGuiViewport* vp2 = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(vp2->WorkPos.x + vp2->WorkSize.x * 0.5f,
                                           vp2->WorkPos.y + vp2->WorkSize.y * 0.35f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.96f);
            if (ImGui::Begin("##aa_join", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoNav)) {
                ImGui::TextUnformatted(omsi::ui::i18n::tr("aa.join_with"));
                ImGui::TextDisabled("%s P%d", autofill_path_b.from_object ? ".sco" : ".sli",
                                    autofill_path_b.path_index);
                if (ImGui::Button((std::string(omsi::ui::i18n::tr("aa.at_start")) + "##aa_start").c_str(),
                                  ImVec2(110.f, 0.f))) {
                    autofill_path_b.is_start = true;
                    aa_join_pending = false;
                    autofill_path_endpoints();
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string(omsi::ui::i18n::tr("aa.at_end")) + "##aa_end").c_str(),
                                  ImVec2(110.f, 0.f))) {
                    autofill_path_b.is_start = false;
                    aa_join_pending = false;
                    autofill_path_endpoints();
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string(omsi::ui::i18n::tr("common.cancel")) + "##aa_cancel").c_str(),
                                  ImVec2(90.f, 0.f))) {
                    aa_join_pending = false;
                    autofill_path_mode = false;
                    autofill_path_stage = 0;
                    autofill_path_a = {};
                    autofill_path_b = {};
                }
            }
            ImGui::End();
        }
        if (edit_mode == EditMode::Mesh) draw_tex_pick_window();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        float y_off = 12.f;
        auto place = [&]() {
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.f, vp->WorkPos.y + vp->WorkSize.y - y_off),
                                    ImGuiCond_Always, ImVec2(0.f, 1.f));
            ImGui::SetNextWindowBgAlpha(0.92f);
        };
        if (edit_mode == EditMode::Mesh) {
            // Only touches module state (captured from run_map_editor_app), so the side panel's
            // "Mesh .o3d" tab can call it from its own place in the frame.
            auto mesh_body = [&]() {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 4.f));
                // ---- layout helpers: n equal-width buttons per row, tooltip with the shortcut ----
                const float sp_x = ImGui::GetStyle().ItemSpacing.x;
                auto bw = [&](int n) {
                    return (std::max)(40.f, (ImGui::GetContentRegionAvail().x - sp_x * static_cast<float>(n - 1)) /
                                                static_cast<float>(n));
                };
                auto tip = [&](const char* t) {
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", t);
                };
                // toggle button: lit when active
                auto toggle_btn = [&](const char* label, bool active, float w) {
                    if (active) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    }
                    const bool hit = ImGui::Button(label, ImVec2(w, 0.f));
                    if (active) ImGui::PopStyleColor(2);
                    return hit;
                };
                {
                    const float w3 = bw(3);
                    if (ImGui::Button(omsi::ui::i18n::tr_lit("Salir mesh"), ImVec2(w3, 0.f))) try_exit_mesh();
                    tip("Esc: salir del modo mesh");
                    ImGui::SameLine();
                    if (ImGui::Button(omsi::ui::i18n::tr_lit("Undo"), ImVec2(w3, 0.f))) {
                        if (mesh_xform.active) mesh_xform_cancel();
                        else mesh_undo_face();
                    }
                    tip("Ctrl+Z");
                    ImGui::SameLine();
                    if (ImGui::Button(omsi::ui::i18n::tr_lit("Redo"), ImVec2(w3, 0.f))) mesh_redo_face();
                    tip("Ctrl+Shift+Z / Ctrl+Y");
                }
                ImGui::SeparatorText(omsi::ui::i18n::tr_lit("Modo"));
                {
                    const float w3 = bw(3);
                    if (toggle_btn("Vertices  1", mesh_edit.pick == MeshEdit::Pick::Verts, w3)) {
                        mesh_edit.pick = MeshEdit::Pick::Verts;
                        mesh_hover_clear();
                    }
                    tip("1 / V: seleccionar vertices");
                    ImGui::SameLine();
                    if (toggle_btn("Edges  2", mesh_edit.pick == MeshEdit::Pick::Edges, w3)) {
                        mesh_edit.pick = MeshEdit::Pick::Edges;
                        mesh_hover_clear();
                    }
                    tip("2: seleccionar edges");
                    ImGui::SameLine();
                    if (toggle_btn("Caras  3", mesh_edit.pick == MeshEdit::Pick::Faces, w3)) {
                        mesh_edit.pick = MeshEdit::Pick::Faces;
                        mesh_hover_clear();
                    }
                    tip("3 / F: seleccionar caras");
                }
                ImGui::TextDisabled("V:%d  E:%d  F:%d   sel %d/%d/%d", static_cast<int>(mesh_edit.verts.size()),
                                    static_cast<int>(mesh_edit.edges.size()),
                                    static_cast<int>(mesh_edit.faces.size()),
                                    static_cast<int>(mesh_edit.sel_verts.size()),
                                    static_cast<int>(mesh_edit.sel_edges.size()),
                                    static_cast<int>(mesh_edit.sel_faces.size()));
                {
                    // Viewport shading / snap as dropdowns
                    const float w2 = bw(2);
                    static const char* kShade[] = {"Solid", "Wireframe", "Render (texturas)"};
                    ImGui::SetNextItemWidth(w2);
                    int shade = mesh_edit.shading;
                    if (ImGui::Combo("##mesh_shading", &shade, kShade, IM_ARRAYSIZE(kShade)) && shade != mesh_edit.shading) {
                        mesh_edit.shading = shade;
                        mesh_refresh_preview();
                    }
                    tip("Vista: Z cambia Render > Solid > Wireframe, Shift+Z alterna wireframe");
                    ImGui::SameLine();
                    static const char* kSnap[] = {"Snap: off", "Snap: vertice", "Snap: edge", "Snap: cara"};
                    ImGui::SetNextItemWidth(w2);
                    int snap = (std::clamp)(mesh_edit.snap_mode, 0, 3);
                    if (ImGui::Combo("##mesh_snap", &snap, kSnap, IM_ARRAYSIZE(kSnap))) mesh_edit.snap_mode = snap;
                    tip("Snap: mientras movés (G) mantené Ctrl para pegar al vertice, edge o cara mas cercano");
                }
                ImGui::SetNextItemWidth(bw(2));
                ImGui::DragFloat(omsi::ui::i18n::tr_lit("Weld"), &mesh_edit.weld_m, 0.001f, 0.001f, 0.5f, "%.3f m");
                tip("Distancia a la que dos vertices se consideran el mismo");

                ImGui::SeparatorText(omsi::ui::i18n::tr_lit("Selección"));
                const bool can_desel = !mesh_edit.sel_verts.empty() || !mesh_edit.sel_edges.empty() ||
                                       !mesh_edit.sel_faces.empty() || !mesh_edit.sel_uv.empty();
                if (!can_desel) ImGui::BeginDisabled();
                const float ws4 = bw(4);
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Desel"), ImVec2(ws4, 0.f))) {
                    mesh_deselect_all();
                    panel.set_status("Deseleccionado");
                }
                tip("A: seleccionar todo / nada");
                if (!can_desel) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Más"), ImVec2(ws4, 0.f))) mesh_select_more();
                tip("Ctrl + +: agrandar seleccion");
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Menos"), ImVec2(ws4, 0.f))) mesh_select_less();
                tip("Ctrl + -: achicar seleccion");
                ImGui::SameLine();
                if (toggle_btn(omsi::ui::i18n::tr_lit("Círculo"), mesh_edit.circle_sel, ws4)) {
                    mesh_edit.circle_sel = !mesh_edit.circle_sel;
                    mesh_edit.circle_stroke_cap = false;
                    panel.set_status(mesh_edit.circle_sel ? "Circulo: LMB pinta, rueda radio, C sale"
                                                          : "Circulo OFF");
                }
                tip("C: seleccion con circulo (rueda = radio)");
                ImGui::TextDisabled("%s", omsi::ui::i18n::tr_lit("Ctrl+clic: camino mas corto  |  Shift+clic: sumar  |  L: enlazado"));

                ImGui::SeparatorText(omsi::ui::i18n::tr_lit("Cortar"));
                if (toggle_btn(omsi::ui::i18n::tr_lit("Loop cut"), mesh_edit.loopcut_on, bw(2))) {
                    mesh_knife_cancel();
                    mesh_edit.loopcut_on = !mesh_edit.loopcut_on;
                    panel.set_status(mesh_edit.loopcut_on ? "Loop cut: rueda cortes, LMB aplica" : "Loop cut OFF");
                }
                tip("Ctrl+R: loop cut (rueda = cantidad de cortes)");
                ImGui::SameLine();
                ImGui::SetNextItemWidth((std::max)(60.f, ImGui::GetContentRegionAvail().x - 60.f));
                ImGui::PushButtonRepeat(true);
                if (ImGui::InputInt(omsi::ui::i18n::tr_lit("Cortes"), &mesh_edit.cut_n, 1, 1)) {
                    if (mesh_edit.cut_n < 1) mesh_edit.cut_n = 1;
                    if (mesh_edit.cut_n > 16) mesh_edit.cut_n = 16;
                }
                ImGui::PopButtonRepeat();
                if (toggle_btn(omsi::ui::i18n::tr_lit("Knife"), mesh_edit.knife_on, bw(2))) {
                    mesh_loopcut_cancel();
                    mesh_edit.knife_on = !mesh_edit.knife_on;
                    if (!mesh_edit.knife_on) mesh_knife_cancel();
                    else {
                        mesh_edit.knife_axis = -1;
                        mesh_edit.knife_pts.clear();
                    }
                    panel.set_status(mesh_edit.knife_on ? "Knife: click, X/Y/Z, Enter" : "Knife OFF");
                }
                tip("K: cuchillo (clic puntos, X/Y/Z bloquea eje, Enter corta)");
                ImGui::SameLine();
                if (ImGui::SmallButton(omsi::ui::i18n::tr_lit("eje View"))) mesh_edit.knife_axis = -1;
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) mesh_edit.knife_axis = 0;
                ImGui::SameLine();
                if (ImGui::SmallButton("Y")) mesh_edit.knife_axis = 1;
                ImGui::SameLine();
                if (ImGui::SmallButton("Z")) mesh_edit.knife_axis = 2;
                if (mesh_edit.knife_on && mesh_edit.knife_pts.size() >= 2) {
                    ImGui::SameLine();
                    if (ImGui::Button(omsi::ui::i18n::tr_lit("Aplicar knife"))) mesh_knife_confirm();
                }

                ImGui::SeparatorText(omsi::ui::i18n::tr_lit("Geometría"));
                const char* sub_lab = mesh_edit.pick == MeshEdit::Pick::Edges ? "Subdividir edges" : "Subdividir faces";
                if (ImGui::Button(sub_lab)) mesh_subdivide_sel();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(72.f);
                ImGui::PushButtonRepeat(true);
                if (ImGui::InputInt(omsi::ui::i18n::tr_lit("Pasos"), &mesh_edit.subdiv_n, 1, 1)) {
                    if (mesh_edit.subdiv_n < 1) mesh_edit.subdiv_n = 1;
                    if (mesh_edit.subdiv_n > 8) mesh_edit.subdiv_n = 8;
                }
                ImGui::PopButtonRepeat();
                const float wg4 = bw(4);
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Extrude"), ImVec2(wg4, 0.f))) mesh_extrude_region();
                tip("E: extruir");
                ImGui::SameLine();
                const bool has_faces = !mesh_edit.sel_faces.empty();
                if (!has_faces) ImGui::BeginDisabled();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Inset"), ImVec2(wg4, 0.f)) && !mesh_xform.active) mesh_inset();
                tip("I: inset (caras)");
                if (!has_faces) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Separar"), ImVec2(wg4, 0.f))) mesh_split_sel();
                tip("Y: separar la seleccion");
                ImGui::SameLine();
                const bool can_face = mesh_edit.sel_verts.size() >= 3 || mesh_edit.sel_edges.size() >= 2;
                if (!can_face) ImGui::BeginDisabled();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Hacer cara"), ImVec2(wg4, 0.f))) mesh_bake_face();
                tip("Crea una cara con los vertices / edges seleccionados");
                if (!can_face) ImGui::EndDisabled();

                ImGui::SeparatorText(omsi::ui::i18n::tr_lit("Caras"));
                const float wf3 = bw(3);
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Borrar sel"), ImVec2(wf3, 0.f))) mesh_delete_sel();
                tip("Supr: borrar la seleccion");
                ImGui::SameLine();
                if (!has_faces) ImGui::BeginDisabled();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Flip normal"), ImVec2(wf3, 0.f)))
                    mesh_flip_faces();
                tip("Invertir la normal de las caras seleccionadas");
                if (!has_faces) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Recalc normales"), ImVec2(wf3, 0.f))) mesh_recalc_n();
                tip("Shift+N: normales hacia afuera");

                ImGui::SeparatorText("Slots de material");
                {
                    const float ws2 = bw(2);
                    if (ImGui::Button("+ Nuevo slot", ImVec2(ws2, 0.f))) mesh_slot_add();
                    tip("Crea un slot y lo activa: cada cara que crees se guarda en el slot activo.\n"
                        "Despues, al darle una textura al slot, se aplica a todos sus triangulos.");
                    ImGui::SameLine();
                    if (toggle_btn("Sin slot", mesh_edit.active_slot < 0, ws2)) mesh_edit.active_slot = -1;
                    tip("Las caras nuevas no van a ningun slot");
                    std::vector<int> slot_n(mesh_edit.slots.size(), 0);
                    for (const auto& f : mesh_edit.faces)
                        if (f.slot >= 0 && static_cast<std::size_t>(f.slot) < slot_n.size()) ++slot_n[static_cast<std::size_t>(f.slot)];
                    int slot_del = -1;
                    for (int si = 0; si < static_cast<int>(mesh_edit.slots.size()); ++si) {
                        auto& sl = mesh_edit.slots[static_cast<std::size_t>(si)];
                        ImGui::PushID(7100 + si);
                        if (ImGui::RadioButton("##act", mesh_edit.active_slot == si)) mesh_edit.active_slot = si;
                        tip("Slot activo: las caras nuevas van aca");
                        ImGui::SameLine();
                        char nbuf[64]{};
                        std::snprintf(nbuf, sizeof(nbuf), "%s", sl.name.c_str());
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.38f);
                        if (ImGui::InputText("##name", nbuf, sizeof(nbuf))) sl.name = nbuf;
                        ImGui::SameLine();
                        const std::string tp =
                            (sl.tex >= 0 && static_cast<std::size_t>(sl.tex) < mesh_edit.tex_list.size())
                                ? mesh_edit.tex_list[static_cast<std::size_t>(sl.tex)].filename().string()
                                : std::string("(sin textura)");
                        ImGui::SetNextItemWidth(-44.f);
                        if (ImGui::BeginCombo("##tex", tp.c_str())) {
                            for (int i = 0; i < static_cast<int>(mesh_edit.tex_list.size()); ++i) {
                                const std::string lab = std::to_string(i) + "  " +
                                                        mesh_edit.tex_list[static_cast<std::size_t>(i)].filename().string();
                                if (ImGui::Selectable(lab.c_str(), i == sl.tex)) mesh_slot_set_tex(si, i);
                            }
                            if (mesh_edit.tex_list.empty()) ImGui::TextDisabled("Agrega texturas abajo");
                            ImGui::EndCombo();
                        }
                        tip("Textura del slot: se aplica a todos sus triangulos");
                        ImGui::SameLine();
                        ImGui::TextDisabled("%d", slot_n[static_cast<std::size_t>(si)]);
                        if (ImGui::SmallButton("Seleccionar")) mesh_slot_select(si, ImGui::GetIO().KeyShift);
                        tip("Selecciona las caras del slot (Shift: agrega a la seleccion)");
                        ImGui::SameLine();
                        if (!has_faces) ImGui::BeginDisabled();
                        if (ImGui::SmallButton("Mover seleccion aca")) mesh_slot_assign_sel(si);
                        tip("Las caras seleccionadas pasan a este slot (y toman su textura)");
                        if (!has_faces) ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (mesh_edit.tex_sel >= 0 && static_cast<std::size_t>(mesh_edit.tex_sel) < mesh_edit.tex_list.size()) {
                            if (ImGui::SmallButton("Tex actual")) mesh_slot_set_tex(si, mesh_edit.tex_sel);
                            tip("Le da al slot la textura elegida en 'Tex'");
                            ImGui::SameLine();
                        }
                        if (ImGui::SmallButton("Borrar")) slot_del = si;
                        tip("Borra el slot (sus caras quedan sin slot, con su textura)");
                        ImGui::PopID();
                    }
                    if (slot_del >= 0) mesh_slot_delete(slot_del);
                }

                ImGui::SeparatorText(omsi::ui::i18n::tr_lit("Textura / UV"));
                if (!mesh_edit.tex_list.empty()) {
                    int shown_mat = mesh_edit.tex_sel;
                    bool mixed = false;
                    if (has_faces) {
                        shown_mat = -1;
                        for (int fi : mesh_edit.sel_faces) {
                            if (fi < 0 || static_cast<std::size_t>(fi) >= mesh_edit.faces.size()) continue;
                            const int m = mesh_edit.faces[static_cast<std::size_t>(fi)].mat;
                            if (shown_mat < 0) shown_mat = m;
                            else if (m != shown_mat) mixed = true;
                        }
                        if (shown_mat < 0) shown_mat = mesh_edit.tex_sel;
                    }
                    std::string preview = mixed ? "(varias)" : "";
                    if (!mixed) {
                        if (shown_mat >= 0 && static_cast<std::size_t>(shown_mat) < mesh_edit.tex_list.size())
                            preview = mesh_edit.tex_list[static_cast<std::size_t>(shown_mat)].filename().string();
                        else
                            preview = "(sin tex)";
                    }
                    if (ImGui::BeginCombo(omsi::ui::i18n::tr_lit("Tex"), preview.c_str())) {
                        for (int i = 0; i < static_cast<int>(mesh_edit.tex_list.size()); ++i) {
                            const bool sel = i == mesh_edit.tex_sel;
                            const std::string lab = std::to_string(i) + "  " +
                                                    mesh_edit.tex_list[static_cast<std::size_t>(i)].filename().string();
                            if (ImGui::Selectable(lab.c_str(), sel)) mesh_assign_sel_mat(i);
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::Button("Assign")) mesh_assign_sel_mat(mesh_edit.tex_sel);
                    ImGui::SameLine();
                    if (ImGui::Button(omsi::ui::i18n::tr_lit("Select"))) {
                        mesh_sel_cap();
                        mesh_edit.pick = MeshEdit::Pick::Faces;
                        for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi)
                            if (mesh_edit.faces[static_cast<std::size_t>(fi)].mat == mesh_edit.tex_sel)
                                mesh_edit.sel_faces.insert(fi);
                        mesh_uv_sync_from_3d();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Deselect")) {
                        mesh_sel_cap();
                        for (auto it = mesh_edit.sel_faces.begin(); it != mesh_edit.sel_faces.end();) {
                            const int fi = *it;
                            if (fi >= 0 && static_cast<std::size_t>(fi) < mesh_edit.faces.size() &&
                                mesh_edit.faces[static_cast<std::size_t>(fi)].mat == mesh_edit.tex_sel)
                                it = mesh_edit.sel_faces.erase(it);
                            else
                                ++it;
                        }
                        mesh_uv_sync_from_3d();
                    }
                    if (has_faces && !mixed)
                        ImGui::TextDisabled("cara: %s", preview.c_str());
                    else if (has_faces)
                        ImGui::TextDisabled(omsi::ui::i18n::tr_lit("cara: varias texturas"));
                } else {
                    ImGui::TextDisabled(omsi::ui::i18n::tr_lit("sin texturas"));
                }
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Agregar textura…"))) mesh_tex_browse();
                ImGui::SameLine();
                if (ImGui::Button(mesh_edit.tex_pick_kind ? omsi::ui::i18n::tr_lit("Pick textura [ON]") : omsi::ui::i18n::tr_lit("Pick textura .sco/.sli"))) {
                    if (mesh_edit.tex_pick_kind) mesh_edit.tex_pick_kind = 0;
                    else ImGui::OpenPopup("##tex_pick_kind");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(omsi::ui::i18n::tr_lit("Hover resalta en amarillo, click selecciona (rojo) y lista sus texturas"));
                if (ImGui::BeginPopup("##tex_pick_kind")) {
                    ImGui::TextDisabled(omsi::ui::i18n::tr_lit("Elegir texturas de:"));
                    if (ImGui::Selectable(".sco  (objeto)")) {
                        mesh_edit.tex_pick_kind = 1;
                        panel.set_status("Pick textura: click sobre un .sco (Esc cancela)");
                    }
                    if (ImGui::Selectable(".sli  (spline)")) {
                        mesh_edit.tex_pick_kind = 2;
                        panel.set_status("Pick textura: click sobre un .sli (Esc cancela)");
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Reload"))) {
                    for (int m = 0; m < static_cast<int>(mesh_edit.tex_list.size()); ++m)
                        renderer.invalidate_texture_path(mesh_tex_path_at(m));
                    mesh_edit.force_tex = true;
                    mesh_edit.uv_srv_ok = false;
                    mesh_refresh_preview();
                }
                ImGui::SameLine();
                if (ImGui::Button(mesh_edit.uv_editor_open ? omsi::ui::i18n::tr_lit("Cerrar UV") : omsi::ui::i18n::tr_lit("UV Editor")))
                    mesh_edit.uv_editor_open = !mesh_edit.uv_editor_open;
                if (!has_faces) ImGui::BeginDisabled();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Izq"))) {
                    mesh_uv_nudge(-0.05f, 0.f);
                    mesh_refresh_preview();
                }
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Der"))) {
                    mesh_uv_nudge(0.05f, 0.f);
                    mesh_refresh_preview();
                }
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Arriba"))) {
                    mesh_uv_nudge(0.f, -0.05f);
                    mesh_refresh_preview();
                }
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Abajo"))) {
                    mesh_uv_nudge(0.f, 0.05f);
                    mesh_refresh_preview();
                }
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Centrar"))) {
                    mesh_uv_center();
                    mesh_refresh_preview();
                }
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Zoom+"))) {
                    mesh_uv_zoom(1.1f);
                    mesh_refresh_preview();
                }
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Zoom-"))) {
                    mesh_uv_zoom(1.f / 1.1f);
                    mesh_refresh_preview();
                }
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("UV Cube"))) mesh_cube_uv_sel();
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Flip X"))) {
                    mesh_uv_flip(true, false);
                    mesh_refresh_preview();
                }
                ImGui::SameLine();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Flip Y"))) {
                    mesh_uv_flip(false, true);
                    mesh_refresh_preview();
                }
                if (!has_faces) ImGui::EndDisabled();

                ImGui::SeparatorText(omsi::ui::i18n::tr_lit("Export"));
                ImGui::SetNextItemWidth(140.f);
                ImGui::InputText(omsi::ui::i18n::tr_lit("Nombre bake"), mesh_edit.bake_name, IM_ARRAYSIZE(mesh_edit.bake_name));
                const bool can_sco = !mesh_edit.faces.empty();
                if (!can_sco) ImGui::BeginDisabled();
                if (ImGui::Button(omsi::ui::i18n::tr_lit("Bake .sco"))) mesh_bake_sco();
                if (!can_sco) ImGui::EndDisabled();
                if (!mesh_edit.o3d_edit_path.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button(omsi::ui::i18n::tr_lit("Guardar .o3d"))) mesh_save_o3d();
                    ImGui::TextDisabled("%s", mesh_edit.o3d_edit_path.filename().string().c_str());
                }
                ImGui::PopStyleVar(2);
            };
            mesh_edit_body_fn = mesh_body;
            if (!mesh_tab_hosted) {
                place();
                if (ImGui::Begin("mesh_edit", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoNav))
                    mesh_body();
                ImGui::End();
            }
            mesh_edit.uv_win_focused = false;
            mesh_edit.uv_canvas_hov = false;
            if (mesh_edit.uv_editor_open) {
                if (mesh_edit.uv_wheel_dirty && glfwGetTime() - mesh_edit.uv_wheel_t > 0.2) {
                    mesh_edit.uv_wheel_dirty = false;
                    mesh_edit.uv_wheel_capped = false;
                }
                mesh_uv_bind_tex();
                const float dpi = ImGui::GetIO().DisplayFramebufferScale.x;
                const float base = 512.f * (dpi > 0.1f ? dpi : 1.f);
                ImGui::SetNextWindowSize(ImVec2(base + 24.f, base + 96.f), ImGuiCond_FirstUseEver);
                if (ImGui::Begin(omsi::ui::i18n::tr_lit("UV Editor"), &mesh_edit.uv_editor_open)) {
                    mesh_edit.uv_win_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
                    if (ImGui::RadioButton(omsi::ui::i18n::tr_lit("Verts"), mesh_edit.pick == MeshEdit::Pick::Verts)) {
                        mesh_edit.pick = MeshEdit::Pick::Verts;
                        mesh_hover_clear();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Edges", mesh_edit.pick == MeshEdit::Pick::Edges)) {
                        mesh_edit.pick = MeshEdit::Pick::Edges;
                        mesh_hover_clear();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Faces", mesh_edit.pick == MeshEdit::Pick::Faces)) {
                        mesh_edit.pick = MeshEdit::Pick::Faces;
                        mesh_hover_clear();
                    }
                    if (!mesh_edit.uv_srv_ok) {
                        ImGui::SameLine();
                        ImGui::TextDisabled(omsi::ui::i18n::tr_lit("sin textura"));
                    }
                    if (mesh_edit.sel_faces.empty() && mesh_edit.sel_uv.empty())
                        ImGui::TextDisabled(omsi::ui::i18n::tr_lit("UV: seleccioná caras en 3D (Faces)"));
                    ImGui::Text("mat %d  zoom %.2f", mesh_uv_mat(), static_cast<double>(mesh_edit.uv_zoom));
                    if (ImGui::Button(omsi::ui::i18n::tr_lit("UV Cube"))) mesh_cube_uv_sel();
                    ImGui::SameLine();
                    if (ImGui::Button(omsi::ui::i18n::tr_lit("Flip X"))) {
                        mesh_uv_flip(true, false);
                        mesh_refresh_preview();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(omsi::ui::i18n::tr_lit("Flip Y"))) {
                        mesh_uv_flip(false, true);
                        mesh_refresh_preview();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Zoom+##uv")) {
                        mesh_uv_zoom(1.1f);
                        mesh_refresh_preview();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Zoom-##uv")) {
                        mesh_uv_zoom(1.f / 1.1f);
                        mesh_refresh_preview();
                    }
                    const ImVec2 canvas = ImGui::GetCursorScreenPos();
                    const float sz = base * mesh_edit.uv_zoom;
                    ImDrawList* udl = ImGui::GetWindowDrawList();
                    const ImVec2 p0(canvas.x + mesh_edit.uv_pan_x, canvas.y + mesh_edit.uv_pan_y);
                    const ImVec2 p1(p0.x + sz, p0.y + sz);
                    if (mesh_edit.uv_g) mesh_uv_g_apply(p0.x, p0.y, sz);
                    if (mesh_edit.uv_srv_ok)
                        udl->AddImage(static_cast<ImTextureID>(mesh_edit.uv_srv_gpu), p0, p1);
                    else
                        udl->AddRectFilled(p0, p1, IM_COL32(40, 40, 44, 255));
                    udl->AddRect(p0, p1, IM_COL32(90, 90, 98, 255));
                    auto uv_to = [&](float u, float v) { return ImVec2(p0.x + u * sz, p0.y + v * sz); };
                    const ImU32 col_uv_wire = IM_COL32(70, 78, 90, 220);
                    const ImU32 col_uv_sel = IM_COL32(255, 122, 10, 255);
                    const ImU32 col_uv_hov = IM_COL32(255, 200, 70, 255);
                    const ImU32 col_uv_vert = IM_COL32(18, 18, 22, 255);
                    const ImU32 col_uv_face_fill = IM_COL32(210, 55, 55, 42);
                    const ImU32 col_uv_face_ln = IM_COL32(230, 70, 70, 230);
                    for (int i = 0; i < static_cast<int>(mesh_edit.faces.size()); ++i) {
                        const auto& f = mesh_edit.faces[static_cast<std::size_t>(i)];
                        if (!mesh_uv_face_shown(i)) continue;
                        const ImVec2 a = uv_to(f.u0, f.v0), b = uv_to(f.u1, f.v1), c = uv_to(f.u2, f.v2);
                        const bool fsel = mesh_edit.sel_faces.count(i) != 0;
                        const bool fhov = mesh_edit.uv_hover_f == i;
                        if (fsel) udl->AddTriangleFilled(a, b, c, col_uv_face_fill);
                        udl->AddTriangle(a, b, c, fsel ? col_uv_face_ln : (fhov ? col_uv_hov : col_uv_wire),
                                         fsel ? 2.4f : (fhov ? 2.0f : 1.15f));
                        const float us[3] = {f.u0, f.u1, f.u2};
                        const float vs[3] = {f.v0, f.v1, f.v2};
                        for (int k = 0; k < 3; ++k) {
                            const int lp0 = i * 3 + k;
                            const int lp1 = i * 3 + ((k + 1) % 3);
                            const bool esel = mesh_edit.sel_uv.count(lp0) && mesh_edit.sel_uv.count(lp1);
                            const bool ehov = mesh_edit.uv_hover == lp0 && mesh_edit.pick == MeshEdit::Pick::Edges;
                            if (esel || ehov)
                                udl->AddLine(uv_to(us[k], vs[k]), uv_to(us[(k + 1) % 3], vs[(k + 1) % 3]),
                                             ehov ? col_uv_hov : col_uv_sel, esel ? 3.4f : 2.6f);
                        }
                        for (int k = 0; k < 3; ++k) {
                            const int lp = i * 3 + k;
                            const bool vsel = mesh_edit.sel_uv.count(lp) != 0;
                            const bool vhov = mesh_edit.uv_hover == lp;
                            const ImVec2 p = uv_to(us[k], vs[k]);
                            const float r = (vsel || vhov) ? 5.2f : 3.4f;
                            udl->AddCircleFilled(p, r, vsel ? col_uv_sel : (vhov ? col_uv_hov : col_uv_vert));
                            udl->AddCircle(p, r + 1.1f, IM_COL32(230, 230, 235, 220), 12, 1.1f);
                        }
                    }
                    ImGui::InvisibleButton("uv_canvas", ImVec2(base, base));
                    const bool hov = ImGui::IsItemHovered();
                    mesh_edit.uv_canvas_hov = hov;
                    const ImVec2 mp = ImGui::GetIO().MousePos;
                    float uu = 0.f, vv = 0.f;
                    if (sz > 1.f) {
                        uu = (mp.x - p0.x) / sz;
                        vv = (mp.y - p0.y) / sz;
                    }
                    mesh_edit.uv_hover = -1;
                    mesh_edit.uv_hover_f = -1;
                    auto uv_d2 = [&](float u, float v) {
                        const float dx = p0.x + u * sz - mp.x;
                        const float dy = p0.y + v * sz - mp.y;
                        return dx * dx + dy * dy;
                    };
                    const float uv_pick_r2 = 64.f;
                    if (hov && mesh_edit.pick == MeshEdit::Pick::Verts) {
                        float best = uv_pick_r2;
                        for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
                            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                            if (!mesh_uv_face_shown(fi)) continue;
                            float us[3] = {f.u0, f.u1, f.u2};
                            float vs[3] = {f.v0, f.v1, f.v2};
                            for (int c = 0; c < 3; ++c) {
                                const float d = uv_d2(us[c], vs[c]);
                                if (d < best) {
                                    best = d;
                                    mesh_edit.uv_hover = fi * 3 + c;
                                }
                            }
                        }
                    } else if (hov && mesh_edit.pick == MeshEdit::Pick::Edges) {
                        float best = uv_pick_r2;
                        auto segd = [&](float u0, float v0, float u1, float v1) {
                            const float ax = p0.x + u0 * sz, ay = p0.y + v0 * sz;
                            const float bx = p0.x + u1 * sz, by = p0.y + v1 * sz;
                            const float den = (std::max)(1.f, (bx - ax) * (bx - ax) + (by - ay) * (by - ay));
                            const float t = (std::max)(
                                0.f, (std::min)(1.f, ((mp.x - ax) * (bx - ax) + (mp.y - ay) * (by - ay)) / den));
                            const float px = ax + t * (bx - ax), py = ay + t * (by - ay);
                            return (px - mp.x) * (px - mp.x) + (py - mp.y) * (py - mp.y);
                        };
                        for (int fi = 0; fi < static_cast<int>(mesh_edit.faces.size()); ++fi) {
                            const auto& f = mesh_edit.faces[static_cast<std::size_t>(fi)];
                            if (!mesh_uv_face_shown(fi)) continue;
                            float us[3] = {f.u0, f.u1, f.u2};
                            float vs[3] = {f.v0, f.v1, f.v2};
                            for (int c = 0; c < 3; ++c) {
                                const float d = segd(us[c], vs[c], us[(c + 1) % 3], vs[(c + 1) % 3]);
                                if (d < best) {
                                    best = d;
                                    mesh_edit.uv_hover = fi * 3 + c;
                                }
                            }
                        }
                    } else if (hov) {
                        mesh_edit.uv_hover_f = mesh_uv_pick_face(uu, vv);
                    }
                    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (mesh_edit.circle_sel && hov) {
                        udl->AddCircle(mp, mesh_edit.circle_r, IM_COL32(255, 255, 255, 210), 64, 1.5f);
                        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                            mesh_circle_paint_uv(mp.x, mp.y, p0.x, p0.y, sz, shift);
                    } else if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (mesh_edit.uv_g) {
                            mesh_uv_g_end(false);
                        } else if (mesh_edit.pick == MeshEdit::Pick::Faces) {
                            if (mesh_edit.uv_hover_f < 0) mesh_uv_sel_clear();
                            else {
                                const int fi = mesh_edit.uv_hover_f;
                                const int lp0 = fi * 3, lp1 = fi * 3 + 1, lp2 = fi * 3 + 2;
                                const bool was = mesh_edit.sel_uv.count(lp0) && mesh_edit.sel_uv.count(lp1) &&
                                                 mesh_edit.sel_uv.count(lp2);
                                mesh_sel_cap();
                                if (!shift) mesh_edit.sel_uv.clear();
                                if (was && !shift) {
                                    mesh_edit.sel_uv.erase(lp0);
                                    mesh_edit.sel_uv.erase(lp1);
                                    mesh_edit.sel_uv.erase(lp2);
                                    mesh_edit.last_uv = -1;
                                } else {
                                    mesh_edit.sel_uv.insert(lp0);
                                    mesh_edit.sel_uv.insert(lp1);
                                    mesh_edit.sel_uv.insert(lp2);
                                    mesh_edit.last_uv = lp0;
                                }
                            }
                        } else if (mesh_edit.pick == MeshEdit::Pick::Verts) {
                            if (mesh_edit.uv_hover < 0) mesh_uv_sel_clear();
                            else
                                mesh_click_id(mesh_edit.sel_uv, mesh_edit.uv_hover, mesh_edit.last_uv, shift, true);
                        } else {
                            if (mesh_edit.uv_hover < 0) mesh_uv_sel_clear();
                            else {
                                const int fi = mesh_edit.uv_hover / 3;
                                const int c = mesh_edit.uv_hover % 3;
                                const int lp0 = fi * 3 + c;
                                const int lp1 = fi * 3 + ((c + 1) % 3);
                                const bool was = mesh_edit.sel_uv.count(lp0) && mesh_edit.sel_uv.count(lp1);
                                mesh_sel_cap();
                                if (!shift) mesh_edit.sel_uv.clear();
                                if (was && !shift) {
                                    mesh_edit.sel_uv.erase(lp0);
                                    mesh_edit.sel_uv.erase(lp1);
                                } else {
                                    mesh_edit.sel_uv.insert(lp0);
                                    mesh_edit.sel_uv.insert(lp1);
                                }
                            }
                        }
                    }
                    if (hov && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                        mesh_edit.uv_pan_x += ImGui::GetIO().MouseDelta.x;
                        mesh_edit.uv_pan_y += ImGui::GetIO().MouseDelta.y;
                    }
                    if (hov && ImGui::GetIO().MouseWheel != 0.f && !mesh_edit.circle_sel) {
                        const float wh = ImGui::GetIO().MouseWheel;
                        const bool shift_uv = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (!shift_uv) {
                            mesh_edit.uv_pan_y += wh * 36.f;
                        } else {
                            if (!mesh_edit.uv_wheel_capped) mesh_edit.uv_wheel_capped = true;
                            const float old_z = mesh_edit.uv_zoom;
                            mesh_edit.uv_zoom *= wh > 0.f ? 1.1f : (1.f / 1.1f);
                            if (mesh_edit.uv_zoom < 0.2f) mesh_edit.uv_zoom = 0.2f;
                            if (mesh_edit.uv_zoom > 16.f) mesh_edit.uv_zoom = 16.f;
                            const float k = mesh_edit.uv_zoom / old_z;
                            mesh_edit.uv_pan_x = (mp.x - canvas.x) - k * (mp.x - canvas.x - mesh_edit.uv_pan_x);
                            mesh_edit.uv_pan_y = (mp.y - canvas.y) - k * (mp.y - canvas.y - mesh_edit.uv_pan_y);
                            mesh_edit.uv_wheel_dirty = true;
                            mesh_edit.uv_wheel_t = glfwGetTime();
                        }
                    }
                }
                ImGui::End();
            } else if (mesh_edit.uv_g) {
                mesh_uv_g_end(true);
            }
            {
                const auto aff = mesh_collect_affected();
                float lx = 0.f, ly = 0.f, lz = 0.f;
                if (!aff.empty()) {
                    std::vector<float> xs, ys, zs;
                    xs.reserve(aff.size());
                    ys.reserve(aff.size());
                    zs.reserve(aff.size());
                    for (int i : aff) {
                        const auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
                        xs.push_back(v.x);
                        ys.push_back(v.y);
                        zs.push_back(v.z);
                    }
                    auto mid = [](std::vector<float>& a) {
                        std::sort(a.begin(), a.end());
                        return a[a.size() / 2];
                    };
                    lx = mid(xs);
                    ly = mid(ys);
                    lz = mid(zs);
                }
                (void)vp;
                ui::float_slot_begin("mesh_item", 15); // docked with the other small windows
                ImGui::SetNextWindowSize(ImVec2(256.f, 210.f), ImGuiCond_Always);
                const bool item_drawn = ImGui::Begin("Item", nullptr,
                                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav |
                                                         ImGuiWindowFlags_NoSavedSettings);
                ui::float_slot_end();
                if (item_drawn) {
                    ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("Transform  (median sel)"));
                    float loc[3] = {lx, ly, lz};
                    static bool loc_cap = false;
                    if (ImGui::DragFloat3(omsi::ui::i18n::tr_lit("Location"), loc, 0.01f) && !aff.empty() && !mesh_xform.active) {
                        if (!loc_cap) {
                            mesh_geo_cap();
                            loc_cap = true;
                        }
                        const float dx = loc[0] - lx, dy = loc[1] - ly, dz = loc[2] - lz;
                        for (int i : aff) {
                            auto& v = mesh_edit.verts[static_cast<std::size_t>(i)];
                            v.x += dx;
                            v.y += dy;
                            v.z += dz;
                        }
                        mesh_refresh_preview();
                    }
                    if (!ImGui::IsItemActive()) loc_cap = false;
                    float rot[3] = {0.f, 0.f, 0.f};
                    if (mesh_xform.active && mesh_xform.mode == 1) {
                        if (mesh_xform.axis == 0) rot[0] = mesh_xform.disp_angle;
                        else if (mesh_xform.axis == 1) rot[1] = mesh_xform.disp_angle;
                        else if (mesh_xform.axis == 2) rot[2] = mesh_xform.disp_angle;
                        else rot[2] = mesh_xform.disp_angle;
                    }
                    ImGui::BeginDisabled(true);
                    ImGui::DragFloat3(omsi::ui::i18n::tr_lit("Rotation"), rot, 0.1f);
                    ImGui::EndDisabled();
                    float scv[3] = {1.f, 1.f, 1.f};
                    if (mesh_xform.active && mesh_xform.mode == 2) {
                        if (mesh_xform.axis == 0) scv[0] = mesh_xform.disp_scale;
                        else if (mesh_xform.axis == 1) scv[1] = mesh_xform.disp_scale;
                        else if (mesh_xform.axis == 2) scv[2] = mesh_xform.disp_scale;
                        else scv[0] = scv[1] = scv[2] = mesh_xform.disp_scale;
                    }
                    ImGui::BeginDisabled(true);
                    ImGui::DragFloat3(omsi::ui::i18n::tr_lit("Scale"), scv, 0.01f);
                    ImGui::EndDisabled();
                    if (mesh_xform.active && mesh_xform.mode == 3)
                        ImGui::Text("Inset  %.3f m", mesh_xform.inset_thick);
                    ImGui::TextDisabled(omsi::ui::i18n::tr_lit("R/S: Ctrl=paso  X/Y/Z eje"));
                }
                ImGui::End();
            }
            {
                const ImGuiViewport* vp2 = ImGui::GetMainViewport();
                const float h = mesh_edit.shortcuts_collapsed ? 32.f : 168.f;
                ImGui::SetNextWindowPos(ImVec2(vp2->WorkPos.x + 12.f, vp2->WorkPos.y + 48.f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(280.f, h), ImGuiCond_Always);
                if (ImGui::Begin(omsi::ui::i18n::tr_lit("Atajos mesh"), nullptr,
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoFocusOnAppearing)) {
                    if (ImGui::SmallButton(mesh_edit.shortcuts_collapsed ? "Atajos mesh  [+]" : "[-]"))
                        mesh_edit.shortcuts_collapsed = !mesh_edit.shortcuts_collapsed;
                    if (!mesh_edit.shortcuts_collapsed) {
                        ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("1/2/3 vertices/edges/caras  Ctrl+clic camino"));
                        ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("Z vista Render/Solid/Wire  Shift+Z wireframe"));
                        ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("Numpad 1/3/7 (+Ctrl) vistas  5 orto  9 opuesto  2/4/6/8 girar"));
                        ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("A sel (UV: solo esta tex)  C circulo"));
                        ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("V/E/F pick+snap UV  Ctrl G: borde tex"));
                        ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("G/R/S  R eje bbox  Ctrl+R 90  Shift+rueda zoom"));
                        ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("Rueda pan UV  Esc cancela G/R/S"));
                        ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("Y split caras  Shift+D dup (edges)"));
                    }
                }
                ImGui::End();
            }
            if (mesh_edit.tex_browser_open) {
                ImGui::OpenPopup("MeshTexBrowser");
                ImGui::SetNextWindowSize(ImVec2(620.f, 520.f), ImGuiCond_FirstUseEver);
            }
            if (ImGui::BeginPopupModal("MeshTexBrowser", &mesh_edit.tex_browser_open, ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("Agregar textura"));
                ImGui::Separator();
                std::error_code ec;
                if (mesh_edit.tex_browser_cwd.empty() ||
                    !std::filesystem::is_directory(mesh_edit.tex_browser_cwd, ec))
                    mesh_edit.tex_browser_cwd = cfg.omsi_root / "Sceneryobjects";
                auto path_u8 = [](const std::filesystem::path& p) -> std::string {
                    try {
                        const auto u8 = p.u8string();
                        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
                    } catch (const std::filesystem::filesystem_error&) {
                        return {};
                    }
                };
                std::filesystem::path want_root = cfg.omsi_root / "Sceneryobjects";
                {
                    std::error_code iec;
                    if (!std::filesystem::is_directory(want_root, iec)) want_root = mesh_edit.tex_browser_cwd;
                    const auto cwd_n = mesh_edit.tex_browser_cwd.lexically_normal();
                    const auto root_n = want_root.lexically_normal();
                    auto under = [](const std::wstring& child, const std::wstring& root) {
                        if (root.empty() || child.size() < root.size()) return false;
                        if (child.compare(0, root.size(), root) != 0) return false;
                        if (child.size() == root.size()) return true;
                        const wchar_t sep = child[root.size()];
                        return sep == L'\\' || sep == L'/';
                    };
                    if (!under(cwd_n.native(), root_n.native()) && cwd_n.native() != root_n.native())
                        want_root = cwd_n;
                }
                ui::browser_index_ensure(want_root);
                char cwd_buf[512]{};
                const std::string cwd_utf8 = path_u8(mesh_edit.tex_browser_cwd);
                std::snprintf(cwd_buf, sizeof(cwd_buf), "%s", cwd_utf8.c_str());
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::InputText("Ruta", cwd_buf, sizeof(cwd_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    const std::filesystem::path p = cwd_buf;
                    if (std::filesystem::is_directory(p, ec)) mesh_edit.tex_browser_cwd = p.lexically_normal();
                }
                if (ImGui::Button("Ir")) {
                    const std::filesystem::path p = cwd_buf;
                    if (std::filesystem::is_directory(p, ec)) mesh_edit.tex_browser_cwd = p.lexically_normal();
                }
                ImGui::SameLine();
                if (ImGui::Button("..") && mesh_edit.tex_browser_cwd.has_parent_path())
                    mesh_edit.tex_browser_cwd = mesh_edit.tex_browser_cwd.parent_path();
                ImGui::SameLine();
                if (ImGui::Button("Cancelar")) {
                    mesh_edit.tex_browser_open = false;
                    ImGui::CloseCurrentPopup();
                }
                struct TexRow {
                    bool dir = false;
                    std::filesystem::path path;
                    std::string label;
                };
                std::vector<TexRow> rows;
                {
                    std::error_code lec;
                    std::filesystem::directory_iterator it(
                        mesh_edit.tex_browser_cwd, std::filesystem::directory_options::skip_permission_denied, lec);
                    const std::filesystem::directory_iterator end;
                    for (; it != end; it.increment(lec)) {
                        if (lec) {
                            lec.clear();
                            continue;
                        }
                        try {
                            TexRow r;
                            r.path = it->path();
                            r.label = path_u8(r.path.filename());
                            std::error_code iec;
                            if (it->is_directory(iec) && !iec) {
                                r.dir = true;
                                rows.push_back(std::move(r));
                            } else if (it->is_regular_file(iec) && !iec && mesh_tex_ext_ok(r.path)) {
                                rows.push_back(std::move(r));
                            }
                        } catch (const std::filesystem::filesystem_error&) {
                            continue;
                        }
                    }
                }
                std::sort(rows.begin(), rows.end(), [](const TexRow& a, const TexRow& b) {
                    if (a.dir != b.dir) return a.dir > b.dir;
                    return a.label < b.label;
                });
                ImGui::SetNextItemWidth(-1.f);
                ImGui::InputTextWithHint("##texfil", omsi::ui::i18n::tr_lit("Buscar por nombre..."), mesh_edit.tex_browser_filter,
                                         IM_ARRAYSIZE(mesh_edit.tex_browser_filter));
                if (ui::browser_index_busy() || !ui::browser_index_ready(want_root))
                    ImGui::TextUnformatted(omsi::ui::i18n::tr_lit("Indexando…"));
                std::string filt = mesh_edit.tex_browser_filter;
                for (char& c : filt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                int match_budget = 0;
                auto tex_ok = [&](const std::filesystem::path& p) { return mesh_tex_ext_ok(p); };
                const bool idx_ready = ui::browser_index_ready(want_root);
                ++mesh_edit.tex_thumb_clock;
                mesh_edit.tex_thumb_loads = 0;
                if (ImGui::BeginChild("mesh_tex_rows", ImVec2(-1.f, 340.f), true)) {
                    for (const auto& r : rows) {
                        if (!filt.empty() && !ui::browser_name_matches(r.label, filt)) {
                            if (!(r.dir && idx_ready &&
                                  ui::browser_dir_has_match(r.path, filt, tex_ok, 0, match_budget)))
                                continue;
                        }
                        ImGui::PushID(path_u8(r.path).c_str());
                        const float thumb = 56.f;
                        if (!r.dir) {
                            const ImVec2 tp = ImGui::GetCursorScreenPos();
                            std::uint64_t gpu = 0;
                            if (ImGui::IsRectVisible(tp, ImVec2(tp.x + thumb, tp.y + thumb)))
                                gpu = mesh_tex_thumb(r.path);
                            if (gpu)
                                ImGui::Image(static_cast<ImTextureID>(gpu), ImVec2(thumb, thumb));
                            else
                                ImGui::Dummy(ImVec2(thumb, thumb));
                            ImGui::GetWindowDrawList()->AddRect(tp, ImVec2(tp.x + thumb, tp.y + thumb),
                                                                IM_COL32(90, 90, 98, 255));
                            ImGui::SameLine();
                        } else {
                            ImGui::Dummy(ImVec2(thumb, thumb));
                            ImGui::SameLine();
                        }
                        const std::string lab = r.dir ? ("[dir] " + r.label) : r.label;
                        if (ImGui::Selectable(lab.c_str(), false, 0, ImVec2(0.f, thumb))) {
                            if (r.dir) {
                                mesh_edit.tex_browser_cwd = r.path.lexically_normal();
                            } else {
                                const std::size_t before = mesh_edit.tex_list.size();
                                if (mesh_tex_append_path(r.path)) {
                                    if (before == 0) {
                                        mesh_geo_cap();
                                        for (auto& f : mesh_edit.faces) f.mat = -1;
                                    }
                                    mesh_assign_sel_mat(mesh_edit.tex_sel);
                                }
                                mesh_edit.tex_browser_open = false;
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }
    };
