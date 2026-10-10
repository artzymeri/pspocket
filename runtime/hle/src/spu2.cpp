// The SPU2 in software, libsynth2's synthesizer (D-611). spu2.hpp lists each part and the documents its rules come
// from: psx-spx for the voice, envelope, noise and reverb arithmetic the SPU2 shares with the PS1's SPU, ps2sdk's libsd
// headers and freesd for the SPU2's registers and its two cores' routing. Written for pspocket; no emulator's code.

#include "spu2.hpp"

#include <algorithm>

namespace pspocket::hle::spu2 {
namespace {

constexpr u32 kAddrMask = kMemorySize - 1;

s32 clamp16(s64 v) { return static_cast<s32>(std::clamp<s64>(v, -0x8000, 0x7FFF)); }

// The ADPCM prediction filters (psx-spx "CDROM XA Audio ADPCM Compression": pos/neg_xa_adpcm_table; SPU-ADPCM has all
// five). Filters 5 to 7 are not documented and Sony's encoders never write them: they predict nothing here.
constexpr s32 kFilter0[5] = {0, 60, 115, 98, 122};
constexpr s32 kFilter1[5] = {0, 0, -52, -55, -60};

// psx-spx "4-Point Gaussian Interpolation" (each four entries i, 0xFF - i, 0x100 + i, 0x1FF - i sum to 0x7F7F..0x7F81).
constexpr std::array<s16, 512> kGauss = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};

// psx-spx "Reverb Buffer Resampling": the 39 taps between the mix's rate and the reverb's half rate. Besides the
// middle one, every odd tap is zero.
constexpr s32 kFir[39] = {
    -1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384,
    10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1,
};

// The filter over the last 39 samples of a ring ending at `pos`.
s32 fir(const std::array<s16, 64>& ring, u32 pos)
{
    s32 sum = kFir[19] * ring[(pos - 19) & 63];
    for (u32 k = 0; k < 39; k += 2) sum += kFir[k] * ring[(pos - k) & 63];
    return sum;
}

// The sustain level of ADSR1's bits 0-3: (N + 1) * 0x800 (psx-spx).
s32 sustain_level(const Voice& v) { return ((v.adsr1 & 0xF) + 1) * 0x800; }

// The end of the attack: decay, or sustain at once when the level is not above the sustain level (a sustain level of
// 15, 0x8000, sustains at the attack's peak).
void enter_decay(Voice& v)
{
    v.phase = v.envx <= sustain_level(v) ? Phase::Sustain : Phase::Decay;
    v.env_counter = 0;
}

// One step of the ADSR envelope (psx-spx "SPU Volume and ADSR Generator"): attack (ADSR1 bit 15 exponential, bits 8-14
// the rate) to 0x7FFF; decay (exponential, bits 4-7 the shift, step -8) to the sustain level; sustain (ADSR2 bit 15
// exponential, bit 14 decreasing, bits 6-12 the rate) until key off; release (bit 5 exponential, bits 0-4 the shift)
// to zero, where the voice stops.
void tick_adsr(Voice& v)
{
    switch (v.phase) {
    case Phase::Off:
        return;
    case Phase::Attack:
        v.envx = static_cast<s16>(Spu2::envelope_tick(v.envx, (v.adsr1 >> 8) & 0x7F, 0x7F, false,
            (v.adsr1 & 0x8000) != 0, false, v.env_counter));
        if (v.envx >= 0x7FFF) enter_decay(v);
        return;
    case Phase::Decay:
        v.envx = static_cast<s16>(
            Spu2::envelope_tick(v.envx, ((v.adsr1 >> 4) & 0xF) << 2, 0x7C, true, true, false, v.env_counter));
        if (v.envx <= sustain_level(v)) {
            v.phase = Phase::Sustain;
            v.env_counter = 0;
        }
        return;
    case Phase::Sustain:
        v.envx = static_cast<s16>(Spu2::envelope_tick(v.envx, (v.adsr2 >> 6) & 0x7F, 0x7F, (v.adsr2 & 0x4000) != 0,
            (v.adsr2 & 0x8000) != 0, false, v.env_counter));
        return;
    case Phase::Release:
        v.envx = static_cast<s16>(Spu2::envelope_tick(v.envx, (v.adsr2 & 0x1F) << 2, 0x7C, true,
            (v.adsr2 & 0x20) != 0, false, v.env_counter));
        if (v.envx <= 0) {
            v.envx = 0;
            v.phase = Phase::Off;
        }
        return;
    }
}

// One step of a volume register (psx-spx: VOLL/VOLR and MVOLL/MVOLR): bit 15 clear, the level is bits 0-14 times two;
// set, a sweep from the current level (bit 14 exponential, bit 13 decreasing, bit 12 negative phase, bits 0-6 the
// rate).
void tick_volume(u16 reg, s16& level, u32& counter)
{
    if (!(reg & 0x8000)) {
        level = static_cast<s16>(static_cast<u16>(reg << 1));
        counter = 0;
        return;
    }
    level = static_cast<s16>(Spu2::envelope_tick(level, reg & 0x7F, 0x7F, (reg & 0x2000) != 0, (reg & 0x4000) != 0,
        (reg & 0x1000) != 0, counter));
}

// One step of the core's noise generator (psx-spx "SPU Noise Generator"), the noise clock's shift in bits 2-5 and step
// in bits 0-1.
void tick_noise(Core& c)
{
    const u32 clock = (c.attr >> 8) & 0x3F;
    const u32 shift = clock >> 2, step = (clock & 3) + 4;
    c.noise_timer -= static_cast<s32>(step);
    const u32 n = c.noise;
    const u32 parity = ((n >> 15) ^ (n >> 12) ^ (n >> 11) ^ (n >> 10) ^ 1) & 1;
    if (c.noise_timer < 0) {
        c.noise = static_cast<u16>((n << 1) | parity);
        c.noise_timer += static_cast<s32>(0x20000u >> shift);
    }
    if (c.noise_timer < 0) c.noise_timer += static_cast<s32>(0x20000u >> shift);
}

// The gaussian interpolation of the voice's four latest samples at the pitch counter's bits 4-11.
s32 interpolate(const Voice& v)
{
    const u32 i = (v.counter >> 4) & 0xFF;
    s32 out = (kGauss[0xFF - i] * v.ring[0]) >> 15;
    out += (kGauss[0x1FF - i] * v.ring[1]) >> 15;
    out += (kGauss[0x100 + i] * v.ring[2]) >> 15;
    out += (kGauss[i] * v.ring[3]) >> 15;
    return clamp16(out);
}

// A reverb address: `offset` words from the buffer's current address, wrapped within the work area [ESA, EEA].
u32 reverb_at(const Core& c, s64 offset)
{
    const u32 start = c.esa >> 1;
    const s64 len = s64{c.eea >> 1} - start + 1;
    s64 rel = (s64{c.rev.addr} - start + offset) % len;
    if (rel < 0) rel += len;
    return start + static_cast<u32>(rel);
}

} // namespace

