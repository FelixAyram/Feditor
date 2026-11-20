// Tile stream load: begin_load must not parse the whole map; AsyncWorldLoader
// parses only the requested tiles in parallel.

#include "omsi/map_attachment.hpp"
#include "omsi/map_types.hpp"
#include "omsi/world.hpp"
#include "omsi/world_load_job.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

int fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << '\n';
    return 1;
}

double ms_since(const clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

omsi::world::LoadOptions make_opts(const fs::path& omsi_root) {
    omsi::world::LoadOptions opts{};
    opts.omsi_root = omsi_root;
    opts.tile_radius = 4;
    opts.max_object_dist = 1.0e7f;
    opts.load_splines = true;
    return opts;
}

int wait_job(omsi::world::AsyncWorldLoader& job, const double timeout_ms) {
    const auto t0 = clock::now();
    while (!job.finished()) {
        if (ms_since(t0) > timeout_ms) return fail("AsyncWorldLoader timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 0;
}

int run_synthetic(const fs::path& fixture) {
    if (!fs::exists(fixture)) return fail("missing fixture " + fixture.string());

    const auto tmp = fs::temp_directory_path() / "omsi_f4_tile_stream_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    omsi::map::GlobalMap global{};
    global.friendly_name = "tile_stream_synthetic";
    global.root = tmp;
    global.tiles.reserve(84);
    for (int i = 0; i < 80; ++i) {
        global.tiles.push_back({1000 + i, 2000, tmp / ("missing_" + std::to_string(i) + ".map")});
    }

    std::vector<omsi::map::MapTileRef> only;
    for (int i = 0; i < 4; ++i) {
        const auto path = tmp / ("tile_" + std::to_string(i) + "_" + std::to_string(i) + ".map");
        fs::copy_file(fixture, path, fs::copy_options::overwrite_existing);
        omsi::map::MapTileRef ref{i, i, path};
        global.tiles.push_back(ref);
        only.push_back(ref);
    }

    omsi::map::EntryPoint entry{};
    const auto opts = make_opts({});

    const auto t_begin = clock::now();
    omsi::world::World probe{};
    probe.begin_load(global, entry, opts);
    const double begin_ms = ms_since(t_begin);
    std::cout << "begin_load (80 missing + 4 real refs) " << begin_ms << " ms\n";
    if (begin_ms > 400.0) return fail("begin_load too slow — still scanning global.tiles?");

    const auto t_job = clock::now();
    omsi::world::AsyncWorldLoader job{};
    job.start(global, entry, opts, only);
    if (const int rc = wait_job(job, 15000.0)) return rc;
    const auto progress = job.poll();
    auto world = job.take_world();
    const double job_ms = ms_since(t_job);
    std::cout << "async 4 tiles " << job_ms << " ms status=[" << progress.status << "] tiles="
              << world.tiles().size() << " meshes=" << world.meshes().size() << '\n';
    if (progress.failed) return fail("async job failed: " + progress.status);
    if (world.tiles().size() != 4) return fail("expected 4 loaded tiles, got " + std::to_string(world.tiles().size()));

    const auto t_job2 = clock::now();
    omsi::world::AsyncWorldLoader job2{};
    job2.start(global, entry, opts, only);
    if (const int rc = wait_job(job2, 15000.0)) return rc;
    auto world2 = job2.take_world();
    const double job2_ms = ms_since(t_job2);
    std::cout << "async 4 tiles (cached parse) " << job2_ms << " ms tiles=" << world2.tiles().size() << '\n';
    if (world2.tiles().size() != 4) return fail("cached reload tile count");

    omsi::map::invalidate_map_tile_cache();
    fs::remove_all(tmp, ec);
    std::cout << "OK synthetic\n";
    return 0;
}

int run_named_map(const fs::path& omsi_root, const fs::path& global_cfg, const std::size_t want_tiles) {
    if (!fs::exists(global_cfg)) {
        std::cout << "SKIP real map (no " << global_cfg.string() << ")\n";
        return 0;
    }
    const auto global = omsi::map::load_global_map(global_cfg);
    if (!global || global->tiles.size() < 2)
        return fail(global_cfg.string() + " unreadable");

    std::vector<omsi::map::MapTileRef> only;
    for (const auto& ref : global->tiles) {
        if (!fs::exists(ref.tile_path)) continue;
        only.push_back(ref);
        if (only.size() >= want_tiles) break;
    }
    if (only.size() < 2) return fail(global_cfg.filename().string() + " has no readable tiles");
    std::cout << "MAP " << global->friendly_name << " global_tiles=" << global->tiles.size()
              << " stream=" << only.size() << '\n';

    omsi::map::EntryPoint entry = global->entrypoints.empty() ? omsi::map::default_spawn_entrypoint()
                                                              : global->entrypoints.front();
    const auto opts = make_opts(omsi_root);

    const auto t_begin = clock::now();
    {
        omsi::world::World probe{};
        probe.begin_load(*global, entry, opts);
    }
    std::cout << "begin_load " << ms_since(t_begin) << " ms (must stay cheap)\n";

    double old_ms = 0.0;
    if (global->tiles.size() <= 80) {
        const auto t_old = clock::now();
        const auto full_index = omsi::map::build_global_spline_index(*global, omsi_root);
        old_ms = ms_since(t_old);
        std::cout << "OLD full-map spline index " << old_ms << " ms entries=" << full_index.size() << '\n';
    } else {
        std::cout << "SKIP OLD full-map index (" << global->tiles.size() << " tiles — that was the 0% stall)\n";
    }

    omsi::map::invalidate_map_tile_cache();
    const auto t1 = clock::now();
    omsi::world::AsyncWorldLoader job{};
    job.start(*global, entry, opts, only);
    if (const int rc = wait_job(job, 120000.0)) return rc;
    const auto progress = job.poll();
    auto world = job.take_world();
    const double new_ms = ms_since(t1);
    std::cout << "NEW async " << only.size() << " tiles " << new_ms << " ms tiles=" << world.tiles().size()
              << " meshes=" << world.meshes().size() << " status=[" << progress.status << "]\n";
    if (progress.failed) return fail("real async failed: " + progress.status);
    if (world.tiles().size() != only.size())
        return fail("real tile count " + std::to_string(world.tiles().size()));

    const auto t2 = clock::now();
    omsi::world::AsyncWorldLoader job2{};
    job2.start(*global, entry, opts, only);
    if (const int rc = wait_job(job2, 120000.0)) return rc;
    auto world2 = job2.take_world();
    const double cache_ms = ms_since(t2);
    std::cout << "NEW async cached " << cache_ms << " ms tiles=" << world2.tiles().size()
              << " meshes=" << world2.meshes().size() << '\n';
    if (world2.tiles().size() != world.tiles().size()) return fail("cached real tile count mismatch");
    if (world2.meshes().size() != world.meshes().size()) return fail("cached real mesh count mismatch");

    std::cout << "OK " << global_cfg.parent_path().filename().string() << '\n';
    (void)old_ms;
    return 0;
}

int run_real_maps(const fs::path& omsi_root) {
    if (const int rc = run_named_map(omsi_root, omsi_root / "maps" / "Test_Lat30" / "global.cfg", 4)) return rc;
    auto pimlico = omsi_root / "maps" / "Pimlico_actual" / "global.cfg";
    if (!fs::exists(pimlico)) pimlico = omsi_root / "maps" / "Pimlico" / "global.cfg";
    return run_named_map(omsi_root, pimlico, 8);
}

} // namespace

int main(int argc, char** argv) {
    fs::path fixture = "_test_data/spline_attach_tile.map";
    fs::path omsi_root = "C:/Program Files (x86)/Steam/steamapps/common/OMSI 2";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--omsi-root" && i + 1 < argc)
            omsi_root = argv[++i];
        else
            fixture = arg;
    }

    std::cout << "cores=" << std::max(1u, std::thread::hardware_concurrency()) << '\n';
    if (const int rc = run_synthetic(fixture)) return rc;
    return run_real_maps(omsi_root);
}
