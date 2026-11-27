#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "omsi/map_tile_layout.hpp"
#include "omsi/map_types.hpp"

namespace omsi::map {

enum class MapCoordKind { Classic, WorldCoordinates, GlobalTileNames };

// Satellite / orthophoto sources (Web Mercator EPSG:3857 tile URLs unless noted).
enum class SatelliteProvider {
    EsriWorldImagery = 0,  // Esri World Imagery orthophoto (free, global)
    EoxSentinel2 = 1,      // EOX Sentinel-2 cloudless (free)
    OmsiLinkAerial = 2,    // OMSI options.cfg [Editor_LinkAerial] ~x~y~z
    MapTiler = 3,          // MapTiler satellite (free tier key)
    GoogleMapTiles = 4,    // Google Map Tiles API (paid, session tiles)
    OpenStreetMap = 5,     // OSM street map (not satellite)
    BingAerial = 6,        // Bing Maps aerial (Azure key)
    GoogleStaticMaps = 7,  // Google Maps Static API satellite (your API key)
    GeoportalPlOrto = 8,   // Geoportal.gov.pl orthophoto WMTS standard (Poland)
    GeoportalPlOrtoHiRes = 9, // Geoportal.gov.pl orthophoto WMTS hi-res (Poland)
    CustomAerialUrl = 10,  // User URL template with {z}/{x}/{y} + preset list
    GcbaAerial2017 = 11,    // Buenos Aires CABA orthophoto 2017 (GCBA, TMS)
    IgnArgentinaVant = 12, // IGN Argentina UAV orthophoto mosaic (TMS)
    // Native OMSI Editor_LinkAerial via GCMods proxy (https://www.gcmods.de/luftbilder/...)
    GcmodsBing = 13,       // aerial.gcmods.de Bing satellite (Azure/Bing key)
    GcmodsGoogle = 14,     // aerial.gcmods.de Google satellite hres=1 (Google key; 256 px)
    // Experimental: unofficial Google Maps satellite (mt*.google.com lyrs=s&scale=2/4) — no API key.
    // scale=2/4 yields 512/1024 px tiles (sharper than upscaling 256). Violates ToS; local test only.
    GoogleMtSatellite = 15,
};

enum class ElevationProvider {
    OpenTopoData = 0,   // OpenTopoData ASTER 30m (free, global; 1 req/s)
    Google = 1,         // Google Elevation API (paid)
    NasaSrtm3 = 2,      // NASA SRTM via OpenTopoData srtm30m (~30 m, global)
    GeoportalPlNmt = 3, // Geoportal NMT WCS 1 m (Poland only)
    Terrarium = 4,      // tilezen/joerd Terrarium PNG XYZ (free)
    OpenMeteo = 5,      // Open-Meteo Copernicus GLO-90 (free)
    OpenElevation = 6,  // Open-Elevation public API
    Mapterhorn = 7,     // tiles.mapterhorn.com Terrarium WebP 512
    CopernicusGlo30 = 8, // AWS Copernicus DSM GLO-30 COG Float32
    Fabdem = 9,          // FABDEM V1-2 DTM (buildings/trees removed)
};

// Preset templates for CustomAerialUrl (see custom_aerial_preset_url).
enum class CustomAerialPreset {
    User = 0,
    EsriWorldImagery = 1,
    OpenStreetMap = 2,
    EoxSentinel2 = 3,
    GeoportalPlOrto = 4,
    GeoportalPlOrtoHiRes = 5,
    BingAerialTemplate = 6,
    GcbaAerial2017 = 7,
    IgnArgentinaVant = 8,
};

struct TileWgs84Bounds {
    double south = 0.;
    double west = 0.;
    double north = 0.;
    double east = 0.;
};

struct EditorGeoSettings {
    SatelliteProvider satellite_provider = SatelliteProvider::EsriWorldImagery;
    ElevationProvider elevation_provider = ElevationProvider::OpenTopoData;
    std::string maptiler_api_key;
    std::string bing_api_key;
    std::string google_api_key;
    double ref_lat = 0.;
    double ref_lon = 0.;
    // OMSI options.cfg [Editor_LinkAerial] — URL template with ~x~y~z placeholders (GCMods default).
    std::string editor_link_aerial =
        "http://aerial.gcmods.de/toquad.php?x=~x&y=~y&z=~z&apicode=[API-Code]";
    std::string custom_aerial_url; // {z} {x} {y} {z3857} {apikey} — CustomAerialUrl
    int custom_aerial_preset = 0;
    bool tile_aerial_visible = false;
    int aerial_resolution_pos = 1; // TrackBar: 0..5 → 256..8192 px
};

// Global editor UI prefs — next to exe as omsi_f4_editor.cfg ([ui]/[vista]/[menu_steps]).
// Same settings for every map. Map folder .omsi_f4_editor.cfg keeps [geo]/aerial only.
struct EditorUiSettings {
    int tool = 1; // EditorTool::Object
    bool show_spline_ends = true;
    bool show_path_ends = true;
    bool show_path_rails = false;
    bool autofill_spline_ends = false;
    bool spline_stick_chain = false;
    bool spline_fill_chain = false;
    int terrain_brush_mode = 0; // 0=Higher .. 4=RubberLayer
    float terrain_brush_size_m = 20.f;
    float terrain_brush_speed = 3.f;
    float terrain_set_height = 0.f;
    int terrain_ground_layer = 0;
    bool spawn_stick_terrain = false;
    bool spawn_on_meshes = false;
    bool spawn_align_pitch_bank = false;
    // N-place random SCO (objects only): 0=catálogo, 1=favoritos, 2=carpeta escaneada.
    bool spawn_random = false;
    int spawn_random_mode = 0;
    int spawn_random_fav_cat = 0;
    std::filesystem::path spawn_random_folder;
    std::string last_omsi_bus;
    int editor_tile_radius = 1;
    float editor_visual_radius_m = 300.f;
    /// Objects (.sco/.sli chunks) whose projected size is below this % of screen height are not drawn. 0 = off.
    float min_screen_size_pct = 0.f; // (default 0: nothing culled by screen size)
    float union_ball_screen_pct = 0.02f;
    float grad_circle_screen_pct = 0.01f;
    bool tex_max_256 = false;
    bool tex_max_256_from_file = false;
    float sim_hour = 14.f;
    bool sim_hour_from_file = false; // [vista] sim_hour present: the user's hour wins over a BeamNG level's TimeOfDay
    int path_style = 0;               // 0 = classic, 1 = modern path rails
    bool hide_onlyeditor_splines = false;
    int language = 0;                 // 0 = español, 1 = english
    bool show_sco_origin = false;     // Vista: 2D marker at the selected .sco origin
    bool arrows_3d = false;           // Vista: path + spline arrows as one translucent 3D arrow
    bool hide_nodes = false;          // Vista: no spline/path nodes (not drawn, not pickable)
    int sim_month = 6;
    int sim_day = 21;
    int sim_year = 2026;       // (chrono scenarios are dated)
    bool time_running = false; // the clock runs (Vista > Hora)
    float time_speed = 1.f;    // times real time
    // Chrono preview (Vista > Cronos): the date whose scenarios are laid over the map, for chrono_map only
    // (0 = the plain map)
    int chrono_date = 0;
    std::string chrono_map;
    float timezone_h = 1.f;
    int weather_id = 0;
    bool show_sun_shadows = true;
    bool shaft_plus = false;          // light shafts: false = Normal (cheap), true = Plus (more steps, stronger)
    bool fog_enabled = true;          // Vista > Graficos > Niebla
    bool parked_cars_on = true;       // Vista > Car park real (off: the helper boxes)
    bool show_onlyeditor_objects = true; // Vista > Objetos onlyeditor ([onlyeditor] .sco, entrypoints)
    float shadow_distance_m = 0.f;    // Vista > Graficos > Distancia de sombras (0 = the visual radius)
    bool vsync = true; // monitor vertical sync (Present interval 1); saved with the other editor prefs
    std::uint32_t shadow_map_size = 4096;
    bool hints_ignore = false;
    bool place_feedback = true;
    float place_feedback_volume = 70.f;
    std::string sfx_pack = "blocky";
    // World (3D) sound of scenery objects; see omsi/sound/world_sound_window.hpp.
    bool world_sound_enabled = true;
    float world_sound_volume = 70.f; // 0..100
    std::unordered_map<std::string, float> field_steps;
};

struct OmsiAerialUrlCoords {
    int url_x = 0;
    int url_y = 0;
};

struct GeoJobResult {
    bool ok = false;
    std::string message;
    std::filesystem::path output_path;
};

[[nodiscard]] MapCoordKind detect_map_coord_kind(const GlobalMap& global);
[[nodiscard]] std::string map_coord_kind_label(MapCoordKind kind);

// TMapKachel+0xD0/+0xD4 from global.cfg [map] grid indices (Unit126 / Button33).
[[nodiscard]] OmsiAerialUrlCoords omsi_aerial_url_coords(std::int32_t grid_x, std::int32_t grid_y);
[[nodiscard]] int aerial_resolution_label_px(int trackbar_pos);
// Output square size for a provider + trackbar pos (matches download_tile_aerial_* writers).
[[nodiscard]] int effective_aerial_download_px(SatelliteProvider provider, int resolution_pos,
                                               const std::string& link_aerial = {});
[[nodiscard]] std::optional<std::pair<int, int>> read_jpeg_dimensions(const std::filesystem::path& jpg_path);
void remove_tile_aerial_files(const std::filesystem::path& omsi_root, const std::filesystem::path& map_root,
                              const std::filesystem::path& tile_path);
// Replace ~x/~y/~z and [API-Code]/[Key]/{apikey} — same as OMSI options.cfg [Editor_LinkAerial].
[[nodiscard]] std::string build_editor_aerial_url(const std::string& link_template, int url_x, int url_y,
                                                  int url_z, const std::string& api_key = {});
[[nodiscard]] const char* gcmods_bing_satellite_link();
[[nodiscard]] const char* gcmods_google_satellite_link();
[[nodiscard]] const char* gcmods_osm_street_link();
// OMSI 2: maps/aerials/<MapName>/tile_X_Y.aerial.jpg (shared across map copies).
[[nodiscard]] std::filesystem::path map_aerials_dir(const std::filesystem::path& omsi_root,
                                                    const std::filesystem::path& map_root);
[[nodiscard]] std::filesystem::path tile_aerial_jpg_path(const std::filesystem::path& omsi_root,
                                                         const std::filesystem::path& map_root,
                                                         const std::filesystem::path& tile_path);
[[nodiscard]] std::optional<std::filesystem::path> find_tile_aerial_jpg(const std::filesystem::path& omsi_root,
                                                                        const std::filesystem::path& map_root,
                                                                        const std::filesystem::path& tile_path);

// OMSI aerial download (Editor_LinkAerial mosaic) → maps\aerials\...\stem.aerial.jpg
[[nodiscard]] GeoJobResult download_tile_aerial_omsi(const std::string& link_template, std::int32_t grid_x,
                                                     std::int32_t grid_y, int resolution_pos,
                                                     const std::filesystem::path& out_jpg,
                                                     const std::string& api_key = {});

// WGS84 satellite mosaic → .aerial.jpg (Esri / EOX / MapTiler / Google).
[[nodiscard]] GeoJobResult download_tile_aerial_satellite(SatelliteProvider provider, const TileWgs84Bounds& bounds,
                                                          float tile_size_m, int output_px,
                                                          const std::filesystem::path& out_jpg,
                                                          const std::string& api_key = {},
                                                          const std::string& custom_url_template = {});

[[nodiscard]] const char* satellite_provider_label(SatelliteProvider p);
[[nodiscard]] const char* elevation_provider_label(ElevationProvider p);
[[nodiscard]] const char* custom_aerial_preset_label(CustomAerialPreset p);
[[nodiscard]] const char* custom_aerial_preset_url(CustomAerialPreset p);
[[nodiscard]] std::string expand_aerial_url_template(std::string tmpl, int z, int x, int y,
                                                     const std::string& api_key = {});
[[nodiscard]] bool bounds_in_poland(const TileWgs84Bounds& bounds);
[[nodiscard]] bool bounds_in_caba(const TileWgs84Bounds& bounds);
[[nodiscard]] bool bounds_in_argentina(const TileWgs84Bounds& bounds);
[[nodiscard]] std::string format_tile_wgs84_summary(const TileWgs84Bounds& bounds);
[[nodiscard]] bool write_aerial_sidecar(const std::filesystem::path& jpg_path, const std::string& source,
                                        const std::string& extra_meta = {});

// Append [worldcoordinates] to global.cfg if missing (OMSI Unit126).
[[nodiscard]] bool enable_world_coordinates(const std::filesystem::path& global_cfg);

[[nodiscard]] std::optional<TileWgs84Bounds> tile_wgs84_bounds(const GlobalMap& global, const MapTileLayout& layout,
                                                             std::int32_t grid_x, std::int32_t grid_y,
                                                             double ref_lat, double ref_lon);

[[nodiscard]] EditorGeoSettings load_editor_geo_settings(const std::filesystem::path& map_root);
[[nodiscard]] EditorUiSettings load_editor_ui_settings(const std::filesystem::path& map_root);
[[nodiscard]] bool save_editor_geo_settings(const std::filesystem::path& map_root, const EditorGeoSettings& s);
[[nodiscard]] bool save_editor_ui_settings(const std::filesystem::path& map_root, const EditorUiSettings& ui);

[[nodiscard]] bool write_terrain_heights(const std::filesystem::path& map_path, const std::vector<float>& heights);

[[nodiscard]] bool update_global_groundtex(const std::filesystem::path& global_cfg,
                                           const std::filesystem::path& diffuse_rel, float scale = 60.f);

// Append / remove a [groundtex] block in global.cfg (OMSI Ground Layers list).
[[nodiscard]] bool append_global_groundtex(const std::filesystem::path& global_cfg,
                                           const std::filesystem::path& diffuse_rel, float scale = 60.f);
// Full [groundtex] block (main, detail, flag, main repeat, detail repeat). index < 0 appends.
[[nodiscard]] bool write_global_groundtex(const std::filesystem::path& global_cfg, int index,
                                          const std::string& main_tex, const std::string& detail_tex, int flag,
                                          int main_rep, int detail_rep);
[[nodiscard]] bool delete_global_groundtex_at(const std::filesystem::path& global_cfg, int index);

// Google Map Tiles API (satellite) → 512×512 BMP beside map texture folder.
[[nodiscard]] GeoJobResult download_tile_aerial_bmp(const std::string& api_key, const TileWgs84Bounds& bounds,
                                                    float tile_size_m, const std::filesystem::path& out_bmp);

// Google Maps Static API (satellite, visible=bounds) → JPEG.
[[nodiscard]] GeoJobResult download_tile_aerial_google_static(const std::string& api_key,
                                                              const TileWgs84Bounds& bounds, int output_px,
                                                              const std::filesystem::path& out_jpg);

// Google Elevation API → 61×61 absolute heights (meters).
[[nodiscard]] GeoJobResult download_tile_dtm(const std::string& api_key, const TileWgs84Bounds& bounds,
                                             int dim, std::vector<float>& out_heights);

// OpenTopoData ASTER 30m (free, no key).
[[nodiscard]] GeoJobResult download_tile_dtm_opentopodata(const TileWgs84Bounds& bounds, int dim,
                                                          std::vector<float>& out_heights);

// NASA SRTM (OpenTopoData srtm30m).
[[nodiscard]] GeoJobResult download_tile_dtm_srtm3(const TileWgs84Bounds& bounds, int dim,
                                                 std::vector<float>& out_heights);

// Geoportal.gov.pl NMT WCS (~1 m, Poland only).
[[nodiscard]] GeoJobResult download_tile_dtm_geoportal_pl(const TileWgs84Bounds& bounds, int dim,
                                                          std::vector<float>& out_heights);

[[nodiscard]] GeoJobResult download_tile_dtm_terrarium(const TileWgs84Bounds& bounds, int dim,
                                                       std::vector<float>& out_heights);
[[nodiscard]] GeoJobResult download_tile_dtm_mapterhorn(const TileWgs84Bounds& bounds, int dim,
                                                        std::vector<float>& out_heights);
[[nodiscard]] GeoJobResult download_tile_dtm_openmeteo(const TileWgs84Bounds& bounds, int dim,
                                                       std::vector<float>& out_heights);
[[nodiscard]] GeoJobResult download_tile_dtm_open_elevation(const TileWgs84Bounds& bounds, int dim,
                                                            std::vector<float>& out_heights);
[[nodiscard]] GeoJobResult download_tile_dtm_copernicus_glo30(const TileWgs84Bounds& bounds, int dim,
                                                              std::vector<float>& out_heights);
[[nodiscard]] GeoJobResult download_tile_dtm_fabdem(const TileWgs84Bounds& bounds, int dim,
                                                    std::vector<float>& out_heights);
[[nodiscard]] GeoJobResult download_tile_dtm_with_fallback(ElevationProvider provider, const std::string& google_key,
                                                           const TileWgs84Bounds& bounds, int dim,
                                                           std::vector<float>& out_heights);
[[nodiscard]] GeoJobResult import_heightmap_file(const std::filesystem::path& path, int dim,
                                                 std::vector<float>& out_heights);

} // namespace omsi::map
