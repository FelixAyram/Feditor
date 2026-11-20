// BeamNG terrain tools: brush edition of the TerrainData heightmap + material layers.
//
// The .ter holds a size*size grid of raw u16 heights (rows run north -> south: row 0 is the northern edge) and
// one byte per sample with the material layer (255 = hole). World z = origin.z + h / 65535 * maxHeight, so every
// tool works on world metres and converts back through bng_ter_z2h(). Sample <-> world uses the same mapping as
// TerrainData::sample_world() and the patch builder (x = origin.x + sx * squareSize, y = origin.y + sy * squareSize),
// so the stroke lands under the circle drawn by draw_ground_ring(). Nothing here touches the format: the file goes
// back to disk with bng::write_ter() inside beamng_workspace/<level>/ (never the game folder).
//
// The affected terrain patches are rebuilt by the async LOD pump of mod_beamng_frame.inc.cpp: a patch is marked
// dirty by forcing its current stride back to 0 (pump: "want_stride == stride -> skip"), so only the patches
// under the brush are rebuilt, on a worker thread, without touching the pump code. Dirty patches are flushed
// when the stroke ends (a running job still reads the height array, so flushing mid-stroke would race it).
//
// This file is included twice from the same function body (the editor's run loop):
//   mod_beamng_setup.inc.cpp : BNG_TERRAIN_SECTION_SETUP -> state, helpers and the "Terreno" tab body
//   mod_beamng_frame.inc.cpp : BNG_TERRAIN_SECTION_FRAME -> per-frame brush input + hover ring

#if defined(BNG_TERRAIN_SECTION_SETUP)
#undef BNG_TERRAIN_SECTION_SETUP

// ── state ─────────────────────────────────────────────────────────────────────────────────────────────────
// tool ids: 1 elevar, 2 bajar (Ctrl invierte), 3 suavizar, 4 aplanar, 5 nivelar a spawn, 6 ruido,
//           7 rampa entre dos puntos, 8 pintar capa, 9 borrar capa, 10 agujeros
struct BngTerrainEdit {
    int tool = 1;
    float radius = 20.f;            // brush radius, metres
    float strength = 0.6f;          // 0..1 (heights per second / layer mask width)
    float falloff = 0.5f;           // 0 = hard edge, 1 = fades from the centre
    int layer = 0;                  // material layer for "pintar capa"
    int flatten_target = 0;         // 0 = height of the first click, 1 = flatten_z, 2 = spawn height
    float flatten_z = 0.f;          // world z used when flatten_target == 1
    bool modified = false;          // the heightmap / layers differ from the .ter on disk
    std::string saved_path;         // last "Guardar terreno" target
    // stroke
    bool stroke = false;
    double last_time = 0.0;         // glfw clock of the previous brush pass (frame-rate independent strength)
    float ramp_x0 = 0.f, ramp_y0 = 0.f, ramp_h0 = 0.f; // rampa: first point + its height
    struct Cell {                   // one sample of an undo batch
        std::uint32_t i = 0;
        std::uint16_t h0 = 0, h1 = 0;
        std::uint8_t l0 = 0, l1 = 0;
    };
    std::unordered_map<std::uint32_t, std::pair<std::uint16_t, std::uint8_t>> touched; // original per stroke
    std::vector<std::vector<Cell>> undo, redo;
    std::vector<int> dirty_patches; // patches waiting for a rebuild (stride reset)
    int undo_cap = 64;
};
BngTerrainEdit bng_ter;
// True while LMB belongs to the terrain brush: the pick / marquee code in loop_03 checks it (same idea as
// spray_owns_lmb) so a stroke never selects or drags objects.
bool bng_ter_owns_lmb = false;

