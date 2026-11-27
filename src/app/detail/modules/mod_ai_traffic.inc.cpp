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
    std::unordered_set<std::int32_t> ai_traffic_light_objs;
    struct AiServiceBus {
        std::uint64_t id = 0;
        std::size_t trip = 0; // index into ai_departures
        double born = 0.0;
    };
    std::vector<AiServiceBus> ai_service_buses;   // crossings whose light program the traffic has
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
        ai_service_buses.clear();
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
            d.width = ap.width;
            d.turn = ap.turn;
            d.offset = ap.offset;
            // ([crossingproblem] not passed on: it makes the meeting place both whole lanes - a car waited until
            //  the other had driven its whole path, as Omsi.exe stalls with it; the geometric meeting place decides)
            d.crossing_problem = 0;
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
    // Timetable buses: the map's trips (TTData) served by its AI buses, as Omsi.exe runs the timetable. A trip
    // whose track starts near the view gets a bus now and then (OMSI_AI_SERVICE=<n>, default 4 at once); it drives
    // the track's lanes and serves its stations at their times (openOMSI BusService).
    double ai_service_t = -100.0;
    // The timetable as it runs (Omsi.exe / openOMSI timetable_run): every tour's trips at their departure times. A
    // departure under way now is somewhere on its track - where, follows from the trip's station times and the
    // clock (no simulation off screen: the position is worked out). Where that point lies in the loaded network
    // near the view, its bus is put there and drives on, serving the stations left at their times; it goes once
    // its trip is done or the view is far from it, and comes back where it should be when the view returns.
    // Moving the clock puts every bus where the new time has it.
    struct AiDeparture {
        std::size_t trip = 0;
        int profile = 0;
        double dep_min = 0.0, dur_min = 0.0;
        std::vector<float> times;      // minutes from departure, per station
        std::vector<int> station_entry; // track entry index per station (-1 unknown)
    };
    std::vector<AiDeparture> ai_departures;
    // Herramientas > Buses IA: every departure under way on the whole map (worked out from the clock, seen or not)
    struct AiBusRow {
        std::size_t di = 0;
        std::string line, trip, terminus;
        int rel_min = 0, dur_min = 0;
        bool has_pos = false; // where it is (its stations loaded), else where its next station's tile is
        float x = 0.f, y = 0.f;
    };
    std::vector<AiBusRow> ai_bus_rows;
    std::uint64_t ai_drive_request = 0;
    std::uint64_t ai_follow_id = 0, ai_follow_pick = 0;
    std::size_t ai_pending_follow = static_cast<std::size_t>(-1), ai_pending_drive = static_cast<std::size_t>(-1);
    bool ai_pending_exact = false; // the camera is on the pending bus's exact place (its stations loaded)
    bool ai_departures_built = false;
    double ai_service_clock = -1.0;
    std::unordered_set<std::size_t> ai_departures_done; // served already (not again until the clock is moved)
    auto ai_service_build = [&]() {
        ai_departures_built = true;
        auto& tm = omsi::tt::global_tt_man();
        if (!tm.loaded) tm.ensure_loaded_now(cfg.global.root);
        std::unordered_map<std::string, std::size_t> by_name;
        for (std::size_t i = 0; i < tm.data.trips.size(); ++i) {
            std::string n = tm.data.trips[i].name;
            for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            by_name.emplace(n, i);
        }
        for (const auto& ln : tm.data.lines)
            for (const auto& tour : ln.tours)
                for (const auto& tt : tour.trips) {
                    std::string n = tt.trip_name;
                    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    const auto it = by_name.find(n);
                    if (it == by_name.end()) continue;
                    AiDeparture d;
                    d.trip = it->second;
                    d.profile = tt.profile_index;
                    d.dep_min = tt.departure_minutes;
                    d.times = tm.station_times(static_cast<int>(d.trip), d.profile);
                    if (d.times.empty()) continue;
                    d.dur_min = d.times.back();
                    for (const auto& stn : tm.data.trips[d.trip].stations) d.station_entry.push_back(stn.typ2 ? -1 : stn.track_entry_index);
                    ai_departures.push_back(std::move(d));
                }
        log::info("ai traffic: timetable " + std::to_string(ai_departures.size()) + " departures in " +
                  std::to_string(tm.data.lines.size()) + " lines");
    };
    auto ai_service_frame = [&](float ex, float ey, double now) {
        static const int env_n = [] {
            const char* e = std::getenv("OMSI_AI_SERVICE");
            return e && *e ? std::atoi(e) : -1;
        }();
        const int cap = env_n >= 0 ? env_n : 16;
        if (cap <= 0) return;
        if (now - ai_service_t < 2.0) return;
        const double dt_wall = ai_service_t < 0.0 ? 0.0 : now - ai_service_t;
        ai_service_t = now;
        if (!ai_departures_built) ai_service_build();
        auto& tm = omsi::tt::global_tt_man();
        const double clock_min = static_cast<double>(panel.imgui_state().sim_hour) * 60.0;
        // the clock jumped (set by hand, not run): every bus to where the new time has it
        const bool jumped = ai_service_clock >= 0.0 && std::fabs(clock_min - ai_service_clock) > 3.0 + dt_wall * 0.5 &&
                            std::fabs(std::fabs(clock_min - ai_service_clock) - 1440.0) > 3.0;
        ai_service_clock = clock_min;
        if (jumped) ai_departures_done.clear();
        // buses gone (trip done, taken off) or gone_far from the view
        std::unordered_map<std::uint64_t, std::array<float, 2>> pos_of;
        {
            const int nc = oo_traffic_car_count(ai_traffic);
            for (int i = 0; i < nc; ++i) {
                std::uint64_t cid = 0;
                int mc = 0;
                if (!oo_traffic_car(ai_traffic, i, &cid, &mc) || mc <= 0) continue;
                float m16[16];
                if (oo_traffic_car_mesh_world(ai_traffic, i, 0, m16)) pos_of[cid] = {m16[12], m16[13]};
            }
        }
        for (auto it = ai_service_buses.begin(); it != ai_service_buses.end();) {
            bool gone_far = jumped;
            if (const auto pp = pos_of.find(it->id); pp != pos_of.end())
                gone_far = gone_far || std::hypot(pp->second[0] - ex, pp->second[1] - ey) > 650.f;
            if (gone_far) (void)oo_traffic_remove_car(ai_traffic, it->id);
            const bool vanished = !pos_of.count(it->id) && now - it->born > 6.0;
            if (vanished && !jumped) ai_departures_done.insert(it->trip);
            if (gone_far || vanished) {
                log::info(std::string("ai traffic: timetable bus ") + std::to_string(it->id) + " off: " +
                          (jumped ? "clock moved" : gone_far ? "far from the view" : "trip done / taken off"));
                it = ai_service_buses.erase(it);
            }
            else ++it;
        }
        // (the list is made at the cap too: it went stale once the map had its buses)
        const bool at_cap = static_cast<int>(ai_service_buses.size()) >= cap;
        std::vector<std::array<float, 3>> pts;
        int placed = 0, c_run = 0, c_track = 0, c_loaded = 0, c_near = 0, c_fail = 0;
        // the list: every departure under way now
        ai_bus_rows.clear();
        for (std::size_t di = 0; di < ai_departures.size() && ai_bus_rows.size() < 400; ++di) {
            const auto& d = ai_departures[di];
            double rel = clock_min - d.dep_min;
            if (rel < 0.0) rel += 1440.0;
            if (rel < 0.0 || rel >= d.dur_min) continue;
            const auto& tr = tm.data.trips[d.trip];
            AiBusRow r;
            r.di = di;
            r.line = tr.line;
            r.trip = tr.name;
            r.terminus = tr.terminus;
            r.rel_min = static_cast<int>(rel);
            r.dur_min = static_cast<int>(d.dur_min);
            std::size_t k = 0;
            while (k + 1 < d.times.size() && d.times[k + 1] <= rel) ++k;
            for (std::size_t q = k; q < tr.stations.size() && q <= k + 1; ++q) {
                AiTrip::Stop s2;
                s2.id = tr.stations[q].object_id;
                std::array<float, 3> sp{};
                bool exact = false;
                if (trip_stop_position(s2, sp, exact) && exact) {
                    r.has_pos = true;
                    r.x = sp[0];
                    r.y = sp[1];
                    break;
                }
            }
            if (!r.has_pos && k + 1 < tr.stations.size()) {
                // (not loaded: the middle of its next station's tile)
                const int ti = tr.stations[k + 1].tile_index;
                if (ti >= 0 && ti < static_cast<int>(cfg.global.tiles.size())) {
                    const auto& tl = cfg.global.tiles[static_cast<std::size_t>(ti)];
                    const auto [wx, wy] = world_from_tile_local(150.f, 150.f, tl.grid_x, tl.grid_y);
                    r.x = wx - tile_ox;
                    r.y = wy - tile_oy;
                }
            }
            ai_bus_rows.push_back(std::move(r));
        }
        // Seguir / Conducir on a bus not on the map yet: the camera went to it; it is placed first, over the cap
        // and from farther off, and the camera goes to its exact place once its stations are loaded
        constexpr std::size_t kNone = static_cast<std::size_t>(-1);
        for (std::size_t* pend : {&ai_pending_follow, &ai_pending_drive}) {
            if (*pend == kNone) continue;
            ai_departures_done.erase(*pend);
            const AiBusRow* row = nullptr;
            for (const auto& r : ai_bus_rows)
                if (r.di == *pend) row = &r;
            if (!row) {
                *pend = kNone;
                panel.set_status("Bus IA: ese viaje ya termino");
                continue;
            }
            bool on_map = false;
            for (const auto& b : ai_service_buses)
                if (b.trip == *pend) {
                    on_map = true;
                    if (pend == &ai_pending_follow) {
                        ai_follow_pick = b.id;
                        *pend = kNone;
                    }
                }
            if (!on_map && row->has_pos && !ai_pending_exact) {
                ai_pending_exact = true;
                camera.target_x = row->x;
                camera.target_y = row->y;
                camera.reset_mouse();
            }
        }
        const bool any_pending = ai_pending_follow != kNone || ai_pending_drive != kNone;
        if (at_cap && !any_pending) return;
        std::vector<std::size_t> order;
        order.reserve(ai_departures.size() + 2);
        for (const std::size_t pd : {ai_pending_follow, ai_pending_drive})
            if (pd != kNone && pd < ai_departures.size()) order.push_back(pd);
        const std::size_t n_first = order.size();
        for (std::size_t di = 0; di < ai_departures.size(); ++di)
            if (di != ai_pending_follow && di != ai_pending_drive) order.push_back(di);
        for (std::size_t oi = 0; oi < order.size(); ++oi) {
            const std::size_t di = order[oi];
            const bool pend = oi < n_first;
            if (!pend && (placed >= 2 || at_cap)) break;
            const float near_m = pend ? 900.f : 420.f;
            const auto& d = ai_departures[di];
            double rel = clock_min - d.dep_min;
            if (rel < 0.0) rel += 1440.0;
            if (rel < 0.0 || rel >= d.dur_min) continue; // not under way now
            ++c_run;
            bool active = ai_departures_done.count(di) != 0;
            for (const auto& b : ai_service_buses) active |= b.trip == di;
            if (active) continue;
            const auto& tr = tm.data.trips[d.trip];
            int tk = tr.track_index;
            if (tk == omsi::tt::Trip::kUnresolved) tk = tm.find_track_by_name(tr.track_name);
            if (tk < 0 || tk >= static_cast<int>(tm.data.tracks.size())) {
                // a trip with stations only (OMSI type 2): between the station it left and the next, by time
                std::size_t k = 0;
                while (k + 1 < d.times.size() && d.times[k + 1] <= rel) ++k;
                if (k + 1 >= tr.stations.size()) continue;
                AiTrip::Stop a2, b2;
                a2.id = tr.stations[k].object_id;
                b2.id = tr.stations[k + 1].object_id;
                std::array<float, 3> pa{}, pb{};
                bool ea = false, eb = false;
                const bool ha = trip_stop_position(a2, pa, ea) && ea, hb = trip_stop_position(b2, pb, eb) && eb;
                if (!ha && !hb) continue;
                ++c_loaded;
                const double span = (std::max)(0.01f, d.times[k + 1] - d.times[k]);
                const double f = std::clamp((rel - d.times[k]) / span, 0.0, 1.0);
                std::array<float, 3> here = ha && hb ? std::array<float, 3>{static_cast<float>(pa[0] + (pb[0] - pa[0]) * f),
                                                                           static_cast<float>(pa[1] + (pb[1] - pa[1]) * f), 0.f}
                                                     : (ha ? pa : pb);
                const float dist = std::hypot(here[0] - ex, here[1] - ey);
                if (dist > near_m) continue;
                ++c_near;
                std::vector<double> stops;
                for (std::size_t q = k + 1; q < tr.stations.size(); ++q) {
                    AiTrip::Stop s2;
                    s2.id = tr.stations[q].object_id;
                    std::array<float, 3> sp{};
                    bool exact = false;
                    if (!trip_stop_position(s2, sp, exact) || !exact) break; // (the rest is not loaded yet)
                    const double dep = (d.dep_min + (q < d.times.size() ? d.times[q] : d.dur_min)) * 60.0;
                    stops.insert(stops.end(), {sp[0], sp[1], static_cast<double>(s2.id), std::fmod(dep, 86400.0)});
                }
                if (stops.empty()) continue;
                const std::uint64_t id = oo_traffic_service_bus_stations(ai_traffic, "", here[0], here[1], stops.data(),
                                                                         stops.size() / 4, tr.line.c_str(), tr.terminus.c_str());
                if (!id) {
                    ++c_fail;
                    continue;
                }
                ai_service_buses.push_back({id, di, now});
                if (di == ai_pending_follow) {
                    ai_follow_pick = id;
                    ai_pending_follow = static_cast<std::size_t>(-1);
                }
                ++placed;
                log::info("ai traffic: timetable bus '" + tr.name + "' (line " + tr.line + ", stations only) " +
                          std::to_string(static_cast<int>(rel)) + " min under way, " + std::to_string(static_cast<int>(dist)) +
                          " m from the view, " + std::to_string(stops.size() / 4) + " stops ahead");
                if (static_cast<int>(ai_service_buses.size()) >= cap) break;
                continue;
            }
            const auto& entries = tm.data.tracks[static_cast<std::size_t>(tk)].entries;
            if (entries.size() < 2) continue;
            ++c_track;
            // where it is now: between the station it left and the next, by the share of that stretch's time
            std::size_t k = 0;
            while (k + 1 < d.times.size() && d.times[k + 1] <= rel) ++k;
            int e0 = k < d.station_entry.size() ? d.station_entry[k] : -1;
            int e1 = k + 1 < d.station_entry.size() ? d.station_entry[k + 1] : -1;
            if (e0 < 0) e0 = 0;
            if (e1 < e0) e1 = static_cast<int>(entries.size()) - 1;
            const double span = k + 1 < d.times.size() ? (std::max)(0.01f, d.times[k + 1] - d.times[k]) : 1.0;
            const double f = std::clamp((rel - d.times[k]) / span, 0.0, 1.0);
            const std::size_t ke = (std::min)(entries.size() - 1, static_cast<std::size_t>(e0 + (e1 - e0) * f));
            if (!track_entry_points(entries[ke], nullptr, pts) || pts.empty()) continue; // (not loaded: not here)
            ++c_loaded;
            const float dist = std::hypot(pts[0][0] - ex, pts[0][1] - ey);
            if (dist > near_m) continue;
            ++c_near;
            std::vector<std::int64_t> ent;
            for (std::size_t q = ke; q < entries.size(); ++q)
                ent.insert(ent.end(), {static_cast<std::int64_t>(entries[q].id), static_cast<std::int64_t>(entries[q].path_index)});
            // the stations still ahead, at their timetable times (seconds of the day)
            std::vector<double> stops;
            for (std::size_t q = k + 1; q < tr.stations.size(); ++q) {
                AiTrip::Stop s2;
                s2.id = tr.stations[q].object_id;
                std::array<float, 3> sp{};
                bool exact = false;
                if (!trip_stop_position(s2, sp, exact) || !exact) continue;
                const double dep = (d.dep_min + (q < d.times.size() ? d.times[q] : d.dur_min)) * 60.0;
                stops.insert(stops.end(), {sp[0], sp[1], static_cast<double>(s2.id), std::fmod(dep, 86400.0)});
            }
            const std::uint64_t id = oo_traffic_service_bus(ai_traffic, "", ent.data(), ent.size() / 2, stops.data(),
                                                            stops.size() / 4, tr.line.c_str(), tr.terminus.c_str());
            if (!id) {
                ++c_fail;
                continue;
            }
            ai_service_buses.push_back({id, di, now});
            if (di == ai_pending_follow) {
                ai_follow_pick = id;
                ai_pending_follow = static_cast<std::size_t>(-1);
            }
            ++placed;
            log::info("ai traffic: timetable bus '" + tr.name + "' (line " + tr.line + ") dep " +
                      std::to_string(static_cast<int>(d.dep_min / 60)) + ":" + std::to_string(static_cast<int>(std::fmod(d.dep_min, 60.0))) +
                      ", " + std::to_string(static_cast<int>(rel)) + " min under way, at entry " + std::to_string(ke) + "/" +
                      std::to_string(entries.size()) + ", " + std::to_string(static_cast<int>(dist)) + " m from the view, " +
                      std::to_string(stops.size() / 4) + " stops ahead");
            if (static_cast<int>(ai_service_buses.size()) >= cap) break;
        }
        static double s_diag_t = 0.0;
        if (now - s_diag_t > 10.0) {
            s_diag_t = now;
            log::info("ai traffic: timetable at " + std::to_string(static_cast<int>(clock_min / 60)) + "h: " +
                      std::to_string(c_run) + " under way, " + std::to_string(c_track) + " with a track, " +
                      std::to_string(c_loaded) + " where the map is loaded, " + std::to_string(c_near) + " near the view, " +
                      std::to_string(c_fail) + " not placed, " + std::to_string(ai_service_buses.size()) + " buses on the road");
            for (const auto& b : ai_service_buses) {
                char dbg[512];
                if (oo_traffic_car_debug(ai_traffic, b.id, dbg, sizeof(dbg)))
                    log::info("ai traffic: timetable bus " + std::to_string(b.id) + ": " + dbg);
            }
        }
    };
    // The camera on an AI vehicle: Seguir in the Auto IA window (any car) or the Buses IA window (a timetable
    // bus, picked by hand: sticky), or Vista > Seguir bus IA / OMSI_FOLLOW_SERVICE (a random timetable bus,
    // another when it is gone). Escape stops following. Views as for the player's bus: behind it (free orbit),
    // F1 driver's seat (again: the next one), F2 outside turning with it, F3 passenger (again: the next), F4 behind.
    bool ai_follow_sticky = false, ai_follow_env_off = false, ai_follow_locked = false, ai_follow_snap = true;
    int ai_follow_cam = 0, ai_follow_drv_idx = -1, ai_follow_pax_idx = 0; // cam: 0 behind, 1 driver, 2 outside, 3 pax
    float ai_fc_dth = 0.f, ai_fc_dph = 0.f, ai_fc_last_th = 0.f, ai_fc_last_ph = 0.f;
    std::unordered_map<std::string, std::shared_ptr<const omsi::bus::BusDocument>> ai_bus_docs; // part file -> .bus
    auto ai_bus_doc = [&](const std::string& file) -> const omsi::bus::BusDocument* {
        auto [it, fresh] = ai_bus_docs.try_emplace(file);
        if (fresh) {
            try {
                if (auto d = omsi::bus::load_bus(std::filesystem::u8path(file)))
                    it->second = std::make_shared<const omsi::bus::BusDocument>(std::move(*d));
            } catch (...) {
            }
        }
        return it->second.get();
    };
    auto ai_follow_release_cam = [&]() {
        if (!ai_follow_locked) return;
        ai_follow_locked = false;
        camera.editor_nav = true;
        camera.orbit_target_locked = false;
        camera.radius = (std::max)(camera.radius, 25.f);
        camera.reset_mouse();
    };
    auto ai_follow_start = [&](std::uint64_t id) {
        ai_follow_id = id;
        ai_follow_sticky = true;
        ai_follow_snap = true;
        log::info("ai traffic: camera follows " + std::to_string(id));
    };
    auto ai_follow_stop = [&]() {
        ai_follow_id = 0;
        ai_follow_sticky = false;
        ai_follow_env_off = true;
        ai_pending_follow = static_cast<std::size_t>(-1);
        panel.imgui_state().follow_ai_bus = false;
        ai_follow_release_cam();
        ai_follow_cam = 0;
    };
    // the views of the followed vehicle (buttons in the Auto IA / Buses IA windows)
    auto ai_follow_cam_buttons = [&]() {
        static const char* kNames[] = {"Detras", "Conductor", "Exterior", "Pasajero"};
        for (int m = 0; m < 4; ++m) {
            if (m) ImGui::SameLine();
            if (ImGui::RadioButton(kNames[m], ai_follow_cam == m)) {
                if (m == 1 && ai_follow_cam == 1) ++ai_follow_drv_idx;
                if (m == 3 && ai_follow_cam == 3) ++ai_follow_pax_idx;
                ai_follow_cam = m;
                ai_follow_snap = true;
            }
        }
        ImGui::TextDisabled("F1-F4 camaras, Esc deja de seguir");
    };
    auto ai_follow_frame = [&]() {
        static const bool env_follow = std::getenv("OMSI_FOLLOW_SERVICE") != nullptr;
        auto& stv = panel.imgui_state();
        if (ai_follow_pick) {
            ai_follow_start(ai_follow_pick);
            ai_follow_pick = 0;
        }
        const bool following = ai_follow_id != 0 || stv.follow_ai_bus || ai_pending_follow != static_cast<std::size_t>(-1);
        if (following && !ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ai_follow_stop();
            panel.set_status("Seguir IA: detenido");
            return;
        }
        const bool random_mode = (env_follow && !ai_follow_env_off) || stv.follow_ai_bus;
        if (!ai_traffic || (!ai_follow_sticky && !random_mode)) {
            ai_follow_id = 0;
            ai_follow_sticky = false;
            ai_follow_release_cam();
            return;
        }
        auto find_i = [&](std::uint64_t want) {
            const int nc = oo_traffic_car_count(ai_traffic);
            for (int i = 0; i < nc; ++i) {
                std::uint64_t cid = 0;
                int mc = 0;
                if (oo_traffic_car(ai_traffic, i, &cid, &mc) && cid == want && mc > 0) return i;
            }
            return -1;
        };
        int fi = ai_follow_id ? find_i(ai_follow_id) : -1;
        if (fi < 0) {
            if (ai_follow_sticky) {
                log::info("ai traffic: followed vehicle " + std::to_string(ai_follow_id) + " gone");
                ai_follow_id = 0;
                ai_follow_sticky = false;
                ai_follow_release_cam();
                return;
            }
            ai_follow_id = 0;
            if (!ai_service_buses.empty()) {
                ai_follow_id = ai_service_buses[static_cast<std::size_t>(std::rand()) % ai_service_buses.size()].id;
                ai_follow_snap = true;
                log::info("ai traffic: camera follows timetable bus " + std::to_string(ai_follow_id));
                fi = find_i(ai_follow_id);
            }
            if (fi < 0) return;
        }
        // F1-F4 as for the player's bus (only while no bus of the player's is out)
        if (!omsi_bus && !ImGui::GetIO().WantCaptureKeyboard) {
            const ImGuiKey keys[4] = {ImGuiKey_F4, ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3};
            for (int m = 0; m < 4; ++m)
                if (ImGui::IsKeyPressed(keys[m], false)) {
                    if (m == 1 && ai_follow_cam == 1) ++ai_follow_drv_idx;
                    if (m == 3 && ai_follow_cam == 3) ++ai_follow_pax_idx;
                    ai_follow_cam = m;
                    ai_follow_snap = true;
                }
        }
        float body16[16];
        if (!oo_traffic_car_body(ai_traffic, fi, body16) && !oo_traffic_car_mesh_world(ai_traffic, fi, 0, body16)) return;
        int mode = (omsi_bus && bus_drive_active) ? 0 : ai_follow_cam;
        const omsi::bus::BusDocument* doc = nullptr;
        if (mode != 0) {
            char part[1024]{};
            if (oo_traffic_car_part_file(ai_traffic, fi, 0, part, sizeof(part))) doc = ai_bus_doc(part);
            if (mode == 1 && (!doc || doc->driver_cameras.empty())) mode = 2;
            if (mode == 3 && (!doc || doc->passenger_cameras.empty())) mode = (doc && !doc->driver_cameras.empty()) ? 1 : 2;
        }
        if (mode == 0) {
            ai_follow_release_cam();
            camera.target_x = body16[12];
            camera.target_y = body16[13];
            camera.target_z = body16[14] + 2.f;
            camera.ground_z = camera.target_z;
            return;
        }
        XMFLOAT4X4 bm{};
        std::memcpy(&bm, body16, sizeof(bm));
        const XMMATRIX body = XMLoadFloat4x4(&bm);
        const auto to_world = [&](float lx, float ly, float lz) {
            XMFLOAT3 w{};
            XMStoreFloat3(&w, XMVector3TransformCoord(XMVectorSet(lx, ly, lz, 1.f), body));
            return w;
        };
        const auto dir_world = [&](float lx, float ly, float lz) {
            XMFLOAT3 w{};
            XMStoreFloat3(&w, XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(lx, ly, lz, 0.f), body)));
            return w;
        };
        if (!ai_follow_locked) ai_follow_snap = true;
        ai_follow_locked = true;
        camera.ground_z = body16[14];
        camera.z_up = true;
        camera.editor_nav = false;
        camera.orbit_target_locked = true;
        // (mouse look since the last frame stays an offset from the view's own direction)
        if (ai_follow_snap) {
            ai_fc_dth = 0.f;
            ai_fc_dph = 0.f;
        } else {
            ai_fc_dth += camera.theta - ai_fc_last_th;
            ai_fc_dph += camera.phi - ai_fc_last_ph;
        }
        const auto set_angles = [&](float base_th, float base_ph) {
            camera.theta = base_th + ai_fc_dth;
            camera.phi = std::clamp(base_ph + ai_fc_dph, 0.1f, 3.14159265f - 0.1f);
            ai_fc_dph = camera.phi - base_ph;
            ai_fc_last_th = camera.theta;
            ai_fc_last_ph = camera.phi;
        };
        if (mode == 1 || mode == 3) {
            const auto& list = mode == 1 ? doc->driver_cameras : doc->passenger_cameras;
            int& idx = mode == 1 ? ai_follow_drv_idx : ai_follow_pax_idx;
            if (idx < 0) idx = mode == 1 ? (std::max)(0, doc->std_driver_camera_index) : 0;
            if (idx >= static_cast<int>(list.size())) idx = 0;
            const auto& c = list[static_cast<std::size_t>(idx)];
            const float yaw = c.yaw_deg * 3.14159265f / 180.f, pitch = c.pitch_deg * 3.14159265f / 180.f;
            const auto f = dir_world(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch));
            const auto eye = to_world(c.pos_x, c.pos_y, c.pos_z);
            constexpr float kR = 0.05f;
            camera.radius = kR;
            camera.target_x = eye.x + f.x * kR;
            camera.target_y = eye.y + f.y * kR;
            camera.target_z = eye.z + f.z * kR;
            set_angles(std::atan2(-f.y, -f.x), std::acos(std::clamp(-f.z, -1.f, 1.f)));
            ai_follow_snap = false;
            return;
        }
        const auto ctr = doc ? to_world(0.f, doc->bbox_offset_y, doc->bbox_half_z) : to_world(0.f, 0.f, 1.5f);
        camera.target_x = ctr.x;
        camera.target_y = ctr.y;
        camera.target_z = ctr.z;
        const auto fwd = dir_world(0.f, 1.f, 0.f);
        if (ai_follow_snap) {
            camera.radius = doc ? (std::max)(doc->bbox_half_y * 2.f + 6.f, 10.f) : 12.f;
            ai_fc_last_ph = camera.phi = 1.2f;
            ai_fc_dph = 0.f;
        }
        set_angles(std::atan2(-fwd.y, -fwd.x), 1.2f); // turns with it
        ai_follow_snap = false;
    };
    // Herramientas > Buses IA (horario): the timetable buses under way on the whole map, Seguir / Conducir per bus
    auto ai_bus_list_window = [&]() {
        auto& stv = panel.imgui_state();
        if (!stv.show_ai_bus_list) return;
        ImGui::SetNextWindowSize(ImVec2(520.f, 420.f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Buses IA (horario)##ai_bus_list", &stv.show_ai_bus_list)) {
            ImGui::End();
            return;
        }
        if (!ai_traffic) ImGui::TextUnformatted("Activa Trafico IA (Vista) para ver los buses.");
        ImGui::Text("%d en servicio ahora, %d en el mapa", static_cast<int>(ai_bus_rows.size()),
                    static_cast<int>(ai_service_buses.size()));
        if (ai_follow_id) ai_follow_cam_buttons();
        else if (ai_pending_follow != static_cast<std::size_t>(-1))
            ImGui::TextDisabled("Esperando que aparezca el bus... (Esc cancela)");
        if (ImGui::BeginTable("##aibl", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("Linea");
            ImGui::TableSetupColumn("Destino");
            ImGui::TableSetupColumn("min");
            ImGui::TableSetupColumn("Estado");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < ai_bus_rows.size(); ++i) {
                const auto& r = ai_bus_rows[i];
                std::uint64_t id = 0;
                for (const auto& b : ai_service_buses)
                    if (b.trip == r.di) id = b.id;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.line.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.terminus.empty() ? r.trip.c_str() : r.terminus.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d/%d", r.rel_min, r.dur_min);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(id ? "en el mapa" : (r.has_pos ? "cerca" : "lejos"));
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                auto go_there = [&]() {
                    camera.target_x = r.x;
                    camera.target_y = r.y;
                    camera.reset_mouse();
                };
                if (ImGui::SmallButton("Seguir")) {
                    if (id) ai_follow_pick = id;
                    else {
                        go_there();
                        ai_pending_follow = r.di;
                        ai_pending_exact = r.has_pos;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Conducir")) {
                    if (id) ai_drive_request = id;
                    else {
                        go_there();
                        ai_pending_drive = r.di;
                        ai_pending_exact = r.has_pos;
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::End();
    };
    // Conducir: the AI bus is taken off and the same bus put there as the player's own
    auto ai_take_over = [&]() {
        if (!ai_traffic) return;
        if (ai_pending_drive != static_cast<std::size_t>(-1))
            for (const auto& b : ai_service_buses)
                if (b.trip == ai_pending_drive) {
                    ai_drive_request = b.id;
                    ai_pending_drive = static_cast<std::size_t>(-1);
                }
        if (!ai_drive_request) return;
        const std::uint64_t want = ai_drive_request;
        ai_drive_request = 0;
        const int nc = oo_traffic_car_count(ai_traffic);
        for (int i = 0; i < nc; ++i) {
            std::uint64_t cid = 0;
            int mc = 0;
            if (!oo_traffic_car(ai_traffic, i, &cid, &mc) || cid != want || mc <= 0) continue;
            char file[1024];
            float m16[16];
            if (!oo_traffic_car_part_file(ai_traffic, i, 0, file, sizeof(file)) || !oo_traffic_car_mesh_world(ai_traffic, i, 0, m16))
                return;
            const float heading = std::atan2(m16[4], m16[5]) * 57.2957795f;
            (void)oo_traffic_remove_car(ai_traffic, want);
            bus_spawn_at = std::array<float, 3>{m16[12], m16[13], heading};
            log::info(std::string("ai traffic: driving timetable bus ") + std::to_string(want) + " (" + file + ")");
            request_spawn_omsi_bus(std::string(file));
            return;
        }
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
        ai_service_frame(ex, ey, now);
        ai_follow_frame();
        ai_take_over();
        // the lights as the editor shows them (the crossing's or its [varparent]'s program, on the editor's
        // clock and time scale): the traffic ran a program of its own, out of step with the lamps on screen
        // (at x5 completely), and drove through what looked red
        for (const auto obj : ai_traffic_light_objs)
            for (int li = 0; li < 32; ++li) {
                const int ph = scenery_scripts.traffic_light_phase(obj, li);
                if (ph < 0 || !oo_traffic_light_set(ai_traffic, obj, li, ph)) break;
            }
        const float fx = camera.target_x - ex, fy = camera.target_y - ey;
        {
            int fbw = 1, fbh = 1;
            glfwGetFramebufferSize(window, &fbw, &fbh);
            oo_traffic_set_view(ai_traffic, camera.target_x - ex, camera.target_y - ey, camera.target_z - ez, 45.f,
                                fbh > 0 ? static_cast<float>(fbw) / static_cast<float>(fbh) : 1.78f);
        }
        oo_traffic_tick(ai_traffic, dt, ex, ey, ez, fx, fy, nullptr, panel.imgui_state().sim_hour);
        // the view cone for the pictures below: a car well out of it is posed every 6th frame only
        float vfx = camera.target_x - ex, vfy = camera.target_y - ey, vfz = camera.target_z - ez;
        {
            const float l = std::sqrt(vfx * vfx + vfy * vfy + vfz * vfz);
            if (l > 1e-5f) {
                vfx /= l;
                vfy /= l;
                vfz /= l;
            }
        }
        static std::uint32_t s_pose_frame = 0;
        ++s_pose_frame;
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
            // (out of the view - more than ~65 degrees off it and 40 m away: not drawn, posed every 6th frame, each
            //  car on its own beat; its first mesh tells where it is)
            if (!car.chunks.empty() && ((s_pose_frame + static_cast<std::uint32_t>(i)) % 6u) != 0u) {
                float p16[16];
                if (oo_traffic_car_mesh_world(ai_traffic, i, 0, p16)) {
                    const float dx = p16[12] - ex, dy = p16[13] - ey, dz = p16[14] - ez;
                    const float dl = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (dl > 40.f && (dx * vfx + dy * vfy + dz * vfz) < 0.42f * dl) continue;
                }
            }
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
        // Test (OMSI_AI_TEST=1): the camera over the nearest junction with lights, day shots every 30 s with the
        // stuck cars in the log; then a car selected (shots before / after: nothing else may vanish), then a
        // timetable bus followed from its driver's seat; closes the editor at the end. Shots in shots/aitest_*.bmp
        static const bool s_ai_test = std::getenv("OMSI_AI_TEST") != nullptr;
        if (s_ai_test) {
            static double s_t0 = -1.0, s_next = 0.0;
            static int s_step = 0;
            static float s_jx = 0.f, s_jy = 0.f, s_jz = 0.f;
            if (s_t0 < 0.0) s_t0 = now;
            const double t = now - s_t0;
            ui::set_hide_all_ui(s_step < 6);
            auto shot = [&](const std::string& name) {
                renderer.set_pending_screenshot(std::filesystem::path("shots") / ("aitest_" + name + ".bmp"));
                log::info("ai test: shot " + name);
            };
            auto stuck_report = [&](const char* tag) {
                const int nc = oo_traffic_car_count(ai_traffic);
                int n_near = 0, standing = 0, stuck = 0;
                int by_action[10]{};
                std::string lines;
                for (int i = 0; i < nc; ++i) {
                    std::uint64_t cid = 0;
                    int mc = 0;
                    float m16[16];
                    if (!oo_traffic_car(ai_traffic, i, &cid, &mc) || !oo_traffic_car_mesh_world(ai_traffic, i, 0, m16)) continue;
                    if (std::hypot(m16[12] - s_jx, m16[13] - s_jy) > 150.f) continue;
                    ++n_near;
                    OoCarState cs{};
                    if (!oo_traffic_car_state(ai_traffic, cid, &cs)) continue;
                    if (cs.action >= 0 && cs.action < 10) ++by_action[cs.action];
                    if (cs.speed_kmh < 1.f) ++standing;
                    if (cs.stopped_s > 45.f) {
                        ++stuck;
                        if (stuck <= 8)
                            lines += " [" + std::to_string(cid) + " act=" + std::to_string(cs.action) + " why=" + cs.why + " " +
                                     std::to_string(static_cast<int>(cs.why_dist)) + "m stood=" +
                                     std::to_string(static_cast<int>(cs.stopped_s)) + "s waits_on=" + std::to_string(cs.waits_on) + "]";
                    }
                }
                std::string acts;
                for (int a = 0; a < 10; ++a) acts += " " + std::to_string(by_action[a]);
                log::info(std::string("ai test: ") + tag + " cars within 150 m=" + std::to_string(n_near) + " standing=" +
                          std::to_string(standing) + " stood>45s=" + std::to_string(stuck) + " actions[0..9]=" + acts + lines);
            };
            if (s_step == 0 && t > 25.0 && !ai_traffic_light_objs.empty()) {
                // the crossing with lights nearest the view
                float best = 1e30f;
                for (const auto obj : ai_traffic_light_objs) {
                    const auto it = edit_obj_by_id.find(obj);
                    if (it == edit_obj_by_id.end() || !it->second) continue;
                    const float d = std::hypot(it->second->position.x - ex, it->second->position.y - ey);
                    if (d < best) {
                        best = d;
                        s_jx = it->second->position.x;
                        s_jy = it->second->position.y;
                        s_jz = it->second->position.z;
                    }
                }
                panel.imgui_state().sim_hour = 13.f;
                camera.target_x = s_jx;
                camera.target_y = s_jy;
                camera.target_z = s_jz + 1.f;
                camera.ground_z = camera.target_z;
                camera.radius = 70.f;
                camera.phi = 0.75f;
                camera.reset_mouse();
                log::info("ai test: junction at " + std::to_string(s_jx) + "," + std::to_string(s_jy) + " (" +
                          std::to_string(static_cast<int>(best)) + " m from the view)");
                s_step = 1;
                s_next = t + 30.0;
            } else if (s_step >= 1 && s_step <= 4 && t > s_next) {
                stuck_report(("junction " + std::to_string(s_step)).c_str());
                shot("junction_" + std::to_string(s_step));
                ++s_step;
                s_next = t + 30.0;
            } else if (s_step == 5 && t > s_next) {
                // item 1: a car selected; nothing else may vanish
                float best = 1e30f;
                std::uint64_t pick = 0;
                for (const auto& [cid, car] : ai_traffic_cars) {
                    for (const auto ch : car.chunks)
                        if (ch != kNoChunk) {
                            const auto r = static_cast<std::size_t>(ch);
                            (void)r;
                        }
                    const int nc = oo_traffic_car_count(ai_traffic);
                    for (int i = 0; i < nc; ++i) {
                        std::uint64_t id2 = 0;
                        int mc = 0;
                        float m16[16];
                        if (oo_traffic_car(ai_traffic, i, &id2, &mc) && id2 == cid && oo_traffic_car_mesh_world(ai_traffic, i, 0, m16)) {
                            const float d = std::hypot(m16[12] - s_jx, m16[13] - s_jy);
                            if (d < best) {
                                best = d;
                                pick = cid;
                            }
                        }
                    }
                }
                shot("select_before");
                ai_selected_id = pick;
                log::info("ai test: selected car " + std::to_string(pick));
                s_step = 6;
                s_next = t + 3.0;
            } else if (s_step == 6 && t > s_next) {
                shot("select_after");
                s_step = 7;
                s_next = t + 2.0;
            } else if (s_step == 7 && t > s_next) {
                ai_selected_id = 0;
                if (!ai_service_buses.empty()) {
                    ai_follow_start(ai_service_buses.front().id);
                    ai_follow_cam = 1;
                    ai_follow_snap = true;
                }
                s_step = 8;
                s_next = t + 6.0;
            } else if (s_step == 8 && t > s_next) {
                shot(ai_follow_id ? "bus_driver" : "bus_none");
                s_step = 9;
                s_next = t + 2.0;
            } else if (s_step == 9 && t > s_next) {
                ai_follow_stop();
                glfwSetWindowShouldClose(window, GLFW_TRUE);
                s_step = 10;
            }
        }
    };
