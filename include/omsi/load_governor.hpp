#pragma once

#include "omsi/frame_budget.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace omsi {

/// OMSI_LOAD_TURBO=1: 60 Hz frame target, larger idle/loading slices (still capped for 60 FPS).
inline bool load_turbo_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("OMSI_LOAD_TURBO");
        return e && e[0] && e[0] != '0';
    }();
    return on;
}

/// Main-thread GPU spline upload during tile stream (behind loading UI). Not tied to the shared
/// governor slice — otherwise one 15 ms spline consumes the whole frame and we get ~1 spline/frame.
inline double stream_spline_gpu_burst_ms() {
    // Streaming now runs while the user keeps moving (no blocking overlay), so a 30-52 ms burst per frame was a
    // visible hitch on every tile crossing. Default to a normal 8 ms slice; OMSI_SPLINE_BURST_MS still overrides.
    const char* e = std::getenv("OMSI_SPLINE_BURST_MS");
    if (e && e[0]) {
        try {
            const double v = std::stod(e);
            if (v >= 8.0 && v <= 120.0) return v;
        } catch (...) {
        }
    }
    return 8.0;
}

/// One main-thread load budget per frame, shared by every pump (stream, rails, remesh, terrain,
/// group, SA). Admission is by *predicted milliseconds* learned from real uploads per kind, never by
/// item count: 500 × 2 KB rails fit in one frame, a single 500 KB SCO gets its own frame.
///
/// The slice comes from the frame time *without* load work, so loading never shrinks its own
/// budget. The first item of a frame is always admitted (progress ≥ 1 item/frame); outliers
/// (first texture read, shader compile) go to a per-asset memo instead of poisoning the rate.
///
/// DO NOT TOUCH: OMSI_LOAD_TURBO Idle/Loading caps (throughput); Interactive is only the main
/// apply slice — never “fix” by lowering Idle/Loading. StreamListsJob must stay
/// worker→main (no D3D Device from worker). Staging/fence residency is owned by the upload ring.
class LoadGovernor {
public:
    enum class Kind : std::size_t { Mesh, Texture, Spline, Rail, Terrain, Scenery, Generic, Count };

    static LoadGovernor& global() {
        static LoadGovernor g;
        return g;
    }

    /// OpenOMSI uses 30 ms/frame behind its loading screen and 6 ms while driving. The editor has no
    /// modal loading screen, so the slice follows what the user is doing:
    ///   Interactive — camera/mouse/tool active: small slice (≤ 8 ms), input stays smooth.
    ///   Idle        — nobody touching anything: stream catches up (≤ 20 ms).
    ///   Loading     — first area after boot: OpenOMSI-style big bites (24 ms).
    enum class Mode { Interactive, Idle, Loading };

    double target_frame_ms = 1000.0 / 30.0;

