// OMSI `.osc` coverage test. Every script of the installed game is loaded through the real
// interpreter, grouped the way the engine groups them:
//  * each vehicle (`*.bus`) and each scenery object (`*.sco`) loads its own [script] list with
//    its own [varnamelist]/[stringvarnamelist] (bus.hpp / sco_types.hpp);
//  * scripts no bus or object references are loaded per folder (their own varlists).
// Each group runs init and a few frames. The report lists what the interpreter does not support.
//
// usage: osc_script_test [omsi_root] [path_filter]
// exit code 0 = nothing unsupported (unknown syntax, callbacks, system variables, macros).

#include "omsi/bus.hpp"
#include "omsi/bus_types.hpp"
#include "omsi/script/osc_vm.hpp"
#include "omsi/script/var_registry.hpp"
#include "omsi/sco_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// UTF-8 text of a path (path::string() would convert through the ANSI code page and throw
// on non-ASCII folder names such as the Vehicles' "捷徑.lnk").
std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool has_ext(const fs::path& p, const char* ext) { return lower(u8(p.extension())) == ext; }

bool is_varlist(const fs::path& p) {
    const std::string n = lower(u8(p.filename()));
    return lower(u8(p.extension())) == ".txt" && n.find("varlist") != std::string::npos;
}

struct Group {
    std::string label;           // for the report
    fs::path dir;                // script folder (relative files are resolved here)
    std::vector<fs::path> scripts;
    std::vector<fs::path> varlists;
    std::vector<fs::path> stringvarlists;
};

struct Issue {
    int count = 0;
    std::string example;
};

