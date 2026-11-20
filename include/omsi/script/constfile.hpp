#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace omsi::script {

struct ConstCurvePoint {
    float x = 0.f;
    float y = 0.f;
};

struct ConstCurve {
    std::string name;
    std::vector<ConstCurvePoint> points;
};

struct ConstFileDocument {
    std::filesystem::path path;
    std::unordered_map<std::string, float> constants;
    std::unordered_map<std::string, ConstCurve> curves;
};

[[nodiscard]] std::optional<ConstFileDocument> load_constfile(const std::filesystem::path& path);
[[nodiscard]] std::vector<ConstFileDocument> load_constfiles(const std::filesystem::path& bus_dir,
                                                               const std::vector<std::filesystem::path>& rel_paths);

} // namespace omsi::script
