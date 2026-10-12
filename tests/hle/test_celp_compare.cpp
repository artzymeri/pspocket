// The CELP libraries against Sony's modules (D-932): libcelpenc (the MPEG-4 CELP encoder at 16 kHz) first. The same
// resources, parameters and synthetic audio go to pspocket's library and to Sony's module. Compared: QueryAttr's
// attributes; every answer of Open, OpenEx, OpenExt, Start, EncodeFrame, WaitForOutput, GetAu, End and Close, through
// their checks and in every state; the handle (as an offset into the work memory) and the words of it libcelpenc keeps
// its state in; the resource OpenEx and OpenExt change; Start's header byte for byte; every access unit byte for byte
// and the information GetAu writes (Sony's SPU program encodes in the SPU's single precision; pspocket's encoder does
// the same arithmetic in the same order, celp_rpe_enc.cpp).
//
// The structures (RPCS3's cellCelpEnc.h): CellCelpEncAttr {workMemSize, celpEncVerUpper, celpEncVerLower};
// CellCelpEncResource {totalMemSize, startAddr, ppuThreadPriority, spuThreadPriority, ppuThreadStackSize};
// CellCelpEncResourceEx {totalMemSize, startAddr, spurs, priority[8], maxContention}; CellCelpEncParam
// {excitationMode, sampleRate, configuration, wordSize, outBuff, outSize}; CellCelpEncPcmInfo and CellCelpEncAuInfo
// {startAddr, size}.
//
// The audio is built here: speech-like (a pulse train through formant resonators), sines, a chirp, noise, a square
// wave, impulses, silence. No game data; the comparison skips without the firmware (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "../../runtime/hle/src/celp_codec.hpp"
#include "../../runtime/hle/src/celp_spu_fp.hpp"
#include "compare_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

namespace {

constexpr u32 kWork = 0x01000000, kWorkSize = 0x00400000;
constexpr u32 kAttr = kData + 0x100, kRes = kData + 0x140, kHandle = kData + 0x180, kParam = kData + 0x1C0,
              kPcmInfo = kData + 0x200, kAuInfo = kData + 0x240, kHeader = kData + 0x400, kAu = kData + 0x800,
              kSpurs = kData + 0x4000, kPcm = kData + 0x10000;
constexpr u8 kFill = 0xEE;
constexpr u32 kHandleSize = 0x7D80;
enum : u32 { ARG = 0x80614003, SEQ = 0x80614002 };

struct CelpFixture : TlsFixture {
    CelpFixture()
    {
        REQUIRE(hle->main_memory().reserve(kWork, kWorkSize));
        REQUIRE(mem.commit(kWork, kWorkSize));
    }
};

// Synthetic audio at 16 kHz, full scale 1. Kinds: 0 speech-like, 1 silence, 2 a sine, 3 a chirp, 4 noise near full
// scale, 5 a square wave, 6 impulses, 7 noise at the level of a 16-bit sample's last bits, 8 a constant, 9 speech
// gated by silence.
std::vector<float> audio(u32 n, u32 kind, u32 seed)
{
    std::vector<float> out(n);
    double ph = 0;
    u32 st = seed * 2654435761u + 12345, nz = seed * 747796405u + 1;
    double y1[3] = {}, y2[3] = {};
    const double formants[3] = {700, 1200, 2600}, bw[3] = {80, 90, 120};
    auto rnd = [&] {
        nz = nz * 1664525u + 1013904223u;
        return static_cast<double>(nz >> 8) / 16777216.0 - 0.5;
    };
    for (u32 i = 0; i < n; i++) {
        const double t = static_cast<double>(i) / 16000;
        const double f0 = 110 + 40 * std::sin(2 * M_PI * 0.7 * t) + 15 * seed;
        ph += f0 / 16000;
        double x = 0;
        if (ph >= 1) {
            ph -= 1;
            x = 1;
        }
        st = st * 1664525u + 1013904223u;
        x += (static_cast<double>(st >> 8) / 16777216.0 - 0.5) * 0.05;
        double v = x;
        for (int k = 0; k < 3; k++) {
            const double r = std::exp(-M_PI * bw[k] / 16000), c = 2 * r * std::cos(2 * M_PI * formants[k] / 16000);
            const double y = v + c * y1[k] - r * r * y2[k];
            y2[k] = y1[k];
            y1[k] = y;
            v = y * (1 - r);
        }
        double s = v * 0.6 * (0.5 + 0.5 * std::sin(2 * M_PI * 1.3 * t));
        switch (kind) {
        case 1: s = 0; break;
        case 2: s = 0.6 * std::sin(2 * M_PI * (300 + 200 * seed) * t); break;
        case 3: s = 0.5 * std::sin(2 * M_PI * (100 + 3000 * t) * t); break;
        case 4: s = 1.9 * rnd(); break;
        case 5: s = std::fmod(t * 220, 1.0) < 0.5 ? 0.999 : -1.0; break;
        case 6: s = i % 97 == 0 ? 0.9 : 0; break;
        case 7: s = 3 * rnd() / 32767; break;
        case 8: s = 0.03; break;
        case 9: s = (i / 1600) % 2 ? s : 0; break;
        default: break;
        }
        out[i] = static_cast<float>(s);
    }
    return out;
}

// One frame of the audio as libcelpenc takes it: big-endian floats, or little-endian 16-bit samples.
void put_frame(u8* b, u32 at, const float* x, u32 samples, bool floats)
{
    for (u32 i = 0; i < samples; i++) {
        if (floats) {
            ps_write32(b, at + 4 * i, std::bit_cast<u32>(x[i]));
        } else {
            const s16 s = static_cast<s16>(std::lround(std::clamp(x[i] * 32767.0f, -32768.0f, 32767.0f)));
            b[at + 2 * i] = static_cast<u8>(s & 0xFF);
            b[at + 2 * i + 1] = static_cast<u8>((s >> 8) & 0xFF);
        }
    }
}

struct Enc : Half<CelpFixture> {
    using Half::Half;

