// MOD: spline chain handles — union balls, gradient circles, prev/next maintenance,
// chain-constrained grab helpers. Included from map_editor run-loop.
//
// Visual sizes come from EditorUiSettings:
//   union_ball_screen_pct  (default 0.02 = 2% of min viewport)
//   grad_circle_screen_pct (default 0.01 = 1% of min viewport)
//
// Colors (RGBA 0-1):
//   Union: idle white 0.9a, hover yellow, selected blue, multi-junction orange
//   Grad:  idle soft yellow, hover bright yellow, selected cyan/blue
//   Edge highlight when union selected: #FF8C00

// ---------------------------------------------------------------------------
// Chain link helpers
// ---------------------------------------------------------------------------

// Clear all prev_id / next_id references that pointed at `deleted_id`.
// Call AFTER removing the spline from edit_splines (or while still present).
auto clear_spline_chain_links = [&](std::int32_t deleted_id) {
    if (deleted_id == 0) return;
    for (auto& s : edit_splines) {
        if (s.next_id == deleted_id) s.next_id = 0;
        if (s.prev_id == deleted_id) s.prev_id = 0;
    }
    for (auto& s : transient_splines) {
        if (s.next_id == deleted_id) s.next_id = 0;
        if (s.prev_id == deleted_id) s.prev_id = 0;
    }
};

// When assigning next_id on `parent_id` → `child_id`, ensure no other spline
// still points next_id at the same child (one parent only).
auto assign_next_id_unique = [&](std::int32_t parent_id, std::int32_t child_id) {
    if (parent_id == 0 || child_id == 0) return;
    for (auto& s : edit_splines) {
        if (s.id == parent_id) {
            s.next_id = child_id;
        } else if (s.next_id == child_id) {
            s.next_id = 0; // steal link
        }
    }
};

// Break cycles: walk forward from every seed; if we revisit, clear the offending next_id.
auto validate_and_break_spline_cycles = [&]() {
    std::unordered_map<std::int32_t, map::SplineInst*> by_id;
    for (auto& s : edit_splines) by_id[s.id] = &s;
    for (auto& s : edit_splines) {
        std::unordered_set<std::int32_t> seen;
        std::int32_t cur = s.id;
        for (int hop = 0; hop < 512; ++hop) {
            if (cur == 0) break;
            if (!seen.insert(cur).second) {
                // cycle detected — clear next_id of the previous node
                // (the one that pointed back into the seen set)
                // Find who points to `cur` among seen and zero it.
                for (std::int32_t sid : seen) {
                    auto it = by_id.find(sid);
                    if (it != by_id.end() && it->second->next_id == cur) {
                        it->second->next_id = 0;
                        log::warn("[spline_chain] broke cycle at id=" + std::to_string(sid) +
                                  " -> " + std::to_string(cur));
                        break;
                    }
                }
                break;
            }
            auto it = by_id.find(cur);
            if (it == by_id.end()) break;
            cur = it->second->next_id;
        }
    }
};

// Find spline by id (mutable). Returns nullptr if not found.
auto find_spline_by_id = [&](std::int32_t id) -> map::SplineInst* {
    if (id == 0) return nullptr;
    for (auto& s : edit_splines) {
        if (s.id == id) return &s;
    }
    return nullptr;
};

// Evaluate end sample of a spline.
auto spline_end_sample = [&](const map::SplineInst& sp) {
    return spline::evaluate_spline(sp, sp.length);
};

// ---------------------------------------------------------------------------
// Chain-constrained geometry helpers
// ---------------------------------------------------------------------------

// Fit L/R so end lands at (tx,ty) with end heading th_deg. Returns residual metres.
// Prefer fit_spline_to_end_heading; fallback to XY-only fit.
auto fit_spline_end_heading_clamped = [&](map::SplineInst& sp, float tx, float ty, float th_deg,
                                          float max_r) -> float {
    float len = sp.length, rad = sp.radius;
    bool ok = spline::fit_spline_to_end_heading(sp.position.x, sp.position.y, sp.heading_deg,
                                                 tx, ty, th_deg, len, rad);
    if (!ok || len < 0.25f || !std::isfinite(len) || !std::isfinite(rad)) {
        spline::fit_spline_to_xy(sp.position.x, sp.position.y, sp.heading_deg, tx, ty, len, rad);
        ok = false;
    }
    if (max_r > 0.f && std::fabs(rad) > max_r) {
        // Soft clamp R — length from chord (loses exact heading but stays bounded)
        const float dist = std::hypot(tx - sp.position.x, ty - sp.position.y);
        rad = (rad >= 0.f) ? max_r : -max_r;
        const float half = (max_r > 1e-4f) ? (dist / (2.f * max_r)) : 1.f;
        if (half >= 0.999f) {
            rad = 0.f;
            len = (std::max)(0.25f, dist);
        } else {
            len = std::fabs(rad) * 2.f * std::asin((std::min)(1.f, half));
        }
        ok = false;
    }
    if (len < 0.25f) len = 0.25f;
    sp.length = len;
    sp.radius = rad;
    const auto e = spline::evaluate_spline(sp, sp.length);
    const float pos_res = std::hypot(e.x - tx, e.y - ty);
    const float eh = e.heading_rad * (180.f / 3.14159265f);
    float dh = std::fabs(eh - th_deg);
    while (dh > 180.f) dh = std::fabs(dh - 360.f);
    // Combine: 1° heading error ≈ 0.05 m penalty for feasibility
    return pos_res + dh * 0.05f;
};

// Legacy XY-only (some call sites)
auto fit_spline_end_to_xy_clamped = [&](map::SplineInst& sp, float tx, float ty, float max_r) -> float {
    return fit_spline_end_heading_clamped(sp, tx, ty, sp.heading_deg, max_r); // weak
};

auto can_fit_end_heading = [&](const map::SplineInst& base, float tx, float ty, float th_deg,
                               float max_r, float tol) -> bool {
    map::SplineInst tmp = base;
    return fit_spline_end_heading_clamped(tmp, tx, ty, th_deg, max_r) <= tol;
};

auto can_fit_end = [&](const map::SplineInst& base, float tx, float ty, float max_r, float tol) -> bool {
    // Without target heading, only check XY reach
    map::SplineInst tmp = base;
    float len = tmp.length, rad = tmp.radius;
    spline::fit_spline_to_xy(tmp.position.x, tmp.position.y, tmp.heading_deg, tx, ty, len, rad);
    if (max_r > 0.f && std::fabs(rad) > max_r) return false;
    tmp.length = len;
    tmp.radius = rad;
    const auto e = spline::evaluate_spline(tmp, tmp.length);
    return std::hypot(e.x - tx, e.y - ty) <= tol;
};

// Per-neighbor radius budget: UI max_r is often 200 (too loose). Bound by current shape.
auto effective_max_r = [&](const map::SplineInst& s, float ui_max) -> float {
    float er = std::fabs(s.radius);
    if (er < 1.f) er = (std::max)(s.length, 8.f); // straight → allow moderate bend
    float cap = (std::max)(er * 2.f, s.length * 2.f);
    if (ui_max > 1.f) cap = (std::min)(cap, ui_max);
    return (std::max)(cap, 5.f);
};

