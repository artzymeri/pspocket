/*
 * Case table of the differential fuzz test. Written (with the lifted step functions) by ppu-fuzz-gen at build
 * time, read by test_ppu_fuzz.
 *
 * Every case: one instruction encoding at a guest pc, the lifted step function for it, and overrides applied on
 * top of a random context so that effective addresses land in the test memory window and edge cases occur.
 */
#ifndef PSPOCKET_TESTS_PPU_FUZZ_CASE_H
#define PSPOCKET_TESTS_PPU_FUZZ_CASE_H

#include <stdint.h>

#include "pspocket/ppu/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Guest memory used by the cases: a data window at 0 and a code area for the instruction itself. */
#define PS_FUZZ_WINDOW_BASE 0x00000000u
#define PS_FUZZ_WINDOW_SIZE 0x00000800u
#define PS_FUZZ_CODE_BASE 0x00100000u
#define PS_FUZZ_CODE_SIZE 0x00040000u

enum ps_fuzz_override_kind {
    PS_FZ_GPR = 1,         /* gpr[index] = value */
    PS_FZ_FPR,             /* fpr[index] bits = value */
    PS_FZ_CTR,             /* ctr = value */
    PS_FZ_XER_BC,          /* xer_bc = value */
    PS_FZ_RES_MATCH,       /* reservation valid for (value = address, index = size), memory value recorded */
    PS_FZ_RES_STALE_VALUE, /* as MATCH, but the recorded value differs from memory */
    PS_FZ_RES_OTHER_ADDR,  /* as MATCH, for another address */
    PS_FZ_RES_NONE,        /* no reservation */
    PS_FZ_XER_CA,          /* xer_ca = value */
};

typedef struct ps_fuzz_override {
    uint8_t kind;
    uint8_t index;
    uint64_t value;
} ps_fuzz_override;

typedef void (*ps_fuzz_fn)(PPUContext* ctx, uint8_t* base);

typedef struct ps_fuzz_case {
    uint16_t op;       /* pspocket::ppu::Op */
    uint8_t variant;   /* Rc/OE/LK/AA bits (pspocket::ppu::form_variant) */
    uint8_t override_count;
    uint32_t insn;
    uint32_t pc;
    uint32_t first_override; /* index into ps_fuzz_overrides */
    ps_fuzz_fn fn;
} ps_fuzz_case;

extern const ps_fuzz_case ps_fuzz_cases[];
extern const uint32_t ps_fuzz_case_count;
extern const ps_fuzz_override ps_fuzz_overrides[];
extern const uint32_t ps_fuzz_cases_per_form;

#ifdef __cplusplus
}
#endif

#endif