const std::array<s16, 512>& Spu2::gauss() { return kGauss; }

s32 Spu2::envelope_tick(s32 level, u32 rate, u32 mask, bool decreasing, bool exponential, bool negative, u32& counter)
{
    const s32 shift = static_cast<s32>(rate >> 2);
    s32 step = 7 - static_cast<s32>(rate & 3); // +7, +6, +5, +4
    if (decreasing != negative) step = ~step;  // -8, -7, -6, -5
    step *= 1 << std::max(0, 11 - shift);
    u32 increment = 0x8000u >> std::max(0, shift - 11);
    if (exponential && !decreasing && level > 0x6000) {
        if (shift < 10) {
            step >>= 2;
        } else if (shift >= 11) {
            increment >>= 2;
        } else {
            step >>= 1;
            increment >>= 1;
        }
    } else if (exponential && decreasing) {
        step = (step * level) >> 15;
    }
    if (rate != mask) increment = std::max(increment, 1u);
    counter += increment;
    if (!(counter & 0x8000)) return level;
    counter = 0;
    level += step;
    if (!decreasing) return std::clamp(level, -0x8000, 0x7FFF);
    if (negative) return std::clamp(level, -0x8000, 0);
    return std::max(level, 0);
}

s16 Spu2::word(u32 w) const
{
    const u32 b = (w * 2) & kAddrMask;
    return static_cast<s16>(static_cast<u16>((memory[b] << 8) | memory[b + 1]));
}

void Spu2::set_word(u32 w, s32 v)
{
    const u32 b = (w * 2) & kAddrMask;
    const u16 x = static_cast<u16>(clamp16(v));
    memory[b] = static_cast<u8>(x >> 8);
    memory[b + 1] = static_cast<u8>(x);
}

