#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "omsi/map_types.hpp"
#include "omsi/map_geo_tools.hpp"
#include "omsi/sco_types.hpp"
#include "app/map_editor/editor_types.hpp"
#include "app/map_editor/path_editor.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace omsi::ui {

// Matches OMSI Map Editor vertical tabs (screenshot / VCL PageControl).
enum class EditorTool {
    Tile = 0,
    Object,
    Spline,
    Terrain,
    TrafficRules,
    TracksTrips,
    StationLinks,
    SplineExp,
    SignalRts,
};

struct EditorSceneItem {
    std::size_t mesh_index = 0;
    std::int32_t object_id = 0;
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    std::string label;
};

struct EditorObjectProps {
    enum class AttachKind { None, AttachObj, SplineAttach };
    bool valid = false;
    std::int32_t id = 0;
    std::int32_t parent_id = 0; // [attachObj] parent instance (0 = none)
    std::int32_t varparent_id = 0; // [varparent] script var owner (TFileObject.myvarparent_IDCode)
    AttachKind attach_kind = AttachKind::None;
    int attach_point = 0;
    int spline_attach_count = 0;
    float spline_repeat_step = 0.f;
    float spline_repeat_range = 0.f;
    std::int32_t spline_index = -1;
    std::int32_t parent_spline_id = 0; // [splineAttachement] parent spline instance
    bool spline_align_terrain = true;
    bool spline_tangential = false;
    // [splineAttachement] extra text slots (count from .map, numbered 1..N in UI).
    int text_slot_count = 0;
    std::string sco_name;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float rot = 0.f;
    float pitch = 0.f;
    float bank = 0.f;
    std::vector<std::string> string_labels;
    std::vector<std::string> strings;
    // Parent [traffic_lights_group] names (OMSI Labels: "Nr. Traffic Light" / 0: Main, 1: Side).
    std::vector<std::string> traffic_light_names;
};

struct EditorSplineProps {
    bool valid = false;
    std::int32_t id = 0;
    std::string sli_name;
    bool is_spline_h = false;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float rot = 0.f;
    float skew_start = 0.f;
    float skew_end = 0.f;
    float length = 0.f;
    float radius = 0.f;
    float start_grad = 0.f;
    float end_grad = 0.f;
    bool use_delta_h = false;
    float delta_h = 0.f;
    float cant_start = 0.f;
    float cant_end = 0.f;
    bool mirror = false;
    // 0=No, 1=both THs, 2=only start TH, 3=only end TH, 4=no THs ([spline_terrain_align_2])
    int terrain_align = 0;
};

struct EditorPathRuleProps {
    bool valid = false;
    std::int32_t owner_id = 0;
    bool from_object = false;
    int path_index = 0;
    std::string label;
    bool crossed_out = false;
    float density = 1.f;
    int density_uvg = 0;
    bool has_speed = false;
    float speed_kmh = 50.f;
    bool has_priority = false;
    float priority = 128.f; // OMSI default priority byte 0x80
    bool no_cars = false;
    bool buses = false;
    bool trucks = false;
    bool no_overtake = false;
    int traffic_brush = 6; // RuleRadio / kTrafficRules index (paint tool)
};

struct EditorListItem {
    std::int32_t id = 0;
    std::string label;
    std::string group;
    std::filesystem::path path;
    int map_index = -1; // index in edit_objects / edit_splines
    std::int32_t tile_gx = 0;
    std::int32_t tile_gy = 0;
    bool missing = false;
    float preview_length = 12.f;
    float preview_radius = 0.f;
};

struct EditorTilePageInfo {
    bool valid = false;
    map::MapCoordKind coord_kind = map::MapCoordKind::Classic;
    bool can_convert = false;
    bool geo_ready = false;
    std::int32_t grid_x = 0;
    std::int32_t grid_y = 0;
    float tile_size_m = 300.f;
    float latitude_deg = 0.f;
    bool tile_in_map = false;
    bool can_create_n = false;
    bool can_create_s = false;
    bool can_create_e = false;
    bool can_create_w = false;
    std::string hint;
    std::string wgs84_summary; // lat/lon bounds for geographic grid (Web Mercator tiles)
    std::filesystem::path map_path; // current camera tile .map (empty if none)
};

struct EditorSidePanelState {
    EditorTool tool = EditorTool::Object;
    int teleport_entry = -1;
    int select_mesh = -1;
    int select_object_index = -1;
    int select_spline_index = -1;
    std::int32_t select_object_id = 0;
    std::int32_t select_spline_id = 0;
    int select_catalog_object = -1;
    int select_catalog_spline = -1;
    bool tool_changed = false;
    bool apply_object = false;
    bool apply_spline = false;
    bool apply_spline_mirror_only = false;
    bool spline_grad_preview = false;
    bool spline_grad_preview_end = false;
    bool undo_before_spline = false;
    bool undo_before_object = false;
    bool apply_path_rule = false;
    bool open_path_rule_dialog = false;
    bool open_extra_lines_dialog = false;
    EditorObjectProps object_edit{};
    EditorSplineProps spline_edit{};
    EditorPathRuleProps path_rule_edit{};
    
    // "Editar paths .sco" without a selection: instruct the user to pick a .o3d, then press Aceptar.
    bool sco_path_pick_pending = false;
    bool sco_path_pick_accept = false;
    