// Body-G + stick_chain: rigid translate of `cur` from grab origin toward want,
// clamped so prev can reach new start and next can reach fixed end.
// grab_ox/oy = pose at G-start (not current frame).
// Fixed end of next spline during G (pos + heading). Declared before grab lambdas.
float chain_fix_next_ex = 0.f, chain_fix_next_ey = 0.f, chain_fix_next_ez = 0.f;
float chain_fix_next_eh = 0.f;
bool chain_fix_next_valid = false;
std::int32_t chain_fix_next_id = 0; // real id of spline after mid (never a temp)
std::int32_t chain_fix_prev_id = 0;

auto rigid_chain_body_grab = [&](map::SplineInst& cur, float want_x, float want_y, float max_r,
                                 float grab_ox, float grab_oy,
                                 float fix_next_ex, float fix_next_ey, bool have_fix_next) {
    map::SplineInst* prev = (cur.prev_id != 0) ? find_spline_by_id(cur.prev_id) : nullptr;
    map::SplineInst* nxt = (cur.next_id != 0) ? find_spline_by_id(cur.next_id) : nullptr;

    // Rigid offset from grab start (keeps L/R/heading of cur).
    const float dx0 = want_x - grab_ox;
    const float dy0 = want_y - grab_oy;

    // Snapshot of neighbors at call time (prev start/heading fixed during search).
    map::SplineInst prev_base{};
    if (prev) prev_base = *prev;
    map::SplineInst nxt_base{};
    if (nxt) nxt_base = *nxt;

    // End offset of cur relative to its start (constant under rigid translate).
    const auto cur_end0 = spline::evaluate_spline(cur, cur.length);
    const float end_off_x = cur_end0.x - cur.position.x;
    const float end_off_y = cur_end0.y - cur.position.y;

    constexpr float kTol = 0.8f; // pos m + heading penalty

    const float prev_rmax = prev ? effective_max_r(prev_base, max_r) : max_r;
    const float next_rmax = nxt ? effective_max_r(nxt_base, max_r) : max_r;
    // Target heading at joint = cur start heading (rigid)
    const float joint_h = cur.heading_deg;
    const float cur_end_h = cur_end0.heading_rad * (180.f / 3.14159265f);

    auto feasible = [&](float s) -> bool {
        const float nx = grab_ox + dx0 * s;
        const float ny = grab_oy + dy0 * s;
        if (prev) {
            if (!can_fit_end_heading(prev_base, nx, ny, joint_h, prev_rmax, kTol)) return false;
        }
        if (nxt && have_fix_next) {
            const float ex = nx + end_off_x;
            const float ey = ny + end_off_y;
            map::SplineInst nb = nxt_base;
            nb.position.x = ex;
            nb.position.y = ey;
            nb.heading_deg = cur_end_h;
            if (!can_fit_end_heading(nb, fix_next_ex, fix_next_ey, chain_fix_next_eh, next_rmax, kTol))
                return false;
        }
        return true;
    };

    float s = 1.f;
    if (!feasible(1.f)) {
        float lo = 0.f, hi = 1.f;
        for (int i = 0; i < 16; ++i) {
            const float mid = 0.5f * (lo + hi);
            if (feasible(mid)) lo = mid;
            else hi = mid;
        }
        s = lo;
        static float last_s = -1.f;
        if (std::fabs(last_s - s) > 0.02f) {
            last_s = s;
            log::info("[chain_dbg] CLAMP s=" + std::to_string(s) +
                      " grab=(" + std::to_string(grab_ox) + "," + std::to_string(grab_oy) +
                      ") want=(" + std::to_string(want_x) + "," + std::to_string(want_y) +
                      ") prev=" + std::to_string(prev ? prev->id : 0) +
                      " next=" + std::to_string(nxt ? nxt->id : 0) +
                      " max_r=" + std::to_string(max_r));
        }
    }

    cur.position.x = grab_ox + dx0 * s;
    cur.position.y = grab_oy + dy0 * s;

    // Keep cur rigid (L/R/heading already restored by caller)
    if (prev) {
        const float res = fit_spline_end_heading_clamped(*prev, cur.position.x, cur.position.y,
                                                        cur.heading_deg, prev_rmax);
        if (res > kTol)
            log::info("[chain_dbg] prev residual=" + std::to_string(res) +
                      " id=" + std::to_string(prev->id) + " rmax=" + std::to_string(prev_rmax));
    }
    if (nxt) {
        const auto e2 = spline::evaluate_spline(cur, cur.length);
        nxt->position.x = e2.x;
        nxt->position.y = e2.y;
        nxt->position.z = e2.z;
        nxt->heading_deg = e2.heading_rad * (180.f / 3.14159265f);
        if (have_fix_next) {
            const float res = fit_spline_end_heading_clamped(*nxt, fix_next_ex, fix_next_ey,
                                                            chain_fix_next_eh, next_rmax);
            if (res > kTol)
                log::info("[chain_dbg] next residual=" + std::to_string(res) +
                          " id=" + std::to_string(nxt->id) + " rmax=" + std::to_string(next_rmax) +
                          " fix=(" + std::to_string(fix_next_ex) + "," + std::to_string(fix_next_ey) +
                          ") eh=" + std::to_string(chain_fix_next_eh));
        }
    }
};


// Union-ball drag: junction moves; A start fixed; B end fixed.
auto union_ball_drag = [&](map::SplineInst& a, map::SplineInst& b, float jx, float jy, float max_r,
                           float fix_b_ex, float fix_b_ey, bool have_fix_b) {
    // Clamp junction so A can reach it and B can still reach fixed end.
    constexpr float kTol = 0.6f;
    map::SplineInst a0 = a;
    map::SplineInst b0 = b;
    const float ox = a0.position.x, oy = a0.position.y; // not used as origin — junction is free in plane
    // Binary search from current junction (end of a) toward (jx,jy)
    const auto a_end0 = spline::evaluate_spline(a, a.length);
    const float j0x = a_end0.x, j0y = a_end0.y;
    float dx = jx - j0x, dy = jy - j0y;
    auto feasible = [&](float s) -> bool {
        const float tx = j0x + dx * s;
        const float ty = j0y + dy * s;
        if (!can_fit_end(a0, tx, ty, max_r, kTol)) return false;
        if (have_fix_b) {
            map::SplineInst nb = b0;
            // After A fits, evaluate actual end for B start
            map::SplineInst ta = a0;
            fit_spline_end_to_xy_clamped(ta, tx, ty, max_r);
            const auto ae = spline::evaluate_spline(ta, ta.length);
            nb.position.x = ae.x;
            nb.position.y = ae.y;
            nb.heading_deg = ae.heading_rad * (180.f / 3.14159265f);
            if (!can_fit_end(nb, fix_b_ex, fix_b_ey, max_r, kTol)) return false;
        }
        return true;
    };
    float s = 1.f;
    if (!feasible(1.f)) {
        float lo = 0.f, hi = 1.f;
        for (int i = 0; i < 16; ++i) {
            const float mid = 0.5f * (lo + hi);
            if (feasible(mid)) lo = mid;
            else hi = mid;
        }
        s = lo;
        log::info("[chain_dbg] union CLAMP s=" + std::to_string(s));
    }
    const float tx = j0x + dx * s;
    const float ty = j0y + dy * s;
    fit_spline_end_to_xy_clamped(a, tx, ty, max_r);
    const auto ae = spline::evaluate_spline(a, a.length);
    b.position.x = ae.x;
    b.position.y = ae.y;
    b.position.z = ae.z;
    b.heading_deg = ae.heading_rad * (180.f / 3.14159265f);
    if (have_fix_b) fit_spline_end_to_xy_clamped(b, fix_b_ex, fix_b_ey, max_r);
    (void)ox;
    (void)oy;
};

