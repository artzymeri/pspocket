/*
 * PPU register context, shared by the runtime (C++20), the interpreter and lifted code (C11).
 *
 * Representation choices (keep lifted code and record forms cheap):
 *
 * - GPRs are 64-bit (the Cell PPE always runs in 64-bit mode, MSR[SF] = 1). CR0 of record forms compares the
 *   full 64-bit result as signed, branches decrement and test the full 64-bit CTR.
 * - FPRs are IEEE doubles. Loads, stores and moves keep the exact bit pattern (lfd/stfd/fmr never convert).
 * - VRs are 128-bit, 16-byte aligned, in the project's full-reverse lane order (docs/decisions.md D-004):
 *   host byte j holds guest byte 15 - j.
 * - CR is held unpacked, one byte per CR bit (value 0 or 1): cr[4 * field + PS_CR_LT/GT/EQ/SO]. A record form
 *   is four independent byte stores, a branch tests one byte, and lifted code keeps each bit in its own local, so
 *   clang turns "cmp + bc" into a native compare-and-branch and drops CR bits that are overwritten before use.
 *   mfcr/mtcrf pack and unpack (rare).
 * - XER is split into SO, OV, CA (bytes, 0 or 1) and the 7-bit string byte count, so carry chains are a local
 *   byte in lifted code.
 * - FPSCR is the architected 32-bit register (IBM bit 0 = bit 31 here). What is modelled is documented in
 *   pspocket/ppu/fpu.h.
 * - The reservation (lwarx/ldarx, stwcx./stdcx.) is the single-thread model of this milestone; see
 *   ps_ppu_reserve_* in pspocket/ppu/runtime.h for the Phase 3 hook (shared 128-byte line table).
 */
#ifndef PSPOCKET_PPU_CONTEXT_H
#define PSPOCKET_PPU_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define PS_PPU_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
extern "C" {
#else
#define PS_PPU_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* One 128-bit vector register (D-004 lane order: guest element i of an N-element vector is host lane N-1-i). */
typedef union __attribute__((aligned(16))) ps_v128 {
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    float f32[4];
} ps_v128;

/* Offsets of the four bits inside a CR field: cr[4 * field + PS_CR_xx]. */
enum { PS_CR_LT = 0, PS_CR_GT = 1, PS_CR_EQ = 2, PS_CR_SO = 3 };

typedef struct PPUContext {
    uint64_t gpr[32];
    double fpr[32];
    ps_v128 vr[32];

    uint8_t cr[32]; /* one byte per CR bit, 0 or 1; index = IBM CR bit number */

    uint64_t lr;
    uint64_t ctr;

    /* XER, split */
    uint8_t xer_so;
    uint8_t xer_ov;
    uint8_t xer_ca;
    uint8_t xer_bc; /* string byte count, XER[57:63] (0..127) */

    uint32_t fpscr; /* architected layout; see fpu.h for the model */
    uint32_t vrsave;

    /* VSCR, split (VMX is not executed in this milestone, the state is carried) */
    uint8_t vscr_sat;
    uint8_t vscr_nj;
    uint8_t pad0[2];

    uint32_t cia; /* current instruction address: kept by the interpreter, set by lifted code at runtime calls */

    /*
     * Reservation, single-thread model (lwarx/ldarx set it, stwcx./stdcx. consume it).
     * Phase 3 replaces the check with the shared 128-byte line table (plan section 6); these fields then hold this
     * thread's reservation address and the line version or data it observed.
     */
    uint32_t reserve_addr;
    uint8_t reserve_valid;
    uint8_t reserve_size; /* 4 (lwarx) or 8 (ldarx) */
    uint8_t pad1[6];
    uint64_t reserve_value;

    void* runtime; /* owned by the thread runtime (lv2 thread object, Phase 2); never touched by lifted code */
} PPUContext;

/* Layout is part of the ABI between the runtime and generated code. */
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, gpr) == 0x000, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, fpr) == 0x100, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, vr) == 0x200, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, cr) == 0x400, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, lr) == 0x420, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, ctr) == 0x428, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, xer_so) == 0x430, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, xer_bc) == 0x433, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, fpscr) == 0x434, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, vrsave) == 0x438, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, vscr_sat) == 0x43C, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, cia) == 0x440, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, reserve_addr) == 0x444, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, reserve_valid) == 0x448, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, reserve_value) == 0x450, "PPUContext layout");
PS_PPU_STATIC_ASSERT(offsetof(PPUContext, runtime) == 0x458, "PPUContext layout");
PS_PPU_STATIC_ASSERT(sizeof(PPUContext) == 0x460, "PPUContext layout");
PS_PPU_STATIC_ASSERT(sizeof(ps_v128) == 16, "ps_v128 is 128 bits");
PS_PPU_STATIC_ASSERT(sizeof(double) == 8, "FPRs are IEEE doubles");

/* Reset state: everything zero except VRSAVE = 0xFFFFFFFF and VSCR[NJ] = 1 (as RPCS3, Emu/Cell/PPUThread.h). */
static inline void ps_ppu_context_init(PPUContext* ctx)
{
    unsigned char* p = (unsigned char*)ctx;
    for (size_t i = 0; i < sizeof(PPUContext); i++) p[i] = 0;
    ctx->vrsave = 0xFFFFFFFFu;
    ctx->vscr_nj = 1;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_PPU_CONTEXT_H */
