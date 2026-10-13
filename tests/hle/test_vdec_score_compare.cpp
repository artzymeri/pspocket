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
#include "vc1_writer.hpp"
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

// ------------------------------------------------------------------------------------------------- VC-1

namespace v1 = pspocket::hle::test::vc1;

// svc1dCreateInstance2's and GetMemorySize2's parameters (ten words): the type (3: the core's own SPURS, 4: the memory
// for a level, 5: for a size), words 1 to 6 the type 3 settings, the taskset (word 7), the width and height (8, 9; all
// ones: the level's).
void svc1d_param(u8* b, u32 type, u32 taskset, u32 w = ~0u, u32 h = ~0u)
{
    std::memset(b + kParam, 0, 0x28);
    ps_write32(b, kParam + 0x00, type);
    ps_write32(b, kParam + 0x1C, taskset);
    ps_write32(b, kParam + 0x20, w);
    ps_write32(b, kParam + 0x24, h);
}

// An intra picture's DC levels (vc1_writer.hpp), as test_vdec_codecs.cpp's streams make them.
v1::DcPicture vc1_levels(int width, int height, int seed)
{
    v1::DcPicture p(width, height);
    for (int by = 0; by < height / 8; by++)
        for (int bx = 0; bx < width / 8; bx++)
            p.y[static_cast<usize>(by * (width / 8) + bx)] = -40 + (bx * 11 + by * 7 + seed * 23) % 80;
    for (int my = 0; my < height / 16; my++)
        for (int mx = 0; mx < width / 16; mx++) {
            p.cb[static_cast<usize>(my * (width / 16) + mx)] = -20 + (mx * 9 + my * 5 + seed * 13) % 40;
            p.cr[static_cast<usize>(my * (width / 16) + mx)] = 30 - (mx * 7 + my * 11 + seed * 5) % 50;
        }
    return p;
}