// Legacy helpers
auto constrain_chain_after_start_move = [&](map::SplineInst& cur, float max_r) {
    if (cur.next_id != 0) {
        if (const auto* nxt = find_spline_by_id(cur.next_id))
            fit_spline_end_to_xy_clamped(cur, nxt->position.x, nxt->position.y, max_r);
    }
    if (cur.prev_id != 0) {
        if (auto* prev = find_spline_by_id(cur.prev_id))
            fit_spline_end_to_xy_clamped(*prev, cur.position.x, cur.position.y, max_r);
    }
};
auto constrain_chain_end_fixed = [&](map::SplineInst& cur, float max_r) {
    if (cur.next_id == 0) return;
    if (const auto* nxt = find_spline_by_id(cur.next_id))
        fit_spline_end_to_xy_clamped(cur, nxt->position.x, nxt->position.y, max_r);
};

// Selected handle (shared across G start / draw). kind: 0=none 1=union 2=grad
int chain_sel_handle_kind = 0;
std::int32_t chain_sel_handle_spline = 0;
std::int32_t chain_sel_handle_partner = 0;
bool chain_sel_handle_start_side = false;
// Evita que el LMB-release del click en la bola re-seleccione el body.
bool node_click_suppress_body_pick = false;
// Grab mode: 0=body 1=center/grad 2=union/nodo 3=end libre 4=start libre
int spline_grab_target = 0;
// Nodo multi (D/E): starters (inician en joint) + enders extras (terminan en joint)
std::vector<std::int32_t> node_starter_ids;
std::vector<float> node_starter_fex, node_starter_fey, node_starter_feh;
std::vector<std::int32_t> node_ender_ids;
std::vector<float> node_ender_sx, node_ender_sy, node_ender_sz, node_ender_sh;
float node_joint0_x = 0.f, node_joint0_y = 0.f, node_joint0_z = 0.f, node_joint0_h = 0.f;
bool node_joint0_valid = false;
// Lock .sli de cada id involucrado en el nodo
std::vector<std::int32_t> node_sli_lock_ids;
std::vector<std::filesystem::path> node_sli_lock_paths;
std::vector<std::int32_t> fill_temp_ids; // temps actively linked this frame (result)
std::vector<std::int32_t> fill_temp_pool; // parked temps available for reuse (NOT linked)

// Node grab: id + snapshot (for the Z delta_h mode) captured when G starts on a node.
std::int32_t grab_spline_id = 0;
bool node_height_mode = false;         // Z on a node grab → adjust only delta_h/grads
bool node_grab_snap_valid = false;
map::SplineInst node_grab_snap_primary{};
map::SplineInst node_grab_snap_prev{};
map::SplineInst node_grab_snap_next{};
bool node_grab_snap_has_prev = false;
bool node_grab_snap_has_next = false;

// Adjust the height profile of a spline node by k (meters, + = up) and propagate to the chain.
// node_kind: 0 = end (with/without next handled inside), 1 = start (no prev), 2 = end (no next),
//            3 = middle (gradients). Splines only (next_id/prev_id).
auto apply_node_delta_h = [&](map::SplineInst& s, int node_kind, float k) {
    auto* nxt = s.next_id ? find_spline_by_id(s.next_id) : nullptr;
    auto* prv = s.prev_id ? find_spline_by_id(s.prev_id) : nullptr;
    auto set_h = [](map::SplineInst& sp, float dh) {
        sp.use_delta_height = true;
        sp.is_spline_h = true;
        sp.delta_height = dh;
    };
    if (node_kind == 3) {
        const float g = (s.length > 0.1f) ? (k * 400.f) / s.length : 0.f;
        s.start_grad = g;
        s.end_grad = -g;
        s.use_delta_height = false;
        if (nxt) {
            const float end_z = spline::evaluate_spline(*nxt, nxt->length).z;
            nxt->start_grad = -g;
            set_h(*nxt, end_z - nxt->position.z);
        }
        if (prv) {
            const float end_z = spline::evaluate_spline(*prv, prv->length).z;
            prv->end_grad = g;
            set_h(*prv, end_z - prv->position.z);
        }
        return;
    }
    if (node_kind == 1) { // start, no prev
        set_h(s, s.delta_height - k);
        return;
    }
    // end: raise the final height and give the next the opposite delta_h so its own end stays put.
    set_h(s, s.delta_height + k);
    if (nxt) {
        nxt->position.z += k;
        set_h(*nxt, -k);
    }
};

auto wrap180 = [](float a) {
    while (a > 180.f) a -= 360.f;
    while (a < -180.f) a += 360.f;
    return a;
};

// Remove temporary fill splines created during current grab (relink neighbors).
// Debug: dump every segment in a chain from head id through next_id links.
auto dump_fill_chain = [&](const char* tag, std::int32_t head_id, std::int32_t expect_tail) {
    log::info(std::string("[chain_dbg] === DUMP ") + tag + " head=" + std::to_string(head_id) +
              " expect_tail=" + std::to_string(expect_tail));
    std::int32_t id = head_id;
    for (int hop = 0; hop < 32 && id != 0; ++hop) {
        auto* sp = find_spline_by_id(id);
        if (!sp) {
            log::info("[chain_dbg]   MISSING id=" + std::to_string(id));
            break;
        }
        const auto e = spline::evaluate_spline(*sp, sp->length);
        log::info("[chain_dbg]   [" + std::to_string(hop) + "] id=" + std::to_string(sp->id) +
                  " prev=" + std::to_string(sp->prev_id) + " next=" + std::to_string(sp->next_id) +
                  " sli=" + sp->sli_path.filename().string() +
                  " start=(" + std::to_string(sp->position.x) + "," + std::to_string(sp->position.y) +
                  "," + std::to_string(sp->position.z) + ") h=" + std::to_string(sp->heading_deg) +
                  " L=" + std::to_string(sp->length) + " R=" + std::to_string(sp->radius) +
                  " end=(" + std::to_string(e.x) + "," + std::to_string(e.y) + "," + std::to_string(e.z) +
                  ") eh=" + std::to_string(e.heading_rad * (180.f / 3.14159265f)));
        if (id == expect_tail) break;
        id = sp->next_id;
    }
    log::info(std::string("[chain_dbg] === END DUMP ") + tag);
};

