// cellL10n (runtime/hle/src/cellL10n.cpp, D-517) against Sony's libl10n (D-193): every function called with the same
// inputs on both, their answers and every byte they wrote compared. Needs PSPOCKET_TEST_DEV_FLASH (the user's
// installed firmware; skipped without it). Synthetic inputs only.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kOurs = kData + 0x10000, kTheirs = kData + 0x40000, kRegion = 0x400;

struct L10n {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    std::optional<FirmwareLibrary> sony;
    usize mismatches = 0;

    L10n()
    {
        if (const auto flash = test_dev_flash()) sony.emplace(f, t, *flash, "cellL10n");
    }

    u32 ours(std::string_view name, const std::vector<u64>& args)
    {
        const Function* fn = registry().find("cellL10n", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        for (usize i = 0; i < args.size(); i++) t.ctx.gpr[3 + i] = args[i];
        fn->fn(t.ctx, b);
        return static_cast<u32>(t.ctx.gpr[3]);
    }

    u32 theirs(std::string_view name, const std::vector<u64>& args)
    {
        const auto it = sony->module->functions.find({std::string("cellL10n"), recomp::nid_of(name)});
        REQUIRE(it != sony->module->functions.end());
        u64 a[8] = {};
        for (usize i = 0; i < args.size(); i++) a[i] = args[i];
        return static_cast<u32>(call_guest(t.ctx, it->second, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
    }

    // `name` on both, `setup(base)` writing its inputs in a cleared region at base and giving its arguments; the
    // answers and the regions compared. The first few differences are reported.
    void same(std::string_view name, const std::function<std::vector<u64>(u32)>& setup, const std::string& what)
    {
        std::memset(b + kOurs, 0xA5, kRegion);
        std::memset(b + kTheirs, 0xA5, kRegion);
        const std::vector<u64> a = setup(kOurs), c = setup(kTheirs);
        const u32 r1 = ours(name, a), r2 = theirs(name, c);
        const bool region = std::memcmp(b + kOurs, b + kTheirs, kRegion) == 0;
        if (r1 != r2 || !region) {
            if (++mismatches <= 12) {
                usize at = 0;
                while (at < kRegion && b[kOurs + at] == b[kTheirs + at]) at++;
                UNSCOPED_INFO(std::format("{} {}: ours {:#x}, Sony's {:#x}; first differing byte +{:#x} ({:#04x} vs "
                                          "{:#04x})",
                    name, what, r1, r2, at, at < kRegion ? b[kOurs + at] : 0, at < kRegion ? b[kTheirs + at] : 0));
                CHECK(false);
            }
        }
    }
};

void put_bytes(u8* b, u32 at, const std::vector<u8>& v) { std::memcpy(b + at, v.data(), v.size()); }

std::string hex(const std::vector<u8>& v)
{
    std::string s;
    for (const u8 x : v) s += std::format("{:02x}", x);
    return s;
}

// Random byte strings biased towards the interesting bytes of each encoding.
std::vector<u8> random_bytes(std::mt19937& rng, usize n)
{
    static constexpr u8 kPicks[] = {0x00, 0x41, 0x7F, 0x80, 0x8E, 0x8F, 0xA1, 0xA4, 0xB0, 0xBF, 0xC0, 0xC1, 0xC2,
        0xDF, 0xE0, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xF8, 0xFE, 0xFF, 0x81, 0x9F, 0x40, 0xFC, 0x21, 0x7E, 0x1B, 0x24};
    std::vector<u8> v(n);
    for (u8& x : v) x = (rng() & 1) ? kPicks[rng() % std::size(kPicks)] : static_cast<u8>(rng());
    return v;
}

} // namespace

TEST_CASE("cellL10n: the Unicode converters are libl10n's (D-517)", "[hle][l10n]")
{
    L10n l;
    if (!l.sony) SKIP("PSPOCKET_TEST_DEV_FLASH is not set");
    u8* b = l.b;
    // The character functions over every UCS-2 value, and the UTF-32 boundaries.
    for (u32 c = 0; c < 0x10000; c++) {
        for (const char* fn : {"UCS2toUTF8", "UCS2toUTF16", "UCS2toUTF32"})
            l.same(fn, [&](u32 base) { return std::vector<u64>{c, base}; }, std::format("{:#x}", c));
        l.same("UTF16toUCS2", [&](u32 base) { ps_write16(b, base + 0x100, static_cast<u16>(c)); return std::vector<u64>{base + 0x100, base}; }, std::format("{:#x}", c));
        l.same("UTF32toUCS2", [&](u32 base) { (void)base; return std::vector<u64>{c, base}; }, std::format("{:#x}", c));
    }
    for (const u32 c : {0x0u, 0x7Fu, 0x80u, 0x7FFu, 0x800u, 0xD7FFu, 0xD800u, 0xDFFFu, 0xE000u, 0xFFFFu, 0x10000u,
             0x10FFFFu, 0x110000u, 0xFFFFFFFFu, 0x1D800u, 0x12345u})
        for (const char* fn : {"UTF32toUTF8", "UTF32toUTF16", "UTF32toUCS2", "UCS2toUTF32", "UCS2toUTF8"})
            l.same(fn, [&](u32 base) { return std::vector<u64>{c, base}; }, std::format("{:#x}", c));
    // UTF-16 pairs and lone surrogates.
    for (const u32 hi : {0x0041u, 0xD800u, 0xDBFFu, 0xDC00u, 0xDFFFu, 0xD83Du})
        for (const u32 lo : {0x0041u, 0xD800u, 0xDC00u, 0xDE00u, 0xDFFFu})
            for (const char* fn : {"UTF16toUTF32", "UTF16toUTF8", "UTF16toUCS2"})
                l.same(fn, [&](u32 base) {
                    ps_write16(b, base + 0x100, static_cast<u16>(hi));
                    ps_write16(b, base + 0x102, static_cast<u16>(lo));
                    return std::vector<u64>{base + 0x100, base, base + 0x80};
                }, std::format("{:#x} {:#x}", hi, lo));
    // UTF-8 sequences: every lead byte with continuation patterns.
    std::mt19937 rng(1234);
    for (u32 lead = 0; lead < 256; lead++) {
        for (u32 k = 0; k < 24; k++) {
            std::vector<u8> s = random_bytes(rng, 4);
            s[0] = static_cast<u8>(lead);
            if (k & 1) for (usize i = 1; i < 4; i++) s[i] = static_cast<u8>(0x80 | (s[i] & 0x3F));
            for (const char* fn : {"UTF8toUCS2", "UTF8toUTF16", "UTF8toUTF32"})
                l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base, base + 0x80}; }, hex(s));
        }
    }
    // The string forms: random strings, every source length, destination sizes around the result's.
    const auto strings = [&](const char* fn, u32 in_unit, const std::function<std::vector<u8>()>& make) {
        for (u32 round = 0; round < 300; round++) {
            const std::vector<u8> s = make();
            const u32 units = static_cast<u32>(s.size() / in_unit);
            for (const u32 n : {units, units / 2, 1u, 0u}) {
                for (const u32 cap : {0x100u, 0u, 1u, 2u, 3u, 5u, 8u}) {
                    for (const bool null_dst : {false, true}) {
                        l.same(fn, [&](u32 base) {
                            put_bytes(b, base + 0x200, s);
                            ps_write32(b, base + 0x3F0, n);
                            ps_write32(b, base + 0x3F4, cap);
                            return std::vector<u64>{base + 0x200, base + 0x3F0, null_dst ? 0u : base, base + 0x3F4};
                        }, std::format("{} n {} cap {}{}", hex(s), n, cap, null_dst ? " NULL" : ""));
                    }
                }
            }
        }
    };
    const auto utf8_text = [&] {
        std::vector<u8> s;
        const u32 chars = 1 + rng() % 8;
        for (u32 i = 0; i < chars; i++) {
            u8 out[4];
            u32 c = rng() % 5 == 0 ? 0xD800 + rng() % 0x800 : (rng() % 3 ? rng() % 0x800 : rng() % 0x110000);
            const u32 n = c > 0xFFFF && c <= 0x10FFFF ? 4 : 0;
            if (n == 4) {
                out[0] = static_cast<u8>(0xF0 | (c >> 18));
                out[1] = static_cast<u8>(0x80 | ((c >> 12) & 0x3F));
                out[2] = static_cast<u8>(0x80 | ((c >> 6) & 0x3F));
                out[3] = static_cast<u8>(0x80 | (c & 0x3F));
                s.insert(s.end(), out, out + 4);
            } else if (rng() % 4 == 0) {
                const auto r = random_bytes(rng, 1 + rng() % 3);
                s.insert(s.end(), r.begin(), r.end());
            } else {
                c &= 0xFFFF;
                if (c < 0x80) s.push_back(static_cast<u8>(c));
                else if (c < 0x800) { s.push_back(static_cast<u8>(0xC0 | (c >> 6))); s.push_back(static_cast<u8>(0x80 | (c & 0x3F))); }
                else { s.push_back(static_cast<u8>(0xE0 | (c >> 12))); s.push_back(static_cast<u8>(0x80 | ((c >> 6) & 0x3F))); s.push_back(static_cast<u8>(0x80 | (c & 0x3F))); }
            }
        }
        return s;
    };
    const auto units = [&](u32 unit) {
        return [&rng, unit] {
            std::vector<u8> s;
            const u32 n = 1 + rng() % 8;
            for (u32 i = 0; i < n; i++) {
                u32 v = rng() % 4 == 0 ? 0xD800 + rng() % 0x800 : (rng() % 2 ? rng() % 0x80 : rng() % 0x10000);
                if (unit == 4 && rng() % 4 == 0) v = rng() % 0x120000;
                for (u32 k = 0; k < unit; k++) s.push_back(static_cast<u8>(v >> (8 * (unit - 1 - k))));
            }
            return s;
        };
    };
    strings("UTF8stoUCS2s", 1, utf8_text);
    strings("UTF8stoUTF16s", 1, utf8_text);
    strings("UTF8stoUTF32s", 1, utf8_text);
    for (const char* fn : {"UCS2stoUTF8s", "UCS2stoUTF16s", "UCS2stoUTF32s", "UTF16stoUCS2s", "UTF16stoUTF8s", "UTF16stoUTF32s"})
        strings(fn, 2, units(2));
    for (const char* fn : {"UTF32stoUCS2s", "UTF32stoUTF16s", "UTF32stoUTF8s"}) strings(fn, 4, units(4));
    CHECK(l.mismatches == 0);
}

