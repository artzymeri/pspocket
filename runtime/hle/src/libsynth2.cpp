// libsynth2: the PlayStation 2's sound processor, the SPU2, as a PS3 library, with the PS2's libsd interface under cell
// names (D-610 to D-614): cellSoundSynth2Init is sceSdInit, SetParam/GetParam, SetSwitch/GetSwitch, SetAddr/GetAddr
// and SetCoreAttr are libsd's register calls with libsd's register numbers, SetEffectAttr its reverb presets,
// VoiceTrans and VoiceTransStatus its transfers into the 2 MB sound memory, Note2Pitch and Pitch2Note its pitch
// arithmetic; Generate renders the next samples as floats for the game to give cellAudio. The synthesizer is
// spu2.hpp's. No Sony module was at hand to compare with, so the library is firmware-first (D-563): where the system
// software is installed a game gets Sony's libsynth2, this one where it is not.
//
// References: RPCS3 rpcs3/Emu/Cell/Modules/libsynth2.cpp and libsynth2.h (the signatures, the error codes,
// CellSoundSynth2EffectAttr; its bodies are stubs); ps2sdk's libsd-common.h (the register, entry, transfer and effect
// mode numbers, SD_VOICE: the core in bit 0, the voice in bits 1-5, the parameter in bits 8-15) and freesd's
// libsd (freesd.c, effect.c, voice.c: what each call does, the initialisation, the reverb presets and work area
// sizes, the echo and delay parameters, the pitch tables and arithmetic), read for those facts, not copied.
// Where libsd and the PS3 library may differ, the choice made and what to compare with Sony's module on the Mac are in
// D-610 to D-614.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "spu2.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "libsynth2";
constexpr const char* kChan = "libsynth2";

enum : u32 {
    CELL_SOUND_SYNTH2_ERROR_FATAL = 0x80310201,
    CELL_SOUND_SYNTH2_ERROR_INVALID_PARAMETER = 0x80310202,
    CELL_SOUND_SYNTH2_ERROR_ALREADY_INITIALIZED = 0x80310203,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// libsd's parameter numbers (the register's bits 8-15; libsd-common.h SD_VPARAM_*, SD_PARAM_*, SD_SWITCH_*,
// SD_ADDR_*, SD_VADDR_*).
enum : u32 {
    kVoll = 0x00, kVolr, kPitch, kAdsr1, kAdsr2, kEnvx, kVolxl, kVolxr,                          // a voice's
    kMmix = 0x08, kMvoll, kMvolr, kEvoll, kEvolr, kAvoll, kAvolr, kBvoll, kBvolr, kMvolxl, kMvolxr, // a core's
    kPmon = 0x13, kNon, kKon, kKoff, kEndx, kVmixl, kVmixel, kVmixr, kVmixer,                     // switches
    kEsa = 0x1C, kEea, kTsa, kIrqa,                                                             // a core's addresses
    kSsa = 0x20, kLsax, kNax,                                                                   // a voice's
};

// SetCoreAttr's entries (libsd-common.h SD_CORE_*; the core in bit 0).
enum : u32 {
    kCoreEffectEnable = 0x2,
    kCoreIrqEnable = 0x4,
    kCoreMuteEnable = 0x6,
    kCoreNoiseClock = 0x8,
    kCoreSpdifMode = 0xA,
};

// VoiceTrans's modes (SD_TRANS_*): the direction in bits 0-1, bit 3 I/O rather than DMA (the same here).
constexpr u32 kTransWrite = 0, kTransRead = 1;

// The effect modes (SD_EFFECT_MODE_*): 0 off, room, three studios, hall, space, echo, delay, pipe; CLEAR also
// zeroes the work area.
constexpr u32 kModes = 10, kModeEcho = 7, kModeDelay = 8, kModeClear = 0x100;

// The reverb presets of the ten modes in spu2::ReverbReg's order (libsd's, freesd effect.c EffectParams; psx-spx
// lists the PS1's same studio, hall, space and half echo presets, and room, echo and delay a little different).
constexpr u16 kPresets[kModes][spu2::kReverbRegs] = {
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000},
    {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227,
     0x0374, 0x01EF, 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136,
     0x00B8, 0x005C, 0x8000, 0x8000},
    {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF,
     0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080,
     0x004C, 0x0026, 0x8000, 0x8000},
    {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F,
     0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2,
     0x0100, 0x0080, 0x8000, 0x8000},
    {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C,
     0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238,
     0x0154, 0x00AA, 0x8000, 0x8000},
    {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD,
     0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A,
     0x0274, 0x013A, 0x8000, 0x8000},
    {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B,
     0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2,
     0x0464, 0x0232, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009,
     0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004,
     0x0008, 0x0004, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009,
     0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004,
     0x0008, 0x0004, 0x8000, 0x8000},
    {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF,
     0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040,
     0x0028, 0x0014, 0x8000, 0x8000},
};

