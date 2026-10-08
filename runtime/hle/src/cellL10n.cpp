// cellL10n (D-517): libl10n's character-set conversions, pspocket's own. Each function is libl10n's, read from the
// firmware's module and compared with it call for call (tests/hle/test_l10n.cpp, PSPOCKET_TEST_DEV_FLASH): the same
// answers, the same output and the same lengths, including where they read past the given length. Its tables are
// generated from public mapping data (l10n_tables.hpp).
//
// libl10n's conventions:
// - A character function converts one character: the "to" side's count of units written or source units read (each
//   function says which), 0 for a character it cannot convert.
// - A string function (XstoYs: src, *src_len, dst, *dst_len, in units of each side) converts until *src_len units
//   are read: 0 (ConversionOK) with *dst_len the units written; 1 (SRCIllegal) at a character it cannot convert,
//   *src_len then the units left from it and *dst_len those written before it; 2 (DSTExhausted) when *dst_len units
//   would be passed, with the same two lengths; *src_len of 0 gives 0 and *dst_len 0. A NULL dst counts only.
// - Guest pointers are checked before use (D-042): a call that would read or write outside guest memory is refused
//   (logged) and answers 0 (a character function) or 1 (a string function), where libl10n would fault.

#include <array>
#include <cstring>
#include <mutex>
#include <format>
#include <string>
#include <vector>

#include "guest.hpp"
#include "l10n_tables.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {


constexpr u32 kOK = 0, kIllegal = 1, kExhausted = 2;

// Guest memory for one call: every access checked; the first that fails is logged and marks the call failed.
class Mem {
public:
    explicit Mem(PPUContext& ctx, const char* fn) : ctx_(ctx), b_(guest_base(ctx)), fn_(fn) {}

    bool ok() const { return ok_; }
    u8 r8(u32 a) { return check(a, 1) ? b_[a] : 0; }
    u16 r16(u32 a) { return check(a, 2) ? ps_read16(b_, a) : 0; }
    u32 r32(u32 a) { return check(a, 4) ? ps_read32(b_, a) : 0; }
    void w8(u32 a, u8 v)
    {
        if (check(a, 1)) b_[a] = v;
    }
    void w16(u32 a, u16 v)
    {
        if (check(a, 2)) ps_write16(b_, a, v);
    }
    void w32(u32 a, u32 v)
    {
        if (check(a, 4)) ps_write32(b_, a, v);
    }

private:
    bool check(u32 a, u32 n)
    {
        if (!ok_) return false;
        if (u64{a} + n <= GuestMemory::kGuestSize && thread_of(ctx_).kernel.memory().is_committed(a) &&
            thread_of(ctx_).kernel.memory().is_committed(a + n - 1))
            return true;
        ok_ = false;
        log_call_site(ctx_, b_, std::format("{}: {:#x} (+{}) is not guest memory; refused", fn_, a, n));
        return false;
    }

    PPUContext& ctx_;
    u8* b_;
    const char* fn_;
    bool ok_ = true;
};

// ------------------------------------------------------------------------------------------------- Unicode

bool surrogate(u32 c) { return (c & 0xF800) == 0xD800; }

// UCS2toUTF8 (libl10n 0xc980): 1-3 bytes, 0 for a surrogate.
u32 ucs2_to_utf8(u32 c, u8* out)
{
    c &= 0xFFFF;
    if (surrogate(c)) return 0;
    if (c > 0x7FF) {
        out[0] = static_cast<u8>(0xE0 | (c >> 12));
        out[1] = static_cast<u8>(0x80 | ((c >> 6) & 0x3F));
        out[2] = static_cast<u8>(0x80 | (c & 0x3F));
        return 3;
    }
    if (c > 0x7F) {
        out[0] = static_cast<u8>(0xC0 | (c >> 6));
        out[1] = static_cast<u8>(0x80 | (c & 0x3F));
        return 2;
    }
    out[0] = static_cast<u8>(c);
    return 1;
}

// UTF32toUTF8 (0xddb0): 1-4 bytes, 0 past U+10FFFF or for a surrogate.
u32 utf32_to_utf8(u32 c, u8* out)
{
    if (c > 0x10FFFF || c - 0xD800 <= 0x7FF) return 0;
    if (c > 0xFFFF) {
        out[0] = static_cast<u8>(0xF0 | (c >> 18));
        out[1] = static_cast<u8>(0x80 | ((c >> 12) & 0x3F));
        out[2] = static_cast<u8>(0x80 | ((c >> 6) & 0x3F));
        out[3] = static_cast<u8>(0x80 | (c & 0x3F));
        return 4;
    }
    return ucs2_to_utf8(c, out);
}

// UTF32toUTF16 (0xdd44): 1 or 2 units.
u32 utf32_to_utf16(u32 c, u16* out)
{
    if (c > 0x10FFFF || c - 0xD800 <= 0x7FF) return 0;
    if (c <= 0xFFFF) {
        out[0] = static_cast<u16>(c);
        return 1;
    }
    out[0] = static_cast<u16>(0xD800 | ((c - 0x10000) >> 10));
    out[1] = static_cast<u16>(0xDC00 | (c & 0x3FF));
    return 2;
}

bool cont(u8 b) { return (b & 0xC0) == 0x80; }

// A UTF-8 character from its bytes (read on demand): the bytes it takes and its code, or 0. `four` takes 4-byte
// sequences (UTF8toUTF16 and UTF8toUTF32); UTF8toUCS2 does not.
template <class Byte>
u32 utf8_decode(Byte&& at, u32* code, bool four)
{
    const u8 b0 = at(0);
    if (four && (b0 & 0xF8) == 0xF0) {
        const u8 b1 = at(1);
        if (!cont(b1)) return 0;
        const u8 b2 = at(2);
        if (!cont(b2)) return 0;
        const u8 b3 = at(3);
        if (!cont(b3)) return 0;
        *code = (u32{b0 & 7u} << 18) | (u32{b1 & 0x3Fu} << 12) | (u32{b2 & 0x3Fu} << 6) | (b3 & 0x3Fu);
        return 4;
    }
    if ((b0 & 0xF0) == 0xE0) {
        const u8 b1 = at(1);
        if (cont(b1)) {
            const u8 b2 = at(2);
            if (cont(b2)) {
                const u32 v = (u32{b0 & 0xFu} << 12) | (u32{b1 & 0x3Fu} << 6) | (b2 & 0x3Fu);
                if (v <= 0x7FF || surrogate(v)) return 0;
                *code = v;
                return 3;
            }
        }
        return 0;
    }
    if ((b0 & 0xE0) == 0xC0 && b0 > 0xC1) {
        const u8 b1 = at(1);
        if (!cont(b1)) return 0;
        *code = (u32{b0 & 0x1Fu} << 6) | (b1 & 0x3Fu);
        return 2;
    }
    if (b0 & 0x80) return 0;
    *code = b0;
    return 1;
}

// UTF8toUTF16's 4-byte rule (0x10a50): the plane from the first two bytes must be 1..16 (U+10000..U+10FFFF).
bool utf16_plane_ok(u32 code) { return code >= 0x10000 && code <= 0x10FFFF; }

// One step of a string conversion: source units taken (0: cannot convert), destination units given.
struct Step {
    u32 took = 0, n = 0;
    std::array<u32, 4> out{};
};

// libl10n's string loop (see the top). `past_end` refuses a character that would read past *src_len (UTF8stoUCS2s
// and the multibyte decoders); the others read on.
template <class F>
u32 string_loop(Mem& m, u32 src, u32 src_len, u32 dst, u32 dst_len, u32 in_unit, u32 out_unit, bool past_end, F step)
{
    if (!m.r32(src_len)) {
        m.w32(dst_len, 0);
        return m.ok() ? kOK : kIllegal;
    }
    u32 consumed = 0, count = 0;
    for (;;) {
        const Step s = step(src + consumed * in_unit);
        if (!m.ok()) return kIllegal;
        const u32 total = m.r32(src_len);
        if (!s.took || (past_end && total < consumed + s.took)) {
            m.w32(src_len, total - consumed);
            m.w32(dst_len, count);
            return kIllegal;
        }
        count += s.n;
        if (dst) {
            if (m.r32(dst_len) < count) {
                m.w32(src_len, total - consumed);
                m.w32(dst_len, count - s.n);
                return m.ok() ? kExhausted : kIllegal;
            }
            const u32 at = dst + (count - s.n) * out_unit;
            for (u32 k = 0; k < s.n; k++) {
                if (out_unit == 1) m.w8(at + k, static_cast<u8>(s.out[k]));
                else if (out_unit == 2) m.w16(at + 2 * k, static_cast<u16>(s.out[k]));
                else m.w32(at + 4 * k, s.out[k]);
            }
        }
        consumed += s.took;
        if (m.r32(src_len) <= consumed) {
            m.w32(dst_len, count);
            return m.ok() ? kOK : kIllegal;
        }
    }
}

// Step makers for the Unicode family.
auto from_utf8(Mem& m, bool four, u32 to)
{
    // to: 1 UTF-8 is not used here; 2 UCS-2, 16 UTF-16, 32 UTF-32
    return [&m, four, to](u32 at) {
        Step s;
        u32 code = 0;
        const u32 took = utf8_decode([&](u32 i) { return m.r8(at + i); }, &code, four);
        if (!took) return s;
        if (took == 4) {
            if (to == 16) {
                if (!utf16_plane_ok(code)) return s;
                u16 w[2];
                utf32_to_utf16(code, w);
                s.out = {w[0], w[1]};
                s.n = 2;
            } else {
                if (code - 0x10000 > 0xFFFFF) return s;
                s.out[0] = code;
                s.n = 1;
            }
        } else {
            s.out[0] = code;
            s.n = 1;
        }
        s.took = took;
        return s;
    };
}

u32 UTF8toUCS2(PPUContext& ctx, u32 src, u32 dst)
{
    Mem m(ctx, "UTF8toUCS2");
    u32 code = 0;
    const u32 took = utf8_decode([&](u32 i) { return m.r8(src + i); }, &code, false);
    if (!m.ok() || !took) return 0;
    m.w16(dst, static_cast<u16>(code));
    return m.ok() ? took : 0;
}

// UTF8toUTF16(src, dst, *len): the bytes read; *len the units written (set only on success).
u32 UTF8toUTF16(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    Mem m(ctx, "UTF8toUTF16");
    const Step s = from_utf8(m, true, 16)(src);
    if (!m.ok() || !s.took) return 0;
    for (u32 k = 0; k < s.n; k++) m.w16(dst + 2 * k, static_cast<u16>(s.out[k]));
    m.w32(len, s.n);
    return m.ok() ? s.took : 0;
}

u32 UTF8toUTF32(PPUContext& ctx, u32 src, u32 dst)
{
    Mem m(ctx, "UTF8toUTF32");
    const Step s = from_utf8(m, true, 32)(src);
    if (!m.ok() || !s.took) return 0;
    m.w32(dst, s.out[0]);
    return m.ok() ? s.took : 0;
}

// UCS2toUTF8(c, dst): the bytes written.
u32 UCS2toUTF8(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UCS2toUTF8");
    u8 out[4];
    const u32 n = ucs2_to_utf8(c, out);
    for (u32 k = 0; k < n; k++) m.w8(dst + k, out[k]);
    return m.ok() ? n : 0;
}

// UCS2toUTF16 and UCS2toUTF32(c, dst): 1, or 0 for a surrogate.
u32 UCS2toUTF16(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UCS2toUTF16");
    if (surrogate(c)) return 0;
    m.w16(dst, static_cast<u16>(c));
    return m.ok() ? 1 : 0;
}

u32 UCS2toUTF32(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UCS2toUTF32");
    if (surrogate(c)) return 0;
    m.w32(dst, c);
    return m.ok() ? 1 : 0;
}

// UTF16toUCS2(src, dst): 1, or 0 for a surrogate.
u32 UTF16toUCS2(PPUContext& ctx, u32 src, u32 dst)
{
    Mem m(ctx, "UTF16toUCS2");
    const u16 c = m.r16(src);
    if (!m.ok() || surrogate(c)) return 0;
    m.w16(dst, c);
    return m.ok() ? 1 : 0;
}

// UTF-16 to a code point: the units taken (1, 2 for a pair), 0 for a lone surrogate (0xd708).
template <class Unit>
u32 utf16_decode(Unit&& at, u32* code)
{
    const u16 c = at(0);
    if (!surrogate(c)) {
        *code = c;
        return 1;
    }
    if ((c & 0xFC00) != 0xD800) return 0;
    const u16 c2 = at(1);
    if ((c2 & 0xFC00) != 0xDC00) return 0;
    *code = ((u32{c} & 0x3FF) << 10) + 0x10000 + (c2 & 0x3FFu);
    return 2;
}

u32 UTF16toUTF32(PPUContext& ctx, u32 src, u32 dst)
{
    Mem m(ctx, "UTF16toUTF32");
    u32 code = 0;
    const u32 took = utf16_decode([&](u32 i) { return m.r16(src + 2 * i); }, &code);
    if (!m.ok() || !took) return 0;
    m.w32(dst, code);
    return m.ok() ? took : 0;
}

// UTF16toUTF8(src, dst, *len): the units read; *len the bytes written (set only on success).
u32 UTF16toUTF8(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    Mem m(ctx, "UTF16toUTF8");
    u32 code = 0;
    const u32 took = utf16_decode([&](u32 i) { return m.r16(src + 2 * i); }, &code);
    if (!m.ok() || !took) return 0;
    u8 out[4];
    const u32 n = utf32_to_utf8(code, out);
    for (u32 k = 0; k < n; k++) m.w8(dst + k, out[k]);
    m.w32(len, n);
    return m.ok() ? took : 0;
}

// UTF32toUCS2(c, dst): 1, or 0 past U+FFFF or for a surrogate.
u32 UTF32toUCS2(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UTF32toUCS2");
    if (c > 0xFFFF || c - 0xD800 <= 0x7FF) return 0;
    m.w16(dst, static_cast<u16>(c));
    return m.ok() ? 1 : 0;
}

// UTF32toUTF16(c, dst): the units written (1 or 2).
u32 UTF32toUTF16(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UTF32toUTF16");
    u16 out[2];
    const u32 n = utf32_to_utf16(c, out);
    for (u32 k = 0; k < n; k++) m.w16(dst + 2 * k, out[k]);
    return m.ok() ? n : 0;
}

u32 UTF32toUTF8(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UTF32toUTF8");
    u8 out[4];
    const u32 n = utf32_to_utf8(c, out);
    for (u32 k = 0; k < n; k++) m.w8(dst + k, out[k]);
    return m.ok() ? n : 0;
}

// The string forms.
u32 UTF8stoUCS2s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UTF8stoUCS2s");
    return string_loop(m, src, src_len, dst, dst_len, 1, 2, true, from_utf8(m, false, 2));
}

u32 UTF8stoUTF16s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UTF8stoUTF16s");
    return string_loop(m, src, src_len, dst, dst_len, 1, 2, false, from_utf8(m, true, 16));
}

u32 UTF8stoUTF32s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UTF8stoUTF32s");
    return string_loop(m, src, src_len, dst, dst_len, 1, 4, false, from_utf8(m, true, 32));
}

auto from_ucs2(Mem& m, u32 to)
{
    return [&m, to](u32 at) {
        Step s;
        const u16 c = m.r16(at);
        if (surrogate(c)) return s;
        if (to == 8) {
            u8 out[4];
            s.n = ucs2_to_utf8(c, out);
            for (u32 k = 0; k < s.n; k++) s.out[k] = out[k];
        } else {
            s.out[0] = c;
            s.n = 1;
        }
        s.took = 1;
        return s;
    };
}

