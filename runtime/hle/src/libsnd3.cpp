// libsnd3: Sony's sound library over libsynth2 (the PS2's SPU2 sound chip as a PS3 library), pspocket's own (D-640 to
// D-643). No Sony module was at hand to compare it with: the library is firmware-first (D-563), so where the system
// software is installed a game gets Sony's libsnd3 whole, and pspocket's where it is not.
//
// References (what each gives is in D-640 to D-643, with what is inferred and what to check on the Mac):
// - RPCS3 rpcs3/Emu/Cell/Modules/libsnd3.cpp and libsnd3.h: every signature, the error codes, CellSnd3KeyOnParam,
//   CellSnd3VoiceBitCtx, CellSnd3DataCtx (32 bytes), CellSnd3SmfCtx (352) and CellSnd3RequestQueueCtx. Its bodies are
//   stubs. libsynth2.cpp and libsynth2.h: libsynth2's exports, its error codes and CellSoundSynth2EffectAttr.
// - libsynth2 is libsd's API under cell names; libsd's register numbers (a voice in bits 1-5 with its core in bit 0, the
//   parameter in the high byte), core attributes and effect modes as ps2sdk documents them (common/include/
//   libsd-common.h), and libsd's Note2Pitch and Pitch2Note arithmetic (iop/sound/libsd/src/freesd.c). Facts read, no
//   code taken (ps2sdk's licence is not compatible with the GPL).
// - The HD3 bank: the header's chunk offsets and the VAG table (P3VA) as vgmstream reads them (src/meta/hd3_bd3.c); the
//   tone (P3TN) and program (P3PG) records inferred from the PS2's HD bank, the format's ancestor (VGMTrans
//   src/main/formats/SonyPS2/SonyPS2InstrSet.h: the Sample, Program and Split blocks). Facts only, no code taken.
// - Standard MIDI Files: the MIDI 1.0 specification's file format (header and track chunks, running status, meta and
//   system exclusive events, tempo).
//
// The library drives libsynth2 through libsynth2's exports, as Sony's libsnd3 does: the firmware module's when the
// loader linked one for this process (Registry::add_imports), else pspocket's own. Requests that change the synthesizer
// (key-ons, voice parameters, key-offs, the effect) go through the request queue the game gives cellSnd3Init: front
// requests are applied before the next block is generated, rear ones (key-offs) after it, so a note keyed on and off
// between two Synthesis calls still sounds for a block; without a queue they are applied at once.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "libsnd3";
constexpr std::string_view kSynth = "libsynth2";
constexpr const char* kChan = "libsnd3";

// RPCS3 libsnd3.h.
enum : u32 {
    CELL_SND3_ERROR_PARAM = 0x80310301,
    CELL_SND3_ERROR_CREATE_MUTEX = 0x80310302,
    CELL_SND3_ERROR_SYNTH = 0x80310303,
    CELL_SND3_ERROR_ALREADY = 0x80310304,
    CELL_SND3_ERROR_NOTINIT = 0x80310305,
    CELL_SND3_ERROR_SMFFULL = 0x80310306,
    CELL_SND3_ERROR_HD3ID = 0x80310307,
    CELL_SND3_ERROR_SMF = 0x80310308,
    CELL_SND3_ERROR_SMFCTX = 0x80310309,
    CELL_SND3_ERROR_FORMAT = 0x8031030a,
    CELL_SND3_ERROR_SMFID = 0x8031030b,
    CELL_SND3_ERROR_SOUNDDATAFULL = 0x8031030c,
    CELL_SND3_ERROR_VOICENUM = 0x8031030d,
    CELL_SND3_ERROR_RESERVEDVOICE = 0x8031030e,
    CELL_SND3_ERROR_REQUESTQUEFULL = 0x8031030f,
    CELL_SND3_ERROR_OUTPUTMODE = 0x80310310,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }
// RPCS3 libsynth2.h.
constexpr u32 CELL_SOUND_SYNTH2_ERROR_ALREADY_INITIALIZED = 0x80310203;

// libsd's registers (ps2sdk libsd-common.h): a voice parameter or address with SD_VOICE(core, voice) = core | voice << 1,
// a core's parameter or switch with the core in bit 0.
constexpr u16 kVoll = 0x0000, kVolr = 0x0100, kPitch = 0x0200, kAdsr1 = 0x0300, kAdsr2 = 0x0400, kEnvx = 0x0500;
constexpr u16 kMvoll = 0x0980, kMvolr = 0x0A80;
constexpr u16 kKon = 0x1500, kKoff = 0x1600, kVmixl = 0x1800, kVmixel = 0x1900, kVmixr = 0x1A00, kVmixer = 0x1B00;
constexpr u16 kSsa = 0x2000;
constexpr u16 kCoreEffectEnable = 0x2; // SD_CORE_EFFECT_ENABLE, with the core in bit 0
constexpr u32 kEffectModes = 10;       // SD_EFFECT_MODE_OFF (0) to SD_EFFECT_MODE_PIPE (9)
constexpr u16 kEffectClear = 0x100;    // SD_EFFECT_MODE_CLEAR
constexpr u16 sd_voice(u32 core, u32 voice) { return static_cast<u16>(core | (voice << 1)); }

// The synthesizer: the SPU2's two cores of 24 voices, 2 MB of sound memory, 48 kHz (D-640).
constexpr u32 kCores = 2, kVoicesPerCore = 24, kSynthVoices = kCores * kVoicesPerCore;
constexpr u32 kSynthMemory = 0x200000, kRate = 48000;
constexpr u32 kMaxPitch = 0x3FFF, kMaxVolume = 0x3FFF;

// The library's tables (D-640: pspocket's choice, Sony's sizes are not known).
constexpr u32 kMaxBanks = 16, kMaxSmf = 16;
constexpr u32 kRequestSize = 32;                 // one request's bytes in the game's queue buffers
constexpr u32 kDataCtxSize = 32, kSmfCtxSize = 352; // CellSnd3DataCtx, CellSnd3SmfCtx
constexpr u32 kQueueCtxSize = 16;                // CellSnd3RequestQueueCtx
constexpr u32 kKeyOnParamSize = 8;               // CellSnd3KeyOnParam: vel, pan, panEx, (pad), s32 addPitch
constexpr u32 kVoiceBitSize = 16;                // CellSnd3VoiceBitCtx: u32 core[4]
constexpr u32 kMaxHd3Size = 16u << 20, kMaxSmfSize = 4u << 20;

// Output modes (D-643: the values are pspocket's reading).
constexpr u32 kOutputStereo = 0, kOutputMono = 1;
// Voice states cellSnd3VoiceGetStatus answers, SMF states cellSnd3SMFGetPlayStatus answers (D-640, D-642).
constexpr s32 kVoiceFree = 0, kVoiceOn = 1, kVoiceReleased = 2;
constexpr s32 kSmfStopped = 0, kSmfPlaying = 1, kSmfPaused = 2;

u32 be32(std::span<const u8> d, usize at) { return ps_ld32(d.data() + at); }
u16 be16(std::span<const u8> d, usize at) { return ps_ld16(d.data() + at); }

// Whether [addr, addr + size) is all committed guest memory (every page).
bool guest_range(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!size) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); a < u64{addr} + size; a += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return false;
    return true;
}