// Each mode's work area, ending at the core's EEA (libsd's EffectSizes times 16 bytes; psx-spx's sizes).
constexpr u32 kEffectBytes[kModes] = {0x10, 0x26C0, 0x1F40, 0x4840, 0x6FE0, 0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00};

// After Init every voice points at libsd's 32 bytes at 0x5000: a looping block (flags 7) and a block of zeros.
constexpr u32 kInitBlock = 0x5000;

// CellSoundSynth2EffectAttr: core, mode, depth_L, depth_R, delay, feedback (big-endian 16-bit fields).
constexpr u32 kEffectAttrSize = 12;

// Generate renders through a host buffer this many samples at a time.
constexpr u32 kChunk = 256;

struct Synth2 {
    std::mutex mutex;
    bool initialized = false;
    spu2::Spu2 spu;
    std::array<u32, spu2::kCores> effect_mode{}; // each core's mode as SetEffectAttr/SetEffectMode last set it
    std::map<s32, s32> config;                   // cellSoundSynth2Config's settings
    std::set<std::string> logged;                // what was logged once
};

Synth2& synth() { return HleProcess::current()->state<Synth2>(); }

// A refused or ignored call logged once per process (a game may make it every frame); under the state's mutex.
void warn_once(Synth2& s, std::string message)
{
    if (s.logged.insert(message).second) PS_LOG_WARN(kChan, "{}", message);
}

// libsd takes a core, channel or bus from bit 0 (core &= 1); a value past 1 is noted once.
u32 core_of(Synth2& s, std::string_view fn, s32 value)
{
    if (value != 0 && value != 1)
        warn_once(s, std::format("{}: core {} taken as {} (bit 0, as libsd)", fn, value, value & 1));
    return static_cast<u32>(value) & 1;
}

// Whether [addr, addr + size) is committed guest memory, page by page.
bool guest_range(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!size) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); a < u64{addr} + size; a += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return false;
    return true;
}

s32 invalid(PPUContext& ctx, std::string_view why)
{
    log_call_site(ctx, guest_base(ctx), std::format("{}; CELL_SOUND_SYNTH2_ERROR_INVALID_PARAMETER", why));
    return error(CELL_SOUND_SYNTH2_ERROR_INVALID_PARAMETER);
}