u32 UCS2stoUTF8s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UCS2stoUTF8s");
    return string_loop(m, src, src_len, dst, dst_len, 2, 1, false, from_ucs2(m, 8));
}

u32 UCS2stoUTF16s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UCS2stoUTF16s");
    return string_loop(m, src, src_len, dst, dst_len, 2, 2, false, from_ucs2(m, 16));
}

u32 UCS2stoUTF32s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UCS2stoUTF32s");
    return string_loop(m, src, src_len, dst, dst_len, 2, 4, false, from_ucs2(m, 32));
}

auto from_utf16(Mem& m, u32 to)
{
    return [&m, to](u32 at) {
        Step s;
        u32 code = 0;
        const u32 took = utf16_decode([&](u32 i) { return m.r16(at + 2 * i); }, &code);
        if (!took) return s;
        if (to == 2) {
            if (took != 1) return s; // UTF16toUCS2 takes no pair
            s.out[0] = code;
            s.n = 1;
        } else if (to == 8) {
            u8 out[4];
            s.n = utf32_to_utf8(code, out);
            for (u32 k = 0; k < s.n; k++) s.out[k] = out[k];
        } else {
            s.out[0] = code;
            s.n = 1;
        }
        s.took = took;
        return s;
    };
}

u32 UTF16stoUCS2s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UTF16stoUCS2s");
    return string_loop(m, src, src_len, dst, dst_len, 2, 2, false, from_utf16(m, 2));
}

u32 UTF16stoUTF8s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UTF16stoUTF8s");
    return string_loop(m, src, src_len, dst, dst_len, 2, 1, false, from_utf16(m, 8));
}

u32 UTF16stoUTF32s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UTF16stoUTF32s");
    return string_loop(m, src, src_len, dst, dst_len, 2, 4, false, from_utf16(m, 32));
}

auto from_utf32(Mem& m, u32 to)
{
    return [&m, to](u32 at) {
        Step s;
        const u32 c = m.r32(at);
        if (to == 2) {
            if (c > 0xFFFF || c - 0xD800 <= 0x7FF) return s;
            s.out[0] = c;
            s.n = 1;
        } else if (to == 16) {
            u16 out[2];
            s.n = utf32_to_utf16(c, out);
            s.out = {out[0], out[1]};
        } else {
            u8 out[4];
            s.n = utf32_to_utf8(c, out);
            for (u32 k = 0; k < s.n; k++) s.out[k] = out[k];
        }
        if (s.n) s.took = 1;
        return s;
    };
}

u32 UTF32stoUCS2s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UTF32stoUCS2s");
    return string_loop(m, src, src_len, dst, dst_len, 4, 2, false, from_utf32(m, 2));
}

u32 UTF32stoUTF16s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UTF32stoUTF16s");
    return string_loop(m, src, src_len, dst, dst_len, 4, 2, false, from_utf32(m, 16));
}

u32 UTF32stoUTF8s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len)
{
    Mem m(ctx, "UTF32stoUTF8s");
    return string_loop(m, src, src_len, dst, dst_len, 4, 1, false, from_utf32(m, 8));
}

// ------------------------------------------------------------------------------------------------- tables

// The packed tables of l10n_tables.hpp, unpacked once.
std::vector<u16> unpack(const l10n_data::Packed& p)
{
    static constexpr auto kDecode = [] {
        std::array<s8, 256> d{};
        d.fill(-1);
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) d[static_cast<u8>(alphabet[i])] = static_cast<s8>(i);
        return d;
    }();
    std::vector<u8> bytes;
    u32 acc = 0;
    int bits = 0;
    for (const char* c = p.base64; *c && *c != '='; c++) {
        const s8 v = kDecode[static_cast<u8>(*c)];
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<u8>(acc >> bits));
        }
    }
    std::vector<u16> t;
    t.reserve(p.count);
    u16 last = 0;
    usize i = 0;
    while (t.size() < p.count && i < bytes.size()) {
        const u8 c = bytes[i++];
        if (c < 0x40) {
            for (u32 k = 0; k <= c; k++) t.push_back(++last);
        } else if (c < 0x60) {
            for (u32 k = 0; k < c - 0x3Fu; k++) t.push_back(0xFFFF);
        } else if (c < 0xC0) {
            last = static_cast<u16>(last + 1 + c - 0x90);
            t.push_back(last);
        } else if (c < 0xE0) {
            last = static_cast<u16>(last + 1 + ((u32{c & 0x1Fu} << 8) | bytes[i++]) - 0x1000);
            t.push_back(last);
        } else {
            last = static_cast<u16>((u32{bytes[i]} << 8) | bytes[i + 1]);
            i += 2;
            t.push_back(last);
        }
    }
    t.resize(p.count, 0xFFFF);
    return t;
}

enum Table : u32 { kJis0208, kRow13, kNecsel, kIbmext, kJis0212, kKsx1001, kUhc1, kUhc2, kGb2312, kGbkrows, kGbk3,
    kGbk4, kGbrows18030, kGbd7, kGba8, kGbfe, kBig5, kArib13, kAribadd, kSbcs, kTableCount };

const std::vector<u16>& table(Table which)
{
    static std::once_flag once;
    static std::array<std::vector<u16>, kTableCount> all;
    std::call_once(once, [] {
        for (u32 i = 0; i < kTableCount; i++) all[i] = unpack(l10n_data::kTables[i]);
    });
    return all[which];
}

// ------------------------------------------------------------------------------------------------- SBCS

// The upper half of a single-byte code page (libl10n's codes: ISO 8859-1..16 without 12 are 5..19, CP437, 850, 863,
// 866 are 20..23, CP1251 and 1252 are 28 and 29, CP852 to 869 are 38..51), or nullptr for another code.
const u16* sbcs_page(u32 code)
{
    static constexpr s8 kIndex[52] = {-1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, -1, -1, -1, -1, 19, 20, -1, -1, -1, -1, -1, -1, -1, -1, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        32, 33, 34};
    if (code > 51 || kIndex[code] < 0) return nullptr;
    return table(kSbcs).data() + 128 * static_cast<u32>(kIndex[code]);
}

// UCS2toSBCS's search (0xc4d4): the byte of c in the page, or 0.
u32 sbcs_byte(const u16* page, u32 c)
{
    c &= 0xFFFF;
    if (c > 0xFFFD) return 0;
    if (c <= 0x7F) return c ? c : 0x100; // 0x100: the byte 0
    for (u32 i = 0; i < 128; i++)
        if (page[i] == c) return 0x80 + i;
    return 0;
}

u32 utf8_bytes_of(u32 v, u8* out)
{
    if (v > 0x7FF) {
        out[0] = static_cast<u8>(0xE0 | (v >> 12));
        out[1] = static_cast<u8>(0x80 | ((v >> 6) & 0x3F));
        out[2] = static_cast<u8>(0x80 | (v & 0x3F));
        return 3;
    }
    if (v > 0x7F) {
        out[0] = static_cast<u8>(0xC0 | (v >> 6));
        out[1] = static_cast<u8>(0x80 | (v & 0x3F));
        return 2;
    }
    out[0] = static_cast<u8>(v);
    return 1;
}

// SBCStoUCS2(c, dst, code): 1, or 0 for an unassigned byte (whose FFFF is still written); -1 for another code.
u32 SBCStoUCS2(PPUContext& ctx, u32 c, u32 dst, u32 code)
{
    Mem m(ctx, "SBCStoUCS2");
    const u16* page = sbcs_page(code);
    if (!page) return ~0u;
    c &= 0xFF;
    const u16 v = c < 0x80 ? static_cast<u16>(c) : page[c - 0x80];
    m.w16(dst, v);
    return m.ok() && v != 0xFFFF ? 1 : 0;
}

// SBCStoUTF8(c, dst, code): the bytes written, 0 for an unassigned byte, -1 for another code.
u32 SBCStoUTF8(PPUContext& ctx, u32 c, u32 dst, u32 code)
{
    Mem m(ctx, "SBCStoUTF8");
    const u16* page = sbcs_page(code);
    if (!page) return ~0u;
    c &= 0xFF;
    if (c < 0x80) {
        m.w8(dst, static_cast<u8>(c));
        return m.ok() ? 1 : 0;
    }
    const u16 v = page[c - 0x80];
    if (v == 0xFFFF) return 0;
    u8 out[4];
    const u32 n = utf8_bytes_of(v, out);
    for (u32 k = 0; k < n; k++) m.w8(dst + k, out[k]);
    return m.ok() ? n : 0;
}

// UCS2toSBCS(c, dst, code): 1, 0 when the page has no byte for c, -1 for another code.
u32 UCS2toSBCS(PPUContext& ctx, u32 c, u32 dst, u32 code)
{
    Mem m(ctx, "UCS2toSBCS");
    const u16* page = sbcs_page(code);
    if (!page) return ~0u;
    const u32 byte = sbcs_byte(page, c);
    if (!byte) return 0;
    m.w8(dst, static_cast<u8>(byte));
    return m.ok() ? 1 : 0;
}

// UTF8toSBCS(src, dst, code): the UTF-8 bytes read; 0 for a sequence that is not UCS-2 or a character the page lacks,
// -1 for another code (only once the sequence was read).
u32 UTF8toSBCS(PPUContext& ctx, u32 src, u32 dst, u32 code)
{
    Mem m(ctx, "UTF8toSBCS");
    u32 c = 0;
    const u32 took = utf8_decode([&](u32 i) { return m.r8(src + i); }, &c, false);
    if (!m.ok() || !took) return 0;
    const u16* page = sbcs_page(code);
    if (!page) return ~0u;
    const u32 byte = sbcs_byte(page, c);
    if (!byte) return 0;
    m.w8(dst, static_cast<u8>(byte));
    return m.ok() ? took : 0;
}

constexpr u32 kUnknownCode = 3;

u32 SBCSstoUCS2s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len, u32 code)
{
    Mem m(ctx, "SBCSstoUCS2s");
    const u16* page = sbcs_page(code);
    if (!page) return kUnknownCode;
    return string_loop(m, src, src_len, dst, dst_len, 1, 2, false, [&](u32 at) {
        Step s;
        const u8 c = m.r8(at);
        const u16 v = c < 0x80 ? c : page[c - 0x80];
        if (v == 0xFFFF) return s;
        s.out[0] = v;
        s.n = 1;
        s.took = 1;
        return s;
    });
}

u32 SBCSstoUTF8s(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len, u32 code)
{
    Mem m(ctx, "SBCSstoUTF8s");
    const u16* page = sbcs_page(code);
    if (!page) return kUnknownCode;
    return string_loop(m, src, src_len, dst, dst_len, 1, 1, false, [&](u32 at) {
        Step s;
        const u8 c = m.r8(at);
        const u16 v = c < 0x80 ? c : page[c - 0x80];
        if (v == 0xFFFF) return s;
        u8 out[4];
        s.n = utf8_bytes_of(v, out);
        for (u32 k = 0; k < s.n; k++) s.out[k] = out[k];
        s.took = 1;
        return s;
    });
}

u32 UCS2stoSBCSs(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len, u32 code)
{
    Mem m(ctx, "UCS2stoSBCSs");
    const u16* page = sbcs_page(code);
    if (!page) return kUnknownCode;
    return string_loop(m, src, src_len, dst, dst_len, 2, 1, false, [&](u32 at) {
        Step s;
        const u32 byte = sbcs_byte(page, m.r16(at));
        if (!byte) return s;
        s.out[0] = byte & 0xFF;
        s.n = 1;
        s.took = 1;
        return s;
    });
}

u32 UTF8stoSBCSs(PPUContext& ctx, u32 src, u32 src_len, u32 dst, u32 dst_len, u32 code)
{
    Mem m(ctx, "UTF8stoSBCSs");
    const u16* page = sbcs_page(code);
    if (!page) return kUnknownCode;
    return string_loop(m, src, src_len, dst, dst_len, 1, 1, true, [&](u32 at) {
        Step s;
        u32 c = 0;
        const u32 took = utf8_decode([&](u32 i) { return m.r8(at + i); }, &c, false);
        if (!took) return s;
        const u32 byte = sbcs_byte(page, c);
        if (!byte) return s;
        s.out[0] = byte & 0xFF;
        s.n = 1;
        s.took = took;
        return s;
    });
}

// ------------------------------------------------------------------------------------------------- Japanese

// The numeric helpers (libl10n 0x10d48-0x113e0): a code of one form to another, 0 when it is not one.
u32 eucjp_ok(u32 c) { return ((c - 0xA1A1) & 0xFFFF) <= 0x5D5D && (c & 0xFF) > 0xA0 && (c & 0xFF) <= 0xFE; }
u32 jis_ok(u32 c) { return ((c - 0x2121) & 0xFFFF) <= 0x5D5D && (c & 0xFF) > 0x20 && (c & 0xFF) <= 0x7E; }
u32 kuten_ok(u32 c, u32 rows) { return ((c - 0x101) & 0xFFFF) <= rows && (c & 0xFF) > 0 && (c & 0xFF) <= 94; }

u32 jis_to_sjis(u32 c)
{
    if (!jis_ok(c)) return 0;
    const u32 hi = (c & 0xFFFF) >> 8, lo = c & 0xFF;
    const s32 r = (static_cast<s32>(hi) - 33) >> 1;
    u32 lead = static_cast<u32>(r + 129) & 0xFFFF;
    if (lead > 0x9F) lead = static_cast<u32>(r + 193) & 0xFFFF;
    const u32 trail = (hi & 1) ? (lo <= 0x5F ? lo + 31 : lo + 32) : lo + 126;
    return ((lead << 8) | trail) & 0xFFFF;
}

u32 kuten_to_sjis(u32 c)
{
    if (!kuten_ok(c, 0x77FF)) return 0;
    const u32 hi = ((c & 0xFFFF) >> 8) + 32, lo = c & 0xFF;
    const s32 r = (static_cast<s32>(hi) - 33) >> 1;
    u32 lead = static_cast<u32>(r + 129) & 0xFFFF;
    if (lead > 0x9F) lead = static_cast<u32>(r + 193) & 0xFFFF;
    const u32 trail = (hi & 1) ? (lo + 32 <= 0x5F ? lo + 63 : lo + 64) : lo + 158;
    return ((lead << 8) | trail) & 0xFFFF;
}

// Shift-JIS to a JIS row and cell (sjis2eucjp, sjis2jis, sjis2kuten share it): false when it is not a two-byte code.
bool sjis_split(u32 c, u32* row, u32* cell)
{
    const u32 hi = (c >> 8) & 0xFF, lo = c & 0xFF;
    if (((hi - 129) & 0xFFFF) > 123 || ((hi - 160) & 0xFFFF) <= 63) return false;
    if (lo <= 63 || lo > 252 || lo == 127) return false;
    const u32 r = hi > 0x9F ? hi - 193 : hi - 129;
    if (lo > 158) {
        *row = r * 2 + 2;
        *cell = lo - 158;
    } else {
        *row = r * 2 + 1;
        *cell = lo - (lo > 126 ? 32 : 31) - 32;
    }
    return true;
}

u32 sjis_to_jis(u32 c)
{
    u32 row, cell;
    if (!sjis_split(c, &row, &cell)) return 0;
    const u32 hi = (row + 32) & 0xFFFF;
    if (hi > 126) return 0;
    return ((hi << 8) | (cell + 32)) & 0xFFFF;
}

u32 sjis_to_kuten(u32 c)
{
    u32 row, cell;
    if (!sjis_split(c, &row, &cell)) return 0;
    return ((row << 8) | cell) & 0xFFFF;
}

