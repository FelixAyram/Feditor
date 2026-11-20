#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <exception>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "omsi/logger.hpp"

namespace omsi {

/// Work-stealing pool. Mutexes are heap-allocated (std::mutex is not movable → no vector<mutex>).
/// One pool. By default reserve one logical CPU for the interactive/editor thread;
/// OMSI_THREAD_COUNT / EDITOR_THREAD_COUNT override, and OMSI_LEAVE_UI_CORE=0 opts into full saturation.
class JobSystem {
public:
    using Job = std::function<void()>;

    enum class Priority : std::uint8_t {
        Low = 0,
        Normal = 1,
        High = 2,
        Critical = 3,
    };

    explicit JobSystem(unsigned worker_count = 0) {
        unsigned logical = std::thread::hardware_concurrency();
        if (logical == 0) logical = 1;
        unsigned affinity_bits = logical;
#ifdef _WIN32
        DWORD_PTR proc_mask = 0;
        DWORD_PTR sys_mask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) && proc_mask) {
            unsigned bits = 0;
            for (DWORD_PTR b = proc_mask; b; b >>= 1)
                bits += static_cast<unsigned>(b & 1);
            if (bits > 0) affinity_bits = bits;
        }
#endif
        unsigned resolved = worker_count;
        bool env_override = false;
        if (resolved == 0) {
            if (const char* e = std::getenv("OMSI_THREAD_COUNT")) {
                resolved = static_cast<unsigned>((std::max)(1, std::atoi(e)));
                env_override = true;
            } else if (const char* e2 = std::getenv("EDITOR_THREAD_COUNT")) {
                resolved = static_cast<unsigned>((std::max)(1, std::atoi(e2)));
                env_override = true;
            } else {
                resolved = affinity_bits;
            }
        }
        // Saturating every logical core caused the render/main thread to be starved while
        // terrain/spline jobs were running. Keep one logical CPU available by default.
        // An explicit worker-count override is respected; set OMSI_LEAVE_UI_CORE=0 to
        // opt back into full CPU saturation for offline/batch operations.
        const bool leave_ui = [] {
            const char* e = std::getenv("OMSI_LEAVE_UI_CORE");
            return !e || !e[0] || e[0] != '0';
        }();
        if (!env_override && worker_count == 0 && leave_ui) {
            if (resolved > 2) --resolved;
        }
        resolved = (std::max)(1u, resolved);

        queues_.resize(resolved);
        queue_mutexes_.reserve(resolved);
        for (unsigned i = 0; i < resolved; ++i)
            queue_mutexes_.push_back(std::make_unique<std::mutex>());

        workers_.reserve(resolved);
        for (unsigned i = 0; i < resolved; ++i)
            workers_.emplace_back([this, i] { worker_loop(i); });

#ifdef _WIN32
        if (proc_mask) {
            unsigned bit = 0;
            auto next_bit = [&]() -> int {
                while (bit < sizeof(DWORD_PTR) * 8) {
                    const DWORD_PTR m = (DWORD_PTR)1 << bit;
                    if (proc_mask & m) {
                        const int out = static_cast<int>(bit);
                        ++bit;
                        return out;
                    }
                    ++bit;
                }
                return -1;
            };
            for (unsigned i = 0; i < workers_.size(); ++i) {
                const int b = next_bit();
                if (b < 0) break;
                const DWORD_PTR one = (DWORD_PTR)1 << b;
                SetThreadAffinityMask(workers_[i].native_handle(), one);
            }
        }
        // Workers below the UI thread: on a slow CPU with every core busy loading, the main thread
        // lost whole scheduler quanta (15-80 ms stutters, often inside PeekMessage) to equal-priority
        // workers. OMSI_WORKER_PRIO=normal restores the old behaviour for batch tools.
        {
            const char* e = std::getenv("OMSI_WORKER_PRIO");
            const bool keep_normal = e && (e[0] == 'n' || e[0] == 'N');
            if (!keep_normal)
                for (auto& w : workers_) SetThreadPriority(w.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
        }
#endif
        char line[256];
        std::snprintf(line, sizeof(line),
                      "JobSystem workers=%u logical=%u affinity=%u env_override=%d", resolved, logical,
                      affinity_bits, env_override ? 1 : 0);
        std::fprintf(stderr, "%s\n", line);
        log::info(line);
    }