auto clear_fill_temps = [&]() {
    std::unordered_set<std::int32_t> drop(fill_temp_ids.begin(), fill_temp_ids.end());
    for (std::int32_t tid : fill_temp_pool) drop.insert(tid);
    if (drop.empty()) return;
    for (std::int32_t tid : drop) {
        map::SplineInst* t = find_spline_by_id(tid);
        if (!t) continue;
        const std::int32_t p = t->prev_id;
        const std::int32_t n = t->next_id;
        if (auto* pp = find_spline_by_id(p)) {
            if (pp->next_id == tid) pp->next_id = n;
        }
        if (auto* nn = find_spline_by_id(n)) {
            if (nn->prev_id == tid) nn->prev_id = p;
        }
        // Hide GPU so undo/cancel no deja fantasma de temp de rellenar cadena
        if (auto* g = find_spline_gpu(tid)) {
            g->alive = false;
            for (const auto c : g->chunks) renderer.set_chunk_visible(c, false);
        }
    }
    edit_splines.erase(std::remove_if(edit_splines.begin(), edit_splines.end(),
                                      [&](const map::SplineInst& s) { return drop.count(s.id) != 0; }),
                       edit_splines.end());
    transient_splines.erase(std::remove_if(transient_splines.begin(), transient_splines.end(),
                                           [&](const map::SplineInst& s) { return drop.count(s.id) != 0; }),
                            transient_splines.end());
    fill_temp_ids.clear();
    fill_temp_pool.clear();
};

// Unlink temps from chain but KEEP them in edit_splines so next AA can reuse (no flicker).
auto park_fill_temps = [&]() {
    // Unlink active temps and move them into the reuse pool.
    for (std::int32_t tid : fill_temp_ids) {
        map::SplineInst* t = find_spline_by_id(tid);
        if (!t) continue;
        const std::int32_t p = t->prev_id;
        const std::int32_t n = t->next_id;
        if (auto* pp = find_spline_by_id(p)) {
            if (pp->next_id == tid) pp->next_id = n;
        }
        if (auto* nn = find_spline_by_id(n)) {
            if (nn->prev_id == tid) nn->prev_id = p;
        }
        t->prev_id = 0;
        t->next_id = 0;
        fill_temp_pool.push_back(tid);
    }
    fill_temp_ids.clear();
};

// Delete pool leftovers that were not taken this frame.
auto purge_unused_fill_temps = [&]() {
    if (fill_temp_pool.empty()) return;
    std::unordered_set<std::int32_t> drop(fill_temp_pool.begin(), fill_temp_pool.end());
    for (std::int32_t tid : drop) {
        if (auto* g = find_spline_gpu(tid)) {
            g->alive = false;
            for (const auto c : g->chunks) renderer.set_chunk_visible(c, false);
        }
    }
    edit_splines.erase(std::remove_if(edit_splines.begin(), edit_splines.end(),
                                      [&](const map::SplineInst& s) { return drop.count(s.id) != 0; }),
                       edit_splines.end());
    transient_splines.erase(std::remove_if(transient_splines.begin(), transient_splines.end(),
                                           [&](const map::SplineInst& s) { return drop.count(s.id) != 0; }),
                            transient_splines.end());
    fill_temp_pool.clear();
};




// Try one segment from pose (x0,y0,h0) to (x1,y1,h1). Returns residual heading+pos score.
auto try_end_heading = [&](float x0, float y0, float h0, float x1, float y1, float h1,
                           float max_r, float& out_len, float& out_rad) -> bool {
    if (!spline::fit_spline_to_end_heading(x0, y0, h0, x1, y1, h1, out_len, out_rad))
        return false;
    if (out_len < 0.25f) return false;
    if (max_r > 0.f && std::fabs(out_rad) > max_r * 1.05f) return false;
    return std::isfinite(out_len) && std::isfinite(out_rad);
};

// Bridge left → right (right.start pose is TARGET, not moved).
// Guarantees geometric continuity: each node starts at evaluate(prev).
// Strategy: 1) single end-heading fit on left  2) biarc  3) N equal chords with forced snap.
// Exact AA bridge left → right.start. Reuses existing fill temps when possible (no flicker).
// Prefers G1 (pos+heading). force_xy only if end-heading fit leaves gap > 5cm.
auto fill_bridge_between = [&](map::SplineInst& left, map::SplineInst& right,
                               const map::SplineInst& template_sp, float max_r, int /*max_new*/) -> int {
    const float tx = right.position.x;
    const float ty = right.position.y;
    const float th = right.heading_deg;
    const float x0 = left.position.x;
    const float y0 = left.position.y;
    const float z0 = left.position.z;
    const float h0 = left.heading_deg;
    const float dh = std::fabs(wrap180(th - h0));
    const float rlim = (max_r > 0.f) ? max_r * 2.5f : 0.f;

    auto fit_eh = [&](float ax, float ay, float ah, float bx, float by, float bh,
                      float& L, float& R) -> bool {
        if (!spline::fit_spline_to_end_heading(ax, ay, ah, bx, by, bh, L, R)) return false;
        if (L < 0.25f || !std::isfinite(L) || !std::isfinite(R)) return false;
        if (rlim > 0.f && std::fabs(R) > rlim * 1.05f) return false;
        return true;
    };
    auto fit_xy = [&](float ax, float ay, float ah, float bx, float by, float& L, float& R) {
        spline::fit_spline_to_xy(ax, ay, ah, bx, by, L, R);
        if (!std::isfinite(L) || L < 0.25f) L = 0.25f;
        if (!std::isfinite(R)) R = 0.f;
        if (rlim > 0.f && std::fabs(R) > rlim * 1.05f)
            R = (R >= 0.f ? rlim : -rlim);
    };
    auto eval_end = [&](const map::SplineInst& s) {
        return spline::evaluate_spline(s, s.length);
    };
    auto gap_of = [&](const map::SplineInst& s, float bx, float by) {
        const auto e = eval_end(s);
        return std::hypot(e.x - bx, e.y - by);
    };
    auto eh_of = [&](const map::SplineInst& s, float bh) {
        const auto e = eval_end(s);
        return std::fabs(wrap180(e.heading_rad * (180.f / 3.14159265f) - bh));
    };

    // Pop one reusable temp id if any (caller may have left previous frame temps)
    auto take_temp = [&]() -> map::SplineInst* {
        while (!fill_temp_pool.empty()) {
            const std::int32_t tid = fill_temp_pool.back();
            fill_temp_pool.pop_back();
            if (auto* t = find_spline_by_id(tid)) return t;
        }
        return nullptr;
    };

    // --- 1) Single G1 ---
    if (dh < 40.f) {
        float L = 0.f, R = 0.f;
        if (fit_eh(x0, y0, h0, tx, ty, th, L, R)) {
            left.length = L; left.radius = R;
            if (gap_of(left, tx, ty) < 0.05f && eh_of(left, th) < 1.0f) {
                left.next_id = right.id; right.prev_id = left.id;
                // No force_xy: preserves heading match. Residual < 5cm is acceptable for G1.
                log::info("[chain_dbg] AA single G1 gap=" + std::to_string(gap_of(left, tx, ty)) +
                          " eh=" + std::to_string(eh_of(left, th)));
                return 0;
            }
        }
    }

    // --- 2) Biarc with reused or new temp ---
    float L0 = 0, R0 = 0, L1 = 0, R1 = 0, jx = 0, jy = 0, hj = 0;
    if (compute_biarc_omsi(x0, y0, h0, tx, ty, th, 0.85f, L0, R0, L1, R1, jx, jy, hj) &&
        L0 >= 0.25f && L1 >= 0.25f) {
        // No clampear R: el biarc G1 ya resolvió pos+heading; clamp lo rompe.
        left.length = L0; left.radius = R0;

        map::SplineInst* ns = take_temp();
        bool created = false;
        if (!ns) {
            map::SplineInst neu = template_sp;
            neu.id = next_spline_id();
            neu.sli_path = template_sp.sli_path;
            neu.start_grad = 0.f; neu.end_grad = 0.f;
            edit_splines.push_back(neu);
            transient_splines.push_back(neu);
            ns = find_spline_by_id(neu.id);
            created = true;
        }
        if (ns) {
            // Confiar en el biarc nuevo (G1): NO re-fit XY que destroza el heading.
            const auto e = eval_end(left);
            ns->position.x = e.x; ns->position.y = e.y; ns->position.z = e.z;
            ns->heading_deg = e.heading_rad * (180.f / 3.14159265f);
            ns->sli_path = template_sp.sli_path;
            ns->length = L1;
            ns->radius = R1;
            ns->prev_id = left.id; ns->next_id = right.id;
            left.next_id = ns->id; right.prev_id = ns->id;
            fill_temp_ids.push_back(ns->id);
            for (auto& t : transient_splines) {
                if (t.id == ns->id) { t = *ns; break; }
            }
            log::info(std::string("[chain_dbg] AA biarc ") + (created ? "NEW" : "REUSE") +
                      " id=" + std::to_string(ns->id) +
                      " L0=" + std::to_string(L0) + " R0=" + std::to_string(R0) +
                      " L1=" + std::to_string(L1) + " R1=" + std::to_string(R1) +
                      " gap=" + std::to_string(gap_of(*ns, tx, ty)) +
                      " eh=" + std::to_string(eh_of(*ns, th)));
            return 1;
        }
    }

    // --- 3) Fallback XY (exact pos; heading may differ) ---
    {
        float L = 0.f, R = 0.f;
        fit_xy(x0, y0, h0, tx, ty, L, R);
        left.length = L; left.radius = R;
        left.next_id = right.id; right.prev_id = left.id;
        log::info("[chain_dbg] AA fallback XY gap=" + std::to_string(gap_of(left, tx, ty)) +
                  " eh=" + std::to_string(eh_of(left, th)));
    }
    return 0;
};

