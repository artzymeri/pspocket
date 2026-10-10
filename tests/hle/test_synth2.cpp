// libsynth2 (D-610 to D-614): the PS2's SPU2 in software behind libsd's interface. Synthetic VAG data encoded here
// (sines and silences, by vag_writer.hpp's closed-loop ADPCM encoder), voices keyed on and off with known pitch, ADSR
// and volumes, and every output sample checked against the test's own model of the SPU2 (written from psx-spx and
// libsd's documents, not from spu2.cpp): the blocks and their loop flags, the pitch counter and gaussian interpolation,
// the envelopes, the cores' mixes and routing, noise, pitch modulation, the reverb. Note2Pitch and Pitch2Note against
// values of libsd's arithmetic; the registers, the transfers, every argument check and error. No game data.

#include "hle_test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "pspocket/recomp/nid.hpp"
#include "spu2.hpp"
#include "vag_writer.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using pspocket::hle::test::vag::encode;
using pspocket::hle::test::vag::looped_sine;
using pspocket::hle::test::vag::looped_sine_pcm;
using pspocket::hle::test::vag::sine;

namespace {

const u64 kInvalid = r3_of_code(0x80310202), kAlready = r3_of_code(0x80310203);

// libsd's register numbers (ps2sdk libsd-common.h): the parameter in bits 8-15, SD_VOICE(core, voice) = core |
// voice << 1; the cores' volume parameters carry 0x80 as libsd's do.
enum : u32 {
    VOLL = 0x00, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLXL, VOLXR,
    MMIX = 0x08, MVOLL, MVOLR, EVOLL, EVOLR, AVOLL, AVOLR, BVOLL, BVOLR, MVOLXL, MVOLXR,
    PMON = 0x13, NON, KON, KOFF, ENDX, VMIXL, VMIXEL, VMIXR, VMIXER,
    ESA = 0x1C, EEA, TSA, IRQA, SSA, LSAX, NAX,
};
constexpr u16 vreg(u32 param, u32 core, u32 voice) { return static_cast<u16>(param << 8 | voice << 1 | core); }
constexpr u16 creg(u32 param, u32 core) { return static_cast<u16>(param << 8 | 0x80 | core); }
constexpr u16 reg(u32 param, u32 core) { return static_cast<u16>(param << 8 | core); }
// SetCoreAttr's entries (SD_CORE_*).
constexpr u16 kEffectEnable = 0x2, kIrqEnable = 0x4, kMuteEnable = 0x6, kNoiseClock = 0x8, kSpdifMode = 0xA;

// Guest buffers: the VAG data to transfer, the effect attributes, the outputs (16384 samples each).
constexpr u32 kSrc = kData, kAttr = kData + 0x7F000, kOutL = kData + 0x80000, kOutR = kData + 0x90000,
              kOutLs = kData + 0xA0000, kOutRs = kData + 0xB0000, kNotGuest = 0xF0000000u;
constexpr u32 kMaxSamples = 0x4000;

s32 clamp16(s64 v) { return static_cast<s32>(std::clamp<s64>(v, -0x8000, 0x7FFF)); }
s16 direct(u16 volume_reg) { return static_cast<s16>(static_cast<u16>(volume_reg << 1)); }

// ---- SPU-ADPCM (vag_writer.hpp encodes it) ----

// psx-spx "Sample Data (BRR/SPU-ADPCM)": one 16-byte block into 28 samples, the decoder's history carried along.
void decode_block(const u8* b, s32& p1, s32& p2, s32* out)
{
    s32 shift = b[0] & 15;
    if (shift > 12) shift = 9;
    s32 filter = (b[0] >> 4) & 7;
    if (filter > 4) filter = 0;
    for (s32 i = 0; i < 28; i++) {
        s32 nibble = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        if (nibble >= 8) nibble -= 16;
        const s32 s = clamp16(((nibble * 4096) >> shift) + ((p1 * vag::kF0[filter] + p2 * vag::kF1[filter] + 32) >> 6));
        p2 = p1;
        p1 = s;
        out[i] = s;
    }
}

// ---- the test's model of the SPU2 ----

// The noise generator (psx-spx "SPU Noise Generator").
struct ModelNoise {
    u16 level = 0;
    s32 timer = 0;
    void tick(u32 clock)
    {
        const u32 shift = clock >> 2, step = (clock & 3) + 4;
        timer -= static_cast<s32>(step);
        const u32 n = level;
        const u32 parity = ((n >> 15) ^ (n >> 12) ^ (n >> 11) ^ (n >> 10) ^ 1) & 1;
        if (timer < 0) {
            level = static_cast<u16>(n << 1 | parity);
            timer += static_cast<s32>(0x20000u >> shift);
        }
        if (timer < 0) timer += static_cast<s32>(0x20000u >> shift);
    }
};

// One voice: its blocks and loop flags, its pitch counter and interpolation, its ADSR phases (each step from
// Spu2::envelope_tick, checked against psx-spx's values on its own), its direct volumes.
struct ModelVoice {
    const std::vector<u8>* mem = nullptr;
    u16 voll = 0, volr = 0, pitch = 0x1000, adsr1 = 0x000F, adsr2 = 0x1FC0;
    u32 ssa = 0;
    bool pmon = false, noise = false;

    enum { Off, Attack, Decay, Sustain, Release } phase = Off;
    u32 addr = 0, loop = 0, pos = 0, frac = 0, env_counter = 0;
    u8 flags = 0;
    s32 env = 0, p1 = 0, p2 = 0, outx = 0;
    s32 blk[28] = {};
    std::array<s32, 4> hist{};
    bool endx = false, lsax_written = false;

    void fetch()
    {
        flags = (*mem)[addr + 1];
        if ((flags & 4) && !lsax_written) loop = addr; // a loop start, unless the game wrote the loop address
        if (phase != Off) decode_block(mem->data() + addr, p1, p2, blk);
    }
    void key_on()
    {
        addr = ssa;
        phase = Attack;
        env = 0;
        env_counter = 0;
        p1 = p2 = 0;
        pos = frac = 0;
        hist = {};
        endx = false;
        lsax_written = false;
        fetch();
        hist[3] = blk[0];
    }
    void key_off()
    {
        if (phase != Off) {
            phase = Release;
            env_counter = 0;
        }
    }
    s32 sustain_level() const { return ((adsr1 & 15) + 1) * 0x800; }
    void envelope()
    {
        const auto step = [&](u32 rate, u32 mask, bool dec, bool exp) {
            env = spu2::Spu2::envelope_tick(env, rate, mask, dec, exp, false, env_counter);
        };
        switch (phase) {
        case Off: break;
        case Attack:
            step((adsr1 >> 8) & 0x7F, 0x7F, false, adsr1 & 0x8000);
            if (env >= 0x7FFF) {
                phase = env <= sustain_level() ? Sustain : Decay;
                env_counter = 0;
            }
            break;
        case Decay:
            step(((adsr1 >> 4) & 15) << 2, 0x7C, true, true);
            if (env <= sustain_level()) {
                phase = Sustain;
                env_counter = 0;
            }
            break;
        case Sustain: step((adsr2 >> 6) & 0x7F, 0x7F, adsr2 & 0x4000, adsr2 & 0x8000); break;
        case Release:
            step((adsr2 & 31) << 2, 0x7C, true, adsr2 & 0x20);
            if (env <= 0) {
                env = 0;
                phase = Off;
            }
            break;
        }
    }
    // One 48 kHz sample: the envelope, the output (interpolated, or the noise), then the pitch counter; `mod` is the
    // previous voice's output.
    s32 tick(s32 mod, u16 noise_level)
    {
        envelope();
        s32 out = 0;
        if (phase != Off) {
            s32 s = static_cast<s16>(noise_level);
            if (!noise) {
                const auto& g = spu2::Spu2::gauss();
                const u32 i = (frac >> 4) & 0xFF;
                s = clamp16(((g[0xFF - i] * hist[0]) >> 15) + ((g[0x1FF - i] * hist[1]) >> 15) +
                    ((g[0x100 + i] * hist[2]) >> 15) + ((g[i] * hist[3]) >> 15));
            }
            out = (s * env) >> 15;
        }
        outx = out;
        u32 step = pitch;
        if (pmon) step = static_cast<u32>((s32{static_cast<s16>(pitch)} * (mod + 0x8000)) >> 15) & 0xFFFF;
        frac += std::min<u32>(step, 0x4000);
        for (; frac >= 0x1000; frac -= 0x1000) {
            if (++pos == 28) {
                pos = 0;
                if (flags & 1) {
                    endx = true;
                    addr = loop;
                    if (!(flags & 2)) {
                        phase = Off;
                        env = 0;
                    }
                } else {
                    addr += 16;
                }
                fetch();
            }
            hist = {hist[1], hist[2], hist[3], phase != Off ? blk[pos] : 0};
        }
        return out;
    }
    s32 left() const { return (outx * direct(voll)) >> 15; }
    s32 right() const { return (outx * direct(volr)) >> 15; }
};

// The reverb of one core (psx-spx "SPU Reverb Formula" and "Reverb Buffer Resampling"), over its own copy of the work
// area, in the order D-612 sets out.
struct ModelReverb {
    std::array<u16, 32> g{};
    s32 evoll = 0, evolr = 0;
    u32 start = 0, end = 0, addr = 0;
    std::vector<s32> area;
    std::array<s32, 64> in_l{}, in_r{}, up_l{}, up_r{};
    u32 pos = 0;
    bool step = false;

