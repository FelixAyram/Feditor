#include "omsi/utf8_win32.hpp"
#include "omsi/disk_stats.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/texture/internal.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"

#include <deque>
#include <objbase.h>

#include <atomic>
#include <execution>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omsi::gfx::dx12 {
namespace {
std::atomic<VirtualFileExists> g_vfs_exists{nullptr};
std::atomic<VirtualFileRead> g_vfs_read{nullptr};

std::mutex g_cpu_tex_mu;
std::unordered_map<std::string, std::shared_ptr<CpuRgbaImage>> g_cpu_tex;
// Full mip chains built on prefetch workers (dilate + box filter moved off the UI thread).
std::unordered_map<std::string, std::shared_ptr<const texture_detail::RgbaMipChain>> g_mip_tex;
std::unordered_map<std::string, std::shared_ptr<std::vector<std::uint8_t>>> g_raw_tex;
std::size_t g_raw_tex_bytes = 0;
std::deque<std::string> g_raw_order; // keys in the order they came (the oldest go first when full)
constexpr std::size_t kRawTexCap = 384ull << 20;

std::string cpu_tex_key(const std::filesystem::path& path) {
    // path_text: path::string() throws on non-ANSI names (that terminated worker threads).
    return texture_detail::lower_ascii(omsi::path_text(path.lexically_normal()));
}

} // namespace

void set_virtual_file_provider(VirtualFileExists exists, VirtualFileRead read) {
    g_vfs_exists.store(exists);
    g_vfs_read.store(read);
}

bool is_virtual_path(const std::filesystem::path& p) {
    const auto& n = p.native();
    return n.size() > 4 && (n[0] == L'v' || n[0] == L'V') && (n[1] == L'f' || n[1] == L'F') &&
           (n[2] == L's' || n[2] == L'S') && n[3] == L':';
}

