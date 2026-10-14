// libavcdec and libmvcdec, the H.264 and MVC codec cores, against Sony's modules (D-933): each core called directly as
// Sony's libvdec (or a game) calls it (libmvcdec with the same lives with one view, and its own with two), with the
// same synthetic streams through pspocket's core and through Sony's module (the user's installed system software), each
// in a process of its own. Sony's core decodes on SPURS: each half makes a SPURS instance (pspocket's cellSpurs) and
// gives it to the core's open, or lets the core make its own. Compared: every answer, every message the core gives its
// callback (in order, with what they point at: the side data, the picture's information), the frame buffers' places (as
// offsets into the core's memory) and their contents, the status. The core's own memory past what its messages point at
// (its SPU side's working memory, queues, task contexts) is opaque: not compared. Skipped without the firmware
// (compare_util.hpp). No game data: the streams are written by the tests (h264_streams.hpp).

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "compare_util.hpp"
#include "h264_streams.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
namespace h = pspocket::hle::test::h264;
namespace hs = pspocket::hle::test::h264s;
namespace as = pspocket::recomp::ppu::as;

namespace {

// The lives a run takes (none: every one PSPOCKET_COMPARE_ONLY selects) and how long a callback stays quiet before the
// test looks (Sony's core answers from its SPU side, pspocket's at once).
const std::vector<std::string>* g_only = nullptr;
std::chrono::milliseconds g_quiet{400}, g_timeout{15000};

// Which core a life drives: libavcdec, or libmvcdec (the same lives with one view, and its two views' own).
bool g_mvc = false;
std::string fn(std::string_view name) { return std::string(g_mvc ? "mvcDec" : "avcDec") + std::string(name); }
std::string library() { return g_mvc ? "libmvcdec" : "libavcdec"; }

bool pick(std::string_view life)
{
    if (!selected(life)) return false;
    if (!g_only) return true;
    for (const std::string& o : *g_only)
        if (life.starts_with(o)) return true;
    return false;
}

// Guest memory: the core's memory, SPURS, the callback and its log, the arguments and the streams.
constexpr u32 kMem = 0x01000000, kMemSize = 0x06000000;
constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000;
constexpr u32 kCode = kData + 0x8000, kOpd = kData + 0x8800, kArg = kData + 0x9000;
constexpr u32 kParam = kArg, kAttr = kArg + 0x20, kMemDesc = kArg + 0x40, kHandle = kArg + 0x60, kTaskset = kArg + 0x80,
              kPrios = kArg + 0xA0, kStatus = kArg + 0xC0, kSide = kArg + 0x100, kPieces = kArg + 0x180;
constexpr u32 kLog = kData + 0x40000, kEntry = 0x300, kLogMax = 0x200;
constexpr u32 kStream = kData + 0x180000, kStreamSize = 0x60000;

struct AvcFixture : Fixture {
    AvcFixture()
    {
        REQUIRE(hle->main_memory().reserve(kMem, kMemSize));
        REQUIRE(mem.commit(kMem, kMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};
using Run = Half<AvcFixture>;

// A small assembler with labels, for the callback.
struct Asm {
    std::vector<u32> code;
    std::vector<std::pair<usize, std::string>> fix;
    std::map<std::string, usize> labels;
    void emit(u32 i) { code.push_back(i); }
    void label(const std::string& l) { labels[l] = code.size(); }
    void bc(u32 bo, u32 bi, const std::string& l)
    {
        fix.push_back({code.size(), l});
        emit(as::bc_rel(bo, bi, 0));
    }
    void b(const std::string& l)
    {
        fix.push_back({code.size(), "b:" + l});
        emit(as::b_rel(0));
    }
    void copy_words(u32 words, const std::string& l) // r4 -> r11, r6 the count
    {
        emit(as::li(6, static_cast<s32>(words)));
        emit(as::mtctr(6));
        label(l);
        emit(as::lwz(0, 0, 4));
        emit(as::stw(0, 0, 11));
        emit(as::addi(4, 4, 4));
        emit(as::addi(11, 11, 4));
        bc(16, 0, l);
    }
    void done()
    {
        for (auto& [at, l] : fix) {
            const bool uncond = l.starts_with("b:");
            const std::string name = uncond ? l.substr(2) : l;
            const s32 disp = static_cast<s32>((labels.at(name) - at) * 4);
            if (uncond) code[at] = as::b_rel(disp);
            else code[at] = (code[at] & ~0xFFFCu) | (static_cast<u32>(disp) & 0xFFFC);
        }
    }
};

// The callback (r3: the message): an entry of kEntry bytes in the log at kLog + 16: the message's 0x50 bytes, then for
// a picture (type 1) 0x200 bytes from the information it points at (+8), for an access unit done (type 0) 0x40 bytes
// of the side data it points at (+0x10; libmvcdec's +0x28, when it points). The count at kLog is raised last, through
// the reservation (as vdec_test_util.hpp's callback, so ThreadSanitizer sees the order).
constexpr u32 kPayload = 0x50; // an entry's payload after the message
void write_callback(u8* b)
{
    Asm a;
    a.emit(as::lis(10, static_cast<s32>(kLog >> 16)));
    a.emit(as::ori(10, 10, kLog & 0xFFFF));
    a.emit(as::lwz(9, 0, 10));
    a.emit(as::mulli(11, 9, static_cast<s32>(kEntry)));
    a.emit(as::add(11, 11, 10));
    a.emit(as::addi(11, 11, 16));
    a.emit(as::mr(4, 3));
    a.copy_words(kPayload / 4, "c1");
    a.emit(as::lwz(5, 0, 3));
    a.emit(as::cmpwi(0, 5, 1));
    a.bc(4, 2, "t0");
    a.emit(as::lwz(4, 8, 3));
    a.copy_words(0x200 / 4, "c2");
    a.b("end");
    a.label("t0");
    a.emit(as::cmpwi(0, 5, 0));
    a.bc(4, 2, "end");
    a.emit(as::lwz(4, g_mvc ? 0x28 : 0x10, 3));
    a.emit(as::cmpwi(0, 4, 0));
    a.bc(12, 2, "end");
    a.copy_words(16, "c3");
    a.label("end");
    a.emit(as::lwsync());
    a.label("inc");
    a.emit(as::lwarx(9, 0, 10));
    a.emit(as::addi(9, 9, 1));
    a.emit(as::stwcx_(9, 0, 10));
    a.bc(4, 2, "inc");
    a.emit(as::li(3, 0));
    a.emit(as::blr());
    a.done();
    for (usize i = 0; i < a.code.size(); i++) ps_write32(b, kCode + 4 * static_cast<u32>(i), a.code[i]);
    ps_write32(b, kOpd, kCode);
    ps_write32(b, kOpd + 4, 0);
    std::memset(b + kLog, 0, 16 + kEntry * kLogMax);
}

u32 log_count(u8* b) { return std::min(line_word(b, kLog), kLogMax); }

std::string hex32(u32 v) { return std::format("{:#x}", v); }

// A SPURS instance with two SPUs, as libvdec makes one (pspocket's cellSpurs in both halves).
void make_spurs(Run& r)
{
    REQUIRE(r.t.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 2, 200, 1000, 1) == 0);
    REQUIRE(r.t.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
}

void end_spurs(Run& r) { r.t.call("cellSpursFinalize", kSpurs); }

// avcDecQueryMemory's and avcDecOpen's parameters.
void params(u8* b, u32 level, u32 deblock = 0, u32 buffers = 0, u32 wmbs = 0, u32 hmbs = 0)
{
    std::memset(b + kParam, 0, 8);
    ps_write8(b, kParam + 0, static_cast<u8>(level));
    ps_write8(b, kParam + 1, static_cast<u8>(deblock));
    ps_write8(b, kParam + 2, static_cast<u8>(buffers));
    ps_write16(b, kParam + 4, static_cast<u16>(wmbs));
    ps_write16(b, kParam + 6, static_cast<u16>(hmbs));
}

// The frame buffer bytes of a picture of this coded size (Sony's layout, D-196).
u32 frame_bytes(u32 w, u32 h) { return (w + 64) * h + (w + 64) * h / 2 + 2048; }

// Open as each core takes it: libavcdec's thirteen arguments; libmvcdec's twelve (no SDK version, the seventh).
u64 call_open(Run& r, std::vector<u64> a)
{
    if (g_mvc) {
        a.erase(a.begin() + 6);
        return r.side.call("mvcDecOpen", {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]})
            .value_or(~0ull);
    }
    return r.side.call("avcDecOpen", {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12]})
        .value_or(~0ull);
}

// AccessUnit as each core takes it: libavcdec's pieces and stamps as arguments (the side data's two past eight),
// libmvcdec's pieces in a structure of eight words (the base view's, then the second view's).
u64 call_au(Run& r, u32 handle, u32 addr, u32 size, u32 cont, u32 cont_size, u64 dts, u64 pts, u32 mode, u32 side = 0,
    u32 side_size = 0, u32 dep = 0, u32 dep_size = 0, u32 pieces = kPieces)
{
    if (g_mvc) {
        if (pieces == kPieces) {
            const u32 words[8] = {addr, size, cont, cont_size, dep, dep_size, 0, 0};
            for (u32 i = 0; i < 8; i++) ps_write32(r.b, kPieces + 4 * i, words[i]);
        }
        return r.side.call("mvcDecAccessUnit", {handle, pieces, dts, pts, mode, side, side_size}).value_or(~0ull);
    }
    if (side || side_size)
        return r.side.call("avcDecAccessUnit", {handle, addr, size, cont, cont_size, dts, pts, mode, side, side_size})
            .value_or(~0ull);
    return r.side.call("avcDecAccessUnit", {handle, addr, size, cont, cont_size, dts, pts, mode}).value_or(~0ull);
}

struct Avc {
    Run& r;
    u32 handle = 0, mem = kMem, size = 0;
    u32 seen = 0; // log entries read
    u32 at = kStream;
    u32 width = 64, height = 64;
    std::vector<u32> held{}; // frame IDs given and not released
    bool release = true;   // release each picture as it comes
    // Within a sequence an access unit with no slice makes Sony's core vary as its SPU side's timing goes (two runs
    // differ): the frame buffer of its picture in error (blank, a copy of a picture moved by a line, or left as it
    // was) and the NAL unit flags (AUD, SPS, PPS, filler, ends) of the pictures after it (their own, or another
    // access unit's). Those are not compared there (D-933).
    bool error_frames = true, nal_flags = true;
    u32 pictures = 0;
    std::string label{};
    std::vector<std::vector<u8>> seconds{}; // libmvcdec: the second views stream() gives

