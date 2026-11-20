#include "omsi/win32_shell_open.hpp"

#include "omsi/logger.hpp"

#include <shellapi.h>

#include <sstream>
#include <vector>

namespace omsi::win32 {
namespace {

std::wstring quote_path(const std::wstring& path) { return L"\"" + path + L"\""; }

bool shell_ok(const HINSTANCE result) { return reinterpret_cast<INT_PTR>(result) > 32; }

std::string shell_err_msg(const HINSTANCE result) {
    std::ostringstream oss;
    oss << "ShellExecute code=" << reinterpret_cast<intptr_t>(result);
    return oss.str();
}

bool try_shell_execute(HWND owner, LPCWSTR operation, LPCWSTR file, LPCWSTR params, std::string* err) {
    const HINSTANCE result = ShellExecuteW(owner, operation, file, params, nullptr, SW_SHOWNORMAL);
    if (shell_ok(result)) return true;
    if (err) *err = shell_err_msg(result);
    return false;
}

bool run_cmd_line(const std::wstring& cmdline, std::string* err) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                       &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    if (err) {
        std::ostringstream oss;
        oss << "CreateProcess err=" << GetLastError();
        *err = oss.str();
    }
    return false;
}

std::filesystem::path absolute_existing_file(const std::filesystem::path& file, std::string* err) {
    std::error_code ec;
    const auto abs = std::filesystem::absolute(file, ec);
    if (!std::filesystem::is_regular_file(abs, ec)) {
        if (err) *err = "file not found: " + abs.string();
        return {};
    }
    return abs;
}

std::filesystem::path absolute_existing_dir(const std::filesystem::path& dir, std::string* err) {
    std::error_code ec;
    const auto abs = std::filesystem::absolute(dir, ec);
    if (!std::filesystem::is_directory(abs, ec)) {
        if (err) *err = "folder not found: " + abs.string();
        return {};
    }
    return abs;
}

} // namespace

bool shell_open_notepad(const std::filesystem::path& file, HWND owner, std::string* err) {
    const auto abs = absolute_existing_file(file, err);
    if (abs.empty()) return false;
    const std::wstring wpath = abs.wstring();
    const std::wstring wparam = quote_path(wpath);
    if (try_shell_execute(owner, L"open", L"notepad.exe", wparam.c_str(), err)) {
        log::info("shell_open_notepad ShellExecute " + abs.string());
        return true;
    }
    const std::wstring cmdline = L"cmd.exe /c start \"\" notepad.exe " + wparam;
    if (run_cmd_line(cmdline, err)) {
        log::info("shell_open_notepad cmd " + abs.string());
        return true;
    }
    log::warn("shell_open_notepad failed " + abs.string() + (err ? (" " + *err) : ""));
    return false;
}

bool shell_open_folder(const std::filesystem::path& dir, HWND owner, std::string* err) {
    const auto abs = absolute_existing_dir(dir, err);
    if (abs.empty()) return false;
    const std::wstring wdir = abs.wstring();
    if (try_shell_execute(owner, L"open", wdir.c_str(), nullptr, err)) {
        log::info("shell_open_folder ShellExecute " + abs.string());
        return true;
    }
    const std::wstring cmdline = L"cmd.exe /c start \"\" explorer " + quote_path(wdir);
    if (run_cmd_line(cmdline, err)) {
        log::info("shell_open_folder cmd " + abs.string());
        return true;
    }
    log::warn("shell_open_folder failed " + abs.string() + (err ? (" " + *err) : ""));
    return false;
}

bool shell_open_folder_select_file(const std::filesystem::path& file, HWND owner, std::string* err) {
    const auto abs = absolute_existing_file(file, err);
    if (abs.empty()) return false;
    const std::wstring wpath = abs.wstring();
    const std::wstring wparam = L"/select," + quote_path(wpath);
    if (try_shell_execute(owner, nullptr, L"explorer.exe", wparam.c_str(), err)) {
        log::info("shell_open_folder_select_file ShellExecute " + abs.string());
        return true;
    }
    const std::wstring cmdline = L"cmd.exe /c start \"\" explorer " + wparam;
    if (run_cmd_line(cmdline, err)) {
        log::info("shell_open_folder_select_file cmd " + abs.string());
        return true;
    }
    const auto parent = abs.parent_path();
    if (!parent.empty() && shell_open_folder(parent, owner, err)) return true;
    log::warn("shell_open_folder_select_file failed " + abs.string() + (err ? (" " + *err) : ""));
    return false;
}

} // namespace omsi::win32
