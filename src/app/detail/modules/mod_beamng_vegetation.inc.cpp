// BeamNG vegetation tools: trees (Level::forest / forest/*.forest4.json) and grass (GroundCover + the terrain
// layer each cover type is painted on).
//
// New instances are appended to Level::forest and shown immediately through bng_show_inst() (with the async
// AssetPool request when the shape is still pending), so the paint brush is WYSIWYG. Deleting only sets
// ForestInstance::deleted (indices stay stable, so undo/redo and the spatial hash of the viewport pick never
// dangle). Every change bumps s_forest_grid_ver (mod_beamng_setup.inc.cpp) so the CPU pick grid is rebuilt.
//
// Saving rewrites the level's forest/*.forest4.json, but inside the editor workspace (the game folder is never
// touched): <Session.workspace>/forest/<file>.forest4.json, with the previous content kept as <file>.bak.
// Instances added by the editor go to forest/vegetation_edit.forest4.json. Level::forest_files_dirty is marked
// so the rest of the editor knows the forest folder changed.
//
// This file is included twice from the same function body (the editor's run loop):
//   mod_beamng_setup.inc.cpp : BNG_VEGETATION_SECTION_SETUP -> state, helpers and the "Vegetacion" tab body
//   mod_beamng_frame.inc.cpp : BNG_VEGETATION_SECTION_FRAME -> per-frame pick status + brush / selection input

#if defined(BNG_VEGETATION_SECTION_SETUP)
#undef BNG_VEGETATION_SECTION_SETUP