    static constexpr s32 kFir[39] = {-1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246,
        16384, 10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};

    void setup(u32 esa_bytes, u32 eea_bytes)
    {
        start = esa_bytes >> 1;
        end = eea_bytes >> 1;
        addr = start;
        area.assign(end - start + 1, 0);
    }
    static s32 fir(const std::array<s32, 64>& ring, u32 at)
    {
        s32 sum = 0;
        for (u32 k = 0; k < 39; k++) sum += kFir[k] * ring[(at - k) & 63];
        return sum;
    }
    s32& cell(s64 offset)
    {
        const s64 len = s64{end} - start + 1;
        s64 rel = (s64{addr} - start + offset) % len;
        if (rel < 0) rel += len;
        return area[static_cast<usize>(rel)];
    }
    s64 vol(u32 r) const { return static_cast<s16>(g[r]); }
    s64 off(u32 r) const { return s64{g[r]} * 4; }
    void compute(s64 in_l_, s64 in_r_, s32& out_l, s32& out_r)
    {
        using namespace spu2;
        const s64 lin = (vol(vLIN) * in_l_) >> 15, rin = (vol(vRIN) * in_r_) >> 15;
        const s64 lsp = cell(off(mLSAME) - 1), rsp = cell(off(mRSAME) - 1), ldp = cell(off(mLDIFF) - 1),
                  rdp = cell(off(mRDIFF) - 1);
        const s64 ls = (((lin + ((cell(off(dLSAME)) * vol(vWALL)) >> 15) - lsp) * vol(vIIR)) >> 15) + lsp;
        const s64 rs = (((rin + ((cell(off(dRSAME)) * vol(vWALL)) >> 15) - rsp) * vol(vIIR)) >> 15) + rsp;
        const s64 ld = (((lin + ((cell(off(dRDIFF)) * vol(vWALL)) >> 15) - ldp) * vol(vIIR)) >> 15) + ldp;
        const s64 rd = (((rin + ((cell(off(dLDIFF)) * vol(vWALL)) >> 15) - rdp) * vol(vIIR)) >> 15) + rdp;
        cell(off(mLSAME)) = clamp16(ls);
        cell(off(mRSAME)) = clamp16(rs);
        cell(off(mLDIFF)) = clamp16(ld);
        cell(off(mRDIFF)) = clamp16(rd);
        s64 l = clamp16((vol(vCOMB1) * cell(off(mLCOMB1)) + vol(vCOMB2) * cell(off(mLCOMB2)) +
                            vol(vCOMB3) * cell(off(mLCOMB3)) + vol(vCOMB4) * cell(off(mLCOMB4))) >> 15);
        s64 r = clamp16((vol(vCOMB1) * cell(off(mRCOMB1)) + vol(vCOMB2) * cell(off(mRCOMB2)) +
                            vol(vCOMB3) * cell(off(mRCOMB3)) + vol(vCOMB4) * cell(off(mRCOMB4))) >> 15);
        const auto apf = [&](s64 x, u32 m, u32 d, s64 v) {
            const s64 delayed = cell(off(m) - off(d));
            const s64 y = clamp16(x - ((v * delayed) >> 15));
            cell(off(m)) = static_cast<s32>(y);
            return s64{clamp16(((y * v) >> 15) + delayed)};
        };
        l = apf(l, mLAPF1, dAPF1, vol(vAPF1));
        r = apf(r, mRAPF1, dAPF1, vol(vAPF1));
        l = apf(l, mLAPF2, dAPF2, vol(vAPF2));
        r = apf(r, mRAPF2, dAPF2, vol(vAPF2));
        out_l = clamp16((l * evoll) >> 15);
        out_r = clamp16((r * evolr) >> 15);
    }
    // One 48 kHz sample of the wet mix in, the reverb's output out.
    void tick(s32 wl, s32 wr, bool enabled, s32& out_l, s32& out_r)
    {
        pos = (pos + 1) & 63;
        in_l[pos] = wl;
        in_r[pos] = wr;
        s32 yl = 0, yr = 0;
        if (step) {
            if (enabled) compute(clamp16(fir(in_l, pos) >> 15), clamp16(fir(in_r, pos) >> 15), yl, yr);
            addr = addr == end ? start : addr + 1;
        }
        step = !step;
        up_l[pos] = yl;
        up_r[pos] = yr;
        out_l = clamp16(fir(up_l, pos) >> 14);
        out_r = clamp16(fir(up_r, pos) >> 14);
    }
};

// ---- the library through its exports ----

struct Synth {
    Fixture f;
    GuestThread g{f, 0, "game"};
    std::vector<u8> mem = std::vector<u8>(spu2::kMemorySize); // the test's copy of what it put in sound memory

    template <class... A>
    u64 call(const char* fn, A... args)
    {
        return g.call(fn, args...);
    }
    void init(u32 flag = 0) { REQUIRE(call("cellSoundSynth2Init", flag) == 0); }
    void param(u16 r, u16 v) { call("cellSoundSynth2SetParam", r, v); }
    u16 get_param(u16 r) { return static_cast<u16>(call("cellSoundSynth2GetParam", r)); }
    void sw(u16 r, u32 v) { call("cellSoundSynth2SetSwitch", r, v); }
    u32 get_sw(u16 r) { return static_cast<u32>(call("cellSoundSynth2GetSwitch", r)); }
    u64 addr(u16 r, u32 v) { return call("cellSoundSynth2SetAddr", r, v); }
    u32 get_addr(u16 r) { return static_cast<u32>(call("cellSoundSynth2GetAddr", r)); }
    void core_attr(u16 entry, u16 v) { call("cellSoundSynth2SetCoreAttr", entry, v); }

    // Puts `data` at `s_addr` in sound memory through VoiceTrans (channel 0, a DMA write).
    void load(u32 s_addr, const std::vector<u8>& data)
    {
        u8* b = f.base();
        std::memcpy(b + kSrc, data.data(), data.size());
        REQUIRE(call("cellSoundSynth2VoiceTrans", 0, 0, kSrc, s_addr, static_cast<u32>(data.size())) == data.size());
        std::memcpy(mem.data() + s_addr, data.data(), data.size());
    }
    // A voice's registers as `v` has them.
    void setup(u32 core, u32 voice, const ModelVoice& v)
    {
        param(vreg(VOLL, core, voice), v.voll);
        param(vreg(VOLR, core, voice), v.volr);
        param(vreg(PITCH, core, voice), v.pitch);
        param(vreg(ADSR1, core, voice), v.adsr1);
        param(vreg(ADSR2, core, voice), v.adsr2);
        REQUIRE(addr(vreg(SSA, core, voice), v.ssa) == 0);
    }
    // The next n samples, left and right.
    std::pair<std::vector<float>, std::vector<float>> generate(u32 n)
    {
        REQUIRE(n <= kMaxSamples);
        REQUIRE(call("cellSoundSynth2Generate", n, kOutL, kOutR, 0, 0) == 0);
        std::vector<float> l(n), r(n);
        for (u32 i = 0; i < n; i++) {
            l[i] = ps_readf32(f.base(), kOutL + 4 * i);
            r[i] = ps_readf32(f.base(), kOutR + 4 * i);
        }
        return {l, r};
    }
};

float out(s32 sample) { return static_cast<float>(sample) / 32768.0f; }

// The first index where `got` and `want` differ, or -1.
long first_difference(const std::vector<float>& got, const std::vector<float>& want)
{
    for (usize i = 0; i < std::min(got.size(), want.size()); i++)
        if (got[i] != want[i]) return static_cast<long>(i);
    return got.size() == want.size() ? -1 : static_cast<long>(std::min(got.size(), want.size()));
}

#define CHECK_SAMPLES(got, want)                                                                                       \
    do {                                                                                                               \
        const long at_ = first_difference(got, want);                                                                  \
        INFO("first difference at sample " << at_ << ": "                                                             \
             << (at_ >= 0 && at_ < long(got.size()) ? got[at_] * 32768.0f : 0.0f) << " where the model has "           \
             << (at_ >= 0 && at_ < long(want.size()) ? want[at_] * 32768.0f : 0.0f));                                  \
        CHECK(at_ == -1);                                                                                              \
    } while (0)

} // namespace