s64 floor_div(s64 a, s64 b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

// ------------------------------------------------------------------------------------------------- pitch (D-643)

// libsd's tables: 0x8000 * 2^(i/12) for the twelve semitones and 0x8000 * 2^(j/1536) for the 128 fine steps of one,
// rounded down.
struct PitchTables {
    std::array<u32, 12> semitone{};
    std::array<u32, 128> fine{};
};

const PitchTables& pitch_tables()
{
    static const PitchTables t = [] {
        PitchTables p;
        for (u32 i = 0; i < 12; i++) p.semitone[i] = static_cast<u32>(std::floor(32768.0 * std::exp2(i / 12.0)));
        for (u32 j = 0; j < 128; j++) p.fine[j] = static_cast<u32>(std::floor(32768.0 * std::exp2(j / 1536.0)));
        return p;
    }();
    return t;
}

// The pitch `f` 1/128 semitones away from a tone's centre: 0x1000 * 2^(f / 1536), as libsd works it out: the product of
// the semitone's and the fine step's table entries (0x4000 at the centre, two octaves up), shifted down for each octave
// below that with rounding. libsd leaves the octaves above two octaves up unshifted (its answer stays 16 bits): `wide`
// shifts them up too, for a voice's pitch, which is clamped to the synthesizer's 0x3FFF afterwards.
u64 pitch_from_fine(s64 f, bool wide)
{
    const PitchTables& t = pitch_tables();
    const s64 semis = floor_div(f, 128), step = f - semis * 128;
    const s64 octave = floor_div(semis, 12), semitone = semis - octave * 12;
    u64 p = (u64{t.semitone[static_cast<usize>(semitone)]} * t.fine[static_cast<usize>(step)]) >> 16;
    const s64 shift = octave - 2;
    if (shift < 0) {
        if (shift < -40) return 0;
        const u32 k = static_cast<u32>(-shift);
        p = (p + (u64{1} << (k - 1))) >> k;
    } else if (wide && shift > 0) {
        p = shift >= 24 ? ~u64{0} >> 8 : p << shift;
    }
    return p;
}

// u16 cellSnd3Note2Pitch(u16 center_note, u16 center_fine, u16 note, s16 fine): libsd's sceSdNote2Pitch. The centre's
// fine tune is added to the note's, as libsd adds it.
u32 cellSnd3Note2Pitch(PPUContext&, u16 center_note, u16 center_fine, u16 note, s16 fine)
{
    const s64 f = (s64{note} - center_note) * 128 + fine + center_fine;
    return static_cast<u16>(pitch_from_fine(f, false));
}

// u16 cellSnd3Pitch2Note(u16 center_note, u16 center_fine, u16 pitch): libsd's sceSdPitch2Note, the note in the high
// byte and its fine tune (1/128 semitones, even) in the low: the pitch (at most 0x3FFF) is taken as a power of two
// times a mantissa in [0x8000, 0x10000), the mantissa's semitone is the last table entry not above it, and the fine
// step the last not above what is left; the centre's fine tune and one are added to the step, and the note carries
// what passes 128.
u32 cellSnd3Pitch2Note(PPUContext&, u16 center_note, u16 center_fine, u16 pitch)
{
    const PitchTables& t = pitch_tables();
    const u32 p = std::min<u32>(pitch, kMaxPitch);
    u32 bit = 0;
    for (u32 i = 0; i < 14; i++)
        if (p & (1u << i)) bit = i;
    const u32 mantissa = (p << (15 - bit)) & 0xFFFF;
    u32 semitone = 0;
    for (u32 i = 12; i-- > 0;)
        if (mantissa >= t.semitone[i]) {
            semitone = i;
            break;
        }
    const u32 rest = (mantissa * 0x8000) / t.semitone[semitone];
    u32 step = 0;
    for (u32 i = 128; i-- > 0;)
        if (rest >= t.fine[i]) {
            step = i;
            break;
        }
    const u32 v = (center_fine + step + 1) & 0xFFFF;
    const s32 n = static_cast<s32>(center_note) + static_cast<s32>(semitone) + (static_cast<s32>(bit) - 12) * 12 +
        static_cast<s32>(v / 128);
    return static_cast<u16>((static_cast<u32>(n * 256) + (v & 0x7E)) & 0xFFFE);
}

// ------------------------------------------------------------------------------------------------- HD3 banks (D-641)

constexpr u32 kP3HD = 0x50334844, kP3PG = 0x50335047, kP3TN = 0x5033544E, kP3VA = 0x50335641;
constexpr u32 kHd3Version = 0x00020000;
constexpr u32 kNoEntry = 0xFFFFFFFF;
constexpr u32 kVagEntrySize = 16, kToneSize = 42, kProgramSize = 36, kSplitSize = 20;
constexpr u32 kMaxEntries = 0x10000; // a tone names its VAG, and a split its tone, with 16 bits
constexpr u32 kMaxSplits = 0x10000;  // in all the bank's programs (their offset tables may share one program's splits)

struct Vag {
    bool present = false; // a sample rate of 0 marks an empty entry (vgmstream: the table's padding)
    u32 offset = 0, rate = 0, size = 0;
};

// A tone: the PS2 bank's Sample block (D-641), what libsnd3 plays of it.
struct Tone {
    bool present = false;
    u16 vag = 0;
    u8 vel_low = 0, vel_high = 127; // the velocities a program's note plays it at
    u8 base_note = 60;              // the note the sample plays at its own rate
    s8 detune = 0;                  // 1/128 semitones
    u8 pan = 64;                    // 0-127, 64 in the middle
    u8 priority = 0;                // a busier voice of a lower priority is taken first
    u8 volume = 127;
    u16 adsr1 = 0, adsr2 = 0; // the SPU2's envelope registers as they are
    u8 spu_attr = 0;          // 0x04 and 0x08: sent to the effect, left and right
};

struct Split {
    u16 tone = 0;
    u8 low = 0, high = 127;           // the notes it plays
    u16 bend_low = 0, bend_high = 0;  // pitch bend range down and up, 1/128 semitones (0: two semitones)
    u8 volume = 127, pan = 64;
    s8 transpose = 0, detune = 0;
};

struct Program {
    bool present = false;
    u8 volume = 127, pan = 64;
    s8 transpose = 0, detune = 0;
    std::vector<Split> splits;
};

struct Bank {
    bool bound = false;
    u32 ctx = 0, hd3 = 0, synth_offset = 0;
    std::vector<Vag> vags;
    std::vector<Tone> tones;
    std::vector<Program> programs;
};

// A chunk of the bank: its magic at +0, the size of the rest at +4, its highest entry's index at +0x14 and its table from
// +0x20 (P3VA as vgmstream reads it; P3TN and P3PG taken to be laid out the same, D-641). `count` its entries.
struct Chunk {
    u32 at = 0, end = 0, count = 0;
};

bool read_chunk(std::span<const u8> d, u32 at, u32 magic, const char* name, Chunk& c, std::string& why)
{
    if (u64{at} + 0x20 > d.size()) {
        why = std::format("{} at {:#x} is past the bank's end ({:#x} bytes)", name, at, d.size());
        return false;
    }
    if (be32(d, at) != magic) {
        why = std::format("no {} at {:#x} ({:#010x})", name, at, be32(d, at));
        return false;
    }
    const u64 end = u64{at} + 8 + be32(d, at + 4);
    if (end > d.size() || end < u64{at} + 0x20) {
        why = std::format("{} at {:#x}: size {:#x} past the bank's end", name, at, be32(d, at + 4));
        return false;
    }
    const u64 count = u64{be32(d, at + 0x14)} + 1;
    if (count > kMaxEntries) {
        why = std::format("{}: {} entries", name, count);
        return false;
    }
    c = {at, static_cast<u32>(end), static_cast<u32>(count)};
    return true;
}

// An offset table's entry `i` (relative to the chunk): 0 when there is none (0 and 0xFFFFFFFF both mark an empty one).
bool table_entry(std::span<const u8> d, const Chunk& c, u32 i, u32 record, const char* name, u32& at, std::string& why)
{
    const u64 slot = u64{c.at} + 0x20 + 4 * u64{i};
    if (slot + 4 > c.end) {
        why = std::format("{}: the offset of entry {} is past the chunk", name, i);
        return false;
    }
    const u32 off = be32(d, static_cast<usize>(slot));
    if (off == 0 || off == kNoEntry) {
        at = 0;
        return true;
    }
    if (u64{c.at} + off + record > c.end) {
        why = std::format("{}: entry {} at {:#x} is past the chunk", name, i, off);
        return false;
    }
    at = c.at + off;
    return true;
}

u8 pan_byte(u8 p) { return std::min<u8>(p, 127); }

// The bank's tables from its HD3 header `d`, or false with the reason.
bool parse_hd3(std::span<const u8> d, Bank& bank, std::string& why)
{
    if (d.size() < 0x20 || be32(d, 0) != kP3HD) {
        why = "no P3HD header";
        return false;
    }
    if (be32(d, 8) != kHd3Version) PS_LOG_INFO(kChan, "HD3 version {:#010x} (the known one is {:#010x})", be32(d, 8), kHd3Version);
    const u32 pg = be32(d, 0x10), tn = be32(d, 0x14), va = be32(d, 0x18);

    Chunk vc;
    if (!read_chunk(d, va, kP3VA, "P3VA", vc, why)) return false;
    if (u64{vc.at} + 0x20 + u64{vc.count} * kVagEntrySize > vc.end) {
        why = std::format("P3VA: {} entries past the chunk", vc.count);
        return false;
    }
    bank.vags.assign(vc.count, {});
    for (u32 i = 0; i < vc.count; i++) {
        const u32 e = vc.at + 0x20 + i * kVagEntrySize;
        Vag& v = bank.vags[i];
        v.offset = be32(d, e);
        v.rate = be32(d, e + 4);
        v.size = be32(d, e + 8);
        v.present = v.rate != 0;
        if (v.present && (u64{v.offset} + v.size > kSynthMemory || v.rate > 0x100000)) {
            why = std::format("P3VA: VAG {} (offset {:#x}, {:#x} bytes, {} Hz) does not fit the synthesizer", i, v.offset,
                v.size, v.rate);
            return false;
        }
    }

    Chunk tc;
    if (!read_chunk(d, tn, kP3TN, "P3TN", tc, why)) return false;
    bank.tones.assign(tc.count, {});
    for (u32 i = 0; i < tc.count; i++) {
        u32 at = 0;
        if (!table_entry(d, tc, i, kToneSize, "P3TN", at, why)) return false;
        if (!at) continue;
        // The PS2 Sample block's layout (D-641): VAG index, velocity range low, cross fade, high, velocity follows (6),
        // base note, detune, pan, group, priority, volume, (1), ADSR1, ADSR2, key follows (10), LFO (10).
        Tone& t = bank.tones[i];
        t.vag = be16(d, at);
        t.vel_low = d[at + 2];
        t.vel_high = d[at + 4];
        t.base_note = d[at + 11];
        t.detune = static_cast<s8>(d[at + 12]);
        t.pan = pan_byte(d[at + 13]);
        t.priority = d[at + 15];
        t.volume = d[at + 16];
        t.adsr1 = be16(d, at + 18);
        t.adsr2 = be16(d, at + 20);
        t.spu_attr = d[at + 41];
        if (t.vag >= bank.vags.size() || !bank.vags[t.vag].present || t.base_note > 127 || t.volume > 127) {
            why = std::format("P3TN: tone {} (VAG {}, base note {}, volume {}) is not one", i, t.vag, t.base_note, t.volume);
            return false;
        }
        if (!t.vel_low && !t.vel_high) t.vel_high = 127;
        t.present = true;
    }

    if (pg == 0 || pg == kNoEntry) return true; // no programs: only the tones play
    Chunk pc;
    if (!read_chunk(d, pg, kP3PG, "P3PG", pc, why)) return false;
    bank.programs.assign(pc.count, {});
    u32 total = 0;
    for (u32 i = 0; i < pc.count; i++) {
        u32 at = 0;
        if (!table_entry(d, pc, i, kProgramSize, "P3PG", at, why)) return false;
        if (!at) continue;
        // The PS2 Program block (D-641): split blocks' offset (from the program), their number and size, volume, pan,
        // transpose, detune, then key follow pan and the LFO, not played.
        Program& p = bank.programs[i];
        const u32 splits = be32(d, at);
        const u32 n = d[at + 4], size = d[at + 5];
        p.volume = d[at + 6];
        p.pan = pan_byte(d[at + 7]);
        p.transpose = static_cast<s8>(d[at + 8]);
        p.detune = static_cast<s8>(d[at + 9]);
        if (p.volume > 127 || (n && size < kSplitSize) || u64{at} + splits + u64{n} * size > pc.end) {
            why = std::format("P3PG: program {} ({} splits of {} bytes at {:#x}, volume {}) is not one", i, n, size, splits,
                p.volume);
            return false;
        }
        if ((total += n) > kMaxSplits) {
            why = std::format("P3PG: more than {} splits", kMaxSplits);
            return false;
        }
        for (u32 k = 0; k < n; k++) {
            const u32 s = at + splits + k * size;
            Split sp;
            sp.tone = be16(d, s);
            sp.low = d[s + 2];
            sp.high = d[s + 4];
            sp.bend_low = be16(d, s + 6);
            sp.bend_high = be16(d, s + 8);
            sp.volume = d[s + 16];
            sp.pan = pan_byte(d[s + 17]);
            sp.transpose = static_cast<s8>(d[s + 18]);
            sp.detune = static_cast<s8>(d[s + 19]);
            if (sp.high < sp.low) sp.high = 127; // as VGMTrans reads a PS2 bank's split
            if (sp.tone >= bank.tones.size() || !bank.tones[sp.tone].present || sp.low > 127 || sp.high > 127 ||
                sp.volume > 127) {
                why = std::format("P3PG: program {} split {} (tone {}, notes {}-{}, volume {}) is not one", i, k, sp.tone,
                    sp.low, sp.high, sp.volume);
                return false;
            }
            p.splits.push_back(sp);
        }
        p.present = true;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------- SMF (D-642)

constexpr u32 kMThd = 0x4D546864, kMTrk = 0x4D54726B;
constexpr u32 kDefaultTempo = 500000; // microseconds a quarter note: 120 BPM
constexpr u8 kTempoEvent = 0xFF;      // a tempo change among the events (its microseconds in `value`)

struct SmfEvent {
    u32 tick = 0;
    u32 value = 0; // a tempo's microseconds
    u8 status = 0, a = 0, b = 0;
};

struct Song {
    u32 division = 0; // ticks a quarter note
    u32 initial_tempo = kDefaultTempo;
    u64 end = 0; // the last track's end, in ticks
    std::vector<SmfEvent> events;
};

bool read_vlq(std::span<const u8> d, usize& i, u32& v)
{
    v = 0;
    for (u32 n = 0; n < 4; n++) {
        if (i >= d.size()) return false;
        const u8 c = d[i++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) return true;
    }
    return false; // more than four bytes
}

// One MTrk chunk's events, appended to `out`; `end` its end of track (or last event) if later.
bool parse_track(std::span<const u8> d, u32 track, std::vector<SmfEvent>& out, u64& end, std::string& why)
{
    usize i = 0;
    u64 tick = 0;
    u8 running = 0;
    const auto bad = [&](const char* what) {
        why = std::format("track {}: {} at byte {:#x}", track, what, i);
        return false;
    };
    while (i < d.size()) {
        u32 delta = 0;
        if (!read_vlq(d, i, delta)) return bad("a delta time cut short");
        tick += delta;
        if (tick > 0xFFFFFFFFu) return bad("a time past 2^32 ticks");
        if (i >= d.size()) return bad("a delta time with no event");
        u8 status = d[i];
        if (status & 0x80) {
            i++;
        } else {
            if (!running) return bad("a data byte with no running status");
            status = running;
        }
        if (status < 0xF0) {
            running = status;
            const u32 n = (status & 0xE0) == 0xC0 ? 1 : 2; // program change, channel pressure: one data byte
            if (i + n > d.size()) return bad("an event cut short");
            const u8 a = d[i], b = n == 2 ? d[i + 1] : 0;
            if ((a | b) & 0x80) return bad("a data byte with its high bit set");
            i += n;
            const u8 type = status & 0xF0;
            if (type != 0xA0 && type != 0xD0) // key and channel pressure are not played
                out.push_back({static_cast<u32>(tick), 0, status, a, b});
        } else if (status == 0xFF) {
            running = 0;
            if (i >= d.size()) return bad("a meta event cut short");
            const u8 type = d[i++];
            u32 len = 0;
            if (!read_vlq(d, i, len) || len > d.size() - i) return bad("a meta event cut short");
            if (type == 0x2F) break; // end of track
            if (type == 0x51 && len >= 3) {
                const u32 us = (u32{d[i]} << 16) | (u32{d[i + 1]} << 8) | d[i + 2];
                if (us) out.push_back({static_cast<u32>(tick), us, kTempoEvent, 0, 0});
            }
            i += len;
        } else if (status == 0xF0 || status == 0xF7) {
            running = 0;
            u32 len = 0;
            if (!read_vlq(d, i, len) || len > d.size() - i) return bad("a system exclusive event cut short");
            i += len;
        } else {
            return bad("a system message in a track");
        }
    }
    end = std::max(end, tick);
    return true;
}

// The SMF at `addr`, read chunk by chunk (an SMF gives its own size), or false with the reason.
bool read_smf(lv2::PpuThread& t, u8* base, u32 addr, Song& song, std::string& why)
{
    if (!guest_range(t, addr, 14) || ps_read32(base, addr) != kMThd) {
        why = "no MThd chunk";
        return false;
    }
    const u32 hlen = ps_read32(base, addr + 4);
    const u32 format = ps_read16(base, addr + 8), tracks = ps_read16(base, addr + 10), division = ps_read16(base, addr + 12);
    if (hlen < 6 || hlen > 0x1000) {
        why = std::format("an MThd of {} bytes", hlen);
        return false;
    }
    if (format > 1) {
        why = std::format("format {} (independent sequences) is not played", format);
        return false;
    }
    if (!tracks || !division || (division & 0x8000)) {
        why = std::format("{} tracks, division {:#06x} (SMPTE time is not played)", tracks, division);
        return false;
    }
    song.division = division;
    u64 at = u64{addr} + 8 + hlen;
    u32 found = 0, others = 0;
    std::vector<u8> bytes;
    while (found < tracks) {
        if (at + 8 - addr > kMaxSmfSize || !guest_range(t, static_cast<u32>(at), 8)) {
            why = std::format("track {} of {} is not there", found, tracks);
            return false;
        }
        const u32 type = ps_read32(base, static_cast<u32>(at)), len = ps_read32(base, static_cast<u32>(at) + 4);
        if (at + 8 + len - addr > kMaxSmfSize || !guest_range(t, static_cast<u32>(at) + 8, len)) {
            why = std::format("a chunk of {} bytes past the file", len);
            return false;
        }
        if (type == kMTrk) {
            bytes.assign(base + at + 8, base + at + 8 + len);
            if (!parse_track(bytes, found, song.events, song.end, why)) return false;
            found++;
        } else if (++others > 64) {
            why = "too many chunks that are not tracks";
            return false;
        }
        at += 8 + len;
    }
    // Every track at once: the events in time order, a track's before the next track's at the same time.
    std::stable_sort(song.events.begin(), song.events.end(),
        [](const SmfEvent& x, const SmfEvent& y) { return x.tick < y.tick; });
    for (const SmfEvent& e : song.events) {
        if (e.tick) break;
        if (e.status == kTempoEvent) song.initial_tempo = e.value;
    }
    return true;
}

// A MIDI channel's state while an SMF plays.
struct Channel {
    u8 program = 0, bank = 0;
    u8 volume = 100, expression = 127, pan = 64; // MIDI's defaults
    bool sustain = false;
    u16 bend = 0x2000;
    bool bend_range_set = false; // RPN 0 seen: its range, else each split's
    u8 bend_semitones = 2, bend_cents = 0;
    u16 rpn = 0x3FFF; // the RPN data entry goes to (0x3FFF: none)
    u16 bend_range() const { return static_cast<u16>(bend_semitones * 128 + bend_cents * 128 / 100); }
};

struct Smf {
    bool bound = false;
    u32 ctx = 0, data = 0, bank = 0;
    Song song;
    s32 status = kSmfStopped;
    u32 velocity = 127, pan = 64, pan_ex = 64;
    u32 play_count = 1, plays = 0; // play_count 0: for ever
    u32 mask = 0xFFFF;             // the channels that play
    s32 add_tempo = 0;             // beats a minute added (cellSnd3SMFAddTempo)
    // Where it is: the next event, the tick reached and the time past it, in units of 1 / (10^6 * division) samples.
    usize next = 0;
    u64 tick = 0;
    u128 rem = 0;
    u32 tempo = kDefaultTempo; // the song's, microseconds a quarter note
    std::array<Channel, 16> channels{};
    bool missing_logged = false;
};

constexpr u32 smf_key_on_id(u32 smf, u32 channel) { return 0x80000000u | (smf << 8) | channel; }

// The tempo playing: the song's with cellSnd3SMFAddTempo's beats a minute added (at least one), in microseconds a
// quarter note.
u32 effective_tempo(const Smf& m)
{
    const double bpm = std::max(1.0, 60e6 / m.tempo + m.add_tempo);
    return static_cast<u32>(std::max(1.0, std::round(60e6 / bpm)));
}

// ------------------------------------------------------------------------------------------------- voices (D-640)

enum class Use : u8 { Free, On, Off };

struct Voice {
    Use use = Use::Free;
    bool reserved = false;               // cellSnd3VoiceSetReserveMode: for the game's own Voice* key-ons only
    bool hold = false, held_off = false; // sustain hold, and a key-off waiting for its end
    u32 gen = 0;                         // key-ons so far: a request for an earlier one is dropped
    u64 serial = 0;                      // when it was keyed on, to take the oldest first
    u32 blocks = 0;                      // blocks generated since its key-on
    u32 key_on_id = 0;
    s32 channel = -1, smf = -1; // the MIDI channel (NoteOn, an SMF) and the SMF playing it, or -1
    u8 note = 0;                // the note it was keyed on with
    u32 bank = 0;
    u16 tone = 0;
    u8 priority = 0;
    // Its loudness: the key-on's velocity and pan, the bank's volumes (tone, program, split) and pan offsets, and for an
    // SMF's voice its channel's volume, expression and pan.
    u8 velocity = 127, pan = 64, pan_ex = 64;
    std::array<u8, 3> volumes{127, 127, 127};
    s32 pan_offset = 0;
    u8 ch_volume = 127, ch_expression = 127, ch_pan = 64;
    // Its pitch: a note on the tone's base note, or a pitch given (KeyOnByTone); fine offsets (detunes, addPitch), the
    // bend and its range; the sample's rate.
    bool by_pitch = false;
    u32 key_pitch = 0x1000;
    s32 play_note = 60, base_note = 60;
    s32 fine = 0, add_pitch = 0;
    u16 bend = 0x2000, bend_up = 256, bend_down = 256;
    u32 rate = kRate;
    u16 envelope = 0; // ENVX after the last block
};

// A request of the queue (CellSnd3RequestQueueCtx): what libsynth2 is to be told.
struct Request {
    enum class Kind : u8 { KeyOn, Params, KeyOff, Effect } kind = Kind::KeyOn;
    u8 voice = 0;
    u32 gen = 0;
    u16 pitch = 0, voll = 0, volr = 0, adsr1 = 0, adsr2 = 0;
    u32 ssa = 0;
    u8 effect = 0; // KeyOn: bit 0 the left effect send, bit 1 the right; Effect: the mode
    s16 depth = 0;
    u16 delay = 0, feedback = 0;
};

struct Batch {
    std::vector<Request> front, rear;
};

struct State {
    std::mutex mutex;
    bool initialized = false, own_synth = false, missing_logged = false;
    u32 max_voice = 0, samples = 0, output = kOutputStereo;
    bool queued = false;
    usize front_cap = 0, rear_cap = 0;
    std::vector<Request> front, rear;
    std::array<Voice, kSynthVoices> voices{};
    u64 serial = 0;
    std::array<Bank, kMaxBanks> banks{};
    std::array<Smf, kMaxSmf> smfs{};
    std::array<u32, kCores> vmixel{}, vmixer{}; // the cores' effect send switches
};

State& state() { return HleProcess::current()->state<State>(); }

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// ------------------------------------------------------------------------------------------------- libsynth2

// libsynth2's function `name`: the firmware module's when the loader linked one (add_imports), else pspocket's own;
// r3. `found` false when the process has neither.
u64 synth(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args, bool* found = nullptr)
{
    const u32 nid = recomp::nid_of(name);
    if (found) *found = true;
    if (const u32 opd = HleProcess::current()->imported(kSynth, nid)) return call_guest(ctx, opd, args);
    if (const Function* f = registry().find(kSynth, nid)) {
        u64 saved[8];
        for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
        u32 i = 3;
        for (const u64 a : args) ctx.gpr[i++] = a;
        f->fn(ctx, gb(ctx));
        const u64 r = ctx.gpr[3];
        for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
        return r;
    }
    if (found) *found = false;
    return 0;
}

void set_param(PPUContext& ctx, u16 reg, u16 value) { synth(ctx, "cellSoundSynth2SetParam", {reg, value}); }
void set_switch(PPUContext& ctx, u16 reg, u32 value) { synth(ctx, "cellSoundSynth2SetSwitch", {reg, value}); }

// A voice's registers from what makes them (D-643).
u16 voice_pitch(const Voice& v)
{
    const s32 bend = v.bend >= 0x2000 ? (static_cast<s32>(v.bend) - 0x2000) * v.bend_up / 0x2000
                                      : -((0x2000 - static_cast<s32>(v.bend)) * v.bend_down / 0x2000);
    const s64 f = s64{v.fine} + v.add_pitch + bend;
    u128 p = 0;
    if (v.by_pitch) p = (u128{v.key_pitch} * pitch_from_fine(f, true) + 0x800) >> 12;
    else p = pitch_from_fine(s64{v.play_note - v.base_note} * 128 + f, true);
    p = (p * v.rate + kRate / 2) / kRate;
    return static_cast<u16>(std::min<u128>(p, kMaxPitch));
}

void voice_volume(const State& s, const Voice& v, u16& left, u16& right)
{
    // 0x3FFF times each 0-127 factor over 127.
    u128 num = kMaxVolume, den = 1;
    const auto factor = [&](u32 f) {
        num *= f;
        den *= 127;
    };
    factor(v.velocity);
    for (const u8 f : v.volumes) factor(f);
    s32 pan = v.pan + v.pan_offset;
    if (v.smf >= 0) {
        const Smf& m = s.smfs[static_cast<usize>(v.smf)];
        factor(v.ch_volume);
        factor(v.ch_expression);
        factor(m.velocity);
        pan += (v.ch_pan - 64) + (static_cast<s32>(m.pan) - 64);
    }
    const u32 amp = static_cast<u32>((num + den / 2) / den);
    if (s.output == kOutputMono) {
        left = right = static_cast<u16>(amp);
        return;
    }
    pan = std::clamp(pan, 0, 127);
    left = static_cast<u16>((amp * static_cast<u32>(std::min(127 - pan, 63)) + 31) / 63);
    right = static_cast<u16>((amp * static_cast<u32>(std::min(pan, 64)) + 32) / 64);
}

Request params_request(const State& s, u32 n)
{
    const Voice& v = s.voices[n];
    Request r;
    r.kind = Request::Kind::Params;
    r.voice = static_cast<u8>(n);
    r.gen = v.gen;
    r.pitch = voice_pitch(v);
    voice_volume(s, v, r.voll, r.volr);
    return r;
}

// Tells libsynth2: each request in order, the cores' effect sends, then the key-ons (or key-offs) of the list at once.
void apply(PPUContext& ctx, State& s, const std::vector<Request>& list)
{
    std::array<u32, kCores> kon{}, koff{};
    std::array<bool, kCores> mix{};
    for (const Request& r : list) {
        const u32 core = r.voice / kVoicesPerCore, v = r.voice % kVoicesPerCore;
        if (r.kind != Request::Kind::Effect && s.voices[r.voice].gen != r.gen) continue; // an earlier key-on's
        switch (r.kind) {
        case Request::Kind::KeyOn: {
            const u16 at = sd_voice(core, v);
            set_param(ctx, at | kPitch, r.pitch);
            set_param(ctx, at | kVoll, r.voll);
            set_param(ctx, at | kVolr, r.volr);
            set_param(ctx, at | kAdsr1, r.adsr1);
            set_param(ctx, at | kAdsr2, r.adsr2);
            synth(ctx, "cellSoundSynth2SetAddr", {static_cast<u16>(at | kSsa), r.ssa});
            const u32 bit = 1u << v;
            const u32 el = (r.effect & 1) ? s.vmixel[core] | bit : s.vmixel[core] & ~bit;
            const u32 er = (r.effect & 2) ? s.vmixer[core] | bit : s.vmixer[core] & ~bit;
            if (el != s.vmixel[core] || er != s.vmixer[core]) mix[core] = true;
            s.vmixel[core] = el;
            s.vmixer[core] = er;
            kon[core] |= bit;
            koff[core] &= ~bit;
            break;
        }
        case Request::Kind::Params: {
            const u16 at = sd_voice(core, v);
            set_param(ctx, at | kPitch, r.pitch);
            set_param(ctx, at | kVoll, r.voll);
            set_param(ctx, at | kVolr, r.volr);
            break;
        }
        case Request::Kind::KeyOff: koff[core] |= 1u << v; break;
        case Request::Kind::Effect: {
            // cellSoundSynth2SetEffectAttr(core, {core, mode, depth_L, depth_R, delay, feedback}) for each core, the
            // mode with libsd's CLEAR (the work area zeroed: no echo of the last mode in the new one), its effect on
            // or off.
            u8* base = gb(ctx);
            for (u32 c = 0; c < kCores; c++) {
                StackVar attr(ctx, base, 12);
                ps_write16(base, attr.addr(), static_cast<u16>(c));
                ps_write16(base, attr.addr() + 2, static_cast<u16>(kEffectClear | r.effect));
                ps_write16(base, attr.addr() + 4, static_cast<u16>(r.depth));
                ps_write16(base, attr.addr() + 6, static_cast<u16>(r.depth));
                ps_write16(base, attr.addr() + 8, r.delay);
                ps_write16(base, attr.addr() + 10, r.feedback);
                synth(ctx, "cellSoundSynth2SetEffectAttr", {c, attr.addr()});
                synth(ctx, "cellSoundSynth2SetCoreAttr", {kCoreEffectEnable | c, r.effect ? 1u : 0u});
            }
            break;
        }
        }
    }
    for (u32 c = 0; c < kCores; c++) {
        if (mix[c]) {
            set_switch(ctx, static_cast<u16>(kVmixel | c), s.vmixel[c]);
            set_switch(ctx, static_cast<u16>(kVmixer | c), s.vmixer[c]);
        }
        if (kon[c]) set_switch(ctx, static_cast<u16>(kKon | c), kon[c]);
        if (koff[c]) set_switch(ctx, static_cast<u16>(kKoff | c), koff[c]);
    }
}

bool room(const State& s, const Batch& b)
{
    return !s.queued || (s.front.size() + b.front.size() <= s.front_cap && s.rear.size() + b.rear.size() <= s.rear_cap);
}

// An API call's requests: queued, or told now without a queue.
void commit(PPUContext& ctx, State& s, Batch& b)
{
    if (s.queued) {
        s.front.insert(s.front.end(), b.front.begin(), b.front.end());
        s.rear.insert(s.rear.end(), b.rear.begin(), b.rear.end());
        return;
    }
    if (!b.front.empty()) apply(ctx, s, b.front);
    if (!b.rear.empty()) apply(ctx, s, b.rear);
}

// Runs `op(batch)` on the voices; when the queue has no room for what it asks, the voices are as they were and the call
// fails with REQUESTQUEFULL. `op` returns its answer (negative: an error, nothing changed).
template <class Op>
s32 transact(PPUContext& ctx, State& s, Op&& op)
{
    const auto saved = s.voices;
    const u64 serial = s.serial;
    Batch b;
    const s32 r = op(b);
    if (r < 0) {
        s.voices = saved;
        s.serial = serial;
        return r;
    }
    if (!room(s, b)) {
        s.voices = saved;
        s.serial = serial;
        return error(CELL_SND3_ERROR_REQUESTQUEFULL);
    }
    commit(ctx, s, b);
    return r;
}

void free_voice(Voice& v)
{
    v.use = Use::Free;
    v.hold = v.held_off = false;
    v.key_on_id = 0;
    v.channel = v.smf = -1;
    v.envelope = 0;
}

// Keys a voice off (a rear request), unless its sustain hold keeps it on until the hold ends (`force`: ends that too).
// Whether it was keyed off.
bool key_off(State& s, Batch& b, u32 n, bool force)
{
    Voice& v = s.voices[n];
    if (v.use != Use::On) {
        if (force) v.hold = v.held_off = false;
        return false;
    }
    if (v.hold && !force) {
        v.held_off = true;
        return false;
    }
    v.use = Use::Off;
    v.hold = v.held_off = false;
    Request r;
    r.kind = Request::Kind::KeyOff;
    r.voice = static_cast<u8>(n);
    r.gen = v.gen;
    b.rear.push_back(r);
    return true;
}

// Sets a voice's sustain hold; ending it keys off a voice a key-off came to meanwhile.
void set_hold(State& s, Batch& b, u32 n, bool hold)
{
    Voice& v = s.voices[n];
    if (v.use == Use::Free) return;
    v.hold = hold;
    if (!hold && v.held_off) key_off(s, b, n, false);
}

// A voice for an automatic key-on (NoteOn, NoteOnByTone, KeyOnByTone, an SMF), or -1: a free one first, else a
// released one, else the playing one of the lowest priority not above the new note's, the oldest first; never a
// reserved voice (D-640).
s32 allocate(const State& s, u8 priority)
{
    s32 best = -1;
    for (u32 n = 0; n < s.max_voice; n++) {
        const Voice& v = s.voices[n];
        if (v.reserved) continue;
        if (v.use == Use::Free) return static_cast<s32>(n);
        if (v.use == Use::On && v.priority > priority) continue;
        if (best < 0) {
            best = static_cast<s32>(n);
            continue;
        }
        const Voice& b = s.voices[static_cast<usize>(best)];
        const auto rank = [](const Voice& x) { return x.use == Use::Off ? 0 : 1; };
        if (rank(v) != rank(b) ? rank(v) < rank(b)
                               : v.priority != b.priority ? v.priority < b.priority : v.serial < b.serial)
            best = static_cast<s32>(n);
    }
    return best;
}

// What a key-on plays: a tone of a bank (and the program and split it came from), at a note or a pitch.
struct KeyOn {
    u32 bank = 0;
    u16 tone = 0;
    const Program* program = nullptr;
    const Split* split = nullptr;
    bool by_pitch = false;
    u32 pitch = 0x1000;
    u8 note = 60;
    u8 velocity = 127, pan = 64, pan_ex = 64;
    s32 add_pitch = 0;
    u32 key_on_id = 0;
    s32 channel = -1, smf = -1;
    bool hold = false;
    const Channel* ch = nullptr; // an SMF's channel
};

// Keys voice `n` on with `k` (a front request).
void start_voice(State& s, Batch& b, u32 n, const KeyOn& k)
{
    const Bank& bank = s.banks[k.bank];
    const Tone& t = bank.tones[k.tone];
    const Vag& vag = bank.vags[t.vag];
    Voice& v = s.voices[n];
    const bool reserved = v.reserved;
    const u32 gen = v.gen;
    v = Voice{};
    v.reserved = reserved;
    v.gen = gen + 1; // a request still queued for the voice's last key-on is dropped
    v.use = Use::On;
    v.serial = ++s.serial;
    v.key_on_id = k.key_on_id;
    v.channel = k.channel;
    v.smf = k.smf;
    v.note = k.note;
    v.bank = k.bank;
    v.tone = k.tone;
    v.priority = t.priority;
    v.hold = k.hold;
    v.velocity = k.velocity;
    v.pan = k.pan;
    v.pan_ex = k.pan_ex;
    v.volumes = {t.volume, k.program ? k.program->volume : u8{127}, k.split ? k.split->volume : u8{127}};
    v.pan_offset = (t.pan - 64) + (k.program ? k.program->pan - 64 : 0) + (k.split ? k.split->pan - 64 : 0);
    v.by_pitch = k.by_pitch;
    v.key_pitch = k.pitch;
    v.play_note = std::clamp(k.note + (k.program ? k.program->transpose : 0) + (k.split ? k.split->transpose : 0), 0, 127);
    v.base_note = t.base_note;
    v.fine = t.detune + (k.program ? k.program->detune : 0) + (k.split ? k.split->detune : 0);
    v.add_pitch = k.add_pitch;
    if (k.split && k.split->bend_high) v.bend_up = k.split->bend_high;
    if (k.split && k.split->bend_low) v.bend_down = k.split->bend_low;
    if (k.ch) {
        v.ch_volume = k.ch->volume;
        v.ch_expression = k.ch->expression;
        v.ch_pan = k.ch->pan;
        v.bend = k.ch->bend;
        if (k.ch->bend_range_set) v.bend_up = v.bend_down = k.ch->bend_range();
    }
    v.rate = vag.rate;
    Request r;
    r.kind = Request::Kind::KeyOn;
    r.voice = static_cast<u8>(n);
    r.pitch = voice_pitch(v);
    voice_volume(s, v, r.voll, r.volr);
    r.adsr1 = t.adsr1;
    r.adsr2 = t.adsr2;
    r.ssa = bank.synth_offset + vag.offset;
    r.effect = static_cast<u8>(((t.spu_attr & 0x04) ? 1 : 0) | ((t.spu_attr & 0x08) ? 2 : 0));
    r.gen = v.gen;
    b.front.push_back(r);
}

// The program `index`'s splits that play note `note` at `velocity`, each on a voice of its own (none left: the note
// plays on fewer, as a MIDI synthesizer's would).
void program_note_on(State& s, Batch& b, KeyOn k, const Program& p)
{
    const Bank& bank = s.banks[k.bank];
    k.program = &p;
    for (const Split& sp : p.splits) {
        if (k.note < sp.low || k.note > sp.high) continue;
        const Tone& t = bank.tones[sp.tone];
        if (k.velocity < t.vel_low || k.velocity > t.vel_high) continue;
        const s32 n = allocate(s, t.priority);
        if (n < 0) {
            PS_LOG_DEBUG(kChan, "no voice for note {} of tone {}", k.note, sp.tone);
            continue;
        }
        k.tone = sp.tone;
        k.split = &sp;
        start_voice(s, b, static_cast<u32>(n), k);
    }
}

// ------------------------------------------------------------------------------------------------- checks

// The library initialised, else NOTINIT.
s32 check_init(const State& s) { return s.initialized ? lv2::CELL_OK : error(CELL_SND3_ERROR_NOTINIT); }

s32 check_voice(const State& s, u32 voice)
{
    if (!s.initialized) return error(CELL_SND3_ERROR_NOTINIT);
    return voice < s.max_voice ? lv2::CELL_OK : error(CELL_SND3_ERROR_VOICENUM);
}

const Bank* bank_of(const State& s, u32 id) { return id < kMaxBanks && s.banks[id].bound ? &s.banks[id] : nullptr; }
Smf* smf_of(State& s, u32 id) { return id < kMaxSmf && s.smfs[id].bound ? &s.smfs[id] : nullptr; }

// A CellSnd3KeyOnParam: NULL for the defaults (velocity 127, pan and front-rear pan in the middle, no pitch offset);
// one that is not guest memory, or a value past 127, is PARAM.
s32 read_key_on_param(PPUContext& ctx, u32 addr, KeyOn& k)
{
    if (!addr) return lv2::CELL_OK;
    if (!guest_readable(thread_of(ctx), addr, kKeyOnParamSize)) return error(CELL_SND3_ERROR_PARAM);
    u8* base = gb(ctx);
    k.velocity = ps_read8(base, addr);
    k.pan = ps_read8(base, addr + 1);
    k.pan_ex = ps_read8(base, addr + 2);
    k.add_pitch = static_cast<s32>(ps_read32(base, addr + 4));
    if (k.velocity > 127 || k.pan > 127 || k.pan_ex > 127) return error(CELL_SND3_ERROR_PARAM);
    return lv2::CELL_OK;
}

// A tone key-on's common checks: the bank, the tone, the note or pitch, the key-on parameter.
s32 tone_key_on(PPUContext& ctx, State& s, u32 hd3, u32 tone, u32 note_or_pitch, bool by_pitch, u32 id, u32 param,
    KeyOn& k)
{
    if (const s32 r = check_init(s); r < 0) return r;
    const Bank* bank = bank_of(s, hd3);
    if (!bank) return error(CELL_SND3_ERROR_HD3ID);
    if (tone >= bank->tones.size() || !bank->tones[tone].present) return error(CELL_SND3_ERROR_PARAM);
    if (!by_pitch && note_or_pitch > 127) return error(CELL_SND3_ERROR_PARAM);
    if (const s32 r = read_key_on_param(ctx, param, k); r < 0) return r;
    k.bank = hd3;
    k.tone = static_cast<u16>(tone);
    k.by_pitch = by_pitch;
    if (by_pitch) k.pitch = note_or_pitch;
    else k.note = static_cast<u8>(note_or_pitch);
    k.key_on_id = id;
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- the library's life

// int cellSnd3Init(u32 maxVoice, u32 samples, CellSnd3RequestQueueCtx *queue): voices 0 to maxVoice - 1 (at most 48)
// are the library's, `samples` the frames each Synthesis makes, `queue` the request queue's two buffers (NULL: no
// queue, requests told to libsynth2 at once). libsynth2 is initialised here (one the game initialised already is used
// as it is, and left to the game at Exit).
s32 cellSnd3Init(PPUContext& ctx, u32 max_voice, u32 samples, u32 queue)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return error(CELL_SND3_ERROR_ALREADY);
    if (!max_voice || max_voice > kSynthVoices || !samples || samples > 0xFFFF) return error(CELL_SND3_ERROR_PARAM);
    u32 front = 0, front_size = 0, rear = 0, rear_size = 0;
    if (queue) {
        lv2::PpuThread& t = thread_of(ctx);
        if (!guest_readable(t, queue, kQueueCtxSize)) return error(CELL_SND3_ERROR_PARAM);
        u8* base = gb(ctx);
        front = ps_read32(base, queue);
        front_size = ps_read32(base, queue + 4);
        rear = ps_read32(base, queue + 8);
        rear_size = ps_read32(base, queue + 12);
        if (!front || !rear || front_size < kRequestSize || rear_size < kRequestSize || !guest_range(t, front, front_size) ||
            !guest_range(t, rear, rear_size))
            return error(CELL_SND3_ERROR_PARAM);
    }
    bool found = false;
    const u32 r = static_cast<u32>(synth(ctx, "cellSoundSynth2Init", {0}, &found));
    if (!found) {
        PS_LOG_ERROR(kChan, "cellSnd3Init: libsynth2 is not in this process");
        return error(CELL_SND3_ERROR_SYNTH);
    }
    if (r != lv2::CELL_OK && r != CELL_SOUND_SYNTH2_ERROR_ALREADY_INITIALIZED) {
        PS_LOG_ERROR(kChan, "cellSnd3Init: cellSoundSynth2Init failed ({:#010x})", r);
        return error(CELL_SND3_ERROR_SYNTH);
    }
    s.initialized = true;
    s.own_synth = r == lv2::CELL_OK;
    s.max_voice = max_voice;
    s.samples = samples;
    s.output = kOutputStereo;
    s.queued = queue != 0;
    s.front_cap = front_size / kRequestSize;
    s.rear_cap = rear_size / kRequestSize;
    s.front.clear();
    s.rear.clear();
    s.voices = {};
    s.serial = 0;
    s.banks = {};
    s.smfs = {};
    // The library's voices heard dry on both sides, sent to no effect, keyed off; the cores' master volume up. The other
    // voices (the game's own, through libsynth2) are left as they are.
    for (u32 c = 0; c < kCores; c++) {
        const u32 first = c * kVoicesPerCore;
        const u32 count = max_voice > first ? std::min(max_voice - first, kVoicesPerCore) : 0;
        const u32 ours = (1u << count) - 1;
        set_param(ctx, static_cast<u16>(kMvoll | c), kMaxVolume);
        set_param(ctx, static_cast<u16>(kMvolr | c), kMaxVolume);
        if (!ours) continue;
        const auto get = [&](u16 reg) { return static_cast<u32>(synth(ctx, "cellSoundSynth2GetSwitch", {reg})); };
        set_switch(ctx, static_cast<u16>(kVmixl | c), get(static_cast<u16>(kVmixl | c)) | ours);
        set_switch(ctx, static_cast<u16>(kVmixr | c), get(static_cast<u16>(kVmixr | c)) | ours);
        s.vmixel[c] = get(static_cast<u16>(kVmixel | c)) & ~ours;
        s.vmixer[c] = get(static_cast<u16>(kVmixer | c)) & ~ours;
        set_switch(ctx, static_cast<u16>(kVmixel | c), s.vmixel[c]);
        set_switch(ctx, static_cast<u16>(kVmixer | c), s.vmixer[c]);
        set_switch(ctx, static_cast<u16>(kKoff | c), ours);
    }
    PS_LOG_INFO(kChan, "cellSnd3Init: {} voices, {} samples a block, {}", max_voice, samples,
        queue ? std::format("a request queue of {} and {} requests", s.front_cap, s.rear_cap) : std::string("no request queue"));
    return lv2::CELL_OK;
}

// int cellSnd3Exit(void): every voice keyed off, every bank and SMF let go, the queue dropped; libsynth2 ended if the
// library started it.
s32 cellSnd3Exit(PPUContext& ctx)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    for (u32 c = 0; c < kCores; c++) {
        const u32 first = c * kVoicesPerCore;
        const u32 count = s.max_voice > first ? std::min(s.max_voice - first, kVoicesPerCore) : 0;
        if (count) set_switch(ctx, static_cast<u16>(kKoff | c), (1u << count) - 1);
    }
    if (s.own_synth) {
        const u32 r = static_cast<u32>(synth(ctx, "cellSoundSynth2Exit", {}));
        if (r != lv2::CELL_OK) PS_LOG_WARN(kChan, "cellSnd3Exit: cellSoundSynth2Exit answered {:#010x}", r);
    }
    s.initialized = s.own_synth = s.queued = false;
    s.front.clear();
    s.rear.clear();
    s.voices = {};
    s.banks = {};
    s.smfs = {};
    return lv2::CELL_OK;
}

// int cellSnd3SetOutputMode(u32 mode): stereo (0) or mono (1, both sides at the voice's full volume); the voices playing
// take the new mode.
s32 cellSnd3SetOutputMode(PPUContext& ctx, u32 mode)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    if (mode != kOutputStereo && mode != kOutputMono) return error(CELL_SND3_ERROR_OUTPUTMODE);
    const u32 old = s.output;
    s.output = mode;
    const s32 r = transact(ctx, s, [&](Batch& b) {
        for (u32 n = 0; n < s.max_voice; n++)
            if (s.voices[n].use != Use::Free) b.front.push_back(params_request(s, n));
        return lv2::CELL_OK;
    });
    if (r < 0) s.output = old;
    return r;
}

// int cellSnd3SetEffectType(u16 effectType, s16 returnVol, u16 delay, u16 feedback): libsd's effect (reverb) modes 0
// (off) to 9, on both cores, the effect returned at returnVol; delay and feedback (0-127) for the echo and delay modes.
// A tone sends to the effect as its bank says.
s32 cellSnd3SetEffectType(PPUContext& ctx, u16 type, s16 volume, u16 delay, u16 feedback)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    if (type >= kEffectModes || delay > 127 || feedback > 127) return error(CELL_SND3_ERROR_PARAM);
    return transact(ctx, s, [&](Batch& b) {
        Request r;
        r.kind = Request::Kind::Effect;
        r.effect = static_cast<u8>(type);
        r.depth = volume;
        r.delay = delay;
        r.feedback = feedback;
        b.front.push_back(r);
        return lv2::CELL_OK;
    });
}

