#pragma once
// Minimal JSON DOM for BeamNG data files.
// - Lenient reader: trailing commas, // and /* */ comments, NaN/inf literals (BeamNG writes some of those).
// - Keeps object key order and the exact source text of every number, so an untouched value is written back
//   byte-for-byte (needed to save items.level.json 1:1 with the game's own serializer).
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bng {

class JsonValue {
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    static JsonValue make_null() { return {}; }
    static JsonValue make_bool(bool b);
    static JsonValue make_number(double d);
    static JsonValue make_number_text(std::string text); // keeps the literal
    static JsonValue make_string(std::string s);
    static JsonValue make_array();
    static JsonValue make_object();

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool def = false) const;
    double as_number(double def = 0.0) const;
    float as_float(float def = 0.f) const { return static_cast<float>(as_number(def)); }
    int as_int(int def = 0) const { return static_cast<int>(as_number(def)); }
    const std::string& as_string() const; // empty string when not a string
    // Strings that hold numbers ("1") are common in BeamNG files; this accepts both.
    double as_number_lenient(double def = 0.0) const;

    // Array
    std::size_t size() const;
    const JsonValue& operator[](std::size_t i) const;
    JsonValue& at(std::size_t i);
    void push_back(JsonValue v);
    std::vector<JsonValue>& items() { return arr_; }
    const std::vector<JsonValue>& items() const { return arr_; }

    // Object (ordered)
    const JsonValue* find(std::string_view key) const;
    JsonValue* find(std::string_view key);
    const JsonValue& get(std::string_view key) const; // null value when missing
    JsonValue& set(std::string_view key, JsonValue v); // replaces in place or appends
    bool erase(std::string_view key);
    std::vector<std::pair<std::string, JsonValue>>& members() { return obj_; }
    const std::vector<std::pair<std::string, JsonValue>>& members() const { return obj_; }

    // Number formatting used for values the editor changed (BeamNG style: up to 9 significant digits, no exponent).
    static std::string format_number(double d);

private:
    Type type_ = Type::Null;
    bool b_ = false;
    double num_ = 0.0;
    std::string str_;      // string payload, or the literal text of a number
    std::vector<JsonValue> arr_;
    std::vector<std::pair<std::string, JsonValue>> obj_;
};

struct JsonParseResult {
    bool ok = false;
    std::string error;
    std::size_t error_offset = 0;
};

JsonParseResult json_parse(std::string_view text, JsonValue& out);
// Compact one-line form ("key":value, no spaces) — the NDJSON style of items.level.json / forest4.json.
std::string json_write_compact(const JsonValue& v);
// Indented form (2 spaces, like BeamNG's *.materials.json / info.json).
std::string json_write_pretty(const JsonValue& v, int indent = 2);

// Splits an NDJSON buffer into lines and parses each one; bad lines are reported and skipped.
std::vector<JsonValue> json_parse_lines(std::string_view text, std::vector<std::string>* errors = nullptr);

} // namespace bng
