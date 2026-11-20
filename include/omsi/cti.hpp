#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace omsi::cti {

// One [item] block in a .cti livery pack (Unit87 Colorscheme).
struct CtiItem {
    std::string label;
    std::string texture_var;  // e.g. farbschema_tex1
    std::string texture_file; // e.g. ASEAG_2.tga
};

struct CtiDocument {
    std::filesystem::path path;
    std::string title;
    std::vector<CtiItem> items;
    std::unordered_map<std::string, float> setvars; // [setvar] name → float
};

[[nodiscard]] std::optional<CtiDocument> load_cti(const std::filesystem::path& path);

} // namespace omsi::cti
