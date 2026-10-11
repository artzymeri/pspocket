// libsnd3 (D-640 to D-643): Sony's sound library over libsynth2, pspocket's own, firmware-first. Synthetic HD3 banks
// and Standard MIDI Files (snd3_writer.hpp), and two libsynth2s under it: a recording one in guest code, linked as a
// firmware module's exports would be (each call logged with its arguments, its registers kept), to check what libsnd3
// tells libsynth2 and when; and pspocket's own (D-610), end to end, with VAG sounds from vag_writer.hpp. No game data.

#include "hle_test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "hle_fuzz_util.hpp"
#include "pspocket/core/be.h"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "snd3_writer.hpp"
#include "vag_writer.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::snd3;
namespace as = pspocket::recomp::ppu::as;

namespace {

// ---- error codes (RPCS3 libsnd3.h) ----
constexpr u32 PARAM = 0x80310301, SYNTH = 0x80310303, ALREADY = 0x80310304, NOTINIT = 0x80310305, SMFFULL = 0x80310306,
              HD3ID = 0x80310307, SMF = 0x80310308, SMFCTX = 0x80310309, FORMAT = 0x8031030a, SMFID = 0x8031030b,
              SOUNDDATAFULL = 0x8031030c, VOICENUM = 0x8031030d, RESERVEDVOICE = 0x8031030e, QUEUEFULL = 0x8031030f,
              OUTPUTMODE = 0x80310310;
u64 err(u32 code) { return r3_of_code(code); }

// ---- libsd's registers (ps2sdk libsd-common.h): the parameter in bits 8-15, the core in bit 0, the voice in 1-5 ----
enum : u32 { VOLL = 0x00, VOLR, PITCH, ADSR1, ADSR2, ENVX, KON = 0x15, KOFF, ENDX, VMIXL, VMIXEL, VMIXR, VMIXER, SSA = 0x20 };
constexpr u16 vreg(u32 param, u32 voice) { return static_cast<u16>(param << 8 | (voice % 24) << 1 | voice / 24); }
constexpr u16 sreg(u32 param, u32 core) { return static_cast<u16>(param << 8 | core); }
constexpr u16 kMvoll = 0x0980, kMvolr = 0x0A80, kEffectEnable = 0x2;

// ---- guest memory ----
constexpr u32 kParam = kData + 0x100, kQueueCtx = kData + 0x140, kDataCtx = kData + 0x180, kBits = kData + 0x1C0,
              kWord = kData + 0x1E0, kSmfCtx = kData + 0x200, kSmfCtx2 = kData + 0x380, kQueueFront = kData + 0x600,
              kQueueRear = kData + 0x800, kHd3 = kData + 0x1000, kSmfData = kData + 0x20000, kOutL = kData + 0x40000,
              kOutR = kData + 0x44000, kOutRL = kData + 0x48000, kOutRR = kData + 0x4C000, kVagSrc = kData + 0x50000,
              kNotGuest = 0xF0000000u;
constexpr u32 kSamples = 256; // each Synthesis's block

// Guest code without lifted code runs in the interpreter while this lives.
struct InterpretMisses {
    const ps_ppu_miss_policy ppu = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(ppu); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// ---- a recording libsynth2 in guest code ----

enum Fn : u32 {
    kInit, kExit, kSetParam, kGetParam, kSetSwitch, kGetSwitch, kSetAddr, kGetAddr, kSetEffectAttr, kSetEffectMode,
    kSetCoreAttr, kGenerate, kVoiceTrans, kVoiceTransStatus, kNote2Pitch, kPitch2Note, kConfig, kFns
};
constexpr const char* kFnNames[kFns] = {"cellSoundSynth2Init", "cellSoundSynth2Exit", "cellSoundSynth2SetParam",
    "cellSoundSynth2GetParam", "cellSoundSynth2SetSwitch", "cellSoundSynth2GetSwitch", "cellSoundSynth2SetAddr",
    "cellSoundSynth2GetAddr", "cellSoundSynth2SetEffectAttr", "cellSoundSynth2SetEffectMode",
    "cellSoundSynth2SetCoreAttr", "cellSoundSynth2Generate", "cellSoundSynth2VoiceTrans",
    "cellSoundSynth2VoiceTransStatus", "cellSoundSynth2Note2Pitch", "cellSoundSynth2Pitch2Note", "cellSoundSynth2Config"};

// The log (a count, then 32-byte records from +16: the function, r3 to r7, two more words), the registers SetParam
// (16 bits) and SetSwitch/SetAddr (32 bits) keep by register number, each function's answer, the code, descriptors.
constexpr u32 kFakeLog = kData + 0x100000, kFakeRegs16 = kData + 0x150000, kFakeRegs32 = kData + 0x170000,
              kFakeRet = kData + 0x1B0000, kFakeCode = kData + 0x1B1000, kFakeOpd = kData + 0x1B8000;
constexpr u32 kFakeMax = 8192;

struct Call {
    u32 fn = 0;
    std::array<u32, 5> a{}; // r3 to r7
    u32 x0 = 0, x1 = 0;     // SetEffectAttr: the attributes' first two words (the third in a[2])
};

struct FakeSynth {
    u8* b;

    explicit FakeSynth(Fixture& f) : b(f.base())
    {
        std::memset(b + kFakeLog, 0, kFakeRet + 0x100 - kFakeLog);
        std::vector<u32> c;
        const auto load = [&](u32 reg, u32 value) {
            c.push_back(as::lis(reg, static_cast<s32>(value >> 16)));
            c.push_back(as::ori(reg, reg, value & 0xFFFF));
        };
        std::array<u32, kFns> entry{};
        for (u32 k = 0; k < kFns; k++) {
            entry[k] = kFakeCode + 4 * static_cast<u32>(c.size());
            load(11, kFakeLog);
            c.push_back(as::lwz(12, 0, 11));
            c.push_back(as::cmplwi(0, 12, kFakeMax));
            const usize skip = c.size();
            c.push_back(0); // bge: the log is full
            c.push_back(as::addi(10, 12, 1));
            c.push_back(as::stw(10, 0, 11));
            c.push_back(as::slwi(12, 12, 5));
            c.push_back(as::add(12, 11, 12));
            c.push_back(as::li(10, static_cast<s32>(k)));
            c.push_back(as::stw(10, 16, 12));
            for (u32 r = 0; r < 5; r++) c.push_back(as::stw(3 + r, static_cast<s32>(20 + 4 * r), 12));
            if (k == kSetEffectAttr || k == kSetEffectMode) {
                c.push_back(as::lwz(9, 0, 4));
                c.push_back(as::stw(9, 40, 12));
                c.push_back(as::lwz(9, 4, 4));
                c.push_back(as::stw(9, 44, 12));
                c.push_back(as::lwz(9, 8, 4));
                c.push_back(as::stw(9, 28, 12));
            }
            c[skip] = as::bc_rel(4, 0, static_cast<s32>(4 * (c.size() - skip)));
            switch (k) {
            case kSetParam:
                c.push_back(as::rlwinm(9, 3, 1, 15, 30));
                load(10, kFakeRegs16);
                c.push_back(as::sthx(4, 10, 9));
                break;
            case kGetParam:
                c.push_back(as::rlwinm(9, 3, 1, 15, 30));
                load(10, kFakeRegs16);
                c.push_back(as::lhzx(3, 10, 9));
                break;
            case kSetSwitch:
            case kSetAddr:
                c.push_back(as::rlwinm(9, 3, 2, 14, 29));
                load(10, kFakeRegs32);
                c.push_back(as::stwx(4, 10, 9));
                load(10, kFakeRet);
                c.push_back(as::lwz(3, static_cast<s32>(4 * k), 10));
                break;
            case kGetSwitch:
            case kGetAddr:
                c.push_back(as::rlwinm(9, 3, 2, 14, 29));
                load(10, kFakeRegs32);
                c.push_back(as::lwzx(3, 10, 9));
                break;
            default:
                load(10, kFakeRet);
                c.push_back(as::lwz(3, static_cast<s32>(4 * k), 10));
                break;
            }
            c.push_back(as::blr());
        }
        REQUIRE(kFakeCode + 4 * c.size() <= kFakeOpd);
        for (u32 i = 0; i < c.size(); i++) ps_write32(b, kFakeCode + 4 * i, c[i]);
        for (u32 k = 0; k < kFns; k++) {
            ps_write32(b, kFakeOpd + 8 * k, entry[k]);
            ps_write32(b, kFakeOpd + 8 * k + 4, 0);
            f.hle->link_import("libsynth2", recomp::nid_of(kFnNames[k]), kFakeOpd + 8 * k);
        }
    }

    u32 count() const { return ps_read32(b, kFakeLog); }
    void clear() { ps_write32(b, kFakeLog, 0); }
    Call call(u32 i) const
    {
        const u32 at = kFakeLog + 16 + 32 * i;
        Call c;
        c.fn = ps_read32(b, at);
        for (u32 r = 0; r < 5; r++) c.a[r] = ps_read32(b, at + 4 + 4 * r);
        c.x0 = ps_read32(b, at + 24);
        c.x1 = ps_read32(b, at + 28);
        return c;
    }
    std::vector<Call> calls() const
    {
        std::vector<Call> out;
        for (u32 i = 0; i < std::min(count(), kFakeMax); i++) out.push_back(call(i));
        return out;
    }
    std::vector<Call> calls(Fn fn) const
    {
        std::vector<Call> out;
        for (const Call& c : calls())
            if (c.fn == fn) out.push_back(c);
        return out;
    }
    usize count(Fn fn) const { return calls(fn).size(); }
    // The last value written to a register with SetParam, SetSwitch or SetAddr since the log was cleared (none: -1).
    s64 last(Fn fn, u16 reg) const
    {
        s64 v = -1;
        for (const Call& c : calls(fn))
            if (static_cast<u16>(c.a[0]) == reg) v = c.a[1];
        return v;
    }
    s64 param(u16 reg) const { return last(kSetParam, reg); }
    s64 sw(u16 reg) const { return last(kSetSwitch, reg); }
    s64 addr(u16 reg) const { return last(kSetAddr, reg); }
    void answer(Fn fn, u32 value) { ps_write32(b, kFakeRet + 4 * fn, value); }
    void set16(u16 reg, u16 v) { ps_write16(b, kFakeRegs16 + 2u * reg, v); }
    void set32(u16 reg, u32 v) { ps_write32(b, kFakeRegs32 + 4u * reg, v); }
    u32 get32(u16 reg) const { return ps_read32(b, kFakeRegs32 + 4u * reg); }
};

void key_on_param(u8* b, u8 velocity, u8 pan, u8 pan_ex, s32 add_pitch)
{
    ps_write8(b, kParam, velocity);
    ps_write8(b, kParam + 1, pan);
    ps_write8(b, kParam + 2, pan_ex);
    ps_write8(b, kParam + 3, 0);
    ps_write32(b, kParam + 4, static_cast<u32>(add_pitch));
}

// The voices CellSnd3VoiceBitCtx names: core c's word, bit v for its voice v.
std::vector<u32> voices_in(u8* b)
{
    std::vector<u32> out;
    for (u32 c = 0; c < 4; c++)
        for (u32 v = 0; v < 32; v++)
            if (ps_read32(b, kBits + 4 * c) & (1u << v)) out.push_back(c * 24 + v);
    return out;
}

// A voice's volume registers as D-643 makes them: 0x3FFF times each factor over 127, then the pan (0-127, 64 the
// middle): the left full to 64 and down to 0 at 127, the right up from 0 to full at 64.
std::pair<u32, u32> volumes(std::initializer_list<u32> factors, s32 pan)
{
    unsigned __int128 num = 0x3FFF, den = 1;
    for (const u32 f : factors) {
        num *= f;
        den *= 127;
    }
    const u32 amp = static_cast<u32>((num + den / 2) / den);
    pan = std::clamp(pan, 0, 127);
    return {(amp * static_cast<u32>(std::min(127 - pan, 63)) + 31) / 63, (amp * static_cast<u32>(std::min(pan, 64)) + 32) / 64};
}

// The fixture with libsnd3's thread and the recording libsynth2.
struct Snd {
    InterpretMisses interpret;
    Fixture f;
    GuestThread g{f, 0, "game"};
    FakeSynth synth{f};
    u8* b = f.base();

