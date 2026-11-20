#pragma once

// Low-overhead main-thread stall watchdog. Deliberately independent from the
// engine logger: a logger lock or disk flush must not disable hang reporting.
//
// Tiers (wall time stuck in one phase without Scope progress):
//   soft     16.67 ms  — one missed 60 Hz frame
//   warning  33 ms     — two frames / ~30 Hz floor
//   critical 250 ms    — hard hang
// Dead-time: if frame_started/last_progress stalls ≥ 200 ms while a phase is
// active, escalate even if phase_since_ was recently reset incorrectly.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

namespace omsi {

class MainThreadWatchdog {
public:
    using Clock = std::chrono::steady_clock;

    inline static MainThreadWatchdog* s_active = nullptr;

    static constexpr std::chrono::milliseconds kSoft{17};      // ≈16.67
    static constexpr std::chrono::milliseconds kWarning{33};
    static constexpr std::chrono::milliseconds kCritical{250};
    static constexpr std::chrono::milliseconds kDeadTime{200};

    explicit MainThreadWatchdog(const char* path = "main_hang.log",
                                std::chrono::milliseconds poll = std::chrono::milliseconds(5),
                                std::chrono::milliseconds /*warning_unused*/ = kWarning,
                                std::chrono::milliseconds /*critical_unused*/ = kCritical)
        : poll_(poll), file_(open_rotated(path)) {
        s_active = this;
        if (file_) std::setvbuf(file_, nullptr, _IOFBF, 64 * 1024);
        const auto now = ticks(Clock::now());
        phase_since_.store(now, std::memory_order_relaxed);
        last_progress_.store(now, std::memory_order_relaxed);
        phase_.store("startup", std::memory_order_relaxed);
        if (file_) {
            std::fprintf(file_,
                         "\nMAIN_WATCHDOG start poll_ms=%lld soft_ms=%lld warning_ms=%lld "
                         "critical_ms=%lld dead_ms=%lld\n",
                         static_cast<long long>(poll_.count()),
                         static_cast<long long>(kSoft.count()),
                         static_cast<long long>(kWarning.count()),
                         static_cast<long long>(kCritical.count()),
                         static_cast<long long>(kDeadTime.count()));
            std::fflush(file_);
        }
        thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    ~MainThreadWatchdog() {
        if (s_active == this) s_active = nullptr;
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
        if (file_) {
            std::fflush(file_);
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    MainThreadWatchdog(const MainThreadWatchdog&) = delete;
    MainThreadWatchdog& operator=(const MainThreadWatchdog&) = delete;

    class Scope {
    public:
        Scope() noexcept : owner_(nullptr), previous_(nullptr) {}
        Scope(MainThreadWatchdog& owner, const char* name) noexcept
            : owner_(&owner), previous_(owner.phase_.load(std::memory_order_relaxed)) {
            const auto now = ticks(Clock::now());
            owner_->charge(previous_, now - owner_->phase_since_.load(std::memory_order_relaxed));
            owner_->phase_.store(name ? name : "unnamed", std::memory_order_relaxed);
            owner_->phase_since_.store(now, std::memory_order_relaxed);
        }
        Scope(Scope&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), previous_(other.previous_) {}
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() {
            if (owner_)
                owner_->charge(owner_->phase_.load(std::memory_order_relaxed),
                               ticks(Clock::now()) - owner_->phase_since_.load(std::memory_order_relaxed));
            if (!owner_) return;
            owner_->phase_.store(previous_ ? previous_ : "frame", std::memory_order_relaxed);
            owner_->phase_since_.store(ticks(Clock::now()), std::memory_order_relaxed);
            owner_->last_progress_.store(ticks(Clock::now()), std::memory_order_release);
        }

    private:
        MainThreadWatchdog* owner_;
        const char* previous_;
    };

    static Scope active_scope(const char* name) noexcept {
        if (s_active) return Scope(*s_active, name);
        return Scope{};
    }

    [[nodiscard]] Scope phase(const char* name) noexcept { return Scope(*this, name); }

    // Stack sampler (app side, crash_report.cpp): on every warning / critical report the main thread's
    // call stack is written as MAIN_STACK (module+rva per frame) - a profile of each hang.
    using StackSampler = void (*)(void* thread, char* out, std::size_t cap);
    void set_stack_sampler(StackSampler fn, void* thread) noexcept {
        sample_thread_.store(thread, std::memory_order_relaxed);
        sampler_.store(fn, std::memory_order_release);
    }

    // OMSI_PERF_PHASES: linear section marks (independent of the Scope stack): the time since the
    // previous mark is charged to that mark's name.
    void mark(const char* name) noexcept {
        if (!prof_on_) return;
        const auto now = ticks(Clock::now());
        if (mark_name_) charge(mark_name_, now - mark_since_);
        mark_name_ = name;
        mark_since_ = now;
    }

    void frame_started(std::uint64_t frame) noexcept {
        if (prof_on_) prof_frame();
        frame_.store(frame, std::memory_order_relaxed);
        last_progress_.store(ticks(Clock::now()), std::memory_order_release);
    }

    /// Snapshot of top CPU phases for MAIN_BUDGET lines (call from main thread).
    void note_budget(std::uint64_t frame, double total_ms, double active_ms, double present_ms,
                     const char* bound, const char* top1, double top1_ms, const char* top2,
                     double top2_ms, const char* top3, double top3_ms) noexcept {
        if (!file_ || !note_allowed()) return;
        std::fprintf(file_,
                     "MAIN_BUDGET frame=%llu total_ms=%.2f active_ms=%.2f present_ms=%.2f bound=%s "
                     "top1=%s:%.2f top2=%s:%.2f top3=%s:%.2f\n",
                     static_cast<unsigned long long>(frame), total_ms, active_ms, present_ms,
                     bound ? bound : "?", top1 ? top1 : "?", top1_ms, top2 ? top2 : "?", top2_ms,
                     top3 ? top3 : "?", top3_ms);
    }

    /// Bridge MAPPERF_SPIKE / hitch tags into the hang log (main thread).
    void note_line(const char* line) noexcept {
        if (!file_ || !line || !note_allowed()) return;
        std::fputs(line, file_);
        std::fputc('\n', file_);
    }

private:
    // Slow PCs hit >16.7 ms every frame: 3-4 lines + fflush per frame grew main_hang.log to 200 MB
    // and the disk writes fed the stutter. Main-thread notes: at most 6 lines/s, buffered (the
    // watchdog thread flushes once a second and on MAIN_HANG lines).
    std::int64_t note_window_ = 0;
    int note_count_ = 0;
    long long note_dropped_ = 0;
    bool note_allowed() noexcept {
        const auto now = ticks(Clock::now());
        if (now - note_window_ >= 1'000'000'000LL) {
            if (note_dropped_ > 0 && file_) std::fprintf(file_, "NOTES_DROPPED n=%lld\n", note_dropped_);
            note_window_ = now;
            note_count_ = 0;
            note_dropped_ = 0;
        }
        if (note_count_ >= 6) {
            ++note_dropped_;
            return false;
        }
        ++note_count_;
        return true;
    }
    // Keep one previous log; start fresh once the file passed 32 MB.
    static std::FILE* open_rotated(const char* path) noexcept {
        if (std::FILE* f = std::fopen(path, "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fclose(f);
            if (size > 32L * 1024L * 1024L) {
                char prev[512];
                std::snprintf(prev, sizeof(prev), "%s.prev", path);
                std::remove(prev);
                std::rename(path, prev);
            }
        }
        return std::fopen(path, "ab");
    }
    // OMSI_PERF_PHASES=1: wall time per phase (self time between Scope changes), dumped every 5 s to
    // the hang log as PHASES lines (ms per frame, top 14). Main thread only, no locks.
    struct ProfEntry {
        const char* name = nullptr;
        std::int64_t ns = 0;
    };
    bool prof_on_ = std::getenv("OMSI_PERF_PHASES") != nullptr;
    ProfEntry prof_[96]{};
    int prof_frames_ = 0;
    const char* mark_name_ = nullptr;
    std::int64_t mark_since_ = 0;
    std::int64_t prof_window_start_ = 0;
    void charge(const char* name, std::int64_t ns) noexcept {
        if (!prof_on_ || !name || ns <= 0) return;
        for (auto& e : prof_) {
            if (e.name == name || (e.name && std::strcmp(e.name, name) == 0)) {
                e.ns += ns;
                return;
            }
            if (!e.name) {
                e.name = name;
                e.ns = ns;
                return;
            }
        }
    }
    void prof_frame() noexcept {
        ++prof_frames_;
        const auto now = ticks(Clock::now());
        if (prof_window_start_ == 0) prof_window_start_ = now;
        if (now - prof_window_start_ < 5'000'000'000LL || !file_) return;
        ProfEntry sorted[96];
        int n = 0;
        for (const auto& e : prof_)
            if (e.name) sorted[n++] = e;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                if (sorted[j].ns > sorted[i].ns) std::swap(sorted[i], sorted[j]);
        std::fprintf(file_, "PHASES frames=%d fps=%.1f", prof_frames_,
                     prof_frames_ * 1e9 / static_cast<double>(now - prof_window_start_));
        for (int i = 0; i < n && i < 14; ++i)
            std::fprintf(file_, " %s=%.2f", sorted[i].name, sorted[i].ns / 1e6 / (prof_frames_ ? prof_frames_ : 1));
        std::fputc(10, file_);
        std::fflush(file_);
        for (auto& e : prof_) e = ProfEntry{};
        prof_frames_ = 0;
        prof_window_start_ = now;
    }

    static std::int64_t ticks(Clock::time_point t) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    }

    void run(std::stop_token stop) noexcept {
        int last_tier = 0; // 0 ok, 1 soft, 2 warning, 3 critical
        auto last_report = Clock::time_point{};
        auto last_flush = Clock::now();
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(poll_);
            const auto now = Clock::now();
            if (file_ && now - last_flush >= std::chrono::seconds(1)) {
                std::fflush(file_);
                last_flush = now;
            }
            const auto phase_ms = (ticks(now) - phase_since_.load(std::memory_order_relaxed)) / 1000000;
            const auto dead_ms = (ticks(now) - last_progress_.load(std::memory_order_acquire)) / 1000000;
            const auto phase = phase_.load(std::memory_order_relaxed);
            const auto frame = frame_.load(std::memory_order_relaxed);

            // Dead-time: no frame_started / Scope dtor for ≥ 200 ms → count as stall.
            std::int64_t stall_ms = phase_ms;
            if (dead_ms >= kDeadTime.count()) stall_ms = (std::max)(stall_ms, dead_ms);

            int tier = 0;
            if (stall_ms >= kCritical.count())
                tier = 3;
            else if (stall_ms >= kWarning.count())
                tier = 2;
            else if (stall_ms >= kSoft.count())
                tier = 1;

            if (tier == 0) {
                if (last_tier > 0) {
                    write_line("MAIN_RECOVER frame=%llu phase=%s stall_ms=%lld dead_ms=%lld",
                               static_cast<unsigned long long>(frame), phase ? phase : "unknown",
                               static_cast<long long>(stall_ms), static_cast<long long>(dead_ms));
                    last_tier = 0;
                }
                continue;
            }

            const auto report_period =
                tier >= 3 ? std::chrono::milliseconds(100) : (tier >= 2 ? kCritical : kWarning);
            // Soft (one missed 60 Hz frame) is the norm on slow CPUs and MAPPERF_SPIKE already
            // records it: only warning and above go to the file.
            if (tier >= 2 && (tier > last_tier || now - last_report >= report_period)) {
                const char* level = tier >= 3 ? "critical" : (tier >= 2 ? "warning" : "soft");
                write_line("MAIN_HANG level=%s frame=%llu stalled_ms=%lld dead_ms=%lld phase=%s",
                           level, static_cast<unsigned long long>(frame),
                           static_cast<long long>(stall_ms), static_cast<long long>(dead_ms),
                           phase ? phase : "unknown");
                if (StackSampler fn = sampler_.load(std::memory_order_acquire)) {
                    char stack[1600];
                    stack[0] = 0;
                    fn(sample_thread_.load(std::memory_order_relaxed), stack, sizeof(stack));
                    if (stack[0])
                        write_line("MAIN_STACK frame=%llu stalled_ms=%lld phase=%s %s",
                                   static_cast<unsigned long long>(frame), static_cast<long long>(stall_ms),
                                   phase ? phase : "unknown", stack);
                }
                last_tier = tier;
                last_report = now;
            }
        }
    }

    template <typename... Args>
    void write_line(const char* format, Args... args) noexcept {
        if (!file_) return;
        std::fprintf(file_, format, args...);
        std::fputc('\n', file_);
        std::fflush(file_);
    }

    std::chrono::milliseconds poll_;
    std::FILE* file_ = nullptr;
    std::jthread thread_;
    std::atomic<const char*> phase_{"startup"};
    std::atomic<std::int64_t> phase_since_{0};
    std::atomic<std::int64_t> last_progress_{0};
    std::atomic<std::uint64_t> frame_{0};
    std::atomic<StackSampler> sampler_{nullptr};
    std::atomic<void*> sample_thread_{nullptr};
};

} // namespace omsi