auto bng_ter_terrain = [&]() -> bng::TerrainData* {
    if (!bng_rt || !bng_rt->s || !bng_rt->s->level || !bng_rt->s->level->terrain) return nullptr;
    auto& t = *bng_rt->s->level->terrain;
    if (t.size < 2 || t.heights.size() < static_cast<std::size_t>(t.size) * t.size) return nullptr;
    if (t.layers.size() < t.heights.size()) t.layers.resize(t.heights.size(), 0);
    return &t;
};
auto bng_ter_h2z = [](const bng::TerrainData& t, std::uint16_t h) {
    return t.origin[2] + static_cast<float>(h) / 65535.f * t.max_height;
};
auto bng_ter_z2h = [](const bng::TerrainData& t, float z) -> std::uint16_t {
    if (t.max_height <= 0.f) return 0;
    return static_cast<std::uint16_t>((std::clamp)((z - t.origin[2]) * 65535.f / t.max_height, 0.f, 65535.f));
};
// Brush weight at distance d: flat core, smooth (smoothstep) ring of width (1 - falloff) * radius.
auto bng_ter_falloff = [](float d, float radius, float falloff) -> float {
    if (radius <= 0.f) return 0.f;
    const float u = (std::clamp)(d / radius, 0.f, 1.f);
    const float edge = (std::clamp)(1.f - falloff, 0.02f, 1.f);
    if (u <= 1.f - edge) return 1.f;
    const float v = (u - (1.f - edge)) / edge;
    return 1.f - v * v * (3.f - 2.f * v);
};
// Value noise in -1..1 (two octaves at the caller).
auto bng_ter_noise = [](float x, float y) -> float {
    auto h2 = [](int xi, int yi) -> float {
        std::uint32_t n = static_cast<std::uint32_t>(xi) * 374761393u + static_cast<std::uint32_t>(yi) * 668265263u;
        n = (n ^ (n >> 13)) * 1274126177u;
        n ^= n >> 16;
        return static_cast<float>(n & 0xFFFFFFu) / 16777215.f * 2.f - 1.f;
    };
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi), fy = y - static_cast<float>(yi);
    const float sx = fx * fx * (3.f - 2.f * fx), sy = fy * fy * (3.f - 2.f * fy);
    const float a = h2(xi, yi), b = h2(xi + 1, yi), c = h2(xi, yi + 1), d = h2(xi + 1, yi + 1);
    return (a * (1.f - sx) + b * sx) * (1.f - sy) + (c * (1.f - sx) + d * sx) * sy;
};

// Patches under a sample rectangle go to the dirty list (patch borders share their last sample).
auto bng_ter_touch_patches = [&](const bng::TerrainData& t, int sx0, int sx1, int sy0, int sy1) {
    if (!bng_rt || bng_rt->patch_samples <= 0 || bng_rt->patches_x <= 0 || bng_rt->patches.empty()) return;
    const int ps = bng_rt->patch_samples;
    const int n = static_cast<int>(t.size);
    sx0 = (std::max)(0, sx0);
    sy0 = (std::max)(0, sy0);
    sx1 = (std::min)(n - 1, sx1);
    sy1 = (std::min)(n - 1, sy1);
    if (sx1 < sx0 || sy1 < sy0) return;
    const int px0 = (std::max)(0, (sx0 - 1) / ps), px1 = (std::min)(bng_rt->patches_x - 1, (sx1 + 1) / ps);
    const int py0 = (std::max)(0, (sy0 - 1) / ps), py1 = (std::min)(bng_rt->patches_y - 1, (sy1 + 1) / ps);
    for (int py = py0; py <= py1; ++py)
        for (int px = px0; px <= px1; ++px) {
            const int idx = py * bng_rt->patches_x + px;
            if (idx < 0 || idx >= static_cast<int>(bng_rt->patches.size())) continue;
            if (std::find(bng_ter.dirty_patches.begin(), bng_ter.dirty_patches.end(), idx) ==
                bng_ter.dirty_patches.end())
                bng_ter.dirty_patches.push_back(idx);
        }
};
// Force the pump to rebuild only the dirty patches (stride 0 != want_stride). Patches whose job is still
// running are retried on the next frame: the worker reads the height array we are writing.
auto bng_ter_flush_patches = [&]() -> int {
    if (!bng_rt || bng_ter.dirty_patches.empty()) return 0;
    int started = 0;
    std::vector<int> keep;
    keep.reserve(bng_ter.dirty_patches.size());
    for (const int idx : bng_ter.dirty_patches) {
        if (idx < 0 || idx >= static_cast<int>(bng_rt->patches.size())) continue;
        auto& p = bng_rt->patches[static_cast<std::size_t>(idx)];
        if (p.building) {
            keep.push_back(idx);
            continue;
        }
        if (p.want_stride <= 0) p.want_stride = 1; // never evaluated yet: build at full detail
        p.stride = 0;
        ++started;
    }
    bng_ter.dirty_patches.swap(keep);
    return started;
};

