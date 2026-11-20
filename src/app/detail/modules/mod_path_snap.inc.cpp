// ============================================================================
// MOD: path_snap � aislado. No importar funciones de otros modulos.
// Editar solo aqui para alineacion path lateral / path align grab.
// ============================================================================
    auto centerline_at_path_pick = [&](const PathEndpointPick& pick, float& cx, float& cy, float& cz,
                                       float& ch) -> bool {
        if (!pick.valid) return false;
        if (pick.from_object) {
            cx = pick.x;
            cy = pick.y;
            cz = pick.z;
            ch = pick.heading_deg;
            return true;
        }
        for (const auto& s : edit_splines) {
            if (s.id != pick.owner_id) continue;
            const float along = pick.is_start ? 0.f : s.length;
            const auto c = spline::evaluate_spline(s, along, 0.f, 0.f);
            cx = c.x;
            cy = c.y;
            cz = c.z;
            ch = c.heading_rad * (180.f / 3.14159265f);
            return true;
        }
        return false;
    };
    // Path endpoint ? centerline anchor (keep lateral offset; do not place spline origin on path point).
    auto centerline_anchor_for_path_pick = [&](const map::SplineInst& proto, const PathEndpointPick& pick,
                                                 float& ax, float& ay, float& az, float& ah) -> bool {
        if (!pick.valid) return false;
        if (pick.from_object) return centerline_at_path_pick(pick, ax, ay, az, ah);
        const auto def = spline::load_sli(proto.sli_path);
        if (!def || pick.path_index < 0 ||
            static_cast<std::size_t>(pick.path_index) >= def->paths.size())
            return centerline_at_path_pick(pick, ax, ay, az, ah);
        const auto& pp = def->paths[static_cast<std::size_t>(pick.path_index)];
        const map::SplineInst& tmp = proto;
        const float along = pick.is_start ? 0.f : tmp.length;
        const auto path_p = spline::evaluate_spline(tmp, along, pp.lateral, pp.height);
        const auto ctr_p = spline::evaluate_spline(tmp, along, 0.f, 0.f);
        ax = pick.x - (path_p.x - ctr_p.x);
        ay = pick.y - (path_p.y - ctr_p.y);
        az = pick.z - (path_p.z - ctr_p.z);
        ah = pick.heading_deg;
        return true;
    };
    auto normalize_heading_deg = [](float h) {
        while (h > 180.f) h -= 360.f;
        while (h < -180.f) h += 360.f;
        return h;
    };
    // Snap target centerline yaw: path/spline start -> opposite; end -> same as owner centerline.
    auto snap_centerline_heading_for_pick = [&](const PathEndpointPick& pick) -> float {
        float cx = 0.f, cy = 0.f, cz = 0.f, ch = 0.f;
        if (!centerline_at_path_pick(pick, cx, cy, cz, ch)) ch = pick.heading_deg;
        return normalize_heading_deg(ch + (pick.is_start ? 180.f : 0.f));
    };
    auto snap_spline_path_to_pick = [&](map::SplineInst& s, int path_idx, const PathEndpointPick& pick,
                                        bool preview_only = true) -> bool {
        if (!pick.valid) return false;
        const auto def = spline::load_sli(s.sli_path);
        if (!def || path_idx < 0 || static_cast<std::size_t>(path_idx) >= def->paths.size()) return false;
        const auto& pp = def->paths[static_cast<std::size_t>(path_idx)];
        s.heading_deg = snap_centerline_heading_for_pick(pick);
        // Always align the start of path N to the target endpoint under the mouse.
        const auto samp = spline::evaluate_spline(s, 0.f, pp.lateral, pp.height);
        s.position.x += pick.x - samp.x;
        s.position.y += pick.y - samp.y;
        s.position.z += pick.z - samp.z;
        set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
        remesh_spline(s, preview_only);
        return true;
    };
    auto snap_spline_path_end_to_pick = [&](map::SplineInst& s, int path_idx, const PathEndpointPick& pick,
                                            bool preview_only = true) -> bool {
        if (!pick.valid) return false;
        const auto def = spline::load_sli(s.sli_path);
        if (!def || path_idx < 0 || static_cast<std::size_t>(path_idx) >= def->paths.size()) return false;
        const auto& pp = def->paths[static_cast<std::size_t>(path_idx)];
        s.heading_deg = snap_centerline_heading_for_pick(pick);
        const auto samp = spline::evaluate_spline(s, s.length, pp.lateral, pp.height);
        s.position.x += pick.x - samp.x;
        s.position.y += pick.y - samp.y;
        s.position.z += pick.z - samp.z;
        set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
        remesh_spline(s, preview_only);
        return true;
    };
    auto object_path_endpoint_world = [&](const map::MapObjectInst& o, int path_idx, bool is_start, float& ox,
                                        float& oy, float& oz, float& oh) -> bool {
        const auto sco = sco::load_sco(o.sco_path);
        if (!sco || path_idx < 0 || static_cast<std::size_t>(path_idx) >= sco->paths.size()) return false;
        const auto& path = sco->paths[static_cast<std::size_t>(path_idx)];
        map::SplineInst mini{};
        mini.position.x = path.x;
        mini.position.y = path.y;
        mini.position.z = path.z;
        mini.heading_deg = path.heading_deg;
        mini.radius = path.radius;
        mini.length = (std::max)(0.05f, std::abs(path.length));
        const float th = terrain_add(sco::skip_terrain_add(*sco), o.position.x, o.position.y);
        const XMMATRIX world_m = sco_world_matrix(o, nullptr, th, 0.f, 0.f);
        const auto local = spline::evaluate_spline(mini, is_start ? 0.f : mini.length);
        DirectX::XMFLOAT3 w{};
        DirectX::XMStoreFloat3(&w, xform_point(world_m, local.x, local.y, local.z));
        log_sco_path_placement(o, *sco, path, path_idx, is_start, th, w.x, w.y, w.z, "snap");
        ox = w.x;
        oy = w.y;
        oz = w.z;
        oh = local.heading_rad * (180.f / 3.14159265f);
        return true;
    };
    auto snap_object_path_to_pick = [&](map::MapObjectInst& o, int path_idx, const PathEndpointPick& pick) -> bool {
        if (!pick.valid) return false;
        float ox = 0.f, oy = 0.f, oz = 0.f, oh = 0.f;
        if (!object_path_endpoint_world(o, path_idx, true, ox, oy, oz, oh)) return false;
        (void)oh;
        o.position.x += pick.x - ox;
        o.position.y += pick.y - oy;
        o.position.z += pick.z - oz;
        o.rotation_deg.x = snap_centerline_heading_for_pick(pick);
        sync_object_gpu(o);
        return true;
    };
    int path_align_path_index = 0;
    int path_align_hover_marker = -1;
    auto path_align_path_count = [&]() -> int {
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
            const auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
            if (const auto def = spline::load_sli(s.sli_path))
                return static_cast<int>((std::max)(std::size_t{1}, def->paths.size()));
        } else if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size()) {
            const auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
            if (const auto sco = sco::load_sco(o.sco_path))
                return static_cast<int>((std::max)(std::size_t{1}, sco->paths.size()));
        }
        return 1;
    };
    auto apply_path_align_grab = [&](const PathEndpointPick& pick) {
        if (!pick.valid) return;
        const int pi = path_align_path_index;
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
            auto& s = edit_splines[static_cast<std::size_t>(selected_spline)];
            if (snap_spline_path_to_pick(s, pi, pick)) sync_all_end_markers();
        } else if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size()) {
            auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
            if (snap_object_path_to_pick(o, pi, pick)) sync_all_end_markers();
        }
    };