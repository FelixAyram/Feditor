#include "omsi/vfs_overlay.hpp"
#include "omsi/disk_stats.hpp"
#include "o3d_loader.hpp"

#include <cctype>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace omsi::o3d {
namespace {

template <typename T>
const T* read_pod(const std::uint8_t* data, std::size_t size, std::size_t& offset) {
    if (offset + sizeof(T) > size) throw std::runtime_error("o3d: truncated read");
    const auto* p = reinterpret_cast<const T*>(data + offset);
    offset += sizeof(T);
    return p;
}

std::string read_pascal_cp1252(const std::uint8_t* data, std::size_t size, std::size_t& offset) {
    if (offset >= size) throw std::runtime_error("o3d: pascal string truncated");
    const auto len = data[offset++];
    if (offset + len > size) throw std::runtime_error("o3d: pascal payload truncated");
    std::string out(reinterpret_cast<const char*>(data + offset), len);
    offset += len;
    return out;
}

std::uint32_t read_count(bool long_header, const std::uint8_t* data, std::size_t size, std::size_t& offset) {
    if (long_header) return *read_pod<std::uint32_t>(data, size, offset);
    return *read_pod<std::uint16_t>(data, size, offset);
}

} // namespace

std::vector<Material> parse_materials_from_section(NativeSection section, std::uint16_t count) {
    std::vector<Material> out;
    out.reserve(count);
    std::size_t offset = 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        Material m{};
        m.fixed = *read_pod<MaterialFixed>(section.data, section.size, offset);
        m.texture = read_pascal_cp1252(section.data, section.size, offset);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<Bone> parse_bones_from_section(NativeSection section, std::uint16_t count, bool long_indices) {
    std::vector<Bone> out;
    out.reserve(count);
    std::size_t offset = 0;
    for (std::uint16_t b = 0; b < count; ++b) {
        Bone bone{};
        bone.name = read_pascal_cp1252(section.data, section.size, offset);
        const auto weight_count = *read_pod<std::uint16_t>(section.data, section.size, offset);
        bone.weights.reserve(weight_count);
        for (std::uint16_t w = 0; w < weight_count; ++w) {
            if (long_indices) {
                const auto idx = *read_pod<std::uint32_t>(section.data, section.size, offset);
                const auto weight = *read_pod<f4f23ma::f32>(section.data, section.size, offset);
                bone.weights.push_back({idx, weight});
            } else {
                const auto idx = *read_pod<std::uint16_t>(section.data, section.size, offset);
                const auto weight = *read_pod<f4f23ma::f32>(section.data, section.size, offset);
                bone.weights.push_back({idx, weight});
            }
        }
        out.push_back(std::move(bone));
    }
    return out;
}

std::vector<Material> NativeMesh::parse_materials() const {
    return parse_materials_from_section(materials_raw, material_count);
}

std::vector<Bone> NativeMesh::parse_bones() const {
  if (!bones_raw.data) return {};
  std::size_t offset = 0;
  const auto count = *read_pod<std::uint16_t>(bones_raw.data, bones_raw.size, offset);
  NativeSection payload{bones_raw.data + offset, bones_raw.size - offset};
  return parse_bones_from_section(payload, count, long_triangle_indices);
}

MappedO3D::MappedO3D(std::filesystem::path path) : path_(std::move(path)) {
    if (auto ram = omsi::vfs::get(path_)) {
        if (ram->empty()) return;
        ram_ = std::move(ram);
        data_ = reinterpret_cast<const std::uint8_t*>(ram_->data());
        size_ = ram_->size();
        if (!parse()) unmap();
        return;
    }
#ifdef _WIN32
    // (open + map + parse: parse touches every page, so the page faults - the real disk reads - are in it)
    omsi::disk_stats::Timer disk_t(omsi::disk_stats::O3d, path_);
    file_handle_ = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file_handle_ == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(static_cast<HANDLE>(file_handle_), &file_size) || file_size.QuadPart <= 0) return;

    mapping_ = CreateFileMappingW(static_cast<HANDLE>(file_handle_), nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) return;

    data_ = static_cast<const std::uint8_t*>(
        MapViewOfFile(static_cast<HANDLE>(mapping_), FILE_MAP_READ, 0, 0, 0));
    if (!data_) return;

    size_ = static_cast<std::size_t>(file_size.QuadPart);
    disk_t.bytes = size_;
    if (!parse()) {
        unmap();
        return;
    }
#else
    (void)path_;
#endif
}

MappedO3D::~MappedO3D() { unmap(); }

MappedO3D::MappedO3D(MappedO3D&& other) noexcept { *this = std::move(other); }

