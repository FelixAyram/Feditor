#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

#include "omsi/spline_types.hpp"

namespace omsi::ui {

struct SplineCreatorResult {
    bool ok = false;
    std::filesystem::path saved_path;
    spline::SplineDefinition def{};
};

// Modal spline profile editor (2D cross-section). Returns true if user saved a .sli.
[[nodiscard]] bool run_spline_creator_dialog(HWND owner, const std::filesystem::path& omsi_root,
                                             std::optional<spline::SplineDefinition> initial,
                                             SplineCreatorResult& out);

// Headless Win32 self-test (A → marquee box). Same dialog code path as editor "Crear .sli...".
[[nodiscard]] bool run_spline_creator_self_test(const std::filesystem::path& omsi_root,
                                                 HWND owner = nullptr);

} // namespace omsi::ui