TEST_CASE("cellL10n: the single-byte code pages are libl10n's (D-517)", "[hle][l10n]")
{
    L10n l;
    if (!l.sony) SKIP("PSPOCKET_TEST_DEV_FLASH is not set");
    u8* b = l.b;
    std::mt19937 rng(77);
    for (u32 code = 0; code < 54; code++) {
        for (u32 c = 0; c < 256; c++) {
            for (const char* fn : {"SBCStoUCS2", "SBCStoUTF8"})
                l.same(fn, [&](u32 base) { return std::vector<u64>{c, base, code}; }, std::format("{} {:#x}", code, c));
        }
        std::vector<u32> values;
        for (u32 c = 0; c < 0x500; c++) values.push_back(c);
        for (u32 k = 0; k < 600; k++) values.push_back(rng() % 0x10000);
        for (const u32 c : {0x20ACu, 0x2122u, 0x2500u, 0x25A0u, 0xFFFDu, 0xFFFEu, 0xFFFFu, 0x10041u})
            values.push_back(c);
        for (const u32 c : values)
            l.same("UCS2toSBCS", [&](u32 base) { return std::vector<u64>{c, base, code}; }, std::format("{} {:#x}", code, c));
        for (u32 k = 0; k < 200; k++) {
            std::vector<u8> s = random_bytes(rng, 4);
            if (k & 1) {
                const u32 c = 0x80 + rng() % 0x780;
                s[0] = static_cast<u8>(0xC0 | (c >> 6));
                s[1] = static_cast<u8>(0x80 | (c & 0x3F));
            }
            l.same("UTF8toSBCS", [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base, code}; }, std::format("{} {}", code, hex(s)));
        }
        // The string forms.
        for (u32 round = 0; round < 40; round++) {
            const std::vector<u8> s = random_bytes(rng, 1 + rng() % 12);
            std::vector<u8> ucs2;
            for (u32 i = 0; i < 1 + rng() % 8; i++) {
                const u32 v = rng() % 3 ? 0x80 + rng() % 0x480 : rng() % 0x10000;
                ucs2.push_back(static_cast<u8>(v >> 8));
                ucs2.push_back(static_cast<u8>(v));
            }
            for (const u32 cap : {0x100u, 0u, 1u, 3u}) {
                for (const char* fn : {"SBCSstoUCS2s", "SBCSstoUTF8s", "UTF8stoSBCSs", "UCS2stoSBCSs"}) {
                    const bool wide = std::string_view(fn) == "UCS2stoSBCSs";
                    const std::vector<u8>& in = wide ? ucs2 : s;
                    const u32 n = static_cast<u32>(wide ? in.size() / 2 : in.size());
                    l.same(fn, [&](u32 base) {
                        put_bytes(b, base + 0x200, in);
                        ps_write32(b, base + 0x3F0, n);
                        ps_write32(b, base + 0x3F4, cap);
                        return std::vector<u64>{base + 0x200, base + 0x3F0, base, base + 0x3F4, code};
                    }, std::format("{} {} cap {}", code, hex(in), cap));
                }
            }
        }
    }
    CHECK(l.mismatches == 0);
}