int run_test(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path("C:/Program Files (x86)/Steam/steamapps/common/OMSI 2");
    if (!fs::exists(root / "program")) {
        std::printf("osc_script_test: OMSI program folder not found under '%s'\n", u8(root).c_str());
        return 2;
    }
    const std::string filter = argc > 2 ? argv[2] : std::string{};

    omsi::script::load_known_lists(root);

    // Lists every object sees: engine + scenery varlists (same as the editor host).
    omsi::script::VarRegistry engine;
    (void)engine.load_engine_lists(root);
    (void)engine.load_scenery_lists(root);

    std::vector<Group> groups;
    std::vector<std::string> load_errors; // objects the engine loader itself rejected
    std::set<std::string> referenced; // lower-case full paths of scripts a bus/object loads

    auto add_ref = [&](const fs::path& dir, const fs::path& rel) {
        referenced.insert(lower(u8((dir / rel).lexically_normal())));
    };

    // Vehicles: every .bus.
    for (const char* sub : {"Vehicles"}) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root / sub, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec || !has_ext(it->path(), ".bus")) continue;
            if (!filter.empty() && u8(it->path()).find(filter) == std::string::npos) continue;
            std::optional<omsi::bus::BusDocument> bus;
            try {
                bus = omsi::bus::load_bus(it->path());
            } catch (const std::exception& ex) {
                load_errors.push_back(u8(it->path().lexically_relative(root)) + ": " + ex.what());
                continue;
            }
            if (!bus || bus->script_files.empty()) continue;
            Group g;
            g.label = u8(it->path().lexically_relative(root));
            g.dir = bus->bus_dir;
            g.scripts = bus->script_files;
            g.varlists = bus->varnamelist_files;
            g.stringvarlists = bus->stringvarnamelist_files;
            for (const auto& s : g.scripts) add_ref(g.dir, s);
            groups.push_back(std::move(g));
        }
    }
    // Scenery: every .sco.
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root / "Sceneryobjects", ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec || !has_ext(it->path(), ".sco")) continue;
            if (!filter.empty() && u8(it->path()).find(filter) == std::string::npos) continue;
            const omsi::sco::SceneryObject* sco = nullptr;
            try {
                sco = omsi::sco::load_sco(it->path());
            } catch (const std::exception& ex) {
                load_errors.push_back(u8(it->path().lexically_relative(root)) + ": " + ex.what());
                continue;
            }
            if (!sco || sco->script_files.empty()) continue;
            Group g;
            g.label = u8(it->path().lexically_relative(root));
            g.dir = it->path().parent_path();
            g.scripts = sco->script_files;
            g.varlists = sco->varnamelist_files;
            g.stringvarlists = sco->stringvarnamelist_files;
            for (const auto& s : g.scripts) add_ref(g.dir, s);
            groups.push_back(std::move(g));
        }
    }

    // Scripts nothing references: one group per folder, with the folder's own varlists.
    {
        std::map<fs::path, Group> orphan;
        std::set<fs::path> orphan_dirs; // folders that hold at least one unreferenced script
        std::error_code ec;
        for (const char* sub : {"Sceneryobjects", "Vehicles"}) {
            for (auto it = fs::recursive_directory_iterator(root / sub, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                std::error_code fec;
                if (!it->is_regular_file(fec) || fec || !has_ext(it->path(), ".osc")) continue;
                if (!filter.empty() && u8(it->path()).find(filter) == std::string::npos) continue;
                // A folder with unreferenced scripts loads all of its scripts together: the
                // macros an unreferenced script calls usually live in its referenced siblings.
                if (!referenced.count(lower(u8(it->path().lexically_normal())))) orphan_dirs.insert(it->path().parent_path());
                auto& g = orphan[it->path().parent_path()];
                if (g.label.empty()) {
                    g.label = "(unreferenced) " + u8(it->path().parent_path().lexically_relative(root));
                    g.dir = it->path().parent_path();
                    std::error_code dec;
                    for (auto e = fs::directory_iterator(g.dir, dec); !dec && e != fs::directory_iterator(); e.increment(dec)) {
                        std::error_code efc;
                        if (!e->is_regular_file(efc) || efc || !is_varlist(e->path())) continue;
                        const std::string n = lower(u8(e->path().filename()));
                        if (n.find("stringvarlist") != std::string::npos) g.stringvarlists.push_back(e->path().filename());
                        else g.varlists.push_back(e->path().filename());
                    }
                }
                g.scripts.push_back(it->path().filename());
            }
        }
        for (auto& [dir, g] : orphan)
            if (orphan_dirs.count(dir)) groups.push_back(std::move(g));
    }

    std::map<std::string, Issue> issues; // unsupported syntax / callbacks / sysvars / macros
    std::map<std::string, Issue> stubs;  // known callbacks with no host implementation
    int scripts = 0;
    int ok = 0;
    int failed = 0;
    int ir_total = 0;
    int ignored_total = 0;

    for (const auto& g : groups) {
        std::printf("[osc] %s (%zu scripts)\n", g.label.c_str(), g.scripts.size());
        std::fflush(stdout);
        scripts += static_cast<int>(g.scripts.size());
        try {
        omsi::script::VarRegistry reg = engine;
        (void)reg.merge_bus_lists(g.dir, g.varlists, g.stringvarlists);

        omsi::script::OscVM vm;
        if (!vm.load(g.dir, g.scripts, reg)) {
            ++failed;
            continue;
        }
        ++ok;
        ir_total += vm.stats().ir_ops;
        ignored_total += vm.stats().warnings;
        for (const auto& u : vm.unsupported()) {
            auto& slot = issues[u];
            if (slot.count++ == 0) slot.example = g.label;
        }
        for (const auto& s : vm.stubbed_callbacks()) {
            auto& slot = stubs[s];
            if (slot.count++ == 0) slot.example = g.label;
        }

        // Smoke run: init + a few frames with per-VM storage. Must not crash or hang.
        std::vector<float> floats(reg.float_var_count(), 0.f);
        std::vector<std::string> strings(reg.string_var_count());
        vm.bind_vars(&floats, &reg);
        vm.bind_strings(&strings);
        omsi::script::OscSystemState sys{};
        sys.get_time = 36000.f;
        sys.day = 1.f;
        sys.month = 1.f;
        sys.year = 2026.f;
        vm.set_system_state(sys);
        vm.run_init();
        for (int f = 0; f < 4; ++f) vm.run_frame(0.05f);
        } catch (const std::exception& ex) {
            load_errors.push_back(g.label + ": " + ex.what());
            ++failed;
        }
    }

    std::printf("\n=== OSC coverage: %d scripts in %zu groups (loaded=%d failed=%d), ir_ops=%d, ignored=%d ===\n",
                scripts, groups.size(), ok, failed, ir_total, ignored_total);
    std::printf("\nUnsupported (%zu distinct):\n", issues.size());
    for (const auto& [name, slot] : issues)
        std::printf("  %6d  %-48s  e.g. %s\n", slot.count, name.c_str(), slot.example.c_str());
    std::printf("\nKnown callbacks without host implementation (%zu distinct, push 0):\n", stubs.size());
    for (const auto& [name, slot] : stubs)
        std::printf("  %6d  %-48s  e.g. %s\n", slot.count, name.c_str(), slot.example.c_str());

    std::printf("\nLoader exceptions (%zu):\n", load_errors.size());
    for (const auto& e : load_errors) std::printf("  %s\n", e.c_str());

    const bool pass = issues.empty() && failed == 0;
    std::printf("\nRESULT: %s (unsupported=%zu, failed groups=%d)\n", pass ? "PASS" : "FAIL", issues.size(), failed);
    return pass ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run_test(argc, argv);
    } catch (const std::exception& ex) {
        std::printf("osc_script_test: exception: %s\n", ex.what());
        return 3;
    }
}