    std::string in_mem(u32 addr) const
    {
        if (addr >= mem && addr < mem + size) return std::format("mem+{:#x}", addr - mem);
        return hex32(addr);
    }
    std::string in_ctx(u32 addr) const
    {
        if (handle && addr >= handle && addr < handle + (g_mvc ? 0x1F80u : 0x1380u)) return std::format("ctx+{:#x}", addr - handle);
        return in_mem(addr);
    }

    // Waits until the log has `want` more entries (at most `timeout`), then until it is quiet for `quiet`.
    void settle(u32 want, std::chrono::milliseconds quiet = g_quiet, std::chrono::milliseconds timeout = g_timeout)
    {
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (log_count(r.b) < seen + want && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        u32 last = log_count(r.b);
        auto since = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - since < quiet) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (log_count(r.b) != last) {
                last = log_count(r.b);
                since = std::chrono::steady_clock::now();
            }
        }
    }

    // The messages that came since the last look: each a line (and a picture's frame buffer as data).
    void messages(const std::string& when)
    {
        u8* b = r.b;
        const u32 n = log_count(b);
        for (; seen < n; seen++) {
            const u32 e = kLog + 16 + seen * kEntry;
            const u32 type = ps_read32(b, e);
            const std::string key = std::format("{}{}: message {}", label, when, seen);
            if (type == 0 && g_mvc) {
                // libmvcdec's: every piece and its size, then the flags, the code and the pictures it gives
                const u32 side = ps_read32(b, e + 0x28), side_size = ps_read32(b, e + 0x2C);
                r.o.line(key, std::format("AU done: pieces {} arg {} side {} ({} bytes{}) flags {}",
                    compare::hex(b + e + 8, 0x20), hex32(ps_read32(b, e + 4)), in_ctx(side), side_size,
                    side_size ? " " + compare::hex(b + e + kPayload, std::min<u32>(side_size, 0x40)) : std::string(),
                    compare::hex(b + e + 0x30, 0x11)));
            } else if (type == 0) {
                const u32 side = ps_read32(b, e + 0x10), side_size = ps_read32(b, e + 0x14);
                r.o.line(key, std::format("AU done: au {} cont {} arg {} side {} ({} bytes{}) flags {}", hex32(ps_read32(b, e + 8)),
                    hex32(ps_read32(b, e + 0x0C)), hex32(ps_read32(b, e + 4)), in_ctx(side), side_size,
                    side_size ? " " + compare::hex(b + e + kPayload, std::min<u32>(side_size, 0x40)) : std::string(),
                    compare::hex(b + e + 0x18, 8)));
            } else if (type == 1) {
                // libavcdec's information (0x178 bytes) or libmvcdec's (0x158): the frame buffer and side data
                // pointers shown apart
                const u32 bytes = g_mvc ? 0x158 : 0x178, at_frame = g_mvc ? 0xAC : 0xA4, at_sides = g_mvc ? 0x9C : 0x94;
                std::vector<u8> info(b + e + kPayload, b + e + kPayload + bytes);
                const u32 frame = ps_read32(info.data(), at_frame);
                const u32 side0 = ps_read32(info.data(), at_sides), side1 = ps_read32(info.data(), at_sides + 4);
                const u32 id = ps_read32(info.data(), 0);
                std::memset(info.data() + at_frame, 0, 4);
                std::memset(info.data() + at_sides, 0, 8);
                // Padding Sony's PPU side never writes: its helper thread's stack as it was (0 in a new thread, what
                // its Reset left after one). Not compared (D-933).
                if (g_mvc) {
                    std::memset(info.data() + 0x71, 0, 7);  // between the last byte and the DTS
                    info[0x153] &= 0xFE;                    // the flags' low 33 bits
                    std::memset(info.data() + 0x154, 0, 4);
                } else {
                    std::memset(info.data() + 0x0D, 0, 3);
                    std::memset(info.data() + 0xCA, 0, 2);
                }
                if (!nal_flags) info[g_mvc ? 0x150 : 0x174] &= g_mvc ? 0x1F : 0x03;
                r.o.line(key, std::format("picture: arg {} frame {} sides {} {}", hex32(ps_read32(b, e + 4)), in_mem(frame),
                    in_ctx(side0), in_ctx(side1)));
                r.o.line(key + ": information", compare::hex(info));
                const bool in_error = info[4] && info[5];
                if (frame >= mem && frame + frame_bytes(width, height) <= mem + kMemSize && (error_frames || !in_error))
                    r.o.bytes(key + ": its frame buffer", b + frame, frame_bytes(width, height));
                pictures++;
                held.push_back(id);
            } else {
                r.o.line(key, std::format("type {} arg {}", type, hex32(ps_read32(b, e + 4))));
            }
        }
    }