s32 floor_div(s32 a, s32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

void set_bit(u16& word, u16 bit, bool on) { word = static_cast<u16>(on ? word | bit : word & ~bit); }

// libsd's pitch tables: 0x8000 * 2^(n/12) for the semitones of an octave and 0x8000 * 2^(n/1536) for the 128 fine steps
// of a semitone, rounded down (freesd.c NotePitchTable holds exactly these).
struct PitchTables {
    std::array<u32, 12> semitone{};
    std::array<u32, 128> fine{};
};

const PitchTables& pitch_tables()
{
    static const PitchTables tables = [] {
        PitchTables t;
        for (u32 i = 0; i < 12; i++) t.semitone[i] = static_cast<u32>(std::floor(32768.0 * std::exp2(i / 12.0)));
        for (u32 i = 0; i < 128; i++) t.fine[i] = static_cast<u32>(std::floor(32768.0 * std::exp2(i / 1536.0)));
        return t;
    }();
    return tables;
}

// sceSdInit's work (freesd.c sceSdInit: Reset, InitVoices, InitCoreVolume): a cold start (flag 0) also keys every
// voice off, zeroes the master and effect volumes, the pitch modulation and noise bits, and puts each core's effect
// area at the top of its half of memory (EEA 0x1DFFFF and 0x1FFFFF) with the "off" preset; a hot start (flag 1) keeps
// those and leaves the cores' effects on. Either way every voice gets volume 0, pitch 0x3FFF, no envelope and libsd's
// looping block at 0x5000, keyed on and off, ENDX clear; the voices go to every mix, MMIX is 0xFF0 and 0xFFC (core 1
// takes core 0's output dry, AVOL 0x7FFF), the memory input volumes 0.
void libsd_init(Synth2& s, bool hot)
{
    spu2::Spu2& spu = s.spu;
    for (u32 n = 0; n < spu2::kCores; n++) {
        spu2::Core& c = spu.cores[n];
        if (!hot) {
            spu.key_off(n, spu2::kVoiceMask);
            c.mvoll = c.mvolr = 0;
            c.mvolxl = c.mvolxr = 0;
            c.pmon = c.non = 0;
            c.eea = n == 0 ? 0x1DFFFF : 0x1FFFFF;
            spu.set_esa(n, c.eea + 1 - kEffectBytes[0]);
            std::copy(std::begin(kPresets[0]), std::end(kPresets[0]), c.reverb.begin());
        }
        s.effect_mode[n] = 0;
    }
    for (u32 i = 0; i < 16; i++) spu.memory[kInitBlock + i] = 0x07;
    for (u32 i = 16; i < 32; i++) spu.memory[kInitBlock + i] = 0x00;
    for (u32 n = 0; n < spu2::kCores; n++) {
        spu2::Core& c = spu.cores[n];
        for (spu2::Voice& v : c.voices) {
            v.voll = v.volr = 0;
            v.pitch = 0x3FFF;
            v.adsr1 = v.adsr2 = 0;
            v.ssa = kInitBlock;
        }
        spu.key_on(n, spu2::kVoiceMask);
        spu.key_off(n, spu2::kVoiceMask);
        c.endx = 0;
        c.attr = static_cast<u16>(spu2::kAttrOn | spu2::kAttrMute | (hot ? spu2::kAttrEffect : 0) |
            (n == 1 ? spu2::kAttrExternalInput : 0));
        c.vmixl = c.vmixr = c.vmixel = c.vmixer = spu2::kVoiceMask;
        c.mmix = n == 0 ? 0xFF0 : 0xFFC;
        if (!hot) c.evoll = c.evolr = 0;
        c.avoll = c.avolr = n == 1 ? 0x7FFF : 0;
        c.bvoll = c.bvolr = 0;
    }
}

// int cellSoundSynth2Config(short param, int value): settings of the library itself, which libsd has no counterpart of
// and pspocket's synthesizer has none to change: each is kept and noted once (D-613).
s32 cellSoundSynth2Config(PPUContext&, s16 param, s32 value)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    s.config[param] = value;
    warn_once(s, std::format("cellSoundSynth2Config(param {}, value {:#x}): kept; pspocket's synthesizer has no "
                             "setting by that number (D-613)", param, static_cast<u32>(value)));
    return lv2::CELL_OK;
}

// int cellSoundSynth2Init(short flag): libsd's sceSdInit, flag bit 0 a hot start. Once only until Exit.
s32 cellSoundSynth2Init(PPUContext&, s16 flag)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return error(CELL_SOUND_SYNTH2_ERROR_ALREADY_INITIALIZED);
    if (flag & ~1) warn_once(s, std::format("cellSoundSynth2Init: flag {:#x}, bit 0 taken (as libsd)", flag));
    libsd_init(s, (flag & 1) != 0);
    s.initialized = true;
    PS_LOG_INFO(kChan, "cellSoundSynth2Init: a {} start", (flag & 1) ? "hot" : "cold");
    return lv2::CELL_OK;
}

// int cellSoundSynth2Exit(void): the library ends; the synthesizer keeps its registers and memory, as libsd's
// sceSdQuit leaves the SPU2's, for a later hot Init.
s32 cellSoundSynth2Exit(PPUContext&)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    s.initialized = false;
    return lv2::CELL_OK;
}