// ------------------------------------------------------------------------------------------------- SMF playback

void smf_voices_off(State& s, Batch& b, u32 id, s32 channel, bool force)
{
    for (u32 n = 0; n < s.max_voice; n++) {
        const Voice& v = s.voices[n];
        if (v.smf == static_cast<s32>(id) && (channel < 0 || v.channel == channel)) key_off(s, b, n, force);
    }
}

void smf_rewind(Smf& m)
{
    m.next = 0;
    m.tick = 0;
    m.rem = 0;
    m.tempo = m.song.initial_tempo;
    m.channels = {};
}

// Each sounding voice of the SMF's channel `channel` (-1: all of them) told its volume and pitch again, `change` made to
// it first.
template <class Change>
void smf_update(State& s, Batch& b, u32 id, s32 channel, Change&& change)
{
    for (u32 n = 0; n < s.max_voice; n++) {
        Voice& v = s.voices[n];
        if (v.use == Use::Free || v.smf != static_cast<s32>(id) || (channel >= 0 && v.channel != channel)) continue;
        change(v);
        b.front.push_back(params_request(s, n));
    }
}

void smf_note_on(State& s, Batch& b, u32 id, Smf& m, u32 ch, u8 note, u8 velocity)
{
    if (!(m.mask & (1u << ch))) return;
    const Bank& bank = s.banks[m.bank];
    const Channel& c = m.channels[ch];
    const u32 index = u32{c.bank} * 128 + c.program;
    if (index >= bank.programs.size() || !bank.programs[index].present) {
        if (!m.missing_logged) {
            m.missing_logged = true;
            PS_LOG_WARN(kChan, "SMF {}: channel {} plays program {}, which bank {} does not have", id, ch, index, m.bank);
        }
        return;
    }
    KeyOn k;
    k.bank = m.bank;
    k.note = note;
    k.velocity = velocity;
    k.key_on_id = smf_key_on_id(id, ch);
    k.channel = static_cast<s32>(ch);
    k.smf = static_cast<s32>(id);
    k.hold = c.sustain;
    k.ch = &c;
    program_note_on(s, b, k, bank.programs[index]);
}

