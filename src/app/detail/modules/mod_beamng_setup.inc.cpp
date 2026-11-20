// BeamNG.drive backend inside the editor (setup part; included at the end of setup_06).
// TSStatic instances become ordinary edit_objects whose sco_path is "vfs:<shape.dae>", so selection, G/R, groups,
// list editing, copy/paste and undo work unchanged. Spawning / pose sync of those objects goes through the hooks
// below (absolute Z, rotationMatrix + scale), meshes come from the async AssetPool, the terrain is the level's
// .ter split into LOD patches built on worker threads.
struct BngObjExtra {
    float scale[3]{1.f, 1.f, 1.f};
    std::uint32_t level_index = UINT32_MAX; // SceneObject this editor object came from (UINT32_MAX = new)
    float orig_yaw = 0.f, orig_pitch = 0.f, orig_bank = 0.f; // pose as loaded (keep the exact matrix if untouched)
};
struct BngShapeGpu {
    std::vector<std::size_t> protos; // one hidden prototype chunk per LOD, [0] = most detailed
    std::vector<int> sizes;          // Torque detail size (pixels) of each LOD
    bool failed = false;
    float radius = 1.f;              // bounding sphere radius (shape space)
    float height = 1.f;
    bool ready() const { return !protos.empty(); }
};
struct BngInst {
    std::vector<std::size_t> lod_chunks; // clone per LOD (SIZE_MAX = not created yet)
    int cur = -1;                        // LOD shown now (-1 = hidden)
    // Frame of the last LOD change: bng_pick_lod keeps the current LOD for at least 8 frames so a camera
    // standing on a threshold cannot alternate between two clones (the remaining flicker of rocks / trees).
    std::uint64_t lod_frame = 0;
};
struct BngTerrainPatch {
    int px = 0, py = 0;          // patch grid index
    int stride = 0;              // current LOD (samples per quad)
    int want_stride = 0;
    std::size_t chunk = static_cast<std::size_t>(-1);
    bool building = false;
};
// DecalRoad / River / WaterBlock / WaterPlane drawn as ribbons built on worker threads
struct BngRibbon {
    std::uint32_t level_index = 0;
    int kind = 0;                 // 0 DecalRoad, 1 River, 2 WaterBlock, 3 WaterPlane
    std::string material;
    float center[2]{0.f, 0.f};
    float radius = 0.f;
    std::size_t chunk = static_cast<std::size_t>(-1);
    bool building = false;
    bool visible = false;
    bool failed = false;
    // Roads that exist for the game's AI only (material "road_invisible" / DefaultDecalRoadMaterial, or a
    // material that no .materials.json defines): the ribbon stays in the list (selectable / editable, its node
    // polyline and selection edge are still drawn) but no chunk is uploaded, so nothing is drawn in 3D.
    bool no_draw = false;
    bool over_objects = false;    // DecalRoad.overObjects: also projects onto meshes (bridges, ramps)
    float max_above_terrain = 0.f; // highest node above the terrain (decides if draping is needed)
    float fade_start = 0.f;       // DecalRoad.distanceFade[0]: distance where the road starts to fade
    float fade_range = 0.f;       // DecalRoad.distanceFade[1]; 0 = field missing (visible up to load radius)
    float render_priority = 10.f; // DecalRoad.renderPriority: draw order inside a job batch
    // Road polylines (x, y, half width) of DecalRoad / River: ground cover is discarded over them (Root E).
    std::vector<float> node_xy;
};
// Decals (main.decals.json) drawn as quads: one chunk per decal type and 64 m cell, built on worker threads
struct BngDecalCell {
    int type = -1;                // index into bng::Level::decal_types
    int gx = 0, gy = 0;           // 64 m cell index
    std::uint32_t first = 0;      // range in BngRuntime::decal_ids
    std::uint32_t count = 0;
    float center[2]{0.f, 0.f};
    float radius = 0.f;
    float render_priority = 1.f;
    float fade_start = 50.f;      // BeamNG decal fade start (m); DecalData.fadeStartPixelSize when present
    float fade_end = 75.f;        // fully faded at this distance (m)
    std::size_t chunk = static_cast<std::size_t>(-1);
    bool building = false;
    bool visible = false;
    bool failed = false;
};
// GroundCover (grass / shrubs billboards) generated per 32 m cell around the camera
struct BngGroundCoverType {
    float uv[4]{0.f, 0.f, 1.f, 1.f}; // billboardUVs x, y, w, h in the atlas
    int layer = -1;                  // terrain material index (-1 = any)
    float probability = 1.f;
    float size_min = 1.f, size_max = 1.f, size_exp = 1.f;
    float max_slope_deg = 90.f;
};
struct BngGroundCover {
    std::vector<BngGroundCoverType> types;
    std::string texture;   // "vfs:..." atlas
    float radius = 150.f;  // dissolveRadius
    float density = 0.f;   // elements per m^2 at densityScale 1
    float z_offset = 0.f;
    std::uint32_t seed = 1;
};
struct BngRuntime {
    std::shared_ptr<bng::Session> s;
    std::unique_ptr<bng::AssetPool> pool;
    std::unordered_map<std::int32_t, BngObjExtra> extra;            // editor object id -> extra data
    std::unordered_map<std::string, BngShapeGpu> shapes;             // shape -> GPU prototype
    std::unordered_map<std::string, std::vector<std::int32_t>> waiting; // shape -> object ids waiting for the mesh
    std::unordered_map<std::int32_t, BngInst> chunk_of;              // object id -> its clones / current LOD
    std::unordered_set<std::int32_t> spawned;                        // currently visible (inside the radius)
    std::size_t stream_cursor = 0;
    // [PERF] Per-object streaming cache: the shape name is resolved once and the shape-space bounds are kept as
    // soon as the GPU shape reports them. The frame loop asks for the radius of thousands of objects every
    // frame; rebuilding the shape string + rehashing the shape map there dominated the streaming pass.
    struct BngObjShapeCache {
        std::string shape;   // sco_path without the "vfs:" prefix (resolved once per object id)
        float radius = 0.f;  // shape bounding-sphere radius (0 = GPU shape still unknown)
        float height = 0.f;
    };
    std::unordered_map<std::int32_t, BngObjShapeCache> obj_shape;
    float stream_cam_x = 1.e30f, stream_cam_y = 1.e30f; // last streaming centre (static camera -> smaller slices)
    // forest (render-only for now)
    std::unordered_map<std::uint32_t, BngInst> forest_chunk;
    std::unordered_set<std::uint32_t> forest_spawned;
    std::unordered_map<std::string, std::vector<std::uint32_t>> forest_waiting;
    std::size_t forest_cursor = 0;
    // terrain
    std::vector<BngTerrainPatch> patches;
    int patches_x = 0, patches_y = 0;
    int patch_samples = 128;
    std::string terrain_texture;
    float lod_cx = 1e30f, lod_cy = 1e30f;
    struct PatchMesh {
        std::vector<o3d::Vertex> vertices;
        std::vector<std::uint32_t> indices;           // grouped by terrain layer
        std::vector<std::pair<int, std::uint32_t>> layer_ranges; // (layer, index count) in order
    };
    struct PatchJob {
        int patch = -1;
        int stride = 0;
        std::future<PatchMesh> fut;
    };
    // per terrain layer: detail textures + tiling (uv 0..1 spans the whole terrain)
    struct TerrainLayerTex {
        std::string detail, detail_normal;
        float tiles = 0.f;
    };
    std::vector<TerrainLayerTex> terrain_layers;
    std::vector<PatchJob> patch_jobs;
    float radius = 1000.f;
    float detail_adjust = 1.f;   // $pref::TS::detailAdjust (Mesh quality)
    float lod_scale = 1.f;       // $pref::Terrain::lodScale
    int q_mesh = 2, q_texture = 2, q_shadows = 2;
    // prefabs: one editor object, children drawn as instances that follow its matrix
    std::unordered_map<std::string, std::shared_ptr<std::vector<bng::PrefabChild>>> prefab_defs;
    std::unordered_map<std::int32_t, std::vector<BngInst>> prefab_inst;
    std::unordered_map<std::string, std::vector<std::int32_t>> prefab_waiting;
    std::vector<BngRibbon> ribbons;
    std::vector<int> ribbon_order; // ribbon indices sorted by renderPriority (ascending)
    std::size_t ribbon_cursor = 0;
    struct RibbonJob {
        int idx = -1;
        std::future<bng::RibbonMesh> fut;
    };
    std::vector<RibbonJob> ribbon_jobs;
    // built ribbons waiting to be draped over meshes before upload
    struct DrapeItem {
        int idx = -1;
        bng::RibbonMesh mesh;
        std::size_t cursor = 0;
    };
    std::deque<DrapeItem> drape;
    // decals: instances grouped by type and 64 m cell (quads built on workers, uploaded on surface)
    std::vector<BngDecalCell> decals;
    std::vector<std::uint32_t> decal_ids; // instance indices, grouped like the cells
    std::vector<int> decal_order;        // cells sorted by renderPriority (ascending)
    std::size_t decal_cursor = 0;
    struct DecalJob {
        int idx = -1;
        std::future<bng::RibbonMesh> fut;
    };
    std::vector<DecalJob> decal_jobs;
    std::vector<BngGroundCover> ground_covers;
    float gc_density_scale = 0.75f; // $pref::GroundCover::densityScale
    struct GcCell {
        std::size_t chunk = static_cast<std::size_t>(-1);
        bool building = false, visible = false, empty = false;
        float density_built = -1.f;
        // [Root E] Real surface heights for this 32 m cell, sampled on the MAIN thread with
        // renderer.pick_surface_hit_z on a 2 m grid (grass must sit on the drawn terrain, not on the
        // bilinear heightfield). BNG_GC_GRID_STEP = 32 / kGcGrid = 2 m; kBlocked = no grass here.
        std::vector<float> surf;
        std::uint32_t surf_done = 0;
    };
    std::unordered_map<std::uint64_t, GcCell> gc_cells; // key: cover index | cell x | cell y
    struct GcJob {
        std::uint64_t key = 0;
        float density = 0.f;
        std::future<std::pair<std::vector<o3d::Vertex>, std::vector<std::uint32_t>>> fut;
    };
    std::vector<GcJob> gc_jobs;
    int sel_ribbon = -1;          // road selected in the Carreteras tab
    int sel_node = -1;
    int sel_forest = -1;                         // forest instance selected in the viewport (Root D)
    std::size_t sel_forest_chunk = static_cast<std::size_t>(-1); // its current highlight chunk
    int sel_gc = -1;                             // GroundCover type selected in the viewport (aggregated instance)
    std::uint64_t sel_gc_key = 0;                // and the 32 m cell it was clicked on
    std::size_t sel_gc_chunk = static_cast<std::size_t>(-1); // its current highlight chunk
    bool roads_tab_open = false;  // nodes are drawn in the viewport while the tab is visible
    std::unordered_set<std::uint32_t> dirty_objects; // level objects edited (saved with Ctrl+S)
    std::unordered_map<std::uint32_t, std::int32_t> eid_of_level;
    double last_stream_t = 0.0;
    bool shim_hidden = false;
    std::size_t uploaded_shapes = 0;
};
std::unique_ptr<BngRuntime> bng_rt;

auto bng_is_obj = [](const map::MapObjectInst& o) {
    const auto& n = o.sco_path.native();
    return n.size() > 4 && n[0] == L'v' && n[1] == L'f' && n[2] == L's' && n[3] == L':';
};
auto bng_shape_of = [](const map::MapObjectInst& o) { return o.sco_path.generic_string().substr(4); };

// Column-vector world matrix (row-major) == the transposed DX matrix set_chunk_world_matrix expects.
auto bng_world16 = [&](const map::MapObjectInst& o, float out16[16]) {
    float sc[3]{1.f, 1.f, 1.f};
    if (bng_rt) {
        if (auto it = bng_rt->extra.find(o.id); it != bng_rt->extra.end()) std::memcpy(sc, it->second.scale, sizeof(sc));
    }
    bng::Mat3 r = bng::editor_euler_to_mat3(o.rotation_deg.x, o.rotation_deg.y, o.rotation_deg.z);
    // untouched objects keep their exact original matrix (no euler round-off)
    if (bng_rt) {
        if (auto it = bng_rt->extra.find(o.id); it != bng_rt->extra.end() && it->second.level_index != UINT32_MAX &&
                                                 std::fabs(it->second.orig_yaw - o.rotation_deg.x) < 1e-4f &&
                                                 std::fabs(it->second.orig_pitch - o.rotation_deg.y) < 1e-4f &&
                                                 std::fabs(it->second.orig_bank - o.rotation_deg.z) < 1e-4f) {
            r = bng_rt->s->level->objects[it->second.level_index].rot;
        }
    }
    const float pos[3]{o.position.x, o.position.y, o.position.z};
    bng::mat3_to_world4x4(r, sc, pos, out16);
};

// ── shared helpers (Root A/D/E/F) ────────────────────────────────────────────────────────────────────────
// Optional ProcMeshSubrange material fields added by the shader/material pass (A). A templated lambda makes
// the condition dependent, so `if constexpr` really discards the branch when the header does not have them yet.
auto bng_sub_material = [](auto& s, const float base_rgba[4], float opacity_factor, int alpha_ref, bool double_sided,
                           const std::filesystem::path& mask) {
    if constexpr (requires { s.base_color[0] = 1.f; }) std::memcpy(s.base_color, base_rgba, sizeof(float) * 4);
    if constexpr (requires { s.opacity_factor = 1.f; }) s.opacity_factor = opacity_factor;
    if constexpr (requires { s.alpha_ref = 0; }) s.alpha_ref = alpha_ref;
    if constexpr (requires { s.double_sided = false; }) s.double_sided = double_sided;
    if constexpr (requires { s.mask = std::filesystem::path{}; }) s.mask = mask;
};

// Terrain height with the SAME triangulation the renderer draws (diagonal b-c of each heightfield quad), so the
// grass and the visible terrain patch agree (Root E). Near the camera patches use stride 1, i.e. this grid.
auto bng_terrain_tri_z = [](const bng::TerrainData& t, float x, float y) -> float {
    const int n = static_cast<int>(t.size);
    if (n <= 1) return 0.f;
    const float fx = (x - t.origin[0]) / t.square_size;
    const float fy = (y - t.origin[1]) / t.square_size;
    const int ix = (std::clamp)(static_cast<int>(std::floor(fx)), 0, n - 2);
    const int iy = (std::clamp)(static_cast<int>(std::floor(fy)), 0, n - 2);
    const float u = (std::clamp)(fx - static_cast<float>(ix), 0.f, 1.f);
    const float v = (std::clamp)(fy - static_cast<float>(iy), 0.f, 1.f);
    auto h = [&](int sx, int sy) {
        sx = (std::clamp)(sx, 0, n - 1);
        sy = (std::clamp)(sy, 0, n - 1);
        return t.height_at_sample(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
    };
    const float z00 = h(ix, iy), z10 = h(ix + 1, iy), z01 = h(ix, iy + 1), z11 = h(ix + 1, iy + 1);
    // patch builder: triangles (a,b,c) and (b,d,c) => diagonal p10-p01
    if (u + v <= 1.f) return z00 + u * (z10 - z00) + v * (z01 - z00);
    return z11 + (1.f - u) * (z01 - z11) + (1.f - v) * (z10 - z11);
};

// True when (x, y) lies on a DecalRoad / River polyline (half width included): no grass on the road (Root E).
auto bng_point_on_road = [&bng_rt](float x, float y) -> bool {
    if (!bng_rt) return false;
    for (const auto& rb : bng_rt->ribbons) {
        if (rb.kind > 1 || rb.node_xy.size() < 6) continue;
        if (std::hypot(x - rb.center[0], y - rb.center[1]) > rb.radius + 8.f) continue;
        const std::size_t n = rb.node_xy.size() / 3;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const float ax = rb.node_xy[i * 3], ay = rb.node_xy[i * 3 + 1], aw = rb.node_xy[i * 3 + 2];
            const float bx = rb.node_xy[(i + 1) * 3], by = rb.node_xy[(i + 1) * 3 + 1], bw = rb.node_xy[(i + 1) * 3 + 2];
            const float ex = bx - ax, ey = by - ay;
            const float l2 = ex * ex + ey * ey;
            const float tt = l2 > 1e-6f ? (std::clamp)(((x - ax) * ex + (y - ay) * ey) / l2, 0.f, 1.f) : 0.f;
            const float px = ax + ex * tt - x, py = ay + ey * tt - y;
            const float w = aw + (bw - aw) * tt;
            if (px * px + py * py <= w * w) return true;
        }
    }
    return false;
};
// Ground cover height grid: one node every 2 m over each 32 m cell (sampled with pick_surface_hit_z).
constexpr int kBngGcGrid = 16;
constexpr float kBngGcCell = 32.f;

