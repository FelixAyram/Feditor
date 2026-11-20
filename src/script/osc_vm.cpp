#include "omsi/script/osc_vm.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include "omsi/sound/sound_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace omsi::script {
namespace {

std::string trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// One lexical item of a script line (OMSI: words, "strings", (X.Y.name) accesses, {keywords}).
struct Tok {
    enum Kind : char { Word = 'w', Str = 's', Paren = 'p', Brace = 'b' };
    Kind kind = Word;
    std::string text; // Str: content without quotes; Paren: including ( ); Brace: including { }
};

// Tokenizer rules (compile.rs / Info_scriptcode.txt): `'` starts a comment outside strings;
// an access ends at its matching bracket, so names such as `Font_(8-1)x3` stay whole.
std::vector<Tok> tokenize(std::string_view line) {
    std::vector<Tok> out;
    const std::size_t n = line.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '\'') break;
        if (c == '"') {
            const std::size_t j = line.find('"', i + 1);
            const std::size_t end = (j == std::string_view::npos) ? n : j;
            out.push_back({Tok::Str, std::string(line.substr(i + 1, end - i - 1))});
            i = (j == std::string_view::npos) ? n : j + 1;
            continue;
        }
        if (c == '(') {
            int depth = 0;
            std::size_t j = i;
            for (; j < n; ++j) {
                if (line[j] == '(') ++depth;
                else if (line[j] == ')' && --depth == 0) break;
            }
            const std::size_t end = (j < n) ? j + 1 : n;
            out.push_back({Tok::Paren, std::string(line.substr(i, end - i))});
            i = end;
            continue;
        }
        if (c == '{') {
            const std::size_t j = line.find('}', i + 1);
            const std::size_t end = (j == std::string_view::npos) ? n : j + 1;
            out.push_back({Tok::Brace, std::string(line.substr(i, end - i))});
            i = end;
            continue;
        }
        std::size_t j = i;
        while (j < n && !std::isspace(static_cast<unsigned char>(line[j])) && line[j] != '"' && line[j] != '(' &&
               line[j] != '{' && line[j] != '\'')
            ++j;
        if (j == i) j = i + 1;
        out.push_back({Tok::Word, std::string(line.substr(i, j - i))});
        i = j;
    }
    return out;
}

// OMSI reads a script as one text: lines are joined with a space while a "string" is open, so
// string literals may run over several lines (Volvopmon's pmon_txt displays). A whole-line
// `'` comment is dropped. Returns the logical lines the compiler sees.
std::vector<std::string> logical_lines(const std::vector<std::string>& raw) {
    std::vector<std::string> out;
    std::string acc;
    bool in_str = false;
    for (const auto& r : raw) {
        if (!in_str) {
            const auto t = trim(r);
            if (t.empty() || t.front() == '\'') continue;
        }
        if (!acc.empty()) acc += ' ';
        acc += r;
        for (const char c : r) {
            if (!in_str && c == '\'') break; // comment to the end of the line
            if (c == '"') in_str = !in_str;
        }
        if (!in_str) {
            out.push_back(std::move(acc));
            acc.clear();
        }
    }
    if (!acc.empty()) out.push_back(std::move(acc));
    return out;
}

bool parse_number(const std::string& t, float& out) {
    if (t.empty()) return false;
    const unsigned char c0 = static_cast<unsigned char>(t[0]);
    if (!(std::isdigit(c0) || c0 == '-' || c0 == '+' || c0 == '.')) return false;
    char* end = nullptr;
    const float v = std::strtof(t.c_str(), &end);
    if (end != t.c_str() + t.size()) return false;
    out = v;
    return true;
}

float parse_float_or_zero(const std::string& s) {
    const std::string t = trim(s);
    char* end = nullptr;
    const float v = std::strtof(t.c_str(), &end);
    return (end == t.c_str()) ? 0.f : v;
}

int parse_int_or_zero(const std::string& s) {
    const std::string t = trim(s);
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 10);
    return (end == t.c_str()) ? 0 : static_cast<int>(v);
}

int round_half_even(float v) { return static_cast<int>(std::nearbyint(v)); }

// $IntToStrEnh: format "<pad><width>", e.g. "02" -> zero padded to 2; overflow -> '#'.
std::string int_to_str_enh(float value, const std::string& fmt) {
    if (fmt.size() < 2) return "ERROR";
    const char pad = fmt[0];
    const int width = parse_int_or_zero(fmt.substr(1));
    if (width <= 0) return {};
    const std::string text = std::to_string(static_cast<int>(std::trunc(value)));
    if (static_cast<int>(text.size()) > width) return text.substr(0, static_cast<std::size_t>(width - 1)) + "#";
    return std::string(static_cast<std::size_t>(width) - text.size(), pad) + text;
}

// $SetLengthR/L/C: right-justified (pads on the left), left-justified, centred.
std::string set_len_r(const std::string& s, int n) {
    if (n <= 0) return {};
    const int len = static_cast<int>(s.size());
    if (len < n) return std::string(static_cast<std::size_t>(n - len), ' ') + s;
    return s.substr(static_cast<std::size_t>(len - n));
}

std::string set_len_l(const std::string& s, int n) {
    if (n <= 0) return {};
    const int len = static_cast<int>(s.size());
    if (len < n) return s + std::string(static_cast<std::size_t>(n - len), ' ');
    return s.substr(0, static_cast<std::size_t>(n));
}

