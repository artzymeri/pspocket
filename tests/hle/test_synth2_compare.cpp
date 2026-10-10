// libsynth2 against Sony's libsynth2 (D-610, D-597): the same calls with the same synthetic sounds go to pspocket's
// library and to Sony's module. Compared: every answer; the registers after Init and after writes (GetParam, GetSwitch,
// GetAddr: what libsd's initialisation leaves, the masks, the addresses' alignment, the unknown registers); Note2Pitch
// and Pitch2Note over notes, fines and pitches; the transfers' answers and sound memory read back; and Generate's
// samples (Lout, Rout, Ls and Rs as floats, within 5 steps of 1/32768) for the silence after Init, a looped sine at
// pitch 0x1000, a one-shot at half pitch with decay and release, an odd pitch keyed off, core 0 through core 1 with the
// mixes switched, noise and pitch modulation, a swept volume, the reverb (the effect off and on, a studio, a hall, an
// echo, SetEffectMode) with its work area; ENDX, ENVX, NAX and LSAX after each. No game data; skipped without the
// firmware (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "compare_util.hpp"
#include "vag_writer.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

namespace {

constexpr u32 kAttr = kData + 0x100, kSrc = kData + 0x1000, kRead = kData + 0x30000, kOutL = kData + 0x80000,
              kOutR = kData + 0x90000, kOutLs = kData + 0xA0000, kOutRs = kData + 0xB0000;
constexpr double kTolerance = 5.0 / 32768.0;

// libsd's register numbers as libsd-common.h has them: the parameter in bits 8-15, SD_VOICE(core, voice); the cores'
// volumes (SD_PARAM_MVOLL to MVOLXR) carry 0x80, MMIX, the switches and the addresses do not.
enum : u32 {
    VOLL = 0x00, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLXL, VOLXR,
    MMIX = 0x08, MVOLL, MVOLR, EVOLL, EVOLR, AVOLL, AVOLR, BVOLL, BVOLR, MVOLXL, MVOLXR,
    PMON = 0x13, NON, KON, KOFF, ENDX, VMIXL, VMIXEL, VMIXR, VMIXER,
    ESA = 0x1C, EEA, TSA, IRQA, SSA, LSAX, NAX,
};
constexpr u64 vreg(u32 param, u32 core, u32 voice) { return param << 8 | voice << 1 | core; }
constexpr u64 creg(u32 param, u32 core) { return param == MMIX ? (param << 8 | core) : (param << 8 | 0x80 | core); }
constexpr u64 reg(u32 param, u32 core) { return param << 8 | core; }
constexpr u64 kEffectEnable = 0x2, kNoiseClock = 0x8;

struct Run : Half<> {
    Run(const Setup& s, bool sony_half) : Half(s, sony_half, "libsynth2") {}

    // A call whose answer is not compared (the void ones: r3 is whatever the function left there).
    u64 call(std::string_view fn, std::initializer_list<u64> args)
    {
        trace("calling: " + std::string(fn));
        const std::optional<u64> v = side.call(fn, args);
        return v ? *v : ~u64{0};
    }
    void set_param(u64 r, u64 v) { call("cellSoundSynth2SetParam", {r, v}); }
    void set_switch(u64 r, u64 v) { call("cellSoundSynth2SetSwitch", {r, v}); }
    void core_attr(u64 entry, u64 v) { call("cellSoundSynth2SetCoreAttr", {entry, v}); }
    // The getters' answers, as the type they return (u16, u32).
    std::string param(u64 r) { return std::format("{:#06x}", static_cast<u16>(call("cellSoundSynth2GetParam", {r}))); }
    std::string sw(u64 r) { return std::format("{:#08x}", static_cast<u32>(call("cellSoundSynth2GetSwitch", {r}))); }
    std::string addr(u64 r) { return std::format("{:#08x}", static_cast<u32>(call("cellSoundSynth2GetAddr", {r}))); }