    template <class... A>
    u64 call(const char* name, A... args)
    {
        return g.call(name, args...);
    }
    u64 init(u32 voices = 48, u32 queue = 0) { return call("cellSnd3Init", voices, kSamples, queue); }
    // The usual bank bound at synthesizer offset 0x10000: its hd3ID.
    u64 bind(const BankSpec& spec = usual_bank(), u32 offset = 0x10000)
    {
        place(b, kHd3, build_hd3(spec));
        return call("cellSnd3BindSoundData", kDataCtx, kHd3, offset);
    }
    u64 synthesis() { return call("cellSnd3Synthesis", kOutL, kOutR); }
    s32 status(u32 voice) { return static_cast<s32>(call("cellSnd3VoiceGetStatus", voice)); }
};

} // namespace

TEST_CASE("libsnd3: all 50 functions are pspocket's, firmware-first, over libsynth2's exports", "[hle][snd3]")
{
    const char* names[] = {"cellSnd3Init", "cellSnd3Exit", "cellSnd3Note2Pitch", "cellSnd3Pitch2Note",
        "cellSnd3SetOutputMode", "cellSnd3Synthesis", "cellSnd3SynthesisEx", "cellSnd3BindSoundData",
        "cellSnd3UnbindSoundData", "cellSnd3NoteOnByTone", "cellSnd3KeyOnByTone", "cellSnd3VoiceNoteOnByTone",
        "cellSnd3VoiceKeyOnByTone", "cellSnd3VoiceSetReserveMode", "cellSnd3VoiceSetSustainHold", "cellSnd3VoiceKeyOff",
        "cellSnd3VoiceSetPitch", "cellSnd3VoiceSetVelocity", "cellSnd3VoiceSetPanpot", "cellSnd3VoiceSetPanpotEx",
        "cellSnd3VoiceSetPitchBend", "cellSnd3VoiceAllKeyOff", "cellSnd3VoiceGetEnvelope", "cellSnd3VoiceGetStatus",
        "cellSnd3KeyOffByID", "cellSnd3GetVoice", "cellSnd3GetVoiceByID", "cellSnd3NoteOn", "cellSnd3NoteOff",
        "cellSnd3SetSustainHold", "cellSnd3SetEffectType", "cellSnd3SMFBind", "cellSnd3SMFUnbind", "cellSnd3SMFPlay",
        "cellSnd3SMFPlayEx", "cellSnd3SMFPause", "cellSnd3SMFResume", "cellSnd3SMFStop", "cellSnd3SMFAddTempo",
        "cellSnd3SMFGetTempo", "cellSnd3SMFSetPlayVelocity", "cellSnd3SMFGetPlayVelocity", "cellSnd3SMFSetPlayPanpot",
        "cellSnd3SMFSetPlayPanpotEx", "cellSnd3SMFGetPlayPanpot", "cellSnd3SMFGetPlayPanpotEx",
        "cellSnd3SMFGetPlayStatus", "cellSnd3SMFSetPlayChannel", "cellSnd3SMFGetPlayChannel", "cellSnd3SMFGetKeyOnID"};
    for (const char* name : names) {
        INFO(name);
        const Function* f = registry().find("libsnd3", recomp::nid_of(name));
        REQUIRE(f != nullptr);
        CHECK(f->name == name);
        CHECK(recomp::NidDb::builtin().find("libsnd3", f->nid) == std::optional<std::string_view>(name));
    }
    CHECK(registry().is_firmware_first("libsnd3"));
    CHECK(registry().is_firmware_first("libsynth2")); // D-610: never Sony's libsnd3 with pspocket's libsynth2
    // What it takes from libsynth2, as a firmware module would give it: every one libsynth2's export by that name.
    usize imports = 0;
    for (const HostImport& i : registry().imports()) {
        if (i.module != "libsnd3") continue;
        imports++;
        CHECK(i.library == "libsynth2");
        CHECK_FALSE(i.variable);
        CHECK(registry().find("libsynth2", i.nid) != nullptr);
    }
    CHECK(imports == 10);
}

TEST_CASE("libsnd3: Note2Pitch and Pitch2Note are libsd's arithmetic, as libsynth2's", "[hle][snd3]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    // The centre plays at 0x1000; an octave doubles it; a semitone is 2^(1/12) (libsd's tables, rounded down); the
    // centre's fine tune adds to the note's; two octaves up and beyond stay at that octave's values (libsd's).
    CHECK(g.call("cellSnd3Note2Pitch", 60, 0, 60, 0) == 0x1000);
    CHECK(g.call("cellSnd3Note2Pitch", 60, 0, 72, 0) == 0x2000);
    CHECK(g.call("cellSnd3Note2Pitch", 60, 0, 48, 0) == 0x0800);
    CHECK(g.call("cellSnd3Note2Pitch", 60, 0, 61, 0) == 0x10F4);
    CHECK(g.call("cellSnd3Note2Pitch", 60, 0, 84, 0) == 0x4000);
    CHECK(g.call("cellSnd3Note2Pitch", 60, 0, 96, 0) == 0x4000);
    CHECK(g.call("cellSnd3Note2Pitch", 60, 0, 0, 0) == 0x0080);
    CHECK(g.call("cellSnd3Note2Pitch", 60, 0, 60, 128) == 0x10F4);
    CHECK(g.call("cellSnd3Note2Pitch", 60, 64, 60, 64) == 0x10F4);
    CHECK(g.call("cellSnd3Note2Pitch", 60, 0, 61, static_cast<u16>(-128)) == 0x1000);
    CHECK(g.call("cellSnd3Pitch2Note", 60, 0, 0x1000) == 0x3C00);
    CHECK(g.call("cellSnd3Pitch2Note", 60, 0, 0x2000) == 0x4800);
    CHECK(g.call("cellSnd3Pitch2Note", 60, 0, 0x0800) == 0x3000);
    CHECK(g.call("cellSnd3Pitch2Note", 60, 0, 0x3FFF) == 0x5400);
    CHECK(g.call("cellSnd3Pitch2Note", 60, 0, 0xFFFF) == 0x5400); // at most 0x3FFF
    CHECK(g.call("cellSnd3Pitch2Note", 60, 0, 0) == 0xAC00);      // 144 semitones below the centre
    CHECK(g.call("cellSnd3Pitch2Note", 60, 0, 0x10F4) == 0x3D00);
    // libsynth2 has the same two (D-614): libsnd3's answers are its answers, before Init too.
    for (u32 note = 0; note < 128; note += 3)
        for (const s32 fine : {-200, -128, -64, -1, 0, 1, 63, 127, 200}) {
            INFO("note " << note << " fine " << fine);
            CHECK(g.call("cellSnd3Note2Pitch", 60, 5, note, static_cast<u16>(fine)) ==
                g.call("cellSoundSynth2Note2Pitch", 60, 5, note, static_cast<u16>(fine)));
        }
    for (u32 pitch = 0; pitch < 0x4000; pitch += 0x7F) {
        INFO("pitch " << pitch);
        CHECK(g.call("cellSnd3Pitch2Note", 64, 3, pitch) == g.call("cellSoundSynth2Pitch2Note", 64, 3, pitch));
    }
}

TEST_CASE("libsnd3: Init and Exit, their checks, and every call before Init", "[hle][snd3]")
{
    Snd s;
    // Before Init: NOTINIT from every call but the pitch arithmetic; KeyOffByID keys nothing off.
    const std::vector<std::pair<const char*, std::vector<u64>>> calls = {{"cellSnd3Exit", {}},
        {"cellSnd3SetOutputMode", {0}}, {"cellSnd3Synthesis", {kOutL, kOutR}},
        {"cellSnd3SynthesisEx", {kOutL, kOutR, kOutRL, kOutRR}}, {"cellSnd3BindSoundData", {kDataCtx, kHd3, 0}},
        {"cellSnd3UnbindSoundData", {0}}, {"cellSnd3NoteOnByTone", {0, 0, 60, 1, 0}},
        {"cellSnd3KeyOnByTone", {0, 0, 0x1000, 1, 0}}, {"cellSnd3VoiceNoteOnByTone", {0, 0, 0, 60, 1, 0}},
        {"cellSnd3VoiceKeyOnByTone", {0, 0, 0, 0x1000, 1, 0}}, {"cellSnd3VoiceSetReserveMode", {0, 1}},
        {"cellSnd3VoiceSetSustainHold", {0, 1}}, {"cellSnd3VoiceKeyOff", {0}}, {"cellSnd3VoiceSetPitch", {0, 0}},
        {"cellSnd3VoiceSetVelocity", {0, 0}}, {"cellSnd3VoiceSetPanpot", {0, 0}}, {"cellSnd3VoiceSetPanpotEx", {0, 0}},
        {"cellSnd3VoiceSetPitchBend", {0, 0}}, {"cellSnd3VoiceAllKeyOff", {}}, {"cellSnd3VoiceGetEnvelope", {0}},
        {"cellSnd3VoiceGetStatus", {0}}, {"cellSnd3GetVoice", {0, 0, kBits}}, {"cellSnd3GetVoiceByID", {0, kBits}},
        {"cellSnd3NoteOn", {0, 0, 0, 60, 0, 0, 1}}, {"cellSnd3NoteOff", {0, 60, 1}},
        {"cellSnd3SetSustainHold", {0, 1, 1}}, {"cellSnd3SetEffectType", {0, 0, 0, 0}},
        {"cellSnd3SMFBind", {kSmfCtx, kSmfData, 0}}, {"cellSnd3SMFUnbind", {0}}, {"cellSnd3SMFPlay", {0, 127, 64, 1}},
        {"cellSnd3SMFPlayEx", {0, 127, 64, 64, 1}}, {"cellSnd3SMFPause", {0}}, {"cellSnd3SMFResume", {0}},
        {"cellSnd3SMFStop", {0}}, {"cellSnd3SMFAddTempo", {0, 1}}, {"cellSnd3SMFGetTempo", {0}},
        {"cellSnd3SMFSetPlayVelocity", {0, 1}}, {"cellSnd3SMFGetPlayVelocity", {0}},
        {"cellSnd3SMFSetPlayPanpot", {0, 1}}, {"cellSnd3SMFSetPlayPanpotEx", {0, 1}},
        {"cellSnd3SMFGetPlayPanpot", {0}}, {"cellSnd3SMFGetPlayPanpotEx", {0}}, {"cellSnd3SMFGetPlayStatus", {0}},
        {"cellSnd3SMFSetPlayChannel", {0, 1}}, {"cellSnd3SMFGetPlayChannel", {0, kWord}},
        {"cellSnd3SMFGetKeyOnID", {0, 0, kWord}}};
    CHECK(calls.size() == 46);
    for (const auto& [name, args] : calls) {
        INFO(name);
        const Function& fn = GuestThread::find(name);
        for (usize i = 0; i < args.size(); i++) {
            if (i < 8) s.g.ctx.gpr[3 + i] = args[i];
            else ps_write64(s.b, static_cast<u32>(s.g.ctx.gpr[1]) + 0x70 + 8 * static_cast<u32>(i - 8), args[i]);
        }
        fn.fn(s.g.ctx, s.b);
        CHECK(s.g.ctx.gpr[3] == err(NOTINIT));
    }
    CHECK(s.call("cellSnd3KeyOffByID", 1) == 0);
    CHECK(s.synth.count() == 0); // nothing reached libsynth2

    // Init's checks: 1 to 48 voices, 1 to 65535 samples a block, a queue whose two buffers hold a request each.
    CHECK(s.call("cellSnd3Init", 0, kSamples, 0) == err(PARAM));
    CHECK(s.call("cellSnd3Init", 49, kSamples, 0) == err(PARAM));
    CHECK(s.call("cellSnd3Init", 8, 0, 0) == err(PARAM));
    CHECK(s.call("cellSnd3Init", 8, 0x10000, 0) == err(PARAM));
    CHECK(s.call("cellSnd3Init", 8, kSamples, kNotGuest) == err(PARAM));
    const auto queue = [&](u32 front, u32 front_size, u32 rear, u32 rear_size) {
        ps_write32(s.b, kQueueCtx, front);
        ps_write32(s.b, kQueueCtx + 4, front_size);
        ps_write32(s.b, kQueueCtx + 8, rear);
        ps_write32(s.b, kQueueCtx + 12, rear_size);
        return s.call("cellSnd3Init", 8, kSamples, kQueueCtx);
    };
    CHECK(queue(0, 0x100, kQueueRear, 0x100) == err(PARAM));
    CHECK(queue(kQueueFront, 0x100, 0, 0x100) == err(PARAM));
    CHECK(queue(kQueueFront, 31, kQueueRear, 0x100) == err(PARAM));
    CHECK(queue(kQueueFront, 0x100, kQueueRear, 31) == err(PARAM));
    CHECK(queue(kQueueFront, 0x100, kNotGuest, 0x100) == err(PARAM));
    CHECK(s.synth.count() == 0);

    // libsynth2 initialised cold; both cores' master volume up; the library's voices (here 30: core 0's 24 and six of
    // core 1's) dry on both sides, sent to no effect, keyed off; the other voices' bits left as libsynth2 had them.
    for (u32 c = 0; c < 2; c++) {
        s.synth.set32(sreg(VMIXL, c), 0x800000);
        s.synth.set32(sreg(VMIXR, c), 0);
        s.synth.set32(sreg(VMIXEL, c), 0xFFFFFF);
        s.synth.set32(sreg(VMIXER, c), 0xFFFFFF);
    }
    CHECK(s.call("cellSnd3Init", 30, kSamples, 0) == 0);
    CHECK(s.call("cellSnd3Init", 30, kSamples, 0) == err(ALREADY));
    REQUIRE(s.synth.count(kInit) == 1);
    CHECK(s.synth.calls(kInit)[0].a[0] == 0);
    for (u32 c = 0; c < 2; c++) {
        INFO("core " << c);
        const u32 ours = c == 0 ? 0xFFFFFF : 0x3F;
        CHECK(s.synth.param(kMvoll | c) == 0x3FFF);
        CHECK(s.synth.param(kMvolr | c) == 0x3FFF);
        CHECK(s.synth.sw(sreg(VMIXL, c)) == (0x800000 | ours));
        CHECK(s.synth.sw(sreg(VMIXR, c)) == ours);
        CHECK(s.synth.sw(sreg(VMIXEL, c)) == (0xFFFFFF & ~ours));
        CHECK(s.synth.sw(sreg(VMIXER, c)) == (0xFFFFFF & ~ours));
        CHECK(s.synth.sw(sreg(KOFF, c)) == ours);
    }
    CHECK(s.status(29) == 0);
    CHECK(s.call("cellSnd3VoiceGetStatus", 30) == err(VOICENUM));

    // Exit: the library's voices keyed off, libsynth2 ended (Init started it); then a second Init.
    s.synth.clear();
    CHECK(s.call("cellSnd3Exit") == 0);
    CHECK(s.call("cellSnd3Exit") == err(NOTINIT));
    CHECK(s.synth.sw(sreg(KOFF, 0)) == 0xFFFFFF);
    CHECK(s.synth.sw(sreg(KOFF, 1)) == 0x3F);
    CHECK(s.synth.count(kExit) == 1);

    // libsynth2 already initialised (by the game): used as it is, and left to the game at Exit.
    s.synth.clear();
    s.synth.answer(kInit, 0x80310203); // ALREADY_INITIALIZED
    CHECK(s.init(8) == 0);
    CHECK(s.call("cellSnd3Exit") == 0);
    CHECK(s.synth.count(kExit) == 0);
    // Another libsynth2 failure: SYNTH, nothing initialised.
    s.synth.answer(kInit, 0x80310201); // FATAL
    CHECK(s.init(8) == err(SYNTH));
    CHECK(s.call("cellSnd3Exit") == err(NOTINIT));
    s.synth.answer(kInit, 0);
    CHECK(s.init(8) == 0);
}

TEST_CASE("libsnd3: each process its own library", "[hle][snd3]")
{
    {
        Snd s;
        CHECK(s.init() == 0);
        CHECK(s.bind() == 0);
    }
    Snd s;
    CHECK(s.call("cellSnd3Exit") == err(NOTINIT));
    CHECK(s.init() == 0);
    CHECK(s.bind() == 0);
}

TEST_CASE("libsnd3: HD3 banks bound, refused and let go", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init() == 0);
    // Sixteen banks, their IDs from 0; a seventeenth SOUNDDATAFULL; one let go makes room again.
    for (u32 i = 0; i < 16; i++) CHECK(s.bind() == i);
    CHECK(s.bind() == err(SOUNDDATAFULL));
    CHECK(s.call("cellSnd3UnbindSoundData", 5) == 0);
    CHECK(s.call("cellSnd3UnbindSoundData", 5) == err(HD3ID));
    CHECK(s.call("cellSnd3UnbindSoundData", 16) == err(HD3ID));
    CHECK(s.bind() == 5);
    for (u32 i = 0; i < 16; i++) CHECK(s.call("cellSnd3UnbindSoundData", i) == 0);