void Spu2::decode(Voice& v)
{
    u32 shift = v.header & 0xF;
    if (shift > 12) shift = 9; // reserved shifts act as 9 (psx-spx)
    u32 filter = (v.header >> 4) & 7;
    if (filter > 4) filter = 0;
    const s32 f0 = kFilter0[filter], f1 = kFilter1[filter];
    for (u32 i = 0; i < kBlockSamples; i++) {
        // The nibbles low first; each the top four bits of a 16-bit sample shifted right, plus the prediction.
        const u8 byte = memory[(v.nax + 2 + i / 2) & kAddrMask];
        const u32 nibble = (byte >> ((i & 1) * 4)) & 0xF;
        const s32 s = static_cast<s16>(static_cast<u16>(nibble << 12)) >> shift;
        const s16 out = static_cast<s16>(clamp16(s + ((v.prev1 * f0 + v.prev2 * f1 + 32) >> 6)));
        v.prev2 = v.prev1;
        v.prev1 = out;
        v.block[i] = out;
    }
}

void Spu2::fetch(Voice& v)
{
    v.header = memory[v.nax & kAddrMask];
    v.flags = memory[(v.nax + 1) & kAddrMask];
    if ((v.flags & 4) && !v.lsax_written) v.lsax = v.nax; // loop start: where the loop goes back to
    for (Core& k : cores)
        if ((k.attr & kAttrIrq) && k.irqa >= v.nax && k.irqa < v.nax + kBlockBytes) k.irq = true;
    if (v.phase != Phase::Off) decode(v); // a stopped voice moves on through its blocks without decoding them
}

void Spu2::next_block(Core& c, Voice& v, u32 bit)
{
    if (v.flags & 1) { // loop end: ENDX, back to the loop address; without the repeat bit, silence
        c.endx |= bit;
        v.nax = v.lsax;
        if (!(v.flags & 2)) {
            v.phase = Phase::Off;
            v.envx = 0;
        }
    } else {
        v.nax = (v.nax + kBlockBytes) & kAddrMask;
    }
    fetch(v);
}

void Spu2::key_on(u32 core, u32 mask)
{
    Core& c = cores[core & 1];
    c.kon = mask & kVoiceMask;
    for (u32 i = 0; i < kVoices; i++) {
        if (!(c.kon >> i & 1)) continue;
        Voice& v = c.voices[i];
        v.nax = v.ssa & kAddrMask & ~(kBlockBytes - 1);
        v.phase = Phase::Attack;
        v.envx = 0;
        v.env_counter = 0;
        v.counter = 0;
        v.index = 0;
        v.prev1 = v.prev2 = 0;
        v.ring = {};
        v.lsax_written = false;
        c.endx &= ~(1u << i);
        fetch(v);
        v.ring[3] = v.block[0];
    }
}

void Spu2::key_off(u32 core, u32 mask)
{
    Core& c = cores[core & 1];
    c.koff = mask & kVoiceMask;
    for (u32 i = 0; i < kVoices; i++) {
        Voice& v = c.voices[i];
        if ((c.koff >> i & 1) && v.phase != Phase::Off) {
            v.phase = Phase::Release;
            v.env_counter = 0;
        }
    }
}

void Spu2::set_esa(u32 core, u32 addr)
{
    Core& c = cores[core & 1];
    c.esa = addr & kAddrMask;
    c.rev.addr = c.esa >> 1;
}

void Spu2::set_nax(u32 core, u32 voice, u32 addr)
{
    Voice& v = cores[core & 1].voices[voice % kVoices];
    v.nax = addr & kAddrMask & ~(kBlockBytes - 1);
    v.index = 0;
    fetch(v);
    if (v.phase != Phase::Off) v.ring = {v.ring[1], v.ring[2], v.ring[3], v.block[0]};
}

void Spu2::set_lsax(u32 core, u32 voice, u32 addr)
{
    Voice& v = cores[core & 1].voices[voice % kVoices];
    v.lsax = addr & kAddrMask & ~(kBlockBytes - 1);
    v.lsax_written = true;
}

