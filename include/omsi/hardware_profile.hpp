#pragma once

#include "omsi/logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace omsi {

/// Measured memcpy + disk open/read. One-shot at editor/viewer boot.
struct HardwareProfile {
    std::size_t memcpy_bps = 5'000'000'000ull;
    double disk_open_ms = 5.0;
    double disk_read_mbps = 100.0;
    double io_weight = 4.0;

    static HardwareProfile& global() {
        static HardwareProfile p;
        return p;
    }

    void calibrate(const std::filesystem::path& probe_dir) {
        measure_memcpy();
        measure_disk(probe_dir);
        io_weight = disk_open_ms > 5.0 ? 20.0 : 4.0;
        std::ostringstream o;
        o << "hwcal memcpy_GBs=" << (static_cast<double>(memcpy_bps) / 1.0e9) << " open_ms=" << disk_open_ms
          << " read_MBps=" << disk_read_mbps << " io_weight=" << io_weight << " entry=" << (entry() ? 1 : 0)
          << " load_r=" << load_radius_m() << " unload_r=" << unload_radius_m();
        log::info(o.str());
    }

    [[nodiscard]] bool entry() const {
        return memcpy_bps < 8'000'000'000ull || disk_open_ms > 8.0;
    }
    [[nodiscard]] float load_radius_m() const { return entry() ? 150.f : 300.f; }
    // Hysteresis: keep tiles a bit beyond load radius to avoid thrash (plan: 1.35×).
    [[nodiscard]] float unload_radius_m() const { return load_radius_m() * 1.35f; }

private:
    void measure_memcpy() {
        std::size_t sz = 64ull * 1024ull * 1024ull;
        auto* src = static_cast<std::byte*>(std::malloc(sz));
        auto* dst = static_cast<std::byte*>(std::malloc(sz));
        if (!src || !dst) {
            std::free(src);
            std::free(dst);
            sz = 8ull * 1024ull * 1024ull;
            src = static_cast<std::byte*>(std::malloc(sz));
            dst = static_cast<std::byte*>(std::malloc(sz));
        }
        if (!src || !dst) {
            std::free(src);
            std::free(dst);
            return;
        }
        std::memset(src, 0xA5, sz);
        std::memcpy(dst, src, sz);
        constexpr int kIters = 4;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i) std::memcpy(dst, src, sz);
        const auto t1 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        if (sec > 1.e-6) {
            const double bytes = static_cast<double>(sz) * static_cast<double>(kIters) * 2.0;
            memcpy_bps = static_cast<std::size_t>((std::max)(1.0e8, bytes / sec));
        }
        std::free(src);
        std::free(dst);
    }

    void measure_disk(const std::filesystem::path& probe_dir) {
        std::error_code ec;
        std::filesystem::path probe;
        bool created = false;
        if (!probe_dir.empty()) {
            for (const auto& e : std::filesystem::directory_iterator(probe_dir, ec)) {
                if (ec) break;
                if (!e.is_regular_file(ec)) continue;
                const auto n = e.file_size(ec);
                if (!ec && n >= 4096ull) {
                    probe = e.path();
                    break;
                }
            }
        }
        if (probe.empty()) {
#ifdef _WIN32
            wchar_t tmp[MAX_PATH]{};
            const DWORD n = GetTempPathW(MAX_PATH, tmp);
            std::filesystem::path td = n ? std::filesystem::path(tmp) : std::filesystem::temp_directory_path(ec);
#else
            std::filesystem::path td = std::filesystem::temp_directory_path(ec);
#endif
            probe = td / L"_omsi_f4_hwcal.bin";
            std::ofstream f(probe, std::ios::binary | std::ios::trunc);
            std::vector<char> buf(1024 * 1024, 0);
            f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            created = true;
        }
        if (probe.empty()) return;

        constexpr int kN = 16;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kN; ++i) {
            std::ifstream f(probe, std::ios::binary);
            std::array<char, 4096> block{};
            f.read(block.data(), static_cast<std::streamsize>(block.size()));
        }
        const auto t1 = std::chrono::steady_clock::now();
        disk_open_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(kN);

        const auto seq_n = (std::min)(std::filesystem::file_size(probe, ec), 1024ull * 1024ull);
        if (!ec && seq_n > 0) {
            std::vector<char> buf(static_cast<std::size_t>(seq_n));
            const auto t2 = std::chrono::steady_clock::now();
            {
                std::ifstream f(probe, std::ios::binary);
                f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            }
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
            if (sec > 1.e-6)
                disk_read_mbps = (static_cast<double>(seq_n) / (1024.0 * 1024.0)) / sec;
        }
        if (created) std::filesystem::remove(probe, ec);
    }
};

} // namespace omsi