// ── state ─────────────────────────────────────────────────────────────────────────────────────────────────
// modes: 0 seleccionar, 1 pintar arboles, 2 borrar, 3 mover/rotar/escalar, 4 pintar pasto
struct BngVegEdit {
    int mode = 0;
    // selection (indices into Level::forest, always ascending)
    std::vector<std::uint32_t> sel;
    int sel_primary = -1;
    // paint trees
    std::vector<float> type_weight;   // per forest type: 0 = not painted, else relative weight
    float paint_radius = 18.f;        // brush radius, metres
    float paint_density = 0.02f;      // instances per m2 inside the brush
    float paint_min_sep = 2.0f;       // minimum separation, metres
    float scale_min = 0.8f, scale_max = 1.3f;
    bool rot_random = true;
    bool align_normal = false;        // align to the terrain normal (needs align_deg_max > 0)
    float align_deg_max = 12.f;
    bool use_surface = true;          // real surface (pick_surface_hit_z) instead of the bilinear heightfield
    // erase
    int erase_type = -1;              // -1 = every type
    float erase_radius = 10.f;
    // transform (mover / rotar / escalar)
    float move_x = 0.f, move_y = 0.f, move_z = 0.f, rot_z = 0.f, scale_mul = 1.f;
    // paint grass: the GroundCover reads the terrain layer, so "pintar pasto" paints that layer
    int gc_cover = 0;
    int gc_layer = -1;                // -1 = first layer of the cover types (resolved on use)
    float gc_radius = 25.f, gc_strength = 0.85f, gc_falloff = 0.4f;
    bool gc_raise = true;             // true = paint the layer (grass grows), false = erase it (base layer 0)
    // stroke bookkeeping
    bool stroke = false;
    std::uint32_t stroke_mode = 0;
    std::vector<float> stroke_pts;    // x,y pairs painted in this stroke (separation test)
    float terrain_tool_bak = 0.f, terrain_radius_bak = 0.f, terrain_strength_bak = 0.f, terrain_falloff_bak = 0.f;
    int terrain_layer_bak = 0;
    bool terrain_bak = false;
    // separacion / borrado: hash propio (indices de Level::forest), reconstruido al empezar cada trazo
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> sep;
    // undo / redo (one batch per stroke / per numeric transform)
    struct Snap {
        std::uint32_t idx = 0;
        float pos[3]{0.f, 0.f, 0.f};
        bng::Mat3 rot{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
        float scale = 1.f;
        int type = -1;
        bool deleted = false;
    };
    struct Batch {
        std::vector<Snap> before, after;
        std::size_t size_before = 0, size_after = 0;
        std::string what;
    };
    std::vector<Batch> undo, redo;
    int undo_cap = 64;
    // save
    bool modified = false;
    std::string saved_path;
    std::string status;
};
BngVegEdit bng_veg;
// True while LMB belongs to a vegetation brush: the pick / marquee code of the loops checks it (same idea as
// bng_ter_owns_lmb) so a stroke never selects or drags objects.
bool bng_veg_owns_lmb = false;
// Number of forest instances that came from disk (everything after it was added by the editor).
std::size_t bng_veg_loaded = 0;
if (bng_rt) {
    const auto& L0 = *bng_rt->s->level;
    bng_veg_loaded = L0.forest.size();
    bng_veg.type_weight.assign(L0.forest_types.size(), 1.f);
}

// Snapshot / restore one instance (also used to bring back an appended one on redo).
auto bng_veg_snap = [](std::uint32_t i, const bng::Level& L) -> BngVegEdit::Snap {
    BngVegEdit::Snap s{};
    s.idx = i;
    if (i >= L.forest.size()) return s;
    const auto& fi = L.forest[i];
    s.pos[0] = fi.pos[0];
    s.pos[1] = fi.pos[1];
    s.pos[2] = fi.pos[2];
    s.rot = fi.rot;
    s.scale = fi.scale;
    s.type = fi.type;
    s.deleted = fi.deleted;
    return s;
};
// Show / hide one instance right now (never waits for the streaming pump).
auto bng_veg_refresh = [&](std::uint32_t idx) {
    if (!bng_rt) return;
    auto& Rv = *bng_rt;
    auto& L = *Rv.s->level;
    ++s_forest_grid_ver; // the CPU pick grid is stale from now on
    if (idx >= L.forest.size()) return;
    const auto& fi = L.forest[static_cast<std::size_t>(idx)];
    auto it = Rv.forest_chunk.find(idx);
    if (fi.deleted || fi.type < 0 || static_cast<std::size_t>(fi.type) >= L.forest_types.size()) {
        if (it != Rv.forest_chunk.end()) {
            bng_hide_inst(it->second);
            Rv.forest_chunk.erase(it);
        }
        Rv.forest_spawned.erase(idx);
        return;
    }
    const auto& ty = L.forest_types[static_cast<std::size_t>(fi.type)];
    if (ty.shape_file.empty()) return;
    auto& g = Rv.shapes[ty.shape_file];
    if (g.failed) return;
    if (!g.ready()) {
        Rv.pool->request(ty.shape_file, 40.f);
        auto& w = Rv.forest_waiting[ty.shape_file];
        if (std::find(w.begin(), w.end(), idx) == w.end()) w.push_back(idx);
        Rv.forest_spawned.insert(idx);
        return;
    }
    float m16[16];
    const float sc[3]{fi.scale, fi.scale, fi.scale};
    bng::mat3_to_world4x4(fi.rot, sc, fi.pos, m16);
    auto& inst = Rv.forest_chunk[idx];
    const float d = std::hypot(fi.pos[0] - camera.target_x, fi.pos[1] - camera.target_y);
    bng_show_inst(inst, g, bng_pick_lod(g, fi.scale, d, inst.cur, &inst), m16);
    Rv.forest_spawned.insert(idx);
};
// Ground height at (x, y): the drawn surface when use_surface is on, the heightfield otherwise.
auto bng_veg_ground_z = [&](float x, float y, bool* ok) -> float {
    if (ok) *ok = true;
    if (bng_veg.use_surface) {
        if (const auto z = camera.sample_ground_xy(x, y)) return *z;
        if (ok) *ok = false;
        return 0.f;
    }
    if (const auto z = bng_terrain_sample(x, y)) return *z;
    if (ok) *ok = false;
    return 0.f;
};
// Undo / redo of a whole stroke or transform.
auto bng_veg_apply_batch = [&](const BngVegEdit::Batch& b, bool after) {
    if (!bng_rt) return;
    auto& L = *bng_rt->s->level;
    const auto& snaps = after ? b.after : b.before;
    const std::size_t want = after ? b.size_after : b.size_before;
    for (const auto& s : snaps) {
        if (s.idx < L.forest.size() && static_cast<std::size_t>(s.idx) >= want) continue; // will be dropped
        while (L.forest.size() <= s.idx) {
            bng::ForestInstance fi;
            fi.id = static_cast<std::uint32_t>(L.forest.size());
            L.forest.push_back(fi);
        }
        auto& fi = L.forest[s.idx];
        fi.pos[0] = s.pos[0];
        fi.pos[1] = s.pos[1];
        fi.pos[2] = s.pos[2];
        fi.rot = s.rot;
        fi.scale = s.scale;
        fi.type = s.type;
        fi.deleted = s.deleted;
        bng_veg_refresh(s.idx);
    }
    if (L.forest.size() > want) {
        for (std::size_t i = want; i < L.forest.size(); ++i) {
            if (auto it = bng_rt->forest_chunk.find(static_cast<std::uint32_t>(i)); it != bng_rt->forest_chunk.end()) {
                bng_hide_inst(it->second);
                bng_rt->forest_chunk.erase(it);
            }
            bng_rt->forest_spawned.erase(static_cast<std::uint32_t>(i));
        }
        L.forest.resize(want);
        ++s_forest_grid_ver;
    }
    bng_veg.modified = true;
    const std::string f = L.info.dir + "/forest/";
    if (std::find(L.forest_files_dirty.begin(), L.forest_files_dirty.end(), f) == L.forest_files_dirty.end())
        L.forest_files_dirty.push_back(f);
};
auto bng_veg_undo = [&]() -> bool {
    if (bng_veg.undo.empty()) return false;
    auto b = std::move(bng_veg.undo.back());
    bng_veg.undo.pop_back();
    bng_veg_apply_batch(b, false);
    bng_veg.status = "deshecho: " + b.what;
    bng_veg.redo.push_back(std::move(b));
    return true;
};
auto bng_veg_redo = [&]() -> bool {
    if (bng_veg.redo.empty()) return false;
    auto b = std::move(bng_veg.redo.back());
    bng_veg.redo.pop_back();
    bng_veg_apply_batch(b, true);
    bng_veg.status = "rehecho: " + b.what;
    bng_veg.undo.push_back(std::move(b));
    return true;
};
auto bng_veg_push_batch = [&](BngVegEdit::Batch& b) {
    bng_veg.undo.push_back(std::move(b));
    if (static_cast<int>(bng_veg.undo.size()) > bng_veg.undo_cap) bng_veg.undo.erase(bng_veg.undo.begin());
    bng_veg.redo.clear();
    bng_veg.modified = true;
};
// Random yaw (rotationMatrix is the row-major pose, same convention as the level parser).
auto bng_veg_rand_rot = [](std::mt19937& rng, float yaw, float tilt_deg, float nx, float ny) -> bng::Mat3 {
    float c = std::cos(yaw), s = std::sin(yaw);
    bng::Mat3 m{c, -s, 0.f, s, c, 0.f, 0.f, 0.f, 1.f};
    if (tilt_deg > 0.01f) {
        // tilt around the horizontal axis perpendicular to the slope direction (n = surface normal)
        const float t = tilt_deg * 3.14159265f / 180.f;
        const float ax = -ny, ay = nx; // horizontal axis of the slope
        const float len = std::hypot(ax, ay);
        if (len > 1e-3f) {
            const float ux = ax / len, uy = ay / len;
            const float ct = std::cos(t), st = std::sin(t);
            const bng::Mat3 r{ct + ux * ux * (1.f - ct), ux * uy * (1.f - ct), uy * st,
                              uy * ux * (1.f - ct), ct + uy * uy * (1.f - ct), -ux * st,
                              -uy * st, ux * st, ct};
            bng::Mat3 o{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    float acc = 0.f;
                    for (int k = 0; k < 3; ++k) acc += r[i * 3 + k] * m[k * 3 + j];
                    o[i * 3 + j] = acc;
                }
            return o;
        }
    }
    (void)rng;
    return m;
};
// Picks a type by weight (index into Level::forest_types, -1 when every weight is 0).
auto bng_veg_pick_type = [](BngVegEdit& st, std::mt19937& rng, std::size_t n_types) -> int {
    float sum = 0.f;
    for (std::size_t i = 0; i < st.type_weight.size() && i < n_types; ++i) sum += (std::max)(0.f, st.type_weight[i]);
    if (!(sum > 0.f)) return -1;
    std::uniform_real_distribution<float> u(0.f, sum);
    float r = u(rng);
    for (std::size_t i = 0; i < st.type_weight.size() && i < n_types; ++i) {
        r -= (std::max)(0.f, st.type_weight[i]);
        if (r <= 0.f) return static_cast<int>(i);
    }
    return -1;
};

// ── save ──────────────────────────────────────────────────────────────────────────────────────────────────
// Rewrites <workspace>/forest/*.forest4.json (never the game folder). The loader pushed the instances in the
// order of Vfs::list(<level>/forest, ".forest4.json") + the row order inside each file, so the file ranges are
// rebuilt the same way; appended instances go to forest/vegetation_edit.forest4.json.
auto bng_veg_save = [&](bool report) -> bool {
    if (!bng_rt) return false;
    auto& L = *bng_rt->s->level;
    auto fail = [&](const std::string& why) {
        bng_veg.status = "guardado: " + why;
        if (report) panel.set_status("Vegetacion: " + why);
        log::warn("beamng vegetation save: " + why);
        return false;
    };
    std::filesystem::path root = cfg.beamng ? cfg.beamng->workspace : std::filesystem::path();
    if (root.empty()) {
        const std::string lvlname = cfg.beamng ? cfg.beamng->info.name : std::string("level");
        root = std::filesystem::current_path() / "beamng_workspace" / lvlname;
    }
    auto row_of = [&](const bng::ForestInstance& fi) -> bng::JsonValue {
        bng::JsonValue row = bng::JsonValue::make_object();
        const std::string tname =
            (fi.type >= 0 && static_cast<std::size_t>(fi.type) < L.forest_types.size())
                ? L.forest_types[static_cast<std::size_t>(fi.type)].name
                : std::string("unknown");
        row.set("type", bng::JsonValue::make_string(tname));
        bng::JsonValue pos = bng::JsonValue::make_array();
        pos.push_back(bng::JsonValue::make_number(static_cast<double>(fi.pos[0])));
        pos.push_back(bng::JsonValue::make_number(static_cast<double>(fi.pos[1])));
        pos.push_back(bng::JsonValue::make_number(static_cast<double>(fi.pos[2])));
        row.set("pos", std::move(pos));
        bng::JsonValue rm = bng::JsonValue::make_array();
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) // forest4 stores the rotation column-major
                rm.push_back(bng::JsonValue::make_number(static_cast<double>(fi.rot[static_cast<std::size_t>(r * 3 + c)])));
        row.set("rotationMatrix", std::move(rm));
        row.set("scale", bng::JsonValue::make_number(static_cast<double>(fi.scale)));
        row.set("ctxid", bng::JsonValue::make_number(static_cast<double>(fi.ctxid)));
        return row;
    };
    auto write_file = [&](const std::filesystem::path& dest, std::size_t i0, std::size_t i1) -> bool {
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) return false;
        if (std::filesystem::is_regular_file(dest, ec)) {
            std::filesystem::path bak = dest;
            bak += ".bak";
            std::filesystem::copy_file(dest, bak, std::filesystem::copy_options::overwrite_existing, ec);
        }
        std::string text;
        for (std::size_t i = i0; i < i1 && i < L.forest.size(); ++i) {
            const auto& fi = L.forest[i];
            if (fi.deleted) continue;
            text += bng::json_write_compact(row_of(fi));
            text += "\n";
        }
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        return static_cast<bool>(out);
    };
    std::vector<std::string> files;
    if (bng_rt->s->vfs) files = bng_rt->s->vfs->list(L.info.dir + "/forest/", ".forest4.json");
    std::size_t cursor = 0;
    for (const auto& f : files) {
        std::size_t rows = 0;
        if (const auto t = bng_rt->s->vfs->read_text(f)) rows = bng::json_parse_lines(*t).size();
        if (rows == 0) continue;
        const std::filesystem::path dest = root / "forest" / std::filesystem::path(f).filename();
        if (!write_file(dest, cursor, cursor + rows)) return fail("no se pudo escribir " + dest.string());
        cursor += rows;
    }
    // Instances the editor added after the ones that came from disk.
    const std::size_t first_new = (std::min)(cursor == 0 ? bng_veg_loaded : cursor, L.forest.size());
    if (L.forest.size() > first_new) {
        const std::filesystem::path dest = root / "forest" / "vegetation_edit.forest4.json";
        if (!write_file(dest, first_new, L.forest.size())) return fail("no se pudo escribir " + dest.string());
    }
    bng_veg.saved_path = (root / "forest").string();
    bng_veg.modified = false;
    const std::string folder = L.info.dir + "/forest/";
    if (std::find(L.forest_files_dirty.begin(), L.forest_files_dirty.end(), folder) == L.forest_files_dirty.end())
        L.forest_files_dirty.push_back(folder);
    bng_veg.status = "guardado en " + bng_veg.saved_path;
    if (report)
        panel.set_status("Vegetacion guardada: " + bng_veg.saved_path +
                         " (arboles " + std::to_string(cursor) + " + nuevos " +
                         std::to_string(L.forest.size() - first_new) + ")");
    log::info("beamng vegetation saved: " + bng_veg.saved_path);
    return true;
};