// One 48 kHz step of a voice: its envelope and volumes, its output (the interpolated sample or the noise, times the
// envelope) into the mixes its bits select (dry L, dry R, wet L, wet R), then its pitch counter, modulated by `mod`
// (the previous voice's output) when its PMON bit is set.
void Spu2::tick_voice(Core& c, u32 index, s32 mod, s32 (&mix)[4])
{
    Voice& v = c.voices[index];
    const u32 bit = 1u << index;
    tick_adsr(v);
    tick_volume(v.voll, v.volxl, v.voll_counter);
    tick_volume(v.volr, v.volxr, v.volr_counter);
    s32 out = 0;
    if (v.phase != Phase::Off) {
        const s32 sample = (c.non & bit) ? s32{static_cast<s16>(c.noise)} : interpolate(v);
        out = (sample * v.envx) >> 15;
    }
    v.outx = static_cast<s16>(out);
    const s32 l = (out * v.volxl) >> 15, r = (out * v.volxr) >> 15;
    if (c.vmixl & bit) mix[0] += l;
    if (c.vmixr & bit) mix[1] += r;
    if (c.vmixel & bit) mix[2] += l;
    if (c.vmixer & bit) mix[3] += r;

    u32 step = v.pitch;
    if (index > 0 && (c.pmon & bit)) {
        // psx-spx "Pitch Counter": the step (sign-extended) times the previous voice's output plus 0x8000, over 0x8000.
        const s32 factor = mod + 0x8000;
        step = static_cast<u32>((s32{static_cast<s16>(v.pitch)} * factor) >> 15) & 0xFFFF;
    }
    if (step > 0x3FFF) step = 0x4000;
    v.counter += step;
    for (u32 n = v.counter >> 12; n; n--) {
        if (++v.index == kBlockSamples) {
            v.index = 0;
            next_block(c, v, bit);
        }
        v.ring = {v.ring[1], v.ring[2], v.ring[3], v.phase != Phase::Off ? v.block[v.index] : s16{0}};
    }
    v.counter &= 0xFFF;
}

// One 48 kHz step of a core: its noise, its 24 voices, the voices' and the external input's dry and wet sums by MMIX,
// the reverb over the wet sum, then the master volume over dry plus reverb.
void Spu2::tick_core(Core& c, s32 ext_l, s32 ext_r, s16& out_l, s16& out_r)
{
    tick_noise(c);
    s32 mix[4] = {0, 0, 0, 0};
    s32 mod = 0;
    for (u32 i = 0; i < kVoices; i++) {
        tick_voice(c, i, mod, mix);
        mod = c.voices[i].outx;
    }
    const s32 voice_l = clamp16(mix[0]), voice_r = clamp16(mix[1]), voice_el = clamp16(mix[2]),
              voice_er = clamp16(mix[3]);
    const s32 el = (ext_l * c.avoll) >> 15, er = (ext_r * c.avolr) >> 15;
    // MMIX bits 11-8 the voices, 3-0 the external input (bits 7-4, the memory input, have nothing to mix here).
    s32 dry_l = 0, dry_r = 0, wet_l = 0, wet_r = 0;
    if (c.mmix & 0x800) dry_l += voice_l;
    if (c.mmix & 0x400) dry_r += voice_r;
    if (c.mmix & 0x200) wet_l += voice_el;
    if (c.mmix & 0x100) wet_r += voice_er;
    if (c.mmix & 0x008) dry_l += el;
    if (c.mmix & 0x004) dry_r += er;
    if (c.mmix & 0x002) wet_l += el;
    if (c.mmix & 0x001) wet_r += er;
    s32 rev_l = 0, rev_r = 0;
    reverb(c, clamp16(wet_l), clamp16(wet_r), rev_l, rev_r);
    tick_volume(c.mvoll, c.mvolxl, c.mvoll_counter);
    tick_volume(c.mvolr, c.mvolxr, c.mvolr_counter);
    out_l = static_cast<s16>(clamp16((s64{clamp16(dry_l + rev_l)} * c.mvolxl) >> 15));
    out_r = static_cast<s16>(clamp16((s64{clamp16(dry_r + rev_r)} * c.mvolxr) >> 15));
}

// The reverb at the mix's rate: the wet sum into the filter's ring; every second sample a reverb step on the filtered
// input (when the effect is on), its output into the other ring between zeros, filtered back (twice the gain).
void Spu2::reverb(Core& c, s32 in_l, s32 in_r, s32& out_l, s32& out_r)
{
    ReverbState& r = c.rev;
    r.pos = (r.pos + 1) & 63;
    r.in_l[r.pos] = static_cast<s16>(in_l);
    r.in_r[r.pos] = static_cast<s16>(in_r);
    s16 yl = 0, yr = 0;
    if (r.step) {
        const u32 start = c.esa >> 1, end = c.eea >> 1;
        if (start <= end) {
            if (r.addr < start || r.addr > end) r.addr = start;
            if (c.attr & kAttrEffect)
                reverb_step(c, clamp16(fir(r.in_l, r.pos) >> 15), clamp16(fir(r.in_r, r.pos) >> 15), yl, yr);
            r.addr = r.addr == end ? start : r.addr + 1;
        }
    }
    r.step = !r.step;
    r.up_l[r.pos] = yl;
    r.up_r[r.pos] = yr;
    out_l = clamp16(fir(r.up_l, r.pos) >> 14);
    out_r = clamp16(fir(r.up_r, r.pos) >> 14);
}

