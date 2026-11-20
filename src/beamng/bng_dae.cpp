#include "beamng/bng_dae.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

namespace bng {

// ── XML ───────────────────────────────────────────────────────────────────────────────────────────────────────
const std::string* XmlNode::attr(std::string_view k) const {
    for (const auto& [a, v] : attrs)
        if (a == k) return &v;
    return nullptr;
}
const XmlNode* XmlNode::child(std::string_view n) const {
    for (const auto& c : children)
        if (c.name == n) return &c;
    return nullptr;
}
std::vector<const XmlNode*> XmlNode::children_named(std::string_view n) const {
    std::vector<const XmlNode*> out;
    for (const auto& c : children)
        if (c.name == n) out.push_back(&c);
    return out;
}

namespace {
void decode_entities(std::string& s) {
    if (s.find('&') == std::string::npos) return;
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            o.push_back(s[i]);
            continue;
        }
        const auto e = s.find(';', i);
        if (e == std::string::npos) {
            o.push_back(s[i]);
            continue;
        }
        const std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "lt") o.push_back('<');
        else if (ent == "gt") o.push_back('>');
        else if (ent == "amp") o.push_back('&');
        else if (ent == "quot") o.push_back('"');
        else if (ent == "apos") o.push_back('\'');
        else if (!ent.empty() && ent[0] == '#') o.push_back(static_cast<char>(std::strtol(ent.c_str() + (ent[1] == 'x' ? 2 : 1), nullptr, ent[1] == 'x' ? 16 : 10)));
        else o += "&" + ent + ";";
        i = e;
    }
    s.swap(o);
}
} // namespace

bool xml_parse(std::string_view s, XmlNode& root, std::string* err) {
    std::vector<XmlNode*> stack;
    root = XmlNode{};
    root.name = "#document";
    stack.push_back(&root);
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') {
            const auto e = s.find('<', i);
            const std::size_t end = e == std::string_view::npos ? s.size() : e;
            // keep text (geometry arrays live here); skip whitespace-only runs
            std::size_t a = i;
            while (a < end && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t')) ++a;
            if (a < end) {
                std::string t(s.substr(i, end - i));
                decode_entities(t);
                stack.back()->text += t;
            }
            i = end;
            continue;
        }
        if (s.compare(i, 4, "<!--") == 0) {
            const auto e = s.find("-->", i + 4);
            i = e == std::string_view::npos ? s.size() : e + 3;
            continue;
        }
        if (s.compare(i, 9, "<![CDATA[") == 0) {
            const auto e = s.find("]]>", i + 9);
            stack.back()->text += std::string(s.substr(i + 9, (e == std::string_view::npos ? s.size() : e) - i - 9));
            i = e == std::string_view::npos ? s.size() : e + 3;
            continue;
        }
        if (s[i + 1] == '?' || s[i + 1] == '!') {
            const auto e = s.find('>', i);
            i = e == std::string_view::npos ? s.size() : e + 1;
            continue;
        }
        if (s[i + 1] == '/') {
            const auto e = s.find('>', i);
            if (stack.size() > 1) stack.pop_back();
            i = e == std::string_view::npos ? s.size() : e + 1;
            continue;
        }
        // start tag
        std::size_t p = i + 1;
        const std::size_t ns = p;
        while (p < s.size() && s[p] != ' ' && s[p] != '>' && s[p] != '/' && s[p] != '\t' && s[p] != '\n' && s[p] != '\r') ++p;
        XmlNode node;
        node.name = std::string(s.substr(ns, p - ns));
        bool self_close = false;
        while (p < s.size()) {
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
            if (p >= s.size()) break;
            if (s[p] == '>') { ++p; break; }
            if (s[p] == '/') { self_close = true; ++p; continue; }
            const std::size_t ks = p;
            while (p < s.size() && s[p] != '=' && s[p] != ' ' && s[p] != '>' && s[p] != '/') ++p;
            std::string key(s.substr(ks, p - ks));
            std::string val;
            while (p < s.size() && (s[p] == ' ' || s[p] == '=')) ++p;
            if (p < s.size() && (s[p] == '"' || s[p] == '\'')) {
                const char q = s[p++];
                const auto e = s.find(q, p);
                const std::size_t ve = e == std::string_view::npos ? s.size() : e;
                val = std::string(s.substr(p, ve - p));
                decode_entities(val);
                p = ve + 1;
            }
            node.attrs.emplace_back(std::move(key), std::move(val));
        }
        i = p;
        stack.back()->children.push_back(std::move(node));
        if (!self_close) stack.push_back(&stack.back()->children.back());
    }
    if (err && stack.size() != 1) *err = "unbalanced xml";
    return true;
}

