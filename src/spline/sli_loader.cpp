#include "omsi/spline_types.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace omsi::spline {
namespace {

constexpr std::uint32_t kSlibMagic = 0x42494c53u; // "SLIB" little-endian
constexpr std::uint32_t kSlibVersion = 1;
constexpr std::size_t kSliCacheMax = 4096;

std::mutex g_sli_cache_mu;
// Negative cache: a missing/unparsable .sli used to hit the disk (stat + hash + read + parse) on EVERY
// call - and terrain restamps call load_sli_shared for every aligned spline of every tile. Remember the
// failure for a few seconds instead. Short TTL so a .sli created while the editor runs still shows up;
// invalidate_sli_cache() also drops it immediately. Guarded by g_sli_cache_mu.
constexpr std::chrono::seconds kSliMissTtl{8};
constexpr std::size_t kSliMissMax = 4096;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_sli_missing;

bool sli_recently_missing(const std::string& key) {
    std::lock_guard lock(g_sli_cache_mu);
    const auto it = g_sli_missing.find(key);
    if (it == g_sli_missing.end()) return false;
    if (std::chrono::steady_clock::now() < it->second) return true;
    g_sli_missing.erase(it);
    return false;
}

void sli_mark_missing(const std::string& key) {
    std::lock_guard lock(g_sli_cache_mu);
    if (g_sli_missing.size() >= kSliMissMax) g_sli_missing.clear();
    g_sli_missing[key] = std::chrono::steady_clock::now() + kSliMissTtl;
}

void sli_clear_missing(const std::string& key) {
    std::lock_guard lock(g_sli_cache_mu);
    g_sli_missing.erase(key);
}
struct SliCacheEntry {
    std::shared_ptr<const SplineDefinition> def;
    std::list<std::string>::iterator lru_it;
};
std::unordered_map<std::string, SliCacheEntry> g_sli_cache;
std::list<std::string> g_sli_lru;

std::atomic<std::uint64_t> g_slib_load_ns{0};
std::atomic<std::uint64_t> g_text_load_ns{0};
std::atomic<std::uint64_t> g_slib_hits{0};
std::atomic<std::uint64_t> g_text_hits{0};

bool slib_cache_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("OMSI_SLI_SLIB");
        return !(e && e[0] == '0');
    }();
    return on;
}

bool slib_stats_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("OMSI_SLI_STATS");
        return e && e[0] && e[0] != '0';
    }();
    return on;
}

void maybe_log_sli_stats() {
    if (!slib_stats_enabled()) return;
    const std::uint64_t sn = g_slib_hits.load();
    const std::uint64_t tn = g_text_hits.load();
    if (((sn + tn) % 256) != 0 || sn + tn == 0) return;
    const std::uint64_t s_ns = g_slib_load_ns.load();
    const std::uint64_t t_ns = g_text_load_ns.load();
    log::info("SLI_CACHE slib_n=" + std::to_string(sn) + " slib_avg_us=" +
              std::to_string(sn ? (s_ns / sn / 1000) : 0) + " text_n=" + std::to_string(tn) +
              " text_avg_us=" + std::to_string(tn ? (t_ns / tn / 1000) : 0));
}