std::string set_len_c(const std::string& s, int n) {
    if (n <= 0) return {};
    const int len = static_cast<int>(s.size());
    const int diff = n - len;
    if (diff >= 0) {
        const int left = diff / 2;
        return std::string(static_cast<std::size_t>(left), ' ') + s +
               std::string(static_cast<std::size_t>(diff - left), ' ');
    }
    const int left = (-diff) / 2;
    return s.substr(static_cast<std::size_t>(left), static_cast<std::size_t>(n));
}

// $* : repeat the string until it fills round(value) characters (value >= 1000 -> ERROR).
std::string repeat_to_length(const std::string& s, float value) {
    if (value >= 1000.f) return "ERROR";
    const int n = round_half_even(value);
    if (n <= 0 || s.empty()) return {};
    std::string out;
    out.reserve(static_cast<std::size_t>(n));
    while (static_cast<int>(out.size()) < n) out += s;
    out.resize(static_cast<std::size_t>(n));
    return out;
}

// Operator and builtin spellings (compile order of _Unit86.sub_005D1E68).
const std::unordered_map<std::string, OscOpcode>& word_ops() {
    static const std::unordered_map<std::string, OscOpcode> kOps{
        {"+", OscOpcode::Add},          {"-", OscOpcode::Sub},          {"*", OscOpcode::Mul},
        {"/", OscOpcode::Div},          {"%", OscOpcode::Mod},          {"=", OscOpcode::Eq},
        {"<", OscOpcode::Lt},           {">", OscOpcode::Gt},           {"<=", OscOpcode::Le},
        {">=", OscOpcode::Ge},          {"&&", OscOpcode::And},         {"||", OscOpcode::Or},
        {"!", OscOpcode::Not},          {"/-/", OscOpcode::Neg},        {"abs", OscOpcode::Abs},
        {"sqr", OscOpcode::Sqr},        {"sqrt", OscOpcode::Sqrt},      {"sin", OscOpcode::Sin},
        {"arcsin", OscOpcode::ArcSin},  {"arctan", OscOpcode::ArcTan},  {"exp", OscOpcode::Exp},
        {"sgn", OscOpcode::Sgn},        {"trunc", OscOpcode::Trunc},    {"min", OscOpcode::Min},
        {"max", OscOpcode::Max},        {"random", OscOpcode::Random},  {"d", OscOpcode::Dup},
        {"pi", OscOpcode::Pi},          {"$+", OscOpcode::StrCat},      {"$*", OscOpcode::StrRepeat},
        {"$=", OscOpcode::StrEq},       {"$<", OscOpcode::StrLt},       {"$>", OscOpcode::StrGt},
        {"$<=", OscOpcode::StrLe},      {"$>=", OscOpcode::StrGe},      {"$d", OscOpcode::StrDup},
        {"$length", OscOpcode::StrLength}, {"$StrToFloat", OscOpcode::StrToFloat},
        {"$IntToStr", OscOpcode::IntToStr}, {"$IntToStrEnh", OscOpcode::IntToStrEnh},
        {"$cutBegin", OscOpcode::StrCutBegin}, {"$cutEnd", OscOpcode::StrCutEnd},
        {"$SetLengthR", OscOpcode::StrSetLenR}, {"$SetLengthL", OscOpcode::StrSetLenL},
        {"$SetLengthC", OscOpcode::StrSetLenC}, {"$RemoveSpaces", OscOpcode::StrRemSpaces},
        {"$msg", OscOpcode::StrMsg},
    };
    return kOps;
}

struct SysEntry {
    const char* name;
    OscSysId id;
};

constexpr SysEntry kSysVars[] = {
    {"Timegap", OscSysId::Timegap},
    {"GetTime", OscSysId::GetTime},
    {"NoSound", OscSysId::NoSound},
    {"Pause", OscSysId::Pause},
    {"Time", OscSysId::Time},
    {"Day", OscSysId::Day},
    {"Month", OscSysId::Month},
    {"Year", OscSysId::Year},
    {"DayOfYear", OscSysId::DayOfYear},
    {"mouse_x", OscSysId::MouseX},
    {"mouse_y", OscSysId::MouseY},
    {"PrecipType", OscSysId::PrecipType},
    {"PrecipRate", OscSysId::PrecipRate},
    {"coll_pos_x", OscSysId::CollPosX},
    {"coll_pos_y", OscSysId::CollPosY},
    {"coll_pos_z", OscSysId::CollPosZ},
    {"coll_energy", OscSysId::CollEnergy},
    {"Weather_Temperature", OscSysId::WeatherTemperature},
    {"Weather_AbsHum", OscSysId::WeatherAbsHum},
    {"wearlifespan", OscSysId::WearLifespan},
    {"AutoClutch", OscSysId::AutoClutch},
    {"SunAlt", OscSysId::SunAlt},
};

// Names from program/callbacklist_*.txt (lower case). Filled by load_known_lists().
std::unordered_set<std::string>& known_callbacks() {
    static std::unordered_set<std::string> names;
    return names;
}
std::string path_utf8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string& known_lists_root() {
    static std::string root;
    return root;
}

} // namespace

