/*
 * A game library (route B, D-152): one game's translated PPU and SPU code as a dynamic library the host loads at run
 * time, instead of code built into the host. The phone makes it from the game's executable once per game (stage 4);
 * the Mac makes it with the same compiler (runtime/codegen, pspocket-prepare).
 *
 * The library exports one symbol, pspocket_game_library (PS_GAME_LIBRARY_ENTRY). The host calls it with the runtime
 * the code calls (ps_game_runtime: the functions and tables runtime.h marks PS_PPU_IMPORT and PS_SPU_IMPORT); it
 * keeps that table and returns what it holds (ps_game_library): the code's register functions and what it was made
 * from. Inside the library those runtime names are hidden pointer variables (PSPOCKET_GAME_LIBRARY), so the lifters'
 * output is the same as for a built-in translation, and the host need not export anything (iOS strips apps).
 *
 * Compatibility: the library was compiled against the runtime's ABI headers (the ten headers translated code
 * includes, and this one). Their hash (ps_game_abi_hash, made by the build) goes into both; the host loads only a
 * library with its own hash and layout version, and another one is made again. The translator's hash (the lifters'
 * sources) says whether a newer pspocket would translate the game differently.
 */
#ifndef PSPOCKET_GAME_LIBRARY_H
#define PSPOCKET_GAME_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

#include "pspocket/ppu/runtime.h"
#include "pspocket/spu/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The layout of the two structures below; bumped when a field is added, moved or retyped. */
#define PS_GAME_LIBRARY_LAYOUT 1u
#define PS_GAME_LIBRARY_ENTRY "pspocket_game_library"

typedef struct ps_game_runtime {
    uint32_t layout; /* PS_GAME_LIBRARY_LAYOUT */
    uint32_t size;   /* sizeof(ps_game_runtime) */
    uint64_t abi_hash;

    /* PPU (pspocket/ppu/runtime.h) */
    ps_ppu_dispatch_table* ppu_dispatch;
    void (*ppu_dispatch_miss_stub)(PPUContext* ctx, uint8_t* base);
    void (*ppu_dispatch_register_table)(const ps_ppu_dispatch_entry* entries, size_t count);
    void (*ppu_dispatch_unregister)(uint32_t addr);
    void (*ppu_jump_miss)(PPUContext* ctx, uint8_t* base, uint32_t target, uint64_t stop_lr, uint64_t stop_sp);
    void (*ppu_jump_table_miss)(PPUContext* ctx, uint8_t* base, uint32_t pc, uint32_t target, uint64_t stop_lr,
        uint64_t stop_sp);
    void (*ppu_interp_one)(PPUContext* ctx, uint8_t* base, uint32_t pc);
    void (*ppu_interp_until)(PPUContext* ctx, uint8_t* base, uint32_t pc, uint64_t stop_lr, uint64_t stop_sp);
    void (*ppu_illegal)(PPUContext* ctx, uint8_t* base, uint32_t pc) __attribute__((noreturn));
    void (*ppu_syscall)(PPUContext* ctx, uint8_t* base);
    void (*ppu_trap)(PPUContext* ctx, uint8_t* base, uint32_t pc);
    uint64_t (*ppu_timebase)(void);
    uint32_t (*ppu_lwarx)(PPUContext* ctx, uint8_t* base, uint32_t ea);
    uint64_t (*ppu_ldarx)(PPUContext* ctx, uint8_t* base, uint32_t ea);
    int (*ppu_stwcx)(PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t value);
    int (*ppu_stdcx)(PPUContext* ctx, uint8_t* base, uint32_t ea, uint64_t value);
    void (*ppu_lswx)(PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t rt);
    void (*ppu_stswx)(PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t rs);

    /* SPU (pspocket/spu/runtime.h, fp.h) */
    void (*spu_register_program)(ps_spu_program* program);
    int (*spu_rdch)(SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t* value);
    int (*spu_wrch)(SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t value);
    int (*spu_rchcnt)(SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t* count);
    int (*spu_stop)(SPUContext* ctx, uint32_t pc, uint32_t code);
    int (*spu_halt)(SPUContext* ctx, uint32_t pc);
    int (*spu_illegal)(SPUContext* ctx, uint32_t pc, uint32_t insn);
    int (*spu_events_pending)(SPUContext* ctx);
    int (*spu_set_interrupts)(SPUContext* ctx, int enable);
    const uint32_t* spu_frest_fraction_lut;   /* [32] */
    const uint32_t* spu_frest_exponent_lut;   /* [256] */
    const uint32_t* spu_frsqest_fraction_lut; /* [64] */
    const uint32_t* spu_frsqest_exponent_lut; /* [256] */
} ps_game_runtime;

typedef struct ps_game_library {
    uint32_t layout; /* PS_GAME_LIBRARY_LAYOUT */
    uint32_t size;   /* sizeof(ps_game_library) */
    uint64_t abi_hash;        /* the headers it was compiled against */
    uint64_t translator_hash; /* the lifters that translated it */
    const char* title_id;     /* "BLES01807" */
    const char* made_by;      /* the pspocket that made it: version and git hash */
    uint8_t executable_sha256[32]; /* the decrypted executable it was translated from */

    /* PPU: ppu-lift's N_register / N_unregister, and its (address, word) samples of the image (D-098). */
    uint32_t ppu_function_count;
    const uint32_t* ppu_image_words;
    uint32_t ppu_image_word_count;
    void (*ppu_register)(void);
    void (*ppu_unregister)(void);

    /* SPU: scripts/lift-spu.sh's register function for every lifted program. */
    uint32_t spu_program_count;
    void (*spu_register)(void);
} ps_game_library;

/* The entry point. Returns NULL when the runtime is not the one it was compiled for (layout, size or ABI hash). */
typedef const ps_game_library* (*ps_game_library_entry)(const ps_game_runtime* runtime);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_GAME_LIBRARY_H */
