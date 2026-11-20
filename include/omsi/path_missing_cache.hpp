#pragma once

// Async "does this SCO/SLI file exist?" cache.
//
// refresh_editor_lists() used to call std::filesystem::exists() for every object and
// spline path on the main thread. On a cold HDD that is thousands of random probes and
// it is exactly what produced the 7.3 s stream_apply freeze (main_hang.log frame 1584:
// 6,810 ms; frame 3379: 6,706 ms) plus the 944 `critical` events.
//
// The cache answers from memory only. Unknown paths are reported as *present*
// (optimistic) and queued; one background job resolves them, then generation() bumps and
// the editor lists are rebuilt with accurate "missing ..." labels.
//
// Main thread: snapshot() + missing_in()/known_in() inside the rebuild loop, request()
// at the end. Worker: resolves the queued paths and publishes a copy-on-write map.
//
// Do NOT put std::filesystem::exists on the main thread again.

#include "omsi/job_system.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace omsi {

class PathMissingCache {
public:
    // key -> 0 = exists, 1 = missing. Immutable once published.
    using Map = std::unordered_map<std::string, char>;

    static PathMissingCache& global() {
        static PathMissingCache inst;
        return inst;
    }

    [[nodiscard]] std::shared_ptr<const Map> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_;
    }

    [[nodiscard]] std::uint64_t generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

    // Absent from the map => unknown => optimistically "present".
    static bool missing_in(const Map& map, const std::filesystem::path& p) {
        if (p.empty()) return true;
        const auto it = map.find(p.lexically_normal().string());
        return it != map.end() && it->second != 0;
    }

    static bool known_in(const Map& map, const std::filesystem::path& p) {
        if (p.empty()) return true;
        return map.find(p.lexically_normal().string()) != map.end();
    }

    // Main thread. Queues unknown paths; submits at most one background job per call.
    void request(std::vector<std::filesystem::path> paths, std::filesystem::path omsi_root) {
        if (paths.empty()) return;
        std::vector<std::filesystem::path> todo;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            todo.reserve(paths.size());
            for (auto& p : paths) {
                if (p.empty()) continue;
                const std::string key = p.lexically_normal().string();
                if (map_->find(key) != map_->end()) continue;
                if (!pending_.insert(key).second) continue; // already queued or in flight
                todo.push_back(std::move(p));
            }
        }
        if (todo.empty()) return;
        auto root = std::move(omsi_root);
        JobSystem::global().submit_background(
            [this, todo = std::move(todo), root = std::move(root)]() mutable {
                std::vector<std::pair<std::string, char>> results;
                results.reserve(todo.size());
                for (const auto& p : todo) {
                    std::error_code ec;
                    bool miss = !std::filesystem::exists(p, ec);
                    if (miss && !root.empty()) miss = !std::filesystem::exists(root / p, ec);
                    results.emplace_back(p.lexically_normal().string(),
                                         miss ? static_cast<char>(1) : static_cast<char>(0));
                }
                std::lock_guard<std::mutex> lock(mutex_);
                auto next = std::make_shared<Map>(*map_);
                for (auto& kv : results) {
                    (*next)[kv.first] = kv.second;
                    pending_.erase(kv.first);
                }
                map_ = std::move(next);
                ++generation_;
            });
    }

private:
    PathMissingCache() = default;

    mutable std::mutex mutex_;
    std::shared_ptr<const Map> map_ = std::make_shared<const Map>();
    std::unordered_set<std::string> pending_;
    std::uint64_t generation_ = 0;
};

} // namespace omsi
