#include "ui/map_editor_ultralight_panel.hpp"
#include "ui/map_editor_imgui_panel.hpp"

#include "omsi/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace omsi::ui {
namespace {

void fire(EditorSidePanel& panel, EditorSidePanelState ev) { panel.imgui_merge_pending(ev); }

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        default: out += c; break;
        }
    }
    return out;
}

bool extract_int(const std::string& json, const char* key, int& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    out = std::atoi(json.c_str() + pos + needle.size());
    return true;
}

bool extract_float(const std::string& json, const char* key, float& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    out = static_cast<float>(std::atof(json.c_str() + pos + needle.size()));
    return true;
}

bool extract_string_field(const std::string& json, const char* key, std::string& out) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    const auto start = pos + needle.size();
    const auto end = json.find('"', start);
    if (end == std::string::npos) return false;
    out = json.substr(start, end - start);
    return true;
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Host-side virtual window (Ultralight cannot scrollTop-jump across 10k+ DOM rows).
constexpr int kInstanceWindow = 64;
int g_inst_win_start = 0;
int g_inst_win_epoch = 0;
std::string g_inst_filter;
std::string g_cat_filter;

std::string build_instance_rows_html(const std::vector<EditorListItem>& instances, const std::vector<int>& filtered,
                                     int win_a, int win_b, std::int32_t want_id) {
    std::string html;
    html.reserve(static_cast<std::size_t>((std::max)(0, win_b - win_a)) * 96);
    for (int k = win_a; k < win_b; ++k) {
        const int gi = filtered[static_cast<std::size_t>(k)];
        const auto& it = instances[static_cast<std::size_t>(gi)];
        const bool on = want_id != 0 && it.id == want_id;
        std::string esc;
        esc.reserve(it.label.size() + 8);
        for (char c : it.label) {
            switch (c) {
            case '&': esc += "&amp;"; break;
            case '<': esc += "&lt;"; break;
            case '>': esc += "&gt;"; break;
            case '"': esc += "&quot;"; break;
            default: esc += c; break;
            }
        }
        html += "<button type=\"button\" data-select=\"instanceSel\" data-index=\"";
        html += std::to_string(gi);
        html += "\" data-id=\"";
        html += std::to_string(it.id);
        html += on ? "\" class=\"sel\">" : "\">";
        html += esc;
        html += "</button>";
    }
    return html;
}

} // namespace