// Torque LOD pick: projected pixel size of the bounding sphere x detailAdjust vs each LOD's detail size.
// Hysteresis: 12 % band around the current LOD (0.88 x threshold to drop a level, 1.12 x to grab it back),
// plus a minimum of 8 frames between two LOD changes of the same instance (`inst`), so a camera sitting on a
// threshold cannot toggle clones every frame.
auto bng_pick_lod = [&](const BngShapeGpu& g, float scale_max, float dist, int cur, const BngInst* inst = nullptr) -> int {
    if (g.protos.size() <= 1) return 0;
    const float screen_h = (std::max)(240.f, ImGui::GetIO().DisplaySize.y);
    const float focal = screen_h / (2.f * std::tan(0.5f * 0.7853982f)); // editor vertical FOV 45 deg
    // Distance to the SURFACE of the bounding sphere, not to its centre (a 500 m road mesh is close at 400 m).
    const float d_eff = (std::max)(dist - g.radius * scale_max, 0.5f);
    const float px = g.radius * scale_max * focal / d_eff * (bng_rt ? bng_rt->detail_adjust : 1.f);
    constexpr std::uint64_t kLodHoldFrames = 8; // min frames between two LOD changes of one instance
    if (inst && cur >= 0 && g_editor_frame_id < inst->lod_frame + kLodHoldFrames) return cur;
    int want = static_cast<int>(g.protos.size()) - 1;
    for (int i = 0; i < static_cast<int>(g.protos.size()); ++i) {
        // 12 % hysteresis downwards: stay on the current LOD until 12 % below its threshold
        const float thr = static_cast<float>(g.sizes[static_cast<std::size_t>(i)]) * (i == cur ? 0.88f : 1.f);
        if (px >= thr) {
            want = i;
            break;
        }
    }
    // 12 % hysteresis upwards too: going back to a FINER LOD needs 12 % above its threshold, otherwise the
    // pair of clones still ping-pongs every kLodHoldFrames frames when the camera sits right on the threshold.
    if (cur >= 0 && want < cur && want >= 0 &&
        px < static_cast<float>(g.sizes[static_cast<std::size_t>(want)]) * 1.12f)
        want = cur;
    return want;
};

auto bng_obj_scale_max = [&](std::int32_t id) {
    if (auto it = bng_rt->extra.find(id); it != bng_rt->extra.end())
        return (std::max)({std::fabs(it->second.scale[0]), std::fabs(it->second.scale[1]), std::fabs(it->second.scale[2])});
    return 1.f;
};

// Per-object streaming cache (shape name resolved once, bounding sphere kept once the GPU shape knows it).
auto bng_obj_cache = [&](const map::MapObjectInst& o) -> BngRuntime::BngObjShapeCache& {
    static BngRuntime::BngObjShapeCache empty; // bng_rt is always alive in the BeamNG editor (defensive)
    if (!bng_rt) return empty;
    auto& c = bng_rt->obj_shape[o.id];
    if (c.shape.empty()) c.shape = bng_shape_of(o); // built once per object, never again in the hot loop
    if (c.radius <= 0.f && !c.shape.empty()) {
        // Only latch bounds from a shape that really has them: R.shapes[shape] is created (radius = 1 m default)
        // by the forest / prefab paths long before the mesh is uploaded, and a latched 1 m radius would bring
        // back the "huge mesh only loads when the camera reaches its centre" bug for those shapes.
        if (auto g = bng_rt->shapes.find(c.shape);
            g != bng_rt->shapes.end() && (g->second.ready() || g->second.failed) && g->second.radius > 0.f) {
            c.radius = (std::max)(0.25f, g->second.radius);
            c.height = (std::max)(0.25f, g->second.height);
        }
    }
    return c;
};

// Streaming / LOD radius of one TSStatic: cached shape bounding-sphere radius x max scale (0.5 m fallback when
// the shape is still unknown, e.g. a prefab root or a mesh that has not been uploaded yet).
auto bng_obj_radius = [&](const map::MapObjectInst& o) -> float {
    if (!bng_rt) return 0.5f;
    const auto& c = bng_obj_cache(o);
    if (c.radius <= 0.f) return 0.5f;
    return (std::max)(0.25f, c.radius * bng_obj_scale_max(o.id));
};
// True once the real bounding sphere of the object's shape is known (see bng_obj_radius's 0.5 m fallback).
auto bng_obj_bounds_known = [&](const map::MapObjectInst& o) -> bool {
    return bng_rt && bng_obj_cache(o).radius > 0.f;
};
// GPU shape of a TSStatic, resolved through the same cache (no std::string rebuilt per frame).
auto bng_obj_shape_gpu = [&](const map::MapObjectInst& o) -> const BngShapeGpu* {
    if (!bng_rt) return nullptr;
    const auto& c = bng_obj_cache(o);
    if (c.shape.empty()) return nullptr;
    const auto it = bng_rt->shapes.find(c.shape);
    return it == bng_rt->shapes.end() ? nullptr : &it->second;
};

// 2D streaming distance used by the radius streaming: centre distance minus the object radius, like the
// ribbons. A huge object (city block, road-system mesh) is visible while any part of it is inside the radius.
// (Inlined in the frame loop, which computes it from the per-object cache in one go.)