    bool closed = false;
    bool switch_map = false;
    std::filesystem::path switch_map_path;
    bool show_spline_ends = true;
    bool show_path_ends = true;
    bool show_path_rails = false;
    bool autofill_spline_ends = false;
    bool tex_max_256 = false;
    bool reload_file_textures = false;
    bool toggled_markers = false;
    bool load_catalog_file = false;
    bool load_catalog_directory = false;
    bool spawn_random_scan_folder = false;
    bool open_catalog_folder = false;
    bool edit_catalog_file = false;
    bool reload_catalog_file = false;
    bool open_spline_creator = false;
    bool open_instance_map = false;
    bool spline_place_commit = false; // "Colocar": solid remesh of splines left in wireframe by edits
    bool request_load_map = false;
    bool request_open_tile_map = false;
    bool show_shortcuts_dialog = false;
    bool open_entrypoint_picker = false;
    bool open_options_dialog = false;
    bool apply_editor_options = false;
    bool options_vsync = false;
    bool options_vsync_set = false;
    bool spawn_omsi_bus = false;
    std::string omsi_bus_path;
    bool omsi_bus_despawn = false;
    bool omsi_bus_reset = false;
    int options_tile_radius = 1;
    int options_visual_radius_m = 300;
    float options_union_pct = 0.02f;
    float options_grad_pct = 0.01f;
    bool save_ui_settings = false;
    bool save_geo_settings = false; // Tile tab: ref lat/lon, API keys, aerial flags -> omsi_f4 .omsi_f4_editor.cfg
    bool apply_map_names = false;
    std::string map_name_value;
    std::string map_friendly_value;
    bool convert_world_coords = false;
    bool download_tile_aerial = false;
    bool delete_tile_aerial = false;
    bool apply_tile_dtm = false;
    bool import_tile_dem = false;
    std::filesystem::path import_tile_dem_path;
    bool tile_aerial_visible = false;
    int aerial_resolution_pos = 0;
    map::EditorGeoSettings tile_geo_edit{};
    bool create_tile = false;
    std::int32_t create_tile_dx = 0;
    std::int32_t create_tile_dy = 0;
    bool delete_tile = false;
    bool attach_to_object = false;
    bool attach_to_spline = false;
    bool parent_to_object = false;
    int attach_point = 0;
    std::int32_t parent_id = 0;
    bool spline_split = false;
    int spline_split_cuts = 1;
    bool spline_complete = false;
    bool spline_align_start = false;
    bool spline_align_end = false;
    bool adapt_spline_gradient_terrain = false;
    bool adapt_spline_clearance = false;
    int spline_clearance_quality = 5;
    bool toggle_spline_multi_pick = false;
    bool toggle_object_multi_pick = false;
    bool start_group_marquee = false;
    bool sel_use_circle = false;
    bool sel_use_lasso = false;
    bool cancel_group_marquee = false;
    bool save_selection_preset = false;
    int spawn_preset_index = -1;
    bool sli_bake_pick = false;
    bool enter_mesh_edit = false;
    bool edit_o3d_file = false;
    bool enter_sco_path_edit = false;
    std::string sco_edit_file;
    int sco_path_count = 0;
    int sco_path_current_index = 0;
    bool enter_sco_traffic = false;
    bool toggle_editor_hide = false;
    // ── Group tools (multi-selection) ─────────────────────────────────────────
    // One row of the "edit as list" table. v[] columns depend on the kind:
    //   SCO: 0 x, 1 y, 2 z, 3 pitch, 4 rot, 5 bank, 6 align_terrain, 7 tangential        (attachment-only flags)
    //   SLI: 0 x, 1 y, 2 z, 3 rot, 4 length, 5 radius, 6 grad0, 7 grad1, 8 delta_h, 9 skew0, 10 skew1,
    //        11 cant0, 12 cant1, 13 mirror, 14 align_terrain (0..4)
    struct GroupRow {
        bool spline = false;
        bool attach = false; // SCO that is a spline attachment (X/Y/Z are lateral / along / height)
        std::int32_t id = 0;
        std::string name;
        float v[16]{};
    };
    // Ground layer dialog result (index < 0 = new layer)
    bool ground_apply = false;
    int ground_apply_index = -1;
    std::string ground_apply_main;
    std::string ground_apply_detail;
    int ground_apply_main_rep = 1;
    int ground_apply_det_rep = 60;
    // Water (one flat quad per tile, 4 corner heights; OMSI TFileWater)
    bool water_create = false;
    bool water_delete = false;
    bool water_set = false;
    float water_h[4]{0.f, 0.f, 0.f, 0.f};
    static constexpr int kSliCols = 15;
    static constexpr int kScoCols = 8;
    bool group_select_same = false;   // select every instance sharing the selected .sco / .sli
    int group_select_attach = 2;      // 1 = only plain .sco, 2 = also attachments
    bool group_new_gesture = false;   // first change of a drag: one undo step per gesture
    bool group_apply_delta = false;   // add group_delta (x, y, z, rot) / group_delta_pb (pitch, bank) to every item
    bool group_about_center = true;   // rotate / set height around the group centre instead of each own origin
    float group_delta[4]{};
    float group_delta_pb[2]{};
    bool group_set_z = false;         // absolute height for every selected item
    float group_z = 0.f;
    bool group_sco_flags = false;     // AlignTerrain / Tangential of the attachments in the group (-1 = keep)
    int group_sco_align = -1;
    int group_sco_tangential = -1;
    bool group_spline_align = false;  // AlignTerrain (0..4) for every .sli of the group
    int group_spline_align_value = 0;
    bool group_split = false;         // split every .sli of the group in `spline_split_cuts` parts
    bool group_replace_apply = false; // replace every instance of the selected .sco / .sli by group_replace_path
    bool group_replace_spline = false;
    std::filesystem::path group_replace_path;
    bool group_list_apply = false;    // "edit as list" Apply: rows overwrite the items
    std::vector<GroupRow> group_rows;
    bool unhide_all_editor = false;
    bool sli_bake_run = false;
    bool sli_bake_replace = false;
    bool tile_bake_run = false;
    bool tile_bake_replace = false;
    bool tile_bake_fbx = false;
    bool tile_bake_clean_o3d = false;
    bool tile_bake_clean_fbx = false;
    bool tile_bake_open_folder = false;
    int tile_bake_parts = 1; // 1..5
    float spline_max_radius = 200.f;
    bool create_lightmap = false;
    bool do_whole_map = false;
    bool aerial_missing = false;
    bool aerial_all = false;
    bool chrono_plus = false;
    bool chrono_minus = false;
    bool chrono_remove = false;
    int chrono_sel = -1;
    bool ground_create_layer = false;
    bool ground_delete_layer = false;
    bool ground_edit_layer = false;
    int ground_layer_sel = -1;
    bool refresh_tt = false;
    bool open_tt_file = false;
    std::filesystem::path tt_file_to_open;
    int tt_mode = -1; // -1 = no change this frame; 0=Add Trackentry, 1=Add Station, 2=Show Timetable (must NOT default to 0: take_events() re-applied it every frame)
    bool tt_create_track = false;
    bool tt_delete_track_entry = false;
    bool tt_create_trip_t1 = false;
    bool tt_create_trip_t2 = false;
    bool tt_delete_station = false;
    bool tt_edit_profiles = false;
    bool tt_assistent = false;
    bool tt_add_from_list = false;
    bool tt_change_type2 = false;
    bool tt_copy_track = false;
    bool tt_copy_trip = false;
    bool tt_copy_both = false;
    bool tt_edit_timetable = false;
    bool tt_find_errors = false;
    bool tt_train_reversed = false;
    std::string tt_terminus;
    std::string tt_line;
    // Selection / camera jump (Unit134 ListBox1Click / ListBox2Click)
    bool tt_track_entry_sel_changed = false;
    int  tt_track_entry_list_index = -1;   // raw list index including "< - >"
    bool tt_station_sel_changed = false;
    int  tt_station_list_index = -1;
    bool tt_track_combo_changed = false;
    int  tt_track_combo_index = -1;
    bool tt_trip_combo_changed = false;
    int  tt_trip_combo_index = -1;
    bool tt_request_camera_jump = false;
    float tt_camera_target[3] = {0.f, 0.f, 0.f};
    // Station Links
    bool sl_filter = false;
    bool sl_new = false;
    bool sl_search_err = false;
    bool sl_delete = false;
    int sl_mode = -1; // 0=start 1=end 2=path segments, -1 none
};

