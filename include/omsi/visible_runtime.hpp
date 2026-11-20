#pragma once

#include "omsi/model_cfg_types.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace omsi::visible {

// OMSI [visible]: mesh shown when script_var == visible_default.
// [mouseevent]: click triggers script; viewer cycles visible_var through cfg values.
class VisibleRuntime {
public:
    void init_from_cfg(const model_cfg::ModelCfgDocument& cfg);
    [[nodiscard]] bool mesh_visible(const model_cfg::ModelCfgMeshEntry& mesh) const;
    bool trigger_mouseevent(const model_cfg::ModelCfgDocument& cfg, const std::string& event_name);
    void set_var(const std::string& name, int value);
    [[nodiscard]] int var(const std::string& name) const;

private:
    std::unordered_map<std::string, int> vars_;
};

} // namespace omsi::visible
