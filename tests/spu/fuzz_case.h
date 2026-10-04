/*
 * Case table of the SPU differential fuzz test. Written (with the lifted step functions) by spu-fuzz-gen at build
 * time, read by test_spu_fuzz.
 *
 * Every case: one instruction encoding at an LS pc, the floating-point model it was lifted for, the lifted step
 * function, and overrides applied on top of a random context (the same before the interpreter run and the lifted
 * run) so that DMA addresses land in the guest window, reservations exist, shift counts hit their edges, and so on.
 */
#ifndef PSPOCKET_TESTS_SPU_FUZZ_CASE_H
#define PSPOCKET_TESTS_SPU_FUZZ_CASE_H

#include <stdint.h>

#include "pspocket/spu/context.h"
#include "pspocket/spu/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Instructions sit in [CODE_BASE, CODE_BASE + CODE_SIZE) of the LS; DMA effective addresses in the guest window. */
#define PS_SPU_FUZZ_CODE_BASE 0x20000u
#define PS_SPU_FUZZ_CODE_SIZE 0x10000u
#define PS_SPU_FUZZ_GUEST_BASE 0x00100000u
#define PS_SPU_FUZZ_GUEST_SIZE 0x00010000u

enum ps_spu_fuzz_override_kind {
    PS_SPU_FZ_PREF = 1, /* gpr[index] guest word 0 = (uint32_t)value */
    PS_SPU_FZ_REG,      /* gpr[index] host u64 lanes = value, value2 */
    PS_SPU_FZ_MFC,      /* MFC parameters: lsa = value[31:0], eal = value[63:32], size = value2[15:0], tag = value2[20:16] */
    PS_SPU_FZ_RES,      /* reservation: index 0 none, 1 valid, 2 stale bytes, 3 stale version; value = line address */
    PS_SPU_FZ_LIST,     /* writes a valid DMA list at LS address value (value2: elements in bits 0-7, seed above) */
};

typedef struct ps_spu_fuzz_override {
    uint8_t kind;
    uint8_t index;
    uint8_t pad[6];
    uint64_t value;
    uint64_t value2;
} ps_spu_fuzz_override;

typedef void (*ps_spu_fuzz_fn)(SPUContext* ctx, uint8_t* ls);

typedef struct ps_spu_fuzz_case {
    uint16_t op;    /* pspocket::spu::Op */
    uint8_t xfloat; /* PS_SPU_XFLOAT_* the step function was lifted for */
    uint8_t override_count;
    uint32_t insn;
    uint32_t pc;             /* where it was lifted */
    uint32_t reloc;          /* relocatable (D-081): runs at pc + reloc with ctx->reloc = reloc; else 0 */
    uint32_t first_override; /* index into ps_spu_fuzz_overrides */
    ps_spu_fuzz_fn fn;
} ps_spu_fuzz_case;

extern const ps_spu_fuzz_case ps_spu_fuzz_cases[];
extern const uint32_t ps_spu_fuzz_case_count;
extern const ps_spu_fuzz_override ps_spu_fuzz_overrides[];
extern const uint32_t ps_spu_fuzz_cases_per_op;

/* The program-level case (D-081): a synthetic program (big-endian bytes, lifted at PROGRAM_BASE) lifted whole,
 * relocatable (ps_spu_fzp) and fixed (ps_spu_fzq); its absolute call goes to a stub (bi $lr) at PROGRAM_STUB. */
#define PS_SPU_FUZZ_PROGRAM_BASE 0x1000u
#define PS_SPU_FUZZ_PROGRAM_STUB 0x100u
extern const uint8_t ps_spu_fuzz_program[];
extern const uint32_t ps_spu_fuzz_program_size;
extern const uint32_t ps_spu_fuzz_program_entry;
extern ps_spu_program ps_spu_fzp_program;
extern ps_spu_program ps_spu_fzq_program;
void ps_spu_fzp_register(void);
void ps_spu_fzq_register(void);

#ifdef __cplusplus
}
#endif

#endif