// OMSI Terrain tab brush (Higher / Lower / Set Height / Paint|Rubber Layer).
enum class TerrainBrushModeUi { Higher = 0, Lower = 1, SetHeight = 2, PaintLayer = 3, RubberLayer = 4, FitMesh = 5 };
struct TerrainBrushUi {
    TerrainBrushModeUi mode = TerrainBrushModeUi::Higher;
    float brush_size_m = 20.f;
    float speed = 3.f;       // m/s raise/lower
    float set_height = 0.f;  // absolute Z for Set Height
    int ground_layer = 0;
};

// Cached UI state when Dear ImGui replaces the Win32 side panel.
struct EditorSidePanelImGuiState {
    bool show_spline_ends = true;
    bool show_path_ends = true;
    bool show_path_rails = false;
    bool autofill_spline_ends = false;
    bool tex_max_256 = false;
    bool place_feedback = true;
    float place_feedback_volume = 70.f;
    std::string sfx_pack = "blocky";
    bool hint_play_sound = false;
    map::EditorGeoSettings tile_geo{};
    bool tile_aerial_visible = false;
    int aerial_resolution_pos = 0;
    TerrainBrushUi terrain_brush{};
    bool terrain_lasso = false;
    int terrain_lasso_action = 0; // 0 paint layer, 1 erase layer
    bool terrain_vertex_edit = false; // clickable terrain vertex handles (▲/▼)
    // Terrain tab sub-section drawn this frame: 0 Terreno (heights), 1 Agua, 2 Capas (layer paint). -1 = none.
    int terrain_active_sub = -1;
    float spline_max_radius = 200.f;
    int spline_cut_count = 1;
    float mesh_opacity = 1.f;
    std::unordered_set<std::int32_t> hidden_object_ids;
    std::unordered_set<std::int32_t> hidden_spline_ids;
    // Hide visual mesh of [onlyeditor] SLI splines; their paths stay.
    bool hide_onlyeditor_splines = false;
    // .sco path editor view toggles.
    bool sco_path_hide_model = false;
    bool sco_path_hide_visual = false;
    bool sco_path_hide_nodes = false;
    // Path visual style: 0 = Original (3D rails), 1 = Modern (page-like overlay for .sco and .sli).
    int path_style = 0;
    float sim_hour = 14.f;
    int sim_month = 6;
    int sim_day = 21;
    int sim_year = 2026;
    bool time_running = false; // the clock runs on by itself
    float time_speed = 1.f;    // x real time
    // Chrono scenarios of the map (Chrono/*/Chrono.cfg), by start date: the Vista stepper shows one and
    // jumps the date to it (the app fills these; chrono_pick = index asked for, -1 none)
    std::vector<std::string> chrono_event_names;
    std::vector<int> chrono_event_dates; // YYYYMMDD
    int chrono_pick = -1;
    int chrono_date = 0; // the chrono preview shown now (0 = plain map)
    bool ai_traffic_on = false; // Vista > Trafico IA (openOMSI random traffic)
    int ai_traffic_cars = 80;
    bool show_carpark_helpers = false; // Vista > Show carpark: the entrypoint / carpark helper boxes
    bool parked_cars_on = true; // Vista > Autos estacionados ([carpark_p] spaces, parklist_p.txt)
    float timezone_h = 1.f;
    int weather_id = 0;
    bool show_sun_shadows = true;
    bool fog_enabled = true;          // Vista > Graficos > Niebla
    float shadow_distance_m = 0.f;    // Vista > Graficos > Distancia de sombras (0 = the visual radius)
    std::uint32_t shadow_map_size = 4096;
    bool hints_ignore = false;
    // Vista: draw a 2D ImGui marker at the selected .sco's world origin (projected every frame).
    bool show_sco_origin = false;
    int spline_pending_place = 0; // splines kept in wireframe until "Colocar" (written by the app)
    bool pack_dialog_open = false; // Archivo > Empaquetar mapa (the app opens its modal)
    bool coop_window_open = false; // Ver > Sesion cooperativa
    bool walk_toggle_request = false; // Vista > Modo peaton (F7)
    bool walk_active_ui = false;      // written by the app (menu check mark)
    bool arrows_3d = false;  // Vista: one translucent 3D arrow instead of path + spline arrows
    bool hide_nodes = false; // Vista: spline/path nodes hidden and not pickable
    bool hint_visible = false;
    int hint_id = 0;
    float hint_anim = 0.f;
    std::string hint_text;
    bool sel_mode_circle = false;
    bool sel_mode_lasso = false;
    // 0=Pick simple, 1=Recuadro, 2=Circulo, 3=Lazo, 4=Multi
    int sel_ui_mode = 0;
    bool spawn_float_collapsed = false; // cozy minimize for upper-left tool window
    bool step_section_collapsed = false;
    std::vector<std::string> uvg_groups;
    int uvg_sel = 0;
    std::int32_t attach_point_field = 0;
    std::int32_t parent_id_field = 0;
    bool spawn_stick_terrain = false;
    bool spawn_on_meshes = false;
    bool spawn_align_pitch_bank = false;
    bool spawn_random = false;
    int spawn_random_mode = 0; // 0=catálogo 1=favoritos 2=carpeta
    int spawn_random_fav_cat = 0;
    std::filesystem::path spawn_random_folder;
    std::vector<std::filesystem::path> spawn_random_folder_pool;
    std::uint32_t spawn_random_seed = 0xC0FFEEu;
    bool spline_stick_chain = false;
    bool spline_fill_chain = false;
    bool spline_multi_pick = false;
    bool object_multi_pick = false;
    bool docks_collapsed = false;
    bool docks_dirty = false;
    bool instance_groups_tree = false;
    bool catalog_groups_tree = false; // false=flat A-Z list; true=A-Z grouped by [groups]
    int docks_size_mode = 0;
    int docks_tab = 0;
    char docks_search[128]{};
    int docks_last_list = 0;
    int docks_last_spray_category = 0;
    std::filesystem::path docks_pending_item;
    bool docks_pending_item_spline = false;
    bool docks_pending_fav_dialog = false;
    char docks_edit_list[96]{};
    int docks_edit_list_kind = -1;
    int docks_edit_list_index = -1;
    bool docks_font_ready = false;
    // ── Tracks & Trips / Station Links / Signal (ImGui panel) ───────────────
    int tt_mode = 0;                 // 0=Add Trackentry 1=Add Station 2=Show TT
    int sl_mode = 0;                 // 0=Set Start 1=Set End 2=Add Path Segments
    std::vector<std::filesystem::path> tt_tracks;
    std::vector<std::filesystem::path> tt_trips;
    std::vector<std::filesystem::path> tt_stations;
    std::vector<std::filesystem::path> tt_signals;
    int tt_track_sel = -1;
    int tt_trip_sel = -1;
    int tt_station_sel = -1;
    int tt_signal_sel = -1;
    char tt_terminus_buf[128]{};
    char tt_line_buf[64]{};
    bool tt_train_reversed = false;
    // Entry/station rows (Unit134 format strings) for rich lists when available
    std::vector<std::string> tt_track_entries;
    std::vector<std::string> tt_trip_stations;
    int tt_track_entry_sel = -1;   // logical index (-1 = none / "< - >")
    int tt_trip_station_sel = -1;