    std::string where(u32 h) const
    {
        if (h >= kWork && h < kWork + kWorkSize) return std::format("the work memory +{:#x}", h - kWork);
        return std::format("{:#x}", h);
    }
    // The words of the handle libcelpenc keeps its state, a frame's size and the access unit's size in.
    void words(const std::string& what, u32 h)
    {
        o.line(what + ": state, frame bytes, access unit bytes",
            std::format("{} {:#x} {:#x}", ps_read32(b, h), ps_read32(b, h + 0x28), ps_read32(b, h + 0x2C)));
    }
    void resource(u32 total, u32 start, u32 ppu = 1000, u32 spu = 100)
    {
        ps_write32(b, kRes, total);
        ps_write32(b, kRes + 4, start);
        ps_write32(b, kRes + 8, ppu);
        ps_write32(b, kRes + 12, spu);
        ps_write32(b, kRes + 16, 0x4000);
    }
    void resource_ex(u32 total, u32 start, u32 spurs, u32 contention)
    {
        ps_write32(b, kRes, total);
        ps_write32(b, kRes + 4, start);
        ps_write32(b, kRes + 8, spurs);
        for (u32 i = 0; i < 8; i++) b[kRes + 12 + i] = static_cast<u8>(i < 2 ? 1 : 0);
        ps_write32(b, kRes + 20, contention);
    }
    u32 open(const std::string& what, const std::string& fn = "cellCelpEncOpen")
    {
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = Half::r(fn, {kRes, kHandle}, fn.substr(11) + " " + what);
        const u32 h = ps_read32(b, kHandle);
        o.line(fn.substr(11) + " " + what + ": the handle", r == 0 ? where(h) : std::format("{:#x}", h));
        o.line(fn.substr(11) + " " + what + ": the resource", hex(b + kRes, 24));
        return r == 0 ? h : 0;
    }
    void param(u32 mode, u32 rate, u32 config, u32 word, u32 out)
    {
        ps_write32(b, kParam, mode);
        ps_write32(b, kParam + 4, rate);
        ps_write32(b, kParam + 8, config);
        ps_write32(b, kParam + 12, word);
        ps_write32(b, kParam + 16, out);
        ps_write32(b, kParam + 20, 0xEEEEEEEE);
    }
    u64 start(const std::string& what, u32 h, u32 config, u32 word, bool out = true)
    {
        param(1, 2, config, word, out ? kHeader : 0);
        std::memset(b + kHeader, kFill, 64);
        const u64 r = Half::r("cellCelpEncStart", {h, kParam}, "Start " + what);
        const u32 size = ps_read32(b, kParam + 20);
        o.line("Start " + what + ": outSize", std::format("{:#x}", size));
        if (r == 0 && out) o.line("Start " + what + ": the header", hex(b + kHeader, std::min<u32>(size, 64)));
        words("Start " + what, h);
        return r;
    }
    // One frame encoded and taken: EncodeFrame, WaitForOutput, GetAu (the access unit as data).
    void frame(const std::string& what, u32 h, const float* x, u32 samples, bool floats)
    {
        put_frame(b, kPcm, x, samples, floats);
        ps_write32(b, kPcmInfo, kPcm);
        ps_write32(b, kPcmInfo + 4, samples * (floats ? 4 : 2));
        Half::r("cellCelpEncEncodeFrame", {h, kPcmInfo}, what + ": EncodeFrame");
        Half::r("cellCelpEncWaitForOutput", {h}, what + ": WaitForOutput");
        std::memset(b + kAu, kFill, 128);
        ps_write32(b, kAuInfo, 0xEEEEEEEE);
        ps_write32(b, kAuInfo + 4, 0xEEEEEEEE);
        Half::r("cellCelpEncGetAu", {h, kAu, kAuInfo}, what + ": GetAu");
        const u32 size = ps_read32(b, kAuInfo + 4);
        o.line(what + ": auItem", std::format("{} {:#x}", ps_read32(b, kAuInfo) == kAu ? "outBuffer" : "?", size));
        o.bytes(what + ": the access unit", b + kAu, std::min<u32>(size, 128));
    }
};

Observed half_enc(const Setup& s, bool sony_half)
{
    Enc h(s, sony_half, "cellCelpEnc");
    u8* b = h.b;

    // QueryAttr.
    h.r("cellCelpEncQueryAttr", {0}, "QueryAttr(NULL)");
    std::memset(b + kAttr, kFill, 16);
    h.r("cellCelpEncQueryAttr", {kAttr}, "QueryAttr");
    h.o.line("QueryAttr: the attributes", hex(b + kAttr, 16));

    // Open's checks: the resource, the handle, the start, the priorities, the memory for the handle's own (0x7D80
    // bytes from the start's next 128-byte boundary) and what is left after it (at least 0x2800).
    const u32 start = kWork + 0x10, pad = 0x70;
    std::memset(b + kWork, kFill, 0x10000);
    h.r("cellCelpEncOpen", {0, kHandle}, "Open(NULL, handle)");
    h.resource(0x10000, start);
    h.r("cellCelpEncOpen", {kRes, 0}, "Open(res, NULL)");
    h.resource(0x10000, 0);
    h.open("with no start");
    h.resource(0x10000, start, 0xC00);
    h.open("at PPU priority 0xc00");
    h.resource(0x10000, start, 0xBFF, 0x100);
    h.open("at SPU priority 0x100");
    h.resource(pad + kHandleSize - 1, start);
    h.open("one byte short of the handle's memory");
    h.words("after that", start + pad);
    h.resource(pad + kHandleSize + 0x27FF, start);
    h.open("one byte short of the work memory");
    h.words("after that", start + pad);

    // Before Start: every call but Start and Close out of sequence.
    h.resource(0x20000, start, 0xBFF, 0xFF);
    const u32 e = h.open("at the highest priorities");
    REQUIRE(e);
    h.words("Open", e);
    h.r("cellCelpEncEnd", {e}, "End before Start");
    h.r("cellCelpEncWaitForOutput", {e}, "WaitForOutput before Start");
    ps_write32(b, kPcmInfo, kPcm);
    ps_write32(b, kPcmInfo + 4, 960);
    h.r("cellCelpEncEncodeFrame", {e, kPcmInfo}, "EncodeFrame before Start");
    h.r("cellCelpEncGetAu", {e, kAu, kAuInfo}, "GetAu before Start");
    h.r("cellCelpEncGetAu", {e, kAu, 0}, "GetAu(handle, out, NULL) before Start");
    for (const u32 fn : {0u, 1u, 2u, 3u, 4u, 5u}) {
        const char* names[] = {"cellCelpEncEnd", "cellCelpEncWaitForOutput", "cellCelpEncClose",
            "cellCelpEncEncodeFrame", "cellCelpEncGetAu", "cellCelpEncStart"};
        h.r(names[fn], {0, kPcmInfo, kAuInfo}, std::format("{}(NULL)", names[fn]));
    }

    // Start's checks.
    h.r("cellCelpEncStart", {e, 0}, "Start(handle, NULL)");
    struct Bad {
        u32 mode, rate, config, word;
    };
    for (const Bad& p : {Bad{0, 2, 0, 0}, Bad{2, 2, 0, 0}, Bad{1, 1, 0, 0}, Bad{1, 3, 0, 0}, Bad{1, 2, 4, 0},
             Bad{1, 2, 0xFFFFFFFF, 0}, Bad{1, 2, 0, 2}, Bad{1, 2, 0, 0xFFFFFFFF}}) {
        h.param(p.mode, p.rate, p.config, p.word, kHeader);
        h.r("cellCelpEncStart", {e, kParam},
            std::format("Start mode {} rate {} configuration {:#x} word size {:#x}", p.mode, p.rate, p.config, p.word));
    }

    // Each configuration and word size: Start's header, then frames of each kind of audio, every access unit.
    u32 frames_total = 0;
    for (u32 config = 0; config < 4; config++)
        for (u32 word = 0; word < 2; word++) {
            const bool floats = word == 1;
            const u32 samples = config == 1 ? 160 : 240;
            const std::string tag = std::format("configuration {} {}", config, floats ? "floats" : "int16");
            REQUIRE(h.start(tag, e, config, word) == 0);
            const u32 kinds[] = {0, 2, 3, 4, 5, 6, 7, 8, 9, 1};
            const u32 per = 4;
            const std::vector<float> x = audio(samples * per * 10, 0, config * 2 + word + 1);
            for (u32 k = 0; k < 10; k++) {
                const std::vector<float> y = audio(samples * per, kinds[k], config + 3 * word + k);
                for (u32 f = 0; f < per; f++) {
                    const float* src = kinds[k] == 0 ? &x[(k * per + f) * samples] : &y[f * samples];
                    h.frame(std::format("{}: frame {}", tag, k * per + f), e, src, samples, floats);
                    frames_total++;
                }
            }
            // The frame's size exactly, and one frame at a time.
            ps_write32(b, kPcmInfo + 4, samples * (floats ? 4 : 2) - 1);
            h.r("cellCelpEncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame of a byte less");
            ps_write32(b, kPcmInfo + 4, samples * (floats ? 4 : 2) + 1);
            h.r("cellCelpEncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame of a byte more");
            ps_write32(b, kPcmInfo, 0);
            ps_write32(b, kPcmInfo + 4, samples * (floats ? 4 : 2));
            h.r("cellCelpEncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame of no frame");
            h.r("cellCelpEncEncodeFrame", {e, 0}, tag + ": EncodeFrame(handle, NULL)");
            h.r("cellCelpEncWaitForOutput", {e}, tag + ": WaitForOutput with no frame");
            h.r("cellCelpEncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu with no frame");
            ps_write32(b, kPcmInfo, kPcm);
            h.r("cellCelpEncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame");
            h.r("cellCelpEncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame again");
            h.r("cellCelpEncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu before WaitForOutput");
            h.r("cellCelpEncWaitForOutput", {e}, tag + ": WaitForOutput");
            h.r("cellCelpEncWaitForOutput", {e}, tag + ": WaitForOutput again");
            h.words(tag + ": an access unit waiting", e);
            if (config == 0) {
                // GetAu without a buffer: the size 0, the access unit taken all the same.
                ps_write32(b, kAuInfo, 0xEEEEEEEE);
                h.r("cellCelpEncGetAu", {e, 0, kAuInfo}, tag + ": GetAu(handle, NULL, auItem)");
                h.o.line(tag + ": auItem", hex(b + kAuInfo, 8));
                h.r("cellCelpEncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu again");
                // End with a frame being encoded, then Start again from state 1.
                h.r("cellCelpEncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame before End");
                h.r("cellCelpEncEnd", {e}, tag + ": End with a frame being encoded");
                h.words(tag + ": End", e);
                h.r("cellCelpEncEnd", {e}, tag + ": End again");
            } else if (config == 1) {
                // End with an access unit waiting.
                h.r("cellCelpEncEnd", {e}, tag + ": End with an access unit waiting");
                h.words(tag + ": End", e);
            } else {
                // Start again while started (it ends first): the access unit waiting is lost.
                h.r("cellCelpEncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu");
            }
        }
    // Start without a header buffer, then Close while started; Close again.
    h.start("without a buffer for the header", e, 3, 0, false);
    h.r("cellCelpEncClose", {e}, "Close while started");
    h.words("Close", e);
    h.r("cellCelpEncClose", {e}, "Close again");
    h.r("cellCelpEncStart", {e, kParam}, "Start after Close");

    // OpenEx and OpenExt on a SPURS instance of the game's: their checks, and the resource they change.
    REQUIRE(h.t.call("cellSpursInitialize", kSpurs, 2, 200, 2016, 0) == 0);
    for (const std::string fn : {"cellCelpEncOpenEx", "cellCelpEncOpenExt"}) {
        const std::string n = fn.substr(11);
        h.r(fn, {0, kHandle}, n + "(NULL, handle)");
        h.resource_ex(0x20000, start, kSpurs, 1);
        h.r(fn, {kRes, 0}, n + "(res, NULL)");
        h.resource_ex(0x20000, 0, kSpurs, 1);
        h.open("with no start", fn);
        h.resource_ex(0x20000, start, 0, 1);
        h.open("with no SPURS", fn);
        h.resource_ex(0x20000, start, kSpurs, 0);
        h.open("at maxContention 0", fn);
        h.resource_ex(0x20000, start, kSpurs, 9);
        h.open("at maxContention 9", fn);
        h.resource_ex(pad + kHandleSize - 1, start, kSpurs, 1);
        h.open("one byte short of the handle's memory", fn);
        h.resource_ex(pad + kHandleSize + 0x27FF, start, kSpurs, 1);
        h.open("one byte short of the work memory", fn);
        h.words(n + ": after that", start + pad);
        h.resource_ex(0x20000, start, kSpurs, 8);
        const u32 x = h.open("at maxContention 8", fn);
        REQUIRE(x);
        REQUIRE(h.start(n, x, 2, 1) == 0);
        const std::vector<float> a = audio(240 * 6, 0, 7);
        for (u32 f = 0; f < 6; f++) h.frame(std::format("{}: frame {}", n, f), x, &a[f * 240], 240, true);
        h.r("cellCelpEncEncodeFrame", {x, kPcmInfo}, n + ": EncodeFrame before Close");
        h.r("cellCelpEncClose", {x}, n + ": Close with a frame being encoded");
        h.words(n + ": Close", x);
    }
    REQUIRE(h.t.call("cellSpursFinalize", kSpurs) == 0);
    h.o.note(std::format("{} frames encoded", frames_total));
    return std::move(h.o);
}

} // namespace

