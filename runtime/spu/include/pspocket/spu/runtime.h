/*
 * SPU runtime C API: lifted-program registry and dispatch, the run loop, the runtime calls the interpreter and
 * lifted code share (channels, MFC, stop-and-signal, halt, interrupts), PPU-side accessors for the lv2 layer
 * (mailboxes, signal notification, run control) and the hooks lv2 installs.
 *
 * Threading: an SPUContext belongs to one host thread (its SPU thread). Other threads only use the PPU-side
 * accessors below, which take the context's lock (ctx->host). Contexts without a host are single-threaded (tests):
 * a channel access that would block stops execution with PS_SPU_STOP_BLOCKED instead.
 */
#ifndef PSPOCKET_SPU_RUNTIME_H
#define PSPOCKET_SPU_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "pspocket/spu/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Route B (D-152): code built into a game library reaches the functions marked PS_SPU_IMPORT through pointers the
 * host fills in when it loads the library (pspocket/game/library.h): each name is then a hidden variable of the
 * library holding the host's function. */
#if defined(PSPOCKET_GAME_LIBRARY)
#define PS_SPU_IMPORT(ret, name, params) extern ret(*name) params __attribute__((visibility("hidden")))
#else
#define PS_SPU_IMPORT(ret, name, params) ret name params
#endif

/* Every lifted function: void spu_<hash>_<start>_<entry>(SPUContext* ctx, uint8_t* ls). It may be entered at any of its
 * block starts (it switches on ctx->pc), runs until control leaves it, and returns with ctx->pc = the next guest
 * instruction (and ctx->stop_reason set if execution must stop). */
typedef void (*ps_spu_fn)(SPUContext* ctx, uint8_t* ls);

/* ---------------------------------------------------------------------------------------------------------------
 * Lifted programs. Generated code (recomp/spu, spu-lift) defines one ps_spu_program per SPU program: every block
 * start of every lifted function is an entry, so the run loop can resume lifted code anywhere a block begins
 * (return addresses after calls included).
 * ------------------------------------------------------------------------------------------------------------- */

typedef struct ps_spu_entry {
    uint32_t addr;
    ps_spu_fn fn;
} ps_spu_entry;

typedef struct ps_spu_program {
    const char* name;
    uint64_t hash;       /* ps_spu_hash64 of LS [code_start, code_start + code_size) */
    uint32_t code_start; /* LS range the program was lifted from */
    uint32_t code_size;
    uint8_t xfloat;      /* single-precision model the code was lifted for (PS_SPU_XFLOAT_*) */
    /* Position independent (D-081): the code computes every address it derives from its own from ctx->reloc, so it
     * runs at any LS address; found by hash and size only. Job binaries and the modules programs load are. */
    uint8_t relocatable;
    uint32_t entry_count;
    const ps_spu_entry* entries; /* ascending addr */
    ps_spu_fn* table;    /* dense lookup over the code range, built by ps_spu_register_program */
    /* The words the program lifted as instructions: bit i of word i / 32 for the word at code_start + 4 * i (NULL: all
     * of them). A DMA over the range invalidates the program only when it writes one of these (D-085): a binary's own
     * data and bss share its range, and jobs copy their buffers there. */
    const uint32_t* code_words;
    /* ps_spu_code_hash of the image it was lifted from: the bytes of its code words only (0: not known). An image whose
     * whole hash is new is this program when its code words hold the same bytes: a module with data that changes with
     * every load (D-208). */
    uint64_t code_hash;
} ps_spu_program;

/* Program key: 64-bit hash of the code bytes (multiply-xorshift over 8-byte words, then the tail). The same function
 * runs in the toolchain (spu-lift) and at runtime. */
uint64_t ps_spu_hash64(const uint8_t* data, size_t size);

/* The hash of an image's code words only (ps_spu_hash64 over their bytes, in order): code_words as in ps_spu_program,
 * NULL for every word of the image. */
uint64_t ps_spu_code_hash(const uint8_t* image, uint32_t code_size, const uint32_t* code_words);