TEST_CASE("cellL10n: the Japanese converters are libl10n's (D-517)", "[hle][l10n]")
{
    L10n l;
    if (!l.sony) SKIP("PSPOCKET_TEST_DEV_FLASH is not set");
    u8* b = l.b;
    std::mt19937 rng(9);
    // The numeric helpers over every 16-bit value.
    for (u32 c = 0; c < 0x10000; c++)
        for (const char* fn : {"eucjp2jis", "eucjp2kuten", "eucjp2sjis", "jis2eucjp", "jis2kuten", "jis2sjis",
                 "kuten2eucjp", "kuten2jis", "kuten2sjis", "sjis2eucjp", "sjis2jis", "sjis2kuten"})
            l.same(fn, [&](u32) { return std::vector<u64>{c}; }, std::format("{:#x}", c));
    // The decoders over every two-byte sequence and every 8F sequence.
    const auto decode = [&](std::vector<u8> s) {
        for (const char* fn : {"EUCJPtoUCS2", "SJIStoUCS2"})
            l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base}; }, hex(s));
        for (const char* fn : {"EUCJPtoUTF8", "SJIStoUTF8", "EUCJPtoSJIS", "SJIStoEUCJP"})
            l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base, base + 0x80}; }, hex(s));
    };
    for (u32 b0 = 0; b0 < 256; b0++)
        for (u32 b1 = 0; b1 < 256; b1++) decode({static_cast<u8>(b0), static_cast<u8>(b1), 0xA1});
    for (u32 b1 = 0xA0; b1 < 0x100; b1++)
        for (u32 b2 = 0xA0; b2 < 0x100; b2++) decode({0x8F, static_cast<u8>(b1), static_cast<u8>(b2)});
    for (u32 c = 0; c < 0x10000; c++) {
        l.same("MSJIStoUCS2", [&](u32 base) { return std::vector<u64>{c, base}; }, std::format("{:#x}", c));
        l.same("MSJIStoUTF8", [&](u32 base) { return std::vector<u64>{c, base}; }, std::format("{:#x}", c));
    }
    // The encoders: every UCS-2 value below 0x3000 and in the kana, the CJK block sampled, the private use area.
    std::vector<u32> values;
    for (u32 c = 0; c < 0x3100; c++) values.push_back(c);
    for (u32 c = 0xE000; c < 0xE800; c++) values.push_back(c);
    for (u32 c = 0xF900; c < 0x10000; c++) values.push_back(c);
    for (u32 k = 0; k < 3000; k++) values.push_back(0x4E00 + rng() % 0x5200);
    for (const u32 c : values) {
        for (const char* fn : {"UCS2toEUCJP", "UCS2toSJIS", "UCS2toMSJIS"})
            l.same(fn, [&](u32 base) { return std::vector<u64>{c, base}; }, std::format("{:#x}", c));
    }
    for (u32 k = 0; k < 2000; k++) {
        const u32 c = values[rng() % values.size()];
        u8 u[4];
        u32 n = 1;
        if (c < 0x80) u[0] = static_cast<u8>(c);
        else if (c < 0x800) { u[0] = static_cast<u8>(0xC0 | (c >> 6)); u[1] = static_cast<u8>(0x80 | (c & 0x3F)); n = 2; }
        else { u[0] = static_cast<u8>(0xE0 | (c >> 12)); u[1] = static_cast<u8>(0x80 | ((c >> 6) & 0x3F)); u[2] = static_cast<u8>(0x80 | (c & 0x3F)); n = 3; }
        const std::vector<u8> s(u, u + n);
        for (const char* fn : {"UTF8toEUCJP", "UTF8toSJIS"})
            l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base, base + 0x80}; }, hex(s));
        l.same("UTF8toMSJIS", [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base}; }, hex(s));
    }
    // The string forms.
    const auto text = [&](u32 kind) {
        std::vector<u8> s;
        const u32 n = 1 + rng() % 10;
        for (u32 i = 0; i < n; i++) {
            const u32 r = rng() % 8;
            if (kind == 2) { // UCS-2 / MSJIS units
                const u32 v = r < 3 ? values[rng() % values.size()] : (r < 6 ? 0x8140 + rng() % 0x7000 : rng() % 0x10000);
                s.push_back(static_cast<u8>(v >> 8));
                s.push_back(static_cast<u8>(v));
            } else if (r < 2) s.push_back(static_cast<u8>(0x20 + rng() % 0x5F));
            else if (r < 5) { s.push_back(static_cast<u8>(0xA1 + rng() % 0x5E)); s.push_back(static_cast<u8>(0xA1 + rng() % 0x5E)); }
            else if (r < 7) { s.push_back(static_cast<u8>(0x81 + rng() % 0x1F)); s.push_back(static_cast<u8>(0x40 + rng() % 0xBD)); }
            else { const auto x = random_bytes(rng, 1 + rng() % 3); s.insert(s.end(), x.begin(), x.end()); }
        }
        return s;
    };
    for (u32 round = 0; round < 400; round++) {
        const std::vector<u8> bytes = text(1), units = text(2);
        for (const char* fn : {"EUCJPstoUCS2s", "EUCJPstoUTF8s", "SJISstoUCS2s", "SJISstoUTF8s", "EUCJPstoSJISs",
                 "SJISstoEUCJPs", "UTF8stoEUCJPs", "UTF8stoSJISs", "UTF8stoMSJISs", "UCS2stoEUCJPs", "UCS2stoSJISs",
                 "UCS2stoMSJISs", "MSJISstoUCS2s", "MSJISstoUTF8s"}) {
            const std::string name(fn);
            const bool wide = name.starts_with("UCS2s") || name.starts_with("MSJISs");
            const std::vector<u8>& in = wide ? units : bytes;
            const u32 n = static_cast<u32>(wide ? in.size() / 2 : in.size());
            for (const u32 sl : {n, n > 1 ? n - 1 : n})
                for (const u32 cap : {0x100u, 0u, 1u, 2u, 3u})
                    l.same(fn, [&](u32 base) {
                        put_bytes(b, base + 0x200, in);
                        ps_write32(b, base + 0x3F0, sl);
                        ps_write32(b, base + 0x3F4, cap);
                        return std::vector<u64>{base + 0x200, base + 0x3F0, base, base + 0x3F4};
                    }, std::format("{} n {} cap {}", hex(in), sl, cap));
        }
    }
    CHECK(l.mismatches == 0);
}