// ── edit ──────────────────────────────────────────────────────────────────────────────────────────────────
// One brush pass at (cx, cy). dt = frame seconds, invert = Ctrl (elevar <-> bajar, agujeros <-> rellenar).
auto bng_ter_apply = [&](bng::TerrainData& t, float cx, float cy, float dt, bool invert) {
    const int n = static_cast<int>(t.size);
    const float R = (std::max)(0.5f, bng_ter.radius);
    const float rate = (std::clamp)(bng_ter.strength, 0.f, 1.f);
    const int sx0 = (std::max)(0, static_cast<int>(std::floor((cx - R - t.origin[0]) / t.square_size)));
    const int sx1 = (std::min)(n - 1, static_cast<int>(std::ceil((cx + R - t.origin[0]) / t.square_size)));
    const int sy0 = (std::max)(0, static_cast<int>(std::floor((cy - R - t.origin[1]) / t.square_size)));
    const int sy1 = (std::min)(n - 1, static_cast<int>(std::ceil((cy + R - t.origin[1]) / t.square_size)));
    if (sx1 < sx0 || sy1 < sy0) return;
    const bool height_tool = bng_ter.tool <= 7;
    if (height_tool && t.max_height <= 0.f) return; // TerrainBlock without maxHeight: nothing to scale
    const float ramp_z1 = (bng_ter.tool == 7) ? t.sample_world(cx, cy) : 0.f;
    int changed_cells = 0;
    for (int sy = sy0; sy <= sy1; ++sy) {
        for (int sx = sx0; sx <= sx1; ++sx) {
            const std::uint32_t i = static_cast<std::uint32_t>(sy) * t.size + static_cast<std::uint32_t>(sx);
            const float wx = t.origin[0] + static_cast<float>(sx) * t.square_size;
            const float wy = t.origin[1] + static_cast<float>(sy) * t.square_size;
            const float ddx = wx - cx, ddy = wy - cy;
            const float d2 = ddx * ddx + ddy * ddy;
            float w = (d2 > R * R) ? 0.f : bng_ter_falloff(std::sqrt(d2), R, bng_ter.falloff);
            float ramp_t = 0.f;
            if (bng_ter.tool == 7) {
                const float dx = cx - bng_ter.ramp_x0, dy = cy - bng_ter.ramp_y0;
                const float len2 = dx * dx + dy * dy;
                if (len2 < 1.f) continue;
                ramp_t = (std::clamp)(((wx - bng_ter.ramp_x0) * dx + (wy - bng_ter.ramp_y0) * dy) / len2, 0.f, 1.f);
                const float px = bng_ter.ramp_x0 + dx * ramp_t, py = bng_ter.ramp_y0 + dy * ramp_t;
                w = bng_ter_falloff(std::hypot(wx - px, wy - py), R, bng_ter.falloff);
            }
            if (w <= 0.002f) continue;
            const std::uint16_t h0 = t.heights[i];
            const std::uint8_t l0 = t.layers[i];
            if (height_tool) {
                const float z = bng_ter_h2z(t, h0);
                float dz = 0.f;
                switch (bng_ter.tool) {
                case 1:
                case 2: { // elevar / bajar, Ctrl invierte
                    float sign = (bng_ter.tool == 1) ? 1.f : -1.f;
                    if (invert) sign = -sign;
                    dz = sign * rate * 30.f * dt * w;
                    break;
                }
                case 3: { // suavizar: media 3x3 de las alturas actuales
                    float sum = 0.f;
                    int k = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox) {
                            const int qx = (std::clamp)(sx + ox, 0, n - 1), qy = (std::clamp)(sy + oy, 0, n - 1);
                            sum += bng_ter_h2z(t, t.heights[static_cast<std::size_t>(qy) * t.size +
                                                             static_cast<std::size_t>(qx)]);
                            ++k;
                        }
                    dz = (sum / static_cast<float>(k) - z) * (std::clamp)(rate * 8.f * dt * w, 0.f, 0.75f);
                    break;
                }
                case 4: // aplanar a la altura objetivo
                case 5: { // nivelar a la altura del spawn
                    const float target = (bng_ter.tool == 5)
                                             ? (cfg.beamng ? cfg.beamng->spawn_pos[2] : z)
                                             : bng_ter.flatten_z;
                    dz = (target - z) * (std::clamp)(rate * 10.f * dt * w, 0.f, 0.75f);
                    break;
                }
                case 6: { // ruido
                    const float nz = bng_ter_noise(wx / 45.f, wy / 45.f) * 0.7f +
                                     bng_ter_noise(wx / 12.f, wy / 12.f) * 0.3f;
                    dz = nz * rate * 25.f * dt * w;
                    break;
                }
                case 7: { // rampa del primer punto al cursor
                    const float target = bng_ter.ramp_h0 + (ramp_z1 - bng_ter.ramp_h0) * ramp_t;
                    dz = (target - z) * (std::clamp)(rate * 12.f * dt * w, 0.f, 0.75f);
                    break;
                }
                default: break;
                }
                if (dz != 0.f) {
                    const std::uint16_t h1 = bng_ter_z2h(t, z + dz);
                    if (h1 != h0) {
                        if (bng_ter.stroke) bng_ter.touched.emplace(i, std::make_pair(h0, l0));
                        t.heights[i] = h1;
                        ++changed_cells;
                    }
                }
            } else {
                // layers: "mascara" = brush falloff, "fuerza" = how wide the painted disc is
                if (w < 1.f - rate) continue;
                const int layer_count = static_cast<int>(t.material_names.size());
                int want = (bng_ter.tool == 8) ? bng_ter.layer : 0;      // 8 pintar, 9 borrar (capa base)
                if (bng_ter.tool == 10) want = invert ? 0 : 255;         // 10 agujeros (Ctrl rellena)
                if (want != 255 && layer_count > 0) want = (std::clamp)(want, 0, layer_count - 1);
                const auto l1 = static_cast<std::uint8_t>(want);
                if (l1 != l0) {
                    if (bng_ter.stroke) bng_ter.touched.emplace(i, std::make_pair(h0, l0));
                    t.layers[i] = l1;
                    ++changed_cells;
                }
            }
        }
    }
    if (changed_cells) {
        bng_ter.modified = true;
        t.dirty = true;
        bng_ter_touch_patches(t, sx0, sx1, sy0, sy1);
    }
};

