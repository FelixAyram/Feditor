// ============================================================================
// MOD autofill path (doble-A)
// Path-only autofill — zero shared code with single-A module.
// ============================================================================
    auto path2_centerline_at_path_pick = [&](const PathEndpointPick& pick, float& cx, float& cy, float& cz,
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
    auto path2_centerline_anchor_for_path_pick = [&](const map::SplineInst& proto, const PathEndpointPick& pick,
                                                       float& ax, float& ay, float& az, float& ah) -> bool {
        if (!pick.valid) return false;
        if (pick.from_object) return path2_centerline_at_path_pick(pick, ax, ay, az, ah);
        const auto def = spline::load_sli(proto.sli_path);
        if (!def || pick.path_index < 0 ||
            static_cast<std::size_t>(pick.path_index) >= def->paths.size())
            return path2_centerline_at_path_pick(pick, ax, ay, az, ah);
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
    auto path2_normalize_heading = [](float h) {
        while (h > 180.f) h -= 360.f;
        while (h < -180.f) h += 360.f;
        return h;
    };
    auto path2_centerline_heading_at_pick = [&](const PathEndpointPick& pick) -> float {
        float cx = 0.f, cy = 0.f, cz = 0.f, ch = 0.f;
        if (!path2_centerline_at_path_pick(pick, cx, cy, cz, ch)) ch = pick.heading_deg;
        return path2_normalize_heading(ch);
    };
    // G+Ctrl placement snap: start -> opposite, end -> same tangent.
    auto path2_snap_heading_for_pick = [&](const PathEndpointPick& pick) -> float {
        return path2_normalize_heading(path2_centerline_heading_at_pick(pick) + (pick.is_start ? 180.f : 0.f));
    };
    // Path picks store evaluated path tangent; centerline picks use spline evaluate.
    auto path2_path_heading_at_pick = [&](const PathEndpointPick& pick) -> float {
        if (pick.path_index >= 0) return path2_normalize_heading(pick.heading_deg);
        return path2_centerline_heading_at_pick(pick);
    };
    // Autofill depart from pick_a: along endpoint tangent (not spawn-flip).
    auto path2_autofill_depart_heading = [&](const PathEndpointPick& pick) -> float {
        return path2_path_heading_at_pick(pick);
    };
    // Autofill aim at pick_b: face the endpoint (>< at two ends): start -> same; end -> opposite.
    auto path2_autofill_aim_heading = [&](const PathEndpointPick& pick) -> float {
        return path2_normalize_heading(path2_path_heading_at_pick(pick) + (pick.is_start ? 0.f : 180.f));
    };
    auto path2_snap_spline_path_to_pick = [&](map::SplineInst& s, int path_idx, const PathEndpointPick& pick,
                                              bool preview_only = false) -> bool {
        if (!pick.valid) return false;
        const auto def = spline::load_sli(s.sli_path);
        if (!def || path_idx < 0 || static_cast<std::size_t>(path_idx) >= def->paths.size()) return false;
        const auto& pp = def->paths[static_cast<std::size_t>(path_idx)];
        const auto samp = spline::evaluate_spline(s, 0.f, pp.lateral, pp.height);
        s.position.x += pick.x - samp.x;
        s.position.y += pick.y - samp.y;
        s.position.z += pick.z - samp.z;
        float dh = pick.heading_deg - samp.heading_rad * (180.f / 3.14159265f);
        while (dh > 180.f) dh -= 360.f;
        while (dh < -180.f) dh += 360.f;
        s.heading_deg += dh;
        set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
        remesh_spline(s, preview_only);
        return true;
    };
    auto path2_snap_spline_path_end_to_pick = [&](map::SplineInst& s, int path_idx, const PathEndpointPick& pick,
                                                  bool preview_only = false) -> bool {
        if (!pick.valid) return false;
        const auto def = spline::load_sli(s.sli_path);
        if (!def || path_idx < 0 || static_cast<std::size_t>(path_idx) >= def->paths.size()) return false;
        const auto& pp = def->paths[static_cast<std::size_t>(path_idx)];
        const auto samp = spline::evaluate_spline(s, s.length, pp.lateral, pp.height);
        s.position.x += pick.x - samp.x;
        s.position.y += pick.y - samp.y;
        s.position.z += pick.z - samp.z;
        float dh = pick.heading_deg - samp.heading_rad * (180.f / 3.14159265f);
        while (dh > 180.f) dh -= 360.f;
        while (dh < -180.f) dh += 360.f;
        s.heading_deg += dh;
        set_spline_chunk_offset(s.id, 0.f, 0.f, 0.f);
        remesh_spline(s, preview_only);
        return true;
    };
    auto path2_lock_spline_start_to_pose = [&](map::SplineInst& s, float px, float py, float pz, float h_deg) {
        const auto samp = spline::evaluate_spline(s, 0.f);
        s.position.x += px - samp.x;
        s.position.y += py - samp.y;
        s.position.z += pz - samp.z;
        float dh = h_deg - samp.heading_rad * (180.f / 3.14159265f);
        while (dh > 180.f) dh -= 360.f;
        while (dh < -180.f) dh += 360.f;
        s.heading_deg += dh;
    };
    auto path2_snap_path_end_in_memory = [&](map::SplineInst& s, int path_idx, const PathEndpointPick& pick) -> bool {
        if (!pick.valid) return false;
        const auto def = spline::load_sli(s.sli_path);
        if (!def || path_idx < 0 || static_cast<std::size_t>(path_idx) >= def->paths.size()) return false;
        const auto& pp = def->paths[static_cast<std::size_t>(path_idx)];
        const auto samp = spline::evaluate_spline(s, s.length, pp.lateral, pp.height);
        s.position.x += pick.x - samp.x;
        s.position.y += pick.y - samp.y;
        s.position.z += pick.z - samp.z;
        float dh = pick.heading_deg - samp.heading_rad * (180.f / 3.14159265f);
        while (dh > 180.f) dh -= 360.f;
        while (dh < -180.f) dh += 360.f;
        s.heading_deg += dh;
        return true;
    };
    auto path2_snap_path_start_in_memory = [&](map::SplineInst& s, int path_idx, const PathEndpointPick& pick) -> bool {
        if (!pick.valid) return false;
        const auto def = spline::load_sli(s.sli_path);
        if (!def || path_idx < 0 || static_cast<std::size_t>(path_idx) >= def->paths.size()) return false;
        const auto& pp = def->paths[static_cast<std::size_t>(path_idx)];
        const auto samp = spline::evaluate_spline(s, 0.f, pp.lateral, pp.height);
        s.position.x += pick.x - samp.x;
        s.position.y += pick.y - samp.y;
        s.position.z += pick.z - samp.z;
        float dh = pick.heading_deg - samp.heading_rad * (180.f / 3.14159265f);
        while (dh > 180.f) dh -= 360.f;
        while (dh < -180.f) dh += 360.f;
        s.heading_deg += dh;
        return true;
    };
    auto path2_snap_path_end_position_only = [&](map::SplineInst& s, int path_idx, const PathEndpointPick& pick) -> bool {
        if (!pick.valid) return false;
        const auto def = spline::load_sli(s.sli_path);
        if (!def || path_idx < 0 || static_cast<std::size_t>(path_idx) >= def->paths.size()) return false;
        const auto& pp = def->paths[static_cast<std::size_t>(path_idx)];
        const auto samp = spline::evaluate_spline(s, s.length, pp.lateral, pp.height);
        s.position.x += pick.x - samp.x;
        s.position.y += pick.y - samp.y;
        s.position.z += pick.z - samp.z;
        return true;
    };
    auto path2_snap_path_start_position_only = [&](map::SplineInst& s, int path_idx,
                                                   const PathEndpointPick& pick) -> bool {
        if (!pick.valid) return false;
        const auto def = spline::load_sli(s.sli_path);
        if (!def || path_idx < 0 || static_cast<std::size_t>(path_idx) >= def->paths.size()) return false;
        const auto& pp = def->paths[static_cast<std::size_t>(path_idx)];
        const auto samp = spline::evaluate_spline(s, 0.f, pp.lateral, pp.height);
        s.position.x += pick.x - samp.x;
        s.position.y += pick.y - samp.y;
        s.position.z += pick.z - samp.z;
        return true;
    };
    auto path2_apply_autofill_seg_fields = [&](map::SplineInst& dst, const map::SplineInst& seg) {
        dst.length = seg.length;
        dst.radius = seg.radius;
        dst.delta_height = seg.delta_height;
        dst.use_delta_height = seg.use_delta_height;
        dst.is_spline_h = seg.is_spline_h;
        dst.start_grad = seg.start_grad;
        dst.end_grad = seg.end_grad;
        dst.skew_start = seg.skew_start;
        dst.skew_end = seg.skew_end;
    };
    auto path2_compute_biarc_omsi = [&](float x0, float y0, float h0_deg, float x1, float y1, float h1_deg,
                                        float soft, float& L0, float& R0, float& L1, float& R1, float& jx, float& jy,
                                        float& hj_deg) -> bool {
        constexpr float kDeg = 3.14159265f / 180.f;
        auto wrap180 = [](float a) {
            while (a > 180.f) a -= 360.f;
            while (a < -180.f) a += 360.f;
            return a;
        };
        float t0x = std::sin(h0_deg * kDeg), t0y = std::cos(h0_deg * kDeg);
        float t1x = std::sin(h1_deg * kDeg), t1y = std::cos(h1_deg * kDeg);
        const float vx = x1 - x0, vy = y1 - y0;
        const float dist = std::sqrt(vx * vx + vy * vy);
        if (dist < 0.05f) return false;

        auto solve_ab = [&](float ax, float ay, float bx, float by, float& a, float& b) -> bool {
            const float det = ax * by - ay * bx;
            if (std::fabs(det) < 1.e-6f) return false;
            a = (vx * by - vy * bx) / det;
            b = (ax * vy - ay * vx) / det;
            return true;
        };

        float a = 0.f, b = 0.f;
        bool ok_ab = solve_ab(t0x, t0y, t1x, t1y, a, b) && a > 0.05f && b > 0.05f;
        if (!ok_ab) {
            t1x = -t1x;
            t1y = -t1y;
            h1_deg = wrap180(h1_deg + 180.f);
            ok_ab = solve_ab(t0x, t0y, t1x, t1y, a, b) && a > 0.05f && b > 0.05f;
        }
        if (!ok_ab) {
            jx = 0.5f * (x0 + x1);
            jy = 0.5f * (y0 + y1);
            const float mx = -vy / dist, my = vx / dist;
            const float chord_h = std::atan2(vx, vy) / kDeg;
            const float alpha = wrap180(chord_h - h0_deg);
            const float beta = wrap180(h1_deg - chord_h);
            const float offs = 0.38f * dist * std::tan(0.5f * (alpha + beta) * kDeg);
            jx += mx * offs;
            jy += my * offs;
        } else {
            a *= soft;
            b *= soft;
            const float Qx = x0 + a * t0x;
            const float Qy = y0 + a * t0y;
            const float d0 = std::sqrt((Qx - x1) * (Qx - x1) + (Qy - y1) * (Qy - y1));
            const float d1 = std::sqrt((Qx - x0) * (Qx - x0) + (Qy - y0) * (Qy - y0));
            const float d2 = dist;
            const float s = d0 + d1 + d2;
            if (s < 1.e-3f) return false;
            jx = (d0 * x0 + d1 * x1 + d2 * Qx) / s;
            jy = (d0 * y0 + d1 * y1 + d2 * Qy) / s;
        }

        spline::fit_spline_to_xy(x0, y0, h0_deg, jx, jy, L0, R0);
        if (!(L0 > 0.25f) || !std::isfinite(L0) || !std::isfinite(R0)) return false;
        map::SplineInst tmp{};
        tmp.position.x = x0;
        tmp.position.y = y0;
        tmp.position.z = 0.f;
        tmp.heading_deg = h0_deg;
        tmp.length = L0;
        tmp.radius = R0;
        const auto mid = spline::evaluate_spline(tmp, L0);
        jx = mid.x;
        jy = mid.y;
        hj_deg = mid.heading_rad / kDeg;

        if (!spline::fit_spline_to_end_heading(jx, jy, hj_deg, x1, y1, h1_deg, L1, R1)) return false;
        if (!(L1 > 0.05f) || !std::isfinite(L1) || !std::isfinite(R1)) return false;
        tmp.position.x = jx;
        tmp.position.y = jy;
        tmp.heading_deg = hj_deg;
        tmp.length = L1;
        tmp.radius = R1;
        const auto end = spline::evaluate_spline(tmp, L1);
        const float err_p = std::sqrt((end.x - x1) * (end.x - x1) + (end.y - y1) * (end.y - y1));
        const float err_h = std::fabs(wrap180(end.heading_rad / kDeg - h1_deg));
        return err_p < 0.35f && err_h < 45.f;
    };

    auto path2_between = [&](map::SplineInst& src, const PathEndpointPick& pick_a, const PathEndpointPick& pick_b,
                             AutofillAttach attach) {
        if (!pick_a.valid || !pick_b.valid) {
            log::warn("autofill path: invalid picks");
            return;
        }
        // Always replace src when pick_a is on it (same as single-A): depart from path START.
        const bool replace_src = !pick_a.from_object && pick_a.owner_id == src.id;
        PathEndpointPick depart_pick = pick_a;
        if (replace_src) {
            depart_pick.is_start = true;
            if (depart_pick.path_index >= 0) {
                if (const auto def = spline::load_sli(src.sli_path)) {
                    if (static_cast<std::size_t>(depart_pick.path_index) < def->paths.size()) {
                        const auto& pp = def->paths[static_cast<std::size_t>(depart_pick.path_index)];
                        const auto path_p = spline::evaluate_spline(src, 0.f, pp.lateral, pp.height);
                        depart_pick.x = path_p.x;
                        depart_pick.y = path_p.y;
                        depart_pick.z = path_p.z;
                        depart_pick.heading_deg = path_p.heading_rad * (180.f / 3.14159265f);
                    }
                }
            } else {
                const auto c = spline::evaluate_spline(src, 0.f);
                depart_pick.x = c.x;
                depart_pick.y = c.y;
                depart_pick.z = c.z;
                depart_pick.heading_deg = c.heading_rad * (180.f / 3.14159265f);
            }
        }
        const bool prepend = replace_src || attach == AutofillAttach::PrependStart;

        // Path pick -> centerline for connecting SLI (src lateral), not target-spline centerline.
        float lat = 0.f, path_height = 0.f;
        if (depart_pick.path_index >= 0) {
            if (const auto def = spline::load_sli(src.sli_path)) {
                if (static_cast<std::size_t>(depart_pick.path_index) < def->paths.size()) {
                    lat = def->paths[static_cast<std::size_t>(depart_pick.path_index)].lateral;
                    path_height = def->paths[static_cast<std::size_t>(depart_pick.path_index)].height;
                }
            }
        }
        if (src.mirror) lat = -lat;
        float path_height_b = path_height;
        if (pick_b.path_index >= 0 && !pick_b.from_object && pick_b.owner_id != src.id) {
            for (const auto& es : edit_splines) {
                if (es.id == pick_b.owner_id) {
                    if (const auto bdef = spline::load_sli(es.sli_path)) {
                        if (static_cast<std::size_t>(pick_b.path_index) < bdef->paths.size())
                            path_height_b = bdef->paths[static_cast<std::size_t>(pick_b.path_index)].height;
                    }
                    break;
                }
            }
        }
        const int conn_path_idx = (pick_a.path_index >= 0) ? pick_a.path_index : 0;
        constexpr float kDegP = 3.14159265f / 180.f;
        constexpr float kRad2 = 180.f / 3.14159265f;
        const float h0 = path2_autofill_depart_heading(depart_pick);
        const float target_heading_deg = path2_autofill_aim_heading(pick_b);
        // Connector SLI path lateral (src). Centerline targets derived from path picks.
        const float depart_rad = h0 * kDegP;
        const float aim_rad = target_heading_deg * kDegP;
        const float sx0 = depart_pick.x - lat * std::cos(depart_rad);
        const float sy0 = depart_pick.y + lat * std::sin(depart_rad);
        const float sz0 = depart_pick.z - path_height;
        const float tx = pick_b.x - lat * std::cos(aim_rad);
        const float ty = pick_b.y + lat * std::sin(aim_rad);
        const float tz = pick_b.z - path_height_b;

        log::info("autofill path: start_ctr=(" + std::to_string(sx0) + "," + std::to_string(sy0) + ") h0=" +
                  std::to_string(h0) + " end_ctr=(" + std::to_string(tx) + "," + std::to_string(ty) +
                  ") aim_h=" + std::to_string(target_heading_deg));
        log::info("autofill path: pick_a_path=(" + std::to_string(pick_a.x) + "," + std::to_string(pick_a.y) + ")" +
                  " pick_b_path=(" + std::to_string(pick_b.x) + "," + std::to_string(pick_b.y) + ")");

        const float dist = std::sqrt((tx - sx0) * (tx - sx0) + (ty - sy0) * (ty - sy0));
        if (dist < 0.05f) {
            log::warn("autofill path: target too close to start (dist=" + std::to_string(dist) + ")");
            return;
        }

        auto path2_apply_z_ramp = [&](std::vector<map::SplineInst>& segs) {
            if (segs.empty()) return;
            const int n = static_cast<int>(segs.size());
            std::vector<float> lens(static_cast<std::size_t>(n), 0.f);
            float total = 0.f;
            for (int i = 0; i < n; ++i) {
                lens[static_cast<std::size_t>(i)] = (std::max)(0.5f, segs[static_cast<std::size_t>(i)].length);
                total += lens[static_cast<std::size_t>(i)];
            }
            if (total < 1.e-3f) total = 1.f;
            std::vector<float> z(static_cast<std::size_t>(n + 1), 0.f);
            z[0] = sz0;
            float acc = 0.f;
            for (int i = 0; i < n; ++i) {
                acc += lens[static_cast<std::size_t>(i)];
                z[static_cast<std::size_t>(i + 1)] = sz0 + (tz - sz0) * (acc / total);
            }
            z[static_cast<std::size_t>(n)] = tz;
            for (int pass = 0; pass < 48; ++pass) {
                std::vector<float> nz = z;
                for (int i = 1; i < n; ++i) {
                    const float l0 = lens[static_cast<std::size_t>(i - 1)];
                    const float l1 = lens[static_cast<std::size_t>(i)];
                    const float w0 = 1.f / l0;
                    const float w1 = 1.f / l1;
                    nz[static_cast<std::size_t>(i)] =
                        (z[static_cast<std::size_t>(i - 1)] * w0 + z[static_cast<std::size_t>(i + 1)] * w1) /
                        (w0 + w1);
                }
                nz[0] = sz0;
                nz[static_cast<std::size_t>(n)] = tz;
                z.swap(nz);
            }
            for (int i = 0; i < n; ++i) {
                auto& s = segs[static_cast<std::size_t>(i)];
                const float z0 = z[static_cast<std::size_t>(i)];
                const float z1 = z[static_cast<std::size_t>(i + 1)];
                const float L = lens[static_cast<std::size_t>(i)];
                const float dz = z1 - z0;
                s.position.z = z0;
                s.delta_height = dz;
                s.use_delta_height = std::fabs(dz) > 0.01f;
                s.is_spline_h = s.use_delta_height;

                float g0 = 0.f;
                float g1 = 0.f;
                if (n == 1 && s.use_delta_height) {
                    g0 = 0.f;
                    g1 = 0.f;
                } else if (s.use_delta_height) {
                    g0 = 100.f * dz / L;
                    g1 = g0;
                    if (i > 0) {
                        const float Lp = lens[static_cast<std::size_t>(i - 1)];
                        const float zp = z[static_cast<std::size_t>(i - 1)];
                        const float prev_mean = 100.f * (z0 - zp) / Lp;
                        const float cur_mean = 100.f * dz / L;
                        g0 = 0.5f * (prev_mean + cur_mean);
                    }
                    if (i + 1 < n) {
                        const float Ln = lens[static_cast<std::size_t>(i + 1)];
                        const float zn = z[static_cast<std::size_t>(i + 2)];
                        const float cur_mean = 100.f * dz / L;
                        const float next_mean = 100.f * (zn - z1) / Ln;
                        g1 = 0.5f * (cur_mean + next_mean);
                    }
                    if (i == 0) {
                        if (attach == AutofillAttach::AppendEnd)
                            g0 = src.end_grad;
                        else if (attach == AutofillAttach::PrependStart || attach == AutofillAttach::ReplaceStart)
                            g0 = src.start_grad;
                    }
                    if (i + 1 == n) g1 = 0.f;
                }
                s.start_grad = g0;
                s.end_grad = g1;
            }
            for (int i = 0; i + 1 < n; ++i) {
                const float joint = 0.5f * (segs[static_cast<std::size_t>(i)].end_grad +
                                           segs[static_cast<std::size_t>(i + 1)].start_grad);
                segs[static_cast<std::size_t>(i)].end_grad = joint;
                segs[static_cast<std::size_t>(i + 1)].start_grad = joint;
            }
        };

        struct Cand {
            std::vector<map::SplineInst> segs;
            float path_ep = 1.e9f;
            float path_eh = 1.e9f;
        };
        Cand best{};

        auto wrap180 = [](float a) {
            while (a > 180.f) a -= 360.f;
            while (a < -180.f) a += 360.f;
            return a;
        };
        const float aim_h = target_heading_deg;

        auto path2_path_err_on = [&](const map::SplineInst& s, float& ep, float& ez, float& eh) {
            ep = ez = eh = 1.e9f;
            if (const auto def = spline::load_sli(s.sli_path)) {
                if (static_cast<std::size_t>(conn_path_idx) < def->paths.size()) {
                    const auto& pp = def->paths[static_cast<std::size_t>(conn_path_idx)];
                    const auto e = spline::evaluate_spline(s, s.length, pp.lateral, pp.height);
                    ep = std::sqrt((e.x - pick_b.x) * (e.x - pick_b.x) + (e.y - pick_b.y) * (e.y - pick_b.y));
                    ez = std::fabs(e.z - pick_b.z);
                    eh = std::fabs(wrap180(e.heading_rad * kRad2 - pick_b.heading_deg));
                }
            }
        };
        auto path2_score_cand = [&](const std::vector<map::SplineInst>& trial, float& ep, float& eh) -> float {
            ep = eh = 1.e9f;
            if (trial.empty()) return 1.e9f;
            float ez = 0.f;
            path2_path_err_on(trial.back(), ep, ez, eh);
            return ep + 0.02f * eh;
        };

        // 1. Single arc (fixed start anchor).
        {
            float L = 0.f, R = 0.f;
            if (spline::fit_spline_to_end_heading(sx0, sy0, h0, tx, ty, aim_h, L, R) && L >= 0.05f) {
                map::SplineInst one = src;
                one.position.x = sx0;
                one.position.y = sy0;
                one.position.z = sz0;
                one.heading_deg = h0;
                one.length = L;
                one.radius = R;
                float ep = 0.f, eh = 0.f;
                const float score = path2_score_cand({one}, ep, eh);
                if (score < path2_score_cand(best.segs, best.path_ep, best.path_eh) ||
                    best.segs.empty()) {
                    best.segs = {one};
                    best.path_ep = ep;
                    best.path_eh = eh;
                }
            }
        }

        // 2. Multi-segment N=2..10 — always try (N>2 blocked before by early biarc filling best).
            constexpr float kRadP      = 180.f / 3.14159265f;
            constexpr int   max_segs   = 10;

            // Build the BiarcInterp guide biarc
            const float t0x = std::sin(h0 * kDegP), t0y = std::cos(h0 * kDegP);
            const float t1x = std::sin(aim_h * kDegP), t1y = std::cos(aim_h * kDegP);
            const float vx = tx - sx0, vy = ty - sy0;
            const float vDotV = vx * vx + vy * vy;
            const float den2 = 2.f * (1.f - (t0x * t1x + t0y * t1y));
            const float vDotT = vx * (t0x + t1x) + vy * (t0y + t1y);
            float d = 0.f; bool pm_ok = false;
            if (std::fabs(den2) < 1.e-6f) {
                const float vdt1 = vx * t1x + vy * t1y;
                if (std::fabs(vdt1) > 1.e-6f) { d = vDotV / (4.f * vdt1); pm_ok = true; }
            } else {
                const float disc = vDotT * vDotT + den2 * vDotV;
                if (disc >= 0.f) { d = (-vDotT + std::sqrt(disc)) / den2; pm_ok = true; }
            }

            if (pm_ok && std::isfinite(d)) {
                const float pmx = 0.5f * (sx0 + tx + d * (t0x - t1x));
                const float pmy = 0.5f * (sy0 + ty + d * (t0y - t1y));

                float L0g = 0.f, R0g = 0.f, L1g = 0.f, R1g = 0.f;
                spline::fit_spline_to_xy(sx0, sy0, h0, pmx, pmy, L0g, R0g);
                map::SplineInst g1 = src;
                g1.position.x = sx0; g1.position.y = sy0; g1.position.z = sz0;
                g1.heading_deg = h0; g1.length = L0g; g1.radius = R0g;
                const auto gm = spline::evaluate_spline(g1, L0g);
                if (!spline::fit_spline_to_end_heading(gm.x, gm.y, gm.heading_rad * kRadP, tx, ty, aim_h, L1g,
                                                       R1g))
                    L1g = 0.f;
                map::SplineInst g2 = src;
                g2.position.x = gm.x; g2.position.y = gm.y; g2.position.z = gm.z;
                g2.heading_deg = gm.heading_rad * kRadP; g2.length = L1g; g2.radius = R1g;

                if (L0g >= 0.25f && L1g >= 0.25f && std::isfinite(L0g) && std::isfinite(L1g)) {
                    const float total_Lg = L0g + L1g;

                    // Helper: build a fresh BiarcInterp biarc for any sub-segment.
                    // Guarantees end position (tx1,ty1) AND end heading (h1_d).
                    auto make_biarc = [&](float x0b, float y0b, float z0b, float h0b,
                                          float x1b, float y1b, float h1b)
                        -> std::vector<map::SplineInst>
                    {
                        const float t0bx = std::sin(h0b * kDegP), t0by = std::cos(h0b * kDegP);
                        const float t1bx = std::sin(h1b * kDegP), t1by = std::cos(h1b * kDegP);
                        const float vxb = x1b - x0b, vyb = y1b - y0b;
                        const float vvb = vxb*vxb + vyb*vyb;
                        const float d2b = 2.f * (1.f - (t0bx*t1bx + t0by*t1by));
                        const float vtb = vxb*(t0bx+t1bx) + vyb*(t0by+t1by);
                        float db = 0.f; bool okb = false;
                        if (std::fabs(d2b) < 1.e-6f) {
                            const float vt1 = vxb*t1bx + vyb*t1by;
                            if (std::fabs(vt1) > 1.e-6f) { db = vvb / (4.f*vt1); okb = true; }
                        } else {
                            const float disb = vtb*vtb + d2b*vvb;
                            if (disb >= 0.f) { db = (-vtb + std::sqrt(disb)) / d2b; okb = true; }
                        }
                        if (!okb || !std::isfinite(db)) return {};
                        const float pm2x = 0.5f*(x0b + x1b + db*(t0bx - t1bx));
                        const float pm2y = 0.5f*(y0b + y1b + db*(t0by - t1by));
                        float La=0,Ra=0;
                        spline::fit_spline_to_xy(x0b, y0b, h0b, pm2x, pm2y, La, Ra);
                        if (!(La >= 0.25f) || !std::isfinite(La) || !std::isfinite(Ra)) return {};
                        map::SplineInst sa = src;
                        sa.position.x=x0b; sa.position.y=y0b; sa.position.z=z0b;
                        sa.heading_deg=h0b; sa.length=La; sa.radius=Ra;
                        const auto gma = spline::evaluate_spline(sa, La);
                        float Lb=0,Rb=0;
                        if (!spline::fit_spline_to_end_heading(gma.x, gma.y, gma.heading_rad * kRadP, x1b, y1b, h1b,
                                                               Lb, Rb))
                            return {};
                        if (!(Lb >= 0.25f) || !std::isfinite(Lb) || !std::isfinite(Rb)) return {};
                        map::SplineInst sb = src;
                        sb.position.x=gma.x; sb.position.y=gma.y; sb.position.z=gma.z;
                        sb.heading_deg=gma.heading_rad*kRadP; sb.length=Lb; sb.radius=Rb;
                        return {sa, sb};
                    };

                    float best_score = path2_score_cand(best.segs, best.path_ep, best.path_eh);
                    float best_r_min = 0.f;
                    for (int N = 2; N <= max_segs; ++N) {
                        std::vector<map::SplineInst> trial;
                        bool all_ok = true;

                        if (N == 2) {
                            trial = {g1, g2};
                        } else {
                            float cx = sx0, cy = sy0, cz = sz0, ch = h0;
                            for (int i = 0; i < N - 2 && all_ok; ++i) {
                                const float along = (float)(i + 1) / N * total_Lg;
                                const spline::SplineSample s =
                                    (along <= L0g) ? spline::evaluate_spline(g1, along)
                                                   : spline::evaluate_spline(g2, along - L0g);
                                float L = 0.f, R = 0.f;
                                spline::fit_spline_to_xy(cx, cy, ch, s.x, s.y, L, R);
                                if (!(L >= 0.05f) || !std::isfinite(L) || !std::isfinite(R)) {
                                    all_ok = false;
                                    break;
                                }
                                map::SplineInst seg = src;
                                seg.position.x = cx;
                                seg.position.y = cy;
                                seg.position.z = cz;
                                seg.heading_deg = ch;
                                seg.length = L;
                                seg.radius = R;
                                trial.push_back(seg);
                                const auto ep2 = spline::evaluate_spline(seg, L);
                                cx = ep2.x;
                                cy = ep2.y;
                                cz = ep2.z;
                                ch = ep2.heading_rad * kRadP;
                            }
                            if (!all_ok) continue;
                            const auto eb = make_biarc(cx, cy, cz, ch, tx, ty, aim_h);
                            if (eb.size() != 2) continue;
                            trial.insert(trial.end(), eb.begin(), eb.end());
                        }

                        float r_min_n = 1.e9f;
                        for (const auto& s : trial)
                            if (s.radius != 0.f) r_min_n = (std::min)(r_min_n, std::fabs(s.radius));

                        float path_ep = 0.f, path_eh = 0.f;
                        const float score = path2_score_cand(trial, path_ep, path_eh);

                        if (score < best_score - 1.e-4f ||
                            (std::fabs(score - best_score) < 1.e-4f && r_min_n < best_r_min)) {
                            best_score = score;
                            best_r_min = r_min_n;
                            best.segs = trial;
                            best.path_ep = path_ep;
                            best.path_eh = path_eh;
                            log::info("autofill path N=" + std::to_string(N) + " path_err=" +
                                      std::to_string(path_ep) + " h_err=" + std::to_string(path_eh) +
                                      " r_min=" + std::to_string(r_min_n));
                            if (path_ep < 0.02f && path_eh < 0.6f) break;
                        }
                    }
                }
            }

        // 3. Fallback: single arc (any radius) — last resort
        if (best.segs.empty()) {
            map::SplineInst one = src;
            one.position.x = sx0; one.position.y = sy0; one.position.z = sz0;
            one.heading_deg = h0;
            float L = 0.f, R = 0.f;
            spline::fit_spline_to_xy(sx0, sy0, h0, tx, ty, L, R);
            one.length = L; one.radius = R;
            best.segs = {one};
            float fep = 0.f, fez = 0.f, feh = 0.f;
            path2_path_err_on(one, fep, fez, feh);
            best.path_ep = fep;
            best.path_eh = feh;
        }

        if (best.segs.empty()) {
            log::warn("autofill path: no solution");
            return;
        }

        // Apply Z ramp
        path2_apply_z_ramp(best.segs);

        push_undo("autofill path");

        const std::int32_t src_id = src.id;
        std::vector<std::int32_t> chain_ids;
        chain_ids.push_back(src_id);

        int src_idx = -1;
        for (std::size_t i = 0; i < edit_splines.size(); ++i) {
            if (edit_splines[i].id == src_id) {
                src_idx = static_cast<int>(i);
                break;
            }
        }
        if (src_idx < 0) {
            log::warn("autofill path: source spline id=" + std::to_string(src_id) + " not in edit_splines");
            return;
        }

        // src is kept on append; on prepend the first segment replaces src from pick_a start.
        edit_splines.reserve(edit_splines.size() + best.segs.size());
        auto& src_inst = edit_splines[static_cast<std::size_t>(src_idx)];

        std::int32_t prev_id = src_id;
        std::int32_t first_new_id = 0;
        int last_idx = src_idx;

        if (prepend) {
            src_inst.position.x = sx0;
            src_inst.position.y = sy0;
            src_inst.position.z = sz0;
            src_inst.heading_deg = h0;
            const auto& first = best.segs.front();
            src_inst.length = first.length;
            src_inst.radius = first.radius;
            src_inst.delta_height = first.delta_height;
            src_inst.use_delta_height = first.use_delta_height;
            src_inst.is_spline_h = first.is_spline_h;
            src_inst.start_grad = first.start_grad;
            src_inst.end_grad = first.end_grad;
            src_inst.skew_start = first.skew_start;
            src_inst.skew_end = first.skew_end;
            remesh_spline(src_inst);
            upsert_transient_spline(src_inst);
            persist_editor_xy(src_inst.position.x, src_inst.position.y);

            for (std::size_t si = 1; si < best.segs.size(); ++si) {
                auto ns = best.segs[si];
                ns.id = next_spline_id();
                ns.prev_id = prev_id;
                ns.next_id = 0;
                if (ns.sli_path.empty()) ns.sli_path = src_inst.sli_path;
                remesh_spline(ns);

                if (prev_id == src_id) {
                    src_inst.next_id = ns.id;
                    remesh_spline(src_inst);
                    upsert_transient_spline(src_inst);
                } else {
                    for (auto& s : edit_splines) {
                        if (s.id == prev_id) {
                            s.next_id = ns.id;
                            remesh_spline(s);
                            upsert_transient_spline(s);
                            break;
                        }
                    }
                }

                edit_splines.push_back(ns);
                upsert_transient_spline(ns);
                persist_editor_xy(ns.position.x, ns.position.y);
                chain_ids.push_back(ns.id);
                last_idx = static_cast<int>(edit_splines.size() - 1);
                if (first_new_id == 0) first_new_id = ns.id;
                prev_id = ns.id;
            }
        } else {
        for (std::size_t si = 0; si < best.segs.size(); ++si) {
            auto ns = best.segs[si];
            ns.id = next_spline_id();
            ns.prev_id = prev_id;
            ns.next_id = 0;
            if (ns.sli_path.empty()) ns.sli_path = src_inst.sli_path;
            remesh_spline(ns);

            // Link previous spline to this new one
            if (prev_id == src_id) {
                src_inst.next_id = ns.id;
                remesh_spline(src_inst);
                upsert_transient_spline(src_inst);
            } else {
                for (auto& s : edit_splines) {
                    if (s.id == prev_id) {
                        s.next_id = ns.id;
                        remesh_spline(s);
                        upsert_transient_spline(s);
                        break;
                    }
                }
            }

            edit_splines.push_back(ns);
            upsert_transient_spline(ns);
            persist_editor_xy(ns.position.x, ns.position.y);
            chain_ids.push_back(ns.id);
            last_idx = static_cast<int>(edit_splines.size() - 1);
            if (first_new_id == 0) first_new_id = ns.id;
            prev_id = ns.id;
        }
        } // append branch
        map::SplineInst* chain_end = nullptr;
        if (last_idx >= 0) chain_end = &edit_splines[static_cast<std::size_t>(last_idx)];

        // Never translate the whole spline to "snap" — that shifts the start off the path pick.
        // Append trim segment(s) from chain tip until path XYZ + tangent match pick_b.
        auto path2_try_append_trim = [&]() -> bool {
            if (!chain_end) return false;
            float ep = 0.f, ez = 0.f, eh = 0.f;
            path2_path_err_on(*chain_end, ep, ez, eh);
            if (ep < 0.02f && ez < 0.05f && eh < 0.6f) return false;
            const auto ctr = spline::evaluate_spline(*chain_end, chain_end->length);
            float L = 0.f, R = 0.f;
            const float h_start = ctr.heading_rad * kRad2;
            if (!spline::fit_spline_to_end_heading(ctr.x, ctr.y, h_start, tx, ty, aim_h, L, R) || L < 0.25f)
                return false;
            map::SplineInst trim = *chain_end;
            trim.id = next_spline_id();
            trim.position.x = ctr.x;
            trim.position.y = ctr.y;
            trim.position.z = ctr.z;
            trim.heading_deg = h_start;
            trim.length = L;
            trim.radius = R;
            const float dz = (pick_b.z - path_height_b) - ctr.z;
            trim.delta_height = dz;
            trim.use_delta_height = std::fabs(dz) > 0.01f;
            trim.is_spline_h = trim.use_delta_height;
            trim.prev_id = chain_end->id;
            trim.next_id = 0;
            chain_end->next_id = trim.id;
            remesh_spline(*chain_end);
            upsert_transient_spline(*chain_end);
            remesh_spline(trim);
            edit_splines.push_back(trim);
            upsert_transient_spline(trim);
            persist_editor_xy(trim.position.x, trim.position.y);
            chain_ids.push_back(trim.id);
            last_idx = static_cast<int>(edit_splines.size() - 1);
            chain_end = &edit_splines[static_cast<std::size_t>(last_idx)];
            path2_path_err_on(*chain_end, ep, ez, eh);
            log::info("autofill path: trim L=" + std::to_string(L) + " R=" + std::to_string(R) + " path_err=" +
                      std::to_string(ep) + " h_err=" + std::to_string(eh));
            return true;
        };
        for (int t = 0; t < 4; ++t)
            if (!path2_try_append_trim()) break;

        float final_path_err = 0.f, final_ez = 0.f, final_eh = 0.f, final_ctr_err = 0.f;
        auto path2_path_end_err = [&](const std::vector<map::SplineInst>& segs, float& ep, float& ez, float& eh) {
            ep = ez = eh = 1.e9f;
            if (segs.empty()) return;
            const auto& last = segs.back();
            if (const auto def = spline::load_sli(last.sli_path)) {
                if (static_cast<std::size_t>(conn_path_idx) < def->paths.size()) {
                    const auto& pp = def->paths[static_cast<std::size_t>(conn_path_idx)];
                    const auto e = spline::evaluate_spline(last, last.length, pp.lateral, pp.height);
                    ep = std::sqrt((e.x - pick_b.x) * (e.x - pick_b.x) + (e.y - pick_b.y) * (e.y - pick_b.y));
                    ez = std::fabs(e.z - pick_b.z);
                    eh = std::fabs(wrap180(e.heading_rad * (180.f / 3.14159265f) - pick_b.heading_deg));
                    return;
                }
            }
            const auto e = spline::evaluate_spline(last, last.length);
            ep = std::sqrt((e.x - pick_b.x) * (e.x - pick_b.x) + (e.y - pick_b.y) * (e.y - pick_b.y));
            ez = std::fabs(e.z - pick_b.z);
            eh = std::fabs(wrap180(e.heading_rad * (180.f / 3.14159265f) - pick_b.heading_deg));
        };
        auto path2_centerline_end_err = [&](const std::vector<map::SplineInst>& segs, float& ep) {
            ep = 1.e9f;
            if (segs.empty()) return;
            const auto& last = segs.back();
            const auto e = spline::evaluate_spline(last, last.length);
            ep = std::sqrt((e.x - tx) * (e.x - tx) + (e.y - ty) * (e.y - ty));
        };

        if (chain_end) path2_path_end_err({*chain_end}, final_path_err, final_ez, final_eh);
        if (chain_end) path2_centerline_end_err({*chain_end}, final_ctr_err);
        if (final_path_err > 0.15f)
            log::warn("autofill path: path_err=" + std::to_string(final_path_err) + " h_err=" +
                      std::to_string(final_eh) + " after trim");

        if (prepend && src_idx >= 0) {
            selected_spline = src_idx;
        } else if (last_idx >= 0) {
            selected_spline = last_idx;
        }
        selected_object = -1;
        autofill_spline_ends = false;
        autofill_path_mode = false;
        autofill_path_stage = 0;
        panel.set_autofill(false);
        panel.set_status("Autofill OFF (joined)");
        refresh_editor_lists();
        if (show_path_rails) {
            for (const std::int32_t cid : chain_ids) rebuild_path_rails_for_owner(cid, false);
            if (pick_b.valid && (pick_b.owner_id != src.id || pick_b.from_object))
                rebuild_path_rails_for_owner(pick_b.owner_id, pick_b.from_object);
        }
        sync_all_end_markers();
        show_spline_at(static_cast<std::size_t>(selected_spline));

        std::string radii_str;
        for (const auto& seg : best.segs) {
            if (!radii_str.empty()) radii_str += ",";
            radii_str += std::to_string(seg.radius);
        }
        log::info("autofill path done solver_segs=" + std::to_string(best.segs.size()) + " chain_total=" +
                  std::to_string(chain_ids.size()) + " path_err=" + std::to_string(final_path_err) + " h_err=" +
                  std::to_string(final_eh) + " ctr_err=" + std::to_string(final_ctr_err) + " radii=[" +
                  radii_str + "]");
    };

    // AA bridge for a .sco source: append ONE fitted path from the chosen end of the source path to
    // the target endpoint (both resolved in the active .sco's local space). Fallback simple arc.
    auto sco_autofill_endpoints = [&]() {
        if (!sco_path_edit.active) {
            panel.set_status("AA: no hay editor de paths .sco activo");
            return;
        }
        map::MapObjectInst* obj = nullptr;
        for (auto& o : edit_objects)
            if (o.id == sco_path_edit.object_id) {
                obj = &o;
                break;
            }
        if (!obj) {
            panel.set_status("AA: no se encontro el .sco activo");
            return;
        }
        int ai = (autofill_path_a.from_object && autofill_path_a.owner_id == sco_path_edit.object_id)
                     ? autofill_path_a.path_index
                     : sco_path_edit.sel;
        if (!sco_path_edit.has_index(ai)) {
            panel.set_status("AA: elegi un extremo de un path del .sco activo");
            return;
        }
        const bool from_start = autofill_path_a.from_object ? autofill_path_a.is_start : false;
        const sco::ScoPath src = sco_path_edit.paths[static_cast<std::size_t>(ai)];
        map::SplineInst mini = sco::sco_path_to_spline_inst(src);
        mini.length = (std::max)(0.05f, std::abs(src.length));

        float lx = src.x, ly = src.y, lz = src.z, out_hdg = src.heading_deg;
        if (from_start) {
            const auto lp = spline::evaluate_spline(mini, 0.f);
            lx = lp.x; ly = lp.y; lz = lp.z;
            out_hdg = lp.heading_rad * (180.f / 3.14159265f) + 180.f;
        } else {
            const auto lp = spline::evaluate_spline(mini, mini.length);
            lx = lp.x; ly = lp.y; lz = lp.z;
            out_hdg = lp.heading_rad * (180.f / 3.14159265f);
        }

        const auto* asco = sco::load_sco(obj->sco_path);
        const bool abs_z = asco && sco::skip_terrain_add(*asco);
        const float th = terrain_add(abs_z, obj->position.x, obj->position.y);
        const XMMATRIX world_m = sco_world_matrix(*obj, nullptr, th, 0.f, 0.f);
        const XMMATRIX inv = DirectX::XMMatrixInverse(nullptr, world_m);
        DirectX::XMFLOAT3 tgt{};
        DirectX::XMStoreFloat3(&tgt, xform_point(inv, autofill_path_b.x, autofill_path_b.y, autofill_path_b.z));

        float len = 1.f, rad = 0.f;
        spline::fit_spline_to_xy(lx, ly, out_hdg, tgt.x, tgt.y, len, rad);

        push_undo("sco_path_aa");
        sco::ScoPath np = src;
        np.x = lx;
        np.y = ly;
        np.z = lz;
        np.heading_deg = out_hdg;
        np.radius = rad;
        np.length = (std::max)(0.05f, len);
        np.grad_start = 0.f;
        np.grad_end = 0.f;
        np.has_use_traffic_light = false;
        np.use_traffic_light = -1;
        sco_path_edit.paths.push_back(np);
        sco_path_edit.select_only(static_cast<int>(sco_path_edit.paths.size()) - 1);
        sco_path_edit.dirty = true;
        sco_path_edit.rebuild_requested = true;
        autofill_path_mode = false;
        sco_path_edit.aa_mode = false;
        panel.set_autofill(false);
        panel.set_status("AA: path .sco unido desde P" + std::to_string(ai));
        log::info("sco AA path appended from=" + std::to_string(ai) + " len=" + std::to_string(len) +
                  " r=" + std::to_string(rad));
    };

    auto autofill_path_endpoints = [&]() {
        if (!autofill_path_a.valid || !autofill_path_b.valid) return;
        if (autofill_path_a.from_object) {
            // .sco source: append fitted path segment(s) to the edited .sco.
            sco_autofill_endpoints();
            autofill_path_a = {};
            autofill_path_b = {};
            return;
        }
        int src_idx = -1;
        if (!autofill_path_a.from_object) {
            for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                if (edit_splines[i].id == autofill_path_a.owner_id) {
                    src_idx = static_cast<int>(i);
                    break;
                }
            }
        }
        if (src_idx < 0) {
            src_idx = selected_spline;
            if (src_idx < 0 || static_cast<std::size_t>(src_idx) >= edit_splines.size()) {
                if (autofill_path_a.from_object) {
                    panel.set_status("Path autofill: first pick must be on a spline path");
                    autofill_path_a = {};
                    autofill_path_b = {};
                    return;
                }
                log::warn("autofill path: could not resolve source spline");
                panel.set_status("Path autofill: no se encontro spline origen");
                return;
            }
        }
        selected_spline = src_idx;
        auto& src = edit_splines[static_cast<std::size_t>(src_idx)];

        if (autofill_path_a.from_object || autofill_path_a.owner_id != src.id) {
            panel.set_status("Path autofill: first path must be on source spline");
            log::warn("autofill path: first pick must be on source spline id=" + std::to_string(src.id));
            autofill_path_a = {};
            autofill_path_b = {};
            return;
        }
        float ax0 = 0.f, ay0 = 0.f, az0 = 0.f, ah0 = 0.f;
        float bx0 = 0.f, by0 = 0.f, bz0 = 0.f, bh0 = 0.f;
        path2_centerline_anchor_for_path_pick(src, autofill_path_a, ax0, ay0, az0, ah0);
        map::SplineInst bproto = src;
        if (!autofill_path_b.from_object) {
            for (const auto& s : edit_splines)
                if (s.id == autofill_path_b.owner_id) {
                    bproto = s;
                    break;
                }
        }
        path2_centerline_anchor_for_path_pick(bproto, autofill_path_b, bx0, by0, bz0, bh0);
        const AutofillAttach attach = AutofillAttach::PrependStart;

        log::info("autofill path pick A xyz=(" + std::to_string(autofill_path_a.x) + "," +
                  std::to_string(autofill_path_a.y) + "," + std::to_string(autofill_path_a.z) + ")" +
                  (autofill_path_a.is_start ? " is_start" : " is_end") +
                  " path_index=" + std::to_string(autofill_path_a.path_index) +
                  " anchor=(" + std::to_string(ax0) + "," + std::to_string(ay0) + "," + std::to_string(az0) + ")");
        log::info("autofill path pick B xyz=(" + std::to_string(autofill_path_b.x) + "," +
                  std::to_string(autofill_path_b.y) + "," + std::to_string(autofill_path_b.z) + ")" +
                  (autofill_path_b.is_start ? " is_start" : " is_end") +
                  " path_index=" + std::to_string(autofill_path_b.path_index) +
                  " anchor=(" + std::to_string(bx0) + "," + std::to_string(by0) + "," + std::to_string(bz0) + ")");
        log::info("autofill path src=" + std::to_string(src.id) + " attach=" +
                  (attach == AutofillAttach::PrependStart ? "prepend" : "append"));

        path2_between(src, autofill_path_a, autofill_path_b, attach);

        autofill_path_a = {};
        autofill_path_b = {};
    };
