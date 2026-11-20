#pragma once

#include "omsi/hardware_profile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

namespace omsi {

/// Dual budget: wall time + raw bytes + I/O-weighted bytes. No fixed proto-count cap.
struct FrameBudget {
    std::uint64_t geometry_bytes = 2ull * 1024ull * 1024ull;
    std::uint64_t texture_bytes = 1ull * 1024ull * 1024ull;
    std::uint64_t upload_bytes = 2ull * 1024ull * 1024ull;
    std::uint64_t vertex_count = 1'500'000ull;
    std::uint64_t index_count = 2'000'000ull;
    std::uint32_t draw_calls = 4000;
    double cpu_ms_hard = 2.0;
    double cpu_ms_soft = 1.7;

    double target_ms = 2.0;
    std::size_t target_bytes = 2ull * 1024ull * 1024ull;
    double target_weighted = 8.0 * 1024.0 * 1024.0;
    double io_weight = 4.0;
    // Ceiling of the whole frame (16.67 idle / 33.3 stream). Slice is headroom, not 12% of dt.
    double frame_ceiling_ms = 1000.0 / 30.0;
    double slice_max_ms = 16.0;

    std::uint64_t used_geometry_bytes = 0;
    std::uint64_t used_upload_bytes = 0;
    std::uint64_t used_vertices = 0;
    std::uint64_t used_indices = 0;
    std::uint32_t used_draws = 0;
    std::uint32_t used_items = 0;
    double used_ms = 0.0;
    double used_weighted = 0.0;
    std::size_t admitted = 0;
    std::size_t deferred = 0;
    std::size_t forced_one = 0;
    std::size_t defer_heavy = 0;
    std::size_t io_items = 0;
    bool force_next_heavy = false;

    using clock = std::chrono::steady_clock;
    clock::time_point deadline{};

    void begin_phase(double hard_ms) {
        cpu_ms_hard = hard_ms;
        target_ms = hard_ms;
        deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
                                       std::chrono::duration<double, std::milli>(hard_ms));
        used_geometry_bytes = used_upload_bytes = used_vertices = used_indices = 0;
        used_draws = used_items = 0;
        used_ms = 0.0;
        used_weighted = 0.0;
        admitted = 0;
        deferred = 0;
        forced_one = 0;
        defer_heavy = 0;
        io_items = 0;
    }

    [[nodiscard]] bool time_ok() const { return clock::now() < deadline; }

    [[nodiscard]] double remaining_ms() const {
        return std::chrono::duration<double, std::milli>(deadline - clock::now()).count();
    }

    struct WorkCost {
        std::uint64_t bytes = 0;
        std::uint64_t vertices = 0;
        std::uint64_t indices = 0;
        std::uint32_t draws = 1;
        std::uint32_t materials = 1;
        double est_ms = 0.15;
        double weight = 1.0;
        bool touches_io = false;
        bool is_clone = false;
    };

    /// GPU VB/IB copy estimate from real vert/index counts (no fixed item cap).
    [[nodiscard]] static WorkCost gpu_mesh(std::uint64_t verts, std::uint64_t inds) {
        WorkCost c;
        c.vertices = verts;
        c.indices = inds;
        c.bytes = verts * 32ull + inds * 4ull + 4096ull;
        const double bps = static_cast<double>(HardwareProfile::global().memcpy_bps);
        c.est_ms = bps > 1.0 ? (static_cast<double>(c.bytes) / bps) * 1000.0 + 0.05 : 0.15;
        c.draws = 1;
        return c;
    }

    [[nodiscard]] bool expired() const {
        return used_ms >= target_ms * 0.9 || used_upload_bytes >= target_bytes ||
               used_weighted >= target_weighted;
    }

    enum class Admit { Yes, Force, Defer, DeferHeavy };

    [[nodiscard]] Admit try_admit(const WorkCost& c) {
        if (used_items == 0) {
            // Never Force oversized work via try_admit — Idle/Loading Force goes through
            // LoadGovernor::admit. Interactive callers always DeferHeavy + prefetch/rotate.
            if (c.est_ms > slice_max_ms || c.est_ms > target_ms) {
                force_next_heavy = false;
                ++defer_heavy;
                return Admit::DeferHeavy;
            }
            force_next_heavy = false;
            return Admit::Yes;
        }
        if (!time_ok() || expired()) {
            ++deferred;
            return Admit::Defer;
        }
        if (used_ms + c.est_ms > target_ms) {
            ++deferred;
            return Admit::Defer;
        }
        if (used_upload_bytes + c.bytes > target_bytes) {
            ++deferred;
            return Admit::Defer;
        }
        if (used_weighted + static_cast<double>(c.bytes) * c.weight > target_weighted) {
            ++deferred;
            return Admit::Defer;
        }
        if (c.vertices != 0 && used_vertices + c.vertices > vertex_count) {
            ++deferred;
            return Admit::Defer;
        }
        return Admit::Yes;
    }