// Close the current stroke: emit one undo batch with everything the stroke changed.
auto bng_ter_end_stroke = [&]() {
    if (!bng_ter.stroke) return;
    bng_ter.stroke = false;
    auto* t = bng_ter_terrain();
    if (t && !bng_ter.touched.empty()) {
        std::vector<BngTerrainEdit::Cell> batch;
        batch.reserve(bng_ter.touched.size());
        for (const auto& [i, orig] : bng_ter.touched) {
            if (i >= t->heights.size() || i >= t->layers.size()) continue;
            const std::uint16_t h1 = t->heights[i];
            const std::uint8_t l1 = t->layers[i];
            if (h1 == orig.first && l1 == orig.second) continue;
            BngTerrainEdit::Cell c{};
            c.i = i;
            c.h0 = orig.first;
            c.h1 = h1;
            c.l0 = orig.second;
            c.l1 = l1;
            batch.push_back(c);
        }
        if (!batch.empty()) {
            bng_ter.undo.push_back(std::move(batch));
            if (static_cast<int>(bng_ter.undo.size()) > bng_ter.undo_cap) bng_ter.undo.erase(bng_ter.undo.begin());
            bng_ter.redo.clear();
        }
    }
    bng_ter.touched.clear();
    bng_ter_flush_patches(); // rebuild now that no worker can be reading the heights mid-edit
};

