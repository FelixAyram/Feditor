// OMSI 2 tile tab parity: centerkachel @ TMap+0x144, GetKachelIndexWithCoord, CreateNewTile, SetNewCenterkachel.
// Included from map_editor_run_setup_06.inc.cpp (after find_tile_path).

    auto tile_has_terrain_gpu = [&](std::int32_t gx, std::int32_t gy) -> bool {
        for (const auto& tc : terrain_chunks) {
            if (tc.alive && tc.tile_gx == gx && tc.tile_gy == gy) return true;
        }
        return false;
    };

    auto centerkachel_as_active = [&]() -> map::ActiveTileAtWorld {
        map::ActiveTileAtWorld out{};
        if (!centerkachel.valid) return out;
        const auto& layout = world.tile_layout();
        const auto rect = map::grid_tile_rect_at(cfg.global, layout, centerkachel.gx, centerkachel.gy);
        if (!rect.resolved) return out;
        out.grid_x = centerkachel.gx;
        out.grid_y = centerkachel.gy;
        out.size_meters = rect.width_m;
        out.origin_x = rect.origin_x;
        out.origin_y = rect.origin_y;
        out.latitude_deg = map::latitude_deg_from_grid_y(centerkachel.gy);
        out.in_map = map::get_kachel_index_with_coord(cfg.global, centerkachel.gx, centerkachel.gy) >= 0;
        out.resolved = true;
        return out;
    };

    auto sync_centerkachel_from_camera = [&]() {
        // OMSI: centerkachel is integer grid; world = (grid - min)*300 + local.
        // cam_wx/wy are world-space (tile_ox is world origin of session spawn tile).
        const float cam_wx = tile_ox + camera.target_x;
        const float cam_wy = tile_oy + camera.target_y;
        const auto hit = map::active_tile_at_world(cfg.global, world.tile_layout(), cam_wx, cam_wy);
        map::CenterKachel next{};
        if (hit.resolved) {
            next.gx = hit.grid_x;
            next.gy = hit.grid_y;
            next.valid = true;
        }
        if (!centerkachel.valid || centerkachel.gx != next.gx || centerkachel.gy != next.gy) {
            log::info("centerkachel " + std::to_string(centerkachel.gx) + "," +
                      std::to_string(centerkachel.gy) + " -> " + std::to_string(next.gx) + "," +
                      std::to_string(next.gy) + " cam_ed=(" + std::to_string(camera.target_x) + "," +
                      std::to_string(camera.target_y) + ") world=(" + std::to_string(cam_wx) + "," +
                      std::to_string(cam_wy) + ") origin=(" + std::to_string(hit.origin_x) + "," +
                      std::to_string(hit.origin_y) + ") size=" + std::to_string(hit.size_meters) +
                      " in_map=" + std::to_string(hit.in_map ? 1 : 0));
        }
        centerkachel = next;
    };

    auto center_camera_on_centerkachel = [&]() {
        if (!centerkachel.valid) return;
        const auto& layout = world.tile_layout();
        const auto rect =
            map::grid_tile_rect_at(cfg.global, layout, centerkachel.gx, centerkachel.gy);
        if (!rect.resolved) return;
        const float cx = rect.origin_x + rect.width_m * 0.5f;
        const float cy = rect.origin_y + rect.height_m * 0.5f;
        camera.target_x = cx - tile_ox;
        camera.target_y = cy - tile_oy;
        camera.target_z = sample_terrain_editor_xy(camera.target_x, camera.target_y);
    };

    // Upload already-stamped RAM heights + hole mask (no disk reload / no restamp).
    [[maybe_unused]] auto upload_terrain_gpu_stamped = [&](std::int32_t gx, std::int32_t gy) -> bool {
        const TerrainHeightField* field = nullptr;
        for (const auto& tf : terrain_fields) {
            if (tf.grid_x == gx && tf.grid_y == gy) {
                field = &tf;
                break;
            }
        }
        if (!field || field->heights.empty()) return false;
        auto map_path = find_tile_path(gx, gy);
        if (map_path.empty()) return false;
        if (cfg.workspace) map_path = map::workspace_tile_map_for_read(&*cfg.workspace, map_path);
        const float size_m = field->size_m > 0.01f ? field->size_m : map::kStandardTileMeters;
        const float rx = field->rel_x;
        const float ry = field->rel_y;
        map::GroundTex tex = groundtex;
        if (geo_settings.tile_aerial_visible) {
            if (const auto aerial = map::find_tile_aerial_jpg(cfg.omsi_root, cfg.global.root, map_path))
                tex.diffuse = *aerial;
        }
        const auto terr = editor::build_local_terrain_tile(map_path, tex, cfg.omsi_root, size_m,
                                                           cfg.global.ground_textures);
        if (!terr) return false;
        auto mesh = terr->mesh;
        editor::apply_heightfield_to_terrain_mesh(mesh, field->heights, size_m);
        terrain_detail_base[tile_key(gx, gy)] = std::make_shared<const gfx::BufferMesh>(mesh);
        // A height/stamp refresh changes only terrain vertex/index data; the ground texture
        // is unchanged. Invalidating it here forced redundant texture re-uploads while streaming.
        for (auto& tc : terrain_chunks) {
            if (tc.tile_gx != gx || tc.tile_gy != gy) continue;
            if (!tc.alive) tc.alive = true;
            if (!renderer.replace_textured_mesh(
                    tc.chunk, mesh.vertices.data(), static_cast<std::uint32_t>(mesh.vertices.size()),
                    mesh.indices.data(), static_cast<std::uint32_t>(mesh.indices.size()),
                    terr->texture_path, 0u))
                return false;
            tc.detail_state = 0; // heights/stamp changed: rebuild the cut detail on demand
            ++tc.detail_gen;
            renderer.clear_chunk_detail(tc.chunk);
            const auto wt = store_world_transposed(XMMatrixTranslation(rx, ry, 0.f));
            renderer.set_chunk_world_matrix(tc.chunk, wt.data());
            renderer.set_chunk_visible(tc.chunk, true);
            return true;
        }
        if (!renderer.append_textured_mesh(mesh.vertices.data(),
                                           static_cast<std::uint32_t>(mesh.vertices.size()),
                                           mesh.indices.data(),
                                           static_cast<std::uint32_t>(mesh.indices.size()), terr->texture_path,
                                           0u))
            return false;
        const std::size_t chunk = renderer.chunk_count() - 1;
        const auto wt = store_world_transposed(XMMatrixTranslation(rx, ry, 0.f));
        renderer.set_chunk_world_matrix(chunk, wt.data());
        register_terrain_chunk(chunk, gx, gy);
        return true;
    };

    [[maybe_unused]] auto terrain_tex_key = [&](const map::GroundTex& tex, float size_m) {
        return tex.diffuse.generic_string() + "|" + std::to_string(size_m);
    };

    // Main-thread half of a terrain reload: the worker already assembled the final mesh, so this is
    // only the GPU buffer copy + transform (no disk I/O, no ground-paint bake, no polycut tessellation).
    auto upload_terrain_payload = [&](std::int32_t gx, std::int32_t gy, float rx, float ry,
                                      const TerrainMeshPayload& pl) -> bool {
        if (!pl.ok) return false;
        const auto& mesh = pl.mesh;
        terrain_detail_base[tile_key(gx, gy)] = std::make_shared<const gfx::BufferMesh>(mesh);
        for (auto& tc : terrain_chunks) {
            if (tc.tile_gx != gx || tc.tile_gy != gy) continue;
            if (!tc.alive) tc.alive = true;
            if (!renderer.replace_textured_mesh(
                    tc.chunk, mesh.vertices.data(), static_cast<std::uint32_t>(mesh.vertices.size()),
                    mesh.indices.data(), static_cast<std::uint32_t>(mesh.indices.size()), pl.texture_path, 0u))
                return false;
            tc.detail_state = 0;
            ++tc.detail_gen;
            renderer.clear_chunk_detail(tc.chunk);
            const auto wt = store_world_transposed(XMMatrixTranslation(rx, ry, 0.f));
            renderer.set_chunk_world_matrix(tc.chunk, wt.data());
            renderer.set_chunk_visible(tc.chunk, true);
            return true;
        }
        if (!renderer.append_textured_mesh(mesh.vertices.data(), static_cast<std::uint32_t>(mesh.vertices.size()),
                                           mesh.indices.data(), static_cast<std::uint32_t>(mesh.indices.size()),
                                           pl.texture_path, 0u))
            return false;
        const std::size_t chunk = renderer.chunk_count() - 1;
        const auto wt = store_world_transposed(XMMatrixTranslation(rx, ry, 0.f));
        renderer.set_chunk_world_matrix(chunk, wt.data());
        register_terrain_chunk(chunk, gx, gy);
        return true;
    };

    // Start a background reload batch. needs_restamp=true: workers restamp from heights_base and then
    // build the mesh; false: the RAM field is already stamped, workers only build the mesh.
    auto terrain_reload_start = [&](const std::vector<std::pair<std::int32_t, std::int32_t>>& tiles,
                                    bool needs_restamp) {
        terrain_reload_ready.clear();
        terrain_reload_queue.cancel_all();
        terrain_reload_restamp = needs_restamp;
        terrain_reload_total = 0;
        std::vector<editor::TerrainRestampTileIn> batch;
        batch.reserve(tiles.size());
        for (const auto& [gx, gy] : tiles) {
            TerrainHeightField* field = nullptr;
            for (auto& tf : terrain_fields) {
                if (tf.grid_x == gx && tf.grid_y == gy) {
                    field = &tf;
                    break;
                }
            }
            if (!field || field->heights.empty()) continue;
            auto map_path = find_tile_path(gx, gy);
            if (map_path.empty()) continue;
            if (cfg.workspace) map_path = map::workspace_tile_map_for_read(&*cfg.workspace, map_path);
            auto user = std::make_shared<TerrainTileUser>();
            user->size_m = field->size_m > 0.01f ? field->size_m : map::kStandardTileMeters;
            user->tex = groundtex;
            if (geo_settings.tile_aerial_visible) {
                if (const auto aerial = map::find_tile_aerial_jpg(cfg.omsi_root, cfg.global.root, map_path))
                    user->tex.diffuse = *aerial;
            }
            user->map_path = std::move(map_path);
            user->omsi_root = cfg.omsi_root;
            user->ground_textures = cfg.global.ground_textures;
            user->tex_key = terrain_tex_key(user->tex, user->size_m);
            editor::TerrainRestampTileIn in;
            in.key = tile_key(gx, gy);
            in.token = field->restamp_token = ++terrain_token_seq;
            in.rel_x = field->rel_x;
            in.rel_y = field->rel_y;
            in.size_m = field->size_m;
            if (needs_restamp) {
                in.heights_base = field->heights_base;
            } else {
                auto pre = std::make_shared<editor::TerrainRestampResult>();
                pre->heights = field->heights;
                pre->hole_mask = field->hole_mask;
                pre->polycut_rings = field->polycut_rings;
                pre->has_cut_data = true;
                in.prestamped = std::move(pre);
            }
            in.user = std::move(user);
            batch.push_back(std::move(in));
        }
        terrain_reload_total = batch.size();
        if (batch.empty()) {
            terrain_reload_active = false;
            return;
        }
        terrain_reload_active = true;
        const auto ctx = needs_restamp ? make_restamp_context()
                                       : editor::TerrainRestampContext::build({}, {}, {});
        auto post = [cache = terrain_mesh_templates](const editor::TerrainRestampTileIn& in,
                                                     const editor::TerrainRestampResult& r)
            -> std::shared_ptr<void> {
            const auto* u = static_cast<const TerrainTileUser*>(in.user.get());
            if (!u) return nullptr;
            auto tpl = cache->get(in.key);
            if (!tpl || tpl->tex_key != u->tex_key) {
                // Disk read + ground-paint bake: now on a worker, once per tile, never on the frame thread.
                const auto terr = editor::build_local_terrain_tile(u->map_path, u->tex, u->omsi_root, u->size_m,
                                                                   u->ground_textures);
                if (!terr) return nullptr;
                auto t = std::make_shared<TerrainMeshTemplate>();
                t->mesh = terr->mesh;
                t->texture_path = terr->texture_path;
                t->tex_key = u->tex_key;
                cache->put(in.key, t);
                tpl = std::move(t);
            }
            auto pl = std::make_shared<TerrainMeshPayload>();
            pl->mesh = tpl->mesh;
            pl->texture_path = tpl->texture_path;
            editor::apply_heightfield_to_terrain_mesh(pl->mesh, r.heights, u->size_m);
            pl->ok = true;
            return pl;
        };
        terrain_reload_queue.submit(ctx, std::move(batch), std::move(post));
    };

    // Per frame: take finished tiles, apply them to RAM and upload within a small time budget.
    auto terrain_reload_pump = [&]() {
        if (!terrain_reload_active) return;
        for (auto& d : terrain_reload_queue.drain()) terrain_reload_ready.push_back(std::move(d));
        const auto t0 = std::chrono::steady_clock::now();
        constexpr double k_budget_ms = 4.0;
        auto rings_equal = [](const std::vector<editor::TerrainHoleRing>& a,
                              const std::vector<editor::TerrainHoleRing>& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
            return true;
        };
        while (!terrain_reload_ready.empty()) {
            auto d = std::move(terrain_reload_ready.back());
            terrain_reload_ready.pop_back();
            TerrainHeightField* field = nullptr;
            for (auto& tf : terrain_fields) {
                if (tile_key(tf.grid_x, tf.grid_y) == d.key) {
                    field = &tf;
                    break;
                }
            }
            // A newer synchronous restamp/reload of this tile already handled it -> drop the late result.
            if (field && field->restamp_token == d.token) {
                field->restamp_token = 0;
                // Restamped result identical to what the GPU already shows -> skip the upload entirely.
                const bool unchanged = d.stamped && d.result.has_cut_data && field->heights == d.result.heights &&
                                       field->hole_mask == d.result.hole_mask &&
                                       rings_equal(field->polycut_rings, d.result.polycut_rings);
                apply_restamp_result(*field, std::move(d.result));
                if (!unchanged && d.payload) {
                    const auto pl = std::static_pointer_cast<TerrainMeshPayload>(d.payload);
                    (void)upload_terrain_payload(field->grid_x, field->grid_y, field->rel_x, field->rel_y, *pl);
                }
            }
            ++terrain_align_gpu_reload_i;
            const auto wall_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (wall_ms >= k_budget_ms) break;
        }
        if (terrain_reload_queue.outstanding() == 0 && terrain_reload_ready.empty()) {
            terrain_reload_active = false;
            terrain_reload_restamp = false;
        }
    };

    auto reload_terrain_gpu_tile = [&](std::int32_t gx, std::int32_t gy) -> bool {
        auto map_path = find_tile_path(gx, gy);
        if (map_path.empty()) return false;
        // Same path resolution as the terrain brush write (workspace *_actual).
        if (cfg.workspace)
            map_path = map::workspace_tile_map_for_read(&*cfg.workspace, map_path);
        const auto& layout = world.tile_layout();
        const auto rect = map::grid_tile_rect_at(cfg.global, layout, gx, gy);
        if (!rect.resolved) return false;
        const float size_m = (std::max)(rect.width_m, rect.height_m);
        const float rx = rect.origin_x - tile_ox;
        const float ry = rect.origin_y - tile_oy;
        map::GroundTex tex = groundtex;
        if (geo_settings.tile_aerial_visible) {
            if (const auto aerial = map::find_tile_aerial_jpg(cfg.omsi_root, cfg.global.root, map_path))
                tex.diffuse = *aerial;
        }
        const auto terr = editor::build_local_terrain_tile(map_path, tex, cfg.omsi_root, size_m,
                                                           cfg.global.ground_textures);
        if (!terr) return false;
        {
            // Fresh disk state (e.g. after painting): refresh the cached pristine mesh for this tile.
            auto t = std::make_shared<TerrainMeshTemplate>();
            t->mesh = terr->mesh;
            t->texture_path = terr->texture_path;
            t->tex_key = terrain_tex_key(tex, size_m);
            terrain_mesh_templates->put(tile_key(gx, gy), std::move(t));
        }
        bool found_tf = false;
        for (auto& tf : terrain_fields) {
            if (tf.grid_x == gx && tf.grid_y == gy) {
                tf.rel_x = rx;
                tf.rel_y = ry;
                tf.size_m = size_m;
                tf.heights_base = terr->heights;
                restamp_terrain_field(tf);
                found_tf = true;
                break;
            }
        }
        if (!found_tf) {
            TerrainHeightField field{};
            field.grid_x = gx;
            field.grid_y = gy;
            field.rel_x = rx;
            field.rel_y = ry;
            field.size_m = size_m;
            field.heights_base = terr->heights;
            restamp_terrain_field(field);
            terrain_fields.push_back(std::move(field));
        }
        auto mesh = terr->mesh;
        apply_stamped_heights_to_mesh(mesh, gx, gy, size_m);
        terrain_detail_base[tile_key(gx, gy)] = std::make_shared<const gfx::BufferMesh>(mesh);
        if (!terr->texture_path.empty()) renderer.invalidate_texture_path(terr->texture_path);
        for (auto& tc : terrain_chunks) {
            if (tc.tile_gx != gx || tc.tile_gy != gy) continue;
            if (!tc.alive) tc.alive = true;
            if (!renderer.replace_textured_mesh(
                    tc.chunk, mesh.vertices.data(), static_cast<std::uint32_t>(mesh.vertices.size()),
                    mesh.indices.data(), static_cast<std::uint32_t>(mesh.indices.size()),
                    terr->texture_path, 0u))
                return false;
            tc.detail_state = 0;
            ++tc.detail_gen;
            renderer.clear_chunk_detail(tc.chunk);
            const auto wt = store_world_transposed(XMMatrixTranslation(rx, ry, 0.f));
            renderer.set_chunk_world_matrix(tc.chunk, wt.data());
            renderer.set_chunk_visible(tc.chunk, true);
            return true;
        }
        if (!renderer.append_textured_mesh(mesh.vertices.data(),
                                           static_cast<std::uint32_t>(mesh.vertices.size()),
                                           mesh.indices.data(),
                                           static_cast<std::uint32_t>(mesh.indices.size()), terr->texture_path,
                                           0u))
            return false;
        const std::size_t chunk = renderer.chunk_count() - 1;
        const auto wt = store_world_transposed(XMMatrixTranslation(rx, ry, 0.f));
        renderer.set_chunk_world_matrix(chunk, wt.data());
        register_terrain_chunk(chunk, gx, gy);
        return true;
    };

    // Rebuild the terrain GPU mesh of one tile from the in-memory height field (no disk read / write).
    auto refresh_terrain_gpu_from_memory = [&](TerrainHeightField& tf) -> bool {
        const auto tpl = terrain_mesh_templates->get(tile_key(tf.grid_x, tf.grid_y));
        if (!tpl) return reload_terrain_gpu_tile(tf.grid_x, tf.grid_y); // first time: build the template from disk
        auto mesh = tpl->mesh;
        apply_stamped_heights_to_mesh(mesh, tf.grid_x, tf.grid_y, tf.size_m);
        terrain_detail_base[tile_key(tf.grid_x, tf.grid_y)] = std::make_shared<const gfx::BufferMesh>(mesh);
        for (auto& tc : terrain_chunks) {
            if (tc.tile_gx != tf.grid_x || tc.tile_gy != tf.grid_y) continue;
            if (!renderer.replace_textured_mesh(tc.chunk, mesh.vertices.data(),
                                                static_cast<std::uint32_t>(mesh.vertices.size()), mesh.indices.data(),
                                                static_cast<std::uint32_t>(mesh.indices.size()), tpl->texture_path, 0u))
                return false;
            tc.detail_state = 0;
            ++tc.detail_gen;
            renderer.clear_chunk_detail(tc.chunk);
            renderer.set_chunk_visible(tc.chunk, true);
            return true;
        }
        return false;
    };
    // ── Water: one quad per tile (OMSI TFileWater: int32 version=1 + 4 float corner heights) ────────────
    struct WaterTile {
        std::int32_t gx = 0, gy = 0;
        float h[4]{0.f, 0.f, 0.f, 0.f}; // file order: (x0,y0) (x1,y0) (x0,y1) (x1,y1)
        bool has = true;
        bool dirty = false;
        bool section_pending = false;   // [water] must be added to / removed from the .map on Save
        std::size_t chunk = static_cast<std::size_t>(-1);
    };
    std::vector<WaterTile> water_tiles;
    std::unordered_set<std::int64_t> water_checked;
    auto water_file_for = [&](std::int32_t gx, std::int32_t gy) -> std::filesystem::path {
        auto mp = find_tile_path(gx, gy);
        if (cfg.workspace) mp = map::workspace_tile_map_for_read(&*cfg.workspace, mp);
        if (mp.empty()) return {};
        return std::filesystem::path(mp.string() + ".water");
    };
    auto find_water = [&](std::int32_t gx, std::int32_t gy) -> WaterTile* {
        for (auto& w : water_tiles)
            if (w.gx == gx && w.gy == gy) return &w;
        return nullptr;
    };
    auto rebuild_water_mesh = [&](WaterTile& w) {
        const TerrainHeightField* tf = nullptr;
        for (const auto& f : terrain_fields)
            if (f.grid_x == w.gx && f.grid_y == w.gy) { tf = &f; break; }
        if (!tf) return;
        constexpr int kSeg = 8;
        std::vector<o3d::Vertex> verts;
        std::vector<std::uint32_t> inds;
        verts.reserve((kSeg + 1) * (kSeg + 1));
        for (int j = 0; j <= kSeg; ++j)
            for (int i = 0; i <= kSeg; ++i) {
                const float u = static_cast<float>(i) / kSeg, v = static_cast<float>(j) / kSeg;
                const float z = w.h[0] * (1.f - u) * (1.f - v) + w.h[1] * u * (1.f - v) + w.h[2] * (1.f - u) * v + w.h[3] * u * v;
                o3d::Vertex vt{};
                vt.position[0] = u * tf->size_m;
                vt.position[1] = v * tf->size_m;
                vt.position[2] = z;
                vt.normal[2] = 1.f;
                vt.uv[0] = u * 16.f;
                vt.uv[1] = v * 16.f;
                verts.push_back(vt);
            }
        for (int j = 0; j < kSeg; ++j)
            for (int i = 0; i < kSeg; ++i) {
                const std::uint32_t a = static_cast<std::uint32_t>(j * (kSeg + 1) + i), b = a + 1, c = a + (kSeg + 1), d = c + 1;
                // both windings: the plane is seen from above and from below
                inds.insert(inds.end(), {a, c, b, b, c, d, a, b, c, b, d, c});
            }
        std::filesystem::path tex;
        for (const char* sub : {"Texture", "texture"}) {
            const auto cand = cfg.omsi_root / sub / "water.tga";
            std::error_code ec;
            if (std::filesystem::is_regular_file(cand, ec)) { tex = cand; break; }
        }
        bool ok = false;
        if (w.chunk != static_cast<std::size_t>(-1))
            ok = renderer.replace_textured_mesh(w.chunk, verts.data(), static_cast<std::uint32_t>(verts.size()), inds.data(),
                                                static_cast<std::uint32_t>(inds.size()), tex, 0u);
        else if (renderer.append_textured_mesh(verts.data(), static_cast<std::uint32_t>(verts.size()), inds.data(),
                                               static_cast<std::uint32_t>(inds.size()), tex, 0u)) {
            w.chunk = renderer.chunk_count() - 1;
            ok = true;
        }
        if (!ok) return;
        const auto wt = store_world_transposed(XMMatrixTranslation(tf->rel_x, tf->rel_y, 0.f));
        renderer.set_chunk_world_matrix(w.chunk, wt.data());
        renderer.set_chunk_highlight(w.chunk, 0.45f, 0.15f, 0.40f, 0.85f); // blue wash over the water texture
        renderer.set_chunk_visible(w.chunk, false); // drawn by the OMSI water pass (sync_water_gpu)
    };
    // Pushes the visible water tiles (world-space quads + corner heights) to the OMSI water pass. Only
    // uploads when the data actually changed (FNV hash over origin/size/heights).
    bool water_gpu_sent = false;
    std::uint64_t water_gpu_sig = 0;
    auto sync_water_gpu = [&]() {
        const bool vis = panel.imgui_state().water_visible;
        std::vector<omsi::gfx::dx12::HelloO3dRenderer::WaterQuad> quads;
        std::uint64_t sig = 1469598103934665603ull;
        auto mix = [&sig](float f) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            sig ^= bits;
            sig *= 1099511628211ull;
        };
        for (const auto& w : water_tiles) {
            if (!w.has || !vis) continue;
            const TerrainHeightField* tf = nullptr;
            for (const auto& f : terrain_fields)
                if (f.grid_x == w.gx && f.grid_y == w.gy) { tf = &f; break; }
            if (!tf) continue;
            omsi::gfx::dx12::HelloO3dRenderer::WaterQuad q;
            q.origin_x = tf->rel_x;
            q.origin_y = tf->rel_y;
            q.size_m = tf->size_m;
            for (int i = 0; i < 4; ++i) q.h[i] = w.h[i];
            quads.push_back(q);
            mix(q.origin_x);
            mix(q.origin_y);
            mix(q.size_m);
            for (int i = 0; i < 4; ++i) mix(q.h[i]);
        }
        if (water_gpu_sent && sig == water_gpu_sig) return;
        water_gpu_sent = true;
        water_gpu_sig = sig;
        renderer.set_water_quads(quads.data(), static_cast<std::uint32_t>(quads.size()));
    };
    // Per frame: load the .water of a few tiles at a time, keep visibility in sync with the checkbox.
    auto pump_water = [&]() {
        int budget = 2;
        for (const auto& tf : terrain_fields) {
            const std::int64_t key = tile_key(tf.grid_x, tf.grid_y);
            if (water_checked.count(key)) continue;
            if (budget-- <= 0) break;
            water_checked.insert(key);
            const auto wp = water_file_for(tf.grid_x, tf.grid_y);
            std::error_code ec;
            if (wp.empty() || !std::filesystem::is_regular_file(wp, ec)) continue;
            std::ifstream in(wp, std::ios::binary);
            std::int32_t ver = 0;
            float h[4]{};
            in.read(reinterpret_cast<char*>(&ver), 4);
            in.read(reinterpret_cast<char*>(h), sizeof(h));
            if (!in || ver != 1) continue;
            WaterTile w;
            w.gx = tf.grid_x;
            w.gy = tf.grid_y;
            for (int i = 0; i < 4; ++i) w.h[i] = h[i];
            water_tiles.push_back(w);
            rebuild_water_mesh(water_tiles.back());
        }
        for (auto& w : water_tiles)
            if (w.chunk != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(w.chunk, false);
        sync_water_gpu();
    };
    // Adds / removes the [water] block in the tile .map text (UTF-8/ANSI or UTF-16LE with BOM).
    auto edit_map_water_section = [&](const std::filesystem::path& map_path, bool add) {
        std::ifstream in(map_path, std::ios::binary);
        if (!in) return;
        std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const bool u16 = raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE;
        std::string text;
        if (u16) {
            for (std::size_t i = 2; i + 1 < raw.size(); i += 2) text.push_back(raw[i]); // ASCII subset is enough for tags
        } else {
            text = raw;
        }
        const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        std::vector<std::string> lines;
        {
            std::size_t p = 0;
            while (p <= text.size()) {
                const std::size_t q = text.find('\n', p);
                std::string ln = text.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                lines.push_back(ln);
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        auto trimmed = [](std::string t) {
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
            return t;
        };
        int ti = -1, wi = -1;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (trimmed(lines[i]) == "[terrain]" && ti < 0) ti = static_cast<int>(i);
            if (trimmed(lines[i]) == "[water]" && wi < 0) wi = static_cast<int>(i);
        }
        if (add && wi < 0 && ti >= 0 && static_cast<std::size_t>(ti) + 1 < lines.size()) {
            const std::string name = lines[static_cast<std::size_t>(ti) + 1];
            lines.insert(lines.begin() + ti + 3, {"[water]", name, ""});
        } else if (!add && wi >= 0) {
            const int last = (std::min)(static_cast<int>(lines.size()), wi + 3);
            lines.erase(lines.begin() + wi, lines.begin() + last);
        } else {
            return;
        }
        std::string outtext;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            outtext += lines[i];
            if (i + 1 < lines.size()) outtext += eol;
        }
        omsi::fsutil::break_link(map_path); // copy-on-write workspace: never write through a link
        std::ofstream out(map_path, std::ios::binary | std::ios::trunc);
        if (u16) {
            out.put(static_cast<char>(0xFF));
            out.put(static_cast<char>(0xFE));
            for (char c : outtext) { out.put(c); out.put('\0'); }
        } else {
            out << outtext;
        }
    };
    auto flush_water_for_save = [&]() {
        for (auto& w : water_tiles) {
            if (!w.dirty) continue;
            const auto wp = water_file_for(w.gx, w.gy);
            if (wp.empty()) continue;
            auto mp = find_tile_path(w.gx, w.gy);
            if (cfg.workspace) mp = map::workspace_tile_map_for_read(&*cfg.workspace, mp);
            if (w.has) {
                omsi::fsutil::break_link(wp); // copy-on-write workspace: never write through a link
                std::ofstream out(wp, std::ios::binary | std::ios::trunc);
                const std::int32_t ver = 1;
                out.write(reinterpret_cast<const char*>(&ver), 4);
                out.write(reinterpret_cast<const char*>(w.h), sizeof(w.h));
                if (w.section_pending) edit_map_water_section(mp, true);
            } else {
                std::error_code ec;
                std::filesystem::remove(wp, ec);
                if (w.section_pending) edit_map_water_section(mp, false);
            }
            w.dirty = false;
            w.section_pending = false;
        }
    };
    // Terrain-tab events + Agua tab info for the UI.
    auto handle_water_events = [&](const ui::EditorSidePanelState& wev) {
        // tile under the camera target
        std::int32_t cgx = 0, cgy = 0;
        bool have_tile = false;
        for (const auto& tf : terrain_fields) {
            const float lx = camera.target_x - tf.rel_x, ly = camera.target_y - tf.rel_y;
            if (lx >= 0.f && ly >= 0.f && lx <= tf.size_m && ly <= tf.size_m) { cgx = tf.grid_x; cgy = tf.grid_y; have_tile = true; break; }
        }
        if (!have_tile) return;
        if (wev.water_create) {
            WaterTile* w = find_water(cgx, cgy);
            if (!w) { water_tiles.emplace_back(); w = &water_tiles.back(); w->gx = cgx; w->gy = cgy; }
            for (int i = 0; i < 4; ++i) w->h[i] = wev.water_h[i];
            w->has = true; w->dirty = true; w->section_pending = true;
            mark_tile_dirty(cgx, cgy);
            rebuild_water_mesh(*w);
            panel.set_status("Agua creada en tile (" + std::to_string(cgx) + "," + std::to_string(cgy) + ") - se guarda con Save");
        } else if (wev.water_set) {
            if (WaterTile* w = find_water(cgx, cgy); w && w->has) {
                for (int i = 0; i < 4; ++i) w->h[i] = wev.water_h[i];
                w->dirty = true;
                mark_tile_dirty(cgx, cgy);
                rebuild_water_mesh(*w);
            }
        } else if (wev.water_delete) {
            if (WaterTile* w = find_water(cgx, cgy); w && w->has) {
                w->has = false; w->dirty = true; w->section_pending = true;
                if (w->chunk != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(w->chunk, false);
                mark_tile_dirty(cgx, cgy);
                panel.set_status("Agua eliminada del tile - se borra al guardar");
            }
        }
    };
    auto fill_water_args = [&](auto& ui_args) {
        for (const auto& tf : terrain_fields) {
            const float lx = camera.target_x - tf.rel_x, ly = camera.target_y - tf.rel_y;
            if (lx < 0.f || ly < 0.f || lx > tf.size_m || ly > tf.size_m) continue;
            ui_args.water_tile_valid = true;
            ui_args.water_tile_gx = tf.grid_x;
            ui_args.water_tile_gy = tf.grid_y;
            if (const WaterTile* w = find_water(tf.grid_x, tf.grid_y); w && w->has) {
                ui_args.water_has = true;
                for (int i = 0; i < 4; ++i) ui_args.water_h[i] = w->h[i];
            }
            break;
        }
        for (const auto& g : cfg.global.ground_textures) {
            ui_args.ground_layer_diffuse.push_back(g.diffuse);
            ui_args.ground_layer_scale.push_back(g.scale);
            ui_args.ground_layer_detail.push_back(g.detail);
            ui_args.ground_layer_main_rep.push_back(g.main_rep);
        }
    };

    // Called by Save: write the .terrain of every edited tile.
    flush_terrain_for_save = [&]() {
        for (const auto key : terrain_dirty_tiles) {
            for (auto& tf : terrain_fields) {
                if (tile_key(tf.grid_x, tf.grid_y) != key || tf.heights_base.empty()) continue;
                auto map_path = find_tile_path(tf.grid_x, tf.grid_y);
                if (cfg.workspace) map_path = map::workspace_tile_map_for_read(&*cfg.workspace, map_path);
                if (map_path.empty() || !map::write_terrain_heights(map_path, tf.heights_base))
                    log::warn("terrain write failed " + map_path.string());
                break;
            }
        }
        terrain_dirty_tiles.clear();
        flush_water_for_save();
    };

    auto on_create_tile_at = [&](const std::int32_t gx, const std::int32_t gy) -> bool {
        if (map::get_kachel_index_with_coord(cfg.global, gx, gy) >= 0) {
            panel.set_status("Celda ocupada (" + std::to_string(gx) + "," + std::to_string(gy) + ")");
            return false;
        }
        bool prepared = true;
        if (cfg.workspace) {
            prepared = map::workspace_prepare_global_cfg_for_write(*cfg.workspace);
            if (prepared) cfg.global.global_cfg_path = cfg.workspace->actual_global_cfg;
        }
        if (!prepared) {
            panel.set_status("Crear tile falló (global.cfg)");
            return false;
        }
        log::info("create_tile_at (" + std::to_string(gx) + "," + std::to_string(gy) + ") root=" +
                  cfg.global.global_cfg_path.parent_path().string());
        if (!map::create_new_tile(cfg.global, gx, gy, cfg.omsi_root, 0.f)) {
            panel.set_status("Crear tile falló (disco)");
            return false;
        }
        for (const auto& ref : cfg.global.tiles) {
            if (ref.grid_x != gx || ref.grid_y != gy) continue;
            bool in_world = false;
            for (const auto& t : world.tiles()) {
                if (t.grid_x == gx && t.grid_y == gy) {
                    in_world = true;
                    break;
                }
            }
            if (!in_world) {
                map::MapTileData tile{};
                tile.path = ref.tile_path;
                tile.grid_x = gx;
                tile.grid_y = gy;
                world.tiles_mut().push_back(std::move(tile));
            }
            break;
        }
        world.refresh_tile_layout(cfg.global);
        tile_frame_empty_pool_dirty = true;
        for (const auto& t : world.tiles()) (void)reload_terrain_gpu_tile(t.grid_x, t.grid_y);
        if (!reload_terrain_gpu_tile(gx, gy)) {
            panel.set_status("Archivos OK; preview GPU falló — recargá el mapa");
            log::warn("create_tile_at GPU reload failed (" + std::to_string(gx) + "," + std::to_string(gy) + ")");
        }
        centerkachel.gx = gx;
        centerkachel.gy = gy;
        centerkachel.valid = true;
        center_camera_on_centerkachel();
        refresh_editor_lists();
        panel.set_status("Tile creado tile_" + std::to_string(gx) + "_" + std::to_string(gy) + ".map");
        log::info("editor tile created (" + std::to_string(gx) + "," + std::to_string(gy) + ")");
        return true;
    };

    auto snap_centerkachel_to_existing = [&](const float wx, const float wy) {
        const auto ck = map::centerkachel_from_world(cfg.global, world.tile_layout(), wx, wy);
        if (ck.valid && map::get_kachel_index_with_coord(cfg.global, ck.gx, ck.gy) >= 0) {
            centerkachel = ck;
            return true;
        }
        const auto& layout = world.tile_layout();
        bool found = false;
        float best_d2 = 1.e30f;
        map::CenterKachel best{};
        for (const auto& ref : cfg.global.tiles) {
            const auto rect = map::grid_tile_rect_at(cfg.global, layout, ref.grid_x, ref.grid_y);
            if (!rect.resolved) continue;
            const float cx = rect.origin_x + rect.width_m * 0.5f;
            const float cy = rect.origin_y + rect.height_m * 0.5f;
            const float ddx = wx - cx;
            const float ddy = wy - cy;
            const float d2 = ddx * ddx + ddy * ddy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best.gx = ref.grid_x;
                best.gy = ref.grid_y;
                best.valid = true;
                found = true;
            }
        }
        if (found) centerkachel = best;
        return found;
    };

    auto on_tile_rmb = [&](const float wx, const float wy) {
        if (!snap_centerkachel_to_existing(wx, wy)) {
            panel.set_status("RMB: no hay tile en el mapa");
            return;
        }
        tile_frame_empty_pool_dirty = true;
        center_camera_on_centerkachel();
        panel.set_status("Tile (" + std::to_string(centerkachel.gx) + "," + std::to_string(centerkachel.gy) + ")");
    };

    auto hide_tile_frame_slots = [&]() {
        if (tile_frame_cam.chunk != static_cast<std::size_t>(-1))
            renderer.set_chunk_visible(tile_frame_cam.chunk, false);
        if (tile_frame_hover.chunk != static_cast<std::size_t>(-1))
            renderer.set_chunk_visible(tile_frame_hover.chunk, false);
        for (const auto& slot : tile_frame_empty_pool) {
            if (slot.chunk != static_cast<std::size_t>(-1))
                renderer.set_chunk_visible(slot.chunk, false);
        }
    };

    auto log_wireframe_cell = [&](const std::string& tag, const std::int32_t gx, const std::int32_t gy,
                                  const map::GridTileRect& rect, const float rx, const float ry,
                                  const std::string& extra = {}) {
        if (!tile_dbg.enabled) return;
        log::info(std::string("TILE_WF ") + tag + " gx=" + std::to_string(gx) + " gy=" + std::to_string(gy) +
                  " world_o=(" + std::to_string(rect.origin_x) + "," + std::to_string(rect.origin_y) + ")" +
                  " size=(" + std::to_string(rect.width_m) + "x" + std::to_string(rect.height_m) + ")" +
                  " gpu_rel=(" + std::to_string(rx) + "," + std::to_string(ry) + ")" + extra);
    };

    auto upsert_tile_frame_slot = [&](TileFrameSlot& slot, std::int32_t gx, std::int32_t gy)
        -> std::optional<std::size_t> {
        if (tile_has_terrain_gpu(gx, gy)) {
            if (slot.chunk != static_cast<std::size_t>(-1)) renderer.set_chunk_visible(slot.chunk, false);
            if (tile_dbg.enabled)
                log::info("TILE_WF skip gx=" + std::to_string(gx) + " gy=" + std::to_string(gy) + " (terrain_gpu)");
            return std::nullopt;
        }
        const auto& layout = world.tile_layout();
        map::GridTileRect rect = map::grid_tile_rect_at(cfg.global, layout, gx, gy);
        if (!rect.resolved) {
            if (tile_dbg.enabled) {
                ++tile_dbg.wireframe_upsert_fail;
                log::warn("TILE_WF fail gx=" + std::to_string(gx) + " gy=" + std::to_string(gy) + " rect_unresolved");
            }
            return std::nullopt;
        }
        const float rx = rect.origin_x - tile_ox;
        const float ry = rect.origin_y - tile_oy;
        constexpr float kFrameZ = 0.08f;
        omsi::o3d::Vertex verts[4]{};
        const float corners[4][2] = {{rx, ry},
                                     {rx + rect.width_m, ry},
                                     {rx + rect.width_m, ry + rect.height_m},
                                     {rx, ry + rect.height_m}};
        for (int i = 0; i < 4; ++i) {
            verts[i].position[0] = corners[i][0];
            verts[i].position[1] = corners[i][1];
            verts[i].position[2] = kFrameZ;
            verts[i].normal[0] = 0.f;
            verts[i].normal[1] = 0.f;
            verts[i].normal[2] = 1.f;
            verts[i].uv[0] = 0.f;
            verts[i].uv[1] = 0.f;
        }
        const std::uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        const bool same_cell = slot.chunk != static_cast<std::size_t>(-1) && slot.tile_gx == gx && slot.tile_gy == gy;
        if (same_cell) {
            renderer.set_chunk_wireframe(slot.chunk, true);
            renderer.set_chunk_visible(slot.chunk, true);
            log_wireframe_cell("reuse", gx, gy, rect, rx, ry);
            return slot.chunk;
        }
        if (slot.chunk != static_cast<std::size_t>(-1)) {
            if (!renderer.replace_textured_mesh(slot.chunk, verts, 4, idx, 6, {}, 0, false, true)) {
                ++tile_dbg.wireframe_upsert_fail;
                if (tile_dbg.enabled)
                    log::warn("TILE_WF fail gx=" + std::to_string(gx) + " gy=" + std::to_string(gy) + " replace_mesh");
                return std::nullopt;
            }
            ++tile_dbg.frame_mesh_replace;
        } else {
            if (!renderer.append_textured_mesh(verts, 4, idx, 6, {})) {
                ++tile_dbg.wireframe_upsert_fail;
                if (tile_dbg.enabled)
                    log::warn("TILE_WF fail gx=" + std::to_string(gx) + " gy=" + std::to_string(gy) + " append_mesh");
                return std::nullopt;
            }
            slot.chunk = renderer.chunk_count() - 1;
            ++tile_dbg.frame_mesh_append;
        }
        slot.tile_gx = gx;
        slot.tile_gy = gy;
        renderer.set_chunk_wireframe(slot.chunk, true);
        renderer.set_chunk_visible(slot.chunk, true);
        log_wireframe_cell("draw", gx, gy, rect, rx, ry, " chunk=" + std::to_string(slot.chunk));
        return slot.chunk;
    };

    auto set_tile_cell_highlight = [&](const std::int32_t gx, const std::int32_t gy, const float amt, const float r,
                                       const float g, const float b) -> bool {
        for (const auto& tc : terrain_chunks) {
            if (tc.alive && tc.tile_gx == gx && tc.tile_gy == gy) {
                renderer.set_chunk_highlight(tc.chunk, amt, r, g, b);
                return true;
            }
        }
        for (const auto& slot : tile_frame_empty_pool) {
            if (slot.tile_gx == gx && slot.tile_gy == gy && slot.chunk != static_cast<std::size_t>(-1)) {
                renderer.set_chunk_wireframe(slot.chunk, true);
                renderer.set_chunk_visible(slot.chunk, true);
                renderer.set_chunk_highlight(slot.chunk, amt, r, g, b);
                return true;
            }
        }
        return false;
    };

    auto resolve_streaming_tile_at_camera = [&]() -> map::ActiveTileAtWorld {
        if (cached_stream_tile_frame_serial == editor_frame_serial) return cached_stream_tile_frame;
        const float cam_wx = tile_ox + camera.target_x;
        const float cam_wy = tile_oy + camera.target_y;
        const auto& layout = world.tile_layout();
        auto active = map::grid_cell_at_world_xy(cfg.global, layout, cam_wx, cam_wy);
        cached_stream_tile_frame = active;
        cached_stream_tile_frame_serial = editor_frame_serial;
        return active;
    };

    auto rebuild_neighbor_wireframes = [&]() {
        const bool center_changed =
            !centerkachel.valid || centerkachel.gx != tile_frame_empty_pool_center_gx ||
            centerkachel.gy != tile_frame_empty_pool_center_gy;
        if (!tile_frame_empty_pool_dirty && !center_changed) return;
        tile_frame_empty_pool_dirty = false;
        ++tile_dbg.wireframe_rebuilds;
        const auto& layout = world.tile_layout();
        std::vector<std::pair<std::int32_t, std::int32_t>> empty_cells;
        empty_cells.reserve(8);
        if (centerkachel.valid) {
            tile_frame_empty_pool_center_gx = centerkachel.gx;
            tile_frame_empty_pool_center_gy = centerkachel.gy;
            if (tile_dbg.enabled) {
                log::info("TILE_WF rebuild centerkachel=(" + std::to_string(centerkachel.gx) + "," +
                          std::to_string(centerkachel.gy) + ") tile_ox=" + std::to_string(tile_ox) +
                          " tile_oy=" + std::to_string(tile_oy));
            }
            for (std::int32_t dy = -1; dy <= 1; ++dy) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const std::int32_t gx = centerkachel.gx + dx;
                    const std::int32_t gy = centerkachel.gy + dy;
                    const bool in_map = map::get_kachel_index_with_coord(cfg.global, gx, gy) >= 0;
                    const bool has_gpu = tile_has_terrain_gpu(gx, gy);
                    const auto rect = map::grid_tile_rect_at(cfg.global, layout, gx, gy);
                    if (tile_dbg.enabled) {
                        std::string tag = in_map ? "in_map" : (has_gpu ? "gpu_only" : "empty");
                        if (rect.resolved) {
                            const float rx = rect.origin_x - tile_ox;
                            const float ry = rect.origin_y - tile_oy;
                            log_wireframe_cell("3x3_" + tag, gx, gy, rect, rx, ry);
                            for (const auto& tc : terrain_chunks) {
                                if (!tc.alive || tc.tile_gx != gx || tc.tile_gy != gy) continue;
                                const auto* tf = static_cast<const TerrainHeightField*>(nullptr);
                                for (const auto& f : terrain_fields) {
                                    if (f.grid_x == gx && f.grid_y == gy) {
                                        tf = &f;
                                        break;
                                    }
                                }
                                if (tf) {
                                    const float dx_rel = tf->rel_x - rx;
                                    const float dy_rel = tf->rel_y - ry;
                                    log::info("TILE_WF terrain_gpu gx=" + std::to_string(gx) + " gy=" +
                                              std::to_string(gy) + " rel=(" + std::to_string(tf->rel_x) + "," +
                                              std::to_string(tf->rel_y) + ") size_m=" + std::to_string(tf->size_m) +
                                              " delta_rel=(" + std::to_string(dx_rel) + "," +
                                              std::to_string(dy_rel) + ")");
                                }
                                break;
                            }
                        } else {
                            log::warn("TILE_WF 3x3_unresolved gx=" + std::to_string(gx) + " gy=" +
                                      std::to_string(gy) + " in_map=" + std::to_string(in_map));
                        }
                    }
                    if (in_map) continue;
                    if (has_gpu) continue;
                    empty_cells.emplace_back(gx, gy);
                }
            }
        } else {
            tile_frame_empty_pool_center_gx = -1;
            tile_frame_empty_pool_center_gy = -1;
            if (tile_dbg.enabled) log::warn("TILE_WF rebuild centerkachel invalid");
        }
        const std::size_t n = empty_cells.size();
        if (tile_dbg.enabled) {
            log::info("TILE_WF pool empty_slots=" + std::to_string(n) + "/8");
        }
        if (tile_frame_empty_pool.size() > n) {
            for (std::size_t i = n; i < tile_frame_empty_pool.size(); ++i) {
                if (tile_frame_empty_pool[i].chunk != static_cast<std::size_t>(-1))
                    renderer.set_chunk_visible(tile_frame_empty_pool[i].chunk, false);
            }
        }
        tile_frame_empty_pool.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            upsert_tile_frame_slot(tile_frame_empty_pool[i], empty_cells[i].first, empty_cells[i].second);
    };

    auto on_create_tile_here = [&]() -> bool {
        if (!centerkachel.valid ||
            map::get_kachel_index_with_coord(cfg.global, centerkachel.gx, centerkachel.gy) < 0) {
            const float wx = tile_ox + camera.target_x;
            const float wy = tile_oy + camera.target_y;
            if (!snap_centerkachel_to_existing(wx, wy)) {
                panel.set_status("Crear tile: RMB sobre un tile existente, luego usa las flechas");
                return false;
            }
        }
        if (pending_create_dx == 0 && pending_create_dy == 0) {
            panel.set_status("Crear tile: usa una flecha N/S/E/O");
            return false;
        }
        const std::int32_t gx = centerkachel.gx + pending_create_dx;
        const std::int32_t gy = centerkachel.gy + pending_create_dy;
        pending_create_dx = 0;
        pending_create_dy = 0;
        return on_create_tile_at(gx, gy);
    };
