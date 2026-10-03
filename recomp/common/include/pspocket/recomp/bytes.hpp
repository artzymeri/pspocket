#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"

// Bounds-checked big-endian reading over untrusted bytes. Every accessor checks its range without overflow
// and returns nullopt instead of reading out of bounds, so parsers built on it cannot hit UB on bad input.
namespace pspocket::recomp {

// True when [offset, offset + length) lies inside a buffer of `size` bytes (overflow-safe).
constexpr bool range_ok(u64 size, u64 offset, u64 length)
{
    return offset <= size && length <= size - offset;
}

// a + b, or nullopt on u64 overflow.
constexpr std::optional<u64> checked_add(u64 a, u64 b)
{
    if (b > ~u64{0} - a) return std::nullopt;
    return a + b;
}

// a * b, or nullopt on u64 overflow.
constexpr std::optional<u64> checked_mul(u64 a, u64 b)
{
    if (a != 0 && b > ~u64{0} / a) return std::nullopt;
    return a * b;
}

class ByteReader {
public:
    constexpr ByteReader() = default;
    constexpr explicit ByteReader(std::span<const u8> data) : data_(data) {}

    constexpr usize size() const { return data_.size(); }
    constexpr std::span<const u8> span() const { return data_; }
    constexpr bool in_bounds(u64 offset, u64 length) const { return range_ok(data_.size(), offset, length); }

    std::optional<u8> u8_at(u64 offset) const
    {
        if (!in_bounds(offset, 1)) return std::nullopt;
        return data_[offset];
    }
    std::optional<u16> be16(u64 offset) const
    {
        if (!in_bounds(offset, 2)) return std::nullopt;
        return ps_ld16(data_.data() + offset);
    }
    std::optional<u32> be32(u64 offset) const
    {
        if (!in_bounds(offset, 4)) return std::nullopt;
        return ps_ld32(data_.data() + offset);
    }
    std::optional<u64> be64(u64 offset) const
    {
        if (!in_bounds(offset, 8)) return std::nullopt;
        return ps_ld64(data_.data() + offset);
    }
    // Little-endian reads, for the few PS3 formats that use them (debug SELF offsets).
    std::optional<u16> le16(u64 offset) const
    {
        if (!in_bounds(offset, 2)) return std::nullopt;
        return static_cast<u16>(data_[offset] | (static_cast<u16>(data_[offset + 1]) << 8));
    }
    std::optional<u64> le64(u64 offset) const
    {
        if (!in_bounds(offset, 8)) return std::nullopt;
        u64 v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | data_[offset + static_cast<u64>(i)];
        return v;
    }

    std::optional<std::span<const u8>> bytes(u64 offset, u64 length) const
    {
        if (!in_bounds(offset, length)) return std::nullopt;
        return data_.subspan(static_cast<usize>(offset), static_cast<usize>(length));
    }

    // The NUL-terminated string at `offset`, at most `max_length` characters. nullopt when no NUL is found
    // within bounds and max_length.
    std::optional<std::string_view> cstring(u64 offset, usize max_length) const;

    // Up to `max_length` bytes starting at offset, stopping at the first NUL or the end of the buffer.
    std::string_view cstring_lenient(u64 offset, usize max_length) const;

private:
    std::span<const u8> data_;
};

// Lowercase hex of the bytes, no separators.
std::string to_hex(std::span<const u8> bytes);
// "0x0001abcd" style, 8 digits.
std::string hex32(u32 value);
// "0x..." with the minimum number of digits (at least one).
std::string hex_compact(u64 value);
// Parses hex digits (no prefix); false on any non-hex character or odd length.
bool parse_hex_bytes(std::string_view text, std::vector<u8>& out);
bool is_hex_digits(std::string_view text);
// Printable ASCII (0x20..0x7e) only.
bool is_printable_ascii(std::string_view text);

} // namespace pspocket::recomp
