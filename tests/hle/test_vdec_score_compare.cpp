// The "s" video cores' own APIs against Sony's modules (D-920): libsmvd2 (MPEG-2) so far, called directly as Sony's
// libvdec (or a game) calls it, with the same synthetic streams (mpeg2_writer.hpp) through pspocket's core and through
// Sony's module (the user's installed system software), each in a process of its own per half. Sony's cores decode on
// SPURS: each half makes a SPURS instance and a taskset (pspocket's cellSpurs) and gives the taskset to the core.
// Compared: every answer and output structure, the frame buffers' places (as offsets into the core's memory) and their
// samples, the pictures' order and the errors. The cores' own memory is opaque but where a caller reads it back. Skipped
// without the firmware (compare_util.hpp). No game data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "compare_util.hpp"
#include "mpeg2_writer.hpp"
#include "vdec_mpeg2.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

namespace {

// Guest memory: the core's memory, SPURS and its taskset, the arguments and the streams.
constexpr u32 kMem = 0x01000000, kMemSize = 0x02000000;
constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kPrio = kData + 0x4400,
              kTs = kData + 0x6000;
constexpr u32 kArg = kData + 0x8000, kParam = kArg, kOut = kArg + 0x40, kAu = kArg + 0x80, kStatus = kArg + 0xC0,
              kPic = kArg + 0x100, kAux = kArg + 0x200, kAuxOut = kArg + 0x300, kBnr = kArg + 0x380, kSeq = kArg + 0x400,
              kCount = kArg + 0x480, kMode = kArg + 0x4C0, kSide = kArg + 0x500; // kSide: two blocks of 0xBC
constexpr u32 kStream = kData + 0x10000;

struct CoreFixture : Fixture {
    CoreFixture()
    {
        REQUIRE(hle->main_memory().reserve(kMem, kMemSize));
        REQUIRE(mem.commit(kMem, kMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};
using Run = Half<CoreFixture>;

std::string in_core(u32 addr)
{
    if (addr >= kMem && addr < kMem + kMemSize) return std::format("the core's memory +{:#x}", addr - kMem);
    return std::format("{:#x}", addr);
}

// A SPURS instance with two SPUs and a taskset on them, as libvdec makes them (pspocket's cellSpurs in both halves).
void make_spurs(Run& r)
{
    REQUIRE(r.t.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 2, 200, 1000, 1) == 0);
    REQUIRE(r.t.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    const u8 prio[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    std::memcpy(r.b + kPrio, prio, 8);
    REQUIRE(r.t.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0, kPrio, 2) == 0);
    REQUIRE(r.t.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs, kTsAttr) == 0);
}

void end_spurs(Run& r)
{
    r.t.call("cellSpursShutdownTaskset", kTs);
    r.t.call("cellSpursJoinTaskset", kTs);
    r.t.call("cellSpursFinalize", kSpurs);
}

// ------------------------------------------------------------------------------------------------- MPEG-2

// smvd2CreateInstance2's parameters: the type (4: the memory for a level, 5: for a size), the SPU tasks, the taskset,
// the width and height.
void smvd2_param(u8* b, u32 type, u32 spus, u32 taskset, u32 w = 0, u32 h = 0)
{
    std::memset(b + kParam, 0, 0x28);
    ps_write32(b, kParam + 0x00, type);
    ps_write32(b, kParam + 0x04, spus);
    ps_write32(b, kParam + 0x08, taskset);
    ps_write32(b, kParam + 0x0C, w);
    ps_write32(b, kParam + 0x10, h);
}

struct Smvd2 {
    Run& r;
    u32 handle = 0;
    u32 taken = 0;
    u32 at = kStream;
    // The frame buffers GetPicture gave (address, bytes), to clear them before a stream whose damaged pictures are
    // compared (damaged_too).
    std::vector<std::pair<u32, u32>> frames_seen;
    bool damaged_too = false;
    void clear_frames(u8 v)
    {
        for (const auto& [a, n] : frames_seen) std::memset(r.b + a, v, n);
    }

    std::string w(u32 v) { return std::format("{:#x}", v); }

    // The core's own state after a call, as a note (opaque, not compared).
    void note_state(const std::string& key)
    {
        if (!handle || !env_on("PSPOCKET_CORE_STATE")) return;
        u8* b = r.b;
        r.o.note(key + ": seq " + compare::hex(b + handle + 0x80, 0x20));
        r.o.note(key + ": words " + compare::hex(b + handle + 0x100, 0x90));
        r.o.note(key + ": queue " + compare::hex(b + handle + 0xF00, 4) + " " + compare::hex(b + handle + 0xF80, 4) +
                 " " + compare::hex(b + handle + 0x1000, 0x10));
        const u32 entries = ps_read32(b, handle + 0xE80);
        if (entries >= kMem && entries < kMem + kMemSize) {
            std::string e;
            for (u32 k = 0; k < 8; k++) e += compare::hex(b + entries + k * 0x80, 8) + " ";
            r.o.note(key + ": entries " + e + compare::hex(b + entries + 0x400, 8));
        }
        for (u32 k = 0; k < 4; k++)
            r.o.note(std::format("{}: picture {} ", key, k) + compare::hex(b + handle + 0x400 + k * 0x280, 0x280));
    }

    // Each picture the core holds: GetNumPicture, then GetPicture (its nine words, the addresses as offsets), its side
    // information, auxiliary data and BNR data, a window of memory at its frame; ReleasePicture after each when
    // `release`.
    void take(const std::string& when, bool release, u32 limit = ~0u)
    {
        u8* b = r.b;
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("smvd2GetNumPicture", {handle, kOut}, when + ": GetNumPicture");
        r.o.line(when + ": pictures", w(ps_read32(b, kOut)));
        for (u32 k = 0; k < limit; k++) {
            std::memset(b + kPic, 0xEE, 0x30);
            const std::string key = std::format("{}: picture {}", when, taken);
            const u64 answer = r.r("smvd2GetPicture", {handle, kPic}, key + ": GetPicture");
            if (ps_read32(b, kPic + 12) == 0xEEEEEEEE) return; // none
            taken++;
            const u32 luma = ps_read32(b, kPic), cb = ps_read32(b, kPic + 4), cr = ps_read32(b, kPic + 8);
            r.o.line(key + ": luma", in_core(luma));
            r.o.line(key + ": Cb", in_core(cb));
            r.o.line(key + ": Cr", in_core(cr));
            std::vector<u8> words(b + kPic, b + kPic + 0x30);
            std::memset(words.data(), 0, 12);
            r.o.line(key + ": the picture", compare::hex(words));
            std::memset(b + kSide, 0xEE, 0x180);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("smvd2GetSideInfoPic", {handle, kCount, kSide, kSide + 0xBC}, key + ": GetSideInfoPic");
            r.o.line(key + ": side information", w(ps_read32(b, kCount)) + " " + compare::hex(b + kSide, 0x180));
            std::memset(b + kAuxOut, 0xEE, 0x20);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("smvd2GetAuxData", {handle, kCount, kAuxOut, kAuxOut + 0x10}, key + ": GetAuxData");
            r.o.line(key + ": auxiliary data", w(ps_read32(b, kCount)) + " " + compare::hex(b + kAuxOut, 0x20));
            std::memset(b + kBnr, 0xEE, 0x40);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("smvd2GetBnrData", {handle, kCount, kBnr, kBnr + 0x20}, key + ": GetBnrData");
            r.o.line(key + ": BNR data", w(ps_read32(b, kCount)) + " " + compare::hex(b + kBnr, 0x40));
            const u32 pw = ps_read32(b, kPic + 16), ph = ps_read32(b, kPic + 20);
            // The frame: macroblock after macroblock (16 lines of luma, then 8 lines of Cb and Cr: 0x180 bytes), to the
            // sample. But a damaged picture's (a warning but no reference's: 0x..11 to 0x..13) or one predicted from
            // one (0x400: 0x..14 to 0x..17, 0x..1C to 0x..1F): where Sony's slice program puts the macroblocks after an
            // error inside a slice is not reproduced (vdec_mpeg2.hpp), so their samples are a note, but in the streams
            // whose damage is the slices' order (damaged_too).
            const u32 bytes = ((pw + 15) / 16) * ((ph + 15) / 16) * 0x180;
            const u32 low = static_cast<u32>(answer) & 0xFF;
            const bool damaged = (static_cast<u32>(answer) & 0xFFFFFF00u) == 0x80615000 && low >= 0x11 && low <= 0x1F &&
                                 (low <= 0x13 || (low & 0x14) == 0x14);
            if (luma >= kMem && luma + bytes <= kMem + kMemSize && pw <= 1920 && ph <= 1152) {
                if (std::find(frames_seen.begin(), frames_seen.end(), std::pair{luma, bytes}) == frames_seen.end())
                    frames_seen.emplace_back(luma, bytes);
                if (damaged && !damaged_too)
                    r.o.note(key + ": its frame (a damaged picture's): " + compare::hex(b + luma, 16));
                else
                    r.o.bytes(key + ": its frame", b + luma, bytes);
            }
            if (release) r.r("smvd2ReleasePicture", {handle}, key + ": ReleasePicture");
        }
    }

    // SetAuxData (16 bytes of the caller's), then DecodePicture(handle, au {address, size, a word}, used), or
    // DecodePicture2(handle, au {address, size, address, size}, used) with the access unit in two pieces.
    u64 decode(const std::string& key, const std::vector<u8>& au, u32 user, bool two = false, usize split = 0)
    {
        u8* b = r.b;
        if (at + au.size() + 0x40 > kStream + 0x80000) at = kStream;
        for (u32 k = 0; k < 4; k++) ps_write32(b, kAux + 4 * k, (0x1000 << k) + user);
        r.r("smvd2SetAuxData", {handle, kAux}, key + ": SetAuxData");
        ps_write32(b, kStatus, 0xEEEEEEEE);
        u64 v = 0;
        if (!two) {
            std::memcpy(b + at, au.data(), au.size());
            ps_write32(b, kAu + 0, at);
            ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
            ps_write32(b, kAu + 8, 0x5000 + user);
            at = (at + static_cast<u32>(au.size()) + 15) & ~15u;
            v = r.r("smvd2DecodePicture", {handle, kAu, kStatus}, key);
        } else {
            const usize first = split ? split : au.size() / 2;
            std::memcpy(b + at, au.data(), first);
            const u32 second_at = (at + static_cast<u32>(first) + 0x20 + 15) & ~15u;
            std::memcpy(b + second_at, au.data() + first, au.size() - first);
            ps_write32(b, kAu + 0, at);
            ps_write32(b, kAu + 4, static_cast<u32>(first));
            ps_write32(b, kAu + 8, second_at);
            ps_write32(b, kAu + 12, static_cast<u32>(au.size() - first));
            at = (second_at + static_cast<u32>(au.size() - first) + 15) & ~15u;
            v = r.r("smvd2DecodePicture2", {handle, kAu, kStatus}, key);
        }
        r.o.line(key + ": used", w(ps_read32(b, kStatus)));
        flag(key);
        note_state(key);
        return v;
    }

    void flag(const std::string& key)
    {
        ps_write32(r.b, kOut, 0xEEEEEEEE);
        r.r("smvd2GetExistenceFlag", {handle, kOut}, key + ": GetExistenceFlag");
        r.o.line(key + ": the flag", w(ps_read32(r.b, kOut)));
    }

    void seq(const std::string& key)
    {
        std::memset(r.b + kSeq, 0xEE, 0x40);
        r.r("smvd2GetSideInfoSeq", {handle, kSeq}, key + ": GetSideInfoSeq");
        r.o.line(key + ": the sequence", compare::hex(r.b + kSeq, 0x40));
    }

    u64 init(const std::string& key) { return r.r("smvd2Initialize", {handle}, key); }

    // A whole stream, each picture taken and released as soon as there is one; then the flush.
    void stream(const std::string& what, const std::vector<std::vector<u8>>& aus, u32 user0, bool two = false)
    {
        for (usize i = 0; i < aus.size(); i++) {
            decode(std::format("{}: DecodePicture {}", what, i), aus[i], user0 + static_cast<u32>(i), two);
            take(std::format("{}: after DecodePicture {}", what, i), true);
        }
        r.r("smvd2FlushPicture", {handle}, what + ": FlushPicture");
        note_state(what + ": FlushPicture");
        take(what + ": after FlushPicture", true);
    }
};

Observed smvd2(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libsmvd2");
    u8* b = r.b;
    make_spurs(r);
    Smvd2 d{r, 0, 0, kStream, {}, false};
    // The memory queries: each level's largest picture, and sizes within them.
    for (u32 level = 0; level <= 18; level++) {
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("smvd2GetMemorySize", {kOut, level}, std::format("GetMemorySize level {}", level));
        r.o.line(std::format("GetMemorySize level {}: the size", level), d.w(ps_read32(b, kOut)));
    }
    r.r("smvd2GetMemorySize", {0, 2}, "GetMemorySize with no output");
    for (const auto& [level, spus, wd, ht] : {std::tuple{2u, 1u, 96u, 64u}, {2u, 2u, 96u, 64u}, {2u, 6u, 96u, 64u},
             {2u, 0u, 96u, 64u}, {2u, 7u, 96u, 64u}, {2u, 1u, 720u, 576u}, {2u, 1u, 721u, 576u}, {2u, 1u, 32u, 32u},
             {2u, 1u, 48u, 48u}, {2u, 1u, 0u, 64u}, {1u, 1u, 352u, 288u}, {16u, 1u, 1280u, 720u}, {5u, 1u, 96u, 64u},
             {0u, 1u, 1920u, 1088u}}) {
        smvd2_param(b, 5, spus, 0, wd, ht);
        ps_write32(b, kOut, 0xEEEEEEEE);
        const std::string key = std::format("GetMemorySize2 level {} {} SPUs {}x{}", level, spus, wd, ht);
        r.r("smvd2GetMemorySize2", {kOut, level, kParam}, key);
        r.o.line(key + ": the size", d.w(ps_read32(b, kOut)));
    }
    smvd2_param(b, 4, 1, 0, 96, 64);
    ps_write32(b, kOut, 0xEEEEEEEE);
    r.r("smvd2GetMemorySize2", {kOut, 2, kParam}, "GetMemorySize2 type 4");
    r.o.line("GetMemorySize2 type 4: the size", d.w(ps_read32(b, kOut)));
    r.r("smvd2GetMemorySize2", {kOut, 2, 0}, "GetMemorySize2 with no parameters");
    r.r("smvd2GetMemorySize2", {0, 2, kParam}, "GetMemorySize2 with no output");
    ps_write32(b, kOut, 0xEEEEEEEE);
    r.r("smvd2GetVersionNumber", {kOut}, "GetVersionNumber");
    r.o.line("GetVersionNumber: the version", d.w(ps_read32(b, kOut)));

    mpeg2::Sequence seq;
    const std::vector<std::vector<u8>> ippp = mpeg2::stream(seq, "IPPPPIPP");
    smvd2_param(b, 5, 1, kTs, 96, 64);
    ps_write32(b, kOut, 0);
    r.r("smvd2GetMemorySize2", {kOut, 2, kParam}, "GetMemorySize2 for the decoder");
    const u32 need = ps_read32(b, kOut);
    // CreateInstance2's refusals.
    std::memset(b + kMem, 0xA5, need + 0x1000);
    r.r("smvd2CreateInstance2", {0, need, 2, kParam}, "CreateInstance2 with no memory");
    r.r("smvd2CreateInstance2", {kMem + 0x40, need, 2, kParam}, "CreateInstance2 with memory not aligned");
    r.r("smvd2CreateInstance2", {kMem, need, 2, 0}, "CreateInstance2 with no parameters");
    r.r("smvd2CreateInstance2", {kMem, 0x3BFF, 2, kParam}, "CreateInstance2 with 0x3BFF bytes");
    for (const u32 type : {0u, 1u, 2u, 3u, 6u}) {
        smvd2_param(b, type, 1, kTs, 96, 64);
        r.r("smvd2CreateInstance2", {kMem, need, 2, kParam}, std::format("CreateInstance2 type {}", type));
    }
    for (const u32 spus : {0u, 7u}) {
        smvd2_param(b, 5, spus, kTs, 96, 64);
        r.r("smvd2CreateInstance2", {kMem, need, 2, kParam}, std::format("CreateInstance2 with {} SPUs", spus));
    }
    smvd2_param(b, 5, 1, 0, 96, 64);
    r.r("smvd2CreateInstance2", {kMem, need, 2, kParam}, "CreateInstance2 with no taskset");
    smvd2_param(b, 5, 1, kTs, 96, 64);
    r.r("smvd2CreateInstance2", {kMem, need - 1, 2, kParam}, "CreateInstance2 with one byte less");
    r.r("smvd2CreateInstance2", {kMem, need, 7, kParam}, "CreateInstance2 level 7");
    smvd2_param(b, 5, 1, kTs, 800, 64);
    r.r("smvd2CreateInstance2", {kMem, need, 2, kParam}, "CreateInstance2 800x64 level 2");
    // The decoder.
    smvd2_param(b, 5, 1, kTs, 96, 64);
    std::memset(b + kMem, 0xA5, need + 0x1000);
    if (r.r("smvd2CreateInstance2", {kMem, need, 2, kParam}, "CreateInstance2") == 0) {
        d.handle = kMem;
        d.note_state("CreateInstance2");
        d.flag("after CreateInstance2");
        d.seq("after CreateInstance2");
        r.r("smvd2GetPicture", {d.handle, kPic}, "GetPicture before Initialize");
        r.r("smvd2ReleasePicture", {d.handle}, "ReleasePicture before Initialize");
        d.init("Initialize");
        d.note_state("Initialize");
        d.flag("after Initialize");
        d.seq("after Initialize");
        d.stream("IPPP", ippp, 10);
        d.flag("after the stream");
        d.seq("after the stream");
        d.init("IBBP: Initialize");
        d.stream("IBBP", mpeg2::stream(seq, "IBBPBBPBBIBBP"), 100);
        d.seq("after IBBP");
        // The decode modes libvdec sets (0 all pictures, 0xFF its B-skipping mode) and others.
        for (const u32 mode : {0xFFu, 1u, 2u, 0u}) {
            const std::string what = std::format("mode {:#x}", mode);
            ps_write32(b, kMode, mode);
            ps_write32(b, kMode + 4, 0);
            d.init(what + ": Initialize");
            r.r("smvd2SetDecodeMode", {d.handle, kMode}, what + ": SetDecodeMode");
            d.stream(what, mpeg2::stream(seq, "IBBPBBP"), 200 + mode);
        }
        r.r("smvd2SetDecodeMode", {d.handle, 0}, "SetDecodeMode with no mode");
        // DecodePicture2: the access unit in two pieces.
        d.init("two pieces: Initialize");
        d.stream("two pieces", mpeg2::stream(seq, "IPBP"), 300, true);
        // Split at chosen places: at a slice's start code, a byte after it, in a slice's data on a 16-byte boundary and
        // off one, in the headers, inside a start code.
        if (selected("split")) {
            const std::vector<std::vector<u8>> aus = mpeg2::stream(seq, "IPBP", true, 0, [](mpeg2::Picture& p, int) {
                p.motion = true;
            });
            const auto slice_at = [](const std::vector<u8>& au, u8 row) {
                for (usize i = 0; i + 3 < au.size(); i++)
                    if (!au[i] && !au[i + 1] && au[i + 2] == 1 && au[i + 3] == row) return i;
                return au.size() / 2;
            };
            const char* kinds[] = {"at a slice", "a byte into a slice", "in a slice on 16 bytes", "in a slice off 16 bytes",
                "in the headers", "inside a start code"};
            for (int kind = 0; kind < 6; kind++) {
                const std::string what = std::string("split ") + kinds[kind];
                d.init(what + ": Initialize");
                for (usize i = 0; i < aus.size(); i++) {
                    const std::vector<u8>& au = aus[i];
                    const usize first = slice_at(au, 1), second = slice_at(au, 2);
                    usize at = 0;
                    switch (kind) {
                    case 0: at = first; break;
                    case 1: at = first + 1; break;
                    case 2: at = (second + 5 + 15) & ~usize{15}; break;
                    case 3: at = ((second + 5) & 15) ? second + 5 : second + 6; break;
                    case 4: at = 7; break;
                    default: at = second + 2; break;
                    }
                    at = std::min(at, au.size() - 1);
                    r.o.note(std::format("{}: access unit {}: {} bytes, split at {}", what, i, au.size(), at));
                    d.decode(std::format("{}: DecodePicture {}", what, i), au, 320 + static_cast<u32>(i), true, at);
                    d.take(std::format("{}: after DecodePicture {}", what, i), true);
                }
                r.r("smvd2FlushPicture", {d.handle}, what + ": FlushPicture");
                d.take(what + ": after FlushPicture", true);
            }
        }
        // Streams of other kinds, each after Initialize.
        // `damaged_too`: the frame buffers cleared before the stream, and its damaged pictures' samples compared.
        const auto variant = [&](const std::string& what, const std::vector<std::vector<u8>>& aus, u32 user0,
                                 bool damaged_too = false) {
            if (!selected(what)) return;
            d.init(what + ": Initialize");
            if (damaged_too) d.clear_frames(0x11);
            d.damaged_too = damaged_too;
            for (usize i = 0; i < aus.size(); i++)
                r.o.note(std::format("{}: access unit {}: {} bytes", what, i, aus[i].size()));
            d.stream(what, aus, user0);
            d.flag(what);
            d.seq(what);
            d.damaged_too = false;
        };
        const auto with = [&](void (*change)(mpeg2::Sequence&)) {
            mpeg2::Sequence v;
            change(v);
            return v;
        };
        variant("aspect 3, 30 Hz, bit rate", mpeg2::stream(with([](mpeg2::Sequence& v) {
            v.aspect = 3;
            v.frame_rate = 5;
            v.bit_rate = 0x123456;
            v.vbv_size = 0x3456;
            v.constrained = true;
        }), "IPBP"), 1000);
        variant("high level, low delay, rate extension", mpeg2::stream(with([](mpeg2::Sequence& v) {
            v.profile_level = 0x44;
            v.low_delay = true;
            v.frame_rate_n = 2;
            v.frame_rate_d = 9;
        }), "IPPP"), 1100);
        variant("display extension", mpeg2::stream(with([](mpeg2::Sequence& v) {
            v.display_width = 90;
            v.display_height = 60;
            v.video_format = 2;
        }), "IPBP"), 1200);
        variant("display extension with colour", mpeg2::stream(with([](mpeg2::Sequence& v) {
            v.display_width = 80;
            v.display_height = 50;
            v.colour_primaries = 5;
            v.transfer = 6;
            v.matrix = 7;
        }), "IPBP"), 1300);
        variant("matrices and user data", mpeg2::stream(with([](mpeg2::Sequence& v) {
            v.intra_matrix.assign(64, 16);
            v.non_intra_matrix.assign(64, 20);
            v.user_data = {'s', 'e', 'q', 'u', 'e', 'n', 'c', 'e'};
        }), "IPBP", true), 1400);
        variant("MPEG-1", mpeg2::stream(with([](mpeg2::Sequence& v) { v.mpeg1 = true; }), "IPBPB", true), 1500);
        variant("interlaced frames", mpeg2::stream(with([](mpeg2::Sequence& v) { v.progressive = false; }), "IPBPB", true),
            1600);
        variant("80x48", mpeg2::stream(with([](mpeg2::Sequence& v) {
            v.width = 80;
            v.height = 48;
        }), "IPBP", true), 1700);
        variant("coding options", mpeg2::stream(seq, "IPBPBIPB", true, 0, [](mpeg2::Picture& p, int i) {
            p.dc_precision = i % 3;
            p.q_scale_type = i % 2;
            p.intra_vlc = i % 2;
            p.alternate_scan = i % 3 == 1;
            p.vbv_delay = 0x1234 + i;
        }), 1800);
        variant("picture extensions and user data", mpeg2::stream(seq, "IPBPBIPB", true, 0, [](mpeg2::Picture& p, int i) {
            p.display_offsets = {{i * 16, -i * 8}};
            p.copyright = i == 1;
            p.composite = i == 2;
            p.v_axis = 1;
            p.field_sequence = 5;
            p.sub_carrier = 1;
            p.burst_amplitude = 0x55;
            p.sub_carrier_phase = 0xAA;
            if (i == 3) p.quant_intra_matrix.assign(64, 24);
            p.user_data = {'p', static_cast<u8>('0' + i)};
        }), 1900);
        // A picture display extension of one frame centre offset where its flags call for two or three.
        variant("too few frame centre offsets", mpeg2::stream(seq, "IPBPBP", true, 0, [](mpeg2::Picture& p, int i) {
            p.repeat_first_field = i % 2;
            p.top_field_first = i % 3 == 0;
            p.display_offsets = {{i * 16, -i * 8}};
        }), 1950);
        variant("too few frame centre offsets, then user data", mpeg2::stream(seq, "IPBP", true, 0, [](mpeg2::Picture& p,
            int i) {
            p.repeat_first_field = i % 2;
            p.display_offsets = {{i * 16, -i * 8}};
            p.user_data = {'u', static_cast<u8>('0' + i), 1, 2};
        }), 1960);
        // Bytes between a header's end and the next start code.
        {
            // `junk` before the start code that follows the first header of start code `code` (and extension `ext`
            // when 0xB5) in each access unit.
            const auto junk_after = [](std::vector<std::vector<u8>> aus, u8 code, int ext, std::vector<u8> junk) {
                for (auto& au : aus) {
                    for (usize i = 0; i + 4 < au.size(); i++) {
                        if (au[i] || au[i + 1] || au[i + 2] != 1 || au[i + 3] != code) continue;
                        if (ext >= 0 && (au[i + 4] >> 4) != ext) continue;
                        usize j = i + 4;
                        while (j + 3 < au.size() && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)) j++;
                        au.insert(au.begin() + static_cast<std::ptrdiff_t>(j), junk.begin(), junk.end());
                        break;
                    }
                }
                return aus;
            };
            const auto shown = mpeg2::stream(seq, "IPBP", true, 0, [](mpeg2::Picture& p, int i) {
                p.display_offsets = {{i * 16, -i * 8}};
                p.user_data = {'u', static_cast<u8>('0' + i)};
            });
            variant("a byte after the picture display extension", junk_after(shown, 0xB5, 7, {0x5A}), 1970);
            variant("bytes after the sequence extension", junk_after(shown, 0xB5, 1, {0x00, 0x5A, 0x00}), 1980);
            variant("a byte after the picture header", junk_after(shown, 0x00, -1, {0x5A}), 1990);
        }
        // Slices missing, out of order, twice or outside the picture: each access unit's slices rebuilt in the order
        // given (by vertical position; 9 is the third renamed). The frame buffers are cleared first, so that where the
        // program puts each slice shows: these damaged pictures' samples are compared too.
        {
            const auto reorder = [](std::vector<std::vector<u8>> aus, const std::vector<u8>& rows) {
                for (auto& au : aus) {
                    usize first = au.size();
                    std::vector<std::vector<u8>> slices(256);
                    for (usize i = 0; i + 4 < au.size(); i++) {
                        if (au[i] || au[i + 1] || au[i + 2] != 1 || au[i + 3] < 1 || au[i + 3] > 0xAF) continue;
                        first = std::min(first, i);
                        usize j = i + 4;
                        while (j + 3 < au.size() && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)) j++;
                        if (j + 3 >= au.size()) j = au.size();
                        slices[au[i + 3]].assign(au.begin() + static_cast<std::ptrdiff_t>(i),
                            au.begin() + static_cast<std::ptrdiff_t>(j));
                        i = j - 1;
                    }
                    slices[9] = slices[3];
                    if (slices[9].size() > 3) slices[9][3] = 9;
                    au.resize(first);
                    for (const u8 row : rows) au.insert(au.end(), slices[row].begin(), slices[row].end());
                }
                return aus;
            };
            const std::vector<std::vector<u8>> plain = mpeg2::stream(seq, "IPBP", true, 0);
            const std::vector<std::vector<u8>> orders = {{2, 3, 4}, {1, 2, 4}, {1, 3}, {1, 2, 9, 4}, {1, 2, 3, 2},
                {1, 3, 2, 4}, {1, 2, 4, 3}, {1, 4, 2, 3}, {2, 1, 3, 4}, {3, 1, 2, 4}, {4, 3, 2, 1}, {1, 2, 2, 3, 4},
                {1, 1, 2, 3, 4}};
            u32 user = 3000;
            for (const std::vector<u8>& rows : orders) {
                std::string what = "slices";
                for (const u8 row : rows) what += std::format(" {}", row);
                variant(what, reorder(plain, rows), user += 20, true);
            }
            std::vector<std::vector<u8>> third = mpeg2::stream(seq, "IPPP", true, 0);
            third[2] = reorder({third[2]}, {2, 3, 4})[0];
            variant("IPPP, the third without its first slice", third, 3300, true);
        }
        variant("frame flags", mpeg2::stream(seq, "IPBPBIPB", false, 0, [](mpeg2::Picture& p, int i) {
            p.repeat_first_field = i % 2;
            p.top_field_first = i % 3 == 0;
            p.chroma_420_type = i % 2 == 0;
        }), 2000);
        {
            std::vector<std::vector<u8>> aus = mpeg2::stream(seq, "IPPPIPPP");
            const mpeg2::Gop g{3723, 4, true, true, false, {'g', 'o', 'p'}};
            const std::vector<u8> gop = mpeg2::gop_header(g);
            const std::vector<u8> sh = mpeg2::sequence_header(seq);
            for (const usize k : {usize{0}, usize{4}}) {
                std::vector<u8> pic(aus[k].begin() + static_cast<std::ptrdiff_t>(sh.size() + 8), aus[k].end());
                aus[k] = mpeg2::join({sh, gop, pic});
            }
            variant("a GOP at 1:02:03, drop frame, closed", aus, 2100);
        }
        // Field pictures: both fields in one access unit, then one field an access unit.
        {
            mpeg2::Sequence il;
            il.progressive = false;
            const auto field_pic = [&](mpeg2::Type type, int frame, int structure) {
                mpeg2::Picture p;
                p.type = type;
                p.temporal_reference = frame;
                p.structure = structure;
                p.top_field_first = true;
                p.progressive_frame = false;
                p.frame_pred_frame_dct = false;
                p.seed = frame;
                p.ac = true;
                return mpeg2::picture(il, p);
            };
            std::vector<std::vector<u8>> both, one;
            const mpeg2::Type types[] = {mpeg2::I, mpeg2::P, mpeg2::P};
            for (int frame = 0; frame < 3; frame++) {
                std::vector<u8> head;
                if (frame == 0) head = mpeg2::join({mpeg2::sequence_header(il), mpeg2::gop_header(0)});
                both.push_back(mpeg2::join({head, field_pic(types[frame], frame, 1), field_pic(types[frame], frame, 2)}));
                one.push_back(mpeg2::join({head, field_pic(types[frame], frame, 1)}));
                one.push_back(field_pic(types[frame], frame, 2));
            }
            variant("field pictures, a frame an access unit", both, 2200);
            variant("field pictures, a field an access unit", one, 2300);
            // A frame of an I field and a P field predicted from it; B frames of two fields; a field of the parity
            // already decoded again.
            const auto field_of = [&](mpeg2::Type type, int tr, int structure, int select) {
                mpeg2::Picture p;
                p.type = type;
                p.temporal_reference = tr;
                p.structure = structure;
                p.top_field_first = true;
                p.progressive_frame = false;
                p.frame_pred_frame_dct = false;
                p.seed = tr + structure;
                p.ac = true;
                p.field_select = select;
                return mpeg2::picture(il, p);
            };
            std::vector<std::vector<u8>> pairs;
            pairs.push_back(mpeg2::join({mpeg2::sequence_header(il), mpeg2::gop_header(0), field_of(mpeg2::I, 0, 1, -1)}));
            pairs.push_back(field_of(mpeg2::P, 0, 2, 0));
            pairs.push_back(field_of(mpeg2::P, 3, 1, -1));
            pairs.push_back(field_of(mpeg2::P, 3, 2, -1));
            for (const int tr : {1, 2}) {
                pairs.push_back(field_of(mpeg2::B, tr, 1, -1));
                pairs.push_back(field_of(mpeg2::B, tr, 2, -1));
            }
            pairs.push_back(field_of(mpeg2::P, 6, 1, -1));
            pairs.push_back(field_of(mpeg2::P, 6, 1, -1)); // the same field again
            pairs.push_back(field_of(mpeg2::P, 6, 2, -1));
            variant("field pairs", pairs, 2350);
        }
        // Macroblocks of every kind (vectors, residuals, intra and skipped macroblocks, quantiser changes).
        {
            using Edit = void (*)(mpeg2::Picture&, int);
            variant("every kind of macroblock", mpeg2::stream(seq, "IPBBPBBP", true, 0, Edit{[](mpeg2::Picture& p, int) {
                p.motion = true;
            }}), 2600);
            variant("every kind of macroblock, f_code 2", mpeg2::stream(seq, "IPBPBP", true, 3, Edit{[](mpeg2::Picture& p,
                int) {
                p.motion = true;
                p.f_code = 2;
            }}), 2620);
            variant("every kind of macroblock, f_code 3", mpeg2::stream(seq, "IPBPBP", true, 4, Edit{[](mpeg2::Picture& p,
                int) {
                p.motion = true;
                p.f_code = 3;
            }}), 2640);
            variant("every kind of macroblock, non-linear quantiser", mpeg2::stream(seq, "IPBPBP", true, 5,
                Edit{[](mpeg2::Picture& p, int i) {
                    p.motion = true;
                    p.q_scale_type = true;
                    p.intra_vlc = i % 2;
                    p.alternate_scan = i % 3 == 2;
                }}), 2660);
            mpeg2::Sequence m1, il, weighted;
            m1.mpeg1 = true;
            il.progressive = false;
            for (int k = 0; k < 64; k++) weighted.non_intra_matrix.push_back(static_cast<u8>(8 + (k * 7) % 40));
            variant("every kind of macroblock, MPEG-1", mpeg2::stream(m1, "IPBPB", true, 6, Edit{[](mpeg2::Picture& p,
                int) {
                p.motion = true;
                p.f_code = 2;
            }}), 2680);
            variant("every kind of macroblock, interlaced", mpeg2::stream(il, "IPBPB", true, 7,
                Edit{[](mpeg2::Picture& p, int) { p.motion = true; }}), 2700);
            variant("every kind of macroblock, a quantiser matrix", mpeg2::stream(weighted, "IPBPB", true, 8,
                Edit{[](mpeg2::Picture& p, int) { p.motion = true; }}), 2720);
        }
        // Closed captions in user data: ATSC's (GA94 cc_data) and DVD's, the sequence's, the GOP's and the picture's.
        {
            const std::vector<u8> atsc = {'G', 'A', '9', '4', 3, 0x43, 0xFF, 0xFC, 0x94, 0x2C, 0xFC, 0x80, 0x80, 0xFD,
                0x10, 0x20, 0xFF};
            const std::vector<u8> atsc_more = {'G', 'A', '9', '4', 3, 0x62, 0xFF, 0xFC, 0x45, 0x46, 0xFC, 0x47, 0x48,
                0xFF, 0x11, 0x22, 0x33, 0x44};
            const std::vector<u8> other = {'G', 'A', '9', '4', 6, 1, 2, 3};
            const std::vector<u8> dvd = {'C', 'C', 0x01, 0xF8, 0x84, 0xFF, 0x94, 0x20, 0xFE, 0x80, 0x80, 0xFF, 0x94,
                0x2C, 0xFE, 0x80, 0x80};
            const std::vector<u8> dvd_long = {'C', 'C', 0x01, 0xF8, 0xBF, 1, 2, 3};
            mpeg2::Sequence cc;
            cc.user_data = dvd;
            std::vector<std::vector<u8>> aus = mpeg2::stream(cc, "IPBPBP", true);
            // The GOP's (the second I picture's) and the pictures' own.
            mpeg2::Gop g{0, 0, false, false, false, atsc};
            const std::vector<u8> sh = mpeg2::sequence_header(cc);
            std::vector<u8> pic(aus[0].begin() + static_cast<std::ptrdiff_t>(sh.size() + 8), aus[0].end());
            aus[0] = mpeg2::join({sh, mpeg2::gop_header(g), pic});
            const auto with_user = [&](int tr, mpeg2::Type type, const std::vector<u8>& data) {
                mpeg2::Picture p;
                p.type = type;
                p.temporal_reference = tr;
                p.seed = tr;
                p.ac = true;
                p.user_data = data;
                return mpeg2::picture(cc, p);
            };
            aus[1] = with_user(1, mpeg2::P, atsc_more);
            aus[2] = with_user(0, mpeg2::B, other);
            aus[3] = with_user(3, mpeg2::P, dvd_long);
            aus[4] = with_user(2, mpeg2::B, dvd);
            variant("closed captions", aus, 2750);
        }
        // A broken link in a GOP that is not closed; sequence end codes (after a picture, alone); an MPEG-2 picture
        // without its coding extension; a picture whose last slice is missing; a slice outside the picture.
        {
            std::vector<std::vector<u8>> aus = mpeg2::stream(seq, "IBBPBBIBBPBBP");
            const std::vector<u8> sh = mpeg2::sequence_header(seq);
            for (usize k = 0; k < aus.size(); k++) {
                if (aus[k].size() < sh.size() + 8 || !std::equal(sh.begin(), sh.end(), aus[k].begin()) || k == 0) continue;
                std::vector<u8> pic(aus[k].begin() + static_cast<std::ptrdiff_t>(sh.size() + 8), aus[k].end());
                aus[k] = mpeg2::join({sh, mpeg2::gop_header(mpeg2::Gop{1, 0, false, false, true, {}}), pic});
            }
            variant("a broken link", aus, 2800);
            std::vector<std::vector<u8>> ends = mpeg2::stream(seq, "IPBPB");
            ends[2] = mpeg2::join({ends[2], mpeg2::sequence_end()});
            ends.push_back(mpeg2::sequence_end());
            ends.push_back(mpeg2::stream(seq, "IP")[0]);
            variant("sequence end codes", ends, 2820);
            mpeg2::Sequence m1;
            m1.mpeg1 = true;
            std::vector<std::vector<u8>> mixed = mpeg2::stream(seq, "IPPP", true);
            mpeg2::Picture p;
            p.type = mpeg2::P;
            p.temporal_reference = 2;
            p.seed = 2;
            mixed[2] = mpeg2::picture(m1, p);
            variant("no coding extension", mixed, 2840);
            std::vector<std::vector<u8>> cut = mpeg2::stream(seq, "IPBP", true);
            for (const usize k : {usize{0}, usize{1}}) {
                std::vector<u8>& au = cut[k];
                for (usize i = au.size() - 4; i > 0; i--)
                    if (!au[i] && !au[i + 1] && au[i + 2] == 1 && au[i + 3] == 4) {
                        au.resize(i);
                        break;
                    }
            }
            std::vector<u8>& outside = cut[3];
            for (usize i = 0; i + 4 < outside.size(); i++)
                if (!outside[i] && !outside[i + 1] && outside[i + 2] == 1 && outside[i + 3] == 3) {
                    outside[i + 3] = 9;
                    break;
                }
            variant("slices missing or outside", cut, 2860);
        }
        // Pictures held: how many access units the core takes before it has no frame buffer.
        if (selected("held")) {
            d.init("held: Initialize");
            const std::vector<std::vector<u8>> aus = mpeg2::stream(seq, "IPBBPBBP");
            for (usize i = 0; i < aus.size(); i++)
                d.decode(std::format("held: DecodePicture {}", i), aus[i], 2400 + static_cast<u32>(i));
            d.take("held", false, 1);
            r.r("smvd2ReleasePicture", {d.handle}, "held: ReleasePicture");
            d.decode("held: DecodePicture again", aus[4], 2410);
            d.take("held, then", true);
            r.r("smvd2ReleasePicture", {d.handle}, "held: ReleasePicture once too many");
        }
        // Discontinue, CancelDecode, a P or B picture first, damaged and cut access units, bytes that are no stream.
        if (selected("broken")) {
            const std::vector<std::vector<u8>> aus = mpeg2::stream(seq, "IPBPBP", true);
            d.init("broken: Initialize");
            d.decode("broken: a P picture first", aus[1], 2500);
            d.take("broken: a P picture first", true);
            d.decode("broken: a B picture", aus[2], 2501);
            d.take("broken: a B picture", true);
            d.decode("broken: DecodePicture 0", aus[0], 2502);
            d.decode("broken: DecodePicture 1", aus[1], 2503);
            r.r("smvd2Discontinue", {d.handle}, "broken: Discontinue");
            d.note_state("broken: Discontinue");
            d.take("broken: after Discontinue", true);
            d.decode("broken: after Discontinue: a B picture", aus[2], 2504);
            d.decode("broken: after Discontinue: a P picture", aus[3], 2505);
            d.take("broken: after Discontinue: pictures", true);
            d.decode("broken: before CancelDecode", aus[4], 2506);
            r.r("smvd2CancelDecode", {d.handle}, "broken: CancelDecode");
            d.take("broken: after CancelDecode", true);
            d.decode("broken: after CancelDecode: a P picture", aus[5], 2507);
            d.take("broken: after CancelDecode: a P picture", true);
            r.r("smvd2FlushPicture", {d.handle}, "broken: FlushPicture");
            d.take("broken: after FlushPicture", true);
            d.decode("broken: after FlushPicture: a P picture", aus[3], 2508);
            d.take("broken: after FlushPicture: a P picture", true);
            std::vector<u8> cut(aus[0].begin(), aus[0].begin() + static_cast<std::ptrdiff_t>(aus[0].size() * 2 / 3));
            d.decode("broken: an I picture cut short", cut, 2509);
            d.take("broken: an I picture cut short", true);
            std::vector<u8> damaged = aus[1];
            for (usize i = damaged.size() / 2; i < damaged.size() / 2 + 6; i++) damaged[i] ^= 0x5A;
            d.decode("broken: a damaged P picture", damaged, 2510);
            d.take("broken: a damaged P picture", true);
            std::vector<u8> junk(40);
            for (usize i = 0; i < junk.size(); i++) junk[i] = static_cast<u8>(i * 37 + 11);
            d.decode("broken: bytes that are no stream", junk, 2511);
            d.take("broken: bytes that are no stream", true);
            const std::vector<u8> headers(aus[0].begin(), aus[0].begin() + 60);
            d.decode("broken: headers alone", headers, 2512);
            d.take("broken: headers alone", true);
            r.r("smvd2FlushPicture", {d.handle}, "broken: FlushPicture 2");
            d.take("broken: after FlushPicture 2", true);
            d.flag("broken");
            d.seq("broken");
        }
        r.r("smvd2ReleaseInstance", {d.handle}, "ReleaseInstance");
    }
    r.r("smvd2ReleaseInstance", {0}, "ReleaseInstance with no instance");
    // Fresh instances: two to six SPU tasks, a level's instance (type 4), a stream larger than the instance's.
    if (selected("fresh")) {
        for (const u32 spus : {2u}) { // (six SPU tasks wait forever on SPURS of two SPUs)
            const std::string key = std::format("fresh, {} SPUs", spus);
            smvd2_param(b, 5, spus, kTs, 96, 64);
            ps_write32(b, kOut, 0);
            r.r("smvd2GetMemorySize2", {kOut, 2, kParam}, key + ": GetMemorySize2");
            const u32 n = ps_read32(b, kOut);
            std::memset(b + kMem, 0xA5, n + 0x1000);
            if (r.r("smvd2CreateInstance2", {kMem, n, 2, kParam}, key + ": CreateInstance2") != 0) continue;
            Smvd2 f{r, 0, 0, kStream, {}, false};
            f.handle = kMem;
            f.init(key + ": Initialize");
            f.stream(key, mpeg2::stream(seq, "IPBPB", true), 3000 + spus);
            r.r("smvd2ReleaseInstance", {f.handle}, key + ": ReleaseInstance");
        }
        smvd2_param(b, 4, 1, kTs);
        ps_write32(b, kOut, 0);
        r.r("smvd2GetMemorySize", {kOut, 1}, "fresh, level 1: GetMemorySize");
        const u32 n = ps_read32(b, kOut);
        std::memset(b + kMem, 0xA5, n + 0x1000);
        if (r.r("smvd2CreateInstance2", {kMem, n, 1, kParam}, "fresh, level 1: CreateInstance2") == 0) {
            Smvd2 f{r, 0, 0, kStream, {}, false};
            f.handle = kMem;
            f.init("fresh, level 1: Initialize");
            mpeg2::Sequence cif;
            cif.width = 352;
            cif.height = 288;
            f.stream("fresh, level 1: 352x288", mpeg2::stream(cif, "IPB"), 3100);
            f.stream("fresh, level 1: 352x288, every kind of macroblock", mpeg2::stream(cif, "IPBPB", true, 9,
                [](mpeg2::Picture& p, int) {
                    p.motion = true;
                    p.f_code = 2;
                }), 3105);
            mpeg2::Sequence qcif;
            qcif.width = 176;
            qcif.height = 144;
            f.stream("fresh, level 1: 176x144, every kind of macroblock", mpeg2::stream(qcif, "IPBBPB", true, 10,
                [](mpeg2::Picture& p, int) { p.motion = true; }), 3107);
            f.stream("fresh, level 1: 96x64", mpeg2::stream(seq, "IPB"), 3110);
            mpeg2::Sequence big;
            big.width = 400;
            big.height = 288;
            f.stream("fresh, level 1: 400x288", mpeg2::stream(big, "IP"), 3120);
            f.init("fresh, level 1: after the larger stream: Initialize");
            f.stream("fresh, level 1: after the larger stream", mpeg2::stream(seq, "IP"), 3130);
            r.r("smvd2ReleaseInstance", {f.handle}, "fresh, level 1: ReleaseInstance");
        }
    }
    end_spurs(r);
    return std::move(r.o);
}

} // namespace

