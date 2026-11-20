#pragma once

#include "omsi/bus_catalog.hpp"

#include <filesystem>
#include <optional>

namespace omsi::ui {

// Win32 TreeView: Category → Model → variant/folder. Returns selected .bus path or nullopt.
[[nodiscard]] std::optional<std::filesystem::path> show_bus_picker(const std::filesystem::path& omsi_root);

} // namespace omsi::ui
