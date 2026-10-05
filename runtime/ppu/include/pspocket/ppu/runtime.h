/*
 * PPU runtime C API: the dispatch table (guest address -> lifted function), interpreter fallbacks, and the hooks
 * the interpreter and lifted code share (syscalls, traps, timebase, reservations, string ops).
 * Hosts and tests configure it from C or C++; generated code reaches it through pspocket/ppu/lifted.h.
 */
#ifndef PSPOCKET_PPU_RUNTIME_H
#define PSPOCKET_PPU_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "pspocket/ppu/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Route B (D-152): code built into a game library reaches the functions marked PS_PPU_IMPORT, and the dispatch table,
 * through pointers the host fills in when it loads the library (pspocket/game/library.h). Each name is then a hidden
 * variable of the library holding the host's function, so the generated code is the same either way.
 */
#if defined(PSPOCKET_GAME_LIBRARY)
#define PS_PPU_IMPORT(ret, name, params) extern ret(*name) params __attribute__((visibility("hidden")))
#else
#define PS_PPU_IMPORT(ret, name, params) ret name params
#endif

/* Every lifted function: void f_XXXXXXXX(PPUContext* restrict ctx, uint8_t* base) (D-009, amended by D-013). */
typedef void (*ps_ppu_fn)(PPUContext* ctx, uint8_t* base);

/* ---------------------------------------------------------------------------------------------------------------
 * Dispatch table: one slot per 4-byte instruction address over the registered code range, in host memory the
 * guest cannot write. Empty slots, and every address outside the range, hold ps_ppu_dispatch_miss_stub, so an
 * indirect call is one bounds check and one load. Registration happens at startup, before guest threads run
 * (it may grow and move the table); lookups are plain reads afterwards. Addresses are 4-byte aligned.
 * ------------------------------------------------------------------------------------------------------------- */

typedef struct ps_ppu_dispatch_entry {
    uint32_t addr;
    ps_ppu_fn fn;
} ps_ppu_dispatch_entry;

typedef struct ps_ppu_dispatch_table {
    ps_ppu_fn* slots; /* slots[i] serves guest address base + 4 * i */
    uint32_t base;
    uint32_t count;
} ps_ppu_dispatch_table;

#if defined(PSPOCKET_GAME_LIBRARY)
extern ps_ppu_dispatch_table* ps_ppu_dispatch_ref __attribute__((visibility("hidden")));
#define ps_ppu_dispatch (*ps_ppu_dispatch_ref)
#else
extern ps_ppu_dispatch_table ps_ppu_dispatch;
#endif

/*
 * The function in every empty slot. It reads the target from ctx->cia (ps_ppu_call_indirect stores it there) and
 * applies the miss policy below.
 */
PS_PPU_IMPORT(void, ps_ppu_dispatch_miss_stub, (PPUContext* ctx, uint8_t* base));

static inline ps_ppu_fn ps_ppu_dispatch_slot(uint32_t addr)
{
    const uint32_t index = (addr - ps_ppu_dispatch.base) >> 2;
    return index < ps_ppu_dispatch.count ? ps_ppu_dispatch.slots[index] : ps_ppu_dispatch_miss_stub;
}

/* The registered function at addr, or NULL. */
ps_ppu_fn ps_ppu_dispatch_find(uint32_t addr);

void ps_ppu_dispatch_register(uint32_t addr, ps_ppu_fn fn);
PS_PPU_IMPORT(void, ps_ppu_dispatch_register_table, (const ps_ppu_dispatch_entry* entries, size_t count));
PS_PPU_IMPORT(void, ps_ppu_dispatch_unregister, (uint32_t addr));
void ps_ppu_dispatch_clear(void);
size_t ps_ppu_dispatch_count(void);

/*
 * What happens when execution reaches a guest address that has no lifted function. CLAUDE.md rule 5: dev builds
 * log the address and abort; release builds log and run the interpreter from that address until the call returns.
 * Default: ABORT without NDEBUG, INTERPRET with it.
 */
typedef enum ps_ppu_miss_policy { PS_PPU_MISS_ABORT = 0, PS_PPU_MISS_INTERPRET = 1 } ps_ppu_miss_policy;
void ps_ppu_set_miss_policy(ps_ppu_miss_policy policy);
ps_ppu_miss_policy ps_ppu_get_miss_policy(void);
uint64_t ps_ppu_dispatch_miss_count(void);

/* Indirect call (bctrl, blrl, bl outside the image). ctx->lr already holds the return address. */
static inline void ps_ppu_call_indirect(PPUContext* ctx, uint8_t* base, uint32_t target)
{
    ctx->cia = target;
    ps_ppu_dispatch_slot(target)(ctx, base);
}

/*
 * Indirect jump that is not a call (bctr without a recognised jump table). A registered target is entered as a
 * tail call. Otherwise the miss policy applies; the interpreter then runs until the enclosing lifted function would
 * have returned: cia == stop_lr and r1 == stop_sp (that function's entry LR and SP).
 */
PS_PPU_IMPORT(void, ps_ppu_jump_miss, (PPUContext* ctx, uint8_t* base, uint32_t target, uint64_t stop_lr,
    uint64_t stop_sp));
