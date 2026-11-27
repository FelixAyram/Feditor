#include <unordered_map>
#include "omsi/vfs_overlay.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/texture/internal.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"
#include "omsi/texture_season.hpp"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omsi::gfx::dx12 {
namespace {

bool ieq_ascii_path(const std::filesystem::path& name, const wchar_t* lit) {
    const auto w = name.wstring();
    const auto n = std::wcslen(lit);
    if (w.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (::towlower(w[i]) != ::towlower(lit[i])) return false;
    }
    return true;
}

// .sco/.sli → parent; .o3d → walk up to owner with texture/ (vehicle root, sco dir, …).
std::filesystem::path texture_owner_dir(const std::filesystem::path& base_file_path) {
    const auto dir = base_file_path.parent_path();
    const auto ext = base_file_path.extension().wstring();
    auto is_ext = [&](const wchar_t* e3) {
        return ext.size() == 4 && ext[0] == L'.' && ::towlower(ext[1]) == e3[0] && ::towlower(ext[2]) == e3[1] &&
               ::towlower(ext[3]) == e3[2];
    };
    if (is_ext(L"sco") || is_ext(L"sli")) return dir;

    for (auto p = dir; !p.empty(); p = p.parent_path()) {
        if (texture_detail::path_exists(p / L"texture") || texture_detail::path_exists(p / L"Texture")) return p;
        if (ieq_ascii_path(p.filename(), L"model")) return p.parent_path();
        if (!p.has_parent_path() || p.parent_path() == p) break;
    }
    return dir;
}

std::optional<std::filesystem::path> try_one_tex_dir(const std::filesystem::path& tex_dir,
                                                     const std::filesystem::path& filename) {
    if (tex_dir.empty() || filename.empty()) return std::nullopt;
    const auto stem = filename.stem().wstring();
    const auto dds = (tex_dir / (stem + L".dds")).lexically_normal();
    if (texture_detail::path_exists(dds)) return dds;
    const auto orig = (tex_dir / filename).lexically_normal();
    if (texture_detail::path_exists(orig)) return orig;
    return std::nullopt;
}

// Albedo resolve: season folders + base texture\. Never texture\night (OMSI nightmaps).
std::optional<std::filesystem::path> try_texture_folder_albedo(const std::filesystem::path& owner_dir,
                                                               const std::filesystem::path& filename,
                                                               bool use_season) {
    if (owner_dir.empty() || filename.empty()) return std::nullopt;
    std::string season;
    if (use_season) {
        std::lock_guard<std::mutex> lk(omsi::gfx::texture_season_mu());
        season = omsi::gfx::texture_season_state().season_dir;
    }
    const auto chain = use_season ? omsi::gfx::season_folder_chain(season) : std::vector<std::string>{};
    for (const wchar_t* folder : {L"texture", L"Texture"}) {
        const auto tex_root = owner_dir / folder;
        for (const auto& sub : chain) {
            if (sub.empty()) continue;
            if (auto hit = try_one_tex_dir(tex_root / sub, filename)) return hit;
        }
        if (auto hit = try_one_tex_dir(tex_root, filename)) return hit;
    }
    return std::nullopt;
}

std::filesystem::path guess_omsi_root(const std::filesystem::path& from) {
    std::filesystem::path p = from;
    for (int i = 0; i < 16; ++i) {
        if (texture_detail::path_exists(p / "texture") && texture_detail::path_exists(p / "maps")) return p;
        const auto parent = p.parent_path();
        if (parent.empty() || parent == p) break;
        p = parent;
    }
    return {};
}

std::optional<std::filesystem::path> resolve_texture_path_uncached(const std::filesystem::path& base_file_path,
                                                                   std::string_view texture_name,
                                                                   const std::filesystem::path& omsi_root,
                                                                   bool use_season);

// [PERF] The answer per (folder + extension of the base file, texture name, root, season): each lookup is
// several file-system probes (season folders, texture/Texture, .dds/original), and on an HDD the main
// thread waited for them while creating every streamed object (resolve.cpp path_exists stalls). The OMSI
// content does not change while the editor runs; the season is part of the key.
std::optional<std::filesystem::path> resolve_texture_path_impl(const std::filesystem::path& base_file_path,
                                                               std::string_view texture_name,
                                                               const std::filesystem::path& omsi_root,
                                                               bool use_season) {
    if (texture_name.empty() || base_file_path.empty()) return std::nullopt;
    if (omsi::vfs::active()) return resolve_texture_path_uncached(base_file_path, texture_name, omsi_root, use_season);
    std::wstring key = base_file_path.parent_path().native();
    key += L'|';
    key += base_file_path.extension().native();
    key += L'|';
    key.append(texture_name.begin(), texture_name.end());
    key += L'|';
    key += omsi_root.native();
    key += L'|';
    if (use_season) {
        std::lock_guard<std::mutex> lk(omsi::gfx::texture_season_mu());
        const auto& sd = omsi::gfx::texture_season_state().season_dir;
        key.append(sd.begin(), sd.end());
    } else {
        key += L'-';
    }
    static std::mutex s_mu;
    static std::unordered_map<std::wstring, std::optional<std::filesystem::path>> s_cache;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        if (const auto it = s_cache.find(key); it != s_cache.end()) return it->second;
    }
    auto out = resolve_texture_path_uncached(base_file_path, texture_name, omsi_root, use_season);
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_cache.size() > 300000) s_cache.clear();
    s_cache.emplace(std::move(key), out);
    return out;
}

