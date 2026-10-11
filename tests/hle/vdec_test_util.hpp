#pragma once

// What the cellVdec tests share (D-195, D-588): the guest's callback that logs every message, the fixture with the
// decoder's memory, the calls into pspocket's libvdec, and a decoder's life over one stream with what it shows.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "guest.hpp" // runtime/hle/src: call_guest
#include "hle_test_util.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

namespace pspocket::hle::test::vdec_test {

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

// Guest memory: the scratch area for the callback, its log and the arguments; a larger area for the decoder.
constexpr u32 kCode = kData, kOpd = kData + 0x400, kLog = kData + 0x1000;
constexpr u32 kArgs = kData + 0x4000, kType = kArgs, kAttr = kArgs + 0x20, kRes = kArgs + 0x40, kCb = kArgs + 0x60,
              kHandle = kArgs + 0x70, kSpec = kArgs + 0x80, kAu = kArgs + 0xA0, kFormat = kArgs + 0xD0,
              kItemOut = kArgs + 0xF0;
constexpr u32 kStream = kData + 0x10000;   // the access units
constexpr u32 kPicture = kData + 0x100000; // GetPicture's output
constexpr u32 kDecoderMem = 0x01000000, kDecoderMemSize = 0x01000000;

// The callback (handle r3, msgType r4, msgData r5, cbArg r6) appends (type, data, handle) to the log at cbArg: a
// count, then 16-byte entries from +16. Only the decoder's thread calls it; after its lwsync it raises the count with
// lwarx/stwcx., an update through the reservation system that log_count reads back, so that ThreadSanitizer (which
// does not model the lwsync's fence) sees what the decoder did before a message ordered before the test's next step:
// a test that reuses an access unit's memory once its AUDONE came (D-650, as D-585's tests).
inline void write_callback(u8* b)
{
    const std::vector<u32> code = {
        as::lwz(9, 0, 6), as::slwi(10, 9, 4), as::add(10, 10, 6), as::stw(4, 16, 10), as::stw(5, 20, 10),
        as::stw(3, 24, 10), as::lwsync(), as::lwarx(9, 0, 6), as::addi(9, 9, 1), as::stwcx_(9, 0, 6),
        as::bc_rel(4, 2, -12), // bne- back to the lwarx
        as::li(3, 0), as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kOpd, kCode);
    ps_write32(b, kOpd + 4, 0);
    std::memset(b + kLog, 0, 0x1000);
}

// The callback's count, through the reservation system (as lwarx reads it): what the callback wrote before it, and
// the decoder before the callback, is then visible.
inline u32 log_count(u8* b) { return line_word(b, kLog); }

// The log holds 255 entries; a count past them (a run gone wrong) is cut there, so a test fails rather than spins.
inline u32 count_of(u8* b, u32 type)
{
    const u32 entries = std::min<u32>(log_count(b), (0x1000 - 16) / 16);
    u32 n = 0;
    for (u32 i = 0; i < entries; i++) n += ps_read32(b, kLog + 16 + 16 * i) == type;
    return n;
}

inline bool wait_until(const std::function<bool()>& done, std::chrono::seconds timeout = std::chrono::seconds(60))
{
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

using Call = std::function<u32(const char* name, u32 nid, std::initializer_list<u64> args)>;

inline std::string hex(const u8* p, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

struct BigFixture : Fixture {
    BigFixture()
    {
        REQUIRE(hle->link_internal_functions({"libvdec"}));
        REQUIRE(hle->main_memory().reserve(kDecoderMem, kDecoderMemSize));
        REQUIRE(mem.commit(kDecoderMem, kDecoderMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

// pspocket's libvdec, linked as a game's imports would be.
inline Call hle_calls(Fixture& f, GuestThread& t)
{
    return [&f, &t](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 descriptor = f.hle->link_function("libvdec", nid, name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    };
}

[[maybe_unused]] inline std::string all_lines(const std::vector<std::string>& lines)
{
    std::string s;
    for (const std::string& l : lines) s += l + "\n";
    return s;
}

[[maybe_unused]] inline const std::string* find_line(const std::vector<std::string>& lines, std::string_view prefix)
{
    for (const std::string& l : lines)
        if (l.starts_with(prefix)) return &l;
    return nullptr;
}

// One stream for a decoder of a codec and level, with its codec specific information (none when empty), each access
// unit decoded in `mode` with PTS 0x1000 + 3003 i, DTS 0x800 + 3003 i and user data 0xABCD0000 + i.
struct Stream {
    u32 codec = 0, level = 0;
    std::vector<u8> spec;
    std::vector<std::vector<u8>> aus;
    s32 mode = 0;
    u32 frame_rate = 0; // cellVdecSetFrameRate's code before the sequence when not 0
};

// What the decoder showed: the answers of the calls that can fail, the callbacks counted, each picture's item
// (CellVdecPicItem, 0x4C bytes) and information block (0x244 bytes, Sony's largest), and each picture taken in YUV
// 4:2:0 (GetPicture, Y then U then V without padding; its size from the item).
struct Decoded {
    std::vector<std::string> lines;
    u32 open = 0, audone = 0, picout = 0, seqdone = 0, pictures_after_aus = 0;
    std::vector<std::vector<u8>> items, infos, pictures;
};

// A decoder's life over `s` (pspocket's or Sony's libvdec through `call`): open, a sequence of the stream's access
// units each awaited (AUDONE), the end of the sequence (SEQDONE), then every picture's item, information and samples.
inline Decoded decode_stream(u8* b, const Call& call, const Stream& s)
{
    Decoded d;
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 v = call(name, nid, args);
        d.lines.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    write_callback(b);
    std::memset(b + kArgs, 0, 0x200);
    ps_write32(b, kType, s.codec);
    ps_write32(b, kType + 4, s.level);
    if (!s.spec.empty()) {
        std::memcpy(b + kSpec, s.spec.data(), s.spec.size());
        ps_write32(b, kType + 8, kSpec);
    }
    if (r("cellVdecQueryAttrEx", 0xC982A84A, {kType, kAttr}) != 0) return d;
    // The memory asked for, at most what the fixture has (libvdec opens with less, D-196).
    const u32 res[6] = {kDecoderMem, std::min(ps_read32(b, kAttr), kDecoderMemSize), 1000, 0x4000, 200, 1};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    d.open = r("cellVdecOpenEx", 0x0053E2D8, {kType, kRes, kCb, kHandle});
    if (d.open != 0) return d;
    const u32 handle = ps_read32(b, kHandle);
    if (s.frame_rate) r("cellVdecSetFrameRate", 0xE13EF6FC, {handle, s.frame_rate});
    if (r("cellVdecStartSeq", 0xC757C2AA, {handle}) != 0) return d;
    u32 at = kStream;
    for (usize i = 0; i < s.aus.size(); i++) {
        const std::vector<u8>& au = s.aus[i];
        REQUIRE(at + au.size() <= kPicture);
        std::memcpy(b + at, au.data(), au.size());
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
        ps_write32(b, kAu + 8, 0);
        ps_write32(b, kAu + 12, 0x1000 + 3003 * static_cast<u32>(i));
        ps_write32(b, kAu + 16, 0);
        ps_write32(b, kAu + 20, 0x800 + 3003 * static_cast<u32>(i));
        ps_write64(b, kAu + 24, 0xABCD0000 + static_cast<u64>(i));
        ps_write64(b, kAu + 32, 0);
        at = (at + static_cast<u32>(au.size()) + 15) & ~15u;
        if (r("cellVdecDecodeAu", 0x2BF4DDD2, {handle, static_cast<u64>(static_cast<u32>(s.mode)), kAu}) != 0) continue;
        if (!wait_until([&] { return count_of(b, 0) >= static_cast<u32>(i + 1); })) d.lines.push_back("no AUDONE");
    }
    // The pictures announced before the end of the sequence (once no more come for 0.4 s): one access unit late, as
    // Sony's.
    u32 seen = count_of(b, 1);
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (int quiet = 0; quiet < 20; quiet++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const u32 now = count_of(b, 1);
        if (now != seen) quiet = 0;
        seen = now;
        if (std::chrono::steady_clock::now() > give_up) {
            d.lines.push_back("pictures still coming after 30 s");
            break;
        }
    }
    d.pictures_after_aus = seen;
    r("cellVdecEndSeq", 0x824433F0, {handle});
    if (!wait_until([&] { return count_of(b, 2) >= 1; })) d.lines.push_back("no SEQDONE");
    for (;;) {
        ps_write32(b, kItemOut, 0);
        if (call("cellVdecGetPicItem", 0x17C702B9, {handle, kItemOut}) != 0) break;
        const u32 item = ps_read32(b, kItemOut), info = ps_read32(b, item + 0x48), size = ps_read32(b, item + 8);
        d.items.emplace_back(b + item, b + item + 0x4C);
        d.infos.emplace_back(b + info, b + info + 0x244);
        ps_write32(b, kFormat, 3); // YUV 4:2:0 planar
        ps_write32(b, kFormat + 4, 0);
        ps_write8(b, kFormat + 8, 0);
        std::memset(b + kPicture, 0xCC, size + 0x100);
        if (r("cellVdecGetPicture", 0x807C861A, {handle, kFormat, kPicture}) != 0) break;
        d.pictures.emplace_back(b + kPicture, b + kPicture + size);
    }
    d.audone = count_of(b, 0);
    d.picout = count_of(b, 1);
    d.seqdone = count_of(b, 2);
    r("cellVdecClose", 0x16698E83, {handle});
    return d;
}

} // namespace pspocket::hle::test::vdec_test
