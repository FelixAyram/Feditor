#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "omsi/editor_map_workspace.hpp"
#include "omsi/map_types.hpp"
#include "omsi/options.hpp"
#include "omsi/world.hpp"

namespace bng {
struct Session;
}

namespace omsi::app {

// When the editor returns 101, main reloads a 9x9 around this focus (streaming).
struct TileStreamRequest {
    bool active = false;
    map::EntryPoint focus{};
    float keep_radius = 50.f;
    float keep_theta = 0.f;
    float keep_phi = 0.f;
};

struct MapEditorConfig {
    std::filesystem::path omsi_root;
    map::GlobalMap global;
    std::size_t entry_index = 0;
    options::OmsiOptions game_opts{};
    int max_frames = -1;
    std::filesystem::path screenshot_path;
    bool verify_mode = false; // auto teleport + pan + multi-shot
    std::filesystem::path screenshot_entry1_path;
    // If set and user picks another global.cfg, path is written and editor returns 100.
    std::filesystem::path* switch_map_out = nullptr;
    // Optional: launcher may change OMSI root / entrypoint when reloading a map.
    std::filesystem::path* switch_omsi_out = nullptr;
    std::size_t* switch_entry_out = nullptr;
    bool* switch_view_only_out = nullptr;
    TileStreamRequest* tile_stream = nullptr;
    // After tile stream reload: restore orbit (optional).
    bool restore_orbit = false;
    float restore_radius = 50.f;
    float restore_theta = 0.f;
    float restore_phi = 0.f;
    // Full edit copy (maps/Foo_actual) via robocopy; Foo_original is frozen. Live map never written.
    std::optional<map::EditorMapWorkspace> workspace{};
    // Entry / stream focus for the initial async tile load inside the editor.
    map::EntryPoint load_entry{};
    int stream_stress_hops = 0; // CLI --stream-stress: random tile hops for drift testing
    bool sa_entry_hop_test = false; // CLI --sa-entry-hop-test: teleport EP0→EP1→EP0 + audit watch ids
    std::int32_t sa_watch_id = 14355; // --sa-watch-id
    bool test_spline_creator = false; // CLI --test-spline-creator: auto "Crear .sli..." + self-test
    bool spline_creator_test_ok = true;
    // Optional boot progress (Win32 LoadingScreen from main after launcher Iniciar).
    std::function<void(float fraction, const std::string& status_utf8)> boot_progress{};
    // Read-only viewer: no workspace robocopy, no place/move/delete/save/undo mutations.
    // Selection, camera, rails, catalog browse, "Ver en .map" (read) still work.
    bool view_only = false;
    // Set true after a successful Ctrl+S this session (keep *_actual on exit).
    bool* ctrl_s_saved_out = nullptr;
    bool* had_map_edits_out = nullptr;
    // BeamNG.drive session (null = OMSI map). The global/tiles above are then a hidden one-tile shim.
    std::shared_ptr<bng::Session> beamng;
};

[[nodiscard]] world::LoadOptions make_editor_load_opts(const options::OmsiOptions& opts,
                                                       const std::filesystem::path& omsi_root,
                                                       std::int32_t tile_radius = 1);

int run_map_editor_app(MapEditorConfig cfg, world::World world);

// TT / station-link click modes (defined in map_editor_app.cpp).
extern int g_omsi_tt_mode; // 0=Add Trackentry 1=Add Station 2=Show TT
extern int g_omsi_sl_mode; // 0=SetStart 1=SetEnd 2=AddPathSeg

} // namespace omsi::app
