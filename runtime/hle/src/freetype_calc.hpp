#pragma once

// FreeType 2.2.1's fixed-point and trigonometric code (src/base/ftcalc.c with FT_LONG64, src/base/fttrigon.c) as the
// PPU computes it in Sony's libfreetype: 32-bit FT_Long, FT_Fixed and FT_Angle, negation wrapping at 32 bits, 64-bit
// products (Sony's code: mulld, srdi, divd). Used by cell_FreeType2's exports and by the outline functions that call
// them (FT_Outline_Embolden, FT_Outline_Get_Orientation).

#include "pspocket/core/types.hpp"

namespace pspocket::hle::ftcalc {

// ------------------------------------------------------------------------------------------------- fixed point
// src/base/ftcalc.c with FT_LONG64, compiled as the PPU does: 32-bit FT_Long, negation wrapping at 32 bits, 64-bit
// products (Sony's code: mulld, srdi, divd).

inline s32 neg32(s32 a) { return static_cast<s32>(0u - static_cast<u32>(a)); }

inline s32 mulfix32(s32 a, s32 b)
{
    s32 s = 1;
    if (a < 0) {
        a = neg32(a);
        s = -1;
    }
    if (b < 0) {
        b = neg32(b);
        s = -s;
    }
    const u64 c = static_cast<u64>(static_cast<s64>(a) * b + 0x8000) >> 16;
    return s > 0 ? static_cast<s32>(c) : neg32(static_cast<s32>(c));
}

inline s32 muldiv32(s32 a, s32 b, s32 c)
{
    s32 s = 1;
    if (a < 0) {
        a = neg32(a);
        s = -1;
    }
    if (b < 0) {
        b = neg32(b);
        s = -s;
    }
    if (c < 0) {
        c = neg32(c);
        s = -s;
    }
    const s64 d = c > 0 ? (static_cast<s64>(a) * b + (c >> 1)) / c : 0x7FFFFFFF;
    return s > 0 ? static_cast<s32>(d) : neg32(static_cast<s32>(d));
}

inline s32 divfix32(s32 a, s32 b)
{
    s32 s = 1;
    if (a < 0) {
        a = neg32(a);
        s = -1;
    }
    if (b < 0) {
        b = neg32(b);
        s = -s;
    }
    s64 q = 0x7FFFFFFF;
    if (b != 0) q = (static_cast<s64>(a) * 65536 + (b >> 1)) / b;
    return s < 0 ? neg32(static_cast<s32>(q)) : static_cast<s32>(q);
}

// ------------------------------------------------------------------------------------------------- trigonometry
// src/base/fttrigon.c (CORDIC) with 32-bit FT_Fixed and FT_Angle.

constexpr s32 kTrigCoscale = 0x11616E8E;
constexpr u32 kTrigScale = 1166391785u;
constexpr s32 kAnglePi = 180 << 16, kAngle2Pi = 360 << 16, kAnglePi2 = 90 << 16;
constexpr s32 kArctan[24] = {4157273, 2949120, 1740967, 919879, 466945, 234379, 117304, 58666, 29335, 14668, 7334,
    3667, 1833, 917, 458, 229, 115, 57, 29, 14, 7, 4, 2, 1};

inline s32 add32(s32 a, s32 b) { return static_cast<s32>(static_cast<u32>(a) + static_cast<u32>(b)); }
inline s32 sub32(s32 a, s32 b) { return static_cast<s32>(static_cast<u32>(a) - static_cast<u32>(b)); }
inline s32 shl32(s32 a, s32 n) { return static_cast<s32>(static_cast<u32>(a) << n); }

struct Vec {
    s32 x, y;
};

// ft_trig_downscale without FT_CONFIG_HAS_INT64: Sony's build multiplies in 16-bit halves here (its ftcalc.c has
// 64-bit products, its fttrigon.c not).
inline s32 trig_downscale(s32 val)
{
    const s32 s = val;
    if (val < 0) val = neg32(val);
    const u32 v1 = static_cast<u32>(val) >> 16, v2 = static_cast<u32>(val) & 0xFFFF;
    const u32 k1 = kTrigScale >> 16, k2 = kTrigScale & 0xFFFF;
    u32 hi = k1 * v1;
    u32 lo1 = k1 * v2 + k2 * v1;
    const u32 lo2 = (k2 * v2) >> 16;
    const u32 lo3 = lo1 >= lo2 ? lo1 : lo2;
    lo1 += lo2;
    hi += lo1 >> 16;
    if (lo1 < lo3) hi += 0x10000u;
    val = static_cast<s32>(hi);
    return s >= 0 ? val : neg32(val);
}

inline s32 trig_prenorm(Vec& vec)
{
    const s32 x = vec.x, y = vec.y;
    s32 z = (x >= 0 ? x : neg32(x)) | (y >= 0 ? y : neg32(y));
    s32 shift = 0;
    if (z >= (1 << 16)) {
        z >>= 16;
        shift += 16;
    }
    if (z >= (1 << 8)) {
        z >>= 8;
        shift += 8;
    }
    if (z >= (1 << 4)) {
        z >>= 4;
        shift += 4;
    }
    if (z >= (1 << 2)) {
        z >>= 2;
        shift += 2;
    }
    if (z >= (1 << 1)) shift += 1;
    if (shift <= 27) {
        shift = 27 - shift;
        vec.x = shl32(x, shift);
        vec.y = shl32(y, shift);
    } else {
        shift -= 27;
        vec.x = x >> shift;
        vec.y = y >> shift;
        shift = -shift;
    }
    return shift;
}

inline void trig_pseudo_rotate(Vec& vec, s32 theta)
{
    s32 x = vec.x, y = vec.y;
    while (theta <= -kAnglePi2) {
        x = neg32(x);
        y = neg32(y);
        theta = add32(theta, kAnglePi);
    }
    while (theta > kAnglePi2) {
        x = neg32(x);
        y = neg32(y);
        theta = sub32(theta, kAnglePi);
    }
    const s32* arctan = kArctan;
    s32 t;
    if (theta < 0) {
        t = add32(x, shl32(y, 1));
        y = sub32(y, shl32(x, 1));
        x = t;
        theta = add32(theta, *arctan++);
    } else {
        t = sub32(x, shl32(y, 1));
        y = add32(y, shl32(x, 1));
        x = t;
        theta = sub32(theta, *arctan++);
    }
    s32 i = 0;
    do {
        if (theta < 0) {
            t = add32(x, y >> i);
            y = sub32(y, x >> i);
            x = t;
            theta = add32(theta, *arctan++);
        } else {
            t = sub32(x, y >> i);
            y = add32(y, x >> i);
            x = t;
            theta = sub32(theta, *arctan++);
        }
    } while (++i < 23);
    vec.x = x;
    vec.y = y;
}

inline void trig_pseudo_polarize(Vec& vec)
{
    s32 x = vec.x, y = vec.y, theta = 0;
    if (x < 0) {
        x = neg32(x);
        y = neg32(y);
        theta = 2 * kAnglePi2;
    }
    if (y > 0) theta = neg32(theta);
    const s32* arctan = kArctan;
    s32 yi;
    if (y < 0) {
        yi = add32(y, shl32(x, 1));
        x = sub32(x, shl32(y, 1));
        y = yi;
        theta = sub32(theta, *arctan++);
    } else {
        yi = sub32(y, shl32(x, 1));
        x = add32(x, shl32(y, 1));
        y = yi;
        theta = add32(theta, *arctan++);
    }
    s32 i = 0;
    do {
        if (y < 0) {
            yi = add32(y, x >> i);
            x = sub32(x, y >> i);
            y = yi;
            theta = sub32(theta, *arctan++);
        } else {
            yi = sub32(y, x >> i);
            x = add32(x, y >> i);
            y = yi;
            theta = add32(theta, *arctan++);
        }
    } while (++i < 23);
    // FT_PAD_ROUND(theta, 32)
    if (theta >= 0) theta = static_cast<s32>((static_cast<u32>(theta) + 16u) & ~31u);
    else theta = neg32(static_cast<s32>((static_cast<u32>(neg32(theta)) + 16u) & ~31u));
    vec.x = x;
    vec.y = theta;
}

inline s32 cos32(s32 angle)
{
    Vec v{kTrigCoscale >> 2, 0};
    trig_pseudo_rotate(v, angle);
    return v.x / (1 << 12);
}

inline void vector_rotate32(Vec& vec, s32 angle)
{
    Vec v = vec;
    if (angle && (v.x != 0 || v.y != 0)) {
        s32 shift = trig_prenorm(v);
        trig_pseudo_rotate(v, angle);
        v.x = trig_downscale(v.x);
        v.y = trig_downscale(v.y);
        if (shift > 0) {
            const s32 half = static_cast<s32>(1u << (shift - 1));
            vec.x = add32(add32(v.x, half), v.x >> 31) >> shift;
            vec.y = add32(add32(v.y, half), v.y >> 31) >> shift;
        } else {
            shift = -shift;
            vec.x = shl32(v.x, shift);
            vec.y = shl32(v.y, shift);
        }
    }
}

inline s32 atan2_32(s32 dx, s32 dy)
{
    if (dx == 0 && dy == 0) return 0;
    Vec v{dx, dy};
    trig_prenorm(v);
    trig_pseudo_polarize(v);
    return v.y;
}

inline s32 angle_diff32(s32 angle1, s32 angle2)
{
    s32 delta = sub32(angle2, angle1);
    delta %= kAngle2Pi;
    if (delta < 0) delta += kAngle2Pi;
    if (delta > kAnglePi) delta -= kAngle2Pi;
    return delta;
}

} // namespace pspocket::hle::ftcalc
