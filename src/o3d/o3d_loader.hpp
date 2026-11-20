#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "omsi/o3d_format.hpp"

namespace omsi::o3d {

// Memory-mapped O3D — file bytes stay in place; spans point into the mapping.
class MappedO3D {
public:
    MappedO3D() = default;
    explicit MappedO3D(std::filesystem::path path);
    ~MappedO3D();

    MappedO3D(const MappedO3D&) = delete;
    MappedO3D& operator=(const MappedO3D&) = delete;
    MappedO3D(MappedO3D&&) noexcept;
    MappedO3D& operator=(MappedO3D&&) noexcept;

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] const NativeMesh& mesh() const noexcept { return mesh_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

private:
    void unmap();
    bool parse();

    std::filesystem::path path_;
    void* file_handle_ = nullptr;
    void* mapping_ = nullptr;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    NativeMesh mesh_{};
    std::shared_ptr<const std::string> ram_; // co-op: bytes held in RAM (omsi::vfs), no mapping
};

[[nodiscard]] std::optional<MappedO3D> load_mapped(const std::filesystem::path& path);
// Process-wide mmap cache — reopen once per unique path.
[[nodiscard]] std::shared_ptr<MappedO3D> load_mapped_shared(const std::filesystem::path& path);
void invalidate_mapped_cache(const std::filesystem::path& path = {});
[[nodiscard]] std::vector<Material> parse_materials_from_section(NativeSection section, std::uint16_t count);
[[nodiscard]] std::vector<Bone> parse_bones_from_section(NativeSection section, std::uint16_t count, bool long_indices);

} // namespace omsi::o3d
