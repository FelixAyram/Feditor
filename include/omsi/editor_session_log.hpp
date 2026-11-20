#pragma once

#include <filesystem>
#include <string_view>

namespace omsi::editor_log {

// Dedicated editor interaction log (shortcuts, clicks, undo/redo). Separate from omsi_f4_engine.log.
void init(const std::filesystem::path& path = {});
void shutdown();

void event(std::string_view category, std::string_view detail);

[[nodiscard]] std::filesystem::path session_path();

} // namespace omsi::editor_log