    ~JobSystem() { shutdown(); }

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    [[nodiscard]] unsigned worker_count() const { return static_cast<unsigned>(workers_.size()); }
    [[nodiscard]] std::uint64_t pending_count() const { return pending_.load(std::memory_order_relaxed); }

    void submit(Job job, Priority prio = Priority::Normal) {
        if (!job) return;
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (stop_.load(std::memory_order_acquire)) return;
        unsigned best = 0;
        std::size_t best_n = SIZE_MAX;
        for (unsigned i = 0; i < queues_.size(); ++i) {
            std::lock_guard<std::mutex> lock(*queue_mutexes_[i]);
            if (queues_[i].size() < best_n) {
                best_n = queues_[i].size();
                best = i;
            }
        }
        // Publish the pending count while holding the queue lock and BEFORE
        // making the job visible. Otherwise a worker can pop/finish the job
        // and decrement pending_ before this submitter increments it.
        {
            std::lock_guard<std::mutex> lock(*queue_mutexes_[best]);
            pending_.fetch_add(1, std::memory_order_release);
            try {
                if (prio >= Priority::High)
                    queues_[best].push_front(std::move(job)); // high: run sooner
                else
                    queues_[best].push_back(std::move(job));
            } catch (...) {
                pending_.fetch_sub(1, std::memory_order_acq_rel);
                throw;
            }
        }
        cv_.notify_one();
    }

    /// Parallel loop that waits ONLY for its own chunks.
    ///
    /// The previous implementation ended with wait_idle(), i.e. it blocked until EVERY pending job in
    /// the whole process had finished and, while waiting, executed unrelated jobs (terrain builds,
    /// SLI parses, ...) on the calling thread. Called from the main/editor thread that turned a
    /// 2 ms loop into multi-second freezes whenever background streaming work was queued.
    ///
    /// Now chunks are claimed from a shared atomic cursor by the caller AND by helper jobs submitted
    /// to the pool. The caller never runs foreign jobs; if every worker is busy with long jobs it
    /// simply processes all chunks itself, so the call is bounded by its own work.
    template <typename Fn>
    void parallel_for(std::size_t begin, std::size_t end, Fn&& fn, std::size_t grain = 1,
                      Priority prio = Priority::Normal) {
        if (begin >= end) return;
        if (workers_.empty() || end - begin <= 1) {
            for (std::size_t i = begin; i < end; ++i) fn(i);
            return;
        }
        if (grain < 1) grain = 1;
        const std::size_t total_chunks = (end - begin + grain - 1) / grain;
        if (total_chunks <= 1) {
            for (std::size_t i = begin; i < end; ++i) fn(i);
            return;
        }

        struct LoopState {
            std::atomic<std::size_t> next{0};
            std::atomic<std::size_t> done{0};
            std::size_t total = 0;
            std::size_t begin = 0;
            std::size_t end = 0;
            std::size_t grain = 1;
            std::mutex mu;
            std::condition_variable cv;
        };
        auto st = std::make_shared<LoopState>();
        st->total = total_chunks;
        st->begin = begin;
        st->end = end;
        st->grain = grain;

        using FnT = std::remove_reference_t<Fn>;
        FnT* fn_ptr = &fn;
        // Helpers only dereference fn_ptr after successfully claiming a chunk. A chunk can only be
        // claimed before the caller observes done == total, so fn_ptr is always still alive.
        auto run_chunks = [fn_ptr](const std::shared_ptr<LoopState>& s) {
            for (;;) {
                const std::size_t c = s->next.fetch_add(1, std::memory_order_acq_rel);
                if (c >= s->total) return;
                const std::size_t a = s->begin + c * s->grain;
                const std::size_t b = (std::min)(a + s->grain, s->end);
                try {
                    for (std::size_t k = a; k < b; ++k) (*fn_ptr)(k);
                } catch (...) {
                    // Keep the latch consistent; a throwing body must not deadlock the caller.
                    log::error("JobSystem::parallel_for body threw");
                }
                if (s->done.fetch_add(1, std::memory_order_acq_rel) + 1 == s->total) {
                    std::lock_guard<std::mutex> lk(s->mu);
                    s->cv.notify_all();
                }
            }
        };

        const std::size_t helpers = (std::min)(static_cast<std::size_t>(workers_.size()), total_chunks - 1);
        for (std::size_t h = 0; h < helpers; ++h) submit([st, run_chunks] { run_chunks(st); }, prio);

        run_chunks(st); // the caller works on its own loop only
        if (st->done.load(std::memory_order_acquire) < st->total) {
            std::unique_lock<std::mutex> lk(st->mu);
            st->cv.wait(lk, [&] { return st->done.load(std::memory_order_acquire) >= st->total; });
        }
    }