void load_known_lists(const std::filesystem::path& omsi_root) {
    if (omsi_root.empty() || known_lists_root() == path_utf8(omsi_root)) return;
    known_lists_root() = path_utf8(omsi_root);
    known_callbacks().clear();
    const auto program = omsi_root / "program";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(program, ec)) {
        if (ec) break;
        const auto fname_u8 = entry.path().filename().u8string(); const std::string fname(fname_u8.begin(), fname_u8.end());
        if (!(fname.size() > 13 && fname.compare(0, 13, "callbacklist_") == 0)) continue;
        for (const auto& raw : cfg::read_cfg_lines(entry.path())) {
            const auto name = trim(raw);
            if (name.empty() || name.front() == '\'') continue;
            known_callbacks().insert(lower(name));
        }
    }
    omsi::log::info("OMSI script: callbacks known=" + std::to_string(known_callbacks().size()));
}

bool is_known_callback(std::string_view name) { return known_callbacks().count(lower(name)) != 0; }

OscSysId sysvar_from_name(std::string_view name) {
    for (const auto& e : kSysVars)
        if (iequals(e.name, name)) return e.id;
    return OscSysId::Unknown;
}

// ---------------------------------------------------------------- compile

void OscVM::note_unsupported(const std::string& what) {
    if (unsupported_set_.size() >= 256) return;
    if (unsupported_set_.insert(what).second) unsupported_.push_back(what);
}

void OscVM::compile_block(const std::vector<std::string>& lines, std::vector<OscOp>& out, const std::string& ctx,
                          const std::unordered_map<std::string, int>& macro_index) {
    (void)macro_index; // macro calls are resolved after every block exists (resolve_macros)
    struct IfFrame {
        std::size_t jif = 0;
        long long jmp = -1;
    };
    std::vector<IfFrame> frames;

    for (const auto& line : lines) {
        for (const auto& tok : tokenize(line)) {
            if (tok.kind == Tok::Brace) {
                const std::string b = lower(tok.text);
                if (b == "{if}") {
                    out.push_back(OscOp{.op = OscOpcode::JmpIfFalse, .i = -1});
                    frames.push_back(IfFrame{out.size() - 1, -1});
                } else if (b == "{else}") {
                    // OMSI's {else} with no open {if} (or a second {else}) builds nothing.
                    if (frames.empty() || frames.back().jmp >= 0) {
                        ++stats_.warnings;
                        continue;
                    }
                    out.push_back(OscOp{.op = OscOpcode::Jmp, .i = -1});
                    frames.back().jmp = static_cast<long long>(out.size() - 1);
                    out[frames.back().jif].i = static_cast<int>(out.size());
                } else if (b == "{endif}") {
                    // OMSI's {endif} is a join point that needs no open {if} (the decompile
                    // creates the node either way): with nothing open it does nothing.
                    if (frames.empty()) {
                        ++stats_.warnings;
                        continue;
                    }
                    const IfFrame fr = frames.back();
                    frames.pop_back();
                    const int target = static_cast<int>(out.size());
                    if (fr.jmp >= 0) out[static_cast<std::size_t>(fr.jmp)].i = target;
                    else out[fr.jif].i = target;
                } else {
                    ++stats_.unknown_tokens;
                    note_unsupported(tok.text + " (" + ctx + ")");
                }
                continue;
            }

            if (tok.kind == Tok::Str) {
                out.push_back(OscOp{.op = OscOpcode::PushStr, .s = tok.text});
                continue;
            }

            if (tok.kind == Tok::Paren) {
                const std::string& t = tok.text;
                const std::string inner = (t.size() >= 2 && t.back() == ')') ? t.substr(1, t.size() - 2) : std::string{};
                if (inner.size() < 5 || inner[1] != '.' || inner[3] != '.') {
                    ++stats_.unknown_tokens;
                    note_unsupported(t + " (" + ctx + ")");
                    continue;
                }
                const char x = static_cast<char>(std::toupper(static_cast<unsigned char>(inner[0])));
                const char y = static_cast<char>(std::toupper(static_cast<unsigned char>(inner[2])));
                const std::string name = inner.substr(4);
                if ((x == 'L' || x == 'S') && y == 'L') {
                    out.push_back(OscOp{.op = x == 'L' ? OscOpcode::LoadF : OscOpcode::StoreF, .s = name});
                } else if ((x == 'L' || x == 'S') && y == '$') {
                    out.push_back(OscOp{.op = x == 'L' ? OscOpcode::LoadStr : OscOpcode::StoreStr, .s = name});
                } else if ((x == 'L' || x == 'S') && y == 'S') {
                    const OscSysId id = sysvar_from_name(name);
                    if (id == OscSysId::Unknown) {
                        ++stats_.unknown_sysvars;
                        note_unsupported(t + " (" + ctx + ")");
                        continue;
                    }
                    out.push_back(OscOp{.op = x == 'L' ? OscOpcode::LoadSys : OscOpcode::StoreSys,
                                        .i = static_cast<int>(id)});
                } else if (x == 'C' && y == 'L') {
                    out.push_back(OscOp{.op = OscOpcode::LoadC, .s = name});
                } else if (x == 'F' && y == 'L') {
                    out.push_back(OscOp{.op = OscOpcode::Curve, .s = name});
                } else if (x == 'M' && y == 'L') {
                    out.push_back(OscOp{.op = OscOpcode::CallM, .s = name});
                } else if (x == 'M' && y == 'V') {
                    if (!is_known_callback(name)) {
                        ++stats_.unknown_callbacks;
                        note_unsupported("callback " + name + " (" + ctx + ")");
                    }
                    out.push_back(OscOp{.op = OscOpcode::CallV, .s = name});
                } else if (x == 'T' && (y == 'L' || y == 'F' || y == 'S')) {
                    out.push_back(OscOp{.op = OscOpcode::TrigSound, .i = y == 'F' ? 1 : 0, .s = name});
                } else {
                    ++stats_.unknown_tokens;
                    note_unsupported(t + " (" + ctx + ")");
                }
                continue;
            }

            // Word.
            const std::string& w = tok.text;
            if (const auto it = word_ops().find(w); it != word_ops().end()) {
                out.push_back(OscOp{.op = it->second});
                continue;
            }
            if (w.size() == 2 && (w[0] == 's' || w[0] == 'l') && w[1] >= '0' && w[1] <= '9') {
                out.push_back(OscOp{.op = w[0] == 's' ? OscOpcode::SaveS : OscOpcode::LoadS, .i = w[1] - '0'});
                continue;
            }
            float v = 0.f;
            if (parse_number(w, v)) {
                out.push_back(OscOp{.op = OscOpcode::PushF, .f = v});
                continue;
            }
            // OMSI's token chain ignores unknown `$` words (no calc block is created).
            if (!w.empty() && w[0] == '$') {
                ++stats_.warnings;
                continue;
            }
            ++stats_.unknown_tokens;
            note_unsupported(w + " (" + ctx + ")");
        }
    }

    // Blocks that end inside an open {if} close it (OMSI: the macro ends the chain).
    while (!frames.empty()) {
        const IfFrame fr = frames.back();
        frames.pop_back();
        const int target = static_cast<int>(out.size());
        if (fr.jmp >= 0) out[static_cast<std::size_t>(fr.jmp)].i = target;
        else out[fr.jif].i = target;
    }
    stats_.ir_ops += static_cast<int>(out.size());
}