    void release_all(const std::string& when, usize keep = 0)
    {
        while (held.size() > keep) {
            const u32 id = held.front();
            held.erase(held.begin());
            r.r(fn("RlsFrame"), {handle, id}, std::format("{}{}: RlsFrame({})", label, when, id));
        }
    }

    void status(const std::string& when)
    {
        if (g_mvc) return; // libmvcdec has no GetStatus
        std::memset(r.b + kStatus, 0xEE, 8);
        r.r(fn("GetStatus"), {handle, kStatus}, std::format("{}{}: GetStatus", label, when));
        r.o.line(std::format("{}{}: the status", label, when), compare::hex(r.b + kStatus, 8));
    }

    // One access unit: its answer, the messages it made, the pictures released (as `release` says). `second`: the
    // second view's piece (libmvcdec).
    u64 au(const std::string& key, const std::vector<u8>& data, u32 mode = 0, u64 dts = 0, u64 pts = 0, u32 side = 0,
        u32 side_size = 0, u32 want = 1, const std::vector<u8>& second = {})
    {
        u8* b = r.b;
        if (at + data.size() + second.size() + 0x100 > kStream + kStreamSize) at = kStream;
        std::memcpy(b + at, data.data(), data.size());
        const u32 first_at = at;
        at = (at + static_cast<u32>(data.size()) + 0x7F) & ~0x7Fu;
        const u32 second_at = at;
        if (!second.empty()) {
            std::memcpy(b + at, second.data(), second.size());
            at = (at + static_cast<u32>(second.size()) + 0x7F) & ~0x7Fu;
        }
        const u64 v = call_au(r, handle, first_at, static_cast<u32>(data.size()), 0, 0, dts, pts, mode, side, side_size,
            second.empty() ? 0 : second_at, static_cast<u32>(second.size()));
        r.o.line(label + key, compare::answer(v));
        if (static_cast<u32>(v) == 0) settle(want);
        messages(key);
        if (release) release_all(key);
        return v;
    }

    void flush(const std::string& key)
    {
        if (r.r(fn("Flush"), {handle, 0}, label + key) == 0) settle(1, g_quiet + std::chrono::milliseconds(200));
        messages(key);
        if (release) release_all(key);
    }
};

// ------------------------------------------------------------------------------------------------- streams

// A picture sequence: each token a picture type (I an IDR, i an I picture, P/p a P picture referenced or not, B/b a B
// picture, B_Skip, referenced or not) and its display index (picture order count 2 x the index, type 0). The first IDR
// access unit has the SPS and PPS; a token may ask for them: '+' both, '%' the SPS, '#' the PPS. Access units with no
// picture: G 16 bytes with no start code, N an access unit delimiter alone, Z the SPS and PPS alone.
struct Seq {
    hs::Sps sps;
    hs::Pps pps;
    std::vector<std::vector<u8>> aus;
    std::vector<int> display;               // each access unit's display index
    std::vector<std::vector<u8>> seconds;   // libmvcdec: each access unit's second view (see `second`)
};

// `second` (libmvcdec): each access unit's second view (its subset SPS and PPS with the first): 'I' I_PCM pictures of
// their own, 'A' anchors predicted from the base view (P_Skip), 'T' P_Skip from the second view's last picture (B_Skip
// for B pictures; I_PCM at an IDR picture).
Seq sequence(const std::string& tokens, u32 w = 64, u32 hgt = 64, const std::function<void(hs::Sps&)>& setup = {},
    char second = 0)
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
        while (pos < tokens.size() && std::isdigit(static_cast<unsigned char>(tokens[pos]))) idx = idx * 10 + (tokens[pos++] - '0');
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
            s.display.push_back(idx);
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
        s.aus.push_back(hs::access_unit(s.sps, s.pps, sl, h::pattern(static_cast<int>(w), static_cast<int>(hgt), idx), parts));
        s.display.push_back(idx);
        if (second) {
            hs::Pps dpps = s.pps;
            dpps.id = 1;
            hs::Slice ds = sl;
            ds.pps_id = 1;
            bool anchor = sl.idr;
            if (second == 'I') {
                ds.kind = hs::Kind::I;
            } else if (second == 'A') {
                ds.kind = hs::Kind::P;
                anchor = true;
            } else {
                ds.kind = sl.idr ? hs::Kind::I : sl.kind;
            }
            const bool with_sets = s.seconds.empty();
            s.seconds.push_back(hs::dependent_unit(s.sps, dpps, ds,
                h::pattern(static_cast<int>(w), static_cast<int>(hgt), 100 + idx), anchor, with_sets, with_sets));
        }
    }
    return s;
}

// The access units' DTS and PTS: Stamp::none all ones (not given).
constexpr u64 kNone = ~0ull;
using Stamps = std::function<std::pair<u64, u64>(usize decode, int display)>;
const Stamps kBackwards = [](usize i, int) { return std::pair<u64, u64>{0x1000 + i, 0x800 + i}; }; // PTS before DTS

// ------------------------------------------------------------------------------------------------- the lives

// Opens a decoder at level `level` with these limits and SPURS (0: the core's own); its handle, or 0.
u32 open(Run& r, Avc& a, const std::string& key, u32 level, u32 buffers = 0, u32 wmbs = 0, u32 hmbs = 0, u32 spurs = kSpurs,
    u32 sdk = 0x360001)
{
    u8* b = r.b;
    params(b, level, 0, buffers, wmbs, hmbs);
    std::memset(b + kAttr, 0xEE, 12);
    if (r.r(fn("QueryMemory"), {kParam, kAttr}, key + ": QueryMemory") != 0) return 0;
    r.o.line(key + ": the memory", compare::hex(b + kAttr + 4, 8));
    a.size = ps_read32(b, kAttr + 4);
    ps_write32(b, kMemDesc + 0, kMem);
    ps_write32(b, kMemDesc + 4, a.size);
    ps_write32(b, kMemDesc + 8, 0x80);
    for (u32 i = 0; i < 8; i++) ps_write8(b, kPrios + i, 1);
    // The callback's log starts again (no decoder runs now), and the memory is filled first, so what the core leaves
    // unwritten shows.
    std::memset(b + kLog, 0, 16 + kEntry * kLogMax);
    a.seen = 0;
    if (std::getenv("AVC_FILL_A5")) std::memset(b + kMem, 0xA5, a.size);
    else if (std::getenv("AVC_FILL_0")) std::memset(b + kMem, 0, a.size);
    else for (u32 i = 0; i < a.size; i += 4) ps_write32(b, kMem + i, 0xA5A50000u | (i & 0xFFFF));
    ps_write32(b, kHandle, 0xEEEEEEEE);
    ps_write32(b, kTaskset, 0xEEEEEEEE);
    const u64 v = call_open(r, {kHandle, spurs, kTaskset, kParam, kMemDesc, 0x4000, sdk, kOpd, 0x1234, 1, kPrios, 200, 1000});
    r.o.line(key + ": Open", compare::answer(v));
    if (static_cast<u32>(v) != 0) return 0;
    a.handle = ps_read32(b, kHandle);
    r.o.line(key + ": the handle", a.in_mem(a.handle));
    r.o.line(key + ": the taskset", a.in_mem(ps_read32(b, kTaskset)));
    return a.handle;
}

void close(Run& r, Avc& a, const std::string& key)
{
    a.release_all(key + ": before Close");
    r.r(fn("Close"), {a.handle}, key + ": Close");
}

// A whole stream: each access unit, its pictures taken and released; then the flush.
void stream(Run& r, Avc& a, const std::string& key, const std::vector<std::vector<u8>>& aus, u32 mode = 0,
    const Stamps& stamps = kBackwards, const std::vector<int>& display = {})
{
    for (usize i = 0; i < aus.size(); i++) {
        const auto [dts, pts] = stamps(i, i < display.size() ? display[i] : static_cast<int>(i));
        a.au(std::format("{}: AccessUnit {}", key, i), aus[i], mode, dts, pts, 0, 0, 1,
            i < a.seconds.size() ? a.seconds[i] : std::vector<u8>{});
    }
    a.status(key + ": before Flush");
    a.flush(key + ": Flush");
    a.status(key + ": after Flush");
    (void)r;
}