// Undo / redo: whole batches of samples (heights + layers together).
auto bng_ter_apply_batch = [&](const std::vector<BngTerrainEdit::Cell>& batch, bool after) {
    auto* t = bng_ter_terrain();
    if (!t) return;
    const int n = static_cast<int>(t->size);
    int sx0 = n, sx1 = 0, sy0 = n, sy1 = 0;
    for (const auto& c : batch) {
        if (c.i >= t->heights.size() || c.i >= t->layers.size()) continue;
        t->heights[c.i] = after ? c.h1 : c.h0;
        t->layers[c.i] = after ? c.l1 : c.l0;
        const int sx = static_cast<int>(c.i % t->size), sy = static_cast<int>(c.i / t->size);
        sx0 = (std::min)(sx0, sx);
        sx1 = (std::max)(sx1, sx);
        sy0 = (std::min)(sy0, sy);
        sy1 = (std::max)(sy1, sy);
    }
    if (sx1 >= sx0 && sy1 >= sy0) bng_ter_touch_patches(*t, sx0, sx1, sy0, sy1);
    bng_ter.modified = true;
    t->dirty = true;
    bng_ter_flush_patches();
};
auto bng_ter_undo = [&]() -> bool {
    bng_ter_end_stroke(); // a pending stroke becomes its own undo batch first
    if (bng_ter.undo.empty()) return false;
    auto batch = std::move(bng_ter.undo.back());
    bng_ter.undo.pop_back();
    bng_ter_apply_batch(batch, false);
    bng_ter.redo.push_back(std::move(batch));
    return true;
};
auto bng_ter_redo = [&]() -> bool {
    if (bng_ter.redo.empty()) return false;
    auto batch = std::move(bng_ter.redo.back());
    bng_ter.redo.pop_back();
    bng_ter_apply_batch(batch, true);
    bng_ter.undo.push_back(std::move(batch));
    return true;
};

