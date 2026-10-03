#include "pspocket/recomp/bytes.hpp"

#include <format>

namespace pspocket::recomp {

std::optional<std::string_view> ByteReader::cstring(u64 offset, usize max_length) const
{
    if (offset >= data_.size()) return std::nullopt;
    const u64 available = data_.size() - offset;
    const u64 limit = available < max_length + 1ull ? available : max_length + 1ull;
    const auto* start = reinterpret_cast<const char*>(data_.data() + offset);
    for (u64 i = 0; i < limit; i++) {
        if (start[i] == '\0') return std::string_view(start, static_cast<usize>(i));
    }
    return std::nullopt;
}

std::string_view ByteReader::cstring_lenient(u64 offset, usize max_length) const
{
    if (offset >= data_.size()) return {};
    const u64 available = data_.size() - offset;
    const u64 limit = available < max_length ? available : max_length;
    const auto* start = reinterpret_cast<const char*>(data_.data() + offset);
    u64 n = 0;
    while (n < limit && start[n] != '\0') n++;
    return std::string_view(start, static_cast<usize>(n));
}

std::string to_hex(std::span<const u8> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const u8 b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

std::string hex32(u32 value) { return std::format("0x{:08x}", value); }

std::string hex_compact(u64 value) { return std::format("0x{:x}", value); }

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_hex_digits(std::string_view text)
{
    for (const char c : text) {
        if (hex_value(c) < 0) return false;
    }
    return true;
}

bool parse_hex_bytes(std::string_view text, std::vector<u8>& out)
{
    if (text.size() % 2 != 0) return false;
    std::vector<u8> bytes;
    bytes.reserve(text.size() / 2);
    for (usize i = 0; i < text.size(); i += 2) {
        const int hi = hex_value(text[i]);
        const int lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes.push_back(static_cast<u8>((hi << 4) | lo));
    }
    out = std::move(bytes);
    return true;
}

bool is_printable_ascii(std::string_view text)
{
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7e) return false;
    }
    return true;
}

} // namespace pspocket::recomp
