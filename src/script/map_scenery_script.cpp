#include "omsi/utf8_win32.hpp"
#include "omsi/map_scenery_script.hpp"

#include "omsi/logger.hpp"
#include "omsi/sco_types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace omsi::map_script {
namespace {

int parse_traffic_index(const std::vector<std::string>& strings) {
    if (strings.empty() || strings.front().empty()) return 0;
    try {
        const int v = std::stoi(strings.front());
        return v < 0 ? 0 : v;
    } catch (...) {
        return 0;
    }
}

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Train/barrier lights are demand-driven in OMSI (idle until a train). Looping them
// starts on phase 0 = "crossing active" → Schranke closes. Street lights still cycle.
bool is_demand_traffic_light(std::string_view name) {
    const auto n = lower_copy(std::string(name));
    return n.find("rail") != std::string::npos || n.find("bahn") != std::string::npos ||
           n.find("zug") != std::string::npos || n.find("bue") != std::string::npos ||
           n.find("schranke") != std::string::npos;
}

int idle_traffic_phase(const sco::ScoTrafficLight& tl) {
    for (const auto& ph : tl.phases) {
        if (ph.phase < 0 || ph.phase > 2) return ph.phase;
    }
    return 6;
}

// Only name-based demand (rail/barrier). Street Ampel phases often start at 0..2 —
// treating that as demand froze the whole Kreuz group on idle (stuck red/yellow).
bool group_has_demand(const sco::ScoTrafficLightsGroup& group) {
    for (const auto& tl : group.lights) {
        if (is_demand_traffic_light(tl.name)) return true;
    }
    return false;
}

int resolve_light_index(const std::vector<std::string>& strings, const std::vector<sco::ScoTrafficLight>& lights) {
    if (lights.empty()) return 0;
    if (!strings.empty() && !strings.front().empty()) {
        const auto key = lower_copy(strings.front());
        for (std::size_t i = 0; i < lights.size(); ++i) {
            if (lower_copy(lights[i].name) == key) return static_cast<int>(i);
        }
    }
    int idx = parse_traffic_index(strings);
    if (idx >= static_cast<int>(lights.size())) idx = static_cast<int>(lights.size()) - 1;
    return idx;
}

} // namespace

std::int32_t resolve_var_owner(std::int32_t object_id,
                               const std::unordered_map<std::int32_t, std::int32_t>& varparent_by_id) {
    std::int32_t id = object_id;
    for (int guard = 0; guard < 64; ++guard) {
        const auto it = varparent_by_id.find(id);
        if (it == varparent_by_id.end() || it->second == 0) return id;
        id = it->second;
    }
    return id;
}

void MapSceneryScriptHost::reset() {
    varparent_by_id_.clear();
    controllers_.clear();
    scripts_.clear();
    // Keep the immutable varlist template: reopening the same OMSI-wide files on
    // the next map would add startup/stream hitches without changing their content.
}

const MapSceneryScriptHost::ScriptInstance* MapSceneryScriptHost::instance(std::int32_t object_id) const {
    const auto it = scripts_.find(object_id);
    return it == scripts_.end() ? nullptr : &it->second;
}

float MapSceneryScriptHost::inst_var(const ScriptInstance& inst, std::string_view name) const {
    if (const auto idx = inst.vars.float_index(std::string(name))) {
        if (static_cast<std::size_t>(*idx) < inst.floats.size())
            return inst.floats.at(static_cast<std::size_t>(*idx));
    }
    return 0.f;
}

void MapSceneryScriptHost::set_inst_var(ScriptInstance& inst, std::string_view name, float value) const {
    if (const auto idx = inst.vars.float_index(std::string(name))) {
        if (inst.floats.size() < inst.vars.float_var_count())
            inst.floats.resize(inst.vars.float_var_count(), 0.f);
        inst.floats.at(static_cast<std::size_t>(*idx)) = value;
    }
}