Observed avc_life(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, library());
    u8* b = r.b;
    write_callback(b);
    make_spurs(r);
    // QueryMemory: every level, then the caller's limits, then refusals.
    if (pick("QueryMemory"))
    for (const u32 level : {9u, 10u, 11u, 12u, 13u, 20u, 21u, 22u, 30u, 31u, 32u, 40u, 41u, 42u, 43u, 50u}) {
        params(b, level);
        std::memset(b + kAttr, 0xEE, 12);
        r.r(fn("QueryMemory"), {kParam, kAttr}, std::format("QueryMemory level {}", level));
        r.o.line(std::format("QueryMemory level {}: the attribute", level), compare::hex(b + kAttr, 12));
    }
    struct Q {
        u32 level, deblock, buffers, w, h;
    };
    for (const Q& q : {Q{21, 1, 0, 0, 0}, Q{21, 2, 0, 0, 0}, Q{21, 3, 0, 0, 0}, Q{21, 0, 4, 0, 0}, Q{21, 0, 14, 0, 0},
             Q{21, 0, 15, 0, 0}, Q{21, 0, 0, 4, 4}, Q{21, 0, 0, 3, 4}, Q{21, 0, 0, 22, 18}, Q{21, 0, 0, 30, 30},
             Q{40, 0, 0, 120, 68}, Q{40, 0, 0, 121, 68}, Q{40, 0, 4, 120, 68}, Q{42, 0, 16, 4, 4}, Q{42, 0, 17, 4, 4},
             Q{30, 0, 0, 20, 15}}) {
        params(b, q.level, q.deblock, q.buffers, q.w, q.h);
        std::memset(b + kAttr, 0xEE, 12);
        const std::string key = std::format("QueryMemory level {} deblock {} buffers {} {}x{} MBs", q.level, q.deblock,
            q.buffers, q.w, q.h);
        r.r(fn("QueryMemory"), {kParam, kAttr}, key);
        r.o.line(key + ": the attribute", compare::hex(b + kAttr, 12));
    }
    params(b, 21);
    r.r(fn("QueryMemory"), {0, kAttr}, "QueryMemory with no parameters");
    r.r(fn("QueryMemory"), {kParam, 0}, "QueryMemory with no attribute");

    // Open's refusals.
    {
        params(b, 21);
        r.r(fn("QueryMemory"), {kParam, kAttr}, "Open's refusals: QueryMemory");
        const u32 need = ps_read32(b, kAttr + 4);
        const auto open_with = [&](const std::string& what, std::vector<u64> args) {
            const u64 v = call_open(r, std::move(args));
            r.o.line("Open " + what, compare::answer(v));
            if (static_cast<u32>(v) == 0) r.side.call(fn("Close"), {ps_read32(b, kHandle)});
        };
        ps_write32(b, kMemDesc + 0, kMem);
        ps_write32(b, kMemDesc + 4, need);
        ps_write32(b, kMemDesc + 8, 0x80);
        for (u32 i = 0; i < 8; i++) ps_write8(b, kPrios + i, 1);
        open_with("with no handle", {0, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 1, kPrios, 200, 1000});
        open_with("with SPURS and no priorities", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234,
                                                      1, 0, 200, 1000});
        open_with("with no parameters", {kHandle, kSpurs, kTaskset, 0, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 1, kPrios, 200, 1000});
        open_with("with no callback", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, 0, 0x1234, 1, kPrios, 200, 1000});
        open_with("with no memory", {kHandle, kSpurs, kTaskset, kParam, 0, 0x4000, 0x360001, kOpd, 0x1234, 1, kPrios, 200, 1000});
        open_with("with a stack of 0xfff", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0xFFF, 0x360001, kOpd, 0x1234, 1,
                                               kPrios, 200, 1000});
        open_with("with 0 SPUs", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 0, kPrios, 200, 1000});
        open_with("with 9 SPUs", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 9, kPrios, 200, 1000});
        open_with("with SPU priority 256", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 1,
                                               kPrios, 256, 1000});
        open_with("with PPU priority 3072", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 1,
                                                kPrios, 200, 3072});
        params(b, 43);
        open_with("at level 43", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 1, kPrios, 200, 1000});
        params(b, 21, 3);
        open_with("with deblocking 3", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 1, kPrios,
                                           200, 1000});
        params(b, 21);
        ps_write32(b, kMemDesc + 4, need - 1);
        open_with("with one byte less", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 1, kPrios,
                                            200, 1000});
        ps_write32(b, kMemDesc + 4, need);
        ps_write32(b, kMemDesc + 8, 0x40);
        open_with("aligned to 0x40", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234, 1, kPrios,
                                         200, 1000});
        ps_write32(b, kMemDesc + 8, 0x80);
        ps_write32(b, kMemDesc + 0, kMem + 0x40);
        open_with("at an address off 128 bytes", {kHandle, kSpurs, kTaskset, kParam, kMemDesc, 0x4000, 0x360001, kOpd, 0x1234,
                                                     1, kPrios, 200, 1000});
        ps_write32(b, kMemDesc + 0, kMem);
    }

    // A life over IDR pictures: each picture released at once.
    if (pick("IDR")) {
        Avc a{r};
        a.label = "IDR: ";
        if (open(r, a, "IDR", 21)) {
            a.status("after Open");
            std::vector<std::vector<u8>> aus;
            for (int i = 0; i < 6; i++) aus.push_back(h::access_unit(h::pattern(64, 64, i), static_cast<u32>(i & 1)));
            stream(r, a, "six", aus);
            close(r, a, "IDR");
        }
    }
    // I and P pictures (a buffer of one, then the level's), B pictures.
    struct Life {
        const char* name;
        const char* tokens;
        std::function<void(hs::Sps&)> setup;
    };
    const Life lives[] = {
        {"I P, a buffer of 1", "I0 P1 P2 P3 P4 P5", [](hs::Sps& s) {
             s.vui.present = s.vui.restriction = true;
             s.vui.max_dec_frame_buffering = 1;
             s.max_num_ref_frames = 1;
         }},
        {"I P, the level's buffer", "I0 P1 P2 P3 P4 P5 P6 P7 P8 P9 P10 P11 P12 P13 P14 P15 P16 P17 P18 P19", {}},
        {"I P B, a buffer of 2", "I0 P2 b1 P4 b3 P6 b5 P8 b7", [](hs::Sps& s) {
             s.vui.present = s.vui.restriction = true;
             s.vui.max_dec_frame_buffering = 2;
             s.vui.num_reorder_frames = 1;
         }},
        {"I P B B, a buffer of 2", "I0 P3 b1 b2 P6 b4 b5 P9 b7 b8 P12 b10", [](hs::Sps& s) {
             s.vui.present = s.vui.restriction = true;
             s.vui.max_dec_frame_buffering = 2;
             s.vui.num_reorder_frames = 2;
         }},
        {"non-reference P, a buffer of 1", "I0 p1 P2 p3 P4 p5 P6 p7", [](hs::Sps& s) {
             s.vui.present = s.vui.restriction = true;
             s.vui.max_dec_frame_buffering = 1;
             s.max_num_ref_frames = 1;
         }},
        {"IDR between, a buffer of 1", "I0 P1 P2 I3 P4 P5 I6", [](hs::Sps& s) {
             s.vui.present = s.vui.restriction = true;
             s.vui.max_dec_frame_buffering = 1;
             s.max_num_ref_frames = 1;
         }},
        {"B then IDR, a buffer of 1", "I0 p1 I2 p3 I4 p5", [](hs::Sps& s) {
             s.vui.present = s.vui.restriction = true;
             s.vui.max_dec_frame_buffering = 1;
             s.max_num_ref_frames = 1;
         }},
    };
    for (const Life& l : lives) {
        if (!pick(l.name)) continue;
        Avc a{r};
        a.label = std::string(l.name) + ": ";
        if (!open(r, a, l.name, 21)) continue;
        const Seq sq = sequence(l.tokens, 64, 64, l.setup);
        stream(r, a, "the stream", sq.aus);
        close(r, a, l.name);
    }
    // Decode modes: B pictures skipped.
    for (const u32 mode : {1u, 2u, 3u}) {
        if (!pick(std::format("mode {}", mode))) continue;
        Avc a{r};
        a.label = std::format("mode {}: ", mode);
        if (!open(r, a, std::format("mode {}", mode), 21)) continue;
        const Seq sq = sequence("I0 P3 b1 b2 P6 b4 b5 P9 b7 b8", 64, 64, [](hs::Sps& s) {
            s.vui.present = s.vui.restriction = true;
            s.vui.max_dec_frame_buffering = 2;
        });
        stream(r, a, "the stream", sq.aus, mode);
        close(r, a, std::format("mode {}", mode));
    }
    // The clock: a picture whose access unit's PTS is not before its DTS leaves when a later access unit's DTS reaches
    // it; with no DTS or PTS, or a PTS before the DTS, the buffer outputs it.
    struct Clock {
        const char* name;
        const char* tokens;
        Stamps stamps;
    };
    const Clock clocks[] = {
        {"clock: PTS = DTS", "I0 P1 P2 P3 P4 P5", [](usize i, int) { return std::pair<u64, u64>{0x1000 + i, 0x1000 + i}; }},
        {"clock: PTS = DTS + 2", "I0 P1 P2 P3 P4 P5", [](usize i, int) { return std::pair<u64, u64>{0x1000 + i, 0x1002 + i}; }},
        {"clock: 0", "I0 P1 P2 P3", [](usize, int) { return std::pair<u64, u64>{0, 0}; }},
        {"clock: no DTS", "I0 P1 P2 P3", [](usize i, int) { return std::pair<u64, u64>{kNone, 0x1000 + i}; }},
        {"clock: no PTS", "I0 P1 P2 P3", [](usize i, int) { return std::pair<u64, u64>{0x1000 + i, kNone}; }},
        {"clock: B pictures", "I0 P3 b1 b2 P6 b4 b5 P9 b7 b8",
            [](usize i, int disp) { return std::pair<u64, u64>{0x1000 + i, 0x1002 + static_cast<u64>(disp)}; }},
        {"clock: B pictures, PTS late", "I0 P3 b1 b2 P6 b4 b5 P9 b7 b8",
            [](usize i, int disp) { return std::pair<u64, u64>{0x1000 + i, 0x1004 + static_cast<u64>(disp)}; }},
        {"clock: B pictures, no DTS", "I0 P3 b1 b2 P6 b4 b5",
            [](usize, int disp) { return std::pair<u64, u64>{kNone, 0x1002 + static_cast<u64>(disp)}; }},
        {"clock: 33 bits", "I0 P1 P2 P3", [](usize i, int) { return std::pair<u64, u64>{0x1FFFFFFFEull + i, 0x1FFFFFFFFull + i}; }},
    };
    for (const Clock& cl : clocks) {
        if (!pick(cl.name)) continue;
        Avc a{r};
        a.label = std::string(cl.name) + ": ";
        if (!open(r, a, cl.name, 21)) continue;
        const Seq sq = sequence(cl.tokens, 64, 64, {});
        stream(r, a, "the stream", sq.aus, 0, cl.stamps, sq.display);
        close(r, a, cl.name);
    }
    // Pictures held: the core stops for a frame buffer; the queue fills; releases let it go on.
    if (pick("held")) {
        Avc a{r};
        a.label = "held: ";
        a.release = false;
        if (open(r, a, "held", 21)) {
            const Seq sq = sequence("I0 P1 P2 P3 P4 P5 P6 P7 P8 P9 P10", 64, 64, [](hs::Sps& s) {
                s.vui.present = s.vui.restriction = true;
                s.vui.max_dec_frame_buffering = 1;
                s.max_num_ref_frames = 1;
            });
            for (usize i = 0; i < sq.aus.size(); i++) {
                const u64 v = a.au(std::format("AccessUnit {}", i), sq.aus[i], 0, 0x1000 + i, 0x800 + i, 0, 0, 1);
                a.status(std::format("AccessUnit {}", i));
                if (static_cast<u32>(v) != 0) break;
            }
            for (int k = 0; k < 4 && !a.held.empty(); k++) {
                a.release_all(std::format("release {}", k), a.held.size() - 1);
                a.settle(1);
                a.messages(std::format("after release {}", k));
                a.status(std::format("after release {}", k));
            }
            a.release = true;
            a.release_all("all");
            a.settle(1);
            a.messages("after all released");
            a.flush("Flush");
            close(r, a, "held");
        }
    }
    // Side data with the access units.
    if (pick("side data")) {
        Avc a{r};
        a.label = "side data: ";
        if (open(r, a, "side data", 21)) {
            const Seq sq = sequence("I0 P1 P2 P3", 64, 64, {});
            for (usize i = 0; i < sq.aus.size(); i++) {
                for (u32 k = 0; k < 0x40; k++) ps_write8(b, kSide + k, static_cast<u8>(0x10 * i + k));
                a.au(std::format("AccessUnit {}", i), sq.aus[i], 0, 0x1000 + i, 0x800 + i, kSide, i == 0 ? 0x40 : 8 * i);
            }
            const u64 v = call_au(r, a.handle, kStream, 16, 0, 0, 0, 0, 0, kSide, 0x41);
            r.o.line("side data: 0x41 bytes", compare::answer(v));
            const u64 w = call_au(r, a.handle, kStream, 16, 0, 0, 0, 0, 0, 0, 8);
            r.o.line("side data: 8 bytes at 0", compare::answer(w));
            a.flush("Flush");
            close(r, a, "side data");
        }
    }
    // The calls in the wrong states, and refused arguments.
    if (pick("states")) {
        Avc a{r};
        a.label = "states: ";
        if (open(r, a, "states", 21)) {
            r.r(fn("RlsFrame"), {a.handle, 0}, "states: RlsFrame before any picture");
            r.r(fn("Flush"), {a.handle, 1}, "states: Flush mode 1");
            r.r(fn("Reset"), {a.handle, 0, 4}, "states: Reset flags 4");
            r.o.line("states: AccessUnit with no address", compare::answer(call_au(r, a.handle, 0, 16, 0, 0, 0, 0, 0)));
            r.o.line("states: AccessUnit with no size", compare::answer(call_au(r, a.handle, kStream, 0, 0, 0, 0, 0, 0)));
            r.o.line("states: AccessUnit with a continuation size alone", compare::answer(call_au(r, a.handle, kStream, 16, 0, 16, 0, 0, 0)));
            r.o.line("states: AccessUnit mode 4", compare::answer(call_au(r, a.handle, kStream, 16, 0, 0, 0, 0, 4)));
            r.o.line("states: AccessUnit with no handle", compare::answer(call_au(r, 0, kStream, 16, 0, 0, 0, 0, 0)));
            if (!g_mvc) r.r(fn("GetStatus"), {a.handle, 0}, "states: GetStatus with no status");
            r.r(fn("Close"), {kMem}, "states: Close with a handle that is not one");
            const Seq sq = sequence("I0 P1", 64, 64, {});
            a.au("AccessUnit 0", sq.aus[0]);
            a.flush("Flush");
            r.o.line("states: AccessUnit after Flush", compare::answer(call_au(r, a.handle, kStream, 16, 0, 0, 0, 0, 0)));
            r.r(fn("Flush"), {a.handle, 0}, "states: Flush again");
            r.r(fn("Reset"), {a.handle, 1, 3}, "states: Reset");
            a.settle(1);
            a.messages("after Reset");
            a.status("after Reset");
            a.au("AccessUnit after Reset", sq.aus[0]);
            a.au("AccessUnit 1 after Reset", sq.aus[1]);
            a.flush("Flush after Reset");
            close(r, a, "states");
            r.r(fn("Close"), {a.handle}, "states: Close again");
            if (!g_mvc) r.r(fn("GetStatus"), {a.handle, kStatus}, "states: GetStatus after Close");
            r.o.line("states: AccessUnit after Close", compare::answer(call_au(r, a.handle, kStream, 16, 0, 0, 0, 0, 0)));
        }
    }
    // The core's own SPURS.
    if (pick("own SPURS")) {
        Avc a{r};
        a.label = "own SPURS: ";
        if (open(r, a, "own SPURS", 21, 0, 0, 0, 0)) {
            const Seq sq = sequence("I0 P1 P2", 64, 64, {});
            stream(r, a, "the stream", sq.aus);
            close(r, a, "own SPURS");
        }
    }
    // Sequences the core refuses: an error picture for each access unit.
    struct Refused {
        const char* name;
        u32 level, wmbs, hmbs, w, h;
        std::function<void(hs::Sps&)> setup;
    };
    const Refused refused[] = {
        {"refused: 80x48 at 3", 30, 0, 0, 80, 48, {}},
        {"refused: 48x64", 21, 0, 0, 48, 64, {}},
        {"refused: High 10", 21, 0, 0, 64, 64, [](hs::Sps& q) { q.profile = 110; }},
        {"refused: 4:2:2", 21, 0, 0, 64, 64, [](hs::Sps& q) {
             q.profile = 100;
             q.chroma_format_idc = 2;
         }},
        {"refused: Extended", 21, 0, 0, 64, 64, [](hs::Sps& q) { q.profile = 88; }},
        {"refused: level 9", 21, 0, 0, 64, 64, [](hs::Sps& q) { q.level = 9; }},
        {"refused: 80x64 opened for 4x4", 21, 4, 4, 80, 64, {}},
    };
    for (const Refused& f : refused) {
        if (!pick(f.name)) continue;
        Avc a{r};
        a.label = std::string(f.name) + ": ";
        a.width = f.w;
        a.height = f.h;
        if (!open(r, a, f.name, f.level, 0, f.wmbs, f.hmbs)) continue;
        const Seq sq = sequence("I0 P1 P2 P3", f.w, f.h, f.setup);
        stream(r, a, "the stream", sq.aus);
        close(r, a, f.name);
    }
    // After Reset with flags 0 every sequence is refused, until a Reset with others.
    if (pick("Reset flags 0")) {
        Avc a{r};
        a.label = "Reset flags 0: ";
        if (open(r, a, "Reset flags 0", 21)) {
            const Seq sq = sequence("I0 P1 P2", 64, 64, {});
            r.r(fn("Reset"), {a.handle, 0, 0}, "Reset flags 0: Reset(0, 0)");
            a.settle(1);
            a.messages("after Reset(0, 0)");
            stream(r, a, "the stream", sq.aus);
            r.r(fn("Reset"), {a.handle, 0, 1}, "Reset flags 0: Reset(0, 1)");
            a.settle(1);
            a.messages("after Reset(0, 1)");
            stream(r, a, "the stream again", sq.aus);
            close(r, a, "Reset flags 0");
        }
    }
    // Access units with no picture to decode: within a sequence, and before one (with an SPS). Not here: an access unit
    // delimiter alone within a sequence (Sony's core conceals into the error picture's frame buffer, a copy of the last
    // picture a row down; pspocket leaves the buffer as it was), and access units with no SPS before the first sequence
    // (Sony's core takes them for fields, one picture of two; pspocket gives a picture for each) (D-933).
    struct NoSlice {
        const char* name;
        const char* tokens;
    };
    const NoSlice no_slice[] = {
        {"no slice: within a sequence", "I0 P1 G2 P3 G4 P5 Z6 i7 P8 G9 G10 P11"},
        {"no slice: before the sequence", "Z0 Z1 I2 P3 P4"},
    };
    for (const NoSlice& ns : no_slice) {
        if (!pick(ns.name)) continue;
        Avc a{r};
        a.label = std::string(ns.name) + ": ";
        a.error_frames = a.nal_flags = std::string_view(ns.name) != "no slice: within a sequence";
        if (!open(r, a, ns.name, 21)) continue;
        const Seq sq = sequence(ns.tokens, 64, 64, [](hs::Sps& q) {
            q.vui.present = q.vui.restriction = true;
            q.vui.max_dec_frame_buffering = 1;
            q.max_num_ref_frames = 1;
        });
        stream(r, a, "the stream", sq.aus);
        close(r, a, ns.name);
    }
    // Where GOPs start: the I picture after an SPS.
    if (pick("GOP")) {
        Avc a{r};
        a.label = "GOP: ";
        if (open(r, a, "GOP", 21)) {
            const Seq sq = sequence("I0 P1 +I2 P3 I4 %I5 #I6 +i7 P8 +P9 I10 P11 +p12 I13 P14", 64, 64, [](hs::Sps& q) {
                q.vui.present = q.vui.restriction = true;
                q.vui.max_dec_frame_buffering = 1;
                q.max_num_ref_frames = 1;
            });
            stream(r, a, "the stream", sq.aus);
            close(r, a, "GOP");
        }
    }
    // Fewer frame buffers than the stream's buffer and two: a 64 x 80 stream in a decoder opened for 5 x 4 macroblocks.
    if (pick("64x80 opened for 5x4")) {
        Avc a{r};
        a.label = "64x80 opened for 5x4: ";
        a.width = 64;
        a.height = 80;
        if (open(r, a, "64x80 opened for 5x4", 21, 0, 5, 4)) {
            const Seq sq = sequence("I0 P1 P2 P3 P4 P5 P6 P7 P8 P9 P10 P11 P12 P13 P14 P15 P16 P17 P18 P19 P20", 64, 80,
                [](hs::Sps& q) { q.max_num_ref_frames = 1; });
            stream(r, a, "the stream", sq.aus);
            close(r, a, "64x80 opened for 5x4");
        }
    }
    // Other sizes and levels: the frame buffers' places.
    if (pick("80x64 at 3")) {
        Avc a{r};
        a.label = "80x64 at 3: ";
        a.width = 80;
        a.height = 64;
        if (open(r, a, "80x64 at 3", 30)) {
            const Seq sq = sequence("I0 P1 P2 P3", 80, 64, {});
            stream(r, a, "the stream", sq.aus);
            close(r, a, "80x64 at 3");
        }
    }
    if (pick("64x64 at 4")) {
        Avc a{r};
        a.label = "64x64 at 4, 4 buffers: ";
        if (open(r, a, "64x64 at 4, 4 buffers", 40, 4, 4, 4)) {
            const Seq sq = sequence("I0 P1 P2 P3", 64, 64, {});
            stream(r, a, "the stream", sq.aus);
            close(r, a, "64x64 at 4, 4 buffers");
        }
    }
    // libmvcdec's two views: each access unit a base view picture and a second view picture.
    struct TwoViews {
        const char* name;
        const char* tokens;
        char second;
        std::function<void(hs::Sps&)> setup;
    };
    const auto buffer_of = [](u32 n) {
        return [n](hs::Sps& q) {
            q.vui.present = q.vui.restriction = true;
            q.vui.max_dec_frame_buffering = n;
            q.max_num_ref_frames = 1;
        };
    };
    const TwoViews two_views[] = {
        {"two views: I_PCM second views", "I0 P1 P2 P3", 'I', {}},
        {"two views: anchors from the base view", "I0 P1 P2 P3", 'A', {}},
        {"two views: from the second view's last", "I0 P1 P2 P3 P4 P5", 'T', {}},
        {"two views: a buffer of 1", "I0 P1 P2 P3 P4 P5 P6", 'T', buffer_of(1)},
        {"two views: non-reference pictures", "I0 p1 P2 p3 P4 p5", 'T', buffer_of(1)},
        {"two views: B pictures", "I0 P3 b1 b2 P6 b4 b5 P9 b7 b8", 'T', [](hs::Sps& q) {
             q.vui.present = q.vui.restriction = true;
             q.vui.max_dec_frame_buffering = 2;
         }},
        {"two views: IDR between", "I0 P1 P2 I3 P4 P5", 'T', buffer_of(1)},
    };
    for (const TwoViews& tv : two_views) {
        if (!g_mvc || !pick(tv.name)) continue;
        Avc a{r};
        a.label = std::string(tv.name) + ": ";
        if (!open(r, a, tv.name, 21)) continue;
        const Seq sq = sequence(tv.tokens, 64, 64, tv.setup, tv.second);
        a.seconds = sq.seconds;
        stream(r, a, "the stream", sq.aus);
        close(r, a, tv.name);
    }
    if (g_mvc && pick("two views: modes")) {
        for (const u32 mode : {1u, 2u, 3u}) {
            Avc a{r};
            a.label = std::format("two views: mode {}: ", mode);
            if (!open(r, a, std::format("two views: mode {}", mode), 21)) continue;
            const Seq sq = sequence("I0 P3 b1 b2 P6 b4 b5", 64, 64, [](hs::Sps& q) {
                q.vui.present = q.vui.restriction = true;
                q.vui.max_dec_frame_buffering = 2;
            }, 'T');
            a.seconds = sq.seconds;
            stream(r, a, "the stream", sq.aus, mode);
            close(r, a, std::format("two views: mode {}", mode));
        }
    }
    if (g_mvc && pick("two views: the views change")) {
        // The first access unit says how many views; an access unit with the other count is refused.
        Avc a{r};
        a.label = "two views: the views change: ";
        if (open(r, a, "two views: the views change", 21)) {
            const Seq sq = sequence("I0 P1 P2", 64, 64, {}, 'I');
            a.au("AccessUnit 0", sq.aus[0], 0, 0x1000, 0x800, 0, 0, 1, sq.seconds[0]);
            a.au("AccessUnit 1 without its second view", sq.aus[1], 0, 0x1001, 0x801);
            a.au("AccessUnit 1", sq.aus[1], 0, 0x1001, 0x801, 0, 0, 1, sq.seconds[1]);
            a.flush("Flush");
            close(r, a, "two views: the views change");
        }
        Avc o{r};
        o.label = "one view: the views change: ";
        if (open(r, o, "one view: the views change", 21)) {
            const Seq sq = sequence("I0 P1 P2", 64, 64, {}, 'I');
            o.au("AccessUnit 0", sq.aus[0], 0, 0x1000, 0x800);
            o.au("AccessUnit 1 with a second view", sq.aus[1], 0, 0x1001, 0x801, 0, 0, 1, sq.seconds[1]);
            o.au("AccessUnit 1", sq.aus[1], 0, 0x1001, 0x801);
            o.flush("Flush");
            close(r, o, "one view: the views change");
        }
    }
    if (g_mvc && pick("two views: side data")) {
        Avc a{r};
        a.label = "two views: side data: ";
        a.release = false;
        if (open(r, a, "two views: side data", 21)) {
            const Seq sq = sequence("I0 P1 P2 P3", 64, 64, buffer_of(1), 'A');
            for (usize i = 0; i < sq.aus.size(); i++) {
                for (u32 k = 0; k < 0x40; k++) ps_write8(b, kSide + k, static_cast<u8>(0x20 * i + k));
                a.au(std::format("AccessUnit {}", i), sq.aus[i], 0, 0x1000 + i, 0x800 + i, kSide, 0x10 + 8 * static_cast<u32>(i),
                    1, sq.seconds[i]);
                // the base view picture released alone first, then the second view's
                if (a.held.size() >= 2) {
                    const u32 base_id = a.held[0], second_id = a.held[1];
                    a.held.erase(a.held.begin(), a.held.begin() + 2);
                    r.r(fn("RlsFrame"), {a.handle, base_id}, std::format("two views: side data: AccessUnit {}: RlsFrame({})", i, base_id));
                    r.r(fn("RlsFrame"), {a.handle, second_id}, std::format("two views: side data: AccessUnit {}: RlsFrame({})", i, second_id));
                }
            }
            a.release = true;
            a.flush("Flush");
            close(r, a, "two views: side data");
        }
    }
    end_spurs(r);
    return std::move(r.o);
}

} // namespace

