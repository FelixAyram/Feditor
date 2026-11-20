    // ===== Parked cars: a car of the map's parklist on every [carpark_p] space =================================
    // As OMSI / openOMSI (omsi-scene stage.rs placed_type): the space's first string picks the list
    // (parklist_p.txt, parklist_p_<n>.txt in the map folder), a hash of the object id picks the car and leaves
    // a quarter of the spaces empty. Cars go up a few a frame around the view and come down far from it; every
    // car of a type is a clone of the first (shared vertex buffers). Vista > Autos estacionados, or
    // OMSI_PARKED=0/1.
    struct ParkedCar {
        std::vector<std::size_t> chunks;
        std::vector<int> lod; // per chunk: its [LOD] band (0 near, 1+ far)
        int shown_lod = -1;
        // its paint: the [CTC] scheme picked for it, (freetex key "ctc:<default texture>", file) - shown once
        // every chunk has it up (a car that came up in its placeholder colours first and changed later)
        std::vector<std::pair<std::string, std::filesystem::path>> paint;
        bool painted = true;
        double placed_t = 0.0;
        float x = 0.f, y = 0.f;
    };
    std::unordered_map<std::int32_t, ParkedCar> parked_cars;       // carpark object id -> its car
    std::unordered_map<std::int32_t, std::array<float, 2>> parked_none; // spaces left empty (and where)
    std::unordered_map<int, std::vector<std::string>> parked_lists;
    // spaces whose car's meshes were not prepared yet (the vertex cache fills on a worker): (next try, tries)
    std::unordered_map<std::int32_t, std::pair<double, int>> parked_retry;
    std::size_t parked_scan_i = 0;
    // per object id: 1 a parking space, 2 not (looking its .sco up for every object near the view, every
    // frame, was 3-4 ms a frame)
    std::unordered_map<std::int32_t, std::uint8_t> parked_kind;
    double parked_scan_t = -1.0, parked_lod_t = -1.0;
    double parked_prune_t = 0.0, parked_log_t = 0.0;
    constexpr float kParkedNear = 450.f, kParkedFar = 600.f;

    auto parked_list = [&](int index) -> const std::vector<std::string>& {
        auto [it, fresh] = parked_lists.try_emplace(index);
        if (!fresh) return it->second;
        const std::string name = index == 0 ? std::string("parklist_p.txt") : "parklist_p_" + std::to_string(index) + ".txt";
        std::ifstream f(cfg.global.root / name, std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            std::size_t a = 0;
            while (a < line.size() && (line[a] == ' ' || line[a] == '\t' || static_cast<unsigned char>(line[a]) >= 0xEF)) ++a;
            line.erase(0, a);
            if (line.size() < 5) continue;
            std::string low = line.substr(line.size() - 4);
            for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (low == ".sco") it->second.push_back(line);
        }
        log::info("parked cars: " + name + " " + std::to_string(it->second.size()) + " car types");
        return it->second;
    };
    auto parked_release = [&](ParkedCar& pc) {
        for (const auto ch : pc.chunks) renderer.release_chunk_gpu(ch);
        pc.chunks.clear();
    };
    auto parked_release_all = [&]() {
        for (auto& [id, pc] : parked_cars) parked_release(pc);
        parked_cars.clear();
        parked_none.clear();
        parked_retry.clear();
    };
    // the car for space `o`: 1 placed, 0 the space stays empty, -1 try again shortly
    auto parked_place = [&](const map::MapObjectInst& o, const sco::SceneryObject& space) -> int {
        int index = 0;
        if (!o.strings.empty()) {
            try {
                index = (std::max)(0, std::stoi(o.strings[0]));
            } catch (...) {
            }
        }
        const auto& list = parked_list(index);
        if (list.empty()) return 0;
        const std::uint64_t h = (static_cast<std::uint64_t>(static_cast<std::int64_t>(o.id)) * 0x9E3779B97F4A7C15ull) >> 33;
        if (h % 4 == 0) return 0;
        const auto car_path = cfg.omsi_root / std::filesystem::u8path(list[(h / 4) % list.size()]);
        const sco::SceneryObject* car = cached_sco(car_path);
        if (!car || car->meshes.empty()) return 0;
        const bool abs_z = sco::skip_terrain_add(space);
        // (a carpark on a [splineAttachement]: its Z is the spline's, absolute already - as the scenery places it,
        //  sco_world_matrix(..., 0) in mod_spline_attach_spawn; with the terrain added again the car floated)
        const float th = spline_attach_spawn_ids.count(o.id)
                             ? 0.f
                             : terrain_add(abs_z, o.position.x, o.position.y, o.position.z, sco_ground_layer(o, space));
        const XMMATRIX world_m = sco_world_matrix(o, nullptr, th, 0.f, 0.f);
        ParkedCar pc;
        pc.placed_t = glfwGetTime();
        pc.x = o.position.x;
        pc.y = o.position.y;
        for (const auto& mr : car->meshes) {
            // its [LOD] bands: the near one up close, the next one (a few hundred triangles) further off
            if (!mr.is_o3d || mr.lod_index > 1) continue;
            const auto stem = mr.mesh_file.stem().string();
            if (stem == "empty_lod" || stem == "empty") continue;
            const auto mapped = o3d::load_mapped_shared(mr.mesh_file);
            if (!mapped || !mapped->valid()) continue;
            auto matl_store = sco_matl_cfg(*car, mr.mesh_file);
            const auto chunk = append_or_clone_o3d(*mapped, mr.mesh_file, &matl_store, car_path);
            if (!chunk) continue;
            const XMMATRIX mesh_world = ma4_to_xm(o3d::o3d_unity_root_transform(mapped->mesh().object_rows())) * world_m;
            renderer.set_chunk_world_matrix(*chunk, store_world_transposed(mesh_world).data());
            renderer.set_chunk_size_cull_scale(*chunk, 5.f); // (small parts of far cars not drawn)
            // (drawn as the scenery object it is: instanced with every other car of its type)
            renderer.set_chunk_visible(*chunk, false);
            pc.chunks.push_back(*chunk);
            pc.lod.push_back(mr.lod_index);
        }
        {
            // (diagnostics: per car type once, its meshes as the renderer has them)
            static std::unordered_set<std::string> s_logged;
            const auto key = car_path.u8string();
            if (s_logged.size() < 40 && s_logged.insert(std::string(key.begin(), key.end())).second) {
                std::size_t lod0 = 0;
                for (const auto& mr : car->meshes) lod0 += mr.lod_index == 0;
                log::info("parked cars: DIAG " + car_path.filename().string() + ": " + std::to_string(car->meshes.size()) +
                          " meshes (" + std::to_string(lod0) + " in LOD 0), " + std::to_string(pc.chunks.size()) + " up");
                for (const auto& mr : car->meshes)
                    log::info("parked cars: DIAG   mesh " + mr.mesh_file.filename().string() + " lod " +
                              std::to_string(mr.lod_index) + (mr.is_o3d ? "" : " (not o3d)"));
                for (const auto ch : pc.chunks) log::info("parked cars: DIAG   " + renderer.chunk_debug_line(ch));
            }
        }
        if (pc.chunks.empty()) return -1;
        // a random .cti scheme per parked car, as Omsi.exe gives each one (openOMSI stage.rs placed_type:
        // the scheme from the same hash, (h / 64) % schemes), on all its chunks - near band and LOD alike
        if (!car->ctc.empty() && !car->ctc.front().schemes.empty()) {
            const auto& schemes = car->ctc.front().schemes;
            const auto& sc = schemes[static_cast<std::size_t>((h / 64) % schemes.size())];
            auto ieq_s = [](const std::string& a, const std::string& b) {
                if (a.size() != b.size()) return false;
                for (std::size_t i = 0; i < a.size(); ++i)
                    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                        return false;
                return true;
            };
            for (const auto& [ctc_name, file] : sc.textures)
                for (const auto& [name, def] : car->ctc_textures) {
                    if (!ieq_s(name, ctc_name)) continue;
                    std::string key = def;
                    for (char& c : key) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    // the scheme folder's picture (its <stem>.dds first, as the loader takes it), else the model's own
                    std::filesystem::path f = sc.dir / std::filesystem::u8path(file);
                    std::error_code ec;
                    const auto dds = f.parent_path() / (f.stem().wstring() + L".dds");
                    if (std::filesystem::exists(dds, ec)) f = dds;
                    else if (!std::filesystem::exists(f, ec)) {
                        if (const auto r = gfx::dx12::resolve_texture_path(car_path, file, cfg.omsi_root)) f = *r;
                        else continue;
                    }
                    pc.paint.emplace_back("ctc:" + key, f);
                }
            pc.painted = pc.paint.empty();
        }
        parked_cars.emplace(o.id, std::move(pc));
        return 1;
    };
    // a click on a parked car: its carpark object (edit_objects index), -1 when no car is under the ray
    parked_pick_hook = [&](float mx, float my, float fw, float fh, const float* wvp, float* t_out) -> int {
        static std::vector<std::size_t> s_chunks;
        s_chunks.clear();
        for (const auto& [oid, pc] : parked_cars)
            for (const auto ch : pc.chunks) s_chunks.push_back(ch);
        if (s_chunks.empty()) return -1;
        float t = 1e30f;
        const auto hit = renderer.pick_chunk(mx, my, static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh), wvp,
                                             false, s_chunks.data(), s_chunks.size(), &t);
        if (!hit) return -1;
        for (const auto& [oid, pc] : parked_cars) {
            if (std::find(pc.chunks.begin(), pc.chunks.end(), *hit) == pc.chunks.end()) continue;
            for (std::size_t i = 0; i < edit_objects.size(); ++i)
                if (edit_objects[i].id == oid) {
                    if (t_out) *t_out = t;
                    return static_cast<int>(i);
                }
        }
        return -1;
    };
    auto parked_frame = [&]() {
        static const int env = [] {
            const char* e = std::getenv("OMSI_PARKED");
            return e && *e ? std::atoi(e) : -1;
        }();
        // one option, "Car park real": the real parked cars, or (off) the spaces' plain helper boxes
        const bool on = env >= 0 ? env > 0 : panel.imgui_state().parked_cars_on;
        renderer.set_show_editor_helpers(!on);
        if (!on) {
            if (!parked_cars.empty() || !parked_none.empty()) parked_release_all();
            return;
        }
        float ex = 0.f, ey = 0.f, ez = 0.f;
        camera.eye_position(ex, ey, ez);
        const auto t0 = std::chrono::steady_clock::now();
        auto spent = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
        // walk the object list a slice a frame: spaces near the view get their car
        const double now = glfwGetTime();
        const std::size_t n = edit_objects.size();
        if (n > 0 && now - parked_scan_t > 0.2) {
            parked_scan_t = now;
            if (parked_scan_i >= n) parked_scan_i = 0;
            int placed = 0;
            for (std::size_t k = 0; k < 20000 && k < n; ++k) {
                const auto& o = edit_objects[parked_scan_i];
                parked_scan_i = (parked_scan_i + 1) % n;
                const float dx = o.position.x - ex, dy = o.position.y - ey;
                if (dx * dx + dy * dy > kParkedNear * kParkedNear) continue;
                if (parked_cars.count(o.id) || parked_none.count(o.id)) continue;
                const auto rt = parked_retry.find(o.id);
                if (rt != parked_retry.end() && now < rt->second.first) continue;
                auto& kind = parked_kind[o.id];
                if (kind == 2) continue;
                if (editor_deleted_object_ids.count(o.id)) continue;
                const sco::SceneryObject* space = cached_sco(o.sco_path);
                kind = (space && space->special == sco::ScoSpecial::Carpark) ? 1 : 2;
                if (kind == 2) continue;
                const int r = parked_place(o, *space);
                if (r == -1 && (rt == parked_retry.end() || rt->second.second < 8)) {
                    auto& e = parked_retry[o.id];
                    e.first = now + 1.5;
                    ++e.second;
                } else {
                    if (r != 1) parked_none.emplace(o.id, std::array<float, 2>{o.position.x, o.position.y});
                    if (rt != parked_retry.end()) parked_retry.erase(rt);
                }
                if (++placed >= 3 || spent() > 2.5) break;
            }
        }
        // far ones come down (and a space far away is looked at again when the view comes back)
        if (parked_retry.size() > 4096) parked_retry.clear();
        if (parked_kind.size() > 400000) parked_kind.clear();
        // the [LOD] band by distance (a car at 50 m is a few pixels of detail; the far band is ~1/20 the triangles)
        const bool lod_now = now - parked_lod_t > 0.2;
        if (lod_now) parked_lod_t = now;
        for (auto& [id, pc] : parked_cars) {
            if (!lod_now) break;
            const float dx = pc.x - ex, dy = pc.y - ey;
            bool has_far = false;
            for (const int l : pc.lod) has_far |= l == 1;
            const int want = (has_far && dx * dx + dy * dy > 55.f * 55.f) ? 1 : 0;
            if (!pc.painted) {
                // (its paint goes up on a worker: set it on every chunk until all show it; 4 s at most)
                bool all = true;
                for (const auto ch : pc.chunks)
                    for (const auto& [key, file] : pc.paint) {
                        (void)renderer.set_chunk_freetex_variant(ch, key, file);
                        all = all && renderer.chunk_freetex_variant_ready(ch, key, file);
                    }
                pc.painted = all || now - pc.placed_t > 4.0;
                if (!pc.painted) continue;
                pc.shown_lod = -1;
            }
            if (want == pc.shown_lod) continue;
            pc.shown_lod = want;
            for (std::size_t k = 0; k < pc.chunks.size(); ++k) renderer.set_chunk_visible(pc.chunks[k], pc.lod[k] == want);
        }
        if (now - parked_prune_t > 1.0) {
            parked_prune_t = now;
            for (auto it = parked_cars.begin(); it != parked_cars.end();) {
                if (std::hypot(it->second.x - ex, it->second.y - ey) > kParkedFar) {
                    parked_release(it->second);
                    it = parked_cars.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = parked_none.begin(); it != parked_none.end();)
                it = std::hypot(it->second[0] - ex, it->second[1] - ey) > kParkedFar ? parked_none.erase(it) : std::next(it);
        }
        if (now - parked_log_t > 10.0 && !parked_cars.empty()) {
            parked_log_t = now;
            std::size_t chunks = 0;
            std::size_t waiting = 0, painted_n = 0;
            for (const auto& [id, pc] : parked_cars) {
                chunks += pc.chunks.size();
                waiting += !pc.painted;
                painted_n += !pc.paint.empty();
            }
            log::info("parked cars: " + std::to_string(parked_cars.size()) + " cars (" + std::to_string(chunks) +
                      " meshes), " + std::to_string(parked_none.size()) + " empty spaces around the view, " +
                      std::to_string(painted_n) + " in a paint scheme (" + std::to_string(waiting) + " waiting for it)");
        }
    };
