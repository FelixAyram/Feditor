#include "beamng/bng_vfs.hpp"

#include "beamng/bng_json.hpp"

#include "../../third_party/miniz/miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <iterator>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bng {

// Own zip reader over a memory-mapped file: handles zip64 records (BeamNG's big level zips), and reads are
// lock-free (every worker reads straight from the mapping; only deflated entries are inflated).
struct Vfs_ZipItem {
    std::uint64_t local_offset = 0;
    std::uint64_t comp_size = 0;
    std::uint64_t size = 0;
    std::uint16_t method = 0;
    std::string name;
};

struct Vfs::ZipHandle {
    using Item = Vfs_ZipItem;
    const std::uint8_t* base = nullptr;
    std::uint64_t file_size = 0;
    std::vector<Item> items;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#endif
    std::string error;

    ~ZipHandle() {
#ifdef _WIN32
        if (base) UnmapViewOfFile(base);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
#endif
    }
    static std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
    static std::uint32_t u32(const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    }
    static std::uint64_t u64(const std::uint8_t* p) { return static_cast<std::uint64_t>(u32(p)) | (static_cast<std::uint64_t>(u32(p + 4)) << 32); }

    bool fresh = false; // central directory was read from disk (not from the index cache)
    bool open(const std::filesystem::path& path, const std::vector<Item>* cached = nullptr) {
#ifdef _WIN32
        file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (file == INVALID_HANDLE_VALUE) { error = "no se pudo abrir"; return false; }
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz) || sz.QuadPart < 22) { error = "archivo vacio"; return false; }
        file_size = static_cast<std::uint64_t>(sz.QuadPart);
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) { error = "no se pudo mapear"; return false; }
        base = static_cast<const std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!base) { error = "no se pudo mapear (vista)"; return false; }
#else
        return false;
#endif
        if (cached) {
            items = *cached;
            return true;
        }
        fresh = true;
        // End of central directory record (within the last 64 KiB + 22 bytes)
        const std::uint64_t lowest = file_size > 65557 ? file_size - 65557 : 0;
        std::uint64_t eocd = UINT64_MAX;
        for (std::uint64_t i = file_size - 22 + 1; i-- > lowest;) {
            if (u32(base + i) == 0x06054b50u) { eocd = i; break; }
        }
        if (eocd == UINT64_MAX) { error = "sin directorio central"; return false; }
        std::uint64_t count = u16(base + eocd + 10);
        std::uint64_t cd_size = u32(base + eocd + 12);
        std::uint64_t cd_off = u32(base + eocd + 16);
        // zip64: only trusted where the classic fields are saturated (some writers add it unconditionally)
        if (eocd >= 20 && u32(base + eocd - 20) == 0x07064b50u) {
            const std::uint64_t z64 = u64(base + eocd - 20 + 8);
            if (z64 + 56 <= file_size && u32(base + z64) == 0x06064b50u) {
                if (count == 0xFFFF) count = u64(base + z64 + 32);
                if (cd_size == 0xFFFFFFFFu) cd_size = u64(base + z64 + 40);
                if (cd_off == 0xFFFFFFFFu) cd_off = u64(base + z64 + 48);
            }
        }
        if (cd_off + cd_size > file_size) { error = "directorio central fuera de rango"; return false; }
        items.reserve(static_cast<std::size_t>(count));
        std::uint64_t p = cd_off;
        const std::uint64_t end = cd_off + cd_size;
        while (p + 46 <= end && u32(base + p) == 0x02014b50u) {
            Item it;
            it.method = u16(base + p + 10);
            it.comp_size = u32(base + p + 20);
            it.size = u32(base + p + 24);
            const std::uint16_t nlen = u16(base + p + 28), xlen = u16(base + p + 30), clen = u16(base + p + 32);
            it.local_offset = u32(base + p + 42);
            it.name.assign(reinterpret_cast<const char*>(base + p + 46), nlen);
            std::uint64_t x = p + 46 + nlen;
            const std::uint64_t xend = x + xlen;
            while (x + 4 <= xend) {
                const std::uint16_t id = u16(base + x), len = u16(base + x + 2);
                if (id == 0x0001) {
                    std::uint64_t q = x + 4;
                    const std::uint64_t qe = x + 4 + len;
                    if (it.size == 0xFFFFFFFFu && q + 8 <= qe) { it.size = u64(base + q); q += 8; }
                    if (it.comp_size == 0xFFFFFFFFu && q + 8 <= qe) { it.comp_size = u64(base + q); q += 8; }
                    if (it.local_offset == 0xFFFFFFFFu && q + 8 <= qe) { it.local_offset = u64(base + q); q += 8; }
                }
                x += 4 + static_cast<std::uint64_t>(len);
            }
            items.push_back(std::move(it));
            p += 46 + static_cast<std::uint64_t>(nlen) + xlen + clen;
        }
        return true;
    }

    bool extract(std::size_t idx, std::vector<std::uint8_t>& out) const {
        if (idx >= items.size()) return false;
        const Item& it = items[idx];
        if (it.local_offset + 30 > file_size || u32(base + it.local_offset) != 0x04034b50u) return false;
        const std::uint64_t data = it.local_offset + 30 + u16(base + it.local_offset + 26) + u16(base + it.local_offset + 28);
        if (data + it.comp_size > file_size) return false;
        out.resize(static_cast<std::size_t>(it.size));
        if (it.method == 0) {
            if (it.size) std::memcpy(out.data(), base + data, static_cast<std::size_t>(it.size));
            return true;
        }
        if (it.method == 8) {
            const std::size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(), base + data, static_cast<std::size_t>(it.comp_size),
                                                                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            return got == out.size();
        }
        return false;
    }
};

