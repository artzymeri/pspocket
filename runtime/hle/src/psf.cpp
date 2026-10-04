#include "psf.hpp"

#include <algorithm>
#include <cstring>

namespace pspocket::hle::psf {
namespace {

constexpr u32 kMagic = 0x46535000; // "\0PSF" read as a little-endian u32
constexpr usize kHeaderSize = 20, kIndexSize = 16;

u16 le16(const u8* p) { return static_cast<u16>(p[0] | p[1] << 8); }
u32 le32(const u8* p) { return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24; }

void put16(std::vector<u8>& out, u16 v)
{
    out.push_back(static_cast<u8>(v));
    out.push_back(static_cast<u8>(v >> 8));
}

void put32(std::vector<u8>& out, u32 v)
{
    for (int i = 0; i < 4; i++) out.push_back(static_cast<u8>(v >> (8 * i)));
}

// RPCS3 psf::entry::size(): a string counts its NUL, everything is capped at the entry's maximum.
u32 stored_size(const Entry& e)
{
    switch (e.format) {
    case Format::String: return std::min(e.max, static_cast<u32>(e.text.size() + 1));
    case Format::Array: return std::min(e.max, static_cast<u32>(e.text.size()));
    case Format::Integer: return 4;
    }
    return 0;
}

} // namespace

std::optional<Registry> parse(std::span<const u8> b)
{
    if (b.size() < kHeaderSize || le32(b.data()) != kMagic) return std::nullopt;
    const u32 key_table = le32(b.data() + 8), data_table = le32(b.data() + 12), count = le32(b.data() + 16);
    if (count > 1024 || kHeaderSize + u64{count} * kIndexSize > b.size() || key_table > b.size() ||
        data_table > b.size()) {
        return std::nullopt;
    }
    Registry out;
    for (u32 i = 0; i < count; i++) {
        const u8* idx = b.data() + kHeaderSize + i * kIndexSize;
        const u64 key_at = u64{key_table} + le16(idx);
        const u16 fmt = le16(idx + 2);
        const u32 len = le32(idx + 4), max = le32(idx + 8);
        const u64 data_at = u64{data_table} + le32(idx + 12);
        if (key_at >= b.size() || len > max || data_at + max > b.size()) return std::nullopt;
        const char* key = reinterpret_cast<const char*>(b.data() + key_at);
        const usize key_len = strnlen(key, b.size() - key_at);
        if (key_at + key_len >= b.size()) return std::nullopt;
        Entry e;
        e.max = max;
        switch (static_cast<Format>(fmt)) {
        case Format::Integer:
            if (len != 4) return std::nullopt;
            e.format = Format::Integer;
            e.integer = le32(b.data() + data_at);
            break;
        case Format::String:
        case Format::Array: {
            e.format = static_cast<Format>(fmt);
            const char* data = reinterpret_cast<const char*>(b.data() + data_at);
            e.text.assign(data, e.format == Format::String ? strnlen(data, len) : len);
            break;
        }
        default: return std::nullopt;
        }
        out.emplace(std::string(key, key_len), std::move(e));
    }
    return out;
}

// RPCS3 psf::save_object: header, indices, keys (in key order, the table padded to 4), then each value padded to its
// entry's maximum.
std::vector<u8> save(const Registry& psf)
{
    u32 key_bytes = 0, data_bytes = 0;
    for (const auto& [key, e] : psf) {
        key_bytes += static_cast<u32>(key.size() + 1);
        data_bytes += e.max;
    }
    const u32 key_table = static_cast<u32>(kHeaderSize + kIndexSize * psf.size());
    const u32 data_table = key_table + ((key_bytes + 3) & ~3u);
    std::vector<u8> out;
    out.reserve(data_table + data_bytes);
    put32(out, kMagic);
    put32(out, 0x101);
    put32(out, key_table);
    put32(out, data_table);
    put32(out, static_cast<u32>(psf.size()));
    u32 key_off = 0, data_off = 0;
    for (const auto& [key, e] : psf) {
        put16(out, static_cast<u16>(key_off));
        put16(out, static_cast<u16>(e.format));
        put32(out, stored_size(e));
        put32(out, e.max);
        put32(out, data_off);
        key_off += static_cast<u32>(key.size() + 1);
        data_off += e.max;
    }
    for (const auto& [key, e] : psf) {
        out.insert(out.end(), key.begin(), key.end());
        out.push_back(0);
    }
    out.resize(data_table, 0);
    for (const auto& [key, e] : psf) {
        const usize start = out.size();
        if (e.format == Format::Integer) {
            put32(out, e.integer);
        } else {
            const usize n = std::min<usize>(e.text.size(), e.max - (e.format == Format::String ? 1 : 0));
            out.insert(out.end(), e.text.begin(), e.text.begin() + static_cast<std::ptrdiff_t>(n));
        }
        out.resize(start + e.max, 0);
    }
    return out;
}

std::string get_string(const Registry& psf, std::string_view key)
{
    const auto it = psf.find(key);
    if (it == psf.end() || it->second.format == Format::Integer) return {};
    return it->second.text;
}

} // namespace pspocket::hle::psf