// One 24 kHz step of psx-spx's "Reverb Formula": the same side and different side reflections, the four combs, the two
// all-pass filters, the effect volume. Values written to the work area saturate; products are over 0x8000.
void Spu2::reverb_step(Core& c, s32 in_l, s32 in_r, s16& out_l, s16& out_r)
{
    const std::array<u16, kReverbRegs>& g = c.reverb;
    const auto vol = [&](u32 reg) { return s64{static_cast<s16>(g[reg])}; };
    const auto off = [&](u32 reg) { return s64{g[reg]} * 4; }; // 8-byte units, in words
    const auto rd = [&](s64 o) { return s64{word(reverb_at(c, o))}; };
    const auto wr = [&](s64 o, s64 v) { set_word(reverb_at(c, o), clamp16(v)); };

    const s64 lin = (vol(vLIN) * in_l) >> 15, rin = (vol(vRIN) * in_r) >> 15;
    const s64 iir = vol(vIIR), wall = vol(vWALL);
    const s64 lsame_prev = rd(off(mLSAME) - 1), rsame_prev = rd(off(mRSAME) - 1);
    const s64 ldiff_prev = rd(off(mLDIFF) - 1), rdiff_prev = rd(off(mRDIFF) - 1);
    const s64 lsame = (((lin + ((rd(off(dLSAME)) * wall) >> 15) - lsame_prev) * iir) >> 15) + lsame_prev;
    const s64 rsame = (((rin + ((rd(off(dRSAME)) * wall) >> 15) - rsame_prev) * iir) >> 15) + rsame_prev;
    const s64 ldiff = (((lin + ((rd(off(dRDIFF)) * wall) >> 15) - ldiff_prev) * iir) >> 15) + ldiff_prev;
    const s64 rdiff = (((rin + ((rd(off(dLDIFF)) * wall) >> 15) - rdiff_prev) * iir) >> 15) + rdiff_prev;
    wr(off(mLSAME), lsame);
    wr(off(mRSAME), rsame);
    wr(off(mLDIFF), ldiff);
    wr(off(mRDIFF), rdiff);

    s64 l = clamp16((vol(vCOMB1) * rd(off(mLCOMB1)) + vol(vCOMB2) * rd(off(mLCOMB2)) + vol(vCOMB3) * rd(off(mLCOMB3)) +
                        vol(vCOMB4) * rd(off(mLCOMB4))) >> 15);
    s64 r = clamp16((vol(vCOMB1) * rd(off(mRCOMB1)) + vol(vCOMB2) * rd(off(mRCOMB2)) + vol(vCOMB3) * rd(off(mRCOMB3)) +
                        vol(vCOMB4) * rd(off(mRCOMB4))) >> 15);

    const auto apf = [&](s64 x, u32 m, u32 d, s64 v) {
        const s64 delayed = rd(off(m) - off(d));
        const s64 y = clamp16(x - ((v * delayed) >> 15));
        wr(off(m), y);
        return s64{clamp16(((y * v) >> 15) + delayed)};
    };
    l = apf(l, mLAPF1, dAPF1, vol(vAPF1));
    r = apf(r, mRAPF1, dAPF1, vol(vAPF1));
    l = apf(l, mLAPF2, dAPF2, vol(vAPF2));
    r = apf(r, mRAPF2, dAPF2, vol(vAPF2));
    out_l = static_cast<s16>(clamp16((l * c.evoll) >> 15));
    out_r = static_cast<s16>(clamp16((r * c.evolr) >> 15));
}

void Spu2::render(s16* left, s16* right, u32 samples)
{
    for (u32 n = 0; n < samples; n++) {
        s16 l0 = 0, r0 = 0, l1 = 0, r1 = 0;
        tick_core(cores[0], 0, 0, l0, r0);
        tick_core(cores[1], l0, r0, l1, r1);
        left[n] = l1;
        right[n] = r1;
    }
}

} // namespace pspocket::hle::spu2