namespace {
// ── zip index cache (beamng_vfs_index.bin next to the exe) ──────────────────────────────────────────────
// Reading ~250 central directories from a cold HDD takes seconds; the cache keeps them keyed by path+size+mtime.
struct ZipCacheEntry {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::vector<Vfs_ZipItem> items;
};
std::unordered_map<std::string, ZipCacheEntry> g_zip_cache;
std::mutex g_zip_cache_mu;

std::filesystem::path zip_cache_path() {
#ifdef _WIN32
    wchar_t mod[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(mod).parent_path() / "beamng_vfs_index.bin";
#endif
    return "beamng_vfs_index.bin";
}
std::string zip_key(const std::filesystem::path& p) {
    const auto u = p.lexically_normal().u8string();
    std::string k(u.begin(), u.end());
    for (auto& c : k)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return k;
}
std::int64_t file_mtime(const std::filesystem::path& p) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}
template <class T> void put(std::string& o, const T& v) { o.append(reinterpret_cast<const char*>(&v), sizeof(T)); }
void put_str(std::string& o, const std::string& s) {
    put(o, static_cast<std::uint32_t>(s.size()));
    o += s;
}
struct Rd {
    const std::string& b;
    std::size_t i = 0;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (i + sizeof(T) > b.size()) { ok = false; return v; }
        std::memcpy(&v, b.data() + i, sizeof(T));
        i += sizeof(T);
        return v;
    }
    std::string str() {
        const auto n = get<std::uint32_t>();
        if (!ok || i + n > b.size()) { ok = false; return {}; }
        std::string s = b.substr(i, n);
        i += n;
        return s;
    }
};
constexpr std::uint32_t kZipCacheMagic = 0x42564631; // "1FVB"

void load_zip_cache() {
    std::lock_guard<std::mutex> lk(g_zip_cache_mu);
    if (!g_zip_cache.empty()) return;
    std::ifstream f(zip_cache_path(), std::ios::binary);
    if (!f) return;
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Rd r{b};
    if (r.get<std::uint32_t>() != kZipCacheMagic) return;
    const auto nz = r.get<std::uint32_t>();
    for (std::uint32_t z = 0; z < nz && r.ok; ++z) {
        const std::string key = r.str();
        ZipCacheEntry e;
        e.size = r.get<std::uint64_t>();
        e.mtime = r.get<std::int64_t>();
        const auto ni = r.get<std::uint32_t>();
        e.items.resize(ni);
        for (auto& it : e.items) {
            it.local_offset = r.get<std::uint64_t>();
            it.comp_size = r.get<std::uint64_t>();
            it.size = r.get<std::uint64_t>();
            it.method = r.get<std::uint16_t>();
            it.name = r.str();
        }
        if (r.ok) g_zip_cache[key] = std::move(e);
    }
}
const std::vector<Vfs_ZipItem>* cached_items(const std::filesystem::path& zip, std::uint64_t size, std::int64_t mtime) {
    std::lock_guard<std::mutex> lk(g_zip_cache_mu);
    auto it = g_zip_cache.find(zip_key(zip));
    if (it == g_zip_cache.end() || it->second.size != size || it->second.mtime != mtime) return nullptr;
    return &it->second.items; // entries are never erased while mounting
}
} // namespace