// Exact AA head.end → fixed (fex,fey,feh). Reuses temps. Prefers G1 heading at end.
// Exact AA: head.end → fixed (fex,fey,feh) via tail (+ optional temp).
// Logs biarc failure reasons; forces 2-seg path so we rarely leave eh~25°.
// SOLO por id: nunca referencias a edit_splines (push_back invalida → AV).
auto fill_to_fixed_end = [&](std::int32_t head_id, std::int32_t tail_id,
                             float fex, float fey, float feh, float max_r, int /*max_new*/) -> int {
    auto* head0 = (head_id != 0) ? find_spline_by_id(head_id) : nullptr;
    auto* tail0 = find_spline_by_id(tail_id);
    if (!tail0) return -1;
    // Snapshot de geometría de partida (head puede ser id=0 → joint en tail start)
    float x0, y0, z0, h0;
    std::filesystem::path sli_keep = tail0->sli_path;
    if (head0) {
        const auto e0 = spline::evaluate_spline(*head0, head0->length);
        x0 = e0.x; y0 = e0.y; z0 = e0.z;
        h0 = e0.heading_rad * (180.f / 3.14159265f);
        sli_keep = head0->sli_path.empty() ? tail0->sli_path : head0->sli_path;
    } else {
        x0 = tail0->position.x; y0 = tail0->position.y; z0 = tail0->position.z;
        h0 = tail0->heading_deg;
    }
    const float dist = std::hypot(fex - x0, fey - y0);
    const float dh = std::fabs(wrap180(feh - h0));
    const float rlim = (max_r > 0.f) ? max_r * 3.0f : 0.f;

    auto fit_eh = [&](float ax, float ay, float ah, float bx, float by, float bh,
                      float& L, float& R) -> bool {
        if (!spline::fit_spline_to_end_heading(ax, ay, ah, bx, by, bh, L, R)) return false;
        if (L < 0.25f || !std::isfinite(L) || !std::isfinite(R)) return false;
        if (rlim > 0.f && std::fabs(R) > rlim * 1.05f) return false;
        return true;
    };
    auto fit_xy = [&](float ax, float ay, float ah, float bx, float by, float& L, float& R) {
        spline::fit_spline_to_xy(ax, ay, ah, bx, by, L, R);
        if (!std::isfinite(L) || L < 0.25f) L = 0.25f;
        if (!std::isfinite(R)) R = 0.f;
        if (rlim > 0.f && std::fabs(R) > rlim * 1.05f)
            R = (R >= 0.f ? rlim : -rlim);
    };
    auto gap_of_id = [&](std::int32_t id, float bx, float by) -> float {
        auto* s = find_spline_by_id(id);
        if (!s) return 1e9f;
        const auto e = spline::evaluate_spline(*s, s->length);
        return std::hypot(e.x - bx, e.y - by);
    };
    auto eh_of_id = [&](std::int32_t id, float bh) -> float {
        auto* s = find_spline_by_id(id);
        if (!s) return 1e9f;
        const auto e = spline::evaluate_spline(*s, s->length);
        return std::fabs(wrap180(e.heading_rad * (180.f / 3.14159265f) - bh));
    };
    auto take_temp = [&]() -> std::int32_t {
        while (!fill_temp_pool.empty()) {
            const std::int32_t tid = fill_temp_pool.back();
            fill_temp_pool.pop_back();
            if (find_spline_by_id(tid)) return tid;
        }
        return 0;
    };
    auto ensure_temp = [&]() -> std::int32_t {
        if (std::int32_t tid = take_temp()) {
            log::info("[chain_dbg] ensure_temp REUSE id=" + std::to_string(tid) +
                      " pool_left=" + std::to_string(fill_temp_pool.size()));
            return tid;
        }
        map::SplineInst neu{};
        neu.id = next_spline_id();
        neu.sli_path = sli_keep;
        neu.start_grad = 0; neu.end_grad = 0;
        neu.prev_id = 0; neu.next_id = 0;
        neu.position = {x0, y0, z0};
        neu.heading_deg = h0;
        neu.length = 0.25f; neu.radius = 0.f;
        const std::size_t n_before = edit_splines.size();
        edit_splines.push_back(neu);
        transient_splines.push_back(neu);
        log::info("[chain_dbg] ensure_temp NEW id=" + std::to_string(neu.id) +
                  " edit_splines " + std::to_string(n_before) + "→" +
                  std::to_string(edit_splines.size()) +
                  " (refs a edit_splines INVALIDADAS)");
        return neu.id;
    };

    if (auto* h = find_spline_by_id(head_id)) h->next_id = 0;
    if (auto* t = find_spline_by_id(tail_id)) { t->prev_id = 0; t->next_id = 0; }

    log::info("[chain_dbg] AA fixed begin head=" + std::to_string(head_id) +
              " tail=" + std::to_string(tail_id) +
              " from=(" + std::to_string(x0) + "," + std::to_string(y0) + ") h0=" + std::to_string(h0) +
              " to=(" + std::to_string(fex) + "," + std::to_string(fey) + ") feh=" + std::to_string(feh) +
              " dist=" + std::to_string(dist) + " dh=" + std::to_string(dh));

    // 1) Single G1
    if (dh < 45.f && dist > 0.3f) {
        float L = 0, R = 0;
        if (fit_eh(x0, y0, h0, fex, fey, feh, L, R)) {
            if (auto* t = find_spline_by_id(tail_id)) {
                t->position.x = x0; t->position.y = y0; t->position.z = z0;
                t->heading_deg = h0; t->length = L; t->radius = R;
            }
            const float g = gap_of_id(tail_id, fex, fey);
            const float eh = eh_of_id(tail_id, feh);
            if (g < 0.05f && eh < 1.0f) {
                if (auto* h = find_spline_by_id(head_id)) h->next_id = tail_id;
                if (auto* t = find_spline_by_id(tail_id)) t->prev_id = head_id;
                log::info("[chain_dbg] AA fixed single G1 gap=" + std::to_string(g) +
                          " eh=" + std::to_string(eh));
                return 0;
            }
            log::info("[chain_dbg] AA fixed single reject gap=" + std::to_string(g) +
                      " eh=" + std::to_string(eh));
        } else {
            log::info("[chain_dbg] AA fixed single fit_eh failed");
        }
    }

    auto apply_two_seg = [&](float aL, float aR, float bL, float bR, const char* tag) -> int {
        const std::int32_t ns_id = ensure_temp();
        if (ns_id == 0) return -1;
        auto* ns = find_spline_by_id(ns_id);
        auto* t = find_spline_by_id(tail_id);
        if (!ns || !t) return -1;
        ns->position.x = x0; ns->position.y = y0; ns->position.z = z0;
        ns->heading_deg = h0;
        ns->length = aL; ns->radius = aR;
        ns->sli_path = sli_keep;
        ns->prev_id = head_id; ns->next_id = tail_id;
        ns->start_grad = 0; ns->end_grad = 0;
        {
            const auto e = spline::evaluate_spline(*ns, ns->length);
            t->position.x = e.x; t->position.y = e.y; t->position.z = e.z;
            t->heading_deg = e.heading_rad * (180.f / 3.14159265f);
        }
        float L = bL, R = bR;
        const bool trust = (tag && (tag[0] == 'l' || tag[0] == 'L'));
        if (!trust) {
            if (!fit_eh(t->position.x, t->position.y, t->heading_deg, fex, fey, feh, L, R))
                fit_xy(t->position.x, t->position.y, t->heading_deg, fex, fey, L, R);
        }
        t->length = L; t->radius = R;
        const float g0 = gap_of_id(tail_id, fex, fey);
        const float eh0 = eh_of_id(tail_id, feh);
        if (g0 > 0.08f && eh0 > 1.0f) {
            fit_xy(t->position.x, t->position.y, t->heading_deg, fex, fey, L, R);
            t->length = L; t->radius = R;
        } else if (g0 > 0.25f && trust) {
            fit_xy(t->position.x, t->position.y, t->heading_deg, fex, fey, L, R);
            t->length = L; t->radius = R;
        }
        if (auto* h = find_spline_by_id(head_id)) h->next_id = ns_id;
        if (auto* t2 = find_spline_by_id(tail_id)) t2->prev_id = ns_id;
        fill_temp_ids.push_back(ns_id);
        for (auto& tr : transient_splines) {
            if (tr.id == ns_id) {
                if (auto* n = find_spline_by_id(ns_id)) tr = *n;
                break;
            }
        }
        log::info(std::string("[chain_dbg] AA fixed 2seg ") + tag +
                  " id=" + std::to_string(ns_id) +
                  " gap=" + std::to_string(gap_of_id(tail_id, fex, fey)) +
                  " eh=" + std::to_string(eh_of_id(tail_id, feh)) +
                  " (raw gap=" + std::to_string(g0) + " eh=" + std::to_string(eh0) + ")");
        return 1;
    };

    // 2) Library biarc
    float L0 = 0, R0 = 0, L1 = 0, R1 = 0, jx = 0, jy = 0, hj = 0;
    const bool bi = compute_biarc_omsi(x0, y0, h0, fex, fey, feh, 0.85f, L0, R0, L1, R1, jx, jy, hj);
    log::info("[chain_dbg] AA fixed biarc_lib ok=" + std::to_string(bi ? 1 : 0) +
              " L0=" + std::to_string(L0) + " R0=" + std::to_string(R0) +
              " L1=" + std::to_string(L1) + " R1=" + std::to_string(R1));
    if (bi && L0 >= 0.25f && L1 >= 0.25f) {
        if (rlim > 0.f) {
            if (std::fabs(R0) > rlim) R0 = (R0 >= 0.f ? rlim : -rlim);
            if (std::fabs(R1) > rlim) R1 = (R1 >= 0.f ? rlim : -rlim);
        }
        int r = apply_two_seg(L0, R0, L1, R1, "lib");
        if (r >= 0) return r;
    }

    // 3) Manual 2-seg with mid heading blend
    {
        const float mx = 0.5f * (x0 + fex);
        const float my = 0.5f * (y0 + fey);
        const float mh = h0 + 0.5f * wrap180(feh - h0);
        float aL = 0, aR = 0, bL = 0, bR = 0;
        if (!fit_eh(x0, y0, h0, mx, my, mh, aL, aR))
            fit_xy(x0, y0, h0, mx, my, aL, aR);
        // Temporary geometry only (no live pointer into edit_splines)
        map::SplineInst tmp{};
        tmp.position.x = x0; tmp.position.y = y0; tmp.position.z = z0;
        tmp.heading_deg = h0; tmp.length = aL; tmp.radius = aR;
        const auto e1 = spline::evaluate_spline(tmp, tmp.length);
        const float x1 = e1.x, y1 = e1.y;
        const float h1 = e1.heading_rad * (180.f / 3.14159265f);
        if (!fit_eh(x1, y1, h1, fex, fey, feh, bL, bR))
            fit_xy(x1, y1, h1, fex, fey, bL, bR);
        log::info("[chain_dbg] AA fixed manual2 aL=" + std::to_string(aL) +
                  " bL=" + std::to_string(bL) + " mh=" + std::to_string(mh));
        int r = apply_two_seg(aL, aR, bL, bR, "manual");
        if (r >= 0) return r;
    }

    // 4) Last resort — XY only via ids (never raw head/tail refs)
    if (auto* t = find_spline_by_id(tail_id)) {
        t->position.x = x0; t->position.y = y0; t->position.z = z0;
        t->heading_deg = h0;
        float L = 0, R = 0;
        fit_xy(x0, y0, h0, fex, fey, L, R);
        t->length = L; t->radius = R;
    }
    if (auto* h = find_spline_by_id(head_id)) h->next_id = tail_id;
    if (auto* t = find_spline_by_id(tail_id)) t->prev_id = head_id;
    log::info("[chain_dbg] AA fixed FALLBACK XY gap=" + std::to_string(gap_of_id(tail_id, fex, fey)) +
              " eh=" + std::to_string(eh_of_id(tail_id, feh)) + " (heading NOT matched)");
    return 0;
};


