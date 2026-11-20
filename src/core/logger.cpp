#include "omsi/logger.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace omsi::log {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::filesystem::path g_path;
std::chrono::steady_clock::time_point g_last_flush{};
std::uint64_t g_console_suppressed = 0;
bool g_dirty = false;

struct RateKey {
    std::chrono::steady_clock::time_point last{};
    std::uint32_t suppressed = 0;
};
std::unordered_map<std::string, RateKey> g_rate;

constexpr auto kFlushInterval = std::chrono::seconds(1);

bool msg_needs_immediate_flush(std::string_view msg) {
    auto has = [&](const char* s) { return msg.find(s) != std::string_view::npos; };
    return has("PERF") || has("frustum") || has("POPULATE") || has("VP_RAW") || has("hwcal") ||
           has("CAM_HITCH");
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
        << ms.count();
    return oss.str();
}

bool console_allow(std::string_view msg) {
    std::string key(msg.substr(0, (std::min)(msg.size(), std::size_t{48})));
    const auto now = std::chrono::steady_clock::now();
    auto& slot = g_rate[key];
    if (slot.last.time_since_epoch().count() != 0 && now - slot.last < std::chrono::milliseconds(500)) {
        ++slot.suppressed;
        ++g_console_suppressed;
        return false;
    }
    if (slot.suppressed > 0) {
        std::cerr << "[...] (console suppressed " << slot.suppressed << " similar lines)\n";
        slot.suppressed = 0;
    }
    slot.last = now;
    return true;
}

void flush_file_unlocked(bool force) {
    if (!g_file.is_open() || !g_dirty) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && g_last_flush.time_since_epoch().count() != 0 && now - g_last_flush < kFlushInterval)
        return;
    g_file.flush();
    g_last_flush = now;
    g_dirty = false;
}

// Periodic FPS / performance telemetry: written to the .log only, never to the console.
bool file_only(std::string_view msg) {
    static constexpr std::string_view kPrefixes[] = {
        "FPSDET",       "PERF60",     "PERF ",         "MEM60",       "HOTSPOTS",
        "MAPPERF",      "draw stats", "STREAM_STATE",  "frustum GH self-test",
    };
    for (const auto p : kPrefixes)
        if (msg.substr(0, p.size()) == p) return true;
    return false;
}

void write_line(const char* level, std::string_view msg) {
    const std::string line = "[" + timestamp() + "] [" + level + "] " + std::string(msg);
    std::lock_guard lock(g_mutex);

    if (!file_only(msg) && console_allow(msg))
        std::cerr << line << '\n';

    if (g_file.is_open()) {
        g_file << line << '\n';
        g_dirty = true;
        flush_file_unlocked(level[0] == 'E' || level[0] == 'W' || msg_needs_immediate_flush(msg));
    }
}

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path local_appdata_log_dir() {
    wchar_t* appdata = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata)) && appdata) {
        out = std::filesystem::path(appdata) / L"OMSI_F4";
        CoTaskMemFree(appdata);
    }
    return out;
}

// Try several writable locations. Returns first path that opens for append.
bool try_open(const std::filesystem::path& path) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);

    // Rotate oversized logs.
    if (std::filesystem::exists(path, ec)) {
        const auto sz = std::filesystem::file_size(path, ec);
        if (!ec && sz > 32ull * 1024ull * 1024ull) {
            std::filesystem::rename(path, path.string() + ".old", ec);
        }
    }

    g_file.close();
    g_file.clear();
#if defined(_MSC_VER)
    g_file.open(path.wstring(), std::ios::out | std::ios::app);
#else
    g_file.open(path, std::ios::out | std::ios::app);
#endif
    if (!g_file.is_open()) return false;
    g_path = path;
    return true;
}

} // namespace

void init(const std::filesystem::path& log_path) {
    std::lock_guard lock(g_mutex);
    g_last_flush = {};
    g_dirty = false;
    g_file.close();
    g_file.clear();

    std::vector<std::filesystem::path> candidates;
    if (!log_path.empty()) {
        candidates.push_back(log_path);
    }
    // 1) Next to the .exe (what users usually expect when they look in the build folder)
    candidates.push_back(exe_dir() / L"logfile.log");
    // 2) Also classic name next to exe
    candidates.push_back(exe_dir() / L"omsi_f4_engine.log");
    // 3) Current working directory (often the project / launch dir)
    {
        std::error_code ec;
        const auto cwd = std::filesystem::current_path(ec);
        if (!ec) {
            candidates.push_back(cwd / L"logfile.log");
            candidates.push_back(cwd / L"omsi_f4_engine.log");
        }
    }
    // 4) %LOCALAPPDATA%\OMSI_F4\logfile.log (always writable for the user)
    {
        const auto dir = local_appdata_log_dir();
        if (!dir.empty())
            candidates.push_back(dir / L"logfile.log");
    }

    bool opened = false;
    for (const auto& c : candidates) {
        if (try_open(c)) {
            opened = true;
            break;
        }
    }

    if (opened) {
        g_file << "\n=== session start " << timestamp() << " ===\n";
        g_file << "log_file=" << g_path.string() << "\n";
        g_file.flush();
        g_last_flush = std::chrono::steady_clock::now();
        g_dirty = false;
        // Always print where the file is (not rate-limited).
        std::cerr << "[" << timestamp() << "] [INFO] log file OPEN: " << g_path.string() << '\n';
    } else {
        g_path.clear();
        std::cerr << "[" << timestamp()
                  << "] [ERROR] log file FAILED to open (tried exe dir, cwd, LocalAppData)\n";
    }
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        if (g_console_suppressed > 0)
            g_file << "(session console suppressed total=" << g_console_suppressed << ")\n";
        g_file << "=== session end " << timestamp() << " ===\n";
        g_file.flush();
        g_file.close();
        g_dirty = false;
    }
}

void trace(std::string_view msg) { write_line("TRACE", msg); }
void info(std::string_view msg) { write_line("INFO", msg); }
void warn(std::string_view msg) { write_line("WARN", msg); }
void error(std::string_view msg) { write_line("ERROR", msg); }

void flush() {
    std::lock_guard lock(g_mutex);
    flush_file_unlocked(true);
}

std::filesystem::path log_path() { return g_path; }

} // namespace omsi::log