u32 eucjp2jis(PPUContext&, u32 c) { return eucjp_ok(c) ? (c & 0x7F7F) : 0; }
u32 eucjp2kuten(PPUContext&, u32 c) { return eucjp_ok(c) ? ((c + 0x5F60) & 0xFFFF) : 0; }
u32 eucjp2sjis(PPUContext&, u32 c) { return eucjp_ok(c) ? jis_to_sjis(c & 0x7F7F) : 0; }
u32 jis2eucjp(PPUContext&, u32 c) { return jis_ok(c) ? ((c & 0xFFFF) | 0x8080) : 0; }
u32 jis2kuten(PPUContext&, u32 c) { return jis_ok(c) ? ((c - 0x2020) & 0xFFFF) : 0; }
u32 jis2sjis(PPUContext&, u32 c) { return jis_to_sjis(c); }
u32 kuten2eucjp(PPUContext&, u32 c) { return kuten_ok(c, 0x5D5D) ? ((c + 0xA0A0) & 0xFFFF) : 0; }
u32 kuten2jis(PPUContext&, u32 c) { return kuten_ok(c, 0x5D5D) ? ((c + 0x2020) & 0xFFFF) : 0; }
u32 kuten2sjis(PPUContext&, u32 c) { return kuten_to_sjis(c); }
u32 sjis2eucjp(PPUContext&, u32 c)
{
    const u32 j = sjis_to_jis(c);
    return j ? (j | 0x8080) : 0;
}
u32 sjis2jis(PPUContext&, u32 c) { return sjis_to_jis(c); }
u32 sjis2kuten(PPUContext&, u32 c) { return sjis_to_kuten(c); }

// EUCJPtoUCS2's lookups (0x43a4): a two-byte EUC-JP code's UCS-2 value, FFFF for none.
u16 eucjp2_ucs2(u32 b0, u32 b1)
{
    if (b0 == 0xAD) return table(kRow13)[b1 - 0xA1];
    const u32 c = (b0 << 8) | b1;
    if (((c - 0xA1A1) & 0xFFFF) <= 0x5305 && ((b0 - 0xA9) & 0xFF) > 6)
        return table(kJis0208)[b0 * 94 + b1 - (b0 <= 0xAF ? 15295 : 15953)];
    if (((b0 - 0xF9) & 0xFF) <= 3) return table(kNecsel)[(b0 - 0xF9) * 94 + (b1 - 0xA1)];
    return 0xFFFF;
}

// A JIS X 0212 code (EUC-JP's 8F b1 b2): its UCS-2 value, FFFF for none.
u16 jis0212_ucs2(u32 b1, u32 b2)
{
    const u32 c = (b1 << 8) | b2;
    if (((c - 0xA2AF) & 0xFFFF) > 0x4B34) return 0xFFFF;
    if (((b1 - 0xA3) & 0xFF) <= 2 || b1 == 0xA8 || ((b1 - 0xAC) & 0xFF) <= 3) return 0xFFFF;
    const u32 base = b1 <= 0xA5 ? 15389 : b1 <= 0xA8 ? 15671 : b1 <= 0xAF ? 15765 : 16141;
    return table(kJis0212)[b1 * 94 + b2 - base];
}

bool in_a1_fe(u32 b) { return ((b - 0xA1) & 0xFF) <= 93; }

// EUCJPtoUCS2: the bytes read (1-3), or 0. 80-9F (but 8E, 8F) pass as U+0080-U+009F.
template <class Byte>
u32 eucjp_decode(Byte&& at, u16* out)
{
    const u8 b0 = at(0);
    if (in_a1_fe(b0)) {
        const u8 b1 = at(1);
        if (in_a1_fe(b1)) {
            const u16 v = eucjp2_ucs2(b0, b1);
            if (v == 0xFFFF) return 0;
            *out = v;
            return 2;
        }
    }
    if (b0 == 0x8E) {
        const u8 b1 = at(1);
        if (((b1 - 0xA1) & 0xFF) > 62) return 0;
        *out = static_cast<u16>(0xFEC0 + b1);
        return 2;
    }
    if (b0 == 0x8F) {
        const u8 b1 = at(1);
        if (!in_a1_fe(b1)) return 0;
        const u8 b2 = at(2);
        if (!in_a1_fe(b2)) return 0;
        const u16 v = jis0212_ucs2(b1, b2);
        if (v == 0xFFFF) return 0;
        *out = v;
        return 3;
    }
    if (b0 & 0x80) {
        if ((b0 & 0xE0) != 0x80) return 0;
    }
    *out = b0;
    return 1;
}

// UCS2toEUCJP (0xb8e4): the bytes of c in out, their count, 0 for none.
u32 ucs2_to_eucjp(u32 c, u8* out)
{
    c &= 0xFFFF;
    if (c > 0xFFFD) return 0;
    if (c <= 0x8D || ((c - 0x90) & 0xFFFF) <= 15) {
        out[0] = static_cast<u8>(c);
        return 1;
    }
    if (((c + 0x9F) & 0xFFFF) <= 62) {
        out[0] = 0x8E;
        out[1] = static_cast<u8>(c + 0x40);
        return 2;
    }
    const auto& j0208 = table(kJis0208);
    for (u32 i = 0; i < j0208.size(); i++) {
        if (j0208[i] == c) {
            u32 e = ((i / 94) << 8) + (i % 94) + 0xA1A1;
            if ((e & 0xFFFF) > 0xA8FF) e += 0x700;
            out[0] = static_cast<u8>(e >> 8);
            out[1] = static_cast<u8>(e);
            return 2;
        }
    }
    const auto& row13 = table(kRow13);
    for (u32 i = 0; i < 94; i++) {
        if (row13[i] == c) {
            out[0] = 0xAD;
            out[1] = static_cast<u8>(0xA1 + i);
            return 2;
        }
    }
    const auto& necsel = table(kNecsel);
    for (u32 i = 0; i < necsel.size() && necsel[i]; i++) {
        if (necsel[i] == c) {
            out[0] = static_cast<u8>(0xF9 + i / 94);
            out[1] = static_cast<u8>(0xA1 + i % 94);
            return 2;
        }
    }
    const auto& j0212 = table(kJis0212);
    for (u32 i = 0; i < j0212.size(); i++) {
        if (j0212[i] == c) {
            u32 e = ((i / 94) << 8) + (i % 94) + 0xA2A1;
            if ((e & 0xFFFF) > 0xA2FF) {
                const u32 a = e + 0x300;
                if ((a & 0xFFFF) <= 0xA7FF) e = a;
                else if (((e + 0x400) & 0xFFFF) <= 0xABFF) e += 0x400;
                else e += 0x800;
            }
            out[0] = 0x8F;
            out[1] = static_cast<u8>(e >> 8);
            out[2] = static_cast<u8>(e);
            return 3;
        }
    }
    return 0;
}

// SJIStoUCS2 (0x8d90): the bytes read (1, 2), or 0.
template <class Byte>
u32 sjis_decode(Byte&& at, u16* out)
{
    const u8 b0 = at(0);
    if (((b0 - 0x81) & 0xFF) <= 30 || ((b0 - 0xE0) & 0xFF) <= 28) {
        const u32 k = sjis_to_kuten((u32{b0} << 8) | at(1));
        if (!k) return 0;
        const u32 jis = k + 0x2020;
        if (((jis - 0x9321) & 0xFFFF) <= 1035) {
            *out = table(kIbmext)[(jis >> 8) * 94 + (jis & 0xFF) - 13851];
            return 2;
        }
        if (((k - 0x5901) & 0xFFFF) <= 861) {
            const u16 v = table(kNecsel)[(jis >> 8) * 94 + (jis & 0xFF) - 11407];
            if (v == 0xFFFF) return 0;
            *out = v;
            return 2;
        }
        if ((jis & 0xFFFF) > 0x7EFF) return 0;
        const u32 e = jis | 0x8080;
        u16 v;
        const u8 e0 = static_cast<u8>(e >> 8), e1 = static_cast<u8>(e);
        if (!eucjp_decode([&](u32 i) { return i == 0 ? e0 : e1; }, &v)) return 0;
        *out = v;
        return 2;
    }
    if (b0 & 0x80) {
        if (((b0 - 0xA1) & 0xFF) > 62) return 0;
        *out = static_cast<u16>(0xFEC0 + b0);
        return 1;
    }
    *out = b0;
    return 1;
}

// UCS2toSJIS (0xc568): the bytes in out, their count, 0 for none.
u32 ucs2_to_sjis(u32 c, u8* out)
{
    u8 e[3];
    const u32 r = ucs2_to_eucjp(c & 0xFFFF, e);
    if (r == 2) {
        if (e[0] == 0x8E) {
            out[0] = e[1];
            return 1;
        }
        const u32 s = jis_to_sjis(((u32{e[0]} << 8) | e[1]) & 0x7F7F);
        out[0] = static_cast<u8>(s >> 8);
        out[1] = static_cast<u8>(s);
        return 2;
    }
    if (r == 3) {
        const u32 c16 = c & 0xFFFF;
        u32 kuten = 0;
        const auto& row13 = table(kRow13);
        for (u32 i = 0; i < 94 && !kuten; i++)
            if (row13[i] == c16) kuten = 0x0D00 | (i + 1);
        if (!kuten) {
            const auto& ibm = table(kIbmext);
            for (u32 i = 0; i < ibm.size() && ibm[i] && !kuten; i++)
                if (ibm[i] == c16) kuten = ((115 + i / 94) << 8) + (i % 94) + 1;
        }
        if (!kuten) return 0;
        const u32 s = kuten_to_sjis(kuten);
        out[0] = static_cast<u8>(s >> 8);
        out[1] = static_cast<u8>(s);
        return 2;
    }
    if (r == 1) {
        if (((c - 0x80) & 0xFFFF) <= 31) return 0;
        out[0] = e[0];
        return 1;
    }
    return 0;
}

// The JIS code of an IBM extension (SJIS FA40-FC4B, kuten rows 115-119) as EUC-JP has it: the same character in NEC's
// selection of the IBM extensions (rows 89-92), else NEC's row 13, else JIS X 0208 (the order libl10n's table follows:
// FA54 is EEF9's FCFB, FA5B row 13's ADDA).
u32 ibm_to_jis(u32 index)
{
    static const std::vector<u32> map = [] {
        const auto& ibm = table(kIbmext);
        const auto& j0208 = table(kJis0208);
        const auto& row13 = table(kRow13);
        const auto& necsel = table(kNecsel);
        std::vector<u32> m(ibm.size(), 0);
        for (u32 i = 0; i < ibm.size(); i++) {
            const u16 u = ibm[i];
            if (u == 0xFFFF) continue;
            for (u32 k = 0; k < necsel.size() && !m[i]; k++)
                if (necsel[k] == u) m[i] = ((0x79 + k / 94) << 8) + 0x21 + k % 94;
            for (u32 k = 0; k < 94 && !m[i]; k++)
                if (row13[k] == u) m[i] = 0x2D21 + k;
            for (u32 k = 0; k < j0208.size() && !m[i]; k++) {
                if (j0208[k] == u) {
                    u32 e = ((k / 94) << 8) + (k % 94) + 0x2121;
                    if ((e & 0xFFFF) > 0x28FF) e += 0x700;
                    m[i] = e;
                }
            }
        }
        return m;
    }();
    return index < map.size() ? map[index] : 0;
}

// SJIS to EUC-JP (0x8a04): the bytes read, the EUC-JP code in *out (8Exx for a half-width kana), or 0.
template <class Byte>
u32 sjis_to_eucjp_code(Byte&& at, u32* out)
{
    const u8 b0 = at(0);
    if (!(b0 & 0x80)) {
        *out = b0;
        return 1;
    }
    if (((b0 - 0xA1) & 0xFF) <= 62) {
        *out = 0x8E00 | b0;
        return 1;
    }
    if (((b0 - 0x81) & 0xFF) > 30 && ((b0 - 0xE0) & 0xFF) > 28) return 0;
    const u32 k = sjis_to_kuten((u32{b0} << 8) | at(1));
    if (!k) return 0;
    const u32 jis = k + 0x2020;
    if (((jis - 0x9321) & 0xFFFF) <= 1035) {
        *out = (ibm_to_jis((jis >> 8) * 94 + (jis & 0xFF) - 13851) | 0x8080) & 0xFFFF;
        return 2;
    }
    if ((jis & 0xFFFF) > 0x7EFF) return 0;
    *out = (jis | 0x8080) & 0xFFFF;
    return 2;
}

// EUC-JP to SJIS (0x4048): the bytes read, the SJIS code in *out (one byte for a half-width kana), or 0.
template <class Byte>
u32 eucjp_to_sjis_code(Byte&& at, u32* out)
{
    const u8 b0 = at(0);
    if (in_a1_fe(b0)) {
        const u8 b1 = at(1);
        if (in_a1_fe(b1)) {
            *out = jis_to_sjis(((u32{b0} << 8) | b1) & 0x7F7F);
            return 2;
        }
    }
    if (b0 == 0x8E) {
        const u8 b1 = at(1);
        if (((b1 - 0xA1) & 0xFF) > 62) return 0;
        *out = b1;
        return 2;
    }
    if (b0 & 0x80) return 0;
    *out = b0;
    return 1;
}

// MSJIStoUCS2 (0x7d10): a two-byte cp932 code's UCS-2 value; its user-defined area F640-F9FC as libl10n maps it.
bool msjis_ucs2(u32 c, u16* out)
{
    const u32 k = sjis_to_kuten(c & 0xFFFF);
    if (!k) return false;
    const u32 row = k >> 8, cell = k & 0xFF;
    if (((k - 0x101) & 0xFFFF) <= 0x5305 && ((row - 9) & 0xFF) > 6) {
        const u16 v = table(kJis0208)[row * 94 + cell - (row <= 15 ? 95 : 753)];
        if (v == 0xFFFF) return false;
        *out = v;
        return true;
    }
    if (((c - 0xF640) & 0xFFFF) > 956) return false;
    *out = static_cast<u16>(row * 94 + cell - 17227);
    return true;
}

// UCS2toMSJIS (0xc390): the cp932 code of c (written even when 0); 1 when there is one.
u32 ucs2_to_msjis(u32 c, u32* out)
{
    c &= 0xFFFF;
    if (c > 0xFFFD) return 0;
    if (((c - 0xE400) & 0xFFFF) <= 751) {
        const u32 v = c - 0xE400;
        *out = kuten_to_sjis((((v / 94) + 107) << 8) | ((v % 94) + 1));
        return *out ? 1 : 0;
    }
    const auto& j0208 = table(kJis0208);
    for (u32 i = 0; i < j0208.size(); i++) {
        if (j0208[i] == c) {
            u32 e = ((i / 94) << 8) + (i % 94) + 0xA1A1;
            if ((e & 0xFFFF) > 0xA8FF) e += 0x700;
            *out = jis_to_sjis(e & 0x7F7F);
            return 2;
        }
    }
    return 0;
}

