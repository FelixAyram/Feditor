#pragma once

#include "omsi/bus_catalog.hpp"
#include "omsi/map_geo_tools.hpp"
#include "ui/editor_side_panel.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiViewport;

namespace omsi::ui {

struct EditorShortcutTable;

struct SplineAttachHudItem {
    std::int32_t object_id = 0;
    std::string label;
};

// Height/rotation HUD while G+Z or R (ImGui replaces Win32 popup when imgui_primary).
struct EditorAdjustHudImGui {
    bool active = false;
    int mode = 0; // 1 = height (Z), 2 = rotation (yaw)
    int seed_mode = 0; // 0=none 1=height 2=rotation (latch once per edit session)
    bool user_editing = false;
    float previous = 0.f;
    float current = 0.f;
    bool restore_requested = false;
    bool commit_pending = false;
    float commit_value = 0.f;
};

struct MapEditorImGuiArgs {
    EditorSidePanel* panel = nullptr;
    // BeamNG.drive session: OMSI-only tabs are hidden; these draw the BeamNG tabs.
    bool beamng_mode = false;
    std::function<void()> bng_scene_tab;
    std::function<void()> bng_quality_tab;
    std::function<void()> bng_roads_tab;
    std::function<void()> bng_terrain_tab; // Terreno: heightmap + capas del .ter
    std::function<void()> bng_vegetation_tab; // Vegetacion: arboles del forest + pasto (GroundCover)
    std::string bng_status;
    EditorTilePageInfo tile_info{};
    const char* map_name = nullptr;
    const char* map_friendly_name = nullptr;
    std::size_t tile_count = 0;
    std::size_t object_count = 0;
    std::size_t spline_count = 0;
    int selected_object_index = -1;
    int selected_spline_index = -1;
    std::int32_t spline_attach_parent_id = 0;
    std::vector<SplineAttachHudItem> spline_attach_hud;
    // Catálogo: browser ImGui (sin GetOpenFileName). for_spline = herramienta Spline activa.
    std::function<void(std::filesystem::path path, bool for_spline)> catalog_on_file_picked;
    std::function<void(std::filesystem::path dir, bool for_spline)> catalog_on_dir_picked;
    std::filesystem::path omsi_root;
    EditorAdjustHudImGui* adjust_hud = nullptr;
    EditorShortcutTable* shortcuts = nullptr;
    std::size_t spline_group_count = 0;
    std::size_t object_group_count = 0;
    // Water of the tile under the camera + ground layer textures (Terrain tab: Agua / Capas).
    bool water_tile_valid = false;
    int water_tile_gx = 0, water_tile_gy = 0;
    bool water_has = false;
    float water_h[4]{};
    std::vector<std::filesystem::path> ground_layer_diffuse;
    std::vector<float> ground_layer_scale;
    std::vector<std::filesystem::path> ground_layer_detail;
    std::vector<int> ground_layer_main_rep;
    // Multi-selection summary (centre of every selected item, editor coords) for the group window.
    float group_center[3]{};
    std::string group_same_name;           // file name shared by the primary selection ("pepito.sli"), "" if none
    std::size_t group_same_attach = 0;     // how many of those .sco instances are spline attachments
    bool group_has_attach = false;         // the current group contains spline attachments
    float group_mean_rot[3]{};             // mean pitch / rot / bank of the selected .sco (rot = mean heading of .sli)
    std::size_t group_same_total = 0;      // how many instances of that file exist in the map
    std::vector<omsi::ui::EditorSidePanelState::GroupRow> group_rows_live; // filled only while the list window is open
    std::string edit_mode_label;
    bool sli_bake_pick_active = false;
    bool sli_bake_ready = false;
    std::string sli_bake_label;
    const std::vector<omsi::bus_catalog::BusEntry>* bus_catalog = nullptr;
    std::string last_omsi_bus;
    // Live options seed for ImGui "Opciones del editor" modal.
    int options_tile_radius = 1;
    float options_visual_radius_m = 300.f;
    float options_union_pct = 0.02f;
    float options_grad_pct = 0.01f;
    map::EditorUiSettings* editor_ui_settings = nullptr;
    // Solo visualizar: terrain / tile mutations are disabled in the UI and blocked in the run loop.
    bool view_only = false;
};

void load_editor_docks(EditorSidePanelImGuiState& st);
void save_editor_docks(const EditorSidePanelImGuiState& st);
void invalidate_catalog_thumb_cache(const std::filesystem::path& src = {});
// Evict one on-disk catalog thumb path from the GPU SRV LRU (see set_thumb_texture_provider).
void set_catalog_thumb_srv_evictor(std::function<void(const std::filesystem::path&)> fn);

void catalog_browser_open(EditorSidePanelImGuiState& st, const std::filesystem::path& start_dir, bool for_spline,
                          bool pick_directory);
void rebuild_spawn_random_folder_pool(EditorSidePanelImGuiState& st);

void catalog_browser_draw(const MapEditorImGuiArgs& args);

bool browser_name_matches(std::string_view name, std::string_view filt_lower);
bool browser_dir_has_match(const std::filesystem::path& dir, std::string_view filt_lower,
                           const std::function<bool(const std::filesystem::path&)>& file_ok, int depth,
                           int& budget);
void browser_index_ensure(const std::filesystem::path& root);
[[nodiscard]] bool browser_index_ready(const std::filesystem::path& root);
[[nodiscard]] bool browser_index_busy();

void build_map_editor_imgui_ui(const MapEditorImGuiArgs& args);

// Path editor tab body (Paths / Semaforos / Props / Texturas). Uses panel.path_editor() state.
void draw_path_editor_tab(EditorSidePanel& panel);

// Floating window layout: the small tool windows (Attachados, Info .sco, Seleccion, ...) dock into slots of a
// column on the right edge, in `order`, and slide to their new slot (animated) whenever one opens, closes or
// changes size, so none covers another; the open .map viewer is kept clear. Call float_slot_begin right before
// ImGui::Begin and float_slot_end right after it (whatever Begin returned). A window not drawn for a frame
// gives its slot up.
void float_slot_begin(const char* key, int order);
void float_slot_end();
// Once per frame (the panel build calls it): starts / draws the close animation of windows that went away.
void float_layout_frame();
// tga path -> ImTextureID (0 = not ready). Thumbnails fall back to per-pixel draw while 0.
void set_thumb_texture_provider(std::function<std::uint64_t(const std::filesystem::path&)> fn);
// A texture file's small preview (decoded on a worker the first time it is asked for). id 0 while loading.
struct TexturePreview {
    std::uint64_t id = 0;
    int w = 0, h = 0;
    bool failed = false;
};
[[nodiscard]] TexturePreview texture_preview(const std::filesystem::path& tex);
// Test captures: draw no editor UI at all while set.
void set_hide_all_ui(bool hide);
// Extra "Mesh .o3d" tab next to the tool tabs, only while active() (Bake / Edit mesh tool).
void set_mesh_edit_tab(std::function<bool()> active, std::function<void()> draw);
// co-op session tab ("Sesion", shown between Tile and Objeto while Ver > Sesion cooperativa is on)
void set_coop_session_tab(std::function<void()> draw);

} // namespace omsi::ui