void OscVM::resolve_macros() {
    std::unordered_set<std::string> missing;
    for (auto& m : macros_) {
        for (auto& op : m.code) {
            if (op.op != OscOpcode::CallM) continue;
            const auto it = macro_index_.find("macro:" + lower(op.s));
            if (it != macro_index_.end()) {
                op.i = it->second;
            } else {
                op.i = -1;
                ++stats_.missing_macros;
                if (missing.insert(op.s).second) note_unsupported("macro " + op.s + " not defined");
            }
        }
    }
}

bool OscVM::load(const std::filesystem::path& bus_dir, const std::vector<std::filesystem::path>& script_files,
                 const VarRegistry& vars) {
    macros_.clear();
    macro_index_.clear();
    init_idx_ = frame_idx_ = frame_ai_idx_ = -1;
    stats_ = {};
    unsupported_.clear();
    unsupported_set_.clear();
    stubbed_callbacks_.clear();

    struct Raw {
        std::string name;
        std::vector<std::string> lines;
    };
    std::vector<Raw> raws;
    std::unordered_set<std::string> seen;

    for (const auto& rel : script_files) {
        const auto path = bus_dir / rel;
        const auto lines = cfg::read_cfg_lines(path);
        if (lines.empty()) {
            omsi::log::error("OMSI Script: could not read a script file");
            ++stats_.errors;
            continue;
        }
        int cur = -1;
        for (const auto& raw : logical_lines(lines)) {
            const auto t = trim(raw);
            if (t.empty() || t.front() == '\'') continue;
            const auto toks = tokenize(t);
            if (toks.size() == 1 && toks[0].kind == Tok::Brace) {
                const std::string b = lower(toks[0].text);
                if (b == "{end}") {
                    cur = -1;
                    continue;
                }
                if (b == "{if}" || b == "{else}" || b == "{endif}") {
                    if (cur >= 0) raws[static_cast<std::size_t>(cur)].lines.push_back(t);
                    continue;
                }
                std::string name;
                if (b == "{init}") name = "init";
                else if (b == "{frame}") name = "frame";
                else if (b == "{frame_ai}") name = "frame_ai";
                else if (b.rfind("{macro:", 0) == 0 && b.back() == '}') name = "macro:" + b.substr(7, b.size() - 8);
                else if (b.rfind("{trigger:", 0) == 0 && b.back() == '}') name = "trigger:" + b.substr(9, b.size() - 10);
                if (!name.empty()) {
                    // First definition in file order wins for macros/init/frame (OMSI keeps the first head it finds).
                    // Triggers: the LAST definition wins. Vehicles override a cockpit trigger in a later script, e.g.
                    // Metalpar Iguazu: engine.osc redefines cp_batterietrennschalter_toggle (key steps + starter)
                    // over cockpit.osc's plain battery toggle; running both turned the battery off on the 2nd press
                    // and the engine could never start. All blocks are kept; fire_trigger runs the last one.
                    const bool is_trigger = name.rfind("trigger:", 0) == 0;
                    if (is_trigger || seen.insert(name).second) {
                        raws.push_back(Raw{name, {}});
                        cur = static_cast<int>(raws.size()) - 1;
                    } else {
                        cur = -1;
                    }
                    continue;
                }
            }
            if (cur >= 0) raws[static_cast<std::size_t>(cur)].lines.push_back(t);
        }
        ++stats_.script_files;
    }

    macros_.reserve(raws.size());
    trigger_blocks_.clear();
    for (const auto& r : raws) {
        // Every {trigger:x} block is recorded in file order; fire_trigger runs the last definition (see parser).
        if (r.name.rfind("trigger:", 0) == 0) trigger_blocks_[r.name].push_back(static_cast<int>(macros_.size()));
        macro_index_[r.name] = static_cast<int>(macros_.size());
        macros_.push_back(OscMacro{.name = r.name, .code = {}});
        if (r.name.rfind("trigger:", 0) == 0) ++stats_.trigger_count;
        else if (r.name == "init") ++stats_.init_macros;
        else if (r.name == "frame") ++stats_.frame_macros;
        else if (r.name == "frame_ai") ++stats_.frame_ai_macros;
        else ++stats_.macro_count;
    }
    for (std::size_t i = 0; i < raws.size(); ++i) compile_block(raws[i].lines, macros_[i].code, raws[i].name, macro_index_);
    resolve_macros();

    if (const auto it = macro_index_.find("init"); it != macro_index_.end()) init_idx_ = it->second;
    if (const auto it = macro_index_.find("frame"); it != macro_index_.end()) frame_idx_ = it->second;
    if (const auto it = macro_index_.find("frame_ai"); it != macro_index_.end()) frame_ai_idx_ = it->second;
    if (frame_ai_idx_ < 0) frame_ai_idx_ = frame_idx_; // OMSI: a missing {frame_ai} falls back to {frame}

    // Load-time checks against the variable registry (unknown names still work at run time).
    std::unordered_set<std::string> unknown_names;
    std::unordered_set<std::string> callbacks_used;
    for (const auto& m : macros_) {
        for (const auto& op : m.code) {
            switch (op.op) {
            case OscOpcode::LoadF:
            case OscOpcode::StoreF:
                if (!vars.float_index(op.s) && unknown_names.insert("L:" + lower(op.s)).second) ++stats_.unknown_vars;
                break;
            case OscOpcode::LoadStr:
            case OscOpcode::StoreStr:
                if (!vars.string_index(op.s) && unknown_names.insert("$:" + lower(op.s)).second) ++stats_.unknown_vars;
                break;
            case OscOpcode::CallV:
                if (is_known_callback(op.s)) callbacks_used.insert(lower(op.s));
                break;
            default:
                break;
            }
        }
    }
    for (const auto& c : callbacks_used) stubbed_callbacks_.push_back(c);
    std::sort(stubbed_callbacks_.begin(), stubbed_callbacks_.end());

    omsi::log::info("OMSI Script VM: files=" + std::to_string(stats_.script_files) +
                    " macros=" + std::to_string(stats_.macro_count) +
                    " triggers=" + std::to_string(stats_.trigger_count) +
                    " init=" + std::to_string(stats_.init_macros) +
                    " frame=" + std::to_string(stats_.frame_macros) +
                    " frame_ai=" + std::to_string(stats_.frame_ai_macros) +
                    " ir_ops=" + std::to_string(stats_.ir_ops) +
                    " unsupported=" + std::to_string(unsupported_.size()));
    return stats_.script_files > 0;
}