int MapSceneryScriptHost::phase_at(const sco::ScoTrafficLight& tl, float elapsed, float cycle) const {
    if (tl.phases.empty()) return 0;
    float listed = 0.f;
    for (const auto& ph : tl.phases)
        if (ph.duration > 1e-6f) listed += ph.duration;
    float wrap = cycle > 0.01f ? cycle : listed;
    if (wrap <= 0.01f) wrap = 60.f;
    float t = std::fmod(elapsed, wrap);
    if (t < 0.f) t += wrap;
    float acc = 0.f;
    for (const auto& ph : tl.phases) {
        if (ph.duration <= 1e-6f) continue;
        acc += ph.duration;
        if (t < acc) return ph.phase;
    }
    // openOMSI state_at: the rest of the group cycle keeps the last phase (a last phase of 0 s
    // lasts until the cycle starts again).
    return tl.phases.back().phase;
}

int MapSceneryScriptHost::traffic_light_phase(std::int32_t object_id, int light_index) const {
    const auto it = controllers_.find(object_id);
    if (it == controllers_.end() || it->second.group.lights.empty()) return -1;
    const auto& lights = it->second.group.lights;
    const int idx = (std::max)(0, (std::min)(light_index, static_cast<int>(lights.size()) - 1));
    const auto& tl = lights[static_cast<std::size_t>(idx)];
    const bool demand = is_demand_traffic_light(tl.name) || group_has_demand(it->second.group);
    return demand ? idle_traffic_phase(tl) : phase_at(tl, it->second.elapsed, it->second.group.cycle);
}

void MapSceneryScriptHost::advance_controller(TrafficController& ctl, float dt_sec) {
    float wrap = ctl.group.cycle;
    if (wrap <= 0.01f && !ctl.group.lights.empty()) {
        wrap = 0.f;
        for (const auto& ph : ctl.group.lights.front().phases) wrap += ph.duration;
    }
    if (wrap <= 0.01f) wrap = 60.f;
    if (!ctl.started) {
        // openOMSI TrafficLightController::start: the clock starts at the time of day, so
        // crossings with the same cycle run in step, as a coordinated street would.
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        const float day_s = static_cast<float>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
        ctl.elapsed = std::fmod(day_s, wrap);
        ctl.started = true;
    }
    ctl.elapsed += dt_sec * traffic_time_scale_;
    while (ctl.elapsed >= wrap) ctl.elapsed -= wrap;
}

void MapSceneryScriptHost::apply_map_strings(const map::MapObjectInst& obj,
                                             const std::vector<std::string>& string_var_names) {
    auto it = scripts_.find(obj.id);
    if (it == scripts_.end() || !it->second.loaded) return;
    auto& inst = it->second;
    if (inst.strings.size() < inst.vars.string_var_count())
        inst.strings.resize(inst.vars.string_var_count());
    for (std::size_t i = 0; i < string_var_names.size() && i < obj.strings.size(); ++i) {
        if (const auto idx = inst.vars.string_index(string_var_names[i])) {
            if (static_cast<std::size_t>(*idx) < inst.strings.size())
                inst.strings[static_cast<std::size_t>(*idx)] = obj.strings[i];
        }
    }
}