MappedO3D& MappedO3D::operator=(MappedO3D&& other) noexcept {
    if (this != &other) {
        unmap();
        path_ = std::move(other.path_);
        file_handle_ = other.file_handle_;
        mapping_ = other.mapping_;
        data_ = other.data_;
        size_ = other.size_;
        mesh_ = other.mesh_;
        ram_ = std::move(other.ram_);
        other.file_handle_ = nullptr;
        other.mapping_ = nullptr;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void MappedO3D::unmap() {
    if (ram_) {
        ram_.reset();
        data_ = nullptr;
        size_ = 0;
        return;
    }
#ifdef _WIN32
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_handle_ && file_handle_ != INVALID_HANDLE_VALUE)
        CloseHandle(static_cast<HANDLE>(file_handle_));
#endif
    data_ = nullptr;
    mapping_ = nullptr;
    file_handle_ = nullptr;
    size_ = 0;
}

std::span<const std::uint8_t> MappedO3D::bytes() const noexcept {
    return {data_, size_};
}

bool MappedO3D::parse() {
    if (!data_ || size_ < sizeof(FileHeaderV1)) return false;

    std::size_t offset = 0;
    const auto* hdr = read_pod<FileHeaderV1>(data_, size_, offset);
    if (hdr->magic != kMagic) return false;
    if (hdr->version > 7) return false;

    mesh_ = {};
    mesh_.version = hdr->version;
    const bool long_header = hdr->version > 3;

    if (hdr->version >= 3) {
        const auto options = *read_pod<std::uint8_t>(data_, size_, offset);
        mesh_.long_triangle_indices = (options & 1u) != 0;
        mesh_.alt_encryption_seed = (options & 2u) != 0;
        if (hdr->version >= 4) {
            mesh_.encryption_key = *read_pod<std::uint32_t>(data_, size_, offset);
            mesh_.encrypted = mesh_.encryption_key != kNoEncryption;
        }
    }

    while (offset < size_) {
        const auto tag = *read_pod<std::uint8_t>(data_, size_, offset);
        switch (tag) {
        case kChunkVertices: {
            mesh_.vertex_count = read_count(long_header, data_, size_, offset);
            mesh_.vertices = {data_ + offset, mesh_.vertex_count * sizeof(Vertex)};
            offset += mesh_.vertices.size;
            break;
        }
        case kChunkTriangles: {
            mesh_.triangle_count = read_count(long_header, data_, size_, offset);
            const std::size_t stride =
                mesh_.long_triangle_indices ? sizeof(TriangleU32) : sizeof(TriangleU16);
            mesh_.triangles = {data_ + offset, mesh_.triangle_count * stride};
            offset += mesh_.triangles.size;
            break;
        }
        case kChunkMaterials: {
            mesh_.material_count = *read_pod<std::uint16_t>(data_, size_, offset);
            const auto start = offset;
            for (std::uint16_t i = 0; i < mesh_.material_count; ++i) {
                offset += sizeof(MaterialFixed);
                if (offset >= size_) return false;
                const auto len = data_[offset++];
                offset += len;
                if (offset > size_) return false;
            }
            mesh_.materials_raw = {data_ + start, offset - start};
            break;
        }
        case kChunkBones: {
            const auto start = offset;
            const auto count = *read_pod<std::uint16_t>(data_, size_, offset);
            for (std::uint16_t b = 0; b < count; ++b) {
                if (offset >= size_) return false;
                const auto name_len = data_[offset++];
                offset += name_len;
                const auto wcount = *read_pod<std::uint16_t>(data_, size_, offset);
                const std::size_t pair_stride = mesh_.long_triangle_indices ? 8u : 6u;
                offset += wcount * pair_stride;
                if (offset > size_) return false;
            }
            mesh_.bones_raw = {data_ + start, offset - start};
            break;
        }
        case kChunkTransform: {
            mesh_.transform = {data_ + offset, sizeof(TransformBlock)};
            offset += sizeof(TransformBlock);
            break;
        }
        default:
            return false;
        }
    }

    return mesh_.vertex_count > 0 && mesh_.triangle_count > 0;
}

std::optional<MappedO3D> load_mapped(const std::filesystem::path& path) {
    MappedO3D file(path);
    if (!file.valid()) return std::nullopt;
    return file;
}

namespace {
std::mutex g_mapped_mu;
std::unordered_map<std::string, std::shared_ptr<MappedO3D>> g_mapped_cache;

std::string mapped_key(const std::filesystem::path& p) {
    auto s = p.lexically_normal().string();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

std::shared_ptr<MappedO3D> load_mapped_shared(const std::filesystem::path& path) {
    const auto key = mapped_key(path);
    {
        std::lock_guard lock(g_mapped_mu);
        if (const auto it = g_mapped_cache.find(key); it != g_mapped_cache.end()) return it->second;
    }
    auto loaded = load_mapped(path);
    if (!loaded) return {};
    auto sp = std::make_shared<MappedO3D>(std::move(*loaded));
    std::lock_guard lock(g_mapped_mu);
    g_mapped_cache.emplace(key, sp);
    return sp;
}

void invalidate_mapped_cache(const std::filesystem::path& path) {
    std::lock_guard lock(g_mapped_mu);
    if (path.empty()) {
        g_mapped_cache.clear();
        return;
    }
    g_mapped_cache.erase(mapped_key(path));
}

} // namespace omsi::o3d