void smf_controller(State& s, Batch& b, u32 id, Smf& m, u32 ch, u8 cc, u8 value)
{
    Channel& c = m.channels[ch];
    const s32 channel = static_cast<s32>(ch);
    const auto bend_range = [&] {
        if (c.rpn != 0) return; // RPN 0: pitch bend sensitivity
        c.bend_range_set = true;
        const u16 range = c.bend_range();
        smf_update(s, b, id, channel, [&](Voice& v) { v.bend_up = v.bend_down = range; });
    };
    switch (cc) {
    case 0: c.bank = value; break;
    case 6:
        c.bend_semitones = c.rpn == 0 ? value : c.bend_semitones;
        bend_range();
        break;
    case 38:
        c.bend_cents = c.rpn == 0 ? value : c.bend_cents;
        bend_range();
        break;
    case 7:
        c.volume = value;
        smf_update(s, b, id, channel, [&](Voice& v) { v.ch_volume = value; });
        break;
    case 10:
        c.pan = value;
        smf_update(s, b, id, channel, [&](Voice& v) { v.ch_pan = value; });
        break;
    case 11:
        c.expression = value;
        smf_update(s, b, id, channel, [&](Voice& v) { v.ch_expression = value; });
        break;
    case 64:
        c.sustain = value >= 64;
        for (u32 n = 0; n < s.max_voice; n++)
            if (s.voices[n].smf == static_cast<s32>(id) && s.voices[n].channel == channel) set_hold(s, b, n, c.sustain);
        break;
    case 98:
    case 99: c.rpn = 0x3FFF; break; // an NRPN: data entry is not for the pitch bend range
    case 100: c.rpn = static_cast<u16>((c.rpn & 0x3F80) | value); break;
    case 101: c.rpn = static_cast<u16>((c.rpn & 0x7F) | (value << 7)); break;
    case 120: smf_voices_off(s, b, id, channel, true); break; // all sound off
    case 121:                                                 // reset all controllers
        c.expression = 127;
        c.bend = 0x2000;
        c.rpn = 0x3FFF;
        c.sustain = false;
        for (u32 n = 0; n < s.max_voice; n++)
            if (s.voices[n].smf == static_cast<s32>(id) && s.voices[n].channel == channel) set_hold(s, b, n, false);
        smf_update(s, b, id, channel, [&](Voice& v) {
            v.ch_expression = 127;
            v.bend = 0x2000;
        });
        break;
    case 123: smf_voices_off(s, b, id, channel, false); break; // all notes off
    default: break;
    }
}

