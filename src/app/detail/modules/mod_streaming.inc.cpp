// ============================================================================
// MOD: streaming - aislado (tile stream diff/merge/GPU shift/upload).
// Motor y estado compartido (world, renderer, edit_*). Editar solo aqui.
// ============================================================================
    std::unique_ptr<world::AsyncWorldLoader> stream_job;
    std::optional<map::EntryPoint> stream_queued_focus;
    bool stream_queued_force = false; // true when user teleported — may cancel in-flight load
    std::int32_t stream_focus_gx = 0;
    std::int32_t stream_focus_gy = 0;
    float stream_focus_wx = 0.f;
    float stream_focus_wy = 0.f;
    bool stream_focus_valid = false; // false until first begin_stream_diff — skip deadzone check
    // Tiles outside the unload ring needed before a pure-drain diff runs (and max removed per drain diff).
    constexpr std::size_t kStreamDrainBatch = 8;
    // Prefetch: cam + velocity * look_ahead (0.6–1.0 s). EWMA of focus motion.
    float stream_vel_x = 0.f;
    float stream_vel_y = 0.f;
    float stream_vel_last_wx = 0.f;
    float stream_vel_last_wy = 0.f;
    bool stream_vel_init = false;
    std::chrono::steady_clock::time_point stream_vel_last_t{};
    enum class StreamPhase {
        Idle,
        Loading,
        PersistTiles,   // disk flush (only doomed tiles) â€” may span frames via worker
        PrepShiftMerge, // unload+merge origin (light)
        PrepShiftGpu,   // budgeted GPU chunk shifts
        PrepLists,      // budgeted substeps (erase/merge/index/terrain) — avoids UI freeze
        UploadMeshes,
        UploadTerrain,
        UploadScenery, // trees / .x-only SCOs / resync Z after terrain heights
        UploadSplines,
        Finish // substeps via stream_finish_pass
    };
    StreamPhase stream_phase = StreamPhase::Idle;
    // g_stream_last_render_ms is declared in setup_02 (before rails/remesh pumps).
    omsi::FrameBudget g_stream_budget{};
    bool g_stream_budget_inited = false;
    std::atomic<bool> stream_spline_warm_done{true};
    bool stream_spline_warm_kicked = false;
    int stream_finish_pass = 0;
    // PrepLists substeps (all time-sliced — never flush a multi-second pass on the UI thread).
    // 0 objs, 1 splines, 2 attachments, 3 SA clear, 4 merge, 5 local idx, 6 global+enqueue,
    // 7 remap, 8 terrain (1 file/frame).
    int stream_lists_pass = 0;
    std::size_t stream_restamp_i = 0;
    std::int32_t stream_keep_sid = -1;
    std::int32_t stream_keep_oid = -1;
    std::vector<std::size_t> stream_sa_enqueue_buf;
    std::vector<std::size_t> stream_lists_sa_doomed;
    std::unordered_set<std::int64_t> stream_remove_set;
    // Current camera 9×9 (AABB load window). GPU apply skips tiles that left this set.
    std::unordered_set<std::int64_t> stream_wanted_set;
    std::unordered_set<std::int64_t> stream_inflight_set;
    // Editor-space AABBs for doomed tiles — O(removes) per object, no find_existing_tile_at_world.
    struct StreamDoomBox {
        float min_x = 0.f, max_x = 0.f, min_y = 0.f, max_y = 0.f;
    };
    std::vector<StreamDoomBox> stream_doom_boxes;
    struct StreamTileBox {
        std::int32_t gx = 0, gy = 0;
        float min_x = 0.f, max_x = 0.f, min_y = 0.f, max_y = 0.f;
    };
    std::vector<StreamTileBox> stream_tile_boxes;
    std::unordered_set<std::int32_t> stream_have_obj;
    std::unordered_set<std::int32_t> stream_have_sli;
    std::unordered_map<std::int32_t, std::size_t> stream_sli_idx;
    std::unordered_set<std::int32_t> stream_tree_alive;
    std::unordered_map<std::int32_t, map::MapObjectInst*> stream_tree_by_id;
    bool stream_tree_maps_ok = false;
    std::unordered_set<std::int32_t> stream_transient_obj;
    std::unordered_set<std::int32_t> stream_transient_sli;
    int stream_persist_stage = 0; // 0 collect+sync, 1 wait async write
    std::thread stream_persist_thread;
    std::atomic<bool> stream_persist_done{true};
    std::vector<std::pair<std::filesystem::path, map::MapTileData>> stream_persist_payload;
    std::filesystem::path stream_persist_omsi;
    std::vector<map::MapAttachObjectInst> stream_keep_att;

    struct EditorDtmJob {
        std::thread thread;
        std::atomic<bool> finished{false};
        map::GeoJobResult result{};
        std::vector<float> heights;
        std::int32_t grid_x = 0;
        std::int32_t grid_y = 0;
        std::filesystem::path map_path;
    };
    std::unique_ptr<EditorDtmJob> dtm_job;

    struct EditorAerialJob {
        std::thread thread;
        std::atomic<bool> finished{false};
        map::GeoJobResult result{};
        std::int32_t grid_x = 0;
        std::int32_t grid_y = 0;
        std::filesystem::path out_jpg;
        bool set_visible = true;
    };
    std::unique_ptr<EditorAerialJob> aerial_job;

    float stream_shift_dx = 0.f, stream_shift_dy = 0.f;
    std::size_t stream_shift_i = 0;
    int stream_shift_pass = 0; // 0 places,1 terrain,2 splines,3 markers,4 rails,5 rest
    world::World stream_world{};
    std::vector<world::PlacedMesh> stream_add_meshes;
    std::vector<map::MapTileData> stream_add_tiles;
    struct StreamNewObject {
        std::int32_t id = 0;
        std::int32_t tile_gx = 0;
        std::int32_t tile_gy = 0;
    };
    struct StreamHfJob {
        std::thread thread;
        std::atomic<bool> done{false};
        std::vector<TerrainHeightField> fields;
        std::unordered_set<std::int64_t> remove_set;
        std::chrono::steady_clock::time_point t0{};
    };
    std::unique_ptr<StreamHfJob> stream_hf_job;
    struct StreamListsJob {
        std::thread thread;
        std::atomic<bool> done{false};
        std::vector<map::MapObjectInst> objects;
        std::vector<map::SplineInst> splines;
        std::vector<map::MapAttachObjectInst> attachments;
        std::vector<EditSplineAttachmentEntry> spline_attachments;
        std::vector<map::MapTileData> add_tiles;
        std::vector<StreamDoomBox> doom_boxes;
        std::unordered_set<std::int64_t> remove_set;
        std::unordered_set<std::int32_t> transient_obj;
        std::unordered_set<std::int32_t> transient_sli;
        std::unordered_set<std::int32_t> sa_spawn_ids;
        std::unordered_set<std::int32_t> deleted_obj;
        std::unordered_set<std::int32_t> deleted_sli;
        std::unordered_set<std::size_t> sa_spawned;
        struct TileSliSnap {
            std::int32_t gx = 0, gy = 0;
            std::vector<std::int32_t> spline_ids;
        };
        std::vector<TileSliSnap> world_sli;
        struct AddRel {
            std::int32_t gx = 0, gy = 0;
            float rx = 0.f, ry = 0.f, size_m = 300.f;
        };
        std::vector<AddRel> add_rel;
        std::vector<TerrainHeightField> extra_heights;
        std::vector<StreamNewObject> new_objects;
        std::vector<std::int32_t> new_spline_ids;
        std::vector<std::size_t> sa_enqueue;
        std::vector<std::size_t> sa_doomed;
        std::unordered_map<std::int64_t, std::vector<std::size_t>> local_sli_idx;
        std::vector<map::GlobalSplineIndexEntry> global_index;
        bool sa_touched = false;
        std::unordered_set<std::int32_t> alive_spline_gpu;
        std::unordered_set<std::int32_t> alive_object_gpu;
        std::unordered_set<std::int64_t> wanted_tiles;
        float focus_ex = 0.f;
        float focus_ey = 0.f;
        float near_r = 300.f;
        int revived_s = 0;
        int revived_o = 0;
        int dup_s = 0;
        int dup_o = 0;
        double job_ms = 0.0; // worker wall time (logged as PrepLists job_ms)
    };
    std::unique_ptr<StreamListsJob> stream_lists_job;
    std::vector<std::pair<std::int32_t, std::int32_t>> stream_remove_grids;
    std::vector<std::int32_t> stream_new_spline_ids;
    std::thread stream_tex_prefetch_thread;
    bool stream_tex_prefetch_started = false;
    bool stream_prefetch_tex_sent = false;
    std::vector<std::filesystem::path> stream_prefetch_o3d_q;
    std::size_t stream_prefetch_o3d_i = 0;
    std::atomic<int> stream_prefetch_inflight{0};
    std::chrono::steady_clock::time_point stream_prepshift_t0{};
    std::chrono::steady_clock::time_point stream_lists_t0{};  // PrepLists wall clock (log only)
    double stream_lists_job_ms = 0.0;                         // worker time of the last lists job (log only)
    std::thread stream_spline_cpu_thread;
    std::atomic<bool> stream_spline_cpu_done{true};
    bool stream_spline_cpu_started = false;
    struct StreamSplineCpuSlot {
        std::int32_t id = 0;
        std::vector<spline::SplineMesh> meshes;
        bool ok = false;
    };
    std::vector<StreamSplineCpuSlot> stream_spline_cpu_slots;
    std::thread stream_terrain_cpu_thread;
    std::atomic<bool> stream_terrain_cpu_done{true};
    bool stream_terrain_cpu_started = false;
    std::mutex stream_terrain_cpu_slots_mu;
    std::unordered_map<std::int64_t, std::optional<editor::LocalTerrainTile>> stream_terrain_cpu_slots;
    std::size_t stream_early_tree_tile_i = 0; // time-sliced tree queue during UploadTerrain
    std::unordered_set<std::int32_t> stream_early_have_ids;
    std::thread stream_scenery_worker;
    std::atomic<bool> stream_scenery_stop{false};
    bool stream_early_have_ids_ready = false;
    std::vector<StreamNewObject> stream_new_objects;
    std::size_t stream_i = 0;
    std::size_t stream_tree_i = 0;
    float stream_cam_wx = 0.f, stream_cam_wy = 0.f, stream_cam_wz = 0.f;
    std::string stream_status;
    float stream_progress_frac = 0.f; // 0..1 for busy toast
    int stream_cooldown_frames = 0;
    bool stream_sa_touched = false;
    auto stream_busy = [&]() { return stream_phase != StreamPhase::Idle; };
    auto set_stream_progress = [&](float f) {
        stream_progress_frac = (std::min)(1.f, (std::max)(0.f, f));
    };
    auto stream_join_cpu_workers = [&]() {
        if (stream_tex_prefetch_thread.joinable()) stream_tex_prefetch_thread.join();
        if (stream_spline_cpu_thread.joinable()) stream_spline_cpu_thread.join();
        if (stream_terrain_cpu_thread.joinable()) stream_terrain_cpu_thread.join();
        stream_tex_prefetch_started = false;
        stream_prefetch_tex_sent = false;
        stream_prefetch_o3d_q.clear();
        stream_prefetch_o3d_i = 0;
        stream_prefetch_inflight.store(0);
        stream_spline_cpu_started = false;
        stream_spline_cpu_done = true;
        stream_spline_cpu_slots.clear();
        stream_terrain_cpu_started = false;
        stream_terrain_cpu_done = true;
        {
            std::lock_guard<std::mutex> lk(stream_terrain_cpu_slots_mu);
            stream_terrain_cpu_slots.clear();
        }
        stream_early_tree_tile_i = 0;
        path_rails_join_cpu();
        path_rails_cpu_slots.clear();
        path_rails_cpu_upload_i = 0;
        sa_join_cpu_workers();
    };
    auto stream_join_bg_list_jobs = [&]() {
        if (stream_hf_job) {
            if (stream_hf_job->thread.joinable()) stream_hf_job->thread.join();
            if (terrain_fields.empty() && !stream_hf_job->fields.empty())
                terrain_fields = std::move(stream_hf_job->fields);
            stream_hf_job.reset();
        }
        if (stream_lists_job) {
            if (stream_lists_job->thread.joinable()) stream_lists_job->thread.join();
            if (edit_objects.empty() && !stream_lists_job->objects.empty()) {
                edit_objects = std::move(stream_lists_job->objects);
                xform_soa.rebuild_from(edit_objects);
                edit_splines = std::move(stream_lists_job->splines);
                edit_attachments = std::move(stream_lists_job->attachments);
                edit_spline_attachments = std::move(stream_lists_job->spline_attachments);
            }
            if (stream_add_tiles.empty() && !stream_lists_job->add_tiles.empty())
                stream_add_tiles = std::move(stream_lists_job->add_tiles);
            stream_lists_job.reset();
            sa_pause_lists = false;
        }
    };
    auto stream_try_join_cpu_workers = [&]() -> bool {
        if (stream_spline_cpu_thread.joinable() && !stream_spline_cpu_done) return false;
        if (stream_terrain_cpu_thread.joinable() && !stream_terrain_cpu_done) return false;
        if (path_rails_cpu_thread.joinable() && !path_rails_cpu_done.load(std::memory_order_acquire))
            return false;
        if (sa_cpu_thread.joinable() && !sa_cpu_done.load(std::memory_order_acquire)) return false;
        if (stream_hf_job && !stream_hf_job->done.load(std::memory_order_acquire)) return false;
        if (stream_lists_job && !stream_lists_job->done.load(std::memory_order_acquire)) return false;
        stream_join_cpu_workers();
        return true;
    };
    // Worker-side asset prep (OpenOMSI-style): .o3d vertices + texture decode never run on the UI
    // thread. The main thread only uploads what this marks ready.
    struct O3dAssetPrefetch {
        std::mutex mu;
        std::unordered_set<std::string> ready;
        std::unordered_set<std::string> inflight;
    };
    static O3dAssetPrefetch s_o3d_assets;
    const std::filesystem::path o3d_assets_root = cfg.omsi_root;
    auto o3d_assets_ready = [](const std::filesystem::path& p) -> bool {
        if (p.empty()) return true;
        std::lock_guard<std::mutex> lk(s_o3d_assets.mu);
        return s_o3d_assets.ready.count(p.lexically_normal().string()) != 0;
    };
    auto submit_o3d_assets = [o3d_assets_root](const std::filesystem::path& p) {
        if (p.empty()) return;
        const auto key = p.lexically_normal().string();
        {
            std::lock_guard<std::mutex> lk(s_o3d_assets.mu);
            if (s_o3d_assets.ready.count(key) || !s_o3d_assets.inflight.insert(key).second) return;
        }
        omsi::JobSystem::global().submit(
            [p, key, root = o3d_assets_root]() {
                bool ok = false;
                try {
                    o3d::prepared_vertex_cache().ensure_path(p);
                    ok = o3d::prepared_vertex_cache().ready(p) ||
                         o3d::prepared_vertex_cache().try_get(p) != nullptr;
                    std::vector<std::filesystem::path> tex;
                    if (const auto m = o3d::load_mapped_shared(p); m && m->valid()) {
                        for (const auto& mat : m->mesh().parse_materials()) {
                            if (mat.texture.empty()) continue;
                            if (const auto t = gfx::dx12::resolve_texture_path(p, mat.texture, root))
                                tex.push_back(*t);
                        }
                    }
                    if (!tex.empty()) gfx::dx12::prefetch_textures_cpu(tex);
                } catch (...) {
                    ok = false;
                }
                std::lock_guard<std::mutex> lk(s_o3d_assets.mu);
                s_o3d_assets.inflight.erase(key);
                // Only mark ready when vertices are actually usable — otherwise UploadScenery
                // skips assets_pending and spins forever on DeferHeavy (est_ms > slice).
                if (ok) s_o3d_assets.ready.insert(key);
            },
            omsi::JobSystem::Priority::High);
    };
    // .x meshes and tree textures: read and decoded on a worker before UploadScenery creates the object, like
    // the .o3d assets above (they were read on the main thread: 30-250 ms per object on a cold disk)
    auto other_assets_ready = [](const std::string& key) -> bool {
        std::lock_guard<std::mutex> lk(s_o3d_assets.mu);
        return s_o3d_assets.ready.count(key) != 0;
    };
    auto submit_other_assets = [o3d_assets_root](const std::string& key, const std::filesystem::path& x_path,
                                                 const std::filesystem::path& tex_path) {
        {
            std::lock_guard<std::mutex> lk(s_o3d_assets.mu);
            if (s_o3d_assets.ready.count(key) || !s_o3d_assets.inflight.insert(key).second) return;
        }
        omsi::JobSystem::global().submit(
            [key, x_path, tex_path, root = o3d_assets_root]() {
                try {
                    std::vector<std::filesystem::path> tex;
                    if (!tex_path.empty()) tex.push_back(tex_path);
                    if (!x_path.empty()) {
                        if (const auto xf = omsi::xfile::load_x_file(x_path)) {
                            for (const auto& part : xf->parts) {
                                if (part.texture_filename.empty()) continue;
                                if (const auto t = gfx::dx12::resolve_texture_path(x_path, part.texture_filename, root))
                                    tex.push_back(*t);
                            }
                        }
                    }
                    if (!tex.empty()) gfx::dx12::prefetch_textures_cpu(tex);
                } catch (...) {
                }
                // ready even when it failed: the main thread then loads (and reports) it as before
                std::lock_guard<std::mutex> lk(s_o3d_assets.mu);
                s_o3d_assets.inflight.erase(key);
                s_o3d_assets.ready.insert(key);
            },
            omsi::JobSystem::Priority::High);
    };
    auto stream_kick_mesh_tex_prefetch = [&]() {
        if (!stream_tex_prefetch_started) {
            stream_tex_prefetch_started = true;
            std::vector<std::filesystem::path> paths;
            std::unordered_set<std::string> seen;
            std::vector<std::filesystem::path> o3d_paths;
            std::unordered_set<std::string> seen_o3d;
            auto add_o3d = [&](const std::filesystem::path& p) {
                if (p.empty()) return;
                const auto k = p.lexically_normal().string();
                if (!seen_o3d.insert(k).second) return;
                o3d_paths.push_back(p);
            };
            paths.reserve(stream_add_meshes.size());
            for (const auto& pm : stream_add_meshes) {
                if (!pm.o3d || !pm.o3d->valid()) continue;
                add_o3d(pm.source_path.empty() ? pm.o3d->path() : pm.source_path);
                for (const auto& mat : pm.o3d->mesh().parse_materials()) {
                    if (mat.texture.empty()) continue;
                    const auto tex = gfx::dx12::resolve_texture_path(pm.source_path, mat.texture, cfg.omsi_root);
                    if (!tex) continue;
                    if (!seen.insert(tex->string()).second) continue;
                    paths.push_back(*tex);
                }
            }
            auto add_sco_o3ds = [&](const std::filesystem::path& sco_path) {
                const sco::SceneryObject* sco = cached_sco(sco_path);
                if (!sco) return;
                for (const auto& mesh_ref : sco->meshes) {
                    if (mesh_ref.is_o3d) add_o3d(mesh_ref.mesh_file);
                }
            };
            std::unordered_map<std::int32_t, const map::MapObjectInst*> object_by_id;
            object_by_id.reserve(edit_objects.size() * 2 + 1);
            for (const auto& o : edit_objects) object_by_id.try_emplace(o.id, &o);
            for (const auto& no : stream_new_objects) {
                const auto it = object_by_id.find(no.id);
                if (it != object_by_id.end() && it->second) add_sco_o3ds(it->second->sco_path);
            }
            for (const auto& e : edit_spline_attachments) add_sco_o3ds(e.att.sco_path);
            stream_prefetch_o3d_q = std::move(o3d_paths);
            stream_prefetch_o3d_i = 0;
            stream_prefetch_tex_sent = false;
            log::info("stream CPU prefetch tex=" + std::to_string(paths.size()) +
                      " o3d=" + std::to_string(stream_prefetch_o3d_q.size()));
            if (!paths.empty()) {
                stream_prefetch_tex_sent = true;
                stream_prefetch_inflight.fetch_add(1);
                omsi::JobSystem::global().submit(
                    [paths = std::move(paths), inf = &stream_prefetch_inflight]() {
                        gfx::dx12::prefetch_textures_cpu(paths);
                        inf->fetch_sub(1, std::memory_order_relaxed);
                    },
                    omsi::JobSystem::Priority::High);
            }
        }
        // Keep workers fed: allow one in-flight prepare per affinity core (min 4, max 16).
        const int kMaxIo = static_cast<int>((std::max)(
            4u, (std::min)(16u, (std::max)(2u, omsi::JobSystem::global().worker_count()))));
        while (stream_prefetch_o3d_i < stream_prefetch_o3d_q.size() &&
               stream_prefetch_inflight.load(std::memory_order_relaxed) < kMaxIo) {
            submit_o3d_assets(stream_prefetch_o3d_q[stream_prefetch_o3d_i++]);
        }
    };
    auto stream_kick_spline_cpu = [&]() {
        if (stream_spline_cpu_started) return;
        stream_spline_cpu_started = true;
        stream_spline_cpu_slots.clear();
        stream_spline_cpu_slots.resize(stream_new_spline_ids.size());
        struct In {
            std::int32_t id = 0;
            map::SplineInst inst{};
        };
        std::vector<In> ins;
        ins.reserve(stream_new_spline_ids.size());
        // Avoid a quadratic main-thread scan before the remesh worker starts.
        // Stress logs include batches of 2,248 splines; repeatedly scanning all
        // edit_splines here makes the supposedly parallel phase hitch the UI first.
        std::unordered_map<std::int32_t, const map::SplineInst*> spline_by_id;
        spline_by_id.reserve(edit_splines.size() * 2 + 1);
        for (const auto& s : edit_splines) spline_by_id.try_emplace(s.id, &s);
        for (const std::int32_t sid : stream_new_spline_ids) {
            In in{};
            in.id = sid;
            if (const auto it = spline_by_id.find(sid); it != spline_by_id.end() && it->second)
                in.inst = *it->second;
            ins.push_back(std::move(in));
        }
        if (ins.empty()) {
            stream_spline_cpu_done = true;
            return;
        }
        stream_spline_cpu_done = false;
        const auto omsi_root = cfg.omsi_root;
        log::info("stream spline CPU remesh kick n=" + std::to_string(ins.size()) + " workers=" +
                  std::to_string(omsi::JobSystem::global().worker_count()));
        stream_spline_cpu_thread = std::thread([ins = std::move(ins), omsi_root,
                                                slots = &stream_spline_cpu_slots,
                                                &done = stream_spline_cpu_done]() {
          try { // an exception escaping a std::thread terminates the editor
            // HDD: load unique .sli once, sorted (sequential); tessellate can stay parallel (RAM).
            {
                std::vector<std::filesystem::path> slis;
                std::unordered_set<std::string> seen;
                for (const auto& in : ins) {
                    if (in.inst.sli_path.empty()) continue;
                    const auto k = in.inst.sli_path.lexically_normal().string();
                    if (seen.insert(k).second) slis.push_back(in.inst.sli_path);
                }
                std::sort(slis.begin(), slis.end());
                const char* hdd_env = std::getenv("OMSI_HDD_LOAD");
                if (!hdd_env || !hdd_env[0]) hdd_env = std::getenv("OMSI_LOAD_HDD");
                const bool hdd = hdd_env && hdd_env[0] && hdd_env[0] != '0';
                if (hdd || slis.size() <= 1) {
                    for (const auto& p : slis) (void)spline::load_sli_shared(p);
                } else {
                    omsi::JobSystem::global().parallel_for(
                        0, slis.size(),
                        [&](std::size_t i) { (void)spline::load_sli_shared(slis[i]); }, 1,
                        omsi::JobSystem::Priority::Low);
                }
            }
            // Work-stealing job system (not std::execution::par) — balances huge vs tiny splines.
            omsi::JobSystem::global().parallel_for(0, ins.size(), [&](std::size_t i) {
                auto& slot = (*slots)[i];
                slot.id = ins[i].id;
                if (ins[i].inst.id == 0) {
                    slot.ok = false;
                    return;
                }
                const auto def = spline::load_sli_shared(ins[i].inst.sli_path); // cache hit
                if (!def) {
                    slot.ok = false;
                    return;
                }
                slot.meshes = spline::build_spline_meshes(ins[i].inst, *def, omsi_root, 0.f, std::nullopt);
                slot.ok = !slot.meshes.empty();
            }, 1, omsi::JobSystem::Priority::Low);
            std::vector<std::filesystem::path> texs;
            std::unordered_set<std::string> seen;
            for (std::size_t i = 0; i < ins.size(); ++i) {
                if (!(*slots)[i].ok) continue;
                for (const auto& mesh : (*slots)[i].meshes) {
                    if (mesh.texture_path.empty()) continue;
                    const auto tex =
                        gfx::dx12::resolve_texture_path(ins[i].inst.sli_path, mesh.texture_path.string(), omsi_root);
                    if (!tex) continue;
                    if (!seen.insert(tex->string()).second) continue;
                    texs.push_back(*tex);
                }
            }
            if (!texs.empty()) gfx::dx12::prefetch_textures_cpu(texs);
          } catch (const std::exception& ex) {
            log::warn(std::string("stream worker thread aborted: ") + ex.what());
          } catch (...) {
            log::warn("stream worker thread aborted (unknown exception)");
          }
            done = true;
        });
    };
    auto stream_kick_terrain_cpu = [&]() {
        if (stream_terrain_cpu_started) return;
        stream_terrain_cpu_started = true;
        const auto& src_tiles = stream_add_tiles.empty() ? stream_world.tiles() : stream_add_tiles;
        struct Job {
            std::int64_t key = 0;
            std::filesystem::path path;
            map::GroundTex tex{};
            f4f23ma::f32 size_m = map::kStandardTileMeters;
        };
        std::vector<Job> jobs;
        jobs.reserve(src_tiles.size());
        {
            std::lock_guard<std::mutex> lk(stream_terrain_cpu_slots_mu);
            stream_terrain_cpu_slots.clear();
        }
        for (const auto& tile : src_tiles) {
            // Skip tiles that already have live GPU terrain (retry / partial upload).
            bool already = false;
            for (const auto& tc : terrain_chunks) {
                if (tc.alive && tc.tile_gx == tile.grid_x && tc.tile_gy == tile.grid_y) {
                    already = true;
                    break;
                }
            }
            if (already) continue;
            Job j{};
            j.key = tile_key(tile.grid_x, tile.grid_y);
            j.path = tile.path;
            j.tex = groundtex;
            if (const auto* m = world.tile_layout().find_grid(tile.grid_x, tile.grid_y)) j.size_m = m->size_meters;
            if (geo_settings.tile_aerial_visible) {
                if (const auto aerial = map::find_tile_aerial_jpg(cfg.omsi_root, cfg.global.root, tile.path))
                    j.tex.diffuse = *aerial;
            }
            jobs.push_back(std::move(j));
        }
        if (jobs.empty()) {
            stream_terrain_cpu_done = true;
            return;
        }
        stream_terrain_cpu_done = false;
        const auto omsi_root = cfg.omsi_root;
        log::info("stream terrain CPU kick n=" + std::to_string(jobs.size()) + " workers=" +
                  std::to_string(omsi::JobSystem::global().worker_count()));
        stream_terrain_cpu_thread = std::thread([jobs = std::move(jobs), omsi_root,
                                                 gtex = cfg.global.ground_textures,
                                                 slots = &stream_terrain_cpu_slots,
                                                 slots_mu = &stream_terrain_cpu_slots_mu,
                                                 &done = stream_terrain_cpu_done]() {
          try { // an exception escaping a std::thread terminates the editor
            std::vector<std::size_t> idx(jobs.size());
            for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            // Prefer sequential path order on HDD (tile .map / .terrain near each other).
            std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
                return jobs[a].path.lexically_normal().wstring() < jobs[b].path.lexically_normal().wstring();
            });
            const char* hdd_env = std::getenv("OMSI_HDD_LOAD");
            if (!hdd_env || !hdd_env[0]) hdd_env = std::getenv("OMSI_LOAD_HDD");
            const bool hdd = hdd_env && hdd_env[0] && hdd_env[0] != '0';
            // Publish each finished tile immediately so the main thread can GPU-upload while
            // the rest still build (pipeline). Never nest JobSystem::parallel_for here.
            auto publish = [&](std::size_t i, std::optional<editor::LocalTerrainTile> tile) {
                std::lock_guard<std::mutex> lk(*slots_mu);
                (*slots)[jobs[i].key] = std::move(tile);
            };
            auto build_one = [&](std::size_t i) {
                publish(i, editor::build_local_terrain_tile(jobs[i].path, jobs[i].tex, omsi_root, jobs[i].size_m,
                                                            gtex));
            };
            if (hdd || jobs.size() <= 1) {
                for (const std::size_t i : idx) build_one(i);
            } else {
                // Saturate JobSystem workers (all affinity cores). Do not nest another pool.
                omsi::JobSystem::global().parallel_for(
                    0, idx.size(), [&](std::size_t k) { build_one(idx[k]); }, 1,
                    omsi::JobSystem::Priority::Low);
            }
          } catch (const std::exception& ex) {
            log::warn(std::string("stream worker thread aborted: ") + ex.what());
          } catch (...) {
            log::warn("stream worker thread aborted (unknown exception)");
          }
            done = true;
        });
    };

    enum class GroupSettlePhase { Idle, Applying, Persisting };
    GroupSettlePhase group_settle_phase = GroupSettlePhase::Idle;
    auto group_settle_busy = [&]() { return group_settle_phase != GroupSettlePhase::Idle; };

    // posTrack: editor / world / tile-local / GPU drift — grep "posTrack" in omsi_f4_engine.log
    auto log_pos_track = [&](std::string_view tag, std::string_view phase, std::int32_t center_gx,
                             std::int32_t center_gy, float ox, float oy) {
        log::info(std::string("posTrack tag=") + std::string(tag) + " phase=" + std::string(phase) +
                  " center=(" + std::to_string(center_gx) + "," + std::to_string(center_gy) + ")" +
                  " tile_ox=" + std::to_string(ox) + " tile_oy=" + std::to_string(oy));
        auto track_obj = [&](std::int32_t id) {
            const map::MapObjectInst* obj = nullptr;
            for (const auto& eo : edit_objects)
                if (eo.id == id) {
                    obj = &eo;
                    break;
                }
            if (!obj) {
                log::info("posTrack obj id=" + std::to_string(id) + " edit=MISSING");
                return;
            }
            float spx = 0.f, spy = 0.f;
            bool has_sp = false;
            std::int32_t tgx = center_gx, tgy = center_gy;
            for (const auto& sp : scene_places) {
                if (!sp.alive || sp.object_id != id) continue;
                spx = sp.tx;
                spy = sp.ty;
                has_sp = true;
                tgx = sp.tile_gx;
                tgy = sp.tile_gy;
                break;
            }
            const float wx = obj->position.x + ox;
            const float wy = obj->position.y + oy;
            const auto tile_o = map::tile_origin_xy(world.tile_layout(), tgx, tgy);
            const float lx = wx - tile_o.first;
            const float ly = wy - tile_o.second;
            const float gpu_drift =
                has_sp ? std::hypot(spx - obj->position.x, spy - obj->position.y) : -1.f;
            log::info("posTrack obj id=" + std::to_string(id) + " editor=(" +
                      std::to_string(obj->position.x) + "," + std::to_string(obj->position.y) + ")" +
                      " world=(" + std::to_string(wx) + "," + std::to_string(wy) + ")" +
                      " local@(" + std::to_string(tgx) + "," + std::to_string(tgy) + ")=(" +
                      std::to_string(lx) + "," + std::to_string(ly) + ")" +
                      (has_sp ? " gpu=(" + std::to_string(spx) + "," + std::to_string(spy) +
                                    ") gpuDrift=" + std::to_string(gpu_drift)
                              : " gpu=none") +
                      (spline_attach_spawn_ids.count(id) ? " [splineAttach]" : ""));
        };
        track_obj(9);
        track_obj(90);
        track_obj(91);
        track_obj(92);
        for (const auto& e : edit_spline_attachments) {
            if (e.att.instance_id != 90 && e.att.instance_id != 91) continue;
            log::info("posTrack sa instId=" + std::to_string(e.att.instance_id) + " entryTile=(" +
                      std::to_string(e.tile_gx) + "," + std::to_string(e.tile_gy) + ") splineIdx=" +
                      std::to_string(e.att.spline_index));
            if (const map::SplineInst* sp = resolve_edit_spline_for_attach(e)) {
                const float wx = sp->position.x + ox;
                const float wy = sp->position.y + oy;
                const auto tile_o = map::tile_origin_xy(world.tile_layout(), e.tile_gx, e.tile_gy);
                log::info("posTrack sa instId=" + std::to_string(e.att.instance_id) + " parentSpline id=" +
                          std::to_string(sp->id) + " editor=(" + std::to_string(sp->position.x) + "," +
                          std::to_string(sp->position.y) + ") world=(" + std::to_string(wx) + "," +
                          std::to_string(wy) + ") local@entry=(" + std::to_string(wx - tile_o.first) +
                          "," + std::to_string(wy - tile_o.second) + ")");
            } else {
                log::info("posTrack sa instId=" + std::to_string(e.att.instance_id) +
                          " parentSpline=MISSING");
            }
        }
    };

    auto stream_update_velocity = [&](float wx, float wy) {
        const auto now = std::chrono::steady_clock::now();
        if (!stream_vel_init) {
            stream_vel_last_wx = wx;
            stream_vel_last_wy = wy;
            stream_vel_last_t = now;
            stream_vel_init = true;
            return;
        }
        const double dt = std::chrono::duration<double>(now - stream_vel_last_t).count();
        if (dt < 1.0e-3 || dt > 0.5) {
            stream_vel_last_wx = wx;
            stream_vel_last_wy = wy;
            stream_vel_last_t = now;
            return;
        }
        const float ix = static_cast<float>((wx - stream_vel_last_wx) / dt);
        const float iy = static_cast<float>((wy - stream_vel_last_wy) / dt);
        constexpr float kA = 0.25f;
        stream_vel_x = stream_vel_x * (1.f - kA) + ix * kA;
        stream_vel_y = stream_vel_y * (1.f - kA) + iy * kA;
        stream_vel_last_wx = wx;
        stream_vel_last_wy = wy;
        stream_vel_last_t = now;
    };
    // Prefetch focus = position + vel * look_ahead (0.75 s default).
    auto stream_prefetch_focus = [&](const map::EntryPoint& focus) -> map::EntryPoint {
        stream_update_velocity(focus.position.x, focus.position.y);
        map::EntryPoint pref = focus;
        constexpr float kLookAheadS = 0.75f;
        const float spd = std::sqrt(stream_vel_x * stream_vel_x + stream_vel_y * stream_vel_y);
        if (spd > 2.f) {
            pref.position.x += stream_vel_x * kLookAheadS;
            pref.position.y += stream_vel_y * kLookAheadS;
        }
        return pref;
    };

    // True when Chebyshev load ring (tile_radius) or unload hysteresis (R+1) differs from world.tiles().
    // Prefetch velocity is NOT used for membership — only for add-priority sort later.
    auto stream_diff_needed = [&](const map::EntryPoint& focus) -> bool {
        const auto load_opts = [&]() {
            auto o = make_editor_load_opts(cfg.game_opts, cfg.omsi_root, editor_tile_radius);
            o.workspace = cfg.workspace ? &*cfg.workspace : nullptr;
            return o;
        }();
        stream_update_velocity(focus.position.x, focus.position.y);
        // Load hysteresis: every diff costs in proportion to ALL loaded objects (lists rebuild + GPU shift), not
        // to the tiles it adds. With a big radius, crossing any tile border used to start a full diff for the
        // outermost strip. Only diff once a tile is missing inside radius - H; begin_stream_diff then loads the
        // full radius, so the far edge is refreshed every H tiles of movement instead of every tile.
        const int load_hyst = (std::max)(0, load_opts.tile_radius / 4);
        const auto origin = world::find_existing_tile_at_world(cfg.global, focus.position.x, focus.position.y);
        {
            const auto wanted = world::tiles_near_entry(cfg.global, focus, load_opts);
            for (const auto& t : wanted) {
                if (world.has_tile(t.grid_x, t.grid_y)) continue;
                if (!origin) return true;
                const int cheb =
                    (std::max)(std::abs(t.grid_x - origin->grid_x), std::abs(t.grid_y - origin->grid_y));
                if (cheb <= load_opts.tile_radius - load_hyst) return true;
            }
        }
        if (!origin) return false;
        // Hysteresis: keep 2 extra rings loaded so moving within/back to nearby tiles never reloads.
        // Drain hysteresis: a pure unload (nothing to load) only starts once kStreamDrainBatch tiles are
        // outside the unload ring. Before, every 2 stray tiles triggered a full diff (lists + GPU shift),
        // which is what kept the editor reloading while the user moved a few metres.
        // Revisits: keep H + 2 extra rings so going back a few tiles never reloads what was just loaded.
        const int unload_cheb = load_opts.tile_radius + load_hyst + 2;
        std::size_t excess = 0;
        for (const auto& t : world.tiles()) {
            const int cheb =
                (std::max)(std::abs(t.grid_x - origin->grid_x), std::abs(t.grid_y - origin->grid_y));
            if (cheb <= unload_cheb) continue;
            // Pinned dirty tile: never unload unsaved edits (they are persisted on save/autosave).
            if (dirty_tile_keys.count(tile_dirty_key(t.grid_x, t.grid_y))) continue;
            if (++excess >= kStreamDrainBatch) return true;
        }
        return false;
    };

    auto begin_stream_diff = [&](const map::EntryPoint& focus) {
        if (cfg.beamng) return;
        stream_new_objects.clear();
        stream_tree_i = 0;
        stream_tree_maps_ok = false;
        const auto load_opts = [&]() {
            auto o = make_editor_load_opts(cfg.game_opts, cfg.omsi_root, editor_tile_radius);
            o.workspace = cfg.workspace ? &*cfg.workspace : nullptr;
            return o;
        }();
        // Wanted = Chebyshev ring around REAL focus. Prefetch only warms velocity for sort order.
        stream_update_velocity(focus.position.x, focus.position.y);
        const auto wanted = world::tiles_near_entry(cfg.global, focus, load_opts);
        std::unordered_set<std::int64_t> want_set;
        want_set.reserve(wanted.size() * 2);
        for (const auto& t : wanted) want_set.insert(tile_key(t.grid_x, t.grid_y));

        std::vector<map::MapTileRef> to_add;
        for (const auto& t : wanted) {
            if (!world.has_tile(t.grid_x, t.grid_y)) to_add.push_back(t);
        }
        stream_remove_grids.clear();
        // Hysteresis: keep 2 extra rings loaded (matches load-radius +2) so short moves never
        // evict tiles that would be needed again seconds later.
        // Must match stream_diff_needed (radius + H + 2), or a diff would evict tiles the check keeps.
        const int unload_cheb = load_opts.tile_radius + (std::max)(0, load_opts.tile_radius / 4) + 2;
        if (const auto origin =
                world::find_existing_tile_at_world(cfg.global, focus.position.x, focus.position.y)) {
            for (const auto& t : world.tiles()) {
                const int cheb =
                    (std::max)(std::abs(t.grid_x - origin->grid_x), std::abs(t.grid_y - origin->grid_y));
                if (cheb <= unload_cheb) continue;
                // Pin dirty tiles until saved: keep their document AND render resident so moving
                // away never forces a save/reload cycle and edits are never lost.
                if (dirty_tile_keys.count(tile_dirty_key(t.grid_x, t.grid_y))) continue;
                stream_remove_grids.emplace_back(t.grid_x, t.grid_y);
            }
            // Interactive: at most 2 teardown tiles/diff when loading too; a pure drain takes a full batch.
            const std::size_t remove_cap = to_add.empty() ? kStreamDrainBatch : 2;
            if (!stream_queued_force && stream_remove_grids.size() > remove_cap) {
                std::sort(stream_remove_grids.begin(), stream_remove_grids.end(),
                          [&](const auto& a, const auto& b) {
                              const int ca = (std::max)(std::abs(a.first - origin->grid_x),
                                                        std::abs(a.second - origin->grid_y));
                              const int cb = (std::max)(std::abs(b.first - origin->grid_x),
                                                        std::abs(b.second - origin->grid_y));
                              return ca > cb;
                          });
                stream_remove_grids.resize(remove_cap);
            }
        }
        // Hysteresis: nothing to load and too few stray tiles → skip the whole diff (no lists/GPU shift churn).
        if (!stream_queued_force && to_add.empty() && stream_remove_grids.size() < kStreamDrainBatch) {
            stream_remove_grids.clear();
            return;
        }

        stream_cam_wx = focus.position.x;
        stream_cam_wy = focus.position.y;
        stream_cam_wz = focus.position.z;

        stream_focus_wx = focus.position.x;
        stream_focus_wy = focus.position.y;
        if (const auto origin = world::find_existing_tile_at_world(cfg.global, stream_focus_wx, stream_focus_wy)) {
            stream_focus_gx = origin->grid_x;
            stream_focus_gy = origin->grid_y;
        }
        stream_focus_valid = true;

        {
            const double target_frame_ms =
                omsi::load_turbo_enabled() ? (1000.0 / 60.0) : (1000.0 / 30.0);
            g_stream_budget =
                omsi::FrameBudget::for_stream(stream_queued_force, g_stream_last_render_ms, target_frame_ms, 16.0);
        }
        g_stream_budget_inited = true;

        log::info("stream diff add=" + std::to_string(to_add.size()) + " remove=" +
                  std::to_string(stream_remove_grids.size()) + " keep=" +
                  std::to_string(world.tiles().size() - stream_remove_grids.size()) +
                  " tile_r=" + std::to_string(load_opts.tile_radius) +
                  " unload_cheb=" + std::to_string(unload_cheb) +
                  " force=" + std::to_string(stream_queued_force ? 1 : 0));

        stream_wanted_set = want_set;
        if (stream_queued_force) {
            stream_join_cpu_workers();
            stream_join_bg_list_jobs();
        } else if (!stream_try_join_cpu_workers()) {
            stream_queued_focus = focus;
            stream_queued_force = false;
            log::info("stream defer begin — cpu workers still running");
            return;
        } else {
            if ((stream_hf_job && !stream_hf_job->done.load(std::memory_order_acquire)) ||
                (stream_lists_job && !stream_lists_job->done.load(std::memory_order_acquire))) {
                stream_queued_focus = focus;
                stream_queued_force = false;
                log::info("stream defer begin — list/hf job still running");
                return;
            }
            stream_join_bg_list_jobs();
        }
        if (to_add.empty() && stream_remove_grids.empty()) return;

        if (to_add.empty()) {
            stream_world = world::World{};
            stream_add_meshes.clear();
            stream_add_tiles.clear();
            stream_sa_touched = false;
            log_watch_scos("streamRemoveOnlyBegin");
            stream_phase = StreamPhase::PrepShiftMerge;
            stream_status = "Diff remove/shift...";
            return;
        }

        stream_job = std::make_unique<world::AsyncWorldLoader>();
        stream_job->start(cfg.global, focus, load_opts, std::move(to_add));
        stream_phase = StreamPhase::Loading;
        stream_status = "Cargando tiles nuevos...";
        stream_sa_touched = false;
        log_watch_scos("streamBegin");
    };

    // Build stream focus from the RMB click position (editor space) — never from live camera/mouse poll.
    auto stream_focus_from_rmb = [&](float editor_x, float editor_y, float editor_z) -> std::optional<map::EntryPoint> {
        const float wx = tile_ox + editor_x;
        const float wy = tile_oy + editor_y;
        if (!world::find_existing_tile_at_world(cfg.global, wx, wy)) return std::nullopt;
        map::EntryPoint focus = world.spawn();
        focus.position.x = wx;
        focus.position.y = wy;
        focus.position.z = editor_z;
        focus.position_local = focus.position;
        focus.category = -1;
        return focus;
    };

    // ONLY entry point for runtime 9×9 reload (boot uses AsyncWorldLoader in setup).
    auto request_tile_stream_on_rmb = [&](float editor_x, float editor_y, float editor_z) -> bool {
        if (cfg.verify_mode) return false;
        if (edit_mode != EditMode::None || marquee.phase != MarqueePhase::None ||
            group_grab_active || shift_s_move.active || group_settle_busy()) {
            editor_log::event("TILE_STREAM_BLOCKED", session_state());
            return false;
        }
        const auto focus_opt = stream_focus_from_rmb(editor_x, editor_y, editor_z);
        if (!focus_opt) return false;
        const map::EntryPoint& focus = *focus_opt;

        log::info("stream RMB trigger world=(" + std::to_string(focus.position.x) + "," +
                  std::to_string(focus.position.y) + ") editor=(" + std::to_string(editor_x) + "," +
                  std::to_string(editor_y) + ")");

        if (stream_busy()) {
            stream_queued_focus = focus;
            stream_queued_force = true;
            if (stream_phase == StreamPhase::Loading && stream_job) {
                stream_job->request_cancel();
                stream_status = "Cancelando carga (RMB)…";
                log::info("stream cancel load — RMB retarget");
            } else {
                stream_status = "Cola stream (RMB)…";
            }
            return true;
        }
        if (!stream_diff_needed(focus)) {
            log::info("stream RMB skipped — 9×9 already loaded for this focus");
            return false;
        }
        stream_queued_force = false;
        begin_stream_diff(focus);
        return true;
    };

    // Entrypoint menu / picker — same as boot spawn: move 9×9 to the entry tile (not RMB passive pan).
    auto request_tile_stream_for_entrypoint = [&](const map::EntryPoint& ep) -> bool {
        if (cfg.verify_mode) return false;
        log::info("stream entrypoint trigger world=(" + std::to_string(ep.position.x) + "," +
                  std::to_string(ep.position.y) + ") name=\"" + ep.name + "\"");
        if (stream_busy()) {
            stream_queued_focus = ep;
            stream_queued_force = true;
            if (stream_phase == StreamPhase::Loading && stream_job) {
                stream_job->request_cancel();
                stream_status = "Cancelando carga (entrypoint)…";
                log::info("stream cancel load — entrypoint retarget");
            } else {
                stream_status = "Cola stream (entrypoint)…";
            }
            return true;
        }
        if (!stream_diff_needed(ep)) {
            log::info("stream entrypoint skipped — 9×9 already loaded for this entry");
            return false;
        }
        stream_queued_force = false;
        begin_stream_diff(ep);
        return true;
    };

    auto kick_queued_stream = [&]() {
        if (!stream_queued_focus) return;
        auto f = *stream_queued_focus;
        stream_queued_focus.reset();
        // Keep stream_queued_force through begin_stream_diff (uncapped unload + worker join).
        const bool force = stream_queued_force;
        if (force || stream_diff_needed(f)) begin_stream_diff(f);
        stream_queued_force = false;
    };

    // Drive / pan follow: never cancel in-flight load (RMB teleport sets force).
    auto request_tile_stream_follow = [&](float editor_x, float editor_y, float editor_z) -> bool {
        if (cfg.verify_mode) return false;
        const auto focus_opt = stream_focus_from_rmb(editor_x, editor_y, editor_z);
        if (!focus_opt) return false;
        const map::EntryPoint& focus = *focus_opt;
        if (stream_busy()) {
            stream_queued_focus = focus;
            stream_queued_force = false;
            return true;
        }
        if (!stream_diff_needed(focus)) return false;
        begin_stream_diff(focus);
        return true;
    };

    float stream_follow_ex = 0.f;
    float stream_follow_ey = 0.f;
    bool stream_follow_init = false;
    auto maybe_stream_follow = [&](float editor_x, float editor_y, float editor_z) -> bool {
        if (cfg.beamng) return false; // BeamNG streams by radius in mod_beamng_frame
        const float dx = editor_x - stream_follow_ex;
        const float dy = editor_y - stream_follow_ey;
        if (stream_follow_init && dx * dx + dy * dy < 10.f * 10.f) return false;
        const bool had_prev = stream_follow_init;
        stream_follow_ex = editor_x;
        stream_follow_ey = editor_y;
        stream_follow_init = true;
        // Predictive prefetch (Tanda 1): stream toward where the camera is heading so crossing a
        // tile boundary does not stall waiting for the next 9x9. Velocity = the recent step; the
        // R+2 hysteresis keeps tiles behind resident, so this only loads ahead, never evicts.
        float look_x = editor_x, look_y = editor_y;
        if (had_prev) {
            const float step2 = dx * dx + dy * dy;
            if (step2 > 4.f) { // moved > 2 m since last sample
                const float step_len = std::sqrt(step2);
                float lead = step_len * 2.0f; // look ~2 steps ahead
                const float cap = 120.f;      // never lead more than ~a tile edge
                if (lead > cap) lead = cap;
                look_x = editor_x + (dx / step_len) * lead;
                look_y = editor_y + (dy / step_len) * lead;
            }
        }
        return request_tile_stream_follow(look_x, look_y, editor_z);
    };

    auto queued_focus_tile_differs = [&]() -> bool {
        if (!stream_queued_focus) return false;
        const auto origin = world::find_existing_tile_at_world(
            cfg.global, stream_queued_focus->position.x, stream_queued_focus->position.y);
        if (!origin) return false;
        return origin->grid_x != stream_focus_gx || origin->grid_y != stream_focus_gy;
    };

    // Freshest known mouse world pos: queued focus (mouse already moved again) or the one in flight.
    auto current_focus_xy = [&]() -> std::pair<float, float> {
        if (stream_queued_focus) return {stream_queued_focus->position.x, stream_queued_focus->position.y};
        return {stream_focus_wx, stream_focus_wy};
    };
    auto stream_tile_wanted = [&](std::int32_t gx, std::int32_t gy) -> bool {
        const auto k = tile_key(gx, gy);
        if (stream_inflight_set.count(k)) return true;
        if (stream_wanted_set.empty()) return true;
        return stream_wanted_set.count(k) != 0;
    };
    auto stream_tile_wanted_spline = [&](const map::SplineInst& sp) -> bool {
        if (SplineGpu* g = find_spline_gpu(sp.id)) {
            if (stream_tile_wanted(g->tile_gx, g->tile_gy)) return true;
        }
        const auto origin =
            world::find_existing_tile_at_world(cfg.global, sp.position.x + tile_ox, sp.position.y + tile_oy);
        if (origin && stream_tile_wanted(origin->grid_x, origin->grid_y)) return true;
        if (sp.length > 0.5f) {
            const auto samp = spline::evaluate_spline(sp, sp.length);
            const auto end_tile =
                world::find_existing_tile_at_world(cfg.global, samp.x + tile_ox, samp.y + tile_oy);
            if (end_tile && stream_tile_wanted(end_tile->grid_x, end_tile->grid_y)) return true;
        }
        if (!origin) return true;
        return stream_wanted_set.empty();
    };
    auto stream_focus_editor_xy = [&]() -> std::pair<float, float> {
        const auto [wx, wy] = current_focus_xy();
        return {wx - tile_ox, wy - tile_oy};
    };
    auto stream_spline_near_r = [&]() -> float {
        // UI visual radius only. 0 used to mean "unlimited", which made the spline CPU remesh
        // build EVERY spline in the loaded tiles and block the main thread for many seconds.
        // Use a finite fallback so the remesh batch stays bounded (matches render distance cap).
        constexpr float kSplineFallbackRadiusM = 600.f;
        return editor_visual_radius_m > 0.f ? editor_visual_radius_m : kSplineFallbackRadiusM;
    };
    auto stream_sort_focus_xy = [&]() -> std::pair<float, float> {
        map::EntryPoint f{};
        const auto [wx, wy] = current_focus_xy();
        f.position.x = wx;
        f.position.y = wy;
        const auto pref = stream_prefetch_focus(f);
        return {pref.position.x, pref.position.y};
    };
    auto stream_spline_near = [&](const map::SplineInst& sp) -> bool {
        const auto [ex, ey] = stream_focus_editor_xy();
        return spline::spline_in_radius_xy(sp, ex, ey, stream_spline_near_r());
    };
    // Once a tile is wanted (Chebyshev ring), GPU all SCO/SLI on it; visual radius gates extras.
    auto stream_spline_upload_ok = [&](const map::SplineInst& sp) -> bool {
        if (stream_tile_wanted_spline(sp)) return true;
        return stream_spline_near(sp);
    };

    // Cesium cullRequestsWhileMoving priority / UE streaming distance-based priority:
    // nearest-to-cursor tile's terrain+meshes must appear first, farther tiles fill in after.
    // Cheap reorder (no re-diff, no data loss) — resorted at each pass/phase checkpoint below
    // so priority keeps tracking the mouse even while the rest of the 9x9 keeps streaming.
    auto sort_add_tiles_by_focus = [&](float fx, float fy) {
        auto d2_u64 = [&](std::int32_t gx, std::int32_t gy) -> std::uint64_t {
            const auto o = map::tile_origin_xy(world.tile_layout(), gx, gy);
            float size_m = map::kStandardTileMeters;
            if (const auto* m = world.tile_layout().find_grid(gx, gy)) size_m = m->size_meters;
            const float cx = o.first + size_m * 0.5f - fx;
            const float cy = o.second + size_m * 0.5f - fy;
            const double d2 = static_cast<double>(cx) * static_cast<double>(cx) +
                              static_cast<double>(cy) * static_cast<double>(cy);
            if (d2 <= 0.0) return 0;
            return static_cast<std::uint64_t>(d2);
        };
        auto order_by_d2 = [&](std::size_t n, auto&& key_at) {
            std::vector<std::uint64_t> keys(n);
            std::vector<std::size_t> ord(n);
            for (std::size_t i = 0; i < n; ++i) {
                keys[i] = key_at(i);
                ord[i] = i;
            }
            std::sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
            return ord;
        };
        {
            const auto ord = order_by_d2(stream_add_tiles.size(), [&](std::size_t i) {
                return d2_u64(stream_add_tiles[i].grid_x, stream_add_tiles[i].grid_y);
            });
            std::vector<map::MapTileData> sorted;
            sorted.reserve(ord.size());
            for (const auto i : ord) sorted.push_back(std::move(stream_add_tiles[i]));
            stream_add_tiles = std::move(sorted);
        }
        {
            const auto ord = order_by_d2(stream_add_meshes.size(), [&](std::size_t i) {
                return d2_u64(stream_add_meshes[i].tile_grid_x, stream_add_meshes[i].tile_grid_y);
            });
            std::vector<world::PlacedMesh> sorted;
            sorted.reserve(ord.size());
            for (const auto i : ord) sorted.push_back(std::move(stream_add_meshes[i]));
            stream_add_meshes = std::move(sorted);
        }
        {
            const auto [ex, ey] = stream_focus_editor_xy();
            if (!stream_new_spline_ids.empty() && !edit_splines.empty()) {
                std::unordered_map<std::int32_t, std::uint64_t> d2;
                d2.reserve(edit_splines.size() * 2);
                for (const auto& s : edit_splines) {
                    const double v = static_cast<double>(spline::spline_focus_dist2_xy(s, ex, ey));
                    d2[s.id] = v <= 0.0 ? 0ull : static_cast<std::uint64_t>(v);
                }
                std::sort(stream_new_spline_ids.begin(), stream_new_spline_ids.end(),
                          [&](std::int32_t a, std::int32_t b) {
                              const auto ia = d2.find(a);
                              const auto ib = d2.find(b);
                              const std::uint64_t ka = ia == d2.end() ? ~0ull : ia->second;
                              const std::uint64_t kb = ib == d2.end() ? ~0ull : ib->second;
                              return ka < kb;
                          });
            }
            if (!stream_new_objects.empty() && !edit_objects.empty()) {
                std::unordered_map<std::int32_t, std::uint64_t> d2;
                d2.reserve(edit_objects.size() * 2);
                for (const auto& o : edit_objects) {
                    const double dx = static_cast<double>(o.position.x - ex);
                    const double dy = static_cast<double>(o.position.y - ey);
                    const double v = dx * dx + dy * dy;
                    d2[o.id] = v <= 0.0 ? 0ull : static_cast<std::uint64_t>(v);
                }
                std::sort(stream_new_objects.begin(), stream_new_objects.end(),
                          [&](const auto& a, const auto& b) {
                              const auto ia = d2.find(a.id);
                              const auto ib = d2.find(b.id);
                              const std::uint64_t ka = ia == d2.end() ? ~0ull : ia->second;
                              const std::uint64_t kb = ib == d2.end() ? ~0ull : ib->second;
                              return ka < kb;
                          });
            }
        }
    };

    // Only on explicit teleport while PersistTiles — passive camera queue waits for apply to finish.
    auto abort_stale_stream_apply = [&]() -> bool {
        if (!stream_busy() || !stream_queued_force || !queued_focus_tile_differs()) return false;
        if (stream_phase != StreamPhase::PersistTiles) return false;

        log::info("stream abort PersistTiles for teleport — was (" + std::to_string(stream_focus_gx) + "," +
                  std::to_string(stream_focus_gy) + ")");
        if (stream_persist_thread.joinable()) {
            stream_persist_thread.join();
        }
        stream_join_cpu_workers();
        stream_join_bg_list_jobs();
        stream_persist_stage = 0;
        stream_persist_payload.clear();
        stream_world = world::World{};
        stream_add_meshes.clear();
        stream_add_tiles.clear();
        stream_remove_grids.clear();
        stream_remove_set.clear();
        stream_doom_boxes.clear();
        stream_sa_enqueue_buf.clear();
        stream_lists_pass = 0;
        stream_finish_pass = 0;
        stream_i = 0;
        stream_phase = StreamPhase::Idle;
        stream_status.clear();
        set_stream_progress(0.f);
        stream_cooldown_frames = 0;
        auto f = *stream_queued_focus;
        stream_queued_focus.reset();
        stream_queued_force = false;
        begin_stream_diff(f);
        return true;
    };

    auto stream_build_remove_caches = [&]() {
        stream_remove_set.clear();
        stream_remove_set.reserve(stream_remove_grids.size() * 2);
        for (const auto& g : stream_remove_grids)
            stream_remove_set.insert(tile_key(g.first, g.second));
        stream_doom_boxes.clear();
        stream_doom_boxes.reserve(stream_remove_grids.size());
        for (const auto& g : stream_remove_grids) {
            const auto [rx, ry] = tile_rel(g.first, g.second);
            float size_m = map::kStandardTileMeters;
            if (const auto* m = world.tile_layout().find_grid(g.first, g.second)) size_m = m->size_meters;
            constexpr float pad = 1.f;
            stream_doom_boxes.push_back(
                StreamDoomBox{rx - pad, rx + size_m + pad, ry - pad, ry + size_m + pad});
        }
    };

    auto stream_kick_lists_job = [&]() {
        if (stream_lists_job) return;
        auto j = std::make_unique<StreamListsJob>();
        j->objects = std::move(edit_objects);
        j->splines = std::move(edit_splines);
        j->attachments = std::move(edit_attachments);
        j->spline_attachments = std::move(edit_spline_attachments);
        j->doom_boxes = stream_doom_boxes;
        j->remove_set = stream_remove_set;
        j->transient_obj = stream_transient_obj;
        j->transient_sli = stream_transient_sli;
        j->sa_spawn_ids = spline_attach_spawn_ids;
        j->deleted_obj = editor_deleted_object_ids;
        j->deleted_sli = editor_deleted_spline_ids;
        j->sa_spawned = sa_spawned_entries;
        {
            const auto [ex, ey] = stream_focus_editor_xy();
            j->focus_ex = ex;
            j->focus_ey = ey;
            j->near_r = stream_spline_near_r();
            j->wanted_tiles = stream_wanted_set;
            for (const auto& g : spline_gpu)
                if (g.alive && g.id != 0) j->alive_spline_gpu.insert(g.id);
            for (const auto& sp : scene_places)
                if (sp.alive) j->alive_object_gpu.insert(sp.object_id);
        }
        j->add_rel.reserve(stream_add_tiles.size());
        for (const auto& tile : stream_add_tiles) {
            StreamListsJob::AddRel r{};
            r.gx = tile.grid_x;
            r.gy = tile.grid_y;
            const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
            r.rx = rx;
            r.ry = ry;
            r.size_m = map::kStandardTileMeters;
            if (const auto* m = world.tile_layout().find_grid(tile.grid_x, tile.grid_y))
                r.size_m = m->size_meters;
            j->add_rel.push_back(r);
        }
        j->add_tiles = std::move(stream_add_tiles);
        j->world_sli.reserve(world.tiles().size());
        for (const auto& t : world.tiles()) {
            StreamListsJob::TileSliSnap s{};
            s.gx = t.grid_x;
            s.gy = t.grid_y;
            s.spline_ids.reserve(t.splines.size());
            for (const auto& sp : t.splines) s.spline_ids.push_back(sp.id);
            j->world_sli.push_back(std::move(s));
        }
        std::unordered_set<std::int64_t> have_hf;
        have_hf.reserve(terrain_fields.size() * 2);
        for (const auto& tf : terrain_fields) {
            if (!tf.heights.empty() || !tf.heights_base.empty())
                have_hf.insert(tile_key(tf.grid_x, tf.grid_y));
        }
        StreamListsJob* raw = j.get();
        j->thread = std::thread([raw, have_hf = std::move(have_hf)]() {
          try { // an exception escaping a std::thread terminates the editor
            const auto job_t0 = std::chrono::steady_clock::now();
            auto tk = [](std::int32_t gx, std::int32_t gy) -> std::int64_t {
                return (static_cast<std::int64_t>(gx) << 32) | static_cast<std::uint32_t>(gy);
            };
            auto doomed_xy = [&](float ex, float ey) {
                for (const auto& b : raw->doom_boxes) {
                    if (ex >= b.min_x && ex <= b.max_x && ey >= b.min_y && ey <= b.max_y) return true;
                }
                return false;
            };
            {
                std::size_t i = 0;
                while (i < raw->objects.size()) {
                    auto& o = raw->objects[i];
                    const bool keep = raw->transient_obj.count(o.id) || raw->sa_spawn_ids.count(o.id) ||
                                      !doomed_xy(o.position.x, o.position.y);
                    if (keep) ++i;
                    else {
                        o = std::move(raw->objects.back());
                        raw->objects.pop_back();
                    }
                }
            }
            {
                std::size_t i = 0;
                while (i < raw->splines.size()) {
                    auto& s = raw->splines[i];
                    float end_x = s.position.x, end_y = s.position.y;
                    if (s.length > 0.5f) {
                        const auto samp = spline::evaluate_spline(s, s.length);
                        end_x = samp.x;
                        end_y = samp.y;
                    }
                    const bool keep = raw->transient_sli.count(s.id) != 0 || !doomed_xy(s.position.x, s.position.y) ||
                                      !doomed_xy(end_x, end_y);
                    if (keep) ++i;
                    else {
                        s = std::move(raw->splines.back());
                        raw->splines.pop_back();
                    }
                }
            }
            std::unordered_set<std::int32_t> have_obj;
            have_obj.reserve(raw->objects.size() * 2);
            for (const auto& o : raw->objects) have_obj.insert(o.id);
            {
                std::vector<map::MapAttachObjectInst> keep_att;
                keep_att.reserve(raw->attachments.size());
                // (kept while its PARENT is here: the child only becomes an object when it is hung on, so
                //  filtering by the child dropped every [attachObj] not hung on yet - strsgnpN's sign
                //  never appeared after the first stream)
                std::unordered_set<std::int32_t> kept_child;
                for (const auto& a : raw->attachments)
                    if ((have_obj.count(a.child_id) || have_obj.count(a.parent_id)) && kept_child.insert(a.child_id).second)
                        keep_att.push_back(a);
                raw->attachments.swap(keep_att);
            }
            for (std::size_t ei = 0; ei < raw->spline_attachments.size(); ++ei) {
                const auto& e = raw->spline_attachments[ei];
                if (raw->remove_set.count(tk(e.tile_gx, e.tile_gy))) raw->sa_doomed.push_back(ei);
            }
            std::unordered_set<std::int32_t> have_sli;
            have_sli.reserve(raw->splines.size() * 2);
            for (const auto& s : raw->splines) have_sli.insert(s.id);
            auto find_sa = [&](std::int32_t gx, std::int32_t gy, std::int32_t id) -> std::optional<std::size_t> {
                if (id <= 0) return std::nullopt;
                for (std::size_t i = 0; i < raw->spline_attachments.size(); ++i) {
                    const auto& e = raw->spline_attachments[i];
                    if (e.tile_gx == gx && e.tile_gy == gy && e.att.instance_id == id) return i;
                }
                return std::nullopt;
            };
            for (std::size_t ti = 0; ti < raw->add_tiles.size(); ++ti) {
                const auto& tile = raw->add_tiles[ti];
                float rx = 0.f, ry = 0.f;
                if (ti < raw->add_rel.size()) {
                    rx = raw->add_rel[ti].rx;
                    ry = raw->add_rel[ti].ry;
                }
                std::unordered_set<std::int32_t> tile_sa_inst_ids;
                for (const auto& sa : tile.spline_attachments)
                    if (sa.instance_id > 0) tile_sa_inst_ids.insert(sa.instance_id);
                for (const auto& o : tile.objects) {
                    if (tile_sa_inst_ids.count(o.id)) continue;
                    if (raw->deleted_obj.count(o.id)) continue;
                    if (have_obj.count(o.id)) continue;
                    auto c = o;
                    c.position.x += rx;
                    c.position.y += ry;
                    have_obj.insert(c.id);
                    raw->new_objects.push_back({c.id, tile.grid_x, tile.grid_y});
                    raw->objects.push_back(std::move(c));
                }
                for (const auto& s : tile.splines) {
                    if (raw->deleted_sli.count(s.id)) continue;
                    if (have_sli.count(s.id)) continue;
                    auto c = s;
                    c.position.x += rx;
                    c.position.y += ry;
                    have_sli.insert(c.id);
                    raw->new_spline_ids.push_back(c.id);
                    raw->splines.push_back(std::move(c));
                }
                {
                    std::unordered_set<std::int32_t> had;
                    for (const auto& a : raw->attachments) had.insert(a.child_id);
                    for (const auto& a : tile.attachments)
                        if (had.insert(a.child_id).second) raw->attachments.push_back(a);
                }
                for (const auto& sa : tile.spline_attachments) {
                    if (sa.instance_id > 0 && raw->deleted_obj.count(sa.instance_id)) continue;
                    if (const auto existing = find_sa(tile.grid_x, tile.grid_y, sa.instance_id)) {
                        auto& e = raw->spline_attachments[*existing];
                        e.att = sa;
                        e.tile_spline_count = static_cast<std::int32_t>(tile.splines.size());
                        if (!raw->sa_spawned.count(*existing)) raw->sa_enqueue.push_back(*existing);
                        raw->sa_touched = true;
                        continue;
                    }
                    EditSplineAttachmentEntry e{};
                    e.att = sa;
                    e.tile_gx = tile.grid_x;
                    e.tile_gy = tile.grid_y;
                    e.tile_spline_count = static_cast<std::int32_t>(tile.splines.size());
                    raw->spline_attachments.push_back(std::move(e));
                    raw->sa_enqueue.push_back(raw->spline_attachments.size() - 1);
                    raw->sa_touched = true;
                }
            }
            std::unordered_map<std::int32_t, std::size_t> sli_idx;
            sli_idx.reserve(raw->splines.size() * 2);
            for (std::size_t ei = 0; ei < raw->splines.size(); ++ei) sli_idx[raw->splines[ei].id] = ei;
            for (const auto& snap : raw->world_sli) {
                auto& idx_vec = raw->local_sli_idx[tk(snap.gx, snap.gy)];
                idx_vec.assign(snap.spline_ids.size(), static_cast<std::size_t>(-1));
                for (std::size_t si = 0; si < snap.spline_ids.size(); ++si) {
                    const auto it = sli_idx.find(snap.spline_ids[si]);
                    if (it != sli_idx.end()) idx_vec[si] = it->second;
                }
            }
            for (const auto& snap : raw->world_sli) {
                for (std::int32_t i = 0; i < static_cast<std::int32_t>(snap.spline_ids.size()); ++i)
                    raw->global_index.push_back({snap.gx, snap.gy, i});
            }
            {
                // Fan-out heightfile IO across JobSystem (same work, all cores).
                struct HfJob {
                    std::size_t ti = 0;
                    std::int32_t gx = 0;
                    std::int32_t gy = 0;
                    float size_m = map::kStandardTileMeters;
                    float rx = 0.f;
                    float ry = 0.f;
                    std::filesystem::path path;
                };
                std::vector<HfJob> hf_jobs;
                hf_jobs.reserve(raw->add_tiles.size());
                for (std::size_t ti = 0; ti < raw->add_tiles.size(); ++ti) {
                    const auto& tile = raw->add_tiles[ti];
                    if (have_hf.count(tk(tile.grid_x, tile.grid_y))) continue;
                    HfJob j{};
                    j.ti = ti;
                    j.gx = tile.grid_x;
                    j.gy = tile.grid_y;
                    j.path = tile.path;
                    if (ti < raw->add_rel.size()) {
                        j.size_m = raw->add_rel[ti].size_m;
                        j.rx = raw->add_rel[ti].rx;
                        j.ry = raw->add_rel[ti].ry;
                    }
                    hf_jobs.push_back(std::move(j));
                }
                std::vector<std::optional<TerrainHeightField>> hf_out(hf_jobs.size());
                if (!hf_jobs.empty()) {
                    omsi::JobSystem::global().parallel_for(
                        0, hf_jobs.size(),
                        [&](std::size_t i) {
                            if (auto heights = editor::load_terrain_heights(hf_jobs[i].path)) {
                                TerrainHeightField field{};
                                field.grid_x = hf_jobs[i].gx;
                                field.grid_y = hf_jobs[i].gy;
                                field.rel_x = hf_jobs[i].rx;
                                field.rel_y = hf_jobs[i].ry;
                                field.size_m = hf_jobs[i].size_m;
                                field.heights_base = std::move(*heights);
                                hf_out[i] = std::move(field);
                            }
                        },
                        1, omsi::JobSystem::Priority::Low);
                }
                for (auto& f : hf_out)
                    if (f) raw->extra_heights.push_back(std::move(*f));
            }
            {
                std::unordered_set<std::int32_t> already;
                already.reserve(raw->new_spline_ids.size() * 2 + 16);
                for (const auto id : raw->new_spline_ids) already.insert(id);
                for (const auto& s : raw->splines) {
                    if (already.count(s.id)) continue;
                    if (raw->alive_spline_gpu.count(s.id)) continue;
                    const bool near_focus =
                        spline::spline_in_radius_xy(s, raw->focus_ex, raw->focus_ey, raw->near_r);
                    if (!near_focus) continue;
                    raw->new_spline_ids.push_back(s.id);
                    already.insert(s.id);
                    ++raw->revived_s;
                }
                std::unordered_set<std::int32_t> already_o;
                already_o.reserve(raw->new_objects.size() * 2 + 16);
                for (const auto& no : raw->new_objects) already_o.insert(no.id);
                const float r2 = raw->near_r * raw->near_r;
                for (const auto& o : raw->objects) {
                    if (already_o.count(o.id)) continue;
                    if (raw->deleted_obj.count(o.id)) continue;
                    if (raw->sa_spawn_ids.count(o.id)) continue;
                    if (raw->alive_object_gpu.count(o.id)) continue;
                    const float dx = o.position.x - raw->focus_ex;
                    const float dy = o.position.y - raw->focus_ey;
                    if (dx * dx + dy * dy > r2) continue;
                    raw->new_objects.push_back({o.id, -2000000000, 0});
                    already_o.insert(o.id);
                    ++raw->revived_o;
                }
                std::unordered_map<std::int32_t, int> sli_cnt, obj_cnt;
                for (const auto& s : raw->splines) ++sli_cnt[s.id];
                for (const auto& o : raw->objects) ++obj_cnt[o.id];
                for (const auto& [id, c] : sli_cnt)
                    if (c > 1) ++raw->dup_s;
                for (const auto& [id, c] : obj_cnt)
                    if (c > 1) ++raw->dup_o;
            }
            raw->job_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job_t0).count();
          } catch (const std::exception& ex) {
            log::warn(std::string("stream lists job aborted: ") + ex.what());
          } catch (...) {
            log::warn("stream lists job aborted (unknown exception)");
          }
            raw->done.store(true, std::memory_order_release);
        });
        stream_lists_job = std::move(j);
        sa_pause_lists = true;
    };

    auto stream_begin_prep_lists = [&]() {
        stream_phase = StreamPhase::PrepLists;
        stream_lists_t0 = std::chrono::steady_clock::now();
        stream_lists_pass = 0;
        stream_i = 0;
        stream_sa_enqueue_buf.clear();
        stream_have_obj.clear();
        stream_have_sli.clear();
        stream_transient_obj.clear();
        stream_transient_sli.clear();
        for (const auto& o : transient_objects) stream_transient_obj.insert(o.id);
        for (const auto& s : transient_splines) stream_transient_sli.insert(s.id);
        stream_keep_sid =
            (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
                ? edit_splines[static_cast<std::size_t>(selected_spline)].id
                : -1;
        stream_keep_oid =
            (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
                ? edit_objects[static_cast<std::size_t>(selected_object)].id
                : -1;
        log_watch_scos("streamPrepListsBefore");
        stream_status = "Diff lists (bg)…";
        log::info("stream PrepLists begin objs=" + std::to_string(edit_objects.size()) + " splines=" +
                  std::to_string(edit_splines.size()) + " add_tiles=" +
                  std::to_string(stream_add_tiles.size()));
        stream_tree_maps_ok = false;
        stream_tree_by_id.clear();
        stream_tree_i = 0;
        stream_kick_lists_job();
    };

    auto stream_goto_after_upload_meshes = [&]() {
        stream_shift_i = 0;
        stream_shift_pass = 0;
        if (stream_remove_grids.empty()) {
            log::info("stream skip PrepShiftGpu (no removes)");
            stream_begin_prep_lists();
        } else {
            stream_phase = StreamPhase::PrepShiftGpu;
            stream_status = "Diff GPU shift...";
            log::info("stream PrepShiftGpu begin removes=" + std::to_string(stream_remove_grids.size()) +
                      " places=" + std::to_string(scene_places.size()) + " terrains=" +
                      std::to_string(terrain_chunks.size()) + " splines_gpu=" +
                      std::to_string(spline_gpu.size()));
            stream_prepshift_t0 = std::chrono::steady_clock::now();
        }
    };

    // Trees + .x-only SCOs are not in stream_add_meshes (world_loader skips them). Also resync Z once
    // terrain heights from PrepLists pass 8 exist.
    auto stream_spawn_object_gpu = [&](map::MapObjectInst& o, const std::int32_t tgx, const std::int32_t tgy) {
        const auto spawn_t0 = std::chrono::steady_clock::now();
        // Inner lambda keeps one exit path so even early-return cases can be timed consistently.
        [&]() {
            if (!stream_tile_wanted(tgx, tgy)) return;
            if (editor_deleted_object_ids.count(o.id)) return;
            if (spline_attach_spawn_ids.count(o.id)) return;
            // Meter visual radius: do not GPU-spawn SCOs outside the draw disk (tile residency still holds).
            if (editor_visual_radius_m > 0.f && !visual_in_radius(o.position.x, o.position.y)) return;
            if (object_has_alive_gpu(o.id)) {
                // Abs/path SCOs already on GPU: visibility-only — do not resync Z from terrain.
                bool abs_z = false;
                if (const sco::SceneryObject* sco = cached_sco(o.sco_path)) abs_z = sco::skip_terrain_add(*sco);
                if (!abs_z) sync_object_gpu(o);
                return;
            }
            spawn_one_object_gpu(o, tgx, tgy);
        }();
        const double spawn_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - spawn_t0).count();
        if (spawn_ms >= 8.0) {
            log::warn("STREAM_SPAWN_SLOW id=" + std::to_string(o.id) + " ms=" +
                      std::to_string(spawn_ms) + " tile=" + std::to_string(tgx) + "," +
                      std::to_string(tgy) + " sco=" + o.sco_path.filename().string());
        }
    };

// Terrain tiles prepared off the main thread: restamp + heightfield/polycut applied to the mesh + texture
        // decode run on workers; the main thread only does the GPU append (one tile per frame).
        struct TerrainGpuDone {
            std::int32_t gx = 0, gy = 0;
            float rx = 0.f, ry = 0.f;
            editor::TerrainRestampResult res;
            decltype(editor::LocalTerrainTile::mesh) mesh;
            std::filesystem::path texture_path;
        };
        struct TerrainGpuQueue {
            std::mutex mu;
            std::deque<TerrainGpuDone> done;
            std::atomic<int> pending{0}; // submitted and not yet popped by the main thread
        };
        const auto stream_terrain_q = std::make_shared<TerrainGpuQueue>();

        // Scenery GPU spawn queue: move actual GPU upload to a worker, main thread only enqueues
        struct SceneryGpuTask {
            map::MapObjectInst obj;
            std::int32_t tgx, tgy;
            XMMATRIX world_m;
            bool abs_z;
        };
        std::unordered_set<std::string> stream_sco_requested; // .sco paths already queued for background load
        int stream_sco_wait_frames = 0;
    auto pump_stream_apply = [&]() {
        // Low-rate steady-state snapshot. This is intentionally independent of the
        // renderer PERF60 report so we can correlate "20 FPS after loading" with
        // the amount of world state that remains alive.
        static std::uint32_t s_perf_stream_state = 0;
        if ((++s_perf_stream_state % 300u) == 1u && stream_phase == StreamPhase::Idle) {
            std::size_t alive_scene = 0;
            for (const auto& sp : scene_places)
                if (sp.alive) ++alive_scene;
            std::size_t alive_terrain = 0;
            for (const auto& tc : terrain_chunks)
                if (tc.alive) ++alive_terrain;
            std::size_t alive_splines = 0;
            for (const auto& sg : spline_gpu)
                if (sg.alive) ++alive_splines;
            std::size_t alive_markers = 0;
            for (const auto& mk : end_markers)
                if (mk.alive) ++alive_markers;
            std::size_t alive_rails = 0;
            for (const auto& r : path_rails)
                if (r.alive) ++alive_rails;
            log::info("STREAM_STATE tiles=" + std::to_string(world.tiles().size()) +
                      " edit_objects=" + std::to_string(edit_objects.size()) +
                      " scene_alive=" + std::to_string(alive_scene) +
                      " terrain_alive=" + std::to_string(alive_terrain) +
                      " spline_alive=" + std::to_string(alive_splines) +
                      " markers_alive=" + std::to_string(alive_markers) +
                      " rails_alive=" + std::to_string(alive_rails) +
                      " gpu_chunks=" + std::to_string(renderer.chunk_count()) +
                      " sa_stale=" + std::to_string(sa_stale_cpu_n) +
                      " sa_empty_along=" + std::to_string(sa_empty_along_n));
        }

        // UE-style AsyncLoadingTimeLimit. Budget starts when GPU work begins (see
        // stream_budget_begin) — NOT before joining CPU workers, or UploadTerrain/Meshes
        // can stick at 0/N forever after join ate the whole window.
        // Time slice (UE AsyncLoadingTimeLimit style). Upload phases were finishing in <1ms
        // but stopped at 12 items — raise window so byte budget can actually fill.
        // Soft budgets: idle protects 60 FPS; RMB stream may use holgura down to 30 FPS (≤16 ms).
        // Never 28 ms (that alone caps you ~35 FPS).
        if (!g_stream_budget_inited) {
            const double target_frame_ms =
                omsi::load_turbo_enabled() ? (1000.0 / 60.0) : (1000.0 / 30.0);
            g_stream_budget =
                omsi::FrameBudget::for_stream(stream_queued_force, g_stream_last_render_ms, target_frame_ms, 16.0);
            g_stream_budget_inited = true;
        } else {
            g_stream_budget.adapt(g_stream_last_render_ms);
        }
        omsi::FrameBudget& frame_budget = g_stream_budget;
        auto& gov = omsi::LoadGovernor::global();
        {
            // Time slice = what the shared governor leaves this frame (never collapses to 1 ms on a heavy UI frame).
            const double slice = (std::max)(0.5, gov.remaining_ms());
            frame_budget.target_ms = frame_budget.cpu_ms_hard = slice;
            frame_budget.cpu_ms_soft = slice * 0.85;
            frame_budget.slice_max_ms = gov.slice_ms();
            frame_budget.target_bytes = 64ull << 20;
            frame_budget.upload_bytes = frame_budget.geometry_bytes = frame_budget.target_bytes;
            frame_budget.target_weighted = static_cast<double>(frame_budget.target_bytes) * frame_budget.io_weight;
        }

        auto budget_end = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double, std::milli>(frame_budget.cpu_ms_hard));
        const auto phase_t0 = std::chrono::steady_clock::now();
        auto budget_ok = [&]() { return std::chrono::steady_clock::now() < budget_end; };
        auto stream_budget_begin = [&]() {
            frame_budget.begin_phase(frame_budget.target_ms);
            budget_end = frame_budget.deadline;
        };
        const std::size_t kStreamBytesPerFrame = static_cast<std::size_t>(frame_budget.upload_bytes);

        auto estimate_o3d_cost = [](const o3d::MappedO3D* m) -> omsi::FrameBudget::WorkCost {
            omsi::FrameBudget::WorkCost c;
            if (!m || !m->valid()) {
                c.bytes = 64ull * 1024ull;
                c.vertices = 256;
                c.indices = 512;
                c.est_ms = 0.2;
                return c;
            }
            const auto& mesh = m->mesh();
            c.vertices = mesh.vertex_count;
            c.indices = static_cast<std::uint64_t>(mesh.triangle_count) * 3ull;
            const std::size_t vb = static_cast<std::size_t>(mesh.vertex_count) * 32ull;
            const std::size_t ib = mesh.long_triangle_indices
                                       ? static_cast<std::size_t>(mesh.triangle_count) * 14ull
                                       : static_cast<std::size_t>(mesh.triangle_count) * 8ull;
            c.bytes = vb + ib + 4096ull;
            c.draws = 1;
            c.materials = 1;
            const auto bps = omsi::HardwareProfile::global().memcpy_bps;
            c.est_ms = static_cast<double>(c.bytes) / (static_cast<double>(bps) / 1000.0) + 0.15;
            return c;
        };
        auto estimate_o3d_bytes = [&](const o3d::MappedO3D* m) -> std::size_t {
            return static_cast<std::size_t>(estimate_o3d_cost(m).bytes);
        };
        auto estimate_spline_slot_bytes = [](const std::vector<spline::SplineMesh>& meshes) -> std::size_t {
            std::size_t b = 0;
            for (const auto& sm : meshes) {
                b += sm.vertices.size() * sizeof(sm.vertices[0]);
                b += sm.indices.size() * sizeof(sm.indices[0]);
            }
            return b + 64ull * 1024ull;
        };
        static std::unordered_map<std::string, std::uint64_t> s_tex_size_cache;
        auto cached_tex_bytes = [&](const std::filesystem::path& p) -> std::uint64_t {
            const auto k = p.string();
            if (const auto it = s_tex_size_cache.find(k); it != s_tex_size_cache.end()) return it->second;
            std::error_code ec;
            const auto n = std::filesystem::file_size(p, ec);
            const auto v = ec ? 0ull : static_cast<std::uint64_t>(n);
            s_tex_size_cache[k] = v;
            return v;
        };
        auto estimate_stream_mesh_cost = [&](const world::PlacedMesh& pm,
                                             const model_cfg::ModelCfgMeshEntry* matl) -> omsi::FrameBudget::WorkCost {
            auto c = estimate_o3d_cost(pm.o3d.get());
            std::string key = o3d_gpu_key(pm.source_path.empty() && pm.o3d ? pm.o3d->path() : pm.source_path, matl);
            const bool unique_tex =
                matl && (matl->on_surface ||
                         std::any_of(matl->materials.begin(), matl->materials.end(),
                                     [](const model_cfg::ModelCfgMaterial& m) {
                                         return m.use_text_texture.has_value() || m.freetex_label.has_value() ||
                                                m.is_matl_change;
                                     }));
            if (!unique_tex && o3d_gpu_proto.find(key) != o3d_gpu_proto.end()) {
                c.est_ms = 0.01;
                c.bytes = 0;
                c.weight = 1.0;
                c.is_clone = true;
                c.touches_io = false;
                return c;
            }
            const auto& hw = omsi::HardwareProfile::global();
            std::uint64_t tex_bytes = 0;
            int n_files = 0;
            if (pm.o3d && pm.o3d->valid()) {
                for (const auto& mat : pm.o3d->mesh().parse_materials()) {
                    if (mat.texture.empty()) continue;
                    const auto tex = gfx::dx12::resolve_texture_path(pm.source_path, mat.texture, cfg.omsi_root);
                    if (!tex) continue;
                    tex_bytes += cached_tex_bytes(*tex);
                    ++n_files;
                }
            }
            c.bytes += tex_bytes;
            if (tex_bytes > 0 && n_files > 0) {
                const auto wait_path =
                    pm.source_path.empty() && pm.o3d ? pm.o3d->path() : pm.source_path;
                const bool verts_ready = o3d::prepared_vertex_cache().ready(wait_path);
                const bool tex_ready = stream_prefetch_tex_sent &&
                                       stream_prefetch_inflight.load(std::memory_order_relaxed) == 0;
                if (verts_ready && tex_ready) {
                    c.touches_io = false;
                    c.weight = 1.0;
                    const auto bps = hw.memcpy_bps;
                    c.est_ms = static_cast<double>(c.bytes) / (static_cast<double>(bps) / 1000.0) + 0.25;
                } else {
                    c.est_ms += static_cast<double>(n_files) * hw.disk_open_ms +
                                (static_cast<double>(tex_bytes) / (hw.disk_read_mbps * 1024.0 * 1024.0)) * 1000.0;
                    c.weight = hw.io_weight;
                    c.touches_io = true;
                }
            }
            return c;
        };
        auto stream_bytes_ok = [&](std::size_t bytes_used, std::size_t n) {
            if (n == 0) return true;
            return bytes_used < kStreamBytesPerFrame;
        };
        auto stream_work_ok = [&](const omsi::FrameBudget::WorkCost& c) {
            if (frame_budget.expired() || !frame_budget.time_ok()) {
                if (frame_budget.used_items > 0) return false;
                if (c.est_ms > frame_budget.slice_max_ms) return false;
            }
            return frame_budget.can_accept(c);
        };
        auto stream_work_consume = [&](const omsi::FrameBudget::WorkCost& c) {
            frame_budget.consume(c);
        };
        // Charge a WorkCost and keep legacy bytes_used in sync for existing loops.
        auto stream_charge = [&](std::size_t& bytes_used, const omsi::FrameBudget::WorkCost& c) {
            stream_work_consume(c);
            bytes_used += static_cast<std::size_t>(c.bytes);
        };
        const omsi::FrameBudget::WorkCost kTreeCost{32ull * 1024ull, 256, 384, 1, 1, 0.04};
        auto stream_pump_queued_trees = [&]() {
            if (stream_tree_i >= stream_new_objects.size()) return;
            stream_tree_alive.clear();
            stream_tree_alive.reserve(scene_places.size() * 2 + 8);
            for (const auto& sp : scene_places)
                if (sp.alive) stream_tree_alive.insert(sp.object_id);
            stream_tree_by_id.clear();
            stream_tree_by_id.reserve(edit_objects.size() * 2 + 8);
            for (auto& o : edit_objects) stream_tree_by_id[o.id] = &o;
            stream_tree_maps_ok = true;
            std::size_t n = 0;
            while (stream_tree_i < stream_new_objects.size()) {
                const auto& no = stream_new_objects[stream_tree_i];
                if (!stream_tile_wanted(no.tile_gx, no.tile_gy)) {
                    ++stream_tree_i;
                    continue;
                }
                if (stream_tree_alive.count(no.id)) {
                    ++stream_tree_i;
                    continue;
                }
                auto it = stream_tree_by_id.find(no.id);
                if (it == stream_tree_by_id.end()) {
                    ++stream_tree_i;
                    continue;
                }
                map::MapObjectInst* obj = it->second;
                if (!obj) {
                    ++stream_tree_i;
                    continue;
                }
                const sco::SceneryObject* sco = cached_sco(obj->sco_path);
                if (!sco || !sco->tree.valid) {
                    ++stream_tree_i;
                    continue;
                }
                omsi::FrameBudget::WorkCost cost = kTreeCost;
                cost.est_ms = 0.04;
                const auto admit = frame_budget.try_admit(cost);
                if (admit == omsi::FrameBudget::Admit::Defer) break;
                if (admit == omsi::FrameBudget::Admit::DeferHeavy) break;
                const auto t0 = std::chrono::steady_clock::now();
                try {
                    stream_spawn_object_gpu(*obj, no.tile_gx, no.tile_gy);
                } catch (...) {
                    log::warn("stream tree pump exception id=" + std::to_string(no.id));
                }
                stream_tree_alive.insert(no.id);
                const double dt = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
                frame_budget.charge(dt, cost);
                ++stream_tree_i;
                ++n;
                if (dt > frame_budget.target_ms)
                    log::warn("LOAD_SLOW tag=apply phase=trees frame=" + std::to_string(g_editor_frame_id) +
                              " ms=" + std::to_string(dt) + " slice=" + std::to_string(frame_budget.target_ms));
                // Hard-stop after every item — never chain past the wall deadline.
                if (!budget_ok() || frame_budget.expired() || !frame_budget.time_ok()) break;
            }
            if (n > 0)
                log::info("stream tree pump +" + std::to_string(n) + " i=" +
                          std::to_string(stream_tree_i) + "/" +
                          std::to_string(stream_new_objects.size()));
        };

        if (abort_stale_stream_apply()) return;
        if (stream_phase == StreamPhase::Idle && stream_queued_focus) {
            kick_queued_stream();
            if (stream_phase == StreamPhase::Idle) return;
        }

        if (stream_phase == StreamPhase::Loading && stream_job) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.loading_poll");
            const auto progress = stream_job->poll();
            stream_status = progress.status.empty() ? "Cargando tiles nuevos..." : progress.status;
            set_stream_progress(progress.fraction * 0.20f);
            if (!progress.done) return;
            stream_world = stream_job->take_world();
            stream_job.reset();
            if (progress.cancelled) {
                stream_phase = StreamPhase::Idle;
                stream_status.clear();
                set_stream_progress(0.f);
                stream_cooldown_frames = 0;
                if (stream_queued_focus && stream_queued_force) {
                    auto f = *stream_queued_focus;
                    stream_queued_focus.reset();
                    stream_queued_force = false;
                    begin_stream_diff(f);
                } else {
                    kick_queued_stream();
                }
                return;
            }
            if (progress.failed) {
                log::warn("tile stream failed: " + progress.status);
                stream_phase = StreamPhase::Idle;
                stream_status.clear();
                set_stream_progress(0.f);
                if (stream_queued_focus && stream_queued_force) {
                    auto f = *stream_queued_focus;
                    stream_queued_focus.reset();
                    stream_queued_force = false;
                    begin_stream_diff(f);
                } else {
                    kick_queued_stream();
                }
                return;
            }
            if (view_only) {
                stream_phase = StreamPhase::PrepShiftMerge;
                stream_status = "Diff merge...";
                log::info("stream skip PersistTiles (VIEW-ONLY)");
                return;
            }
            stream_phase = StreamPhase::PersistTiles;
            stream_shift_i = 0; // index into stream_remove_grids while collecting
            stream_persist_stage = 0;
            stream_persist_payload.clear();
            stream_persist_omsi = cfg.omsi_root;
            stream_kick_terrain_cpu();
            if (stream_persist_thread.joinable()) {
                stream_persist_done = true;
                stream_persist_thread.join();
            }
            stream_persist_done = true;
            stream_status = "Guardando tiles...";
            set_stream_progress(0.20f);
            return;
        }

        if (stream_phase == StreamPhase::PersistTiles) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.persist_tiles");
            if (view_only) {
                stream_phase = StreamPhase::PrepShiftMerge;
                stream_status = "Diff merge...";
                return;
            }
            // Stage 0: sync lists on main thread (1 tile / slice) — no disk yet.
            // Stage 1: write_tile_map_file on worker (UE AsyncLoadingThread pattern).
            if (stream_persist_stage == 0) {
                while (stream_shift_i < stream_remove_grids.size() &&
                       std::chrono::steady_clock::now() < budget_end) {
                    const auto g = stream_remove_grids[stream_shift_i++];
                    for (auto& tile : world.tiles_mut()) {
                        if (tile.grid_x != g.first || tile.grid_y != g.second) continue;
                        sync_tile_lists_only(tile);
                        stream_persist_payload.emplace_back(tile.path, tile);
                        break;
                    }
                }
                stream_status = "Guardando tiles (sync) " + std::to_string(stream_shift_i) + "/" +
                                std::to_string(stream_remove_grids.size());
                if (stream_shift_i < stream_remove_grids.size()) return;
                stream_persist_done = false;
                stream_persist_stage = 1;
                stream_persist_thread = std::thread([payload = std::move(stream_persist_payload),
                                                     omsi = stream_persist_omsi,
                                                     ws = cfg.workspace,
                                                     &done = stream_persist_done]() mutable {
                    for (auto& [path, tile] : payload) {
                        if (!ws || ws->actual_dir.empty()) {
                            omsi::log::warn("persist tile async refused (no workspace) " + path.string());
                            continue;
                        }
                        std::filesystem::path write_path = map::workspace_overlay_write_path(*ws, path);
                        if (write_path.empty()) {
                            omsi::log::warn("persist tile async refused " + path.string());
                            continue;
                        }
                        const auto read_fb = map::workspace_tile_map_for_read(&*ws, path);
                        if (!map::write_tile_map_file(write_path, tile, omsi, read_fb))
                            omsi::log::warn("persist tile async failed " + write_path.string());
                    }
                    done = true;
                });
                stream_persist_payload.clear();
                stream_status = "Guardando tiles (async)…";
                return;
            }
            if (!stream_persist_done.load()) {
                stream_status = "Guardando tiles (async)…";
                return;
            }
            if (stream_persist_thread.joinable()) stream_persist_thread.join();
            stream_persist_stage = 0;
            // Tiles written on unload are clean now.
            for (const auto& g : stream_remove_grids)
                dirty_tile_keys.erase(tile_dirty_key(g.first, g.second));
            stream_phase = StreamPhase::PrepShiftMerge;
            stream_status = "Diff merge...";
            return;
        }

        if (stream_phase == StreamPhase::PrepShiftMerge) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.prep_shift_merge");
            stream_add_meshes = stream_world.meshes();
            stream_add_tiles.clear();
            for (const auto& tw : stream_world.tiles()) stream_add_tiles.push_back(tw);
            stream_inflight_set.clear();
            stream_inflight_set.reserve(stream_add_tiles.size() * 2 + 8);
            for (const auto& t : stream_add_tiles) stream_inflight_set.insert(tile_key(t.grid_x, t.grid_y));
            {
                const auto [fx, fy] = stream_sort_focus_xy();
                sort_add_tiles_by_focus(fx, fy);
            }
            stream_kick_terrain_cpu();
            stream_kick_mesh_tex_prefetch();

            const float old_ox = tile_ox;
            const float old_oy = tile_oy;
            (void)old_ox;
            (void)old_oy;

            world.unload_tiles(stream_remove_grids);
            world.merge_from(std::move(stream_world));
            {
                map::EntryPoint sp = world.spawn();
                sp.position.x = stream_cam_wx;
                sp.position.y = stream_cam_wy;
                sp.position.z = stream_cam_wz;
                world.set_spawn(sp);
            }
            // Session origin (tile_ox/tile_oy) stays fixed at boot — never recenter editor coords.
            stream_shift_dx = 0.f;
            stream_shift_dy = 0.f;
            log::info("stream merge session_ox=" + std::to_string(tile_ox) + " session_oy=" +
                      std::to_string(tile_oy) + " remove=" + std::to_string(stream_remove_grids.size()) +
                      " add=" + std::to_string(stream_add_tiles.size()));
            // Tree early-queue is time-sliced inside UploadTerrain (while CPU terrain builds) —
            // never scan every tile/object on this frame (was a multi-ms hitch after merge).
            log_watch_scos("streamMerge");
            stream_build_remove_caches();
            stream_i = 0;
            stream_tree_i = 0;
            stream_shift_i = 0;
            stream_shift_pass = 0;
            stream_early_tree_tile_i = 0;
            stream_early_have_ids.clear();
            stream_early_have_ids_ready = false;
            // Terrain/meshes FIRST — PrepShiftGpu (hide old splines) was blocking visible tiles at ~20%.
            stream_phase = StreamPhase::UploadTerrain;
            stream_status = "GPU add terrain...";
            log::info("stream UploadTerrain begin tiles=" + std::to_string(stream_add_tiles.size()) +
                      " removes=" + std::to_string(stream_remove_grids.size()));
            return;
        }

        if (stream_phase == StreamPhase::UploadTerrain) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.upload_terrain");
            stream_kick_terrain_cpu();
            stream_budget_begin();
            // Stage 2: GPU append of one finished tile (CPU work already done on a worker).
            {
                TerrainGpuDone dn;
                bool have = false;
                {
                    std::lock_guard<std::mutex> lk(stream_terrain_q->mu);
                    if (!stream_terrain_q->done.empty()) {
                        dn = std::move(stream_terrain_q->done.front());
                        stream_terrain_q->done.pop_front();
                        have = true;
                    }
                }
                if (have) {
                    stream_terrain_q->pending.fetch_sub(1);
                    bool already = false;
                    for (const auto& tc : terrain_chunks)
                        if (tc.alive && tc.tile_gx == dn.gx && tc.tile_gy == dn.gy) already = true;
                    if (!already && stream_tile_wanted(dn.gx, dn.gy)) {
                        const auto t0 = std::chrono::steady_clock::now();
                        for (auto& tf : terrain_fields) {
                            if (tf.grid_x != dn.gx || tf.grid_y != dn.gy) continue;
                            apply_restamp_result(tf, std::move(dn.res));
                            break;
                        }
                        const double restamp_ms = std::chrono::duration<double, std::milli>(
                                                      std::chrono::steady_clock::now() - t0).count();
                        terrain_detail_base[tile_key(dn.gx, dn.gy)] =
                            std::make_shared<const gfx::BufferMesh>(dn.mesh);
                        {
                            auto _w_mesh = omsi::MainThreadWatchdog::active_scope("stream.upload_terrain.append_mesh");
                            if (renderer.append_textured_mesh(dn.mesh.vertices.data(),
                                                              static_cast<std::uint32_t>(dn.mesh.vertices.size()),
                                                              dn.mesh.indices.data(),
                                                              static_cast<std::uint32_t>(dn.mesh.indices.size()),
                                                              dn.texture_path, 0u)) {
                                const std::size_t chunk = renderer.chunk_count() - 1;
                                const auto wt = store_world_transposed(XMMatrixTranslation(dn.rx, dn.ry, 0.f));
                                renderer.set_chunk_world_matrix(chunk, wt.data());
                                register_terrain_chunk(chunk, dn.gx, dn.gy);
                            }
                        }
                        const double ms = std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - t0).count();
                        if (ms >= 8.0)
                            log::warn("STREAM_TERRAIN_GPU_SLOW tile=(" + std::to_string(dn.gx) + "," +
                                      std::to_string(dn.gy) + ") ms=" + std::to_string(ms) +
                                      " restamp_ms=" + std::to_string(restamp_ms) +
                                      " append_ms=" + std::to_string(ms - restamp_ms) +
                                      " verts=" + std::to_string(dn.mesh.vertices.size()));
                    }
                }
            }

            // While terrain CPU runs: queue trees + upload cache-ready mesh clones (pipeline fill).
            auto early_queue_trees_slice = [&]() {
                auto _w_trees = omsi::MainThreadWatchdog::active_scope("stream.upload_terrain.early_trees");
                if (stream_early_tree_tile_i >= stream_add_tiles.size()) return;
                auto& have = stream_early_have_ids;
                if (!stream_early_have_ids_ready) {
                    have.clear();
                    have.reserve(edit_objects.size() * 2 + stream_new_objects.size() * 2 + 16);
                    for (const auto& o : edit_objects) have.insert(o.id);
                    for (const auto& no : stream_new_objects) have.insert(no.id);
                    stream_early_have_ids_ready = true;
                }
                int queued = 0;
                while (stream_early_tree_tile_i < stream_add_tiles.size() && budget_ok()) {
                    const auto& tile = stream_add_tiles[stream_early_tree_tile_i++];
                    const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
                    std::unordered_set<std::int32_t> tile_sa;
                    for (const auto& sa : tile.spline_attachments)
                        if (sa.instance_id > 0) tile_sa.insert(sa.instance_id);
                    for (const auto& o : tile.objects) {
                        if (tile_sa.count(o.id) || editor_deleted_object_ids.count(o.id)) continue;
                        const sco::SceneryObject* sco = cached_sco(o.sco_path);
                        if (!sco || !sco->tree.valid) continue;
                        if (have.count(o.id)) {
                            if (!object_has_alive_gpu(o.id) && !spline_attach_spawn_ids.count(o.id)) {
                                stream_new_objects.push_back({o.id, tile.grid_x, tile.grid_y});
                                ++queued;
                            }
                            continue;
                        }
                        auto c = o;
                        c.position.x += rx;
                        c.position.y += ry;
                        have.insert(c.id);
                        edit_objects.push_back(std::move(c));
                        stream_new_objects.push_back({c.id, tile.grid_x, tile.grid_y});
                        ++queued;
                    }
                }
                if (queued > 0)
                    log::info("stream early-queue trees +" + std::to_string(queued) + " tot_new_obj=" +
                              std::to_string(stream_new_objects.size()) + " tiles=" +
                              std::to_string(stream_early_tree_tile_i) + "/" +
                              std::to_string(stream_add_tiles.size()));
            };

            stream_kick_mesh_tex_prefetch();
            const bool terrain_cpu_done = stream_terrain_cpu_done.load();
            if (terrain_cpu_done && stream_terrain_cpu_thread.joinable())
                stream_terrain_cpu_thread.join();

            // Pipeline: GPU-upload tiles as CPU slots appear (don't wait for the whole batch).
            // Prefer terrain GPU over tree queue so budget isn't eaten before the expensive append.
            std::size_t n = 0;
            std::size_t bytes_used = 0;
            std::size_t missing_cpu = 0;
            while (stream_i < stream_add_tiles.size() && budget_ok()) {
                const auto tile_t0 = std::chrono::steady_clock::now();
                const auto& tile = stream_add_tiles[stream_i];
                bool terrain_already = false;
                for (const auto& tc : terrain_chunks) {
                    if (tc.alive && tc.tile_gx == tile.grid_x && tc.tile_gy == tile.grid_y) {
                        terrain_already = true;
                        break;
                    }
                }
                if (terrain_already) {
                    ++stream_i;
                    ++n;
                    omsi::FrameBudget::WorkCost skip{32ull * 1024ull, 0, 0, 0, 0, 0.02};
                    if (gov.admit(frame_budget, omsi::LoadGovernor::Kind::Terrain, skip) ==
                        omsi::FrameBudget::Admit::Defer)
                        break;
                    stream_charge(bytes_used, skip);
                    gov.learn(omsi::LoadGovernor::Kind::Terrain, skip.bytes, 0.02);
                    continue;
                }

                std::optional<editor::LocalTerrainTile> terr;
                bool slot_present = false;
                {
                    std::lock_guard<std::mutex> lk(stream_terrain_cpu_slots_mu);
                    const auto slot_it =
                        stream_terrain_cpu_slots.find(tile_key(tile.grid_x, tile.grid_y));
                    if (slot_it == stream_terrain_cpu_slots.end()) {
                        slot_present = false;
                    } else if (!slot_it->second) {
                        log::warn("stream UploadTerrain build empty tile=(" +
                                  std::to_string(tile.grid_x) + "," + std::to_string(tile.grid_y) + ")");
                        stream_terrain_cpu_slots.erase(slot_it);
                        ++stream_i;
                        ++n;
                        continue;
                    } else {
                        slot_present = true;
                        // Peek only — take after admit succeeds.
                    }
                }
                if (!slot_present) {
                    // Still building this tile — yield; only treat as missing once CPU batch is done.
                    if (terrain_cpu_done) ++missing_cpu;
                    break;
                }

                omsi::FrameBudget::WorkCost cost{};
                cost.bytes = 1024ull * 1024ull;
                cost.vertices = 50000;
                cost.indices = 100000;
                cost.draws = 2;
                // Cold terrain+tex is never a sub-ms job; underpredict → 400 ms Force hitch.
                cost.est_ms = (std::max)(12.0, gov.predict(omsi::LoadGovernor::Kind::Terrain, cost.bytes));
                if (gov.admit(frame_budget, omsi::LoadGovernor::Kind::Terrain, cost) ==
                    omsi::FrameBudget::Admit::Defer)
                    break;

                {
                    std::lock_guard<std::mutex> lk(stream_terrain_cpu_slots_mu);
                    const auto slot_it =
                        stream_terrain_cpu_slots.find(tile_key(tile.grid_x, tile.grid_y));
                    if (slot_it == stream_terrain_cpu_slots.end() || !slot_it->second) {
                        if (terrain_cpu_done) ++missing_cpu;
                        break;
                    }
                    terr = std::move(slot_it->second);
                    stream_terrain_cpu_slots.erase(slot_it);
                }

                ++stream_i;
                ++n;
                float size_m = map::kStandardTileMeters;
                if (const auto* m = world.tile_layout().find_grid(tile.grid_x, tile.grid_y)) size_m = m->size_meters;
                const auto [rx, ry] = tile_rel(tile.grid_x, tile.grid_y);
                // PrepLists has not merged this tile's SLI/SCO into edit_* yet — temporarily
                // append them (editor XY) so AlignTerrain stamps into RAM before the GPU upload.
                const std::size_t sli_mark = edit_splines.size();
                const std::size_t obj_mark = edit_objects.size();
                const std::size_t att_mark = edit_attachments.size();
                const std::size_t sa_mark = edit_spline_attachments.size();
                {
                    std::unordered_set<std::int32_t> have_sli;
                    have_sli.reserve(edit_splines.size() * 2 + tile.splines.size() + 1);
                    for (const auto& s : edit_splines) have_sli.insert(s.id);
                    for (const auto& s : tile.splines) {
                        if (have_sli.count(s.id)) continue;
                        auto c = s;
                        c.position.x += rx;
                        c.position.y += ry;
                        edit_splines.push_back(std::move(c));
                    }
                    std::unordered_set<std::int32_t> have_obj;
                    have_obj.reserve(edit_objects.size() * 2 + tile.objects.size() + 1);
                    for (const auto& o : edit_objects) have_obj.insert(o.id);
                    for (const auto& o : tile.objects) {
                        if (have_obj.count(o.id)) continue;
                        auto c = o;
                        c.position.x += rx;
                        c.position.y += ry;
                        edit_objects.push_back(std::move(c));
                    }
                    std::unordered_map<std::int32_t, std::size_t> temp_object_index;
                    temp_object_index.reserve(edit_objects.size() * 2 + 1);
                    for (std::size_t oi = 0; oi < edit_objects.size(); ++oi)
                        temp_object_index.try_emplace(edit_objects[oi].id, oi);
                    for (const auto& a : tile.attachments) {
                        if (a.align_terrain) {
                            if (const auto it = temp_object_index.find(a.child_id); it != temp_object_index.end())
                                edit_objects[it->second].align_terrain = true;
                        }
                        edit_attachments.push_back(a);
                    }
                    if (!tile.attachments.empty()) attach_pending = true; // hang them on without a camera move
                    for (const auto& sa : tile.spline_attachments) {
                        EditSplineAttachmentEntry e{};
                        e.att = sa;
                        e.tile_gx = tile.grid_x;
                        e.tile_gy = tile.grid_y;
                        e.tile_spline_count = static_cast<std::int32_t>(tile.splines.size());
                        edit_spline_attachments.push_back(std::move(e));
                    }
                }
                // Snapshot the stamping inputs while the tile's SLI/SCO are temporarily merged in edit_*.
                const auto restamp_ctx = make_restamp_context();
                {
                    bool found = false;
                    for (auto& tf : terrain_fields) {
                        if (tf.grid_x != tile.grid_x || tf.grid_y != tile.grid_y) continue;
                        tf.rel_x = rx;
                        tf.rel_y = ry;
                        tf.size_m = size_m;
                        tf.heights_base = terr->heights;
                        tf.restamp_token = 0; // invalidate any in-flight async result for this field
                        found = true;
                        break;
                    }
                    if (!found) {
                        TerrainHeightField field{};
                        field.grid_x = tile.grid_x;
                        field.grid_y = tile.grid_y;
                        field.rel_x = rx;
                        field.rel_y = ry;
                        field.size_m = size_m;
                        field.heights_base = terr->heights;
                        field.heights = terr->heights; // unstamped until the worker result is applied
                        terrain_fields.push_back(std::move(field));
                    }
                }
                edit_splines.resize(sli_mark);
                edit_objects.resize(obj_mark);
                edit_attachments.resize(att_mark);
                edit_spline_attachments.resize(sa_mark);
                {
                    auto tp = std::make_shared<editor::LocalTerrainTile>(std::move(*terr));
                    stream_terrain_q->pending.fetch_add(1);
                    const std::int32_t gx = tile.grid_x, gy = tile.grid_y;
                    const float rx_c = rx, ry_c = ry; // structured bindings cannot be captured (C++17)
                    omsi::JobSystem::global().submit(
                        [q = stream_terrain_q, tp, restamp_ctx, gx, gy, rx_c, ry_c, size_m]() {
                            TerrainGpuDone dn;
                            dn.gx = gx;
                            dn.gy = gy;
                            dn.rx = rx_c;
                            dn.ry = ry_c;
                            dn.texture_path = tp->texture_path;
                            try {
                                restamp_ctx->ensure_prepared();
                                dn.res = restamp_ctx->stamp_tile(tp->heights, rx_c, ry_c, size_m);
                                dn.mesh = tp->mesh;
                                // Uncut 61x61 grid only (3.7 k vertices instead of 1-2 M): the cut mesh is built
                                // later by pump_terrain_detail, and only for tiles within 2 tiles of the camera.
                                editor::apply_heightfield_to_terrain_mesh(dn.mesh, dn.res.heights, size_m);
                                if (!dn.texture_path.empty())
                                    gfx::dx12::prefetch_textures_cpu({dn.texture_path}); // decode off the UI thread
                            } catch (...) {
                                dn.mesh = tp->mesh; // fall back to the unstamped mesh
                            }
                            std::lock_guard<std::mutex> lk(q->mu);
                            q->done.push_back(std::move(dn));
                        },
                        omsi::JobSystem::Priority::High);
                }
                const auto tile_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - tile_t0)
                                         .count();
                frame_budget.charge(tile_ms, cost);
                gov.learn(omsi::LoadGovernor::Kind::Terrain, cost.bytes, tile_ms);
                bytes_used += static_cast<std::size_t>(cost.bytes);
                // [PERF] Hard-abort: a terrain tile dispatches a worker immediately but the
                // main-thread cost (slot peek, set-matrix, push TerrainChunk) can still spike
                // if the slot was taken synchronously. Exit after any tile that busted 2× budget.
                if (tile_ms > frame_budget.cpu_ms_hard * 2.0 && frame_budget.cpu_ms_hard > 0.5) {
                    log::warn("STREAM_HARD_ABORT phase=UploadTerrain frame=" + std::to_string(g_editor_frame_id) +
                              " ms=" + std::to_string(tile_ms) +
                              " budget=" + std::to_string(frame_budget.cpu_ms_hard));
                    break;
                }
                // Keep at most 3 tiles in flight (memory); otherwise let the loop queue more this frame.
                if (stream_terrain_q->pending.load() >= 3) break;
                continue;
            }
            // Fill leftover with tree queue while more CPU tiles cook.
            if (budget_ok()) early_queue_trees_slice();
            if (missing_cpu > 0) {
                bool slots_empty = false;
                {
                    std::lock_guard<std::mutex> lk(stream_terrain_cpu_slots_mu);
                    slots_empty = stream_terrain_cpu_slots.empty();
                }
                if (slots_empty) {
                    if (stream_terrain_cpu_thread.joinable()) stream_terrain_cpu_thread.join();
                    stream_terrain_cpu_started = false;
                    stream_terrain_cpu_done = false;
                    stream_kick_terrain_cpu();
                    stream_status = "CPU terrain retry…";
                } else {
                    stream_status = "GPU add terrain " + std::to_string(stream_i) + "/" +
                                    std::to_string(stream_add_tiles.size());
                }
                return;
            }
            stream_status = !terrain_cpu_done
                                ? ("CPU+GPU terrain " + std::to_string(stream_i) + "/" +
                                   std::to_string(stream_add_tiles.size()))
                                : ("GPU add terrain " + std::to_string(stream_i) + "/" +
                                   std::to_string(stream_add_tiles.size()));
            if (!stream_add_tiles.empty())
                set_stream_progress(0.20f + 0.25f * (static_cast<float>(stream_i) /
                                                     static_cast<float>(stream_add_tiles.size())));
            // Only leave this phase once every add-tile is handled and CPU publish finished.
            if (stream_i >= stream_add_tiles.size() && terrain_cpu_done && stream_terrain_q->pending.load() == 0) {
                stream_i = 0;
                if (!stream_add_meshes.empty()) {
                    {
                        const auto [fx, fy] = stream_sort_focus_xy();
                        sort_add_tiles_by_focus(fx, fy);
                    }
                    stream_phase = StreamPhase::UploadMeshes;
                    stream_status = "GPU add meshes...";
                    stream_kick_mesh_tex_prefetch();
                    log::info("stream UploadMeshes begin n=" + std::to_string(stream_add_meshes.size()));
                } else {
                    stream_goto_after_upload_meshes();
                }
            }
            return;
        }

        if (stream_phase == StreamPhase::UploadMeshes) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.upload_meshes");
            o3d_defer_if_unready = true;
            stream_kick_mesh_tex_prefetch();
            stream_budget_begin();
            std::size_t n = 0;
            std::size_t bytes_used = 0;
            std::size_t tex_waits_avoided = 0;
            std::size_t n_io = 0, n_gpu = 0, n_clone = 0;
            double ms_io = 0.0, ms_gpu = 0.0, ms_clone = 0.0;
            const auto pump_t0 = std::chrono::steady_clock::now();
            std::size_t rotated = 0;
            while (stream_i < stream_add_meshes.size()) {
                const auto& pm = stream_add_meshes[stream_i];
                if (!stream_tile_wanted(pm.tile_grid_x, pm.tile_grid_y)) {
                    ++stream_i;
                    continue;
                }
                if (editor_deleted_object_ids.count(pm.object_id) || !pm.o3d || !pm.o3d->valid()) {
                    ++stream_i;
                    continue;
                }
                const auto stem = pm.source_path.stem().string();
                if (stem == "empty_lod" || stem == "empty") {
                    ++stream_i;
                    continue;
                }
                // ([attachObj] / spline-row meshes of the world loader: no [object] of that id, so they were put
                //  down with no pose at all, at the origin, and blocked the editor's own placement of them -
                //  which hangs each on its parent's final pose, as openOMSI does)
                if (!find_object(world, pm.object_id)) {
                    ++stream_i;
                    continue;
                }
                const model_cfg::ModelCfgMeshEntry* matl = nullptr;
                model_cfg::ModelCfgMeshEntry matl_store{};
                if (const map::MapObjectInst* obj0 = find_object(world, pm.object_id)) {
                    if (const sco::SceneryObject* sco = cached_sco(obj0->sco_path)) {
                        matl_store = sco_matl_cfg(*sco, pm.source_path);
                        matl = &matl_store;
                    }
                }
                auto wcost_peek = estimate_stream_mesh_cost(pm, matl);
                const std::string mesh_key = pm.source_path.string();
                {
                    const auto mesh_admit = gov.admit(
                        frame_budget,
                        wcost_peek.is_clone ? omsi::LoadGovernor::Kind::Generic : omsi::LoadGovernor::Kind::Mesh,
                        wcost_peek, &mesh_key);
                    if (mesh_admit == omsi::FrameBudget::Admit::Defer ||
                        mesh_admit == omsi::FrameBudget::Admit::DeferHeavy) {
                        (void)rotated;
                        break;
                    }
                }
                const auto item_t0 = std::chrono::steady_clock::now();
                ++stream_i;
                ++n;
                {
                    const auto chunk_opt = append_or_clone_o3d(*pm.o3d, pm.source_path, matl, pm.sco_path);
                    if (!chunk_opt) {
                        const auto wait_path = pm.source_path.empty() ? pm.o3d->path() : pm.source_path;
                        if (!o3d::prepared_vertex_cache().ready(wait_path)) {
                            omsi::JobSystem::global().submit(
                                [wait_path]() { o3d::prepared_vertex_cache().ensure_path(wait_path); },
                                omsi::JobSystem::Priority::High);
                            --stream_i;
                            --n;
                            break;
                        }
                        continue;
                    }
                    const std::size_t chunk = *chunk_opt;
                    const map::MapObjectInst* obj = find_object(world, pm.object_id);
                    if (obj) {
                        if (const sco::SceneryObject* sco = cached_sco(obj->sco_path)) {
                            const auto phase = static_cast<gfx::dx12::WorldRenderPhase>(
                                sco::world_render_phase_from_priority(sco->render_priority));
                            const bool surface_bias =
                                sco->render_priority == sco::ScoRenderPriority::Surface || sco->surface;
                            renderer.set_chunk_render_phase(
                                chunk, phase, surface_bias,
                                sco->render_priority == sco::ScoRenderPriority::OnSurface);
                            if (sco->render_priority == sco::ScoRenderPriority::Presurface)
                                renderer.set_chunk_presurface(chunk, true);
                            if (sco->special == sco::ScoSpecial::EntryPoint || sco->special == sco::ScoSpecial::Carpark)
                                renderer.set_chunk_editor_helper(chunk, true);
                        } else if (matl && matl->presurface) {
                            renderer.set_chunk_presurface(chunk, true);
                        }
                    } else if (matl && matl->presurface) {
                        renderer.set_chunk_presurface(chunk, true);
                    }
                    XMMATRIX world_m = XMMatrixIdentity();
                    ScenePlace sp{};
                    sp.object_id = pm.object_id;
                    sp.chunk = chunk;
                    sp.alive = true;
                    sp.is_tree = false;
                    sp.tile_gx = pm.tile_grid_x;
                    sp.tile_gy = pm.tile_grid_y;
                    sp.label = "id=" + std::to_string(pm.object_id) + " " + pm.sco_path.filename().string();
                    if (obj) {
                        const auto [rx, ry] = tile_rel(pm.tile_grid_x, pm.tile_grid_y);
                        bool abs_z = false;
                        bool ground_layer = false;
                        if (const sco::SceneryObject* sco = cached_sco(obj->sco_path)) {
                            abs_z = sco::skip_terrain_add(*sco);
                            ground_layer = sco_ground_layer(*obj, *sco);
                            for (std::size_t mi = 0; mi < sco->meshes.size(); ++mi) {
                                const auto& mr = sco->meshes[mi];
                                if (mr.mesh_file == pm.source_path ||
                                    mr.mesh_file.filename() == pm.source_path.filename()) {
                                    sp.sco_mesh_index = static_cast<int>(mi);
                                    break;
                                }
                            }
                        }
                        const float th = terrain_add(abs_z, obj->position.x + rx, obj->position.y + ry,
                                                     obj->position.z, ground_layer);
                        world_m = sco_world_matrix(*obj, pm.o3d.get(), th, rx, ry);
                        sp.tx = obj->position.x + rx;
                        sp.ty = obj->position.y + ry;
                        sp.tz = obj->position.z + th;
                        sp.abs_z = abs_z;
                    }
                    const auto wt = store_world_transposed(world_m);
                    renderer.set_chunk_world_matrix(chunk, wt.data());
                    renderer.set_chunk_visible(chunk, true);
                    // (the mesh animator poses a mesh from this base: without it the streamed objects'
                    //  animated parts - clock hands, barriers - never moved)
                    if (obj) {
                        DirectX::XMStoreFloat4x4(&sp.base_world, world_m);
                        sp.has_base_world = true;
                    }
                    scene_places.push_back(std::move(sp));
                }
                const double dt = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - item_t0)
                                      .count();
                frame_budget.charge(dt, wcost_peek);
                gov.learn(wcost_peek.is_clone ? omsi::LoadGovernor::Kind::Generic : omsi::LoadGovernor::Kind::Mesh,
                          wcost_peek.bytes, dt, &mesh_key);
                if (dt > frame_budget.target_ms)
                    log::warn("LOAD_SLOW tag=" +
                              std::string(wcost_peek.touches_io ? "io" : (wcost_peek.is_clone ? "apply" : "upload")) +
                              " phase=UploadMeshes frame=" + std::to_string(g_editor_frame_id) +
                              " ms=" + std::to_string(dt) + " est=" + std::to_string(wcost_peek.est_ms) +
                              " slice=" + std::to_string(frame_budget.target_ms) +
                              " kb=" + std::to_string(wcost_peek.bytes / 1024) + " " +
                              pm.source_path.filename().string());
                bytes_used += static_cast<std::size_t>(wcost_peek.bytes);
                // [PERF] Hard-abort: if a single mesh took more than 2× the hard budget,
                // stop immediately so the renderer can present the frame. This distributes
                // the load over more frames without skipping any mesh — stream_i already
                // incremented, so the next frame picks up exactly where this one stopped.
                if (dt > frame_budget.cpu_ms_hard * 2.0 && frame_budget.cpu_ms_hard > 0.5) {
                    log::warn("STREAM_HARD_ABORT phase=UploadMeshes frame=" + std::to_string(g_editor_frame_id) +
                              " ms=" + std::to_string(dt) +
                              " budget=" + std::to_string(frame_budget.cpu_ms_hard) +
                              " " + pm.source_path.filename().string());
                    break;
                }
                if (wcost_peek.is_clone) {
                    ms_clone += dt;
                    ++n_clone;
                } else if (wcost_peek.touches_io) {
                    ms_io += dt;
                    ++n_io;
                } else {
                    ms_gpu += dt;
                    ++n_gpu;
                }
                (void)tex_waits_avoided;
                if (!budget_ok() || frame_budget.expired() || !frame_budget.time_ok()) break;
            }
            const double pump_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - pump_t0)
                                       .count();
            if (n > 0 || frame_budget.deferred > 0 || frame_budget.defer_heavy > 0)
                log::info("stream UploadMeshes breakdown total=" + std::to_string(pump_ms) + " io=" +
                          std::to_string(n_io) + "/" + std::to_string(ms_io) + " gpu=" + std::to_string(n_gpu) +
                          "/" + std::to_string(ms_gpu) + " clone=" + std::to_string(n_clone) + "/" +
                          std::to_string(ms_clone) + " " + frame_budget.summary() + " tot=" +
                          std::to_string(stream_i) + "/" + std::to_string(stream_add_meshes.size()));
            stream_status = "GPU add meshes " + std::to_string(stream_i) + "/" +
                            std::to_string(stream_add_meshes.size());
            if (!stream_add_meshes.empty())
                set_stream_progress(0.45f + 0.15f * (static_cast<float>(stream_i) /
                                                     static_cast<float>(stream_add_meshes.size())));
            if (stream_i >= stream_add_meshes.size()) {
                stream_i = 0;
                stream_goto_after_upload_meshes();
            }
            return;
        }

        if (stream_phase == StreamPhase::PrepShiftGpu) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.prep_shift_gpu");
            stream_budget_begin();
            const float dx = stream_shift_dx;
            const float dy = stream_shift_dy;
            (void)dx;
            (void)dy;
            auto doomed = [&](const std::int32_t gx, const std::int32_t gy) {
                return stream_remove_set.count(tile_key(gx, gy)) != 0;
            };
            // Unload has its own share (OpenOMSI Streamer::update): what the frame has left, at least half a
            // slice — never a fixed count (128/frame left 5000 doomed chunks for 40 slow frames).
            const double unload_ms = (std::max)(gov.remaining_ms(), gov.slice_ms() * 0.5);
            const auto gpu_hide_deadline =
                std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double, std::milli>(unload_ms));
            int gpu_hide_n = 0;
            std::size_t gpu_scan_n = 0;
            // Always time-bound the scan. The old `hide_n==0` bypass scanned 10k+ places with no
            // deadline when doomed tiles sat late in the array (600–1100 ms PrepShiftGpu frames).
            // Allow one index step past the deadline so we never stall when deadline==now.
            auto gpu_hide_ok = [&]() {
                if (gpu_scan_n == 0) return true;
                return std::chrono::steady_clock::now() < gpu_hide_deadline;
            };
            auto pass_total = [&]() -> std::size_t {
                switch (stream_shift_pass) {
                    case 0:
                        return scene_places.size();
                    case 1:
                        return terrain_chunks.size();
                    case 2:
                        return spline_gpu.size();
                    case 3:
                        return end_markers.size();
                    case 4:
                        return path_rails.size();
                    default:
                        return 1;
                }
            };
            auto gpu_progress = [&]() {
                const auto tot = pass_total();
                const float frac = tot ? static_cast<float>(stream_shift_i) / static_cast<float>(tot) : 1.f;
                set_stream_progress(0.60f + 0.08f * ((static_cast<float>(stream_shift_pass) + frac) / 6.f));
            };
            const auto hide_t0 = std::chrono::steady_clock::now();
            auto hide_slice_ms = [&]() {
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hide_t0)
                    .count();
            };

            if (stream_shift_pass == 0) {
                // Tile-indexed hide: only walk places that sit on doomed tiles (not O(all places)).
                static thread_local std::vector<std::size_t> doomed_place_idx;
                if (stream_shift_i == 0 && gpu_scan_n == 0) {
                    doomed_place_idx.clear();
                    doomed_place_idx.reserve(256);
                    for (std::size_t pi = 0; pi < scene_places.size(); ++pi) {
                        const auto& sp = scene_places[pi];
                        if (!sp.alive || sp.pinned) continue;
                        if (spline_attach_spawn_ids.count(sp.object_id)) continue;
                        if (doomed(sp.tile_gx, sp.tile_gy)) doomed_place_idx.push_back(pi);
                    }
                }
                while (stream_shift_i < doomed_place_idx.size() && gpu_hide_ok()) {
                    const std::size_t pi = doomed_place_idx[stream_shift_i++];
                    ++gpu_scan_n;
                    if (pi >= scene_places.size()) continue;
                    auto& sp = scene_places[pi];
                    if (!sp.alive) continue;
                    if (!sp.pinned && !spline_attach_spawn_ids.count(sp.object_id) &&
                        doomed(sp.tile_gx, sp.tile_gy)) {
                        sp.alive = false;
                        if (gpu_proto_chunks.count(sp.chunk))
                            renderer.set_chunk_visible(sp.chunk, false);
                        else
                            renderer.release_chunk_gpu(sp.chunk);
                        ++gpu_hide_n;
                    }
                }
                if (stream_shift_i >= doomed_place_idx.size()) {
                    log::info("stream PrepShiftGpu pass=objects done n=" +
                              std::to_string(doomed_place_idx.size()) + "/" +
                              std::to_string(scene_places.size()) +
                              " slice_ms=" + std::to_string(hide_slice_ms()));
                    stream_shift_i = 0;
                    stream_shift_pass = 1;
                    doomed_place_idx.clear();
                }
                stream_status = std::string("Diff GPU objects ") + std::to_string(stream_shift_i) +
                                "/" + std::to_string(doomed_place_idx.empty() ? scene_places.size()
                                                                               : doomed_place_idx.size());
                gpu_progress();
                if (stream_shift_pass == 0 || !gpu_hide_ok()) return;
            }
            if (stream_shift_pass == 1) {
                while (stream_shift_i < terrain_chunks.size() && gpu_hide_ok()) {
                    auto& tc = terrain_chunks[stream_shift_i++];
                    ++gpu_scan_n;
                    if (!tc.alive) continue;
                    if (doomed(tc.tile_gx, tc.tile_gy)) {
                        tc.alive = false;
                        renderer.release_chunk_gpu(tc.chunk);
                        ++gpu_hide_n;
                    }
                }
                if (stream_shift_i >= terrain_chunks.size()) {
                    log::info("stream PrepShiftGpu pass=terrain done n=" +
                              std::to_string(terrain_chunks.size()) +
                              " slice_ms=" + std::to_string(hide_slice_ms()));
                    stream_shift_i = 0;
                    stream_shift_pass = 2;
                }
                stream_status = std::string("Diff GPU terrain ") + std::to_string(stream_shift_i) +
                                "/" + std::to_string(terrain_chunks.size());
                gpu_progress();
                if (stream_shift_pass == 1 || !gpu_hide_ok()) return;
            }
            if (stream_shift_pass == 2) {
                while (stream_shift_i < spline_gpu.size() && gpu_hide_ok()) {
                    auto& g = spline_gpu[stream_shift_i++];
                    ++gpu_scan_n;
                    if (!g.alive) continue;
                    if (!doomed(g.tile_gx, g.tile_gy)) continue;
                    while (!g.chunks.empty() && gpu_hide_ok()) {
                        renderer.release_chunk_gpu(g.chunks.back());
                        g.chunks.pop_back();
                        ++gpu_hide_n;
                        ++gpu_scan_n;
                    }
                    if (!g.chunks.empty()) {
                        --stream_shift_i;
                        break;
                    }
                    g.alive = false;
                }
                if (stream_shift_i >= spline_gpu.size()) {
                    log::info("stream PrepShiftGpu pass=splines done n=" +
                              std::to_string(spline_gpu.size()) +
                              " slice_ms=" + std::to_string(hide_slice_ms()));
                    stream_shift_i = 0;
                    stream_shift_pass = 3;
                }
                stream_status = std::string("Diff GPU splines ") + std::to_string(stream_shift_i) +
                                "/" + std::to_string(spline_gpu.size());
                gpu_progress();
                if (stream_shift_pass == 2 || !gpu_hide_ok()) return;
            }
            if (stream_shift_pass == 3) {
                while (stream_shift_i < end_markers.size() && gpu_hide_ok()) {
                    auto& mk = end_markers[stream_shift_i++];
                    ++gpu_scan_n;
                    if (!mk.alive) continue;
                    if (!doomed(mk.tile_gx, mk.tile_gy)) continue;
                    mk.alive = false;
                    renderer.release_chunk_gpu(mk.chunk);
                    ++gpu_hide_n;
                }
                if (stream_shift_i >= end_markers.size()) {
                    log::info("stream PrepShiftGpu pass=markers done n=" +
                              std::to_string(end_markers.size()) +
                              " slice_ms=" + std::to_string(hide_slice_ms()));
                    stream_shift_i = 0;
                    stream_shift_pass = 4;
                }
                stream_status = std::string("Diff GPU markers ") + std::to_string(stream_shift_i) +
                                "/" + std::to_string(end_markers.size());
                gpu_progress();
                if (stream_shift_pass == 3 || !gpu_hide_ok()) return;
            }
            if (stream_shift_pass == 4) {
                while (stream_shift_i < path_rails.size() && gpu_hide_ok()) {
                    auto& rail = path_rails[stream_shift_i++];
                    ++gpu_scan_n;
                    if (!rail.alive) continue;
                    if (doomed(rail.tile_gx, rail.tile_gy)) {
                        rail.alive = false;
                        renderer.release_chunk_gpu(rail.chunk);
                        ++gpu_hide_n;
                    }
                }
                if (stream_shift_i >= path_rails.size()) {
                    log::info("stream PrepShiftGpu pass=rails done n=" +
                              std::to_string(path_rails.size()) +
                              " slice_ms=" + std::to_string(hide_slice_ms()));
                    stream_shift_i = 0;
                    stream_shift_pass = 5;
                }
                stream_status = std::string("Diff GPU rails ") + std::to_string(stream_shift_i) + "/" +
                                std::to_string(path_rails.size());
                gpu_progress();
                if (stream_shift_pass == 4 || !gpu_hide_ok()) return;
            }
            // Pass 5: compact heightfields on main (move-filter, ~µs). A worker stole
            // terrain_fields and the same frame then kicked PrepLists (deep tile copy) —
            // toast stuck at 67% "heightfields (bg)" with a hitch. Yield one frame
            // before lists so the copy isn't billed to this status.
            if (stream_shift_pass == 5) {
                if (stream_hf_job) {
                    if (stream_hf_job->thread.joinable()) stream_hf_job->thread.join();
                    if (terrain_fields.empty() && !stream_hf_job->fields.empty())
                        terrain_fields = std::move(stream_hf_job->fields);
                    stream_hf_job.reset();
                }
                {
                    std::size_t w = 0;
                    for (std::size_t i = 0; i < terrain_fields.size(); ++i) {
                        if (stream_remove_set.count(
                                tile_key(terrain_fields[i].grid_x, terrain_fields[i].grid_y)))
                            continue;
                        if (w != i) terrain_fields[w] = std::move(terrain_fields[i]);
                        ++w;
                    }
                    terrain_fields.resize(w);
                }
                refresh_entry_visibility();
                const auto wall_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - stream_prepshift_t0)
                                         .count();
                log::info("stream PrepShiftGpu done wall_ms=" + std::to_string(wall_ms) +
                          " add_meshes=" + std::to_string(stream_add_meshes.size()) +
                          " hf=" + std::to_string(terrain_fields.size()));
                ++editor_obj_tile_epoch;
                editor_visual_cull_cache_dirty = true;
                stream_shift_pass = 6;
                stream_status = "Diff GPU heightfields done";
                set_stream_progress(0.68f);
                return;
            }
            if (stream_shift_pass == 6) {
                stream_begin_prep_lists();
                return;
            }
            return;
        }

        if (stream_phase == StreamPhase::PrepLists) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.prep_lists");
            stream_budget_begin();
            set_stream_progress(0.68f + (stream_lists_job && !stream_lists_job->done.load(std::memory_order_acquire)
                                              ? 0.04f
                                              : 0.08f * static_cast<float>(stream_lists_pass) / 9.0f));
            if (stream_lists_job && !stream_lists_job->done.load(std::memory_order_acquire)) {
                stream_status = "Diff lists (bg)…";
                return;
            }
            if (stream_lists_job) {
                if (stream_lists_job->thread.joinable()) stream_lists_job->thread.join();
                auto& j = *stream_lists_job;
                stream_lists_job_ms = j.job_ms;
                edit_objects = std::move(j.objects);
                xform_soa.rebuild_from(edit_objects);
                edit_splines = std::move(j.splines);
                edit_attachments = std::move(j.attachments);
                attach_pending = true; // the streamed tiles' [attachObj]s: hang them on
                edit_spline_attachments = std::move(j.spline_attachments);
                // attachObj AlignTerrain tag lives on the attachment — mirror onto child SCO.
                // Index IDs once instead of scanning all objects for every attachment.
                {
                    std::unordered_map<std::int32_t, std::size_t> object_index;
                    object_index.reserve(edit_objects.size() * 2 + 1);
                    for (std::size_t oi = 0; oi < edit_objects.size(); ++oi)
                        object_index.try_emplace(edit_objects[oi].id, oi);
                    for (const auto& a : edit_attachments) {
                        if (!a.align_terrain) continue;
                        if (const auto it = object_index.find(a.child_id); it != object_index.end())
                            edit_objects[it->second].align_terrain = true;
                    }
                }
                if (stream_add_tiles.empty() && !j.add_tiles.empty())
                    stream_add_tiles = std::move(j.add_tiles);
                tile_local_spline_edit_idx = std::move(j.local_sli_idx);
                global_spline_index = std::move(j.global_index);
                stream_new_objects = std::move(j.new_objects);
                stream_new_spline_ids = std::move(j.new_spline_ids);
                stream_sa_enqueue_buf = std::move(j.sa_enqueue);
                stream_lists_sa_doomed = std::move(j.sa_doomed);
                stream_sa_touched = j.sa_touched;
                if (j.revived_s > 0)
                    log::info("stream revive dead GPU splines n=" + std::to_string(j.revived_s) +
                              " (worker)");
                if (j.revived_o > 0)
                    log::info("stream revive dead GPU objects n=" + std::to_string(j.revived_o) +
                              " (worker)");
                if (j.dup_s > 0 || j.dup_o > 0)
                    log::warn("IDs duplicados: " + std::to_string(j.dup_s) + " splines, " +
                              std::to_string(j.dup_o) + " objects — revisar mapa");
                {
                    // Merge heightfields by packed tile key rather than a nested scan.
                    std::unordered_map<std::int64_t, std::size_t> field_index;
                    field_index.reserve((terrain_fields.size() + j.extra_heights.size()) * 2 + 1);
                    for (std::size_t fi = 0; fi < terrain_fields.size(); ++fi)
                        field_index.try_emplace(tile_key(terrain_fields[fi].grid_x, terrain_fields[fi].grid_y), fi);
                    for (auto& field : j.extra_heights) {
                        const auto key = tile_key(field.grid_x, field.grid_y);
                        if (const auto it = field_index.find(key); it != field_index.end()) {
                            auto& tf = terrain_fields[it->second];
                            tf.rel_x = field.rel_x;
                            tf.rel_y = field.rel_y;
                            tf.size_m = field.size_m;
                            tf.heights_base = std::move(field.heights_base);
                        } else {
                            const auto idx = terrain_fields.size();
                            terrain_fields.push_back(std::move(field));
                            field_index.emplace(key, idx);
                        }
                    }
                }
                // UploadTerrain ran before these lists existed — restamp RAM then force GPU reload
                // so AlignTerrain under .sco/.sli is visible in viewer/stream (not only after brush).
                stream_lists_job.reset();
                sa_pause_lists = false;
                stream_restamp_i = 0;
                stream_restamp_submitted = false;
                stream_lists_pass = 2;
                stream_i = 0;
                stream_status = "Diff lists (terrain stamp)…";
                return;
            }

            // Pass 2: time-sliced AlignTerrain restamp (avoid 10s+ hitch on big maps).
            if (stream_lists_pass == 2) {
                // Background restamp: one snapshot of the lists, one worker job per tile. The main
                // thread only submits once, then applies finished results (3 vector moves each).
                if (!stream_restamp_submitted) {
                    stream_restamp_submitted = true;
                    terrain_restamp_queue.cancel_all();
                    std::vector<editor::TerrainRestampTileIn> batch;
                    batch.reserve(terrain_fields.size());
                    for (auto& tf : terrain_fields) {
                        if (tf.heights_base.empty()) continue;
                        editor::TerrainRestampTileIn in;
                        in.key = tile_key(tf.grid_x, tf.grid_y);
                        in.token = tf.restamp_token = ++terrain_token_seq;
                        in.heights_base = tf.heights_base;
                        in.rel_x = tf.rel_x;
                        in.rel_y = tf.rel_y;
                        in.size_m = tf.size_m;
                        batch.push_back(std::move(in));
                    }
                    terrain_restamp_queue.submit(make_restamp_context(), std::move(batch));
                }
                for (auto& d : terrain_restamp_queue.drain()) {
                    for (auto& tf : terrain_fields) {
                        if (tile_key(tf.grid_x, tf.grid_y) != d.key) continue;
                        if (tf.restamp_token == d.token) { // else a newer synchronous restamp won
                            apply_restamp_result(tf, std::move(d.result));
                            tf.restamp_token = 0;
                        }
                        ++stream_restamp_i;
                        break;
                    }
                }
                stream_status = "Diff lists (terrain stamp) " + std::to_string(stream_restamp_i) + "/" +
                                std::to_string(terrain_fields.size());
                if (terrain_restamp_queue.outstanding() > 0) return;
                stream_restamp_submitted = false;
                terrain_align_gpu_reload_tiles.clear();
                // All RAM heightfields were restamped by the time-sliced pass above. Only queue
                // GPU-resident terrain chunks; uploading every map tile here was forcing the editor
                // to materialize the whole map in GPU memory and invalidating shadows continuously.
                terrain_align_gpu_reload_tiles.reserve(terrain_chunks.size());
                {
                    std::unordered_set<std::uint64_t> queued_tiles;
                    queued_tiles.reserve(terrain_chunks.size() * 2 + 1);
                    for (const auto& tc : terrain_chunks) {
                        if (!tc.alive) continue;
                        const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tc.tile_gx)) << 32) |
                                         static_cast<std::uint32_t>(tc.tile_gy);
                        if (!queued_tiles.insert(key).second) continue;
                        const bool has_field = std::any_of(terrain_fields.begin(), terrain_fields.end(),
                            [&](const TerrainHeightField& tf) { return tf.grid_x == tc.tile_gx && tf.grid_y == tc.tile_gy; });
                        if (has_field) terrain_align_gpu_reload_tiles.emplace_back(tc.tile_gx, tc.tile_gy);
                    }
                }
                terrain_align_gpu_reload_only = true;
                terrain_align_gpu_needs_restamp = false; // restamp pass above already covered all RAM fields
                terrain_align_gpu_reload_i = 0;
                terrain_align_gpu_dirty = true;
                ++editor_obj_tile_epoch;
                editor_visual_cull_cache_dirty = true;
                stream_lists_pass = 3;
                stream_i = 0;
                stream_status = "Diff lists (SA clear)…";
                return;
            }

            // Pass 3: clear SA GPU spawns (indices from worker).
            if (stream_lists_pass == 3) {
                while (stream_i < stream_lists_sa_doomed.size() &&
                       std::chrono::steady_clock::now() < budget_end) {
                    clear_sa_spawns_for_entry(stream_lists_sa_doomed[stream_i++]);
                }
                stream_status = "Diff lists (SA clear) " + std::to_string(stream_i) + "/" +
                                std::to_string(stream_lists_sa_doomed.size());
                if (stream_i < stream_lists_sa_doomed.size()) return;
                stream_lists_pass = 6;
                stream_i = 0;
                return;
            }

            // Pass 6: global index + enqueue SA (spawn is pumped later with tiny budget).
            if (stream_lists_pass == 6) {
                for (const std::size_t ei : stream_sa_enqueue_buf)
                    fill_sa_parent_spline_id(edit_spline_attachments[ei]);
                if (!stream_sa_enqueue_buf.empty()) enqueue_sa_spawn(stream_sa_enqueue_buf);
                selected_spline = -1;
                selected_object = -1;
                if (stream_keep_sid >= 0) {
                    for (std::size_t i = 0; i < edit_splines.size(); ++i) {
                        if (edit_splines[i].id == stream_keep_sid) {
                            selected_spline = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (stream_keep_oid >= 0) {
                    for (std::size_t i = 0; i < edit_objects.size(); ++i) {
                        if (edit_objects[i].id == stream_keep_oid) {
                            selected_object = static_cast<int>(i);
                            break;
                        }
                    }
                }
                log_watch_scos("streamPrepListsAfter");
                stream_lists_pass = 7;
                stream_status = "Diff lists (remap)…";
                return;
            }

            if (stream_lists_pass == 7) {
                remap_marquee_and_group_after_stream();
                // Mouse may have moved during passes 0-6 — reprioritize before the heavy tail.
                {
                    const auto [fx, fy] = stream_sort_focus_xy();
                    sort_add_tiles_by_focus(fx, fy);
                }
                stream_lists_pass = 8;
                stream_i = 0;
                stream_status = "Diff lists (revive bg)…";
                return;
            }

            // Pass 8: lists already filtered on the worker (revive/dup/near).
            if (stream_lists_pass == 8) {
                const double lists_wall_ms = std::chrono::duration<double, std::milli>(
                                                 std::chrono::steady_clock::now() - stream_lists_t0).count();
                log::info("stream PrepLists done wall_ms=" + std::to_string(lists_wall_ms) +
                          " job_ms=" + std::to_string(stream_lists_job_ms) +
                          " objects=" + std::to_string(edit_objects.size()) +
                          " splines=" + std::to_string(edit_splines.size()) +
                          " terrains=" + std::to_string(terrain_fields.size()) +
                          " new_spline_ids=" + std::to_string(stream_new_spline_ids.size()) +
                          " new_objects=" + std::to_string(stream_new_objects.size()));
                ++editor_obj_tile_epoch;
                editor_visual_cull_cache_dirty = true;
                stream_i = 0;
                stream_tree_i = 0;
                {
                    const auto [fx, fy] = stream_sort_focus_xy();
                    sort_add_tiles_by_focus(fx, fy);
                }
                stream_phase = StreamPhase::UploadScenery;
                stream_status = "GPU add scenery...";
                log::info("stream UploadScenery begin n=" + std::to_string(stream_new_objects.size()));
                // Fan-out CPU prepare on JobSystem workers. Everything that touches disk (load_sco, .o3d
                // vertex prep, tree texture decode) runs here, never on the main thread. Old code did an
                // O(new*edit_objects) scan and a synchronous load_sco per object on the UI thread.
                {
                    stream_sco_requested.clear();
                    stream_sco_wait_frames = 0;
                    struct PrepItem { std::filesystem::path sco; std::string tree_tex; };
                    std::unordered_map<std::int32_t, const map::MapObjectInst*> by_id_prep;
                    by_id_prep.reserve(edit_objects.size() * 2 + 8);
                    for (const auto& o : edit_objects) by_id_prep.emplace(o.id, &o);
                    std::vector<PrepItem> items;
                    std::unordered_set<std::string> seen_prep;
                    for (const auto& no : stream_new_objects) {
                        const auto it = by_id_prep.find(no.id);
                        if (it == by_id_prep.end()) continue;
                        const auto& o = *it->second;
                        if (!seen_prep.insert(o.sco_path.string() + "|" + o.tree_texture).second) continue;
                        items.push_back({o.sco_path, o.tree_texture});
                        stream_sco_requested.insert(o.sco_path.string());
                    }
                    const unsigned nw = (std::max)(1u, omsi::JobSystem::global().worker_count());
                    const std::size_t nitems = items.size();
                    const std::size_t chunk = (std::max)(std::size_t{4}, (nitems + nw - 1) / nw);
                    const auto omsi_root_copy = cfg.omsi_root;
                    for (std::size_t base = 0; base < nitems; base += chunk) {
                        const std::size_t end = (std::min)(nitems, base + chunk);
                        std::vector<PrepItem> part(items.begin() + static_cast<std::ptrdiff_t>(base),
                                                   items.begin() + static_cast<std::ptrdiff_t>(end));
                        omsi::JobSystem::global().submit(
                            [part = std::move(part), omsi_root_copy]() {
                                std::vector<std::filesystem::path> tex_paths;
                                for (const auto& it : part) {
                                    try {
                                        const sco::SceneryObject* sco = sco::load_sco(it.sco);
                                        if (!sco) continue;
                                        if (sco->tree.valid) {
                                            const std::string& tn = !it.tree_tex.empty() ? it.tree_tex : sco->tree.texture;
                                            if (auto tp = gfx::dx12::resolve_texture_path(it.sco, tn, omsi_root_copy))
                                                tex_paths.push_back(*tp);
                                        } else {
                                            for (const auto& m : sco->meshes)
                                                if (m.is_o3d) o3d::prepared_vertex_cache().ensure_path(m.mesh_file);
                                        }
                                    } catch (...) {
                                    }
                                }
                                if (!tex_paths.empty()) gfx::dx12::prefetch_textures_cpu(tex_paths);
                            },
                            omsi::JobSystem::Priority::High);
                    }
                }
                return;
            }
            return;
        }

        if (stream_phase == StreamPhase::UploadScenery) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.upload_scenery");
            o3d_defer_if_unready = true;
            stream_budget_begin();
            // Ensure minimum budget for loading phase to prevent starvation
            frame_budget.cpu_ms_hard = (std::max)(frame_budget.cpu_ms_hard, 4.0);
            frame_budget.cpu_ms_soft = frame_budget.cpu_ms_hard * 0.85;
            std::size_t n = 0;
            std::size_t bytes_used = 0;
            std::unordered_set<std::int32_t> alive_ids;
            alive_ids.reserve(scene_places.size() * 2 + 8);
            for (const auto& sp : scene_places)
                if (sp.alive) alive_ids.insert(sp.object_id);
            std::unordered_map<std::int32_t, map::MapObjectInst*> by_id;
            by_id.reserve(edit_objects.size() * 2 + 8);
            for (auto& o : edit_objects) by_id[o.id] = &o;
            std::size_t rotated = 0;
            while (stream_i < stream_new_objects.size()) {
                const auto& no = stream_new_objects[stream_i];
                if (no.tile_gx != -2000000000 && !stream_tile_wanted(no.tile_gx, no.tile_gy)) {
                    ++stream_i;
                    continue;
                }
                if (alive_ids.count(no.id)) {
                    ++stream_i;
                    continue;
                }
                auto it = by_id.find(no.id);
                if (it == by_id.end()) {
                    ++stream_i;
                    continue;
                }
                map::MapObjectInst* obj = it->second;
                // .sco not parsed yet: never read disk on the main thread. Make sure a worker is loading it,
                // rotate this object to the back and move on. After ~10 s of waiting fall through (missing file).
                if (!sco::peek_sco(obj->sco_path) && stream_sco_wait_frames < 600) {
                    const std::string sk = obj->sco_path.string();
                    if (stream_sco_requested.insert(sk).second) {
                        const auto sp_copy = obj->sco_path;
                        omsi::JobSystem::global().submit(
                            [sp_copy]() {
                                try { (void)sco::load_sco(sp_copy); } catch (...) {}
                            },
                            omsi::JobSystem::Priority::High);
                    }
                    const std::size_t left_sco = stream_new_objects.size() - stream_i;
                    if (rotated >= left_sco) {
                        ++stream_sco_wait_frames;
                        break; // everything left is still loading: yield this frame
                    }
                    std::rotate(stream_new_objects.begin() + static_cast<std::ptrdiff_t>(stream_i),
                                stream_new_objects.begin() + static_cast<std::ptrdiff_t>(stream_i + 1),
                                stream_new_objects.end());
                    ++rotated;
                    continue;
                }
                omsi::FrameBudget::WorkCost cost = kTreeCost;
                cost.est_ms = 0.05;
                if (const sco::SceneryObject* sco = cached_sco(obj->sco_path)) {
                    if (!sco->tree.valid) {
                        const auto& hw = omsi::HardwareProfile::global();
                        cost.bytes = 0;
                        cost.vertices = 0;
                        cost.indices = 0;
                        cost.draws = 0;
                        bool all_ready = !sco->meshes.empty();
                        for (const auto& mesh_ref : sco->meshes) {
                            ++cost.draws;
                            if (mesh_ref.is_o3d) {
                                if (const auto* pv = o3d::prepared_vertex_cache().try_get(mesh_ref.mesh_file)) {
                                    cost.vertices += pv->size();
                                    cost.bytes += pv->size() * sizeof(o3d::Vertex);
                                } else {
                                    all_ready = false;
                                    cost.bytes += 64ull * 1024ull;
                                    cost.vertices += 512;
                                }
                            } else {
                                cost.bytes += 32ull * 1024ull;
                            }
                        }
                        if (cost.bytes == 0) cost.bytes = 32ull * 1024ull;
                        if (cost.draws == 0) cost.draws = 1;
                        const int n_files = static_cast<int>((std::max)(std::size_t{1}, sco->meshes.size()));
                        if (all_ready) {
                            cost.est_ms = static_cast<double>(cost.bytes) /
                                          (static_cast<double>(hw.memcpy_bps) / 1000.0) +
                                          0.25;
                            // First GPU spawn still pays texture bind / PSO — floor above "free".
                            cost.est_ms = (std::max)(cost.est_ms, 1.25);
                            cost.weight = 1.0;
                            cost.touches_io = false;
                        } else {
                            cost.est_ms = 0.2 + static_cast<double>(n_files) * hw.disk_open_ms +
                                          (static_cast<double>(cost.bytes) / (hw.disk_read_mbps * 1024.0 * 1024.0)) *
                                              1000.0;
                            // Cold SCO open is never a sub-ms job; underpredict → Force hitch.
                            cost.est_ms = (std::max)(cost.est_ms, 4.0 + static_cast<double>(n_files) * 1.5);
                            cost.weight = hw.io_weight;
                            cost.touches_io = true;
                        }
                    }
                }
                bool assets_pending = false;
                if (const sco::SceneryObject* sco = cached_sco(obj->sco_path); sco && !sco->tree.valid) {
                    for (const auto& mesh_ref : sco->meshes) {
                        if (mesh_ref.mesh_file.empty()) continue;
                        if (mesh_ref.is_o3d) {
                            if (o3d_assets_ready(mesh_ref.mesh_file)) continue;
                            submit_o3d_assets(mesh_ref.mesh_file);
                            assets_pending = true;
                        } else {
                            const std::string xk = "x:" + mesh_ref.mesh_file.lexically_normal().string();
                            if (other_assets_ready(xk)) continue;
                            submit_other_assets(xk, mesh_ref.mesh_file, {});
                            assets_pending = true;
                        }
                    }
                } else if (sco && sco->tree.valid) {
                    const std::string& tn = !obj->tree_texture.empty() ? obj->tree_texture : sco->tree.texture;
                    if (!tn.empty()) {
                        if (const auto tp = gfx::dx12::resolve_texture_path(obj->sco_path, tn, cfg.omsi_root)) {
                            const std::string tk = "t:" + tp->lexically_normal().string();
                            if (!other_assets_ready(tk)) {
                                submit_other_assets(tk, {}, *tp);
                                assets_pending = true;
                            }
                        }
                    }
                }
                const std::string asset_key = obj->sco_path.string();
                if (assets_pending) {
                    const std::size_t left = stream_new_objects.size() - stream_i;
                    if (rotated >= left) {
                        // Full rotate still pending assets — skip this object so the phase advances.
                        log::warn("stream UploadScenery skip-pending-assets id=" +
                                  std::to_string(no.id) + " " + obj->sco_path.filename().string());
                        ++stream_i;
                        continue;
                    }
                    std::rotate(stream_new_objects.begin() + static_cast<std::ptrdiff_t>(stream_i),
                                stream_new_objects.begin() + static_cast<std::ptrdiff_t>(stream_i + 1),
                                stream_new_objects.end());
                    ++rotated;
                    continue;
                }
                // Livelock guard (same as UploadSplines): after 3 frames that rotated the whole
                // queue without one admit, force one object; its assets were prefetched by then.
                static int s_sco_yield_frames = 0;
                auto scenery_admit =
                    gov.admit(frame_budget, omsi::LoadGovernor::Kind::Scenery, cost, &asset_key);
                if (scenery_admit == omsi::FrameBudget::Admit::DeferHeavy && n == 0 && s_sco_yield_frames >= 3) {
                    log::info("stream UploadScenery force-admit heavy id=" + std::to_string(no.id));
                    scenery_admit = omsi::FrameBudget::Admit::Force;
                }
                if (scenery_admit == omsi::FrameBudget::Admit::Defer) break;
                if (scenery_admit == omsi::FrameBudget::Admit::DeferHeavy) {
                    // Prefetch and rotate — normally never Force a cold I/O SCO onto this frame.
                    if (const sco::SceneryObject* sco = cached_sco(obj->sco_path); sco && !sco->tree.valid) {
                        for (const auto& mesh_ref : sco->meshes) {
                            if (!mesh_ref.is_o3d) continue;
                            submit_o3d_assets(mesh_ref.mesh_file);
                            const auto p = mesh_ref.mesh_file;
                            omsi::JobSystem::global().submit(
                                [p]() { o3d::prepared_vertex_cache().ensure_path(p); },
                                omsi::JobSystem::Priority::High);
                        }
                    }
                    const std::size_t left = stream_new_objects.size() - stream_i;
                    if (rotated >= left) {
                        // Full queue rotated with zero Interactive admits: yield the frame.
                        // Never Force a cold SCO onto the UI thread (was 600+ ms SPIKING).
                        // Idle/Loading Force still comes from LoadGovernor when mode allows.
                        if (n == 0) ++s_sco_yield_frames;
                        log::info("stream UploadScenery yield-after-rotate pending=" +
                                  std::to_string(left) + " id=" + std::to_string(no.id));
                        break;
                    }
                    std::rotate(stream_new_objects.begin() + static_cast<std::ptrdiff_t>(stream_i),
                                stream_new_objects.begin() + static_cast<std::ptrdiff_t>(stream_i + 1),
                                stream_new_objects.end());
                    ++rotated;
                    continue;
                }
                s_sco_yield_frames = 0;
                const auto item_t0 = std::chrono::steady_clock::now();
                ++stream_i;
                ++n;
                try {
                    {
                        auto _w_spawn = omsi::MainThreadWatchdog::active_scope("stream.upload_scenery.spawn_gpu");
                        spawn_one_object_gpu(*obj, no.tile_gx, no.tile_gy);
                    }
                    if (!object_has_alive_gpu(no.id)) {
                        bool waiting = false;
                        if (const sco::SceneryObject* sco = cached_sco(obj->sco_path)) {
                            if (!sco->tree.valid) {
                                for (const auto& mesh_ref : sco->meshes) {
                                    if (!mesh_ref.is_o3d) continue;
                                    if (!o3d::prepared_vertex_cache().ready(mesh_ref.mesh_file)) {
                                        waiting = true;
                                        const auto p = mesh_ref.mesh_file;
                                        omsi::JobSystem::global().submit(
                                            [p]() { o3d::prepared_vertex_cache().ensure_path(p); },
                                            omsi::JobSystem::Priority::High);
                                    }
                                }
                            }
                        }
                        if (waiting) {
                            const std::size_t left = stream_new_objects.size() - stream_i;
                            if (rotated >= left) {
                                // Assets never became ready after a full rotate — skip so
                                // UploadScenery can finish and clear the overlay.
                                log::warn("stream UploadScenery skip-unready id=" +
                                          std::to_string(no.id) + " " +
                                          obj->sco_path.filename().string());
                                alive_ids.insert(no.id);
                            } else {
                                --stream_i;
                                --n;
                                std::rotate(stream_new_objects.begin() +
                                                static_cast<std::ptrdiff_t>(stream_i),
                                            stream_new_objects.begin() +
                                                static_cast<std::ptrdiff_t>(stream_i + 1),
                                            stream_new_objects.end());
                                ++rotated;
                                continue;
                            }
                        } else {
                            alive_ids.insert(no.id);
                        }
                    } else {
                        alive_ids.insert(no.id);
                    }
                } catch (const std::exception& ex) {
                    log::warn("stream UploadScenery exception id=" + std::to_string(no.id) + " " +
                              obj->sco_path.filename().string() + " what=" + ex.what());
                } catch (...) {
                    log::warn("stream UploadScenery unknown exception id=" + std::to_string(no.id) + " " +
                              obj->sco_path.filename().string());
                }
                const double dt = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - item_t0)
                                      .count();
                frame_budget.charge(dt, cost);
                gov.learn(omsi::LoadGovernor::Kind::Scenery, cost.bytes, dt, &asset_key);
                if (dt > frame_budget.target_ms)
                    log::warn("LOAD_SLOW tag=create phase=UploadScenery frame=" +
                              std::to_string(g_editor_frame_id) + " ms=" + std::to_string(dt) +
                              " est=" + std::to_string(cost.est_ms) +
                              " slice=" + std::to_string(frame_budget.target_ms) +
                              " kb=" + std::to_string(cost.bytes / 1024) + " " + obj->sco_path.filename().string());
                bytes_used += static_cast<std::size_t>(cost.bytes);
                // [PERF] Hard-abort: scenery object creation can occasionally spike (e.g. large trees/buildings).
                // Break immediately if a single item takes >2× hard budget to maintain smooth 60 FPS rendering.
                if (dt > frame_budget.cpu_ms_hard * 2.0 && frame_budget.cpu_ms_hard > 0.5) {
                    log::warn("STREAM_HARD_ABORT phase=UploadScenery frame=" + std::to_string(g_editor_frame_id) +
                              " ms=" + std::to_string(dt) +
                              " budget=" + std::to_string(frame_budget.cpu_ms_hard) +
                              " " + obj->sco_path.filename().string());
                    break;
                }
                if (!budget_ok() || frame_budget.expired() || !frame_budget.time_ok()) break;
            }
            stream_pump_queued_trees();
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_t0)
                                .count();
            if (n > 0 || frame_budget.deferred > 0 || frame_budget.defer_heavy > 0)
                log::info("stream UploadScenery breakdown total=" + std::to_string(ms) + " n=" +
                          std::to_string(n) + " " + frame_budget.summary() + " tot=" +
                          std::to_string(stream_i) + "/" + std::to_string(stream_new_objects.size()));
            // Toast uses stream_i only (not max with trees) so it matches tot=i/n in logs.
            const std::size_t scenery_n = stream_new_objects.size();
            const std::size_t scenery_i = stream_i;
            stream_status = "GPU add scenery " + std::to_string(scenery_i) + "/" +
                            std::to_string(scenery_n);
            if (scenery_n > 0)
                set_stream_progress(0.82f + 0.03f * (static_cast<float>(scenery_i) /
                                                     static_cast<float>(scenery_n)));
            if (stream_i >= stream_new_objects.size()) {
                stream_i = 0;
                o3d_defer_if_unready = false;
                stream_phase = StreamPhase::UploadSplines;
                stream_status = "GPU add splines...";
                stream_kick_spline_cpu();
                stream_spline_warm_kicked = false;
                stream_spline_warm_done.store(true, std::memory_order_release);
                log::info("stream UploadSplines begin n=" + std::to_string(stream_new_spline_ids.size()));
            }
            return;
        }

        if (stream_phase == StreamPhase::UploadSplines) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.upload_splines");
            if (stream_spline_cpu_started && !stream_spline_cpu_done.load()) {
                stream_status = "CPU remesh splines " + std::to_string(stream_new_spline_ids.size()) +
                                " (parallel)…";
                return;
            }
            if (stream_spline_cpu_thread.joinable() && !stream_spline_cpu_done) {
                stream_status = "CPU remesh splines…";
                return;
            }
            if (stream_spline_cpu_thread.joinable()) {
                auto _w_join = omsi::MainThreadWatchdog::active_scope("stream.upload_splines.thread_join");
                stream_spline_cpu_thread.join();
            }
            static auto s_upl_spl_t0 = std::chrono::steady_clock::now();
            static int s_upl_spl_frames = 0;
            static std::size_t s_upl_spl_chunks0 = 0;
            if (stream_i == 0 && !stream_spline_warm_kicked) {
                auto _w_warm = omsi::MainThreadWatchdog::active_scope("stream.upload_splines.warm_textures");
                s_upl_spl_t0 = std::chrono::steady_clock::now();
                s_upl_spl_frames = 0;
                s_upl_spl_chunks0 = renderer.chunk_count();
                stream_spline_warm_kicked = true;
                // CPU prefetch on workers only — never sync-decode + GPU-ensure all spline
                // textures on this frame (was 200 ms+ stream_apply with warm_tex=32).
                std::vector<std::filesystem::path> warm_tex;
                std::unordered_set<std::string> warm_seen;
                warm_tex.reserve(stream_spline_cpu_slots.size() * 2);
                std::unordered_map<std::int32_t, const map::SplineInst*> warm_spline_by_id;
                warm_spline_by_id.reserve(edit_splines.size() * 2 + 1);
                for (const auto& s : edit_splines) warm_spline_by_id.try_emplace(s.id, &s);
                for (const auto& slot : stream_spline_cpu_slots) {
                    if (!slot.ok) continue;
                    const auto spit_it = warm_spline_by_id.find(slot.id);
                    if (spit_it == warm_spline_by_id.end() || !spit_it->second) continue;
                    const auto* spit = spit_it->second;
                    for (const auto& mesh : slot.meshes) {
                        if (mesh.texture_path.empty()) continue;
                        const auto resolved =
                            gfx::dx12::resolve_texture_path(spit->sli_path, mesh.texture_path.string(), cfg.omsi_root);
                        if (!resolved) continue;
                        if (!warm_seen.insert(resolved->string()).second) continue;
                        warm_tex.push_back(*resolved);
                    }
                }
                if (!warm_tex.empty()) {
                    stream_spline_warm_done.store(false, std::memory_order_release);
                    const auto n_warm = warm_tex.size();
                    omsi::JobSystem::global().submit(
                        [tex = std::move(warm_tex), &done = stream_spline_warm_done]() {
                            gfx::dx12::prefetch_textures_cpu(tex);
                            done.store(true, std::memory_order_release);
                        },
                        omsi::JobSystem::Priority::Low);
                    log::info("stream UploadSplines warm_tex=" + std::to_string(n_warm) + " (async, wait)");
                    stream_status = "CPU warm spline textures…";
                    return;
                }
                stream_spline_warm_done.store(true, std::memory_order_release);
            }
            if (!stream_spline_warm_done.load(std::memory_order_acquire)) {
                stream_status = "CPU warm spline textures…";
                return;
            }
            if (stream_i == 0 && s_upl_spl_frames == 0) s_upl_spl_t0 = std::chrono::steady_clock::now();
            ++s_upl_spl_frames;
            {
                const double burst_ms = omsi::stream_spline_gpu_burst_ms();
                frame_budget.begin_phase(burst_ms);
                frame_budget.target_ms = burst_ms;
                frame_budget.cpu_ms_hard = burst_ms;
                frame_budget.cpu_ms_soft = burst_ms * 0.9;
                frame_budget.slice_max_ms = burst_ms;
                frame_budget.target_bytes = 128ull << 20;
                frame_budget.upload_bytes = frame_budget.geometry_bytes = frame_budget.target_bytes;
                frame_budget.target_weighted =
                    static_cast<double>(frame_budget.target_bytes) * frame_budget.io_weight;
                budget_end = frame_budget.deadline;
                if (s_upl_spl_frames == 1)
                    log::info("stream UploadSplines burst_ms=" + std::to_string(burst_ms) + " gov_slice=" +
                              std::to_string(gov.slice_ms()));
            }
            std::size_t n = 0;
            std::size_t bytes_used = 0;
            std::size_t spl_rotated = 0;
            // Perf: these two id->index maps were rebuilt over ALL edit_splines every frame of the phase
            // (thousands of splines x 100+ frames). Keep them across frames; rebuild only when the vector
            // changed (size/data pointer) or an entry no longer matches its id.
            static std::unordered_map<std::int32_t, std::size_t> s_slot_idx, s_spl_idx;
            static std::size_t s_slot_built_n = static_cast<std::size_t>(-1), s_spl_built_n = static_cast<std::size_t>(-1);
            static const void* s_slot_built_p = nullptr;
            static const void* s_spl_built_p = nullptr;
            if (stream_i == 0) { // new upload phase: always start from a fresh index
                s_slot_built_n = s_spl_built_n = static_cast<std::size_t>(-1);
            }
            auto idx_lookup = [](auto& vec, std::unordered_map<std::int32_t, std::size_t>& idx,
                                 std::size_t& built_n, const void*& built_p,
                                 std::int32_t sid) -> decltype(&vec[0]) {
                auto rebuild = [&]() {
                    idx.clear();
                    idx.reserve(vec.size() * 2 + 8);
                    for (std::size_t i = 0; i < vec.size(); ++i) idx[vec[i].id] = i;
                    built_n = vec.size();
                    built_p = static_cast<const void*>(vec.data());
                };
                if (built_n != vec.size() || built_p != static_cast<const void*>(vec.data())) rebuild();
                auto it = idx.find(sid);
                if (it == idx.end()) return nullptr;
                if (it->second < vec.size() && vec[it->second].id == sid) return &vec[it->second];
                rebuild(); // contents shifted under the same size/data
                it = idx.find(sid);
                return it == idx.end() ? nullptr : &vec[it->second];
            };
            while (stream_i < stream_new_spline_ids.size()) {
                const std::int32_t sid = stream_new_spline_ids[stream_i];
                map::SplineInst* sp = idx_lookup(edit_splines, s_spl_idx, s_spl_built_n, s_spl_built_p, sid);
                StreamSplineCpuSlot* slot =
                    idx_lookup(stream_spline_cpu_slots, s_slot_idx, s_slot_built_n, s_slot_built_p, sid);
                omsi::FrameBudget::WorkCost sc;
                sc.bytes = 8ull * 1024ull;
                sc.draws = 2;
                sc.est_ms = 0.2;
                if (slot && slot->ok) {
                    sc.bytes = estimate_spline_slot_bytes(slot->meshes);
                    for (const auto& sm : slot->meshes) {
                        sc.vertices += sm.vertices.size();
                        sc.indices += sm.indices.size();
                    }
                    const auto bps = omsi::HardwareProfile::global().memcpy_bps;
                    sc.est_ms = static_cast<double>(sc.bytes) / (static_cast<double>(bps) / 1000.0) + 0.15;
                }
                // No visual-radius skip here: a spline outside the radius at the moment of the hop was
                // dropped for good, and the radius fallback (ensure_visual_radius_gpu) does not run
                // while streaming - driving on, the roads ahead never appeared. The wanted-tile set
                // already bounds what is uploaded.
                if (!sp || !stream_tile_wanted_spline(*sp) || !stream_spline_upload_ok(*sp)) {
                    ++stream_i;
                    continue;
                }
                sc.est_ms = (std::max)(sc.est_ms, gov.predict(omsi::LoadGovernor::Kind::Spline, sc.bytes));
                // Livelock guard: a spline whose predicted cost never fits the slice was rotated
                // forever ("yield-after-rotate" every frame, load never finished). The mesh is
                // already built on a worker, so after 2 frames with zero admits force one item.
                static int s_spl_yield_frames = 0;
                auto admit = frame_budget.try_admit(sc);
                if (admit == omsi::FrameBudget::Admit::DeferHeavy && n == 0 && s_spl_yield_frames >= 2) {
                    log::info("stream UploadSplines force-admit heavy id=" + std::to_string(sid) +
                              " est_ms=" + std::to_string(sc.est_ms));
                    admit = omsi::FrameBudget::Admit::Force;
                }
                if (admit == omsi::FrameBudget::Admit::DeferHeavy) {
                    // Rotate like UploadScenery — never skip forever / Force cold GPU work.
                    const std::size_t left = stream_new_spline_ids.size() - stream_i;
                    if (spl_rotated >= left) {
                        if (n == 0) ++s_spl_yield_frames;
                        log::info("stream UploadSplines yield-after-rotate pending=" +
                                  std::to_string(left) + " id=" + std::to_string(sid));
                        break;
                    }
                    std::rotate(stream_new_spline_ids.begin() + static_cast<std::ptrdiff_t>(stream_i),
                                stream_new_spline_ids.begin() + static_cast<std::ptrdiff_t>(stream_i + 1),
                                stream_new_spline_ids.end());
                    ++spl_rotated;
                    continue;
                }
                if (admit == omsi::FrameBudget::Admit::Defer) break;
                s_spl_yield_frames = 0;
                const auto item_t0 = std::chrono::steady_clock::now();
                ++stream_i;
                ++n;
                bool ok = false;
                if (slot && slot->ok) {
                    bool meshes_ok = !slot->meshes.empty();
                    for (const auto& m : slot->meshes) {
                        if (m.vertices.empty() || m.indices.empty()) {
                            meshes_ok = false;
                            break;
                        }
                    }
                    if (meshes_ok) {
                        auto _w_mesh = omsi::MainThreadWatchdog::active_scope("stream.upload_splines.apply_gpu_mesh");
                        ok = apply_spline_gpu_meshes(*sp, slot->meshes, false, true);
                    }
                }
                if (ok) {
                    if (auto* g = find_spline_gpu(sid)) {
                        g->alive = true;
                        const std::size_t max_chunks = renderer.chunk_count();
                        const bool mesh_vis = !spline_mesh_hidden_onlyeditor(*sp);
                        for (const auto c : g->chunks) {
                            if (c >= max_chunks) continue;
                            renderer.set_chunk_visible(c, mesh_vis);
                            renderer.set_chunk_opacity(c, 1.f);
                            renderer.set_chunk_wireframe(c, false);
                        }
                    }
                }
                const double dt = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - item_t0)
                                      .count();
                frame_budget.charge(dt, sc);
                gov.learn(omsi::LoadGovernor::Kind::Spline, sc.bytes, dt);
                if (dt > frame_budget.target_ms && sp)
                    log::warn("LOAD_SLOW tag=upload phase=UploadSplines frame=" +
                              std::to_string(g_editor_frame_id) + " ms=" + std::to_string(dt) +
                              " est=" + std::to_string(sc.est_ms) +
                              " slice=" + std::to_string(frame_budget.target_ms) +
                              " kb=" + std::to_string(sc.bytes / 1024) + " " + sp->sli_path.filename().string());
                bytes_used += static_cast<std::size_t>(sc.bytes);
                // [PERF] Hard-abort: spline mesh uploads can exceed estimate by 10-100×
                // when geometry is complex. Break immediately to protect frame time.
                if (dt > frame_budget.cpu_ms_hard * 2.0 && frame_budget.cpu_ms_hard > 0.5) {
                    log::warn("STREAM_HARD_ABORT phase=UploadSplines frame=" + std::to_string(g_editor_frame_id) +
                              " ms=" + std::to_string(dt) +
                              " budget=" + std::to_string(frame_budget.cpu_ms_hard));
                    break;
                }
                if (!budget_ok() || frame_budget.expired() || !frame_budget.time_ok()) break;
            }
            {
                auto _w_flush = omsi::MainThreadWatchdog::active_scope("stream.upload_splines.flush_gpu_uploads");
                renderer.flush_batched_mesh_uploads();
            }
            if (n > 0 && s_upl_spl_frames % 30 == 0)
                log::info("stream UploadSplines slice n=" + std::to_string(n) + " i=" +
                          std::to_string(stream_i) + "/" + std::to_string(stream_new_spline_ids.size()) +
                          " budget=" + frame_budget.summary());
            if (stream_i < stream_new_spline_ids.size() && n == 0 && !budget_ok())
                log::warn("stream UploadSplines budget exhausted i=" + std::to_string(stream_i) + "/" +
                          std::to_string(stream_new_spline_ids.size()));
            stream_status = "GPU add splines " + std::to_string(stream_i) + "/" +
                            std::to_string(stream_new_spline_ids.size());
            if (!stream_new_spline_ids.empty())
                set_stream_progress(0.85f + 0.10f * (static_cast<float>(stream_i) /
                                                     static_cast<float>(stream_new_spline_ids.size())));
            if (stream_i >= stream_new_spline_ids.size()) {
                log::info("stream UploadSplines done n=" + std::to_string(stream_new_spline_ids.size()) +
                          " chunks+=" + std::to_string(renderer.chunk_count() - s_upl_spl_chunks0) + " frames=" +
                          std::to_string(s_upl_spl_frames) + " ms=" +
                          std::to_string(std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - s_upl_spl_t0)
                                             .count()));
                stream_phase = StreamPhase::Finish;
                stream_finish_pass = 0;
                stream_i = 0;
                stream_status = "Stream finish…";
            }
            return;
        }

        if (stream_phase == StreamPhase::Finish) {
            auto _w_sub = omsi::MainThreadWatchdog::active_scope("stream.finish");
            // Spread marker/list refresh across frames — was a second freeze after Diff lists.
            if (stream_finish_pass == 0) {
                map::compute_spline_length_accum(edit_splines);
                log_watch_scos("streamFinish");
                log_pos_track("stream", "finish", tile_gx, tile_gy, tile_ox, tile_oy);
                log::info("stream Finish begin marker_splines=" +
                          std::to_string(stream_new_spline_ids.size()) + " edit_splines=" +
                          std::to_string(edit_splines.size()));
                stream_finish_pass = 1;
                stream_i = 0;
                stream_status = "Stream markers…";
                set_stream_progress(0.95f);
                return;
            }
            if (stream_finish_pass == 1) {
                if (view_only || stream_new_spline_ids.empty()) {
                    stream_finish_pass = 2;
                    stream_i = 0;
                    stream_status = view_only ? "Stream markers (view-only skip)…" : "Stream markers (none)…";
                    set_stream_progress(0.99f);
                    return;
                }
                stream_budget_begin();
                std::size_t n = 0;
                std::unordered_map<std::int32_t, const map::SplineInst*> by_sid;
                by_sid.reserve(edit_splines.size() * 2 + 8);
                for (const auto& s : edit_splines) by_sid[s.id] = &s;
                while (stream_i < stream_new_spline_ids.size()) {
                    omsi::FrameBudget::WorkCost mc{8ull * 1024ull, 0, 0, 1, 0};
                    mc.est_ms = 0.08;
                    if (!stream_work_ok(mc)) break;
                    const std::int32_t sid = stream_new_spline_ids[stream_i++];
                    auto it = by_sid.find(sid);
                    if (it == by_sid.end()) continue;
                    const map::SplineInst* sp = it->second;
                    if (editor_visual_radius_m > 0.f && !visual_in_radius_spline(*sp))
                        continue;
                    ensure_spline_end_markers(*sp);
                    stream_work_consume(mc);
                    ++n;
                }
                if (stream_i < stream_new_spline_ids.size() && n == 0 && !budget_ok())
                    log::warn("stream Finish markers budget exhausted i=" + std::to_string(stream_i) + "/" +
                              std::to_string(stream_new_spline_ids.size()));
                stream_status = "Stream markers " + std::to_string(stream_i) + "/" +
                                std::to_string(stream_new_spline_ids.size());
                set_stream_progress(0.95f + 0.04f * (static_cast<float>(stream_i) /
                                                       static_cast<float>(stream_new_spline_ids.size())));
                if (stream_i >= stream_new_spline_ids.size()) {
                    log::info("stream Finish markers done n=" + std::to_string(stream_new_spline_ids.size()));
                    stream_finish_pass = 2;
                    stream_i = 0;
                }
                return;
            }
            if (stream_finish_pass == 2) {
                stream_budget_begin();
                std::unordered_set<std::int32_t> live_sli;
                std::unordered_set<std::int32_t> live_obj;
                live_sli.reserve(edit_splines.size() * 2);
                live_obj.reserve(edit_objects.size() * 2);
                for (const auto& s : edit_splines) live_sli.insert(s.id);
                for (const auto& o : edit_objects) live_obj.insert(o.id);
                std::size_t hidden = 0;
                while (stream_i < end_markers.size()) {
                    omsi::FrameBudget::WorkCost hc{4ull * 1024ull, 0, 0, 1, 0, 0.04};
                    if (!stream_work_ok(hc)) break;
                    auto& mk = end_markers[stream_i++];
                    if (!mk.alive) {
                        stream_work_consume(hc);
                        continue;
                    }
                    const bool live = mk.from_object ? live_obj.count(mk.owner_id) != 0
                                                     : live_sli.count(mk.owner_id) != 0;
                    if (live) {
                        stream_work_consume(hc);
                        continue;
                    }
                    const auto t0 = std::chrono::steady_clock::now();
                    mk.alive = false;
                    renderer.release_chunk_gpu(mk.chunk);
                    ++hidden;
                    frame_budget.charge(
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), hc);
                    if (frame_budget.expired() || !frame_budget.time_ok()) break;
                }
                if (stream_i < end_markers.size()) {
                    stream_status = "Stream finish markers " + std::to_string(stream_i) + "/" +
                                    std::to_string(end_markers.size());
                    return;
                }
                log::info("stream Finish orphan markers hidden=" + std::to_string(hidden) + "/" +
                          std::to_string(end_markers.size()));
                stream_finish_pass = 3;
                stream_i = 0;
                stream_status = "Stream rails…";
                set_stream_progress(0.995f);
                return;
            }
            if (stream_finish_pass == 3) {
                stream_finish_pass = 4;
                stream_status = "Stream lists…";
                return;
            }
            if (stream_finish_pass == 4) {
                const auto t_lists = std::chrono::steady_clock::now();
                if (editor_lists_defer) editor_lists_dirty = true;
                else refresh_editor_lists();
                if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                               t_lists).count();
                    ms > 10.0)
                    log::warn("STREAM_FINISH_SLOW step=refresh_editor_lists ms=" + std::to_string(ms));
                stream_finish_pass = 5;
                stream_status = "Stream scripts…";
                return;
            }
            if (stream_finish_pass == 5) {
                const auto t_p5 = std::chrono::steady_clock::now();
                recreate_marquee_from_blueprint();
                remap_marquee_and_group_after_stream();
                const auto t_sync = std::chrono::steady_clock::now();
                scenery_scripts.sync(edit_objects, cfg.omsi_root);
                {
                    const auto now = std::chrono::steady_clock::now();
                    const double marquee_ms = std::chrono::duration<double, std::milli>(t_sync - t_p5).count();
                    const double sync_ms = std::chrono::duration<double, std::milli>(now - t_sync).count();
                    if (marquee_ms + sync_ms > 10.0)
                        log::warn("STREAM_FINISH_SLOW step=marquee+scripts marquee_ms=" + std::to_string(marquee_ms) +
                                  " scripts_sync_ms=" + std::to_string(sync_ms));
                }
                if (selected_spline >= 0 &&
                    static_cast<std::size_t>(selected_spline) < edit_splines.size())
                    show_spline_at(static_cast<std::size_t>(selected_spline));
                else if (selected_object >= 0 &&
                         static_cast<std::size_t>(selected_object) < edit_objects.size())
                    show_object_at(static_cast<std::size_t>(selected_object));
                stream_finish_pass = 6;
                return;
            }
            if (stream_finish_pass == 6) {
                if (!stream_try_join_cpu_workers()) {
                    stream_status = "Stream join workers…";
                    return;
                }
                stream_finish_pass = 7;
                return;
            }
            if (stream_finish_pass == 7) {
                log::info("tile stream diff done tiles=" + std::to_string(world.tiles().size()) +
                          " add_meshes=" + std::to_string(stream_add_meshes.size()) + " remove=" +
                          std::to_string(stream_remove_grids.size()) + " center=(" +
                          std::to_string(tile_gx) + "," + std::to_string(tile_gy) + ")");
                enqueue_unspawned_sa_entries();
                if (show_path_rails) sync_path_rails_after_stream();
                {
                    const auto& hid_o = panel.imgui_state().hidden_object_ids;
                    const auto& hid_s = panel.imgui_state().hidden_spline_ids;
                    if (!hid_o.empty() || !hid_s.empty()) {
                        for (auto& sp : scene_places) {
                            if (!sp.alive) continue;
                            if (!hid_o.count(sp.object_id)) continue;
                            if (gpu_proto_chunks.count(sp.chunk))
                                renderer.set_chunk_visible(sp.chunk, false);
                            else
                                renderer.release_chunk_gpu(sp.chunk);
                        }
                        for (auto& g : spline_gpu) {
                            if (!g.alive) continue;
                            if (!hid_s.count(g.id)) continue;
                            for (const auto c : g.chunks) renderer.release_chunk_gpu(c);
                        }
                    }
                }
                stream_add_meshes.clear();
                stream_add_tiles.clear();
                stream_remove_grids.clear();
                stream_new_objects.clear();
                stream_finish_pass = 0;
                stream_phase = StreamPhase::Idle;
                stream_status.clear();
                set_stream_progress(0.f);
                stream_cooldown_frames = 2;
                if (stream_queued_focus && stream_queued_force) {
                    auto f = *stream_queued_focus;
                    stream_queued_focus.reset();
                    stream_queued_force = false;
                    if (stream_diff_needed(f)) begin_stream_diff(f);
                } else {
                    kick_queued_stream();
                }
            }
        }
    };

