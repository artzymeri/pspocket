#pragma once

// The PlayStation 2's sound processor, the SPU2, in software: libsynth2's synthesizer (D-611). Two cores of 24 voices
// over 2 MB of sound memory, rendered one 48 kHz sample at a time.
//
// What each part does, and where the rule comes from (no PS2 emulator's code is used; PCSX2 is GPLv3):
// - Voices play 4-bit ADPCM ("VAG") blocks of 16 bytes: a shift/filter byte, a flag byte (bit 0 loop end, bit 1
//   repeat, bit 2 loop start) and 28 nibbles, decoded with the five prediction filters; reserved shifts 13 to 15 act
//   as 9. The pitch counter (4096 = one sample a tick, here 48 kHz) steps through the samples, at most 0x4000 a tick,
//   with pitch modulation by the previous voice's output, and the four latest samples are interpolated with the
//   SPU's gaussian table. A loop end sets the voice's ENDX bit and jumps to the loop address (with the repeat bit
//   clear, the envelope also goes to zero); a loop start copies the block's address to the loop address, unless the
//   loop address was written since the voice was keyed on. psx-spx, "SPU ADPCM Samples", "SPU ADPCM Pitch", "4-Point
//   Gaussian Interpolation", "SPU Voice Flags"; the PS1's SPU is one SPU2 core ("the PS2's SPU2 is basically two PS1
//   SPU cores sharing the same memory"); the written loop address that a loop start leaves alone is the SPU2's as
//   PCSX2's tests found it (pcsx2/SPU2/Mixer.cpp's note on LOOP/START), made here from that description.
// - The envelopes (ADSR) and the swept volumes step as psx-spx's "Envelope Operation depending on
//   Shift/Step/Mode/Direction" has it; noise is psx-spx's "SPU Noise Generator", clocked by the core's 6-bit noise
//   clock (ATTR bits 8-13, shift and step).
// - Each core mixes its voices into dry and wet (effect) sums by the VMIXL/VMIXR/VMIXEL/VMIXER bits, then by MMIX
//   (bits 11-8: the voices to dry L, dry R, wet L, wet R; bits 7-4 the memory input, bits 3-0 the external input):
//   core 0's output, after its master volume, is core 1's external input (times AVOL), and core 1's output, after
//   its master volume, is what the SPU2 plays (ps2sdk freesd.c InitCoreVolume: "The external Input is Core 0's
//   output", and its MMIX values 0xFF0 and 0xFFC). libsynth2 has no block (streaming) transfer, so the memory input
//   is silent.
// - The reverb is psx-spx's "SPU Reverb Formula" at half the rate (24 kHz), with its 39-tap filter between the two
//   rates, in each core's work area [ESA, EEA] of sound memory (16-bit words, the PPU's byte order). With the core's
//   effect bit clear it neither writes nor plays, and its address still moves on.
// - A voice reading the block at a core's IRQ address with that core's IRQ enabled raises the core's IRQ flag.

#include <array>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::spu2 {

inline constexpr u32 kMemorySize = 0x200000; // 2 MB of sound memory, byte addressed here
inline constexpr u32 kCores = 2;
inline constexpr u32 kVoices = 24;
inline constexpr u32 kVoiceMask = 0xFFFFFF;
inline constexpr u32 kBlockBytes = 16, kBlockSamples = 28;

// The reverb's 32 registers in the order of the PS1's (psx-spx "SPU Reverb Registers") and of libsd's presets: two
// all-pass displacements, eight volumes, twenty addresses relative to the buffer's current address, two input volumes.
// The displacements and addresses count 8-byte units (four 16-bit words), as the PS1's and libsd's presets do.
enum ReverbReg : u32 {
    dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2,
    mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF,
    mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2,
    vLIN, vRIN,
    kReverbRegs
};

enum class Phase : u8 { Off, Attack, Decay, Sustain, Release };

// ATTR bits (psx-spx "SPU Control and Status Register"; libsd's SD_CORE_* entries set them).
inline constexpr u16 kAttrExternalInput = 0x0001, kAttrIrq = 0x0040, kAttrEffect = 0x0080, kAttrMute = 0x4000,
                     kAttrOn = 0x8000;

