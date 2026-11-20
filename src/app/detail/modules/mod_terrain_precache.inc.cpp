// MOD: terrain precache — GPU mesh + heights for tiles in a ring beyond the active 9×9.
    constexpr int kTerrainPrecacheRingTiles = 7;
    bool terrain_precache_armed = false;
    std::int32_t terrain_precache_center_gx = 0;
    std::int32_t terrain_precache_center_gy = 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> terrain_precache_queue;
    std::size_t terrain_precache_qi = 0;

    auto terrain_precache_outer_radius = [&]() { return editor_tile_radius + kTerrainPrecacheRingTiles; };

    auto chebyshev_tile_dist = [](const std::int32_t gx, const std::int32_t gy, const std::int32_t cx,
                                  const std::int32_t cy) -> int {
        return (std::max)(std::abs(gx - cx), std::abs(gy - cy));
    };

    auto terrain_gpu_alive_at = [&](const std::int32_t gx, const std::int32_t gy) -> bool {
        for (const auto& tc : terrain_chunks) {
            if (tc.alive && tc.tile_gx == gx && tc.tile_gy == gy) return true;
        }
        return false;
    };

    auto in_terrain_precache_ring = [&](const std::int32_t gx, const std::int32_t gy, const std::int32_t cx,
                                        const std::int32_t cy) -> bool {
        const int d = chebyshev_tile_dist(gx, gy, cx, cy);
        return d > editor_tile_radius && d <= terrain_precache_outer_radius();
    };

    auto rebuild_terrain_precache_queue = [&]() {
        const auto active = resolve_streaming_tile_at_camera();
        const std::int32_t cx = active.grid_x;
        const std::int32_t cy = active.grid_y;
        if (cx == terrain_precache_center_gx && cy == terrain_precache_center_gy) return;
        terrain_precache_center_gx = cx;
        terrain_precache_center_gy = cy;
        terrain_precache_queue.clear();
        terrain_precache_qi = 0;
        const int R = terrain_precache_outer_radius();
        const float cam_wx = tile_ox + camera.target_x;
        const float cam_wy = tile_oy + camera.target_y;
        const auto& layout = world.tile_layout();
        struct Item {
            std::int32_t gx = 0;
            std::int32_t gy = 0;
            float dist2 = 0.f;
        };
        std::vector<Item> items;
        items.reserve(static_cast<std::size_t>((2 * R + 1) * (2 * R + 1)));
        for (int dy = -R; dy <= R; ++dy) {
            for (int dx = -R; dx <= R; ++dx) {
                const std::int32_t gx = cx + dx;
                const std::int32_t gy = cy + dy;
                if (!in_terrain_precache_ring(gx, gy, cx, cy)) continue;
                if (world.has_tile(gx, gy)) continue;
                if (terrain_gpu_alive_at(gx, gy)) continue;
                const auto map_path = find_tile_path(gx, gy);
                if (map_path.empty()) continue;
                const auto origin = map::tile_origin_xy(layout, gx, gy);
                const float size_m = map::tile_size_meters_at_grid(cfg.global, layout, gx, gy);
                const float tx = origin.first + size_m * 0.5f;
                const float ty = origin.second + size_m * 0.5f;
                const float ddx = tx - cam_wx;
                const float ddy = ty - cam_wy;
                items.push_back({gx, gy, ddx * ddx + ddy * ddy});
            }
        }
        std::sort(items.begin(), items.end(),
                  [](const Item& a, const Item& b) { return a.dist2 < b.dist2; });
        terrain_precache_queue.reserve(items.size());
        for (const auto& it : items) terrain_precache_queue.emplace_back(it.gx, it.gy);
        if (!items.empty()) {
            log::info("terrain precache queue center=(" + std::to_string(cx) + "," + std::to_string(cy) +
                      ") pending=" + std::to_string(items.size()) + " outer_r=" +
                      std::to_string(terrain_precache_outer_radius()) + " active_r=" +
                      std::to_string(editor_tile_radius));
        }
#ifdef _WIN32
        if (tile_dbg.enabled && !items.empty()) {
            log::info("TILE_DEBUG terrain_precache rebuild center=(" + std::to_string(cx) + "," +
                      std::to_string(cy) + ") pending=" + std::to_string(items.size()));
        }
#endif
    };

    auto evict_terrain_precache_outside = [&]() {
        const int R = terrain_precache_outer_radius();
        int n = 0;
        for (auto& tc : terrain_chunks) {
            if (!tc.alive) continue;
            if (world.has_tile(tc.tile_gx, tc.tile_gy)) continue;
            if (chebyshev_tile_dist(tc.tile_gx, tc.tile_gy, terrain_precache_center_gx,
                                    terrain_precache_center_gy) <= R)
                continue;
            renderer.set_chunk_visible(tc.chunk, false);
            tc.alive = false;
            terrain_fields.erase(std::remove_if(terrain_fields.begin(), terrain_fields.end(),
                                                [&](const TerrainHeightField& tf) {
                                                    return tf.grid_x == tc.tile_gx && tf.grid_y == tc.tile_gy;
                                                }),
                               terrain_fields.end());
            if (++n >= 2) break;
        }
    };

    auto pump_terrain_precache = [&]() {
        if (!terrain_precache_armed) return;
        if (panel.current_tool() == ui::EditorTool::Tile) return;
        if (stream_busy() || group_settle_busy() || save_job_active) return;
        if (stream_cooldown_frames > 0) return;
        std::size_t terrain_alive = 0;
        for (const auto& tc : terrain_chunks)
            if (tc.alive) ++terrain_alive;
        if (!world.tiles().empty() && terrain_alive >= world.tiles().size()) return;
        static std::size_t last_world_tile_count = 0;
        if (world.tiles().size() != last_world_tile_count) {
            last_world_tile_count = world.tiles().size();
            terrain_precache_center_gx -= 9999;
            terrain_precache_queue.clear();
            terrain_precache_qi = 0;
        }
        rebuild_terrain_precache_queue();
        evict_terrain_precache_outside();
        if (terrain_precache_qi >= terrain_precache_queue.size()) return;
        auto& gov = omsi::LoadGovernor::global();
        auto precache_fb = gov.budget();
        int uploaded = 0;
        while (terrain_precache_qi < terrain_precache_queue.size()) {
            auto c = omsi::FrameBudget::gpu_mesh(3721, 0);
            c.bytes += 256ull * 1024ull;
            if (gov.admit(precache_fb, omsi::LoadGovernor::Kind::Terrain, c) == omsi::FrameBudget::Admit::Defer)
                break;
            const auto [gx, gy] = terrain_precache_queue[terrain_precache_qi++];
            if (world.has_tile(gx, gy) || terrain_gpu_alive_at(gx, gy)) continue;
            const auto t0 = std::chrono::steady_clock::now();
            if (reload_terrain_gpu_tile(gx, gy)) {
                const double dt =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                precache_fb.charge(dt, c);
                gov.learn(omsi::LoadGovernor::Kind::Terrain, c.bytes, dt);
                ++uploaded;
                log::info("terrain precache +(" + std::to_string(gx) + "," + std::to_string(gy) + ") left=" +
                          std::to_string(terrain_precache_queue.size() - terrain_precache_qi));
                if (precache_fb.expired() || !precache_fb.time_ok()) break;
            }
        }
    };

    auto arm_terrain_precache = [&]() {
        if (terrain_precache_armed) return;
        terrain_precache_armed = true;
        terrain_precache_center_gx = tile_gx - 9999;
        terrain_precache_center_gy = tile_gy - 9999;
        log::info("terrain precache armed active_r=" + std::to_string(editor_tile_radius) + " ring=+" +
                  std::to_string(kTerrainPrecacheRingTiles) + " outer_r=" +
                  std::to_string(terrain_precache_outer_radius()) + " (mesh+heights, follows camera)");
    };

    // ---- Terrain detail (cut) meshes ------------------------------------------------------------------------
    // Every streamed tile is uploaded as the cheap uncut 61x61 grid (3.7 k vertices). The polycut mesh around
    // AlignTerrain splines/objects (up to 1-2 M vertices, 30-150 ms of upload) is built on a worker and attached
    // (HelloO3dRenderer::set_chunk_detail_mesh) only for tiles within kTerrainDetailRing tiles of the camera; the
    // renderer draws it on top of the base, which becomes an underlay so a gap in the cut never
    // shows the clear colour. Beyond kTerrainDetailDropRing the detail buffers are freed.
    constexpr int kTerrainDetailRing = 2;
    constexpr int kTerrainDetailDropRing = 6;
    struct TerrainDetailDone {
        std::int32_t gx = 0;
        std::int32_t gy = 0;
        std::uint32_t gen = 0;
        bool ok = false;
        gfx::BufferMesh mesh;
    };
    struct TerrainDetailQueue {
        std::mutex mu;
        std::vector<TerrainDetailDone> done;
        std::atomic<int> inflight{0};
    };
    auto terrain_detail_q = std::make_shared<TerrainDetailQueue>();

    auto pump_terrain_detail = [&]() {
        if (terrain_chunks.empty()) return;
        // 1) Upload at most one finished build per frame.
        TerrainDetailDone dn;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(terrain_detail_q->mu);
            if (!terrain_detail_q->done.empty()) {
                dn = std::move(terrain_detail_q->done.back());
                terrain_detail_q->done.pop_back();
                have = true;
            }
        }
        if (have) {
            for (auto& tc : terrain_chunks) {
                if (!tc.alive || tc.tile_gx != dn.gx || tc.tile_gy != dn.gy) continue;
                if (tc.detail_gen != dn.gen || tc.detail_state != 1) break; // stale: the tile changed meanwhile
                if (dn.ok && !dn.mesh.vertices.empty() && !dn.mesh.indices.empty() &&
                    renderer.set_chunk_detail_mesh(tc.chunk, dn.mesh.vertices.data(),
                                                   static_cast<std::uint32_t>(dn.mesh.vertices.size()),
                                                   dn.mesh.indices.data(),
                                                   static_cast<std::uint32_t>(dn.mesh.indices.size()),
                                                   kTerrainDetailRing))
                    tc.detail_state = 2;
                else
                    tc.detail_state = 3;
                break;
            }
        }
        // 2) Free far detail / queue builds for tiles that came into range, nearest ring first. Until its
        // cut mesh arrives a tile shows the uncut base grid, which covers crossings and roads: building in
        // list order (often the far ring first) left the tile under the bus uncut for seconds.
        std::vector<std::pair<int, std::size_t>> detail_want;
        for (std::size_t ti = 0; ti < terrain_chunks.size(); ++ti) {
            auto& tc = terrain_chunks[ti];
            if (!tc.alive) continue;
            const int ring = renderer.chunk_camera_ring(tc.chunk);
            if (ring < 0) continue;
            if (tc.detail_state == 2 && ring >= kTerrainDetailDropRing) {
                renderer.clear_chunk_detail(tc.chunk);
                tc.detail_state = 0;
                continue;
            }
            if (tc.detail_state != 0 || ring > kTerrainDetailRing) continue;
            detail_want.emplace_back(ring, ti);
        }
        std::sort(detail_want.begin(), detail_want.end());
        for (const auto& [ring, ti] : detail_want) {
            auto& tc = terrain_chunks[ti];
            if (terrain_detail_q->inflight.load() >= 3) break;
            const TerrainHeightField* tf = nullptr;
            for (const auto& f : terrain_fields) {
                if (f.grid_x == tc.tile_gx && f.grid_y == tc.tile_gy) {
                    tf = &f;
                    break;
                }
            }
            const auto base_it = terrain_detail_base.find(tile_key(tc.tile_gx, tc.tile_gy));
            if (!tf || base_it == terrain_detail_base.end() ||
                (tf->polycut_rings.empty() && tf->hole_mask.empty())) {
                tc.detail_state = 3; // nothing is cut here: the base grid is the final surface
                continue;
            }
            tc.detail_state = 1;
            terrain_detail_q->inflight.fetch_add(1);
            omsi::JobSystem::global().submit(
                [q = terrain_detail_q, base = base_it->second, heights = tf->heights, rings = tf->polycut_rings,
                 mask = tf->hole_mask, size_m = (tf->size_m > 0.01f ? tf->size_m : map::kStandardTileMeters),
                 gx = tc.tile_gx, gy = tc.tile_gy, gen = tc.detail_gen]() {
                    TerrainDetailDone out;
                    out.gx = gx;
                    out.gy = gy;
                    out.gen = gen;
                    try {
                        out.mesh = *base;
                        if (!rings.empty())
                            editor::apply_terrain_polycut_to_mesh(out.mesh, heights, size_m, rings,
                                                                  editor::kTerrainDim);
                        else
                            editor::apply_terrain_hole_mask_to_mesh(out.mesh, mask, editor::kTerrainDim);
                        out.ok = true;
                    } catch (...) {
                        out.ok = false;
                    }
                    {
                        std::lock_guard<std::mutex> lk(q->mu);
                        q->done.push_back(std::move(out));
                    }
                    q->inflight.fetch_sub(1);
                },
                // High: at Normal the cut queued behind every tile-load job while driving
                ring <= 1 ? omsi::JobSystem::Priority::High : omsi::JobSystem::Priority::Normal);
        }
    };
