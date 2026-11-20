#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace omsi::app {

struct SplineCreatorCli {
    std::filesystem::path omsi_root;
    std::filesystem::path open_sli; // optional .sli to edit
    bool show_help = false;
    bool self_test = false;
};

// Parse argv (UTF-16 on Windows). Returns false on parse error.
[[nodiscard]] bool parse_spline_creator_cli(int argc, wchar_t* argv[], SplineCreatorCli& out);

[[nodiscard]] std::filesystem::path default_omsi_root();

// Standalone C++ dialog (legacy). Exit 0 = saved, 1 = cancelled/error.
[[nodiscard]] int run_spline_creator_app(const SplineCreatorCli& cli);

// Python app in tools/spline_creator (no rebuild to iterate UI). 0=ok/cancel, -1=no python.
struct SplineCreatorLaunchResult {
    int exit_code = 1;
    std::filesystem::path saved_path;
    bool started = false;
};
[[nodiscard]] SplineCreatorLaunchResult launch_python_spline_creator(const SplineCreatorCli& cli, void* owner_hwnd);

} // namespace omsi::app