std::vector<u8> join(std::initializer_list<std::vector<u8>> parts)
{
    std::vector<u8> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

struct Svc1d {
    Run& r;
    u32 handle = 0;
    u32 taken = 0;
    u32 at = kStream;
    u32 width = 64, height = 48; // the stream's, for the frame's window
    // Whether the pictures' frames and the bytes used are compared (left out where Sony's program reads past a
    // stream's data: noted below where so).
    bool frames = true, used_compared = true;

    std::string w(u32 v) { return std::format("{:#x}", v); }

    void flag(const std::string& key)
    {
        ps_write32(r.b, kOut, 0xEEEEEEEE);
        r.r("svc1dGetExistenceFlag", {handle, kOut}, key + ": GetExistenceFlag");
        r.o.line(key + ": the flag", w(ps_read32(r.b, kOut)));
    }
    void seq(const std::string& key)
    {
        std::memset(r.b + kSeq, 0xEE, 0x40);
        r.r("svc1dGetSideInfoSeq", {handle, kSeq}, key + ": GetSideInfoSeq");
        r.o.line(key + ": the sequence", compare::hex(r.b + kSeq, 0x40));
    }
    // The instance's first 0x480 bytes (the PPU side's own state), a note.
    void state(const std::string& key)
    {
        if (!handle) return;
        r.o.note(key + ": state " + compare::hex(r.b + handle, 0x480));
    }

    // Each picture the core holds: GetNumPicture, GetPicture (its words, the addresses as offsets), its side information,
    // auxiliary data, its frame; ReleasePicture after each when `release`.
    void take(const std::string& when, bool release, u32 limit = ~0u)
    {
        u8* b = r.b;
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("svc1dGetNumPicture", {handle, kOut}, when + ": GetNumPicture");
        r.o.line(when + ": pictures", w(ps_read32(b, kOut)));
        for (u32 k = 0; k < limit; k++) {
            std::memset(b + kPic, 0xEE, 0x40);
            const std::string key = std::format("{}: picture {}", when, taken);
            const u64 answer = r.r("svc1dGetPicture", {handle, kPic}, key + ": GetPicture");
            if (ps_read32(b, kPic) == 0xEEEEEEEE && ps_read32(b, kPic + 4) == 0xEEEEEEEE) return; // none
            taken++;
            std::string words;
            for (u32 i = 0; i < 0x40; i += 4) {
                const u32 v = ps_read32(b, kPic + i);
                words += v >= kMem && v < kMem + kMemSize ? " [" + in_core(v) + "]" : std::format(" {:#x}", v);
            }
            r.o.line(key + ": the picture", words);
            std::memset(b + kSide, 0xEE, 0x200);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("svc1dGetSideInfoPic", {handle, kCount, kSide, kSide + 0x100}, key + ": GetSideInfoPic");
            r.o.line(key + ": side information", w(ps_read32(b, kCount)) + " " + compare::hex(b + kSide, 0x200));
            std::memset(b + kAuxOut, 0xEE, 0x20);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("svc1dGetAuxData", {handle, kCount, kAuxOut, kAuxOut + 0x10}, key + ": GetAuxData");
            r.o.line(key + ": auxiliary data", w(ps_read32(b, kCount)) + " " + compare::hex(b + kAuxOut, 0x20));
            const u32 luma = ps_read32(b, kPic);
            // The frame's layout: its lines (the picture's seventh word: its height to 32, or 16, and 128 of edge).
            const u32 w16 = (width + 15) & ~15u, lines = ps_read32(b, kPic + 24);
            const u32 h32 = lines - 128;
            const u32 before = lines * 16 + 64 * 16, bytes = (w16 + 32) * lines * 3 / 2;
            if (frames && lines >= 128 && lines < 0x1000 && luma >= kMem + before &&
                luma - before + bytes <= kMem + kMemSize) {
                std::vector<u8> frame(b + luma - before, b + luma - before + bytes);
                // A field picture's (its format word's 0x400): its edge strips' lines above and below the picture,
                // which Sony's program pads racing with itself (they keep a frame buffer's earlier samples now and
                // then, in either field's corners), left out.
                if (ps_read32(b, kPic + 12) & 0x400) {
                    const u32 strips = w16 / 16 + 2, clines = h32 / 2 + 64;
                    for (const u32 strip : {0u, strips - 1}) {
                        for (u32 line = 0; line < lines; line++)
                            if (line < 64 || line >= 64 + h32) std::memset(frame.data() + (strip * lines + line) * 16, 0, 16);
                        for (u32 line = 0; line < clines; line++)
                            if (line < 32 || line >= 32 + h32 / 2)
                                std::memset(frame.data() + strips * lines * 16 + (strip * clines + line) * 16, 0, 16);
                    }
                }
                r.o.bytes(key + ": its frame", frame.data(), bytes);
            }
            (void)answer;
            if (release) r.r("svc1dReleasePicture", {handle}, key + ": ReleasePicture");
        }
    }

    // DecodePicture2(handle, au {address, size, address, size}, used), the access unit in one piece; or
    // DecodePicture(handle, au {address, size}, used) when `one`.
    u64 decode(const std::string& key, const std::vector<u8>& au, u32 user, bool one = false)
    {
        u8* b = r.b;
        if (at + au.size() + 0x100 > kStream + 0x80000) at = kStream;
        for (u32 k = 0; k < 4; k++) ps_write32(b, kAux + 4 * k, (0x1000 << k) + user);
        r.r("svc1dSetAuxData", {handle, kAux}, key + ": SetAuxData");
        ps_write32(b, kStatus, 0xEEEEEEEE);
        std::memcpy(b + at, au.data(), au.size());
        std::memset(b + kAu, 0, 0x20);
        u64 v = 0;
        if (one) {
            ps_write32(b, kAu + 0, at);
            ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
            v = r.r("svc1dDecodePicture", {handle, kAu, kStatus}, key);
        } else {
            ps_write32(b, kAu + 0, at);
            ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
            v = r.r("svc1dDecodePicture2", {handle, kAu, kStatus}, key);
        }
        r.o.note(std::format("{}: {} bytes", key, au.size()));
        at = (at + static_cast<u32>(au.size()) + 0x7F) & ~0x7Fu;
        if (used_compared) r.o.line(key + ": used", w(ps_read32(b, kStatus)));
        else r.o.note(key + ": used " + w(ps_read32(b, kStatus)));
        flag(key);
        return v;
    }

    u64 init(const std::string& key) { return r.r("svc1dInitialize", {handle}, key); }

    void stream(const std::string& what, const std::vector<std::vector<u8>>& aus, u32 user0, bool one = false)
    {
        for (usize i = 0; i < aus.size(); i++) {
            decode(std::format("{}: DecodePicture {}", what, i), aus[i], user0 + static_cast<u32>(i), one);
            take(std::format("{}: after DecodePicture {}", what, i), true);
        }
        r.r("svc1dFlushPicture", {handle}, what + ": FlushPicture");
        take(what + ": after FlushPicture", true);
        state(what + ": at the end");
    }
};

// DecodePicture2 with an access unit in two pieces: the first `split` bytes, then the rest at the next 128-byte
// boundary 0x80 bytes on.
void at_two(Run& r, Svc1d& d, const std::string& key, const std::vector<u8>& au, u32 split, u32 user)
{
    u8* b = r.b;
    for (u32 k = 0; k < 4; k++) ps_write32(b, kAux + 4 * k, (0x1000 << k) + user);
    r.r("svc1dSetAuxData", {d.handle, kAux}, key + ": SetAuxData");
    const u32 a1 = kStream + 0x40000, a2 = (a1 + split + 0x80 + 0x7F) & ~0x7Fu;
    std::memcpy(b + a1, au.data(), split);
    std::memcpy(b + a2, au.data() + split, au.size() - split);
    ps_write32(b, kAu + 0, a1);
    ps_write32(b, kAu + 4, split);
    ps_write32(b, kAu + 8, a2);
    ps_write32(b, kAu + 12, static_cast<u32>(au.size() - split));
    ps_write32(b, kStatus, 0xEEEEEEEE);
    r.r("svc1dDecodePicture2", {d.handle, kAu, kStatus}, key);
    r.o.line(key + ": used", d.w(ps_read32(b, kStatus)));
    d.flag(key);
}

Observed svc1d(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libsvc1d");
    u8* b = r.b;
    make_spurs(r);
    Svc1d d{r};
    ps_write32(b, kOut, 0xEEEEEEEE);
    r.r("svc1dGetVersionNumber", {kOut}, "GetVersionNumber");
    r.o.line("GetVersionNumber: the version", d.w(ps_read32(b, kOut)));
    // The memory queries: each level's largest picture, and sizes within them.
    for (u32 level = 0; level <= 12; level++) {
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("svc1dGetMemorySize", {kOut, level}, std::format("GetMemorySize level {}", level));
        r.o.line(std::format("GetMemorySize level {}: the size", level), d.w(ps_read32(b, kOut)));
    }
    r.r("svc1dGetMemorySize", {0, 6}, "GetMemorySize with no output");
    for (const auto& [type, level, wd, ht] : {std::tuple{5u, 6u, 64u, 48u}, {5u, 6u, ~0u, ~0u}, {5u, 6u, 64u, ~0u},
             {5u, 6u, 176u, 144u}, {5u, 6u, 177u, 144u}, {5u, 6u, 0u, 48u}, {5u, 6u, 64u, 0u}, {5u, 1u, 64u, 48u},
             {5u, 3u, 64u, 48u}, {5u, 10u, 64u, 48u}, {5u, 10u, 1920u, 1088u}, {4u, 6u, 64u, 48u}, {3u, 6u, 64u, 48u},
             {2u, 6u, 64u, 48u}, {6u, 6u, 64u, 48u}}) {
        svc1d_param(b, type, 0, wd, ht);
        ps_write32(b, kOut, 0xEEEEEEEE);
        const std::string key = std::format("GetMemorySize2 type {} level {} {}x{}", type, level, static_cast<s32>(wd),
            static_cast<s32>(ht));
        r.r("svc1dGetMemorySize2", {kOut, level, kParam}, key);
        r.o.line(key + ": the size", d.w(ps_read32(b, kOut)));
    }
    r.r("svc1dGetMemorySize2", {kOut, 6, 0}, "GetMemorySize2 with no parameters");
    // CreateInstance2's refusals, then the decoder.
    svc1d_param(b, 5, kTs, 64, 48);
    ps_write32(b, kOut, 0);
    r.r("svc1dGetMemorySize2", {kOut, 6, kParam}, "GetMemorySize2 for the decoder");
    const u32 need = ps_read32(b, kOut);
    std::memset(b + kMem, 0xA5, need + 0x1000);
    r.r("svc1dCreateInstance2", {0, need, 6, kParam}, "CreateInstance2 with no memory");
    r.r("svc1dCreateInstance2", {kMem, need, 6, 0}, "CreateInstance2 with no parameters");
    for (const u32 type : {0u, 2u, 6u}) {
        svc1d_param(b, type, kTs, 64, 48);
        r.r("svc1dCreateInstance2", {kMem, need, 6, kParam}, std::format("CreateInstance2 type {}", type));
    }
    svc1d_param(b, 5, 0, 64, 48);
    r.r("svc1dCreateInstance2", {kMem, need, 6, kParam}, "CreateInstance2 with no taskset");
    svc1d_param(b, 5, kTs, 64, 48);
    r.r("svc1dCreateInstance2", {kMem, need - 1, 6, kParam}, "CreateInstance2 with one byte less");
    r.r("svc1dCreateInstance2", {kMem, need, 11, kParam}, "CreateInstance2 level 11");
    svc1d_param(b, 5, kTs, 64, 48);
    std::memset(b + kMem, 0xA5, need + 0x1000);
    if (r.r("svc1dCreateInstance2", {kMem, need, 6, kParam}, "CreateInstance2") == 0) {
        d.handle = kMem;
        d.state("CreateInstance2");
        d.flag("after CreateInstance2");
        d.seq("after CreateInstance2");
        d.init("Initialize");
        d.state("Initialize");
        d.flag("after Initialize");
        d.seq("after Initialize");
        v1::Sequence sq;
        const v1::Entry entry;
        const std::vector<u8> headers = join({v1::sequence_header(sq), v1::entry_point(entry)});
        const std::vector<std::vector<u8>> isis = {join({headers, v1::intra_frame(vc1_levels(64, 48, 0), sq)}),
            v1::skipped_frame(sq), join({headers, v1::intra_frame(vc1_levels(64, 48, 1), sq)}), v1::skipped_frame(sq)};
        d.stream("I, skipped, I, skipped", isis, 10);
        d.seq("after the stream");
        d.init("by DecodePicture: Initialize");
        d.stream("by DecodePicture", isis, 20, true);
        // Streams of other kinds, each after Initialize: the pictures' kinds, then each header field.
        const auto variant = [&](const std::string& what, const std::vector<std::vector<u8>>& aus, u32 user0) {
            if (!selected(what)) return;
            d.init(what + ": Initialize");
            d.stream(what, aus, user0);
            d.seq(what + ": at the end");
        };
        // A stream of the headers given and these pictures: I a, P (every macroblock skipped), I c, B (direct), BI d,
        // P (intensity compensation), skipped P.
        const auto kinds = [&](const v1::Sequence& s, const v1::Entry& e, const v1::Header& h) {
            const std::vector<u8> hd = join({v1::sequence_header(s), v1::entry_point(e, s)});
            v1::Header ic = h;
            ic.intensity = true;
            ic.lumscale = 16;
            ic.lumshift = 10;
            const int mbw = s.width / 16, mbh = s.height / 16;
            // (An I picture's VOPDQUANT with no per-macroblock quantizer: its macroblocks carry no MQDIFF.)
            v1::Header hi = h;
            if (hi.dqprofile == 3) hi.dquantfrm = false;
            return std::vector<std::vector<u8>>{join({hd, v1::intra_frame(vc1_levels(s.width, s.height, 0), s, e, hi)}),
                v1::skipped_mbs_p_frame(s, e, h, mbw, mbh), v1::intra_frame(vc1_levels(s.width, s.height, 1), s, e, hi),
                v1::direct_b_frame(s, e, h, mbw, mbh), v1::intra_frame(vc1_levels(s.width, s.height, 2), s, e, hi, true),
                v1::skipped_mbs_p_frame(s, e, ic, mbw, mbh), v1::skipped_frame(s, e, h)};
        };
        const v1::Header plain;
        variant("picture kinds", kinds(sq, entry, plain), 100);
        using SeqEdit = void (*)(v1::Sequence&);
        using EntryEdit = void (*)(v1::Entry&);
        using HeaderEdit = void (*)(v1::Header&);
        const auto with = [&](const std::string& what, SeqEdit se, EntryEdit ee, HeaderEdit he, u32 user0) {
            v1::Sequence s2;
            v1::Entry e2;
            v1::Header h2;
            if (se) se(s2);
            if (ee) ee(e2);
            if (he) he(h2);
            variant(what, kinds(s2, e2, h2), user0);
        };
        with("level 3, post-processing hints", [](v1::Sequence& s2) {
            s2.level = 3;
            s2.frmrtq_postproc = 5;
            s2.bitrtq_postproc = 17;
        }, nullptr, nullptr, 200);
        with("post-processing", [](v1::Sequence& s2) { s2.postprocflag = true; }, nullptr,
            [](v1::Header& h2) { h2.postproc = 2; }, 210);
        with("pulldown, repeated frames", [](v1::Sequence& s2) { s2.pulldown = true; }, nullptr,
            [](v1::Header& h2) { h2.rptfrm = 3; }, 220);
        with("interlaced sequence, progressive frames", [](v1::Sequence& s2) { s2.interlace = true; }, nullptr,
            [](v1::Header& h2) { h2.uvsamp = true; }, 230);
        with("interlaced, pulldown", [](v1::Sequence& s2) {
            s2.interlace = true;
            s2.pulldown = true;
        }, nullptr, [](v1::Header& h2) {
            h2.tff = false;
            h2.rff = true;
        }, 240);
        with("frame counter", [](v1::Sequence& s2) { s2.tfcntrflag = true; }, nullptr,
            [](v1::Header& h2) { h2.tfcntr = 0x5A; }, 250);
        with("frame interpolation", [](v1::Sequence& s2) { s2.finterpflag = true; }, nullptr,
            [](v1::Header& h2) { h2.interpfrm = true; }, 260);
        with("display extension", [](v1::Sequence& s2) {
            s2.display_width = 60;
            s2.display_height = 40;
            s2.aspect_ratio = 15;
            s2.aspect_width = 16;
            s2.aspect_height = 9;
            s2.framerate_nr = 3;
            s2.framerate_dr = 2;
            s2.color_prim = 1;
            s2.transfer_char = 6;
            s2.matrix_coef = 7;
        }, nullptr, nullptr, 270);
        with("display extension, frame rate exponent, aspect 3", [](v1::Sequence& s2) {
            s2.display_width = 64;
            s2.display_height = 48;
            s2.aspect_ratio = 3;
            s2.framerate_exp = 0x3BF;
        }, nullptr, nullptr, 280);
        with("HRD", [](v1::Sequence& s2) {
            s2.hrd_buckets = 2;
            s2.hrd_rate_exp = 5;
            s2.hrd_buffer_exp = 6;
        }, [](v1::Entry& e2) { e2.hrd_full = 0x40; }, nullptr, 290);
        with("an open entry point, broken link", nullptr, [](v1::Entry& e2) {
            e2.closed_entry = false;
            e2.broken_link = true;
        }, nullptr, 300);
        with("pan-scan flag, no windows", nullptr, [](v1::Entry& e2) { e2.panscan = true; }, nullptr, 310);
        with("pan-scan windows", nullptr, [](v1::Entry& e2) { e2.panscan = true; }, [](v1::Header& h2) {
            h2.ps_present = true;
            h2.windows = {{-0x100, 0x200, 600, 400}};
        }, 320);
        with("reference distances, loop filter, fast UV", nullptr, [](v1::Entry& e2) {
            e2.refdist = true;
            e2.loopfilter = true;
            e2.fastuvmc = true;
        }, nullptr, 330);
        with("extended vectors", nullptr, [](v1::Entry& e2) {
            e2.extended_mv = true;
            e2.extended_dmv = true;
        }, [](v1::Header& h2) { h2.mvrange = 2; }, 340);
        with("DQUANT 1, variable transform", nullptr, [](v1::Entry& e2) {
            e2.dquant = 1;
            e2.vstransform = true;
        }, nullptr, 350);
        with("overlap, PQINDEX 6, all overlapped", nullptr, [](v1::Entry& e2) { e2.overlap = true; },
            [](v1::Header& h2) {
                h2.pqindex = 6;
                h2.condover = 1;
            }, 360);
        with("explicit quantizer, uniform", nullptr, [](v1::Entry& e2) { e2.quantizer = 1; },
            [](v1::Header& h2) { h2.pquantizer = true; }, 370);
        with("non-uniform quantizer", nullptr, [](v1::Entry& e2) { e2.quantizer = 2; }, nullptr, 380);
        with("coded size", nullptr, [](v1::Entry& e2) {
            e2.coded_width = 64;
            e2.coded_height = 48;
        }, nullptr, 390);
        with("range mapping", nullptr, [](v1::Entry& e2) {
            e2.range_mapy = 5;
            e2.range_mapuv = 2;
        }, nullptr, 400);
        with("rounding, half step, AC tables", nullptr, nullptr, [](v1::Header& h2) {
            h2.rndctrl = true;
            h2.halfqp = true;
            h2.transacfrm = 2;
            h2.transacfrm2 = 1;
            h2.transdctab = true;
        }, 410);
        with("AC and DC tables a", nullptr, nullptr, [](v1::Header& h2) {
            h2.transacfrm = 2;
            h2.transacfrm2 = 0;
            h2.transdctab = true;
        }, 412);
        with("AC and DC tables b", nullptr, nullptr, [](v1::Header& h2) {
            h2.transacfrm = 0;
            h2.transacfrm2 = 2;
            h2.transdctab = false;
        }, 414);
        with("AC and DC tables c", nullptr, nullptr, [](v1::Header& h2) {
            h2.transacfrm = 1;
            h2.transacfrm2 = 2;
            h2.transdctab = true;
        }, 416);
        with("PQINDEX 20, B fraction 3/5", nullptr, nullptr, [](v1::Header& h2) {
            h2.pqindex = 20;
            h2.bfraction = 7;
        }, 420);
        // The P and B pictures' fields: each motion vector mode (both tables: PQUANT up to 12, above), the tables,
        // the transform types, the quantizer's VOPDQUANT.
        {
            const std::vector<std::pair<const char*, HeaderEdit>> modes = {
                {"mixed", [](v1::Header& h2) { h2.mvmode = v1::kMixedMv; }},
                {"half-pel", [](v1::Header& h2) { h2.mvmode = v1::kHalfPel; }},
                {"half-pel bilinear", [](v1::Header& h2) {
                    h2.mvmode = v1::kHalfPelBilinear;
                    h2.b_hpel_bilinear = true;
                }},
                {"1MV, PQINDEX 20", [](v1::Header& h2) { h2.pqindex = 20; }},
                {"mixed, PQINDEX 20", [](v1::Header& h2) {
                    h2.mvmode = v1::kMixedMv;
                    h2.pqindex = 20;
                }},
                {"half-pel, PQINDEX 20", [](v1::Header& h2) {
                    h2.mvmode = v1::kHalfPel;
                    h2.pqindex = 20;
                }},
                {"half-pel bilinear, PQINDEX 20", [](v1::Header& h2) {
                    h2.mvmode = v1::kHalfPelBilinear;
                    h2.pqindex = 20;
                }},
                {"tables", [](v1::Header& h2) {
                    h2.mvtab = 3;
                    h2.cbptab = 2;
                }},
            };
            u32 user = 430;
            for (const auto& [name, edit] : modes) with(std::string("motion vector mode ") + name, nullptr, nullptr, edit,
                user += 10);
            const auto vs = [](v1::Entry& e2) { e2.vstransform = true; };
            with("transform per macroblock", nullptr, vs, [](v1::Header& h2) { h2.ttmbf = false; }, 520);
            with("transform 8x4", nullptr, vs, [](v1::Header& h2) { h2.ttfrm = 1; }, 530);
            with("transform 4x8", nullptr, vs, [](v1::Header& h2) { h2.ttfrm = 2; }, 540);
            with("transform 4x4", nullptr, vs, [](v1::Header& h2) { h2.ttfrm = 3; }, 550);
            with("transform per macroblock after 4x4", nullptr, vs, [](v1::Header& h2) { h2.ttmbf = false; }, 555);
            const auto dq1 = [](v1::Entry& e2) { e2.dquant = 1; };
            with("VOPDQUANT all four edges", nullptr, dq1, [](v1::Header& h2) {
                h2.dquantfrm = true;
                h2.pqdiff = 2;
            }, 560);
            with("VOPDQUANT double edges, ABSPQ", nullptr, dq1, [](v1::Header& h2) {
                h2.dquantfrm = true;
                h2.dqprofile = 1;
                h2.dqedge = 3;
                h2.pqdiff = 7;
                h2.abspq = 9;
            }, 570);
            with("VOPDQUANT single edge", nullptr, dq1, [](v1::Header& h2) {
                h2.dquantfrm = true;
                h2.dqprofile = 2;
                h2.dqedge = 1;
                h2.pqdiff = 5;
            }, 580);
            with("VOPDQUANT every macroblock, two levels", nullptr, dq1, [](v1::Header& h2) {
                h2.dquantfrm = true;
                h2.dqprofile = 3;
                h2.pqdiff = 1;
            }, 590);
            with("VOPDQUANT every macroblock", nullptr, dq1, [](v1::Header& h2) {
                h2.dquantfrm = true;
                h2.dqprofile = 3;
                h2.dqbilevel = false;
            }, 600);
            with("VOPDQUANT of DQUANT 2", nullptr, [](v1::Entry& e2) { e2.dquant = 2; },
                [](v1::Header& h2) { h2.pqdiff = 3; }, 610);
            with("VOPDQUANT of DQUANT 2, ABSPQ", nullptr, [](v1::Entry& e2) { e2.dquant = 2; }, [](v1::Header& h2) {
                h2.pqdiff = 7;
                h2.abspq = 20;
            }, 620);
            // DQUANT 2 after a single edge's profile (which fields it leaves as they were).
            with("VOPDQUANT single edge again", nullptr, dq1, [](v1::Header& h2) {
                h2.dquantfrm = true;
                h2.dqprofile = 2;
                h2.dqedge = 2;
                h2.pqdiff = 4;
            }, 630);
            with("VOPDQUANT of DQUANT 2 again", nullptr, [](v1::Entry& e2) { e2.dquant = 2; },
                [](v1::Header& h2) { h2.pqdiff = 6; }, 640);
            with("VOPDQUANT none after DQUANT 2", nullptr, nullptr, nullptr, 650);
            // DQUANT 2 with PQDIFF 0 (the edges' quantizer one more: PQINDEX 5's DC step 8, 6's 9), ABSPQ the
            // picture's own quantizer.
            with("VOPDQUANT of DQUANT 2, PQDIFF 0, PQINDEX 5", nullptr, [](v1::Entry& e2) { e2.dquant = 2; },
                [](v1::Header& h2) { h2.pqindex = 5; }, 660);
            with("VOPDQUANT of DQUANT 2, ABSPQ its own", nullptr, [](v1::Entry& e2) { e2.dquant = 2; },
                [](v1::Header& h2) {
                    h2.pqdiff = 7;
                    h2.abspq = 4;
                }, 670);
            with("VOPDQUANT of DQUANT 2, PQDIFF 1", nullptr, [](v1::Entry& e2) { e2.dquant = 2; },
                [](v1::Header& h2) { h2.pqdiff = 1; }, 675);
            with("VOPDQUANT all four edges, PQDIFF 0", nullptr, dq1, [](v1::Header& h2) { h2.dquantfrm = true; }, 680);
            with("VOPDQUANT all four edges, ABSPQ its own", nullptr, dq1, [](v1::Header& h2) {
                h2.dquantfrm = true;
                h2.pqdiff = 7;
                h2.abspq = 4;
            }, 690);
            with("VOPDQUANT every macroblock, two levels, PQDIFF 0", nullptr, dq1, [](v1::Header& h2) {
                h2.dquantfrm = true;
                h2.dqprofile = 3;
            }, 695);
            // Two levels, then each other profile (which fields the record keeps).
            {
                using Edit = void (*)(v1::Header&);
                const std::vector<std::pair<const char*, Edit>> after = {
                    {"single edge", [](v1::Header& h2) {
                        h2.dquantfrm = true;
                        h2.dqprofile = 2;
                        h2.dqedge = 0;
                    }},
                    {"double edges", [](v1::Header& h2) {
                        h2.dquantfrm = true;
                        h2.dqprofile = 1;
                        h2.dqedge = 1;
                    }},
                    {"all four edges", [](v1::Header& h2) {
                        h2.dquantfrm = true;
                        h2.pqdiff = 6;
                    }},
                    {"none", [](v1::Header&) {}},
                };
                u32 user2 = 700;
                for (const auto& [name, edit] : after) {
                    with(std::string("VOPDQUANT two levels before ") + name, nullptr, dq1, [](v1::Header& h2) {
                        h2.dquantfrm = true;
                        h2.dqprofile = 3;
                        h2.pqdiff = 2;
                    }, user2 += 10);
                    with(std::string("VOPDQUANT two levels before ") + name + ": then", nullptr, dq1, edit, user2 += 10);
                }
            }
        }
        // Slices, user data.
        {
            const std::vector<u8> hd3 = join({v1::sequence_header(sq), v1::entry_point(entry)});
            const std::vector<u8> pskip3 = v1::skipped_mbs_p_frame(sq, entry, plain, 4, 3);
            variant("slices", {join({hd3, v1::intra_frame_slices(vc1_levels(64, 48, 90), sq, entry, plain, {1})}), pskip3,
                v1::intra_frame_slices(vc1_levels(64, 48, 91), sq, entry, plain, {1, 2}),
                v1::intra_frame_slices(vc1_levels(64, 48, 92), sq, entry, plain, {2}), pskip3}, 1500);
            variant("user data", {join({hd3, v1::user_data({1, 2, 3}), v1::intra_frame(vc1_levels(64, 48, 93), sq, entry,
                plain)}), join({pskip3, v1::user_data({4, 5})}), v1::user_data({6, 7, 8, 9}),
                join({v1::user_data({0x0A}, 0x1F), v1::user_data({0x0B}, 0x1E), pskip3})}, 1510);
            // Each level's user data before a frame and after it, among the headers.
            std::vector<std::vector<u8>> levels = {join({hd3, v1::intra_frame(vc1_levels(64, 48, 94), sq, entry, plain)})};
            for (const u8 suffix : {0x1F, 0x1E, 0x1D, 0x1C, 0x1B}) {
                levels.push_back(join({v1::user_data({suffix}, suffix), pskip3}));
                levels.push_back(join({pskip3, v1::user_data({suffix}, suffix)}));
            }
            levels.push_back(join({v1::sequence_header(sq), v1::user_data({0x21}, 0x1F), v1::entry_point(entry),
                v1::user_data({0x22}, 0x1E), v1::intra_frame(vc1_levels(64, 48, 95), sq, entry, plain),
                v1::user_data({0x23}, 0x1D)}));
            levels.push_back(join({pskip3, v1::end_of_sequence(), v1::user_data({0x24}, 0x1D)}));
            levels.push_back(join({pskip3, v1::user_data({0x25}, 0x1D), pskip3}));
            variant("user data of each level", levels, 1520);
        }
        // The bitplanes in each coding mode (their tiles and what is left of them at other sizes).
        {
            const std::vector<std::pair<const char*, int>> modes = {{"NORM-2", v1::kNorm2}, {"DIFF-2", v1::kDiff2},
                {"NORM-6", v1::kNorm6}, {"DIFF-6", v1::kDiff6}, {"column skip", v1::kColSkip}};
            u32 user = 1400;
            for (const auto& [name, mode] : modes) {
                v1::Header hb2;
                hb2.bitplane = mode;
                hb2.mvmode = v1::kMixedMv;
                variant(std::string("bitplanes ") + name, kinds(sq, entry, hb2), user += 10);
            }
            for (const auto& [w2, h2s] : {std::pair{48, 48}, {48, 32}, {16, 48}, {32, 16}}) {
                for (const int mode : {v1::kNorm6, v1::kDiff2}) {
                    v1::Sequence s2;
                    s2.width = w2;
                    s2.height = h2s;
                    v1::Header hb2;
                    hb2.bitplane = mode;
                    hb2.mvmode = v1::kMixedMv;
                    d.width = static_cast<u32>(w2);
                    d.height = static_cast<u32>(h2s);
                    variant(std::format("bitplanes {} {}x{}", mode == v1::kNorm6 ? "NORM-6" : "DIFF-2", w2, h2s),
                        kinds(s2, entry, hb2), user += 10);
                }
            }
            d.width = 64;
            d.height = 48;
        }
        // Interlaced pictures (64x32: each field one row of macroblocks): interlaced frames of each kind, field
        // pictures of each pair of I and P fields, with the fields' order and repeats.
        {
            d.width = 64;
            d.height = 32;
            const auto ilace = [&](const std::string& what, const v1::Sequence& s2, const v1::Entry& e2,
                                   const v1::Header& h2, u32 user0) {
                const std::vector<u8> hd2 = join({v1::sequence_header(s2), v1::entry_point(e2, s2)});
                const auto lv = [](int seed) { return vc1_levels(64, 32, seed); };
                const auto fl = [](int seed) { return vc1_levels(64, 16, seed); };
                variant(what + ": frames", {join({hd2, v1::interlaced_intra_frame(lv(40), s2, e2, h2)}),
                    v1::interlaced_skipped_p_frame(s2, e2, h2, 4, 2), v1::interlaced_intra_frame(lv(41), s2, e2, h2),
                    v1::interlaced_direct_b_frame(s2, e2, h2, 4, 2),
                    v1::interlaced_intra_frame(lv(42), s2, e2, h2, true), v1::interlaced_skipped_p_frame(s2, e2, h2, 4,
                    2), v1::skipped_frame(s2, e2, h2)}, user0);
                variant(what + ": fields", {join({hd2, v1::field_picture(0, fl(43), fl(44), s2, e2, h2)}),
                    v1::field_picture(1, fl(45), fl(46), s2, e2, h2), v1::field_picture(3, fl(0), fl(0), s2, e2, h2),
                    v1::field_picture(2, fl(47), fl(48), s2, e2, h2)}, user0 + 10);
                variant(what + ": B fields", {join({hd2, v1::field_picture(0, fl(54), fl(55), s2, e2, h2)}),
                    v1::field_picture(0, fl(56), fl(57), s2, e2, h2), v1::field_picture(4, fl(0), fl(0), s2, e2, h2),
                    v1::field_picture(5, fl(0), fl(58), s2, e2, h2), v1::field_picture(6, fl(59), fl(0), s2, e2, h2),
                    v1::field_picture(7, fl(60), fl(61), s2, e2, h2), v1::field_picture(3, fl(0), fl(0), s2, e2, h2)},
                    user0 + 15);
                variant(what + ": progressive, interlaced, fields", {join({hd2, v1::intra_frame(lv(49), s2, e2, h2)}),
                    v1::interlaced_skipped_p_frame(s2, e2, h2, 4, 2), v1::field_picture(3, fl(0), fl(0), s2, e2, h2),
                    v1::skipped_mbs_p_frame(s2, e2, h2, 4, 2)}, user0 + 20);
                // Skipped pictures after a progressive frame, an interlaced one, a field picture.
                variant(what + ": skipped pictures", {join({hd2, v1::intra_frame(lv(50), s2, e2, h2)}),
                    v1::skipped_frame(s2, e2, h2), v1::interlaced_intra_frame(lv(51), s2, e2, h2),
                    v1::skipped_frame(s2, e2, h2), v1::field_picture(0, fl(52), fl(53), s2, e2, h2),
                    v1::skipped_frame(s2, e2, h2), v1::skipped_frame(s2, e2, h2)}, user0 + 25);
            };
            v1::Sequence si;
            si.width = 64;
            si.height = 32;
            si.interlace = true;
            const v1::Entry ei;
            const v1::Header hi;
            ilace("interlaced pictures", si, ei, hi, 900);
            v1::Sequence sp = si;
            sp.pulldown = true;
            v1::Header hb;
            hb.tff = false;
            hb.rff = true;
            ilace("interlaced pictures, bottom field first, repeated", sp, ei, hb, 930);
            v1::Entry er;
            er.refdist = true;
            er.extended_mv = true;
            er.extended_dmv = true;
            v1::Header hr;
            hr.refdist = 2;
            hr.reffield = true;
            hr.mvrange = 1;
            hr.dmvrange = 2;
            hr.mbmodetab = 1;
            hr.imvtab = 2;
            hr.icbptab = 5;
            hr.twomvbptab = 3;
            ilace("interlaced pictures, reference distance, ranges, tables", si, er, hr, 960);
            v1::Header hf;
            hf.fourmvswitch = true;
            hf.fourmvbptab = 2;
            hf.intcomp = true;
            hf.lumscale = 20;
            hf.lumshift = 7;
            hf.numref = true;
            hf.imvtab = 5;
            ilace("interlaced pictures, 4MV switch, intensity compensation, two references", si, ei, hf, 990);
            v1::Header hn6;
            hn6.bitplane = v1::kNorm6;
            ilace("interlaced pictures, NORM-6 bitplanes", si, ei, hn6, 1020);
            v1::Entry eps;
            eps.panscan = true;
            v1::Header hps;
            hps.ps_present = true;
            hps.windows = {{0x10, -0x20, 50, 30}, {-0x30, 0x40, 60, 20}};
            ilace("interlaced pictures, pan-scan", si, eps, hps, 1030);
            v1::Header hps3 = hps;
            hps3.rff = true;
            hps3.windows.push_back({0x50, 0x60, 40, 10});
            ilace("interlaced pictures, pan-scan, repeated", sp, eps, hps3, 1040);
            // Field pictures' P fields by their reference fields: each edge of the frame buffer as they leave it.
            {
                const auto fl = [](int seed) { return vc1_levels(64, 16, seed); };
                const auto fields = [&](const std::string& what, const v1::Sequence& s2, const v1::Entry& e2,
                                        const v1::Header& h2, u32 user0) {
                    const std::vector<u8> hd2 = join({v1::sequence_header(s2), v1::entry_point(e2, s2)});
                    variant(what, {join({hd2, v1::field_picture(0, fl(60), fl(61), s2, e2, h2)}),
                        v1::field_picture(3, fl(0), fl(0), s2, e2, h2), v1::field_picture(2, fl(62), fl(63), s2, e2, h2),
                        v1::field_picture(1, fl(64), fl(65), s2, e2, h2), v1::field_picture(0, fl(66), fl(67), s2, e2, h2),
                        v1::field_picture(2, fl(68), fl(69), s2, e2, h2), v1::field_picture(3, fl(0), fl(0), s2, e2, h2)},
                        user0);
                };
                v1::Header r1;
                r1.reffield = true;
                fields("interlaced fields, the other reference field", si, ei, r1, 1100);
                v1::Header n1;
                n1.numref = true;
                fields("interlaced fields, two reference fields", si, ei, n1, 1110);
                v1::Entry rd;
                rd.refdist = true;
                v1::Header r2;
                r2.refdist = 2;
                fields("interlaced fields, a reference distance", si, rd, r2, 1120);
                v1::Entry xm;
                xm.extended_mv = true;
                xm.extended_dmv = true;
                fields("interlaced fields, extended vectors", si, xm, ei.closed_entry ? v1::Header{} : v1::Header{}, 1130);
                v1::Header rb = r1;
                rb.tff = false;
                fields("interlaced fields, the other reference field, bottom first", sp, ei, rb, 1140);
                // A P field into a frame buffer that held another picture: I/I a, b, c, then each kind.
                const auto into = [&](const std::string& what, const v1::Header& h2, int fptype, u32 user0) {
                    const std::vector<u8> hd2 = join({v1::sequence_header(si), v1::entry_point(ei, si)});
                    variant(what, {join({hd2, v1::field_picture(0, fl(70), fl(71), si, ei, h2)}),
                        v1::field_picture(0, fl(72), fl(73), si, ei, h2), v1::field_picture(0, fl(74), fl(75), si, ei, h2),
                        v1::field_picture(fptype, fl(76), fl(77), si, ei, h2)}, user0);
                };
                const v1::Header h0;
                into("interlaced fields into a used frame buffer: P/I", h0, 2, 1150);
                into("interlaced fields into a used frame buffer: I/P", h0, 1, 1160);
                into("interlaced fields into a used frame buffer: P/P", h0, 3, 1170);
                into("interlaced fields into a used frame buffer: P/I, the other reference field", r1, 2, 1180);
                into("interlaced fields into a used frame buffer: I/P, the other reference field", r1, 1, 1190);
                into("interlaced fields into a used frame buffer: P/P, the other reference field", r1, 3, 1200);
                const auto into2 = [&](const std::string& what, const v1::Entry& e2, const v1::Header& h2, u32 user0) {
                    const std::vector<u8> hd2 = join({v1::sequence_header(si), v1::entry_point(e2, si)});
                    variant(what, {join({hd2, v1::field_picture(0, fl(70), fl(71), si, e2, h2)}),
                        v1::field_picture(0, fl(72), fl(73), si, e2, h2), v1::field_picture(0, fl(74), fl(75), si, e2, h2),
                        v1::field_picture(2, fl(76), fl(77), si, e2, h2)}, user0);
                };
                v1::Entry emv;
                emv.extended_mv = true;
                v1::Header hmv;
                hmv.mvrange = 1;
                into2("interlaced fields into a used frame buffer: P/I, MVRANGE 1", emv, hmv, 1210);
                v1::Entry edmv = emv;
                edmv.extended_dmv = true;
                v1::Header hdmv;
                hdmv.dmvrange = 2;
                into2("interlaced fields into a used frame buffer: P/I, DMVRANGE 2", edmv, hdmv, 1220);
                v1::Header hboth = hmv;
                hboth.dmvrange = 2;
                into2("interlaced fields into a used frame buffer: P/I, both ranges", edmv, hboth, 1230);
                v1::Header htab;
                htab.imvtab = 2;
                htab.icbptab = 5;
                into2("interlaced fields into a used frame buffer: P/I, tables", ei, htab, 1240);
                v1::Entry erd;
                erd.refdist = true;
                v1::Header hrd = r1;
                hrd.refdist = 2;
                into2("interlaced fields into a used frame buffer: P/I, reference distance, other field", erd, hrd, 1250);
                v1::Header hall = hr;
                into2("interlaced fields into a used frame buffer: P/I, all", er, hall, 1260);
                const auto chain = [&](const std::string& what, const v1::Entry& e2, const v1::Header& h2,
                                       std::initializer_list<int> types, u32 user0) {
                    const std::vector<u8> hd2 = join({v1::sequence_header(si), v1::entry_point(e2, si)});
                    std::vector<std::vector<u8>> aus;
                    int seed = 80;
                    for (const int t : types) {
                        std::vector<u8> au = v1::field_picture(t, fl(seed), fl(seed + 1), si, e2, h2);
                        seed += 2;
                        aus.push_back(aus.empty() ? join({hd2, au}) : au);
                    }
                    variant(what, aus, user0);
                };
                chain("interlaced field chains: all, I/I I/P P/P P/I", er, hr, {0, 1, 3, 2}, 1270);
                chain("interlaced field chains: other field, I/I I/P P/P P/I", ei, r1, {0, 1, 3, 2}, 1280);
                v1::Header h3 = r1;
                h3.refdist = 2;
                chain("interlaced field chains: distance, other field, I/I I/P P/P P/I", erd, h3, {0, 1, 3, 2}, 1290);
                chain("interlaced field chains: all, I/I I/P I/I P/I", er, hr, {0, 1, 0, 2}, 1300);
                chain("interlaced field chains: all, I/I I/I P/P P/I", er, hr, {0, 0, 3, 2}, 1310);
                chain("interlaced field chains: plain, I/I I/P P/P P/I", ei, v1::Header{}, {0, 1, 3, 2}, 1320);
            }
            d.width = 64;
            d.height = 48;
        }
        // PROBE: each entry point field alone, the defaults between.
        {
            const std::vector<std::pair<const char*, EntryEdit>> fields = {
                {"loopfilter", [](v1::Entry& e2) { e2.loopfilter = true; }},
                {"fastuvmc", [](v1::Entry& e2) { e2.fastuvmc = true; }},
                {"refdist", [](v1::Entry& e2) { e2.refdist = true; }},
                {"extended mv", [](v1::Entry& e2) { e2.extended_mv = true; }},
                {"extended dmv", [](v1::Entry& e2) {
                    e2.extended_mv = true;
                    e2.extended_dmv = true;
                }},
                {"dquant 1", [](v1::Entry& e2) { e2.dquant = 1; }},
                {"dquant 2", [](v1::Entry& e2) { e2.dquant = 2; }},
                {"dquant 3", [](v1::Entry& e2) { e2.dquant = 3; }},
                {"vstransform", [](v1::Entry& e2) { e2.vstransform = true; }},
                {"overlap", [](v1::Entry& e2) { e2.overlap = true; }},
                {"quantizer 1", [](v1::Entry& e2) { e2.quantizer = 1; }},
                {"quantizer 2", [](v1::Entry& e2) { e2.quantizer = 2; }},
                {"quantizer 3", [](v1::Entry& e2) { e2.quantizer = 3; }},
                {"coded size", [](v1::Entry& e2) {
                    e2.coded_width = 64;
                    e2.coded_height = 48;
                }},
                {"panscan", [](v1::Entry& e2) { e2.panscan = true; }},
            };
            u32 user = 500;
            for (const auto& [name, edit] : fields) {
                with(std::string("PROBE ") + name, nullptr, edit, nullptr, user += 10);
                with(std::string("PROBE reset after ") + name, nullptr, nullptr, nullptr, user += 10);
            }
        }
        // The picture queue: pictures not taken, a flush, CancelDecode, Discontinue and Initialize inside a stream.
        const std::vector<u8> hd = join({v1::sequence_header(sq), v1::entry_point(entry)});
        const auto intra = [&](int seed) { return v1::intra_frame(vc1_levels(64, 48, seed), sq, entry, plain); };
        const std::vector<u8> pskip = v1::skipped_mbs_p_frame(sq, entry, plain, 4, 3),
                              bdirect = v1::direct_b_frame(sq, entry, plain, 4, 3);
        if (selected("held")) {
            d.init("held: Initialize");
            const std::vector<std::vector<u8>> aus = {join({hd, intra(3)}), pskip, pskip, pskip, pskip, pskip, pskip,
                pskip};
            for (usize i = 0; i < aus.size(); i++) {
                const std::string key = std::format("held: DecodePicture {}", i);
                d.decode(key, aus[i], 600 + static_cast<u32>(i));
                ps_write32(b, kOut, 0xEEEEEEEE);
                r.r("svc1dGetNumPicture", {d.handle, kOut}, key + ": GetNumPicture");
                r.o.line(key + ": pictures", d.w(ps_read32(b, kOut)));
            }
            d.take("held: one taken", true, 1);
            d.decode("held: DecodePicture after one taken", pskip, 620);
            d.take("held: the rest", true);
            r.r("svc1dFlushPicture", {d.handle}, "held: FlushPicture");
            d.take("held: after FlushPicture", true);
            // Taken and not released: what GetNumPicture counts, ReleasePicture in turn.
            d.init("held, not released: Initialize");
            for (usize i = 0; i < 4; i++) {
                d.decode(std::format("held, not released: DecodePicture {}", i), i ? pskip : join({hd, intra(4)}),
                    630 + static_cast<u32>(i));
                d.take(std::format("held, not released: after DecodePicture {}", i), false);
            }
            for (u32 k = 0; k < 4; k++)
                r.r("svc1dReleasePicture", {d.handle}, std::format("held, not released: ReleasePicture {}", k));
            ps_write32(b, kOut, 0xEEEEEEEE);
            r.r("svc1dGetNumPicture", {d.handle, kOut}, "held, not released: GetNumPicture");
        }
        if (selected("flush in the stream")) {
            d.init("flush in the stream: Initialize");
            d.decode("flush in the stream: DecodePicture 0", join({hd, intra(5)}), 700);
            d.decode("flush in the stream: DecodePicture 1", pskip, 701);
            r.r("svc1dFlushPicture", {d.handle}, "flush in the stream: FlushPicture");
            d.take("flush in the stream: after FlushPicture", true);
            d.decode("flush in the stream: DecodePicture 2", pskip, 702);
            d.decode("flush in the stream: DecodePicture 3", intra(6), 703);
            d.decode("flush in the stream: DecodePicture 4", bdirect, 704);
            d.take("flush in the stream: after DecodePicture 4", true);
            r.r("svc1dFlushPicture", {d.handle}, "flush in the stream: FlushPicture 2");
            r.r("svc1dFlushPicture", {d.handle}, "flush in the stream: FlushPicture 3");
            d.take("flush in the stream: after FlushPicture 3", true);
            // A skipped picture with no anchor before it, P pictures after it; a B picture with no anchor after it,
            // one with none before it.
            const std::vector<std::vector<u8>> after = {v1::skipped_frame(sq), pskip, pskip, {}, bdirect, intra(20),
                bdirect, pskip, bdirect, {}};
            for (usize i = 0; i < after.size(); i++) {
                const std::string key = std::format("flush in the stream: DecodePicture {}", 5 + i);
                if (after[i].empty()) {
                    r.r("svc1dFlushPicture", {d.handle}, key + ": FlushPicture");
                } else {
                    d.decode(key, after[i], 705 + static_cast<u32>(i));
                }
                d.take(key + ": after", true);
            }
        }
        // Pictures predicted from anchors that are gone (after FlushPicture), or that had a warning: where their samples
        // come from, and their statuses.
        if (selected("references")) {
            // An empty access unit: FlushPicture; one of a single byte 1: one picture taken and not released (in
            // the parts that hold one, no pictures taken but that).
            bool hold = false;
            const auto step = [&](const std::string& key, const std::vector<u8>& au, u32 user) {
                if (au.empty()) {
                    r.r("svc1dFlushPicture", {d.handle}, key + ": FlushPicture");
                } else if (au.size() == 1) {
                    d.take(key, false, 1);
                    return;
                } else {
                    d.decode(key, au, user);
                }
                if (!hold) d.take(key, true);
            };
            const std::vector<std::pair<std::string, std::vector<std::vector<u8>>>> parts = {
                // A P picture with no anchor: its frame buffer held I a, the anchor before the flush is I b.
                {"references: a P picture with no anchor", {join({hd, intra(21)}), pskip, intra(22), {}, pskip, {}}},
                // A B picture with no anchor after it.
                {"references: a B picture with no anchor after it", {join({hd, intra(23)}), pskip, intra(24), {},
                    bdirect, {}}},
                // A B picture with no anchor before it.
                {"references: a B picture with no anchor before it", {join({hd, intra(25)}), {}, intra(26), bdirect,
                    {}}},
                // A P picture after an I picture that came after pictures with warnings.
                {"references: warnings, then an I picture", {join({hd, intra(27)}), {}, pskip, pskip, intra(28),
                    pskip, bdirect, {}}},
                // The same with the first frame buffer held (its picture taken, not released): a P picture with no
                // anchor, a B picture with none after it.
                {"references: held, a P picture with no anchor", {join({hd, intra(29)}), intra(30), {1}, {}, pskip}},
                {"references: held, a B picture with no anchor after it", {join({hd, intra(31)}), intra(32), {1}, {},
                    bdirect}},
                // The first entry's frame buffer not the first one (a skipped picture's entry holds its anchor's).
                {"references: entries and frame buffers", {join({hd, intra(33)}), v1::skipped_frame(sq), intra(34),
                    intra(35), {}, pskip, {}}},
                // A P picture with no anchor after a single one.
                {"references: a P picture with no anchor after one I picture", {join({hd, intra(36)}), {}, pskip, {}}},
                // A skipped picture with no anchor, the first frame buffer held.
                {"references: held, a skipped picture with no anchor", {join({hd, intra(37)}), intra(38), {1}, {},
                    v1::skipped_frame(sq)}},
            };
            u32 user = 800;
            for (const auto& [what, aus] : parts) {
                d.init(what + ": Initialize");
                hold = what.find("held") != std::string::npos;
                for (usize i = 0; i < aus.size(); i++)
                    step(std::format("{}: {}", what, i), aus[i], user + static_cast<u32>(i));
                if (hold) {
                    r.r("svc1dFlushPicture", {d.handle}, what + ": FlushPicture");
                    d.take(what + ": at the end", true);
                    for (u32 k = 0; k < 4; k++)
                        r.r("svc1dReleasePicture", {d.handle}, std::format("{}: ReleasePicture {}", what, k));
                }
                user += 10;
            }
        }
        for (const char* what : {"CancelDecode", "Discontinue", "Initialize"}) {
            const std::string key = std::string(what) + " in the stream";
            if (!selected(key)) continue;
            d.init(key + ": Initialize");
            d.decode(key + ": DecodePicture 0", join({hd, intra(7)}), 710);
            d.decode(key + ": DecodePicture 1", pskip, 711);
            d.decode(key + ": DecodePicture 2", pskip, 712);
            r.r(std::string("svc1d") + what, {d.handle}, key + ": " + what);
            ps_write32(b, kOut, 0xEEEEEEEE);
            r.r("svc1dGetNumPicture", {d.handle, kOut}, key + ": GetNumPicture");
            r.o.line(key + ": pictures", d.w(ps_read32(b, kOut)));
            d.decode(key + ": DecodePicture 3", pskip, 713);
            d.decode(key + ": DecodePicture 4", bdirect, 714);
            d.decode(key + ": DecodePicture 5", intra(8), 715);
            d.decode(key + ": DecodePicture 6", pskip, 716);
            d.take(key + ": after DecodePicture 6", true);
            // An entry point and an I picture, then the headers and one, then a P picture.
            d.decode(key + ": DecodePicture 7", join({v1::entry_point(entry), intra(17)}), 717);
            d.take(key + ": after DecodePicture 7", true);
            d.decode(key + ": DecodePicture 8", join({hd, intra(18)}), 718);
            d.take(key + ": after DecodePicture 8", true);
            d.decode(key + ": DecodePicture 9", pskip, 719);
            d.take(key + ": after DecodePicture 9", true);
            r.r("svc1dFlushPicture", {d.handle}, key + ": FlushPicture");
            d.take(key + ": after FlushPicture", true);
        }
        // The decode modes (SetDecodeMode's {byte, word}: libvdec gives {0, 0} for all pictures, {0, 0xFF} to skip
        // B pictures, and the byte 1 for its own reasons).
        for (const auto& [byte, word] : {std::pair{0u, 0xFFu}, {1u, 0u}, {1u, 0xFFu}, {2u, 0u}, {3u, 0u}, {0u, 1u},
                 {0u, 2u}, {0x80u, 0u}, {0u, 0u}}) {
            const std::string what = std::format("mode {:#x} {:#x}", byte, word);
            if (!selected(what)) continue;
            d.init(what + ": Initialize");
            std::memset(b + kMode, 0, 8);
            ps_write8(b, kMode, static_cast<u8>(byte));
            ps_write32(b, kMode + 4, word);
            r.r("svc1dSetDecodeMode", {d.handle, kMode}, what + ": SetDecodeMode");
            d.stream(what, kinds(sq, entry, plain), 720 + 10 * byte + word);
        }
        // Access units of other shapes: in two pieces, with zeros after, with an end of sequence, two frames, headers
        // alone, an end of sequence alone, bytes that are no stream.
        if (selected("shapes")) {
            d.init("shapes: Initialize");
            const std::vector<u8> first = join({hd, intra(9)});
            // Two pieces: the first of whole 16-byte blocks, the second at a 128-byte boundary.
            {
                const u32 split = 0x80;
                at_two(r, d, "shapes: two pieces", first, split, 760);
                d.take("shapes: two pieces", true);
            }
            std::vector<u8> zeros = pskip;
            zeros.insert(zeros.end(), 6, 0);
            std::vector<u8> junk(40);
            for (usize i = 0; i < junk.size(); i++) junk[i] = static_cast<u8>(i * 37 + 11);
            const std::vector<std::pair<const char*, std::vector<u8>>> shapes = {
                {"zeros after", zeros},
                {"an end of sequence after", join({pskip, v1::end_of_sequence()})},
                {"two frames", join({pskip, pskip})},
                {"an end of sequence between two frames", join({pskip, v1::end_of_sequence(), pskip})},
                {"the headers after a frame", join({pskip, hd})},
                {"headers alone", hd},
                {"a P picture after headers alone", pskip},
                {"an end of sequence alone", v1::end_of_sequence()},
                {"a P picture after an end of sequence alone", pskip},
                {"bytes that are no stream", junk},
                {"junk before a frame", join({std::vector<u8>(junk.begin(), junk.begin() + 9), pskip})},
                {"an I picture again", intra(10)},
            };
            u32 user = 761;
            for (const auto& [what, au] : shapes) {
                const std::string key = std::string("shapes: ") + what;
                d.decode(key, au, user++);
                ps_write32(b, kOut, 0xEEEEEEEE);
                r.r("svc1dGetNumPicture", {d.handle, kOut}, key + ": GetNumPicture");
                d.take(key, true);
            }
            r.r("svc1dFlushPicture", {d.handle}, "shapes: FlushPicture");
            d.take("shapes: after FlushPicture", true);
            // DecodePicture2's refusals: a second piece after a first not of whole 16-byte blocks, at an address not
            // on 128 bytes, at no address.
            for (const auto& [what, a1, s1, a2, s2] : {std::tuple{"a first piece not of 16-byte blocks", kStream, 0x81u,
                     kStream + 0x100, 0x10u}, {"a second piece off 128 bytes", kStream, 0x80u, kStream + 0x110, 0x10u},
                     {"a second piece at no address", kStream, 0x80u, 0u, 0x10u}}) {
                ps_write32(b, kAu + 0, a1);
                ps_write32(b, kAu + 4, s1);
                ps_write32(b, kAu + 8, a2);
                ps_write32(b, kAu + 12, s2);
                r.r("svc1dDecodePicture2", {d.handle, kAu, kStatus}, std::string("shapes: ") + what);
            }
        }
        // The simple and main profiles: SetPictureSize and DecodeSeqHeader (STRUCT_C), then frames without start
        // codes.
        {
            const auto header = [&](const std::string& key, const std::vector<u8>& bytes) {
                std::memcpy(b + kStream + 0x7F000, bytes.data(), bytes.size());
                ps_write32(b, kAu + 0, kStream + 0x7F000);
                ps_write32(b, kAu + 4, static_cast<u32>(bytes.size()));
                ps_write32(b, kStatus, 0xEEEEEEEE);
                r.r("svc1dDecodeSeqHeader", {d.handle, kAu, kStatus}, key);
                r.o.line(key + ": used", d.w(ps_read32(b, kStatus)));
                d.flag(key);
                d.seq(key);
            };
            const auto levels = [](int seed) {
                v1::DcPicture p = vc1_levels(64, 48, seed);
                for (auto* plane : {&p.y, &p.cb, &p.cr})
                    for (int& v : *plane) v += 100;
                return p;
            };
            // STRUCT_C and the frames as BDUs (a sequence header's start code, a frame's), escaped, as Sony's program
            // takes them; or `raw`, as a container has them (Sony's program finds no start code in them).
            const auto bdu = [](u8 code, const std::vector<u8>& payload) {
                std::vector<u8> out = {0x00, 0x00, 0x01, code};
                int zeros = 0;
                for (const u8 c : payload) {
                    if (zeros >= 2 && c <= 3) {
                        out.push_back(3);
                        zeros = 0;
                    }
                    out.push_back(c);
                    zeros = c == 0 ? zeros + 1 : 0;
                }
                return out;
            };
            // (`frames` false: its pictures' frames left out; `bytes` false: the bytes used too.)
            const auto simple = [&](const std::string& what, const v1::SimpleSequence& s2, const v1::SimpleHeader& h2,
                                    u32 user0, bool raw = false, bool frames = true, bool bytes = true) {
                if (!selected(what)) return;
                d.frames = frames;
                d.used_compared = bytes;
                const auto as = [&](u8 code, const std::vector<u8>& bytes) { return raw ? bytes : bdu(code, bytes); };
                d.init(what + ": Initialize");
                r.r("svc1dSetPictureSize", {d.handle, 64, 48}, what + ": SetPictureSize");
                header(what + ": DecodeSeqHeader", as(0x0F, v1::struct_c(s2)));
                std::vector<std::vector<u8>> aus = {as(0x0D, v1::simple_intra_frame(levels(1), s2, h2)),
                    as(0x0D, v1::simple_skipped_mbs_p_frame(s2, h2, 4, 3)),
                    as(0x0D, v1::simple_intra_frame(levels(2), s2, h2))};
                if (s2.maxbframes) {
                    aus.push_back(as(0x0D, v1::simple_direct_b_frame(s2, h2, 4, 3)));
                    aus.push_back(as(0x0D, v1::simple_intra_frame(levels(3), s2, h2, true)));
                }
                v1::SimpleHeader ic = h2;
                ic.intensity = true;
                ic.lumscale = 16;
                ic.lumshift = 10;
                aus.push_back(as(0x0D, v1::simple_skipped_mbs_p_frame(s2, ic, 4, 3)));
                d.stream(what, aus, user0);
                d.seq(what + ": at the end");
                d.frames = d.used_compared = true;
            };
            const v1::SimpleHeader sh;
            // (The bytes as a container has them first: what DecodeSeqHeader records then is a simple profile
            // sequence, under which the next SetPictureSize sets the frames up as for these profiles, below.)
            simple("main profile unframed", v1::SimpleSequence{}, sh, 1605, true);
            simple("main profile", v1::SimpleSequence{}, sh, 1600);
            simple("main profile again", v1::SimpleSequence{}, sh, 1606);
            // After an advanced profile sequence (the size set before STRUCT_C, or after it).
            if (selected("main profile after the advanced profile")) {
                d.init("main profile after the advanced profile: AP");
                d.stream("main profile after the advanced profile: AP", {join({v1::sequence_header(sq),
                    v1::entry_point(entry), v1::intra_frame(vc1_levels(64, 48, 6), sq, entry, plain)})}, 1608);
            }
            // SetPictureSize under an advanced profile sequence: the frames laid out for it (the height to 32), and the
            // program decoding the rows to that height, the last one from past the frame's data (its samples left
            // out).
            simple("main profile after the advanced profile", v1::SimpleSequence{}, sh, 1609, false, false);
            if (selected("main profile, size after STRUCT_C")) {
                d.init("main profile, size after STRUCT_C: AP");
                d.stream("main profile, size after STRUCT_C: AP", {join({v1::sequence_header(sq),
                    v1::entry_point(entry), v1::intra_frame(vc1_levels(64, 48, 6), sq, entry, plain)})}, 1611);
                d.init("main profile, size after STRUCT_C: Initialize");
                header("main profile, size after STRUCT_C: DecodeSeqHeader", bdu(0x0F, v1::struct_c(v1::SimpleSequence{})));
                r.r("svc1dSetPictureSize", {d.handle, 64, 48}, "main profile, size after STRUCT_C: SetPictureSize");
                d.stream("main profile, size after STRUCT_C", {bdu(0x0D, v1::simple_intra_frame(levels(1),
                    v1::SimpleSequence{}, sh)), bdu(0x0D, v1::simple_skipped_mbs_p_frame(v1::SimpleSequence{}, sh, 4,
                    3))}, 1612);
            }
            v1::SimpleSequence sp0;
            sp0.profile = 0;
            simple("simple profile", sp0, sh, 1610);
            v1::SimpleSequence mb;
            mb.maxbframes = 2;
            v1::SimpleHeader hb;
            hb.bfraction = 3;
            simple("main profile with B pictures", mb, hb, 1620);
            v1::SimpleSequence mt;
            mt.frmrtq = 5;
            mt.bitrtq = 17;
            mt.loopfilter = true;
            mt.multires = true;
            mt.extended_mv = true;
            mt.dquant = 2;
            mt.vstransform = true;
            mt.syncmarker = false; // (SYNCMARKER: below)
            mt.rangered = true;
            mt.quantizer = 1;
            mt.finterpflag = true;
            v1::SimpleHeader ht;
            ht.interpfrm = true;
            ht.frmcnt = 2;
            ht.bf = 0x55;
            ht.pquantizer = true;
            ht.mvrange = 1;
            ht.pqdiff = 3;
            ht.ttfrm = 2;
            simple("main profile, every tool", mt, ht, 1630);
            v1::SimpleSequence mo;
            mo.overlap = true;
            v1::SimpleHeader ho;
            ho.pqindex = 12;
            ho.transacfrm = 1;
            ho.transacfrm2 = 2;
            ho.transdctab = true;
            simple("main profile, overlap, PQINDEX 12", mo, ho, 1640);
            v1::SimpleHeader hr2;
            hr2.rangeredfrm = true;
            v1::SimpleSequence mr;
            mr.rangered = true;
            simple("main profile, range reduction", mr, hr2, 1650);
            v1::SimpleSequence md = mt;
            md.dquant = 1;
            md.multires = false;
            // (Not FINTERPFLAG: its I pictures' macroblocks then end within a byte; the bytes used of one ending on a
            // byte's boundary are one less in pspocket's count, which does not read the macroblock layer.)
            md.finterpflag = false;
            v1::SimpleHeader hd2 = ht;
            hd2.dquantfrm = true;
            hd2.dqprofile = 2;
            hd2.dqedge = 3;
            hd2.mvmode = v1::kHalfPel;
            hd2.mvtab = 2;
            hd2.cbptab = 1;
            hd2.frmcnt = 3;
            simple("main profile, DQUANT 1, half-pel", md, hd2, 1660);
            // Each tool alone (which ones the program warns of), the tables, RESPIC.
            {
                using SimpleEdit = void (*)(v1::SimpleSequence&, v1::SimpleHeader&);
                const std::vector<std::pair<const char*, SimpleEdit>> tools = {
                    {"loop filter", [](v1::SimpleSequence& s2, v1::SimpleHeader&) { s2.loopfilter = true; }},
                    {"extended vectors", [](v1::SimpleSequence& s2, v1::SimpleHeader& h2) {
                        s2.extended_mv = true;
                        h2.mvrange = 2;
                    }},
                    {"variable transforms", [](v1::SimpleSequence& s2, v1::SimpleHeader& h2) {
                        s2.vstransform = true;
                        h2.ttfrm = 1;
                    }},
                    // (SYNCMARKER: the program reads the frames otherwise, past their data: their samples and bytes
                    // used left out, below.)
                    {"range reduction allowed", [](v1::SimpleSequence& s2, v1::SimpleHeader&) { s2.rangered = true; }},
                    {"explicit quantizer", [](v1::SimpleSequence& s2, v1::SimpleHeader& h2) {
                        s2.quantizer = 1;
                        h2.pquantizer = true;
                    }},
                    {"frame interpolation", [](v1::SimpleSequence& s2, v1::SimpleHeader& h2) {
                        s2.finterpflag = true;
                        h2.interpfrm = true;
                    }},
                    {"DQUANT 2", [](v1::SimpleSequence& s2, v1::SimpleHeader& h2) {
                        s2.dquant = 2;
                        h2.pqdiff = 5;
                    }},
                    {"multiresolution", [](v1::SimpleSequence& s2, v1::SimpleHeader& h2) {
                        s2.multires = true;
                        h2.respic = 0;
                    }},
                    {"frame count 1", [](v1::SimpleSequence&, v1::SimpleHeader& h2) { h2.frmcnt = 1; }},
                    {"tables a", [](v1::SimpleSequence&, v1::SimpleHeader& h2) {
                        h2.transacfrm2 = 1;
                        h2.transacfrm = 0;
                    }},
                    {"tables b", [](v1::SimpleSequence&, v1::SimpleHeader& h2) {
                        h2.transacfrm = 2;
                        h2.transdctab = true;
                    }},
                    {"tables c", [](v1::SimpleSequence&, v1::SimpleHeader& h2) { h2.transacfrm2 = 2; }},
                    {"tables d", [](v1::SimpleSequence&, v1::SimpleHeader& h2) { h2.transacfrm = 1; }},
                    {"buffer fullness", [](v1::SimpleSequence&, v1::SimpleHeader& h2) { h2.bf = 0x7F; }},
                    {"half step", [](v1::SimpleSequence&, v1::SimpleHeader& h2) { h2.halfqp = true; }},
                };
                u32 user = 1700;
                for (const auto& [name, edit] : tools) {
                    v1::SimpleSequence s2;
                    v1::SimpleHeader h2;
                    edit(s2, h2);
                    simple(std::string("main profile tool: ") + name, s2, h2, user += 10);
                }
                v1::SimpleSequence sync;
                sync.syncmarker = true;
                simple("main profile tool: sync markers", sync, v1::SimpleHeader{}, 1890, false, false, false);
            }
            // A simple profile stream's calls: CancelDecode, Discontinue and Initialize in it, frames of no bytes and of
            // one, DecodeSeqHeader with a frame.
            for (const char* call : {"CancelDecode", "Discontinue", "Initialize"}) {
                const std::string what = std::string("simple profile stream, ") + call;
                if (!selected(what)) continue;
                const v1::SimpleSequence ss;
                d.init(what + ": Initialize");
                // (A simple profile sequence first, so that SetPictureSize sets the frames up for these profiles.)
                header(what + ": DecodeSeqHeader, no start code", {0, 0, 0, 0});
                r.r("svc1dSetPictureSize", {d.handle, 64, 48}, what + ": SetPictureSize");
                header(what + ": DecodeSeqHeader", bdu(0x0F, v1::struct_c(ss)));
                d.decode(what + ": DecodePicture 0", bdu(0x0D, v1::simple_intra_frame(levels(5), ss, sh)), 1900);
                d.decode(what + ": DecodePicture 1", bdu(0x0D, v1::simple_skipped_mbs_p_frame(ss, sh, 4, 3)), 1901);
                r.r(std::string("svc1d") + call, {d.handle}, what + ": " + call);
                d.decode(what + ": DecodePicture 2", bdu(0x0D, v1::simple_skipped_mbs_p_frame(ss, sh, 4, 3)), 1902);
                d.decode(what + ": DecodePicture 3", bdu(0x0D, v1::simple_intra_frame(levels(6), ss, sh)), 1903);
                d.decode(what + ": DecodePicture 4", bdu(0x0D, v1::simple_skipped_mbs_p_frame(ss, sh, 4, 3)), 1904);
                d.take(what + ": after DecodePicture 4", true);
                r.r("svc1dFlushPicture", {d.handle}, what + ": FlushPicture");
                d.take(what + ": after FlushPicture", true);
            }
            if (selected("simple profile frames of no bytes")) {
                const v1::SimpleSequence ss;
                const std::string what = "simple profile frames of no bytes";
                d.init(what + ": Initialize");
                header(what + ": DecodeSeqHeader, no start code", {0, 0, 0, 0});
                r.r("svc1dSetPictureSize", {d.handle, 64, 48}, what + ": SetPictureSize");
                header(what + ": DecodeSeqHeader", bdu(0x0F, v1::struct_c(ss)));
                d.stream(what, {bdu(0x0D, v1::simple_intra_frame(levels(7), ss, sh)), {0x00, 0x00, 0x01, 0x0D},
                    {0x00, 0x00, 0x01, 0x0D, 0x00}, bdu(0x0D, v1::simple_skipped_mbs_p_frame(ss, sh, 4, 3))}, 1910);
                header(what + ": DecodeSeqHeader with a frame", bdu(0x0D, v1::simple_intra_frame(levels(8), ss, sh)));
            }
            // DecodeSeqHeader with other bytes: 3, 5, an advanced profile's sequence header; DecodePicture with a simple
            // profile frame before any.
            if (selected("sequence headers")) {
                d.init("sequence headers: Initialize");
                d.decode("sequence headers: a frame first", v1::simple_intra_frame(levels(4), v1::SimpleSequence{}, sh),
                    1660);
                std::vector<u8> sc = v1::struct_c(v1::SimpleSequence{});
                header("sequence headers: 3 bytes", std::vector<u8>(sc.begin(), sc.begin() + 3));
                std::vector<u8> sc5 = sc;
                sc5.push_back(0x5A);
                header("sequence headers: 5 bytes", sc5);
                header("sequence headers: the advanced profile's", join({v1::sequence_header(sq), v1::entry_point(entry)}));
                {
                    // A framed one of every tool, then raw bytes: what the record keeps.
                    std::vector<u8> framed = {0x00, 0x00, 0x01, 0x0F};
                    v1::SimpleSequence every;
                    every.loopfilter = every.multires = every.extended_mv = every.vstransform = every.overlap = true;
                    every.syncmarker = every.rangered = every.finterpflag = true;
                    every.dquant = 2;
                    every.maxbframes = 3;
                    every.quantizer = 2;
                    every.frmrtq = 6;
                    every.bitrtq = 30;
                    const std::vector<u8> ec = v1::struct_c(every);
                    framed.insert(framed.end(), ec.begin(), ec.end());
                    header("sequence headers: framed, every tool", framed);
                    header("sequence headers: raw after every tool", sc);
                    header("sequence headers: framed, every tool again", framed);
                    header("sequence headers: zeros", {0, 0, 0, 0, 0, 0, 0, 0});
                    header("sequence headers: framed with zeros after", join({framed, std::vector<u8>(6, 0)}));
                    header("sequence headers: framed, an entry point after", join({framed, v1::entry_point(entry)}));
                }
                header("sequence headers: profile 2", {0x80, sc[1], sc[2], sc[3]});
                header("sequence headers: profile 3", {0xC0, sc[1], sc[2], sc[3]});
                r.r("svc1dSetPictureSize", {d.handle, 32, 32}, "sequence headers: SetPictureSize 32x32");
                header("sequence headers: after SetPictureSize 32x32", sc);
                d.decode("sequence headers: an advanced profile's access unit", join({v1::sequence_header(sq),
                    v1::entry_point(entry), v1::intra_frame(vc1_levels(64, 48, 5), sq, entry, plain)}), 1661);
                d.take("sequence headers: at the end", true);
                r.r("svc1dFlushPicture", {d.handle}, "sequence headers: FlushPicture");
                d.take("sequence headers: after FlushPicture", true);
            }
        }
        // A new sequence header of another size in the stream, then one larger than the instance's.
        if (selected("sizes")) {
            d.init("sizes: Initialize");
            v1::Sequence small;
            small.width = small.height = 32;
            v1::Sequence large;
            large.width = 128;
            large.height = 96;
            d.decode("sizes: 64x48", join({hd, intra(11)}), 780);
            d.width = 32;
            d.height = 32;
            d.decode("sizes: 32x32", join({v1::sequence_header(small), v1::entry_point(entry),
                v1::intra_frame(vc1_levels(32, 32, 12), small, entry, plain)}), 781);
            d.take("sizes: after 32x32", true);
            d.decode("sizes: 32x32 again", v1::skipped_mbs_p_frame(small, entry, plain, 2, 2), 782);
            d.take("sizes: after 32x32 again", true);
            d.width = 128;
            d.height = 96;
            d.decode("sizes: 128x96", join({v1::sequence_header(large), v1::entry_point(entry),
                v1::intra_frame(vc1_levels(128, 96, 13), large, entry, plain)}), 783);
            d.take("sizes: after 128x96", true);
            d.width = 64;
            d.height = 48;
            d.decode("sizes: 64x48 again", join({hd, intra(14)}), 784);
            d.take("sizes: after 64x48 again", true);
            r.r("svc1dFlushPicture", {d.handle}, "sizes: FlushPicture");
            d.take("sizes: after FlushPicture", true);
            d.seq("sizes: at the end");
            // The program stopped: a new instance for what follows.
            r.r("svc1dReleaseInstance", {d.handle}, "sizes: ReleaseInstance");
            std::memset(b + kMem, 0xA5, need + 0x1000);
            r.r("svc1dCreateInstance2", {kMem, need, 6, kParam}, "sizes: CreateInstance2");
            d.state("sizes: CreateInstance2");
            d.seq("sizes: after CreateInstance2");
        }
        // SetPictureSize and DecodeSeqHeader, as libvdec calls them for the simple and main profiles.
        if (selected("picture size")) {
            d.init("picture size: Initialize");
            r.r("svc1dSetPictureSize", {d.handle, 64, 48}, "picture size: SetPictureSize 64x48");
            d.seq("picture size: after SetPictureSize 64x48");
            r.r("svc1dSetPictureSize", {d.handle, 32, 16}, "picture size: SetPictureSize 32x16");
            d.seq("picture size: after SetPictureSize 32x16");
            d.flag("picture size: after SetPictureSize 32x16");
            r.r("svc1dSetPictureSize", {0, 32, 16}, "picture size: SetPictureSize with no handle");
            // Past the instance's size: the program stops, and every call that asks it answers so.
            r.r("svc1dSetPictureSize", {d.handle, 65, 48}, "picture size: SetPictureSize 65x48");
            r.r("svc1dSetPictureSize", {d.handle, 64, 49}, "picture size: SetPictureSize 64x49");
            r.r("svc1dSetPictureSize", {d.handle, 32, 16}, "picture size: SetPictureSize 32x16 again");
            d.seq("picture size: stopped");
            d.flag("picture size: stopped");
            d.init("picture size: stopped: Initialize");
            d.decode("picture size: stopped: DecodePicture", join({hd, intra(19)}), 790);
            r.r("svc1dFlushPicture", {d.handle}, "picture size: stopped: FlushPicture");
            r.r("svc1dDiscontinue", {d.handle}, "picture size: stopped: Discontinue");
            r.r("svc1dCancelDecode", {d.handle}, "picture size: stopped: CancelDecode");
            std::memset(b + kMode, 0, 8);
            r.r("svc1dSetDecodeMode", {d.handle, kMode}, "picture size: stopped: SetDecodeMode");
            d.take("picture size: stopped", true);
            std::memset(b + kSide, 0xEE, 0x200);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("svc1dGetSideInfoPic", {d.handle, kCount, kSide, kSide + 0x100}, "picture size: stopped: GetSideInfoPic");
            r.o.line("picture size: stopped: side information", d.w(ps_read32(b, kCount)) + " " +
                compare::hex(b + kSide, 0x200));
            std::memset(b + kAuxOut, 0xEE, 0x20);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("svc1dGetAuxData", {d.handle, kCount, kAuxOut, kAuxOut + 0x10}, "picture size: stopped: GetAuxData");
            r.o.line("picture size: stopped: auxiliary data", d.w(ps_read32(b, kCount)) + " " +
                compare::hex(b + kAuxOut, 0x20));
            r.r("svc1dReleasePicture", {d.handle}, "picture size: stopped: ReleasePicture");
        }
        // New instances, as libvdec gives each decoder one. A new instance's records hold nothing: a simple profile
        // sequence's (SetPictureSize before the sequence header lays the frames out to 16 lines). The simple and main
        // profiles at each level (range reduction a warning but at the main profile's levels, 3 to 5, and level 0;
        // sync markers at every level); libvdec's calls around their sequence's command; what an advanced profile
        // stream's entry point leaves in the records for a simple or main profile picture.
        {
            const auto bdu = [](u8 code, const std::vector<u8>& payload) {
                std::vector<u8> out = {0x00, 0x00, 0x01, code};
                int zeros = 0;
                for (const u8 c : payload) {
                    if (zeros >= 2 && c <= 3) {
                        out.push_back(3);
                        zeros = 0;
                    }
                    out.push_back(c);
                    zeros = c == 0 ? zeros + 1 : 0;
                }
                return out;
            };
            const auto renew = [&](const std::string& what, u32 level, u32 width, u32 height) {
                r.r("svc1dReleaseInstance", {d.handle}, what + ": ReleaseInstance");
                svc1d_param(b, 5, kTs, width, height);
                ps_write32(b, kOut, 0);
                r.r("svc1dGetMemorySize2", {kOut, level, kParam}, what + ": GetMemorySize2");
                const u32 size = ps_read32(b, kOut);
                std::memset(b + kMem, 0xA5, size + 0x1000);
                r.r("svc1dCreateInstance2", {kMem, size, level, kParam}, what + ": CreateInstance2");
                d.init(what + ": Initialize");
                d.width = width;
                d.height = height;
            };
            const auto sequence = [&](const std::string& what, const std::vector<u8>& bytes) {
                std::memcpy(b + kStream + 0x7F000, bytes.data(), bytes.size());
                ps_write32(b, kAu + 0, kStream + 0x7F000);
                ps_write32(b, kAu + 4, static_cast<u32>(bytes.size()));
                ps_write32(b, kStatus, 0xEEEEEEEE);
                r.r("svc1dDecodeSeqHeader", {d.handle, kAu, kStatus}, what + ": DecodeSeqHeader");
                r.o.line(what + ": DecodeSeqHeader: used", d.w(ps_read32(b, kStatus)));
                d.flag(what + ": DecodeSeqHeader");
                d.seq(what + ": DecodeSeqHeader");
            };
            const auto levels = [](u32 width, u32 height, int seed) {
                v1::DcPicture p = vc1_levels(static_cast<int>(width), static_cast<int>(height), seed);
                for (auto* plane : {&p.y, &p.cb, &p.cr})
                    for (int& v : *plane) v += 100;
                return p;
            };
            // How the instance is made and given the sequence: `calls` are libvdec's other calls around the
            // sequence's command (1 CancelDecode and 2 SetDecodeMode before it, 4 SetDecodeMode after it, 8 SetAuxData
            // before it); the frames an I, a P, an I and a P picture, the last two without RANGEREDFRM unless `all`,
            // the last with intensity compensation when `ic`. (`frames`, `bytes` false: the pictures' frames, the
            // bytes used, left out: sync markers.)
            struct Fresh {
                bool size_first = true, framed = true;
                u32 calls = 0, level = 6, width = 64, height = 32;
                bool all = false, ic = false, frames = true, bytes = true;
            };
            const auto fresh = [&](const std::string& what, const v1::SimpleSequence& ms, const v1::SimpleHeader& mh,
                                   u32 user0, const Fresh& f) {
                if (!selected(what)) return;
                renew(what, f.level, f.width, f.height);
                const auto picture_size = [&] {
                    r.r("svc1dSetPictureSize", {d.handle, f.width, f.height}, what + ": SetPictureSize");
                };
                const auto mode = [&](const std::string& key) {
                    std::memset(b + kMode, 0, 8);
                    r.r("svc1dSetDecodeMode", {d.handle, kMode}, what + ": " + key);
                };
                if (f.calls & 1) r.r("svc1dCancelDecode", {d.handle}, what + ": CancelDecode");
                if (f.calls & 2) mode("SetDecodeMode before");
                if (f.calls & 8) {
                    for (u32 k = 0; k < 4; k++) ps_write32(b, kAux + 4 * k, 0x100 << k);
                    r.r("svc1dSetAuxData", {d.handle, kAux}, what + ": SetAuxData before");
                }
                if (f.size_first) picture_size();
                sequence(what, f.framed ? bdu(0x0F, v1::struct_c(ms)) : v1::struct_c(ms));
                if (!f.size_first) picture_size();
                if (f.calls & 4) mode("SetDecodeMode after");
                v1::SimpleHeader later = mh;
                if (!f.all) later.rangeredfrm = false;
                v1::SimpleHeader last = later;
                if (f.ic) {
                    last.intensity = true;
                    last.lumscale = 16;
                    last.lumshift = 10;
                }
                const int mbw = static_cast<int>(f.width / 16), mbh = static_cast<int>(f.height / 16);
                d.frames = f.frames;
                d.used_compared = f.bytes;
                d.stream(what, {bdu(0x0D, v1::simple_intra_frame(levels(f.width, f.height, 30), ms, mh)),
                    bdu(0x0D, v1::simple_skipped_mbs_p_frame(ms, mh, mbw, mbh)),
                    bdu(0x0D, v1::simple_intra_frame(levels(f.width, f.height, 31), ms, later)),
                    bdu(0x0D, v1::simple_skipped_mbs_p_frame(ms, last, mbw, mbh))}, user0);
                d.frames = d.used_compared = true;
                d.seq(what + ": at the end");
                d.width = 64;
                d.height = 48;
            };
            v1::SimpleSequence reduced, sync;
            reduced.rangered = true;
            sync.syncmarker = true;
            v1::SimpleHeader reduced_frames;
            reduced_frames.rangeredfrm = true;
            fresh("new instance, main profile, the size first", v1::SimpleSequence{}, {}, 2000, {});
            fresh("new instance, range reduction, the size first", reduced, reduced_frames, 2010, {});
            fresh("new instance, range reduction, the size after", reduced, reduced_frames, 2020, {.size_first = false});
            // (No start code: DecodeSeqHeader's default sequence, of sync markers.)
            fresh("new instance, STRUCT_C without its start code", v1::SimpleSequence{}, {}, 2030,
                {.framed = false, .frames = false, .bytes = false});
            fresh("new instance, range reduction, libvdec's calls", reduced, reduced_frames, 2040, {.calls = 15});
            fresh("new instance, range reduction, libvdec's calls, every frame", reduced, reduced_frames, 2050,
                {.calls = 15, .all = true, .ic = true});
            for (u32 level = 0; level <= 10; level++) {
                const u32 user = 2100 + 20 * level;
                fresh(std::format("new instance, level {}, main profile 64x48", level), v1::SimpleSequence{}, {}, user,
                    {.level = level, .width = 64, .height = 48});
                fresh(std::format("new instance, level {}, range reduction", level), reduced, reduced_frames, user + 5,
                    {.level = level, .width = 64, .height = 64, .all = true, .ic = true});
                fresh(std::format("new instance, level {}, sync markers", level), sync, {}, user + 10,
                    {.level = level, .width = 64, .height = 64, .frames = false, .bytes = false});
            }
            // An advanced profile stream's entry point of every field (once or twice), then a main profile sequence in
            // the same instance: the entry point's fields its pictures' records keep. (64x64: the frames laid out as
            // the advanced profile's, to 32 lines, which Sony's program decodes to from bytes past a frame's.)
            for (const u32 entries : {1u, 2u}) {
                const std::string what = std::format("new instance, main profile after {} entry point{} of every field",
                    entries, entries == 1 ? "" : "s");
                if (!selected(what)) continue;
                renew(what, 6, 64, 64);
                v1::Sequence sa;
                sa.width = sa.height = 64;
                v1::Entry ea;
                ea.broken_link = true;
                ea.closed_entry = false;
                ea.panscan = true;
                ea.refdist = true;
                ea.extended_mv = true;
                ea.extended_dmv = true;
                ea.coded_width = 64;
                ea.coded_height = 64;
                ea.range_mapy = 3;
                ea.range_mapuv = 5;
                const v1::Header ha;
                d.stream(what + ": AP", {join({v1::sequence_header(sa), v1::entry_point(ea, sa),
                    v1::intra_frame(vc1_levels(64, 64, 40), sa, ea, ha)})}, 2400);
                // (The second not the first since Initialize.)
                if (entries == 2)
                    d.stream(what + ": AP again", {join({v1::entry_point(ea, sa),
                        v1::intra_frame(vc1_levels(64, 64, 42), sa, ea, ha)})}, 2405);
                r.r("svc1dSetPictureSize", {d.handle, 64, 64}, what + ": SetPictureSize");
                const v1::SimpleSequence ms;
                sequence(what, bdu(0x0F, v1::struct_c(ms)));
                d.stream(what, {bdu(0x0D, v1::simple_intra_frame(levels(64, 64, 41), ms, {})),
                    bdu(0x0D, v1::simple_skipped_mbs_p_frame(ms, {}, 4, 4))}, 2410);
                d.width = 64;
                d.height = 48;
            }
        }
        r.r("svc1dReleaseInstance", {d.handle}, "ReleaseInstance");
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

TEST_CASE("The VC-1 core (libsvc1d): pspocket's own, answering as Sony's did (D-920)", "[hle][vdec-cores]")
{
    check_names("libsvc1d", {"svc1dGetMemorySize", "svc1dGetMemorySize2", "svc1dCreateInstance2",
                                "svc1dReleaseInstance", "svc1dInitialize", "svc1dDecodePicture", "svc1dDecodePicture2",
                                "svc1dDecodeSeqHeader", "svc1dSetPictureSize", "svc1dFlushPicture", "svc1dDiscontinue",
                                "svc1dCancelDecode", "svc1dSetDecodeMode", "svc1dGetPicture", "svc1dGetNumPicture",
                                "svc1dReleasePicture", "svc1dGetSideInfoPic", "svc1dGetAuxData", "svc1dGetSideInfoSeq",
                                "svc1dGetExistenceFlag", "svc1dSetAuxData", "svc1dGetVersionNumber"},
        false);
    Setup s;
    s.mode = Mode::Self;
    const Observed o = svc1d(s, false);
    // A step's line by its start and end (the pictures' numbers count every stream's before them).
    const auto value = [&](std::string_view step, std::string_view what) {
        for (const Line& l : o.lines)
            if (l.key.starts_with(step) && l.key.ends_with(what)) return l.value;
        return std::string("(missing)");
    };
    const auto sample = [&](std::string_view step, usize at) {
        for (const Data& d : o.data)
            if (d.key.starts_with(step) && d.key.ends_with(": its frame")) return d.bytes.at(at);
        FAIL("no frame for " << step);
        return u8{0};
    };
    // Sony's answers (this file's comparison, firmware 4.92).
    CHECK(o.value("GetVersionNumber: the version") == "0x1090000");
    CHECK(o.value("GetMemorySize level 0: the size") == "0x4c6cb70");
    CHECK(o.value("GetMemorySize level 10: the size") == "0x3900070");
    CHECK(o.value("GetMemorySize level 11") == "0x80615342");
    CHECK(o.value("GetMemorySize2 type 5 level 6 64x48: the size") == "0x1412f0");
    CHECK(o.value("GetMemorySize2 type 5 level 6 177x144: the size") == "0x1c0df0");
    CHECK(o.value("GetMemorySize2 type 5 level 10 1920x1088: the size") == "0x1df8070");
    CHECK(o.value("GetMemorySize2 type 3 level 6 64x48: the size") == "0x2dfef0");
    CHECK(o.value("GetMemorySize2 type 2 level 6 64x48") == "0x80615342");
    CHECK(o.value("CreateInstance2 with no taskset") == "0x80615342");
    CHECK(o.value("CreateInstance2 with one byte less") == "0x0"); // the PPU side's 0x480 bytes not counted
    // An I picture with its headers (used to its stop bit's byte), a skipped picture (its 4 bytes), one access unit
    // late, in the 64x48 frame of 16-byte strips with 64 lines and 16 samples of edge.
    CHECK(o.value("I, skipped, I, skipped: DecodePicture 0: used") == "0x141");
    CHECK(o.value("I, skipped, I, skipped: DecodePicture 0: the flag") == "0x10101");
    CHECK(o.value("I, skipped, I, skipped: DecodePicture 1: used") == "0x4");
    CHECK(o.value("I, skipped, I, skipped: DecodePicture 1: the flag") == "0x10000");
    CHECK(o.value("I, skipped, I, skipped: after DecodePicture 1: picture 0: the picture") ==
        " [the core's memory +0x119100] [the core's memory +0x11d100] [the core's memory +0x11d108] 0x40000004 0x40 "
        "0x30 0xc0 0x60 0x0 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee");
    CHECK(o.value("I, skipped, I, skipped: after DecodePicture 1: picture 0: side information")
              .starts_with("0x1 hex:0101000100000000000000000000001f00170000000000010000000104"));
    CHECK(o.value("after the stream: the sequence").starts_with("hex:01a5a5a50000004000000030030000"));
    CHECK(o.value("sequence headers: framed, every tool: the sequence")
              .starts_with("hex:01a5a5a5000000400000003001061e0101010101020101010103020101"));
    // User data: before the sequence header's picture (its flag, the picture not decoded), and of each level.
    CHECK(o.value("user data: DecodePicture 0") == "0x80615320");
    CHECK(o.value("user data: DecodePicture 0: used") == "0x1a");
    CHECK(o.value("user data: DecodePicture 0: the flag") == "0x100101");
    CHECK(o.value("user data of each level: DecodePicture 3: the flag") == "0x1000");
    CHECK(o.value("user data of each level: DecodePicture 8: the flag") == "0x210000");
    CHECK(o.value("user data of each level: DecodePicture 11: the flag") == "0x101011");
    // Access units of no frame: headers alone, an end of sequence alone (the held picture out, flushed).
    CHECK(o.value("shapes: headers alone") == "0x80615308");
    CHECK(o.value("shapes: headers alone: used") == "0x11");
    CHECK(o.value("shapes: an end of sequence alone") == "0x80615324");
    CHECK(o.value("shapes: an end of sequence alone: the flag") == "0x1000000");
    CHECK(o.value("shapes: an end of sequence alone: pictures") == "0x1");
    CHECK(o.value("shapes: junk before a frame: used") == "0xf");
    CHECK(o.value("shapes: a second piece off 128 bytes") == "0x80615341");
    // A P picture with no anchor before it: its picture's warning.
    CHECK(value("references: a P picture with no anchor after one I picture: 3: picture ", ": GetPicture") ==
          "0x80615318");
    // CancelDecode: the stream's pictures passed over to the next entry point.
    CHECK(o.value("CancelDecode in the stream: DecodePicture 3") == "0x80615320");
    CHECK(o.value("CancelDecode in the stream: DecodePicture 5: used") == "0x12d");
    CHECK(o.value("CancelDecode in the stream: DecodePicture 8: the flag") == "0x10101");
    // Interlaced field pictures: a frame of two fields' records, top field first or bottom first, repeated.
    CHECK(o.value("interlaced pictures: fields: DecodePicture 0: used") == "0xe4");
    CHECK(o.value("interlaced pictures: fields: DecodePicture 1: used") == "0x70");
    CHECK(value("interlaced pictures: fields: after DecodePicture 1: picture ", ": the picture")
              .ends_with(" 0x40000410 0x40 0x20 0xa0 0x50 0x0 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee "
                         "0xeeeeeeee 0xeeeeeeee"));
    CHECK(value("interlaced pictures, bottom field first, repeated: fields: after DecodePicture 1: picture ",
              ": the picture")
              .find(" 0x40005410 ") != std::string::npos);
    CHECK(value("interlaced pictures: fields: after DecodePicture 1: picture ", ": side information")
              .starts_with("0x2 hex:0101000100000000000000000000001f000f0000050002010100000604"));
    // Simple and main profiles: STRUCT_C by DecodeSeqHeader, frames of their own (a skipped one of no data), range
    // reduction's and sync markers' warnings.
    CHECK(o.value("main profile: DecodeSeqHeader: used") == "0x8");
    CHECK(o.value("main profile: DecodeSeqHeader: the flag") == "0x1");
    CHECK(o.value("main profile: DecodePicture 0: used") == "0x12f");
    CHECK(o.value("main profile: DecodePicture 1: used") == "0x6");
    CHECK(o.value("simple profile frames of no bytes: DecodePicture 1: used") == "0x4");
    CHECK(o.value("main profile, range reduction: DecodePicture 1") == "0x80615302");
    CHECK(o.value("main profile tool: sync markers: DecodePicture 0") == "0x80615304");
    // New instances: their records empty (a simple profile's: the frames laid out to 16 lines, 0xb0 of them for 64x48;
    // the sequence's size 2x2 before SetPictureSize), range reduction a warning but at the main profile's levels,
    // sync markers at every level; an advanced profile entry point's fields kept in a main profile picture's record
    // (the first-entry flag too: 1 after the first entry point since Initialize, 0 after a second).
    CHECK(value("new instance, level 1, main profile 64x48: after DecodePicture 1: picture ", ": the picture")
              .find(" 0x40000004 0x40 0x30 0xb0 0x58 ") != std::string::npos);
    CHECK(o.value("new instance, range reduction, the size after: DecodeSeqHeader: the sequence")
              .starts_with("hex:01a5a5a500000002000000020100"));
    CHECK(o.value("new instance, level 1, range reduction: DecodePicture 0") == "0x80615302");
    CHECK(o.value("new instance, level 3, range reduction: DecodePicture 0") == "0x0");
    CHECK(o.value("new instance, level 6, range reduction: DecodePicture 0") == "0x80615302");
    CHECK(value("new instance, level 3, range reduction: after DecodePicture 1: picture ", ": GetPicture") == "0x0");
    CHECK(value("new instance, level 6, range reduction: after DecodePicture 1: picture ", ": GetPicture") ==
          "0x80615313");
    CHECK(o.value("new instance, level 3, sync markers: DecodePicture 0") == "0x80615304");
    CHECK(o.value("new instance, STRUCT_C without its start code: DecodeSeqHeader") == "0x8061530c");
    CHECK(value("new instance, main profile after 1 entry point of every field: after DecodePicture 1: picture ",
              ": side information")
              .starts_with("0x1 hex:0001010001010001000000000001001f001f010103010501"));
    CHECK(value("new instance, main profile after 2 entry points of every field: after DecodePicture 1: picture ",
              ": side information")
              .starts_with("0x1 hex:0000010001010001000000000001001f001f010103010501"));
    // A picture size past the instance's: the core stopped for good.
    CHECK(o.value("sizes: 128x96") == "0x80615381");
    CHECK(o.value("sizes: 64x48 again") == "0x80615380");
    CHECK(o.value("picture size: SetPictureSize 65x48") == "0x80615381");
    CHECK(o.value("picture size: stopped: Initialize") == "0x80615380");
    CHECK(o.value("picture size: stopped: GetNumPicture") == "0x0");
    // Samples: Sony's inverse transforms, prediction and filters, to the bit.
    CHECK(sample("I, skipped, I, skipped: after DecodePicture 1: picture ", 0x1000) == 83);
    CHECK(sample("I, skipped, I, skipped: after DecodePicture 1: picture ", 0x1234) == 115);
    CHECK(sample("I, skipped, I, skipped: after DecodePicture 1: picture ", 0x5000) == 106); // Cb
    CHECK(sample("I, skipped, I, skipped: after DecodePicture 1: picture ", 0x5008) == 162); // Cr
    CHECK(sample("motion vector mode mixed: after DecodePicture 2: picture ", 0x2003) == 147);
    CHECK(sample("transform 4x4: after DecodePicture 3: picture ", 0x1500) == 135);
    CHECK(sample("interlaced pictures: fields: after DecodePicture 2: picture ", 0x1100) == 85);
    CHECK(sample("interlaced pictures: B fields: after DecodePicture 2: picture ", 0x1100) == 164);
    CHECK(sample("main profile with B pictures: after DecodePicture 2: picture ", 0x1234) == 133);
    CHECK(sample("main profile, range reduction: after DecodePicture 2: picture ", 0x1234) == 138);
}

TEST_CASE("The MPEG-2 core (libsmvd2) against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    const Setup s = setup();
    run(s, "libsmvd2 against Sony's", [&](bool sony) { return smvd2(s, sony); }, [](const Observed&) {});
}

TEST_CASE("The VC-1 core (libsvc1d) against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    const Setup s = setup();
    run(s, "libsvc1d against Sony's", [&](bool sony) { return svc1d(s, sony); }, [](const Observed&) {});
}
