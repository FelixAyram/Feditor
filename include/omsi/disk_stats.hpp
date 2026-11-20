#pragma once
// Disk read timing (textures, .o3d maps, VFS reads): how many reads, MB, ms per read, the slowest file,
// and how much of it ran on the main thread (a read there is a frame stutter). One summary line every
// few seconds via disk_stats::maybe_log (DISK ... in logfile.log, next to MAPPERF_SPIKE).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace omsi::disk_stats {

enum Kind : int { Texture = 0, O3d = 1, Vfs = 2, KindCount = 3 };

struct Counters {
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> us{0};
    std::atomic<std::uint64_t> main_us{0};
    std::atomic<std::uint64_t> main_reads{0};
    std::atomic<std::uint64_t> slow{0}; // reads over 50 ms
};

inline Counters g_kind[KindCount];
inline std::atomic<std::uint64_t> g_max_us{0};
inline std::mutex g_slowest_mu;
inline std::string g_slowest;
inline std::thread::id g_main_thread{};

inline void set_main_thread() { g_main_thread = std::this_thread::get_id(); }

// Reads from the worker pool at once: an HDD reading many files at a time seeks between them all (streaming
// read o3d at 0.1 MB/s, 126 ms a file, one texture 805 ms). OMSI_IO_SLOTS overrides (an SSD can take more).
// The main thread never waits for a slot.
struct IoSlots {
    std::mutex mu;
    std::condition_variable cv;
    int free = -1;
};
inline IoSlots g_io;
inline void io_acquire() {
    if (std::this_thread::get_id() == g_main_thread) return;
    std::unique_lock<std::mutex> lk(g_io.mu);
    if (g_io.free < 0) {
        const char* e = std::getenv("OMSI_IO_SLOTS");
        g_io.free = (e && *e) ? (std::max)(1, std::atoi(e)) : 3;
    }
    g_io.cv.wait(lk, [] { return g_io.free > 0; });
    --g_io.free;
}
inline void io_release() {
    if (std::this_thread::get_id() == g_main_thread) return;
    {
        std::lock_guard<std::mutex> lk(g_io.mu);
        ++g_io.free;
    }
    g_io.cv.notify_one();
}

inline void note(Kind k, std::uint64_t bytes, double ms, const std::filesystem::path& path) {
    auto& c = g_kind[k];
    const auto us = static_cast<std::uint64_t>((std::max)(0.0, ms) * 1000.0);
    c.reads.fetch_add(1, std::memory_order_relaxed);
    c.bytes.fetch_add(bytes, std::memory_order_relaxed);
    c.us.fetch_add(us, std::memory_order_relaxed);
    if (std::this_thread::get_id() == g_main_thread) {
        c.main_us.fetch_add(us, std::memory_order_relaxed);
        c.main_reads.fetch_add(1, std::memory_order_relaxed);
    }
    if (ms > 50.0) c.slow.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t prev = g_max_us.load(std::memory_order_relaxed);
    while (us > prev && !g_max_us.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
    }
    if (us >= prev && us > 0) {
        std::lock_guard<std::mutex> lk(g_slowest_mu);
        char b[32];
        std::snprintf(b, sizeof(b), " %.1f ms", ms);
        g_slowest = path.filename().string() + b;
    }
}

// RAII timer around one read
struct Timer {
    Kind kind;
    const std::filesystem::path& path;
    std::uint64_t bytes = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    Timer(Kind k, const std::filesystem::path& p) : kind(k), path(p) {
        io_acquire();
        t0 = std::chrono::steady_clock::now(); // (the read itself, not the wait for a slot)
    }
    ~Timer() {
        io_release();
        note(kind, bytes, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), path);
    }
};

// One line every `every_s` seconds with what was read since the last line; empty when nothing to say.
inline std::string maybe_log(double every_s = 5.0) {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    if (dt < every_s) return {};
    last = now;
    static const char* kName[KindCount] = {"tex", "o3d", "vfs"};
    std::string line = "DISK window_s=" + std::to_string(static_cast<int>(dt + 0.5));
    std::uint64_t total_reads = 0;
    for (int k = 0; k < KindCount; ++k) {
        auto& c = g_kind[k];
        const auto n = c.reads.exchange(0), by = c.bytes.exchange(0), us = c.us.exchange(0);
        const auto mus = c.main_us.exchange(0), mn = c.main_reads.exchange(0), sl = c.slow.exchange(0);
        total_reads += n;
        char b[256];
        std::snprintf(b, sizeof(b), " %s:n=%llu,MB=%.1f,ms=%.0f,avg=%.2f,MBps=%.1f,main_n=%llu,main_ms=%.0f,slow50=%llu",
                      kName[k], static_cast<unsigned long long>(n), by / 1048576.0, us / 1000.0,
                      n ? us / 1000.0 / static_cast<double>(n) : 0.0, us ? (by / 1048576.0) / (us / 1.e6) : 0.0,
                      static_cast<unsigned long long>(mn), mus / 1000.0, static_cast<unsigned long long>(sl));
        line += b;
    }
    const auto mx = g_max_us.exchange(0);
    std::string slowest;
    {
        std::lock_guard<std::mutex> lk(g_slowest_mu);
        slowest.swap(g_slowest);
    }
    if (total_reads == 0) return {};
    char b[64];
    std::snprintf(b, sizeof(b), " max_ms=%.1f", mx / 1000.0);
    return line + b + " slowest=" + slowest;
}

} // namespace omsi::disk_stats