TEST_CASE("libsynth2: the library's 17 functions, firmware-first", "[hle][synth2]")
{
    const char* names[] = {"cellSoundSynth2Config", "cellSoundSynth2Init", "cellSoundSynth2Exit",
        "cellSoundSynth2SetParam", "cellSoundSynth2GetParam", "cellSoundSynth2SetSwitch", "cellSoundSynth2GetSwitch",
        "cellSoundSynth2SetAddr", "cellSoundSynth2GetAddr", "cellSoundSynth2SetEffectAttr",
        "cellSoundSynth2SetEffectMode", "cellSoundSynth2SetCoreAttr", "cellSoundSynth2Generate",
        "cellSoundSynth2VoiceTrans", "cellSoundSynth2VoiceTransStatus", "cellSoundSynth2Note2Pitch",
        "cellSoundSynth2Pitch2Note"};
    for (const char* name : names) {
        INFO(name);
        const Function* fn = registry().find("libsynth2", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
        CHECK_FALSE(fn->unverified);
    }
    CHECK(registry().is_firmware_first("libsynth2"));
}

TEST_CASE("libsynth2: Note2Pitch and Pitch2Note, libsd's arithmetic", "[hle][synth2]")
{
    Synth s;
    // Values of libsd's own arithmetic (its tables, its rounding, its ceiling two octaves up).
    struct N2P {
        u32 center_note, center_fine, note;
        s32 fine;
        u32 pitch;
    };
    const N2P n2p[] = {{60, 0, 60, 0, 0x1000}, {60, 0, 72, 0, 0x2000}, {60, 0, 48, 0, 0x800}, {60, 0, 36, 0, 0x400},
        {60, 0, 84, 0, 0x4000}, {60, 0, 96, 0, 0x4000}, {60, 0, 61, 0, 0x10F4}, {60, 0, 59, 0, 0xF1A},
        {60, 0, 60, 64, 0x1078}, {60, 0, 60, -64, 0xF8B}, {60, 0, 60, -128, 0xF1A}, {60, 0, 60, 127, 0x10F2},
        {60, 32, 60, 0, 0x103C}, {64, 100, 70, -30, 0x175A}, {48, 0, 0, 0, 0x100}, {0, 0, 127, 0, 0x5FE4},
        {60, 127, 61, 127, 0x1302}, {72, 50, 30, -100, 0x162}, {60, 0, 12, 0, 0x100}, {60, 0, 1, 0, 0x88},
        {45, 64, 45, 64, 0x10F4}};
    for (const N2P& c : n2p) {
        INFO(c.center_note << "/" << c.center_fine << " note " << c.note << " fine " << c.fine);
        CHECK(s.call("cellSoundSynth2Note2Pitch", c.center_note, c.center_fine, c.note, c.fine) == c.pitch);
    }
    struct P2N {
        u32 center_note, center_fine, pitch, note;
    };
    const P2N p2n[] = {{60, 0, 0x1000, 0x3C00}, {60, 0, 0x2000, 0x4800}, {60, 0, 0x800, 0x3000},
        {60, 0, 0x10F4, 0x3D00}, {60, 0, 0xF1A, 0x3B00}, {60, 0, 0x1078, 0x3C40}, {60, 0, 0x3FFF, 0x5400},
        {60, 0, 0x4000, 0x5400}, {60, 0, 0xFFFF, 0x5400}, {60, 0, 1, 0xAC00}, {60, 0, 0, 0xAC00},
        {60, 64, 0x1000, 0x3C40}, {48, 127, 0x1234, 0x331C}, {69, 10, 0xABC, 0x3E16}};
    for (const P2N& c : p2n) {
        INFO(c.center_note << "/" << c.center_fine << " pitch " << c.pitch);
        CHECK(s.call("cellSoundSynth2Pitch2Note", c.center_note, c.center_fine, c.pitch) == c.note);
    }
    // Every note and fine step within the pitch's range: near 4096 * 2^((note + fine / 128 - centre) / 12) (the
    // tables round down and each octave down halves with rounding), rising with the note, and back through
    // Pitch2Note within a few fine steps (its fine is even and the low pitches have few bits).
    u32 last = 0;
    for (s32 note = 12; note < 84; note++)
        for (s32 fine = 0; fine < 128; fine++) {
            const u32 p = static_cast<u32>(s.call("cellSoundSynth2Note2Pitch", 60, 0, note, fine));
            const double exact = 4096.0 * std::exp2((note - 60 + fine / 128.0) / 12.0);
            INFO("note " << note << " fine " << fine << ": " << p << " for " << exact);
            CHECK(std::abs(p - exact) <= 1.0 + exact * 0.0006);
            CHECK(p >= last);
            last = p;
            const u32 back = static_cast<u32>(s.call("cellSoundSynth2Pitch2Note", 60, 0, p));
            const s32 steps = static_cast<s32>((back >> 8) * 128 + (back & 0xFF)) - (note * 128 + fine);
            CHECK(std::abs(steps) <= 5);
            if (note >= 60) CHECK(std::abs(steps) <= 1);
        }
}

TEST_CASE("libsynth2: Init, Exit, Config and the state libsd's initialisation leaves", "[hle][synth2]")
{
    Synth s;
    CHECK(s.call("cellSoundSynth2Exit") == 0); // nothing to end
    CHECK(s.call("cellSoundSynth2Config", 1, 0x1234) == 0);
    s.init();
    CHECK(s.call("cellSoundSynth2Init", 0) == kAlready);
    CHECK(s.call("cellSoundSynth2Init", 1) == kAlready);
    CHECK(s.call("cellSoundSynth2Config", -3, -1) == 0);
    for (u32 core = 0; core < 2; core++) {
        INFO("core " << core);
        for (const u32 voice : {0u, 7u, 23u}) {
            CHECK(s.get_param(vreg(PITCH, core, voice)) == 0x3FFF);
            CHECK(s.get_param(vreg(VOLL, core, voice)) == 0);
            CHECK(s.get_param(vreg(ADSR1, core, voice)) == 0);
            CHECK(s.get_param(vreg(ENVX, core, voice)) == 0);
            CHECK(s.get_addr(vreg(SSA, core, voice)) == 0x5000);
            CHECK(s.get_addr(vreg(LSAX, core, voice)) == 0x5000); // the block's loop start flag
            CHECK(s.get_addr(vreg(NAX, core, voice)) == 0x5000);
        }
        CHECK(s.get_param(reg(MMIX, core)) == (core == 0 ? 0xFF0 : 0xFFC));
        CHECK(s.get_param(creg(AVOLL, core)) == (core == 0 ? 0 : 0x7FFF));
        CHECK(s.get_param(creg(AVOLR, core)) == (core == 0 ? 0 : 0x7FFF));
        CHECK(s.get_param(creg(MVOLL, core)) == 0);
        CHECK(s.get_param(creg(EVOLR, core)) == 0);
        CHECK(s.get_param(creg(BVOLL, core)) == 0);
        for (const u32 sw : {VMIXL, VMIXEL, VMIXR, VMIXER}) CHECK(s.get_sw(reg(sw, core)) == 0xFFFFFF);
        CHECK(s.get_sw(reg(PMON, core)) == 0);
        CHECK(s.get_sw(reg(NON, core)) == 0);
        CHECK(s.get_sw(reg(KON, core)) == 0xFFFFFF);  // libsd keys every voice on
        CHECK(s.get_sw(reg(KOFF, core)) == 0xFFFFFF); // and off
        CHECK(s.get_sw(reg(ENDX, core)) == 0);
        const u32 eea = core == 0 ? 0x1DFFFF : 0x1FFFFF;
        CHECK(s.get_addr(reg(EEA, core)) == eea);
        CHECK(s.get_addr(reg(ESA, core)) == eea + 1 - 0x10); // the "off" mode's 16 bytes
    }
    // libsd's block at 0x5000 loops (flags 7) and the next is zeros.
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 1, kSrc, 0x5000, 32) == 32);
    for (u32 i = 0; i < 32; i++) CHECK(ps_read8(s.f.base(), kSrc + i) == (i < 16 ? 7 : 0));
    // The stopped voices go on through it at pitch 0x3FFF: within eight samples every one has met its loop end.
    s.generate(8);
    CHECK(s.get_sw(reg(ENDX, 0)) == 0xFFFFFF);
    CHECK(s.get_sw(reg(ENDX, 1)) == 0xFFFFFF);
    for (float v : s.generate(64).first) CHECK(v == 0.0f);

    // Exit, then a hot start keeps the volumes a cold one clears.
    s.param(creg(MVOLL, 1), 0x1234);
    s.param(creg(EVOLL, 1), 0x2345);
    CHECK(s.call("cellSoundSynth2Exit") == 0);
    CHECK(s.call("cellSoundSynth2Init", 1) == 0);
    CHECK(s.get_param(creg(MVOLL, 1)) == 0x1234);
    CHECK(s.get_param(creg(EVOLL, 1)) == 0x2345);
    CHECK(s.get_param(vreg(PITCH, 1, 3)) == 0x3FFF);
    CHECK(s.call("cellSoundSynth2Exit") == 0);
    CHECK(s.call("cellSoundSynth2Init", 2) == 0); // bit 0 decides, as libsd: a cold start
    CHECK(s.get_param(creg(MVOLL, 1)) == 0);
    CHECK(s.get_param(creg(EVOLL, 1)) == 0);
}