TEST_CASE("The MPEG-2 core (libsmvd2): pspocket's own, answering as Sony's did (D-920)", "[hle][vdec-cores]")
{
    check_names("libsmvd2", {"smvd2GetMemorySize", "smvd2GetMemorySize2", "smvd2CreateInstance2",
                                "smvd2ReleaseInstance", "smvd2Initialize", "smvd2DecodePicture", "smvd2DecodePicture2",
                                "smvd2FlushPicture", "smvd2Discontinue", "smvd2CancelDecode", "smvd2SetDecodeMode",
                                "smvd2GetPicture", "smvd2GetNumPicture", "smvd2ReleasePicture", "smvd2GetSideInfoPic",
                                "smvd2GetAuxData", "smvd2GetBnrData", "smvd2GetSideInfoSeq", "smvd2GetExistenceFlag",
                                "smvd2SetAuxData", "smvd2GetVersionNumber"},
        false);
    Setup s;
    s.mode = Mode::Self;
    const Observed o = smvd2(s, false);
    // A picture's line or data by the step and what (the picture's number between them counts every stream's before).
    const auto picture_value = [&](std::string_view step, std::string_view what) {
        for (const Line& l : o.lines)
            if (l.key.starts_with(step) && l.key.ends_with(what)) return l.value;
        return std::string("(missing)");
    };
    // Sony's answers (this file's comparison, firmware 4.92).
    CHECK(o.value("GetMemorySize level 2: the size") == "0x295680");
    CHECK(o.value("GetMemorySize level 16: the size") == "0x59d300");
    CHECK(o.value("GetMemorySize2 level 2 6 SPUs 96x64: the size") == "0x3b400");
    CHECK(o.value("GetMemorySize2 level 2 1 SPUs 721x576") == "0x80615042");
    CHECK(o.value("GetVersionNumber: the version") == "0x1030000");
    CHECK(o.value("CreateInstance2 with no taskset") == "0x80615042");
    CHECK(o.value("CreateInstance2 with one byte less") == "0x80615081");
    CHECK(o.value("IPPP: DecodePicture 0: used") == "0xf1");
    CHECK(o.value("IPPP: DecodePicture 0: the flag") == "0x30103");
    CHECK(o.value("IPPP: after DecodePicture 1: picture 0: luma") == "the core's memory +0x30d00");
    CHECK(o.value("IPPP: after DecodePicture 1: picture 0: the picture") ==
        "hex:000000000000000000000000200000040000006000000040000000600000003000000000eeeeeeeeeeeeeeeeeeeeeeee");
    // The BNR data: a P picture's residual by band of slice rows; the motion of one with f_code 2.
    CHECK(o.value("IPPP: after DecodePicture 2: picture 1: BNR data")
              .starts_with("0x1 hex:0000000000000000000000000000000000000001000000020000000100000000"));
    CHECK(picture_value("every kind of macroblock, f_code 2: after DecodePicture 2: picture ", ": BNR data")
              .starts_with("0x1 hex:0000002000000330000001480000000000000002000000050000000200000000"));
    CHECK(o.value("IPPP: after FlushPicture: GetNumPicture") == "0x80615022");
    CHECK(picture_value("mode 0xff: after DecodePicture 2: picture ", ": GetPicture") == "0x80615028"); // B skipped
    CHECK(o.value("held: DecodePicture 4") == "0x80615021");          // four frame buffers, none free
    CHECK(o.value("broken: a P picture first") == "0x80615020");      // no sequence header since Initialize
    CHECK(o.value("two pieces: DecodePicture 0") == "0x80615004");    // the second piece after a gap of the first's
    CHECK(o.value("split in the headers: DecodePicture 0") == "0x80615001"); // a marker bit that is not 1
    CHECK(o.value("no coding extension: DecodePicture 2") == "0x80615004");
    CHECK(o.value("sequence end codes: DecodePicture 2: used") == "0xda");
    CHECK(o.value("sequence end codes: DecodePicture 2: the flag") == "0x1030000");
    CHECK(o.value("fresh, level 1: 400x288: DecodePicture 0") == "0x80615081");
    // The record's headers (to the display extension's offsets, which keep what earlier streams left) and its closed
    // captions (0x3A on: 12 bytes, the picture's user data, the ATSC flags, em_data and its cc_data triples).
    const std::string side = picture_value("closed captions: after DecodePicture 1: picture ", ": side information");
    CHECK(side.starts_with(
        "0x1 hex:01010000000010000000010000000100ffff00070007010f0f0f0f00030001000000000001010001050155aa"));
    CHECK(side.substr(8 + 0x3A * 2, 28) == "0c0143fffc942cfc8080fd1020ff");
    // A picture display extension with fewer offsets than its flags call for reads the next start code's bytes: the
    // first slice's (no picture decoded: damaged, with the marker bit's warning) or the user data's (left).
    CHECK(o.value("too few frame centre offsets: DecodePicture 1") == "0x80615005");
    CHECK(o.value("too few frame centre offsets, then user data: DecodePicture 1: the flag") == "0x70000");
    // Slices: none of vertical position 1 (nothing decoded), one missing, the picture's from its first slice (those
    // before it passed over), a row's slice twice.
    CHECK(o.value("slices 2 3 4: DecodePicture 0") == "0x80615004");
    CHECK(o.value("slices 1 2 4: DecodePicture 1") == "0x80615004");
    CHECK(o.value("slices 4 3 2 1: DecodePicture 1") == "0x0");
    CHECK(o.value("slices 1 2 2 3 4: DecodePicture 1") == "0x0");
    CHECK(picture_value("slices 1 2 4: after DecodePicture 2: picture ", ": BNR data")
              .starts_with("0x1 hex:0000000000000000000000000000000000000001000000010000000100000000"));
    CHECK(picture_value("IPPP, the third without its first slice: after DecodePicture 3: picture ", ": BNR data")
              .starts_with("0x1 hex:0000000000000000000000000000000000000000000000000000000000000000"));
    // Samples: Sony's slice program's inverse DCT and prediction, to the bit.
    const auto sample = [&](std::string_view step, usize at) {
        for (const Data& d : o.data)
            if (d.key.starts_with(step) && d.key.ends_with(": its frame")) return d.bytes.at(at);
        FAIL("no frame for " << step);
        return u8{0};
    };
    CHECK(sample("IPPP: after DecodePicture 1: picture 0", 0x1234) == 76);
    CHECK(sample("every kind of macroblock: after DecodePicture 2: picture ", 0x1234) == 92);
    CHECK(sample("every kind of macroblock: after DecodePicture 2: picture ", 0x2003) == 169);
    CHECK(sample("every kind of macroblock, MPEG-1: after DecodePicture 2: picture ", 0x1801) == 128);
    // A slice out of order: written from where the slice before it ended (slices 1 and 3: the third row's macroblocks
    // in the second row, the rest as cleared).
    CHECK(sample("slices 1 3: after DecodePicture 1: picture ", 6 * 0x180) ==
          sample("slices 1 2 2 3 4: after DecodePicture 1: picture ", 12 * 0x180));
    CHECK(sample("slices 1 3: after DecodePicture 1: picture ", 12 * 0x180) == 0x11);
}

TEST_CASE("The MPEG-2 core's inverse DCT is Sony's slice program's (D-920)", "[hle][vdec-cores]")
{
    // A DC coefficient alone: flat. 6 at DC and 1 at the last coefficient (MPEG-2's mismatch control's odd sum): 1 but
    // where the last basis function is lowest, rounded down there as Sony's program rounds (an exact transform gives
    // 0.51: 1).
    s16 block[64] = {};
    block[0] = 6 * 8;
    hle::mpeg2dec::idct(block);
    for (const s16 v : block) CHECK(v == 6);
    std::memset(block, 0, sizeof(block));
    block[0] = 6;
    block[63] = 1;
    hle::mpeg2dec::idct(block);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            INFO(std::format("{}, {}", x, y));
            CHECK(block[y * 8 + x] == ((x == 4 && y == 3) || (x == 3 && y == 4) ? 0 : 1));
        }
}

TEST_CASE("The MPEG-2 core (libsmvd2) against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    const Setup s = setup();
    run(s, "libsmvd2 against Sony's", [&](bool sony) { return smvd2(s, sony); }, [](const Observed&) {});
}