std::optional<std::filesystem::path> resolve_texture_path_uncached(const std::filesystem::path& base_file_path,
                                                                   std::string_view texture_name,
                                                                   const std::filesystem::path& omsi_root,
                                                                   bool use_season) {
    if (texture_name.empty() || base_file_path.empty()) return std::nullopt;

    {
        std::error_code ec;
        const std::filesystem::path abs{std::string(texture_name)};
        if (abs.is_absolute() && omsi::vfs::exists(abs, ec)) return abs.lexically_normal();
    }

    const auto rel = texture_detail::texture_path_from_name(texture_name);
    const auto filename = rel.filename();
    if (filename.empty()) return std::nullopt;

    if (auto hit = try_texture_folder_albedo(texture_owner_dir(base_file_path), filename, use_season)) return hit;
    {
        const auto owner = texture_owner_dir(base_file_path);
        const auto beside = (owner / filename).lexically_normal();
        if (texture_detail::path_exists(beside)) return beside;
        const auto dds = (owner / (filename.stem().wstring() + L".dds")).lexically_normal();
        if (texture_detail::path_exists(dds)) return dds;
    }
    if (rel.has_parent_path()) {
        if (auto hit = try_texture_folder_albedo(texture_owner_dir(base_file_path), rel, use_season)) return hit;
    }

    auto root = omsi_root;
    if (root.empty()) root = guess_omsi_root(base_file_path);
    if (!root.empty()) {
        if (auto hit = try_texture_folder_albedo(root, filename, use_season)) return hit;
    }
    return std::nullopt;
}

} // namespace

std::optional<std::filesystem::path> resolve_season_rebind(const std::filesystem::path& current_resolved) {
    if (current_resolved.empty()) return std::nullopt;
    const auto filename = current_resolved.filename();
    if (filename.empty()) return std::nullopt;
    auto dir = current_resolved.parent_path();
    auto is_season_or_night = [](const std::filesystem::path& name) {
        return ieq_ascii_path(name, L"night") || ieq_ascii_path(name, L"Night") ||
               ieq_ascii_path(name, L"Spring") || ieq_ascii_path(name, L"Fall") ||
               ieq_ascii_path(name, L"Winter") || ieq_ascii_path(name, L"WinterSnow") ||
               ieq_ascii_path(name, L"WinterSnowfall") || ieq_ascii_path(name, L"SummerDry") ||
               ieq_ascii_path(name, L"Summer");
    };
    // A night map (texture\night\...) stays: peeled and re-resolved it became the day albedo, and that added at
    // night lit everything up after a month change in the dark.
    if (ieq_ascii_path(dir.filename(), L"night")) return std::nullopt;
    // Peel season so we re-resolve from owner; never pick night for albedo.
    if (is_season_or_night(dir.filename())) dir = dir.parent_path();
    if (ieq_ascii_path(dir.filename(), L"texture") || ieq_ascii_path(dir.filename(), L"Texture")) {
        return try_texture_folder_albedo(dir.parent_path(), filename, true);
    }
    return try_texture_folder_albedo(dir, filename, true);
}

std::optional<std::filesystem::path> resolve_texture_path(const std::filesystem::path& base_file_path,
                                                          std::string_view texture_name,
                                                          const std::filesystem::path& omsi_root) {
    return resolve_texture_path_impl(base_file_path, texture_name, omsi_root, true);
}

std::optional<std::filesystem::path> resolve_texture_path_day(const std::filesystem::path& base_file_path,
                                                              std::string_view texture_name,
                                                              const std::filesystem::path& omsi_root) {
    // Catalog thumbs / fixed previews: base texture\ only (no season, no night).
    return resolve_texture_path_impl(base_file_path, texture_name, omsi_root, false);
}

} // namespace omsi::gfx::dx12