    /// Call once per frame before any pump. frame_ms = last frame wall time.
    /// Slice = leftover to the frame target (never invents a smaller artificial throttle).
    /// Pumps must stop at the deadline — throughput comes from filling the leftover, not from
    /// dumping unbounded work into one frame.
    void begin_frame(double frame_ms, Mode mode = Mode::Interactive) {
        const double frame_budget_ms =
            load_turbo_enabled() ? (1000.0 / 60.0) : target_frame_ms;
        const double base = (std::max)(1.0, frame_ms - last_load_ms_);
        base_ms_ = base_ms_ <= 0.0 ? base : base_ms_ * 0.8 + base * 0.2;
        mode_ = mode;
        const bool turbo = load_turbo_enabled();
        // Keep ~2 ms for present/UI after load when targeting 60 Hz; otherwise fill the frame.
        const double render_reserve_ms = turbo ? 2.0 : 3.0;
        const double leftover = (std::max)(1.0, frame_budget_ms - base_ms_ - render_reserve_ms);
        double floor_ms = 4.0, cap_ms = leftover;
        if (mode == Mode::Idle) {
            floor_ms = (std::max)(turbo ? 6.0 : 8.0, base_ms_ * 0.35);
            // Idle/stream catch-up: allow overshoot of the 60 Hz leftover so floor is not
            // collapsed to 1 ms when base_ms_ (UI/render) ate the whole frame.
            cap_ms = (std::max)(leftover, floor_ms);
            cap_ms = (std::min)(cap_ms, turbo ? 16.0 : 24.0);
        } else if (mode == Mode::Loading) {
            floor_ms = turbo ? 8.0 : 16.0;
            // Loading must keep OpenOMSI-style bites even when leftover collapses to 1 ms.
            cap_ms = (std::max)(leftover, floor_ms);
            cap_ms = (std::min)(cap_ms, turbo ? 16.0 : 24.0);
        } else {
            // Interactive: main apply slice only (~5–6 ms). Workers/IO are not cut by this cap.
            floor_ms = (std::max)(2.5, base_ms_ * 0.10);
            cap_ms = (std::min)(leftover, turbo ? 6.0 : 5.5);
        }
        if (mode == Mode::Loading || mode == Mode::Idle) {
            // Prefer floor when leftover is tiny so DeferHeavy cannot stall forever.
            slice_ms_ = (std::clamp)((std::max)(leftover, floor_ms), floor_ms, cap_ms);
        } else {
            slice_ms_ = (std::clamp)(leftover, (std::min)(floor_ms, cap_ms), cap_ms);
        }
        used_ms_ = 0.0;
        last_load_ms_ = 0.0;
    }
    [[nodiscard]] Mode mode() const { return mode_; }

    /// Non-load frame cost (EWMA). Feed this, not total frame ms, to FrameBudget::for_stream.
    [[nodiscard]] double base_ms() const { return base_ms_; }
    [[nodiscard]] double slice_ms() const { return slice_ms_; }
    [[nodiscard]] double remaining_ms() const { return (std::max)(0.0, slice_ms_ - used_ms_); }

    /// Wall time spent by load pumps this frame (measured around them by the loop).
    void note_load_ms(double ms) { last_load_ms_ += ms; }

    /// Predicted main-thread ms for an item of `bytes`, from learned throughput for this kind.
    [[nodiscard]] double predict(Kind k, std::uint64_t bytes, const std::string* asset = nullptr) const {
        const auto& r = rate(k);
        double ms = r.overhead_ms + static_cast<double>(bytes) * r.ms_per_byte;
        if (asset && !asset->empty()) {
            std::lock_guard<std::mutex> lk(mu_);
            if (const auto it = asset_ms_.find(*asset); it != asset_ms_.end()) ms = (std::max)(ms, it->second);
        }
        return ms;
    }

    /// Learn from a measured upload. Samples > 4× prediction are outliers: remembered per asset,
    /// and only nudge the shared rate (clamped), so one slow first-use cannot throttle a whole kind.
    void learn(Kind k, std::uint64_t bytes, double ms, const std::string* asset = nullptr) {
        used_ms_ += ms;
        auto& r = rate(k);
        const double pred = r.overhead_ms + static_cast<double>(bytes) * r.ms_per_byte;
        const bool outlier = r.samples > 4 && ms > (std::max)(1.0, pred * 4.0);
        const double s = outlier ? (std::max)(1.0, pred * 1.5) : ms;
        const double pred_bytes = static_cast<double>(bytes) * r.ms_per_byte;
        if (bytes < 32 * 1024) {
            r.overhead_ms = r.overhead_ms * 0.9 + (std::max)(0.0, s - pred_bytes) * 0.1;
        } else {
            const double per = (std::max)(0.0, s - r.overhead_ms) / static_cast<double>(bytes);
            r.ms_per_byte = r.ms_per_byte * 0.85 + per * 0.15;
        }
        r.overhead_ms = (std::min)(r.overhead_ms, 8.0);
        r.samples++;
        if (asset && !asset->empty()) {
            std::lock_guard<std::mutex> lk(mu_);
            if (ms > 4.0) asset_ms_[*asset] = ms;
            else asset_ms_.erase(*asset);
        }
    }

