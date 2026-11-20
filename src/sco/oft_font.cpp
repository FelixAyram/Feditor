#include "omsi/sco_types.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include "gfx/dx12/dx12_texture.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omsi::sco {
namespace {

// path::string() throws ("no mapping for Unicode character") when the name has characters outside the
// ANSI code page; it killed font/text-texture jobs. UTF-8 never throws.
std::string path_utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::uint32_t utf8_next(std::string_view& s) {
    if (s.empty()) return 0;
    const auto c0 = static_cast<unsigned char>(s[0]);
    if (c0 < 0x80u) {
        s.remove_prefix(1);
        return c0;
    }
    if ((c0 & 0xE0u) == 0xC0u && s.size() >= 2) {
        const std::uint32_t cp =
            (static_cast<std::uint32_t>(c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[1]) & 0x3Fu);
        s.remove_prefix(2);
        return cp;
    }
    if ((c0 & 0xF0u) == 0xE0u && s.size() >= 3) {
        const std::uint32_t cp = (static_cast<std::uint32_t>(c0 & 0x0Fu) << 12) |
                                 (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1]) & 0x3Fu) << 6) |
                                 (static_cast<unsigned char>(s[2]) & 0x3Fu);
        s.remove_prefix(3);
        return cp;
    }
    s.remove_prefix(1);
    return c0;
}

std::uint32_t utf8_first(std::string_view s) {
    return utf8_next(s);
}

struct OftGlyph {
    int x0 = 0;
    int x1 = 0;
    int y0 = 0;
};

struct OftFont {
    std::string name;
    std::filesystem::path oft_path;
    std::string color_file;
    std::string alpha_file;
    int height = 16;
    int gap = 0;
    gfx::dx12::CpuRgbaImage color;
    gfx::dx12::CpuRgbaImage alpha;
    bool images_loaded = false;
    bool images_loading = false;
    std::unordered_map<std::uint32_t, OftGlyph> glyphs;
};

std::optional<OftFont> parse_oft_meta(const std::filesystem::path& oft_path) {
    const auto lines = cfg::read_cfg_lines(oft_path);
    if (lines.empty()) return std::nullopt;

    OftFont font{};
    font.oft_path = oft_path;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto tok = lines[i];
        if (tok.empty() || tok.front() != '[') continue;
        const auto tag = trim(tok);
        if (ieq(tag, "[newfont]")) {
            auto next = [&]() -> std::string {
                for (++i; i < lines.size(); ++i) {
                    const auto t = trim(lines[i]);
                    if (!t.empty()) return t;
                }
                return {};
            };
            font.name = next();
            font.color_file = next();
            font.alpha_file = next();
            try {
                font.height = std::stoi(next());
            } catch (...) {
                font.height = 16;
            }
            try {
                font.gap = std::stoi(next());
            } catch (...) {
                font.gap = 0;
            }
            continue;
        }
        if (ieq(tag, "[char]")) {
            if (i + 1 >= lines.size()) continue;
            ++i;
            std::string ch = lines[i];
            if (!ch.empty() && ch.back() == '\r') ch.pop_back();
            if (trim(ch).empty()) ch = " ";
            else ch = trim(ch);
            auto next_num = [&]() -> int {
                for (++i; i < lines.size(); ++i) {
                    const auto t = trim(lines[i]);
                    if (t.empty()) continue;
                    try {
                        return std::stoi(t);
                    } catch (...) {
                        return 0;
                    }
                }
                return 0;
            };
            OftGlyph g{};
            g.x0 = next_num();
            g.x1 = next_num();
            g.y0 = next_num();
            const std::uint32_t cp = utf8_first(ch);
            if (cp != 0) font.glyphs[cp] = g;
        }
    }
    if (font.name.empty() && font.glyphs.empty()) return std::nullopt;
    if (font.height < 1) font.height = 16;
    return font;
}

std::mutex g_oft_mu;
std::unordered_map<std::string, std::shared_ptr<OftFont>> g_oft_by_name;
std::unordered_set<std::string> g_oft_missing; // root|font — negative cache
bool g_oft_indexed = false;