std::string cache_key(const std::filesystem::path& p) {
    std::string s = p.lexically_normal().string();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::filesystem::path slib_path_for(const std::filesystem::path& sli_path) {
    auto out = sli_path;
    out.replace_extension(L".slib");
    return out;
}

inline std::string_view trim_view(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool ieq_view(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::optional<float> try_float(std::string_view s) {
    s = trim_view(s);
    if (s.empty()) return std::nullopt;
    float v = 0.f;
    const auto* first = s.data();
    const auto* last = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || ptr == first) return std::nullopt;
    return v;
}

std::shared_ptr<const SplineDefinition> cache_get(const std::string& key) {
    std::lock_guard lock(g_sli_cache_mu);
    const auto it = g_sli_cache.find(key);
    if (it == g_sli_cache.end()) return nullptr;
    g_sli_lru.splice(g_sli_lru.begin(), g_sli_lru, it->second.lru_it);
    return it->second.def;
}

void cache_put(const std::string& key, std::shared_ptr<const SplineDefinition> def) {
    if (!def) return;
    std::lock_guard lock(g_sli_cache_mu);
    if (const auto it = g_sli_cache.find(key); it != g_sli_cache.end()) {
        it->second.def = std::move(def);
        g_sli_lru.splice(g_sli_lru.begin(), g_sli_lru, it->second.lru_it);
        return;
    }
    while (g_sli_cache.size() >= kSliCacheMax && !g_sli_lru.empty()) {
        g_sli_cache.erase(g_sli_lru.back());
        g_sli_lru.pop_back();
    }
    g_sli_lru.push_front(key);
    g_sli_cache.emplace(key, SliCacheEntry{std::move(def), g_sli_lru.begin()});
}

void cache_erase(const std::string& key) {
    std::lock_guard lock(g_sli_cache_mu);
    const auto it = g_sli_cache.find(key);
    if (it == g_sli_cache.end()) return;
    g_sli_lru.erase(it->second.lru_it);
    g_sli_cache.erase(it);
}

bool read_u32(std::istream& in, std::uint32_t& v) {
    in.read(reinterpret_cast<char*>(&v), 4);
    return static_cast<bool>(in);
}

bool read_u64(std::istream& in, std::uint64_t& v) {
    in.read(reinterpret_cast<char*>(&v), 8);
    return static_cast<bool>(in);
}

bool read_f32(std::istream& in, float& v) {
    in.read(reinterpret_cast<char*>(&v), 4);
    return static_cast<bool>(in);
}

bool read_str(std::istream& in, std::string& s) {
    std::uint32_t n = 0;
    if (!read_u32(in, n) || n > 64 * 1024 * 1024) return false;
    s.resize(n);
    if (n) in.read(s.data(), static_cast<std::streamsize>(n));
    return static_cast<bool>(in);
}

void write_u32(std::ostream& out, std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
void write_u64(std::ostream& out, std::uint64_t v) { out.write(reinterpret_cast<const char*>(&v), 8); }
void write_f32(std::ostream& out, float v) { out.write(reinterpret_cast<const char*>(&v), 4); }
void write_str(std::ostream& out, const std::string& s) {
    write_u32(out, static_cast<std::uint32_t>(s.size()));
    if (!s.empty()) out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void write_slib_body(std::ostream& out, const SplineDefinition& def) {
    write_str(out, def.sli_path.string());
    write_f32(out, def.path_length);
    write_u32(out, def.only_editor ? 1u : 0u);

    write_u32(out, static_cast<std::uint32_t>(def.textures.size()));
    for (const auto& t : def.textures) write_str(out, t);
    write_u32(out, static_cast<std::uint32_t>(def.texture_alpha.size()));
    if (!def.texture_alpha.empty())
        out.write(reinterpret_cast<const char*>(def.texture_alpha.data()),
                  static_cast<std::streamsize>(def.texture_alpha.size()));
    write_u32(out, static_cast<std::uint32_t>(def.texture_extra.size()));
    for (const auto& ex : def.texture_extra) {
        const std::uint8_t b[3] = {ex.damage_surf ? 1u : 0u, ex.puddles ? 1u : 0u, ex.moisture ? 1u : 0u};
        out.write(reinterpret_cast<const char*>(b), 3);
    }

    write_u32(out, static_cast<std::uint32_t>(def.height_profiles.size()));
    for (const auto& hp : def.height_profiles) {
        write_f32(out, hp.left);
        write_f32(out, hp.right);
        write_f32(out, hp.height);
        write_f32(out, hp.skew);
    }

    write_u32(out, static_cast<std::uint32_t>(def.profiles.size()));
    for (std::size_t pi = 0; pi < def.profiles.size(); ++pi) {
        const auto& prof = def.profiles[pi];
        write_u32(out, static_cast<std::uint32_t>(prof.size()));
        for (const auto& pt : prof) {
            write_f32(out, pt.x);
            write_f32(out, pt.z);
            write_f32(out, pt.uv_x);
            write_f32(out, pt.y_tiling);
            write_u32(out, static_cast<std::uint32_t>(pt.material_id));
        }
        const std::string& cmt =
            pi < def.profile_comments.size() ? def.profile_comments[pi] : std::string{};
        write_str(out, cmt);
    }

    write_u32(out, static_cast<std::uint32_t>(def.paths.size()));
    for (const auto& p : def.paths) {
        write_u32(out, static_cast<std::uint32_t>(p.typ));
        write_f32(out, p.lateral);
        write_f32(out, p.height);
        write_f32(out, p.width);
        write_u32(out, static_cast<std::uint32_t>(p.direction));
        write_u32(out, p.secondary ? 1u : 0u);
    }

    write_u32(out, static_cast<std::uint32_t>(def.hole_profiles.size()));
    for (const auto& ring : def.hole_profiles) {
        write_u32(out, static_cast<std::uint32_t>(ring.size()));
        for (const auto& pt : ring) {
            write_f32(out, pt.x);
            write_f32(out, pt.y);
            write_f32(out, pt.z);
        }
    }

    write_u32(out, def.patchwork.valid ? 1u : 0u);
    if (def.patchwork.valid) {
        write_u32(out, static_cast<std::uint32_t>(def.patchwork.material_index));
        write_f32(out, def.patchwork.seg_length);
        write_str(out, def.patchwork.transitions);
        write_str(out, def.patchwork.factors);
        write_str(out, def.patchwork.inverse);
    }
}

bool read_slib_body(std::istream& in, SplineDefinition& def, const std::filesystem::path& fallback_path) {
    std::string path_s;
    if (!read_str(in, path_s)) return false;
    def.sli_path = path_s.empty() ? fallback_path : std::filesystem::path(path_s);
    if (!read_f32(in, def.path_length)) return false;
    std::uint32_t only_ed = 0;
    if (!read_u32(in, only_ed)) return false;
    def.only_editor = only_ed != 0;

    std::uint32_t n_tex = 0;
    if (!read_u32(in, n_tex)) return false;
    def.textures.clear();
    def.textures.reserve(n_tex);
    for (std::uint32_t i = 0; i < n_tex; ++i) {
        std::string t;
        if (!read_str(in, t)) return false;
        def.textures.push_back(std::move(t));
    }
    std::uint32_t n_alpha = 0;
    if (!read_u32(in, n_alpha)) return false;
    def.texture_alpha.resize(n_alpha);
    if (n_alpha) in.read(reinterpret_cast<char*>(def.texture_alpha.data()), static_cast<std::streamsize>(n_alpha));
    std::uint32_t n_extra = 0;
    if (!read_u32(in, n_extra)) return false;
    def.texture_extra.resize(n_extra);
    for (std::uint32_t i = 0; i < n_extra; ++i) {
        std::uint8_t b[3]{};
        in.read(reinterpret_cast<char*>(b), 3);
        def.texture_extra[i].damage_surf = b[0] != 0;
        def.texture_extra[i].puddles = b[1] != 0;
        def.texture_extra[i].moisture = b[2] != 0;
    }

    std::uint32_t n_hp = 0;
    if (!read_u32(in, n_hp)) return false;
    def.height_profiles.resize(n_hp);
    for (std::uint32_t i = 0; i < n_hp; ++i) {
        if (!read_f32(in, def.height_profiles[i].left) || !read_f32(in, def.height_profiles[i].right) ||
            !read_f32(in, def.height_profiles[i].height) || !read_f32(in, def.height_profiles[i].skew))
            return false;
    }

    std::uint32_t n_prof = 0;
    if (!read_u32(in, n_prof)) return false;
    def.profiles.resize(n_prof);
    def.profile_comments.resize(n_prof);
    for (std::uint32_t pi = 0; pi < n_prof; ++pi) {
        std::uint32_t n_pts = 0;
        if (!read_u32(in, n_pts)) return false;
        auto& prof = def.profiles[pi];
        prof.resize(n_pts);
        for (std::uint32_t j = 0; j < n_pts; ++j) {
            std::uint32_t mid = 0;
            if (!read_f32(in, prof[j].x) || !read_f32(in, prof[j].z) || !read_f32(in, prof[j].uv_x) ||
                !read_f32(in, prof[j].y_tiling) || !read_u32(in, mid))
                return false;
            prof[j].material_id = static_cast<std::int32_t>(mid);
        }
        if (!read_str(in, def.profile_comments[pi])) return false;
    }

    std::uint32_t n_paths = 0;
    if (!read_u32(in, n_paths)) return false;
    def.paths.resize(n_paths);
    for (std::uint32_t i = 0; i < n_paths; ++i) {
        std::uint32_t typ = 0, dir = 0, sec = 0;
        if (!read_u32(in, typ) || !read_f32(in, def.paths[i].lateral) || !read_f32(in, def.paths[i].height) ||
            !read_f32(in, def.paths[i].width) || !read_u32(in, dir) || !read_u32(in, sec))
            return false;
        def.paths[i].typ = static_cast<std::int32_t>(typ);
        def.paths[i].direction = static_cast<std::int32_t>(dir);
        def.paths[i].secondary = sec != 0;
    }

    std::uint32_t n_holes = 0;
    if (!read_u32(in, n_holes)) return false;
    def.hole_profiles.resize(n_holes);
    for (std::uint32_t hi = 0; hi < n_holes; ++hi) {
        std::uint32_t n_pts = 0;
        if (!read_u32(in, n_pts)) return false;
        auto& ring = def.hole_profiles[hi];
        ring.resize(n_pts);
        for (std::uint32_t j = 0; j < n_pts; ++j) {
            if (!read_f32(in, ring[j].x) || !read_f32(in, ring[j].y) || !read_f32(in, ring[j].z)) return false;
        }
    }

    std::uint32_t pw_valid = 0;
    if (!read_u32(in, pw_valid)) return false;
    def.patchwork = {};
    if (pw_valid) {
        std::uint32_t mi = 0;
        if (!read_u32(in, mi) || !read_f32(in, def.patchwork.seg_length) ||
            !read_str(in, def.patchwork.transitions) || !read_str(in, def.patchwork.factors) ||
            !read_str(in, def.patchwork.inverse))
            return false;
        def.patchwork.material_index = static_cast<std::int32_t>(mi);
        def.patchwork.valid = true;
    }
    return static_cast<bool>(in);
}

std::shared_ptr<const SplineDefinition> try_load_slib(const std::filesystem::path& sli_path,
                                                    const std::filesystem::path& slib_path, std::uintmax_t sli_size,
                                                    std::filesystem::file_time_type sli_mtime, std::uint64_t sli_hash) {
    std::ifstream in(slib_path, std::ios::binary);
    if (!in) return nullptr;
    std::uint32_t magic = 0;
    if (!read_u32(in, magic) || magic != kSlibMagic) return nullptr;
    std::uint32_t version = 0;
    if (!read_u32(in, version) || version != kSlibVersion) return nullptr;
    std::uint64_t stored_mtime = 0, stored_size = 0, stored_hash = 0;
    if (!read_u64(in, stored_mtime) || !read_u64(in, stored_size) || !read_u64(in, stored_hash)) return nullptr;
    if (stored_size != sli_size) return nullptr;
    if (static_cast<std::uint64_t>(sli_mtime.time_since_epoch().count()) != stored_mtime) return nullptr;
    if (stored_hash != sli_hash) return nullptr;

    auto def = std::make_shared<SplineDefinition>();
    if (!read_slib_body(in, *def, sli_path)) return nullptr;
    return def;
}

void write_slib_atomic(const std::filesystem::path& slib_path, const SplineDefinition& def,
                       std::filesystem::file_time_type mtime, std::uintmax_t size, std::uint64_t hash) {
    const auto tmp = slib_path.parent_path() / (slib_path.filename().wstring() + L".tmp");
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    write_u32(out, kSlibMagic);
    write_u32(out, kSlibVersion);
    const std::uint64_t mt = static_cast<std::uint64_t>(mtime.time_since_epoch().count());
    write_u64(out, mt);
    write_u64(out, static_cast<std::uint64_t>(size));
    write_u64(out, hash);
    write_slib_body(out, def);
    out.close();
    std::error_code ec;
    std::filesystem::rename(tmp, slib_path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

std::shared_ptr<const SplineDefinition> parse_sli_text(const std::filesystem::path& sli_path,
                                                       const cfg::CfgFileBuffer& buf) {
    if (buf.lines.empty()) return nullptr;

    auto def = std::make_shared<SplineDefinition>();
    def->sli_path = sli_path;

    std::string_view tag;
    std::optional<std::string> pending_profile_comment;
    std::int32_t cur_mat = 0;
    const auto& lines = buf.lines;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = trim_view(lines[i]);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            tag = line;
            if (ieq_view(tag, "[onlyeditor]") || ieq_view(tag, "[only_editor]")) def->only_editor = true;
            if (ieq_view(tag, "[terrainholeprofile]")) {
                def->hole_profiles.push_back({});
                tag = {};
            }
            continue;
        }

        if (tag.empty() && line.size() >= 7 && line.rfind("###", 0) == 0) {
            const auto end = line.rfind("###");
            if (end > 3) {
                pending_profile_comment = std::string(trim_view(line.substr(3, end - 3)));
                continue;
            }
        }

        if (ieq_view(tag, "[heightprofile]")) {
            HeightProfile hp{};
            if (auto v = try_float(line)) {
                hp.left = *v;
                if (i + 3 < lines.size()) {
                    if (auto r = try_float(trim_view(lines[i + 1]))) hp.right = *r;
                    if (auto h = try_float(trim_view(lines[i + 2]))) hp.height = *h;
                    if (auto s = try_float(trim_view(lines[i + 3]))) hp.skew = *s;
                    def->height_profiles.push_back(hp);
                    i += 3;
                }
            }
            tag = {};
        } else if (ieq_view(tag, "[profile]")) {
            if (auto v = try_float(line)) cur_mat = static_cast<std::int32_t>(*v);
            def->profiles.push_back({});
            def->profile_comments.push_back(pending_profile_comment.value_or(""));
            pending_profile_comment.reset();
            tag = {};
        } else if (ieq_view(tag, "[profilepnt]")) {
            if (def->profiles.empty()) {
                def->profiles.push_back({});
                def->profile_comments.push_back(pending_profile_comment.value_or(""));
                pending_profile_comment.reset();
            }
            ProfilePoint pt{};
            pt.material_id = cur_mat;
            if (auto x = try_float(line)) {
                pt.x = *x;
                if (i + 3 < lines.size()) {
                    if (auto z = try_float(trim_view(lines[i + 1]))) pt.z = *z;
                    if (auto u = try_float(trim_view(lines[i + 2]))) pt.uv_x = *u;
                    if (auto t = try_float(trim_view(lines[i + 3]))) pt.y_tiling = *t;
                    def->profiles.back().push_back(pt);
                    i += 3;
                }
            }
            tag = {};
        } else if (ieq_view(tag, "[texture]")) {
            def->textures.emplace_back(line);
            def->texture_alpha.push_back(0);
            tag = {};
        } else if (ieq_view(tag, "[matl_alpha]")) {
            int mode = 0;
            if (auto v = try_float(line)) mode = static_cast<int>(*v);
            if (mode < 0) mode = 0;
            if (mode > 2) mode = 2;
            if (!def->texture_alpha.empty())
                def->texture_alpha.back() = static_cast<std::uint8_t>(mode);
            else if (!def->textures.empty()) {
                def->texture_alpha.assign(def->textures.size(), 0);
                def->texture_alpha.back() = static_cast<std::uint8_t>(mode);
            }
            tag = {};
        } else if (ieq_view(tag, "[path]") || ieq_view(tag, "[path_2]")) {
            SliPath p{};
            p.secondary = ieq_view(tag, "[path_2]");
            if (auto t = try_float(line)) {
                p.typ = static_cast<std::int32_t>(*t);
                std::vector<float> vals{*t};
                std::size_t j = i + 1;
                while (j < lines.size() && vals.size() < 5) {
                    const auto tln = trim_view(lines[j]);
                    if (tln.empty()) {
                        ++j;
                        continue;
                    }
                    if (tln.front() == '[') break;
                    if (auto n = try_float(tln)) vals.push_back(*n);
                    else break;
                    ++j;
                }
                if (vals.size() >= 3) {
                    p.lateral = vals[1];
                    p.height = vals[2];
                    if (vals.size() >= 5) {
                        p.width = vals[3];
                        p.direction = static_cast<std::int32_t>(vals[4]);
                    } else if (vals.size() >= 4)
                        p.direction = static_cast<std::int32_t>(vals[3]);
                    def->paths.push_back(p);
                }
                i = j - 1;
            }
            tag = {};
        } else if (ieq_view(tag, "[patchwork_chain]")) {
            PatchworkChain pw{};
            pw.material_index =
                def->textures.empty() ? -1 : static_cast<std::int32_t>(def->textures.size()) - 1;
            if (auto seg = try_float(line)) {
                pw.seg_length = *seg;
                std::size_t j = i + 1;
                auto next_nonempty = [&](std::string& out) -> bool {
                    while (j < lines.size()) {
                        const auto tln = trim_view(lines[j++]);
                        if (tln.empty()) continue;
                        if (!tln.empty() && tln.front() == '[') {
                            --j;
                            return false;
                        }
                        out.assign(tln.begin(), tln.end());
                        return true;
                    }
                    return false;
                };
                if (next_nonempty(pw.transitions) && next_nonempty(pw.factors) && next_nonempty(pw.inverse)) {
                    if (pw.transitions.size() >= 2 && pw.factors.size() + 1 == pw.transitions.size() &&
                        pw.inverse.size() + 1 == pw.transitions.size()) {
                        pw.valid = pw.material_index >= 0 && pw.seg_length > 1.e-3f;
                    }
                }
                if (pw.valid) def->patchwork = std::move(pw);
                i = j - 1;
            }
            tag = {};
        } else if (ieq_view(tag, "[terrainholeprofilepnt]")) {
            if (def->hole_profiles.empty()) def->hole_profiles.push_back({});
            if (auto x = try_float(line)) {
                HoleProfilePoint pt{};
                pt.x = *x;
                if (i + 2 < lines.size()) {
                    if (auto y = try_float(trim_view(lines[i + 1]))) pt.y = *y;
                    if (auto z = try_float(trim_view(lines[i + 2]))) pt.z = *z;
                    def->hole_profiles.back().push_back(pt);
                    i += 2;
                }
            }
            tag = {};
        } else if (ieq_view(tag, "[length]")) {
            if (auto v = try_float(line)) def->path_length = *v;
            tag = {};
        }
    }

    log::info("sli load " + sli_path.filename().string() + " paths=" + std::to_string(def->paths.size()) +
              " profiles=" + std::to_string(def->profiles.size()) +
              " holes=" + std::to_string(def->hole_profiles.size()) +
              (def->patchwork.valid ? (" patchwork seg=" + std::to_string(def->patchwork.seg_length)) : ""));
    return def;
}

} // namespace

void invalidate_sli_cache(const std::filesystem::path& sli_path) {
    if (sli_path.empty()) {
        std::lock_guard lock(g_sli_cache_mu);
        g_sli_cache.clear();
        g_sli_lru.clear();
        g_sli_missing.clear();
        return;
    }
    sli_clear_missing(cache_key(sli_path));
    cache_erase(cache_key(sli_path));
    if (slib_cache_enabled()) {
        std::error_code ec;
        std::filesystem::remove(slib_path_for(sli_path), ec);
    }
}

std::optional<std::size_t> peek_sli_path_count(const std::filesystem::path& sli_path) {
    if (auto sp = cache_get(cache_key(sli_path))) return sp->paths.size();
    return std::nullopt;
}

std::shared_ptr<const SplineDefinition> load_sli_shared(const std::filesystem::path& sli_path) {
    const auto key = cache_key(sli_path);
    if (auto hit = cache_get(key)) return hit;
    if (sli_recently_missing(key)) return nullptr;

    std::error_code ec_time, ec_size;
    const auto sli_mtime = std::filesystem::last_write_time(sli_path, ec_time);
    const auto sli_size = std::filesystem::file_size(sli_path, ec_size);
    const std::uint64_t sli_hash = cfg::fnv1a_file_bytes(sli_path);
    const auto slib_path = slib_path_for(sli_path);

    const auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<const SplineDefinition> def;
    if (slib_cache_enabled() && !ec_time && !ec_size) {
        def = try_load_slib(sli_path, slib_path, sli_size, sli_mtime, sli_hash);
        if (def) {
            const auto t1 = std::chrono::steady_clock::now();
            g_slib_load_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            ++g_slib_hits;
            maybe_log_sli_stats();
            cache_put(key, def);
            if (auto again = cache_get(key)) return again;
            return def;
        }
    }

    const auto buf = cfg::read_cfg_file_buffer(sli_path);
    def = parse_sli_text(sli_path, buf);
    const auto t2 = std::chrono::steady_clock::now();
    if (def) {
        g_text_load_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t0).count();
        ++g_text_hits;
        maybe_log_sli_stats();
        if (slib_cache_enabled() && !ec_time && !ec_size) write_slib_atomic(slib_path, *def, sli_mtime, sli_size, sli_hash);
    }

    if (!def) {
        sli_mark_missing(key);
        return nullptr;
    }
    sli_clear_missing(key);
    cache_put(key, def);
    if (auto again = cache_get(key)) return again;
    return def;
}

std::optional<SplineDefinition> load_sli(const std::filesystem::path& sli_path) {
    const auto sp = load_sli_shared(sli_path);
    if (!sp) return std::nullopt;
    return *sp;
}

} // namespace omsi::spline
