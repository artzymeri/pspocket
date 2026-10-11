// libsnd3 against Sony's libsnd3 (D-644, D-597): the same calls with the same synthetic bank, sounds and songs go to
// pspocket's libsnd3 over pspocket's libsynth2 and to Sony's module over Sony's libsynth2 (both firmware-first, so
// Sony's libsnd3 imports Sony's libsynth2, loaded with it). Compared: every answer; what libsnd3 leaves in its
// libsynth2's registers, each half's own read back through GetParam, GetSwitch and GetAddr (after Init, each key-on and
// parameter, the effect); what Synthesis renders (floats within 5 steps of 1/32768); the voices' states and bit masks;
// the request queue's timing; the SMF player's answers and loudness block by block. The bank is laid out as D-641
// infers it: where Sony's module reads it otherwise, its answers and registers show it. No game data; skipped without
// the firmware (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstring>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compare_util.hpp"
#include "snd3_writer.hpp"
#include "vag_writer.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
using namespace pspocket::hle::test::snd3;

namespace {

constexpr u32 kParam = kData + 0x100, kQueueCtx = kData + 0x140, kDataCtx = kData + 0x180, kDataCtx2 = kData + 0x1A0,
              kBits = kData + 0x1C0, kWord = kData + 0x1E0, kSmfCtx = kData + 0x200, kQueueFront = kData + 0x600,
              kQueueRear = kData + 0x800, kHd3 = kData + 0x1000, kSmf = kData + 0x8000, kOutL = kData + 0x10000,
              kOutR = kData + 0x18000, kOutRL = kData + 0x20000, kOutRR = kData + 0x28000, kSrc = kData + 0x30000,
              kNotGuest = 0xF0000000u;
constexpr u32 kSamples = 256, kMaxBlocks = 8; // a run of blocks kept whole: 8 * 256 floats a side
constexpr double kTolerance = 5.0 / 32768.0;

// libsd's register numbers (libsd-common.h): the parameter in bits 8-15, a voice's SD_VOICE(core, voice), a core's
// volumes with 0x80.
enum : u32 {
    VOLL = 0x00, VOLR, PITCH, ADSR1, ADSR2, ENVX, MVOLL = 0x09, MVOLR, EVOLL, EVOLR,
    KON = 0x15, KOFF, ENDX, VMIXL, VMIXEL, VMIXR, VMIXER, ESA = 0x1C, SSA = 0x20,
};
constexpr u64 vreg(u32 param, u32 voice) { return param << 8 | (voice % 24) << 1 | voice / 24; }
constexpr u64 creg(u32 param, u32 core) { return param << 8 | 0x80 | core; }
constexpr u64 reg(u32 param, u32 core) { return param << 8 | core; }

struct Run : Half<> {
    Run(const Setup& s, bool sony_half) : Half(s, sony_half, "libsnd3") {}

    // libsynth2's export in this half: Sony's module's (libsnd3's module imports it and was loaded with it), else
    // pspocket's.
    u32 synth_descriptor(std::string_view fn)
    {
        const u32 nid = recomp::nid_of(fn);
        if (const FirmwareLibrary* fw = side.firmware()) {
            for (const auto& m : fw->lle.loaded())
                if (const auto it = m->functions.find({"libsynth2", nid}); it != m->functions.end()) return it->second;
            return 0;
        }
        return f.hle->link_function("libsynth2", nid, fn);
    }
    u64 synth(std::string_view fn, std::initializer_list<u64> args)
    {
        const u32 d = synth_descriptor(fn);
        if (!d) {
            o.line(std::string(fn) + ": libsynth2's export", "(not loaded)");
            return ~u64{0};
        }
        trace("calling: " + std::string(fn));
        return pspocket::hle::call_guest(t.ctx, d, args);
    }
    std::string param(u64 r) { return std::format("{:#06x}", static_cast<u16>(synth("cellSoundSynth2GetParam", {r}))); }
    std::string sw(u64 r) { return std::format("{:#08x}", static_cast<u32>(synth("cellSoundSynth2GetSwitch", {r}))); }
    std::string addr(u64 r) { return std::format("{:#08x}", static_cast<u32>(synth("cellSoundSynth2GetAddr", {r}))); }