// void cellSoundSynth2SetParam(u16 register, u16 value): a voice's volumes, pitch, ADSR and current levels, or a
// core's MMIX and volumes. An unknown register changes nothing (noted once).
void cellSoundSynth2SetParam(PPUContext&, u16 entry, u16 value)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    const u32 param = entry >> 8, voice = (entry >> 1) & 0x1F;
    spu2::Core& c = s.spu.cores[entry & 1];
    if (param <= kVolxr) {
        if (voice >= spu2::kVoices) {
            warn_once(s,
                std::format("cellSoundSynth2SetParam({:#06x}): voice {} does not exist; ignored", entry, voice));
            return;
        }
        spu2::Voice& v = c.voices[voice];
        switch (param) {
        case kVoll: v.voll = value; break;
        case kVolr: v.volr = value; break;
        case kPitch: v.pitch = value; break;
        case kAdsr1: v.adsr1 = value; break;
        case kAdsr2: v.adsr2 = value; break;
        case kEnvx: v.envx = static_cast<s16>(value); break;
        case kVolxl: v.volxl = static_cast<s16>(value); break;
        case kVolxr: v.volxr = static_cast<s16>(value); break;
        }
        return;
    }
    switch (param) {
    case kMmix: c.mmix = value; return;
    case kMvoll: c.mvoll = value; return;
    case kMvolr: c.mvolr = value; return;
    case kEvoll: c.evoll = static_cast<s16>(value); return;
    case kEvolr: c.evolr = static_cast<s16>(value); return;
    case kAvoll: c.avoll = static_cast<s16>(value); return;
    case kAvolr: c.avolr = static_cast<s16>(value); return;
    case kBvoll: c.bvoll = static_cast<s16>(value); return;
    case kBvolr: c.bvolr = static_cast<s16>(value); return;
    case kMvolxl: c.mvolxl = static_cast<s16>(value); return;
    case kMvolxr: c.mvolxr = static_cast<s16>(value); return;
    default:
        warn_once(s, std::format("cellSoundSynth2SetParam({:#06x}): no parameter {:#x}; ignored", entry, param));
    }
}

// u16 cellSoundSynth2GetParam(u16 register): what SetParam sets; ENVX, VOLXL/VOLXR and MVOLXL/MVOLXR are the current
// envelope and volume levels. An unknown register reads 0 (noted once).
u32 cellSoundSynth2GetParam(PPUContext&, u16 entry)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    const u32 param = entry >> 8, voice = (entry >> 1) & 0x1F;
    const spu2::Core& c = s.spu.cores[entry & 1];
    if (param <= kVolxr) {
        if (voice >= spu2::kVoices) {
            warn_once(s, std::format("cellSoundSynth2GetParam({:#06x}): voice {} does not exist; 0", entry, voice));
            return 0;
        }
        const spu2::Voice& v = c.voices[voice];
        switch (param) {
        case kVoll: return v.voll;
        case kVolr: return v.volr;
        case kPitch: return v.pitch;
        case kAdsr1: return v.adsr1;
        case kAdsr2: return v.adsr2;
        case kEnvx: return static_cast<u16>(v.envx);
        case kVolxl: return static_cast<u16>(v.volxl);
        default: return static_cast<u16>(v.volxr);
        }
    }
    switch (param) {
    case kMmix: return c.mmix;
    case kMvoll: return c.mvoll;
    case kMvolr: return c.mvolr;
    case kEvoll: return static_cast<u16>(c.evoll);
    case kEvolr: return static_cast<u16>(c.evolr);
    case kAvoll: return static_cast<u16>(c.avoll);
    case kAvolr: return static_cast<u16>(c.avolr);
    case kBvoll: return static_cast<u16>(c.bvoll);
    case kBvolr: return static_cast<u16>(c.bvolr);
    case kMvolxl: return static_cast<u16>(c.mvolxl);
    case kMvolxr: return static_cast<u16>(c.mvolxr);
    default:
        warn_once(s, std::format("cellSoundSynth2GetParam({:#06x}): no parameter {:#x}; 0", entry, param));
        return 0;
    }
}

// void cellSoundSynth2SetSwitch(u16 register, u32 value): a core's 24 voice bits (bit n voice n): pitch modulation,
// noise, key on, key off, the dry and wet mixes; a write to ENDX clears it, whatever the value (the SPU2 clears the
// half of ENDX written, PCSX2's spu2sys.cpp finds, and libsd writes both). An unknown register changes nothing (noted
// once).
void cellSoundSynth2SetSwitch(PPUContext&, u16 entry, u32 value)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    const u32 param = entry >> 8, core = entry & 1;
    spu2::Core& c = s.spu.cores[core];
    value &= spu2::kVoiceMask;
    switch (param) {
    case kPmon: c.pmon = value; return;
    case kNon: c.non = value; return;
    case kKon: s.spu.key_on(core, value); return;
    case kKoff: s.spu.key_off(core, value); return;
    case kEndx: c.endx = 0; return;
    case kVmixl: c.vmixl = value; return;
    case kVmixel: c.vmixel = value; return;
    case kVmixr: c.vmixr = value; return;
    case kVmixer: c.vmixer = value; return;
    default:
        warn_once(s, std::format("cellSoundSynth2SetSwitch({:#06x}): no switch {:#x}; ignored", entry, param));
    }
}