// ── COLLADA ───────────────────────────────────────────────────────────────────────────────────────────────────
std::size_t DaeLod::triangle_count() const {
    std::size_t n = 0;
    for (const auto& s : submeshes) n += s.indices.size() / 3;
    return n;
}

namespace {
struct Mat4 {
    float m[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // row-major, column vectors
};
Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
void parse_floats(const std::string& t, std::vector<float>& out) {
    const char* p = t.c_str();
    const char* end = p + t.size();
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        if (p >= end) break;
        float v = 0.f;
        auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc()) {
            // tolerate junk tokens
            while (p < end && *p != ' ' && *p != '\n') ++p;
            continue;
        }
        out.push_back(v);
        p = r.ptr;
    }
}
void parse_ints(const std::string& t, std::vector<int>& out) {
    const char* p = t.c_str();
    const char* end = p + t.size();
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        if (p >= end) break;
        int v = 0;
        auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc()) {
            while (p < end && *p != ' ' && *p != '\n') ++p;
            continue;
        }
        out.push_back(v);
        p = r.ptr;
    }
}
std::string strip_hash(const std::string* s) {
    if (!s) return {};
    return (!s->empty() && (*s)[0] == '#') ? s->substr(1) : *s;
}

struct Source {
    std::vector<float> data;
    int stride = 1;
};

std::string lower_copy(const std::string& n) {
    std::string l;
    l.reserve(n.size());
    for (const char c : n) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return l;
}

// Torque detail size from a node name: the trailing number of a *detail marker* only.
// Accepted shapes (all shipped content uses one of them):
//   "<object>_a<size>"   Blender/BeamNG exporter ("Birch_Large_01_a900", "OakLarge_01_a1400")
//   "<marker><size>"     Torque legacy ("mesh500", "detail500", "start01", "base00")
//   "<marker>-<n>"       negative detail = collision / LOS ("Colmesh-1", "collision-1")
// A trailing integer that is just part of an object name ("Cube_002", "Plane_741", "trunk_01") is NOT a detail
// size: taking it as one split a single visual level into several buckets, so a body part became the LOD0
// threshold and geometry from different detail versions got mixed on screen when the camera got close.
bool detail_from_name(const std::string& n, int& out) {
    if (n.empty()) return false;
    std::size_t b = n.size();
    while (b > 0 && std::isdigit(static_cast<unsigned char>(n[b - 1]))) --b;
    if (b == n.size()) return false;
    int v = std::atoi(n.c_str() + b);
    std::size_t p = b;
    if (p > 0 && n[p - 1] == '-') { // negative detail: "Colmesh-1" is collision, never a visual LOD
        out = -v;
        return true;
    }
    // Strip the separators between the name and the digits to look at the marker word before them.
    std::size_t q = p;
    while (q > 0 && (n[q - 1] == '_' || n[q - 1] == '-' || n[q - 1] == '.' || n[q - 1] == ' ')) --q;
    const std::string pre = lower_copy(n.substr(0, q));
    bool marker = pre.empty() || (q > 0 && n[q - 1] == 'a'); // "<object>_a<size>"
    static const char* kMarkers[] = {"mesh", "detail", "start", "base", "lod", "colmesh", "col",
                                     "collision", "colbox", "box", "billboard", "impostor", "nulldetail"};
    for (const char* m : kMarkers) {
        const std::size_t ml = std::strlen(m);
        if (!marker && pre.size() >= ml && pre.compare(pre.size() - ml, ml, m) == 0) marker = true;
    }
    if (!marker) return false;
    out = v;
    return true;
}
bool is_helper_node(const std::string& n) {
    const std::string l = lower_copy(n);
    return l.rfind("bounds", 0) == 0 || l.rfind("nulldetail", 0) == 0 || l.rfind("los", 0) == 0;
}

// Torque containers: "base00" and "start01" carry a number that is NOT a detail size (base 0 / start 1).
// Their children inherit the detail of the enclosing level; treating the container number as a LOD used to put
// every unnumbered child into a bucket of its own (base00 -> 0, start01 -> 1), i.e. a phantom near LOD.
bool is_detail_container(const std::string& n) {
    if (n.empty()) return false;
    std::size_t e = n.size();
    while (e > 0 && (std::isdigit(static_cast<unsigned char>(n[e - 1])) || n[e - 1] == '_' || n[e - 1] == '-' ||
                     n[e - 1] == '.' || n[e - 1] == ' '))
        --e;
    const std::string l = lower_copy(n.substr(0, e));
    return l == "base" || l == "start";
}