void smf_event(State& s, Batch& b, u32 id, Smf& m, const SmfEvent& e)
{
    if (e.status == kTempoEvent) {
        m.tempo = e.value;
        return;
    }
    const u32 ch = e.status & 0x0F;
    switch (e.status & 0xF0) {
    case 0x90:
        if (e.b) {
            smf_note_on(s, b, id, m, ch, e.a, e.b);
            break;
        }
        [[fallthrough]]; // velocity 0: a note off
    case 0x80:
        for (u32 n = 0; n < s.max_voice; n++) {
            const Voice& v = s.voices[n];
            if (v.smf == static_cast<s32>(id) && v.channel == static_cast<s32>(ch) && v.note == e.a) key_off(s, b, n, false);
        }
        break;
    case 0xB0: smf_controller(s, b, id, m, ch, e.a, e.b); break;
    case 0xC0: m.channels[ch].program = e.a; break;
    case 0xE0: {
        const u16 bend = static_cast<u16>(e.a | (e.b << 7));
        m.channels[ch].bend = bend;
        smf_update(s, b, id, static_cast<s32>(ch), [&](Voice& v) { v.bend = bend; });
        break;
    }
    default: break;
    }
}

// Plays the SMF for one block of `samples` frames: the events that fall in it, the song's end (looping while plays are
// left), the time past the last event kept for the next block (D-642).
void smf_block(State& s, Batch& b, u32 id, Smf& m)
{
    u128 budget = u128{s.samples} * 1000000u * m.song.division;
    bool restarted = false;
    while (m.status == kSmfPlaying) {
        const u128 cost = u128{effective_tempo(m)} * kRate; // one tick, in units of 1 / (10^6 * division) samples
        const bool event = m.next < m.song.events.size();
        const u64 at = event ? m.song.events[m.next].tick : m.song.end;
        const u128 to = at > m.tick ? u128{at - m.tick} * cost : 0;
        const u128 need = to > m.rem ? to - m.rem : 0;
        if (need >= budget) {
            const u128 total = m.rem + budget;
            m.tick += static_cast<u64>(total / cost);
            m.rem = total % cost;
            return;
        }
        budget -= need;
        m.tick = std::max(m.tick, at);
        m.rem = 0;
        if (event) {
            smf_event(s, b, id, m, m.song.events[m.next++]);
            continue;
        }
        // The end of the song: its notes still on keyed off, held or not; again from the start while plays are left
        // (a song of no length plays once). A song shorter than a block starts again once a block at most: the rest of
        // the block is not played, so a song of a tick at a tempo of microseconds cannot hold the block up.
        smf_voices_off(s, b, id, -1, true);
        m.plays++;
        if ((m.play_count && m.plays >= m.play_count) || m.song.end == 0) {
            m.status = kSmfStopped;
            smf_rewind(m);
            return;
        }
        smf_rewind(m);
        if (restarted) return;
        restarted = true;
    }
}