// u32 cellSoundSynth2GetSwitch(u16 register): the bits SetSwitch sets; KON and KOFF read what was last written
// (psx-spx), ENDX each voice that reached a loop end since it was keyed on. An unknown register reads 0.
u32 cellSoundSynth2GetSwitch(PPUContext&, u16 entry)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    const u32 param = entry >> 8;
    const spu2::Core& c = s.spu.cores[entry & 1];
    switch (param) {
    case kPmon: return c.pmon;
    case kNon: return c.non;
    case kKon: return c.kon;
    case kKoff: return c.koff;
    case kEndx: return c.endx;
    case kVmixl: return c.vmixl;
    case kVmixel: return c.vmixel;
    case kVmixr: return c.vmixr;
    case kVmixer: return c.vmixer;
    default:
        warn_once(s, std::format("cellSoundSynth2GetSwitch({:#06x}): no switch {:#x}; 0", entry, param));
        return 0;
    }
}

// int cellSoundSynth2SetAddr(u16 register, u32 value): a core's effect area start and end, transfer start and IRQ
// address, or a voice's start, loop and current addresses, as byte addresses in sound memory. As libsd stores them,
// each is a 16-byte block's address (the low four bits dropped) and EEA keeps only its 128 KB boundary (it ends there,
// its low 17 bits all set). Past sound memory or an unknown register: INVALID_PARAMETER, nothing set.
s32 cellSoundSynth2SetAddr(PPUContext& ctx, u16 entry, u32 value)
{
    const u32 param = entry >> 8, core = entry & 1, voice = (entry >> 1) & 0x1F;
    if (param < kEsa || param > kNax || (param >= kSsa && voice >= spu2::kVoices))
        return invalid(ctx, std::format("cellSoundSynth2SetAddr({:#06x}): no such address register", entry));
    if (value >= spu2::kMemorySize)
        return invalid(ctx, std::format("cellSoundSynth2SetAddr({:#06x}, {:#x}): past the 2 MB of sound memory", entry,
            value));
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    spu2::Core& c = s.spu.cores[core];
    const u32 block = value & ~(spu2::kBlockBytes - 1);
    switch (param) {
    case kEsa: s.spu.set_esa(core, block); break;
    case kEea: c.eea = (value & ~0x1FFFFu) | 0x1FFFF; break;
    case kTsa: c.tsa = block; break;
    case kIrqa: c.irqa = block; break;
    case kSsa: c.voices[voice].ssa = block; break;
    case kLsax: s.spu.set_lsax(core, voice, block); break;
    default: s.spu.set_nax(core, voice, block); break;
    }
    return lv2::CELL_OK;
}

// u32 cellSoundSynth2GetAddr(u16 register): what SetAddr sets; LSAX as SetAddr or the voice's loop start flags last
// set it, NAX the block the voice plays. An unknown register reads 0 (noted once).
u32 cellSoundSynth2GetAddr(PPUContext&, u16 entry)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    const u32 param = entry >> 8, voice = (entry >> 1) & 0x1F;
    const spu2::Core& c = s.spu.cores[entry & 1];
    if (param < kEsa || param > kNax || (param >= kSsa && voice >= spu2::kVoices)) {
        warn_once(s, std::format("cellSoundSynth2GetAddr({:#06x}): no such address register; 0", entry));
        return 0;
    }
    switch (param) {
    case kEsa: return c.esa;
    case kEea: return c.eea;
    case kTsa: return c.tsa;
    case kIrqa: return c.irqa;
    case kSsa: return c.voices[voice].ssa;
    case kLsax: return c.voices[voice].lsax;
    default: return c.voices[voice].nax;
    }
}

