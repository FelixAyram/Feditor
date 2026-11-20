#pragma once
#include <cstddef>
#include <filesystem>
#include <string>

namespace omsi::app {

struct BeamngFlowArgs {
    std::filesystem::path omsi_root; // may be empty (BeamNG does not need OMSI)
    int max_frames = -1;
    std::filesystem::path screenshot_path;
    // CLI: --beamng <level> [--beamng-spawn name] [--beamng-radius m] skips the launcher
    std::string cli_level, cli_spawn;
    float cli_radius = 0.f;
};

// Outcome of one launcher + editor pass (the launchers can be re-entered any number of times).
struct BeamngFlowResult {
    int rc = 0;                  // exit code of the BeamNG editor when editor_ran, else 0
    bool editor_ran = false;     // a level was confirmed and the editor was opened
    bool switch_to_omsi = false; // launcher: user pressed "OMSI 2"
    // The editor's "Cargar mapa" (rc 100) inside BeamNG mode: main reopens this OMSI map instead of quitting.
    std::filesystem::path switch_map;  // global.cfg
    std::filesystem::path switch_omsi; // OMSI root chosen in that launcher
    std::size_t switch_entry = 0;
    bool switch_view_only = false;
};

// Shows the BeamNG launcher and runs the editor on the chosen level.
int run_beamng_flow(const BeamngFlowArgs& args, BeamngFlowResult& out);

} // namespace omsi::app