void handle_ultralight_panel_event(EditorSidePanel& panel, const std::string& json) {
    if (json.find("\"type\":\"instance_win\"") != std::string::npos) {
        int delta = 0;
        extract_int(json, "delta", delta);
        g_inst_win_start += delta;
        if (g_inst_win_start < 0) g_inst_win_start = 0;
        ++g_inst_win_epoch;
        return;
    }
    if (json.find("\"type\":\"instance_filter\"") != std::string::npos) {
        std::string text;
        extract_string_field(json, "text", text);
        g_inst_filter = std::move(text);
        g_inst_win_start = 0;
        ++g_inst_win_epoch;
        return;
    }
    if (json.find("\"type\":\"catalog_filter\"") != std::string::npos) {
        std::string text;
        extract_string_field(json, "text", text);
        g_cat_filter = std::move(text);
        ++g_inst_win_epoch; // reuse epoch to dirty-sync
        return;
    }
    if (json.find("\"type\":\"tool\"") != std::string::npos) {
        int id = 0;
        if (extract_int(json, "id", id)) {
            panel.imgui_set_tool(static_cast<EditorTool>((std::max)(0, (std::min)(8, id))));
            EditorSidePanelState ev{};
            ev.save_ui_settings = true;
            fire(panel, ev);
        }
        return;
    }
    if (json.find("\"type\":\"select\"") != std::string::npos) {
        int index = -1;
        extract_int(json, "index", index);
        if (json.find("catalogSel") != std::string::npos) {
            const bool spline = panel.current_tool() == EditorTool::Spline;
            panel.imgui_set_catalog_sel(spline ? -1 : index, spline ? index : -1);
            EditorSidePanelState ev{};
            if (spline)
                ev.select_catalog_spline = index;
            else
                ev.select_catalog_object = index;
            fire(panel, ev);
        }         else if (json.find("instanceSel") != std::string::npos && index >= 0) {
            const bool spline = panel.current_tool() == EditorTool::Spline;
            const auto& items = spline ? panel.spline_instances() : panel.object_instances();
            if (static_cast<std::size_t>(index) < items.size()) {
                EditorSidePanelState ev{};
                if (spline) {
                    ev.select_spline_index = items[static_cast<std::size_t>(index)].map_index;
                    ev.select_spline_id = items[static_cast<std::size_t>(index)].id;
                } else {
                    ev.select_object_index = items[static_cast<std::size_t>(index)].map_index;
                    ev.select_object_id = items[static_cast<std::size_t>(index)].id;
                }
                fire(panel, ev);
            }
        } else if (json.find("groundLayer") != std::string::npos && index >= 0) {
            auto& brush = panel.imgui_state().terrain_brush;
            const int n = static_cast<int>(panel.imgui_state().ground_layer_labels.size());
            if (n > 0) {
                brush.ground_layer = (std::max)(0, (std::min)(index, n - 1));
                EditorSidePanelState ev{};
                ev.ground_layer_sel = brush.ground_layer;
                fire(panel, ev);
            }
        }
        return;
    }
    if (json.find("\"type\":\"field\"") != std::string::npos) {
        std::string field;
        float value = 0.f;
        extract_string_field(json, "field", field);
        extract_float(json, "value", value);
        auto& sp = panel.imgui_spline_edit();
        auto& obj = panel.imgui_object_edit();
        auto& brush = panel.imgui_state().terrain_brush;
        bool spline_dirty = false;
        bool object_dirty = false;
        if (field == "spline.z") { sp.z = value; spline_dirty = true; }
        else if (field == "spline.rot") { sp.rot = value; spline_dirty = true; }
        else if (field == "spline.skew_start") { sp.skew_start = value; spline_dirty = true; }
        else if (field == "spline.skew_end") { sp.skew_end = value; spline_dirty = true; }
        else if (field == "spline.length") { sp.length = value; spline_dirty = true; }
        else if (field == "spline.radius") { sp.radius = value; spline_dirty = true; }
        else if (field == "spline.start_grad") { sp.start_grad = value; spline_dirty = true; }
        else if (field == "spline.end_grad") { sp.end_grad = value; spline_dirty = true; }
        else if (field == "spline.delta_h") { sp.delta_h = value; spline_dirty = true; }
        else if (field == "spline.cant_start") { sp.cant_start = value; spline_dirty = true; }
        else if (field == "spline.cant_end") { sp.cant_end = value; spline_dirty = true; }
        else if (field == "spline.max_radius") { panel.imgui_state().spline_max_radius = value; }
        else if (field == "object.x") { obj.x = value; object_dirty = true; }
        else if (field == "object.y") { obj.y = value; object_dirty = true; }
        else if (field == "object.z") { obj.z = value; object_dirty = true; }
        else if (field == "object.rot") { obj.rot = value; object_dirty = true; }
        else if (field == "object.pitch") { obj.pitch = value; object_dirty = true; }
        else if (field == "object.bank") { obj.bank = value; object_dirty = true; }
        else if (field == "object.parent") {
            panel.imgui_state().parent_id_field = static_cast<std::int32_t>(value);
            obj.varparent_id = panel.imgui_state().parent_id_field;
            object_dirty = true;
        } else if (field == "object.attach") {
            panel.imgui_state().attach_point_field = static_cast<int>(value);
        }
        else if (field == "terrain.brush_size") { brush.brush_size_m = (std::max)(1.f, value); }
        else if (field == "terrain.speed") { brush.speed = (std::max)(0.1f, value); }
        else if (field == "terrain.set_height") { brush.set_height = value; }
        else if (field == "terrain.mode") {
            const int m = (std::max)(0, (std::min)(4, static_cast<int>(value)));
            brush.mode = static_cast<TerrainBrushModeUi>(m);
        }
        if (spline_dirty && sp.valid) {
            EditorSidePanelState ev{};
            ev.apply_spline = true;
            fire(panel, ev);
        } else if (object_dirty && obj.valid) {
            EditorSidePanelState ev{};
            ev.apply_object = true;
            fire(panel, ev);
        }
        return;
    }
    if (json.find("\"type\":\"check\"") != std::string::npos) {
        auto& sp = panel.imgui_spline_edit();
        const bool on = json.find("\"value\":true") != std::string::npos;
        if (json.find("spline.use_delta_h") != std::string::npos) sp.use_delta_h = on;
        if (json.find("spline.mirror") != std::string::npos) sp.mirror = on;
        return;
    }
    if (json.find("\"type\":\"spawn\"") != std::string::npos) {
        auto& st = panel.imgui_state();
        const bool on = json.find("\"value\":true") != std::string::npos;
        if (json.find("showSplineEnds") != std::string::npos) st.show_spline_ends = on;
        else if (json.find("showPathEnds") != std::string::npos) st.show_path_ends = on;
        else if (json.find("showPathRails") != std::string::npos) st.show_path_rails = on;
        else if (json.find("autofillEnds") != std::string::npos) st.autofill_spline_ends = on;
        else if (json.find("stickTerrain") != std::string::npos) st.spawn_stick_terrain = on;
        else if (json.find("spawnMeshes") != std::string::npos) st.spawn_on_meshes = on;
        else if (json.find("alignPitchBank") != std::string::npos) st.spawn_align_pitch_bank = on;
        else if (json.find("spawnRandom") != std::string::npos) st.spawn_random = on;
        EditorSidePanelState ev{};
        ev.toggled_markers = true;
        ev.save_ui_settings = true;
        fire(panel, ev);
        return;
    }
    if (json.find("\"type\":\"spawn_random\"") != std::string::npos) {
        auto& st = panel.imgui_state();
        int mode = st.spawn_random_mode;
        int fav = st.spawn_random_fav_cat;
        extract_int(json, "mode", mode);
        extract_int(json, "favCat", fav);
        st.spawn_random_mode = (std::clamp)(mode, 0, 2);
        st.spawn_random_fav_cat = (std::max)(0, fav);
        st.spawn_random = true;
        EditorSidePanelState ev{};
        ev.save_ui_settings = true;
        fire(panel, ev);
        panel.set_status(st.spawn_random_mode == 0   ? "Generar al azar: catálogo"
                         : st.spawn_random_mode == 1 ? "Generar al azar: favoritos"
                                                     : "Generar al azar: carpeta");
        return;
    }
    if (json.find("\"type\":\"set_field_step\"") != std::string::npos) {
        std::string field;
        float step = 0.f;
        extract_string_field(json, "field", field);
        extract_float(json, "step", step);
        if (!field.empty() && step > 0.f) panel.imgui_state().field_steps[field] = step;
        return;
    }
    if (json.find("\"type\":\"action\"") != std::string::npos || json.find("\"type\":\"menu\"") != std::string::npos) {
        EditorSidePanelState ev{};
        if (json.find("load_catalog_file") != std::string::npos) ev.load_catalog_file = true;
        else if (json.find("load_catalog_directory") != std::string::npos) ev.load_catalog_directory = true;
        else if (json.find("spawn_random_scan_folder") != std::string::npos) ev.spawn_random_scan_folder = true;
        else if (json.find("open_catalog_folder") != std::string::npos) ev.open_catalog_folder = true;
        else if (json.find("edit_catalog_file") != std::string::npos) ev.edit_catalog_file = true;
        else if (json.find("reload_catalog_file") != std::string::npos) ev.reload_catalog_file = true;
        else if (json.find("open_spline_creator") != std::string::npos) ev.open_spline_creator = true;
        else if (json.find("apply_spline") != std::string::npos) ev.apply_spline = true;
        else if (json.find("apply_spline_mirror") != std::string::npos) {
            ev.apply_spline = true;
            ev.apply_spline_mirror_only = true;
        } else if (json.find("spline_complete") != std::string::npos) ev.spline_complete = true;
        else if (json.find("spline_split") != std::string::npos) ev.spline_split = true;
        else if (json.find("spline_align_start") != std::string::npos) ev.spline_align_start = true;
        else if (json.find("spline_align_end") != std::string::npos) ev.spline_align_end = true;
        else if (json.find("sli_bake_pick") != std::string::npos) ev.sli_bake_pick = true;
        else if (json.find("sli_bake_replace") != std::string::npos) ev.sli_bake_replace = true;
        else if (json.find("sli_bake_run") != std::string::npos) ev.sli_bake_run = true;
        else if (json.find("apply_object") != std::string::npos) ev.apply_object = true;
        else if (json.find("parent_to_object") != std::string::npos) ev.parent_to_object = true;
        else if (json.find("attach_to_object") != std::string::npos) ev.attach_to_object = true;
        else if (json.find("attach_to_spline") != std::string::npos) ev.attach_to_spline = true;
        else if (json.find("open_instance_map") != std::string::npos) ev.open_instance_map = true;
        else if (json.find("\"name\":\"archivo\"") != std::string::npos) ev.request_load_map = true;
        else if (json.find("show_shortcuts") != std::string::npos) ev.show_shortcuts_dialog = true;
        else if (json.find("open_options") != std::string::npos) ev.open_options_dialog = true;
        else if (json.find("spawn_omsi_bus") != std::string::npos) {
            ev.spawn_omsi_bus = true;
            extract_string_field(json, "path", ev.omsi_bus_path);
        }
        else if (json.find("save_ui_settings") != std::string::npos) ev.save_ui_settings = true;
        else if (json.find("ground_create_layer") != std::string::npos) {
            ev.ground_create_layer = true;
            ev.ground_layer_sel = panel.imgui_state().terrain_brush.ground_layer;
        } else if (json.find("ground_edit_layer") != std::string::npos) {
            ev.ground_edit_layer = true;
            ev.ground_layer_sel = panel.imgui_state().terrain_brush.ground_layer;
        } else if (json.find("ground_delete_layer") != std::string::npos) {
            ev.ground_delete_layer = true;
            ev.ground_layer_sel = panel.imgui_state().terrain_brush.ground_layer;
        }
        fire(panel, ev);
    }
}

