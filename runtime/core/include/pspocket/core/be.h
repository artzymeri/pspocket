/*
 * Big-endian accessors shared by the runtime (C++20) and generated code (C11).
 *
 * The one rule of the guest memory model (plan section 4): a guest address is 32 bits, and every access
 * is host_base + (uint32_t)addr with an explicit byte swap. ARM64 does the swap in one REV instruction.
 *
 * - ps_ldN / ps_stN work on a host pointer to big-endian data (unaligned is fine). Plain accesses: the compiler
 *   may merge or drop them, which is right for host-side code working on data it owns.
 * - ps_readN / ps_writeN take the guest base and a 32-bit guest address. They are volatile (D-013): guest code polls
 *   memory that other host threads write (SPU completion flags, cellSync primitives, RSX labels, the FIFO get
 *   pointer), so the compiler must not hoist, merge or drop these accesses. Unaligned addresses are allowed, as on
 *   the PPU.
 *
 * 128-bit (VMX/SPU) accessors follow the project lane-order convention, docs/decisions.md D-004.
 */
#ifndef PSPOCKET_CORE_BE_H
#define PSPOCKET_CORE_BE_H

#include <stdint.h>
#include <string.h>

#define PS_ALWAYS_INLINE static inline __attribute__((always_inline))

#ifdef __cplusplus
extern "C" {
#endif

PS_ALWAYS_INLINE uint16_t ps_bswap16(uint16_t v) { return __builtin_bswap16(v); }
PS_ALWAYS_INLINE uint32_t ps_bswap32(uint32_t v) { return __builtin_bswap32(v); }
PS_ALWAYS_INLINE uint64_t ps_bswap64(uint64_t v) { return __builtin_bswap64(v); }

/* ---- host pointer to big-endian data ---- */

PS_ALWAYS_INLINE uint8_t ps_ld8(const void* p) { return *(const uint8_t*)p; }
PS_ALWAYS_INLINE uint16_t ps_ld16(const void* p) { uint16_t v; memcpy(&v, p, 2); return ps_bswap16(v); }
PS_ALWAYS_INLINE uint32_t ps_ld32(const void* p) { uint32_t v; memcpy(&v, p, 4); return ps_bswap32(v); }
PS_ALWAYS_INLINE uint64_t ps_ld64(const void* p) { uint64_t v; memcpy(&v, p, 8); return ps_bswap64(v); }

PS_ALWAYS_INLINE void ps_st8(void* p, uint8_t v) { *(uint8_t*)p = v; }
PS_ALWAYS_INLINE void ps_st16(void* p, uint16_t v) { v = ps_bswap16(v); memcpy(p, &v, 2); }
PS_ALWAYS_INLINE void ps_st32(void* p, uint32_t v) { v = ps_bswap32(v); memcpy(p, &v, 4); }
PS_ALWAYS_INLINE void ps_st64(void* p, uint64_t v) { v = ps_bswap64(v); memcpy(p, &v, 8); }

PS_ALWAYS_INLINE float ps_ldf32(const void* p) { uint32_t u = ps_ld32(p); float f; memcpy(&f, &u, 4); return f; }
PS_ALWAYS_INLINE double ps_ldf64(const void* p) { uint64_t u = ps_ld64(p); double d; memcpy(&d, &u, 8); return d; }
PS_ALWAYS_INLINE void ps_stf32(void* p, float f) { uint32_t u; memcpy(&u, &f, 4); ps_st32(p, u); }
PS_ALWAYS_INLINE void ps_stf64(void* p, double d) { uint64_t u; memcpy(&u, &d, 8); ps_st64(p, u); }

/*
 * 128-bit: full 16-byte reverse (D-004). Host byte j holds guest byte 15-j, so guest element i of an
 * N-element vector lives in host lane N-1-i, already in host (little-endian) byte order.
 */
typedef uint8_t ps_v16u8 __attribute__((vector_size(16)));

PS_ALWAYS_INLINE ps_v16u8 ps_rev128(ps_v16u8 v)
{
    return __builtin_shufflevector(v, v, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
}
PS_ALWAYS_INLINE void ps_ld128_rev(void* dst, const void* src)
{
    ps_v16u8 v;
    memcpy(&v, src, 16);
    v = ps_rev128(v);
    memcpy(dst, &v, 16);
}
PS_ALWAYS_INLINE void ps_st128_rev(void* dst, const void* src)
{
    ps_v16u8 v;
    memcpy(&v, src, 16);
    v = ps_rev128(v);
    memcpy(dst, &v, 16);
}

/* ---- guest address space: base is the host address of guest address 0 ---- */

/* Byte-aligned views so unaligned guest addresses are well-defined; ARM64 executes them as single loads/stores. */
typedef uint16_t ps_u16_ua __attribute__((aligned(1), may_alias));
typedef uint32_t ps_u32_ua __attribute__((aligned(1), may_alias));
typedef uint64_t ps_u64_ua __attribute__((aligned(1), may_alias));

PS_ALWAYS_INLINE uint8_t* ps_g2h(uint8_t* base, uint32_t addr) { return base + addr; }

PS_ALWAYS_INLINE uint8_t ps_read8(uint8_t* base, uint32_t addr) { return *(volatile const uint8_t*)(base + addr); }
PS_ALWAYS_INLINE uint16_t ps_read16(uint8_t* base, uint32_t addr)
{
    return ps_bswap16(*(volatile const ps_u16_ua*)(base + addr));
}
PS_ALWAYS_INLINE uint32_t ps_read32(uint8_t* base, uint32_t addr)
{
    return ps_bswap32(*(volatile const ps_u32_ua*)(base + addr));
}
PS_ALWAYS_INLINE uint64_t ps_read64(uint8_t* base, uint32_t addr)
{
    return ps_bswap64(*(volatile const ps_u64_ua*)(base + addr));
}
PS_ALWAYS_INLINE float ps_readf32(uint8_t* base, uint32_t addr)
{
    uint32_t u = ps_read32(base, addr);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
PS_ALWAYS_INLINE double ps_readf64(uint8_t* base, uint32_t addr)
{
    uint64_t u = ps_read64(base, addr);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

PS_ALWAYS_INLINE void ps_write8(uint8_t* base, uint32_t addr, uint8_t v) { *(volatile uint8_t*)(base + addr) = v; }
PS_ALWAYS_INLINE void ps_write16(uint8_t* base, uint32_t addr, uint16_t v)
{
    *(volatile ps_u16_ua*)(base + addr) = ps_bswap16(v);
}
PS_ALWAYS_INLINE void ps_write32(uint8_t* base, uint32_t addr, uint32_t v)
{
    *(volatile ps_u32_ua*)(base + addr) = ps_bswap32(v);
}
PS_ALWAYS_INLINE void ps_write64(uint8_t* base, uint32_t addr, uint64_t v)
{
    *(volatile ps_u64_ua*)(base + addr) = ps_bswap64(v);
}
PS_ALWAYS_INLINE void ps_writef32(uint8_t* base, uint32_t addr, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    ps_write32(base, addr, u);
}
PS_ALWAYS_INLINE void ps_writef64(uint8_t* base, uint32_t addr, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    ps_write64(base, addr, u);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_CORE_BE_H */
