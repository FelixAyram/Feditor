#include "omsi/editor_session_log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace omsi::editor_log {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::filesystem::path g_path;

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::filesystem::path default_session_path() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto p = std::filesystem::path(buf);
    p.replace_filename(L"omsi_f4_editor_session.log");
    return p;
}

} // namespace

void init(const std::filesystem::path& path) {
    std::lock_guard lock(g_mutex);
    g_path = path.empty() ? default_session_path() : path;
    g_file.open(g_path, std::ios::out | std::ios::app);
    if (g_file.is_open()) {
        g_file << "\n=== editor session " << timestamp() << " ===\n";
        g_file << "log_file=" << g_path.string() << "\n";
        g_file.flush();
    }
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file << "=== editor session end " << timestamp() << " ===\n";
        g_file.flush();
        g_file.close();
    }
}

void event(std::string_view category, std::string_view detail) {
    const std::string line = "[" + timestamp() + "] [" + std::string(category) + "] " + std::string(detail);
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file << line << '\n';
        g_file.flush();
    }
}

std::filesystem::path session_path() { return g_path; }

} // namespace omsi::editor_log