    /// Budget for one pump, sized to what is left of the shared slice this frame.
    [[nodiscard]] FrameBudget budget() const {
        FrameBudget b = FrameBudget::for_stream(false, base_ms_, target_frame_ms, 16.0);
        const double left = (std::max)(0.25, remaining_ms());
        b.begin_phase(left);
        b.slice_max_ms = slice_ms_;
        // Bytes are not the limiter — predicted ms is. Keep a generous byte ceiling for GPU memory churn.
        b.target_bytes = 64ull << 20;
        b.upload_bytes = b.geometry_bytes = b.target_bytes;
        b.target_weighted = static_cast<double>(b.target_bytes) * b.io_weight;
        return b;
    }

    /// First item of the frame progresses when it fits (or is GPU-bound). Cold I/O that exceeds
    /// the slice is DeferHeavy so the caller can prefetch/rotate instead of hitching the UI.
    [[nodiscard]] FrameBudget::Admit admit(FrameBudget& fb, Kind k, FrameBudget::WorkCost& c,
                                           const std::string* asset = nullptr) {
        c.est_ms = (std::max)(c.est_ms, predict(k, c.bytes, asset));
        const bool frame_has_load = used_ms_ > 0.0 || fb.used_items > 0;
        if (!frame_has_load) {
            if (c.est_ms > slice_ms_) {
                // Interactive: never Force oversized work onto the UI thread (DeferHeavy + prefetch).
                // Idle/Loading may Force so catch-up / first-area load cannot stall forever.
                if (mode_ == Mode::Interactive || (c.touches_io && mode_ != Mode::Loading)) {
                    ++fb.defer_heavy;
                    return FrameBudget::Admit::DeferHeavy;
                }
                ++fb.forced_one;
                return FrameBudget::Admit::Force;
            }
            return FrameBudget::Admit::Yes;
        }
        if (c.est_ms > remaining_ms()) {
            ++fb.deferred;
            return FrameBudget::Admit::Defer;
        }
        return FrameBudget::Admit::Yes;
    }

    [[nodiscard]] std::string summary() const {
        static const char* names[] = {"mesh", "tex", "spline", "rail", "terrain", "scenery", "gen"};
        static const char* modes[] = {"interactive", "idle", "loading"};
        std::string s = std::string("mode=") + modes[static_cast<int>(mode_)] + " base=" + std::to_string(base_ms_) +
                        " slice=" + std::to_string(slice_ms_);
        for (std::size_t i = 0; i < static_cast<std::size_t>(Kind::Count); ++i) {
            const auto& r = rates_[i];
            if (r.samples == 0) continue;
            s += std::string(" ") + names[i] + "=" + std::to_string(r.overhead_ms) + "ms+" +
                 std::to_string(r.ms_per_byte * 1024.0 * 1024.0) + "ms/MB n=" + std::to_string(r.samples);
        }
        return s;
    }

private:
    struct Rate {
        double overhead_ms = 0.15;
        double ms_per_byte = 1.0 / (512.0 * 1024.0); // 2 ms/MB until learned
        std::uint64_t samples = 0;
    };
    [[nodiscard]] const Rate& rate(Kind k) const { return rates_[static_cast<std::size_t>(k)]; }
    Rate& rate(Kind k) { return rates_[static_cast<std::size_t>(k)]; }

    std::array<Rate, static_cast<std::size_t>(Kind::Count)> rates_{};
    Mode mode_ = Mode::Interactive;
    double base_ms_ = 0.0;
    double slice_ms_ = 4.0;
    double used_ms_ = 0.0;
    double last_load_ms_ = 0.0;
    mutable std::mutex mu_;
    std::unordered_map<std::string, double> asset_ms_;
};

} // namespace omsi