    bool docks_pending_rename = false;
    std::filesystem::path hover_preview_path;
    double hover_preview_since = 0.0;
    bool hover_preview_spline = false;
    enum class SprayShapeUi { Circle = 0, Square = 1, Lasso = 2 };
    struct SprayItem {
        std::filesystem::path path;
        std::string label;
        bool enabled = true;
        float weight = 1.f;
    };
    struct SprayCategory {
        std::string name;
        std::vector<SprayItem> items;
        bool enabled = true;
        bool open = false;
    };
    std::vector<SprayCategory> spray_categories;
    int spray_active_category = 0;
    char spray_new_category[64]{};
    char spray_filter[128]{};
    float spray_speed = 12.f;
    float spray_radius = 10.f;
    float spray_spacing = 2.f;
    int spray_definition = 28;
    int spray_max_count = 100;
    SprayShapeUi spray_shape = SprayShapeUi::Circle;
    float spray_yaw_deg = 0.f;
    bool spray_random_rotation = false;
    bool spray_enabled = false;
    bool spray_stroke_active = false;
    std::uint64_t spray_placed_total = 0;
    float spray_accumulator = 0.f;
    bool spray_stroke_undo_open = false;
    bool spray_has_candidates = true;
    std::uint32_t spray_seed = 0xA53C9E17u;
    std::string spray_save_list_name;
    struct SprayPoint { float x = 0.f, y = 0.f; };
    std::vector<SprayPoint> spray_lasso_points;
    std::vector<SprayPoint> spray_stroke_points;
    bool spray_painting = false;
    bool spray_pick_mode = false;
    bool spray_stop_no_fit = true;
    std::size_t spray_stroke_object_count = 0;
    // Debug: speed up traffic light OSC phases (x5).
    bool traffic_light_x5 = false;
    struct CatalogFavItem {
        bool spline = false;
        std::filesystem::path path;
        std::string label;
    };
    struct CatalogFavCat {
        std::string name;
        std::vector<CatalogFavItem> items;
    };
    struct EditorPresetItem {
        bool spline = false;
        std::filesystem::path path;
        float dx = 0.f, dy = 0.f, dz = 0.f;
        float heading = 0.f;
        float rot_x = 0.f, rot_y = 0.f, rot_z = 0.f; // object pitch/yaw/bank
        float length = 12.f, radius = 0.f;
        // Object extras
        std::int32_t varparent_id = 0;
        std::int32_t string_line_count = 0;
        std::vector<std::string> strings;
        // Spline extras
        float start_grad = 0.f, end_grad = 0.f;
        bool use_delta_h = false;
        float delta_h = 0.f;
        float skew_start = 0.f, skew_end = 0.f;
        float cant_start = 0.f, cant_end = 0.f;
        bool mirror = false;
    };
    struct EditorPreset {
        std::string name;
        std::string list = "General";
        std::vector<EditorPresetItem> items;
    };
    std::vector<CatalogFavCat> fav_cats;
    std::vector<EditorPreset> presets;
    std::vector<std::string> preset_lists;
    int preset_active_list = 0;
    char preset_new_list[64]{};
    char fav_new_cat[64]{};
    char preset_name_buf[64]{};
    int fav_active_cat = 0;
    std::int32_t inst_list_scrolled_id = 0;
    int catalog_list_scrolled_sel = -1;
    bool docks_loaded = false;
    bool spline_adapt_gradient_live = false;
    bool spline_clearance_enabled = false;
    int spline_clearance_quality = 5;
    char catalog_filter[128]{};
    char instance_filter[128]{};
    std::vector<std::string> ground_layer_labels;
    // Ground layer edit/create dialog
    bool layer_dlg_open = false;
    int layer_dlg_index = -1;
    char layer_dlg_main[260]{};
    char layer_dlg_detail[260]{};
    int layer_dlg_main_rep = 1;
    int layer_dlg_det_rep = 60;
    std::vector<std::string> chrono_labels;
    int chrono_sel = -1;
    // Per-field arrow/drag step (gear popup). Keys like "spline.z", "object.rot".
    std::unordered_map<std::string, float> field_steps;
    bool field_steps_dirty = false;
    // DragFloat gesture: true on the frame the user starts scrubbing an input.
    bool spin_gesture_began = false;
    // True the frame the user releases a DragFloat (end of one undo group).
    bool spin_gesture_ended = false;
    // Bottom-right busy toast (save / stream / boot). progress < 0 → indeterminate.
    bool busy_overlay = false;
    // Boot: full-screen black loading screen (centered spinner/progress) instead of the bottom-right toast.
    bool boot_screen = false;
    bool busy_spinner = false;
    float busy_progress = -1.f;
    std::string busy_title;
    std::string busy_detail;
    bool tile_bake_ready = false;
    bool tile_bake_busy = false;
    bool tile_bake_has_o3d = false;
    bool tile_bake_has_fbx = false;
    std::filesystem::path tile_bake_folder;
    // One-shot keyboard Tab → ImGui TabBar (never SetSelected every frame — imgui#7835).
    int queued_tool_tab = -1;
    // One-shot "open the BeamNG Carreteras tab" (set when a road is picked with the mouse in the 3D view).
    bool bng_roads_tab_selected = false;
    // BeamNG terrain brush (mod_beamng_terrain): while set, LMB drags edit the .ter instead of selecting.
    bool bng_terrain_brush = false;
    bool bng_terrain_tab_selected = false;
    // In-app catalog picker (no Win32 GetOpenFileName — see docs/MAP_EDITOR_CATALOG_PICKER.md).
    bool catalog_browser_open = false;
    bool catalog_browser_scan_dir = false;
    bool catalog_browser_spline = false;
    // When set, catalog browser picks feed the active aerosol list instead of the main catalog.
    bool catalog_browser_to_spray = false;
    bool catalog_browser_to_spawn_random = false;
    bool catalog_browser_to_replace = false; // catalog browser result feeds "replace all instances"
    bool water_visible = true;
    float water_h_ui[4]{0.f, 0.f, 0.f, 0.f};
    int water_ui_tile = -1;                  // tile the sliders were last synced with
    bool group_list_open = false;
    bool group_attach_popup = false;         // asks plain / with attachments before select-all-same
    bool group_gesture_open = false;            // "edit as list" window open (loop fills group_rows_live)
    std::vector<EditorSidePanelState::GroupRow> group_list_edit; // rows being edited in the window
    bool group_list_dirty = true;
    float group_delta_ui[4]{};
    float group_ui_center[3]{};
    float group_ui_rot[3]{};
    bool group_about_center_ui = true;
    float group_z_ui = 0.f;
    std::filesystem::path catalog_browser_cwd;
    std::filesystem::path catalog_browser_selected;
    char catalog_browser_filter[128]{};
    int catalog_browser_sort = 0; // 0 nombre 1 fecha 2 tamaño
    bool catalog_browser_sort_desc = false;
    // Tile tab — global.cfg [name] / [friendlyname] (UTF-16 safe write on Apply).
    char map_name_buf[256]{};
    char map_friendly_buf[256]{};
    bool map_names_seeded = false;
    bool shortcuts_editor_open = false;
    bool options_editor_open = false;
    bool options_editor_seeded = false;
    bool options_draft_vsync = false; // monitor vertical sync (Present interval 1)
    int options_draft_tile_radius = 1;
    int options_draft_visual_m = 300;
    float options_draft_min_screen_pct = 0.6f;
    float options_draft_union_pct = 0.02f;
    float options_draft_grad_pct = 0.01f;
    int options_snap_tile_radius = 1;
    int options_snap_visual_m = 300;
    float options_snap_min_screen_pct = 0.6f;
    float options_snap_union_pct = 0.02f;
    float options_snap_grad_pct = 0.01f;
    bool options_snap_vsync = true;
    int options_snap_path_style = 0;
};