// SetEffectAttr and SetEffectMode (libsd effect.c sceSdSetEffectAttr): the mode's preset into the core's reverb, its
// work area the mode's size ending at EEA (the reverb's address back at its start); echo and delay take the delay
// ((delay + 1) * 32 words further for the same side reflection, the first comb and all-pass filters) and the
// feedback (feedback * 258 as the reflection volume vWALL). CLEAR zeroes the new area, and the old mode's when the
// effect was on. SetEffectAttr also sets the effect volume (EVOL) to the depths; SetEffectMode leaves it (D-612).
s32 set_effect(PPUContext& ctx, std::string_view fn, s16 bus, u32 attr, bool depth)
{
    if (!attr || !guest_readable(thread_of(ctx), attr, kEffectAttrSize))
        return invalid(ctx, std::format("{}: the attributes at {:#x} are not guest memory", fn, attr));
    u8* base = guest_base(ctx);
    const u32 raw_mode = ps_read16(base, attr + 2);
    const s16 depth_l = static_cast<s16>(ps_read16(base, attr + 4));
    const s16 depth_r = static_cast<s16>(ps_read16(base, attr + 6));
    const u16 delay = ps_read16(base, attr + 8), feedback = ps_read16(base, attr + 10);
    const bool clear = (raw_mode & kModeClear) != 0;
    const u32 mode = raw_mode & ~kModeClear;
    if (mode >= kModes) return invalid(ctx, std::format("{}: no effect mode {:#x}", fn, raw_mode));

    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    const u32 core = core_of(s, fn, bus);
    spu2::Core& c = s.spu.cores[core];
    std::array<u16, spu2::kReverbRegs> p{};
    std::copy(std::begin(kPresets[mode]), std::end(kPresets[mode]), p.begin());
    if (mode == kModeEcho || mode == kModeDelay) {
        const u16 d = static_cast<u16>((delay + 1) << 5);
        p[spu2::vWALL] = static_cast<u16>(((feedback << 7) + feedback) << 1);
        p[spu2::mLSAME] = static_cast<u16>((d << 1) - p[spu2::dAPF1]);
        p[spu2::mRSAME] = static_cast<u16>(d - p[spu2::dAPF2]);
        p[spu2::mLCOMB1] = static_cast<u16>(p[spu2::mRCOMB1] + d);
        p[spu2::dLSAME] = static_cast<u16>(p[spu2::dRSAME] + d);
        p[spu2::mLAPF1] = static_cast<u16>(p[spu2::mLAPF2] + d);
        p[spu2::mRAPF1] = static_cast<u16>(p[spu2::mRAPF2] + d);
    }
    const u32 end = c.eea + 1;
    if (clear && (c.attr & spu2::kAttrEffect)) {
        const u32 old = kEffectBytes[s.effect_mode[core]];
        std::memset(s.spu.memory.data() + (end - old), 0, old);
    }
    if (depth) {
        c.evoll = depth_l;
        c.evolr = depth_r;
    }
    c.reverb = p;
    s.spu.set_esa(core, end - kEffectBytes[mode]);
    if (clear) std::memset(s.spu.memory.data() + (end - kEffectBytes[mode]), 0, kEffectBytes[mode]);
    s.effect_mode[core] = mode;
    return lv2::CELL_OK;
}

// int cellSoundSynth2SetEffectAttr(short bus, CellSoundSynth2EffectAttr *attr)
s32 cellSoundSynth2SetEffectAttr(PPUContext& ctx, s16 bus, u32 attr)
{
    return set_effect(ctx, "cellSoundSynth2SetEffectAttr", bus, attr, true);
}

// int cellSoundSynth2SetEffectMode(short bus, CellSoundSynth2EffectAttr *attr): the mode without the depths (D-612).
s32 cellSoundSynth2SetEffectMode(PPUContext& ctx, s16 bus, u32 attr)
{
    return set_effect(ctx, "cellSoundSynth2SetEffectMode", bus, attr, false);
}

// void cellSoundSynth2SetCoreAttr(u16 entry, u16 value): a core's effect enable (ATTR bit 7), IRQ enable (bit 6;
// clearing it also takes back a raised IRQ), mute bit (bit 14: kept, D-612), noise clock (bits 8-13) or S/PDIF mode
// (kept). An unknown entry changes nothing (noted once).
void cellSoundSynth2SetCoreAttr(PPUContext&, u16 entry, u16 value)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    spu2::Core& c = s.spu.cores[entry & 1];
    const bool on = (value & 1) != 0;
    switch (entry & ~1u) {
    case kCoreEffectEnable: set_bit(c.attr, spu2::kAttrEffect, on); return;
    case kCoreIrqEnable:
        set_bit(c.attr, spu2::kAttrIrq, on);
        if (!on) c.irq = false;
        return;
    case kCoreMuteEnable: set_bit(c.attr, spu2::kAttrMute, on); return;
    case kCoreNoiseClock: c.attr = static_cast<u16>((c.attr & ~0x3F00u) | ((value & 0x3Fu) << 8)); return;
    case kCoreSpdifMode: c.spdif = value; return;
    default:
        warn_once(s, std::format("cellSoundSynth2SetCoreAttr({:#06x}, {:#x}): no such entry; ignored", entry, value));
    }
}

