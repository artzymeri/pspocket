// libsjvtd, the JVT (H.264) core, against Sony's module (D-920 part 8): called directly as Sony's libvdec (or a game)
// calls it, with the same synthetic streams (h264_streams.hpp: I_PCM, P_Skip and B_Skip pictures, whose samples any
// conforming decoder gives exactly) through pspocket's core and through Sony's module (the user's installed system
// software), each in a process of its own per half. Sony's core decodes on SPURS: each half makes a SPURS instance and
// a taskset (pspocket's cellSpurs) and gives the taskset to the core. The frame buffers are in the instance's memory
// (filled with 0xA5 first, so what a core leaves unwritten shows). Compared: every answer and output structure, the
// frame buffers' places and every byte of them, the pictures' order and the errors. Skipped without the firmware
// (compare_util.hpp). No game data.

#include <catch_amalgamated.hpp>

#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "compare_util.hpp"
#include "h264_streams.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
namespace h = pspocket::hle::test::h264;
namespace hs = pspocket::hle::test::h264s;

namespace {

// Guest memory: the core's memory, SPURS and its taskset, the arguments and the streams. The instances' memories do not
// overlap: in pspocket's SPURS, Sony's ReleaseInstance leaves some of its tasks waiting once an instance has been
// initialized again after a stream (D-920 part 8), and a new instance in the same memory would overwrite their saved
// contexts.
constexpr u32 kMem = 0x01000000, kMemSize = 0x03000000;
constexpr u32 kMemSeq = kMem + 0x200000, kMemProbe = kMem + 0x400000, kMemLayout = kMem + 0x600000,
              kMemVui = kMem + 0xA00000, kMemDpb = kMem + 0xC00000, kMemSeqVui = kMem + 0xE00000,
              kMemApi = kMem + 0x1000000; // instances of 2 MB each
constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kPrio = kData + 0x4400,
              kTs = kData + 0x6000;
constexpr u32 kArg = kData + 0x8000, kParam = kArg, kOut = kArg + 0x40, kAu = kArg + 0x80, kStatus = kArg + 0xC0,
              kPic = kArg + 0x100, kAux = kArg + 0x200, kAuxOut = kArg + 0x300, kCount = kArg + 0x380,
              kMode = kArg + 0x3C0, kSide = kArg + 0x800; // kSide: two blocks of 0x188
constexpr u32 kStream = kData + 0x10000, kStreamSize = 0x80000;
constexpr u32 kSeqInfo = kData + 0xA0000, kSeqInfoSize = 0x800; // GetSideInfoSeq's structure

struct CoreFixture : Fixture {
    CoreFixture()
    {
        REQUIRE(hle->main_memory().reserve(kMem, kMemSize));
        REQUIRE(mem.commit(kMem, kMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};
using Run = Half<CoreFixture>;

// An address as a place in the instance's memory (from `base`).
std::string where(u32 addr, u32 base)
{
    if (addr >= base && addr < kMem + kMemSize) return std::format("the core's memory +{:#x}", addr - base);
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

// sjvtdCreateInstance2's and GetMemorySize2's parameters (nine words): the type (4: the level's largest picture, 5 and
// 6: a size), the SPU count, the taskset, the width and height (all ones: the level's), two flags of 0 or 1, the frame
// buffers (up to 16), type 6's last flag; as Sony's libvdec gives them: type 6, its own SPU count and taskset.
void sjvtd_param(u8* b, u32 type, u32 spus, u32 taskset, u32 w = ~0u, u32 h = ~0u, u32 f5 = ~0u, u32 f6 = ~0u,
    u32 frames = ~0u, u32 f8 = 0)
{
    std::memset(b + kParam, 0, 0x40);
    const u32 words[9] = {type, spus, taskset, w, h, f5, f6, frames, f8};
    for (u32 k = 0; k < 9; k++) ps_write32(b, kParam + 4 * k, words[k]);
}

// ------------------------------------------------------------------------------------------------- streams

// A picture sequence (test_vdec_avc_compare.cpp's): each token a picture type (I an IDR, i an I picture, P/p a P
// picture referenced or not, B/b a B picture, B_Skip, referenced or not) and its display index (picture order count 2 x
// the index, type 0). The first IDR access unit has the SPS and PPS; a token may ask for them: '+' both, '%' the SPS,
// '#' the PPS. Access units with no picture: G 16 bytes with no start code, N an access unit delimiter alone, Z the SPS
// and PPS alone.
struct Seq {
    hs::Sps sps;
    hs::Pps pps;
    std::vector<std::vector<u8>> aus;
};

Seq sequence(const std::string& tokens, u32 w = 64, u32 hgt = 64, const std::function<void(hs::Sps&)>& setup = {})
{
    Seq s;
    s.sps.width_mbs = w / 16;
    s.sps.height_map_units = hgt / 16;
    s.sps.poc_type = 0;
    s.sps.profile = 77;
    s.sps.constraints = 0x40;
    s.sps.max_num_ref_frames = 2;
    if (setup) setup(s.sps);
    u32 fn = 0, idr = 0;
    bool first = true;
    usize pos = 0;
    while (pos < tokens.size()) {
        while (pos < tokens.size() && tokens[pos] == ' ') pos++;
        if (pos >= tokens.size()) break;
        bool with_sps = false, with_pps = false;
        for (; pos < tokens.size() && std::strchr("+%#", tokens[pos]); pos++) {
            with_sps = with_sps || tokens[pos] == '+' || tokens[pos] == '%';
            with_pps = with_pps || tokens[pos] == '+' || tokens[pos] == '#';
        }
        const char t = tokens[pos++];
        int idx = 0;
        while (pos < tokens.size() && std::isdigit(static_cast<unsigned char>(tokens[pos])))
            idx = idx * 10 + (tokens[pos++] - '0');
        if (t == 'G' || t == 'N' || t == 'Z') {
            std::vector<u8> au;
            if (t == 'G')
                for (u32 k = 0; k < 16; k++) au.push_back(static_cast<u8>(0x31 * k + 7));
            if (t == 'N') hs::put_nal(au, hs::NAL_AUD, hs::aud_rbsp(1));
            if (t == 'Z') {
                hs::put_nal(au, 0x60 | hs::NAL_SPS, hs::sps_rbsp(s.sps));
                hs::put_nal(au, 0x60 | hs::NAL_PPS, hs::pps_rbsp(s.pps));
            }
            s.aus.push_back(au);
            continue;
        }
        hs::Slice sl;
        hs::AuParts parts;
        parts.sps = with_sps;
        parts.pps = with_pps;
        sl.kind = t == 'I' || t == 'i' ? hs::Kind::I : t == 'P' || t == 'p' ? hs::Kind::P : hs::Kind::B;
        sl.nal_ref_idc = std::isupper(static_cast<unsigned char>(t)) || t == 'i' ? 3 : 0;
        if (t == 'I') {
            sl.idr = true;
            sl.idr_pic_id = idr++;
            fn = 0;
            if (first) parts.sps = parts.pps = true;
            first = false;
        }
        sl.frame_num = fn % (1u << s.sps.log2_max_frame_num);
        if (sl.nal_ref_idc) fn++;
        sl.poc_lsb = static_cast<u32>(2 * idx) % (1u << s.sps.log2_max_poc_lsb);
        s.aus.push_back(
            hs::access_unit(s.sps, s.pps, sl, h::pattern(static_cast<int>(w), static_cast<int>(hgt), idx), parts));
    }
    return s;
}

// An SPS with its HRD parameters as given (h264_streams.hpp writes one set): `cpb` the CPBs' (bit_rate_value_minus1,
// cpb_size_value_minus1, cbr_flag), the scales and the four lengths (minus 1, the last as it is).
struct HrdParams {
    std::vector<std::array<u32, 3>> cpb = {{1999, 3999, 0}};
    u32 bit_rate_scale = 0, cpb_size_scale = 2;
    u32 lengths[4] = {23, 23, 23, 24};
};

void hrd_params(h::BitWriter& w, const HrdParams& p)
{
    w.ue(static_cast<u32>(p.cpb.size() - 1));
    w.bits(p.bit_rate_scale, 4);
    w.bits(p.cpb_size_scale, 4);
    for (const auto& c : p.cpb) {
        w.ue(c[0]);
        w.ue(c[1]);
        w.bit(c[2]);
    }
    for (const u32 l : p.lengths) w.bits(l, 5);
}

// The SPS of `s` (its VUI's HRD parameters `nal` and `vcl` when those are on), as an RBSP.
std::vector<u8> sps_with_hrd(const hs::Sps& s, const HrdParams& nal, const HrdParams& vcl)
{
    // (Picture order count types 0 and 2, frames only, no cropping; the VUI's timing, HRD and pic_struct flags.)
    h::BitWriter w;
    w.byte(s.profile);
    w.byte(s.constraints);
    w.byte(s.level);
    w.ue(s.id);
    w.ue(s.log2_max_frame_num - 4);
    w.ue(s.poc_type);
    if (s.poc_type == 0) w.ue(s.log2_max_poc_lsb - 4);
    w.ue(s.max_num_ref_frames);
    w.bit(s.gaps ? 1 : 0);
    w.ue(s.width_mbs - 1);
    w.ue(s.height_map_units - 1);
    w.bit(1); // frame_mbs_only_flag
    w.bit(1); // direct_8x8_inference_flag
    w.bit(0); // frame_cropping_flag
    w.bit(1); // vui_parameters_present_flag
    const hs::Vui& v = s.vui;
    w.bit(0); // aspect_ratio_info_present_flag
    w.bit(0); // overscan_info_present_flag
    w.bit(0); // video_signal_type_present_flag
    w.bit(0); // chroma_loc_info_present_flag
    w.bit(v.timing ? 1 : 0);
    if (v.timing) {
        w.bits(v.units_in_tick, 32);
        w.bits(v.time_scale, 32);
        w.bit(v.fixed_rate ? 1 : 0);
    }
    w.bit(v.nal_hrd ? 1 : 0);
    if (v.nal_hrd) hrd_params(w, nal);
    w.bit(v.vcl_hrd ? 1 : 0);
    if (v.vcl_hrd) hrd_params(w, vcl);
    if (v.nal_hrd || v.vcl_hrd) w.bit(v.low_delay_hrd ? 1 : 0);
    w.bit(v.pic_struct ? 1 : 0);
    w.bit(0); // bitstream_restriction_flag
    w.trailing();
    return w.take();
}

// A VUI with every field the syntax may leave out. Sony's decoder keeps the last sequence parameter set's fields where
// a new one has none (its SPU task's record, which starts as the SPU's local store was): an instance's first VUI of all
// fields makes what the following ones report known.
void full_vui(hs::Sps& q)
{
    hs::Vui& v = q.vui;
    v.present = true;
    v.aspect_ratio_idc = 255;
    v.sar_width = 4;
    v.sar_height = 3;
    v.overscan = 1;
    v.video_signal = true;
    v.video_format = 2;
    v.full_range = 1;
    v.colour = true;
    v.primaries = 5;
    v.transfer = 6;
    v.matrix = 7;
    v.chroma_loc = true;
    v.chroma_top = 1;
    v.chroma_bottom = 2;
    v.timing = true;
    v.units_in_tick = 1001;
    v.time_scale = 60000;
    v.nal_hrd = true;
    v.vcl_hrd = true;
    v.low_delay_hrd = false;
    v.pic_struct = true;
    v.restriction = true;
    v.num_reorder_frames = 1;
    v.max_dec_frame_buffering = 16;
}

// ------------------------------------------------------------------------------------------------- the core's life

struct Sjvtd {
    Run& r;
    u32 handle = 0;
    u32 taken = 0;
    u32 at = kStream;
    u32 width = 64, height = 64; // the stream's, for the frame's window
    bool frames = true;          // whether the pictures' frames are compared

    std::string w(u32 v) { return std::format("{:#x}", v); }

    void flag(const std::string& key)
    {
        ps_write32(r.b, kOut, 0xEEEEEEEE);
        r.r("sjvtdGetExistenceFlag", {handle, kOut}, key + ": GetExistenceFlag");
        r.o.line(key + ": the flag", w(ps_read32(r.b, kOut)));
    }
    void seq(const std::string& key)
    {
        std::memset(r.b + kSeqInfo, 0xEE, kSeqInfoSize);
        r.r("sjvtdGetSideInfoSeq", {handle, kSeqInfo}, key + ": GetSideInfoSeq");
        r.o.line(key + ": the sequence", compare::hex(r.b + kSeqInfo, kSeqInfoSize));
    }
    // The instance's 0x250 bytes (the PPU side's own state), a note.
    void state(const std::string& key)
    {
        if (!handle) return;
        r.o.note(key + ": state " + compare::hex(r.b + handle, 0x250));
    }

    // Each picture the core holds: GetNumPicture, GetPicture (its words), its side information, auxiliary data, its
    // frame; ReleasePicture after each when `release`.
    void take(const std::string& when, bool release, u32 limit = ~0u)
    {
        u8* b = r.b;
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("sjvtdGetNumPicture", {handle, kOut}, when + ": GetNumPicture");
        r.o.line(when + ": pictures", w(ps_read32(b, kOut)));
        for (u32 k = 0; k < limit; k++) {
            std::memset(b + kPic, 0xEE, 0x80);
            const std::string key = std::format("{}: picture {}", when, taken);
            r.r("sjvtdGetPicture", {handle, kPic}, key + ": GetPicture");
            if (ps_read32(b, kPic) == 0xEEEEEEEE && ps_read32(b, kPic + 4) == 0xEEEEEEEE) return; // none
            taken++;
            std::string words;
            for (u32 i = 0; i < 0x80; i += 4) words += " " + where(ps_read32(b, kPic + i), handle);
            r.o.line(key + ": the picture", words);
            // The frame buffer: strips of 16 samples, the luma's then the chroma's lines (GetPicture's seventh and
            // eighth words), one strip of edge each side; the luma's first sample 32 lines into its strip.
            const u32 luma = ps_read32(b, kPic), pw = ps_read32(b, kPic + 16), lines = ps_read32(b, kPic + 24),
                      clines = ps_read32(b, kPic + 28);
            if (frames && pw && pw <= 1920 && lines <= 1200 && clines <= 600 && luma >= kMem + lines * 16 + 0x200) {
                const u32 frame = luma - lines * 16 - 32 * 16, bytes = ((pw + 15) / 16 + 2) * 16 * (lines + clines);
                if (frame + u64{bytes} <= kMem + kMemSize) r.o.bytes(key + ": its frame buffer", b + frame, bytes);
            }
            std::memset(b + kSide, 0xEE, 0x310);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("sjvtdGetSideInfoPic", {handle, kCount, kSide, kSide + 0x188}, key + ": GetSideInfoPic");
            // The blocks the count names; of a picture of one block, whether the second was written (Sony's module
            // copies a field picture's other field's from where its frame's were last, D-920 part 8).
            const u32 count = ps_read32(b, kCount);
            std::string side = w(count) + " " + compare::hex(b + kSide, count == 1 ? 0x188 : 0x310);
            if (count == 1) {
                bool second = false;
                for (u32 i = 0x188; i < 0x310; i++) second |= b[kSide + i] != 0xEE;
                side += second ? " (the second block written)" : "";
            }
            r.o.line(key + ": side information", side);
            std::memset(b + kAuxOut, 0xEE, 0x20);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("sjvtdGetAuxData", {handle, kCount, kAuxOut, kAuxOut + 0x10}, key + ": GetAuxData");
            r.o.line(key + ": auxiliary data", w(ps_read32(b, kCount)) + " " + compare::hex(b + kAuxOut, 0x20));
            if (release) r.r("sjvtdReleasePicture", {handle}, key + ": ReleasePicture");
        }
    }

    // DecodePicture(handle, au {address, size}, used).
    u64 decode(const std::string& key, const std::vector<u8>& au, u32 user)
    {
        u8* b = r.b;
        if (at + au.size() + 0x100 > kStream + kStreamSize) at = kStream;
        for (u32 k = 0; k < 4; k++) ps_write32(b, kAux + 4 * k, (0x1000 << k) + user);
        r.r("sjvtdSetAuxData", {handle, kAux}, key + ": SetAuxData");
        ps_write32(b, kStatus, 0xEEEEEEEE);
        std::memcpy(b + at, au.data(), au.size());
        std::memset(b + kAu, 0, 0x20);
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
        const u64 v = r.r("sjvtdDecodePicture", {handle, kAu, kStatus}, key);
        r.o.note(std::format("{}: {} bytes", key, au.size()));
        at = (at + static_cast<u32>(au.size()) + 0x7F) & ~0x7Fu;
        r.o.line(key + ": used", w(ps_read32(b, kStatus)));
        flag(key);
        return v;
    }

    u64 init(const std::string& key) { return r.r("sjvtdInitialize", {handle}, key); }

    void stream(const std::string& what, const std::vector<std::vector<u8>>& aus, u32 user0)
    {
        for (usize i = 0; i < aus.size(); i++) {
            decode(std::format("{}: DecodePicture {}", what, i), aus[i], user0 + static_cast<u32>(i));
            take(std::format("{}: after DecodePicture {}", what, i), true);
        }
        r.r("sjvtdFlushPicture", {handle}, what + ": FlushPicture");
        take(what + ": after FlushPicture", true);
        seq(what + ": at the end");
        state(what + ": at the end");
    }

    // An IDR picture of a sequence with every VUI field (full_vui), first in an instance whose streams have VUIs.
    void warm(const std::string& what, u32 user)
    {
        hs::Sps sps;
        sps.width_mbs = width / 16;
        sps.height_map_units = height / 16;
        sps.poc_type = 0;
        sps.profile = 77;
        sps.constraints = 0x40;
        full_vui(sps);
        const hs::Pps pps;
        hs::Slice sl;
        sl.kind = hs::Kind::I;
        sl.idr = true;
        hs::AuParts parts;
        parts.sps = parts.pps = true;
        init(what + ": Initialize");
        stream(what, {hs::access_unit(sps, pps, sl, h::pattern(static_cast<int>(width), static_cast<int>(height), 0),
                         parts)}, user);
    }
};

Observed sjvtd(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libsjvtd");
    u8* b = r.b;
    make_spurs(r);
    Sjvtd d{r};
    ps_write32(b, kOut, 0xEEEEEEEE);
    r.r("sjvtdGetVersionNumber", {kOut}, "GetVersionNumber");
    r.o.line("GetVersionNumber: the version", d.w(ps_read32(b, kOut)));
    // The memory queries: each level, then sizes, flags and frame buffers within one.
    for (u32 level = 0; level <= 18; level++) {
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("sjvtdGetMemorySize", {kOut, level}, std::format("GetMemorySize level {}", level));
        r.o.line(std::format("GetMemorySize level {}: the size", level), d.w(ps_read32(b, kOut)));
    }
    r.r("sjvtdGetMemorySize", {0, 6}, "GetMemorySize with no output");
    struct Query {
        u32 type, level, w, h, f5, f6, frames, f8;
    };
    for (const Query& q : {Query{6, 6, ~0u, ~0u, ~0u, ~0u, ~0u, 0}, Query{6, 6, 64, 64, ~0u, ~0u, ~0u, 0},
             Query{6, 6, 64, 64, 0, 0, 4, 0}, Query{6, 6, 64, 64, 1, 1, 16, 1}, Query{6, 6, 64, 64, 2, ~0u, ~0u, 0},
             Query{6, 6, 64, 64, ~0u, 2, ~0u, 0}, Query{6, 6, 64, 64, ~0u, ~0u, 17, 0},
             Query{6, 6, 64, 64, ~0u, ~0u, ~0u, 2}, Query{6, 6, 0, 64, ~0u, ~0u, ~0u, 0},
             Query{6, 6, 64, 0, ~0u, ~0u, ~0u, 0}, Query{6, 6, 4096, 64, ~0u, ~0u, ~0u, 0},
             Query{5, 6, 64, 64, ~0u, ~0u, ~0u, 0}, Query{4, 6, 64, 64, ~0u, ~0u, ~0u, 0},
             Query{3, 6, 64, 64, ~0u, ~0u, ~0u, 0}, Query{7, 6, 64, 64, ~0u, ~0u, ~0u, 0},
             Query{6, 0, 64, 64, ~0u, ~0u, ~0u, 0}, Query{6, 16, 64, 64, ~0u, ~0u, ~0u, 0},
             Query{6, 17, 64, 64, ~0u, ~0u, ~0u, 0}}) {
        for (const u32 taskset : {0u, kTs}) {
            sjvtd_param(b, q.type, 1, taskset, q.w, q.h, q.f5, q.f6, q.frames, q.f8);
            ps_write32(b, kOut, 0xEEEEEEEE);
            const std::string key = std::format("GetMemorySize2 type {} level {} {}x{} {} {} {} {}{}", q.type, q.level,
                static_cast<s32>(q.w), static_cast<s32>(q.h), static_cast<s32>(q.f5), static_cast<s32>(q.f6),
                static_cast<s32>(q.frames), q.f8, taskset ? " with a taskset" : "");
            r.r("sjvtdGetMemorySize2", {kOut, q.level, kParam}, key);
            r.o.line(key + ": the size", d.w(ps_read32(b, kOut)));
        }
    }
    r.r("sjvtdGetMemorySize2", {kOut, 6, 0}, "GetMemorySize2 with no parameters");
    // CreateInstance2's refusals, then the decoder.
    sjvtd_param(b, 6, 1, kTs, 64, 64);
    ps_write32(b, kOut, 0);
    r.r("sjvtdGetMemorySize2", {kOut, 6, kParam}, "GetMemorySize2 for the decoder");
    const u32 need = ps_read32(b, kOut);
    std::memset(b + kMem, 0xA5, need + 0x1000);
    r.r("sjvtdCreateInstance2", {0, need, 6, kParam}, "CreateInstance2 with no memory");
    r.r("sjvtdCreateInstance2", {kMem, need, 6, 0}, "CreateInstance2 with no parameters");
    r.r("sjvtdCreateInstance2", {kMem, 0x24F, 6, kParam}, "CreateInstance2 with 0x24F bytes");
    r.r("sjvtdCreateInstance2", {kMem, need - 1, 6, kParam}, "CreateInstance2 with one byte less");
    for (const u32 type : {3u, 7u}) {
        sjvtd_param(b, type, 1, kTs, 64, 64);
        r.r("sjvtdCreateInstance2", {kMem, need, 6, kParam}, std::format("CreateInstance2 type {}", type));
    }
    sjvtd_param(b, 6, 1, 0, 64, 64);
    r.r("sjvtdCreateInstance2", {kMem, need, 6, kParam}, "CreateInstance2 with no taskset");
    sjvtd_param(b, 6, 0, kTs, 64, 64);
    r.r("sjvtdCreateInstance2", {kMem, need, 6, kParam}, "CreateInstance2 with 0 SPUs");
    sjvtd_param(b, 6, 1, kTs, 64, 64);
    r.r("sjvtdCreateInstance2", {kMem, need, 17, kParam}, "CreateInstance2 level 17");
    std::memset(b + kMem, 0xA5, need + 0x1000);
    sjvtd_param(b, 6, 1, kTs, 64, 64);
    using SpsEdit = std::function<void(hs::Sps&)>;
    // A new instance of `w` x `h` at `at` (its Initialize done): false when Sony's refuses it.
    const auto open = [&](const std::string& what, u32 at, u32 w, u32 h) {
        sjvtd_param(b, 6, 1, kTs, w, h);
        ps_write32(b, kOut, 0);
        r.r("sjvtdGetMemorySize2", {kOut, 6, kParam}, what + ": GetMemorySize2");
        const u32 size = ps_read32(b, kOut);
        std::memset(b + at, 0xA5, size + 0x1000);
        if (r.r("sjvtdCreateInstance2", {at, size, 6, kParam}, what + ": CreateInstance2") != 0) return false;
        d.handle = at;
        d.width = d.height = 64;
        d.init(what + ": Initialize");
        return true;
    };
    // A sequence's stream after Initialize.
    const auto play = [&](const std::string& what, const std::string& tokens, const SpsEdit& edit, u32 user0) {
        if (!selected(what)) return;
        const Seq sq = sequence(tokens, 64, 64, edit);
        d.init(what + ": Initialize");
        d.stream(what, sq.aus, user0);
    };
    // The streams without a VUI: the instance's first.
    if (r.r("sjvtdCreateInstance2", {kMem, need, 6, kParam}, "CreateInstance2") == 0) {
        d.handle = kMem;
        d.state("CreateInstance2");
        d.flag("after CreateInstance2");
        d.seq("after CreateInstance2");
        for (const u32 mode : {0u, 0xFFu, 1u}) {
            std::memset(b + kMode, 0, 8);
            ps_write32(b, kMode, mode);
            r.r("sjvtdSetDecodeMode", {d.handle, kMode}, std::format("SetDecodeMode {:#x}", mode));
        }
        std::memset(b + kMode, 0, 8);
        r.r("sjvtdSetDecodeMode", {d.handle, kMode}, "SetDecodeMode 0 again");
        d.init("Initialize");
        d.state("Initialize");
        d.flag("after Initialize");
        d.seq("after Initialize");
        if (selected("I P P P")) {
            const Seq ipp = sequence("I0 P1 P2 P3", 64, 64, [](hs::Sps& q) { q.max_num_ref_frames = 1; });
            d.stream("I P P P", ipp.aus, 100);
            d.seq("after the stream");
        }
        if (selected("I P B")) {
            const Seq ipb = sequence("I0 P2 b1 P4 b3", 64, 64);
            d.init("I P B: Initialize");
            d.seq("I P B: after Initialize");
            d.stream("I P B", ipb.aus, 200);
        }
        // The decoded picture buffer's size without a VUI: the level's.
        for (const u32 level : {10u, 11u, 12u, 13u, 20u}) {
            play(std::format("level {}", level), "I0 P1 P2 P3 P4 P5 P6 P7 P8 P9 P10 P11 P12 P13 P14 P15 P16 P17 P18",
                [level](hs::Sps& q) { q.level = static_cast<u8>(level); }, 400 + 20 * level);
        }
        for (const u32 poc : {1u, 2u}) {
            play(std::format("poc type {}", poc), "I0 P1 P2 P3", [poc](hs::Sps& q) {
                q.poc_type = poc;
                q.max_num_ref_frames = 1;
            }, 800 + 10 * poc);
        }
        // Memory management operations: a short-term picture unmarked, long-term indices set and freed, then operation
        // 5 (every picture before it output, the order counts and frame numbers start again).
        if (selected("mmco")) {
            hs::Sps sps;
            sps.width_mbs = sps.height_map_units = 4;
            sps.poc_type = 0;
            sps.profile = 77;
            sps.constraints = 0x40;
            sps.max_num_ref_frames = 3;
            const hs::Pps pps;
            struct Pic {
                hs::Kind kind;
                bool idr;
                u32 frame_num, poc_lsb;
                std::vector<hs::Mmco> mmco;
            };
            const std::vector<Pic> pics = {
                {hs::Kind::I, true, 0, 0, {}},
                {hs::Kind::P, false, 1, 4, {}},
                {hs::Kind::P, false, 2, 8, {{1, 0, 0}, {4, 2, 0}, {6, 0, 0}}},
                {hs::Kind::P, false, 3, 12, {{2, 0, 0}}},
                {hs::Kind::P, false, 4, 16, {{5, 0, 0}}},
                {hs::Kind::P, false, 1, 4, {}},
                {hs::Kind::B, false, 2, 2, {}},
                {hs::Kind::P, false, 2, 8, {}},
            };
            std::vector<std::vector<u8>> aus;
            for (usize k = 0; k < pics.size(); k++) {
                hs::Slice sl;
                sl.kind = pics[k].kind;
                sl.idr = pics[k].idr;
                sl.nal_ref_idc = pics[k].kind == hs::Kind::B ? 0 : 3;
                sl.frame_num = pics[k].frame_num;
                sl.poc_lsb = pics[k].poc_lsb;
                sl.mmco = pics[k].mmco;
                hs::AuParts parts;
                parts.sps = parts.pps = k == 0;
                aus.push_back(hs::access_unit(sps, pps, sl, h::pattern(64, 64, static_cast<int>(k)), parts));
            }
            d.init("mmco: Initialize");
            d.stream("mmco", aus, 1250);
        }
        // The side information of each kind of picture: I pictures not IDR, P and B pictures referenced or not.
        play("pic types", "I0 P2 B1 i4 p3 P6 b5", [](hs::Sps& q) { q.max_num_ref_frames = 3; }, 1200);
        // Pictures of more slices, and of slices of different types.
        if (selected("slices")) {
            hs::Sps sps;
            sps.width_mbs = sps.height_map_units = 4;
            sps.poc_type = 2;
            sps.profile = 77;
            sps.constraints = 0x40;
            sps.max_num_ref_frames = 2;
            const hs::Pps pps;
            std::vector<std::vector<u8>> aus;
            for (u32 k = 0; k < 4; k++) {
                hs::Slice sl;
                sl.kind = k == 0 || k == 2 ? hs::Kind::I : hs::Kind::P;
                sl.idr = k == 0;
                sl.frame_num = k;
                hs::AuParts parts;
                parts.sps = parts.pps = k == 0;
                if (k == 3) {
                    // An I slice then a P slice in one picture.
                    std::vector<u8> au;
                    hs::put_nal(au, hs::NAL_AUD, hs::aud_rbsp(1));
                    hs::Slice a = sl;
                    a.kind = hs::Kind::I;
                    a.first_mb = 0;
                    a.mbs = 8;
                    hs::Slice c = sl;
                    c.kind = hs::Kind::P;
                    c.first_mb = 8;
                    c.mbs = 8;
                    const h::Picture pic = h::pattern(64, 64, 3);
                    hs::put_nal(au, 0x60 | hs::NAL_SLICE, hs::slice_rbsp(sps, pps, a, pic));
                    hs::put_nal(au, 0x60 | hs::NAL_SLICE, hs::slice_rbsp(sps, pps, c, pic));
                    aus.push_back(au);
                    continue;
                }
                aus.push_back(hs::access_unit(sps, pps, sl, h::pattern(64, 64, static_cast<int>(k)), parts, k + 2));
            }
            d.init("slices: Initialize");
            d.stream("slices", aus, 1300);
        }
        if (selected("fields")) {
            hs::Sps sps;
            sps.width_mbs = 4;
            sps.height_map_units = 2; // 64x64 frames of two 32-line fields
            sps.frame_mbs_only = false;
            sps.poc_type = 0;
            sps.profile = 77;
            sps.constraints = 0x40;
            sps.max_num_ref_frames = 2;
            const hs::Pps pps;
            std::vector<std::vector<u8>> aus;
            u32 fn = 0;
            for (u32 k = 0; k < 3; k++) {
                for (u32 f = 0; f < 2; f++) {
                    hs::Slice sl;
                    sl.kind = k == 0 ? hs::Kind::I : hs::Kind::P;
                    sl.idr = k == 0 && f == 0;
                    if (k == 0 && f == 1) sl.kind = hs::Kind::I;
                    sl.field = true;
                    sl.bottom = f == 1;
                    sl.frame_num = fn;
                    sl.poc_lsb = 4 * k + f;
                    hs::AuParts parts;
                    parts.sps = parts.pps = k == 0 && f == 0;
                    aus.push_back(hs::access_unit(sps, pps, sl, h::pattern(64, 64, static_cast<int>(k)), parts));
                }
                fn++;
            }
            d.init("fields: Initialize");
            d.stream("fields", aus, 900);
        }
        r.r("sjvtdReleaseInstance", {d.handle}, "ReleaseInstance");
    }
    // The streams with VUIs: an instance of their own, its first sequence one of every VUI field (Sony's decoder keeps
    // the fields a VUI leaves out from the one before: full_vui).
    if (open("with VUIs", kMemVui, 64, 64)) {
        d.warm("with VUIs: warm", 90);
        // The decoded picture buffer's size: the VUI's max_dec_frame_buffering.
        for (const u32 buffering : {0u, 1u, 2u, 3u}) {
            play(std::format("buffering {}", buffering), "I0 P1 P2 P3 P4 P5", [buffering](hs::Sps& q) {
                q.max_num_ref_frames = std::max(buffering, 1u);
                q.vui.present = true;
                q.vui.restriction = true;
                q.vui.num_reorder_frames = 0;
                q.vui.max_dec_frame_buffering = buffering;
            }, 300 + 10 * buffering);
        }
        play("buffering 2, B pictures", "I0 P2 b1 P4 b3 P6 b5", [](hs::Sps& q) {
            q.vui.present = true;
            q.vui.restriction = true;
            q.vui.num_reorder_frames = 1;
            q.vui.max_dec_frame_buffering = 2;
        }, 350);
        // What the side information reports: the VUI's fields, cropping.
        play("vui", "I0 P2 b1 P4 b3", [](hs::Sps& q) {
            q.crop = true;
            q.crop_right = 2;
            q.crop_bottom = 1;
            full_vui(q);
            hs::Vui& v = q.vui;
            v.aspect_ratio_idc = 255;
            v.sar_width = 16;
            v.sar_height = 11;
            v.overscan = 0;
            v.video_signal = true;
            v.video_format = 3;
            v.full_range = 0;
            v.primaries = 6;
            v.transfer = 7;
            v.matrix = 8;
            v.chroma_top = 3;
            v.chroma_bottom = 4;
            v.units_in_tick = 1;
            v.time_scale = 50;
            v.low_delay_hrd = false;
            v.num_reorder_frames = 1;
            v.max_dec_frame_buffering = 2;
        }, 600);
        // SEI messages.
        if (selected("sei")) {
            hs::Sps sps;
            sps.width_mbs = 4;
            sps.height_map_units = 4;
            sps.poc_type = 2;
            sps.profile = 77;
            sps.constraints = 0x40;
            sps.vui.present = true;
            sps.vui.timing = true;
            sps.vui.nal_hrd = true;
            sps.vui.pic_struct = true;
            const hs::Pps pps;
            std::vector<std::vector<u8>> aus;
            for (u32 k = 0; k < 4; k++) {
                hs::Slice sl;
                sl.kind = k == 0 ? hs::Kind::I : hs::Kind::P;
                sl.idr = k == 0;
                sl.frame_num = k;
                hs::AuParts parts;
                parts.sps = parts.pps = k == 0;
                // buffering_period (0), pic_timing (1: delays of 24 bits, pic_struct k+1, no clock timestamps),
                // recovery_point (6), closed captions (4).
                parts.sei.push_back({0, hs::payload([](h::BitWriter& w) {
                    w.ue(0);
                    w.bits(9000, 24);
                    w.bits(0, 24);
                })});
                parts.sei.push_back({1, hs::payload([k](h::BitWriter& w) {
                    w.bits(2 * k, 24);
                    w.bits(k, 24);
                    w.bits(k + 1, 4);
                    const u32 clocks = k + 1 == 3 || k + 1 == 4 ? 2 : 1;
                    for (u32 c = 0; c < clocks; c++) w.bit(0);
                })});
                if (k == 1) parts.sei.push_back({6, hs::payload([](h::BitWriter& w) {
                    w.ue(3);
                    w.bit(1);
                    w.bit(0);
                    w.bits(0, 2);
                })});
                parts.sei.push_back(hs::cc_data({{0xFC, static_cast<u8>(0x94 + k), 0x2C}, {0xFD, 0x10, 0x20}}));
                aus.push_back(hs::access_unit(sps, pps, sl, h::pattern(64, 64, static_cast<int>(k)), parts));
            }
            d.init("sei: Initialize");
            d.stream("sei", aus, 700);
        }
        // Each kind of SEI message alone, and each pic_struct of a frame (D.2.2): the existence flag, the pictures'
        // structure (GetPicture's fourth word, the side information).
        const auto sei_sps = [] {
            hs::Sps sps;
            sps.width_mbs = 4;
            sps.height_map_units = 4;
            sps.poc_type = 2;
            sps.profile = 77;
            sps.constraints = 0x40;
            sps.max_num_ref_frames = 1;
            sps.vui.present = true;
            sps.vui.timing = true;
            sps.vui.nal_hrd = true;
            sps.vui.pic_struct = true;
            return sps;
        };
        // buffering_period (one NAL CPB, 24-bit delays) and pic_timing (24-bit delays, pic_struct, its clock
        // timestamps' flags 0).
        const auto bp = []() -> hs::SeiMessage {
            return {0, hs::payload([](h::BitWriter& w) {
                        w.ue(0);
                        w.bits(9000, 24);
                        w.bits(0, 24);
                    })};
        };
        const auto pt = [](u32 cpb, u32 dpb, u32 pic_struct) -> hs::SeiMessage {
            return {1, hs::payload([=](h::BitWriter& w) {
                        w.bits(cpb, 24);
                        w.bits(dpb, 24);
                        w.bits(pic_struct, 4);
                        constexpr u32 kClocks[9] = {1, 1, 1, 2, 2, 3, 3, 2, 3};
                        for (u32 c = 0; c < kClocks[std::min(pic_struct, 8u)]; c++) w.bit(0);
                    })};
        };
        if (selected("sei kinds")) {
            const hs::Sps sps = sei_sps();
            const hs::Pps pps;
            std::vector<std::vector<u8>> aus;
            for (u32 k = 0; k < 11; k++) {
                hs::Slice sl;
                sl.kind = k == 0 ? hs::Kind::I : hs::Kind::P;
                sl.idr = k == 0;
                sl.frame_num = k % 16;
                hs::AuParts parts;
                parts.sps = parts.pps = k == 0;
                switch (k) {
                case 0:
                    parts.sei = {bp(), pt(0, 0, 0)};
                    break;
                case 1: parts.sei = {pt(2, 0, 3)}; break;
                case 2: parts.sei = {bp()}; break;
                case 3: parts.sei = {hs::cc_data({{0xFC, 0x94, 0x2C}})}; break;
                case 4:
                    parts.sei = {{6, hs::payload([](h::BitWriter& w) {
                                      w.ue(3);
                                      w.bit(1);
                                      w.bit(0);
                                      w.bits(0, 2);
                                  })}};
                    break;
                default:
                    if (k <= 9) parts.sei = {pt(2 * k, 0, k - 1)};
                    break;
                }
                aus.push_back(hs::access_unit(sps, pps, sl, h::pattern(64, 64, static_cast<int>(k)), parts));
            }
            d.init("sei kinds: Initialize");
            d.stream("sei kinds", aus, 1800);
        }
        // The output times of the HRD (C.4.5.2): a buffering period in every access unit, the same delays in each.
        if (selected("hrd times")) {
            const hs::Sps sps = sei_sps();
            const hs::Pps pps;
            std::vector<std::vector<u8>> aus;
            for (u32 k = 0; k < 5; k++) {
                hs::Slice sl;
                sl.kind = k == 0 ? hs::Kind::I : hs::Kind::P;
                sl.idr = k == 0;
                sl.frame_num = k;
                hs::AuParts parts;
                parts.sps = parts.pps = k == 0;
                parts.sei = {bp(), pt(2, 3, 0)};
                aus.push_back(hs::access_unit(sps, pps, sl, h::pattern(64, 64, static_cast<int>(k)), parts));
            }
            d.init("hrd times: Initialize");
            d.stream("hrd times", aus, 1900);
        }
        r.r("sjvtdReleaseInstance", {d.handle}, "with VUIs: ReleaseInstance");
    }
    // The sequence structure's fields: an IDR picture of a sequence each, one field of the SPS changed from the base (a
    // Main profile 64x64 sequence of picture order count type 0); those without a VUI in an instance with none, those
    // with one in an instance whose first had every field.
    {
        using Edit = std::function<void(hs::Sps&)>;
        const std::vector<std::pair<std::string, Edit>> edits = {
            {"base", [](hs::Sps&) {}},
            {"profile 66", [](hs::Sps& q) { q.profile = 66; q.constraints = 0xE0; }},
            {"profile 100", [](hs::Sps& q) { q.profile = 100; q.constraints = 0x00; }},
            {"constraints 0x10", [](hs::Sps& q) { q.constraints = 0x10; }},
            {"level 30", [](hs::Sps& q) { q.level = 30; }},
            {"log2 max frame num 7", [](hs::Sps& q) { q.log2_max_frame_num = 7; }},
            {"log2 max poc lsb 9", [](hs::Sps& q) { q.log2_max_poc_lsb = 9; }},
            {"poc type 1", [](hs::Sps& q) {
                q.poc_type = 1;
                q.offset_non_ref = -3;
                q.offset_top_bottom = 5;
                q.ref_frame_offsets = {2, 7, -4};
            }},
            {"poc type 1, delta always zero", [](hs::Sps& q) {
                q.poc_type = 1;
                q.delta_always_zero = true;
            }},
            {"poc type 2", [](hs::Sps& q) { q.poc_type = 2; }},
            {"ref frames 5", [](hs::Sps& q) { q.max_num_ref_frames = 5; }},
            {"gaps", [](hs::Sps& q) { q.gaps = true; }},
            {"fields", [](hs::Sps& q) { q.frame_mbs_only = false; q.height_map_units = 2; }},
            {"direct 8x8 off", [](hs::Sps& q) { q.direct_8x8 = false; }},
            {"crop", [](hs::Sps& q) {
                q.crop = true;
                q.crop_left = 1;
                q.crop_right = 2;
                q.crop_top = 3;
                q.crop_bottom = 4;
            }},
            {"80x48", [](hs::Sps& q) { q.width_mbs = 5; q.height_map_units = 3; }},
            // (delta_pic_order_always_zero_flag 0 again: the decoder's SPU task keeps it for the next instance too,
            // whose task may run where this one's did or not.)
            {"poc type 1 again", [](hs::Sps& q) { q.poc_type = 1; }},
            {"vui empty", [](hs::Sps& q) { q.vui.present = true; }},
            {"vui aspect 3", [](hs::Sps& q) { q.vui.present = true; q.vui.aspect_ratio_idc = 3; }},
            {"vui overscan 0", [](hs::Sps& q) { q.vui.present = true; q.vui.overscan = 0; }},
            {"vui video signal", [](hs::Sps& q) {
                q.vui.present = true;
                q.vui.video_signal = true;
                q.vui.video_format = 3;
            }},
            {"vui colour", [](hs::Sps& q) {
                q.vui.present = true;
                q.vui.video_signal = true;
                q.vui.colour = true;
                q.vui.primaries = 9;
                q.vui.transfer = 10;
                q.vui.matrix = 11;
            }},
            {"vui chroma loc", [](hs::Sps& q) {
                q.vui.present = true;
                q.vui.chroma_loc = true;
                q.vui.chroma_top = 4;
                q.vui.chroma_bottom = 5;
            }},
            {"vui timing", [](hs::Sps& q) {
                q.vui.present = true;
                q.vui.timing = true;
                q.vui.units_in_tick = 1;
                q.vui.time_scale = 50;
                q.vui.fixed_rate = false;
            }},
            {"vui nal hrd", [](hs::Sps& q) { q.vui.present = true; q.vui.nal_hrd = true; q.vui.low_delay_hrd = true; }},
            {"vui vcl hrd", [](hs::Sps& q) { q.vui.present = true; q.vui.vcl_hrd = true; }},
            {"vui pic struct", [](hs::Sps& q) { q.vui.present = true; q.vui.pic_struct = true; }},
            {"vui restriction", [](hs::Sps& q) {
                q.vui.present = true;
                q.vui.restriction = true;
                q.vui.num_reorder_frames = 3;
                q.vui.max_dec_frame_buffering = 4;
                q.max_num_ref_frames = 4;
            }},
            {"vui timing fixed", [](hs::Sps& q) {
                q.vui.present = true;
                q.vui.timing = true;
                q.vui.units_in_tick = 1;
                q.vui.time_scale = 50;
            }},
            {"vui timing fixed, pic struct", [](hs::Sps& q) {
                q.vui.present = true;
                q.vui.timing = true;
                q.vui.units_in_tick = 2;
                q.vui.time_scale = 48;
                q.vui.pic_struct = true;
            }},
            {"vui aspect 255", [](hs::Sps& q) {
                q.vui.present = true;
                q.vui.aspect_ratio_idc = 255;
                q.vui.sar_width = 10;
                q.vui.sar_height = 11;
            }},
            {"vui colour 1 1 1", [](hs::Sps& q) {
                q.vui.present = true;
                q.vui.video_signal = true;
                q.vui.video_format = 1;
                q.vui.full_range = 0;
                q.vui.colour = true;
                q.vui.primaries = 1;
                q.vui.transfer = 1;
                q.vui.matrix = 1;
            }},
            {"vui 80x48", [](hs::Sps& q) { q.vui.present = true; q.width_mbs = 5; q.height_map_units = 3; }},
            {"sps id 3", [](hs::Sps& q) { q.id = 3; }},
        };
        bool any = false;
        for (const auto& e : edits) any |= selected("seq fields: " + e.first);
        for (const bool with_vui : {false, true}) {
            if (!any) break;
            const std::string inst = with_vui ? "seq fields with VUIs" : "seq fields";
            if (!open(inst, with_vui ? kMemSeqVui : kMemSeq, 80, 64)) continue;
            if (with_vui) d.warm(inst + ": warm", 990);
            u32 user = with_vui ? 1100 : 1000;
            for (const auto& [name, edit] : edits) {
                hs::Sps probe;
                edit(probe);
                if (probe.vui.present != with_vui) continue;
                const std::string what = "seq fields: " + name;
                if (!selected(what)) continue;
                const Seq sq = sequence("I0", 64, 64, edit);
                d.init(what + ": Initialize");
                d.stream(what, sq.aus, user++);
            }
            // HRD parameters of more CPBs, scales and lengths (a custom SPS in the IDR access unit).
            if (with_vui) {
                struct HrdCase {
                    std::string name;
                    bool nal, vcl;
                };
                HrdParams two;
                two.cpb = {{999, 1999, 1}, {4999, 7999, 0}};
                two.bit_rate_scale = 3;
                two.cpb_size_scale = 5;
                two.lengths[0] = 10;
                two.lengths[1] = 11;
                two.lengths[2] = 12;
                two.lengths[3] = 13;
                for (const HrdCase& c : {HrdCase{"hrd nal two cpbs", true, false},
                         HrdCase{"hrd vcl two cpbs", false, true}, HrdCase{"hrd both", true, true}}) {
                    const std::string what = "seq fields: " + c.name;
                    if (!selected(what)) continue;
                    hs::Sps sps;
                    sps.width_mbs = sps.height_map_units = 4;
                    sps.profile = 77;
                    sps.constraints = 0x40;
                    sps.vui.present = true;
                    sps.vui.timing = true;
                    sps.vui.nal_hrd = c.nal;
                    sps.vui.vcl_hrd = c.vcl;
                    sps.vui.low_delay_hrd = true;
                    sps.vui.pic_struct = true;
                    const hs::Pps pps;
                    hs::Slice sl;
                    sl.kind = hs::Kind::I;
                    sl.idr = true;
                    std::vector<u8> au;
                    hs::put_nal(au, hs::NAL_AUD, hs::aud_rbsp(0));
                    hs::put_nal(au, 0x60 | hs::NAL_SPS, sps_with_hrd(sps, two, two));
                    hs::put_nal(au, 0x60 | hs::NAL_PPS, hs::pps_rbsp(pps));
                    hs::put_nal(au, 0x65, hs::slice_rbsp(sps, pps, sl, h::pattern(64, 64, 0)));
                    d.init(what + ": Initialize");
                    d.stream(what, {au}, user++);
                }
            }
            r.r("sjvtdReleaseInstance", {d.handle}, inst + ": ReleaseInstance");
        }
    }
    // A new sequence parameter set at an IDR picture without Initialize: whether the decoded picture buffer's size
    // follows it.
    if (selected("dpb change") && open("dpb change", kMemDpb, 64, 64)) {
        std::vector<std::vector<u8>> aus =
            sequence("I0 P1 P2", 64, 64, [](hs::Sps& q) { q.max_num_ref_frames = 1; }).aus;
        for (auto& au : sequence("I0 P1 P2 P3", 64, 64, [](hs::Sps& q) {
                 q.max_num_ref_frames = 1;
                 full_vui(q);
                 q.vui.num_reorder_frames = 0;
                 q.vui.max_dec_frame_buffering = 1;
             }).aus)
            aus.push_back(au);
        d.stream("dpb change", aus, 1600);
        r.r("sjvtdReleaseInstance", {d.handle}, "dpb change: ReleaseInstance");
    }
    // The calls around the pictures: on an instance with none, the decode mode, Discontinue and CancelDecode in a
    // stream, a picture taken twice, access units of no bytes, a picture larger than the instance's (which stops it)
    // and every call after that. Each in an instance of its own.
    if (selected("api")) {
        u32 slot = 0;
        const auto fresh = [&](const std::string& what) {
            const u32 at = kMemApi + 0x200000 * slot++;
            sjvtd_param(b, 6, 1, kTs, 64, 64);
            ps_write32(b, kOut, 0);
            r.r("sjvtdGetMemorySize2", {kOut, 6, kParam}, what + ": GetMemorySize2");
            const u32 size = ps_read32(b, kOut);
            std::memset(b + at, 0xA5, size + 0x1000);
            if (r.r("sjvtdCreateInstance2", {at, size, 6, kParam}, what + ": CreateInstance2") != 0) return false;
            d.handle = at;
            d.width = d.height = 64;
            d.init(what + ": Initialize");
            return true;
        };
        // Every query on the instance as it is (the sequence structure once a picture was decoded: before, Sony's
        // module gives what its SPU task's memory held from an earlier instance).
        const auto queries = [&](const std::string& what, bool sequence_too = true) {
            ps_write32(b, kOut, 0xEEEEEEEE);
            r.r("sjvtdGetNumPicture", {d.handle, kOut}, what + ": GetNumPicture");
            r.o.line(what + ": pictures", d.w(ps_read32(b, kOut)));
            std::memset(b + kPic, 0xEE, 0x40);
            r.r("sjvtdGetPicture", {d.handle, kPic}, what + ": GetPicture");
            r.o.line(what + ": GetPicture's words", compare::hex(b + kPic, 0x24));
            std::memset(b + kSide, 0xEE, 0x310);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("sjvtdGetSideInfoPic", {d.handle, kCount, kSide, kSide + 0x188}, what + ": GetSideInfoPic");
            r.o.line(what + ": side information's count", d.w(ps_read32(b, kCount)));
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("sjvtdGetAuxData", {d.handle, kCount, kAuxOut, kAuxOut + 0x10}, what + ": GetAuxData");
            r.o.line(what + ": auxiliary data's count", d.w(ps_read32(b, kCount)));
            r.r("sjvtdReleasePicture", {d.handle}, what + ": ReleasePicture");
            if (sequence_too) {
                std::memset(b + kSeqInfo, 0xEE, 0x20);
                r.r("sjvtdGetSideInfoSeq", {d.handle, kSeqInfo}, what + ": GetSideInfoSeq");
                r.o.line(what + ": the sequence's first words", compare::hex(b + kSeqInfo, 0x20));
            }
            d.flag(what);
        };
        const auto feed = [&](const std::string& what, const std::string& tokens, u32 user0, bool take = true) {
            const Seq sq = sequence(tokens, 64, 64, [](hs::Sps& q) { q.max_num_ref_frames = 2; });
            for (usize i = 0; i < sq.aus.size(); i++) {
                d.decode(std::format("{}: DecodePicture {}", what, i), sq.aus[i], user0 + static_cast<u32>(i));
                if (take) d.take(std::format("{}: after DecodePicture {}", what, i), true);
            }
        };
        if (fresh("api: nothing")) {
            queries("api: nothing", false);
            r.r("sjvtdFlushPicture", {d.handle}, "api: nothing: FlushPicture");
            queries("api: nothing: after FlushPicture", false);
            r.r("sjvtdDiscontinue", {d.handle}, "api: nothing: Discontinue");
            r.r("sjvtdCancelDecode", {d.handle}, "api: nothing: CancelDecode");
            queries("api: nothing: after CancelDecode", false);
            r.r("sjvtdReleaseInstance", {d.handle}, "api: nothing: ReleaseInstance");
        }
        for (const u32 mode : {0xFFu, 0x1FFu}) {
            const std::string what = std::format("api: mode {:#x}", mode);
            if (!fresh(what)) continue;
            std::memset(b + kMode, 0, 8);
            ps_write32(b, kMode, mode);
            r.r("sjvtdSetDecodeMode", {d.handle, kMode}, what + ": SetDecodeMode");
            feed(what, "I0 P2 b1 P4 b3 p5 P6 B7", 2200);
            std::memset(b + kMode, 0, 8);
            r.r("sjvtdSetDecodeMode", {d.handle, kMode}, what + ": SetDecodeMode 0");
            feed(what + " then 0", "p8 P9 b10", 2210);
            r.r("sjvtdFlushPicture", {d.handle}, what + ": FlushPicture");
            d.take(what + ": after FlushPicture", true);
            r.r("sjvtdReleaseInstance", {d.handle}, what + ": ReleaseInstance");
        }
        for (const std::string call : {"Discontinue", "CancelDecode"}) {
            const std::string what = "api: " + call;
            if (!fresh(what)) continue;
            feed(what, "I0 P1 P2", 2300, false);
            r.r("sjvtd" + call, {d.handle}, what + ": " + call);
            d.take(what + ": after " + call, true);
            queries(what + ": after " + call);
            // The stream goes on: P pictures, then an I picture that is not IDR, then P pictures.
            feed(what + ": then", "P3 P4 i5 P6", 2310);
            r.r("sjvtdFlushPicture", {d.handle}, what + ": FlushPicture");
            d.take(what + ": after FlushPicture", true);
            r.r("sjvtdReleaseInstance", {d.handle}, what + ": ReleaseInstance");
        }
        if (fresh("api: twice")) {
            feed("api: twice", "I0 P1 P2 P3", 2400, false);
            r.r("sjvtdFlushPicture", {d.handle}, "api: twice: FlushPicture");
            for (u32 k = 0; k < 3; k++) {
                std::memset(b + kPic, 0xEE, 0x40);
                r.r("sjvtdGetPicture", {d.handle, kPic}, std::format("api: twice: GetPicture {}", k));
                r.o.line(std::format("api: twice: GetPicture {}: its words", k), compare::hex(b + kPic, 0x24));
            }
            for (u32 k = 0; k < 4; k++)
                r.r("sjvtdReleasePicture", {d.handle}, std::format("api: twice: ReleasePicture {}", k));
            queries("api: twice: after");
            r.r("sjvtdReleaseInstance", {d.handle}, "api: twice: ReleaseInstance");
        }
        if (fresh("api: sizes")) {
            // Access units of 0 and 3 bytes, an IDR picture, one of 0 bytes again.
            d.decode("api: sizes: 0 bytes", {}, 2500);
            d.decode("api: sizes: 3 bytes", {0, 0, 1}, 2501);
            feed("api: sizes", "I0", 2502);
            d.decode("api: sizes: 0 bytes again", {}, 2503);
            r.r("sjvtdFlushPicture", {d.handle}, "api: sizes: FlushPicture");
            d.take("api: sizes: after FlushPicture", true);
            r.r("sjvtdReleaseInstance", {d.handle}, "api: sizes: ReleaseInstance");
        }
        // The instance's first sequence sets how many frames its decoded picture buffer may have: a later one (after
        // Initialize) that needs more stops the decoder for good.
        if (fresh("api: capacity")) {
            const Seq two = sequence("I0 P1 P2", 64, 64, [](hs::Sps& q) {
                q.max_num_ref_frames = 1;
                full_vui(q);
                q.vui.max_dec_frame_buffering = 2;
            });
            d.stream("api: capacity: 2 frames", two.aus, 2700);
            d.init("api: capacity: Initialize");
            const Seq one = sequence("I0 P1", 64, 64, [](hs::Sps& q) {
                q.max_num_ref_frames = 1;
                full_vui(q);
                q.vui.max_dec_frame_buffering = 1;
            });
            d.stream("api: capacity: 1 frame", one.aus, 2710);
            d.init("api: capacity: Initialize again");
            const Seq three = sequence("I0 P1", 64, 64, [](hs::Sps& q) {
                q.max_num_ref_frames = 1;
                full_vui(q);
                q.vui.max_dec_frame_buffering = 3;
            });
            d.stream("api: capacity: 3 frames", three.aus, 2720);
            queries("api: capacity: stopped");
            d.init("api: capacity: stopped: Initialize");
            r.r("sjvtdFlushPicture", {d.handle}, "api: capacity: stopped: FlushPicture");
            r.r("sjvtdReleaseInstance", {d.handle}, "api: capacity: ReleaseInstance");
        }
        if (fresh("api: too large")) {
            feed("api: too large", "I0 P1", 2600, false);
            const Seq big = sequence("I0 P1", 80, 64);
            d.decode("api: too large: an 80x64 picture", big.aus[0], 2610);
            queries("api: too large: after it");
            d.decode("api: too large: a 64x64 picture again", sequence("I0", 64, 64).aus[0], 2611);
            r.r("sjvtdFlushPicture", {d.handle}, "api: too large: FlushPicture");
            r.r("sjvtdDiscontinue", {d.handle}, "api: too large: Discontinue");
            r.r("sjvtdCancelDecode", {d.handle}, "api: too large: CancelDecode");
            std::memset(b + kMode, 0, 8);
            r.r("sjvtdSetDecodeMode", {d.handle, kMode}, "api: too large: SetDecodeMode");
            for (u32 k = 0; k < 4; k++) ps_write32(b, kAux + 4 * k, k);
            r.r("sjvtdSetAuxData", {d.handle, kAux}, "api: too large: SetAuxData");
            d.init("api: too large: Initialize");
            queries("api: too large: after Initialize");
            r.r("sjvtdReleaseInstance", {d.handle}, "api: too large: ReleaseInstance");
        }
    }
    // The sequence structure before a picture: an access unit of the parameter sets alone, then a picture, then
    // Initialize.
    if (selected("seq probes")) {
        sjvtd_param(b, 6, 1, kTs, 64, 64);
        ps_write32(b, kOut, 0);
        r.r("sjvtdGetMemorySize2", {kOut, 6, kParam}, "seq probes: GetMemorySize2");
        const u32 size = ps_read32(b, kOut);
        std::memset(b + kMemProbe, 0xA5, size + 0x1000);
        if (r.r("sjvtdCreateInstance2", {kMemProbe, size, 6, kParam}, "seq probes: CreateInstance2") == 0) {
            d.handle = kMemProbe;
            d.width = d.height = 64;
            d.init("seq probes: Initialize");
            const Seq z = sequence("Z", 64, 64);
            d.stream("seq probes: parameter sets", z.aus, 1400);
            const Seq ng = sequence("N G", 64, 64);
            d.stream("seq probes: no slices", ng.aus, 1405);
            {
                // A picture's access unit without its delimiter.
                hs::Sps sps;
                sps.width_mbs = sps.height_map_units = 4;
                sps.poc_type = 2;
                sps.profile = 77;
                sps.constraints = 0x40;
                const hs::Pps pps;
                hs::Slice sl;
                sl.kind = hs::Kind::I;
                sl.idr = true;
                hs::AuParts parts;
                parts.aud = false;
                parts.sps = parts.pps = true;
                d.stream("seq probes: no delimiter", {hs::access_unit(sps, pps, sl, h::pattern(64, 64, 1), parts)},
                    1407);
            }
            const Seq one = sequence("I0", 64, 64);
            d.stream("seq probes: a picture", one.aus, 1410);
            d.init("seq probes: Initialize again");
            d.seq("seq probes: after Initialize");
            r.r("sjvtdReleaseInstance", {d.handle}, "seq probes: ReleaseInstance");
        }
    }
    // The frame buffers' layout: an instance for each size, its first picture's places.
    for (const auto& [wd, ht] : {std::pair{16u, 16u}, {32u, 32u}, {48u, 48u}, {80u, 48u}, {176u, 144u}, {64u, 32u}}) {
        const std::string what = std::format("layout {}x{}", wd, ht);
        if (!selected(what)) continue;
        sjvtd_param(b, 6, 1, kTs, wd, ht);
        ps_write32(b, kOut, 0);
        r.r("sjvtdGetMemorySize2", {kOut, 6, kParam}, what + ": GetMemorySize2");
        const u32 size = ps_read32(b, kOut);
        r.o.line(what + ": the size", d.w(size));
        std::memset(b + kMemLayout, 0xA5, size + 0x1000);
        if (r.r("sjvtdCreateInstance2", {kMemLayout, size, 6, kParam}, what + ": CreateInstance2") != 0) continue;
        d.handle = kMemLayout;
        d.width = wd;
        d.height = ht;
        d.init(what + ": Initialize");
        const Seq sq = sequence("I0 P1", wd, ht);
        d.stream(what, sq.aus, 500);
        r.r("sjvtdReleaseInstance", {d.handle}, what + ": ReleaseInstance");
    }
    // The frame buffers' places by the instance's flags and frame count (64x64).
    // (The second flag 1 is not decoded with: Sony's module refuses every access unit then, answering 0x80615842, and
    // gives its frame buffers out as they were, with warnings; pspocket refuses them and gives none, D-920 part 8.)
    for (const auto& [f5, f6, frames] : {std::tuple{0u, 0u, ~0u}, {1u, 0u, ~0u}, {~0u, 0u, ~0u}, {~0u, ~0u, 2u},
             {~0u, ~0u, 4u}, {0u, 0u, 1u}}) {
        const std::string what = std::format("layout flags {} {} frames {}", static_cast<s32>(f5), static_cast<s32>(f6),
            static_cast<s32>(frames));
        if (!selected(what)) continue;
        sjvtd_param(b, 6, 1, kTs, 64, 64, f5, f6, frames);
        ps_write32(b, kOut, 0);
        r.r("sjvtdGetMemorySize2", {kOut, 6, kParam}, what + ": GetMemorySize2");
        const u32 size = ps_read32(b, kOut);
        r.o.line(what + ": the size", d.w(size));
        std::memset(b + kMemLayout, 0xA5, size + 0x1000);
        if (r.r("sjvtdCreateInstance2", {kMemLayout, size, 6, kParam}, what + ": CreateInstance2") != 0) continue;
        d.handle = kMemLayout;
        d.width = d.height = 64;
        d.init(what + ": Initialize");
        const Seq sq = sequence("I0 P1 P2 P3 P4", 64, 64, [](hs::Sps& q) { q.max_num_ref_frames = 1; });
        d.stream(what, sq.aus, 550);
        r.r("sjvtdReleaseInstance", {d.handle}, what + ": ReleaseInstance");
    }
    // The memory: sizes, flags and frame buffers.
    if (selected("sizes")) {
        for (const u32 level : {0u, 1u, 2u, 6u, 7u, 9u, 10u, 11u, 16u})
            for (const auto& [wd, ht] : {std::pair{16u, 16u}, {32u, 32u}, {64u, 64u}, {80u, 48u}, {176u, 144u},
                     {352u, 288u}, {720u, 480u}, {1280u, 720u}, {1920u, 1088u}})
                for (const u32 frames : {~0u, 1u, 2u, 4u, 16u})
                    for (const auto& [f5, f6] : {std::pair{~0u, ~0u}, {0u, 0u}, {1u, 0u}, {0u, 1u}}) {
                        sjvtd_param(b, 6, 1, 0, wd, ht, f5, f6, frames, 0);
                        ps_write32(b, kOut, 0xEEEEEEEE);
                        const std::string key = std::format("sizes: level {} {}x{} frames {} flags {} {}", level, wd,
                            ht, static_cast<s32>(frames), static_cast<s32>(f5), static_cast<s32>(f6));
                        r.r("sjvtdGetMemorySize2", {kOut, level, kParam}, key);
                        r.o.line(key + ": the size", d.w(ps_read32(b, kOut)));
                    }
    }
    end_spurs(r);
    return std::move(r.o);
}

} // namespace

TEST_CASE("The JVT core (libsjvtd): pspocket's own, answering as Sony's did (D-920)", "[hle][vdec-cores]")
{
    check_names("libsjvtd",
        {"sjvtdGetMemorySize", "sjvtdGetMemorySize2", "sjvtdCreateInstance2", "sjvtdReleaseInstance", "sjvtdInitialize",
            "sjvtdDecodePicture", "sjvtdFlushPicture", "sjvtdDiscontinue", "sjvtdCancelDecode", "sjvtdSetDecodeMode",
            "sjvtdGetNumPicture", "sjvtdGetPicture", "sjvtdReleasePicture", "sjvtdGetSideInfoPic", "sjvtdGetAuxData",
            "sjvtdGetSideInfoSeq", "sjvtdGetExistenceFlag", "sjvtdSetAuxData", "sjvtdGetVersionNumber"},
        false);
    Setup s;
    s.mode = Mode::Self;
    const Observed o = sjvtd(s, false);
    // A step's line by its start and end (the pictures' numbers count every stream's before them).
    const auto value = [&](std::string_view step, std::string_view what) {
        for (const Line& l : o.lines)
            if (l.key.starts_with(step) && l.key.ends_with(what)) return l.value;
        return std::string("(missing)");
    };
    const auto frame = [&](std::string_view step) -> const std::vector<u8>& {
        for (const Data& d : o.data)
            if (d.key.starts_with(step) && d.key.ends_with(": its frame buffer")) return d.bytes;
        FAIL("no frame for " << step);
        static const std::vector<u8> none;
        return none;
    };
    // Sony's answers (this file's comparison, firmware 4.92).
    CHECK(o.value("GetVersionNumber: the version") == "0x1050000");
    CHECK(o.value("GetMemorySize level 0: the size") == "0x6b03fd0");
    CHECK(o.value("GetMemorySize level 1: the size") == "0x2ed5d0");
    CHECK(o.value("GetMemorySize level 10: the size") == "0x44065d0");
    CHECK(o.value("GetMemorySize level 17") == "0x80615842");
    CHECK(o.value("GetMemorySize2 type 6 level 6 64x64 -1 -1 -1 0: the size") == "0x17dad0");
    CHECK(o.value("GetMemorySize2 type 6 level 6 64x64 -1 -1 -1 0 with a taskset: the size") == "0x17b1d0");
    CHECK(o.value("GetMemorySize2 type 6 level 6 64x64 0 0 4 0 with a taskset: the size") == "0xeded0");
    CHECK(o.value("GetMemorySize2 type 4 level 6 64x64 -1 -1 -1 0: the size") == "0xc23350");
    CHECK(o.value("CreateInstance2 with one byte less") == "0x80615842");
    CHECK(o.value("CreateInstance2 with no taskset") == "0x80615842");
    CHECK(o.value("after CreateInstance2: the flag") == "0xa5a5a5a5"); // the memory's own word: not set
    // An IDR picture with its parameter sets and delimiter, then P pictures: the existence flag, the bytes used, the
    // pictures held by the level's buffer until FlushPicture, in 16-sample strips with 32 lines of edge.
    CHECK(o.value("I P P P: DecodePicture 0: used") == "0x1843");
    CHECK(o.value("I P P P: DecodePicture 0: the flag") == "0x13");
    CHECK(o.value("I P P P: DecodePicture 1: the flag") == "0x10");
    CHECK(o.value("I P P P: after FlushPicture: GetNumPicture") == "0x80615822");
    CHECK(o.value("I P P P: after FlushPicture: pictures") == "0x4");
    CHECK(value("I P P P: after FlushPicture: picture ", ": the picture") ==
          " the core's memory +0x103900 the core's memory +0x106400 the core's memory +0x106408 0x40000004 0x40 0x40 "
          "0x80 0x40 0x0 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee "
          "0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee "
          "0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee 0xeeeeeeee");
    CHECK(o.value("after the stream: the sequence")
              .starts_with("hex:0000000100000001000000010000000000000000000000400000004000000000"));
    CHECK(o.value("seq fields: crop: at the end: the sequence")
              .starts_with("hex:0000000100000001000000010000000100000000000000400000004000000000"));
    {
        // The first picture's samples (I_PCM, h264_writer.hpp's pattern 0) where the strips put them; its edge strips
        // repeat its first and last columns, its lines of edge are as the memory was (0xA5).
        const std::vector<u8>& f = frame("I P P P: after FlushPicture: picture 0");
        REQUIRE(f.size() == 96 * 128 * 3 / 2);
        const h::Picture p = h::pattern(64, 64, 0);
        const auto luma = [&](int x, int y) {
            return f[static_cast<usize>(((x + 16) / 16) * 16 * 128 + (y + 32) * 16 + (x + 16) % 16)];
        };
        CHECK(luma(0, 0) == p.y[0]);
        CHECK(luma(37, 41) == p.y[41 * 64 + 37]);
        CHECK(luma(-5, 3) == p.y[3 * 64]);
        CHECK(luma(70, 3) == p.y[3 * 64 + 63]);
        CHECK(luma(5, -1) == 0xA5);
        CHECK(luma(5, 64) == 0xA5);
    }
    // B pictures out in their order, each in the lowest frame buffer free.
    CHECK(value("I P B: after FlushPicture: picture 5", ": the picture").find(" 0x40000004 0x40 0x40 0x80 0x40 0x2 ") !=
          std::string::npos);
    // The decoded picture buffer: the level's (16 frames: the 17th picture out at the 17th access unit), the VUI's
    // (two frames).
    CHECK(o.value("level 10: after DecodePicture 15: pictures") == "0x0");
    CHECK(o.value("level 10: after DecodePicture 16: pictures") == "0x1");
    CHECK(o.value("buffering 2: after DecodePicture 1: pictures") == "0x0");
    CHECK(o.value("buffering 2: after DecodePicture 2: pictures") == "0x1");
    // Field pairs: a frame of two fields' blocks.
    CHECK(value("fields: after FlushPicture: picture ", ": the picture").find(" 0x40000810 ") != std::string::npos);
    CHECK(value("fields: after FlushPicture: picture ", ": side information").starts_with("0x2 "));
    // SEI: the flag's buffering period and picture timing bits; pic_struct in the structure word; the HRD's times.
    CHECK(o.value("sei kinds: DecodePicture 0: the flag") == "0x313");
    CHECK(o.value("sei kinds: DecodePicture 1: the flag") == "0x210");
    CHECK(o.value("sei kinds: DecodePicture 2: the flag") == "0x110");
    CHECK(o.value("sei kinds: DecodePicture 3: the flag") == "0x10");
    CHECK(value("sei kinds: after DecodePicture 6: picture ", ": the picture").find(" 0x40004014 ") !=
          std::string::npos);
    CHECK(value("sei kinds: after DecodePicture 8: picture ", ": the picture").find(" 0x40001004 ") !=
          std::string::npos);
    CHECK(o.value("hrd times: after DecodePicture 1: pictures") == "0x0");
    CHECK(o.value("hrd times: after DecodePicture 2: pictures") == "0x1");
    CHECK(o.value("dpb change: after DecodePicture 3: pictures") == "0x3");
    // Access units of parameter sets alone, of a delimiter alone, of no NAL unit.
    CHECK(o.value("seq probes: parameter sets: DecodePicture 0") == "0x0");
    CHECK(o.value("seq probes: no slices: DecodePicture 0") == "0x80615820");
    CHECK(o.value("seq probes: no slices: DecodePicture 1") == "0x80615808");
    // The decode mode 0xFF passes the non-reference pictures over; CancelDecode's P pictures warn until an IDR one; a
    // picture larger than the instance's is refused (nothing used); a sequence that needs more frames than the
    // instance's first stops the decoder.
    CHECK(o.value("api: mode 0xff: DecodePicture 2") == "0x80615828");
    CHECK(o.value("api: mode 0xff: DecodePicture 3") == "0x0");
    CHECK(value("api: CancelDecode: after FlushPicture: picture ", ": GetPicture") == "0x8061581b");
    CHECK(o.value("api: too large: an 80x64 picture") == "0x80615821");
    CHECK(o.value("api: too large: an 80x64 picture: used") == "0x0");
    CHECK(o.value("api: capacity: 3 frames: DecodePicture 0") == "0x80615881");
    CHECK(o.value("api: capacity: 3 frames: DecodePicture 1") == "0x80615880");
    CHECK(o.value("api: capacity: ReleaseInstance") == "0x80615880");
}

TEST_CASE("The JVT core (libsjvtd) against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    const Setup s = setup();
    run(s, "libsjvtd against Sony's", [&](bool sony) { return sjvtd(s, sony); }, [](const Observed&) {});
}