void OscVM::bind_vars(std::vector<float>* public_floats, const VarRegistry* vars) {
    floats_ = public_floats;
    vars_ = vars;
}

void OscVM::bind_strings(std::vector<std::string>* public_strings) { strings_ = public_strings; }
void OscVM::bind_consts(const std::unordered_map<std::string, float>* consts) { consts_ = consts; }
void OscVM::bind_curves(const std::unordered_map<std::string, ConstCurve>* curves) { curves_ = curves; }
void OscVM::bind_sound(omsi::sound::SoundRuntime* sound) { sound_ = sound; }
void OscVM::bind_system_vars(float weather_temperature_c) { sys_.weather_temperature = weather_temperature_c; }
void OscVM::set_system_state(const OscSystemState& state) { sys_ = state; }

bool OscVM::has_trigger(const std::string& name) const {
    return macro_index_.count("trigger:" + lower(name)) != 0;
}

void OscVM::run_entry(int index) {
    if (index < 0) return;
    fstack_.fill(0.f);
    for (auto& s : sstack_) s.clear();
    active_macro_.clear();
    run_macro_index(index);
}

void OscVM::run_init() { run_entry(init_idx_); }

void OscVM::run_frame(float timegap_sec) {
    sys_.timegap = timegap_sec;
    run_entry((ai_mode_ && frame_ai_idx_ >= 0) ? frame_ai_idx_ : frame_idx_);
}

void OscVM::fire_trigger(const std::string& trigger_name) {
    const auto it = trigger_blocks_.find("trigger:" + lower(trigger_name));
    if (std::getenv("OMSI_TRACE_TRIGGERS"))
        omsi::log::info("OSC trigger '" + trigger_name + "' blocks=" +
                        std::to_string(it == trigger_blocks_.end() ? 0 : it->second.size()));
    if (it == trigger_blocks_.end() || it->second.empty()) return;
    // Triggers run from outside a chain: they get clean stacks, and the caller's come back.
    const auto saved_f = fstack_;
    const auto saved_s = sstack_;
    const auto saved_scratch = scratch_;
    fstack_.fill(0.f);
    for (auto& s : sstack_) s.clear();
    scratch_.fill(0.f);
    const std::string prev = active_macro_;
    run_macro_index(it->second.back());
    active_macro_ = prev;
    fstack_ = saved_f;
    sstack_ = saved_s;
    scratch_ = saved_scratch;
}

bool OscVM::run_macro_index(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= macros_.size()) return false;
    if (call_depth_ >= 64) {
        omsi::log::error("OMSI Script: macro recursion limit in '" + macros_[static_cast<std::size_t>(index)].name + "'");
        return false;
    }
    ++call_depth_;
    const std::string prev = active_macro_;
    active_macro_ = macros_[static_cast<std::size_t>(index)].name;
    run_code(macros_[static_cast<std::size_t>(index)].code);
    active_macro_ = prev;
    --call_depth_;
    return true;
}

