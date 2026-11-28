// ============================================================================
// MOD: path_rails / traffic - aislado (reglas de path, pintar, rieles helper).
// Motor y estado compartido. Editar solo aqui para trafico/rieles de path.
// kPathRailZLift defined in map_editor_run_setup_02.inc.cpp before this include.
// ============================================================================
    auto rule_ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    };
    // [PERF] id -> index of edit_objects / edit_splines for the rails' owner lookups. Every rail looked its
    // owner up with a scan of all objects, twice a frame (visual radius + tint): with 4 tiles streamed (47k
    // objects, thousands of rails) that was 50-150 ms a frame (interaction_chunk_highlights).
    struct RailOwnerIndex {
        const void* data = nullptr;
        std::size_t n = 0;
        std::unordered_map<std::int32_t, std::size_t> idx;
    };
    RailOwnerIndex rail_obj_index, rail_spl_index;
    auto rail_owner_find = [](RailOwnerIndex& ix, const auto& vec, std::int32_t id) -> std::ptrdiff_t {
        auto rebuild = [&] {
            ix.idx.clear();
            ix.idx.reserve(vec.size() * 2 + 16);
            for (std::size_t i = 0; i < vec.size(); ++i) ix.idx.emplace(vec[i].id, i); // (first one, as the scan)
            ix.data = vec.data();
            ix.n = vec.size();
        };
        if (ix.data != static_cast<const void*>(vec.data()) || ix.n != vec.size()) rebuild();
        auto it = ix.idx.find(id);
        if (it != ix.idx.end() && (it->second >= vec.size() || vec[it->second].id != id)) {
            rebuild(); // (the vector changed in place)
            it = ix.idx.find(id);
        }
        return it == ix.idx.end() ? -1 : static_cast<std::ptrdiff_t>(it->second);
    };
    auto path_rules_for = [&](bool from_object, std::int32_t owner_id) -> std::vector<map::PathRuleEntry>* {
        if (from_object) {
            const auto i = rail_owner_find(rail_obj_index, edit_objects, owner_id);
            if (i >= 0) return &edit_objects[static_cast<std::size_t>(i)].path_rules;
        } else {
            const auto i = rail_owner_find(rail_spl_index, edit_splines, owner_id);
            if (i >= 0) return &edit_splines[static_cast<std::size_t>(i)].path_rules;
        }
        return nullptr;
    };
    auto pack_path_rule_view = [&](const std::vector<map::PathRuleEntry>& rules, int pi,
                                   bool from_object, std::int32_t owner_id,
                                   const std::string& label) {
        ui::EditorPathRuleProps p{};
        p.valid = true;
        p.owner_id = owner_id;
        p.from_object = from_object;
        p.path_index = pi;
        p.label = label;
        p.density = 1.f;
        for (const auto& r : rules) {
            if (r.path_index != pi) continue;
            if (rule_ieq(r.keyword, "trafficdensity")) {
                if (r.kill) {
                    p.density = 1.f;
                    p.crossed_out = false;
                } else {
                    p.density = r.value;
                    p.density_uvg = r.extra;
                    p.crossed_out = r.value <= 1.e-4f;
                }
            } else if (rule_ieq(r.keyword, "speedlimit")) {
                p.has_speed = !r.kill;
                if (!r.kill) p.speed_kmh = r.value;
            } else if (rule_ieq(r.keyword, "priority")) {
                p.has_priority = !r.kill;
                if (!r.kill) p.priority = r.value;
            } else if (rule_ieq(r.keyword, "no_cars")) {
                p.no_cars = !r.kill;
            } else if (rule_ieq(r.keyword, "buses")) {
                p.buses = !r.kill;
            } else if (rule_ieq(r.keyword, "trucks")) {
                p.trucks = !r.kill;
            } else if (rule_ieq(r.keyword, "overtaking_prohib")) {
                p.no_overtake = !r.kill;
            }
        }
        return p;
    };
    auto apply_path_rule_view = [&](const ui::EditorPathRuleProps& p) {
        auto* rules = path_rules_for(p.from_object, p.owner_id);
        if (!rules) return;
        // Remove conflicting primary rules for this path_index so the newly applied
        // rule's color takes effect (otherwise stale rules keep the old color).
        auto is_primary = [&](const std::string& kw) {
            return rule_ieq(kw, "speedlimit") || rule_ieq(kw, "overtaking_prohib") ||
                   rule_ieq(kw, "priority") || rule_ieq(kw, "no_cars") ||
                   rule_ieq(kw, "buses") || rule_ieq(kw, "trucks") ||
                   rule_ieq(kw, "trafficdensity");
        };
        for (auto it = rules->begin(); it != rules->end();) {
            if (it->path_index == p.path_index && is_primary(it->keyword)) {
                // keep if the brush still sets this keyword
                bool keep = false;
                if (rule_ieq(it->keyword, "speedlimit")) keep = p.has_speed;
                else if (rule_ieq(it->keyword, "overtaking_prohib")) keep = p.no_overtake;
                else if (rule_ieq(it->keyword, "priority")) keep = p.has_priority;
                else if (rule_ieq(it->keyword, "no_cars")) keep = p.no_cars;
                else if (rule_ieq(it->keyword, "buses")) keep = p.buses;
                else if (rule_ieq(it->keyword, "trucks")) keep = p.trucks;
                else if (rule_ieq(it->keyword, "trafficdensity")) keep = true; // density always present unless kill
                if (!keep) { it = rules->erase(it); continue; }
            }
            ++it;
        }
        // TPathRule_Road_SetRule case 6: act + priority byte; OMSI BoundErr if value > 255.
        auto clamp_prio = [](float v) {
            if (!(v >= 0.f)) return 0.f;
            if (v > 255.f) return 255.f;
            return v;
        };
        auto upsert = [&](const char* kw, float v, int extra, bool kill) {
            const bool dens = rule_ieq(kw, "trafficdensity");
            for (auto& r : *rules) {
                if (r.path_index != p.path_index) continue;
                if (!rule_ieq(r.keyword, kw)) continue;
                if (dens && r.extra != extra) continue;
                r.kill = kill;
                r.value = v;
                r.extra = extra;
                return;
            }
            map::PathRuleEntry e{};
            e.path_index = p.path_index; // first [rule] line — not the SetRule cond id
            e.keyword = kw;
            e.value = v;
            e.extra = extra;
            e.kill = kill;
            rules->push_back(e);
        };
        // Brush radios set a single primary flag; dialog may set several.
        if (p.has_speed) upsert("speedlimit", p.speed_kmh, 0, false);
        if (p.no_overtake) upsert("overtaking_prohib", 1.f, 0, false);
        if (p.has_priority) upsert("priority", clamp_prio(p.priority), 0, false);
        if (p.no_cars) upsert("no_cars", 1.f, 0, false);
        if (p.buses) upsert("buses", 1.f, 0, false);
        if (p.trucks) upsert("trucks", 1.f, 0, false);
        if (p.crossed_out) {
            upsert("trafficdensity", 0.f, p.density_uvg, false);
        } else if (!p.has_speed && !p.no_overtake && !p.has_priority && !p.no_cars && !p.buses &&
                   !p.trucks) {
            upsert("trafficdensity", p.density, p.density_uvg, false);
        }
    };
    std::optional<ui::EditorPathRuleProps> pending_path_dialog;

    auto show_path_rail_at = [&](int rail_i) {
        if (rail_i < 0 || static_cast<std::size_t>(rail_i) >= path_rails.size()) return;
        const auto& rail = path_rails[static_cast<std::size_t>(rail_i)];
        const auto* rules = path_rules_for(rail.from_object, rail.owner_id);
        std::string kind = rail.path_typ == 1 ? "peatones" : "autos";
        if (rail.path_dir == 2) kind += " bi";
        const std::string label = (rail.from_object ? "obj #" : "spline #") + std::to_string(rail.owner_id) +
                                  " path " + std::to_string(rail.path_index) + " (" + kind + ")";
        std::vector<map::PathRuleEntry> empty;
        auto view = pack_path_rule_view(rules ? *rules : empty, rail.path_index, rail.from_object, rail.owner_id,
                                        label);
        selected_path_rail = rail_i;
        panel.show_path_rule(view);
        panel.set_selection_text(label);
    };
    auto resolve_path_tex = [&](int typ, int dir) {
        const char* family = "cars";
        switch (typ) {
        case 1: family = "hum"; break;
        case 2: family = "rail"; break;
        case 3: family = "air"; break;
        default: break;
        }
        const bool bi = (dir == 2);
        const std::string name =
            std::string("helper_path_") + family + "_" + (bi ? "2" : "1") + ".bmp";
        for (const char* folder : {"Texture", "texture"}) {
            const auto cand = cfg.omsi_root / folder / name;
            if (std::filesystem::exists(cand)) return cand;
        }
        for (const char* folder : {"Texture", "texture", "helper", "Helper"}) {
            const auto cand = cfg.omsi_root / folder / "helper_path_cars_1.bmp";
            if (std::filesystem::exists(cand)) return cand;
        }
        return std::filesystem::path{};
    };
    // OMSI editor visual filter (Unit84 TPathRule_Road + map editor brush mode):
    // Only the ACTIVE brush category is coloured. Other rule types stay invisible (original tex).
    // Density modes also apply UVG default from unsched_vehgroups.txt (path_mode):
    //   0 = no traffic by default → black until an explicit rule unlocks the path
    //   1 = inherit group 0 medium / first UVG default, etc.
    auto rail_tint_for = [&](bool from_object, std::int32_t owner_id, int pi, int typ) {
        (void)typ;
        const auto none = std::array<float, 4>{1.f, 1.f, 1.f, 0.f};
        const auto* rules = path_rules_for(from_object, owner_id);

        const auto& brush = panel.imgui_path_rule_edit();
        const int mode = brush.traffic_brush; // kTrafficRules index
        // 0 speed | 1 overtaking | 2 no_unscheduled | 3-7 density tiers | 8/9 priority | 10 no_cars | 11 trucks | 12 buses
        const bool mode_speed = (mode == 0) || brush.has_speed;
        const bool mode_overtake = (mode == 1) || brush.no_overtake;
        const bool mode_density = (mode >= 2 && mode <= 7) || brush.crossed_out ||
                                  (!brush.has_speed && !brush.no_overtake && !brush.has_priority &&
                                   !brush.no_cars && !brush.trucks && !brush.buses);
        const bool mode_prio = (mode == 8 || mode == 9) || brush.has_priority;
        const bool mode_nocars = (mode == 10) || brush.no_cars;
        const bool mode_trucks = (mode == 11) || brush.trucks;
        const bool mode_buses = (mode == 12) || brush.buses;

        auto setc = [](float r, float g, float b) { return std::array<float, 4>{r, g, b, 1.f}; };
        // Omsi.exe TFileObject.SetRule: every keyword is its own field of the path's rule (trafficdensity one
        // per vehicle group), a later rule of the same field overwrites it and a [kill_rule] clears it -
        // rules of different kinds never cover each other. The first match (as before) showed the oldest.
        auto last_rule = [&](const char* kw, int group) -> const map::PathRuleEntry* {
            const map::PathRuleEntry* last = nullptr;
            if (!rules) return nullptr;
            for (const auto& e : *rules) {
                if (e.path_index != pi || !rule_ieq(e.keyword, kw)) continue;
                if (group >= 0 && e.extra != group) continue;
                last = &e;
            }
            return (last && !last->kill) ? last : nullptr;
        };
        auto dens_color = [&](float dens_val) {
            if (dens_val <= 1.e-4f) return setc(0.04f, 0.04f, 0.04f); // black — no unscheduled
            if (dens_val < 0.05f) return setc(0.06f, 0.10f, 0.38f);
            if (dens_val < 0.25f) return setc(0.08f, 0.16f, 0.55f);
            if (dens_val < 0.75f) return setc(0.14f, 0.32f, 0.78f);
            if (dens_val < 1.75f) return setc(0.28f, 0.58f, 0.98f);
            return setc(0.15f, 0.88f, 0.95f);
        };

        // --- Speed limit mode: only speedlimit rules (red = match brush, yellow = other speed) ---
        if (mode_speed) {
            const auto* e = last_rule("speedlimit", -1);
            if (!e) return none; // no speed rule → original texture
            const float ref = brush.has_speed ? brush.speed_kmh : e->value;
            if (std::fabs(e->value - ref) < 0.5f) return setc(1.00f, 0.15f, 0.10f); // red — matches selected speed
            return setc(1.00f, 0.90f, 0.12f);                                       // yellow — other speed
        }

        // --- Overtaking prohibited: red only if rule present ---
        if (mode_overtake) {
            return last_rule("overtaking_prohib", -1) ? setc(1.00f, 0.18f, 0.12f) : none;
        }

        // --- Cars prohibited ---
        if (mode_nocars) {
            return last_rule("no_cars", -1) ? setc(1.00f, 0.18f, 0.12f) : none;
        }

        // --- Trucks (allowed flag inverted in OMSI display as prohibition colour when set) ---
        if (mode_trucks) {
            return last_rule("trucks", -1) ? setc(1.00f, 0.18f, 0.12f) : none;
        }

        // --- Buses & cabs allowed ---
        if (mode_buses) {
            return last_rule("buses", -1) ? setc(1.00f, 0.18f, 0.12f) : none;
        }

        // --- Priority (higher/lower): red match / yellow other; no other rule types ---
        if (mode_prio) {
            const auto* e = last_rule("priority", -1);
            if (!e) return none;
            const float ref = brush.has_priority ? brush.priority : e->value;
            if (std::fabs(e->value - ref) < 0.5f) return setc(1.00f, 0.15f, 0.10f);
            return setc(1.00f, 0.90f, 0.12f);
        }

        // --- Density family (no unscheduled + all density tiers): show density for SELECTED UVG ---
        // Resolve density for the active vehicle group (uvg_sel / density_uvg).
        if (mode_density) {
            const int uvg = (brush.density_uvg >= 0) ? brush.density_uvg : panel.imgui_state().uvg_sel;
            float dens_val = 1.f;
            const auto* de = last_rule("trafficdensity", uvg); // per-group density, last one wins
            const bool has_explicit = de != nullptr;
            if (de) dens_val = de->value;
            if (!has_explicit) {
                // UVG default from unsched_vehgroups.txt path_mode (Unit84 GetDensity walk):
                // 0 → no traffic (black); 1 → medium (1.0) for first group / inherit index;
                // N>0 → treat as "unlocked" at medium density class.
                int path_mode = 1;
                if (uvg >= 0 && static_cast<std::size_t>(uvg) < cfg.global.unsched_vehgroups.size())
                    path_mode = cfg.global.unsched_vehgroups[static_cast<std::size_t>(uvg)].path_mode;
                if (path_mode == 0)
                    dens_val = 0.f; // locked / no unscheduled traffic until rule applied
                else
                    dens_val = 1.f; // default medium appearance
            }
            return dens_color(dens_val);
        }

        return none;
    };

        // Preview color for the active traffic brush (hover only — does not write rules).
        auto brush_preview_rgb = [&](float& r, float& g, float& b) -> bool {
            ui::EditorPathRuleProps brush{};
            if (!panel.traffic_paint_brush(brush)) return false;
            if (brush.no_cars || brush.trucks || brush.buses || brush.no_overtake) {
                r = 1.00f; g = 0.18f; b = 0.12f; return true;
            }
            if (brush.has_speed) {
                r = 0.12f; g = 0.92f; b = 0.35f; return true; // green-ish speed
            }
            if (brush.has_priority) {
                r = 1.00f; g = 0.90f; b = 0.12f; return true; // yellow priority
            }
            // density tiers (match rail_tint_for)
            const float dens = brush.crossed_out ? 0.f : brush.density;
            if (dens <= 1.e-4f) { r = 0.04f; g = 0.04f; b = 0.04f; }
            else if (dens < 0.05f) { r = 0.06f; g = 0.10f; b = 0.38f; }
            else if (dens < 0.25f) { r = 0.08f; g = 0.16f; b = 0.55f; }
            else if (dens < 0.75f) { r = 0.14f; g = 0.32f; b = 0.78f; }
            else if (dens < 1.75f) { r = 0.28f; g = 0.58f; b = 0.98f; }
            else { r = 0.15f; g = 0.88f; b = 0.95f; }
            return true;
        };

        // --- Ctrl-chain: last painted rail → hover rail via endpoint links (async BFS) ---
        int traffic_anchor_rail = -1;
        std::vector<int> traffic_chain_rails;
        std::mutex traffic_chain_mtx;
        std::atomic<std::uint64_t> traffic_chain_gen{0};
        std::atomic<bool> traffic_chain_ready{false};
        std::vector<int> traffic_chain_pending; // written by worker, swapped on main
        constexpr float kChainEpsM = 0.02f; // ~2 cm — near snap precision

        auto path_rail_endpoints = [&](int rail_i, float& sx, float& sy, float& sz, float& ex, float& ey,
                                       float& ez) -> bool {
            if (rail_i < 0 || static_cast<std::size_t>(rail_i) >= path_rails.size()) return false;
            const auto& rail = path_rails[static_cast<std::size_t>(rail_i)];
            if (!rail.alive) return false;
            auto eval_end = [&](bool is_start, float& x, float& y, float& z) -> bool {
                if (rail.from_object) {
                    const map::MapObjectInst* ob = nullptr;
                    for (const auto& o : edit_objects)
                        if (o.id == rail.owner_id) { ob = &o; break; }
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
                    map::SplineInst mini = sco::sco_path_to_spline_inst(path);
                    float plen = std::abs(path.length);
                    if (std::abs(path.radius) > 0.001f && plen > 0.001f && plen <= 360.f &&
                        plen / std::abs(path.radius) > 3.2f)
                        plen = std::abs(path.radius) * plen * (3.14159265f / 180.f);
                    mini.length = (std::max)(0.05f, plen);
                    const bool abs_z = sco && sco::skip_terrain_add(*sco);
                    const float th = terrain_add(abs_z, ob->position.x, ob->position.y, ob->position.z,
                                                sco && sco_ground_layer(*ob, *sco));
                    const XMMATRIX world_m = sco_world_matrix(*ob, nullptr, th, 0.f, 0.f);
                    const auto local = spline::evaluate_spline(mini, is_start ? 0.f : mini.length);
                    DirectX::XMFLOAT3 w{};
                    DirectX::XMStoreFloat3(&w, xform_point(world_m, local.x, local.y, local.z));
                    x = w.x; y = w.y; z = w.z;
                    return true;
                }
                for (const auto& s : edit_splines) {
                    if (s.id != rail.owner_id) continue;
                    const auto def = spline::load_sli_shared(s.sli_path);
                    if (!def || rail.path_index < 0 ||
                        static_cast<std::size_t>(rail.path_index) >= def->paths.size())
                        return false;
                    const auto& pp = def->paths[static_cast<std::size_t>(rail.path_index)];
                    const auto samp =
                        spline::evaluate_spline(s, is_start ? 0.f : s.length, pp.lateral, pp.height);
                    x = samp.x; y = samp.y; z = samp.z;
                    return true;
                }
                return false;
            };
            return eval_end(true, sx, sy, sz) && eval_end(false, ex, ey, ez);
        };

        auto kick_traffic_chain_async = [&](int anchor_i, int target_i) {
            if (anchor_i < 0 || target_i < 0 || anchor_i == target_i) {
                std::lock_guard<std::mutex> lk(traffic_chain_mtx);
                traffic_chain_rails.clear();
                traffic_chain_ready.store(true, std::memory_order_release);
                return;
            }
            const std::uint64_t gen = traffic_chain_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
            // Snapshot endpoints on main thread (edit state is main-owned).
            struct Node {
                int rail_i;
                float sx, sy, sz, ex, ey, ez;
            };
            std::vector<Node> nodes;
            nodes.reserve(path_rails.size());
            for (std::size_t i = 0; i < path_rails.size(); ++i) {
                if (!path_rails[i].alive) continue;
                Node n{};
                n.rail_i = static_cast<int>(i);
                if (!path_rail_endpoints(n.rail_i, n.sx, n.sy, n.sz, n.ex, n.ey, n.ez)) continue;
                nodes.push_back(n);
            }
            std::thread([gen, anchor_i, target_i, nodes = std::move(nodes), &traffic_chain_gen,
                         &traffic_chain_mtx, &traffic_chain_pending, &traffic_chain_ready]() {
                if (traffic_chain_gen.load(std::memory_order_acquire) != gen) return;
                const float eps2 = kChainEpsM * kChainEpsM;
                auto near3 = [&](float ax, float ay, float az, float bx, float by, float bz) {
                    const float dx = ax - bx, dy = ay - by, dz = az - bz;
                    return dx * dx + dy * dy + dz * dz <= eps2;
                };
                // Adjacency: rail indices in nodes[] space
                const int n = static_cast<int>(nodes.size());
                std::unordered_map<int, int> rail_to_node;
                for (int i = 0; i < n; ++i) rail_to_node[nodes[static_cast<std::size_t>(i)].rail_i] = i;
                if (!rail_to_node.count(anchor_i) || !rail_to_node.count(target_i)) return;
                std::vector<std::vector<int>> adj(static_cast<std::size_t>(n));
                for (int i = 0; i < n; ++i) {
                    for (int j = i + 1; j < n; ++j) {
                        const auto& A = nodes[static_cast<std::size_t>(i)];
                        const auto& B = nodes[static_cast<std::size_t>(j)];
                        // Any endpoint of A near any endpoint of B → link
                        if (near3(A.sx, A.sy, A.sz, B.sx, B.sy, B.sz) ||
                            near3(A.sx, A.sy, A.sz, B.ex, B.ey, B.ez) ||
                            near3(A.ex, A.ey, A.ez, B.sx, B.sy, B.sz) ||
                            near3(A.ex, A.ey, A.ez, B.ex, B.ey, B.ez)) {
                            adj[static_cast<std::size_t>(i)].push_back(j);
                            adj[static_cast<std::size_t>(j)].push_back(i);
                        }
                    }
                }
                const int src = rail_to_node[anchor_i];
                const int dst = rail_to_node[target_i];
                std::vector<int> parent(static_cast<std::size_t>(n), -1);
                std::vector<char> seen(static_cast<std::size_t>(n), 0);
                std::queue<int> q;
                q.push(src);
                seen[static_cast<std::size_t>(src)] = 1;
                while (!q.empty()) {
                    const int u = q.front();
                    q.pop();
                    if (u == dst) break;
                    for (int v : adj[static_cast<std::size_t>(u)]) {
                        if (seen[static_cast<std::size_t>(v)]) continue;
                        seen[static_cast<std::size_t>(v)] = 1;
                        parent[static_cast<std::size_t>(v)] = u;
                        q.push(v);
                    }
                }
                std::vector<int> path;
                if (seen[static_cast<std::size_t>(dst)]) {
                    for (int cur = dst; cur >= 0; cur = parent[static_cast<std::size_t>(cur)]) {
                        path.push_back(nodes[static_cast<std::size_t>(cur)].rail_i);
                        if (cur == src) break;
                    }
                    std::reverse(path.begin(), path.end());
                }
                if (traffic_chain_gen.load(std::memory_order_acquire) != gen) return;
                {
                    std::lock_guard<std::mutex> lk(traffic_chain_mtx);
                    traffic_chain_pending = std::move(path);
                }
                traffic_chain_ready.store(true, std::memory_order_release);
            }).detach();
        };

        auto poll_traffic_chain = [&]() {
            if (!traffic_chain_ready.exchange(false, std::memory_order_acq_rel)) return;
            std::lock_guard<std::mutex> lk(traffic_chain_mtx);
            traffic_chain_rails.swap(traffic_chain_pending);
            traffic_chain_pending.clear();
        };

        auto rail_in_traffic_chain = [&](int rail_i) -> bool {
            for (int x : traffic_chain_rails)
                if (x == rail_i) return true;
            return false;
        };

        auto tint_path_rail_chunk = [&](const PathRail& rail) {
        const auto tint = rail_tint_for(rail.from_object, rail.owner_id, rail.path_index, rail.path_typ);
        // Soft wash so helper texture stays readable (shader: lerp(tex, tint, amount)).
        if (tint[3] < 0.01f) {
            // Modern: solid cool-magenta base so paths never blend with rule tints (red/yellow/blue/black).
            if (panel.imgui_state().path_style == 1)
                renderer.set_chunk_highlight(rail.chunk, 0.92f, 0.75f, 0.35f, 0.95f);
            else
                renderer.set_chunk_highlight(rail.chunk, 0.f, 1.f, 1.f, 1.f);
            return;
        }
        const float lum = 0.2126f * tint[0] + 0.7152f * tint[1] + 0.0722f * tint[2];
        const float amt = (lum < 0.12f) ? 0.95f : 0.90f;
        renderer.set_chunk_highlight(rail.chunk, amt, tint[0], tint[1], tint[2]);
    };
    // Fast paint: mutate rules + tint only (no remesh / refresh_path_rails).
    auto paint_path_rail_fast = [&](int rail_i) -> bool {
        if (rail_i < 0 || static_cast<std::size_t>(rail_i) >= path_rails.size()) return false;
        const auto& pr = path_rails[static_cast<std::size_t>(rail_i)];
        if (!pr.alive) return false;
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        // Alt = select only (no paint / no erase). Sticky for the rules panel.
        if (alt) {
            selected_path_rail = rail_i;
            traffic_anchor_rail = rail_i;
            show_path_rail_at(rail_i);
            return true;
        }

        auto erase_one = [&](const PathRail& tr) {
            auto* rules = path_rules_for(tr.from_object, tr.owner_id);
            if (!rules) return;
            ui::EditorPathRuleProps brush{};
            if (!panel.traffic_paint_brush(brush)) return;
            brush.density_uvg = panel.imgui_state().uvg_sel;
            if (brush.density_uvg < 0) brush.density_uvg = 0;
            auto kill_kw = [&](const char* kw, float v, int extra) {
                const bool dens = rule_ieq(kw, "trafficdensity");
                for (auto it = rules->begin(); it != rules->end();) {
                    if (it->path_index != tr.path_index) { ++it; continue; }
                    if (!rule_ieq(it->keyword, kw)) { ++it; continue; }
                    if (dens && it->extra != extra) { ++it; continue; }
                    it = rules->erase(it);
                    return;
                }
                (void)v; (void)extra;
            };
            if (brush.has_speed) kill_kw("speedlimit", brush.speed_kmh, 0);
            else if (brush.no_overtake) kill_kw("overtaking_prohib", 1.f, 0);
            else if (brush.no_cars) kill_kw("no_cars", 1.f, 0);
            else if (brush.trucks) kill_kw("trucks", 1.f, 0);
            else if (brush.buses) kill_kw("buses", 1.f, 0);
            else if (brush.has_priority) kill_kw("priority", brush.priority, 0);
            else kill_kw("trafficdensity", brush.crossed_out ? 0.f : brush.density, brush.density_uvg);
            if (tr.from_object) {
                for (const auto& o : edit_objects)
                    if (o.id == tr.owner_id) { persist_editor_xy(o.position.x, o.position.y); break; }
            } else {
                for (const auto& s : edit_splines)
                    if (s.id == tr.owner_id) { persist_editor_xy(s.position.x, s.position.y); break; }
            }
            for (auto& rail : path_rails) {
                if (!rail.alive) continue;
                if (rail.owner_id == tr.owner_id && rail.from_object == tr.from_object &&
                    rail.path_index == tr.path_index)
                    tint_path_rail_chunk(rail);
            }
        };

        auto paint_one = [&](const PathRail& tr) {
            ui::EditorPathRuleProps brush{};
            if (!panel.traffic_paint_brush(brush)) return;
            // Always bind density to the currently selected UVG (NormalCars=0, Trucks=1, …).
            // OMSI writes one [rule] trafficdensity line per group index.
            brush.density_uvg = panel.imgui_state().uvg_sel;
            if (brush.density_uvg < 0) brush.density_uvg = 0;
            brush.owner_id = tr.owner_id;
            brush.from_object = tr.from_object;
            brush.path_index = tr.path_index;
            brush.valid = true;
            apply_path_rule_view(brush);
            if (tr.from_object) {
                for (const auto& o : edit_objects)
                    if (o.id == tr.owner_id) { persist_editor_xy(o.position.x, o.position.y); break; }
            } else {
                for (const auto& s : edit_splines)
                    if (s.id == tr.owner_id) { persist_editor_xy(s.position.x, s.position.y); break; }
            }
            for (auto& rail : path_rails) {
                if (!rail.alive) continue;
                if (rail.owner_id == tr.owner_id && rail.from_object == tr.from_object &&
                    rail.path_index == tr.path_index)
                    tint_path_rail_chunk(rail);
            }
        };

        // Targets: ctrl chain (if hover is in chain) else single. Each keeps own owner.
        std::vector<int> targets;
        if (ctrl && !traffic_chain_rails.empty() && rail_in_traffic_chain(rail_i))
            targets = traffic_chain_rails;
        else
            targets.push_back(rail_i);

        if (shift) {
            for (int ti : targets) {
                if (ti < 0 || static_cast<std::size_t>(ti) >= path_rails.size()) continue;
                const auto& tr = path_rails[static_cast<std::size_t>(ti)];
                if (!tr.alive) continue;
                erase_one(tr);
            }
        } else {
            for (int ti : targets) {
                if (ti < 0 || static_cast<std::size_t>(ti) >= path_rails.size()) continue;
                const auto& tr = path_rails[static_cast<std::size_t>(ti)];
                if (!tr.alive) continue;
                paint_one(tr);
            }
            traffic_anchor_rail = rail_i;
        }
        selected_path_rail = rail_i;
        return true;
    };
    int paint_drag_last_rail = -1;
    struct PathRailOwnerKey {
        std::int32_t id = 0;
        bool from_obj = false;
        bool operator==(const PathRailOwnerKey& o) const { return id == o.id && from_obj == o.from_obj; }
    };
    struct PathRailOwnerKeyHash {
        std::size_t operator()(PathRailOwnerKey k) const {
            return std::hash<std::int32_t>()(k.id) ^ (k.from_obj ? 0x9e3779b9u : 0u);
        }
    };
    std::unordered_set<PathRailOwnerKey, PathRailOwnerKeyHash> path_rails_dirty;
    bool path_rails_rebuild = false;
    // Traffic / Tracks&Trips / Station Links need the rails even when the "Rails path" checkbox is off.
    bool path_rails_forced_by_tool = false;
    int path_rails_phase = 0;
    std::size_t path_rails_spline_i = 0;
    std::size_t path_rails_obj_i = 0;
    std::shared_ptr<const spline::SplineDefinition> path_rails_mesh_def_cache;
    std::vector<std::size_t> path_rails_free_chunks;
    struct PathRailCpuSlot {
        std::vector<o3d::Vertex> verts;
        std::vector<std::uint32_t> inds;
        std::int32_t owner = 0;
        int path_index = 0;
        bool from_object = false;
        std::int32_t tile_gx = 0;
        std::int32_t tile_gy = 0;
        int path_typ = 0;
        int path_dir = 0;
        float ox = 0.f; // owner position, for the O(1) visual-radius test at upload
        float oy = 0.f;
        bool ok = false;
    };
    std::thread path_rails_cpu_thread;
    std::atomic<bool> path_rails_cpu_done{true};
    bool path_rails_cpu_started = false;
    std::vector<PathRailCpuSlot> path_rails_cpu_slots;
    std::size_t path_rails_cpu_upload_i = 0;

    auto mark_path_rails_dirty = [&](std::int32_t owner_id, bool from_obj) {
        if (owner_id == 0) return;
        path_rails_dirty.insert(PathRailOwnerKey{owner_id, from_obj});
    };
    auto mark_path_rails_dirty_spline_id = [&](std::int32_t id) {
        if (id == 0) return;
        mark_path_rails_dirty(id, false);
        for (const auto& s : edit_splines)
            if (s.next_id == id) mark_path_rails_dirty(s.id, false);
    };
    auto retint_path_rails_for_owner = [&](std::int32_t owner_id, bool from_obj, int path_index = -1) {
        for (auto& rail : path_rails) {
            if (!rail.alive || rail.owner_id != owner_id || rail.from_object != from_obj) continue;
            if (path_index >= 0 && rail.path_index != path_index) continue;
            tint_path_rail_chunk(rail);
        }
    };
    auto path_rail_exists = [&](std::int32_t owner, int pi, bool from_obj) -> bool {
        for (const auto& r : path_rails) {
            if (r.alive && r.owner_id == owner && r.path_index == pi && r.from_object == from_obj) return true;
        }
        return false;
    };
    auto path_rail_owner_xy = [&](const std::int32_t owner_id, const bool from_obj, float& ox, float& oy) -> bool {
        if (from_obj) {
            const auto i = rail_owner_find(rail_obj_index, edit_objects, owner_id);
            if (i < 0) return false;
            const auto& o = edit_objects[static_cast<std::size_t>(i)];
            ox = o.position.x;
            oy = o.position.y;
            return true;
        }
        const auto i = rail_owner_find(rail_spl_index, edit_splines, owner_id);
        if (i < 0) return false;
        const auto& s = edit_splines[static_cast<std::size_t>(i)];
        ox = s.position.x;
        oy = s.position.y;
        return true;
    };
    auto path_rail_in_visual_radius = [&](const std::int32_t owner_id, const bool from_obj) -> bool {
        // While editing a .sco, its own rails must never be culled by the visual radius (they
        // disappeared when the owner left the radius / got overlapped by the model).
        if (sco_path_edit.active && from_obj && owner_id == sco_path_edit.object_id) return true;
        if (editor_visual_radius_m <= 0.f) return true;
        float ox = 0.f, oy = 0.f;
        if (!path_rail_owner_xy(owner_id, from_obj, ox, oy)) return false;
        return visual_in_radius(ox, oy);
    };
    // O(1) variant for callers that already hold the owner (the id lookup above scans every spline/object).
    auto path_rail_in_radius_at = [&](const std::int32_t owner_id, const bool from_obj, const float x, const float y) -> bool {
        if (sco_path_edit.active && from_obj && owner_id == sco_path_edit.object_id) return true;
        if (editor_visual_radius_m <= 0.f) return true;
        return visual_in_radius(x, y);
    };
    auto ensure_path_rails_mesh_cache = [&]() {
        if (path_rails_mesh_def_cache) return;
        for (const char* sub : {"helper", "Helper"}) {
            const auto cand = cfg.omsi_root / sub / "path.sli";
            if (std::filesystem::exists(cand)) {
                path_rails_mesh_def_cache = spline::load_sli_shared(cand);
                break;
            }
        }
    };
    // ── Change detection (only owners whose geometry really changed get new rails) ───────────────
    // Every owner that has rails keeps a signature of the data its rails were built from. A cheap pass
    // (every few frames, never while a G/R/height drag is live) compares it with the current spline /
    // object; only a mismatch marks the owner dirty. This is what makes "G" moves, undo and replace_sli
    // refresh the rails of exactly the pieces that changed and leaves the other thousands untouched.
    std::unordered_map<std::uint64_t, std::uint64_t> path_rail_sigs;
    bool path_rails_detect_paused = false; // set by the main loop while an interactive edit is running
    auto prs_mix = [](std::uint64_t h, std::uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    };
    // (quantised to 1 mm / 0.001 deg: the raw bits changed with every float round trip - a parent's
    //  pose re-derived for its attachments, a chain re-measured after streaming - and the same object's
    //  or spline's paths were torn down and rebuilt over and over)
    auto prs_f = [&](std::uint64_t h, float f) {
        if (!std::isfinite(f)) return prs_mix(h, 0x7fc00000u);
        const long long q = std::llround(static_cast<double>(f) * 1000.0);
        return prs_mix(h, static_cast<std::uint64_t>(q));
    };
    auto prs_key = [](std::int32_t id, bool from_obj) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(id)) << 1) | (from_obj ? 1u : 0u);
    };
    auto spline_rail_sig = [&](const map::SplineInst& s) {
        std::uint64_t h = 1469598103934665603ULL;
        h = prs_f(h, s.position.x); h = prs_f(h, s.position.y); h = prs_f(h, s.position.z);
        h = prs_f(h, s.heading_deg); h = prs_f(h, s.length); h = prs_f(h, s.radius);
        h = prs_f(h, s.start_grad); h = prs_f(h, s.end_grad); h = prs_f(h, s.delta_height);
        h = prs_f(h, s.cant_start); h = prs_f(h, s.cant_end); h = prs_f(h, s.skew_start);
        h = prs_f(h, s.skew_end); h = prs_f(h, s.local_length);
        h = prs_mix(h, (s.is_spline_h ? 1u : 0u) | (s.use_delta_height ? 2u : 0u) | (s.mirror ? 4u : 0u) |
                           (static_cast<std::uint32_t>(s.terrain_align) << 8));
        h = prs_mix(h, static_cast<std::uint32_t>(s.next_id));
        h = prs_mix(h, static_cast<std::uint32_t>(s.prev_id));
        h = prs_mix(h, std::filesystem::hash_value(s.sli_path));
        return h;
    };
    auto object_rail_sig = [&](const map::MapObjectInst& o) {
        std::uint64_t h = 1099511628211ULL;
        h = prs_f(h, o.position.x); h = prs_f(h, o.position.y); h = prs_f(h, o.position.z);
        h = prs_f(h, o.rotation_deg.x); h = prs_f(h, o.rotation_deg.y); h = prs_f(h, o.rotation_deg.z);
        h = prs_mix(h, (o.align_terrain ? 1u : 0u));
        h = prs_mix(h, std::filesystem::hash_value(o.sco_path));
        auto mix_paths = [&](const std::vector<sco::ScoPath>& ps) {
            for (const auto& p : ps) {
                h = prs_f(h, p.x); h = prs_f(h, p.y); h = prs_f(h, p.z); h = prs_f(h, p.heading_deg);
                h = prs_f(h, p.radius); h = prs_f(h, p.length);
                h = prs_mix(h, (static_cast<std::uint32_t>(p.type) << 8) ^ static_cast<std::uint32_t>(p.direction));
            }
        };
        if (sco_path_edit.active && sco_path_edit.object_id == o.id) mix_paths(sco_path_edit.paths);
        else if (const auto* sc = sco::peek_sco(o.sco_path)) mix_paths(sc->paths);
        return h;
    };
    bool path_rails_detect_now = false; // run the full signature pass on the next pump (edit just ended / undo)
    std::size_t path_rails_detect_cursor = 0;
    // Signatures of the owners captured when a CPU build was kicked; committed to path_rail_sigs on upload.
    std::unordered_map<std::uint64_t, std::uint64_t> path_rails_cpu_sigs;
    auto path_rails_join_cpu = [&]() {
        if (path_rails_cpu_thread.joinable()) path_rails_cpu_thread.join();
        path_rails_cpu_started = false;
        path_rails_cpu_done.store(true, std::memory_order_release);
    };
    auto path_rails_kick_cpu = [&]() {
        // Already running — never join+restart mid-job (was spamming "CPU kick" every frame).
        if (path_rails_cpu_started && !path_rails_cpu_done.load(std::memory_order_acquire)) return;
        path_rails_join_cpu();
        ensure_path_rails_mesh_cache();
        struct SplineSnap {
            map::SplineInst inst;
            std::int32_t gx = 0;
            std::int32_t gy = 0;
        };
        struct ObjSnap {
            map::MapObjectInst o;
            float th = 0.f;
            std::int32_t gx = 0;
            std::int32_t gy = 0;
            bool attached = false;
        };
        // Owners that already own rails keep them untouched: only owners WITHOUT rails (new, dirty-erased,
        // entering the radius) are snapshotted. Copying every spline/object of the map made each rebuild O(map).
        std::unordered_set<std::uint64_t> owners_with_rails;
        owners_with_rails.reserve(path_rails.size() * 2 + 8);
        for (const auto& r : path_rails)
            if (r.alive) owners_with_rails.insert(prs_key(r.owner_id, r.from_object));
        std::unordered_map<std::int32_t, const map::SplineInst*> spl_by_id;
        spl_by_id.reserve(edit_splines.size() * 2 + 8);
        for (const auto& s : edit_splines) spl_by_id.emplace(s.id, &s);
        path_rails_cpu_sigs.clear();
        std::vector<SplineSnap> splines;
        std::unordered_set<std::int32_t> spline_in;
        auto add_spline = [&](const map::SplineInst& s) {
            if (!spline_in.insert(s.id).second) return;
            SplineSnap sn{};
            sn.inst = s;
            sn.gx = tile_gx;
            sn.gy = tile_gy;
            if (const auto* g = find_spline_gpu(s.id)) {
                sn.gx = g->tile_gx;
                sn.gy = g->tile_gy;
            }
            splines.push_back(std::move(sn));
        };
        for (const auto& s : edit_splines) {
            if (owners_with_rails.count(prs_key(s.id, false))) continue;
            if (!path_rail_in_radius_at(s.id, false, s.position.x, s.position.y)) continue;
            add_spline(s);
            path_rails_cpu_sigs[prs_key(s.id, false)] = spline_rail_sig(s);
            // The mesh end height depends on the chained next spline: ship it too (it is skipped by `have`).
            if (s.next_id != 0)
                if (const auto it = spl_by_id.find(s.next_id); it != spl_by_id.end()) add_spline(*it->second);
        }
        std::unordered_set<std::int32_t> attach_children;
        for (const auto& a : edit_attachments) attach_children.insert(a.child_id);
        std::unordered_map<std::int32_t, std::pair<std::int32_t, std::int32_t>> obj_tile;
        obj_tile.reserve(scene_places.size() * 2 + 8);
        for (const auto& sp : scene_places)
            if (sp.alive) obj_tile[sp.object_id] = {sp.tile_gx, sp.tile_gy};
        std::vector<ObjSnap> objects;
        for (const auto& o : edit_objects) {
            if (owners_with_rails.count(prs_key(o.id, true))) continue;
            if (!path_rail_in_radius_at(o.id, true, o.position.x, o.position.y)) continue;
            // Already-parsed .sco without [path]: never has rails, don't copy/queue it again.
            if (const auto* sc = sco::peek_sco(o.sco_path); sc && sc->paths.empty() &&
                !(sco_path_edit.active && sco_path_edit.object_id == o.id))
                continue;
            ObjSnap sn{};
            sn.o = o;
            sn.attached = attach_children.count(o.id) != 0;
            // Height sample only (no SCO I/O). Worker decides skip_terrain_add after load_sco.
            sn.th = terrain_add(false, o.position.x, o.position.y);
            sn.gx = tile_gx;
            sn.gy = tile_gy;
            if (const auto it = obj_tile.find(o.id); it != obj_tile.end()) {
                sn.gx = it->second.first;
                sn.gy = it->second.second;
            }
            objects.push_back(std::move(sn));
            path_rails_cpu_sigs[prs_key(o.id, true)] = object_rail_sig(o);
        }
        std::unordered_set<std::uint64_t> have;
        auto pack_key = [](std::int32_t id, int pi, bool fo) -> std::uint64_t {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(id)) << 32) |
                   (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pi)) << 1) | (fo ? 1ull : 0ull);
        };
        for (const auto& r : path_rails)
            if (r.alive) have.insert(pack_key(r.owner_id, r.path_index, r.from_object));
        path_rails_cpu_slots.clear();
        path_rails_cpu_upload_i = 0;
        if (splines.empty() && objects.empty()) {
            path_rails_cpu_done.store(true, std::memory_order_release);
            path_rails_rebuild = false;
            return;
        }
        const auto mesh_def = path_rails_mesh_def_cache;
        // path_style only affects tint colours (classic textured helper for both styles).
        // Modern: thin 6 cm strip; classic: wide helper mesh. Phase is always Normal+surface_bias.
        const bool modern_thin = (panel.imgui_state().path_style == 1);
        path_rails_cpu_started = true;
        path_rails_cpu_done.store(false, std::memory_order_release);
        {
            int total_rules = 0;
            for (const auto& o : edit_objects) total_rules += static_cast<int>(o.path_rules.size());
            for (const auto& s : edit_splines) total_rules += static_cast<int>(s.path_rules.size());
            log::info("path rails CPU kick sli=" + std::to_string(splines.size()) +
                      " sco=" + std::to_string(objects.size()) +
                      " rules=" + std::to_string(total_rules) + " cores=" +
                      std::to_string((std::max)(1u, std::thread::hardware_concurrency())) +
                      " style=" + std::to_string(panel.imgui_state().path_style));
        }
        path_rails_cpu_thread = std::thread([splines = std::move(splines), objects = std::move(objects), mesh_def,
                                             modern_thin, have = std::move(have), pack_key,
                                             slots = &path_rails_cpu_slots, &done = path_rails_cpu_done]() {
            auto run = [&]() {
            auto strip = [&](const map::SplineInst& s, float width, float lat0, float h0) {
                std::vector<o3d::Vertex> verts;
                std::vector<std::uint32_t> inds;
                std::optional<float> chain_z;
                if (s.next_id != 0) {
                    for (const auto& n : splines)
                        if (n.inst.id == s.next_id) {
                            chain_z = spline::spline_mesh_chain_end_z(s, n.inst.position.z);
                            break;
                        }
                }
                const int segs = (std::max)(2, spline::omis_teile(s, chain_z));
                const float half = (width < 0.25f) ? (std::max)(0.02f, width * 0.5f) : (std::max)(0.2f, width * 0.5f);
                constexpr float kPathTiling = 0.1f;
                for (int i = 0; i <= segs; ++i) {
                    const float along = s.length * (static_cast<float>(i) / static_cast<float>(segs));
                    const auto L = spline::evaluate_spline(s, along, lat0 - half, h0, chain_z);
                    const auto R = spline::evaluate_spline(s, along, lat0 + half, h0, chain_z);
                    o3d::Vertex a{}, b{};
                    a.position[0] = L.x;
                    a.position[1] = L.y;
                    const float zlift = kPathRailZLift;
                    a.position[2] = L.z + zlift;
                    b.position[0] = R.x;
                    b.position[1] = R.y;
                    b.position[2] = R.z + zlift;
                    a.normal[2] = b.normal[2] = 1.f;
                    a.uv[0] = 0.f;
                    b.uv[0] = 1.f;
                    a.uv[1] = b.uv[1] = along * kPathTiling;
                    verts.push_back(a);
                    verts.push_back(b);
                    if (i > 0) {
                        const std::uint32_t i0 = static_cast<std::uint32_t>((i - 1) * 2);
                        const std::uint32_t i1 = i0 + 1;
                        const std::uint32_t i2 = static_cast<std::uint32_t>(i * 2);
                        const std::uint32_t i3 = i2 + 1;
                        inds.insert(inds.end(), {i0, i2, i1, i1, i2, i3});
                    }
                }
                return std::make_pair(std::move(verts), std::move(inds));
            };
            auto build_geom = [&](const map::SplineInst& parent, float lat, float h) {
                // Modern: thin 0.06 m line. Classic: helper path.sli ribbon (or 1.0 m strip fallback).
                if (modern_thin)
                    return strip(parent, 0.06f, lat, h);
                if (mesh_def && !mesh_def->profiles.empty()) {
                    if (const auto mesh =
                            spline::build_path_rail_mesh(parent, lat, h, *mesh_def, kPathRailZLift, std::nullopt))
                        return std::make_pair(mesh->vertices, mesh->indices);
                }
                return strip(parent, 1.0f, lat, h);
            };
            std::vector<std::filesystem::path> slis;
            std::vector<std::filesystem::path> scos;
            std::unordered_set<std::string> seen_s;
            std::unordered_set<std::string> seen_c;
            for (const auto& s : splines) {
                if (s.inst.sli_path.empty()) continue;
                if (seen_s.insert(s.inst.sli_path.lexically_normal().string()).second)
                    slis.push_back(s.inst.sli_path);
            }
            for (const auto& o : objects) {
                if (o.o.sco_path.empty()) continue;
                if (seen_c.insert(o.o.sco_path.lexically_normal().string()).second) scos.push_back(o.o.sco_path);
            }
            if (slis.size() >= 2) {
                std::for_each(std::execution::par, slis.begin(), slis.end(), [](const std::filesystem::path& p) {
                    try {
                        (void)spline::load_sli_shared(p);
                    } catch (...) {
                    }
                });
            } else if (slis.size() == 1) {
                (void)spline::load_sli_shared(slis.front());
            }
            if (scos.size() >= 2) {
                std::for_each(std::execution::par, scos.begin(), scos.end(), [](const std::filesystem::path& p) {
                    try {
                        (void)sco::load_sco(p);
                    } catch (...) {
                    }
                });
            } else if (scos.size() == 1) {
                (void)sco::load_sco(scos.front());
            }
            struct Job {
                map::SplineInst parent;
                float lat = 0.f;
                float h = 0.f;
                std::int32_t owner = 0;
                int pi = 0;
                bool from_obj = false;
                std::int32_t gx = 0;
                std::int32_t gy = 0;
                float ox = 0.f;
                float oy = 0.f;
                int typ = 0;
                int dir = 0;
                bool xform = false;
                DirectX::XMFLOAT4X4 world{};
            };
            std::vector<Job> jobs;
            for (const auto& s : splines) {
                const auto def = spline::load_sli_shared(s.inst.sli_path);
                if (!def || def->paths.empty()) continue;
                for (int pi = 0; pi < static_cast<int>(def->paths.size()); ++pi) {
                    if (have.count(pack_key(s.inst.id, pi, false))) continue;
                    const auto& pp = def->paths[static_cast<std::size_t>(pi)];
                    Job j{};
                    j.parent = s.inst;
                    j.lat = pp.lateral;
                    j.h = pp.height;
                    j.owner = s.inst.id;
                    j.ox = s.inst.position.x;
                    j.oy = s.inst.position.y;
                    j.pi = pi;
                    j.gx = s.gx;
                    j.gy = s.gy;
                    j.typ = pp.typ;
                    j.dir = pp.direction;
                    jobs.push_back(j);
                }
            }
            for (const auto& o : objects) {
                const auto* sco = sco::load_sco(o.o.sco_path);
                if (!sco || sco->paths.empty()) continue;
                const bool abs_z = sco::skip_terrain_add(*sco) || o.attached;
                const float th = abs_z ? 0.f : o.th;
                DirectX::XMFLOAT4X4 world{};
                DirectX::XMStoreFloat4x4(&world, sco_world_matrix(o.o, nullptr, th, 0.f, 0.f));
                for (int pi = 0; pi < static_cast<int>(sco->paths.size()); ++pi) {
                    if (have.count(pack_key(o.o.id, pi, true))) continue;
                    const auto& path = sco->paths[static_cast<std::size_t>(pi)];
                    map::SplineInst mini = sco::sco_path_to_spline_inst(path);
                    float plen = std::abs(path.length);
                    if (std::abs(path.radius) > 0.001f && plen > 0.001f && plen <= 360.f &&
                        plen / std::abs(path.radius) > 3.2f)
                        plen = std::abs(path.radius) * plen * (3.14159265f / 180.f);
                    mini.length = (std::max)(0.05f, plen);
                    Job j{};
                    j.parent = mini;
                    j.owner = o.o.id;
                    j.ox = o.o.position.x;
                    j.oy = o.o.position.y;
                    j.pi = pi;
                    j.from_obj = true;
                    j.gx = o.gx;
                    j.gy = o.gy;
                    j.typ = path.type;
                    j.dir = path.direction;
                    j.xform = true;
                    j.world = world;
                    jobs.push_back(j);
                }
            }
            std::vector<PathRailCpuSlot> built(jobs.size());
            std::vector<std::size_t> idx(jobs.size());
            for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            if (idx.size() >= 2) {
                std::for_each(std::execution::par, idx.begin(), idx.end(), [&](std::size_t i) {
                    try {
                    const auto& job = jobs[i];
                    auto [verts, inds] = build_geom(job.parent, job.lat, job.h);
                    if (job.xform) {
                        const XMMATRIX world_m = DirectX::XMLoadFloat4x4(&job.world);
                        const float modern_extra = 0.f;
                        for (auto& v : verts) {
                            DirectX::XMFLOAT3 w{};
                            DirectX::XMStoreFloat3(
                                &w, xform_point(world_m, v.position[0], v.position[1], v.position[2]));
                            v.position[0] = w.x;
                            v.position[1] = w.y;
                            v.position[2] = w.z + kPathRailZLift + modern_extra;
                        }
                    }
                    auto& slot = built[i];
                    slot.verts = std::move(verts);
                    slot.inds = std::move(inds);
                    slot.owner = job.owner;
                    slot.path_index = job.pi;
                    slot.from_object = job.from_obj;
                    slot.tile_gx = job.gx;
                    slot.tile_gy = job.gy;
                    slot.path_typ = job.typ;
                    slot.path_dir = job.dir;
                    slot.ox = job.ox;
                    slot.oy = job.oy;
                    slot.ok = !slot.verts.empty() && !slot.inds.empty();
                    } catch (...) {
                        built[i].ok = false;
                    }
                });
            } else if (idx.size() == 1) {
                const auto& job = jobs[0];
                auto [verts, inds] = build_geom(job.parent, job.lat, job.h);
                if (job.xform) {
                    const XMMATRIX world_m = DirectX::XMLoadFloat4x4(&job.world);
                    const float modern_extra = 0.f;
                    for (auto& v : verts) {
                        DirectX::XMFLOAT3 w{};
                        DirectX::XMStoreFloat3(&w, xform_point(world_m, v.position[0], v.position[1], v.position[2]));
                        v.position[0] = w.x;
                        v.position[1] = w.y;
                        v.position[2] = w.z + kPathRailZLift + modern_extra;
                    }
                }
                built[0].verts = std::move(verts);
                built[0].inds = std::move(inds);
                built[0].owner = job.owner;
                built[0].path_index = job.pi;
                built[0].from_object = job.from_obj;
                built[0].tile_gx = job.gx;
                built[0].tile_gy = job.gy;
                built[0].path_typ = job.typ;
                built[0].path_dir = job.dir;
                built[0].ox = job.ox;
                built[0].oy = job.oy;
                built[0].ok = !built[0].verts.empty() && !built[0].inds.empty();
            }
            *slots = std::move(built);
            };
            std::function<void()> rails_fn = run;
            const int seh =
                seh_invoke(+[](void* p) { (*static_cast<std::function<void()>*>(p))(); }, &rails_fn);
            if (seh != 0)
                std::fprintf(stderr, "[rails] SEH 0x%08X — worker abort, editor sigue\n",
                             static_cast<unsigned>(seh));
            done.store(true, std::memory_order_release);
        });
    };
    auto sync_path_rails_after_stream = [&]() {
        // Create-once / dirty-only: drop rails whose owners left the stream set; queue create for
        // new in-radius owners. Camera/hour never remesh — only show/hide + this dirty set.
        std::unordered_set<std::int32_t> sli_ids;
        std::unordered_set<std::int32_t> obj_ids;
        for (const auto& s : edit_splines) sli_ids.insert(s.id);
        for (const auto& o : edit_objects) obj_ids.insert(o.id);
        for (auto it = path_rails.begin(); it != path_rails.end();) {
            const bool owner_ok =
                it->from_object ? obj_ids.count(it->owner_id) != 0 : sli_ids.count(it->owner_id) != 0;
            if (owner_ok) {
                ++it;
                continue;
            }
            renderer.set_chunk_visible(it->chunk, false);
            path_rails_free_chunks.push_back(it->chunk);
            path_rails_dirty.erase(PathRailOwnerKey{it->owner_id, it->from_object});
            it = path_rails.erase(it);
        }
        if (!editor_ui_settings.show_path_rails && !path_rails_forced_by_tool) return;
        std::unordered_set<std::uint64_t> have_rail;
        have_rail.reserve(path_rails.size() * 2 + 8);
        for (const auto& r : path_rails)
            if (r.alive) have_rail.insert(prs_key(r.owner_id, r.from_object));
        for (const auto& s : edit_splines) {
            if (have_rail.count(prs_key(s.id, false))) continue;
            if (!path_rail_in_radius_at(s.id, false, s.position.x, s.position.y)) continue;
            path_rails_dirty.insert(PathRailOwnerKey{s.id, false});
        }
        for (const auto& o : edit_objects) {
            if (have_rail.count(prs_key(o.id, true))) continue;
            if (!path_rail_in_radius_at(o.id, true, o.position.x, o.position.y)) continue;
            if (const auto* sco = sco::peek_sco(o.sco_path); sco && sco->paths.empty()) continue;
            path_rails_dirty.insert(PathRailOwnerKey{o.id, true});
        }
    };

    auto refresh_path_rails = [&]() {
        // Don't abort an in-flight CPU build — pump will finish / next toggle can refresh.
        if (path_rails_cpu_started && !path_rails_cpu_done.load(std::memory_order_acquire)) {
            path_rails_rebuild = true;
            return;
        }
        path_rails_join_cpu();
        path_rails_cpu_slots.clear();
        path_rails_cpu_upload_i = 0;
        std::vector<std::size_t> free_chunks;
        free_chunks.reserve(path_rails.size());
        for (auto& r : path_rails) {
            r.alive = false;
            renderer.set_chunk_visible(r.chunk, false);
            free_chunks.push_back(r.chunk);
        }
        path_rails.clear();
        selected_path_rail = -1;
        path_rails_mesh_def_cache.reset();
        ensure_path_rails_mesh_cache();
        path_rails_free_chunks = std::move(free_chunks);
        path_rails_spline_i = 0;
        path_rails_obj_i = 0;
        path_rails_phase = 3;
        path_rails_rebuild = true;
        path_rails_kick_cpu();
    };

    auto upload_path_rail_mesh = [&](const std::vector<o3d::Vertex>& verts_in, const std::vector<std::uint32_t>& inds,
                                     std::int32_t owner, int pi, bool from_obj, std::int32_t gx, std::int32_t gy,
                                     int typ, int dir) {
        if (verts_in.empty() || inds.empty()) return;
        // Direction 1 (reverse): the helper texture must point backwards along the path. Rotating the
        // texture 180 degrees (u -> 1-u, v -> -v) gives that without mirroring the arrow shape; direction 0
        // keeps the authored UVs and direction 2 (both) uses its own two-way texture.
        std::vector<o3d::Vertex> verts_flipped;
        if (dir == 1) {
            verts_flipped = verts_in;
            // Keep V positive (vmax - v): a plain -v relies on the sampler wrapping negative coordinates and
            // left the arrow unchanged wherever the helper texture is clamped / mirrored.
            float vmin = verts_flipped.front().uv[1], vmax = vmin;
            for (const auto& v : verts_flipped) {
                vmin = (std::min)(vmin, v.uv[1]);
                vmax = (std::max)(vmax, v.uv[1]);
            }
            for (auto& v : verts_flipped) {
                v.uv[0] = 1.f - v.uv[0];
                v.uv[1] = vmax - (v.uv[1] - vmin);
            }
        }
        const std::vector<o3d::Vertex>& verts = (dir == 1) ? verts_flipped : verts_in;
        const auto path_tex = resolve_path_tex(typ, dir);
        std::size_t chunk = 0;
        bool ok = false;
        if (!path_rails_free_chunks.empty()) {
            chunk = path_rails_free_chunks.back();
            path_rails_free_chunks.pop_back();
            ok = renderer.replace_textured_mesh(chunk, verts.data(), static_cast<std::uint32_t>(verts.size()),
                                                inds.data(), static_cast<std::uint32_t>(inds.size()), path_tex, 0u);
        } else {
            ok = renderer.append_textured_mesh(verts.data(), static_cast<std::uint32_t>(verts.size()), inds.data(),
                                               static_cast<std::uint32_t>(inds.size()), path_tex, 0u);
            if (ok) chunk = renderer.chunk_count() - 1;
        }
        if (!ok) return;
        PathRail rail{};
        rail.chunk = chunk;
        rail.owner_id = owner;
        rail.path_index = pi;
        rail.path_typ = typ;
        rail.path_dir = dir;
        rail.from_object = from_obj;
        rail.tile_gx = gx;
        rail.tile_gy = gy;
        rail.alive = true;
        path_rails.push_back(rail);
        const auto wt = store_world_transposed(XMMatrixIdentity());
        renderer.set_chunk_world_matrix(rail.chunk, wt.data());
        renderer.set_chunk_visible(rail.chunk, false);
        renderer.set_chunk_fixed_lit(rail.chunk, true);
        renderer.set_chunk_path_rail(rail.chunk, true);
        // Classic: Normal + surface_bias. Modern: no surface bias, so the path-rail pipeline draws it (vs_rail lifts
        // it 6 cm over the ground; with the bias the thin strip sat in the road's plane and sank). OnSurface/decal
        // hid modern under .sli/.sco.
        renderer.set_chunk_render_phase(
            rail.chunk, omsi::gfx::dx12::WorldRenderPhase::Normal,
            /*surface_bias=*/panel.imgui_state().path_style != 1, /*decal=*/false);
        const auto tint = rail_tint_for(from_obj, owner, pi, typ);
        if (false && tint[3] < 0.01f && panel.imgui_state().path_style == 1)
            renderer.set_chunk_highlight(rail.chunk, 0.92f, 0.75f, 0.35f, 0.95f);
        else
            renderer.set_chunk_highlight(rail.chunk, tint[3], tint[0], tint[1], tint[2]);
    };

    auto build_owner_path_geometry = [&](const map::SplineInst& parent, float path_lat, float path_h) {
        // Modern: 0.06 m thin strip. Classic: helper path.sli ribbon.
        if (panel.imgui_state().path_style == 1)
            return make_rail_strip(parent, 0.06f, path_lat, path_h);
        if (path_rails_mesh_def_cache && !path_rails_mesh_def_cache->profiles.empty()) {
            if (const auto mesh = spline::build_path_rail_mesh(parent, path_lat, path_h, *path_rails_mesh_def_cache,
                                                               kPathRailZLift, std::nullopt))
                return std::make_pair(mesh->vertices, mesh->indices);
        }
        return make_rail_strip(parent, 1.0f, path_lat, path_h);
    };

    auto remove_path_rails_for_owner = [&](std::int32_t owner_id, bool from_obj) {
        for (auto it = path_rails.begin(); it != path_rails.end();) {
            if (it->owner_id == owner_id && it->from_object == from_obj) {
                renderer.set_chunk_visible(it->chunk, false);
                path_rails_free_chunks.push_back(it->chunk);
                it = path_rails.erase(it);
            } else {
                ++it;
            }
        }
    };
    auto purge_path_rails_for_owner = [&](std::int32_t owner_id, bool from_obj) {
        remove_path_rails_for_owner(owner_id, from_obj);
        path_rails_dirty.erase(PathRailOwnerKey{owner_id, from_obj});
        selected_path_rail = -1;
    };

    auto set_path_rails_offset = [&](std::int32_t owner_id, bool from_obj, float dx, float dy, float dz = 0.f) {
        for (const auto& rail : path_rails) {
            if (!rail.alive || rail.owner_id != owner_id || rail.from_object != from_obj) continue;
            const auto wt = store_world_transposed(XMMatrixTranslation(dx, dy, dz));
            renderer.set_chunk_world_matrix(rail.chunk, wt.data());
        }
    };
    auto set_one_path_rail_offset = [&](std::int32_t owner_id, bool from_obj, int pi, float dx, float dy,
                                        float dz, bool wire) {
        for (const auto& rail : path_rails) {
            if (!rail.alive || rail.owner_id != owner_id || rail.from_object != from_obj) continue;
            if (pi >= 0 && rail.path_index != pi) continue;
            const auto wt = store_world_transposed(XMMatrixTranslation(dx, dy, dz));
            renderer.set_chunk_world_matrix(rail.chunk, wt.data());
            renderer.set_chunk_wireframe(rail.chunk, wire);
        }
    };
    auto set_owner_path_rails_wireframe = [&](std::int32_t owner_id, bool from_obj, bool wire) {
        const auto ident = store_world_transposed(XMMatrixIdentity());
        for (const auto& rail : path_rails) {
            if (!rail.alive || rail.owner_id != owner_id || rail.from_object != from_obj) continue;
            renderer.set_chunk_wireframe(rail.chunk, wire);
            if (!wire) renderer.set_chunk_world_matrix(rail.chunk, ident.data());
        }
    };

    auto rebuild_path_rails_for_owner = [&](std::int32_t owner_id, bool from_obj) {
        if (!path_rail_in_visual_radius(owner_id, from_obj)) return;
        remove_path_rails_for_owner(owner_id, from_obj);
        ensure_path_rails_mesh_cache();
        if (!from_obj) {
            for (const auto& s : edit_splines) {
                if (s.id != owner_id) continue;
                const auto def = spline::load_sli_shared(s.sli_path);
                if (!def || def->paths.empty()) return;
                std::int32_t sgx = tile_gx, sgy = tile_gy;
                if (const auto* g = find_spline_gpu(s.id)) {
                    sgx = g->tile_gx;
                    sgy = g->tile_gy;
                }
                for (int pi = 0; pi < static_cast<int>(def->paths.size()); ++pi) {
                    const auto& pp = def->paths[static_cast<std::size_t>(pi)];
                    auto [verts, inds] = build_owner_path_geometry(s, pp.lateral, pp.height);
                    upload_path_rail_mesh(verts, inds, s.id, pi, false, sgx, sgy, pp.typ, pp.direction);
                }
                path_rail_sigs[prs_key(s.id, false)] = spline_rail_sig(s);
                return;
            }
        } else {
            for (const auto& o : edit_objects) {
                if (o.id != owner_id) continue;
                const auto sco = sco::load_sco(o.sco_path);
                if (!sco || (sco->paths.empty() &&
                             !(sco_path_edit.active && sco_path_edit.object_id == o.id)))
                    return;
                const std::vector<sco::ScoPath>& live_paths =
                    (sco_path_edit.active && sco_path_edit.object_id == o.id) ? sco_path_edit.paths : sco->paths;
                if (live_paths.empty()) return;
                bool abs_z = sco::skip_terrain_add(*sco);
                const bool ground_layer = sco_ground_layer(o, *sco);
                for (const auto& a : edit_attachments)
                    if (a.child_id == o.id) {
                        abs_z = true;
                        break;
                    }
                const float th = terrain_add(abs_z, o.position.x, o.position.y, o.position.z, ground_layer);
                std::int32_t ogx = tile_gx, ogy = tile_gy;
                for (const auto& sp : scene_places) {
                    if (sp.alive && sp.object_id == o.id) {
                        ogx = sp.tile_gx;
                        ogy = sp.tile_gy;
                        break;
                    }
                }
                const XMMATRIX world_m = sco_world_matrix(o, nullptr, th, 0.f, 0.f);
                for (int pi = 0; pi < static_cast<int>(live_paths.size()); ++pi) {
                    const auto& path = live_paths[static_cast<std::size_t>(pi)];
                    map::SplineInst mini = sco::sco_path_to_spline_inst(path);
                    float plen = std::abs(path.length);
                    if (std::abs(path.radius) > 0.001f && plen > 0.001f && plen <= 360.f &&
                        plen / std::abs(path.radius) > 3.2f)
                        plen = std::abs(path.radius) * plen * (3.14159265f / 180.f);
                    mini.length = (std::max)(0.05f, plen);
                    {
                        const auto p0 = spline::evaluate_spline(mini, 0.f);
                        DirectX::XMFLOAT3 w0{};
                        DirectX::XMStoreFloat3(&w0, xform_point(world_m, p0.x, p0.y, p0.z));
                        log_sco_path_placement(o, *sco, path, pi, true, th, w0.x, w0.y, w0.z, "rails_rebuild",
                                                 kPathRailZLift);
                    }
                    auto [verts, inds] = build_owner_path_geometry(mini, 0.f, 0.f);
                    for (auto& v : verts) {
                        DirectX::XMFLOAT3 w{};
                        DirectX::XMStoreFloat3(&w, xform_point(world_m, v.position[0], v.position[1], v.position[2]));
                        v.position[0] = w.x;
                        v.position[1] = w.y;
                        v.position[2] = w.z + kPathRailZLift;
                    }
                    upload_path_rail_mesh(verts, inds, o.id, pi, true, ogx, ogy, path.type, path.direction);
                }
                path_rail_sigs[prs_key(o.id, true)] = object_rail_sig(o);
                return;
            }
        }
    };

    auto rebuild_one_object_path_rail = [&](std::int32_t owner_id, int pi, bool wire) {
        if (pi < 0) return;
        for (auto it = path_rails.begin(); it != path_rails.end();) {
            if (it->alive && it->from_object && it->owner_id == owner_id && it->path_index == pi) {
                renderer.set_chunk_wireframe(it->chunk, false);
                renderer.set_chunk_visible(it->chunk, false);
                path_rails_free_chunks.push_back(it->chunk);
                it = path_rails.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& o : edit_objects) {
            if (o.id != owner_id) continue;
            const auto sco = sco::load_sco(o.sco_path);
            const std::vector<sco::ScoPath>* live =
                (sco_path_edit.active && sco_path_edit.object_id == o.id) ? &sco_path_edit.paths
                                                                         : (sco ? &sco->paths : nullptr);
            if (!live || static_cast<std::size_t>(pi) >= live->size()) return;
            const auto& path = (*live)[static_cast<std::size_t>(pi)];
            bool abs_z = sco && sco::skip_terrain_add(*sco);
            const bool ground_layer =
                sco && sco_ground_layer(o, *sco);
            for (const auto& a : edit_attachments)
                if (a.child_id == o.id) abs_z = true;
            const float th = terrain_add(abs_z, o.position.x, o.position.y, o.position.z, ground_layer);
            std::int32_t ogx = tile_gx, ogy = tile_gy;
            for (const auto& sp : scene_places) {
                if (sp.alive && sp.object_id == o.id) {
                    ogx = sp.tile_gx;
                    ogy = sp.tile_gy;
                    break;
                }
            }
            const XMMATRIX world_m = sco_world_matrix(o, nullptr, th, 0.f, 0.f);
            map::SplineInst mini = sco::sco_path_to_spline_inst(path);
            float plen = std::abs(path.length);
            if (std::abs(path.radius) > 0.001f && plen > 0.001f && plen <= 360.f &&
                plen / std::abs(path.radius) > 3.2f)
                plen = std::abs(path.radius) * plen * (3.14159265f / 180.f);
            mini.length = (std::max)(0.05f, plen);
            auto [verts, inds] = build_owner_path_geometry(mini, 0.f, 0.f);
            for (auto& v : verts) {
                DirectX::XMFLOAT3 w{};
                DirectX::XMStoreFloat3(&w, xform_point(world_m, v.position[0], v.position[1], v.position[2]));
                v.position[0] = w.x;
                v.position[1] = w.y;
                v.position[2] = w.z + kPathRailZLift;
            }
            upload_path_rail_mesh(verts, inds, o.id, pi, true, ogx, ogy, path.type, path.direction);
            path_rail_sigs[prs_key(o.id, true)] = object_rail_sig(o);
            for (auto& rail : path_rails) {
                if (!rail.alive || !rail.from_object || rail.owner_id != o.id || rail.path_index != pi)
                    continue;
                renderer.set_chunk_visible(rail.chunk, true);
                renderer.set_chunk_wireframe(rail.chunk, wire);
                break;
            }
            return;
        }
    };

    auto owner_path_rails_complete = [&](std::int32_t owner_id, bool from_obj) -> bool {
        int need = -1, have = 0;
        if (from_obj) {
            for (const auto& o : edit_objects) {
                if (o.id != owner_id) continue;
                if (sco_path_edit.active && sco_path_edit.object_id == o.id) {
                    need = static_cast<int>(sco_path_edit.paths.size());
                } else if (const auto* sco = sco::peek_sco(o.sco_path)) {
                    need = static_cast<int>(sco->paths.size());
                }
                break;
            }
        } else {
            for (const auto& s : edit_splines) {
                if (s.id != owner_id) continue;
                if (const auto n = spline::peek_sli_path_count(s.sli_path))
                    need = static_cast<int>(*n);
                break;
            }
        }
        // [PERF] Count alive rails for this owner without scanning all path_rails linearly.
        // Build a count cache keyed by (owner_id, from_obj) that is valid for this call site.
        static std::unordered_map<std::int64_t, int> s_rail_count_cache;
        static std::size_t s_rail_count_gen = 0;
        if (s_rail_count_gen != path_rails.size()) {
            s_rail_count_cache.clear();
            for (const auto& r : path_rails) {
                if (!r.alive) continue;
                const std::int64_t k = (static_cast<std::int64_t>(r.owner_id) << 1) | (r.from_object ? 1 : 0);
                s_rail_count_cache[k]++;
            }
            s_rail_count_gen = path_rails.size();
        }
        const std::int64_t key = (static_cast<std::int64_t>(owner_id) << 1) | (from_obj ? 1 : 0);
        const auto cit = s_rail_count_cache.find(key);
        have = (cit != s_rail_count_cache.end()) ? cit->second : 0;
        if (need == 0) return true;
        if (need < 0) return have > 0;
        return have >= need;
    };

    auto ensure_path_rails_for_owner = [&](std::int32_t owner_id, bool from_obj) {
        if (owner_id == 0) return;
        if (owner_path_rails_complete(owner_id, from_obj)) return;
        mark_path_rails_dirty(owner_id, from_obj);
        path_rails_rebuild = true;
    };
    auto ensure_path_rails_build_started = [&]() {
        if (!path_rails.empty()) return;
        ensure_path_rails_mesh_cache();
        if (path_rails_rebuild) return;
        path_rails_spline_i = 0;
        path_rails_obj_i = 0;
        path_rails_phase = 3;
        path_rails_rebuild = true;
        path_rails_kick_cpu();
    };
    auto detect_path_rail_changes = [&]() {
        static unsigned frame = 0;
        if (path_rails_detect_paused || path_rails.empty()) return;
        // Forced pass right after a G/R/height edit or an undo; otherwise a cheap slice every few frames.
        const bool full = path_rails_detect_now;
        if (!full && (++frame % 8u) != 0u) return;
        path_rails_detect_now = false;
        std::unordered_map<std::int32_t, const map::SplineInst*> spl;
        std::unordered_map<std::int32_t, const map::MapObjectInst*> obj;
        std::unordered_set<std::uint64_t> seen;
        constexpr std::size_t kSlice = 2500; // rails inspected per non-forced pass
        const std::size_t n = path_rails.size();
        if (path_rails_detect_cursor >= n) path_rails_detect_cursor = 0;
        const std::size_t first = full ? 0 : path_rails_detect_cursor;
        const std::size_t count = full ? n : (std::min)(kSlice, n - first);
        if (!full) path_rails_detect_cursor = first + count;
        for (std::size_t ri = first; ri < first + count; ++ri) {
            const auto& r = path_rails[ri];
            if (!r.alive) continue;
            const std::uint64_t k = prs_key(r.owner_id, r.from_object);
            if (!seen.insert(k).second) continue;
            std::uint64_t sg = 0;
            bool have = false;
            if (!r.from_object) {
                if (spl.empty()) { spl.reserve(edit_splines.size() * 2); for (const auto& s : edit_splines) spl.emplace(s.id, &s); }
                const auto it = spl.find(r.owner_id);
                if (it != spl.end()) { sg = spline_rail_sig(*it->second); have = true; }
            } else {
                if (obj.empty()) { obj.reserve(edit_objects.size() * 2); for (const auto& o : edit_objects) obj.emplace(o.id, &o); }
                const auto it = obj.find(r.owner_id);
                // (a .sco not in the cache this moment says nothing: its paths would drop out of the signature)
                if (it != obj.end() && ((sco_path_edit.active && sco_path_edit.object_id == it->second->id) ||
                                        sco::peek_sco(it->second->sco_path))) {
                    sg = object_rail_sig(*it->second);
                    have = true;
                }
            }
            if (!have) continue;
            const auto st = path_rail_sigs.find(k);
            // No recorded signature (rails from before this tracking): assume they match the current data.
            if (st == path_rail_sigs.end()) { path_rail_sigs.emplace(k, sg); continue; }
            if (st->second != sg) {
                st->second = sg;
                if (r.from_object) mark_path_rails_dirty(r.owner_id, true);
                else mark_path_rails_dirty_spline_id(r.owner_id); // also the predecessor that chains into it
            }
        }
        if (full && path_rail_sigs.size() > seen.size() * 2 + 64) { // forget owners that no longer have rails
            for (auto it = path_rail_sigs.begin(); it != path_rail_sigs.end();)
                it = seen.count(it->first) ? std::next(it) : path_rail_sigs.erase(it);
        }
    };
    auto pump_path_rails_dirty = [&]() {
        detect_path_rail_changes();
        if (path_rails_dirty.empty()) return;
        if (path_rails_cpu_started && !path_rails_cpu_done.load(std::memory_order_acquire)) return;
        if (path_rails_cpu_upload_i < path_rails_cpu_slots.size()) return;
        auto keys = std::move(path_rails_dirty);
        path_rails_dirty.clear();
        for (auto it = path_rails.begin(); it != path_rails.end();) {
            const PathRailOwnerKey k{it->owner_id, it->from_object};
            if (keys.count(k)) {
                renderer.set_chunk_visible(it->chunk, false);
                path_rails_free_chunks.push_back(it->chunk);
                it = path_rails.erase(it);
            } else {
                ++it;
            }
        }
        path_rails_rebuild = true;
        path_rails_kick_cpu();
    };
    auto ensure_path_rails_visual_radius = [&]() {
        if (editor_visual_radius_m <= 0.f || (!editor_ui_settings.show_path_rails && !path_rails_forced_by_tool)) return;
        if (panel.current_tool() == ui::EditorTool::Tile) return;
        if (path_rails_rebuild) return;
        if (path_rails_cpu_started && !path_rails_cpu_done.load(std::memory_order_acquire)) return;
        static float last_cx = 1.e30f, last_cy = 1.e30f;
        const float ddx = camera.target_x - last_cx;
        const float ddy = camera.target_y - last_cy;
        if (ddx * ddx + ddy * ddy < 64.f) return;
        last_cx = camera.target_x;
        last_cy = camera.target_y;
        // [PERF] Previously iterated ALL edit_splines + ALL edit_objects here (~20ms/frame on
        // large maps), calling path_rail_in_visual_radius (O(N) lookup) and
        // owner_path_rails_complete (O(N) scan) for each — O(N²) total per camera move.
        // Now we do a single O(N) pass building a budget-aware dirty set, then stop early.
        // The count cache inside owner_path_rails_complete makes repeated calls O(1).
        constexpr std::size_t kMaxDirtyPerMove = 64; // limit dirty additions per checkpoint
        std::size_t added = 0;
        for (const auto& s : edit_splines) {
            if (added >= kMaxDirtyPerMove) break;
            if (!path_rail_in_radius_at(s.id, false, s.position.x, s.position.y)) continue;
            if (owner_path_rails_complete(s.id, false)) continue;
            path_rails_dirty.insert(PathRailOwnerKey{s.id, false});
            ++added;
        }
        for (const auto& o : edit_objects) {
            if (added >= kMaxDirtyPerMove) break;
            if (!path_rail_in_radius_at(o.id, true, o.position.x, o.position.y)) continue;
            if (const auto* sco = sco::peek_sco(o.sco_path); sco && sco->paths.empty()) continue;
            if (owner_path_rails_complete(o.id, true)) continue;
            path_rails_dirty.insert(PathRailOwnerKey{o.id, true});
            ++added;
        }
    };
