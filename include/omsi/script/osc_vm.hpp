#pragma once

// OMSI 2 `.osc` interpreter (script language of program/Info_scriptcode.txt and the
// decompiled TXPC_calcblock_* classes of _Unit86.c).
//
// Semantics (verified against Omsi.exe / units_c/_Unit86.c):
//  * Two 8-slot register files: a float stack and a string stack. A push shifts every slot
//    one place back (slot 7 is lost); a pop shifts every slot one place forward. Binary
//    operators pop once and write the result to slot 0; unary operators replace slot 0.
//  * `{if}` tests slot 0 without popping it: true when != 0. `{else}` / `{endif}` close it.
//  * `(S.L.x)` / `(S.$.x)` store slot 0 without popping. `(L.L.x)` pushes.
//  * Macros, triggers and callbacks share the caller's stacks (no isolation).
//  * `=`, `&&`, `||`, `!` compare exactly (0 is false, anything else true).
//  * Variables are resolved against the bound VarRegistry; unknown names live in a private
//    per-VM store and are reported in the stats.
//
// Compile-once: `load()` turns every block into a flat IR once. Hot path = IR only.

#include "omsi/script/constfile.hpp"
#include "omsi/script/var_registry.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omsi::sound {
class SoundRuntime;
}

namespace omsi::script {

enum class OscOpcode : std::uint8_t {
    Nop = 0,
    // operands / variables
    PushF,      // push f
    PushStr,    // push string literal s
    LoadF,      // (L.L.s) push
    StoreF,     // (S.L.s) peek-store
    LoadStr,    // (L.$.s) push
    StoreStr,   // (S.$.s) peek-store
    LoadC,      // (C.L.s) push constant
    Curve,      // (F.L.s) replace slot 0 with curve(slot0)
    LoadSys,    // (L.S.s) push system var i (SysVar id, -1 unknown)
    StoreSys,   // (S.S.s) peek-store system var i
    CallM,      // (M.L.s) run macro i
    CallV,      // (M.V.s) callback s
    TrigSound,  // (T.L.s) / (T.F.s) sound trigger; i=1 for new file
    SaveS,      // sN peek-store scratch N
    LoadS,      // lN push scratch N
    Jmp,        // unconditional jump to i
    JmpIfFalse, // {if}: jump to i when slot0 == 0 (slot0 is not popped)
    // float stack
    Add, Sub, Mul, Div, Mod, Eq, Lt, Gt, Le, Ge, And, Or, Min, Max,
    Neg, Not, Abs, Sqr, Sqrt, Sin, ArcSin, ArcTan, Exp, Sgn, Trunc, Random,
    Dup, Pi,
    // string stack
    StrCat,      // $+ (a b -> a+b)
    StrRepeat,   // $* (number pops, string replaced)
    StrEq,       // $= (pops 2 push float)
    StrLt,       // $<
    StrGt,       // $>
    StrLe,       // $<=
    StrGe,       // $>=
    StrDup,      // $d
    StrLength,   // $length (push float, string kept)
    StrToFloat,  // $StrToFloat (pops string, pushes float)
    IntToStr,    // $IntToStr (pops number, pushes string)
    IntToStrEnh, // $IntToStrEnh (pops number; format string replaced)
    StrCutBegin, // $cutBegin (pops number)
    StrCutEnd,   // $cutEnd (pops number)
    StrSetLenR,  // $SetLengthR (number kept)
    StrSetLenL,  // $SetLengthL
    StrSetLenC,  // $SetLengthC
    StrRemSpaces,// $RemoveSpaces
    StrMsg,      // $msg (debug, no-op)
};

// SysVar ids (program/varlist_system.txt), index into OscSystemState::values.
enum class OscSysId : std::int16_t {
    Timegap = 0,
    GetTime,
    NoSound,
    Pause,
    Time,
    Day,
    Month,
    Year,
    DayOfYear,
    MouseX,
    MouseY,
    PrecipType,
    PrecipRate,
    CollPosX,
    CollPosY,
    CollPosZ,
    CollEnergy,
    WeatherTemperature,
    WeatherAbsHum,
    WearLifespan,
    AutoClutch,
    SunAlt,
    Count,
    Unknown = -1,
};

struct OscOp {
    OscOpcode op = OscOpcode::Nop;
    float f = 0.f;
    int i = 0;
    std::string s;
    // Per-op cache of the resolved variable index (valid for one bound registry).
    mutable const void* cache_key = nullptr;
    mutable int cache_idx = -1;
};

struct OscMacro {
    std::string name;     // lower case: "init", "frame", "frame_ai", "macro:x", "trigger:x"
    std::vector<OscOp> code;
};

struct OscLoadStats {
    int script_files = 0;
    int macro_count = 0;
    int trigger_count = 0;
    int init_macros = 0;
    int frame_macros = 0;
    int frame_ai_macros = 0;
    int ir_ops = 0;
    int unknown_tokens = 0;      // syntax the interpreter does not know
    int unknown_callbacks = 0;   // (M.V.x) names missing from the OMSI callback lists
    int unknown_sysvars = 0;     // (L.S.x) names missing from varlist_system
    int unknown_consts = 0;      // (C.L.x) not in any loaded constfile (checked at run time)
    int unknown_curves = 0;      // (F.L.x) not in any loaded constfile (checked at run time)
    int unknown_vars = 0;        // (L.L.x)/(S.L.x)/(L.$.x)/(S.$.x) not in the bound registry at load
    int missing_macros = 0;      // (M.L.x) without a block
    int warnings = 0;
    int errors = 0;
};

// Values of the OMSI system variables (program/varlist_system.txt). Field names are the
// names vehicle code already uses; see OscVM::load_sys for the mapping.
struct OscSystemState {
    float timegap = 0.f;
    float get_time = 0.f;      // seconds since midnight
    float no_sound = 0.f;
    float pause = 0.f;
    float time = 0.f;
    float day = 1.f;
    float month = 7.f;
    float year = 2026.f;
    float day_of_year = 191.f;
    float mouse_x = 0.f;
    float mouse_y = 0.f;
    float precip_type = 0.f;
    float precip_rate = 0.f;
    float coll_pos_x = 0.f;
    float coll_pos_y = 0.f;
    float coll_pos_z = 0.f;
    float coll_energy = 0.f;
    float weather_temperature = 20.f;
    float weather_abs_hum = 8.f;
    float wear_lifespan = 0.f;
    float auto_clutch = 0.f;
    float sun_alt = 0.f;
};

// Process-wide lists read from <omsi>/program. Idempotent per root.
// callbacklist_*.txt -> known callback names; varlist_system.txt is built in (see OscSysId).
void load_known_lists(const std::filesystem::path& omsi_root);
[[nodiscard]] bool is_known_callback(std::string_view name);
[[nodiscard]] OscSysId sysvar_from_name(std::string_view name);

class OscVM {
public:
    using CallbackHandler = std::function<void(std::string_view name, OscVM& vm)>;