// ── save ──────────────────────────────────────────────────────────────────────────────────────────────────
// bng::write_ter() output goes to <Session.workspace>/<path relative to the level folder>, with the previous
// content kept as <file>.bak. The game install is never written.
auto bng_ter_save = [&](bool report) -> bool {
    auto* t = bng_ter_terrain();
    if (!t) return false;
    auto fail = [&](const std::string& why) {
        if (report) panel.set_status("Terreno: " + why);
        log::warn("beamng terrain save: " + why);
        return false;
    };
    std::filesystem::path root = cfg.beamng ? cfg.beamng->workspace : std::filesystem::path();
    if (root.empty()) {
        const std::string lvl = cfg.beamng ? cfg.beamng->info.name : std::string("level");
        root = std::filesystem::current_path() / "beamng_workspace" / lvl;
    }
    // t->file is the VFS path of the .ter ("/levels/<level>/art/terrains/x.ter"); keep the part after the level
    // folder so the workspace mirrors the game tree.
    std::filesystem::path rel;
    std::string vpath = t->file;
    if (vpath.find(':') != std::string::npos || vpath.find('\\') != std::string::npos) {
        rel = std::filesystem::u8path(vpath).filename(); // loose file on disk: only its name
    } else {
        vpath = bng::Vfs::normalize(vpath);
        const std::string lvl = cfg.beamng ? cfg.beamng->info.name : std::string();
        std::string prefix = "/levels/";
        if (!lvl.empty()) prefix += lvl + "/";
        const std::size_t at = vpath.find(prefix);
        if (at != std::string::npos) rel = std::filesystem::u8path(vpath.substr(at + prefix.size()));
        else if (!vpath.empty()) rel = std::filesystem::u8path(vpath.substr(1));
    }
    if (rel.empty()) rel = std::filesystem::path("terrain.ter");
    const std::filesystem::path dest = root / rel;
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) return fail("no se pudo crear " + dest.parent_path().string() + ": " + ec.message());
    std::filesystem::path bak = dest;
    bak += ".bak";
    bool bak_ok = false;
    if (std::filesystem::is_regular_file(dest, ec)) {
        bak_ok = std::filesystem::copy_file(dest, bak, std::filesystem::copy_options::overwrite_existing, ec) && !ec;
    } else {
        // first save: keep the pristine .ter when it is a loose file of the install / user folder
        const std::filesystem::path src = std::filesystem::u8path(t->file);
        if (std::filesystem::is_regular_file(src, ec))
            bak_ok = std::filesystem::copy_file(src, bak, std::filesystem::copy_options::overwrite_existing, ec) && !ec;
    }
    const auto bytes = bng::write_ter(*t);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) return fail("no se pudo escribir " + dest.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) return fail("escritura incompleta en " + dest.string());
    t->dirty = false;
    bng_ter.modified = false;
    bng_ter.saved_path = dest.string();
    if (report) {
        panel.set_status("Terreno guardado: " + dest.string() + (bak_ok ? " (respaldo .ter.bak)" : std::string()));
        log::info("beamng terrain saved: " + dest.string() + " bytes=" + std::to_string(bytes.size()) +
                  (bak_ok ? " (+bak)" : ""));
    }
    return true;
};