// ---------------------------------------------------------------- stacks

void OscVM::push_float(float v) {
    for (std::size_t i = kStackCap - 1; i > 0; --i) fstack_[i] = fstack_[i - 1];
    fstack_[0] = v;
}

float OscVM::pop_float() {
    const float v = fstack_[0];
    for (std::size_t i = 0; i + 1 < kStackCap; ++i) fstack_[i] = fstack_[i + 1];
    return v;
}

void OscVM::push_string(std::string v) {
    for (std::size_t i = kStackCap - 1; i > 0; --i) sstack_[i] = std::move(sstack_[i - 1]);
    sstack_[0] = std::move(v);
}

std::string OscVM::pop_string() {
    std::string v = std::move(sstack_[0]);
    for (std::size_t i = 0; i + 1 < kStackCap; ++i) sstack_[i] = std::move(sstack_[i + 1]);
    return v;
}

// ---------------------------------------------------------------- variables

float* OscVM::float_ref(const OscOp& op) {
    if (vars_ && floats_) {
        if (op.cache_key != static_cast<const void*>(vars_)) {
            const auto idx = vars_->float_index(op.s);
            op.cache_key = vars_;
            op.cache_idx = idx ? *idx : -1;
        }
        if (op.cache_idx >= 0 && static_cast<std::size_t>(op.cache_idx) < floats_->size())
            return &(*floats_)[static_cast<std::size_t>(op.cache_idx)];
    }
    return &extra_floats_[op.s];
}

std::string* OscVM::string_ref(const OscOp& op) {
    if (vars_ && strings_) {
        if (op.cache_key != static_cast<const void*>(vars_)) {
            const auto idx = vars_->string_index(op.s);
            op.cache_key = vars_;
            op.cache_idx = idx ? *idx : -1;
        }
        if (op.cache_idx >= 0 && static_cast<std::size_t>(op.cache_idx) < strings_->size())
            return &(*strings_)[static_cast<std::size_t>(op.cache_idx)];
    }
    return &extra_strings_[op.s];
}

float OscVM::get_const(const std::string& name) const {
    if (!consts_) return 0.f;
    if (const auto it = consts_->find(name); it != consts_->end()) return it->second;
    if (consts_lc_n_ != consts_->size()) {
        consts_lc_.clear();
        for (const auto& [k, v] : *consts_) consts_lc_.emplace(lower(k), v);
        consts_lc_n_ = consts_->size();
    }
    const auto it = consts_lc_.find(lower(name));
    return it != consts_lc_.end() ? it->second : 0.f;
}

float OscVM::eval_curve(const std::string& name, float x) const {
    if (!curves_) return 0.f;
    const ConstCurve* curve = nullptr;
    if (const auto it = curves_->find(name); it != curves_->end()) {
        curve = &it->second;
    } else {
        if (curves_lc_n_ != curves_->size()) {
            curves_lc_.clear();
            for (const auto& [k, v] : *curves_) curves_lc_.emplace(lower(k), &v);
            curves_lc_n_ = curves_->size();
        }
        if (const auto jt = curves_lc_.find(lower(name)); jt != curves_lc_.end()) curve = jt->second;
    }
    if (!curve || curve->points.empty()) return 0.f;
    const auto& pts = curve->points;
    if (x <= pts.front().x) return pts.front().y;
    if (x >= pts.back().x) return pts.back().y;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        if (x <= pts[i].x) {
            const float x0 = pts[i - 1].x;
            const float x1 = pts[i].x;
            const float t = x1 != x0 ? (x - x0) / (x1 - x0) : 0.f;
            return pts[i - 1].y + t * (pts[i].y - pts[i - 1].y);
        }
    }
    return pts.back().y;
}

float OscVM::load_sys(OscSysId id) const {
    switch (id) {
    case OscSysId::Timegap: return sys_.timegap;
    case OscSysId::GetTime: return sys_.get_time;
    case OscSysId::NoSound: return sys_.no_sound;
    case OscSysId::Pause: return sys_.pause;
    case OscSysId::Time: return sys_.time;
    case OscSysId::Day: return sys_.day;
    case OscSysId::Month: return sys_.month;
    case OscSysId::Year: return sys_.year;
    case OscSysId::DayOfYear: return sys_.day_of_year;
    case OscSysId::MouseX: return sys_.mouse_x;
    case OscSysId::MouseY: return sys_.mouse_y;
    case OscSysId::PrecipType: return sys_.precip_type;
    case OscSysId::PrecipRate: return sys_.precip_rate;
    case OscSysId::CollPosX: return sys_.coll_pos_x;
    case OscSysId::CollPosY: return sys_.coll_pos_y;
    case OscSysId::CollPosZ: return sys_.coll_pos_z;
    case OscSysId::CollEnergy: return sys_.coll_energy;
    case OscSysId::WeatherTemperature: return sys_.weather_temperature;
    case OscSysId::WeatherAbsHum: return sys_.weather_abs_hum;
    case OscSysId::WearLifespan: return sys_.wear_lifespan;
    case OscSysId::AutoClutch: return sys_.auto_clutch;
    case OscSysId::SunAlt: return sys_.sun_alt;
    default: return 0.f;
    }
}