    // A voice's registers as libsnd3 left them in libsynth2.
    void voice(const std::string& when, u32 v)
    {
        for (const u32 p : {PITCH, VOLL, VOLR, ADSR1, ADSR2})
            o.line(std::format("{}: voice {} parameter {:#x}", when, v, p), param(vreg(p, v)));
        o.line(std::format("{}: voice {} SSA", when, v), addr(vreg(SSA, v)));
    }
    // Both cores' switches (KON and KOFF read back what was last written).
    void switches(const std::string& when)
    {
        for (u32 c = 0; c < 2; c++)
            for (const u32 p : {KON, KOFF, VMIXL, VMIXEL, VMIXR, VMIXER})
                o.line(std::format("{}: core {} switch {:#x}", when, c, p), sw(reg(p, c)));
    }
    // libsnd3's answers about voices 0 to n - 1.
    void voices(const std::string& when, u32 n)
    {
        for (u32 v = 0; v < n; v++) {
            r("cellSnd3VoiceGetStatus", {v}, std::format("{}: VoiceGetStatus({})", when, v));
            r("cellSnd3VoiceGetEnvelope", {v}, std::format("{}: VoiceGetEnvelope({})", when, v));
        }
    }
    // GetVoice or GetVoiceByID into kBits (first filled with 0xEE, so what is not written shows): the answer, the 16
    // bytes, and the voices they name (core c's word, bit v: voice 24c + v).
    std::vector<u32> bits(const std::string& key, std::string_view fn, std::initializer_list<u64> args)
    {
        for (u32 i = 0; i < 16; i++) ps_write8(b, kBits + i, 0xEE);
        r(fn, args, key);
        o.line(key + ": the bits", hex(b + kBits, 16));
        std::vector<u32> out;
        for (u32 c = 0; c < 2; c++)
            for (u32 v = 0; v < 24; v++)
                if (ps_read32(b, kBits + 4 * c) != 0xEEEEEEEE && (ps_read32(b, kBits + 4 * c) & (1u << v)))
                    out.push_back(24 * c + v);
        return out;
    }
    // `n` Synthesis (or SynthesisEx) calls, their answers, and what they render kept whole (at most kMaxBlocks).
    void blocks(const std::string& key, u32 n, bool ex = false)
    {
        REQUIRE(n <= kMaxBlocks);
        for (u32 i = 0; i < n * kSamples; i++)
            for (const u32 out : {kOutL, kOutR, kOutRL, kOutRR}) ps_writef32(b, out + 4 * i, 2.0f);
        for (u32 k = 0; k < n; k++) {
            const u32 at = 4 * kSamples * k;
            if (ex)
                r("cellSnd3SynthesisEx", {kOutL + at, kOutR + at, kOutRL + at, kOutRR + at},
                    std::format("{}: SynthesisEx {}", key, k));
            else r("cellSnd3Synthesis", {kOutL + at, kOutR + at}, std::format("{}: Synthesis {}", key, k));
        }
        o.bytes(key + ": left", b + kOutL, 4 * kSamples * n, Rule::floats(kTolerance));
        o.bytes(key + ": right", b + kOutR, 4 * kSamples * n, Rule::floats(kTolerance));
        if (ex) {
            o.bytes(key + ": rear left", b + kOutRL, 4 * kSamples * n, Rule::floats(kTolerance));
            o.bytes(key + ": rear right", b + kOutRR, 4 * kSamples * n, Rule::floats(kTolerance));
        }
    }
    // `n` Synthesis calls, each block's loudness (the left side's RMS) as numbers and the answers they gave: the SMF
    // player's timing at a block's resolution, over runs too long to keep whole.
    void loudness(const std::string& key, u32 n)
    {
        std::vector<double> rms;
        std::set<u32> answers;
        for (u32 k = 0; k < n; k++) {
            trace(std::format("{}: Synthesis {}", key, k));
            const std::optional<u64> a = side.call("cellSnd3Synthesis", {kOutL, kOutR});
            answers.insert(a ? static_cast<u32>(*a) : 0xFFFFFFFFu);
            double sum = 0;
            for (u32 i = 0; i < kSamples; i++) {
                const double v = static_cast<double>(ps_readf32(b, kOutL + 4 * i));
                sum += v * v;
            }
            rms.push_back(std::sqrt(sum / kSamples));
        }
        std::string text;
        for (const u32 a : answers) text += (text.empty() ? "" : " ") + std::format("{:#x}", a);
        o.line(key + ": Synthesis answered", text);
        o.numbers(key + ": each block's loudness", std::move(rms), 2e-3);
    }
    void key_on_param(u8 velocity, u8 pan, u8 pan_ex, s32 add_pitch)
    {
        ps_write8(b, kParam, velocity);
        ps_write8(b, kParam + 1, pan);
        ps_write8(b, kParam + 2, pan_ex);
        ps_write8(b, kParam + 3, 0);
        ps_write32(b, kParam + 4, static_cast<u32>(add_pitch));
    }
    void queue(u32 front, u32 front_size, u32 rear, u32 rear_size)
    {
        ps_write32(b, kQueueCtx, front);
        ps_write32(b, kQueueCtx + 4, front_size);
        ps_write32(b, kQueueCtx + 8, rear);
        ps_write32(b, kQueueCtx + 12, rear_size);
    }
    // The sounds in this half's sound memory: vag_writer's looped sine at 0x10000 and 0x10800 (the usual bank's VAG 0
    // and 1), a one-shot sine at 0x11000.
    void sounds()
    {
        const std::vector<u8> loop = vag::looped_sine();
        const std::vector<u8> once = vag::encode(vag::sine(280, 4, 16000), 0, 0, 1);
        std::memset(b + kSrc, 0, 0x1800);
        std::memcpy(b + kSrc, loop.data(), loop.size());
        std::memcpy(b + kSrc + 0x800, loop.data(), loop.size());
        std::memcpy(b + kSrc + 0x1000, once.data(), once.size());
        o.line("VoiceTrans of the sounds", answer(synth("cellSoundSynth2VoiceTrans", {0, 0, kSrc, 0x10000, 0x1800})));
    }
};

Observed half(const Setup& s, bool sony_half)
{
    Run h(s, sony_half);
    u8* b = h.b;

    // ---- before Init ----
    h.r("cellSnd3Exit", {}, "Exit before Init");
    h.r("cellSnd3SetOutputMode", {0}, "SetOutputMode before Init");
    h.r("cellSnd3VoiceGetStatus", {0}, "VoiceGetStatus before Init");
    h.r("cellSnd3KeyOffByID", {1}, "KeyOffByID before Init");
    h.r("cellSnd3SMFGetPlayStatus", {0}, "SMFGetPlayStatus before Init");
    h.r("cellSnd3Synthesis", {kOutL, kOutR}, "Synthesis before Init");

    // ---- Note2Pitch and Pitch2Note ----
    h.o.group("Note2Pitch");
    for (const auto& [cn, cf] : {std::pair<u64, u64>{60, 0}, {48, 64}, {72, 127}})
        for (s32 note = 0; note < 128; note += 7)
            for (s32 fine = -128; fine < 128; fine += 37)
                h.r("cellSnd3Note2Pitch", {cn, cf, static_cast<u64>(note), static_cast<u64>(static_cast<u16>(fine))},
                    std::format("Note2Pitch({}, {}, {}, {})", cn, cf, note, fine));
    h.o.group("Pitch2Note");
    for (const auto& [cn, cf] : {std::pair<u64, u64>{60, 0}, {48, 64}})
        for (u64 pitch = 0; pitch < 0x10000; pitch += pitch < 0x4000 ? 0x7B : 0x3F1)
            h.r("cellSnd3Pitch2Note", {cn, cf, pitch}, std::format("Pitch2Note({}, {}, {:#x})", cn, cf, pitch));
    h.o.group("");

    // ---- Init's checks, each followed by an Exit (in case one is taken) ----
    for (const auto& [voices, samples] : {std::pair<u64, u64>{0, 256}, {49, 256}, {48, 256}, {8, 0}, {8, 0x10000},
             {8, 1}, {8, 0xFFFF}}) {
        const std::string key = std::format("Init({}, {}, NULL)", voices, samples);
        h.r("cellSnd3Init", {voices, samples, 0}, key);
        h.r("cellSnd3Exit", {}, key + ": Exit");
    }
    h.r("cellSnd3Init", {8, 256, kNotGuest}, "Init with a queue context outside memory");
    h.r("cellSnd3Exit", {}, "Init with a queue context outside memory: Exit");
    for (const auto& [front, rear] : {std::pair<u32, u32>{0, 64}, {64, 0}, {31, 64}, {64, 31}, {32, 32}, {1, 1}}) {
        h.queue(kQueueFront, front, kQueueRear, rear);
        const std::string key = std::format("Init with queues of {} and {} bytes", front, rear);
        h.r("cellSnd3Init", {8, 256, kQueueCtx}, key);
        h.r("cellSnd3Exit", {}, key + ": Exit");
    }
    h.queue(0, 64, kQueueRear, 64);
    h.r("cellSnd3Init", {8, 256, kQueueCtx}, "Init with no front queue");
    h.r("cellSnd3Exit", {}, "Init with no front queue: Exit");

    // ---- Init, and what it leaves in libsynth2 ----
    h.r("cellSnd3Init", {16, kSamples, 0}, "Init(16, 256, NULL)");
    h.r("cellSnd3Init", {16, kSamples, 0}, "Init again");
    for (u32 c = 0; c < 2; c++)
        for (const u32 p : {MVOLL, MVOLR, EVOLL, EVOLR})
            h.o.line(std::format("after Init: core {} parameter {:#x}", c, p), h.param(creg(p, c)));
    h.switches("after Init");
    h.voice("after Init", 0);
    h.blocks("after Init", 2, true);

    // ---- the bank ----
    h.sounds();
    place(b, kHd3, build_hd3(usual_bank()));
    h.r("cellSnd3BindSoundData", {0, kHd3, 0x10000}, "BindSoundData without a context");
    h.r("cellSnd3BindSoundData", {kDataCtx, 0, 0x10000}, "BindSoundData without a header");
    h.r("cellSnd3BindSoundData", {kDataCtx, kHd3, 0x200000}, "BindSoundData past sound memory");
    h.r("cellSnd3BindSoundData", {kDataCtx, kHd3, 0x1FF800}, "BindSoundData with the samples ending past it");
    {
        std::vector<u8> bad = build_hd3(usual_bank());
        bad[0] = 'S';
        place(b, kHd3 + 0x2000, bad);
        h.r("cellSnd3BindSoundData", {kDataCtx2, kHd3 + 0x2000, 0x10000}, "BindSoundData of a header not P3HD");
    }
    const u32 hd3 = static_cast<u32>(h.r("cellSnd3BindSoundData", {kDataCtx, kHd3, 0x10000}, "BindSoundData"));
    h.o.note("CellSnd3DataCtx after BindSoundData: " + hex(b + kDataCtx, 32));
    const u32 second = static_cast<u32>(h.r("cellSnd3BindSoundData", {kDataCtx2, kHd3, 0x10000}, "a second bank"));
    h.r("cellSnd3UnbindSoundData", {second}, "UnbindSoundData of the second");
    h.r("cellSnd3UnbindSoundData", {second}, "UnbindSoundData of the second again");
    h.r("cellSnd3UnbindSoundData", {99}, "UnbindSoundData(99)");

    // ---- tone key-ons ----
    h.r("cellSnd3NoteOnByTone", {hd3, 0, 72, 1, 0}, "NoteOnByTone(tone 0, note 72, ID 1)");
    for (u32 v = 0; v < 3; v++) h.voice("tone 0 at note 72", v);
    h.switches("tone 0 at note 72");
    h.voices("tone 0 at note 72", 3);
    h.blocks("tone 0 at note 72", 4);
    h.voices("tone 0 at note 72, four blocks on", 3);
    h.key_on_param(100, 32, 64, 128);
    h.r("cellSnd3KeyOnByTone", {hd3, 1, 0x1000, 2, kParam}, "KeyOnByTone(tone 1, 0x1000, ID 2, velocity 100, pan 32)");
    for (u32 v = 0; v < 3; v++) h.voice("KeyOnByTone", v);
    h.blocks("both tones", 4);
    h.bits("GetVoiceByID(1)", "cellSnd3GetVoiceByID", {1, kBits});
    const std::vector<u32> two = h.bits("GetVoiceByID(2)", "cellSnd3GetVoiceByID", {2, kBits});
    h.bits("GetVoiceByID(3)", "cellSnd3GetVoiceByID", {3, kBits});
    h.r("cellSnd3GetVoiceByID", {2, 0}, "GetVoiceByID(2, NULL)");
    h.r("cellSnd3KeyOffByID", {1}, "KeyOffByID(1)");
    h.r("cellSnd3KeyOffByID", {1}, "KeyOffByID(1) again");
    h.blocks("tone 0 keyed off", 4);
    h.voices("tone 0 keyed off", 3);
    h.key_on_param(128, 64, 64, 0);
    h.r("cellSnd3NoteOnByTone", {hd3, 0, 60, 1, kParam}, "NoteOnByTone with velocity 128");
    for (const auto& [tone, note] : {std::pair<u64, u64>{2, 60}, {9, 60}, {0, 128}})
        h.r("cellSnd3NoteOnByTone", {hd3, tone, note, 1, 0}, std::format("NoteOnByTone(tone {}, note {})", tone, note));
    h.r("cellSnd3NoteOnByTone", {99, 0, 60, 1, 0}, "NoteOnByTone(bank 99)");
    h.r("cellSnd3NoteOnByTone", {hd3, 0, 60, 1, kNotGuest}, "NoteOnByTone with a parameter outside memory");

    // ---- a voice's parameters (on the voice of ID 2, as each half found it) ----
    const u32 v2 = two.empty() ? 1 : two[0];
    h.o.line("ID 2's voice", std::format("{}", v2));
    const std::vector<std::pair<std::string, std::pair<std::string, u64>>> params = {
        {"VoiceSetPitch(-1536)", {"cellSnd3VoiceSetPitch", static_cast<u32>(-1536)}},
        {"VoiceSetPitch(0)", {"cellSnd3VoiceSetPitch", 0}}, {"VoiceSetVelocity(32)", {"cellSnd3VoiceSetVelocity", 32}},
        {"VoiceSetVelocity(128)", {"cellSnd3VoiceSetVelocity", 128}},
        {"VoiceSetPanpot(96)", {"cellSnd3VoiceSetPanpot", 96}}, {"VoiceSetPanpot(128)", {"cellSnd3VoiceSetPanpot", 128}},
        {"VoiceSetPanpotEx(100)", {"cellSnd3VoiceSetPanpotEx", 100}},
        {"VoiceSetPitchBend(0x3FFF)", {"cellSnd3VoiceSetPitchBend", 0x3FFF}},
        {"VoiceSetPitchBend(0)", {"cellSnd3VoiceSetPitchBend", 0}},
        {"VoiceSetPitchBend(0x4000)", {"cellSnd3VoiceSetPitchBend", 0x4000}}};
    for (const auto& [key, call] : params) {
        h.r(call.first, {v2, call.second}, key);
        h.voice(key, v2);
    }
    h.blocks("the voice's parameters", 2);
    h.r("cellSnd3VoiceSetPitch", {16, 0}, "VoiceSetPitch(16, 0)");
    h.r("cellSnd3VoiceGetStatus", {16}, "VoiceGetStatus(16)");

    // ---- reserve mode, holds ----
    h.r("cellSnd3VoiceNoteOnByTone", {hd3, 5, 0, 60, 4, 0}, "VoiceNoteOnByTone on voice 5, not reserved");
    h.r("cellSnd3VoiceSetReserveMode", {5, 1}, "VoiceSetReserveMode(5, 1)");
    h.r("cellSnd3VoiceSetReserveMode", {5, 2}, "VoiceSetReserveMode(5, 2)");
    h.r("cellSnd3VoiceSetReserveMode", {16, 1}, "VoiceSetReserveMode(16, 1)");
    h.r("cellSnd3VoiceNoteOnByTone", {hd3, 5, 0, 60, 4, 0}, "VoiceNoteOnByTone on voice 5, reserved");
    h.voice("VoiceNoteOnByTone on voice 5", 5);
    h.r("cellSnd3VoiceKeyOnByTone", {hd3, 5, 1, 0x800, 4, 0}, "VoiceKeyOnByTone on voice 5");
    h.voice("VoiceKeyOnByTone on voice 5", 5);
    h.r("cellSnd3VoiceSetSustainHold", {5, 1}, "VoiceSetSustainHold(5, 1)");
    h.r("cellSnd3VoiceKeyOff", {5}, "VoiceKeyOff(5) held");
    h.voices("voice 5 held", 6);
    h.r("cellSnd3VoiceSetSustainHold", {5, 0}, "VoiceSetSustainHold(5, 0)");
    h.voices("voice 5 let go", 6);
    h.r("cellSnd3VoiceAllKeyOff", {}, "VoiceAllKeyOff");
    h.blocks("all keyed off", 4);
    h.voices("all keyed off", 8);

    // ---- programs ----
    h.r("cellSnd3NoteOn", {hd3, 3, 0, 50, 0, 0, 7}, "NoteOn(channel 3, program 0, note 50, ID 7)");
    h.r("cellSnd3NoteOn", {hd3, 3, 0, 72, 0, 0, 7}, "NoteOn(channel 3, program 0, note 72, ID 7)");
    h.key_on_param(100, 64, 64, 0);
    h.r("cellSnd3NoteOn", {hd3, 3, 2, 60, 0, kParam, 7}, "NoteOn(channel 3, program 2, note 60, ID 7)");
    const std::vector<u32> seven = h.bits("GetVoice(3, 7)", "cellSnd3GetVoice", {3, 7, kBits});
    for (const u32 v : seven) h.voice("channel 3's notes", v);
    h.bits("GetVoice(4, 7)", "cellSnd3GetVoice", {4, 7, kBits});
    h.r("cellSnd3GetVoice", {16, 7, kBits}, "GetVoice(16, 7)");
    h.blocks("channel 3's notes", 4);
    h.r("cellSnd3NoteOff", {3, 60, 7}, "NoteOff(3, 60, 7)");
    h.bits("GetVoice(3, 7) after NoteOff", "cellSnd3GetVoice", {3, 7, kBits});
    h.blocks("channel 3's note 60 off", 4);
    h.r("cellSnd3NoteOn", {hd3, 5, 0, 50, 1, 0, 9}, "NoteOn(channel 5, note 50, sustain 1, ID 9)");
    h.r("cellSnd3NoteOff", {5, 50, 9}, "NoteOff(5, 50, 9) held");
    h.bits("GetVoice(5, 9) held", "cellSnd3GetVoice", {5, 9, kBits});
    h.r("cellSnd3SetSustainHold", {5, 0, 9}, "SetSustainHold(5, 0, 9)");
    h.blocks("channel 5's note let go", 4);
    h.bits("GetVoice(5, 9) let go", "cellSnd3GetVoice", {5, 9, kBits});
    for (const auto& [channel, program, note] : {std::tuple<u64, u64, u64>{16, 0, 60}, {3, 1, 60}, {3, 3, 60},
             {3, 0, 128}})
        h.r("cellSnd3NoteOn", {hd3, channel, program, note, 0, 0, 7},
            std::format("NoteOn(channel {}, program {}, note {})", channel, program, note));
    h.r("cellSnd3NoteOff", {16, 60, 7}, "NoteOff(16, 60, 7)");
    h.r("cellSnd3SetSustainHold", {16, 0, 7}, "SetSustainHold(16, 0, 7)");
    h.r("cellSnd3VoiceAllKeyOff", {}, "VoiceAllKeyOff after the programs");
    h.blocks("the programs keyed off", 4);

    // ---- the effect, the output mode ----
    h.r("cellSnd3SetEffectType", {5, 0x3000, 0, 0}, "SetEffectType(hall, 0x3000)");
    for (u32 c = 0; c < 2; c++) {
        for (const u32 p : {EVOLL, EVOLR})
            h.o.line(std::format("a hall: core {} parameter {:#x}", c, p), h.param(creg(p, c)));
        h.o.line(std::format("a hall: core {} ESA", c), h.addr(reg(ESA, c)));
    }
    h.r("cellSnd3NoteOnByTone", {hd3, 1, 72, 8, 0}, "NoteOnByTone(tone 1, sent to the effect)");
    h.switches("tone 1 sent to the effect");
    h.blocks("tone 1 with a hall", 8);
    h.r("cellSnd3KeyOffByID", {8}, "KeyOffByID(8) with a hall");
    h.blocks("the hall's tail", 8);
    for (const auto& [type, delay, feedback] : {std::tuple<u64, u64, u64>{10, 0, 0}, {7, 128, 0}, {7, 0, 128},
             {7, 64, 127}, {0, 0, 0}})
        h.r("cellSnd3SetEffectType", {type, 0x1000, delay, feedback},
            std::format("SetEffectType({}, delay {}, feedback {})", type, delay, feedback));
    h.key_on_param(127, 0, 64, 0);
    h.r("cellSnd3NoteOnByTone", {hd3, 0, 60, 10, kParam}, "NoteOnByTone panned left");
    const std::vector<u32> ten = h.bits("GetVoiceByID(10)", "cellSnd3GetVoiceByID", {10, kBits});
    const u32 v10 = ten.empty() ? 0 : ten[0];
    h.voice("panned left", v10);
    h.r("cellSnd3SetOutputMode", {1}, "SetOutputMode(1)");
    h.voice("mono", v10);
    h.blocks("mono", 2);
    h.r("cellSnd3SetOutputMode", {2}, "SetOutputMode(2)");
    h.r("cellSnd3SetOutputMode", {0}, "SetOutputMode(0)");
    h.voice("stereo again", v10);
    h.r("cellSnd3VoiceAllKeyOff", {}, "VoiceAllKeyOff after the output modes");
    h.blocks("the output modes keyed off", 4);

    // ---- the request queue ----
    h.r("cellSnd3Exit", {}, "Exit before the queue");
    h.queue(kQueueFront, 64, kQueueRear, 32);
    h.r("cellSnd3Init", {8, kSamples, kQueueCtx}, "Init with a queue of two and one requests");
    h.sounds();
    const u32 qbank = static_cast<u32>(h.r("cellSnd3BindSoundData", {kDataCtx, kHd3, 0x10000}, "BindSoundData, queued"));
    for (u32 i = 0; i < 3; i++)
        h.r("cellSnd3NoteOnByTone", {qbank, 0, 60, 20, 0}, std::format("NoteOnByTone {} with a queue", i));
    h.switches("queued, before a block");
    h.voices("queued, before a block", 3);
    h.r("cellSnd3VoiceKeyOff", {0}, "VoiceKeyOff(0) with a queue");
    h.r("cellSnd3VoiceKeyOff", {1}, "VoiceKeyOff(1) with a queue");
    h.blocks("the queue's first block", 1);
    h.switches("queued, after a block");
    h.voices("queued, after a block", 3);
    h.blocks("the queue's next blocks", 3);

    // ---- an SMF ----
    place(b, kSmf, usual_song());
    h.r("cellSnd3SMFBind", {0, kSmf, qbank}, "SMFBind without a context");
    h.r("cellSnd3SMFBind", {kSmfCtx, 0, qbank}, "SMFBind without a file");
    h.r("cellSnd3SMFBind", {kSmfCtx, kSmf, 99}, "SMFBind over bank 99");
    {
        std::vector<u8> bad = usual_song();
        bad[9] = 2;
        place(b, kSmf + 0x4000, bad);
        h.r("cellSnd3SMFBind", {kSmfCtx, kSmf + 0x4000, qbank}, "SMFBind of a format 2 file");
    }
    const u32 smf = static_cast<u32>(h.r("cellSnd3SMFBind", {kSmfCtx, kSmf, qbank}, "SMFBind"));
    h.o.note("CellSnd3SmfCtx after SMFBind: " + hex(b + kSmfCtx, 352));
    h.r("cellSnd3SMFGetTempo", {smf}, "SMFGetTempo");
    h.r("cellSnd3SMFGetPlayStatus", {smf}, "SMFGetPlayStatus after the bind");
    for (const auto& get : {"cellSnd3SMFGetPlayVelocity", "cellSnd3SMFGetPlayPanpot", "cellSnd3SMFGetPlayPanpotEx"})
        h.r(get, {smf}, std::format("{} after the bind", get));
    ps_write32(b, kWord, 0xEEEEEEEE);
    h.r("cellSnd3SMFGetPlayChannel", {smf, kWord}, "SMFGetPlayChannel");
    h.o.line("SMFGetPlayChannel's word", std::format("{:#x}", ps_read32(b, kWord)));
    for (const u64 channel : {0u, 15u, 16u}) {
        ps_write32(b, kWord, 0xEEEEEEEE);
        h.r("cellSnd3SMFGetKeyOnID", {smf, channel, kWord}, std::format("SMFGetKeyOnID(channel {})", channel));
        h.o.line(std::format("SMFGetKeyOnID(channel {})'s word", channel), std::format("{:#x}", ps_read32(b, kWord)));
    }
    h.r("cellSnd3SMFPlay", {smf, 127, 64, 1}, "SMFPlay once");
    h.r("cellSnd3SMFGetPlayStatus", {smf}, "SMFGetPlayStatus playing");
    h.loudness("the song once", 192);
    h.r("cellSnd3SMFGetPlayStatus", {smf}, "SMFGetPlayStatus at its end");
    h.r("cellSnd3SMFPlayEx", {smf, 100, 32, 100, 2}, "SMFPlayEx twice, velocity 100, pan 32");
    h.loudness("the song twice", 380);
    h.r("cellSnd3SMFGetPlayStatus", {smf}, "SMFGetPlayStatus after twice");
    h.r("cellSnd3SMFAddTempo", {smf, 120}, "SMFAddTempo(120)");
    h.r("cellSnd3SMFGetTempo", {smf}, "SMFGetTempo after AddTempo(120)");
    h.r("cellSnd3SMFAddTempo", {smf, static_cast<u32>(-60)}, "SMFAddTempo(-60)");
    h.r("cellSnd3SMFGetTempo", {smf}, "SMFGetTempo after AddTempo(-60)");
    h.r("cellSnd3SMFPlay", {smf, 127, 64, 0}, "SMFPlay for ever at 180 BPM");
    h.loudness("the song at 180 BPM", 100);
    h.r("cellSnd3SMFPause", {smf}, "SMFPause");
    h.r("cellSnd3SMFGetPlayStatus", {smf}, "SMFGetPlayStatus paused");
    h.loudness("paused", 10);
    h.r("cellSnd3SMFResume", {smf}, "SMFResume");
    h.r("cellSnd3SMFGetPlayStatus", {smf}, "SMFGetPlayStatus resumed");
    h.loudness("resumed", 60);
    h.r("cellSnd3SMFSetPlayVelocity", {smf, 64}, "SMFSetPlayVelocity(64)");
    h.r("cellSnd3SMFSetPlayPanpot", {smf, 0}, "SMFSetPlayPanpot(0)");
    h.r("cellSnd3SMFSetPlayPanpotEx", {smf, 127}, "SMFSetPlayPanpotEx(127)");
    for (const auto& get : {"cellSnd3SMFGetPlayVelocity", "cellSnd3SMFGetPlayPanpot", "cellSnd3SMFGetPlayPanpotEx"})
        h.r(get, {smf}, std::format("{} after the sets", get));
    h.loudness("velocity 64, all left", 60);
    h.r("cellSnd3SMFSetPlayChannel", {smf, 0xFFFE}, "SMFSetPlayChannel(0xfffe)");
    h.loudness("channel 0 out", 60);
    h.r("cellSnd3SMFSetPlayChannel", {smf, 0x10000}, "SMFSetPlayChannel(0x10000)");
    h.r("cellSnd3SMFSetPlayVelocity", {smf, 128}, "SMFSetPlayVelocity(128)");
    h.r("cellSnd3SMFStop", {smf}, "SMFStop");
    h.r("cellSnd3SMFGetPlayStatus", {smf}, "SMFGetPlayStatus stopped");
    h.r("cellSnd3SMFUnbind", {smf}, "SMFUnbind");
    h.r("cellSnd3SMFUnbind", {smf}, "SMFUnbind again");
    h.r("cellSnd3SMFGetPlayStatus", {smf}, "SMFGetPlayStatus unbound");
    h.r("cellSnd3Exit", {}, "the last Exit");
    h.r("cellSnd3Exit", {}, "Exit after the last");
    return std::move(h.o);
}

} // namespace