// ------------------------------------------------------------------------------------------------- synthesis

// After a block: each voice's envelope; a released voice silent, or a playing one whose sample ended, is free again.
void update_voices(PPUContext& ctx, State& s)
{
    for (u32 n = 0; n < s.max_voice; n++) {
        Voice& v = s.voices[n];
        if (v.use == Use::Free) continue;
        const u16 env = static_cast<u16>(
            synth(ctx, "cellSoundSynth2GetParam", {static_cast<u16>(sd_voice(n / kVoicesPerCore, n % kVoicesPerCore) | kEnvx)}));
        v.envelope = env;
        if (env == 0 && (v.use == Use::Off || v.blocks > 0)) free_voice(v);
        else v.blocks++;
    }
}

s32 synthesis(PPUContext& ctx, u32 left, u32 right, u32 rear_left, u32 rear_right, bool four)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    lv2::PpuThread& t = thread_of(ctx);
    const u64 bytes = u64{s.samples} * 4;
    for (const u32 out : {left, right})
        if (!out || !guest_range(t, out, bytes)) return error(CELL_SND3_ERROR_PARAM);
    if (four)
        for (const u32 out : {rear_left, rear_right})
            if (!out || !guest_range(t, out, bytes)) return error(CELL_SND3_ERROR_PARAM);
    // The queue's front requests, then the SMFs' events in this block, the block, then the rear requests and the SMFs'
    // key-offs.
    std::vector<Request> front, rear;
    front.swap(s.front);
    rear.swap(s.rear);
    if (!front.empty()) apply(ctx, s, front);
    Batch smf;
    for (u32 id = 0; id < kMaxSmf; id++)
        if (s.smfs[id].bound && s.smfs[id].status == kSmfPlaying) smf_block(s, smf, id, s.smfs[id]);
    if (!smf.front.empty()) apply(ctx, s, smf.front);
    const u32 r = static_cast<u32>(synth(ctx, "cellSoundSynth2Generate",
        {static_cast<u16>(s.samples), left, right, four ? rear_left : 0u, four ? rear_right : 0u}));
    rear.insert(rear.end(), smf.rear.begin(), smf.rear.end());
    if (!rear.empty()) apply(ctx, s, rear);
    update_voices(ctx, s);
    if (r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "cellSoundSynth2Generate failed ({:#010x})", r);
        return error(CELL_SND3_ERROR_SYNTH);
    }
    return lv2::CELL_OK;
}

// int cellSnd3Synthesis(float *pOutL, float *pOutR): the next block, `samples` frames (cellSnd3Init) of each side.
s32 cellSnd3Synthesis(PPUContext& ctx, u32 left, u32 right) { return synthesis(ctx, left, right, 0, 0, false); }

// int cellSnd3SynthesisEx(float *pOutL, float *pOutR, float *pOutRL, float *pOutRR): the same with the rear pair
// libsynth2 generates.
s32 cellSnd3SynthesisEx(PPUContext& ctx, u32 left, u32 right, u32 rear_left, u32 rear_right)
{
    return synthesis(ctx, left, right, rear_left, rear_right, true);
}

// ------------------------------------------------------------------------------------------------- banks

// int cellSnd3BindSoundData(CellSnd3DataCtx *snd3Ctx, void *hd3, u32 synthMemOffset): the bank whose HD3 header is at
// `hd3`, its BD3 samples already in synthesizer memory at synthMemOffset (libsynth2's VoiceTrans); its hd3ID.
s32 cellSnd3BindSoundData(PPUContext& ctx, u32 data_ctx, u32 hd3, u32 synth_offset)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    lv2::PpuThread& t = thread_of(ctx);
    if (!data_ctx || !guest_readable(t, data_ctx, kDataCtxSize) || !hd3 || synth_offset >= kSynthMemory)
        return error(CELL_SND3_ERROR_PARAM);
    if (!guest_range(t, hd3, 8)) return error(CELL_SND3_ERROR_PARAM);
    u8* base = gb(ctx);
    if (ps_read32(base, hd3) != kP3HD) {
        PS_LOG_ERROR(kChan, "cellSnd3BindSoundData: no HD3 at {:#x} ({:#010x})", hd3, ps_read32(base, hd3));
        return error(CELL_SND3_ERROR_FORMAT);
    }
    const u64 size = u64{ps_read32(base, hd3 + 4)} + 8;
    if (size > kMaxHd3Size || !guest_range(t, hd3, size)) {
        PS_LOG_ERROR(kChan, "cellSnd3BindSoundData: an HD3 of {:#x} bytes at {:#x} is not in guest memory", size, hd3);
        return error(CELL_SND3_ERROR_FORMAT);
    }
    u32 slot = kMaxBanks;
    for (u32 i = 0; i < kMaxBanks; i++)
        if (!s.banks[i].bound) {
            slot = i;
            break;
        }
    if (slot == kMaxBanks) return error(CELL_SND3_ERROR_SOUNDDATAFULL);
    Bank bank;
    std::string why;
    const std::vector<u8> bytes(base + hd3, base + hd3 + size);
    if (!parse_hd3(bytes, bank, why)) {
        PS_LOG_ERROR(kChan, "cellSnd3BindSoundData: the HD3 at {:#x}: {}", hd3, why);
        return error(CELL_SND3_ERROR_FORMAT);
    }
    for (u32 i = 0; i < bank.vags.size(); i++) {
        const Vag& v = bank.vags[i];
        if (v.present && u64{synth_offset} + v.offset + v.size > kSynthMemory) {
            PS_LOG_ERROR(kChan, "cellSnd3BindSoundData: VAG {} ends past the synthesizer's memory at offset {:#x}", i,
                synth_offset);
            return error(CELL_SND3_ERROR_PARAM);
        }
    }
    bank.bound = true;
    bank.ctx = data_ctx;
    bank.hd3 = hd3;
    bank.synth_offset = synth_offset;
    PS_LOG_INFO(kChan, "bank {}: {} VAGs, {} tones, {} programs, its samples at {:#x}", slot, bank.vags.size(),
        bank.tones.size(), bank.programs.size(), synth_offset);
    s.banks[slot] = std::move(bank);
    return static_cast<s32>(slot);
}

// int cellSnd3UnbindSoundData(u32 hd3ID): its voices keyed off; the SMFs bound to it stop (and play again only once
// bound to a bank).
s32 cellSnd3UnbindSoundData(PPUContext& ctx, u32 hd3)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    if (!bank_of(s, hd3)) return error(CELL_SND3_ERROR_HD3ID);
    const s32 r = transact(ctx, s, [&](Batch& b) {
        for (u32 n = 0; n < s.max_voice; n++)
            if (s.voices[n].use != Use::Free && s.voices[n].bank == hd3) key_off(s, b, n, true);
        return lv2::CELL_OK;
    });
    if (r < 0) return r;
    for (Smf& m : s.smfs)
        if (m.bound && m.bank == hd3 && m.status != kSmfStopped) {
            m.status = kSmfStopped;
            smf_rewind(m);
        }
    s.banks[hd3] = Bank{};
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- notes

// int cellSnd3NoteOnByTone(u32 hd3ID, u32 toneIndex, u32 note, u32 keyOnID, CellSnd3KeyOnParam *keyOnParam)
s32 cellSnd3NoteOnByTone(PPUContext& ctx, u32 hd3, u32 tone, u32 note, u32 id, u32 param)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    KeyOn k;
    if (const s32 r = tone_key_on(ctx, s, hd3, tone, note, false, id, param, k); r < 0) return r;
    return transact(ctx, s, [&](Batch& b) {
        const s32 n = allocate(s, s.banks[hd3].tones[tone].priority);
        if (n >= 0) start_voice(s, b, static_cast<u32>(n), k);
        else PS_LOG_DEBUG(kChan, "cellSnd3NoteOnByTone: no voice for tone {}", tone);
        return lv2::CELL_OK;
    });
}

// int cellSnd3KeyOnByTone(u32 hd3ID, u32 toneIndex, u32 pitch, u32 keyOnID, CellSnd3KeyOnParam *keyOnParam): `pitch`
// relative to the tone's own (0x1000), as cellSnd3Note2Pitch gives it.
s32 cellSnd3KeyOnByTone(PPUContext& ctx, u32 hd3, u32 tone, u32 pitch, u32 id, u32 param)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    KeyOn k;
    if (const s32 r = tone_key_on(ctx, s, hd3, tone, pitch, true, id, param, k); r < 0) return r;
    return transact(ctx, s, [&](Batch& b) {
        const s32 n = allocate(s, s.banks[hd3].tones[tone].priority);
        if (n >= 0) start_voice(s, b, static_cast<u32>(n), k);
        else PS_LOG_DEBUG(kChan, "cellSnd3KeyOnByTone: no voice for tone {}", tone);
        return lv2::CELL_OK;
    });
}

// The Voice* key-ons play on a voice the game reserved (cellSnd3VoiceSetReserveMode), else RESERVEDVOICE (D-640).
s32 voice_key_on(PPUContext& ctx, u32 hd3, u32 voice, u32 tone, u32 note_or_pitch, bool by_pitch, u32 id, u32 param)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_voice(s, voice); r < 0) return r;
    KeyOn k;
    if (const s32 r = tone_key_on(ctx, s, hd3, tone, note_or_pitch, by_pitch, id, param, k); r < 0) return r;
    if (!s.voices[voice].reserved) return error(CELL_SND3_ERROR_RESERVEDVOICE);
    return transact(ctx, s, [&](Batch& b) {
        start_voice(s, b, voice, k);
        return lv2::CELL_OK;
    });
}

// int cellSnd3VoiceNoteOnByTone(u32 hd3ID, u32 voiceNum, u32 toneIndex, u32 note, u32 keyOnID, CellSnd3KeyOnParam *)
s32 cellSnd3VoiceNoteOnByTone(PPUContext& ctx, u32 hd3, u32 voice, u32 tone, u32 note, u32 id, u32 param)
{
    return voice_key_on(ctx, hd3, voice, tone, note, false, id, param);
}

// int cellSnd3VoiceKeyOnByTone(u32 hd3ID, u32 voiceNum, u32 toneIndex, u32 pitch, u32 keyOnID, CellSnd3KeyOnParam *)
s32 cellSnd3VoiceKeyOnByTone(PPUContext& ctx, u32 hd3, u32 voice, u32 tone, u32 pitch, u32 id, u32 param)
{
    return voice_key_on(ctx, hd3, voice, tone, pitch, true, id, param);
}

// int cellSnd3NoteOn(u32 hd3ID, u32 midiChannel, u32 midiProgram, u32 midiNote, u32 sustain,
// CellSnd3KeyOnParam *keyOnParam, u32 keyOnID): the program's splits for the note, each on a voice; `sustain` holds
// them on until cellSnd3SetSustainHold ends it.
s32 cellSnd3NoteOn(PPUContext& ctx, u32 hd3, u32 channel, u32 program, u32 note, u32 sustain, u32 param, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    const Bank* bank = bank_of(s, hd3);
    if (!bank) return error(CELL_SND3_ERROR_HD3ID);
    if (channel > 15 || note > 127 || program >= bank->programs.size() || !bank->programs[program].present)
        return error(CELL_SND3_ERROR_PARAM);
    KeyOn k;
    if (const s32 r = read_key_on_param(ctx, param, k); r < 0) return r;
    k.bank = hd3;
    k.note = static_cast<u8>(note);
    k.key_on_id = id;
    k.channel = static_cast<s32>(channel);
    k.hold = sustain != 0;
    return transact(ctx, s, [&](Batch& b) {
        program_note_on(s, b, k, s.banks[hd3].programs[program]);
        return lv2::CELL_OK;
    });
}

// int cellSnd3NoteOff(u32 midiChannel, u32 midiNote, u32 keyOnID): the voices of that channel, note and key-on ID
// (a held one when its hold ends).
s32 cellSnd3NoteOff(PPUContext& ctx, u32 channel, u32 note, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    if (channel > 15 || note > 127) return error(CELL_SND3_ERROR_PARAM);
    return transact(ctx, s, [&](Batch& b) {
        for (u32 n = 0; n < s.max_voice; n++) {
            const Voice& v = s.voices[n];
            if (v.channel == static_cast<s32>(channel) && v.note == note && v.key_on_id == id) key_off(s, b, n, false);
        }
        return lv2::CELL_OK;
    });
}