// ---------------------------------------------------------------------------
// Screen-space handle state (per frame)
// ---------------------------------------------------------------------------

struct SplineHandleHit {
    enum class Kind { None, UnionBall, GradCircle } kind = Kind::None;
    std::int32_t spline_id = 0;       // primary spline
    std::int32_t partner_id = 0;      // other side of union (if any)
    bool is_start_side = false;       // union at start of primary?
    float screen_x = 0.f;
    float screen_y = 0.f;
    float radius_px = 8.f;
};

// Project world → screen. Must match screen_dist2: WVP is stored transposed for HLSL,
// so un-transpose before XMVector4Transform. Returns false if behind camera.
auto world_xy_to_screen = [&](float wx, float wy, float wz, float fw, float fh,
                              const float* wvp_t, float& out_sx, float& out_sy) -> bool {
    DirectX::XMFLOAT4X4 wvp_t_m{};
    std::memcpy(&wvp_t_m, wvp_t, sizeof(wvp_t_m));
    const DirectX::XMMATRIX wvp = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&wvp_t_m));
    DirectX::XMFLOAT4 clip{};
    DirectX::XMStoreFloat4(&clip,
                           DirectX::XMVector4Transform(DirectX::XMVectorSet(wx, wy, wz, 1.f), wvp));
    if (clip.w <= 1.e-5f) return false;
    out_sx = (clip.x / clip.w * 0.5f + 0.5f) * fw;
    out_sy = (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * fh; // top-left origin
    return true;
};

