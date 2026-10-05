/*
 * The entry of a game library (route B, D-152; pspocket/game/library.h): compiled into every game library with
 * PSPOCKET_GAME_LIBRARY, never into the host. game_library_info.h is written by whoever makes the library
 * (pspocket-prepare on the Mac, the app on the phone): what the code was made from, and which parts it has.
 */
#include "pspocket/game/library.h"

#include "game_library_info.h"

/* ---- the runtime the translated code calls (runtime.h's PS_PPU_IMPORT and PS_SPU_IMPORT names, fp.h's tables) ---- */

ps_ppu_dispatch_table* ps_ppu_dispatch_ref;
void (*ps_ppu_dispatch_miss_stub)(PPUContext* ctx, uint8_t* base);
void (*ps_ppu_dispatch_register_table)(const ps_ppu_dispatch_entry* entries, size_t count);
void (*ps_ppu_dispatch_unregister)(uint32_t addr);
void (*ps_ppu_jump_miss)(PPUContext* ctx, uint8_t* base, uint32_t target, uint64_t stop_lr, uint64_t stop_sp);
void (*ps_ppu_jump_table_miss)(PPUContext* ctx, uint8_t* base, uint32_t pc, uint32_t target, uint64_t stop_lr,
    uint64_t stop_sp);
void (*ps_ppu_interp_one)(PPUContext* ctx, uint8_t* base, uint32_t pc);
void (*ps_ppu_interp_until)(PPUContext* ctx, uint8_t* base, uint32_t pc, uint64_t stop_lr, uint64_t stop_sp);
ps_ppu_illegal_fn ps_ppu_illegal;
void (*ps_ppu_syscall)(PPUContext* ctx, uint8_t* base);
void (*ps_ppu_trap)(PPUContext* ctx, uint8_t* base, uint32_t pc);
uint64_t (*ps_ppu_timebase)(void);
uint32_t (*ps_ppu_lwarx)(PPUContext* ctx, uint8_t* base, uint32_t ea);
uint64_t (*ps_ppu_ldarx)(PPUContext* ctx, uint8_t* base, uint32_t ea);
int (*ps_ppu_stwcx)(PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t value);
int (*ps_ppu_stdcx)(PPUContext* ctx, uint8_t* base, uint32_t ea, uint64_t value);
void (*ps_ppu_lswx)(PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t rt);
void (*ps_ppu_stswx)(PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t rs);

void (*ps_spu_register_program)(ps_spu_program* program);
int (*ps_spu_rdch)(SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t* value);
int (*ps_spu_wrch)(SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t value);
int (*ps_spu_rchcnt)(SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t* count);
int (*ps_spu_stop)(SPUContext* ctx, uint32_t pc, uint32_t code);
int (*ps_spu_halt)(SPUContext* ctx, uint32_t pc);
int (*ps_spu_illegal)(SPUContext* ctx, uint32_t pc, uint32_t insn);
int (*ps_spu_events_pending)(SPUContext* ctx);
int (*ps_spu_set_interrupts)(SPUContext* ctx, int enable);
const uint32_t* ps_spu_frest_fraction_lut;
const uint32_t* ps_spu_frest_exponent_lut;
const uint32_t* ps_spu_frsqest_fraction_lut;
const uint32_t* ps_spu_frsqest_exponent_lut;

/* ---- what the library holds ---- */

#if PS_GAME_HAS_PPU
/* ppu-lift's --name (game_ppu unless game_library_info.h says otherwise): N_register, N_image_words... */
#ifndef PS_GAME_PPU_NAME
#define PS_GAME_PPU_NAME game_ppu
#endif
#define PS_GAME_CAT2(a, b) a##b
#define PS_GAME_CAT(a, b) PS_GAME_CAT2(a, b)
#define PS_GAME_PPU(x) PS_GAME_CAT(PS_GAME_PPU_NAME, x)
void PS_GAME_PPU(_register)(void);
void PS_GAME_PPU(_unregister)(void);
extern const uint32_t PS_GAME_PPU(_function_count);
extern const uint32_t PS_GAME_PPU(_image_words)[];
extern const uint32_t PS_GAME_PPU(_image_word_count);
#endif
#if PS_GAME_HAS_SPU
void pspocket_game_spu_register(void); /* scripts/lift-spu.sh's game_spu.c */
#endif

