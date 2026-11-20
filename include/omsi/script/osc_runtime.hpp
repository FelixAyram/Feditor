#pragma once

#include "omsi/script/osc_vm.hpp"
#include "omsi/script/var_registry.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace omsi::script {

struct OscCompileStats {
    std::filesystem::path path;
    int macro_count = 0;
    int frame_macro_count = 0;
    int init_macro_count = 0;
    int token_count = 0;
    int unknown_tokens = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

struct OscScriptBundle {
    std::vector<OscCompileStats> scripts;
    int total_errors = 0;
    int total_warnings = 0;
};

// Validate .osc scripts (ref scan). Live execution is OscVM (compile-once IR).
[[nodiscard]] OscScriptBundle load_osc_scripts(const std::filesystem::path& bus_dir,
                                                 const std::vector<std::filesystem::path>& script_files,
                                                 const VarRegistry& vars);

} // namespace omsi::script