TEST_CASE("libsynth2: VoiceTrans into sound memory and back, its status", "[hle][synth2]")
{
    Synth s;
    s.init();
    u8* b = s.f.base();
    for (u32 i = 0; i < 0x3000; i++) ps_write8(b, kSrc + i, static_cast<u8>(i * 7 + 3));
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 0, kSrc, 0x10000, 0x3000) == 0x3000);
    CHECK(s.get_addr(reg(TSA, 0)) == 0x10000);
    CHECK(s.call("cellSoundSynth2VoiceTransStatus", 0, 0) == 1);
    CHECK(s.call("cellSoundSynth2VoiceTransStatus", 0, 1) == 1);
    // Read back through the other channel, in I/O mode, at an odd length.
    CHECK(s.call("cellSoundSynth2VoiceTrans", 1, 1 | 8, kSrc + 0x4000, 0x10000, 0x2FFF) == 0x2FFF);
    CHECK(s.get_addr(reg(TSA, 1)) == 0x10000);
    CHECK(std::memcmp(b + kSrc, b + kSrc + 0x4000, 0x2FFF) == 0);
    CHECK(ps_read8(b, kSrc + 0x4000 + 0x2FFF) == 0); // one byte short
    CHECK(s.call("cellSoundSynth2VoiceTransStatus", 1, 0) == 1);
    // The last bytes of sound memory, and nothing.
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 0, kSrc, 0x1FFFF0, 16) == 16);
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 0, 0, 0x1000, 0) == 0);

    // Errors: STOP and the fourth mode are no transfers; past sound memory; main memory that is not the guest's.
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 2, kSrc, 0x10000, 16) == kInvalid);
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 3, kSrc, 0x10000, 16) == kInvalid);
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 0, kSrc, 0x1FFFF0, 17) == kInvalid);
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 0, kSrc, 0x200000, 0) == kInvalid);
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 0, kSrc, 0x10, 0xFFFFFFF0u) == kInvalid);
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 1, kNotGuest, 0x10000, 16) == kInvalid);
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 0, 0, 0x10000, 16) == kInvalid);
    // Nothing of the refused calls reached sound memory.
    CHECK(s.call("cellSoundSynth2VoiceTrans", 0, 1, kSrc + 0x8000, 0x10000, 16) == 16);
    CHECK(std::memcmp(b + kSrc, b + kSrc + 0x8000, 16) == 0);
}

TEST_CASE("libsynth2: the registers, set and read back as libsd stores them", "[hle][synth2]")
{
    Synth s;
    s.init();
    // A voice's parameters (raw), ENVX and the current volumes as written.
    for (const u32 p : {VOLL, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLXL, VOLXR}) {
        s.param(vreg(p, 1, 17), static_cast<u16>(0x1230 + p));
        CHECK(s.get_param(vreg(p, 1, 17)) == 0x1230 + p);
        CHECK(s.get_param(vreg(p, 0, 17)) != 0x1230 + p);
    }
    // A core's: MMIX and the volumes (the 0x80 bit or not).
    for (const u32 p : {MMIX, MVOLL, MVOLR, EVOLL, EVOLR, AVOLL, AVOLR, BVOLL, BVOLR, MVOLXL, MVOLXR}) {
        s.param(creg(p, 0), static_cast<u16>(0x8100 + p));
        CHECK(s.get_param(reg(p, 0)) == 0x8100 + p);
    }
    // Voices 24 to 31 and the parameters past 0x12 are not there.
    s.param(vreg(PITCH, 0, 24), 0x1111);
    CHECK(s.get_param(vreg(PITCH, 0, 24)) == 0);
    s.param(reg(0x13, 0), 0x2222);
    CHECK(s.get_param(reg(0x13, 0)) == 0);
    CHECK(s.get_param(reg(0x40, 1)) == 0);

    // Switches: 24 voice bits.
    for (const u32 p : {PMON, NON, VMIXL, VMIXEL, VMIXR, VMIXER}) {
        s.sw(reg(p, 0), 0xFF123456);
        CHECK(s.get_sw(reg(p, 0)) == 0x123456);
        s.sw(reg(p, 0), 0);
        CHECK(s.get_sw(reg(p, 0)) == 0);
    }
    // ENDX: every stopped voice meets its loop end within eight samples after Init; a write clears it, whatever the
    // value.
    s.generate(8);
    CHECK(s.get_sw(reg(ENDX, 0)) == 0xFFFFFF);
    s.sw(reg(ENDX, 0), 0x000001);
    CHECK(s.get_sw(reg(ENDX, 0)) == 0);
    CHECK(s.get_sw(reg(ENDX, 1)) == (0xFFFFFFu & ~(1u << 17))); // core 1's voice 17 got a slower pitch above
    s.sw(reg(KON, 1), 0x800001);
    CHECK(s.get_sw(reg(KON, 1)) == 0x800001);
    s.sw(reg(KOFF, 1), 0x1000000);
    CHECK(s.get_sw(reg(KOFF, 1)) == 0);
    CHECK(s.get_sw(reg(0x12, 1)) == 0);
    s.sw(reg(0x1C, 1), 5); // an address register through SetSwitch: nothing
    CHECK(s.get_addr(reg(ESA, 1)) == 0x1FFFF0);

    // Addresses: 16-byte blocks; EEA its 128 KB boundary with the low 17 bits set.
    CHECK(s.addr(vreg(SSA, 1, 9), 0x12345) == 0);
    CHECK(s.get_addr(vreg(SSA, 1, 9)) == 0x12340);
    CHECK(s.addr(vreg(LSAX, 1, 9), 0x2000F) == 0);
    CHECK(s.get_addr(vreg(LSAX, 1, 9)) == 0x20000);
    CHECK(s.addr(vreg(NAX, 1, 9), 0x30010) == 0);
    CHECK(s.get_addr(vreg(NAX, 1, 9)) == 0x30010);
    CHECK(s.addr(reg(TSA, 0), 0x777) == 0);
    CHECK(s.get_addr(reg(TSA, 0)) == 0x770);
    CHECK(s.addr(reg(IRQA, 0), 0x1FFFFF) == 0);
    CHECK(s.get_addr(reg(IRQA, 0)) == 0x1FFFF0);
    CHECK(s.addr(reg(ESA, 0), 0x150008) == 0);
    CHECK(s.get_addr(reg(ESA, 0)) == 0x150000);
    CHECK(s.addr(reg(EEA, 0), 0x17ABCD) == 0);
    CHECK(s.get_addr(reg(EEA, 0)) == 0x17FFFF);
    // Errors: past sound memory, a voice that is not there, an unknown register; nothing set.
    CHECK(s.addr(vreg(SSA, 1, 9), 0x200000) == kInvalid);
    CHECK(s.get_addr(vreg(SSA, 1, 9)) == 0x12340);
    CHECK(s.addr(vreg(SSA, 1, 24), 0x1000) == kInvalid);
    CHECK(s.addr(reg(0x1B, 0), 0x1000) == kInvalid);
    CHECK(s.addr(reg(0x23, 0), 0x1000) == kInvalid);
    CHECK(s.get_addr(vreg(SSA, 1, 30)) == 0);
    CHECK(s.get_addr(reg(0x05, 1)) == 0);

    // SetCoreAttr: the noise clock's six bits; an unknown entry changes nothing.
    s.core_attr(kNoiseClock | 1, 0xFF);
    s.core_attr(kSpdifMode | 1, 0x0102);
    s.core_attr(kMuteEnable | 1, 1);
    s.core_attr(0xC | 1, 1);
    s.core_attr(0x0, 1);
}