    // The arguments: a context, a header, an offset inside the 2 MB, the samples fitting there.
    place(s.b, kHd3, build_hd3(usual_bank()));
    CHECK(s.call("cellSnd3BindSoundData", 0, kHd3, 0) == err(PARAM));
    CHECK(s.call("cellSnd3BindSoundData", kNotGuest, kHd3, 0) == err(PARAM));
    CHECK(s.call("cellSnd3BindSoundData", kDataCtx, 0, 0) == err(PARAM));
    CHECK(s.call("cellSnd3BindSoundData", kDataCtx, kNotGuest, 0) == err(PARAM));
    CHECK(s.call("cellSnd3BindSoundData", kDataCtx, kHd3, 0x200000) == err(PARAM));
    CHECK(s.call("cellSnd3BindSoundData", kDataCtx, kHd3, 0x200000 - 0xFFF) == err(PARAM)); // VAG 1 ends past it
    CHECK(s.call("cellSnd3BindSoundData", kDataCtx, kHd3, 0x200000 - 0x1000) == 0);       // just fits
    CHECK(s.call("cellSnd3UnbindSoundData", 0) == 0);

    // A bank that is not one: FORMAT, nothing bound.
    const auto refused = [&](const std::vector<u8>& hd3, const char* why) {
        INFO(why);
        place(s.b, kHd3, hd3);
        CHECK(s.call("cellSnd3BindSoundData", kDataCtx, kHd3, 0) == err(FORMAT));
    };
    std::vector<u8> good = build_hd3(usual_bank());
    {
        auto d = good;
        d[0] = 'S';
        refused(d, "no P3HD");
    }
    {
        auto d = good;
        put32(d, 4, 0x01000000);
        refused(d, "bigger than guest memory holds");
    }
    const u32 pg = ps_ld32(good.data() + 0x10), tn = ps_ld32(good.data() + 0x14), va = ps_ld32(good.data() + 0x18);
    {
        auto d = good;
        put32(d, 0x18, static_cast<u32>(d.size()));
        refused(d, "P3VA past the end");
    }
    {
        auto d = good;
        d[va] = 'X';
        refused(d, "no P3VA magic");
    }
    {
        auto d = good;
        put32(d, va + 0x14, 0x10000);
        refused(d, "too many VAGs");
    }
    {
        auto d = good;
        put32(d, va + 0x14, 2); // three entries in a chunk of two
        refused(d, "VAG entries past the chunk");
    }
    {
        auto d = good;
        put32(d, va + 0x20 + 16 + 8, 0x200000);
        refused(d, "a VAG bigger than sound memory");
    }
    {
        auto d = good;
        d[tn + 1] = 0;
        refused(d, "no P3TN magic");
    }
    {
        auto d = good;
        put32(d, tn + 0x20, 0x7FFF0);
        refused(d, "a tone past its chunk");
    }
    {
        BankSpec b = usual_bank();
        b.tones[0]->vag = 2;
        refused(build_hd3(b), "a tone of a VAG that is not there");
    }
    {
        BankSpec b = usual_bank();
        b.vags[1].rate = 0; // an empty entry
        refused(build_hd3(b), "a tone of an empty VAG");
    }
    {
        BankSpec b = usual_bank();
        b.tones[0]->base_note = 128;
        refused(build_hd3(b), "a base note past 127");
    }
    {
        auto d = good;
        d[pg + 2] = 0;
        refused(d, "no P3PG magic");
    }
    {
        BankSpec b = usual_bank();
        b.programs[0]->splits[1].tone = 2; // the missing tone
        refused(build_hd3(b), "a split of a tone that is not there");
    }
    {
        BankSpec b = usual_bank();
        b.programs[0]->splits[0].high = 200;
        refused(build_hd3(b), "a split past note 127");
    }
    {
        auto d = good;
        const u32 prog = pg + ps_ld32(d.data() + pg + 0x20);
        d[prog + 5] = 12; // split blocks of 12 bytes
        refused(d, "split blocks too small");
    }
    {
        auto d = good;
        const u32 prog = pg + ps_ld32(d.data() + pg + 0x20);
        put32(d, prog, 0x10000);
        refused(d, "splits past the chunk");
    }
    CHECK(s.bind() == 0);
    // Without programs (P3PG's offset 0) the tones still play; NoteOn has no program to take.
    BankSpec tones_only = usual_bank();
    tones_only.programs.clear();
    CHECK(s.bind(tones_only) == 1);
    CHECK(s.call("cellSnd3NoteOnByTone", 1, 0, 60, 7, 0) == 0);
    CHECK(s.call("cellSnd3NoteOn", 1, 0, 0, 60, 0, 0, 7) == err(PARAM));
    // A split whose high note is below its low plays up to 127 (as VGMTrans reads a PS2 bank).
    BankSpec wide = usual_bank();
    wide.programs[0]->splits = {{.tone = 0, .low = 100, .high = 20}};
    CHECK(s.bind(wide) == 2);
    s.synth.clear();
    CHECK(s.call("cellSnd3NoteOn", 2, 0, 0, 120, 0, 0, 8) == 0);
    CHECK(s.call("cellSnd3NoteOn", 2, 0, 0, 99, 0, 0, 8) == 0);
    CHECK(s.synth.count(kSetAddr) == 1);
}

TEST_CASE("libsnd3: tone key-ons tell libsynth2 a voice's registers and key it on", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init(30) == 0);
    REQUIRE(s.bind() == 0);
    s.synth.clear();
    // Tone 0 (VAG 0 at 24 kHz, base note 60) at note 72: 0x2000 for the octave, half for the rate.
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 72, 0x1234, 0) == 0);
    CHECK(s.synth.param(vreg(PITCH, 0)) == 0x1000);
    CHECK(s.synth.param(vreg(VOLL, 0)) == 0x3FFF);
    CHECK(s.synth.param(vreg(VOLR, 0)) == 0x3FFF);
    CHECK(s.synth.param(vreg(ADSR1, 0)) == 0x000F);
    CHECK(s.synth.param(vreg(ADSR2, 0)) == 0x1FC0);
    CHECK(s.synth.addr(vreg(SSA, 0)) == 0x10000);
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x1);
    CHECK(s.status(0) == 1);
    // The next voice: a semitone up, velocity 64, panned left, through the key-on parameter.
    key_on_param(s.b, 64, 0, 64, 0);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 61, 0x1234, kParam) == 0);
    CHECK(s.synth.param(vreg(PITCH, 1)) == 0x087A); // 0x10F4 at 24 kHz
    const auto [l, r] = volumes({64, 127}, 0);
    CHECK(s.synth.param(vreg(VOLL, 1)) == l);
    CHECK(s.synth.param(vreg(VOLR, 1)) == r);
    CHECK(r == 0);
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x2);
    // KeyOnByTone: the pitch relative to the tone's (0x1000 its own), times the rate; addPitch in 1/128 semitones.
    key_on_param(s.b, 127, 64, 64, 128 * 12);
    CHECK(s.call("cellSnd3KeyOnByTone", 0, 0, 0x1000, 0x55, kParam) == 0);
    CHECK(s.synth.param(vreg(PITCH, 2)) == 0x1000);
    key_on_param(s.b, 127, 64, 64, 0);
    CHECK(s.call("cellSnd3KeyOnByTone", 0, 0, 0x1000, 0x55, kParam) == 0);
    CHECK(s.synth.param(vreg(PITCH, 3)) == 0x0800);
    CHECK(s.call("cellSnd3KeyOnByTone", 0, 1, 0x3000, 0x55, kParam) == 0); // tone 1 at 48 kHz
    CHECK(s.synth.param(vreg(PITCH, 4)) == 0x3000);
    CHECK(s.call("cellSnd3KeyOnByTone", 0, 1, 0x7000, 0x55, kParam) == 0); // past the synthesizer's 0x3FFF
    CHECK(s.synth.param(vreg(PITCH, 5)) == 0x3FFF);
    // Tone 3: half volume, panned right by its own pan; tone 1 sends to the effect (its bits in VMIXEL/VMIXER).
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 3, 60, 0x66, 0) == 0);
    const auto [l3, r3] = volumes({127, 64}, 127);
    CHECK(s.synth.param(vreg(VOLL, 6)) == l3);
    CHECK(s.synth.param(vreg(VOLR, 6)) == r3);
    CHECK(l3 == 0);
    CHECK(s.synth.sw(sreg(VMIXEL, 0)) == ((1u << 4) | (1u << 5)));
    CHECK(s.synth.sw(sreg(VMIXER, 0)) == ((1u << 4) | (1u << 5)));
    // The key-on IDs: voices 0 and 1 have 0x1234, 2 to 5 0x55, 6 0x66.
    CHECK(s.call("cellSnd3GetVoiceByID", 0x1234, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{0, 1});
    CHECK(s.call("cellSnd3GetVoiceByID", 0x55, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{2, 3, 4, 5});
    CHECK(s.call("cellSnd3GetVoiceByID", 0x77, kBits) == 0);
    CHECK(voices_in(s.b).empty());
    CHECK(s.call("cellSnd3GetVoiceByID", 0x55, 0) == err(PARAM));
    CHECK(s.call("cellSnd3GetVoiceByID", 0x55, kNotGuest) == err(PARAM));
    // Voices past core 0's 24: core 1's registers.
    for (u32 i = 7; i < 26; i++) CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 0x99, 0) == 0);
    CHECK(s.synth.param(vreg(PITCH, 25)) == 0x0800);
    CHECK(vreg(PITCH, 25) == 0x0203);
    CHECK(s.synth.sw(sreg(KON, 1)) == 0x2);
    CHECK(s.status(25) == 1);
    CHECK(s.call("cellSnd3GetVoiceByID", 0x99, kBits) == 0);
    CHECK(ps_read32(s.b, kBits) == 0xFFFF80);
    CHECK(ps_read32(s.b, kBits + 4) == 0x3);
    CHECK(ps_read32(s.b, kBits + 8) == 0);
    CHECK(ps_read32(s.b, kBits + 12) == 0);
    // KeyOffByID: how many it keyed off, one KOFF write a core.
    s.synth.clear();
    CHECK(s.call("cellSnd3KeyOffByID", 0x99) == 19);
    CHECK(s.synth.sw(sreg(KOFF, 0)) == 0xFFFF80);
    CHECK(s.synth.sw(sreg(KOFF, 1)) == 0x3);
    CHECK(s.status(7) == 2);
    CHECK(s.call("cellSnd3KeyOffByID", 0x99) == 0); // already keyed off
    // The checks: the bank, the tone, the note, the key-on parameter.
    CHECK(s.call("cellSnd3NoteOnByTone", 1, 0, 60, 1, 0) == err(HD3ID));
    CHECK(s.call("cellSnd3NoteOnByTone", 99, 0, 60, 1, 0) == err(HD3ID));
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 2, 60, 1, 0) == err(PARAM)); // tone 2 is missing
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 4, 60, 1, 0) == err(PARAM));
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 128, 1, 0) == err(PARAM));
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 1, kNotGuest) == err(PARAM));
    for (const std::array<u8, 3> bad : {std::array<u8, 3>{128, 64, 64}, {127, 128, 64}, {127, 64, 128}}) {
        key_on_param(s.b, bad[0], bad[1], bad[2], 0);
        CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 1, kParam) == err(PARAM));
        CHECK(s.call("cellSnd3KeyOnByTone", 0, 0, 0x1000, 1, kParam) == err(PARAM));
    }
    CHECK(s.call("cellSnd3KeyOnByTone", 0, 2, 0x1000, 1, 0) == err(PARAM));
}

