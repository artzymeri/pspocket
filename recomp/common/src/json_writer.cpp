#include "pspocket/recomp/json_writer.hpp"

#include <format>

#include "pspocket/core/log.hpp"

namespace pspocket::recomp {

std::string JsonWriter::escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (u < 0x20 || u >= 0x7F) {
                out += std::format("\\u{:04x}", u);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
    return out;
}

void JsonWriter::newline_indent()
{
    if (!pretty_) return;
    out_.push_back('\n');
    out_.append(stack_.size() * 2, ' ');
}

void JsonWriter::before_value()
{
    if (stack_.empty()) {
        PS_CHECK(!wrote_root_);
        wrote_root_ = true;
        return;
    }
    if (stack_.back() == Frame::Object) {
        PS_CHECK(expecting_value_);
        expecting_value_ = false;
        return;
    }
    if (!first_.back()) out_.push_back(',');
    first_.back() = false;
    newline_indent();
}

JsonWriter& JsonWriter::begin_object()
{
    before_value();
    out_.push_back('{');
    stack_.push_back(Frame::Object);
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_object()
{
    PS_CHECK(!stack_.empty() && stack_.back() == Frame::Object && !expecting_value_);
    const bool empty = first_.back();
    stack_.pop_back();
    first_.pop_back();
    if (!empty) newline_indent();
    out_.push_back('}');
    return *this;
}

JsonWriter& JsonWriter::begin_array()
{
    before_value();
    out_.push_back('[');
    stack_.push_back(Frame::Array);
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_array()
{
    PS_CHECK(!stack_.empty() && stack_.back() == Frame::Array);
    const bool empty = first_.back();
    stack_.pop_back();
    first_.pop_back();
    if (!empty) newline_indent();
    out_.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name)
{
    PS_CHECK(!stack_.empty() && stack_.back() == Frame::Object && !expecting_value_);
    if (!first_.back()) out_.push_back(',');
    first_.back() = false;
    newline_indent();
    out_ += escape(name);
    out_ += pretty_ ? ": " : ":";
    expecting_value_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text)
{
    before_value();
    out_ += escape(text);
    return *this;
}

JsonWriter& JsonWriter::value(bool b)
{
    before_value();
    out_ += b ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::integer(s64 signed_value, bool is_unsigned, u64 unsigned_value)
{
    before_value();
    out_ += is_unsigned ? std::to_string(unsigned_value) : std::to_string(signed_value);
    return *this;
}

JsonWriter& JsonWriter::null()
{
    before_value();
    out_ += "null";
    return *this;
}

} // namespace pspocket::recomp