class EditorSidePanel {
public:
    // OMSI content ~280 + vertical tabs ~72
    static constexpr int kPanelWidth = 360;
    static constexpr int kTabStripW = 72;
    static constexpr int kToolbarH = 28;

    bool open(HWND owner);
    void close();
    void set_entrypoints(const std::vector<map::EntryPoint>& entries);
    void set_scene_items(const std::vector<EditorSceneItem>& items);
    void set_object_list(const std::vector<EditorListItem>& items);
    void set_spline_list(const std::vector<EditorListItem>& items);
    void set_catalog_objects(const std::vector<EditorListItem>& items);
    void set_catalog_splines(const std::vector<EditorListItem>& items);
    void show_object(const EditorObjectProps& props);
    void update_object_text_slots(const EditorObjectProps& props);
    void update_object_live_values(const EditorObjectProps& props);
    [[nodiscard]] bool object_spline_tangential() const;
    void show_spline(const EditorSplineProps& props);
    void set_spline_track_atten_y(float world_y);
    void show_spline_fields(const EditorSplineProps& props);
    void update_spline_live_values(const EditorSplineProps& props);
    void clear_spline_grad_undo_pending();
    void show_path_rule(const EditorPathRuleProps& props);
    void hide_path_rule();
    
    // .sco path editor: the run loop owns the state and hands its address to the panel each frame.
    void set_path_editor_state(omsi::app::map_editor::PathEditorState* st) { path_editor_ = st; }
    [[nodiscard]] omsi::app::map_editor::PathEditorState* path_editor() const { return path_editor_; }
    [[nodiscard]] bool is_in_sco_path_edit_mode() const { return path_editor_ && path_editor_->active; }
    bool sco_path_pick_pending() const { return pending_.sco_path_pick_pending; }
    void begin_sco_path_pick() { pending_.sco_path_pick_pending = true; }
    void clear_sco_path_pick() { pending_.sco_path_pick_pending = false; }
    void set_status(const std::string& status);
    void set_selection_text(const std::string& text);
    void set_autofill(bool enabled);
    void set_tile_page(const EditorTilePageInfo& info, const map::EditorGeoSettings& geo);
    void read_tile_page_edits(map::EditorGeoSettings& out) const;
    void read_tile_page_flags(bool& tile_aerial_visible, int& aerial_resolution_pos) const;
    void set_tool(EditorTool tool);
    void queue_imgui_tool_tab(int tab_index);
    void clear_selection_props();
    void apply_ui_settings(const map::EditorUiSettings& ui);
    void collect_ui_settings(map::EditorUiSettings& out) const;
    void remember_catalog_path(const std::filesystem::path& path);
    [[nodiscard]] bool random_tile_stress_enabled() const;
    void pump();
    [[nodiscard]] EditorSidePanelState take_events();
    // ImGui end-of-frame: drain catalog modal flags only (do not steal apply/undo/etc.).
    [[nodiscard]] EditorSidePanelState drain_catalog_pending();
    [[nodiscard]] bool pointer_over_panel() const;
    [[nodiscard]] bool keyboard_captured() const;
    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    [[nodiscard]] int panel_width() const { return kPanelWidth; }
    [[nodiscard]] int catalog_object_sel() const;
    [[nodiscard]] int catalog_spline_sel() const;
    [[nodiscard]] std::filesystem::path catalog_selected_path() const;
    // Random .sco for N-place when imgui_state().spawn_random (empty if pool vacío).
    [[nodiscard]] std::filesystem::path pick_spawn_random_sco_path();
    // Active RadioGroup2 brush for OMSI-style path paint (Traffic Rules tab).
    [[nodiscard]] bool traffic_paint_brush(EditorPathRuleProps& out) const;
    // Terrain tab brush (only meaningful when tool == Terrain).
    [[nodiscard]] bool terrain_paint_brush(TerrainBrushUi& out) const;
    void set_ground_layers(const std::vector<std::string>& labels, int sel);
    void set_chrono_list(const std::vector<std::string>& labels);
    void set_uvg_groups(const std::vector<std::string>& names);
    // Populate combos + entry/station lists from TimeTableMan (Unit134 format).
    void set_tt_from_man();
    void set_tt_lists(const std::vector<std::filesystem::path>& tracks, const std::vector<std::filesystem::path>& trips,
                      const std::vector<std::filesystem::path>& station,
                      const std::vector<std::filesystem::path>& signals);
    [[nodiscard]] int ground_layer_sel() const;
    [[nodiscard]] int chrono_sel() const;
    [[nodiscard]] std::filesystem::path tt_selected_path() const;
    [[nodiscard]] float spline_max_radius() const;
    [[nodiscard]] int attach_point() const;
    [[nodiscard]] std::int32_t parent_id_field() const;
    bool try_apply_on_enter();