    // Every register a game can read: the voices' given, the cores' parameters, switches and addresses.
    void registers(const std::string& when, std::initializer_list<std::pair<u32, u32>> voices)
    {
        o.group("registers");
        for (const auto& [core, voice] : voices) {
            for (u32 p = VOLL; p <= VOLXR; p++)
                o.line(std::format("{}: voice {}/{} parameter {:#x}", when, core, voice, p),
                    param(vreg(p, core, voice)));
            for (u32 p = SSA; p <= NAX; p++)
                o.line(std::format("{}: voice {}/{} address {:#x}", when, core, voice, p), addr(vreg(p, core, voice)));
        }
        for (u32 core = 0; core < 2; core++) {
            for (u32 p = MMIX; p <= MVOLXR; p++)
                o.line(std::format("{}: core {} parameter {:#x}", when, core, p), param(creg(p, core)));
            for (u32 p = PMON; p <= VMIXER; p++)
                o.line(std::format("{}: core {} switch {:#x}", when, core, p), sw(reg(p, core)));
            for (u32 p = ESA; p <= IRQA; p++)
                o.line(std::format("{}: core {} address {:#x}", when, core, p), addr(reg(p, core)));
        }
        o.group("");
    }
    // What a game polls of a voice.
    void voice_state(const std::string& when, u32 core, u32 voice)
    {
        o.line(when + ": ENVX", param(vreg(ENVX, core, voice)));
        o.line(when + ": VOLXL", param(vreg(VOLXL, core, voice)));
        o.line(when + ": NAX", addr(vreg(NAX, core, voice)));
        o.line(when + ": LSAX", addr(vreg(LSAX, core, voice)));
        o.line(when + ": ENDX", sw(reg(ENDX, core)));
    }
    void load(u32 s_addr, const std::vector<u8>& data, const std::string& key)
    {
        std::memcpy(b + kSrc, data.data(), data.size());
        r("cellSoundSynth2VoiceTrans", {0, 0, kSrc, s_addr, data.size()}, key);
    }
    void voice(u32 core, u32 v, u16 voll, u16 volr, u16 pitch, u16 adsr1, u16 adsr2, u32 ssa)
    {
        set_param(vreg(VOLL, core, v), voll);
        set_param(vreg(VOLR, core, v), volr);
        set_param(vreg(PITCH, core, v), pitch);
        set_param(vreg(ADSR1, core, v), adsr1);
        set_param(vreg(ADSR2, core, v), adsr2);
        r("cellSoundSynth2SetAddr", {vreg(SSA, core, v), ssa}, std::format("SetAddr(SSA {}/{}, {:#x})", core, v, ssa));
    }
    // `n` samples (in Generate calls of at most 1024), the outputs as data; Ls and Rs given when `surround`, every
    // output first filled with a value Generate never writes (2.0f), so a sample left out shows.
    void generate(const std::string& key, u32 n, bool surround = false)
    {
        for (u32 i = 0; i < n; i++)
            for (const u32 out : {kOutL, kOutR, kOutLs, kOutRs}) ps_writef32(b, out + 4 * i, 2.0f);
        for (u32 done = 0; done < n;) {
            const u32 m = std::min<u32>(1024, n - done);
            r("cellSoundSynth2Generate", {m, kOutL + 4 * done, kOutR + 4 * done, surround ? kOutLs + 4 * done : 0,
                                             surround ? kOutRs + 4 * done : 0},
                std::format("{}: Generate({}) at sample {}", key, m, done));
            done += m;
        }
        o.bytes(key + ": Lout", b + kOutL, 4 * n, Rule::floats(kTolerance));
        o.bytes(key + ": Rout", b + kOutR, 4 * n, Rule::floats(kTolerance));
        if (surround) {
            o.bytes(key + ": Ls", b + kOutLs, 4 * n, Rule::floats(kTolerance));
            o.bytes(key + ": Rs", b + kOutRs, 4 * n, Rule::floats(kTolerance));
        }
    }
    // A cold start between the scenarios: Exit, Init(0), core 1's master volume up.
    void fresh(const std::string& key)
    {
        r("cellSoundSynth2Exit", {}, key + ": Exit");
        r("cellSoundSynth2Init", {0}, key + ": Init(0)");
        set_param(creg(MVOLL, 1), 0x3FFF);
        set_param(creg(MVOLR, 1), 0x3FFF);
    }
    // The effect attributes: core, mode, depths, delay, feedback (CellSoundSynth2EffectAttr).
    void effect_attr(u16 mode, u16 depth_l, u16 depth_r, u16 delay, u16 feedback)
    {
        ps_write16(b, kAttr, 0);
        ps_write16(b, kAttr + 2, mode);
        ps_write16(b, kAttr + 4, depth_l);
        ps_write16(b, kAttr + 6, depth_r);
        ps_write16(b, kAttr + 8, delay);
        ps_write16(b, kAttr + 10, feedback);
    }
};

Observed half(const Setup& s, bool sony_half)
{
    Run h(s, sony_half);
    u8* b = h.b;

    // ---- before Init, Init, what libsd's initialisation leaves ----
    h.o.line("GetParam(PITCH 0/0) before Init", h.param(vreg(PITCH, 0, 0)));
    h.r("cellSoundSynth2Exit", {}, "Exit before Init");
    for (const auto& [param, value] : {std::pair<u64, u64>{0, 0}, {1, 1}, {2, 0x100}, {0xFFFF, 0xFFFFFFFF}})
        h.r("cellSoundSynth2Config", {param, value}, std::format("Config({:#x}, {:#x})", param, value));
    h.r("cellSoundSynth2Init", {0}, "Init(0)");
    h.r("cellSoundSynth2Init", {0}, "Init(0) again");
    h.r("cellSoundSynth2Init", {1}, "Init(1) again");
    h.registers("after Init", {{0, 0}, {0, 7}, {0, 23}, {1, 0}, {1, 23}});
    h.r("cellSoundSynth2VoiceTrans", {0, 1, kRead, 0x5000, 32}, "VoiceTrans: read 32 bytes at 0x5000");
    h.o.line("sound memory at 0x5000 after Init", hex(b + kRead, 32));
    h.r("cellSoundSynth2VoiceTrans", {1, 1, kRead, 0x1DFFF0, 32}, "VoiceTrans: read 32 bytes at 0x1DFFF0");
    h.o.line("sound memory at 0x1DFFF0 after Init", hex(b + kRead, 32));
    h.generate("after Init", 64, true);
    h.registers("after Init and 64 samples", {{0, 0}, {1, 9}});

    // ---- the registers written and read back ----
    for (u32 p = VOLL; p <= VOLXR; p++) h.set_param(vreg(p, 1, 17), 0x1230 + p);
    for (u32 p = MMIX; p <= MVOLXR; p++) h.set_param(creg(p, 0), 0x8100 + p);
    for (u32 p = PMON; p <= VMIXER; p++)
        if (p != KON && p != KOFF) h.set_switch(reg(p, 0), 0xFF123456);
    h.set_param(vreg(PITCH, 0, 24), 0x1111);
    h.set_param(reg(0x13, 0), 0x2222);
    for (const auto& [r, v] : {std::pair<u64, u64>{vreg(SSA, 1, 9), 0x12345}, {vreg(LSAX, 1, 9), 0x2000F},
             {vreg(NAX, 1, 9), 0x30010}, {reg(TSA, 0), 0x777}, {reg(IRQA, 0), 0x1FFFFF}, {reg(ESA, 0), 0x150008},
             {reg(EEA, 0), 0x17ABCD}, {vreg(SSA, 1, 9), 0x200000}, {vreg(SSA, 1, 24), 0x1000}, {reg(0x1B, 0), 0x1000},
             {reg(0x23, 0), 0x1000}, {reg(EEA, 1), 0xFFFFFFFF}})
        h.r("cellSoundSynth2SetAddr", {r, v}, std::format("SetAddr({:#06x}, {:#x})", r, v));
    h.registers("after the writes", {{1, 17}, {1, 9}, {0, 17}});
    for (const u64 r : {vreg(PITCH, 0, 24), reg(0x13, 0), reg(0x40, 1)})
        h.o.line(std::format("GetParam({:#06x})", r), h.param(r));
    for (const u64 r : {reg(0x12, 1), reg(0x1C, 1)}) h.o.line(std::format("GetSwitch({:#06x})", r), h.sw(r));
    for (const u64 r : {vreg(SSA, 1, 30), reg(0x05, 1), reg(0x23, 0)})
        h.o.line(std::format("GetAddr({:#06x})", r), h.addr(r));

    // ---- Note2Pitch and Pitch2Note ----
    h.o.group("Note2Pitch");
    for (const auto& [cn, cf] : {std::pair<u64, u64>{60, 0}, {48, 64}, {72, 127}, {0, 0}})
        for (s32 note = 0; note < 128; note += 5)
            for (s32 fine = -128; fine < 128; fine += 23)
                h.r("cellSoundSynth2Note2Pitch", {cn, cf, static_cast<u64>(note), static_cast<u64>(fine)},
                    std::format("Note2Pitch({}, {}, {}, {})", cn, cf, note, fine));
    h.o.group("Pitch2Note");
    for (const auto& [cn, cf] : {std::pair<u64, u64>{60, 0}, {48, 64}, {72, 127}})
        for (u64 pitch = 0; pitch < 0x10000; pitch += pitch < 0x4000 ? 0x3D : 0x1F3)
            h.r("cellSoundSynth2Pitch2Note", {cn, cf, pitch}, std::format("Pitch2Note({}, {}, {:#x})", cn, cf, pitch));
    h.o.group("");

    // ---- transfers ----
    for (u32 i = 0; i < 0x3000; i++) ps_write8(b, kSrc + i, static_cast<u8>(i * 7 + 3));
    h.r("cellSoundSynth2VoiceTrans", {0, 0, kSrc, 0x10000, 0x3000}, "VoiceTrans: write 0x3000 bytes at 0x10000");
    h.o.line("TSA 0 after the write", h.addr(reg(TSA, 0)));
    h.r("cellSoundSynth2VoiceTrans", {1, 9, kRead, 0x10000, 0x2FFF}, "VoiceTrans: read 0x2fff bytes in I/O mode");
    h.o.line("TSA 1 after the read", h.addr(reg(TSA, 1)));
    h.o.bytes("sound memory at 0x10000 read back", b + kRead, 0x3000);
    for (const auto& [ch, flag] : {std::pair<u64, u64>{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {0, 2}})
        h.r("cellSoundSynth2VoiceTransStatus", {ch, flag}, std::format("VoiceTransStatus({}, {})", ch, flag));
    for (const auto& [mode, saddr, size] : {std::tuple<u64, u64, u64>{2, 0x10000, 16}, {3, 0x10000, 16},
             {0, 0x1FFFF0, 16}, {0, 0x1FFFF0, 17}, {0, 0x200000, 0}, {0, 0x10, 0xFFFFFFF0}, {0, 0x1000, 0}})
        h.r("cellSoundSynth2VoiceTrans", {0, mode, kSrc, saddr, size},
            std::format("VoiceTrans(mode {}, {:#x}, {:#x} bytes)", mode, saddr, size));
    h.r("cellSoundSynth2VoiceTrans", {2, 0, kSrc, 0x10000, 16}, "VoiceTrans on channel 2");

    // ---- a looped sine at pitch 0x1000 ----
    h.fresh("looped sine");
    h.load(0x10000, vag::looped_sine(), "looped sine: VoiceTrans");
    h.voice(1, 5, 0x3FFF, 0x2000, 0x1000, 0x000F, 0x1FC0, 0x10000);
    h.set_switch(reg(KON, 1), 1u << 5);
    h.voice_state("looped sine: keyed on", 1, 5);
    h.generate("looped sine", 2000);
    h.voice_state("looped sine: after 2000 samples", 1, 5);

    // ---- a one-shot at half pitch, an odd pitch keyed off ----
    h.fresh("one-shot");
    h.load(0x20000, vag::encode(vag::sine(280, 5, 24000), 0, 0, 1), "one-shot: VoiceTrans");
    h.load(0x30000, vag::looped_sine(), "odd pitch: VoiceTrans");
    h.voice(1, 0, 0x3000, 0x3000, 0x0800, 0x8A75, 0x4D4A, 0x20000);
    h.voice(1, 1, 0x2000, 0x7000, 0x1ABC, 0x1F3A, 0x8FE7, 0x30000);
    h.set_param(creg(MVOLR, 1), 0x2FFF);
    h.set_switch(reg(KON, 1), 3);
    h.generate("one-shot: the first 300", 300);
    h.voice_state("one-shot: after 300 samples, voice 0", 1, 0);
    h.voice_state("one-shot: after 300 samples, voice 1", 1, 1);
    h.set_switch(reg(KOFF, 1), 2);
    h.generate("one-shot: 400 after a key off", 400);
    h.voice_state("one-shot: after 700 samples, voice 0", 1, 0);
    h.voice_state("one-shot: after 700 samples, voice 1", 1, 1);
    h.generate("one-shot: 600 more", 600);
    h.set_switch(reg(KON, 1), 1);
    h.voice_state("one-shot: keyed on again", 1, 0);

    // ---- core 0 through core 1, the mixes ----
    h.fresh("cores");
    h.load(0x10000, vag::looped_sine(), "cores: VoiceTrans");
    h.voice(0, 2, 0x3FFF, 0x3FFF, 0x1000, 0x000F, 0x1FC0, 0x10000);
    h.voice(1, 4, 0x3FFF, 0x3FFF, 0x1800, 0x000F, 0x1FC0, 0x10000);
    h.set_param(creg(MVOLL, 0), 0x2000);
    h.set_param(creg(MVOLR, 0), 0x3000);
    h.set_param(creg(AVOLL, 1), 0x6000);
    h.set_switch(reg(KON, 0), 1u << 2);
    h.set_switch(reg(KON, 1), 1u << 4);
    struct Gates {
        u32 vmixl0, vmixr0, vmixl1, vmixr1, mmix1;
    };
    for (const Gates g : {Gates{4, 4, 16, 16, 0xFFC}, Gates{0, 4, 16, 0, 0xFFC}, Gates{4, 4, 16, 16, 0xFF0},
             Gates{4, 4, 16, 16, 0x0FC}, Gates{4, 4, 16, 16, 0xFF8}}) {
        const std::string key = std::format("cores: VMIXL/R {:#x}/{:#x} and {:#x}/{:#x}, MMIX 1 {:#x}", g.vmixl0,
            g.vmixr0, g.vmixl1, g.vmixr1, g.mmix1);
        h.set_switch(reg(VMIXL, 0), g.vmixl0);
        h.set_switch(reg(VMIXR, 0), g.vmixr0);
        h.set_switch(reg(VMIXL, 1), g.vmixl1);
        h.set_switch(reg(VMIXR, 1), g.vmixr1);
        h.set_param(creg(MMIX, 1), g.mmix1);
        h.generate(key, 700, true);
    }

    // ---- noise and pitch modulation ----
    h.fresh("noise");
    h.load(0x10000, vag::looped_sine(), "noise: VoiceTrans");
    h.voice(1, 0, 0x2000, 0x2000, 0x0C00, 0x000F, 0x1FC0, 0x10000);
    h.voice(1, 3, 0x2000, 0x2000, 0x0100, 0x000F, 0x1FC0, 0x10000);
    h.voice(1, 6, 0x2000, 0x2000, 0x0400, 0x000F, 0x1FC0, 0x10000);
    h.voice(1, 7, 0x2000, 0x2000, 0x1200, 0x000F, 0x1FC0, 0x10000);
    h.set_switch(reg(NON, 1), 1u << 3);
    h.set_switch(reg(PMON, 1), (1u << 7) | 1u);
    h.core_attr(kNoiseClock | 1, 0x2D);
    h.set_switch(reg(KON, 1), (1u << 3) | (1u << 6) | (1u << 7) | 1u);
    h.generate("noise and pitch modulation", 3000);

    // ---- a swept volume, an envelope through attack, decay and sustain ----
    h.fresh("sweep");
    h.load(0x10000, vag::looped_sine(), "sweep: VoiceTrans");
    h.voice(0, 11, 0x8010, 0x2000, 0x1000, 0x1F54, 0xC7C0, 0x10000);
    h.set_param(creg(MVOLL, 0), 0x3FFF);
    h.set_param(creg(MVOLR, 0), 0x3FFF);
    h.set_switch(reg(KON, 0), 1u << 11);
    h.o.group("the envelope and the sweep");
    for (u32 step = 0; step < 40; step++) {
        h.call("cellSoundSynth2Generate", {97, kOutL, kOutR, 0, 0});
        h.o.line(std::format("sweep: ENVX after {} samples", 97 * (step + 1)), h.param(vreg(ENVX, 0, 11)));
        h.o.line(std::format("sweep: VOLXL after {} samples", 97 * (step + 1)), h.param(vreg(VOLXL, 0, 11)));
    }
    h.o.group("");
    h.set_param(creg(MVOLL, 0), 0xE040);
    h.call("cellSoundSynth2Generate", {200, kOutL, kOutR, 0, 0});
    h.o.line("sweep: MVOLXL 0 after a decreasing sweep", h.param(creg(MVOLXL, 0)));

    // ---- the reverb: off, a studio, a hall, an echo; SetEffectMode ----
    const std::vector<u8> sound = vag::encode(vag::sine(672, 7, 22000), 6, 2, 3);
    struct Reverb {
        u16 mode;
        bool effect, dry;
        u16 delay, feedback;
        u32 bytes;
    };
    for (const Reverb rv : {Reverb{5, false, true, 0, 0, 0xADE0}, Reverb{2, true, false, 0, 0, 0x1F40},
             Reverb{5, true, true, 0, 0, 0xADE0}, Reverb{7, true, false, 10, 100, 0x18040}}) {
        const std::string key = std::format("reverb mode {}{}{}", rv.mode, rv.effect ? "" : " (effect off)",
            rv.dry ? " with the dry sound" : "");
        h.fresh(key);
        h.load(0x10000, sound, key + ": VoiceTrans");
        h.voice(1, 8, 0x3FFF, 0x3000, 0x0E00, 0x000F, 0x1FC0, 0x10000);
        h.set_switch(reg(VMIXL, 1), rv.dry ? 1u << 8 : 0);
        h.set_switch(reg(VMIXR, 1), rv.dry ? 1u << 8 : 0);
        h.set_switch(reg(VMIXEL, 1), 1u << 8);
        h.set_switch(reg(VMIXER, 1), 1u << 8);
        if (rv.effect) h.core_attr(kEffectEnable | 1, 1);
        h.effect_attr(static_cast<u16>(rv.mode | 0x100), 0x6000, 0x5000, rv.delay, rv.feedback);
        h.r("cellSoundSynth2SetEffectAttr", {1, kAttr}, key + ": SetEffectAttr");
        h.o.line(key + ": EVOLL", h.param(creg(EVOLL, 1)));
        h.o.line(key + ": ESA", h.addr(reg(ESA, 1)));
        h.set_switch(reg(KON, 1), 1u << 8);
        h.generate(key, 9600);
        h.r("cellSoundSynth2VoiceTrans", {1, 1, kRead, 0x200000 - rv.bytes, rv.bytes}, key + ": the work area read");
        std::vector<double> words(rv.bytes / 2);
        for (u32 w = 0; w < rv.bytes / 2; w++) words[w] = static_cast<s16>(ps_read16(b, kRead + 2 * w));
        h.o.numbers(key + ": the work area", std::move(words), 0.002);
    }
    h.effect_attr(3, 0x3333, 0x4444, 0, 0);
    h.set_param(creg(EVOLL, 0), 0x1111);
    h.r("cellSoundSynth2SetEffectMode", {0, kAttr}, "SetEffectMode(core 0, mode 3)");
    h.o.line("EVOLL 0 after SetEffectMode", h.param(creg(EVOLL, 0)));
    h.o.line("ESA 0 after SetEffectMode", h.addr(reg(ESA, 0)));
    for (const u16 mode : {u16{10}, u16{0x10A}, u16{0x200}, u16{0xFFFF}}) {
        h.effect_attr(mode, 1, 1, 0, 0);
        h.r("cellSoundSynth2SetEffectAttr", {1, kAttr}, std::format("SetEffectAttr(mode {:#x})", mode));
        h.r("cellSoundSynth2SetEffectMode", {1, kAttr}, std::format("SetEffectMode(mode {:#x})", mode));
    }
    h.r("cellSoundSynth2SetEffectAttr", {1, 0}, "SetEffectAttr(NULL)");
    h.effect_attr(1, 7, 8, 0, 0);
    h.r("cellSoundSynth2SetEffectAttr", {3, kAttr}, "SetEffectAttr(bus 3)");
    h.o.line("EVOLL 1 after SetEffectAttr(bus 3)", h.param(creg(EVOLL, 1)));

    // ---- Generate's arguments ----
    h.r("cellSoundSynth2Generate", {16, 0, kOutR, 0, 0}, "Generate without Lout");
    h.r("cellSoundSynth2Generate", {16, kOutL, 0, 0, 0}, "Generate without Rout");
    h.r("cellSoundSynth2Generate", {0, kOutL, kOutR, 0, 0}, "Generate(0)");
    h.r("cellSoundSynth2Exit", {}, "the last Exit");
    return std::move(h.o);
}

} // namespace

TEST_CASE("libsynth2's comparison calls Sony's exports by their names; pspocket's library, firmware first",
    "[hle][synth2]")
{
    check_names("libsynth2",
        {"cellSoundSynth2Config", "cellSoundSynth2Init", "cellSoundSynth2Exit", "cellSoundSynth2SetParam",
            "cellSoundSynth2GetParam", "cellSoundSynth2SetSwitch", "cellSoundSynth2GetSwitch", "cellSoundSynth2SetAddr",
            "cellSoundSynth2GetAddr", "cellSoundSynth2SetEffectAttr", "cellSoundSynth2SetEffectMode",
            "cellSoundSynth2SetCoreAttr", "cellSoundSynth2Generate", "cellSoundSynth2VoiceTrans",
            "cellSoundSynth2VoiceTransStatus", "cellSoundSynth2Note2Pitch", "cellSoundSynth2Pitch2Note"});
}

TEST_CASE("libsynth2 against Sony's libsynth2: registers, pitches, transfers, voices, mixes, reverb",
    "[.][hle][lle][compare][synth2]")
{
    const Setup s = setup();
    run(s, "libsynth2 against Sony's libsynth2", [&](bool sony) { return half(s, sony); },
        [](const Observed& o) {
            // pspocket's own half: test_synth2.cpp's answers.
            CHECK(o.value("Init(0)") == "0x0");
            CHECK(o.value("Init(0) again") == "0x80310203");
            CHECK(o.value("after Init: voice 0/7 parameter 0x2") == "0x3fff");
            CHECK(o.value("after Init: voice 1/23 address 0x20") == "0x005000");
            CHECK(o.value("after Init: core 0 parameter 0x8") == "0x0ff0");
            CHECK(o.value("after Init: core 1 parameter 0x8") == "0x0ffc");
            CHECK(o.value("after Init: core 1 parameter 0xd") == "0x7fff");
            CHECK(o.value("after Init: core 1 address 0x1d") == "0x1fffff");
            CHECK(o.value("after Init and 64 samples: core 1 switch 0x17") == "0xffffff");
            CHECK(o.value("sound memory at 0x5000 after Init") ==
                "hex:0707070707070707070707070707070700000000000000000000000000000000");
            CHECK(o.value("after the writes: voice 1/9 address 0x20") == "0x012340");
            CHECK(o.value("after the writes: core 0 address 0x1d") == "0x17ffff");
            CHECK(o.value("SetAddr(0x2013, 0x200000)") == "0x80310202");
            CHECK(o.value("Note2Pitch(60, 0, 60, -36)") == "0xfbe");
            CHECK(o.value("Pitch2Note(60, 0, 0x0)") == "0xac00");
            CHECK(o.value("VoiceTrans: write 0x3000 bytes at 0x10000") == "0x3000");
            CHECK(o.value("VoiceTransStatus(0, 1)") == "0x1");
            CHECK(o.value("VoiceTrans(mode 2, 0x10000, 0x10 bytes)") == "0x80310202");
            CHECK(o.value("looped sine: after 2000 samples: ENVX") == "0x7fff");
            CHECK(o.value("looped sine: after 2000 samples: NAX") == "0x010170");
            CHECK(o.value("one-shot: after 700 samples, voice 0: ENVX") == "0x0000");
            CHECK(o.value("reverb mode 2: ESA") == "0x1fe0c0");
            CHECK(o.value("EVOLL 0 after SetEffectMode") == "0x1111");
            CHECK(o.value("SetEffectAttr(mode 0xa)") == "0x80310202");
            CHECK(o.value("Generate without Lout") == "0x80310202");
            const Data* sine = o.find_data("looped sine: Lout");
            REQUIRE(sine);
            CHECK(sine->bytes.size() == 2000 * 4);
            CHECK(o.lines.size() > 2000);
        });
}
