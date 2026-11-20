#pragma once

#include "omsi/map_types.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace omsi::editor {

struct EditorTransformSoA {
    std::vector<std::int32_t> id;
    std::vector<float> x, y, z, yaw;
    std::unordered_map<std::int32_t, std::uint32_t> id_to_i;

    void clear() {
        id.clear();
        x.clear();
        y.clear();
        z.clear();
        yaw.clear();
        id_to_i.clear();
    }

    void rebuild_from(const std::vector<map::MapObjectInst>& objs) {
        clear();
        const auto n = objs.size();
        id.resize(n);
        x.resize(n);
        y.resize(n);
        z.resize(n);
        yaw.resize(n);
        id_to_i.reserve(n * 2);
        for (std::size_t i = 0; i < n; ++i) {
            id[i] = objs[i].id;
            x[i] = objs[i].position.x;
            y[i] = objs[i].position.y;
            z[i] = objs[i].position.z;
            yaw[i] = objs[i].rotation_deg.z;
            id_to_i[objs[i].id] = static_cast<std::uint32_t>(i);
        }
    }

    void apply_delta(std::span<const std::int32_t> ids, float dx, float dy) {
        for (const std::int32_t oid : ids) {
            const auto it = id_to_i.find(oid);
            if (it == id_to_i.end()) continue;
            const auto i = it->second;
            x[i] += dx;
            y[i] += dy;
        }
    }

    void write_back(std::vector<map::MapObjectInst>& objs) const {
        for (auto& o : objs) {
            const auto it = id_to_i.find(o.id);
            if (it == id_to_i.end()) continue;
            const auto i = it->second;
            o.position.x = x[i];
            o.position.y = y[i];
            o.position.z = z[i];
            o.rotation_deg.z = yaw[i];
        }
    }
};

} // namespace omsi::editor
