#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/result.hpp"

// A TOML subset for game manifests (games/<title>/manifest.toml).
//
// Supported: comments, bare / quoted / dotted keys, [tables], [[arrays of tables]], basic strings ("..." with
// \b \t \n \f \r \" \\ \uXXXX \UXXXXXXXX escapes), literal strings ('...'), integers (decimal with optional sign
// and underscores, 0x hex, 0o octal, 0b binary), booleans, and arrays (of any supported value, across lines,
// trailing comma allowed).
// Not supported, and rejected with an error: floats, dates and times, multi-line strings, inline tables.
namespace pspocket::recomp::toml {

struct Value {
    enum class Type : u8 { String, Integer, Boolean, Array, Table };

    Type type = Type::Table;
    std::string string;
    s64 integer = 0;
    bool boolean = false;
    std::vector<Value> items;      // Array
    std::vector<std::string> keys; // Table, in definition order
    std::vector<Value> values;     // Table, parallel to keys
    u32 line = 0;                  // where the value (or table header) was defined

    // Parser bookkeeping.
    bool defined = false;           // table: defined by a header or key/value (not just implied by a dotted path)
    bool array_of_tables = false;   // array: created by [[...]]
    bool inline_value = false;      // table/array: written as a value (cannot be extended by headers)

    bool is_table() const { return type == Type::Table; }
    bool is_array() const { return type == Type::Array; }
    const Value* get(std::string_view key) const;
    Value* get(std::string_view key);
};

std::string_view type_name(Value::Type type);

// Parses a document; the result is the root table. Errors carry "line L, column C: ...".
Result<Value> parse(std::string_view text);

} // namespace pspocket::recomp::toml