TEST_CASE("libsnd3: the request queue: front before the block, rear after, and full", "[hle][snd3]")
{
    Snd s;
    // Four front requests and two rear ones.
    ps_write32(s.b, kQueueCtx, kQueueFront);
    ps_write32(s.b, kQueueCtx + 4, 4 * 32 + 31);
    ps_write32(s.b, kQueueCtx + 8, kQueueRear);
    ps_write32(s.b, kQueueCtx + 12, 2 * 32);
    REQUIRE(s.init(8, kQueueCtx) == 0);
    REQUIRE(s.bind() == 0);
    s.synth.clear();
    // Key-ons wait for Synthesis; the voices are the library's at once.
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 1, 0) == 0);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 1, 0) == 0);
    CHECK(s.synth.count() == 0);
    CHECK(s.status(0) == 1);
    CHECK(s.status(1) == 1);
    CHECK(s.call("cellSnd3GetVoiceByID", 1, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{0, 1});
    // One of them keyed off before the block: it still sounds for that block (KON before Generate, KOFF after).
    CHECK(s.call("cellSnd3VoiceKeyOff", 1) == 0);
    CHECK(s.synth.count() == 0);
    CHECK(s.status(1) == 2);
    s.synth.set16(vreg(ENVX, 0), 0x4000);
    s.synth.set16(vreg(ENVX, 1), 0x2000);
    CHECK(s.synthesis() == 0);
    std::vector<std::string> order;
    for (const Call& c : s.synth.calls()) {
        if (c.fn == kGenerate) order.push_back("generate");
        if (c.fn == kSetSwitch && c.a[0] == sreg(KON, 0)) order.push_back(std::format("kon {:#x}", c.a[1]));
        if (c.fn == kSetSwitch && c.a[0] == sreg(KOFF, 0)) order.push_back(std::format("koff {:#x}", c.a[1]));
    }
    CHECK(order == std::vector<std::string>{"kon 0x3", "generate", "koff 0x2"});
    const std::vector<Call> generate = s.synth.calls(kGenerate);
    REQUIRE(generate.size() == 1);
    CHECK(generate[0].a == std::array<u32, 5>{kSamples, kOutL, kOutR, 0, 0});
    CHECK(s.call("cellSnd3VoiceGetEnvelope", 0) == 0x4000);
    CHECK(s.call("cellSnd3VoiceGetEnvelope", 1) == 0x2000);
    // A full front: REQUESTQUEFULL, the voices as they were; a full rear likewise.
    for (u32 i = 0; i < 4; i++) CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 2, 0) == 0);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 2, 0) == err(QUEUEFULL));
    CHECK(s.status(6) == 0);
    CHECK(s.call("cellSnd3VoiceSetPitch", 2, 5) == err(QUEUEFULL));
    CHECK(s.call("cellSnd3KeyOffByID", 2) == 0); // four key-offs for a rear of two: none
    CHECK(s.status(2) == 1);
    CHECK(s.call("cellSnd3VoiceKeyOff", 2) == 0);
    CHECK(s.call("cellSnd3VoiceKeyOff", 3) == 0);
    CHECK(s.call("cellSnd3VoiceKeyOff", 4) == err(QUEUEFULL));
    CHECK(s.status(4) == 1);
    s.synth.clear();
    CHECK(s.synthesis() == 0);
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x3C);
    CHECK(s.synth.sw(sreg(KOFF, 0)) == 0xC);
    // The key-off the full rear refused, made again with room: told after the next block.
    CHECK(s.call("cellSnd3VoiceKeyOff", 4) == 0);
    s.synth.clear();
    CHECK(s.synthesis() == 0);
    CHECK(s.synth.sw(sreg(KOFF, 0)) == 0x10);
    // Synthesis's buffers: guest memory for the block's samples.
    CHECK(s.call("cellSnd3Synthesis", 0, kOutR) == err(PARAM));
    CHECK(s.call("cellSnd3Synthesis", kOutL, kNotGuest) == err(PARAM));
    CHECK(s.call("cellSnd3SynthesisEx", kOutL, kOutR, 0, kOutRR) == err(PARAM));
    CHECK(s.call("cellSnd3SynthesisEx", kOutL, kOutR, kOutRL, kNotGuest) == err(PARAM));
    s.synth.clear();
    CHECK(s.call("cellSnd3SynthesisEx", kOutL, kOutR, kOutRL, kOutRR) == 0);
    REQUIRE(s.synth.count(kGenerate) == 1);
    CHECK(s.synth.calls(kGenerate)[0].a == std::array<u32, 5>{kSamples, kOutL, kOutR, kOutRL, kOutRR});
    // libsynth2's Generate failing: SYNTH.
    s.synth.answer(kGenerate, 0x80310202);
    CHECK(s.synthesis() == err(SYNTH));
    s.synth.answer(kGenerate, 0);
    // Exit drops what is queued.
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 3, 0) == 0);
    CHECK(s.call("cellSnd3Exit") == 0);
    s.synth.clear();
    CHECK(s.init(8) == 0); // no queue now: told at once
    CHECK(s.bind() == 0);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 3, 0) == 0);
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x1);
}

TEST_CASE("libsnd3: voices: allocation, stealing, reserve mode, holds and their status", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init(4) == 0);
    REQUIRE(s.bind() == 0);
    // Reserved voices are the game's: the library's key-ons take the others.
    CHECK(s.call("cellSnd3VoiceSetReserveMode", 0, 1) == 0);
    CHECK(s.call("cellSnd3VoiceSetReserveMode", 0, 2) == err(PARAM));
    CHECK(s.call("cellSnd3VoiceSetReserveMode", 4, 1) == err(VOICENUM));
    for (u32 i = 0; i < 3; i++) CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 10 + i, 0) == 0);
    CHECK(s.status(0) == 0);
    for (u32 n = 1; n < 4; n++) CHECK(s.status(n) == 1);
    // All busy at priority 0: a new note of priority 0 takes the oldest (voice 1, ID 10).
    s.synth.clear();
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 62, 20, 0) == 0);
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x2);
    CHECK(s.call("cellSnd3GetVoiceByID", 10, kBits) == 0);
    CHECK(voices_in(s.b).empty());
    CHECK(s.call("cellSnd3GetVoiceByID", 20, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{1});
    // A released voice goes before any playing one; a note of a higher priority (tone 1's 5) takes a playing one of a
    // lower; a note of priority 0 takes none of priority 5.
    CHECK(s.call("cellSnd3VoiceKeyOff", 3) == 0);
    s.synth.clear();
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 1, 72, 30, 0) == 0); // tone 1, priority 5: the released voice 3
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x8);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 1, 72, 31, 0) == 0); // the oldest of priority 0: voice 2
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x4);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 1, 72, 32, 0) == 0); // voice 1
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x2);
    s.synth.clear();
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 33, 0) == 0); // every voice priority 5: none
    CHECK(s.synth.count(kSetSwitch) == 0);
    CHECK(s.call("cellSnd3GetVoiceByID", 33, kBits) == 0);
    CHECK(voices_in(s.b).empty());
    // The Voice* key-ons play on a reserved voice only.
    CHECK(s.call("cellSnd3VoiceNoteOnByTone", 0, 1, 0, 60, 40, 0) == err(RESERVEDVOICE));
    CHECK(s.call("cellSnd3VoiceKeyOnByTone", 0, 1, 0, 0x1000, 40, 0) == err(RESERVEDVOICE));
    CHECK(s.call("cellSnd3VoiceNoteOnByTone", 0, 4, 0, 60, 40, 0) == err(VOICENUM));
    CHECK(s.call("cellSnd3VoiceNoteOnByTone", 1, 0, 0, 60, 40, 0) == err(HD3ID));
    CHECK(s.call("cellSnd3VoiceNoteOnByTone", 0, 0, 2, 60, 40, 0) == err(PARAM));
    CHECK(s.call("cellSnd3VoiceNoteOnByTone", 0, 0, 0, 128, 40, 0) == err(PARAM));
    s.synth.clear();
    CHECK(s.call("cellSnd3VoiceNoteOnByTone", 0, 0, 0, 84, 40, 0) == 0);
    CHECK(s.synth.param(vreg(PITCH, 0)) == 0x2000); // two octaves up at 24 kHz
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x1);
    CHECK(s.call("cellSnd3VoiceKeyOnByTone", 0, 0, 1, 0x0800, 41, 0) == 0); // the same voice, again
    CHECK(s.synth.param(vreg(PITCH, 0)) == 0x0800);
    CHECK(s.call("cellSnd3GetVoiceByID", 41, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{0});
    // Sustain hold: a held voice's key-off waits for the hold's end.
    s.synth.clear();
    CHECK(s.call("cellSnd3VoiceSetSustainHold", 0, 1) == 0);
    CHECK(s.call("cellSnd3VoiceKeyOff", 0) == 0);
    CHECK(s.status(0) == 1);
    CHECK(s.synth.count(kSetSwitch) == 0);
    CHECK(s.call("cellSnd3KeyOffByID", 41) == 0);
    CHECK(s.call("cellSnd3VoiceSetSustainHold", 0, 0) == 0);
    CHECK(s.status(0) == 2);
    CHECK(s.synth.sw(sreg(KOFF, 0)) == 0x1);
    CHECK(s.call("cellSnd3VoiceSetSustainHold", 9, 0) == err(VOICENUM));
    CHECK(s.call("cellSnd3VoiceKeyOff", 9) == err(VOICENUM));
    // VoiceAllKeyOff: every voice, held or not.
    CHECK(s.call("cellSnd3VoiceSetSustainHold", 1, 1) == 0);
    s.synth.clear();
    CHECK(s.call("cellSnd3VoiceAllKeyOff") == 0);
    CHECK(s.synth.sw(sreg(KOFF, 0)) == 0xE); // voice 0 was keyed off already
    for (u32 n = 0; n < 4; n++) CHECK(s.status(n) == 2);
    // After a block a silent released voice is free; a playing one whose envelope went to zero after a block too (its
    // one-shot sample ended); one keyed on in that block is not.
    s.synth.set16(vreg(ENVX, 0), 0);
    s.synth.set16(vreg(ENVX, 1), 0x100);
    CHECK(s.synthesis() == 0);
    CHECK(s.status(0) == 0);
    CHECK(s.status(1) == 2);
    CHECK(s.call("cellSnd3VoiceGetEnvelope", 1) == 0x100);
    CHECK(s.call("cellSnd3VoiceGetEnvelope", 0) == 0);
    CHECK(s.call("cellSnd3VoiceGetEnvelope", 4) == err(VOICENUM));
    CHECK(s.call("cellSnd3VoiceGetStatus", 4) == err(VOICENUM));
    CHECK(s.call("cellSnd3VoiceSetReserveMode", 0, 0) == 0);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 50, 0) == 0); // the free voice 0, no longer reserved
    CHECK(s.status(0) == 1);
    s.synth.set16(vreg(ENVX, 0), 0);
    CHECK(s.synthesis() == 0);
    CHECK(s.status(0) == 1); // keyed on in that block
    CHECK(s.synthesis() == 0);
    CHECK(s.status(0) == 0); // its sample ended
}

