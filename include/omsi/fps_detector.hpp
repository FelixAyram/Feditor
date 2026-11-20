#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace omsi {

// PresentMon / FrameView style: smooth FPS with EWMA, 1% low from frame-time percentile
// (never percentile FPS). Detects CPU vs GPU vs Present vs fence stalls.
struct FpsDetector {
    static constexpr std::size_t kWindow = 120;
    static constexpr double kEwmaAlpha = 0.12; // slower than 0.9: title bar must not flicker
    static constexpr double kBudgetMs = 1000.0 / 60.0;

    void push(double frame_ms, double gpu_total_ms, double gpu_scene_ms, double gpu_ui_ms,
              double wait_fence_ms, double present_ms, double cpu_populate_ms) {
        frame_ms = (std::max)(frame_ms, 0.001);
        samples_[write_] = frame_ms;
        gpu_samples_[write_] = (std::max)(0.0, gpu_total_ms);
        write_ = (write_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;

        const double inst_fps = 1000.0 / frame_ms;
        if (smoothed_fps_ <= 0.0) smoothed_fps_ = inst_fps;
        else smoothed_fps_ = kEwmaAlpha * inst_fps + (1.0 - kEwmaAlpha) * smoothed_fps_;

        last_frame_ms_ = frame_ms;
        last_gpu_ms_ = gpu_total_ms;
        last_gpu_scene_ms_ = gpu_scene_ms;
        last_gpu_ui_ms_ = gpu_ui_ms;
        last_wait_fence_ms_ = wait_fence_ms;
        last_present_ms_ = present_ms;
        last_populate_ms_ = cpu_populate_ms;
        last_bound_ = classify(frame_ms, gpu_total_ms, gpu_scene_ms, gpu_ui_ms, wait_fence_ms, present_ms,
                               cpu_populate_ms);
        if (frame_ms > kBudgetMs) ++over_budget_;
        ++total_frames_;
    }

    [[nodiscard]] double smoothed_fps() const { return smoothed_fps_; }
    [[nodiscard]] double last_frame_ms() const { return last_frame_ms_; }
    [[nodiscard]] double last_gpu_ms() const { return last_gpu_ms_; }
    [[nodiscard]] const char* bound() const { return last_bound_; }
    [[nodiscard]] std::uint64_t over_budget() const { return over_budget_; }
    [[nodiscard]] std::uint64_t frames() const { return total_frames_; }

    // 1% low FPS: 99th percentile of frame times in the window, inverted.
    [[nodiscard]] double fps_1pct_low() const {
        if (count_ == 0) return 0.0;
        std::array<double, kWindow> tmp{};
        for (std::size_t i = 0; i < count_; ++i) tmp[i] = samples_[i];
        std::sort(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(count_));
        const std::size_t idx = (std::min)(count_ - 1, (count_ * 99) / 100);
        const double p99_ms = tmp[idx];
        return p99_ms > 0.001 ? 1000.0 / p99_ms : 0.0;
    }

    [[nodiscard]] double max_ms() const {
        double m = 0.0;
        for (std::size_t i = 0; i < count_; ++i) m = (std::max)(m, samples_[i]);
        return m;
    }

    [[nodiscard]] double avg_gpu_ms() const {
        if (count_ == 0) return 0.0;
        double s = 0.0;
        for (std::size_t i = 0; i < count_; ++i) s += gpu_samples_[i];
        return s / static_cast<double>(count_);
    }

    [[nodiscard]] std::string summary_line() const {
        char buf[384]{};
        std::snprintf(buf, sizeof(buf),
                      "FPSDET ewma=%.1f 1pct=%.1f frame_ms=%.2f gpu_ms=%.2f gpu_scene=%.2f gpu_ui=%.2f "
                      "wait_fence=%.2f present=%.2f populate=%.2f bound=%s over60=%llu/%llu",
                      smoothed_fps_, fps_1pct_low(), last_frame_ms_, last_gpu_ms_, last_gpu_scene_ms_,
                      last_gpu_ui_ms_, last_wait_fence_ms_, last_present_ms_, last_populate_ms_, last_bound_,
                      static_cast<unsigned long long>(over_budget_),
                      static_cast<unsigned long long>(total_frames_));
        return buf;
    }

    static const char* classify(double frame_ms, double gpu_total, double gpu_scene, double gpu_ui,
                                double wait_fence, double present, double populate) {
        if (frame_ms < kBudgetMs && gpu_total < kBudgetMs) return "ok";
        if (present >= 8.0 && present >= gpu_total * 0.6) return "present_vsync";
        if (wait_fence >= 4.0 && gpu_total >= 8.0) return "gpu_bound";
        if (gpu_ui > gpu_scene && gpu_ui >= 4.0) return "gpu_ui";
        if (gpu_scene >= 8.0 && gpu_scene >= populate) return "gpu_scene";
        if (populate >= 8.0) return "cpu_record";
        if (wait_fence >= 4.0) return "cpu_wait_gpu";
        if (gpu_total < 4.0 && frame_ms >= kBudgetMs) return "cpu_sim";
        return "mixed";
    }

private:
    std::array<double, kWindow> samples_{};
    std::array<double, kWindow> gpu_samples_{};
    std::size_t write_ = 0;
    std::size_t count_ = 0;
    double smoothed_fps_ = 0.0;
    double last_frame_ms_ = 0.0;
    double last_gpu_ms_ = 0.0;
    double last_gpu_scene_ms_ = 0.0;
    double last_gpu_ui_ms_ = 0.0;
    double last_wait_fence_ms_ = 0.0;
    double last_present_ms_ = 0.0;
    double last_populate_ms_ = 0.0;
    const char* last_bound_ = "ok";
    std::uint64_t over_budget_ = 0;
    std::uint64_t total_frames_ = 0;
};

} // namespace omsi