// ── "Terreno" tab ────────────────────────────────────────────────────────────────────────────────────────
bng_ui_terrain_tab = [&]() {
    if (!bng_rt) return;
    auto& st = panel.imgui_state();
    auto* t = bng_ter_terrain();
    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.f), "Terreno: heightmap y capas del .ter");
    ImGui::Separator();
    if (!t) {
        ImGui::TextDisabled("Este nivel no tiene TerrainBlock (o el .ter no se pudo leer).");
        return;
    }
    const float ext = static_cast<float>(t->size) * t->square_size;
    ImGui::Text("size %u | square %.2f m | %.0f x %.0f m | maxHeight %.1f m", t->size, t->square_size, ext, ext,
                t->max_height);
    ImGui::TextDisabled("origen (%.1f, %.1f, %.1f)  v0 = %.2f m  v65535 = %.2f m  |  %u capas", t->origin[0],
                        t->origin[1], t->origin[2], t->origin[2], t->origin[2] + t->max_height,
                        static_cast<unsigned>(t->material_names.size()));
    if (t->max_height <= 0.f)
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f),
                           "maxHeight = 0 en el TerrainBlock: no hay escala de altura, solo se pueden editar capas.");
    ImGui::Spacing();
    if (view_only) {
        ImGui::TextDisabled("Sesion en modo solo lectura (view_only): no se puede editar.");
        return;
    }
    ImGui::Checkbox("Pincel de terreno (arrastrar con LMB)", &st.bng_terrain_brush);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Mientras este activo, el arrastre izquierdo pinta en vez de seleccionar objetos.\n"
                          "El circulo amarillo es el pincel (radio, fuerza y caida de abajo).");
    static const char* kTools[] = {"Elevar",          "Bajar",       "Suavizar", "Aplanar", "Nivelar a spawn",
                                   "Ruido",           "Rampa (2 puntos)", "Pintar capa", "Borrar capa", "Agujeros"};
    int tool_idx = (std::clamp)(bng_ter.tool - 1, 0, 9);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##bng_ter_tool", &tool_idx, kTools, 10)) {
        bng_ter_end_stroke();
        bng_ter.tool = tool_idx + 1;
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##bng_ter_radius", &bng_ter.radius, 1.f, 400.f, "radio del pincel %.1f m");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##bng_ter_strength", &bng_ter.strength, 0.02f, 1.f, "fuerza %.2f");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##bng_ter_falloff", &bng_ter.falloff, 0.f, 1.f, "caida (falloff) %.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = borde duro (disco plano), 1 = caida suave desde el centro.");
    if (bng_ter.tool == 4) {
        ImGui::Separator();
        int ft = bng_ter.flatten_target;
        ImGui::RadioButton("Altura del primer click", &ft, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Altura fija", &ft, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Spawn", &ft, 2);
        bng_ter.flatten_target = ft;
        if (ft == 1) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##bng_ter_flat_z", &bng_ter.flatten_z, 0.25f, -10000.f, 10000.f, "altura objetivo %.2f m");
        } else if (ft == 2 && cfg.beamng) {
            ImGui::TextDisabled("objetivo: spawn z = %.2f m", cfg.beamng->spawn_pos[2]);
        }
    }
    if (bng_ter.tool == 8 || bng_ter.tool == 9) {
        ImGui::Separator();
        // layer combo from terrain.material_names
        std::string cur = (bng_ter.layer >= 0 && bng_ter.layer < static_cast<int>(t->material_names.size()))
                              ? t->material_names[static_cast<std::size_t>(bng_ter.layer)]
                              : std::string("(capa ") + std::to_string(bng_ter.layer) + ")";
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##bng_ter_layer", cur.c_str())) {
            for (std::size_t li = 0; li < t->material_names.size(); ++li)
                if (ImGui::Selectable(t->material_names[li].c_str(), static_cast<int>(li) == bng_ter.layer))
                    bng_ter.layer = static_cast<int>(li);
            ImGui::EndCombo();
        }
        ImGui::InputInt("capa", &bng_ter.layer, 1, 1);
        bng_ter.layer = (std::max)(0, bng_ter.layer);
        if (bng_ter.tool == 9) ImGui::TextDisabled("Borra a la capa base (0). Para abrir huecos usa Agujeros.");
    }
    if (bng_ter.tool == 10)
        ImGui::TextDisabled("LMB abre agujeros (capa 255); con Ctrl los rellena (vuelve a la capa 0).");
    if (bng_ter.tool == 1 || bng_ter.tool == 2) ImGui::TextDisabled("Ctrl invierte elevar / bajar.");
    if (bng_ter.tool == 7)
        ImGui::TextDisabled("Arrastra desde el punto de inicio hasta el final: la rampa se interpola en el ancho del pincel.");
    ImGui::Spacing();
    const float half_w = (ImGui::GetContentRegionAvail().x - 4.f) * 0.5f;
    ImGui::BeginDisabled(bng_ter.undo.empty());
    if (ImGui::Button("Deshacer", ImVec2(half_w, 0.f))) bng_ter_undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(bng_ter.redo.empty());
    if (ImGui::Button("Rehacer", ImVec2(half_w, 0.f))) bng_ter_redo();
    ImGui::EndDisabled();
    ImGui::TextDisabled("lotes: %d deshacer / %d rehacer  |  parches pendientes: %d", static_cast<int>(bng_ter.undo.size()),
                        static_cast<int>(bng_ter.redo.size()), static_cast<int>(bng_ter.dirty_patches.size()));
    ImGui::Separator();
    if (ImGui::Button("Guardar terreno (.ter + .bak)", ImVec2(-FLT_MIN, 0.f))) bng_ter_save(true);
    if (bng_ter.modified)
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.35f, 1.f), "Terreno modificado (sin guardar)");
    else
        ImGui::TextDisabled("Terreno sin cambios");
    if (!bng_ter.saved_path.empty()) ImGui::TextDisabled("ultimo guardado: %s", bng_ter.saved_path.c_str());
    ImGui::TextDisabled("Se escribe en el directorio de trabajo del nivel (beamng_workspace/<nivel>/);\n"
                        "el .ter del juego no se modifica.");
};