/* Registration happens at startup, before SPU threads run. The program must stay alive. */
PS_SPU_IMPORT(void, ps_spu_register_program, (ps_spu_program* program));
void ps_spu_unregister_program(ps_spu_program* program);
/* The registered program with this key: hash, code start and size; a relocatable program matches any code start. */
const ps_spu_program* ps_spu_find_program(uint64_t hash, uint32_t code_start, uint32_t code_size);
/* A registered relocatable program of this size whose code words hold the same bytes as `image` (D-208): for an image
 * ps_spu_find_program does not know. NULL: none. */
const ps_spu_program* ps_spu_find_program_by_code(const uint8_t* image, uint32_t code_size);
/* Changes whenever a program is registered or unregistered: what a caller remembered from ps_spu_find_program holds
 * while it stays the same (D-109). */
uint64_t ps_spu_registry_generation(void);

/* Selects the lifted program for the LS image now in ctx->ls: hashes [code_start, code_start + size) and attaches
 * the registered program with that key. Returns it, or NULL (logs the hash: CLAUDE.md rule 5; the interpreter
 * runs the program). */
const ps_spu_program* ps_spu_identify(SPUContext* ctx, uint32_t code_start, uint32_t code_size);
/* Attaches a program directly (also sets ctx->xfloat to its model) as the context's home and current program. NULL
 * detaches. */
void ps_spu_attach_program(SPUContext* ctx, const ps_spu_program* program);
/* Attaches a program whose code sits at LS address code_start: a relocatable one anywhere, another only where it
 * was lifted. NULL detaches. */
void ps_spu_attach_program_at(SPUContext* ctx, const ps_spu_program* program, uint32_t code_start);

/* Code a program loads itself (D-079). The host may name another lifted program for an address the current one has
 * no code at: a module the program copied into local store. The run loop asks on a miss, the interpreter on a taken
 * branch out of the current program's code; the resolver returns the program lifted from the code at pc (it may still
 * lack an entry there: a miss to report) and where that code sits (*code_start: a relocatable program runs there), or
 * NULL. The run loop makes it current; when execution comes back to the home program's code, home is current again.
 * Called on the SPU's thread. The observer sees every GET into local store as it completes (lsa, effective address,
 * size), on the SPU's thread. Set both before SPU threads run; NULL clears. */
typedef const ps_spu_program* (*ps_spu_resolver)(SPUContext* ctx, uint32_t pc, uint32_t* code_start);
void ps_spu_set_resolver(ps_spu_resolver resolver);
typedef void (*ps_spu_get_observer)(SPUContext* ctx, uint32_t lsa, uint32_t eal, uint32_t size);
void ps_spu_set_get_observer(ps_spu_get_observer observer);

static inline ps_spu_fn ps_spu_lookup(const SPUContext* ctx, uint32_t pc)
{
    const ps_spu_program* p = ctx->program;
    if (!p) return NULL;
    const uint32_t off = (pc - ctx->reloc - p->code_start) & 0x3FFFFu;
    return off < p->code_size ? p->table[off >> 2] : NULL;
}

/*
 * What happens when a program is attached and execution reaches an address with no lifted code (CLAUDE.md rule 5):
 * dev builds log the address and abort, release builds log once per address and interpret until lifted code can
 * be resumed. Default: ABORT without NDEBUG, INTERPRET with it. Contexts without a program always interpret.
 */
typedef enum ps_spu_miss_policy { PS_SPU_MISS_ABORT = 0, PS_SPU_MISS_INTERPRET = 1 } ps_spu_miss_policy;
void ps_spu_set_miss_policy(ps_spu_miss_policy policy);
ps_spu_miss_policy ps_spu_get_miss_policy(void);
uint64_t ps_spu_miss_count(void);
/* Each address a miss is logged for is also appended to the file at `path` (NULL: none), in the log's own words, so
 * scripts/lift-spu.sh can take the file as a log: the iOS app keeps one for the Mac to translate (D-117). */
