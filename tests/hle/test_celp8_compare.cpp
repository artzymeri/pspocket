// libcelp8enc (the MPEG-4 CELP encoder at 8 kHz) against Sony's module (D-932). The same resources, parameters and
// synthetic audio go to pspocket's library and to Sony's module. Compared: QueryAttr's attributes; every answer of
// Open, OpenEx, Start, EncodeFrame, WaitForOutput, GetAu, End and Close, through their checks and in every state; the
// handle (as an offset into the work memory) and the words of it libcelp8enc keeps its state in; the resource OpenEx
// changes; Start's header byte for byte; every access unit byte for byte and the information GetAu writes (Sony's SPU
// program encodes in the SPU's single precision; pspocket's encoder does the same arithmetic in the same order,
// celp_mpe_enc.cpp).
//
// The structures (RPCS3's cellCelp8Enc.h): CellCelp8EncAttr {workMemSize, celpEncVerUpper, celpEncVerLower};
// CellCelp8EncResource {totalMemSize, startAddr, ppuThreadPriority, spuThreadPriority, ppuThreadStackSize};
// CellCelp8EncResourceEx {totalMemSize, startAddr, spurs, priority[8], maxContention}; CellCelp8EncParam
// {excitationMode, sampleRate, configuration, wordSize, outBuff, outSize}; CellCelp8EncPcmInfo and CellCelp8EncAuInfo
// {startAddr, size}.
//
// The audio is built here: speech-like (a pulse train through formant resonators), sines, a chirp, noise, a square
// wave, impulses, silence, speech past full scale. No game data; the comparison skips without the firmware
// (compare_util.hpp).

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
constexpr u32 kHandleSize = 0x7A80;
constexpr u32 kConfigs[10] = {0, 2, 6, 9, 12, 15, 18, 21, 24, 26};
enum : u32 { ARG = 0x806140A3, SEQ = 0x806140A2 };

struct CelpFixture : TlsFixture {
    CelpFixture()
    {
        REQUIRE(hle->main_memory().reserve(kWork, kWorkSize));
        REQUIRE(mem.commit(kWork, kWorkSize));
    }
};

// Synthetic audio at 8 kHz, full scale 1. Kinds: 0 speech-like, 1 silence, 2 a sine, 3 a chirp, 4 noise near full
// scale, 5 a square wave, 6 impulses, 7 noise at the level of a 16-bit sample's last bits, 8 a constant, 9 speech
// gated by silence, 10 speech eight times louder (past full scale).
std::vector<float> audio(u32 n, u32 kind, u32 seed)
{
    constexpr double kRate = 8000;
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
        const double t = static_cast<double>(i) / kRate;
        const double f0 = 110 + 40 * std::sin(2 * M_PI * 0.7 * t) + 15 * seed;
        ph += f0 / kRate;
        double x = 0;
        if (ph >= 1) {
            ph -= 1;
            x = 1;
        }
        st = st * 1664525u + 1013904223u;
        x += (static_cast<double>(st >> 8) / 16777216.0 - 0.5) * 0.05;
        double v = x;
        for (int k = 0; k < 3; k++) {
            const double r = std::exp(-M_PI * bw[k] / kRate), c = 2 * r * std::cos(2 * M_PI * formants[k] / kRate);
            const double y = v + c * y1[k] - r * r * y2[k];
            y2[k] = y1[k];
            y1[k] = y;
            v = y * (1 - r);
        }
        double s = v * 0.6 * (0.5 + 0.5 * std::sin(2 * M_PI * 1.3 * t));
        switch (kind) {
        case 1: s = 0; break;
        case 2: s = 0.6 * std::sin(2 * M_PI * (150 + 100 * seed) * t); break;
        case 3: s = 0.5 * std::sin(2 * M_PI * (100 + 1500 * t) * t); break;
        case 4: s = 1.9 * rnd(); break;
        case 5: s = std::fmod(t * 110, 1.0) < 0.5 ? 0.37 : -0.37; break;
        case 6: s = i % 61 == 0 ? 0.9 : 0; break;
        case 7: s = 3 * rnd() / 32767; break;
        case 8: s = 0.03; break;
        case 9: s = (i / 800) % 2 ? s : 0; break;
        case 10: s *= 8; break;
        default: break;
        }
        out[i] = static_cast<float>(s);
    }
    return out;
}

// One frame of the audio as libcelp8enc takes it: big-endian floats.
void put_frame(u8* b, u32 at, const float* x, u32 samples)
{
    for (u32 i = 0; i < samples; i++) ps_write32(b, at + 4 * i, std::bit_cast<u32>(x[i]));
}

