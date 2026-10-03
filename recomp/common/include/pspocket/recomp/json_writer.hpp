#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "pspocket/core/types.hpp"

// A small streaming JSON writer (no dependencies). Strings are escaped so the output is always valid JSON and
// pure ASCII: control characters and bytes >= 0x80 (binary data often isn't UTF-8) become \u00XX escapes.
// Misuse (a value where a key is expected, unbalanced end_*) is a programming error and trips PS_CHECK.
namespace pspocket::recomp {

class JsonWriter {
public:
    explicit JsonWriter(bool pretty = true) : pretty_(pretty) {}

    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view text);
    JsonWriter& value(const char* text) { return value(std::string_view(text)); }
    JsonWriter& value(const std::string& text) { return value(std::string_view(text)); }
    JsonWriter& value(bool b);
    template <class T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>)
    JsonWriter& value(T n)
    {
        if constexpr (std::is_signed_v<T>) {
            return integer(static_cast<s64>(n), false);
        } else {
            return integer(static_cast<s64>(0), true, static_cast<u64>(n));
        }
    }
    JsonWriter& null();

    template <class T>
    JsonWriter& field(std::string_view name, const T& v)
    {
        key(name);
        return value(v);
    }

    // True once the top-level value is complete.
    bool complete() const { return stack_.empty() && wrote_root_; }
    const std::string& str() const { return out_; }

    static std::string escape(std::string_view text);

private:
    enum class Frame : u8 { Object, Array };
    JsonWriter& integer(s64 signed_value, bool is_unsigned, u64 unsigned_value = 0);
    void before_value();
    void newline_indent();

    std::string out_;
    std::vector<Frame> stack_;
    std::vector<bool> first_;
    bool expecting_value_ = false; // a key was written
    bool wrote_root_ = false;
    bool pretty_;
};

} // namespace pspocket::recomp
