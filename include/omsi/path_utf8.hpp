#pragma once

#include <filesystem>
#include <string>

namespace omsi {

// UTF-8 text of a path. std::filesystem::path::string() converts through the ANSI code page
// on Windows and throws for characters it cannot map (OMSI content has Cyrillic, umlaut and
// CJK file names), so loaders use this for logs, cache keys and extension checks.
[[nodiscard]] inline std::string path_utf8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace omsi