#elif defined(BNG_TERRAIN_SECTION_FRAME)
#undef BNG_TERRAIN_SECTION_FRAME

// ── per-frame brush input ──────────────────────────────────────────────────────────────────────────────────
// Runs before the pick / marquee / camera code of the frame (same place as mod_spray), so a stroke never turns
// into an object selection: bng_ter_owns_lmb is honoured by the LMB block of the loop.
{
    bng_ter_owns_lmb = false;
    auto* t = bng_ter_terrain();
    if (t && !view_only && panel.imgui_state().bng_terrain_brush && edit_mode == EditMode::None) {
        float tmx = 0.f, tmy = 0.f, tfw = 1.f, tfh = 1.f;
        input::cursor_to_framebuffer(window, tmx, tmy, tfw, tfh);
        const bool lmb_ter = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        // wall clock, not ImGui::GetIO().DeltaTime: the brush also works with the Ultralight panel
        const double now_s = glfwGetTime();
        float dt = 0.016f;
        if (bng_ter.last_time > 0.0)
            dt = (std::clamp)(static_cast<float>(now_s - bng_ter.last_time), 0.002f, 0.05f);
        bng_ter.last_time = now_s;
        const bool invert = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        // Ray onto the (already edited) heightfield: the plane hit of hit_ground_xy is refined with the .ter.
        std::optional<f4f23ma::f4> hit;
        if (!over_ui) {
            const float gz0 = camera.ground_z;
            camera.ground_z = camera.target_z;
            hit = camera.hit_ground_xy(tmx, tmy, tfw, tfh);
            for (int it = 0; hit && it < 4; ++it) {
                camera.ground_z = t->sample_world(hit->x, hit->y);
                const auto again = camera.hit_ground_xy(tmx, tmy, tfw, tfh);
                if (!again) {
                    hit.reset();
                    break;
                }
                hit = again;
            }
            camera.ground_z = gz0;
            if (hit && !t->contains(hit->x, hit->y)) hit.reset();
        }
        bng_ter_owns_lmb = hit.has_value();
        if (hit) {
            const ImU32 ring_col = bng_ter.tool == 10   ? IM_COL32(255, 120, 120, 235)
                                   : bng_ter.tool >= 8  ? IM_COL32(130, 220, 255, 235)
                                                        : IM_COL32(255, 235, 120, 230);
            if (imgui_overlay.active()) // draw_ground_ring uses the ImGui foreground draw list
                draw_ground_ring(hit->x, hit->y, bng_ter.radius, false, ring_col, IM_COL32(255, 235, 120, 22));
            if (lmb_ter) {
                if (!bng_ter.stroke) {
                    bng_ter.stroke = true;
                    bng_ter.touched.clear();
                    bng_ter.ramp_x0 = hit->x;
                    bng_ter.ramp_y0 = hit->y;
                    bng_ter.ramp_h0 = t->sample_world(hit->x, hit->y);
                    if (bng_ter.tool == 4) {
                        const int ft = bng_ter.flatten_target;
                        if (ft == 0) bng_ter.flatten_z = t->sample_world(hit->x, hit->y);
                        else if (ft == 2 && cfg.beamng) bng_ter.flatten_z = cfg.beamng->spawn_pos[2];
                    }
                }
                bng_ter_apply(*t, hit->x, hit->y, dt, invert);
            }
        }
        if (!lmb_ter) bng_ter_end_stroke(); // release inside or outside the terrain closes the undo batch
    } else if (bng_ter.stroke) {
        bng_ter_end_stroke();
    }
    if (!bng_ter.stroke) bng_ter_flush_patches(); // retry patches whose job was still running
}

#else
#error "mod_beamng_terrain.inc.cpp: define BNG_TERRAIN_SECTION_SETUP or BNG_TERRAIN_SECTION_FRAME"
#endif