    [[nodiscard]] bool can_accept(const WorkCost& c) const {
        if (used_items == 0) return c.est_ms <= slice_max_ms;
        if (used_geometry_bytes + c.bytes > geometry_bytes) return false;
        if (used_upload_bytes + c.bytes > upload_bytes) return false;
        if (c.vertices != 0 && used_vertices + c.vertices > vertex_count) return false;
        if (c.indices != 0 && used_indices + c.indices > index_count) return false;
        return true;
    }

    void consume(const WorkCost& c) {
        used_geometry_bytes += c.bytes;
        used_upload_bytes += c.bytes;
        used_vertices += c.vertices;
        used_indices += c.indices;
        used_draws += c.draws;
        ++used_items;
    }

    void charge(double actual_ms, const WorkCost& c) {
        used_ms += actual_ms;
        used_weighted += static_cast<double>(c.bytes) * c.weight;
        consume(c);
        ++admitted;
        if (c.touches_io) ++io_items;
    }

    void apply_headroom(double measured_render_ms) {
        const double obs = (std::max)(1.0, measured_render_ms);
        double head = (std::max)(0.0, frame_ceiling_ms - obs);
        double t = (std::clamp)(head, 1.0, slice_max_ms);
        if (obs > frame_ceiling_ms) t = (std::max)(1.0, t * 0.5);
        target_ms = t;
        const double cap_bytes =
            slice_max_ms <= 2.5 ? 2.0 * 1024.0 * 1024.0 : 16.0 * 1024.0 * 1024.0;
        target_bytes = static_cast<std::size_t>(
            (std::clamp)(target_ms * 1024.0 * 1024.0, 1.0 * 1024.0 * 1024.0, cap_bytes));
        target_weighted = static_cast<double>(target_bytes) * io_weight;
        cpu_ms_hard = target_ms;
        cpu_ms_soft = target_ms * 0.85;
        upload_bytes = target_bytes;
        geometry_bytes = target_bytes;
        texture_bytes = (std::max)(256ull * 1024ull, target_bytes / 2ull);
    }

    void adapt(double frame_ms_observed) {
        apply_headroom(frame_ms_observed);
        const double io_ratio =
            admitted > 0 ? static_cast<double>(io_items) / static_cast<double>(admitted) : 0.0;
        const std::size_t cap_bytes = slice_max_ms <= 2.5 ? (2ull << 20) : (16ull << 20);
        if (io_ratio > 0.5) {
            target_bytes = (std::max)(1ull << 20, static_cast<std::size_t>(target_bytes * 9 / 10));
        } else if (io_ratio < 0.1 && admitted > 0) {
            target_bytes = (std::min)(cap_bytes, static_cast<std::size_t>(target_bytes * 105 / 100));
        }
        target_weighted = static_cast<double>(target_bytes) * io_weight;
        upload_bytes = target_bytes;
        geometry_bytes = target_bytes;
        texture_bytes = (std::max)(256ull * 1024ull, target_bytes / 2ull);
    }

    [[nodiscard]] std::string summary() const {
        std::ostringstream o;
        o << "admitted=" << admitted << " deferred=" << deferred << " forced=" << forced_one
          << " defer_heavy=" << defer_heavy << " io=" << io_items << " ms=" << used_ms << "/" << target_ms
          << " MB=" << (static_cast<double>(used_upload_bytes) / (1024.0 * 1024.0)) << "/"
          << (static_cast<double>(target_bytes) / (1024.0 * 1024.0));
        return o.str();
    }

    static FrameBudget for_stream(bool force_teleport, double measured_render_ms = 10.0,
                                  double target_frame_ms = 1000.0 / 30.0, double slice_max_ms = 16.0) {
        FrameBudget b;
        const auto& hw = HardwareProfile::global();
        b.io_weight = hw.io_weight;
        b.frame_ceiling_ms = (std::max)(8.0, target_frame_ms);
        b.slice_max_ms = (std::clamp)(slice_max_ms, 1.0, 16.0);
        b.apply_headroom(measured_render_ms);
        if (force_teleport) {
            b.target_ms = (std::min)(b.slice_max_ms, b.target_ms * 1.5);
            const double cap_bytes =
                b.slice_max_ms <= 2.5 ? 2.0 * 1024.0 * 1024.0 : 16.0 * 1024.0 * 1024.0;
            b.target_bytes = static_cast<std::size_t>(
                (std::clamp)(b.target_ms * 1024.0 * 1024.0, 1.0 * 1024.0 * 1024.0, cap_bytes));
            b.target_weighted = static_cast<double>(b.target_bytes) * b.io_weight;
            b.cpu_ms_hard = b.target_ms;
            b.cpu_ms_soft = b.target_ms * 0.85;
            b.upload_bytes = b.target_bytes;
            b.geometry_bytes = b.target_bytes;
            b.texture_bytes = (std::max)(256ull * 1024ull, b.target_bytes / 2ull);
        }
        b.vertex_count = force_teleport ? 4'000'000ull : 2'500'000ull;
        b.index_count = force_teleport ? 5'000'000ull : 3'000'000ull;
        return b;
    }
};

} // namespace omsi