    bool load(const std::filesystem::path& bus_dir, const std::vector<std::filesystem::path>& script_files,
              const VarRegistry& vars);
    void bind_vars(std::vector<float>* public_floats, const VarRegistry* vars);
    void bind_strings(std::vector<std::string>* public_strings);
    void bind_consts(const std::unordered_map<std::string, float>* consts);
    void bind_curves(const std::unordered_map<std::string, ConstCurve>* curves);
    void bind_sound(omsi::sound::SoundRuntime* sound);
    void bind_system_vars(float weather_temperature_c);
    void set_system_state(const OscSystemState& state);
    void set_callback_handler(CallbackHandler handler) { callback_handler_ = std::move(handler); }
    void set_ai_mode(bool ai) { ai_mode_ = ai; }
    [[nodiscard]] bool ai_mode() const { return ai_mode_; }

    void run_init();
    void run_frame(float timegap_sec);
    void fire_trigger(const std::string& trigger_name);

    // Stack access for callback handlers (slot 0 = top).
    void push_float(float v);
    float pop_float();
    [[nodiscard]] float top_float() const { return fstack_[0]; }
    void set_top_float(float v) { fstack_[0] = v; }
    void push_string(std::string v);
    std::string pop_string();
    [[nodiscard]] const std::string& top_string() const { return sstack_[0]; }
    void set_top_string(std::string v) { sstack_[0] = std::move(v); }