// Build list of visible handles for selected + hovered splines.
// Always start+end+center. Linked junctions → ONE ball (merge by link or proximity).
auto collect_spline_handles =
    [&](std::vector<SplineHandleHit>& out, float fw, float fh, const float* wvp,
        int hovered_spline_idx, float union_pct, float grad_pct) {
        out.clear();
        const float min_side = (std::min)(fw, fh);
        const float union_r = (std::max)(6.f, union_pct * min_side * 0.5f);
        const float grad_r = (std::max)(5.f, grad_pct * min_side * 0.5f);

        struct Cand {
            SplineHandleHit h;
            float wx, wy, wz;
            std::int32_t link_lo = 0; // min(id,partner) for junction key
            std::int32_t link_hi = 0;
        };
        std::vector<Cand> cands;

        auto add_cand = [&](SplineHandleHit::Kind kind, std::int32_t sid, std::int32_t partner,
                            bool start_side, float wx, float wy, float wz, float rpx) {
            float sx = 0.f, sy = 0.f;
            if (!world_xy_to_screen(wx, wy, wz, fw, fh, wvp, sx, sy)) return;
            Cand c{};
            c.h.kind = kind;
            c.h.spline_id = sid;
            c.h.partner_id = partner;
            c.h.is_start_side = start_side;
            c.h.screen_x = sx;
            c.h.screen_y = sy;
            c.h.radius_px = rpx;
            c.wx = wx;
            c.wy = wy;
            c.wz = wz;
            if (kind == SplineHandleHit::Kind::UnionBall && partner != 0) {
                c.link_lo = (std::min)(sid, partner);
                c.link_hi = (std::max)(sid, partner);
            }
            cands.push_back(c);
        };

        // Primero: anchors de splines visibles (sel/hov/nodo)
        struct Anchor {
            float x, y, z;
        };
        std::vector<Anchor> anchors;
        std::unordered_set<std::int32_t> included_ids;
        auto spline_ok = [&](const map::SplineInst& sp) -> bool {
            if (sp.id == 0) return false;
            if (!std::isfinite(sp.position.x) || !std::isfinite(sp.position.y) ||
                !std::isfinite(sp.position.z))
                return false;
            if (!std::isfinite(sp.length) || sp.length < 0.f || sp.length > 1.e5f) return false;
            if (!std::isfinite(sp.radius) || !std::isfinite(sp.heading_deg)) return false;
            return true;
        };
        auto include_sp = [&](std::size_t i) {
            if (i >= edit_splines.size()) return;
            const auto& sp = edit_splines[i];
            if (!spline_ok(sp)) return;
            if (included_ids.count(sp.id)) return;
            included_ids.insert(sp.id);
            float mid_t = sp.length * 0.5f;
            if (mid_t < 0.f) mid_t = 0.f;
            const auto mid = spline::evaluate_spline(sp, mid_t);
            if (std::isfinite(mid.x) && std::isfinite(mid.y))
                add_cand(SplineHandleHit::Kind::GradCircle, sp.id, 0, false, mid.x, mid.y, mid.z, grad_r);
            add_cand(SplineHandleHit::Kind::UnionBall, sp.id, sp.prev_id, true, sp.position.x,
                     sp.position.y, sp.position.z, union_r);
            anchors.push_back({sp.position.x, sp.position.y, sp.position.z});
            const auto end = spline_end_sample(sp);
            if (std::isfinite(end.x) && std::isfinite(end.y)) {
                add_cand(SplineHandleHit::Kind::UnionBall, sp.id, sp.next_id, false, end.x, end.y, end.z,
                         union_r);
                anchors.push_back({end.x, end.y, end.z});
            }
        };
        // Only the hovered or selected spline gets handles. No prev/next chain node, no
        // proximity neighbours, and no "node being dragged" extras — the user asked for
        // exactly hover/click only.
        for (std::size_t i = 0; i < edit_splines.size(); ++i) {
            const auto& sp = edit_splines[i];
            if (!spline_ok(sp)) continue;
            const bool is_sel = (selected_spline == static_cast<int>(i));
            const bool is_hov = (hovered_spline_idx == static_cast<int>(i));
            // Keep handles alive for the spline whose handle is selected, even when nothing is
            // hovered/selected (so a clicked node stays visible and red across frames).
            const bool is_handle_sel = (chain_sel_handle_spline != 0 && sp.id == chain_sel_handle_spline);
            if (is_sel || is_hov || is_handle_sel) include_sp(i);
        }
        (void)anchors;

        // Merge: same link pair (id↔partner) OR within 0.15 m (mismo nodo).
        // CRÍTICO multi-end: varias A terminan + B inicia en el mismo punto.
        // Identity = un ENDER (is_start_side=false); partner = un STARTER (is_start_side=true).
        // NUNCA poner otro ender como partner (rompe G: se trata como starter y se mueve su start).
        constexpr float kMergeM = 0.15f; // ~15 cm mismo nodo
        std::vector<char> used(cands.size(), 0);
        for (std::size_t i = 0; i < cands.size(); ++i) {
            if (used[i]) continue;
            if (cands[i].h.kind == SplineHandleHit::Kind::GradCircle) {
                out.push_back(cands[i].h);
                used[i] = 1;
                continue;
            }
            SplineHandleHit best = cands[i].h;
            float bx = cands[i].wx, by = cands[i].wy;
            int nmerge = 1;
            used[i] = 1;
            std::int32_t starter_partner = 0; // solo un ball con is_start_side
            if (best.is_start_side)
                starter_partner = best.spline_id;
            for (std::size_t j = i + 1; j < cands.size(); ++j) {
                if (used[j]) continue;
                if (cands[j].h.kind != SplineHandleHit::Kind::UnionBall) continue;
                bool same_link = false;
                if (cands[i].link_lo != 0 && cands[i].link_lo == cands[j].link_lo &&
                    cands[i].link_hi == cands[j].link_hi)
                    same_link = true;
                if (!same_link && cands[i].h.partner_id == cands[j].h.spline_id &&
                    cands[j].h.partner_id == cands[i].h.spline_id)
                    same_link = true;
                const float d = std::hypot(cands[j].wx - cands[i].wx, cands[j].wy - cands[i].wy);
                if (!same_link && d > kMergeM) continue;
                used[j] = 1;
                nmerge++;
                bx += cands[j].wx;
                by += cands[j].wy;
                // Preferir identity = ender (no start_side)
                if (best.is_start_side && !cands[j].h.is_start_side) {
                    best.spline_id = cands[j].h.spline_id;
                    best.is_start_side = false;
                }
                // Partner SOLO si el cand es start_side (B inicia aquí)
                if (cands[j].h.is_start_side && starter_partner == 0)
                    starter_partner = cands[j].h.spline_id;
            }
            // Partner = starter real, nunca otro ender
            if (starter_partner != 0 && starter_partner != best.spline_id)
                best.partner_id = starter_partner;
            else if (!best.is_start_side)
                best.partner_id = 0; // ender solo; partners se detectan por proximidad en G
            if (nmerge > 1) {
                bx /= static_cast<float>(nmerge);
                by /= static_cast<float>(nmerge);
                float sx = 0.f, sy = 0.f;
                if (world_xy_to_screen(bx, by, cands[i].wz, fw, fh, wvp, sx, sy)) {
                    best.screen_x = sx;
                    best.screen_y = sy;
                }
            }
            out.push_back(best);
        }
    };

