#pragma once

// Map packager ("Empaquetar mapa"): walks a map's dependencies (tile .map files -> .sco / .sli ->
// meshes, textures, scripts, sounds; ailists -> AI .bus / .ovh; [standarddepot]) and writes them to a
// .zip with the chosen compression. C++ port of the map_dependency_scanner / map_dependency_pack tools.
//
// Steam content is left out: the installed depots of OMSI 2 and its DLCs are read from the local
// Steam depot manifests (Steam\depotcache\<depot>_<manifest>.manifest, the same lists SteamDB shows),
// and every file they list is skipped, so only user / third-party content is packed.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace omsi::pack {

struct PackOptions {
    std::filesystem::path omsi_root;
    std::filesystem::path global_cfg; // the map's global.cfg (live map folder)
    std::filesystem::path out_zip;
    bool map_files = true;  // everything inside maps\<map>\ (tiles, terrain, cfgs, textures of the map)
    bool objects = true;    // .sco used by the tiles (+ meshes / textures / scripts / sounds)
    bool splines = true;    // .sli used by the tiles (+ textures)
    bool ai_vehicles = true; // ailists*.cfg -> .bus / .ovh (+ humans.txt / drivers.txt)
    bool depot_vehicles = true; // [standarddepot]
    int compression = 6;    // 0 store .. 9 best
    bool exclude_steam = true; // skip files listed in the Steam depot manifests of OMSI 2 + DLCs
    // folder with map_dependency_scanner.py + f4_pack_scan.py: the same scan as the Python tool
    // (needs Python 3). Empty / no Python -> built-in C++ scan.
    std::filesystem::path scanner_dir;
};

struct PackProgress {
    std::atomic<float> frac{0.f};
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    std::atomic<bool> cancel{false};
    std::mutex mu;
    std::string stage;   // guarded by mu
    std::string current; // guarded by mu
    std::string result;  // guarded by mu: summary / error text
    int added = 0, excluded_steam = 0, missing = 0, total = 0;
    std::uint64_t bytes_in = 0;
    bool steam_data = false; // depot manifests were found
};

// Blocking: run it on a worker thread and poll `progress`.
void run_pack(const PackOptions& opt, PackProgress& progress);

// Default output path: <omsi_root>\F4_Packs\<map folder>_<yyyymmdd_hhmm>.zip
[[nodiscard]] std::filesystem::path default_pack_path(const std::filesystem::path& omsi_root,
                                                      const std::filesystem::path& global_cfg);

// Lower-case, backslash relative paths of every file in the installed Steam depots of OMSI 2
// (app 252530) and its DLCs, with their sizes. Empty when no manifest could be read.
struct SteamFileList {
    std::vector<std::pair<std::string, std::uint64_t>> files; // sorted by path
    int depots_read = 0, depots_missing = 0, depots_encrypted = 0;
    [[nodiscard]] const std::uint64_t* find(const std::string& rel_lower) const;
};
[[nodiscard]] SteamFileList load_steam_file_list(const std::filesystem::path& omsi_root);

// Every file one .sco / .sli needs to be drawn (itself, meshes, model cfgs, textures and their
// variants), absolute paths below omsi_root. Sounds are left out. Used by the co-op host.
[[nodiscard]] std::vector<std::filesystem::path> asset_closure(const std::filesystem::path& omsi_root,
                                                               const std::filesystem::path& asset_file);

} // namespace omsi::pack