// Torque auto billboard / impostor node ("bb__autobillboard25", also plain "billboard10" / "impostor").
// Its trailing number is the pixel size the impostor is used AT (25 px in 350 of the 394 shipped shapes), NOT a
// detail size: if it were taken as a detail LOD it would sit between the real LODs and win the pick near the
// camera, drawing a flat quad instead of the mesh.
bool is_billboard_name(const std::string& n) {
    std::string l;
    l.reserve(n.size());
    for (const char c : n) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return l.rfind("bb__", 0) == 0 || l.find("autobillboard") != std::string::npos ||
           l.find("impostor") != std::string::npos || l.rfind("billboard", 0) == 0;
}

// Collision / occluder nodes. Torque exports name them Col, Colmesh, collision, colbox, box (often with a
// "-1" detail suffix). They must never become a visual LOD: that is what turned rock_small_wall_02.prefab
// into a cube (the box collider won the LOD pick).
bool is_collision_name(const std::string& n) {
    std::string l;
    l.reserve(n.size());
    for (const char c : n) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    // strip trailing detail/index decorations: "Colmesh-1", "collision_02", "box.001"
    std::size_t e = l.size();
    while (e > 0 && (std::isdigit(static_cast<unsigned char>(l[e - 1])) || l[e - 1] == '-' || l[e - 1] == '_' || l[e - 1] == '.' ||
                     l[e - 1] == ' '))
        --e;
    l.resize(e);
    return l == "colmesh" || l == "collision" || l == "col" || l == "colbox" || l == "box";
}

std::string trim_text(const std::string& t) {
    std::size_t b = 0, e = t.size();
    while (b < e && (t[b] == ' ' || t[b] == '\n' || t[b] == '\r' || t[b] == '\t')) ++b;
    while (e > b && (t[e - 1] == ' ' || t[e - 1] == '\n' || t[e - 1] == '\r' || t[e - 1] == '\t')) --e;
    std::string o = t.substr(b, e - b);
    for (auto& c : o)
        if (c == '\\') c = '/';
    return o;
}
} // namespace

