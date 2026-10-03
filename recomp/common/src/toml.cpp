#include "pspocket/recomp/toml.hpp"

#include <format>
#include <optional>

namespace pspocket::recomp::toml {

const Value* Value::get(std::string_view key) const
{
    for (usize i = 0; i < keys.size(); i++) {
        if (keys[i] == key) return &values[i];
    }
    return nullptr;
}

Value* Value::get(std::string_view key)
{
    return const_cast<Value*>(static_cast<const Value*>(this)->get(key));
}

std::string_view type_name(Value::Type type)
{
    switch (type) {
    case Value::Type::String: return "string";
    case Value::Type::Integer: return "integer";
    case Value::Type::Boolean: return "boolean";
    case Value::Type::Array: return "array";
    case Value::Type::Table: return "table";
    }
    return "?";
}

namespace {

constexpr int kMaxNesting = 64;

bool is_bare_key_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int digit_value(char c, int base)
{
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    return v >= 0 && v < base ? v : -1;
}

void append_utf8(std::string& out, u32 cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Result<Value> run()
    {
        root_.type = Value::Type::Table;
        root_.defined = true;
        current_ = &root_;
        while (true) {
            skip_blank_lines();
            if (eof()) break;
            if (peek() == '[') {
                if (peek(1) == '[') {
                    array_table_header();
                } else {
                    table_header();
                }
            } else {
                key_value(*current_);
            }
            if (!error_) expect_end_of_line();
            if (error_) return *error_;
        }
        return std::move(root_);
    }

private:
    bool eof() const { return pos_ >= s_.size(); }
    char peek(usize ahead = 0) const { return pos_ + ahead < s_.size() ? s_[pos_ + ahead] : '\0'; }

    bool fail(std::string_view message)
    {
        if (!error_) {
            error_ = make_error("line {}, column {}: {}", line_, pos_ - line_start_ + 1, message);
        }
        return false;
    }

    void skip_spaces()
    {
        while (!eof() && (peek() == ' ' || peek() == '\t')) pos_++;
    }

    void skip_comment()
    {
        if (peek() != '#') return;
        while (!eof() && peek() != '\n') {
            const auto c = static_cast<unsigned char>(peek());
            if ((c < 0x20 && c != '\t' && c != '\r') || c == 0x7F) {
                fail("control character in comment");
                return;
            }
            pos_++;
        }
    }

    bool newline()
    {
        if (peek() == '\n') {
            pos_++;
        } else if (peek() == '\r' && peek(1) == '\n') {
            pos_ += 2;
        } else {
            return false;
        }
        line_++;
        line_start_ = pos_;
        return true;
    }

    // Whitespace, comments and newlines.
    void skip_blank_lines()
    {
        while (!eof() && !error_) {
            skip_spaces();
            skip_comment();
            if (!newline()) break;
        }
    }

    void expect_end_of_line()
    {
        skip_spaces();
        skip_comment();
        if (error_ || eof()) return;
        if (!newline()) fail("expected the end of the line");
    }

    std::optional<std::vector<std::string>> key_path()
    {
        std::vector<std::string> path;
        while (true) {
            skip_spaces();
            std::string part;
            if (peek() == '"') {
                if (!basic_string(part)) return std::nullopt;
            } else if (peek() == '\'') {
                if (!literal_string(part)) return std::nullopt;
            } else if (is_bare_key_char(peek())) {
                while (!eof() && is_bare_key_char(peek())) part.push_back(s_[pos_++]);
            } else {
                fail("expected a key");
                return std::nullopt;
            }
            path.push_back(std::move(part));
            skip_spaces();
            if (peek() != '.') break;
            pos_++;
        }
        return path;
    }

    static std::string join(const std::vector<std::string>& path, usize count)
    {
        std::string out;
        for (usize i = 0; i < count; i++) {
            if (i) out.push_back('.');
            out += path[i];
        }
        return out;
    }

    // Walks/creates the tables of path[0..count) starting at `t` (for headers: the root).
    Value* walk(Value* t, const std::vector<std::string>& path, usize count, bool mark_defined)
    {
        for (usize i = 0; i < count; i++) {
            Value* child = t->get(path[i]);
            if (!child) {
                t->keys.push_back(path[i]);
                Value table;
                table.type = Value::Type::Table;
                table.line = line_;
                table.defined = mark_defined;
                t->values.push_back(std::move(table));
                child = &t->values.back();
            } else if (child->is_array()) {
                if (!child->array_of_tables || child->items.empty()) {
                    fail(std::format("'{}' is an array, not a table", join(path, i + 1)));
                    return nullptr;
                }
                child = &child->items.back();
            } else if (!child->is_table()) {
                fail(std::format("'{}' is a {}, not a table", join(path, i + 1), type_name(child->type)));
                return nullptr;
            } else if (child->inline_value) {
                fail(std::format("'{}' cannot be extended", join(path, i + 1)));
                return nullptr;
            }
            t = child;
        }
        return t;
    }

    void table_header()
    {
        const u32 header_line = line_;
        pos_++; // [
        auto path = key_path();
        if (!path) return;
        skip_spaces();
        if (peek() != ']') {
            fail("expected ']'");
            return;
        }
        pos_++;
        Value* parent = walk(&root_, *path, path->size() - 1, false);
        if (!parent) return;
        if (Value* existing = parent->get(path->back())) {
            if (existing->is_table() && !existing->defined && !existing->inline_value) {
                existing->defined = true;
                existing->line = header_line;
                current_ = existing;
                return;
            }
            fail(std::format("'{}' is defined twice", join(*path, path->size())));
            return;
        }
        parent->keys.push_back(path->back());
        Value table;
        table.type = Value::Type::Table;
        table.line = header_line;
        table.defined = true;
        parent->values.push_back(std::move(table));
        current_ = &parent->values.back();
    }

    void array_table_header()
    {
        const u32 header_line = line_;
        pos_ += 2; // [[
        auto path = key_path();
        if (!path) return;
        skip_spaces();
        if (peek() != ']' || peek(1) != ']') {
            fail("expected ']]'");
            return;
        }
        pos_ += 2;
        Value* parent = walk(&root_, *path, path->size() - 1, false);
        if (!parent) return;
        Value* array = parent->get(path->back());
        if (!array) {
            parent->keys.push_back(path->back());
            Value a;
            a.type = Value::Type::Array;
            a.array_of_tables = true;
            a.line = header_line;
            parent->values.push_back(std::move(a));
            array = &parent->values.back();
        } else if (!array->is_array() || !array->array_of_tables) {
            fail(std::format("'{}' is already defined and is not an array of tables", join(*path, path->size())));
            return;
        }
        Value table;
        table.type = Value::Type::Table;
        table.line = header_line;
        table.defined = true;
        array->items.push_back(std::move(table));
        current_ = &array->items.back();
    }

    void key_value(Value& table)
    {
        auto path = key_path();
        if (!path) return;
        skip_spaces();
        if (peek() != '=') {
            fail("expected '='");
            return;
        }
        pos_++;
        skip_spaces();
        Value v;
        if (!value(v, 0)) return;
        Value* t = walk(&table, *path, path->size() - 1, true);
        if (!t) return;
        if (t->get(path->back())) {
            fail(std::format("duplicate key '{}'", join(*path, path->size())));
            return;
        }
        t->keys.push_back(path->back());
        t->values.push_back(std::move(v));
    }

    bool value(Value& out, int depth)
    {
        if (depth > kMaxNesting) return fail("arrays nested too deeply");
        out.line = line_;
        const char c = peek();
        if (c == '"') {
            if (peek(1) == '"' && peek(2) == '"') return fail("multi-line strings are not supported");
            out.type = Value::Type::String;
            return basic_string(out.string);
        }
        if (c == '\'') {
            if (peek(1) == '\'' && peek(2) == '\'') return fail("multi-line strings are not supported");
            out.type = Value::Type::String;
            return literal_string(out.string);
        }
        if (c == 't' || c == 'f') {
            const std::string_view word = c == 't' ? "true" : "false";
            if (s_.substr(pos_, word.size()) == word && !is_bare_key_char(peek(word.size()))) {
                pos_ += word.size();
                out.type = Value::Type::Boolean;
                out.boolean = c == 't';
                return true;
            }
            return fail("expected a value");
        }
        if (c == '[') return array(out, depth);
        if (c == '{') return fail("inline tables are not supported");
        if (c == '+' || c == '-' || (c >= '0' && c <= '9')) return integer(out);
        if (c == 'i' || c == 'n') return fail("floats are not supported");
        return fail("expected a value");
    }

    bool array(Value& out, int depth)
    {
        pos_++; // [
        out.type = Value::Type::Array;
        out.inline_value = true;
        while (true) {
            skip_blank_lines();
            if (error_) return false;
            if (eof()) return fail("unterminated array");
            if (peek() == ']') {
                pos_++;
                return true;
            }
            Value item;
            if (!value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skip_blank_lines();
            if (error_) return false;
            if (eof()) return fail("unterminated array");
            if (peek() == ',') {
                pos_++;
                continue;
            }
            if (peek() == ']') {
                pos_++;
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }

    bool integer(Value& out)
    {
        bool negative = false;
        bool signed_ = false;
        if (peek() == '+' || peek() == '-') {
            negative = peek() == '-';
            signed_ = true;
            pos_++;
        }
        int base = 10;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'o' || peek(1) == 'b')) {
            if (signed_) return fail("a sign is not allowed on hex, octal or binary integers");
            base = peek(1) == 'x' ? 16 : (peek(1) == 'o' ? 8 : 2);
            pos_ += 2;
        }
        const usize digits_start = pos_;
        u64 magnitude = 0;
        bool overflow = false;
        bool prev_digit = false;
        while (!eof()) {
            const char c = peek();
            if (c == '_') {
                if (!prev_digit || digit_value(peek(1), base) < 0) return fail("misplaced '_' in integer");
                prev_digit = false;
                pos_++;
                continue;
            }
            const int d = digit_value(c, base);
            if (d < 0) break;
            if (magnitude > (~u64{0} - static_cast<u64>(d)) / static_cast<u64>(base)) overflow = true;
            magnitude = magnitude * static_cast<u64>(base) + static_cast<u64>(d);
            prev_digit = true;
            pos_++;
        }
        if (pos_ == digits_start) return fail("expected digits");
        if (base == 10 && s_[digits_start] == '0' && pos_ - digits_start > 1) return fail("leading zeros are not allowed");
        const char next = peek();
        if (base == 10 && (next == '.' || next == 'e' || next == 'E')) return fail("floats are not supported");
        if (base == 10 && (next == '-' || next == ':')) return fail("dates and times are not supported");
        if (is_bare_key_char(next)) return fail("invalid character in integer");
        const u64 limit = negative ? (u64{1} << 63) : (u64{1} << 63) - 1;
        if (overflow || magnitude > limit) return fail("integer out of range");
        out.type = Value::Type::Integer;
        out.integer = negative ? static_cast<s64>(0 - magnitude) : static_cast<s64>(magnitude);
        return true;
    }

    bool basic_string(std::string& out)
    {
        pos_++; // "
        while (true) {
            if (eof()) return fail("unterminated string");
            const char c = s_[pos_];
            const auto u = static_cast<unsigned char>(c);
            if (c == '"') {
                pos_++;
                return true;
            }
            if (c == '\n' || c == '\r') return fail("newline in a string");
            if ((u < 0x20 && c != '\t') || u == 0x7F) return fail("control character in a string");
            if (c != '\\') {
                out.push_back(c);
                pos_++;
                continue;
            }
            pos_++;
            const char e = peek();
            pos_++;
            switch (e) {
            case 'b': out.push_back('\b'); break;
            case 't': out.push_back('\t'); break;
            case 'n': out.push_back('\n'); break;
            case 'f': out.push_back('\f'); break;
            case 'r': out.push_back('\r'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'u':
            case 'U': {
                const usize n = e == 'u' ? 4 : 8;
                u32 cp = 0;
                for (usize i = 0; i < n; i++) {
                    const int d = digit_value(peek(), 16);
                    if (d < 0) return fail("bad unicode escape");
                    cp = (cp << 4) | static_cast<u32>(d);
                    pos_++;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("unicode escape is not a scalar value");
                append_utf8(out, cp);
                break;
            }
            default: return fail("unknown escape sequence");
            }
        }
    }

    bool literal_string(std::string& out)
    {
        pos_++; // '
        while (true) {
            if (eof()) return fail("unterminated string");
            const char c = s_[pos_];
            const auto u = static_cast<unsigned char>(c);
            if (c == '\'') {
                pos_++;
                return true;
            }
            if (c == '\n' || c == '\r') return fail("newline in a string");
            if ((u < 0x20 && c != '\t') || u == 0x7F) return fail("control character in a string");
            out.push_back(c);
            pos_++;
        }
    }

    std::string_view s_;
    usize pos_ = 0;
    u32 line_ = 1;
    usize line_start_ = 0;
    std::optional<Error> error_;
    Value root_;
    Value* current_ = nullptr;
};

} // namespace

Result<Value> parse(std::string_view text) { return Parser(text).run(); }

} // namespace pspocket::recomp::toml
