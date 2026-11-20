// BeamNG prefabs: .prefab.json (NDJSON like items.level.json) and legacy .prefab (TorqueScript), flattened to
// shapes in prefab space. Nested prefabs are composed with their parent transform.
#include "beamng/bng_level.hpp"

#include "beamng/bng_vfs.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace bng {

namespace {
// child in parent space = parent (R, S, T) * child local
PrefabChild compose(const Mat3& pr, const float ps[3], const float pp[3], const PrefabChild& c) {
    PrefabChild o;
    o.shape = c.shape;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
            float v = 0.f;
            for (int m = 0; m < 3; ++m) v += pr[static_cast<std::size_t>(r * 3 + m)] * c.rot[static_cast<std::size_t>(m * 3 + k)];
            o.rot[static_cast<std::size_t>(r * 3 + k)] = v;
        }
    for (int r = 0; r < 3; ++r) {
        float v = pp[r];
        for (int m = 0; m < 3; ++m) v += pr[static_cast<std::size_t>(r * 3 + m)] * c.pos[m] * ps[m];
        o.pos[r] = v;
    }
    for (int k = 0; k < 3; ++k) o.scale[k] = c.scale[k] * ps[k];
    return o;
}

bool read_vec3_any(const JsonValue& v, float out[3]) {
    if (v.is_array() && v.size() >= 3) {
        for (int i = 0; i < 3; ++i) out[i] = v[static_cast<std::size_t>(i)].as_float();
        return true;
    }
    if (v.is_string()) return std::sscanf(v.as_string().c_str(), "%f %f %f", &out[0], &out[1], &out[2]) == 3;
    if (v.is_number()) {
        out[0] = out[1] = out[2] = v.as_float();
        return true;
    }
    return false;
}

// BeamNG/Torque writes "rotationMatrix"/"rotationmatrix" column-major in prefab.json (and in the textual
// .prefab/.tso files); Mat3 is row-major, so the transpose is stored — same as items.level.json. Real data:
// the pieces of a prefab row (guard rails, fences) follow the local X axis only with this reading
// (column-major matches 827 of 914 non-90-deg pairs, row-major 32).
bool read_mat3_any(const JsonValue& v, Mat3& out) {
    float m[9];
    const bool got = [&] {
        if (v.is_array() && v.size() >= 9) {
            for (int i = 0; i < 9; ++i) m[i] = v[static_cast<std::size_t>(i)].as_float();
            return true;
        }
        if (v.is_string())
            return std::sscanf(v.as_string().c_str(), "%f %f %f %f %f %f %f %f %f", &m[0], &m[1], &m[2], &m[3], &m[4],
                               &m[5], &m[6], &m[7], &m[8]) == 9;
        return false;
    }();
    if (!got) return false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[static_cast<std::size_t>(r * 3 + c)] = m[c * 3 + r];
    return true;
}

Mat3 axis_angle_deg(float ax, float ay, float az, float deg) {
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len < 1e-6f) return mat3_identity();
    ax /= len;
    ay /= len;
    az /= len;
    const float a = deg * 0.01745329252f, c = std::cos(a), s = std::sin(a), t = 1.f - c;
    return {t * ax * ax + c,      t * ax * ay - s * az, t * ax * az + s * ay,
            t * ax * ay + s * az, t * ay * ay + c,      t * ay * az - s * ax,
            t * ax * az - s * ay, t * ay * az + s * ax, t * az * az + c};
}

// TorqueScript object: new Class(name) { key = "value"; new Child() { ... }; };
struct TsObj {
    std::string cls;
    std::unordered_map<std::string, std::string> f;
    std::vector<TsObj> kids;
};

void ts_parse_block(const std::string& s, std::size_t& i, TsObj& o, int depth);

// Parses "new Class(...) { ... };" starting at s[i] == 'n'.
bool ts_parse_new(const std::string& s, std::size_t& i, TsObj& o, int depth) {
    std::size_t p = i + 4;
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '_')) o.cls.push_back(s[p++]);
    const auto brace = s.find('{', p);
    const auto semi = s.find(';', p);
    if (brace == std::string::npos || (semi != std::string::npos && semi < brace)) {
        i = semi == std::string::npos ? s.size() : semi + 1; // "new X();" without body
        return !o.cls.empty();
    }
    i = brace + 1;
    ts_parse_block(s, i, o, depth);
    while (i < s.size() && (s[i] == ';' || std::isspace(static_cast<unsigned char>(s[i])))) ++i;
    return !o.cls.empty();
}