void ensure_oft_images(OftFont& font) {
    {
        std::lock_guard lock(g_oft_mu);
        if (font.images_loaded) return;
        font.images_loading = true;
    }
    const auto dir = font.oft_path.parent_path();
    auto load_bmp = [&](const std::string& rel) -> gfx::dx12::CpuRgbaImage {
        if (rel.empty()) return {};
        auto img = gfx::dx12::load_image_cpu_rgba(dir / rel);
        if (!img) img = gfx::dx12::load_image_cpu_rgba(dir / std::filesystem::path(rel).filename());
        return img ? std::move(*img) : gfx::dx12::CpuRgbaImage{};
    };
    auto color = load_bmp(font.color_file);
    auto alpha = load_bmp(font.alpha_file);
    if (alpha.rgba.empty()) alpha = color;
    {
        std::lock_guard lock(g_oft_mu);
        font.color = std::move(color);
        font.alpha = std::move(alpha);
        font.images_loaded = true;
        font.images_loading = false;
    }
    log::info("oft load name=\"" + font.name + "\" file=" + path_utf8(font.oft_path.filename()) +
              " glyphs=" + std::to_string(font.glyphs.size()) + " h=" + std::to_string(font.height) +
              " gap=" + std::to_string(font.gap) + " color=" + std::to_string(font.color.width) + "x" +
              std::to_string(font.color.height) + " alpha=" + std::to_string(font.alpha.width) + "x" +
              std::to_string(font.alpha.height));
}

std::mutex g_oft_job_mu;
std::condition_variable g_oft_job_cv;
std::deque<std::shared_ptr<OftFont>> g_oft_jobs;
std::thread g_oft_worker;
bool g_oft_worker_started = false;

void ensure_oft_image_worker() {
    if (g_oft_worker_started) return;
    g_oft_worker_started = true;
    g_oft_worker = std::thread([] {
        for (;;) {
            std::shared_ptr<OftFont> font;
            {
                std::unique_lock lk(g_oft_job_mu);
                g_oft_job_cv.wait(lk, [] { return !g_oft_jobs.empty(); });
                font = std::move(g_oft_jobs.front());
                g_oft_jobs.pop_front();
            }
            if (font) ensure_oft_images(*font);
        }
    });
    g_oft_worker.detach();
}

void enqueue_oft_images(const std::shared_ptr<OftFont>& font) {
    if (!font) return;
    {
        std::lock_guard lock(g_oft_mu);
        if (font->images_loaded || font->images_loading) return;
        font->images_loading = true;
    }
    {
        std::lock_guard lk(g_oft_job_mu);
        ensure_oft_image_worker();
        g_oft_jobs.push_back(font);
    }
    g_oft_job_cv.notify_one();
}

std::string oft_cache_key(const std::filesystem::path& omsi_root, std::string_view font_name) {
    return lower_ascii(path_utf8(omsi_root)) + "|" + lower_ascii(std::string(font_name));
}

bool is_placeholder_oft_name(std::string_view name) {
    return name.find("oft-Datei") != std::string_view::npos ||
           name.find("wie in der oft") != std::string_view::npos;
}