void MapSceneryScriptHost::sync(const std::vector<map::MapObjectInst>& objects,
                                const std::filesystem::path& omsi_root) {
    script::load_known_lists(omsi_root); // (M.V.) callback names from program/callbacklist_*.txt
    if (!scenery_vars_template_ready_ || scenery_vars_root_ != omsi_root) {
        scenery_vars_template_ = script::VarRegistry{};
        (void)scenery_vars_template_.load_scenery_lists(omsi_root);
        scenery_vars_root_ = omsi_root;
        scenery_vars_template_ready_ = true;
    }
    // Most maps contain many instances of the same SCO. Cache the merged variable
    // layout once per SCO path for this sync; each instance still receives its own
    // mutable VarRegistry/float/string storage and VM state.
    std::unordered_map<std::string, script::VarRegistry> vars_by_sco;
    vars_by_sco.reserve((std::min)(objects.size(), static_cast<std::size_t>(2048)));
    // Keyed by the native path string: lexically_normal + UTF-8 per object (3 lookups for each of
    // ~50k objects) made a sync on every streamed tile cost 30-160 ms on the main thread.
    std::unordered_map<std::wstring, const sco::SceneryObject*> sco_by_path;
    sco_by_path.reserve((std::min)(objects.size(), static_cast<std::size_t>(4096)));
    auto load_sync_sco = [&](const std::filesystem::path& path) -> const sco::SceneryObject* {
        if (const auto it = sco_by_path.find(path.native()); it != sco_by_path.end()) return it->second;
        const auto* loaded = sco::load_sco(path);
        sco_by_path.emplace(path.native(), loaded);
        return loaded;
    };
    std::unordered_set<std::int32_t> live;
    live.reserve(objects.size() * 2);
    varparent_by_id_.clear();
    for (const auto& o : objects) {
        live.insert(o.id);
        if (o.varparent_id != 0) varparent_by_id_[o.id] = o.varparent_id;
    }

    for (auto it = controllers_.begin(); it != controllers_.end();) {
        if (live.count(it->first)) ++it;
        else it = controllers_.erase(it);
    }
    for (auto it = scripts_.begin(); it != scripts_.end();) {
        if (live.count(it->first)) ++it;
        else it = scripts_.erase(it);
    }

    int vp_logged = 0;
    static std::size_t s_last_vp_count = static_cast<std::size_t>(-1);
    const bool vp_changed = varparent_by_id_.size() != s_last_vp_count;
    s_last_vp_count = varparent_by_id_.size();
    for (const auto& o : objects) {
        if (!vp_changed) break;
        if (o.varparent_id == 0) continue;
        if (vp_logged < 24) {
            omsi::log::info("varparent id=" + std::to_string(o.id) + " -> " + std::to_string(o.varparent_id) +
                            " sco=" + omsi::path_text(o.sco_path.filename()));
            ++vp_logged;
        }
    }
    if (vp_logged > 0)
        omsi::log::info("varparent count=" + std::to_string(varparent_by_id_.size()) + " (logged " +
                        std::to_string(vp_logged) + ")");

    for (const auto& o : objects) {
        if (controllers_.count(o.id)) continue;
        const auto sco = load_sync_sco(o.sco_path);
        if (!sco || !sco->traffic_lights_group) continue;
        TrafficController ctl{};
        ctl.object_id = o.id;
        ctl.group = *sco->traffic_lights_group;
        controllers_[o.id] = std::move(ctl);
        omsi::log::info("traffic_lights_group id=" + std::to_string(o.id) + " lights=" +
                        std::to_string(sco->traffic_lights_group->lights.size()) +
                        " cycle=" + std::to_string(sco->traffic_lights_group->cycle) + " sco=" +
                        omsi::path_text(o.sco_path.filename()));
    }

    std::unordered_set<std::int32_t> varparent_targets;
    for (const auto& o : objects)
        if (o.varparent_id != 0) varparent_targets.insert(o.varparent_id);

    std::vector<std::int32_t> added;
    for (const auto& o : objects) {
        if (scripts_.count(o.id)) {
            // Most surviving objects have no map string overrides. Avoid repeating
            // SCO-cache lookups for them on every streaming diff; non-empty overrides
            // still refresh exactly as before.
            if (!o.strings.empty()) {
                if (const auto sco = load_sync_sco(o.sco_path)) apply_map_strings(o, sco->string_var_names);
            }
            continue;
        }
        const auto sco = load_sync_sco(o.sco_path);
        if (!sco) continue;
        const bool need = !sco->script_files.empty() || sco->traffic_lights_group.has_value() ||
                          varparent_targets.count(o.id) != 0 || o.varparent_id != 0;
        if (!need) continue;
        const std::int32_t owner_id = resolve_var_owner(o.id, varparent_by_id_);

        ScriptInstance inst{};
        inst.object_id = o.id;
        inst.var_owner_id = owner_id;
        inst.traffic_light_index = parse_traffic_index(o.strings);
        if (const auto ctl_it = controllers_.find(owner_id); ctl_it != controllers_.end())
            inst.traffic_light_index = resolve_light_index(o.strings, ctl_it->second.group.lights);
        const auto sco_dir = o.sco_path.parent_path();
        const auto vars_key = omsi::path_text(o.sco_path.lexically_normal());
        auto vars_it = vars_by_sco.find(vars_key);
        if (vars_it == vars_by_sco.end()) {
            script::VarRegistry merged = scenery_vars_template_;
            (void)merged.merge_bus_lists(sco_dir, sco->varnamelist_files, sco->stringvarnamelist_files);
            merged.ensure_float("TrafficLightPhase");
            merged.ensure_float("TrafficLightApproach");
            vars_it = vars_by_sco.emplace(vars_key, std::move(merged)).first;
        }
        inst.vars = vars_it->second;
        inst.floats.assign(inst.vars.float_var_count(), 0.f);
        inst.strings.assign(inst.vars.string_var_count(), std::string{});
        bool compiled_now = false;
        if (!sco->script_files.empty()) {
            if (const auto ok_it = vm_template_ok_.find(vars_key); ok_it != vm_template_ok_.end()) {
                if (ok_it->second) {
                    inst.vm = vm_template_by_sco_.at(vars_key); // copy of the compiled program
                    inst.loaded = true;
                }
            } else if (!inst.vm.load(sco_dir, sco->script_files, inst.vars)) {
                vm_template_ok_.emplace(vars_key, false);
                omsi::log::warn("sco script load failed id=" + std::to_string(o.id) + " sco=" +
                                omsi::path_text(o.sco_path.filename()));
            } else {
                vm_template_by_sco_.emplace(vars_key, inst.vm);
                vm_template_ok_.emplace(vars_key, true);
                inst.loaded = true;
                compiled_now = true;
            }
        }
        // One line per SCO type (first instance), not per instance: 600+ log lines per diff.
        if (compiled_now)
        omsi::log::info("sco script id=" + std::to_string(o.id) + " var_owner=" + std::to_string(owner_id) +
                        " loaded=" + std::to_string(inst.loaded ? 1 : 0) +
                        " tl_index=" + std::to_string(inst.traffic_light_index) +
                        (o.varparent_id != 0 ? " varparent=" + std::to_string(o.varparent_id) : "") + " sco=" +
                        omsi::path_text(o.sco_path.filename()));
        scripts_[o.id] = std::move(inst);
        apply_map_strings(o, sco->string_var_names);
        added.push_back(o.id);
    }

    // OMSI myvarparent: child (S.L.Red) writes into the parent's var block.
    std::vector<std::int32_t> rebind_init;
    for (const auto& o : objects) {
        auto child_it = scripts_.find(o.id);
        if (child_it == scripts_.end()) continue;
        const auto owner_id = resolve_var_owner(o.id, varparent_by_id_);
        const bool owner_changed = child_it->second.var_owner_id != owner_id;
        child_it->second.var_owner_id = owner_id;
        if (o.varparent_id == 0) continue;
        auto owner_it = scripts_.find(owner_id);
        if (owner_it == scripts_.end() || owner_id == o.id) continue;
        const auto sco = load_sync_sco(o.sco_path);
        if (!sco) continue;
        owner_it->second.vars.merge_bus_lists(o.sco_path.parent_path(), sco->varnamelist_files,
                                              sco->stringvarnamelist_files);
        owner_it->second.vars.ensure_float("TrafficLightPhase");
        owner_it->second.vars.ensure_float("TrafficLightApproach");
        owner_it->second.floats.resize(owner_it->second.vars.float_var_count(), 0.f);
        owner_it->second.strings.resize(owner_it->second.vars.string_var_count());
        if (owner_changed) rebind_init.push_back(o.id);
    }

    for (auto& [_, inst] : scripts_) {
        if (!inst.loaded) continue;
        // openOMSI / Omsi.exe RefreshAmpelParenting: every object runs its script on its OWN
        // variables; the [varparent] crossing only hands it TrafficLightPhase/Approach. Binding
        // the lamps' VMs to the crossing's store made them overwrite each other's lamp vars
        // (the [visible] ones stayed 0 on the child): lights changed rarely and out of order.
        ScriptInstance* store = &inst;
        inst.vm.bind_vars(&store->floats, &store->vars);
        inst.vm.bind_strings(&store->strings);
    }
    for (const std::int32_t id : added) {
        auto it = scripts_.find(id);
        if (it == scripts_.end() || !it->second.loaded) continue;
        it->second.vm.run_init();
    }
    for (const std::int32_t id : rebind_init) {
        if (std::find(added.begin(), added.end(), id) != added.end()) continue;
        auto it = scripts_.find(id);
        if (it == scripts_.end() || !it->second.loaded) continue;
        it->second.vm.run_init();
    }

    int tl_logged = 0;
    for (const std::int32_t id : added) {
        auto it = scripts_.find(id);
        if (it == scripts_.end()) continue;
        auto& inst = it->second;
        const auto ctl_it = controllers_.find(inst.var_owner_id);
        const TrafficController* ctl = ctl_it != controllers_.end() ? &ctl_it->second : nullptr;
        int phase = 6;
        std::string light_name = "(none)";
        bool demand = false;
        if (ctl && !ctl->group.lights.empty()) {
            const auto& lights = ctl->group.lights;
            const int idx = (std::min)(inst.traffic_light_index, static_cast<int>(lights.size()) - 1);
            light_name = lights[static_cast<std::size_t>(idx)].name;
            demand = is_demand_traffic_light(light_name) || group_has_demand(ctl->group);
            phase = idle_traffic_phase(lights[static_cast<std::size_t>(idx)]);
        }
        set_inst_var(inst, "TrafficLightPhase", static_cast<float>(phase));
        set_inst_var(inst, "TrafficLightApproach", 0.f);
        if (tl_logged < 12) {
            {

                static std::uint32_t s_tl_log = 0;

                if ((++s_tl_log % 600u) == 1u)

                    omsi::log::info("TrafficLightPhase id=" + std::to_string(inst.object_id) +
                            " owner=" + std::to_string(inst.var_owner_id) + " light[" +
                            std::to_string(inst.traffic_light_index) + "]=" + light_name +
                            " demand=" + (demand ? "1" : "0") + " phase=" + std::to_string(phase) +
                            " Schranke=" + std::to_string(inst_var(inst, "Schranke")));

            }
            ++tl_logged;
        }
    }
}