std::optional<DaeShape> parse_dae(std::string_view text) {
    XmlNode doc;
    if (!xml_parse(text, doc)) return std::nullopt;
    const XmlNode* col = doc.child("COLLADA");
    if (!col) return std::nullopt;
    DaeShape shape;

    // asset: up axis + unit
    bool y_up = false;
    float unit = 1.f;
    if (const auto* asset = col->child("asset")) {
        if (const auto* up = asset->child("up_axis")) y_up = up->text.find("Y_UP") != std::string::npos;
        if (const auto* u = asset->child("unit"))
            if (const auto* m = u->attr("meter")) unit = std::strtof(m->c_str(), nullptr);
        if (unit <= 0.f) unit = 1.f;
    }
    shape.unit_scale = unit;

    // materials: id -> name
    std::unordered_map<std::string, std::string> mat_name;
    const XmlNode* lib_mat = col->child("library_materials");
    if (lib_mat)
        for (const auto* m : lib_mat->children_named("material")) {
            const std::string id = m->attr("id") ? *m->attr("id") : std::string();
            std::string nm = m->attr("name") ? *m->attr("name") : id;
            // Blender/Max exporters append "-material"; BeamNG maps by the bare name.
            if (nm.size() > 9 && nm.compare(nm.size() - 9, 9, "-material") == 0) nm.resize(nm.size() - 9);
            mat_name[id] = nm;
        }

    // Diffuse texture baked in the DAE, used as a material fallback when the level has no such material.
    // COLLADA chain: material -> instance_effect -> profile_COMMON technique diffuse <texture texture="s">
    // -> newparam sampler2D/source -> newparam surface/init_from -> library_images init_from.
    std::unordered_map<std::string, std::string> image_file; // image id|name -> path
    if (const auto* li = col->child("library_images"))
        for (const auto* im : li->children_named("image")) {
            const std::string id = im->attr("id") ? *im->attr("id") : std::string();
            std::string p;
            if (const auto* ini = im->child("init_from")) {
                p = trim_text(ini->text);
                if (p.empty())
                    if (const auto* ref = ini->child("ref")) p = trim_text(ref->text);
            }
            if (p.rfind("file://", 0) == 0) p = p.substr(7);
            if (p.empty()) continue;
            if (!id.empty()) image_file[id] = p;
            if (const auto* nm = im->attr("name")) image_file[*nm] = p;
        }
    std::unordered_map<std::string, std::string> effect_image; // effect id -> path
    if (const auto* le = col->child("library_effects"))
        for (const auto* ef : le->children_named("effect")) {
            const std::string eid = ef->attr("id") ? *ef->attr("id") : std::string();
            const auto* pc = ef->child("profile_COMMON");
            const auto* tech = pc ? pc->child("technique") : nullptr;
            if (eid.empty() || !tech) continue;
            std::unordered_map<std::string, const XmlNode*> params; // newparam sid -> node
            for (const auto* np : pc->children_named("newparam"))
                if (const auto* sid = np->attr("sid")) params[*sid] = np;
            // sampler2D source -> surface init_from -> image id
            auto resolve_sampler = [&](const std::string& sid) -> std::string {
                auto it = params.find(sid);
                if (it == params.end()) return {};
                if (const auto* s2 = it->second->child("sampler2D")) {
                    const auto* src = s2->child("source");
                    if (!src) return {};
                    auto sit = params.find(trim_text(src->text));
                    if (sit == params.end()) return {};
                    it = sit;
                }
                const auto* sf = it->second->child("surface");
                const auto* ini = sf ? sf->child("init_from") : nullptr;
                if (!ini) return {};
                std::string img = trim_text(ini->text);
                if (img.empty())
                    if (const auto* ref = ini->child("ref")) img = trim_text(ref->text);
                if (img.empty()) return {};
                if (auto f = image_file.find(img); f != image_file.end()) return f->second;
                return img.find('.') != std::string::npos ? img : std::string();
            };
            std::string tex;
            for (const char* shader : {"phong", "lambert", "blinn", "constant"}) {
                const auto* sh = tech->child(shader);
                if (!sh) continue;
                const auto* diff = sh->child("diffuse");
                const auto* t = diff ? diff->child("texture") : nullptr;
                if (!t) continue;
                const auto* tsid = t->attr("texture");
                if (tsid) tex = resolve_sampler(*tsid);
                if (!tex.empty()) break;
            }
            if (!tex.empty()) effect_image[eid] = tex;
        }
    std::unordered_map<std::string, std::string> mat_image; // material name|id -> path
    if (lib_mat && !effect_image.empty())
        for (const auto* m : lib_mat->children_named("material")) {
            const std::string id = m->attr("id") ? *m->attr("id") : std::string();
            std::string nm = m->attr("name") ? *m->attr("name") : id;
            if (nm.size() > 9 && nm.compare(nm.size() - 9, 9, "-material") == 0) nm.resize(nm.size() - 9);
            const auto* ie = m->child("instance_effect");
            if (!ie) continue;
            auto it = effect_image.find(strip_hash(ie->attr("url")));
            if (it == effect_image.end()) continue;
            mat_image[nm] = it->second;
            if (!id.empty()) mat_image[id] = it->second;
        }

    // geometries
    struct Prim {
        std::string symbol;
        std::vector<DaeVertex> verts;
        std::vector<std::uint32_t> idx;
    };
    std::unordered_map<std::string, std::vector<Prim>> geoms;
    if (const auto* lg = col->child("library_geometries")) {
        for (const auto* g : lg->children_named("geometry")) {
            const auto* mesh = g->child("mesh");
            if (!mesh) continue;
            std::unordered_map<std::string, Source> sources;
            for (const auto* s : mesh->children_named("source")) {
                Source src;
                if (const auto* fa = s->child("float_array")) parse_floats(fa->text, src.data);
                if (const auto* tc = s->child("technique_common"))
                    if (const auto* acc = tc->child("accessor"))
                        if (const auto* st = acc->attr("stride")) src.stride = (std::max)(1, std::atoi(st->c_str()));
                sources[s->attr("id") ? *s->attr("id") : std::string()] = std::move(src);
            }
            // <vertices> may alias POSITION (and sometimes NORMAL/TEXCOORD)
            std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> vert_alias;
            for (const auto* v : mesh->children_named("vertices")) {
                auto& list = vert_alias[v->attr("id") ? *v->attr("id") : std::string()];
                for (const auto* in : v->children_named("input"))
                    list.emplace_back(in->attr("semantic") ? *in->attr("semantic") : "", strip_hash(in->attr("source")));
            }
            auto& prims = geoms[g->attr("id") ? *g->attr("id") : std::string()];
            for (const auto& pn : mesh->children) {
                const bool tris = pn.name == "triangles";
                const bool poly = pn.name == "polylist" || pn.name == "polygons";
                if (!tris && !poly) continue;
                struct In {
                    std::string sem;
                    const Source* src = nullptr;
                    int offset = 0;
                    int set = 0;
                };
                std::vector<In> ins;
                int max_off = 0;
                for (const auto* in : pn.children_named("input")) {
                    const std::string sem = in->attr("semantic") ? *in->attr("semantic") : "";
                    const int off = in->attr("offset") ? std::atoi(in->attr("offset")->c_str()) : 0;
                    const int set = in->attr("set") ? std::atoi(in->attr("set")->c_str()) : 0;
                    max_off = (std::max)(max_off, off);
                    const std::string sid = strip_hash(in->attr("source"));
                    if (sem == "VERTEX") {
                        for (const auto& [asem, asrc] : vert_alias[sid]) {
                            auto it = sources.find(asrc);
                            if (it != sources.end()) ins.push_back({asem, &it->second, off, 0});
                        }
                    } else {
                        auto it = sources.find(sid);
                        if (it != sources.end()) ins.push_back({sem, &it->second, off, set});
                    }
                }
                const int stride = max_off + 1;
                std::vector<int> p, vcount;
                for (const auto* pe : pn.children_named("p")) parse_ints(pe->text, p);
                if (const auto* vc = pn.child("vcount")) parse_ints(vc->text, vcount);
                Prim prim;
                prim.symbol = pn.attr("material") ? *pn.attr("material") : std::string();
                // TEXCOORD sets are numbered by "set" or by order of appearance
                int tex_order = 0;
                for (auto& in : ins)
                    if (in.sem == "TEXCOORD") {
                        if (!in.set) in.set = tex_order;
                        ++tex_order;
                    }
                std::unordered_map<std::uint64_t, std::uint32_t> dedup;
                auto fetch = [&](std::size_t corner) -> std::uint32_t {
                    DaeVertex v{};
                    v.color = 0xFFFFFFFFu;
                    std::uint64_t key = 1469598103934665603ull;
                    for (const auto& in : ins) {
                        const std::size_t pi = corner * static_cast<std::size_t>(stride) + static_cast<std::size_t>(in.offset);
                        if (pi >= p.size()) continue;
                        const int ix = p[pi];
                        key = (key ^ static_cast<std::uint64_t>(ix + 1) ^ (static_cast<std::uint64_t>(in.offset) << 40)) * 1099511628211ull;
                        const auto& d = in.src->data;
                        const std::size_t b = static_cast<std::size_t>(ix) * static_cast<std::size_t>(in.src->stride);
                        auto at = [&](std::size_t k) { return b + k < d.size() ? d[b + k] : 0.f; };
                        if (in.sem == "POSITION") {
                            v.pos[0] = at(0); v.pos[1] = at(1); v.pos[2] = at(2); // metres applied by the root unit scale
                        } else if (in.sem == "NORMAL") {
                            v.normal[0] = at(0); v.normal[1] = at(1); v.normal[2] = at(2);
                        } else if (in.sem == "TEXCOORD") {
                            float* uv = in.set == 0 ? v.uv0 : v.uv1;
                            uv[0] = at(0);
                            uv[1] = 1.f - at(1); // COLLADA v up -> DX v down
                        } else if (in.sem == "COLOR") {
                            const float r = at(0), g = at(1), bl = at(2), a = in.src->stride >= 4 ? at(3) : 1.f;
                            auto q = [](float f) { return static_cast<std::uint32_t>((std::clamp)(f, 0.f, 1.f) * 255.f + 0.5f); };
                            v.color = q(r) | (q(g) << 8) | (q(bl) << 16) | (q(a) << 24);
                        }
                    }
                    auto it = dedup.find(key);
                    if (it != dedup.end()) return it->second;
                    const auto ni = static_cast<std::uint32_t>(prim.verts.size());
                    prim.verts.push_back(v);
                    dedup.emplace(key, ni);
                    return ni;
                };
                const std::size_t corners = p.size() / static_cast<std::size_t>(stride);
                if (tris) {
                    for (std::size_t c = 0; c + 2 < corners + 0 && c + 2 < corners; c += 3) {
                        prim.idx.push_back(fetch(c));
                        prim.idx.push_back(fetch(c + 1));
                        prim.idx.push_back(fetch(c + 2));
                    }
                } else {
                    std::size_t c = 0;
                    const std::size_t polys = vcount.empty() ? corners / 3 : vcount.size();
                    for (std::size_t k = 0; k < polys; ++k) {
                        const int n = vcount.empty() ? 3 : vcount[k];
                        if (n >= 3) {
                            const std::uint32_t a = fetch(c);
                            for (int t = 1; t + 1 < n; ++t) {
                                prim.idx.push_back(a);
                                prim.idx.push_back(fetch(c + static_cast<std::size_t>(t)));
                                prim.idx.push_back(fetch(c + static_cast<std::size_t>(t + 1)));
                            }
                        }
                        c += static_cast<std::size_t>((std::max)(n, 0));
                    }
                }
                if (!prim.idx.empty()) prims.push_back(std::move(prim));
            }
        }
    }

    // visual scene: walk nodes, accumulate transforms, bucket instances per detail level
    std::map<int, DaeLod, std::greater<int>> by_detail;
    std::map<int, DaeLod, std::greater<int>> by_billboard; // bb__autobillboard impostors, kept apart from lods
    const XmlNode* scene = nullptr;
    if (const auto* lvs = col->child("library_visual_scenes")) scene = lvs->child("visual_scene");
    Mat4 axis;
    if (y_up) { // (x,y,z)_yup -> (x,-z,y)_zup
        axis.m[5] = 0; axis.m[6] = -1; axis.m[9] = 1; axis.m[10] = 0;
    }
    // COLLADA <unit meter="0.01">: node transforms *and* geometry are in those units. Scaling the root
    // matrix (instead of the vertices) also puts node translations in metres.
    if (unit != 1.f) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) axis.m[r * 4 + c] *= unit;
    }
    auto node_local = [](const XmlNode& n) {
        Mat4 m;
        for (const auto& c : n.children) {
            std::vector<float> f;
            if (c.name == "matrix") {
                parse_floats(c.text, f);
                if (f.size() >= 16) {
                    Mat4 t;
                    for (int i = 0; i < 16; ++i) t.m[i] = f[static_cast<std::size_t>(i)];
                    m = mul(m, t);
                }
            } else if (c.name == "translate") {
                parse_floats(c.text, f);
                if (f.size() >= 3) {
                    Mat4 t;
                    t.m[3] = f[0]; t.m[7] = f[1]; t.m[11] = f[2];
                    m = mul(m, t);
                }
            } else if (c.name == "rotate") {
                parse_floats(c.text, f);
                if (f.size() >= 4) {
                    const float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                    if (len > 1e-6f) {
                        const float x = f[0] / len, y = f[1] / len, z = f[2] / len, a = f[3] * 3.14159265f / 180.f;
                        const float cs = std::cos(a), sn = std::sin(a), t1 = 1 - cs;
                        Mat4 r;
                        r.m[0] = t1 * x * x + cs; r.m[1] = t1 * x * y - sn * z; r.m[2] = t1 * x * z + sn * y;
                        r.m[4] = t1 * x * y + sn * z; r.m[5] = t1 * y * y + cs; r.m[6] = t1 * y * z - sn * x;
                        r.m[8] = t1 * x * z - sn * y; r.m[9] = t1 * y * z + sn * x; r.m[10] = t1 * z * z + cs;
                        m = mul(m, r);
                    }
                }
            } else if (c.name == "scale") {
                parse_floats(c.text, f);
                if (f.size() >= 3) {
                    Mat4 t;
                    t.m[0] = f[0]; t.m[5] = f[1]; t.m[10] = f[2];
                    m = mul(m, t);
                }
            }
        }
        return m;
    };
    std::unordered_map<std::string, const XmlNode*> lib_nodes;
    if (const auto* ln = col->child("library_nodes"))
        for (const auto* n : ln->children_named("node"))
            if (const auto* id = n->attr("id")) lib_nodes[*id] = n;

    // Torque convention: geometry nodes carry the detail number; when only a parent marker carries it
    // ("start01" > "base00" > "mesh"), inherit from the closest ancestor with a number.
    auto add_instance = [&](const XmlNode& inst, const Mat4& world, int detail, bool has_detail, bool collision,
                            bool billboard) {
        auto git = geoms.find(strip_hash(inst.attr("url")));
        if (git == geoms.end()) return;
        std::unordered_map<std::string, std::string> bind; // symbol -> material name
        if (const auto* bm = inst.child("bind_material"))
            if (const auto* tc = bm->child("technique_common"))
                for (const auto* im : tc->children_named("instance_material")) {
                    const std::string sym = im->attr("symbol") ? *im->attr("symbol") : "";
                    const std::string tgt = strip_hash(im->attr("target"));
                    auto mn = mat_name.find(tgt);
                    bind[sym] = mn != mat_name.end() ? mn->second : tgt;
                }
        // collision geometry always lands in a negative-detail bucket, so it can never be picked as a visual LOD
        const int bucket = collision ? -1 : (has_detail ? detail : 0);
        DaeLod& lod = billboard ? by_billboard[bucket] : by_detail[bucket];
        lod.detail_size = bucket;
        // normal matrix = upper 3x3 (assumes no shear; normalised later)
        for (const auto& prim : git->second) {
            DaeSubmesh* sm = nullptr;
            std::string mname = bind.count(prim.symbol) ? bind[prim.symbol] : prim.symbol;
            if (auto mn = mat_name.find(mname); mn != mat_name.end()) mname = mn->second;
            for (auto& s : lod.submeshes)
                if (s.material == mname) { sm = &s; break; }
            if (!sm) {
                lod.submeshes.push_back({});
                sm = &lod.submeshes.back();
                sm->material = mname;
            }
            if (sm->texture.empty()) {
                if (auto ti = mat_image.find(mname); ti != mat_image.end()) sm->texture = ti->second;
            }
            const auto base = static_cast<std::uint32_t>(sm->vertices.size());
            for (DaeVertex v : prim.verts) {
                const float x = v.pos[0], y = v.pos[1], z = v.pos[2];
                v.pos[0] = world.m[0] * x + world.m[1] * y + world.m[2] * z + world.m[3];
                v.pos[1] = world.m[4] * x + world.m[5] * y + world.m[6] * z + world.m[7];
                v.pos[2] = world.m[8] * x + world.m[9] * y + world.m[10] * z + world.m[11];
                const float nx = v.normal[0], ny = v.normal[1], nz = v.normal[2];
                float tn[3] = {world.m[0] * nx + world.m[1] * ny + world.m[2] * nz, world.m[4] * nx + world.m[5] * ny + world.m[6] * nz,
                               world.m[8] * nx + world.m[9] * ny + world.m[10] * nz};
                const float l = std::sqrt(tn[0] * tn[0] + tn[1] * tn[1] + tn[2] * tn[2]);
                if (l > 1e-8f) for (float& c : tn) c /= l;
                std::memcpy(v.normal, tn, sizeof(tn));
                sm->vertices.push_back(v);
            }
            // mirrored transforms flip winding
            const float det = world.m[0] * (world.m[5] * world.m[10] - world.m[6] * world.m[9]) -
                              world.m[1] * (world.m[4] * world.m[10] - world.m[6] * world.m[8]) +
                              world.m[2] * (world.m[4] * world.m[9] - world.m[5] * world.m[8]);
            for (std::size_t k = 0; k + 2 < prim.idx.size(); k += 3) {
                sm->indices.push_back(base + prim.idx[k]);
                if (det < 0) {
                    sm->indices.push_back(base + prim.idx[k + 2]);
                    sm->indices.push_back(base + prim.idx[k + 1]);
                } else {
                    sm->indices.push_back(base + prim.idx[k + 1]);
                    sm->indices.push_back(base + prim.idx[k + 2]);
                }
            }
        }
    };
    // Collision state for children: the flag is inherited, but a node carrying an explicit non-negative detail
    // marker ("mesh500") is a visual LOD again — otherwise an asset whose root happens to be named "box" would
    // disappear entirely.
    auto child_collision = [](bool parent_coll, const std::string& cn) {
        if (!parent_coll) return is_collision_name(cn);
        int cd = 0;
        if (!is_collision_name(cn) && !is_helper_node(cn) && detail_from_name(cn, cd) && cd >= 0) return false;
        return true;
    };
    std::function<void(const XmlNode&, const Mat4&, int, bool, bool, int, bool)> walk =
        [&](const XmlNode& n, const Mat4& parent, int detail, bool has_detail, bool in_collision, int depth,
            bool in_billboard) {
        if (depth > 64) return;
        const std::string nm = n.attr("name") ? *n.attr("name") : (n.attr("id") ? *n.attr("id") : "");
        const bool helper = is_helper_node(nm);
        const bool collision = in_collision || is_collision_name(nm);
        // Impostor subtree (bb__autobillboard25 …): never a visual detail LOD, the number is a pixel size.
        const bool billboard = in_billboard || is_billboard_name(nm);
        if (billboard && !in_billboard) {
            shape.has_billboard = true;
            int bp = 0;
            if (detail_from_name(nm, bp) && bp > 0) shape.billboard_px = bp;
        }
        if (is_helper_node(nm) && !n.child("instance_geometry")) {
            // markers like "bounds" carry no visible geometry; still walk children (e.g. start01 under base00)
        }
        // A real detail marker re-keys the whole subtree (also when an ancestor already had a detail: nested
        // groups of the same level must end up in ONE bucket, and a node without a number must never be
        // filtered into another LOD). Containers (base/start) and helper markers (bounds / nulldetail / los)
        // are not LOD boundaries: their children keep inheriting the enclosing detail.
        int d = 0;
        bool hd = has_detail;
        if (!helper && !is_detail_container(nm) && detail_from_name(nm, d)) {
            detail = d;
            hd = true;
        }
        const Mat4 world = mul(parent, node_local(n));
        if (!is_helper_node(nm))
            for (const auto* ig : n.children_named("instance_geometry"))
                add_instance(*ig, world, detail, hd, collision, billboard);
        for (const auto* in : n.children_named("instance_node"))
            if (auto it = lib_nodes.find(strip_hash(in->attr("url"))); it != lib_nodes.end()) {
                const std::string cn = it->second->attr("name") ? *it->second->attr("name")
                                                              : (it->second->attr("id") ? *it->second->attr("id") : std::string());
                walk(*it->second, world, detail, hd, child_collision(collision, cn), depth + 1, billboard);
            }
        for (const auto* c : n.children_named("node")) {
            const std::string cn = c->attr("name") ? *c->attr("name") : (c->attr("id") ? *c->attr("id") : std::string());
            walk(*c, world, detail, hd, child_collision(collision, cn), depth + 1, billboard);
        }
    };
    if (scene)
        for (const auto* n : scene->children_named("node")) walk(*n, axis, 0, false, false, 0, false);

    // Geometry that never carried a detail marker (old exporters, prefab helpers) belongs to the most detailed
    // level. It used to land in bucket 0, which sorts last: it was only drawn from a few pixels up, so the
    // camera close-up showed a far LOD / a mixed model. Merge it into the top bucket instead.
    if (by_detail.size() > 1) {
        auto zero = by_detail.find(0);
        auto top = by_detail.begin(); // greatest detail size (std::greater<int>)
        if (zero != by_detail.end() && zero != top && top->first > 0) {
            for (auto& sm : zero->second.submeshes) {
                DaeSubmesh* dst = nullptr;
                for (auto& s : top->second.submeshes)
                    if (s.material == sm.material) { dst = &s; break; }
                if (!dst) {
                    top->second.submeshes.push_back(std::move(sm));
                    continue;
                }
                const auto base = static_cast<std::uint32_t>(dst->vertices.size());
                for (const auto& v : sm.vertices) dst->vertices.push_back(v);
                for (const auto i : sm.indices) dst->indices.push_back(base + i);
            }
            by_detail.erase(zero);
        }
    }

    for (auto& [d, lod] : by_detail) {
        if (lod.submeshes.empty()) continue;
        if (d < 0) shape.collision.push_back(std::move(lod));
        else shape.lods.push_back(std::move(lod));
    }
    for (auto& [d, lod] : by_billboard) {
        if (lod.submeshes.empty()) continue;
        shape.has_billboard = true;
        if (d > 0 && shape.billboards.empty()) shape.billboard_px = d;
        shape.billboards.push_back(std::move(lod));
    }
    // A shape whose only visual is the impostor plane (some far-LOD-only props) must still be drawable.
    if (shape.lods.empty() && !shape.billboards.empty()) {
        shape.lods = std::move(shape.billboards);
        shape.billboards.clear();
    }
    // bounds from the most detailed LOD
    bool first = true;
    for (const auto& sm : shape.lods.empty() ? std::vector<DaeSubmesh>{} : shape.lods.front().submeshes)
        for (const auto& v : sm.vertices)
            for (int k = 0; k < 3; ++k) {
                if (first) {
                    shape.bounds_min[k] = shape.bounds_max[k] = v.pos[k];
                } else {
                    shape.bounds_min[k] = (std::min)(shape.bounds_min[k], v.pos[k]);
                    shape.bounds_max[k] = (std::max)(shape.bounds_max[k], v.pos[k]);
                }
                if (k == 2) first = false;
            }
    const float dx = shape.bounds_max[0] - shape.bounds_min[0], dy = shape.bounds_max[1] - shape.bounds_min[1],
                dz = shape.bounds_max[2] - shape.bounds_min[2];
    shape.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    return shape;
}

} // namespace bng
