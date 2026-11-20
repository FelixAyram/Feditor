// BeamNG per-frame pump: radius streaming (budgeted), GPU upload of meshes finished by the AssetPool workers,
// forest instances and terrain LOD patches built on worker threads.
if (bng_rt) {
    auto& R = *bng_rt;
    const auto& lvl = *R.s->level;
    auto t_sec0 = std::chrono::steady_clock::now();
    auto elapsed_ms = [&]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_sec0).count();
    };
    // [PERF] Budget per section: every section calls section_begin() and gets its own budget. The caps used to be
    // measured from the module entry, so the first heavy section ate the budget of all the following ones.
    auto section_begin = [&]() { t_sec0 = std::chrono::steady_clock::now(); };
    constexpr double kBudgetUploads = 3.0;    // GPU upload of finished meshes
    constexpr double kBudgetObjects = 3.0;    // radius streaming / LOD of TSStatic
    constexpr double kBudgetForest = 1.5;     // forest instances
    constexpr double kBudgetRibbons = 1.5;    // road / river / water streaming
    constexpr double kBudgetRibbonJobs = 1.0; // ribbon meshes built on workers
    constexpr double kBudgetDecals = 1.5;     // decal cells streaming
    constexpr double kBudgetDecalJobs = 1.0;  // decal quads built on workers
    constexpr double kBudgetGroundCover = 1.5; // ground cover cells
    constexpr double kBudgetGcJobs = 1.0;      // ground cover meshes built on workers
    constexpr double kBudgetPatches = 1.5;     // terrain LOD patches
    const float cx = camera.target_x, cy = camera.target_y;
    const float r_in = R.radius, r_out = R.radius * 1.15f;
    float cam_eye[3]{cx, cy, camera.target_z};
    camera.eye_position(cam_eye[0], cam_eye[1], cam_eye[2]);

    bng_lock_enforce(); // locked objects (Objeto > Bloquear) snap back to their locked pose

    // The editor core loads a one-tile OMSI shim: keep its flat terrain hidden.
    if (!R.shim_hidden) {
        for (auto& tc : terrain_chunks) renderer.set_chunk_visible(tc.chunk, false);
        if (!terrain_chunks.empty()) R.shim_hidden = true;
    }

    // F8 = toggle "follow surface" (camera sits on the first mesh/object under it instead of the heightfield).
    {
        static bool f8_was = false;
        const bool f8 = glfwGetKey(window, GLFW_KEY_F8) == GLFW_PRESS;
        if (f8 && !f8_was && !panel.keyboard_captured() && !bus_drive_active) {
            camera.ground_follow = !camera.ground_follow;
            panel.set_status(camera.ground_follow ? "BeamNG: seguir superficie ON (F8)" : "BeamNG: seguir superficie OFF (F8)");
        }
        f8_was = f8;
    }

    // F11 = free-flight camera (BeamNG editor style). OMSI mode never reaches this module, so its
    // camera navigation (teleport/orbit) is untouched. Ground follow starts OFF in flight.
    {
        static bool f11_was = false;
        const bool f11 = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS;
        if (f11 && !f11_was && !panel.keyboard_captured() && !bus_drive_active) {
            camera.fly_mode = !camera.fly_mode;
            if (camera.fly_mode) {
                camera.ground_follow = false;
                camera.sync_fly_from_orbit();
                char fbuf[128];
                std::snprintf(fbuf, sizeof(fbuf),
                              "BeamNG vuelo libre ON (F11): RMB mirar, WASD mover, Q/E bajar/subir, base %.1f m/s",
                              static_cast<double>(camera.fly_speed));
                panel.set_status(fbuf);
            } else {
                camera.reset_mouse();
                panel.set_status("BeamNG vuelo libre OFF (F11)");
            }
        }
        f11_was = f11;
    }

    // ── 1) uploads of finished meshes: one hidden prototype per LOD (own budget) ────────────────────────────
    {
        section_begin();
        std::string shape;
        std::shared_ptr<bng::ReadyMesh> mesh;
        while (elapsed_ms() < kBudgetUploads && R.pool->take_ready(shape, mesh)) {
            auto& g = R.shapes[shape];
            if (!mesh || !mesh->ok || mesh->lods.empty()) {
                g.failed = true;
                log::warn("beamng shape: " + shape + " (" + (mesh ? mesh->error : std::string("null")) + ")");
                R.waiting.erase(shape);
                R.forest_waiting.erase(shape);
                continue;
            }
            for (const auto& lod : mesh->lods) {
                std::vector<gfx::dx12::ProcMeshSubrange> subs;
                for (const auto& s : lod.subs) {
                    gfx::dx12::ProcMeshSubrange pr;
                    pr.index_start = s.index_start;
                    pr.index_count = s.index_count;
                    auto u8 = [](const std::string& t) { return t.empty() ? std::filesystem::path() : std::filesystem::u8path(t); };
                    pr.texture_path = u8(s.texture);
                    pr.normal_path = u8(s.normal);
                    pr.detail_path = u8(s.detail);
                    pr.detail_normal_path = u8(s.detail_normal);
                    pr.detail_scale = s.detail_scale;
                    pr.detail_scale_y = s.detail_scale_y;
                    pr.has_solid = s.has_solid;
                    std::memcpy(pr.solid_rgba, s.solid, sizeof(pr.solid_rgba));
                    pr.alpha_mode = s.alpha_mode;
                    // Material of the sub (base colour / opacityFactor / alphaRef / doubleSided): leaves, foliage
                    // and the bus stop need CULL_NONE and the real alpha ref (Root A). The opacityMap is bound
                    // as the mask when there is also a colorMap.
                    const bng::Material* mt = s.material.empty() ? nullptr : R.pool->materials().find(s.material);
                    // The opacityMap mask must be bound even when there is NO colorMap (tyre marks / decals keep
                    // the mask in the texture alpha). Fall back to the sub's own opacity path when the material
                    // is not in the shared table.
                    auto vfs_p = [](const std::string& t) {
                        if (t.empty()) return std::filesystem::path();
                        return std::filesystem::u8path(t.rfind("vfs:", 0) == 0 ? t : "vfs:" + t);
                    };
                    std::filesystem::path mask;
                    if (mt && !mt->stage0.opacity_map.empty())
                        mask = vfs_p(mt->stage0.opacity_map);
                    else if (!s.opacity.empty())
                        mask = vfs_p(s.opacity);
                    if (!mask.empty()) {
                        pr.mask_path = mask;
                        std::memcpy(pr.mask_tint, mt ? mt->stage0.base_color : s.base_color, sizeof(pr.mask_tint));
                        pr.mask_tint[3] = mt ? mt->stage0.opacity_factor : s.opacity_factor;
                    }
                    // Per-submesh material state ALWAYS comes from the sub (bng_assets already resolved it from the
                    // level material, LOD0 inheritance included). Reading it back from the shared table is wrong:
                    // a level-only material is not there, so every near LOD fell back to alphaRef 127 and to the
                    // sub's own solid colour, which is what made foliage opaque and props flat as the camera
                    // got close.
                    if (pr.texture_path.empty() && pr.mask_path.empty() && !pr.has_solid) {
                        // No albedo at all for this LOD: draw the material colour instead of the white 1x1
                        // placeholder (only ever happens with a broken/missing material entry).
                        pr.has_solid = true;
                        const float grey[4]{0.45f, 0.45f, 0.45f, 1.f};
                        std::memcpy(pr.solid_rgba, grey, sizeof(grey));
                    }
                    static const float kWhite4[4]{1.f, 1.f, 1.f, 1.f}; // solid != tint: never square the colour
                    const bool tinted_solid = pr.has_solid && pr.texture_path.empty();
                    bng_sub_material(pr, tinted_solid ? kWhite4 : s.base_color, s.opacity_factor, s.alpha_ref,
                                     s.double_sided, mask);
                    pr.detail_distance = s.detail_distance;
                    subs.push_back(std::move(pr));
                }
                if (!renderer.append_textured_mesh_merged(lod.vertices.data(), static_cast<std::uint32_t>(lod.vertices.size()),
                                                          lod.indices.data(), static_cast<std::uint32_t>(lod.indices.size()), subs))
                    continue;
                const std::size_t proto = renderer.chunk_count() - 1;
                renderer.set_chunk_visible(proto, false);
                // A TSStatic mesh is never a terrain tile: vertex clustering would wreck a city / road mesh.
                renderer.set_chunk_lod_capable(proto, false);
                // BeamNG shapes (TSStatic / prefab / forest): never size-culled. Their only limit is the
                // app-side load radius; the projected-size cull made rocks and trees blink in and out.
                renderer.set_chunk_no_size_cull(proto, true);
                g.protos.push_back(proto);
                g.sizes.push_back(lod.detail_size);
            }
            if (!g.ready()) {
                g.failed = true;
                continue;
            }
            g.radius = (std::max)(0.25f, mesh->radius);
            g.height = (std::max)(0.25f, mesh->bmax[2] - mesh->bmin[2]);
            ++R.uploaded_shapes;
            if (auto w = R.waiting.find(shape); w != R.waiting.end()) {
                for (const auto id : w->second)
                    for (auto& o : edit_objects)
                        if (o.id == id) {
                            // distance to the object's surface, not to its centre (huge meshes stream in properly)
                            const float dx = o.position.x - cx, dy = o.position.y - cy;
                            const float dc = std::sqrt(dx * dx + dy * dy);
                            const float ro = (std::max)(0.25f, g.radius * bng_obj_scale_max(o.id));
                            if ((std::max)(dc - ro, 0.5f) <= r_out)
                                bng_place_clone(o, g, bng_pick_lod(g, bng_obj_scale_max(o.id), dc, -1));
                            break;
                        }
                R.waiting.erase(w);
            }
            if (auto w = R.prefab_waiting.find(shape); w != R.prefab_waiting.end()) {
                const auto ids = std::move(w->second);
                R.prefab_waiting.erase(w);
                for (const auto id : ids)
                    if (R.spawned.count(id))
                        for (auto& o : edit_objects)
                            if (o.id == id) {
                                bng_place_prefab(o);
                                break;
                            }
            }
            if (auto w = R.forest_waiting.find(shape); w != R.forest_waiting.end()) {
                for (const auto fid : w->second) {
                    const auto& fi = lvl.forest[fid];
                    float m16[16];
                    const float sc[3]{fi.scale, fi.scale, fi.scale};
                    bng::mat3_to_world4x4(fi.rot, sc, fi.pos, m16);
                    const float d = std::hypot(fi.pos[0] - cx, fi.pos[1] - cy);
                    auto& finst = R.forest_chunk[fid];
                    bng_show_inst(finst, g, bng_pick_lod(g, fi.scale, d, finst.cur, &finst), m16);
                }
                R.forest_waiting.erase(w);
            }
        }
        renderer.flush_batched_mesh_uploads();
    }

    // ── 2) radius streaming + LOD of TSStatic (rotating slices, own budget) ────────────────────────────────
    if (!edit_objects.empty()) {
        section_begin();
        // Static camera: walk a smaller slice per frame (the cursor keeps rotating, so nothing is skipped) and
        // keep the saved time for rendering. Moving camera: the full slice, the user is watching the streaming.
        const bool cam_moved = std::fabs(cx - R.stream_cam_x) > 0.05f || std::fabs(cy - R.stream_cam_y) > 0.05f;
        R.stream_cam_x = cx;
        R.stream_cam_y = cy;
        const std::size_t slice = (std::min)(edit_objects.size(), cam_moved ? std::size_t(4000) : std::size_t(1500));
        const auto& hidden_ids = panel.imgui_state().hidden_object_ids; // H / "Ocultar seleccion"
        for (std::size_t k = 0; k < slice && elapsed_ms() < kBudgetObjects; ++k) {
            if (R.stream_cursor >= edit_objects.size()) R.stream_cursor = 0;
            auto& o = edit_objects[R.stream_cursor++];
            if (!bng_is_obj(o)) continue;
            if (hidden_ids.count(o.id)) {
                if (R.spawned.count(o.id)) bng_hide_object(o.id);
                continue;
            }
            // Request gate: while the shape bounds are unknown (0.5 m fallback) a huge mesh would only be
            // requested once the camera reaches its centre. Use the distance to the centre with a r_in * 2
            // threshold for the request; placement and hide keep using the real radius (surface distance).
            // A prefab root has no single mesh, so it keeps the old centre-based gate (wide gate + 0.5 m radius
            // would make it spawn/hide every frame between r_out and 2 * r_in).
            const bool is_prefab = bng_is_prefab(o);
            const bool bounds_known = !is_prefab && bng_obj_bounds_known(o);
            const float dc = std::hypot(o.position.x - cx, o.position.y - cy);
            const float ro = bounds_known ? bng_obj_radius(o) : 0.5f;
            const float d = (std::max)(dc - ro, 0.5f);  // surface distance: placement / hide
            const float d_gate = bounds_known ? d : dc; // request gate
            const float r_gate = bounds_known ? r_in : r_in * 2.f;
            const bool vis = R.spawned.count(o.id) != 0;
            if (!vis && d_gate <= r_gate) {
                spawn_editor_object_gpu(o);
            } else if (vis && d > r_out) {
                bng_hide_object(o.id);
            } else if (vis && is_prefab) {
                // LOD refresh of its children, staggered by object id (Root F): the old `stream_cursor & 7`
                // parity refreshed whole waves at once, which is what made prefabs flicker in groups.
                if ((static_cast<std::uint32_t>(o.id) % 8u) == (g_editor_frame_id % 8u)) bng_place_prefab(o);
            } else if (vis) {
                // LOD update (3D distance to the camera eye); the shape comes from the per-object cache, so no
                // std::string is rebuilt and the shape map is not rehashed for every object every frame.
                const BngShapeGpu* g = bng_obj_shape_gpu(o);
                if (!g || !g->ready() || g->protos.size() < 2) continue;
                auto& inst = R.chunk_of[o.id];
                const float ex = o.position.x - cam_eye[0], ey = o.position.y - cam_eye[1], ez = o.position.z - cam_eye[2];
                const float smax = bng_obj_scale_max(o.id);
                const int want = bng_pick_lod(*g, smax, std::sqrt(ex * ex + ey * ey + ez * ez), inst.cur, &inst);
                if (want != inst.cur) bng_place_clone(o, *g, want);
            }
        }
    }

    // ── 3) forest (render only for now) with LOD / impostors ────────────────────────────────────────────────
    if (!lvl.forest.empty()) {
        section_begin();
        const float fr = r_in, fr_out = r_out;
        const std::size_t slice = (std::min)(lvl.forest.size(), std::size_t(8000));
        for (std::size_t k = 0; k < slice && elapsed_ms() < kBudgetForest; ++k) {
            if (R.forest_cursor >= lvl.forest.size()) R.forest_cursor = 0;
            const auto fid = static_cast<std::uint32_t>(R.forest_cursor++);
            const auto& fi = lvl.forest[fid];
            if (fi.deleted || fi.type < 0) continue;
            const float dx = fi.pos[0] - cx, dy = fi.pos[1] - cy;
            const float d2 = dx * dx + dy * dy;
            const bool vis = R.forest_spawned.count(fid) != 0;
            const auto& type = lvl.forest_types[static_cast<std::size_t>(fi.type)];
            if (type.shape_file.empty()) continue;
            auto& g = R.shapes[type.shape_file];
            if (g.failed) continue;
            if (!vis && d2 <= fr * fr) {
                if (g.ready()) {
                    float m16[16];
                    const float sc[3]{fi.scale, fi.scale, fi.scale};
                    bng::mat3_to_world4x4(fi.rot, sc, fi.pos, m16);
                    auto& finst = R.forest_chunk[fid];
                    bng_show_inst(finst, g, bng_pick_lod(g, fi.scale, std::sqrt(d2), finst.cur, &finst), m16);
                } else {
                    R.pool->request(type.shape_file, std::sqrt(d2) + 50.f);
                    R.forest_waiting[type.shape_file].push_back(fid);
                }
                R.forest_spawned.insert(fid); // pending counts as spawned (placed on upload)
            } else if (vis && d2 > fr_out * fr_out) {
                if (auto it = R.forest_chunk.find(fid); it != R.forest_chunk.end()) bng_hide_inst(it->second);
                R.forest_spawned.erase(fid);
            } else if (vis && g.ready() && g.protos.size() > 1) {
                auto it = R.forest_chunk.find(fid);
                if (it == R.forest_chunk.end() || it->second.cur < 0) continue;
                const float ex = fi.pos[0] - cam_eye[0], ey = fi.pos[1] - cam_eye[1], ez = fi.pos[2] - cam_eye[2];
                const int want = bng_pick_lod(g, fi.scale, std::sqrt(ex * ex + ey * ey + ez * ez), it->second.cur, &it->second);
                if (want != it->second.cur) {
                    float m16[16];
                    const float sc[3]{fi.scale, fi.scale, fi.scale};
                    bng::mat3_to_world4x4(fi.rot, sc, fi.pos, m16);
                    bng_show_inst(it->second, g, want, m16);
                }
            }
        }
    }

    // Keep the selection highlight on the LOD chunk that is really drawn for the selected forest instance
    // (Root D: the instance is not an edit_object, its clone changes with the LOD).
    if (R.sel_forest >= 0) {
        const auto fid = static_cast<std::uint32_t>(R.sel_forest);
        if (auto it = R.forest_chunk.find(fid); it != R.forest_chunk.end() && it->second.cur >= 0 &&
                                                static_cast<std::size_t>(it->second.cur) < it->second.lod_chunks.size()) {
            const auto ch = it->second.lod_chunks[static_cast<std::size_t>(it->second.cur)];
            if (ch != static_cast<std::size_t>(-1) && ch != R.sel_forest_chunk) {
                if (R.sel_forest_chunk != static_cast<std::size_t>(-1))
                    renderer.set_chunk_highlight(R.sel_forest_chunk, 0.f);
                renderer.set_chunk_highlight(ch, 0.35f, 0.35f, 0.8f, 0.35f);
                R.sel_forest_chunk = ch;
            }
        }
    }

    // GroundCover selection: keep the tint on the chunk of the selected cell that is really drawn now (the
    // 32 m cell chunk is rebuilt when the density preset changes / the cell scrolls back in).
    if (R.sel_gc >= 0) {
        std::size_t ch = static_cast<std::size_t>(-1);
        if (auto it = R.gc_cells.find(R.sel_gc_key); it != R.gc_cells.end() && it->second.visible)
            ch = it->second.chunk;
        if (ch != R.sel_gc_chunk) {
            if (R.sel_gc_chunk != static_cast<std::size_t>(-1)) renderer.set_chunk_highlight(R.sel_gc_chunk, 0.f);
            if (ch != static_cast<std::size_t>(-1))
                renderer.set_chunk_highlight(ch, 0.35f, 0.9f, 0.85f, 0.35f);
            R.sel_gc_chunk = ch;
        }
    }

    // ── 3b) roads / rivers / water: geometry on workers, upload + draw-phase here ─────────────────────────────
    if (!R.ribbons.empty()) {
        section_begin();
        const int max_jobs = (std::max)(2, static_cast<int>(std::thread::hardware_concurrency()) / 2);
        // Builds are launched in renderPriority order (ascending) inside every batch of jobs.
        if (R.ribbon_order.size() != R.ribbons.size()) {
            R.ribbon_order.resize(R.ribbons.size());
            for (std::size_t i = 0; i < R.ribbon_order.size(); ++i) R.ribbon_order[i] = static_cast<int>(i);
        }
        const std::size_t slice = (std::min)(R.ribbon_order.size(), std::size_t(3000));
        for (std::size_t k = 0; k < slice && elapsed_ms() < kBudgetRibbons; ++k) {
            if (R.ribbon_cursor >= R.ribbon_order.size()) R.ribbon_cursor = 0;
            const int ri = R.ribbon_order[R.ribbon_cursor++];
            auto& rb = R.ribbons[static_cast<std::size_t>(ri)];
            if (rb.failed || rb.no_draw) continue; // AI lanes / transparent materials: nothing to stream
            const float d = (std::max)(0.f, std::hypot(rb.center[0] - cx, rb.center[1] - cy) - rb.radius);
            // distanceFade [start, range]: the road is built/kept only up to start+range (no point building
            // what is already fully faded); inside that range the alpha fades instead of a hard cut. Without
            // the field it stays visible up to the load radius, as before.
            float d_keep = r_in, d_hide = r_out;
            if (rb.fade_range > 0.f) {
                const float fade_end = rb.fade_start + rb.fade_range;
                d_keep = (std::min)(d_keep, fade_end);
                d_hide = (std::min)(d_hide, fade_end);
            }
            if (d <= d_keep) {
                if (rb.chunk != static_cast<std::size_t>(-1)) {
                    if (!rb.visible) {
                        renderer.set_chunk_visible(rb.chunk, true);
                        rb.visible = true;
                    }
                    // BeamNG DecalRoad.distanceFade [start, range]: alpha fades out instead of vanishing
                    // on a hard cut. The per-chunk opacity is applied in the pixel shader (gOpacity), so a
                    // mask/tint decal road fades without touching mask_tint.
                    if (rb.fade_range > 0.f) {
                        const float t = (std::clamp)((d - rb.fade_start) / rb.fade_range, 0.f, 1.f);
                        renderer.set_chunk_opacity(rb.chunk, 1.f - t);
                    }
                } else if (!rb.building && static_cast<int>(R.ribbon_jobs.size()) < max_jobs) {
                    rb.building = true;
                    const auto& so = lvl.objects[rb.level_index];
                    const bng::TerrainData* tp = lvl.terrain ? &*lvl.terrain : nullptr;
                    BngRuntime::RibbonJob job;
                    job.idx = ri;
                    const int kind = rb.kind;
                    const auto raw = so.raw; // copy: the worker must not touch the level while the editor edits it
                    float pos[3]{so.pos[0], so.pos[1], so.pos[2]}, sc[3]{so.scale[0], so.scale[1], so.scale[2]};
                    const bng::Mat3 rot = so.rot;
                    float plane_half = 4096.f;
                    if (tp) plane_half = static_cast<float>(tp->size) * tp->square_size;
                    job.fut = std::async(std::launch::async, [kind, raw, tp, pos, sc, rot, plane_half]() {
                        if (kind == 0)
                            return bng::build_decal_road(bng::read_road_nodes(raw.get("nodes")),
                                                         static_cast<float>(raw.get("textureLength").as_number_lenient(5.0)),
                                                         raw.get("looped").as_bool(), tp);
                        if (kind == 1)
                            return bng::build_flat_ribbon(bng::read_road_nodes(raw.get("nodes")),
                                                          static_cast<float>(raw.get("textureLength").as_number_lenient(10.0)),
                                                          raw.get("looped").as_bool());
                        if (kind == 2) {
                            const float top[3]{pos[0], pos[1], pos[2]};
                            return bng::build_water_quad(top, rot, 0.5f * sc[0], 0.5f * sc[1], 0.05f);
                        }
                        const float c[3]{pos[0], pos[1], pos[2]};
                        return bng::build_water_quad(c, bng::mat3_identity(), plane_half, plane_half, 0.02f);
                    });
                    R.ribbon_jobs.push_back(std::move(job));
                }
            } else if (d > d_hide && rb.visible && rb.chunk != static_cast<std::size_t>(-1)) {
                renderer.set_chunk_visible(rb.chunk, false);
                rb.visible = false;
            }
        }
        section_begin();
        for (auto it = R.ribbon_jobs.begin(); it != R.ribbon_jobs.end() && elapsed_ms() < kBudgetRibbonJobs;) {
            if (it->fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            auto mesh = it->fut.get();
            const int ridx = it->idx;
            auto& rb = R.ribbons[static_cast<std::size_t>(ridx)];
            it = R.ribbon_jobs.erase(it);
            if (mesh.indices.empty()) {
                rb.building = false;
                rb.failed = true;
                continue;
            }
            if (rb.kind == 0 && (rb.over_objects || rb.max_above_terrain > 1.f)) {
                BngRuntime::DrapeItem di;
                di.idx = ridx;
                di.mesh = std::move(mesh);
                R.drape.push_back(std::move(di));
                continue; // uploaded once draped (still "building")
            }
            bng_upload_ribbon(ridx, mesh);
        }
        // Drape over loaded meshes (wait until nearby models are in, then ~2 ms of ray casts per frame).
        if (!R.drape.empty() && R.pool->pending() == 0) {
            const auto t0 = std::chrono::steady_clock::now();
            while (!R.drape.empty() &&
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < 2.0) {
                auto& di = R.drape.front();
                auto& verts = di.mesh.vertices;
                for (int n = 0; n < 32 && di.cursor < verts.size(); ++n, ++di.cursor) {
                    auto& v = verts[di.cursor];
                    const float z = v.position[2];
                    if (const auto hz = renderer.pick_surface_hit_z(v.position[0], v.position[1], z + 4.f, 0.f, 0.f, -1.f)) {
                        if (*hz > z - 8.f && *hz < z + 4.f) v.position[2] = *hz + 0.04f;
                    }
                }
                if (di.cursor >= verts.size()) {
                    const int ridx = di.idx;
                    auto mesh = std::move(di.mesh);
                    R.drape.pop_front();
                    bng_upload_ribbon(ridx, mesh);
                }
            }
        }
    }
    // ── 3b-bis) decals (main.decals.json): quads per 64 m cell + type, built on workers ────────────────
    if (!R.decals.empty()) {
        section_begin();
        const int max_jobs = (std::max)(2, static_cast<int>(std::thread::hardware_concurrency()) / 2);
        const std::size_t slice = (std::min)(R.decal_order.size(), std::size_t(3000));
        // launched in renderPriority order so the blending inside a batch follows the decal order
        for (std::size_t k = 0; k < slice && elapsed_ms() < kBudgetDecals; ++k) {
            if (R.decal_cursor >= R.decal_order.size()) R.decal_cursor = 0;
            const int di = R.decal_order[R.decal_cursor++];
            auto& dc = R.decals[static_cast<std::size_t>(di)];
            if (dc.failed) continue;
            const float d = (std::max)(0.f, std::hypot(dc.center[0] - cx, dc.center[1] - cy) - dc.radius);
            if (d <= r_in) {
                if (dc.chunk != static_cast<std::size_t>(-1)) {
                    // BeamNG decal fade: alpha *= 1 - clamp((dist - fadeStart) / (fadeEnd - fadeStart)),
                    // defaults fadeStart = 50 m / fadeEnd = 75 m. Distance is measured to the cell AABB
                    // (centre minus radius), so a cell that still has a decal closer than fadeStart never
                    // fades. The chunk is hidden once it is fully faded, like BeamNG deleting the instance
                    // when its fade reaches 0 (kept in memory, re-shown when coming back under fadeEnd).
                    const float span = dc.fade_end - dc.fade_start;
                    const float t = (std::clamp)(span > 0.001f ? (d - dc.fade_start) / span : 1.f, 0.f, 1.f);
                    const float alpha = 1.f - t;
                    if (alpha > 0.001f) {
                        if (!dc.visible) {
                            renderer.set_chunk_visible(dc.chunk, true);
                            dc.visible = true;
                        }
                        renderer.set_chunk_opacity(dc.chunk, alpha);
                    } else if (dc.visible) {
                        renderer.set_chunk_visible(dc.chunk, false);
                        dc.visible = false;
                    }
                } else if (!dc.building && static_cast<int>(R.decal_jobs.size()) < max_jobs) {
                    dc.building = true;
                    if (dc.type < 0 || static_cast<std::size_t>(dc.type) >= lvl.decal_types.size()) {
                        dc.failed = true;
                        continue;
                    }
                    // copy the cell instances and the type rects: the worker must not walk the level lists
                    std::vector<bng::DecalInstance> insts;
                    insts.reserve(dc.count);
                    for (std::uint32_t i = 0; i < dc.count; ++i) insts.push_back(lvl.decals[R.decal_ids[dc.first + i]]);
                    const auto rects = lvl.decal_types[static_cast<std::size_t>(dc.type)].texture_coords;
                    BngRuntime::DecalJob job;
                    job.idx = di;
                    job.fut = std::async(std::launch::async,
                                         [insts, rects]() { return bng::build_decal_quads(insts, rects); });
                    R.decal_jobs.push_back(std::move(job));
                }
            } else if (dc.visible && dc.chunk != static_cast<std::size_t>(-1)) {
                renderer.set_chunk_visible(dc.chunk, false);
                dc.visible = false;
            }
        }
        section_begin();
        for (auto it = R.decal_jobs.begin(); it != R.decal_jobs.end() && elapsed_ms() < kBudgetDecalJobs;) {
            if (it->fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            auto mesh = it->fut.get();
            const int di = it->idx;
            auto& dc = R.decals[static_cast<std::size_t>(di)];
            it = R.decal_jobs.erase(it);
            if (mesh.indices.empty()) {
                dc.building = false;
                dc.failed = true;
                continue;
            }
            bng_upload_decal(di, mesh);
        }
    }
    // ── 3c) GroundCover cells (32 m) around the camera, generated on workers ────────────────────────────────
    if (!R.ground_covers.empty() && lvl.terrain) {
        section_begin();
        constexpr float kCell = kBngGcCell;   // 32 m cells
        constexpr int kGrid = kBngGcGrid;     // 16 subdivisions -> one surface node every 2 m
        constexpr int kGridN = kGrid + 1;
        constexpr float kStep = kCell / static_cast<float>(kGrid); // 2 m
        constexpr float kBlocked = -1e30f;    // no grass on this grid node (road / object / outside)
        const auto& T = *lvl.terrain;
        const int max_jobs = (std::max)(2, static_cast<int>(std::thread::hardware_concurrency()) / 2);
        // Real surface height of one grid node. renderer.pick_surface_hit_z walks the GPU chunks, so it can
        // only run on the MAIN thread (Root E): it is sampled here and handed to the worker as a plain grid.
        auto sample_surface = [&](float x, float y) -> float {
            if (!T.contains(x, y)) return kBlocked;
            if (bng_point_on_road(x, y)) return kBlocked; // roads / rivers: no grass
            const float tz = bng_terrain_tri_z(T, x, y); // same triangulation as the drawn terrain patch
            const float top = tz + 200.f;                 // start above anything standing there
            const auto hz = renderer.pick_surface_hit_z(x, y, top, 0.f, 0.f, -1.f);
            if (!hz) return tz;                           // no mesh under the point: the terrain itself
            if (*hz > tz + 0.6f) return kBlocked;         // an object / kerb / roof covers the node
            if (std::fabs(*hz - tz) > 0.6f) return kBlocked;
            return *hz; // the real surface (terrain patch triangles, decal-free)
        };
        auto key_of = [](std::size_t gi, int x, int y) {
            return (static_cast<std::uint64_t>(gi) << 48) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0xFFFFFF) << 24) |
                   (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0xFFFFFF));
        };
        for (std::size_t gi = 0; gi < R.ground_covers.size() && R.gc_density_scale > 0.f; ++gi) {
            const auto& gc = R.ground_covers[gi];
            const float rad = (std::min)(gc.radius, R.radius);
            const int c0x = static_cast<int>(std::floor((cx - rad) / kCell)), c1x = static_cast<int>(std::floor((cx + rad) / kCell));
            const int c0y = static_cast<int>(std::floor((cy - rad) / kCell)), c1y = static_cast<int>(std::floor((cy + rad) / kCell));
            for (int gy = c0y; gy <= c1y; ++gy)
                for (int gx = c0x; gx <= c1x; ++gx) {
                    const float ccx = (static_cast<float>(gx) + 0.5f) * kCell, ccy = (static_cast<float>(gy) + 0.5f) * kCell;
                    // show / hide bands are the same radius x the same margins as the hide pass below (Root F);
                    // the wider hide margin is the hysteresis that stops the cells from popping in and out.
                    if (std::hypot(ccx - cx, ccy - cy) > rad + kCell * 0.71f) continue;
                    if (!T.contains(ccx, ccy)) continue;
                    auto& cell = R.gc_cells[key_of(gi, gx, gy)];
                    // Quality changes densityScale only to turn the grass off: a built cell is never rebuilt for
                    // a density change (Root F: every chunk was released and regenerated on a preset change).
                    if (cell.chunk != static_cast<std::size_t>(-1)) {
                        if (!cell.visible) {
                            renderer.set_chunk_visible(cell.chunk, true);
                            cell.visible = true;
                        }
                        continue;
                    }
                    if (cell.empty || cell.building || static_cast<int>(R.gc_jobs.size()) >= max_jobs) continue;
                    // (1) 2 m surface grid, main thread, bounded per frame (a whole cell would be a hitch)
                    if (cell.surf.empty()) {
                        cell.surf.assign(static_cast<std::size_t>(kGridN) * kGridN, kBlocked);
                        cell.surf_done = 0;
                    }
                    if (cell.surf_done < cell.surf.size()) {
                        const auto t_surf0 = std::chrono::steady_clock::now();
                        while (cell.surf_done < cell.surf.size() && elapsed_ms() < kBudgetGroundCover &&
                               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_surf0).count() < 1.0) {
                            const std::uint32_t n = cell.surf_done++;
                            const int sx = static_cast<int>(n % kGridN), sy = static_cast<int>(n / kGridN);
                            cell.surf[n] = sample_surface((static_cast<float>(gx) + static_cast<float>(sx) / kGrid) * kCell,
                                                          (static_cast<float>(gy) + static_cast<float>(sy) / kGrid) * kCell);
                        }
                        if (cell.surf_done < cell.surf.size()) continue; // finish next frame
                    }
                    // (2) quads on a worker, from the sampled grid (never the bilinear heightfield)
                    cell.building = true;
                    BngRuntime::GcJob job;
                    job.key = key_of(gi, gx, gy);
                    job.density = gc.density; // base density: the quality preset no longer rebuilds the cell
                    const float dens = job.density;
                    const bng::TerrainData* tp = &T;
                    const BngGroundCover gcc = gc;
                    job.fut = std::async(std::launch::async, [tp, gcc, gx, gy, dens, grid = cell.surf]() {
                        std::vector<o3d::Vertex> v;
                        std::vector<std::uint32_t> idx;
                        const auto& t = *tp;
                        std::mt19937 rng(gcc.seed * 73856093u ^ static_cast<std::uint32_t>(gx) * 19349663u ^ static_cast<std::uint32_t>(gy) * 83492791u);
                        std::uniform_real_distribution<float> u01(0.f, 1.f);
                        // bilinear read of the 2 m surface grid; any blocked corner rejects the sample
                        auto surf_z = [&](float x, float y, bool* blocked) -> float {
                            const float lx = (x - static_cast<float>(gx) * kCell) / kStep;
                            const float ly = (y - static_cast<float>(gy) * kCell) / kStep;
                            const int i0 = (std::clamp)(static_cast<int>(std::floor(lx)), 0, kGrid - 1);
                            const int j0 = (std::clamp)(static_cast<int>(std::floor(ly)), 0, kGrid - 1);
                            const float u = (std::clamp)(lx - static_cast<float>(i0), 0.f, 1.f);
                            const float v = (std::clamp)(ly - static_cast<float>(j0), 0.f, 1.f);
                            const float z00 = grid[static_cast<std::size_t>(j0) * kGridN + i0];
                            const float z10 = grid[static_cast<std::size_t>(j0) * kGridN + i0 + 1];
                            const float z01 = grid[static_cast<std::size_t>(j0 + 1) * kGridN + i0];
                            const float z11 = grid[static_cast<std::size_t>(j0 + 1) * kGridN + i0 + 1];
                            if (z00 < -1e29f || z10 < -1e29f || z01 < -1e29f || z11 < -1e29f) {
                                *blocked = true;
                                return 0.f;
                            }
                            *blocked = false;
                            return (z00 * (1.f - u) + z10 * u) * (1.f - v) + (z01 * (1.f - u) + z11 * u) * v;
                        };
                        const int count = static_cast<int>(dens * kCell * kCell);
                        float psum = 0.f;
                        for (const auto& ty : gcc.types) psum += ty.probability;
                        if (!(psum > 0.f)) return std::make_pair(std::move(v), std::move(idx));
                        for (int i = 0; i < count; ++i) {
                            const float x = (static_cast<float>(gx) + u01(rng)) * kCell, y = (static_cast<float>(gy) + u01(rng)) * kCell;
                            if (!t.contains(x, y)) continue;
                            const auto sx = static_cast<std::uint32_t>((std::clamp)((x - t.origin[0]) / t.square_size, 0.f, static_cast<float>(t.size - 1)));
                            const auto sy = static_cast<std::uint32_t>((std::clamp)((y - t.origin[1]) / t.square_size, 0.f, static_cast<float>(t.size - 1)));
                            const int layer = t.layers[static_cast<std::size_t>(sy) * t.size + sx];
                            if (layer == 255) continue;
                            float pick = u01(rng) * psum;
                            const BngGroundCoverType* ty = nullptr;
                            for (const auto& c : gcc.types) {
                                pick -= c.probability;
                                if (pick <= 0.f) {
                                    ty = &c;
                                    break;
                                }
                            }
                            if (!ty || (ty->layer >= 0 && ty->layer != layer)) continue;
                            bool blocked = false;
                            const float z = surf_z(x, y, &blocked);
                            if (blocked) continue;
                            if (ty->max_slope_deg < 89.f) {
                                // slope from the same 2 m surface grid (the heightfield gave a different one)
                                bool bs0 = false, bs1 = false, bs2 = false, bs3 = false;
                                const float zxm = surf_z(x - kStep, y, &bs0), zxp = surf_z(x + kStep, y, &bs1);
                                const float zym = surf_z(x, y - kStep, &bs2), zyp = surf_z(x, y + kStep, &bs3);
                                if (!bs0 && !bs1 && !bs2 && !bs3) {
                                    const float gxs = (zxp - zxm) / (2.f * kStep);
                                    const float gys = (zyp - zym) / (2.f * kStep);
                                    if (std::atan(std::hypot(gxs, gys)) * 57.2958f > ty->max_slope_deg) continue;
                                }
                            }
                            const float sz = ty->size_min + (ty->size_max - ty->size_min) * std::pow(u01(rng), (std::max)(0.1f, ty->size_exp));
                            const float ang = u01(rng) * 3.14159265f;
                            // two crossed quads (bottom on the ground), atlas UVs
                            for (int q = 0; q < 2; ++q) {
                                const float a = ang + static_cast<float>(q) * 1.5707963f;
                                const float dx = std::cos(a) * sz * 0.5f, dy = std::sin(a) * sz * 0.5f;
                                const auto base = static_cast<std::uint32_t>(v.size());
                                const float u0 = ty->uv[0], v0 = ty->uv[1], u1 = ty->uv[0] + ty->uv[2], v1 = ty->uv[1] + ty->uv[3];
                                const float px[4] = {x - dx, x + dx, x + dx, x - dx}, py[4] = {y - dy, y + dy, y + dy, y - dy};
                                const float pz[4] = {z, z, z + sz, z + sz};
                                const float uu[4] = {u0, u1, u1, u0}, vv[4] = {v1, v1, v0, v0};
                                for (int k = 0; k < 4; ++k) {
                                    o3d::Vertex vx{};
                                    vx.position[0] = px[k];
                                    vx.position[1] = py[k];
                                    vx.position[2] = pz[k] + gcc.z_offset;
                                    vx.normal[2] = 1.f;
                                    vx.uv[0] = uu[k];
                                    vx.uv[1] = vv[k];
                                    v.push_back(vx);
                                }
                                idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                            }
                        }
                        return std::make_pair(std::move(v), std::move(idx));
                    });
                    R.gc_jobs.push_back(std::move(job));
                    cell.surf.clear(); // the worker owns a copy now
                    cell.surf_done = 0;
                }
        }
        section_begin();
        for (auto it = R.gc_jobs.begin(); it != R.gc_jobs.end() && elapsed_ms() < kBudgetGcJobs;) {
            if (it->fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            auto data = it->fut.get();
            auto& cell = R.gc_cells[it->key];
            const auto gi = static_cast<std::size_t>(it->key >> 48);
            cell.building = false;
            cell.density_built = it->density;
            if (cell.chunk != static_cast<std::size_t>(-1)) {
                renderer.set_chunk_visible(cell.chunk, false); // replaced (density changed)
                cell.chunk = static_cast<std::size_t>(-1);
            }
            cell.empty = data.second.empty();
            if (!cell.empty && gi < R.ground_covers.size()) {
                gfx::dx12::ProcMeshSubrange sub;
                sub.index_count = static_cast<std::uint32_t>(data.second.size());
                sub.texture_path = std::filesystem::u8path(R.ground_covers[gi].texture);
                sub.alpha_mode = 1; // cutout
                // crossed quads: drawn from both sides (CULL_NONE when the subrange carries the flag)
                const float gc_white[4]{1.f, 1.f, 1.f, 1.f};
                bng_sub_material(sub, gc_white, 1.f, 127, true, std::filesystem::path());
                if (renderer.append_textured_mesh_merged(data.first.data(), static_cast<std::uint32_t>(data.first.size()),
                                                          data.second.data(), static_cast<std::uint32_t>(data.second.size()), {sub})) {
                    cell.chunk = renderer.chunk_count() - 1;
                    cell.visible = true;
                    // BeamNG grass: never size-culled (a sparse 32 m cell would vanish at mid distance).
                    renderer.set_chunk_no_size_cull(cell.chunk, true);
                }
            }
            it = R.gc_jobs.erase(it);
        }
        // hide cells outside their radius (cheap pass over the map)
        for (auto& [k, cell] : R.gc_cells) {
            if (!cell.visible || cell.chunk == static_cast<std::size_t>(-1)) continue;
            const auto gi = static_cast<std::size_t>(k >> 48);
            const int gx = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>((k >> 24) & 0xFFFFFF) << 8) >> 8);
            const int gy = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(k & 0xFFFFFF) << 8) >> 8);
            const float rad = gi < R.ground_covers.size() ? (std::min)(R.ground_covers[gi].radius, R.radius) : 0.f; // same radius as the placement pass
            const float ccx = (static_cast<float>(gx) + 0.5f) * kCell, ccy = (static_cast<float>(gy) + 0.5f) * kCell;
            // wider margin than the placement band (0.71) on purpose: hysteresis, so a cell on the edge is not
            // hidden and regenerated every few frames when the camera jitters (Root F pop-in waves)
            if (std::hypot(ccx - cx, ccy - cy) > rad + kCell * 1.2f || R.gc_density_scale <= 0.f) {
                renderer.set_chunk_visible(cell.chunk, false);
                cell.visible = false;
            }
        }
    }

    // ── 3d) click pick of roads / skid marks: DecalRoad ribbons are chunks too ─────────────────────────
    // Only BeamNG (this module runs inside `if (bng_rt)`). The OMSI click handling is untouched: this
    // reacts to the left-button release edge, and only when nothing was selected under the cursor.
    {
        static bool lmb_was_bng = false;        // module-local edge state (like the other static flags here)
        static bool lmb_press_over_ui = false;
        static double lmb_px = 0.0, lmb_py = 0.0; // press position (window coords) to reject drags
        const bool lmb_now = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        const bool rmb_now = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        const bool alt_now = glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                             glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
        double cx_win = 0.0, cy_win = 0.0;
        glfwGetCursorPos(window, &cx_win, &cy_win);
        if (lmb_now && !lmb_was_bng) {
            lmb_press_over_ui = over_ui; // pressed on the panel/overlay: never a 3D pick
            lmb_px = cx_win;
            lmb_py = cy_win;
        } else if (!lmb_now && lmb_was_bng && !lmb_press_over_ui && !over_ui && edit_mode == EditMode::None &&
                   !bus_drive_active && !rmb_now && !alt_now &&
                   std::hypot(cx_win - lmb_px, cy_win - lmb_py) < 6.0) {
            float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
            input::cursor_to_framebuffer(window, mx, my, fw, fh);
            const float aspect = (fw > 0.f && fh > 0.f) ? fw / fh : 16.f / 9.f;
            float wvp[16]{};
            camera.build_world_view_proj(aspect, wvp);
            // An object under the cursor always wins: the ribbon pick ignores occlusion by meshes.
            const int obj_hit = pick_object_index(mx, my, fw, fh, wvp);
            if (obj_hit < 0 && bng_forest_pick_won) {
                // BeamNG forest instance (Root D): not an edit_object, so it is selected on Level::forest.
                if (bng_select_groundcover_hook) bng_select_groundcover_hook(-1, 0);
                if (bng_select_forest_hook) bng_select_forest_hook(static_cast<long long>(bng_forest_pick_fid));
            } else if (obj_hit < 0 && bng_gc_pick_won) {
                // GroundCover (grass): aggregated instance = cover type + its 32 m cell.
                if (bng_select_forest_hook) bng_select_forest_hook(-1);
                if (bng_select_groundcover_hook) bng_select_groundcover_hook(bng_gc_pick_gi, bng_gc_pick_key);
            } else if (obj_hit < 0) {
                static std::vector<std::size_t> rb_cand; // reused: no allocation per click
                rb_cand.clear();
                for (const auto& rb : R.ribbons)
                    if (rb.visible && !rb.failed && rb.kind <= 1 && rb.chunk != static_cast<std::size_t>(-1))
                        rb_cand.push_back(rb.chunk);
                if (!rb_cand.empty()) {
                    // DecalRoad ribbons are thin: retry on a ±6 px ring (minimum click width) and keep the
                    // nearest hit along the ray, otherwise roads / skid marks never select.
                    float rb_t = 1e30f;
                    auto rb_hit = renderer.pick_chunk(mx, my, static_cast<std::uint32_t>(fw),
                                                      static_cast<std::uint32_t>(fh), wvp, false, rb_cand.data(),
                                                      rb_cand.size(), &rb_t);
                    if (!rb_hit) {
                        static constexpr float kRibRing[] = {0.f, 3.f, -3.f, 6.f, -6.f};
                        for (const float ox : kRibRing)
                            for (const float oy : kRibRing) {
                                if (ox == 0.f && oy == 0.f) continue;
                                float t = 1e30f;
                                if (const auto h = renderer.pick_chunk(
                                        mx + ox, my + oy, static_cast<std::uint32_t>(fw),
                                        static_cast<std::uint32_t>(fh), wvp, false, rb_cand.data(),
                                        rb_cand.size(), &t)) {
                                    if (!rb_hit || t < rb_t) {
                                        rb_hit = h;
                                        rb_t = t;
                                    }
                                }
                            }
                    }
                    if (rb_hit) {
                        const auto hit = rb_hit;
                        int found = -1;
                        for (std::size_t i = 0; i < R.ribbons.size(); ++i)
                            if (R.ribbons[i].chunk == *hit) {
                                found = static_cast<int>(i);
                                break;
                            }
                        if (found >= 0) {
                            if (bng_select_forest_hook) bng_select_forest_hook(-1); // a road wins over the tree
                            if (bng_select_groundcover_hook) bng_select_groundcover_hook(-1, 0);
                            if (R.sel_ribbon >= 0 && R.sel_ribbon < static_cast<int>(R.ribbons.size()) &&
                                R.sel_ribbon != found) {
                                const auto old = R.ribbons[static_cast<std::size_t>(R.sel_ribbon)].chunk;
                                if (old != static_cast<std::size_t>(-1)) renderer.set_chunk_highlight(old, 0.f);
                            }
                            R.sel_ribbon = found;
                            R.sel_node = -1;
                            panel.imgui_state().bng_roads_tab_selected = true; // open the Carreteras tab (one shot)
                            const auto& sel_rb = R.ribbons[static_cast<std::size_t>(found)];
                            const auto& so = lvl.objects[sel_rb.level_index];
                            panel.set_status("BeamNG: carretera seleccionada " +
                                             (so.name.empty() ? sel_rb.material : so.name));
                        }
                    }
                }
            }
            if (obj_hit >= 0) {
                // An object was picked: drop the tree / grass selections so only one thing stays highlighted.
                if (bng_select_forest_hook) bng_select_forest_hook(-1);
                if (bng_select_groundcover_hook) bng_select_groundcover_hook(-1, 0);
            }
        }
        lmb_was_bng = lmb_now;
    }

    // node handles of the road selected in the Carreteras tab
    if (R.roads_tab_open && R.sel_ribbon >= 0 && R.sel_ribbon < static_cast<int>(R.ribbons.size())) {
        const auto& so = lvl.objects[R.ribbons[static_cast<std::size_t>(R.sel_ribbon)].level_index];
        float mx = 0.f, my = 0.f, fw = 1.f, fh = 1.f;
        input::cursor_to_framebuffer(window, mx, my, fw, fh);
        float wvp[16]{};
        camera.build_world_view_proj(fh > 0.f ? fw / fh : 16.f / 9.f, wvp);
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float kx = disp.x / (std::max)(1.f, fw), ky = disp.y / (std::max)(1.f, fh);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        ImVec2 prev{};
        bool have = false;
        int i = 0;
        for (const auto& n : so.raw.get("nodes").items()) {
            if (!n.is_array() || n.size() < 3) continue;
            float sx = 0.f, sy = 0.f;
            if (world_to_screen(n[0].as_float(), n[1].as_float(), n[2].as_float() + 0.2f, 0.f, 0.f, fw, fh, wvp, sx, sy)) {
                const ImVec2 p(sx * kx, sy * ky);
                if (have) fg->AddLine(prev, p, IM_COL32(120, 200, 255, 200), 2.f);
                const bool sel = i == R.sel_node;
                fg->AddCircleFilled(p, sel ? 7.f : 5.f, sel ? IM_COL32(255, 80, 60, 255) : IM_COL32(255, 230, 90, 240));
                fg->AddCircle(p, sel ? 7.f : 5.f, IM_COL32(0, 0, 0, 200), 12, 1.5f);
                prev = p;
                have = true;
            } else {
                have = false;
            }
            ++i;
        }
    }
    R.roads_tab_open = false; // set again by the tab while it is drawn

    // ── 4) terrain LOD patches (CPU on workers, upload here) ─────────────────────────────────────────────────
    if (lvl.terrain && !R.patches.empty()) {
        const auto& T = *lvl.terrain;
        const float patch_m = static_cast<float>(R.patch_samples) * T.square_size;
        // re-evaluate LOD when the camera moved a quarter patch
        if (std::hypot(cx - R.lod_cx, cy - R.lod_cy) > patch_m * 0.25f) {
            R.lod_cx = cx;
            R.lod_cy = cy;
            for (auto& p : R.patches) {
                const float pcx = T.origin[0] + (static_cast<float>(p.px) + 0.5f) * patch_m;
                const float pcy = T.origin[1] + (static_cast<float>(p.py) + 0.5f) * patch_m;
                const float d = (std::max)(0.f, std::hypot(pcx - cx, pcy - cy) - patch_m * 0.71f);
                // Terrain::lodScale would scale these bands (Mesh quality preset)
                const float ls = (std::max)(0.25f, R.lod_scale); // Terrain::lodScale: bigger = coarser sooner
                p.want_stride = d < 200.f / ls ? 1 : d < 600.f / ls ? 2 : d < 1500.f / ls ? 4 : d < 3000.f / ls ? 8 : 16;
            }
        }
        // launch up to N jobs
        const int max_jobs = (std::max)(2, static_cast<int>(std::thread::hardware_concurrency()) / 2);
        for (std::size_t pi = 0; pi < R.patches.size() && static_cast<int>(R.patch_jobs.size()) < max_jobs; ++pi) {
            auto& p = R.patches[pi];
            if (p.building || p.want_stride == p.stride || p.want_stride == 0) continue;
            p.building = true;
            BngRuntime::PatchJob job;
            job.patch = static_cast<int>(pi);
            job.stride = p.want_stride;
            const int ps = R.patch_samples;
            const bng::TerrainData* tp = &T;
            const int px = p.px, py = p.py, stride = p.want_stride;
            job.fut = std::async(std::launch::async, [tp, ps, px, py, stride]() {
                const auto& t = *tp;
                const int n = static_cast<int>(t.size);
                const int x0 = px * ps, y0 = py * ps;
                const int x1 = (std::min)(x0 + ps, n - 1), y1 = (std::min)(y0 + ps, n - 1);
                std::vector<int> xs, ys;
                for (int x = x0; x < x1; x += stride) xs.push_back(x);
                xs.push_back(x1);
                for (int y = y0; y < y1; y += stride) ys.push_back(y);
                ys.push_back(y1);
                const float ext = static_cast<float>(t.size) * t.square_size;
                std::vector<o3d::Vertex> v;
                v.reserve(xs.size() * ys.size());
                auto h = [&](int x, int y) {
                    x = (std::clamp)(x, 0, n - 1);
                    y = (std::clamp)(y, 0, n - 1);
                    return t.height_at_sample(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                };
                for (const int y : ys)
                    for (const int x : xs) {
                        o3d::Vertex q{};
                        q.position[0] = t.origin[0] + static_cast<float>(x) * t.square_size;
                        q.position[1] = t.origin[1] + static_cast<float>(y) * t.square_size;
                        q.position[2] = h(x, y);
                        const float gx = (h(x + 1, y) - h(x - 1, y)) / (2.f * t.square_size);
                        const float gy = (h(x, y + 1) - h(x, y - 1)) / (2.f * t.square_size);
                        const float inv = 1.f / std::sqrt(gx * gx + gy * gy + 1.f);
                        q.normal[0] = -gx * inv;
                        q.normal[1] = -gy * inv;
                        q.normal[2] = inv;
                        q.uv[0] = (q.position[0] - t.origin[0]) / ext;
                        q.uv[1] = 1.f - (q.position[1] - t.origin[1]) / ext;
                        v.push_back(q);
                    }
                // triangles bucketed by the terrain layer of their corner sample (holes = 255 stay open)
                std::vector<std::vector<std::uint32_t>> by_layer(256);
                const auto W = static_cast<std::uint32_t>(xs.size());
                for (std::uint32_t j = 0; j + 1 < ys.size(); ++j)
                    for (std::uint32_t i = 0; i + 1 < W; ++i) {
                        const int sx = xs[i], sy = ys[j];
                        const std::uint8_t layer = t.layers[static_cast<std::size_t>(sy) * t.size + static_cast<std::size_t>(sx)];
                        if (layer == 255) continue;
                        const std::uint32_t a = j * W + i, b = a + 1, c = a + W, d = c + 1;
                        by_layer[layer].insert(by_layer[layer].end(), {a, b, c, b, d, c}); // counter-clockwise from above
                    }
                BngRuntime::PatchMesh pm;
                pm.vertices = std::move(v);
                for (int l = 0; l < 256; ++l) {
                    if (by_layer[static_cast<std::size_t>(l)].empty()) continue;
                    pm.layer_ranges.emplace_back(l, static_cast<std::uint32_t>(by_layer[static_cast<std::size_t>(l)].size()));
                    pm.indices.insert(pm.indices.end(), by_layer[static_cast<std::size_t>(l)].begin(), by_layer[static_cast<std::size_t>(l)].end());
                }
                return pm;
            });
            R.patch_jobs.push_back(std::move(job));
        }
        // collect finished jobs (budget)
        section_begin();
        for (auto it = R.patch_jobs.begin(); it != R.patch_jobs.end() && elapsed_ms() < kBudgetPatches;) {
            if (it->fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            auto data = it->fut.get();
            auto& p = R.patches[static_cast<std::size_t>(it->patch)];
            p.building = false;
            p.stride = it->stride;
            if (!data.indices.empty()) {
                std::vector<gfx::dx12::ProcMeshSubrange> subs;
                std::uint32_t start = 0;
                for (const auto& [layer, count] : data.layer_ranges) {
                    gfx::dx12::ProcMeshSubrange sub;
                    sub.index_start = start;
                    sub.index_count = count;
                    start += count;
                    if (!R.terrain_texture.empty()) sub.texture_path = std::filesystem::u8path(R.terrain_texture);
                    if (layer >= 0 && static_cast<std::size_t>(layer) < R.terrain_layers.size()) {
                        const auto& lt = R.terrain_layers[static_cast<std::size_t>(layer)];
                        if (!lt.detail.empty()) sub.detail_path = std::filesystem::u8path(lt.detail);
                        if (!lt.detail_normal.empty()) sub.detail_normal_path = std::filesystem::u8path(lt.detail_normal);
                        sub.detail_scale = lt.tiles;
                    }
                    subs.push_back(std::move(sub));
                }
                if (p.chunk != static_cast<std::size_t>(-1)) {
                    renderer.set_chunk_visible(p.chunk, false);
                    renderer.release_chunk_gpu(p.chunk);
                    p.chunk = static_cast<std::size_t>(-1);
                }
                if (renderer.append_textured_mesh_merged(data.vertices.data(), static_cast<std::uint32_t>(data.vertices.size()),
                                                          data.indices.data(), static_cast<std::uint32_t>(data.indices.size()), subs)) {
                    p.chunk = renderer.chunk_count() - 1;
                    // BeamNG terrain patch: never cut by the render-distance cull (horizon stays closed).
                    renderer.set_chunk_terrain(p.chunk, true);
                }
            }
            it = R.patch_jobs.erase(it);
        }
    }

    // status line (once a second)
    {
        const double now = ImGui::GetTime();
        if (now - R.last_stream_t > 1.0) {
            R.last_stream_t = now;
            const std::size_t pend = R.pool->pending();
            if (pend > 0 || !R.patch_jobs.empty())
                panel.set_status("BeamNG: cargando " + std::to_string(pend) + " modelos, " + std::to_string(R.patch_jobs.size()) +
                                 " parches de terreno... (" + std::to_string(R.spawned.size()) + " objetos visibles)");
        }
    }
    // Terrain brush: input + hover ring + async rebuild of the touched patches (mod_beamng_terrain.inc.cpp).
#define BNG_TERRAIN_SECTION_FRAME 1
#include "modules/mod_beamng_terrain.inc.cpp"
    // Vegetation: pick status under the cursor + selection / paint / erase / grass brush input.
#define BNG_VEGETATION_SECTION_FRAME 1
#include "modules/mod_beamng_vegetation.inc.cpp"
}