TEST_CASE("cellL10n: the Korean converters are libl10n's (D-517)", "[hle][l10n]")
{
    L10n l;
    if (!l.sony) SKIP("PSPOCKET_TEST_DEV_FLASH is not set");
    u8* b = l.b;
    std::mt19937 rng(21);
    for (u32 b0 = 0; b0 < 256; b0++) {
        for (u32 b1 = 0; b1 < 256; b1++) {
            const std::vector<u8> s{static_cast<u8>(b0), static_cast<u8>(b1), 0xA1};
            for (const char* fn : {"EUCKRtoUCS2", "UHCtoUCS2"})
                l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base}; }, hex(s));
            for (const char* fn : {"EUCKRtoUTF8", "UHCtoUTF8", "EUCKRtoUHC", "UHCtoEUCKR"})
                l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base, base + 0x80}; }, hex(s));
        }
    }
    std::vector<u32> values;
    for (u32 c = 0; c < 0x2700; c++) values.push_back(c);
    for (u32 c = 0x3000; c < 0x3400; c++) values.push_back(c);
    for (u32 c = 0xF900; c < 0x10000; c++) values.push_back(c);
    for (u32 k = 0; k < 2500; k++) values.push_back(0xAC00 + rng() % 0x2BA4);
    for (u32 k = 0; k < 1500; k++) values.push_back(0x4E00 + rng() % 0x5200);
    for (const u32 c : values)
        for (const char* fn : {"UCS2toEUCKR", "UCS2toUHC"})
            l.same(fn, [&](u32 base) { return std::vector<u64>{c, base}; }, std::format("{:#x}", c));
    for (u32 k = 0; k < 1500; k++) {
        const u32 c = values[rng() % values.size()];
        std::vector<u8> s;
        if (c < 0x80) s = {static_cast<u8>(c)};
        else if (c < 0x800) s = {static_cast<u8>(0xC0 | (c >> 6)), static_cast<u8>(0x80 | (c & 0x3F))};
        else s = {static_cast<u8>(0xE0 | (c >> 12)), static_cast<u8>(0x80 | ((c >> 6) & 0x3F)), static_cast<u8>(0x80 | (c & 0x3F))};
        for (const char* fn : {"UTF8toEUCKR", "UTF8toUHC"})
            l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base, base + 0x80}; }, hex(s));
    }
    for (u32 round = 0; round < 400; round++) {
        std::vector<u8> bytes, units;
        for (u32 i = 0, n = 1 + rng() % 10; i < n; i++) {
            const u32 r = rng() % 6;
            if (r < 2) bytes.push_back(static_cast<u8>(0x20 + rng() % 0x5F));
            else if (r < 4) { bytes.push_back(static_cast<u8>(0xA1 + rng() % 0x5E)); bytes.push_back(static_cast<u8>(0xA1 + rng() % 0x5E)); }
            else if (r < 5) { bytes.push_back(static_cast<u8>(0x81 + rng() % 0x46)); bytes.push_back(static_cast<u8>(0x41 + rng() % 0xBE)); }
            else { const auto x = random_bytes(rng, 2); bytes.insert(bytes.end(), x.begin(), x.end()); }
            const u32 v = values[rng() % values.size()];
            units.push_back(static_cast<u8>(v >> 8));
            units.push_back(static_cast<u8>(v));
        }
        for (const char* fn : {"EUCKRstoUCS2s", "EUCKRstoUTF8s", "UHCstoUCS2s", "UHCstoUTF8s", "UTF8stoEUCKRs",
                 "UTF8stoUHCs", "EUCKRstoUHCs", "UHCstoEUCKRs", "UCS2stoEUCKRs", "UCS2stoUHCs"}) {
            const bool wide = std::string(fn).starts_with("UCS2s");
            const std::vector<u8>& in = wide ? units : bytes;
            const u32 n = static_cast<u32>(wide ? in.size() / 2 : in.size());
            for (const u32 sl : {n, n > 1 ? n - 1 : n})
                for (const u32 cap : {0x100u, 0u, 1u, 2u, 3u})
                    l.same(fn, [&](u32 base) {
                        put_bytes(b, base + 0x200, in);
                        ps_write32(b, base + 0x3F0, sl);
                        ps_write32(b, base + 0x3F4, cap);
                        return std::vector<u64>{base + 0x200, base + 0x3F0, base, base + 0x3F4};
                    }, std::format("{} n {} cap {}", hex(in), sl, cap));
        }
    }
    CHECK(l.mismatches == 0);
}