namespace texture_detail {

bool virtual_path_exists(const std::filesystem::path& path) {
    const std::string v = path.generic_string().substr(4);
    if (const VirtualFileExists ex = g_vfs_exists.load()) return ex(v);
    std::vector<std::uint8_t> tmp;
    const VirtualFileRead rd = g_vfs_read.load();
    return rd && rd(v, tmp);
}

std::shared_ptr<const CpuRgbaImage> cpu_texture_cache_take(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(g_cpu_tex_mu);
    const auto it = g_cpu_tex.find(cpu_tex_key(path));
    if (it == g_cpu_tex.end()) return nullptr;
    auto img = it->second;
    g_cpu_tex.erase(it);
    return img;
}

void cpu_texture_cache_put(const std::filesystem::path& path, CpuRgbaImage image) {
    if (image.rgba.empty() || image.width == 0 || image.height == 0) return;
    std::lock_guard<std::mutex> lock(g_cpu_tex_mu);
    g_cpu_tex[cpu_tex_key(path)] = std::make_shared<CpuRgbaImage>(std::move(image));
}

std::shared_ptr<const RgbaMipChain> cpu_mip_cache_take(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(g_cpu_tex_mu);
    const auto it = g_mip_tex.find(cpu_tex_key(path));
    if (it == g_mip_tex.end()) return nullptr;
    auto chain = it->second;
    g_mip_tex.erase(it);
    return chain;
}

void cpu_mip_cache_put(const std::filesystem::path& path, RgbaMipChain chain) {
    if (chain.levels == 0 || chain.mips.empty()) return;
    std::lock_guard<std::mutex> lock(g_cpu_tex_mu);
    g_mip_tex[cpu_tex_key(path)] = std::make_shared<const RgbaMipChain>(std::move(chain));
}

void cpu_texture_cache_invalidate(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(g_cpu_tex_mu);
    auto drop = [](const std::filesystem::path& p) {
        g_mip_tex.erase(cpu_tex_key(p));
        g_cpu_tex.erase(cpu_tex_key(p));
        const auto it = g_raw_tex.find(cpu_tex_key(p));
        if (it != g_raw_tex.end()) {
            g_raw_tex_bytes -= it->second->size();
            g_raw_tex.erase(it);
        }
    };
    drop(path);
    // load_texture_file prefers <stem>.dds, so the cached image may live under the .dds key
    // (invalidating only the original name re-uploaded the stale pixels after a quality change).
    if (!iequals_ext(path, L".dds")) drop(path.parent_path() / (path.stem().wstring() + L".dds"));
}

std::shared_ptr<const std::vector<std::uint8_t>> raw_file_cache_take(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(g_cpu_tex_mu);
    const auto it = g_raw_tex.find(cpu_tex_key(path));
    if (it == g_raw_tex.end()) return nullptr;
    auto b = it->second;
    g_raw_tex_bytes -= b->size();
    g_raw_tex.erase(it);
    return b;
}

void raw_file_cache_put(const std::filesystem::path& path, std::vector<std::uint8_t> bytes) {
    if (bytes.empty()) return;
    decltype(g_raw_tex) dropped; // freed after the lock is released (up to 384 MB)
    std::lock_guard<std::mutex> lock(g_cpu_tex_mu);
    if (bytes.size() > kRawTexCap / 4) return;
    // Full: the oldest entries go (never taken - their textures went up some other way), one by one. Emptied
    // whole, the files the workers had just read for the objects streaming in went too, and the main thread
    // read them again off the disk while creating those objects (90-180 ms hitches on an HDD at 100 km/h).
    std::vector<std::shared_ptr<std::vector<std::uint8_t>>> freed; // (freed after the lock is released)
    while (g_raw_tex_bytes + bytes.size() > kRawTexCap && !g_raw_order.empty()) {
        const auto it = g_raw_tex.find(g_raw_order.front());
        g_raw_order.pop_front();
        if (it == g_raw_tex.end()) continue; // (taken already)
        g_raw_tex_bytes -= it->second->size();
        freed.push_back(std::move(it->second));
        g_raw_tex.erase(it);
    }
    if (g_raw_tex_bytes + bytes.size() > kRawTexCap) {
        dropped.swap(g_raw_tex);
        g_raw_tex_bytes = 0;
        g_raw_order.clear();
    }
    auto key = cpu_tex_key(path);
    if (const auto old_it = g_raw_tex.find(key); old_it != g_raw_tex.end()) g_raw_tex_bytes -= old_it->second->size();
    g_raw_tex_bytes += bytes.size();
    g_raw_order.push_back(key);
    if (g_raw_order.size() > 65536) g_raw_order.pop_front(); // (stale keys of taken entries)
    g_raw_tex[std::move(key)] = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
}

bool read_file_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    if (is_virtual_path(path)) {
        const VirtualFileRead rd = g_vfs_read.load();
        return rd && rd(path.generic_string().substr(4), out) && !out.empty();
    }
    if (const auto ram = omsi::vfs::get(path)) {
        out.assign(ram->begin(), ram->end());
        return !out.empty();
    }
    omsi::disk_stats::Timer disk_t(omsi::disk_stats::Texture, path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto n = static_cast<std::streamoff>(f.tellg());
    disk_t.bytes = n > 0 ? static_cast<std::uint64_t>(n) : 0u;
    if (n <= 0) return false;
    out.resize(static_cast<std::size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(f);
}

} // namespace texture_detail

Dx12Texture load_texture_file(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                              const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                              const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, const std::filesystem::path& path) {
    Dx12Texture out{};
    if (!device || !cmd_list || path.empty()) return out;

    auto load_one = [&](const std::filesystem::path& p) -> Dx12Texture {
        try {
            if (!texture_detail::path_exists(p)) return {};
            if (!is_virtual_path(p)) {
                // by the bytes, as D3DX does; the extension only where the bytes say nothing
                using F = texture_detail::TexFormat;
                switch (texture_detail::sniff_texture_format(p)) {
                case F::dds: return load_texture_dds(device, cmd_list, srv_cpu, srv_gpu, p);
                case F::bmp:
                case F::png:
                case F::jpg: return load_texture_wic(device, cmd_list, srv_cpu, srv_gpu, p);
                case F::tga:
                    if (!texture_detail::iequals_ext(p, L".dds")) return load_texture_tga(device, cmd_list, srv_cpu, srv_gpu, p);
                    break;
                default: break;
                }
            }
            if (texture_detail::iequals_ext(p, L".dds")) return load_texture_dds(device, cmd_list, srv_cpu, srv_gpu, p);
            if (texture_detail::iequals_ext(p, L".tga")) return load_texture_tga(device, cmd_list, srv_cpu, srv_gpu, p);
            return load_texture_wic(device, cmd_list, srv_cpu, srv_gpu, p);
        } catch (...) {
            return {};
        }
    };

    if (!texture_detail::iequals_ext(path, L".dds")) {
        const auto dds = path.parent_path() / (path.stem().wstring() + L".dds");
        out = load_one(dds);
        if (out.valid) return out;
    }
    return load_one(path);
}

void prefetch_textures_cpu(const std::vector<std::filesystem::path>& paths_in) {
    // OMSI's night textures (the same file in a `night` folder beside it, looked for by upload_mesh_gpu for every
    // texture without a [matl_nightmap]) are read here too: the main thread read them off the disk itself while
    // creating the streamed objects (stream.upload_scenery stalls of 50-400 ms on an HDD at 100 km/h)
    std::vector<std::filesystem::path> paths = paths_in;
    for (const auto& tp : paths_in) {
        if (tp.empty() || is_virtual_path(tp)) continue;
        const auto night_dir = tp.parent_path() / L"night";
        auto night = night_dir / tp.filename();
        if (!texture_detail::path_exists(night)) night = night_dir / (tp.stem().wstring() + L".dds");
        if (!texture_detail::path_exists(night)) night = night_dir / (tp.stem().wstring() + L".bmp");
        if (texture_detail::path_exists(night)) paths.push_back(std::move(night));
    }
    std::vector<std::filesystem::path> work;
    std::vector<std::filesystem::path> raw_work;
    work.reserve(paths.size());
    // The disk probes (does <stem>.dds exist, does it need CPU mips = header read) used to run for every path while
    // holding g_cpu_tex_mu. The UI thread takes that same mutex for every texture it uploads, so one big prefetch
    // stalled object creation for 100-400 ms. Probe first without the lock, then filter against the caches.
    struct Cand {
        std::filesystem::path load_p;
        std::string key;
        bool raw_only = false;
    };
    std::vector<Cand> cands;
    cands.reserve(paths.size());
    {
        std::unordered_set<std::string> seen;
        for (const auto& p : paths) {
            if (p.empty()) continue;
            Cand c;
            c.load_p = p;
            if (!texture_detail::iequals_ext(p, L".dds")) {
                const auto dds = p.parent_path() / (p.stem().wstring() + L".dds");
                if (texture_detail::path_exists(dds)) {
                    c.raw_only = texture_max_edge() == 0 && !texture_detail::dds_file_needs_cpu_mips(dds);
                    c.load_p = dds;
                }
            } else {
                c.raw_only = texture_max_edge() == 0 && !texture_detail::dds_file_needs_cpu_mips(p);
            }
            // (sniffed here for every file: load_texture_file sniffs before it looks in the caches, and on the
            //  main thread that opened the file - a seek on an HDD - for textures already read)
            const auto fmt = is_virtual_path(c.load_p) ? texture_detail::TexFormat::unknown
                                                       : texture_detail::sniff_texture_format(c.load_p);
            if (c.raw_only && !is_virtual_path(c.load_p) && fmt != texture_detail::TexFormat::dds)
                c.raw_only = false;
            c.key = cpu_tex_key(c.load_p);
            if (!seen.insert(c.key).second) continue;
            cands.push_back(std::move(c));
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_cpu_tex_mu);
        for (auto& c : cands) {
            if (c.raw_only) {
                if (g_raw_tex.find(c.key) == g_raw_tex.end()) raw_work.push_back(std::move(c.load_p));
                continue;
            }
            if (g_mip_tex.find(c.key) != g_mip_tex.end()) continue;
            work.push_back(std::move(c.load_p));
        }
    }
    std::for_each(std::execution::par, raw_work.begin(), raw_work.end(), [](const std::filesystem::path& p) {
        try {
            std::vector<std::uint8_t> bytes;
            if (texture_detail::read_file_bytes(p, bytes)) texture_detail::raw_file_cache_put(p, std::move(bytes));
        } catch (...) {
        }
    });
    if (work.empty()) return;

    std::for_each(std::execution::par, work.begin(), work.end(), [](const std::filesystem::path& p) {
        try {
            (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::optional<CpuRgbaImage> img;
            img = load_image_cpu_rgba(p);
            if (img && !img->rgba.empty() && img->width > 0 && img->height > 0) {
                // Mip chain (scale/dilate/box filter) is pure CPU: build it here on the worker.
                texture_detail::cpu_mip_cache_put(
                    p, texture_detail::build_rgba_mip_chain(img->rgba.data(), img->width, img->height,
                                                            img->width * 4u, 0));
            }
        } catch (...) {
        }
    });
}

} // namespace omsi::gfx::dx12