void ps_spu_set_miss_log(const char* path);

/* Where the run loop interprets: instructions interpreted, by the program current when the interpreter took over (NULL:
 * none) and the address it took over at, for the n largest (the table holds the first 4096 keys). Diagnostics. */
typedef struct ps_spu_interp_site {
    const ps_spu_program* program;
    uint32_t pc;
    uint64_t steps;
    uint64_t entries;
} ps_spu_interp_site;
uint32_t ps_spu_interp_sites(ps_spu_interp_site* out, uint32_t n);
/* Bytes SPU DMAs read (put = 0) or wrote (put = 1) in MB `mb` of RSX local memory so far. Diagnostics. */
uint64_t ps_spu_video_dma(uint32_t mb, int put);

/* The last DMA writes to guest memory (PUT and its forms, list elements, SDCRZ, PUTLLC, PUTLLUC) made on each host
 * thread, in a ring per thread (D-138): when a structure turns out overwritten, ps_spu_dma_writes_over tells which SPU
 * work wrote there. `tag` names the work the calling thread runs from now on (a SPURS worker: the job's descriptor and
 * binary); 0: none. Diagnostics. */
typedef struct ps_spu_dma_write {
    uint32_t ea;
    uint32_t size;
    uint64_t tag;     /* the writing thread's tag then */
    uint64_t time_ns; /* steady clock */
    uint32_t pc;      /* the SPU's pc at the command */
    uint32_t cmd;     /* the MFC command */
} ps_spu_dma_write;
void ps_spu_set_dma_tag(uint64_t tag);
/* The recorded writes that overlap [ea, ea + size), newest first, at most n; returns how many. */
uint32_t ps_spu_dma_writes_over(uint32_t ea, uint32_t size, ps_spu_dma_write* out, uint32_t n);

/* An observer of the calling host thread's MFC traffic, for diagnostics (a job's reads and writes, D-987): called
 * after each transfer (a list's elements one by one, SDCRZ, sndsig) and each atomic command (GETLLAR, PUTLLC with
 * size 1 when it succeeded and 0 when it failed, PUTLLUC) the thread's SPU makes, with the SPU's pc and the command's
 * LS and guest addresses. NULL: none (the default; one relaxed load a transfer). */
typedef void (*ps_spu_mfc_observer)(void* user, const SPUContext* ctx, uint32_t pc, uint32_t cmd, uint32_t lsa,
    uint32_t ea, uint32_t size);
void ps_spu_set_mfc_observer(ps_spu_mfc_observer fn, void* user);
/* A filter of the calling host thread's transfers (D-988): called before each transfer (a list's elements one by one;
 * not the atomic commands); a non-zero result leaves it undone, as if it had been made (EDGE geometry on the GPU drops
 * the job's PUTs of what the GPU makes instead). NULL: none (the default). */
typedef int (*ps_spu_mfc_filter)(void* user, SPUContext* ctx, uint32_t pc, uint32_t cmd, uint32_t lsa, uint32_t ea,
    uint32_t size);
void ps_spu_set_mfc_filter(ps_spu_mfc_filter fn, void* user);

/* ---------------------------------------------------------------------------------------------------------------
 * Run loop.
 * ------------------------------------------------------------------------------------------------------------- */

/* Runs from ctx->pc until execution stops; returns ctx->stop_reason (never PS_SPU_STOP_NONE). Lifted code runs
 * where the attached program has it, the interpreter everywhere else. Enters the SPU FP environment (fp.h) for the
 * duration. Stop-and-signal codes go to the stop handler first; execution continues when it says so. */
uint32_t ps_spu_run(SPUContext* ctx);

/* Interpreter only, at most max_steps instructions (0 = no limit); PS_SPU_STOP_LIMIT when the limit is hit. */
uint32_t ps_spu_interpret(SPUContext* ctx, uint64_t max_steps);

/* The context running on this host thread (inside ps_spu_run or ps_spu_interpret), or NULL: for fault reports. */
SPUContext* ps_spu_current(void);