    [[nodiscard]] const OscLoadStats& stats() const { return stats_; }
    [[nodiscard]] const std::vector<std::string>& unsupported() const { return unsupported_; }
    [[nodiscard]] const std::vector<std::string>& stubbed_callbacks() const { return stubbed_callbacks_; }
    [[nodiscard]] bool has_trigger(const std::string& name) const;
    [[nodiscard]] bool has_frame_ai() const { return frame_ai_idx_ >= 0; }
    [[nodiscard]] bool has_frame() const { return frame_idx_ >= 0; }

private:
    static constexpr std::size_t kStackCap = 8;

    std::vector<OscMacro> macros_;
    std::unordered_map<std::string, int> macro_index_;    // "init", "macro:x", "trigger:x" (lower)
    std::unordered_map<std::string, std::vector<int>> trigger_blocks_; // "trigger:x" -> every block, file order
    int init_idx_ = -1;
    int frame_idx_ = -1;
    int frame_ai_idx_ = -1;
    OscLoadStats stats_{};
    std::vector<std::string> unsupported_;
    std::unordered_set<std::string> unsupported_set_;
    std::vector<std::string> stubbed_callbacks_;

    std::vector<float>* floats_ = nullptr;
    std::vector<std::string>* strings_ = nullptr;
    const VarRegistry* vars_ = nullptr;
    const std::unordered_map<std::string, float>* consts_ = nullptr;
    const std::unordered_map<std::string, ConstCurve>* curves_ = nullptr;
    // OMSI resolves (C.L.x) / (F.L.x) names case-insensitively (scripts mix bremse_V_Brzyl_FBA and
    // bremse_V_brzyl_FBA); lower-case indexes, rebuilt when the bound map changes size.
    mutable std::unordered_map<std::string, float> consts_lc_;
    mutable std::unordered_map<std::string, const ConstCurve*> curves_lc_;
    mutable std::size_t consts_lc_n_ = static_cast<std::size_t>(-1);
    mutable std::size_t curves_lc_n_ = static_cast<std::size_t>(-1);
    omsi::sound::SoundRuntime* sound_ = nullptr;
    OscSystemState sys_{};
    std::array<float, 10> scratch_{};
    CallbackHandler callback_handler_;
    bool ai_mode_ = false;

    // Undeclared variables (not in the bound registry) and system writes.
    std::unordered_map<std::string, float> extra_floats_;
    std::unordered_map<std::string, std::string> extra_strings_;
    std::array<float, static_cast<std::size_t>(OscSysId::Count)> sys_written_{};

    std::array<float, kStackCap> fstack_{};
    std::array<std::string, kStackCap> sstack_{};
    int call_depth_ = 0;
    std::string active_macro_;

    void note_unsupported(const std::string& what);
    void compile_block(const std::vector<std::string>& lines, std::vector<OscOp>& out, const std::string& ctx,
                       const std::unordered_map<std::string, int>& macro_index);
    void resolve_macros();

    bool run_macro_index(int index);
    void run_code(const std::vector<OscOp>& code);
    void run_entry(int index);

    float* float_ref(const OscOp& op);
    std::string* string_ref(const OscOp& op);
    [[nodiscard]] float get_const(const std::string& name) const;
    [[nodiscard]] float eval_curve(const std::string& name, float x) const;
    [[nodiscard]] float load_sys(OscSysId id) const;
    void store_sys(OscSysId id, float v);
};

} // namespace omsi::script
