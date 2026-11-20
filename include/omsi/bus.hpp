#pragma once

#include "omsi/bus_types.hpp"

#include <filesystem>
#include <optional>

namespace omsi::bus {

[[nodiscard]] std::optional<BusDocument> load_bus(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::path resolve_model_cfg_path(const BusDocument& bus);

} // namespace omsi::bus