    // Dear ImGui primary UI (hides Win32 panel; edits use cached buffers below).
    void set_imgui_primary(bool on);
    [[nodiscard]] bool imgui_primary() const { return imgui_primary_; }
    [[nodiscard]] EditorTool current_tool() const { return tool_; }
    void imgui_set_tool(EditorTool tool);
    void imgui_set_catalog_sel(int object_sel, int spline_sel);
    [[nodiscard]] const std::vector<EditorListItem>& catalog_objects() const { return catalog_objects_; }
    [[nodiscard]] const std::vector<EditorListItem>& catalog_splines() const { return catalog_splines_; }
    [[nodiscard]] const std::vector<EditorListItem>& object_instances() const { return object_list_; }
    [[nodiscard]] const std::vector<EditorListItem>& spline_instances() const { return spline_list_; }
    [[nodiscard]] const std::vector<std::filesystem::path>& tt_paths() const { return tt_paths_; }
    EditorObjectProps& imgui_object_edit() { return imgui_object_; }
    EditorSplineProps& imgui_spline_edit() { return imgui_spline_; }
    EditorPathRuleProps& imgui_path_rule_edit() { return imgui_path_rule_; }
    EditorSidePanelImGuiState& imgui_state() { return imgui_state_; }
    [[nodiscard]] const EditorSidePanelImGuiState& imgui_state() const { return imgui_state_; }
    
    [[nodiscard]] std::string status_message() const { return status_text_; }
    [[nodiscard]] std::string selection_message() const { return selection_text_; }
    void imgui_merge_pending(const EditorSidePanelState& ev);
    void arm_spline_grad_undo();
    void arm_object_edit_undo();

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK toolbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void layout_children();
    void sync_page_visibility();
    void refill_catalog();
    void refill_instances();
    void paint_tab_strip(HDC hdc);
    int hit_tab(int x, int y) const;
    void read_object_edits(EditorObjectProps& out) const;
    void read_spline_edits(EditorSplineProps& out) const;
    void read_traffic_rule_edits(EditorPathRuleProps& out) const;
    void set_edit_float(HWND edit, float v) const;
    [[nodiscard]] float get_edit_float(HWND edit, float fallback) const;
    void set_grad_controls(HWND track, HWND edit, float pct);
    [[nodiscard]] float get_grad_value(HWND track, HWND edit, float fallback) const;
    void sync_grad_track_to_edit(HWND track, HWND edit) const;
    void sync_grad_edit_to_track(HWND track, HWND edit) const;
    void set_spline_scalar_tracks(const EditorSplineProps& props);
    void handle_spline_track_hscroll(HWND track, WPARAM wp);
    [[nodiscard]] bool is_spline_smooth_track(HWND track) const;
    bool handle_spline_track_thumb_changing(HWND track, int raw_pos, int reason);
    void display_spline_track_value(HWND track, float v);
    void finish_spline_track_drag();
    void tick_spline_track_smooth(float dt_seconds);
    void sync_spline_scalar_edit(int control_id);
    void set_scalar_field(HWND track, HWND edit, float min_v, float max_v, float step, int decimals, float v);
    [[nodiscard]] float get_scalar_field(HWND track, HWND edit, float min_v, float max_v, float step,
                                         float fallback) const;
    void nudge_grad(HWND track, HWND edit, float delta);
    void nudge_z(float delta);
    void handle_spline_nudge_btn(int btn_id);
    void build_objects_page(HWND page);
    void build_splines_page(HWND page);
    void build_traffic_page(HWND page);
    void build_tile_page(HWND page);
    void build_terrain_page(HWND page);
    void build_tracks_page(HWND page);
    void build_station_page(HWND page);
    void build_splexp_page(HWND page);
    void build_signal_page(HWND page);
    void build_stub_page(HWND page, const wchar_t* title);

    HWND owner_ = nullptr;
    HWND toolbar_ = nullptr;
    HWND btn_load_map_ = nullptr;
    HWND btn_shortcuts_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tab_strip_ = nullptr;
    HWND status_ = nullptr;