// ── aplica mover / rotar / escalar a la seleccion (un lote de undo por pulsacion) ───────────────────────
auto bng_veg_apply_transform = [&]() {
    if (!bng_rt) return;
    auto& L = *bng_rt->s->level;
    if (bng_veg.sel.empty()) return;
    BngVegEdit::Batch b;
    b.what = "mover/rotar/escalar";
    b.size_before = b.size_after = L.forest.size();
    double cx_s = 0.0, cy_s = 0.0;
    for (const auto i : bng_veg.sel) {
        if (i >= L.forest.size()) continue;
        cx_s += L.forest[i].pos[0];
        cy_s += L.forest[i].pos[1];
    }
    const double n = static_cast<double>((std::max)(std::size_t(1), bng_veg.sel.size()));
    const float pcx = static_cast<float>(cx_s / n), pcy = static_cast<float>(cy_s / n);
    const float rad = bng_veg.rot_z * 3.14159265f / 180.f;
    const float ca = std::cos(rad), sa = std::sin(rad);
    for (const auto i : bng_veg.sel) {
        if (i >= L.forest.size()) continue;
        b.before.push_back(bng_veg_snap(i, L));
        auto& fi = L.forest[i];
        const float rx = fi.pos[0] - pcx, ry = fi.pos[1] - pcy;
        fi.pos[0] = pcx + rx * ca - ry * sa + bng_veg.move_x;
        fi.pos[1] = pcy + rx * sa + ry * ca + bng_veg.move_y;
        fi.pos[2] += bng_veg.move_z;
        if (std::fabs(bng_veg.rot_z) > 1e-4f) {
            const bng::Mat3 rz{ca, -sa, 0.f, sa, ca, 0.f, 0.f, 0.f, 1.f};
            bng::Mat3 o{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    float acc = 0.f;
                    for (int k = 0; k < 3; ++k) acc += rz[static_cast<std::size_t>(r * 3 + k)] * fi.rot[static_cast<std::size_t>(k * 3 + c)];
                    o[static_cast<std::size_t>(r * 3 + c)] = acc;
                }
            fi.rot = o;
        }
        if (std::fabs(bng_veg.scale_mul - 1.f) > 1e-4f && bng_veg.scale_mul > 0.01f) fi.scale *= bng_veg.scale_mul;
        b.after.push_back(bng_veg_snap(i, L));
        bng_veg_refresh(i);
    }
    bng_veg_push_batch(b);
    bng_veg.status = "seleccion transformada (" + std::to_string(bng_veg.sel.size()) + ")";
    if (bng_select_forest_hook) bng_select_forest_hook(bng_veg.sel_primary >= 0 ? bng_veg.sel_primary : -1);
};

