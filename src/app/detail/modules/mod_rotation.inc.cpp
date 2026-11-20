// MOD: rotation � aislado (R / Ctrl+R yaw).

    struct HudGroupPose {
        bool valid = false;
        float pivot_x = 0.f;
        float pivot_y = 0.f;
        float anchor_yaw = 0.f;
        std::vector<int> kind;
        std::vector<std::int32_t> ids;
        std::vector<float> off_x;
        std::vector<float> off_y;
        std::vector<float> start_yaw;
    };
    HudGroupPose hud_group_pose;

    auto capture_hud_group_pose = [&]() {
        hud_group_pose.valid = !group_grab_ids.empty();
        hud_group_pose.pivot_x = group_pivot_x;
        hud_group_pose.pivot_y = group_pivot_y;
        hud_group_pose.anchor_yaw = group_start_yaw.empty() ? 0.f : group_start_yaw.front();
        hud_group_pose.kind = group_grab_kind;
        hud_group_pose.ids = group_grab_ids;
        hud_group_pose.off_x = group_off_x;
        hud_group_pose.off_y = group_off_y;
        hud_group_pose.start_yaw = group_start_yaw;
    };

    auto apply_exact_yaw = [&](float abs_yaw) {
        auto wrap_delta = [](float d) {
            while (d > 180.f) d -= 360.f;
            while (d < -180.f) d += 360.f;
            return d;
        };
        if (hud_group_pose.valid && !hud_group_pose.ids.empty()) {
            const float delta = wrap_delta(abs_yaw - hud_group_pose.anchor_yaw);
            const float rad = delta * (3.14159265f / 180.f);
            const float c = std::cos(rad);
            const float s = std::sin(rad);
            for (std::size_t i = 0; i < hud_group_pose.ids.size(); ++i) {
                const float ox = i < hud_group_pose.off_x.size() ? hud_group_pose.off_x[i] : 0.f;
                const float oy = i < hud_group_pose.off_y.size() ? hud_group_pose.off_y[i] : 0.f;
                const float rx = ox * c + oy * s;
                const float ry = -ox * s + oy * c;
                const int kind = i < hud_group_pose.kind.size() ? hud_group_pose.kind[i] : 1;
                if (kind == 0) {
                    for (auto& o : edit_objects) {
                        if (o.id != hud_group_pose.ids[i]) continue;
                        o.position.x = hud_group_pose.pivot_x + rx;
                        o.position.y = hud_group_pose.pivot_y + ry;
                        if (i < hud_group_pose.start_yaw.size()) o.rotation_deg.x = hud_group_pose.start_yaw[i] + delta;
                        sync_object_gpu(o);
                        break;
                    }
                } else {
                    for (auto& sp : edit_splines) {
                        if (sp.id != hud_group_pose.ids[i]) continue;
                        sp.position.x = hud_group_pose.pivot_x + rx;
                        sp.position.y = hud_group_pose.pivot_y + ry;
                        if (i < hud_group_pose.start_yaw.size()) sp.heading_deg = hud_group_pose.start_yaw[i] + delta;
                        set_spline_chunk_offset(sp.id, 0.f, 0.f, 0.f);
                        remesh_spline(sp);
                        break;
                    }
                }
            }
            return;
        }
        float delta = wrap_delta(abs_yaw - grab_start_yaw);
        if (group_rotate_active && !group_grab_index.empty()) {
            const float rad = delta * (3.14159265f / 180.f);
            const float c = std::cos(rad);
            const float s = std::sin(rad);
            for (std::size_t i = 0; i < group_grab_index.size(); ++i) {
                const float ox = group_off_x[i];
                const float oy = group_off_y[i];
                const float rx = ox * c + oy * s;
                const float ry = -ox * s + oy * c;
                if (group_grab_kind[i] == 0) {
                    auto& o = edit_objects[static_cast<std::size_t>(group_grab_index[i])];
                    o.position.x = group_pivot_x + rx;
                    o.position.y = group_pivot_y + ry;
                    if (i < group_start_yaw.size()) o.rotation_deg.x = group_start_yaw[i] + delta;
                    sync_object_gpu(o);
                } else {
                    auto& sp = edit_splines[static_cast<std::size_t>(group_grab_index[i])];
                    sp.position.x = group_pivot_x + rx;
                    sp.position.y = group_pivot_y + ry;
                    if (i < group_start_yaw.size()) sp.heading_deg = group_start_yaw[i] + delta;
                    set_spline_chunk_offset(sp.id, 0.f, 0.f, 0.f);
                    remesh_spline(sp);
                }
            }
            return;
        }
        if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size()) {
            auto& o = edit_objects[static_cast<std::size_t>(selected_object)];
            o.rotation_deg.x = abs_yaw;
            sync_object_gpu(o);
            show_object_at(static_cast<std::size_t>(selected_object));
        } else if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
            auto& sp = edit_splines[static_cast<std::size_t>(selected_spline)];
            sp.heading_deg = abs_yaw;
            after_rotate_spline_ids({sp.id}, false, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
            show_spline_at(static_cast<std::size_t>(selected_spline));
        }
    };