TEST_CASE("libsnd3: a voice's pitch, velocity, pans and bend told again", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init(8) == 0);
    REQUIRE(s.bind() == 0);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 72, 1, 0) == 0); // voice 0 at 0x1000
    s.synth.clear();
    CHECK(s.call("cellSnd3VoiceSetPitch", 0, -128 * 12) == 0); // an octave down
    CHECK(s.synth.param(vreg(PITCH, 0)) == 0x0800);
    CHECK(s.synth.param(vreg(VOLL, 0)) == 0x3FFF);
    CHECK(s.call("cellSnd3VoiceSetPitch", 0, 0) == 0);
    CHECK(s.call("cellSnd3VoiceSetVelocity", 0, 32) == 0);
    CHECK(s.synth.param(vreg(VOLL, 0)) == volumes({32, 127}, 64).first);
    CHECK(s.call("cellSnd3VoiceSetVelocity", 0, 128) == err(PARAM));
    CHECK(s.call("cellSnd3VoiceSetVelocity", 0, 127) == 0);
    CHECK(s.call("cellSnd3VoiceSetPanpot", 0, 96) == 0);
    CHECK(s.synth.param(vreg(VOLL, 0)) == volumes({127, 127}, 96).first);
    CHECK(s.synth.param(vreg(VOLR, 0)) == 0x3FFF);
    CHECK(s.call("cellSnd3VoiceSetPanpot", 0, 128) == err(PARAM));
    CHECK(s.call("cellSnd3VoiceSetPanpotEx", 0, 100) == 0); // kept: the registers as they were
    CHECK(s.synth.param(vreg(VOLL, 0)) == volumes({127, 127}, 96).first);
    CHECK(s.call("cellSnd3VoiceSetPanpotEx", 0, 128) == err(PARAM));
    // Pitch bend over two semitones (no split): 0x3FFF 255/128 semitones up, 0 two semitones down, at 24 kHz.
    const auto at_24k = [&](s32 fine) { return (s.call("cellSnd3Note2Pitch", 60, 0, 72, static_cast<u16>(fine)) + 1) / 2; };
    CHECK(s.call("cellSnd3VoiceSetPitchBend", 0, 0x3FFF) == 0);
    CHECK(s.synth.param(vreg(PITCH, 0)) == static_cast<s64>(at_24k(255)));
    CHECK(s.call("cellSnd3VoiceSetPitchBend", 0, 0) == 0);
    CHECK(s.synth.param(vreg(PITCH, 0)) == static_cast<s64>(at_24k(-256)));
    CHECK(s.call("cellSnd3VoiceSetPitchBend", 0, 0x2000) == 0);
    CHECK(s.synth.param(vreg(PITCH, 0)) == 0x1000);
    CHECK(s.call("cellSnd3VoiceSetPitchBend", 0, 0x4000) == err(PARAM));
    CHECK(s.call("cellSnd3VoiceSetPitchBend", 9, 0x2000) == err(VOICENUM));
    CHECK(s.call("cellSnd3VoiceSetPitch", 9, 0) == err(VOICENUM));
    // A free voice: accepted, nothing to tell.
    s.synth.clear();
    CHECK(s.call("cellSnd3VoiceSetPitch", 5, 100) == 0);
    CHECK(s.synth.count() == 0);
}

TEST_CASE("libsnd3: NoteOn plays a program's splits; NoteOff, GetVoice and the sustain hold", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init(8) == 0);
    REQUIRE(s.bind() == 0);
    for (u32 n = 0; n < 8; n++) s.synth.set16(vreg(ENVX, n), 0x7FFF); // every voice sounding
    const auto at_24k = [&](u32 note) { return static_cast<s64>((s.call("cellSnd3Note2Pitch", 60, 0, note, 0) + 1) / 2); };
    s.synth.clear();
    // Program 0: note 50 is tone 0's (VAG 0 at 24 kHz, base 60), note 72 tone 1's (VAG 1 at 48 kHz, base 72).
    CHECK(s.call("cellSnd3NoteOn", 0, 3, 0, 50, 0, 0, 7) == 0);
    CHECK(s.synth.param(vreg(PITCH, 0)) == at_24k(50));
    CHECK(s.synth.addr(vreg(SSA, 0)) == 0x10000);
    CHECK(s.call("cellSnd3NoteOn", 0, 3, 0, 72, 0, 0, 7) == 0);
    CHECK(s.synth.param(vreg(PITCH, 1)) == 0x1000);
    CHECK(s.synth.addr(vreg(SSA, 1)) == 0x10800);
    // Program 2: two layers on two voices, an octave up; tone 3 at half volume, all right.
    key_on_param(s.b, 100, 64, 64, 0);
    CHECK(s.call("cellSnd3NoteOn", 0, 3, 2, 60, 0, kParam, 7) == 0);
    CHECK(s.synth.param(vreg(PITCH, 2)) == 0x1000);
    CHECK(s.synth.param(vreg(PITCH, 3)) == 0x1000);
    CHECK(s.synth.param(vreg(VOLL, 2)) == volumes({100, 127}, 64).first);
    CHECK(s.synth.param(vreg(VOLL, 3)) == 0);
    CHECK(s.synth.param(vreg(VOLR, 3)) == volumes({100, 64}, 127).second);
    CHECK(s.call("cellSnd3GetVoice", 3, 7, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{0, 1, 2, 3});
    CHECK(s.call("cellSnd3GetVoice", 4, 7, kBits) == 0);
    CHECK(voices_in(s.b).empty());
    CHECK(s.call("cellSnd3GetVoice", 3, 8, kBits) == 0);
    CHECK(voices_in(s.b).empty());
    CHECK(s.call("cellSnd3GetVoice", 16, 7, kBits) == err(PARAM));
    CHECK(s.call("cellSnd3GetVoice", 3, 7, 0) == err(PARAM));
    // NoteOff: the channel's voices of that note and ID.
    s.synth.clear();
    CHECK(s.call("cellSnd3NoteOff", 3, 60, 8) == 0);
    CHECK(s.call("cellSnd3NoteOff", 4, 60, 7) == 0);
    CHECK(s.synth.count() == 0);
    CHECK(s.call("cellSnd3NoteOff", 3, 60, 7) == 0);
    CHECK(s.synth.sw(sreg(KOFF, 0)) == 0xC);
    CHECK(s.status(2) == 2);
    CHECK(s.status(0) == 1);
    CHECK(s.call("cellSnd3NoteOff", 16, 60, 7) == err(PARAM));
    CHECK(s.call("cellSnd3NoteOff", 3, 128, 7) == err(PARAM));
    // The checks: the bank, the channel, the program, the note, the parameter.
    CHECK(s.call("cellSnd3NoteOn", 1, 3, 0, 60, 0, 0, 7) == err(HD3ID));
    CHECK(s.call("cellSnd3NoteOn", 0, 16, 0, 60, 0, 0, 7) == err(PARAM));
    CHECK(s.call("cellSnd3NoteOn", 0, 3, 1, 60, 0, 0, 7) == err(PARAM)); // program 1 is missing
    CHECK(s.call("cellSnd3NoteOn", 0, 3, 3, 60, 0, 0, 7) == err(PARAM));
    CHECK(s.call("cellSnd3NoteOn", 0, 3, 0, 128, 0, 0, 7) == err(PARAM));
    CHECK(s.call("cellSnd3NoteOn", 0, 3, 0, 60, 0, kNotGuest, 7) == err(PARAM));
    // Sustain: held voices' NoteOff waits for SetSustainHold's end, of that channel and ID only.
    s.synth.clear();
    CHECK(s.call("cellSnd3NoteOn", 0, 5, 0, 50, 1, 0, 9) == 0); // voice 4, held
    CHECK(s.call("cellSnd3NoteOff", 5, 50, 9) == 0);
    CHECK(s.status(4) == 1);
    CHECK(s.call("cellSnd3SetSustainHold", 5, 0, 10) == 0);
    CHECK(s.call("cellSnd3SetSustainHold", 6, 0, 9) == 0);
    CHECK(s.status(4) == 1);
    CHECK(s.synth.sw(sreg(KOFF, 0)) == -1);
    CHECK(s.call("cellSnd3SetSustainHold", 5, 0, 9) == 0);
    CHECK(s.status(4) == 2);
    CHECK(s.synth.sw(sreg(KOFF, 0)) == 0x10);
    // A hold set afterwards on a voice keyed without one.
    CHECK(s.call("cellSnd3NoteOn", 0, 5, 0, 52, 0, 0, 9) == 0); // the free voice 5
    CHECK(s.call("cellSnd3SetSustainHold", 5, 1, 9) == 0);
    CHECK(s.call("cellSnd3NoteOff", 5, 52, 9) == 0);
    CHECK(s.status(5) == 1);
    CHECK(s.call("cellSnd3SetSustainHold", 16, 1, 9) == err(PARAM));
}