TEST_CASE("libsynth2: a looped sine at pitch 0x1000, sample for sample", "[hle][synth2]")
{
    Synth s;
    s.init();
    const std::vector<u8> vag = looped_sine();
    s.load(0x10000, vag);
    ModelVoice v;
    v.mem = &s.mem;
    v.ssa = 0x10000;
    v.voll = 0x3FFF;
    v.volr = 0x2000;
    v.pitch = 0x1000;
    v.adsr1 = 0x000F; // attack linear at the fastest, sustain level 15
    v.adsr2 = 0x1FC0; // sustain that never steps, release at the fastest
    s.setup(1, 5, v);
    s.param(creg(MVOLL, 1), 0x3FFF);
    s.param(creg(MVOLR, 1), 0x3FFF);
    s.sw(reg(KON, 1), 1u << 5);
    CHECK(s.get_sw(reg(ENDX, 1)) == 0); // nothing rendered since Init
    v.key_on();

    const u32 n = 2000;
    auto [l, r] = s.generate(n);
    std::vector<float> want_l(n), want_r(n);
    for (u32 i = 0; i < n; i++) {
        v.tick(0, 0);
        want_l[i] = out(clamp16((s64{clamp16(v.left())} * direct(0x3FFF)) >> 15));
        want_r[i] = out(clamp16((s64{clamp16(v.right())} * direct(0x3FFF)) >> 15));
    }
    CHECK_SAMPLES(l, want_l);
    CHECK_SAMPLES(r, want_r);
    // The decoded sine itself: close to what was encoded, two samples late (the interpolation's delay).
    const std::vector<s16> pcm = looped_sine_pcm();
    double err = 0;
    for (u32 i = 710; i < 1300; i++) {
        const double got = double(l[i]) * 32768.0 / (0x7FFE / 32768.0) / (0x7FFE / 32768.0);
        err = std::max(err, std::abs(got - pcm[(i - 2) % 672]));
    }
    CHECK(err < 900); // the gaussian filter and the ADPCM, under 5% of the amplitude
    // The loop came round: ENDX, the loop address (the first block's loop start), the envelope at its peak.
    CHECK((s.get_sw(reg(ENDX, 1)) >> 5 & 1) == 1);
    CHECK(s.get_addr(vreg(LSAX, 1, 5)) == 0x10000);
    CHECK(s.get_param(vreg(ENVX, 1, 5)) == 0x7FFF);
    CHECK(s.get_param(vreg(VOLXL, 1, 5)) == 0x7FFE);
    CHECK(s.get_param(vreg(VOLXR, 1, 5)) == 0x4000);
    CHECK(s.get_addr(vreg(NAX, 1, 5)) == 0x10000 + 16 * ((n % 672) / 28));
    // Another 2000 samples go on from where these ended.
    auto [l2, r2] = s.generate(n);
    for (u32 i = 0; i < n; i++) {
        v.tick(0, 0);
        want_l[i] = out(clamp16((s64{clamp16(v.left())} * direct(0x3FFF)) >> 15));
    }
    CHECK_SAMPLES(l2, want_l);
}

TEST_CASE("libsynth2: half and odd pitches, decay and release, a one-shot's end", "[hle][synth2]")
{
    Synth s;
    s.init();
    // A one-shot: 10 blocks of a sine, the last with loop end and no repeat (end and mute: it goes on, silent, at its
    // loop address, libsd's block at 0x5000 since Init).
    std::vector<s16> pcm = sine(280, 5, 24000);
    std::vector<u8> vag = encode(pcm, 0, 0, 1);
    s.load(0x20000, vag);
    ModelVoice a;
    a.mem = &s.mem;
    a.ssa = 0x20000;
    a.voll = a.volr = 0x3000;
    a.pitch = 0x0800; // half speed: the interpolation at 0 and 0x80
    a.adsr1 = 0x8A75; // exponential attack (rate 0x0A), decay shift 7, sustain level 5
    a.adsr2 = 0x4D4A; // sustain linear decreasing (rate 0x35), release linear (shift 10)
    s.setup(1, 0, a);
    // A looped sine at an odd pitch on voice 1, keyed off part way.
    s.load(0x30000, looped_sine());
    ModelVoice b;
    b.mem = &s.mem;
    b.ssa = 0x30000;
    b.voll = 0x2000;
    b.volr = 0x7000; // negative: phase inverted
    b.pitch = 0x1ABC;
    b.adsr1 = 0x1F3A;  // linear attack (rate 0x1F), decay shift 3, sustain level 10
    b.adsr2 = 0x8FE7;  // exponential sustain increasing (rate 0x3F), exponential release (shift 7)
    s.setup(1, 1, b);
    s.param(creg(MVOLL, 1), 0x3FFF);
    s.param(creg(MVOLR, 1), 0x2FFF);
    s.sw(reg(KON, 1), 3);
    a.key_on();
    b.key_on();

    std::vector<float> got_l, got_r, want_l, want_r;
    const auto run = [&](u32 n) {
        auto [l, r] = s.generate(n);
        got_l.insert(got_l.end(), l.begin(), l.end());
        got_r.insert(got_r.end(), r.begin(), r.end());
        for (u32 i = 0; i < n; i++) {
            a.tick(0, 0);
            b.tick(a.outx, 0);
            want_l.push_back(out(clamp16((s64{clamp16(a.left() + b.left())} * direct(0x3FFF)) >> 15)));
            want_r.push_back(out(clamp16((s64{clamp16(a.right() + b.right())} * direct(0x2FFF)) >> 15)));
        }
    };
    run(300);
    CHECK(s.get_param(vreg(ENVX, 1, 0)) == static_cast<u16>(a.env));
    CHECK(s.get_param(vreg(ENVX, 1, 1)) == static_cast<u16>(b.env));
    s.sw(reg(KOFF, 1), 2); // voice 1 to release
    b.key_off();
    run(400);
    // Voice 0 met its end (560 samples at half speed): ENDX, silence, the envelope at zero; voice 1 went round its
    // loop in release.
    CHECK(a.endx);
    CHECK(b.endx);
    CHECK((s.get_sw(reg(ENDX, 1)) & 3) == 3);
    CHECK(s.get_param(vreg(ENVX, 1, 0)) == 0);
    CHECK(a.phase == ModelVoice::Off);
    run(600);
    CHECK_SAMPLES(got_l, want_l);
    CHECK_SAMPLES(got_r, want_r);
    // Something played on both sides before the end.
    CHECK(std::count_if(got_l.begin(), got_l.end(), [](float x) { return x != 0.0f; }) > 500);
    CHECK(s.get_param(vreg(ENVX, 1, 1)) == static_cast<u16>(b.env));
    // A key on again starts over: ENDX clear, attack from zero.
    s.sw(reg(KON, 1), 1);
    CHECK((s.get_sw(reg(ENDX, 1)) & 1) == 0);
    CHECK(s.get_param(vreg(ENVX, 1, 0)) == 0);
    CHECK(s.get_addr(vreg(NAX, 1, 0)) == 0x20000);
}

TEST_CASE("libsynth2: core 0 through core 1, the voice and core mixes", "[hle][synth2]")
{
    Synth s;
    s.init();
    s.load(0x10000, looped_sine());
    ModelVoice v0, v1;
    for (ModelVoice* v : {&v0, &v1}) {
        v->mem = &s.mem;
        v->ssa = 0x10000;
        v->voll = 0x3FFF;
        v->volr = 0x3FFF;
    }
    v1.pitch = 0x1800;
    s.setup(0, 2, v0); // core 0, voice 2
    s.setup(1, 4, v1); // core 1, voice 4
    s.param(creg(MVOLL, 0), 0x2000);
    s.param(creg(MVOLR, 0), 0x3000);
    s.param(creg(AVOLL, 1), 0x6000);
    s.param(creg(MVOLL, 1), 0x3FFF);
    s.param(creg(MVOLR, 1), 0x3FFF);
    s.sw(reg(KON, 0), 1u << 2);
    s.sw(reg(KON, 1), 1u << 4);
    v0.key_on();
    v1.key_on();
    struct Gates {
        bool v0l, v0r, v1l, v1r; // the voices' VMIXL/VMIXR bits
        u16 mmix1;               // core 1's MMIX
    };
    const Gates gates[] = {
        {true, true, true, true, 0xFFC},    // libsd's: core 0 dry into core 1, every voice dry
        {false, true, true, false, 0xFFC},  // voice mixes off on one side each
        {true, true, true, true, 0xFF0},    // core 1 without core 0
        {true, true, true, true, 0x0FC},    // core 1's voices off (MSND), core 0 still in
        {true, true, true, true, 0xFF8},    // core 0 to the left only
    };
    for (const Gates& gt : gates) {
        INFO("mmix " << gt.mmix1 << " v0 " << gt.v0l << gt.v0r << " v1 " << gt.v1l << gt.v1r);
        s.sw(reg(VMIXL, 0), gt.v0l ? 1u << 2 : 0);
        s.sw(reg(VMIXR, 0), gt.v0r ? 1u << 2 : 0);
        s.sw(reg(VMIXL, 1), gt.v1l ? 1u << 4 : 0);
        s.sw(reg(VMIXR, 1), gt.v1r ? 1u << 4 : 0);
        s.param(reg(MMIX, 1), gt.mmix1);
        auto [l, r] = s.generate(700);
        std::vector<float> want_l, want_r;
        for (u32 i = 0; i < 700; i++) {
            v0.tick(0, 0);
            v1.tick(0, 0);
            // core 0: its voices' dry sums (MMIX 0xFF0), its master volume
            const s32 c0l = clamp16((s64{clamp16(gt.v0l ? v0.left() : 0)} * direct(0x2000)) >> 15);
            const s32 c0r = clamp16((s64{clamp16(gt.v0r ? v0.right() : 0)} * direct(0x3000)) >> 15);
            // core 1: core 0 times AVOL as its external input, its voices, by MMIX
            const s32 el = (c0l * 0x6000) >> 15, er = (c0r * 0x7FFF) >> 15;
            s32 dl = 0, dr = 0;
            if (gt.mmix1 & 0x800) dl += clamp16(gt.v1l ? v1.left() : 0);
            if (gt.mmix1 & 0x400) dr += clamp16(gt.v1r ? v1.right() : 0);
            if (gt.mmix1 & 0x008) dl += el;
            if (gt.mmix1 & 0x004) dr += er;
            want_l.push_back(out(clamp16((s64{clamp16(dl)} * direct(0x3FFF)) >> 15)));
            want_r.push_back(out(clamp16((s64{clamp16(dr)} * direct(0x3FFF)) >> 15)));
        }
        CHECK_SAMPLES(l, want_l);
        CHECK_SAMPLES(r, want_r);
    }
}