// u32 cellSnd3KeyOffByID(u32 keyOnID): every voice keyed on with that ID (a held one when its hold ends); how many were
// keyed off (D-640: 0 before Init or when the queue has no room, logged).
u32 cellSnd3KeyOffByID(PPUContext& ctx, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) {
        PS_LOG_WARN(kChan, "cellSnd3KeyOffByID before cellSnd3Init");
        return 0;
    }
    u32 count = 0;
    const s32 r = transact(ctx, s, [&](Batch& b) {
        for (u32 n = 0; n < s.max_voice; n++)
            if (s.voices[n].use != Use::Free && s.voices[n].key_on_id == id && key_off(s, b, n, false)) count++;
        return lv2::CELL_OK;
    });
    if (r < 0) {
        PS_LOG_WARN(kChan, "cellSnd3KeyOffByID({:#x}): the request queue is full", id);
        return 0;
    }
    return count;
}

// int cellSnd3SetSustainHold(u32 midiChannel, u32 sustainHold, u32 keyOnID): the hold of that channel's voices of that
// key-on ID; ending it keys off those a NoteOff came to.
s32 cellSnd3SetSustainHold(PPUContext& ctx, u32 channel, u32 hold, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    if (channel > 15) return error(CELL_SND3_ERROR_PARAM);
    return transact(ctx, s, [&](Batch& b) {
        for (u32 n = 0; n < s.max_voice; n++)
            if (s.voices[n].channel == static_cast<s32>(channel) && s.voices[n].key_on_id == id) set_hold(s, b, n, hold != 0);
        return lv2::CELL_OK;
    });
}

// The voices `match` takes, as CellSnd3VoiceBitCtx: u32 core[4], core c's word with bit v for its voice v (D-640).
template <class Match>
s32 voice_bits(PPUContext& ctx, const State& s, u32 out, Match&& match)
{
    if (!out || !guest_readable(thread_of(ctx), out, kVoiceBitSize)) return error(CELL_SND3_ERROR_PARAM);
    std::array<u32, 4> bits{};
    for (u32 n = 0; n < s.max_voice; n++)
        if (s.voices[n].use != Use::Free && match(s.voices[n])) bits[n / kVoicesPerCore] |= 1u << (n % kVoicesPerCore);
    for (u32 i = 0; i < 4; i++) ps_write32(gb(ctx), out + 4 * i, bits[i]);
    return lv2::CELL_OK;
}

// int cellSnd3GetVoice(u32 midiChannel, u32 keyOnID, CellSnd3VoiceBitCtx *voiceBit): the sounding voices of that
// channel and key-on ID.
s32 cellSnd3GetVoice(PPUContext& ctx, u32 channel, u32 id, u32 out)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    if (channel > 15) return error(CELL_SND3_ERROR_PARAM);
    return voice_bits(ctx, s, out, [&](const Voice& v) { return v.channel == static_cast<s32>(channel) && v.key_on_id == id; });
}

// int cellSnd3GetVoiceByID(u32 ID, CellSnd3VoiceBitCtx *voiceBit): the sounding voices of that key-on ID.
s32 cellSnd3GetVoiceByID(PPUContext& ctx, u32 id, u32 out)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    return voice_bits(ctx, s, out, [&](const Voice& v) { return v.key_on_id == id; });
}

// ------------------------------------------------------------------------------------------------- voices

// int cellSnd3VoiceSetReserveMode(u32 voiceNum, u32 reserveMode): 1 reserves the voice for the Voice* key-ons (the
// library's own key-ons leave it alone), 0 gives it back.
s32 cellSnd3VoiceSetReserveMode(PPUContext&, u32 voice, u32 mode)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_voice(s, voice); r < 0) return r;
    if (mode > 1) return error(CELL_SND3_ERROR_PARAM);
    s.voices[voice].reserved = mode != 0;
    return lv2::CELL_OK;
}

// int cellSnd3VoiceSetSustainHold(u32 voiceNum, u32 sustainHold)
s32 cellSnd3VoiceSetSustainHold(PPUContext& ctx, u32 voice, u32 hold)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_voice(s, voice); r < 0) return r;
    return transact(ctx, s, [&](Batch& b) {
        set_hold(s, b, voice, hold != 0);
        return lv2::CELL_OK;
    });
}

// int cellSnd3VoiceKeyOff(u32 voiceNum): a held voice when its hold ends.
s32 cellSnd3VoiceKeyOff(PPUContext& ctx, u32 voice)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_voice(s, voice); r < 0) return r;
    return transact(ctx, s, [&](Batch& b) {
        key_off(s, b, voice, false);
        return lv2::CELL_OK;
    });
}

// int cellSnd3VoiceAllKeyOff(void): every voice, held or not.
s32 cellSnd3VoiceAllKeyOff(PPUContext& ctx)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    return transact(ctx, s, [&](Batch& b) {
        for (u32 n = 0; n < s.max_voice; n++) key_off(s, b, n, true);
        return lv2::CELL_OK;
    });
}

// A sounding voice's parameter: `change` made, its pitch and volume told again (nothing to tell for a free voice).
template <class Change>
s32 voice_param(PPUContext& ctx, u32 voice, Change&& change)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_voice(s, voice); r < 0) return r;
    return transact(ctx, s, [&](Batch& b) {
        Voice& v = s.voices[voice];
        if (const s32 r = change(v); r < 0) return r;
        if (v.use != Use::Free) b.front.push_back(params_request(s, voice));
        return lv2::CELL_OK;
    });
}

// int cellSnd3VoiceSetPitch(u32 voiceNum, s32 addPitch): the pitch offset in 1/128 semitones.
s32 cellSnd3VoiceSetPitch(PPUContext& ctx, u32 voice, s32 add)
{
    return voice_param(ctx, voice, [&](Voice& v) {
        v.add_pitch = add;
        return lv2::CELL_OK;
    });
}

// int cellSnd3VoiceSetVelocity(u32 voiceNum, u32 velocity): 0-127.
s32 cellSnd3VoiceSetVelocity(PPUContext& ctx, u32 voice, u32 velocity)
{
    return voice_param(ctx, voice, [&](Voice& v) {
        if (velocity > 127) return error(CELL_SND3_ERROR_PARAM);
        v.velocity = static_cast<u8>(velocity);
        return lv2::CELL_OK;
    });
}

// int cellSnd3VoiceSetPanpot(u32 voiceNum, u32 panpot): 0 (left) to 127 (right), 64 in the middle.
s32 cellSnd3VoiceSetPanpot(PPUContext& ctx, u32 voice, u32 pan)
{
    return voice_param(ctx, voice, [&](Voice& v) {
        if (pan > 127) return error(CELL_SND3_ERROR_PARAM);
        v.pan = static_cast<u8>(pan);
        return lv2::CELL_OK;
    });
}

// int cellSnd3VoiceSetPanpotEx(u32 voiceNum, u32 panpotEx): front (0) to rear (127), kept (D-643: where libsynth2
// puts a voice front or rear is its own).
s32 cellSnd3VoiceSetPanpotEx(PPUContext& ctx, u32 voice, u32 pan)
{
    return voice_param(ctx, voice, [&](Voice& v) {
        if (pan > 127) return error(CELL_SND3_ERROR_PARAM);
        v.pan_ex = static_cast<u8>(pan);
        return lv2::CELL_OK;
    });
}

// int cellSnd3VoiceSetPitchBend(u32 voiceNum, u32 bendValue): MIDI's 14 bits, 0x2000 in the middle, over the split's
// bend range (two semitones without one).
s32 cellSnd3VoiceSetPitchBend(PPUContext& ctx, u32 voice, u32 bend)
{
    return voice_param(ctx, voice, [&](Voice& v) {
        if (bend > 0x3FFF) return error(CELL_SND3_ERROR_PARAM);
        v.bend = static_cast<u16>(bend);
        return lv2::CELL_OK;
    });
}

// int cellSnd3VoiceGetEnvelope(u32 voiceNum): its envelope (ENVX) as the last block left it.
s32 cellSnd3VoiceGetEnvelope(PPUContext&, u32 voice)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_voice(s, voice); r < 0) return r;
    return s.voices[voice].use == Use::Free ? 0 : s.voices[voice].envelope;
}

// int cellSnd3VoiceGetStatus(u32 voiceNum): 0 free, 1 keyed on, 2 keyed off and still sounding (D-640).
s32 cellSnd3VoiceGetStatus(PPUContext&, u32 voice)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_voice(s, voice); r < 0) return r;
    switch (s.voices[voice].use) {
    case Use::On: return kVoiceOn;
    case Use::Off: return kVoiceReleased;
    case Use::Free: break;
    }
    return kVoiceFree;
}

// ------------------------------------------------------------------------------------------------- SMF calls

// int cellSnd3SMFBind(CellSnd3SmfCtx *smfCtx, void *smf, u32 hd3ID): a Standard MIDI File (format 0 or 1) to play over
// a bank's programs; its smfID.
s32 cellSnd3SMFBind(PPUContext& ctx, u32 smf_ctx, u32 data, u32 hd3)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_init(s); r < 0) return r;
    lv2::PpuThread& t = thread_of(ctx);
    if (!smf_ctx || !guest_readable(t, smf_ctx, kSmfCtxSize)) return error(CELL_SND3_ERROR_SMFCTX);
    if (!data) return error(CELL_SND3_ERROR_PARAM);
    if (!bank_of(s, hd3)) return error(CELL_SND3_ERROR_HD3ID);
    u32 slot = kMaxSmf;
    for (u32 i = 0; i < kMaxSmf; i++)
        if (!s.smfs[i].bound) {
            slot = i;
            break;
        }
    if (slot == kMaxSmf) return error(CELL_SND3_ERROR_SMFFULL);
    Smf m;
    std::string why;
    if (!read_smf(t, gb(ctx), data, m.song, why)) {
        PS_LOG_ERROR(kChan, "cellSnd3SMFBind: the SMF at {:#x}: {}", data, why);
        return error(CELL_SND3_ERROR_SMF);
    }
    m.bound = true;
    m.ctx = smf_ctx;
    m.data = data;
    m.bank = hd3;
    smf_rewind(m);
    PS_LOG_INFO(kChan, "SMF {}: {} events, {} ticks a quarter note, {} ticks long, over bank {}", slot,
        m.song.events.size(), m.song.division, m.song.end, hd3);
    s.smfs[slot] = std::move(m);
    return static_cast<s32>(slot);
}

// The SMF calls' common start: initialised, and an SMF bound as `id`.
s32 check_smf(State& s, u32 id, Smf*& m)
{
    if (const s32 r = check_init(s); r < 0) return r;
    m = smf_of(s, id);
    return m ? lv2::CELL_OK : error(CELL_SND3_ERROR_SMFID);
}

// int cellSnd3SMFUnbind(u32 smfID): stopped, its voices keyed off.
s32 cellSnd3SMFUnbind(PPUContext& ctx, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    const s32 r = transact(ctx, s, [&](Batch& b) {
        smf_voices_off(s, b, id, -1, true);
        return lv2::CELL_OK;
    });
    if (r < 0) return r;
    for (Voice& v : s.voices)
        if (v.smf == static_cast<s32>(id)) v.smf = -1;
    *m = Smf{};
    return lv2::CELL_OK;
}

s32 smf_play(PPUContext& ctx, u32 id, u32 velocity, u32 pan, u32 pan_ex, u32 count)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    if (velocity > 127 || pan > 127 || pan_ex > 127) return error(CELL_SND3_ERROR_PARAM);
    if (!bank_of(s, m->bank)) return error(CELL_SND3_ERROR_HD3ID);
    const s32 r = transact(ctx, s, [&](Batch& b) {
        smf_voices_off(s, b, id, -1, true);
        return lv2::CELL_OK;
    });
    if (r < 0) return r;
    smf_rewind(*m);
    m->velocity = velocity;
    m->pan = pan;
    m->pan_ex = pan_ex;
    m->play_count = count;
    m->plays = 0;
    m->status = kSmfPlaying;
    return lv2::CELL_OK;
}

// int cellSnd3SMFPlay(u32 smfID, u32 playVelocity, u32 playPan, u32 playCount): from the start, its velocities scaled by
// playVelocity / 127, panned by playPan (64: as written), playCount times (0: for ever), from the next Synthesis.
s32 cellSnd3SMFPlay(PPUContext& ctx, u32 id, u32 velocity, u32 pan, u32 count)
{
    return smf_play(ctx, id, velocity, pan, 64, count);
}

// int cellSnd3SMFPlayEx(u32 smfID, u32 playVelocity, u32 playPan, u32 playPanEx, u32 playCount)
s32 cellSnd3SMFPlayEx(PPUContext& ctx, u32 id, u32 velocity, u32 pan, u32 pan_ex, u32 count)
{
    return smf_play(ctx, id, velocity, pan, pan_ex, count);
}

