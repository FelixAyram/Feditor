#include "omsi/visible_runtime.hpp"

#include "omsi/logger.hpp"
#include "omsi/model_cfg.hpp"

#include <algorithm>
#include <set>

namespace omsi::visible {

void VisibleRuntime::init_from_cfg(const model_cfg::ModelCfgDocument& cfg) {
    vars_.clear();
    const auto* lod = model_cfg::primary_lod(cfg);
    if (!lod) return;

    // OMSI: mesh visible iff script_var == visible_default. Vars start at 0 ({init} / OSC sets them).
    for (const auto& mesh : lod->meshes) {
        if (!mesh.visible_var) continue;
        vars_.try_emplace(*mesh.visible_var, 0);
    }

    for (const auto& [name, value] : vars_)
        omsi::log::info("visible init: " + name + "=" + std::to_string(value));
}

bool VisibleRuntime::mesh_visible(const model_cfg::ModelCfgMeshEntry& mesh) const {
    if (!mesh.visible_var) return true;
    const auto it = vars_.find(*mesh.visible_var);
    if (it == vars_.end()) return mesh.visible_default != 0;
    return it->second == mesh.visible_default;
}

bool VisibleRuntime::trigger_mouseevent(const model_cfg::ModelCfgDocument& cfg,
                                         const std::string& event_name) {
    const auto* lod = model_cfg::primary_lod(cfg);
    if (!lod) return false;

    std::optional<std::string> visible_var;
    std::set<int> values;
    for (const auto& mesh : lod->meshes) {
        if (!mesh.mouseevent_var || *mesh.mouseevent_var != event_name) continue;
        if (mesh.visible_var) {
            visible_var = mesh.visible_var;
            values.insert(mesh.visible_default);
        }
    }
    if (!visible_var) {
        for (const auto& mesh : lod->meshes) {
            if (!mesh.visible_var || *mesh.visible_var != event_name) continue;
            visible_var = mesh.visible_var;
            values.insert(mesh.visible_default);
        }
    }
    if (!visible_var || values.empty()) {
        omsi::log::warn("mouseevent without visible group: " + event_name);
        return false;
    }

    const int current = var(*visible_var);
    auto it = values.upper_bound(current);
    if (it == values.end()) it = values.begin();
    const int next = *it;
    set_var(*visible_var, next);
    omsi::log::info("mouseevent " + event_name + " -> " + *visible_var + "=" + std::to_string(next));
    return true;
}

void VisibleRuntime::set_var(const std::string& name, int value) { vars_[name] = value; }

int VisibleRuntime::var(const std::string& name) const {
    const auto it = vars_.find(name);
    return it == vars_.end() ? 0 : it->second;
}

} // namespace omsi::visible