TEST_CASE("cellL10n: the Chinese converters are libl10n's (D-517)", "[hle][l10n]")
{
    L10n l;
    if (!l.sony) SKIP("PSPOCKET_TEST_DEV_FLASH is not set");
    u8* b = l.b;
    std::mt19937 rng(31);
    for (u32 b0 = 0; b0 < 256; b0++) {
        for (u32 b1 = 0; b1 < 256; b1++) {
            const std::vector<u8> s{static_cast<u8>(b0), static_cast<u8>(b1), 0x81, 0x30};
            for (const char* fn : {"EUCCNtoUCS2", "GBKtoUCS2", "GB18030toUCS2", "BIG5toUCS2"})
                l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base}; }, hex(s));
            if ((b0 & 7) == 0 || b0 >= 0x80)
                for (const char* fn : {"EUCCNtoUTF8", "GBKtoUTF8", "GB18030toUTF8", "BIG5toUTF8"})
                    l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base, base + 0x80}; }, hex(s));
        }
    }
    // GB18030's four-byte area around CJK Extension A.
    for (u32 b0 : {0x81u, 0x82u, 0x84u, 0x90u, 0xE3u, 0xFEu})
        for (u32 b1 = 0x2F; b1 <= 0x3A; b1++)
            for (u32 k = 0; k < 60; k++) {
                const std::vector<u8> s{static_cast<u8>(b0), static_cast<u8>(b1), static_cast<u8>(0x80 + rng() % 0x80), static_cast<u8>(0x2F + rng() % 12)};
                l.same("GB18030toUCS2", [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base}; }, hex(s));
            }
    std::vector<u32> values;
    for (u32 c = 0; c < 0x3200; c++) values.push_back(c);
    for (u32 c = 0x3400; c < 0x4DC0; c += 3) values.push_back(c);
    for (u32 c = 0xE000; c < 0xE900; c++) values.push_back(c);
    for (u32 c = 0xF900; c < 0x10000; c++) values.push_back(c);
    for (u32 k = 0; k < 2500; k++) values.push_back(0x4E00 + rng() % 0x5200);
    for (const u32 c : values)
        for (const char* fn : {"UCS2toEUCCN", "UCS2toGBK", "UCS2toGB18030", "UCS2toBIG5"})
            l.same(fn, [&](u32 base) { return std::vector<u64>{c, base}; }, std::format("{:#x}", c));
    for (u32 k = 0; k < 1500; k++) {
        const u32 c = values[rng() % values.size()];
        std::vector<u8> s;
        if (c < 0x80) s = {static_cast<u8>(c)};
        else if (c < 0x800) s = {static_cast<u8>(0xC0 | (c >> 6)), static_cast<u8>(0x80 | (c & 0x3F))};
        else s = {static_cast<u8>(0xE0 | (c >> 12)), static_cast<u8>(0x80 | ((c >> 6) & 0x3F)), static_cast<u8>(0x80 | (c & 0x3F))};
        for (const char* fn : {"UTF8toEUCCN", "UTF8toGBK", "UTF8toGB18030", "UTF8toBIG5"})
            l.same(fn, [&](u32 base) { put_bytes(b, base + 0x100, s); return std::vector<u64>{base + 0x100, base, base + 0x80}; }, hex(s));
    }
    for (u32 round = 0; round < 300; round++) {
        std::vector<u8> bytes, units;
        for (u32 i = 0, n = 1 + rng() % 10; i < n; i++) {
            const u32 r = rng() % 7;
            if (r < 2) bytes.push_back(static_cast<u8>(0x20 + rng() % 0x5F));
            else if (r < 4) { bytes.push_back(static_cast<u8>(0xA1 + rng() % 0x5E)); bytes.push_back(static_cast<u8>(0xA1 + rng() % 0x5E)); }
            else if (r < 5) { bytes.push_back(static_cast<u8>(0x81 + rng() % 0x7E)); bytes.push_back(static_cast<u8>(0x40 + rng() % 0xBF)); }
            else if (r < 6) { bytes.push_back(0x81 + rng() % 2); bytes.push_back(static_cast<u8>(0x30 + rng() % 10)); bytes.push_back(static_cast<u8>(0x81 + rng() % 0x7E)); bytes.push_back(static_cast<u8>(0x30 + rng() % 10)); }
            else { const auto x = random_bytes(rng, 2); bytes.insert(bytes.end(), x.begin(), x.end()); }
            const u32 v = values[rng() % values.size()];
            units.push_back(static_cast<u8>(v >> 8));
            units.push_back(static_cast<u8>(v));
        }
        for (const char* fam : {"EUCCN", "GBK", "GB18030", "BIG5"}) {
            const std::string F(fam);
            for (const std::string& name : {F + "stoUCS2s", F + "stoUTF8s", "UCS2sto" + F + "s", "UTF8sto" + F + "s"}) {
                const bool wide = name.starts_with("UCS2s");
                const std::vector<u8>& in = wide ? units : bytes;
                const u32 n = static_cast<u32>(wide ? in.size() / 2 : in.size());
                for (const u32 sl : {n, n > 1 ? n - 1 : n})
                    for (const u32 cap : {0x100u, 0u, 1u, 3u})
                        l.same(name, [&](u32 base) {
                            put_bytes(b, base + 0x200, in);
                            ps_write32(b, base + 0x3F0, sl);
                            ps_write32(b, base + 0x3F4, cap);
                            return std::vector<u64>{base + 0x200, base + 0x3F0, base, base + 0x3F4};
                        }, std::format("{} n {} cap {}", hex(in), sl, cap));
            }
        }
    }
    CHECK(l.mismatches == 0);
}