void OscVM::store_sys(OscSysId id, float v) {
    switch (id) {
    case OscSysId::Timegap: sys_.timegap = v; break;
    case OscSysId::GetTime: sys_.get_time = v; break;
    case OscSysId::NoSound: sys_.no_sound = v; break;
    case OscSysId::Pause: sys_.pause = v; break;
    case OscSysId::Time: sys_.time = v; break;
    case OscSysId::Day: sys_.day = v; break;
    case OscSysId::Month: sys_.month = v; break;
    case OscSysId::Year: sys_.year = v; break;
    case OscSysId::DayOfYear: sys_.day_of_year = v; break;
    case OscSysId::MouseX: sys_.mouse_x = v; break;
    case OscSysId::MouseY: sys_.mouse_y = v; break;
    case OscSysId::PrecipType: sys_.precip_type = v; break;
    case OscSysId::PrecipRate: sys_.precip_rate = v; break;
    case OscSysId::CollPosX: sys_.coll_pos_x = v; break;
    case OscSysId::CollPosY: sys_.coll_pos_y = v; break;
    case OscSysId::CollPosZ: sys_.coll_pos_z = v; break;
    case OscSysId::CollEnergy: sys_.coll_energy = v; break;
    case OscSysId::WeatherTemperature: sys_.weather_temperature = v; break;
    case OscSysId::WeatherAbsHum: sys_.weather_abs_hum = v; break;
    case OscSysId::WearLifespan: sys_.wear_lifespan = v; break;
    case OscSysId::AutoClutch: sys_.auto_clutch = v; break;
    case OscSysId::SunAlt: sys_.sun_alt = v; break;
    default: break;
    }
}

// ---------------------------------------------------------------- interpreter