TEST_CASE("libsnd3's comparison calls Sony's exports by their names; pspocket's library, firmware first", "[hle][snd3]")
{
    check_names("libsnd3",
        {"cellSnd3Init", "cellSnd3Exit", "cellSnd3Note2Pitch", "cellSnd3Pitch2Note", "cellSnd3SetOutputMode",
            "cellSnd3Synthesis", "cellSnd3SynthesisEx", "cellSnd3BindSoundData", "cellSnd3UnbindSoundData",
            "cellSnd3NoteOnByTone", "cellSnd3KeyOnByTone", "cellSnd3VoiceNoteOnByTone", "cellSnd3VoiceKeyOnByTone",
            "cellSnd3VoiceSetReserveMode", "cellSnd3VoiceSetSustainHold", "cellSnd3VoiceKeyOff", "cellSnd3VoiceSetPitch",
            "cellSnd3VoiceSetVelocity", "cellSnd3VoiceSetPanpot", "cellSnd3VoiceSetPanpotEx", "cellSnd3VoiceSetPitchBend",
            "cellSnd3VoiceAllKeyOff", "cellSnd3VoiceGetEnvelope", "cellSnd3VoiceGetStatus", "cellSnd3KeyOffByID",
            "cellSnd3GetVoice", "cellSnd3GetVoiceByID", "cellSnd3NoteOn", "cellSnd3NoteOff", "cellSnd3SetSustainHold",
            "cellSnd3SetEffectType", "cellSnd3SMFBind", "cellSnd3SMFUnbind", "cellSnd3SMFPlay", "cellSnd3SMFPlayEx",
            "cellSnd3SMFPause", "cellSnd3SMFResume", "cellSnd3SMFStop", "cellSnd3SMFAddTempo", "cellSnd3SMFGetTempo",
            "cellSnd3SMFSetPlayVelocity", "cellSnd3SMFGetPlayVelocity", "cellSnd3SMFSetPlayPanpot",
            "cellSnd3SMFSetPlayPanpotEx", "cellSnd3SMFGetPlayPanpot", "cellSnd3SMFGetPlayPanpotEx",
            "cellSnd3SMFGetPlayStatus", "cellSnd3SMFSetPlayChannel", "cellSnd3SMFGetPlayChannel",
            "cellSnd3SMFGetKeyOnID"});
    // The libsynth2 functions it reads each half's registers with, by their names in libsynth2.
    check_names("libsynth2", {"cellSoundSynth2GetParam", "cellSoundSynth2GetSwitch", "cellSoundSynth2GetAddr",
                                 "cellSoundSynth2VoiceTrans"});
}