namespace {
std::string lower_ascii(std::string_view s) {
    std::string o(s);
    for (auto& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}
bool ends_with_ci(std::string_view s, std::string_view suf) {
    if (suf.size() > s.size()) return false;
    for (std::size_t i = 0; i < suf.size(); ++i) {
        char a = s[s.size() - suf.size() + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}
std::string parent_dir(std::string_view p) {
    const auto k = p.find_last_of('/');
    return k == std::string_view::npos ? std::string("/") : std::string(p.substr(0, k + 1));
}
} // namespace

Vfs::Vfs() = default;
Vfs::~Vfs() = default;

std::string Vfs::normalize(std::string_view path) {
    std::string o;
    o.reserve(path.size() + 1);
    for (char c : path) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        o.push_back(c);
    }
    // collapse "//" and "/./"
    std::string r;
    r.reserve(o.size() + 1);
    if (o.empty() || o[0] != '/') r.push_back('/');
    for (std::size_t i = 0; i < o.size(); ++i) {
        if (o[i] == '/' && !r.empty() && r.back() == '/') continue;
        if (o[i] == '.' && (i + 1 == o.size() || o[i + 1] == '/') && !r.empty() && r.back() == '/') {
            ++i;
            continue;
        }
        r.push_back(o[i]);
    }
    // resolve ".."
    if (r.find("/..") != std::string::npos) {
        std::vector<std::string> parts;
        std::stringstream ss(r);
        std::string part;
        while (std::getline(ss, part, '/')) {
            if (part.empty()) continue;
            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else
                parts.push_back(part);
        }
        r.clear();
        for (const auto& p : parts) r += "/" + p;
        if (r.empty()) r = "/";
    }
    return r;
}

void Vfs::add_entry(std::string norm, VfsEntry e, int priority) {
    auto it = index_priority_.find(norm);
    if (it != index_priority_.end() && it->second > priority) return;
    index_priority_[norm] = priority;
    index_[std::move(norm)] = std::move(e);
}

bool Vfs::mount_zip(const std::filesystem::path& zip_path, int priority) {
    auto h = std::make_unique<ZipHandle>();
    std::error_code ec;
    const auto fsize = static_cast<std::uint64_t>(std::filesystem::file_size(zip_path, ec));
    const auto* cached = ec ? nullptr : cached_items(zip_path, fsize, file_mtime(zip_path));
    if (!h->open(zip_path, cached)) {
        failed_mounts.push_back(zip_path.string() + " (" + h->error + ")");
        return false;
    }
    const auto mount_idx = static_cast<std::uint32_t>(mounts_.size());
    std::size_t added = 0;
    for (std::size_t i = 0; i < h->items.size(); ++i) {
        const auto& it = h->items[i];
        if (it.name.empty() || it.name.back() == '/') continue;
        VfsEntry e;
        e.mount = mount_idx;
        e.zip_index = static_cast<std::uint32_t>(i);
        e.size = it.size;
        e.real_name = "/" + it.name;
        add_entry(normalize(it.name), std::move(e), priority);
        ++added;
    }
    mounts_.push_back({zip_path, true, priority, added});
    zips_.push_back(std::move(h));
    folder_roots_.emplace_back();
    return true;
}

bool Vfs::mount_folder(const std::filesystem::path& folder, int priority, std::string_view virtual_prefix) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return false;
    const auto mount_idx = static_cast<std::uint32_t>(mounts_.size());
    std::size_t added = 0;
    const std::string prefix = normalize(virtual_prefix);
    for (auto it = std::filesystem::recursive_directory_iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const auto rel = std::filesystem::relative(it->path(), folder, ec).generic_u8string();
        std::string rel_s(rel.begin(), rel.end());
        VfsEntry e;
        e.mount = mount_idx;
        e.size = it->file_size(ec);
        e.real_name = (prefix == "/" ? std::string("/") : prefix + "/") + rel_s;
        e.loose_rel = rel_s;
        add_entry(normalize(e.real_name), std::move(e), priority);
        ++added;
    }
    mounts_.push_back({folder, false, priority, added});
    zips_.push_back(nullptr);
    folder_roots_.push_back(folder);
    return true;
}

bool Vfs::mount_beamng(const std::filesystem::path& install_root, const std::filesystem::path& user_folder,
                       const std::function<void(int, int, const std::string&)>& progress) {
    std::error_code ec;
    struct Job {
        std::filesystem::path path;
        int priority;
        bool zip;
    };
    std::vector<Job> jobs;
    auto add_zips = [&](const std::filesystem::path& dir, int prio, bool recursive) {
        if (!std::filesystem::is_directory(dir, ec)) return;
        auto take = [&](const std::filesystem::directory_entry& d) {
            if (d.is_regular_file(ec) && ends_with_ci(d.path().filename().string(), ".zip")) jobs.push_back({d.path(), prio, true});
        };
        if (recursive) {
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec)) {
                if (ec) break;
                if (it->is_directory(ec) && _wcsicmp(it->path().filename().c_str(), L"unpacked") == 0) {
                    it.disable_recursion_pending();
                    continue;
                }
                take(*it);
            }
        } else {
            for (const auto& d : std::filesystem::directory_iterator(dir, ec)) take(d);
        }
    };
    // install
    if (std::filesystem::exists(install_root / "gameengine.zip", ec)) jobs.push_back({install_root / "gameengine.zip", 10, true});
    add_zips(install_root / "content" / "assets", 20, true);
    if (std::filesystem::exists(install_root / "content" / "art_shapes.zip", ec))
        jobs.push_back({install_root / "content" / "art_shapes.zip", 20, true});
    add_zips(install_root / "content" / "levels", 30, false);
    // user folder (mods + loose)
    if (!user_folder.empty()) {
        add_zips(user_folder / "mods", 40, true);
        const auto unpacked = user_folder / "mods" / "unpacked";
        if (std::filesystem::is_directory(unpacked, ec))
            for (const auto& d : std::filesystem::directory_iterator(unpacked, ec))
                if (d.is_directory(ec)) jobs.push_back({d.path(), 50, false});
    }
    load_zip_cache();
    // Zip central directories are read in parallel; the merge into the index stays deterministic (job order).
    const int total = static_cast<int>(jobs.size()) + (user_folder.empty() ? 0 : 1);
    std::vector<std::future<std::unique_ptr<Vfs>>> futs;
    futs.reserve(jobs.size());
    for (const auto& j : jobs) {
        futs.push_back(std::async(std::launch::async, [j]() {
            auto v = std::make_unique<Vfs>();
            if (j.zip) v->mount_zip(j.path, j.priority);
            else v->mount_folder(j.path, j.priority);
            return v;
        }));
    }
    int done = 0;
    for (std::size_t k = 0; k < futs.size(); ++k) {
        auto part = futs[k].get();
        if (progress) progress(++done, total, jobs[k].path.filename().string());
        for (auto& f : part->failed_mounts) failed_mounts.push_back(std::move(f));
        if (part->mounts_.empty()) continue;
        const auto base = static_cast<std::uint32_t>(mounts_.size());
        for (std::size_t m = 0; m < part->mounts_.size(); ++m) {
            mounts_.push_back(part->mounts_[m]);
            zips_.push_back(std::move(part->zips_[m]));
            folder_roots_.push_back(part->folder_roots_[m]);
        }
        for (auto& [k2, e] : part->index_) {
            VfsEntry ne = e;
            ne.mount += base;
            add_entry(k2, std::move(ne), part->index_priority_[k2]);
        }
    }
    // refresh the index cache when any zip had to be read from disk
    {
        bool any_fresh = false;
        for (const auto& z : zips_)
            if (z && z->fresh) any_fresh = true;
        if (any_fresh) {
            std::string out;
            put(out, kZipCacheMagic);
            std::uint32_t n = 0;
            for (const auto& z : zips_) n += z ? 1u : 0u;
            put(out, n);
            for (std::size_t m = 0; m < zips_.size(); ++m) {
                const auto& z = zips_[m];
                if (!z) continue;
                put_str(out, zip_key(mounts_[m].source));
                put(out, z->file_size);
                put(out, file_mtime(mounts_[m].source));
                put(out, static_cast<std::uint32_t>(z->items.size()));
                for (const auto& it : z->items) {
                    put(out, it.local_offset);
                    put(out, it.comp_size);
                    put(out, it.size);
                    put(out, it.method);
                    put_str(out, it.name);
                }
            }
            std::ofstream f(zip_cache_path(), std::ios::binary | std::ios::trunc);
            f.write(out.data(), static_cast<std::streamsize>(out.size()));
        }
    }
    // loose user levels / art override everything (this is also where saves go)
    if (!user_folder.empty()) {
        for (const char* sub : {"levels", "art", "assets"}) {
            if (std::filesystem::is_directory(user_folder / sub, ec)) mount_folder(user_folder / sub, 60, std::string("/") + sub);
        }
        if (progress) progress(total, total, "user folder");
    }
    return !index_.empty();
}