u32 EUCJPtoUCS2(PPUContext& ctx, u32 src, u32 dst)
{
    Mem m(ctx, "EUCJPtoUCS2");
    u16 v = 0;
    const u32 n = eucjp_decode([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    m.w16(dst, v);
    return m.ok() ? n : 0;
}

u32 SJIStoUCS2(PPUContext& ctx, u32 src, u32 dst)
{
    Mem m(ctx, "SJIStoUCS2");
    u16 v = 0;
    const u32 n = sjis_decode([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    m.w16(dst, v);
    return m.ok() ? n : 0;
}

// EUCJPtoUTF8 and SJIStoUTF8(src, dst, *len): the bytes read; *len the UTF-8 bytes (only when read).
u32 EUCJPtoUTF8(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    Mem m(ctx, "EUCJPtoUTF8");
    u16 v = 0;
    const u32 n = eucjp_decode([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    u8 out[4];
    const u32 k = ucs2_to_utf8(v, out);
    for (u32 i = 0; i < k; i++) m.w8(dst + i, out[i]);
    m.w32(len, k);
    return m.ok() ? n : 0;
}

u32 SJIStoUTF8(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    Mem m(ctx, "SJIStoUTF8");
    u16 v = 0;
    const u32 n = sjis_decode([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    u8 out[4];
    const u32 k = ucs2_to_utf8(v, out);
    for (u32 i = 0; i < k; i++) m.w8(dst + i, out[i]);
    m.w32(len, k);
    return m.ok() ? n : 0;
}

u32 UCS2toEUCJP(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UCS2toEUCJP");
    u8 out[3];
    const u32 n = ucs2_to_eucjp(c, out);
    for (u32 i = 0; i < n; i++) m.w8(dst + i, out[i]);
    return m.ok() ? n : 0;
}

u32 UCS2toSJIS(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UCS2toSJIS");
    u8 out[2];
    const u32 n = ucs2_to_sjis(c, out);
    for (u32 i = 0; i < n; i++) m.w8(dst + i, out[i]);
    return m.ok() ? n : 0;
}

// UTF8toEUCJP and UTF8toSJIS(src, dst, *len): the UTF-8 bytes read; *len the bytes written.
template <class Enc>
u32 from_utf8_one(PPUContext& ctx, const char* fn, u32 src, u32 dst, u32 len, Enc enc)
{
    Mem m(ctx, fn);
    u32 c = 0;
    const u32 took = utf8_decode([&](u32 i) { return m.r8(src + i); }, &c, false);
    if (!m.ok() || !took) return 0;
    u8 out[4];
    const u32 n = enc(c, out);
    if (!n) return 0;
    for (u32 i = 0; i < n; i++) m.w8(dst + i, out[i]);
    m.w32(len, n);
    return m.ok() ? took : 0;
}

u32 UTF8toEUCJP(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    return from_utf8_one(ctx, "UTF8toEUCJP", src, dst, len, [](u32 c, u8* o) { return ucs2_to_eucjp(c, o); });
}

u32 UTF8toSJIS(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    return from_utf8_one(ctx, "UTF8toSJIS", src, dst, len, [](u32 c, u8* o) { return ucs2_to_sjis(c, o); });
}

// EUCJPtoSJIS and SJIStoEUCJP(src, dst, *len): the bytes read; *len the bytes written (one for a single-byte code).
u32 EUCJPtoSJIS(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    Mem m(ctx, "EUCJPtoSJIS");
    u32 v = 0;
    const u32 n = eucjp_to_sjis_code([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    if (v > 0xFF) {
        m.w8(dst, static_cast<u8>(v >> 8));
        m.w32(len, 2);
        m.w8(dst + 1, static_cast<u8>(v));
    } else {
        m.w32(len, 1);
        m.w8(dst, static_cast<u8>(v));
    }
    return m.ok() ? n : 0;
}

u32 SJIStoEUCJP(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    Mem m(ctx, "SJIStoEUCJP");
    u32 v = 0;
    const u32 n = sjis_to_eucjp_code([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    if (v > 0xFF) {
        m.w32(len, 2);
        m.w8(dst, static_cast<u8>(v >> 8));
        m.w8(dst + 1, static_cast<u8>(v));
    } else {
        m.w32(len, 1);
        m.w8(dst, static_cast<u8>(v));
    }
    return m.ok() ? n : 0;
}

// MSJIStoUCS2(code, dst) and MSJIStoUTF8(code, dst): 1 / the UTF-8 bytes, 0 for none.
u32 MSJIStoUCS2(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "MSJIStoUCS2");
    u16 v = 0;
    if (!msjis_ucs2(c, &v)) return 0;
    m.w16(dst, v);
    return m.ok() ? 1 : 0;
}

u32 MSJIStoUTF8(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "MSJIStoUTF8");
    u16 v = 0;
    if (!msjis_ucs2(c, &v)) return 0;
    u8 out[4];
    const u32 n = ucs2_to_utf8(v, out);
    for (u32 i = 0; i < n; i++) m.w8(dst + i, out[i]);
    return m.ok() ? n : 0;
}

// UCS2toMSJIS(c, dst): the cp932 code as a u16 (written for the user-defined area even when 0); 1 or 0.
u32 UCS2toMSJIS(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UCS2toMSJIS");
    u32 v = 0;
    const u32 r = ucs2_to_msjis(c, &v);
    const u32 c16 = c & 0xFFFF;
    if (c16 <= 0xFFFD && ((c16 - 0xE400) & 0xFFFF) <= 751) {
        m.w16(dst, static_cast<u16>(v));
        return m.ok() ? r : 0;
    }
    if (!r) return 0;
    m.w16(dst, static_cast<u16>(v));
    return m.ok() ? 1 : 0;
}

// UTF8toMSJIS(src, dst): the UTF-8 bytes read when the character has a cp932 code, else 0.
u32 UTF8toMSJIS(PPUContext& ctx, u32 src, u32 dst)
{
    Mem m(ctx, "UTF8toMSJIS");
    u32 c = 0;
    const u32 took = utf8_decode([&](u32 i) { return m.r8(src + i); }, &c, false);
    if (!m.ok() || !took) return 0;
    u32 v = 0;
    const u32 r = ucs2_to_msjis(c, &v);
    const u32 c16 = c & 0xFFFF;
    if (r || (c16 <= 0xFFFD && ((c16 - 0xE400) & 0xFFFF) <= 751)) m.w16(dst, static_cast<u16>(v));
    return m.ok() && r ? took : 0;
}

// The string forms of the multibyte Japanese codes: libl10n's loop, refusing a character read past *src_len.
template <class Dec, class Enc>
u32 mb_string(PPUContext& ctx, const char* fn, u32 src, u32 src_len, u32 dst, u32 dst_len, u32 out_unit, Dec dec,
    Enc enc)
{
    Mem m(ctx, fn);
    return string_loop(m, src, src_len, dst, dst_len, 1, out_unit, true, [&](u32 at) {
        Step s;
        u16 v = 0;
        const u32 took = dec([&](u32 i) { return m.r8(at + i); }, &v);
        if (!took) return s;
        if (!enc(v, s)) return s;
        s.took = took;
        return s;
    });
}

bool to_ucs2_unit(u16 v, Step& s)
{
    s.out[0] = v;
    s.n = 1;
    return true;
}

bool to_utf8_units(u16 v, Step& s)
{
    u8 out[4];
    s.n = ucs2_to_utf8(v, out);
    for (u32 k = 0; k < s.n; k++) s.out[k] = out[k];
    return s.n != 0;
}

auto eucjp_dec = [](auto&& at, u16* v) { return eucjp_decode(at, v); };
auto sjis_dec = [](auto&& at, u16* v) { return sjis_decode(at, v); };

u32 EUCJPstoUCS2s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return mb_string(ctx, "EUCJPstoUCS2s", src, sl, dst, dl, 2, eucjp_dec, to_ucs2_unit);
}
u32 EUCJPstoUTF8s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return mb_string(ctx, "EUCJPstoUTF8s", src, sl, dst, dl, 1, eucjp_dec, to_utf8_units);
}
u32 SJISstoUCS2s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return mb_string(ctx, "SJISstoUCS2s", src, sl, dst, dl, 2, sjis_dec, to_ucs2_unit);
}
u32 SJISstoUTF8s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return mb_string(ctx, "SJISstoUTF8s", src, sl, dst, dl, 1, sjis_dec, to_utf8_units);
}

// EUCJPstoSJISs and SJISstoEUCJPs: one or two bytes out per character.
template <class Conv>
u32 jp_recode(PPUContext& ctx, const char* fn, u32 src, u32 sl, u32 dst, u32 dl, Conv conv)
{
    Mem m(ctx, fn);
    return string_loop(m, src, sl, dst, dl, 1, 1, true, [&](u32 at) {
        Step s;
        u32 v = 0;
        const u32 took = conv([&](u32 i) { return m.r8(at + i); }, &v);
        if (!took) return s;
        if (v > 0xFF) {
            s.out[0] = (v >> 8) & 0xFF;
            s.out[1] = v & 0xFF;
            s.n = 2;
        } else {
            s.out[0] = v;
            s.n = 1;
        }
        s.took = took;
        return s;
    });
}

u32 EUCJPstoSJISs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return jp_recode(ctx, "EUCJPstoSJISs", src, sl, dst, dl, [](auto&& at, u32* v) { return eucjp_to_sjis_code(at, v); });
}
u32 SJISstoEUCJPs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return jp_recode(ctx, "SJISstoEUCJPs", src, sl, dst, dl, [](auto&& at, u32* v) { return sjis_to_eucjp_code(at, v); });
}

// UCS2stoEUCJPs, UCS2stoSJISs and the UTF-8 forms.
template <class Enc>
u32 ucs2_string(PPUContext& ctx, const char* fn, u32 src, u32 sl, u32 dst, u32 dl, Enc enc)
{
    Mem m(ctx, fn);
    return string_loop(m, src, sl, dst, dl, 2, 1, false, [&](u32 at) {
        Step s;
        u8 out[4];
        s.n = enc(m.r16(at), out);
        if (!s.n) return s;
        for (u32 k = 0; k < s.n; k++) s.out[k] = out[k];
        s.took = 1;
        return s;
    });
}

template <class Enc>
u32 utf8_string(PPUContext& ctx, const char* fn, u32 src, u32 sl, u32 dst, u32 dl, Enc enc)
{
    Mem m(ctx, fn);
    return string_loop(m, src, sl, dst, dl, 1, 1, true, [&](u32 at) {
        Step s;
        u32 c = 0;
        const u32 took = utf8_decode([&](u32 i) { return m.r8(at + i); }, &c, false);
        if (!took) return s;
        u8 out[4];
        s.n = enc(c, out);
        if (!s.n) return s;
        for (u32 k = 0; k < s.n; k++) s.out[k] = out[k];
        s.took = took;
        return s;
    });
}

// The kana a combining voiced (U+3099) or semi-voiced (U+309A) mark may join, in EUC-JP and Shift-JIS: the h-rows
// first (the ten that take the semi-voiced mark), then k-, s- and t-rows, then katakana U (U+3099 makes it VU).
constexpr u16 kDakutenEuc[] = {0xA4CF, 0xA4D2, 0xA4D5, 0xA4D8, 0xA4DB, 0xA5CF, 0xA5D2, 0xA5D5, 0xA5D8, 0xA5DB, 0xA4AB,
    0xA4AD, 0xA4AF, 0xA4B1, 0xA4B3, 0xA4B5, 0xA4B7, 0xA4B9, 0xA4BB, 0xA4BD, 0xA4BF, 0xA4C1, 0xA4C4, 0xA4C6, 0xA4C8,
    0xA5AB, 0xA5AD, 0xA5AF, 0xA5B1, 0xA5B3, 0xA5B5, 0xA5B7, 0xA5B9, 0xA5BB, 0xA5BD, 0xA5BF, 0xA5C1, 0xA5C4, 0xA5C6,
    0xA5C8, 0xA5A6};
constexpr u16 kDakutenSjis[] = {0x82CD, 0x82D0, 0x82D3, 0x82D6, 0x82D9, 0x836E, 0x8371, 0x8374, 0x8377, 0x837A, 0x82A9,
    0x82AB, 0x82AD, 0x82AF, 0x82B1, 0x82B3, 0x82B5, 0x82B7, 0x82B9, 0x82BB, 0x82BD, 0x82BF, 0x82C2, 0x82C4, 0x82C6,
    0x834A, 0x834C, 0x834E, 0x8350, 0x8352, 0x8354, 0x8356, 0x8358, 0x835A, 0x835C, 0x835E, 0x8360, 0x8363, 0x8365,
    0x8367, 0x8345};

s32 dakuten_index(const u16* list, u32 code)
{
    for (s32 i = 0; i < 41; i++)
        if (list[i] == code) return i;
    return -1;
}

// The string converters into EUC-JP and Shift-JIS (libl10n 0x9bfc and its kin): a combining voiced or semi-voiced
// mark after a kana that takes it replaces the two bytes last written with the voiced kana (and counts nothing). The
// kana is remembered until a mark comes, whatever is written in between, as libl10n does.
template <bool kSjis, bool kUtf8, class Enc>
u32 kana_string(PPUContext& ctx, const char* fn, u32 src, u32 src_len, u32 dst, u32 dst_len, Enc enc)
{
    Mem m(ctx, fn);
    const u16* list = kSjis ? kDakutenSjis : kDakutenEuc;
    if (!m.r32(src_len)) {
        m.w32(dst_len, 0);
        return m.ok() ? kOK : kIllegal;
    }
    u32 consumed = 0, count = 0;
    s32 state = -1;
    for (;;) {
        u32 c = 0, took = 1;
        if constexpr (kUtf8) {
            took = utf8_decode([&](u32 i) { return m.r8(src + consumed + i); }, &c, false);
            if (!took || m.r32(src_len) < consumed + took) {
                m.w32(src_len, m.r32(src_len) - consumed);
                m.w32(dst_len, count);
                return m.ok() ? kIllegal : kIllegal;
            }
        } else {
            c = m.r16(src + 2 * consumed);
        }
        if (!m.ok()) return kIllegal;
        u8 out[4];
        u32 n = enc(c, out);
        if (!n) {
            const u32 c16 = c & 0xFFFF;
            if (state < 0 || (c16 != 0x3099 && !(c16 == 0x309A && state <= 9))) {
                m.w32(src_len, m.r32(src_len) - consumed);
                m.w32(dst_len, count);
                return kIllegal;
            }
            if (!dst) {
                state = -1;
                consumed += took;
                if (m.r32(src_len) <= consumed) {
                    m.w32(dst_len, count);
                    return m.ok() ? kOK : kIllegal;
                }
                continue;
            }
            u32 v = list[state] + (c16 == 0x3099 ? 1 : 2);
            if (c16 == 0x3099 && list[state] == (kSjis ? 0x8345 : 0xA5A6)) v = kSjis ? 0x8394 : 0xA5F4;
            dst -= 2;
            out[0] = static_cast<u8>(v >> 8);
            out[1] = static_cast<u8>(v);
            n = 2;
            state = -1;
        } else {
            if (state < 0) {
                const u32 code = (u32{out[0]} << 8) | out[1];
                const bool kana = kSjis ? (((out[0] - 0x81) & 0xFF) <= 30 || ((out[0] - 0xE0) & 0xFF) <= 28) &&
                                              ((((code - 0x8340) & 0xFFFF) <= 86 && code != 0x837F) ||
                                                  ((code - 0x829F) & 0xFFFF) <= 82)
                                        : in_a1_fe(out[0]) &&
                                              (((code - 0xA5A1) & 0xFFFF) <= 85 || ((code - 0xA4A1) & 0xFFFF) <= 82);
                if (kana) state = dakuten_index(list, code);
            }
            count += n;
        }
        if (dst) {
            if (m.r32(dst_len) < count) {
                m.w32(src_len, m.r32(src_len) - consumed);
                m.w32(dst_len, count - n);
                return m.ok() ? kExhausted : kIllegal;
            }
            for (u32 k = 0; k < n; k++) m.w8(dst + k, out[k]);
            dst += n;
        }
        consumed += took;
        if (m.r32(src_len) <= consumed) {
            m.w32(dst_len, count);
            return m.ok() ? kOK : kIllegal;
        }
    }
}

u32 UCS2stoEUCJPs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return kana_string<false, false>(ctx, "UCS2stoEUCJPs", src, sl, dst, dl, ucs2_to_eucjp);
}
u32 UCS2stoSJISs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return kana_string<true, false>(ctx, "UCS2stoSJISs", src, sl, dst, dl, ucs2_to_sjis);
}
u32 UTF8stoEUCJPs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return kana_string<false, true>(ctx, "UTF8stoEUCJPs", src, sl, dst, dl, ucs2_to_eucjp);
}
u32 UTF8stoSJISs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return kana_string<true, true>(ctx, "UTF8stoSJISs", src, sl, dst, dl, ucs2_to_sjis);
}