/* ---------------------------------------------------------------------------------------------------------------
 * Hooks for the lv2 layer (process-wide; set before SPU threads run). They run with the host's default FP
 * environment.
 * ------------------------------------------------------------------------------------------------------------- */

/* stop / stopd. Return non-zero to continue at the next instruction (syscall handled: sys_spu_thread_receive_event
 * and friends), zero to stop the SPU (exit codes, unknown codes). NULL: every stop stops the SPU. */
typedef int (*ps_spu_stop_handler)(SPUContext* ctx, uint32_t code);
void ps_spu_set_stop_handler(ps_spu_stop_handler handler);

/* SPU_WrOutIntrMbox on SPU threads carries lv2 calls (sys_spu_thread_send_event, throw_event, event_flag_set_bit;
 * RPCS3 SPUThread.cpp set_ch_value). Return 0 when handled, non-zero to stop the SPU with the write pending (it
 * re-executes when resumed). NULL: plain one-entry interrupt mailbox (raw SPU behaviour). */
typedef int (*ps_spu_intr_mbox_handler)(SPUContext* ctx, uint32_t value);
void ps_spu_set_intr_mbox_handler(ps_spu_intr_mbox_handler handler);

/* Per-context handlers (SPUContext::hooks): when a context has them, its stop codes and interrupt-mailbox writes go
 * to these instead of the process-wide handlers above, with the same return conventions. A NULL member falls back to
 * the process-wide handler. */
typedef struct ps_spu_hooks {
    ps_spu_stop_handler stop;
    ps_spu_intr_mbox_handler intr_mbox;
    /* Raw SPUs (D-528): non-zero makes interrupt-mailbox writes the plain one-entry mailbox even with a process-wide
     * handler, and intr_mbox_written (when set) is called after each, without the context's lock. */
    int plain_intr_mbox;
    void (*intr_mbox_written)(SPUContext* ctx);
} ps_spu_hooks;

/* Decrementer and event timing. NULL restores ps_timebase_now (pspocket/core/timebase.h); tests install a clock. */
typedef uint64_t (*ps_spu_timebase_source)(void);
void ps_spu_set_timebase_source(ps_spu_timebase_source source);
uint64_t ps_spu_timebase(void);

/* ---------------------------------------------------------------------------------------------------------------
 * Runtime calls of the interpreter and lifted code. A non-zero return means "stop": ctx->stop_reason, stop_code,
 * exception and pc are set, and the caller returns to the run loop (lifted code spills its registers first).
 * ------------------------------------------------------------------------------------------------------------- */

PS_SPU_IMPORT(int, ps_spu_rdch, (SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t* value));
PS_SPU_IMPORT(int, ps_spu_wrch, (SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t value));
PS_SPU_IMPORT(int, ps_spu_rchcnt, (SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t* count));
/* stop (signal type 0..0x3FFF) / stopd (0x3FFF) at pc. */
PS_SPU_IMPORT(int, ps_spu_stop, (SPUContext* ctx, uint32_t pc, uint32_t code));
/* heq/hgt/hlgt whose condition holds (pc = the halt instruction). Always stops. */
PS_SPU_IMPORT(int, ps_spu_halt, (SPUContext* ctx, uint32_t pc));
/* Illegal or unsupported instruction at pc. Always stops (PS_SPU_EXC_ILLEGAL). */
PS_SPU_IMPORT(int, ps_spu_illegal, (SPUContext* ctx, uint32_t pc, uint32_t insn));
/* bisled: non-zero when an enabled event is pending (the SPU_RdEventStat count). */
PS_SPU_IMPORT(int, ps_spu_events_pending, (SPUContext* ctx));
/* E/D bits of an indirect branch, applied after the branch updated ctx->pc. Non-zero when an interrupt must be
 * taken now (ctx->pc and srr0 are updated): the caller returns to the run loop. */
PS_SPU_IMPORT(int, ps_spu_set_interrupts, (SPUContext* ctx, int enable));

