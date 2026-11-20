#include "beamng/bng_json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bng {

namespace {
const JsonValue kNull{};
const std::string kEmpty;

struct Reader {
    std::string_view s;
    std::size_t i = 0;
    std::string err;

    bool fail(const char* m) {
        if (err.empty()) err = m;
        return false;
    }
    void skip_ws() {
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' ||
                                    static_cast<unsigned char>(s[i]) == 0xEF || static_cast<unsigned char>(s[i]) == 0xBB ||
                                    static_cast<unsigned char>(s[i]) == 0xBF))
                ++i;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
                i = (std::min)(s.size(), i + 2);
                continue;
            }
            break;
        }
    }
    static void put_utf8(std::string& o, std::uint32_t cp) {
        if (cp < 0x80) o.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool parse_string(std::string& out) {
        const char q = s[i];
        ++i;
        while (i < s.size() && s[i] != q) {
            char c = s[i++];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i >= s.size()) return fail("bad escape");
            c = s[i++];
            switch (c) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                if (i + 4 > s.size()) return fail("bad \\u");
                std::uint32_t cp = static_cast<std::uint32_t>(std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16));
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    const std::uint32_t lo =
                        static_cast<std::uint32_t>(std::strtoul(std::string(s.substr(i + 2, 4)).c_str(), nullptr, 16));
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                put_utf8(out, cp);
                break;
            }
            default: out.push_back(c); break;
            }
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i;
        return true;
    }
    bool parse_value(JsonValue& v, int depth) {
        if (depth > 256) return fail("too deep");
        skip_ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            ++i;
            v = JsonValue::make_object();
            for (;;) {
                skip_ws();
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                std::string key;
                if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                    if (!parse_string(key)) return false;
                } else {
                    // bare key (lenient)
                    const std::size_t b = i;
                    while (i < s.size() && s[i] != ':' && s[i] != ' ' && s[i] != '\t') ++i;
                    key = std::string(s.substr(b, i - b));
                    if (key.empty()) return fail("expected key");
                }
                skip_ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                ++i;
                JsonValue child;
                if (!parse_value(child, depth + 1)) return false;
                v.members().emplace_back(std::move(key), std::move(child));
                skip_ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                // BeamNG sometimes omits commas between members in hand-edited files.
                if (i < s.size() && s[i] == '"') continue;
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i;
            v = JsonValue::make_array();
            for (;;) {
                skip_ws();
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                JsonValue child;
                if (!parse_value(child, depth + 1)) return false;
                v.push_back(std::move(child));
                skip_ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"' || c == '\'') {
            std::string str;
            if (!parse_string(str)) return false;
            v = JsonValue::make_string(std::move(str));
            return true;
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; v = JsonValue::make_bool(true); return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; v = JsonValue::make_bool(false); return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; v = JsonValue::make_null(); return true; }
        // numbers (also NaN / inf / -inf / 1.#INF that Torque may write)
        const std::size_t b = i;
        while (i < s.size()) {
            const char d = s[i];
            if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E' || d == '#' ||
                d == 'I' || d == 'N' || d == 'F' || d == 'n' || d == 'a' || d == 'i' || d == 'f' || d == 'A')
                ++i;
            else
                break;
        }
        if (i == b) return fail("unexpected character");
        v = JsonValue::make_number_text(std::string(s.substr(b, i - b)));
        return true;
    }
};

void write_string(std::string& o, const std::string& s) {
    o.push_back('"');
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            } else {
                o.push_back(ch);
            }
        }
    }
    o.push_back('"');
}

void write_value(std::string& o, const JsonValue& v, int indent, int level) {
    auto nl = [&](int lv) {
        if (indent <= 0) return;
        o.push_back('\n');
        o.append(static_cast<std::size_t>(lv * indent), ' ');
    };
    switch (v.type()) {
    case JsonValue::Type::Null: o += "null"; break;
    case JsonValue::Type::Bool: o += v.as_bool() ? "true" : "false"; break;
    case JsonValue::Type::Number: {
        const auto& lit = v.as_string();
        o += lit.empty() ? JsonValue::format_number(v.as_number()) : lit;
        break;
    }
    case JsonValue::Type::String: write_string(o, v.as_string()); break;
    case JsonValue::Type::Array: {
        o.push_back('[');
        // numeric arrays stay on one line even in pretty mode (BeamNG style)
        bool flat = indent <= 0;
        if (!flat) {
            flat = true;
            for (const auto& e : v.items())
                if (e.is_array() || e.is_object()) { flat = false; break; }
        }
        for (std::size_t k = 0; k < v.size(); ++k) {
            if (k) o.push_back(',');
            if (!flat) nl(level + 1);
            write_value(o, v[k], indent, level + 1);
        }
        if (!flat && v.size()) nl(level);
        o.push_back(']');
        break;
    }
    case JsonValue::Type::Object: {
        o.push_back('{');
        bool first = true;
        for (const auto& [k, e] : v.members()) {
            if (!first) o.push_back(',');
            first = false;
            nl(level + 1);
            write_string(o, k);
            o += indent > 0 ? ": " : ":";
            write_value(o, e, indent, level + 1);
        }
        if (!v.members().empty()) nl(level);
        o.push_back('}');
        break;
    }
    }
}
} // namespace

