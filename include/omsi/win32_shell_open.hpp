#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>

namespace omsi::win32 {

[[nodiscard]] bool shell_open_notepad(const std::filesystem::path& file, HWND owner, std::string* err = nullptr);

[[nodiscard]] bool shell_open_folder(const std::filesystem::path& dir, HWND owner, std::string* err = nullptr);

// Opens Explorer with the file selected (explorer.exe /select,"path").
[[nodiscard]] bool shell_open_folder_select_file(const std::filesystem::path& file, HWND owner,
                                                 std::string* err = nullptr);

} // namespace omsi::win32