void MapSceneryScriptHost::refresh_traffic_indices(const std::vector<map::MapObjectInst>& objects) {
    for (const auto& o : objects) {
        const auto it = scripts_.find(o.id);
        if (it == scripts_.end()) continue;
        it->second.traffic_light_index = parse_traffic_index(o.strings);
        const auto ctl_it = controllers_.find(it->second.var_owner_id);
        if (ctl_it != controllers_.end())
            it->second.traffic_light_index =
                resolve_light_index(o.strings, ctl_it->second.group.lights);
    }
}

void MapSceneryScriptHost::tick(float dt_sec) {
    if (dt_sec <= 0.f) return;
    script::OscSystemState sys{};
    sys.timegap = dt_sec;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    sys.get_time = static_cast<float>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
    sys.time = sys.get_time; // (L.S.Time): seconds of the day - left at 0, the street clocks stood still
    sys.day = static_cast<float>(local.tm_mday);
    sys.month = static_cast<float>(local.tm_mon + 1);
    sys.year = static_cast<float>(local.tm_year + 1900);
    sys.day_of_year = static_cast<float>(local.tm_yday + 1);
    for (auto& [_, ctl] : controllers_) advance_controller(ctl, dt_sec);
    for (auto& [_, inst] : scripts_) {
        // openOMSI / Omsi.exe RefreshAmpelParenting: every object runs its script on its OWN
        // variables; the [varparent] crossing only hands it TrafficLightPhase/Approach. Binding
        // the lamps' VMs to the crossing's store made them overwrite each other's lamp vars
        // (the [visible] ones stayed 0 on the child): lights changed rarely and out of order.
        ScriptInstance* store = &inst;

        // Per-Ampel colors on the child. Parent store is shared by Main/Side — writing only
        // there made every light show the same colour (last writer wins).
        auto ensure_ryg = [&](ScriptInstance& s) {
            s.vars.ensure_float("TrafficLightPhase");
            s.vars.ensure_float("TrafficLightApproach");
            s.vars.ensure_float("Red");
            s.vars.ensure_float("Yellow");
            s.vars.ensure_float("Green");
            if (s.floats.size() < s.vars.float_var_count())
                s.floats.resize(s.vars.float_var_count(), 0.f);
        };
        ensure_ryg(inst);
        ensure_ryg(*store);
        const auto ctl_it = controllers_.find(inst.var_owner_id);
        if (ctl_it != controllers_.end() && !ctl_it->second.group.lights.empty()) {
            const auto& lights = ctl_it->second.group.lights;
            const int idx =
                (std::max)(0, (std::min)(inst.traffic_light_index, static_cast<int>(lights.size()) - 1));
            const auto& tl = lights[static_cast<std::size_t>(idx)];
            const bool demand = is_demand_traffic_light(tl.name) || group_has_demand(ctl_it->second.group);
            const int phase =
                demand ? idle_traffic_phase(tl) : phase_at(tl, ctl_it->second.elapsed, ctl_it->second.group.cycle);
            const int p = phase;
            const float r = (p >= 0 && p <= 5) ? 1.f : 0.f;
            const float y = ((p >= 3 && p <= 5) || (p >= 9 && p <= 11)) ? 1.f : 0.f;
            const float g = (p >= 6 && p <= 8) ? 1.f : 0.f;
            // Child: mesh [visible] / float_var
            set_inst_var(inst, "TrafficLightPhase", static_cast<float>(phase));
            set_inst_var(inst, "TrafficLightApproach", 0.f);
            set_inst_var(inst, "Red", r);
            set_inst_var(inst, "Yellow", y);
            set_inst_var(inst, "Green", g);
            // Parent: OSC (L.L.TrafficLightPhase) with myvarparent — set just before this frame
            set_inst_var(*store, "TrafficLightPhase", static_cast<float>(phase));
            set_inst_var(*store, "TrafficLightApproach", 0.f);
            set_inst_var(*store, "Red", r);
            set_inst_var(*store, "Yellow", y);
            set_inst_var(*store, "Green", g);
        }

        if (!inst.loaded) continue;
        inst.vm.bind_vars(&store->floats, &store->vars);
        inst.vm.bind_strings(&store->strings);
        inst.vm.set_system_state(sys);
        inst.vm.run_frame(dt_sec);
        // OSC may rewrite Red/Yellow/Green on the parent — copy back so this Ampel keeps its colours.
        if (store != &inst) {
            set_inst_var(inst, "Red", inst_var(*store, "Red"));
            set_inst_var(inst, "Yellow", inst_var(*store, "Yellow"));
            set_inst_var(inst, "Green", inst_var(*store, "Green"));
            set_inst_var(inst, "TrafficLightPhase", inst_var(*store, "TrafficLightPhase"));
        }
    }
}