TEST_CASE("libsnd3 against Sony's libsnd3: banks, key-ons, registers, output, the queue, an SMF",
    "[.][hle][lle][compare][snd3]")
{
    const Setup s = setup();
    run(s, "libsnd3 against Sony's libsnd3", [&](bool sony) { return half(s, sony); },
        [](const Observed& o) {
            // pspocket's own half: test_snd.cpp's answers.
            CHECK(o.value("Exit before Init") == "0x80310305");
            CHECK(o.value("Note2Pitch(60, 0, 63, -17)") == "0x12e2"); // 4096 * 2^((3 - 17/128) / 12), 4833.8
            CHECK(o.value("Init(49, 256, NULL)") == "0x80310301");
            CHECK(o.value("Init with queues of 32 and 32 bytes") == "0x0");
            CHECK(o.value("Init(16, 256, NULL)") == "0x0");
            CHECK(o.value("Init again") == "0x80310304");
            CHECK(o.value("after Init: core 1 parameter 0x9") == "0x3fff");
            CHECK(o.value("after Init: core 0 switch 0x18") == "0xffffff");
            CHECK(o.value("after Init: core 0 switch 0x19") == "0xff0000");
            CHECK(o.value("VoiceTrans of the sounds") == "0x1800");
            CHECK(o.value("BindSoundData") == "0x0");
            CHECK(o.value("BindSoundData of a header not P3HD") == "0x8031030a");
            CHECK(o.value("a second bank") == "0x1");
            CHECK(o.value("tone 0 at note 72: voice 0 parameter 0x2") == "0x1000");
            CHECK(o.value("tone 0 at note 72: voice 0 SSA") == "0x010000");
            CHECK(o.value("tone 0 at note 72: core 0 switch 0x15") == "0x000001");
            CHECK(o.value("KeyOffByID(1)") == "0x1");
            CHECK(o.value("ID 2's voice") == "1");
            CHECK(o.value("VoiceSetPitch(-1536): voice 1 parameter 0x2") == "0x0800");
            CHECK(o.value("VoiceNoteOnByTone on voice 5, not reserved") == "0x8031030e");
            CHECK(o.value("NoteOn(channel 3, program 1, note 60)") == "0x80310301");
            CHECK(o.value("SetOutputMode(2)") == "0x80310310");
            CHECK(o.value("a hall: core 1 parameter 0xb") == "0x3000");
            CHECK(o.value("NoteOnByTone 2 with a queue") == "0x8031030f");
            // Before the block KON still reads what libsynth2's own Init wrote (every voice keyed on and off).
            CHECK(o.value("queued, before a block: core 0 switch 0x15") == "0xffffff");
            CHECK(o.value("queued, after a block: core 0 switch 0x15") == "0x000003");
            CHECK(o.value("queued, after a block: core 0 switch 0x16") == "0x000001");
            CHECK(o.value("SMFBind") == "0x0");
            CHECK(o.value("SMFBind of a format 2 file") == "0x80310308");
            CHECK(o.value("SMFGetTempo") == "0x78");
            CHECK(o.value("SMFGetKeyOnID(channel 15)'s word") == "0x8000000f");
            CHECK(o.value("SMFGetPlayStatus at its end") == "0x0");
            CHECK(o.value("SMFGetTempo after AddTempo(-60)") == "0xb4");
            CHECK(o.value("SMFGetPlayStatus paused") == "0x2");
            CHECK(o.value("SMFUnbind again") == "0x8031030b");
            CHECK(o.value("Exit after the last") == "0x80310305");
            const Data* tone = o.find_data("tone 0 at note 72: left");
            REQUIRE(tone);
            CHECK(tone->bytes.size() == 4 * 4 * kSamples);
            CHECK(o.lines.size() > 1200);
        });
}
