// ============================================================================
// MOD autofill single-A (Complete to / una A)
// Centerline autofill only — no path snap, no doble-A.
// ============================================================================
    auto lock_spline_start_to_pose = [&](map::SplineInst& s, float px, float py, float pz, float h_deg) {
        const auto samp = spline::evaluate_spline(s, 0.f);
        s.position.x += px - samp.x;
        s.position.y += py - samp.y;
        s.position.z += pz - samp.z;
        float dh = h_deg - samp.heading_rad * (180.f / 3.14159265f);
        while (dh > 180.f) dh -= 360.f;
        while (dh < -180.f) dh += 360.f;
        s.heading_deg += dh;
    };
    auto apply_autofill_seg_fields = [&](map::SplineInst& dst, const map::SplineInst& seg) {
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
    // Biarc G1 exacto (pos+heading en ambos extremos).
    // Reemplaza el biarc antiguo (solo XY + tolerancia eh<45° / gap<0.35m).
    // Estrategia: grid sobre (L0, giro0) del 1er arco; 2º arco = fit_end_heading al target.
    auto compute_biarc_omsi = [&](float x0, float y0, float h0_deg, float x1, float y1, float h1_deg,
                                 float /*soft*/, float& L0, float& R0, float& L1, float& R1, float& jx, float& jy,
                                 float& hj_deg) -> bool {
        constexpr float kPi = 3.14159265f;
        constexpr float kDeg = kPi / 180.f;
        auto wrap180 = [](float a) {
            while (a > 180.f) a -= 360.f;
            while (a < -180.f) a += 360.f;
            return a;
        };
        const float dist = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
        if (dist < 0.25f) return false;

        // 1) Probar single G1 primero
        {
            float Ls = 0.f, Rs = 0.f;
            if (spline::fit_spline_to_end_heading(x0, y0, h0_deg, x1, y1, h1_deg, Ls, Rs) &&
                Ls >= 0.25f && std::isfinite(Ls) && std::isfinite(Rs)) {
                map::SplineInst tmp{};
                tmp.position.x = x0; tmp.position.y = y0; tmp.position.z = 0.f;
                tmp.heading_deg = h0_deg; tmp.length = Ls; tmp.radius = Rs;
                const auto e = spline::evaluate_spline(tmp, Ls);
                const float gap = std::sqrt((e.x - x1) * (e.x - x1) + (e.y - y1) * (e.y - y1));
                const float eh = std::fabs(wrap180(e.heading_rad / kDeg - h1_deg));
                if (gap < 0.02f && eh < 0.5f) {
                    // Single alcanza: devolver como biarc degenerado (L1 mínimo no se usa si caller prefiere single)
                    L0 = Ls; R0 = Rs; L1 = 0.f; R1 = 0.f;
                    jx = x1; jy = y1; hj_deg = h1_deg;
                    return true;
                }
            }
        }

        auto eval_end = [&](float ax, float ay, float ah, float L, float R,
                            float& ox, float& oy, float& oh) {
            map::SplineInst tmp{};
            tmp.position.x = ax; tmp.position.y = ay; tmp.position.z = 0.f;
            tmp.heading_deg = ah; tmp.length = L; tmp.radius = R;
            const auto e = spline::evaluate_spline(tmp, L);
            ox = e.x; oy = e.y; oh = e.heading_rad / kDeg;
        };

        float best_cost = 1.e9f;
        float bL0 = 0, bR0 = 0, bL1 = 0, bR1 = 0, bjx = 0, bjy = 0, bhj = 0;

        // Grid: giro del 1er arco (grados) y longitud
        for (int turn_i = -120; turn_i <= 120; turn_i += 4) {
            if (turn_i == 0) continue;
            const float turn_deg = static_cast<float>(turn_i);
            const float turn_rad = turn_deg * kDeg;
            for (int li = 5; li <= 180; li += 3) {
                const float l0 = static_cast<float>(li) * 0.15f; // ~0.75 .. 27 m
                const float r0 = l0 / turn_rad;
                if (std::fabs(r0) < 0.3f || std::fabs(r0) > 2000.f) continue;
                if (!std::isfinite(r0)) continue;

                float mx = 0, my = 0, mh = 0;
                eval_end(x0, y0, h0_deg, l0, r0, mx, my, mh);

                float l1 = 0.f, r1 = 0.f;
                if (!spline::fit_spline_to_end_heading(mx, my, mh, x1, y1, h1_deg, l1, r1))
                    continue;
                if (l1 < 0.25f || !std::isfinite(l1) || !std::isfinite(r1)) continue;
                if (std::fabs(r1) > 5000.f) continue;

                float ex = 0, ey = 0, eh = 0;
                eval_end(mx, my, mh, l1, r1, ex, ey, eh);
                const float gap = std::sqrt((ex - x1) * (ex - x1) + (ey - y1) * (ey - y1));
                const float dhh = std::fabs(wrap180(eh - h1_deg));
                const float cost = gap + 0.02f * dhh;
                if (cost < best_cost) {
                    best_cost = cost;
                    bL0 = l0; bR0 = r0; bL1 = l1; bR1 = r1;
                    bjx = mx; bjy = my; bhj = mh;
                    if (best_cost < 0.005f) break;
                }
            }
            if (best_cost < 0.005f) break;
        }

        // Refinado local alrededor del mejor
        if (best_cost < 1.e8f && bL0 > 0.f) {
            const float turn0 = (bR0 != 0.f) ? (bL0 / bR0) / kDeg : 0.f;
            for (int dti = -8; dti <= 8; ++dti) {
                for (int dli = -8; dli <= 8; ++dli) {
                    const float l0 = bL0 + static_cast<float>(dli) * 0.05f;
                    if (l0 < 0.25f) continue;
                    const float turn_deg = turn0 + static_cast<float>(dti) * 0.35f;
                    if (std::fabs(turn_deg) < 0.2f) continue;
                    const float r0 = l0 / (turn_deg * kDeg);
                    if (std::fabs(r0) < 0.3f || std::fabs(r0) > 2000.f) continue;
                    float mx = 0, my = 0, mh = 0;
                    eval_end(x0, y0, h0_deg, l0, r0, mx, my, mh);
                    float l1 = 0.f, r1 = 0.f;
                    if (!spline::fit_spline_to_end_heading(mx, my, mh, x1, y1, h1_deg, l1, r1))
                        continue;
                    if (l1 < 0.25f || !std::isfinite(l1) || !std::isfinite(r1)) continue;
                    float ex = 0, ey = 0, eh = 0;
                    eval_end(mx, my, mh, l1, r1, ex, ey, eh);
                    const float gap = std::sqrt((ex - x1) * (ex - x1) + (ey - y1) * (ey - y1));
                    const float dhh = std::fabs(wrap180(eh - h1_deg));
                    const float cost = gap + 0.02f * dhh;
                    if (cost < best_cost) {
                        best_cost = cost;
                        bL0 = l0; bR0 = r0; bL1 = l1; bR1 = r1;
                        bjx = mx; bjy = my; bhj = mh;
                    }
                }
            }
        }

        // Aceptar solo uniones casi exactas (el caller de cadena no debe "arreglar" con XY)
        if (best_cost > 0.08f) return false;

        L0 = bL0; R0 = bR0; L1 = bL1; R1 = bR1;
        jx = bjx; jy = bjy; hj_deg = bhj;

        // Si el "biarc" degeneró a single (L1==0), marcar fallo para que el caller use single path
        if (L1 < 0.25f) return false;
        return true;
    };

    auto autofill_between = [&](map::SplineInst& src, float sx0, float sy0, float sz0, float h0, float tx, float ty,
                                float tz, float target_heading_deg, AutofillAttach attach = AutofillAttach::ReplaceStart) {
        const float dist = std::sqrt((tx - sx0) * (tx - sx0) + (ty - sy0) * (ty - sy0));
        if (dist < 0.05f) {
            log::warn("autofill: target too close to start");
            return;
        }

        auto apply_z_ramp = [&](std::vector<map::SplineInst>& segs) {
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
            float ep = 1.e9f;
        };
        Cand best{};

        auto wrap180 = [](float a) {
            while (a > 180.f) a -= 360.f;
            while (a < -180.f) a += 360.f;
            return a;
        };
        const float aim_h = target_heading_deg;

        // Score = endpoint error + heading error + soft penalty for tight radii.
        // Path autofill that worked used the same ep/eh idea; the r_min term stops N=2 with
        // r≈6 "winning" (ep≈0) while leaving a visible mid-gap on long chords.
        auto score_chain = [&](const std::vector<map::SplineInst>& trial, float& ep, float& eh) -> float {
            ep = eh = 1.e9f;
            if (trial.empty()) return 1.e9f;
            float cx = trial.front().position.x;
            float cy = trial.front().position.y;
            float ch = trial.front().heading_deg;
            float r_pen = 0.f;
            for (const auto& seg : trial) {
                map::SplineInst t = seg;
                t.position.x = cx;
                t.position.y = cy;
                t.heading_deg = ch;
                const auto e = spline::evaluate_spline(t, t.length);
                cx = e.x;
                cy = e.y;
                ch = e.heading_rad * (180.f / 3.14159265f);
            }
            ep = std::sqrt((cx - tx) * (cx - tx) + (cy - ty) * (cy - ty));
            eh = std::fabs(wrap180(ch - aim_h));
            return ep + 0.02f * eh + r_pen;
        };

        // 1. Single arc with end-heading lock (OMSI complete-to style).
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
                const float sc = score_chain({one}, ep, eh);
                if (ep < 0.05f && eh < 5.f) {
                    best.segs = {one};
                    best.ep = ep;
                } else if (best.segs.empty() || sc < best.ep) {
                    best.segs = {one};
                    best.ep = ep;
                }
            }
        }

        // 2. Multi-segment N=2..10 — port of the path-autofill BiarcInterp solver that already worked.
        //    Critical fixes vs the broken single-A path:
        //    - last arc uses fit_spline_to_end_heading (not fit_spline_to_xy)
        //    - do NOT stop at N=2 just because r_min≥8 (that left empty gaps in the middle)
        //    - keep searching N until ep+eh are good, preferring larger r_min on ties
        {
            constexpr float kDeg2 = 3.14159265f / 180.f;
            constexpr float kRad2 = 180.f / 3.14159265f;
            constexpr int max_segs = 10;

            const float t0x = std::sin(h0 * kDeg2), t0y = std::cos(h0 * kDeg2);
            const float t1x = std::sin(aim_h * kDeg2), t1y = std::cos(aim_h * kDeg2);
            const float vx = tx - sx0, vy = ty - sy0;
            const float vDotV = vx * vx + vy * vy;
            const float den2 = 2.f * (1.f - (t0x * t1x + t0y * t1y));
            const float vDotT = vx * (t0x + t1x) + vy * (t0y + t1y);
            float d = 0.f;
            bool pm_ok = false;
            if (std::fabs(den2) < 1.e-6f) {
                const float vdt1 = vx * t1x + vy * t1y;
                if (std::fabs(vdt1) > 1.e-6f) {
                    d = vDotV / (4.f * vdt1);
                    pm_ok = true;
                }
            } else {
                const float disc = vDotT * vDotT + den2 * vDotV;
                if (disc >= 0.f) {
                    d = (-vDotT + std::sqrt(disc)) / den2;
                    pm_ok = true;
                }
            }
            if (pm_ok && std::isfinite(d)) {
                const float pmx = 0.5f * (sx0 + tx + d * (t0x - t1x));
                const float pmy = 0.5f * (sy0 + ty + d * (t0y - t1y));

                float L0g = 0.f, R0g = 0.f, L1g = 0.f, R1g = 0.f;
                spline::fit_spline_to_xy(sx0, sy0, h0, pmx, pmy, L0g, R0g);
                map::SplineInst g1 = src;
                g1.position.x = sx0;
                g1.position.y = sy0;
                g1.position.z = sz0;
                g1.heading_deg = h0;
                g1.length = L0g;
                g1.radius = R0g;
                const auto gm = spline::evaluate_spline(g1, L0g);
                if (!spline::fit_spline_to_end_heading(gm.x, gm.y, gm.heading_rad * kRad2, tx, ty, aim_h, L1g,
                                                       R1g))
                    L1g = 0.f;
                map::SplineInst g2 = src;
                g2.position.x = gm.x;
                g2.position.y = gm.y;
                g2.position.z = gm.z;
                g2.heading_deg = gm.heading_rad * kRad2;
                g2.length = L1g;
                g2.radius = R1g;

                if (L0g >= 0.25f && L1g >= 0.25f && std::isfinite(L0g) && std::isfinite(L1g)) {
                    const float total_Lg = L0g + L1g;

                    auto make_biarc2 = [&](float x0b, float y0b, float z0b, float h0b, float x1b, float y1b,
                                           float h1b) -> std::vector<map::SplineInst> {
                        const float tbx0 = std::sin(h0b * kDeg2), tby0 = std::cos(h0b * kDeg2);
                        const float tbx1 = std::sin(h1b * kDeg2), tby1 = std::cos(h1b * kDeg2);
                        const float vxb = x1b - x0b, vyb = y1b - y0b;
                        const float vvb = vxb * vxb + vyb * vyb;
                        const float d2b = 2.f * (1.f - (tbx0 * tbx1 + tby0 * tby1));
                        const float vtb = vxb * (tbx0 + tbx1) + vyb * (tby0 + tby1);
                        float db = 0.f;
                        bool okb = false;
                        if (std::fabs(d2b) < 1.e-6f) {
                            const float vt1 = vxb * tbx1 + vyb * tby1;
                            if (std::fabs(vt1) > 1.e-6f) {
                                db = vvb / (4.f * vt1);
                                okb = true;
                            }
                        } else {
                            const float disb = vtb * vtb + d2b * vvb;
                            if (disb >= 0.f) {
                                db = (-vtb + std::sqrt(disb)) / d2b;
                                okb = true;
                            }
                        }
                        if (!okb || !std::isfinite(db)) return {};
                        const float pm2x = 0.5f * (x0b + x1b + db * (tbx0 - tbx1));
                        const float pm2y = 0.5f * (y0b + y1b + db * (tby0 - tby1));
                        float La = 0.f, Ra = 0.f;
                        spline::fit_spline_to_xy(x0b, y0b, h0b, pm2x, pm2y, La, Ra);
                        if (!(La >= 0.25f) || !std::isfinite(La) || !std::isfinite(Ra)) return {};
                        map::SplineInst sa = src;
                        sa.position.x = x0b;
                        sa.position.y = y0b;
                        sa.position.z = z0b;
                        sa.heading_deg = h0b;
                        sa.length = La;
                        sa.radius = Ra;
                        const auto gma = spline::evaluate_spline(sa, La);
                        float Lb = 0.f, Rb = 0.f;
                        if (!spline::fit_spline_to_end_heading(gma.x, gma.y, gma.heading_rad * kRad2, x1b, y1b,
                                                               h1b, Lb, Rb))
                            return {};
                        if (!(Lb >= 0.25f) || !std::isfinite(Lb) || !std::isfinite(Rb)) return {};
                        map::SplineInst sb = src;
                        sb.position.x = gma.x;
                        sb.position.y = gma.y;
                        sb.position.z = gma.z;
                        sb.heading_deg = gma.heading_rad * kRad2;
                        sb.length = Lb;
                        sb.radius = Rb;
                        return {sa, sb};
                    };

                    float best_score = 1.e9f;
                    if (!best.segs.empty()) {
                        float ep0 = 0.f, eh0 = 0.f;
                        best_score = score_chain(best.segs, ep0, eh0);
                        best.ep = ep0;
                    }
                    float best_r_min = 0.f;
                    for (int N = 2; N <= max_segs; ++N) {
                        std::vector<map::SplineInst> trial;
                        bool all_ok = true;

                        if (N == 2) {
                            trial = {g1, g2};
                        } else {
                            float cx = sx0, cy = sy0, cz = sz0, ch = h0;
                            for (int i = 0; i < N - 2 && all_ok; ++i) {
                                const float along = (float)(i + 1) / (float)N * total_Lg;
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
                                const auto ep = spline::evaluate_spline(seg, L);
                                cx = ep.x;
                                cy = ep.y;
                                cz = ep.z;
                                ch = ep.heading_rad * kRad2;
                            }
                            if (!all_ok) continue;
                            const auto eb = make_biarc2(cx, cy, cz, ch, tx, ty, aim_h);
                            if (eb.size() != 2) continue;
                            trial.insert(trial.end(), eb.begin(), eb.end());
                        }

                        // Re-anchor every segment so the chain is continuous (avoids ghost gaps).
                        {
                            float cx = sx0, cy = sy0, cz = sz0, ch = h0;
                            for (auto& seg : trial) {
                                seg.position.x = cx;
                                seg.position.y = cy;
                                seg.position.z = cz;
                                seg.heading_deg = ch;
                                const auto ep = spline::evaluate_spline(seg, seg.length);
                                cx = ep.x;
                                cy = ep.y;
                                cz = ep.z;
                                ch = ep.heading_rad * kRad2;
                            }
                        }

                        float r_min_n = 1.e9f;
                        for (const auto& s : trial)
                            if (s.radius != 0.f) r_min_n = (std::min)(r_min_n, std::fabs(s.radius));
                        if (r_min_n > 1.e8f) r_min_n = 0.f;

                        float ep_n = 0.f, eh_n = 0.f;
                        const float score = score_chain(trial, ep_n, eh_n);

                        if (score < best_score - 1.e-4f ||
                            (std::fabs(score - best_score) < 1.e-4f && r_min_n < best_r_min) ||
                            best.segs.empty()) {
                            best_score = score;
                            best_r_min = r_min_n;
                            best.segs = trial;
                            best.ep = ep_n;
                            log::info("autofill N=" + std::to_string(N) + " r_min=" + std::to_string(r_min_n) +
                                      " ep=" + std::to_string(ep_n) + " eh=" + std::to_string(eh_n));
                            // Only stop early when the chain actually closes — never on r_min alone
                            // (that was the bug: N=2 with r_min≥8 left empty middle gaps).
                            if (ep_n < 0.05f && eh_n < 2.f) break;
                        }
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
            const auto end_p = spline::evaluate_spline(one, L);
            best.ep = std::sqrt((end_p.x-tx)*(end_p.x-tx) + (end_p.y-ty)*(end_p.y-ty));
        }

        if (best.segs.empty()) {
            log::warn("autofill: no solution");
            return;
        }

        // Apply Z ramp
        apply_z_ramp(best.segs);

        push_undo("autofill");

        // Never keep a live reference across push_back — same pattern as path autofill (src_id + reindex).
        const std::int32_t src_id = src.id;
        const std::filesystem::path src_sli = src.sli_path;
        int src_idx = -1;
        for (std::size_t i = 0; i < edit_splines.size(); ++i) {
            if (edit_splines[i].id == src_id) {
                src_idx = static_cast<int>(i);
                break;
            }
        }
        if (src_idx < 0) {
            log::warn("autofill: source spline id=" + std::to_string(src_id) + " not in edit_splines");
            return;
        }

        edit_splines.reserve(edit_splines.size() + best.segs.size() + 2);

        std::int32_t prev_id = src_id;
        std::int32_t first_new_id = 0;
        int last_idx = src_idx;
        std::vector<std::int32_t> chain_ids;
        chain_ids.push_back(src_id);

        auto find_spl = [&](std::int32_t id) -> map::SplineInst* {
            for (auto& s : edit_splines)
                if (s.id == id) return &s;
            return nullptr;
        };

        auto link_prev_to = [&](std::int32_t new_id) {
            if (auto* p = find_spl(prev_id)) {
                p->next_id = new_id;
                if (!p->sli_path.empty()) {
                    remesh_spline(*p);
                    upsert_transient_spline(*p);
                }
            }
        };

        auto spawn_seg = [&](map::SplineInst ns) -> bool {
            ns.id = next_spline_id();
            ns.prev_id = prev_id;
            ns.next_id = 0;
            if (ns.sli_path.empty()) ns.sli_path = src_sli;
            if (ns.sli_path.empty()) {
                log::warn("autofill skip remesh: empty sli_path new id=" + std::to_string(ns.id));
                return false;
            }
            remesh_spline(ns);
            link_prev_to(ns.id);
            edit_splines.push_back(ns);
            upsert_transient_spline(edit_splines.back());
            persist_editor_xy(edit_splines.back().position.x, edit_splines.back().position.y);
            last_idx = static_cast<int>(edit_splines.size() - 1);
            if (first_new_id == 0) first_new_id = ns.id;
            chain_ids.push_back(ns.id);
            prev_id = ns.id;
            return true;
        };

        if (attach == AutofillAttach::AppendEnd) {
            for (std::size_t si = 0; si < best.segs.size(); ++si) {
                if (!spawn_seg(best.segs[si])) break;
            }
        } else {
            auto* src_inst = find_spl(src_id);
            if (!src_inst) {
                log::warn("autofill: lost src id=" + std::to_string(src_id));
                return;
            }
            src_inst->position.x = sx0;
            src_inst->position.y = sy0;
            src_inst->position.z = sz0;
            src_inst->heading_deg = h0;
            src_inst->next_id = 0;
            apply_autofill_seg_fields(*src_inst, best.segs.front());
            if (src_inst->sli_path.empty()) src_inst->sli_path = src_sli;
            if (!src_inst->sli_path.empty()) {
                remesh_spline(*src_inst);
                upsert_transient_spline(*src_inst);
            } else {
                log::warn("autofill: src empty sli_path id=" + std::to_string(src_id));
            }
            persist_editor_xy(src_inst->position.x, src_inst->position.y);

            for (std::size_t si = 1; si < best.segs.size(); ++si) {
                if (!spawn_seg(best.segs[si])) break;
            }
        }

        if (last_idx >= 0) {
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
        }
        sync_all_end_markers();
        if (selected_spline >= 0) show_spline_at(static_cast<std::size_t>(selected_spline));

        log::info("autofill done segs=" + std::to_string(best.segs.size()) + " from id=" + std::to_string(src_id) +
                  " attach=" +
                  std::string(attach == AutofillAttach::AppendEnd
                                  ? "append"
                                  : (attach == AutofillAttach::PrependStart ? "prepend" : "replace")) +
                  " dist=" + std::to_string(dist) + " ep=" + std::to_string(best.ep));
    };

    auto autofill_from_selection_to = [&](float tx, float ty, float tz, float target_heading_deg,
                                          bool target_is_start) {
        if (selected_spline < 0 || static_cast<std::size_t>(selected_spline) >= edit_splines.size()) {
            log::warn("autofill: select a spline first");
            return;
        }
        auto& src = edit_splines[static_cast<std::size_t>(selected_spline)];

        // Complete from the nearer free tip (OMSI complete-to). Always starting at map START
        // was the gap bug: N=2 closed ep≈0 on the wrong end while the road middle stayed empty.
        const auto beg = spline::evaluate_spline(src, 0.f);
        const auto tip = spline::evaluate_spline(src, src.length);
        const float d0 = (beg.x - tx) * (beg.x - tx) + (beg.y - ty) * (beg.y - ty);
        const float d1 = (tip.x - tx) * (tip.x - tx) + (tip.y - ty) * (tip.y - ty);
        const bool from_end = d1 <= d0;

        float sx0 = 0.f, sy0 = 0.f, sz0 = 0.f, h0 = 0.f;
        AutofillAttach attach = AutofillAttach::ReplaceStart;
        if (from_end) {
            sx0 = tip.x;
            sy0 = tip.y;
            sz0 = tip.z;
            h0 = tip.heading_rad * (180.f / 3.14159265f);
            attach = AutofillAttach::AppendEnd;
        } else {
            sx0 = beg.x;
            sy0 = beg.y;
            sz0 = beg.z;
            h0 = beg.heading_rad * (180.f / 3.14159265f);
            attach = AutofillAttach::ReplaceStart;
        }

        float aim_h = target_heading_deg;
        if (!target_is_start) {
            aim_h += 180.f;
            while (aim_h > 180.f) aim_h -= 360.f;
            while (aim_h < -180.f) aim_h += 360.f;
        }

        log::info(std::string("autofill tip=") + (from_end ? "end" : "start") + " src_id=" +
                  std::to_string(src.id) + " aim_h=" + std::to_string(aim_h));
        autofill_between(src, sx0, sy0, sz0, h0, tx, ty, tz, aim_h, attach);
    };