bool MapSceneryScriptHost::mesh_visible(const map::MapObjectInst& obj, int sco_mesh_index,
                                        const sco::SceneryObject& sco) const {
    const sco::ScoVisibleLink* link = nullptr;
    for (const auto& v : sco.visible_links) {
        if (v.mesh_index == sco_mesh_index) {
            link = &v;
            break;
        }
    }
    if (!link) return true;

    auto vis_from = [&](std::int32_t id) -> std::optional<bool> {
        const auto* inst = instance(id);
        if (!inst) return std::nullopt;
        if (!inst->vars.float_index(link->var_name)) return std::nullopt;
        return inst_var(*inst, link->var_name) == link->value;
    };
    // Prefer this Ampel's own vars (per traffic_light_index). Parent is shared across Main/Side
    // — reading parent first made every light show the last written Red/Yellow/Green.
    if (auto v = vis_from(obj.id)) return *v;
    if (obj.varparent_id != 0) {
        const auto owner = var_owner_of(obj.id);
        if (auto v = vis_from(owner)) return *v;
        if (owner != obj.varparent_id)
            if (auto v = vis_from(obj.varparent_id)) return *v;
    }
    return link->value == 0.f;
}

std::int32_t MapSceneryScriptHost::var_owner_of(std::int32_t object_id) const {
    return resolve_var_owner(object_id, varparent_by_id_);
}

