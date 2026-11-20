#pragma once
// A BeamNG editing session: mounted VFS + loaded level + launch choices. Created by the launcher / main,
// handed to the map editor through MapEditorConfig::beamng.
#include "beamng/bng_level.hpp"
#include "beamng/bng_vfs.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bng {

struct Prefs {
    std::string game = "omsi";          // "omsi" | "beamng" — what the launcher opens first
    std::filesystem::path install;
    std::filesystem::path user_folder;
    std::string last_level;
    std::string last_spawn;
    float load_radius = 1000.f;
    // editor quality (BeamNG preset levels): mesh 0..4 (Lowest..Ultra), texture 0..3, shadows 0..3
    int q_mesh = 2, q_texture = 2, q_shadows = 2;
    // folders used before (most recent first), shown in the launcher's drop-down lists
    std::vector<std::filesystem::path> recent_installs, recent_users;
};
// put `p` first in `list` (no duplicates, at most 12)
void remember_path(std::vector<std::filesystem::path>& list, const std::filesystem::path& p);
Prefs load_prefs();          // omsi_f4_beamng_prefs.cfg next to the exe
void save_prefs(const Prefs& p);

struct Session {
    std::filesystem::path install;
    std::filesystem::path user_folder;
    std::shared_ptr<Vfs> vfs;
    LevelInfo info;
    std::shared_ptr<Level> level;
    std::string spawn_name;
    float load_radius = 1000.f;
    bool view_only = false;
    float spawn_pos[3]{0.f, 0.f, 0.f};
    float spawn_heading = 0.f;   // radians, rotation about Z
    std::filesystem::path workspace; // scratch folder for editor-only files (ui prefs, shim tile)
};

// Global hook for the renderer's "vfs:" texture provider (one session at a time).
void set_active_vfs(std::shared_ptr<Vfs> vfs);
const Vfs* active_vfs();
std::shared_ptr<Vfs> active_vfs_shared();

} // namespace bng
