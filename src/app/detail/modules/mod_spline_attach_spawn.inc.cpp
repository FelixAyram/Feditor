// MOD: spline attachment spawn — async queue, CPU prefetch, entry reindex, sco/o3d/x GPU reuse.
    std::vector<std::size_t> sa_spawn_queue;
    std::size_t sa_spawn_queue_i = 0;
    bool sa_spawn_active = false;
    std::size_t sa_spawn_wave = 0;
    std::int32_t sa_synth_cursor = 1;
    std::unordered_map<std::size_t, int> sa_spawn_retry_count;
    std::unordered_map<std::size_t, int> sa_mesh_defer_count;
    std::unordered_set<std::size_t> sa_spawned_entries;
    std::unordered_set<std::size_t> sa_spawn_queued;

    struct SaCpuPose {
        std::int32_t spline_id = 0;
        float along = 0.f;
        map::SplineAttachmentPose pose{};
    };
    struct SaCpuSlot {
        std::size_t entry_i = 0;
        std::vector<SaCpuPose> poses;
        std::atomic<bool> ready{false};
        bool skip = false;
        bool have_poses = false;
    };
    std::thread sa_cpu_thread;
    std::atomic<bool> sa_cpu_done{true};
    bool sa_cpu_started = false;
    std::vector<std::unique_ptr<SaCpuSlot>> sa_cpu_slots;
    std::unordered_map<std::size_t, SaCpuSlot*> sa_cpu_by_entry;

    auto sa_join_cpu_workers = [&]() {
        if (sa_cpu_thread.joinable()) sa_cpu_thread.join();
        sa_cpu_started = false;
        sa_cpu_done.store(true, std::memory_order_release);
        sa_cpu_by_entry.clear();
        sa_cpu_slots.clear();
    };

    auto sa_queue_compact = [&]() {
        if (sa_spawn_queue_i == 0) return;
        if (sa_spawn_queue_i >= sa_spawn_queue.size()) {
            sa_spawn_queue.clear();
            sa_spawn_queue_i = 0;
            return;
        }
        sa_spawn_queue.erase(sa_spawn_queue.begin(), sa_spawn_queue.begin() +
                                                       static_cast<std::ptrdiff_t>(sa_spawn_queue_i));
        sa_spawn_queue_i = 0;
    };

    auto find_sa_entry_index = [&](const std::int32_t gx, const std::int32_t gy,
                                   const std::int32_t instance_id) -> std::optional<std::size_t> {
        if (instance_id <= 0) return std::nullopt;
        for (std::size_t i = 0; i < edit_spline_attachments.size(); ++i) {
            const auto& e = edit_spline_attachments[i];
            if (e.tile_gx == gx && e.tile_gy == gy && e.att.instance_id == instance_id) return i;
        }
        return std::nullopt;
    };

    auto reindex_spline_attach_obj_to_entry_after_erase = [&](const std::size_t erased_i) {
        sa_join_cpu_workers();
        for (auto& kv : spline_attach_obj_to_entry) {
            if (kv.second > erased_i) --kv.second;
        }
        auto reindex_int_map = [&](std::unordered_map<std::size_t, int>& m) {
            std::unordered_map<std::size_t, int> next;
            for (const auto& [ei, cnt] : m) {
                if (ei < erased_i)
                    next.emplace(ei, cnt);
                else if (ei > erased_i)
                    next.emplace(ei - 1, cnt);
            }
            m = std::move(next);
        };
        reindex_int_map(sa_spawn_retry_count);
        reindex_int_map(sa_mesh_defer_count);
        if (sa_spawned_entries.count(erased_i)) sa_spawned_entries.erase(erased_i);
        std::unordered_set<std::size_t> new_spawned;
        for (const auto ei : sa_spawned_entries) {
            if (ei < erased_i)
                new_spawned.insert(ei);
            else if (ei > erased_i)
                new_spawned.insert(ei - 1);
        }
        sa_spawned_entries = std::move(new_spawned);
        sa_queue_compact();
        for (auto& q : sa_spawn_queue) {
            if (q > erased_i) --q;
        }
        sa_spawn_queued.clear();
        sa_spawn_queued.insert(sa_spawn_queue.begin(), sa_spawn_queue.end());
    };

    auto clear_sa_spawns_for_entry = [&](const std::size_t entry_i) {
        if (entry_i >= edit_spline_attachments.size()) return;
        const std::int32_t primary = edit_spline_attachments[entry_i].att.instance_id;
        std::vector<std::int32_t> oids;
        for (const auto& kv : spline_attach_obj_to_entry) {
            if (kv.second == entry_i) oids.push_back(kv.first);
        }
        // Only fall back to primary when it is not owned by a *different* SA entry.
        // Orphan map entries used to kill in-place converted free objects (same instance_id).
        if (primary > 0 && std::find(oids.begin(), oids.end(), primary) == oids.end()) {
            const auto own = spline_attach_obj_to_entry.find(primary);
            if (own == spline_attach_obj_to_entry.end() || own->second == entry_i) oids.push_back(primary);
        }
        for (const std::int32_t id : oids) {
            spline_attach_spawn_ids.erase(id);
            spline_attach_obj_to_entry.erase(id);
            for (auto& sp : scene_places) {
                if (sp.object_id != id) continue;
                sp.alive = false;
                renderer.set_chunk_visible(sp.chunk, false);
            }
            edit_objects.erase(std::remove_if(edit_objects.begin(), edit_objects.end(),
                                              [&](const map::MapObjectInst& o) { return o.id == id; }),
                               edit_objects.end());
        }
        sa_spawned_entries.erase(entry_i);
        sa_spawn_retry_count.erase(entry_i);
        sa_mesh_defer_count.erase(entry_i);
    };

    auto clear_sa_spawns_on_doomed_tiles = [&](const auto& grid_doomed) {
        for (std::size_t i = 0; i < edit_spline_attachments.size(); ++i) {
            const auto& e = edit_spline_attachments[i];
            if (grid_doomed(e.tile_gx, e.tile_gy)) clear_sa_spawns_for_entry(i);
        }
    };

    auto compute_sa_synth_object_id = [&]() -> std::int32_t {
        std::int32_t synth = 1;
        for (const auto& o : edit_objects) synth = (std::max)(synth, o.id + 1);
        for (const auto& s : edit_splines) synth = (std::max)(synth, s.id + 1);
        for (const auto& e : edit_spline_attachments)
            if (e.att.instance_id > 0) synth = (std::max)(synth, e.att.instance_id + 1);
        for (const auto& a : edit_attachments)
            if (a.child_id > 0) synth = (std::max)(synth, a.child_id + 1);
        return synth;
    };

    auto sa_kick_cpu = [&]() {
        if (sa_cpu_started && !sa_cpu_done.load(std::memory_order_acquire)) return;
        if (sa_cpu_thread.joinable()) sa_cpu_thread.join();
        sa_cpu_started = false;

        struct In {
            std::size_t entry_i = 0;
            map::MapSplineAttachmentInst att{};
            std::int32_t parent_spline_id = 0;
            std::filesystem::path sco_path;
        };
        constexpr std::size_t kSaKickBatch = 32;
        std::vector<In> ins;
        ins.reserve((std::min)(kSaKickBatch, sa_spawn_queue.size() - sa_spawn_queue_i));
        for (std::size_t i = sa_spawn_queue_i; i < sa_spawn_queue.size() && ins.size() < kSaKickBatch; ++i) {
            const std::size_t ei = sa_spawn_queue[i];
            if (ei >= edit_spline_attachments.size()) continue;
            if (sa_cpu_by_entry.count(ei)) continue;
            if (sa_spawned_entries.count(ei)) continue;
            In in{};
            in.entry_i = ei;
            in.att = edit_spline_attachments[ei].att;
            in.parent_spline_id = edit_spline_attachments[ei].parent_spline_id;
            in.sco_path = edit_spline_attachments[ei].att.sco_path;
            ins.push_back(std::move(in));
        }
        if (ins.empty()) return;

        const std::size_t base = sa_cpu_slots.size();
        sa_cpu_slots.reserve(base + ins.size());
        for (std::size_t i = 0; i < ins.size(); ++i) {
            auto slot = std::make_unique<SaCpuSlot>();
            slot->entry_i = ins[i].entry_i;
            sa_cpu_by_entry[ins[i].entry_i] = slot.get();
            sa_cpu_slots.push_back(std::move(slot));
        }

        std::vector<map::SplineInst> splines = edit_splines;
        const auto omsi_root = cfg.omsi_root;
        sa_cpu_started = true;
        sa_cpu_done.store(false, std::memory_order_release);
        log::info("SA cpu kick n=" + std::to_string(ins.size()) + " cores=" +
                  std::to_string((std::max)(1u, std::thread::hardware_concurrency())));
        sa_cpu_thread = std::thread([ins = std::move(ins), splines = std::move(splines), omsi_root,
                                     slots = &sa_cpu_slots, base, &done = sa_cpu_done]() {
            try {
            // Serial on sa_cpu_thread only — never nest JobSystem::parallel_for here (main frame
            // also uses the pool; nested wait_idle from this thread caused uncaught EH / teardown).
            auto seq_for = [&](std::size_t n, auto&& fn) {
                for (std::size_t i = 0; i < n; ++i) fn(i);
            };

            std::vector<std::filesystem::path> scos;
            scos.reserve(ins.size());
            for (const auto& in : ins) {
                if (!in.sco_path.empty()) scos.push_back(in.sco_path);
            }
            std::sort(scos.begin(), scos.end());
            scos.erase(std::unique(scos.begin(), scos.end()), scos.end());
            seq_for(scos.size(), [&](std::size_t i) { (void)sco::load_sco(scos[i]); });

            std::vector<std::filesystem::path> o3ds;
            std::vector<std::filesystem::path> xs;
            o3ds.reserve(scos.size() * 2);
            xs.reserve(scos.size());
            for (const auto& p : scos) {
                const auto* sco = sco::load_sco(p);
                if (!sco) continue;
                for (const auto& mesh_ref : sco->meshes) {
                    if (mesh_ref.mesh_file.empty()) continue;
                    if (mesh_ref.is_o3d)
                        o3ds.push_back(mesh_ref.mesh_file);
                    else
                        xs.push_back(mesh_ref.mesh_file);
                }
            }
            std::sort(o3ds.begin(), o3ds.end());
            o3ds.erase(std::unique(o3ds.begin(), o3ds.end()), o3ds.end());
            std::sort(xs.begin(), xs.end());
            xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
            seq_for(o3ds.size(), [&](std::size_t i) { o3d::prepared_vertex_cache().ensure_path(o3ds[i]); });
            seq_for(xs.size(), [&](std::size_t i) { (void)xfile::load_x_file(xs[i]); });

            std::vector<std::filesystem::path> texs;
            std::unordered_set<std::string> seen_tex;
            auto add_tex = [&](const std::optional<std::filesystem::path>& tex) {
                if (!tex || tex->empty()) return;
                if (seen_tex.insert(tex->string()).second) texs.push_back(*tex);
            };
            for (const auto& p : scos) {
                const auto* sco = sco::load_sco(p);
                if (!sco) continue;
                for (const auto& mesh_ref : sco->meshes) {
                    if (mesh_ref.is_o3d) {
                        const auto mapped = o3d::load_mapped_shared(mesh_ref.mesh_file);
                        if (!mapped || !mapped->valid()) continue;
                        for (const auto& mat : mapped->mesh().parse_materials()) {
                            if (mat.texture.empty()) continue;
                            add_tex(gfx::dx12::resolve_texture_path(mesh_ref.mesh_file, mat.texture, omsi_root));
                        }
                    } else if (const auto xf = xfile::load_x_file(mesh_ref.mesh_file)) {
                        for (const auto& part : xf->parts) {
                            if (part.texture_filename.empty()) continue;
                            add_tex(gfx::dx12::resolve_texture_path(mesh_ref.mesh_file, part.texture_filename,
                                                                   omsi_root));
                            add_tex(gfx::dx12::resolve_texture_path(p, part.texture_filename, omsi_root));
                        }
                    }
                }
            }
            if (!texs.empty()) gfx::dx12::prefetch_textures_cpu(texs);

            for (const auto& p : scos) {
                const auto* sco = sco::load_sco(p);
                if (!sco) continue;
                for (const auto& tt : sco->text_textures) {
                    if (!tt.font_name.empty()) sco::prefetch_oft_images(tt.font_name, omsi_root);
                }
            }

            seq_for(ins.size(), [&](std::size_t i) {
                auto& slot = *(*slots)[base + i];
                const auto& in = ins[i];
                if (in.parent_spline_id <= 0) {
                    slot.ready.store(true, std::memory_order_release);
                    return;
                }
                const map::SplineInst* parent = nullptr;
                for (const auto& s : splines) {
                    if (s.id == in.parent_spline_id) {
                        parent = &s;
                        break;
                    }
                }
                if (!parent || parent->length <= 1.e-4f) {
                    slot.ready.store(true, std::memory_order_release);
                    return;
                }
                const auto along = map::enumerate_spline_attachment_along_chain(in.att, *parent, splines);
                if (along.empty()) {
                    slot.skip = true;
                    slot.ready.store(true, std::memory_order_release);
                    return;
                }
                slot.poses.reserve(along.size());
                for (const auto& hit : along) {
                    const map::SplineInst* pose_sp = parent;
                    for (const auto& s : splines) {
                        if (s.id == hit.spline_id) {
                            pose_sp = &s;
                            break;
                        }
                    }
                    SaCpuPose p{};
                    p.spline_id = hit.spline_id;
                    p.along = hit.along;
                    p.pose = map::compute_spline_attachment_pose(*pose_sp, in.att, hit.along, std::nullopt, false);
                    if (!std::isfinite(p.pose.x) || !std::isfinite(p.pose.y)) continue;
                    slot.poses.push_back(p);
                }
                slot.have_poses = true;
                slot.ready.store(true, std::memory_order_release);
            });
            } catch (const std::exception& e) {
                log::error(std::string("SA cpu thread exception: ") + e.what());
            } catch (...) {
                log::error("SA cpu thread exception: unknown");
            }
            done.store(true, std::memory_order_release);
        });
    };

    enum class SaSpawnResult { Ok, Skip, Defer };

    auto spawn_sa_entry = [&](const std::size_t entry_i) -> SaSpawnResult {
        if (entry_i >= edit_spline_attachments.size()) return SaSpawnResult::Skip;
        auto& entry = edit_spline_attachments[entry_i];
        const std::int32_t inst_id = entry.att.instance_id;
        if (inst_id > 0 && editor_deleted_object_ids.count(inst_id)) return SaSpawnResult::Skip;
        if (sa_spawned_entries.count(entry_i)) return SaSpawnResult::Ok;
        // Duplicate/orphan [splineAttachement] for an id already bound in-place — do not kill/respawn.
        if (inst_id > 0) {
            if (const auto own = spline_attach_obj_to_entry.find(inst_id);
                own != spline_attach_obj_to_entry.end() && own->second != entry_i) {
                sa_spawned_entries.insert(entry_i);
                return SaSpawnResult::Skip;
            }
        }

        SaCpuSlot* cpu = nullptr;
        if (const auto it = sa_cpu_by_entry.find(entry_i); it != sa_cpu_by_entry.end()) cpu = it->second;
        const bool cpu_ready = cpu && cpu->ready.load(std::memory_order_acquire);

        const map::SplineInst* parent_spline = nullptr;
        std::vector<SaCpuPose> local_poses;
        const std::vector<SaCpuPose>* poses = nullptr;
        if (cpu_ready && cpu->skip) {
            // Stale CPU "empty along" for a reused entry index — recompute on main thread.
            ++sa_stale_cpu_n;
            sa_cpu_by_entry.erase(entry_i);
            cpu = nullptr;
        }
        if (cpu_ready && cpu && cpu->have_poses && !cpu->poses.empty()) {
            poses = &cpu->poses;
            parent_spline = resolve_edit_spline_for_attach(entry);
            if (!parent_spline || parent_spline->length <= 1.e-4f) {
                for (const auto& s : edit_splines) {
                    if (s.id == cpu->poses.front().spline_id) {
                        parent_spline = &s;
                        break;
                    }
                }
            }
        }
        if (!poses) {
            parent_spline = resolve_edit_spline_for_attach(entry);
            if (!parent_spline || parent_spline->length <= 1.e-4f) {
                const int retries = ++sa_spawn_retry_count[entry_i];
                if (retries == 1 || retries % 60 == 0) {
                    log::warn("SA defer id=" + std::to_string(inst_id) + " tile=(" +
                              std::to_string(entry.tile_gx) + "," + std::to_string(entry.tile_gy) +
                              ") parent=" + std::to_string(entry.parent_spline_id) + " splineIdx=" +
                              std::to_string(entry.att.spline_index) + " try=" + std::to_string(retries));
                }
                // Wall-clock give-up: unresolved parent for >4 s (and >=20 tries) is not coming (spline outside the
                // loaded radius) - was 120 pump frames, i.e. ~20 entries x 120 frames of an unusable queue.
                static std::unordered_map<std::size_t, std::chrono::steady_clock::time_point> s_first_defer;
                const auto now_sa = std::chrono::steady_clock::now();
                if (retries == 1) s_first_defer[entry_i] = now_sa; // retry counter restarts after a success/give-up
                const auto fd_it = s_first_defer.try_emplace(entry_i, now_sa).first;
                const bool aged = retries >= 20 && now_sa - fd_it->second > std::chrono::seconds(4);
                if (aged || retries >= 120) {
                    s_first_defer.erase(entry_i);
                    log::warn("SA give up id=" + std::to_string(inst_id) + " sco=" +
                              entry.att.sco_path.filename().string() + " (parent spline unresolved)");
                    sa_spawn_retry_count.erase(entry_i);
                    return SaSpawnResult::Skip;
                }
                return SaSpawnResult::Defer;
            }
            auto along_list =
                map::enumerate_spline_attachment_along_chain(entry.att, *parent_spline, edit_splines);
            if (along_list.empty()) {
                // Stale parent after stream: drop cache and resolve again from .map index.
                entry.parent_spline_id = 0;
                if (const map::SplineInst* again = resolve_edit_spline_for_attach(entry)) {
                    parent_spline = again;
                    entry.parent_spline_id = again->id;
                    along_list =
                        map::enumerate_spline_attachment_along_chain(entry.att, *parent_spline, edit_splines);
                }
            }
            if (along_list.empty()) {
                // Permanent for the currently loaded spline set: mark spawned so
                // enqueue_unspawned_sa_entries / stream Finish do not re-queue this
                // entry every frame (was burning CPU + log spam with hundreds of
                // SKIP empty along). Full respawn still clears sa_spawned_entries.
                ++sa_empty_along_n;
                sa_spawned_entries.insert(entry_i);
                return SaSpawnResult::Skip;
            }
            local_poses.reserve(along_list.size());
            for (const auto& hit : along_list) {
                const map::SplineInst* pose_spline = parent_spline;
                for (const auto& s : edit_splines) {
                    if (s.id == hit.spline_id) {
                        pose_spline = &s;
                        break;
                    }
                }
                SaCpuPose p{};
                p.spline_id = hit.spline_id;
                p.along = hit.along;
                p.pose = map::compute_spline_attachment_pose(*pose_spline, entry.att, hit.along, std::nullopt,
                                                             false);
                local_poses.push_back(p);
            }
            poses = &local_poses;
        }
        if (!parent_spline || !poses || poses->empty()) return SaSpawnResult::Skip;
        entry.parent_spline_id = parent_spline->id;
        sa_spawn_retry_count.erase(entry_i);

        const sco::SceneryObject* child_sco = cached_sco(entry.att.sco_path);
        if (!child_sco || child_sco->meshes.empty()) {
            log::warn("splineAttach sco missing id=" + std::to_string(inst_id) + " " +
                      entry.att.sco_path.string());
            return SaSpawnResult::Skip;
        }
        for (const auto& mesh_ref : child_sco->meshes) {
            if (!mesh_ref.is_o3d) continue;
            const auto stem = mesh_ref.mesh_file.stem().string();
            if (stem == "empty_lod" || stem == "empty") continue;
            if (o3d::prepared_vertex_cache().ready(mesh_ref.mesh_file)) continue;
            if (o3d_gpu_proto.find(o3d_path_key(mesh_ref.mesh_file)) != o3d_gpu_proto.end()) continue;
            auto& cache = o3d::prepared_vertex_cache();
            if (cache.failed(mesh_ref.mesh_file)) {
                log::warn("SA skip id=" + std::to_string(inst_id) + " o3d failed " +
                          mesh_ref.mesh_file.filename().string());
                sa_spawned_entries.insert(entry_i);
                sa_mesh_defer_count.erase(entry_i);
                return SaSpawnResult::Skip;
            }
            const auto p = mesh_ref.mesh_file;
            omsi::JobSystem::global().submit([p]() { o3d::prepared_vertex_cache().ensure_path(p); },
                                             omsi::JobSystem::Priority::High);
            const int mesh_tries = ++sa_mesh_defer_count[entry_i];
            if (mesh_tries >= 180) {
                log::warn("SA give up id=" + std::to_string(inst_id) + " o3d not ready " +
                          mesh_ref.mesh_file.filename().string() + " try=" + std::to_string(mesh_tries));
                sa_spawned_entries.insert(entry_i);
                sa_mesh_defer_count.erase(entry_i);
                return SaSpawnResult::Skip;
            }
            return SaSpawnResult::Defer;
        }
        sa_mesh_defer_count.erase(entry_i);

        if (inst_id > 0 && spline_attach_obj_to_entry.count(inst_id)) clear_sa_spawns_for_entry(entry_i);

        bool spawned_any = false;
        std::size_t along_i = 0;
        for (const auto& hit : *poses) {
            const auto& pose = hit.pose;
            const std::int32_t oid =
                (along_i == 0 && entry.att.instance_id > 0) ? entry.att.instance_id : sa_synth_cursor++;
            ++along_i;
            if (oid + 1 > sa_synth_cursor) sa_synth_cursor = oid + 1;
            map::MapObjectInst spawned_obj{};
            spawned_obj.id = oid;
            spawned_obj.sco_path = entry.att.sco_path;
            spawned_obj.position.x = pose.x;
            spawned_obj.position.y = pose.y;
            if (entry.att.align_terrain)
                spawned_obj.position.z = sample_terrain_editor_xy(pose.x, pose.y) + entry.att.offset_y;
            else
                spawned_obj.position.z = pose.z;
            spawned_obj.rotation_deg.x = pose.yaw_deg;
            spawned_obj.rotation_deg.y = pose.pitch_deg;
            spawned_obj.rotation_deg.z = pose.bank_deg;
            spawned_obj.strings = entry.att.object_text_lines;
            // tree on a spline: its strings are texture / height / aspect, as on an [object] tree
            if (child_sco->tree.valid && !spawned_obj.strings.empty() &&
                spawned_obj.strings[0].find('.') != std::string::npos) {
                spawned_obj.obj_type = 4;
                spawned_obj.tree_texture = spawned_obj.strings[0];
                try {
                    if (spawned_obj.strings.size() > 1) spawned_obj.tree_height = std::stof(spawned_obj.strings[1]);
                    if (spawned_obj.strings.size() > 2) spawned_obj.tree_aspect = std::stof(spawned_obj.strings[2]);
                } catch (...) {
                }
            }
            spawned_obj.varparent_id = entry.att.varparent_id;
            spawned_obj.align_terrain = entry.att.align_terrain;
            // AlignTerrain here adjusts the child object's pose from the existing heightfield;
            // it does not modify terrain vertices, so do not enqueue a terrain GPU rebuild.
            if (!entry.att.align_terrain && apply_object_surface_follow_fn) {
                apply_object_surface_follow_fn(spawned_obj, true);
            }
            if (oid == entry.att.instance_id && spawn_align_pitch_bank && (std::abs(entry.att.rotation_y)+std::abs(entry.att.rotation_x)+std::abs(entry.att.rotation_z)) > 0.01f) {
                entry.att.rotation_x = spawned_obj.rotation_deg.y;
                entry.att.rotation_z = spawned_obj.rotation_deg.z;
            }
            edit_objects.push_back(spawned_obj);
            spline_attach_spawn_ids.insert(oid);
            spline_attach_obj_to_entry[oid] = entry_i;
            attach_pending = true; // [attachObj] may hang on this spline attachment instance
            const XMMATRIX child_world = sco_world_matrix(spawned_obj, nullptr, 0.f, 0.f, 0.f);
            bool gpu_ok = append_sco_meshes_at(*child_sco, entry.att.sco_path, child_world, oid, parent_spline->id,
                                               entry.tile_gx, entry.tile_gy, "[splineAttach]", true);
            if (!gpu_ok && child_sco->tree.valid) {
                spawn_one_object_gpu(spawned_obj, entry.tile_gx, entry.tile_gy);
                gpu_ok = object_has_alive_gpu(oid);
            }
            if (gpu_ok) spawned_any = true;
        }
        if (spawned_any) {
            sa_spawned_entries.insert(entry_i);
            if (inst_id == 14355)
                log::info("splineAttach watch id=14355 ok sco=" + entry.att.sco_path.filename().string() +
                          " splineId=" + std::to_string(parent_spline->id));
        } else {
            clear_sa_spawns_for_entry(entry_i);
        }
        return spawned_any ? SaSpawnResult::Ok : SaSpawnResult::Skip;
    };

    auto sa_entry_map_name = [&](const EditSplineAttachmentEntry& e) -> std::string {
        for (const auto& t : world.tiles()) {
            if (t.grid_x == e.tile_gx && t.grid_y == e.tile_gy) return t.path.filename().string();
        }
        return "tile_" + std::to_string(e.tile_gx) + "_" + std::to_string(e.tile_gy) + ".map";
    };
    auto sa_entry_log_line = [&](const std::size_t ei) -> std::string {
        if (ei >= edit_spline_attachments.size()) return "ei=?";
        const auto& e = edit_spline_attachments[ei];
        return "id=" + std::to_string(e.att.instance_id) + " map=" + sa_entry_map_name(e) +
               " sco=" + e.att.sco_path.filename().string() + " tile=(" + std::to_string(e.tile_gx) + "," +
               std::to_string(e.tile_gy) + ")";
    };

    auto enqueue_sa_spawn = [&](const std::vector<std::size_t>& indices) {
        std::size_t added = 0;
        std::size_t orphans = 0;
        std::unordered_set<std::int32_t> claimed;
        claimed.reserve(indices.size() * 2);
        for (const std::size_t ei : indices) {
            if (ei >= edit_spline_attachments.size()) continue;
            const std::int32_t inst_id = edit_spline_attachments[ei].att.instance_id;
            if (editor_deleted_object_ids.count(inst_id)) continue;
            if (inst_id > 0) {
                const auto own = spline_attach_obj_to_entry.find(inst_id);
                if (own != spline_attach_obj_to_entry.end() && own->second != ei) {
                    ++orphans;
                    sa_spawned_entries.insert(ei);
                    continue;
                }
                if (!claimed.insert(inst_id).second) {
                    ++orphans;
                    sa_spawned_entries.insert(ei);
                    continue;
                }
            }
            if (!sa_spawn_queued.insert(ei).second) continue;
            sa_spawn_queue.push_back(ei);
            ++added;
        }
        if (orphans > 0)
            log::info("SA skip orphans n=" + std::to_string(orphans));
        if (added == 0) {
            if (!sa_spawn_queue.empty()) sa_spawn_active = true;
            return;
        }
        if (!sa_spawn_active) sa_spawn_wave = 0;
        sa_spawn_wave += added;
        sa_spawn_active = true;
        log::info("SA queue +" + std::to_string(added) + " pend=" +
                  std::to_string(sa_spawn_queue.size() - sa_spawn_queue_i) +
                  (added == 1 ? " " + sa_entry_log_line(indices.front()) : ""));
    };

    auto enqueue_unspawned_sa_entries = [&]() {
        std::vector<std::size_t> pending;
        for (std::size_t i = 0; i < edit_spline_attachments.size(); ++i) {
            if (sa_spawned_entries.count(i)) continue;
            if (editor_deleted_object_ids.count(edit_spline_attachments[i].att.instance_id)) continue;
            pending.push_back(i);
        }
        enqueue_sa_spawn(pending);
    };

    auto request_sa_entries_respawn = [&](const std::vector<std::size_t>& indices) {
        sa_queue_compact();
        std::vector<std::size_t> todo;
        for (const std::size_t ei : indices) {
            if (ei >= edit_spline_attachments.size()) continue;
            clear_sa_spawns_for_entry(ei);
            sa_spawn_queued.erase(ei);
            sa_spawn_queue.erase(std::remove_if(sa_spawn_queue.begin(), sa_spawn_queue.end(),
                                                [&](std::size_t q) { return q == ei; }),
                                  sa_spawn_queue.end());
            sa_cpu_by_entry.erase(ei);
            todo.push_back(ei);
        }
        enqueue_sa_spawn(todo);
    };

    auto request_sa_entry_respawn = [&](const std::size_t entry_i) {
        request_sa_entries_respawn({entry_i});
    };

    auto request_sa_full_respawn = [&]() {
        sa_join_cpu_workers();
        for (auto& sp : scene_places) {
            if (!spline_attach_spawn_ids.count(sp.object_id)) continue;
            sp.alive = false;
            renderer.set_chunk_visible(sp.chunk, false);
        }
        edit_objects.erase(std::remove_if(edit_objects.begin(), edit_objects.end(),
                                          [&](const map::MapObjectInst& o) {
                                              return spline_attach_spawn_ids.count(o.id) != 0;
                                          }),
                           edit_objects.end());
        spline_attach_spawn_ids.clear();
        spline_attach_obj_to_entry.clear();
        sa_spawned_entries.clear();
        sa_spawn_retry_count.clear();
        sa_mesh_defer_count.clear();
        sa_spawn_queue.clear();
        sa_spawn_queue_i = 0;
        sa_spawn_queued.clear();
        sa_spawn_wave = 0;
        enqueue_unspawned_sa_entries();
    };

    auto request_sa_respawn_for_sco = [&](const std::filesystem::path& path) {
        const auto want = path.lexically_normal().string();
        std::vector<std::size_t> indices;
        for (std::size_t i = 0; i < edit_spline_attachments.size(); ++i) {
            if (edit_spline_attachments[i].att.sco_path.lexically_normal().string() != want) continue;
            indices.push_back(i);
        }
        if (!indices.empty()) request_sa_entries_respawn(indices);
    };

    bool sa_scripts_dirty = false;
    auto sa_scripts_last_sync = std::chrono::steady_clock::now();
    auto pump_sa_spawn = [&](const double slice_ms) {
        // PrepLists moves edit_objects/splines/attachments to the worker. Pumping here
        // would prune the whole queue (size==0) or spawn into a vector the join overwrites.
        if (sa_pause_lists) return;
        if (!sa_spawn_active || sa_spawn_queue_i >= sa_spawn_queue.size()) {
            sa_queue_compact();
            if (sa_spawn_queue.empty()) {
                sa_spawn_active = false;
                sa_spawn_queued.clear();
                return;
            }
        }
        {
            std::unordered_set<std::int64_t> loaded;
            loaded.reserve(world.tiles().size() * 2);
            for (const auto& t : world.tiles()) {
                loaded.insert((static_cast<std::int64_t>(t.grid_x) << 32) ^
                              static_cast<std::uint32_t>(t.grid_y));
            }
            sa_queue_compact();
            const auto before = sa_spawn_queue.size();
            sa_spawn_queue.erase(
                std::remove_if(sa_spawn_queue.begin(), sa_spawn_queue.end(),
                               [&](const std::size_t ei) {
                                   if (ei >= edit_spline_attachments.size()) {
                                       sa_spawn_queued.erase(ei);
                                       return true;
                                   }
                                   const auto& e = edit_spline_attachments[ei];
                                   const auto key = (static_cast<std::int64_t>(e.tile_gx) << 32) ^
                                                    static_cast<std::uint32_t>(e.tile_gy);
                                   if (loaded.count(key) == 0) {
                                       sa_spawn_queued.erase(ei);
                                       return true;
                                   }
                                   return false;
                               }),
                sa_spawn_queue.end());
            if (sa_spawn_queue.size() != before) {
                const auto pruned = before - sa_spawn_queue.size();
                if (sa_spawn_wave > pruned) sa_spawn_wave -= pruned;
                else sa_spawn_wave = sa_spawn_queue.size();
                log::info("SA queue pruned unloaded tiles " + std::to_string(before) + "→" +
                          std::to_string(sa_spawn_queue.size()));
            }
            if (sa_spawn_queue.empty()) {
                sa_spawn_active = false;
                sa_spawn_queued.clear();
                return;
            }
        }
        // Defer first pump so async remesh + DXGI settle (avoids loader pool races on frame 0).
        static std::atomic<int> s_sa_defer_pumps{1};
        if (s_sa_defer_pumps.fetch_sub(1) >= 0) return;
        sa_kick_cpu();
        // Do not GPU-spawn while the SA CPU prefetch thread is still loading .sco/.o3d and
        // writing sa_cpu_slots — parallel spawn_sa_entry on the UI thread caused heap races
        // (process exit 0xC0000409 right after "SA pump", often with no error line).
        if (sa_cpu_started && !sa_cpu_done.load(std::memory_order_acquire)) {
            o3d_defer_if_unready = false;
            return;
        }
        sa_synth_cursor = compute_sa_synth_object_id();
        o3d_defer_if_unready = true;
        // Wall-clock only (caller derives slice from measured frame time). First entry always runs.
        const auto budget_end = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double, std::milli>((std::max)(0.5, slice_ms)));
        int ok = 0, skip = 0, defer = 0;
        // One entry waiting for its parent spline used to `break` the whole pump, so a queue of 20 deferred
        // entries was served 1/frame and "Cargando attachments" lasted ~minutes. Now every entry is tried once per
        // pump (deferred ones go to the back); the pump ends when each entry was visited or the slice is spent.
        const std::size_t pass_n = sa_spawn_queue.size() - sa_spawn_queue_i;
        std::size_t visited = 0;
        while (visited < pass_n && sa_spawn_queue_i < sa_spawn_queue.size() &&
               ((ok + skip == 0 && defer < 48) || std::chrono::steady_clock::now() < budget_end)) {
            ++visited;
            const std::size_t entry_i = sa_spawn_queue[sa_spawn_queue_i++];
            sa_spawn_queued.erase(entry_i);
            const auto r = spawn_sa_entry(entry_i);
            if (r == SaSpawnResult::Defer) {
                sa_spawn_queue.push_back(entry_i);
                sa_spawn_queued.insert(entry_i);
                ++defer;
                continue;
            }
            if (r == SaSpawnResult::Ok)
                ++ok;
            else
                ++skip;
        }
        sa_queue_compact();
        sa_spawn_active = !sa_spawn_queue.empty();
        if (!sa_spawn_active) sa_spawn_queued.clear();
        if (ok > 0) sa_scripts_dirty = true;
        // Log at most ~2/s when queue is large (console was filling and truncating).
        static auto s_last_sa_log = std::chrono::steady_clock::time_point{};
        const auto now_log = std::chrono::steady_clock::now();
        const bool log_now = (ok > 0 || skip > 0 || defer > 0) &&
                             (sa_spawn_queue.empty() ||
                              now_log - s_last_sa_log > std::chrono::milliseconds(500));
        if (log_now) {
            s_last_sa_log = now_log;
            log::info("SA pump ok=" + std::to_string(ok) + " skip=" + std::to_string(skip) +
                      (defer > 0 ? " defer=" + std::to_string(defer) : "") +
                      " left=" + std::to_string(sa_spawn_queue.size()));
        }
        o3d_defer_if_unready = false;
    };

    auto respawn_spline_attachment_objects = [&]() { request_sa_full_respawn(); };

    auto audit_sa_watch = [&](const std::int32_t watch_id, const std::string_view tag) {
        if (watch_id <= 0) return;
        bool has_entry = false;
        std::size_t entry_i = 0;
        std::int32_t parent_id = 0;
        for (std::size_t i = 0; i < edit_spline_attachments.size(); ++i) {
            if (edit_spline_attachments[i].att.instance_id != watch_id) continue;
            has_entry = true;
            entry_i = i;
            parent_id = edit_spline_attachments[i].parent_spline_id;
            break;
        }
        bool spawned_flag = has_entry && sa_spawned_entries.count(entry_i) != 0;
        bool in_edit_objects = false;
        bool gpu_visible = false;
        float yaw_deg = 0.f;
        float rot_y = 0.f;
        if (has_entry) rot_y = edit_spline_attachments[entry_i].att.rotation_y;
        for (const auto& o : edit_objects) {
            if (o.id != watch_id) continue;
            in_edit_objects = true;
            yaw_deg = o.rotation_deg.x;
            break;
        }
        for (const auto& sp : scene_places) {
            if (sp.object_id != watch_id || !sp.alive) continue;
            gpu_visible = true;
            break;
        }
        log::info(std::string("saAudit tag=") + std::string(tag) + " id=" + std::to_string(watch_id) +
                  " entry=" + (has_entry ? "1" : "0") + " entry_i=" + std::to_string(entry_i) +
                  " parentSplineId=" + std::to_string(parent_id) + " spawned=" +
                  (spawned_flag ? "1" : "0") + " editObj=" + (in_edit_objects ? "1" : "0") + " gpu=" +
                  (gpu_visible ? "1" : "0") + " yaw=" + std::to_string(yaw_deg) + " rotY=" +
                  std::to_string(rot_y) + " saQueue=" + std::to_string(sa_spawn_queue.size()));
    };