// OMSI .sco often references FontName_Disp while Fonts\FontName.oft declares [newfont] FontName.
std::vector<std::string> oft_lookup_names(std::string_view name) {
    std::vector<std::string> out;
    auto add = [&](std::string_view n) {
        if (n.empty()) return;
        const std::string s(n);
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
    add(name);
    const std::string s(name);
    static constexpr const char* k_suffixes[] = {"_Disp", "_disp", "_DISP", "_Display", "_display", "_DISPLAY"};
    for (const char* suf : k_suffixes) {
        const std::size_t sl = std::strlen(suf);
        if (s.size() > sl && ieq(s.substr(s.size() - sl), suf)) {
            add(s.substr(0, s.size() - sl));
            break;
        }
    }
    return out;
}

void warn_missing_oft_once(std::string_view font_name, const std::filesystem::path& omsi_root) {
    if (is_placeholder_oft_name(font_name)) return;
    static std::mutex warn_mu;
    static std::unordered_set<std::string> warned;
    const auto key = oft_cache_key(omsi_root, font_name);
    std::lock_guard lock(warn_mu);
    if (!warned.insert(key).second) return;
    log::warn("oft missing font=\"" + std::string(font_name) + "\" root=" + path_utf8(omsi_root));
}

void cache_font(const std::shared_ptr<OftFont>& font) {
    if (!font) return;
    const auto name_key = lower_ascii(font->name);
    const auto stem_key = lower_ascii(path_utf8(font->oft_path.stem()));
    if (!name_key.empty()) g_oft_by_name.emplace(name_key, font);
    if (!stem_key.empty() && stem_key != name_key) g_oft_by_name.emplace(stem_key, font);
}

void index_fonts_dir(const std::filesystem::path& fonts_dir) {
    std::error_code ec;
    if (fonts_dir.empty() || !std::filesystem::exists(fonts_dir, ec)) return;
    for (const auto& ent : std::filesystem::directory_iterator(fonts_dir, ec)) {
        if (ec) break;
        if (!ent.is_regular_file(ec)) continue;
        auto ext = path_utf8(ent.path().extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".oft") continue;
        const auto stem_key = lower_ascii(path_utf8(ent.path().stem()));
        if (!stem_key.empty() && g_oft_by_name.find(stem_key) != g_oft_by_name.end()) continue;
        auto loaded = parse_oft_meta(ent.path());
        if (!loaded) continue;
        cache_font(std::make_shared<OftFont>(std::move(*loaded)));
    }
}

std::shared_ptr<OftFont> load_font_file(const std::filesystem::path& oft_path) {
    auto loaded = parse_oft_meta(oft_path);
    if (!loaded) return nullptr;
    auto font = std::make_shared<OftFont>(std::move(*loaded));
    std::lock_guard lock(g_oft_mu);
    // Another worker may have loaded the same file meanwhile: keep the cached one.
    const auto name_key = lower_ascii(font->name);
    const auto stem_key = lower_ascii(path_utf8(font->oft_path.stem()));
    if (const auto it = g_oft_by_name.find(name_key); !name_key.empty() && it != g_oft_by_name.end()) return it->second;
    if (const auto it = g_oft_by_name.find(stem_key); !stem_key.empty() && it != g_oft_by_name.end()) return it->second;
    cache_font(font);
    return font;
}

std::shared_ptr<OftFont> find_font_impl(const std::filesystem::path& omsi_root, std::string_view name) {
    if (name.empty()) return nullptr;

    const auto names = oft_lookup_names(name);
    const auto req_key = lower_ascii(std::string(name));

    {
        std::lock_guard lock(g_oft_mu);
        if (g_oft_missing.count(oft_cache_key(omsi_root, name))) return nullptr;
    }

    std::shared_ptr<OftFont> font;
    for (const auto& try_name : names) {
        const auto key = lower_ascii(try_name);
        {
            std::lock_guard lock(g_oft_mu);
            if (const auto it = g_oft_by_name.find(key); it != g_oft_by_name.end()) {
                font = it->second;
                if (!req_key.empty() && req_key != key) g_oft_by_name.emplace(req_key, font);
                break;
            }
        }
        if (font) break;

        const auto fonts_dir = omsi_root / "Fonts";
        const auto direct = fonts_dir / (try_name + ".oft");
        std::error_code ec;
        if (!direct.empty() && std::filesystem::exists(direct, ec) && !ec)
            font = load_font_file(direct);
        if (font) {
            std::lock_guard lock(g_oft_mu);
            if (!req_key.empty()) g_oft_by_name.emplace(req_key, font);
            break;
        }
    }

    if (!font) {
        const auto fonts_dir = omsi_root / "Fonts";
        std::lock_guard lock(g_oft_mu);
        if (!g_oft_indexed) {
            index_fonts_dir(fonts_dir);
            g_oft_indexed = true;
        }
        for (const auto& try_name : names) {
            const auto key = lower_ascii(try_name);
            if (const auto it = g_oft_by_name.find(key); it != g_oft_by_name.end()) {
                font = it->second;
                if (!req_key.empty() && req_key != key) g_oft_by_name.emplace(req_key, font);
                break;
            }
        }
    }

    if (!font) {
        std::lock_guard lock(g_oft_mu);
        g_oft_missing.insert(oft_cache_key(omsi_root, name));
        return nullptr;
    }
    return font;
}

const OftFont* find_font(const std::filesystem::path& omsi_root, std::string_view name) {
    return find_font_impl(omsi_root, name).get(); // cached entries are never replaced
}

std::shared_ptr<OftFont> find_font_shared(const std::filesystem::path& omsi_root, std::string_view name) {
    return find_font_impl(omsi_root, name);
}

std::uint8_t sample_lum(const gfx::dx12::CpuRgbaImage& img, int x, int y) {
    if (img.rgba.empty() || img.width == 0 || img.height == 0) return 0;
    if (x < 0 || y < 0 || x >= static_cast<int>(img.width) || y >= static_cast<int>(img.height)) return 0;
    const std::size_t o = (static_cast<std::size_t>(y) * img.width + static_cast<std::size_t>(x)) * 4u;
    const int r = img.rgba[o];
    const int g = img.rgba[o + 1];
    const int b = img.rgba[o + 2];
    return static_cast<std::uint8_t>((r + g + b) / 3);
}

void sample_rgb(const gfx::dx12::CpuRgbaImage& img, int x, int y, std::uint8_t& r, std::uint8_t& g,
                std::uint8_t& b) {
    r = g = b = 0;
    if (img.rgba.empty() || img.width == 0 || img.height == 0) return;
    if (x < 0 || y < 0 || x >= static_cast<int>(img.width) || y >= static_cast<int>(img.height)) return;
    const std::size_t o = (static_cast<std::size_t>(y) * img.width + static_cast<std::size_t>(x)) * 4u;
    r = img.rgba[o];
    g = img.rgba[o + 1];
    b = img.rgba[o + 2];
}

std::vector<std::string> split_text_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string cur;
    auto flush = [&]() {
        lines.push_back(cur);
        cur.clear();
    };
    std::string_view s = text;
    while (!s.empty()) {
        if (s.front() == '@' || s.front() == '\n') {
            flush();
            s.remove_prefix(1);
            continue;
        }
        if (s.front() == '\r') {
            s.remove_prefix(1);
            continue;
        }
        const auto rest = s;
        const auto cp = utf8_next(s);
        (void)cp;
        cur.append(rest.data(), rest.size() - s.size());
    }
    lines.push_back(std::move(cur));
    return lines;
}