// The cp932 string forms: source and destination codes as big-endian u16 (MSJIS) units.
u32 MSJISstoUCS2s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "MSJISstoUCS2s");
    return string_loop(m, src, sl, dst, dl, 2, 2, false, [&](u32 at) {
        Step s;
        u16 v = 0;
        if (!msjis_ucs2(m.r16(at), &v)) return s;
        s.out[0] = v;
        s.n = 1;
        s.took = 1;
        return s;
    });
}

u32 MSJISstoUTF8s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "MSJISstoUTF8s");
    return string_loop(m, src, sl, dst, dl, 2, 1, false, [&](u32 at) {
        Step s;
        u16 v = 0;
        if (!msjis_ucs2(m.r16(at), &v)) return s;
        u8 out[4];
        s.n = ucs2_to_utf8(v, out);
        for (u32 k = 0; k < s.n; k++) s.out[k] = out[k];
        if (s.n) s.took = 1;
        return s;
    });
}

u32 UCS2stoMSJISs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "UCS2stoMSJISs");
    return string_loop(m, src, sl, dst, dl, 2, 2, false, [&](u32 at) {
        Step s;
        u32 v = 0;
        if (!ucs2_to_msjis(m.r16(at), &v)) return s;
        s.out[0] = v;
        s.n = 1;
        s.took = 1;
        return s;
    });
}

u32 UTF8stoMSJISs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "UTF8stoMSJISs");
    return string_loop(m, src, sl, dst, dl, 1, 2, true, [&](u32 at) {
        Step s;
        u32 c = 0;
        const u32 took = utf8_decode([&](u32 i) { return m.r8(at + i); }, &c, false);
        if (!took) return s;
        u32 v = 0;
        if (!ucs2_to_msjis(c, &v)) return s;
        s.out[0] = v;
        s.n = 1;
        s.took = took;
        return s;
    });
}

// ------------------------------------------------------------------------------------------------- Japanese text

// The full-width forms of ASCII 20-7E and of the half-width katakana A1-DF as EUC-JP codes (libl10n's han2zen table,
// 158 entries; backslash is FULLWIDTH REVERSE SOLIDUS A1C0, as Sony's): its zen2han functions take the first entry of a
// code.
const std::array<u16, 158>& han2zen()
{
    static constexpr std::array<u16, 158> kTable = {
        0xA1A1, 0xA1AA, 0xA1C9, 0xA1F4, 0xA1F0, 0xA1F3, 0xA1F5, 0xA1C7, 0xA1CA, 0xA1CB, 0xA1F6, 0xA1DC, 0xA1A4, 0xA1DD,
        0xA1A5, 0xA1BF, 0xA3B0, 0xA3B1, 0xA3B2, 0xA3B3, 0xA3B4, 0xA3B5, 0xA3B6, 0xA3B7, 0xA3B8, 0xA3B9, 0xA1A7, 0xA1A8,
        0xA1E3, 0xA1E1, 0xA1E4, 0xA1A9, 0xA1F7, 0xA3C1, 0xA3C2, 0xA3C3, 0xA3C4, 0xA3C5, 0xA3C6, 0xA3C7, 0xA3C8, 0xA3C9,
        0xA3CA, 0xA3CB, 0xA3CC, 0xA3CD, 0xA3CE, 0xA3CF, 0xA3D0, 0xA3D1, 0xA3D2, 0xA3D3, 0xA3D4, 0xA3D5, 0xA3D6, 0xA3D7,
        0xA3D8, 0xA3D9, 0xA3DA, 0xA1CE, 0xA1C0, 0xA1CF, 0xA1B0, 0xA1B2, 0xA1C6, 0xA3E1, 0xA3E2, 0xA3E3, 0xA3E4, 0xA3E5,
        0xA3E6, 0xA3E7, 0xA3E8, 0xA3E9, 0xA3EA, 0xA3EB, 0xA3EC, 0xA3ED, 0xA3EE, 0xA3EF, 0xA3F0, 0xA3F1, 0xA3F2, 0xA3F3,
        0xA3F4, 0xA3F5, 0xA3F6, 0xA3F7, 0xA3F8, 0xA3F9, 0xA3FA, 0xA1D0, 0xA1C3, 0xA1D1, 0xA1B1,
        // A1-DF
        0xA1A3, 0xA1D6, 0xA1D7, 0xA1A2, 0xA1A6, 0xA5F2, 0xA5A1, 0xA5A3, 0xA5A5, 0xA5A7, 0xA5A9, 0xA5E3, 0xA5E5, 0xA5E7,
        0xA5C3, 0xA1BC, 0xA5A2, 0xA5A4, 0xA5A6, 0xA5A8, 0xA5AA, 0xA5AB, 0xA5AD, 0xA5AF, 0xA5B1, 0xA5B3, 0xA5B5, 0xA5B7,
        0xA5B9, 0xA5BB, 0xA5BD, 0xA5BF, 0xA5C1, 0xA5C4, 0xA5C6, 0xA5C8, 0xA5CA, 0xA5CB, 0xA5CC, 0xA5CD, 0xA5CE, 0xA5CF,
        0xA5D2, 0xA5D5, 0xA5D8, 0xA5DB, 0xA5DE, 0xA5DF, 0xA5E0, 0xA5E1, 0xA5E2, 0xA5E4, 0xA5E6, 0xA5E8, 0xA5E9, 0xA5EA,
        0xA5EB, 0xA5EC, 0xA5ED, 0xA5EF, 0xA5F3, 0xA1AB, 0xA1AC};
    return kTable;
}

u32 euc_han2zen(u32 c)
{
    const u32 c16 = c & 0xFFFF;
    if (c16 <= 0x1F || ((c - 0x7F) & 0xFFFF) <= 0x8E21 || c16 > 0x8EDF) return c16;
    const u32 lo = c & 0xFF;
    return han2zen()[(lo > 0xA0 ? lo - 34 : lo) - 32];
}

u32 euc_zen2han(u32 c)
{
    const auto& t = han2zen();
    for (u32 i = 0; i < t.size(); i++) {
        if (t[i] == c) return i + 32 <= 0x7E ? i + 32 : (i + 0x8E42) & 0xFFFF;
    }
    return c;
}

u32 sjis_han2zen(u32 c)
{
    const u32 c16 = c & 0xFFFF;
    if (c16 <= 0x1F || ((c16 - 0x7F) & 0xFFFF) <= 0x21 || c16 > 0xDF) return c16;
    return jis_to_sjis(han2zen()[(c16 > 0xA0 ? c16 - 34 : c16) - 32] & 0x7F7F);
}

u32 sjis_zen2han(u32 c)
{
    const u32 e = sjis_to_jis(c & 0xFFFF);
    if (!e) return c & 0xFFFF;
    const auto& t = han2zen();
    for (u32 i = 0; i < t.size(); i++) {
        if (t[i] == (e | 0x8080)) return i + 32 <= 0x7E ? i + 32 : i + 66;
    }
    return c & 0xFFFF;
}

u32 eucjphan2zen(PPUContext&, u32 c) { return euc_han2zen(c); }
u32 eucjpzen2han(PPUContext&, u32 c) { return euc_zen2han(c & 0xFFFFFFFF); }
u32 sjishan2zen(PPUContext&, u32 c) { return sjis_han2zen(c); }
u32 sjiszen2han(PPUContext&, u32 c) { return sjis_zen2han(c); }

// The symbols of JIS X 0208 rows 1 and 2 (isSjisKigou's table, Shift-JIS 8141-81FC; isEucJpKigou reads its row 2).
const std::array<u8, 188>& kigou_table()
{
    static const std::array<u8, 188> t = [] {
        std::array<u8, 188> f{};
        const auto& j = table(kJis0208);
        for (u32 i = 0; i < 188; i++) {
            const u32 code = 0x8141 + i;
            u32 row, cell;
            if ((code & 0xFF) == 0x7F || !sjis_split(code, &row, &cell)) continue;
            f[i] = row == 1 ? 1 : (j[(row - 1) * 94 + cell - 1] != 0xFFFF ? 1 : 0);
        }
        return f;
    }();
    return t;
}

u32 isEucJpKigou(PPUContext&, u32 c)
{
    const u32 c16 = c & 0xFFFF;
    if (((c16 - 0xA1A2) & 0xFFFF) <= 92) return 1;
    if (((c16 - 0xA2A1) & 0xFFFF) <= 93 && kigou_table()[c16 - 0xA2A1 + 94]) return 1;
    return ((c16 - 0xA8A1) & 0xFFFF) <= 31 ? 1 : 0;
}

u32 isSjisKigou(PPUContext&, u32 c)
{
    const u32 c16 = c & 0xFFFF;
    if (((c16 - 0x8141) & 0xFFFF) <= 187 && kigou_table()[c16 - 0x8141]) return 1;
    return ((c16 - 0x849F) & 0xFFFF) <= 31 ? 1 : 0;
}

// The voiced-mark joins of the text tools (0x16c8 EUC-JP, 0x1808 Shift-JIS): the voiced (or semi-voiced) kana of
// `prev` and the mark `code`, or 0.
u32 combine_euc(u32 prev, u32 code)
{
    if (((code - 0xA1AB) & 0xFFFF) > 1) return 0;
    u32 k;
    bool kata;
    if (((prev - 0xA4A1) & 0xFFFF) <= 82) {
        k = euc_zen2han((prev + 0x100) & 0xFFFF) & 0xFF;
        kata = false;
    } else if (((prev - 0xA5A1) & 0xFFFF) <= 85) {
        k = euc_zen2han(prev) & 0xFF;
        kata = true;
    } else {
        return 0;
    }
    if (((k - 0xB6) & 0xFFFF) <= 14 || ((k - 0xCA) & 0xFFFF) <= 4) {
        if (code == 0xA1AB) return (prev + 1) & 0xFFFF;
        return k > 0xC9 ? (prev + 2) & 0xFFFF : 0;
    }
    if (k == 0xB3 && kata && code == 0xA1AB) return 0xA5F4;
    return 0;
}

u32 combine_sjis(u32 prev, u32 code)
{
    if (((code - 0x814A) & 0xFFFF) > 1) return 0;
    u32 conv;
    bool kata;
    if (((prev - 0x829F) & 0xFFFF) <= 82) {
        kata = false;
        conv = ((prev - 0x829F) & 0xFFFF) <= 62 ? prev + 161 : ((prev - 0x82DE) & 0xFFFF) <= 19 ? prev + 162 : prev;
    } else if (((prev - 0x8340) & 0xFFFF) <= 86 && prev != 0x837F) {
        kata = true;
        conv = prev;
    } else {
        return 0;
    }
    const u32 k = sjis_zen2han(conv & 0xFFFF) & 0xFF;
    if (((k - 0xB6) & 0xFFFF) <= 14 || ((k - 0xCA) & 0xFFFF) <= 4) {
        if (code == 0x814A) return (prev + 1) & 0xFFFF;
        return k > 0xC9 ? (prev + 2) & 0xFFFF : 0;
    }
    if (k == 0xB3 && kata && code == 0x814A) return 0x8394;
    return 0;
}

// The text tools' loop: `next(at, total, &code, &took)` reads a character at `at` (false: an illegal or cut
// sequence); `join` (0 for none) joins a two-byte code with the one before it, overwriting its two bytes.
template <class Next>
u32 text_loop(Mem& m, u32 src, u32 src_len, u32 dst, u32 dst_len, u32 (*join)(u32, u32), Next next)
{
    u32 consumed = 0, count = 0, prev = 0;
    while (consumed < m.r32(src_len)) {
        if (!m.ok()) return kIllegal;
        u32 code = 0, took = 0;
        if (!next(src + consumed, m.r32(src_len) - consumed, &code, &took)) {
            m.w32(src_len, m.r32(src_len) - consumed);
            m.w32(dst_len, count);
            return m.ok() ? kIllegal : kIllegal;
        }
        u32 size = code > 0xFFFFFF ? 4 : code > 0xFFFF ? 3 : code > 0xFF ? 2 : 1;
        bool skip = false;
        if (size == 2 && join) {
            if (prev) {
                if (const u32 joined = join(prev, code)) {
                    prev = 0;
                    if (!dst) {
                        skip = true;
                    } else {
                        dst -= 2;
                        code = joined;
                        size = 0;
                    }
                } else {
                    prev = code;
                }
            } else {
                prev = code;
            }
        } else {
            prev = 0;
        }
        if (!skip) {
            count += size;
            if (dst) {
                if (m.r32(dst_len) < count) {
                    m.w32(src_len, m.r32(src_len) - consumed);
                    m.w32(dst_len, count - size);
                    return m.ok() ? kExhausted : kIllegal;
                }
                const u32 bytes = size ? size : 2;
                for (u32 k = 0; k < bytes; k++) m.w8(dst + k, static_cast<u8>(code >> (8 * (bytes - 1 - k))));
                dst += bytes;
            }
        }
        consumed += took;
    }
    m.w32(dst_len, count);
    return m.ok() ? kOK : kIllegal;
}

bool sjis_lead(u32 b) { return ((b - 0x81) & 0xFF) <= 30 || ((b - 0xE0) & 0xFF) <= 28; }
bool sjis_trail(u32 t) { return ((t - 0x40) & 0xFF) <= 188 && t != 0x7F; }

// EUC-JP characters for the text tools: 8F (with `three`), 8E and A1-FE pairs, the rest single bytes; `pair(c)` maps
// a two-byte code, `single(b)` a byte and `kana(c)` an 8Exx code.
template <class Pair, class Single, class Kana>
auto euc_reader(Mem& m, bool three, Pair pair, Single single, Kana kana)
{
    return [&m, three, pair, single, kana](u32 at, u32 left, u32* code, u32* took) {
        const u8 b = m.r8(at);
        if (three && b == 0x8F) {
            if (left <= 2) return false;
            const u8 b1 = m.r8(at + 1);
            if (!in_a1_fe(b1)) return false;
            const u8 b2 = m.r8(at + 2);
            if (!in_a1_fe(b2)) return false;
            *code = 0x8F0000 | (u32{b1} << 8) | b2;
            *took = 3;
            return true;
        }
        if (b == 0x8E) {
            if (left <= 1) return false;
            const u8 b1 = m.r8(at + 1);
            if (((b1 - 0xA1) & 0xFF) > 62) return false;
            *code = kana(0x8E00 | b1);
            *took = 2;
            return true;
        }
        if (in_a1_fe(b)) {
            if (left <= 1) return false;
            const u8 b1 = m.r8(at + 1);
            if (!in_a1_fe(b1)) return false;
            *code = pair((u32{b} << 8) | b1);
            *took = 2;
            return true;
        }
        *code = single(b);
        *took = 1;
        return true;
    };
}

template <class Pair, class Single>
auto sjis_reader(Mem& m, Pair pair, Single single)
{
    return [&m, pair, single](u32 at, u32 left, u32* code, u32* took) {
        const u8 b = m.r8(at);
        if (sjis_lead(b)) {
            if (left <= 1) return false;
            const u8 t = m.r8(at + 1);
            if (!sjis_trail(t)) return false;
            *code = pair((u32{b} << 8) | t);
            *took = 2;
            return true;
        }
        *code = single(b);
        *took = 1;
        return true;
    };
}

auto same = [](u32 c) { return c; };

