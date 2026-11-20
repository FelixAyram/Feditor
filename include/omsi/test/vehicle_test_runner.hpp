#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace omsi::test {

struct VehicleTestResult {
    std::string name;
    bool passed = false;
    std::string message;
};

// Headless vehicle simulation tests (no window). AI/scriptable via --vehicle-test-script.
class VehicleTestRunner {
public:
    static constexpr float kTickSec = 1.f / 60.f;

    [[nodiscard]] static int run(const std::filesystem::path& bus_path, const std::filesystem::path& omsi_root,
                                const std::string& scenario);
  [[nodiscard]] static int run_script_file(const std::filesystem::path& bus_path,
                                            const std::filesystem::path& omsi_root,
                                            const std::filesystem::path& script_path);

private:
    [[nodiscard]] static std::vector<VehicleTestResult> run_all(const std::filesystem::path& bus_path,
                                                                   const std::filesystem::path& omsi_root);
    [[nodiscard]] static VehicleTestResult test_load(const std::filesystem::path& bus_path,
                                                      const std::filesystem::path& omsi_root);
    [[nodiscard]] static VehicleTestResult test_startup(const std::filesystem::path& bus_path,
                                                         const std::filesystem::path& omsi_root);
    [[nodiscard]] static VehicleTestResult test_accelerate(const std::filesystem::path& bus_path,
                                                            const std::filesystem::path& omsi_root);
    [[nodiscard]] static VehicleTestResult test_doors(const std::filesystem::path& bus_path,
                                                       const std::filesystem::path& omsi_root);
    [[nodiscard]] static VehicleTestResult test_physics(const std::filesystem::path& bus_path,
                                                         const std::filesystem::path& omsi_root);
};

} // namespace omsi::test
