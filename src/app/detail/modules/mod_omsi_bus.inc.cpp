// MOD omsi_bus — spawn and drive a real OMSI 2 bus in the map editor (lives in main() until the loop).
// Replaces the GTA Vice City car (mod_vc_car). The bus is the engine's own OMSI stack:
//   - .bus + model.cfg + .o3d meshes      -> renderer chunks, model.cfg animator (doors, wheels, steering)
//   - OSC scripts, sounds, Axle_* vars    -> omsi::script::VehicleRuntime (same runtime as the viewer)
//   - keyboard.cfg [vehicles] bindings    -> omsi::inputs::VehicleInputBindings
//   - ground follow                       -> omsi::world_height over the editor terrain, splines and objects
// Simulation: openOMSI itself (ffi/omsi_vehicle_ffi, include/omsi/vehicle/oo_vehicle.h) - its scripts,
// rigid body, gearbox, sound set and auto-start, unmodified. The editor feeds it ground heights,
// obstacles, keys and the camera, and renders its pose. Without the DLL (or OMSI_CPP_BUS=1) the older
// C++ path (VehicleRuntime + BusBodyPhysics) is used.

    std::vector<omsi::bus_catalog::BusEntry> omsi_bus_catalog;
    // Scanning Vehicles/ reads every .bus (1000+ on a modded install): ~30 s that used to block the editor before its
    // first frame. It runs on a background worker; the picker shows the list once omsi_bus_catalog_pump() swaps it in.
    struct BusCatalogJob {
        std::mutex mu;
        bool done = false;
        std::vector<omsi::bus_catalog::BusEntry> result;
    };
    auto omsi_bus_catalog_job = std::make_shared<BusCatalogJob>();
    auto omsi_bus_catalog_pump = [&]() {
        if (!omsi_bus_catalog_job) return;
        std::lock_guard<std::mutex> lk(omsi_bus_catalog_job->mu);
        if (!omsi_bus_catalog_job->done) return;
        omsi_bus_catalog = std::move(omsi_bus_catalog_job->result);
        log::info("omsi bus catalog n=" + std::to_string(omsi_bus_catalog.size()) + " (background scan)");
        omsi_bus_catalog_job.reset();
    };
    if (!cfg.omsi_root.empty()) {
        omsi::JobSystem::global().submit_background([job = omsi_bus_catalog_job, root = cfg.omsi_root]() {
            std::vector<omsi::bus_catalog::BusEntry> r;
            try {
                r = omsi::bus_catalog::scan_vehicles(root);
            } catch (const std::exception& ex) {
                log::warn(std::string("omsi bus catalog scan failed: ") + ex.what());
            } catch (...) {
                log::warn("omsi bus catalog scan failed");
            }
            std::lock_guard<std::mutex> lk(job->mu);
            job->result = std::move(r);
            job->done = true;
        });
    } else {
        omsi_bus_catalog_job.reset();
        log::warn("omsi bus: OMSI 2 root not configured; no vehicles to list");
    }

    constexpr std::size_t kNoChunk = static_cast<std::size_t>(-1);

    // One spawned bus. Heap-allocated: VehicleRuntime owns a thread and a mutex (not movable), and
    // MeshAnimator keeps pointers into `cfg`.
    OoTraffic* ai_traffic = nullptr; // the AI road traffic (mod_ai_traffic): the bus keeps its distance to it
    struct VehMatlChange {
        std::string slot_key;  // "mc:<tex>#<occurrence>" as mesh.cpp names the slot
        std::string var;
        std::filesystem::path on_path;
        std::filesystem::path o3d_path;
        int target = 0; // 0: the texture swaps to on_path; 2 / 3: item 1's night / light map at the night slot
    };
    // a [useTextTexture] slot of the bus: its [texttexture] / [texttexture_enh] drawn with the script's string
    struct BusTextBake {
        std::atomic<bool> done{false};
        std::string text;
        std::optional<sco::TextTextureImage> img;
    };
    struct BusTextTex {
        std::size_t chunk = 0;
        int ti = 0;
        sco::ScoTextTexture tt;
        std::string field;
        std::string shown;
        bool ever = false;
        std::shared_ptr<BusTextBake> job;
    };
    struct OmsiBusInst {
        std::string bus_file;
        omsi::bus::BusDocument doc;
        omsi::model_cfg::ModelCfgDocument model;
        std::unique_ptr<omsi::script::VehicleRuntime> rt;
        omsi::model_cfg::MeshAnimator anim;
        std::vector<std::size_t> chunks; // per [mesh] of the primary LOD; kNoChunk = not on the GPU
        std::vector<bool> shadow;        // per mesh: a shadow blob, laid on the road (viewer rule)
        float x = 0.f, y = 0.f, z = 0.f; // editor space; z = the ground under the wheels
        float heading_deg = 0.f;         // OMSI heading: forward = (sin h, cos h), clockwise
        float pitch_deg = 0.f, roll_deg = 0.f;
        omsi::vehicle::BusBodyPhysics body; // rigid body: springs, pitch/roll, tyre grip, yaw
        float ground_z = 0.f;               // mean road height under the wheels (shadow blob)
        // openOMSI simulation (null: the C++ fallback above drives the bus)
        OoBus* oo = nullptr;
        std::unordered_map<std::string, int> oo_slots;
        std::function<float(float, float, float)> ground_fn;
        float oo_ground_hint = 0.f;
        float oo_body[16]{};
        // openOMSI's tyres ask the ground hundreds of times a step; the editor's height query scans
        // splines and objects, so heights are cached on a 0.25 m grid and interpolated.
        std::unordered_map<std::int64_t, std::pair<float, std::uint32_t>> ground_cache; // height, surface_gen
        float oo_time_acc = 0.f;            // real time not simulated yet
        float obst_x = 1e9f, obst_y = 1e9f; // where the obstacles were last gathered
        float obst_h = 0.f, obst_age = 0.f;
        float age = 0.f;             // seconds since spawn
        bool waiting_stream = true;  // held still until the map around it has streamed in
        bool session_checked = false; // the app's Windows audio session put to 100 % once sound plays
        std::vector<int> oo_mesh; // per LOD mesh (our chunks): openOMSI's mesh index, -1 = none
        // coupled sections (articulated rear sections, trailers): their meshes' chunks and openOMSI
        // mesh indices, and their model.cfg's kept for the materials
        std::vector<std::size_t> sec_chunks;
        std::vector<int> sec_oo;
        std::vector<int> sec_viewpoint; // per section chunk: its mesh's [viewpoint]
        std::vector<char> sec_skin;     // per section chunk: a [smoothskin] mesh (the bellows)
        std::vector<omsi::model_cfg::ModelCfgDocument> sec_models;
        // [matl_change] slots of its chunks (main + sections) and what they show
        std::vector<std::size_t> mc_chunk;
        std::vector<std::vector<VehMatlChange>> mc_changes;
        std::vector<std::vector<signed char>> mc_state;
        // [CTC] paint still to show: (chunk, "ctc:<default>" key, scheme file) until its texture is up
        std::vector<std::tuple<std::size_t, std::string, std::filesystem::path>> paint;
        std::vector<BusTextTex> texttex; // its displays (destination, side signs, passenger info)
        float texttex_t = 0.f;
        OmsiBusInst() = default;
        OmsiBusInst(const OmsiBusInst&) = delete;
        OmsiBusInst& operator=(const OmsiBusInst&) = delete;
        ~OmsiBusInst() {
            if (oo) oo_despawn(oo);
        }
    };
    std::unique_ptr<OmsiBusInst> omsi_bus;
    bool bus_drive_active = false;
    bool bus_cam_need_snap = true;
    // OMSI views: 1 driver ([add_camera_driver], F1 again = next), 2 outside (orbits the bbox centre
    // and turns with the bus, F2 again = reset), 3 passenger ([add_camera_pax], F3 again = next),
    // 4 free orbit around the bbox centre.
    int bus_cam_mode = 1;
    int bus_cam_driver_idx = 0;
    int bus_cam_pax_idx = 0;
    // mouse look kept as an offset from the camera's base direction (rides with the bus)
    float bus_cam_look_dth = 0.f, bus_cam_look_dph = 0.f, bus_cam_last_th = 0.f, bus_cam_last_ph = 0.f;
    bool bus_f1_was = false;
    bool bus_f2_was = false;
    bool bus_f3_was = false;
    bool bus_f4_was = false;
    omsi::inputs::VehicleInputBindings bus_inputs{};
    if (!cfg.omsi_root.empty()) bus_inputs.load_from_omsi_root(cfg.omsi_root);

    // openOMSI DLL: opened on the first spawn (it opens the audio device and the OMSI 2 content).
    int oo_state = 0; // 0 not tried, 1 ready, -1 unavailable
    auto oo_ready = [&]() -> bool {
        if (oo_state != 0) return oo_state > 0;
        oo_state = -1;
        if (const char* e = std::getenv("OMSI_CPP_BUS"); e && *e == '1') {
            log::info("omsi bus: OMSI_CPP_BUS=1, using the C++ vehicle runtime");
            return false;
        }
#ifdef _WIN32
        // delay-loaded DLL: make sure it is next to the exe before the first call (a missing delay-load
        // is an SEH exception, not something C++ can catch)
        if (!LoadLibraryW(L"omsi_vehicle_ffi.dll")) {
            log::warn("omsi bus: omsi_vehicle_ffi.dll not found - using the C++ vehicle runtime");
            return false;
        }
#endif
        const auto u8 = cfg.omsi_root.u8string();
        const std::string root(u8.begin(), u8.end());
        const OoLogFn logfn = +[](int32_t level, const char* msg) {
            if (level >= 2) log::error(msg);
            else if (level == 1) log::warn(msg);
            else log::info(msg);
        };
        try {
            if (oo_init(root.c_str(), logfn) == 1) oo_state = 1;
        } catch (...) {
        }
        log::info(std::string("omsi bus: openOMSI vehicle simulation ") + (oo_state > 0 ? "ready" : "NOT available"));
        if (oo_state > 0) (void)omsi::sound::restore_app_session_volume();
        return oo_state > 0;
    };
    // Script variable of the bus (openOMSI slot cache, or the C++ runtime).
    auto bus_var = [&](OmsiBusInst& b, const std::string& name, float def) -> float {
        if (b.oo) {
            auto it = b.oo_slots.find(name);
            if (it == b.oo_slots.end()) it = b.oo_slots.emplace(name, oo_var_slot(b.oo, name.c_str())).first;
            return it->second >= 0 ? oo_var_at(b.oo, it->second) : def;
        }
        return b.rt ? b.rt->public_float(name, def) : def;
    };
    // A keyboard.cfg action (trigger name without _off) pressed or released.
    auto bus_key = [&](OmsiBusInst& b, const std::string& action, bool pressed) {
        if (b.oo) {
            oo_key(b.oo, action.c_str(), pressed ? 1 : 0);
        } else if (b.rt) {
            b.rt->apply_trigger(pressed ? action : action + "_off");
        }
    };

    // What the bus stands on, as OMSI / openOMSI give it: the terrain, the roads' height profiles
    // and the faces of [surface] objects (crossings, squares, depots) - not the boxes or collision
    // meshes of other objects (world_height's flat plates at a box's top: false steps and plateaus
    // the tyres met as walls or floated on).
    struct SurfaceShape {
        std::vector<std::array<float, 9>> tris; // object frame
        std::unordered_map<std::int64_t, std::vector<std::uint32_t>> grid; // 4 m cells
        float reach = 0.f; // farthest face corner from the object origin (m, XY)
    };
    std::unordered_map<std::wstring, std::shared_ptr<const SurfaceShape>> surface_shapes;
    auto surface_shape = [&](const map::MapObjectInst& o, const sco::SceneryObject& sco) -> std::shared_ptr<const SurfaceShape> {
        const std::wstring key = o.sco_path.wstring();
        if (const auto it = surface_shapes.find(key); it != surface_shapes.end()) return it->second;
        auto shape = std::make_shared<SurfaceShape>();
        // Omsi.exe's ground query (0x7a0814, as openOMSI cut_terrain.rs reads it) casts down only into the
        // FIRST [mesh] of a [surface] object (0x5f9218 with mesh 0); its other meshes (kerbs, buildings,
        // lamps standing on the yard) and its [collision_mesh] are no ground
        for (const auto& mr : sco.meshes) {
            if (&mr != &sco.meshes.front()) break;
            if (!mr.is_o3d) continue;
            const auto path = sco::resolve_sco_o3d(o.sco_path.parent_path(), omsi::path_text(mr.mesh_file));
            const auto mapped = o3d::load_mapped_shared(path);
            if (!mapped || !mapped->valid()) continue;
            const auto& mesh = mapped->mesh();
            std::vector<o3d::Vertex> verts;
            o3d::prepare_o3d_mesh_vertices_for_gpu(mesh, verts);
            const auto add = [&](std::uint32_t a, std::uint32_t b2, std::uint32_t c) {
                if (a >= verts.size() || b2 >= verts.size() || c >= verts.size()) return;
                std::array<float, 9> t{};
                for (int k = 0; k < 3; ++k) {
                    t[k] = verts[a].position[k];
                    t[3 + k] = verts[b2].position[k];
                    t[6 + k] = verts[c].position[k];
                }
                // only faces a wheel can stand on (normal up within ~60 deg)
                const float ux = t[3] - t[0], uy = t[4] - t[1], uz = t[5] - t[2];
                const float vx = t[6] - t[0], vy = t[7] - t[1], vz = t[8] - t[2];
                const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (nl < 1e-8f || std::fabs(nz) / nl < 0.5f) return;
                const auto idx = static_cast<std::uint32_t>(shape->tris.size());
                shape->tris.push_back(t);
                for (int k = 0; k < 3; ++k)
                    shape->reach = (std::max)(shape->reach, std::hypot(t[3 * k], t[3 * k + 1]));
                const float x0 = (std::min)({t[0], t[3], t[6]}), x1 = (std::max)({t[0], t[3], t[6]});
                const float y0 = (std::min)({t[1], t[4], t[7]}), y1 = (std::max)({t[1], t[4], t[7]});
                for (int gy = static_cast<int>(std::floor(y0 / 4.f)); gy <= static_cast<int>(std::floor(y1 / 4.f)); ++gy)
                    for (int gx = static_cast<int>(std::floor(x0 / 4.f)); gx <= static_cast<int>(std::floor(x1 / 4.f)); ++gx)
                        shape->grid[(static_cast<std::int64_t>(gx) << 32) ^ static_cast<std::uint32_t>(gy)].push_back(idx);
            };
            if (mesh.long_triangle_indices)
                for (const auto& t : mesh.triangle_u32_span()) add(t.i0, t.i1, t.i2);
            else
                for (const auto& t : mesh.triangle_u16_span()) add(t.i0, t.i1, t.i2);
        }
        surface_shapes.emplace(key, shape);
        return shape;
    };
    struct SurfaceInst {
        std::shared_ptr<const SurfaceShape> shape;
        float x = 0.f, y = 0.f, z = 0.f, c = 1.f, s = 0.f;
    };
    // Every [surface] object of the loaded tiles, in 32 m cells (the bus AND the AI cars stand on them).
    // Gathered only within 120 m of the player's bus before, the AI cars anywhere else - and all of them
    // with no bus out - drove through the crossings' and squares' plates on the terrain under them.
    std::vector<SurfaceInst> bus_surfaces;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> surface_cells;
    std::uint32_t surface_gen = 1; // bumped when the set changes: the ground caches start again
    std::size_t surface_sig = 0, surface_nobj = ~std::size_t{0};
    const void* surface_obj_data = nullptr;
    double surface_scan_t = -1.0;
    constexpr float kSurfCell = 32.f;
    auto surf_cell_key = [](int cx, int cy) {
        return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cy);
    };
    // Changed areas (a surface came or went), so the ground caches redo only the cells under them: cleared
    // whole on every change while tiles streamed in, the AI cars' 0.5 m height cache was empty every
    // half second and each wheel went through the slow all-splines query (ai_traffic 5-12 ms a frame).
    struct SurfDirty {
        std::uint32_t gen = 0;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    };
    std::vector<SurfDirty> surface_dirty;
    std::uint32_t surface_full_gen = 0; // cache entries made before this generation are all stale
    // a cached height made under generation `gen` at (x, y) still holds
    auto surface_cache_valid = [&](std::uint32_t gen, float x, float y) -> bool {
        if (gen >= surface_gen) return true;
        if (gen < surface_full_gen) return false;
        for (const auto& r : surface_dirty)
            if (r.gen > gen && x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) return false;
        return true;
    };
    // The scan walks edit_objects a slice a frame (all ~80k objects with a load_sco lookup each at once
    // was a 10 ms frame every half second while streaming) and swaps the set in when it went through.
    std::vector<SurfaceInst> surface_scan;
    std::vector<std::uint64_t> surface_scan_keys;
    std::unordered_map<std::uint64_t, std::array<float, 4>> surface_keys_now; // key -> rect of the current set
    std::size_t surface_scan_i = 0, surface_scan_n = 0;
    const void* surface_scan_data = nullptr;
    bool surface_scanning = false;
    auto refresh_surface_index = [&]() {
        if (!surface_scanning) {
            if (edit_objects.size() == surface_nobj && edit_objects.data() == surface_obj_data) return;
            const double now = glfwGetTime();
            if (now - surface_scan_t < 0.5) return; // (tiles streaming in: at most twice a second)
            surface_scan_t = now;
            surface_scanning = true;
            surface_scan_i = 0;
            surface_scan.clear();
            surface_scan_keys.clear();
            surface_scan_n = edit_objects.size();
            surface_scan_data = edit_objects.data();
        }
        if (edit_objects.size() != surface_scan_n || edit_objects.data() != surface_scan_data) {
            surface_scan_i = 0; // (the list changed under the scan: from the start)
            surface_scan.clear();
            surface_scan_keys.clear();
            surface_scan_n = edit_objects.size();
            surface_scan_data = edit_objects.data();
        }
        const std::size_t end = (std::min)(surface_scan_n, surface_scan_i + 4000);
        for (; surface_scan_i < end; ++surface_scan_i) {
            const auto& o = edit_objects[surface_scan_i];
            const auto* sco = sco::load_sco(o.sco_path);
            if (!sco || !sco->surface || sco->only_editor_mesh || sco->meshes.empty()) continue;
            const float tz = sco::skip_terrain_add(*sco) ? 0.f : sample_terrain_editor_xy(o.position.x, o.position.y);
            const float yaw = o.rotation_deg.x * 3.14159265f / 180.f;
            auto shape = surface_shape(o, *sco);
            if (!shape || shape->tris.empty()) continue;
            surface_scan.push_back(SurfaceInst{std::move(shape), o.position.x, o.position.y, o.position.z + tz,
                                               std::cos(yaw), std::sin(yaw)});
            const auto pos_hash = static_cast<std::uint32_t>(std::lround(o.position.x * 10.f) * 73856093L ^
                                                             std::lround(o.position.y * 10.f) * 19349663L);
            surface_scan_keys.push_back((static_cast<std::uint64_t>(static_cast<std::uint32_t>(o.id)) << 32) ^ pos_hash);
        }
        if (surface_scan_i < surface_scan_n) return; // (more next frame)
        surface_scanning = false;
        surface_nobj = surface_scan_n;
        surface_obj_data = surface_scan_data;
        // what came and what went
        std::unordered_map<std::uint64_t, std::array<float, 4>> keys_new;
        keys_new.reserve(surface_scan.size());
        std::vector<std::array<float, 4>> changed;
        for (std::size_t i = 0; i < surface_scan.size(); ++i) {
            const auto& si = surface_scan[i];
            const float r = si.shape->reach + 0.5f;
            const std::array<float, 4> rect{si.x - r, si.y - r, si.x + r, si.y + r};
            keys_new.emplace(surface_scan_keys[i], rect);
            if (!surface_keys_now.count(surface_scan_keys[i])) changed.push_back(rect);
        }
        for (const auto& [k, rect] : surface_keys_now)
            if (!keys_new.count(k)) changed.push_back(rect);
        if (changed.empty()) return;
        surface_keys_now = std::move(keys_new);
        bus_surfaces = std::move(surface_scan);
        surface_scan = {};
        surface_cells.clear();
        for (std::uint32_t i = 0; i < bus_surfaces.size(); ++i) {
            const auto& si = bus_surfaces[i];
            const float r = si.shape->reach + 0.5f;
            const int x0 = static_cast<int>(std::floor((si.x - r) / kSurfCell)), x1 = static_cast<int>(std::floor((si.x + r) / kSurfCell));
            const int y0 = static_cast<int>(std::floor((si.y - r) / kSurfCell)), y1 = static_cast<int>(std::floor((si.y + r) / kSurfCell));
            for (int cy = y0; cy <= y1; ++cy)
                for (int cx = x0; cx <= x1; ++cx) surface_cells[surf_cell_key(cx, cy)].push_back(i);
        }
        ++surface_gen;
        if (surface_dirty.size() + changed.size() > 512) {
            surface_dirty.clear();
            surface_full_gen = surface_gen; // (too many to look through: everything again)
        } else {
            for (const auto& c : changed) surface_dirty.push_back(SurfDirty{surface_gen, c[0], c[1], c[2], c[3]});
        }
        log::info("ground: " + std::to_string(bus_surfaces.size()) + " [surface] objects under the wheels (" +
                  std::to_string(changed.size()) + " changed)");
    };
    const std::vector<map::MapObjectInst> no_objects;
    // highest surface face under (x, y) at or below z_hint, or -1e9
    auto surface_height = [&](float x, float y, float z_hint) {
        float best = -1.e9f;
        const auto cit = surface_cells.find(surf_cell_key(static_cast<int>(std::floor(x / kSurfCell)),
                                                          static_cast<int>(std::floor(y / kSurfCell))));
        if (cit == surface_cells.end()) return best;
        for (const auto sidx : cit->second) {
            const auto& si = bus_surfaces[sidx];
            const float dx = x - si.x, dy = y - si.y;
            const float lx = dx * si.c - dy * si.s, ly = dx * si.s + dy * si.c;
            const auto it = si.shape->grid.find((static_cast<std::int64_t>(static_cast<int>(std::floor(lx / 4.f))) << 32) ^
                                                static_cast<std::uint32_t>(static_cast<int>(std::floor(ly / 4.f))));
            if (it == si.shape->grid.end()) continue;
            for (const auto ti : it->second) {
                const auto& t = si.shape->tris[ti];
                const float d = (t[4] - t[7]) * (t[0] - t[6]) + (t[6] - t[3]) * (t[1] - t[7]);
                if (std::fabs(d) < 1e-9f) continue;
                const float w0 = ((t[4] - t[7]) * (lx - t[6]) + (t[6] - t[3]) * (ly - t[7])) / d;
                const float w1 = ((t[7] - t[1]) * (lx - t[6]) + (t[0] - t[6]) * (ly - t[7])) / d;
                const float w2 = 1.f - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                const float z = si.z + w0 * t[2] + w1 * t[5] + w2 * t[8];
                if (z <= z_hint + 0.6f && z > best) best = z;
            }
        }
        return best;
    };
    // Near-bus candidates (rebuilt every 40 m driven or when the lists change): the splines that can
    // be under a wheel with their .sli resolved, and the objects with their .sco. Every ground-cache
    // miss scanned all edit_splines (load_sli_shared + projection for each within 80 m) and both
    // gathers scanned all edit_objects with load_sco every metre: 100-700 ms bus ticks at 1 GHz.
    struct BusSplCand {
        std::size_t idx = 0;
        std::int32_t id = 0;
        std::shared_ptr<const spline::SplineDefinition> def;
    };
    struct BusObjCand {
        std::size_t idx = 0;
        std::int32_t id = 0;
        const sco::SceneryObject* sco = nullptr;
    };
    std::vector<BusSplCand> bus_spl_cands;
    std::vector<BusObjCand> bus_obj_cands;
    struct GroundCellCands {
        std::vector<BusSplCand> cands;
        std::size_t nspl = 0; // edit_splines looked at so far (streaming appends)
        std::int32_t last_id = 0; // id of the last of them (the list changed under it otherwise)
    };
    std::unordered_map<std::int64_t, GroundCellCands> ground_cell_cands; // 100 m cell -> its splines
    float bus_cand_cx = 1e30f, bus_cand_cy = 1e30f;
    std::size_t bus_cand_nspl = 0, bus_cand_nobj = 0;
    const void* bus_cand_spl_data = nullptr;
    const void* bus_cand_obj_data = nullptr;
    std::int32_t bus_cand_spl_last = 0, bus_cand_obj_last = 0; // id of the last entry looked at
    auto refresh_bus_cands = [&](float cx, float cy) {
        if (std::fabs(cx - bus_cand_cx) < 40.f && std::fabs(cy - bus_cand_cy) < 40.f &&
            bus_cand_nspl == edit_splines.size() && bus_cand_spl_data == edit_splines.data() &&
            bus_cand_nobj == edit_objects.size() && bus_cand_obj_data == edit_objects.data())
            return;
        // Streaming only appends: the new entries are looked at, not all ~47k objects and 14k splines again
        // (rebuilt whole each time a tile added objects - most frames while driving). Entries that moved are
        // caught by the id checks of the users and by the full rebuild every 40 m.
        const bool same_place = std::fabs(cx - bus_cand_cx) < 40.f && std::fabs(cy - bus_cand_cy) < 40.f;
        std::size_t spl_from = 0, obj_from = 0;
        // (only when the entries already looked at are still where they were: the last one keeps its id)
        const bool spl_kept = bus_cand_nspl == 0 ||
                              (edit_splines.size() >= bus_cand_nspl && edit_splines[bus_cand_nspl - 1].id == bus_cand_spl_last);
        const bool obj_kept = bus_cand_nobj == 0 ||
                              (edit_objects.size() >= bus_cand_nobj && edit_objects[bus_cand_nobj - 1].id == bus_cand_obj_last);
        if (same_place && spl_kept && obj_kept) {
            spl_from = bus_cand_nspl;
            obj_from = bus_cand_nobj;
        } else {
            bus_cand_cx = cx;
            bus_cand_cy = cy;
            bus_spl_cands.clear();
            bus_obj_cands.clear();
        }
        cx = bus_cand_cx;
        cy = bus_cand_cy;
        bus_cand_nspl = edit_splines.size();
        bus_cand_spl_data = edit_splines.data();
        bus_cand_nobj = edit_objects.size();
        bus_cand_obj_data = edit_objects.data();
        bus_cand_spl_last = edit_splines.empty() ? 0 : edit_splines.back().id;
        bus_cand_obj_last = edit_objects.empty() ? 0 : edit_objects.back().id;
        for (std::size_t i = spl_from; i < edit_splines.size(); ++i) {
            const auto& sp = edit_splines[i];
            if (sp.sli_path.empty()) continue;
            // a query within 50 m of the centre takes splines starting within 80 m of it
            if (std::fabs(sp.position.x - cx) > 130.f + sp.length || std::fabs(sp.position.y - cy) > 130.f + sp.length) continue;
            auto def = spline::load_sli_shared(sp.sli_path);
            if (!def || def->only_editor || def->height_profiles.empty()) continue;
            bus_spl_cands.push_back({i, sp.id, std::move(def)});
        }
        for (std::size_t i = obj_from; i < edit_objects.size(); ++i) {
            const auto& o = edit_objects[i];
            if (std::fabs(o.position.x - cx) > 170.f || std::fabs(o.position.y - cy) > 170.f) continue;
            const auto* sco = sco::load_sco(o.sco_path);
            if (!sco) continue;
            bus_obj_cands.push_back({i, o.id, sco});
        }
    };
    auto bus_ground = [&](float ex, float ey, float z_hint) {
        // (outside the candidates' area only: a spline list grown by streaming no longer sent every query
        //  through all edit_splines - the candidates are re-checked by id and topped up on the next gather)
        // Away from the bus (the AI cars, up to 400 m off): the splines of the 100 m cell around the point,
        // found once per cell. Each query went through all edit_splines (get_height_above_all, ~1 ms with
        // 14k splines) for every car wheel entering a new 0.5 m ground cell.
        const std::vector<BusSplCand>* cands = &bus_spl_cands;
        if (std::fabs(ex - bus_cand_cx) > 50.f || std::fabs(ey - bus_cand_cy) > 50.f || bus_cand_nspl == 0) {
            constexpr float kCell = 100.f;
            const int gx = static_cast<int>(std::floor(ex / kCell)), gy = static_cast<int>(std::floor(ey / kCell));
            const std::int64_t key = (static_cast<std::int64_t>(gx) << 32) ^ static_cast<std::uint32_t>(gy);
            auto& cell = ground_cell_cands[key];
            if (cell.nspl > edit_splines.size() || (cell.nspl > 0 && edit_splines[cell.nspl - 1].id != cell.last_id)) {
                cell.cands.clear();
                cell.nspl = 0;
            }
            if (cell.nspl < edit_splines.size()) {
                const float ccx = (gx + 0.5f) * kCell, ccy = (gy + 0.5f) * kCell;
                for (std::size_t i = cell.nspl; i < edit_splines.size(); ++i) {
                    const auto& sp = edit_splines[i];
                    if (sp.sli_path.empty()) continue;
                    // (a spline starting within 80 m + the half cell of any point in it, as the bus candidates)
                    if (std::fabs(sp.position.x - ccx) > 130.f + sp.length || std::fabs(sp.position.y - ccy) > 130.f + sp.length) continue;
                    auto def = spline::load_sli_shared(sp.sli_path);
                    if (!def || def->only_editor || def->height_profiles.empty()) continue;
                    cell.cands.push_back({i, sp.id, std::move(def)});
                }
                cell.nspl = edit_splines.size();
                cell.last_id = edit_splines.empty() ? 0 : edit_splines.back().id;
            }
            if (ground_cell_cands.size() > 4096) ground_cell_cands.clear(); // (rebuilt as needed)
            else cands = &cell.cands;
            if (cands == &bus_spl_cands) {
                const float base = world_height::get_height_above_all(
                    ex, ey, z_hint, [&](float x, float y) { return sample_terrain_editor_xy(x, y); }, edit_splines,
                    no_objects, cfg.omsi_root);
                return (std::max)(base, surface_height(ex, ey, z_hint));
            }
        }
        // Same rule as world_height::get_height_above_all (TMapKachel_GetHeightAboveAll) over the
        // candidates only: highest spline profile under the point at or below z_hint.
        float best = sample_terrain_editor_xy(ex, ey);
        for (const auto& c : *cands) {
            if (c.idx >= edit_splines.size()) continue; // (the list shrank since the candidates were found)
            const auto& sp = edit_splines[c.idx];
            if (sp.id != c.id) continue;
            if (std::fabs(sp.position.x - ex) > 80.f + sp.length || std::fabs(sp.position.y - ey) > 80.f + sp.length) continue; // (a long spline reaches far from its start: cars sank into the far part of every one over ~80 m)
            float along = 0.f, lat = 0.f;
            if (!spline::world_xy_to_spline_local(sp, ex, ey, along, lat)) continue;
            float z_off = -1.e9f;
            for (const auto& hp : c.def->height_profiles) {
                const float lo = (std::min)(hp.left, hp.right);
                const float hi = (std::max)(hp.left, hp.right);
                if (lat < lo - 0.05f || lat > hi + 0.05f) continue;
                const float span = hi - lo;
                const float t = span > 1.e-5f ? (lat - hp.left) / (hp.right - hp.left + 1.e-8f) : 0.f;
                z_off = (std::max)(z_off, hp.height + (hp.skew - hp.height) * std::clamp(t, 0.f, 1.f));
            }
            if (z_off < -1.e8f) continue;
            const auto samp = spline::evaluate_spline(sp, std::clamp(along, 0.f, (std::max)(sp.length, 0.01f)), 0.f, z_off);
            if (samp.z > best && samp.z <= z_hint) best = samp.z;
        }
        return (std::max)(best, surface_height(ex, ey, z_hint));
    };

    auto bus_height = [&](float ex, float ey, float z_hint) {
        return world_height::get_height_above_all(
            ex, ey, z_hint, [&](float x, float y) { return sample_terrain_editor_xy(x, y); }, edit_splines,
            edit_objects, cfg.omsi_root);
    };

    auto exit_bus_drive = [&]() {
        if (!bus_drive_active) return;
        bus_drive_active = false;
        camera.editor_nav = true;
        camera.orbit_target_locked = false;
        camera.reset_mouse();
        panel.set_status("OMSI bus: editor (Esc)");
        log::info("omsi bus exit drive");
    };

    auto despawn_omsi_bus = [&]() {
        if (!omsi_bus) return;
        for (const auto ch : omsi_bus->chunks)
            if (ch != kNoChunk) renderer.release_chunk_gpu(ch);
        for (const auto ch : omsi_bus->sec_chunks)
            if (ch != kNoChunk) renderer.release_chunk_gpu(ch);
        if (bus_drive_active) exit_bus_drive();
        omsi_bus.reset();
        panel.set_status("OMSI bus: despawn");
        log::info("omsi bus despawned");
    };

    // Camera (OMSI views). Points of the .bus are in the vehicle frame (x right, y forward, z up)
    // and go through openOMSI's body matrix (pitch/roll ride with the chassis). The orbit camera puts
    // its eye at target + radius * (sin phi cos theta, sin phi sin theta, cos phi).
    auto apply_bus_camera = [&](const OmsiBusInst& b) {
        XMMATRIX body = omsi_object_rotation_matrix(b.heading_deg, b.pitch_deg, b.roll_deg) *
                        XMMatrixTranslation(b.x, b.y, b.z);
        if (b.oo) {
            XMFLOAT4X4 m{};
            std::memcpy(&m, b.oo_body, sizeof(m));
            body = XMLoadFloat4x4(&m);
        }
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
        camera.ground_z = b.ground_z;
        camera.z_up = true;
        camera.editor_nav = false;
        camera.orbit_target_locked = true;

        const auto& drv = b.doc.driver_cameras;
        const auto& pax = b.doc.passenger_cameras;
        if (bus_cam_mode == 1 && drv.empty()) bus_cam_mode = 2;
        if (bus_cam_mode == 3 && pax.empty()) bus_cam_mode = drv.empty() ? 2 : 1;

        // mouse look since the last frame becomes part of the offset
        if (bus_cam_need_snap) {
            bus_cam_look_dth = 0.f;
            bus_cam_look_dph = 0.f;
        } else {
            bus_cam_look_dth += camera.theta - bus_cam_last_th;
            bus_cam_look_dph += camera.phi - bus_cam_last_ph;
        }
        const auto set_angles = [&](float base_th, float base_ph) {
            camera.theta = base_th + bus_cam_look_dth;
            camera.phi = std::clamp(base_ph + bus_cam_look_dph, 0.1f, 3.14159265f - 0.1f);
            bus_cam_look_dph = camera.phi - base_ph;
            bus_cam_last_th = camera.theta;
            bus_cam_last_ph = camera.phi;
        };

        if (bus_cam_mode == 1 || bus_cam_mode == 3) {
            const auto& list = bus_cam_mode == 1 ? drv : pax;
            int& idx = bus_cam_mode == 1 ? bus_cam_driver_idx : bus_cam_pax_idx;
            if (idx < 0 || idx >= static_cast<int>(list.size())) idx = 0;
            const auto& c = list[static_cast<std::size_t>(idx)];
            const float yaw = c.yaw_deg * 3.14159265f / 180.f, pitch = c.pitch_deg * 3.14159265f / 180.f;
            const auto f = dir_world(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch));
            const auto eye = to_world(c.pos_x, c.pos_y, c.pos_z);
            // eye on the camera point, target a few cm ahead: mouse look turns the head in place
            constexpr float kR = 0.05f;
            camera.radius = kR;
            camera.target_x = eye.x + f.x * kR;
            camera.target_y = eye.y + f.y * kR;
            camera.target_z = eye.z + f.z * kR;
            set_angles(std::atan2(-f.y, -f.x), std::acos(std::clamp(-f.z, -1.f, 1.f)));
            bus_cam_need_snap = false;
            return;
        }

        // outside: orbit the middle of the bounding box, not the vehicle origin (an axle)
        const auto ctr = to_world(0.f, b.doc.bbox_offset_y, b.doc.bbox_half_z);
        camera.target_x = ctr.x;
        camera.target_y = ctr.y;
        camera.target_z = ctr.z;
        const auto fwd = dir_world(0.f, 1.f, 0.f);
        const float behind_th = std::atan2(-fwd.y, -fwd.x);
        if (bus_cam_need_snap) {
            camera.radius = (std::max)(b.doc.bbox_half_y * 2.f + 6.f, 10.f);
            camera.phi = 1.2f;
            bus_cam_last_ph = camera.phi;
            bus_cam_look_dph = 0.f;
        }
        if (bus_cam_mode == 2) {
            set_angles(behind_th, 1.2f); // turns with the bus
        } else {
            if (bus_cam_need_snap) camera.theta = behind_th; // free orbit: world-fixed after the snap
            bus_cam_last_th = camera.theta;
            bus_cam_last_ph = camera.phi;
        }
        bus_cam_need_snap = false;
    };

    // Spawn point: the start of the selected spline (heading along it), else the camera's target,
    // facing away from the camera. Editor space.
    std::optional<std::array<float, 3>> bus_spawn_at; // x, y, heading: set by the track test hook
    auto bus_spawn_pose = [&](float& ex, float& ey, float& heading_deg) {
        constexpr float kRad2Deg = 180.f / 3.14159265f;
        if (bus_spawn_at) {
            ex = (*bus_spawn_at)[0];
            ey = (*bus_spawn_at)[1];
            heading_deg = (*bus_spawn_at)[2];
            bus_spawn_at.reset();
            return;
        }
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size()) {
            const auto& fs = edit_splines[static_cast<std::size_t>(selected_spline)];
            const auto p0 = spline::evaluate_spline(fs, 0.f);
            const auto p1 = spline::evaluate_spline(fs, (std::min)(fs.length, 3.f));
            ex = p0.x;
            ey = p0.y;
            heading_deg = std::atan2(p1.x - p0.x, p1.y - p0.y) * kRad2Deg;
            return;
        }
        // The nearest vehicle lane ([path] typ 0, one-way) to a point: position and heading of travel.
        const auto nearest_lane = [&](float px, float py, float radius, float& lx, float& ly, float& lh) -> float {
            float best = radius;
            for (const auto& sp : edit_splines) {
                if (std::hypot(sp.position.x - px, sp.position.y - py) > radius + sp.length) continue;
                const auto def = spline::load_sli_shared(sp.sli_path);
                if (!def || def->only_editor) continue;
                for (const auto& pth : def->paths) {
                    if (pth.typ != 0 || pth.direction == 2) continue;
                    const int n = (std::max)(2, static_cast<int>(sp.length / 2.f) + 1);
                    for (int i = 0; i + 1 < n; ++i) {
                        const float a0 = sp.length * static_cast<float>(i) / (n - 1);
                        const auto q = spline::evaluate_spline(sp, a0, pth.lateral);
                        const float d = std::hypot(q.x - px, q.y - py);
                        if (d >= best) continue;
                        const auto q2 = spline::evaluate_spline(sp, (std::min)(a0 + 2.f, sp.length), pth.lateral);
                        float hx = q2.x - q.x, hy = q2.y - q.y;
                        if (pth.direction == 1) {
                            hx = -hx;
                            hy = -hy;
                        }
                        best = d;
                        lx = q.x;
                        ly = q.y;
                        lh = std::atan2(hx, hy) * kRad2Deg;
                    }
                }
            }
            return best;
        };
        // 1) as OMSI / openOMSI start the player's bus: at a map entrypoint (global.cfg [entrypoints]),
        //    the one nearest the camera (where the entrypoint picker put it)
        {
            const map::EntryPoint* pick = nullptr;
            float best = 600.f;
            for (const auto& ep : cfg.global.entrypoints) {
                const float d = std::hypot(ep.position.x - tile_ox - camera.target_x, ep.position.y - tile_oy - camera.target_y);
                if (d < best) {
                    best = d;
                    pick = &ep;
                }
            }
            if (pick) {
                ex = pick->position.x - tile_ox;
                ey = pick->position.y - tile_oy;
                // OMSI's quaternion is D3D (y up, z north): yaw about y, clockwise from north
                const float qx = pick->rotation_quat.x, qy = pick->rotation_quat.y, qz = pick->rotation_quat.z,
                            qw = pick->rotation_quat.w;
                heading_deg = std::atan2(2.f * (qw * qy + qx * qz), 1.f - 2.f * (qx * qx + qy * qy)) * kRad2Deg;
                float lx = 0.f, ly = 0.f, lh = 0.f;
                const float off = nearest_lane(ex, ey, 30.f, lx, ly, lh);
                log::info("omsi bus: spawn at entrypoint '" + pick->name + "' id=" + std::to_string(pick->id) +
                          " heading " + std::to_string(heading_deg) + " (nearest lane " + std::to_string(off) +
                          " m away, heading " + std::to_string(lh) + ")");
                return;
            }
        }
        // 2) else as openOMSI puts its AI vehicles: on the nearest lane, facing its direction of travel
        {
            float lx = 0.f, ly = 0.f, lh = 0.f;
            const float d = nearest_lane(camera.target_x, camera.target_y, 300.f, lx, ly, lh);
            if (d < 300.f) {
                ex = lx;
                ey = ly;
                heading_deg = lh;
                log::info("omsi bus: spawn on the nearest lane, " + std::to_string(d) + " m from the camera");
                return;
            }
        }
        ex = camera.target_x;
        ey = camera.target_y;
        heading_deg = std::atan2(-std::cos(camera.theta), -std::sin(camera.theta)) * kRad2Deg;
        log::warn("omsi bus: no entrypoint or lane near the camera - spawned at the camera");
    };

    // Every .o3d of a model and its textures read and decoded on all cores before the meshes go up one by
    // one: the HH AGG300 (some 470 meshes over three sections) froze the editor ~7 s at spawn reading them in
    // turn on the main thread.
    auto prefetch_model_assets = [&](const omsi::model_cfg::ModelCfgDocument& model) {
        const auto* plod = omsi::model_cfg::primary_lod(model);
        if (!plod) return;
        const auto dir = model.cfg_path.parent_path();
        std::vector<std::filesystem::path> o3ds;
        std::unordered_set<std::wstring> seen;
        std::vector<std::filesystem::path> texs;
        std::unordered_set<std::wstring> seen_tex;
        auto add_tex = [&](const std::filesystem::path& base, const std::string& name) {
            if (name.empty()) return;
            if (const auto t = gfx::dx12::resolve_texture_path(base, name, cfg.omsi_root))
                if (seen_tex.insert(t->native()).second) texs.push_back(*t);
        };
        for (const auto& mesh : plod->meshes) {
            if (mesh.mesh_file.empty()) continue;
            const auto o3d_path = dir / mesh.mesh_file;
            if (seen.insert(o3d_path.native()).second) o3ds.push_back(o3d_path);
            for (const auto& m : mesh.materials) {
                add_tex(o3d_path, m.diffuse_texture);
                if (m.nightmap_texture) add_tex(o3d_path, *m.nightmap_texture);
                if (m.lightmap_texture) add_tex(o3d_path, *m.lightmap_texture);
                if (m.envmap_texture) add_tex(o3d_path, *m.envmap_texture);
            }
        }
        std::vector<std::vector<std::string>> mats(o3ds.size());
        std::for_each(std::execution::par, o3ds.begin(), o3ds.end(), [&](const std::filesystem::path& p) {
            try {
                if (const auto m = o3d::load_mapped_shared(p); m && m->valid()) {
                    auto& out = mats[static_cast<std::size_t>(&p - o3ds.data())];
                    for (const auto& mt : m->mesh().parse_materials()) out.push_back(mt.texture);
                }
            } catch (...) {
            }
        });
        for (std::size_t i = 0; i < o3ds.size(); ++i)
            for (const auto& t : mats[i]) add_tex(o3ds[i], t);
        const auto t0 = std::chrono::steady_clock::now();
        gfx::dx12::prefetch_textures_cpu(texs);
        log::info("omsi bus: prefetched " + std::to_string(o3ds.size()) + " meshes, " + std::to_string(texs.size()) +
                  " textures in " +
                  std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()) +
                  " ms");
    };
    // meshes put on the GPU a few per frame while a bus is pending: (model.cfg path # mesh index) -> chunk, taken
    // by try_spawn_omsi_bus instead of uploading them all in one frame
    std::unordered_map<std::string, std::size_t> bus_preuploaded;
    // a bus request made ahead: where it goes and openOMSI's vehicle already made on the worker
    std::optional<std::array<float, 4>> bus_pre_pose; // x, y, ground z, heading
    OoBus* bus_pre_oo = nullptr;
    const auto bus_pre_key = [](const std::filesystem::path& cfg_path, std::size_t mi) {
        return omsi::path_text(cfg_path) + "#" + std::to_string(mi);
    };
    // A vehicle mesh's [matl_change] slots: the slot's texture swaps to the change's texture while its variable
    // is on (lamps, brake lights, blinkers, destination LEDs) - openOMSI does it per vehicle from its variables;
    // the editor left every lamp off.
    auto vehicle_matl_changes = [&](const omsi::model_cfg::ModelCfgMeshEntry& mesh, const std::filesystem::path& o3d_path) {
        std::vector<VehMatlChange> out;
        for (const auto& m : mesh.materials) {
            if (!m.is_matl_change || m.change_var.empty() || m.diffuse_texture.empty()) continue;
            std::string k = m.diffuse_texture;
            for (char& c : k) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            VehMatlChange c;
            c.slot_key = "mc:" + k + "#" + std::to_string(m.mat_id);
            c.var = m.change_var;
            c.o3d_path = o3d_path;
            // a [matl_item] that adds a night / light map: the slot keeps its texture and glows by that map while
            // the variable is on (openOMSI 0x5efae8; the AGG300's saloon, cockpit and advert lights)
            const auto& item_map = m.item_nightmap_texture ? m.item_nightmap_texture : m.item_lightmap_texture;
            if (item_map && !item_map->empty()) {
                c.target = m.item_nightmap_texture ? 2 : 3;
                if (const auto p = gfx::dx12::resolve_texture_path(o3d_path, *item_map, cfg.omsi_root)) c.on_path = *p;
            } else if (const auto p = gfx::dx12::resolve_texture_path(o3d_path, m.diffuse_texture, cfg.omsi_root)) {
                c.on_path = *p;
            }
            if (!c.on_path.empty()) out.push_back(std::move(c));
        }
        // (decoded on a worker now: switched on the first time - a destination sign, the lamps - six of them
        //  read on the main thread at once froze the editor for 1.2 s)
        std::vector<std::filesystem::path> pre;
        for (const auto& c : out) pre.push_back(c.on_path);
        if (!pre.empty()) gfx::dx12::prefetch_textures_cpu(pre);
        return out;
    };
    // set chunk `ch`'s change slots for the variables now (`state`: what each slot shows, -1 unknown)
    auto vehicle_apply_changes = [&](std::size_t ch, const std::vector<VehMatlChange>& changes, std::vector<signed char>& state,
                                     const std::function<float(const std::string&)>& var) {
        if (state.size() != changes.size()) state.assign(changes.size(), -1);
        for (std::size_t j = 0; j < changes.size(); ++j) {
            const auto& c = changes[j];
            const signed char on = var(c.var) > 0.5f ? 1 : 0;
            if (state[j] == on) continue;
            // one new texture table a frame over all vehicles (the rest follow on the next frames)
            static double s_up_t = -1.0;
            static int s_up_n = 0;
            if (on && !renderer.chunk_freetex_variant_ready(ch, c.slot_key, c.on_path, c.target)) {
                const double tnow = glfwGetTime();
                if (tnow != s_up_t) {
                    s_up_t = tnow;
                    s_up_n = 0;
                }
                if (s_up_n++ >= 1) continue;
                // (starts the worker decode, or puts the finished texture up: shown once it is ready)
                (void)renderer.set_chunk_freetex_variant(ch, c.slot_key, c.on_path, c.target);
                if (!renderer.chunk_freetex_variant_ready(ch, c.slot_key, c.on_path, c.target)) continue;
            }
            state[j] = on;
            (void)renderer.set_chunk_freetex_variant(ch, c.slot_key, on ? c.on_path : std::filesystem::path{}, c.target);
        }
    };
    auto try_spawn_omsi_bus = [&](std::string bus_file) {
        log::info("omsi bus spawn request '" + bus_file + "' omsi_root='" + omsi::path_text(cfg.omsi_root) + "'");
        if (cfg.omsi_root.empty()) {
            panel.set_status("OMSI bus: no hay raiz de OMSI 2 configurada");
            log::warn("omsi bus: spawn aborted, no OMSI root configured");
            return;
        }
        if (bus_file.empty()) {
            panel.set_status("OMSI bus: elige un vehiculo");
            log::warn("omsi bus: spawn aborted, no vehicle selected");
            return;
        }
        // path_text gives ACP text when every character fits, UTF-8 otherwise (Cyrillic/CJK bus folders)
        const std::filesystem::path bus_path = omsi::is_valid_utf8(bus_file)
                                                   ? std::filesystem::path(omsi::utf8_to_wide(bus_file))
                                                   : std::filesystem::path(bus_file);
        const auto doc = omsi::bus::load_bus(bus_path);
        if (!doc) {
            panel.set_status("OMSI bus: no se pudo leer " + omsi::path_text(bus_path.filename()));
            log::warn("omsi bus: load_bus failed " + bus_file);
            return;
        }
        const auto model_path = omsi::bus::resolve_model_cfg_path(*doc);
        auto model = omsi::model_cfg::load_model_cfg(model_path);
        if (!model) {
            panel.set_status("OMSI bus: falta model.cfg " + omsi::path_text(model_path.filename()));
            log::warn("omsi bus: model.cfg missing " + omsi::path_text(model_path));
            return;
        }

        despawn_omsi_bus();
        auto b = std::make_unique<OmsiBusInst>();
        b->bus_file = bus_file;
        b->doc = *doc;
        b->model = std::move(*model);
        const bool use_oo = oo_ready();
        if (!use_oo) {
        b->rt = std::make_unique<omsi::script::VehicleRuntime>();
        const auto rt_t0 = std::chrono::steady_clock::now();
        const bool rt_ok = b->rt->load(b->doc, cfg.omsi_root);
        log::info("omsi bus runtime load ok=" + std::to_string(rt_ok) + " consts=" +
                  std::to_string(b->rt->merged_consts().size()) + " public_vars=" +
                  std::to_string(b->rt->public_floats().size()) + " model=" + omsi::path_text(model_path) + " in " +
                  std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rt_t0).count()) +
                  " ms");
        // Colour scheme: first CTI of the first pack (as the viewer does).
        {
            std::filesystem::path cti_pick;
            for (const auto& pack : b->model.ctc_packs) {
                for (const auto& p : pack.cti_files) {
                    if (cti_pick.empty() && omsi::path_text(p.stem()).find("ASEAG") != std::string::npos) cti_pick = p;
                }
                if (cti_pick.empty() && !pack.cti_files.empty()) cti_pick = pack.cti_files.front();
                if (!cti_pick.empty()) break;
            }
            if (cti_pick.empty() && !b->model.cti_files.empty()) cti_pick = b->model.cti_files.front();
            if (!cti_pick.empty())
                if (const auto cti = omsi::cti::load_cti(cti_pick)) b->rt->apply_cti(*cti);
        }
        } // !use_oo

        const auto* lod = omsi::model_cfg::primary_lod(b->model);
        if (!lod) {
            panel.set_status("OMSI bus: model.cfg sin [LOD]");
            return;
        }
        if (bus_preuploaded.empty()) prefetch_model_assets(b->model); // (already up when it came through request_spawn)
        b->chunks.assign(lod->meshes.size(), kNoChunk);
        b->shadow.assign(lod->meshes.size(), false);
        std::size_t uploaded = 0;
        for (std::size_t mi = 0; mi < lod->meshes.size(); ++mi) {
            const auto& mesh = lod->meshes[mi];
            b->shadow[mi] = mesh.is_shadow_mesh;
            if (mesh.mesh_file.empty()) continue;
            if (const auto pit = bus_preuploaded.find(bus_pre_key(b->model.cfg_path, mi)); pit != bus_preuploaded.end()) {
                renderer.set_chunk_visible(pit->second, true);
                b->chunks[mi] = pit->second;
                bus_preuploaded.erase(pit);
                ++uploaded;
                continue;
            }
            const auto o3d_path = b->model.cfg_path.parent_path() / mesh.mesh_file;
            const auto mapped = o3d::load_mapped_shared(o3d_path);
            if (!mapped || !mapped->valid()) {
                log::warn("omsi bus: mesh load failed " + omsi::path_text(o3d_path));
                continue;
            }
            if (!renderer.append_o3d(*mapped, &mesh)) {
                log::warn("omsi bus: GPU upload failed " + omsi::path_text(o3d_path));
                continue;
            }
            const std::size_t chunk = renderer.chunk_count() - 1;
            renderer.set_chunk_visible(chunk, true);
            // (one-sided, depth-written alpha 2 in file order, as Omsi.exe draws a vehicle: CULLMODE stays
            //  CCW for meshes, ZWRITEENABLE = !noZwrite - see model_cfg_loader apply_omsi_material_state)
            b->chunks[mi] = chunk;
            ++uploaded;
        }
        if (uploaded == 0) {
            for (const auto ch : b->chunks)
                if (ch != kNoChunk) renderer.release_chunk_gpu(ch);
            panel.set_status("OMSI bus: ningun mesh del modelo se pudo subir");
            return;
        }
        b->anim.init(b->model);
        // lamps: the [matl_change] slots of every mesh, switched from the bus variables each frame
        for (std::size_t mi = 0; mi < lod->meshes.size(); ++mi) {
            if (b->chunks[mi] == kNoChunk || lod->meshes[mi].mesh_file.empty()) continue;
            auto ch = vehicle_matl_changes(lod->meshes[mi], b->model.cfg_path.parent_path() / lod->meshes[mi].mesh_file);
            if (ch.empty()) continue;
            b->mc_chunk.push_back(b->chunks[mi]);
            b->mc_changes.push_back(std::move(ch));
            b->mc_state.emplace_back();
        }

        float ex = 0.f, ey = 0.f, heading = 0.f;
        float pre_gz = 0.f;
        if (bus_pre_pose) {
            ex = (*bus_pre_pose)[0];
            ey = (*bus_pre_pose)[1];
            pre_gz = (*bus_pre_pose)[2];
            heading = (*bus_pre_pose)[3];
        } else {
            bus_spawn_pose(ex, ey, heading);
        }
        if (use_oo) {
            const float gz = bus_pre_pose ? pre_gz : bus_ground(ex, ey, camera.target_z + 50.f);
            const auto u8 = bus_path.u8string();
            b->oo = bus_pre_oo ? bus_pre_oo : oo_spawn(std::string(u8.begin(), u8.end()).c_str(), ex, ey, gz, heading);
            bus_pre_oo = nullptr;
            if (!b->oo) {
                for (const auto ch : b->chunks)
                    if (ch != kNoChunk) renderer.release_chunk_gpu(ch);
                panel.set_status("OMSI bus: openOMSI no pudo cargar " + omsi::path_text(bus_path.filename()) + " (ver log)");
                return;
            }
            // its depot file (.hof) for this map and a destination on its signs - without one its IBIS had no
            // terminus to show and the destination sign stayed a blank green panel
            {
                const auto mu8 = cfg.global.root.u8string();
                const std::string mdir(mu8.begin(), mu8.end());
                if (!oo_bus_set_depot(b->oo, mdir.c_str(), "", ""))
                    log::warn("omsi bus: no depot file (.hof) for this bus on this map - its signs stay blank");
            }
            b->ground_fn = [&, bi = b.get()](float gx, float gy, float zh) {
                constexpr float kCell = 0.25f;
                const auto corner = [&](int ix, int iy) {
                    const std::int64_t key = (static_cast<std::int64_t>(ix) << 32) ^ static_cast<std::uint32_t>(iy);
                    if (const auto it = bi->ground_cache.find(key); it != bi->ground_cache.end()) {
                        if (surface_cache_valid(it->second.second, ix * kCell, iy * kCell)) {
                            it->second.second = surface_gen;
                            return it->second.first;
                        }
                        bi->ground_cache.erase(it); // (a [surface] object came or went over this cell)
                    }
                    if (bi->ground_cache.size() > 400000) bi->ground_cache.clear();
                    const float h = bus_ground(ix * kCell, iy * kCell, zh);
                    bi->ground_cache.emplace(key, std::make_pair(h, surface_gen));
                    return h;
                };
                const float fx = gx / kCell, fy = gy / kCell;
                const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
                const float tx = fx - ix, ty = fy - iy;
                const float h00 = corner(ix, iy), h10 = corner(ix + 1, iy);
                const float h01 = corner(ix, iy + 1), h11 = corner(ix + 1, iy + 1);
                return (h00 * (1.f - tx) + h10 * tx) * (1.f - ty) + (h01 * (1.f - tx) + h11 * tx) * ty;
            };
            b->oo_ground_hint = gz + 3.f;
            oo_set_ground(
                b->oo,
                +[](void* user, double x, double y, double* out_z) -> int32_t {
                    auto* bi = static_cast<OmsiBusInst*>(user);
                    *out_z = bi->ground_fn(static_cast<float>(x), static_cast<float>(y), bi->oo_ground_hint);
                    return 1;
                },
                b.get());
            {
                // our LOD meshes <-> openOMSI's meshes (same model.cfg, matched by .o3d path in order)
                const auto lower_path = [](std::string t) {
                    for (auto& c : t) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return t;
                };
                std::unordered_map<std::string, std::vector<int>> by_file;
                const int n = oo_mesh_count(b->oo);
                char buf[1024];
                for (int i = 0; i < n; ++i) {
                    if (oo_mesh_file(b->oo, i, buf, sizeof(buf)) == 0) continue;
                    const auto canon = std::filesystem::weakly_canonical(std::filesystem::u8path(buf));
                    const auto u8 = canon.u8string();
                    by_file[lower_path(std::string(u8.begin(), u8.end()))].push_back(i);
                }
                for (auto& [k, v] : by_file) std::reverse(v.begin(), v.end());
                b->oo_mesh.assign(lod->meshes.size(), -1);
                int matched = 0;
                for (std::size_t mi = 0; mi < lod->meshes.size(); ++mi) {
                    if (lod->meshes[mi].mesh_file.empty()) continue;
                    const auto canon =
                        std::filesystem::weakly_canonical(b->model.cfg_path.parent_path() / lod->meshes[mi].mesh_file);
                    const auto u8 = canon.u8string();
                    auto it = by_file.find(lower_path(std::string(u8.begin(), u8.end())));
                    if (it == by_file.end() || it->second.empty()) continue;
                    b->oo_mesh[mi] = it->second.back();
                    it->second.pop_back();
                    ++matched;
                }
                log::info("omsi bus: " + std::to_string(matched) + "/" + std::to_string(lod->meshes.size()) +
                          " meshes posed by openOMSI (it has " + std::to_string(n) + ")");
                // coupled sections: each one's own model.cfg meshes, posed by openOMSI (it tows them)
                const int nsec = oo_section_count(b->oo);
                for (int sct = 0; sct < nsec; ++sct) {
                    char pbuf[1024];
                    int first = 0, count = 0;
                    if (!oo_section_info(b->oo, sct, pbuf, sizeof(pbuf), &first, &count)) continue;
                    const auto sec_bus = std::filesystem::u8path(pbuf);
                    const auto sdoc = omsi::bus::load_bus(sec_bus);
                    if (!sdoc) {
                        log::warn("omsi bus: coupled section .bus unreadable " + omsi::path_text(sec_bus));
                        continue;
                    }
                    auto smodel = omsi::model_cfg::load_model_cfg(omsi::bus::resolve_model_cfg_path(*sdoc));
                    const auto* slod = smodel ? omsi::model_cfg::primary_lod(*smodel) : nullptr;
                    if (!slod) continue;
                    if (bus_preuploaded.empty()) prefetch_model_assets(*smodel);
                    std::unordered_map<std::string, std::vector<int>> sec_by_file;
                    for (int i = first; i < first + count; ++i) {
                        if (oo_mesh_file(b->oo, i, buf, sizeof(buf)) == 0) continue;
                        const auto canon = std::filesystem::weakly_canonical(std::filesystem::u8path(buf));
                        const auto u8c = canon.u8string();
                        sec_by_file[lower_path(std::string(u8c.begin(), u8c.end()))].push_back(i);
                    }
                    for (auto& [k, v] : sec_by_file) std::reverse(v.begin(), v.end());
                    int sec_up = 0;
                    for (std::size_t smi = 0; smi < slod->meshes.size(); ++smi) {
                        const auto& mesh = slod->meshes[smi];
                        if (mesh.mesh_file.empty()) continue;
                        const auto o3d_path = smodel->cfg_path.parent_path() / mesh.mesh_file;
                        std::size_t chunk = kNoChunk;
                        if (const auto pit = bus_preuploaded.find(bus_pre_key(smodel->cfg_path, smi)); pit != bus_preuploaded.end()) {
                            chunk = pit->second;
                            bus_preuploaded.erase(pit);
                        } else {
                            const auto mapped = o3d::load_mapped_shared(o3d_path);
                            if (!mapped || !mapped->valid() || !renderer.append_o3d(*mapped, &mesh)) continue;
                            chunk = renderer.chunk_count() - 1;
                        }
                        renderer.set_chunk_visible(chunk, true);
                        int oi = -1;
                        const auto canon = std::filesystem::weakly_canonical(o3d_path);
                        const auto u8c = canon.u8string();
                        if (auto it = sec_by_file.find(lower_path(std::string(u8c.begin(), u8c.end())));
                            it != sec_by_file.end() && !it->second.empty()) {
                            oi = it->second.back();
                            it->second.pop_back();
                        }
                        // (one-sided, depth-written alpha 2 in file order, as Omsi.exe draws a vehicle: CULLMODE stays
                        //  CCW for meshes, ZWRITEENABLE = !noZwrite - see model_cfg_loader apply_omsi_material_state)
                        b->sec_chunks.push_back(chunk);
                        if (auto mc = vehicle_matl_changes(mesh, o3d_path); !mc.empty()) {
                            b->mc_chunk.push_back(chunk);
                            b->mc_changes.push_back(std::move(mc));
                            b->mc_state.emplace_back();
                        }
                        b->sec_oo.push_back(oi);
                        b->sec_viewpoint.push_back(mesh.viewpoint);
                        b->sec_skin.push_back(mesh.smooth_skin ? 1 : 0);
                        ++sec_up;
                    }
                    log::info("omsi bus: coupled section " + std::to_string(sct + 1) + " " +
                              omsi::path_text(sec_bus.filename()) + ": " + std::to_string(sec_up) + " meshes");
                    b->sec_models.push_back(std::move(*smodel));
                }
                // its paint scheme (oo_bus_paint): every slot showing a [CTCTexture] default takes the scheme's
                // file - the body, its adverts and the window foils (the AGG300's VAN_A_trans.tga)
                auto add_paint = [&](std::size_t chunk, int oi, const std::filesystem::path& cfg_path) {
                    if (chunk == kNoChunk || oi < 0) return;
                    std::string pb(8192, '\0');
                    const std::size_t pn = oo_bus_paint(b->oo, oi, pb.data(), pb.size());
                    pb.resize((std::min)(pn, pb.size()));
                    std::size_t q = 0;
                    while (q < pb.size()) {
                        std::size_t eol = pb.find('\n', q);
                        if (eol == std::string::npos) eol = pb.size();
                        const std::string line = pb.substr(q, eol - q);
                        q = eol + 1;
                        const auto tab = line.find('\t');
                        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
                        std::string key = "ctc:" + line.substr(0, tab);
                        for (char& c : key) {
                            if (c == '\\') c = '/';
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        }
                        std::filesystem::path file = std::filesystem::u8path(line.substr(tab + 1));
                        if (file.is_relative()) file = cfg_path.parent_path().parent_path() / "Texture" / file;
                        b->paint.emplace_back(chunk, std::move(key), std::move(file));
                    }
                };
                for (std::size_t mi = 0; mi < b->chunks.size() && mi < b->oo_mesh.size(); ++mi)
                    add_paint(b->chunks[mi], b->oo_mesh[mi], b->model.cfg_path);
                {
                    std::size_t k = 0;
                    for (std::size_t sm = 0; sm < b->sec_models.size(); ++sm) {
                        const auto* sl = omsi::model_cfg::primary_lod(b->sec_models[sm]);
                        if (!sl) continue;
                        for (std::size_t smi = 0; smi < sl->meshes.size() && k < b->sec_chunks.size(); ++smi) {
                            if (sl->meshes[smi].mesh_file.empty()) continue;
                            add_paint(b->sec_chunks[k], b->sec_oo[k], b->sec_models[sm].cfg_path);
                            ++k;
                        }
                    }
                }
                log::info("omsi bus: paint scheme " + std::to_string(b->paint.size()) + " texture swaps");
                // its meshes: its own headlight pools leave them dark (they lit the cab and dash from the front corners)
                for (const auto ch : b->chunks)
                    if (ch != kNoChunk) renderer.set_chunk_vehicle_self(ch, true);
                for (const auto ch : b->sec_chunks)
                    if (ch != kNoChunk) renderer.set_chunk_vehicle_self(ch, true);
                // its displays: every [useTextTexture] slot with the model's [texttexture(_enh)] of that index
                auto add_texttex = [&](std::size_t chunk, const omsi::model_cfg::ModelCfgMeshEntry& mesh,
                                       const omsi::model_cfg::ModelCfgDocument& model) {
                    if (chunk == kNoChunk) return;
                    for (const auto& m : mesh.materials) {
                        if (!m.use_text_texture) continue;
                        const int ti = *m.use_text_texture;
                        if (ti < 0 || static_cast<std::size_t>(ti) >= model.text_textures.size()) continue;
                        const auto& src = model.text_textures[static_cast<std::size_t>(ti)];
                        bool dup = false;
                        for (const auto& e : b->texttex) dup |= e.chunk == chunk && e.ti == ti;
                        if (dup) continue;
                        BusTextTex e;
                        e.chunk = chunk;
                        e.ti = ti;
                        e.field = src.field;
                        e.tt.string_var = src.field;
                        e.tt.source_index = -1;
                        e.tt.font_name = src.font_name;
                        e.tt.width = src.width;
                        e.tt.height = src.height;
                        e.tt.color_mode = src.color_mode;
                        e.tt.r = src.r;
                        e.tt.g = src.g;
                        e.tt.b = src.b;
                        e.tt.align = src.enh ? src.align : 1;
                        e.tt.letter_spacing = src.enh ? src.letter_spacing : 0;
                        b->texttex.push_back(std::move(e));
                    }
                };
                for (std::size_t mi = 0; mi < b->chunks.size() && mi < lod->meshes.size(); ++mi)
                    add_texttex(b->chunks[mi], lod->meshes[mi], b->model);
                {
                    std::size_t k = 0;
                    for (std::size_t sm = 0; sm < b->sec_models.size(); ++sm) {
                        const auto* sl = omsi::model_cfg::primary_lod(b->sec_models[sm]);
                        if (!sl) continue;
                        for (std::size_t smi = 0; smi < sl->meshes.size() && k < b->sec_chunks.size(); ++smi) {
                            if (sl->meshes[smi].mesh_file.empty()) continue;
                            add_texttex(b->sec_chunks[k], sl->meshes[smi], b->sec_models[sm]);
                            ++k;
                        }
                    }
                }
                log::info("omsi bus: " + std::to_string(b->texttex.size()) + " text-texture display slots");
            }
            (void)maybe_stream_follow(ex, ey, gz); // load the map around the bus before it moves
            oo_autostart(b->oo); // openOMSI's Shift+U: electrics, starter until it runs, gear
            double pose[6]{};
            oo_pose(b->oo, pose);
            oo_body_matrix(b->oo, b->oo_body);
            b->x = static_cast<float>(pose[0]);
            b->y = static_cast<float>(pose[1]);
            b->z = static_cast<float>(pose[2]);
            b->heading_deg = static_cast<float>(pose[3]);
            b->ground_z = gz;
        } else {
        b->body.init(b->doc);
        b->body.place(ex, ey, heading, [&](float gx, float gy, float zh) { return bus_height(gx, gy, zh); });
        b->rt->set_external_chassis(true);
        b->x = b->body.x();
        b->y = b->body.y();
        b->z = b->body.z();
        b->heading_deg = b->body.heading_deg();
        b->pitch_deg = b->body.pitch_deg();
        b->roll_deg = b->body.roll_deg();
        b->ground_z = bus_height(ex, ey, camera.target_z + 50.f);
        log::info("omsi bus body: wheels=" + std::to_string(b->body.wheels().size()) + " z=" + std::to_string(b->z) +
                  " ground=" + std::to_string(bus_height(ex, ey, camera.target_z + 50.f)));
        b->rt->set_driver_input(0.f, 0.f);
        b->rt->set_steering_keys(false, false, true);
        } // C++ fallback

        omsi_bus = std::move(b);
        bus_drive_active = true;
        bus_cam_mode = 1; // OMSI starts in the driver's seat (falls back to outside without cameras)
        bus_cam_driver_idx = (std::max)(0, omsi_bus->doc.std_driver_camera_index);
        bus_cam_pax_idx = 0;
        bus_cam_need_snap = true;
        apply_bus_camera(*omsi_bus);
        editor_ui_settings.last_omsi_bus = bus_file;
        (void)map::save_editor_ui_settings(cfg.global.root, editor_ui_settings);
        panel.set_status("OMSI bus " + omsi::path_text(bus_path.filename()) +
                         (use_oo ? " - arrancando solo (Shift+U)... Num8/arriba acelerar, Num2/abajo frenar, Num4-6/flechas direccion, F1-F4 camara, Esc sale"
                                 : " - Num8/Flecha arriba acelerar, Num2/abajo frenar, Num4-6/flechas direccion, D marcha, . freno mano, F1-F4 camara, Esc sale"));
        log::info("omsi bus spawned " + bus_file + " meshes=" + std::to_string(uploaded) + "/" +
                  std::to_string(lod->meshes.size()) + " at editor=(" + std::to_string(ex) + "," +
                  std::to_string(ey) + ") backend=" + (use_oo ? "openOMSI" : "C++"));
    };

    // Pose: wheels on the ground under the four corners (front/rear axles, half track), pitch and
    // roll from the four heights exactly as the GTA car did, then the model.cfg meshes through their
    // animator (the viewer's mesh * body order, body = OMSI rotation * translation).
    auto update_bus_pose = [&](OmsiBusInst& b, float /*dt*/) {
        const auto* lod = omsi::model_cfg::primary_lod(b.model);
        if (!lod) return;
        for (std::size_t j = 0; j < b.mc_chunk.size(); ++j)
            vehicle_apply_changes(b.mc_chunk[j], b.mc_changes[j], b.mc_state[j],
                                  [&](const std::string& name) { return bus_var(b, name, 0.f); });
        XMMATRIX body = omsi_object_rotation_matrix(b.heading_deg, b.pitch_deg, b.roll_deg) *
                        XMMatrixTranslation(b.x, b.y, b.z);
        if (b.oo) {
            XMFLOAT4X4 m{};
            std::memcpy(&m, b.oo_body, sizeof(m)); // glam column order == DirectXMath row-vector, row-major
            body = XMLoadFloat4x4(&m);
        }
        // [smoothskin]: the bellows as their [setbone] bones (the joint's arches) hold them, from openOMSI
        auto bus_skin = [&](OmsiBusInst& bb, std::size_t chunk, int oi) {
            static std::vector<float> s_skin(6u * 65536u);
            const std::size_t n = oo_bus_skinned(bb.oo, oi, s_skin.data(), 65536u);
            if (n > 0) renderer.update_chunk_skin(chunk, s_skin.data(), static_cast<std::uint32_t>(n));
        };
        // [viewpoint] (OMSI: 1 outside, 2 inside - cab and saloon, 4 AI only): what the camera sees from where
        // it is - inside from the driver's and the passengers' views (F1 / F3), and whenever the eye stands in the
        // bus (walking through the saloon, the free camera put in it); outside from the chase view, the free camera
        // out there, walking past it. (It was "inside while driven" only: the outside view showed the cab, the
        // pedestrian inside saw the bare shell.) The AI-only meshes never show on the player's bus.
        bool view_inside = bus_drive_active && (bus_cam_mode == 1 || bus_cam_mode == 3);
        if (!view_inside) {
            float ex = 0.f, ey = 0.f, ez = 0.f;
            camera.eye_position(ex, ey, ez);
            const float h = b.heading_deg * 3.14159265f / 180.f;
            const float dx = ex - b.x, dy = ey - b.y;
            const float lx = dx * std::cos(h) - dy * std::sin(h);                       // across
            const float ly = dx * std::sin(h) + dy * std::cos(h) - b.doc.bbox_offset_y; // along
            view_inside = std::fabs(lx) < b.doc.bbox_half_x + 0.1f && std::fabs(ly) < b.doc.bbox_half_y + 0.1f &&
                          ez > b.z - 0.5f && ez < b.z + 4.5f;
        }
        const int view_bit = view_inside ? 2 : 1;
        auto viewpoint_ok = [&](int vp) { return vp == 0 || (vp & view_bit) != 0; };
        if (b.oo) {
            for (std::size_t mi = 0; mi < lod->meshes.size(); ++mi) {
                const std::size_t chunk = b.chunks[mi];
                if (chunk == kNoChunk) continue;
                const int oi = mi < b.oo_mesh.size() ? b.oo_mesh[mi] : -1;
                XMMATRIX world = body;
                bool vis = viewpoint_ok(lod->meshes[mi].viewpoint);
                if (oi >= 0) {
                    float m16[16];
                    if (oo_mesh_world(b.oo, oi, m16)) {
                        XMFLOAT4X4 m{};
                        std::memcpy(&m, m16, sizeof(m));
                        world = XMLoadFloat4x4(&m);
                    }
                    vis = vis && oo_mesh_visible(b.oo, oi) != 0;
                }
                renderer.set_chunk_world_matrix(chunk, store_world_transposed(world).data());
                renderer.set_chunk_visible(chunk, vis);
                if (vis && oi >= 0 && lod->meshes[mi].smooth_skin) bus_skin(b, chunk, oi);
                if (vis && renderer.chunk_has_uv_vars(chunk))
                    renderer.apply_chunk_uv_vars(chunk, [&](const std::string& name) { return bus_var(b, name, 0.f); });
            }
            // the coupled sections (towed by openOMSI)
            for (std::size_t k = 0; k < b.sec_chunks.size(); ++k) {
                const std::size_t chunk = b.sec_chunks[k];
                const int oi = b.sec_oo[k];
                float m16[16];
                if (oi < 0 || !oo_mesh_world(b.oo, oi, m16)) {
                    renderer.set_chunk_visible(chunk, false);
                    continue;
                }
                XMFLOAT4X4 m{};
                std::memcpy(&m, m16, sizeof(m));
                renderer.set_chunk_world_matrix(chunk, store_world_transposed(XMLoadFloat4x4(&m)).data());
                const bool vis = (k >= b.sec_viewpoint.size() || viewpoint_ok(b.sec_viewpoint[k])) &&
                                 oo_mesh_visible(b.oo, oi) != 0;
                renderer.set_chunk_visible(chunk, vis);
                if (vis && k < b.sec_skin.size() && b.sec_skin[k]) bus_skin(b, chunk, oi);
                if (vis && renderer.chunk_has_uv_vars(chunk))
                    renderer.apply_chunk_uv_vars(chunk, [&](const std::string& name) { return bus_var(b, name, 0.f); });
            }
            return;
        }
        const auto mats = b.anim.matrices_dx_transposed();
        for (std::size_t mi = 0; mi < lod->meshes.size() && mi < mats.size(); ++mi) {
            const std::size_t chunk = b.chunks[mi];
            if (chunk == kNoChunk) continue;
            XMFLOAT4X4 stored{};
            std::memcpy(&stored, mats[mi].data(), sizeof(stored));
            XMMATRIX world = XMMatrixTranspose(XMLoadFloat4x4(&stored)) * body;
            if (b.shadow[mi]) {
                XMFLOAT4X4 wf{};
                XMStoreFloat4x4(&wf, world);
                world = XMMatrixTranslation(wf._41, wf._42, b.ground_z + 0.02f);
            }
            renderer.set_chunk_world_matrix(chunk, store_world_transposed(world).data());
        }
        // [visible]: a mesh shows when its script variable equals its default.
        for (std::size_t mi = 0; mi < lod->meshes.size(); ++mi) {
            const auto& mesh = lod->meshes[mi];
            if (b.chunks[mi] == kNoChunk) continue;
            bool vis = viewpoint_ok(mesh.viewpoint);
            if (vis && mesh.visible_var) {
                const int v = static_cast<int>(std::lround(bus_var(b, *mesh.visible_var, static_cast<float>(mesh.visible_default))));
                vis = v == mesh.visible_default;
            }
            renderer.set_chunk_visible(b.chunks[mi], vis);
            // [texcoordtransX/Y] and [alphascale] (rain on the glass, roller blinds) from the bus script.
            if (vis && renderer.chunk_has_uv_vars(b.chunks[mi]))
                renderer.apply_chunk_uv_vars(b.chunks[mi],
                                             [&](const std::string& name) { return bus_var(b, name, 0.f); });
        }
    };

    // Obstacles for the bus (openOMSI omsi-scene place.rs rules, AGPL-3.0): only [fixed] objects
    // (incl. [crashmode_pole]) are solid; [collision_mesh] as triangle boxes, else the [boundingbox];
    // no [surface], trees or editor helpers; boxes whose top stays under 0.3 m, flat or giant
    // ones, and big boxes a street runs through (bridges, halls) are no wall.
    std::vector<omsi::vehicle::Obb> bus_obstacles;
    std::unordered_map<std::int32_t, bool> bus_road_through; // object id -> a street crosses its box
    auto gather_bus_surfaces = [&](OmsiBusInst& b) {
        refresh_bus_cands(b.x, b.y);
        refresh_surface_index(); // (the ground cache follows surface_gen in ground_fn)
    };
    std::unordered_map<std::int32_t, std::pair<float, float>> bus_poles; // object id -> [crashmode_pole]
    auto gather_bus_obstacles = [&](const OmsiBusInst& b) {
        bus_obstacles.clear();
        const float bbb[6] = {b.doc.bbox_half_x * 2.f, b.doc.bbox_half_y * 2.f, b.doc.bbox_half_z * 2.f, 0.f,
                              b.doc.bbox_offset_y, b.doc.bbox_half_z};
        omsi::vehicle::Obb probe_box =
            omsi::vehicle::Obb::from_box(bbb, omsi::vehicle::Vec3d{b.x, b.y, b.z}, b.heading_deg);
        probe_box.half.x += 1.0;
        probe_box.half.y += 1.0;
        const double reach = probe_box.radius() + 40.0;
        refresh_bus_cands(b.x, b.y);
        for (const auto& cand : bus_obj_cands) {
            if (cand.idx >= edit_objects.size()) continue;
            const auto& o = edit_objects[cand.idx];
            if (o.id != cand.id) continue;
            if (std::fabs(o.position.x - b.x) > reach || std::fabs(o.position.y - b.y) > reach) continue;
            const auto* sco = cand.sco;
            if (!sco || !sco->fixed || sco->surface || sco->no_collision || sco->tree.valid ||
                sco->only_editor_mesh || sco->meshes.empty())
                continue;
            const float tz = sco::skip_terrain_add(*sco) ? 0.f : sample_terrain_editor_xy(o.position.x, o.position.y);
            const omsi::vehicle::Vec3d pos{o.position.x, o.position.y, o.position.z + tz};
            if (sco->crash_pole) bus_poles[o.id] = {sco->crash_pole_mass_t, sco->crash_pole_load_kn};
            if (!sco->collision_mesh.empty()) {
                const auto mesh_path = sco::resolve_sco_o3d(o.sco_path.parent_path(), sco->collision_mesh);
                if (auto shape = omsi::vehicle::load_collision_shape(mesh_path)) {
                    const omsi::vehicle::MeshObstacle mo(shape, pos, o.rotation_deg.x, o.id);
                    mo.parts_near(probe_box, bus_obstacles);
                }
                continue;
            }
            if (!sco->has_boundingbox) continue;
            const float bb[6] = {sco->bbox_w, sco->bbox_l, sco->bbox_h, sco->bbox_off_x, sco->bbox_off_y, sco->bbox_off_z};
            const float top = bb[5] + bb[2] * 0.5f;
            if (!(bb[2] > 0.4f && top > 0.3f && bb[0] < 400.f && bb[1] < 400.f && bb[0] > 0.05f && bb[1] > 0.05f))
                continue;
            auto ob = omsi::vehicle::Obb::from_box(bb, pos, o.rotation_deg.x);
            ob.id = o.id;
            if (bb[0] > 3.f || bb[1] > 3.f) {
                auto it = bus_road_through.find(o.id);
                if (it == bus_road_through.end()) {
                    bool through = false;
                    omsi::vehicle::Vec2d r, f;
                    ob.axes(r, f);
                    const double rad = ob.radius();
                    for (const auto& sp : edit_splines) {
                        if (through) break;
                        if (std::fabs(sp.position.x - ob.center.x) > rad + sp.length + 5.0 ||
                            std::fabs(sp.position.y - ob.center.y) > rad + sp.length + 5.0)
                            continue;
                        for (float t = 0.f; t <= sp.length && !through; t += 2.f) {
                            const auto w = spline::evaluate_spline(sp, t);
                            const omsi::vehicle::Vec2d d{w.x - ob.center.x, w.y - ob.center.y};
                            through = std::fabs(d.dot(r)) <= ob.half.x && std::fabs(d.dot(f)) <= ob.half.y &&
                                      w.z >= ob.z0 - 1.0 && w.z <= ob.z1 - 0.5;
                        }
                    }
                    it = bus_road_through.emplace(o.id, through).first;
                }
                if (it->second) continue;
            }
            if (ob.overlaps_plan(probe_box)) bus_obstacles.push_back(ob);
        }
    };

    auto bus_camera_keys = [&]() {
        const bool f1 = glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS;
        const bool f2 = glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS;
        const bool f3 = glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS;
        const bool f4 = glfwGetKey(window, GLFW_KEY_F4) == GLFW_PRESS;
        const int n_drv = omsi_bus ? static_cast<int>(omsi_bus->doc.driver_cameras.size()) : 0;
        const int n_pax = omsi_bus ? static_cast<int>(omsi_bus->doc.passenger_cameras.size()) : 0;
        if (f1 && !bus_f1_was) {
            if (bus_cam_mode == 1 && n_drv > 0) bus_cam_driver_idx = (bus_cam_driver_idx + 1) % n_drv;
            bus_cam_mode = 1;
            bus_cam_need_snap = true;
        }
        if (f2 && !bus_f2_was) { bus_cam_mode = 2; bus_cam_need_snap = true; }
        if (f3 && !bus_f3_was) {
            if (bus_cam_mode == 3 && n_pax > 0) bus_cam_pax_idx = (bus_cam_pax_idx + 1) % n_pax;
            bus_cam_mode = 3;
            bus_cam_need_snap = true;
        }
        if (f4 && !bus_f4_was) { bus_cam_mode = 4; bus_cam_need_snap = true; }
        if ((f1 && !bus_f1_was) || (f2 && !bus_f2_was) || (f3 && !bus_f3_was) || (f4 && !bus_f4_was))
            log::info("omsi bus camera mode=" + std::to_string(bus_cam_mode) + " driver=" +
                      std::to_string(bus_cam_driver_idx) + "/" + std::to_string(n_drv) + " pax=" +
                      std::to_string(bus_cam_pax_idx) + "/" + std::to_string(n_pax));
        bus_f1_was = f1;
        bus_f2_was = f2;
        bus_f3_was = f3;
        bus_f4_was = f4;
    };

    // Test autopilot (OMSI_TEST_AUTOPILOT=1): pure pursuit along the spline the bus was spawned on and
    // the splines chained to it (next_id / prev_id), 30 km/h, analog controls into openOMSI.
    struct BusAutopilot {
        bool on = false;
        int spline = -1;   // index into edit_splines
        bool forward = true;
        float lateral = 0.f, lateral_pending = 0.f; // lane offset from the spline's centre
        float t = 0.f, log_t = 0.f, dist = 0.f, err_max = 0.f, err_sum = 0.f;
        int err_n = 0, splines_done = 0;
        float last_x = 0.f, last_y = 0.f;
        // TTData track route (P / OMSI_TEST_TRACK): resolved entries as world polylines
        bool ai = false;                    // AI driving on the path network (P)
        int track = -1;                     // index into the track list, -1 = spline mode
        std::vector<std::array<float, 3>> route; // x, y, z every ~1 m
        std::size_t next_entry = 0;         // first entry not resolved yet (streamed later)
        std::size_t route_i = 0;            // nearest route point
        float resolve_t = 0.f;
    } autopilot;
    // One track entry (spline lane or .sco path) as world points every ~1 m, oriented to start near
    // `from` (the end of the route so far) when the path can be driven both ways.
    auto track_entry_points = [&](const omsi::tt::TrackEntry& e, const std::array<float, 3>* from,
                                  std::vector<std::array<float, 3>>& out) -> bool {
        out.clear();
        for (const auto& sp : edit_splines) {
            if (sp.id != e.id) continue;
            const auto def = spline::load_sli_shared(sp.sli_path);
            float lat = 0.f, hgt = 0.f;
            int dir = 0;
            if (def && e.path_index >= 0 && static_cast<std::size_t>(e.path_index) < def->paths.size()) {
                const auto& pp = def->paths[static_cast<std::size_t>(e.path_index)];
                lat = pp.lateral;
                hgt = pp.height;
                dir = pp.direction;
            }
            const int n = (std::max)(2, static_cast<int>(sp.length) + 1);
            for (int i = 0; i < n; ++i) {
                const auto q = spline::evaluate_spline(sp, sp.length * static_cast<float>(i) / (n - 1), lat, hgt);
                out.push_back({q.x, q.y, q.z});
            }
            bool reverse = dir == 1;
            if (from && dir == 2) {
                const auto d2 = [&](const std::array<float, 3>& a) {
                    return (a[0] - (*from)[0]) * (a[0] - (*from)[0]) + (a[1] - (*from)[1]) * (a[1] - (*from)[1]);
                };
                reverse = d2(out.back()) < d2(out.front());
            }
            if (reverse) std::reverse(out.begin(), out.end());
            return true;
        }
        for (const auto& ob : edit_objects) {
            if (ob.id != e.id) continue;
            const auto sco = sco::load_sco(ob.sco_path);
            if (!sco || e.path_index < 0 || static_cast<std::size_t>(e.path_index) >= sco->paths.size()) return false;
            const auto& path = sco->paths[static_cast<std::size_t>(e.path_index)];
            map::SplineInst mini = sco::sco_path_to_spline_inst(path);
            float plen = std::abs(path.length);
            if (std::abs(path.radius) > 0.001f && plen > 0.001f && plen <= 360.f && plen / std::abs(path.radius) > 3.2f)
                plen = std::abs(path.radius) * plen * (3.14159265f / 180.f);
            mini.length = (std::max)(0.05f, plen);
            const bool abs_z = sco::skip_terrain_add(*sco);
            const float th = terrain_add(abs_z, ob.position.x, ob.position.y, ob.position.z, sco_ground_layer(ob, *sco));
            const XMMATRIX world_m = sco_world_matrix(ob, nullptr, th, 0.f, 0.f);
            const int n = (std::max)(2, static_cast<int>(mini.length) + 1);
            for (int i = 0; i < n; ++i) {
                const auto q = spline::evaluate_spline(mini, mini.length * static_cast<float>(i) / (n - 1));
                XMFLOAT3 w{};
                XMStoreFloat3(&w, xform_point(world_m, q.x, q.y, q.z));
                out.push_back({w.x, w.y, w.z});
            }
            bool reverse = path.direction == 1;
            if (from && path.direction == 2) {
                const auto d2 = [&](const std::array<float, 3>& a) {
                    return (a[0] - (*from)[0]) * (a[0] - (*from)[0]) + (a[1] - (*from)[1]) * (a[1] - (*from)[1]);
                };
                reverse = d2(out.back()) < d2(out.front());
            }
            if (reverse) std::reverse(out.begin(), out.end());
            return true;
        }
        return false; // owner not loaded (yet)
    };
    // Resolve the track's entries from next_entry on, as far as the loaded map goes.
    auto extend_route = [&]() {
        auto& ap = autopilot;
        const auto& tracks = omsi::tt::global_tt_man().data.tracks;
        if (ap.track < 0 || static_cast<std::size_t>(ap.track) >= tracks.size()) return;
        const auto& tr = tracks[static_cast<std::size_t>(ap.track)];
        std::vector<std::array<float, 3>> pts;
        while (ap.next_entry < tr.entries.size()) {
            const std::array<float, 3>* from = ap.route.empty() ? nullptr : &ap.route.back();
            if (!track_entry_points(tr.entries[ap.next_entry], from, pts)) break;
            ap.route.insert(ap.route.end(), pts.begin(), pts.end());
            ++ap.next_entry;
        }
    };
    // The track running under (x, y): its index and the entry it is on, or -1.
    auto track_under = [&](float x, float y, std::size_t& entry_out) -> int {
        omsi::tt::global_tt_man().ensure_loaded_now(cfg.global.root);
        const auto& tracks = omsi::tt::global_tt_man().data.tracks;
        int best = -1;
        float best_d = 6.f;
        std::vector<std::array<float, 3>> pts;
        std::unordered_set<std::string> used; // tracks a trip runs on: bus routes
        for (const auto& trp : omsi::tt::global_tt_man().data.trips) {
            std::string n = trp.track_name;
            for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            used.insert(n);
        }
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            std::string tn = tracks[t].name;
            for (auto& c : tn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!used.empty() && !used.count(tn)) continue;
            for (std::size_t k = 0; k < tracks[t].entries.size(); ++k) {
                if (!track_entry_points(tracks[t].entries[k], nullptr, pts)) continue;
                for (const auto& q : pts) {
                    const float d = std::hypot(q[0] - x, q[1] - y);
                    if (d < best_d) {
                        best_d = d;
                        best = static_cast<int>(t);
                        entry_out = k;
                    }
                }
            }
        }
        return best;
    };
    std::function<void(OmsiBusInst&, float, const OoStatus&, float&)> ai_step_fn; // set below (AI driving)
    auto spline_index_by_id = [&](std::int32_t id) -> int {
        if (id == 0) return -1;
        for (std::size_t i = 0; i < edit_splines.size(); ++i)
            if (edit_splines[i].id == id) return static_cast<int>(i);
        return -1;
    };
    // How far along `route` (from point i0, within `reach` metres) the first AI car or towed section stands on
    // it (1e9: none). A body is on the route where a route point lies within its footprint (a circle of most of
    // its half length): a car crossing ahead or standing in the lane, not one in the next lane.
    auto traffic_gap_on_route = [&](const std::vector<std::array<float, 3>>& route, std::size_t i0, float reach) -> float {
        if (!ai_traffic || i0 + 1 >= route.size()) return 1e9f;
        static std::vector<float> buf;
        buf.resize(4 * 512);
        const std::size_t n = oo_traffic_bodies(ai_traffic, buf.data(), 512);
        if (n == 0) return 1e9f;
        const auto& o = route[i0];
        std::vector<std::size_t> close_k;
        for (std::size_t k = 0; k < n; ++k)
            if (std::hypot(buf[k * 4] - o[0], buf[k * 4 + 1] - o[1]) < reach + 12.f) close_k.push_back(k);
        if (close_k.empty()) return 1e9f;
        float acc = 0.f;
        for (std::size_t i = i0 + 1; i < route.size() && acc < reach; ++i) {
            acc += std::hypot(route[i][0] - route[i - 1][0], route[i][1] - route[i - 1][1]);
            for (const auto k : close_k) {
                const float* q = &buf[k * 4];
                const float r = (std::min)((std::max)(q[2] * 0.85f, q[3] + 0.4f), 5.f);
                const float dx = q[0] - route[i][0], dy = q[1] - route[i][1];
                if (dx * dx + dy * dy < r * r) return (std::max)(0.f, acc - r);
            }
        }
        return 1e9f;
    };
    auto run_autopilot = [&](OmsiBusInst& b, float dt) {
        auto& ap = autopilot;
        if (!ap.on || (!ap.ai && ap.track < 0 && (ap.spline < 0 || ap.spline >= static_cast<int>(edit_splines.size())))) return;
        OoStatus st{};
        oo_status(b.oo, &st);
        ap.t += dt;
        if (st.autostart_running || !st.engine_running || ap.t < 12.f) {
            oo_set_analog(b.oo, 0, 0.f, 0.f, 0.f); // the auto-start has the pedals
            ap.last_x = b.x;
            ap.last_y = b.y;
            return;
        }
        if (ap.ai) {
            if (ai_step_fn) ai_step_fn(b, dt, st, ap.log_t);
            return;
        }
        if (ap.track >= 0) {
            ap.resolve_t += dt;
            if (ap.resolve_t > 1.f) {
                ap.resolve_t = 0.f;
                extend_route();
            }
            const auto& tracks = omsi::tt::global_tt_man().data.tracks;
            if (ap.route.size() < 2) {
                oo_set_analog(b.oo, 1, 0.f, 1.f, 0.f);
                return;
            }
            // nearest point ahead of the last one (the route may cross itself)
            float best_d = 1e9f;
            const std::size_t i0 = ap.route_i, i1 = (std::min)(ap.route.size(), ap.route_i + 60);
            for (std::size_t i = i0; i < i1; ++i) {
                const float d = std::hypot(ap.route[i][0] - b.x, ap.route[i][1] - b.y);
                if (d < best_d) {
                    best_d = d;
                    ap.route_i = i;
                }
            }
            const float v = (std::max)(st.speed_kmh / 3.6f, 0.f);
            const float look = (std::max)(7.f, v * 1.3f);
            std::size_t ti = ap.route_i;
            float acc = 0.f;
            while (ti + 1 < ap.route.size() && acc < look) {
                acc += std::hypot(ap.route[ti + 1][0] - ap.route[ti][0], ap.route[ti + 1][1] - ap.route[ti][1]);
                ++ti;
            }
            const bool at_end = ti + 1 >= ap.route.size() && ap.next_entry >= tracks[static_cast<std::size_t>(ap.track)].entries.size();
            const float h = b.heading_deg * 3.14159265f / 180.f;
            const float dx = ap.route[ti][0] - b.x, dy = ap.route[ti][1] - b.y;
            const float fwd = dx * std::sin(h) + dy * std::cos(h), right = dx * std::cos(h) - dy * std::sin(h);
            const float alpha = std::atan2(right, fwd);
            const float dist_t = (std::max)(std::sqrt(dx * dx + dy * dy), 1.f);
            const float steer = std::clamp(2.f * std::sin(alpha) / dist_t / (std::max)(oo_inv_min_turn_radius(b.oo), 0.01f), -1.f, 1.f);
            // slower into bends: the curvature of the route ahead
            float target_kmh = std::fabs(steer) > 0.5f ? 15.f : (std::fabs(steer) > 0.2f ? 22.f : 35.f);
            if (at_end) target_kmh = (std::min)(target_kmh, dist_t * 2.f);
            // the AI traffic on the route ahead: keep a braking distance to it, stop 3 m behind it
            bool held = false;
            {
                const float gap = traffic_gap_on_route(ap.route, ap.route_i, 50.f) - (b.doc.bbox_half_y + b.doc.bbox_offset_y);
                if (gap < 1e8f) {
                    target_kmh = (std::min)(target_kmh, std::sqrt(2.f * 2.5f * (std::max)(gap - 3.f, 0.f)) * 3.6f);
                    held = gap < 3.5f;
                }
            }
            const float e = target_kmh - st.speed_kmh;
            const float thr = (at_end && dist_t < 4.f) || held ? 0.f : std::clamp(e * 0.08f, 0.f, 0.8f);
            const float brk = (at_end && dist_t < 4.f) || held ? 0.6f : (e < -4.f ? std::clamp(-e * 0.06f, 0.f, 0.6f) : (target_kmh < 1.f ? 0.4f : 0.f));
            oo_set_analog(b.oo, 1, thr, brk, steer);
            ap.dist += std::hypot(b.x - ap.last_x, b.y - ap.last_y);
            ap.last_x = b.x;
            ap.last_y = b.y;
            ap.err_max = (std::max)(ap.err_max, best_d);
            ap.err_sum += best_d;
            ++ap.err_n;
            ap.log_t += dt;
            if (ap.log_t >= 2.f) {
                ap.log_t = 0.f;
                log::info("track drive '" + tracks[static_cast<std::size_t>(ap.track)].name + "': v=" + std::to_string(st.speed_kmh) +
                          " km/h dist=" + std::to_string(ap.dist) + " m off_path=" + std::to_string(best_d) + " m (mean " +
                          std::to_string(ap.err_sum / (std::max)(ap.err_n, 1)) + ") entry " + std::to_string(ap.next_entry) + "/" +
                          std::to_string(tracks[static_cast<std::size_t>(ap.track)].entries.size()) + " route " +
                          std::to_string(ap.route_i) + "/" + std::to_string(ap.route.size()) + " steer=" + std::to_string(steer) +
                          " rpm=" + std::to_string(st.engine_rpm));
            }
            return;
        }
        const auto* sp = &edit_splines[static_cast<std::size_t>(ap.spline)];
        float along = 0.f, lat = 0.f;
        if (!spline::world_xy_to_spline_local(*sp, b.x, b.y, along, lat)) along = ap.forward ? 0.f : sp->length;
        const float v = (std::max)(st.speed_kmh / 3.6f, 0.f);
        const float look = (std::max)(7.f, v * 1.3f);
        float tgt_along = ap.forward ? along + look : along - look;
        // past the end of this spline: go on to the chained one
        if (tgt_along > sp->length || tgt_along < 0.f) {
            const auto& cur = *sp;
            const int nx = spline_index_by_id(ap.forward ? cur.next_id : cur.prev_id);
            const float over = ap.forward ? tgt_along - cur.length : -tgt_along;
            if (nx >= 0 && (ap.forward ? along > cur.length - 1.f : along < 1.f)) {
                const auto& nsp = edit_splines[static_cast<std::size_t>(nx)];
                ap.forward = nsp.prev_id == cur.id || nsp.next_id != cur.id;
                ap.spline = nx;
                ++ap.splines_done;
                log::info("autopilot: on spline id " + std::to_string(nsp.id) + (ap.forward ? " forward" : " reversed"));
                sp = &nsp;
                tgt_along = ap.forward ? over : nsp.length - over;
            } else {
                tgt_along = std::clamp(tgt_along, 0.f, cur.length);
            }
        }
        const auto tp = spline::evaluate_spline(*sp, std::clamp(tgt_along, 0.f, sp->length), ap.forward ? ap.lateral : -ap.lateral);
        const float h = b.heading_deg * 3.14159265f / 180.f;
        const float dx = tp.x - b.x, dy = tp.y - b.y;
        const float fwd = dx * std::sin(h) + dy * std::cos(h);
        const float right = dx * std::cos(h) - dy * std::sin(h);
        const float alpha = std::atan2(right, fwd);
        const float dist_t = (std::max)(std::sqrt(dx * dx + dy * dy), 1.f);
        const float kappa = 2.f * std::sin(alpha) / dist_t;
        const float steer = std::clamp(kappa / (std::max)(oo_inv_min_turn_radius(b.oo), 0.01f), -1.f, 1.f);
        const float target_kmh = std::fabs(steer) > 0.5f ? 15.f : 30.f;
        const float e = target_kmh - st.speed_kmh;
        const float thr = std::clamp(e * 0.08f, 0.f, 0.8f);
        const float brk = e < -4.f ? std::clamp(-e * 0.06f, 0.f, 0.6f) : 0.f;
        oo_set_analog(b.oo, 1, thr, brk, steer);
        ap.dist += std::hypot(b.x - ap.last_x, b.y - ap.last_y);
        ap.last_x = b.x;
        ap.last_y = b.y;
        const float err = std::fabs(lat - (ap.forward ? ap.lateral : -ap.lateral));
        ap.err_max = (std::max)(ap.err_max, err);
        ap.err_sum += err;
        ++ap.err_n;
        ap.log_t += dt;
        if (ap.log_t >= 2.f) {
            ap.log_t = 0.f;
            log::info("autopilot: v=" + std::to_string(st.speed_kmh) + " km/h dist=" + std::to_string(ap.dist) +
                      " m off_centre=" + std::to_string(err) + " m (max " + std::to_string(ap.err_max) + ", mean " +
                      std::to_string(ap.err_sum / (std::max)(ap.err_n, 1)) + ") steer=" + std::to_string(steer) +
                      " thr=" + std::to_string(thr) + " brk=" + std::to_string(brk) + " splines=" +
                      std::to_string(ap.splines_done) + " rpm=" + std::to_string(st.engine_rpm) +
                      " voices=" + std::to_string(st.voices));
        }
    };


    // ---- AI driving on the map's path network (OMSI / openOMSI AI traffic paths) ----------------
    // Vehicle paths ([path] typ 0) of the roads (spline lanes) and of the scenery objects (crossings),
    // linked where one ends and the next starts; the bus picks its way like an AI car, keeps the
    // paths' speed limits, slows into bends and stops at the line of a red / yellow traffic light
    // ([use_traffic_light] on a path -> the object's [trafficlights] group phase).
    struct AiPath {
        std::int32_t owner = 0;
        bool from_object = false;
        int path_index = 0;
        int dir = 0; // 0 forward only, 1 backward only, 2 both ways
        std::vector<std::array<float, 3>> pts; // the path's own order, ~1 m apart
        std::int32_t light_obj = -1;
        int light_idx = -1;
        float speed_limit = 0.f; // km/h from [pathrule] speedlimit, 0 = none
        // (for the AI traffic's lanes, as openOMSI's own map loader sets them: the junction rules need the turn -
        //  a left turn waits for the oncoming traffic - and the lanes of one spline their offsets to be neighbours)
        int turn = 0;          // object path blinker: 0 none, 1 left, 2 right
        float width = 3.f;
        float offset = 0.f;    // spline path lateral
        bool crossing_problem = false;
    };
    struct AiLane {
        int path = -1;
        bool rev = false;
    };
    struct AiNet {
        std::vector<AiPath> paths;
        std::unordered_map<std::int64_t, std::vector<AiLane>> starts; // 2 m grid of lane start points
        float cx = 1e9f, cy = 1e9f;
        std::size_t n_obj = 0, n_spl = 0;
    } ai_net;
    // computed vehicle paths per owner ((id << 1) | from_object): roads and crossings don't move while
    // driving, so a rebuild only computes owners it has not seen (it took up to 426 ms each time)
    std::unordered_map<std::int64_t, std::vector<AiPath>> ai_path_cache;
    std::size_t ai_cache_objects = 0, ai_cache_splines = 0;
    const auto ai_cell = [](float x, float y) {
        return (static_cast<std::int64_t>(static_cast<int>(std::floor(x / 2.f))) << 32) ^
               static_cast<std::uint32_t>(static_cast<int>(std::floor(y / 2.f)));
    };
    const auto lane_pt = [&](const AiLane& l, bool start) -> const std::array<float, 3>& {
        const auto& pts = ai_net.paths[static_cast<std::size_t>(l.path)].pts;
        return (start != l.rev) ? pts.front() : pts.back();
    };
    const auto rule_speed = [](const std::vector<map::PathRuleEntry>& rules, int path_index) {
        float v = 0.f;
        for (const auto& r : rules)
            if (r.path_index == path_index && !r.kill && r.keyword.size() == 10 &&
                std::equal(r.keyword.begin(), r.keyword.end(), "speedlimit",
                           [](char a, char c) { return std::tolower(static_cast<unsigned char>(a)) == c; }))
                v = r.value;
        return v;
    };
    bool ai_build_quiet = false; // (the AI traffic's lane feed calls it every few seconds: no log line)
    auto ai_build = [&](float cx, float cy) {
        AiNet old = std::move(ai_net);
        ai_net = AiNet{};
        ai_net.cx = cx;
        ai_net.cy = cy;
        ai_net.n_obj = edit_objects.size();
        ai_net.n_spl = edit_splines.size();
        constexpr float kReach = 450.f;
        if (ai_path_cache.size() > 20000) ai_path_cache.clear();
        for (const auto& sp : edit_splines) {
            if (std::hypot(sp.position.x - cx, sp.position.y - cy) > kReach + sp.length) continue;
            const std::int64_t ck = static_cast<std::int64_t>(sp.id) << 1;
            if (const auto cit = ai_path_cache.find(ck); cit != ai_path_cache.end()) {
                ai_net.paths.insert(ai_net.paths.end(), cit->second.begin(), cit->second.end());
                continue;
            }
            const std::size_t first_new = ai_net.paths.size();
            const auto def = spline::load_sli_shared(sp.sli_path);
            if (!def || def->only_editor) {
                ai_path_cache[ck] = {};
                continue;
            }
            for (std::size_t pi = 0; pi < def->paths.size(); ++pi) {
                const auto& pth = def->paths[pi];
                if (pth.typ != 0) continue;
                AiPath ap;
                ap.owner = sp.id;
                ap.path_index = static_cast<int>(pi);
                ap.dir = pth.direction;
                // (a mirrored spline's forward path runs backwards and the other way round - OMSI / openOMSI
                //  lanes.rs; the traffic drove the wrong way down every mirrored one-way carriageway)
                if (sp.mirror && ap.dir != 2) ap.dir = ap.dir == 1 ? 0 : 1;
                ap.speed_limit = rule_speed(sp.path_rules, static_cast<int>(pi));
                ap.width = pth.width > 0.1f ? pth.width : 3.f;
                ap.offset = sp.mirror ? -pth.lateral : pth.lateral;
                const int n = (std::max)(2, static_cast<int>(sp.length) + 1);
                for (int i = 0; i < n; ++i) {
                    const auto q = spline::evaluate_spline(sp, sp.length * static_cast<float>(i) / (n - 1), pth.lateral, pth.height);
                    ap.pts.push_back({q.x, q.y, q.z});
                }
                ai_net.paths.push_back(std::move(ap));
            }
            ai_path_cache[ck].assign(ai_net.paths.begin() + static_cast<std::ptrdiff_t>(first_new), ai_net.paths.end());
        }
        for (const auto& ob : edit_objects) {
            if (std::hypot(ob.position.x - cx, ob.position.y - cy) > kReach) continue;
            const std::int64_t ck = (static_cast<std::int64_t>(ob.id) << 1) | 1;
            if (const auto cit = ai_path_cache.find(ck); cit != ai_path_cache.end()) {
                ai_net.paths.insert(ai_net.paths.end(), cit->second.begin(), cit->second.end());
                continue;
            }
            const std::size_t first_new = ai_net.paths.size();
            const auto sco = sco::load_sco(ob.sco_path);
            if (!sco || sco->paths.empty()) {
                ai_path_cache[ck] = {};
                continue;
            }
            const bool abs_z = sco::skip_terrain_add(*sco);
            const float th = terrain_add(abs_z, ob.position.x, ob.position.y, ob.position.z, sco_ground_layer(ob, *sco));
            const XMMATRIX world_m = sco_world_matrix(ob, nullptr, th, 0.f, 0.f);
            for (std::size_t pi = 0; pi < sco->paths.size(); ++pi) {
                const auto& path = sco->paths[pi];
                if (path.type != 0) continue;
                map::SplineInst mini = sco::sco_path_to_spline_inst(path);
                float plen = std::abs(path.length);
                if (std::abs(path.radius) > 0.001f && plen > 0.001f && plen <= 360.f && plen / std::abs(path.radius) > 3.2f)
                    plen = std::abs(path.radius) * plen * (3.14159265f / 180.f);
                mini.length = (std::max)(0.05f, plen);
                AiPath ap;
                ap.owner = ob.id;
                ap.from_object = true;
                ap.path_index = static_cast<int>(pi);
                ap.dir = path.direction;
                ap.speed_limit = rule_speed(ob.path_rules, static_cast<int>(pi));
                ap.turn = path.blinker == 2 ? 1 : (path.blinker == 3 ? 2 : 0); // (openOMSI lanes.rs: 2 left, 3 right)
                ap.width = path.width > 0.1f ? path.width : 3.f;
                ap.crossing_problem = path.crossing_problem;
                if (path.has_use_traffic_light && path.use_traffic_light >= 0) {
                    ap.light_obj = ob.id;
                    ap.light_idx = path.use_traffic_light;
                    if (scenery_scripts.traffic_light_phase(ob.id, 0) < 0 && ob.varparent_id != 0)
                        ap.light_obj = ob.varparent_id;
                }
                const int n = (std::max)(2, static_cast<int>(mini.length) + 1);
                for (int i = 0; i < n; ++i) {
                    const auto q = spline::evaluate_spline(mini, mini.length * static_cast<float>(i) / (n - 1));
                    XMFLOAT3 w{};
                    XMStoreFloat3(&w, xform_point(world_m, q.x, q.y, q.z));
                    ap.pts.push_back({w.x, w.y, w.z});
                }
                ai_net.paths.push_back(std::move(ap));
            }
            ai_path_cache[ck].assign(ai_net.paths.begin() + static_cast<std::ptrdiff_t>(first_new), ai_net.paths.end());
        }
        if (ai_net.paths.empty() && !old.paths.empty()) {
            ai_net = std::move(old); // the map is swapping its lists while it streams: keep what we had
            ai_net.n_obj = edit_objects.size();
            ai_net.n_spl = edit_splines.size();
            return;
        }
        int lanes = 0, lights = 0, lights_live = 0;
        for (std::size_t i = 0; i < ai_net.paths.size(); ++i) {
            const auto& ap = ai_net.paths[i];
            if (ap.light_obj >= 0) {
                ++lights;
                if (scenery_scripts.traffic_light_phase(ap.light_obj, ap.light_idx) >= 0) ++lights_live;
            }
            for (const bool rev : {false, true}) {
                if ((ap.dir == 0 && rev) || (ap.dir == 1 && !rev)) continue;
                const AiLane l{static_cast<int>(i), rev};
                const auto& st = lane_pt(l, true);
                ai_net.starts[ai_cell(st[0], st[1])].push_back(l);
                ++lanes;
            }
        }
        if (!ai_build_quiet)
        log::info("ai paths: " + std::to_string(ai_net.paths.size()) + " vehicle paths, " + std::to_string(lanes) +
                  " lanes, " + std::to_string(lights) + " with traffic lights (" + std::to_string(lights_live) +
                  " running) around (" + std::to_string(cx) + "," +
                  std::to_string(cy) + ")");
    };
    // Lanes that go on from the end of `l` (start within 0.8 m, no U-turn).
    auto ai_next = [&](const AiLane& l) {
        std::vector<AiLane> out;
        const auto& e = lane_pt(l, false);
        const auto& pts = ai_net.paths[static_cast<std::size_t>(l.path)].pts;
        const auto& e2 = l.rev ? pts[(std::min)(pts.size() - 1, std::size_t{2})] : pts[pts.size() >= 3 ? pts.size() - 3 : 0];
        float hx = e[0] - e2[0], hy = e[1] - e2[1];
        const float hl = std::hypot(hx, hy);
        if (hl > 1e-4f) {
            hx /= hl;
            hy /= hl;
        }
        const int gx = static_cast<int>(std::floor(e[0] / 2.f)), gy = static_cast<int>(std::floor(e[1] / 2.f));
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const auto it = ai_net.starts.find((static_cast<std::int64_t>(gx + dx) << 32) ^ static_cast<std::uint32_t>(gy + dy));
                if (it == ai_net.starts.end()) continue;
                for (const auto& c : it->second) {
                    if (c.path == l.path) continue;
                    const auto& s0 = lane_pt(c, true);
                    if (std::hypot(s0[0] - e[0], s0[1] - e[1]) > 0.8f) continue;
                    const auto& cp = ai_net.paths[static_cast<std::size_t>(c.path)].pts;
                    const auto& s1 = c.rev ? cp[cp.size() >= 3 ? cp.size() - 3 : 0] : cp[(std::min)(cp.size() - 1, std::size_t{2})];
                    float cx2 = s1[0] - s0[0], cy2 = s1[1] - s0[1];
                    const float cl = std::hypot(cx2, cy2);
                    if (cl > 1e-4f && (cx2 * hx + cy2 * hy) / cl < 0.2f) continue;
                    out.push_back(c);
                }
            }
        return out;
    };
    // The lane under the bus going its way.
    auto ai_lane_at = [&](float x, float y, float heading_deg, std::size_t& point) -> AiLane {
        AiLane best{};
        float best_d = 5.f;
        const float h = heading_deg * 3.14159265f / 180.f;
        const float fx = std::sin(h), fy = std::cos(h);
        for (std::size_t i = 0; i < ai_net.paths.size(); ++i) {
            const auto& ap = ai_net.paths[i];
            for (std::size_t k = 0; k + 1 < ap.pts.size(); ++k) {
                const float d = std::hypot(ap.pts[k][0] - x, ap.pts[k][1] - y);
                if (d >= best_d) continue;
                const float dx = ap.pts[k + 1][0] - ap.pts[k][0], dy = ap.pts[k + 1][1] - ap.pts[k][1];
                const float dl = (std::max)(std::hypot(dx, dy), 1e-4f);
                const float along = (dx * fx + dy * fy) / dl;
                bool rev;
                if (along > 0.5f && ap.dir != 1) rev = false;
                else if (along < -0.5f && ap.dir != 0) rev = true;
                else continue;
                best_d = d;
                best = AiLane{static_cast<int>(i), rev};
                point = rev ? ap.pts.size() - 1 - k : k;
            }
        }
        return best;
    };
    std::mt19937 ai_rng{1234567u};
    struct AiDrive {
        std::vector<AiLane> plan;            // lanes appended to the route, in order
        std::vector<std::array<float, 3>> route;
        std::vector<int> route_lane;         // per route point: index into plan
        std::size_t route_i = 0;
        int light_state = -2;                // last logged light decision
        std::vector<std::size_t> lit;        // rail chunks highlighted
        int lit_from = -1;                   // plan index the highlight starts at
        std::size_t lit_plan_n = 0;          // plan size when highlighted
    } ai_drive;
    auto ai_append = [&](const AiLane& l) {
        const auto& pts = ai_net.paths[static_cast<std::size_t>(l.path)].pts;
        const int pi = static_cast<int>(ai_drive.plan.size());
        ai_drive.plan.push_back(l);
        const std::size_t n = pts.size();
        for (std::size_t k = 0; k < n; ++k) {
            if (k == 0 && !ai_drive.route.empty()) continue; // shared joint
            ai_drive.route.push_back(l.rev ? pts[n - 1 - k] : pts[k]);
            ai_drive.route_lane.push_back(pi);
        }
    };
    bool ai_trip_on_flag = false; // mirrors ai_trip.on (declared below)
    auto ai_extend = [&]() {
        if (ai_trip_on_flag) return; // a trip's route is planned stop to stop
        // keep ~250 m of route ahead
        while (!ai_drive.plan.empty() && ai_drive.route.size() < ai_drive.route_i + 250) {
            auto opts = ai_next(ai_drive.plan.back());
            if (opts.empty()) {
                static std::pair<int, bool> logged{-1, false};
                const auto& l = ai_drive.plan.back();
                if (logged != std::make_pair(l.path, l.rev)) {
                    logged = {l.path, l.rev};
                    const auto& ap = ai_net.paths[static_cast<std::size_t>(l.path)];
                    const auto& e = lane_pt(l, false);
                    float nearest = 1e9f;
                    for (const auto& [k, v] : ai_net.starts)
                        for (const auto& c : v) {
                            const auto& s0 = lane_pt(c, true);
                            if (c.path != l.path) nearest = (std::min)(nearest, std::hypot(s0[0] - e[0], s0[1] - e[1]));
                        }
                    std::string nxt;
                    if (!ap.from_object)
                        for (const auto& sp : edit_splines)
                            if (sp.id == ap.owner) {
                                const int want = l.rev ? sp.prev_id : sp.next_id;
                                bool loaded = false;
                                for (const auto& sp2 : edit_splines) loaded = loaded || sp2.id == want;
                                nxt = " chained spline " + std::to_string(want) + (loaded ? " loaded" : " NOT loaded");
                            }
                    log::info("ai: no lane goes on from " + std::string(ap.from_object ? "object " : "spline ") +
                              std::to_string(ap.owner) + " path " + std::to_string(ap.path_index) + " end (" +
                              std::to_string(e[0]) + "," + std::to_string(e[1]) + "); nearest lane start " +
                              std::to_string(nearest) + " m;" + nxt);
                }
                break;
            }
            // (a way that ends within 8 lanes - the map's edge, a yard - is taken only when nothing else is
            // left: chosen blindly, a long drive soon stood at the end of the network)
            std::function<bool(const AiLane&, int)> goes_on = [&](const AiLane& l, int depth) -> bool {
                if (depth <= 0) return true;
                for (const auto& n : ai_next(l))
                    if (goes_on(n, depth - 1)) return true;
                return false;
            };
            std::vector<AiLane> open;
            for (const auto& o : opts)
                if (goes_on(o, 8)) open.push_back(o);
            const auto& pool = open.empty() ? opts : open;
            ai_append(pool[std::uniform_int_distribution<std::size_t>(0, pool.size() - 1)(ai_rng)]);
        }
    };
    // (owner, from_object, path) -> rail chunks, rebuilt when the rails change
    std::unordered_map<std::int64_t, std::vector<std::size_t>> rails_by_path;
    std::size_t rails_by_path_n = static_cast<std::size_t>(-1);
    const auto rail_key = [](std::int32_t owner, bool from_object, int path_index) {
        return (static_cast<std::int64_t>(owner) << 17) ^ (static_cast<std::int64_t>(path_index) << 1) ^ (from_object ? 1 : 0);
    };
    auto ai_highlight = [&](bool on) {
        const int from = ai_drive.route_lane.empty() ? 0
                                                     : ai_drive.route_lane[(std::min)(ai_drive.route_i, ai_drive.route_lane.size() - 1)];
        // only when something changed: the lane the bus is on, or the plan (it used to scan every rail
        // of the map for every planned lane on every frame)
        if (on && from == ai_drive.lit_from && ai_drive.plan.size() == ai_drive.lit_plan_n && rails_by_path_n == path_rails.size())
            return;
        for (const auto ch : ai_drive.lit) renderer.set_chunk_highlight(ch, 0.f);
        ai_drive.lit.clear();
        ai_drive.lit_from = -1;
        ai_drive.lit_plan_n = 0;
        if (!on) return;
        if (rails_by_path_n != path_rails.size()) {
            rails_by_path.clear();
            for (const auto& r : path_rails)
                if (r.alive) rails_by_path[rail_key(r.owner_id, r.from_object, r.path_index)].push_back(r.chunk);
            rails_by_path_n = path_rails.size();
        }
        for (int k = from; k < static_cast<int>(ai_drive.plan.size()); ++k) {
            const auto& ap = ai_net.paths[static_cast<std::size_t>(ai_drive.plan[static_cast<std::size_t>(k)].path)];
            const auto it = rails_by_path.find(rail_key(ap.owner, ap.from_object, ap.path_index));
            if (it == rails_by_path.end()) continue;
            for (const auto ch : it->second) {
                renderer.set_chunk_highlight(ch, 0.9f, 0.1f, 1.f, 0.2f);
                ai_drive.lit.push_back(ch);
            }
        }
        ai_drive.lit_from = from;
        ai_drive.lit_plan_n = ai_drive.plan.size();
    };
    bool bus_ai_paths_visible = false;
    // ---- real routes: a timetable trip (TTData .ttp), stop by stop ------------------------------
    // Bus trips of most maps are "type 2": only the list of their stops; OMSI / openOMSI find the
    // way between consecutive stops on the path network. Here: Dijkstra over the AI lanes to the
    // lane point nearest the next stop (its object when loaded, else the centre of its tile from
    // Busstops.cfg, re-planned as the map streams in), a 10 s halt at each stop, then the next one.
    struct AiTrip {
        bool on = false;
        std::string name;
        struct Stop {
            std::int32_t id = 0;
            std::string name;
            int tile = -1;
        };
        std::vector<Stop> stops;
        std::size_t next = 0;      // stop being driven to
        float hold = 0.f;          // seconds left standing at a stop
        bool exact = false;        // the goal is the stop object itself (loaded)
        std::size_t goal_route_i = 0;
        float replan_t = 0.f;
        bool unreachable_logged = false;
    } ai_trip;
    // Where stop `st` is in editor space; exact = its object is loaded.
    auto trip_stop_position = [&](const AiTrip::Stop& st, std::array<float, 3>& out, bool& exact) -> bool {
        for (const auto& ob : edit_objects)
            if (ob.id == st.id) {
                const auto* sco = sco::load_sco(ob.sco_path);
                const float tz = (sco && sco::skip_terrain_add(*sco)) ? 0.f : sample_terrain_editor_xy(ob.position.x, ob.position.y);
                out = {ob.position.x, ob.position.y, ob.position.z + tz};
                exact = true;
                return true;
            }
        if (st.tile >= 0 && static_cast<std::size_t>(st.tile) < cfg.global.tiles.size()) {
            const auto& t = cfg.global.tiles[static_cast<std::size_t>(st.tile)];
            const auto [rx, ry] = tile_rel(t.grid_x, t.grid_y);
            out = {rx + 150.f, ry + 150.f, 0.f};
            exact = false;
            return true;
        }
        return false;
    };
    // Load trip `idx` of TTData.
    auto trip_load = [&](std::size_t idx) -> bool {
        auto& tm = omsi::tt::global_tt_man();
        tm.ensure_loaded_now(cfg.global.root);
        if (idx >= tm.data.trips.size()) return false;
        const auto& tr = tm.data.trips[idx];
        ai_trip = AiTrip{};
        ai_trip.name = tr.name;
        for (const auto& st : tr.stations) {
            AiTrip::Stop s2;
            s2.id = st.object_id;
            s2.name = st.name;
            for (const auto& bs : tm.data.busstops)
                if (bs.object_id == st.object_id) {
                    s2.tile = bs.tile_index;
                    if (s2.name.empty()) s2.name = bs.name;
                }
            ai_trip.stops.push_back(s2);
        }
        ai_trip.on = !ai_trip.stops.empty();
        return ai_trip.on;
    };
    // Dijkstra over the lanes from where the bus is to the lane point nearest the next stop.
    auto trip_route = [&](OmsiBusInst& b) -> bool {
        if (!ai_trip.on || ai_trip.next >= ai_trip.stops.size()) return false;
        std::array<float, 3> goal{};
        bool exact = false;
        if (!trip_stop_position(ai_trip.stops[ai_trip.next], goal, exact)) return false;
        if (std::hypot(b.x - ai_net.cx, b.y - ai_net.cy) > 150.f || ai_net.n_obj != edit_objects.size() ||
            ai_net.n_spl != edit_splines.size())
            ai_build(b.x, b.y);
        std::size_t start_pt = 0;
        const AiLane start = ai_lane_at(b.x, b.y, b.heading_deg, start_pt);
        if (start.path < 0) return false;
        // goal: every drivable lane passing the stop (within 15 m, or the 8 nearest when the stop is only
        // known by its tile), its point nearest the stop; the reachable one with the least cost wins
        // (the nearest lane is often the other direction's)
        struct Cand {
            AiLane lane;
            std::size_t pt = 0;
            float off = 1e9f;
        };
        std::vector<Cand> cands;
        for (std::size_t i = 0; i < ai_net.paths.size(); ++i) {
            const auto& ap = ai_net.paths[i];
            for (const bool rev : {false, true}) {
                if ((ap.dir == 0 && rev) || (ap.dir == 1 && !rev)) continue;
                Cand c{AiLane{static_cast<int>(i), rev}};
                for (std::size_t k = 0; k < ap.pts.size(); ++k) {
                    const auto& q = ap.pts[rev ? ap.pts.size() - 1 - k : k];
                    const float d = std::hypot(q[0] - goal[0], q[1] - goal[1]);
                    if (d < c.off) {
                        c.off = d;
                        c.pt = k;
                    }
                }
                cands.push_back(c);
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b2) { return a.off < b2.off; });
        if (cands.empty()) return false;
        const float goal_d = cands.front().off;
        {
            std::size_t keep = 0;
            while (keep < cands.size() && (cands[keep].off < 15.f || keep < (exact ? 1u : 8u))) ++keep;
            cands.resize((std::max<std::size_t>)(keep, 1));
        }
        const auto key = [](const AiLane& l) { return l.path * 2 + (l.rev ? 1 : 0); };
        std::unordered_map<int, float> dist;
        std::unordered_map<int, AiLane> prev;
        std::unordered_map<int, AiLane> lane_of;
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
        dist[key(start)] = 0.f;
        lane_of[key(start)] = start;
        q.push({0.f, key(start)});
        while (!q.empty()) {
            const auto [d, k] = q.top();
            q.pop();
            if (d > dist[k] || d > 6000.f) continue;
            const AiLane cur = lane_of[k];
            const float len = static_cast<float>(ai_net.paths[static_cast<std::size_t>(cur.path)].pts.size());
            for (const auto& nx : ai_next(cur)) {
                const int nk = key(nx);
                const float nd = d + len;
                if (!dist.count(nk) || nd < dist[nk]) {
                    dist[nk] = nd;
                    prev[nk] = cur;
                    lane_of[nk] = nx;
                    q.push({nd, nk});
                }
            }
        }
        AiLane goal_lane{};
        std::size_t goal_pt = 0;
        float best_cost = 1e30f;
        for (const auto& c : cands) {
            float cost;
            if (key(c.lane) == key(start) && c.pt >= start_pt) cost = static_cast<float>(c.pt - start_pt);
            else if (dist.count(key(c.lane)) && key(c.lane) != key(start)) cost = dist[key(c.lane)] + static_cast<float>(c.pt);
            else continue;
            cost += c.off * 20.f; // a lane right at the stop beats a shorter one across the road
            if (cost < best_cost) {
                best_cost = cost;
                goal_lane = c.lane;
                goal_pt = c.pt;
            }
        }
        if (goal_lane.path < 0) {
            if (!ai_trip.unreachable_logged) {
                ai_trip.unreachable_logged = true;
                log::warn("ai trip: no way on the loaded paths to stop '" + ai_trip.stops[ai_trip.next].name + "' (" +
                          std::to_string(goal_d) + " m from the nearest lane, " + std::to_string(cands.size()) + " candidates)");
            }
            return false;
        }
        ai_trip.unreachable_logged = false;
        std::vector<AiLane> seq{goal_lane};
        if (key(start) != key(goal_lane) || goal_pt < start_pt) {
            AiLane cur = goal_lane;
            while (key(cur) != key(start)) {
                cur = prev[key(cur)];
                seq.push_back(cur);
            }
            std::reverse(seq.begin(), seq.end());
        }
        ai_drive = AiDrive{};
        for (const auto& l : seq) ai_append(l);
        ai_drive.route_i = start_pt;
        // the stop on the goal lane: its point in the route
        const std::size_t goal_lane_first = ai_drive.route.size() -
                                            ai_net.paths[static_cast<std::size_t>(goal_lane.path)].pts.size() + (seq.size() > 1 ? 1 : 0);
        ai_trip.goal_route_i = (std::min)(ai_drive.route.size() - 1, goal_lane_first + goal_pt - (seq.size() > 1 ? 1 : 0));
        ai_trip.exact = exact && goal_d < 15.f;
        log::info("ai trip '" + ai_trip.name + "': to stop " + std::to_string(ai_trip.next + 1) + "/" +
                  std::to_string(ai_trip.stops.size()) + " '" + ai_trip.stops[ai_trip.next].name + "' " +
                  (ai_trip.exact ? "" : "(approximate, not streamed yet) ") + std::to_string(seq.size()) + " lanes, " +
                  std::to_string(ai_drive.route.size() - ai_drive.route_i) + " m");
        return true;
    };

    // Start AI driving from where the bus is; false when no lane runs under it.
    auto ai_start = [&](const OmsiBusInst& b) -> bool {
        if (std::hypot(b.x - ai_net.cx, b.y - ai_net.cy) > 150.f || ai_net.n_obj != edit_objects.size() ||
            ai_net.n_spl != edit_splines.size())
            ai_build(b.x, b.y);
        ai_drive = AiDrive{};
        std::size_t point = 0;
        const AiLane l = ai_lane_at(b.x, b.y, b.heading_deg, point);
        if (l.path < 0) return false;
        ai_append(l);
        ai_drive.route_i = point;
        ai_extend();
        return true;
    };
    // One frame of AI driving: analog controls into openOMSI; returns a log line every 2 s.
    struct AiPerf {
        double total = 0, highlight = 0, rebuild = 0;
        int n = 0;
    } ai_perf;
    auto ai_step_impl = [&](OmsiBusInst& b, float dt, const OoStatus& st, float& log_t) {
        // network refresh (the map streams) and re-seed from the bus
        ai_trip_on_flag = ai_trip.on;
        if (ai_trip.on) {
            if (ai_trip.next >= ai_trip.stops.size()) {
                oo_set_analog(b.oo, 1, 0.f, 0.7f, 0.f);
                return;
            }
            if (ai_trip.hold > 0.f) {
                ai_trip.hold -= dt;
                oo_set_analog(b.oo, 1, 0.f, 0.7f, 0.f);
                if (ai_trip.hold <= 0.f) {
                    ++ai_trip.next;
                    if (ai_trip.next >= ai_trip.stops.size()) log::info("ai trip '" + ai_trip.name + "': finished");
                    else (void)trip_route(b);
                }
                return;
            }
            ai_trip.replan_t += dt;
            const float rest_to_goal = static_cast<float>(ai_trip.goal_route_i) - static_cast<float>(ai_drive.route_i);
            if (ai_drive.route.size() < 2 || (ai_trip.replan_t > 3.f && (!ai_trip.exact || rest_to_goal < -5.f))) {
                ai_trip.replan_t = 0.f;
                if (!trip_route(b) && ai_drive.route.size() < 2) {
                    oo_set_analog(b.oo, 1, 0.f, 0.7f, 0.f);
                    return;
                }
            }
        }
        static float rebuild_t = 0.f;
        rebuild_t += dt;
        const bool route_ends = ai_drive.route.size() < ai_drive.route_i + 60;
        if ((std::hypot(b.x - ai_net.cx, b.y - ai_net.cy) > 200.f || ai_net.n_obj != edit_objects.size() ||
             ai_net.n_spl != edit_splines.size() || route_ends) &&
            rebuild_t > 2.f) {
            rebuild_t = 0.f;
            const auto r0 = std::chrono::steady_clock::now();
            struct RebuildTimer {
                AiPerf& p;
                std::chrono::steady_clock::time_point t0;
                ~RebuildTimer() { p.rebuild += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
            } rebuild_timer{ai_perf, r0};
            if (ai_trip.on) {
                (void)trip_route(b);
            } else if (!ai_start(b)) {
                oo_set_analog(b.oo, 1, 0.f, 0.8f, 0.f);
                return;
            }
        }
        if (ai_drive.route.size() < 2) {
            oo_set_analog(b.oo, 1, 0.f, 0.8f, 0.f);
            return;
        }
        float best_d = 1e9f;
        for (std::size_t i = ai_drive.route_i, e = (std::min)(ai_drive.route.size(), ai_drive.route_i + 40); i < e; ++i) {
            const float d = std::hypot(ai_drive.route[i][0] - b.x, ai_drive.route[i][1] - b.y);
            if (d < best_d) {
                best_d = d;
                ai_drive.route_i = i;
            }
        }
        ai_extend();
        const float v = (std::max)(st.speed_kmh / 3.6f, 0.f);
        const float look = (std::max)(6.f, v * 1.2f);
        std::size_t ti = ai_drive.route_i;
        float acc = 0.f;
        while (ti + 1 < ai_drive.route.size() && acc < look) {
            acc += std::hypot(ai_drive.route[ti + 1][0] - ai_drive.route[ti][0], ai_drive.route[ti + 1][1] - ai_drive.route[ti][1]);
            ++ti;
        }
        const float h = b.heading_deg * 3.14159265f / 180.f;
        const float dx = ai_drive.route[ti][0] - b.x, dy = ai_drive.route[ti][1] - b.y;
        const float fwd = dx * std::sin(h) + dy * std::cos(h), right = dx * std::cos(h) - dy * std::sin(h);
        const float dist_t = (std::max)(std::hypot(dx, dy), 1.f);
        const float steer = std::clamp(2.f * std::sin(std::atan2(right, fwd)) / dist_t / (std::max)(oo_inv_min_turn_radius(b.oo), 0.01f), -1.f, 1.f);
        // speed: the lane's limit (else 50), bends ahead (curvature over 40 m), lights, route end
        const auto& cur = ai_net.paths[static_cast<std::size_t>(ai_drive.plan[static_cast<std::size_t>(ai_drive.route_lane[ai_drive.route_i])].path)];
        float target = cur.speed_limit > 1.f ? cur.speed_limit : 50.f;
        {
            float turn = 0.f, run = 0.f;
            for (std::size_t i = ai_drive.route_i + 1; i + 1 < ai_drive.route.size() && run < 40.f; ++i) {
                const auto& a0 = ai_drive.route[i - 1];
                const auto& a1 = ai_drive.route[i];
                const auto& a2 = ai_drive.route[i + 1];
                const float h1 = std::atan2(a1[0] - a0[0], a1[1] - a0[1]), h2 = std::atan2(a2[0] - a1[0], a2[1] - a1[1]);
                float dh = std::fabs(h2 - h1);
                if (dh > 3.14159265f) dh = 6.2831853f - dh;
                const float seg = std::hypot(a1[0] - a0[0], a1[1] - a0[1]);
                run += seg;
                if (seg > 0.05f) turn = (std::max)(turn, dh / seg); // curvature 1/m
                // lateral acceleration ~1.6 m/s^2 at the curvature found, reached by the time we get there
                const float vmax = std::sqrt(1.6f / (std::max)(turn, 1e-4f)) * 3.6f;
                const float reach = std::sqrt(vmax / 3.6f * vmax / 3.6f + 2.f * 1.2f * run) * 3.6f;
                target = (std::min)(target, reach);
            }
        }
        if (ai_trip.on && ai_trip.exact && ai_trip.next < ai_trip.stops.size()) {
            float rest = 0.f;
            for (std::size_t i = ai_drive.route_i + 1; i <= ai_trip.goal_route_i && i < ai_drive.route.size(); ++i)
                rest += std::hypot(ai_drive.route[i][0] - ai_drive.route[i - 1][0], ai_drive.route[i][1] - ai_drive.route[i - 1][1]);
            target = (std::min)(target, std::sqrt(2.f * 1.3f * (std::max)(rest - 0.5f, 0.f)) * 3.6f);
            if (rest < 1.5f && st.speed_kmh < 3.f) {
                ai_trip.hold = 10.f;
                log::info("ai trip '" + ai_trip.name + "': at stop " + std::to_string(ai_trip.next + 1) + "/" +
                          std::to_string(ai_trip.stops.size()) + " '" + ai_trip.stops[ai_trip.next].name + "' (" +
                          std::to_string(best_d) + " m off the path)");
            }
        }
        // the end of the known route (a dead end, or the map not streamed further yet): stop there
        {
            float rest = 0.f;
            for (std::size_t i = ai_drive.route_i + 1; i < ai_drive.route.size() && rest < 80.f; ++i)
                rest += std::hypot(ai_drive.route[i][0] - ai_drive.route[i - 1][0], ai_drive.route[i][1] - ai_drive.route[i - 1][1]);
            if (rest < 80.f) target = (std::min)(target, std::sqrt(2.f * 1.5f * (std::max)(rest - 2.f, 0.f)) * 3.6f);
        }
        // the AI traffic on the route ahead: keep a braking distance to it, stop 3 m behind it
        {
            const float gap = traffic_gap_on_route(ai_drive.route, ai_drive.route_i, 50.f) - (b.doc.bbox_half_y + b.doc.bbox_offset_y);
            if (gap < 1e8f) target = (std::min)(target, std::sqrt(2.f * 2.5f * (std::max)(gap - 3.f, 0.f)) * 3.6f);
        }
        // traffic lights: the first lane ahead (within 70 m) controlled by a light
        const float front = b.doc.bbox_half_y + b.doc.bbox_offset_y; // origin -> front bumper
        float stop_at = 0.f;
        bool must_stop = false;
        std::string light_info;
        {
            float run = 0.f;
            int last_lane = ai_drive.route_lane[ai_drive.route_i];
            for (std::size_t i = ai_drive.route_i + 1; i < ai_drive.route.size() && run < 70.f; ++i) {
                run += std::hypot(ai_drive.route[i][0] - ai_drive.route[i - 1][0], ai_drive.route[i][1] - ai_drive.route[i - 1][1]);
                const int lane = ai_drive.route_lane[i];
                if (lane == last_lane) continue;
                last_lane = lane;
                const auto& ap = ai_net.paths[static_cast<std::size_t>(ai_drive.plan[static_cast<std::size_t>(lane)].path)];
                if (ap.light_obj < 0) continue;
                const int phase = scenery_scripts.traffic_light_phase(ap.light_obj, ap.light_idx);
                if (phase < 0) continue;
                const bool go = phase >= 6 && phase <= 8;
                light_info = "light obj " + std::to_string(ap.light_obj) + "/" + std::to_string(ap.light_idx) + " phase " +
                             std::to_string(phase) + " at " + std::to_string(run) + " m";
                // red / yellow: stop at the line - unless already too close to stop (yellow)
                const bool can_stop = run - front > v * v / (2.f * 3.5f);
                if (!go && (phase <= 2 || can_stop) && run - front > -2.f) {
                    must_stop = true;
                    stop_at = run - front - 1.f;
                }
                break;
            }
        }
        float brk = 0.f;
        if (must_stop) {
            const float vs = std::sqrt(2.f * 1.5f * (std::max)(stop_at, 0.f)) * 3.6f;
            target = (std::min)(target, vs);
            if (stop_at < 1.f) {
                target = 0.f;
                brk = 0.6f;
            }
        }
        const int decision = must_stop ? 1 : 0;
        if (decision != ai_drive.light_state) {
            ai_drive.light_state = decision;
            if (!light_info.empty())
                log::info(std::string("ai: ") + (decision ? "stopping for " : "going on ") + light_info);
        }
        // (test OMSI_TEST_FULL_SPEED=<km/h>: the stress drive - that speed all along, never braking for bends,
        //  traffic, lights or stops: the map streams in as fast as the bus can cross it)
        static const float s_full_speed = [] {
            const char* e = std::getenv("OMSI_TEST_FULL_SPEED");
            return e && *e ? static_cast<float>(std::atof(e)) : 0.f;
        }();
        bool full_bend_brake = false;
        if (s_full_speed > 0.f) {
            target = s_full_speed;
            brk = 0.f;
            // ...except the tight bends: the speed a bend allows (lateral 3 m/s^2, a bus at its limit), looked for as
            // far ahead as it takes to brake for it (3.5 m/s^2), and full brakes while faster than the bend allows
            float turn = 0.f, run = 0.f;
            const float look = std::clamp(v * v / (2.f * 3.5f) + 30.f, 40.f, 200.f);
            for (std::size_t i = ai_drive.route_i + 1; i + 1 < ai_drive.route.size() && run < look; ++i) {
                const auto& a0 = ai_drive.route[i - 1];
                const auto& a1 = ai_drive.route[i];
                const auto& a2 = ai_drive.route[i + 1];
                const float h1 = std::atan2(a1[0] - a0[0], a1[1] - a0[1]), h2 = std::atan2(a2[0] - a1[0], a2[1] - a1[1]);
                float dh = std::fabs(h2 - h1);
                if (dh > 3.14159265f) dh = 6.2831853f - dh;
                const float seg = std::hypot(a1[0] - a0[0], a1[1] - a0[1]);
                run += seg;
                if (seg > 0.05f) turn = (std::max)(turn, dh / seg);
                const float vmax = std::sqrt(3.f / (std::max)(turn, 1e-4f)) * 3.6f;
                const float reach = std::sqrt(vmax / 3.6f * vmax / 3.6f + 2.f * 3.5f * run) * 3.6f;
                target = (std::min)(target, reach);
            }
            if (st.speed_kmh > target + 3.f) {
                brk = 1.f;
                full_bend_brake = true;
            }
        }
        const float e = target - st.speed_kmh;
        if (target < 1.f) brk = (std::max)(brk, 0.6f); // an automatic creeps: hold it
        const float thr = brk > 0.f ? 0.f : std::clamp(e * 0.08f, 0.f, full_bend_brake ? 0.f : (s_full_speed > 0.f ? 1.f : 0.85f));
        if (brk == 0.f && e < -3.f && s_full_speed <= 0.f) brk = std::clamp(-e * 0.05f, 0.f, 0.7f);
        oo_set_analog(b.oo, 1, thr, brk, steer);
        if (bus_ai_paths_visible) {
            const auto h0 = std::chrono::steady_clock::now();
            ai_highlight(true);
            ai_perf.highlight += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - h0).count();
        }
        log_t += dt;
        if (log_t >= 2.f) {
            log_t = 0.f;
            log::info("ai drive: v=" + std::to_string(st.speed_kmh) + " target=" + std::to_string(target) + " off_path=" +
                      std::to_string(best_d) + " m lane " + std::to_string(ai_drive.route_lane[ai_drive.route_i]) + "/" +
                      std::to_string(ai_drive.plan.size()) + " (" + (cur.from_object ? "object " : "spline ") +
                      std::to_string(cur.owner) + " path " + std::to_string(cur.path_index) + ") steer=" + std::to_string(steer) +
                      " thr=" + std::to_string(thr) + " brk=" + std::to_string(brk) + (light_info.empty() ? "" : " " + light_info));
        }
    };

    auto ai_step = [&](OmsiBusInst& b, float dt, const OoStatus& st, float& log_t) {
        const auto t0 = std::chrono::steady_clock::now();
        const float log_before = log_t;
        ai_step_impl(b, dt, st, log_t);
        ai_perf.total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++ai_perf.n;
        if (log_t < log_before) { // a log line was written this frame
            log::info("ai perf: step " + std::to_string(ai_perf.total / ai_perf.n) + " ms/frame (highlight " +
                      std::to_string(ai_perf.highlight / ai_perf.n) + ", rebuilds " + std::to_string(ai_perf.rebuild / ai_perf.n) + ")");
            ai_perf = AiPerf{};
        }
    };
    ai_step_fn = ai_step;
    // openOMSI drives the bus: keys in, ground/obstacles/camera in, pose and variables out.
    auto editor_keys_live_for_bus = [&]() {
        return glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0 && !panel.keyboard_captured();
    };
    bool oo_arrow_was[4]{};
    bool oo_shift_u_was = false;
    // OMSI / openOMSI mouse driving (O): the cursor across the window is the full lock left to right,
    // divided by (km/h / 10) above 10 km/h; above the middle the throttle, below it the brake; the
    // wheel eases to the cursor for a second after switching on, then follows within ~60 ms.
    struct MouseDrive {
        bool on = false, o_was = false;
        float steer = 0.f, thr = 0.f, brk = 0.f, fade = 0.f, kmh = 0.f;
    } mouse_drive;
    std::vector<OoBox> oo_boxes;
    auto tick_omsi_bus_oo = [&](OmsiBusInst& b, float dt) {
        // (BUS_SLOW: where a slow bus frame went, one line when it takes over 6 ms)
        auto bus_lap_t = std::chrono::steady_clock::now();
        double bus_ms[7]{};
        auto bus_lap = [&](int slot) {
            const auto t = std::chrono::steady_clock::now();
            bus_ms[slot] += std::chrono::duration<double, std::milli>(t - bus_lap_t).count();
            bus_lap_t = t;
        };
        struct BusSlowLog {
            double* ms;
            float* last;
            ~BusSlowLog() {
                double tot = 0.0;
                for (int k = 0; k < 7; ++k) tot += ms[k];
                const float t = static_cast<float>(glfwGetTime());
                if (tot < 6.0 || t - *last < 0.25f) return;
                *last = t;
                char b[256];
                std::snprintf(b, sizeof(b), "BUS_SLOW total=%.2f diag=%.2f texttex=%.2f paint=%.2f mid=%.2f obst=%.2f sim=%.2f pose=%.2f",
                              tot, ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6]);
                log::warn(b);
            }
        };
        static float s_bus_slow_t = 0.f;
        BusSlowLog bus_slow_log{bus_ms, &s_bus_slow_t};
        // (diagnostics, every 5 s: what decides the headlights, the coronas and the saloon lamps)
        {
            static float s_light_log_t = 0.f;
            s_light_log_t += dt;
            if (s_light_log_t >= 5.f && b.oo) {
                s_light_log_t = 0.f;
                float lb[48 * 10];
                const std::size_t n_spot = oo_bus_lamps(b.oo, lb, 48);
                const std::size_t n_cor = oo_bus_coronas(b.oo, lb, 48);
                float ib[32 * 8];
                const std::size_t n_in = oo_bus_interior_lights(b.oo, ib, 32);
                std::string vars;
                for (const char* vn : {"Spot_Select", "lights_abbl", "lights_fern", "lights_stand", "lights_beleuchtung_vorn_1",
                                       "elec_busbar_main", "elec_busbar_av", "engine_on", "bat_switch"})
                    vars += std::string(" ") + vn + "=" + std::to_string(bus_var(b, vn, -99.f));
                log::info("omsi bus lights:" + vars + " | headlight lamps=" + std::to_string(n_spot) +
                          " coronas=" + std::to_string(n_cor) + " interior=" + std::to_string(n_in));
            }
        }
        bus_lap(0);
        // displays: the script's strings drawn in their .oft fonts on a worker, up when baked (5 times a second)
        b.texttex_t += dt;
        if (b.oo && !b.texttex.empty() && b.texttex_t >= 0.2f) {
            b.texttex_t = 0.f;
            int submitted = 0;
            for (auto& e : b.texttex) {
                if (e.job && e.job->done.load(std::memory_order_acquire)) {
                    const auto& img = e.job->img;
                    if (img && !img->rgba.empty()) {
                        renderer.replace_chunk_albedo_rgba(e.chunk, img->rgba.data(), img->width, img->height, e.ti);
                    } else {
                        // (no text: the display shows nothing, as Omsi.exe's empty TOFTTex)
                        const std::uint8_t clear[16]{};
                        renderer.replace_chunk_albedo_rgba(e.chunk, clear, 2, 2, e.ti);
                    }
                    e.shown = e.job->text;
                    e.ever = true;
                    e.job.reset();
                }
                if (e.job || submitted >= 4) continue;
                char tb[1024];
                const std::size_t tn = oo_bus_texttex_string(b.oo, e.field.c_str(), tb, sizeof(tb));
                std::string text(tb, (std::min)(tn, sizeof(tb) - 1));
                if (e.ever && text == e.shown) continue;
                auto job = std::make_shared<BusTextBake>();
                job->text = text;
                e.job = job;
                ++submitted;
                omsi::JobSystem::global().submit(
                    [job, tt = e.tt, root = cfg.omsi_root]() {
                        try {
                            if (!job->text.empty()) job->img = sco::rasterize_text_texture(tt, job->text, root);
                        } catch (...) {
                        }
                        job->done.store(true, std::memory_order_release);
                    },
                    omsi::JobSystem::Priority::Low);
            }
        }
        bus_lap(1);
        // paint: the scheme's textures decode on a worker; a slot shows its own until then
        for (auto pit = b.paint.begin(); pit != b.paint.end();) {
            const auto& [pch, key, file] = *pit;
            renderer.set_chunk_freetex_variant(pch, key, file);
            if (renderer.chunk_freetex_variant_ready(pch, key, file)) pit = b.paint.erase(pit);
            else ++pit;
        }
        const bool drive = bus_drive_active;
        static const char* kArrowAction[4] = {"throttle", "brake", "steering_left", "steering_right"};
        static const int kArrowKey[4] = {GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_LEFT, GLFW_KEY_RIGHT};
        bus_lap(2);
        if (drive) {
            bus_camera_keys();
            bus_inputs.update(window, dt);
            for (const auto& t : bus_inputs.pressed_triggers()) {
                log::info("omsi bus key press " + t);
                oo_key(b.oo, t.c_str(), 1);
            }
            for (auto t : bus_inputs.released_triggers()) {
                if (t.size() > 4 && t.compare(t.size() - 4, 4, "_off") == 0) t.resize(t.size() - 4);
                oo_key(b.oo, t.c_str(), 0);
            }
            for (int i = 0; i < 4; ++i) {
                const bool down = glfwGetKey(window, kArrowKey[i]) == GLFW_PRESS;
                if (down != oo_arrow_was[i]) oo_key(b.oo, kArrowAction[i], down ? 1 : 0);
                oo_arrow_was[i] = down;
            }
            const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                               glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
            const bool shift_u = shift && glfwGetKey(window, GLFW_KEY_U) == GLFW_PRESS;
            if (shift_u && !oo_shift_u_was) {
                log::info("omsi bus: auto-start (Shift+U)");
                oo_autostart(b.oo);
                panel.set_status("OMSI bus: arranque automatico (Shift+U)...");
            }
            oo_shift_u_was = shift_u;
            // The cab's [mouseevent]s (buttons, switches, door taps): a click on such a mesh fires its trigger and
            // letting go its `_off`, as Omsi.exe does - the same path as a key (oo_key). Before, a click in the cab
            // reached nothing (the editor's FFI had no way to them).
            {
                static bool s_me_lmb_was = false;
                static std::string s_me_held;
                const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                if (lmb && !s_me_lmb_was && !ImGui::GetIO().WantCaptureMouse && b.oo) {
                    static std::vector<std::size_t> s_cab;
                    s_cab.clear();
                    for (const auto ch : b.chunks)
                        if (ch != kNoChunk) s_cab.push_back(ch);
                    for (const auto ch : b.sec_chunks)
                        if (ch != kNoChunk) s_cab.push_back(ch);
                    float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
                    input::cursor_to_framebuffer(window, mx, my, fw, fh);
                    float wvp[16]{};
                    camera.build_world_view_proj(fh > 0.f ? fw / fh : 1.6f, wvp);
                    if (!s_cab.empty())
                        if (const auto hit = renderer.pick_chunk(mx, my, static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh),
                                                                 wvp, true, s_cab.data(), s_cab.size()))
                            if (const auto me = renderer.chunk_mouseevent(*hit); me && !me->empty()) {
                                s_me_held = *me;
                                log::info("omsi bus mouseevent " + s_me_held);
                                oo_key(b.oo, s_me_held.c_str(), 1);
                            }
                } else if (!lmb && s_me_lmb_was && !s_me_held.empty()) {
                    if (b.oo) oo_key(b.oo, s_me_held.c_str(), 0);
                    s_me_held.clear();
                }
                s_me_lmb_was = lmb;
            }
            const bool o_now = editor_keys_live_for_bus() && glfwGetKey(window, GLFW_KEY_O) == GLFW_PRESS;
            if (o_now && !mouse_drive.o_was) {
                mouse_drive.on = !mouse_drive.on;
                mouse_drive.fade = 1.f;
                log::info(std::string("omsi bus: mouse driving ") + (mouse_drive.on ? "ON" : "OFF"));
                panel.set_status(mouse_drive.on ? "OMSI bus: manejo con mouse (O) - cursor: izq/der direccion, arriba acelera, abajo frena"
                                                : "OMSI bus: manejo con teclado");
                if (!mouse_drive.on) oo_set_analog(b.oo, 0, 0.f, 0.f, 0.f);
            }
            mouse_drive.o_was = o_now;
            static bool p_was = false;
            const bool p_now = editor_keys_live_for_bus() && glfwGetKey(window, GLFW_KEY_P) == GLFW_PRESS;
            const bool i_now = editor_keys_live_for_bus() && glfwGetKey(window, GLFW_KEY_I) == GLFW_PRESS;
            static bool i_was = false;
            if (i_now && !i_was) {
                bus_ai_paths_visible = !bus_ai_paths_visible;
                if (!bus_ai_paths_visible) ai_highlight(false);
                panel.set_status(bus_ai_paths_visible ? "OMSI bus: paths de IA visibles (I)" : "OMSI bus: paths de IA ocultos (I)");
            }
            i_was = i_now;
            if (p_now && !p_was && !shift && !autopilot.on) {
                if (ai_start(b)) {
                    autopilot = BusAutopilot{};
                    autopilot.on = true;
                    autopilot.ai = true;
                    autopilot.t = 100.f;
                    panel.set_status("OMSI bus: maneja como la IA por los paths (P para soltar, I muestra los paths)");
                    log::info("ai drive: on from lane " + std::to_string(ai_drive.plan.front().path));
                } else {
                    panel.set_status("OMSI bus: no hay ningun path de vehiculos bajo el bus");
                    log::warn("ai drive: no vehicle lane under the bus");
                }
            } else if (p_now && !p_was && (shift || autopilot.on)) {
                if (autopilot.on) {
                    ai_highlight(false);
                    autopilot = BusAutopilot{};
                    oo_set_analog(b.oo, 0, 0.f, 0.f, 0.f);
                    panel.set_status("OMSI bus: piloto de track apagado (P)");
                    log::info("track drive: off");
                } else {
                    std::size_t entry = 0;
                    const int t = track_under(b.x, b.y, entry);
                    if (t < 0) {
                        panel.set_status("OMSI bus: no hay ningun track (TTData) bajo el bus");
                        log::warn("track drive: no track within 6 m of the bus");
                    } else {
                        autopilot = BusAutopilot{};
                        autopilot.on = true;
                        autopilot.track = t;
                        autopilot.next_entry = entry;
                        autopilot.t = 100.f; // no start-up wait: the bus is running
                        extend_route();
                        const auto& name = omsi::tt::global_tt_man().data.tracks[static_cast<std::size_t>(t)].name;
                        panel.set_status("OMSI bus: siguiendo el track " + name + " (P para soltar)");
                        log::info("track drive: following '" + name + "' from entry " + std::to_string(entry) + ", route " +
                                  std::to_string(autopilot.route.size()) + " points");
                    }
                }
            }
            p_was = p_now;
            if (mouse_drive.on && !autopilot.on) {
                double cx = 0.0, cy = 0.0;
                int w = 1, h = 1;
                glfwGetCursorPos(window, &cx, &cy);
                glfwGetWindowSize(window, &w, &h);
                OoStatus mst{};
                oo_status(b.oo, &mst);
                mouse_drive.kmh += (std::fabs(mst.speed_kmh) - mouse_drive.kmh) * (1.f - std::exp(-dt / 0.4f));
                const float target = std::clamp(
                    (2.f * static_cast<float>(cx) / (std::max)(w, 1) - 1.f) / (std::max)(mouse_drive.kmh / 10.f, 1.f), -1.f, 1.f);
                const float y = std::clamp(2.f * static_cast<float>(cy) / (std::max)(h, 1) - 1.f, -1.f, 1.f);
                const float pt = (std::min)((std::max)(-y, 0.f), 1.f), pb = (std::min)((std::max)(y, 0.f), 1.f);
                const float k = mouse_drive.fade > 0.f ? std::exp(-0.6931472f / mouse_drive.fade * dt) : std::exp(-dt / 0.06f);
                mouse_drive.steer = target + (mouse_drive.steer - target) * k;
                mouse_drive.thr = pt + (mouse_drive.thr - pt) * k;
                mouse_drive.brk = pb + (mouse_drive.brk - pb) * k;
                if (std::fabs(mouse_drive.thr - pt) < 1e-4f) mouse_drive.thr = pt;
                if (std::fabs(mouse_drive.brk - pb) < 1e-4f) mouse_drive.brk = pb;
                mouse_drive.fade = (std::max)(mouse_drive.fade - dt, 0.f);
                oo_set_analog(b.oo, 1, mouse_drive.thr, mouse_drive.brk, mouse_drive.steer);
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                if (oo_arrow_was[i]) oo_key(b.oo, kArrowAction[i], 0);
                oo_arrow_was[i] = false;
            }
        }

        bus_lap(3);
        // obstacles: gathered again only after the bus moved 1 m / turned 3 deg, or each 2 s
        b.obst_age += dt;
        if (std::hypot(b.x - b.obst_x, b.y - b.obst_y) > 1.f || std::fabs(b.heading_deg - b.obst_h) > 3.f ||
            b.obst_age > 2.f) {
            gather_bus_surfaces(b);
            static const bool no_obstacles = std::getenv("OMSI_BUS_NO_OBSTACLES") != nullptr;
            if (no_obstacles) bus_obstacles.clear();
            else gather_bus_obstacles(b);
            oo_boxes.clear();
            for (const auto& o : bus_obstacles)
                {
                OoBox ob{o.center.x, o.center.y, o.half.x, o.half.y, o.heading, o.z0, o.z1, o.id, 0.f, 0.f};
                if (const auto pit = bus_poles.find(static_cast<std::int32_t>(o.id)); pit != bus_poles.end()) {
                    ob.pole_mass_t = pit->second.first;
                    ob.pole_load_kn = pit->second.second;
                }
                oo_boxes.push_back(ob);
            }
            oo_set_obstacles(b.oo, oo_boxes.data(), oo_boxes.size());
            b.obst_x = b.x;
            b.obst_y = b.y;
            b.obst_h = b.heading_deg;
            b.obst_age = 0.f;
        }

        float ex = 0.f, ey = 0.f, ez = 0.f;
        camera.eye_position(ex, ey, ez);
        float fx = camera.target_x - ex, fy = camera.target_y - ey, fz = camera.target_z - ez;
        if (std::fabs(fx) + std::fabs(fy) + std::fabs(fz) < 1e-4f) fy = 1.f;
        b.oo_ground_hint = b.z + 3.f;
        if (b.waiting_stream) {
            // the tiles around the spawn are still coming in: the bus waits (ground, objects, paths)
            static float wait_t = 0.f;
            wait_t += dt;
            // (8 s at most: with a wide view radius the streamer is hardly ever idle, and held 30 s the bus
            //  looked dead - no start-up, no throttle; the tiles at the spawn come first and are in by then)
            if (!stream_busy() || wait_t > 8.f) {
                b.waiting_stream = false;
                b.ground_cache.clear();
                b.obst_age = 1e9f;
                ai_net.cx = 1e9f;
                log::info("omsi bus: surroundings loaded after " + std::to_string(wait_t) + " s");
                wait_t = 0.f;
                panel.set_status("OMSI bus: listo");
            } else {
                panel.set_status("OMSI bus: cargando el mapa alrededor...");
                update_bus_pose(b, dt);
                if (drive) apply_bus_camera(b);
                return;
            }
        }
        b.age += dt;
        {
            // diagnostics: openOMSI's sound report every 10 s; a sudden stop (ground step or obstacle)
            static float report_t = 0.f, last_kmh = 0.f, stuck_t = 0.f;
            report_t += dt;
            if (report_t > 10.f) {
                report_t = 0.f;
                oo_sound_report(b.oo);
            }
            OoStatus st{};
            oo_status(b.oo, &st);
            const float h = b.heading_deg * 3.14159265f / 180.f;
            const auto ground_ahead = [&]() {
                std::string prof = "ground ahead (m: z):";
                for (float d = 0.f; d <= 12.f; d += 1.f) {
                    const float gx = b.x + std::sin(h) * (d + 4.f), gy = b.y + std::cos(h) * (d + 4.f);
                    prof += " " + std::to_string(static_cast<int>(d)) + ":" + std::to_string(b.ground_fn(gx, gy, b.z + 3.f)).substr(0, 6);
                }
                return prof;
            };
            if (last_kmh > 12.f && st.speed_kmh < last_kmh - 8.f)
                log::warn("omsi bus: sudden stop " + std::to_string(last_kmh) + " -> " + std::to_string(st.speed_kmh) +
                          " km/h at (" + std::to_string(b.x) + "," + std::to_string(b.y) + "," + std::to_string(b.z) + "); " +
                          ground_ahead() + "; obstacles near: " + std::to_string(oo_boxes.size()));
            if (last_kmh > 12.f && st.speed_kmh < last_kmh - 8.f) oo_debug_wheels(b.oo);
            last_kmh = st.speed_kmh;
            stuck_t = ((st.throttle > 0.5f || (autopilot.on && b.age > 25.f)) && std::fabs(st.speed_kmh) < 0.5f && st.engine_running)
                          ? stuck_t + dt
                          : 0.f;
            if (stuck_t > 3.f) {
                oo_debug_wheels(b.oo);
                stuck_t = -1000.f; // once
                std::string touching = "omsi bus: stuck with throttle; " + ground_ahead() + "; touching:";
                const float bbb[6] = {b.doc.bbox_half_x * 2.f + 0.6f, b.doc.bbox_half_y * 2.f + 0.6f, b.doc.bbox_half_z * 2.f, 0.f,
                                      b.doc.bbox_offset_y, b.doc.bbox_half_z};
                const auto me = omsi::vehicle::Obb::from_box(bbb, omsi::vehicle::Vec3d{b.x, b.y, b.z}, b.heading_deg);
                int shown = 0;
                for (const auto& o : bus_obstacles) {
                    if (!me.overlaps(o) || shown >= 8) continue;
                    ++shown;
                    std::string name = "?";
                    for (const auto& eo : edit_objects)
                        if (eo.id == o.id) {
                            name = omsi::path_text(eo.sco_path.filename());
                            break;
                        }
                    touching += " [" + name + " id=" + std::to_string(o.id) + " half=" + std::to_string(o.half.x).substr(0, 5) + "x" +
                            std::to_string(o.half.y).substr(0, 5) + " z=" + std::to_string(o.z0).substr(0, 6) + ".." +
                            std::to_string(o.z1).substr(0, 6) + "]";
                }
                log::warn(touching);
            }
        }
        if (!b.session_checked && b.age > 2.f) {
            b.session_checked = true;
            (void)omsi::sound::restore_app_session_volume();
        }
        run_autopilot(b, dt);
        bus_lap(4);
        // real time in fixed steps of at most 1/30 s (openOMSI's frame), so a slow editor frame does
        // not slow the bus down; at most 6 steps a frame
        b.oo_time_acc = (std::min)(b.oo_time_acc + dt, 0.2f);
        int steps = 0;
        const auto tick_t0 = std::chrono::steady_clock::now();
        while (b.oo_time_acc > 1e-4f && steps < 6) {
            const float h = (std::min)(b.oo_time_acc, 1.f / 30.f);
            oo_tick(b.oo, h, ex, ey, ez, fx, fy, fz);
            b.oo_time_acc -= h;
            ++steps;
        }
        static float tick_ms_max = 0.f;
        tick_ms_max = (std::max)(tick_ms_max, std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - tick_t0).count());

        double pose[6]{};
        oo_pose(b.oo, pose);
        oo_body_matrix(b.oo, b.oo_body);
        b.x = static_cast<float>(pose[0]);
        b.y = static_cast<float>(pose[1]);
        b.z = static_cast<float>(pose[2]);
        b.heading_deg = static_cast<float>(pose[3]);
        b.ground_z = bus_ground(b.x, b.y, b.z + 3.f);

        {
            static float state_log_t = 0.f;
            static int running_was = -1;
            static int autostart_was = -1;
            state_log_t += dt;
            OoStatus st{};
            oo_status(b.oo, &st);
            if (state_log_t >= 2.f || st.engine_running != running_was || st.autostart_running != autostart_was) {
                state_log_t = 0.f;
                log::info("omsi bus state running=" + std::to_string(st.engine_running) + " rpm=" +
                          std::to_string(st.engine_rpm) + " v_kmh=" + std::to_string(st.speed_kmh) +
                          " power=" + std::to_string(st.power_on) + " autostart=" + std::to_string(st.autostart_running) +
                          " thr=" + std::to_string(st.throttle) + " brk=" + std::to_string(st.brake) +
                          " steer=" + std::to_string(st.steering) + " voices=" + std::to_string(st.voices) +
                          " audio=" + std::to_string(st.audio_ok) + " sim_ms_max=" + std::to_string(tick_ms_max) +
                          " ground_cache=" + std::to_string(b.ground_cache.size()) + " obstacles=" +
                          std::to_string(oo_boxes.size()) + " bus=(" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
                          std::to_string(b.z) + ") ear=(" + std::to_string(ex) + "," + std::to_string(ey) + "," +
                          std::to_string(ez) + ")");
                tick_ms_max = 0.f;
                if (drive && st.autostart_running == 0 && autostart_was == 1)
                    panel.set_status(st.engine_running ? "OMSI bus: motor en marcha - Num8/arriba para acelerar"
                                                       : "OMSI bus: no arranco - Shift+U para reintentar");
            }
            running_was = st.engine_running;
            autostart_was = st.autostart_running;
        }

        bus_lap(5);
        b.anim.update(b.model, dt, [&](const std::string& name) { return bus_var(b, name, 0.f); });
        update_bus_pose(b, dt);
        if (drive) apply_bus_camera(b);
        bus_lap(6);
    };

    // Bus spawn in two steps: its model and every coupled section's (.o3d, textures) are read and decoded on a
    // worker, then the bus is put up from the warm caches - read at once on the main thread, the HH AGG300
    // froze the editor for seconds.
    struct PendingBusSpawn {
        std::string file;
        std::shared_ptr<std::atomic<bool>> done;
        std::shared_ptr<std::vector<omsi::model_cfg::ModelCfgDocument>> models; // the train's parts, read on the worker
        std::size_t model_i = 0, mesh_i = 0;                                   // upload progress
        std::array<float, 4> pose{};                                           // x, y, ground z, heading
        std::shared_ptr<std::atomic<OoBus*>> oo;                               // made on the worker
    };
    std::optional<PendingBusSpawn> pending_bus_spawn;
    auto request_spawn_omsi_bus = [&](std::string bus_file) {
        if (bus_file.empty() || cfg.omsi_root.empty()) {
            try_spawn_omsi_bus(bus_file);
            return;
        }
        if (pending_bus_spawn && pending_bus_spawn->file == bus_file) return;
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto models = std::make_shared<std::vector<omsi::model_cfg::ModelCfgDocument>>();
        // where it goes, decided now; openOMSI's vehicle (its type, coupled parts, scripts) is made on the
        // worker too - oo_spawn took half a second of the main thread for the AGG300
        std::array<float, 4> pose{};
        bus_spawn_pose(pose[0], pose[1], pose[3]);
        pose[2] = bus_ground(pose[0], pose[1], camera.target_z + 50.f);
        auto pre_oo = std::make_shared<std::atomic<OoBus*>>(nullptr);
        const bool with_oo = oo_ready();
        panel.set_status("OMSI bus: cargando " + omsi::path_text(std::filesystem::u8path(bus_file).filename()) + "...");
        omsi::JobSystem::global().submit(
            [bus_file, done, models, pose, pre_oo, with_oo, &prefetch_model_assets]() {
                try {
                    std::filesystem::path p = std::filesystem::u8path(bus_file);
                    for (int part = 0; part < 6 && !p.empty(); ++part) {
                        const auto doc = omsi::bus::load_bus(p);
                        if (!doc) break;
                        if (auto model = omsi::model_cfg::load_model_cfg(omsi::bus::resolve_model_cfg_path(*doc))) {
                            prefetch_model_assets(*model);
                            models->push_back(std::move(*model));
                        }
                        std::filesystem::path next;
                        const auto lines = cfg::read_cfg_lines(p);
                        for (std::size_t i = 0; i + 1 < lines.size() && next.empty(); ++i) {
                            std::string t = lines[i];
                            while (!t.empty() && (t.back() == '\r' || t.back() == ' ' || t.back() == '	')) t.pop_back();
                            if (t != "[couple_back]") continue;
                            for (std::size_t j = i + 1; j < lines.size(); ++j) {
                                std::string f = lines[j];
                                while (!f.empty() && (f.back() == '\r' || f.back() == ' ' || f.back() == '	')) f.pop_back();
                                if (f.empty()) continue;
                                next = p.parent_path() / f;
                                break;
                            }
                        }
                        p = next;
                    }
                    if (with_oo) pre_oo->store(oo_spawn(bus_file.c_str(), pose[0], pose[1], pose[2], pose[3]));
                } catch (...) {
                }
                done->store(true, std::memory_order_release);
            },
            omsi::JobSystem::Priority::High);
        pending_bus_spawn = PendingBusSpawn{bus_file, done, models, 0, 0, pose, pre_oo};
    };
    auto tick_omsi_bus = [&](float dt) {
        if (pending_bus_spawn && pending_bus_spawn->done->load(std::memory_order_acquire)) {
            // its meshes go up 5 ms a frame (all at once the AGG300 held one frame for over a second)
            auto& pb = *pending_bus_spawn;
            const auto up_t0 = std::chrono::steady_clock::now();
            while (pb.model_i < pb.models->size()) {
                const auto& m = (*pb.models)[pb.model_i];
                const auto* plod = omsi::model_cfg::primary_lod(m);
                if (!plod || pb.mesh_i >= plod->meshes.size()) {
                    ++pb.model_i;
                    pb.mesh_i = 0;
                    continue;
                }
                if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - up_t0).count() > 5.0) break;
                const std::size_t mi = pb.mesh_i++;
                const auto& mesh = plod->meshes[mi];
                if (mesh.mesh_file.empty()) continue;
                const auto key = bus_pre_key(m.cfg_path, mi);
                if (bus_preuploaded.count(key)) continue;
                const auto mapped = o3d::load_mapped_shared(m.cfg_path.parent_path() / mesh.mesh_file);
                if (!mapped || !mapped->valid() || !renderer.append_o3d(*mapped, &mesh)) continue;
                const std::size_t ch = renderer.chunk_count() - 1;
                renderer.set_chunk_visible(ch, false);
                // (one-sided, depth-written alpha 2 in file order, as Omsi.exe draws a vehicle: CULLMODE stays
                //  CCW for meshes, ZWRITEENABLE = !noZwrite - see model_cfg_loader apply_omsi_material_state)
                bus_preuploaded.emplace(key, ch);
            }
            if (pb.model_i >= pb.models->size()) {
                const std::string f = pb.file;
                bus_pre_pose = pb.pose;
                bus_pre_oo = pb.oo->exchange(nullptr);
                pending_bus_spawn.reset();
                try_spawn_omsi_bus(f);
                if (bus_pre_oo) oo_despawn(bus_pre_oo); // (try_spawn gave up before using it)
                bus_pre_oo = nullptr;
                bus_pre_pose.reset();
                for (const auto& [k, ch] : bus_preuploaded) renderer.release_chunk_gpu(ch); // (not used by it)
                bus_preuploaded.clear();
            }
        }
        if (!omsi_bus) return;
        auto& b = *omsi_bus;
        if (b.oo) {
            tick_omsi_bus_oo(b, std::clamp(dt, 0.f, 0.25f));
            return;
        }
        dt = std::clamp(dt, 0.f, 0.05f); // VehicleRuntime clamps the same way
        const bool drive = bus_drive_active;

        if (drive) {
            // OMSI's own keys come from Inputs/keyboard.cfg (Num8/Num2 pedals, Num4/5/6 steering, D/N/R
            // gearbox, '.' parking brake). W/A/S/D are OMSI functions (wipers, view, drive) and are
            // NOT pedals; the arrow keys are a free extra (OMSI leaves Up/Down unbound).
            const bool k_w = glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS;
            const bool k_s = glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;
            const bool k_a = glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
            const bool k_d = glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
            const bool f1 = glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS;
            const bool f2 = glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS;
            const bool f3 = glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS;
            const bool f4 = glfwGetKey(window, GLFW_KEY_F4) == GLFW_PRESS;
            if (f1 && !bus_f1_was) { bus_cam_mode = 1; bus_cam_need_snap = true; }
            if (f2 && !bus_f2_was) { bus_cam_mode = 2; bus_cam_need_snap = true; }
            if (f3 && !bus_f3_was) { bus_cam_mode = 3; bus_cam_need_snap = true; }
            if (f4 && !bus_f4_was) { bus_cam_mode = 4; bus_cam_need_snap = false; }
            bus_f1_was = f1;
            bus_f2_was = f2;
            bus_f3_was = f3;
            bus_f4_was = f4;

            bus_inputs.update(window, dt);
            const bool thr = k_w || bus_inputs.held("throttle");
            const bool brk = k_s || bus_inputs.held("brake");
            const bool left = k_a || bus_inputs.held("steering_left");
            const bool right = k_d || bus_inputs.held("steering_right");
            b.rt->set_driver_input(thr ? 1.f : 0.f, brk ? 1.f : 0.f);
            b.rt->set_steering_keys(left, right, !left && !right);
            {
                const auto pressed = bus_inputs.pressed_triggers();
                const auto released = bus_inputs.released_triggers();
                for (const auto& t : pressed) log::info("omsi bus trigger press " + t);
                for (const auto& t : released) log::info("omsi bus trigger release " + t);
                b.rt->apply_triggers(pressed, released);
            }
        } else {
            b.rt->set_driver_input(0.f, 1.f);
            b.rt->set_steering_keys(false, false, true);
        }

        b.rt->tick(dt);
        {
            // Periodic state line (2 s) plus an immediate line on engine start/stop.
            static float state_log_t = 0.f;
            static int engine_on_was = -1;
            state_log_t += dt;
            const int engine_on = static_cast<int>(std::lround(b.rt->public_float("engine_on", 0.f)));
            if (drive && (state_log_t >= 2.f || engine_on != engine_on_was)) {
                state_log_t = 0.f;
                log::info("omsi bus state engine_on=" + std::to_string(engine_on) +
                          " n=" + std::to_string(b.rt->public_float("engine_n", 0.f)) +
                          " v_kmh=" + std::to_string(b.rt->public_float("Velocity", 0.f)) +
                          " throttle=" + std::to_string(b.rt->public_float("throttle", 0.f)) +
                          " batt=" + std::to_string(b.rt->public_float("elec_busbar_main", 0.f)));
            }
            engine_on_was = engine_on;
        }

        // Rigid body (springs, pitch/roll, tyre grip, yaw) from the scripts' wheel torque and brakes.
        {
            const auto& cf = b.rt->chassis_forces();
            omsi::vehicle::BusBodyPhysics::Inputs in;
            in.drive_force_n = cf.drive_n;
            in.brake_force_n = cf.brake_n;
            in.rolling_force_n = cf.rolling_n;
            in.inv_turn_radius = b.rt->inv_lenkradius();
            for (int i = 0; i < 8; ++i) {
                const float f = b.rt->public_float("Axle_Springfactor_" + std::to_string(i / 2) + "_" +
                                                       (i % 2 == 0 ? "L" : "R"),
                                                   1.f);
                in.spring_factor[i] = f > 0.05f ? f : 1.f;
            }
            b.body.step(dt, in, [&](float gx, float gy, float zh) { return bus_height(gx, gy, zh); });
            gather_bus_obstacles(b);
            for (const auto& im : b.body.collide(bus_obstacles, dt)) {
                if (im.speed > 0.4f)
                    log::info("omsi bus collision obj=" + std::to_string(im.obstacle_id) + " speed=" +
                              std::to_string(im.speed * 3.6f) + " km/h energy=" + std::to_string(im.energy) + " J");
            }
            float comp[8]{};
            const auto& ws = b.body.wheels();
            for (std::size_t i = 0; i < ws.size() && i < 8; ++i) comp[i] = ws[i].compression;
            b.rt->set_external_motion(b.body.speed_ms() * 3.6f, b.body.accel_long(), comp,
                                      static_cast<int>((std::min)(ws.size(), std::size_t{8})));
            b.x = b.body.x();
            b.y = b.body.y();
            b.z = b.body.z();
            b.heading_deg = b.body.heading_deg();
            b.pitch_deg = b.body.pitch_deg();
            b.roll_deg = b.body.roll_deg();
            float gz = 0.f;
            for (const auto& w : ws) gz += w.ground_z;
            b.ground_z = ws.empty() ? b.z : gz / static_cast<float>(ws.size());
        }

        b.anim.update(b.model, dt, [&](const std::string& name) { return b.rt->public_float(name, 0.f); });
        update_bus_pose(b, dt);
        if (drive) apply_bus_camera(b);
    };