const VfsEntry* Vfs::entry(std::string_view path) const {
    const auto it = index_.find(normalize(path));
    return it == index_.end() ? nullptr : &it->second;
}
bool Vfs::exists(std::string_view path) const { return entry(path) != nullptr; }

std::optional<std::vector<std::uint8_t>> Vfs::read(std::string_view path) const {
    const VfsEntry* e = entry(path);
    if (!e) return std::nullopt;
    if (e->mount >= mounts_.size()) return std::nullopt;
    if (auto* z = zips_[e->mount].get()) {
        std::vector<std::uint8_t> buf;
        if (!z->extract(e->zip_index, buf)) return std::nullopt;
        return buf;
    }
    const auto real = loose_path(path);
    std::ifstream f(real, std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(e->size));
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return buf;
}

std::optional<std::string> Vfs::read_text(std::string_view path) const {
    auto b = read(path);
    if (!b) return std::nullopt;
    return std::string(b->begin(), b->end());
}

std::filesystem::path Vfs::loose_path(std::string_view path) const {
    const VfsEntry* e = entry(path);
    if (!e || e->mount >= mounts_.size() || zips_[e->mount]) return {};
    return folder_roots_[e->mount] / std::filesystem::u8path(e->loose_rel);
}

std::vector<std::string> Vfs::list(std::string_view dir_prefix, std::string_view suffix) const {
    const std::string pre = normalize(dir_prefix);
    std::vector<std::string> out;
    for (const auto& [k, e] : index_) {
        if (k.compare(0, pre.size(), pre) != 0) continue;
        if (!suffix.empty() && !ends_with_ci(k, suffix)) continue;
        out.push_back(k);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string Vfs::resolve_texture(std::string_view path, std::string_view base_dir) const {
    if (path.empty()) return {};
    std::vector<std::string> bases;
    bases.push_back(normalize(path));
    if (!base_dir.empty() && path.find('/') == std::string_view::npos && path.find('\\') == std::string_view::npos)
        bases.push_back(normalize(std::string(base_dir) + "/" + std::string(path)));
    for (int hop = 0; hop < 4; ++hop) {
        std::vector<std::string> next;
        for (const auto& b : bases) {
            std::vector<std::string> cands{b};
            const auto dot = b.find_last_of('.');
            const auto slash = b.find_last_of('/');
            const std::string stem = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? b.substr(0, dot) : b;
            for (const char* ext : {".dds", ".png", ".jpg", ".tga", ".jpeg"}) cands.push_back(stem + ext);
            for (const auto& c : cands)
                if (exists(c)) return c;
            // .link redirect (e.g. foo.color.png.link -> {"path":"/assets/..."})
            for (const auto& c : {b + ".link", stem + ".png.link", stem + ".dds.link"}) {
                if (!exists(c)) continue;
                if (auto t = read_text(c)) {
                    JsonValue j;
                    if (json_parse(*t, j).ok) {
                        const auto& p = j.get("path").as_string();
                        if (!p.empty()) next.push_back(normalize(p));
                    }
                }
            }
        }
        if (next.empty()) break;
        bases = std::move(next);
    }
    return {};
}

std::string Vfs::resolve_shape(std::string_view path, std::string_view base_dir) const {
    if (path.empty()) return {};
    std::string p = normalize(path);
    auto try_ext = [&](const std::string& b) -> std::string {
        if (exists(b) && ends_with_ci(b, ".dae")) return b;
        const auto dot = b.find_last_of('.');
        const std::string stem = dot == std::string::npos ? b : b.substr(0, dot);
        if (exists(stem + ".dae")) return stem + ".dae";
        if (exists(b)) return b;
        return {};
    };
    if (auto r = try_ext(p); !r.empty()) return r;
    if (!base_dir.empty()) {
        if (auto r = try_ext(normalize(std::string(base_dir) + "/" + std::string(path))); !r.empty()) return r;
    }
    // Last resort: match by file stem anywhere under /art/shapes (prefabs and materials often reference the
    // bare shape name, e.g. "rock_small_wall_02" or "shapes/common/wall"), then anywhere in the tree.
    const auto slash = p.find_last_of('/');
    const std::string file = slash == std::string::npos ? p : p.substr(slash + 1);
    const auto dot = file.find_last_of('.');
    const std::string want = dot == std::string::npos ? file : file.substr(0, dot);
    if (!want.empty()) {
        // one pass: prefer a match under base_dir's tree, then any /art/shapes/, then anywhere
        const std::string pre = base_dir.empty() ? std::string() : normalize(base_dir);
        std::string best_base, best_shapes, best_any;
        for (const auto& [k, e] : index_) {
            const auto ks = k.find_last_of('/');
            const std::string kf = ks == std::string::npos ? k : k.substr(ks + 1);
            const auto kd = kf.find_last_of('.');
            const std::string stem = kd == std::string::npos ? kf : kf.substr(0, kd);
            if (stem != want) continue;
            if (!ends_with_ci(kf, ".dae")) continue;
            if (!pre.empty() && k.compare(0, pre.size(), pre) == 0) {
                if (best_base.empty() || k.size() < best_base.size()) best_base = k;
            } else if (k.find("/art/shapes/") != std::string::npos) {
                if (best_shapes.empty() || k < best_shapes) best_shapes = k;
            } else if (best_any.empty() || k < best_any) {
                best_any = k;
            }
        }
        if (!best_base.empty()) return best_base;
        if (!best_shapes.empty()) return best_shapes;
        if (!best_any.empty()) return best_any;
    }
    return {};
}

std::filesystem::path find_beamng_install() {
    std::vector<std::filesystem::path> libs;
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        const std::filesystem::path steam = buf;
        libs.push_back(steam);
        std::ifstream vdf(steam / "steamapps" / "libraryfolders.vdf");
        std::string line;
        while (std::getline(vdf, line)) {
            const auto k = line.find("\"path\"");
            if (k == std::string::npos) continue;
            const auto a = line.find('"', k + 6);
            const auto b = line.find('"', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            std::string p = line.substr(a + 1, b - a - 1);
            std::string q;
            for (std::size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
                q.push_back(p[i]);
            }
            libs.push_back(std::filesystem::u8path(q));
        }
    }
#endif
    std::error_code ec;
    for (const auto& l : libs) {
        const auto cand = l / "steamapps" / "common" / "BeamNG.drive";
        if (std::filesystem::exists(cand / "gameengine.zip", ec) || std::filesystem::exists(cand / "content", ec)) return cand;
    }
    return {};
}

std::filesystem::path find_beamng_user_folder() {
#ifdef _WIN32
    wchar_t* la = nullptr;
    size_t n = 0;
    if (_wdupenv_s(&la, &n, L"LOCALAPPDATA") != 0 || !la) return {};
    const std::filesystem::path local = la;
    free(la);
    std::error_code ec;
    std::filesystem::path folder = local / "BeamNG" / "BeamNG.drive";
    std::ifstream ini(local / "BeamNG" / "BeamNG.drive.ini");
    std::string line;
    while (std::getline(ini, line)) {
        const auto k = line.find("userFolder");
        if (k == std::string::npos) continue;
        const auto eq = line.find('=', k);
        if (eq == std::string::npos) continue;
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '"')) v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '"' || v.back() == '\r')) v.pop_back();
        if (!v.empty()) folder = std::filesystem::u8path(v);
    }
    if (std::filesystem::is_directory(folder / "current", ec)) return folder / "current";
    return folder;
#else
    return {};
#endif
}

} // namespace bng