// EucJpZen2Han's split of a voiced or semi-voiced katakana into its half-width kana and mark (A5F4 is 8EB3 8EDE).
u32 euc_zen2han_split(u32 c)
{
    u32 code = euc_zen2han(c) & 0xFFFF;
    if (((code - 0xA5AC) & 0xFFFF) <= 29 || ((code - 0xA5D0) & 0xFFFF) <= 12) {
        const u32 t = euc_zen2han(code - 1);
        if (t != code - 1) code = (t << 16) | 0x8EDE;
    }
    if (code <= 0xFFFF && ((code - 0xA5D1) & 0xFFFF) <= 12) {
        const u32 t = euc_zen2han(code - 2);
        if (t != code - 2) code = (t << 16) | 0x8EDF;
    }
    if (code == 0xA5F4) code = 0x8EB38EDEu;
    return code;
}

u32 sjis_zen2han_split(u32 c)
{
    const u32 code = sjis_zen2han(c) & 0xFFFF;
    u32 out = code;
    bool split = false;
    if (((code - 0x834B) & 0xFFFF) <= 29 || ((code - 0x836F) & 0xFFFF) <= 12) {
        const u32 t = sjis_zen2han(code - 1) & 0xFFFF;
        if (t != code - 1) {
            out = (t << 8) | 0xDE;
            split = true;
        }
    }
    if (!split && ((out - 0x8370) & 0xFFFF) <= 12) {
        const u32 t = sjis_zen2han(out - 2) & 0xFFFF;
        if (t != out - 2) out = (t << 8) | 0xDF;
    }
    if ((out & 0xFFFF) == 0x8394) out = 0xB3DE;
    return out;
}

u32 EucJpHan2Zen(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "EucJpHan2Zen");
    return text_loop(m, src, sl, dst, dl, combine_euc,
        euc_reader(m, true, same, [](u32 b) { return euc_han2zen(b); }, [](u32 c) { return euc_han2zen(c); }));
}

u32 EucJpZen2Han(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "EucJpZen2Han");
    return text_loop(m, src, sl, dst, dl, nullptr, euc_reader(m, true, euc_zen2han_split, same, same));
}

u32 SjisHan2Zen(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "SjisHan2Zen");
    return text_loop(m, src, sl, dst, dl, combine_sjis, sjis_reader(m, same, [](u32 b) { return sjis_han2zen(b); }));
}

u32 SjisZen2Han(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "SjisZen2Han");
    return text_loop(m, src, sl, dst, dl, nullptr, sjis_reader(m, sjis_zen2han_split, same));
}

u32 ToEucJpHira(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "ToEucJpHira");
    return text_loop(m, src, sl, dst, dl, combine_euc, euc_reader(m, false, [](u32 c) {
        if (((c - 0xA5A1) & 0xFFFF) <= 82) return c - 0x100;
        return c == 0xA5F4 ? 0xA4A6A1ABu : c;
    }, same, same));
}

u32 ToEucJpKata(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "ToEucJpKata");
    return text_loop(m, src, sl, dst, dl, combine_euc, euc_reader(m, false, [](u32 c) {
        return ((c - 0xA4A1) & 0xFFFF) <= 82 ? c + 0x100 : c;
    }, same, same));
}

u32 ToSjisHira(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "ToSjisHira");
    return text_loop(m, src, sl, dst, dl, combine_sjis, sjis_reader(m, [](u32 c) {
        if (((c - 0x8340) & 0xFFFF) <= 62) return c - 161;
        if (((c - 0x8380) & 0xFFFF) <= 19) return c - 162;
        return c == 0x8394 ? 0x82A4814Au : c;
    }, same));
}

u32 ToSjisKata(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "ToSjisKata");
    return text_loop(m, src, sl, dst, dl, combine_sjis, sjis_reader(m, [](u32 c) {
        if (((c - 0x829F) & 0xFFFF) <= 62) return c + 161;
        if (((c - 0x82DE) & 0xFFFF) <= 19) return c + 162;
        return c;
    }, same));
}

u32 ToEucJpLower(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "ToEucJpLower");
    return text_loop(m, src, sl, dst, dl, nullptr,
        euc_reader(m, false, [](u32 c) { return ((c - 0xA3C1) & 0xFFFF) <= 25 ? c + 32 : c; }, same, same));
}

u32 ToEucJpUpper(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "ToEucJpUpper");
    return text_loop(m, src, sl, dst, dl, nullptr,
        euc_reader(m, false, [](u32 c) { return ((c - 0xA3E1) & 0xFFFF) <= 25 ? c - 32 : c; }, same, same));
}

u32 ToSjisLower(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "ToSjisLower");
    return text_loop(m, src, sl, dst, dl, nullptr,
        sjis_reader(m, [](u32 c) { return ((c - 0x8260) & 0xFFFF) <= 25 ? c + 33 : c; }, same));
}

u32 ToSjisUpper(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, "ToSjisUpper");
    return text_loop(m, src, sl, dst, dl, nullptr,
        sjis_reader(m, [](u32 c) { return ((c - 0x8281) & 0xFFFF) <= 25 ? c - 33 : c; }, same));
}

// ------------------------------------------------------------------------------------------------- Korean

// KS X 1001 (EUC-KR's two-byte area, rows A1-AC, B0-C8, CA-FD): the UCS-2 value of a pair of A1-FE bytes, FFFF for
// none (libl10n 0x4bc8).
u16 ksx1001_ucs2(u32 b0, u32 b1)
{
    const u32 c = (b0 << 8) | b1;
    u32 index;
    if (b0 <= 0xAC) {
        index = b0 * 94 + b1 - 15295;
    } else if (((b0 - 0xB0) & 0xFF) <= 24) {
        index = b0 * 94 + b1 - 15295 - 282;
    } else {
        if (b0 <= 0xC9 || c > 0xFDFE) return 0xFFFF;
        index = b0 * 94 + b1 - 15295 - 282 - 94;
    }
    return table(kKsx1001)[index];
}

template <class Byte>
u32 euckr_decode(Byte&& at, u16* out)
{
    const u8 b0 = at(0);
    if (in_a1_fe(b0)) {
        const u8 b1 = at(1);
        if (in_a1_fe(b1)) {
            const u16 v = ksx1001_ucs2(b0, b1);
            if (v == 0xFFFF) return 0;
            *out = v;
            return 2;
        }
    }
    if (b0 & 0x80) {
        if ((b0 & 0xE0) != 0x80 || b0 == 0x8E || b0 == 0x8F) return 0;
    }
    *out = b0;
    return 1;
}

// UHCtoUCS2 (0xcfdc): KS X 1001 for a pair of A1-FE bytes, cp949's extension for leads 81-C6 with trails A-Z, a-z
// and 81-FE (no FFFF check there), a byte up to 80 as itself.
template <class Byte>
u32 uhc_decode(Byte&& at, u16* out)
{
    const auto alpha = [](u32 b) { return ((b - 0x41) & 0xFF) <= 25 || ((b - 0x61) & 0xFF) <= 25; };
    const auto trail_index = [](u32 b) { return b <= 0x5A ? b - 0x41 : b <= 0x7A ? b - 0x47 : b - 0x4D; };
    const u8 b0 = at(0);
    if (in_a1_fe(b0)) {
        const u8 b1 = at(1);
        if (in_a1_fe(b1)) {
            const u16 v = ksx1001_ucs2(b0, b1);
            if (v == 0xFFFF) return 0;
            *out = v;
            return 2;
        }
        if (!alpha(b1) && ((b1 - 0x81) & 0xFF) > 125) return 0;
        const u32 c = (u32{b0} << 8) | b1;
        if (((c - 0xA141) & 0xFFFF) > 0x2511) return 0;
        *out = table(kUhc2)[(b0 - 0xA1) * 84 + trail_index(b1)];
        return 2;
    }
    if (((b0 - 0x81) & 0xFF) <= 125) {
        const u8 b1 = at(1);
        if (!alpha(b1) && ((b1 - 0x81) & 0xFF) > 125) return 0;
        const u32 c = (u32{b0} << 8) | b1;
        if (((c - 0x8141) & 0xFFFF) > 0x1FBD) return 0;
        *out = table(kUhc1)[(b0 - 0x81) * 178 + trail_index(b1)];
        return 2;
    }
    if (b0 > 0x80) return 0;
    *out = b0;
    return 1;
}

// A KS X 1001 table index's EUC-KR code.
u32 ksx1001_code(u32 i)
{
    u32 e = ((i / 94) << 8) + (i % 94) + 0xA1A1;
    if ((e & 0xFFFF) > 0xACFF) e += 0x300;
    if ((e & 0xFFFF) > 0xC8FF) e += 0x100;
    return e & 0xFFFF;
}

u32 ucs2_to_euckr(u32 c, u8* out)
{
    c &= 0xFFFF;
    if (c > 0xFFFD) return 0;
    if (c <= 0x8D || ((c - 0x90) & 0xFFFF) <= 15) {
        out[0] = static_cast<u8>(c);
        return 1;
    }
    const auto& ks = table(kKsx1001);
    for (u32 i = 0; i < ks.size(); i++) {
        if (ks[i] == c) {
            const u32 e = ksx1001_code(i);
            out[0] = static_cast<u8>(e >> 8);
            out[1] = static_cast<u8>(e);
            return 2;
        }
    }
    return 0;
}

// UCS2toUHC (0xc760): bytes up to 80 as themselves, KS X 1001, then cp949's extension.
u32 ucs2_to_uhc(u32 c, u8* out)
{
    c &= 0xFFFF;
    if (c > 0xFFFD) return 0;
    if (c <= 0x80) {
        out[0] = static_cast<u8>(c);
        return 1;
    }
    const auto& ks = table(kKsx1001);
    for (u32 i = 0; i < ks.size(); i++) {
        if (ks[i] == c) {
            const u32 e = ksx1001_code(i);
            out[0] = static_cast<u8>(e >> 8);
            out[1] = static_cast<u8>(e);
            return 2;
        }
    }
    const auto ext = [&](const std::vector<u16>& t, u32 per_lead, u32 base) -> u32 {
        for (u32 i = 0; i < t.size(); i++) {
            if (t[i] == c) {
                u32 e = ((i / per_lead) << 8) + (i % per_lead) + base;
                if ((e & 0xFF) > 0x5A) e += 6;
                if ((e & 0xFF) > 0x7A) e += 6;
                out[0] = static_cast<u8>((e & 0xFFFF) >> 8);
                out[1] = static_cast<u8>(e);
                return 2;
            }
        }
        return 0;
    };
    if (const u32 n = ext(table(kUhc1), 178, 0x8141)) return n;
    return ext(table(kUhc2), 84, 0xA141);
}

// EUCKRtoUHC and UHCtoEUCKR (0x4cf0, 0xcf64): the same work, a KS X 1001 pair or a single byte copied.
template <class Byte>
u32 ks_copy(Byte&& at, u8* out)
{
    const u8 b0 = at(0);
    if (in_a1_fe(b0)) {
        const u8 b1 = at(1);
        if (in_a1_fe(b1)) {
            out[0] = b0;
            out[1] = b1;
            return 2;
        }
    }
    if (((b0 - 0x81) & 0xFF) <= 125) return 0;
    out[0] = b0;
    return 1;
}

u32 EUCKRtoUCS2(PPUContext& ctx, u32 src, u32 dst)
{
    Mem m(ctx, "EUCKRtoUCS2");
    u16 v = 0;
    const u32 n = euckr_decode([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    m.w16(dst, v);
    return m.ok() ? n : 0;
}

u32 UHCtoUCS2(PPUContext& ctx, u32 src, u32 dst)
{
    Mem m(ctx, "UHCtoUCS2");
    u16 v = 0;
    const u32 n = uhc_decode([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    m.w16(dst, v);
    return m.ok() ? n : 0;
}

template <class Dec>
u32 to_utf8_one(PPUContext& ctx, const char* fn, u32 src, u32 dst, u32 len, Dec dec)
{
    Mem m(ctx, fn);
    u16 v = 0;
    const u32 n = dec([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    u8 out[4];
    const u32 k = ucs2_to_utf8(v, out);
    for (u32 i = 0; i < k; i++) m.w8(dst + i, out[i]);
    m.w32(len, k);
    return m.ok() ? n : 0;
}

auto euckr_dec = [](auto&& at, u16* v) { return euckr_decode(at, v); };
auto uhc_dec = [](auto&& at, u16* v) { return uhc_decode(at, v); };

u32 EUCKRtoUTF8(PPUContext& ctx, u32 src, u32 dst, u32 len) { return to_utf8_one(ctx, "EUCKRtoUTF8", src, dst, len, euckr_dec); }
u32 UHCtoUTF8(PPUContext& ctx, u32 src, u32 dst, u32 len) { return to_utf8_one(ctx, "UHCtoUTF8", src, dst, len, uhc_dec); }

u32 UCS2toEUCKR(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UCS2toEUCKR");
    u8 out[2];
    const u32 n = ucs2_to_euckr(c, out);
    for (u32 i = 0; i < n; i++) m.w8(dst + i, out[i]);
    return m.ok() ? n : 0;
}

u32 UCS2toUHC(PPUContext& ctx, u32 c, u32 dst)
{
    Mem m(ctx, "UCS2toUHC");
    u8 out[2];
    const u32 n = ucs2_to_uhc(c, out);
    for (u32 i = 0; i < n; i++) m.w8(dst + i, out[i]);
    return m.ok() ? n : 0;
}

u32 UTF8toEUCKR(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    return from_utf8_one(ctx, "UTF8toEUCKR", src, dst, len, [](u32 c, u8* o) { return ucs2_to_euckr(c, o); });
}

u32 UTF8toUHC(PPUContext& ctx, u32 src, u32 dst, u32 len)
{
    return from_utf8_one(ctx, "UTF8toUHC", src, dst, len, [](u32 c, u8* o) { return ucs2_to_uhc(c, o); });
}

u32 ks_copy_one(PPUContext& ctx, const char* fn, u32 src, u32 dst, u32 len)
{
    Mem m(ctx, fn);
    u8 out[2];
    const u32 n = ks_copy([&](u32 i) { return m.r8(src + i); }, out);
    if (!m.ok() || !n) return 0;
    for (u32 i = 0; i < n; i++) m.w8(dst + i, out[i]);
    m.w32(len, n);
    return m.ok() ? n : 0;
}

u32 EUCKRtoUHC(PPUContext& ctx, u32 src, u32 dst, u32 len) { return ks_copy_one(ctx, "EUCKRtoUHC", src, dst, len); }
u32 UHCtoEUCKR(PPUContext& ctx, u32 src, u32 dst, u32 len) { return ks_copy_one(ctx, "UHCtoEUCKR", src, dst, len); }

u32 EUCKRstoUCS2s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return mb_string(ctx, "EUCKRstoUCS2s", src, sl, dst, dl, 2, euckr_dec, to_ucs2_unit);
}
u32 EUCKRstoUTF8s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return mb_string(ctx, "EUCKRstoUTF8s", src, sl, dst, dl, 1, euckr_dec, to_utf8_units);
}
u32 UHCstoUCS2s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return mb_string(ctx, "UHCstoUCS2s", src, sl, dst, dl, 2, uhc_dec, to_ucs2_unit);
}
u32 UHCstoUTF8s(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return mb_string(ctx, "UHCstoUTF8s", src, sl, dst, dl, 1, uhc_dec, to_utf8_units);
}
u32 UCS2stoEUCKRs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return ucs2_string(ctx, "UCS2stoEUCKRs", src, sl, dst, dl, ucs2_to_euckr);
}
u32 UCS2stoUHCs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return ucs2_string(ctx, "UCS2stoUHCs", src, sl, dst, dl, ucs2_to_uhc);
}
u32 UTF8stoEUCKRs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return utf8_string(ctx, "UTF8stoEUCKRs", src, sl, dst, dl, ucs2_to_euckr);
}
u32 UTF8stoUHCs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl)
{
    return utf8_string(ctx, "UTF8stoUHCs", src, sl, dst, dl, ucs2_to_uhc);
}

