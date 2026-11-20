#pragma once

#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace omsi::ui {

// Modeless .map text editor — does not block the map editor main loop.
void open_map_text_editor(HWND owner, const std::filesystem::path& path, int goto_line = 1,
                          const std::filesystem::path* allowed_write_root = nullptr);
void pump_map_text_editor();
[[nodiscard]] bool map_text_editor_is_open();

} // namespace omsi::ui