struct Voice {
    // The voice's registers: volumes (direct, or a sweep when bit 15 is set), pitch, ADSR, start and loop addresses.
    u16 voll = 0, volr = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
    u32 ssa = 0, lsax = 0;
    bool lsax_written = false; // LSAX written since the key on: the blocks' loop start flags leave it alone
    // What it is playing: the block (NAX) and its two header bytes, the sample in it, the pitch counter's fraction.
    u32 nax = 0;
    u8 header = 0, flags = 0;
    u32 index = 0, counter = 0;
    Phase phase = Phase::Off;
    s16 envx = 0, volxl = 0, volxr = 0, outx = 0;
    u32 env_counter = 0, voll_counter = 0, volr_counter = 0;
    std::array<s16, kBlockSamples> block{}; // the block's samples, decoded
    std::array<s16, 4> ring{};              // the oldest, older, old and newest samples, interpolated
    s16 prev1 = 0, prev2 = 0;               // the ADPCM decoder's last two samples
};

// The reverb's work between the 48 kHz mix and its 24 kHz steps.
struct ReverbState {
    std::array<s16, 64> in_l{}, in_r{}; // the wet mix, the last 64 samples (a ring)
    std::array<s16, 64> up_l{}, up_r{}; // the reverb's output, one sample in two (zero between)
    u32 pos = 0;
    bool step = false; // this sample is a 24 kHz step
    u32 addr = 0;      // the buffer's current address (a word)
};

struct Core {
    std::array<Voice, kVoices> voices{};
    // Voice bit masks: pitch modulation, noise, key on and off (as last written), loop end reached, the mixes.
    u32 pmon = 0, non = 0, kon = 0, koff = 0, endx = 0, vmixl = 0, vmixel = 0, vmixr = 0, vmixer = 0;
    u16 mmix = 0, attr = 0, spdif = 0;
    u16 mvoll = 0, mvolr = 0; // the master volume's registers (direct or swept)
    s16 mvolxl = 0, mvolxr = 0;
    u32 mvoll_counter = 0, mvolr_counter = 0;
    s16 evoll = 0, evolr = 0, avoll = 0, avolr = 0, bvoll = 0, bvolr = 0;
    // Byte addresses: the effect work area's start and end (EEA's low 17 bits are all set), transfer start, IRQ.
    u32 esa = 0, eea = 0x1FFFF, tsa = 0, irqa = 0;
    bool irq = false;
    std::array<u16, kReverbRegs> reverb{};
    ReverbState rev;
    u16 noise = 0;
    s32 noise_timer = 0;
};

class Spu2 {
public:
    Spu2() : memory(kMemorySize, 0) {}

    std::vector<u8> memory;
    std::array<Core, kCores> cores{};

    // KON: each voice in `mask` starts at its start address, its envelope in attack from zero, its ENDX bit clear.
    void key_on(u32 core, u32 mask);
    // KOFF: each voice in `mask` that plays goes to release.
    void key_off(u32 core, u32 mask);
    // ESA: the work area's start, where the reverb's buffer address goes too.
    void set_esa(u32 core, u32 addr);
    // NAX: the voice goes on from the block at `addr`.
    void set_nax(u32 core, u32 voice, u32 addr);
    // LSAX: where the voice's loop goes back to, whatever loop start flags it meets until its next key on.
    void set_lsax(u32 core, u32 voice, u32 addr);
    // The next `samples` samples of the SPU2's output (core 1's), left and right.
    void render(s16* left, s16* right, u32 samples);

    // One 48 kHz step of an envelope or a swept volume at `level` (psx-spx): `rate` is shift << 2 | step, `mask` the
    // rate with every settable bit set (0x7F; 0x7C for decay and release, whose step is fixed), at which it never
    // steps. Returns the new level.
    static s32 envelope_tick(s32 level, u32 rate, u32 mask, bool decreasing, bool exponential, bool negative,
        u32& counter);
    // The gaussian interpolation table (psx-spx, 512 entries).
    static const std::array<s16, 512>& gauss();

private:
    void fetch(Voice& v);
    void next_block(Core& c, Voice& v, u32 bit);
    void decode(Voice& v);
    void tick_voice(Core& c, u32 index, s32 mod, s32 (&mix)[4]);
    void tick_core(Core& c, s32 ext_l, s32 ext_r, s16& out_l, s16& out_r);
    void reverb(Core& c, s32 in_l, s32 in_r, s32& out_l, s32& out_r);
    void reverb_step(Core& c, s32 in_l, s32 in_r, s16& out_l, s16& out_r);
    s16 word(u32 w) const;
    void set_word(u32 w, s32 v);
};

} // namespace pspocket::hle::spu2