TEST_CASE("libsnd3: a program's and a split's volume, pan, transpose, detune, bend range and velocities", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init(8) == 0);
    BankSpec b = usual_bank();
    b.tones.push_back(ToneSpec{.vag = 1, .vel_low = 0, .vel_high = 63, .base_note = 60, .detune = 32});
    b.tones.push_back(ToneSpec{.vag = 1, .vel_low = 64, .vel_high = 127, .base_note = 60, .volume = 100});
    b.programs.push_back(ProgramSpec{.volume = 64, .pan = 32, .transpose = -12, .detune = 16,
        .splits = {{.tone = 4, .bend_low = 128 * 12, .bend_high = 128 * 7, .volume = 120, .pan = 80, .transpose = 2,
                       .detune = 8},
            {.tone = 5, .volume = 110}}});
    REQUIRE(s.bind(b) == 0);
    // Velocity 40: only tone 4's layer (0-63). Note 70: up two, down twelve: 60; fine 32 + 16 + 8.
    key_on_param(s.b, 40, 64, 64, 0);
    CHECK(s.call("cellSnd3NoteOn", 0, 0, 3, 70, 0, kParam, 1) == 0);
    CHECK(s.call("cellSnd3GetVoiceByID", 1, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{0});
    CHECK(s.synth.param(vreg(PITCH, 0)) == static_cast<s64>(s.call("cellSnd3Note2Pitch", 60, 0, 60, 56)));
    const auto [l, r] = volumes({40, 127, 64, 120}, 64 + (32 - 64) + (80 - 64));
    CHECK(s.synth.param(vreg(VOLL, 0)) == l);
    CHECK(s.synth.param(vreg(VOLR, 0)) == r);
    // Its split's bend range: up seven semitones, down twelve.
    CHECK(s.call("cellSnd3VoiceSetPitchBend", 0, 0x3FFF) == 0);
    CHECK(s.synth.param(vreg(PITCH, 0)) ==
        static_cast<s64>(s.call("cellSnd3Note2Pitch", 60, 0, 60, 56 + (0x1FFF * 128 * 7) / 0x2000)));
    CHECK(s.call("cellSnd3VoiceSetPitchBend", 0, 0) == 0);
    CHECK(s.synth.param(vreg(PITCH, 0)) == static_cast<s64>(s.call("cellSnd3Note2Pitch", 60, 0, 48, 56)));
    // Velocity 100: tone 5's layer only.
    key_on_param(s.b, 100, 64, 64, 0);
    CHECK(s.call("cellSnd3NoteOn", 0, 0, 3, 70, 0, kParam, 2) == 0);
    CHECK(s.call("cellSnd3GetVoiceByID", 2, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{1});
    CHECK(s.synth.param(vreg(VOLL, 1)) == volumes({100, 100, 64, 110}, 32).first);
}

TEST_CASE("libsnd3: the effect and the output mode", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init(8) == 0);
    REQUIRE(s.bind() == 0);
    s.synth.clear();
    // A hall on both cores, returned at 0x3000: libsynth2's attributes (the mode with CLEAR) and each core's effect bit.
    CHECK(s.call("cellSnd3SetEffectType", 5, 0x3000, 0, 0) == 0);
    const std::vector<Call> attrs = s.synth.calls(kSetEffectAttr);
    REQUIRE(attrs.size() == 2);
    for (u32 c = 0; c < 2; c++) {
        INFO("core " << c);
        CHECK(attrs[c].a[0] == c);
        CHECK(attrs[c].x0 == (c << 16 | 0x105)); // core, mode | SD_EFFECT_MODE_CLEAR
        CHECK(attrs[c].x1 == 0x30003000);        // depth_L, depth_R
        CHECK(attrs[c].a[2] == 0);               // delay, feedback
    }
    const std::vector<Call> cores = s.synth.calls(kSetCoreAttr);
    REQUIRE(cores.size() == 2);
    CHECK(cores[0].a[0] == kEffectEnable);
    CHECK(cores[0].a[1] == 1);
    CHECK(cores[1].a[0] == (kEffectEnable | 1));
    // An echo with its delay and feedback; off again; the checks.
    s.synth.clear();
    CHECK(s.call("cellSnd3SetEffectType", 7, static_cast<u16>(-0x1000), 64, 127) == 0);
    REQUIRE(s.synth.count(kSetEffectAttr) == 2);
    CHECK(s.synth.calls(kSetEffectAttr)[1].x1 == 0xF000F000);
    CHECK(s.synth.calls(kSetEffectAttr)[1].a[2] == (64u << 16 | 127));
    s.synth.clear();
    CHECK(s.call("cellSnd3SetEffectType", 0, 0, 0, 0) == 0);
    REQUIRE(s.synth.count(kSetCoreAttr) == 2);
    CHECK(s.synth.calls(kSetCoreAttr)[0].a[1] == 0);
    CHECK(s.call("cellSnd3SetEffectType", 10, 0, 0, 0) == err(PARAM));
    CHECK(s.call("cellSnd3SetEffectType", 5, 0, 128, 0) == err(PARAM));
    CHECK(s.call("cellSnd3SetEffectType", 5, 0, 0, 128) == err(PARAM));
    // Mono: both sides at the voice's full volume, the voices playing told again; stereo back; no third mode.
    key_on_param(s.b, 127, 0, 64, 0);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 0, 60, 1, kParam) == 0);
    CHECK(s.synth.param(vreg(VOLR, 0)) == 0);
    CHECK(s.call("cellSnd3SetOutputMode", 1) == 0);
    CHECK(s.synth.param(vreg(VOLL, 0)) == 0x3FFF);
    CHECK(s.synth.param(vreg(VOLR, 0)) == 0x3FFF);
    CHECK(s.call("cellSnd3NoteOnByTone", 0, 3, 60, 1, kParam) == 0); // tone 3, half volume, all right: both sides
    CHECK(s.synth.param(vreg(VOLL, 1)) == s.synth.param(vreg(VOLR, 1)));
    CHECK(s.synth.param(vreg(VOLL, 1)) == volumes({127, 64}, 64).first);
    CHECK(s.call("cellSnd3SetOutputMode", 0) == 0);
    CHECK(s.synth.param(vreg(VOLR, 0)) == 0);
    CHECK(s.call("cellSnd3SetOutputMode", 2) == err(OUTPUTMODE));
    // With a request queue the effect waits for the next block too.
    CHECK(s.call("cellSnd3Exit") == 0);
    ps_write32(s.b, kQueueCtx, kQueueFront);
    ps_write32(s.b, kQueueCtx + 4, 0x100);
    ps_write32(s.b, kQueueCtx + 8, kQueueRear);
    ps_write32(s.b, kQueueCtx + 12, 0x100);
    REQUIRE(s.init(8, kQueueCtx) == 0);
    s.synth.clear();
    CHECK(s.call("cellSnd3SetEffectType", 1, 0x1000, 0, 0) == 0);
    CHECK(s.synth.count() == 0);
    CHECK(s.synthesis() == 0);
    CHECK(s.synth.count(kSetEffectAttr) == 2);
}

namespace {

// Runs `blocks` Synthesis calls: each key-on and key-off libsynth2 was told, as "block on|off core:bits", a key-on
// after its block's Generate or a key-off before it marked.
std::vector<std::string> run_blocks(Snd& s, u32 blocks, u32 first = 0)
{
    std::vector<std::string> out;
    for (u32 i = 0; i < blocks; i++) {
        s.synth.clear();
        REQUIRE(s.synthesis() == 0);
        bool generated = false;
        for (const Call& c : s.synth.calls()) {
            if (c.fn == kGenerate) generated = true;
            if (c.fn != kSetSwitch) continue;
            const u32 param = c.a[0] >> 8, core = c.a[0] & 1;
            if (param == KON)
                out.push_back(std::format("{} on {}:{:#x}{}", first + i, core, c.a[1], generated ? " late" : ""));
            if (param == KOFF)
                out.push_back(std::format("{} off {}:{:#x}{}", first + i, core, c.a[1], generated ? "" : " early"));
        }
    }
    return out;
}

std::string joined(const std::vector<std::string>& v)
{
    std::string out;
    for (const std::string& x : v) out += (out.empty() ? "" : ", ") + x;
    return out;
}

} // namespace

TEST_CASE("libsnd3: SMFs bound and refused", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init(8) == 0);
    REQUIRE(s.bind() == 0);
    place(s.b, kSmfData, usual_song());
    CHECK(s.call("cellSnd3SMFBind", 0, kSmfData, 0) == err(SMFCTX));
    CHECK(s.call("cellSnd3SMFBind", kNotGuest, kSmfData, 0) == err(SMFCTX));
    CHECK(s.call("cellSnd3SMFBind", kSmfCtx, 0, 0) == err(PARAM));
    CHECK(s.call("cellSnd3SMFBind", kSmfCtx, kSmfData, 1) == err(HD3ID));
    // Sixteen at once, from 0; then SMFFULL.
    for (u32 i = 0; i < 16; i++) CHECK(s.call("cellSnd3SMFBind", i & 1 ? kSmfCtx2 : kSmfCtx, kSmfData, 0) == i);
    CHECK(s.call("cellSnd3SMFBind", kSmfCtx, kSmfData, 0) == err(SMFFULL));
    for (u32 i = 0; i < 16; i++) CHECK(s.call("cellSnd3SMFUnbind", i) == 0);
    // Every SMF call on an ID not bound: SMFID.
    const std::vector<std::pair<const char*, std::vector<u64>>> calls = {{"cellSnd3SMFUnbind", {}},
        {"cellSnd3SMFPlay", {127, 64, 1}}, {"cellSnd3SMFPlayEx", {127, 64, 64, 1}}, {"cellSnd3SMFPause", {}},
        {"cellSnd3SMFResume", {}}, {"cellSnd3SMFStop", {}}, {"cellSnd3SMFAddTempo", {1}}, {"cellSnd3SMFGetTempo", {}},
        {"cellSnd3SMFSetPlayVelocity", {1}}, {"cellSnd3SMFGetPlayVelocity", {}}, {"cellSnd3SMFSetPlayPanpot", {1}},
        {"cellSnd3SMFSetPlayPanpotEx", {1}}, {"cellSnd3SMFGetPlayPanpot", {}}, {"cellSnd3SMFGetPlayPanpotEx", {}},
        {"cellSnd3SMFGetPlayStatus", {}}, {"cellSnd3SMFSetPlayChannel", {1}}, {"cellSnd3SMFGetPlayChannel", {kWord}},
        {"cellSnd3SMFGetKeyOnID", {0, kWord}}};
    for (const u32 id : {0u, 16u, 0xFFFFFFFFu})
        for (const auto& [name, rest] : calls) {
            INFO(name << " " << id);
            const Function& fn = GuestThread::find(name);
            s.g.ctx.gpr[3] = id;
            for (usize i = 0; i < rest.size(); i++) s.g.ctx.gpr[4 + i] = rest[i];
            fn.fn(s.g.ctx, s.b);
            CHECK(s.g.ctx.gpr[3] == err(SMFID));
        }
    // Files that are not SMFs this player plays: SMF.
    const auto refused = [&](const std::vector<u8>& smf, const char* why) {
        INFO(why);
        place(s.b, kSmfData, smf);
        CHECK(s.call("cellSnd3SMFBind", kSmfCtx, kSmfData, 0) == err(SMF));
    };
    const std::vector<u8> good = usual_song();
    {
        auto d = good;
        d[0] = 'R';
        refused(d, "no MThd");
    }
    {
        auto d = good;
        d[7] = 5;
        refused(d, "a header of 5 bytes");
    }
    {
        auto d = good;
        d[9] = 2;
        refused(d, "format 2");
    }
    {
        auto d = good;
        d[11] = 0;
        refused(d, "no tracks");
    }
    {
        auto d = good;
        d[11] = 3;
        place(s.b, kSmfData + static_cast<u32>(d.size()), std::vector<u8>(64, 0));
        refused(d, "a track missing");
    }
    {
        auto d = good;
        d[12] = 0;
        d[13] = 0;
        refused(d, "division 0");
    }
    {
        auto d = good;
        d[12] = 0xE7; // -25 fps
        d[13] = 40;
        refused(d, "SMPTE time");
    }
    Track bad;
    bad.at(0, {60, 100}).end(0);
    refused(build_smf(0, 96, {bad}), "a data byte with no running status");
    Track bad2;
    bad2.d = {0x81, 0x81, 0x81, 0x81, 0x01, 0x90, 60, 100};
    refused(build_smf(0, 96, {bad2}), "a delta time of five bytes");
    Track bad3;
    bad3.at(0, {0xF1, 0x00}).end(0);
    refused(build_smf(0, 96, {bad3}), "a system message in a track");
    Track bad4;
    bad4.at(0, {0x90, 60});
    refused(build_smf(0, 96, {bad4}), "an event cut short");
    Track bad5;
    bad5.at(0, {0xFF, 0x01, 0x40, 'x'});
    refused(build_smf(0, 96, {bad5}), "a meta event past its track");
    Track bad6;
    bad6.at(0, {0x90, 60, 0x80}).end(0);
    refused(build_smf(0, 96, {bad6}), "a data byte with its high bit set");
    // Accepted: format 0, a chunk that is not a track skipped, running status, system exclusive, a track without its
    // end, the bytes past an end of track left alone.
    Track ok;
    ok.at(0, {0xF0, 0x03, 0x7E, 0x7F, 0xF7}).at(0, {0x90, 60, 100}).at(10, {62, 100}).end(10).at(0, {0xF1});
    std::vector<u8> d = build_smf(0, 96, {ok});
    const std::vector<u8> junk = {'J', 'U', 'N', 'K', 0, 0, 0, 2, 1, 2};
    d.insert(d.begin() + 14, junk.begin(), junk.end());
    place(s.b, kSmfData, d);
    CHECK(s.call("cellSnd3SMFBind", kSmfCtx, kSmfData, 0) == 0);
    Track open;
    open.at(0, {0x90, 60, 100}).at(5, {0x80, 60, 0});
    place(s.b, kSmfData, build_smf(0, 96, {open}));
    CHECK(s.call("cellSnd3SMFBind", kSmfCtx, kSmfData, 0) == 1);
    CHECK(s.call("cellSnd3SMFGetPlayStatus", 1) == 0);
    CHECK(s.call("cellSnd3SMFGetTempo", 1) == 120);
}