    /// Fire-and-forget convenience: run `fn` on the pool at Low priority.
    void submit_background(Job job) { submit(std::move(job), Priority::Low); }

    void wait_idle() {
        while (pending_.load(std::memory_order_acquire) > 0) {
            help_once();
            std::this_thread::yield();
        }
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
            if (stop_.exchange(true)) return;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    static JobSystem& global() {
        static JobSystem inst;
        return inst;
    }

private:
    static void execute_job(Job& job) noexcept {
        try {
            job();
        } catch (const std::exception& e) {
            // Do not let an exception escape a worker thread and terminate the
            // process. Keep this on the worker path, never on the main loop.
            log::error(std::string("JobSystem task threw std::exception: ") + e.what());
        } catch (...) {
            log::error("JobSystem task threw an unknown exception");
        }
    }

    bool try_pop(unsigned self, Job& out) {
        std::lock_guard<std::mutex> lock(*queue_mutexes_[self]);
        if (queues_[self].empty()) return false;
        out = std::move(queues_[self].front());
        queues_[self].pop_front();
        return true;
    }

    bool try_steal(unsigned self, Job& out) {
        const unsigned n = static_cast<unsigned>(queues_.size());
        for (unsigned k = 1; k < n; ++k) {
            const unsigned victim = (self + k) % n;
            std::lock_guard<std::mutex> lock(*queue_mutexes_[victim]);
            if (queues_[victim].empty()) continue;
            out = std::move(queues_[victim].back());
            queues_[victim].pop_back();
            return true;
        }
        return false;
    }

    void help_once() {
        Job job;
        for (unsigned i = 0; i < queues_.size(); ++i) {
            if (try_pop(i, job) || try_steal(i, job)) {
                execute_job(job);
                pending_.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
        }
    }

    void worker_loop(unsigned self) {
        Job job;
        for (;;) {
            if (try_pop(self, job) || try_steal(self, job)) {
                execute_job(job);
                pending_.fetch_sub(1, std::memory_order_acq_rel);
                continue;
            }
            if (stop_.load(std::memory_order_acquire) && pending_.load(std::memory_order_acquire) == 0)
                return;
            std::unique_lock<std::mutex> lock(wait_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(2), [&] {
                return stop_.load(std::memory_order_acquire) || pending_.load(std::memory_order_acquire) > 0;
            });
        }
    }

    std::vector<std::thread> workers_;
    std::vector<std::deque<Job>> queues_;
    std::vector<std::unique_ptr<std::mutex>> queue_mutexes_;
    std::mutex wait_mutex_;
    std::mutex lifecycle_mutex_; // serializes submit against shutdown
    std::condition_variable cv_;
    std::atomic<std::uint64_t> pending_{0};
    std::atomic<bool> stop_{false};
};

} // namespace omsi