TEST_CASE("libavcdec: the comparison calls Sony's exports by their names; pspocket's own", "[hle][vdec-cores]")
{
    check_names("libavcdec", {"avcDecQueryMemory", "avcDecOpen", "avcDecClose", "avcDecReset", "avcDecAccessUnit",
                                 "avcDecRlsFrame", "avcDecFlush", "avcDecGetStatus", "avcDecGetVersion",
                                 "avcDecQueryCharacteristics"},
        false);
}

// A byte of a "hex:" value.
u32 byte_of(const std::string& hex, u32 offset)
{
    if (hex.size() < 4 + 2 * offset + 2) return 0x100;
    return static_cast<u32>(std::stoul(hex.substr(4 + 2 * offset, 2), nullptr, 16));
}

TEST_CASE("The H.264 core (libavcdec): pspocket's own, answering as Sony's did (D-933)", "[hle][vdec-cores]")
{
    Setup s;
    s.mode = Mode::Self;
    const std::vector<std::string> only = {"QueryMemory", "I P B B", "clock: B pictures", "held", "states", "GOP",
        "refused: 80x48", "no slice"};
    g_only = &only;
    g_quiet = std::chrono::milliseconds(40);
    g_timeout = std::chrono::milliseconds(1500);
    const Observed o = avc_life(s, false);
    g_only = nullptr;
    g_quiet = std::chrono::milliseconds(400);
    g_timeout = std::chrono::milliseconds(15000);
    // Sony's answers (this file's comparison, firmware 4.92).
    CHECK(o.value("QueryMemory level 21: the attribute") == "hex:eeeeeeee00e3d68000000080");
    CHECK(o.value("QueryMemory level 42: the attribute") == "hex:eeeeeeee0335288000000080");
    CHECK(o.value("QueryMemory level 21 deblock 0 buffers 0 22x18 MBs: the attribute") == "hex:eeeeeeee00ac828000000080");
    CHECK(o.value("QueryMemory level 43") == "0x80615201");
    CHECK(o.value("Open with 9 SPUs") == "0x80615201");
    CHECK(o.value("Open with one byte less") == "0x80615201");
    // The buffer of two with B pictures: the frame buffers in turn, a picture's buffer free once released and out of
    // the decoded picture buffer.
    CHECK(o.value("I P B B, a buffer of 2: the stream: AccessUnit 6: message 10") ==
        "picture: arg 0x1234 frame mem+0x801100 sides ctx+0x598 0x0");
    CHECK(o.value("I P B B, a buffer of 2: the stream: AccessUnit 6: message 11") ==
        "picture: arg 0x1234 frame mem+0x7fd900 sides ctx+0x5dc 0x0");
    CHECK(o.value("I P B B, a buffer of 2: the stream: Flush: message 23") ==
        "picture: arg 0x1234 frame mem+0x801100 sides ctx+0x554 0x0");
    // The clock: a picture leaves when an access unit's DTS reaches its PTS.
    CHECK(o.value("clock: B pictures: the stream: AccessUnit 4: message 7") ==
        "picture: arg 0x1234 frame mem+0x808100 sides ctx+0x554 0x0");
    // Pictures held: the access units wait in the queue.
    CHECK(o.value("held: AccessUnit 5: the status") == "hex:0000000000000003");
    CHECK(o.value("states: RlsFrame before any picture") == "0x80615202");
    CHECK(o.value("states: AccessUnit after Flush") == "0x80615202");
    CHECK(o.value("states: Close again") == "0x80615202");
    CHECK(o.value("states: AccessUnit after Close") == "0x80010005");
    // Refused sequences and access units with no slice.
    CHECK(o.value("refused: 80x48 at 3: the stream: AccessUnit 0: message 0") ==
        "AU done: au 0x280000 cont 0x0 arg 0x1234 side ctx+0x554 (0 bytes) flags hex:0001000100000012");
    CHECK(o.value("refused: 80x48 at 3: the stream: AccessUnit 0: message 1") ==
        "picture: arg 0x1234 frame mem+0xd4b380 sides ctx+0x554 0x0");
    CHECK(o.value("no slice: before the sequence: the stream: AccessUnit 0: message 0") ==
        "AU done: au 0x280000 cont 0x0 arg 0x1234 side ctx+0x554 (0 bytes) flags hex:0001000100000011");
    CHECK(o.value("no slice: within a sequence: the stream: AccessUnit 2: message 2") ==
        "AU done: au 0x281900 cont 0x0 arg 0x1234 side ctx+0x5dc (0 bytes) flags hex:0001010000000051");
    const Data* blank = o.find_data("refused: 80x48 at 3: the stream: AccessUnit 0: message 1: its frame buffer");
    REQUIRE(blank);
    CHECK(blank->bytes[0x1EFF] == 0);
    CHECK(blank->bytes[0x1F00] == 0x80);
    // The GOP starts: the I pictures after an SPS (by their PTS).
    std::vector<u32> gop;
    for (const Line& l : o.lines)
        if (l.key.starts_with("GOP: the stream") && l.key.ends_with(": information") && byte_of(l.value, 0x0C) == 1)
            gop.push_back(byte_of(l.value, 0x86) << 8 | byte_of(l.value, 0x87));
    std::sort(gop.begin(), gop.end());
    CHECK(gop == std::vector<u32>{0x800, 0x802, 0x805, 0x807, 0x80A, 0x80D});
}