TEST_CASE("libsnd3: an SMF's timing, tempo, loops, pause and stop, block by block", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init(8) == 0);
    REQUIRE(s.bind() == 0);
    for (u32 n = 0; n < 8; n++) s.synth.set16(vreg(ENVX, n), 0x7FFF);
    place(s.b, kSmfData, usual_song());
    REQUIRE(s.call("cellSnd3SMFBind", kSmfCtx, kSmfData, 0) == 0);
    CHECK(s.call("cellSnd3SMFGetTempo", 0) == 120);
    // Nothing plays before SMFPlay.
    CHECK(run_blocks(s, 2).empty());
    // Played once: a tick is 50 samples, a block 256: tick 0 in block 0, tick 240 (12000) in block 46, 480 (24000) in
    // 93, 720 (36000) in 140, the end at 960 (48000) in 187. A key-on before its block, a key-off after it.
    CHECK(s.call("cellSnd3SMFPlay", 0, 127, 64, 1) == 0);
    CHECK(s.call("cellSnd3SMFGetPlayStatus", 0) == 1);
    CHECK(joined(run_blocks(s, 190)) == "0 on 0:0x1, 46 off 0:0x1, 93 on 0:0x2, 140 off 0:0x2");
    CHECK(s.call("cellSnd3SMFGetPlayStatus", 0) == 0);
    // The notes' key-on ID: channel 0's.
    CHECK(s.call("cellSnd3SMFGetKeyOnID", 0, 0, kWord) == 0);
    CHECK(ps_read32(s.b, kWord) == 0x80000000u);
    CHECK(s.call("cellSnd3SMFGetKeyOnID", 0, 15, kWord) == 0);
    CHECK(ps_read32(s.b, kWord) == 0x8000000Fu);
    CHECK(s.call("cellSnd3SMFGetKeyOnID", 0, 16, kWord) == err(PARAM));
    CHECK(s.call("cellSnd3SMFGetKeyOnID", 0, 0, 0) == err(PARAM));

    // Twice, the second from the first's end (48000 samples): its events at 48000, 60000, 72000 and 84000 samples
    // (blocks 187, 234, 281, 328), its end at 96000 (block 375).
    CHECK(s.call("cellSnd3SMFPlay", 0, 127, 64, 2) == 0);
    CHECK(joined(run_blocks(s, 375)) == "0 on 0:0x4, 46 off 0:0x4, 93 on 0:0x8, 140 off 0:0x8, 187 on 0:0x10, "
                                       "234 off 0:0x10, 281 on 0:0x20, 328 off 0:0x20");
    CHECK(s.call("cellSnd3SMFGetPlayStatus", 0) == 1);
    run_blocks(s, 1);
    CHECK(s.call("cellSnd3SMFGetPlayStatus", 0) == 0);
    // For ever (0): still playing after three times.
    CHECK(s.call("cellSnd3SMFPlay", 0, 127, 64, 0) == 0);
    run_blocks(s, 3 * 188);
    CHECK(s.call("cellSnd3SMFGetPlayStatus", 0) == 1);
    // Stop: keyed off, back to the start: the next Play from tick 0.
    CHECK(s.call("cellSnd3SMFStop", 0) == 0);
    CHECK(s.call("cellSnd3SMFGetPlayStatus", 0) == 0);

    // A tempo of 240 BPM (250000 us a quarter note), written in the song: everything in half the time.
    place(s.b, kSmfData, usual_song(250000));
    REQUIRE(s.call("cellSnd3SMFBind", kSmfCtx2, kSmfData, 0) == 1);
    CHECK(s.call("cellSnd3SMFGetTempo", 1) == 240);
    CHECK(s.call("cellSnd3SMFPlay", 1, 127, 64, 1) == 0);
    std::vector<std::string> ev = run_blocks(s, 100);
    REQUIRE(ev.size() == 4);
    CHECK(ev[1].starts_with("23 off"));
    CHECK(ev[2].starts_with("46 on"));
    CHECK(ev[3].starts_with("70 off"));
    // SMFAddTempo: 120 BPM more on the first song (240), added up; the tempo never below 1 BPM.
    CHECK(s.call("cellSnd3SMFUnbind", 1) == 0);
    CHECK(s.call("cellSnd3SMFAddTempo", 0, 60) == 0);
    CHECK(s.call("cellSnd3SMFAddTempo", 0, 60) == 0);
    CHECK(s.call("cellSnd3SMFGetTempo", 0) == 240);
    CHECK(s.call("cellSnd3SMFPlay", 0, 127, 64, 1) == 0);
    ev = run_blocks(s, 100);
    REQUIRE(ev.size() == 4);
    CHECK(ev[2].starts_with("46 on"));
    CHECK(s.call("cellSnd3SMFAddTempo", 0, -1000) == 0);
    CHECK(s.call("cellSnd3SMFGetTempo", 0) == 1);
    CHECK(s.call("cellSnd3SMFAddTempo", 0, 880) == 0);
    CHECK(s.call("cellSnd3SMFGetTempo", 0) == 120);
    // The tempo changed between two blocks: 47 blocks at 120 BPM (tick 240.64), then 240 BPM (25 samples a tick):
    // tick 480 at 12032 + 239.36 * 25 = 18016 samples, block 70.
    CHECK(s.call("cellSnd3SMFPlay", 0, 127, 64, 1) == 0);
    ev = run_blocks(s, 47);
    CHECK(s.call("cellSnd3SMFAddTempo", 0, 120) == 0);
    ev = run_blocks(s, 100, 47);
    REQUIRE(ev.size() >= 2);
    CHECK(ev[0].starts_with("70 on"));
    CHECK(s.call("cellSnd3SMFAddTempo", 0, -120) == 0);

    // Pause and Resume: the time paused does not count; Pause keys the SMF's voices off at once (no request queue).
    CHECK(s.call("cellSnd3SMFPlay", 0, 127, 64, 1) == 0);
    ev = run_blocks(s, 10);
    REQUIRE(ev.size() == 1);
    s.synth.clear();
    CHECK(s.call("cellSnd3SMFPause", 0) == 0);
    CHECK(s.synth.sw(sreg(KOFF, 0)) > 0);
    CHECK(s.call("cellSnd3SMFGetPlayStatus", 0) == 2);
    CHECK(s.call("cellSnd3SMFPause", 0) == 0);
    CHECK(run_blocks(s, 20, 10).empty());
    CHECK(s.call("cellSnd3SMFResume", 0) == 0);
    CHECK(s.call("cellSnd3SMFGetPlayStatus", 0) == 1);
    // The note off at tick 240 finds its voice keyed off already; the next note, at 24000 samples of the song, comes
    // in block 113: 93 and the 20 paused.
    ev = run_blocks(s, 100, 30);
    REQUIRE(!ev.empty());
    CHECK(ev[0].starts_with("113 on"));
    CHECK(s.call("cellSnd3SMFResume", 0) == 0);
    CHECK(s.call("cellSnd3SMFStop", 0) == 0);
    CHECK(s.call("cellSnd3SMFUnbind", 0) == 0);
    CHECK(s.call("cellSnd3SMFUnbind", 0) == err(SMFID));
}