void OscVM::run_code(const std::vector<OscOp>& code) {
    const std::size_t n = code.size();
    std::size_t pc = 0;
    while (pc < n) {
        const OscOp& op = code[pc];
        switch (op.op) {
        case OscOpcode::Nop:
            break;
        case OscOpcode::PushF:
            push_float(op.f);
            break;
        case OscOpcode::PushStr:
            push_string(op.s);
            break;
        case OscOpcode::LoadF:
            push_float(*float_ref(op));
            break;
        case OscOpcode::StoreF:
            *float_ref(op) = fstack_[0];
            break;
        case OscOpcode::LoadStr:
            push_string(*string_ref(op));
            break;
        case OscOpcode::StoreStr:
            *string_ref(op) = sstack_[0];
            break;
        case OscOpcode::LoadC:
            push_float(get_const(op.s));
            break;
        case OscOpcode::Curve:
            fstack_[0] = eval_curve(op.s, fstack_[0]);
            break;
        case OscOpcode::LoadSys:
            push_float(load_sys(static_cast<OscSysId>(op.i)));
            break;
        case OscOpcode::StoreSys:
            store_sys(static_cast<OscSysId>(op.i), fstack_[0]);
            break;
        case OscOpcode::CallM:
            if (op.i >= 0) run_macro_index(op.i);
            break;
        case OscOpcode::CallV:
            if (callback_handler_) callback_handler_(op.s, *this);
            else push_float(0.f); // no host implementation: the engine's usual "no data" result
            break;
        case OscOpcode::TrigSound:
            if (sound_) {
                const std::string via = active_macro_.empty() ? std::string("osc:(T.L.)")
                                                              : ("osc:{" + active_macro_ + "}");
                sound_->fire_trigger(op.s, via);
            }
            break;
        case OscOpcode::SaveS:
            if (op.i >= 0 && op.i < 10) scratch_[static_cast<std::size_t>(op.i)] = fstack_[0];
            break;
        case OscOpcode::LoadS:
            push_float((op.i >= 0 && op.i < 10) ? scratch_[static_cast<std::size_t>(op.i)] : 0.f);
            break;
        case OscOpcode::Jmp:
            pc = op.i >= 0 ? static_cast<std::size_t>(op.i) : n;
            continue;
        case OscOpcode::JmpIfFalse:
            if (fstack_[0] == 0.f) {
                pc = op.i >= 0 ? static_cast<std::size_t>(op.i) : n;
                continue;
            }
            break;

        // float stack: binary ops pop once and write slot 0
        case OscOpcode::Add: { const float b = pop_float(); fstack_[0] += b; break; }
        case OscOpcode::Sub: { const float b = pop_float(); fstack_[0] -= b; break; }
        case OscOpcode::Mul: { const float b = pop_float(); fstack_[0] *= b; break; }
        case OscOpcode::Div: {
            const float b = pop_float();
            fstack_[0] = b != 0.f ? fstack_[0] / b : 0.f;
            break;
        }
        case OscOpcode::Mod: {
            const float b = pop_float();
            fstack_[0] = b != 0.f ? std::fmod(fstack_[0], b) : 0.f;
            break;
        }
        case OscOpcode::Eq: { const float b = pop_float(); fstack_[0] = fstack_[0] == b ? 1.f : 0.f; break; }
        case OscOpcode::Lt: { const float b = pop_float(); fstack_[0] = fstack_[0] < b ? 1.f : 0.f; break; }
        case OscOpcode::Gt: { const float b = pop_float(); fstack_[0] = fstack_[0] > b ? 1.f : 0.f; break; }
        case OscOpcode::Le: { const float b = pop_float(); fstack_[0] = fstack_[0] <= b ? 1.f : 0.f; break; }
        case OscOpcode::Ge: { const float b = pop_float(); fstack_[0] = fstack_[0] >= b ? 1.f : 0.f; break; }
        case OscOpcode::And: {
            const float b = pop_float();
            fstack_[0] = (fstack_[0] != 0.f && b != 0.f) ? 1.f : 0.f;
            break;
        }
        case OscOpcode::Or: {
            const float b = pop_float();
            fstack_[0] = (fstack_[0] != 0.f || b != 0.f) ? 1.f : 0.f;
            break;
        }
        case OscOpcode::Min: { const float b = pop_float(); fstack_[0] = (std::min)(fstack_[0], b); break; }
        case OscOpcode::Max: { const float b = pop_float(); fstack_[0] = (std::max)(fstack_[0], b); break; }

        // unary: replace slot 0
        case OscOpcode::Neg: fstack_[0] = -fstack_[0]; break;
        case OscOpcode::Not: fstack_[0] = fstack_[0] == 0.f ? 1.f : 0.f; break;
        case OscOpcode::Abs: fstack_[0] = std::fabs(fstack_[0]); break;
        case OscOpcode::Sqr: fstack_[0] = fstack_[0] * fstack_[0]; break;
        case OscOpcode::Sqrt: fstack_[0] = fstack_[0] > 0.f ? std::sqrt(fstack_[0]) : 0.f; break;
        case OscOpcode::Sin: fstack_[0] = std::sin(fstack_[0]); break;
        case OscOpcode::ArcSin: {
            const float x = fstack_[0];
            fstack_[0] = (x >= -1.f && x <= 1.f) ? std::asin(x) : 0.f;
            break;
        }
        case OscOpcode::ArcTan: fstack_[0] = std::atan(fstack_[0]); break;
        case OscOpcode::Exp: fstack_[0] = std::exp(fstack_[0]); break;
        case OscOpcode::Sgn: {
            const float x = fstack_[0];
            fstack_[0] = x > 0.f ? 1.f : (x < 0.f ? -1.f : 0.f);
            break;
        }
        case OscOpcode::Trunc: fstack_[0] = std::trunc(fstack_[0]); break;
        case OscOpcode::Random: {
            int range = round_half_even(fstack_[0]);
            if (range < 0) range = -range;
            fstack_[0] = range > 0 ? static_cast<float>(std::rand() % range) : 0.f;
            break;
        }
        case OscOpcode::Dup: push_float(fstack_[0]); break;
        case OscOpcode::Pi: push_float(3.14159265358979323846f); break;

        // string stack
        case OscOpcode::StrCat: {
            const std::string b = pop_string();
            sstack_[0] += b;
            break;
        }
        case OscOpcode::StrRepeat: {
            const float v = pop_float();
            sstack_[0] = repeat_to_length(sstack_[0], v);
            break;
        }
        case OscOpcode::StrEq: { const std::string b = pop_string(); push_float(sstack_[0] == b ? 1.f : 0.f); break; }
        case OscOpcode::StrLt: { const std::string b = pop_string(); push_float(sstack_[0] < b ? 1.f : 0.f); break; }
        case OscOpcode::StrGt: { const std::string b = pop_string(); push_float(sstack_[0] > b ? 1.f : 0.f); break; }
        case OscOpcode::StrLe: { const std::string b = pop_string(); push_float(sstack_[0] <= b ? 1.f : 0.f); break; }
        case OscOpcode::StrGe: { const std::string b = pop_string(); push_float(sstack_[0] >= b ? 1.f : 0.f); break; }
        case OscOpcode::StrDup: push_string(sstack_[0]); break;
        case OscOpcode::StrLength: push_float(static_cast<float>(sstack_[0].size())); break;
        case OscOpcode::StrToFloat: {
            const std::string s = pop_string();
            push_float(parse_float_or_zero(s));
            break;
        }
        case OscOpcode::IntToStr: {
            const float v = pop_float();
            push_string(std::to_string(static_cast<int>(std::trunc(v))));
            break;
        }
        case OscOpcode::IntToStrEnh: {
            const float v = pop_float();
            sstack_[0] = int_to_str_enh(v, sstack_[0]);
            break;
        }
        case OscOpcode::StrCutBegin: {
            const float v = pop_float();
            if (v > 0.f) {
                const int k = round_half_even(v);
                const int len = static_cast<int>(sstack_[0].size());
                sstack_[0] = k >= len ? std::string{} : sstack_[0].substr(static_cast<std::size_t>(k));
            }
            break;
        }
        case OscOpcode::StrCutEnd: {
            const float v = pop_float();
            if (v > 0.f) {
                const int k = round_half_even(v);
                const int len = static_cast<int>(sstack_[0].size());
                sstack_[0] = k >= len ? std::string{} : sstack_[0].substr(0, static_cast<std::size_t>(len - k));
            }
            break;
        }
        case OscOpcode::StrSetLenR: sstack_[0] = set_len_r(sstack_[0], round_half_even(fstack_[0])); break;
        case OscOpcode::StrSetLenL: sstack_[0] = set_len_l(sstack_[0], round_half_even(fstack_[0])); break;
        case OscOpcode::StrSetLenC: sstack_[0] = set_len_c(sstack_[0], round_half_even(fstack_[0])); break;
        case OscOpcode::StrRemSpaces: {
            std::string& s = sstack_[0];
            s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
            break;
        }
        case OscOpcode::StrMsg: break;
        }
        ++pc;
    }
}

} // namespace omsi::script