float MapSceneryScriptHost::float_var(std::int32_t object_id, std::string_view name) const {
    auto from = [&](std::int32_t id) -> std::optional<float> {
        const auto* inst = instance(id);
        if (!inst) return std::nullopt;
        if (!inst->vars.float_index(std::string(name))) return std::nullopt;
        return inst_var(*inst, name);
    };
    if (auto v = from(object_id)) return *v;
    const auto owner = var_owner_of(object_id);
    if (owner != object_id)
        if (auto v = from(owner)) return *v;
    return 0.f;
}

std::string MapSceneryScriptHost::string_var(std::int32_t object_id, std::string_view name) const {
    auto from = [&](std::int32_t id) -> std::optional<std::string> {
        const auto* inst = instance(id);
        if (!inst) return std::nullopt;
        const auto idx = inst->vars.string_index(std::string(name));
        if (!idx) return std::nullopt;
        if (static_cast<std::size_t>(*idx) >= inst->strings.size()) return std::string{};
        return inst->strings[static_cast<std::size_t>(*idx)];
    };
    if (auto v = from(object_id)) return *v;
    const auto owner = var_owner_of(object_id);
    if (owner != object_id)
        if (auto v = from(owner)) return *v;
    return {};
}


void MapSceneryScriptHost::tick_near(float dt_sec, float cam_x, float cam_y, float max_dist_m,
                                     const std::vector<map::MapObjectInst>& objects) {
    if (dt_sec <= 0.f) return;
    if (scripts_.empty() && controllers_.empty()) return;
    static thread_local std::unordered_set<std::int32_t> near_ids;
    // The near set only changes when the camera moves or the object list changes. Rebuilding it from every
    // object each frame cost several ms on big maps; reuse it until the camera moved >4 m (or ~0.25 s passed).
    static thread_local const void* s_host = nullptr;
    static thread_local const void* s_obj_data = nullptr;
    static thread_local std::size_t s_obj_n = 0;
    static thread_local float s_cx = 0.f, s_cy = 0.f, s_max = -1.f;
    static thread_local int s_age = 1 << 30;
    const float moved2 = (cam_x - s_cx) * (cam_x - s_cx) + (cam_y - s_cy) * (cam_y - s_cy);
    const bool reuse = s_host == this && s_obj_data == static_cast<const void*>(objects.data()) &&
                       s_obj_n == objects.size() && s_max == max_dist_m && moved2 < 16.f && s_age < 15;
    if (reuse) {
        ++s_age;
    } else {
        s_host = this;
        s_obj_data = objects.data();
        s_obj_n = objects.size();
        s_cx = cam_x;
        s_cy = cam_y;
        s_max = max_dist_m;
        s_age = 0;
        near_ids.clear();
        if (near_ids.bucket_count() < objects.size() * 2 + 1)
            near_ids.reserve(objects.size() * 2 + 1);
        const float max2 = max_dist_m > 1.f ? max_dist_m * max_dist_m : 900.f * 900.f;
        for (const auto& o : objects) {
            const float dx = o.position.x - cam_x;
            const float dy = o.position.y - cam_y;
            if (dx * dx + dy * dy <= max2) {
                near_ids.insert(o.id);
                if (o.varparent_id != 0) near_ids.insert(o.varparent_id);
            }
        }
    }
    // Always advance all controllers (cycle clocks); only run OSC for near instances.
    for (auto& [_, ctl] : controllers_) advance_controller(ctl, dt_sec);
    if (std::getenv("OMSI_TRACE_TL_CLOCK")) {
        static double sim_sum = 0.0;
        static auto wall0 = std::chrono::steady_clock::now();
        static int calls = 0;
        sim_sum += dt_sec * traffic_time_scale_;
        ++calls;
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        if (wall > 5.0) {
            omsi::log::info("TL clock: " + std::to_string(sim_sum) + " s of light time in " + std::to_string(wall) +
                            " s wall, " + std::to_string(calls) + " ticks, scale " + std::to_string(traffic_time_scale_) +
                            ", " + std::to_string(controllers_.size()) + " controllers");
            sim_sum = 0.0;
            calls = 0;
            wall0 = std::chrono::steady_clock::now();
        }
    }

    script::OscSystemState sys{};
    sys.timegap = dt_sec;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    sys.get_time = static_cast<float>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
    sys.time = sys.get_time; // (L.S.Time): seconds of the day - left at 0, the street clocks stood still
    sys.day = static_cast<float>(local.tm_mday);
    sys.month = static_cast<float>(local.tm_mon + 1);
    sys.year = static_cast<float>(local.tm_year + 1900);
    sys.day_of_year = static_cast<float>(local.tm_yday + 1);

    for (auto& [id, inst] : scripts_) {
        if (!near_ids.count(id) && !near_ids.count(inst.var_owner_id)) continue;
        // openOMSI / Omsi.exe RefreshAmpelParenting: every object runs its script on its OWN
        // variables; the [varparent] crossing only hands it TrafficLightPhase/Approach. Binding
        // the lamps' VMs to the crossing's store made them overwrite each other's lamp vars
        // (the [visible] ones stayed 0 on the child): lights changed rarely and out of order.
        ScriptInstance* store = &inst;

        // Per-Ampel colors on the child. Parent store is shared by Main/Side — writing only
        // there made every light show the same colour (last writer wins).
        auto ensure_ryg = [&](ScriptInstance& s) {
            s.vars.ensure_float("TrafficLightPhase");
            s.vars.ensure_float("TrafficLightApproach");
            s.vars.ensure_float("Red");
            s.vars.ensure_float("Yellow");
            s.vars.ensure_float("Green");
            if (s.floats.size() < s.vars.float_var_count())
                s.floats.resize(s.vars.float_var_count(), 0.f);
        };
        ensure_ryg(inst);
        ensure_ryg(*store);
        const auto ctl_it = controllers_.find(inst.var_owner_id);
        if (ctl_it != controllers_.end() && !ctl_it->second.group.lights.empty()) {
            const auto& lights = ctl_it->second.group.lights;
            const int idx =
                (std::max)(0, (std::min)(inst.traffic_light_index, static_cast<int>(lights.size()) - 1));
            const auto& tl = lights[static_cast<std::size_t>(idx)];
            const bool demand = is_demand_traffic_light(tl.name) || group_has_demand(ctl_it->second.group);
            const int phase =
                demand ? idle_traffic_phase(tl) : phase_at(tl, ctl_it->second.elapsed, ctl_it->second.group.cycle);
            const int p = phase;
            const float r = (p >= 0 && p <= 5) ? 1.f : 0.f;
            const float y = ((p >= 3 && p <= 5) || (p >= 9 && p <= 11)) ? 1.f : 0.f;
            const float g = (p >= 6 && p <= 8) ? 1.f : 0.f;
            // Child: mesh [visible] / float_var
            set_inst_var(inst, "TrafficLightPhase", static_cast<float>(phase));
            set_inst_var(inst, "TrafficLightApproach", 0.f);
            set_inst_var(inst, "Red", r);
            set_inst_var(inst, "Yellow", y);
            set_inst_var(inst, "Green", g);
            // Parent: OSC (L.L.TrafficLightPhase) with myvarparent — set just before this frame
            set_inst_var(*store, "TrafficLightPhase", static_cast<float>(phase));
            set_inst_var(*store, "TrafficLightApproach", 0.f);
            set_inst_var(*store, "Red", r);
            set_inst_var(*store, "Yellow", y);
            set_inst_var(*store, "Green", g);
        }

        if (!inst.loaded) continue;
        inst.vm.bind_vars(&store->floats, &store->vars);
        inst.vm.bind_strings(&store->strings);
        inst.vm.set_system_state(sys);
        inst.vm.run_frame(dt_sec);
        if (store != &inst) {
            set_inst_var(inst, "Red", inst_var(*store, "Red"));
            set_inst_var(inst, "Yellow", inst_var(*store, "Yellow"));
            set_inst_var(inst, "Green", inst_var(*store, "Green"));
            set_inst_var(inst, "TrafficLightPhase", inst_var(*store, "TrafficLightPhase"));
        }
    }
}

} // namespace omsi::map_script