    HWND page_tile_ = nullptr;
    HWND page_object_ = nullptr;
    HWND page_spline_ = nullptr;
    HWND page_terrain_ = nullptr;
    HWND terr_brush_size_ = nullptr;
    HWND terr_speed_ = nullptr;
    HWND terr_set_h_ = nullptr;
    HWND terr_radio_higher_ = nullptr;
    HWND terr_radio_lower_ = nullptr;
    HWND terr_radio_set_ = nullptr;
    HWND terr_radio_paint_ = nullptr;
    HWND terr_radio_rubber_ = nullptr;
    HWND terr_layers_ = nullptr;
    HWND terr_btn_layer_new_ = nullptr;
    HWND terr_btn_layer_edit_ = nullptr;
    HWND terr_btn_layer_del_ = nullptr;
    HWND page_traffic_ = nullptr;
    HWND page_tracks_ = nullptr;
    HWND page_station_ = nullptr;
    HWND page_splexp_ = nullptr;
    HWND page_signal_ = nullptr;
    HWND tracks_list_ = nullptr;
    HWND trips_list_ = nullptr;
    HWND tracks_combo_ = nullptr;
    HWND tt_length_label_ = nullptr;
    HWND trips_combo_ = nullptr;
    HWND tt_terminus_ = nullptr;
    HWND tt_line_ = nullptr;
    HWND tt_train_rev_ = nullptr;
    HWND tt_mode0_ = nullptr;
    HWND tt_mode1_ = nullptr;
    HWND tt_mode2_ = nullptr;
    HWND station_list_ = nullptr;
    HWND sl_links_ = nullptr;
    HWND sl_segments_ = nullptr;
    HWND sl_log_ = nullptr;
    HWND sl_mode_start_ = nullptr;
    HWND sl_mode_end_ = nullptr;
    HWND sl_mode_path_ = nullptr;
    HWND signal_list_ = nullptr;
    HWND chrono_list_ = nullptr;
    HWND t_btn_lightmap_ = nullptr;
    HWND t_btn_whole_map_ = nullptr;
    HWND t_btn_aerial_miss_ = nullptr;
    HWND t_btn_aerial_all_ = nullptr;
    HWND t_btn_chrono_plus_ = nullptr;
    HWND t_btn_chrono_minus_ = nullptr;
    HWND t_btn_chrono_rem_ = nullptr;
    HWND t_bake_lbl_ = nullptr;
    HWND t_bake_parts_ = nullptr;
    HWND t_btn_bake_o3d_ = nullptr;
    HWND t_btn_bake_replace_ = nullptr;
    HWND t_btn_bake_fbx_ = nullptr;
    HWND t_bake_status_ = nullptr;

    // Tile tab — world coords + Google geo
    HWND t_coord_lbl_ = nullptr;
    HWND t_tile_lbl_ = nullptr;
    HWND t_tile_hint_ = nullptr;
    HWND t_btn_convert_ = nullptr;
    HWND t_aerial_grp_ = nullptr;
    HWND t_aerial_vis_ = nullptr;
    HWND t_aerial_res_lbl_ = nullptr;
    HWND t_aerial_res_ = nullptr;
    HWND t_btn_aerial_dnl_ = nullptr;
    HWND t_btn_aerial_del_ = nullptr;
    HWND t_sat_lbl_ = nullptr;
    HWND t_sat_provider_ = nullptr;
    HWND t_link_lbl_ = nullptr;
    HWND t_link_aerial_ = nullptr;
    HWND t_custom_preset_lbl_ = nullptr;
    HWND t_custom_preset_ = nullptr;
    HWND t_dtm_lbl_ = nullptr;
    HWND t_dtm_provider_ = nullptr;
    HWND t_api_lbl_ = nullptr;
    HWND t_api_key_ = nullptr;
    HWND t_ref_lbl_ = nullptr;
    HWND t_ref_lat_ = nullptr;
    HWND t_ref_lon_ = nullptr;
    HWND t_btn_dtm_ = nullptr;
    HWND t_chk_spawn_terr_ = nullptr;
    HWND t_chk_spawn_mesh_ = nullptr;
    HWND t_chk_spawn_align_ = nullptr;
    HWND t_btn_create_tile_ = nullptr;
    HWND t_btn_delete_tile_ = nullptr;

    // Objects (OMSI layout)
    HWND o_tile_lbl_ = nullptr;
    HWND o_tile_search_ = nullptr;
    HWND o_cat_search_ = nullptr;
    HWND o_inst_search_ = nullptr;
    HWND o_types_lbl_ = nullptr;
    HWND o_catalog_ = nullptr;
    HWND o_btn_load_ = nullptr;
    HWND o_chk_random_ = nullptr;
    HWND o_btn_loaddir_ = nullptr;
    HWND o_grp_ = nullptr;
    HWND o_x_ = nullptr;
    HWND o_y_ = nullptr;
    HWND o_z_ = nullptr;
    HWND o_rot_ = nullptr;
    HWND o_name_ = nullptr;
    HWND o_pitch_ = nullptr;
    HWND o_bank_ = nullptr;
    HWND o_btn_attach_obj_ = nullptr;
    HWND o_att_pnt_ = nullptr;
    HWND o_btn_attach_sli_ = nullptr;
    HWND o_count_lbl_ = nullptr;
    HWND o_dist_ = nullptr;
    HWND o_range_ = nullptr;
    HWND o_chk_tangential_ = nullptr;
    HWND o_chk_align_terr_ = nullptr;
    HWND o_btn_autorot_ = nullptr;
    HWND o_btn_parent_ = nullptr;
    HWND o_parent_ = nullptr;
    HWND o_btn_apply_ = nullptr;
    HWND o_btn_strings_ = nullptr;
    static constexpr int kMaxSplineTextSlots = 16;
    HWND o_slots_grp_ = nullptr;
    HWND o_slot_lbl_[kMaxSplineTextSlots]{};
    HWND o_slot_edits_[kMaxSplineTextSlots]{};
    int o_obj_grp_y_ = 0;
    int o_obj_grp_base_h_ = 210;
    int o_slots_base_y_ = 0;
    int o_object_tail_y_ = 0;
    HWND o_obj_search_lbl_ = nullptr;
    HWND o_obj_list_lbl_ = nullptr;
    HWND o_instances_ = nullptr;
    HWND o_chk_spawn_terr_ = nullptr;
    HWND o_chk_spawn_mesh_ = nullptr;
    HWND o_chk_spawn_align_ = nullptr;