u32 ks_copy_string(PPUContext& ctx, const char* fn, u32 src, u32 sl, u32 dst, u32 dl)
{
    Mem m(ctx, fn);
    return string_loop(m, src, sl, dst, dl, 1, 1, true, [&](u32 at) {
        Step s;
        u8 out[2];
        s.n = ks_copy([&](u32 i) { return m.r8(at + i); }, out);
        if (!s.n) return s;
        for (u32 k = 0; k < s.n; k++) s.out[k] = out[k];
        s.took = s.n;
        return s;
    });
}

u32 EUCKRstoUHCs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl) { return ks_copy_string(ctx, "EUCKRstoUHCs", src, sl, dst, dl); }
u32 UHCstoEUCKRs(PPUContext& ctx, u32 src, u32 sl, u32 dst, u32 dl) { return ks_copy_string(ctx, "UHCstoEUCKRs", src, sl, dst, dl); }

// ------------------------------------------------------------------------------------------------- Chinese

// GB2312's rows A1-A9, B0-F7 (EUC-CN): the UCS-2 value of a pair of A1-FE bytes, FFFF for none (libl10n 0x3838).
u16 gb2312_ucs2(u32 b0, u32 b1)
{
    const u32 c = (b0 << 8) | b1;
    if (((c - 0xA1A1) & 0xFFFF) > 0x565D || ((b0 - 0xAA) & 0xFF) <= 5) return 0xFFFF;
    return table(kGb2312)[b0 * 94 + b1 - (b0 <= 0xAF ? 15295 : 15859)];
}

template <class Byte>
u32 euccn_decode(Byte&& at, u16* out)
{
    const u8 b0 = at(0);
    if (in_a1_fe(b0)) {
        const u8 b1 = at(1);
        if (in_a1_fe(b1)) {
            const u16 v = gb2312_ucs2(b0, b1);
            if (v == 0xFFFF) return 0;
            *out = v;
            return 2;
        }
    }
    if (b0 & 0x80) {
        if ((b0 & 0xE0) != 0x80 || b0 == 0x8E || b0 == 0x8F) return 0;
    }
    *out = b0;
    return 1;
}

// A GB2312 table index's code (rows A1-A9, then B0-F7).
u32 gb2312_code(u32 i)
{
    u32 e = ((i / 94) << 8) + (i % 94) + 0xA1A1;
    if ((e & 0xFFFF) > 0xA9FF) e += 0x600;
    return e & 0xFFFF;
}

bool find_code(const std::vector<u16>& t, u32 c, u32* index, usize from = 0, usize to = ~usize{0})
{
    for (usize i = from; i < t.size() && i < to; i++) {
        if (t[i] == c) {
            *index = static_cast<u32>(i);
            return true;
        }
    }
    return false;
}

u32 put2(u8* out, u32 e)
{
    out[0] = static_cast<u8>((e & 0xFFFF) >> 8);
    out[1] = static_cast<u8>(e);
    return 2;
}

u32 ucs2_to_euccn(u32 c, u8* out)
{
    c &= 0xFFFF;
    if (c > 0xFFFD) return 0;
    if (c <= 0x8D || ((c - 0x90) & 0xFFFF) <= 15) {
        out[0] = static_cast<u8>(c);
        return 1;
    }
    u32 i;
    if (find_code(table(kGb2312), c, &i)) return put2(out, gb2312_code(i));
    return 0;
}

// GBK's code of a GBK/3 index (leads 81-A0) and a GBK/4 index (leads A8-FE): trails 40-7E, 80-FE / 80-A0.
u32 gbk3_code(u32 i)
{
    u32 e = ((i / 190) << 8) + (i % 190) + 0x8140;
    if ((e & 0xFF) > 0x7E) e += 1;
    return e;
}

u32 gbk4_code(u32 i)
{
    u32 e = ((i / 96) << 8) + (i % 96) + 0xA840;
    if ((e & 0xFF) > 0x7E) e += 1;
    return e;
}

// GBKtoUCS2 (0x5ba8): 80 is the euro sign; the two-byte GBK/3 and GBK/4 cells are read without an FFFF check.
template <class Byte>
u32 gbk_decode(Byte&& at, u16* out)
{
    const u8 b0 = at(0);
    if (in_a1_fe(b0)) {
        const u8 b1 = at(1);
        if (in_a1_fe(b1)) {
            u16 v;
            if (b0 == 0xA2) v = table(kGbkrows)[b1 - 0xA1];
            else if (b0 == 0xA6) v = table(kGbkrows)[94 + b1 - 0xA1];
            else if (b0 == 0xA8) v = table(kGbkrows)[188 + b1 - 0xA1];
            else v = gb2312_ucs2(b0, b1);
            if (v == 0xFFFF) return 0;
            *out = v;
            return 2;
        }
    }
    if (((b0 - 0x81) & 0xFF) <= 125) {
        const u8 b1 = at(1);
        if (((b1 - 0x40) & 0xFF) > 190 || b1 == 0x7F) return 0;
        const u32 t = b1 < 0x80 ? b1 - 0x40 : b1 - 0x41;
        if (b0 <= 0xA0) {
            *out = table(kGbk3)[(b0 - 0x81) * 190 + t];
            return 2;
        }
        if (b0 <= 0xA7) return 0;
        const u32 index = (b0 - 0xA8) * 96 + t;
        if (index > 8271) return 0;
        const u16 v = table(kGbk4)[index];
        if (v == 0xFFFF) return 0;
        *out = v;
        return 2;
    }
    if (!(b0 & 0x80)) {
        *out = b0;
        return 1;
    }
    if (b0 != 0x80) return 0;
    *out = 0x20AC;
    return 1;
}

// UCS2toGBK (0xc07c): ASCII, the euro sign, then GBK's rows A2/A6/A8, GBK/3, GBK/4 and GB2312, in that order.
u32 ucs2_to_gbk(u32 c, u8* out)
{
    c &= 0xFFFF;
    if (c > 0xFFFD) return 0;
    if (c <= 0x7F) {
        out[0] = static_cast<u8>(c);
        return 1;
    }
    if (c == 0x20AC) {
        out[0] = 0x80;
        return 1;
    }
    u32 i;
    if (find_code(table(kGbkrows), c, &i)) {
        u32 e = ((i / 94) << 8) + (i % 94) + 0xA2A1;
        if ((e & 0xFFFF) > 0xA2FF) e += 0x300;
        if ((e & 0xFFFF) > 0xA6FF) e += 0x100;
        return put2(out, e);
    }
    if (find_code(table(kGbk3), c, &i)) return put2(out, gbk3_code(i));
    if (find_code(table(kGbk4), c, &i)) return put2(out, gbk4_code(i));
    if (find_code(table(kGb2312), c, &i)) return put2(out, gb2312_code(i));
    return 0;
}

// GB18030toUCS2 (0x5e34): GB2312's area with GB18030's rows A2, A4-A9 and D7, GBK's areas with GB18030's A8-A9 and FE
// cells, and the four-byte cells of CJK Extension A (no FFFF checks: every such cell is assigned).
template <class Byte>
u32 gb18030_decode(Byte&& at, u16* out)
{
    const u8 b0 = at(0);
    if (in_a1_fe(b0)) {
        const u8 b1 = at(1);
        if (in_a1_fe(b1)) {
            if (b0 == 0xA2) *out = table(kGbrows18030)[b1 - 0xA1];
            else if (((b0 - 0xA4) & 0xFF) <= 5) *out = table(kGbrows18030)[(b0 - 0xA3) * 94 + b1 - 0xA1];
            else if (b0 == 0xD7) *out = table(kGbd7)[b1 - 0xA1];
            else {
                const u32 c = (u32{b0} << 8) | b1;
                if (c - 0xA1A1 > 0x565D || ((b0 - 0xAA) & 0xFF) <= 5) return 0;
                *out = table(kGb2312)[b0 * 94 + b1 - (b0 <= 0xAF ? 15295 : 15859)];
            }
            return 2;
        }
    }
    if (((b0 - 0x81) & 0xFF) <= 125) {
        const u8 b1 = at(1);
        if (((b1 - 0x40) & 0xFF) <= 190 && b1 != 0x7F) {
            const u32 t = b1 < 0x80 ? b1 - 0x40 : b1 - 0x41;
            if (((b0 - 0xA8) & 0xFF) <= 1) *out = table(kGba8)[(b0 - 0xA8) * 96 + t];
            else if (b0 == 0xFE) *out = table(kGbfe)[t];
            else if (b0 <= 0xA0) *out = table(kGbk3)[(b0 - 0x81) * 190 + t];
            else if (b0 <= 0xA7) return 0;
            else *out = table(kGbk4)[(b0 - 0xA8) * 96 + t];
            return 2;
        }
        if (((b1 - 0x30) & 0xFF) > 9) return 0;
        const u8 b2 = at(2);
        if (((b2 - 0x81) & 0xFF) > 125) return 0;
        const u8 b3 = at(3);
        if (((b3 - 0x30) & 0xFF) > 9) return 0;
        const u32 v = (u32{b0} << 24) | (u32{b1} << 16) | (u32{b2} << 8) | b3;
        if (v - 0x8139EE39u > 0xFB98FFu) return 0;
        const u32 linear = ((((b0 - 0x81u) * 10 + (b1 - 0x30u)) * 126 + (b2 - 0x81u)) * 10 + (b3 - 0x30u)) & 0xFFFF;
        for (const auto& r : l10n_data::kGb18030Ranges) {
            if (linear >= r.linear && linear < r.linear + r.count) {
                *out = static_cast<u16>(linear - r.linear + r.ucs);
                return 4;
            }
        }
        return 0;
    }
    if (b0 & 0x80) return 0;
    *out = b0;
    return 1;
}

// UCS2toGB18030 (0xbc50): GB18030's rows, D7, A8-A9 and FE cells, GBK/3, GBK/4 (from lead AA), GB2312, then the
// four-byte cells.
u32 ucs2_to_gb18030(u32 c, u8* out)
{
    c &= 0xFFFF;
    if (c > 0xFFFD) return 0;
    if (c <= 0x7F) {
        out[0] = static_cast<u8>(c);
        return 1;
    }
    u32 i;
    if (find_code(table(kGbrows18030), c, &i)) {
        u32 e = ((i / 94) << 8) + (i % 94) + 0xA2A1;
        if ((e & 0xFFFF) > 0xA2FF) e += 0x100;
        return put2(out, e);
    }
    if (find_code(table(kGbd7), c, &i)) return put2(out, 0xD7A1 + i);
    if (find_code(table(kGba8), c, &i)) return put2(out, gbk4_code(i));
    if (find_code(table(kGbfe), c, &i)) {
        u32 e = 0xFE40 + i;
        if ((e & 0xFF) > 0x7E) e += 1;
        return put2(out, e);
    }
    if (find_code(table(kGbk3), c, &i)) return put2(out, gbk3_code(i));
    if (find_code(table(kGbk4), c, &i, 192, 8256)) return put2(out, gbk4_code(i));
    if (find_code(table(kGb2312), c, &i)) return put2(out, gb2312_code(i));
    for (const auto& r : l10n_data::kGb18030Ranges) {
        if (c >= r.ucs && c < r.ucs + r.count) {
            const u32 linear = r.linear + (c - r.ucs);
            const u32 q1 = linear / 10, t2 = q1 / 126;
            out[0] = static_cast<u8>(t2 / 10 + 0x81);
            out[1] = static_cast<u8>(t2 % 10 + 0x30);
            out[2] = static_cast<u8>(q1 % 126 + 0x81);
            out[3] = static_cast<u8>(linear % 10 + 0x30);
            return 4;
        }
    }
    return 0;
}

// Big5 (cp950 without ETEN's C6A1-C6FE; leads C7 and C8 have nothing): BIG5toUCS2 (0x336c), UCS2toBIG5 (0xb74c).
template <class Byte>
u32 big5_decode(Byte&& at, u16* out)
{
    const u8 b0 = at(0);
    if (in_a1_fe(b0)) {
        const u8 b1 = at(1);
        if (((b1 - 0x40) & 0xFF) > 62 && !in_a1_fe(b1)) return 0;
        const u32 c = (u32{b0} << 8) | b1;
        if (((c - 0xA140) & 0xFFFF) > 0x58BE || b0 == 0xC7 || b0 == 0xC8) return 0;
        u32 index = b0 * 157 + b1 - (b0 <= 0xC8 ? 25341 : 25655);
        if (b1 > 0xA0) index -= 34;
        const u16 v = table(kBig5)[index];
        if (v == 0xFFFF) return 0;
        *out = v;
        return 2;
    }
    if (b0 > 0x80) return 0;
    *out = b0;
    return 1;
}

u32 ucs2_to_big5(u32 c, u8* out)
{
    c &= 0xFFFF;
    if (c > 0xFFFD) return 0;
    if (c <= 0x80) {
        out[0] = static_cast<u8>(c);
        return 1;
    }
    u32 i;
    if (!find_code(table(kBig5), c, &i)) return 0;
    u32 e = ((i / 157) << 8) + (i % 157) + 0xA140;
    if ((e & 0xFFFF) > 0xC6FF) e += 0x200;
    if ((e & 0xFF) > 0x7E) e += 34;
    return put2(out, e);
}

auto euccn_dec = [](auto&& at, u16* v) { return euccn_decode(at, v); };
auto gbk_dec = [](auto&& at, u16* v) { return gbk_decode(at, v); };
auto gb18030_dec = [](auto&& at, u16* v) { return gb18030_decode(at, v); };
auto big5_dec = [](auto&& at, u16* v) { return big5_decode(at, v); };

template <class Dec>
u32 to_ucs2_one(PPUContext& ctx, const char* fn, u32 src, u32 dst, Dec dec)
{
    Mem m(ctx, fn);
    u16 v = 0;
    const u32 n = dec([&](u32 i) { return m.r8(src + i); }, &v);
    if (!m.ok() || !n) return 0;
    m.w16(dst, v);
    return m.ok() ? n : 0;
}

template <class Enc>
u32 from_ucs2_one(PPUContext& ctx, const char* fn, u32 c, u32 dst, Enc enc)
{
    Mem m(ctx, fn);
    u8 out[4];
    const u32 n = enc(c, out);
    for (u32 i = 0; i < n; i++) m.w8(dst + i, out[i]);
    return m.ok() ? n : 0;
}

