#pragma once

#include "omsi/o3d_format.hpp"
#include "o3d/o3d_loader.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omsi::o3d {

// Process-wide GPU-ready vertex blobs (decrypt + 0x79 remap). Main thread copies; workers fill.
class PreparedVertexCache {
public:
    static PreparedVertexCache& instance() {
        static PreparedVertexCache c;
        return c;
    }

    [[nodiscard]] static std::string key_of(const std::filesystem::path& p) {
        return p.lexically_normal().string();
    }

    [[nodiscard]] const std::vector<Vertex>* try_get(const std::filesystem::path& p) const {
        const auto k = key_of(p);
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = map_.find(k);
        if (it == map_.end()) return nullptr;
        return it->second.get();
    }

    [[nodiscard]] bool ready(const std::filesystem::path& p) const { return try_get(p) != nullptr; }

    [[nodiscard]] bool failed(const std::filesystem::path& p) const {
        if (p.empty()) return true;
        const auto k = key_of(p);
        std::lock_guard<std::mutex> lk(mu_);
        return failed_.count(k) != 0;
    }

    void store(const std::filesystem::path& p, std::vector<Vertex> verts) {
        if (p.empty() || verts.empty()) return;
        auto sp = std::make_shared<std::vector<Vertex>>(std::move(verts));
        const auto k = key_of(p);
        std::lock_guard<std::mutex> lk(mu_);
        map_[k] = std::move(sp);
        failed_.erase(k);
        inflight_.erase(k);
    }

    void ensure(const MappedO3D& mapped) {
        if (!mapped.valid()) return;
        if (try_get(mapped.path())) return;
        std::vector<Vertex> v;
        prepare_o3d_mesh_vertices_for_gpu(mapped.mesh(), v);
        if (v.empty()) {
            const auto k = key_of(mapped.path());
            std::lock_guard<std::mutex> lk(mu_);
            failed_.insert(k);
            inflight_.erase(k);
            return;
        }
        store(mapped.path(), std::move(v));
    }

    void ensure_path(const std::filesystem::path& p) {
        if (p.empty()) return;
        const auto k = key_of(p);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (map_.count(k) || failed_.count(k)) return;
            if (!inflight_.insert(k).second) return;
        }
        auto m = load_mapped_shared(p);
        bool ok = false;
        if (m && m->valid()) {
            std::vector<Vertex> v;
            prepare_o3d_mesh_vertices_for_gpu(m->mesh(), v);
            if (!v.empty()) {
                store(p, std::move(v));
                ok = true;
            }
        }
        if (!ok) {
            std::lock_guard<std::mutex> lk(mu_);
            inflight_.erase(k);
            failed_.insert(k);
        }
    }

    // files that failed may exist now (co-op: received from the host into RAM)
    void clear_failed() {
        std::lock_guard<std::mutex> lk(mu_);
        failed_.clear();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<std::vector<Vertex>>> map_;
    std::unordered_set<std::string> failed_;
    std::unordered_set<std::string> inflight_;
};

inline PreparedVertexCache& prepared_vertex_cache() { return PreparedVertexCache::instance(); }

} // namespace omsi::o3d