u32 frame_samples(u32 config) { return hle::celp::kMpeConfigs[config].frame_size; }

struct Enc : Half<CelpFixture> {
    using Half::Half;

    std::string where(u32 h) const
    {
        if (h >= kWork && h < kWork + kWorkSize) return std::format("the work memory +{:#x}", h - kWork);
        return std::format("{:#x}", h);
    }
    // The words of the handle libcelp8enc keeps its state, a frame's size and the access unit's size in.
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
    u32 open(const std::string& what, const std::string& fn = "cellCelp8EncOpen")
    {
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = Half::r(fn, {kRes, kHandle}, fn.substr(12) + " " + what);
        const u32 h = ps_read32(b, kHandle);
        o.line(fn.substr(12) + " " + what + ": the handle", r == 0 ? where(h) : std::format("{:#x}", h));
        o.line(fn.substr(12) + " " + what + ": the resource", hex(b + kRes, 24));
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
    u64 start(const std::string& what, u32 h, u32 config, bool out = true)
    {
        param(0, 1, config, 0, out ? kHeader : 0);
        std::memset(b + kHeader, kFill, 64);
        const u64 r = Half::r("cellCelp8EncStart", {h, kParam}, "Start " + what);
        const u32 size = ps_read32(b, kParam + 20);
        o.line("Start " + what + ": outSize", std::format("{:#x}", size));
        if (r == 0 && out) o.line("Start " + what + ": the header", hex(b + kHeader, std::min<u32>(size + 3, 64)));
        words("Start " + what, h);
        return r;
    }
    // One frame encoded and taken: EncodeFrame, WaitForOutput, GetAu (the access unit as data).
    void frame(const std::string& what, u32 h, const float* x, u32 samples)
    {
        put_frame(b, kPcm, x, samples);
        ps_write32(b, kPcmInfo, kPcm);
        ps_write32(b, kPcmInfo + 4, samples * 4);
        Half::r("cellCelp8EncEncodeFrame", {h, kPcmInfo}, what + ": EncodeFrame");
        Half::r("cellCelp8EncWaitForOutput", {h}, what + ": WaitForOutput");
        std::memset(b + kAu, kFill, 128);
        ps_write32(b, kAuInfo, 0xEEEEEEEE);
        ps_write32(b, kAuInfo + 4, 0xEEEEEEEE);
        Half::r("cellCelp8EncGetAu", {h, kAu, kAuInfo}, what + ": GetAu");
        const u32 size = ps_read32(b, kAuInfo + 4);
        o.line(what + ": auItem", std::format("{} {:#x}", ps_read32(b, kAuInfo) == kAu ? "outBuffer" : "?", size));
        o.bytes(what + ": the access unit", b + kAu, std::min<u32>(size + 2, 128));
    }
};

Observed half_enc8(const Setup& s, bool sony_half)
{
    Enc h(s, sony_half, "cellCelp8Enc");
    u8* b = h.b;

    // QueryAttr.
    h.r("cellCelp8EncQueryAttr", {0}, "QueryAttr(NULL)");
    std::memset(b + kAttr, kFill, 16);
    h.r("cellCelp8EncQueryAttr", {kAttr}, "QueryAttr");
    h.o.line("QueryAttr: the attributes", hex(b + kAttr, 16));

    // Open's checks: the resource, the handle, the start, the priorities, the memory for the handle's own (0x7A80
    // bytes from the start's next 128-byte boundary) and what is left after it (at least 0x4000).
    const u32 start = kWork + 0x10, pad = 0x70;
    std::memset(b + kWork, kFill, 0x10000);
    h.r("cellCelp8EncOpen", {0, kHandle}, "Open(NULL, handle)");
    h.resource(0x10000, start);
    h.r("cellCelp8EncOpen", {kRes, 0}, "Open(res, NULL)");
    h.resource(0x10000, 0);
    h.open("with no start");
    h.resource(0x10000, start, 0xC00);
    h.open("at PPU priority 0xc00");
    h.resource(0x10000, start, 0xBFF, 0x100);
    h.open("at SPU priority 0x100");
    h.resource(pad + kHandleSize - 1, start);
    h.open("one byte short of the handle's memory");
    h.words("after that", start + pad);
    h.resource(pad + kHandleSize + 0x3FFF, start);
    h.open("one byte short of the work memory");
    h.words("after that", start + pad);
    h.resource(kHandleSize + 0x4000, start + pad);
    const u32 aligned = h.open("at a 128-byte boundary, the least memory");
    if (aligned) h.r("cellCelp8EncClose", {aligned}, "Close of that");

    // Before Start: every call but Start and Close out of sequence.
    h.resource(0x20000, start, 0xBFF, 0xFF);
    const u32 e = h.open("at the highest priorities");
    REQUIRE(e);
    h.words("Open", e);
    h.r("cellCelp8EncEnd", {e}, "End before Start");
    h.r("cellCelp8EncWaitForOutput", {e}, "WaitForOutput before Start");
    ps_write32(b, kPcmInfo, kPcm);
    ps_write32(b, kPcmInfo + 4, 1280);
    h.r("cellCelp8EncEncodeFrame", {e, kPcmInfo}, "EncodeFrame before Start");
    h.r("cellCelp8EncGetAu", {e, kAu, kAuInfo}, "GetAu before Start");
    h.r("cellCelp8EncGetAu", {e, kAu, 0}, "GetAu(handle, out, NULL) before Start");
    for (const u32 fn : {0u, 1u, 2u, 3u, 4u, 5u}) {
        const char* names[] = {"cellCelp8EncEnd", "cellCelp8EncWaitForOutput", "cellCelp8EncClose",
            "cellCelp8EncEncodeFrame", "cellCelp8EncGetAu", "cellCelp8EncStart"};
        h.r(names[fn], {0, kPcmInfo, kAuInfo}, std::format("{}(NULL)", names[fn]));
    }

    // Start's checks: the excitation mode (0), the sampling rate (1), the configuration (the ten), the word size (0).
    h.r("cellCelp8EncStart", {e, 0}, "Start(handle, NULL)");
    struct Bad {
        u32 mode, rate, config, word;
    };
    for (const Bad& p : {Bad{1, 1, 0, 0}, Bad{0xFFFFFFFF, 1, 0, 0}, Bad{0, 0, 0, 0}, Bad{0, 2, 0, 0}, Bad{0, 1, 1, 0},
             Bad{0, 1, 3, 0}, Bad{0, 1, 25, 0}, Bad{0, 1, 27, 0}, Bad{0, 1, 0xFFFFFFFF, 0}, Bad{0, 1, 0, 1},
             Bad{0, 1, 0, 0xFFFFFFFF}}) {
        h.param(p.mode, p.rate, p.config, p.word, kHeader);
        h.r("cellCelp8EncStart", {e, kParam},
            std::format("Start mode {:#x} rate {} configuration {:#x} word size {:#x}", p.mode, p.rate, p.config,
                p.word));
    }
    h.words("after Start's checks", e);

    // Each configuration: Start's header, then frames of each kind of audio, every access unit.
    u32 frames_total = 0;
    for (u32 c = 0; c < 10; c++) {
        const u32 config = kConfigs[c];
        const u32 samples = frame_samples(config);
        const std::string tag = std::format("configuration {}", config);
        REQUIRE(h.start(tag, e, config) == 0);
        const u32 kinds[] = {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1};
        const u32 per = 4;
        const std::vector<float> x = audio(samples * per * 11, 0, c + 1);
        for (u32 k = 0; k < 11; k++) {
            const std::vector<float> y = audio(samples * per, kinds[k], c + k);
            for (u32 f = 0; f < per; f++) {
                const float* src = kinds[k] == 0 ? &x[(k * per + f) * samples] : &y[f * samples];
                h.frame(std::format("{}: frame {}", tag, k * per + f), e, src, samples);
                frames_total++;
            }
        }
        // The frame's size exactly, and one frame at a time.
        ps_write32(b, kPcmInfo + 4, samples * 4 - 1);
        h.r("cellCelp8EncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame of a byte less");
        ps_write32(b, kPcmInfo + 4, samples * 4 + 4);
        h.r("cellCelp8EncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame of a sample more");
        ps_write32(b, kPcmInfo + 4, samples * 2);
        h.r("cellCelp8EncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame of 16-bit samples");
        ps_write32(b, kPcmInfo, 0);
        ps_write32(b, kPcmInfo + 4, samples * 4);
        h.r("cellCelp8EncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame of no frame");
        h.r("cellCelp8EncEncodeFrame", {e, 0}, tag + ": EncodeFrame(handle, NULL)");
        h.r("cellCelp8EncWaitForOutput", {e}, tag + ": WaitForOutput with no frame");
        h.r("cellCelp8EncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu with no frame");
        ps_write32(b, kPcmInfo, kPcm);
        h.r("cellCelp8EncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame");
        h.r("cellCelp8EncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame again");
        h.r("cellCelp8EncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu before WaitForOutput");
        h.r("cellCelp8EncWaitForOutput", {e}, tag + ": WaitForOutput");
        h.r("cellCelp8EncWaitForOutput", {e}, tag + ": WaitForOutput again");
        h.words(tag + ": an access unit waiting", e);
        if (c == 0) {
            // GetAu without a buffer: the size 0, the access unit taken all the same.
            ps_write32(b, kAuInfo, 0xEEEEEEEE);
            h.r("cellCelp8EncGetAu", {e, 0, kAuInfo}, tag + ": GetAu(handle, NULL, auItem)");
            h.o.line(tag + ": auItem", hex(b + kAuInfo, 8));
            h.r("cellCelp8EncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu again");
            // End with a frame being encoded, then Start again from state 1.
            h.r("cellCelp8EncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame before End");
            h.r("cellCelp8EncEnd", {e}, tag + ": End with a frame being encoded");
            h.words(tag + ": End", e);
            h.r("cellCelp8EncEnd", {e}, tag + ": End again");
        } else if (c == 1) {
            // End with an access unit waiting.
            h.r("cellCelp8EncEnd", {e}, tag + ": End with an access unit waiting");
            h.words(tag + ": End", e);
        } else if (c == 2) {
            // Start again with a frame being encoded: out of sequence.
            h.r("cellCelp8EncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu");
            h.r("cellCelp8EncEncodeFrame", {e, kPcmInfo}, tag + ": EncodeFrame before Start");
            h.param(0, 1, config, 0, kHeader);
            h.r("cellCelp8EncStart", {e, kParam}, tag + ": Start with a frame being encoded");
            h.r("cellCelp8EncWaitForOutput", {e}, tag + ": WaitForOutput after that");
            h.param(0, 1, config, 0, kHeader);
            h.r("cellCelp8EncStart", {e, kParam}, tag + ": Start with an access unit waiting");
            h.r("cellCelp8EncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu after that");
        } else {
            // Start again while started (it ends first): the access unit waiting is lost.
            h.r("cellCelp8EncGetAu", {e, kAu, kAuInfo}, tag + ": GetAu");
        }
    }
    // Start without a buffer for the header, then Close while started; Close again.
    h.start("without a buffer for the header", e, 26, false);
    h.r("cellCelp8EncClose", {e}, "Close while started");
    h.words("Close", e);
    h.r("cellCelp8EncClose", {e}, "Close again");
    h.param(0, 1, 0, 0, kHeader);
    h.r("cellCelp8EncStart", {e, kParam}, "Start after Close");

    // OpenEx on a SPURS instance of the game's: its checks, and the resource it changes.
    REQUIRE(h.t.call("cellSpursInitialize", kSpurs, 2, 200, 2016, 0) == 0);
    {
        const std::string fn = "cellCelp8EncOpenEx", n = "OpenEx";
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
        h.resource_ex(pad + kHandleSize + 0x3FFF, start, kSpurs, 1);
        h.open("one byte short of the work memory", fn);
        h.words(n + ": after that", start + pad);
        h.resource_ex(0x20000, start, kSpurs, 8);
        const u32 x = h.open("at maxContention 8", fn);
        REQUIRE(x);
        REQUIRE(h.start(n, x, 6) == 0);
        const std::vector<float> a = audio(160 * 6, 0, 7);
        for (u32 f = 0; f < 6; f++) h.frame(std::format("{}: frame {}", n, f), x, &a[f * 160], 160);
        h.r("cellCelp8EncEncodeFrame", {x, kPcmInfo}, n + ": EncodeFrame before Close");
        h.r("cellCelp8EncClose", {x}, n + ": Close with a frame being encoded");
        h.words(n + ": Close", x);
    }
    REQUIRE(h.t.call("cellSpursFinalize", kSpurs) == 0);
    h.o.note(std::format("{} frames encoded", frames_total));
    return std::move(h.o);
}

} // namespace

