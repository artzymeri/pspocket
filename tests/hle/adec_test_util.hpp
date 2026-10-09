#pragma once

// What the cellAdec tests share (D-197, D-577): the guest's callback that logs every message, the fixture with the
// decoder's memory, the calls into pspocket's cellAdec, and a decoder's life on a core with what it shows.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

namespace pspocket::hle::test::adec_test {

namespace as = pspocket::recomp::ppu::as;

struct InterpretMisses {
    const ps_ppu_miss_policy ppu = ps_ppu_get_miss_policy();
    const ps_spu_miss_policy spu = ps_spu_get_miss_policy();
    InterpretMisses()
    {
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    }
    ~InterpretMisses()
    {
        ps_ppu_set_miss_policy(ppu);
        ps_spu_set_miss_policy(spu);
    }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

constexpr u32 kCode = kData, kOpd = kData + 0x400, kLog = kData + 0x1000;
constexpr u32 kArgs = kData + 0x4000, kType = kArgs, kAttr = kArgs + 0x10, kRes = kArgs + 0x20, kCb = kArgs + 0x40,
              kHandle = kArgs + 0x50, kParam = kArgs + 0x60, kAu = kArgs + 0x80, kItemOut = kArgs + 0xB0;
constexpr u32 kStream = kData + 0x10000; // access units, 0x2000 bytes each
constexpr u32 kPcm = kData + 0x40000;    // GetPcm's output
constexpr u32 kDecoderMem = 0x01000000, kDecoderMemSize = 0x00800000;

// The callback (handle r3, msgType r4, msgData r5, cbArg r6) appends (type, data, handle) to the log at cbArg.
inline void write_callback(u8* b)
{
    const std::vector<u32> code = {
        as::lwz(9, 0, 6), as::slwi(10, 9, 4), as::add(10, 10, 6), as::stw(4, 16, 10), as::stw(5, 20, 10),
        as::stw(3, 24, 10), as::lwsync(), as::addi(9, 9, 1), as::stw(9, 0, 6), as::li(3, 0), as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kOpd, kCode);
    ps_write32(b, kOpd + 4, 0);
    std::memset(b + kLog, 0, 0x1000);
}

inline u32 log_count(u8* b)
{
    const u32 n = ps_read32(b, kLog);
    std::atomic_thread_fence(std::memory_order_acquire);
    return n;
}

inline u32 count_of(u8* b, u32 type)
{
    u32 n = 0;
    for (u32 i = 0; i < log_count(b); i++) n += ps_read32(b, kLog + 16 + 16 * i) == type;
    return n;
}

inline bool wait_until(const std::function<bool()>& done, std::chrono::seconds timeout = std::chrono::seconds(30))
{
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

inline std::string hex(const u8* p, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

struct BigFixture : Fixture {
    BigFixture()
    {
        REQUIRE(hle->link_internal_functions({"cellAdec"}));
        REQUIRE(hle->main_memory().reserve(kDecoderMem, kDecoderMemSize));
        REQUIRE(mem.commit(kDecoderMem, kDecoderMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

using Call = std::function<u32(const char* name, u32 nid, std::initializer_list<u64> args)>;

// pspocket's cellAdec, linked as a game's imports would be.
inline Call hle_calls(Fixture& f, GuestThread& t)
{
    return [&f, &t](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 descriptor = f.hle->link_function("cellAdec", nid, name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    };
}

// What a decoder's life shows: every answer, item, information block and callback as a line (compared exactly), and
// each PCM taken (compared byte by byte).
struct Observed {
    std::vector<std::string> lines;
    std::vector<std::vector<u8>> pcm;
};

// A decoder of `type` on a firmware core: its answers, items, information blocks (0x40 bytes), PCM and callbacks.
inline Observed core_run(u8* b, const Call& call, u32 type, std::initializer_list<u32> param, const std::vector<std::vector<u8>>& aus)
{
    Observed o;
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 v = call(name, nid, args);
        o.lines.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    const auto note = [&](std::string s) { o.lines.push_back(std::move(s)); };
    std::memset(b + kArgs, 0, 0x200);
    write_callback(b);
    ps_write32(b, kType, type);
    r("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr});
    note(std::format("attr {}: {:#x} {:#x} {:#x}", type, ps_read32(b, kAttr), ps_read32(b, kAttr + 4), ps_read32(b, kAttr + 8)));
    const u32 res[5] = {ps_read32(b, kAttr), kDecoderMem, 1000, 200, 0x4000};
    for (u32 i = 0; i < 5; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    if (r("cellAdecOpen", 0xD00A6988, {kType, kRes, kCb, kHandle}) != 0) return o;
    const u32 handle = ps_read32(b, kHandle);
    u32 i = 0;
    for (const u32 v : param) ps_write32(b, kParam + 4 * i++, v);
    if (r("cellAdecStartSeq", 0x487B613E, {handle, kParam}) != 0) {
        r("cellAdecClose", 0x847D2380, {handle});
        return o;
    }
    u32 outs = 0;
    for (u32 k = 0; k < aus.size(); k++) {
        const u32 at = kStream + 0x2000 * (k % 16);
        std::memcpy(b + at, aus[k].data(), aus[k].size());
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(aus[k].size()));
        ps_write32(b, kAu + 8, k == 2 ? 0xFFFFFFFF : 0); // the third without a PTS: the core's correction
        ps_write32(b, kAu + 12, k == 2 ? 0xFFFFFFFF : 0x1000 + 2880 * k);
        ps_write64(b, kAu + 16, 0x77000000 + k);
        if (r("cellAdecDecodeAu", 0x1529E506, {handle, kAu}) != 0) continue;
        if (!wait_until([&] { return count_of(b, 0) >= k + 1; })) note("no AUDONE");
        // The PCM of this access unit (a core may hold one back: then what is out once it settles).
        if (!wait_until([&] { return count_of(b, 1) >= outs + 1; }, std::chrono::seconds(2))) {
            for (int quiet = 0; quiet < 25; quiet++) {
                const u32 now = count_of(b, 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (count_of(b, 1) != now) quiet = 0;
            }
        }
        for (; outs < count_of(b, 1); outs++) {
            ps_write32(b, kItemOut, 0);
            if (r("cellAdecGetPcmItem", 0xBD75F78B, {handle, kItemOut}) != 0) break;
            const u32 item = ps_read32(b, kItemOut);
            note(std::format("item at {:+#x}: {}", static_cast<s64>(item) - handle, hex(b + item, 0x30)));
            const u32 bsi = ps_read32(b, item + 0x10);
            note(std::format("  information at {:+#x}: {}", static_cast<s64>(bsi) - handle, hex(b + bsi, 0x40)));
            std::memset(b + kPcm, 0xCC, 0x10000);
            if (r("cellAdecGetPcm", 0x97FF2AF1, {handle, kPcm}) != 0) break;
            const u32 size = ps_read32(b, item + 0xC);
            note(std::format("  pcm: {} bytes", size));
            o.pcm.emplace_back(b + kPcm, b + kPcm + std::min<u32>(size, 0x10000));
        }
    }
    r("cellAdecEndSeq", 0xE2EA549B, {handle});
    if (!wait_until([&] { return count_of(b, 3) >= 1; })) note("no SEQDONE");
    for (u32 n = 0; n < log_count(b); n++) {
        const u32 type_n = ps_read32(b, kLog + 16 + 16 * n), data = ps_read32(b, kLog + 20 + 16 * n);
        note(std::format("callback {}: type {} data {}", n, type_n,
            type_n == 0 ? std::format("{:+#x}", static_cast<s64>(data) - handle) : std::format("{:#x}", data)));
    }
    r("cellAdecClose", 0x847D2380, {handle});
    return o;
}

// The firmware's codec cores linked for pspocket's cellAdec as the loader links them for a game (D-197), started.
struct FirmwareCores {
    loader::LleModules lle;
    FirmwareCores(Fixture& f, GuestThread& t, const std::string& dev_flash) : lle(dev_flash)
    {
        REQUIRE(lle.index());
        lle.link_host_imports({{"cellAdec", {}, {}}}, *f.hle);
        for (const u32 start : lle.take_starts()) hle::call_guest(t.ctx, start, {0, 0});
    }
};

inline usize compare(const Observed& ours, const Observed& theirs)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(ours.lines.size(), theirs.lines.size()); i++) {
        const std::string x = i < ours.lines.size() ? ours.lines[i] : "(none)";
        const std::string y = i < theirs.lines.size() ? theirs.lines[i] : "(none)";
        if (x == y) continue;
        if (++differ <= 40) UNSCOPED_INFO(std::format("line {}:\n  pspocket: {}\n  Sony:     {}", i, x, y));
    }
    if (ours.pcm.size() != theirs.pcm.size()) {
        UNSCOPED_INFO(std::format("{} PCM outputs, Sony {}", ours.pcm.size(), theirs.pcm.size()));
        return differ + 1;
    }
    for (usize i = 0; i < ours.pcm.size(); i++) {
        if (ours.pcm[i] == theirs.pcm[i]) continue;
        usize first = 0;
        while (first < std::min(ours.pcm[i].size(), theirs.pcm[i].size()) && ours.pcm[i][first] == theirs.pcm[i][first])
            first++;
        if (++differ <= 60)
            UNSCOPED_INFO(std::format("pcm {} differs ({} and {} bytes), from byte {}", i, ours.pcm[i].size(),
                theirs.pcm[i].size(), first));
    }
    return differ;
}

inline std::string all_lines(const std::vector<std::string>& lines)
{
    std::string s;
    for (const std::string& l : lines) s += l + "\n";
    return s;
}

inline const std::string* find_line(const std::vector<std::string>& lines, std::string_view prefix)
{
    for (const std::string& l : lines)
        if (l.starts_with(prefix)) return &l;
    return nullptr;
}

inline float be_float(const std::vector<u8>& v, usize i)
{
    const u32 bits = static_cast<u32>(v[4 * i]) << 24 | static_cast<u32>(v[4 * i + 1]) << 16 |
        static_cast<u32>(v[4 * i + 2]) << 8 | v[4 * i + 3];
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

} // namespace pspocket::hle::test::adec_test