// int cellSoundSynth2Generate(u16 samples, float *Lout, float *Rout, float *Ls, float *Rs): the next `samples` samples
// at 48 kHz, the SPU2's output (core 1's) as floats (a sample over 32768) into Lout and Rout. Ls and Rs, the surround
// pair, may be NULL; given, they get silence: the SPU2 has a stereo output only (D-612). Lout or Rout NULL, or a
// buffer that is not guest memory: INVALID_PARAMETER, nothing rendered.
s32 cellSoundSynth2Generate(PPUContext& ctx, u16 samples, u32 lout, u32 rout, u32 ls, u32 rs)
{
    lv2::PpuThread& t = thread_of(ctx);
    const u64 bytes = u64{samples} * 4;
    if (!lout || !rout) return invalid(ctx, "cellSoundSynth2Generate: no Lout or Rout buffer");
    for (const u32 buffer : {lout, rout, ls, rs})
        if (buffer && !guest_range(t, buffer, bytes))
            return invalid(ctx,
                std::format("cellSoundSynth2Generate: {} samples at {:#x} are not guest memory", samples, buffer));
    u8* base = guest_base(ctx);
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    s16 left[kChunk], right[kChunk];
    for (u32 done = 0; done < samples;) {
        const u32 n = std::min<u32>(kChunk, samples - done);
        s.spu.render(left, right, n);
        for (u32 i = 0; i < n; i++) {
            ps_writef32(base, lout + 4 * (done + i), left[i] / 32768.0f);
            ps_writef32(base, rout + 4 * (done + i), right[i] / 32768.0f);
        }
        done += n;
    }
    for (const u32 buffer : {ls, rs})
        if (buffer)
            for (u32 i = 0; i < samples; i++) ps_writef32(base, buffer + 4 * i, 0.0f);
    return lv2::CELL_OK;
}

// int cellSoundSynth2VoiceTrans(short channel, u16 mode, u8 *m_addr, u32 s_addr, u32 size): copies `size` bytes from
// main memory into sound memory at s_addr (SD_TRANS_WRITE) or back (SD_TRANS_READ), DMA or I/O alike, at once; the
// channel's (core's) TSA becomes s_addr. Answers the bytes copied, as libsd does. Another mode (STOP),
// a range past sound memory or main memory that is not the guest's: INVALID_PARAMETER, nothing copied.
s32 cellSoundSynth2VoiceTrans(PPUContext& ctx, s16 channel, u16 mode, u32 m_addr, u32 s_addr, u32 size)
{
    const u32 direction = mode & 3;
    if (direction != kTransWrite && direction != kTransRead)
        return invalid(ctx, std::format("cellSoundSynth2VoiceTrans: mode {:#x} is no transfer", mode));
    if (s_addr >= spu2::kMemorySize || size > spu2::kMemorySize - s_addr)
        return invalid(ctx, std::format("cellSoundSynth2VoiceTrans: {:#x} bytes at {:#x} pass the 2 MB of sound memory",
            size, s_addr));
    if (size && (!m_addr || !guest_range(thread_of(ctx), m_addr, size)))
        return invalid(ctx, std::format("cellSoundSynth2VoiceTrans: {:#x} bytes at {:#x} are not guest memory", size,
            m_addr));
    u8* base = guest_base(ctx);
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    const u32 core = core_of(s, "cellSoundSynth2VoiceTrans", channel);
    if (direction == kTransWrite) std::memcpy(s.spu.memory.data() + s_addr, base + m_addr, size);
    else std::memcpy(base + m_addr, s.spu.memory.data() + s_addr, size);
    s.spu.cores[core].tsa = s_addr & ~1u;
    return static_cast<s32>(size);
}

// int cellSoundSynth2VoiceTransStatus(short channel, short flag): 1, the transfer done (VoiceTrans copies at once;
// libsd answers 1 for a finished one, and for flag 1 waits until then).
s32 cellSoundSynth2VoiceTransStatus(PPUContext&, s16 channel, s16 flag)
{
    Synth2& s = synth();
    std::lock_guard lock(s.mutex);
    core_of(s, "cellSoundSynth2VoiceTransStatus", channel);
    if (flag & ~1) warn_once(s, std::format("cellSoundSynth2VoiceTransStatus: flag {:#x}, bit 0 taken", flag));
    return 1;
}