TEST_CASE("cellCelp8Enc's comparison calls Sony's exports by their names; pspocket's library", "[hle][celp]")
{
    check_names("cellCelp8Enc", {"cellCelp8EncQueryAttr", "cellCelp8EncOpen", "cellCelp8EncOpenEx",
                                    "cellCelp8EncClose", "cellCelp8EncStart", "cellCelp8EncEnd",
                                    "cellCelp8EncEncodeFrame", "cellCelp8EncWaitForOutput", "cellCelp8EncGetAu"},
        /*marked=*/false);
}

TEST_CASE("cellCelp8Enc against Sony's libcelp8enc: its checks and states, Start's headers, every access unit",
    "[hle][lle][compare][celp]")
{
    const Setup s = setup();
    run(s, "cellCelp8Enc against Sony's libcelp8enc", [&](bool sony) { return half_enc8(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("QueryAttr: the attributes") == "hex:0000bb000400000000000200eeeeeeee");
            CHECK(o.value("Open at the highest priorities: the handle") == "the work memory +0x80");
            CHECK(o.value("Start configuration 0: the header") ==
                  "hex:2e6d70340040030100001f4000000f0a009a009a0000000002000b0000eeeeee");
        });
}

// Without the firmware: pspocket's encoder through its library, its output decoded by pspocket's CELP decoder.
TEST_CASE("cellCelp8Enc: the states, the headers, and speech that decodes back", "[hle][celp]")
{
    CelpFixture f;
    GuestThread t(f, 0, "game");
    u8* b = f.base();
    CHECK(t.call("cellCelp8EncQueryAttr", kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == 0xBB00);
    ps_write32(b, kRes, 0x20000);
    ps_write32(b, kRes + 4, kWork + 0x10);
    ps_write32(b, kRes + 8, 1000);
    ps_write32(b, kRes + 12, 100);
    REQUIRE(t.call("cellCelp8EncOpen", kRes, kHandle) == 0);
    const u32 h = ps_read32(b, kHandle);
    CHECK(h == kWork + 0x80);
    CHECK(static_cast<u32>(t.call("cellCelp8EncEncodeFrame", h, kPcmInfo)) == SEQ);
    for (const u32 config : kConfigs) {
        INFO("configuration " << config);
        ps_write32(b, kParam, 0);
        ps_write32(b, kParam + 4, 1);
        ps_write32(b, kParam + 8, config);
        ps_write32(b, kParam + 12, 0);
        ps_write32(b, kParam + 16, kHeader);
        REQUIRE(t.call("cellCelp8EncStart", h, kParam) == 0);
        REQUIRE(ps_read32(b, kParam + 20) == 29);
        // The header: ".mp4", the sampling rate, the bit rate, a frame's bits, the CelpHeader (MPE, 8 kHz).
        const u32 samples = frame_samples(config);
        CHECK(ps_read32(b, kHeader) == 0x2E6D7034);
        CHECK(ps_read32(b, kHeader + 8) == 8000);
        const u32 bitrate = ps_read32(b, kHeader + 12), bits = ps_read32(b, kHeader + 16) >> 16;
        CHECK(bits == bitrate * samples / 8000);
        CHECK(b[kHeader + 27] == config);
        const u32 frames = 16000 / samples;
        const std::vector<float> x = audio(samples * frames, 0, config + 1);
        hle::celp::MpeDecoder dec;
        {
            hle::celp::spu::Scope scope;
            dec.reset(config);
        }
        double signal = 0, noise = 0;
        for (u32 k = 0; k < frames; k++) {
            put_frame(b, kPcm, &x[k * samples], samples);
            ps_write32(b, kPcmInfo, kPcm);
            ps_write32(b, kPcmInfo + 4, samples * 4);
            REQUIRE(t.call("cellCelp8EncEncodeFrame", h, kPcmInfo) == 0);
            REQUIRE(t.call("cellCelp8EncWaitForOutput", h) == 0);
            REQUIRE(t.call("cellCelp8EncGetAu", h, kAu, kAuInfo) == 0);
            const u32 size = ps_read32(b, kAuInfo + 4);
            REQUIRE(size == (k < 1 ? 0 : (bits + 7) / 8));
            if (k < 1) continue;
            // Access unit k is frame k - 1 (the encoder's look-ahead).
            float y[320];
            u32 used = 0;
            {
                hle::celp::spu::Scope scope;
                dec.decode(b + kAu, size, &used, y, 0);
            }
            CHECK(used == size);
            if (k < 8) continue; // the decoder's adaptive codebook settles
            for (u32 i = 0; i < samples; i++) {
                const double in = static_cast<double>(x[(k - 1) * samples + i]) * 32767;
                signal += in * in;
                const double d = static_cast<double>(y[i]) - in;
                noise += d * d;
            }
        }
        // About 7 to 9 dB (the encoder weighs the error perceptually); a broken coder gives none.
        const double snr = 10 * std::log10(signal / noise);
        INFO("SNR " << snr << " dB");
        CHECK(snr > 5.0);
    }
    CHECK(t.call("cellCelp8EncClose", h) == 0);
    CHECK(static_cast<u32>(t.call("cellCelp8EncClose", h)) == SEQ);
}