// Shows LOD `lod` of an instance (creating the clone on first use) with world matrix m16.
// Exactly ONE clone per instance is visible: every other LOD clone is hidden in the same frame the new one is
// shown (the old code only hid inst.cur, so a clone created while the bookkeeping disagreed stayed on screen).
auto bng_show_inst = [&](BngInst& inst, const BngShapeGpu& g, int lod, const float m16[16]) -> std::size_t {
    lod = (std::clamp)(lod, 0, static_cast<int>(g.protos.size()) - 1);
    // Never swap to a LOD whose prototype has no GPU mesh / batches yet (upload still in flight): keep the LOD
    // that is already on screen, otherwise the instance flickers through a mesh that draws with the white
    // placeholder until the next upload lands.
    auto proto_ready = [&](int i) {
        return i >= 0 && i < static_cast<int>(g.protos.size()) && renderer.chunk_has_gpu(g.protos[static_cast<std::size_t>(i)]);
    };
    if (!proto_ready(lod)) {
        if (proto_ready(inst.cur) && static_cast<std::size_t>(inst.cur) < inst.lod_chunks.size())
            return inst.lod_chunks[static_cast<std::size_t>(inst.cur)];
        int fallback = -1;
        for (std::size_t k = 0; k < g.protos.size() && fallback < 0; ++k)
            if (proto_ready(static_cast<int>(k))) fallback = static_cast<int>(k);
        if (fallback < 0) return static_cast<std::size_t>(-1);
        lod = fallback;
    }
    if (inst.lod_chunks.size() < g.protos.size()) inst.lod_chunks.resize(g.protos.size(), static_cast<std::size_t>(-1));
    auto& c = inst.lod_chunks[static_cast<std::size_t>(lod)];
    if (c == static_cast<std::size_t>(-1)) {
        if (auto nc = renderer.clone_chunk(g.protos[static_cast<std::size_t>(lod)])) c = *nc;
        else return static_cast<std::size_t>(-1);
        // clone_chunk copies the prototype state (visible = true): keep it hidden until its matrix is set.
        renderer.set_chunk_visible(c, false);
    }
    for (std::size_t k = 0; k < inst.lod_chunks.size(); ++k) {
        if (k == static_cast<std::size_t>(lod)) continue;
        const auto other = inst.lod_chunks[k];
        if (other != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(other, false);
    }
    const bool lod_changed = inst.cur != lod;
    renderer.set_chunk_world_matrix(c, m16);
    renderer.set_chunk_visible(c, true);
    inst.cur = lod;
    if (lod_changed) inst.lod_frame = g_editor_frame_id; // starts the 8-frame hysteresis of bng_pick_lod
    return c;
};
auto bng_hide_inst = [&](BngInst& inst) {
    if (inst.cur >= 0 && static_cast<std::size_t>(inst.cur) < inst.lod_chunks.size()) {
        const auto c = inst.lod_chunks[static_cast<std::size_t>(inst.cur)];
        if (c != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(c, false);
    }
    inst.cur = -1;
};

// scene_places is append-only (dead chunks are marked alive=false, never erased), so the id -> indices index
// built by the setup can be topped up incrementally here. Walking the places of one object then costs
// O(meshes of that object) instead of O(all scene chunks); hide / sync / clone run thousands of times a frame.
auto bng_scene_index_sync = [&]() {
    if (scene_places.size() < scene_places_indexed_n) { // defensive: the vector was replaced
        scene_place_indices_by_object.clear();
        scene_places_indexed_n = 0;
    }
    for (std::size_t i = scene_places_indexed_n; i < scene_places.size(); ++i) {
        const auto id = scene_places[i].object_id;
        if (id != 0) scene_place_indices_by_object[id].push_back(i);
    }
    scene_places_indexed_n = scene_places.size();
};
// Indices in scene_places that belong to one object (empty when it has none yet).
auto bng_scene_places_of = [&](std::int32_t id) -> const std::vector<std::size_t>& {
    static const std::vector<std::size_t> kNone;
    if (!bng_rt) return kNone;
    bng_scene_index_sync();
    const auto it = scene_place_indices_by_object.find(id);
    return it == scene_place_indices_by_object.end() ? kNone : it->second;
};

auto bng_place_clone = [&](map::MapObjectInst& o, const BngShapeGpu& g, int lod) {
    if (!g.ready()) return;
    float w[16];
    bng_world16(o, w);
    auto& inst = bng_rt->chunk_of[o.id];
    const std::size_t chunk = bng_show_inst(inst, g, lod, w);
    if (chunk == static_cast<std::size_t>(-1)) return;
    const float smax = bng_obj_scale_max(o.id);
    bool found = false;
    for (const std::size_t i : bng_scene_places_of(o.id)) {
        if (i >= scene_places.size()) continue;
        auto& sp = scene_places[i];
        sp.chunk = chunk; // picking / highlight follow the visible LOD
        sp.alive = true;
        sp.tx = o.position.x;
        sp.ty = o.position.y;
        sp.tz = o.position.z;
        found = true;
    }
    if (!found) {
        ScenePlace sp{};
        sp.object_id = o.id;
        sp.chunk = chunk;
        sp.tile_gx = tile_gx;
        sp.tile_gy = tile_gy;
        sp.tx = o.position.x;
        sp.ty = o.position.y;
        sp.tz = o.position.z;
        sp.abs_z = true;
        sp.alive = true;
        sp.pinned = true;
        sp.half_xy = (std::max)(0.25f, g.radius * smax);
        sp.height = (std::max)(0.25f, g.height * smax);
        scene_places.push_back(std::move(sp));
    }
    bng_rt->spawned.insert(o.id);
};

auto bng_is_prefab = [](const map::MapObjectInst& o) {
    const auto& n = o.sco_path.native();
    auto ends = [&](const wchar_t* suf) {
        const std::size_t l = std::wcslen(suf);
        return n.size() >= l && _wcsicmp(n.c_str() + n.size() - l, suf) == 0;
    };
    return ends(L".prefab") || ends(L".prefab.json");
};

// Places / refreshes every child of a prefab (missing meshes are requested and placed when they arrive).
auto bng_place_prefab = [&](map::MapObjectInst& o) {
    const std::string file = bng_shape_of(o);
    auto& def = bng_rt->prefab_defs[file];
    if (!def) def = std::make_shared<std::vector<bng::PrefabChild>>(bng::load_prefab(*bng_rt->s->vfs, file, bng_rt->s->info.dir));
    float pw[16];
    bng_world16(o, pw);
    auto& insts = bng_rt->prefab_inst[o.id];
    if (insts.size() < def->size()) insts.resize(def->size());
    float eye[3];
    camera.eye_position(eye[0], eye[1], eye[2]);
    std::vector<std::size_t> child_chunk(def->size(), static_cast<std::size_t>(-1)); // one slot per CHILD
    for (std::size_t k = 0; k < def->size(); ++k) {
        const auto& c = (*def)[k];
        auto& g = bng_rt->shapes[c.shape];
        if (g.failed) continue;
        if (!g.ready()) {
            // same 3D distance to the eye as the frame pump (Root F: 2D distance to the orbit target made
            // requests and placement disagree and the prefab popped in and out)
            const float rx = o.position.x - eye[0], ry = o.position.y - eye[1], rz = o.position.z - eye[2];
            bng_rt->pool->request(c.shape, std::sqrt(rx * rx + ry * ry + rz * rz));
            auto& w = bng_rt->prefab_waiting[c.shape];
            if (std::find(w.begin(), w.end(), o.id) == w.end()) w.push_back(o.id);
            continue;
        }
        float cl[16], cw[16];
        bng::mat3_to_world4x4(c.rot, c.scale, c.pos, cl);
        for (int r = 0; r < 4; ++r)
            for (int q = 0; q < 4; ++q) {
                float v = 0.f;
                for (int m = 0; m < 4; ++m) v += pw[r * 4 + m] * cl[m * 4 + q];
                cw[r * 4 + q] = v;
            }
        const float dx = cw[3] - eye[0], dy = cw[7] - eye[1], dz = cw[11] - eye[2];
        const float smax = (std::max)({std::fabs(c.scale[0]), std::fabs(c.scale[1]), std::fabs(c.scale[2])}) * bng_obj_scale_max(o.id);
        const int lod = bng_pick_lod(g, smax, std::sqrt(dx * dx + dy * dy + dz * dz), insts[k].cur, &insts[k]);
        const auto ch = bng_show_inst(insts[k], g, lod, cw);
        if (ch != static_cast<std::size_t>(-1)) child_chunk[k] = ch;
    }
    // One ScenePlace per CHILD, in child order (Root F): the old code compacted the list over visible
    // children only, so a child whose mesh was still loading shifted every following place and picking /
    // highlight jumped between children when the pool finished a request.
    const std::vector<std::size_t> places = bng_scene_places_of(o.id); // copy: scene_places grows below
    for (std::size_t k = 0; k < def->size(); ++k) {
        const std::size_t ch = child_chunk[k];
        if (k < places.size() && places[k] < scene_places.size()) {
            auto& sp = scene_places[places[k]];
            sp.chunk = ch;
            sp.alive = ch != static_cast<std::size_t>(-1);
            sp.tx = o.position.x;
            sp.ty = o.position.y;
            sp.tz = o.position.z;
            continue;
        }
        ScenePlace sp{};
        sp.object_id = o.id;
        sp.chunk = ch;
        sp.tile_gx = tile_gx;
        sp.tile_gy = tile_gy;
        sp.tx = o.position.x;
        sp.ty = o.position.y;
        sp.tz = o.position.z;
        sp.abs_z = true;
        sp.alive = ch != static_cast<std::size_t>(-1);
        sp.pinned = true;
        scene_places.push_back(std::move(sp));
    }
    bng_rt->spawned.insert(o.id);
};

// DecalRoads the game only uses as an AI driving line: material road_invisible, the engine placeholder
// DefaultDecalRoadMaterial (art/shapes/race/main.materials.json: translucent + opacityFactor 0 + alpha 0) or any
// material no .materials.json defines. They must not be drawn, but they stay listed / selectable / editable.
auto bng_road_no_draw = [](const bng::Material* m, const std::string& name) {
    std::string l = name;
    for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l.empty() || l == "none" || l == "null") return true;                          // no material at all
    if (l.find("invisible") != std::string::npos) return true;                           // road_invisible
    if (l.rfind("default", 0) == 0) return true;                                         // DefaultDecalRoadMaterial
    if (!m) return false; // maybe defined in the shared index (art/…): bng_upload_ribbon re-checks when it is ready
    // Fully transparent material: nothing to draw either way (opacityFactor / baseColorFactor alpha 0).
    return m->stage0.opacity_factor <= 0.001f || m->stage0.base_color[3] <= 0.001f;
};

// Uploads a finished road / river / water ribbon with its material and draw phase. Roads flagged no_draw (AI
// lanes: road_invisible / DefaultDecalRoadMaterial / a material no .materials.json defines) never get a chunk:
// they stay selectable and editable, only their node polyline / selection edge is drawn.
auto bng_upload_ribbon = [&](int ridx, bng::RibbonMesh& mesh) {
    auto& R = *bng_rt;
    const auto& lvl = *R.s->level;
    auto& rb = R.ribbons[static_cast<std::size_t>(ridx)];
    rb.building = false;
    rb.no_draw = false;
    gfx::dx12::ProcMeshSubrange sub;
    sub.index_start = 0;
    sub.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    if (rb.kind >= 1) {
        // water: translucent tint (water shader comes later)
        sub.alpha_mode = 2;
        sub.has_solid = true;
        const float wc[4]{0.10f, 0.24f, 0.30f, 0.72f};
        std::memcpy(sub.solid_rgba, wc, sizeof(wc));
    } else {
        const bng::Material* m = lvl.material_for(rb.material);
        if (!m) m = R.pool->materials().find(rb.material);
        // The shared material index builds on its own thread; only call a material "missing" once it is ready,
        // otherwise a first-frame upload of a shared road material would be dropped as if it did not exist.
        const bool material_missing = !m && R.pool->materials().ready();
        if (bng_road_no_draw(m, rb.material) || material_missing) {
            // Not drawn: no chunk. The ribbon stays in R.ribbons, so it is still listed in the Carreteras tab,
            // selectable and editable, and the selected road still shows its node polyline / selection edge.
            // Material transparent for the sake of the ground-cover test remains the AI lane.
            rb.no_draw = true;
            rb.chunk = static_cast<std::size_t>(-1);
            rb.visible = false;
            rb.failed = false;
            return;
        }
        if (m) {
            auto u8 = [](const std::string& t) { return t.empty() ? std::filesystem::path() : std::filesystem::u8path("vfs:" + t); };
            sub.texture_path = u8(m->stage0.color_map);
            sub.normal_path = u8(m->stage0.normal_map);
            sub.detail_path = u8(m->stage0.detail_map);
            sub.detail_normal_path = u8(m->stage0.detail_normal_map);
            sub.detail_scale = m->stage0.detail_scale[0];
            sub.detail_scale_y = m->stage0.detail_scale[1];
            // The opacityMap carries the cutout mask (tyre marks / dirt decals): it must be bound even when
            // there is a colorMap, otherwise the road is drawn opaque with a dark tint (Root A).
            if (!m->stage0.opacity_map.empty()) {
                sub.mask_path = u8(m->stage0.opacity_map);
                std::memcpy(sub.mask_tint, m->stage0.base_color, sizeof(sub.mask_tint));
                sub.mask_tint[3] = m->stage0.opacity_factor;
            }
            // base colour / opacityFactor / alphaRef / doubleSided of the material (used when the subrange has them)
            bng_sub_material(sub, m->stage0.base_color, m->stage0.opacity_factor, m->alpha_ref, m->double_sided,
                             sub.mask_path);
        }
        sub.alpha_mode = 2; // decal roads fade at the edges (texture alpha)
        if (sub.texture_path.empty() && sub.mask_path.empty()) {
            sub.has_solid = true;
            const float gc[4]{0.25f, 0.25f, 0.26f, 1.f};
            std::memcpy(sub.solid_rgba, gc, sizeof(gc));
            bng_sub_material(sub, gc, 1.f, 127, false, std::filesystem::path());
        }
    }
    if (!renderer.append_textured_mesh_merged(mesh.vertices.data(), static_cast<std::uint32_t>(mesh.vertices.size()),
                                              mesh.indices.data(), static_cast<std::uint32_t>(mesh.indices.size()), {sub})) {
        rb.failed = true;
        return;
    }
    rb.chunk = renderer.chunk_count() - 1;
    // decal roads: depth-biased over the ground, alpha blended, no Z write (like OMSI on-surface decals)
    if (rb.kind == 0) renderer.set_chunk_render_phase(rb.chunk, gfx::dx12::WorldRenderPhase::OnSurface, false, true);
    renderer.set_chunk_visible(rb.chunk, true);
    rb.visible = true;
};

// Uploads a finished decal cell (quads of one decal type) with the decal material, drawn on the surface.
auto bng_upload_decal = [&](int didx, bng::RibbonMesh& mesh) {
    auto& R = *bng_rt;
    const auto& lvl = *R.s->level;
    auto& dc = R.decals[static_cast<std::size_t>(didx)];
    dc.building = false;
    const auto& dt = lvl.decal_types[static_cast<std::size_t>(dc.type)];
    gfx::dx12::ProcMeshSubrange sub;
    sub.index_start = 0;
    sub.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    sub.alpha_mode = 2; // decals blend over the ground / road
    const bng::Material* m = dt.material.empty() ? nullptr : lvl.material_for(dt.material);
    if (!m && !dt.material.empty()) m = R.pool->materials().find(dt.material);
    if (m) {
        auto u8 = [](const std::string& t) { return t.empty() ? std::filesystem::path() : std::filesystem::u8path("vfs:" + t); };
        sub.texture_path = u8(m->stage0.color_map);
        sub.normal_path = u8(m->stage0.normal_map);
        // opacityMap mask also with a colorMap (the mask lives in its alpha channel)
        if (!m->stage0.opacity_map.empty()) {
            sub.mask_path = u8(m->stage0.opacity_map);
            std::memcpy(sub.mask_tint, m->stage0.base_color, sizeof(sub.mask_tint));
            sub.mask_tint[3] = m->stage0.opacity_factor;
        }
        bng_sub_material(sub, m->stage0.base_color, m->stage0.opacity_factor, m->alpha_ref, m->double_sided,
                         sub.mask_path);
    }
    if (sub.texture_path.empty() && sub.mask_path.empty()) {
        sub.has_solid = true;
        const float gc[4]{0.78f, 0.78f, 0.78f, 0.85f};
        std::memcpy(sub.solid_rgba, gc, sizeof(gc));
        bng_sub_material(sub, gc, 1.f, 127, false, std::filesystem::path());
    }
    if (!renderer.append_textured_mesh_merged(mesh.vertices.data(), static_cast<std::uint32_t>(mesh.vertices.size()),
                                              mesh.indices.data(), static_cast<std::uint32_t>(mesh.indices.size()), {sub})) {
        dc.failed = true;
        return;
    }
    dc.chunk = renderer.chunk_count() - 1;
    renderer.set_chunk_render_phase(dc.chunk, gfx::dx12::WorldRenderPhase::OnSurface, false, true);
    renderer.set_chunk_visible(dc.chunk, true);
    dc.visible = true;
};

auto bng_hide_object = [&](std::int32_t id) {
    if (auto it = bng_rt->chunk_of.find(id); it != bng_rt->chunk_of.end()) bng_hide_inst(it->second);
    if (auto it = bng_rt->prefab_inst.find(id); it != bng_rt->prefab_inst.end())
        for (auto& in : it->second) bng_hide_inst(in);
    for (const std::size_t i : bng_scene_places_of(id))
        if (i < scene_places.size()) scene_places[i].alive = false;
    bng_rt->spawned.erase(id);
};

// Forest CPU pick grid: declared at loop scope (not inside the beamng block below) because the vegetation
// tools (mod_beamng_vegetation.inc.cpp, included after the block) bump the version to force a rebuild.
constexpr float kForestCell = 32.f; // metres, side of one hash cell
std::uint64_t s_forest_grid_ver = 0; // bumped by the vegetation tools (add / delete / move) to rebuild the hash

if (cfg.beamng && cfg.beamng->level && cfg.beamng->vfs) {
    bng_rt = std::make_unique<BngRuntime>();
    bng_rt->s = cfg.beamng;
    bng_rt->radius = (std::max)(100.f, cfg.beamng->load_radius);
    bng::set_active_vfs(cfg.beamng->vfs);
    gfx::dx12::set_virtual_file_provider(
        +[](const std::string& p) {
            const auto* v = bng::active_vfs();
            return v && v->exists(p);
        },
        +[](const std::string& p, std::vector<std::uint8_t>& out) {
            const auto* v = bng::active_vfs();
            if (!v) return false;
            auto b = v->read(p);
            if (!b) return false;
            out = std::move(*b);
            return true;
        });
    bng_rt->pool = std::make_unique<bng::AssetPool>(*cfg.beamng->vfs, *cfg.beamng->level);
    auto& lvl = *cfg.beamng->level;

    // TSStatic -> edit_objects (ids 1..N in scene order; GPU spawn happens by distance in the frame pump)
    std::int32_t next_id = 1;
    for (const auto& o : edit_objects) next_id = (std::max)(next_id, o.id + 1);
    std::size_t n_static = 0;
    for (std::uint32_t i = 0; i < lvl.objects.size(); ++i) {
        const auto& so = lvl.objects[i];
        if (so.cls != "TSStatic" || so.shape.empty()) continue;
        map::MapObjectInst o{};
        o.id = next_id++;
        o.sco_path = std::filesystem::path(L"vfs:" + std::filesystem::u8path(so.shape).wstring());
        o.position.x = so.pos[0];
        o.position.y = so.pos[1];
        o.position.z = so.pos[2];
        bng::mat3_to_editor_euler(so.rot, o.rotation_deg.x, o.rotation_deg.y, o.rotation_deg.z);
        BngObjExtra ex;
        std::memcpy(ex.scale, so.scale, sizeof(ex.scale));
        ex.level_index = i;
        ex.orig_yaw = o.rotation_deg.x;
        ex.orig_pitch = o.rotation_deg.y;
        ex.orig_bank = o.rotation_deg.z;
        bng_rt->extra[o.id] = ex;
        edit_objects.push_back(std::move(o));
        ++n_static;
    }

    // Prefabs -> one editor object each (children follow its pose)
    std::size_t n_prefab = 0;
    for (std::uint32_t i = 0; i < lvl.objects.size(); ++i) {
        const auto& so = lvl.objects[i];
        if (so.cls != "Prefab") continue;
        const std::string file = so.raw.get("filename").as_string();
        if (file.empty()) continue;
        map::MapObjectInst o{};
        o.id = next_id++;
        o.sco_path = std::filesystem::path(L"vfs:" + std::filesystem::u8path(bng::Vfs::normalize(file)).wstring());
        o.position.x = so.pos[0];
        o.position.y = so.pos[1];
        o.position.z = so.pos[2];
        bng::mat3_to_editor_euler(so.rot, o.rotation_deg.x, o.rotation_deg.y, o.rotation_deg.z);
        BngObjExtra ex;
        std::memcpy(ex.scale, so.scale, sizeof(ex.scale));
        ex.level_index = i;
        ex.orig_yaw = o.rotation_deg.x;
        ex.orig_pitch = o.rotation_deg.y;
        ex.orig_bank = o.rotation_deg.z;
        bng_rt->extra[o.id] = ex;
        edit_objects.push_back(std::move(o));
        ++n_prefab;
    }
    log::info("beamng editor: " + std::to_string(n_prefab) + " prefabs");

    // Roads / rivers / water: bounds now, geometry on demand (radius streaming)
    for (std::uint32_t i = 0; i < lvl.objects.size(); ++i) {
        const auto& so = lvl.objects[i];
        BngRibbon rb;
        rb.level_index = i;
        if (so.cls == "DecalRoad" || so.cls == "River") {
            rb.kind = so.cls == "River" ? 1 : 0;
            rb.material = so.raw.get("material").as_string();
            // AI-only lanes (road_invisible / DefaultDecalRoadMaterial / a material no .materials.json defines)
            // are NOT skipped here: they must stay selectable and editable in the Carreteras tab. When the
            // material is already known to be a no-draw one the geometry is not even built (no_draw); the
            // authoritative check is in bng_upload_ribbon, which also covers the shared material index.
            if (rb.kind == 0) rb.no_draw = bng_road_no_draw(lvl.material_for(rb.material), rb.material);
            const auto nodes = bng::read_road_nodes(so.raw.get("nodes"));
            if (nodes.size() < 2) continue;
            // polyline kept for the ground-cover road test (Root E: no grass on the road)
            rb.node_xy.reserve(nodes.size() * 3);
            for (const auto& n : nodes) {
                rb.node_xy.push_back(n.x);
                rb.node_xy.push_back(n.y);
                rb.node_xy.push_back(0.5f * n.w);
            }
            float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
            for (const auto& n : nodes) {
                mnx = (std::min)(mnx, n.x - n.w);
                mxx = (std::max)(mxx, n.x + n.w);
                mny = (std::min)(mny, n.y - n.w);
                mxy = (std::max)(mxy, n.y + n.w);
            }
            rb.center[0] = 0.5f * (mnx + mxx);
            rb.center[1] = 0.5f * (mny + mxy);
            rb.radius = 0.5f * std::hypot(mxx - mnx, mxy - mny);
            rb.over_objects = so.raw.get("overObjects").as_bool();
            // distanceFade [start, range]: past start+range the road is hidden, like in the game
            const auto& fade = so.raw.get("distanceFade");
            if (fade.is_array() && fade.size() >= 2) {
                rb.fade_start = fade[0].as_float();
                rb.fade_range = fade[1].as_float();
            }
            rb.render_priority = static_cast<float>(so.raw.get("renderPriority").as_number_lenient(10.0));
            if (lvl.terrain)
                for (const auto& n : nodes)
                    if (lvl.terrain->contains(n.x, n.y))
                        rb.max_above_terrain = (std::max)(rb.max_above_terrain, n.z - lvl.terrain->sample_world(n.x, n.y));
        } else if (so.cls == "WaterBlock") {
            rb.kind = 2;
            rb.center[0] = so.pos[0];
            rb.center[1] = so.pos[1];
            rb.radius = 0.5f * std::hypot(so.scale[0], so.scale[1]);
        } else if (so.cls == "WaterPlane") {
            rb.kind = 3;
            rb.center[0] = so.pos[0];
            rb.center[1] = so.pos[1];
            rb.radius = 1e9f; // infinite
        } else {
            continue;
        }
        bng_rt->ribbons.push_back(std::move(rb));
    }
    log::info("beamng editor: " + std::to_string(bng_rt->ribbons.size()) + " roads/rivers/water");
    // Ribbon builds are launched in renderPriority order so the alpha blending of a batch follows the
    // game's decal ordering (lower priority underneath, higher on top).
    bng_rt->ribbon_order.resize(bng_rt->ribbons.size());
    for (std::size_t i = 0; i < bng_rt->ribbon_order.size(); ++i) bng_rt->ribbon_order[i] = static_cast<int>(i);
    std::stable_sort(bng_rt->ribbon_order.begin(), bng_rt->ribbon_order.end(), [&](int a, int b) {
        return bng_rt->ribbons[static_cast<std::size_t>(a)].render_priority <
               bng_rt->ribbons[static_cast<std::size_t>(b)].render_priority;
    });

    // Decals (main.decals.json): instances grouped by type and 64 m cell, quads built on demand like the roads.
    if (!lvl.decals.empty()) {
        constexpr float kDecalCell = 64.f;
        auto cell_of = [&](const bng::DecalInstance& d) {
            return std::array<int, 3>{d.type, static_cast<int>(std::floor(d.pos[0] / kDecalCell)),
                                      static_cast<int>(std::floor(d.pos[1] / kDecalCell))};
        };
        std::vector<std::uint32_t> ids(lvl.decals.size());
        for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = static_cast<std::uint32_t>(i);
        std::sort(ids.begin(), ids.end(), [&](std::uint32_t a, std::uint32_t b) {
            const auto ka = cell_of(lvl.decals[a]), kb = cell_of(lvl.decals[b]);
            if (ka[0] != kb[0]) return ka[0] < kb[0];
            if (ka[1] != kb[1]) return ka[1] < kb[1];
            if (ka[2] != kb[2]) return ka[2] < kb[2];
            return a < b;
        });
        bng_rt->decal_ids = ids;
        for (std::size_t i = 0; i < ids.size();) {
            const auto key = cell_of(lvl.decals[ids[i]]);
            std::size_t j = i;
            float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f, prio = 0.f;
            for (; j < ids.size(); ++j) {
                const auto& d = lvl.decals[ids[j]];
                const auto kj = cell_of(d);
                if (kj != key) break;
                const float half = 0.5f * (d.size > 0.f ? d.size : 1.f);
                mnx = (std::min)(mnx, d.pos[0] - half);
                mny = (std::min)(mny, d.pos[1] - half);
                mxx = (std::max)(mxx, d.pos[0] + half);
                mxy = (std::max)(mxy, d.pos[1] + half);
                prio = (std::max)(prio, d.render_priority);
            }
            BngDecalCell dc;
            dc.type = key[0];
            dc.gx = key[1];
            dc.gy = key[2];
            dc.first = static_cast<std::uint32_t>(i);
            dc.count = static_cast<std::uint32_t>(j - i);
            dc.center[0] = 0.5f * (mnx + mxx);
            dc.center[1] = 0.5f * (mny + mxy);
            dc.radius = 0.5f * std::hypot(mxx - mnx, mxy - mny);
            dc.render_priority = prio;
            // BeamNG decal fade: fadeStartPixelSize/fadeEndPixelSize are screen sizes (*LOD value*), larger
            // start = closer. Converted to metres with a nominal 1300 px focal (1080p @ 90 deg vfov) and the
            // type's default size; negative/missing values keep the engine defaults (50 m -> 75 m).
            dc.fade_start = 50.f;
            dc.fade_end = 75.f;
            if (key[0] >= 0 && static_cast<std::size_t>(key[0]) < lvl.decal_types.size()) {
                const auto& dt = lvl.decal_types[static_cast<std::size_t>(key[0])];
                const float ps = static_cast<float>(dt.raw.get("fadeStartPixelSize").as_number_lenient(0.0));
                const float pe = static_cast<float>(dt.raw.get("fadeEndPixelSize").as_number_lenient(0.0));
                if (ps > 0.f && pe > 0.f && pe < ps) {
                    constexpr float kFocalPx = 1300.f;
                    const float sz = dt.size > 0.f ? dt.size : 1.f;
                    dc.fade_start = (std::clamp)(sz * kFocalPx / ps, 5.f, 500.f);
                    dc.fade_end = (std::clamp)(sz * kFocalPx / pe, dc.fade_start + 1.f, 1000.f);
                }
            }
            bng_rt->decals.push_back(dc);
            i = j;
        }
        bng_rt->decal_order.resize(bng_rt->decals.size());
        for (std::size_t i = 0; i < bng_rt->decal_order.size(); ++i) bng_rt->decal_order[i] = static_cast<int>(i);
        std::stable_sort(bng_rt->decal_order.begin(), bng_rt->decal_order.end(), [&](int a, int b) {
            return bng_rt->decals[static_cast<std::size_t>(a)].render_priority <
                   bng_rt->decals[static_cast<std::size_t>(b)].render_priority;
        });
    }
    log::info("beamng editor: " + std::to_string(lvl.decals.size()) + " decals in " + std::to_string(bng_rt->decals.size()) +
              " cells / " + std::to_string(lvl.decal_types.size()) + " types");

    for (const auto& so : lvl.objects) {
        if (so.cls != "GroundCover" || so.raw.get("hidden").as_bool()) continue;
        BngGroundCover gc;
        const std::string mat = so.raw.get("material").as_string();
        const bng::Material* m = lvl.material_for(mat);
        if (!m) m = bng_rt->pool->materials().find(mat);
        if (m && !m->stage0.color_map.empty()) gc.texture = "vfs:" + m->stage0.color_map;
        gc.radius = static_cast<float>(so.raw.get("dissolveRadius").as_number_lenient(150.0));
        const float max_el = static_cast<float>(so.raw.get("maxElements").as_number_lenient(1000.0));
        gc.density = max_el / (3.14159265f * gc.radius * gc.radius);
        // zOffset never lifts the grass: the height now comes from the real surface grid (Root E)
        gc.z_offset = (std::min)(0.f, static_cast<float>(so.raw.get("zOffset").as_number_lenient(0.0)));
        gc.seed = static_cast<std::uint32_t>(so.raw.get("seed").as_number_lenient(1.0));
        for (const auto& t : so.raw.get("Types").items()) {
            const auto& uvs = t.get("billboardUVs");
            if (!uvs.is_array() || uvs.size() < 4) continue; // shape types: not yet
            BngGroundCoverType ty;
            for (std::size_t k = 0; k < 4; ++k) ty.uv[k] = uvs[k].as_float();
            const std::string layer = t.get("layer").as_string();
            if (!layer.empty() && lvl.terrain) {
                for (std::size_t k = 0; k < lvl.terrain->material_names.size(); ++k)
                    if (lvl.terrain->material_names[k] == layer) ty.layer = static_cast<int>(k);
                if (ty.layer < 0) continue; // layer not on this terrain
            }
            ty.probability = static_cast<float>(t.get("probability").as_number_lenient(1.0));
            ty.size_min = static_cast<float>(t.get("sizeMin").as_number_lenient(1.0));
            ty.size_max = static_cast<float>(t.get("sizeMax").as_number_lenient(ty.size_min));
            ty.size_exp = static_cast<float>(t.get("sizeExponent").as_number_lenient(1.0));
            ty.max_slope_deg = static_cast<float>(t.get("maxSlope").as_number_lenient(90.0));
            gc.types.push_back(ty);
        }
        if (!gc.types.empty() && !gc.texture.empty()) bng_rt->ground_covers.push_back(std::move(gc));
    }
    log::info("beamng editor: " + std::to_string(bng_rt->ground_covers.size()) + " ground covers");

    // Terrain patches
    if (lvl.terrain && lvl.terrain->size > 0) {
        const auto& t = *lvl.terrain;
        bng_rt->patches_x = static_cast<int>((t.size + static_cast<std::uint32_t>(bng_rt->patch_samples) - 1) /
                                             static_cast<std::uint32_t>(bng_rt->patch_samples));
        bng_rt->patches_y = bng_rt->patches_x;
        for (int py = 0; py < bng_rt->patches_y; ++py)
            for (int px = 0; px < bng_rt->patches_x; ++px) bng_rt->patches.push_back({px, py, 0, 0, static_cast<std::size_t>(-1), false});
        for (const auto& m : lvl.terrain_materials)
            if (!m.base_color.empty()) {
                bng_rt->terrain_texture = "vfs:" + m.base_color;
                break;
            }
        const float ext = static_cast<float>(lvl.terrain->size) * lvl.terrain->square_size;
        for (const auto& name : lvl.terrain->material_names) {
            BngRuntime::TerrainLayerTex lt;
            for (const auto& m : lvl.terrain_materials)
                if (m.internal_name == name) {
                    if (!m.detail_color.empty()) lt.detail = "vfs:" + m.detail_color;
                    if (!m.detail_normal.empty()) lt.detail_normal = "vfs:" + m.detail_normal;
                    lt.tiles = ext / (std::max)(0.25f, m.detail_size);
                    break;
                }
            bng_rt->terrain_layers.push_back(std::move(lt));
        }
    }
    log::info("beamng editor: " + std::to_string(n_static) + " TSStatic, " + std::to_string(lvl.forest.size()) + " forest, " +
              std::to_string(bng_rt->patches.size()) + " terrain patches, radius=" + std::to_string(bng_rt->radius) +
              " terrain_tex=" + bng_rt->terrain_texture);
    // Start looking at the spawn from a useful distance (BeamNG spawns are vehicle-scale).
    camera.target_x = cfg.beamng->spawn_pos[0];
    camera.target_y = cfg.beamng->spawn_pos[1];
    camera.target_z = cfg.beamng->spawn_pos[2];
    camera.radius = 140.f;
    camera.max_radius = 6000.f; // BeamNG maps are 8-16 km wide: let the editor zoom out to the horizon
    camera.theta = -cfg.beamng->spawn_heading - 1.5707963f;
    camera.phi = 0.55f;
    // Free flight by default in BeamNG (F11 toggles it off/on); no ground re-anchor while flying.
    camera.fly_mode = true;
    camera.ground_follow = false;
    camera.sync_fly_from_orbit();
    panel.set_status(
        std::string("BeamNG.drive: ") + cfg.beamng->info.name + " - " + std::to_string(n_static) +
        " objetos | Vuelo: RMB mirar, WASD mover, Q/E bajar/subir, Shift x4, Alt+LMB orbitar, F11 salir, F8 seguir superficie");
    // The dock / "lista de instancias" / catalogo were built in setup_03, i.e. BEFORE this module appended the
    // TSStatic + Prefab objects to edit_objects. Without this the lists stayed empty in BeamNG mode (they only
    // refreshed on edit events). `vfs:` paths count as present (see path_missing in setup_03).
    refresh_editor_lists();


    // ── quality (BeamNG preset tables: core/settings/*Quality.lua + settingsPresets.json) ────────────────────
    {
        auto pr = bng::load_prefs();
        bng_rt->q_mesh = (std::clamp)(pr.q_mesh, 0, 4);
        bng_rt->q_texture = (std::clamp)(pr.q_texture, 0, 3);
        bng_rt->q_shadows = (std::clamp)(pr.q_shadows, 0, 3);
    }
    // from_ui: the user changed the quality tab. At level load (from_ui=false) the saved Vista "Sombras
    // solares (CSM)" choice is kept; it used to be overwritten here on every BeamNG load.
    auto bng_apply_quality = [&](bool from_ui) {
        static const float kDetailAdjust[5] = {0.5f, 0.75f, 1.f, 1.5f, 2.f};
        static const float kLodScale[5] = {2.f, 1.5f, 1.f, 0.75f, 0.75f};
        static const std::uint32_t kMipSkip[4] = {2, 1, 0, 0};
        static const std::uint32_t kShadowSize[4] = {0, 1024, 2048, 4096}; // texSize 1024 x textureScalar
        bng_rt->detail_adjust = kDetailAdjust[bng_rt->q_mesh];
        static const float kDensity[5] = {0.f, 0.5f, 0.75f, 1.f, 1.f};
        bng_rt->gc_density_scale = kDensity[bng_rt->q_mesh];
        bng_rt->lod_scale = kLodScale[bng_rt->q_mesh];
        bng_rt->lod_cx = 1e30f; // re-evaluate terrain LOD
        if (gfx::dx12::texture_mip_skip() != kMipSkip[bng_rt->q_texture]) {
            gfx::dx12::set_texture_mip_skip(kMipSkip[bng_rt->q_texture]);
            // re-upload the textures already on the GPU with the new mip level (same SRV slots)
            if (bng_rt->uploaded_shapes > 0) renderer.reload_file_textures();
        }
        auto& st = panel.imgui_state();
        if (from_ui || bng_rt->q_shadows == 0) st.show_sun_shadows = bng_rt->q_shadows > 0;
        if (kShadowSize[bng_rt->q_shadows] > 0) st.shadow_map_size = kShadowSize[bng_rt->q_shadows];
        auto pr = bng::load_prefs();
        pr.q_mesh = bng_rt->q_mesh;
        pr.q_texture = bng_rt->q_texture;
        pr.q_shadows = bng_rt->q_shadows;
        pr.load_radius = bng_rt->radius;
        bng::save_prefs(pr);
    };
    bng_apply_quality(false);

    // ── time of day / sky from the level (TimeOfDay.time: 0 = noon, 0.5 = midnight) ────────────────────────
    for (const auto& o : lvl.objects) {
        if (o.cls == "TimeOfDay") {
            // Level time is only the default: an hour the user saved (Vista > Hora) wins.
            const double t = o.raw.get("time").as_number_lenient(0.0);
            if (!editor_ui_settings.sim_hour_from_file)
                panel.imgui_state().sim_hour = static_cast<float>(std::fmod(12.0 + t * 24.0 + 48.0, 24.0));
        } else if (o.cls == "ScatterSky") {
            const float sd = static_cast<float>(o.raw.get("shadowDistance").as_number_lenient(400.0));
            bng_shadow_distance = (std::clamp)(sd, 100.f, 1600.f);
        } else if (o.cls == "LevelInfo") {
            // visibleDistance: the render-distance cull in BeamNG mode never exceeds it (6000 when missing)
            const float vd = static_cast<float>(o.raw.get("visibleDistance").as_number_lenient(6000.0));
            if (vd > 0.f) bng_visible_distance = vd;
        }
    }

    // Reverse-Z far plane: keep the 10 km default, but never clip the level's own render distance.
    // BeamNG mode draws up to LevelInfo.visibleDistance (the run loop passes it as cull_max_distance,
    // there is no per-object distance cull), so the far plane is kept strictly beyond it.
    input::set_luna_far_plane((std::max)(input::luna_far_plane(), bng_visible_distance * 1.2f));

    // reverse lookup: level object -> editor object id (scene tree selection)
    for (const auto& [eid, ex] : bng_rt->extra)
        if (ex.level_index != UINT32_MAX) bng_rt->eid_of_level[ex.level_index] = eid;

    bng_ui_status = [&]() -> std::string {
        if (!bng_rt) return {};
        return "Visibles: " + std::to_string(bng_rt->spawned.size()) + "  Forest: " + std::to_string(bng_rt->forest_spawned.size()) +
               "  Modelos: " + std::to_string(bng_rt->uploaded_shapes) +
               (bng_rt->pool->pending() ? "  (cargando " + std::to_string(bng_rt->pool->pending()) + ")" : std::string());
    };

    bng_ui_quality_tab = [&, bng_apply_quality]() {
        if (!bng_rt) return;
        static const char* kMesh[] = {"Lowest", "Low", "Normal", "High", "Ultra"};
        static const char* kTex[] = {"Lowest", "Low", "Normal", "High"};
        static const char* kShadow[] = {"Lowest (sin sombras)", "Low", "Normal", "High"};
        static const char* kPresets[] = {"Lowest", "Low", "Normal", "High", "Ultra"};
        static const int kPresetMesh[5] = {0, 1, 2, 3, 4};
        static const int kPresetTex[5] = {0, 1, 2, 3, 3};
        static const int kPresetShadow[5] = {0, 1, 2, 3, 3};
        auto& R = *bng_rt;
        int preset = -1;
        for (int i = 0; i < 5; ++i)
            if (R.q_mesh == kPresetMesh[i] && R.q_texture == kPresetTex[i] && R.q_shadows == kPresetShadow[i]) preset = i;
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.f), "Calidad grafica (presets de BeamNG.drive)");
        ImGui::Separator();
        bool changed = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##bng_preset", preset >= 0 ? kPresets[preset] : "Custom")) {
            for (int i = 0; i < 5; ++i)
                if (ImGui::Selectable(kPresets[i], preset == i)) {
                    R.q_mesh = kPresetMesh[i];
                    R.q_texture = kPresetTex[i];
                    R.q_shadows = kPresetShadow[i];
                    changed = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mismos presets que Opciones > Graficos del juego. Tocar un ajuste pasa a Custom.");
        changed |= ImGui::Combo("Mallas / LOD", &R.q_mesh, kMesh, 5);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("$pref::TS::detailAdjust 0.5 / 0.75 / 1 / 1.5 / 2 (cuando cambia de LOD cada modelo)\n"
                              "$pref::Terrain::lodScale 2 / 1.5 / 1 / 0.75 / 0.75 (detalle del terreno)");
        changed |= ImGui::Combo("Texturas", &R.q_texture, kTex, 4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("$pref::Video::textureReductionLevel 2 / 1 / 0 / 0: mips que se saltean (menos VRAM).\n"
                              "Se aplica a las texturas que se cargan de ahora en adelante.");
        changed |= ImGui::Combo("Sombras", &R.q_shadows, kShadow, 4);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("$pref::Shadows::textureScalar 0.5 / 1 / 2 sobre texSize 1024 del ScatterSky.");
        ImGui::Separator();
        float rad = R.radius;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##bng_rad", &rad, 200.f, 8000.f, "radio de carga %.0f m")) {
            R.radius = rad;
            R.s->load_radius = rad;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Modelos, forest y carreteras dentro de este radio alrededor de la camara.");
        if (ImGui::Button("Importar mis ajustes del juego", ImVec2(-FLT_MIN, 0.f))) {
            // <userfolder>/settings/settings.json: GraphicMeshQuality / GraphicTextureQuality / GraphicShadowsQuality
            const auto path = R.s->user_folder / "settings" / "settings.json";
            std::ifstream f(path, std::ios::binary);
            std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            bng::JsonValue j;
            if (!txt.empty() && bng::json_parse(txt, j).ok) {
                auto lvl_of = [](const std::string& v, int def) {
                    if (v == "Lowest") return 0;
                    if (v == "Low") return 1;
                    if (v == "Normal") return 2;
                    if (v == "High") return 3;
                    if (v == "Ultra") return 4;
                    return def;
                };
                R.q_mesh = (std::min)(4, lvl_of(j.get("GraphicMeshQuality").as_string(), 2));
                R.q_texture = (std::min)(3, lvl_of(j.get("GraphicTextureQuality").as_string(), 2));
                R.q_shadows = (std::min)(3, lvl_of(j.get("GraphicShadowsQuality").as_string(), 2));
                changed = true;
                panel.set_status("Calidad importada de " + path.string());
            } else {
                panel.set_status("No se pudo leer " + path.string());
            }
        }
        if (changed) bng_apply_quality(true);
        ImGui::Spacing();
        ImGui::TextDisabled("detailAdjust %.2f  lodScale %.2f  mips -%u", R.detail_adjust, R.lod_scale, gfx::dx12::texture_mip_skip());
    };

    bng_ui_scene_tab = [&]() {
        if (!bng_rt) return;
        auto& R = *bng_rt;
        auto& L = *bng_rt->s->level;
        if (bng_ui_object_actions) bng_ui_object_actions();
        // Camera shortcuts: spawn and "fit the loaded objects" (same maths as the road tab: 1.6 x radius).
        {
            const float half = ImGui::GetContentRegionAvail().x * 0.5f - 4.f;
            if (ImGui::Button("Ir al spawn", ImVec2(half, 0.f))) {
                camera.target_x = cfg.beamng->spawn_pos[0];
                camera.target_y = cfg.beamng->spawn_pos[1];
                camera.target_z = cfg.beamng->spawn_pos[2];
                camera.ground_z = cfg.beamng->spawn_pos[2];
                camera.radius = 140.f;
                camera.phi = 0.55f;
                camera.theta = -cfg.beamng->spawn_heading - 1.5707963f;
                panel.set_status("BeamNG: camara al spawn");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pone la camara en el SpawnSphere del nivel (radio 140 m).");
            ImGui::SameLine(0.f, 8.f);
            if (ImGui::Button("Centrar en objetos cargados", ImVec2(-FLT_MIN, 0.f))) {
                double sx = 0.0, sy = 0.0, sz = 0.0;
                std::size_t n = 0;
                float max_r = 1.f;
                for (const auto& o : edit_objects) {
                    if (!bng_is_obj(o) || !bng_rt->spawned.count(o.id)) continue;
                    sx += o.position.x;
                    sy += o.position.y;
                    sz += o.position.z;
                    max_r = (std::max)(max_r, bng_obj_radius(o));
                    ++n;
                }
                if (n == 0) {
                    panel.set_status("BeamNG: no hay objetos cargados en el radio de la camara");
                } else {
                    camera.target_x = static_cast<float>(sx / static_cast<double>(n));
                    camera.target_y = static_cast<float>(sy / static_cast<double>(n));
                    camera.target_z = static_cast<float>(sz / static_cast<double>(n));
                    camera.ground_z = camera.target_z;
                    camera.radius = (std::clamp)(max_r * 1.6f, 30.f, 600.f);
                    panel.set_status("BeamNG: camara centrada en " + std::to_string(n) + " objetos cargados");
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Media de las posiciones de los objetos visibles, radio 1.6 x el mayor radio (30..600 m).");
            bool gf = camera.ground_follow;
            if (ImGui::Checkbox("Seguir superficie", &gf)) {
                camera.ground_follow = gf;
                panel.set_status(gf ? "BeamNG: la camara sigue la superficie (F8)" : "BeamNG: la camara no sigue la superficie (F8)");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("La camara se apoya en el primer mesh u objeto bajo ella (no en el heightfield).\nF8 lo alterna en modo BeamNG.");
            ImGui::Separator();
        }
        // ── Forest instance selected in the viewport (Root D): pose / scale / delete on Level::forest ────
        if (R.sel_forest >= 0 && static_cast<std::size_t>(R.sel_forest) < L.forest.size()) {
            auto& fi = L.forest[static_cast<std::size_t>(R.sel_forest)];
            const bng::ForestItemType* ty =
                (fi.type >= 0 && static_cast<std::size_t>(fi.type) < L.forest_types.size())
                    ? &L.forest_types[static_cast<std::size_t>(fi.type)]
                    : nullptr;
            auto mark_forest_dirty = [&]() {
                // the loader keeps no per-instance file name: mark the forest folder as needing a rewrite
                const std::string f = L.info.dir + "/forest/";
                if (std::find(L.forest_files_dirty.begin(), L.forest_files_dirty.end(), f) == L.forest_files_dirty.end())
                    L.forest_files_dirty.push_back(f);
            };
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.f), "Forest #%d  %s", R.sel_forest,
                               ty ? ty->name.c_str() : "");
            bool touched = false;
            ImGui::SetNextItemWidth(-90.f);
            if (ImGui::DragFloat3("pos", fi.pos, 0.25f, 0.f, 0.f, "%.2f")) touched = true;
            float eul[3]{0.f, 0.f, 0.f};
            bng::mat3_to_editor_euler(fi.rot, eul[0], eul[1], eul[2]);
            ImGui::SetNextItemWidth(-90.f);
            if (ImGui::DragFloat3("rot", eul, 0.5f, -360.f, 360.f, "%.1f")) {
                fi.rot = bng::editor_euler_to_mat3(eul[0], eul[1], eul[2]);
                touched = true;
            }
            ImGui::SetNextItemWidth(-90.f);
            if (ImGui::DragFloat("escala", &fi.scale, 0.01f, 0.05f, 20.f, "%.3f")) touched = true;
            if (touched) {
                if (auto it = R.forest_chunk.find(static_cast<std::uint32_t>(R.sel_forest)); it != R.forest_chunk.end() && ty) {
                    if (auto sg = R.shapes.find(ty->shape_file); sg != R.shapes.end() && sg->second.ready()) {
                        float m16[16];
                        const float sc3[3]{fi.scale, fi.scale, fi.scale};
                        bng::mat3_to_world4x4(fi.rot, sc3, fi.pos, m16);
                        bng_show_inst(it->second, sg->second, it->second.cur < 0 ? 0 : it->second.cur, m16);
                    }
                }
                mark_forest_dirty();
            }
            if (ImGui::Button("Borrar instancia", ImVec2(-FLT_MIN, 0.f))) {
                fi.deleted = true;
                if (auto it = R.forest_chunk.find(static_cast<std::uint32_t>(R.sel_forest)); it != R.forest_chunk.end()) {
                    bng_hide_inst(it->second);
                    R.forest_chunk.erase(it);
                }
                R.forest_spawned.erase(static_cast<std::uint32_t>(R.sel_forest));
                mark_forest_dirty();
                if (bng_select_forest_hook) bng_select_forest_hook(-1);
                panel.set_status("Forest: instancia borrada (se guarda con Ctrl+S)");
            }
            ImGui::Separator();
        }
        // ── GroundCover selected in the viewport: aggregated instance (cover type + 32 m cell) ──────────────
        if (R.sel_gc >= 0 && static_cast<std::size_t>(R.sel_gc) < R.ground_covers.size()) {
            const auto& gc = R.ground_covers[static_cast<std::size_t>(R.sel_gc)];
            ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.35f, 1.f), "GroundCover #%d", R.sel_gc);
            if (!gc.texture.empty())
                ImGui::TextDisabled("%s", std::filesystem::path(gc.texture).filename().string().c_str());
            const int gx = static_cast<int>(
                static_cast<std::int32_t>(static_cast<std::uint32_t>((R.sel_gc_key >> 24) & 0xFFFFFF) << 8) >> 8);
            const int gy = static_cast<int>(
                static_cast<std::int32_t>(static_cast<std::uint32_t>(R.sel_gc_key & 0xFFFFFF) << 8) >> 8);
            std::size_t cells = 0;
            for (const auto& [k, c] : R.gc_cells)
                if ((k >> 48) == static_cast<std::uint64_t>(R.sel_gc) && c.visible &&
                    c.chunk != static_cast<std::size_t>(-1))
                    ++cells;
            ImGui::Text("celda (%d,%d)  %s celdas visibles", gx, gy, std::to_string(cells).c_str());
            ImGui::Text("densidad %.2f/m2  radio %.0f m  %s tipos", static_cast<double>(gc.density),
                        static_cast<double>(gc.radius), std::to_string(gc.types.size()).c_str());
            if (ImGui::Button("Deseleccionar GroundCover", ImVec2(-FLT_MIN, 0.f)) && bng_select_groundcover_hook)
                bng_select_groundcover_hook(-1, 0);
            ImGui::Separator();
        }
        static char filter[128]{};
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##bng_scene_filter", "Buscar por nombre, clase o modelo...", filter, sizeof(filter));
        // [PERF] Scene tab cache: the lowercase filter, the flat hit list and the per-group item vectors are
        // rebuilt only when the filter text or the level objects change (cheap per-frame signature counter),
        // instead of building a lowercase haystack for every object / every group on every frame.
        static std::string s_scene_filter_raw, s_scene_filter_low;
        static std::vector<std::uint32_t> s_scene_hits, s_scene_no_items;
        static std::unordered_map<int, std::vector<std::uint32_t>> s_scene_group_items;
        static bool s_scene_hits_valid = false, s_scene_groups_valid = false;
        static std::uint64_t s_scene_ver = ~std::uint64_t(0);
        std::size_t scene_deleted = 0;
        for (const auto& so : L.objects)
            if (so.deleted) ++scene_deleted;
        std::uint64_t scene_ver = 1469598103934665603ull;
        scene_ver = (scene_ver ^ static_cast<std::uint64_t>(L.objects.size())) * 1099511628211ull;
        scene_ver = (scene_ver ^ static_cast<std::uint64_t>(L.groups.size())) * 1099511628211ull;
        scene_ver = (scene_ver ^ static_cast<std::uint64_t>(scene_deleted)) * 1099511628211ull;
        if (scene_ver != s_scene_ver) { // objects added / removed / deleted since the cache was built
            s_scene_ver = scene_ver;
            s_scene_hits_valid = false;
            s_scene_groups_valid = false;
        }
        if (s_scene_filter_raw != filter) { // the lowercase filter is computed once per edit, not per frame
            s_scene_filter_raw = filter;
            s_scene_filter_low = s_scene_filter_raw;
            for (auto& c : s_scene_filter_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s_scene_hits_valid = false;
        }
        const std::string& fl = s_scene_filter_low;
        auto label_of = [&](const bng::SceneObject& o) {
            std::string lab = o.cls;
            if (!o.name.empty()) lab += "  " + o.name;
            else if (!o.shape.empty()) lab += "  " + o.shape.substr(o.shape.find_last_of('/') + 1);
            return lab;
        };
        auto matches = [&](const bng::SceneObject& o) {
            if (fl.empty()) return true;
            std::string h = o.cls + " " + o.name + " " + o.shape + " " + o.persistent_id;
            for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return h.find(fl) != std::string::npos;
        };
        auto select_level_obj = [&](std::uint32_t li) {
            const auto& so = L.objects[li];
            if (auto it = bng_rt->eid_of_level.find(li); it != bng_rt->eid_of_level.end()) {
                for (std::size_t k = 0; k < edit_objects.size(); ++k)
                    if (edit_objects[k].id == it->second) {
                        group_sel_obj.clear();
                        group_sel_obj_ids.clear();
                        selected_object = static_cast<int>(k);
                        selected_spline = -1;
                        break;
                    }
            }
            if (so.has_pos) {
                camera.target_x = so.pos[0];
                camera.target_y = so.pos[1];
                camera.target_z = so.pos[2];
                camera.ground_z = so.pos[2];
            }
            panel.set_selection_text(label_of(so));
        };

        // ── Instance browser: EVERY BeamNG instance kind ─────────────────────────────────────────────────
        // Why the lists looked empty: the dock list / the tree below only walk edit_objects and SimGroups, so
        // forest items, decal instances, roads and ground cover (none of them is an edit_object) were never
        // listed, and a level whose objects live under a plain folder has no SimGroup tree to open. Rows are
        // cached and rebuilt only when the level counters change (never per frame) and drawn with
        // ImGuiListClipper, so a level with 50k TSStatic + 60k forest instances scrolls fine.
        enum : int { kRowStatic = 0, kRowPrefab, kRowRoad, kRowForest, kRowGround, kRowDecal };
        struct BngRow {
            int kind = 0;            // kRowStatic / kRowPrefab / ... (see the enum above)
            int index = -1;          // level object / forest type / cover type / decal type index
            std::uint32_t count = 0; // instances behind the row (prefab children, trees, decals...)
            float pos[3]{0.f, 0.f, 0.f};
            std::string label, hay; // label + lowercase haystack (name, class, shape / material)
        };
        static std::vector<BngRow> s_inst_rows;
        static std::vector<int> s_inst_hits;
        static std::unordered_map<std::string, std::uint32_t> s_prefab_children;
        static std::uint64_t s_inst_ver = ~std::uint64_t(0);
        static std::string s_inst_fl;
        static bool s_inst_hits_valid = false;
        static std::uint32_t s_n_static = 0, s_n_prefab = 0, s_n_road = 0, s_n_forest = 0, s_n_decal = 0;
        auto center_cam = [&](const float p[3], float rad) {
            camera.target_x = p[0];
            camera.target_y = p[1];
            camera.target_z = p[2];
            camera.ground_z = p[2];
            if (rad > 0.f) camera.radius = (std::clamp)(rad, 30.f, 600.f);
        };
        std::uint64_t inst_ver = 1469598103934665603ull;
        auto mix = [&](std::uint64_t v) { inst_ver = (inst_ver ^ v) * 1099511628211ull; };
        mix(L.objects.size());
        mix(L.forest.size());
        mix(L.forest_types.size());
        mix(L.decals.size());
        mix(L.decal_types.size());
        mix(R.ground_covers.size());
        mix(L.forest_files_dirty.size());
        mix(scene_deleted);
        if (inst_ver != s_inst_ver) {
            s_inst_ver = inst_ver;
            s_inst_rows.clear();
            s_inst_rows.reserve(L.objects.size() / 2 + L.forest_types.size() + L.decal_types.size() + 16);
            auto leaf_of = [](const std::string& s) {
                const auto p = s.find_last_of("/\\");
                return p == std::string::npos ? s : s.substr(p + 1);
            };
            auto add_row = [&](int kind, int index, std::uint32_t count, const std::string& label, const float* pos,
                               const std::string& extra) {
                BngRow r;
                r.kind = kind;
                r.index = index;
                r.count = count;
                if (pos) std::memcpy(r.pos, pos, sizeof(r.pos));
                r.label = label;
                r.hay = label + " " + extra;
                for (auto& c : r.hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                s_inst_rows.push_back(std::move(r));
            };
            s_n_static = s_n_prefab = s_n_road = s_n_forest = s_n_decal = 0;
            for (std::size_t i = 0; i < L.objects.size(); ++i) {
                const auto& so = L.objects[i];
                if (so.deleted) continue;
                if (so.cls == "TSStatic") {
                    if (so.shape.empty()) continue;
                    const std::string lab = "TSStatic  " + (so.name.empty() ? leaf_of(so.shape) : so.name);
                    add_row(kRowStatic, static_cast<int>(i), 1, lab, so.has_pos ? so.pos : nullptr, so.shape);
                    ++s_n_static;
                } else if (so.cls == "Prefab") {
                    const std::string file = so.raw.get("filename").as_string();
                    std::uint32_t kids = 0;
                    if (!file.empty()) {
                        auto it = s_prefab_children.find(file);
                        if (it != s_prefab_children.end()) {
                            kids = it->second;
                        } else {
                            auto d = R.prefab_defs.find(file);
                            if (d == R.prefab_defs.end() && s_prefab_children.size() < std::size_t{512}) {
                                // same cache the spawner uses: read once per prefab file, never per frame
                                auto def = std::make_shared<std::vector<bng::PrefabChild>>(bng::load_prefab(
                                    *R.s->vfs, bng::Vfs::normalize(file), R.s->info.dir));
                                d = R.prefab_defs.emplace(file, std::move(def)).first;
                            }
                            if (d != R.prefab_defs.end() && d->second) kids = static_cast<std::uint32_t>(d->second->size());
                            s_prefab_children[file] = kids;
                        }
                    }
                    const std::string lab = "Prefab  " + (so.name.empty() ? leaf_of(file) : so.name);
                    add_row(kRowPrefab, static_cast<int>(i), kids, lab, so.has_pos ? so.pos : nullptr, file);
                    ++s_n_prefab;
                } else if (so.cls == "DecalRoad" || so.cls == "River" || so.cls == "WaterBlock" ||
                           so.cls == "WaterPlane") {
                    const std::string mat = so.raw.get("material").as_string();
                    const std::string lab = so.cls + "  " + (so.name.empty() ? leaf_of(mat) : so.name);
                    add_row(kRowRoad, static_cast<int>(i), 1, lab, so.has_pos ? so.pos : nullptr, mat);
                    ++s_n_road;
                }
            }
            for (std::size_t t = 0; t < L.forest_types.size(); ++t) {
                std::uint32_t n = 0;
                float first[3]{0.f, 0.f, 0.f};
                for (const auto& fi : L.forest) {
                    if (fi.type != static_cast<int>(t) || fi.deleted) continue;
                    if (n == 0) std::memcpy(first, fi.pos, sizeof(first));
                    ++n;
                }
                if (n == 0) continue;
                const auto& ty = L.forest_types[t];
                std::string nm = ty.name;
                if (nm.empty()) nm = leaf_of(ty.shape_file);
                add_row(kRowForest, static_cast<int>(t), n, "Forest  " + nm, first, ty.shape_file);
                s_n_forest += n;
            }
            for (std::size_t t = 0; t < L.decal_types.size(); ++t) {
                std::uint32_t n = 0;
                float first[3]{0.f, 0.f, 0.f};
                for (const auto& d : L.decals) {
                    if (d.type != static_cast<int>(t)) continue;
                    if (n == 0) std::memcpy(first, d.pos, sizeof(first));
                    ++n;
                }
                if (n == 0) continue;
                add_row(kRowDecal, static_cast<int>(t), n, "Decal  " + L.decal_types[t].name, first,
                        L.decal_types[t].material);
                s_n_decal += n;
            }
            for (std::size_t g = 0; g < R.ground_covers.size(); ++g) {
                const auto& gc = R.ground_covers[g];
                std::string nm = leaf_of(gc.texture);
                if (nm.empty()) nm = "groundcover #" + std::to_string(g);
                add_row(kRowGround, static_cast<int>(g), 0, "GroundCover  " + nm, nullptr, gc.texture);
            }
            s_inst_hits_valid = false;
        }
        // Per-cover live cell counter (streaming changes every frame: computed here, never in the cached rows).
        std::vector<std::uint32_t> gc_live(R.ground_covers.size(), 0);
        for (const auto& [gk, gcell] : R.gc_cells) {
            const std::size_t gi = static_cast<std::size_t>(gk >> 48);
            if (gi < gc_live.size() && gcell.visible && gcell.chunk != static_cast<std::size_t>(-1)) ++gc_live[gi];
        }
        if (s_inst_fl != fl) {
            s_inst_fl = fl;
            s_inst_hits_valid = false; // the filter changed: rebuild the hit list only
        }
        if (!s_inst_hits_valid) {
            s_inst_hits.clear();
            s_inst_hits.reserve(s_inst_rows.size());
            for (std::size_t i = 0; i < s_inst_rows.size(); ++i)
                if (fl.empty() || s_inst_rows[i].hay.find(fl) != std::string::npos)
                    s_inst_hits.push_back(static_cast<int>(i));
            s_inst_hits_valid = true;
        }
        auto row_selected = [&](const BngRow& r) -> bool {
            if (r.kind > kRowPrefab || r.index < 0 || static_cast<std::size_t>(r.index) >= L.objects.size()) return false;
            if (selected_object < 0 || static_cast<std::size_t>(selected_object) >= edit_objects.size()) return false;
            const auto eit = R.eid_of_level.find(static_cast<std::uint32_t>(r.index));
            return eit != R.eid_of_level.end() && edit_objects[static_cast<std::size_t>(selected_object)].id == eit->second;
        };
        auto center_row = [&](const BngRow& r) {
            if (r.kind <= kRowRoad) {
                center_cam(r.pos, 0.f);
            } else if (r.kind == kRowForest) {
                double sx = 0.0, sy = 0.0, sz = 0.0;
                std::size_t n = 0;
                for (const auto& fi : L.forest)
                    if (fi.type == r.index && !fi.deleted) {
                        sx += fi.pos[0];
                        sy += fi.pos[1];
                        sz += fi.pos[2];
                        ++n;
                    }
                if (n) {
                    const float p[3]{static_cast<float>(sx / static_cast<double>(n)),
                                     static_cast<float>(sy / static_cast<double>(n)),
                                     static_cast<float>(sz / static_cast<double>(n))};
                    center_cam(p, 0.f);
                }
            } else if (r.kind == kRowGround) {
                double sx = 0.0, sy = 0.0;
                std::size_t n = 0;
                for (const auto& [gk, gcell] : R.gc_cells) {
                    (void)gcell;
                    if ((gk >> 48) != static_cast<std::uint64_t>(r.index)) continue;
                    const int gx = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>((gk >> 24) & 0xFFFFFF) << 8) >> 8);
                    const int gy = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(gk & 0xFFFFFF) << 8) >> 8);
                    sx += (static_cast<double>(gx) + 0.5) * kBngGcCell;
                    sy += (static_cast<double>(gy) + 0.5) * kBngGcCell;
                    ++n;
                }
                if (n) {
                    const float px = static_cast<float>(sx / static_cast<double>(n));
                    const float py = static_cast<float>(sy / static_cast<double>(n));
                    const float p[3]{px, py, L.terrain ? bng_terrain_tri_z(*L.terrain, px, py) : 0.f};
                    center_cam(p, 0.f);
                }
            } else { // decal type
                double sx = 0.0, sy = 0.0, sz = 0.0;
                std::size_t n = 0;
                for (const auto& d : L.decals)
                    if (d.type == r.index) {
                        sx += d.pos[0];
                        sy += d.pos[1];
                        sz += d.pos[2];
                        ++n;
                    }
                if (n) {
                    const float p[3]{static_cast<float>(sx / static_cast<double>(n)),
                                     static_cast<float>(sy / static_cast<double>(n)),
                                     static_cast<float>(sz / static_cast<double>(n))};
                    center_cam(p, 0.f);
                }
            }
        };
        auto select_row = [&](const BngRow& r) {
            if (r.kind <= kRowPrefab) {
                select_level_obj(static_cast<std::uint32_t>(r.index)); // selects the edit object AND centres
                return;
            }
            if (r.kind == kRowForest) {
                for (std::size_t i = 0; i < L.forest.size(); ++i) {
                    const auto& fi = L.forest[i];
                    if (fi.type != r.index || fi.deleted) continue;
                    if (bng_select_forest_hook) bng_select_forest_hook(static_cast<long long>(i));
                    const float p[3]{fi.pos[0], fi.pos[1], fi.pos[2]};
                    center_cam(p, 0.f);
                    panel.set_selection_text("Forest " + L.forest_types[static_cast<std::size_t>(r.index)].name);
                    break;
                }
                return;
            }
            if (r.kind == kRowGround) {
                // nearest built cell of this cover type (the viewport picks one cell, the row is the type)
                std::uint64_t best = 0;
                float bd = 1e30f;
                for (const auto& [gk, gcell] : R.gc_cells) {
                    if ((gk >> 48) != static_cast<std::uint64_t>(r.index) || !gcell.visible) continue;
                    const int gx = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>((gk >> 24) & 0xFFFFFF) << 8) >> 8);
                    const int gy = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(gk & 0xFFFFFF) << 8) >> 8);
                    const float d = std::hypot((static_cast<float>(gx) + 0.5f) * kBngGcCell - camera.target_x,
                                               (static_cast<float>(gy) + 0.5f) * kBngGcCell - camera.target_y);
                    if (d < bd) {
                        bd = d;
                        best = gk;
                    }
                }
                if (best && bng_select_groundcover_hook) bng_select_groundcover_hook(r.index, best);
                center_row(r);
                return;
            }
            panel.set_selection_text(r.label); // roads / decals: no edit object, just centre + selection text
            center_row(r);
        };
        ImGui::SeparatorText("Instancias del nivel");
        ImGui::TextDisabled("%u TSStatic (%zu filas) - %u prefabs - %u carreteras/rios - %zu inst. Forest - %zu decals - %zu tipos de pasto",
                            s_n_static, s_inst_rows.size(), s_n_prefab, s_n_road, static_cast<std::size_t>(s_n_forest),
                            static_cast<std::size_t>(s_n_decal), R.ground_covers.size());
        ImGui::TextDisabled("%zu visibles con el filtro", s_inst_hits.size());
        if (ImGui::BeginChild("##bng_inst_list", ImVec2(0.f, 210.f), ImGuiChildFlags_Borders)) {
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(s_inst_hits.size()));
            while (clip.Step())
                for (int row_i = clip.DisplayStart; row_i < clip.DisplayEnd; ++row_i) {
                    const int idx = s_inst_hits[static_cast<std::size_t>(row_i)];
                    const BngRow& row = s_inst_rows[static_cast<std::size_t>(idx)];
                    std::string lab = row.label;
                    if (row.kind == kRowForest) lab += "  (" + std::to_string(row.count) + " inst.)";
                    else if (row.kind == kRowPrefab) lab += "  (" + std::to_string(row.count) + " hijos)";
                    else if (row.kind == kRowDecal) lab += "  (" + std::to_string(row.count) + " decals)";
                    else if (row.kind == kRowGround) {
                        const std::size_t gi = static_cast<std::size_t>(row.index);
                        lab += "  (" + std::to_string(gi < gc_live.size() ? gc_live[gi] : 0u) + " celdas de 32 m)";
                    }
                    ImGui::PushID(0x4200 + idx);
                    const float bw = ImGui::CalcTextSize("Centrar").x + ImGui::GetStyle().FramePadding.x * 2.f;
                    if (ImGui::Selectable(lab.c_str(), row_selected(row), ImGuiSelectableFlags_AllowOverlap,
                                          ImVec2((std::max)(40.f, ImGui::GetContentRegionAvail().x - bw - 8.f), 0.f)))
                        select_row(row);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Centrar")) center_row(row);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mueve la camara al centro de esta fila (sin cambiar la seleccion)");
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();

        if (!fl.empty()) {
            // flat filtered list (cached: rebuilt only when the filter or the objects change)
            if (!s_scene_hits_valid) {
                s_scene_hits.clear();
                s_scene_hits.reserve(L.objects.size() / 8 + 16);
                for (std::uint32_t i = 0; i < L.objects.size(); ++i)
                    if (!L.objects[i].deleted && matches(L.objects[i])) s_scene_hits.push_back(i);
                s_scene_hits_valid = true;
            }
            const auto& hits = s_scene_hits;
            ImGui::TextDisabled("%zu resultados", hits.size());
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(hits.size()));
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    const auto li = hits[static_cast<std::size_t>(r)];
                    ImGui::PushID(static_cast<int>(li));
                    if (ImGui::Selectable(label_of(L.objects[li]).c_str())) select_level_obj(li);
                    ImGui::PopID();
                }
            return;
        }
        // per-group item vectors, cached until the objects change (was rebuilt for every open group every frame)
        if (!s_scene_groups_valid) {
            s_scene_group_items.clear();
            for (std::size_t gi = 0; gi < L.groups.size(); ++gi) {
                const auto& g = L.groups[gi];
                auto& items = s_scene_group_items[static_cast<int>(gi)];
                items.reserve(g.objects.size());
                for (const auto id : g.objects)
                    if (id < L.objects.size() && !L.objects[id].deleted && L.objects[id].cls != "SimGroup") items.push_back(id);
            }
            s_scene_groups_valid = true;
        }
        std::function<void(int)> draw_group = [&](int gi) {
            const auto& g = L.groups[static_cast<std::size_t>(gi)];
            const std::string head = g.name + "  (" + std::to_string(g.objects.size()) + ")";
            if (!ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::intptr_t>(gi + 1)), ImGuiTreeNodeFlags_OpenOnArrow,
                                   "%s", head.c_str()))
                return;
            for (const int c : g.children) draw_group(c);
            const auto it_items = s_scene_group_items.find(gi);
            const auto& items = it_items == s_scene_group_items.end() ? s_scene_no_items : it_items->second;
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(items.size()));
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    const auto li = items[static_cast<std::size_t>(r)];
                    ImGui::PushID(static_cast<int>(li) + 1000000);
                    if (ImGui::Selectable(label_of(L.objects[li]).c_str())) select_level_obj(li);
                    ImGui::PopID();
                }
            ImGui::TreePop();
        };
        for (std::size_t gi = 0; gi < L.groups.size(); ++gi)
            if (L.group_by_name.find(L.groups[gi].parent) == L.group_by_name.end()) draw_group(static_cast<int>(gi));
    };

    // Rebuilds one ribbon from its (edited) level object.
    auto bng_rebuild_ribbon = [&](int ri) {
        auto& rb = bng_rt->ribbons[static_cast<std::size_t>(ri)];
        if (rb.chunk != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(rb.chunk, false);
        rb.chunk = static_cast<std::size_t>(-1); // the old chunk stays hidden; a new one is streamed in
        rb.visible = false;
        rb.failed = false;
        rb.building = false;
        rb.no_draw = false; // re-decided by bng_upload_ribbon (the material may have been edited)
        const auto& so = bng_rt->s->level->objects[rb.level_index];
        const auto nodes = bng::read_road_nodes(so.raw.get("nodes"));
        float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
        for (const auto& n : nodes) {
            mnx = (std::min)(mnx, n.x - n.w);
            mxx = (std::max)(mxx, n.x + n.w);
            mny = (std::min)(mny, n.y - n.w);
            mxy = (std::max)(mxy, n.y + n.w);
        }
        if (!nodes.empty()) {
            rb.center[0] = 0.5f * (mnx + mxx);
            rb.center[1] = 0.5f * (mny + mxy);
            rb.radius = 0.5f * std::hypot(mxx - mnx, mxy - mny);
            rb.node_xy.clear();
            rb.node_xy.reserve(nodes.size() * 3);
            for (const auto& n : nodes) {
                rb.node_xy.push_back(n.x);
                rb.node_xy.push_back(n.y);
                rb.node_xy.push_back(0.5f * n.w);
            }
        }
        bng_rt->dirty_objects.insert(rb.level_index);
    };

    bng_ui_roads_tab = [&, bng_rebuild_ribbon]() {
        if (!bng_rt) return;
        auto& R = *bng_rt;
        auto& L = *R.s->level;
        R.roads_tab_open = true;
        static char filter[96]{};
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##road_filter", "Buscar carretera (nombre, material, grupo)...", filter, sizeof(filter));
        std::string f = filter;
        for (auto& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto label = [&](int ri) {
            const auto& rb = R.ribbons[static_cast<std::size_t>(ri)];
            const auto& so = L.objects[rb.level_index];
            const char* kinds[] = {"DecalRoad", "River", "WaterBlock", "WaterPlane"};
            std::string s = std::string(kinds[rb.kind]) + "  " + (so.name.empty() ? rb.material : so.name);
            if (!so.parent.empty()) s += "   [" + so.parent + "]";
            return s;
        };
        std::vector<int> rows;
        for (int i = 0; i < static_cast<int>(R.ribbons.size()); ++i) {
            if (R.ribbons[static_cast<std::size_t>(i)].kind > 1) continue;
            if (!f.empty()) {
                std::string h = label(i);
                for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (h.find(f) == std::string::npos) continue;
            }
            rows.push_back(i);
        }
        ImGui::TextDisabled("%zu carreteras / rios", rows.size());
        if (ImGui::BeginChild("##road_list", ImVec2(0.f, 170.f), ImGuiChildFlags_Borders)) {
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(rows.size()));
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    const int ri = rows[static_cast<std::size_t>(r)];
                    ImGui::PushID(ri);
                    if (ImGui::Selectable(label(ri).c_str(), R.sel_ribbon == ri)) {
                        if (R.sel_ribbon >= 0 && R.ribbons[static_cast<std::size_t>(R.sel_ribbon)].chunk != static_cast<std::size_t>(-1))
                            renderer.set_chunk_highlight(R.ribbons[static_cast<std::size_t>(R.sel_ribbon)].chunk, 0.f);
                        R.sel_ribbon = ri;
                        R.sel_node = -1;
                        const auto& rb = R.ribbons[static_cast<std::size_t>(ri)];
                        camera.target_x = rb.center[0];
                        camera.target_y = rb.center[1];
                        if (camera.sample_ground_xy)
                            if (auto z = camera.sample_ground_xy(rb.center[0], rb.center[1])) camera.target_z = *z;
                        camera.radius = (std::clamp)(rb.radius * 1.6f, 30.f, 600.f);
                    }
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();
        if (R.sel_ribbon < 0 || R.sel_ribbon >= static_cast<int>(R.ribbons.size())) {
            ImGui::TextDisabled("Elegi una carretera para editarla. Sus nodos se ven en el 3D.");
            return;
        }
        auto& rb = R.ribbons[static_cast<std::size_t>(R.sel_ribbon)];
        auto& so = L.objects[rb.level_index];
        if (rb.chunk != static_cast<std::size_t>(-1)) renderer.set_chunk_highlight(rb.chunk, 0.35f, 0.3f, 0.8f, 1.f);
        ImGui::SeparatorText(so.cls.c_str());
        bool rebuild = false;
        // material / scalar fields (written straight into the level JSON)
        {
            char mat[128];
            std::snprintf(mat, sizeof(mat), "%s", so.raw.get("material").as_string().c_str());
            ImGui::SetNextItemWidth(-90.f);
            if (ImGui::InputText("material", mat, sizeof(mat), ImGuiInputTextFlags_EnterReturnsTrue)) {
                so.raw.set("material", bng::JsonValue::make_string(mat));
                rb.material = mat;
                rebuild = true;
            }
            auto num = [&](const char* key, const char* lab, float def, float spd, float mn, float mx) {
                float v = static_cast<float>(so.raw.get(key).as_number_lenient(def));
                ImGui::SetNextItemWidth(-90.f);
                if (ImGui::DragFloat(lab, &v, spd, mn, mx, "%.3f")) so.raw.set(key, bng::JsonValue::make_number(v));
                if (ImGui::IsItemDeactivatedAfterEdit()) rebuild = true;
            };
            num("textureLength", "textureLength", 5.f, 0.1f, 0.1f, 1000.f);
            num("renderPriority", "renderPriority", 10.f, 1.f, 0.f, 100.f);
            num("breakAngle", "breakAngle", 3.f, 0.1f, 0.f, 180.f);
            bool looped = so.raw.get("looped").as_bool();
            if (ImGui::Checkbox("looped", &looped)) {
                so.raw.set("looped", bng::JsonValue::make_bool(looped));
                rebuild = true;
            }
            ImGui::SameLine();
            bool over = so.raw.get("overObjects").as_bool();
            if (ImGui::Checkbox("overObjects", &over)) {
                so.raw.set("overObjects", bng::JsonValue::make_bool(over));
                rb.over_objects = over;
                rebuild = true;
            }
            ImGui::SameLine();
            int drv = static_cast<int>(so.raw.get("drivability").as_number_lenient(0.0));
            ImGui::SetNextItemWidth(60.f);
            if (ImGui::InputInt("drivability", &drv, 0)) so.raw.set("drivability", bng::JsonValue::make_number(drv));
        }
        // nodes
        if (!so.raw.find("nodes")) return;
        auto& nodes = *so.raw.find("nodes");
        ImGui::SeparatorText("Nodos (x, y, z, ancho)");
        if (ImGui::Button("Ensanchar +10%")) {
            for (auto& n : nodes.items())
                if (n.is_array() && n.size() > 3) n.at(3) = bng::JsonValue::make_number(n[3].as_number() * 1.1);
            rebuild = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Angostar -10%")) {
            for (auto& n : nodes.items())
                if (n.is_array() && n.size() > 3) n.at(3) = bng::JsonValue::make_number(n[3].as_number() / 1.1);
            rebuild = true;
        }
        if (ImGui::BeginTable("##nodes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                              ImVec2(0.f, 220.f))) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26.f);
            ImGui::TableSetupColumn("x");
            ImGui::TableSetupColumn("y");
            ImGui::TableSetupColumn("z");
            ImGui::TableSetupColumn("ancho");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44.f);
            ImGui::TableHeadersRow();
            int erase = -1, insert_after = -1;
            for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
                auto& n = nodes.at(static_cast<std::size_t>(i));
                if (!n.is_array() || n.size() < 4) continue;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(std::to_string(i).c_str(), R.sel_node == i, ImGuiSelectableFlags_SpanAllColumns |
                                                                                      ImGuiSelectableFlags_AllowOverlap)) {
                    R.sel_node = i;
                    camera.target_x = n[0].as_float();
                    camera.target_y = n[1].as_float();
                    camera.target_z = n[2].as_float();
                }
                for (int c = 0; c < 4; ++c) {
                    ImGui::TableNextColumn();
                    float v = n[static_cast<std::size_t>(c)].as_float();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::PushID(c);
                    if (ImGui::DragFloat("##v", &v, c == 3 ? 0.05f : 0.1f, 0.f, 0.f, "%.2f"))
                        n.at(static_cast<std::size_t>(c)) = bng::JsonValue::make_number(v);
                    if (ImGui::IsItemDeactivatedAfterEdit()) rebuild = true;
                    ImGui::PopID();
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("+")) insert_after = i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Insertar un nodo despues de este (punto medio)");
                ImGui::SameLine(0, 2);
                if (ImGui::SmallButton("x")) erase = i;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (erase >= 0 && nodes.size() > 2) {
                nodes.items().erase(nodes.items().begin() + erase);
                rebuild = true;
            }
            if (insert_after >= 0) {
                const auto& a = nodes[static_cast<std::size_t>(insert_after)];
                const auto& b = nodes[static_cast<std::size_t>((std::min)(insert_after + 1, static_cast<int>(nodes.size()) - 1))];
                bng::JsonValue m = bng::JsonValue::make_array();
                for (std::size_t c = 0; c < a.size(); ++c)
                    m.push_back(bng::JsonValue::make_number(0.5 * (a[c].as_number() + b[c].as_number())));
                nodes.items().insert(nodes.items().begin() + insert_after + 1, std::move(m));
                rebuild = true;
            }
        }
        if (rebuild) {
            so.dirty = true;
            bng_rebuild_ribbon(R.sel_ribbon);
            panel.set_status("Carretera regenerada (se guarda con Ctrl+S)");
        }
    };

    bng_spawn_hook = [&](map::MapObjectInst& o) -> bool {
        if (!bng_rt || !bng_is_obj(o)) return false;
        const std::string shape = bng_shape_of(o);
        if (bng_rt->extra.find(o.id) == bng_rt->extra.end()) bng_rt->extra[o.id] = BngObjExtra{}; // new / pasted
        if (bng_is_prefab(o)) {
            bng_place_prefab(o);
            return true;
        }
        // Root F: same 3D distance to the eye as the per-frame LOD pass (2D to the orbit target made the
        // spawn LOD disagree with the frame and the object popped between LODs).
        float eye[3];
        camera.eye_position(eye[0], eye[1], eye[2]);
        auto eye_dist = [&]() {
            const float dx = o.position.x - eye[0], dy = o.position.y - eye[1], dz = o.position.z - eye[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        auto it = bng_rt->shapes.find(shape);
        if (it != bng_rt->shapes.end() && it->second.ready()) {
            const float d = eye_dist();
            const BngInst* cur_inst = bng_rt->chunk_of.count(o.id) ? &bng_rt->chunk_of[o.id] : nullptr;
            bng_place_clone(o, it->second,
                            bng_pick_lod(it->second, bng_obj_scale_max(o.id), d, cur_inst ? cur_inst->cur : -1, cur_inst));
            return true;
        }
        if (it != bng_rt->shapes.end() && it->second.failed) return true;
        bng_rt->pool->request(shape, eye_dist());
        auto& w = bng_rt->waiting[shape];
        if (std::find(w.begin(), w.end(), o.id) == w.end()) w.push_back(o.id);
        return true;
    };
    bng_sync_hook = [&](map::MapObjectInst& o) -> bool {
        if (!bng_rt || !bng_is_obj(o)) return false;
        if (bng_is_prefab(o)) {
            if (bng_rt->spawned.count(o.id)) bng_place_prefab(o);
            return true;
        }
        auto it = bng_rt->chunk_of.find(o.id);
        if (it == bng_rt->chunk_of.end()) return true;
        float w[16];
        bng_world16(o, w);
        for (const auto c : it->second.lod_chunks)
            if (c != static_cast<std::size_t>(-1)) renderer.set_chunk_world_matrix(c, w);
        for (const std::size_t i : bng_scene_places_of(o.id)) {
            if (i >= scene_places.size()) continue;
            auto& sp = scene_places[i];
            sp.tx = o.position.x;
            sp.ty = o.position.y;
            sp.tz = o.position.z;
        }
        return true;
    };
    // ── Forest pick / selection hooks (Root D) ────────────────────────────────────────────────────────────
    // PURE CPU pick, independent of the renderer: a 32 m spatial hash over Level::forest (pos / scale / type /
    // radius+height of the type's shape or defaults), the camera ray from the mouse and a vertical-cylinder test
    // per instance in the cells the ray crosses (up to 400 m), nearest wins. The old version iterated
    // BngRuntime::forest_chunk, i.e. only the instances the renderer had ALREADY cloned for the current LOD (the
    // pump in mod_beamng_frame fills it with a 1.5 ms/frame budget and bng_hide_inst leaves entries with cur = -1),
    // so every tree that was not streamed yet was unclickable: that was the broken click.
    bng_pick_forest_hook = [&](float mx, float my, float fw, float fh, const float* wvp_t,
                               std::uint32_t* out_fid) -> float {
        if (!bng_rt || fw <= 0.f || fh <= 0.f) return 1e30f;
        auto& Rr = *bng_rt;
        const auto& Fl = *Rr.s->level;
        if (Fl.forest.empty()) return 1e30f;
        DirectX::XMFLOAT4X4 vp{};
        std::memcpy(&vp, wvp_t, sizeof(vp));
        const DirectX::XMMATRIX inv =
            DirectX::XMMatrixInverse(nullptr, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&vp)));
        const float nx = 2.f * mx / fw - 1.f, ny = 1.f - 2.f * my / fh;
        DirectX::XMFLOAT4 n4{}, f4{};
        // same near->far convention as renderer.pick_chunk (reverse-Z NDC)
        DirectX::XMStoreFloat4(&n4, DirectX::XMVector4Transform(DirectX::XMVectorSet(nx, ny, 1.f, 1.f), inv));
        DirectX::XMStoreFloat4(&f4, DirectX::XMVector4Transform(DirectX::XMVectorSet(nx, ny, 0.f, 1.f), inv));
        if (std::fabs(n4.w) < 1e-8f || std::fabs(f4.w) < 1e-8f) return 1e30f;
        const float ox = n4.x / n4.w, oy = n4.y / n4.w, oz = n4.z / n4.w;
        float dx = f4.x / f4.w - ox, dy = f4.y / f4.w - oy, dz = f4.z / f4.w - oz;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(len > 1e-6f)) return 1e30f;
        dx /= len;
        dy /= len;
        dz /= len;
        const float a2 = dx * dx + dy * dy;
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        // Screen-space tolerance: a trunk is 1-2 px wide at 40 m and the crown is what the user aims at.
        const float min_px = (std::max)(7.f, 0.008f * disp.y);
        float best_t = 1e30f;
        std::uint32_t best = 0;
        // ── spatial hash (CPU only) ────────────────────────────────────────────────────────────────────
        static std::unordered_map<std::int64_t, std::vector<std::uint32_t>> s_veg_grid;
        static std::size_t s_veg_grid_n = static_cast<std::size_t>(-1);
        static std::uint64_t s_veg_grid_ver = ~0ull;
        auto grid_key = [](int gx, int gy) -> std::int64_t {
            return (static_cast<std::int64_t>(gx) << 32) | static_cast<std::int64_t>(static_cast<std::uint32_t>(gy));
        };
        if (s_veg_grid_n != Fl.forest.size() || s_veg_grid_ver != s_forest_grid_ver) {
            s_veg_grid.clear();
            s_veg_grid.reserve(Fl.forest.size() / 4 + 16);
            for (std::size_t i = 0; i < Fl.forest.size(); ++i) {
                const auto& fi = Fl.forest[i];
                if (fi.deleted) continue;
                const int gx = static_cast<int>(std::floor(fi.pos[0] / kForestCell));
                const int gy = static_cast<int>(std::floor(fi.pos[1] / kForestCell));
                s_veg_grid[grid_key(gx, gy)].push_back(static_cast<std::uint32_t>(i));
            }
            s_veg_grid_n = Fl.forest.size();
            s_veg_grid_ver = s_forest_grid_ver;
        }
        // Cells crossed by the ray: one sample every 32 m of ray plus a one-cell halo (a diagonal ray cannot skip
        // a cell that way) and a visited list so each cell is walked once. 400 m is the pick range.
        static std::vector<std::uint32_t> s_veg_cand;
        static std::vector<std::int64_t> s_veg_visited;
        s_veg_cand.clear();
        s_veg_visited.clear();
        for (float s = 0.f; s <= 400.f; s += kForestCell) {
            const float px = ox + dx * s, py = oy + dy * s;
            const int gx0 = static_cast<int>(std::floor(px / kForestCell));
            const int gy0 = static_cast<int>(std::floor(py / kForestCell));
            for (int gy = -1; gy <= 1; ++gy)
                for (int gx = -1; gx <= 1; ++gx) {
                    const std::int64_t k = grid_key(gx0 + gx, gy0 + gy);
                    if (std::find(s_veg_visited.begin(), s_veg_visited.end(), k) != s_veg_visited.end()) continue;
                    s_veg_visited.push_back(k);
                    const auto itg = s_veg_grid.find(k);
                    if (itg == s_veg_grid.end()) continue;
                    for (const std::uint32_t ci : itg->second) s_veg_cand.push_back(ci);
                }
        }
        // Every candidate is tested at every LOD / impostor: the body test never looks at the geometry.
        for (const std::uint32_t fid : s_veg_cand) {
            if (static_cast<std::size_t>(fid) >= Fl.forest.size()) continue;
            const auto& fi = Fl.forest[static_cast<std::size_t>(fid)];
            if (fi.deleted || fi.type < 0 || static_cast<std::size_t>(fi.type) >= Fl.forest_types.size()) continue;
            const auto& ty = Fl.forest_types[static_cast<std::size_t>(fi.type)];
            const float scl = std::fabs(fi.scale) > 1e-3f ? std::fabs(fi.scale) : 1.f;
            // Radius / height: the type's real shape bounds when the asset pool already knows them (g.radius /
            // g.height, shape space), else the managedItemData radius plus a default height. never requires the
            // mesh to be uploaded or a LOD clone to exist (renderer-independent pick).
            float rad = ty.radius > 0.01f ? ty.radius * scl : 1.5f * scl;
            float height = 0.f;
            if (auto sg = Rr.shapes.find(ty.shape_file);
                sg != Rr.shapes.end() && sg->second.radius > 0.f && sg->second.height > 0.f) {
                rad = (std::max)(rad, sg->second.radius * scl * 0.5f);
                height = sg->second.height * scl;
            }
            rad = (std::max)(0.25f, rad);
            if (!(height > 0.1f)) height = (std::max)(3.f, 2.6f * rad); // crown tip: clicking the top must work
            height = (std::max)(height, 1.6f * rad);
            const float z0 = fi.pos[2];
            float t = 1e30f;
            if (a2 > 1e-9f) {
                const float px = fi.pos[0] - ox, py = fi.pos[1] - oy;
                const float b = px * dx + py * dy;
                const float c = px * px + py * py - rad * rad;
                const float disc = b * b - a2 * c;
                if (disc >= 0.f) {
                    const float t0 = (-b - std::sqrt(disc)) / a2;
                    if (t0 >= 0.f) {
                        const float hz = oz + t0 * dz;
                        // entry point must be inside the trunk..crown slab, not the infinite cylinder
                        if (hz >= z0 - 1.f && hz <= z0 + height) t = t0;
                    }
                }
            }
            // Crown from above (near-vertical ray): the cap of the cylinder is a valid hit.
            if (std::fabs(dz) > 1e-6f) {
                const float tc = (z0 + height - oz) / dz;
                if (tc >= 0.f && tc < t) {
                    const float hx = ox + tc * dx - fi.pos[0], hy = oy + tc * dy - fi.pos[1];
                    if (hx * hx + hy * hy <= rad * rad) t = tc;
                }
            }
            if (t >= 1e29f) {
                // Screen-space fallback: distance from the cursor to the projected trunk..crown axis.
                float sx0 = 0.f, sy0 = 0.f, sx1 = 0.f, sy1 = 0.f;
                if (world_to_screen(fi.pos[0], fi.pos[1], z0, mx, my, fw, fh, wvp_t, sx0, sy0) &&
                    world_to_screen(fi.pos[0], fi.pos[1], z0 + height, mx, my, fw, fh, wvp_t, sx1, sy1)) {
                    const float ex = sx1 - sx0, ey = sy1 - sy0;
                    const float l2 = ex * ex + ey * ey;
                    float u = l2 > 1e-6f ? ((mx - sx0) * ex + (my - sy0) * ey) / l2 : 0.f;
                    u = (std::clamp)(u, 0.f, 1.f);
                    const float ddx = sx0 + ex * u - mx, ddy = sy0 + ey * u - my;
                    float rad_px = min_px;
                    float rx = 0.f, ry = 0.f;
                    if (world_to_screen(fi.pos[0] + rad, fi.pos[1], z0, mx, my, fw, fh, wvp_t, rx, ry))
                        rad_px = (std::max)(rad_px, std::hypot(rx - sx0, ry - sy0));
                    if (std::sqrt(ddx * ddx + ddy * ddy) <= rad_px) {
                        // order by distance along the ray (nearest to the camera wins)
                        const float px = fi.pos[0] - ox, py = fi.pos[1] - oy, pz = z0 - oz;
                        const float tc = px * dx + py * dy + pz * dz;
                        if (tc > 0.f) t = tc;
                    }
                }
            }
            if (t < best_t) {
                best_t = t;
                best = fid;
            }
        }
        if (best_t > 1e29f) return 1e30f;
        if (out_fid) *out_fid = best;
        return best_t;
    };
    bng_select_forest_hook = [&](long long fid_s) {
        if (!bng_rt) return;
        auto& Rr = *bng_rt;
        auto& Fl = *Rr.s->level;
        auto highlight = [&](int fid0, float amount) {
            if (fid0 < 0) return;
            const auto it = Rr.forest_chunk.find(static_cast<std::uint32_t>(fid0));
            if (it == Rr.forest_chunk.end() || it->second.cur < 0) return;
            if (static_cast<std::size_t>(it->second.cur) >= it->second.lod_chunks.size()) return;
            const auto ch = it->second.lod_chunks[static_cast<std::size_t>(it->second.cur)];
            if (ch == static_cast<std::size_t>(-1)) return;
            renderer.set_chunk_highlight(ch, amount, 0.35f, 0.8f, 0.35f);
            if (amount > 0.f) Rr.sel_forest_chunk = ch;
        };
        highlight(Rr.sel_forest, 0.f);
        Rr.sel_forest_chunk = static_cast<std::size_t>(-1);
        Rr.sel_forest = -1;
        if (fid_s < 0 || static_cast<std::size_t>(fid_s) >= Fl.forest.size()) {
            panel.set_selection_text(std::string());
            return;
        }
        Rr.sel_forest = static_cast<int>(fid_s);
        const auto& fi = Fl.forest[static_cast<std::size_t>(fid_s)];
        highlight(Rr.sel_forest, 0.35f);
        std::string label = "forest  #" + std::to_string(fid_s);
        if (fi.type >= 0 && static_cast<std::size_t>(fi.type) < Fl.forest_types.size() &&
            !Fl.forest_types[static_cast<std::size_t>(fi.type)].name.empty())
            label += "  " + Fl.forest_types[static_cast<std::size_t>(fi.type)].name;
        panel.set_selection_text(label);
    };
    // ── GroundCover pick / selection hooks ──────────────────────────────────────────────────────────────────
    // PURE CPU: the camera ray is marched against the level heightfield (.ter) until it hits the ground, the hit
    // point is mapped to the 32 m cell the grass is built in and to the GroundCover type whose dissolveRadius
    // reaches it and whose types are painted on that terrain layer. The old version asked the renderer to
    // hit-test the grass chunks: they only exist for cells already streamed and are 0.5 m crossed quads, so the
    // 6 px ring almost never intersected a triangle and the grass could not be clicked.
    bng_pick_groundcover_hook = [&](float mx, float my, float fw, float fh, const float* wvp_t, int* out_gi,
                                    std::uint64_t* out_key) -> float {
        if (!bng_rt || fw <= 0.f || fh <= 0.f) return 1e30f;
        auto& Rr = *bng_rt;
        if (Rr.ground_covers.empty()) return 1e30f;
        const auto& Fl = *Rr.s->level;
        if (!Fl.terrain) return 1e30f;
        const auto& T = *Fl.terrain;
        if (T.size < 2 || T.heights.size() < static_cast<std::size_t>(T.size) * T.size) return 1e30f;
        // ray, same reverse-Z convention as renderer.pick_chunk (ndc z 1 = near plane, 0 = far plane)
        DirectX::XMFLOAT4X4 vp{};
        std::memcpy(&vp, wvp_t, sizeof(vp));
        const DirectX::XMMATRIX inv =
            DirectX::XMMatrixInverse(nullptr, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&vp)));
        const float nx = 2.f * mx / fw - 1.f, ny = 1.f - 2.f * my / fh;
        DirectX::XMFLOAT4 n4{}, f4{};
        DirectX::XMStoreFloat4(&n4, DirectX::XMVector4Transform(DirectX::XMVectorSet(nx, ny, 1.f, 1.f), inv));
        DirectX::XMStoreFloat4(&f4, DirectX::XMVector4Transform(DirectX::XMVectorSet(nx, ny, 0.f, 1.f), inv));
        if (std::fabs(n4.w) < 1e-8f || std::fabs(f4.w) < 1e-8f) return 1e30f;
        const float ox = n4.x / n4.w, oy = n4.y / n4.w, oz = n4.z / n4.w;
        float dx = f4.x / f4.w - ox, dy = f4.y / f4.w - oy, dz = f4.z / f4.w - oz;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(len > 1e-6f)) return 1e30f;
        dx /= len;
        dy /= len;
        dz /= len;
        // march under the heightfield (1 m steps growing to 8 m), then bisect the crossing
        float t_prev = 0.f, t_hit = -1.f, step = 1.f;
        for (float t = 0.f; t <= 600.f; t += step) {
            const float x = ox + dx * t, y = oy + dy * t;
            if (T.contains(x, y) && oz + dz * t <= T.sample_world(x, y)) {
                t_hit = t;
                break;
            }
            t_prev = t;
            step = (std::min)(8.f, step * 1.35f);
        }
        if (t_hit < 0.f) return 1e30f;
        float a = t_prev, b = t_hit;
        for (int it = 0; it < 14; ++it) {
            const float m = 0.5f * (a + b);
            const float x = ox + dx * m, y = oy + dy * m;
            if (!T.contains(x, y)) {
                a = m;
                continue;
            }
            if (oz + dz * m <= T.sample_world(x, y)) b = m;
            else a = m;
        }
        const float ht = 0.5f * (a + b);
        const float hx = ox + dx * ht, hy = oy + dy * ht;
        if (!T.contains(hx, hy)) return 1e30f;
        constexpr float kGcCell = 32.f; // one grass chunk per (cover type, 32 m cell)
        const int gx = static_cast<int>(std::floor(hx / kGcCell));
        const int gy = static_cast<int>(std::floor(hy / kGcCell));
        int layer = -1;
        if (!T.layers.empty()) {
            const int sx = (std::clamp)(static_cast<int>((hx - T.origin[0]) / T.square_size), 0,
                                        static_cast<int>(T.size) - 1);
            const int sy = (std::clamp)(static_cast<int>((hy - T.origin[1]) / T.square_size), 0,
                                        static_cast<int>(T.size) - 1);
            const std::size_t li = static_cast<std::size_t>(sy) * T.size + static_cast<std::size_t>(sx);
            if (li < T.layers.size()) layer = T.layers[li];
        }
        if (layer == 255) return 1e30f; // hole: neither terrain nor grass there
        // the outermost cover that reaches this point AND has grass painted on that layer (last match wins: the
        // pump draws the covers in order, so the last one is what the user sees)
        int best_gi = -1;
        for (std::size_t gi = 0; gi < Rr.ground_covers.size(); ++gi) {
            const auto& gc = Rr.ground_covers[gi];
            if (!(gc.density > 0.f) || !(gc.radius > 0.f)) continue;
            if (std::hypot(hx - camera.target_x, hy - camera.target_y) > gc.radius) continue;
            bool layer_ok = false;
            for (const auto& ty : gc.types) {
                if (ty.probability <= 0.f) continue;
                if (ty.layer < 0 || ty.layer == layer) {
                    layer_ok = true;
                    break;
                }
            }
            if (layer_ok) best_gi = static_cast<int>(gi);
        }
        if (best_gi < 0) return 1e30f;
        if (out_gi) *out_gi = best_gi;
        if (out_key)
            *out_key = (static_cast<std::uint64_t>(best_gi) << 48) |
                       (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx) & 0xFFFFFF) << 24) |
                       static_cast<std::uint64_t>(static_cast<std::uint32_t>(gy) & 0xFFFFFF);
        return ht;
    };
    bng_select_groundcover_hook = [&](int gi, std::uint64_t key) {
        if (!bng_rt) return;
        auto& Rr = *bng_rt;
        if (Rr.sel_gc_chunk != static_cast<std::size_t>(-1)) renderer.set_chunk_highlight(Rr.sel_gc_chunk, 0.f);
        Rr.sel_gc_chunk = static_cast<std::size_t>(-1);
        Rr.sel_gc = -1;
        Rr.sel_gc_key = 0;
        if (gi < 0 || static_cast<std::size_t>(gi) >= Rr.ground_covers.size()) {
            panel.set_selection_text(std::string());
            return;
        }
        Rr.sel_gc = gi;
        Rr.sel_gc_key = key;
        if (auto it = Rr.gc_cells.find(key); it != Rr.gc_cells.end() &&
                                                it->second.chunk != static_cast<std::size_t>(-1)) {
            renderer.set_chunk_highlight(it->second.chunk, 0.35f, 0.9f, 0.85f, 0.35f);
            Rr.sel_gc_chunk = it->second.chunk;
        }
        const auto& gc = Rr.ground_covers[static_cast<std::size_t>(gi)];
        std::string label = "GroundCover  #" + std::to_string(gi);
        if (!gc.texture.empty()) label += "  " + std::filesystem::path(gc.texture).filename().string();
        label += "  (" + std::to_string(gc.types.size()) + " tipos)";
        panel.set_selection_text(label);
    };
    camera.sample_ground_xy = [&](float x, float y) -> std::optional<float> {
        // Shoot straight down from just above the camera target and take the FIRST surface hit (terrain patch,
        // road ribbon, decal or object mesh). The heightfield is only a fallback: on gridmap it sits ~50 m
        // below the real floor (the concrete slabs), which is what made the camera dive under the world.
        const float top = camera.target_z + 60.f;
        if (const auto hz = renderer.pick_surface_hit_z(x, y, top, 0.f, 0.f, -1.f)) return hz;
        if (bng_rt && bng_rt->s->level->terrain && bng_rt->s->level->terrain->contains(x, y))
            return bng_rt->s->level->terrain->sample_world(x, y);
        return std::nullopt;
    };
    bng_terrain_sample = [&](float x, float y) -> std::optional<float> {
        if (!bng_rt || !bng_rt->s->level->terrain) return std::nullopt;
        const auto& t = *bng_rt->s->level->terrain;
        if (!t.contains(x, y)) return std::nullopt;
        return t.sample_world(x, y);
    };
}
// Terrain tools (heightmap + material layers of the .ter) and their "Terreno" tab body.
#define BNG_TERRAIN_SECTION_SETUP 1
#include "modules/mod_beamng_terrain.inc.cpp"
// Vegetation tools (Level::forest + GroundCover) and their "Vegetacion" tab body. After the terrain include:
// the grass brush reuses bng_ter_apply / bng_ter_touch_patches / bng_ter_flush_patches.
#define BNG_VEGETATION_SECTION_SETUP 1
#include "modules/mod_beamng_vegetation.inc.cpp"

