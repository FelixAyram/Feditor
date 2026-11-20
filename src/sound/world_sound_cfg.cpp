#include "omsi/utf8_win32.hpp"
#include "omsi/sound/world_sound_cfg.hpp"

#include "omsi/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>

namespace omsi::sound {
namespace {

std::mutex g_cfg_mu;
std::unordered_map<std::string, std::shared_ptr<const SoundCfg>> g_cfg_cache;

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parse_float(const std::string& s, float& out) {
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return false;
    out = v;
    return true;
}

// OMSI writes wav names relative to the cfg folder (sometimes with "..\" prefixes).
std::filesystem::path resolve_wav(const std::filesystem::path& cfg_dir, std::string name) {
    namespace fs = std::filesystem;
    std::replace(name.begin(), name.end(), '\\', '/');
    const fs::path rel(name);
    const fs::path leaf = rel.filename();
    std::error_code ec;
    const fs::path cands[] = {
        cfg_dir / rel,
        cfg_dir / leaf,
        cfg_dir / ".." / "sound" / leaf,
        cfg_dir / ".." / "Sound" / leaf,
    };
    for (const auto& c : cands)
        if (fs::exists(c, ec)) return c;
    return cfg_dir / rel;
}

struct RawSection {
    std::string name; // "[sound]" etc., lower-cased is NOT applied (OMSI is case-sensitive here)
    std::vector<std::string> vals;
};

std::shared_ptr<const SoundCfg> parse_cfg(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return nullptr;

    // Pass 1: split into sections. Values stop at the next [header]; comment lines in between
    // are harmless because each consumer only reads the leading values it needs.
    std::vector<RawSection> secs;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            secs.push_back({line, {}});
            continue;
        }
        if (secs.empty()) continue;
        secs.back().vals.push_back(line);
    }

    auto out = std::make_shared<SoundCfg>();
    out->path = path;
    const auto dir = path.parent_path();
    SoundDef* cur = nullptr;

    for (const auto& s : secs) {
        const auto& v = s.vals;
        if (s.name == "[sound]" || s.name == "[loopsound]") {
            out->defs.emplace_back();
            cur = &out->defs.back();
            cur->kind = s.name == "[loopsound]" ? SoundDef::Kind::Loop : SoundDef::Kind::OneShot;
            if (!v.empty()) cur->wav = resolve_wav(dir, v[0]);
            if (cur->kind == SoundDef::Kind::OneShot) {
                if (v.size() >= 2) parse_float(v[1], cur->volume);
            } else {
                // wav, samplerate, pitch variable, pitch reference, volume
                if (v.size() >= 3) cur->pitch_var = v[2];
                if (v.size() >= 4) parse_float(v[3], cur->pitch_ref);
                if (v.size() >= 5) parse_float(v[4], cur->volume);
            }
            continue;
        }
        if (!cur) continue;

        if (s.name == "[3d]") {
            float x = 0.f, y = 0.f, z = 0.f, r = 0.f;
            if (v.size() >= 4 && parse_float(v[0], x) && parse_float(v[1], y) && parse_float(v[2], z) &&
                parse_float(v[3], r)) {
                cur->offset = {x, y, z};
                cur->range_m = std::max(0.5f, r);
                cur->has_3d = true;
            }
        } else if (s.name == "[trigger]") {
            if (!v.empty()) cur->trigger = v[0];
        } else if (s.name == "[noloop]") {
            cur->no_loop = true;
        } else if (s.name == "[conditionSingle]") {
            SoundCondition c{};
            if (v.size() >= 3 && !v[0].empty()) {
                c.var = v[0];
                float op = 3.f;
                parse_float(v[1], c.ref);
                parse_float(v[2], op);
                c.op = static_cast<int>(op);
                cur->conditions.push_back(c);
            }
        } else if (s.name == "[conditionBool]" || s.name == "[conditionInt]") {
            // Not evaluated yet: gate the definition so it never auto-starts.
            SoundCondition c{};
            c.var = v.empty() ? std::string("?") : v[0];
            c.op = 1;
            c.ref = 1.f;
            cur->conditions.push_back(c);
        }
    }
    return out;
}

} // namespace

std::shared_ptr<const SoundCfg> load_sound_cfg(const std::filesystem::path& cfg_path) {
    const std::string key = omsi::path_text(cfg_path.lexically_normal());
    {
        std::lock_guard lock(g_cfg_mu);
        if (const auto it = g_cfg_cache.find(key); it != g_cfg_cache.end()) return it->second;
    }
    auto parsed = parse_cfg(cfg_path);
    if (!parsed) {
        log::warn("world_sound: cannot read sound cfg " + omsi::path_text(cfg_path));
        // Cache the miss as an empty config so a missing file is reported once.
        parsed = std::make_shared<const SoundCfg>();
    }
    std::lock_guard lock(g_cfg_mu);
    auto [it, _] = g_cfg_cache.emplace(key, std::move(parsed));
    return it->second;
}

bool sound_condition_holds(const SoundCondition& c, float value) {
    constexpr float kEps = 1e-4f;
    const bool eq = std::fabs(value - c.ref) < kEps;
    switch (c.op) {
    case 0: return !eq;
    case 1: return eq;
    case 2: return value < c.ref;
    case 3: return value > c.ref;
    case 4: return value <= c.ref;
    case 5: return value >= c.ref;
    default: return false;
    }
}

} // namespace omsi::sound