TEST_CASE("libsynth2: noise voices and pitch modulation", "[hle][synth2]")
{
    Synth s;
    s.init();
    s.load(0x10000, looped_sine());
    // Core 1: voice 3 noise; voice 6 a sine, voice 7 the sine modulated by voice 6 (PMON), voice 0's PMON bit
    // meaningless.
    ModelVoice noise, carrier, modulated, first;
    for (ModelVoice* v : {&noise, &carrier, &modulated, &first}) {
        v->mem = &s.mem;
        v->ssa = 0x10000;
        v->voll = 0x2000;
        v->volr = 0x2000;
    }
    noise.noise = true;
    noise.pitch = 0x0100;
    carrier.pitch = 0x0400;
    modulated.pitch = 0x1200;
    modulated.pmon = true;
    first.pitch = 0x0C00;
    s.setup(1, 3, noise);
    s.setup(1, 6, carrier);
    s.setup(1, 7, modulated);
    s.setup(1, 0, first);
    s.sw(reg(NON, 1), 1u << 3);
    s.sw(reg(PMON, 1), (1u << 7) | 1u);
    s.core_attr(kNoiseClock | 1, 0x2D); // shift 11, step 1
    s.param(creg(MVOLL, 1), 0x3FFF);
    s.sw(reg(KON, 1), (1u << 3) | (1u << 6) | (1u << 7) | 1u);
    for (ModelVoice* v : {&noise, &carrier, &modulated, &first}) v->key_on();
    // The noise generators ran for neither core before (no sample rendered): both from their power-on state.
    ModelNoise core_noise;
    ModelVoice unmodulated = modulated; // what voice 7 would play without PMON
    unmodulated.pmon = false;
    u32 modulation = 0;
    auto [l, r] = s.generate(3000);
    std::vector<float> want(3000);
    for (u32 i = 0; i < 3000; i++) {
        core_noise.tick(0x2D);
        first.tick(0, core_noise.level);
        noise.tick(0, core_noise.level);
        carrier.tick(0, core_noise.level);
        modulated.tick(carrier.outx, core_noise.level);
        unmodulated.tick(0, core_noise.level);
        modulation += modulated.outx != unmodulated.outx;
        want[i] = out(clamp16((s64{clamp16(first.left() + noise.left() + carrier.left() + modulated.left())} *
            direct(0x3FFF)) >> 15));
    }
    CHECK_SAMPLES(l, want);
    // The noise is noise: many distinct values; the modulated voice played otherwise than its pitch alone.
    std::vector<float> sorted = l;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::unique(sorted.begin(), sorted.end()) - sorted.begin() > 500);
    CHECK(modulation > 2000);
}

TEST_CASE("libsynth2: envelope steps and swept volumes as psx-spx has them", "[hle][synth2]")
{
    using spu2::Spu2;
    // Linear increase at rate 0: +0x3800 a step, every step (psx-spx: +7 << 11).
    u32 c = 0;
    CHECK(Spu2::envelope_tick(0, 0, 0x7F, false, false, false, c) == 0x3800);
    CHECK(Spu2::envelope_tick(0x3800, 0, 0x7F, false, false, false, c) == 0x7000);
    CHECK(Spu2::envelope_tick(0x7000, 0, 0x7F, false, false, false, c) == 0x7FFF);
    // Rate 44 (shift 11, step +7): +7 every tick; rate 49 (shift 12, step +6): +6 every second tick.
    c = 0;
    CHECK(Spu2::envelope_tick(100, 44, 0x7F, false, false, false, c) == 107);
    c = 0;
    CHECK(Spu2::envelope_tick(100, 49, 0x7F, false, false, false, c) == 100);
    CHECK(Spu2::envelope_tick(100, 49, 0x7F, false, false, false, c) == 106);
    // Exponential increase past 0x6000: a quarter of the step below shift 10.
    c = 0;
    CHECK(Spu2::envelope_tick(0x6001, 4 * 9, 0x7F, false, true, false, c) == 0x6001 + ((7 << 2) >> 2));
    // Exponential decrease: the step times the level over 0x8000 (decay at shift 0: -8 << 11).
    c = 0;
    CHECK(Spu2::envelope_tick(0x4000, 0, 0x7C, true, true, false, c) == 0x4000 + ((-0x4000 * 0x4000) >> 15));
    // Linear decrease stops at zero; the all-ones rate never steps.
    c = 0;
    CHECK(Spu2::envelope_tick(5, 0, 0x7C, true, false, false, c) == 0);
    c = 0;
    s32 level = 0x1234;
    for (int i = 0; i < 100000; i++) level = Spu2::envelope_tick(level, 0x7F, 0x7F, false, false, false, c);
    CHECK(level == 0x1234);
    // The gaussian table: each four entries sum to 0x7F7F..0x7F81 (psx-spx), its ends as psx-spx lists them.
    const auto& g = Spu2::gauss();
    for (u32 i = 0; i < 256; i++) {
        const s32 sum = g[i] + g[0xFF - i] + g[0x100 + i] + g[0x1FF - i];
        CHECK(sum >= 0x7F7F);
        CHECK(sum <= 0x7F81);
    }
    CHECK(g[0] == -1);
    CHECK(g[0x100] == 0x1307);
    CHECK(g[0x1FF] == 0x59B3);

    // Through the library: a voice's ENVX in attack, decay and sustain; a swept volume's VOLXL.
    Synth s;
    s.init();
    s.load(0x10000, looped_sine());
    ModelVoice v;
    v.mem = &s.mem;
    v.ssa = 0x10000;
    v.adsr1 = 0x1F54; // linear attack rate 0x1F, decay shift 5, sustain level 4
    v.adsr2 = 0xC7C0; // exponential decreasing sustain rate 0x1F
    s.setup(0, 11, v);
    s.param(vreg(VOLL, 0, 11), 0x8000 | 0x0000 | 0x10); // a sweep: linear increase, rate 0x10
    s.param(vreg(VOLR, 0, 11), 0x2000);
    s.sw(reg(KON, 0), 1u << 11);
    v.key_on();
    s16 swept = 0;
    u32 sweep_counter = 0;
    for (u32 step = 0; step < 40; step++) {
        s.generate(97);
        for (u32 i = 0; i < 97; i++) {
            v.tick(0, 0);
            swept = static_cast<s16>(Spu2::envelope_tick(swept, 0x10, 0x7F, false, false, false, sweep_counter));
        }
        INFO("after " << (step + 1) * 97 << " samples");
        CHECK(s.get_param(vreg(ENVX, 0, 11)) == static_cast<u16>(v.env));
        CHECK(s.get_param(vreg(VOLXL, 0, 11)) == static_cast<u16>(swept));
        CHECK(s.get_param(vreg(VOLXR, 0, 11)) == 0x4000);
    }
    CHECK(v.phase == ModelVoice::Sustain);
    CHECK(swept == 0x7FFF);
    // A swept master volume: decreasing exponentially from where it is.
    s.param(creg(MVOLL, 0), 0x3FFF);
    s.generate(1);
    CHECK(s.get_param(creg(MVOLXL, 0)) == 0x7FFE);
    s.param(creg(MVOLL, 0), 0x8000 | 0x6000 | 0x40); // exponential decrease, rate 0x40 (a step every 32)
    s.generate(200);
    const u16 later = s.get_param(creg(MVOLXL, 0));
    CHECK(later < 0x7FFE);
    CHECK(later > 0);
}