void ts_parse_block(const std::string& s, std::size_t& i, TsObj& o, int depth) {
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) return;
        if (s[i] == '}') {
            ++i;
            return;
        }
        if (s.compare(i, 2, "//") == 0) {
            const auto e = s.find('\n', i);
            i = e == std::string::npos ? s.size() : e + 1;
            continue;
        }
        if (s.compare(i, 4, "new ") == 0) {
            TsObj kid;
            if (depth < 32 && ts_parse_new(s, i, kid, depth + 1)) o.kids.push_back(std::move(kid));
            continue;
        }
        const auto eq = s.find('=', i), semi = s.find(';', i), close = s.find('}', i);
        if (semi == std::string::npos) {
            i = s.size();
            return;
        }
        if (close != std::string::npos && close < semi) {
            i = close + 1;
            return;
        }
        if (eq == std::string::npos || eq > semi) {
            i = semi + 1;
            continue;
        }
        std::string key = s.substr(i, eq - i);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string val = s.substr(eq + 1, semi - eq - 1);
        const auto q0 = val.find('"'), q1 = val.rfind('"');
        if (q0 != std::string::npos && q1 > q0) val = val.substr(q0 + 1, q1 - q0 - 1);
        o.f[key] = val;
        i = semi + 1;
    }
}
} // namespace

std::vector<PrefabChild> load_prefab(const Vfs& vfs, const std::string& file, const std::string& level_dir, int depth) {
    std::vector<PrefabChild> out;
    if (depth > 8 || file.empty()) return out;
    std::string path = Vfs::normalize(file);
    if (!vfs.exists(path)) {
        const auto alt = Vfs::normalize(level_dir + "/" + file);
        if (vfs.exists(alt)) path = alt;
        else if (vfs.exists(path + ".json")) path += ".json";
    }
    auto txt = vfs.read_text(path);
    if (!txt) return out;
    // shapeName is relative to the prefab file itself (prefabs live under .../art/prefabs/, shapes under
    // .../art/shapes/), so the prefab folder is the base; the level folder stays as a fallback.
    const auto slash = path.find_last_of('/');
    const std::string prefab_dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
    auto add_obj = [&](const std::string& cls, const float pos[3], const Mat3& rot, const float sc[3], const std::string& shape_name,
                       const std::string& sub_prefab) {
        if (cls == "TSStatic") {
            PrefabChild c;
            c.shape = vfs.resolve_shape(shape_name, prefab_dir);
            if (c.shape.empty() && level_dir != prefab_dir) c.shape = vfs.resolve_shape(shape_name, level_dir);
            if (c.shape.empty()) return;
            std::memcpy(c.pos, pos, sizeof(c.pos));
            c.rot = rot;
            std::memcpy(c.scale, sc, sizeof(c.scale));
            out.push_back(std::move(c));
        } else if (cls == "Prefab" && !sub_prefab.empty()) {
            for (const auto& k : load_prefab(vfs, sub_prefab, level_dir, depth + 1)) out.push_back(compose(rot, sc, pos, k));
        }
    };
    const bool json = path.size() > 5 && path.compare(path.size() - 5, 5, ".json") == 0;
    if (json) {
        for (const auto& row : json_parse_lines(*txt)) {
            float pos[3]{0, 0, 0}, sc[3]{1, 1, 1};
            Mat3 rot = mat3_identity();
            read_vec3_any(row.get("position"), pos);
            read_mat3_any(row.get("rotationMatrix"), rot);
            read_vec3_any(row.get("scale"), sc);
            add_obj(row.get("class").as_string(), pos, rot, sc, row.get("shapeName").as_string(), row.get("filename").as_string());
        }
        return out;
    }
    TsObj root;
    std::size_t i = 0;
    while (i < txt->size()) {
        const auto n = txt->find("new ", i);
        if (n == std::string::npos) break;
        i = n;
        TsObj o;
        if (ts_parse_new(*txt, i, o, 0)) root.kids.push_back(std::move(o));
    }
    std::function<void(const TsObj&)> walk = [&](const TsObj& o) {
        float pos[3]{0, 0, 0}, sc[3]{1, 1, 1};
        Mat3 rot = mat3_identity();
        auto get = [&](const char* k) {
            auto it = o.f.find(k);
            return it == o.f.end() ? std::string() : it->second;
        };
        if (const auto v = get("position"); !v.empty()) std::sscanf(v.c_str(), "%f %f %f", &pos[0], &pos[1], &pos[2]);
        if (const auto v = get("scale"); !v.empty()) std::sscanf(v.c_str(), "%f %f %f", &sc[0], &sc[1], &sc[2]);
        if (const auto v = get("rotationmatrix"); !v.empty()) {
            JsonValue tmp = JsonValue::make_string(v);
            read_mat3_any(tmp, rot);
        } else if (const auto v2 = get("rotation"); !v2.empty()) {
            float ax = 0, ay = 0, az = 1, deg = 0;
            if (std::sscanf(v2.c_str(), "%f %f %f %f", &ax, &ay, &az, &deg) == 4) rot = axis_angle_deg(ax, ay, az, deg);
        }
        add_obj(o.cls, pos, rot, sc, get("shapename"), get("filename"));
        for (const auto& k : o.kids) walk(k);
    };
    for (const auto& k : root.kids) walk(k);
    return out;
}

} // namespace bng