// int cellSnd3SMFPause(u32 smfID): where it is kept, its voices keyed off.
s32 cellSnd3SMFPause(PPUContext& ctx, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    if (m->status != kSmfPlaying) return lv2::CELL_OK;
    const s32 r = transact(ctx, s, [&](Batch& b) {
        smf_voices_off(s, b, id, -1, true);
        return lv2::CELL_OK;
    });
    if (r == lv2::CELL_OK) m->status = kSmfPaused;
    return r;
}

// int cellSnd3SMFResume(u32 smfID): a paused SMF plays on from where it was.
s32 cellSnd3SMFResume(PPUContext&, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    if (m->status == kSmfPaused) m->status = kSmfPlaying;
    return lv2::CELL_OK;
}

// int cellSnd3SMFStop(u32 smfID): stopped and back at its start, its voices keyed off.
s32 cellSnd3SMFStop(PPUContext& ctx, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    const s32 r = transact(ctx, s, [&](Batch& b) {
        smf_voices_off(s, b, id, -1, true);
        return lv2::CELL_OK;
    });
    if (r < 0) return r;
    m->status = kSmfStopped;
    smf_rewind(*m);
    return lv2::CELL_OK;
}

// int cellSnd3SMFAddTempo(u32 smfID, s32 addTempo): beats a minute added to the song's tempo (added up over calls; the
// tempo stays at least one beat a minute).
s32 cellSnd3SMFAddTempo(PPUContext&, u32 id, s32 add)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    const u128 before = effective_tempo(*m);
    m->add_tempo = static_cast<s32>(std::clamp<s64>(s64{m->add_tempo} + add, -0x1000000, 0x1000000));
    m->rem = m->rem * effective_tempo(*m) / before; // the same part of the tick at the new tempo
    return lv2::CELL_OK;
}

// int cellSnd3SMFGetTempo(u32 smfID): the tempo playing, beats a minute (rounded).
s32 cellSnd3SMFGetTempo(PPUContext&, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    return static_cast<s32>(std::round(60e6 / effective_tempo(*m)));
}

// The play velocity and pans: each sounding voice of the SMF takes a new one.
template <class Set>
s32 smf_set(PPUContext& ctx, u32 id, u32 value, Set&& set)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    if (value > 127) return error(CELL_SND3_ERROR_PARAM);
    const u32 old = set(*m, value);
    const s32 r = transact(ctx, s, [&](Batch& b) {
        smf_update(s, b, id, -1, [](Voice&) {});
        return lv2::CELL_OK;
    });
    if (r < 0) set(*m, old);
    return r;
}

template <class Get>
s32 smf_get(u32 id, Get&& get)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    return static_cast<s32>(get(*m));
}

// int cellSnd3SMFSetPlayVelocity(u32 smfID, u32 playVelocity), int cellSnd3SMFGetPlayVelocity(u32 smfID)
s32 cellSnd3SMFSetPlayVelocity(PPUContext& ctx, u32 id, u32 velocity)
{
    return smf_set(ctx, id, velocity, [](Smf& m, u32 v) { return std::exchange(m.velocity, v); });
}
s32 cellSnd3SMFGetPlayVelocity(PPUContext&, u32 id) { return smf_get(id, [](const Smf& m) { return m.velocity; }); }

// int cellSnd3SMFSetPlayPanpot(u32 smfID, u32 playPanpot), int cellSnd3SMFGetPlayPanpot(u32 smfID)
s32 cellSnd3SMFSetPlayPanpot(PPUContext& ctx, u32 id, u32 pan)
{
    return smf_set(ctx, id, pan, [](Smf& m, u32 v) { return std::exchange(m.pan, v); });
}
s32 cellSnd3SMFGetPlayPanpot(PPUContext&, u32 id) { return smf_get(id, [](const Smf& m) { return m.pan; }); }

// int cellSnd3SMFSetPlayPanpotEx(u32 smfID, u32 playPanpotEx), int cellSnd3SMFGetPlayPanpotEx(u32 smfID): kept, as a
// voice's front-rear pan (D-643).
s32 cellSnd3SMFSetPlayPanpotEx(PPUContext& ctx, u32 id, u32 pan)
{
    return smf_set(ctx, id, pan, [](Smf& m, u32 v) { return std::exchange(m.pan_ex, v); });
}
s32 cellSnd3SMFGetPlayPanpotEx(PPUContext&, u32 id) { return smf_get(id, [](const Smf& m) { return m.pan_ex; }); }

// int cellSnd3SMFGetPlayStatus(u32 smfID): 0 stopped (or played to its end), 1 playing, 2 paused (D-642).
s32 cellSnd3SMFGetPlayStatus(PPUContext&, u32 id) { return smf_get(id, [](const Smf& m) { return m.status; }); }

// int cellSnd3SMFSetPlayChannel(u32 smfID, u32 playChannelBit): the MIDI channels that play (bit n for channel n); the
// voices of a channel taken out are keyed off.
s32 cellSnd3SMFSetPlayChannel(PPUContext& ctx, u32 id, u32 bits)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    if (bits > 0xFFFF) return error(CELL_SND3_ERROR_PARAM);
    const s32 r = transact(ctx, s, [&](Batch& b) {
        for (u32 ch = 0; ch < 16; ch++)
            if (!(bits & (1u << ch))) smf_voices_off(s, b, id, static_cast<s32>(ch), true);
        return lv2::CELL_OK;
    });
    if (r == lv2::CELL_OK) m->mask = bits;
    return r;
}

// int cellSnd3SMFGetPlayChannel(u32 smfID, u32 *playChannelBit)
s32 cellSnd3SMFGetPlayChannel(PPUContext& ctx, u32 id, u32 out)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    if (!out || !guest_readable(thread_of(ctx), out, 4)) return error(CELL_SND3_ERROR_PARAM);
    ps_write32(gb(ctx), out, m->mask);
    return lv2::CELL_OK;
}

// int cellSnd3SMFGetKeyOnID(u32 smfID, u32 midiChannel, u32 *keyOnID): the key-on ID the SMF's notes of that channel
// play with, for cellSnd3GetVoice and the others (D-642).
s32 cellSnd3SMFGetKeyOnID(PPUContext& ctx, u32 id, u32 channel, u32 out)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    Smf* m = nullptr;
    if (const s32 r = check_smf(s, id, m); r < 0) return r;
    if (channel > 15 || !out || !guest_readable(thread_of(ctx), out, 4)) return error(CELL_SND3_ERROR_PARAM);
    ps_write32(gb(ctx), out, smf_key_on_id(id, channel));
    return lv2::CELL_OK;
}

// What libsnd3 takes from libsynth2: the firmware module's exports where the system software has one (D-197), so that a
// process has one synthesizer whichever serves it.
constexpr HostImport kImports[] = {
    {"libsnd3", "libsynth2", 0x03D1843C, false}, // cellSoundSynth2Init
    {"libsnd3", "libsynth2", 0x699C4E5E, false}, // cellSoundSynth2Exit
    {"libsnd3", "libsynth2", 0x6F87A42F, false}, // cellSoundSynth2SetParam
    {"libsnd3", "libsynth2", 0xD374FA07, false}, // cellSoundSynth2GetParam
    {"libsnd3", "libsynth2", 0x363883A6, false}, // cellSoundSynth2SetSwitch
    {"libsnd3", "libsynth2", 0x49697FBC, false}, // cellSoundSynth2GetSwitch
    {"libsnd3", "libsynth2", 0x2165E05A, false}, // cellSoundSynth2SetAddr
    {"libsnd3", "libsynth2", 0x7A218453, false}, // cellSoundSynth2SetEffectAttr
    {"libsnd3", "libsynth2", 0xF01E5FD4, false}, // cellSoundSynth2SetCoreAttr
    {"libsnd3", "libsynth2", 0x57F0D642, false}, // cellSoundSynth2Generate
};

} // namespace

void register_libsnd3(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x60BB76C2, cellSnd3Init),
        PS_HLE_FUNC(0xEA8B6003, cellSnd3Exit),
        PS_HLE_FUNC(0x1D7F54EB, cellSnd3Note2Pitch),
        PS_HLE_FUNC(0x86D53E9E, cellSnd3Pitch2Note),
        PS_HLE_FUNC(0x435FC9F7, cellSnd3SetOutputMode),
        PS_HLE_FUNC(0xB4C4624D, cellSnd3Synthesis),
        PS_HLE_FUNC(0x5D25716B, cellSnd3SynthesisEx),
        PS_HLE_FUNC(0x6F9B8E91, cellSnd3BindSoundData),
        PS_HLE_FUNC(0xCF115FCC, cellSnd3UnbindSoundData),
        PS_HLE_FUNC(0xFEF776EE, cellSnd3NoteOnByTone),
        PS_HLE_FUNC(0x8A092450, cellSnd3KeyOnByTone),
        PS_HLE_FUNC(0x6961E09C, cellSnd3VoiceNoteOnByTone),
        PS_HLE_FUNC(0xBEDD8EBF, cellSnd3VoiceKeyOnByTone),
        PS_HLE_FUNC(0x42C78EF2, cellSnd3VoiceSetReserveMode),
        PS_HLE_FUNC(0xA7D7329C, cellSnd3VoiceSetSustainHold),
        PS_HLE_FUNC(0xC97DC2A7, cellSnd3VoiceKeyOff),
        PS_HLE_FUNC(0x2A5CA085, cellSnd3VoiceSetPitch),
        PS_HLE_FUNC(0x269386D3, cellSnd3VoiceSetVelocity),
        PS_HLE_FUNC(0x0C8EC006, cellSnd3VoiceSetPanpot),
        PS_HLE_FUNC(0x496258DF, cellSnd3VoiceSetPanpotEx),
        PS_HLE_FUNC(0x665808A3, cellSnd3VoiceSetPitchBend),
        PS_HLE_FUNC(0x20DEF6C6, cellSnd3VoiceAllKeyOff),
        PS_HLE_FUNC(0x711FB8A8, cellSnd3VoiceGetEnvelope),
        PS_HLE_FUNC(0x0AADF8D3, cellSnd3VoiceGetStatus),
        PS_HLE_FUNC(0x58B02458, cellSnd3KeyOffByID),
        PS_HLE_FUNC(0xD9F73B7B, cellSnd3GetVoice),
        PS_HLE_FUNC(0x8AD5666C, cellSnd3GetVoiceByID),
        PS_HLE_FUNC(0x1B52FCD5, cellSnd3NoteOn),
        PS_HLE_FUNC(0xBEDAB73A, cellSnd3NoteOff),
        PS_HLE_FUNC(0xDA423963, cellSnd3SetSustainHold),
        PS_HLE_FUNC(0x80F85E20, cellSnd3SetEffectType),
        PS_HLE_FUNC(0x084B6EFD, cellSnd3SMFBind),
        PS_HLE_FUNC(0x913D5FD3, cellSnd3SMFUnbind),
        PS_HLE_FUNC(0x8889E6B9, cellSnd3SMFPlay),
        PS_HLE_FUNC(0x57BD4B22, cellSnd3SMFPlayEx),
        PS_HLE_FUNC(0xAFDDE4B2, cellSnd3SMFPause),
        PS_HLE_FUNC(0x54CA1E88, cellSnd3SMFResume),
        PS_HLE_FUNC(0x98D8E375, cellSnd3SMFStop),
        PS_HLE_FUNC(0x2D4C1C98, cellSnd3SMFAddTempo),
        PS_HLE_FUNC(0x2719D7D6, cellSnd3SMFGetTempo),
        PS_HLE_FUNC(0x7F482AA0, cellSnd3SMFSetPlayVelocity),
        PS_HLE_FUNC(0x3D880FA3, cellSnd3SMFGetPlayVelocity),
        PS_HLE_FUNC(0x48DC7245, cellSnd3SMFSetPlayPanpot),
        PS_HLE_FUNC(0x198A0710, cellSnd3SMFSetPlayPanpotEx),
        PS_HLE_FUNC(0xDBC76E6D, cellSnd3SMFGetPlayPanpot),
        PS_HLE_FUNC(0x2D5B4DCC, cellSnd3SMFGetPlayPanpotEx),
        PS_HLE_FUNC(0x50D3ABD4, cellSnd3SMFGetPlayStatus),
        PS_HLE_FUNC(0xFA214D8A, cellSnd3SMFSetPlayChannel),
        PS_HLE_FUNC(0x58A47A3D, cellSnd3SMFGetPlayChannel),
        PS_HLE_FUNC(0x44BAEB86, cellSnd3SMFGetKeyOnID),
    });
    r.add_imports(kModule, kImports);
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