void sync_map_editor_ultralight_state(const MapEditorUltralightArgs& args) {
    if (!args.panel || !args.overlay || !args.overlay->active() || !args.overlay->js_ready()) return;
    EditorSidePanel& panel = *args.panel;
    load_editor_docks(panel.imgui_state());
    const EditorTool tool = panel.current_tool();
    const bool spline_tool = tool == EditorTool::Spline;
    const auto& catalog = spline_tool ? panel.catalog_splines() : panel.catalog_objects();
    const auto& instances = spline_tool ? panel.spline_instances() : panel.object_instances();
    const int cat_sel = spline_tool ? panel.catalog_spline_sel() : panel.catalog_object_sel();
    const auto& st = panel.imgui_state();
    const std::int32_t want_inst_id = spline_tool ? args.selected_spline_id : args.selected_object_id;
    int instance_sel = -1;
    if (want_inst_id != 0) {
        for (std::size_t i = 0; i < instances.size(); ++i) {
            if (instances[i].id != want_inst_id) continue;
            instance_sel = static_cast<int>(i);
            break;
        }
    }

    static int last_tool = -1;
    static int last_cat = -2;
    static int last_inst_sel = -3;
    static std::size_t last_cat_n = 0;
    static std::size_t last_inst_n = 0;
    static std::string last_status;
    static std::string last_sel;
    static std::string last_filter;
    static std::string last_cat_filter;
    static std::int32_t last_sp_id = -1;
    static std::int32_t last_obj_id = -1;
    static std::int32_t last_want_inst = -999;
    static int last_win_epoch = -1;
    static int last_win_start = -1;
    static std::size_t last_layers_n = static_cast<std::size_t>(-1);
    static int last_layer_sel = -999;
    static int last_brush_mode = -999;
    const auto& sp = panel.imgui_spline_edit();
    const auto& obj = panel.imgui_object_edit();
    const auto& brush = st.terrain_brush;
    const bool sel_changed = last_want_inst != want_inst_id || last_inst_sel != instance_sel;
    const bool cat_changed = last_cat != cat_sel;
    const bool terrain_changed =
        last_layers_n != st.ground_layer_labels.size() || last_layer_sel != brush.ground_layer ||
        last_brush_mode != static_cast<int>(brush.mode);

    // Secondary search: on 3D/list pick, put id into Instancia filter so the hit is the list
    // (Ultralight cannot scrollTop-jump 10k rows — filtering IS the scroll).
    if (sel_changed) {
        if (want_inst_id != 0 && instance_sel >= 0 &&
            static_cast<std::size_t>(instance_sel) < instances.size()) {
            g_inst_filter = std::to_string(want_inst_id) + ":"; // "133949:" — unique vs substring ids
            g_inst_win_start = 0;
            ++g_inst_win_epoch;
            const std::string focus =
                "#" + std::to_string(want_inst_id) + " " + instances[static_cast<std::size_t>(instance_sel)].label;
            panel.set_status("FOCUS " + focus); // visible proof in status bar if sync runs
            log::info("Ultralight instance SEARCH focus filter=[" + g_inst_filter + "] label=" +
                      instances[static_cast<std::size_t>(instance_sel)].label);
            // Same for catalog: filter to this .sco/.sli filename (sticky card + search).
            const auto fname = instances[static_cast<std::size_t>(instance_sel)].path.filename().string();
            if (!fname.empty()) g_cat_filter = fname;
        } else if (want_inst_id == 0) {
            // Deselect — clear auto id-search filter.
            if (!g_inst_filter.empty() && g_inst_filter == std::to_string(last_want_inst) + ":") {
                g_inst_filter.clear();
                ++g_inst_win_epoch;
            }
        }
    }
    if (cat_changed && cat_sel >= 0 && static_cast<std::size_t>(cat_sel) < catalog.size()) {
        g_cat_filter = catalog[static_cast<std::size_t>(cat_sel)].label;
        ++g_inst_win_epoch;
        log::info("Ultralight catalog SEARCH focus=[" + g_cat_filter + "]");
    }

    std::vector<int> filtered;
    filtered.reserve(instances.size());
    const std::string needle = to_lower_ascii(g_inst_filter);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        if (!needle.empty()) {
            const std::string hay = to_lower_ascii(instances[i].label);
            if (hay.find(needle) == std::string::npos) continue;
        }
        filtered.push_back(static_cast<int>(i));
    }
    int sel_in_filtered = -1;
    if (instance_sel >= 0) {
        for (std::size_t k = 0; k < filtered.size(); ++k) {
            if (filtered[k] != instance_sel) continue;
            sel_in_filtered = static_cast<int>(k);
            break;
        }
    }

    if (g_inst_win_start < 0) g_inst_win_start = 0;
    if (!filtered.empty() && g_inst_win_start >= static_cast<int>(filtered.size()))
        g_inst_win_start = static_cast<int>(filtered.size()) - 1;
    if (filtered.empty()) g_inst_win_start = 0;

    const bool same =
        last_tool == static_cast<int>(tool) && last_cat == cat_sel && last_inst_sel == instance_sel &&
        last_want_inst == want_inst_id && last_cat_n == catalog.size() && last_inst_n == instances.size() &&
        last_status == panel.status_message() && last_sel == panel.selection_message() &&
        last_filter == g_inst_filter && last_cat_filter == g_cat_filter && last_win_epoch == g_inst_win_epoch &&
        last_win_start == g_inst_win_start && !terrain_changed &&
        (!spline_tool || (last_sp_id == sp.id && sp.valid == (last_sp_id > 0 || sp.valid))) &&
        (tool != EditorTool::Object || last_obj_id == obj.id);
    if (same) return;

    last_tool = static_cast<int>(tool);
    last_cat = cat_sel;
    last_inst_sel = instance_sel;
    last_want_inst = want_inst_id;
    last_cat_n = catalog.size();
    last_inst_n = instances.size();
    last_status = panel.status_message();
    last_sel = panel.selection_message();
    last_filter = g_inst_filter;
    last_cat_filter = g_cat_filter;
    last_sp_id = sp.id;
    last_obj_id = obj.id;
    last_win_epoch = g_inst_win_epoch;
    last_win_start = g_inst_win_start;
    last_layers_n = st.ground_layer_labels.size();
    last_layer_sel = brush.ground_layer;
    last_brush_mode = static_cast<int>(brush.mode);

    const int win_a = g_inst_win_start;
    const int win_b = (std::min)(win_a + kInstanceWindow, static_cast<int>(filtered.size()));

    std::string js = "window.omsiApplyState({";
    js += "tool:" + std::to_string(static_cast<int>(tool)) + ",";
    js += "mapName:\"" + json_escape(args.map_name ? args.map_name : "") + "\",";
    js += "status:\"" + json_escape(panel.status_message()) + "\",";
    js += "selection:\"" + json_escape(panel.selection_message()) + "\",";
    js += "stats:{tiles:" + std::to_string(args.tile_count) + ",objects:" + std::to_string(args.object_count) +
          ",splines:" + std::to_string(args.spline_count) + "},";
    js += "catalogSel:" + std::to_string(cat_sel) + ",";
    js += "catalogFilter:\"" + json_escape(g_cat_filter) + "\",";
    if (cat_sel >= 0 && static_cast<std::size_t>(cat_sel) < catalog.size()) {
        js += "catalogFocus:\"" + json_escape(catalog[static_cast<std::size_t>(cat_sel)].label) + "\",";
    } else {
        js += "catalogFocus:\"\",";
    }
    js += "instanceSel:" + std::to_string(instance_sel) + ",";
    js += "instanceId:" + std::to_string(want_inst_id) + ",";
    if (want_inst_id != 0 && instance_sel >= 0 &&
        static_cast<std::size_t>(instance_sel) < instances.size()) {
        js += "instanceFocus:\"" +
              json_escape("#" + std::to_string(want_inst_id) + " " +
                          instances[static_cast<std::size_t>(instance_sel)].label) +
              "\",";
    } else {
        js += "instanceFocus:\"\",";
    }
    js += "instanceFilter:\"" + json_escape(g_inst_filter) + "\",";
    js += "instanceOffset:" + std::to_string(win_a) + ",";
    js += "instanceTotal:" + std::to_string(filtered.size()) + ",";
    {
        const int shown_a = filtered.empty() ? 0 : (win_a + 1);
        const int shown_b = win_b;
        js += "instanceRange:\"" + std::to_string(shown_a) + "-" + std::to_string(shown_b) + "/" +
              std::to_string(filtered.size()) + "\",";
    }
    js += "catalog:[";
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        if (i) js += ',';
        js += "{label:\"" + json_escape(catalog[i].label) + "\"}";
    }
    js += "],instances:[";
    for (int k = win_a; k < win_b; ++k) {
        if (k > win_a) js += ',';
        const int gi = filtered[static_cast<std::size_t>(k)];
        const auto& it = instances[static_cast<std::size_t>(gi)];
        js += "{id:" + std::to_string(it.id) + ",index:" + std::to_string(gi) + ",label:\"" +
              json_escape(it.label) + "\"}";
    }
    js += "],";

    js += "spawn:{showSplineEnds:" + std::string(st.show_spline_ends ? "true" : "false") +
          ",showPathEnds:" + std::string(st.show_path_ends ? "true" : "false") +
          ",showPathRails:" + std::string(st.show_path_rails ? "true" : "false") +
          ",autofillEnds:" + std::string(st.autofill_spline_ends ? "true" : "false") +
          ",stickTerrain:" + std::string(st.spawn_stick_terrain ? "true" : "false") +
          ",spawnMeshes:" + std::string(st.spawn_on_meshes ? "true" : "false") +
          ",alignPitchBank:" + std::string(st.spawn_align_pitch_bank ? "true" : "false") +
          ",spawnRandom:" + std::string(st.spawn_random ? "true" : "false") +
          ",spawnRandomMode:" + std::to_string(st.spawn_random_mode) +
          ",spawnRandomFavCat:" + std::to_string(st.spawn_random_fav_cat) +
          ",spawnRandomFolder:\"" + json_escape(st.spawn_random_folder.filename().string()) + "\"" +
          ",spawnRandomFolderCount:" + std::to_string(st.spawn_random_folder_pool.size()) + "},";
    js += "favCats:[";
    for (std::size_t i = 0; i < st.fav_cats.size(); ++i) {
        if (i) js += ',';
        js += "{name:\"" + json_escape(st.fav_cats[i].name) + "\",count:" +
              std::to_string(st.fav_cats[i].items.size()) + "}";
    }
    js += "],";

    js += "lastOmsiBus:\"" + json_escape(args.last_omsi_bus) + "\",busCars:[";
    if (args.bus_catalog) {
        bool first = true;
        for (const auto& e : *args.bus_catalog) {
            if (!first) js += ',';
            first = false;
            js += "{path:\"" + json_escape(e.bus_path.string()) + "\",label:\"" + json_escape(e.list_label) + "\"}";
        }
    }
    js += "],";

    js += "fieldSteps:{";
    {
        bool first = true;
        for (const auto& [key, step] : st.field_steps) {
            if (key.empty() || !(step > 0.f)) continue;
            if (!first) js += ',';
            first = false;
            js += "\"" + json_escape(key) + "\":" + std::to_string(step);
        }
    }
    js += "},";

    if (spline_tool) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "spline:{valid:%s,id:%d,name:\"%s\",z:%.4f,rot:%.3f,skew_start:%.3f,skew_end:%.3f,"
                      "length:%.3f,radius:%.3f,start_grad:%.3f,end_grad:%.3f,use_delta_h:%s,delta_h:%.3f,"
                      "cant_start:%.3f,cant_end:%.3f,mirror:%s,max_radius:%.1f}",
                      sp.valid ? "true" : "false", sp.id, json_escape(sp.sli_name).c_str(), sp.z, sp.rot,
                      sp.skew_start, sp.skew_end, sp.length, sp.radius, sp.start_grad, sp.end_grad,
                      sp.use_delta_h ? "true" : "false", sp.delta_h, sp.cant_start, sp.cant_end,
                      sp.mirror ? "true" : "false", st.spline_max_radius);
        js += buf;
    } else if (tool == EditorTool::Object) {
        char buf[768];
        std::snprintf(buf, sizeof(buf),
                      "object:{valid:%s,id:%d,name:\"%s\",x:%.3f,y:%.3f,z:%.3f,rot:%.3f,pitch:%.3f,bank:%.3f,"
                      "parent:%d,attach:%d}",
                      obj.valid ? "true" : "false", obj.id, json_escape(obj.sco_name).c_str(), obj.x, obj.y, obj.z,
                      obj.rot, obj.pitch, obj.bank, panel.imgui_state().parent_id_field,
                      panel.imgui_state().attach_point_field);
        js += buf;
    }

    {
        if (spline_tool || tool == EditorTool::Object) js += ',';
        js += "terrain:{mode:" + std::to_string(static_cast<int>(brush.mode)) +
              ",brushSize:" + std::to_string(brush.brush_size_m) + ",speed:" + std::to_string(brush.speed) +
              ",setHeight:" + std::to_string(brush.set_height) +
              ",layerSel:" + std::to_string(brush.ground_layer) + ",layers:[";
        for (std::size_t i = 0; i < st.ground_layer_labels.size(); ++i) {
            if (i) js += ',';
            js += "\"" + json_escape(st.ground_layer_labels[i]) + "\"";
        }
        js += "]}";
    }
    js += "});";
    args.overlay->evaluate_script(js);

    // Always rewrite Instancia rows + filter box after a pick (search-by-id).
    if (sel_changed) {
        const std::string rows = build_instance_rows_html(instances, filtered, win_a, win_b, want_inst_id);
        const int shown_a = filtered.empty() ? 0 : (win_a + 1);
        const std::string range =
            std::to_string(shown_a) + "-" + std::to_string(win_b) + "/" + std::to_string(filtered.size());
        std::string pin = "window.omsiFocusInstanceSearch&&window.omsiFocusInstanceSearch(\"";
        pin += json_escape(g_inst_filter);
        pin += "\",\"";
        pin += json_escape(rows);
        pin += "\",\"";
        pin += json_escape(range);
        pin += "\",";
        pin += std::to_string(want_inst_id);
        pin += ");";
        args.overlay->evaluate_script(pin);
    }
}

void draw_map_editor_ultralight_panel(const MapEditorUltralightArgs& args) {
    sync_map_editor_ultralight_state(args);
    if (!args.overlay) return;
    args.overlay->begin_frame();
    args.overlay->end_frame_and_draw();
}

} // namespace omsi::ui