// ── pestana "Vegetacion" ─────────────────────────────────────────────────────────────────────────────────
bng_ui_vegetation_tab = [&]() {
    if (!bng_rt) return;
    auto& L = *bng_rt->s->level;
    auto& st = bng_veg;
    ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.55f, 1.f), "Vegetacion: arboles del forest y pasto del GroundCover");
    ImGui::Separator();
    ImGui::Text("instancias %u  |  tipos %u  |  pasto %u", static_cast<unsigned>(L.forest.size()),
                static_cast<unsigned>(L.forest_types.size()), static_cast<unsigned>(bng_rt->ground_covers.size()));
    ImGui::Text("seleccionadas: %u", static_cast<unsigned>(st.sel.size()));
    if (view_only) {
        ImGui::TextDisabled("Sesion en modo solo lectura (view_only): no se puede editar.");
        return;
    }
    static const char* kModes[] = {"Seleccionar", "Pintar arboles", "Borrar", "Mover/Rotar/Escalar",
                                   "Pintar pasto"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##bng_veg_mode", &st.mode, kModes, 5)) {
        bng_veg_owns_lmb = false;
        st.status = std::string("modo: ") + kModes[st.mode];
    }
    ImGui::Separator();
    if (st.mode == 0 || st.mode == 3) {
        ImGui::TextWrapped("Seleccionar: click en un arbol, arrastrar para caja, Shift anade y Ctrl quita.");
        const float half = (ImGui::GetContentRegionAvail().x - 4.f) * 0.5f;
        if (ImGui::Button("Todo##veg_selall", ImVec2(half, 0.f))) {
            st.sel.clear();
            for (std::uint32_t i = 0; i < L.forest.size(); ++i)
                if (!L.forest[i].deleted) st.sel.push_back(i);
            st.sel_primary = st.sel.empty() ? -1 : static_cast<int>(st.sel.front());
            if (bng_select_forest_hook) bng_select_forest_hook(st.sel_primary);
        }
        ImGui::SameLine();
        if (ImGui::Button("Nada##veg_selnone", ImVec2(half, 0.f))) {
            st.sel.clear();
            st.sel_primary = -1;
            if (bng_select_forest_hook) bng_select_forest_hook(-1);
        }
        if (st.mode == 3) {
            ImGui::Separator();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##veg_mx", &st.move_x, 0.05f, -5000.f, 5000.f, "mover X %.2f m");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##veg_my", &st.move_y, 0.05f, -5000.f, 5000.f, "mover Y %.2f m");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##veg_mz", &st.move_z, 0.05f, -500.f, 500.f, "mover Z %.2f m");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##veg_rz", &st.rot_z, 0.25f, -360.f, 360.f, "rotar (Z) %.2f deg");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##veg_sm", &st.scale_mul, 0.005f, 0.05f, 5.f, "escalar x%.3f");
            ImGui::BeginDisabled(st.sel.empty());
            if (ImGui::Button("Aplicar a la seleccion", ImVec2(-FLT_MIN, 0.f))) {
                bng_veg_apply_transform();
                st.move_x = st.move_y = st.move_z = st.rot_z = 0.f;
                st.scale_mul = 1.f;
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Gizmo: los campos se aplican de golpe (un paso de deshacer por pulsacion).");
        }
    } else if (st.mode == 1) {
        ImGui::TextWrapped("Pintar arboles: arrastra con LMB. El circulo es el pincel.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##veg_r", &st.paint_radius, 1.f, 120.f, "radio %.1f m");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##veg_dens", &st.paint_density, 0.001f, 0.4f, "densidad %.3f arboles/m2",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##veg_sep", &st.paint_min_sep, 0.3f, 20.f, "separacion minima %.2f m");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloatRange2("##veg_sc", &st.scale_min, &st.scale_max, 0.01f, 0.05f, 4.f, "escala min %.2f",
                               "max %.2f");
        ImGui::Checkbox("rotacion aleatoria", &st.rot_random);
        ImGui::SameLine();
        ImGui::Checkbox("altura real", &st.use_surface);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Si: la altura sale de renderer.pick_surface_hit_z (terreno dibujado, carreteras, objetos).\n"
                              "No: solo la altura del heightfield del .ter.");
        ImGui::Checkbox("alinear a la normal", &st.align_normal);
        if (st.align_normal) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##veg_al", &st.align_deg_max, 0.f, 45.f, "inclinacion max %.1f deg");
        }
        ImGui::Separator();
        ImGui::TextDisabled("tipos a pintar (peso 0 = no aparece):");
        if (st.type_weight.size() < L.forest_types.size()) st.type_weight.resize(L.forest_types.size(), 1.f);
        if (ImGui::BeginChild("##veg_types", ImVec2(0.f, 120.f), ImGuiChildFlags_None)) {
            for (std::size_t t = 0; t < L.forest_types.size() && t < st.type_weight.size(); ++t) {
                bool on = st.type_weight[t] > 0.f;
                const std::string lbl = (L.forest_types[t].name.empty() ? std::string("tipo ") + std::to_string(t)
                                                                        : L.forest_types[t].name) +
                                        "##veg_t" + std::to_string(t);
                if (ImGui::Checkbox(lbl.c_str(), &on)) st.type_weight[t] = on ? 1.f : 0.f;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.f);
                ImGui::SetNextItemWidth(90.f);
                float w = st.type_weight[t] > 0.f ? st.type_weight[t] : 0.f;
                if (ImGui::DragFloat(("##veg_w" + std::to_string(t)).c_str(), &w, 0.05f, 0.f, 10.f, "%.2f"))
                    st.type_weight[t] = (std::max)(0.f, w);
            }
        }
        ImGui::EndChild();
    } else if (st.mode == 2) {
        ImGui::TextWrapped("Borrar: arrastra con LMB. Borra por radio y, opcionalmente, por tipo.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##veg_er", &st.erase_radius, 1.f, 120.f, "radio de borrado %.1f m");
        std::string cur = "(todos los tipos)";
        if (st.erase_type >= 0 && static_cast<std::size_t>(st.erase_type) < L.forest_types.size())
            cur = L.forest_types[static_cast<std::size_t>(st.erase_type)].name;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##veg_etype", cur.c_str())) {
            if (ImGui::Selectable("(todos los tipos)", st.erase_type < 0)) st.erase_type = -1;
            for (std::size_t t = 0; t < L.forest_types.size(); ++t)
                if (ImGui::Selectable(L.forest_types[t].name.c_str(), st.erase_type == static_cast<int>(t)))
                    st.erase_type = static_cast<int>(t);
            ImGui::EndCombo();
        }
    } else if (st.mode == 4) {
        ImGui::TextWrapped("Pintar pasto: el GroundCover solo crece donde la capa del terreno coincide con la del "
                           "tipo de pasto, asi que esto pinta esa capa con el pincel de terreno (mod_beamng_terrain).");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##veg_gr", &st.gc_radius, 1.f, 200.f, "radio %.1f m");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##veg_gs", &st.gc_strength, 0.05f, 1.f, "fuerza %.2f");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##veg_gf", &st.gc_falloff, 0.f, 1.f, "caida %.2f");
        if (bng_rt->ground_covers.empty()) {
            ImGui::TextDisabled("Este nivel no define GroundCover.");
        } else {
            std::string line = "#" + std::to_string(st.gc_cover);
            if (static_cast<std::size_t>(st.gc_cover) < bng_rt->ground_covers.size()) {
                const auto& gc = bng_rt->ground_covers[static_cast<std::size_t>(st.gc_cover)];
                line += std::filesystem::path(gc.texture).filename().string();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##veg_gcc", line.c_str())) {
                for (std::size_t gi = 0; gi < bng_rt->ground_covers.size(); ++gi)
                    if (ImGui::Selectable(("#" + std::to_string(gi) + " " +
                                           std::filesystem::path(bng_rt->ground_covers[gi].texture).filename().string())
                                              .c_str(),
                                          st.gc_cover == static_cast<int>(gi)))
                        st.gc_cover = static_cast<int>(gi);
                ImGui::EndCombo();
            }
            auto* T = bng_ter_terrain();
            if (T) {
                // capas usadas por este cover (las que le hacen crecer pasto)
                std::vector<int> layers;
                if (static_cast<std::size_t>(st.gc_cover) < bng_rt->ground_covers.size())
                    for (const auto& ty : bng_rt->ground_covers[static_cast<std::size_t>(st.gc_cover)].types)
                        if (ty.layer >= 0 && std::find(layers.begin(), layers.end(), ty.layer) == layers.end())
                            layers.push_back(ty.layer);
                std::string lname = st.gc_layer < 0 ? std::string("(auto)")
                                   : (static_cast<std::size_t>(st.gc_layer) < T->material_names.size()
                                          ? T->material_names[static_cast<std::size_t>(st.gc_layer)]
                                          : std::string("capa ") + std::to_string(st.gc_layer));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##veg_glayer", lname.c_str())) {
                    if (ImGui::Selectable("(auto: la del tipo de pasto)", st.gc_layer < 0)) st.gc_layer = -1;
                    for (std::size_t li = 0; li < T->material_names.size(); ++li) {
                        const bool used = std::find(layers.begin(), layers.end(), static_cast<int>(li)) != layers.end();
                        const std::string opt = T->material_names[li] + (used ? "  (pasto)" : "");
                        if (ImGui::Selectable(opt.c_str(), st.gc_layer == static_cast<int>(li)))
                            st.gc_layer = static_cast<int>(li);
                    }
                    ImGui::EndCombo();
                }
            }
        }
        if (ImGui::RadioButton("aumentar densidad", st.gc_raise)) st.gc_raise = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("reducir (borra la capa)", !st.gc_raise)) st.gc_raise = false;
        ImGui::TextDisabled("Al terminar el trazo se regeneran las celdas de 32 m afectadas.");
    }
    ImGui::Separator();
    const float half = (ImGui::GetContentRegionAvail().x - 4.f) * 0.5f;
    // un trazo de pasto vive en la pila del modulo de terreno: se deshace desde aqui tambien
    ImGui::BeginDisabled(st.undo.empty() && bng_ter.undo.empty());
    if (ImGui::Button("Deshacer", ImVec2(half, 0.f))) {
        if (!bng_veg_undo() && !bng_ter.undo.empty()) bng_ter_undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(st.redo.empty() && bng_ter.redo.empty());
    if (ImGui::Button("Rehacer", ImVec2(half, 0.f))) {
        if (!bng_veg_redo() && !bng_ter.redo.empty()) bng_ter_redo();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("lotes: %d arboles / %d pasto (deshacer)  |  %d / %d (rehacer)",
                        static_cast<int>(st.undo.size()), static_cast<int>(bng_ter.undo.size()),
                        static_cast<int>(st.redo.size()), static_cast<int>(bng_ter.redo.size()));
    ImGui::Separator();
    if (ImGui::Button("Guardar forest (forest4.json + .bak)", ImVec2(-FLT_MIN, 0.f))) bng_veg_save(true);
    if (st.modified)
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.35f, 1.f), "Vegetacion modificada (sin guardar)");
    else
        ImGui::TextDisabled("Sin cambios");
    if (!st.saved_path.empty()) ImGui::TextDisabled("ultimo guardado: %s", st.saved_path.c_str());
    if (!st.status.empty()) ImGui::TextDisabled("%s", st.status.c_str());
    ImGui::TextDisabled("Se escribe en el directorio de trabajo del nivel (beamng_workspace/<nivel>/forest/);\n"
                        "los .forest4.json del juego no se modifican.");
};