int line_width_px(const OftFont& font, std::string_view line) {
    int w = 0;
    std::string_view s = line;
    while (!s.empty()) {
        const auto cp = utf8_next(s);
        const auto it = font.glyphs.find(cp);
        if (it == font.glyphs.end()) {
            w += (std::max)(4, font.height / 4);
            continue;
        }
        w += (std::max)(1, it->second.x1 - it->second.x0);
    }
    return w;
}

} // namespace

void prefetch_oft_images(std::string_view font_name, const std::filesystem::path& omsi_root) {
    if (font_name.empty()) return;
    auto font = find_font_shared(omsi_root, font_name);
    if (!font) return;
    ensure_oft_images(*font);
}

std::optional<TextTextureImage> rasterize_text_texture(const ScoTextTexture& tt, std::string_view text,
                                                       const std::filesystem::path& omsi_root) {
    const int tw = tt.width > 8 ? tt.width : 128;
    const int th = tt.height > 8 ? tt.height : 128;

    auto font_ptr = find_font_shared(omsi_root, tt.font_name);
    if (!font_ptr) {
        warn_missing_oft_once(tt.font_name, omsi_root);
        TextTextureImage out{};
        out.width = static_cast<std::uint32_t>(tw);
        out.height = static_cast<std::uint32_t>(th);
        out.rgba.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4u, 0);
        return out;
    }
    if (!font_ptr->images_loaded) {
        enqueue_oft_images(font_ptr);
        return std::nullopt;
    }
    const OftFont* font = font_ptr.get();

    TextTextureImage out{};
    out.width = static_cast<std::uint32_t>(tw);
    out.height = static_cast<std::uint32_t>(th);
    out.rgba.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4u, 0);

    const auto lines = split_text_lines(text);
    const int line_h = font->height > 0 ? font->height : 16;
    const int total_h = static_cast<int>(lines.size()) * line_h;
    int y = (th - total_h) / 2;

    int painted = 0;
    int missing = 0;
    std::string missing_chars;
    auto blit_glyph = [&](const OftGlyph& g, int dx, int dy) {
        const int gw = g.x1 - g.x0;
        const int gh = line_h;
        for (int gy = 0; gy < gh; ++gy) {
            const int py = dy + gy;
            if (py < 0 || py >= th) continue;
            for (int gx = 0; gx < gw; ++gx) {
                const int px = dx + gx;
                if (px < 0 || px >= tw) continue;
                const int ax = g.x0 + gx;
                const int ay = g.y0 + gy;
                const std::uint8_t a = sample_lum(font->alpha, ax, ay);
                if (a < 8) continue;
                const std::size_t o =
                    (static_cast<std::size_t>(py) * static_cast<std::size_t>(tw) + static_cast<std::size_t>(px)) * 4u;
                if (tt.color_mode != 0) {
                    std::uint8_t r = 0, gch = 0, b = 0;
                    sample_rgb(font->color, ax, ay, r, gch, b);
                    out.rgba[o] = r;
                    out.rgba[o + 1] = gch;
                    out.rgba[o + 2] = b;
                } else {
                    out.rgba[o] = static_cast<std::uint8_t>(tt.r);
                    out.rgba[o + 1] = static_cast<std::uint8_t>(tt.g);
                    out.rgba[o + 2] = static_cast<std::uint8_t>(tt.b);
                }
                out.rgba[o + 3] = a;
                ++painted;
            }
        }
        return gw;
    };

    for (const auto& line : lines) {
        const int lw = line_width_px(*font, line) +
                       tt.letter_spacing * (std::max)(0, static_cast<int>(line.size()) - 1);
        int x = 0;
        if (tt.align == 2) x = tw - lw;
        else if (tt.align != 0) x = (tw - lw) / 2;
        std::string_view s = line;
        while (!s.empty()) {
            const auto cp = utf8_next(s);
            auto it = font->glyphs.find(cp);
            if (it == font->glyphs.end() && cp > 127u) it = font->glyphs.find(cp & 0xFFu);
            if (it == font->glyphs.end() && cp == static_cast<std::uint32_t>(':'))
                it = font->glyphs.find(static_cast<std::uint32_t>('.'));
            if (it == font->glyphs.end()) {
                ++missing;
                if (missing_chars.size() < 24) {
                    if (cp >= 32 && cp < 127) missing_chars.push_back(static_cast<char>(cp));
                    else missing_chars += "?";
                }
                x += (std::max)(4, line_h / 4) + tt.letter_spacing;
                continue;
            }
            x += blit_glyph(it->second, x, y) + tt.letter_spacing;
        }
        y += line_h;
    }
    static int raster_logs = 0;
    if (raster_logs < 16) {
        ++raster_logs;
        log::info("oft raster font=\"" + tt.font_name + "\" glyphs=" + std::to_string(font->glyphs.size()) +
                  " text=\"" + std::string(text) + "\" " + std::to_string(tw) + "x" + std::to_string(th) +
                  " painted=" + std::to_string(painted) + " miss=" + std::to_string(missing) +
                  (missing_chars.empty() ? "" : " missing=\"" + missing_chars + "\"") +
                  " alpha=" + std::to_string(font->alpha.width) + "x" + std::to_string(font->alpha.height) +
                  " mode=" + std::to_string(tt.color_mode));
    }
    return out;
}

} // namespace omsi::sco