// u16 cellSoundSynth2Note2Pitch(u16 center_note, u16 center_fine, u16 note, short fine): the pitch that plays `note`
// (and `fine`, in 1/128 semitones) of a sample whose `center_note`/`center_fine` plays at 0x1000. libsd's arithmetic:
// the semitone and fine factors from its tables multiplied (0x4000 at the centre's octave + 2), then halved, with
// rounding, for each octave lower; two octaves up and beyond stay at that octave's values (0x4000 up).
u32 cellSoundSynth2Note2Pitch(PPUContext&, u16 center_note, u16 center_fine, u16 note, s16 fine)
{
    const PitchTables& t = pitch_tables();
    const s32 total = s32{fine} + s32{center_fine};
    const s32 carry = floor_div(total, 128);
    const s32 rest = total - 128 * carry;
    const s32 offset = s32{note} + carry - s32{center_note};
    const s32 octave = floor_div(offset, 12);
    const s32 semitone = offset - 12 * octave;
    u32 pitch = (t.semitone[static_cast<u32>(semitone)] * t.fine[static_cast<u32>(rest)]) >> 16;
    const s32 down = 2 - octave;
    if (down > 0) pitch = down >= 17 ? 0 : (pitch + (1u << (down - 1))) >> down;
    return pitch & 0xFFFF;
}

// u16 cellSoundSynth2Pitch2Note(u16 center_note, u16 center_fine, u16 pitch): the note (high byte) and fine (low byte,
// even) a pitch plays, libsd's inverse: the pitch (at most 0x3FFF) normalised to its top bit, the largest semitone and
// then fine factor not above it, the fine rounded by one step.
u32 cellSoundSynth2Pitch2Note(PPUContext&, u16 center_note, u16 center_fine, u16 pitch)
{
    const PitchTables& t = pitch_tables();
    const u32 p = std::min<u32>(pitch, 0x3FFF);
    const u32 top = p ? static_cast<u32>(std::bit_width(p)) - 1 : 0;
    const u32 m = (p << (15 - top)) & 0xFFFF;
    u32 semitone = 0;
    for (u32 i = 12; i-- > 0;)
        if (m >= t.semitone[i]) {
            semitone = i;
            break;
        }
    const u32 ratio = ((m * 0x8000) / t.semitone[semitone]) & 0xFFFF;
    u32 fine = 0;
    for (u32 i = 128; i-- > 0;)
        if (ratio >= t.fine[i]) {
            fine = i;
            break;
        }
    const u32 f = (u32{center_fine} + fine + 1) & 0xFFFF;
    const s32 note = s32{center_note} + static_cast<s32>(semitone) + 12 * (static_cast<s32>(top) - 12) +
        static_cast<s32>(f / 128);
    return static_cast<u32>((note * 256 + static_cast<s32>(f & 0x7E)) & 0xFFFE);
}

} // namespace

void register_libsynth2(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x846EADC6, cellSoundSynth2Config),
        PS_HLE_FUNC(0x03D1843C, cellSoundSynth2Init),
        PS_HLE_FUNC(0x699C4E5E, cellSoundSynth2Exit),
        PS_HLE_FUNC(0x6F87A42F, cellSoundSynth2SetParam),
        PS_HLE_FUNC(0xD374FA07, cellSoundSynth2GetParam),
        PS_HLE_FUNC(0x363883A6, cellSoundSynth2SetSwitch),
        PS_HLE_FUNC(0x49697FBC, cellSoundSynth2GetSwitch),
        PS_HLE_FUNC(0x2165E05A, cellSoundSynth2SetAddr),
        PS_HLE_FUNC(0x820C8DCE, cellSoundSynth2GetAddr),
        PS_HLE_FUNC(0x7A218453, cellSoundSynth2SetEffectAttr),
        PS_HLE_FUNC(0x4F5094B1, cellSoundSynth2SetEffectMode),
        PS_HLE_FUNC(0xF01E5FD4, cellSoundSynth2SetCoreAttr),
        PS_HLE_FUNC(0x57F0D642, cellSoundSynth2Generate),
        PS_HLE_FUNC(0xD7D873FB, cellSoundSynth2VoiceTrans),
        PS_HLE_FUNC(0x7B9291B5, cellSoundSynth2VoiceTransStatus),
        PS_HLE_FUNC(0xA61674A4, cellSoundSynth2Note2Pitch),
        PS_HLE_FUNC(0x75C2A4E2, cellSoundSynth2Pitch2Note),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