    // Splines
    HWND s_tile_lbl_ = nullptr;
    HWND s_tile_search_ = nullptr;
    HWND s_cat_search_ = nullptr;
    HWND s_inst_search_ = nullptr;
    HWND s_types_lbl_ = nullptr;
    HWND s_catalog_ = nullptr;
    HWND s_btn_load_ = nullptr;
    HWND s_btn_create_ = nullptr;
    HWND s_btn_parent_ = nullptr;
    HWND s_btn_split_ = nullptr;
    HWND s_z_ = nullptr;
    HWND s_rot_ = nullptr;
    HWND s_rot_trk_ = nullptr;
    HWND s_skew0_ = nullptr;
    HWND s_skew0_trk_ = nullptr;
    HWND s_skew1_ = nullptr;
    HWND s_skew1_trk_ = nullptr;
    HWND s_len_ = nullptr;
    HWND s_len_trk_ = nullptr;
    HWND s_angle_ = nullptr;
    HWND s_radius_ = nullptr;
    HWND s_rad_trk_ = nullptr;
    HWND s_btn_align_start_ = nullptr;
    HWND s_btn_align_end_ = nullptr;
    HWND s_g0_ = nullptr;
    HWND s_g0_edit_ = nullptr;
    HWND s_g0_up_ = nullptr;
    HWND s_g0_dn_ = nullptr;
    HWND s_g1_ = nullptr;
    HWND s_g1_edit_ = nullptr;
    HWND s_g1_up_ = nullptr;
    HWND s_g1_dn_ = nullptr;
    HWND s_dh_check_ = nullptr;
    HWND s_dh_ = nullptr;
    HWND s_cant0_ = nullptr;
    HWND s_cant0_trk_ = nullptr;
    HWND s_cant1_ = nullptr;
    HWND s_cant1_trk_ = nullptr;
    HWND s_mirror_ = nullptr;
    HWND s_align_terr_ = nullptr;
    HWND s_btn_complete_ = nullptr;
    HWND s_max_radius_ = nullptr;
    HWND s_btn_apply_ = nullptr;
    HWND s_instances_ = nullptr;
    HWND s_chk_ends_ = nullptr;
    HWND s_chk_path_ends_ = nullptr;
    HWND s_chk_path_rails_ = nullptr;
    HWND s_chk_autofill_ = nullptr;
    HWND s_chk_spawn_terr_ = nullptr;
    HWND s_chk_spawn_mesh_ = nullptr;
    HWND s_chk_spawn_align_ = nullptr;

    // Traffic Rules
    HWND t_uvg_lbl_ = nullptr;
    HWND t_uvg_list_ = nullptr;
    HWND t_grp_ = nullptr;
    HWND t_speed_ = nullptr;
    std::vector<HWND> t_radios_;
    HWND t_btn_apply_ = nullptr;
    HWND t_hint_ = nullptr;

    HBRUSH bg_brush_ = nullptr;
    HFONT font_ = nullptr;
    HFONT font_vert_ = nullptr;

    EditorObjectProps last_object_{};
    EditorSplineProps last_spline_{};
    EditorPathRuleProps last_path_rule_{};

    std::vector<map::EntryPoint> entries_data_;
    std::vector<EditorSceneItem> scene_data_;
    std::vector<EditorListItem> object_list_;
    std::vector<EditorListItem> spline_list_;
    std::vector<EditorListItem> catalog_objects_;
    std::vector<EditorListItem> catalog_splines_;
    std::vector<std::filesystem::path> tt_paths_;
    EditorSidePanelState pending_{};
    EditorTool tool_ = EditorTool::Object;
    int catalog_obj_sel_ = -1;
    int catalog_sli_sel_ = -1;
    mutable bool suppress_grad_notify_ = false;
    bool grad_edit_armed_ = true;
    float grad_smooth_g0_ = 0.f;
    float grad_smooth_g1_ = 0.f;
    HWND track_drag_hwnd_ = nullptr;
    int track_drag_anchor_pos_ = 0;
    int track_drag_anchor_mouse_x_ = 0;
    float track_drag_anchor_val_ = 0.f;
    float spline_track_atten_y_ = 0.f;
    float track_smooth_value_ = 0.f;
    float track_smooth_target_ = 0.f;
    bool track_smooth_active_ = false;
    std::chrono::steady_clock::time_point track_smooth_last_tick_{};
    // layout cache: children are only moved when the owner's client size changes (15 MoveWindow + 2
    // BringWindowToTop every frame cost ~7 ms of the editor frame)
    int layout_ow_ = -1, layout_oh_ = -1;
    bool layout_force_ = true;
    std::chrono::steady_clock::time_point z_order_last_{};
    bool spline_grad_undo_pending_ = false;
    bool object_edit_undo_pending_ = false;
    bool object_edit_armed_ = true;
    int ground_layer_sel_ = 0;
    std::vector<int> filtered_object_indices_;
    std::vector<int> filtered_spline_indices_;
    std::vector<int> filtered_catalog_object_indices_;
    std::vector<int> filtered_catalog_spline_indices_;
    bool imgui_primary_ = false;
    omsi::app::map_editor::PathEditorState* path_editor_ = nullptr;
    EditorSidePanelImGuiState imgui_state_{};
    EditorObjectProps imgui_object_{};
    EditorSplineProps imgui_spline_{};
    EditorPathRuleProps imgui_path_rule_{};
    std::string status_text_;
    std::string selection_text_;
    [[nodiscard]] int filtered_object_index(int list_sel) const;
    [[nodiscard]] int filtered_spline_index(int list_sel) const;
    [[nodiscard]] int filtered_catalog_object_index(int list_sel) const;
    [[nodiscard]] int filtered_catalog_spline_index(int list_sel) const;
    [[nodiscard]] static std::wstring edit_text(HWND edit);
    [[nodiscard]] static bool item_matches_search(const std::wstring& filter, const EditorListItem& it);
    [[nodiscard]] static bool item_matches_tile(const std::wstring& filter, const EditorListItem& it);
    void sync_spawn_height_checks(bool stick_terrain, bool spawn_on_meshes) const;
    void on_spawn_height_clicked(int id);
};

} // namespace omsi::ui