// ── Ctrl+S in BeamNG mode ───────────────────────────────────────────────────────────────────────────────────
// Editor objects -> Level::objects (pose / new rows / deletions), then the scene files, the .ter and the forest
// are written under beamng_workspace/<level>/ (bng::save_scene mirrors the level tree; the install is untouched).
bng_save_hook = [&]() -> bool {
    if (!bng_rt || !cfg.beamng || !cfg.beamng->level) return false;
    auto& L = *cfg.beamng->level;
    std::filesystem::path root = cfg.beamng->workspace;
    if (root.empty()) root = std::filesystem::current_path() / "beamng_workspace" / cfg.beamng->info.name;
    std::unordered_set<std::uint32_t> alive;
    std::size_t added = 0;
    for (auto& o : edit_objects) {
        if (!bng_is_obj(o)) continue;
        auto& ex = bng_rt->extra[o.id];
        const bool prefab = bng_is_prefab(o);
        if (ex.level_index == UINT32_MAX || ex.level_index >= L.objects.size()) {
            bng::JsonValue raw = bng::JsonValue::make_object();
            raw.set(prefab ? "filename" : "shapeName", bng::JsonValue::make_string(bng_shape_of(o)));
            ex.level_index = bng::add_scene_object(L, prefab ? "Prefab" : "TSStatic", "MissionGroup", std::move(raw));
            if (!prefab) L.objects[ex.level_index].shape = bng_shape_of(o);
            ex.orig_yaw = o.rotation_deg.x + 1.f; // force the matrix below to be rebuilt from the euler angles
            bng_rt->eid_of_level[ex.level_index] = o.id;
            ++added;
        }
        alive.insert(ex.level_index);
        auto& so = L.objects[ex.level_index];
        so.deleted = false;
        const bool rot_same = std::fabs(ex.orig_yaw - o.rotation_deg.x) < 1e-4f &&
                              std::fabs(ex.orig_pitch - o.rotation_deg.y) < 1e-4f &&
                              std::fabs(ex.orig_bank - o.rotation_deg.z) < 1e-4f;
        const float p[3]{o.position.x, o.position.y, o.position.z};
        bool changed = !rot_same;
        for (int i = 0; i < 3; ++i)
            if (std::fabs(so.pos[i] - p[i]) > 1e-4f || std::fabs(so.scale[i] - ex.scale[i]) > 1e-6f) changed = true;
        if (!changed) continue;
        std::memcpy(so.pos, p, sizeof(so.pos));
        std::memcpy(so.scale, ex.scale, sizeof(so.scale));
        if (!rot_same) so.rot = bng::editor_euler_to_mat3(o.rotation_deg.x, o.rotation_deg.y, o.rotation_deg.z);
        so.dirty = true;
        ex.orig_yaw = o.rotation_deg.x; // the new matrix is the reference from now on
        ex.orig_pitch = o.rotation_deg.y;
        ex.orig_bank = o.rotation_deg.z;
    }
    // TSStatic / Prefab rows the editor no longer has were deleted in the viewport
    std::size_t removed = 0;
    for (auto& so : L.objects) {
        const bool editable = (so.cls == "TSStatic" && !so.shape.empty()) ||
                              (so.cls == "Prefab" && !so.raw.get("filename").as_string().empty());
        if (!editable || so.deleted || alive.count(so.id)) continue;
        so.deleted = true;
        ++removed;
    }
    for (const std::uint32_t li : bng_rt->dirty_objects)
        if (li < L.objects.size()) L.objects[li].dirty = true; // roads edited in the Carreteras tab
    const auto res = bng::save_scene(L, root);
    if (res.ok) bng_rt->dirty_objects.clear();
    const bool ter_ok = !bng_ter.modified || bng_ter_save(false);
    const bool veg_ok = !bng_veg.modified || bng_veg_save(false);
    std::string msg;
    if (!res.ok) msg = "Escena: " + res.error;
    else if (res.written.empty()) msg = "Escena: sin cambios";
    else
        msg = "Escena guardada: " + std::to_string(res.written.size()) + " archivo(s), " + std::to_string(res.changed) +
              " cambiados, " + std::to_string(added) + " nuevos, " + std::to_string(removed) +
              " borrados -> " + root.string();
    if (!ter_ok) msg += " | terreno: error";
    if (!veg_ok) msg += " | forest: error";
    panel.set_status(msg);
    log::info("beamng save: " + msg);
    return res.ok && ter_ok && veg_ok;
};

// Edit / Camera / Object actions of the World Editor menus (selection based, undoable).
#include "modules/mod_beamng_object_actions.inc.cpp"