u32 EUCCNtoUCS2(PPUContext& ctx, u32 s, u32 d) { return to_ucs2_one(ctx, "EUCCNtoUCS2", s, d, euccn_dec); }
u32 GBKtoUCS2(PPUContext& ctx, u32 s, u32 d) { return to_ucs2_one(ctx, "GBKtoUCS2", s, d, gbk_dec); }
u32 GB18030toUCS2(PPUContext& ctx, u32 s, u32 d) { return to_ucs2_one(ctx, "GB18030toUCS2", s, d, gb18030_dec); }
u32 BIG5toUCS2(PPUContext& ctx, u32 s, u32 d) { return to_ucs2_one(ctx, "BIG5toUCS2", s, d, big5_dec); }
u32 EUCCNtoUTF8(PPUContext& ctx, u32 s, u32 d, u32 l) { return to_utf8_one(ctx, "EUCCNtoUTF8", s, d, l, euccn_dec); }
u32 GBKtoUTF8(PPUContext& ctx, u32 s, u32 d, u32 l) { return to_utf8_one(ctx, "GBKtoUTF8", s, d, l, gbk_dec); }
u32 GB18030toUTF8(PPUContext& ctx, u32 s, u32 d, u32 l) { return to_utf8_one(ctx, "GB18030toUTF8", s, d, l, gb18030_dec); }
u32 BIG5toUTF8(PPUContext& ctx, u32 s, u32 d, u32 l) { return to_utf8_one(ctx, "BIG5toUTF8", s, d, l, big5_dec); }
u32 UCS2toEUCCN(PPUContext& ctx, u32 c, u32 d) { return from_ucs2_one(ctx, "UCS2toEUCCN", c, d, ucs2_to_euccn); }
u32 UCS2toGBK(PPUContext& ctx, u32 c, u32 d) { return from_ucs2_one(ctx, "UCS2toGBK", c, d, ucs2_to_gbk); }
u32 UCS2toGB18030(PPUContext& ctx, u32 c, u32 d) { return from_ucs2_one(ctx, "UCS2toGB18030", c, d, ucs2_to_gb18030); }
u32 UCS2toBIG5(PPUContext& ctx, u32 c, u32 d) { return from_ucs2_one(ctx, "UCS2toBIG5", c, d, ucs2_to_big5); }
u32 UTF8toEUCCN(PPUContext& ctx, u32 s, u32 d, u32 l)
{
    return from_utf8_one(ctx, "UTF8toEUCCN", s, d, l, [](u32 c, u8* o) { return ucs2_to_euccn(c, o); });
}
u32 UTF8toGBK(PPUContext& ctx, u32 s, u32 d, u32 l)
{
    return from_utf8_one(ctx, "UTF8toGBK", s, d, l, [](u32 c, u8* o) { return ucs2_to_gbk(c, o); });
}
u32 UTF8toGB18030(PPUContext& ctx, u32 s, u32 d, u32 l)
{
    return from_utf8_one(ctx, "UTF8toGB18030", s, d, l, [](u32 c, u8* o) { return ucs2_to_gb18030(c, o); });
}
u32 UTF8toBIG5(PPUContext& ctx, u32 s, u32 d, u32 l)
{
    return from_utf8_one(ctx, "UTF8toBIG5", s, d, l, [](u32 c, u8* o) { return ucs2_to_big5(c, o); });
}

u32 EUCCNstoUCS2s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return mb_string(ctx, "EUCCNstoUCS2s", s, sl, d, dl, 2, euccn_dec, to_ucs2_unit); }
u32 EUCCNstoUTF8s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return mb_string(ctx, "EUCCNstoUTF8s", s, sl, d, dl, 1, euccn_dec, to_utf8_units); }
u32 GBKstoUCS2s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return mb_string(ctx, "GBKstoUCS2s", s, sl, d, dl, 2, gbk_dec, to_ucs2_unit); }
u32 GBKstoUTF8s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return mb_string(ctx, "GBKstoUTF8s", s, sl, d, dl, 1, gbk_dec, to_utf8_units); }
u32 GB18030stoUCS2s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return mb_string(ctx, "GB18030stoUCS2s", s, sl, d, dl, 2, gb18030_dec, to_ucs2_unit); }
u32 GB18030stoUTF8s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return mb_string(ctx, "GB18030stoUTF8s", s, sl, d, dl, 1, gb18030_dec, to_utf8_units); }
u32 BIG5stoUCS2s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return mb_string(ctx, "BIG5stoUCS2s", s, sl, d, dl, 2, big5_dec, to_ucs2_unit); }
u32 BIG5stoUTF8s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return mb_string(ctx, "BIG5stoUTF8s", s, sl, d, dl, 1, big5_dec, to_utf8_units); }
u32 UCS2stoEUCCNs(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return ucs2_string(ctx, "UCS2stoEUCCNs", s, sl, d, dl, ucs2_to_euccn); }
u32 UCS2stoGBKs(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return ucs2_string(ctx, "UCS2stoGBKs", s, sl, d, dl, ucs2_to_gbk); }
u32 UCS2stoGB18030s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return ucs2_string(ctx, "UCS2stoGB18030s", s, sl, d, dl, ucs2_to_gb18030); }
u32 UCS2stoBIG5s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return ucs2_string(ctx, "UCS2stoBIG5s", s, sl, d, dl, ucs2_to_big5); }
u32 UTF8stoEUCCNs(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return utf8_string(ctx, "UTF8stoEUCCNs", s, sl, d, dl, ucs2_to_euccn); }
u32 UTF8stoGBKs(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return utf8_string(ctx, "UTF8stoGBKs", s, sl, d, dl, ucs2_to_gbk); }
u32 UTF8stoGB18030s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return utf8_string(ctx, "UTF8stoGB18030s", s, sl, d, dl, ucs2_to_gb18030); }
u32 UTF8stoBIG5s(PPUContext& ctx, u32 s, u32 sl, u32 d, u32 dl) { return utf8_string(ctx, "UTF8stoBIG5s", s, sl, d, dl, ucs2_to_big5); }

} // namespace

void register_cellL10n(Registry& r)
{
    using detail::thunk;
    const auto f = [](std::string_view name, HleFn fn) {
        return Function{{}, name, recomp::nid_of(name), fn, false};
    };
    r.add("cellL10n", {
        f("UTF8toUCS2", &thunk<&UTF8toUCS2>), f("UTF8toUTF16", &thunk<&UTF8toUTF16>),
        f("UTF8toUTF32", &thunk<&UTF8toUTF32>), f("UCS2toUTF8", &thunk<&UCS2toUTF8>),
        f("UCS2toUTF16", &thunk<&UCS2toUTF16>), f("UCS2toUTF32", &thunk<&UCS2toUTF32>),
        f("UTF16toUCS2", &thunk<&UTF16toUCS2>), f("UTF16toUTF32", &thunk<&UTF16toUTF32>),
        f("UTF16toUTF8", &thunk<&UTF16toUTF8>), f("UTF32toUCS2", &thunk<&UTF32toUCS2>),
        f("UTF32toUTF16", &thunk<&UTF32toUTF16>), f("UTF32toUTF8", &thunk<&UTF32toUTF8>),
        f("UTF8stoUCS2s", &thunk<&UTF8stoUCS2s>), f("UTF8stoUTF16s", &thunk<&UTF8stoUTF16s>),
        f("UTF8stoUTF32s", &thunk<&UTF8stoUTF32s>), f("UCS2stoUTF8s", &thunk<&UCS2stoUTF8s>),
        f("UCS2stoUTF16s", &thunk<&UCS2stoUTF16s>), f("UCS2stoUTF32s", &thunk<&UCS2stoUTF32s>),
        f("UTF16stoUCS2s", &thunk<&UTF16stoUCS2s>), f("UTF16stoUTF8s", &thunk<&UTF16stoUTF8s>),
        f("UTF16stoUTF32s", &thunk<&UTF16stoUTF32s>), f("UTF32stoUCS2s", &thunk<&UTF32stoUCS2s>),
        f("UTF32stoUTF16s", &thunk<&UTF32stoUTF16s>), f("UTF32stoUTF8s", &thunk<&UTF32stoUTF8s>),
        f("SBCStoUCS2", &thunk<&SBCStoUCS2>), f("SBCStoUTF8", &thunk<&SBCStoUTF8>),
        f("UCS2toSBCS", &thunk<&UCS2toSBCS>), f("UTF8toSBCS", &thunk<&UTF8toSBCS>),
        f("SBCSstoUCS2s", &thunk<&SBCSstoUCS2s>), f("SBCSstoUTF8s", &thunk<&SBCSstoUTF8s>),
        f("UCS2stoSBCSs", &thunk<&UCS2stoSBCSs>), f("UTF8stoSBCSs", &thunk<&UTF8stoSBCSs>),
        f("eucjp2jis", &thunk<&eucjp2jis>), f("eucjp2kuten", &thunk<&eucjp2kuten>),
        f("eucjp2sjis", &thunk<&eucjp2sjis>), f("jis2eucjp", &thunk<&jis2eucjp>), f("jis2kuten", &thunk<&jis2kuten>),
        f("jis2sjis", &thunk<&jis2sjis>), f("kuten2eucjp", &thunk<&kuten2eucjp>), f("kuten2jis", &thunk<&kuten2jis>),
        f("kuten2sjis", &thunk<&kuten2sjis>), f("sjis2eucjp", &thunk<&sjis2eucjp>), f("sjis2jis", &thunk<&sjis2jis>),
        f("sjis2kuten", &thunk<&sjis2kuten>), f("EUCJPtoUCS2", &thunk<&EUCJPtoUCS2>),
        f("SJIStoUCS2", &thunk<&SJIStoUCS2>), f("EUCJPtoUTF8", &thunk<&EUCJPtoUTF8>),
        f("SJIStoUTF8", &thunk<&SJIStoUTF8>), f("UCS2toEUCJP", &thunk<&UCS2toEUCJP>),
        f("UCS2toSJIS", &thunk<&UCS2toSJIS>), f("UTF8toEUCJP", &thunk<&UTF8toEUCJP>),
        f("UTF8toSJIS", &thunk<&UTF8toSJIS>), f("EUCJPtoSJIS", &thunk<&EUCJPtoSJIS>),
        f("SJIStoEUCJP", &thunk<&SJIStoEUCJP>), f("MSJIStoUCS2", &thunk<&MSJIStoUCS2>),
        f("MSJIStoUTF8", &thunk<&MSJIStoUTF8>), f("UCS2toMSJIS", &thunk<&UCS2toMSJIS>),
        f("UTF8toMSJIS", &thunk<&UTF8toMSJIS>), f("EUCJPstoUCS2s", &thunk<&EUCJPstoUCS2s>),
        f("EUCJPstoUTF8s", &thunk<&EUCJPstoUTF8s>), f("SJISstoUCS2s", &thunk<&SJISstoUCS2s>),
        f("SJISstoUTF8s", &thunk<&SJISstoUTF8s>), f("EUCJPstoSJISs", &thunk<&EUCJPstoSJISs>),
        f("SJISstoEUCJPs", &thunk<&SJISstoEUCJPs>), f("UCS2stoEUCJPs", &thunk<&UCS2stoEUCJPs>),
        f("UCS2stoSJISs", &thunk<&UCS2stoSJISs>), f("UTF8stoEUCJPs", &thunk<&UTF8stoEUCJPs>),
        f("UTF8stoSJISs", &thunk<&UTF8stoSJISs>), f("MSJISstoUCS2s", &thunk<&MSJISstoUCS2s>),
        f("MSJISstoUTF8s", &thunk<&MSJISstoUTF8s>), f("UCS2stoMSJISs", &thunk<&UCS2stoMSJISs>),
        f("UTF8stoMSJISs", &thunk<&UTF8stoMSJISs>), f("EUCKRtoUCS2", &thunk<&EUCKRtoUCS2>),
        f("eucjphan2zen", &thunk<&eucjphan2zen>), f("eucjpzen2han", &thunk<&eucjpzen2han>),
        f("sjishan2zen", &thunk<&sjishan2zen>), f("sjiszen2han", &thunk<&sjiszen2han>),
        f("isEucJpKigou", &thunk<&isEucJpKigou>), f("isSjisKigou", &thunk<&isSjisKigou>),
        f("EucJpHan2Zen", &thunk<&EucJpHan2Zen>), f("EucJpZen2Han", &thunk<&EucJpZen2Han>),
        f("SjisHan2Zen", &thunk<&SjisHan2Zen>), f("SjisZen2Han", &thunk<&SjisZen2Han>),
        f("ToEucJpHira", &thunk<&ToEucJpHira>), f("ToEucJpKata", &thunk<&ToEucJpKata>),
        f("ToSjisHira", &thunk<&ToSjisHira>), f("ToSjisKata", &thunk<&ToSjisKata>),
        f("ToEucJpLower", &thunk<&ToEucJpLower>), f("ToEucJpUpper", &thunk<&ToEucJpUpper>),
        f("ToSjisLower", &thunk<&ToSjisLower>), f("ToSjisUpper", &thunk<&ToSjisUpper>),
        f("UHCtoUCS2", &thunk<&UHCtoUCS2>), f("EUCKRtoUTF8", &thunk<&EUCKRtoUTF8>), f("UHCtoUTF8", &thunk<&UHCtoUTF8>),
        f("UCS2toEUCKR", &thunk<&UCS2toEUCKR>), f("UCS2toUHC", &thunk<&UCS2toUHC>),
        f("UTF8toEUCKR", &thunk<&UTF8toEUCKR>), f("UTF8toUHC", &thunk<&UTF8toUHC>),
        f("EUCKRtoUHC", &thunk<&EUCKRtoUHC>), f("UHCtoEUCKR", &thunk<&UHCtoEUCKR>),
        f("EUCKRstoUCS2s", &thunk<&EUCKRstoUCS2s>), f("EUCKRstoUTF8s", &thunk<&EUCKRstoUTF8s>),
        f("UHCstoUCS2s", &thunk<&UHCstoUCS2s>), f("UHCstoUTF8s", &thunk<&UHCstoUTF8s>),
        f("UCS2stoEUCKRs", &thunk<&UCS2stoEUCKRs>), f("UCS2stoUHCs", &thunk<&UCS2stoUHCs>),
        f("UTF8stoEUCKRs", &thunk<&UTF8stoEUCKRs>), f("UTF8stoUHCs", &thunk<&UTF8stoUHCs>),
        f("EUCKRstoUHCs", &thunk<&EUCKRstoUHCs>), f("UHCstoEUCKRs", &thunk<&UHCstoEUCKRs>),
        f("EUCCNtoUCS2", &thunk<&EUCCNtoUCS2>), f("EUCCNtoUTF8", &thunk<&EUCCNtoUTF8>),
        f("UCS2toEUCCN", &thunk<&UCS2toEUCCN>), f("UTF8toEUCCN", &thunk<&UTF8toEUCCN>),
        f("EUCCNstoUCS2s", &thunk<&EUCCNstoUCS2s>), f("EUCCNstoUTF8s", &thunk<&EUCCNstoUTF8s>),
        f("UCS2stoEUCCNs", &thunk<&UCS2stoEUCCNs>), f("UTF8stoEUCCNs", &thunk<&UTF8stoEUCCNs>),
        f("GBKtoUCS2", &thunk<&GBKtoUCS2>), f("GBKtoUTF8", &thunk<&GBKtoUTF8>), f("UCS2toGBK", &thunk<&UCS2toGBK>),
        f("UTF8toGBK", &thunk<&UTF8toGBK>), f("GBKstoUCS2s", &thunk<&GBKstoUCS2s>),
        f("GBKstoUTF8s", &thunk<&GBKstoUTF8s>), f("UCS2stoGBKs", &thunk<&UCS2stoGBKs>),
        f("UTF8stoGBKs", &thunk<&UTF8stoGBKs>), f("GB18030toUCS2", &thunk<&GB18030toUCS2>),
        f("GB18030toUTF8", &thunk<&GB18030toUTF8>), f("UCS2toGB18030", &thunk<&UCS2toGB18030>),
        f("UTF8toGB18030", &thunk<&UTF8toGB18030>), f("GB18030stoUCS2s", &thunk<&GB18030stoUCS2s>),
        f("GB18030stoUTF8s", &thunk<&GB18030stoUTF8s>), f("UCS2stoGB18030s", &thunk<&UCS2stoGB18030s>),
        f("UTF8stoGB18030s", &thunk<&UTF8stoGB18030s>), f("BIG5toUCS2", &thunk<&BIG5toUCS2>),
        f("BIG5toUTF8", &thunk<&BIG5toUTF8>), f("UCS2toBIG5", &thunk<&UCS2toBIG5>),
        f("UTF8toBIG5", &thunk<&UTF8toBIG5>), f("BIG5stoUCS2s", &thunk<&BIG5stoUCS2s>),
        f("BIG5stoUTF8s", &thunk<&BIG5stoUTF8s>), f("UCS2stoBIG5s", &thunk<&UCS2stoBIG5s>),
        f("UTF8stoBIG5s", &thunk<&UTF8stoBIG5s>),
    });
}

} // namespace pspocket::hle