static inline void ps_ppu_jump_indirect(PPUContext* ctx, uint8_t* base, uint32_t target, uint64_t stop_lr,
    uint64_t stop_sp)
{
    const ps_ppu_fn fn = ps_ppu_dispatch_slot(target);
    if (fn != ps_ppu_dispatch_miss_stub) {
        ctx->cia = target;
        fn(ctx, base);
    } else {
        ps_ppu_jump_miss(ctx, base, target, stop_lr, stop_sp);
    }
}

/*
 * A recognised jump table (bctr) saw a target that is not in the table: the analysis was wrong or the table was
 * patched. Logs pc and target and aborts in dev builds (never __builtin_unreachable); release builds continue
 * like ps_ppu_jump_indirect.
 */
PS_PPU_IMPORT(void, ps_ppu_jump_table_miss, (PPUContext* ctx, uint8_t* base, uint32_t pc, uint32_t target,
    uint64_t stop_lr, uint64_t stop_sp));

/* ---------------------------------------------------------------------------------------------------------------
 * Interpreter fallbacks for lifted code (context fully spilled before the call, reloaded after).
 * ------------------------------------------------------------------------------------------------------------- */

/* Executes the single (non-branch) instruction at pc. */
PS_PPU_IMPORT(void, ps_ppu_interp_one, (PPUContext* ctx, uint8_t* base, uint32_t pc));
/* Runs from pc until cia == stop_lr and r1 == stop_sp (calls registered lifted functions on the way). */
PS_PPU_IMPORT(void, ps_ppu_interp_until, (PPUContext* ctx, uint8_t* base, uint32_t pc, uint64_t stop_lr,
    uint64_t stop_sp));
/* Illegal/unknown instruction reached: logs pc and the word, then aborts. */
#if defined(PSPOCKET_GAME_LIBRARY)
typedef void (*ps_ppu_illegal_fn)(PPUContext* ctx, uint8_t* base, uint32_t pc) __attribute__((noreturn));
extern ps_ppu_illegal_fn ps_ppu_illegal __attribute__((visibility("hidden")));
#else
__attribute__((noreturn)) void ps_ppu_illegal(PPUContext* ctx, uint8_t* base, uint32_t pc);
#endif

/* ---------------------------------------------------------------------------------------------------------------
 * System hooks (process-wide; set before guest code runs).
 * ------------------------------------------------------------------------------------------------------------- */

/* sc: r11 holds the syscall number, ctx->cia the address of the sc. Execution continues at cia + 4. */
typedef void (*ps_ppu_syscall_handler)(PPUContext* ctx, uint8_t* base);
/* tw/td/twi/tdi when the condition holds. Execution continues at pc + 4 when the handler returns. */
typedef void (*ps_ppu_trap_handler)(PPUContext* ctx, uint8_t* base, uint32_t pc);

/* NULL restores the defaults, which log (syscall number / trap address) and abort. */
void ps_ppu_set_syscall_handler(ps_ppu_syscall_handler handler);
void ps_ppu_set_trap_handler(ps_ppu_trap_handler handler);
PS_PPU_IMPORT(void, ps_ppu_syscall, (PPUContext* ctx, uint8_t* base));
PS_PPU_IMPORT(void, ps_ppu_trap, (PPUContext* ctx, uint8_t* base, uint32_t pc));

/* mftb / mfspr TBL,TBU. NULL restores ps_timebase_now (pspocket/core/timebase.h); tests install a fixed clock. */
typedef uint64_t (*ps_ppu_timebase_source)(void);
void ps_ppu_set_timebase_source(ps_ppu_timebase_source source);
PS_PPU_IMPORT(uint64_t, ps_ppu_timebase, (void));

/*
 * Reservations: lwarx/ldarx/stwcx./stdcx., on the shared 128-byte-line reservation system that SPU GETLLAR/PUTLLC
 * and HLE sync-object updates use too (pspocket/core/reservation.h, D-030). lwarx/ldarx record the address, size and
 * loaded value with a snapshot of the line and its version; stwcx./stdcx. succeed, atomically, when a reservation of
 * the same size exists for the same aligned address and the line is unchanged since the snapshot (no conditional
 * store or notified write in between, its bytes equal: RPCS3 also compares data, PPUThread.cpp
 * ppu_store_reservation), and always clear the reservation.
 */
PS_PPU_IMPORT(uint32_t, ps_ppu_lwarx, (PPUContext* ctx, uint8_t* base, uint32_t ea));
PS_PPU_IMPORT(uint64_t, ps_ppu_ldarx, (PPUContext* ctx, uint8_t* base, uint32_t ea));
PS_PPU_IMPORT(int, ps_ppu_stwcx, (PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t value));
PS_PPU_IMPORT(int, ps_ppu_stdcx, (PPUContext* ctx, uint8_t* base, uint32_t ea, uint64_t value));

/* lswx / stswx (byte count in ctx->xer_bc), on the spilled ctx->gpr. Semantics: RPCS3 PPUInterpreter.cpp LSWX. */
PS_PPU_IMPORT(void, ps_ppu_lswx, (PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t rt));
PS_PPU_IMPORT(void, ps_ppu_stswx, (PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t rs));

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_PPU_RUNTIME_H */
