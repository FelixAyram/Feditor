// BeamNG parsers smoke/roundtrip test against a real install.
// usage: bng_parse_test [install_root] [user_folder] [level]
#include "beamng/bng_assets.hpp"
#include "beamng/bng_dae.hpp"
#include "beamng/bng_roads.hpp"
#include "beamng/bng_json.hpp"
#include "beamng/bng_level.hpp"
#include "beamng/bng_vfs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <string>
#include <unordered_map>

using namespace bng;

int main(int argc, char** argv) {
    std::filesystem::path root = argc > 1 ? std::filesystem::u8path(argv[1]) : find_beamng_install();
    std::filesystem::path user = argc > 2 ? std::filesystem::u8path(argv[2]) : find_beamng_user_folder();
    const std::string want = argc > 3 ? argv[3] : "gridmap_v2";
    std::printf("install=%s\nuser=%s\n", root.string().c_str(), user.string().c_str());
    int fails = 0;
    auto t0 = std::chrono::steady_clock::now();
    Vfs vfs;
    vfs.mount_beamng(root, user);
    auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    std::printf("vfs: %zu files, %zu mounts, %.0f ms\n", vfs.file_count(), vfs.mounts().size(), ms());

    for (const auto& f : vfs.failed_mounts) std::printf("  mount FAIL: %s\n", f.c_str());
    const auto levels = discover_levels(vfs);
    std::printf("levels: %zu\n", levels.size());
    const LevelInfo* li = nullptr;
    for (const auto& l : levels) {
        std::printf("  %-28s spawns=%zu title=%s\n", l.name.c_str(), l.spawn_points.size(), l.title.c_str());
        if (l.name == want) li = &l;
    }
    if (!li) {
        std::printf("FAIL: level %s not found\n", want.c_str());
        return 1;
    }
    t0 = std::chrono::steady_clock::now();
    LoadProgress pr;
    auto lvl = load_level(vfs, *li, &pr);
    if (!lvl) {
        std::printf("FAIL: load_level\n");
        return 1;
    }
    std::size_t ts = 0, roads = 0, prefabs = 0, unresolved = 0;
    for (const auto& o : lvl->objects) {
        if (o.cls == "TSStatic") {
            ++ts;
            if (o.shape.empty()) ++unresolved;
        }
        if (o.cls == "DecalRoad") ++roads;
        if (o.cls == "Prefab") ++prefabs;
    }
    std::printf("load: %.0f ms objects=%zu groups=%zu TSStatic=%zu (unresolved shapes %zu) DecalRoad=%zu Prefab=%zu forest=%zu types=%zu "
                "materials=%zu terrainMats=%zu\n",
                ms(), lvl->objects.size(), lvl->groups.size(), ts, unresolved, roads, prefabs, lvl->forest.size(),
                lvl->forest_types.size(), lvl->materials.size(), lvl->terrain_materials.size());
    for (const auto& w : lvl->warnings) std::printf("  warn: %s\n", w.c_str());

    // terrain roundtrip
    if (lvl->terrain) {
        const auto& t = *lvl->terrain;
        std::printf("terrain: %s v%d size=%u mats=%zu origin=(%.1f,%.1f,%.1f) maxH=%.1f\n", t.file.c_str(), t.version, t.size,
                    t.material_names.size(), t.origin[0], t.origin[1], t.origin[2], t.max_height);
        auto orig = vfs.read(t.file);
        const auto out = write_ter(t);
        const bool same = orig && *orig == out;
        std::printf("  ter roundtrip: %s (%zu vs %zu bytes)\n", same ? "OK" : "DIFF", orig ? orig->size() : 0, out.size());
        if (!same && t.version == 9) ++fails;
        for (const auto& n : t.material_names) {
            bool found = false;
            for (const auto& m : lvl->terrain_materials)
                if (m.internal_name == n) found = true;
            std::printf("  layer %-16s %s\n", n.c_str(), found ? "ok" : "SIN TerrainMaterial");
        }
    } else {
        std::printf("terrain: none\n");
    }

    // items.level.json roundtrip (every line re-serialized must match)
    std::size_t lines = 0, diffs = 0;
    for (const auto& f : vfs.list(li->dir + "/main/", "/items.level.json")) {
        auto txt = vfs.read_text(f);
        if (!txt) continue;
        std::size_t p = 0;
        while (p < txt->size()) {
            std::size_t e = txt->find('\n', p);
            if (e == std::string::npos) e = txt->size();
            std::string line = txt->substr(p, e - p);
            p = e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            JsonValue v;
            if (!json_parse(line, v).ok) {
                ++diffs;
                continue;
            }
            ++lines;
            if (json_write_compact(v) != line) {
                if (diffs < 3) std::printf("  DIFF\n   in : %.200s\n   out: %.200s\n", line.c_str(), json_write_compact(v).c_str());
                ++diffs;
            }
        }
    }
    std::printf("items.level.json roundtrip: %zu lines, %zu diffs\n", lines, diffs);
    if (diffs) ++fails;

    // a few shapes
    int shown = 0;
    std::size_t tex_ok = 0, tex_miss = 0;
    for (const auto& o : lvl->objects) {
        if (o.cls != "TSStatic" || o.shape.empty() || shown >= 6) continue;
        static std::vector<std::string> seen;
        bool dup = false;
        for (const auto& s : seen) dup |= s == o.shape;
        if (dup) continue;
        seen.push_back(o.shape);
        ++shown;
        auto txt = vfs.read_text(o.shape);
        auto sh = txt ? parse_dae(*txt) : std::nullopt;
        if (!sh) {
            std::printf("dae FAIL %s\n", o.shape.c_str());
            ++fails;
            continue;
        }
        std::printf("dae %s: lods=%zu col=%zu r=%.1f\n", o.shape.c_str(), sh->lods.size(), sh->collision.size(), sh->radius);
        for (const auto& l : sh->lods) {
            std::printf("   lod %d tris=%zu:", l.detail_size, l.triangle_count());
            for (const auto& sm : l.submeshes) {
                const Material* m = lvl->material_for(sm.material);
                std::printf(" [%s%s]", sm.material.c_str(), m ? (m->stage0.color_map.empty() ? " sin color" : "") : " ?");
                if (m && !m->stage0.color_map.empty()) (vfs.exists(m->stage0.color_map) ? tex_ok : tex_miss)++;
            }
            std::printf("\n");
        }
    }
    std::printf("textures resolved: %zu ok, %zu missing\n", tex_ok, tex_miss);
    // ── A. DAE en centimetros ────────────────────────────────────────────────────────────────
    // <unit meter="0.01"> escala la matriz raiz, no los vertices: asi el tamano Y las traslaciones
    // de los nodos quedan en metros. Antes de eso un nodo en z=-2.99 cm saltaba a -2.99 m.
    auto extents = [](const DaeShape& s, float e[3]) {
        for (int k = 0; k < 3; ++k) e[k] = s.bounds_max[k] - s.bounds_min[k];
    };
    auto max_abs_bound = [](const DaeShape& s) {
        float m = 0.f;
        for (int k = 0; k < 3; ++k) m = (std::max)(m, (std::max)(std::fabs(s.bounds_min[k]), std::fabs(s.bounds_max[k])));
        return m;
    };
    // Sintetico (no necesita instalacion): quad de 400 cm y nodo trasladado 200 cm en Y_UP (=2 m en Z).
    {
        auto make_dae = [](const char* meter, int side, int ty) {
            const std::string s = std::to_string(side), y = std::to_string(ty);
            std::string x = "<?xml version=\"1.0\"?>\n<COLLADA version=\"1.4.1\">\n";
            x += "<asset><up_axis>Y_UP</up_axis><unit meter=\"";
            x += meter;
            x += "\" name=\"u\"/></asset>\n<library_geometries>\n";
            x += "<geometry id=\"quad\" name=\"quad\"><mesh>\n<source id=\"qp\">";
            x += "<float_array id=\"qp-a\" count=\"12\">0 0 0 " + s + " 0 0 " + s + " 0 " + s + " 0 " + s + "</float_array>\n";
            x += "<technique_common><accessor source=\"#qp\" count=\"4\" stride=\"3\">";
            x += "<param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/>"
                 "<param name=\"Z\" type=\"float\"/></accessor></technique_common></source>\n";
            x += "<vertices id=\"qv\"><input semantic=\"POSITION\" source=\"#qp\"/></vertices>\n";
            x += "<triangles count=\"2\" material=\"qm\"><input semantic=\"VERTEX\" source=\"#qv\" offset=\"0\"/>"
                 "<p>0 1 2 0 2 3</p></triangles>\n</mesh></geometry>\n</library_geometries>\n";
            x += "<library_visual_scenes><visual_scene id=\"sc\"><node id=\"root\" name=\"base00\">";
            x += "<node id=\"nm\" name=\"mesh500\"><translate>0 " + y + " 0</translate>";
            x += "<instance_geometry url=\"#quad\"/></node></node></visual_scene></library_visual_scenes>\n";
            x += "<scene><instance_visual_scene url=\"#sc\"/></scene>\n</COLLADA>\n";
            return x;
        };
        const auto cm = parse_dae(make_dae("0.01", 400, 200));
        const auto mt = parse_dae(make_dae("1", 4, 2));
        if (!cm || cm->lods.empty() || !mt || mt->lods.empty()) {
            std::printf("unidades FAIL: no se pudo parsear el DAE sintetico\n");
            ++fails;
        } else {
            float ec[3], em[3];
            extents(*cm, ec);
            extents(*mt, em);
            std::printf("unidades: cm unit=%.3f ext=(%.2f,%.2f,%.2f) base z=%.2f | m unit=%.3f ext=(%.2f,%.2f,%.2f) base z=%.2f\n",
                        cm->unit_scale, ec[0], ec[1], ec[2], cm->bounds_min[2], mt->unit_scale, em[0], em[1], em[2],
                        mt->bounds_min[2]);
            if (std::fabs(ec[0] - 4.f) > 0.05f || std::fabs(ec[1] - 4.f) > 0.05f) {
                std::printf("  FAIL: el DAE en cm no mide 4 m (vertices mal escalados)\n");
                ++fails;
            }
            if (std::fabs(cm->bounds_min[2] - 2.f) > 0.05f) {
                std::printf("  FAIL: la traslacion del nodo (200 cm) no cayo en 2 m de altura (quedo en %.2f)\n",
                            cm->bounds_min[2]);
                ++fails;
            }
            for (int k = 0; k < 3; ++k) {
                if (std::fabs(cm->bounds_min[k] - mt->bounds_min[k]) > 1e-3f ||
                    std::fabs(cm->bounds_max[k] - mt->bounds_max[k]) > 1e-3f) {
                    std::printf("  FAIL: DAE en cm y en m dan bounds distintos (eje %d: %.4f/%.4f vs %.4f/%.4f)\n", k,
                                cm->bounds_min[k], cm->bounds_max[k], mt->bounds_min[k], mt->bounds_max[k]);
                    ++fails;
                    break;
                }
            }
        }
    }
    // ── C. prefab cubo: colision fuera del LOD visual + textura del DAE ───────────────────
    // Los nodos de colision (Col/Colmesh/collision/box) no deben entrar como LOD visual (era el cubo de
    // rock_small_wall_02.prefab) y la textura declarada en el propio .dae queda como respaldo del material.
    {
        auto geometry = [](const char* id, const char* verts, int count) {
            const std::string sid = id;
            std::string s = "<geometry id=\"" + sid + "\"><mesh><source id=\"" + sid + "pos\"><float_array id=\"" + sid +
                            "arr\" count=\"" + std::to_string(count * 3) + "\">" + verts + "</float_array><technique_common><accessor source=\"#" + sid +
                            "arr\" count=\"" + std::to_string(count) +
                            "\" stride=\"3\"><param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/>"
                            "<param name=\"Z\" type=\"float\"/></accessor></technique_common></source><vertices id=\"" +
                            sid + "v\"><input semantic=\"VERTEX\" source=\"#" + sid + "pos\"/></vertices><triangles count=\"1\" material=\"qm\">"
                            "<input semantic=\"VERTEX\" source=\"#" + sid + "v\" offset=\"0\"/><p>0 1 2</p></triangles></mesh></geometry>";
            return s;
        };
        std::string x = "<?xml version=\"1.0\"?>\n<COLLADA version=\"1.4.1\">\n<asset><up_axis>Z_UP</up_axis><unit meter=\"1\" name=\"m\"/></asset>\n";
        x += "<library_images><image id=\"img0\" name=\"rock_low\"><init_from>../textures/rock_small_wall_02_d.dds</init_from></image></library_images>\n";
        x += "<library_effects><effect id=\"fx\"><profile_COMMON>";
        x += "<newparam sid=\"surf\"><surface><init_from>img0</init_from></surface></newparam>";
        x += "<newparam sid=\"samp\"><sampler2D><source>surf</source></sampler2D></newparam>";
        x += "<technique sid=\"t\"><phong><diffuse><texture texture=\"samp\" texcoord=\"TEX0\"/></diffuse></phong></technique>";
        x += "</profile_COMMON></effect></library_effects>\n";
        x += "<library_materials><material id=\"m0\" name=\"rock_small_wall_02\"><instance_effect url=\"#fx\"/></material></library_materials>\n";
        x += "<library_geometries>";
        x += geometry("gvis", "0 0 0 2 0 0 0 2 0", 3);  // malla real: 2 m
        x += geometry("gcol", "0 0 0 10 0 0 0 10 10", 3); // caja de colision: 10 m
        x += "</library_geometries>\n";
        x += "<library_visual_scenes><visual_scene id=\"sc\"><node id=\"base\" name=\"base00\">";
        x += "<node id=\"nvis\" name=\"mesh500\"><instance_geometry url=\"#gvis\"><bind_material><technique_common>";
        x += "<instance_material symbol=\"qm\" target=\"#m0\"/></technique_common></bind_material></instance_geometry></node>";
        x += "<node id=\"ncol\" name=\"Colmesh-1\"><instance_geometry url=\"#gcol\"/></node>";
        x += "<node id=\"nbox\" name=\"Box\"><instance_geometry url=\"#gcol\"/></node>";
        x += "</node></visual_scene></library_visual_scenes><scene><instance_visual_scene url=\"#sc\"/></scene></COLLADA>\n";
        const auto sh = parse_dae(x);
        if (!sh || sh->lods.empty()) {
            std::printf("colision FAIL: no parsea el DAE sintetico\n");
            ++fails;
        } else {
            float e[3];
            extents(*sh, e);
            const float big = (std::max)(e[0], (std::max)(e[1], e[2]));
            std::printf("colision: lods=%zu col=%zu tris=%zu mayor=%.2f tex=%s\n", sh->lods.size(), sh->collision.size(),
                        sh->lods.front().triangle_count(), big, sh->lods.front().submeshes.empty() ? "" : sh->lods.front().submeshes.front().texture.c_str());
            if (sh->collision.empty()) {
                std::printf("  FAIL: los nodos Colmesh/Box no se clasificaron como colision\n");
                ++fails;
            }
            if (big > 4.f) {
                std::printf("  FAIL: la caja de colision (10 m) entra en el LOD visual (mayor=%.2f)\n", big);
                ++fails;
            }
            if (sh->lods.front().submeshes.empty() || sh->lods.front().submeshes.front().texture.empty()) {
                std::printf("  FAIL: no se leyo la textura del .dae (library_images/effect)\n");
                ++fails;
            }
        }
        // Si la instalacion tiene el DAE real del prefab cubo, comprobarlo tambien.
        const auto rocks = vfs.list("/", "rock_small_wall_02.dae");
        if (!rocks.empty()) {
            auto txt = vfs.read_text(rocks.front());
            auto real = txt ? parse_dae(*txt) : std::nullopt;
            if (!real || real->lods.empty()) {
                std::printf("rock_small_wall_02 FAIL: no parsea %s\n", rocks.front().c_str());
                ++fails;
            } else {
                int leaking = 0;
                float re[3];
                extents(*real, re);
                std::printf("rock_small_wall_02: %s lods=%zu col=%zu ext=(%.1f,%.1f,%.1f) tex=%s\n", rocks.front().c_str(),
                            real->lods.size(), real->collision.size(), re[0], re[1], re[2],
                            real->lods.front().submeshes.empty() ? "" : real->lods.front().submeshes.front().texture.c_str());
                for (const auto& l : real->lods)
                    if (l.detail_size < 0) ++leaking;
                if (leaking) {
                    std::printf("  FAIL: %d LOD visuales con detalle negativo (colision)\n", leaking);
                    ++fails;
                }
            }
        }
    }
    // Datos reales de gridmap: el nombre de los cubos es su tamano nominal en metros.
    {
        struct Want {
            const char* file;
            float size;
        };
        static const Want cubes[] = {
            {"/levels/gridmap_v2/art/shapes/grid/s_gm_cube_1m.dae", 1.f},
            {"/levels/gridmap_v2/art/shapes/grid/s_gm_cube_4m.dae", 4.f},
            {"/levels/gridmap_v2/art/shapes/grid/s_gm_cube_16m.dae", 16.f},
        };
        const bool gridmap = li->name == "gridmap_v2";
        for (const auto& w : cubes) {
            auto txt = vfs.read_text(w.file);
            if (!txt) {
                if (gridmap) {
                    std::printf("cube FAIL: no existe %s\n", w.file);
                    ++fails;
                }
                continue;
            }
            auto sh = parse_dae(*txt);
            if (!sh || sh->lods.empty()) {
                std::printf("cube FAIL: no parsea %s\n", w.file);
                ++fails;
                continue;
            }
            float e[3];
            extents(*sh, e);
            const float big = (std::max)(e[0], (std::max)(e[1], e[2]));
            std::printf("cube %-5.0fm: unit=%.3f ext=(%.2f,%.2f,%.2f) mayor=%.2f base z=%.2f\n", w.size, sh->unit_scale, e[0], e[1],
                        e[2], big, sh->bounds_min[2]);
            if (big < w.size * 0.8f || big > w.size * 1.25f) {
                std::printf("  FAIL: mayor extension %.2f m no cuadra con %.0f m (unidad COLLADA mal aplicada)\n", big,
                            w.size);
                ++fails;
            }
            if (std::fabs(sh->bounds_min[2]) > w.size * 1.25f) {
                std::printf("  FAIL: base en z=%.2f, la traslacion del nodo no esta en metros\n", sh->bounds_min[2]);
                ++fails;
            }
        }
        // road_signal_glow es cm: con la unidad aplicada solo a los vertices su nodo saltaba metros.
        const char* glow = "/levels/gridmap_v2/art/shapes/grid/s_gm_road_signal_glow.dae";
        if (auto txt = vfs.read_text(glow); txt) {
            auto sh = parse_dae(*txt);
            if (!sh || sh->lods.empty()) {
                std::printf("road_signal_glow FAIL: no parsea\n");
                ++fails;
            } else {
                const float m = max_abs_bound(*sh);
                std::printf("road_signal_glow: unit=%.3f bounds_min=(%.2f,%.2f,%.2f) bounds_max=(%.2f,%.2f,%.2f) |max|=%.2f m\n",
                            sh->unit_scale, sh->bounds_min[0], sh->bounds_min[1], sh->bounds_min[2], sh->bounds_max[0],
                            sh->bounds_max[1], sh->bounds_max[2], m);
                if (m > 20.f) {
                    std::printf("  FAIL: desplazamiento de %.1f m; la unidad no esta escalando las traslaciones\n", m);
                    ++fails;
                }
            }
        }
    }
    float sp[3];
    float hd = 0;
    find_spawn(*lvl, "", sp, &hd);
    std::printf("spawn: (%.1f, %.1f, %.1f) heading %.1f deg\n", sp[0], sp[1], sp[2], hd * 57.2958f);
    // prefabs + roads
    {
        std::size_t pf = 0, pf_children = 0, pf_empty = 0;
        std::unordered_map<std::string, std::size_t> seen;
        for (const auto& o : lvl->objects) {
            if (o.cls != "Prefab") continue;
            ++pf;
            const std::string f = o.raw.get("filename").as_string();
            auto it = seen.find(f);
            if (it == seen.end()) it = seen.emplace(f, load_prefab(vfs, f, li->dir).size()).first;
            pf_children += it->second;
            if (it->second == 0) ++pf_empty;
        }
        std::printf("prefabs: %zu (files %zu, children %zu, empty %zu)%c", pf, seen.size(), pf_children, pf_empty, 10);
        std::size_t roads = 0, tris = 0, bad = 0;
        const TerrainData* tp = lvl->terrain ? &*lvl->terrain : nullptr;
        for (const auto& o : lvl->objects) {
            if (o.cls != "DecalRoad") continue;
            auto m = build_decal_road(read_road_nodes(o.raw.get("nodes")), static_cast<float>(o.raw.get("textureLength").as_number_lenient(5.0)),
                                      o.raw.get("looped").as_bool(), tp);
            ++roads;
            tris += m.indices.size() / 3;
            if (m.indices.empty()) ++bad;
        }
        std::printf("decal roads: %zu tris=%zu empty=%zu%c", roads, tris, bad, 10);
    }
    // Decals (main.decals.json): 876 instances in gridmap_v2, 4764 in west_coast_usa.
    {
        std::size_t with_mat = 0;
        for (const auto& dt : lvl->decal_types)
            if (!dt.material.empty() && lvl->material_for(dt.material)) ++with_mat;
        std::printf("decals: %zu instancias, %zu tipos (%zu con material)%c", lvl->decals.size(), lvl->decal_types.size(),
                    with_mat, 10);
        std::size_t quads = 0, empty_types = 0;
        for (std::size_t ti = 0; ti < lvl->decal_types.size(); ++ti) {
            std::vector<DecalInstance> inst;
            for (const auto& d : lvl->decals)
                if (d.type == static_cast<int>(ti)) inst.push_back(d);
            if (inst.empty()) continue;
            auto m = build_decal_quads(inst, lvl->decal_types[ti].texture_coords);
            quads += m.indices.size() / 6;
            if (m.indices.empty()) ++empty_types;
        }
        std::printf("  quads=%zu tipos sin geometria=%zu%c", quads, empty_types, 10);
        const std::size_t want_decals = li->name == "gridmap_v2" ? 876u : li->name == "west_coast_usa" ? 4764u : 0u;
        if (lvl->decals.empty() || (want_decals && lvl->decals.size() != want_decals)) {
            std::printf("  FAIL: se esperaban %zu decals para %s\n", want_decals, li->name.c_str());
            ++fails;
        }
    }
    // every distinct shape: how many parse to nothing (= invisible objects)
    {
        std::unordered_map<std::string, int> shapes;
        for (const auto& o : lvl->objects)
            if (o.cls == "TSStatic" && !o.shape.empty()) shapes[o.shape]++;
        for (const auto& t : lvl->forest_types)
            if (!t.shape_file.empty()) shapes[t.shape_file]++;
        std::size_t bad = 0, bad_inst = 0, shown = 0;
        for (const auto& [sh, cnt] : shapes) {
            auto txt = vfs.read_text(sh);
            auto d = txt ? parse_dae(*txt) : std::nullopt;
            if (!d || d->lods.empty()) {
                ++bad;
                bad_inst += static_cast<std::size_t>(cnt);
                if (shown++ < 12) std::printf("  shape vacia: %s (x%d) col=%zu%c", sh.c_str(), cnt, d ? d->collision.size() : 0, 10);
            }
        }
        std::printf("shapes: %zu distintas, %zu sin geometria (%zu instancias)%c", shapes.size(), bad, bad_inst, 10);
    }
    // euler round trip on every TSStatic rotation
    {
        double worst = 0;
        for (const auto& o : lvl->objects) {
            if (!o.has_rot) continue;
            float y, p, b;
            mat3_to_editor_euler(o.rot, y, p, b);
            const Mat3 r2 = editor_euler_to_mat3(y, p, b);
            double e = 0;
            for (int k = 0; k < 9; ++k) e = (std::max)(e, static_cast<double>(std::fabs(r2[k] - o.rot[k])));
            if (e > 1e-3 && worst <= 1e-3) {
                std::printf("  bad rot %s:", o.cls.c_str());
                for (int k = 0; k < 9; ++k) std::printf(" %.4f", o.rot[k]);
                const float det = o.rot[0] * (o.rot[4] * o.rot[8] - o.rot[5] * o.rot[7]) - o.rot[1] * (o.rot[3] * o.rot[8] - o.rot[5] * o.rot[6]) +
                                  o.rot[2] * (o.rot[3] * o.rot[7] - o.rot[4] * o.rot[6]);
                std::printf(" det=%.4f\n", det);
            }
            worst = (std::max)(worst, e);
        }
        std::printf("euler roundtrip worst err: %.6f\n", worst);
        if (worst > 1e-3) ++fails;
    }
    // ── D. rotationMatrix es column-major (Torque MatrixF) ────────────────────────────────────────────────
    // items.level.json / prefab.json escriben las 9 componentes por COLUMNAS. Leerlas como filas espeja cada
    // objeto rotado: el cartel sign_2way de east_coast_usa salia girado ~46 deg al reves y las vallas de madera
    // de una misma fila quedaban cruzadas. Los dos casos reales de abajo fijan la convencion.
    {
        auto rotation_from = [](const char* line) {
            JsonValue v;
            bool ok = json_parse(line, v).ok;
            bool has = false;
            const Mat3 r = mat3_from_rotation_matrix(v, has);
            return ok && has ? r : mat3_identity();
        };
        // east_coast_usa/main/MissionGroup/road_signs/items.level.json (TSStatic sign_2way)
        const Mat3 sign = rotation_from("[0.920190573,-0.391470253,0,0.391470253,0.920190573,0,0,0,1]");
        // east_coast_usa/main/MissionGroup/Fishing_Village/fence_a/items.level.json (eca_bld_wood_fence_c)
        const Mat3 fence = rotation_from("[0.996975362,0.077718094,0,-0.077718094,0.996975362,0,0,0,1]");
        const float sign_deg = mat3_heading(sign) * 57.2958f;
        const float fence_deg = mat3_heading(fence) * 57.2958f;
        std::printf("rotationMatrix: sign_2way yaw=%.2f deg (local X %.3f,%.3f) fence yaw=%.2f deg\n", sign_deg, sign[0],
                    sign[3], fence_deg);
        // yaw = atan2(-r[1], r[4]); la lectura vieja (fila a fila) daba -yaw en los dos casos
        if (std::fabs(sign_deg - (-23.05f)) > 0.1f) {
            std::printf("  FAIL: sign_2way deberia quedar a -23.05 deg (column-major); dio %.2f deg\n", sign_deg);
            ++fails;
        }
        if (std::fabs(fence_deg - 4.46f) > 0.1f) {
            std::printf("  FAIL: eca_bld_wood_fence_c deberia quedar a +4.46 deg; dio %.2f deg\n", fence_deg);
            ++fails;
        }
        // matriz de rotacion propia: det +1, X e Y ortonormales (una transposicion tambien lo cumple, por eso
        // ademas se comprueba el signo de arriba)
        const float det = sign[0] * (sign[4] * sign[8] - sign[5] * sign[7]) -
                          sign[1] * (sign[3] * sign[8] - sign[5] * sign[6]) +
                          sign[2] * (sign[3] * sign[7] - sign[4] * sign[6]);
        if (std::fabs(det - 1.f) > 1e-3f) {
            std::printf("  FAIL: determinante %.3f (se esperaba +1)\n", det);
            ++fails;
        }
    }
    // ── E. follaje: alphaTest => cutout (no blend) ─────────────────────────────────────────────────────────
    {
        bng::Material leaf; // trees_aspen/main.materials.json Birch_Leaves_01
        leaf.name = "Birch_Leaves_01";
        leaf.alpha_test = true;
        leaf.alpha_ref = 180;
        leaf.double_sided = true;
        leaf.stage0.opacity_map = "/levels/east_coast_usa/art/shapes/trees/trees_aspen/t_birch_leaves_o.data.png";
        bng::Material merged = leaf; // m_birch_merged_foliage
        merged.name = "m_birch_merged_foliage";
        merged.alpha_ref = 100;
        merged.stage0.opacity_map.clear();
        bng::Material marking; // road markings: translucent + alphaTest -> blend
        marking.name = "line_parking1";
        marking.alpha_test = true;
        marking.translucent = true;
        marking.alpha_ref = 32;
        bng::Material plain;
        plain.name = "concrete";
        const int am_leaf = static_cast<int>(alpha_mode_for(leaf));
        const int am_merged = static_cast<int>(alpha_mode_for(merged));
        const int am_mark = static_cast<int>(alpha_mode_for(marking));
        const int am_plain = static_cast<int>(alpha_mode_for(plain));
        std::printf("alpha_mode: hoja=%d follaje_unido=%d marca_vial=%d opaco=%d (1 = cutout)\n", am_leaf, am_merged,
                    am_mark, am_plain);
        if (am_leaf != 1 || am_merged != 1) {
            std::printf("  FAIL: el follaje con alphaTest tiene que ser cutout (1), no blend (2)\n");
            ++fails;
        }
        if (am_mark != 2) {
            std::printf("  FAIL: las marcas viales translucidas siguen siendo blend (2)\n");
            ++fails;
        }
        if (am_plain != 0) {
            std::printf("  FAIL: un material opaco no lleva alpha mode\n");
            ++fails;
        }
    }
    // ── F. impostores bb__autobillboard: no son un LOD de detalle ───────────────────────────────────────────
    // El numero final es el tamano en px al que se usa el impostor, no un detail size: si entra como LOD gana el
    // pick cerca de la camara y dibuja un quad plano en vez de la malla.
    {
        std::string x = "<?xml version=\"1.0\"?>\n<COLLADA version=\"1.4.1\">\n"
                        "<asset><up_axis>Z_UP</up_axis><unit meter=\"1\" name=\"m\"/></asset>\n"
                        "<library_geometries><geometry id=\"g\"><mesh><source id=\"p\"><float_array id=\"pa\" count=\"9\">"
                        "0 0 0 4 0 0 0 4 0</float_array><technique_common><accessor source=\"#pa\" count=\"3\" "
                        "stride=\"3\"><param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/>"
                        "<param name=\"Z\" type=\"float\"/></accessor></technique_common></source>"
                        "<vertices id=\"v\"><input semantic=\"VERTEX\" source=\"#p\"/></vertices>"
                        "<triangles count=\"1\" material=\"m0\"><input semantic=\"VERTEX\" source=\"#v\" offset=\"0\"/>"
                        "<p>0 1 2</p></triangles></mesh></geometry></library_geometries>\n"
                        "<library_visual_scenes><visual_scene id=\"sc\"><node id=\"b\" name=\"base00\">"
                        "<node id=\"bb\" name=\"bb__autobillboard25\"><instance_geometry url=\"#g\"/></node>"
                        "<node id=\"m\" name=\"tree_aspen_forest_a400\"><instance_geometry url=\"#g\"/></node>"
                        "</node></visual_scene></library_visual_scenes>"
                        "<scene><instance_visual_scene url=\"#sc\"/></scene></COLLADA>\n";
        const auto bb = parse_dae(x);
        if (!bb) {
            std::printf("billboard FAIL: no parsea el DAE sintetico\n");
            ++fails;
        } else {
            std::printf("billboard: lods=%zu billboards=%zu px=%d has=%d lod0=%d\n", bb->lods.size(),
                        bb->billboards.size(), bb->billboard_px, bb->has_billboard ? 1 : 0,
                        bb->lods.empty() ? -1 : bb->lods.front().detail_size);
            if (!bb->has_billboard || bb->billboards.empty()) {
                std::printf("  FAIL: no se reconocio el nodo bb__autobillboard25\n");
                ++fails;
            }
            if (bb->lods.size() != 1 || bb->lods.front().detail_size != 400) {
                std::printf("  FAIL: el impostor entro como LOD visual (lods=%zu)\n", bb->lods.size());
                ++fails;
            }
            if (bb->billboard_px != 25) {
                std::printf("  FAIL: tamano del impostor %d px (se esperaban 25)\n", bb->billboard_px);
                ++fails;
            }
        }
    }
    // async pool: build every distinct shape near the spawn
    {
        AssetPool pool(vfs, *lvl);
        std::size_t req = 0;
        for (const auto& o : lvl->objects)
            if (o.cls == "TSStatic" && !o.shape.empty() && std::hypot(o.pos[0] - sp[0], o.pos[1] - sp[1]) < 400.f) {
                pool.request(o.shape, 0.f);
                ++req;
            }
        auto t1 = std::chrono::steady_clock::now();
        std::size_t got = 0, ok = 0, tris = 0, subs = 0, untextured = 0;
        while (std::chrono::steady_clock::now() - t1 < std::chrono::seconds(120)) {
            std::string s;
            std::shared_ptr<ReadyMesh> m;
            if (pool.take_ready(s, m)) {
                ++got;
                if (m->ok) {
                    ++ok;
                    tris += m->triangles;
                    for (const auto& sub : m->subs) {
                        ++subs;
                        if (sub.texture.empty()) {
                            ++untextured;
                            if (untextured < 6) std::printf("  sin textura: %s mat=%s\n", s.c_str(), sub.material.c_str());
                        }
                    }
                }
                continue;
            }
            if (pool.pending() == 0 && got >= pool.done_count() && got > 0 && pool.done_count() >= 1 &&
                got == pool.done_count() && pool.materials().ready()) {
                // all requested built?
                bool idle = true;
                for (int i = 0; i < 20 && idle; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    if (pool.done_count() != got) idle = false;
                }
                if (idle && pool.pending() == 0) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        std::printf("pool: %zu TSStatic near spawn, %zu shapes built (%zu ok) in %.2fs, tris=%zu subs=%zu sin textura=%zu, shared mats=%zu\n",
                    req, got, ok, sec, tris, subs, untextured, pool.materials().size());
    }
    std::printf(fails ? "RESULT: %d FAIL\n" : "RESULT: OK\n", fails);
    return fails ? 1 : 0;
}
