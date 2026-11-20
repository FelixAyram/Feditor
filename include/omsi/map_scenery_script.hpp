#pragma once

#include "omsi/map_types.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/script/osc_vm.hpp"
#include "omsi/script/var_registry.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace omsi::map_script {

// OMSI TFileObject.myvarparent_IDCode: child looks up parent crossing / script owner.
[[nodiscard]] std::int32_t resolve_var_owner(std::int32_t object_id,
                                               const std::unordered_map<std::int32_t, std::int32_t>& varparent_by_id);

class MapSceneryScriptHost {
public:
    void reset();
    // Incremental: add VMs for new IDs, drop gone IDs, do not re-init survivors.
    void sync(const std::vector<map::MapObjectInst>& objects, const std::filesystem::path& omsi_root);
    void refresh_traffic_indices(const std::vector<map::MapObjectInst>& objects);
    void apply_map_strings(const map::MapObjectInst& obj, const std::vector<std::string>& string_var_names);

    void tick(float dt_sec);
    // Current phase of light `light_index` in the [trafficlights] group of object `object_id`
    // (OMSI phases: 0..5 red / red-yellow / yellow, 6..8 green), -1 when the object has no group.
    [[nodiscard]] int traffic_light_phase(std::int32_t object_id, int light_index) const;
    // OMSI-like: only advance scripts near the player/camera (skip far tiles).
    void tick_near(float dt_sec, float cam_x, float cam_y, float max_dist_m,
                   const std::vector<map::MapObjectInst>& objects);

    // Editor: speed up traffic light cycles (e.g. 5x for preview).
    void set_traffic_time_scale(float scale) { traffic_time_scale_ = (scale > 0.f) ? scale : 1.f; }
    [[nodiscard]] float traffic_time_scale() const { return traffic_time_scale_; }

    [[nodiscard]] bool mesh_visible(const map::MapObjectInst& obj, int sco_mesh_index,
                                    const sco::SceneryObject& sco) const;

    [[nodiscard]] std::int32_t var_owner_of(std::int32_t object_id) const;
    // diagnostics: 0 no instance, 1 instance without a compiled script, 2 running script
    [[nodiscard]] int script_state(std::int32_t object_id) const {
        const auto it = scripts_.find(object_id);
        return it == scripts_.end() ? 0 : (it->second.loaded ? 2 : 1);
    }
    [[nodiscard]] float float_var(std::int32_t object_id, std::string_view name) const;
    [[nodiscard]] std::string string_var(std::int32_t object_id, std::string_view name) const;

private:
    struct TrafficController {
        std::int32_t object_id = 0;
        sco::ScoTrafficLightsGroup group{};
        float elapsed = 0.f;
        bool started = false; // clock seeded from the time of day on the first advance
    };

    struct ScriptInstance {
        std::int32_t object_id = 0;
        std::int32_t var_owner_id = 0;
        int traffic_light_index = 0;
        script::VarRegistry vars;
        std::vector<float> floats;
        std::vector<std::string> strings;
        script::OscVM vm;
        bool loaded = false;
    };

    std::unordered_map<std::int32_t, std::int32_t> varparent_by_id_;
    std::unordered_map<std::int32_t, TrafficController> controllers_;
    std::unordered_map<std::int32_t, ScriptInstance> scripts_;
    // Immutable global scenery varlists are shared as a template during sync. The
    // old code reopened/reparsed the same OMSI-wide lists for every scripted object.
    script::VarRegistry scenery_vars_template_;
    std::filesystem::path scenery_vars_root_;
    bool scenery_vars_template_ready_ = false;
    // Compiled .osc per SCO path (+ its merged var layout): new instances copy it instead of re-reading and
    // re-parsing the same script files (a stream diff adding 600+ objects parsed them 600+ times on the UI thread).
    std::unordered_map<std::string, script::OscVM> vm_template_by_sco_;
    std::unordered_map<std::string, bool> vm_template_ok_;
    float traffic_time_scale_ = 1.f;

    [[nodiscard]] const ScriptInstance* instance(std::int32_t object_id) const;
    [[nodiscard]] float inst_var(const ScriptInstance& inst, std::string_view name) const;
    void set_inst_var(ScriptInstance& inst, std::string_view name, float value) const;
    void advance_controller(TrafficController& ctl, float dt_sec);
    [[nodiscard]] int phase_at(const sco::ScoTrafficLight& tl, float elapsed, float cycle) const;
};

} // namespace omsi::map_script
