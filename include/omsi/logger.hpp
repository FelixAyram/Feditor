#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace omsi::log {

void init(const std::filesystem::path& log_path = {});
void shutdown();
void flush();

void trace(std::string_view msg);
void info(std::string_view msg);
void warn(std::string_view msg);
void error(std::string_view msg);

[[nodiscard]] std::filesystem::path log_path();

} // namespace omsi::log
