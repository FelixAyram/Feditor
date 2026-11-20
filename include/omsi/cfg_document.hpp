#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace omsi::cfg {

// Single UTF-8 buffer + line views (no per-line string allocations).
struct CfgFileBuffer {
    std::string text;
    std::vector<std::string_view> lines;
};

// OMSI bracket-section config ([section] + lines). Matches TMap.loadGlobalFile parser style.
using CfgSection = std::vector<std::vector<std::string>>;
using CfgDocument = std::unordered_map<std::string, CfgSection>;

[[nodiscard]] std::optional<CfgDocument> load_cfg_file(const std::filesystem::path& path);
[[nodiscard]] std::vector<std::string> read_cfg_lines(const std::filesystem::path& path);
[[nodiscard]] CfgFileBuffer read_cfg_file_buffer(const std::filesystem::path& path);
// FNV-1a over raw file bytes (for .slib validation).
[[nodiscard]] std::uint64_t fnv1a_file_bytes(const std::filesystem::path& path);
[[nodiscard]] inline const CfgSection& cfg_section(const CfgDocument& doc, const std::string& key) {
    return doc.at(key);
}

} // namespace omsi::cfg
