/*
 * The one header lifted (generated) code includes, besides its own generated header.
 *
 * Generated functions have the D-009 signature (amended by D-013: base is not restrict-qualified)
 *     void f_XXXXXXXX(PPUContext* restrict ctx, uint8_t* base)
 * keep guest registers in locals (loaded at entry, spilled around calls, syscalls and fallbacks, and at return),
 * access guest memory only through the volatile ps_read and ps_write functions of pspocket/core/be.h, and call
 * the runtime through pspocket/ppu/runtime.h. Helpers here are small static inline functions; clang folds them
 * into straight-line code.
 */
#ifndef PSPOCKET_PPU_LIFTED_H
#define PSPOCKET_PPU_LIFTED_H

#include <stdint.h>
#include <string.h>

#include "pspocket/core/be.h"
#include "pspocket/ppu/context.h"
#include "pspocket/ppu/fpu.h"
#include "pspocket/ppu/runtime.h"

/* Generated headers are also included from C++ (tests, the self-test probes), which has no `restrict`. */
#ifdef __cplusplus
#define PS_RESTRICT __restrict
#else
#define PS_RESTRICT restrict
#endif

/*
 * Every lifted prologue tells the compiler the guest base is this aligned (D-013): guest address 0 maps to a
 * 64 KB-aligned host address, so aligned guest accesses (16-byte VMX loads at base + (ea & ~15)) compile to
 * aligned host accesses. GuestMemory::reserve() must provide it; hosts that call lifted code check it.
 */
#define PS_PPU_BASE_ALIGN 65536

/* Floating point is evaluated exactly as written: no contraction of a*b+c into fma (matches the interpreter). */
#pragma STDC FP_CONTRACT OFF

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t ps_rotl32(uint32_t v, uint32_t n)
{
    n &= 31;
    return n ? (v << n) | (v >> (32 - n)) : v;
}

static inline uint64_t ps_rotl64(uint64_t v, uint32_t n)
{
    n &= 63;
    return n ? (v << n) | (v >> (64 - n)) : v;
}

/* ROTL32 of the architecture rotates the 32-bit value duplicated into both words. */
static inline uint64_t ps_dup32(uint32_t v) { return (uint64_t)v | ((uint64_t)v << 32); }

/* a + b + cin with carry out (XER[CA]). */
static inline uint64_t ps_add_carry(uint64_t a, uint64_t b, uint8_t cin, uint8_t* cout)
{
    const uint64_t s = a + b;
    const uint64_t r = s + cin;
    *cout = (uint8_t)((s < a) | (r < s));
    return r;
}

/* Signed overflow of a + b + cin, given the result (sign of both inputs equal, sign of the result differs). */
static inline uint8_t ps_add_ov(uint64_t a, uint64_t b, uint64_t r) { return (uint8_t)(((a ^ r) & (b ^ r)) >> 63); }

__extension__ typedef __int128 ps_i128;
__extension__ typedef unsigned __int128 ps_u128;

static inline uint64_t ps_mulhd(uint64_t a, uint64_t b)
{
    return (uint64_t)(((ps_i128)(int64_t)a * (int64_t)b) >> 64);
}

static inline uint64_t ps_mulhdu(uint64_t a, uint64_t b) { return (uint64_t)(((ps_u128)a * b) >> 64); }

static inline uint8_t ps_mulld_ov(uint64_t a, uint64_t b)
{
    int64_t r;
    return (uint8_t)__builtin_mul_overflow((int64_t)a, (int64_t)b, &r);
}

static inline uint64_t ps_cntlzw(uint64_t v)
{
    const uint32_t w = (uint32_t)v;
    return w ? (uint64_t)__builtin_clz(w) : 32;
}

static inline uint64_t ps_cntlzd(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64; }

/* sync / lwsync / eieio: full fence (as RPCS3); isync: acquire. They also stop the compiler moving guest
 * memory accesses across them. */
#define PS_PPU_SYNC() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define PS_PPU_ISYNC() __atomic_thread_fence(__ATOMIC_ACQUIRE)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_PPU_LIFTED_H */