/* fscrwr: the architected FPSCR bits (RPCS3 SPU_FPSCR::Write). Host word i = guest word 3 - i. */
static inline void ps_spu_fscrwr(SPUContext* ctx, const ps_spu_v128* value)
{
    ctx->fpscr.u32[3] = value->u32[3] & 0x00000F07u;
    ctx->fpscr.u32[2] = value->u32[2] & 0x00003F07u;
    ctx->fpscr.u32[1] = value->u32[1] & 0x00003F07u;
    ctx->fpscr.u32[0] = value->u32[0] & 0x00000F07u;
}

/* Attention bits (ctx->attention): lifted code returns to the run loop when any is set. */
enum { PS_SPU_ATTN_STOP = 1u, PS_SPU_ATTN_INTERRUPT = 2u };

/* ---------------------------------------------------------------------------------------------------------------
 * PPU-side accessors (thread-safe; for lv2's sys_spu_thread_* and raw SPU problem-state emulation).
 * ------------------------------------------------------------------------------------------------------------- */

/* Allocates / frees the host part of a context (lock, condition variable, run state). */
void ps_spu_host_attach(SPUContext* ctx);
void ps_spu_host_detach(SPUContext* ctx);

/* Inbound mailbox (4 entries; a write to a full mailbox overwrites the newest entry, as RPCS3). */
void ps_spu_write_in_mbox(SPUContext* ctx, uint32_t value);
uint32_t ps_spu_in_mbox_count(SPUContext* ctx);
/* Outbound and outbound interrupt mailboxes. Non-blocking: 1 and *value on success, 0 when empty. */
int ps_spu_read_out_mbox(SPUContext* ctx, uint32_t* value);
int ps_spu_read_out_intr_mbox(SPUContext* ctx, uint32_t* value);
/* Blocking read of the outbound mailbox: waits up to timeout_ns (0 = forever). */
int ps_spu_wait_out_mbox(SPUContext* ctx, uint32_t* value, uint64_t timeout_ns);

/* Signal notification register 1 or 2 (which = 1 or 2). OR mode accumulates instead of overwriting. */
void ps_spu_write_signal(SPUContext* ctx, uint32_t which, uint32_t value);
void ps_spu_set_signal_or_mode(SPUContext* ctx, uint32_t which, int or_mode);

/* Asks the SPU to stop: blocked channel accesses return, running lifted code returns at its next loop back edge
 * or runtime call, and the run loop exits with PS_SPU_STOP_REQUESTED (pc = next instruction to execute). */
void ps_spu_request_stop(SPUContext* ctx);
/* Clears a previous stop request (before running the context again). */
void ps_spu_clear_stop(SPUContext* ctx);

/* A command the PPU queued through a raw SPU's MFC proxy registers (D-528; RPCS3 spu_thread::read_reg's
 * MFC_CMDStatus): the GET, PUT and SDCRZ families run at once between the local store and guest memory (an EA in the
 * raw SPU or SPU thread areas is not copied: -1). 0 when done, -1 for a command or address the proxy does not take. */
int ps_spu_proxy_dma(SPUContext* ctx, uint32_t cmd, uint32_t lsa, uint32_t eal, uint32_t size);

/* SPU status register: PS_SPU_STATUS_* bits, stop-and-signal code in bits 16-31 after a stop. */
uint32_t ps_spu_status(SPUContext* ctx);

/* ---------------------------------------------------------------------------------------------------------------
 * Thread groups: DMA to PS_SPU_THREAD_BASE + index * PS_SPU_THREAD_OFFSET + offset reaches member `index`'s LS
 * (offset < 256 KB) or its signal notification registers (SNR1/SNR2 offsets, 4-byte PUT / sndsig).
 * ------------------------------------------------------------------------------------------------------------- */
typedef struct ps_spu_group {
    SPUContext* members[8];
    uint32_t count;
} ps_spu_group;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_SPU_RUNTIME_H */