TEST_CASE("cellCelpEnc's comparison calls Sony's exports by their names; pspocket's library", "[hle][celp]")
{
    check_names("cellCelpEnc", {"cellCelpEncQueryAttr", "cellCelpEncOpen", "cellCelpEncOpenEx", "cellCelpEncOpenExt",
                                   "cellCelpEncClose", "cellCelpEncStart", "cellCelpEncEnd", "cellCelpEncEncodeFrame",
                                   "cellCelpEncWaitForOutput", "cellCelpEncGetAu"},
        /*marked=*/false);
}

TEST_CASE("cellCelpEnc against Sony's libcelpenc: its checks and states, Start's headers, every access unit",
    "[hle][lle][compare][celp]")
{
    const Setup s = setup();
    run(s, "cellCelpEnc against Sony's libcelpenc", [&](bool sony) { return half_enc(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("QueryAttr: the attributes") == "hex:0000a6000400000000000100eeeeeeee");
            CHECK(o.value("Open at the highest priorities: the handle") == "the work memory +0x80");
            CHECK(o.value("Start configuration 0 int16: the header") ==
                  "hex:2e6d70340040030100003e800000384000d800d800000000020006c0");
        });
}

// Without the firmware: pspocket's encoder through its library, its output decoded by pspocket's CELP decoder.
TEST_CASE("cellCelpEnc: the states, the headers, and speech that decodes back", "[hle][celp]")
{
    CelpFixture f;
    GuestThread t(f, 0, "game");
    u8* b = f.base();
    CHECK(t.call("cellCelpEncQueryAttr", kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == 0xA600);
    ps_write32(b, kRes, 0x20000);
    ps_write32(b, kRes + 4, kWork + 0x10);
    ps_write32(b, kRes + 8, 1000);
    ps_write32(b, kRes + 12, 100);
    REQUIRE(t.call("cellCelpEncOpen", kRes, kHandle) == 0);
    const u32 h = ps_read32(b, kHandle);
    CHECK(h == kWork + 0x80);
    CHECK(static_cast<u32>(t.call("cellCelpEncEncodeFrame", h, kPcmInfo)) == SEQ);
    static constexpr u32 kBytes[4] = {27, 20, 35, 43};
    static constexpr u32 kBits[4] = {216, 160, 280, 338};
    for (u32 config = 0; config < 4; config++) {
        INFO("configuration " << config);
        ps_write32(b, kParam, 1);
        ps_write32(b, kParam + 4, 2);
        ps_write32(b, kParam + 8, config);
        ps_write32(b, kParam + 12, 0);
        ps_write32(b, kParam + 16, kHeader);
        REQUIRE(t.call("cellCelpEncStart", h, kParam) == 0);
        REQUIRE(ps_read32(b, kParam + 20) == 28);
        // The header: ".mp4", the sampling rate, the bit rate, a frame's bits, the CelpHeader (RPE, 16 kHz).
        CHECK(ps_read32(b, kHeader) == 0x2E6D7034);
        CHECK(ps_read32(b, kHeader + 8) == 16000);
        CHECK(ps_read32(b, kHeader + 16) == (kBits[config] << 16 | kBits[config]));
        CHECK(b[kHeader + 27] == (0xC0 | config << 2));
        const u32 samples = config == 1 ? 160 : 240, frames = 60;
        const std::vector<float> x = audio(samples * frames, 0, config + 1);
        hle::celp::RpeDecoder dec;
        {
            hle::celp::spu::Scope scope;
            dec.reset(config);
        }
        double signal = 0, noise = 0;
        for (u32 k = 0; k < frames; k++) {
            put_frame(b, kPcm, &x[k * samples], samples, false);
            ps_write32(b, kPcmInfo, kPcm);
            ps_write32(b, kPcmInfo + 4, samples * 2);
            REQUIRE(t.call("cellCelpEncEncodeFrame", h, kPcmInfo) == 0);
            REQUIRE(t.call("cellCelpEncWaitForOutput", h) == 0);
            REQUIRE(t.call("cellCelpEncGetAu", h, kAu, kAuInfo) == 0);
            const u32 size = ps_read32(b, kAuInfo + 4);
            REQUIRE(size == (k < 2 ? 0 : kBytes[config]));
            if (k < 2) continue;
            // Access unit k is frame k - 2 (the encoder's look-ahead).
            float y[240];
            u32 used = 0;
            {
                hle::celp::spu::Scope scope;
                REQUIRE(dec.decode(b + kAu, size, &used, y, 0));
            }
            CHECK(used == size);
            if (k < 8) continue; // the decoder's adaptive codebook settles
            for (u32 i = 0; i < samples; i++) {
                const double in = std::lround(std::clamp(x[(k - 2) * samples + i] * 32767.0f, -32768.0f, 32767.0f));
                signal += in * in;
                const double d = static_cast<double>(y[i]) - in;
                noise += d * d;
            }
        }
        // About 6 dB (the encoder weighs the error perceptually); a broken coder gives none.
        const double snr = 10 * std::log10(signal / noise);
        INFO("SNR " << snr << " dB");
        CHECK(snr > 4.0);
    }
    CHECK(t.call("cellCelpEncClose", h) == 0);
    CHECK(static_cast<u32>(t.call("cellCelpEncClose", h)) == SEQ);
}