TEST_CASE("libsnd3: an SMF's velocity, pans, channels, controllers and the voices it plays", "[hle][snd3]")
{
    Snd s;
    REQUIRE(s.init(8) == 0);
    REQUIRE(s.bind() == 0);
    for (u32 n = 0; n < 8; n++) s.synth.set16(vreg(ENVX, n), 0x7FFF);
    // Channel 0: a note at tick 0, then volume 64, pan 0, expression 100, a bend to the top, sustain on, its note off,
    // sustain off; channel 1: program 2 and its note; channel 2: a note.
    Track t;
    t.at(0, {0x90, 72, 100}).at(0, {0xC1, 2}).at(0, {0x91, 60, 127}).at(0, {0x92, 50, 90});
    t.at(48, {0xB0, 7, 64}).at(48, {0xB0, 10, 0}).at(48, {0xB0, 11, 100}).at(48, {0xE0, 0x7F, 0x7F});
    t.at(48, {0xB0, 64, 127}).at(48, {0x80, 72, 0}).at(48, {0xB0, 64, 0}).end(48);
    place(s.b, kSmfData, build_smf(0, 480, {t}));
    REQUIRE(s.call("cellSnd3SMFBind", kSmfCtx, kSmfData, 0) == 0);
    CHECK(s.call("cellSnd3SMFPlayEx", 0, 64, 64, 30, 1) == 0);
    CHECK(s.call("cellSnd3SMFGetPlayVelocity", 0) == 64);
    CHECK(s.call("cellSnd3SMFGetPlayPanpot", 0) == 64);
    CHECK(s.call("cellSnd3SMFGetPlayPanpotEx", 0) == 30);
    s.synth.clear();
    REQUIRE(s.synthesis() == 0);
    // Voice 0: channel 0's note 72, tone 1 at its base note; velocity 100, channel volume 100 (MIDI's default), play
    // velocity 64. Voices 1 and 2: channel 1's program 2 (two layers); voice 3 channel 2's note 50 (tone 0).
    CHECK(s.synth.param(vreg(PITCH, 0)) == 0x1000);
    CHECK(s.synth.param(vreg(VOLL, 0)) == volumes({100, 127, 127, 127, 100, 127, 64}, 64).first);
    CHECK(s.synth.sw(sreg(KON, 0)) == 0xF);
    CHECK(s.call("cellSnd3GetVoice", 0, 0x80000000u, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{0});
    CHECK(s.call("cellSnd3GetVoice", 1, 0x80000001u, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{1, 2});
    CHECK(s.call("cellSnd3GetVoice", 2, 0x80000002u, kBits) == 0);
    CHECK(voices_in(s.b) == std::vector<u32>{3});
    // The play velocity changed: every voice of the SMF told its volume again.
    s.synth.clear();
    CHECK(s.call("cellSnd3SMFSetPlayVelocity", 0, 127) == 0);
    CHECK(s.call("cellSnd3SMFGetPlayVelocity", 0) == 127);
    CHECK(s.synth.param(vreg(VOLL, 0)) == volumes({100, 127, 127, 127, 100, 127, 127}, 64).first);
    CHECK(s.synth.count(kSetParam) == 4 * 3);
    CHECK(s.call("cellSnd3SMFSetPlayVelocity", 0, 128) == err(PARAM));
    // The play pan: all left, the voices told again.
    CHECK(s.call("cellSnd3SMFSetPlayPanpot", 0, 0) == 0);
    CHECK(s.call("cellSnd3SMFGetPlayPanpot", 0) == 0);
    CHECK(s.synth.param(vreg(VOLR, 0)) == 0);
    CHECK(s.call("cellSnd3SMFSetPlayPanpot", 0, 64) == 0);
    CHECK(s.call("cellSnd3SMFSetPlayPanpot", 0, 128) == err(PARAM));
    CHECK(s.call("cellSnd3SMFSetPlayPanpotEx", 0, 127) == 0);
    CHECK(s.call("cellSnd3SMFGetPlayPanpotEx", 0) == 127);
    CHECK(s.call("cellSnd3SMFSetPlayPanpotEx", 0, 128) == err(PARAM));
    // Channel 2 taken out: its voice keyed off at once.
    s.synth.clear();
    CHECK(s.call("cellSnd3SMFSetPlayChannel", 0, 0xFFFB) == 0);
    CHECK(s.synth.sw(sreg(KOFF, 0)) == 0x8);
    CHECK(s.call("cellSnd3SMFGetPlayChannel", 0, kWord) == 0);
    CHECK(ps_read32(s.b, kWord) == 0xFFFB);
    CHECK(s.call("cellSnd3SMFGetPlayChannel", 0, 0) == err(PARAM));
    CHECK(s.call("cellSnd3SMFSetPlayChannel", 0, 0x10000) == err(PARAM));
    // The controllers, a tick of 50 samples: volume 64 at tick 48 (block 9), pan 0 at 96 (block 18), expression 100 at
    // 144 (block 28), the bend at 192 (block 37), sustain at 240 (46), the note's off at 288 (56) held, sustain off at
    // 336 (65) keys it off.
    run_blocks(s, 8, 1);
    s.synth.clear();
    REQUIRE(s.synthesis() == 0); // block 9
    CHECK(s.synth.param(vreg(VOLL, 0)) == volumes({100, 127, 127, 127, 64, 127, 127}, 64).first);
    run_blocks(s, 8, 10);
    s.synth.clear();
    REQUIRE(s.synthesis() == 0); // block 18
    CHECK(s.synth.param(vreg(VOLR, 0)) == 0);
    CHECK(s.synth.param(vreg(VOLL, 0)) == volumes({100, 127, 127, 127, 64, 127, 127}, 0).first);
    run_blocks(s, 9, 19);
    s.synth.clear();
    REQUIRE(s.synthesis() == 0); // block 28
    CHECK(s.synth.param(vreg(VOLL, 0)) == volumes({100, 127, 127, 127, 64, 100, 127}, 0).first);
    run_blocks(s, 8, 29);
    s.synth.clear();
    REQUIRE(s.synthesis() == 0); // block 37: two semitones up (0x3FFF of 0x2000, 255/128)
    CHECK(s.synth.param(vreg(PITCH, 0)) == static_cast<s64>(s.call("cellSnd3Note2Pitch", 72, 0, 72, 255)));
    CHECK(joined(run_blocks(s, 30, 38)) == "65 off 0:0x1");
    CHECK(s.status(0) == 2);
    CHECK(s.status(1) == 1);
    // RPN 0: a bend range of 12 semitones; a program of bank 1, which the bank does not have, not played (logged).
    Track r;
    r.at(0, {0xB0, 101, 0}).at(0, {0xB0, 100, 0}).at(0, {0xB0, 6, 12}).at(0, {0xE0, 0x7F, 0x7F}).at(0, {0x90, 50, 100});
    r.at(0, {0xB1, 0, 1}).at(0, {0x91, 50, 100}).end(10);
    place(s.b, kSmfData, build_smf(0, 480, {r}));
    REQUIRE(s.call("cellSnd3SMFBind", kSmfCtx2, kSmfData, 0) == 1);
    CHECK(s.call("cellSnd3VoiceAllKeyOff") == 0);
    for (u32 n = 0; n < 8; n++) s.synth.set16(vreg(ENVX, n), 0);
    REQUIRE(s.synthesis() == 0); // every voice free
    for (u32 n = 0; n < 8; n++) s.synth.set16(vreg(ENVX, n), 0x7FFF);
    CHECK(s.call("cellSnd3SMFPlay", 1, 127, 64, 1) == 0);
    s.synth.clear();
    REQUIRE(s.synthesis() == 0);
    CHECK(s.synth.sw(sreg(KON, 0)) == 0x1);
    CHECK(s.synth.param(vreg(PITCH, 0)) ==
        static_cast<s64>((s.call("cellSnd3Note2Pitch", 60, 0, 50, (0x1FFF * 128 * 12) / 0x2000) + 1) / 2));
}

namespace {

// pspocket's libsynth2 under libsnd3, the VAGs at 48 kHz in sound memory from 0x10000: VAG 0 vag_writer's looped sine
// (500 Hz at its own rate, 96 samples a period), VAG 1 a one-shot sine of 280 samples; tone 0 the loop, tone 1 the
// one-shot, tone 2 the loop sent to the effect; program 0 tone 0 over every note.
struct Real {
    Fixture f;
    GuestThread g{f, 0, "game"};
    u8* b = f.base();
    std::vector<float> left, right;

    template <class... A>
    u64 call(const char* name, A... args)
    {
        return g.call(name, args...);
    }

    Real()
    {
        REQUIRE(registry().find("libsynth2", recomp::nid_of("cellSoundSynth2Init")) != nullptr);
        REQUIRE(call("cellSnd3Init", 8, kSamples, 0) == 0);
        const std::vector<u8> loop = vag::looped_sine();
        const std::vector<u8> once = vag::encode(vag::sine(280, 4, 16000), 0, 0, 1);
        std::memcpy(b + kVagSrc, loop.data(), loop.size());
        std::memcpy(b + kVagSrc + 0x400, once.data(), once.size());
        REQUIRE(call("cellSoundSynth2VoiceTrans", 0, 0, kVagSrc, 0x10000, 0x800) == 0x800);
        BankSpec spec;
        spec.vags = {{0, 48000, static_cast<u32>(loop.size())}, {0x400, 48000, static_cast<u32>(once.size())}};
        spec.tones = {ToneSpec{.vag = 0}, ToneSpec{.vag = 1}, ToneSpec{.vag = 0, .spu_attr = 0x0C}};
        spec.programs = {ProgramSpec{.splits = {{.tone = 0}}}};
        place(b, kHd3, build_hd3(spec));
        REQUIRE(call("cellSnd3BindSoundData", kDataCtx, kHd3, 0x10000) == 0);
    }

    // `blocks` blocks, their samples kept.
    void run(u32 blocks)
    {
        left.clear();
        right.clear();
        for (u32 i = 0; i < blocks; i++) {
            REQUIRE(call("cellSnd3Synthesis", kOutL, kOutR) == 0);
            for (u32 n = 0; n < kSamples; n++) {
                left.push_back(ps_readf32(b, kOutL + 4 * n));
                right.push_back(ps_readf32(b, kOutR + 4 * n));
            }
        }
    }
    static double rms(const std::vector<float>& v, usize from = 0)
    {
        double sum = 0;
        for (usize i = from; i < v.size(); i++) sum += double(v[i]) * double(v[i]);
        return v.size() > from ? std::sqrt(sum / double(v.size() - from)) : 0;
    }
    static float peak(const std::vector<float>& v, usize from = 0)
    {
        float m = 0;
        for (usize i = from; i < v.size(); i++) m = std::max(m, std::fabs(v[i]));
        return m;
    }
    static u32 rising(const std::vector<float>& v, usize from = 0)
    {
        u32 n = 0;
        for (usize i = from + 1; i < v.size(); i++)
            if (v[i - 1] < 0 && v[i] >= 0) n++;
        return n;
    }
};

} // namespace

TEST_CASE("libsnd3 on pspocket's libsynth2: tones heard at their pitch and pan, keyed off, ended", "[hle][snd3]")
{
    Real r;
    // Tone 0 at its base note: the sine at 500 Hz, 4096 samples after the first block about 42.7 periods.
    CHECK(r.call("cellSnd3NoteOnByTone", 0, 0, 60, 1, 0) == 0);
    r.run(17);
    CHECK(Real::rms(r.left, 256) > 0.2);
    CHECK(Real::rms(r.right, 256) > 0.2);
    CHECK(Real::rising(r.left, 256) >= 42);
    CHECK(Real::rising(r.left, 256) <= 43);
    CHECK(r.call("cellSnd3VoiceGetStatus", 0) == 1);
    CHECK(r.call("cellSnd3VoiceGetEnvelope", 0) == 0x7FFF); // the attack done, the sustain at the peak
    // Keyed off: released (release shift 0) and silent, the voice free.
    CHECK(r.call("cellSnd3KeyOffByID", 1) == 1);
    r.run(4);
    CHECK(Real::peak(r.left, 512) < 1e-4f);
    CHECK(r.call("cellSnd3VoiceGetStatus", 0) == 0);
    // An octave up, panned all left: 1000 Hz, the right silent.
    key_on_param(r.b, 127, 0, 64, 0);
    CHECK(r.call("cellSnd3NoteOnByTone", 0, 0, 72, 2, kParam) == 0);
    r.run(17);
    CHECK(Real::rms(r.left, 256) > 0.2);
    CHECK(Real::peak(r.right, 256) < 1e-4f);
    CHECK(Real::rising(r.left, 256) >= 85);
    CHECK(Real::rising(r.left, 256) <= 86);
    CHECK(r.call("cellSnd3KeyOffByID", 2) == 1);
    r.run(4);
    // KeyOnByTone at 0x1000, the tone's own pitch; SynthesisEx's rear pair: libsynth2's silence.
    CHECK(r.call("cellSnd3KeyOnByTone", 0, 0, 0x1000, 3, 0) == 0);
    for (u32 n = 0; n < kSamples; n++) {
        ps_writef32(r.b, kOutRL + 4 * n, 1.0f);
        ps_writef32(r.b, kOutRR + 4 * n, 1.0f);
    }
    CHECK(r.call("cellSnd3SynthesisEx", kOutL, kOutR, kOutRL, kOutRR) == 0);
    float rear = 0, front = 0;
    for (u32 n = 0; n < kSamples; n++) {
        rear = std::max({rear, std::fabs(ps_readf32(r.b, kOutRL + 4 * n)), std::fabs(ps_readf32(r.b, kOutRR + 4 * n))});
        front = std::max(front, std::fabs(ps_readf32(r.b, kOutL + 4 * n)));
    }
    CHECK(rear == 0.0f);
    CHECK(front > 0.2f);
    r.run(16); // 3840 samples from the second block: 40 periods
    CHECK(Real::rising(r.left, 256) >= 39);
    CHECK(Real::rising(r.left, 256) <= 41);
    CHECK(r.call("cellSnd3VoiceAllKeyOff") == 0);
    r.run(4);
    // The one-shot: heard, then ended by its last block with no key-off; its voice free again.
    CHECK(r.call("cellSnd3NoteOnByTone", 0, 1, 60, 4, 0) == 0);
    r.run(1);
    CHECK(Real::rms(r.left) > 0.1);
    r.run(2); // its last 24 samples, then nothing
    CHECK(Real::peak(r.left, 128) < 1e-4f);
    CHECK(r.call("cellSnd3VoiceGetStatus", 0) == 0);
    CHECK(r.call("cellSnd3Exit") == 0);
}

TEST_CASE("libsnd3 on pspocket's libsynth2: an SMF heard, and the effect's tail", "[hle][snd3]")
{
    Real r;
    // A note from tick 0 to 480 (division 480, 120 BPM: 24000 samples, 93.75 blocks), program 0 (tone 0).
    Track t;
    t.at(0, {0x90, 60, 127}).at(480, {0x80, 60, 0}).end(0);
    place(r.b, kSmfData, build_smf(0, 480, {t}));
    REQUIRE(r.call("cellSnd3SMFBind", kSmfCtx, kSmfData, 0) == 0);
    REQUIRE(r.call("cellSnd3SMFPlay", 0, 127, 64, 1) == 0);
    r.run(93);
    CHECK(Real::rms(r.left, 256) > 0.1);
    CHECK(r.call("cellSnd3VoiceGetStatus", 0) == 1);
    r.run(4); // keyed off in block 93, then released
    CHECK(Real::peak(r.left, 512) < 1e-4f);
    CHECK(r.call("cellSnd3SMFGetPlayStatus", 0) == 0);
    CHECK(r.call("cellSnd3VoiceGetStatus", 0) == 0);
    // Tone 2 sends to the effect: with a hall on, its tail goes on after the dry voice is silent; without, silence.
    for (const bool effect : {false, true}) {
        INFO("effect " << effect);
        CHECK(r.call("cellSnd3SetEffectType", effect ? 5 : 0, 0x7FFF, 0, 0) == 0);
        CHECK(r.call("cellSnd3NoteOnByTone", 0, 2, 60, 9, 0) == 0);
        r.run(20);
        CHECK(r.call("cellSnd3KeyOffByID", 9) == 1);
        r.run(8);
        if (effect) CHECK(Real::rms(r.left, 1024) > 1e-3);
        else CHECK(Real::peak(r.left, 1024) < 1e-4f);
        r.run(200); // the tail dies away
    }
}

TEST_CASE("fuzz: libsnd3's HD3 banks and SMFs, damaged, each flush against unmapped memory", "[hle][fuzz][snd3]")
{
    namespace fz = pspocket::test::fuzz;
    Fixture f;
    fuzzing::add_guarded_areas(f);
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    REQUIRE(g.call("cellSnd3Init", 16, kSamples, 0) == 0);
    {
        // The banks: bound or refused; a bound one's tones and programs played.
        const std::vector<u8> bank = build_hd3(usual_bank());
        std::vector<fz::Field> fields = fz::fields(0x04, 6);
        for (const u32 chunk : {ps_ld32(bank.data() + 0x10), ps_ld32(bank.data() + 0x14), ps_ld32(bank.data() + 0x18)})
            fz::append(fields, fz::fields(chunk + 4, 8));
        fz::Tally tally("libsnd3 HD3");
        fz::run(tally, {{"usual bank", bank, fields}}, 1500, [&](const std::vector<u8>& input, fz::Rng& rng) {
            const u32 at = fuzzing::place_input(b, input);
            const u64 id = g.call("cellSnd3BindSoundData", kDataCtx, at, static_cast<u32>(rng.below(4)) * 0x8000);
            if (id >= 16) return false;
            for (u32 tone = 0; tone < 6; tone++) g.call("cellSnd3NoteOnByTone", id, tone, rng.below(128), 1, 0);
            for (u32 program = 0; program < 4; program++)
                g.call("cellSnd3NoteOn", id, 0, program, rng.below(128), 0, 0, 2);
            g.call("cellSnd3Synthesis", kOutL, kOutR);
            g.call("cellSnd3VoiceAllKeyOff");
            CHECK(g.call("cellSnd3UnbindSoundData", id) == 0);
            return true;
        });
    }
    {
        // The SMFs: bound or refused; a bound one played for a few blocks.
        place(b, kHd3, build_hd3(usual_bank()));
        REQUIRE(g.call("cellSnd3BindSoundData", kDataCtx, kHd3, 0) == 0);
        Track t0, t1;
        t0.tempo(0, 400000).at(10, {0xF0, 0x02, 0x10, 0xF7}).end(200);
        t1.at(0, {0xC0, 2}).at(0, {0x90, 60, 100}).at(5, {0xB0, 7, 90}).at(5, {0xE0, 0x00, 0x50}).at(5, {0xB0, 64, 100});
        t1.at(5, {0x80, 60, 0}).at(0, {0xB0, 64, 0}).at(5, {0x91, 72, 80}).at(20, {72, 0}).at(0, {0xB0, 121, 0}).end(5);
        const std::vector<u8> song = build_smf(1, 24, {t0, t1});
        const std::vector<fz::Field> fields = {{4, 4}, {8, 2}, {10, 2}, {12, 2}, {18, 4}};
        fz::Tally tally("libsnd3 SMF");
        fz::run(tally, {{"two tracks", song, fields}}, 1500, [&](const std::vector<u8>& input, fz::Rng& rng) {
            const u32 at = fuzzing::place_input(b, input);
            const u64 id = g.call("cellSnd3SMFBind", kSmfCtx, at, 0);
            if (id >= 16) return false;
            g.call("cellSnd3SMFPlay", id, 127, 64, static_cast<u32>(rng.below(3)));
            for (u32 i = 0; i < 6; i++) g.call("cellSnd3Synthesis", kOutL, kOutR);
            CHECK(g.call("cellSnd3SMFUnbind", id) == 0);
            return true;
        });
    }
    CHECK(g.call("cellSnd3Exit") == 0);
}
