#pragma once

// RAM overlay for one map folder ("edit the host's version in RAM", co-op guest).
// Files received from the host live only in memory and shadow the same paths on disk; files the
// host does not have are hidden. While the overlay is active the guest's own files are held open
// without write sharing, so nothing in the editor can modify them (writes fail, reads still work),
// and writers that would create new files under the root are refused (write_blocked).
// Everything is a no-op / plain disk access while no overlay is active.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace omsi::vfs {

[[nodiscard]] bool active();
// start a new overlay for `root` (drops the previous one)
void begin(const std::filesystem::path& root);
// active without a protected map root (co-op guest receiving the host's objects / textures only)
void activate_assets();
void put(const std::filesystem::path& path, std::string bytes);
void hide(const std::filesystem::path& path);
// hold every existing file under the root open (read sharing only) until end()
void protect_disk_files();
void end();

[[nodiscard]] std::filesystem::path root();
[[nodiscard]] bool under_root(const std::filesystem::path& path);
// true while the overlay is active and `path` is inside its root: do not write it
[[nodiscard]] bool write_blocked(const std::filesystem::path& path);

// RAM bytes of `path`, or null (not overlaid / overlay inactive)
[[nodiscard]] std::shared_ptr<const std::string> get(const std::filesystem::path& path);
[[nodiscard]] bool hidden(const std::filesystem::path& path);
// fs::exists that sees RAM files / directories and hides removed ones
[[nodiscard]] bool exists(const std::filesystem::path& path);
[[nodiscard]] bool exists(const std::filesystem::path& path, std::error_code& ec);
// whole file from RAM or disk
bool read_file(const std::filesystem::path& path, std::string& out);

[[nodiscard]] std::size_t ram_bytes();
[[nodiscard]] std::size_t ram_files();

} // namespace omsi::vfs
