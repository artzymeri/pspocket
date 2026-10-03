// PPU interpreter, VMX (AltiVec) instructions: scalar code over the host lanes of ps_v128 (D-004 lane order).
//
// Semantics are rpcs3/Emu/Cell/PPUInterpreter.cpp with the accuracy options on (set_sat, use_nj, set_vnan) and its
// vector helpers in rpcs3/util/simd.hpp, written out per element. The lifter emits NEON for the same instructions
// (pspocket/ppu/vmx.h, which documents the model); the differential fuzz test compares the two bit for bit.
// Deliberate differences from RPCS3's interpreter, both following the architecture and RPCS3's LLVM recompiler:
// - mfvscr/mtvscr use guest word 3 (host lane 0); RPCS3's interpreter uses word 0.
// - lvrx with an aligned address reads nothing (the result is zero either way; RPCS3 reads the instruction's line).

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/ppu/interpreter.hpp"

#pragma clang fp contract(off)

namespace pspocket::ppu {
namespace {

using V = ps_v128;

inline float fl(u32 bits) { return std::bit_cast<float>(bits); }
inline u32 bits(float f) { return std::bit_cast<u32>(f); }
inline bool is_nan(u32 x) { return (x & 0x7f800000u) == 0x7f800000u && (x & 0x007fffffu) != 0; }

inline u128 to128(const V& v)
{
    u128 x;
    std::memcpy(&x, &v, 16);
    return x;
}

inline V from128(u128 x)
{
    V v;
    std::memcpy(&v, &x, 16);
    return v;
}

inline s8 s8_(u8 x) { return static_cast<s8>(x); }
inline s16 s16_(u16 x) { return static_cast<s16>(x); }
inline s32 s32_(u32 x) { return static_cast<s32>(x); }

// RPCS3 ppu_set_vnan (set_vnan + fix_vnan): the first NaN operand quieted, else the default NaN for a NaN result.
inline u32 vnan(u32 r) { return is_nan(r) ? 0x7fc00000u : r; }
inline u32 vnan(u32 r, u32 a) { return is_nan(a) ? (a | 0x7fc00000u) : vnan(r); }
inline u32 vnan(u32 r, u32 a, u32 b) { return is_nan(a) ? (a | 0x7fc00000u) : vnan(r, b); }

// rpcs3/util/simd.hpp utils::roundevenf32 (round half to even on the bit pattern; NaN/inf/large pass through).
u32 roundeven_bits(u32 val)
{
    const u32 exp = (val >> 23) & 0xff;
    const u32 abs = val & 0x7fffffff;
    if (exp >= 127 + 23) return val;
    if (exp >= 127) {
        const u32 int_pos = (127 + 23) - exp;
        const u32 half_bit = 1u << (int_pos - 1);
        const u32 int_bit = 1u << int_pos;
        if (val & (int_bit | (half_bit - 1))) val += half_bit;
        return val & ~(int_bit - 1);
    }
    if (exp == 126 && abs > 0x3f000000) return (val & 0x80000000u) | 0x3f800000u;
    return val & 0x80000000u;
}

// Load/store of an aligned quadword as two big-endian doublewords: guest bytes 0-7 are host bytes 15-8.
V read_quad(u8* base, u32 ea)
{
    V v;
    v.u64[1] = ps_read64(base, ea);
    v.u64[0] = ps_read64(base, ea + 8);
    return v;
}

void write_quad(u8* base, u32 ea, const V& v)
{
    ps_write64(base, ea, v.u64[1]);
    ps_write64(base, ea + 8, v.u64[0]);
}

// Saturating conversion of an exact intermediate to T, recording saturation.
template <class T>
T saturate(s64 v, bool& sat)
{
    constexpr s64 lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    if (v < lo) {
        sat = true;
        return static_cast<T>(lo);
    }
    if (v > hi) {
        sat = true;
        return static_cast<T>(hi);
    }
    return static_cast<T>(v);
}

u16 pack_pixel(u32 w) { return static_cast<u16>(((w >> 9) & 0xFC00) | ((w >> 6) & 0x03E0) | ((w >> 3) & 0x001F)); }

u32 unpack_pixel(u16 h)
{
    const u32 x = static_cast<u32>(static_cast<s32>(static_cast<s16>(h)));
    return ((x << 6) & 0x1f0000u) | ((x << 3) & 0x1f00u) | (x & 0xff00001fu);
}

} // namespace

void execute_vmx(PPUContext& ctx, u8* base, const Inst& in)
{
    const u32 vd = in.rt(), va = in.ra(), vb = in.rb(), vc = in.rc_reg();
    const V a = ctx.vr[va], b = ctx.vr[vb], c = ctx.vr[vc];
    const bool nj = ctx.vscr_nj != 0;
    V d{};
    bool sat = false, write = true;

    // ppu_flush_denormal with the NJ mask: denormals (and zeros) become signed zero when NJ = 1.
    auto flush = [nj](u32 x) { return (nj && (x & 0x7f800000u) == 0) ? (x & 0x80000000u) : x; };
    // Effective address of the X-form loads and stores: (rA|0) + rB.
    auto ea = [&] { return static_cast<u32>((va ? ctx.gpr[va] : 0) + ctx.gpr[vb]); };

    switch (in.op) {
    // ---------------------------------------------------------------- loads, stores, permute control
    case Op::LVX:
    case Op::LVXL:
    case Op::LVEBX: // the element loads read the whole quadword (RPCS3 LVEBX/LVEHX/LVEWX)
    case Op::LVEHX:
    case Op::LVEWX: d = read_quad(base, ea() & ~15u); break;
    case Op::STVX:
    case Op::STVXL: write_quad(base, ea() & ~15u, ctx.vr[vd]); write = false; break;
    case Op::STVEBX: {
        const u32 e = ea();
        ps_write8(base, e, ctx.vr[vd].u8[15 - (e & 15)]);
        write = false;
        break;
    }
    case Op::STVEHX: {
        const u32 e = ea() & ~1u;
        ps_write16(base, e, ctx.vr[vd].u16[7 - ((e & 15) >> 1)]);
        write = false;
        break;
    }
    case Op::STVEWX: {
        const u32 e = ea() & ~3u;
        ps_write32(base, e, ctx.vr[vd].u32[3 - ((e & 15) >> 2)]);
        write = false;
        break;
    }
    case Op::LVSL: { // guest byte i = sh + i
        const u32 sh = ea() & 15;
        for (u32 j = 0; j < 16; j++) d.u8[j] = static_cast<u8>(sh + 15 - j);
        break;
    }
    case Op::LVSR: { // guest byte i = 16 - sh + i
        const u32 sh = ea() & 15;
        for (u32 j = 0; j < 16; j++) d.u8[j] = static_cast<u8>(31 - sh - j);
        break;
    }
    case Op::LVLX:
    case Op::LVLXL: { // RPCS3 LVLX: line << 8 * (ea & 15)
        const u32 e = ea();
        d = from128(to128(read_quad(base, e & ~15u)) << (8 * (e & 15)));
        break;
    }
    case Op::LVRX:
    case Op::LVRXL: { // RPCS3 LVRX: line >> 8 * (16 - (ea & 15)), zero when aligned
        const u32 e = ea(), k = e & 15;
        if (k) d = from128(to128(read_quad(base, e & ~15u)) >> (8 * (16 - k)));
        break;
    }
    case Op::STVLX:
    case Op::STVLXL: { // guest bytes 0 .. 15-k to ea .. end of line
        const u32 e = ea(), k = e & 15;
        for (u32 j = 0; j < 16 - k; j++) ps_write8(base, e + j, ctx.vr[vd].u8[15 - j]);
        write = false;
        break;
    }
    case Op::STVRX:
    case Op::STVRXL: { // guest bytes 16-k .. 15 to line start .. ea - 1
        const u32 e = ea(), k = e & 15;
        for (u32 i = 16 - k; i < 16; i++) ps_write8(base, e - 16 + i, ctx.vr[vd].u8[15 - i]);
        write = false;
        break;
    }

    // ---------------------------------------------------------------- VSCR
    case Op::MFVSCR: d.u32[0] = u32{ctx.vscr_sat} | (u32{ctx.vscr_nj} << 16); break;
    case Op::MTVSCR:
        ctx.vscr_sat = static_cast<u8>(b.u32[0] & 1);
        ctx.vscr_nj = static_cast<u8>((b.u32[0] >> 16) & 1);
        write = false;
        break;

    // ---------------------------------------------------------------- logical, select, permute
    case Op::VAND: for (u32 i = 0; i < 2; i++) d.u64[i] = a.u64[i] & b.u64[i]; break;
    case Op::VANDC: for (u32 i = 0; i < 2; i++) d.u64[i] = a.u64[i] & ~b.u64[i]; break;
    case Op::VOR: for (u32 i = 0; i < 2; i++) d.u64[i] = a.u64[i] | b.u64[i]; break;
    case Op::VXOR: for (u32 i = 0; i < 2; i++) d.u64[i] = a.u64[i] ^ b.u64[i]; break;
    case Op::VNOR: for (u32 i = 0; i < 2; i++) d.u64[i] = ~(a.u64[i] | b.u64[i]); break;
    case Op::VSEL: for (u32 i = 0; i < 2; i++) d.u64[i] = (b.u64[i] & c.u64[i]) | (a.u64[i] & ~c.u64[i]); break;
    case Op::VPERM: // guest index g = c & 31 picks (vA || vB)[g]; host index 31 - g into (vB, vA)
        for (u32 j = 0; j < 16; j++) {
            const u32 idx = ~c.u8[j] & 0x1fu;
            d.u8[j] = idx < 16 ? b.u8[idx] : a.u8[idx - 16];
        }
        break;

    // ---------------------------------------------------------------- integer add/sub
    case Op::VADDUBM: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(a.u8[i] + b.u8[i]); break;
    case Op::VADDUHM: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(a.u16[i] + b.u16[i]); break;
    case Op::VADDUWM: for (u32 i = 0; i < 4; i++) d.u32[i] = a.u32[i] + b.u32[i]; break;
    case Op::VSUBUBM: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(a.u8[i] - b.u8[i]); break;
    case Op::VSUBUHM: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(a.u16[i] - b.u16[i]); break;
    case Op::VSUBUWM: for (u32 i = 0; i < 4; i++) d.u32[i] = a.u32[i] - b.u32[i]; break;
    case Op::VADDUBS: for (u32 i = 0; i < 16; i++) d.u8[i] = saturate<u8>(s64{a.u8[i]} + b.u8[i], sat); break;
    case Op::VADDUHS: for (u32 i = 0; i < 8; i++) d.u16[i] = saturate<u16>(s64{a.u16[i]} + b.u16[i], sat); break;
    case Op::VADDUWS: for (u32 i = 0; i < 4; i++) d.u32[i] = saturate<u32>(s64{a.u32[i]} + b.u32[i], sat); break;
    case Op::VADDSBS: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(saturate<s8>(s64{s8_(a.u8[i])} + s8_(b.u8[i]), sat)); break;
    case Op::VADDSHS: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(saturate<s16>(s64{s16_(a.u16[i])} + s16_(b.u16[i]), sat)); break;
    case Op::VADDSWS: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(saturate<s32>(s64{s32_(a.u32[i])} + s32_(b.u32[i]), sat)); break;
    case Op::VSUBUBS: for (u32 i = 0; i < 16; i++) d.u8[i] = saturate<u8>(s64{a.u8[i]} - b.u8[i], sat); break;
    case Op::VSUBUHS: for (u32 i = 0; i < 8; i++) d.u16[i] = saturate<u16>(s64{a.u16[i]} - b.u16[i], sat); break;
    case Op::VSUBUWS: for (u32 i = 0; i < 4; i++) d.u32[i] = saturate<u32>(s64{a.u32[i]} - b.u32[i], sat); break;
    case Op::VSUBSBS: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(saturate<s8>(s64{s8_(a.u8[i])} - s8_(b.u8[i]), sat)); break;
    case Op::VSUBSHS: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(saturate<s16>(s64{s16_(a.u16[i])} - s16_(b.u16[i]), sat)); break;
    case Op::VSUBSWS: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(saturate<s32>(s64{s32_(a.u32[i])} - s32_(b.u32[i]), sat)); break;
    case Op::VADDCUW: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>((u64{a.u32[i]} + b.u32[i]) >> 32); break;
    case Op::VSUBCUW: for (u32 i = 0; i < 4; i++) d.u32[i] = a.u32[i] >= b.u32[i] ? 1 : 0; break;

    // ---------------------------------------------------------------- max, min, average
    case Op::VMAXUB: for (u32 i = 0; i < 16; i++) d.u8[i] = std::max(a.u8[i], b.u8[i]); break;
    case Op::VMAXUH: for (u32 i = 0; i < 8; i++) d.u16[i] = std::max(a.u16[i], b.u16[i]); break;
    case Op::VMAXUW: for (u32 i = 0; i < 4; i++) d.u32[i] = std::max(a.u32[i], b.u32[i]); break;
    case Op::VMAXSB: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(std::max(s8_(a.u8[i]), s8_(b.u8[i]))); break;
    case Op::VMAXSH: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(std::max(s16_(a.u16[i]), s16_(b.u16[i]))); break;
    case Op::VMAXSW: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(std::max(s32_(a.u32[i]), s32_(b.u32[i]))); break;
    case Op::VMINUB: for (u32 i = 0; i < 16; i++) d.u8[i] = std::min(a.u8[i], b.u8[i]); break;
    case Op::VMINUH: for (u32 i = 0; i < 8; i++) d.u16[i] = std::min(a.u16[i], b.u16[i]); break;
    case Op::VMINUW: for (u32 i = 0; i < 4; i++) d.u32[i] = std::min(a.u32[i], b.u32[i]); break;
    case Op::VMINSB: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(std::min(s8_(a.u8[i]), s8_(b.u8[i]))); break;
    case Op::VMINSH: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(std::min(s16_(a.u16[i]), s16_(b.u16[i]))); break;
    case Op::VMINSW: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(std::min(s32_(a.u32[i]), s32_(b.u32[i]))); break;
    case Op::VAVGUB: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>((u32{a.u8[i]} + b.u8[i] + 1) >> 1); break;
    case Op::VAVGUH: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>((u32{a.u16[i]} + b.u16[i] + 1) >> 1); break;
    case Op::VAVGUW: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>((u64{a.u32[i]} + b.u32[i] + 1) >> 1); break;
    case Op::VAVGSB: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>((s32{s8_(a.u8[i])} + s8_(b.u8[i]) + 1) >> 1); break;
    case Op::VAVGSH: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>((s32{s16_(a.u16[i])} + s16_(b.u16[i]) + 1) >> 1); break;
    case Op::VAVGSW: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>((s64{s32_(a.u32[i])} + s32_(b.u32[i]) + 1) >> 1); break;

    // ---------------------------------------------------------------- compares (Rc = 1: CR6 = all true, 0, all false, 0)
    case Op::VCMPEQUB: for (u32 i = 0; i < 16; i++) d.u8[i] = a.u8[i] == b.u8[i] ? 0xff : 0; break;
    case Op::VCMPEQUH: for (u32 i = 0; i < 8; i++) d.u16[i] = a.u16[i] == b.u16[i] ? 0xffff : 0; break;
    case Op::VCMPEQUW: for (u32 i = 0; i < 4; i++) d.u32[i] = a.u32[i] == b.u32[i] ? ~0u : 0; break;
    case Op::VCMPGTUB: for (u32 i = 0; i < 16; i++) d.u8[i] = a.u8[i] > b.u8[i] ? 0xff : 0; break;
    case Op::VCMPGTUH: for (u32 i = 0; i < 8; i++) d.u16[i] = a.u16[i] > b.u16[i] ? 0xffff : 0; break;
    case Op::VCMPGTUW: for (u32 i = 0; i < 4; i++) d.u32[i] = a.u32[i] > b.u32[i] ? ~0u : 0; break;
    case Op::VCMPGTSB: for (u32 i = 0; i < 16; i++) d.u8[i] = s8_(a.u8[i]) > s8_(b.u8[i]) ? 0xff : 0; break;
    case Op::VCMPGTSH: for (u32 i = 0; i < 8; i++) d.u16[i] = s16_(a.u16[i]) > s16_(b.u16[i]) ? 0xffff : 0; break;
    case Op::VCMPGTSW: for (u32 i = 0; i < 4; i++) d.u32[i] = s32_(a.u32[i]) > s32_(b.u32[i]) ? ~0u : 0; break;
    case Op::VCMPEQFP: for (u32 i = 0; i < 4; i++) d.u32[i] = fl(a.u32[i]) == fl(b.u32[i]) ? ~0u : 0; break;
    case Op::VCMPGEFP: for (u32 i = 0; i < 4; i++) d.u32[i] = fl(a.u32[i]) >= fl(b.u32[i]) ? ~0u : 0; break;
    case Op::VCMPGTFP: for (u32 i = 0; i < 4; i++) d.u32[i] = fl(a.u32[i]) > fl(b.u32[i]) ? ~0u : 0; break;
    case Op::VCMPBFP: // bit 0 (guest) = a > b or unordered, bit 1 = a < -b or unordered
        for (u32 i = 0; i < 4; i++) {
            const float x = fl(a.u32[i]), y = fl(b.u32[i]);
            d.u32[i] = (!(x <= y) ? 0x80000000u : 0) | (!(x >= -y) ? 0x40000000u : 0);
        }
        break;

    // ---------------------------------------------------------------- shifts and rotates (count per element)
    case Op::VSLB: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(a.u8[i] << (b.u8[i] & 7)); break;
    case Op::VSLH: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(a.u16[i] << (b.u16[i] & 15)); break;
    case Op::VSLW: for (u32 i = 0; i < 4; i++) d.u32[i] = a.u32[i] << (b.u32[i] & 31); break;
    case Op::VSRB: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(a.u8[i] >> (b.u8[i] & 7)); break;
    case Op::VSRH: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(a.u16[i] >> (b.u16[i] & 15)); break;
    case Op::VSRW: for (u32 i = 0; i < 4; i++) d.u32[i] = a.u32[i] >> (b.u32[i] & 31); break;
    case Op::VSRAB: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(s8_(a.u8[i]) >> (b.u8[i] & 7)); break;
    case Op::VSRAH: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(s16_(a.u16[i]) >> (b.u16[i] & 15)); break;
    case Op::VSRAW: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(s32_(a.u32[i]) >> (b.u32[i] & 31)); break;
    case Op::VRLB: for (u32 i = 0; i < 16; i++) d.u8[i] = std::rotl(a.u8[i], b.u8[i] & 7); break;
    case Op::VRLH: for (u32 i = 0; i < 8; i++) d.u16[i] = std::rotl(a.u16[i], b.u16[i] & 15); break;
    case Op::VRLW: for (u32 i = 0; i < 4; i++) d.u32[i] = std::rotl(a.u32[i], static_cast<int>(b.u32[i] & 31)); break;
    case Op::VSL: // RPCS3 gv_fshl8(a, a << 8 bits, b): each byte takes its own count
        for (u32 i = 0; i < 16; i++) {
            const u32 n = b.u8[i] & 7, lower = i ? a.u8[i - 1] : 0;
            d.u8[i] = static_cast<u8>((a.u8[i] << n) | (n ? lower >> (8 - n) : 0));
        }
        break;
    case Op::VSR: // RPCS3 gv_fshr8(a >> 8 bits, a, b)
        for (u32 i = 0; i < 16; i++) {
            const u32 n = b.u8[i] & 7, upper = i < 15 ? a.u8[i + 1] : 0;
            d.u8[i] = static_cast<u8>((a.u8[i] >> n) | (n ? upper << (8 - n) : 0));
        }
        break;
    case Op::VSLO: d = from128(to128(a) << (b.u8[0] & 0x78)); break; // RPCS3 VSLO: shift by octets
    case Op::VSRO: d = from128(to128(a) >> (b.u8[0] & 0x78)); break;
    case Op::VSLDOI: { // bytes sh .. sh+15 of vA || vB
        const u32 sh = in.vsh();
        d = sh ? from128((to128(a) << (8 * sh)) | (to128(b) >> (128 - 8 * sh))) : a;
        break;
    }

    // ---------------------------------------------------------------- splats (guest element i = host lane N-1-i)
    case Op::VSPLTB: for (u32 i = 0; i < 16; i++) d.u8[i] = b.u8[15 - (in.vuimm() & 15)]; break;
    case Op::VSPLTH: for (u32 i = 0; i < 8; i++) d.u16[i] = b.u16[7 - (in.vuimm() & 7)]; break;
    case Op::VSPLTW: for (u32 i = 0; i < 4; i++) d.u32[i] = b.u32[3 - (in.vuimm() & 3)]; break;
    case Op::VSPLTISB: for (u32 i = 0; i < 16; i++) d.u8[i] = static_cast<u8>(in.vsimm()); break;
    case Op::VSPLTISH: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(in.vsimm()); break;
    case Op::VSPLTISW: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(in.vsimm()); break;

    // ---------------------------------------------------------------- merges (high = host-high halves, vA first)
    case Op::VMRGHB: for (u32 i = 0; i < 8; i++) { d.u8[2 * i] = b.u8[8 + i]; d.u8[2 * i + 1] = a.u8[8 + i]; } break;
    case Op::VMRGHH: for (u32 i = 0; i < 4; i++) { d.u16[2 * i] = b.u16[4 + i]; d.u16[2 * i + 1] = a.u16[4 + i]; } break;
    case Op::VMRGHW: for (u32 i = 0; i < 2; i++) { d.u32[2 * i] = b.u32[2 + i]; d.u32[2 * i + 1] = a.u32[2 + i]; } break;
    case Op::VMRGLB: for (u32 i = 0; i < 8; i++) { d.u8[2 * i] = b.u8[i]; d.u8[2 * i + 1] = a.u8[i]; } break;
    case Op::VMRGLH: for (u32 i = 0; i < 4; i++) { d.u16[2 * i] = b.u16[i]; d.u16[2 * i + 1] = a.u16[i]; } break;
    case Op::VMRGLW: for (u32 i = 0; i < 2; i++) { d.u32[2 * i] = b.u32[i]; d.u32[2 * i + 1] = a.u32[i]; } break;

    // ---------------------------------------------------------------- packs (vA -> guest-first = host-high half)
    case Op::VPKUHUM: for (u32 i = 0; i < 8; i++) { d.u8[i] = static_cast<u8>(b.u16[i]); d.u8[8 + i] = static_cast<u8>(a.u16[i]); } break;
    case Op::VPKUWUM: for (u32 i = 0; i < 4; i++) { d.u16[i] = static_cast<u16>(b.u32[i]); d.u16[4 + i] = static_cast<u16>(a.u32[i]); } break;
    case Op::VPKUHUS:
        for (u32 i = 0; i < 8; i++) {
            d.u8[i] = saturate<u8>(b.u16[i], sat);
            d.u8[8 + i] = saturate<u8>(a.u16[i], sat);
        }
        break;
    case Op::VPKUWUS:
        for (u32 i = 0; i < 4; i++) {
            d.u16[i] = saturate<u16>(b.u32[i], sat);
            d.u16[4 + i] = saturate<u16>(a.u32[i], sat);
        }
        break;
    case Op::VPKSHSS:
        for (u32 i = 0; i < 8; i++) {
            d.u8[i] = static_cast<u8>(saturate<s8>(s16_(b.u16[i]), sat));
            d.u8[8 + i] = static_cast<u8>(saturate<s8>(s16_(a.u16[i]), sat));
        }
        break;
    case Op::VPKSHUS:
        for (u32 i = 0; i < 8; i++) {
            d.u8[i] = saturate<u8>(s16_(b.u16[i]), sat);
            d.u8[8 + i] = saturate<u8>(s16_(a.u16[i]), sat);
        }
        break;
    case Op::VPKSWSS:
        for (u32 i = 0; i < 4; i++) {
            d.u16[i] = static_cast<u16>(saturate<s16>(s32_(b.u32[i]), sat));
            d.u16[4 + i] = static_cast<u16>(saturate<s16>(s32_(a.u32[i]), sat));
        }
        break;
    case Op::VPKSWUS:
        for (u32 i = 0; i < 4; i++) {
            d.u16[i] = saturate<u16>(s32_(b.u32[i]), sat);
            d.u16[4 + i] = saturate<u16>(s32_(a.u32[i]), sat);
        }
        break;
    case Op::VPKPX: for (u32 i = 0; i < 4; i++) { d.u16[i] = pack_pixel(b.u32[i]); d.u16[4 + i] = pack_pixel(a.u32[i]); } break;

    // ---------------------------------------------------------------- unpacks (high = host-high half)
    case Op::VUPKHSB: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(s8_(b.u8[8 + i])); break;
    case Op::VUPKLSB: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(s8_(b.u8[i])); break;
    case Op::VUPKHSH: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(s16_(b.u16[4 + i])); break;
    case Op::VUPKLSH: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(s16_(b.u16[i])); break;
    case Op::VUPKHPX: for (u32 i = 0; i < 4; i++) d.u32[i] = unpack_pixel(b.u16[4 + i]); break;
    case Op::VUPKLPX: for (u32 i = 0; i < 4; i++) d.u32[i] = unpack_pixel(b.u16[i]); break;

    // ---------------------------------------------------------------- multiplies (guest even = host odd lanes)
    case Op::VMULEUB: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(u32{a.u8[2 * i + 1]} * b.u8[2 * i + 1]); break;
    case Op::VMULOUB: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(u32{a.u8[2 * i]} * b.u8[2 * i]); break;
    case Op::VMULESB: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(s32{s8_(a.u8[2 * i + 1])} * s8_(b.u8[2 * i + 1])); break;
    case Op::VMULOSB: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(s32{s8_(a.u8[2 * i])} * s8_(b.u8[2 * i])); break;
    case Op::VMULEUH: for (u32 i = 0; i < 4; i++) d.u32[i] = u32{a.u16[2 * i + 1]} * b.u16[2 * i + 1]; break;
    case Op::VMULOUH: for (u32 i = 0; i < 4; i++) d.u32[i] = u32{a.u16[2 * i]} * b.u16[2 * i]; break;
    case Op::VMULESH: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(s32{s16_(a.u16[2 * i + 1])} * s16_(b.u16[2 * i + 1])); break;
    case Op::VMULOSH: for (u32 i = 0; i < 4; i++) d.u32[i] = static_cast<u32>(s32{s16_(a.u16[2 * i])} * s16_(b.u16[2 * i])); break;
    case Op::VMLADDUHM: for (u32 i = 0; i < 8; i++) d.u16[i] = static_cast<u16>(u32{a.u16[i]} * b.u16[i] + c.u16[i]); break;
    case Op::VMHADDSHS: // sat16(((a * b) >> 15) + c)
        for (u32 i = 0; i < 8; i++) {
            const s32 p = (s32{s16_(a.u16[i])} * s16_(b.u16[i])) >> 15;
            d.u16[i] = static_cast<u16>(saturate<s16>(s64{p} + s16_(c.u16[i]), sat));
        }
        break;
    case Op::VMHRADDSHS: // sat16(((a * b + 0x4000) >> 15) + c)
        for (u32 i = 0; i < 8; i++) {
            const s32 p = (s32{s16_(a.u16[i])} * s16_(b.u16[i]) + 0x4000) >> 15;
            d.u16[i] = static_cast<u16>(saturate<s16>(s64{p} + s16_(c.u16[i]), sat));
        }
        break;

    // ---------------------------------------------------------------- multiply-sums (word lane i = bytes 4i..4i+3)
    case Op::VMSUMUBM:
        for (u32 i = 0; i < 4; i++) {
            u32 s = c.u32[i];
            for (u32 k = 0; k < 4; k++) s += u32{a.u8[4 * i + k]} * b.u8[4 * i + k];
            d.u32[i] = s;
        }
        break;
    case Op::VMSUMMBM: // signed bytes of vA, unsigned bytes of vB
        for (u32 i = 0; i < 4; i++) {
            s64 s = s32_(c.u32[i]);
            for (u32 k = 0; k < 4; k++) s += s32{s8_(a.u8[4 * i + k])} * s32{b.u8[4 * i + k]};
            d.u32[i] = static_cast<u32>(s);
        }
        break;
    case Op::VMSUMUHM:
        for (u32 i = 0; i < 4; i++) d.u32[i] = c.u32[i] + u32{a.u16[2 * i]} * b.u16[2 * i] + u32{a.u16[2 * i + 1]} * b.u16[2 * i + 1];
        break;
    case Op::VMSUMUHS:
        for (u32 i = 0; i < 4; i++) {
            const u64 s = u64{c.u32[i]} + u64{a.u16[2 * i]} * b.u16[2 * i] + u64{a.u16[2 * i + 1]} * b.u16[2 * i + 1];
            d.u32[i] = saturate<u32>(static_cast<s64>(s), sat);
        }
        break;
    case Op::VMSUMSHM:
        for (u32 i = 0; i < 4; i++) {
            const s64 s = s64{s32_(c.u32[i])} + s32{s16_(a.u16[2 * i])} * s16_(b.u16[2 * i]) + s32{s16_(a.u16[2 * i + 1])} * s16_(b.u16[2 * i + 1]);
            d.u32[i] = static_cast<u32>(s);
        }
        break;
    case Op::VMSUMSHS:
        for (u32 i = 0; i < 4; i++) {
            const s64 s = s64{s32_(c.u32[i])} + s32{s16_(a.u16[2 * i])} * s16_(b.u16[2 * i]) + s32{s16_(a.u16[2 * i + 1])} * s16_(b.u16[2 * i + 1]);
            d.u32[i] = static_cast<u32>(saturate<s32>(s, sat));
        }
        break;

    // ---------------------------------------------------------------- sums across
    case Op::VSUMSWS: { // guest word 3 (host lane 0) = sat32(b word 3 + every word of a)
        s64 s = s32_(b.u32[0]);
        for (u32 i = 0; i < 4; i++) s += s32_(a.u32[i]);
        d.u32[0] = static_cast<u32>(saturate<s32>(s, sat));
        break;
    }
    case Op::VSUM2SWS: // guest words 1 and 3 (host lanes 2 and 0)
        for (u32 j = 0; j < 4; j += 2) {
            const s64 s = s64{s32_(a.u32[j])} + s32_(a.u32[j + 1]) + s32_(b.u32[j]);
            d.u32[j] = static_cast<u32>(saturate<s32>(s, sat));
        }
        break;
    case Op::VSUM4SBS:
        for (u32 i = 0; i < 4; i++) {
            s64 s = s32_(b.u32[i]);
            for (u32 k = 0; k < 4; k++) s += s8_(a.u8[4 * i + k]);
            d.u32[i] = static_cast<u32>(saturate<s32>(s, sat));
        }
        break;
    case Op::VSUM4SHS:
        for (u32 i = 0; i < 4; i++) {
            const s64 s = s64{s32_(b.u32[i])} + s16_(a.u16[2 * i]) + s16_(a.u16[2 * i + 1]);
            d.u32[i] = static_cast<u32>(saturate<s32>(s, sat));
        }
        break;
    case Op::VSUM4UBS:
        for (u32 i = 0; i < 4; i++) {
            s64 s = b.u32[i];
            for (u32 k = 0; k < 4; k++) s += a.u8[4 * i + k];
            d.u32[i] = saturate<u32>(s, sat);
        }
        break;

    // ---------------------------------------------------------------- floating point (NJ, NaN rules: vmx.h)
    case Op::VADDFP:
    case Op::VSUBFP:
        for (u32 i = 0; i < 4; i++) {
            const u32 x = flush(a.u32[i]), y = flush(b.u32[i]);
            const float r = in.op == Op::VADDFP ? fl(x) + fl(y) : fl(x) - fl(y);
            d.u32[i] = flush(vnan(bits(r), x, y));
        }
        break;
    case Op::VMAXFP: // inputs are not flushed (RPCS3); max(+0, -0) = +0
        for (u32 i = 0; i < 4; i++) {
            const u32 x = a.u32[i], y = b.u32[i];
            const u32 r = fl(x) > fl(y) ? x : fl(y) > fl(x) ? y : (x & y);
            d.u32[i] = flush(vnan(r, x, y));
        }
        break;
    case Op::VMINFP: // min(+0, -0) = -0
        for (u32 i = 0; i < 4; i++) {
            const u32 x = a.u32[i], y = b.u32[i];
            const u32 r = fl(x) < fl(y) ? x : fl(y) < fl(x) ? y : (x | y);
            d.u32[i] = flush(vnan(r, x, y));
        }
        break;
    case Op::VMADDFP: // vA * vC + vB, one rounding
        for (u32 i = 0; i < 4; i++) {
            const u32 x = flush(a.u32[i]), y = flush(b.u32[i]), z = flush(c.u32[i]);
            d.u32[i] = flush(vnan(bits(std::fma(fl(x), fl(z), fl(y)))));
        }
        break;
    case Op::VNMSUBFP: // -(vA * vC - vB), one rounding
        for (u32 i = 0; i < 4; i++) {
            const u32 x = flush(a.u32[i]), y = flush(b.u32[i]), z = flush(c.u32[i]);
            d.u32[i] = flush(vnan(bits(-std::fma(fl(x), fl(z), -fl(y)))));
        }
        break;
    case Op::VREFP:
        for (u32 i = 0; i < 4; i++) {
            const u32 x = flush(b.u32[i]);
            d.u32[i] = flush(vnan(bits(1.0f / fl(x)), x));
        }
        break;
    case Op::VRSQRTEFP:
        for (u32 i = 0; i < 4; i++) {
            const u32 x = flush(b.u32[i]);
            d.u32[i] = flush(vnan(bits(1.0f / std::sqrt(fl(x))), x));
        }
        break;
    case Op::VRFIM:
        for (u32 i = 0; i < 4; i++) {
            const u32 x = flush(b.u32[i]);
            d.u32[i] = flush(vnan(bits(std::floor(fl(x))), x));
        }
        break;
    case Op::VRFIP:
        for (u32 i = 0; i < 4; i++) {
            const u32 x = flush(b.u32[i]);
            d.u32[i] = flush(vnan(bits(std::ceil(fl(x))), x));
        }
        break;
    case Op::VRFIN: for (u32 i = 0; i < 4; i++) d.u32[i] = flush(vnan(roundeven_bits(b.u32[i]), b.u32[i])); break;
    case Op::VRFIZ: for (u32 i = 0; i < 4; i++) d.u32[i] = flush(vnan(bits(std::trunc(fl(b.u32[i]))), b.u32[i])); break;
    case Op::VEXPTEFP: for (u32 i = 0; i < 4; i++) d.u32[i] = vnan(bits(std::exp2(fl(b.u32[i])))); break;
    case Op::VLOGEFP: for (u32 i = 0; i < 4; i++) d.u32[i] = vnan(bits(std::log2(fl(b.u32[i])))); break;
    case Op::VCFSX: {
        const float scale = 1.0f / static_cast<float>(1u << in.vuimm());
        for (u32 i = 0; i < 4; i++) d.u32[i] = bits(static_cast<float>(s32_(b.u32[i])) * scale);
        break;
    }
    case Op::VCFUX: {
        const float scale = 1.0f / static_cast<float>(1u << in.vuimm());
        for (u32 i = 0; i < 4; i++) d.u32[i] = bits(static_cast<float>(b.u32[i]) * scale);
        break;
    }
    case Op::VCTSXS: { // b * 2^uimm, truncated, saturated; NaN -> 0 without SAT (RPCS3 VCTSXS)
        const float scale = fl((in.vuimm() + 127u) << 23);
        for (u32 i = 0; i < 4; i++) {
            const float r = fl(b.u32[i]) * scale;
            if (std::isnan(r)) {
                d.u32[i] = 0;
            } else if (r < -2147483648.0f) {
                d.u32[i] = 0x80000000u;
                sat = true;
            } else if (r >= 2147483648.0f) {
                d.u32[i] = 0x7fffffffu;
                sat = true;
            } else {
                d.u32[i] = static_cast<u32>(static_cast<s32>(r));
            }
        }
        break;
    }
    case Op::VCTUXS: { // negative values (not -0) saturate to 0 with SAT
        const float scale = fl((in.vuimm() + 127u) << 23);
        for (u32 i = 0; i < 4; i++) {
            const float r = fl(b.u32[i]) * scale;
            if (std::isnan(r)) {
                d.u32[i] = 0;
            } else if (r < 0.0f) {
                d.u32[i] = 0;
                sat = true;
            } else if (r >= 4294967296.0f) {
                d.u32[i] = 0xffffffffu;
                sat = true;
            } else {
                d.u32[i] = static_cast<u32>(r);
            }
        }
        break;
    }

    default: PS_FATAL("ppu", "execute_vmx: {} is not a VMX instruction", in.info().mnemonic);
    }

    if (in.info().fmt == Fmt::VXR_VD_VA_VB && in.vrc()) {
        bool all = true, none = true;
        for (u32 i = 0; i < 4; i++) {
            all = all && d.u32[i] == ~0u;
            none = none && d.u32[i] == 0;
        }
        u8* cr6 = &ctx.cr[24];
        cr6[PS_CR_LT] = in.op == Op::VCMPBFP ? 0 : all;
        cr6[PS_CR_GT] = 0;
        cr6[PS_CR_EQ] = none;
        cr6[PS_CR_SO] = 0;
    }
    if (sat) ctx.vscr_sat = 1;
    if (write) ctx.vr[vd] = d;
}

} // namespace pspocket::ppu