TEST_CASE("The H.264 core (libavcdec) against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    const Setup s = setup();
    g_mvc = false;
    run(s, "libavcdec against Sony's", [&](bool sony) { return avc_life(s, sony); }, [](const Observed&) {});
}

TEST_CASE("libmvcdec: the comparison calls Sony's exports by their names; pspocket's own", "[hle][vdec-cores]")
{
    check_names("libmvcdec", {"mvcDecQueryMemory", "mvcDecOpen", "mvcDecClose", "mvcDecReset", "mvcDecAccessUnit",
                                 "mvcDecRlsFrame", "mvcDecFlush", "mvcDecGetVersion", "mvcDecQueryCharacteristics"},
        false);
}

TEST_CASE("The MVC core (libmvcdec): pspocket's own, answering as Sony's did (D-933)", "[hle][vdec-cores]")
{
    Setup s;
    s.mode = Mode::Self;
    const std::vector<std::string> only = {"QueryMemory", "non-reference P, a buffer of 1", "states", "two views: I_PCM",
        "two views: B pictures", "two views: modes", "two views: the views change"};
    g_only = &only;
    g_quiet = std::chrono::milliseconds(40);
    g_timeout = std::chrono::milliseconds(1500);
    g_mvc = true;
    const Observed o = avc_life(s, false);
    g_mvc = false;
    g_only = nullptr;
    g_quiet = std::chrono::milliseconds(400);
    g_timeout = std::chrono::milliseconds(15000);
    // Sony's answers (this file's comparison, firmware 4.92).
    CHECK(o.value("QueryMemory level 21: the attribute") == "hex:eeeeeeee013f138000000080");
    CHECK(o.value("QueryMemory level 41: the attribute") == "hex:eeeeeeee0487ed0000000080");
    CHECK(o.value("QueryMemory level 21 deblock 0 buffers 4 0x0 MBs: the attribute") == "hex:eeeeeeee00d2538000000080");
    CHECK(o.value("QueryMemory level 21 deblock 0 buffers 15 0x0 MBs") == "0x80615901"); // two views' buffers: even
    // One view: the store as each picture is decoded; one output at once comes with the next access unit.
    CHECK(o.value("non-reference P, a buffer of 1: the stream: AccessUnit 1: message 2") ==
        "picture: arg 0x1234 frame mem+0x81e380 sides ctx+0x584 0x0");
    CHECK(o.value("non-reference P, a buffer of 1: the stream: AccessUnit 2: message 4") ==
        "picture: arg 0x1234 frame mem+0x821b80 sides ctx+0x5cc 0x0");
    CHECK(o.value("states: AccessUnit after Close") == "0x2");
    // Two views: each picture's second view picture in the next frame buffer, right after it.
    CHECK(o.value("two views: I_PCM second views: the stream: AccessUnit 0: message 0") ==
        "AU done: pieces hex:0028000000001843000000000000000000281880000018460000000000000000 arg 0x1234 side ctx+0x584 "
        "(0 bytes) flags hex:0000000000000000010000000000000002");
    CHECK(o.value("two views: I_PCM second views: the stream: Flush: message 5") ==
        "picture: arg 0x1234 frame mem+0x821b80 sides ctx+0x584 0x0");
    const std::string second = o.value("two views: I_PCM second views: the stream: Flush: message 5: information");
    CHECK(byte_of(second, 0x0D) == 1); // the second view
    CHECK(byte_of(second, 0x0E) == 0); // not its access unit's first picture
    CHECK(byte_of(second, 0x0F) == 2); // two pictures in its access unit
    // A B picture output at once: its second view picture comes before the next access unit is done.
    CHECK(o.value("two views: B pictures: the stream: AccessUnit 3: message 6") ==
        "picture: arg 0x1234 frame mem+0x82fb80 sides ctx+0x614 0x0");
    CHECK(o.value("two views: B pictures: the stream: AccessUnit 3: message 7").starts_with("AU done"));
    // Skipped pictures have no frame buffer.
    CHECK(o.value("two views: mode 1: the stream: AccessUnit 2: message 5") == "picture: arg 0x1234 frame 0x0 sides ctx+0x614 0x0");
    CHECK(o.value("two views: the views change: AccessUnit 1 without its second view") == "0x80615901");
    CHECK(o.value("one view: the views change: AccessUnit 1 with a second view") == "0x80615901");
    // The second view decoded (its own I_PCM pictures, through the made one-view stream): its frame buffer's samples.
    const Data* frame = o.find_data("two views: I_PCM second views: the stream: Flush: message 5: its frame buffer");
    REQUIRE(frame);
    const h::Picture want = h::pattern(64, 64, 100);
    CHECK(frame->bytes[2 * 16 * 64] == want.y[0]); // the picture's first sample, after two strips of edge
}

TEST_CASE("The MVC core (libmvcdec) against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    const Setup s = setup();
    g_mvc = true;
    run(s, "libmvcdec against Sony's", [&](bool sony) { return avc_life(s, sony); }, [](const Observed&) {});
    g_mvc = false;
}