JsonValue JsonValue::make_bool(bool b) {
    JsonValue v;
    v.type_ = Type::Bool;
    v.b_ = b;
    return v;
}
JsonValue JsonValue::make_number(double d) {
    JsonValue v;
    v.type_ = Type::Number;
    v.num_ = d;
    return v;
}
JsonValue JsonValue::make_number_text(std::string text) {
    JsonValue v;
    v.type_ = Type::Number;
    const char* p = text.c_str();
    if (text.find("NaN") != std::string::npos || text.find("nan") != std::string::npos) v.num_ = std::nan("");
    else if (text.find("INF") != std::string::npos || text.find("inf") != std::string::npos)
        v.num_ = text[0] == '-' ? -HUGE_VAL : HUGE_VAL;
    else
        v.num_ = std::strtod(p, nullptr);
    v.str_ = std::move(text);
    return v;
}
JsonValue JsonValue::make_string(std::string s) {
    JsonValue v;
    v.type_ = Type::String;
    v.str_ = std::move(s);
    return v;
}
JsonValue JsonValue::make_array() {
    JsonValue v;
    v.type_ = Type::Array;
    return v;
}
JsonValue JsonValue::make_object() {
    JsonValue v;
    v.type_ = Type::Object;
    return v;
}

bool JsonValue::as_bool(bool def) const {
    if (type_ == Type::Bool) return b_;
    if (type_ == Type::Number) return num_ != 0.0;
    if (type_ == Type::String) return str_ == "1" || str_ == "true";
    return def;
}
double JsonValue::as_number(double def) const { return type_ == Type::Number ? num_ : def; }
double JsonValue::as_number_lenient(double def) const {
    if (type_ == Type::Number) return num_;
    if (type_ == Type::String && !str_.empty()) {
        char* end = nullptr;
        const double d = std::strtod(str_.c_str(), &end);
        if (end != str_.c_str()) return d;
    }
    if (type_ == Type::Bool) return b_ ? 1.0 : 0.0;
    return def;
}
const std::string& JsonValue::as_string() const {
    return (type_ == Type::String || type_ == Type::Number) ? str_ : kEmpty;
}

std::size_t JsonValue::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}
const JsonValue& JsonValue::operator[](std::size_t i) const { return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : kNull; }
JsonValue& JsonValue::at(std::size_t i) { return arr_.at(i); }
void JsonValue::push_back(JsonValue v) {
    if (type_ != Type::Array) {
        *this = make_array();
    }
    arr_.push_back(std::move(v));
}

const JsonValue* JsonValue::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& [k, v] : obj_)
        if (k == key) return &v;
    return nullptr;
}
JsonValue* JsonValue::find(std::string_view key) {
    if (type_ != Type::Object) return nullptr;
    for (auto& [k, v] : obj_)
        if (k == key) return &v;
    return nullptr;
}
const JsonValue& JsonValue::get(std::string_view key) const {
    const auto* p = find(key);
    return p ? *p : kNull;
}
JsonValue& JsonValue::set(std::string_view key, JsonValue v) {
    if (type_ != Type::Object) *this = make_object();
    for (auto& [k, e] : obj_)
        if (k == key) {
            e = std::move(v);
            return e;
        }
    obj_.emplace_back(std::string(key), std::move(v));
    return obj_.back().second;
}
bool JsonValue::erase(std::string_view key) {
    for (auto it = obj_.begin(); it != obj_.end(); ++it)
        if (it->first == key) {
            obj_.erase(it);
            return true;
        }
    return false;
}

std::string JsonValue::format_number(double d) {
    if (std::isnan(d)) return "0";
    if (std::isinf(d)) return d > 0 ? "1e+30" : "-1e+30";
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", d);
        return buf;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.9g", d);
    std::string s = buf;
    if (s.find('e') != std::string::npos) {
        std::snprintf(buf, sizeof(buf), "%.9f", d);
        s = buf;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

JsonParseResult json_parse(std::string_view text, JsonValue& out) {
    Reader r{text};
    JsonParseResult res;
    if (!r.parse_value(out, 0)) {
        res.error = r.err;
        res.error_offset = r.i;
        return res;
    }
    res.ok = true;
    return res;
}

std::string json_write_compact(const JsonValue& v) {
    std::string o;
    write_value(o, v, 0, 0);
    return o;
}
std::string json_write_pretty(const JsonValue& v, int indent) {
    std::string o;
    write_value(o, v, indent, 0);
    return o;
}

std::vector<JsonValue> json_parse_lines(std::string_view text, std::vector<std::string>* errors) {
    std::vector<JsonValue> out;
    std::size_t p = 0;
    int line_no = 0;
    while (p < text.size()) {
        std::size_t e = text.find('\n', p);
        if (e == std::string_view::npos) e = text.size();
        std::string_view line = text.substr(p, e - p);
        p = e + 1;
        ++line_no;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        std::size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
        line.remove_prefix(lead);
        if (line.empty()) continue;
        JsonValue v;
        const auto r = json_parse(line, v);
        if (!r.ok) {
            if (errors) errors->push_back("line " + std::to_string(line_no) + ": " + r.error);
            continue;
        }
        out.push_back(std::move(v));
    }
    return out;
}

} // namespace bng