static void bind(const ps_game_runtime* rt)
{
    ps_ppu_dispatch_ref = rt->ppu_dispatch;
    ps_ppu_dispatch_miss_stub = rt->ppu_dispatch_miss_stub;
    ps_ppu_dispatch_register_table = rt->ppu_dispatch_register_table;
    ps_ppu_dispatch_unregister = rt->ppu_dispatch_unregister;
    ps_ppu_jump_miss = rt->ppu_jump_miss;
    ps_ppu_jump_table_miss = rt->ppu_jump_table_miss;
    ps_ppu_interp_one = rt->ppu_interp_one;
    ps_ppu_interp_until = rt->ppu_interp_until;
    ps_ppu_illegal = rt->ppu_illegal;
    ps_ppu_syscall = rt->ppu_syscall;
    ps_ppu_trap = rt->ppu_trap;
    ps_ppu_timebase = rt->ppu_timebase;
    ps_ppu_lwarx = rt->ppu_lwarx;
    ps_ppu_ldarx = rt->ppu_ldarx;
    ps_ppu_stwcx = rt->ppu_stwcx;
    ps_ppu_stdcx = rt->ppu_stdcx;
    ps_ppu_lswx = rt->ppu_lswx;
    ps_ppu_stswx = rt->ppu_stswx;
    ps_spu_register_program = rt->spu_register_program;
    ps_spu_rdch = rt->spu_rdch;
    ps_spu_wrch = rt->spu_wrch;
    ps_spu_rchcnt = rt->spu_rchcnt;
    ps_spu_stop = rt->spu_stop;
    ps_spu_halt = rt->spu_halt;
    ps_spu_illegal = rt->spu_illegal;
    ps_spu_events_pending = rt->spu_events_pending;
    ps_spu_set_interrupts = rt->spu_set_interrupts;
    ps_spu_frest_fraction_lut = rt->spu_frest_fraction_lut;
    ps_spu_frest_exponent_lut = rt->spu_frest_exponent_lut;
    ps_spu_frsqest_fraction_lut = rt->spu_frsqest_fraction_lut;
    ps_spu_frsqest_exponent_lut = rt->spu_frsqest_exponent_lut;
}

/* The descriptor's register functions are left out (NULL) until the runtime matches: nothing in the library may run
 * against a runtime it was not compiled for. */
static ps_game_library library = {
    .layout = PS_GAME_LIBRARY_LAYOUT,
    .size = sizeof(ps_game_library),
    .abi_hash = PS_GAME_ABI_HASH,
    .translator_hash = PS_GAME_TRANSLATOR_HASH,
    .title_id = PS_GAME_TITLE_ID,
    .made_by = PS_GAME_MADE_BY,
    .executable_sha256 = PS_GAME_EXECUTABLE_SHA256,
    .spu_program_count = PS_GAME_SPU_PROGRAM_COUNT,
};

__attribute__((visibility("default"))) const ps_game_library* pspocket_game_library(const ps_game_runtime* rt)
{
    if (rt && rt->layout == PS_GAME_LIBRARY_LAYOUT && rt->size == sizeof(ps_game_runtime) &&
        rt->abi_hash == PS_GAME_ABI_HASH) {
        bind(rt);
#if PS_GAME_HAS_PPU
        library.ppu_function_count = PS_GAME_PPU(_function_count);
        library.ppu_image_words = PS_GAME_PPU(_image_words);
        library.ppu_image_word_count = PS_GAME_PPU(_image_word_count);
        library.ppu_register = PS_GAME_PPU(_register);
        library.ppu_unregister = PS_GAME_PPU(_unregister);
#endif
#if PS_GAME_HAS_SPU
        library.spu_register = pspocket_game_spu_register;
#endif
    }
    return &library;
}
