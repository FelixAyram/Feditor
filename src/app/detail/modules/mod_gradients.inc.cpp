// MOD: gradients aislado (sliders / undo latch / terrain adapt manual).

    auto ensure_grad_slider_undo = [&](std::int32_t id) {
        if (grad_slider_undo_id == id) return;
        push_undo("spline_grad_slider");
        grad_slider_undo_id = id;
    };

    auto adapt_one_spline_gradient_terrain = [&](map::SplineInst& s) -> bool {
        // Dense multi-point sampling along the whole body. Fit one linear slope
        // by least squares. If the surface is nearly planar / residual < 5 cm,
        // force BOTH gradients to the same value (0 when almost flat). Only when
        // the surface clearly bends do we allow different start/end grads.
        // Goal: body stays within ~0.05 m of the surface; no phantom grades on planes.
        const float len = (std::max)(s.length, 0.05f);
        constexpr float kMaxBodyGap = 0.05f; // metres
        constexpr float kFlatDelta = 0.05f;  // total Δh to treat as flat
        const int n = (std::max)(8, static_cast<int>(std::ceil(len / 0.25f)) + 1);

        std::vector<float> along(static_cast<std::size_t>(n));
        std::vector<float> h(static_cast<std::size_t>(n));
        float h_min = 1.e9f, h_max = -1.e9f;
        for (int i = 0; i < n; ++i) {
            const float a = len * static_cast<float>(i) / static_cast<float>(n - 1);
            along[static_cast<std::size_t>(i)] = a;
            const auto p = spline::evaluate_spline(s, a);
            float z = sample_terrain_editor_xy(p.x, p.y);
            h[static_cast<std::size_t>(i)] = z;
            h_min = (std::min)(h_min, z);
            h_max = (std::max)(h_max, z);
        }

        // Least-squares: h ≈ h0 + m * along  → grade % = m * 100
        double sum_a = 0, sum_h = 0, sum_aa = 0, sum_ah = 0;
        for (int i = 0; i < n; ++i) {
            const double a = along[static_cast<std::size_t>(i)];
            const double z = h[static_cast<std::size_t>(i)];
            sum_a += a;
            sum_h += z;
            sum_aa += a * a;
            sum_ah += a * z;
        }
        const double nn = static_cast<double>(n);
        const double den = nn * sum_aa - sum_a * sum_a;
        float linear_grade = 0.f;
        float h0_fit = static_cast<float>(sum_h / nn);
        if (std::abs(den) > 1.e-9) {
            const double m = (nn * sum_ah - sum_a * sum_h) / den;
            linear_grade = static_cast<float>(m * 100.0);
            h0_fit = static_cast<float>((sum_h - m * sum_a) / nn);
        }
        linear_grade = (std::clamp)(linear_grade, -100.f, 100.f);

        // Residual of linear fit = how "non-planar" the surface is along the path
        float max_res = 0.f;
        for (int i = 0; i < n; ++i) {
            const float pred = h0_fit + (linear_grade * 0.01f) * along[static_cast<std::size_t>(i)];
            max_res = (std::max)(max_res, std::abs(h[static_cast<std::size_t>(i)] - pred));
        }

        float new_start = 0.f;
        float new_end = 0.f;
        if ((h_max - h_min) < kFlatDelta || std::abs(linear_grade) < 0.35f) {
            // Surface essentially flat → zero gradients (no micro-tilt)
            new_start = 0.f;
            new_end = 0.f;
        } else if (max_res <= kMaxBodyGap) {
            // Good linear plane match → same grade at both ends (constant slope)
            new_start = linear_grade;
            new_end = linear_grade;
        } else {
            // Surface bends: local LS at each end over ~30% of length (min 3 samples)
            auto local_grade = [&](int i0, int i1) -> float {
                i0 = (std::max)(0, i0);
                i1 = (std::min)(n - 1, i1);
                if (i1 - i0 < 2) return linear_grade;
                double sa = 0, sh = 0, saa = 0, sah = 0;
                int c = 0;
                for (int i = i0; i <= i1; ++i) {
                    const double a = along[static_cast<std::size_t>(i)];
                    const double z = h[static_cast<std::size_t>(i)];
                    sa += a;
                    sh += z;
                    saa += a * a;
                    sah += a * z;
                    ++c;
                }
                const double d = static_cast<double>(c) * saa - sa * sa;
                if (std::abs(d) < 1.e-9) return linear_grade;
                return (std::clamp)(static_cast<float>((static_cast<double>(c) * sah - sa * sh) / d * 100.0),
                                    -100.f, 100.f);
            };
            const int win = (std::max)(3, n / 3);
            new_start = local_grade(0, win);
            new_end = local_grade(n - 1 - win, n - 1);
            // Pull toward linear so the cubic profile does not dig / float mid-body
            new_start = new_start * 0.55f + linear_grade * 0.45f;
            new_end = new_end * 0.55f + linear_grade * 0.45f;
        }

        if (std::abs(s.start_grad - new_start) < 0.02f && std::abs(s.end_grad - new_end) < 0.02f)
            return false;
        s.start_grad = new_start;
        s.end_grad = new_end;
        remesh_spline(s, false);
        return true;
    };

    auto spline_gradient_adapt_targets = [&]() {
        std::vector<int> targets;
        if (!group_sel_spl.empty())
            targets = group_sel_spl;
        else if (selected_spline >= 0)
            targets.push_back(selected_spline);
        return targets;
    };

    auto apply_spline_gradient_terrain_targets = [&](bool push_undo_first) -> int {
        const auto targets = spline_gradient_adapt_targets();
        if (targets.empty()) return 0;
        if (push_undo_first) push_undo("adapt_spline_gradient");
        for (const int gi : targets) {
            if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
            adapt_one_spline_gradient_terrain(edit_splines[static_cast<std::size_t>(gi)]);
        }
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
            show_spline_at(static_cast<std::size_t>(selected_spline));
        return static_cast<int>(targets.size());
    };

    auto tick_spline_gradient_terrain_live = [&]() {
        if (!panel.imgui_state().spline_adapt_gradient_live) return;
        if (view_only || panel.current_tool() != ui::EditorTool::Spline) return;
        bool changed = false;
        for (const int gi : spline_gradient_adapt_targets()) {
            if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
            if (adapt_one_spline_gradient_terrain(edit_splines[static_cast<std::size_t>(gi)]))
                changed = true;
        }
        if (changed && selected_spline >= 0 &&
            static_cast<std::size_t>(selected_spline) < edit_splines.size())
            update_spline_panel_live(static_cast<std::size_t>(selected_spline));
    };


    // Collision/clearance adaptation ("Adaptar entorno spline").
    //
    // Calidad = número de PARTES reales (1..10). No se fuerza largo igual:
    // un solver elige cortes para que cada extremo quede a <= 0.05 m de la
    // superficie real (terreno + mesh). La Z inicial de la spline es FIJA.
    // El largo total de la cadena se conserva exactamente.
    //
    // El muestreo de superficie corre en el hilo del editor (DX12-safe).
    // El DP/optimización corre en un worker sin tocar el renderer.

    struct SplineEnvSurfaceSample {
        float along = 0.f;
        // surface Z at laterals[0..n-1] in world at this along
        std::vector<float> lateral_z;
    };
    struct SplineEnvSurfaceStrip {
        map::SplineInst source{};
        std::vector<float> laterals;
        std::vector<SplineEnvSurfaceSample> samples; // dense along [0..L]
        float total_len = 0.f;
        float original_start_z = 0.f;
        std::int32_t old_prev = 0;
        std::int32_t old_next = 0;
        std::filesystem::path sli_path;
        std::int32_t source_id = 0;
    };

    auto sample_spline_environment_strip =
        [&](const map::SplineInst& source, const spline::SplineDefinition& def,
            const std::vector<std::size_t>& excluded) -> SplineEnvSurfaceStrip {
        SplineEnvSurfaceStrip strip{};
        strip.source = source;
        strip.source_id = source.id;
        strip.original_start_z = source.position.z;
        strip.old_prev = source.prev_id;
        strip.old_next = source.next_id;
        strip.sli_path = source.sli_path;
        strip.total_len = (std::max)(source.length, 0.5f);

        float min_x = 0.f, max_x = 0.f;
        bool have_x = false;
        for (const auto& profile : def.profiles) {
            for (const auto& pp : profile) {
                if (!have_x) {
                    min_x = max_x = pp.x;
                    have_x = true;
                } else {
                    min_x = (std::min)(min_x, pp.x);
                    max_x = (std::max)(max_x, pp.x);
                }
            }
        }
        if (!have_x) {
            min_x = -1.5f;
            max_x = 1.5f;
        }
        const float width = (std::max)(max_x - min_x, 0.1f);
        const float center = 0.5f * (min_x + max_x);
        // 11 laterals across body (L…C…R) so every station checks the full cross-section.
        constexpr int kLatCount = 11;
        strip.laterals.clear();
        strip.laterals.reserve(kLatCount);
        for (int i = 0; i < kLatCount; ++i)
            strip.laterals.push_back(min_x + width * static_cast<float>(i) / static_cast<float>(kLatCount - 1));
        strip.laterals[kLatCount / 2] = center;

        // Hard body rule: ~10 stations per metre × 11 laterals ≈ 110 probes/m.
        // User requirement: no sample farther than 5 cm from supporting surface.
        constexpr float kAlongStep = 0.10f;
        const int n_samp = (std::max)(32, static_cast<int>(std::ceil(strip.total_len / kAlongStep)) + 1);
        strip.samples.reserve(static_cast<std::size_t>(n_samp));
        float sample_z_min = 1.e9f, sample_z_max = -1.e9f;
        for (int i = 0; i < n_samp; ++i) {
            SplineEnvSurfaceSample s{};
            s.along = strip.total_len * static_cast<float>(i) / static_cast<float>(n_samp - 1);
            s.lateral_z.resize(strip.laterals.size());
            for (std::size_t li = 0; li < strip.laterals.size(); ++li) {
                const auto samp = spline::evaluate_spline(source, s.along, strip.laterals[li], 0.f);
                // Terrain grid first; then raycast mesh tops (own body excluded).
                // Take the HIGHEST support under the probe (road/sli top under the path).
                float z = sample_terrain_editor_xy(samp.x, samp.y);
                if (const auto hit = renderer.pick_surface_hit_z(
                        samp.x, samp.y, 8000.f, 0.f, 0.f, -1.f,
                        excluded.empty() ? nullptr : excluded.data(), excluded.size())) {
                    // Closest hit from above is the top of whatever is below the ray.
                    z = (std::max)(z, *hit);
                }
                s.lateral_z[li] = z;
                sample_z_min = (std::min)(sample_z_min, z);
                sample_z_max = (std::max)(sample_z_max, z);
            }
            strip.samples.push_back(std::move(s));
        }
        log::info("[spline_env] sample id=" + std::to_string(source.id) +
                  " L=" + std::to_string(strip.total_len) +
                  " n_along=" + std::to_string(n_samp) +
                  " n_lat=" + std::to_string(strip.laterals.size()) +
                  " probes=" + std::to_string(n_samp * static_cast<int>(strip.laterals.size())) +
                  " surf_z=[" + std::to_string(sample_z_min) + "," + std::to_string(sample_z_max) + "]" +
                  " width=" + std::to_string(width) +
                  " excl=" + std::to_string(excluded.size()));
        return strip;
    };

    auto compute_spline_environment_plan_from_strip =
        [&](const SplineEnvSurfaceStrip& strip, int quality) -> SplineEnvSplinePlan {
        SplineEnvSplinePlan result{};
        result.source_id = strip.source_id;
        result.original_start_z = strip.original_start_z;
        result.old_prev = strip.old_prev;
        result.old_next = strip.old_next;
        result.sli_path = strip.sli_path;

        const float total_len = strip.total_len;
        const int n_parts = (std::clamp)(quality, 1, 10);
        const int candidate_count = static_cast<int>(strip.samples.size());
        if (candidate_count < 2) {
            result.all_endpoints_within_tolerance = false;
            return result;
        }

        const auto& laterals = strip.laterals;

        struct SectionFit {
            float along = 0.f;
            float target_z = 0.f;
            float target_cant = 0.f;
            float target_grad = 0.f;
            float max_gap = 0.f;
            float min_clearance = 0.f;
            bool feasible = false;
        };

        auto solve_section_at = [&](int sample_idx) -> SectionFit {
            SectionFit best{};
            const auto& sample = strip.samples[static_cast<std::size_t>(sample_idx)];
            best.along = sample.along;
            best.max_gap = 1.e9f;
            best.min_clearance = -1.e9f;
            const auto& surfaces = sample.lateral_z;

            constexpr float kEndpointLift = 0.0025f;
            constexpr float kTargetGap = 0.025f;
            float best_score = 1.e30f;
            for (int ci = -100; ci <= 100; ci += 1) {
                const float cant = static_cast<float>(ci);
                float z = -1.e30f;
                for (std::size_t i = 0; i < laterals.size(); ++i)
                    z = (std::max)(z, surfaces[i] + laterals[i] * cant * 0.01f + kEndpointLift);
                float max_gap = -1.e30f;
                float min_clear = 1.e30f;
                for (std::size_t i = 0; i < laterals.size(); ++i) {
                    const float clearance = z - laterals[i] * cant * 0.01f - surfaces[i];
                    max_gap = (std::max)(max_gap, clearance);
                    min_clear = (std::min)(min_clear, clearance);
                }
                // ABSOLUTE: no dig AND every lateral within 5 cm of support surface.
                const float over = (std::max)(0.f, max_gap - 0.05f);
                const float under = (std::max)(0.f, -min_clear);
                const bool hard_ok = (under < 1.e-4f) && (over < 1.e-4f);
                // Prefer hard-ok solutions; among them hug ~2.5 cm; soft fallback only if none.
                const float score = (hard_ok ? 0.f : 1.e7f) + under * 1.e6f + over * 5.e5f +
                                    std::abs(max_gap - kTargetGap) * 10.f + std::abs(cant) * 0.01f;
                if (score < best_score) {
                    best_score = score;
                    best.target_z = z;
                    best.target_cant = cant;
                    best.max_gap = max_gap;
                    best.min_clearance = min_clear;
                    best.feasible = hard_ok;
                }
            }
            // First sample: keep original start Z fixed (immutable)
            if (sample_idx == 0) {
                best.target_z = strip.original_start_z;
                float max_gap = -1.e30f;
                float min_clear = 1.e30f;
                for (std::size_t i = 0; i < laterals.size(); ++i) {
                    const float clearance =
                        best.target_z - laterals[i] * best.target_cant * 0.01f - surfaces[i];
                    max_gap = (std::max)(max_gap, clearance);
                    min_clear = (std::min)(min_clear, clearance);
                }
                best.max_gap = max_gap;
                best.min_clearance = min_clear;
                best.feasible = (min_clear >= -0.001f);
            }
            return best;
        };

        std::vector<SectionFit> boundaries;
        boundaries.reserve(static_cast<std::size_t>(candidate_count));
        for (int i = 0; i < candidate_count; ++i) boundaries.push_back(solve_section_at(i));

        // Gradiente en cada corte: pendiente de la polilínea de alturas objetivo.
        // Ventana ±2 muestras para suavizar y que start/end de cada tramo no queden "cuadrados".
        for (int i = 0; i < candidate_count; ++i) {
            const int lo = (std::max)(0, i - 2);
            const int hi = (std::min)(candidate_count - 1, i + 2);
            const float ds = boundaries[static_cast<std::size_t>(hi)].along -
                             boundaries[static_cast<std::size_t>(lo)].along;
            float g = 0.f;
            if (ds > 1.e-4f) {
                g = (boundaries[static_cast<std::size_t>(hi)].target_z -
                     boundaries[static_cast<std::size_t>(lo)].target_z) /
                    ds * 100.f;
            }
            boundaries[static_cast<std::size_t>(i)].target_grad = (std::clamp)(g, -100.f, 100.f);
        }
        // Suavizado 1-pass (media con vecinos) para continuidad C0 de la pendiente
        {
            std::vector<float> smoothed(static_cast<std::size_t>(candidate_count));
            for (int i = 0; i < candidate_count; ++i) {
                const int lo = (std::max)(0, i - 1);
                const int hi = (std::min)(candidate_count - 1, i + 1);
                float s = 0.f;
                int n = 0;
                for (int j = lo; j <= hi; ++j) {
                    s += boundaries[static_cast<std::size_t>(j)].target_grad;
                    ++n;
                }
                smoothed[static_cast<std::size_t>(i)] = s / static_cast<float>((std::max)(1, n));
            }
            for (int i = 0; i < candidate_count; ++i)
                boundaries[static_cast<std::size_t>(i)].target_grad = smoothed[static_cast<std::size_t>(i)];
        }

        struct Cell {
            float cost = 1.e30f;
            int prev = -1;
        };
        std::vector<std::vector<Cell>> dp(static_cast<std::size_t>(n_parts + 1),
                                          std::vector<Cell>(static_cast<std::size_t>(candidate_count)));
        dp[0][0].cost = 0.f;

        auto body_dig_penalty = [&](int a, int b) -> float {
            // Every sample along the segment (multi-point body) must stay close
            // to the surface: no dig under, and gap ideally ≤ 0.05 m.
            constexpr float kBodyMaxGap = 0.05f;
            const float z0 = boundaries[static_cast<std::size_t>(a)].target_z;
            const float z1 = boundaries[static_cast<std::size_t>(b)].target_z;
            const float c0 = boundaries[static_cast<std::size_t>(a)].target_cant;
            const float c1 = boundaries[static_cast<std::size_t>(b)].target_cant;
            float worst_under = 0.f;
            float worst_over = 0.f;
            for (int i = a; i <= b; ++i) {
                const float t = (b == a) ? 0.f
                                         : static_cast<float>(i - a) / static_cast<float>(b - a);
                const float zc = z0 + (z1 - z0) * t;
                const float cant = c0 + (c1 - c0) * t;
                const auto& surfaces = strip.samples[static_cast<std::size_t>(i)].lateral_z;
                for (std::size_t li = 0; li < laterals.size() && li < surfaces.size(); ++li) {
                    const float z_vert = zc - laterals[li] * cant * 0.01f;
                    const float clear = z_vert - surfaces[li];
                    if (clear < worst_under) worst_under = clear;
                    if (clear > worst_over) worst_over = clear;
                }
            }
            // ABSOLUTE body rule: any dig or any probe > 5 cm above support → reject segment.
            if (worst_under < -1.e-4f) return 1.e20f;
            if (worst_over > kBodyMaxGap + 1.e-4f) return 1.e20f;
            // Prefer hugging (~0..2.5 cm). Soft cost only inside the hard window.
            float cost = 0.f;
            if (worst_over > 0.025f) cost += (worst_over - 0.025f) * 50.f;
            return cost;
        };

        auto segment_cost = [&](int part_index, int a, int b) -> float {
            if (b <= a) return 1.e20f;
            const float seg_len = boundaries[static_cast<std::size_t>(b)].along -
                                  boundaries[static_cast<std::size_t>(a)].along;
            const int remaining = n_parts - part_index;
            // Keep every part meshable but allow very uneven lengths
            if (seg_len < 0.35f) return 1.e20f;
            const int remaining_cuts = remaining - 1;
            if (candidate_count - 1 - b < remaining_cuts) return 1.e20f;

            const float end_gap = (std::max)(0.f, boundaries[static_cast<std::size_t>(b)].max_gap - 0.05f) +
                                  (std::max)(0.f, -boundaries[static_cast<std::size_t>(b)].min_clearance);
            const float start_gap =
                part_index == 0
                    ? 0.f
                    : (std::max)(0.f, boundaries[static_cast<std::size_t>(a)].max_gap - 0.05f) +
                          (std::max)(0.f, -boundaries[static_cast<std::size_t>(a)].min_clearance);
            const float grad_j =
                std::abs(boundaries[static_cast<std::size_t>(b)].target_grad -
                         boundaries[static_cast<std::size_t>(a)].target_grad) *
                0.01f;
            const float cant_j =
                std::abs(boundaries[static_cast<std::size_t>(b)].target_cant -
                         boundaries[static_cast<std::size_t>(a)].target_cant) *
                0.002f;
            const float ideal = total_len / static_cast<float>(n_parts);
            const float len_soft = std::abs(seg_len - ideal) / (ideal + 1.f) * 0.08f;
            const float infeas = boundaries[static_cast<std::size_t>(b)].feasible ? 0.f : 50.f;
            const float body = body_dig_penalty(a, b);
            return (start_gap + end_gap) * 1.e5f + body + grad_j + cant_j + len_soft + infeas;
        };

        for (int pcount = 1; pcount <= n_parts; ++pcount) {
            for (int b = pcount; b < candidate_count; ++b) {
                Cell best{};
                for (int a = pcount - 1; a < b; ++a) {
                    if (dp[pcount - 1][static_cast<std::size_t>(a)].cost >= 1.e29f) continue;
                    const float sc = segment_cost(pcount - 1, a, b);
                    if (sc >= 1.e19f) continue;
                    const float total = dp[pcount - 1][static_cast<std::size_t>(a)].cost + sc;
                    if (total < best.cost) {
                        best.cost = total;
                        best.prev = a;
                    }
                }
                dp[pcount][static_cast<std::size_t>(b)] = best;
            }
        }

        std::vector<int> cuts(static_cast<std::size_t>(n_parts + 1), 0);
        cuts[static_cast<std::size_t>(n_parts)] = candidate_count - 1;
        int cursor = candidate_count - 1;
        bool path_ok = false;
        if (n_parts == 1) {
            cuts[0] = 0;
            cuts[1] = candidate_count - 1;
            path_ok = true;
        } else if (dp[n_parts][static_cast<std::size_t>(cursor)].prev >= 0) {
            path_ok = true;
            for (int pcount = n_parts; pcount >= 1; --pcount) {
                const int prev = dp[pcount][static_cast<std::size_t>(cursor)].prev;
                if (prev < 0) {
                    path_ok = false;
                    break;
                }
                cuts[static_cast<std::size_t>(pcount - 1)] = prev;
                cursor = prev;
            }
            cuts[0] = 0;
            cuts[static_cast<std::size_t>(n_parts)] = candidate_count - 1;
            // Validate strictly increasing sample indices
            for (int i = 0; i < n_parts; ++i) {
                if (cuts[static_cast<std::size_t>(i + 1)] <= cuts[static_cast<std::size_t>(i)]) {
                    path_ok = false;
                    break;
                }
            }
        }
        if (!path_ok) {
            // Equal sample indices — still n_parts real pieces covering full length
            for (int i = 0; i <= n_parts; ++i)
                cuts[static_cast<std::size_t>(i)] = static_cast<int>(std::round(
                    static_cast<double>(candidate_count - 1) * static_cast<double>(i) /
                    static_cast<double>(n_parts)));
            for (int i = 1; i <= n_parts; ++i) {
                if (cuts[static_cast<std::size_t>(i)] <= cuts[static_cast<std::size_t>(i - 1)])
                    cuts[static_cast<std::size_t>(i)] = cuts[static_cast<std::size_t>(i - 1)] + 1;
            }
            cuts[static_cast<std::size_t>(n_parts)] = candidate_count - 1;
        }

        // Convert sample indices → along in [0, total_len] with STRICT partition (no gaps).
        // Use exact equal arc lengths as base, then nudge to DP sample alongs when valid.
        std::vector<float> along_cuts(static_cast<std::size_t>(n_parts + 1), 0.f);
        along_cuts[0] = 0.f;
        along_cuts[static_cast<std::size_t>(n_parts)] = total_len;
        if (path_ok) {
            for (int i = 1; i < n_parts; ++i) {
                along_cuts[static_cast<std::size_t>(i)] =
                    boundaries[static_cast<std::size_t>(cuts[static_cast<std::size_t>(i)])].along;
            }
        } else {
            for (int i = 1; i < n_parts; ++i)
                along_cuts[static_cast<std::size_t>(i)] =
                    total_len * static_cast<float>(i) / static_cast<float>(n_parts);
        }
        // Enforce monotonic + min spacing without eating the tail
        const float min_seg = (std::min)(0.35f, total_len / static_cast<float>(n_parts * 4 + 1));
        for (int i = 1; i < n_parts; ++i) {
            const float lo = along_cuts[static_cast<std::size_t>(i - 1)] + min_seg;
            const float hi = total_len - min_seg * static_cast<float>(n_parts - i);
            float& a = along_cuts[static_cast<std::size_t>(i)];
            if (a < lo) a = lo;
            if (a > hi) a = hi;
        }
        along_cuts[static_cast<std::size_t>(n_parts)] = total_len;

        result.all_endpoints_within_tolerance = path_ok;
        result.worst_gap = 0.f;
        result.parts.clear();
        result.parts.reserve(static_cast<std::size_t>(n_parts));
        for (int i = 0; i < n_parts; ++i) {
            const int ia = cuts[static_cast<std::size_t>(i)];
            const int ib = cuts[static_cast<std::size_t>(i + 1)];
            const auto& a = boundaries[static_cast<std::size_t>((std::clamp)(ia, 0, candidate_count - 1))];
            const auto& b = boundaries[static_cast<std::size_t>((std::clamp)(ib, 0, candidate_count - 1))];
            SplineEnvPartPlan part{};
            part.start_along = along_cuts[static_cast<std::size_t>(i)];
            part.end_along = along_cuts[static_cast<std::size_t>(i + 1)];
            part.start_z = (i == 0) ? strip.original_start_z : a.target_z;
            part.end_z = b.target_z;
            part.start_grad = a.target_grad;
            part.end_grad = b.target_grad;
            part.start_cant = a.target_cant;
            part.end_cant = b.target_cant;
            part.delta_height = part.end_z - part.start_z;
            result.parts.push_back(part);
            result.worst_gap = (std::max)(result.worst_gap, a.max_gap);
            result.worst_gap = (std::max)(result.worst_gap, b.max_gap);
        }

        // Continuidad en uniones + preferencia por rampa lineal del tramo.
        // Si |Δz| del tramo es < 5 cm → gradientes a 0 (plano real, sin micro-tilt).
        for (std::size_t i = 0; i < result.parts.size(); ++i) {
            auto& p = result.parts[i];
            const float seg = (std::max)(p.end_along - p.start_along, 1.e-3f);
            const float dz = p.end_z - p.start_z;
            const float linear_g = dz / seg * 100.f;
            if (std::abs(dz) < 0.05f) {
                p.start_grad = 0.f;
                p.end_grad = 0.f;
            } else {
                // 35% terreno local + 65% rampa lineal → cuerpo sigue la superficie
                p.start_grad = (std::clamp)(p.start_grad * 0.35f + linear_g * 0.65f, -100.f, 100.f);
                p.end_grad = (std::clamp)(p.end_grad * 0.35f + linear_g * 0.65f, -100.f, 100.f);
            }
            p.delta_height = dz;
        }
        for (std::size_t i = 0; i + 1 < result.parts.size(); ++i) {
            // C1 suave en el corte compartido
            const float g_joint = 0.5f * (result.parts[i].end_grad + result.parts[i + 1].start_grad);
            const float c_joint = 0.5f * (result.parts[i].end_cant + result.parts[i + 1].start_cant);
            result.parts[i].end_grad = g_joint;
            result.parts[i + 1].start_grad = g_joint;
            result.parts[i].end_cant = c_joint;
            result.parts[i + 1].start_cant = c_joint;
            // z continua
            result.parts[i + 1].start_z = result.parts[i].end_z;
            result.parts[i].delta_height = result.parts[i].end_z - result.parts[i].start_z;
            result.parts[i + 1].delta_height =
                result.parts[i + 1].end_z - result.parts[i + 1].start_z;
        }
        {
            float sum = 0.f;
            for (const auto& p : result.parts) sum += (p.end_along - p.start_along);
            if (std::fabs(sum - total_len) > 1.e-3f && !result.parts.empty()) {
                result.parts.back().end_along = total_len;
                result.parts.back().delta_height =
                    result.parts.back().end_z - result.parts.back().start_z;
            }
        }
        // Re-validate full body against dense samples (absolute 5 cm rule).
        {
            constexpr float kBodyMaxGap = 0.05f;
            float worst = 0.f;
            int fail_i = -1, fail_li = -1;
            float fail_clear = 0.f;
            int n_fail = 0;
            for (const auto& part : result.parts) {
                for (int i = 0; i < candidate_count; ++i) {
                    const float a = strip.samples[static_cast<std::size_t>(i)].along;
                    if (a < part.start_along - 1.e-4f || a > part.end_along + 1.e-4f) continue;
                    const float t = (part.end_along - part.start_along) > 1.e-6f
                                        ? (a - part.start_along) / (part.end_along - part.start_along)
                                        : 0.f;
                    const float zc = part.start_z + (part.end_z - part.start_z) * t;
                    const float cant = part.start_cant + (part.end_cant - part.start_cant) * t;
                    const auto& surfaces = strip.samples[static_cast<std::size_t>(i)].lateral_z;
                    for (std::size_t li = 0; li < laterals.size() && li < surfaces.size(); ++li) {
                        const float z_vert = zc - laterals[li] * cant * 0.01f;
                        const float clear = z_vert - surfaces[li];
                        if (clear > worst) {
                            worst = clear;
                            fail_i = i;
                            fail_li = static_cast<int>(li);
                            fail_clear = clear;
                        }
                        if (clear < -1.e-4f || clear > kBodyMaxGap + 1.e-4f) ++n_fail;
                    }
                }
            }
            result.worst_gap = (std::max)(result.worst_gap, worst);
            result.all_endpoints_within_tolerance = (n_fail == 0);
            log::info("[spline_env] plan id=" + std::to_string(result.source_id) +
                      " parts=" + std::to_string(result.parts.size()) +
                      " worst_gap_m=" + std::to_string(result.worst_gap) +
                      " fails=" + std::to_string(n_fail) +
                      " hard_ok=" + (result.all_endpoints_within_tolerance ? "1" : "0") +
                      (fail_i >= 0
                           ? (" worst@along=" +
                              std::to_string(strip.samples[static_cast<std::size_t>(fail_i)].along) +
                              " lat=" + std::to_string(laterals[static_cast<std::size_t>(fail_li)]) +
                              " clear=" + std::to_string(fail_clear))
                           : std::string()));
            for (std::size_t pi = 0; pi < result.parts.size(); ++pi) {
                const auto& p = result.parts[pi];
                log::info("[spline_env]  part[" + std::to_string(pi) + "] along=[" +
                          std::to_string(p.start_along) + "," + std::to_string(p.end_along) +
                          "] z=[" + std::to_string(p.start_z) + "," + std::to_string(p.end_z) +
                          "] grad=[" + std::to_string(p.start_grad) + "," + std::to_string(p.end_grad) +
                          "] cant=[" + std::to_string(p.start_cant) + "," + std::to_string(p.end_cant) +
                          "]");
            }
        }
        return result;
    };

    auto start_spline_environment_job = [&](int quality) -> bool {
        if (view_only || panel.current_tool() != ui::EditorTool::Spline) return false;
        const auto targets = spline_gradient_adapt_targets();
        if (targets.empty()) return false;
        {
            std::scoped_lock lock(spline_env_job.mutex);
            if (spline_env_job.running) return false;
            spline_env_job.finished = false;
            spline_env_job.result.reset();
            spline_env_job.running = true;
        }

        // Sample surfaces on the MAIN thread (renderer / terrain are not worker-safe).
        std::vector<SplineEnvSurfaceStrip> strips;
        strips.reserve(targets.size());
        for (const int gi : targets) {
            if (gi < 0 || static_cast<std::size_t>(gi) >= edit_splines.size()) continue;
            const auto& source = edit_splines[static_cast<std::size_t>(gi)];
            const auto def = spline::load_sli(source.sli_path);
            if (!def) continue;
            std::vector<std::size_t> excluded;
            if (const auto* old_gpu = find_spline_gpu(source.id)) excluded = old_gpu->chunks;
            for (const auto& mk : end_markers)
                if (mk.alive) excluded.push_back(mk.chunk);
            for (const auto& rail : path_rails)
                if (rail.alive) excluded.push_back(rail.chunk);
            strips.push_back(sample_spline_environment_strip(source, *def, excluded));
        }
        if (strips.empty()) {
            std::scoped_lock lock(spline_env_job.mutex);
            spline_env_job.running = false;
            return false;
        }

        const int q = (std::clamp)(quality, 1, 10);
        try {
            if (spline_env_job.worker.joinable()) spline_env_job.worker.join();
            // Capture plan lambda by reference is safe: run-loop lives for the process,
            // and the plan function only reads the strip (no renderer).
            auto* job_ptr = &spline_env_job;
            auto plan_fn = compute_spline_environment_plan_from_strip;
            spline_env_job.worker = std::thread([strips = std::move(strips), q, job_ptr, plan_fn]() mutable {
                auto done = std::make_shared<SplineEnvJobResult>();
                done->requested_quality = q;
                done->splines.reserve(strips.size());
                try {
                    for (const auto& strip : strips)
                        done->splines.push_back(plan_fn(strip, q));
                } catch (...) {
                    done->splines.clear();
                }
                {
                    std::scoped_lock lock(job_ptr->mutex);
                    job_ptr->result = std::move(done);
                    job_ptr->finished = true;
                    job_ptr->running = false;
                }
            });
        } catch (...) {
            std::scoped_lock lock(spline_env_job.mutex);
            spline_env_job.running = false;
            spline_env_job.finished = false;
            return false;
        }
        return true;
    };

    auto commit_spline_environment_result =
        [&](const std::shared_ptr<SplineEnvJobResult>& done) -> int {
        if (!done || done->splines.empty()) {
            log::info("[spline_env] commit: empty result");
            editor_log::event("SPLINE_ENV_COMMIT", "empty result");
            return 0;
        }
        int adapted = 0;
        for (const auto& plan : done->splines) {
            log::info("[spline_env] plan source_id=" + std::to_string(plan.source_id) +
                      " parts=" + std::to_string(plan.parts.size()) +
                      " quality=" + std::to_string(done->requested_quality) +
                      " worst_gap=" + std::to_string(plan.worst_gap) +
                      " ok=" + (plan.all_endpoints_within_tolerance ? "1" : "0"));
            if (plan.parts.empty()) {
                log::info("[spline_env] skip: no parts in plan");
                continue;
            }
            int src_idx = -1;
            for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                if (edit_splines[i].id == plan.source_id) {
                    src_idx = static_cast<int>(i);
                    break;
                }
            }
            if (src_idx < 0) {
                log::info("[spline_env] skip: source id " + std::to_string(plan.source_id) +
                          " not in edit_splines (size=" + std::to_string(edit_splines.size()) + ")");
                continue;
            }
            const map::SplineInst source = edit_splines[static_cast<std::size_t>(src_idx)];
            const float total_len = (std::max)(source.length, 0.5f);
            const std::size_t n = plan.parts.size();

            // Exact equal-weight residual partition from plan alongs, always covering [0,L].
            std::vector<float> cuts(n + 1, 0.f);
            cuts[0] = 0.f;
            cuts[n] = total_len;
            for (std::size_t i = 1; i < n; ++i) {
                float a = plan.parts[i].start_along;
                if (!(a > 0.f) || a >= total_len) a = total_len * static_cast<float>(i) / static_cast<float>(n);
                cuts[i] = a;
            }
            // Sort-stabilize monotonic
            for (std::size_t i = 1; i < n; ++i) {
                if (cuts[i] <= cuts[i - 1])
                    cuts[i] = cuts[i - 1] + total_len / static_cast<float>(n * 2);
            }
            // Clamp into (0,L) and re-open room for remaining parts
            for (std::size_t i = 1; i < n; ++i) {
                const float lo = total_len * 0.001f * static_cast<float>(i);
                const float hi = total_len * (1.f - 0.001f * static_cast<float>(n - i));
                cuts[i] = (std::max)(lo, (std::min)(hi, cuts[i]));
                if (cuts[i] <= cuts[i - 1]) cuts[i] = cuts[i - 1] + (hi - cuts[i - 1]) * 0.5f;
            }
            cuts[0] = 0.f;
            cuts[n] = total_len;
            // If still degenerate → uniform
            bool uniform = false;
            for (std::size_t i = 0; i < n; ++i) {
                if (cuts[i + 1] - cuts[i] < total_len * 0.001f) {
                    uniform = true;
                    break;
                }
            }
            if (uniform) {
                log::info("[spline_env] cuts degenerate → uniform partition n=" + std::to_string(n) +
                          " L=" + std::to_string(total_len));
                for (std::size_t i = 0; i <= n; ++i)
                    cuts[i] = total_len * static_cast<float>(i) / static_cast<float>(n);
            }

            {
                std::string cut_log = "[spline_env] cuts L=" + std::to_string(total_len) + " |";
                for (std::size_t i = 0; i <= n; ++i)
                    cut_log += " " + std::to_string(cuts[i]);
                log::info(cut_log);
            }

            // next_spline_id() only looks at current edit_splines — calling it N times
            // BEFORE push_back always returns the same max+1. Allocate consecutive ids.
            std::vector<std::int32_t> ids;
            ids.reserve(n);
            ids.push_back(source.id);
            std::int32_t nid = next_spline_id();
            for (std::size_t i = 1; i < n; ++i) {
                // skip collision with source id or already-chosen
                while (nid == source.id) ++nid;
                for (std::size_t j = 1; j < i; ++j) {
                    if (nid == ids[j]) {
                        ++nid;
                        j = 0;
                    }
                }
                ids.push_back(nid);
                ++nid;
            }
            {
                std::string idlog = "[spline_env] allocated ids:";
                for (auto id : ids) idlog += " " + std::to_string(id);
                log::info(idlog);
            }

            if (auto* old_gpu = find_spline_gpu(source.id)) {
                for (const auto c : old_gpu->chunks) renderer.set_chunk_visible(c, false);
                old_gpu->chunks.clear();
                old_gpu->alive = false;
            }

            std::vector<map::SplineInst> parts;
            parts.reserve(n);
            float length_sum = 0.f;
            for (std::size_t i = 0; i < n; ++i) {
                const auto& pp = plan.parts[i];
                const float along0 = cuts[i];
                float seg_len = cuts[i + 1] - cuts[i];
                if (i + 1 == n) seg_len = (std::max)(0.05f, total_len - length_sum);
                if (seg_len < 0.05f) {
                    log::info("[spline_env] WARN part " + std::to_string(i) + " tiny len=" +
                              std::to_string(seg_len) + " — boosting");
                    seg_len = 0.05f;
                }
                length_sum += seg_len;

                const auto start_pose = spline::evaluate_spline(source, along0);
                map::SplineInst seg = source;
                seg.id = ids[i];
                seg.prev_id = (i == 0) ? plan.old_prev : ids[i - 1];
                seg.next_id = (i + 1 < n) ? ids[i + 1] : plan.old_next;
                seg.position.x = start_pose.x;
                seg.position.y = start_pose.y;
                seg.position.z = (i == 0) ? plan.original_start_z : pp.start_z;
                seg.heading_deg = start_pose.heading_rad * (180.f / 3.14159265358979323846f);
                seg.length = seg_len;
                seg.start_grad = (std::clamp)(pp.start_grad, -100.f, 100.f);
                seg.end_grad = (std::clamp)(pp.end_grad, -100.f, 100.f);
                seg.cant_start = (std::clamp)(pp.start_cant, -100.f, 100.f);
                seg.cant_end = (std::clamp)(pp.end_cant, -100.f, 100.f);
                seg.use_delta_height = true;
                seg.is_spline_h = true;
                seg.delta_height = pp.end_z - ((i == 0) ? plan.original_start_z : pp.start_z);
                seg.local_length = 0.f;
                parts.push_back(seg);
                log::info("[spline_env] part[" + std::to_string(i) + "] id=" + std::to_string(seg.id) +
                          " along0=" + std::to_string(along0) + " len=" + std::to_string(seg_len) +
                          " z=" + std::to_string(seg.position.z) +
                          " dH=" + std::to_string(seg.delta_height) +
                          " prev=" + std::to_string(seg.prev_id) +
                          " next=" + std::to_string(seg.next_id));
            }
            if (!parts.empty()) {
                float sum = 0.f;
                for (std::size_t i = 0; i + 1 < parts.size(); ++i) sum += parts[i].length;
                parts.back().length = (std::max)(0.05f, total_len - sum);
                log::info("[spline_env] length_sum_before_last=" + std::to_string(sum) +
                          " last_len=" + std::to_string(parts.back().length) +
                          " total=" + std::to_string(total_len) +
                          " check_sum=" + std::to_string(sum + parts.back().length));
            }

            edit_splines[static_cast<std::size_t>(src_idx)] = parts.front();
            upsert_transient_spline(edit_splines[static_cast<std::size_t>(src_idx)]);
            for (std::size_t i = 1; i < parts.size(); ++i) {
                edit_splines.push_back(parts[i]);
                upsert_transient_spline(edit_splines.back());
            }

            for (std::size_t i = 0; i < parts.size(); ++i) {
                int idx = -1;
                for (std::size_t j = 0; j < edit_splines.size(); ++j)
                    if (edit_splines[j].id == ids[i]) {
                        idx = static_cast<int>(j);
                        break;
                    }
                if (idx < 0) {
                    log::info("[spline_env] REMESH SKIP missing id=" + std::to_string(ids[i]));
                    continue;
                }
                remesh_spline(edit_splines[static_cast<std::size_t>(idx)], false);
                if (auto* g = find_spline_gpu(ids[i])) {
                    log::info("[spline_env] remesh ok id=" + std::to_string(ids[i]) +
                              " chunks=" + std::to_string(g->chunks.size()) +
                              " alive=" + (g->alive ? "1" : "0"));
                } else {
                    log::info("[spline_env] remesh ok id=" + std::to_string(ids[i]) + " (no gpu yet)");
                }
                ++adapted;
                if ((adapted % 2) == 0) pump_editor_busy();
            }
            if (plan.old_next != 0) {
                for (auto& sp : edit_splines)
                    if (sp.id == plan.old_next) sp.prev_id = ids.back();
            }
            editor_log::event("SPLINE_ENV_COMMIT",
                              "id=" + std::to_string(plan.source_id) + " parts=" + std::to_string(n) +
                                  " adapted=" + std::to_string(adapted) + " L=" + std::to_string(total_len));
        }
        refresh_editor_lists();
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
            show_spline_at(static_cast<std::size_t>(selected_spline));
        log::info("[spline_env] commit done adapted=" + std::to_string(adapted) +
                  " edit_splines=" + std::to_string(edit_splines.size()));
        return adapted;
    };

    auto poll_spline_environment_job = [&]() -> bool {
        std::shared_ptr<SplineEnvJobResult> done;
        {
            std::scoped_lock lock(spline_env_job.mutex);
            if (!spline_env_job.finished) return false;
            done = std::move(spline_env_job.result);
            spline_env_job.finished = false;
            spline_env_job.running = false;
        }
        if (spline_env_job.worker.joinable()) spline_env_job.worker.join();
        const int n = commit_spline_environment_result(done);
        panel.imgui_state().busy_overlay = false;
        panel.imgui_state().busy_spinner = false;
        panel.imgui_state().busy_progress = -1.f;
        panel.imgui_state().busy_title.clear();
        panel.imgui_state().busy_detail.clear();
        panel.set_status(n > 0 ? ("Entorno spline adaptado (" + std::to_string(n) + " partes, largo conservado)")
                               : "Adaptar entorno spline: no se encontró una solución válida");
        return true;
    };