// Pick nearest handle under mouse. Prefer UnionBall over GradCircle when overlapping.
auto pick_spline_handle = [&](float mx, float my, const std::vector<SplineHandleHit>& handles)
    -> int /* index or -1 */ {
    int best = -1;
    float best_d2 = 1e30f;
    int best_prio = -1; // higher = preferred
    for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
        const auto& h = handles[static_cast<std::size_t>(i)];
        const float dx = mx - h.screen_x;
        const float dy = my - h.screen_y;
        const float d2 = dx * dx + dy * dy;
        const float r2 = h.radius_px * h.radius_px;
        if (d2 > r2) continue;
        const int prio = (h.kind == SplineHandleHit::Kind::UnionBall) ? 2 : 1;
        if (prio > best_prio || (prio == best_prio && d2 < best_d2)) {
            best = i;
            best_d2 = d2;
            best_prio = prio;
        }
    }
    return best;
};

// ---------------------------------------------------------------------------
// Isolated-spline center drag (no prev/next): keep start & end fixed, refit
// rotation + length + radius so the mid-point follows the mouse.
// Axis lock: 0=free, 1=X only, 2=Y only, 3=Z/grad only
// ---------------------------------------------------------------------------
auto fit_isolated_center_drag = [&](map::SplineInst& sp, float mid_x, float mid_y, float mid_z,
                                   int axis_lock, float max_r) {
    const auto end0 = spline_end_sample(sp);
    const float sx = sp.position.x;
    const float sy = sp.position.y;
    const float sz = sp.position.z;
    const float ex = end0.x;
    const float ey = end0.y;
    const float ez = end0.z;

    if (axis_lock == 3) {
        // Z / gradient only: adjust start_grad / end_grad so mid height ≈ mid_z
        // Simple symmetric model: start_grad = g, end_grad = -g
        // Height at mid ≈ sz + (length/2) * (g/100) roughly for small slopes.
        // Use delta_height path when available.
        const float target_mid_z = mid_z;
        const float avg_end = 0.5f * (sz + ez);
        const float rise = target_mid_z - avg_end;
        // Approximate: peak of a linear-grad pair is rise ≈ length * |g| / 400
        // Solve for g.
        if (sp.length > 0.1f) {
            float g = (rise * 400.f) / sp.length;
            if (g > 1000.f) g = 1000.f;
            if (g < -1000.f) g = -1000.f;
            sp.start_grad = g;
            sp.end_grad = -g;
            sp.use_delta_height = false;
        }
        return;
    }

    // Horizontal fit: keep start & end XY fixed, place a circular arc whose
    // midpoint is as close as possible to (mid_x, mid_y).
    float aim_x = mid_x;
    float aim_y = mid_y;
    if (axis_lock == 1) {
        // X only relative to initial heading
        const float hr = sp.heading_deg * (3.14159265f / 180.f);
        const float fx = std::cos(hr), fy = std::sin(hr); // forward
        const float rx = -fy, ry = fx;                    // right
        // Project mouse offset onto right axis only from original mid
        const auto mid0 = spline::evaluate_spline(sp, sp.length * 0.5f);
        const float ox = mid_x - mid0.x;
        const float oy = mid_y - mid0.y;
        const float lat = ox * rx + oy * ry;
        aim_x = mid0.x + lat * rx;
        aim_y = mid0.y + lat * ry;
    } else if (axis_lock == 2) {
        const float hr = sp.heading_deg * (3.14159265f / 180.f);
        const float fx = std::cos(hr), fy = std::sin(hr);
        const auto mid0 = spline::evaluate_spline(sp, sp.length * 0.5f);
        const float ox = mid_x - mid0.x;
        const float oy = mid_y - mid0.y;
        const float along = ox * fx + oy * fy;
        aim_x = mid0.x + along * fx;
        aim_y = mid0.y + along * fy;
    }

    // Chord from start to end
    const float cdx = ex - sx;
    const float cdy = ey - sy;
    const float chord = std::sqrt(cdx * cdx + cdy * cdy);
    if (chord < 0.05f) return;

    // Mid of chord
    const float cmx = 0.5f * (sx + ex);
    const float cmy = 0.5f * (sy + ey);
    // Lateral offset of aim from chord mid
    const float nx = -cdy / chord; // unit normal
    const float ny = cdx / chord;
    float lat = (aim_x - cmx) * nx + (aim_y - cmy) * ny;

    // For a circular arc, sagitta = |R| - sqrt(R^2 - (chord/2)^2) = |lat|
    // Solve for R: R = (sagitta^2 + (chord/2)^2) / (2 sagitta)
    float rad = 0.f;
    float len = chord;
    if (std::fabs(lat) > 0.01f) {
        const float half = chord * 0.5f;
        rad = (lat * lat + half * half) / (2.f * lat);
        if (max_r > 0.f && std::fabs(rad) > max_r) {
            rad = (rad >= 0.f) ? max_r : -max_r;
        }
        // arc length = |R| * 2 * asin(half/|R|)
        const float ar = std::fabs(rad);
        const float ratio = (std::min)(1.f, half / ar);
        len = ar * 2.f * std::asin(ratio);
        // Heading: tangent at start
        // For circle centre relative to start...
        // Keep existing heading for stability; only update length/radius.
    }
    if (len < 0.05f) len = 0.05f;
    sp.length = len;
    sp.radius = rad;
    // Height: optional blend
    (void)mid_z;
};