TEST_CASE("libsynth2: the reverb off and on, its work area, its presets", "[hle][synth2]")
{
    const std::vector<u8> vag = encode(sine(672, 7, 22000), 6, 2, 3);
    // The voice on core 1 into the effect only (and dry when `dry`); `mode` and the effect as given.
    struct Run {
        std::vector<float> l, r;
    };
    const auto play = [&](u32 mode, bool effect, bool dry, u16 delay, u16 feedback, bool model_check) {
        Synth s;
        s.init();
        s.load(0x10000, vag);
        ModelVoice v;
        v.mem = &s.mem;
        v.ssa = 0x10000;
        v.voll = 0x3FFF;
        v.volr = 0x3000;
        v.pitch = 0x0E00;
        s.setup(1, 8, v);
        s.sw(reg(VMIXL, 1), dry ? 1u << 8 : 0);
        s.sw(reg(VMIXR, 1), dry ? 1u << 8 : 0);
        s.sw(reg(VMIXEL, 1), 1u << 8);
        s.sw(reg(VMIXER, 1), 1u << 8);
        s.param(creg(MVOLL, 1), 0x3FFF);
        s.param(creg(MVOLR, 1), 0x3FFF);
        u8* b = s.f.base();
        ps_write16(b, kAttr + 0, 1);
        ps_write16(b, kAttr + 2, static_cast<u16>(mode | 0x100));
        ps_write16(b, kAttr + 4, 0x6000);
        ps_write16(b, kAttr + 6, 0x5000);
        ps_write16(b, kAttr + 8, delay);
        ps_write16(b, kAttr + 10, feedback);
        if (effect) s.core_attr(kEffectEnable | 1, 1);
        REQUIRE(s.call("cellSoundSynth2SetEffectAttr", 1, kAttr) == 0);
        CHECK(s.get_param(creg(EVOLL, 1)) == 0x6000);
        CHECK(s.get_param(creg(EVOLR, 1)) == 0x5000);
        const u32 sizes[] = {0x10, 0x26C0, 0x1F40, 0x4840, 0x6FE0, 0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00};
        CHECK(s.get_addr(reg(ESA, 1)) == 0x200000 - sizes[mode]);
        s.sw(reg(KON, 1), 1u << 8);
        v.key_on();
        Run run;
        for (u32 k = 0; k < 4; k++) {
            auto [l, r] = s.generate(2400);
            run.l.insert(run.l.end(), l.begin(), l.end());
            run.r.insert(run.r.end(), r.begin(), r.end());
        }
        if (model_check) {
            // The model: libsd's preset with its echo arithmetic, the work area the mode's size below EEA.
            ModelReverb m;
            const u16 presets[3][32] = {
                {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B,
                    0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
                    0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000},
                {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB,
                    0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
                    0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
                {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD, 0x0FFD,
                    0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
                    0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000}};
            const u16* preset = mode == 2 ? presets[0] : mode == 5 ? presets[1] : presets[2];
            std::copy(preset, preset + 32, m.g.begin());
            if (mode == 7) { // echo: libsd's delay and feedback
                using namespace spu2;
                const u16 d = static_cast<u16>((delay + 1) * 32);
                m.g[vWALL] = static_cast<u16>(feedback * 258);
                m.g[mLSAME] = static_cast<u16>(2 * d - m.g[dAPF1]);
                m.g[mRSAME] = static_cast<u16>(d - m.g[dAPF2]);
                m.g[mLCOMB1] = static_cast<u16>(m.g[mRCOMB1] + d);
                m.g[dLSAME] = static_cast<u16>(m.g[dRSAME] + d);
                m.g[mLAPF1] = static_cast<u16>(m.g[mLAPF2] + d);
                m.g[mRAPF1] = static_cast<u16>(m.g[mRAPF2] + d);
            }
            m.evoll = 0x6000;
            m.evolr = 0x5000;
            m.setup(0x200000 - sizes[mode], 0x1FFFFF);
            std::vector<float> want_l, want_r;
            for (u32 i = 0; i < run.l.size(); i++) {
                v.tick(0, 0);
                s32 rl = 0, rr = 0;
                m.tick(clamp16(v.left()), clamp16(v.right()), effect, rl, rr);
                const s32 dl = dry ? clamp16(v.left()) : 0, dr = dry ? clamp16(v.right()) : 0;
                want_l.push_back(out(clamp16((s64{clamp16(dl + rl)} * direct(0x3FFF)) >> 15)));
                want_r.push_back(out(clamp16((s64{clamp16(dr + rr)} * direct(0x3FFF)) >> 15)));
            }
            CHECK_SAMPLES(run.l, want_l);
            CHECK_SAMPLES(run.r, want_r);
            // The work area holds what the model's does (the sound memory read back through VoiceTrans).
            const u32 bytes = sizes[mode];
            REQUIRE(s.call("cellSoundSynth2VoiceTrans", 1, 1, kSrc, 0x200000 - bytes, bytes) == bytes);
            u32 differ = 0;
            for (u32 w = 0; w < bytes / 2; w++)
                if (static_cast<s16>(ps_read16(b, kSrc + 2 * w)) != m.area[w]) differ++;
            CHECK(differ == 0);
            // CLEAR again zeroes the area.
            REQUIRE(s.call("cellSoundSynth2SetEffectAttr", 1, kAttr) == 0);
            REQUIRE(s.call("cellSoundSynth2VoiceTrans", 1, 1, kSrc, 0x200000 - bytes, bytes) == bytes);
            u32 nonzero = 0;
            for (u32 i = 0; i < bytes; i++) nonzero += ps_read8(b, kSrc + i) != 0;
            CHECK(nonzero == 0);
        }
        return run;
    };
    // With the effect off the reverb neither writes nor plays: the dry sound alone, whatever the mode.
    const Run plain = play(0, false, true, 0, 0, false);
    const Run off = play(5, false, true, 0, 0, true);
    CHECK_SAMPLES(off.l, plain.l);
    // With it on: the studio, the hall and an echo (delay 10, feedback 100), each sample as the model has it.
    const Run studio = play(2, true, false, 0, 0, true);
    CHECK(std::count_if(studio.l.begin(), studio.l.end(), [](float x) { return x != 0.0f; }) > 5000);
    play(5, true, true, 0, 0, true);
    const Run echo = play(7, true, false, 10, 100, true);
    // An echo of the sine comes back: the effect's output after the voice keeps going.
    CHECK(std::count_if(echo.l.begin() + 4000, echo.l.end(), [](float x) { return x != 0.0f; }) > 1000);
}

TEST_CASE("libsynth2: SetEffectMode leaves the effect volume; the effect calls' errors", "[hle][synth2]")
{
    Synth s;
    s.init();
    u8* b = s.f.base();
    const auto attr = [&](u16 mode, s16 dl, s16 dr) {
        ps_write16(b, kAttr + 0, 0);
        ps_write16(b, kAttr + 2, mode);
        ps_write16(b, kAttr + 4, static_cast<u16>(dl));
        ps_write16(b, kAttr + 6, static_cast<u16>(dr));
        ps_write16(b, kAttr + 8, 0);
        ps_write16(b, kAttr + 10, 0);
    };
    s.param(creg(EVOLL, 0), 0x1111);
    s.param(creg(EVOLR, 0), 0x2222);
    attr(3, 0x3333, 0x4444);
    CHECK(s.call("cellSoundSynth2SetEffectMode", 0, kAttr) == 0);
    CHECK(s.get_param(creg(EVOLL, 0)) == 0x1111);
    CHECK(s.get_param(creg(EVOLR, 0)) == 0x2222);
    CHECK(s.get_addr(reg(ESA, 0)) == 0x1E0000 - 0x4840);
    CHECK(s.call("cellSoundSynth2SetEffectAttr", 0, kAttr) == 0);
    CHECK(s.get_param(creg(EVOLL, 0)) == 0x3333);
    CHECK(s.get_param(creg(EVOLR, 0)) == 0x4444);
    // After EEA moves, the area ends there.
    CHECK(s.addr(reg(EEA, 0), 0x100000) == 0);
    attr(9, -1, -2);
    CHECK(s.call("cellSoundSynth2SetEffectAttr", 0, kAttr) == 0);
    CHECK(s.get_addr(reg(ESA, 0)) == 0x120000 - 0x3C00);
    CHECK(s.get_param(creg(EVOLL, 0)) == 0xFFFF);
    // The bus is bit 0, as libsd's core: bus 3 is core 1.
    attr(1, 7, 8);
    CHECK(s.call("cellSoundSynth2SetEffectAttr", 3, kAttr) == 0);
    CHECK(s.get_param(creg(EVOLL, 1)) == 7);
    CHECK(s.get_addr(reg(ESA, 1)) == 0x200000 - 0x26C0);
    // Errors: no mode 10 or past it, CLEAR or not; no attributes, or not guest memory. Nothing changes.
    for (const u16 mode : {u16{10}, u16{0x10A}, u16{0x200}, u16{0xFFFF}}) {
        INFO("mode " << mode);
        attr(mode, 1, 1);
        CHECK(s.call("cellSoundSynth2SetEffectAttr", 1, kAttr) == kInvalid);
        CHECK(s.call("cellSoundSynth2SetEffectMode", 1, kAttr) == kInvalid);
    }
    CHECK(s.get_param(creg(EVOLL, 1)) == 7);
    CHECK(s.call("cellSoundSynth2SetEffectAttr", 1, 0) == kInvalid);
    CHECK(s.call("cellSoundSynth2SetEffectMode", 1, kNotGuest) == kInvalid);
    CHECK(s.get_addr(reg(ESA, 1)) == 0x200000 - 0x26C0);
}

TEST_CASE("libsynth2: Generate's arguments, the surround pair, before Init", "[hle][synth2]")
{
    Synth s;
    u8* b = s.f.base();
    // Before Init the synthesizer is silent and Generate renders it.
    REQUIRE(s.call("cellSoundSynth2Generate", 64, kOutL, kOutR, 0, 0) == 0);
    for (u32 i = 0; i < 64; i++) CHECK(ps_readf32(b, kOutL + 4 * i) == 0.0f);
    s.init();
    s.load(0x10000, looped_sine());
    ModelVoice v;
    v.mem = &s.mem;
    v.ssa = 0x10000;
    v.voll = v.volr = 0x3FFF;
    s.setup(1, 0, v);
    s.param(creg(MVOLL, 1), 0x3FFF);
    s.param(creg(MVOLR, 1), 0x3FFF);
    s.sw(reg(KON, 1), 1);
    // Ls and Rs get silence, over whatever was there.
    for (u32 i = 0; i <= 512; i++) {
        ps_writef32(b, kOutLs + 4 * i, 1.0f);
        ps_writef32(b, kOutRs + 4 * i, -1.0f);
    }
    REQUIRE(s.call("cellSoundSynth2Generate", 512, kOutL, kOutR, kOutLs, kOutRs) == 0);
    u32 sound = 0;
    for (u32 i = 0; i < 512; i++) {
        sound += ps_readf32(b, kOutL + 4 * i) != 0.0f;
        CHECK(ps_readf32(b, kOutLs + 4 * i) == 0.0f);
        CHECK(ps_readf32(b, kOutRs + 4 * i) == 0.0f);
    }
    CHECK(sound > 400);
    CHECK(ps_readf32(b, kOutLs + 4 * 512) == 1.0f); // not one past
    // Errors: no Lout or Rout, a buffer that is not guest memory or runs past it; nothing rendered or written.
    ps_writef32(b, kOutL, 0.5f);
    CHECK(s.call("cellSoundSynth2Generate", 16, 0, kOutR, 0, 0) == kInvalid);
    CHECK(s.call("cellSoundSynth2Generate", 16, kOutL, 0, 0, 0) == kInvalid);
    CHECK(s.call("cellSoundSynth2Generate", 0, 0, 0, 0, 0) == kInvalid);
    CHECK(s.call("cellSoundSynth2Generate", 16, kOutL, kNotGuest, 0, 0) == kInvalid);
    CHECK(s.call("cellSoundSynth2Generate", 16, kOutL, kOutR, kNotGuest, 0) == kInvalid);
    CHECK(s.call("cellSoundSynth2Generate", 16, kOutL, kOutR, 0, 0xFFFFFFF0u) == kInvalid);
    CHECK(ps_readf32(b, kOutL) == 0.5f);
    // Nothing to render.
    CHECK(s.call("cellSoundSynth2Generate", 0, kOutL, kOutR, 0, 0) == 0);
    CHECK(ps_readf32(b, kOutL) == 0.5f);
    // The refused calls rendered nothing: the voice goes on from sample 512.
    std::vector<float> want(256);
    v.key_on();
    for (u32 i = 0; i < 512; i++) v.tick(0, 0);
    for (u32 i = 0; i < 256; i++) {
        v.tick(0, 0);
        want[i] = out(clamp16((s64{clamp16(v.left())} * direct(0x3FFF)) >> 15));
    }
    CHECK_SAMPLES(s.generate(256).first, want);
}

TEST_CASE("libsynth2: the IRQ address and NAX", "[hle][synth2]")
{
    Synth s;
    s.init();
    s.load(0x10000, looped_sine());
    ModelVoice v;
    v.mem = &s.mem;
    v.ssa = 0x10000;
    s.setup(0, 1, v);
    s.sw(reg(KON, 0), 2);
    // NAX follows the blocks; written, the voice goes on from there.
    s.generate(28 * 3 + 1);
    CHECK(s.get_addr(vreg(NAX, 0, 1)) == 0x10030);
    CHECK(s.addr(vreg(NAX, 0, 1), 0x10100) == 0);
    s.generate(28);
    CHECK(s.get_addr(vreg(NAX, 0, 1)) == 0x10110);
    // The IRQ enable and address are kept (libsynth2 has no interrupt handler to call); clearing the enable bit is
    // harmless either way.
    CHECK(s.addr(reg(IRQA, 0), 0x10120) == 0);
    s.core_attr(kIrqEnable, 1);
    s.generate(28);
    s.core_attr(kIrqEnable, 0);
    CHECK(s.get_addr(reg(IRQA, 0)) == 0x10120);
}

TEST_CASE("libsynth2: a loop address written after the key on stays", "[hle][synth2]")
{
    Synth s;
    s.init();
    // A looped sine at 0x10000 (its first block a loop start) and a silent one-block loop at 0x18000.
    s.load(0x10000, looped_sine());
    std::vector<u8> silent(16, 0);
    silent[1] = 7; // loop start, repeat, loop end
    s.load(0x18000, silent);
    ModelVoice v;
    v.mem = &s.mem;
    v.ssa = 0x10000;
    v.voll = v.volr = 0x3FFF;
    s.setup(1, 2, v);
    s.param(creg(MVOLL, 1), 0x3FFF);
    s.sw(reg(KON, 1), 1u << 2);
    v.key_on();
    CHECK(s.get_addr(vreg(LSAX, 1, 2)) == 0x10000); // the first block's loop start
    // The game sends the loop elsewhere: the sound plays once, then the silent block, whose loop start flag leaves
    // the written address alone.
    REQUIRE(s.addr(vreg(LSAX, 1, 2), 0x18000) == 0);
    v.loop = 0x18000;
    v.lsax_written = true;
    auto [l, r] = s.generate(1000);
    std::vector<float> want(1000);
    for (u32 i = 0; i < 1000; i++) {
        v.tick(0, 0);
        want[i] = out(clamp16((s64{clamp16(v.left())} * direct(0x3FFF)) >> 15));
    }
    CHECK_SAMPLES(l, want);
    CHECK(std::all_of(l.begin() + 680, l.end(), [](float x) { return x == 0.0f; }));
    CHECK(std::count_if(l.begin(), l.begin() + 672, [](float x) { return x != 0.0f; }) > 600);
    CHECK(s.get_addr(vreg(NAX, 1, 2)) == 0x18000);
    CHECK(s.get_addr(vreg(LSAX, 1, 2)) == 0x18000);
    CHECK((s.get_sw(reg(ENDX, 1)) >> 2 & 1) == 1);
    // A key on forgets the written address: the first block's loop start sets it again.
    s.sw(reg(KON, 1), 1u << 2);
    CHECK(s.get_addr(vreg(LSAX, 1, 2)) == 0x10000);
}

TEST_CASE("libsynth2: the SPU2's IRQ flag, raised by a voice reading the IRQ address", "[hle][synth2]")
{
    // libsynth2 has no interrupt handler to call (libsd's sceSdSetIRQCallback has no cell form): the flag is the
    // synthesizer's own, seen here on the engine itself.
    spu2::Spu2 spu;
    const std::vector<u8> vag = encode(sine(56, 1, 10000), 6, 2, 3); // two blocks, looped
    std::memcpy(spu.memory.data() + 0x1000, vag.data(), vag.size());
    spu2::Voice& v = spu.cores[1].voices[0];
    v.ssa = 0x1000;
    v.pitch = 0x1000;
    spu.cores[0].irqa = 0x1010; // the second block, core 0's IRQ, read by a voice of core 1
    spu.cores[1].irqa = 0x2000;
    spu.key_on(1, 1);
    s16 l[64], r[64];
    spu.render(l, r, 40);
    CHECK_FALSE(spu.cores[0].irq); // not enabled
    spu.cores[0].attr = spu2::kAttrIrq;
    spu.cores[1].attr = spu2::kAttrIrq;
    spu.render(l, r, 20); // back to the first block
    CHECK_FALSE(spu.cores[0].irq);
    spu.render(l, r, 40); // into the second again
    CHECK(spu.cores[0].irq);
    CHECK_FALSE(spu.cores[1].irq);
    CHECK(v.nax == 0x1010);
    CHECK((spu.cores[1].endx & 1) == 1);
}

TEST_CASE("libsynth2: each process its own synthesizer", "[hle][synth2]")
{
    {
        Synth s;
        s.init();
        s.param(vreg(PITCH, 0, 0), 0x1234);
    }
    Synth s;
    CHECK(s.get_param(vreg(PITCH, 0, 0)) == 0);
    s.init();
    CHECK(s.get_param(vreg(PITCH, 0, 0)) == 0x3FFF);
}