#elif defined(BNG_VEGETATION_SECTION_FRAME)
#undef BNG_VEGETATION_SECTION_FRAME
// ── per-frame part: pick status + selection / brush input ─────────────────────────────────────────────────
if (bng_rt) {
    bng_veg_owns_lmb = false;
    auto& Lw = *bng_rt->s->level;
    float vmx = 0.f, vmy = 0.f, vfw = 1.f, vfh = 1.f;
    input::cursor_to_framebuffer(window, vmx, vmy, vfw, vfh);
    const float vasp = (vfw > 0.f && vfh > 0.f) ? vfw / vfh : 16.f / 9.f;
    float vwvp[16]{};
    camera.build_world_view_proj(vasp, vwvp);

    // (1) "que hay bajo el cursor": the status bar names the tree / grass cell the CPU pick just found.
    //     This is the diagnostic that was missing: before, a failed pick was completely silent.
    {
        static std::string s_veg_hover;
        std::string txt;
        if (!over_ui && !Lw.forest.empty()) {
            std::uint32_t hfid = 0;
            const float ft = bng_pick_forest_hook ? bng_pick_forest_hook(vmx, vmy, vfw, vfh, vwvp, &hfid) : 1e30f;
            int hgi = -1;
            std::uint64_t hkey = 0;
            const float gt =
                bng_pick_groundcover_hook ? bng_pick_groundcover_hook(vmx, vmy, vfw, vfh, vwvp, &hgi, &hkey) : 1e30f;
            char buf[224];
            if (ft < 1e29f && static_cast<std::size_t>(hfid) < Lw.forest.size()) {
                const auto& fi = Lw.forest[static_cast<std::size_t>(hfid)];
                const std::string tn = (fi.type >= 0 && static_cast<std::size_t>(fi.type) < Lw.forest_types.size())
                                           ? Lw.forest_types[static_cast<std::size_t>(fi.type)].name
                                           : std::string("?");
                std::snprintf(buf, sizeof(buf), "Pick: forest %s  dist %.1f m  escala %.2f  (nodo %u)", tn.c_str(),
                              static_cast<double>(ft), static_cast<double>(fi.scale), hfid);
                txt = buf;
            } else if (gt < 1e29f && hgi >= 0) {
                const int gx = static_cast<int>(static_cast<std::int32_t>(
                    static_cast<std::uint32_t>((hkey >> 24) & 0xFFFFFF) << 8) >> 8);
                const int gy = static_cast<int>(static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(hkey & 0xFFFFFF) << 8) >> 8);
                std::snprintf(buf, sizeof(buf), "Pick: pasto #%d  celda %d,%d  dist %.1f m", hgi, gx, gy,
                              static_cast<double>(gt));
                txt = buf;
            }
        }
        if (txt != s_veg_hover) {
            s_veg_hover = txt;
            if (!txt.empty()) panel.set_status(txt);
        }
    }

    // (2) tools
    auto& st = bng_veg;
    if (!view_only && st.mode != 0) {
        const bool lmb_v = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        static bool lmb_v_prev = false;
        const bool vpress = lmb_v && !lmb_v_prev;
        const bool vrelease = !lmb_v && lmb_v_prev;
        const double now_v = glfwGetTime();
        static double v_dt_t = 0.0;
        float v_dt = 0.016f;
        if (v_dt_t > 0.0) v_dt = (std::clamp)(static_cast<float>(now_v - v_dt_t), 0.002f, 0.05f);
        v_dt_t = now_v;
        static std::mt19937 v_rng(1234567u);
        std::uniform_real_distribution<float> u01(0.f, 1.f);
        // separacion / borrado: hash propio, reconstruido al empezar cada trazo (indices de Level::forest)
        auto sep_key = [](int gx, int gy) -> std::int64_t {
            return (static_cast<std::int64_t>(gx) << 32) | static_cast<std::int64_t>(static_cast<std::uint32_t>(gy));
        };
        auto sep_rebuild = [&]() {
            st.sep.clear();
            st.sep.reserve(Lw.forest.size() / 8 + 16);
            for (std::uint32_t i = 0; i < Lw.forest.size(); ++i) {
                if (Lw.forest[i].deleted) continue;
                st.sep[sep_key(static_cast<int>(std::floor(Lw.forest[i].pos[0] / kForestCell)),
                               static_cast<int>(std::floor(Lw.forest[i].pos[1] / kForestCell)))]
                    .push_back(i);
            }
        };
        auto sep_insert = [&](std::uint32_t i) {
            if (i >= Lw.forest.size()) return;
            st.sep[sep_key(static_cast<int>(std::floor(Lw.forest[i].pos[0] / kForestCell)),
                           static_cast<int>(std::floor(Lw.forest[i].pos[1] / kForestCell)))]
                .push_back(i);
        };
        auto sep_min_d2 = [&](float x, float y) -> float {
            float best = 1e30f;
            const int gx0 = static_cast<int>(std::floor(x / kForestCell));
            const int gy0 = static_cast<int>(std::floor(y / kForestCell));
            for (int gy = -1; gy <= 1; ++gy)
                for (int gx = -1; gx <= 1; ++gx) {
                    const auto itc = st.sep.find(sep_key(gx0 + gx, gy0 + gy));
                    if (itc == st.sep.end()) continue;
                    for (const std::uint32_t ci : itc->second) {
                        if (ci >= Lw.forest.size() || Lw.forest[ci].deleted) continue;
                        const float dx = Lw.forest[ci].pos[0] - x, dy = Lw.forest[ci].pos[1] - y;
                        const float d2 = dx * dx + dy * dy;
                        if (d2 < best) best = d2;
                    }
                }
            return best;
        };
        // ground point under the cursor (refined against the real surface / heightfield)
        std::optional<f4f23ma::f4> vhit;
        if (!over_ui && edit_mode == EditMode::None) {
            const float gz0 = camera.ground_z;
            camera.ground_z = camera.target_z;
            auto h0 = camera.hit_ground_xy(vmx, vmy, vfw, vfh);
            camera.ground_z = gz0;
            for (int it = 0; h0 && it < 4; ++it) {
                bool ok = true;
                const float zz = bng_veg_ground_z(h0->x, h0->y, &ok);
                if (!ok) {
                    h0.reset();
                    break;
                }
                camera.ground_z = zz;
                auto again = camera.hit_ground_xy(vmx, vmy, vfw, vfh);
                camera.ground_z = gz0;
                if (!again) {
                    h0.reset();
                    break;
                }
                h0 = again;
            }
            vhit = h0;
        }
        bng_veg_owns_lmb = vhit.has_value() && (st.mode == 1 || st.mode == 2 || st.mode == 4);
        if (bng_veg_owns_lmb && imgui_overlay.active() && vhit)
            draw_ground_ring(vhit->x, vhit->y,
                             (st.mode == 2) ? st.erase_radius : (st.mode == 4 ? st.gc_radius : st.paint_radius), false,
                             IM_COL32(140, 230, 130, 235), IM_COL32(120, 220, 120, 20));

        // ── pintar arboles ────────────────────────────────────────────────────────────────────────────────
        if (st.mode == 1) {
            static BngVegEdit::Batch v_batch;
            if (vpress && vhit) {
                st.stroke = true;
                v_batch = BngVegEdit::Batch{};
                v_batch.what = "pintar arboles";
                v_batch.size_before = Lw.forest.size();
                sep_rebuild();
            }
            if (st.stroke && lmb_v && vhit) {
                const float r = (std::max)(1.f, st.paint_radius);
                const float sp = 1.f / std::sqrt((std::max)(0.0008f, st.paint_density));
                int added = 0;
                for (float y = std::floor((vhit->y - r) / sp) * sp; y <= vhit->y + r && added < 300; y += sp) {
                    for (float x = std::floor((vhit->x - r) / sp) * sp; x <= vhit->x + r && added < 300; x += sp) {
                        const float jx = x + (u01(v_rng) - 0.5f) * sp * 0.9f;
                        const float jy = y + (u01(v_rng) - 0.5f) * sp * 0.9f;
                        const float dx = jx - vhit->x, dy = jy - vhit->y;
                        const float d2 = dx * dx + dy * dy;
                        if (d2 > r * r) continue;
                        const float u = std::sqrt(d2) / r;
                        if (u01(v_rng) < u * u * 0.95f) continue; // falloff: menos densidad en el borde
                        const float min_sep = (std::max)(0.1f, st.paint_min_sep);
                        if (sep_min_d2(jx, jy) < min_sep * min_sep) continue;
                        const int t = bng_veg_pick_type(st, v_rng, Lw.forest_types.size());
                        if (t < 0) continue;
                        bool ok = true;
                        const float z = bng_veg_ground_z(jx, jy, &ok);
                        if (!ok) continue;
                        float nx = 0.f, ny = 0.f, tilt = 0.f;
                        if (st.align_normal) {
                            bool ok2 = true;
                            const float zx = bng_veg_ground_z(jx + 1.5f, jy, &ok2);
                            const float zy = bng_veg_ground_z(jx, jy + 1.5f, &ok2);
                            if (ok2) {
                                const float gx = (zx - z) / 1.5f, gy = (zy - z) / 1.5f;
                                nx = -gx;
                                ny = -gy;
                                tilt = (std::min)(st.align_deg_max, std::atan(std::hypot(gx, gy)) * 57.29578f);
                            }
                        }
                        bng::ForestInstance fi;
                        fi.id = static_cast<std::uint32_t>(Lw.forest.size());
                        fi.type = t;
                        fi.pos[0] = jx;
                        fi.pos[1] = jy;
                        fi.pos[2] = z;
                        const float yaw = st.rot_random ? u01(v_rng) * 6.2831853f : 0.f;
                        fi.rot = bng_veg_rand_rot(v_rng, yaw, tilt, nx, ny);
                        fi.scale = st.scale_min + (st.scale_max - st.scale_min) * u01(v_rng);
                        fi.ctxid = 0;
                        Lw.forest.push_back(fi);
                        const std::uint32_t ni = static_cast<std::uint32_t>(Lw.forest.size() - 1);
                        bng_veg_refresh(ni);
                        sep_insert(ni);
                        v_batch.after.push_back(bng_veg_snap(ni, Lw));
                        ++added;
                    }
                }
                if (added > 0) {
                    bng_veg_owns_lmb = true;
                    bng_ter_owns_lmb = true;
                }
            }
            if (vrelease && st.stroke) {
                st.stroke = false;
                v_batch.size_after = Lw.forest.size();
                if (v_batch.size_after > v_batch.size_before) {
                    bng_veg_push_batch(v_batch);
                    st.status = "pintados " + std::to_string(v_batch.size_after - v_batch.size_before) + " arboles";
                }
            }
        // ── borrar ────────────────────────────────────────────────────────────────────────────────────────
        } else if (st.mode == 2) {
            static BngVegEdit::Batch e_batch;
            if (vpress && vhit) {
                st.stroke = true;
                e_batch = BngVegEdit::Batch{};
                e_batch.what = "borrar arboles";
                e_batch.size_before = e_batch.size_after = Lw.forest.size();
                sep_rebuild();
            }
            if (st.stroke && lmb_v && vhit) {
                const float r = (std::max)(0.5f, st.erase_radius);
                const int gx0 = static_cast<int>(std::floor((vhit->x - r) / kForestCell));
                const int gx1 = static_cast<int>(std::floor((vhit->x + r) / kForestCell));
                const int gy0 = static_cast<int>(std::floor((vhit->y - r) / kForestCell));
                const int gy1 = static_cast<int>(std::floor((vhit->y + r) / kForestCell));
                int killed = 0;
                for (int gy = gy0; gy <= gy1; ++gy)
                    for (int gx = gx0; gx <= gx1; ++gx) {
                        const auto itc = st.sep.find(sep_key(gx, gy));
                        if (itc == st.sep.end()) continue;
                        for (const std::uint32_t ci : itc->second) {
                            if (ci >= Lw.forest.size()) continue;
                            auto& fi = Lw.forest[ci];
                            if (fi.deleted) continue;
                            if (st.erase_type >= 0 && fi.type != st.erase_type) continue;
                            const float dx = fi.pos[0] - vhit->x, dy = fi.pos[1] - vhit->y;
                            if (dx * dx + dy * dy > r * r) continue;
                            e_batch.before.push_back(bng_veg_snap(ci, Lw));
                            fi.deleted = true;
                            bng_veg_refresh(ci);
                            e_batch.after.push_back(bng_veg_snap(ci, Lw));
                            ++killed;
                        }
                    }
                if (killed > 0) {
                    bng_veg_owns_lmb = true;
                    bng_ter_owns_lmb = true;
                }
            }
            if (vrelease && st.stroke) {
                st.stroke = false;
                if (!e_batch.before.empty()) {
                    st.status = "borrados " + std::to_string(e_batch.before.size()) + " arboles";
                    bng_veg_push_batch(e_batch);
                }
            }
        // ── pintar pasto (capa del terreno que lee el GroundCover) ───────────────────────────────────────
        } else if (st.mode == 4) {
            static std::unordered_map<std::uint32_t, std::pair<std::uint16_t, std::uint8_t>> t_touched;
            static float t_bx0 = 0.f, t_by0 = 0.f, t_bx1 = 0.f, t_by1 = 0.f;
            auto* Tw = bng_ter_terrain();
            if (vpress && vhit && Tw) {
                st.stroke = true;
                t_touched.clear();
                t_bx0 = t_bx1 = vhit->x;
                t_by0 = t_by1 = vhit->y;
                if (!st.terrain_bak) {
                    st.terrain_tool_bak = static_cast<float>(bng_ter.tool);
                    st.terrain_radius_bak = bng_ter.radius;
                    st.terrain_strength_bak = bng_ter.strength;
                    st.terrain_falloff_bak = bng_ter.falloff;
                    st.terrain_layer_bak = bng_ter.layer;
                    st.terrain_bak = true;
                }
            }
            if (st.stroke && lmb_v && vhit && Tw) {
                int layer = st.gc_layer;
                if (layer < 0 && static_cast<std::size_t>(st.gc_cover) < bng_rt->ground_covers.size())
                    for (const auto& ty : bng_rt->ground_covers[static_cast<std::size_t>(st.gc_cover)].types)
                        if (ty.layer >= 0) {
                            layer = ty.layer;
                            break;
                        }
                bng_ter.tool = st.gc_raise ? 8 : 9; // 8 pintar capa, 9 borrar capa (mod_beamng_terrain)
                bng_ter.layer = (std::max)(0, layer);
                bng_ter.radius = st.gc_radius;
                bng_ter.strength = st.gc_strength;
                bng_ter.falloff = st.gc_falloff;
                // one brush pass through the terrain module with its stroke bookkeeping on just for this pass:
                // the samples are merged into our own per-stroke map, so it never keeps a stroke open between
                // frames (its "close on release" branch would restart the undo batch every frame).
                bng_ter.stroke = true;
                bng_ter.touched.clear();
                bng_ter_apply(*Tw, vhit->x, vhit->y, v_dt, !st.gc_raise);
                for (const auto& [ti, orig] : bng_ter.touched) t_touched.emplace(ti, orig);
                bng_ter.touched.clear();
                bng_ter.stroke = false;
                t_bx0 = (std::min)(t_bx0, vhit->x);
                t_by0 = (std::min)(t_by0, vhit->y);
                t_bx1 = (std::max)(t_bx1, vhit->x);
                t_by1 = (std::max)(t_by1, vhit->y);
                bng_veg_owns_lmb = true;
                bng_ter_owns_lmb = true;
            }
            if (vrelease && st.stroke) {
                st.stroke = false;
                if (!t_touched.empty() && Tw) {
                    std::vector<BngTerrainEdit::Cell> batch; // one undo batch for the whole stroke
                    batch.reserve(t_touched.size());
                    for (const auto& [ti, orig] : t_touched) {
                        if (ti >= Tw->layers.size() || ti >= Tw->heights.size()) continue;
                        BngTerrainEdit::Cell c{};
                        c.i = ti;
                        c.h0 = Tw->heights[ti];
                        c.h1 = Tw->heights[ti];
                        c.l0 = orig.second;
                        c.l1 = Tw->layers[ti];
                        if (c.l0 == c.l1) continue;
                        batch.push_back(c);
                    }
                    if (!batch.empty()) {
                        bng_ter.undo.push_back(std::move(batch));
                        if (static_cast<int>(bng_ter.undo.size()) > bng_ter.undo_cap)
                            bng_ter.undo.erase(bng_ter.undo.begin());
                        bng_ter.redo.clear();
                    }
                    // rebuild the 32 m grass cells under the stroke: the GroundCover only grows on the layer we
                    // just painted, so this is what makes the grass appear / disappear under the brush.
                    int rebuilt = 0;
                    for (auto& [gc_key, cell] : bng_rt->gc_cells) {
                        const int gx = static_cast<int>(static_cast<std::int32_t>(
                            static_cast<std::uint32_t>((gc_key >> 24) & 0xFFFFFF) << 8) >> 8);
                        const int gy = static_cast<int>(static_cast<std::int32_t>(
                            static_cast<std::uint32_t>(gc_key & 0xFFFFFF) << 8) >> 8);
                        const float ccx = (static_cast<float>(gx) + 0.5f) * 32.f;
                        const float ccy = (static_cast<float>(gy) + 0.5f) * 32.f;
                        if (ccx < t_bx0 - 48.f || ccx > t_bx1 + 48.f || ccy < t_by0 - 48.f || ccy > t_by1 + 48.f)
                            continue;
                        if (cell.chunk != static_cast<std::size_t>(-1)) {
                            renderer.set_chunk_visible(cell.chunk, false);
                            renderer.release_chunk_gpu(cell.chunk);
                            cell.chunk = static_cast<std::size_t>(-1);
                        }
                        cell.visible = false;
                        cell.building = false;
                        cell.empty = false;
                        ++rebuilt;
                    }
                    st.status = "pasto (capa " + std::to_string(bng_ter.layer) + "): " +
                                std::to_string(t_touched.size()) + " muestras, " + std::to_string(rebuilt) + " celdas";
                    st.modified = true;
                }
                if (st.terrain_bak) { // restore the Terreno tab settings
                    bng_ter.tool = static_cast<int>(st.terrain_tool_bak);
                    bng_ter.radius = st.terrain_radius_bak;
                    bng_ter.strength = st.terrain_strength_bak;
                    bng_ter.falloff = st.terrain_falloff_bak;
                    bng_ter.layer = st.terrain_layer_bak;
                    st.terrain_bak = false;
                }
                bng_ter_flush_patches();
                t_touched.clear();
            }
        }
        lmb_v_prev = lmb_v;
    }

    // (3) seleccion (click / caja, Shift anade, Ctrl quita) — modo Seleccionar
    if (!view_only && st.mode == 0) {
        const bool lmb_s = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        static bool lmb_s_prev = false;
        static double s_px = 0.0, s_py = 0.0;
        double swx = 0.0, swy = 0.0;
        glfwGetCursorPos(window, &swx, &swy);
        const bool spress = lmb_s && !lmb_s_prev;
        const bool srelease = !lmb_s && lmb_s_prev;
        const bool shift_s = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl_s = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (spress && !over_ui) {
            s_px = swx;
            s_py = swy;
        }
        const bool box_now = lmb_s && !over_ui && std::hypot(swx - s_px, swy - s_py) > 6.0;
        if (box_now && imgui_overlay.active()) { // rubber band
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const float kx = disp.x / (std::max)(1.f, vfw), ky = disp.y / (std::max)(1.f, vfh);
            ImGui::GetForegroundDrawList()->AddRect(
                ImVec2(static_cast<float>(s_px) * kx, static_cast<float>(s_py) * ky),
                ImVec2(static_cast<float>(swx) * kx, static_cast<float>(swy) * ky), IM_COL32(140, 230, 130, 220));
        }
        if (srelease && !over_ui) {
            auto mark = [&](std::size_t i) {
                if (std::find(st.sel.begin(), st.sel.end(), static_cast<std::uint32_t>(i)) == st.sel.end())
                    st.sel.push_back(static_cast<std::uint32_t>(i));
                st.sel_primary = static_cast<int>(i);
            };
            auto unmark = [&](std::size_t i) {
                st.sel.erase(std::remove(st.sel.begin(), st.sel.end(), static_cast<std::uint32_t>(i)), st.sel.end());
            };
            if (std::hypot(swx - s_px, swy - s_py) <= 6.0) {
                std::uint32_t fid = 0;
                const float t = bng_pick_forest_hook ? bng_pick_forest_hook(vmx, vmy, vfw, vfh, vwvp, &fid) : 1e30f;
                if (t < 1e29f && static_cast<std::size_t>(fid) < Lw.forest.size()) {
                    if (ctrl_s) {
                        unmark(fid);
                        st.sel_primary = st.sel.empty() ? -1 : static_cast<int>(st.sel.back());
                    } else {
                        if (!shift_s) st.sel.clear();
                        mark(fid);
                    }
                    if (bng_select_forest_hook) bng_select_forest_hook(st.sel_primary);
                } else if (!shift_s && !ctrl_s && !st.sel.empty()) {
                    st.sel.clear();
                    st.sel_primary = -1;
                    if (bng_select_forest_hook) bng_select_forest_hook(-1);
                }
            } else {
                // box: screen-space test over the instances inside the streaming radius
                const float x0 = static_cast<float>((std::min)(s_px, swx)), x1 = static_cast<float>((std::max)(s_px, swx));
                const float y0 = static_cast<float>((std::min)(s_py, swy)), y1 = static_cast<float>((std::max)(s_py, swy));
                if (!shift_s && !ctrl_s) st.sel.clear();
                const float rad_stream = bng_rt->radius;
                for (std::size_t i = 0; i < Lw.forest.size(); ++i) {
                    const auto& fi = Lw.forest[i];
                    if (fi.deleted) continue;
                    if (std::hypot(fi.pos[0] - camera.target_x, fi.pos[1] - camera.target_y) > rad_stream) continue;
                    float sx = 0.f, sy = 0.f;
                    if (!world_to_screen(fi.pos[0], fi.pos[1], fi.pos[2] + 1.f, 0.f, 0.f, vfw, vfh, vwvp, sx, sy))
                        continue;
                    if (sx < x0 || sx > x1 || sy < y0 || sy > y1) continue;
                    if (ctrl_s) unmark(i);
                    else mark(i);
                }
                if (bng_select_forest_hook) bng_select_forest_hook(st.sel_primary);
                st.status = std::to_string(st.sel.size()) + " seleccionados";
            }
        }
        lmb_s_prev = lmb_s;
    }
}
#else
#error "mod_beamng_vegetation.inc.cpp: define BNG_VEGETATION_SECTION_SETUP or BNG_VEGETATION_SECTION_FRAME"
#endif
