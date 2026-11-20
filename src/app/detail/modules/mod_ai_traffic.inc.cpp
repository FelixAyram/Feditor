    // ===== AI road traffic (openOMSI TrafficSim through omsi_vehicle_ffi, AGPL-3.0) =========================
    // OMSI_AI_TRAFFIC=<cars> or Vista > Trafico IA: the map's random traffic (ailists.cfg /
    // unsched_vehgroups.txt types, every car a full openOMSI vehicle: scripts, animations, paint schemes,
    // trailers) on the lanes of the editor's own paths (the same network the bus autopilot drives), giving way
    // at junctions and stopping at the crossings' traffic lights. Each car type's model is put on the GPU once;
    // every car is clones of it (shared vertex buffers: drawn instanced).
    int ai_traffic_target = 0;
    float ai_traffic_radius = 400.f;
    struct AiTrafficProto {
        // 0 new, 1 read on a worker (model.cfg, .o3d, textures decoded), 2 going up to the GPU a few meshes
        // a frame, 3 ready, -1 failed. A car shows once all its parts are ready (loading a model at once on
        // the main thread froze the editor every time a new type came on the road).
        int state = 0;
        std::shared_ptr<std::atomic<bool>> read_done = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::optional<omsi::model_cfg::ModelCfgDocument>> model =
            std::make_shared<std::optional<omsi::model_cfg::ModelCfgDocument>>();
        std::size_t next_mesh = 0;
        std::string name;
        bool ok = false;
        std::unordered_map<std::string, std::vector<std::size_t>> by_file; // lower .o3d path -> prototype chunks
        std::vector<std::size_t> all;
        std::unordered_map<std::size_t, std::vector<VehMatlChange>> changes; // prototype chunk -> its lamp slots
        std::unordered_map<std::size_t, int> viewpoint; // prototype chunk -> its mesh's [viewpoint]
    };
    std::unordered_map<std::string, AiTrafficProto> ai_traffic_protos; // lower part file (.bus / .ovh)
    struct AiTrafficCar {
        std::vector<std::size_t> chunks; // per mesh of the car's train (kNoChunk: none)
        std::vector<const std::vector<VehMatlChange>*> changes; // per mesh: its lamp slots (null: none)
        std::vector<std::vector<signed char>> mc_state;
        std::vector<char> hide; // per mesh: [viewpoint] says not for an AI-driven vehicle (bit 4 unset)
        double lamps_t = -1.0;
        bool seen = false;
        // [CTC] paint still to show: (mesh index, "ctc:<default>" key, scheme file) until its texture is up
        std::vector<std::tuple<std::size_t, std::string, std::filesystem::path>> paint;
    };
    std::unordered_map<std::uint64_t, AiTrafficCar> ai_traffic_cars;
    std::unordered_set<std::int64_t> ai_traffic_owners_sent; // (owner << 1 | object) whose lanes went over
    std::unordered_set<std::int32_t> ai_traffic_light_objs;   // crossings whose light program the traffic has
    // lanes go over around a ring of points (the view, the bus and 8 points 380 m out) one a quarter second,
    // each 250 m cell again whenever the map streamed in more roads: the cars always find the road on ahead
    // (fed only around the view every 120 m, a car came to the end of what it knew and was taken off)
    std::unordered_map<std::int64_t, std::size_t> ai_traffic_fed_cells; // cell -> map size when fed
    int ai_traffic_ring_i = 0;
    double ai_traffic_feed_t = -1.0, ai_traffic_log_t = 0.0;
    struct AiGroundCell {
        float h = 0.f;
        std::atomic<std::uint32_t> gen{0};
        AiGroundCell(float h_, std::uint32_t g) : h(h_), gen(g) {}
    };
    std::unordered_map<std::int64_t, AiGroundCell> ai_traffic_ground_cache; // height, surface_gen
    // the ground under the AI cars: the editor's surface height (roads, objects, terrain) on a 0.5 m grid
    // (openOMSI steps the cars on several threads at once: cache hits are read side by side under a shared lock;
    //  the editor's height query - which keeps caches of its own - runs one caller at a time under a lock of its
    //  own, so a car finding its cell never waits behind another car's query - the main thread did, 5-20 ms)
    std::shared_mutex ai_traffic_ground_mu;
    std::mutex ai_traffic_ground_query_mu;
    std::atomic<std::uint64_t> ai_ground_hits{0}, ai_ground_misses{0}, ai_ground_miss_us{0}; // (AI_SLOW)
    std::function<float(float, float)> ai_traffic_ground_fn = [&](float gx, float gy) {
        constexpr float kCell = 0.5f;
        const int ix = static_cast<int>(std::floor(gx / kCell)), iy = static_cast<int>(std::floor(gy / kCell));
        const std::int64_t key = (static_cast<std::int64_t>(ix) << 32) ^ static_cast<std::uint32_t>(iy);
        const float cx = (ix + 0.5f) * kCell, cy = (iy + 0.5f) * kCell;
        {
            std::shared_lock<std::shared_mutex> lk(ai_traffic_ground_mu);
            if (const auto it = ai_traffic_ground_cache.find(key); it != ai_traffic_ground_cache.end()) {
                // (still good unless a [surface] object - crossing plate, square - came or went over this cell)
                if (surface_cache_valid(it->second.gen.load(std::memory_order_relaxed), cx, cy)) {
                    ai_ground_hits.fetch_add(1, std::memory_order_relaxed);
                    it->second.gen.store(surface_gen, std::memory_order_relaxed);
                    return it->second.h;
                }
            }
        }
        float h = 0.f;
        {
            const auto q0 = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> q(ai_traffic_ground_query_mu);
            h = bus_ground(cx, cy, camera.target_z + 40.f);
            ai_ground_misses.fetch_add(1, std::memory_order_relaxed);
            ai_ground_miss_us.fetch_add(static_cast<std::uint64_t>(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - q0).count()),
                std::memory_order_relaxed);
        }
        std::unique_lock<std::shared_mutex> lk(ai_traffic_ground_mu);
        if (ai_traffic_ground_cache.size() > 600000) ai_traffic_ground_cache.clear();
        ai_traffic_ground_cache.erase(key);
        ai_traffic_ground_cache.try_emplace(key, h, surface_gen);
        return h;
    };

    // the AI car picked with a click (0: none): its details / delete window (map_editor_run_loop_04)
    std::uint64_t ai_selected_id = 0;
    ai_pick_hook = [&](float mx, float my, float fw, float fh, const float* wvp, float* t_out,
                       std::uint64_t* id_out) -> bool {
        static std::vector<std::size_t> s_chunks;
        s_chunks.clear();
        for (const auto& [id, car] : ai_traffic_cars)
            for (const auto ch : car.chunks)
                if (ch != kNoChunk) s_chunks.push_back(ch);
        if (s_chunks.empty()) return false;
        float t = 1e30f;
        const auto hit = renderer.pick_chunk(mx, my, static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh), wvp,
                                             false, s_chunks.data(), s_chunks.size(), &t);
        if (!hit) return false;
        for (const auto& [id, car] : ai_traffic_cars)
            if (std::find(car.chunks.begin(), car.chunks.end(), *hit) != car.chunks.end()) {
                if (t_out) *t_out = t;
                if (id_out) *id_out = id;
                return true;
            }
        return false;
    };
    const auto ai_traffic_lower = [](std::string t) {
        for (auto& c : t) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    auto ai_traffic_release_all = [&]() {
        for (auto& [id, car] : ai_traffic_cars)
            for (const auto ch : car.chunks)
                if (ch != kNoChunk) renderer.release_chunk_gpu(ch);
        ai_traffic_cars.clear();
        for (auto it = ai_traffic_protos.begin(); it != ai_traffic_protos.end();) {
            for (const auto ch : it->second.all) renderer.release_chunk_gpu(ch);
            // (a model still read on a worker keeps its entry: the job writes into it)
            if (it->second.state == 1) ++it;
            else it = ai_traffic_protos.erase(it);
        }
    };
    auto ai_traffic_stop = [&]() {
        ai_traffic_release_all();
        if (ai_traffic) oo_traffic_destroy(ai_traffic);
        ai_traffic = nullptr;
        ai_traffic_owners_sent.clear();
        ai_traffic_fed_cells.clear();
        ai_traffic_light_objs.clear();
    };
    // a part's model (front or coupled section of a car type) on the GPU once, hidden: the cars clone it
    auto ai_traffic_proto = [&](const std::string& part_file) -> AiTrafficProto& {
        const std::string key = ai_traffic_lower(part_file);
        auto [it, fresh] = ai_traffic_protos.try_emplace(key);
        auto& pr = it->second;
        if (!fresh) return pr;
        pr.state = 1;
        pr.name = omsi::path_text(std::filesystem::u8path(part_file).filename());
        auto done = pr.read_done;
        auto holder = pr.model;
        omsi::JobSystem::global().submit(
            [part_file, done, holder, &prefetch_model_assets]() {
                try {
                    if (const auto doc = omsi::bus::load_bus(std::filesystem::u8path(part_file))) {
                        auto model = omsi::model_cfg::load_model_cfg(omsi::bus::resolve_model_cfg_path(*doc));
                        if (model && omsi::model_cfg::primary_lod(*model)) {
                            prefetch_model_assets(*model);
                            *holder = std::move(model);
                        }
                    }
                } catch (...) {
                }
                done->store(true, std::memory_order_release);
            },
            omsi::JobSystem::Priority::Low);
        return pr;
    };
    // the models read so far go up to the GPU, a few milliseconds a frame
    auto ai_traffic_pump_protos = [&](double budget_ms) {
        const auto t0 = std::chrono::steady_clock::now();
        auto spent = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
        for (auto& [key, pr] : ai_traffic_protos) {
            if (pr.state == 1 && pr.read_done->load(std::memory_order_acquire)) pr.state = pr.model->has_value() ? 2 : -1;
            if (pr.state != 2) continue;
            const auto& model = **pr.model;
            const auto* lod = omsi::model_cfg::primary_lod(model);
            while (lod && pr.next_mesh < lod->meshes.size()) {
                if (spent() > budget_ms) return;
                const auto& mesh = lod->meshes[pr.next_mesh++];
                if (mesh.mesh_file.empty()) continue;
                const auto o3d_path = model.cfg_path.parent_path() / mesh.mesh_file;
                const auto mapped = o3d::load_mapped_shared(o3d_path);
                if (!mapped || !mapped->valid() || !renderer.append_o3d(*mapped, &mesh)) continue;
                const std::size_t chunk = renderer.chunk_count() - 1;
                renderer.set_chunk_visible(chunk, false);
                // (one-sided, depth-written alpha 2 in file order, as Omsi.exe draws a vehicle: CULLMODE stays
                //  CCW for meshes, ZWRITEENABLE = !noZwrite - see model_cfg_loader apply_omsi_material_state)
                const auto canon = std::filesystem::weakly_canonical(o3d_path).u8string();
                pr.by_file[ai_traffic_lower(std::string(canon.begin(), canon.end()))].push_back(chunk);
                pr.all.push_back(chunk);
                if (mesh.viewpoint != 0) pr.viewpoint.emplace(chunk, mesh.viewpoint);
                if (auto mc = vehicle_matl_changes(mesh, o3d_path); !mc.empty()) pr.changes.emplace(chunk, std::move(mc));
            }
            pr.state = 3;
            pr.ok = !pr.all.empty();
            log::info("ai traffic: model " + pr.name + " " + std::to_string(pr.all.size()) + " meshes on the GPU");
        }
    };
    // the lanes of the paths around (x, y) not handed over yet (the bus autopilot's own network builder)
    auto ai_traffic_feed = [&](float x, float y) {
        AiNet keep = std::move(ai_net);
        ai_build_quiet = true;
        ai_build(x, y);
        ai_build_quiet = false;
        AiNet around = std::move(ai_net);
        ai_net = std::move(keep);
        ensure_edit_obj_index();
        std::unordered_map<std::int32_t, const map::SplineInst*> spl_by_id;
        for (const auto& sp : edit_splines) spl_by_id.emplace(sp.id, &sp);
        std::vector<OoLaneDesc> descs;
        std::vector<float> pts;
        std::vector<float> groups; // (group, density) pairs of every lane's [rule] trafficdensity
        auto rules_of = [&](const AiPath& ap) -> const std::vector<map::PathRuleEntry>* {
            if (ap.from_object) {
                const auto it = edit_obj_by_id.find(ap.owner);
                return it != edit_obj_by_id.end() && it->second ? &it->second->path_rules : nullptr;
            }
            const auto it = spl_by_id.find(ap.owner);
            return it != spl_by_id.end() ? &it->second->path_rules : nullptr;
        };
        auto kw_is = [](const std::string& k, const char* w) {
            if (k.size() != std::strlen(w)) return false;
            for (std::size_t i = 0; i < k.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(k[i])) != w[i]) return false;
            return true;
        };
        std::unordered_set<std::int64_t> owners_now;
        for (const auto& ap : around.paths) {
            const std::int64_t ok = (static_cast<std::int64_t>(ap.owner) << 1) | (ap.from_object ? 1 : 0);
            if (ai_traffic_owners_sent.count(ok)) continue;
            owners_now.insert(ok);
            if (ap.pts.size() < 2) continue;
            OoLaneDesc d{};
            d.kind = 0;
            d.width = 3.f;
            d.speed_limit_kmh = ap.speed_limit;
            d.density = 1.f;
            d.priority = -1.f;
            d.source = ap.from_object ? 2 : 1;
            d.id = ap.owner;
            d.path = ap.path_index;
            d.light_object = -1;
            d.light_index = -1;
            const std::size_t g0 = groups.size() / 2;
            if (const auto* rules = rules_of(ap)) {
                float dens0 = -1.f, dens_max = 0.f;
                for (const auto& r : *rules) {
                    if (r.path_index != ap.path_index || r.kill) continue;
                    if (kw_is(r.keyword, "trafficdensity")) {
                        if (r.extra == 0) dens0 = r.value;
                        dens_max = (std::max)(dens_max, r.value);
                        // per group, as Omsi.exe keeps it: which AI groups may spawn and drive on the path
                        groups.push_back(static_cast<float>((std::max)(0, static_cast<int>(r.extra))));
                        groups.push_back(r.value);
                    } else if (kw_is(r.keyword, "no_cars")) {
                        d.no_cars = 1;
                    } else if (kw_is(r.keyword, "buses") || kw_is(r.keyword, "bus")) {
                        d.rule_bus = 1;
                    } else if (kw_is(r.keyword, "trucks")) {
                        d.rule_trucks = 1;
                    } else if (kw_is(r.keyword, "overtaking_prohib")) {
                        d.no_overtaking = 1;
                    } else if (kw_is(r.keyword, "priority")) {
                        d.priority = r.value;
                    }
                }
                // (openOMSI path_densities: the first group's density, else 1, and at least any group's)
                d.density = (std::max)(dens0 >= 0.f ? dens0 : 1.f, dens_max);
            }
            if (ap.light_obj >= 0 && ap.light_idx >= 0) {
                const auto it = edit_obj_by_id.find(ap.light_obj);
                if (it != edit_obj_by_id.end() && it->second) {
                    const auto u8 = it->second->sco_path.u8string();
                    if (oo_traffic_light_program(ai_traffic, ap.light_obj, std::string(u8.begin(), u8.end()).c_str()) >= 0) {
                        ai_traffic_light_objs.insert(ap.light_obj);
                        d.light_object = ap.light_obj;
                        d.light_index = ap.light_idx;
                    }
                }
            }
            d.group_first = static_cast<std::int32_t>(g0);
            d.n_groups = static_cast<std::int32_t>(groups.size() / 2 - g0);
            for (const bool rev : {false, true}) {
                if ((ap.dir == 0 && rev) || (ap.dir == 1 && !rev)) continue;
                OoLaneDesc e = d;
                e.reversed = rev ? 1 : 0;
                e.n_points = static_cast<std::int32_t>(ap.pts.size());
                if (!rev)
                    for (const auto& q : ap.pts) pts.insert(pts.end(), {q[0], q[1], q[2]});
                else
                    for (auto it = ap.pts.rbegin(); it != ap.pts.rend(); ++it) pts.insert(pts.end(), {(*it)[0], (*it)[1], (*it)[2]});
                descs.push_back(e);
            }
        }
        if (!descs.empty()) {
            const int added = oo_traffic_add_lanes(ai_traffic, descs.data(), descs.size(), pts.data(),
                                                   groups.empty() ? nullptr : groups.data());
            log::info("ai traffic: +" + std::to_string(added) + " lanes from " + std::to_string(owners_now.size()) +
                      " roads / crossings around (" + std::to_string(static_cast<int>(x)) + "," +
                      std::to_string(static_cast<int>(y)) + "), " + std::to_string(oo_traffic_lane_count(ai_traffic)) +
                      " in all");
        }
        ai_traffic_owners_sent.insert(owners_now.begin(), owners_now.end());
    };
    auto ai_traffic_frame = [&](float dt) {
        static const int env_target = [] {
            const char* e = std::getenv("OMSI_AI_TRAFFIC");
            return e && *e ? std::atoi(e) : 0;
        }();
        const auto& stv = panel.imgui_state();
        const int want = env_target > 0 ? env_target : (stv.ai_traffic_on ? stv.ai_traffic_cars : 0);
        if (want <= 0) {
            if (ai_traffic) ai_traffic_stop();
            return;
        }
        if (!oo_ready()) return;
        // (AI_SLOW: where a slow traffic frame went, one line when it takes over 6 ms)
        auto ai_lap_t = std::chrono::steady_clock::now();
        double ai_ms[7]{};
        auto ai_lap = [&](int slot) {
            const auto t = std::chrono::steady_clock::now();
            ai_ms[slot] += std::chrono::duration<double, std::milli>(t - ai_lap_t).count();
            ai_lap_t = t;
        };
        struct AiSlowLog {
            double* ms;
            float* last;
            std::atomic<std::uint64_t>* g_hit;
            std::atomic<std::uint64_t>* g_miss;
            std::atomic<std::uint64_t>* g_us;
            ~AiSlowLog() {
                double tot = 0.0;
                for (int k = 0; k < 7; ++k) tot += ms[k];
                const float t = static_cast<float>(glfwGetTime());
                if (tot < 6.0 || t - *last < 0.25f) return;
                *last = t;
                char b[320];
                // (ground: cache hits / misses and the misses' query time since the last line)
                std::snprintf(b, sizeof(b), "AI_SLOW total=%.2f surf=%.2f feed=%.2f tick=%.2f protos=%.2f spawn=%.2f cars=%.2f reap=%.2f ground_hit=%llu ground_miss=%llu ground_miss_ms=%.2f",
                              tot, ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6],
                              static_cast<unsigned long long>(g_hit->exchange(0)),
                              static_cast<unsigned long long>(g_miss->exchange(0)), g_us->exchange(0) / 1000.0);
                log::warn(b);
            }
        };
        static float s_ai_slow_t = 0.f;
        AiSlowLog ai_slow_log{ai_ms, &s_ai_slow_t, &ai_ground_hits, &ai_ground_misses, &ai_ground_miss_us};
        refresh_surface_index(); // the cars stand on every [surface] object, bus or no bus
        ai_lap(0);
        if (!ai_traffic) {
            // (made on a worker: it reads the map's ailists and loads every AI vehicle type - a third of a second
            //  and more of a frozen editor when done here)
            static std::future<OoTraffic*> s_creating;
            if (!s_creating.valid()) {
                const auto u8 = cfg.global.root.u8string();
                const std::string dir(u8.begin(), u8.end());
                const float radius = ai_traffic_radius;
                s_creating = std::async(std::launch::async,
                                        [dir, want, radius]() { return oo_traffic_create(dir.c_str(), want, radius, 0); });
                return;
            }
            if (s_creating.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            ai_traffic = s_creating.get();
            if (!ai_traffic) return;
            ai_traffic_target = want;
            oo_traffic_set_ground(
                ai_traffic,
                +[](void* user, double x, double y, double* out_z) -> int32_t {
                    auto* f = static_cast<std::function<float(float, float)>*>(user);
                    *out_z = (*f)(static_cast<float>(x), static_cast<float>(y));
                    return 1;
                },
                &ai_traffic_ground_fn);
            log::info("ai traffic: on, " + std::to_string(want) + " cars within " +
                      std::to_string(static_cast<int>(ai_traffic_radius)) + " m");
        }
        if (want != ai_traffic_target) {
            ai_traffic_target = want;
            oo_traffic_set_target(ai_traffic, want, ai_traffic_radius);
        }
        float ex = 0.f, ey = 0.f, ez = 0.f;
        camera.eye_position(ex, ey, ez);
        const double now = glfwGetTime();
        // new lanes around the ring of points, a cell again when the map grew since it was fed
        if (now - ai_traffic_feed_t > 0.25) {
            ai_traffic_feed_t = now;
            const std::size_t stamp = edit_splines.size() + edit_objects.size();
            const bool bus = omsi_bus != nullptr;
            const int n_ring = bus ? 10 : 9;
            for (int tries = 0; tries < n_ring; ++tries) {
                const int r = ai_traffic_ring_i++ % n_ring;
                float px = ex, py = ey;
                if (r >= 1 && r <= 8) {
                    const float a = static_cast<float>(r - 1) * 0.785398f;
                    px += 380.f * std::cos(a);
                    py += 380.f * std::sin(a);
                } else if (r == 9) {
                    px = omsi_bus->x;
                    py = omsi_bus->y;
                }
                constexpr float kCell = 250.f;
                const std::int64_t cell = (static_cast<std::int64_t>(std::floor(px / kCell)) << 32) ^
                                          static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(py / kCell)));
                auto [it, fresh] = ai_traffic_fed_cells.try_emplace(cell, stamp);
                if (!fresh && it->second == stamp) continue;
                it->second = stamp;
                ai_traffic_feed((std::floor(px / kCell) + 0.5f) * kCell, (std::floor(py / kCell) + 0.5f) * kCell);
                break; // (one a quarter second: a network build is a few milliseconds on X10 Berlin)
            }
            if (ai_traffic_fed_cells.size() > 4096) ai_traffic_fed_cells.clear();
        }
        ai_lap(1);
        // the player's bus: its front section's box and every coupled section's (the traffic sees the whole
        // articulated bus, not a 12 m box around its front)
        oo_traffic_set_bus(ai_traffic, omsi_bus && omsi_bus->oo ? omsi_bus->oo : nullptr);
        // the lights as the editor shows them (the crossing's or its [varparent]'s program, on the editor's
        // clock and time scale): the traffic ran a program of its own, out of step with the lamps on screen
        // (at x5 completely), and drove through what looked red
        for (const auto obj : ai_traffic_light_objs)
            for (int li = 0; li < 32; ++li) {
                const int ph = scenery_scripts.traffic_light_phase(obj, li);
                if (ph < 0 || !oo_traffic_light_set(ai_traffic, obj, li, ph)) break;
            }
        const float fx = camera.target_x - ex, fy = camera.target_y - ey;
        oo_traffic_tick(ai_traffic, dt, ex, ey, ez, fx, fy, nullptr, panel.imgui_state().sim_hour);
        ai_lap(2);
        ai_traffic_pump_protos(3.0);
        ai_lap(3);
        // pictures: clones of each type's model, posed by openOMSI
        for (auto& [id, car] : ai_traffic_cars) car.seen = false;
        const int n = oo_traffic_car_count(ai_traffic);
        char buf[1024];
        for (int i = 0; i < n; ++i) {
            std::uint64_t id = 0;
            int mc = 0;
            if (!oo_traffic_car(ai_traffic, i, &id, &mc)) continue;
            auto& car = ai_traffic_cars[id];
            car.seen = true;
            ai_lap(5);
            if (car.chunks.empty() && mc > 0) {
                bool parts_ready = true;
                for (int k = 0; k < mc && parts_ready; ++k)
                    if (oo_traffic_car_part_file(ai_traffic, i, k, buf, sizeof(buf))) {
                        const int stt = ai_traffic_proto(std::string(buf)).state;
                        parts_ready = stt == 3 || stt == -1;
                    }
                if (!parts_ready) continue; // (its model is still being read: it shows once that is done)
                car.chunks.assign(static_cast<std::size_t>(mc), kNoChunk);
                car.changes.assign(static_cast<std::size_t>(mc), nullptr);
                car.mc_state.assign(static_cast<std::size_t>(mc), {});
                std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> used; // part -> file -> taken
                for (int k = 0; k < mc; ++k) {
                    if (!oo_traffic_car_part_file(ai_traffic, i, k, buf, sizeof(buf))) continue;
                    const std::string part(buf);
                    auto& pr = ai_traffic_proto(part);
                    if (!pr.ok || !oo_traffic_car_mesh_file(ai_traffic, i, k, buf, sizeof(buf))) continue;
                    const auto canon = std::filesystem::weakly_canonical(std::filesystem::u8path(buf)).u8string();
                    const std::string file = ai_traffic_lower(std::string(canon.begin(), canon.end()));
                    const auto it = pr.by_file.find(file);
                    if (it == pr.by_file.end()) {
                        static std::unordered_set<std::string> s_logged_miss;
                        if (s_logged_miss.size() < 200 && s_logged_miss.insert(file).second)
                            log::info("ai traffic: DIAG no model mesh for " + file + " (part " + pr.name + ", " +
                                      std::to_string(pr.by_file.size()) + " files in its model)");
                        continue;
                    }
                    std::size_t& taken = used[ai_traffic_lower(part)][file];
                    if (taken >= it->second.size()) continue;
                    const std::size_t src = it->second[taken++];
                    if (const auto cl = renderer.clone_chunk(src)) {
                        car.chunks[static_cast<std::size_t>(k)] = *cl;
                        renderer.set_chunk_size_cull_scale(*cl, 5.f); // (small parts of far cars not drawn)
                        // ([viewpoint] 0 or bit 4: what an AI-driven vehicle shows - not the player's cab meshes)
                        if (const auto vp = pr.viewpoint.find(src); vp != pr.viewpoint.end() && (vp->second & 4) == 0) {
                            if (car.hide.size() < static_cast<std::size_t>(mc)) car.hide.assign(static_cast<std::size_t>(mc), 0);
                            car.hide[static_cast<std::size_t>(k)] = 1;
                        }
                        // its paint scheme (openOMSI picks one per car from the model's [CTC] .cti list): every slot
                        // showing a [CTCTexture] default takes the scheme's file, as Omsi.exe repaints a vehicle
                        char pb[4096];
                        const std::size_t pn = oo_traffic_car_paint(ai_traffic, i, k, pb, sizeof(pb));
                        std::string_view rest(pb, (std::min)(pn, sizeof(pb) - 1));
                        while (!rest.empty()) {
                            const auto eol = rest.find('\n');
                            const std::string_view line = rest.substr(0, eol);
                            rest = eol == std::string_view::npos ? std::string_view{} : rest.substr(eol + 1);
                            const auto tab = line.find('\t');
                            if (tab == std::string_view::npos || tab == 0 || tab + 1 >= line.size()) continue;
                            std::string key = "ctc:" + std::string(line.substr(0, tab));
                            for (char& c : key) {
                                if (c == '\\') c = '/';
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            }
                            std::filesystem::path file = std::filesystem::u8path(std::string(line.substr(tab + 1)));
                            if (file.is_relative()) {
                                // (not found in the scheme folder: beside the model's own textures, as openOMSI does)
                                if (pr.model && pr.model->has_value())
                                    file = (*pr.model)->cfg_path.parent_path().parent_path() / "Texture" / file;
                            }
                            car.paint.emplace_back(static_cast<std::size_t>(k), std::move(key), std::move(file));
                        }
                        if (const auto ci = pr.changes.find(src); ci != pr.changes.end())
                            car.changes[static_cast<std::size_t>(k)] = &ci->second;
                    }
                }
                // (diagnostics: per type once, how many of its meshes the openOMSI vars hide)
                static std::unordered_set<std::string> s_logged_vis;
                if (oo_traffic_car_part_file(ai_traffic, i, 0, buf, sizeof(buf)) && s_logged_vis.size() < 60 &&
                    s_logged_vis.insert(buf).second) {
                    int shown = 0, hidden = 0, none = 0;
                    std::string hidden_names;
                    for (int k = 0; k < mc; ++k) {
                        float m16[16];
                        const int r = oo_traffic_car_mesh_world(ai_traffic, i, k, m16);
                        if (r == 1) ++shown;
                        else if (r == 2) {
                            ++hidden;
                            char fb[512];
                            if (hidden <= 6 && oo_traffic_car_mesh_file(ai_traffic, i, k, fb, sizeof(fb)))
                                hidden_names += std::string(" ") + std::filesystem::u8path(fb).filename().string();
                        } else ++none;
                    }
                    int chunks = 0;
                    for (const auto c : car.chunks) chunks += c != kNoChunk;
                    log::info("ai traffic: DIAG type " + std::filesystem::u8path(buf).filename().string() + ": " +
                              std::to_string(mc) + " meshes, " + std::to_string(chunks) + " with a model chunk, shown " +
                              std::to_string(shown) + " hidden " + std::to_string(hidden) + hidden_names);
                    for (std::size_t k = 0; k < car.chunks.size() && k < 10; ++k)
                        if (car.chunks[k] != kNoChunk)
                            log::info("ai traffic: DIAG   " + renderer.chunk_debug_line(car.chunks[k]));
                }
            }
            ai_lap(4);
            const bool lamps = now - car.lamps_t > 0.15;
            if (lamps) car.lamps_t = now;
            // paint: the scheme's texture decodes on a worker; the slot shows its own until then (looked at on
            // the lamp tick, not every frame: one texture goes up a frame, the rest of the cars only wait)
            for (auto pit = car.paint.begin(); lamps && pit != car.paint.end();) {
                const auto& [pk, key, file] = *pit;
                const std::size_t pch = pk < car.chunks.size() ? car.chunks[pk] : kNoChunk;
                if (pch == kNoChunk) { pit = car.paint.erase(pit); continue; }
                renderer.set_chunk_freetex_variant(pch, key, file);
                if (renderer.chunk_freetex_variant_ready(pch, key, file)) pit = car.paint.erase(pit);
                else ++pit;
            }
            auto car_var = [&](const std::string& name) { return oo_traffic_car_var(ai_traffic, i, name.c_str(), 0.f); };
            for (int k = 0; k < mc && k < static_cast<int>(car.chunks.size()); ++k) {
                const std::size_t ch = car.chunks[static_cast<std::size_t>(k)];
                if (ch == kNoChunk) continue;
                float m16[16];
                const int r = oo_traffic_car_mesh_world(ai_traffic, i, k, m16);
                if (lamps && r == 1) {
                    // its lamps (head / brake / blinker textures) and light maps follow its script variables
                    if (const auto* chg = car.changes[static_cast<std::size_t>(k)])
                        vehicle_apply_changes(ch, *chg, car.mc_state[static_cast<std::size_t>(k)], car_var);
                    if (renderer.chunk_has_uv_vars(ch)) renderer.apply_chunk_uv_vars(ch, car_var);
                }
                if (r == 0) {
                    renderer.set_chunk_visible(ch, false);
                    continue;
                }
                XMFLOAT4X4 m{};
                std::memcpy(&m, m16, sizeof(m));
                renderer.set_chunk_world_matrix(ch, store_world_transposed(XMLoadFloat4x4(&m)).data());
                renderer.set_chunk_visible(ch, r == 1 && !(static_cast<std::size_t>(k) < car.hide.size() && car.hide[static_cast<std::size_t>(k)]));
            }
        }
        ai_lap(5);
        for (auto it = ai_traffic_cars.begin(); it != ai_traffic_cars.end();) {
            if (it->second.seen) {
                ++it;
                continue;
            }
            for (const auto ch : it->second.chunks)
                if (ch != kNoChunk) renderer.release_chunk_gpu(ch);
            it = ai_traffic_cars.erase(it);
        }
        if (now - ai_traffic_log_t > 10.0) {
            ai_traffic_log_t = now;
            if (oo_traffic_stats(ai_traffic, buf, sizeof(buf))) log::info(std::string("ai traffic: ") + buf);
            int slots = 0, lit = 0, cars_lit = 0;
            for (const auto& [cid, c] : ai_traffic_cars) {
                int on = 0;
                for (const auto& st : c.mc_state)
                    for (const auto v : st) {
                        ++slots;
                        on += v == 1;
                    }
                lit += on;
                cars_lit += on > 0;
            }
            log::info("ai traffic: lamps " + std::to_string(lit) + "/" + std::to_string(slots) + " slots on, " +
                      std::to_string(cars_lit) + " cars with a lamp lit");
        }
        ai_lap(6);
    };
